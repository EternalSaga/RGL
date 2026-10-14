module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）
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
import render_target_cache;  // 矩图托管：RtAttachmentSpec / RenderTargetCache::ensure
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

/** 矩图的规格申报（RT 缓存托管的唯一形态来源；本模块不再自己建图 / 领槽 / 持 view）。 */
RtAttachmentSpec momentsSpec(uint32_t mipLevels, uint32_t momentsSampleSlot) {
	RtAttachmentSpec spec{};
	spec.aspect = vk::ImageAspectFlagBits::eColor;
	spec.mipLevels = mipLevels;
	// 无 color/transfer flag：矩图只被 compute 写（STORAGE）+ light pass 采（SAMPLED）
	spec.usage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
	spec.dedicatedMemory = true;  // 采样 image（RT 缓存先例同款）
	spec.allocationName = "evssm_shadow/mipmaps";  // vma 日志归属名（迁入前后同名，账可比）
	spec.descriptors.reserve(mipLevels + 1);
	// 每 mip 一个 STORAGE_IMAGE（降采样链逐级读写），描述符 layout 一律 GENERAL
	for (uint32_t i = 0; i < mipLevels; ++i)
		spec.descriptors.push_back(RtDescriptorSpec{
			.range = {vk::ImageAspectFlagBits::eColor, i, 1, 0, 1},
			.type = vk::DescriptorType::eStorageImage,
			.layout = vk::ImageLayout::eGeneral,
			.heapSlot = std::nullopt,  // 由缓存领号（模块侧不再自领 Runtime 段槽）
		});
	// 全 mip SAMPLED（light pass binding 15）：🔴 **预发号**——槽号装配期就要烘进管线，
	// 而图首帧才懒构造，两段式不可合并（见 Resources::ensureEvssmSampleSlot）
	spec.descriptors.push_back(RtDescriptorSpec{
		.range = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1},
		.type = vk::DescriptorType::eSampledImage,
		.layout = vk::ImageLayout::eGeneral,
		.heapSlot = momentsSampleSlot,
	});
	return spec;
}

}  // namespace

EvssmShadow::EvssmShadow(RGL::DeviceContext& vkDevice, RGL::Resources& vkResources,
	RGL::RenderTargetCache& rtCache, uint32_t mapSize_, uint32_t cols_, uint32_t mipLevels_,
	float exponent_, uint32_t shadowMapSlot_, uint32_t momentsSampleSlot_, OffGraphSync& sync_)
	: sync(sync_), deviceCtx(&vkDevice), resources(&vkResources), mapSize(mapSize_), cols(cols_),
	  mipLevels(mipLevels_), exponent(exponent_), shadowMapSlot(shadowMapSlot_),
	  sampleSlot(momentsSampleSlot_) {
	// 批次 B1b：级间 barrier 每帧恰发 `mipLevels − 1` 次（③ 那个循环的圈数）。**构造期**申报
	// 这个事实，帧末对账才独立——录制期申报等于自己出题自己答，那条判据就自证了。
	sync.expectCount(RgSyncEdgeId::evssmMomentsChain, mipLevels - 1u);
	auto& dev = vkDevice.getDevice();
	const uint32_t descSize = static_cast<uint32_t>(resources->getDescriptorHeap().getDescriptorSize());

	// ── 矩图：向 RT 缓存按规格 ensure（幂等；本模块零私有资源）──
	auto moments = rtCache.ensure(std::string{kEvssmMomentsName},
		vk::Extent2D{mapSize, mapSize}, vk::Format::eR16G16B16A16Sfloat,
		momentsSpec(mipLevels, momentsSampleSlot_));
	if (!moments)
		throw std::runtime_error(fmt::format("EvssmShadow: 矩图 ensure 失败: {}",
			moments.error().describe()));
	RtAttachment& rt = **moments;
	// 到手描述符必须与规格逐项同号同类（下面按**规格顺序**取 storage 槽，序即契约：
	// 前 mipLevels 项 = 每 mip 一个 storage，末项 = 全 mip sampled）
	if (rt.descriptors.size() != mipLevels + 1u)
		throw std::runtime_error(fmt::format(
			"EvssmShadow: 矩图描述符数 {} ≠ 规格 {}（缓存没照规格发号）",
			rt.descriptors.size(), mipLevels + 1u));
	const auto mipSlot = [&](uint32_t level) {
		const auto& d = rt.descriptors[level];
		if (d.type != vk::DescriptorType::eStorageImage)
			throw std::runtime_error(fmt::format(
				"EvssmShadow: 矩图第 {} 个描述符不是 STORAGE_IMAGE", level));
		return d.heapSlot;
	};

	// 初始 UNDEFINED → GENERAL（一次性 CB；此后帧内零 layout 转移）。图本体归缓存，本模块
	// 只在构造期对它发这一次：矩图 extent = kShadowMapSize 与窗口解耦、模块一旦构造就不再
	// 重建（generation 恒 1）。若将来它变成会重建的附件，这里要跟着 generation 重发。
	runOnce(dev, vkDevice.getGraphicsCommandPool(), vkDevice.getGraphicsQueue(), [&](vk::raii::CommandBuffer& cmd) {
		vk::ImageMemoryBarrier2 b{};
		b.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
		b.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
		b.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;
		b.oldLayout = vk::ImageLayout::eUndefined;
		b.newLayout = vk::ImageLayout::eGeneral;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = rt.image.image;
		b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, mipLevels, 0, 1};
		vk::DependencyInfo dep{};
		dep.setImageMemoryBarriers(b);
		cmd.pipelineBarrier2(dep);
	});

	// ── compute shader objects（binding→槽一对一，常量偏移映射）──
	// 键 = shader 里的 binding **变量名**（号由构建期接口表查得）：以前这里手抄 `{0, …}`，
	// shader 把 `[[vk::binding(0)]]` 改成别的号时 C++ 一声不响地往错堆槽读数据。
	momentsShader = makeMappedCompute(dev, "generated_shaders/evssm_moments.comp.spv",
		std::vector<std::pair<std::string_view, RGL::HeapSlot>>{
			{"shadowDepth", HeapSlot{shadowMapSlot}}, {"moments", HeapSlot{mipSlot(0)}}}, descSize,
		&resources->computeAccessLog());
	downShaders.reserve(mipLevels - 1);
	for (uint32_t level = 1; level < mipLevels; ++level)
		// 逐级各建一个 shader object：同一份 SPV，mipSrc/mipDst 烘不同槽（级间换视图）
		downShaders.push_back(makeMappedCompute(dev, "generated_shaders/evssm_downsample.comp.spv",
			std::vector<std::pair<std::string_view, RGL::HeapSlot>>{
				{"mipSrc", HeapSlot{mipSlot(level - 1)}},
				{"mipDst", HeapSlot{mipSlot(level)}}}, descSize,
			&resources->computeAccessLog()));

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
