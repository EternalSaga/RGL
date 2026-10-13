module;
#include <vk_mem_alloc.h>  // VMA 分配类型（VmaRaii 不导出 C 层，使用方自含）
export module render_target_cache;

import rlstd;
import rlvk;
import vkerror;
import VmaRaii;
import vk_device;
import vk_resources;  // 架构债务续账步骤 2：RT 缓存从内核拆出——缓存向内核引 allocator/heap/槽，单向

namespace RGL {

/**
 * offscreen RT 缓存条目（Phase 6 批次 4；架构债务续账步骤 2 随 RenderTargetCache 拆出，
 * 原 Resources::RtAttachment）。key = attachment 名，幂等创建 / extent 失配重建（随
 * swapchain resize，重建前 waitIdle——双帧在飞下旧 RT 可能仍被另一帧命令引用，同
 * recreateDepthImage 规则）。
 * usage = COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST | SAMPLED（blit 上屏与
 * shader 采样两条消费路都预留）；format 由调用点解析（"inherit" = swapchain 格式，
 * blit 到后缓冲要求同格式类，LINEAR filter 的 VUL 兼容前提）。
 * 存于 std::map，节点地址稳定（view 的 raii 对象与描述符指针链不 move）。
 */
export struct RtAttachment {
	RGL::VmaImage image;  // offscreen RT 图（extent/format 漂移时重建）
	std::optional<vk::raii::ImageView> view;  // RT view（aspect 随 ensure 参数：颜色 / 深度）
	vk::Extent2D extent{};  // 图 extent（重建命中判断条件之一）
	vk::Format format = vk::Format::eUndefined;  // v341 raii::ImageView 无 getFormat，自建记录
	// 重建计数：extent/format 漂移换新图（真实布局回 Undefined）——消费方
	//（Renderer 布局追踪）据此重置，旧值失配 = spec 违例
	uint64_t generation = 0;
	// Phase 7 gap ④：供 deferred light pass 采样的 SAMPLED_IMAGE 描述符链。
	// heapSlot 首建 allocSlot 分配、跨 resize 复用（重建只换 image/view、槽不变，
	// 故 light 管线 baked 的槽号永久有效）。描述符指针链（imageDesc.pView→
	// sampleViewCreate、resourceDesc.data.pImage→imageDesc）存本结构体——std::map
	// 节点地址稳定，满足 v341 writeResourceDescriptors 指针生命周期前提（同 TextureAsset）
	uint32_t heapSlot = 0;  // SAMPLED_IMAGE 槽号（首建 allocSlot，跨 resize 复用）
	bool hasSampleDescriptor = false;  // 采样描述符已建标记（首建后恒 true）
	vk::ImageViewCreateInfo sampleViewCreate{};  // imageDesc.pView 的稳定指向目标
	vk::ImageDescriptorInfoEXT imageDesc{};  // data.pImage 链上游（v341：pView 指 create info）
	vk::ResourceDescriptorInfoEXT resourceDesc{};  // eSampledImage 描述符定义
	vk::HostAddressRangeEXT dst{};  // 描述符写入目标槽范围（getSlotCpuAddress 解析）
};

/**
 * offscreen RT 缓存（2026-09-19 架构债务续账步骤 2：原 Resources::ensureRtAttachment /
 * findRtAttachment / rtHeapSlot + rtAttachments map 整体搬出——通用"名字 → RT"缓存，
 * 内核只留"写描述符进 heap 槽"这类内容中性操作）。**不认识任何具体 pass**：deferred
 * G-buffer / scene_hdr 的装配函数在 render_graph（rtslot partition，契约表
 * kGBufferAttachments 本家）。生命周期：main 栈对象，构造于 Resources 之后、
 * Renderer/drawFrame 之前。
 */
export class RenderTargetCache {
   public:
	/**
	 * @param kernel 资源内核（VMA 分配器 / descriptor heap / allocSlot 领槽）
	 * @param device 设备层（waitIdle / 描述符写入）
	 */
	RenderTargetCache(RGL::Resources& kernel, RGL::DeviceContext& device)
		: kernel(kernel), device(device) {}

	/**
	 * 幂等创建（或 extent/format 失配时重建）一个 offscreen RT，返回 map 节点稳定指针
	 *（std::expected 不支持引用类型，AGENTS 规范）。行为契约见 RtAttachment 注释。
	 *
	 * **深度 RT（批 11.2 基建）**：aspect = eDepth 时三处随深度面走——① image usage 用
	 * `DEPTH_STENCIL_ATTACHMENT | SAMPLED`（**不加** transfer 两端：深度格式对 transfer
	 * 的支持按格式而异，且本缓存无需求，多要一个 flag 就多一份驱动差异面）；② RT view 与
	 * 采样 view 的 subresourceRange.aspectMask = eDepth（漏这一步 = 颜色 view 指向深度图，
	 * invalid usage）；③ 采样描述符 layout 仍 eShaderReadOnlyOptimal（深度**独占**格式允许
	 * 该布局，与主深度图的 SSAO 采样描述符同约定，见 Resources::writeDepthSampleDescriptor）。
	 * @param name attachment 名（缓存 key；同名同 extent 同格式命中复用——
	 *   drawFrame 图循环按图声明每帧调用）
	 * @param extent RT extent（= swapchain extent，或图属独立尺寸如阴影图）
	 * @param format RT 格式（调用点解析，"inherit" = swapchain 格式；深度档走 kDepthRgFormats）
	 * @param aspect view aspect（eColor 默认 = 既有颜色 RT 行为逐位不变；eDepth = 深度 RT）
	 * @return 稳定指针（extent/format 漂移重建：waitIdle 后换 image/view、generation+1、
	 *   采样槽号不变；Error = VMA 图像创建 / allocSlot / 槽地址查询失败）
	 */
	[[nodiscard]] std::expected<RtAttachment*, Error> ensure(const std::string& name,
		vk::Extent2D extent, vk::Format format,
		vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

	/** @return RT 的 SAMPLED_IMAGE heap 槽号（ensure 首建分配、跨 resize
	 * 复用，管线 baked 永久有效）；尚未 ensure 的 RT 名 → nullopt（调用方须先 ensure
	 * 该 RT） */
	[[nodiscard]] std::optional<uint32_t> heapSlot(const std::string& name) const {
		auto it = rtAttachments.find(name);
		if (it == rtAttachments.end() || !it->second.hasSampleDescriptor)
			return std::nullopt;
		return it->second.heapSlot;
	}

	/** @return 已存在 RT 的稳定指针（帧内捕获直读 GB image/extent/format；未建 =
	 * nullptr——deferred 图每帧 ensure，正常路径必命中） */
	RtAttachment* find(const std::string& name) {
		auto it = rtAttachments.find(name);
		return it == rtAttachments.end() ? nullptr : &it->second;
	}

   private:
	RGL::Resources& kernel;  // 资源内核引用（allocator / descriptorHeap / allocSlot）
	RGL::DeviceContext& device;  // 设备层引用（waitIdle / writeResourceDescriptorsEXT）
	std::map<std::string, RtAttachment> rtAttachments;  // std::map 节点地址稳定（RtAttachment 指针链前提）
};

/**
 * 创建/重建行为契约见 RtAttachment 注释。实现：命中 = 已存在且 extent/format
 * 均匹配（不重建）；重建先 waitIdle（双帧在飞安全）再换 image/view、generation+1；
	 * SAMPLED_IMAGE 槽首建 allocSlot Runtime、跨 resize 复用（槽号永久有效，light 管线 baked 不失效）；
 * 描述符指针链存 map 节点（v341 契约：imageDesc.pView 指 create info 而非 VkImageView，
 * 见体内注释）；forward/offscreen 图不 bind 此槽 = 无副作用（行为中性）。
 */
inline std::expected<RtAttachment*, Error> RenderTargetCache::ensure(
		const std::string& name, vk::Extent2D extent, vk::Format format,
		vk::ImageAspectFlags aspect) {
		const bool isDepth = aspect == vk::ImageAspectFlagBits::eDepth;
		auto& descriptorHeap = kernel.getDescriptorHeap();
		auto it = rtAttachments.find(name);
		if (it != rtAttachments.end() && it->second.extent == extent &&
			it->second.format == format)
			return &it->second;

		// 重建（extent 随 swapchain resize / 格式漂移）：双帧在飞下旧 RT 可能仍被
		// 另一帧命令引用 → waitIdle 后替换（同 recreateDepthImage 规则）
		device.getDevice().waitIdle();

		vk::ImageCreateInfo info{};
		info.imageType = vk::ImageType::e2D;
		info.format = format;
		info.extent = vk::Extent3D(extent.width, extent.height, 1);
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = vk::SampleCountFlagBits::e1;
		info.tiling = vk::ImageTiling::eOptimal;
		info.initialLayout = vk::ImageLayout::eUndefined;
		// 消费面：colorWrite（渲入）+ transferRead/Write（blit 两端）+ sampleRead
		//（shader 采样，后处理类图预留）。深度档（批 11.2 阴影图）：DEPTH_STENCIL_ATTACHMENT
		// + SAMPLED，**不申请 transfer 两端**（深度格式的 transfer 支持按格式而异，
		// 且本缓存对深度 RT 无 blit 需求——少要一个 flag 少一分驱动差异面）
		info.usage = isDepth
			? (vk::ImageUsageFlagBits::eDepthStencilAttachment
				  | vk::ImageUsageFlagBits::eSampled)
			: (vk::ImageUsageFlagBits::eColorAttachment
				  | vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst
				  | vk::ImageUsageFlagBits::eSampled);
		VmaAllocationCreateInfo alloc{};
		alloc.usage = VMA_MEMORY_USAGE_AUTO;
		auto imageResult = kernel.getAllocator().createImage(info, alloc, nullptr, "render_target_cache/rt");
		if (!imageResult)
			return std::unexpected(std::move(imageResult).error());

		RtAttachment& slot = rtAttachments[name];  // 覆盖旧值（map 节点地址稳定）
		const uint64_t prevGeneration = slot.generation;
		slot.image = std::move(*imageResult);
		slot.extent = extent;
		slot.format = format;
		slot.generation = prevGeneration + 1;
		vk::ImageViewCreateInfo viewInfo{};
		viewInfo.image = slot.image.image;
		viewInfo.viewType = vk::ImageViewType::e2D;
		viewInfo.format = format;
		viewInfo.subresourceRange = {aspect, 0, 1, 0, 1};
		slot.view.emplace(device.getDevice(), viewInfo);

		// Phase 7 gap ④：写 SAMPLED_IMAGE 描述符供 deferred light pass 采样。首建 allocSlot Runtime，
		// 跨 resize 复用同槽（重建只换 image→更新 sampleViewCreate，指针链稳定）；light 管线
		// baked 的槽号因此永久有效。forward/offscreen 图不 bind 此槽 = 无副作用（行为中性）。
		if (!slot.hasSampleDescriptor) {
			auto sampleSlot = kernel.allocSlot(RGL::SlotSegment::Runtime, name.c_str());
			if (!sampleSlot)
				return std::unexpected(std::move(sampleSlot).error());
			slot.heapSlot = *sampleSlot;
			slot.hasSampleDescriptor = true;
		}
		slot.sampleViewCreate = vk::ImageViewCreateInfo{};
		slot.sampleViewCreate.image = slot.image.image;
		slot.sampleViewCreate.viewType = vk::ImageViewType::e2D;
		slot.sampleViewCreate.format = format;
		slot.sampleViewCreate.subresourceRange = {aspect, 0, 1, 0, 1};
		slot.imageDesc = vk::ImageDescriptorInfoEXT{};
		slot.imageDesc.pView = &slot.sampleViewCreate;  // v341 契约：pView 指 create info（非 VkImageView）
		slot.imageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		slot.resourceDesc = vk::ResourceDescriptorInfoEXT{};
		slot.resourceDesc.type = vk::DescriptorType::eSampledImage;
		slot.resourceDesc.data.pImage = &slot.imageDesc;
		auto slotAddr = descriptorHeap.getSlotCpuAddress(slot.heapSlot);
		if (!slotAddr)
			return std::unexpected(std::move(slotAddr).error());
		slot.dst = vk::HostAddressRangeEXT{*slotAddr, descriptorHeap.getDescriptorSize()};
		device.getDevice().writeResourceDescriptorsEXT(slot.resourceDesc, slot.dst);

		return &slot;
	}

}  // namespace RGL
