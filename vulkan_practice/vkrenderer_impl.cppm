module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <ranges>
#include <cstring>
#include <fmt/format.h>
module vk_renderer:impl;

import vk_renderer;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_swapchain;
import vk_resources;
import shader_registry;
import shader;
import vulkan_utils;
import VmaRaii;
import frame_draw_list;
import model_types_generated;
import features;

namespace RGL {

// Renderer 重成员函数定义（2026-09-02 编译提速 A 方案）：ctor/drawFrame/重建路径/
// 同步对象/staging/离屏辅助全部移入本 partition implementation unit——接口 TU
// （vkrenderer.cppm）只留类声明，200+ 成员函数体的 AST 不再序列化进 BMI；
// 本单元是叶子（无人 import），编译完全并行、不上关键路径。
// 各函数语义/步骤注释随体迁自 vkrenderer.cppm。

Renderer::Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::ShaderRegistry& shaderRegistry)
	: vkDevice(vkDevice),
	  swapchain(&vkSwapchain),
	  resources(&vkResources),
	  registry(shaderRegistry),
	  offscreen(std::getenv("RGL_OFFSCREEN") != nullptr),
	  // edS3：depth clip 动态状态能力（feature 探测位，启动期一次；未支持 = 恒 false no-op）
	  depthClipDynamic(vkDevice.getOptionalFeatures().depthClipEnable),
	  graphicsQueue(vkDevice.getGraphicsQueue()),
	  graphicsCommandPool(vkDevice.getGraphicsCommandPool()) {
		// descriptor heap 的 GPU 端地址，每帧传给 vkCmdBindResourceHeapEXT
		heapAddress = vkResources.getDescriptorHeap().getGpuAddress();
		// 透明段状态（Phase 5）：不透明段之外的差异三项——不写深度 + 开 alpha 混合 +
		// 背面剔除。cull 必要：关深度写后封闭网格（如 glTF 盒）的正反两面同屏各混合
		// 一次 = 有效 alpha 翻倍（实测 0.25 → 1-(1-0.25)²=0.4375）；剔除背面后
		// 每像素单层混合，alpha 语义正确（opaque 段保持 eNone——硬编码 cube 绕序未验证）
		transparentState.depthWrite = false;
		transparentState.colorBlend = true;
		transparentState.cullMode = vk::CullModeFlagBits::eBack;
		// 天空盒段状态（Phase 5.5 批次 4）：大三角 z=1（far 平面）+ LEQUAL + 不写深度——
		// 只在未被几何占据的像素（深度 = clear 1.0）通过测试画天空；绕序不剔（cull None）
		skyboxState.depthWrite = false;
		skyboxState.depthCompareOp = vk::CompareOp::eLessOrEqual;
		createSyncObjects();
		allocateCommandBuffers();
		createSceneStaging(vkResources);
	}
void Renderer::createSyncObjects() {
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
void Renderer::allocateCommandBuffers() {
		vk::CommandBufferAllocateInfo allocInfoCmd{
			*graphicsCommandPool, vk::CommandBufferLevel::ePrimary,
			static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
		commandBuffers = vkDevice.getDevice().allocateCommandBuffers(allocInfoCmd);
	}
void Renderer::createSceneStaging(RGL::Resources& vkResources) {
		for (auto& staging : sceneStaging) {
			vk::BufferCreateInfo info{};
			info.size = kSceneDataBytes;
			// eShaderDeviceAddress：createBufferWithAddress 无条件查询地址（staging 实际
			// 不用，usage 缺位会被 VVL 拦下 vkGetBufferDeviceAddress 报错）
			info.usage = vk::BufferUsageFlagBits::eTransferSrc |
				vk::BufferUsageFlagBits::eShaderDeviceAddress;
			VmaAllocationCreateInfo alloc{};
			alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
			alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
				VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
			VmaAllocationInfo allocData{};
			auto result = vkResources.getAllocator().createBufferWithAddress(
				vkDevice.getDevice(), info, alloc, &allocData);
			if (!result)
				throw std::runtime_error(fmt::format(
					"Renderer: scene staging alloc failed: {}", result.error().describe()));
			staging = std::move(*result);
		}
	}
std::expected<RGL::VmaImage, Error> Renderer::createOffscreenImage(RGL::VmaAllocatorRAII& allocator,
		vk::Extent2D extent) {
		vk::ImageCreateInfo info{};
		info.imageType = vk::ImageType::e2D;
		info.format = vk::Format::eR8G8B8A8Unorm;
		info.extent = vk::Extent3D(extent.width, extent.height, 1);
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = vk::SampleCountFlagBits::e1;
		info.tiling = vk::ImageTiling::eOptimal;
		info.initialLayout = vk::ImageLayout::eUndefined;
		info.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc;
		VmaAllocationCreateInfo alloc{};
		alloc.usage = VMA_MEMORY_USAGE_AUTO;
		return allocator.createImage(info, alloc);
	}
VkImageViewCreateInfo Renderer::buildOffscreenView() const {
		VkImageViewCreateInfo info{};
		info.image = offscreenImage.image;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = VK_FORMAT_R8G8B8A8_UNORM;
		info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		return info;
	}
std::expected<void, Error> Renderer::recreateSwapchainAndDepth(const char* context) {
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
			return {};  // extent 未变 → 深度图/离屏图仍有效，跳过重建
		}
		auto result = resources->recreateDepthImage(swapchain->getExtent());
		if (!result) {
			auto logger = RLLogger::getInstance();
			if (logger) logger->error("Renderer: failed to recreate depth image ({}): {}",
				context, result.error().describe());
			return std::unexpected(std::move(result).error());
		}
		// DIAG（RGL_OFFSCREEN）：离屏目标随 swapchain extent 同步重建（同深度图规则）；
		// 仅当已惰性创建（resize 只会在首帧 drawFrame 之后发生）
		if (offscreen && offscreenImage.image) {
			auto offscreenResult = createOffscreenImage(resources->getAllocator(), swapchain->getExtent());
			if (!offscreenResult) {
				auto logger = RLLogger::getInstance();
				if (logger) logger->error("Renderer: failed to recreate offscreen image ({}): {}",
					context, offscreenResult.error().describe());
				return std::unexpected(std::move(offscreenResult).error());
			}
			offscreenImage = std::move(*offscreenResult);
			offscreenLayout = vk::ImageLayout::eUndefined;  // 新图创建：布局 Undefined
			offscreenView.emplace(vkDevice.getDevice(), buildOffscreenView());
		}
		return {};
	}
	std::expected<void, Error> Renderer::drawFrame(const FrameDrawList& list) {
		const uint64_t frame = frameCount++;  // DIAG：device lost 帧号定位
		auto& device = vkDevice.getDevice();

		// DIAG（RGL_OFFSCREEN）：离屏目标惰性创建（首帧才分配——raii view 构造期即调
		// vkCreateImageView，构造期 null image 会被校验拦下；默认路径零分配零调用）
		if (offscreen && !offscreenImage.image) {
			auto offscreenResult = createOffscreenImage(resources->getAllocator(), swapchain->getExtent());
			if (!offscreenResult)
				return std::unexpected(std::move(offscreenResult).error());
			offscreenImage = std::move(*offscreenResult);
			offscreenLayout = vk::ImageLayout::eUndefined;  // 新图创建：布局 Undefined
			offscreenView.emplace(device, buildOffscreenView());
		}

	// ① 等待上一帧同 slot 完全结束：inFlight fence（命令缓冲执行完）与 present
	// fence（present 完成）都信号后，本 slot 的 semaphore / command buffer 才可重用。
	// waitAll=VK_TRUE + 无限超时。正常时 fence 保证在超时内信号；device reset/lost 时
	// fence 永不信号 → 返回 eErrorDeviceLost/Reset（非无限自旋，报错退出帧循环）
	// DIAG（RGL_NO_PRESENT）：不 present → presentFence 永不 signal，只等 inFlight
	const vk::Result waitResult = noPresent
		? device.waitForFences(
			{*inFlightFences[currentFrame]}, VK_TRUE, std::numeric_limits<uint64_t>::max())
		: device.waitForFences(
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
		device.resetFences({*inFlightFences[currentFrame]});
	} else {
		device.resetFences({*inFlightFences[currentFrame], *presentFences[currentFrame]});
	}

	// ② 从 swapchain 取一张后缓冲：成功时驱动信号 imageAvailableSemaphores
	//（queueSubmit 的等待对象），imageIndex 为取到的图像下标。
	// 100ms 超时（非无限）：compositor 停发 present（如 Wayland 息屏）时限时返回
	// eTimeout，跳过本帧保住帧循环存活（llm_debug 离屏截图与事件轮询不依赖本帧
	// 上屏）；正常 vsync 节奏下 acquire 在超时前返回，行为不变
	auto [acquireResult, imageIndex] = swapchain->getSwapchain().acquireNextImage(
		100'000'000,
		*imageAvailableSemaphores[currentFrame], nullptr);

	// eTimeout：无可用后缓冲——本帧不渲染，事件/system 下一轮照常
	if (acquireResult == vk::Result::eTimeout) {
		return {};
	}

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

	// ③.5 scene data 上传（Phase 5 批次 2）：CPU 写 staging[currentFrame]——本 slot
	// 的 fence 已在 ① wait（f-2 帧命令跑完，staging 不再被读），写安全。
	// 容量契约（objectMatrices ≤ kSceneMatrixCapacity）由 main 启动期检查，此处防御。
	{
		auto* stage = static_cast<std::byte*>(sceneStaging[currentFrame].getMappedData());
		std::memcpy(stage, &list.sceneData.light, sizeof(LightDataGpu));
		// Phase 5.5：env 参数写 @48（light 48B 之后、64B 头内——与 objectMatrices 区无重叠）
		std::memcpy(stage + kSceneDataEnvOffset, &list.sceneData.envParams, sizeof(glm::vec2));
		// Phase 5.5 批次 4：skyboxInvViewProj 写 @64（sky.slang skyClipToWorld 消费）
		std::memcpy(stage + kSceneDataSkyOffset, &list.sceneData.skyInvViewProj, sizeof(glm::mat4));
		const auto matricesBytes = list.sceneData.objectMatrices.size() * sizeof(glm::mat4);
		if (matricesBytes > kSceneDataBytes - kSceneDataHeaderBytes)
			return std::unexpected(Error{ErrorCode::Unknown,
				"scene objectMatrices exceeds capacity (Phase 5 contract, main startup check bypassed)"});
		std::memcpy(stage + kSceneDataHeaderBytes, list.sceneData.objectMatrices.data(),
			matricesBytes);
	}

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
	// DIAG（RGL_OFFSCREEN）：离屏目标 → ColorAttachmentOptimal。
	// 与后缓冲不同：离屏图是持久图（storeOp=eStore，endRendering 后布局保留），
	// 帧末 blit barrier 迁到 TransferSrcOptimal 且从不迁回 → 第 2 帧起真实布局是
	// TransferSrcOptimal，声明 eUndefined 失配（非 swapchain 图无 Undefined 豁免）。
	// 跟踪真实布局（offscreenLayout，帧末 blit 后更新）
	if (offscreen) {
		auto offscreenToDrawBarrier = RGL::createImageBarrier(
			offscreenImage.image, offscreenLayout,
			vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eTopOfPipe,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::AccessFlagBits2::eNone,
			vk::AccessFlagBits2::eColorAttachmentWrite);
		vk::DependencyInfo offscreenDepInfo;
		offscreenDepInfo.setImageMemoryBarriers(offscreenToDrawBarrier);
		commandBuffers[currentFrame].pipelineBarrier2(offscreenDepInfo);
	}

	// ⑤ 动态渲染附件（Dynamic Rendering：不创建 RenderPass 对象，
	// beginRendering 时直接描述附件）
	// 后缓冲：绘制前清深灰色 (0.1) 作为背景；Store 写回结果（后缓冲要上屏，
	// 不能 DontCare）
	vk::RenderingAttachmentInfo colorAttachment{};
	// DIAG（RGL_OFFSCREEN）：场景渲到离屏目标（纹理采样在离屏 pass，后缓冲只做 blit 拷贝）
	colorAttachment.imageView = offscreen ? *offscreenView : *swapchainImageViews[imageIndex];
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
	// 两个 heap 的读访问 flag 相互独立（spec：resource heap 与 sampler heap 各有专属
	// access bit）——只设 eResourceHeapReadEXT 时 sampler 描述符的 host write 未同步到
	// GPU，sampler 描述符读到未初始化内存 → 采样恒 garbage（7e1abce 内修复，当时注释
	// "Phase 3 黑屏根因"；2026-08-28 G 实验：当前启动期 fence/waitIdle 已掩盖该缺位
	// 的影响，spec 层面仍必须保留，见 docs/archive/heap_texture_research.md §12）
	// eTransferRead：⑥.5 的 vkCmdCopyBuffer 在 eTransfer 阶段读 host-mapped staging
	// buffer（scene data 源），本 eHost→eAllGraphics barrier 是该读之前唯一的
	// host write → device read 排序点，dstAccess 须覆盖具体读（VVL sync validation）
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT |
		vk::AccessFlagBits2::eSamplerHeapReadEXT | vk::AccessFlagBits2::eTransferRead;
	heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
	heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
	vk::DependencyInfo heapDepInfo{};
	heapDepInfo.setMemoryBarriers(heapBarrier);
	commandBuffers[currentFrame].pipelineBarrier2(heapDepInfo);

	// ⑥.5 scene data staging → device buffer 拷贝（Phase 5 批次 2）+ Transfer 写 →
	// 图形阶段 shader 读 barrier（storage buffer 的写/读即使同 cmd 也须显式依赖，
	// VVL BUFFER_SYNC 检查项）。目标未创建（无模型场景 = ensureSceneData 未被调用）
	// 则跳过——cube 管线不绑 scene data。
	{
		const auto& sceneBuffer = resources->getSceneDataBuffer();
		if (sceneBuffer.buffer != VK_NULL_HANDLE) {
			VkBufferCopy region{0, 0, static_cast<VkDeviceSize>(kSceneDataBytes)};
			::vkCmdCopyBuffer(*commandBuffers[currentFrame], *sceneStaging[currentFrame],
				sceneBuffer.buffer, 1, &region);
			vk::BufferMemoryBarrier2 sceneBarrier{};
			sceneBarrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
			sceneBarrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
			sceneBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
			sceneBarrier.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
			sceneBarrier.buffer = sceneBuffer.buffer;
			sceneBarrier.size = kSceneDataBytes;
			vk::DependencyInfo sceneDepInfo;
			sceneDepInfo.setBufferMemoryBarriers(sceneBarrier);
			commandBuffers[currentFrame].pipelineBarrier2(sceneDepInfo);
		}
	}

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
	// sampler heap 同法绑定（Phase 3：OpTypeSampler 的 sampler 描述符；每帧无条件，
	// 与 resource heap 一致——未引用时绑定无副作用）
	vk::DeviceAddressRangeEXT samplerHeapRange{};
	samplerHeapRange.address = resources->getSamplerHeap().getGpuAddress();
	samplerHeapRange.size = resources->getSamplerHeap().getHeapSize();
	commandBuffers[currentFrame].bindSamplerHeapEXT(vk::BindHeapInfoEXT(
		samplerHeapRange,
		0,
		resources->getSamplerReservedRangeSize()));

	// ⑧ 开始渲染（Dynamic Rendering 的 render pass 起点）
	commandBuffers[currentFrame].beginRendering(renderingInfo);

	// ⑨ 绘制：遍历渲染 system 产出的绘制列表（顺序 = 列表顺序；push data 布局随管线，
	// 见 DrawCall）。每条：推绘制参数（push constant，独立于 descriptor heap，不占 slot，
	// Slang 端经 [[vk::push_constant]] 访问，spec 3.4.1；host 内存录制时读取；
	// push set 经 PipelineEntry.pushDataSet 显式化，Phase 4 归一，L5 解决——替代旧硬编码 0）→
	// 绑定 ShaderObject → drawMeshTasksEXT（mesh shader 内部生成顶点/三角形，
	// 不需要传统顶点管线；group 数经资产静态字段 MeshAsset.groupCount，见 renderListSystem）
	// 动态状态在 draw 时采样（spec chapters/shaders.adoc "Setting State"），与 bindShadersEXT
	// 的先后无约束。Phase 5 透明排序：opaque/transparent 两段各在段首 applyDynamicState 一次
	//（不透明写深度关混合 → 透明关深度写开混合；段内循环不变）。
	auto drawCall = [&](const DrawCall& call) {
		const auto& entry = registry.entry(call.pipeline);
		commandBuffers[currentFrame].pushDataEXT(vk::PushDataInfoEXT(
			entry.pushDataSet, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
		entry.pipeline.bindShaders(commandBuffers[currentFrame]);
		commandBuffers[currentFrame].drawMeshTasksEXT(call.groupCount, 1, 1);
	};
	const auto extent = swapchain->getExtent();
	RGL::applyDynamicState(commandBuffers[currentFrame], extent.width, extent.height, opaqueState,
		depthClipDynamic);
	for (uint32_t i = 0; i < list.skyboxStart; ++i)
		drawCall(list.calls[i]);
	// Phase 5.5 批次 4 天空盒段（z=1 大三角 + LEQUAL 不写深度，空段自然跳过）
	if (list.skyboxStart < list.transparentStart) {
		RGL::applyDynamicState(commandBuffers[currentFrame], extent.width, extent.height,
			skyboxState, depthClipDynamic);
		for (uint32_t i = list.skyboxStart; i < list.transparentStart; ++i)
			drawCall(list.calls[i]);
	}
	if (list.transparentStart < list.calls.size()) {
		RGL::applyDynamicState(commandBuffers[currentFrame], extent.width, extent.height,
			transparentState, depthClipDynamic);
		for (uint32_t i = list.transparentStart; i < static_cast<uint32_t>(list.calls.size()); ++i)
			drawCall(list.calls[i]);
	}

	commandBuffers[currentFrame].endRendering();

	// DIAG（RGL_OFFSCREEN）：离屏目标 → 后缓冲 blit（纹理采样已在离屏 pass 完成，
	// 本阶段纯拷贝不触碰纹理描述符）；后缓冲 ④ 已迁 CAO，此处迁 TransferDst 作 blit 目标
	if (offscreen) {
		auto offscreenToBlitBarrier = RGL::createImageBarrier(
			offscreenImage.image, vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageLayout::eTransferSrcOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::AccessFlagBits2::eTransferRead);
		auto swapchainToBlitBarrier = RGL::createImageBarrier(
			swapchainImages[imageIndex], vk::ImageLayout::eColorAttachmentOptimal,
			vk::ImageLayout::eTransferDstOptimal,
			vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eColorAttachmentOutput,
			vk::PipelineStageFlagBits2::eTransfer,
			vk::AccessFlagBits2::eColorAttachmentWrite,
			vk::AccessFlagBits2::eTransferWrite);
		std::array<vk::ImageMemoryBarrier2, 2> blitBarriers = {offscreenToBlitBarrier, swapchainToBlitBarrier};
		vk::DependencyInfo blitDepInfo;
		blitDepInfo.setImageMemoryBarriers(blitBarriers);
		commandBuffers[currentFrame].pipelineBarrier2(blitDepInfo);
		vk::ImageBlit blitRegion{};
		blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		const int32_t w = static_cast<int32_t>(swapchain->getExtent().width);
		const int32_t h = static_cast<int32_t>(swapchain->getExtent().height);
		// hpp ArrayWrapper1D 不接受 brace 整体赋值，逐元素填（std::array 布局）
		blitRegion.srcOffsets[0] = vk::Offset3D{0, 0, 0};
		blitRegion.srcOffsets[1] = vk::Offset3D{w, h, 1};
		blitRegion.dstOffsets[0] = vk::Offset3D{0, 0, 0};
		blitRegion.dstOffsets[1] = vk::Offset3D{w, h, 1};
		// 裸 C 调用（同 llm_debug 回读拷贝风格）：hpp 重载走 ArrayProxy，此处 count+指针直传
		::vkCmdBlitImage(*commandBuffers[currentFrame], offscreenImage.image,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchainImages[imageIndex],
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
			reinterpret_cast<const VkImageBlit*>(&blitRegion), VK_FILTER_LINEAR);
		offscreenLayout = vk::ImageLayout::eTransferSrcOptimal;  // 本帧末真实布局，下帧 ④ 用
	}

	// ⑫ 后缓冲：ColorAttachmentOptimal（直渲）/ TransferDstOptimal（RGL_OFFSCREEN blit 后）
	// → PresentSrcKHR，使其"可 present"。该转换必须排在颜色附件的最后一次使用之后
	//（此处紧随 endRendering / blit）。
	vk::ImageMemoryBarrier2 colorToPresentBarrier = RGL::createImageBarrier(
		swapchainImages[imageIndex],
		offscreen ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eColorAttachmentOptimal,
		vk::ImageLayout::ePresentSrcKHR, vk::ImageAspectFlagBits::eColor,
		offscreen ? vk::PipelineStageFlagBits2::eTransfer : vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::PipelineStageFlagBits2::eBottomOfPipe,
		offscreen ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::AccessFlagBits2::eNone);
	vk::DependencyInfo colorToPresentDepInfo;
	colorToPresentDepInfo.setImageMemoryBarriers(colorToPresentBarrier);
	commandBuffers[currentFrame].pipelineBarrier2(colorToPresentDepInfo);
	// 录制完毕
	commandBuffers[currentFrame].end();

	// ⑬ 提交到图形队列。用裸 C API（vkQueueSubmit）而非 vk-hpp RAII：
	// RAII 的 Queue::submit 重载不支持 fence 参数。
	// wait：imageAvailable 信号量（acquireNextImage 信号）——wait stage 必须覆盖 command
	// buffer 首次使用后缓冲的 stage：直渲 = 颜色附件输出；RGL_OFFSCREEN = blit（Transfer）。
	// signal：renderFinished 信号量（presentKHR 的等待对象）；
	// fence：inFlightFence，命令缓冲执行完时信号。
	VkSemaphore waitSem = *imageAvailableSemaphores[currentFrame];
	const VkPipelineStageFlags waitStage = offscreen ? VK_PIPELINE_STAGE_TRANSFER_BIT
		: VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
		if (logger) logger->error("Renderer: device lost on submit! (frame {})", frame);
		return std::unexpected(Error{ErrorCode::DeviceLost, "VK_ERROR_DEVICE_LOST on submit"});
	}
	// 其余失败（OOM/device reset 等）：fence 可能永不信号 → 下帧 ① 会报错退出，
	// 此处先留观测（旧实现静默吞掉，排障零线索）
	if (submitVkResult != VK_SUCCESS) {
		auto logger = RLLogger::getInstance();
		if (logger) logger->error("Renderer: submit failed (frame {}): {}", frame,
			vk::to_string(vk::Result{submitVkResult}));
	}

	// ⑭ 上屏：等 renderFinished 信号量（确保绘制完成），把 imageIndex 交给合成器。
	// SwapchainPresentFenceInfoEXT（KHR/EXT 为 typedef 别名，sType 相同）链到
	// presentInfo.pNext：present 完成时信号 presentFence，实现 present 端完成
	// 追踪（VK_KHR_swapchain_maintenance1，见 docs/技术概念.md 的"PresentFenceInfoKHR"）
	// DIAG（RGL_NO_PRESENT）：跳 present——buffer 不出进程，KWin 不 import dmabuf
	if (!noPresent) {
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
		// 其余错误码（如 SURFACE_LOST）：spec 语义下队列操作仍入队、present fence 仍
		// 信号（不死锁），但必须留观测——下帧 acquire 大概率持续失败
		if (presentResult != vk::Result::eSuccess &&
			presentResult != vk::Result::eErrorDeviceLost &&
			presentResult != vk::Result::eErrorOutOfDateKHR &&
			presentResult != vk::Result::eSuboptimalKHR) {
			auto logger = RLLogger::getInstance();
			if (logger) logger->error("Renderer: present failed (frame {}): {}", frame,
				vk::to_string(presentResult));
		}
	}

	// ⑮ 推进到下一个帧 slot（双缓冲）
	currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	return {};
}

}  // namespace RGL
