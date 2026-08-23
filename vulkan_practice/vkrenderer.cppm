module;
#include <entt/entt.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <ranges>

export module vk_renderer;

import rlstd;
import rllogger;

import vulkan_utils;
import vulkan_window;
import vk_device;
import vk_swapchain;
import vk_resources;
import types_generated;
import shader;
import vkerror;

namespace RGL {

// Renderer：每帧"录制 → 提交 → 显示"的驱动器 + 每帧同步状态的持有者。
//
// 渲染流程（一次 drawFrame() 调用，逐步骤注释见函数体内 ①~⑮）：
//   等待上一帧同 slot 的双 fence（submit 与 present 均完成）并重置
//   → acquireNextImage 获取后缓冲（信号 imageAvailable 信号量）
//   → 录制命令缓冲：图像 barrier → 动态渲染附件
//     → descriptor heap barrier/绑定/push data → beginRendering
//     → 绑定 ShaderObject + 动态状态 → drawMeshTasksEXT → endRendering
//     → PresentSrc barrier
//   → queueSubmit（等 imageAvailable / 信号 renderFinished / 挂 inFlight fence）
//   → presentKHR（pNext 挂 SwapchainPresentFenceInfoEXT 信号 present fence）
//   → currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT
//
// 核心现代特性：Dynamic Rendering（无 RenderPass）+ ShaderObject（无 PipelineLayout，
// bindShadersEXT + 动态状态）+ DescriptorHeap（vkCmdBindResourceHeapEXT 取代 descriptor set，
// 使用规范见 docs/DESCRIPTOR_HEAP_SPEC.md）。

export class Renderer;

export class Renderer {
   public:
	// 成员初始化列表顺序必须与下方声明顺序一致（RAII 规则：vk::raii::* 成员只能在
	// 初始化列表初始化，绝不在函数体内）。cubePipeline 依赖 Resources 的 descriptor heap
	//（descriptorSize + 顶点/索引 slot 号），故在 resources 之后构造。
	Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		entt::registry& registry)
		: vkDevice(vkDevice),
		  swapchain(&vkSwapchain),
		  resources(&vkResources),
		  registry(&registry),
		  graphicsQueue(vkDevice.getGraphicsQueue()),
		  graphicsCommandPool(vkDevice.getGraphicsCommandPool()),
		  cubePipeline(createRenderPipeline(vkDevice.getDevice(),
			  vkResources.getDescriptorHeap().getDescriptorSize(),
			  vkResources.getHeapSlotVertex(), vkResources.getHeapSlotIndex())) {
		// descriptor heap 的 GPU 端地址，每帧传给 vkCmdBindResourceHeapEXT
		heapAddress = vkResources.getDescriptorHeap().getGpuAddress();
		createSyncObjects();
		allocateCommandBuffers();
	}

	RGL::Swapchain& getSwapchainRef() const { return *swapchain; }
	RGL::Resources& getResourcesRef() const { return *resources; }
	// 更新每帧绘制参数（MVP 矩阵）；drawFrame() 内经 vkCmdPushDataEXT 上传为
	// [[vk::push_constant]]（CPU 侧副本，每帧重推）
	void updateDrawArgs(const glm::mat4& mvp) { drawArgs.mvp = mvp; }
	// 等待 GPU 空闲（resize / 重建 swapchain 前调用，确保旧资源不再被使用）
	void waitForIdle() { vkDevice.getDevice().waitIdle(); }
	// 渲染一帧：整体流程见类头注释；失败（device lost 等）时返回错误
	[[nodiscard]] std::expected<void, Error> drawFrame();

   private:
	// 构建渲染管线（ShaderObject 模式，无 PipelineLayout）：
	// ① 加载构建期由 slangc 编译好的 mesh/fragment SPV
	// ② 交给 RenderPipeline 构造器，其内部创建两个 VkShaderEXT 对象，并通过
	//    VkShaderCreateInfoEXT.pNext 链上 VkDescriptorSetAndBindingMappingEXT：
	//    binding 0 → 顶点缓冲 slot、binding 1 → 索引缓冲 slot（HEAP_WITH_CONSTANT_OFFSET，
	//    heapOffset = slot * descriptorSize，单位是字节）。细节见 shader.cppm。
	static RGL::RenderPipeline createRenderPipeline(vk::raii::Device& device,
		uint32_t descriptorSize,
		uint32_t slotVertex, uint32_t slotIndex) {
		auto meshResult = RGL::loadShaderSpv("generated_shaders/cube.mesh.spv");
		if (!meshResult) throw std::runtime_error(meshResult.error().describe());
		auto fragResult = RGL::loadShaderSpv("generated_shaders/cube.frag.spv");
		if (!fragResult) throw std::runtime_error(fragResult.error().describe());

		RGL::ShaderEffect cubeEffect;
		cubeEffect.meshStageSpv = std::move(*meshResult);
		cubeEffect.fragStageSpv = std::move(*fragResult);
		return RGL::RenderPipeline(device, cubeEffect, descriptorSize, slotVertex, slotIndex);
	}

	// 创建 MAX_FRAMES_IN_FLIGHT 套同步对象（每个帧 slot 一套，双缓冲）。
	//
	// 四个角色：
	//   imageAvailableSemaphores — acquireNextImage 信号，queueSubmit 等待
	//                              （通知 GPU 后缓冲已可绘制）
	//   renderFinishedSemaphores — queueSubmit 信号，presentKHR 等待
	//                              （通知合成器本帧绘制完成）
	//   inFlightFences — 挂 queueSubmit，追踪"命令缓冲执行完"
	//   presentFences — 经 SwapchainPresentFenceInfoEXT 挂 presentInfo.pNext，
	//                   追踪"present 完成"（VK_KHR_swapchain_maintenance1）
	//
	// 为什么是双 fence：单 fence 只挂 queueSubmit 时，triple-buffer（KWin）下 fence
	// 信号时 present 可能仍在异步等待，下一帧重用 semaphore 会违反 VUID-00067。
	// 见 docs/技术概念.md 的"双 Fence"一节。
	void createSyncObjects() {
		vk::SemaphoreCreateInfo semaphoreInfo{};
		// fence 初始为 signaled：首帧的 waitForFences 不会阻塞
		vk::FenceCreateInfo fenceInfo{vk::FenceCreateFlagBits::eSignaled};
		vk::FenceCreateInfo presentFenceInfo{vk::FenceCreateFlagBits::eSignaled};

		imageAvailableSemaphores.reserve(MAX_FRAMES_IN_FLIGHT);
		renderFinishedSemaphores.reserve(MAX_FRAMES_IN_FLIGHT);
		inFlightFences.reserve(MAX_FRAMES_IN_FLIGHT);
		presentFences.reserve(MAX_FRAMES_IN_FLIGHT);

		std::ranges::for_each(std::views::iota(0, MAX_FRAMES_IN_FLIGHT),
			[&](int) {
				imageAvailableSemaphores.emplace_back(vkDevice.getDevice(), semaphoreInfo);
				renderFinishedSemaphores.emplace_back(vkDevice.getDevice(), semaphoreInfo);
				inFlightFences.emplace_back(vkDevice.getDevice(), fenceInfo);
				presentFences.emplace_back(vkDevice.getDevice(), presentFenceInfo);
			});
	}

	// 批量分配 MAX_FRAMES_IN_FLIGHT 个 primary command buffer（每帧 slot 一个，
	// 每帧 reset() 后重新录制）
	void allocateCommandBuffers() {
		vk::CommandBufferAllocateInfo allocInfoCmd{
			*graphicsCommandPool, vk::CommandBufferLevel::ePrimary,
			static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
		commandBuffers = vkDevice.getDevice().allocateCommandBuffers(allocInfoCmd);
	}

	// 窗口尺寸变化（resize）或 acquire/present 返回 out-of-date 时重建 swapchain + 深度图。
	// 深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。
	[[nodiscard]] std::expected<void, Error> recreateSwapchainAndDepth(const char* context) {
		swapchain->recreate();
		auto result = resources->recreateDepthImage(swapchain->getExtent());
		if (!result) {
			auto logger = RLLogger::getInstance();
			if (logger) logger->error("Renderer: failed to recreate depth image ({}): {}",
				context, result.error().describe());
			return std::unexpected(std::move(result).error());
		}
		return {};
	}

	RGL::DeviceContext& vkDevice;
	RGL::Swapchain* swapchain;
	RGL::Resources* resources;
	entt::registry* registry;
	// 非 RAII 句柄（队列生命周期由 DeviceContext 的 raii Device 管理）
	vk::Queue graphicsQueue;
	const vk::raii::CommandPool& graphicsCommandPool;
	// ShaderObject 管线：无 RenderPass/PipelineLayout，drawFrame() 每帧调用
	// recordRenderCommands() 绑定着色器 + 动态状态
	RGL::RenderPipeline cubePipeline;
	// CPU 侧绘制参数（64B，types_generated 中 static_assert 与 shader 反射一致）；
	// Slang 端经 [[vk::push_constant]] 访问，布局必须逐字段匹配
	RGL::DrawArgs drawArgs{};
	// descriptor heap 的 GPU 地址（VmaDescriptorHeap 管理的"描述符槽位数组"，
	// 对齐细节见 VmaBuffer.cppm）
	vk::DeviceAddress heapAddress = 0;

	// 双缓冲：最多 2 帧同时在飞——当前帧在录制 + 上一帧还在 GPU 执行。
	// 下方同步对象数组按 currentFrame 索引，fence 信号后才可重用（双 fence 设计
	// 见 createSyncObjects 注释）
	static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
	uint32_t currentFrame = 0;
	std::vector<vk::raii::Semaphore> imageAvailableSemaphores;
	std::vector<vk::raii::Semaphore> renderFinishedSemaphores;
	std::vector<vk::raii::Fence> inFlightFences;
	std::vector<vk::raii::Fence> presentFences;
	std::vector<vk::raii::CommandBuffer> commandBuffers;
};

std::expected<void, Error> Renderer::drawFrame() {
	auto& device = vkDevice.getDevice();

	// ① 等待上一帧同 slot 完全结束：inFlight fence（命令缓冲执行完）与 present
	// fence（present 完成）都信号后，本 slot 的 semaphore / command buffer 才可重用。
	// waitAll=VK_TRUE + 无限超时（fence 保证在超时内信号，返回值忽略）
	(void)device.waitForFences(
		{*inFlightFences[currentFrame], *presentFences[currentFrame]},
		VK_TRUE, std::numeric_limits<uint64_t>::max());
	// 重置为 unsignaled，本帧 queueSubmit / presentKHR 会再次信号它们
	device.resetFences({*inFlightFences[currentFrame], *presentFences[currentFrame]});

	// ② 从 swapchain 取一张后缓冲：成功时驱动信号 imageAvailableSemaphores
	//（queueSubmit 的等待对象），imageIndex 为取到的图像下标
	auto [acquireResult, imageIndex] = swapchain->getSwapchain().acquireNextImage(
		std::numeric_limits<uint64_t>::max(),
		*imageAvailableSemaphores[currentFrame], nullptr);

	// eErrorOutOfDateKHR：swapchain 已失效（如 surface 尺寸变化），必须重建
	if (acquireResult == vk::Result::eErrorOutOfDateKHR) {
		return recreateSwapchainAndDepth("acquire out-of-date");
	}
	// eSuboptimalKHR：图像仍可用，只是非最优（如尺寸不匹配），可继续渲染
	if (acquireResult != vk::Result::eSuccess &&
		acquireResult != vk::Result::eSuboptimalKHR) {
		return std::unexpected(Error{ErrorCode::SwapchainAcquireFailed,
			"Failed to acquire swapchain image!"});
	}

	// ③ 开始录制：reset() 清除上一次录制状态（command buffer 每帧重用，
	// 重新录制前必须 reset）
	commandBuffers[currentFrame].reset();
	// 默认 begin flags（非 one-time、非 secondary）：可多次提交
	commandBuffers[currentFrame].begin(
		vk::CommandBufferBeginInfo{});

	auto& swapchainImages = swapchain->getImages();
	auto& swapchainImageViews = swapchain->getImageViews();

	// ④ 图像 barrier（Synchronization2 风格：pipelineBarrier2 + DependencyInfo，
	// 旧 vkCmdPipelineBarrier 已弃用）
	//
	// 后缓冲：Undefined → ColorAttachmentOptimal。
	// oldLayout=eUndefined 是 spec 中唯一正确的写法：首帧时图像刚创建（Undefined），
	// 第 2 帧起是 present 返回后的布局（PresentSrcKHR），eUndefined 同时覆盖两种
	// 状态、无需分支判断。见 docs/技术概念.md 的"colorToDrawBarrier 的 oldLayout"。
	vk::ImageMemoryBarrier2 colorToDrawBarrier = RGL::createImageBarrier(
		swapchainImages[imageIndex], vk::ImageLayout::eUndefined,
		vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eColorAttachmentWrite);

	// 深度图：Undefined → DepthStencilAttachmentOptimal。
	// dstAccess 需要 read（深度测试读旧值）+ write（更新深度缓冲）两者
	vk::ImageMemoryBarrier2 depthToDrawBarrier = RGL::createImageBarrier(
		*resources->getDepthImage(), vk::ImageLayout::eUndefined,
		vk::ImageLayout::eDepthStencilAttachmentOptimal,
		vk::ImageAspectFlagBits::eDepth,
		vk::PipelineStageFlagBits2::eTopOfPipe,
		vk::PipelineStageFlagBits2::eEarlyFragmentTests,
		vk::AccessFlagBits2::eNone,
		vk::AccessFlagBits2::eDepthStencilAttachmentRead |
			vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

	std::array<vk::ImageMemoryBarrier2, 2> drawBarriers = {colorToDrawBarrier, depthToDrawBarrier};
	vk::DependencyInfo drawDepInfo;
	drawDepInfo.setImageMemoryBarriers(drawBarriers);
	commandBuffers[currentFrame].pipelineBarrier2(drawDepInfo);

	// ⑤ 动态渲染附件（Dynamic Rendering：不创建 RenderPass 对象，
	// beginRendering 时直接描述附件）
	// 后缓冲：绘制前清深灰色 (0.1) 作为背景；Store 写回结果（后缓冲要上屏，
	// 不能 DontCare）
	vk::RenderingAttachmentInfo colorAttachment{};
	colorAttachment.imageView = *swapchainImageViews[imageIndex];
	colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
	colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
	colorAttachment.clearValue = vk::ClearValue{
		std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f}};

	// 深度缓冲：清到 1.0（最远处）；帧末结果不再使用，DontCare 省开销
	vk::RenderingAttachmentInfo depthAttachment{};
	depthAttachment.imageView = *resources->getDepthImageView();
	depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
	depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
	depthAttachment.storeOp = vk::AttachmentStoreOp::eDontCare;
	depthAttachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

	// 渲染区域覆盖整张后缓冲
	vk::RenderingInfo renderingInfo{};
	renderingInfo.renderArea = vk::Rect2D({0, 0}, swapchain->getExtent());
	renderingInfo.layerCount = 1;
	renderingInfo.setColorAttachments(colorAttachment);
	renderingInfo.setPDepthAttachment(&depthAttachment);

	// ⑥ Descriptor heap 同步：描述符此前由 vkWriteResourceDescriptorsEXT 写进
	// CPU mapped 区域（vkresources.cppm），GPU 读取前需要 host write →
	// resource heap read 的内存 barrier。eHost stage 不能在 render pass 内部使用，
	// 故该 barrier 必须放在 beginRendering 之前（见 docs/DESCRIPTOR_HEAP_SPEC.md 坑 8）。
	vk::MemoryBarrier2 heapBarrier{};
	heapBarrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT;
	heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
	heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
	vk::DependencyInfo heapDepInfo{};
	heapDepInfo.setMemoryBarriers(heapBarrier);
	commandBuffers[currentFrame].pipelineBarrier2(heapDepInfo);

	// ⑦ 把 descriptor heap 绑定到命令缓冲（必须在 beginRendering 之前）：
	// 此后所有描述符引用走 heap，与传统 descriptor set 状态互斥（坑 9）。
	// heapRange.size 用 getHeapSize()（已扣除内部对齐偏移），而非
	// descriptorSize * capacity——原因见 VmaBuffer.cppm。
	// reservedRange = [0, reservedSlots * descriptorSize)：实现保留区域
	//（如 NVIDIA 保留 6048 槽），在此声明为不可用。
	vk::DeviceAddressRangeEXT heapRange{};
	heapRange.address = heapAddress;
	heapRange.size = resources->getDescriptorHeap().getHeapSize();
	commandBuffers[currentFrame].bindResourceHeapEXT(vk::BindHeapInfoEXT(
		heapRange,
		resources->getReservedRangeOffset(),
		resources->getReservedRangeSize()));

	// ⑧ 上传绘制参数（push constant，独立于 descriptor heap，不占 slot）：
	// Slang 端经 [[vk::push_constant]] 访问，无需 binding mapping（spec 3.4.1）
	commandBuffers[currentFrame].pushDataEXT(vk::PushDataInfoEXT(
		0, vk::HostAddressRangeConstEXT(&drawArgs, sizeof(RGL::DrawArgs))));

	// ⑨ 开始渲染（Dynamic Rendering 的 render pass 起点）
	commandBuffers[currentFrame].beginRendering(renderingInfo);

	// ⑩ 绑定 ShaderObject（未使用的 stage 传 VK_NULL_HANDLE）+ 设置全部动态状态
	//（viewport/scissor/光栅化/深度/多采样/混合，详见 shader.cppm）
	cubePipeline.recordRenderCommands(commandBuffers[currentFrame],
		swapchain->getExtent().width,
		swapchain->getExtent().height);
	// ⑪ 绘制 1 个 mesh task：mesh shader 内部生成顶点/三角形，
	// 不需要传统顶点管线（draw/drawIndexed）
	commandBuffers[currentFrame].drawMeshTasksEXT(1, 1, 1);
	commandBuffers[currentFrame].endRendering();

	// ⑫ 后缓冲：ColorAttachmentOptimal → PresentSrcKHR，使其"可 present"。
	// 该转换必须排在颜色附件的最后一次使用之后（此处紧随 endRendering）。
	vk::ImageMemoryBarrier2 colorToPresentBarrier = RGL::createImageBarrier(
		swapchainImages[imageIndex], vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::ePresentSrcKHR, vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::PipelineStageFlagBits2::eBottomOfPipe,
		vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::AccessFlagBits2::eNone);
	vk::DependencyInfo colorToPresentDepInfo;
	colorToPresentDepInfo.setImageMemoryBarriers(colorToPresentBarrier);
	commandBuffers[currentFrame].pipelineBarrier2(colorToPresentDepInfo);
	// 录制完毕
	commandBuffers[currentFrame].end();

	// ⑬ 提交到图形队列。用裸 C API（vkQueueSubmit）而非 vk-hpp RAII：
	// RAII 的 Queue::submit 重载不支持 fence 参数。
	// wait：imageAvailable 信号量（acquireNextImage 信号）——wait stage 取
	// command buffer 首次使用后缓冲的 stage（颜色附件输出）；
	// signal：renderFinished 信号量（presentKHR 的等待对象）；
	// fence：inFlightFence，命令缓冲执行完时信号。
	VkSemaphore waitSem = *imageAvailableSemaphores[currentFrame];
	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkCommandBuffer cb = *commandBuffers[currentFrame];
	VkSemaphore signalSem = *renderFinishedSemaphores[currentFrame];
	VkFence rawFence = *inFlightFences[currentFrame];

	VkSubmitInfo rawSubmitInfo{};
	rawSubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	rawSubmitInfo.waitSemaphoreCount = 1;
	rawSubmitInfo.pWaitSemaphores = &waitSem;
	rawSubmitInfo.pWaitDstStageMask = &waitStage;
	rawSubmitInfo.commandBufferCount = 1;
	rawSubmitInfo.pCommandBuffers = &cb;
	rawSubmitInfo.signalSemaphoreCount = 1;
	rawSubmitInfo.pSignalSemaphores = &signalSem;

	VkResult submitVkResult = ::vkQueueSubmit(graphicsQueue, 1, &rawSubmitInfo, rawFence);
	// DEVICE_LOST：GPU 挂起/复位，不可恢复——记录后退出帧循环
	if (submitVkResult == VK_ERROR_DEVICE_LOST) {
		auto logger = RLLogger::getInstance();
		if (logger) logger->error("Renderer: device lost on submit!");
		return std::unexpected(Error{ErrorCode::DeviceLost, "VK_ERROR_DEVICE_LOST on submit"});
	}

	// ⑭ 上屏：等 renderFinished 信号量（确保绘制完成），把 imageIndex 交给合成器。
	// SwapchainPresentFenceInfoEXT（KHR/EXT 为 typedef 别名，sType 相同）链到
	// presentInfo.pNext：present 完成时信号 presentFence，实现 present 端完成
	// 追踪（VK_KHR_swapchain_maintenance1，见 docs/技术概念.md 的"PresentFenceInfoKHR"）
	vk::SwapchainPresentFenceInfoEXT presentFenceInfoEXT{};
	presentFenceInfoEXT.setFences(*presentFences[currentFrame]);
	vk::PresentInfoKHR presentInfo{};
	presentInfo.setPNext(&presentFenceInfoEXT);
	presentInfo.setWaitSemaphores(*renderFinishedSemaphores[currentFrame]);
	presentInfo.setSwapchains(*swapchain->getSwapchain());
	presentInfo.setImageIndices(imageIndex);
	auto presentResult = graphicsQueue.presentKHR(presentInfo);

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

	// ⑮ 推进到下一个帧 slot（双缓冲）
	currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	return {};
}

}  // namespace RGL
