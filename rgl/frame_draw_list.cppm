module;
#include <glm/glm.hpp>
export module frame_draw_list;

import rlstd;
import vk_resources;
import lights_types_generated;
import material_types_generated;  // DrawArgs（Phase 7b cullArgs：GPU culling 候选 args）
import scene_forward_types_generated;  // 前向头 SceneLight（C API 反射生成，替换原 LightDataGpu）
import scene_data;  // kSceneShadowCascadeCount（阴影 VP 区容量；单向依赖：scene_data 不认识本模块）

namespace RGL {

// 绘制契约（ECS 规范 4：渲染 system 产出、执行器盲执行的唯一接口）。
// 2026-09-02 拆自 vkrenderer：struct 与 Renderer 执行器解耦——ecs/llm_debug/main 只
// import 本模块即可表达契约，不必拖入整个 Renderer（Vulkan 执行层）。
//
/**
 * 一次绘制（FrameDrawList 的条目；由渲染 system 每帧产出，drawFrame 消费）：
 *   pipeline   = 管线表下标（直接取自资产记录 MeshAsset.pipelineIndex /
 *                SceneEnv.skyboxPipeline——启动期经 ShaderRegistry::bindPipelineToAsset
 *                写回，每帧路径零字符串查找，构造后稳定；见 render_list_impl）
 *   pushData   = 本次 draw 的 push constant，布局随管线（CPU 内存录制时读取——
 *                vkCmdPushDataEXT 只要求内存活到录制结束，故帧局部存储即可）
 *   groupCount = drawMeshTasksEXT 的 X（Y/Z 现状恒 1，见 vkrenderer draw 循环）
 */
export struct DrawCall {
	uint32_t pipeline;                    // 管线表下标（ShaderRegistry::entries 下标）
	std::span<const std::byte> pushData;  // push constant 字节序列（DrawArgs，帧局部存储）
	uint32_t groupCount;                  // mesh task group 数（资产记录 MeshAsset.groupCount）
};

/**
 * 阴影 pass 的 push constant（Phase 11 批 11.2，8B，与 DrawArgs 不共用块）。
 *
 * **为何不直接给 DrawArgs 加一个 cascadeIndex 字段**：DrawArgs 是 material.slang
 * 的全局共享契约（9B1 条模型管线条目全部按它烘焙 binding mapping，改布局
 * = 整表重建）；而阴影只需多带一个索引。两者的区别在 shader 侧已有例子（体积几何
 * 与牌展示两类管线各自带推理块）。
 *
 * 两个字段都是**纯索引**（AGENTS bindless 原则）：objectIndex 索 scene data 各
 * per-object 表（与 DrawArgs.objectIndex 同值），cascadeIndex 索阴影 VP 区槽。
 */
export struct ShadowDrawArgs {
	uint32_t objectIndex = 0;   // 与 DrawArgs.objectIndex 同值（MeshRef.objectSlot）
	uint32_t cascadeIndex = 0;  // 阴影图分块号（对应 shadow.mapDrawSource 名后缀）
};
static_assert(sizeof(ShadowDrawArgs) == 8,
	"ShadowDrawArgs layout drifted from shadow.mesh.slang ShadowDrawArgs (scalar layout)");

/**
 * 阴影段**簇路径** pass 的 push constant（簇化轴 ★18-B1，12B；spec = "shadow_meshlet"）。
 *
 * = `ShadowDrawArgs` 的两字段原样 + 一枚 `clusterStart`。为什么不复用 8B 那份：本管线要多带
 * 一个选级偏移，而 8B 那份是 shadow.mesh.slang 与 izb_mask 共用的既有契约（改了 = 既有条目的
 * argsSize 全表重烘焙）。§18.3 三条扩法里选「偏移走 push constant」正是为了这条：零 scene
 * data 变更、零反射重跑、旧 shadow 管线一字节不动。
 *
 * 三个字段全是**纯索引/偏移**（AGENTS bindless 原则）。
 */
export struct ShadowMeshletDrawArgs {
	uint32_t objectIndex = 0;    // 与 DrawArgs.objectIndex 同值（MeshRef.objectSlot）
	uint32_t cascadeIndex = 0;   // 阴影图分块号（同 ShadowDrawArgs.cascadeIndex）
	/**
	 * 🔴 **相对 `ObjectClusterRange::base` 的偏移**，不是全局簇下标——口径 = 批 A 的
	 * `MeshletLodPrimRange::start`（meshlet_lod.cppm）。shader 侧 `cluster = base +
	 * clusterStart + (groupID.x >> 1)` 里 base 由 `readObjectClusterRange(objectIndex)` 提供，
	 * 两者拼起来才是簇下标；传全局下标会在「同资产多 prim / 多实例」上错（base 被加两遍）。
	 */
	uint32_t clusterStart = 0;
};
static_assert(sizeof(ShadowMeshletDrawArgs) == 12,
	"ShadowMeshletDrawArgs layout drifted from shadow_meshlet.mesh.slang ShadowMeshletDrawArgs (scalar layout)");

/**
 * 场景级每帧动态数据（Phase 5 批次 2，bindless）：渲染 system 产出、drawFrame 消费——
 * CPU 写 staging[currentFrame] + 录制期 vkCmdCopyBuffer 拷进 scene data buffer
 *（布局契约见 scene.slang SceneLight / scene_forward_types_generated）。objectMatrices 按
 * **对象矩阵槽**（MeshRef.objectSlot，实例级；DrawArgs.objectIndex = 同值——Phase 6
 * instancing：同 asset 多实例各持独立槽，**非**资产 id 索引），span 指向渲染 system
 * 帧局部存储（录制期读取即完成使命，同 DrawCall.pushData 生命周期规则）。
 */
export struct SceneFrameData {
	SceneLight light;  // scene data @0 头单灯（forward 消费 lights[0]，布局见 scene_forward_types_generated）
	// Phase 5.5 env 参数（scene data 头 @48/@52，原 16B 保留区启用；shader 端
	// env.slang readEnvParams 字面量一致）：x=envIntensity y=envMipRange（=L−1，0 = 无环境）
	glm::vec2 envParams{0.0f};
	// Phase 8 收尾：tonemap 曝光（scene data 头 @56 保留区，tonemap.frag 直读；
	// ACES 前线性倍率，1.0 = 直通旧行为）
	float exposure = 1.0f;
	// Phase 11 批 11.4：SSAO 施加强度（scene data 头 @60，头部保留区最后一格；
	// tonemap.frag `lerp(1, ao, strength)` 的 strength，0 = 逐位旁路）
	float aoStrength = 0.0f;
	// Phase 8 批次 ④a：漫反射辐照度 radiance SH9（scene data 尾区 kSceneDataIrradianceOffset，
	// 9×vec4 .xyz=系数，scene.slang irradiance(N) 消费）；renderListSystem 每帧从
	// Resources::getEnvIrradianceSH 填。
	std::array<glm::vec4, 9> irradianceSH{};
	// Phase 5.5 批次 4：天空盒 clip→world 矩阵（scene data @64，sky.slang skyClipToWorld；
	// = inverse(proj·view)，renderListSystem 每帧从 ViewProjection 推导）
	glm::mat4 skyInvViewProj{1.0f};
	std::span<const glm::mat4> objectMatrices;  // scene data @128 对象矩阵表（spin·world，按 objectSlot 索引）
	// Phase 7b+（NV DGC 规避）：per-object MVP 表（scene data @kSceneDataMvpTableOffset，
	// 按 objectSlot 索引，值 = 原 DrawArgs.mvp 位级一致；shader readObjectMvp）
	std::span<const glm::mat4> objectMvps;
	// Phase 7b+（续）：per-object 材质/几何表（scene data @kSceneDataDrawTableOffset，
	// 按 objectSlot 索引；shader readObjectDraw 消费，DrawArgs 仅剩 objectIndex）。
	// Phase 10 批 10.2a 单源化：条目类型 = scene_forward_types_generated 的 ObjectDraw
	//（scene.slang struct 经 C API 反射生成 + 逐字段 offsetof static_assert，手写
	// ObjectDrawGpu 已删；布局 @0 totalTri|vertOff|roughnessMetallic / @16 baseColor /
	// @32 emissive，emissive.w = MASK alphaCutoff）
	std::span<const ObjectDraw> objectDraws;
	// Phase 7 D3：多灯数组（scene data 尾区 kSceneDataLightArrayOffset，4B count@base +
	// GpuLight[count]@base+16，契约见 lights.slang / lights_schema.lua）。lightCount=0 时
	// 该区不写。forward 仍走 .light（@0 头单灯），二者独立填。
	uint32_t lightCount = 0;
	std::array<GpuLight, kMaxLights> lightArray{};
	// Phase 11 批 11.2：级联阴影光空间 VP（scene data 尾区 kSceneDataShadowMatrixOffset，
	// 索引 = cascade 号，列主序，shader scene.slang readShadowViewProj 消费）。区容量 =
	// kSceneShadowCascadeCount（一次留够 4 槽，见 scene_data.cppm 该区注释）；5a-1 只写
	// [0]（单级、拟合场景包围球、相机无关 = 无爬行），[1..3] 留单位阵（尚无消费者）。
	std::array<glm::mat4, kSceneShadowCascadeCount> shadowViewProj{};
	// 阴影采样参数（scene data 尾区子区，基址见 scene_data.cppm
	// kSceneDataShadowSplitsOffset；shadow.slang 逐字面量对应）：
	//  · shadowSplits —— 每级远截止平方距离（选级用；当前未切分 = 全小值、全落级 0）
	//  · shadowTexelSizes —— 每级阴影图 texel 的世界尺寸（2·r/mapSize，normal offset 用）
	//  · shadowParams —— (x=depthBias, y=normalOffsetScale, z=总开关, w=保留)
	glm::vec4 shadowSplits{1e9f, 1e9f, 1e9f, 1e9f};
	glm::vec4 shadowTexelSizes{};
	glm::vec4 shadowParams{};
	// · shadowAtlas —— (图集边长, 单块边长, 分块列数, 保留)；shadow.slang 用它把级联块的
	// 光空间 uv 叠到图集上（与图装饰的分块子矩形同源）。
	glm::vec4 shadowAtlas{2048.0f, 1024.0f, 2.0f, 0.0f};
	// · shadowPcss —— 批 11.5 PCSS 参数（半影全宽系数 2·tan(θ/2), blocker 搜索半径[图集纹素],
	//   半影半径上限[图集纹素], 保留）。只有 -DPCSS_SOFT 变体消费（scene.slang readShadowPcss
	//   @722032）；原版 SPV 不读本区 = 门关红线。默认值 = diagnostics 默认（tan(0.53° 全宽)、16、32）
	glm::vec4 shadowPcss{0.00925f, 16.0f, 32.0f, 0.0f};
	// · shadowEvssm —— 批 11.6 EVSSM 参数（半坡 tan(θ/2), 搜索半径[图集纹素 mip0 域],
	//   半影半径上限[图集纹素], 矩链最大 mip）。只有 -DEVSSM_SOFT 变体消费（scene.slang
	//   readShadowEvssm @722048）；原版 SPV 不读本区 = 门关红线。
	glm::vec4 shadowEvssm{0.00465f, 24.0f, 48.0f, 7.0f};
	// · shadowEvssmConst —— 批 11.6 EVSSM 常量 vec4（x = EVSM 指数 c，yzw 留白）。只有
	//   -DEVSSM_SOFT 变体消费（scene.slang readShadowEvssmExponent @722064）；原版 SPV
	//   不读本区 = 门关红线。值 = vk_resources kEvssmExponent（唯一权威源，
	//   生产/消费/fallback 三方同源——禁在此写死字面量）。
	glm::vec4 shadowEvssmConst{kEvssmExponent, 0.0f, 0.0f, 0.0f};
	// ── 批 12.6 TAA（时域抗锯齿）帧间载荷，scene data 尾区 @722080 / @722144 / @722208
	//   （+ @722224，批 13.5 起 = 上一帧内容位移）──
	// 口径（**改动前必读**，全量推导 → scene_data.cppm kSceneDataTaa* 区注释）：
	//   重建世界坐标 = 本帧**抖动后**逆矩阵（与深度缓冲配套）；投回上一帧 = 上一帧
	//   **抖动后**矩阵（产出**抖动网格**坐标），随后在 taa.frag 减 c_{t−1} 落回**基空间**
	//   历史网格。生产者 = ecs taaSystem。
	// 默认值 = **完全旁路**（params.y historyValid = 0 → taa.frag 直接输出本帧）。
	glm::mat4 taaPrevViewProj{1.0f};
	glm::mat4 taaMotion{1.0f};  ///< prevSpin · inverse(curSpin)（spin 关 = 单位阵）
	/// (feedback, historyValid, **本帧内容位移 c_t** x/y px)
	/// ⚠ zw 自批 13.5 起被 taa.frag **真正使用**（输入侧 un-jitter）；此前装 jitterPx
	///   但 shader 从未读它（死载荷）。口径 = `taa_jitter.cppm::contentShiftPx`，非 jitterPx。
	glm::vec4 taaParams{0.0f};
	/// · taaPrevContentShift —— 批 13.5 历史基空间修正：**上一帧内容位移** c_{t−1}
	///   (x, y = 像素, z/w 保留)，scene data 尾区 @722224（复用批 13.2 基空间偏移删除后
	///   空出的槽 ⟹ **零布局变更**）。taa.frag 把抖动网格的 prevUv 减它才落到基空间历史网格。
	///   默认值 = 全 0 ⇒ 减法逐位恒等（TAA 关 / jitter_scale=0 走这条，BIT-IDENTICAL 基线）。
	glm::vec4 taaPrevContentShift{0.0f};
	/// · taaInputKernel —— 批 13.8 输入侧 un-jitter 的**重建核选择**（x = 0 双线性 / 1 CR 4×4 /
	///   2 Lanczos3 6×6，yzw 保留），scene data @722240。默认 0 = 走改前那条 `SampleLevel`
	///   ⟹ **逐位今天**（门红线 = `just shot` sha256）。消费者 = taa.frag `sampleInputColor`。
	glm::vec4 taaInputKernel{0.0f};
	/// · taaClipControl —— 批 13.11 **历史裁剪消融门**（x = clip_mode：0 现行 / 1 关盒 /
	///   2 绕 centroid 扩 1.5× / 3 box9 / 4 软裁剪 / … / 10 臂 8 + α 逐像素 / 11 记忆挂重投影位移 /
	///   **13 臂 11 + α 也吃 gate**（批 13.15b，债-69 修法；⚠ 12′ 预留给 FSR3 加性可信度）
	///   （判据与两条已知缺陷 → `phase13_plan.md` §1.14、§1.21）；y = 软裁剪 λ（mode=4）/
	///   decay（mode=7~11/13），**z = 短窗端点（臂 10 与臂 13 读）**），
	///   scene data @722256。默认 0 = 现行那条硬裁剪 ⟹ **逐位今天**（门红线 = `just shot` sha256）。
	///   为什么要有：`clipToAabb` 在 blend 之前替换历史 ⟹ 一次裁剪 = 一次历史重置，而 1px 高对比
	///   细线的 box5 随抖动相位双峰 ⟹ 静止机位残余 97.8% 住在这条路上（`git:调试历史` 2026-10-06）。
	glm::vec4 taaClipControl{0.0f};
	// ── Phase 15 批 15.1 SSR（屏幕空间反射）帧载荷，scene data 尾区 @722272 ──
	/// 本帧**抖动后** proj·view（与 @64 skyInvViewProj 严格互逆、与深度缓冲配套；口径与
	/// 「为什么不让 shader 自己求逆」→ scene_data.cppm kSceneDataSsrOffset）。生产者 =
	/// fillSsrViewProj（render_list_impl，可测接缝）。交付时无消费者 = 写它不改像素。
	glm::mat4 ssrViewProj{1.0f};
	/// · ssrParams —— SSR 系数子区 @722336（vec4）= (SceneBounds.radius, maxDistanceRadii,
	///   thicknessPx, biasPx)：x = 尺度无关参数锚点（口径 → scene_data.cppm
	///   kSceneDataSsrParamsOffset），yzw = 由它导出的三个无量纲系数（configs/runtime.lua
	///   `ssr` 块，**每帧量**）。生产者 = fillSsrParams（render_list_impl，可测接缝）。
	glm::vec4 ssrParams{0.0f};
	/// · ssrGate —— SSR 粗糙度门控子区 @722352（vec4）= (roughFull, roughCutoff, blurScale, 0)。
	///   与场景尺度无关，故与 ssrParams 分槽（分组理由 → scene_data.cppm 该区注释）。
	///   z = blurScale（批 15.3B，ssr_apply 螺旋模糊半径系数，全分辨率 px 口径）。
	///   生产者同 fillSsrParams（一次填两槽，两槽永远同源同帧）。
	glm::vec4 ssrGate{0.0f};
	// ── Phase 15 批 15.2 子步 A 湿面参数，scene data 尾区 @722368 ──
	/// · wetParams —— 湿面身份调制系数子区 @722368（vec4）= (strength, dark, roughLo,
	///   roughHi)：x = 总开关兼强度（0 = 关 = 消融档，**默认 1.0 = 开**）。湿面**身份**
	///   （哪些面湿）不占参数 = 1 − smoothstep(ssrGate.x, ssrGate.y, roughness)，与 SSR
	///   门控同区间同源（理由 → scene_data.cppm 湿面区注释）。生产者 = fillWetParams
	///   （render_list_impl，可测接缝）。消费者 = deferred_geometry.frag.slang 湿面调制（15.2B）。
	glm::vec4 wetParams{0.0f};
	/// · wetSpatial —— 湿面空间变化参数子区 @722384（vec4）= (rippleAmp, puddleFreqRadii,
	///   0, 0)。与 wetParams 语义分组不同（身份调制系数 vs 空间变化形状参数），故分槽
	///   （分组理由照 ssrParams/ssrGate 那条，→ scene_data.cppm 该区注释）。
	///   生产者同 fillWetParams（一次填两槽，两槽永远同源同帧）。
	glm::vec4 wetSpatial{0.0f};
	// ── 簇化轴 ★1-a1i：per-object 簇区间表，scene data 尾区 @722400（stride 16 × 4096）──
	/// · clusterRanges —— 按 objectSlot 索引的 `ObjectClusterRange` 表（scene data 尾区
	///   kSceneDataClusterRangeOffset）。**生产者不是本 system**：装配层启动期用
	///   `meshlet_ranges::scanClusterRanges` 扫簇产物 groups → registry ctx `ClusterRangeTable`，
	///   renderListSystem 每帧只搬 span（生命周期规则同 objectDraws：录制期读取即完成使命）。
	///   门 `meshlet.enable` 关 = ctx 里根本没有那张表 = 空 span ⟹ 该区保持全 0。
	///   读取点 = meshlet.mesh.slang（`base + (groupID.x >> 1)` = 全局簇下标（★1-a1ii-α″ 去索引发射后每簇固定 2 槽：
/// 半区号 = `groupID.x & 1`，每槽 ≤ 85 三角，3×85 = 255 ≤ maxMeshOutputVertices）），
	///   写它不改任何像素（门关逐位不变的构造性保证 = 方案 B 追加尾区，见 scene_data 该区注释）。
	std::span<const ObjectClusterRange> clusterRanges;
};

/**
 * 每帧 GPU culling 输入（Phase 7b 批次 5；render system 产出 → drawFrame 经 push
 * constant 传给 culling compute）：planes 与 CPU 剔除路径同源同值（extractFrustum
 * 产物）——GPU/CPU 对账基准一致的保证。布局（cull.comp.slang CullPush 同字节，
 * scalar layout 无尾 pad = 100B ≤ maxPushDataSize 256B）：
 *   @0 planes[6] float4 | @96 sequenceCount u32（mvp 不经 push——CPU 算进序列）
 */
export struct CullFrameInput {
	glm::vec4 planes[6]{};
	uint32_t sequenceCount = 0;
};
static_assert(sizeof(CullFrameInput) == 100, "CullFrameInput layout drifted from cull.comp.slang CullPush");

/**
 * 每帧绘制列表：顺序 = 绘制顺序（Phase 5 透明排序已落地：不透明段在前、透明段按
 * 相机距离 back-to-front 排；Phase 7 culling 输出的落点）。
 * Phase 5.5 批次 4 三段：opaque = [0, skyboxStart)，skybox = [skyboxStart,
 * transparentStart)，transparent = [transparentStart, size)——两个边界字段与段序
 * **与 Phase 6 RenderGraph pass 序列同构**（机械翻译：每段 = 一个 Pass，边界 = 依赖边）。
 * drawFrame 段首各 applyDynamicState 一次（不透明写深度关混合 / 天空盒 LEQUAL 不写深度 /
 * 透明关深度写开混合），段内逐 draw 循环不变（盲执行契约保持）。
 */
export struct FrameDrawList {
	std::vector<DrawCall> calls;
	uint32_t skyboxStart = 0;       // 无天空盒 = 与 transparentStart 重合（空段跳过）
	uint32_t transparentStart = 0;
	// Phase 11 批 11.2：阴影段（**独立向量，不属于 calls**）。为什么不拼进 calls：
	// calls 的段边界（skyboxStart / transparentStart）被 GPU culling 的候选序对账与
	// cull_driver 重放路强依赖（采样候选表 = CPU opaque 段序），插一个新段就要重新对账；
	// 而阴影段的可见集与主视野不同（下方“源几何”备注），属于独立通道。
	// drawSource = "shadow" + 分块号字符串（见 scene_context::shadowDrawSource）。
	std::vector<DrawCall> shadowCalls;
	// 簇化轴 ★1-a1ii-α：簇路径**影子叠加**段（**独立向量，不属于 calls**，shadowCalls 同法）。
	// 为什么不拼进 calls：opaque 段序被 GPU culling 候选序对账（collectCpuObjectSlots）与
	// cull_driver 重放路强依赖，插叠加 draw 会在非默认 cull 档打破对账；独立向量 = 对账
	// 零感知。执行点 = executePassDraws 通用分支在 opaque 段之后盲执行（同 pass 同状态）；
	// `cull_replay=gpu_sync` 走 cullDriver 分支 = 叠加静默跳过（本批不接 DGC，★1-b 再议）。
	// 门 `meshlet.enable` 关 = 资产无 meshlet 条目 = 本向量恒空（逐位不变的构造性保证）。
	std::vector<DrawCall> meshletCalls;
	// 阴影段的**分块边界**（kSceneShadowCascadeCount + 1 个）：boundaries[0] = 0，boundaries[c+1] = 级 c 的段尾。
	// 为何不用 4 个独立向量：这 4 段在**同一张图集**上、为 4 个 pass 交替渲染，
	// 序号连续才是自然的；拼一个向量 + 边界数组与 calls/transparentStart 的现有风格一致。
	// 长度固定为 kSceneShadowCascadeCount + 1，未启用的级区间为空（段边界相等 → 该 pass 零 draw）。
	std::array<uint32_t, kSceneShadowCascadeCount + 1> shadowBoundaries{};

	/** 阴影第 c 级的 draw 序列（c 超出已启用级数时返回空 span）。
	 *  @param c 分块号（受控于 boundaries 长度 = 区容量）
	 *  @return 该级的 draw 序列 */
	[[nodiscard]] std::span<const DrawCall> shadowSegment(uint32_t c) const {
		if (c + 1u >= shadowBoundaries.size() || shadowBoundaries[c] >= shadowBoundaries[c + 1])
			return {};
		return {shadowCalls.data() + shadowBoundaries[c], shadowBoundaries[c + 1] - shadowBoundaries[c]};
	}
	// Phase 10 批 10.7：点选高亮通道（**纯 CPU 侧**）——高亮 pass 录制期据此过滤 draw：
	// 只画 materialBySlot[objectSlot] == selectedMaterial 的 draw，即「选中材质全部实体
	// 一起发光」。hasSelection=false → 该 pass 零 draw 空转。
	// 走 FrameDrawList 而非让执行器问 ECS：执行器保持零 entt 依赖（ECS 规范 4 = 渲染
	// system 产出抽象 draw list、执行器只盲执行）。span 与 materialBySlot 同帧局部存储。
	std::span<const uint32_t> materialBySlot;  // 按 objectSlot 索引的材质 id
	uint32_t selectedMaterial = 0;             // 选中材质 id（hasSelection=false 时无意义）
	bool hasSelection = false;                 // 本帧有无选中（Esc 取消 / 首帧 = false）
	// 批 14.5：FSR upscaler 的相机输入（**不进 scene data 缓冲**，纯 CPU 侧每帧量，
	// 执行器直接读）。走 FrameDrawList 而不是让执行器问 ECS = 规范 4（执行器零 entt）。
	// near/far 必须是**投影实际生效值**：projectionSystem 做自适应 near/far 并把结果写回
	// ViewProjection（ecs_impl.cppm:349-363），取 Camera 原值会按错的平面线性化深度。
	// fov 由 proj 矩阵反推（= 2·atan(1/proj[1][1])，glm::perspective 恒等式）——比读配置
	// 字段更硬：读的就是本帧真正用了的那个投影。单位 = 弧度。
	float fsrNear = 0.05f;
	float fsrFar = 1000.0f;
	float fsrFovVertical = 1.0f;
	/**
	 * 批 14.5 第四批：本帧**进投影矩阵那份抖动**（渲染像素单位，= `ViewProjection::jitterPx`
	 * 原值）。口径 = `docs/FSR输入契约.md` §3。
	 *
	 * ⚠ **本字段不是申报给 FSR 的值**——申报要用它经 `contentShiftPx` 换算后的**内容位移**
	 * （两者 **Y 轴反号**）。本注释此前写的是「传原始 Halton 值，不是内容位移 c_t」，
	 * **正好反了**，照它实现就把 Y 符号报错了一整个 Phase（地面真值实测：内容位移残差
	 * 0.029 vs 原始值残差 0.543）。换算点收在唯一消费者 `vkrenderer_offgraph.cppm`，
	 * 落地字段名已是 `FsrFrameInput::contentShiftX/Y`。
	 *
	 * ⚠ 这条是**成对约束**的一半：FSR 的 `jitterOffset` 必须恰好等于「本帧色输入实际带的
	 * 内容位移」。报多了报少了都不报错——vendor 零校验，症状是重建窗口偏移（糊或鬼影）。
	 * 另一半见 `fsr_upscaler_impl.cppm` 的 cancellation 标志。
	 */
	glm::vec2 fsrJitterPx{0.0f, 0.0f};
	SceneFrameData sceneData;
	// Phase 7b：GPU culling 每帧输入（render_list 与 CPU 剔除同源填充；GpuCull 未激活
	// 时 Renderer 忽略——始终填充，零条件分支）
	CullFrameInput cullInput;
	// Phase 7b：GPU culling 候选 args（**逐 candidate、candidate 序**——与 GpuCull 表同序
	// 同守卫，render_list 与 main C3 双处契约）。mvp 在剔除测试**前**算（被剔候选也要有
	// 值——序列 CPU 侧全量装配）；重放/DGC 消费 = 与 CPU drawOne 位级同源的 DrawArgs。
	std::vector<RGL::DrawArgs> cullArgs;
};

// 执行器可识别的 drawSource 段名 = pass_vocabulary::isKnownDrawSource（**单一真相源**
// = kAllDrawSources：三个精确段名 + 可变分块号的 "shadow" 前缀，分块号可变
// kActiveShadowCascades 1→4，写死字面量会在切级联时漏校验）。本模块只负责
// **段名 → draw 序列**的映射（下方 segmentSpan）；"合法吗"归词表，图校验里判。
// ⚠️ 本模块的 if 链与那张表不共享机制（表判合法、链做映射）——漂移由
// tests/test_render_graph.cppm 的「segmentSpan 覆盖 kAllDrawSources」用例拦。

/** 从阴影段名里解析分块号（"shadow" → 0，"shadow2" → 2；非 shadow 前缀 → 0，调用点保证只在
 *  阴影分支上调用）。不用 from_chars（rlstd 不导出它），手扫后缀数字。 */
uint32_t shadowCascadeOf(std::string_view drawSource) {
	uint32_t v = 0;
	for (char ch : drawSource) {
		if (ch < '0' || ch > '9')
			continue;
		v = v * 10u + static_cast<uint32_t>(ch - '0');
	}
	return v;
}

/**
 * FrameDrawList 段名 → draw 序列（段名 = 机械映射，返回 span 而非下标区间）。
 *
 * **为何改 span**：阴影段存在独立向量 FrameDrawList::shadowCalls 里（见该字段注释：不拼进
 * calls 是为了不打破 GPU culling 的候选序对账契约），二者不共用索引空间 → 一个统一的
 * span 返回值比“返回下标对、由调用点自己取对应向量”不容易错。
 * @param drawSource 段名（"opaque" / "skybox" / "transparent" / "shadow0..3"）
 * @param list 每帧绘制列表
 * @return 该段的 draw 序列（空段 = 空 span，无特判）
 */
export std::span<const DrawCall> segmentSpan(std::string_view drawSource,
	const FrameDrawList& list) {
	if (drawSource == "opaque")
		return {list.calls.data(), list.skyboxStart};
	if (drawSource == "skybox")
		return {list.calls.data() + list.skyboxStart, list.transparentStart - list.skyboxStart};
	// 阴影段：独立向量 + 名字串前缀匹配，**后缀是分块号**（"shadow" = 0）。
	// 先判前缀再退到 transparent 兼容分支：退容分支返回【transparentStart, size)。
	// 漏掉前缀判断等于让 shadow pass 把透明段再画一遍进阴影图（静默错渲染）。
	if (drawSource.starts_with("shadow"))
		return list.shadowSegment(shadowCascadeOf(drawSource));
	return {list.calls.data() + list.transparentStart, list.calls.size() - list.transparentStart};
}

}  // namespace RGL
