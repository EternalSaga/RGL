module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <fmt/format.h>
module frame_capture:impl;

import frame_capture;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_resources;
import render_target_cache;  // RenderTargetCache（架构债务续账步骤 2）
import render_graph;
import VmaRaii;
import vulkan_utils;
import profiler;  // 批 9.5：drawFrame.capture / drawFrame.collect 子段 zone

namespace RGL {

// FrameCapturer 函数体（2026-09-10 Phase 10 批次 0 手术 2：随类自 vkrenderer 摘出，
// 逐步骤注记迁自 vkrenderer_impl ⑪.5 / 收口块）。

namespace {

/**
 * 捕获回读每像素字节数（仅可能出现的格式：swapchain 候选 4B/像素系 + A2B10G10R10=4B +
 * HDR FP16=8B + G-buffer 词表 rgba32_sfloat=16B + object_id 的 r32_uint=4B +
 * ao 附件的 r8_unorm=1B，其余 8bit×4/rg16×2 = 4B）。
 *
 * ⚠ 漏 1B 分支不会崩、也不会报错——只会把 staging/`bytes` 按 4B 算多（拷贝本身按
 * 格式的 texel 大小写紧，前 w×h 字节仍是对的），于是 `pixels` 尾部 3/4 是上一次捕获的
 * 残留。今天没人读那段，但"bytes 撒谎"的诊断面迟早被下一个判据当真值读，故补全。
 * @param f 捕获格式（swapchain format 或 G-buffer / 具名附件 RT format）
 * @return 每像素字节数（1 / 4 / 8 / 16）
 */
uint32_t captureBpp(vk::Format f) {
	switch (f) {
		case vk::Format::eR32G32B32A32Sfloat: return 16;
		case vk::Format::eR16G16B16A16Sfloat: return 8;
		case vk::Format::eR8Unorm: return 1;  // ao 附件（批 11.4）：单通道 8bit 数据
		default: return 4;
	}
}

}  // namespace

FrameCapturer::FrameCapturer(RGL::Resources& vkResources, RGL::RenderTargetCache& vkRts,
		const RGL::CompiledRenderGraph& renderGraph)
	: resources(vkResources), rts(vkRts), graph(renderGraph) {
	// 帧内捕获：定位 swapchain attachment slot（捕获时读写其布局追踪值）。
	// 债-129 S1a：按 kind 现查收进图事实查询面（本文件另一处按名查见 record 内）。
	swapchainSlot = graphAttachmentSlotByKind(graph.attachments, RgAttachmentKind::swapchain)
		.value_or(kRgNoAttachment);
}

bool FrameCapturer::ensureCaptureStaging(VkDeviceSize bytes) {
	if (captureStagingSize >= bytes)
		return true;
	vk::BufferCreateInfo info{};
	info.size = bytes;
	info.usage = vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
		VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT;
	VmaAllocationInfo allocData{};
	auto result = resources.getAllocator().createBuffer(info, alloc, &allocData, "frame_capture/staging");
	if (!result) {
		auto logger = RLLogger::getInstance();
		if (logger)
			logger->error("Renderer: capture staging alloc failed: {}", result.error().describe());
		return false;
	}
	captureStaging = std::move(*result);
	captureMapped = allocData.pMappedData;
	captureStagingSize = bytes;
	return true;
}

void FrameCapturer::record(vk::raii::CommandBuffer& cmd, VkImage swapchainImage,
		vk::Extent2D swapchainExtent, vk::Format swapchainFormat,
		std::vector<vk::ImageLayout>& layouts) {
	// ⑪.5 帧内捕获（llm_debug 回读，frame_capture 契约）：请求帧在 presentTransition
	// 前插回读拷贝（布局推进语义见接口 record 注释）。
	// acquire 超时跳帧的 return 在本段之前 = 请求保持 pending 至真正录制的帧。
	auto z = zone("drawFrame.capture");  // 批 9.5 观测：无请求帧 = 首行早退，≈ 0
	if (captureRequest == -2 && captureRequestName.empty())
		return;
	auto barrier = [&](VkImage image, vk::ImageLayout from, vk::ImageLayout to,
		vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
		vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
		vk::ImageMemoryBarrier2 b = RGL::createImageBarrier(image, from, to,
			vk::ImageAspectFlagBits::eColor, srcStage, dstStage, srcAccess, dstAccess);
		vk::DependencyInfo dep{};
		dep.setImageMemoryBarriers(b);
		cmd.pipelineBarrier2(dep);
	};
	auto copyOut = [&](VkImage image, uint32_t w, uint32_t h, vk::Format format, int channel,
						  std::string attachmentName) {
		const uint32_t bpp = captureBpp(format);
		const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * bpp;
		if (!ensureCaptureStaging(bytes))
			return false;
		VkBufferImageCopy region{};
		region.bufferRowLength = w;
		region.bufferImageHeight = h;
		region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		region.imageExtent = {w, h, 1};
		::vkCmdCopyImageToBuffer(*cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			*captureStaging, 1, &region);
		captureInFlight = FrameCapture{.width = w, .height = h, .format = format,
			.gbufChannel = channel, .attachmentName = attachmentName, .bytes = bytes, .pixels = {}};
		captureArmed = true;
		return true;
	};
	// 失败收口（2026-10-19）：原因字符串**同时**是日志文本与回给消费方的原因——一个意思写
	// 两遍迟早漂（旧代码正是两处各写一份，结果两处都没兑现"warn 一次"）。清具名请求 = 让本
	// 请求退出"每帧重演"；失败值由 hasFailure()/takeFailure() 取走，一条失败只兑现一个请求。
	auto failWith = [&](std::string why) {
		captureRequestName.clear();
		if (auto logger = RLLogger::getInstance())
			logger->warn("frame capture: {}", why);
		captureFailure = std::move(why);
	};
	// GB 直读分支（captureRequest = gbuf 通道号）：deferred 图专属。**未命中必须置失败通道**
	// （2026-10-19 补）——本分支的 `captureRequest` 在 record 末尾被无条件复位，不会像具名
	// 分支那样每帧重演，但"没有失败原因"同样致命：消费方（RPC `/shot` 的 gbuf 路径）只会一直
	// 等产物、等到超时，且 `pendingShot` 永不复位 = 端点被一次越界通道号打死到进程重启。
	// 不做限次重试的理由：`dump_gbuf` 的既有语义是"每帧重新 request"（ecs_frame_diag），重试
	// 由它自己负责；本类只负责把"这次为什么没产物"说清楚。
	auto recordGBuffer = [&] {
		if (captureRequest >= static_cast<int>(kGBufferAttachments.size()))
			return failWith(fmt::format("gbuf channel {} out of range [0, {})", captureRequest,
				kGBufferAttachments.size()));
		const std::string name{kGBufferAttachments[captureRequest].name};
		const auto slot = graphAttachmentSlot(graph.attachments, name);
		if (!slot.has_value())
			return failWith(fmt::format(
				"gbuf channel {} ('{}') is not in the compiled render graph (非 deferred 图？)",
				captureRequest, name));
		auto* rt = rts.find(name);
		if (rt == nullptr)
			return failWith(fmt::format(
				"gbuf channel {} ('{}') is in the graph but its RT is not created",
				captureRequest, name));
		const auto from = layouts[*slot];  // deferred 帧末 = SRO（light 采样后）
		barrier(rt->image.image, from, vk::ImageLayout::eTransferSrcOptimal,
			vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
		if (copyOut(rt->image.image, rt->extent.width, rt->extent.height,
		        rt->format, captureRequest, {}))
			barrier(rt->image.image, vk::ImageLayout::eTransferSrcOptimal, from,
				vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
				vk::PipelineStageFlagBits2::eFragmentShader, vk::AccessFlagBits2::eShaderRead);
	};
	// swapchain 回读分支（captureRequest = -1）：presentTransition 前回读后缓冲（present 前真实内容）
	auto recordSwapchain = [&] {
		const auto from = layouts[swapchainSlot];  // 帧末：直渲 = CAO、blit 变体 = TransferDst
		const bool blitWritten = from == vk::ImageLayout::eTransferDstOptimal;
		barrier(swapchainImage, from, vk::ImageLayout::eTransferSrcOptimal,
			blitWritten ? vk::PipelineStageFlagBits2::eTransfer
			            : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			blitWritten ? vk::AccessFlagBits2::eTransferWrite
			            : vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
		if (copyOut(swapchainImage, swapchainExtent.width, swapchainExtent.height,
		        swapchainFormat, -1, {}))
			layouts[swapchainSlot] = vk::ImageLayout::eTransferSrcOptimal;
	};
	// 具名附件分支（captureRequestName 非空，批 12.2）：QA 层的 object_id 全分辨率 dump。
	// 与 G-buffer 分支的差别在**依赖覆盖**——G-buffer 附件帧末必被 light pass 采样过
	// （src = 着色器读），而 object_id 的最后写者是 picking pass 的**颜色附件写**且
	// 无人采样。通用具名路径面对的是"任意附件"，不能假定它一定被采样过，故取两个
	// stage/access 的并集（调试期一次性成本可忽略，换掉一整类 WAW 漏声明）。
	// 追踪表原值进出（转回 from）——本路径不接管布局，下游按追踪值继续推导。
	auto recordNamedAttachment = [&] {
		const std::string name = captureRequestName;
		const auto slot = graphAttachmentSlot(graph.attachments, name);
		// ⚠ 两种「未命中」分开处置（2026-10-19 修；此前两者同走"保持 pending"）：名字不在编译
		//   好的图里 = **永久**失败——图不会自己长出附件。留着 pending 得到的是：① 每帧一条
		//   warn（实测 46 秒刷 1505 条，而接口原本承诺"warn 一次"）② `pendingShot` 永不复位
		//   ⟹ 之后每个 `/shot` 都吃 409，**一次拼错把端点打死到进程重启** ③ 客户端只能等到
		//   超时（504），拿不到任何原因
		if (!slot.has_value())
			return failWith(fmt::format(
				"attachment '{}' is not in the compiled render graph (未装饰的图 / 门未开？)", name));
		auto* rt = rts.find(name);
		if (rt == nullptr) {
			// 图里有、RT 这帧还没建 = 暂时失败，可重试；但**限次**——RT 因装饰条件永不建时，
			// 无限重试与上面那个 bug 同形。分类逻辑抽成纯函数（`classifyCaptureMiss`），为的是
			// 它能在 `just test` 里被钉住，而不是只能开设备复现。
			++captureRtMisses;
			if (classifyCaptureMiss(true, captureRtMisses) == CaptureMiss::retryable)
				return;  // 保持 pending，下个帧边界重试
			return failWith(fmt::format(
				"attachment '{}' is in the graph but its RT was never created ({} 帧内)",
				name, captureRtMisses));
		}
		captureRtMisses = 0;
		const auto from = layouts[*slot];
		barrier(rt->image.image, from, vk::ImageLayout::eTransferSrcOptimal,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput
				| vk::PipelineStageFlagBits2::eFragmentShader,
			vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eShaderRead,
			vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
		if (copyOut(rt->image.image, rt->extent.width, rt->extent.height, rt->format, -1, name))
			barrier(rt->image.image, vk::ImageLayout::eTransferSrcOptimal, from,
				vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
				vk::PipelineStageFlagBits2::eColorAttachmentOutput
					| vk::PipelineStageFlagBits2::eFragmentShader,
				vk::AccessFlagBits2::eColorAttachmentWrite | vk::AccessFlagBits2::eShaderRead);
	};
	if (!captureRequestName.empty())
		recordNamedAttachment();
	else if (captureRequest >= 0)
		recordGBuffer();
	else if (swapchainSlot != kRgNoAttachment)
		recordSwapchain();
	captureRequest = -2;
}

void FrameCapturer::collect(vk::raii::Device& device, vk::Fence inFlightFence) {
	// 帧内捕获收口：同步等本帧命令（含回读拷贝）完成，把 mapped 字节搬进结果。
	// fence 不在此 reset（下帧 ① waitAll 对已信号 fence 立即通过 + 照常 reset，语义不变）；
	// 停顿只发生在捕获请求帧（llm 一次性截图），正常帧零影响。
	auto z = zone("drawFrame.collect");  // 批 9.5 观测：本帧 fence 等待 + mapped 搬运（请求帧才大）
	if (!captureArmed)
		return;
	(void)device.waitForFences(inFlightFence, VK_TRUE,
		std::numeric_limits<uint64_t>::max());
	if (captureMapped) {
		auto* src = static_cast<const uint8_t*>(captureMapped);
		captureInFlight.pixels.assign(src, src + captureInFlight.bytes);
		captureResult = std::move(captureInFlight);
	}
	captureArmed = false;
}

}  // namespace RGL
