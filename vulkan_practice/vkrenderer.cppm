module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
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
import VmaRaii;
import model_types_generated;
import shader;
import scene_config;
import vkerror;

namespace RGL {

// Renderer：每帧"录制 → 提交 → 显示"的驱动器 + 每帧同步状态的持有者。
//
// 渲染流程（一次 drawFrame(list) 调用，逐步骤注释见函数体内 ①~⑮）：
//   等待上一帧同 slot 的双 fence（submit 与 present 均完成）并重置
//   → acquireNextImage 获取后缓冲（信号 imageAvailable 信号量）
//   → 录制命令缓冲：图像 barrier → 动态渲染附件
//     → descriptor heap barrier/绑定 → beginRendering
//     → 动态状态 → 逐 draw：push data → 绑定 ShaderObject → drawMeshTasksEXT → endRendering
//     → PresentSrc barrier
//   → queueSubmit（等 imageAvailable / 信号 renderFinished / 挂 inFlight fence）
//   → presentKHR（pNext 挂 SwapchainPresentFenceInfoEXT 信号 present fence）
//   → currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT
//
// 绘制内容 = 渲染 system 产出的 FrameDrawList（本类是盲执行器，不认识具体场景资产，
// 见 DrawCall/FrameDrawList 注释）。核心现代特性：Dynamic Rendering（无 RenderPass）+
// ShaderObject（无 PipelineLayout，bindShadersEXT + 动态状态）+ DescriptorHeap
//（vkCmdBindResourceHeapEXT 取代 descriptor set，使用规范见 docs/DESCRIPTOR_HEAP_SPEC.md）。

// 一次绘制（FrameDrawList 的条目；由渲染 system 每帧产出，drawFrame 消费）：
//   pipeline   = 管线表下标（经 Renderer::pipelineIndex 由条目名解析，构造后稳定）
//   pushData   = 本次 draw 的 push constant，布局随管线（CPU 内存录制时读取——
//                vkCmdPushDataEXT 只要求内存活到录制结束，故帧局部存储即可）
//   groupCount = drawMeshTasksEXT 的 X（Y/Z 现状恒 1）
export struct DrawCall {
	uint32_t pipeline;
	std::span<const std::byte> pushData;
	uint32_t groupCount;
};

// 每帧绘制列表：顺序 = 绘制顺序（Phase 5 透明排序 / Phase 7 culling 输出的落点）。
export struct FrameDrawList {
	std::vector<DrawCall> calls;
};

export class Renderer;

export class Renderer {
   public:
	// 成员初始化列表顺序必须与下方声明顺序一致（RAII 规则：vk::raii::* 直接成员只能在
	// 初始化列表初始化）。modelSpec 从 pipelineSpecs（SPV 路径，来自 configs/scene.lua）
	// 解析并按值存一份（span 指向 main 局部数据，生命周期不够长）；pipelines 依赖
	// Resources 的 descriptor heap（descriptorSize + 顶点/索引 slot 号）与 pipelineSpecs，
	// 故在 resources 之后、经静态工厂 makePipelineTable 在初始化列表构造
	//（RenderPipeline 是包装类，非直接 vk::raii 成员）。
	// "cube" 管线条目恒建（批次 3：删构造期 gating——画不画 = cube entity 有无，
	// 数据驱动，showCube 配置由 main 决定是否注册 entity；条目多建 2 个 shader object，
	// 成本可忽略）。模型管线条目不在构造期建（批次 3：管线表与场景内容解耦，E1.3）——
	// 资产上传后每 primitive 调一次 addModelPipeline。
	Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		std::span<const RGL::PipelineSpec> pipelineSpecs)
	: vkDevice(vkDevice),
	  swapchain(&vkSwapchain),
	  resources(&vkResources),
	  offscreen(std::getenv("RGL_OFFSCREEN") != nullptr),
	  graphicsQueue(vkDevice.getGraphicsQueue()),
	  graphicsCommandPool(vkDevice.getGraphicsCommandPool()),
	  modelSpec(resolveModelSpec(pipelineSpecs)),
	  pipelines(makePipelineTable(vkDevice.getDevice(), pipelineSpecs, vkResources)) {
		// descriptor heap 的 GPU 端地址，每帧传给 vkCmdBindResourceHeapEXT
		heapAddress = vkResources.getDescriptorHeap().getGpuAddress();
		// 管线表 name → 下标（构造一次，供渲染 system 产出 DrawCall 时经 pipelineIndex 解析）
		for (size_t i = 0; i < pipelines.size(); ++i) {
			pipelineIndexMap[pipelines[i].name] = static_cast<uint32_t>(i);
		}
		createSyncObjects();
		allocateCommandBuffers();
	}

	RGL::Swapchain& getSwapchainRef() const { return *swapchain; }
	RGL::Resources& getResourcesRef() const { return *resources; }
	// 离屏截图（llm_debug.cppm captureFrame）用的只读访问器
	RGL::DeviceContext& getDeviceContextRef() const { return vkDevice; }
	const vk::raii::CommandPool& getGraphicsCommandPoolRef() const { return graphicsCommandPool; }
	const RGL::RenderPipeline& pipelineAt(uint32_t index) const { return pipelines[index].pipeline; }
	const RGL::RenderState& getOpaqueState() const { return opaqueState; }
	// 管线表条目名 → 下标（启动期解析用：bindPipelineToAsset / 防御检查；
	// 每帧路径经资产记录的 pipelineIndex 直接取，零字符串查找）
	[[nodiscard]] std::optional<uint32_t> pipelineIndex(std::string_view name) const {
		auto it = pipelineIndexMap.find(std::string(name));
		return it == pipelineIndexMap.end() ? std::nullopt : std::optional<uint32_t>(it->second);
	}
	// 管线表下标写回资产记录（批次 3：启动期一次，cube entity 走此路径；
	// addModelPipeline 内部同款写回）。条目缺失 = 配置/构建不一致，直接报错
	void bindPipelineToAsset(const char* name, uint32_t assetId) {
		auto idx = pipelineIndex(name);
		if (!idx)
			throw std::runtime_error(std::string("pipeline entry not found: ") + name);
		resources->getAssetTable().setPipelineIndex(assetId, *idx);
	}
	// 模型管线条目（批次 3：从构造期移到资产上传期，每 primitive 一次）：
	// 用构造期解析的 "model" 规格创建 RenderPipeline，binding mapping 显式烘焙
	// 顶点 slot + 纹理 slot（Phase 3：binding 1 = resource heap 的 SAMPLED_IMAGE 描述符槽、
	// binding 2 = sampler heap 槽——OpTypeSampler 映射，共享一份 sampler 描述符，
	// 偏移 = samplerSlot * samplerHeap.getDescriptorSize()）；
	// 更新 name→下标 映射，并把条目下标写回资产记录（renderListSystem 经资产表解析管线，
	// 无 "model_i" 字符串约定，E2.4）。失败（SPV 加载失败等）抛出（同构造期路径）。
	void addModelPipeline(uint32_t vertexSlot, uint32_t textureSlot, uint32_t samplerSlot,
		uint32_t assetId) {
		const uint32_t descriptorSize = resources->getDescriptorHeap().getDescriptorSize();
		const uint32_t samplerHeapOffset =
			samplerSlot * resources->getSamplerHeap().getDescriptorSize();
		auto entry = PipelineEntry{
			"model_" + std::to_string(assetId),
			RGL::RenderPipeline(vkDevice.getDevice(), loadShaderEffect(modelSpec), descriptorSize,
				vertexSlot, vertexSlot, textureSlot, samplerHeapOffset, true)};
		const uint32_t index = static_cast<uint32_t>(pipelines.size());
		pipelineIndexMap[entry.name] = index;
		pipelines.push_back(std::move(entry));
		resources->getAssetTable().setPipelineIndex(assetId, index);
	}
	// 等待 GPU 空闲（resize / 重建 swapchain 前调用，确保旧资源不再被使用）
	void waitForIdle() { vkDevice.getDevice().waitIdle(); }
	// 渲染一帧：整体流程见类头注释；绘制内容 = 渲染 system 产出的 FrameDrawList
	//（盲执行器，不认识具体场景资产）；失败（device lost 等）时返回错误
	[[nodiscard]] std::expected<void, Error> drawFrame(const FrameDrawList& list);

   private:
	// 管线表的一条：shader 风格名 + 其 RenderPipeline（shader object + 烘焙的
	// descriptor heap binding mapping）。Phase 2 按条目索引；Phase 4 升级为 name→pipeline 的 ShaderRegistry。
	struct PipelineEntry {
		std::string name;
		RGL::RenderPipeline pipeline;
	};

	// 把一条 pipeline 规格对应的 mesh/fragment SPV 载入 ShaderEffect（SPV 路径来自
	// SceneConfig，不再写死）。
	static RGL::ShaderEffect loadShaderEffect(const RGL::PipelineSpec& spec) {
		auto meshResult = RGL::loadShaderSpv(spec.meshSpv);
		if (!meshResult) throw std::runtime_error(meshResult.error().describe());
		auto fragResult = RGL::loadShaderSpv(spec.fragSpv);
		if (!fragResult) throw std::runtime_error(fragResult.error().describe());

		RGL::ShaderEffect effect;
		effect.meshStageSpv = std::move(*meshResult);
		effect.fragStageSpv = std::move(*fragResult);
		return effect;
	}

	// 按名查找 pipeline 规格（找不到返回 nullptr）。
	static const RGL::PipelineSpec* findPipelineSpec(std::span<const RGL::PipelineSpec> specs, const char* name) {
		for (const auto& spec : specs) {
			if (spec.name == name) return &spec;
		}
		return nullptr;
	}

	// "model" pipeline 规格（构造期从 pipelineSpecs 解析，按值存成员；
	// 资产上传期 addModelPipeline 用）。scene.lua 缺 "model" 条目时回落默认 SPV 路径。
	static RGL::PipelineSpec resolveModelSpec(std::span<const RGL::PipelineSpec> specs) {
		const RGL::PipelineSpec* modelSpec = findPipelineSpec(specs, "model");
		if (!modelSpec) {
			static const RGL::PipelineSpec defaultModel{
				"model", "generated_shaders/model.mesh.spv", "generated_shaders/model.frag.spv"};
			return defaultModel;
		}
		return *modelSpec;
	}

	// 构建管线表（ShaderObject 模式，无 PipelineLayout）：每条创建两个 VkShaderEXT 对象，
	// 并通过 VkShaderCreateInfoEXT.pNext 链上 VkDescriptorSetAndBindingMappingEXT
	//（binding 0 → 顶点 slot、binding 1 → 索引 slot，heapOffset = slot * descriptorSize；
	// 细节见 shader.cppm）。
	// 条目：cube（恒建——批次 3 删构造期 gating，画不画 = cube entity 有无；slot 经
	// cube 资产记录取，布局由反射生成的偏移常量钉死 vertex=0/index=1）。模型条目由
	// addModelPipeline 在资产上传期动态加入（顶点 slot 显式传入——binding mapping 是
	// shader object 级状态，无法运行时换，故按资产拆条目；批次 3 起构造期不再按
	// primitive 数预 resize，E1.3）。
	static std::vector<PipelineEntry> makePipelineTable(vk::raii::Device& device,
		std::span<const RGL::PipelineSpec> specs, RGL::Resources& res) {
		std::vector<PipelineEntry> table;
		const uint32_t descriptorSize = res.getDescriptorHeap().getDescriptorSize();

		const RGL::PipelineSpec* cubeSpec = findPipelineSpec(specs, "cube");
		if (!cubeSpec) {
			static const RGL::PipelineSpec defaultCube{
				"cube", "generated_shaders/cube.mesh.spv", "generated_shaders/cube.frag.spv"};
			cubeSpec = &defaultCube;
		}
		const auto& cubeAsset = res.getAssetTable().get(res.cubeAssetId());
		table.emplace_back(PipelineEntry{
			"cube",
			RGL::RenderPipeline(device, loadShaderEffect(*cubeSpec), descriptorSize,
				cubeAsset.heapSlot, *cubeAsset.indexHeapSlot)});

		return table;
	}

	// DIAG（RGL_OFFSCREEN）：离屏颜色目标（8-bit UNORM，与后缓冲同 extent；
	// 同 llm_debug captureFrame 的颜色图形态，VMA 自动绑内存）
	static std::expected<RGL::VmaImage, Error> createOffscreenImage(RGL::VmaAllocatorRAII& allocator,
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

	// DIAG（RGL_OFFSCREEN）：离屏目标 view 的 create info（!offscreen 时 image = null）
	VkImageViewCreateInfo buildOffscreenView() const {
		VkImageViewCreateInfo info{};
		info.image = offscreenImage.image;
		info.viewType = VK_IMAGE_VIEW_TYPE_2D;
		info.format = VK_FORMAT_R8G8B8A8_UNORM;
		info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		return info;
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
			offscreenView.emplace(vkDevice.getDevice(), buildOffscreenView());
		}
		return {};
	}

	RGL::DeviceContext& vkDevice;
	RGL::Swapchain* swapchain;
	RGL::Resources* resources;
	// DIAG（可见窗 GPUVM fault 规避 2026-08-28）：RGL_OFFSCREEN=1 → 场景渲到离屏
	// 8-bit UNORM 目标（纹理采样路径与 llm_debug 离屏同构——隐藏窗离屏路径实测无
	// fault），再 blit 到后缓冲上屏；规避可见后缓冲（compositor 在读）关联的
	// 纹理 high VA 采样 fault（现象/排查过程见 docs/heap_texture_research.md §十一）。
	// 默认关 = 原路径零变化。
	bool offscreen = false;
	// DIAG（window+texture 组合触发验证）：可见窗渲染但永不 present——KWin 永不
	// import 我们的 swapchain dmabuf。fault 在 frame 2（buffer pool 窗口内），
	// 前几帧渲染完整，判据有效
	bool noPresent = std::getenv("RGL_NO_PRESENT") != nullptr;
	// DIAG（可见窗 fault 排查）：drawFrame 帧计数——DEVICE_LOST 时打帧号，
	// 区分"帧数累积触发"（fault 帧号恒定）vs"窗口显示触发"（fault 帧号 >> showAfter）
	uint64_t frameCount = 0;
	VmaImage offscreenImage;
	// 离屏目标 view（raii 默认构造已删 + 构造期即调 vkCreateImageView——null image
	// 会被校验拦下，故不能进初始化列表；optional 惰性 emplace，默认路径零分配零调用）
	std::optional<vk::raii::ImageView> offscreenView;
	// 非 RAII 句柄（队列生命周期由 DeviceContext 的 raii Device 管理）
	vk::Queue graphicsQueue;
	const vk::raii::CommandPool& graphicsCommandPool;
	// "model" pipeline 规格（构造期 resolveModelSpec 解析，按值存；
	// addModelPipeline 创建模型条目用）
	RGL::PipelineSpec modelSpec;
	// 管线表：ShaderObject 条目（"cube" 恒建 + 每 primitive 一条 "model_<assetId>"，
	// 后者资产上传期经 addModelPipeline 加入）。cube 部分初始化列表构建
	//（makePipelineTable），见构造器注释。无 RenderPass/PipelineLayout。
	std::vector<PipelineEntry> pipelines;
	// 管线表 name → 下标（构造器体内建一次 + addModelPipeline 动态更新；
	// 渲染 system 经 pipelineIndex 解析 DrawCall 的管线条目，录制路径不做字符串查找）
	std::map<std::string, uint32_t> pipelineIndexMap;
	// 动态状态：opaque pass（默认值 = cube 的 opaque 状态），drawFrame() 每帧
	// beginRendering 后调一次 applyDynamicState（Phase 7 多 pass 时按 pass 换状态）
	RGL::RenderState opaqueState{};
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
			offscreenView.emplace(device, buildOffscreenView());
		}

	// ① 等待上一帧同 slot 完全结束：inFlight fence（命令缓冲执行完）与 present
	// fence（present 完成）都信号后，本 slot 的 semaphore / command buffer 才可重用。
	// waitAll=VK_TRUE + 无限超时（fence 保证在超时内信号，返回值忽略）
	// DIAG（RGL_NO_PRESENT）：不 present → presentFence 永不 signal，只等 inFlight
	if (noPresent) {
		(void)device.waitForFences(
			{*inFlightFences[currentFrame]}, VK_TRUE, std::numeric_limits<uint64_t>::max());
		device.resetFences({*inFlightFences[currentFrame]});
	} else {
		(void)device.waitForFences(
			{*inFlightFences[currentFrame], *presentFences[currentFrame]},
			VK_TRUE, std::numeric_limits<uint64_t>::max());
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
	// DIAG（RGL_OFFSCREEN）：离屏目标 Undefined → ColorAttachmentOptimal（同后缓冲规则）
	if (offscreen) {
		auto offscreenToDrawBarrier = RGL::createImageBarrier(
			offscreenImage.image, vk::ImageLayout::eUndefined,
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
	// 的影响，spec 层面仍必须保留，见 docs/heap_texture_research.md §12）
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT | vk::AccessFlagBits2::eSamplerHeapReadEXT;
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

	// ⑨ 设置动态状态（opaque pass，每帧一次；viewport/scissor 用当前 swapchain 宽高，
	// 细节见 shader.cppm 的 applyDynamicState）。动态状态在 draw 时采样
	//（spec chapters/shaders.adoc "Setting State"），与 bindShadersEXT 的先后无约束。
	RGL::applyDynamicState(commandBuffers[currentFrame], swapchain->getExtent().width,
		swapchain->getExtent().height, opaqueState);

	// ⑩ 绘制：遍历渲染 system 产出的绘制列表（顺序 = 列表顺序；push data 布局随管线，
	// 见 DrawCall）。每条：推绘制参数（push constant，独立于 descriptor heap，不占 slot，
	// Slang 端经 [[vk::push_constant]] 访问，spec 3.4.1；host 内存录制时读取）→
	// 绑定 ShaderObject → drawMeshTasksEXT（mesh shader 内部生成顶点/三角形，
	// 不需要传统顶点管线；group 数由渲染 system 按契约常量派生，见 renderListSystem）
	for (const auto& call : list.calls) {
		const auto& entry = pipelines[call.pipeline];
		commandBuffers[currentFrame].pushDataEXT(vk::PushDataInfoEXT(
			0, vk::HostAddressRangeConstEXT(call.pushData.data(), call.pushData.size())));
		entry.pipeline.bindShaders(commandBuffers[currentFrame]);
		commandBuffers[currentFrame].drawMeshTasksEXT(call.groupCount, 1, 1);
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
		if (logger) logger->error("Renderer: device lost on submit! (frame {})", frame);
		return std::unexpected(Error{ErrorCode::DeviceLost, "VK_ERROR_DEVICE_LOST on submit"});
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
	}

	// ⑮ 推进到下一个帧 slot（双缓冲）
	currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
	return {};
}

}  // namespace RGL
