module;

export module vk_renderer;

import rlstd;
import rlvk;
import rllogger;

import vulkan_utils;
import vulkan_window;
import vk_device;
import vk_swapchain;
import vk_resources;
import VmaRaii;
import shader_registry;  // Phase 4 批次 2：管线表迁出（PipelineEntry/ShaderRegistry，零 entt）
import shader;
import vkerror;

// 绘制契约（DrawCall/SceneFrameData/FrameDrawList）拆出为独立模块——本模块 export import
// 保持 Renderer::drawFrame 接口类型对外可见（2026-09-02 拆分）
export import frame_draw_list;

// Phase 6：渲染图类型（RgGraphDesc/CompiledRenderGraph/RgBarrierPlan…）经构造参数
// 进入本类公开接口——同 frame_draw_list 先例 export import，main 无需单独 import
export import render_graph;

namespace RGL {

// Renderer：每帧"录制 → 提交 → 显示"的驱动器 + 每帧同步状态的持有者。
//
// 渲染流程（一次 drawFrame(list) 调用，逐步骤注释见函数体内 ①~⑮）：
//   等待上一帧同 slot 完全结束（双 fence）并重置
//   → acquireNextImage 获取后缓冲（信号 imageAvailable 信号量）
//   → 录制命令缓冲：descriptor heap barrier + scene staging 拷贝/绑定（数据通路
//     固定段，Phase 6 起不入图）→ 逐 pass 循环（编译图 CompiledRenderGraph）：
//     attachment barrier（布局链推导产物物化）→ beginRendering（逐 pass 附件）
//     → applyDynamicState(pass.state) → 段内逐 draw → endRendering
//   → swapchain PresentSrc barrier（图 presentTransition）
//   → queueSubmit（waitStage = 图派生的 swapchain 首用阶段）
//   → presentKHR（pNext 挂 SwapchainPresentFenceInfoEXT 信号 present fence）
//   → currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT
//
// 绘制内容 = 渲染 system 产出的 FrameDrawList（本类是盲执行器，不认识具体场景资产，
// 见 DrawCall/FrameDrawList 注释）；帧拓扑 = 构造传入的编译图（Phase 6：barrier 与
// 布局转换由 render_graph 编译器从 pass 声明推导，执行器不再手写；现默认图 =
// opaque/skybox/transparent 三段直渲）。核心现代特性：Dynamic Rendering（无 RenderPass）+
// ShaderObject（无 PipelineLayout，bindShadersEXT + 动态状态）+ DescriptorHeap
//（vkCmdBindResourceHeapEXT 取代 descriptor set，使用规范见 docs/DESCRIPTOR_HEAP_SPEC.md）。

export class Renderer;

export class Renderer {
   public:
	// 成员初始化列表顺序必须与下方声明顺序一致（RAII 规则：vk::raii::* 直接成员只能在
	// 初始化列表初始化）。管线表不再是 Renderer 成员（Phase 4 批次 2：迁出至 ShaderRegistry，
	// Vulkan 层零 entt）——registry 在 main 先于 Renderer 构造（需 Resources 的 cube 资产
	// + descriptor heap），Renderer 持引用；"cube" 条目恒建（批次 3：画不画 = cube entity
	// 有无，数据驱动，showCube 配置由 main 决定是否注册 entity；条目多建 2 个 shader object，
	// 成本可忽略）。模型管线条目不在构造期建（批次 3：管线表与场景内容解耦，E1.3）——
	// 资产上传期经 ShaderRegistry::addModelEntry 动态加入（按 (风格, 顶点 slot, 纹理 slot)
	// 组合去重，同组合共享一条，Phase 4 验收 392 primitive → 12 条）。
	// renderGraph（Phase 6）：编译图由 main 启动期 compileRenderGraph 产出（批次 3 前 =
	// defaultRenderGraph()）——构造期校验执行器可解析性（attachment kind / state 与
	// drawSource 名字 / blit 暂未支持），不可解析 = 启动 fail-fast，录制期零图错误。
	Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::ShaderRegistry& shaderRegistry, RGL::CompiledRenderGraph renderGraph);
	RGL::Swapchain& getSwapchainRef() const { return *swapchain; }
	RGL::Resources& getResourcesRef() const { return *resources; }
	// 离屏截图（llm_debug.cppm captureFrame）用的只读访问器
	RGL::DeviceContext& getDeviceContextRef() const { return vkDevice; }
	const vk::raii::CommandPool& getGraphicsCommandPoolRef() const { return graphicsCommandPool; }
	// 离屏截图（llm_debug.cppm captureFrame）用的只读访问器：委托管线表（ShaderRegistry）。
	// entry 访问器供截图路径取 pushDataSet（与主路径 drawCall 同一契约字段，不硬编码 0——
	// push set 显式化契约（L5）两条路径必须同步，否则 pushDataSet 离开 0 时截图静默分歧）
	const RGL::PipelineEntry& pipelineEntryAt(uint32_t index) const { return registry.entry(index); }
	const RGL::RenderState& getOpaqueState() const { return opaqueState; }
	const RGL::RenderState& getTransparentState() const { return transparentState; }
	// edS3：depth clip 动态状态能力（llm_debug 离屏重渲消费，同主路径同一门，避免两路分歧）
	[[nodiscard]] bool isDepthClipDynamic() const { return depthClipDynamic; }
	const RGL::RenderState& getSkyboxState() const { return skyboxState; }
	// 等待 GPU 空闲（resize / 重建 swapchain 前调用，确保旧资源不再被使用）
	void waitForIdle() { vkDevice.getDevice().waitIdle(); }
	// 渲染一帧：整体流程见类头注释；绘制内容 = 渲染 system 产出的 FrameDrawList
	//（盲执行器，不认识具体场景资产）；失败（device lost 等）时返回错误
	[[nodiscard]] std::expected<void, Error> drawFrame(const FrameDrawList& list);

   private:
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
	void createSyncObjects();
	// 批量分配 MAX_FRAMES_IN_FLIGHT 个 primary command buffer（每帧 slot 一个，
	// 每帧 reset() 后重新录制）
	void allocateCommandBuffers();
	// scene data staging ring（Phase 5 批次 2，全项目首个每帧动态 GPU 数据）：
	// ×MAX_FRAMES_IN_FLIGHT 常驻映射 host buffer。写安全 = 帧 slot 复用规则——
	// drawFrame ① 已 wait 本 currentFrame slot 的 inFlight fence（f-2 帧命令跑完）
	// 才写本 slot staging；录制期 vkCmdCopyBuffer 拷进 scene data device buffer
	//（命令流内天然有序），描述符静态指向该 buffer 永不改动。
	void createSceneStaging(RGL::Resources& vkResources);
	// 窗口尺寸变化（resize）或 acquire/present 返回 out-of-date 时重建 swapchain + 深度图。
	// 深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。
	[[nodiscard]] std::expected<void, Error> recreateSwapchainAndDepth(const char* context);
	RGL::DeviceContext& vkDevice;
	RGL::Swapchain* swapchain;
	RGL::Resources* resources;
	// 管线表（Phase 4 批次 2：迁出至 ShaderRegistry，零 entt；main 先于 Renderer 构造并持引用，
	// 析构序 Renderer 先于 registry 安全）
	RGL::ShaderRegistry& registry;
	// Phase 6：编译渲染图（构造传入，main 启动期 compileRenderGraph 产出，之后不变）
	RGL::CompiledRenderGraph renderGraph;
	// attachment 真实布局追踪（按图 slot 索引，跨帧持久）：帧首 barrier 的 oldLayout
	// 来源（plan.oldLayoutDiscard=false 者用本值；swapchain/depth 走帧首丢弃规则
	// 不查询）；录制期随每条物化 barrier 更新为 plan.newLayout。
	//（Phase 4 前 DIAG RGL_OFFSCREEN 的 offscreenLayout 单变量泛化为逐 slot 表）
	std::vector<vk::ImageLayout> attachmentLayouts;
	// edS3：depth clip enable 动态状态能力门（vkCmdSetDepthClipEnableEXT；启动期一次探测，
	// 未支持 = false，applyDynamicState 内 no-op 不 set——"扩展缺失干净降级"原则）
	bool depthClipDynamic = false;
	// DIAG（window+texture 组合触发验证）：可见窗渲染但永不 present——KWin 永不
	// import 我们的 swapchain dmabuf。fault 在 frame 2（buffer pool 窗口内），
	// 前几帧渲染完整，判据有效
	bool noPresent = std::getenv("RGL_NO_PRESENT") != nullptr;
	// DIAG（可见窗 fault 排查）：drawFrame 帧计数——DEVICE_LOST 时打帧号，
	// 区分"帧数累积触发"（fault 帧号恒定）vs"窗口显示触发"（fault 帧号 >> showAfter）
	uint64_t frameCount = 0;
	// 非 RAII 句柄（队列生命周期由 DeviceContext 的 raii Device 管理）
	vk::Queue graphicsQueue;
	const vk::raii::CommandPool& graphicsCommandPool;
	// 动态状态：opaque pass（默认值 = cube 的 opaque 状态），drawFrame() 在 opaque 段首
	// applyDynamicState 一次（Phase 7 多 pass 时按 pass 换状态）
	RGL::RenderState opaqueState{};
	// 动态状态：transparent pass（Phase 5 透明排序）——不写深度 + 开混合（factors 在
	// applyDynamicState 内接线），drawFrame() 在 transparent 段首 applyDynamicState 一次
	RGL::RenderState transparentState{};
	// 动态状态：skybox pass（Phase 5.5 批次 4）——不写深度 + LEQUAL（大三角 z=1 只在
	// 空像素通过），drawFrame() 在 skybox 段首 applyDynamicState 一次
	RGL::RenderState skyboxState{};
	// 图 pass.state 名 → 动态状态（构造期校验保证图内名字全部可解析，未知名 = 校验
	// 遗漏的编程错误，抛出）
	const RGL::RenderState& stateFor(std::string_view name) const;
	// descriptor heap 的 GPU 地址（VmaDescriptorHeap 管理的"描述符槽位数组"，
	// 对齐细节见 VmaBuffer.cppm）
	vk::DeviceAddress heapAddress = 0;

	// 双缓冲：最多 2 帧同时在飞——当前帧在录制 + 上一帧还在 GPU 执行。
	// 下方同步对象数组按 currentFrame 索引，fence 信号后才可重用（双 fence 设计
	// 见 createSyncObjects 注释）
	static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
	uint32_t currentFrame = 0;
	// scene data staging ring（Phase 5 批次 2，见 createSceneStaging）
	std::array<RGL::VmaBuffer, MAX_FRAMES_IN_FLIGHT> sceneStaging;
	std::vector<vk::raii::Semaphore> imageAvailableSemaphores;
	std::vector<vk::raii::Semaphore> renderFinishedSemaphores;
	std::vector<vk::raii::Fence> inFlightFences;
	std::vector<vk::raii::Fence> presentFences;
	std::vector<vk::raii::CommandBuffer> commandBuffers;
};

}  // namespace RGL
