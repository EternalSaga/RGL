module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（vkQueueSubmit 裸调用 + SUBMIT_INFO 结构体）
#include <ranges>           // std::views（AGENTS 例外：namespace alias 不可 re-export）
#include <fmt/format.h>
module frame_session:impl;

import frame_session;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_swapchain;
import vk_resources;
import profiler;  // Phase 9 帧相 zone（fenceWait/acquire/submit/present，docs/phase9_plan.md）

namespace RGL {

// FrameSession 函数体（2026-09-10 Phase 10 批次 0 手术 4：逐行迁自 vkrenderer_impl
// createSyncObjects / allocateCommandBuffers / recreateSwapchainAndDepth / drawFrame
// ①②③ / ⑬⑭⑮；graphicsQueue/graphicsCommandPool 改经 device 访问器取，语义零改动）。

FrameSession::FrameSession(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain,
		RGL::Resources& vkResources, bool noPresent_)
	: device(vkDevice), swapchain(&vkSwapchain), resources(&vkResources), noPresent(noPresent_) {
	createSyncObjects();
	allocateCommandBuffers();
}

void FrameSession::createSyncObjects() {
	vk::SemaphoreCreateInfo semaphoreInfo{};
	// fence 初始为 signaled：首帧的 waitForFences 不会阻塞
	vk::FenceCreateInfo fenceInfo{vk::FenceCreateFlagBits::eSignaled};
	vk::FenceCreateInfo presentFenceInfo{vk::FenceCreateFlagBits::eSignaled};

	imageAvailableSemaphores.reserve(kFramesInFlight);
	renderFinishedSemaphores.reserve(kFramesInFlight);
	inFlightFences.reserve(kFramesInFlight);
	presentFences.reserve(kFramesInFlight);

	std::ranges::for_each(std::views::iota(0u, kFramesInFlight),
		[&](uint32_t) {
			imageAvailableSemaphores.emplace_back(device.getDevice(), semaphoreInfo);
			renderFinishedSemaphores.emplace_back(device.getDevice(), semaphoreInfo);
			inFlightFences.emplace_back(device.getDevice(), fenceInfo);
			presentFences.emplace_back(device.getDevice(), presentFenceInfo);
		});
}

void FrameSession::allocateCommandBuffers() {
	vk::CommandBufferAllocateInfo allocInfoCmd{
		*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary,
		kFramesInFlight};
	commandBuffers = device.getDevice().allocateCommandBuffers(allocInfoCmd);
}

std::expected<void, Error> FrameSession::recreateSwapchainAndDepth(const char* context) {
	// recreate 内部重查 caps/format/presentMode（acquire OUT_OF_DATE 的根因未必是
	// extent，如 HDR 状态切换）。全部未变 = 重建修不好，打破静默死循环：报一次错
	if (!swapchain->recreate()) {
		static bool warned = false;
		auto logger = RLLogger::getInstance();
		if (!warned) {
			warned = true;
			if (logger) logger->error("Renderer: recreate requested ({}) but surface "
				"capabilities unchanged — cannot recover, expect repeated failure", context);
		}
		return {};  // extent 未变 → 深度图仍有效，跳过重建
	}
	auto result = resources->recreateDepthImage(swapchain->getExtent());
	if (!result) {
		auto logger = RLLogger::getInstance();
		if (logger) logger->error("Renderer: failed to recreate depth image ({}): {}",
			context, result.error().describe());
		return std::unexpected(std::move(result).error());
	}
	return {};
}

std::expected<FrameSession::FrameStart, Error> FrameSession::beginFrame() {
	const uint64_t frame = frameCount++;  // DIAG：device lost 帧号定位
	currentFrameNumber = frame;
	auto& dev = device.getDevice();

	// ① 等待上一帧同 slot 完全结束：inFlight fence（命令缓冲执行完）与 present
	// fence（present 完成）都信号后，本 slot 的 semaphore / command buffer 才可重用。
	// waitAll=VK_TRUE + 无限超时。正常时 fence 保证在超时内信号；device reset/lost 时
	// fence 永不信号 → 返回 eErrorDeviceLost/Reset（非无限自旋，报错退出帧循环）
	// DIAG（diagnostics.lua no_present）：不 present → presentFence 永不 signal，只等 inFlight
	// Phase 9 拦截点 3（phase9_plan §2.3）：本段 = CPU 等 GPU（帧相里最长的嫌疑段）
	{
		auto z = zone("fenceWait");
		const vk::Result waitResult = noPresent
			? dev.waitForFences(
				{*inFlightFences[currentFrame]}, VK_TRUE, std::numeric_limits<uint64_t>::max())
			: dev.waitForFences(
				{*inFlightFences[currentFrame], *presentFences[currentFrame]},
				VK_TRUE, std::numeric_limits<uint64_t>::max());
		if (waitResult != vk::Result::eSuccess) {
			auto logger = RLLogger::getInstance();
			if (logger) logger->error("Renderer: fence wait failed at frame start ({}): {}",
				frame, vk::to_string(waitResult));
			return std::unexpected(Error{ErrorCode::DeviceLost,
				fmt::format("frame fence wait failed: {}", vk::to_string(waitResult))});
		}
		if (noPresent) {
			dev.resetFences({*inFlightFences[currentFrame]});
		} else {
			dev.resetFences({*inFlightFences[currentFrame], *presentFences[currentFrame]});
		}
	}

	// ② 从 swapchain 取一张后缓冲：成功时驱动信号 imageAvailableSemaphores
	//（queueSubmit 的等待对象），imageIndex 为取到的图像下标。
	// 100ms 超时（非无限）：compositor 停发 present（如 Wayland 息屏）时限时返回
	// eTimeout，跳过本帧保住帧循环存活（llm_debug 离屏截图与事件轮询不依赖本帧
	// 上屏）；正常 vsync 节奏下 acquire 在超时前返回，行为不变。
	// Phase 9：acquire zone 含 ③ CB reset+begin（录制准备，量级同段）
	{
		auto z = zone("acquire");
		auto [acquireResult, imageIndex] = swapchain->getSwapchain().acquireNextImage(
			100'000'000,
			*imageAvailableSemaphores[currentFrame], nullptr);

		// eTimeout：无可用后缓冲——本帧不渲染，事件/system 下一轮照常
		if (acquireResult == vk::Result::eTimeout)
			return FrameStart::skipped;

		// eErrorOutOfDateKHR：swapchain 已失效（如 surface 尺寸变化），必须重建
		if (acquireResult == vk::Result::eErrorOutOfDateKHR) {
			if (auto result = recreateSwapchainAndDepth("acquire out-of-date"); !result)
				return std::unexpected(std::move(result).error());
			return FrameStart::recreated;
		}
		// eSuboptimalKHR：图像仍可用，只是非最优（如尺寸不匹配），可继续渲染
		if (acquireResult != vk::Result::eSuccess &&
			acquireResult != vk::Result::eSuboptimalKHR) {
			return std::unexpected(Error{ErrorCode::SwapchainAcquireFailed,
				"Failed to acquire swapchain image!"});
		}
		acquiredImageIndex = imageIndex;

		// ③ 开始录制：reset() 清除上一次录制状态（command buffer 每帧重用，
		// 重新录制前必须 reset）
		commandBuffers[currentFrame].reset();
		// 默认 begin flags（非 one-time、非 secondary）：可多次提交
		commandBuffers[currentFrame].begin(
			vk::CommandBufferBeginInfo{});
	}
	return FrameStart::began;
}

std::expected<void, Error> FrameSession::submit(vk::PipelineStageFlags2 waitStage) {
	auto z = zone("submit");  // Phase 9：CPU 提交成本（应为 µs 级；异常大 = 驱动同步嫌疑）
	// ⑬ 提交到图形队列。用裸 C API（vkQueueSubmit）而非 vk-hpp RAII：
	// RAII 的 Queue::submit 重载不支持 fence 参数。
	// wait：imageAvailable 信号量（acquireNextImage 信号）——wait stage = 图派生的
	// swapchainWaitStage（命令流首次触碰后缓冲的阶段：直渲图 = ColorAttachmentOutput、
	// blit 上屏图 = Transfer；现值均在 stage1 32 位范围内，截断安全）。
	// signal：renderFinished 信号量（presentKHR 的等待对象）；
	// fence：inFlightFence，命令缓冲执行完时信号。
	VkSemaphore waitSem = *imageAvailableSemaphores[currentFrame];
	const VkPipelineStageFlags stage1 = static_cast<VkPipelineStageFlags>(
		static_cast<VkPipelineStageFlags2>(waitStage));
	VkCommandBuffer cb = *commandBuffers[currentFrame];
	VkSemaphore signalSem = *renderFinishedSemaphores[currentFrame];
	VkFence rawFence = *inFlightFences[currentFrame];

	VkSubmitInfo rawSubmitInfo{};
	rawSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	rawSubmitInfo.waitSemaphoreCount = 1;
	rawSubmitInfo.pWaitSemaphores = &waitSem;
	rawSubmitInfo.pWaitDstStageMask = &stage1;
	rawSubmitInfo.commandBufferCount = 1;
	rawSubmitInfo.pCommandBuffers = &cb;
	rawSubmitInfo.signalSemaphoreCount = 1;
	rawSubmitInfo.pSignalSemaphores = &signalSem;

	VkResult submitVkResult = ::vkQueueSubmit(device.getGraphicsQueue(), 1, &rawSubmitInfo, rawFence);
	// DEVICE_LOST：GPU 挂起/复位，不可恢复——记录后退出帧循环
	if (submitVkResult == VK_ERROR_DEVICE_LOST) {
		auto logger = RLLogger::getInstance();
		if (logger) logger->error("Renderer: device lost on submit! (frame {})", currentFrameNumber);
		return std::unexpected(Error{ErrorCode::DeviceLost, "VK_ERROR_DEVICE_LOST on submit"});
	}
	// 其余失败（OOM/device reset 等）：fence 可能永不信号 → 下帧 ① 会报错退出，
	// 此处先留观测（旧实现静默吞掉，排障零线索）
	if (submitVkResult != VK_SUCCESS) {
		auto logger = RLLogger::getInstance();
		if (logger) logger->error("Renderer: submit failed (frame {}): {}", currentFrameNumber,
			vk::to_string(vk::Result{submitVkResult}));
	}
	return {};
}

std::expected<void, Error> FrameSession::present() {
	auto z = zone("present");  // Phase 9：present 提交 CPU 成本（合成器等待不在内——那是 9.3）
	// ⑭ 上屏：等 renderFinished 信号量（确保绘制完成），把 imageIndex 交给合成器。
	// SwapchainPresentFenceInfoEXT（KHR/EXT 为 typedef 别名，sType 相同）链到
	// presentInfo.pNext：present 完成时信号 presentFence，实现 present 端完成
	// 追踪（VK_KHR_swapchain_maintenance1，见 docs/技术概念.md 的"PresentFenceInfoKHR"）
	// DIAG（diagnostics.lua no_present）：跳 present——buffer 不出进程，KWin 不 import dmabuf
	if (noPresent)
		return {};
	vk::SwapchainPresentFenceInfoEXT presentFenceInfoEXT{};
	presentFenceInfoEXT.setFences(*presentFences[currentFrame]);
	vk::PresentInfoKHR presentInfo{};
	presentInfo.setPNext(&presentFenceInfoEXT);
	presentInfo.setWaitSemaphores(*renderFinishedSemaphores[currentFrame]);
	presentInfo.setSwapchains(*swapchain->getSwapchain());
	presentInfo.setImageIndices(acquiredImageIndex);
	auto presentResult = device.getGraphicsQueue().presentKHR(presentInfo);

	if (presentResult == vk::Result::eErrorDeviceLost) {
		auto logger = RLLogger::getInstance();
		if (logger) logger->error("Renderer: device lost on present!");
		return std::unexpected(Error{ErrorCode::DeviceLost, "VK_ERROR_DEVICE_LOST on present"});
	}
	// present 时 swapchain 失效：重建后下一帧继续
	if (presentResult == vk::Result::eErrorOutOfDateKHR ||
		presentResult == vk::Result::eSuboptimalKHR) {
		if (auto result = recreateSwapchainAndDepth("present out-of-date/suboptimal"); !result) {
			return std::unexpected(std::move(result).error());
		}
	}
	// 其余错误码（如 SURFACE_LOST）：spec 语义下队列操作仍入队、present fence 仍
	// 信号（不死锁），但必须留观测——下帧 acquire 大概率持续失败
	if (presentResult != vk::Result::eSuccess &&
		presentResult != vk::Result::eErrorDeviceLost &&
		presentResult != vk::Result::eErrorOutOfDateKHR &&
		presentResult != vk::Result::eSuboptimalKHR) {
		auto logger = RLLogger::getInstance();
		if (logger) logger->error("Renderer: present failed (frame {}): {}", currentFrameNumber,
			vk::to_string(presentResult));
	}
	return {};
}

void FrameSession::advance() {
	// ⑮ 推进到下一个帧 slot（双缓冲）
	currentFrame = (currentFrame + 1) % kFramesInFlight;
}

}  // namespace RGL
