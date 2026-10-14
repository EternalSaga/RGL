module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）
#include <vk_mem_alloc.h>   // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 的 C 面）
#include <fmt/format.h>
module evssm_shadow:impl;

import evssm_shadow;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import offgraph_sync;  // 批次 B1b：RgSyncEdgeId / emit（import 不传递，接口里的不算）
import compute_push_types_generated;  // EvssmMomentsPush / EvssmDownPush（反射产物，布局有出处）
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import VmaRaii;
import shader;  // loadShaderSpv（evssm_moments/evssm_downsample.comp.slang）

namespace RGL {

namespace {

// 两个 push 布局（EvssmMomentsPush / EvssmDownPush）**不在本文件**——由 compute-push 反射管线
// 生成（generated/compute_push_types_generated.cppm，import 见文件头）。
//
// 🔴 这里原来是一对 `struct MomentsPush { … }` + `static_assert(sizeof/offsetof …)`。删掉它们的
// 理由不是"清理冗余"，而是**那几条 assert 是关于 C++ 侧的同义反复**：量的是手抄进 C++ 的 struct，
// 不是 shader。shader 把 `mapSize` 与 `cols` 换序而 C++ 没跟着动时三条 assert 全过，C++ 写
// mapSize@0 而 shader 读 mapSize@4 —— 静默。布局现在由反射派生，生成物自带 sizeof/offsetof assert
// （那份 assert 量的才是编译器实查的 shader 布局）；在这里再留一份手抄数值 = 造第二个真相源。
// "期望值 = 去 shader 逐字段手算"这条独立判据没有丢，它挪进了 tests/test_compute_push_layout.cppm
// ——红在测试里，而不是静默在渲染里。

/** 一次性 layout 转移 CB（vulkan_utils::submitCmdBufferAndWait 同款机制——raii 局部
 *  cmd/fence 函数尾销毁；提交 graphics 队列 + fence 等待——但本站失败直接抛，
 *  = 启动期 fail-fast 契约，故不复用那个返回 expected 的版本）。 */
void runOnce(vk::raii::Device& dev, const vk::raii::CommandPool& pool, VkQueue queue,
	const std::function<void(vk::raii::CommandBuffer&)>& body) {
	vk::CommandBufferAllocateInfo ai{*pool, vk::CommandBufferLevel::ePrimary, 1};
	auto buffers = dev.allocateCommandBuffers(ai);
	auto& cmd = buffers[0];
	cmd.begin({});
	body(cmd);
	cmd.end();
	vk::raii::Fence fence(dev, vk::FenceCreateInfo{});
	const VkCommandBuffer raw = *cmd;
	VkSubmitInfo si{};
	si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &raw;
	if (::vkQueueSubmit(queue, 1, &si, *fence) != VK_SUCCESS)
		throw std::runtime_error("EvssmShadow: 一次性提交失败");
	if (dev.waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX) != vk::Result::eSuccess)
		throw std::runtime_error("EvssmShadow: 一次性 fence 超时");
}

}  // namespace

EvssmShadow::EvssmShadow(RGL::DeviceContext& vkDevice, RGL::Resources& vkResources,
	uint32_t mapSize_, uint32_t cols_, uint32_t mipLevels_, float exponent_,
	uint32_t shadowMapSlot_, uint32_t momentsSampleSlot_, OffGraphSync& sync_)
	: sync(sync_), deviceCtx(&vkDevice), resources(&vkResources), mapSize(mapSize_), cols(cols_),
	  mipLevels(mipLevels_), exponent(exponent_), shadowMapSlot(shadowMapSlot_),
	  sampleSlot(momentsSampleSlot_) {
	// 批次 B1b：级间 barrier 每帧恰发 `mipLevels − 1` 次（③ 那个循环的圈数）。**构造期**申报
	// 这个事实，帧末对账才独立——录制期申报等于自己出题自己答，那条判据就自证了。
	sync.expectCount(RgSyncEdgeId::evssmMomentsChain, mipLevels - 1u);
	auto& dev = vkDevice.getDevice();
	const uint32_t descSize = static_cast<uint32_t>(resources->getDescriptorHeap().getDescriptorSize());

	// ── heap 槽：每 mip 一个 storage 视图槽（本模块自有）；sampled 全 mip 槽 =
	// Resources 预发号（binding 15 启动期已 bake 槽号，此处覆写真描述符）──
	mipSlots.reserve(mipLevels);
	for (uint32_t i = 0; i < mipLevels; ++i) {
		auto slot = resources->allocSlot(RGL::SlotSegment::Runtime, "evssm.moments.mip");
		if (!slot)
			throw std::runtime_error(fmt::format("EvssmShadow: mip {} 槽领取失败: {}", i,
				slot.error().describe()));
		mipSlots.push_back(*slot);
	}

	// ── 矩图：mapSize² RGBA16F，mipLevels 级，STORAGE|SAMPLED，全程 GENERAL ──
	vk::ImageCreateInfo ii{};
	ii.imageType = vk::ImageType::e2D;
	ii.format = vk::Format::eR16G16B16A16Sfloat;
	ii.extent = vk::Extent3D(mapSize, mapSize, 1);
	ii.mipLevels = mipLevels;
	ii.arrayLayers = 1;
	ii.samples = vk::SampleCountFlagBits::e1;
	ii.tiling = vk::ImageTiling::eOptimal;
	ii.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
	ii.sharingMode = vk::SharingMode::eExclusive;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_AUTO;
	ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;  // 采样 image（RT 缓存先例同款）
	auto img = resources->getAllocator().createImage(ii, ai, nullptr, "evssm_shadow/mipmaps");
	if (!img)
		throw std::runtime_error(
			fmt::format("EvssmShadow: 矩图创建失败: {}", img.error().describe()));
	momentsImage = std::move(*img);

	// 初始 UNDEFINED → GENERAL（一次性 CB；此后帧内零 layout 转移）
	runOnce(dev, vkDevice.getGraphicsCommandPool(), vkDevice.getGraphicsQueue(), [&](vk::raii::CommandBuffer& cmd) {
		vk::ImageMemoryBarrier2 b{};
		b.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
		b.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
		b.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;
		b.oldLayout = vk::ImageLayout::eUndefined;
		b.newLayout = vk::ImageLayout::eGeneral;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = *momentsImage;
		b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1};
		vk::DependencyInfo dep{};
		dep.setImageMemoryBarriers(b);
		cmd.pipelineBarrier2(dep);
	});

	// ── 描述符写入（v341 heap 语义：pView 指 view create info——常驻成员，随槽存活）──
	const auto writeSlot = [&](uint32_t slot, vk::ImageViewCreateInfo& viewCreate,
	                           vk::DescriptorType type) {
		vk::ImageDescriptorInfoEXT imgDesc{};
		imgDesc.pView = &viewCreate;
		imgDesc.layout = vk::ImageLayout::eGeneral;
		vk::ResourceDescriptorInfoEXT resDesc{};
		resDesc.type = type;
		resDesc.data.pImage = &imgDesc;
		const auto addr = resources->getDescriptorHeap().getSlotCpuAddress(slot);
		if (!addr)
			throw std::runtime_error(fmt::format("EvssmShadow: 槽 {} 地址解析失败", slot));
		dev.writeResourceDescriptorsEXT(resDesc,
			vk::HostAddressRangeEXT{*addr, static_cast<VkDeviceSize>(descSize)});
	};
	mipViewCreates.clear();
	for (uint32_t i = 0; i < mipLevels; ++i) {
		vk::ImageViewCreateInfo vc{};
		vc.image = *momentsImage;
		vc.viewType = vk::ImageViewType::e2D;
		vc.format = vk::Format::eR16G16B16A16Sfloat;
		vc.subresourceRange = {vk::ImageAspectFlagBits::eColor, i, 1, 0, 1};
		mipViewCreates.push_back(vc);
		writeSlot(mipSlots[i], mipViewCreates.back(), vk::DescriptorType::eStorageImage);
	}
	{
		vk::ImageViewCreateInfo vc{};
		vc.image = *momentsImage;
		vc.viewType = vk::ImageViewType::e2D;
		vc.format = vk::Format::eR16G16B16A16Sfloat;
		vc.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1};
		sampleViewCreate = vc;
		writeSlot(sampleSlot, *sampleViewCreate, vk::DescriptorType::eSampledImage);
	}

	// ── compute shader objects（binding→槽一对一，常量偏移映射）──
	momentsShader = makeMappedCompute(dev, "generated_shaders/evssm_moments.comp.spv",
		std::vector<std::pair<uint32_t, RGL::HeapSlot>>{
			{0, HeapSlot{shadowMapSlot}}, {1, HeapSlot{mipSlots[0]}}}, descSize);
	downShaders.reserve(mipLevels - 1);
	for (uint32_t level = 1; level < mipLevels; ++level)
		downShaders.push_back(makeMappedCompute(dev, "generated_shaders/evssm_downsample.comp.spv",
			std::vector<std::pair<uint32_t, RGL::HeapSlot>>{
				{0, HeapSlot{mipSlots[level - 1]}}, {1, HeapSlot{mipSlots[level]}}}, descSize));

	if (auto* logger = RLLogger::getInstance())
		logger->info("EvssmShadow: {}x{} RGBA16F x{} mip（GENERAL）| {} heap 槽 | 矩从 D32 派生（C 方案）",
			mapSize, mapSize, mipLevels, mipLevels + 1);
}

void EvssmShadow::recordFrame(vk::raii::CommandBuffer& cmd) {
	// ① 阴影图 depth-write → 本链 read（边 `evssm_depth_to_compute`：无句柄、纯执行依赖，
	//    覆盖 shadow pass 写 → compute 读；布局由 PCF 侧既有转移保证，本模块零漂移）。
	//    为什么是纯执行序边而不是 layout sandwich（IZB 那种）：矩图全程 GENERAL 零转移、
	//    阴影图归 RT 缓存本模块无句柄 ⟹「依赖要、布局免」。masks 出处 = usage 词表
	//    （`offgraph_sync`），本模块不写 stage/access 字面量。
	sync.emit(cmd, RgSyncEdgeId::evssmDepthToCompute);

	// ② 矩派生（全幅同 tile 布局恒等映射）
	{
		std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
		std::array<vk::ShaderEXT, 1> handles = {*momentsShader};
		cmd.bindShadersEXT(stages, handles);
		// 指定初始化器 = 让"shader 里改字段名/换序"在本调用点编译期红（理由同 mip_gen_impl，
		// 一句话版：位置聚合初始化与字段名无关；C++20 要求指定初始化按声明序 ⟹ 换序也红）
		const EvssmMomentsPush push{.mapSize = mapSize, .cols = cols, .exponent = exponent};
		cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
		cmd.dispatch(mapSize / 8u, mapSize / 8u, 1);
	}

	// ③ 逐级 box 降采样（dispatch z = tile；级间 compute→compute barrier）
	const uint32_t baseTile = mapSize / cols;
	for (uint32_t level = 1; level < mipLevels; ++level) {
		// 级间边（`evssm_moments_chain`）：本循环圈数 = 构造期申报的应发次数
		sync.emit(cmd, RgSyncEdgeId::evssmMomentsChain);
		std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
		std::array<vk::ShaderEXT, 1> handles = {downShaders[level - 1]};
		cmd.bindShadersEXT(stages, handles);
		// 指定初始化器 = 同上（字段名以生成物为准：baseTileTexels，本地短名 baseTile）
		const EvssmDownPush push{.level = level, .cols = cols, .baseTileTexels = baseTile};
		cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
		const uint32_t dstTile = baseTile >> level;
		cmd.dispatch((dstTile + 7u) / 8u, (dstTile + 7u) / 8u, cols * cols);
	}

	// ④ 收尾：compute write → light pass fragment 采样 read（边 `evssm_moments_to_light`）
	sync.emit(cmd, RgSyncEdgeId::evssmMomentsToLight);
}

}  // namespace RGL
