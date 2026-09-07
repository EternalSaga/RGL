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
import render_graph;

namespace RGL {

// Renderer 重成员函数定义（2026-09-02 编译提速 A 方案）：ctor/drawFrame/重建路径/
// 同步对象/staging 全部移入本 partition implementation unit——接口 TU
// （vkrenderer.cppm）只留类声明，200+ 成员函数体的 AST 不再序列化进 BMI；
// 本单元是叶子（无人 import），编译完全并行、不上关键路径。
// 各函数语义/步骤注释随体迁自 vkrenderer.cppm。

namespace {

// 执行器可解析性校验（Phase 6，构造期 fail-fast——录制期零图错误）：
// 图必须恰好引用 swapchain/depth 各一次；draw pass 的 state/drawSource 名字可解析；
// blit pass 必须两端齐（compileRenderGraph 已保证拓扑，此处防绕过）。
void validateRenderGraphForExecutor(const RGL::CompiledRenderGraph& graph) {
	auto fail = [](std::string msg) { throw std::runtime_error("Renderer: " + msg); };
	if (graph.passes.empty())
		fail("render graph has no passes");
	uint32_t swapchainRefs = 0, depthRefs = 0;
	for (const auto& att : graph.attachments) {
		if (att.kind == RgAttachmentKind::swapchain) ++swapchainRefs;
		if (att.kind == RgAttachmentKind::depth) ++depthRefs;
	}
	if (swapchainRefs != 1)
		fail(fmt::format("render graph must reference exactly one swapchain attachment (got {})",
			swapchainRefs));
	if (depthRefs != 1)
		fail(fmt::format("render graph must reference exactly one depth attachment (got {})",
			depthRefs));
	static constexpr std::array<const char*, 3> kKnownDrawSources{"opaque", "skybox",
		"transparent"};
	for (const auto& pass : graph.passes) {
		if (pass.kind == RgPassKind::blit) {
			if (!pass.blitSource || !pass.blitTarget)
				fail(fmt::format("blit pass '{}' missing source/target resolution", pass.name));
			continue;
		}
		// Phase 7 D1：drawLight = 固定全屏 draw（无 drawSource/段），state 必 "light"
		if (pass.kind == RgPassKind::drawLight) {
			if (pass.state != "light")
				fail(fmt::format("render graph light pass '{}' has state '{}' (expected 'light')",
					pass.name, pass.state));
			continue;
		}
		if (pass.state != "opaque" && pass.state != "skybox" && pass.state != "transparent")
			fail(fmt::format("render graph pass '{}' has unknown state '{}'", pass.name,
				pass.state));
		if (!std::ranges::any_of(kKnownDrawSources,
				[&](const char* s) { return pass.drawSource == s; }))
			fail(fmt::format("render graph pass '{}' has unknown drawSource '{}'", pass.name,
				pass.drawSource));
	}
}

// FrameDrawList 段边界 → [begin, end)（drawSource 名 = 段名的机械映射，
// 段序契约 = FrameDrawList 注释：opaque | skybox | transparent）
std::pair<uint32_t, uint32_t> segmentRange(std::string_view drawSource,
	const FrameDrawList& list) {
	const uint32_t size = static_cast<uint32_t>(list.calls.size());
	if (drawSource == "opaque") return {0, list.skyboxStart};
	if (drawSource == "skybox") return {list.skyboxStart, list.transparentStart};
	return {list.transparentStart, size};
}

}  // namespace

Renderer::Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::ShaderRegistry& shaderRegistry, RGL::CompiledRenderGraph renderGraph)
	: vkDevice(vkDevice),
	  swapchain(&vkSwapchain),
	  resources(&vkResources),
	  registry(shaderRegistry),
	  renderGraph(std::move(renderGraph)),
	  attachmentLayouts(this->renderGraph.attachments.size(), vk::ImageLayout::eUndefined),
	  // edS3：depth clip 动态状态能力（feature 探测位，启动期一次；未支持 = 恒 false no-op）
	  depthClipDynamic(vkDevice.getOptionalFeatures().depthClipEnable),
	  graphicsQueue(vkDevice.getGraphicsQueue()),
	  graphicsCommandPool(vkDevice.getGraphicsCommandPool()) {
		validateRenderGraphForExecutor(this->renderGraph);
		for (const auto& warning : this->renderGraph.warnings) {
			auto logger = RLLogger::getInstance();
			if (logger) logger->warn("RenderGraph: {}", warning);
		}
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
		// Phase 7 D1：deferred light pass —— 全屏终图，无深度附件（depth 无关）、cull none
		//（大三角绕序不剔）、不混合。取 light 管线下标；图含 drawLight pass 却无 "light" 条目 = 配置错误
		// depthTest 显式关：主图 light pass 无深度附件本就不测，但 captureFrame 2-pass 截图给 light 绑了
		// 深度附件（清 1.0），默认 LESS + 大三角 z=1.0 → 1.0<1.0 全剔（截图纯 clear）。关测试后两路一致。
		lightState.depthTest = false;
		lightState.depthWrite = false;
		lightState.cullMode = vk::CullModeFlagBits::eNone;
		if (auto idx = shaderRegistry.pipelineIndex("light"))
			lightPipelineIdx = *idx;
		for (const auto& p : this->renderGraph.passes)
			if (p.kind == RgPassKind::drawLight && !lightPipelineIdx)
				throw std::runtime_error(
					"Renderer: render graph has drawLight pass but no 'light' pipeline entry "
					"(main must addLightEntry before Renderer)");
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
	const RGL::RenderState& Renderer::stateFor(std::string_view name) const {
		if (name == "opaque") return opaqueState;
		if (name == "skybox") return skyboxState;
		if (name == "transparent") return transparentState;
		if (name == "light") return lightState;
		throw std::runtime_error(
			fmt::format("Renderer::stateFor: unknown state '{}' (constructor validation bypassed)", name));
	}
	std::expected<void, Error> Renderer::drawFrame(const FrameDrawList& list) {
		const uint64_t frame = frameCount++;  // DIAG：device lost 帧号定位
		auto& device = vkDevice.getDevice();


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

	// ④ 数据通路固定段（Phase 6 起不入图——非 attachment 状态同步，见 render_graph.cppm
	// 头注释）：heap barrier → scene staging 拷贝/屏障 → heap/sampler heap 绑定。
	//（旧 ④ 的 color/depth attachment barrier 已由图 passes[0].barriers 推导发射；
	// 与 heap barrier 相互独立，命令流中先后位置对换无语义影响）
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


	// ⑤ pass 循环（执行序 = 图线性序，Phase 6）：attachment barrier（布局链推导
	// 物化）→ beginRendering（逐 pass 附件）→ applyDynamicState(pass.state)
	// → 段内逐 draw（pass.drawSource）→ endRendering。执行器盲消费——barrier/
	// load/store/布局全部来自编译图，手写布局转换清零。
	auto& swapchainImages = swapchain->getImages();
	auto& swapchainImageViews = swapchain->getImageViews();
	const auto extent = swapchain->getExtent();
	VkImage swapchainImage = swapchainImages[imageIndex];
	VkImageView swapchainView = *swapchainImageViews[imageIndex];
	VkImage depthImage = *resources->getDepthImage();
	VkImageView depthView = *resources->getDepthImageView();

	// offscreen attachment RT（批次 4）：幂等 ensure（extent 随 swapchain resize 重建，
	// 重建含 waitIdle——录制期调用安全，本 CB 尚未提交）。format 经词表解析：
	// "inherit" = swapchain 格式（blit LINEAR 的 VVL 格式兼容前提），具名 = 显式格式
	// RT（Phase 7 G-buffer；词表与校验归 render_graph，此处只物化）
	std::vector<Resources::RtAttachment*> offscreenRt(renderGraph.attachments.size());
	// RT 重建检测（generation 变化 = 新图布局 Undefined）：布局追踪同步重置，
	// 否则下帧首 barrier oldLayout 失配（extent 漂移 = resize 必现）
	std::vector<uint64_t> rtGenerations(renderGraph.attachments.size(), 0);
	for (uint32_t s = 0; s < renderGraph.attachments.size(); ++s) {
		const auto& att = renderGraph.attachments[s];
		if (att.kind != RgAttachmentKind::offscreen)
			continue;
		const auto fmt = RGL::resolveRgFormat(att.format, swapchain->getFormat().format);
		auto rtResult = resources->ensureRtAttachment(att.name, extent, fmt);
		if (!rtResult)
			return std::unexpected(std::move(rtResult).error());
		offscreenRt[s] = *rtResult;
		if ((*rtResult)->generation != rtGenerations[s]) {
			rtGenerations[s] = (*rtResult)->generation;
			attachmentLayouts[s] = vk::ImageLayout::eUndefined;
		}
	}

	auto imageForSlot = [&](RgAttachmentSlot slot) -> VkImage {
		switch (renderGraph.attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainImage;
			case RgAttachmentKind::depth: return depthImage;
			case RgAttachmentKind::offscreen: return offscreenRt[slot]->image.image;
		}
		return VK_NULL_HANDLE;  // 不可达（穷尽），消 warning
	};
	auto viewForSlot = [&](RgAttachmentSlot slot) -> VkImageView {
		switch (renderGraph.attachments[slot].kind) {
			case RgAttachmentKind::swapchain: return swapchainView;
			case RgAttachmentKind::depth: return depthView;
			case RgAttachmentKind::offscreen: return **offscreenRt[slot]->view;  /* optional 解引用 + raii handle */
		}
		return VK_NULL_HANDLE;
	};

	// 物化 + 发射一组 attachment barrier（单 DependencyInfo 批量）；oldLayout =
	// 帧首丢弃规则（eUndefined）或跨帧/帧内追踪值，发射后推进追踪
	auto emitBarriers = [&](std::span<const RgBarrierPlan> barriers) {
		if (barriers.empty()) return;
		std::vector<vk::ImageMemoryBarrier2> materialized;
		materialized.reserve(barriers.size());
		for (const auto& plan : barriers) {
			const auto oldLayout = plan.oldLayoutDiscard ? vk::ImageLayout::eUndefined
			                                              : attachmentLayouts[plan.attachment];
			materialized.push_back(RGL::materializeBarrier(plan, imageForSlot(plan.attachment),
				oldLayout));
		}
		vk::DependencyInfo depInfo;
		depInfo.setImageMemoryBarriers(materialized);
		commandBuffers[currentFrame].pipelineBarrier2(depInfo);
		for (const auto& plan : barriers)
			attachmentLayouts[plan.attachment] = plan.newLayout;
	};

	auto drawOne = [&](const DrawCall& call) {
		const auto& entry = registry.entry(call.pipeline);
		commandBuffers[currentFrame].pushDataEXT(vk::PushDataInfoEXT(
			entry.pushDataSet, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
		entry.pipeline.bindShaders(commandBuffers[currentFrame]);
		commandBuffers[currentFrame].drawMeshTasksEXT(call.groupCount, 1, 1);
	};

	for (const auto& pass : renderGraph.passes) {
		emitBarriers(pass.barriers);
		// blit pass（批次 4，RGL_OFFSCREEN 图变体的 composite）：transferRead 源 →
		// transferWrite 目标整幅拷贝（1:1 同尺寸同格式，LINEAR = 精确拷贝）；
		// 两端布局转换已由 pass.barriers 完成（scene_rt CAO→TransferSrc、
		// swapchain 帧首 Undefined→TransferDst），无绘制、无动态状态
		if (pass.kind == RgPassKind::blit) {
			VkImage srcImage = imageForSlot(*pass.blitSource);
			VkImage dstImage = imageForSlot(*pass.blitTarget);
			vk::ImageBlit blitRegion{};
			blitRegion.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			blitRegion.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
			const int32_t w = static_cast<int32_t>(extent.width);
			const int32_t h = static_cast<int32_t>(extent.height);
			// hpp ArrayWrapper1D 不接受 brace 整体赋值，逐元素填（同旧 DIAG 通路写法）
			blitRegion.srcOffsets[0] = vk::Offset3D{0, 0, 0};
			blitRegion.srcOffsets[1] = vk::Offset3D{w, h, 1};
			blitRegion.dstOffsets[0] = vk::Offset3D{0, 0, 0};
			blitRegion.dstOffsets[1] = vk::Offset3D{w, h, 1};
			::vkCmdBlitImage(*commandBuffers[currentFrame], srcImage,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
				reinterpret_cast<const VkImageBlit*>(&blitRegion), VK_FILTER_LINEAR);
			continue;
		}

		std::vector<vk::RenderingAttachmentInfo> colorInfos;
		colorInfos.reserve(pass.colors.size());
		for (const auto& cp : pass.colors) {
			vk::RenderingAttachmentInfo info{};
			info.imageView = viewForSlot(cp.slot);
			info.imageLayout = cp.layout;
			info.loadOp = cp.loadOp;
			info.storeOp = cp.storeOp;
			info.clearValue = cp.clearValue;
			colorInfos.push_back(info);
		}
		// 背景清深灰色 (0.1) 等清除值来自图计划（默认图 = 原 ⑤ 逐位一致）；
		// 深度清 1.0，末段 DontCare 省带宽（中间 pass store 保内容）
		std::optional<vk::RenderingAttachmentInfo> depthInfo;
		if (pass.depth) {
			vk::RenderingAttachmentInfo info{};
			info.imageView = viewForSlot(pass.depth->slot);
			info.imageLayout = pass.depth->layout;
			info.loadOp = pass.depth->loadOp;
			info.storeOp = pass.depth->storeOp;
			info.clearValue = pass.depth->clearValue;
			depthInfo = info;
		}
		vk::RenderingInfo renderingInfo{};
		renderingInfo.renderArea = vk::Rect2D({0, 0}, extent);
		renderingInfo.layerCount = 1;
		renderingInfo.setColorAttachments(colorInfos);
		if (depthInfo)
			renderingInfo.setPDepthAttachment(&*depthInfo);

		commandBuffers[currentFrame].beginRendering(renderingInfo);
		// 动态状态在 draw 时采样（spec chapters/shaders.adoc "Setting State"），与
		// bindShadersEXT 先后无约束；段首各 applyDynamicState 一次（Phase 5 语义，
		// 状态对象经 pass.state 名解析——opaque/skybox/transparent 三成员不变）
		RGL::applyDynamicState(commandBuffers[currentFrame], extent.width, extent.height,
			stateFor(pass.state), depthClipDynamic, static_cast<uint32_t>(pass.colors.size()));
		if (pass.kind == RgPassKind::drawLight) {
			// Phase 7 D1：固定全屏单 group light draw（无 push constant；采样描述符 baked 在 light
			// shader object 的 extra mapping，4 RT 的 CAO→SRO 布局转换已由 pass.barriers 完成）
			registry.entry(*lightPipelineIdx).pipeline.bindShaders(commandBuffers[currentFrame]);
			commandBuffers[currentFrame].drawMeshTasksEXT(1, 1, 1);
		} else {
			const auto [segBegin, segEnd] = segmentRange(pass.drawSource, list);
			for (uint32_t i = segBegin; i < segEnd; ++i)
				drawOne(list.calls[i]);
		}
		commandBuffers[currentFrame].endRendering();
	}

	// ⑫ swapchain → PresentSrcKHR（图 presentTransition——src 阶段/访问由最后
	// usage 推导：直渲 = CAO 写、blit 上屏 = Transfer 写，等价旧手工分支；
	// 无 swapchain 写入的离屏图 = nullopt 跳过）
	if (renderGraph.presentTransition)
		emitBarriers(std::span<const RgBarrierPlan>{&*renderGraph.presentTransition, 1});

	// 录制完毕
	commandBuffers[currentFrame].end();

	// ⑬ 提交到图形队列。用裸 C API（vkQueueSubmit）而非 vk-hpp RAII：
	// RAII 的 Queue::submit 重载不支持 fence 参数。
	// wait：imageAvailable 信号量（acquireNextImage 信号）——wait stage = 图派生的
	// swapchainWaitStage（命令流首次触碰后缓冲的阶段：直渲图 = ColorAttachmentOutput、
	// blit 上屏图 = Transfer；现值均在 stage1 32 位范围内，截断安全）。
	// signal：renderFinished 信号量（presentKHR 的等待对象）；
	// fence：inFlightFence，命令缓冲执行完时信号。
	VkSemaphore waitSem = *imageAvailableSemaphores[currentFrame];
	const VkPipelineStageFlags waitStage = static_cast<VkPipelineStageFlags>(
		static_cast<VkPipelineStageFlags2>(renderGraph.swapchainWaitStage));
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
