module;
#include <vk_mem_alloc.h>  // VmaAllocationCreateInfo / VMA_* 宏（VmaRaii 的 C 面，不导出宏）
module render_target_cache:impl;

import render_target_cache;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;
import VmaRaii;
import vk_device;
import vk_resources;

namespace RGL {

namespace {

/**
 * 默认 usage 档（= 迁入前的老行为，本文件只写这一处）：颜色档 = colorWrite +
 * transfer 两端（blit 上屏与 shader 采样两条消费路都预留）+ sampleRead；深度档不申请
 * transfer 两端（深度格式的 transfer 支持按格式而异，且本缓存对深度 RT 无 blit 需求
 * ——少要一个 flag 少一分驱动差异面）。
 * @param isDepth 深度面（aspect 含 eDepth）
 * @return image usage
 */
vk::ImageUsageFlags defaultUsage(bool isDepth) {
	return isDepth
		? (vk::ImageUsageFlagBits::eDepthStencilAttachment | vk::ImageUsageFlagBits::eSampled)
		: (vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc
			  | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
}

/**
 * 规格校验（先于任何 GPU 侧动作：报错不得留下"图已换、描述符没写"的半态）。
 *
 * ① 一个附件至多一个 SAMPLED_IMAGE = `heapSlot(name)` 的语义前提（"那个采样槽"）。
 *    多于一个时"哪个是采样槽"没有答案，静默取第一个 = 碰运气，故直接点名。
 * ② 同名二次调用的描述符规格（项数 / 类型 / layout / 子资源范围）必须与首建一致：
 *    烘进管线条目的槽号只对"规格不变、只有图在换"这一种漂移有意义；而项数变了还会
 *    走到 `descriptors.resize()` = 已交给驱动的指针链悬空。这条对账就是那条指针链
 *    不变式的机器守卫，不靠注释自觉。
 * @param name 附件名（错误串归属）
 * @param spec 本次申报的规格
 * @param existing 已存在的附件；nullptr = 首建（无账可对）
 * @return 空 = 通过；Error = 违规（带附件名与项号，可归因）
 */
std::expected<void, Error> checkSpec(const std::string& name, const RtAttachmentSpec& spec,
	const RtAttachment* existing) {
	size_t sampledCount = 0;
	for (const auto& d : spec.descriptors)
		sampledCount += d.type == vk::DescriptorType::eSampledImage ? 1u : 0u;
	if (sampledCount > 1)
		return std::unexpected(Error{ErrorCode::VulkanError,
			name + ": 至多一个 SAMPLED_IMAGE 描述符（实到 " + std::to_string(sampledCount)
				+ " 个）——heapSlot(name) 无法指名采样槽"});
	if (existing == nullptr)
		return {};
	if (existing->descriptors.size() != spec.descriptors.size())
		return std::unexpected(Error{ErrorCode::VulkanError,
			name + ": 描述符项数漂移 " + std::to_string(existing->descriptors.size()) + " → "
				+ std::to_string(spec.descriptors.size())});
	for (size_t i = 0; i < spec.descriptors.size(); ++i) {
		const auto& o = existing->descriptors[i];
		const auto& n = spec.descriptors[i];
		if (o.type != n.type || o.layout != n.layout || o.range != n.range)
			return std::unexpected(Error{ErrorCode::VulkanError,
				name + ": 第 " + std::to_string(i) + " 个描述符规格漂移（type/layout/range）"});
	}
	return {};
}

/**
 * 按规格把附件的每个描述符附件项写进 heap 槽（首建定容 + 定号，重建只换图句柄）。
 *
 * 🔴 跨重建复用：`descriptors` 只在首建 resize，此后**永不 resize**（vector 缓冲地址
 * = 描述符指针链的稳定前提，见 RtDescriptor 头注释），且**只有首建领号**——重建换图
 * 不换槽，烘进管线条目的槽号因此永久有效。forward/offscreen 图不 bind 这些槽 =
 * 无副作用（行为中性）。
 * @param slot 附件（map 节点，地址稳定）
 * @param name 附件名（领号时写进槽台账的 owner 名）
 * @param spec 规格（已过 checkSpec）
 * @param firstBuild 首建标记（true = 定容 + 定号）
 * @param kernel 资源内核（领号 + 槽地址）
 * @param device 设备层（描述符写入）
 * @return 空 = 全部写好；Error = 领号 / 槽地址解析失败
 */
std::expected<void, Error> syncDescriptors(RtAttachment& slot, const std::string& name,
	const RtAttachmentSpec& spec, bool firstBuild, RGL::Resources& kernel,
	RGL::DeviceContext& device) {
	auto& descriptorHeap = kernel.getDescriptorHeap();
	if (firstBuild)
		slot.descriptors.resize(spec.descriptors.size());
	for (size_t i = 0; i < spec.descriptors.size(); ++i) {
		const auto& dspec = spec.descriptors[i];
		auto& d = slot.descriptors[i];
		if (firstBuild) {
			d.type = dspec.type;
			d.layout = dspec.layout;
			d.range = dspec.range;
		}
		if (dspec.heapSlot) {
			// 外部台账预发号（装配期已烘进管线的槽）：缓存只写描述符，不领号
			d.heapSlot = *dspec.heapSlot;
		} else if (firstBuild) {
			// 🔴 跨重建复用守卫：只有首建领号，重建换图**不换槽**（去掉这个
			// firstBuild 守卫 = 每次重建重新领号 = 管线 baked 的槽号失效）
			auto sampleSlot = kernel.allocSlot(RGL::SlotSegment::Runtime, name.c_str());
			if (!sampleSlot)
				return std::unexpected(std::move(sampleSlot).error());
			d.heapSlot = *sampleSlot;
		}
		d.viewCreate = vk::ImageViewCreateInfo{};
		d.viewCreate.image = slot.image.image;
		d.viewCreate.viewType = vk::ImageViewType::e2D;
		d.viewCreate.format = slot.format;
		d.viewCreate.subresourceRange = d.range;
		d.imageDesc = vk::ImageDescriptorInfoEXT{};
		d.imageDesc.pView = &d.viewCreate;  // v341 契约：pView 指 create info（非 VkImageView）
		d.imageDesc.layout = d.layout;
		d.resourceDesc = vk::ResourceDescriptorInfoEXT{};
		d.resourceDesc.type = d.type;
		d.resourceDesc.data.pImage = &d.imageDesc;
		auto slotAddr = descriptorHeap.getSlotCpuAddress(d.heapSlot);
		if (!slotAddr)
			return std::unexpected(std::move(slotAddr).error());
		d.dst = vk::HostAddressRangeEXT{*slotAddr, descriptorHeap.getDescriptorSize()};
		device.getDevice().writeResourceDescriptorsEXT(d.resourceDesc, d.dst);
	}
	return {};
}

}  // namespace

/**
 * 老四参入口 = 把"过去的行为"写成一份规格，再交给规格入口。老路径**没有第二条实现
 * 可漂**（规格入口是唯一实现处）。
 */
std::expected<RtAttachment*, Error> RenderTargetCache::ensure(
	const std::string& name, vk::Extent2D extent, vk::Format format,
	vk::ImageAspectFlags aspect) {
	RtAttachmentSpec spec{};
	spec.aspect = aspect;  // mipLevels 恒 1、usage 留空 = 默认档、dedicatedMemory 恒 false
	spec.descriptors.push_back(RtDescriptorSpec{
		.range = vk::ImageSubresourceRange{aspect, 0, 1, 0, 1},
		.type = vk::DescriptorType::eSampledImage,
		.layout = vk::ImageLayout::eShaderReadOnlyOptimal,
		.heapSlot = std::nullopt,  // 槽由缓存在 Runtime 段领
	});
	return ensure(name, extent, format, spec);
}

/**
 * 创建/重建行为契约见 RtAttachment 注释。实现：先对账规格（不合规即早退，不碰 GPU）；
 * 命中 = 已存在且 extent/format 均匹配（不重建）；重建先 waitIdle（双帧在飞安全）再换
 * image/view、generation+1，最后逐项同步描述符（首建定号、跨重建复用）。
 */
std::expected<RtAttachment*, Error> RenderTargetCache::ensure(
	const std::string& name, vk::Extent2D extent, vk::Format format,
	const RtAttachmentSpec& spec) {
	const auto it = rtAttachments.find(name);
	const RtAttachment* existing = it == rtAttachments.end() ? nullptr : &it->second;
	if (auto checked = checkSpec(name, spec, existing); !checked)
		return std::unexpected(std::move(checked).error());
	if (existing != nullptr && existing->extent == extent && existing->format == format)
		return &rtAttachments[name];  // 命中：不重建（drawFrame 图循环每帧调用走这条）

	// 重建（extent 随 swapchain resize / 格式漂移）：双帧在飞下旧 RT 可能仍被
	// 另一帧命令引用 → waitIdle 后替换（同 recreateDepthImage 规则）
	device.getDevice().waitIdle();

	vk::ImageCreateInfo info{};
	info.imageType = vk::ImageType::e2D;
	info.format = format;
	info.extent = vk::Extent3D(extent.width, extent.height, 1);
	info.mipLevels = spec.mipLevels;  // 老入口恒 1（= 老行为）
	info.arrayLayers = 1;
	info.samples = vk::SampleCountFlagBits::e1;
	info.tiling = vk::ImageTiling::eOptimal;
	info.initialLayout = vk::ImageLayout::eUndefined;
	// 空 usage = 老行为档（颜色/深度两档见 defaultUsage）；规格入口自报 usage
	info.usage = spec.usage != vk::ImageUsageFlags{}
		? spec.usage
		: defaultUsage(spec.aspect == vk::ImageAspectFlagBits::eDepth);
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	if (spec.dedicatedMemory)
		alloc.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
	auto imageResult = kernel.getAllocator().createImage(info, alloc, nullptr, spec.allocationName);
	if (!imageResult)
		return std::unexpected(std::move(imageResult).error());

	RtAttachment& slot = rtAttachments[name];  // 覆盖旧值（map 节点地址稳定）
	// 🔴 首建标记必须在覆盖旧值**之前**读：descriptors 空 = 这个 key 第一次建
	//（重建路径上它非空 ⟹ 不 resize、不重新领号）
	const bool firstBuild = slot.descriptors.empty();
	const uint64_t prevGeneration = slot.generation;
	slot.image = std::move(*imageResult);
	slot.extent = extent;
	slot.format = format;
	slot.generation = prevGeneration + 1;
	vk::ImageViewCreateInfo viewInfo{};
	viewInfo.image = slot.image.image;
	viewInfo.viewType = vk::ImageViewType::e2D;
	viewInfo.format = format;
	viewInfo.subresourceRange = {spec.aspect, 0, spec.mipLevels, 0, 1};
	slot.view.emplace(device.getDevice(), viewInfo);

	if (auto synced = syncDescriptors(slot, name, spec, firstBuild, kernel, device); !synced)
		return std::unexpected(std::move(synced).error());
	return &slot;
}

}  // namespace RGL
