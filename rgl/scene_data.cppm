module;
#include <vk_mem_alloc.h>  // VmaAllocationCreateInfo（VmaRaii 不导出 C 层，使用方自含，gpu_cull_impl 同款）
#include <glm/glm.hpp>  // sizeof(glm::vec2/mat4) 参与布局常量派生（GMF 三方头白名单内）
export module scene_data;

import rlstd;
import rlvk;
import lights_types_generated;  // GpuLight struct + kMaxLights/kGpuLightStride/kSceneDataLightArrayBase
import scene_forward_types_generated;  // 前向头 SceneLight（C API 反射生成）
import VmaRaii;
import vk_device;
import vk_resources;  // 架构债务续账（2026-09-19）：服务向内核领 buffer/slot——单向依赖，内核不认识本模块

namespace RGL {

// SceneData：场景级每帧动态数据的 bindless 契约（Phase 5 批次 2；2026-09-19 架构债务
// 步骤 1 由 vk_resources 拆出——布局常量性质同 lights_types_generated，与"资源管理"无关）。
// 单一 storage buffer（heap slot 经 Resources::allocSlot Runtime 运行时段分配——纹理槽数随场景动态，
// light 槽无法钉编译期偏移，同 slotTexture 模式），字节布局的单一真相源（shader 端
// scene.slang 字面量必须一致，kTrianglesPerGroup 先例——无反射端对应）：
//   @0   SceneLight（48B：direction/eyePos/color/ambient 各 vec3，C API 反射生成，
//        见 scene_forward_types_generated / docs/archive/slang_api_reflection_research.md）
//   @48  Phase 5.5 env 参数（原 16B 保留区启用）：envIntensity float + envMipRange
//        float（= mipLevels−1，0 = 无环境）+ 8B 留白——scene.slang/env.slang 字面量一致
//   @64  Phase 5.5 批次 4 skyboxInvViewProj（mat4 列主序 = inverse(proj·view)，
//        sky.slang skyClipToWorld 消费）
//   @128 对象矩阵表：mat4 列主序 × kSceneMatrixCapacity（[objectSlot] = spin·world，
//        实例级 Phase 6 instancing；DrawArgs.objectIndex = MeshRef.objectSlot 索引；
//        世界空间光照 + Phase 7 shadow/culling 复用）
// 内容每帧变 = 全项目首个动态 GPU 数据：CPU 写 staging ring（×MAX_FRAMES_IN_FLIGHT，
// drawFrame ① 已 wait 本 slot fence → 写安全）+ 录制期 vkCmdCopyBuffer 拷进本 buffer
//（命令流内天然有序；描述符静态指向本 buffer 永不改动，见 vkrenderer.cppm）。

// 前向头 SceneLight（scene data @0-48）= C API 反射生成（scene_forward_types_generated，
// 布局真相源 = scene.slang SceneLight，逐字段 offsetof static_assert 钉死）。原手写
// LightDataGpu 已删（双源消除）——默认成员初始化为死代码（renderListSystem 每帧显式
// 聚合初始化 .light，从不用默认值），故无默认不影响行为。
// 注意：SceneLight 无默认成员初始化；消费方须显式赋值（render_list 已保证）。

/** Phase 5.5：env 参数偏移（envIntensity float @48 + envMipRange float @52；env.slang
 * readEnvParams 的 Load(48)/Load(52) 字面量必须与本值一致——scene.slang 先例）。派生自
 * sizeof(SceneLight)（C API 反射生成），shader 布局漂移即编译失败。 */
export inline constexpr uint32_t kSceneDataEnvOffset = sizeof(SceneLight);
/** tonemap 曝光（scene data 头 @56，env 16B 之后的保留区；tonemap.frag Load(56) 直读，
 *  ACES 前线性倍率，1.0 = 直通）。 */
export inline constexpr uint32_t kSceneDataExposureOffset = 56;
/** Phase 11 批 11.4：SSAO 施加强度（scene data 头 @60，头部保留区**最后一格**，不新增区、
 *  不动任何既有偏移——@64 才是 invViewProj）。tonemap.frag Load(60) 直读，
 * `lerp(1, ao, strength)` 的 strength：0 = 逐位旁路（默认，BIT-IDENTICAL 基线）。 */
export inline constexpr uint32_t kSceneDataAoOffset = 60;
/** Phase 5.5 批次 4：skyboxInvViewProj mat4 列主序 @64（inverse(proj·view)，sky.slang
 * skyClipToWorld 的 Load4(64/80/96/112) 字面量必须与本值一致）。 */
export inline constexpr uint32_t kSceneDataSkyOffset = 64;
export inline constexpr uint32_t kSceneDataHeaderBytes = 128;   // light 48 + env 16（含曝光 4B@56）+ sky invVP 64
static_assert(kSceneDataEnvOffset == 48 && kSceneDataSkyOffset == 64 &&
	kSceneDataExposureOffset == kSceneDataEnvOffset + sizeof(glm::vec2) &&
	kSceneDataAoOffset == kSceneDataEnvOffset + 3 * sizeof(float) &&
	kSceneDataAoOffset + sizeof(float) == kSceneDataSkyOffset &&
	kSceneDataSkyOffset + sizeof(glm::mat4) == kSceneDataHeaderBytes);
/** mat4 列主序 stride：派生自 sizeof(glm::mat4) 而非裸字面量（C++ 侧漂移即编译失败；
 * shader 端 scene.slang 的字面量 64u 须与本值一致——无反射端对应，见 scene.slang 头注释）。 */
export inline constexpr uint32_t kSceneMatrixStride = sizeof(glm::mat4);
export inline constexpr uint32_t kSceneMatrixCapacity = 4096;   // 对象矩阵表容量（Phase 7：Bistro 1591 primitive 各一实例，1024 不够；scene data 缓冲随之 256KB）

/** Phase 7 D3：多灯数组区（追加于对象矩阵表之后——@0-128 头逐字节不动，forward/env/sky
 * shader 零改动 diff0）。基址 = 头(128) + 矩阵表容量×stride（= 128 + 4096×64 = 262272，
 * lights.slang kSceneDataLightArrayBase 同值）；static_assert 钉死与生成常量
 * kSceneDataLightArrayBase（lights_schema.lua）三方同步，任一侧漂移编译失败。 */
export inline constexpr uint32_t kSceneDataLightArrayOffset =
	kSceneDataHeaderBytes + kSceneMatrixCapacity * kSceneMatrixStride;
static_assert(kSceneDataLightArrayOffset == kSceneDataLightArrayBase,
	"scene data light-array offset drifted from lights_schema.lua kSceneDataLightArrayBase");
/** 灯数组区字节数 = 16B lightCount 头 + kMaxLights × kGpuLightStride（= 16 + 8×64 = 528；
 * 区布局见 lights.slang readLightCount/readGpuLight）。 */
export inline constexpr uint32_t kSceneDataLightArrayBytes = 16u + kMaxLights * kGpuLightStride;

/** Phase 7b+（NV DGC 规避）：per-object MVP 表区（追加于灯数组之后——既有各区基址
 * 逐字节不动 diff0）。CPU 每帧写（值 = 原 DrawArgs.mvp，位级一致），shader
 * scene.slang readObjectMvp 按 objectIndex 读——DrawArgs 去 mvp 字段后 push 36B（当时值，
 * 后 bindless 化终态 4B，见 kSceneDataDrawTableOffset），
 * 绕 NV DGC PUSH token >83B 多序列挂死（见 docs/调试历史）。
 * 基址 = 灯数组尾（262272 + 528 = 262800，scene.slang 字面量同值）。 */
export inline constexpr uint32_t kSceneDataMvpTableOffset =
	kSceneDataLightArrayOffset + kSceneDataLightArrayBytes;
static_assert(kSceneDataMvpTableOffset % 16 == 0, "MVP table must stay 16B aligned");

/** Phase 7b+（NV DGC 规避，续 MVP 表）：per-object 材质/几何表区（DrawArgs 去 mvp 后
 * 仍剩 baseColor/roughnessMetallic/totalTriangles/vertexOffset 是 per-object 数据，一并
 * 进 heap，DrawArgs 最终只留 objectIndex（4B push）——彻底绕开 NV DGC push 挂死）。
 * 基址 = MVP 表尾（262800 + 4096×64 = 524944，scene.slang 字面量同值，下方 static_assert
 * 钉死）。stride 48（`ObjectDraw`，scene_forward_types_generated 生成；Phase 8 ③ 加
 * emissive 后 32→48）。 */
export inline constexpr uint32_t kSceneDataDrawTableOffset =
	kSceneDataMvpTableOffset + kSceneMatrixCapacity * kSceneMatrixStride;
export inline constexpr uint32_t kSceneDrawStride = 48;
static_assert(kSceneDataDrawTableOffset % 16 == 0, "draw table must stay 16B aligned");
// Phase 10 批 10.2a：钉死 scene.slang readObjectDraw 的字面量基址/stride——容量常量若变
// 而 shader 字面量未同步 = 此处编译报错（shader 侧无编译期反射通道，kTrianglesPerGroup 同纪律）
static_assert(kSceneDataDrawTableOffset == 524944, "scene.slang readObjectDraw literal 524944 out of sync");
static_assert(kSceneDrawStride == sizeof(ObjectDraw),
	"scene.slang ObjectDraw (reflection-generated) stride out of sync with table stride");

/** Phase 8 批次 ④a：漫反射辐照度 SH9 区（追加于材质表之后——既有各区基址逐字节不动 diff0）。
 * 9 × vec4 = 144B（.xyz=系数、.w 留白，满足 ByteAddressBuffer.Load4 16B 对齐前提；CPU env
 * 加载期从等距柱状源余弦加权投影 radiance，shader scene.slang irradiance(N) 做辐照度卷积）。
 * 基址 = 材质表尾（524944 + 4096×48 = 721552，scene.slang 字面量同值；Phase 8 ③ stride
 * 32→48 后本区随之后移）。 */
export inline constexpr uint32_t kSceneDataIrradianceOffset =
	kSceneDataDrawTableOffset + kSceneMatrixCapacity * kSceneDrawStride;
export inline constexpr uint32_t kSceneDataIrradianceBytes = 144;  // 9×vec4=144（16B 对齐）
static_assert(kSceneDataIrradianceOffset % 16 == 0, "irradiance region must stay 16B aligned");

/**
 * Phase 11 批 11.2：级联阴影**光空间 VP 矩阵区**（追加于辐照度区之后——既有各区基址
 * 逐字节不动，沿用 Phase 8 ④a 的追加法）。区内容 = `kSceneShadowCascadeCount` 个 mat4
 * 列主序，索引 = cascade 号，相对基址 + 64×i。
 *
 * **为什么一次就预留 4 槽**：cascade 是"同一张阴影图上的 4 个视口分块"，每块一张 VP。
 * 先写 1 槽、后续再加 3 槽 = 再动一次尾部基址 + 改两侧手写字面量，而这类"加区域"的
 * 最省事形态就是**一次留够**。本次（5a-1）只写 [0]（单级，拟合场景包围球），
 * [1..3] 留单位阵——尚无消费者。
 */
/**
 * 阴影图边长（**单一真相源**，Phase 11 批 11.2）。两个消费方：① 视锥包围球拟合的
 * texel 吸附（scene_context::buildShadowVolume）；② 图声明里 depthOffscreen
 * 附件的独立 extent（render_graph::decorateShadowPass —— 常量由调用方 main_impl 传入，
 * 图模块本身不认识 scene_data 契约）。
 *
 * **1024 的来由**：正交半边长 = 视锥包围球半径 r → texel 世界尺寸 = 2r/1024。实测
 * bistro 的 r 随机位在 ~10（近景微缩模型）到 ~116（默认俯视全场景）之间浮动，故
 * texel 在 0.02~0.23 之间；近景够用、远景偏糊（级联切分正是为此，见 phase11_plan §10）。
 * 2048 是一行改值的旋钮：内存 4→16 MB，GPU 成本只随**被光栅化的几何量**增长，
 * 不随贴图尺寸增长。
 */
/**
 * 阴影偏置默认值（Phase 11 批 11.2，look-dev 目标值，可通过后续 RPC 调参覆盖）。
 *
 * - `kShadowDepthBias`：**NDC 深度域**的常数偏置。为何不用光深 bias 推导出来的世界尺寸：
 *   正交投影的深度是**线性**的（无透视组归一化敏感度），NDC 偏置 × (深度范围/世界尺寸)
 *   = 常数。直接给 NDC 值使 bias 在**距离变化时自动缩放**（远处阴影图深度精度低、
 *   近处高）——这正是我们要的行为，不需要再乘世界尺寸。
 * - `kShadowNormalOffsetScale`：法线外推的 texel 数。acne 的**主手段**（bias 只是补丁）。1.5 是常见甜点：
 *   小于 1 接近不足一个纹素 → 自遮挡仍发生（黑斑）；大于 2.5 接触阴影明显回撤。
 *   剩余常数的含义见 shadow.slang 采样函数注释。
 */
export inline constexpr float kShadowDepthBias = 0.0015f;        // NDC 深度域
export inline constexpr float kShadowNormalOffsetScale = 1.5f;   // texel 数

/**
 * 当前**生效**的级联数（与 scene data 区容量 kSceneShadowCascadeCount = 4 **分离**）。
 *
 * 为何分离：区容量是一次性预留（避免加级联时再动尾部基址），而本值是“本帧确实画出与采样
 * 哪些级”。切换 = 改这一行（图装饰插 N 个 pass + CPU 逐级剔除 + 采样端选级）。
 *
 * ## 实测定案（非计划；2026-09-29 5a-5 修复后重测）
 * 本机 GPU 侧余量约 **3.1 ms/帧**（Phase 9 判定）。bistro 阴影各级开销（GPU 时间戳，1280×720）：
 *
 * | 生效级数 | 各级 GPU 时间 | 合计 | 占 3.1 ms 余量 |
 * |---|---|---|---|
 * | 1 | 2.21 | **2.21 ms** | 71% |
 * | 2 | 2.11 + 1.91 | **4.02 ms** | 130%（超预算） |
 * | **4** | 1.10 + 2.17 + 1.91 + 1.91 | **7.10 ms** | **229%** |
 *
 * 逐级剔除：级 0 收益明显（近切片球小 → 1.10 ms），级 1-3 各 ≈ 单级全量——默认机位是**俯视
 * 微缩街景**，整个模型在视线方向上基本同一个深度带，远切片包围球又大 → 大多数对象落在每一级里。
 *
 * **定 4 级 = 画质优先的显式取舍**（2026-09-29 用户拍板：1 级 2048² 单块下投影分辨率不够，
 * 4 级近块 texel 尺寸减半）。代价 = gpu.frame 5.05 → 10.05 ms（帧率仍高于 vsync，实测无掉帧
 * 感知则接受）。换回 1 级 = 改这一行；机制（图集 + 逐级剔除 + 采样端选级）全部常驻。
 *
 * 定义放在本模块是为了让 render_list / render_list_impl / render_graph_rtslot 等不引入
 * scene_context（后者拉 entt，让纯资源模块依赖它不合适）。
 */
export inline constexpr uint32_t kActiveShadowCascades = 4;

export inline constexpr uint32_t kSceneShadowCascadeCount = 4;
export inline constexpr uint32_t kSceneDataShadowMatrixOffset =
	kSceneDataIrradianceOffset + kSceneDataIrradianceBytes;
export inline constexpr uint32_t kSceneDataShadowMatrixBytes =
	kSceneShadowCascadeCount * static_cast<uint32_t>(sizeof(glm::mat4));
static_assert(kSceneDataShadowMatrixOffset % 16 == 0, "shadow matrix region must stay 16B aligned");

/** 阴影采样参数子区（跟在 VP 区之后，各 16B）。三个 vec4 与 scene.slang 逐字面量同源：
 *  - 分界远距（x4）：每级级联的远截止平方距离，光照 shader 按片元刷实际距离选级；
 *  - texel 世界尺寸（x4）：2·r/mapSize，normal offset 与偏置的世界尺度换算必需；
 *  - 参数：(x = NDC 深度偏置、y = normal offset 系数（texel 单位）、z = 总开关、w 保留)。
 * 为何单独一个子区而不拼进 VP 区：VP 是每槽 64B 矩阵，拼会破坏“区容量 = 固定槽数×stride”的简单不变式，
 * 而参数是一段量级元数据（改 bias 不需动矩阵区）。 */
export inline constexpr uint32_t kSceneDataShadowSplitsOffset = kSceneDataShadowMatrixOffset
	+ kSceneDataShadowMatrixBytes;
export inline constexpr uint32_t kSceneDataShadowTexelOffset = kSceneDataShadowSplitsOffset + 16u;
export inline constexpr uint32_t kSceneDataShadowParamsOffset = kSceneDataShadowSplitsOffset + 32u;
export inline constexpr uint32_t kSceneDataShadowAtlasOffset = kSceneDataShadowSplitsOffset + 48u;
export inline constexpr uint32_t kSceneDataShadowAuxBytes = 64u;  // 分界远距/texel/参数/图集各 16

/** IZB 消费区（批 11.3-1e，shadow aux 之后 16B）：(x = 消费开关 1/0、y = 像素 W、
 *  z = 像素 H、w 保留)。Renderer 在 writeSceneStaging 尾部按 izb_shadow 门写；
 *  light.frag 字面量消费（static_assert 钉死同步）。flag=0 时因子恒 1 = 逐位旁路。 */
export inline constexpr uint32_t kSceneDataIzbOffset = kSceneDataShadowAtlasOffset + 16u;
export inline constexpr uint32_t kSceneDataIzbBytes = 16u;
static_assert(kSceneDataIzbOffset == 722016,
	"light.frag izb 字面量 722016 与 scene_data.cppm 失去同步");

/** PCSS 参数区（批 11.5，IZB 区之后 16B）：(x = 半影全宽系数 2·tan(θ/2)（θ = 太阳视直径，
 *  半影全宽 = Δz·x）、y = blocker 搜索半径（图集纹素）、z = 半影半径上限（图集纹素）、w 保留)。
 *  fillShadowData 按 diagnostics 填；**只有 -DPCSS_SOFT 变体读**（原版 SPV 不引用本区 =
 *  门关红线构造性保证，scene.slang readShadowPcss 字面量 static_assert 钉死）。 */
export inline constexpr uint32_t kSceneDataPcssOffset = kSceneDataIzbOffset + kSceneDataIzbBytes;
export inline constexpr uint32_t kSceneDataPcssBytes = 16u;
static_assert(kSceneDataPcssOffset == 722032,
	"scene.slang readShadowPcss literal 722032 与 scene_data.cppm 失去同步");

/** 批 11.6 EVSSM 块（2 × vec4，32B）：
 *  @+0  参数 vec4（半坡 tan(θ/2), 搜索半径[图集纹素], 半影上限[图集纹素], 矩链最大 mip）
 *  @+16 常量 vec4（x = EVSM 指数 c = vk_resources kEvssmExponent（唯一权威源），yzw 留白）
 *  ——生产端（EvssmShadow push）/消费端（本块）/fallback 三方同源，shader 侧禁写字面量。
 *  只有 -DEVSSM_SOFT 变体消费（shadow.slang readShadowEvssm/Const 字面量
 *  static_assert 同步，同 11.5 门关红线构造性保证）。 */
export inline constexpr uint32_t kSceneDataEvssmOffset = kSceneDataPcssOffset + kSceneDataPcssBytes;
export inline constexpr uint32_t kSceneDataEvssmBytes = 32u;
/** 常量 vec4 偏移（x = c；scene.slang readShadowEvssmConst 字面量同步点）。 */
export inline constexpr uint32_t kSceneDataEvssmConstOffset = kSceneDataEvssmOffset + 16u;
static_assert(kSceneDataEvssmOffset == 722048,
	"scene.slang readShadowEvssm literal 722048 与 scene_data.cppm 失去同步");
static_assert(kSceneDataEvssmConstOffset == 722064,
	"scene.slang readShadowEvssmConst literal 722064 与 scene_data.cppm 失去同步");
static_assert(kSceneDataShadowSplitsOffset == 721952 && kSceneDataShadowParamsOffset == 721984
	&& kSceneDataShadowAtlasOffset == 722000,
	"scene.slang shadow sampling literals drifted from scene_data.cppm offsets");
// 与 scene.slang 的字面量基址钉死（两侧手写同步点，改一侧必编译失败）
static_assert(kSceneDataShadowMatrixOffset == 721696,
	"scene.slang readShadowViewProj literal 721696 out of sync with kSceneDataShadowMatrixOffset");

/** 批 12.6 TAA 区（EVSSM 常量 vec4 之后 **176B**（原 160B，批 13.8 追加一段 16B），
 *  五段子区；第四段 16B 自批 13.5 改义）：
 *
 *  🔵 **批 13.5 的架构变更（读整段前先看这条）**：历史附件 `taa_hist` 自批 13.5 起住在
 *  **基空间**（无抖动网格），不再住抖动空间。做法 = taa.frag 把**本帧**输入按 c_t 做
 *  un-jitter 重建后再进时域混合 ⟹ 混合结果天然基空间 ⟹ 历史不再每帧被亚像素重采样
 *  （静止机位下重采样偏移恒 0）。这修掉「固定频率 + 固定位置」的 16 帧周期边缘 shimmer。
 *  全案 → `docs/taa_history_space_plan.md`。**本区四段载荷的字节布局与偏移全部未变。**
 *
 *  - `kSceneDataTaaOffset`（mat4, 64B）= 上一帧**抖动后**的 proj·view。重投影公式：
 *      world = (sceneData@64 skyInvViewProj) · clip(uv, depth)   ← 本帧抖动后的逆矩阵
 *      prevClip = prevViewProj · (motionPrevFromCur · world)     ← 上一帧抖动后的矩阵
 *    两侧各用**自己那一帧的抖动后**矩阵（不是「口径不统一」，而是「两边都是渲染出各自
 *    那张图的那个矩阵」）：深度缓冲由抖动后矩阵产出 ⇒ 重建必须配套。
 *    ⚠ 这里曾长期存「未抖」那份（理由写作「历史图是像素中心对齐的」）——把「像素中心
 *    对齐」误读成「在未抖网格里」。后果 = 采样点系统性偏 -jitter_prev（±0.5px 且每帧
 *    随机变），静止机位实测 S1/S2 = 0.48×（开滤波逐帧差反而**翻倍**）。批 12.6 T4。
 *    🔵 **批 13.5：本裁决仍然成立、未被反转**，但**理由变了，且多了一条成对约束**：
 *      · 本字段产出的是**抖动网格**上的坐标 = `β_{t−1}(w) + c_{t−1}`（β = 基网格位置）。
 *      · 历史现在住基空间 ⟹ taa.frag 拿到后要**减 `kSceneDataTaaPrevContentShiftOffset`**
 *        才落到 `β_{t−1}(w)`。
 *      · ⟹ **两处成对、二选一**：要么「本字段抖动后 + shader 减 c_{t−1}」（现行），要么
 *        「本字段未抖 + shader 不减」（Filament 形态）。**都做 = 多减一次 c_{t−1}，
 *        都不做 = 少减一次 c_{t−1}**，两种都让静止机位 0.667px 游动原样回来。
 *        改一侧必须同时改另一侧。
 *  - `kSceneDataTaaMotionOffset`（mat4, 64B）= `prevSpin · inverse(curSpin)`。本项目的
 *    运动源是 SceneSpin「全场景同一旋转矩阵」，所以一个 mat4 就**精确**补偿物体运动
 *    （Filament 纯深度重投影假设世界静止，动场景必拖影；RGL 这里没有这个 gap）。
 *    spin 关 = 单位阵。
 *  - `kSceneDataTaaParamsOffset`（vec4, 16B）= (x = feedback 本帧权重, y = historyValid 1/0,
 *    z/w = **本帧内容位移 c_t** 像素)。y=0 时 shader 直接输出本帧 = **逐位旁路**
 *    （首帧 / resize / 相机跳切走这条，同 ao.strength=0 的旁路纪律）。
 *    🔵 批 13.5：zw 此前装 `jitterPx` 但 **shader 从未读**（死载荷）；现改装 c_t 并**真读**
 *    （输入侧 un-jitter）。⚠ 两者差一个 y 轴翻号（`c = (+jx, −jy)`，见
 *    `taa_jitter.cppm::contentShiftPx`），照旧口径读会错一整轴。
 *  - `kSceneDataTaaPrevContentShiftOffset`（vec4, 16B）= **上一帧内容位移 c_{t−1}**（批 13.5）
 *    + **AO 已施加标志**（z，批 13.6）。
 *  - `kSceneDataTaaInputKernelOffset`（vec4, 16B，批 13.8 新增）= x = **输入侧 un-jitter 的
 *    重建核选择**（0 双线性 = 默认 / 1 CR 4×4 / 2 Lanczos3 6×6），yzw 保留。
 *  - `kSceneDataTaaClipControlOffset`（vec4, 16B，批 13.11 新增）= (x = **历史裁剪消融门**,
 *    y = 软裁剪 λ)，zw 保留；0 = 逐位今天。
 * 消费端 = taa.frag.slang / tonemap.frag.slang 字面量（两侧 static_assert 钉死同步）。 */
export inline constexpr uint32_t kSceneDataTaaOffset = kSceneDataEvssmConstOffset + 16u;
export inline constexpr uint32_t kSceneDataTaaMotionOffset = kSceneDataTaaOffset + 64u;
export inline constexpr uint32_t kSceneDataTaaParamsOffset = kSceneDataTaaOffset + 128u;
/**
 * 批 13.5 历史基空间修正：**上一帧内容位移** c_{t−1}（vec4 = x/y = 像素，**z = 批 13.6 AO
 * 施加点标志**，w 保留）。
 * 偏移 @722224 = 复用批 13.2「基空间输出」删除后空出的槽 ⟹ **零布局变更**（static_assert 不变）。
 *
 * 为什么需要它（一句话）：批 13.2 只把**显示**搬回基空间（在 tonemap 加 `+c_t` 采样偏移），
 * 而**累积**那一半没动——历史图仍住抖动空间 ⟹ 静止机位下每帧按 `δ_t = c_t − c_{t−1}`
 * （闭式均值 0.667px）亚像素重采样历史 ⟹ 输出对 16 帧抖动相位是周期函数、且只在边缘响应
 * = 用户报障的「固定频率 + 固定位置」shimmer；老贡献被反复重采样 = 比理想参考还糊 1.5 倍。
 * 批 13.5 把 un-jitter 从 tonemap **前移到 taa 的输入侧** ⟹ 混合结果天然基空间 ⟹ 历史
 * 不再被亚像素重采样（静止机位偏移恒 0）⟹ 两个症状同源一起消。
 * tonemap 侧的 `+c_t` 偏移随之删除（留着 = 双重 un-jitter，比改前更糟）。全案 →
 * `docs/taa_history_space_plan.md`。
 *
 * 为什么偏移仍由 CPU 算好、shader 只管减：与批 13.2 同理由（把口径判定留在 CPU 一处）。
 * 本字段**无门标志位**——它不是门，是数据：TAA 关 / `jitter_scale=0` ⇒ `jitterPx = 0`
 * ⇒ 本字段全 0 ⇒ shader 的 `prevUv − 0` 逐位 = `prevUv`（IEEE：`x − 0 = x`，折成 FMA 亦成立）
 * ⟹ 门红线是**构造性**的，不需要显式门标志。
 *
 * ⚠ **但 z 是个例外，它确实是标志位**（批 13.6 AO 施加点搬进 taa pass）：
 * `z = 1` ⟹ AO 已由 taa pass 施加，tonemap **必须**跳过自己的 AO 乘法（否则双施）；
 * `z = 0` ⟹ taa pass 不在图里，tonemap 照旧施加（TAA 关时的唯一施加点）。
 * 写入方 = `fillTaaData`，取值 = **实际图形态**（`decorateTaaPass` 的返回值，经
 * `SceneEnv::taaAoApplied`），**不是配置键 `taa.enabled`**——offscreen 变体配置开了 TAA
 * 也会装饰失败（`decorateTaaOrWarn` 返回 false），拿配置派生就会「taa pass 不存在 + tonemap
 * 又跳过」= AO 一次都不施加。两个分支位精确（`lerp(x,1,1) ≡ 1.0`、`lerp(x,1,0) ≡ x`）
 * ⟹ TAA 关时 tonemap 逐位不变（门红线）。
 */
export inline constexpr uint32_t kSceneDataTaaPrevContentShiftOffset = kSceneDataTaaOffset + 144u;
/**
 * 批 13.8 输入侧重建核选择（vec4，x = 核编号，yzw 保留）：0 = 双线性（**默认 = 逐位今天**）/
 * 1 = Catmull-Rom 4×4 / 2 = Lanczos3 6×6。消费端 = taa.frag.slang `sampleInputColor`。
 *
 * 为什么要新增 16B 而不是复用槽位：前四个 vec4 的**每个分量都有主**（feedback / historyValid /
 * c_t.xy ｜ prevViewProj 与 motion 是矩阵 ｜ c_{t−1}.xy / aoApplied / diagReject），
 * 复用 = 造第二份真相。本区因此从 160B 扩到 176B（`kSceneDataBytes` 随之 +16B，
 * 分配与每帧 staging 拷贝都取自本常量，无第二处硬编码）。
 *
 * 门红线是**构造性**的：门关（0）⟹ shader 走 `k < 0.5` 那一条 `SampleLevel`，
 * 与改前同一条表达式，一个 bit 不动（`just shot` sha256 实测钉死）。
 */
export inline constexpr uint32_t kSceneDataTaaInputKernelOffset = kSceneDataTaaOffset + 160u;
/**
 * 批 13.11 **历史裁剪消融门**（vec4：x = clip_mode，y = clip_soft λ，zw 保留）。
 *
 * 要回答的问题（实测根因 → `调试历史.md` 2026-10-06）：`clipToAabb` 在 **blend 之前**替换历史，
 * 而混合是 `lerp(clipped, cur, α)` ⟹ **一次裁剪 = 一次历史重置**（历史被换成"本帧邻域盒边界"，
 * 权重 1−α）。1px 高对比细线的 box5 在 16 个抖动相位间**双峰**（有杆 / 没杆）⟹ "历史是否被重置"
 * 由相位决定 ⟹ 相位依赖方差以 100% 权重注入输出。实测：门开态盒裁剪 6.60% 帧均面积扛着
 * **97.8%** 残余质量，而 `塌回 centroid` 恰 **0.00%**。
 *
 * 档位（0 = 默认 = **逐位今天**；1/2/3 是消融臂，4 是修法臂）：
 *   0 现行（box5 + 硬裁剪）
 *   1 关盒（历史不裁 = 短路的**天花板**参照）
 *   2 绕 centroid 扩 1.5×（§1.7「尺寸不是杠杆」的**重测臂**——那条结论跑在 CR 核奇函数的坏构建上）
 *   3 box9（3×3 邻域 = 空间加宽臂；⚠ 合成靶竖直杆上零区分力，只有真场景/斜向靶量得出）
 *   4 **软裁剪**（弹簧不是墙：历史每帧最多走完「到盒边界距离」的 λ 分之一）
 *   5 绕 centroid 扩 **K×**（K = y，参数化扩盒扫描臂；量「盒差多少倍」用，**不是交付修法**）
 *   6 **历史信任门**（🔴 实测**否决** 2026-10-06：三档 γ_up 只动 3–5%。内生矛盾 = 信任平衡点
 *     t* ≈ γ_up·p/(γ_up·p + γ_dn·(1−p))，而需要救的像素正是通过率 p 低的那批 ⟹ t* 低）：
 *     把「本位置最近有多常通过本测试」存进 taa_out.a（既有 blit 携带 ⟹ 零新附件），按
 *     `lerp(hist, hardClip, 1−trust)` 调制裁剪咬合深度。γ_up = 1/16（= 抖动周期）、γ_dn = 0.5。
 *     y = γ_up 覆盖位（扫 γ_up 用，0 = 用 1/16）。⚠ 与 `diag_reject` 互斥（抢同一个 alpha）：
 *     diag 开时本臂退化成臂 0。为什么单帧信息原理上分不开「细线」与「鬼影」→ taa.frag 同段注释。
 *   7 **标量时域 luma 膨胀**（**判据对、响应错**；由臂 8 取代）：同一个 alpha 载体改存"记忆"——
 *     `mem = max(本帧盒 luma 跨度, mem_prev · decay)`，收下 ⟺ `|histLuma − centroidLuma| ≤ mem`。
 *     问的是"这个亮度近几帧在这里出现过没有"，**扩大接受域**而非折扣测试 ⟹ 没有臂 6 的矛盾；
 *     且是**距离**判据（mem 有界于近几帧真实跨度）⟹ 不是关盒：物体离开后新面从未出现过，照样拒
 *     （实测鬼影 R：交付档 1.751 → **0.927**、boost 开 1.390 → **0.722**，两个工作点都优于现行）。
 *     decay 的**推导值 = 1 − 1/16**（记忆覆盖一整个抖动周期 = 全体相位的支撑集）；实测 d=0.9 就
 *     已过线 ⟹ 是**宽平台不是尖峰**，四机位 ρ 终点 0.0785–0.0952 全贴闭式 0.0827。
 *     y = decay 覆盖位（0 = 用推导值）。同样与 `diag_reject` 互斥。
 *     🔴 **本臂自带缺陷（2026-10-06 实测，勿当交付臂）**：响应是**硬开关**，而阈值 mem 带 τ=16 帧
 *     滞后 ⟹ 「带滞后的继电器反馈」= 极限环。brickfence 上 **278–857 px**（0.03–0.09%）出现
 *     **0.91 线性域**的跨周期跳变（基线 0–2 px；tie-flip 地板 0.001–0.03 ⟹ 不是噪声）。
 *     指纹四条全中：幅度大 / 面积小 / 对 d 不敏感（0.9~0.975 全在 0.4 附近）/ 对热身不敏感（×4 不塌）。
 *   8 **mem 门控软裁剪**（当前修法臂）：**判据与臂 7 一字不改**，只把硬开关换成**单边**线性斜坡
 *     `w = saturate((dist − mem)/(r·mem))`，过渡带 = `[mem, mem(1+r)]`、**接受域内恒为 0**
 *     （无量纲）⟹ 环路增益从 ∞ 降到 ≈1/r。**r = 1 是唯一不含自由数值的取值**（带宽 = 记忆半径本身）。
 *     实测（brickfence，r=1）：ρ = **0.0814** = 闭式 ρ\* 的 0.98×（**优于臂 7 的 0.0958** ⟹ 那些硬
 *     重置本身在贡献残余闪烁）、折叠超 τ 像素数 278–857 → **0**、max swing 0.899 → **0.100**。
 *     ⚠ 两条实测红线：① **带不能对称**——对称带吃掉接受域（`dist = mem` 处 w = 0.5 = 在证明要求
 *     全接受处裁掉一半历史），ρ 被从 0.0909 单调推到 0.2104、只有 r ≤ 0.5 过线 = 扫出来的参数，已否；
 *     ② **r 有稳定性下界 ≈ 0.5**（r = 0.25 炸回 662 px = **阳性对照**：连续化不够，增益 ≈1/r 须低于
 *     阈值）。下界是**测**的不是推的，但失效是响的（0 → 662 px 两个数量级），不是悄悄越线。
 *   9 **mem 接受 + ext 斜率**（债-62 的修法：把两份冲突的工**拆给两个量**）：接受判据与臂 7/8
 *     一字不改（`dist ≤ mem`，接受域内 w ≡ 0 ⟹ 闪烁性质原样保留），**斜坡宽度改用 `ext`**
 *     （本帧盒 luma 跨度，**不粘**）：`w = saturate((dist − mem)/ext)`（k = 1，不含自由常数）。
 *     物体离开后 ext 塌回背景的小跨度 ⟹ 斜坡自动变紧 ⟹ 鬼影快；静止边缘 ext 大 ⟹ 斜坡宽
 *     ⟹ 环路增益低 ⟹ 不振荡。定量预测：完全替换条件从臂 8 的 `dist > 2·mem` 变成
 *     `dist > mem + ext` ⟹ 鬼影延迟 ≈3.5 帧（臂 7 ≈2.9 / 臂 8 ≈13.6）。
 *     判据（跑前写死）：鬼影 R ≤ 1.0（boost 开**且**交付档）**且** ρ ≤ 0.09 **且** 超 τ = 0 px，
 *     缺一不算；不算 ⟹ 单标量接受域结案，转 velocity / 深度遮挡探测。
 *
 * λ（y，仅 mode=4 读）= **无量纲时间常数**，语义 = 裁剪能以多快的速度搬动历史。
 * λ=1 在语义上退化成硬裁剪（浮点上不逐位等于 mode 0，故红线只钉 mode 0）。
 * ⚠ 安全线：鬼影衰减 ≈ 1/λ 帧，累积窗 ≈ 1/α 帧——**λ < α 时盒不再是鬼影刹车**
 * （那时鬼影由混合决定，= 债-49 的风险区）。选值必须有这条对照，不许扫出来。
 *
 * 门红线**构造性**：mode=0 ⟹ shader 走改前那一条表达式（盒构造循环与裁剪调用一个 bit 不动），
 * 红线由 `just shot` sha256 钉。本区因此从 176B 扩到 192B。
 */
export inline constexpr uint32_t kSceneDataTaaClipControlOffset = kSceneDataTaaOffset + 176u;
export inline constexpr uint32_t kSceneDataTaaBytes = 192u;
static_assert(kSceneDataTaaOffset == 722080 && kSceneDataTaaMotionOffset == 722144
	&& kSceneDataTaaParamsOffset == 722208 && kSceneDataTaaPrevContentShiftOffset == 722224
	&& kSceneDataTaaInputKernelOffset == 722240 && kSceneDataTaaClipControlOffset == 722256,
	"scene.slang taa literals drifted from scene_data.cppm offsets");

/** scene data buffer 总字节数 = 头 128 + 对象矩阵表 + 灯数组区 + MVP 表 + 材质表 +
 * 辐照度 SH9 区 + 阴影 VP 区（= 721952B，编译期派生）；SceneDataService::ensure 的分配尺寸与
 * drawFrame 每帧 staging 拷贝区尺寸均为本值（vkrenderer.cppm ⑥.5）。 */
export inline constexpr vk::DeviceSize kSceneDataBytes =
	kSceneDataHeaderBytes + vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride
	+ kSceneDataLightArrayBytes
	+ vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride
	+ vk::DeviceSize{kSceneMatrixCapacity} * kSceneDrawStride
	+ kSceneDataIrradianceBytes
	+ kSceneDataShadowMatrixBytes
	+ kSceneDataShadowAuxBytes
	+ kSceneDataIzbBytes
	+ kSceneDataPcssBytes
	+ kSceneDataEvssmBytes
	+ kSceneDataTaaBytes;

/**
 * SceneData buffer 服务（2026-09-19 架构债务续账步骤 1：原 Resources::ensureSceneData/
 * getSceneDataBuffer + 三成员整体搬出——SceneData 是"shader 布局契约的 GPU 载体"，
 * 不是资源内核职责）。向内核领 buffer + 槽（Resources& 单向 import，内核不认识本模块），
 * 幂等语义与原 Resources 方法逐字节一致（scene.data 领运行时段槽）。
 * 生命周期：main 栈对象，构造于 Resources 之后、一切取用者（ShaderRegistry / GpuCull /
 * Renderer）之前。
 */
export class SceneDataService {
   public:
	/**
	 * @param kernel 资源内核（allocator / descriptorHeap / allocSlot 领槽）
	 * @param device 设备层（描述符写入）
	 */
	SceneDataService(RGL::Resources& kernel, RGL::DeviceContext& device)
		: kernel(kernel), device(device) {}

	/**
	 * SceneData buffer（Phase 5 批次 2）：幂等创建设备端 storage buffer（eTransferDst——
	 * 每帧内容由 staging 拷入）+ 描述符写 allocSlot 运行时段槽（同 slotTexture 模式，
	 * 纹理槽数随场景动态、light 槽无编译期偏移）。调用点 = 建模型管线条目前
	 *（ShaderRegistry::addModelEntry，同 ensureSharedSampler 时序）。
	 * @return resource heap 绝对槽号（binding mapping 烘焙用，shader 端 binding 3，
	 *   见 scene.slang）
	 */
	[[nodiscard]] uint32_t ensure();

	/** @return SceneData 设备端 buffer const 引用（drawFrame 每帧 staging copy 的
	 * 目标；未创建 = 空句柄，drawFrame 跳过拷贝——无模型场景 ensure 未被调用） */
	[[nodiscard]] const RGL::VmaBuffer& getBuffer() const { return sceneDataBuffer; }

   private:
	RGL::Resources& kernel;  // 资源内核引用（领 allocator/heap/槽）
	RGL::DeviceContext& device;  // 设备层引用（writeResourceDescriptorsEXT）
	RGL::VmaBuffer sceneDataBuffer;  // 设备端 scene data buffer（BDA + eTransferDst；未创建 = 空句柄）
	uint32_t sceneDataSlot = 0;  // scene data 在 resource heap 的槽号（allocSlot 领取）
	bool sceneDataReady = false;  // 就绪标记（ensure 幂等守卫）
};

/**
 * 幂等契约见 SceneDataService::ensure 注释。实现：buffer usage =
 * eShaderDeviceAddress | eStorageBuffer | eTransferDst，VMA_MEMORY_USAGE_AUTO
 * device-local（CPU 不直接写——每帧唯一写入方 = staging copy，勿用 createGpuBuffer 的
 * CPU_TO_GPU+MAPPED 形态，见体内注释）；slot = allocSlot(Runtime, "scene.data")；描述符 =
 * eStorageBuffer，data = buffer BDA 地址范围（全 kSceneDataBytes）。
 */
inline uint32_t SceneDataService::ensure() {
	if (sceneDataReady)
		return sceneDataSlot;
	vk::BufferCreateInfo info{};
	info.size = kSceneDataBytes;
	info.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo alloc{};
	// device-local：CPU 不直接写（每帧唯一写入方 = staging copy），勿用 createGpuBuffer 的
	// CPU_TO_GPU+MAPPED 形态
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	auto bufResult = kernel.getAllocator().createBufferWithAddress(device.getDevice(), info, alloc);
	if (!bufResult)
		throw std::runtime_error(bufResult.error().describe());
	sceneDataBuffer = std::move(*bufResult);

	auto slotResult = kernel.allocSlot(RGL::SlotSegment::Runtime, "scene.data");
	if (!slotResult)
		throw std::runtime_error(slotResult.error().describe());
	sceneDataSlot = *slotResult;

	vk::DeviceAddressRangeEXT range{sceneDataBuffer.getDeviceAddress(), kSceneDataBytes};
	vk::ResourceDescriptorInfoEXT desc{};
	desc.type = vk::DescriptorType::eStorageBuffer;
	desc.data.pAddressRange = &range;
	auto slotAddr = kernel.getDescriptorHeap().getSlotCpuAddress(sceneDataSlot);
	if (!slotAddr)
		throw std::runtime_error(slotAddr.error().describe());
	vk::HostAddressRangeEXT dst{*slotAddr, kernel.getDescriptorHeap().getDescriptorSize()};
	device.getDevice().writeResourceDescriptorsEXT(
		std::array<vk::ResourceDescriptorInfoEXT, 1>{desc},
		std::array<vk::HostAddressRangeEXT, 1>{dst});
	sceneDataReady = true;
	return sceneDataSlot;
}

}  // namespace RGL
