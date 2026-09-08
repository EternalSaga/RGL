module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <stb_image_write.h>
#include <filesystem>
#include <chrono>
#include <fmt/format.h>
module llm_debug:capture;

import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import vk_renderer;
import render_graph;  // Phase 7：G-buffer 契约表 kGBufferAttachments + resolveRgFormat（单源）
import frame_draw_list;
import camera_config;
import VmaRaii;
import shader;
import vulkan_utils;

namespace RGL {

[[nodiscard]] std::expected<std::string, Error>
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

	// image 句柄嵌在 createInfo 内（Hpp raii 单参构造，同 vkresources 深度 view 模式）
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

	auto drawCall = [&](const RGL::DrawCall& call) {
		// 同主路径 drawCall 契约：push set 取条目字段（非硬编码 0），两路径不漂移
		const auto& entry = renderer.pipelineEntryAt(call.pipeline);
		cmd.pushDataEXT(vk::PushDataInfoEXT(
			entry.pushDataSet, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
		entry.pipeline.bindShaders(cmd);
		cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
	};

	// deferred 自包含 geometry pass：本命令缓冲内把 4-MRT deferred_geometry 渲入 G-buffer RT
	// （capture 尺寸 == swapchain extent 命中已建 GB），再下方 light pass 同缓冲采样出光照。
	// 不能依赖跨命令缓冲读持久 GB——实测跨独立提交读上帧 GB 返回黑（GB 内容未对该提交建立可见性）。
	std::array<RGL::Resources::RtAttachment*, 4> gb{};
	// RGL_DUMP_GBUF=N（0..3）：deferred 下跳过 light/捕获，geometry pass 后直接 copyImageToBuffer
	// 读回第 N 个 G-buffer RT 出 PNG（descriptor 无关，直查 geometry 是否写进 GB）。
	int gbDumpCh = -1;
	if (renderer.isDeferred()) {
		if (const char* g = std::getenv("RGL_DUMP_GBUF")) {
			gbDumpCh = std::atoi(g);
			if (gbDumpCh < 0 || gbDumpCh > 3) gbDumpCh = 0;
		}
		// 名字/格式 = render_graph kGBufferAttachments 契约表（单源，与图 desc/vkresources 共享）
		const vk::Extent2D gbExt(w, h);
		for (uint32_t i = 0; i < kGBufferAttachments.size(); ++i) {
			auto r = resources.ensureRtAttachment(std::string{kGBufferAttachments[i].name}, gbExt,
				resolveRgFormat(std::string{kGBufferAttachments[i].format}, vk::Format::eUndefined));
			if (!r)
				return std::unexpected(std::move(r).error());
			gb[i] = *r;
		}
		// GB：SRO（上帧遗留）→ CAO
		std::array<vk::ImageMemoryBarrier2, 4> gbToDraw{};
		for (uint32_t i = 0; i < 4; ++i)
			gbToDraw[i] = RGL::createImageBarrier(*gb[i]->image, vk::ImageLayout::eShaderReadOnlyOptimal,
				vk::ImageLayout::eColorAttachmentOptimal, vk::ImageAspectFlagBits::eColor,
				vk::PipelineStageFlagBits2::eFragmentShader, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
				vk::AccessFlagBits2::eShaderRead, vk::AccessFlagBits2::eColorAttachmentWrite);
		vk::DependencyInfo gbDrawDep{};
		gbDrawDep.setImageMemoryBarriers(gbToDraw);
		cmd.pipelineBarrier2(gbDrawDep);
		std::array<vk::RenderingAttachmentInfo, 4> gbAttach{};
		for (uint32_t i = 0; i < 4; ++i) {
			gbAttach[i].imageView = *gb[i]->view;
			gbAttach[i].imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
			gbAttach[i].loadOp = vk::AttachmentLoadOp::eClear;
			gbAttach[i].storeOp = vk::AttachmentStoreOp::eStore;
			gbAttach[i].clearValue = vk::ClearValue{std::array<float, 4>{0, 0, 0, 0}};
		}
		vk::RenderingInfo geoInfo{};
		geoInfo.renderArea = vk::Rect2D({0, 0}, vk::Extent2D(w, h));
		geoInfo.layerCount = 1;
		geoInfo.setColorAttachments(gbAttach);
		geoInfo.setPDepthAttachment(&depthAttachment);
		cmd.beginRendering(geoInfo);
		RGL::applyDynamicState(cmd, w, h, renderer.getOpaqueState(), renderer.isDepthClipDynamic(), 4);
		for (uint32_t i = 0; i < list.skyboxStart; ++i)
			drawCall(list.calls[i]);
		cmd.endRendering();
		if (gbDumpCh < 0) {
			// GB：CAO → SRO（light 采样前）
			std::array<vk::ImageMemoryBarrier2, 4> gbToSample{};
			for (uint32_t i = 0; i < 4; ++i)
				gbToSample[i] = RGL::createImageBarrier(*gb[i]->image,
					vk::ImageLayout::eColorAttachmentOptimal,
					vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
					vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::PipelineStageFlagBits2::eFragmentShader,
					vk::AccessFlagBits2::eColorAttachmentWrite, vk::AccessFlagBits2::eShaderRead);
			vk::DependencyInfo gbSampleDep{};
			gbSampleDep.setImageMemoryBarriers(gbToSample);
			cmd.pipelineBarrier2(gbSampleDep);
		}
	}

	// G-buffer dump 模式：geometry 已写 GB，直接把第 ch 个 RT copyImageToBuffer 读回（绕开 light/捕获）
	if (gbDumpCh >= 0) {
		vk::ImageMemoryBarrier2 gbToRead = RGL::createImageBarrier(*gb[gbDumpCh]->image,
			vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageAspectFlagBits::eColor, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::AccessFlagBits2::eTransferRead);
		vk::DependencyInfo gbReadDep{};
		gbReadDep.setImageMemoryBarriers(gbToRead);
		cmd.pipelineBarrier2(gbReadDep);
		VkBufferImageCopy gbCopy{};
		gbCopy.bufferOffset = 0;
		gbCopy.bufferRowLength = 0;
		gbCopy.bufferImageHeight = h;
		gbCopy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		gbCopy.imageOffset = {0, 0, 0};
		gbCopy.imageExtent = {w, h, 1};
		::vkCmdCopyImageToBuffer(*cmd, *gb[gbDumpCh]->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*readback, 1, &gbCopy);
		cmd.end();
	}
	else {
	cmd.beginRendering(renderingInfo);

	// ⑧ 绘制（同 drawFrame ⑨ 契约：opaque/skybox/transparent 三段各段首 applyDynamicState
	// 一次——离屏 pass 与上屏 pass 状态同构，天空盒/透明排序在截图路径同样生效）
	if (renderer.isDeferred()) {
		// light pass：全屏采样刚在本命令缓冲内填好的 GB，写单捕获 color RT（前向/延迟同 RT 可比）
		RGL::applyDynamicState(cmd, w, h, renderer.getLightState(), renderer.isDepthClipDynamic());
		const auto& lightEntry = renderer.pipelineEntryAt(renderer.lightPipelineIndex());
		lightEntry.pipeline.bindShaders(cmd);
		cmd.drawMeshTasksEXT(1, 1, 1);
	}
	else {
		RGL::applyDynamicState(cmd, w, h, renderer.getOpaqueState(), renderer.isDepthClipDynamic());
		for (uint32_t i = 0; i < list.skyboxStart; ++i)
			drawCall(list.calls[i]);
		if (list.skyboxStart < list.transparentStart) {
			RGL::applyDynamicState(cmd, w, h, renderer.getSkyboxState(), renderer.isDepthClipDynamic());
			for (uint32_t i = list.skyboxStart; i < list.transparentStart; ++i)
				drawCall(list.calls[i]);
		}
		if (list.transparentStart < list.calls.size()) {
			RGL::applyDynamicState(cmd, w, h, renderer.getTransparentState(), renderer.isDepthClipDynamic());
			for (uint32_t i = list.transparentStart; i < static_cast<uint32_t>(list.calls.size()); ++i)
				drawCall(list.calls[i]);
		}
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
	}

	// ⑫ 提交 + 等待（独立 fence；裸 C 提交同 drawFrame ⑬ 风格）
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	VkCommandBuffer cb = *cmd;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cb;
	VkFence rawFence = *fence;
	// VkResult 直接用（vkerror::fromVkResult 契约；vkResultName 供日志，免 vulkan.hpp 的 to_string）
	if (auto result = ::vkQueueSubmit(queue, 1, &submitInfo, rawFence);
		result != VK_SUCCESS) {
		if (logger)
			logger->error("llm_debug: capture submit failed: {}", vkResultName(result));
		return std::unexpected(Error::fromVkResult(result, "llm_debug capture submit"));
	}
	(void)device.waitForFences(*fence, true, std::numeric_limits<uint64_t>::max());
	device.resetFences(*fence);

	// ⑬ 回读 + 写 PNG（回读缓冲 VMA 常驻映射，CPU 指针直接用）
	const uint8_t* pixels = static_cast<const uint8_t*>(readbackAllocData.pMappedData);
	std::filesystem::path relPath;
	if (gbDumpCh >= 0) {
		relPath = fmt::format("screenshots/gbuffer_{}.png", gbDumpCh);
	} else if (cfg.file.empty()) {
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
