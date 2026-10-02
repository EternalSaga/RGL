module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）
#include <vk_mem_alloc.h>   // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 的 C 面）
#include <fmt/format.h>
module evssm_shadow:impl;

import evssm_shadow;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
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

/** MomentsPush —— 与 evssm_moments.comp.slang 同名 struct 逐字段一致（12B）。 */
struct MomentsPush {
	uint32_t mapSize;
	uint32_t cols;
	float exponent;
};

/** DownPush —— 与 evssm_downsample.comp.slang 同名 struct 逐字段一致（12B）。 */
struct DownPush {
	uint32_t level;
	uint32_t cols;
	uint32_t baseTileTexels;
};

/** 全局 stage/access barrier（MemoryBarrier2 载体、无 image/memory 条目）：只建执行
 *  依赖，不碰布局（v357 头 VkDependencyInfo 无 stage 字段，stage/access 挂在 barrier
 *  本体——IZB barrier 同款形态）。矩图全程 GENERAL 零转移；阴影图归 RT 缓存（本模块
 *  无句柄）——「依赖要、布局免」的正解就是这种纯 stage barrier（IZB 的 layout
 *  sandwich 是因为它要改场景深度布局）。 */
void stageBarrier(vk::raii::CommandBuffer& cmd, vk::PipelineStageFlags2 srcStage,
	vk::AccessFlags2 srcAccess, vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
	vk::MemoryBarrier2 mb{};
	mb.srcStageMask = srcStage;
	mb.srcAccessMask = srcAccess;
	mb.dstStageMask = dstStage;
	mb.dstAccessMask = dstAccess;
	vk::DependencyInfo dep{};
	dep.setMemoryBarriers(mb);
	cmd.pipelineBarrier2(dep);
}

/**
 * heap 映射的 compute shader object（izb_shadow_impl / gpu_cull_impl 同款模式）：
 * binding→heap 槽一对一；push constant 无需映射条目（heap 下 push data 区自动供给）。
 */
vk::raii::ShaderEXT makeMappedCompute(vk::raii::Device& dev, const char* spvPath,
	const std::vector<std::pair<uint32_t, uint32_t>>& bindingToSlot, uint32_t descriptorSize) {
	auto spvResult = loadShaderSpv(spvPath);
	if (!spvResult)
		throw std::runtime_error(spvResult.error().describe());
	const auto spv = std::move(*spvResult);

	std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
	mappings.reserve(bindingToSlot.size());
	for (const auto& [binding, slot] : bindingToSlot) {
		VkDescriptorSetAndBindingMappingEXT m{};
		m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
		m.descriptorSet = 0;
		m.firstBinding = binding;
		m.bindingCount = 1;
		m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
		m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
		m.sourceData.constantOffset.heapOffset = slot * descriptorSize;
		m.sourceData.constantOffset.heapArrayStride = descriptorSize;
		mappings.push_back(m);
	}
	VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
	mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
	mappingInfo.mappingCount = static_cast<uint32_t>(mappings.size());
	mappingInfo.pMappings = mappings.data();

	VkShaderCreateInfoEXT raw{};
	raw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	raw.pNext = &mappingInfo;
	raw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
	raw.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	raw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	raw.codeSize = spv.size() * sizeof(uint32_t);
	raw.pCode = spv.data();
	raw.pName = "main";
	auto shaders = dev.createShadersEXT(std::vector<vk::ShaderCreateInfoEXT>{vk::ShaderCreateInfoEXT(raw)});
	return std::move(shaders[0]);
}

/** 一次性 layout 转移 CB（texture_upload::submitUploadAndWait 同款：raii 局部
 *  cmd/fence 函数尾销毁；提交 graphics 队列 + fence 等待，失败抛 = 启动期 fail-fast）。 */
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
	uint32_t shadowMapSlot_, uint32_t momentsSampleSlot_)
	: deviceCtx(&vkDevice), resources(&vkResources), mapSize(mapSize_), cols(cols_),
	  mipLevels(mipLevels_), exponent(exponent_), shadowMapSlot(shadowMapSlot_),
	  sampleSlot(momentsSampleSlot_) {
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
	auto img = resources->getAllocator().createImage(ii, ai);
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
		{{0u, shadowMapSlot}, {1u, mipSlots[0]}}, descSize);
	downShaders.reserve(mipLevels - 1);
	for (uint32_t level = 1; level < mipLevels; ++level)
		downShaders.push_back(makeMappedCompute(dev, "generated_shaders/evssm_downsample.comp.spv",
			{{0u, mipSlots[level - 1]}, {1u, mipSlots[level]}}, descSize));

	if (auto* logger = RLLogger::getInstance())
		logger->info("EvssmShadow: {}x{} RGBA16F x{} mip（GENERAL）| {} heap 槽 | 矩从 D32 派生（C 方案）",
			mapSize, mapSize, mipLevels, mipLevels + 1);
}

void EvssmShadow::recordFrame(vk::raii::CommandBuffer& cmd) {
	// ① 阴影图 depth-write → 本链 read（全局 stage barrier：无句柄、纯执行依赖，
	//    覆盖 shadow pass 写 → compute 读；布局由 PCF 侧既有转移保证，本模块零漂移）
	stageBarrier(cmd, vk::PipelineStageFlagBits2::eAllGraphics,
		vk::AccessFlagBits2::eMemoryWrite,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);

	// ② 矩派生（全幅同 tile 布局恒等映射）
	{
		std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
		std::array<vk::ShaderEXT, 1> handles = {*momentsShader};
		cmd.bindShadersEXT(stages, handles);
		const MomentsPush push{mapSize, cols, exponent};
		cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
		cmd.dispatch(mapSize / 8u, mapSize / 8u, 1);
	}

	// ③ 逐级 box 降采样（dispatch z = tile；级间 compute→compute barrier）
	const uint32_t baseTile = mapSize / cols;
	for (uint32_t level = 1; level < mipLevels; ++level) {
		stageBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderWrite,
			vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite);
		std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
		std::array<vk::ShaderEXT, 1> handles = {downShaders[level - 1]};
		cmd.bindShadersEXT(stages, handles);
		const DownPush push{level, cols, baseTile};
		cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
		const uint32_t dstTile = baseTile >> level;
		cmd.dispatch((dstTile + 7u) / 8u, (dstTile + 7u) / 8u, cols * cols);
	}

	// ④ 收尾：compute write → light pass fragment 采样 read
	stageBarrier(cmd, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
}

}  // namespace RGL
