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
	 * @param vkResources 资源层（descriptor heap / 深度图 / offscreen RT）
	 * @param sceneData scene data 服务（staging 拷贝目标 buffer，架构债务续账步骤 1）
	 * @param env 环境服务（ecs renderListSystem 经 getEnvironmentRef 读，架构债务续账步骤 3）
	 * @param rtCache offscreen RT 缓存（drawFrame 每帧 ensure，架构债务续账步骤 2）
	 * @param shaderRegistry 管线表（条目查找 + bindShaders；main 先于 Renderer 构造）
	 * @param renderGraph 编译渲染图（构造后不变；不可解析启动期抛出）
	 * @param gpuCull GPU culling 子系统（Phase 7b；main 先于 Renderer 构造。inactive
	 *   （候选表空）时 drawFrame 全路径不碰 culling = 行为中性）
	 * @param dgc DGC 命令生成子系统（Phase 7b NVIDIA 路；main 仅 graphicsIndirect=true
	 *   时构造，否则 null → 看 dgcStatic / 读回重放）
	 * @param dgcStatic 静态 shader DGC 路（RADV spike，docs/archive/radv_static_dgc_spike.md；
	 *   main 按 staticGraphicsIndirect + diagnostics.dgcStatic 条件构造，null = 关）
	 */
	Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::SceneDataService& sceneData, RGL::Environment& env, RGL::RenderTargetCache& rtCache,
		RGL::ShaderRegistry& shaderRegistry, RGL::CompiledRenderGraph renderGraph,
		RGL::GpuCull* gpuCull, RGL::DgcCommands* dgc, RGL::DgcStaticCommands* dgcStatic = nullptr);
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
	 * 编译渲染图只读访问（Phase 10 批 10.5：PickingService 构造需附件表定位 object_id
	 * slot；同 getSwapchainRef 惯例）。
	 * @return 图 const 引用
	 */
	[[nodiscard]] const RGL::CompiledRenderGraph& getRenderGraphRef() const { return renderGraph; }

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
	/**
	 * ④⑥⑥.5⑦ 数据通路固定段（Phase 6 起不入图——非 attachment 状态同步）：heap barrier
	 *（host write → resource/sampler heap read；eHost stage 不能在 render pass 内，故必须在
	 * beginRendering 之前）→ scene staging → device buffer 拷贝 + Transfer 写→图形 shader 读
	 * barrier → resource heap / sampler heap 绑定。手术 6：块体自 drawFrame 摘出，命令流
	 * 逐条不变。
	 * @param cmd 本帧命令缓冲（录制期）
	 * @param frameSlot 本帧 staging slot（scene staging 拷贝源）
	 */
	void recordFixedDataPath(vk::raii::CommandBuffer& cmd, uint32_t frameSlot);
	/**
	 * offscreen attachment RT 幂等 ensure（批次 4）：逐 offscreen 附件 resolveRgFormat +
	 * RenderTargetCache::ensure（extent 随 swapchain resize 重建，重建含 waitIdle，录制期调用安全）。
	 * generation 变化（新图布局 Undefined）→ 重置 attachmentLayouts[slot]，防下帧首 barrier
	 * oldLayout 失配。手术 6：块体自 drawFrame 摘出（rtGenerations 归本函数局部）。
	 * @param extent 当前 swapchain extent
	 * @param offscreenRt 出参：按图 slot 填 RT 指针（非 offscreen 槽留 null），大小 = 附件数
	 * @return void；RenderTargetCache::ensure 失败 → Error
	 */
	[[nodiscard]] std::expected<void, Error> ensureOffscreenAttachments(vk::Extent2D extent,
		std::vector<RtAttachment*>& offscreenRt);
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
	 * picking 回读录制（Phase 10 批 10.5）：drawFrame ⑪.5 之后调用——服务未挂 =
	 * 零调用；否则 PickingService::record（object_id RT 请求像素 1-texel → staging）。
	 * @param cmd 本帧命令缓冲（录制期）
	 */
	void pickingRecord(vk::raii::CommandBuffer& cmd);
	/**
	 * 单个 mesh draw：push DrawArgs（pushDataEXT）→ bindShaders → drawMeshTasksEXT。
	 * @param cmd 本帧命令缓冲（录制期）
	 * @param call 绘制条目（pipeline / pushData / groupCount）
	 */
	void drawOne(vk::raii::CommandBuffer& cmd, const DrawCall& call);
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
	 *  EvssmShadow（需 deferred light pass + 阴影图 RT 已在 + shadow_pass 开）。
	 *  门关 = 一次布尔判断零分配（evssmShadow 恒空 = 逐位红线）。
	 *  ⚠️ v1 边界 = 仅 deferred：forward 的矩消费在 geometry pass（sandwich 未接），
	 *  forward + 门开 = warn-once 不构造（变体采样读 fallback 零矩 = 全亮，无害）。 */
	void evssmPreFrame();
	/** 批 11.6d-2：pass 循环头部 EVSSM sandwich（与 izbLightPassPre 同位）——仅
	 *  deferred light pass 录矩派生 + 7 级 mip 链（图外 compute，其余 pass 零命令）。 */
	void evssmLightPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass);
	/** 批 9.5 补：首帧补打一次显存统计（`vma[first_frame]`）。动机 = IzbShadow /
	 *  EvssmShadow 均首帧懒构造，而 `logStartupBaseline` 在帧循环之前 ⟹ `vma[startup]`
	 *  不含这两个模块的资源（实测开/关 shadow_evssm 的 startup 统计逐字节相同）。
	 *  进程内只打一次（`vmaCalculateStatistics` 遍历全分配，有开销）。 */
	void logFirstFrameMemory();
	/** 批 11.3-1d 后半：光空间 mask pass（零附件 G×G + izb_mask 态 + shadow0 段
	 *  重放，push 尾缀 gridDim；shadow0 段空 = warn-once 跳过）。 */
	void replayIzbMaskSegment(vk::raii::CommandBuffer& cmd, const FrameDrawList& list);
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
	// 批 11.3-1d：FTIZB 硬阴影 IZB 数据层（门 = diagnostics().izbShadow 且图含
	// drawLight "light" pass；**门关 = 恒 nullptr，零分配零命令逐位不变**，
	// docs/hfts_izb_plan.md）。extent 变化重建（重建前 waitIdle，同 RT 缓存规则）。
	std::unique_ptr<RGL::IzbShadow> izbShadow;
	// 批 11.6d-2：EVSSM 矩图数据层（门 = diagnostics().shadowEvssm 且 deferred 且
	// shadow_pass；**门关 = 恒 nullptr，零分配零命令逐位不变**，同 izbShadow）。
	std::unique_ptr<RGL::EvssmShadow> evssmShadow;
	uint32_t izbFrameNo = 0;   ///< 帧计数（izb_dump_frame 比对，1 起）
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
