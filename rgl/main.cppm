module;
#include <entt/entt.hpp>  // 启动流 helper 签名含 entt::registry

// 入口模块 main 的主接口单元——2026-09-14 由 main.cpp 转模块（CMake CXX_MODULES FILE_SET
// 只收接口单元/分区，不收纯实现单元，故 main() 放分区实现单元 main_impl.cppm）。
// 形状：本接口 = 启动流 helper 声明 + 轻量 struct（接口瘦身惯例）；重定义在分区
// main:scene（main_scene.cppm），轻定义 + main() 在分区 main:impl（main_impl.cppm）。
// 本模块无外部 importer（入口叶子），导出仅为分区间可见性走接口声明惯例。
export module main;

import rlstd;
import camera_config;       // CameraConfig
import scene_config;        // SceneConfig
import runtime_config;      // RuntimeConfig
import vulkan_window;       // SDLWindow
import render_graph;        // CompiledRenderGraph
import render_graph_config;  // RgGraphVariant（CompiledGraphResult.variant，gate_lint 交叉校验）
import vk_device;           // DeviceContext
import rlvk;               // vk::raii::PhysicalDevice（checkMeshletDeviceFit 入参）
import vk_swapchain;        // Swapchain
import vk_resources;        // Resources
import scene_data;          // SceneDataService（架构债务续账步骤 1）
import sampler_library;    // SamplerLibrary（架构债务续账步骤 4a/4b）
import environment;        // Environment（架构债务续账步骤 3）
import render_target_cache;  // RenderTargetCache（架构债务续账步骤 2）
import asset_table;        // AssetTable（装配 helper 参数，F 拆出）
import meshlet_asset;       // MeshletAsset（★0-b2 簇产物容器元素，纯 CPU 只读视图）
import meshlet_geometry;     // MeshletGeometryStore（★1-a0 簇产物 4 段的 GPU 侧持有者）
import gltf_loader;         // GltfModel
import texture_pool;        // ModelTextureIds
import shader_registry;     // ShaderRegistry
import gpu_cull;            // GpuCull
import meshlet_cull_service;  // MeshletCullService（★1-a2-i 逐簇剔除的 GPU 侧载体，装配层持有）
import meshlet_shadow_args;   // MeshletShadowArgsService（★M-1a 真 MDI 行表的 GPU 载体，装配层持有）
import vk_indirect_commands;  // DgcCommands / DgcStaticCommands
import picking;              // PickingService（wirePicking 签名，Phase 10 批 10.5）
import vk_renderer;         // Renderer

namespace RGL {

/** 启动四配置包（camera/scene/runtime 按值持有；diagnostics 在 loadAllConfigs 内 setDiagnostics 应用）。 */
export struct StartupConfigs {
	CameraConfig camera;
	SceneConfig scene;
	RuntimeConfig runtime;
};

/**
 * 配置目录（相对 CWD）。**默认 `"configs"`**；环境变量 `RGL_CONFIG_DIR` 可覆盖为
 * 另一目录（绝对或相对 CWD 均可），供自动化测试把配置放到一次性目录里跑（现成使用者 =
 * `tools/smoke.py`，入口 `just smoke` / `just shot`；imgqa 的 profile 目录同款）。
 * 反面做法是改 `build/rgl/configs/` 副本：那份**不会**被下次 `just build` 自动复原
 * （`copy_if_different` 只在源更新时才覆盖，见 cmake/RglRuntimeAssets.cmake），于是
 * 配置出现两个真相源且"忘复原"是静默的——本 env 覆盖正是为绕开它而存在。
 *
 * **只覆盖配置目录**：资产路径（`models/` `env/` `skybox/` `generated_shaders/`）
 * 仍按 CWD 相对解析，契约不变（见 AGENTS.md「CWD 必须是 build/rgl/」）。
 * @return 配置目录路径（不带尾分隔符；env 未设 = `"configs"`）
 */
export std::string configDir();

/**
 * 配置文件完整路径 = `configDir() + "/" + fileName`。
 * @param fileName 配置文件名（如 `"scene.lua"`）
 * @return 拼接后的路径字符串
 */
export std::string configPath(const char* fileName);

/**
 * 加载四份启动配置（camera/scene/runtime/diagnostics）并应用诊断单例（setDiagnostics
 * 必须先于一切 GPU 对象构造——vkinstance 建 instance 时读 no_vvl）。
 * @return 空 optional = 任一配置加载失败（错误已在本函数内日志）
 */
export std::optional<StartupConfigs> loadAllConfigs();

/** 渲染意图图编译产物（图 + deferred 路线标志 + 路线意图枚举，按 variant 求值）。
 *  variant 随产物带出供 gate_lint 交叉校验（意图 vs 图事实，docs/gate_lint_plan.md §3-A）。 */
export struct CompiledGraphResult {
	CompiledRenderGraph graph;
	// B2-4③′（D5 第一刀）：图描述本体随产物带出 → move 进 Renderer::DynamicRenderGraph。
	// 图自此是脏驱动派生件；`graph` 只是**首编译**产物（main 已校验，Renderer 不重复编译）。
	RgGraphDesc desc;
	bool deferredMode = false;
	RgGraphVariant variant = RgGraphVariant::forward;
	/// 批 12.6：TAA 装饰是否落图（门开且图形态支持）。装配层据此决定 TAA RT/条目
	/// 是否领建——装饰失败（offscreen 变体）时条目建了也无人执行，不如整套不建。
	bool taaDecorated = false;
	/// 批 14.4：逐像素速度场装饰是否落图（门开且图形态支持）。装配层据此决定 velocity
	/// RT / velocity 条目是否领建。与 taaDecorated 同构，但**互相独立**：速度场是 FSR
	/// 的前置，不是 TAA 的一部分（判据全文 → docs/phase14_plan.md §1.10）。
	bool velocityDecorated = false;
	/// 批 14.5 第三批：AO 施加专用 pass 是否落图（FSR 门开**且** taa 不在图里**且**有 ao
	/// 附件）。装配层据此决定 ao_applied RT / ao_apply 条目是否领建，并把「AO 已在 tonemap
	/// 之前施加」这个标志置真（tonemap 据此跳过自己的乘法）。与 taaDecorated **互斥**：
	/// 两条路各有一个施加点，绝不双施。判据全文 → `shaders/ao_apply.frag.slang` 头注释。
	bool aoApplyDecorated = false;
	/// 批 15.1 前半：SSR pass 是否落图（门开**且**图里有 G-buffer position 附件 = deferred
	/// 形态）。批 15.3B 起 = trace + apply **两个 pass 一起**落图。装配层据此决定
	/// ssr_trace / ssr_applied RT 与两条目是否领建，并把色输入优先级链的
	/// 「ssr_applied 档」接上（taa / ao_apply / tonemap 的色输入槽跟着换）。与 taaDecorated
	/// 是**串联**关系（SSR 先合成、TAA 后滤波），不互斥。判据全文 → `shaders/ssr_trace.frag.slang`。
	bool ssrDecorated = false;
	/**
	 * B2-5：highlight 装饰**实际落图的那张申报单**（`applicable = false` = 门没开或图形态
	 * 不支持，与图里没有 highlight 一一对应）。
	 *
	 * 为什么必须随产物带出（而不是让装配层事后"重新算一遍"）：撤回这个 pass 时要还原的
	 * 跨 pass 补丁**原值**只存在于「装饰落图之前」的那一刻，事后重算问不到（图已经是
	 * 装饰后的样子）。装配层拿它调 `DynamicRenderGraph::adoptPass` 把它登记成可撤 ——
	 * 于是「启动期装饰」与「运行期插拔」吃的是同一张单子，A→B→A′ 往返判据不是自证。
	 */
	RgHighlightPlan highlightPlan;
};

/**
 * 启动前置产物（loadAllConfigs 之后、registry 之前）：渲染图自检 + headless/llm
 * 尺寸契约（headless+llm 时 headless_size 对齐 llm_shot.size）+ 建窗 + 派生量
 * （窗口 extent / llm 配置指针 / show_after 诊断门控）。
 */
export struct PreWindow {
	bool llmEnabled = false;
	bool headless = false;
	uint32_t headlessW = 0, headlessH = 0;  // 已按 llm 契约对齐
	uint32_t extentW = 0, extentH = 0;      // WindowExtent 初值（headless 用配置，窗口用实测像素尺寸）
	int showAfter = -1;
	bool diagShow = false;                  // show_after 诊断门控（showAfter ≥ 0 且非 headless）
	const LlmDebugConfig* llm = nullptr;  // llm 配置指针（llmEnabled 时非空）
	std::unique_ptr<SDLWindow> window;         // headless = nullptr
};

/**
 * 渲染图编译器自检（无 GPU，纯 CPU 图算法回归守卫，Phase 6 批次 2）——失败 =
 * 编译器/默认图被改坏，启动期报错退出（早于建窗，headless/llm 路径同样被覆盖）。
 * @return false = 自检失败（失败项已在本函数内日志）
 */
export bool checkRenderGraphSelfCheck();

/**
 * 建交互窗：headless = 不建 SDL 窗返回 nullptr（日志窗口尺寸；surface 由
 * Swapchain 走 vkCreateHeadlessSurfaceEXT）；llm 模式按 llm_shot.size 建离屏隐藏窗
 * 并日志一次性截图模式；show_after ≥ 0 = 隐藏窗启动（DIAG 可见窗 fault 排查）。
 * headlessW/H = 已按 headless+llm 尺寸契约对齐后的值（日志用）。
 * @return 窗口（headless = nullptr）
 * @note 建窗失败 → 抛 std::runtime_error
 */
export std::unique_ptr<SDLWindow> createAppWindow(const RuntimeConfig& runtime, bool llmEnabled,
												  bool headless, uint32_t headlessW, uint32_t headlessH);

/**
 * 启动前置（loadAllConfigs 之后、registry 之前）：渲染图自检 → headless/llm
 * 尺寸契约求值（headless+llm 时 headless_size 对齐 llm_shot.size——截图 = swapchain
 * extent）→ 建窗 → 派生量（extent / llm 指针 / show_after 诊断门控）。
 * @return 空 optional = 自检失败（错误已在本函数内日志）
 */
export std::optional<PreWindow> prepareWindow(const StartupConfigs& configs);

/**
 * push data 容量契约（Phase 5）：shader object + descriptor heap 模式无 pipeline
 * layout，per-draw push data（DrawArgs 4B——Phase 7b+ 仅 objectIndex，vkCmdPushDataEXT）
 * 受设备硬限约束
 *（双驱动实测 256B）：① maxPushDataSize（descriptor heap 专属 limit，
 * VUID-VkPushDataInfoEXT-offset-11243）② maxPushConstantsSize（经典
 * VkPhysicalDeviceLimits）。超限 = 设备不支持，启动期报错优于运行时静默布局错配。
 * @note 超限 → 抛 std::runtime_error
 */
export void checkPushDataLimits(const DeviceContext& device);

/**
 * Phase 5.5 IBL 风格契约：引用 model_pbr_ibl/model_vegetation/model_light 风格必须
 * 配 environment——frag 声明的 binding 4/5 无 mapping 烘焙 = 采样未写描述符（内容
 * 未定义；VVL 会抓 "no mapping for envSampler"，但启动期明确报错优于运行时未定义）。
 * 检查对象只取 styleMap 的值（风格真正引用的规格）：pipelines 表里存在未被引用的
 * ibl 条目是合法配置（addModelEntry 按引用建条目，无引用 = 不建，条目永不生效）；
 * 子串 contains 匹配还会误伤 model_pbr_ibl_v2 类未来命名（2026-09-03 教程拆解 ③：
 * 教程截图配置 styles.pbr="model_pbr" 但 pipelines 保留 ibl 条目被旧检查误杀）。
 * @note 违约 → 抛 std::runtime_error
 */
export void validateEnvironmentContract(const SceneConfig& scene);
/** 按 file 去重的已上传模型（instancing：同文件一次解析一次上传，entities 共享资产）。 */
export struct UploadedModel {
	std::string file;
	std::unique_ptr<GltfModel> model;
	std::vector<uint32_t> assetIds;    // per-primitive 网格资产 id
	ModelTextureIds textureIds;        // per-primitive 纹理资产 id 四元组（base/normal/orm/emissive，Phase 8 ③）
	uint32_t instances = 0;            // entities 引用计数（日志）
};

/** A0+A1+A2 上传产物（资产数据在 Resources 资产表；GltfModel 活到 C 阶段管线烘焙）。 */
export struct UploadedAssets {
	std::vector<UploadedModel> models;
	std::map<std::string, size_t> modelOf;  // file → models 下标
	std::optional<uint32_t> cubeAsset;      // 内建 cube 资产（entities 含 cube 时）
	// 批 12.6 T4 细线靶**不在此另立字段**：它是内存合成的 GltfModel，走的就是 models
	// 那条去重表（键 = aaTarget::kModelKey），字段化 = 第二份真相。
};

/**
 * 簇产物容器（★0-b2，装配层持有；docs/mdi_plan.md §9 ★0-b2）。
 *
 * 🔴 **为什么不进 UploadedAssets**（ECS 规范 5，"第二份真相"红线）：UploadedAssets 的
 * 语义是"**已上传 GPU** 的东西"，而 ★0-b2 只加载、只校验、只打日志，**什么都没上传**
 * ——放进去会让"uploaded"一词同时指两件事。落点 = 装配层独立持有（main_impl 与
 * AssetTable 同层声明）；解析出的 MeshletAsset 在这里**活着**，★1-a 直接消费本容器，
 * 不再走一遍装配管线。
 * 键 = 模型路径（SceneEntityDesc.file，CWD 相对，与产物路径约定同源）；
 * 值 = 经校验的只读视图（meshlet_asset 读取侧）。门开且加载成功才有条目——
 * 门关 = 空容器零装配（逐位不变构造性成立）。
 */
export struct MeshletAssetStore {
	std::map<std::string, MeshletAsset> byFile;
};

/**
 * A0 内建 cube 上传 + A1 模型解析+顶点上传 + A2 纹理上传。段契约：几何分配走几何段，
 * 纹理分配走纹理段；几何段期望槽号仍由 AssetTable::expectedGeometrySlot 校验，纹理首槽
 * = 白由纹理段首槽校验。
 * ★0-b2：A1 去重循环里挂簇产物**加载**（门 = `meshletAssets` **AND** 该模型路径旁有
 * `.meshlets.bin`）；★1-a0：加载成功后同处挂 **4 段 GPU 上传**（`meshletGeometry`，
 * 键与 `meshlets` 同源 = 模型路径；🔴 容器元素是 `VmaBuffer`，调用方必须让它在
 * `MeshletAssetStore` 同一作用域存活——AGENTS 生命周期红线）。
 * 门关 = 两者一行都不执行（逐位不变构造性成立）；门开 = 只多分配 buffer + 日志，
 * 不碰上传路径、不建条目、不领 heap slot、不写描述符 ⟹ 图像仍逐位不变。
 * @note 任一加载/上传失败 → 抛 std::runtime_error（启动期 catch 退出）；
 *       簇产物是**可选加速资产**，其加载失败与 GPU 上传失败**都不抛**（WARNING + 回落，
 *       见 meshlet_asset / meshlet_geometry）
 */
export UploadedAssets uploadModelAssets(DeviceContext& device, Resources& resources,
								 SamplerLibrary& samplers, AssetTable& assets, const SceneConfig& scene,
								 bool meshletAssets, MeshletAssetStore& meshlets,
								 MeshletGeometryStore& meshletGeometry);

/**
 * Phase 5.5 环境 cubemap 上传（scene.lua environment 段，faces 按 Vulkan 层序 0-5）+
 * SceneEnv 回填（intensity/mipRange）。**调用序契约**：env 纹理走纹理段，白纹理首槽由段
 * 首固定校验；env 上传仍须在管线烘焙前完成（addModelEntry 查 getEnvTexture 烘焙 binding 4/5）。
 * environment.hdr 非空 = 优先 Radiance .hdr 路径（.hdr 属 gitignore 本地素材，缺失告警
 * 回落 faces 六面 PNG，干净 checkout 仍可运行）。
 * @note 文件读取 / cubemap 创建失败 → 抛 std::runtime_error
 */
export void uploadEnvironment(Resources& resources, Environment& env, DeviceContext& device,
							  const SceneConfig& scene, entt::registry& registry);

/**
 * C1：管线条目烘焙（per **unique 模型**——instancing 共享条目；addModelEntry 按
 * (规格,顶点 slot,base/normal/orm/emissive 纹理 slot,sampler slot) 组合去重兜底）。
 * 风格派生：primitive.unlit → "unlit"，否则 "pbr" 再经 scene.lua material_rules pattern
 * 匹配（有序首个命中）；forward 经 styles 表（缺失 = 启动抛错）、deferred 经 deferred_styles
 * 表（缺失回落 deferred_geometry）。DIAG style_dist/tex_dist：per-模型风格/纹理 slot 分布日志。
 */
export void bakePipelineEntries(UploadedAssets& uploaded, const SceneConfig& scene, bool deferredMode,
								ShaderRegistry& pipelines, Resources& resources,
								SamplerLibrary& samplers, AssetTable& assets,
								RenderTargetCache& rtCache);

/**
 * 克隆管线条目（Phase 10 批 10.5 picking / 10.7 highlight，门 = `diagnostics.picking`
 * 默认关 = 零条目追加）。契约 = **模型条目烘焙完成后立即调用**（克隆只认 modelEntry 位，
 * 天空盒/光等后续条目在不在表内无关）。两个用途同门——高亮依赖点选出结果，单独开高亮
 * 无意义。★0-b2 起自 main() 抽出（main 行数红线 120，装配层预算被簇容器占满；
 * 纯代码搬移零行为变化）。
 */
export void addDiagnosticCloneEntries(ShaderRegistry& pipelines);

/**
 * C2/C2b：场景实体注册——per **entity**：模型 entity（rootTransform = T·Rx·Ry·Rz·S，
 * 旋转应用序 X→Y→Z）+ 独立实例矩阵槽；cube entity（内建资产 + 默认材质 + cube 管线条目，
 * 下标写回启动期一次幂等）；灯 entity（index = lights 列表位，正序契约：0 = 主光
 * forward @0 头，deferred 全数组按 index 落槽）。矩阵容量契约：实例槽超
 * kSceneMatrixCapacity → 抛 std::runtime_error（对象矩阵表越界写，启动期先拦截）。
 */
export void registerSceneEntities(entt::registry& registry, const UploadedAssets& assets,
								  const SceneConfig& scene, Resources& resources, AssetTable& table,
								  ShaderRegistry& pipelines);

/**
 * 簇化轴 ★1-a1ii-α：4 个簇 buffer 的 descriptor heap 槽号（装配层启动期领号一次，
 * 烘进 meshlet 管线条目的固定 binding 17-20；docs/mdi_plan.md §10.2「全局单例进固定
 * binding」）。字段序 = binding 升序（17/18/19/20）。
 *
 * ★1-a2-i 追加 `clusterVisible`（binding 21）：逐簇可见标志的槽号，**由 `MeshletCullService`
 * 领**（compute binding 3 与 mesh binding 21 同槽 ⟹ 读写两侧天然一致）。🔴 它不是「门
 * `meshlet.cull` 开才有」的可选槽——mesh SPV 只有一份、binding 21 无条件声明，门关时描述符
 * 照样必须有效（内容 = 全 1 = 全可见），所以它的生命周期跟着 `meshlet.assets` 走，不跟 cull。
 */
export struct MeshletBindingSlots {
	uint32_t vertexPool = 0;  ///< binding 17：顶点池（48B/顶点 ModelVertex 契约）
	uint32_t clusters = 0;    ///< binding 18：簇描述表（44B/条 DiskCluster）
	uint32_t tris = 0;        ///< binding 19：扁平 u8 三角索引流
	uint32_t remap = 0;       ///< binding 20：扁平 u32 顶点重映射流
	uint32_t clusterVisible = 0;  ///< binding 21：逐簇可见标志（★1-a2-i，MeshletCullService 的槽）
};

/**
 * 簇化轴的两个 **GPU 侧服务**持有者（★M-1a 起；🔴 装配层里那个变量的声明序 = 本结构的生命周期
 * 契约，见 `wireMeshletOverlay` 的 @return）。
 *
 * 为什么合成一份而不是两个 `auto`：`main()` 的代码行数已顶到红线（120），而两格的所有权规则
 * **完全相同**（都要比 Renderer 活得久）、时序却不同（cull 服务在注册**前**建、行表载体必须
 * 在注册**后**建 = 它要读选级表这个生效事实）。⟹ 一次装配出两格，各自在能算出闭式的那一刻填，
 * 而不是为第二格再要一行声明。
 *
 * ⚠ 两格都可以是 nullptr（门关 / 无产物 / 多产物回落 / 生效事实没成立）；「nullptr 是不是
 * 应该出声」由各接线函数自己判读（配置说开了而没跑 = 空开关，本项目反复踩的形态）。
 */
export struct MeshletAssembly {
	std::unique_ptr<MeshletCullService> cullService;       ///< ★1-a2-i 逐簇剔除的 GPU 载体
	std::unique_ptr<MeshletShadowArgsService> shadowArgs;  ///< ★M-1a 真 MDI 行表的 GPU 载体（零消费者）
	/** ★1-a1ii-α 的 4+1 簇 buffer heap 槽快照（★M-1b：第⑤步烘 MDI 条目要复用 binding 17-20，
	 *  而 slots 的产地在 `wireMeshletOverlay` 的局部——nullopt = 门关 / 无产物 / 多产物回落，
	 *  与 `bakeShadowMeshletEntries` 的早退判据同一个事实）。 */
	std::optional<MeshletBindingSlots> bindingSlots;
};

/**
 * 簇化轴 ★1-a1ii-α：影子叠加装配总入口 = ① 簇 buffer 描述符接线（领 4 个 Runtime 段
 * heap 槽 + 把 `MeshletGeometryStore` 四段 BDA 写成 storage buffer 描述符，binding
 * 17-20 固定；ensureIzbSlots fallback 同法 host 直写，单份产物前提与多产物回落 →
 * 定义处注释）→ ② 为**有簇产物的模型**逐 primitive 烘叠加管线条目（与
 * bakePipelineEntries 同风格派生 = resolvePrimitiveSpecName 同一函数 = 同 frag；
 * addMeshletEntry 按纹理集去重）写回资产表 meshletPipelineIndex。
 *
 * 调用序契约 = `bakePipelineEntries` / `addDiagnosticCloneEntries` 之后（meshlet 条目
 * modelEntry=false 且晚于克隆调用 = picking/highlight 重放天然跳过）、`assembleBackEnd`
 * 之前；三个 sampler/RT 槽经各自幂等 ensure 重取 = 与模型条目同号。门关
 * （meshletAssets=false）= 连槽号都不领、一条条目都不建（逐位不变构造性成立）。
 * @param cfg 运行配置（读 meshletAssets 门 + meshletDebug/meshletTint 两枚 SPV 变体位：
 *   debug = 阳性对照（判据 3），tint = ★0.5-b2 双臂阳性对照，同真时 tint 优先 + warn。
 *   🔴 债-105 起这两枚**只在 meshlet.overlay 开时**才被采纳——变体烘进的是 `switch` 原位取代
 *   共用的那批条目，叠加段关着也能改条目 = 诊断渗进产品路 ⟹ 关时强制 base + 偏离默认才 warn）
 * @param sceneData scene data 服务（★1-a2-i：`MeshletCullService` 要它的槽号与 buffer 句柄）
 * @return 簇化轴的 GPU 侧服务持有者 `MeshletAssembly`（★M-1a 起两格）：
 *   · `cullService`（★1-a2-i）= 逐簇视锥剔除服务；门不开 / 无产物 / 多产物回落 = nullptr。
 *     🔴 **构造判据是 `meshlet.assets` 而不是 `meshlet.cull`**——mesh SPV 只有一份、binding 21
 *     无条件声明 ⟹ 标志 buffer 与它的描述符在门关时也必须存在（内容 = 全 1 = 全可见）。
 *     「门开但要不要 dispatch」是另一件事，归 `wireMeshletCull`。
 *   · `shadowArgs`（★M-1a）**本函数恒留空**：行表载体的每段行数上界要读注册**之后**才 emplace
 *     的选级表（生效事实），填它的时机 = `wireMeshletPostRegistration` 第四步。
 *   🔴 所有权必须落到装配层里**先于 Renderer** 声明的变量（两格都持 GPU buffer，Renderer 只
 *   持裸指针；AGENTS 生命周期红线：块尾析构 = 帧循环里读到已销毁的 VA）。
 */
export MeshletAssembly wireMeshletOverlay(
	Resources& resources, DeviceContext& device,
	const MeshletGeometryStore& meshletGeometry, const UploadedAssets& uploaded,
	const SceneConfig& scene, bool deferredMode, ShaderRegistry& pipelines, AssetTable& assets,
	SamplerLibrary& samplers, RenderTargetCache& rtCache, const RuntimeConfig& cfg,
	SceneDataService& sceneData);

/**
 * ★M-1a（docs/mdi_plan.md §19.5）：真 MDI 阴影段**行表 GPU 载体**的接线（零消费者，一条 draw 都不接）。
 * **调用点 = `wireMeshletPostRegistration` 第四步**（要读它 emplace 的 `MeshletShadowLodTable`
 * = 「阴影簇路径真生效」的事实），产物落进 `MeshletAssembly::shadowArgs`（🔴 声明序先于
 * Renderer = 析构序在其后；本服务持一张 GPU buffer，AGENTS 生命周期红线）。
 *
 * 建不建 = 宿主条件 `meshlet.assets` + `meshlet.shadow_lod ≥ 0` + 选级表在 ctx；门关 =
 * **连号都不领**（先例 `wireMeshletBindings`）。🔴 生效条件里**不含** `meshlet.mdi_shadow`
 * （§19.6 挂号那条：载体与「改用 MDI 形态」是两件事，后者才选 SPV 变体 / 选条目）。
 * @param registry 场景 registry（🔴 必须**实体注册之后**：读选级表这个生效事实）
 * @return 载体服务；门关 / 阴影簇路径未生效 / 每段行数上界为 0 = nullptr
 */
export std::unique_ptr<MeshletShadowArgsService> wireMeshletShadowArgs(entt::registry& registry,
	Resources& resources, DeviceContext& device, const RuntimeConfig& cfg);

/**
 * 簇化轴 ★1-a2-i：逐簇视锥剔除的**生效开关**（`meshlet.cull` → Renderer 帧首 dispatch）。
 * **调用序契约 = `assembleBackEnd` 之后**（Renderer 已构造，与 `wireMeshletOverlayDepthCompare`
 * 同位同法），且**必须晚于** `wireMeshletPostRegistration`（第 ③ 步要读它 emplace 的
 * `MeshletSwitchOn`）。
 *
 * 四步：门关早退（零调用零日志）→ 服务缺位（多产物回落 / 无产物）时门开**出声**（配置说开而
 * 实际没跑 = 判据全绿而结论是假的，同 switch 的第二道机器护栏）→ 消费者缺位出声且不 dispatch →
 * setter + INFO 打印生效档。
 *
 * 🔴 第 ③ 步读的是 ctx 里的 `MeshletSwitchOn`（**生效**事实）而不是配置里的 `meshletSwitch`
 * （**意图**）：switch 有三条回落路（设备发射规格不够 / 非 cpu cull 档 / 逐资产对账失败）都只
 * emplace 失败不写配置，读配置会让剔除在这些档下照跑而取代根本没发生 = 空转。判定只有一处的
 * 纪律同 `render_list_impl.cppm` 的 `meshletInPlace`。
 *
 * 🔴 门关的形态 = Renderer 成员恒 nullptr ⟹ 帧首**零新命令**（连 barrier 都不发）。逐位不变
 * 的构造性保证在这里，不在「dispatch 了但标志全是 1」。
 */
export void wireMeshletCull(Renderer& renderer, entt::registry& registry,
	const RuntimeConfig& cfg, MeshletCullService* cullService);

/**
 * 簇化轴 ★0.5-b2：共面双臂深度比较接线（`meshlet.coplanar` → Renderer 叠加段的动态
 * `depthCompareOp`）。**调用序契约 = `assembleBackEnd` 之后**（Renderer 已构造；与
 * `wireMeshletOverlay` 的「之前」契约互补，两步各自门早退）。
 *
 * 语义：off = 传 nullopt = 执行器**一条 setDepthCompareOp 都不发**（门关与 off 档的
 * 动态状态序列逐 bit 不变 = A1 判据的构造性保证）；equal/ne = 叠加那批 draw 的深度比较
 * 换成 eEqual/eNotEqual（只作用叠加 draw，pass 与旧路径状态不动）。
 *
 * 🔴 门开时 INFO 打印**生效**档（独立于图像的生效证据）：本批主判据 = 四臂截图哈希全同，
 * 若 coplanar 静默落回 off，四臂照样全绿 = 整批假绿——解析期 fail-fast 拦不住「Lua 里
 * 值写成裸标识符 = nil = 键缺失」这一种，故生效日志是第二道机器护栏。门关 = 零日志
 * （门关 grep meshlet = 0 的护栏）。
 * @param renderer 已装配的后端（setMeshletOverlayDepthCompareOp 目标）
 * @param cfg 运行配置（读 meshletAssets / meshletCoplanar 两字段）
 */
export void wireMeshletOverlayDepthCompare(Renderer& renderer, const RuntimeConfig& cfg);

/**
 * 「Renderer 装配后挂」三连（★M-1b；调用序契约 = assembleBackEnd 之后，main_impl 一次调用）：
 * coplanar 双臂 → 逐簇剔除生效档 → MDI 行表载体指针。成员函数各自的判据/出声口径不变，
 * 本入口只固定顺序与 main() 的行数预算（三行合一行；定义在 main:meshlets）。
 * @param registry 场景 registry（wireMeshletCull 读取代生效事实）
 * @param renderer 已装配的后端
 * @param cfg 运行配置
 * @param assembly 服务持有者（cullService / shadowArgs 裸指针的来源）
 */
export void wireMeshletRendererServices(entt::registry& registry, Renderer& renderer,
	const RuntimeConfig& cfg, MeshletAssembly& assembly);

/**
 * 簇化轴 ★1-a1ii-β-1：实体注册之后的簇轴装配合成一次调用（`main()` 已在 120 行代码
 * 红线，`wireSceneSpinContext` 同法抽 helper）= ① `wireClusterRangeTable`（簇区间表；★18-B2
 * 起同一个循环还建阴影远级联 LOD 选级表）→ ② `wireMeshletSwitch`（`meshlet.switch` 落地）
 * → ③ `wireMeshletOverlayActivation`（债-105：`meshlet.overlay` 落地）。
 * 顺序契约内聚在定义处：区间表**先**建，
 * ② 的逐资产对账要读它的逐槽 l0Count；③ 只读配置 + 资产表里的**条目事实**。
 *
 * ② 的语义：`assets && switch` 时，门开资产（有簇管线条目 + L0 簇数 > 0 + 非 blend）在主
 * opaque 段里由簇路径 draw **1:1 原位**顶掉旧 mesh draw（同 objectIndex / 同 pushData /
 * 同段内位置，只换管线条目与 task 数），同时清空 α 的影子叠加段（互斥）。三态语义与
 * 「为什么是原位而不是另开一段」→ `RuntimeConfig::meshletSwitch` 注释；生效标记 = registry
 * ctx 里的 `MeshletSwitchOn`（存在即生效，消费者 = renderListSystem）。
 *
 * 两道**不生效**闸门（各自 warn 出声、保持旧路径，绝不静默半切）：
 *  · `diagnostics.cull_replay != "cpu"` → warn「switch 在非默认 cull 档不生效」（gpu_sync 档
 *    的 opaque 段按候选表重放，段内换管线不在那条路上）；
 *  · 逐资产对账失败（有簇条目却无 L0 簇）→ warn 并停用（半切形态的画面可能逐位不变而结论假）。
 *
 * 🔴 门开且切档时 INFO 打印**生效**档（同 coplanar 的第二道机器护栏：主判据是截图哈希，
 * 开关静默不落地时哈希也可能全绿）。门关 = 零 ctx 零日志。
 *
 * ③ 的语义（**债-105**）：`meshlet.overlay` 落地 = emplace `MeshletOverlayOn`，消费者 =
 * `collectMeshletOverlay` 的第五枚判据。生效条件**不含** `assets`（本键的宿主是叠加段自己），
 * 但 assets 关时条目没烘 ⟹ 孤立组合按**事实**（有无资产带簇管线条目）出声，不静默空开关。
 *
 * 调用序契约 = `registerSceneEntities` 之后（三步都读 MeshRef）；与
 * `wireMeshletOverlayDepthCompare` 互不依赖。
 * @param registry   场景 registry（区间表与生效标记的 emplace 目标）
 * @param uploaded   已上传资产（区间表扫描输入）
 * @param meshlets   簇产物容器（区间表扫描输入）
 * @param assetTable 资产表（对账读 pipelineIndex / groupCount / meshletPipelineIndex）
 * @param cfg        运行配置（读 meshletAssets / meshletSwitch / meshletCoplanar）
 * @param device     设备上下文（**只读物理设备属性**：核对簇路径的发射规格，见 checkMeshletDeviceFit）
 * @param assembly   ★1-a2-i / ★M-1a：`wireMeshletOverlay` 装配出来的服务持有者（就地填两格）。
 *   `cullService` = 逐簇剔除服务，剔除项表扫出来后**就地上传**给它（同一处产出 cullBase 与
 *   items，分两处传 = 迟早漂）；`shadowArgs` = 本函数**第四步**新建的 MDI 行表载体（🔴 只能
 *   在这里建：它的每段行数上界要读第三步 emplace 的选级表 = 生效事实）。
 * @param pipelines  ★M-1b：管线表（第五步烘真 MDI 条目 + 逐资产回填的目标，
 *   `addShadowMeshletMdiEntry` 的槽号读 `assembly.shadowArgs->argSlot()`——时序接缝的解法
 *   是「条目烘焙排在载体建成之后（第⑤步）」而不是领槽前移，理由与雷 1/雷 2 的快照分析
 *   → wireMeshletMdiShadowActivation 头注释；本函数整体仍在 `assembleBackEnd` **之前**）。
 */
export void wireMeshletPostRegistration(entt::registry& registry, const UploadedAssets& uploaded,
	const MeshletAssetStore& meshlets, const AssetTable& assetTable, ShaderRegistry& pipelines,
	Resources& resources,
	const RuntimeConfig& cfg, DeviceContext& device, MeshletAssembly& assembly);

/**
 * 材质风格 → 管线规格名派生（★1-a1ii-α 起 bakePipelineEntries 与 bakeMeshletEntries
 * **共用**——叠加与本体必须走同一 material_rules / deferred_styles 映射，否则 frag
 * 静默接错；原为 bakePipelineEntries 内 lambda，抬出 = 单一真相源）。
 * 风格 = prim.unlit ? "unlit" : "pbr" 再经 scene.lua material_rules pattern（有序首个
 * 命中）；deferred 经 deferred_styles（缺失回落 deferred_geometry）、forward 经 styles
 * 表（缺失 = 启动抛错，同原行为）。
 */
export std::string resolvePrimitiveSpecName(const SceneConfig& scene, bool deferredMode,
	const GltfPrimitive& prim);

/**
 * 簇化轴 ★1-a1i：把簇产物的**逐 prim 簇区间**落到 per-objectSlot 表（registry ctx
 * `ClusterRangeTable`），供 renderListSystem 每帧搬进 `SceneFrameData::clusterRanges`
 * → scene data 尾区 `ObjectClusterRange` 表（@724224，stride 16）。
 *
 * **调用序契约 = `registerSceneEntities` 之后**：落表要读 `MeshRef{asset, objectSlot}` 与
 * `NextObjectSlot`，两者都在实体注册时才成形。区间本身来自
 * `meshlet_ranges::scanClusterRanges`（扫产物 groups，校验 §10.1 四条结构不变量）。
 *
 * 门 `meshlet.assets` 关 = 首行返回、ctx 不建条目 ⟹ 执行器把该区写成**全 0**（逐位不变）。
 * 门开但扫描/对账失败 = WARNING + 该模型回落现行路径（簇产物是可选加速资产，不砖引擎）。
 *
 * ★18-B2 起**同一个 MeshRef 落表循环还建第二张表**：阴影远级联 LOD 选级表
 * `MeshletShadowLodTable`（registry ctx，存在即生效）。前置三步 = 门（`meshlet.shadow_lod < 0`
 * = 零 ctx 零日志）→ 设备发射规格闸门（warnIfMeshletDeviceUnfit，理由同 β-1）→ 同一份扫描
 * 逐模型 `scanMeshletLod`；后置两步 = 逐资产对账（`auditMeshletSwitch` 形态，「半切」挡在
 * 生效之前）+ Σtask/派发上限机器对账 + emplace + INFO（§18.6 判据 6 的常驻输出）。任一步
 * 不过 = **不 emplace**（阴影全走旧路径，与门关逐位相同）；LOD 停用不连坐 ClusterRangeTable。
 * 定义在 main:meshlet_tables（建表半区，与区间表同文件保证「同一份扫描、同一个循环」）。
 * @param registry 已注册实体的 registry（读 MeshRef / NextObjectSlot，写 ctx）
 * @param assets 上传产物（`models[].file` 与 `meshlets` 同源；`assetIds` 与 prim 下标对齐）
 * @param meshlets 簇产物容器（★0-b2；门关 = 空）
 * @param meshletAssets 门 `runtime.lua meshlet.assets`（关 = 一行都不走）
 * @param meshletCull 门 `runtime.lua meshlet.cull`（★1-a2-i；关 = 不建剔除项表、cullBase 保持 0
 *        ⟹ scene data 该区与 β-1 逐位同，构造性保证不靠"读了但结果一样"）
 * @param cullService 逐簇剔除服务（★1-a2-i，可空 = 多产物回落）。门开时把刚扫出的剔除项表
 *        **就地**上传给它——cullBase 与 items 必须同生共死，分两处交出去迟早漂
 * @param assetTable 资产表（★18-B2 逐资产对账读 shadowPipelineIndex / shadowMeshletPipelineIndex）
 * @param cfg 运行配置（★18-B2 读 meshletShadowLod / meshletShadowLodFallback）
 * @param device 设备上下文（★18-B2 只读物理设备属性：发射规格 + maxMeshWorkGroupCount.x 上限）
 */
export void wireClusterRangeTable(entt::registry& registry, const UploadedAssets& assets,
								  const MeshletAssetStore& meshlets, bool meshletAssets, bool meshletCull,
								  MeshletCullService* cullService, const AssetTable& assetTable,
								  const RuntimeConfig& cfg, const DeviceContext& device);

/**
 * 设备发射规格核对的结果（★1-a2-i；★18-B2 抬出 = switch 与阴影 LOD 两条装配路径共用，
 * 定义在 main:meshlet_tables）。四个上限值供 warn 归因与「单 draw 最大 task」对账读取。
 */
export struct MeshletDeviceFit {
	bool ok = true;
	uint32_t maxOutputVertices = 0;
	uint32_t maxOutputPrimitives = 0;
	uint32_t maxWorkGroupInvocations = 0;
	uint32_t maxMeshWorkGroupCountX = 0;  ///< 单 draw task 派发上限（★18-B2 阴影 LOD 对账读它）
};

/** 查询设备是否吃得下簇路径的发射规格（口径 → 定义处注释；定义在 main:meshlet_tables）。 */
export MeshletDeviceFit checkMeshletDeviceFit(const vk::raii::PhysicalDevice& physicalDevice);

/** 规格不够就 warn 并返回 false（调用方必须停用簇路径保持旧路径；定义在 main:meshlet_tables）。 */
export bool warnIfMeshletDeviceUnfit(const DeviceContext& device);

/**
 * C3+deferred+DGC+Renderer 装配产物（成员析构逆序 renderer→dgcStatic→dgc→cull，与
 * 拆分前 main 局部变量声明序一致）。cull 堆分配：Renderer 持 GpuCull*，聚合返回时
 * 局部对象析构会悬空指针——堆地址跨 move 稳定（启动期一次分配，无每帧开销）。
 * dgc/dgcStatic 空 = 读回重放路。
 */
export struct RenderBackEnd {
	std::unique_ptr<GpuCull> cull;
	std::unique_ptr<DgcCommands> dgc;
	std::unique_ptr<DgcStaticCommands> dgcStatic;
	Renderer renderer;
};

/**
 * C3+deferred+HDR+DGC+Renderer 装配（时序关键；运行期辅助 heap 槽走运行时段）：GPU culling
 * 候选表（守卫/迭代序与 render_list CPU 路径**逐位一致**——重放序 BIT-IDENTICAL 前提，
 * 透明段不入候选）→ GpuCull 构造（静态 DGC 流条目数快照 = 此刻管线表条目数，sky/light
 * 条目其后加入非 candidate）→ skybox 条目（env 已上传才建 + SceneEnv 回填）→ deferred
 * G-buffer RT 堆槽 + light 条目（deferredMode）→ scene_hdr RT + tonemap 条目（无条件）
 * → DGC 命令生成（能力门控，构造异常回落读回重放路）→ 选路日志 → Renderer 构造（构造期
 * 校验 drawLight pass 有 light 条目）。
 * @note G-buffer / scene_hdr RT 创建失败 → 抛 std::runtime_error
 */
export RenderBackEnd assembleBackEnd(DeviceContext& device, Swapchain& swapchain, Resources& resources,
									 SceneDataService& sceneData, SamplerLibrary& samplers, Environment& env,
									 RenderTargetCache& rtCache, AssetTable& assets,
									 ShaderRegistry& pipelines, CompiledGraphResult&& graphResult,
									 entt::registry& registry, const SceneConfig& scene,
									 bool fsrEnabled = false, bool fsrTemporal = false,
									 bool fsrJitter = false);

/**
 * 配置热重载装配（Phase 10 批 10.3；docs/archive/phase10_plan.md §5）。门 =
 * `diagnostics.config_reload`（默认 false = 直接返回零变化）；监视源 = **源树**
 * configs/（编译期定义 RGL_SOURCE_CONFIG_DIR，D7——build 副本会被 just build 覆盖
 * 复原，监听无意义；目录缺失 warn + 关）。挂 `ConfigReloadHook`（ecs ctx）由
 * renderSystem 帧边界轮询 mtime，变更即重解析 + 套用 T1 字段：
 *   · scene.lua  → SceneSpin.speed（spin 开关）/ SceneEnv.exposure / SceneEnv.intensity
 *     （entities/灯/资产/环境贴图**不**热载——重注册牵动 heap 槽与管线表，超 T1 范围）
 *   · camera.lua → Camera 调参字段（fov/near/far/fpsSpeed/各灵敏度/键位）；
 *     **姿态与模式不套**（运行中相机已漂移，position/target 覆盖 = 瞬移；mode 切换
 *     涉及 basis 状态语义，重启生效）
 *   · log_level.json → RLLogger::reloadLevels（只改 sink level）
 * 解析失败 = warn + 保留旧值（不崩）。imgui 面板开关本身 = 启动期（需重建后端）。
 * @param registry 场景 registry（写回调捕获：SceneSpin/SceneEnv/Camera ctx）
 */
export void wireHotReload(entt::registry& registry);

/**
 * shader 热重载装配（Phase 10 批 10.4，L1 = D2 同签名才热换；docs/archive/phase10_plan.md §6）。
 * 门 = `diagnostics.shader_reload`（默认 false = 直接返回零变化）。独立于 config_reload
 * 但**复用同一 ConfigReloadHook 帧边界挂点**（已 emplace = 组合追加，未 emplace = 独占
 * emplace）。装配 = 构造 ShaderHotReloader（基线 = 在役 SPV hash+接口签名）+ init
 * （失败降级关：编译期三件套缺失 / 基线 SPV 读不到）；poll 时 fork slangc 全量重编源
 * rgl/shaders/，同签名条目同槽重建（pipelineIndex 不变 = 资产表/cull 零改动）。
 * @param registry 场景 registry（ConfigReloadHook ctx 挂点）
 * @param pipelines 管线表（基线来源 + build/commitReplacement 宿主）
 * @param backEnd 后端（waitForIdle 宿主 + DGC 两路可空指针——换入前清 in-flight、
 *                execution set / 静态 shader 对刷成新句柄后才销毁旧句柄）
 */
export void wireShaderHotReload(entt::registry& registry, ShaderRegistry& pipelines,
	RenderBackEnd& backEnd);

/**
 * GPU id 点选装配（Phase 10 批 10.5，D3；docs/archive/phase10_plan.md §7）。
 * 门 = `diagnostics.picking`（默认关 = 返回 nullptr 零装配）。装配 = 构造服务 +
 * `Renderer::setPickingService` + `PickingHook`（registry ctx）双向函数桥：
 * requestPick = eventSystem 鼠标按下（未被 imgui InputFilter 消费的事件）→ 服务
 * 请求；takeResult = 帧边界 pickingTick 消费回读 objectSlot → 反查 MeshRef entity
 * → 写 `SelectedObject` ctx（面板实体上下文读）。
 * @param registry 场景 registry（hook ctx 挂点）
 * @param resources 资源层（服务 staging）
 * @param rts offscreen RT 缓存（服务定位 object_id RT）
 * @param renderer 渲染器（图访问 + 服务挂点）
 * @return 服务实例（nullptr = 门关）；所有权归调用方（main 栈，析构须早于 registry）
 */
export std::unique_ptr<PickingService> wirePicking(entt::registry& registry,
	Resources& resources, RenderTargetCache& rts, Renderer& renderer);

}  // namespace RGL
