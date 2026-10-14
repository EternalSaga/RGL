module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）；offsetof 经其 stddef.h 提供
#include <fmt/format.h>
module mip_gen:impl;

import mip_gen;  // clang 不给 partition impl unit 隐式 import 主接口
import compute_push_types_generated;  // MipGenPush（反射产物，布局有出处；不进接口 unit）
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import VmaRaii;       // VmaDescriptorHeap（heap 引用类型 + getSlotCpuAddress/getDescriptorSize）
import vulkan_utils;  // createImageBarrier（per-level 布局物化）
import shader;        // loadShaderSpv（mip_gen.comp.slang）

namespace RGL {

// 构造器 / init / addLevel / writeImageDesc / recordChain 定义（契约见接口声明）。本单元是叶子边
// （不 import vk_resources——Resources 持本模块类型，无环），编译不上关键路径。descriptor heap
// + shader object 版（迁离旧 descriptor pool）；逐 level 的 shader object 创建已收成共享点
// shader.cppm::makePerLevelComputeShader（与 env_prefilter 同一份实现）。

namespace {

/** mip_gen.comp.spv 码流（加载失败 = 启动期错误，抛出——同 GpuCull SPV 加载路径）。 */
std::vector<uint32_t> loadMipGenSpv() {
	auto spv = loadShaderSpv("generated_shaders/mip_gen.comp.spv");
	if (!spv)
		throw std::runtime_error(fmt::format("mip_gen: {}", spv.error().describe()));
	return *spv;
}

/** 本模块 push data 布局（24B）：@0 MipGenPush（12B shader push constant，**类型来自反射生成物**）
 * + 2 u32 留白 + @20 heap push index（u32，恒 0）。descriptor heap 下 push constant 经
 * vkCmdPushDataEXT 供给（vkCmdPushConstants 与 descriptor heap 互斥——见 descriptorheaps.adoc :962-965）。
 * static_assert 锁死 heapIndex 落在 kPushIndexByte（共享常量，现居 shader.cppm；漂移即编译失败）。
 *
 * 🔴 下面两条 assert **不是**同义反复，别跟着 §7.13 那批一起删：它们量的对象（留白 + heapIndex
 * 落在 @20 + 24B 总长）在 shader 里**根本不存在**——descriptor heap 的 push data 分帧是纯 C++ 侧
 * 事实。内层 push 契约一旦变长，heapIndex 会被顶离 offset 20，这两条必须炸。判据：断言量的是
 * shader 里有的东西 → 同义反复，删；量的是 shader 里没有的 C++ 侧布局 → 真不变量，留。 */
struct MipGenPushData {
	MipGenPush push;
	uint32_t pad[2];
	uint32_t heapIndex;
};
static_assert(offsetof(MipGenPushData, heapIndex) == kPushIndexByte,
	"MipGenPushData.heapIndex 必须落在 push data offset 20（与 mapping.pushOffset 同频）");
static_assert(sizeof(MipGenPushData) == 24, "MipGenPushData 须 24B（覆盖 heapIndex@20）");

}  // namespace

/** 契约见接口注释。实现：初始化列表加载 SPV + 存设备引用；体内校验 UNORM STORAGE_IMAGE
 * 能力 + 日志。**不建 shader object**（heap 槽由 Resources 后续喂入）。 */
MipGen::MipGen(RGL::DeviceContext& device)
	: device(device.getDevice()), code(loadMipGenSpv()) {
	// sRGB 纹理的 storage view 走 UNORM 重解释——UNORM 形式必须有 STORAGE_IMAGE 能力
	const auto props = device.getPhysicalDevice().getFormatProperties(vk::Format::eR8G8B8A8Unorm);
	if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage) == vk::FormatFeatureFlags{})
		throw std::runtime_error("mip_gen: R8G8B8A8_UNORM 无 STORAGE_IMAGE 能力（UNORM compute 链前提）");
	if (auto* logger = RLLogger::getInstance())
		logger->info("mip_gen: SPV 就绪（{} 字节），heap shader object 待 Resources 喂槽后建",
			code.size() * sizeof(uint32_t));
}

/** 契约见接口注释。实现：仅记 heap 引用 + sampler 槽（不拥有 heap）。幂等由调用方
 * （SamplerLibrary::ensureMipGenReady 的 mipGenBound 守卫）保证——重复调用覆盖同值无害。 */
void MipGen::init(const VmaDescriptorHeap& resourceHeapIn, const VmaDescriptorHeap& samplerHeapIn,
	uint32_t samplerSlotIn) {
	resourceHeap = &resourceHeapIn;
	samplerHeap = &samplerHeapIn;
	samplerSlot = samplerSlotIn;
}

/** 契约见接口注释。实现：经共享点 shader.cppm::makePerLevelComputeShader 建 shader object
 * （heap 尺寸由本类持有的两个 heap 现取）+ 登记 src/dst 槽 + 追加 per-level 描述符链
 * （deque 尾追加——元素地址不迁移，pView 指针稳定活过提交）。level index = mip level
 * （0 占位不用于链生成，但同批建好以便按下标直接取）。 */
std::expected<void, Error> MipGen::addLevel(uint32_t srcSlot, uint32_t dstSlot) {
	auto shader = makePerLevelComputeShader(device, code, srcSlot, dstSlot, samplerSlot,
		resourceHeap->getDescriptorSize(), samplerHeap->getDescriptorSize(), levels, "mip_gen");
	if (!shader)
		return std::unexpected(std::move(shader).error());
	shaders.push_back(std::move(*shader));
	srcSlots.push_back(srcSlot);
	dstSlots.push_back(dstSlot);
	srcChains.emplace_back();
	dstChains.emplace_back();
	++levels;
	return {};
}

/** 契约见接口注释。实现：填 chain（viewCreate/imageDesc/resourceDesc/dst 全在 chain 内，
 * 互指地址稳定）+ vkWriteResourceDescriptorsEXT 写进 resource heap 该 level 固定槽。
 * pView 指 chain.viewCreate（v341 契约：create info 值，非 VkImageView 句柄）。 */
std::expected<void, Error> MipGen::writeImageDesc(DescChain& chain, uint32_t slot, VkImage image,
	vk::Format format, uint32_t mip, vk::DescriptorType type, vk::ImageLayout layout) {
	chain.viewCreate = vk::ImageViewCreateInfo{};
	chain.viewCreate.image = image;
	chain.viewCreate.viewType = vk::ImageViewType::e2D;
	chain.viewCreate.format = format;
	chain.viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1};
	chain.imageDesc = vk::ImageDescriptorInfoEXT{};
	chain.imageDesc.pView = &chain.viewCreate;
	chain.imageDesc.layout = layout;
	chain.resourceDesc = vk::ResourceDescriptorInfoEXT{};
	chain.resourceDesc.type = type;
	chain.resourceDesc.data.pImage = &chain.imageDesc;
	auto addr = resourceHeap->getSlotCpuAddress(slot);
	if (!addr)
		return std::unexpected(std::move(addr).error());
	chain.dst = vk::HostAddressRangeEXT{*addr, resourceHeap->getDescriptorSize()};
	device.writeResourceDescriptorsEXT(chain.resourceDesc, chain.dst);
	return {};
}

/** 契约见接口注释。实现：先各 level 描述符一次性写各自固定槽（同 submission 内独立、
 * 互不覆盖），再 heap host barrier → 一次 bind resource/sampler heap → 逐 level { dst
 * UNDEFINED→GENERAL → bindShadersEXT（该 level）→ pushDataEXT（MipGenPush + @20 push index 0）
 * → dispatch → dst GENERAL→SHADER_READ_ONLY }。视图范围精确到单 level，VVL 逐 subresource
 * 布局检查干净（全链视图会在链中期看到未过渡 level 误报）。仅 UNORM（sRGB 走 blit 链）。 */
std::expected<void, Error> MipGen::recordChain(vk::raii::CommandBuffer& cmd, VkImage image,
	vk::Format format, uint32_t w, uint32_t h, uint32_t mipLevels, const char* name) {
	if (mipLevels <= 1)
		return {};
	if (resourceHeap == nullptr || samplerHeap == nullptr)
		return std::unexpected(Error{ErrorCode::Unknown, "mip_gen: heap 未 init（ensureMipGenReady 未跑）"});
	if (levelCount() < mipLevels)
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("mip_gen: level shader 不足（有 {} 需 {}）", levelCount(), mipLevels)});

	// 阶段 1：各 level 的 src/dst image 描述符写进各自固定槽（host 写；同 submission 内各
	// level 独立槽互不覆盖——这是"每 level 一对固定槽"而非"重写同槽"的根本理由）
	for (uint32_t level = 1; level < mipLevels; ++level) {
		auto s = writeImageDesc(srcChains[level], srcSlots[level], image, format, level - 1,
			vk::DescriptorType::eSampledImage, vk::ImageLayout::eShaderReadOnlyOptimal);
		if (!s)
			return s;
		auto d = writeImageDesc(dstChains[level], dstSlots[level], image, format, level,
			vk::DescriptorType::eStorageImage, vk::ImageLayout::eGeneral);
		if (!d)
			return d;
	}

	const auto emitBarrier = [&](const vk::ImageMemoryBarrier2& b) {
		VkDependencyInfo dep{};
		dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&b);
		::vkCmdPipelineBarrier2(*cmd, &dep);
	};

	// heap host barrier（host 写描述符 + 共享 sampler → compute heap 读；eHost 不能在 render
	// pass 内，本上传 CB 全程 pass 外——坑 8）
	vk::MemoryBarrier2 heapBarrier{};
	heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
	heapBarrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
	heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT |
		vk::AccessFlagBits2::eSamplerHeapReadEXT;
	vk::DependencyInfo heapDep{};
	heapDep.setMemoryBarriers(heapBarrier);
	cmd.pipelineBarrier2(heapDep);

	// bind heap + sampler heap（一次，覆盖本 CB 全部 dispatch；reserved 声明 [0, reservedSlots)
	// 不可用——本模块用的都是 ≥ reservedSlots 的用户槽）
	const uint32_t descriptorSize = resourceHeap->getDescriptorSize();
	vk::DeviceAddressRangeEXT heapRange{resourceHeap->getGpuAddress(), resourceHeap->getHeapSize()};
	cmd.bindResourceHeapEXT(vk::BindHeapInfoEXT(heapRange, 0,
		resourceHeap->getReservedSlots() * descriptorSize));
	vk::DeviceAddressRangeEXT samplerRange{samplerHeap->getGpuAddress(), samplerHeap->getHeapSize()};
	cmd.bindSamplerHeapEXT(vk::BindHeapInfoEXT(samplerRange, 0,
		samplerHeap->getReservedSlots() * samplerHeap->getDescriptorSize()));

	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	uint32_t dstW = w;
	uint32_t dstH = h;
	for (uint32_t level = 1; level < mipLevels; ++level) {
		dstW = std::max(1u, dstW >> 1);
		dstH = std::max(1u, dstH >> 1);

		// level i：UNDEFINED→GENERAL（compute 写目标）
		emitBarrier(RGL::createImageBarrier(image, vk::ImageLayout::eUndefined, vk::ImageLayout::eGeneral,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eBottomOfPipe, vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eMemoryWrite, vk::AccessFlagBits2::eShaderWrite,
			level, 1, 0, 1));
		// bind 该 level 的 shader object（烘焙到本 level 的固定 src/dst 槽）+ push data + dispatch
		std::array<vk::ShaderEXT, 1> handles = {*shaders[level]};
		cmd.bindShadersEXT(stages, handles);
		// 内层 MipGenPush 用指定初始化器 = 让"shader 里改字段名/换序"在**本调用点**编译期红
		//（位置聚合初始化与字段名无关，实测不红；C++20 另要求指定初始化按声明序 ⟹ 换序也红）。
		// 外层 24B 包装不是生成物类型（@20 是本模块 heap push index），保持位置初始化不动。
		const MipGenPushData pushData{
		    MipGenPush{.srcMip = level, .dstWidth = dstW, .dstHeight = dstH}, {0, 0}, 0u};
		cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(
			reinterpret_cast<const std::byte*>(&pushData), sizeof(pushData))));
		cmd.dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
		// level i：GENERAL→SHADER_READ_ONLY（下级的采样源 / 渲染端最终布局）
		emitBarrier(RGL::createImageBarrier(image, vk::ImageLayout::eGeneral, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderWrite, vk::AccessFlagBits2::eShaderSampledRead,
			level, 1, 0, 1));
	}
	if (auto* logger = RLLogger::getInstance())
		logger->debug("mip_gen {}: {} 级链（{} 次 dispatch，heap shader object，UNORM compute）", name,
			mipLevels - 1, mipLevels - 1);
	return {};
}

/** 契约见接口注释。实现：level 1..mipLevels−1 逐级 blit（LINEAR）。每级两组 barrier：
 * blit 前 {src(level−1) SRO→TRANSFER_SRC + dst(level) UNDEFINED→TRANSFER_DST}，blit 后
 * {dst(level) TRANSFER_DST→SRO + src(level−1) TRANSFER_SRC→SRO}——源级用完必须回 SRO
 * （渲染端全链采样），否则 mip0..n−2 停在 TRANSFER_SRC。blit 源/目标是同一 image 的
 * 不同 subresource，无视图/描述符。 */
void recordMipBlitChain2d(vk::raii::CommandBuffer& cmd, VkImage image, uint32_t w, uint32_t h,
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

}  // namespace RGL
