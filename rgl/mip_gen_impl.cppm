module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <fmt/format.h>
module mip_gen:impl;

import mip_gen;  // clang 不给 partition impl unit 隐式 import 主接口
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vulkan_utils;  // createImageBarrier（per-level 布局物化）
import shader;        // loadShaderSpv（mip_gen.comp.spv）

namespace RGL {

// 构造器 / recordChain 定义（契约与行为注释见 mip_gen.cppm 接口声明）。本单元是
// 叶子边（不 import vk_resources——Resources 持本模块类型，无环），编译不上关键路径。

namespace {

/** mip_gen.comp.spv 码流（加载失败 = 启动期错误，抛出——同 GpuCull SPV 加载路径）。 */
std::vector<uint32_t> loadMipGenSpv() {
	auto spv = loadShaderSpv("generated_shaders/mip_gen.comp.spv");
	if (!spv)
		throw std::runtime_error(fmt::format("mip_gen: {}", spv.error().describe()));
	return *spv;
}

/** shader module create info（SPV 码流拷贝进驱动）。 */
vk::ShaderModuleCreateInfo buildShaderModuleInfo(const std::vector<uint32_t>& spv) {
	vk::ShaderModuleCreateInfo info{};
	info.codeSize = spv.size() * sizeof(uint32_t);
	info.pCode = spv.data();
	return info;
}

/** 3 binding（0=sampled 1=storage 2=sampler，均 COMPUTE 阶段）——返回值存成员
 * setLayoutBindings（pBindings 目标须活过 create 调用，见接口成员注释）。 */
std::array<vk::DescriptorSetLayoutBinding, 3> buildSetLayoutBindings() {
	std::array<vk::DescriptorSetLayoutBinding, 3> bindings{};
	bindings[0] = {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr};
	bindings[1] = {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr};
	bindings[2] = {2, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eCompute, nullptr};
	return bindings;
}

/** set layout create info（pBindings 指向调用方长期存活的 bindings——helper 不得持局部数组）。 */
vk::DescriptorSetLayoutCreateInfo buildSetLayoutInfo(const std::array<vk::DescriptorSetLayoutBinding, 3>& bindings) {
	vk::DescriptorSetLayoutCreateInfo info{};
	info.bindingCount = static_cast<uint32_t>(bindings.size());
	info.pBindings = bindings.data();
	return info;
}

/** pipeline layout（set layout + COMPUTE push 16B = MipGenPush）。
 * hpp struct 的数组成员是 handle-class 指针（与 raii 同名冲突、rlvk 未导出）——
 * 走 C struct + hpp from-C 构造（项目既有模式，如 ensureSharedSampler）。
 * pSetLayouts/pPushConstantRanges 指向调用方成员（活过 create 调用）。 */
vk::PipelineLayoutCreateInfo buildPipelineLayoutInfo(const VkDescriptorSetLayout& layout,
	const VkPushConstantRange& range) {
	VkPipelineLayoutCreateInfo cInfo{};
	cInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	cInfo.setLayoutCount = 1;
	cInfo.pSetLayouts = &layout;
	cInfo.pushConstantRangeCount = 1;
	cInfo.pPushConstantRanges = &range;
	return vk::PipelineLayoutCreateInfo(cInfo);
}

/** compute 管线 create info（entry = main，传统管线非 shader object；C struct 同上）。 */
vk::ComputePipelineCreateInfo buildPipelineInfo(VkShaderModule module, VkPipelineLayout layout) {
	VkPipelineShaderStageCreateInfo stage{};
	stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stage.module = module;
	stage.pName = "main";
	VkComputePipelineCreateInfo cInfo{};
	cInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	cInfo.stage = stage;
	cInfo.layout = layout;
	return vk::ComputePipelineCreateInfo(cInfo);
}

/** 专用 sampler（LINEAR/CLAMP；mip-gen 自用，不碰渲染共享 sampler——避免污染模型纹理采样）。 */
vk::SamplerCreateInfo buildSamplerInfo() {
	vk::SamplerCreateInfo info{};
	info.magFilter = vk::Filter::eLinear;
	info.minFilter = vk::Filter::eLinear;
	info.mipmapMode = vk::SamplerMipmapMode::eLinear;
	info.addressModeU = vk::SamplerAddressMode::eClampToEdge;
	info.addressModeV = vk::SamplerAddressMode::eClampToEdge;
	info.addressModeW = vk::SamplerAddressMode::eClampToEdge;
	info.anisotropyEnable = false;
	info.compareEnable = false;
	info.minLod = 0.0f;
	info.maxLod = 32.0f;  // SampleLevel 显式 lod 钳到此值（覆盖任何现实链长）
	info.unnormalizedCoordinates = false;
	return info;
}

}  // namespace

/** 契约见接口注释。实现：初始化列表按成员声明序建全栈（raii 无默认构造，必须初始化列表）；
 * 体内校验 UNORM STORAGE_IMAGE 能力 + 建初始 pool + 日志。 */
MipGen::MipGen(RGL::DeviceContext& device)
	: device(device.getDevice()),
	  shaderModule(device.getDevice(), buildShaderModuleInfo(loadMipGenSpv())),
	  setLayoutBindings(buildSetLayoutBindings()),
	  setLayout(device.getDevice(), buildSetLayoutInfo(setLayoutBindings)),
	  setLayoutHandle(*setLayout),
	  pushRange{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(MipGenPush)},
	  pipelineLayout(device.getDevice(), buildPipelineLayoutInfo(setLayoutHandle, pushRange)),
	  pipeline(device.getDevice(), nullptr, buildPipelineInfo(*shaderModule, *pipelineLayout)),
	  mipSampler(device.getDevice(), buildSamplerInfo()) {
	// sRGB 纹理的 storage view 走 UNORM 重解释——UNORM 形式必须有 STORAGE_IMAGE 能力
	const auto props = device.getPhysicalDevice().getFormatProperties(vk::Format::eR8G8B8A8Unorm);
	if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage) == vk::FormatFeatureFlags{})
		throw std::runtime_error("mip_gen: R8G8B8A8_UNORM 无 STORAGE_IMAGE 能力（UNORM compute 链前提）");
	createPool();
	if (auto* logger = RLLogger::getInstance())
		logger->info("mip_gen: compute mip 链管线就绪（传统 descriptor set + 12B push）");
}

/** 契约见接口注释。实现：逐目标级 ① per-level 视图对（sampled=level−1 SRO /
 * storage=level）② 描述符集分配 + 3 write ③ UNDEFINED→GENERAL barrier → bind +
 * dispatch → GENERAL→SHADER_READ_ONLY barrier。视图/集存成员容器（活过调用方的
 * submit + fence wait）。仅 UNORM（sRGB 走 recordMipBlitChain，见模块头注释）。 */
std::expected<void, Error> MipGen::recordChain(vk::raii::CommandBuffer& cmd, VkImage image,
	vk::Format format, uint32_t w, uint32_t h, uint32_t mipLevels, const char* name) {
	if (mipLevels <= 1)
		return {};

	const auto emitBarrier = [&](const vk::ImageMemoryBarrier2& b) {
		VkDependencyInfo dep{};
		dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&b);
		::vkCmdPipelineBarrier2(*cmd, &dep);
	};

	uint32_t dstW = w;
	uint32_t dstH = h;
	for (uint32_t level = 1; level < mipLevels; ++level) {
		dstW = std::max(1u, dstW >> 1);
		dstH = std::max(1u, dstH >> 1);

		// per-level 视图对（视图范围 = 本次精确访问的子资源——VVL 逐 subresource
		// 布局检查天然干净；全链视图会在链生成中期看到 UNDEFINED/GENERAL 级误报）
		vk::ImageViewCreateInfo sInfo{};
		sInfo.image = image;
		sInfo.viewType = vk::ImageViewType::e2D;
		sInfo.format = format;  // UNORM src（本链仅 UNORM）
		sInfo.subresourceRange = {vk::ImageAspectFlagBits::eColor, level - 1, 1, 0, 1};
		sampledViews.emplace_back(device, sInfo);
		vk::ImageViewCreateInfo dInfo{};
		dInfo.image = image;
		dInfo.viewType = vk::ImageViewType::e2D;
		dInfo.format = format;  // UNORM storage（同格式；sRGB 走 blit 链）
		dInfo.subresourceRange = {vk::ImageAspectFlagBits::eColor, level, 1, 0, 1};
		storageViews.emplace_back(device, dInfo);

		// 描述符集（3 write：sampled/storage/sampler）。本 spec 版本
		// WriteDescriptorSet 已无 pSampler——sampler 一律走 DescriptorImageInfo.sampler
		// （SAMPLER 型 imageView=null，sType 默认 eWriteDescriptorSet）。
		const VkDescriptorSet set = allocateSet();
		vk::DescriptorImageInfo sImg{};
		sImg.sampler = *mipSampler;
		sImg.imageView = *sampledViews.back();
		sImg.imageLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		vk::DescriptorImageInfo dImg{};
		dImg.imageView = *storageViews.back();
		dImg.imageLayout = vk::ImageLayout::eGeneral;
		vk::DescriptorImageInfo lImg{};
		lImg.sampler = *mipSampler;
		std::array<vk::WriteDescriptorSet, 3> writes{};
		for (auto& wr : writes)
			wr.descriptorCount = 1;  // 默认 0——VVL VUID-00348 必须 >0
		writes[0].dstSet = set;
		writes[0].descriptorType = vk::DescriptorType::eSampledImage;
		writes[0].pImageInfo = &sImg;
		writes[1].dstSet = set;
		writes[1].dstBinding = 1;
		writes[1].descriptorType = vk::DescriptorType::eStorageImage;
		writes[1].pImageInfo = &dImg;
		writes[2].dstSet = set;
		writes[2].dstBinding = 2;
		writes[2].descriptorType = vk::DescriptorType::eSampler;
		writes[2].pImageInfo = &lImg;
		device.updateDescriptorSets(vk::ArrayProxy<vk::WriteDescriptorSet const>(3, writes.data()), nullptr);

		// level i：UNDEFINED→GENERAL（compute 写目标）
		emitBarrier(RGL::createImageBarrier(image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eShaderWrite,
			level, 1, 0, 1));
		cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
		// raii bindDescriptorSets 收 ArrayProxy<handle>（handle-class 转换不便）——直接 C 调用
		//（本 GMF 已含 vulkan.h，precedent = 上方 vkCmdPipelineBarrier2）
		vkCmdBindDescriptorSets(*cmd, VK_PIPELINE_BIND_POINT_COMPUTE, *pipelineLayout, 0, 1, &set, 0, nullptr);
		const MipGenPush push{level, dstW, dstH};
		cmd.pushConstants(*pipelineLayout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(MipGenPush), &push);
		cmd.dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
		// level i：GENERAL→SHADER_READ_ONLY（下级的采样源 / 渲染端最终布局）
		emitBarrier(RGL::createImageBarrier(image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderWrite, vk::AccessFlagBits2::eShaderSampledRead,
			level, 1, 0, 1));
	}
	if (auto* logger = RLLogger::getInstance())
		logger->debug("mip_gen {}: {} 级链（{} 次 dispatch，UNORM compute）", name, mipLevels - 1,
			mipLevels - 1);
	return {};
}

/** 契约见接口注释。实现：1024 套 × {sampled,storage,sampler} 一池，追加进 pools。 */
void MipGen::createPool() {
	constexpr uint32_t kPoolSets = 1024;
	std::array<vk::DescriptorPoolSize, 3> sizes{};
	sizes[0] = {vk::DescriptorType::eSampledImage, kPoolSets};
	sizes[1] = {vk::DescriptorType::eStorageImage, kPoolSets};
	sizes[2] = {vk::DescriptorType::eSampler, kPoolSets};
	vk::DescriptorPoolCreateInfo info{};
	info.maxSets = kPoolSets;
	info.poolSizeCount = 3;
	info.pPoolSizes = sizes.data();
	// raii::DescriptorSet 析构走 vkFreeDescriptorSets——池须带 FREE_DESCRIPTOR_SET_BIT
	info.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	pools.emplace_back(device, info);
}

/** 契约见接口注释。实现：level 1..mipLevels−1 逐级 blit（LINEAR）。每级两组 barrier：
 * blit 前 {src(level−1) SRO→TRANSFER_SRC + dst(level) UNDEFINED→TRANSFER_DST}，blit 后
 * {dst(level) TRANSFER_DST→SRO + src(level−1) TRANSFER_SRC→SRO}——源级用完必须回 SRO
 * （渲染端全链采样），否则 mip0..n−2 停在 TRANSFER_SRC。blit 源/目标是同一 image 的
 * 不同 subresource，无视图/描述符。 */
void recordMipBlitChain(vk::raii::CommandBuffer& cmd, VkImage image, uint32_t w, uint32_t h,
	uint32_t mipLevels) {
	if (mipLevels <= 1)
		return;
	const auto emitBarrier = [&](const vk::ImageMemoryBarrier2* barriers, uint32_t count) {
		VkDependencyInfo dep{};
		dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = count;
		dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(barriers);  // hpp 与裸 struct 布局兼容
		::vkCmdPipelineBarrier2(*cmd, &dep);
	};
	uint32_t srcW = w;  // 当前"源"级（level−1）尺寸，初值 = mip0
	uint32_t srcH = h;
	for (uint32_t level = 1; level < mipLevels; ++level) {
		const uint32_t dstW = std::max(1u, srcW >> 1);  // 目标级（level）尺寸
		const uint32_t dstH = std::max(1u, srcH >> 1);
		// blit 前：src 级 SRO→TRANSFER_SRC + dst 级 UNDEFINED→TRANSFER_DST。src 侧
		// stage/access 覆盖 SRO 态的全部潜在消费者（compute 下级 blit 前的链内采样 +
		// 渲染端 fragment——BOTTOM_OF_PIPE/纯 TRANSFER 不支持 SHADER_SAMPLED_READ）
		std::array<vk::ImageMemoryBarrier2, 2> pre{
			RGL::createImageBarrier(image, vk::ImageLayout::eShaderReadOnlyOptimal,
				vk::ImageLayout::eTransferSrcOptimal, vk::ImageAspectFlagBits::eColor,
				vk::PipelineStageFlagBits2::eComputeShader | vk::PipelineStageFlagBits2::eFragmentShader,
				vk::PipelineStageFlagBits2::eTransfer,
				vk::AccessFlagBits2::eShaderSampledRead, vk::AccessFlagBits2::eTransferRead,
				level - 1, 1, 0, 1),
			RGL::createImageBarrier(image, vk::ImageLayout::eUndefined,
				vk::ImageLayout::eTransferDstOptimal, vk::ImageAspectFlagBits::eColor,
				vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eTransfer,
				vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eTransferWrite,
				level, 1, 0, 1)};
		emitBarrier(pre.data(), 2);
		VkImageBlit region{};
		region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
		region.srcOffsets[0] = {0, 0, 0};
		region.srcOffsets[1].x = static_cast<int32_t>(srcW);  // 源级（level−1）全尺寸
		region.srcOffsets[1].y = static_cast<int32_t>(srcH);
		region.srcOffsets[1].z = 1;
		region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
		region.dstOffsets[0] = {0, 0, 0};
		region.dstOffsets[1].x = static_cast<int32_t>(dstW);  // 目标级（level）尺寸
		region.dstOffsets[1].y = static_cast<int32_t>(dstH);
		region.dstOffsets[1].z = 1;
		::vkCmdBlitImage(*cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_LINEAR);
		// blit 后：dst 级 TRANSFER_DST→SRO + src 级 TRANSFER_SRC→SRO（源级回渲染态）。
		// dst 侧 stage = FRAGMENT（最终消费者 = 渲染端采样；TRANSFER 不支持
		// SHADER_SAMPLED_READ，VVL 同上）
		std::array<vk::ImageMemoryBarrier2, 2> post{
			RGL::createImageBarrier(image, vk::ImageLayout::eTransferDstOptimal,
				vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
				vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
				vk::AccessFlagBits2::eTransferWrite, vk::AccessFlagBits2::eShaderSampledRead,
				level, 1, 0, 1),
			RGL::createImageBarrier(image, vk::ImageLayout::eTransferSrcOptimal,
				vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
				vk::PipelineStageFlagBits2::eTransfer, vk::PipelineStageFlagBits2::eFragmentShader,
				vk::AccessFlagBits2::eTransferRead, vk::AccessFlagBits2::eShaderSampledRead,
				level - 1, 1, 0, 1)};
		emitBarrier(post.data(), 2);
		srcW = dstW;
		srcH = dstH;
	}
}

/** 契约见接口注释。实现：pools 尾部池优先，VK_ERROR_OUT_OF_POOL_MEMORY = 建新池重试
 *（bistro 395 图 × ~10 级 ≈ 4000 套 > 首池 1024，扩容是常态路径）。 */
VkDescriptorSet MipGen::allocateSet() {
	for (;;) {
		const VkDescriptorPool cPool = *pools.back();    // C 句柄（raii → handle 隐式转）
		const VkDescriptorSetLayout cLayout = *setLayout;
		VkDescriptorSetAllocateInfo cInfo{};
		cInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		cInfo.descriptorPool = cPool;
		cInfo.descriptorSetCount = 1;
		cInfo.pSetLayouts = &cLayout;
		try {
			// 异常模式（默认）：失败抛 vk::SystemError，成功直返 vector<raii::DescriptorSet>
			//（raii Device 方法签名里的 DescriptorSet = raii 类——自带 pool 引用，析构自动归还）
			auto allocated = device.allocateDescriptorSets(vk::DescriptorSetAllocateInfo(cInfo));
			sets.push_back(std::move(allocated.front()));
			return *sets.back();
		} catch (const vk::SystemError& e) {
			if (static_cast<vk::Result>(e.code().value()) != vk::Result::eErrorOutOfPoolMemory)
				throw;
			createPool();
		}
	}
}

}  // namespace RGL
