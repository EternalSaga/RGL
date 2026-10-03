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

/** 批 12.6 TAA 区（EVSSM 常量 vec4 之后 144B，三段子区）：
 *  - `kSceneDataTaaOffset`（mat4, 64B）= 上一帧**未抖**的 proj·view。重投影公式：
 *      world = (sceneData@64 skyInvViewProj) · clip(uv, depth)   ← **抖动后**的逆矩阵
 *      prevClip = prevViewProj · (motionPrevFromCur · world)     ← 未抖矩阵（历史图是像素中心对齐的）
 *    两侧矩阵口径不同是**刻意的**：深度缓冲由抖动后矩阵产出，重建必须配套；
 *    历史图存的是「本像素中心的累积估计」，投影回去必须用未抖矩阵（Filament 同法）。
 *  - `kSceneDataTaaMotionOffset`（mat4, 64B）= `prevSpin · inverse(curSpin)`。本项目的
 *    运动源是 SceneSpin「全场景同一旋转矩阵」，所以一个 mat4 就**精确**补偿物体运动
 *    （Filament 纯深度重投影假设世界静止，动场景必拖影；RGL 这里没有这个 gap）。
 *    spin 关 = 单位阵。
 *  - `kSceneDataTaaParamsOffset`（vec4, 16B）= (x = feedback 历史权重, y = historyValid 1/0,
 *    z = jitterX 像素, w = jitterY 像素)。y=0 时 shader 直接输出本帧 = **逐位旁路**
 *    （首帧 / resize / 相机跳切走这条，同 ao.strength=0 的旁路纪律）。
 * 消费端 = taa.frag.slang 字面量（两侧 static_assert 钉死同步）。 */
export inline constexpr uint32_t kSceneDataTaaOffset = kSceneDataEvssmConstOffset + 16u;
export inline constexpr uint32_t kSceneDataTaaMotionOffset = kSceneDataTaaOffset + 64u;
export inline constexpr uint32_t kSceneDataTaaParamsOffset = kSceneDataTaaOffset + 128u;
export inline constexpr uint32_t kSceneDataTaaBytes = 144u;
static_assert(kSceneDataTaaOffset == 722080 && kSceneDataTaaMotionOffset == 722144
	&& kSceneDataTaaParamsOffset == 722208,
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
