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

/**
 * 加载 configs/render_graph.lua + compileRenderGraph（校验 + barrier 布局链推导，任何图
 * 错误启动期抛 std::runtime_error）+ variant 求值 deferredMode（Phase 7 路线选路：
 * forward(默认)/deferred/offscreen——后两者为内建 exclusive 图变体）。
 * @param taaEnabled 批 12.6：「TAA 要不要进图」（true = decorateTaaPass，结果随
 *        CompiledGraphResult::taaDecorated 带回；offscreen 变体装饰失败 = false）。
 *        批 14.6 起调用点传 `RuntimeConfig::aaWantsTaa()`（原 scene.lua taa.enabled 已删）
 * @param velocityEnabled 批 14.4：configs/runtime.velocity_field（逐像素速度场装饰；
 *        aa 为 fsr 时该键已在解析期被隐含打开，本形参对隐含关系无感知）
 * @param fsrEnabled      批 14.5 第三批：「FSR 要不要接」（批 14.6 起调用点传
 *        `aaWantsFsr()`，原 fsr_upscaler 键已删）。它在这里**只**决定
 *        「要不要插 AO 施加专用 pass」（FSR 当时域滤波器 ⟹ AO 必须在它之前施加）；
 *        FSR 本身能不能接是装配层的事，两者不耦合（理由见 decorateAoApplyOrWarn）。
 * @param ssrEnabled      批 15.1 前半：configs/runtime.lua `ssr.enabled`（屏幕空间反射
 *        装饰；与 aa 三态键正交，结果随 CompiledGraphResult::ssrDecorated 带回）。
 */
export CompiledGraphResult compileGraphOrThrow(const char* path, bool taaEnabled,
	bool velocityEnabled = false, bool fsrEnabled = false, bool ssrEnabled = false);

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
 * ★0-b2：A1 去重循环里挂簇产物**加载**（门 = `meshletEnable` **AND** 该模型路径旁有
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
								 bool meshletEnable, MeshletAssetStore& meshlets,
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
