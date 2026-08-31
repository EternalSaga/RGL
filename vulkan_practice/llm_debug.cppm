module;
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
#include <filesystem>
#include <chrono>
#include <fmt/format.h>
export module llm_debug;

import rlstd;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import vk_renderer;
import VmaRaii;
import shader;
import vulkan_utils;

namespace RGL {

// 一次性截图配置（configs/camera.lua）：
//   llm_debug = true/false 纯开关（缺省 false，交互模式零变化）——截图只改这一行；
//   llm_shot  = {file, size, delay_frames} 常驻截图配置（全可选回落默认）。
// 向后兼容：llm_debug 仍接受 table（旧格式 = 启用 + 细字段，覆盖 llm_shot）。
// 相机位置不走额外字段——agent 直接编辑 camera.lua 顶层的
// position/target/fovDegrees（loadCameraConfig 现有解析），无 presets 层。
export struct LlmDebugConfig {
	bool enabled = false;
	uint32_t delayFrames = 0;  // 第 N 帧渲染完成后截图（0 = 首帧；为动态场景预留）
	std::string file;          // PNG 路径（相对 CWD）；空 = screenshots/llm_<时间戳>.png
	uint32_t width = 1280;    // llm_debug 模式的离屏窗口尺寸（隐藏窗，永不显示）
	uint32_t height = 720;
};

// 离屏一次性渲染 + 截图（llm_debug 模式，每次运行至多调用一次）：
// 把给定帧的 draw list 重渲到临时离屏目标（8-bit UNORM 颜色 + D32Sfloat 深度，
// 与上屏 pass 同状态同管线表），回读 CPU 写 PNG。
//
// 为什么离屏重渲而不读回 swapchain 后缓冲：后缓冲的 present 生命周期由
// compositor 掌控（Wayland 息屏后 present 可能永久停滞，后缓冲状态不可靠），
// 而离屏目标是自持资源 + 独立提交/独立 fence——对 present 循环状态免疫。
// 8-bit UNORM 直接映射 PNG，免 HDR 格式转换。
//
// 返回 PNG 绝对路径（agent 据此读取截图）。
export [[nodiscard]] std::expected<std::string, Error>
captureFrame(RGL::Renderer& renderer, const RGL::FrameDrawList& list, const LlmDebugConfig& cfg) {
	auto* logger = RLLogger::getInstance();
	auto& deviceCtx = renderer.getDeviceContextRef();
	auto& device = deviceCtx.getDevice();
	vk::Queue queue = deviceCtx.getGraphicsQueue();
	auto& resources = renderer.getResourcesRef();
	auto& heap = resources.getDescriptorHeap();
	// 目标尺寸 = 配置的窗口尺寸（llm 模式窗口按 cfg.width/height 创建），不用 swapchain
	// extent——Wayland compositor 对隐藏窗的 extent 协商不稳定（实测 KWin 有时 clamp
	// 到 1920×1080，同配置运行间漂移），截图尺寸必须确定（4K 黑屏排查 2026-08-27）
	const uint32_t w = cfg.width;
	const uint32_t h = cfg.height;
	const vk::DeviceSize colorBytes = static_cast<vk::DeviceSize>(w) * h * 4;

	// ① 离屏颜色目标（8-bit UNORM：4 字节/像素，直接 PNG 拷贝；VMA 自动绑内存，
	// Hpp raii Image 构造不绑内存不可用）
	auto& allocator = resources.getAllocator();
	vk::ImageCreateInfo colorInfo{};
	colorInfo.imageType = vk::ImageType::e2D;
	colorInfo.format = vk::Format::eR8G8B8A8Unorm;
	colorInfo.extent = vk::Extent3D(w, h, 1);
	colorInfo.mipLevels = 1;
	colorInfo.arrayLayers = 1;
	colorInfo.samples = vk::SampleCountFlagBits::e1;
	colorInfo.tiling = vk::ImageTiling::eOptimal;
	colorInfo.initialLayout = vk::ImageLayout::eUndefined;
	colorInfo.usage =
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
	VmaAllocationCreateInfo colorAlloc{};
	colorAlloc.usage = VMA_MEMORY_USAGE_AUTO;
	auto colorImageResult = allocator.createImage(colorInfo, colorAlloc);
	if (!colorImageResult)
		return std::unexpected(std::move(colorImageResult).error());
	auto colorImage = std::move(*colorImageResult);

	// ② 深度附件（与上屏同格式 D32Sfloat：离屏 pass 的深度测试行为与上屏一致；
	// 结果无人再读，DontCare）
	vk::ImageCreateInfo depthInfo{};
	depthInfo.imageType = vk::ImageType::e2D;
	depthInfo.format = vk::Format::eD32Sfloat;
	depthInfo.extent = vk::Extent3D(w, h, 1);
	depthInfo.mipLevels = 1;
	depthInfo.arrayLayers = 1;
	depthInfo.samples = vk::SampleCountFlagBits::e1;
	depthInfo.tiling = vk::ImageTiling::eOptimal;
	depthInfo.initialLayout = vk::ImageLayout::eUndefined;
	depthInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
	VmaAllocationCreateInfo depthAlloc{};
	depthAlloc.usage = VMA_MEMORY_USAGE_AUTO;
	auto depthImageResult = allocator.createImage(depthInfo, depthAlloc);
	if (!depthImageResult)
		return std::unexpected(std::move(depthImageResult).error());
	auto depthImage = std::move(*depthImageResult);

	// image 句柄嵌在 createInfo 内（Hpp ImageView 单参构造，同 vkresources 深度 view 模式）
	vk::ImageViewCreateInfo colorViewInfo{};
	colorViewInfo.image = *colorImage;
	colorViewInfo.viewType = vk::ImageViewType::e2D;
	colorViewInfo.format = vk::Format::eR8G8B8A8Unorm;
	colorViewInfo.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	auto colorView = vk::raii::ImageView(device, colorViewInfo);

	vk::ImageViewCreateInfo depthViewInfo{};
	depthViewInfo.image = *depthImage;
	depthViewInfo.viewType = vk::ImageViewType::e2D;
	depthViewInfo.format = vk::Format::eD32Sfloat;
	depthViewInfo.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
	auto depthView = vk::raii::ImageView(device, depthViewInfo);

	// ③ 回读缓冲（线性，行距 = w*4 紧凑，无额外 padding；常驻映射——VMA 直接给
	// CPU 指针，免手动 map/unmap）
	vk::BufferCreateInfo readbackInfo{};
	readbackInfo.size = colorBytes;
	readbackInfo.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo readbackAlloc{};
	// CPU_TO_GPU = host-visible + device-local（RADV 上缓存一致）：GPU 写入、CPU 读回。
	// 勿用 VMA_MEMORY_USAGE_AUTO——AUTO 对 eTransferDst 缓冲的启发式会选
	// device-local 类型（非 host-visible），MAPPED 位被静默忽略、pMappedData 为 NULL
	readbackAlloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	readbackAlloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo readbackAllocData{};
	auto readbackResult = allocator.createBuffer(readbackInfo, readbackAlloc, &readbackAllocData);
	if (!readbackResult)
		return std::unexpected(std::move(readbackResult).error());
	auto readback = std::move(*readbackResult);

	// ④ 一次性命令缓冲（从渲染器的图形池分配：本函数在帧循环停顿期运行，
	// 与 drawFrame 的 per-slot 命令缓冲无并发）+ 独立 fence（不占双帧 slot）
	vk::CommandBufferAllocateInfo cmdAllocInfo{
		*renderer.getGraphicsCommandPoolRef(), vk::CommandBufferLevel::ePrimary, 1};
	auto allocatedCmds = device.allocateCommandBuffers(cmdAllocInfo);
	auto cmd = std::move(allocatedCmds[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});
	auto fence = vk::raii::Fence(device, vk::FenceCreateInfo{});

	// ⑤ 离屏目标布局迁移：Undefined → AttachmentOptimal（同 drawFrame ④）
	vk::ImageMemoryBarrier2 colorToDrawBarrier = RGL::createImageBarrier(
		*colorImage, vk::ImageLayout::eUndefined,
		vk::ImageLayout::eColorAttachmentOptimal, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eColorAttachmentWrite);
	vk::ImageMemoryBarrier2 depthToDrawBarrier = RGL::createImageBarrier(
		*depthImage, vk::ImageLayout::eUndefined,
		vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageAspectFlagBits::eDepth,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eDepthStencilAttachmentRead |
			vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
	std::array<vk::ImageMemoryBarrier2, 2> drawBarriers = {colorToDrawBarrier, depthToDrawBarrier};
	vk::DependencyInfo drawDepInfo;
	drawDepInfo.setImageMemoryBarriers(drawBarriers);
	cmd.pipelineBarrier2(drawDepInfo);

	// ⑥ descriptor heap 同步 + 绑定（同 drawFrame ⑥⑦：host write → heap read 的
	// memory barrier 必须在 beginRendering 之前；heapRange 用 getHeapSize() 已扣偏移）
	vk::MemoryBarrier2 heapBarrier{};
	heapBarrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
	// 同 drawFrame ⑥：sampler heap 读访问 flag 独立于 resource heap，须一并声明
	//（缺 eSamplerHeapReadEXT 时 sampler 描述符未同步 → 采样恒 garbage）
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT | vk::AccessFlagBits2::eSamplerHeapReadEXT;
	heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
	heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
	vk::DependencyInfo heapDepInfo{};
	heapDepInfo.setMemoryBarriers(heapBarrier);
	cmd.pipelineBarrier2(heapDepInfo);
	vk::DeviceAddressRangeEXT heapRange{};
	heapRange.address = heap.getGpuAddress();
	heapRange.size = heap.getHeapSize();
	cmd.bindResourceHeapEXT(vk::BindHeapInfoEXT(
		heapRange, resources.getReservedRangeOffset(), resources.getReservedRangeSize()));
	// sampler heap 同法绑定（Phase 3，同 drawFrame ⑦）
	const auto& samplerHeap = resources.getSamplerHeap();
	vk::DeviceAddressRangeEXT samplerHeapRange{};
	samplerHeapRange.address = samplerHeap.getGpuAddress();
	samplerHeapRange.size = samplerHeap.getHeapSize();
	cmd.bindSamplerHeapEXT(vk::BindHeapInfoEXT(
		samplerHeapRange, 0, resources.getSamplerReservedRangeSize()));

	// ⑦ beginRendering：8-bit 颜色（Store，回读要用）+ 深度（清 1.0，DontCare）
	vk::RenderingAttachmentInfo colorAttachment{};
	colorAttachment.imageView = *colorView;
	colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
	colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	colorAttachment.clearValue = vk::ClearValue{
		std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f}};
	vk::RenderingAttachmentInfo depthAttachment{};
	depthAttachment.imageView = *depthView;
	depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
	depthAttachment.storeOp = vk::AttachmentStoreOp::eDontCare;
	depthAttachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};
	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = vk::Rect2D({0, 0}, vk::Extent2D(w, h));
	renderingInfo.layerCount = 1;
	renderingInfo.setColorAttachments(colorAttachment);
	renderingInfo.setPDepthAttachment(&depthAttachment);
	cmd.beginRendering(renderingInfo);

	// ⑧ 绘制（同 drawFrame ⑨ 契约：opaque/transparent 两段各段首 applyDynamicState
	// 一次——离屏 pass 与上屏 pass 状态同构，透明排序在截图路径同样生效）
	auto drawCall = [&](const RGL::DrawCall& call) {
		// 同主路径 drawCall 契约：push set 取条目字段（非硬编码 0），两路径不漂移
		const auto& entry = renderer.pipelineEntryAt(call.pipeline);
		cmd.pushDataEXT(vk::PushDataInfoEXT(
			entry.pushDataSet, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
		entry.pipeline.bindShaders(cmd);
		cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
	};
	RGL::applyDynamicState(cmd, w, h, renderer.getOpaqueState());
	for (uint32_t i = 0; i < list.transparentStart; ++i)
		drawCall(list.calls[i]);
	if (list.transparentStart < list.calls.size()) {
		RGL::applyDynamicState(cmd, w, h, renderer.getTransparentState());
		for (uint32_t i = list.transparentStart; i < static_cast<uint32_t>(list.calls.size()); ++i)
			drawCall(list.calls[i]);
	}
	cmd.endRendering();

	// ⑩ 颜色：ColorAttachmentOptimal → TransferSrcOptimal（回读拷贝前）
	vk::ImageMemoryBarrier2 colorToReadback = RGL::createImageBarrier(
		*colorImage, vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::eTransferSrcOptimal, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::AccessFlagBits2::eTransferRead);
	vk::DependencyInfo readbackDepInfo;
	readbackDepInfo.setImageMemoryBarriers(colorToReadback);
	cmd.pipelineBarrier2(readbackDepInfo);

	// ⑪ 拷贝到回读缓冲（行距 0 = 紧凑 w*4；裸 C 调用，同 drawFrame ⑬ 风格）
	VkBufferImageCopy copy{};
	copy.bufferOffset = 0;
	copy.bufferRowLength = 0;
	copy.bufferImageHeight = h;
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageOffset = {0, 0, 0};
	copy.imageExtent = {w, h, 1};
	::vkCmdCopyImageToBuffer(*cmd, *colorImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		*readback, 1, &copy);
	cmd.end();

	// ⑫ 提交 + 等待（独立 fence；裸 C 提交同 drawFrame ⑬ 风格）
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkCommandBuffer cb = *cmd;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cb;
	VkFence rawFence = *fence;
	if (auto result = vk::Result(::vkQueueSubmit(queue, 1, &submitInfo, rawFence));
		result != vk::Result::eSuccess) {
		if (logger)
			logger->error("llm_debug: capture submit failed: {}", vk::to_string(result));
		return std::unexpected(Error::fromVkResult(result, "llm_debug capture submit"));
	}
	(void)device.waitForFences(*fence, true, std::numeric_limits<uint64_t>::max());
	device.resetFences(*fence);

	// ⑬ 回读 + 写 PNG（回读缓冲 VMA 常驻映射，CPU 指针直接用）
	const uint8_t* pixels = static_cast<const uint8_t*>(readbackAllocData.pMappedData);
	std::filesystem::path relPath;
	if (cfg.file.empty()) {
		relPath = fmt::format("screenshots/llm_{}.png",
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
	} else {
		relPath = cfg.file;
	}
	std::error_code ec;
	std::filesystem::create_directories(relPath.parent_path(), ec);
	bool pngOk = stbi_write_png(relPath.string().c_str(), static_cast<int>(w),
		static_cast<int>(h), 4, pixels, static_cast<int>(w) * 4);
	if (!pngOk) {
		if (logger)
			logger->error("llm_debug: stbi_write_png failed: {}", relPath.string());
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("llm_debug: failed to write PNG: {}", relPath.string())});
	}
	return std::filesystem::absolute(relPath).string();
}

}  // namespace RGL
