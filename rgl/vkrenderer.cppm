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
import scene_data;  // SceneDataService（架构债务续账步骤 1：scene data buffer 移出内核）
import environment;  // Environment（架构债务续账步骤 3：env 单例移出内核，经访问器给 ecs）
import render_target_cache;  // RenderTargetCache/RtAttachment（架构债务续账步骤 2：RT 缓存移出内核）
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
import picking;  // Phase 10 批 10.5：GPU id 点选服务（main 门控构造，Renderer 只挂指针）
import izb_shadow;  // 批 11.3-1d：FTIZB 硬阴影 IZB 数据层（门控构造；门关 = 指针恒空）
import evssm_shadow;  // 批 11.6d-2：EVSSM 矩图数据层（同门控模式；门关 = 指针恒空）
import vkerror;
import diagnostics;  // no_present 诊断单例（configs/diagnostics.lua）
import fsr_upscaler;   // 批 14.5：FSR 3.1.4 upscaler（图外 compute，门控构造）
import meshlet_cull_service;  // ★1-a2-i：逐簇视锥剔除（图外 compute，装配后挂裸指针）
import offgraph_contract;  // 批次 B1：图外工作申报面（挂点/相位表；零设备调用，判据在 tests/）
import offgraph_sync;      // 批次 B1b：图外同步发射账（barrier 由 usage 词表物化 + 帧末次数对账）
import dynamic_render_graph;  // B2-4③′（D5 第一刀）：脏驱动的图派生件（安全点重编译 + 按名结转）
import pending_evictions;     // B2-4③′ 债-124：RT 退役台账（frames-in-flight 到期销毁）
import meshlet_shadow_args;   // ★M-1b：真 MDI 行表载体（upload + 间接 buffer，装配后挂裸指针）
import mdi_rows;              // ★M-1b：kMdiShadowRowStride（drawMeshTasksIndirectEXT 的 stride 唯一真相源）

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
 * 布局转换由 render_graph 编译器从 pass 声明推导，执行器不再手写；现内建图 =
 * deferred geometry→light + ao + tonemap；forward 三段直渲已随刀②.2 删除）。核心现代特性：
 * Dynamic Rendering（无 RenderPass）+
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
	 * graphDesc（B2-4③′，D5 第一刀）：图描述本体 move 进本类的 DynamicRenderGraph——
	 * 图自此是**脏驱动派生件**：资源事实在运行期变（IZB 首建 / extent 漂移）→ 标脏 →
	 * 下个安全点（beginFrame 围栏后、任何录制前）重编译，逐 slot 跨帧表按名结转。
	 * renderGraph 仍是首编译产物（main 已校验过，本类不重复首编译）；重编译次数出厂档
	 * 判据 = 0（日志 "DynamicGraph: recompiled" 打点，IZB 开档 = 1）。
	 * @param vkDevice 设备层（队列 / 命令池 / fence 等待 / 能力探测）
	 * @param vkSwapchain 交换链层（acquire / recreate / extent）
	 * @param vkResources 资源层（descriptor heap / 深度图 / offscreen RT）
	 * @param sceneData scene data 服务（staging 拷贝目标 buffer，架构债务续账步骤 1）
	 * @param env 环境服务（ecs renderListSystem 经 getEnvironmentRef 读，架构债务续账步骤 3）
	 * @param rtCache offscreen RT 缓存（drawFrame 每帧 ensure，架构债务续账步骤 2）
	 * @param shaderRegistry 管线表（条目查找 + bindShaders；main 先于 Renderer 构造）
	 * @param renderGraph 编译渲染图（首编译产物；重编译经 graph 派生后按名结转替换）
	 * @param graphDesc 图描述（move 进 DynamicRenderGraph；重编译的真相源）
	 * @param gpuCull GPU culling 子系统（Phase 7b；main 先于 Renderer 构造。inactive
	 *   （候选表空）时 drawFrame 全路径不碰 culling = 行为中性）
	 * @param dgc DGC 命令生成子系统（Phase 7b NVIDIA 路；main 仅 graphicsIndirect=true
	 *   时构造，否则 null → 看 dgcStatic / 读回重放）
	 * @param dgcStatic 静态 shader DGC 路（RADV spike，docs/archive/radv_static_dgc_spike.md；
	 *   main 按 staticGraphicsIndirect + diagnostics.dgcStatic 条件构造，null = 关）
	 * @param highlightPlan 批 B2-5：highlight 装饰**实际落图的那张申报单**（门没开 / 图形态
	 *   不支持 = `applicable = false`，本参数整体空转）。非空即登记为「可动态撤回」——
	 *   原值只在装饰前问得到，所以必须由装配层带进来，不能在 Renderer 里重算。
	 */
	Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::SceneDataService& sceneData, RGL::Environment& env, RGL::RenderTargetCache& rtCache,
		RGL::ShaderRegistry& shaderRegistry, RGL::CompiledRenderGraph renderGraph,
		RGL::RgGraphDesc graphDesc,
		RGL::GpuCull* gpuCull, RGL::DgcCommands* dgc, RGL::DgcStaticCommands* dgcStatic = nullptr,
		bool fsrEnabled = false, bool fsrTemporal = false,
		RGL::RgHighlightPlan highlightPlan = {});
	/** @return 交换链层引用（extent / recreate / 后缓冲）。 */
	RGL::Swapchain& getSwapchainRef() const { return *swapchain; }
	/** @return 资源层引用（深度图 / descriptor heap / RT 缓存）。 */
	RGL::Resources& getResourcesRef() const { return *resources; }
	/** @return 环境服务引用（ecs renderListSystem 每帧读 irradianceSH，架构债务续账步骤 3）。 */
	RGL::Environment& getEnvironmentRef() const { return *env; }
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
	 * drawFrame 前调用：请求**按附件名**回读（转发 FrameCapturer::requestAttachment）。
	 * Phase 12 批 12.2 为 QA 层加的出口——目前唯一消费方是 `object_id` 全分辨率
	 * dump（picking 的 1-texel 回读不够图像断言用）。与 requestFrameCapture(int)
	 * 互斥且覆盖：谁后调谁生效。
	 * @param name 编译后图里的附件名（如 `kObjectIdAttachmentName`）
	 */
	void requestFrameCapture(std::string_view name) { capturer.requestAttachment(name); }
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

	/**
	 * 捕获请求是否**注定失败**（转发 `FrameCapturer::hasFailure`，动机见其注释）。
	 *
	 * 消费方（RPC `/shot`、`/shotburst`）必须在「没产物就下帧重试」之前先问这个：
	 * 只问 `hasFrameCapture()` 的循环在「回读源永不存在」时不会结束，于是跨帧响应
	 * 句柄永不复位 ⟹ 之后每个截图请求都吃 409，**一次拼错附件名就把端点打死到重启**。
	 * @return true = 有失败待取（此时 hasFrameCapture() 恒 false）
	 */
	[[nodiscard]] bool hasFrameCaptureFailure() const { return capturer.hasFailure(); }

	/**
	 * 取走捕获失败原因（取后即清，一条失败只兑现一个请求）。
	 * @return 可直接进 400 应答体的原因；空 = 无失败
	 */
	std::optional<std::string> takeFrameCaptureFailure() { return capturer.takeFailure(); }

	/**
	 * 帧末扩展段 hook（Phase 10 调试层：imgui 面板；main 装配，缺省空）。
	 * 调用点 = drawFrame ⑪.5 帧内捕获之后、⑫ presentTransition barrier 之前，
	 * 参数 = 录制中的命令缓冲 + swapchain extent + 本帧后缓冲 view。
	 * **契约**：调用 hook 前执行器已把 swapchain 推进到 COLOR_ATTACHMENT_OPTIMAL
	 * （含布局追踪表更新）；hook 内以 beginRendering(load) 叠加绘制、结束回
	 * 非渲染态即可——presentTransition barrier 的 oldLayout 派生自追踪表自动衔接。
	 * 空 hook = 零调用零 barrier（正常路径逐位不变）。返回值当前仅留观测语义。
	 */
	void setFrameEndHook(std::function<bool(vk::raii::CommandBuffer&, vk::Extent2D, vk::ImageView)> hook) {
		frameEndHook = std::move(hook);
	}

	/**
	 * picking 服务挂点（Phase 10 批 10.5，D3；main 门控装配 = diagnostics.picking，
	 * 缺省 nullptr = 零开销）。非空时执行器：picking pass 走克隆管线重放分支
	 *（executePassDraws）+ 帧末 record（capturer.record 后）+ submit 后 collect。
	 * 生命周期 = 装配方（main）持有，须活过 Renderer。
	 * @param svc 服务指针（nullptr = 解除）
	 */
	void setPickingService(PickingService* svc) { pickingService = svc; }

	/**
	 * 簇化轴 ★0.5-b2：共面双臂的叠加段深度比较覆写（main 在 assembleBackEnd 之后装配，
	 * 接线 = `wireMeshletOverlayDepthCompare`；同 setPickingService 的「装配后挂」惯例）。
	 *
	 * 🔴 nullopt（默认 = coplanar off / 门关）⟹ `executeMeshletOverlay` **一条
	 * setDepthCompareOp 都不发** ⟹ 门关与 off 档的动态状态序列逐 bit 不变（A1 判据的
	 * 构造性保证）。eEqual/eNotEqual = 只作用叠加那批 draw（loop 前 set、loop 后复原
	 * pass 原值），pass 与旧路径的状态不许动。
	 * @param compareOp nullopt = off；否则 = 叠加 draw 携带的深度比较
	 */
	void setMeshletOverlayDepthCompareOp(std::optional<vk::CompareOp> compareOp) {
		meshletOverlayDepthCompareOp = compareOp;
	}

	/**
	 * 簇化轴 ★1-a2-i：逐簇视锥剔除服务（main 在 assembleBackEnd 之后装配，接线 =
	 * `wireMeshletCull`；同 setMeshletOverlayDepthCompareOp 的「装配后挂」惯例）。
	 *
	 * 🔴 nullptr（默认 = 门关 / 服务未装配）⟹ 帧首**一条命令都不发**（连 barrier 都不发）。
	 * 「门关逐位不变」的构造性保证在这里：判据是「有没有这个指针」，不是「dispatch 了但标志
	 * 全是 1」。门开时它在每帧 `recordFixedDataPath` 之后、pass 循环之前录一次 compute
	 * dispatch（见 `recordMeshletCullDispatch`）。
	 * @param service 剔除服务（装配层持有所有权；本类只持裸指针，声明序保证它比 Renderer 长寿）
	 */
	void setMeshletCullService(MeshletCullService* service) {
		meshletCull = service;
		// B2-4④′：服务在场 = 「标志表被图内 mesh draw 读」成为事实 → 把这份共享声明进图
		// （生产者 + 消费者双声明 + 附件↔边对账，见 syncMeshletGraphAttachments）。
		// 门关（nullptr）= 零声明零日志，图里永远没有这个附件（逐位不变的构造性保证）。
		if (service)
			syncMeshletGraphAttachments();
	}

	/**
	 * 簇化轴 ★M-1b：真 MDI 行表载体（main 在 assembleBackEnd 之后挂，同 setMeshletCullService
	 * 的「装配后挂」惯例；所有权在装配层 meshletAssembly，声明序先于 Renderer）。
	 *
	 * 🔴 本指针**不是**生效档判据：mdi_shadow=false 的出厂态载体照建、指针照挂（宿主条件见
	 * `wireMeshletShadowArgs`），生效与否的唯一事实 = `FrameDrawList::mdiSegments` 是否非空
	 * （产地 = 采集侧读 registry ctx 的 `MeshletMdiShadowActivation`）。空段 ⟹
	 * `prepareMdiShadowRows` 首行早退 = 零 upload 零 barrier 零新命令（逐位不变的构造性保证）。
	 * @param service 行表载体（nullptr = 宿主条件不成立；此时采集侧必零段，指针无人解引用）
	 */
	void setMeshletShadowArgsService(MeshletShadowArgsService* service) {
		mdiShadowArgs = service;
	}

	/**
	 * ★M-1b：本帧录制槽号（ecs 帧循环在 drawFrame **之前**调，传给 `renderListSystem` 做
	 * MDI 同源 pair 的一半输入）。slot 只在 drawFrame 末尾 advance（FrameSession::advance）
	 * ⟹ 采集期与录制期取到同一个数；执行器 `prepareMdiShadowRows` 逐段核对失配即抛。
	 * @return 本帧槽（< FrameSession::kFramesInFlight）
	 */
	[[nodiscard]] uint32_t currentFrameSlot() const { return session.currentFrameSlot(); }

	/**
	 * 编译渲染图只读访问（Phase 10 批 10.5：PickingService 构造需附件表定位 object_id
	 * slot；同 getSwapchainRef 惯例）。
	 * @return 图 const 引用
	 */
	[[nodiscard]] const RGL::CompiledRenderGraph& getRenderGraphRef() const { return renderGraph; }

	/**
	 * 图声明面只读访问（能力观测批 1：`/state` 能力段取脏位；同 getRenderGraphRef 惯例）。
	 * ⚠ 名单类内容**不从这条访问取**——能力段吃的是 getRenderGraphRef() 的编译产物，
	 * 这里只问「声明侧有没有变更还没落图」（`docs/能力观测_plan.md` §3.1）。
	 * @return DynamicRenderGraph const 引用
	 */
	[[nodiscard]] const RGL::DynamicRenderGraph& dynamicGraphRef() const { return graph; }

	/**
	 * RT 缓存只读访问（能力观测批 1：能力段按图逐个问 `isBuilt`；同惯例）。
	 * 方向约束：调用方**遍历编译图的附件来问缓存**，缓存不反向提供名单（§3.3）。
	 * @return RenderTargetCache const 引用
	 */
	[[nodiscard]] const RGL::RenderTargetCache& getRtCacheRef() const { return *rtCache; }

	// ── 批次 B2-5：pass 级动态开关（D5 收官一刀；定案 → docs/compute_调研.md §6 D5）─────
	/**
	 * 收单「把这个 pass 插上 / 拔掉」（B2-5；RPC 帧边界调，**不改图**）。
	 *
	 * 为什么是收单而不是直接改：改图是**复合事务**（pass 进出 + 挂在它上面的图外申报 +
	 * 相位表重推），必须整体落在安全点（围栏后、录制前），否则就是「半张图 + 旧相位表」
	 * 那种录制期才炸的形态。收单让调用方（HTTP 线程 / 帧边界钩子）与生效点解耦，
	 * 延迟 ≤1 帧（宪法口径：正确性 + ≤1 帧延迟，逐帧切换是**测试模态**不是 SLA）。
	 *
	 * 幂等：同一意图重复收单 = 最后一次说了算；意图与现状相同时 `applyPassToggles` 空转。
	 * @param passName 可开关的 pass 名（必须是装配期登记过的那一个，否则出声）
	 * @param on       true = 在场；false = 撤回
	 */
	void requestPassToggle(std::string_view passName, bool on) {
		pendingPassToggle = PassToggleRequest { .passName = std::string { passName }, .on = on };
	}

	/**
	 * pass 开关的观测面（RPC `/state` 的 graph 段；判据「开关真的发生了」读这里，
	 * 而不是数截图——highlight 无选中时零 draw，画面本来就不该变）。
	 */
	struct GraphStatus {
		uint32_t passCount = 0;             ///< 当前 desc 里的 pass 数（插拔的直接读数）
		std::vector<std::string> dynamicPasses;  ///< 在册可开关的 pass 名（含已撤回的）
		std::vector<std::string> presentPasses;  ///< 当前在场的 pass 名（顺序 = 执行序）
		uint32_t recompiles = 0;            ///< 累计重编译次数（出厂档判据 = 0）
		uint64_t lastRecompileNs = 0;       ///< 最近一次重编译耗时（ns；0 = 还没重编译过）
	};

	/**
	 * @return pass 开关观测面快照（现取，不存第二份真相源）
	 */
	[[nodiscard]] GraphStatus graphStatus() const;

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
	 * @128 + 多灯数组尾区@262656（Phase 7 D3）。本 slot fence 已在 ① wait（f-2 帧命令
	 * 跑完，staging 不再被读），写安全；录制期 vkCmdCopyBuffer 拷进 device buffer。
	 * 容量契约（objectMatrices ≤ kSceneMatrixCapacity、lightCount ≤ kMaxLights）由 main
	 * 启动期检查，此处防御性再校验。
	 * @param list 每帧绘制列表（sceneData：light/envParams/skyInvViewProj/objectMatrices/灯数组）
	 * @param frameSlot 本帧 staging slot 下标
	 * @return void；超容量 → Error（Phase 5/7 契约，main 启动检查被绕过的兜底）
	 */
	[[nodiscard]] std::expected<void, Error> writeSceneStaging(const FrameDrawList& list,
		uint32_t frameSlot);
	/**
	 * ④⑥⑥.5⑦ 数据通路固定段（Phase 6 起不入图——非 attachment 状态同步）：heap barrier
	 *（host write → resource/sampler heap read；eHost stage 不能在 render pass 内，故必须在
	 * beginRendering 之前）→ scene staging → device buffer 拷贝 + Transfer 写→图形 shader 读
	 * barrier → resource heap / sampler heap 绑定。手术 6：块体自 drawFrame 摘出，命令流
	 * 逐条不变。
	 * @param cmd 本帧命令缓冲（录制期）
	 * @param list 每帧绘制列表（★M-1b：尾部 host 上传 MDI 行表的数据源，门关 = 空段早退）
	 * @param frameSlot 本帧 staging slot（scene staging 拷贝源 + 行表轮转槽）
	 */
	void recordFixedDataPath(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
		uint32_t frameSlot);
	/**
	 * 绑定 resource heap + sampler heap 到命令缓冲（坑 9：与 classic descriptor set 互斥）。
	 *
	 * 两个调用者：① `recordFixedDataPath`（每帧一次，pass 循环之前）；② FSR dispatch
	 * 之后——vendor 的 VK 后端用 `vkCmdBindDescriptorSets`，那会把 CB 切回 classic 模式并
	 * **作废** heap 绑定（docs/DESCRIPTOR_HEAP_SPEC.md §坑 9 的新实例），不重绑则同帧其后
	 * 每个 mesh draw 都报「heap was not bound」。
	 */
	void bindDescriptorHeaps(vk::raii::CommandBuffer& cmd) const;
	/**
	 * offscreen attachment RT 幂等 ensure（批次 4）：逐 offscreen 附件 resolveRgFormat +
	 * RenderTargetCache::ensure（extent 随 swapchain resize 重建，重建含 waitIdle，录制期调用安全）。
	 * generation 变化（新图布局 Undefined）→ 重置 attachmentLayouts[slot]，防下帧首 barrier
	 * oldLayout 失配。手术 6：块体自 drawFrame 摘出（rtGenerations 归本函数局部）。
	 * B2-4③′ 债-124：extent 漂移 → `RenderTargetCache::retire`（发现帧 + frames-in-flight
	 * 到期销毁 + 还槽）；到期收割也跑在本函数（安全点内）。
	 * @param extent 当前 swapchain extent
	 * @param frame 当前帧号（退役 due 计算与收割；= session.frameNumber()）
	 * @param offscreenRt 出参：按图 slot 填 RT 指针（非 offscreen 槽留 null），大小 = 附件数
	 * @return void；RenderTargetCache::ensure 失败 → Error
	 */
	[[nodiscard]] std::expected<void, Error> ensureOffscreenAttachments(vk::Extent2D extent,
		uint64_t frame, std::vector<RtAttachment*>& offscreenRt);
	/**
	 * 安全点重编译（B2-4③′，D5 第一刀；drawFrame 在 beginFrame 之后、任何录制之前调）。
	 * 不脏 = 零成本空转。脏 = 重编译 + **全部可失败步骤先跑完**（编译 / slot 稳定核对 /
	 * 相位表重建 / drawLight 管线条目核对），任一失败**旧图旧表原样、脏位保留**（下帧
	 * 重试）；全过才换图 + 按名结转 attachmentLayouts / rtGenerations + 重建 OffGraphSchedule。
	 * slot 稳定核对 = 「附件只准追加在末尾」的机器守卫：旧名在新图里的 slot 必须不变
	 *（FrameCapturer / PickingService 构造期缓存 slot，中段插入 = 它们读错附件）。
	 * 失败**内部出声**（error 日志）后返回——本帧继续用旧图、脏位保留下帧重试；
	 * drawFrame 调用点零分支（认知复杂度红线：安全点若逼调用点写 if，红线记账就转嫁给 drawFrame）。
	 */
	void recompileGraphIfNeeded();
	/**
	 * IZB buffer 附件的图侧同步（B2-4③′ 迁入；izbPreFrame 构造/重建后调）：
	 * 注册 head/nodes 字节（同名替换 = extent 漂移的正常路径）→ 写上界对账
	 *（失败 = throw，变异现场 fail-fast）→ declareBufferProductions("light", …)
	 *（幂等 + 试编译 fail-fast；首建标脏，下个安全点重编译吃到，延迟 ≤1 帧合规）。
	 */
	void syncIzbGraphAttachments();
	/**
	 * meshlet 可见标志表附件的图侧同步（B2-4④′；setMeshletCullService 挂服务后调）：
	 * declareBufferProductions(**图首 pass**, {meshletVisibleFlags}) →
	 * declareBufferConsumptions(**首个 drawSource=opaque 的 drawMesh pass**, 同)。
	 * 🔴 生产者锚点 = 图首 pass 的理由：cull dispatch 录在帧首 prologue（所有 pass 之前），
	 * 图能表达的最强事实就是「先于一切消费者」；shadow 级 0 由 decorateShadowPass **无条件**
	 * 前插 = 任何图形态的 pass 0，且 shadow_meshlet.mesh.slang 不声明 binding 21（不读标志）
	 * ⟹ 同 pass 无读写 feedback。消费者锚点判据用 drawSource 而不是 pass 名：deferred 的
	 * 主 opaque 段叫 "geometry"（drawSource 仍是 "opaque"）；
	 * meshlet.mode = "switch" 档只替换主 opaque 段 ⟹ 消费者就是它。失败 = throw（装配期路径，
	 * 同 syncIzbGraphAttachments 写上界对账的 fail-fast 口径；对账缺边红在声明现场）。
	 */
	void syncMeshletGraphAttachments();
	/**
	 * pass draw 三路分发（手术 6：自 drawFrame pass 循环摘出）：drawLight 固定全屏单 group
	 *（无 push constant，条目按 pass.state 名解析）/ opaque 段 GPU culling（DGC execute 或
	 * 读回重放，能力位自动切换）/ 普通段逐 draw（segmentRange + drawOne）。
	 * @param cmd 本帧命令缓冲（录制期）
	 * @param pass 编译 pass 计划（kind/drawSource/state/colors）
	 * @param list 每帧绘制列表（calls + 段边界）
	 * @param frameSlot 本帧 slot（cull 重放 args 索引）
	 */
	void executePassDraws(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
		const FrameDrawList& list, uint32_t frameSlot);
	/**
	 * 簇化轴 ★1-a1ii-α：影子叠加段盲执行（executePassDraws 通用分支在 opaque 段之后
	 * 调用；口径全在调用点注释）。空向量 = 零 draw 零开销。
	 *
	 * ★0.5-b2 共面双臂：若 `meshletOverlayDepthCompareOp` 有值（coplanar = equal/ne），
	 * **loop 前** set 该比较、**loop 后**复原 `passDepthCompareOp`——状态窗口恰好盖住叠加
	 * 那几条 draw。nullopt（off/门关）= 两条 set 都不发 = 动态状态序列逐 bit 不变（A1
	 * 构造性保证）。
	 * @param passDepthCompareOp 本 pass 段首 applyDynamicState 用的深度比较（复原目标）
	 */
	void executeMeshletOverlay(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
		vk::CompareOp passDepthCompareOp);
	/**
	 * picking 回读录制（Phase 10 批 10.5）：drawFrame ⑪.5 之后调用——服务未挂 =
	 * 零调用；否则 PickingService::record（object_id RT 请求像素 1-texel → staging）。
	 * @param cmd 本帧命令缓冲（录制期）
	 */
	void pickingRecord(vk::raii::CommandBuffer& cmd);
	/**
	 * 单个 mesh draw：push DrawArgs（pushDataEXT）→ bindShaders → drawMeshTasksEXT。
	 * ★M-1b：`call.drawCount > 1` 是唯一判据走 MDI 分支（此时 groupCount 被忽略，GPU 逐行
	 * 读行表里的 groupCountX，含 gcX=0 空转行）——push/bind 两条命令与逐条形态**同一个函数
	 * 同一条路**（argsSize 12B 不变 = D1 的复用块理由），只差第三条换成
	 * `drawMeshTasksIndirectEXT(buffer, call.argsOffset, call.drawCount, kMdiShadowRowStride)`
	 * （经唯一具名入口 `recordMeshTasksIndirectEXT`，参数序红线 → 其定义注释）。
	 * 🔴 本分支要求行表已落 GPU：段级 upload 在 `prepareMdiShadowRows`（pass 循环之前，
	 * 一次逐段），缺位 = 读到陈旧行 = 静默错影子，所以那里对 upload 失败**抛**（雷 3）。
	 * @param cmd 本帧命令缓冲（录制期）
	 * @param call 绘制条目（pipeline / pushData / groupCount；★M-1b + argsOffset / drawCount）
	 */
	void drawOne(vk::raii::CommandBuffer& cmd, const DrawCall& call);
	/**
	 * ★M-1b：录制期把 `list.mdiSegments` 的 CPU 行表逐段写进载体本帧槽（host memcpy+flush，
	 * 无命令流；ring×2 的写安全 = 只写 frameSlot——beginFrame 已等完本槽 fence）。
	 * 首行判空早退（门关/回落 = 零新命令 = 逐位不变构造性保证）。
	 * 🔴 fail-fast（雷 3 定案：采集期预检全数拦下后，这里任何一条不过 = 接线 bug，静默
	 * 继续 = GPU 拿陈旧行表照常画，本项目反复堵的「错了不报错只图像错」形态）：
	 *  · 段非空但服务未挂 → throw；
	 *  · MDI DrawCall 条数 ≠ 段数 → throw（行表与 draw 分家 = A5 读数会骗人）；
	 *  · 段自带 frameSlot ≠ 本帧槽 → throw（写错槽 = 踩在途帧）；
	 *  · upload 返回错误串 → throw（原样带出四道前置的名字）。
	 * @param list 每帧绘制列表（mdiSegments）
	 * @param frameSlot 本帧录制槽（drawFrame ① 取的同一个数）
	 */
	void prepareMdiShadowRows(const FrameDrawList& list, uint32_t frameSlot);
	/** materialFilter 哨兵 = 不过滤（整段重放，picking 用）。 */
	static constexpr uint32_t kNoMaterialFilter = 0xFFFFFFFFu;
	/**
	 * 克隆管线重放（Phase 10 批 10.5 picking / 10.7 highlight 共用）：逐 opaque 段 draw
	 * 换用指定用途的克隆管线重录（不走 cull/DGC——可见性由 depth 测试保证）。
	 * @param purpose 克隆用途（picking = 整段重放；highlight = 按材质过滤）
	 * @param materialFilter kNoMaterialFilter = 不过滤；否则只重放该材质 id 的 draw
	 *   （materialBySlot 越界的 draw 跳过，不读越界内存）
	 */
	void replayClonedPass(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
		ClonePurpose purpose, uint32_t materialFilter);
	/**
	 * ⑪.6 帧末扩展段（drawFrame 仅 hook 非空时调用；契约见 setFrameEndHook）：
	 * swapchain 任意帧末布局（直渲 = CAO / blit 变体 = TransferDst / 捕获帧 = TransferSrc）
	 * 推进到 COLOR_ATTACHMENT_OPTIMAL（含追踪表更新）→ 调 hook 叠加 UI。
	 * 图内无 swapchain 附件（纯离屏图）= 早退不调 hook。
	 * @param cmd 录制中的命令缓冲
	 * @param swapchainImage 本帧后缓冲（布局 barrier 目标）
	 * @param swapchainView 本帧后缓冲 view（hook 参数）
	 * @param swapchainExtent **后缓冲** extent（批 14.3 起不再等于渲染 extent）——imgui
	 *   的像素空间就是后缓冲像素，传渲染 extent 会让 UI 随 renderScale 一起缩。
	 */
	void emitFrameEndOverlay(vk::raii::CommandBuffer& cmd, VkImage swapchainImage,
		vk::ImageView swapchainView, vk::Extent2D swapchainExtent);
	/** 批 11.3-1d IZB 帧首段（drawFrame 调用）：收割上帧 dump + 帧计数 + 门开时
	 *  构造/重建 IzbShadow（deferred 判据 + extent 漂移重建前 waitIdle）。门关 =
	 *  仅剩一次布尔判断（izbShadow 恒空）。 */
	void izbPreFrame();
	/** 批 11.3-1d：pass 循环头部的 IZB sandwich——仅 deferred light pass
	 *  （drawLight "light"）且 izbShadow 非空时录 create+prime（深度已由 geometry
	 *  pass 写完；layout 进出同值 = 追踪表零漂移）。其余 pass 零命令。 */
	void izbLightPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
		const FrameDrawList& list);
	/** 批 11.6d-2 EVSSM 帧首段（drawFrame 调用，与 izbPreFrame 同位）：门开时构造
	 *  EvssmShadow（需 deferred light pass + 阴影图 RT 已在 + shadow.lua 的 mode=="evssm"）。
	 *  门关 = 一次布尔判断零分配（evssmShadow 恒空 = 逐位红线）。
	 *  ⚠️ 仅 deferred：消费点在 deferred light pass；非 deferred 图形态由 gate_lint 启动期
	 *  error（2026-10-02 从本处 warn-once 迁走），运行期早退保留。 */
	void evssmPreFrame();
	/** 批 11.6d-2：pass 循环头部 EVSSM sandwich（与 izbLightPassPre 同位）——仅
	 *  deferred light pass 录矩派生 + 7 级 mip 链（图外 compute，其余 pass 零命令）。 */
	void evssmLightPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass);
	/** 批 14.5 FSR 帧首段（与 evssmPreFrame 同位、同在其**之后**，让首帧懒构造的显存
	 *  被 `logFirstFrameMemory` 计入）。门开且图里两个输入附件齐备时构造 FsrUpscaler；
	 *  渲染网格变化 ⟹ 重建（vendor 内部共享图按 maxRenderSize 定尺寸，**时域历史全丢**）。
	 *  构造**不在这里**（在 Renderer ctor，理由见 `initFsr`）；门关 = 一次指针判空。 */
	void fsrPreFrame();
	/** 批 14.5：领 FSR 输出采样槽 + 现查三个输入附件 slot + 构造 FsrUpscaler。
	 *  Renderer ctor 在 fsrEnabled 时调用（**不懒**，理由见 `FsrUpscaler` 接口注释），
	 *  渲染网格变化后的重建也走它（重写同槽）。前置不满足即抛——见函数内注释。 */
	void initFsr();
	/**
	 * 批 14.5：pass 循环头部 FSR sandwich（与 izbLightPassPre 同位）——仅 tonemap pass
	 *  之前录一段图外 compute（vendor 内部一次录完全部 pass）。其余 pass 零命令。
	 *
	 * 本批 FSR 输出**尚无消费者**（接 tonemap 色输入是下一批），所以门开也画面逐位不变；
	 * 活性判据 = 故意申报错的 depth state，VVL 必须响（见 docs/phase14_plan.md 14.5 判据）。
	 *
	 * @param colorImage    tonemap 当前采的那张色图（taa_out / scene_hdr），SRO
	 * @param velocityImage 速度场 RT（挂点处通常还在 CAO，本函数负责转 SRO 并同步追踪表）
	 */
	void fsrTonemapPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
		const FrameDrawList& list);
	/**
	 * 簇化轴 ★1-a2-i：帧首录一次**逐簇视锥剔除** compute（图外 compute，定义在 :offgraph）。
	 *
	 * 位置契约 = `recordFixedDataPath` **之后**（heap 已绑定、scene data 已拷完）、pass 循环
	 * **之前**（还在 `gpu.frame` 时间戳内、任何 render pass 外——barrier 的 eHost stage 不能在
	 * pass 内）。消费方 = meshlet.mesh.slang binding 21 的早退，它在随后的 geometry pass 里。
	 *
	 * 门关 = `meshletCull` 恒 nullptr ⟹ 本函数一次指针判空、零命令（逐位不变的构造性保证）。
	 * @param cmd 本帧主命令缓冲（录制期）
	 * @param list 每帧绘制列表（cullInput.planes = 与 CPU 剔除同源同值的视锥 6 面）
	 */
	void recordMeshletCullDispatch(vk::raii::CommandBuffer& cmd, const FrameDrawList& list);
	// ── 批次 B1：图外工作申报面（定案与批次 → `docs/compute_调研.md` §6/§7）──────────
	/**
	 * 图外工作申报表（**唯一真相源**，实现在 `:offgraph`）。
	 *
	 * 它替代的是原先散在 C++ 里的两类东西：① 挂点判据（`pass.kind == drawLight &&
	 * pass.state == "light"` 这类字符串比较）；② 只活在注释里的位置契约
	 * （`recordMeshletCullDispatch` 声明上那三条硬约束）。
	 *
	 * 🔴 **新增图外录制点必须在这里挂号**：没挂号的 `offGraphEnter` 会报「未申报」，
	 * 挂号了却没录的会在帧末报「静默旁路」。
	 */
	static std::vector<RgOffGraphWorkDecl> offGraphWorkDecls();
	/**
	 * 图外工作开录：查位置契约（当前相位是否 = 申报相位）。
	 *
	 * 违规**只出声不改行为**（同 `layoutDrift` 的 A 档口径：护栏不改变交付路径的命令流）。
	 * 必须在门判据**之后**调用——门关 = 零命令，也就零申报。
	 * @param name 申报名（须出现在 `offGraphWorkDecls()` 里）
	 */
	void offGraphEnter(std::string_view name);
	/**
	 * 帧末覆盖对账：推进到帧末相位 + 核对「门开着 ⟺ 本帧录过」。
	 *
	 * 两个方向都要（缺一半就是摆设）：门开着没录 = 挂点被静默删掉；门关着却录了 = 破
	 * 「门关零命令」这条逐位不变红线。门位在**帧末**求值（IZB / EVSSM 首帧懒构造）。
	 *
	 * @param list 本帧的 draw list——同步边的 `izbMaskSegment` 门要**关于输入**判 shadow0 段
	 *        是否为空（读"刚才调过 `beginMaskPass` 没有"会让那条断言自证）
	 */
	void offGraphEndFrame(const FrameDrawList& list);
	/**
	 * 单条图外工作的**门位**（帧末覆盖对账的输入；B2-5 起按名字求值）。
	 *
	 * 为什么从「一个 `std::array<bool, N>` 按下标写死」改成按名字问：申报面能撤回之后，
	 * 在场项的**位序**会随插拔变，而下标形态的门位数组隐含「第 i 个门 = 第 i 条申报」——
	 * 两者一旦错位，覆盖对账就指错工作项（报「meshlet_cull 静默旁路」而真凶是别的项）。
	 * 按名字问还白送一条判据：**在册却没有门位规则 = 出声**（新增图外工作只挂号不加门位，
	 * 原先会静默不参与对账）。
	 *
	 * 🔴 三条判据都必须关于**门与输入**，不许写成「刚才 enter 过没有」——后者让断言自证。
	 * @param name 申报名
	 * @param list 本帧 draw list（IZB mask 段那道边要它；本函数不用，签名与调用点对齐）
	 * @return 门是否开着；nullopt = 这个名字没有门位规则（调用方出声）
	 */
	std::optional<bool> offGraphGateActive(std::string_view name, const FrameDrawList& list);
	/**
	 * 消费在途的 pass 开关意图（B2-5 **复合事务**；drawFrame 在 `recompileGraphIfNeeded`
	 * **之前**调，两者落在同一个安全点内）。
	 *
	 * 一笔事务做三件事，顺序是**先做可失败的**：
	 *  ① 撤 / 插 pass 本体（`withdrawPass` / `declarePass` / `redeclarePass`，各有试编译
	 *     fail-fast）——失败就整笔不生效，图与册子都不动；
	 *  ② 跟着撤 / 复挂在它上面的**图外申报**（`OffGraphWorkTable`，按锚点解析）——不可失败，
	 *     但必须在 ① 之前问锚点（pass 还在的时候才问得出它排第几）；
	 *  ③ 门位与相位表不需要在这里动：它们都是**派生件**，前者帧末按在册项现求，后者由紧随
	 *     其后的 `recompileGraphIfNeeded` 对着新图重建。
	 *
	 * 失败**内部出声**（error 日志）后继续跑下一格意图：本帧用旧图，不留半态。
	 */
	void applyPassToggles();
	/**
	 * 构造期登记 highlight 的装饰申报单（图里有 → `adoptPass`；没有 → 只留首插配方）。
	 * @param highlightPlan 装配层算好的装饰计划（纯函数产物，门关档时 applicable=false）
	 */
	void registerHighlightRecipe(const RgHighlightPlan& highlightPlan);
	/**
	 * `applyPassToggles` 的**撤出半笔**（拆出来只为认知复杂度红线，判据一条没挪）。
	 * @param passName 即将离场的 pass 名
	 * @return 空值 = 申报与 pass 都已离场；失败带 Error（此时申报已被反向补偿回原状）
	 */
	std::expected<void, Error> withdrawPassTransaction(std::string_view passName);
	/**
	 * `applyPassToggles` 的**进场半笔**（同上）。
	 * @param passName 即将进场的 pass 名
	 * @return 空值 = pass 与挂在它上面的申报都已就位
	 */
	std::expected<void, Error> restorePassTransaction(std::string_view passName);
	/** FSR 挂点要的两个图属输入（色 + 速度场）；任一解析不到时整体为 nullopt。 */
	struct FsrInputs {
		vk::Image color = {};     ///< 图属色输入（tonemap 的上游 RT）
		vk::Image velocity = {};  ///< 图属速度场输入（挂点处需转布局的那张）
	};
	/**
	 * FSR 挂点要的两个图属输入（挂点与帧末门判据**共用同一个表达式**，防两处漂）。
	 *
	 * 取不到 = 装配错（`ensureOffscreenAttachments` 在 pass 循环之前跑完，正常路径必已存在）
	 * ⟹ 不录。把它单独成函数是为了让帧末覆盖对账能问「这帧到底该不该录」，而不是把
	 * 「没录」误判成静默旁路。
	 * @return 两个 VkImage；任一解析不到 = nullopt
	 */
	std::optional<FsrInputs> fsrInputs();
	/**
	 * 申报表项数（= `offGraphWorkDecls()` 的长度 = `offGraphEndFrame` 门位数组的长度）。
	 *
	 * 三处之一改动而另两处没跟上 ⟹ 构造期 throw（不是每帧一条 error，也不是静默漏对账）。
	 */
	static constexpr uint32_t kOffGraphWorkCount = 4;
	/**
	 * 相位表（构造期对着**编译图**建，锚点解析不到即 throw）。
	 *
	 * 编译图构造完就不再变 ⟹ 锚点与图的错配没有「运行期才知道」的理由。
	 * ⚠ B2-5 起它**会**变：安全点重编译时对着新图重建（pass 进出 ⇒ 相位 2+2i 全体挪位）。
	 * ⚠ 债-128 起「解析不到」分两档：`mayBeAbsent` 锚点命中 0 = 不 throw，该项标成
	 * 不可调度（启动期 warn 打点）；其余锚点命中 0 / 命中多个仍 throw。
	 */
	OffGraphSchedule offGraph;
	/**
	 * 图外工作**在册表**（B2-5）：规范序的申报 + 在场位。
	 *
	 * 为什么从 static 函数变成成员：`offGraphWorkDecls()` 那份静态表是「申报集编译期定死」的
	 * 载体，而 pass 能进出图之后，挂在某个 pass 上的申报必须跟着进出（否则安全点会拿旧锚点
	 * 建表，报一句「命中 0 个 pass」把「事务没做全」伪装成「装配错」）。静态表保留为
	 * **构造输入**（规范序的唯一出处），在册表是它的运行期投影。
	 */
	OffGraphWorkTable offGraphWorks;
	/**
	 * 装配期登记的「可开关 pass 名单」（B2-5）：pass 名 → 从**无到有**时用的申报单。
	 *
	 * 为什么需要第二张表：启动期装饰已把 pass 插进 desc 的，走 `adoptPass`（登记即可撤）；
	 * 而门没开、图里根本没有它的，收到 `on` 时得知道**怎么把它造出来**。两种来源共用
	 * `applyPassToggles` 这一条事务路径，差别只在"从哪张单子拿申报"。
	 */
	std::vector<RgPassDeclaration> passRecipes;
	/** 在途的开关意图（B2-5；单格 = 最后一次收单说了算，同 FrameCapturer 的 request 口径）。 */
	struct PassToggleRequest {
		std::string passName;  ///< 目标 pass 名
		bool on = true;        ///< 期望状态
	};
	std::optional<PassToggleRequest> pendingPassToggle;
	/**
	 * 图外同步发射账（批次 B1b → `docs/compute_调研.md` §7.2）：图外模块经它**申报即发射**
	 * （barrier 参数由 usage 词表物化，模块不写 mask 字面量），帧末 `offGraphEndFrame` 对账
	 * 实发次数 vs 应发次数。
	 */
	OffGraphSync offGraphSync;
	/** 批 9.5 补：首帧补打一次显存统计（`vma[first_frame]`）。动机 = IzbShadow /
	 *  EvssmShadow 均首帧懒构造，而 `logStartupBaseline` 在帧循环之前 ⟹ `vma[startup]`
	 *  不含这两个模块的资源（实测开/关 EVSSM 档的 startup 统计逐字节相同）。
	 *  进程内只打一次（`vmaCalculateStatistics` 遍历全分配，有开销）。 */
	void logFirstFrameMemory();
	/** 批 11.3-1d 后半：光空间 mask pass（零附件 G×G + izb_mask 态 + shadow0 段
	 *  重放，push 尾缀 gridDim；shadow0 段空 = warn-once 跳过）。 */
	void replayIzbMaskSegment(vk::raii::CommandBuffer& cmd, const FrameDrawList& list);
	/**
	 * 本帧是否是 izb 的 dump 帧（`shadow.lua` 的 `izb.dump_frame`：第 N 帧起每 30 帧转一次）。
	 *
	 * 抽成一处是因为它有**两个消费者**：`izbLightPassPre`（决定要不要录 dump 拷贝）与帧末
	 * 同步对账（`izbDump` 门的启用位）。两处各写一遍表达式 = 门位算错时护栏**假阳性**，
	 * 而假阳性的护栏三个月后只会被关掉（`docs/compute_调研.md` §7.5 第 1 条）。
	 */
	bool izbDumpDue() const;
	// recreateSwapchainAndDepth（swapchain+depth 重建）已随手术 4 迁 FrameSession。
	RGL::DeviceContext& vkDevice;  // 设备层引用（队列 / 命令池 / fence 等待 / 能力探测）
	RGL::Swapchain* swapchain;      // 交换链层（main 持有，析构序在 Renderer 之后）
	RGL::Resources* resources;      // 资源层（main 持有，析构序在 Renderer 之后）
	RGL::SceneDataService* sceneData;  // scene data 服务（main 持有，架构债务续账步骤 1 拆自内核）
	RGL::Environment* env;  // 环境服务（main 持有，架构债务续账步骤 3 拆自内核；ecs 经访问器读）
	RGL::RenderTargetCache* rtCache;  // offscreen RT 缓存（main 持有，架构债务续账步骤 2 拆自内核）
	// 管线表（Phase 4 批次 2：迁出至 ShaderRegistry，零 entt；main 先于 Renderer 构造并持引用，
	// 析构序 Renderer 先于 registry 安全）
	RGL::ShaderRegistry& registry;
	// Phase 6：编译渲染图（构造传入，main 启动期 compileRenderGraph 产出）
	// B2-4③′（D5 第一刀）起它不再是「之后不变」：安全点重编译成功后被整体替换，
	// 逐 slot 跨帧表按名结转（FrameCapturer/PickingService 持本成员引用——替换是
	// **原地赋值**不换对象地址，引用恒有效）。
	RGL::CompiledRenderGraph renderGraph;
	// B2-4③′：脏驱动的图派生件（desc 真相源 + 脏位 + 重编译计数；声明在 renderGraph
	// 之后 = 初始化列表顺序，构造只吃 desc 不碰 renderGraph）。
	RGL::DynamicRenderGraph graph;
	// Phase 7b：GPU culling 子系统（main 持有，构造序先于 Renderer → 析构序其后安全）；
	// Phase 10 批次 0 手术 3：④c 提交时序 + opaque 段双路 + 对账全迁 CullDriver
	//（本类构造期以 main 传入的裸指针建 driver；inactive = 全路径不碰 culling 语义不变）
	RGL::CullDriver cullDriver;
	// attachment 真实布局追踪（按图 slot 索引，跨帧持久）：帧首 barrier 的 oldLayout
	// 来源（plan.oldLayoutDiscard=false 者用本值；swapchain/depth 走帧首丢弃规则
	// 不查询）；录制期随每条物化 barrier 更新为 plan.newLayout。
	//（Phase 4 前 DIAG RGL_OFFSCREEN 的 offscreenLayout 单变量泛化为逐 slot 表）
	std::vector<vk::ImageLayout> attachmentLayouts;
	/**
	 * 图外编舞用的**永久布局转换**原子原语：发一条 `pipelineBarrier2` + 写
	 * `attachmentLayouts[slot]` + 返回实际写入的布局，三者由同一个局部量产出、不可分割。
	 *
	 * 为什么必须是**一个**函数而不是「调用方自己发 barrier 再自己写表」：barrier 的
	 * `newLayout` 与写进表的值一旦是两个独立字面量，表就可能与真实布局脱钩，而**读表的护栏
	 * 随之退化成同义反复**（恒真）。velocity 那条护栏在批 14.4b 之前就是这个形态，镜像探针
	 * 实测恒假 → 见 `fsrTonemapPassPre` 的注释。
	 *
	 * 语义细则（改动前逐条读）：
	 * - **同布局早退**：`attachmentLayouts[slot] == newLayout` 时一条命令都不发、直接返回该
	 *   布局。⚠ 这条判据**不许改宽也不许改窄**——它决定「是否发 barrier」，而 barrier 除了
	 *   布局转换还建执行依赖；放宽 = 少一条同步（UB），改窄 = 每帧多发转换。
	 * - `oldLayout` 取转换前的 `attachmentLayouts[slot]`。
	 *
	 * 适用边界（本原语**只**服务永久转换）：
	 * - 永久转换 = 转换后一直保持、下帧的 barrier 该拿它当 `oldLayout` ⟹ **必须写表**。
	 * - **临时 sandwich 一律不得用它**：IZB 的深度 sandwich（`DSO → SRO → compute → 还原
	 *   DSO`）靠的正是「**不写表** + 返回值与表对账」——一旦它改成写表，表就自洽 ⟹ 那条
	 *   漂移护栏永远为真 = 假护栏。它的写法见 `rgl/izb_shadow_impl.cppm` 的 `recordFrame`。
	 * - 目前**只有一个用户**（FSR 的 velocity 转换）。第二个用户出现时再议要不要推广，
	 *   **不要**顺手把 IZB 接上（见上一条）。
	 *
	 * @param cmd       录制中的命令缓冲
	 * @param slot      图附件 slot（`attachmentLayouts` 的下标）
	 * @param image     该附件的 VkImage（表只记布局、不持句柄，故须由调用方给）
	 * @param newLayout 目标布局（永久转换的落点）
	 * @param aspect    subresourceRange 的 aspect（图属附件均为单 mip 单层）
	 * @return 实际写入表与 barrier 的那个布局（同布局早退时 = 原值）
	 */
	vk::ImageLayout transitionAndTrack(vk::raii::CommandBuffer& cmd, size_t slot,
		vk::Image image, vk::ImageLayout newLayout, vk::ImageAspectFlags aspect,
		vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
		vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess);
	// 批 12.6 T4：RT generation 的**跨帧**快照（按图 slot 索引，与 attachmentLayouts 同
	// 生命周期）。⚠ **必须是成员，不能是 ensureOffscreenAttachments 的局部量**：局部量每帧
	// 归零，而 RT 首建即 generation=1（`ensure` 的早退只挡「尺寸/格式没变」，首建必走
	// generation+1），于是 `generation != 0` **恒真** ⟹ 每帧把每个 offscreen 附件的追踪布局
	// 抹成 eUndefined = 每帧声明「内容可丢弃」。本帧先写后读的 RT 无感（那正是帧首丢弃的
	// 语义），但**跨帧历史会在被读到之前销毁**——批 12.6 T4 实测：taa_hist 每帧
	// `Undefined -> ShaderReadOnlyOptimal`，TAA 历史恒黑、时域滤波名存实亡。
	std::vector<uint64_t> rtGenerations;
	// 帧内捕获（Phase 10 批次 0 手术 2）：请求状态 + 回读缓冲 + record/collect 三步曲
	// 全迁 RGL::FrameCapturer（frame_capture 模块，构造期自图定位 swapchain slot）。
	// 声明位置 = 原 capture 成员块（attachmentLayouts 之后、stateKit 之前），
	// ctor 初始化列表顺序与之对应（-Wreorder 同步）。
	RGL::FrameCapturer capturer;
	// 帧末扩展段 hook（Phase 10 批 10.1，setFrameEndHook 装配；空 = drawFrame 零调用。
	// 不列 ctor 初始化列表——默认空 std::function，无初始化顺序约束）
	std::function<bool(vk::raii::CommandBuffer&, vk::Extent2D, vk::ImageView)> frameEndHook;
	// picking 服务（Phase 10 批 10.5，setPickingService 装配；nullptr = 零开销）。
	// 非空 = 执行器 picking pass 走克隆管线重放 + 帧末 record + submit 后 collect。
	PickingService* pickingService = nullptr;
	// 簇化轴 ★0.5-b2：共面双臂的叠加段深度比较覆写（setMeshletOverlayDepthCompareOp 装配；
	// nullopt = 默认 = coplanar off/门关 = executeMeshletOverlay 零 set = 动态状态序列
	// 逐 bit 不变，A1 判据的构造性保证）。
	std::optional<vk::CompareOp> meshletOverlayDepthCompareOp;
	// 簇化轴 ★1-a2-i：逐簇视锥剔除服务（setMeshletCullService 装配；**门关 = 恒 nullptr，
	// 帧首零命令逐位不变**，同 pickingService 的「装配后挂裸指针」惯例）。所有权在装配层
	// （main 的 unique_ptr，声明序在 backEnd 之前 = 析构序在其后，AGENTS 生命周期红线）。
	MeshletCullService* meshletCull = nullptr;
	// 簇化轴 ★M-1b：真 MDI 行表载体（setMeshletShadowArgsService 装配；裸指针，所有权在
	// 装配层 meshletAssembly，声明序先于本类 = 长寿契约同族）。⚠ 非生效档判据（出厂态也
	// 挂得上）；生效判据唯一 = FrameDrawList::mdiSegments 非空（prepareMdiShadowRows 首行）。
	MeshletShadowArgsService* mdiShadowArgs = nullptr;
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
	// Phase 7 D1 / Phase 8 批次 ①泛化：drawLight pass 的 state 名 → 固定全屏管线条目下标
	//（词表 = pass_vocabulary kFullscreenPassStates，构造期从 registry 取；
	// 图含对应 state 的 drawLight pass 而表内无条目 = 配置错误，构造期抛）
	std::map<std::string, uint32_t> fullscreenPipelineIdx;
	// descriptor heap 的 GPU 地址（VmaDescriptorHeap 管理的"描述符槽位数组"，
	// 对齐细节见 VmaBuffer.cppm）
	vk::DeviceAddress heapAddress = 0;
	// 批 11.3-1d：FTIZB 硬阴影 IZB 数据层（门 = shadowFormPlan().consumeIzbMask 且图含
	// drawLight "light" pass；**门关 = 恒 nullptr，零分配零命令逐位不变**，
	// docs/hfts_izb_plan.md）。extent 变化重建（重建前 waitIdle，同 RT 缓存规则）。
	std::unique_ptr<RGL::IzbShadow> izbShadow;
	// 批 11.6d-2：EVSSM 矩图数据层（门 = shadowFormPlan().buildEvssmMoments 且 deferred；
	// **门关 = 恒 nullptr，零分配零命令逐位不变**，同 izbShadow）。
	std::unique_ptr<RGL::EvssmShadow> evssmShadow;
	// 批 14.5：FSR 3.1.4 upscaler（门 = runtime.fsr_upscaler 且 velocity 真的进了图；
	// **门关 = 恒 nullptr，零构造零命令逐位不变**，同 izbShadow / evssmShadow）。
	std::unique_ptr<RGL::FsrUpscaler> fsr;
	bool fsrEnabled = false;   ///< 装配层传入的门（已含 velocity 齐备判据）
	bool fsrTemporal = false;  ///< 批 14.5：false = 每帧 reset（纯空间上采样，口径见 configs/runtime.lua）
	int32_t fsrColorSlot = -1;    ///< FSR 色输入附件 slot（ao_applied → taa_out → scene_hdr）
	bool fsrColorBaseSpace = false;  ///< 色输入是否已在**基空间**（= 选中的是 `taa_out`）；
	                                 ///< 申报给 FSR 的内容位移必须由它推导，见 `initFsr`
	int32_t fsrVelocitySlot = -1; ///< 速度场附件 slot（`transitionAndTrack` 的转换对象；布局
	                              ///< 护栏**不读**它——理由见 `fsrTonemapPassPre` 的 ② 注释）
	bool fsrDispatchedOnce = false;  ///< 首帧之后不再 reset（时域历史已建立）
	int32_t fsrDepthSlot = -1;   ///< 深度附件 slot（布局护栏；**不用 depthAttSlot**——
	                             ///< 那个只在 IZB 门开时由 izbPreFrame 填，FSR 不依赖 IZB）
	uint32_t izbFrameNo = 0;   ///< 帧计数（shadow.lua 的 izb.dump_frame 比对，1 起）
	bool izbMaskSegmentWarned = false;  ///< shadow0 段空 warn-once（mask 依赖投影体）
	bool memStatsLogged = false;        ///< 显存基线只打一次（首帧懒建资源入账用）
	int32_t depthAttSlot = -1; ///< 图内 depth 附件 slot（IZB sandwich 读布局追踪表用）

	// scene data staging ring（Phase 5 批次 2，见 createSceneStaging；slot 数 =
	// FrameSession::kFramesInFlight，与帧 slot 一一对应）
	std::array<RGL::VmaBuffer, FrameSession::kFramesInFlight> sceneStaging;
	// 双缓冲游标 / 帧计数 / noPresent / 四套同步对象 / 每帧命令缓冲（原
	// MAX_FRAMES_IN_FLIGHT/currentFrame/frameCount/noPresent/graphicsQueue/
	// imageAvailableSemaphores/renderFinishedSemaphores/inFlightFences/presentFences/
	// commandBuffers）已随手术 4 迁 FrameSession 私有成员。
};

}  // namespace RGL
