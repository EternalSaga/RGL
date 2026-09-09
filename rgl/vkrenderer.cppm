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
import state_kit;  // Phase 10 批次 0 手术 1：pass 状态表迁出 Renderer（增改集中一处）
import gpu_cull;  // Phase 7b：GPU culling 子系统（culling compute + 读回；main 先于 Renderer 构造）
export import frame_capture;  // 手术 2：帧内回读捕获器；FrameCapture 出现在本类公开返回类型
	//（同 frame_draw_list 先例 export import，消费方只 import vk_renderer 即可见全类型）
import cull_driver;  // 手术 3：culling 帧编舞（④c 提交时序 + 双路 + 对账）迁出本类
import frame_session;  // 手术 4：帧生命周期会话（同步对象/命令缓冲/acquire/submit/present/重建）
import vk_indirect_commands;  // Phase 7b：DGC 命令生成（NVIDIA 路；main 按能力位条件构造）
import vkerror;
import diagnostics;  // no_present 诊断单例（configs/diagnostics.lua）

// 绘制契约（DrawCall/SceneFrameData/FrameDrawList）拆出为独立模块——本模块 export import
// 保持 Renderer::drawFrame 接口类型对外可见（2026-09-02 拆分）
export import frame_draw_list;

// Phase 6：渲染图类型（RgGraphDesc/CompiledRenderGraph/RgBarrierPlan…）经构造参数
// 进入本类公开接口——同 frame_draw_list 先例 export import，main 无需单独 import
export import render_graph;

namespace RGL {

/**
 * Renderer：每帧"录制 → 提交 → 显示"的驱动器 + 每帧同步状态的持有者。
 *
 * 渲染流程（一次 drawFrame(list) 调用，逐步骤注释见函数体内 ①~⑮）：
 *   等待上一帧同 slot 完全结束（双 fence）并重置
 *   → acquireNextImage 获取后缓冲（信号 imageAvailable 信号量）
 *   → 录制命令缓冲：descriptor heap barrier + scene staging 拷贝/绑定（数据通路
 *     固定段，Phase 6 起不入图）→ 逐 pass 循环（编译图 CompiledRenderGraph）：
 *     attachment barrier（布局链推导产物物化）→ beginRendering（逐 pass 附件）
 *     → applyDynamicState(pass.state) → 段内逐 draw → endRendering
 *   → swapchain PresentSrc barrier（图 presentTransition）
 *   → queueSubmit（waitStage = 图派生的 swapchain 首用阶段）
 *   → presentKHR（pNext 挂 SwapchainPresentFenceInfoEXT 信号 present fence）
 *   → currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT
 *
 * 绘制内容 = 渲染 system 产出的 FrameDrawList（本类是盲执行器，不认识具体场景资产，
 * 见 DrawCall/FrameDrawList 注释）；帧拓扑 = 构造传入的编译图（Phase 6：barrier 与
 * 布局转换由 render_graph 编译器从 pass 声明推导，执行器不再手写；现默认图 =
 * opaque/skybox/transparent 三段直渲）。核心现代特性：Dynamic Rendering（无 RenderPass）+
 * ShaderObject（无 PipelineLayout，bindShadersEXT + 动态状态）+ DescriptorHeap
 *（vkCmdBindResourceHeapEXT 取代 descriptor set，使用规范见 docs/DESCRIPTOR_HEAP_SPEC.md）。
 */
export class Renderer;

export class Renderer {
   public:
	/**
	 * 构建执行器（main.cpp 初始化层构造顺序末站：ShaderRegistry→Renderer）。
	 * 成员初始化列表顺序必须与下方声明顺序一致（RAII 规则：vk::raii::* 直接成员只能在
	 * 初始化列表初始化）。管线表不再是 Renderer 成员（Phase 4 批次 2：迁出至 ShaderRegistry，
	 * Vulkan 层零 entt）——registry 在 main 先于 Renderer 构造（需 descriptor heap +
	 * 资产表），Renderer 持引用；条目全按需建（架构债务 1：cube 不再是内核资产、条目不再
	 * 恒建，画不画 = cube entity 有无，数据驱动）。模型管线条目不在构造期建（批次 3：
	 * 管线表与场景内容解耦，E1.3）——
	 * 资产上传期经 ShaderRegistry::addModelEntry 动态加入（按 (风格, 顶点 slot, 纹理 slot)
	 * 组合去重，同组合共享一条，Phase 4 验收 392 primitive → 12 条）。
	 * renderGraph（Phase 6）：编译图由 main 启动期 compileRenderGraph 产出（批次 3 前 =
	 * defaultRenderGraph()）——构造期校验执行器可解析性（validateRenderGraphForExecutor：
	 * swapchain/depth 各恰一次 / state 与 drawSource 名字 / blit 两端齐 / drawLight state
	 * 必 "light"），不可解析 = 启动 fail-fast，录制期零图错误。
	 * @param vkDevice 设备层（队列 / 命令池 / fence 等待 / 能力探测）
	 * @param vkSwapchain 交换链层（acquire / recreate / extent）
	 * @param vkResources 资源层（descriptor heap / 深度图 / scene data / offscreen RT）
	 * @param shaderRegistry 管线表（条目查找 + bindShaders；main 先于 Renderer 构造）
	 * @param renderGraph 编译渲染图（构造后不变；不可解析启动期抛出）
	 * @param gpuCull GPU culling 子系统（Phase 7b；main 先于 Renderer 构造。inactive
	 *   （候选表空）时 drawFrame 全路径不碰 culling = 行为中性）
	 * @param dgc DGC 命令生成子系统（Phase 7b NVIDIA 路；main 仅 graphicsIndirect=true
	 *   时构造，否则 null → opaque 段走读回重放）
	 */
	Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::ShaderRegistry& shaderRegistry, RGL::CompiledRenderGraph renderGraph,
		RGL::GpuCull* gpuCull, RGL::DgcCommands* dgc);
	/** @return 交换链层引用（extent / recreate / 后缓冲）。 */
	RGL::Swapchain& getSwapchainRef() const { return *swapchain; }
	/** @return 资源层引用（深度图 / descriptor heap / RT 缓存 / scene data）。 */
	RGL::Resources& getResourcesRef() const { return *resources; }
	/**
	 * 帧循环侧（ecs texel 金丝雀）用的只读访问器（截图路径已改帧内回读，不再需要
	 * 管线/状态访问器——2026-09-08 离屏重渲器退役）。
	 * @return 设备层引用（队列 / 命令池 / fence）
	 */
	RGL::DeviceContext& getDeviceContextRef() const { return vkDevice; }
	/** @return 图形命令池 const 引用（ecs 金丝雀录制路径）。 */
	const vk::raii::CommandPool& getGraphicsCommandPoolRef() const { return graphicsCommandPool; }
	/** 等待 GPU 空闲（resize / 重建 swapchain 前调用，确保旧资源不再被使用）。 */
	void waitForIdle() { vkDevice.getDevice().waitIdle(); }
	/**
	 * 渲染一帧：整体流程见类头注释；绘制内容 = 渲染 system 产出的 FrameDrawList
	 *（盲执行器，不认识具体场景资产）；失败（device lost / fence 等待 / acquire /
	 * submit / RT 创建等）时返回错误。
	 * @param list 每帧绘制列表（calls + 段边界 + scene data，见 FrameDrawList）
	 * @return void；失败 → Error
	 */
	[[nodiscard]] std::expected<void, Error> drawFrame(const FrameDrawList& list);

	// ===== 帧内捕获（2026-09-10 Phase 10 批次 0 手术 2：状态与实现迁 frame_capture
	// 模块 FrameCapturer，此处保留原签名转发——llm_debug/ecs 消费面零改动）=====
	/** llm_debug 消费的类型别名（原 Renderer 嵌套类型；定义见 frame_capture）。 */
	using FrameCapture = RGL::FrameCapture;
	/**
	 * drawFrame 前调用：请求本帧回读捕获（转发 FrameCapturer::request）。
	 * @param gbufChannel -1 = swapchain 后缓冲；>=0 = G-buffer 通道号（dump_gbuf）
	 */
	void requestFrameCapture(int gbufChannel = -1) { capturer.request(gbufChannel); }
	/**
	 * drawFrame 后调用（fence 已在收口阶段等待）。
	 * @return 本帧捕获产物；空 = 本帧无捕获产物
	 */
	std::optional<FrameCapture> takeFrameCapture() { return capturer.take(); }
	/**
	 * 本帧是否已产出捕获（acquire 超时跳帧时请求保持 pending，据此决定重试而非误读空）。
	 * @return true = 捕获产物就绪（takeFrameCapture 可取）
	 */
	[[nodiscard]] bool hasFrameCapture() const { return capturer.has(); }

   private:
	/**
	 * scene data staging ring（Phase 5 批次 2，全项目首个每帧动态 GPU 数据）：
	 * ×FrameSession::kFramesInFlight 常驻映射 host buffer。写安全 = 帧 slot 复用规则——
	 * drawFrame ① 已 wait 本 currentFrame slot 的 inFlight fence（f-2 帧命令跑完）
	 * 才写本 slot staging；录制期 vkCmdCopyBuffer 拷进 scene data device buffer
	 *（命令流内天然有序），描述符静态指向该 buffer 永不改动。
	 * @param vkResources 资源层（VMA 分配器）
	 */
	void createSceneStaging(RGL::Resources& vkResources);
	/**
	 * ③.5 scene data 上传（Phase 5 批次 2，全项目首个每帧动态 GPU 数据）：CPU 写
	 * staging[frameSlot] 头 128B（光参数@0 / env@48 / skyInvViewProj@64）+ 对象矩阵表
	 * @128 + 多灯数组尾区@262272（Phase 7 D3）。本 slot fence 已在 ① wait（f-2 帧命令
	 * 跑完，staging 不再被读），写安全；录制期 vkCmdCopyBuffer 拷进 device buffer。
	 * 容量契约（objectMatrices ≤ kSceneMatrixCapacity、lightCount ≤ kMaxLights）由 main
	 * 启动期检查，此处防御性再校验。
	 * @param list 每帧绘制列表（sceneData：light/envParams/skyInvViewProj/objectMatrices/灯数组）
	 * @param frameSlot 本帧 staging slot 下标
	 * @return void；超容量 → Error（Phase 5/7 契约，main 启动检查被绕过的兜底）
	 */
	[[nodiscard]] std::expected<void, Error> writeSceneStaging(const FrameDrawList& list,
		uint32_t frameSlot);
	// recreateSwapchainAndDepth（swapchain+depth 重建）已随手术 4 迁 FrameSession。
	RGL::DeviceContext& vkDevice;  // 设备层引用（队列 / 命令池 / fence 等待 / 能力探测）
	RGL::Swapchain* swapchain;      // 交换链层（main 持有，析构序在 Renderer 之后）
	RGL::Resources* resources;      // 资源层（main 持有，析构序在 Renderer 之后）
	// 管线表（Phase 4 批次 2：迁出至 ShaderRegistry，零 entt；main 先于 Renderer 构造并持引用，
	// 析构序 Renderer 先于 registry 安全）
	RGL::ShaderRegistry& registry;
	// Phase 6：编译渲染图（构造传入，main 启动期 compileRenderGraph 产出，之后不变）
	RGL::CompiledRenderGraph renderGraph;
	// Phase 7b：GPU culling 子系统（main 持有，构造序先于 Renderer → 析构序其后安全）；
	// Phase 10 批次 0 手术 3：④c 提交时序 + opaque 段双路 + 对账全迁 CullDriver
	//（本类构造期以 main 传入的裸指针建 driver；inactive = 全路径不碰 culling 语义不变）
	RGL::CullDriver cullDriver;
	// attachment 真实布局追踪（按图 slot 索引，跨帧持久）：帧首 barrier 的 oldLayout
	// 来源（plan.oldLayoutDiscard=false 者用本值；swapchain/depth 走帧首丢弃规则
	// 不查询）；录制期随每条物化 barrier 更新为 plan.newLayout。
	//（Phase 4 前 DIAG RGL_OFFSCREEN 的 offscreenLayout 单变量泛化为逐 slot 表）
	std::vector<vk::ImageLayout> attachmentLayouts;
	// 帧内捕获（Phase 10 批次 0 手术 2）：请求状态 + 回读缓冲 + record/collect 三步曲
	// 全迁 RGL::FrameCapturer（frame_capture 模块，构造期自图定位 swapchain slot）。
	// 声明位置 = 原 capture 成员块（attachmentLayouts 之后、stateKit 之前），
	// ctor 初始化列表顺序与之对应（-Wreorder 同步）。
	RGL::FrameCapturer capturer;
	// edS3：depth clip enable 动态状态能力门 + 四段 RenderState 值集合 + 状态名解析
	//（Phase 10 批次 0 手术 1：状态表迁出至 StateKit——pass.state 词表与状态值的唯一
	// 持有者；Phase 10 阴影新增状态集中改该模块一处）
	RGL::StateKit stateKit;
	// 帧生命周期会话（Phase 10 批次 0 手术 4：①②③/⑬⑭⑮ + 四套同步对象 + 每帧命令
	// 缓冲 + swapchain/depth 重建迁 FrameSession；本类录制期经 commandBuffer()/
	// currentInFlightFence()/currentFrameSlot()/imageIndex() 消费）。声明序在 stateKit 后
	// （ctor 初始化列表同序：session 依赖 vkDevice/swapchain/resources，均在更前）。
	RGL::FrameSession session;
	const vk::raii::CommandPool& graphicsCommandPool;  // 图形命令池（DeviceContext 持有，金丝雀录制路径用）
	// Phase 7 D1：light 管线条目下标（构造期从 registry 取 "light"；图含 drawLight pass 时必填）
	std::optional<uint32_t> lightPipelineIdx;
	// descriptor heap 的 GPU 地址（VmaDescriptorHeap 管理的"描述符槽位数组"，
	// 对齐细节见 VmaBuffer.cppm）
	vk::DeviceAddress heapAddress = 0;

	// scene data staging ring（Phase 5 批次 2，见 createSceneStaging；slot 数 =
	// FrameSession::kFramesInFlight，与帧 slot 一一对应）
	std::array<RGL::VmaBuffer, FrameSession::kFramesInFlight> sceneStaging;
	// 双缓冲游标 / 帧计数 / noPresent / 四套同步对象 / 每帧命令缓冲（原
	// MAX_FRAMES_IN_FLIGHT/currentFrame/frameCount/noPresent/graphicsQueue/
	// imageAvailableSemaphores/renderFinishedSemaphores/inFlightFences/presentFences/
	// commandBuffers）已随手术 4 迁 FrameSession 私有成员。
};

}  // namespace RGL
