module;
#include <vk_mem_alloc.h>  // VmaAllocationCreateInfo（VmaRaii 不导出 C 层，使用方自含，gpu_cull_impl 同款）
#include <glm/glm.hpp>  // sizeof(glm::vec2/mat4) 参与布局常量派生（GMF 三方头白名单内）
#include <fmt/format.h>  // 运行期对齐闸门的失败消息点名（checkMeshletDeviceFit 先例同用 fmt）
export module scene_data;

import rlstd;
import rlvk;
import rllogger;
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
//   @256 对象矩阵表（批 L 256B 对齐）：mat4 列主序 × kSceneMatrixCapacity（[objectSlot] = spin·world，
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

/** 迁移计划 §9.2/§9.7 布局红线（批 L）：**每个区基址静态 256B 对齐**。子范围描述符的
 *  `pAddressRange->address` 必须是 minUniformBufferOffsetAlignment（UBO）/
 *  minStorageBufferOffsetAlignment（SSBO）的整数倍（VUID-VkResourceDescriptorInfoEXT-
 *  type-12350/12351），256 覆盖桌面端两者常见上限；buffer 基址由
 *  `VmaAllocationCreateInfo::allocationAlignment = 256` 钉住，各区基址由 alignUp 从
 *  前区尾派生（区顺序/大小/写入内容不变，只有基址走 256 台阶）。运行期闸门见
 *  SceneDataService::ensure（查询值不满足 ⟹ 响亮失败，不静默画错）。 */
export inline constexpr uint32_t kSceneRegionAlign = 256;
/** 向上取整到 align（2 的幂）的整数倍——区基址派生唯一算子，勿在各处手写 `(v + 255) & ~255`。 */
export constexpr uint32_t alignUp(uint32_t value, uint32_t align) {
	return (value + align - 1u) & ~(align - 1u);
}

/** 迁移批 T0（§9.9 替代护栏）：**表尾毒化哨兵元素**的填充模式——每个 4 字节字一个词。
 *
 *  为什么需要它：各区**紧邻**（表大小都是 256 的整数倍，下一区正好接在表尾），越界读读到的是
 *  邻居区的**合法数据** = 静默错值，逐位锚点看不见。GPU-AV 在本架构不可用（§9.9 三条实测障碍，
 *  别再试），所以用**结构性的**办法把"静默"变成"必炸"：每张表尾追加一个元素、写成毒化模式，
 *  越界读 ⟹ 毒值进着色器 ⟹ 锚点必炸。
 *
 *  为什么是 `0xFFC00000`：三种元素类型读出来**都有毒**——float 读出来是 NaN（指数全 1、尾数
 *  非 0 ⟹ 非 inf）、uint 读出来是 4290772992（远超任何表容量/下标）、int 读出来是负数
 *  （`-4194304`，任何无符号语义的下标/计数都越界）。写 0 或写邻居的合法值 = 护栏恒不咬人。
 *
 *  🔴 生命周期：只在 **staging 分配时一次性**写（`Renderer::createSceneStaging`），
 *  `writeSceneStaging` **永不碰**哨兵字节——每帧是 `kSceneDataBytes` 全量 `vkCmdCopyBuffer`
 *  （vkrenderer_passrec.cppm ⑥.5 与 GpuCull::recordCullPass 两处都是全量），所以"写一次就在
 *  一直在"成立，代价 = 零每帧开销。任何"把整区 memset 到区尾"的写法都会覆盖哨兵 = 拆护栏，
 *  `writeClusterRangeRegion` 的清零段因此**止于哨兵之前**（tests/test_scene_sentinel 钉死）。 */
export inline constexpr uint32_t kSceneSentinelWord = 0xFFC00000u;

/** 迁移批 S-5（§9.5 binding 22）：场景**头区**类型化载体 `SceneHeaderBlock` 的基址
 *  （= scene data 起点，子范围 UBO 描述符的 `pAddressRange->address`）。头区三字段的偏移
 *  由下面三个常量派生并 static_assert 钉死（scene.slang 读取面零字节数）。 */
export inline constexpr uint32_t kSceneDataHeaderOffset = 0;
/** Phase 5.5：env 参数偏移（envIntensity float @48 + envMipRange float @52；契约字段
 * `SceneHeaderBlock::envParams`.xy，批 S-5 起 env.slang readEnvParams 改字段读）。派生自
 * sizeof(SceneLight)（C API 反射生成），shader 布局漂移即编译失败。 */
export inline constexpr uint32_t kSceneDataEnvOffset = sizeof(SceneLight);
/** Phase 5.5 批次 4：skyboxInvViewProj mat4 列主序 @64（inverse(proj·view)；契约字段
 *  `SceneHeaderBlock::skyInvVP`，批 S-5 起 sky.slang skyClipToWorld / shadow.slang 级联选级
 *  改字段读）。 */
export inline constexpr uint32_t kSceneDataSkyOffset = 64;
export inline constexpr uint32_t kSceneDataHeaderBytes = 128;   // light 48 + env 16 + sky invVP 64
// 批 N+1 删掉两枚**子字段槽位**常量：`kSceneDataExposureOffset = 56` / `kSceneDataAoOffset = 60`。
// 它们表达的是「envParams 这个 vec4 里的第 3、第 4 个 float」，而这件事现在由契约字段
// `SceneHeaderBlock::envParams.z` / `.w` 说（生成物 + shader 字段读），留两个字节数 = 双源。
// 下面这条链因此改写成 vec4 整体跨度（48..63）——env/sky 两枚常量本身保留：kSceneDataSkyOffset
// 是 tests/test_shadow_forward 字节级镜像的取材基址，kSceneDataEnvOffset 是它的派生式。
static_assert(kSceneDataEnvOffset == 48 && kSceneDataSkyOffset == 64 &&
	kSceneDataEnvOffset + sizeof(glm::vec4) == kSceneDataSkyOffset &&
	kSceneDataSkyOffset + sizeof(glm::mat4) == kSceneDataHeaderBytes);
/** 迁移批 S-5：头区**类型化载体与手写偏移常量对账**。字段级对账是**传递**成立的，两侧各钉
 *  一半（引擎 GMF 样式规则 B2 禁带 <cstddef> ⟹ 本文件不能直接用 offsetof，
 *  meshlet_hand_impl.cppm 同款处置）：
 *    · 生成物 scene_forward_types_generated.cppm 自带 `offsetof(SceneHeaderBlock, envParams)
 *      == 48` / `::skyInvVP == 64`（反射端钉字段 ↔ 字面量）；
 *    · 上面的 static_assert 钉 `kSceneDataEnvOffset == 48` / `kSceneDataSkyOffset == 64`
 *      （C++ 写入侧三个 memcpy 目标常量 ↔ 同一组字面量）。
 *  本处再钉**容量**：`kSceneDataHeaderBytes` 与 `sizeof(SceneHeaderBlock)` 必须逐位同
 *  （后 16B 是 128B 对齐填充——写入侧不写、读取侧不读；容量不同 = 描述符 range 与 UBO
 *  尺寸不一致，UBO 越界读未定义）。 */
static_assert(kSceneDataHeaderOffset == 0 && kSceneDataHeaderBytes == sizeof(SceneHeaderBlock),
	"头区容量与 SceneHeaderBlock 不一致（批 S-5 的 descriptor range 就是这个 sizeof）");
/** mat4 列主序 stride：派生自 `sizeof(glm::mat4)` 而非裸字面量。
 *  🔴 与契约 `ObjectMatrix`（shader 侧元素类型，`StructuredBuffer<ObjectMatrix>` 的 T）
 *  **直接钉死**：CPU 按 `kSceneMatrixStride` 逐步长写、GPU 按 `ArrayStride` 逐步长读，
 *  两者不同源 ⟹ 改 `ObjectMatrix` 加字段时区容量仍按 64 算、描述符 range 不变，而 shader
 *  按新步长索引 = **逐元素错位读**（画面渐变式坏掉，不是编译错误）。旧注释写的是
 *  "shader 端字面量 64u 须与本值一致——无反射端对应"，那条**已过时**：批 R 把
 *  `ObjectMatrix` 登记进 schema、批 T1 把 shader 侧矩阵基址/步长字面量清零，现在
 *  `sizeof(ObjectMatrix)` 就是反射端真相，直接钉即可（间接牙换直接牙）。 */
export inline constexpr uint32_t kSceneMatrixStride = sizeof(glm::mat4);
static_assert(kSceneMatrixStride == sizeof(ObjectMatrix)
	, "矩阵表步长与契约 ObjectMatrix 不同源（CPU 写步长 ↔ GPU ArrayStride 错位 = 逐元素错读）");
export inline constexpr uint32_t kSceneMatrixCapacity = 4096;   // 对象矩阵表容量（Phase 7：Bistro 1591 primitive 各一实例，1024 不够；scene data 缓冲随之 256KB）

/** 批 L（§9.7）：对象矩阵表基址 = 头(128) 尾之后按 256 取整 = **256**（原 @128 过不了
 * 子范围描述符对齐）。🔴 shader 侧**已无基址字面量**（批 T1：`scene_tables.slang` 的
 * `StructuredBuffer<ObjectMatrix> objectMatrixTable` = binding 23，基址由子范围描述符承载），
 * 本常量只剩 C++ 写侧与描述符两个消费者。头 128B 与本区之间 128B 为对齐间隙，
 * 无常驻内容、无消费者。 */
export inline constexpr uint32_t kSceneDataObjectMatrixOffset =
	alignUp(kSceneDataHeaderBytes, kSceneRegionAlign);
static_assert(kSceneDataObjectMatrixOffset == 256,
	"scene.slang readObjectMatrix / cull kSceneMatrixBase literal 256 out of sync");
/** 对象矩阵表**区字节数** = (可用容量 + 1 个哨兵元素) × stride（批 T0，§9.9）。
 *  🔴 可用容量仍是 `kSceneMatrixCapacity`（4096）——本常量多出来的那一个元素**不是**槽位，
 *  是毒化哨兵；把容量常量本身改成 4097 = 把"能装多少实例"这个语义弄脏（且越界写会静默
 *  踩进哨兵而不是被容量闸门拦下）。 */
export inline constexpr uint32_t kSceneDataObjectMatrixBytes =
	(kSceneMatrixCapacity + 1u) * kSceneMatrixStride;
/** 对象矩阵表哨兵元素的字节偏移（= 表基址 + 可用容量 × stride，即"下标 4096 那一格"）。 */
export inline constexpr uint32_t kSceneDataObjectMatrixSentinelOffset =
	kSceneDataObjectMatrixOffset + kSceneMatrixCapacity * kSceneMatrixStride;

/** Phase 7 D3：多灯数组区（追加于对象矩阵表之后——@0-128 头逐字节不动，forward/env/sky
 * shader 零改动 diff0）。基址 = 矩阵表尾按 256 取整（= 256 + 4097×64 = 262464 → 262656；批 T0 起表尾多一个哨兵元素）。
 * 消费端（迁移批 3 起）= lights.slang `ConstantBuffer<LightArrayBlock> lightArray`
 * （binding 24，本区**子范围 UBO 描述符**，SceneDataService::ensure 写一次）——shader 侧
 * 零字节字面量。static_assert 的三方同步降为两方 + 测试第三算（lights.slang 字面量已清零
 * = 被钉的第三方不存在了）：本派生式 ↔ 生成常量 kSceneDataLightArrayBase（lights_schema.lua
 * 字面量）↔ tests/test_reflection_layout 独立重推导，矩阵容量/头尺寸单边改动仍被抓住。 */
export inline constexpr uint32_t kSceneDataLightArrayOffset =
	alignUp(kSceneDataObjectMatrixOffset + kSceneDataObjectMatrixBytes, kSceneRegionAlign);
static_assert(kSceneDataLightArrayOffset == kSceneDataLightArrayBase,
	"scene data light-array offset drifted from lights_schema.lua kSceneDataLightArrayBase");
/** 灯数组区字节数 = 16B lightCount 头 + kMaxLights × kGpuLightStride（= 16 + 8×64 = 528；
 * 区布局 = lights.slang LightArrayBlock 经反射，与旧字节布局逐字节同）。
 * 🔴 本值是下游 MVP 表基址的派生输入（scene.slang readObjectMvp 字面量 263424 手工同源，
 * 该区暂无独立 assert 链——尺寸漂移由下方 sizeof assert + schema 交叉校验先抓）。 */
export inline constexpr uint32_t kSceneDataLightArrayBytes = 16u + kMaxLights * kGpuLightStride;
static_assert(sizeof(LightArrayBlock) == kSceneDataLightArrayBytes,
	"lights.slang LightArrayBlock (reflection-generated) size out of sync with region bytes");

/** Phase 7b+（NV DGC 规避）：per-object MVP 表区（追加于灯数组之后——既有各区基址
 * 逐字节不动 diff0）。CPU 每帧写（值 = 原 DrawArgs.mvp，位级一致），shader
 * scene.slang readObjectMvp 按 objectIndex 读——DrawArgs 去 mvp 字段后 push 36B（当时值，
 * 后 bindless 化终态 4B，见 kSceneDataDrawTableOffset），
 * 绕 NV DGC PUSH token >83B 多序列挂死（见 git:调试历史）。
 * 基址 = 灯数组尾按 256 取整（262656 + 528 = 263184 → 263424，scene.slang 字面量同值）。 */
export inline constexpr uint32_t kSceneDataMvpTableOffset =
	alignUp(kSceneDataLightArrayOffset + kSceneDataLightArrayBytes, kSceneRegionAlign);
/** MVP 表**区字节数** = (可用容量 + 1 个哨兵元素) × stride（批 T0，§9.9，对象矩阵表同形）。 */
export inline constexpr uint32_t kSceneDataMvpTableBytes =
	(kSceneMatrixCapacity + 1u) * kSceneMatrixStride;
/** MVP 表哨兵元素字节偏移（= 表基址 + 可用容量 × stride）。 */
export inline constexpr uint32_t kSceneDataMvpTableSentinelOffset =
	kSceneDataMvpTableOffset + kSceneMatrixCapacity * kSceneMatrixStride;

/** Phase 7b+（NV DGC 规避，续 MVP 表）：per-object 材质/几何表区（DrawArgs 去 mvp 后
 * 仍剩 baseColor/roughnessMetallic/totalTriangles/vertexOffset 是 per-object 数据，一并
 * 进 heap，DrawArgs 最终只留 objectIndex（4B push）——彻底绕开 NV DGC push 挂死）。
 * 基址 = MVP 表尾按 256 取整（263424 + 4097×64 = 525632 → 525824，scene.slang 字面量同值，下方
 * static_assert 钉死）。stride 48（`ObjectDraw`，scene_forward_types_generated 生成；
 * Phase 8 ③ 加 emissive 后 32→48）。 */
export inline constexpr uint32_t kSceneDataDrawTableOffset =
	alignUp(kSceneDataMvpTableOffset + kSceneDataMvpTableBytes, kSceneRegionAlign);
export inline constexpr uint32_t kSceneDrawStride = 48;
/** 材质表**区字节数** = (可用容量 + 1 个哨兵元素) × stride（批 T0，§9.9）。 */
export inline constexpr uint32_t kSceneDataDrawTableBytes =
	(kSceneMatrixCapacity + 1u) * kSceneDrawStride;
/** 材质表哨兵元素字节偏移（= 表基址 + 可用容量 × stride）。 */
export inline constexpr uint32_t kSceneDataDrawTableSentinelOffset =
	kSceneDataDrawTableOffset + kSceneMatrixCapacity * kSceneDrawStride;
// Phase 10 批 10.2a：钉死 scene.slang readObjectDraw 的字面量基址/stride——容量常量若变
// 而 shader 字面量未同步 = 此处编译报错（shader 侧无编译期反射通道，kTrianglesPerGroup 同纪律）
static_assert(kSceneDataDrawTableOffset == 525824,
	"scene.slang readObjectDraw literal 525824 out of sync");
static_assert(kSceneDrawStride == sizeof(ObjectDraw),
	"scene.slang ObjectDraw (reflection-generated) stride out of sync with table stride");

/** Phase 8 批次 ④a：漫反射辐照度 SH9 区（追加于材质表之后——既有各区基址逐字节不动 diff0）。
 * 9 × vec4 = 144B（.xyz=系数、.w 留白；CPU env 加载期从等距柱状源余弦加权投影 radiance，
 * shader scene.slang irradianceSH(N) 做辐照度卷积）。
 * 基址 = 材质表尾按 256 取整（525824 + 4097×48 = 722480 → 722688；Phase 8 ③ stride 32→48 后本区随之后移）。
 * 消费端（迁移批 2 起）= scene.slang `ConstantBuffer<IrradianceBlock> sceneIrradiance`
 * （binding 27，本区**子范围 UBO 描述符**，SceneDataService::ensure 写一次）——shader 侧零
 * 字节字面量，本区不再需要「C++ 常量 == shader 字面量」的 static_assert（批 1 湿面同判据：
 * 钉的对象已不存在）；区大小与契约 struct 的同步由下方 sizeof assert 钉死。 */
export inline constexpr uint32_t kSceneDataIrradianceOffset =
	alignUp(kSceneDataDrawTableOffset + kSceneDataDrawTableBytes, kSceneRegionAlign);
export inline constexpr uint32_t kSceneDataIrradianceBytes = 144;  // 9×vec4=144（16B 对齐）
static_assert(sizeof(IrradianceBlock) == kSceneDataIrradianceBytes,
	"scene.slang IrradianceBlock (reflection-generated) size out of sync with region bytes");

/**
 * Phase 11 批 11.2：级联阴影**光空间 VP 矩阵区**（追加于辐照度区之后——既有各区基址
 * 逐字节不动，沿用 Phase 8 ④a 的追加法）。区内容 = `kSceneShadowCascadeCount` 个 mat4
 * 列主序，索引 = cascade 号，相对基址 + 64×i。
 *
 * 🔵 **迁移批 S-1 起本区已类型化**：shader 端读路 = `ConstantBuffer<ShadowViewProjBlock>`
 * （scene.slang binding 28，经本区**子范围 UBO 描述符**，SceneDataService::shadowViewProjSlot），
 * 本文件不再有字节字面量与之同步；"mat4 列主序 + 64×i" 的口径由契约元素类型 ObjectMatrix
 * （反射生成，sizeof/offsetof assert）承载，写入端（writeSceneStaging）逐字节不变。
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
	alignUp(kSceneDataIrradianceOffset + kSceneDataIrradianceBytes, kSceneRegionAlign);
export inline constexpr uint32_t kSceneDataShadowMatrixBytes =
	kSceneShadowCascadeCount * static_cast<uint32_t>(sizeof(glm::mat4));
// 迁移批 S-1（§9.5 binding 28）：本区载体 = `ConstantBuffer<ShadowViewProjBlock>`
//（scene.slang，元素 = 契约类型 ObjectMatrix 列主序 4 列，元素步长 64）。区尺寸与反射
// 生成物同源钉死；原「scene.slang readShadowViewProj 字面量 722944」的两侧手写同步点
// 随字面量一起消失（本文件下方那条 assert 已删）。
static_assert(sizeof(ShadowViewProjBlock) == kSceneDataShadowMatrixBytes,
	"scene.slang ShadowViewProjBlock (reflection-generated) size out of sync with region bytes");
// 级联槽数 = 契约数组长度（改一侧必编译失败：数组是 shader 侧唯一可读上界）
static_assert(kSceneShadowCascadeCount
		== sizeof(ShadowViewProjBlock::viewProj) / sizeof(ObjectMatrix),
	"级联槽数与 scene.slang ShadowViewProjBlock.viewProj 数组长度失同步");

/** 阴影采样参数区（迁移批 S-2，§9.5 binding 29）：`ShadowParamsBlock`（128B = 8×vec4）的
 *  类型化 UBO 载体。合并前 = 7 个各 16B 的小小区（splits/texel/params/atlas 连排成 64B 的
 *  "aux" + IZB / PCSS / EVSSM(2×vec4) 各占一个 256 台阶，共吃 1024B）；合并后 128B ⟹
 *  其后各区往下搬 3 个 256 台阶 = **768B**（§9.7 原地重排，判据仍是两档锚点逐位不变）。
 *
 *  字段语义与顺序 = scene.slang `ShadowParamsBlock`（反射契约，生成物带逐字段 offsetof
 *  assert；字段序 = 合并前的物理序，前四段本就同区连排、逐字节同）。本文件只留**区基址与
 *  区容量**，字段级常量（旧 kSceneDataShadowSplitsOffset / Texel / Params / Atlas /
 *  Izb / Pcss / Evssm / EvssmConst 共 9 个）全部删除——写端（writeSceneStaging）按同一个
 *  生成 struct 整块 memcpy，两侧不再各写一份偏移。
 *
 *  为何单独一区而不拼进 VP 区：VP 是每槽 64B 矩阵，拼会破坏"区容量 = 固定槽数×stride"的
 *  简单不变式，而参数是一段量级元数据（改 bias 不需动矩阵区）。
 *
 *  门关红线（合并前后同）：IZB 段 flag=0 时因子恒 1 = 逐位旁路；PCSS/EVSSM 两段只有
 *  -DPCSS_SOFT / -DEVSSM_SOFT 变体读（原版 SPV 不引用 = 构造性保证）。 */
export inline constexpr uint32_t kSceneDataShadowParamsOffset =
	alignUp(kSceneDataShadowMatrixOffset + kSceneDataShadowMatrixBytes, kSceneRegionAlign);
export inline constexpr uint32_t kSceneDataShadowParamsBytes =
	static_cast<uint32_t>(sizeof(ShadowParamsBlock));
static_assert(kSceneDataShadowParamsBytes == 128,
	"scene.slang ShadowParamsBlock (reflection-generated) 尺寸变了——区容量取它，其后各区基址跟着搬");

/** 批 12.6 TAA 区（阴影参数区之后 **192B**，六段连排；第四段 16B 自批 13.5 改义）：
 *
 *  🔵 **迁移批 S-4 起本区已类型化**：shader 端读路 = `ConstantBuffer<TaaParamsBlock>`
 *  （scene.slang binding 30，经本区**子范围 UBO 描述符**，SceneDataService::taaSlot），
 *  本文件不再有字段级基址常量与 shader 字面量的手写同步点；字段偏移只存在于反射生成物
 *  一处，写端（writeSceneStaging）按同一个生成 struct 整块 memcpy。区容量与契约 struct
 *  同源（下方 sizeof assert）。
 *
 *  🔵 **批 13.5 的架构变更（读整段前先看这条）**：历史附件 `taa_hist` 自批 13.5 起住在
 *  **基空间**（无抖动网格），不再住抖动空间。做法 = taa.frag 把**本帧**输入按 c_t 做
 *  un-jitter 重建后再进时域混合 ⟹ 混合结果天然基空间 ⟹ 历史不再每帧被亚像素重采样
 *  （静止机位下重采样偏移恒 0）。这修掉「固定频率 + 固定位置」的 16 帧周期边缘 shimmer。
 *  全案 → `docs/taa_history_space_plan.md`。**本区六段的字节布局与偏移全部未变。**
 *
 *  - `prevViewProj`（mat4, 64B）= 上一帧**抖动后**的 proj·view。重投影公式：
 *      world = (sceneData@64 skyInvViewProj) · clip(uv, depth)   ← 本帧抖动后的逆矩阵
 *      prevClip = prevViewProj · (motionPrevFromCur · world)     ← 上一帧抖动后的矩阵
 *    两侧各用**自己那一帧的抖动后**矩阵（不是「口径不统一」，而是「两边都是渲染出各自
 *    那张图的那个矩阵」）：深度缓冲由抖动后矩阵产出 ⇒ 重建必须配套。
 *    ⚠ 这里曾长期存「未抖」那份（理由写作「历史图是像素中心对齐的」）——把「像素中心
 *    对齐」误读成「在未抖网格里」。后果 = 采样点系统性偏 -jitter_prev（±0.5px 且每帧
 *    随机变），静止机位实测 S1/S2 = 0.48×（开滤波逐帧差反而**翻倍**）。批 12.6 T4。
 *    🔵 **批 13.5：本裁决仍然成立、未被反转**，但**理由变了，且多了一条成对约束**：
 *      · 本字段产出的是**抖动网格**上的坐标 = `β_{t−1}(w) + c_{t−1}`（β = 基网格位置）。
 *      · 历史现在住基空间 ⟹ taa.frag 拿到后要**减 `prevContentShift`**
 *        才落到 `β_{t−1}(w)`。
 *      · ⟹ **两处成对、二选一**：要么「本字段抖动后 + shader 减 c_{t−1}」（现行），要么
 *        「本字段未抖 + shader 不减」（Filament 形态）。**都做 = 多减一次 c_{t−1}，
 *        都不做 = 少减一次 c_{t−1}**，两种都让静止机位 0.667px 游动原样回来。
 *        改一侧必须同时改另一侧。
 *  - `motion`（mat4, 64B）= `prevSpin · inverse(curSpin)`。本项目的运动源是 SceneSpin
 *    「全场景同一旋转矩阵」，所以一个 mat4 就**精确**补偿物体运动
 *    （Filament 纯深度重投影假设世界静止，动场景必拖影；RGL 这里没有这个 gap）。
 *    spin 关 = 单位阵。
 *  - `params`（vec4, 16B）= (x = feedback 本帧权重, y = historyValid 1/0,
 *    z/w = **本帧内容位移 c_t** 像素)。y=0 时 shader 直接输出本帧 = **逐位旁路**
 *    （首帧 / resize / 相机跳切走这条，同 ao.strength=0 的旁路纪律）。
 *    🔵 批 13.5：zw 此前装 `jitterPx` 但 **shader 从未读**（死载荷）；现改装 c_t 并**真读**
 *    （输入侧 un-jitter）。⚠ 两者差一个 y 轴翻号（`c = (+jx, −jy)`，见
 *    `taa_jitter.cppm::contentShiftPx`），照旧口径读会错一整轴。
 *  - `prevContentShift`（vec4, 16B）= **上一帧内容位移 c_{t−1}**（批 13.5）
 *    + **AO 已施加标志**（z，批 13.6）+ **历史拒收诊断门标志**（w）。
 *  - `inputKernel`（vec4, 16B，批 13.8 新增）= x = **输入侧 un-jitter 的重建核选择**
 *    （0 双线性 = 默认 / 1 CR 4×4 / 2 Lanczos3 6×6），yzw 保留。
 *  - `clipControl`（vec4, 16B，批 13.11 新增）= (x = **历史裁剪消融门**,
 *    y = 软裁剪 λ, z = **臂 10 的短窗端点**（常量 1.0；批 13.14b 起不再 = `taa.feedback`，
 *    否则与 `taaParams.x` 同值 ⟹ 臂 10 退化成臂 8，见债-66）, w 保留)；0 = 逐位今天。
 * 消费端 = taa.frag.slang / tonemap.frag.slang（经 scene.slang 的类型化 read 函数）。 */
export inline constexpr uint32_t kSceneDataTaaOffset =
	alignUp(kSceneDataShadowParamsOffset + kSceneDataShadowParamsBytes, kSceneRegionAlign);
export inline constexpr uint32_t kSceneDataTaaBytes =
	static_cast<uint32_t>(sizeof(TaaParamsBlock));
// 区容量与契约 struct 同源（192 = 2×ObjectMatrix + 4×vec4）。原「scene.slang 六条 TAA
// 字面量 == 本文件六个字段级偏移」的两侧手写同步点随字面量一起消失（本文件下方那条
// 六值 assert 已删），换成两条**同源** assert：区容量取 sizeof(生成物)、区基址这条是
// 「上游区改尺寸 ⟹ 本区搬家」的绊线（TAA 侧已无字节字面量，搬家时本 assert 红 = 强制
// 确认 SSR / 湿面 / 簇区间等下游绊线同步）。
static_assert(kSceneDataTaaBytes == 192u,
	"TaaParamsBlock (reflection-generated) size out of sync with TAA region bytes");
static_assert(kSceneDataTaaOffset == 723456,
	"TAA region base drifted from the documented offset 723456");

/*
 * 契约字段 `TaaParamsBlock::prevContentShift`（@区基址 + 144）：批 13.5 历史基空间修正
 * ——**上一帧内容位移** c_{t−1}（vec4 = x/y = 像素，**z = 批 13.6 AO 施加点标志**，w 保留）。
 * 槽位 = 复用批 13.2「基空间输出」删除后空出的那一格（批 S-2 的合并把它连同整区往下搬了
 * 768B，但「复用空槽、不额外占位」的形状不变）。
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
 * `SceneEnv::aoAppliedPreTemporal`），**不是配置键 `taa.enabled`**——offscreen 变体配置开了 TAA
 * 也会装饰失败（`decorateTaaOrWarn` 返回 false），拿配置派生就会「taa pass 不存在 + tonemap
 * 又跳过」= AO 一次都不施加。两个分支位精确（`lerp(x,1,1) ≡ 1.0`、`lerp(x,1,0) ≡ x`）
 * ⟹ TAA 关时 tonemap 逐位不变（门红线）。
 */
/*
 * 契约字段 `TaaParamsBlock::inputKernel`（@区基址 + 160）：批 13.8 输入侧重建核选择
 * （vec4，x = 核编号，yzw 保留）：0 = 双线性（**默认 = 逐位今天**）/
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
/*
 * 契约字段 `TaaParamsBlock::clipControl`（@区基址 + 176）：批 13.11 **历史裁剪消融门**
 * （vec4：x = clip_mode，y = clip_soft λ，**z = 短窗端点（臂 10 与
 * 臂 13 读；批 13.14b 删 boost 后固定填 1.0，出处见 `render_list_impl.cppm`）**，w 保留）。
 *
 * 要回答的问题（实测根因 → `git:调试历史` 2026-10-06）：`clipToAabb` 在 **blend 之前**替换历史，
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
 *   10 **臂 8 接受域 + α 交还逐像素证据**（债-62 的第三条路；当前唯一候选交付臂）：前九臂都在
 *     「一个标量接受域」里找平衡，而债-62 证明那是死路。本臂的观察 = **接受域之所以要同时打两份
 *     工，是因为 α 是全画面一个数**——`taaParams.x` 由 CPU 按**相机**静止帧数算，boost 开 = 0.01
 *     ⟹ 100 帧长窗发给每个像素，而"这个像素的历史可不可信"的证据（w）就在同一行 `lerp(clipped,
 *     cur, α)` 里却没被用来决定 α。鬼影的真实来源因此不是"臂 8 太粘"：臂 8 boost **关**时 R =
 *     1.347（**优于**今天 boost 关的 1.751），boost **开**才炸到 3.028 = 机位一动就按长窗洗历史。
 *     改法 = `α_eff = mix(α_boost, α_base, w)`（**零新常数**：两个 α 是 CPU 已算好的端点，w 是
 *     臂 8 已算好的量；`α_base` 走本槽 **z**）。结构保证：boost 只把 α 调小 ⟹ `α_eff ≥ α_boost`
 *     恒成立 ⟹ 历史只会比臂 8 **丢得更快** ⟹ 数学上不可能让鬼影比臂 8 更差（实测若更差 = 仪器错）。
 *     反向用例（必须逐位）：boost 关 ⟹ 两端点相等 ⟹ mix 恒等 ⟹ 与臂 8 逐位相同。
 *     判据（跑前写死，三条 AND）：ρ ≤ 0.09（五机位 + 合成靶）**且** 折叠超 τ = 0 px **且**
 *     鬼影 R（boost 开）≤ **1.751**（= 今天 boost 关的鬼影，口径 = 不再拿鬼影付 boost 换来的安静）。
 *   11 **记忆门控：把"允许动用跨帧色彩记忆"接到重投影位移上**（批 13.13；债-63 的出口，
 *     判据与待办 → `phase13_plan.md` §1.14）：`motion = ‖prevUvBase − baseUv‖/texel`（像素）、
 *     `gate = saturate(2 − motion)`、`mem = max(ext, memPrev·decay·gate)`。换轴逻辑 = 债-62 不可能性
 *     证明的适用条件明写「不含跨帧运动信息」⟹ 门一直开着。
 *     **可证明的那条（本臂主判据）**：批 13.5 后静止机位 `prevUvBase ≡ baseUv` ⟹ `gate` 恰 = 1.0
 *     ⟹ `x·1.0` IEEE 精确 ⟹ **静止机位下与臂 8 逐位相同**（不是"测出来差不多"）。
 *     🔴 两条已知缺陷（未修完，别当已解决）：① 旧注释的「`motion ≤ 1` 可证明静止」**是过强声明、
 *     已收回**——静止时 motion 恰为 0，1px 下界是保守选择不是导出量；② `motion` 是**每帧**位移而
 *     鬼影是整窗（≈100 帧）**累积**位移 ⟹ 慢速运动漏过去（gate 半开）。y = decay 覆盖位（同臂 8）。
 *   11 **记忆挂在重投影位移上**（债-62/债-63 的出口；当前唯一候选交付臂）：前十臂只读颜色距离，
 *     而债-62 不可能性证明的**适用条件自己写着**「不含跨帧运动信息」——门一直开着。本项目没有
 *     velocity buffer 是**因为不需要**：重投影解析（相机 prevViewProj + 场景 prevSpin）⟹
 *     `motion = ‖prevUvBase − baseUv‖/texel`（像素）现成。关键 = 批 13.5 基空间历史 ⟹ **静止机位下
 *     它恰为 0**，场景真动时是好几个像素 ⟹ 两情形线性可分（不是"大小不同"）。
 *     `mem = max(ext, memPrev·decay·gate)`，`gate = saturate(2 − motion)`：`motion ≤ 1` **可证明静止**
 *     （Halton ±0.5px 两帧差上界恰 1px）⟹ 记忆全开 = 与臂 8 **逐位同**；`motion ≥ 2` ⟹ 记忆清零
 *     ⟹ mem 退回本帧瞬时 ext = 臂 0 的无记忆硬盒。**两个断点由抖动调度推出、斜坡宽 = 1 像素
 *     = 采样格周期，零自由参数。**
 *     为什么臂 10 不够：场景转动时 `ext` 把 `mem` 撑大 ⟹ `w` 塌成 0 ⟹ α 耦合恰好失效；
 *     **根子是记忆本身在动的时候还在给豁免**，本臂关掉的正是这个豁免。
 *
 *   13 **臂 11 + α 也吃 gate**（批 13.15b；**债-69 的修法**）：臂 11 只把 gate 用在**接受域**上，
 *     平移阶跃仪器首次量到它在去遮挡处比现行默认盒**更鬼 23%**（`AUC 1.073/0.870 = 1.234`，
 *     → `phase13_plan.md` §1.21）。机制 = 瞬切后机位**又静止** ⟹ `motion` 恰为 0 ⟹ `gate = 1`
 *     ⟹ `mem` 是被撑大的记忆 ⟹ 盒比臂 0 的瞬时盒更宽。**臂 11 只加了记忆，遗忘那条线没人接。**
 *     本臂把同一份 gate 接给遗忘：`α_eff = α_long + (α_short − α_long)·(1 − gate)`，
 *     `α_short = z` = 常量 1.0（复用臂 10 的端点，**零新常数**）。
 *     🔴 **闪烁轴逐位不变**（本臂的免费一半 = 它的 bit 级反向用例）：`gate = saturate(2 − motion)`
 *     ⟹ 只要 `motion < 1px` 就 `2 − motion > 1` ⟹ saturate **精确返回 1.0** ⟹ `1 − gate` 恰为 `+0`
 *     ⟹ `α_eff ≡ α_long` **精确**。⚠ 这条零点**比臂 10/11 更宽**：不要求 `motion` 恰为 0，只要求
 *     < 1px。⟹ 静止机位（= 闪烁的全部论域，ρ 六机位 / 交付基线）不需要重拍。
 *     ⚠ **代价如实报**：`motion ≥ 2px` 期间 α = 1 ⟹ TAA 不提供抗锯齿（设计意图 = 运动中别把旧
 *     内容平均进来，但锯齿因此回到单帧水平）。
 *     ⚠ 编号：12′ 预留给 FSR3 加性可信度（债-68）⟹ 本臂取 13。
 *
 * λ（y，仅 mode=4 读）= **无量纲时间常数**，语义 = 裁剪能以多快的速度搬动历史。
 * λ=1 在语义上退化成硬裁剪（浮点上不逐位等于 mode 0，故红线只钉 mode 0）。
 * ⚠ 安全线：鬼影衰减 ≈ 1/λ 帧，累积窗 ≈ 1/α 帧——**λ < α 时盒不再是鬼影刹车**
 * （那时鬼影由混合决定，= 债-49 的风险区）。选值必须有这条对照，不许扫出来。
 *
 * 门红线**构造性**：mode=0 ⟹ shader 走改前那一条表达式（盒构造循环与裁剪调用一个 bit 不动），
 * 红线由 `just shot` sha256 钉。本区因此从 176B 扩到 192B。
 */

/** Phase 15 批 15.1 SSR 区（mat4 + 2×vec4, 96B）= 本帧**抖动后** proj·view + 尺度锚点与
 *  系数 + 粗糙度门控区间（尾区追加，既有偏移
 *  逐字节不动 diff0，先例 = 批 11.2 阴影 VP 区 / 批 12.6 TAA 区）。
 *
 *  口径（接消费者前必读）：
 *  - 与 @64 `skyInvViewProj = inverse(proj·view)` **严格互逆**：生产点填同一个表达式
 *    `viewProj.proj * viewProj.view`，@64 那份取逆、本区不取（`fillSsrViewProj`）。
 *  - **抖动后** = taaSystem 注入抖动的那一份 proj ⟹ 与 SSR ray march 读的 `scene_hdr`
 *    同一张采样网格、与深度缓冲配套（深度由抖动后矩阵产出）。口径漂成非抖动时采样位置
 *    整体偏亚像素而画面「看着还行」= 本项目反复踩的静默漂移，护栏 = tests/test_ssr_viewproj。
 *  - 为什么不让 shader 自己 `inverse(@64)`：`inverse(inverse(M))` 与 M **无位级契约**
 *    （余子式展开的舍入误差被两次求逆放大），而 SSR 的 uv 精度直接进 ray march 采样位置
 *    ——误差形态不是「糊一点」而是系统性偏移。CPU 已持有正向矩阵，直传 = 零重推导。
 *  - 接线前（批 15.1 交付时）无消费者，批 15.3 起 ssr_trace / ssr_apply 消费；**无条件写**、
 *    门关路径也保证值正确（同 shadow/TAA 尾区纪律：省下「按门分支写」的第二条路径）。
 *  消费端 = scene.slang `ConstantBuffer<SsrParamsBlock>`（binding 31，迁移批 S-3；本区三段
 *  = 契约字段 viewProj / params / gate，字节偏移只存在于反射生成物一处）。三段本就同区连排
 *  ⟹ 本批**零布局变更**；字段级常量（旧 kSceneDataSsrParamsOffset / kSceneDataSsrGateOffset）
 *  删除，写端（writeSceneStaging）按同一个生成 struct 整块 memcpy。
 *
 *  **字段 params**（vec4, 16B）= (sceneRadius, maxDistanceRadii, thicknessPx, biasPx)。
 *  **语义分组 = 「尺度锚点 + 三个由它导出的系数」**——四个分量同属"射线几何量"，故同槽：
 *  - `x` = 世界空间场景并集包围球半径（boundsSystem 每帧聚合，空场景 = 0）= **尺度锚点**。
 *    旧版 kMaxDistance/kThickness/kBias 是按闭式靶（米制）调的世界单位常量，交付场景
 *    bistro 是 scale=0.01（r≈1.16）⟹ 同一常量无法同时服务两者（2026-10-10 实测：靶
 *    radius=31.06 / bistro radius=1.16）。半径与相机推拉无关、随场景尺度线性走 ⟹
 *    maxDistance 由它导出后在任意尺度合理。
 *  - `y/z/w` = 三个**无量纲系数**（configs/runtime.lua `ssr` 块，每帧量、非创建期量）：
 *    maxDistance = x·y；thickness/bias = z/w × worldPerPixel(**采样点**视深，不是起点)。
 *
 *  生产者 = fillSsrParams（可测接缝），护栏 = tests/test_ssr_params + tests/test_scene_config
 *  （配置往返）。**默认值唯一真相源 = `RuntimeConfig` 成员初始化值**
 *  （shader 侧无 fallback 常量——两处定义迟早漂移，见 ssr.frag.slang 参数段）。
 *  字段位置 = `SsrParamsBlock::params`（@64，反射生成物 offsetof assert 钉死）；旧的
 *  `kSceneDataSsrParamsOffset` 常量随迁移批 S-3 删除（写端按生成 struct 整块写）。
 *
 *  **字段 gate**（vec4, 16B）= (roughFull, roughCutoff, blurScale, 保留)。
 *
 *  **为什么不塞进 params 字段**（设计决策，别"顺手合并"）：那四个是"由 sceneRadius
 *  导出的射线几何量"，本组是**与场景尺度无关的材质属性区间/像素域系数**（roughness 本身
 *  无量纲；blurScale 的单位 = 全分辨率 px，与场景尺度无关）。混进一槽 = 语义上两组旋钮
 *  共用一个名字，改任一组都要读另一组的口径。
 *
 *  `z` = blurScale（批 15.3 子步 B 占用，此前措辞「保留」）：apply pass 的螺旋模糊半径 =
 *  roughness × z（**全分辨率 px** 口径——trace 图半分辨率但 uv 空间一致，偏移按全分辨率
 *  换算）。消费者 = ssr_apply.frag；0 = 合法消融档（taps 全落中心 = 纯双线性上采样），
 *  CPU 侧不 clamp（readSsrBlock 同款纪律：消融档必须写得出来）。
 *
 *  ⚠ `x < y` 是 **shader 侧不变式**（`smoothstep(x, y, roughness)` 要求等号左 ≤ 右，
 *  反了 = 门控反向 = 粗糙面满反射、光滑面全关）。配置层**不校验**，理由与不变式落点 →
 *  `SsrTuning::roughCutoff` 注释。
 *  字段位置 = `SsrParamsBlock::gate`（@80）；旧的 `kSceneDataSsrGateOffset` 常量随迁移批
 *  S-3 删除。 */
export inline constexpr uint32_t kSceneDataSsrOffset =
	alignUp(kSceneDataTaaOffset + kSceneDataTaaBytes, kSceneRegionAlign);
export inline constexpr uint32_t kSceneDataSsrBytes =
	static_cast<uint32_t>(sizeof(SsrParamsBlock));
// 区容量与契约 struct 同源（96 = mat4 + 2×vec4，与合并前的三段连排逐字节同 ⟹ 其后各区
// 不动，本批**零布局变更**）。区基址这条是「上游区改尺寸 ⟹ 本区搬家」的绊线：SSR 侧
// 已无字节字面量（迁移批 S-3），搬家时本 assert 红 = 强制确认湿面/簇区间等下游绊线同步。
static_assert(kSceneDataSsrBytes == 96u,
	"SsrParamsBlock (reflection-generated) size out of sync with SSR region bytes");
static_assert(kSceneDataSsrOffset == 723712,
	"SSR region base drifted from the documented tail offset 723712");

/** Phase 15 批 15.2 子步 A 湿面参数区（2×vec4, 32B）= 身份调制系数 + 空间变化参数
 *  （尾区追加，既有偏移逐字节不动 diff0，先例 = 批 11.2 阴影 VP 区 / 批 12.6 TAA 区 /
 *  批 15.1 SSR 区）。
 *
 *  口径（接消费者前必读）：
 *  - 湿面**身份**（哪些面湿）**不占参数** = `1 − smoothstep(ssrGate.x, ssrGate.y, roughness)`，
 *    与 SSR 粗糙度门控**同区间同源**（资产已标定：湿材质 roughness 0.122 在区间下方 →
 *    wetness = 1、干面 ~0.74/1.0 在上方 → 0，出处 → docs/phase15_plan.md §2.4 修正块）
 *    ⟹ 结构上排除「反射了但没压暗」的错配。本区只放**空间变化与调制系数**。
 *  - 消费者 = deferred_geometry.frag.slang 湿面 G-buffer 调制（15.2B 接线，**默认开**）；
 *    无条件写（同 shadow/TAA/SSR 尾区纪律），`strength = 0` 消融档实测逐位 = 换锚前旧图。
 *  消费端（迁移批 1 起）= scene.slang `ConstantBuffer<WetParamsBlock> sceneWet`（binding 32，
 *  本区**子范围 UBO 描述符**，SceneDataService::ensure 写一次）——shader 侧零字节字面量，
 *  本区因此不再需要「C++ 常量 == shader 字面量」的 static_assert（钉的对象已不存在）。 */
export inline constexpr uint32_t kSceneDataWetOffset =
	alignUp(kSceneDataSsrOffset + kSceneDataSsrBytes, kSceneRegionAlign);
// ── 湿面区两槽**语义**（迁移批复补后：两槽的**位置**只剩契约 `WetParamsBlock` 一处，
//    下面两段只记语义，不再声明偏移；写端 = writeSceneStaging 整块 memcpy）
//
// `WetParamsBlock::params`（vec4）= (strength, dark, roughLo, roughHi)
//   - `x` = 总开关兼强度（0 = 关 = 消融档；**默认 1.0 = 开**，用户拍板「湿面默认开」）。
//   - `y` = 湿区 albedo 压暗系数（×0.5–0.7 区间取中）。
//   - `z/w` = 水洼/湿石粗糙度端点（🔴 shader 侧不变式 `z < w`，反了 = 空间调制方向倒置；
//     配置层**不校验**，理由 → readWetBlock 注释）。
//   **默认值唯一真相源 = `WetTuning` 成员初始化值**（shader 侧无 fallback 常量——两处定义
//   迟早漂移，见 SsrTuning 同款纪律）。生产者 = fillWetParams（可测接缝），护栏 =
//   tests/test_wet_params（两源结构）+ tests/test_scene_config（配置往返）。
//
// `WetParamsBlock::spatial`（vec4）= (rippleAmp, puddleFreqRadii, 保留, 保留)
//   **为什么不塞进 params**（设计决策，别"顺手合并"，分组理由照 ssrParams/ssrGate 那条）：
//   params 四个是**身份调制系数**（压暗/粗糙度端点，逐湿面常数），本两个是**空间变化的
//   形状参数**（水膜扰动幅度/水洼噪声频率，逐像素自变量不同）。混进一槽 = 语义上两组旋钮
//   共用一个名字，改任一组都要读另一组的口径。
//   `y` = puddleFreqRadii：噪声频率 = 本值 / sceneRadius（**尺度无关**口径，锚点与 SSR
//   的 ssrParams.x 同一颗 SceneBounds.radius 球）。
export inline constexpr uint32_t kSceneDataWetBytes = 32u;
// 迁移批 1：原「C++ 常量 == scene.slang 湿面字面量」的 static_assert 已删——shader 侧
// 改读 binding 32 的 ConstantBuffer<WetParamsBlock>（子范围描述符基址 = 本区常量，
// C++ 写侧与描述符**同源同值**，不存在可单侧漂移的第二份字面量）。
// 🔴 收口后的**补做**（原注释在此声称"写侧子区 == 生成 struct 字段偏移双钉"，实测**第二钉
// 不存在**：测试只钉了 `子区B == 子区A + 16u`，字面量对字面量，与契约 `offsetof` 无连线）：
// 写端改为按 `WetParamsBlock` 整块写，两个子区常量随之删除 ⟹ "spatial 在区内第 16 字节"
// 这个事实**只剩反射一处**。同步链从"三段两钉"变成"两段一钉 + 一处根本不需要钉"。

/** 簇化轴 ★1-a1i：per-object **簇区间表**区（湿面区之后，尾区追加，既有偏移逐字节不动
 *  diff0，先例 = 批 11.2 阴影 VP 区 / 批 15.1 SSR 区 / 批 15.2 湿面区）。
 *
 *  条目 = `ObjectClusterRange{base, count, l0Count, reserved}`（stride 16，布局真相源 =
 *  scene.slang struct 经 C API 反射生成，见 scene_forward_types_generated）。索引 =
 *  objectIndex（= MeshRef.objectSlot，与对象矩阵/MVP/ObjectDraw 三张 per-object 表同源）。
 *
 *  🔴 **方案 B（docs/mdi_plan.md §10.1，主 agent 拍板，别再重新讨论）**：新增独立表而不是
 *  给 `ObjectDraw` 加字段——后者把 stride 48→56 改在**所有** shader 热路径整读的结构上，
 *  "门关逐位不变"只能靠"没人读新字段"保证（弱）；本方案现有结构逐字节不动 ⟹ 不变性是
 *  **构造性**的。代价 = 多一张表 + 一个 offset + 每帧多拷 64KB（见下方风险注记）。
 *
 *  内容来源 = `meshlet_ranges::scanClusterRanges`（装配层启动期扫一遍产物 groups），
 *  门 `meshlet.enable` 关 = 不写 = 全 0（本区常驻零）。🔴 **本批零消费者**：读取点归
 *  ★1-a1 的 mesh shader（`base + (groupID.x >> 1)` = 全局簇下标（★1-a1ii-α″ 去索引发射后每簇固定 2 槽：
 *  半区号 = `groupID.x & 1`，每槽 ≤ 85 三角，3×85 = 255 ≤ maxMeshOutputVertices）），门开时它只叠加在旧路径
 *  之后（同 objectIndex、同深度），门关逐位不变。
 *
 *  容量与对象矩阵表同 = `kSceneMatrixCapacity`（表按 objectIndex 索引，容量必须一致，
 *  否则越界写的是**下一个字节**——本区是尾区，越界即写出 buffer 外）。
 *  ⚠ 每帧 staging 写入面因此从 724000B 涨到 789776B（+65552B ≈ +9%）：scene data 是单
 *  buffer 整区 vkCmdCopyBuffer，追加尾区的成本按容量线性走，这条账记在 ★1-a 的性能面。 */
export inline constexpr uint32_t kSceneDataClusterRangeOffset =
	alignUp(kSceneDataWetOffset + kSceneDataWetBytes, kSceneRegionAlign);
/** 条目字节数 = 生成 struct 的 sizeof（C++ 侧漂移即编译失败；shader 侧由反射实查）。 */
export inline constexpr uint32_t kSceneDataClusterRangeStride = sizeof(ObjectClusterRange);
/** 容量与其余三张 per-object 表同（按 objectIndex 索引 ⟹ 容量必须同）。 */
export inline constexpr uint32_t kSceneDataClusterRangeCapacity = kSceneMatrixCapacity;
/** 簇区间表**区字节数** = (可用容量 + 1 个哨兵元素) × stride（批 T0，§9.9）。 */
export inline constexpr uint32_t kSceneDataClusterRangeBytes =
	(kSceneDataClusterRangeCapacity + 1u) * kSceneDataClusterRangeStride;
/** 簇区间表哨兵元素字节偏移（= 表基址 + 可用容量 × stride）。 */
export inline constexpr uint32_t kSceneDataClusterRangeSentinelOffset =
	kSceneDataClusterRangeOffset + kSceneDataClusterRangeCapacity * kSceneDataClusterRangeStride;
// 钉的是「C++ 常量 == 已知的基址/布局」：改 scene_data.cppm 一侧必编译失败（本区暂无
// shader 字面量消费者，★1-a1 接线时再补 scene.slang 侧字面量的同步注释）。
static_assert(kSceneDataClusterRangeOffset == 724224,
	"cluster-range region drifted from the documented tail offset 724224");
static_assert(kSceneDataClusterRangeStride == 16, "ObjectClusterRange stride drifted from 16B");
static_assert(kSceneDataClusterRangeCapacity == kSceneMatrixCapacity,
	"cluster range capacity must equal the object matrix capacity (both index by objectIndex)");

/** scene data buffer 总字节数 = **尾区（簇区间表）末端**（= 724224 + 65552 = 789776B，
 * 编译期派生）。批 L 起各区基址按 256 取整、区间存在对齐间隙 ⟹ 旧的「各区字节数求和」
 * 公式失真（求和会漏掉间隙），改为直接取尾区末端——它同时就是「最后一个区无重叠无空洞
 * 且贴 buffer 尾」的构造性定义。SceneDataService::ensure 的分配尺寸与 drawFrame 每帧
 * staging 拷贝区尺寸均为本值（vkrenderer.cppm ⑥.5）。 */
export inline constexpr vk::DeviceSize kSceneDataBytes =
	kSceneDataClusterRangeOffset + kSceneDataClusterRangeBytes;
static_assert(kSceneDataBytes == 789776,
	"kSceneDataBytes drifted from the documented 789776 (tail region end)");

/** 批 L 布局红线（§9.7）：**所有区基址 256B 对齐**——子范围描述符（§9.2）的唯一前置。
 *  新增区时把基址加进本组（漏加 = 本 assert 红，而不是运行期 VUID 炸）。 */
static_assert(kSceneDataObjectMatrixOffset % kSceneRegionAlign == 0
	&& kSceneDataLightArrayOffset % kSceneRegionAlign == 0
	&& kSceneDataMvpTableOffset % kSceneRegionAlign == 0
	&& kSceneDataDrawTableOffset % kSceneRegionAlign == 0
	&& kSceneDataIrradianceOffset % kSceneRegionAlign == 0
	&& kSceneDataShadowMatrixOffset % kSceneRegionAlign == 0
	&& kSceneDataShadowParamsOffset % kSceneRegionAlign == 0
	&& kSceneDataTaaOffset % kSceneRegionAlign == 0
	&& kSceneDataSsrOffset % kSceneRegionAlign == 0
	&& kSceneDataWetOffset % kSceneRegionAlign == 0
	&& kSceneDataClusterRangeOffset % kSceneRegionAlign == 0,
	"every scene data region base must be kSceneRegionAlign (256B) aligned (VUID-12350/12351)");

/** 批 T0 哨兵红线（§9.9）：四张 per-object 表的**区大小 = 可用容量 + 1 个元素**，且哨兵恒在
 *  「下标 capacity 那一格」。这四条是护栏本身的位置定义——任何一侧漂了（区大小忘了 +1、
 *  哨偏移写成 capacity-1、容量常量被偷偷改成 4097）都在编译期炸，而不是让护栏静默失效。
 *  另两条同样致命的性质（毒值三型有毒、写侧不覆盖哨兵）断言写不出字节语义，落测：
 *  tests/test_scene_sentinel。 */
static_assert(kSceneDataObjectMatrixBytes == (kSceneMatrixCapacity + 1u) * kSceneMatrixStride
		&& kSceneDataMvpTableBytes == (kSceneMatrixCapacity + 1u) * kSceneMatrixStride
		&& kSceneDataDrawTableBytes == (kSceneMatrixCapacity + 1u) * kSceneDrawStride
		&& kSceneDataClusterRangeBytes
			== (kSceneDataClusterRangeCapacity + 1u) * kSceneDataClusterRangeStride,
	"per-object table region must be capacity + 1 elements wide (the extra one is the poison sentinel)");
static_assert(kSceneMatrixCapacity == 4096u && kSceneDataClusterRangeCapacity == 4096u,
	"usable capacity must stay 4096 — the sentinel widens the REGION, never the capacity");
/** 哨兵相对各自表基址的偏移恒 = 可用容量 × stride（= "下标 capacity 那一格"），且它正是该区
 *  最后一个元素（哨兵尾 = 区尾 = 下一区尾对齐前的最后一个字节）。 */
static_assert(kSceneDataObjectMatrixSentinelOffset - kSceneDataObjectMatrixOffset
			== kSceneMatrixCapacity * kSceneMatrixStride
		&& kSceneDataMvpTableSentinelOffset - kSceneDataMvpTableOffset
			== kSceneMatrixCapacity * kSceneMatrixStride
		&& kSceneDataDrawTableSentinelOffset - kSceneDataDrawTableOffset
			== kSceneMatrixCapacity * kSceneDrawStride
		&& kSceneDataClusterRangeSentinelOffset - kSceneDataClusterRangeOffset
			== kSceneDataClusterRangeCapacity * kSceneDataClusterRangeStride,
	"sentinel must sit exactly at index == capacity of its own table (that is the first OOB slot)");
static_assert(kSceneDataObjectMatrixSentinelOffset + kSceneMatrixStride
			== kSceneDataObjectMatrixOffset + kSceneDataObjectMatrixBytes
		&& kSceneDataMvpTableSentinelOffset + kSceneMatrixStride
			== kSceneDataMvpTableOffset + kSceneDataMvpTableBytes
		&& kSceneDataDrawTableSentinelOffset + kSceneDrawStride
			== kSceneDataDrawTableOffset + kSceneDataDrawTableBytes
		&& kSceneDataClusterRangeSentinelOffset + kSceneDataClusterRangeStride
			== kSceneDataClusterRangeOffset + kSceneDataClusterRangeBytes,
	"sentinel must be the LAST element of its region (region = capacity+1 elements, no slack)");
/** 哨兵落点的**绝对地址**（手抄字面量 = 第二源）：区大小或任何前区尺寸单边漂移都会在这里炸，
 *  而上面的派生式对那种漂移是"跟着一起漂"的（派生式自己抓不到"整体错位"）。 */
static_assert(kSceneDataObjectMatrixSentinelOffset == 262400
		&& kSceneDataMvpTableSentinelOffset == 525568
		&& kSceneDataDrawTableSentinelOffset == 722432
		&& kSceneDataClusterRangeSentinelOffset == 789760,
	"sentinel absolute address drifted (region width or some upstream region changed size)");

/**
 * 把四张 per-object 表尾的**毒化哨兵元素**写进 staging（批 T0，§9.9 替代护栏本体）。
 *
 * 调用点**只有一个**：`Renderer::createSceneStaging` 分配每个 staging ring slot 之后各调一次。
 * 之后 `writeSceneStaging` 每帧只写各区的有效段（矩阵/MVP/材质三表 memcpy 长度 = 实际条目数，
 * 簇区间表的清零段止于哨兵之前），**永不碰**这 4×(48|64|16) 字节；每帧的 `kSceneDataBytes`
 * 全量 `vkCmdCopyBuffer` 把毒值原样带进 device buffer ⟹ "分配时写一次"就一直在。
 *
 * 判据（有牙）：把任一 read 的下标临时 +capacity（正好读进哨兵）→ 两档锚点 sha 必变；
 * 还原回锚。这条排掉"哨兵其实没被毒化 / 位置不对 / 每帧被覆盖"三种护栏失效。
 *
 * @param staging 整块 scene data staging（大小须 ≥ `kSceneDataBytes`）
 */
export inline void writeSceneSentinels(std::span<std::byte> staging) {
	const auto fill = [&](uint32_t offset, uint32_t bytes) {
		for (uint32_t i = 0; i < bytes; i += sizeof(uint32_t)) {
			const uint32_t word = kSceneSentinelWord;
			std::memcpy(staging.data() + offset + i, &word, sizeof(word));
		}
	};
	fill(kSceneDataObjectMatrixSentinelOffset, kSceneMatrixStride);
	fill(kSceneDataMvpTableSentinelOffset, kSceneMatrixStride);
	fill(kSceneDataDrawTableSentinelOffset, kSceneDrawStride);
	fill(kSceneDataClusterRangeSentinelOffset, kSceneDataClusterRangeStride);
}

/**
 * 把 per-object 簇区间表写进 staging 的 ★1-a1i 尾区（**可测接缝**：本函数不需要 VkDevice，
 * 故「门关 = 整区全 0」与「有效段之外清零」两条能在 `just test` 里逐字节验）。
 *
 * 先清尾再写有效段 = 「门 `meshlet.enable` 关（表空）⟹ 整区全 0」是**构造性**成立，不依赖
 * staging 映射内存的初值（VMA 不保证新分配内存为 0，未初始化字节会静默变成一条假区间）。
 *
 * @param staging 整块 scene data staging（大小须 ≥ `kSceneDataBytes`，调用方 =
 *                `Renderer::writeSceneStaging`，尺寸由 `SceneDataService::ensure` 保证）
 * @param ranges  按 objectSlot 索引的表（空 = 门关）
 * @return false = 表超出容量（调用方报错；**不静默截断**——截断的失效形态是「排在容量之外
 *         的实例没有区间」= 静默少画一块，与 `objectDraws` 的容量契约同纪律）
 */
export [[nodiscard]] inline bool writeClusterRangeRegion(
	std::span<std::byte> staging, std::span<const ObjectClusterRange> ranges) {
	if (ranges.size() > kSceneDataClusterRangeCapacity)
		return false;
	const size_t usedBytes = ranges.size() * sizeof(ObjectClusterRange);
	auto* tail = staging.data() + kSceneDataClusterRangeOffset;
	if (usedBytes > 0)
		std::memcpy(tail, ranges.data(), usedBytes);
	// 🔴 清零段**止于哨兵之前**（批 T0）：写成 `kSceneDataClusterRangeBytes - usedBytes` 会把
	// 表尾那个毒化哨兵元素一起抹成 0 = 每帧拆一次护栏（§9.9 的"写一次就一直在"前提正是
	// "没人每帧覆盖它"）。tests/test_scene_sentinel 的「写侧不覆盖哨兵」用例钉死这条。
	std::memset(tail + usedBytes, 0,
		static_cast<size_t>(kSceneDataClusterRangeCapacity) * sizeof(ObjectClusterRange)
			- usedBytes);
	return true;
}

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
	 * 每帧内容由 staging 拷入）+ 逐区领 allocSlot 运行时段槽（同 slotTexture 模式，
	 * 纹理槽数随场景动态、light 槽无编译期偏移）。调用点 = 建模型管线条目前
	 *（ShaderRegistry::addModelEntry，同 ensureSharedSampler 时序）。
	 *
	 * 🔴 批 N+1 起返回 void：迁移期它返回「全区 binding 3」的槽号，而那条全区读**已经
	 * 没有消费者**（各区全部走子范围类型化载体）⟹ 槽号没有去处。buffer 本体仍在这里创建，
	 * 各子范围描述符全部指向它（`getBuffer()`），所以**本函数一次都不能少调**。
	 */
	void ensure();

	/** 对齐闸门判据（ensure 首步调用；实现见文件下方，注释在那）。 */
	void checkAlignmentGate() const;

	/** @return SceneData 设备端 buffer const 引用（drawFrame 每帧 staging copy 的
	 * 目标；未创建 = 空句柄，drawFrame 跳过拷贝——无模型场景 ensure 未被调用） */
	[[nodiscard]] const RGL::VmaBuffer& getBuffer() const { return sceneDataBuffer; }

	/**
	 * 头区 UBO 子范围槽号（迁移批 S-5，shader 端 binding 22；用法与 wetSlot 同型，
	 * PipelineBindings::slotSceneHeader）。描述符内容 = {buffer 基址 +
	 * kSceneDataHeaderOffset（= 0）, sizeof(SceneHeaderBlock)} 的 eUniformBuffer——
	 * 头区基址就是 buffer 基址，但**仍走子范围描述符**而不是整 buffer：UBO 的 range 必须
	 * 正好是 128B（多给 = shader 越界读到矩阵表区，少给 = UBO 尺寸与契约不符）。
	 */
	[[nodiscard]] uint32_t headerSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneHeaderSlot;
	}

	/**
	 * 湿面区 UBO 子范围槽号（迁移批 1，shader 端 binding 32；ShaderRegistry 烘 mapping 用，
	 * PipelineBindings::slotSceneWet）。幂等取号语义同 ensure（内部先 ensure）；描述符内容 =
	 * {buffer 基址 + kSceneDataWetOffset, sizeof(WetParamsBlock)} 的 eUniformBuffer。
	 */
	[[nodiscard]] uint32_t wetSlot() {
		ensure();  // 幂等取号（批 N+1：ensure 不再返回槽号，本访问器返回自己的子范围槽）
		return sceneWetSlot;
	}

	/**
	 * 辐照度区 UBO 子范围槽号（迁移批 2，shader 端 binding 27；用法与 wetSlot 同型，
	 * PipelineBindings::slotSceneIrradiance）。描述符内容 = {buffer 基址 +
	 * kSceneDataIrradianceOffset, sizeof(IrradianceBlock)} 的 eUniformBuffer。
	 */
	[[nodiscard]] uint32_t irradianceSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneIrradianceSlot;
	}

	/**
	 * 灯数组区 UBO 子范围槽号（迁移批 3，shader 端 binding 24；用法与 wetSlot 同型，
	 * PipelineBindings::slotSceneLights）。描述符内容 = {buffer 基址 +
	 * kSceneDataLightArrayOffset, sizeof(LightArrayBlock)} 的 eUniformBuffer。
	 */
	[[nodiscard]] uint32_t lightSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneLightArraySlot;
	}

	/**
	 * 阴影光空间 VP 区 UBO 子范围槽号（迁移批 S-1，shader 端 binding 28；用法与 wetSlot
	 * 同型，PipelineBindings::slotSceneShadowViewProj）。描述符内容 = {buffer 基址 +
	 * kSceneDataShadowMatrixOffset, sizeof(ShadowViewProjBlock)} 的 eUniformBuffer。
	 */
	[[nodiscard]] uint32_t shadowViewProjSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneShadowViewProjSlot;
	}

	/**
	 * 阴影采样参数区 UBO 子范围槽号（迁移批 S-2，shader 端 binding 29；用法与 wetSlot
	 * 同型，PipelineBindings::slotSceneShadowParams）。描述符内容 = {buffer 基址 +
	 * kSceneDataShadowParamsOffset, sizeof(ShadowParamsBlock)} 的 eUniformBuffer。
	 */
	[[nodiscard]] uint32_t shadowParamsSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneShadowParamsSlot;
	}

	/**
	 * SSR 区 UBO 子范围槽号（迁移批 S-3，shader 端 binding 31；用法与 wetSlot 同型，
	 * PipelineBindings::slotSceneSsr）。描述符内容 = {buffer 基址 +
	 * kSceneDataSsrOffset, sizeof(SsrParamsBlock)} 的 eUniformBuffer。
	 */
	[[nodiscard]] uint32_t ssrSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneSsrSlot;
	}

	/**
	 * TAA 区 UBO 子范围槽号（迁移批 S-4，shader 端 binding 30；用法与 wetSlot 同型，
	 * PipelineBindings::slotSceneTaa）。描述符内容 = {buffer 基址 +
	 * kSceneDataTaaOffset, sizeof(TaaParamsBlock)} 的 eUniformBuffer。
	 */
	[[nodiscard]] uint32_t taaSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneTaaSlot;
	}

	/**
	 * 逐对象世界矩阵表 SSBO 子范围槽号（类型化迁移 批 T1，shader 端 binding 23；
	 * PipelineBindings::slotObjectMatrices）。描述符内容 = {buffer 基址 +
	 * kSceneDataObjectMatrixOffset, kSceneDataObjectMatrixBytes} 的 eStorageBuffer——
	 * 长度**含尾部哨兵**（批 T0），越界索引因此落在描述符范围内、落在 poison 值上。
	 */
	[[nodiscard]] uint32_t objectMatrixSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneObjectMatrixSlot;
	}

	/** 逐对象 MVP 表 SSBO 子范围槽号（批 T1，shader 端 binding 25；同 objectMatrixSlot 形态）。 */
	[[nodiscard]] uint32_t objectMvpSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneObjectMvpSlot;
	}

	/** 逐对象绘制属性表 SSBO 子范围槽号（批 T1，shader 端 binding 26；同形态）。 */
	[[nodiscard]] uint32_t objectDrawSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneObjectDrawSlot;
	}

	/** 逐对象簇区间表 SSBO 子范围槽号（批 T1，shader 端 binding 33；同形态）。 */
	[[nodiscard]] uint32_t objectClusterRangeSlot() {
		ensure();  // 幂等取号（同 wetSlot）
		return sceneObjectClusterRangeSlot;
	}

   private:
	/** 区槽总数（含全区 scene.data 本身）= ensure 领槽数 = writeRegionDescriptors 行数。
	 *  🔴 每迁一区 +1 时**只改这一处**：四张 std::array 都以它取尺寸，漏写描述符行会
	 *  编译失败（初始化列表元素多于 N），而不是留到运行期读脏槽。 */
	static constexpr size_t kRegionSlotCount = 12;

	/**
	 * 全部区槽描述符一次写（ensure 领槽后调用；实现见文件下方）。
	 * 🔴 领槽与写描述符是同一份知识的两半——分成两处写会出
	 * 「加了子范围槽、忘了写描述符」的隐蔽形态（描述符没被写过 = 读到 heap 槽里的上一个
	 * 描述符，VVL 未必出声，画面上是随机垃圾）。两半都以本类的槽号成员为准。
	 */
	void writeRegionDescriptors();

	RGL::Resources& kernel;  // 资源内核引用（领 allocator/heap/槽）
	RGL::DeviceContext& device;  // 设备层引用（writeResourceDescriptorsEXT）
	RGL::VmaBuffer sceneDataBuffer;  // 设备端 scene data buffer（BDA + eTransferDst；未创建 = 空句柄）
	// 批 N+1：原 `sceneDataSlot`（全区 binding 3 的 heap 槽）已删——全区读取面归零后
	// 没有任何 mapping 指向它。buffer 本体保留（下面 12 个子范围描述符全部指进它的各区）。
	uint32_t sceneHeaderSlot = 0;  // 头区 UBO 子范围槽号（迁移批 S-5，binding 22）
	uint32_t sceneWetSlot = 0;   // 湿面区 UBO 子范围槽号（迁移批 1，binding 32）
	uint32_t sceneIrradianceSlot = 0;  // 辐照度区 UBO 子范围槽号（迁移批 2，binding 27）
	uint32_t sceneLightArraySlot = 0;  // 灯数组区 UBO 子范围槽号（迁移批 3，binding 24）
	uint32_t sceneShadowViewProjSlot = 0;  // 阴影 VP 区 UBO 子范围槽号（迁移批 S-1，binding 28）
	uint32_t sceneShadowParamsSlot = 0;  // 阴影采样参数区 UBO 子范围槽号（迁移批 S-2，binding 29）
	uint32_t sceneSsrSlot = 0;  // SSR 区 UBO 子范围槽号（迁移批 S-3，binding 31）
	uint32_t sceneTaaSlot = 0;  // TAA 区 UBO 子范围槽号（迁移批 S-4，binding 30）
	uint32_t sceneObjectMatrixSlot = 0;  // 对象矩阵表 SSBO 子范围槽号（批 T1，binding 23）
	uint32_t sceneObjectMvpSlot = 0;  // MVP 表 SSBO 子范围槽号（批 T1，binding 25）
	uint32_t sceneObjectDrawSlot = 0;  // 绘制属性表 SSBO 子范围槽号（批 T1，binding 26）
	uint32_t sceneObjectClusterRangeSlot = 0;  // 簇区间表 SSBO 子范围槽号（批 T1，binding 33）
	bool sceneDataReady = false;  // 就绪标记（ensure 幂等守卫）
};

/**
 * scene data 对齐闸门（迁移批 1 引入，批 2/3 沿用；从 ensure() 拆出控复杂度）。
 * 注：必须是类的静态成员而非文件级 static 函数——本模块是接口单元，inline 成员函数
 * 引用 TU-local 实体会触发 clang -WTU-local-entity-exposure（-Werror）。
 */
void SceneDataService::checkAlignmentGate() const {
// §9.2/§3.1 运行期对齐闸门（形态先例 = checkMeshletDeviceFit：查询 → 核对 → 点名，
// 读数常驻日志要能抄）。判据三条：
//   ① 子范围描述符 address 必须是 minUniformBufferOffsetAlignment（UBO）/
//      minStorageBufferOffsetAlignment（SSBO）的整数倍（VUID-VkResourceDescriptorInfoEXT-
//      type-12350/12351）；各区基址恒为 kSceneRegionAlign 的倍数 ⟹ 判据 = kSceneRegionAlign
//      同时是两者的整数倍（两者皆 2 的幂，非倍数 = 该设备 256 台阶挂不上去）。
//   ② buffer 基址本身 256 对齐（VmaAllocationCreateInfo::minAlignment 已钉，这里实测
//      复核——VMA 给不了对齐是失败还是静默降级取决于版本，不赌）。
//   ③ 将来走 UBO 载体的各区（§9.5：SceneHeader/LightArray/Irradiance/ShadowVP/ShadowParams/
//      Taa/Ssr/Wet）尺寸 ≤ maxUniformBufferRange；大表走 SSBO 不受此限。
	
const auto limits = device.getPhysicalDevice().getProperties().limits;
const uint64_t minUboAlign = limits.minUniformBufferOffsetAlignment;
const uint64_t minSboAlign = limits.minStorageBufferOffsetAlignment;
const uint32_t maxUboRange = limits.maxUniformBufferRange;
if (auto* logger = RLLogger::getInstance())
	logger->info("scene data 对齐闸门读数：minUniformBufferOffsetAlignment={} "
				 "minStorageBufferOffsetAlignment={} maxUniformBufferRange={}（kSceneRegionAlign={}）",
		minUboAlign, minSboAlign, maxUboRange, kSceneRegionAlign);
constexpr uint32_t kLargestUboRegionBytes = std::max({kSceneDataHeaderBytes,
	kSceneDataLightArrayBytes, kSceneDataIrradianceBytes, kSceneDataShadowMatrixBytes,
	kSceneDataShadowParamsBytes, kSceneDataTaaBytes, kSceneDataSsrBytes, kSceneDataWetBytes});
if (minUboAlign == 0 || kSceneRegionAlign % minUboAlign != 0)
	throw std::runtime_error(fmt::format(
		"scene data 对齐闸门：minUniformBufferOffsetAlignment={} 不整除 kSceneRegionAlign={} —— "
		"子范围 UBO 描述符（VUID-12350）挂不上去", minUboAlign, kSceneRegionAlign));
if (minSboAlign == 0 || kSceneRegionAlign % minSboAlign != 0)
	throw std::runtime_error(fmt::format(
		"scene data 对齐闸门：minStorageBufferOffsetAlignment={} 不整除 kSceneRegionAlign={} —— "
		"子范围 SSBO 描述符（VUID-12351）挂不上去", minSboAlign, kSceneRegionAlign));
if (kLargestUboRegionBytes > maxUboRange)
	throw std::runtime_error(fmt::format(
		"scene data 对齐闸门：最大 UBO 候选区 {}B 超 maxUniformBufferRange={} —— "
		"该区载体需改走 SSBO（§9.5）", kLargestUboRegionBytes, maxUboRange));
}

/**
 * 幂等契约见 SceneDataService::ensure 注释。实现：buffer usage =
 * eShaderDeviceAddress | eStorageBuffer | eTransferDst，VMA_MEMORY_USAGE_AUTO
 * device-local（CPU 不直接写——每帧唯一写入方 = staging copy，勿用 createGpuBuffer 的
 * CPU_TO_GPU+MAPPED 形态）+ VmaAllocationCreateInfo::minAlignment = kSceneRegionAlign
 *（§9.7 基址对齐）；创建前跑运行期对齐闸门（§3.1，不满足 = throw 点名）；每迁一区领一
 * 「子范围」槽（类型化载体口径与 binding 逐行写在 writeRegionDescriptors，数量 =
 * kRegionSlotCount；批 N+1 起**不再领全区 "scene.data" 槽**——binding 3 的读取面已归零）；
 * 全部区槽一次 writeResourceDescriptorsEXT。
 */
inline void SceneDataService::ensure() {
	if (sceneDataReady)
		return;
	// 对齐闸门（判据与读数注释见文件级 checkSceneDataAlignmentGate）。
	checkAlignmentGate();


	vk::BufferCreateInfo info{};
	info.size = kSceneDataBytes;
	info.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
		vk::BufferUsageFlagBits::eTransferDst;  // eUniformBuffer（迁移批 1）：湿面区子范围
	// UBO 描述符要求 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT（VUID-VkResourceDescriptorInfoEXT-
	// type-11461，§9.2 事实基线），同 buffer 双 usage 合法
	VmaAllocationCreateInfo alloc{};
	// device-local：CPU 不直接写（每帧唯一写入方 = staging copy），勿用 createGpuBuffer 的
	// CPU_TO_GPU+MAPPED 形态
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	// §9.7：基址钉 256 对齐（子范围描述符 address = 基址 + 区基址，两侧都要 256 台阶）。
	// 字段名注：VMA 3.4 起叫 minAlignment（3.0–3.3 名 allocationAlignment，计划文本用旧名）
	alloc.minAlignment = kSceneRegionAlign;
	auto bufResult = kernel.getAllocator().createBufferWithAddress(device.getDevice(), info, alloc, nullptr,
		"scene_data/storage");
	if (!bufResult)
		throw std::runtime_error(bufResult.error().describe());
	sceneDataBuffer = std::move(*bufResult);
	// 闸门②：基址实测复核（见上）
	if (const uint64_t base = sceneDataBuffer.getDeviceAddress(); base % kSceneRegionAlign != 0)
		throw std::runtime_error(fmt::format(
			"scene data 对齐闸门：buffer 基址 {:#x} 不是 kSceneRegionAlign={} 的整数倍"
			"（minAlignment 未生效？）", base, kSceneRegionAlign));

	// 每迁一区 +1 槽（§9.2「拆描述符不拆 buffer」，运行时段容量 1024 用不完）。
	// 领槽与写描述符是同一份知识的两半 ⟹ 描述符统一交给 writeRegionDescriptors()。
	auto allocRuntimeSlot = [&](const char* name) -> uint32_t {
		auto slot = kernel.allocSlot(RGL::SlotSegment::Runtime, name);
		if (!slot)
			throw std::runtime_error(fmt::format(
				"scene data 领槽 \"{}\" 失败：{}", name, slot.error().describe()));
		return *slot;
	};
	// 批 N+1：全区 "scene.data" 槽（旧 binding 3）已删——scene.slang 的 `sceneData` 声明
	// 连同它唯一的用途（全区字节读）一起退役，槽号再无去处。buffer 本体保留。
	sceneHeaderSlot = allocRuntimeSlot("scene.header");  // binding 22：头区（迁移批 S-5）
	sceneWetSlot = allocRuntimeSlot("scene.wet");  // binding 32：湿面区（迁移批 1）
	sceneIrradianceSlot = allocRuntimeSlot("scene.irradiance");  // binding 27（迁移批 2）
	sceneLightArraySlot = allocRuntimeSlot("scene.lights");  // binding 24（迁移批 3）
	// 迁移批 S-1：阴影光空间 VP 区（UBO 子范围，binding 28）
	sceneShadowViewProjSlot = allocRuntimeSlot("scene.shadow_vp");  // binding 28
	// 迁移批 S-2：阴影采样参数区（UBO 子范围，binding 29）
	sceneShadowParamsSlot = allocRuntimeSlot("scene.shadow_params");  // binding 29
	// 迁移批 S-3：SSR 区（UBO 子范围，binding 31）
	sceneSsrSlot = allocRuntimeSlot("scene.ssr");  // binding 31
	// 迁移批 S-4：TAA 区（UBO 子范围，binding 30）
	sceneTaaSlot = allocRuntimeSlot("scene.taa");  // binding 30
	// 批 T1：逐对象四张表（世界矩阵 / MVP / 绘制属性 / 簇区间）各一 SSBO 子范围槽
	sceneObjectMatrixSlot = allocRuntimeSlot("scene.object_matrices");  // binding 23
	sceneObjectMvpSlot = allocRuntimeSlot("scene.object_mvps");  // binding 25
	sceneObjectDrawSlot = allocRuntimeSlot("scene.object_draws");  // binding 26
	sceneObjectClusterRangeSlot = allocRuntimeSlot("scene.object_cluster_ranges");  // binding 33

	writeRegionDescriptors();
	sceneDataReady = true;
}

/**
 * 全部区槽描述符一次 writeResourceDescriptorsEXT（领槽方 = ensure，见其 allocRuntimeSlot）。
 * 载体口径：批 T1 的四张表 = `eStorageBuffer`（shader 端 StructuredBuffer）；头区 / 湿面 /
 * 辐照度 / 灯数组 / 阴影 VP / 阴影参数 / SSR / TAA = `eUniformBuffer`（shader 端
 * ConstantBuffer，迁移批 1/2/3/S-1/S-2/S-3/S-4/S-5）。🔴 批 N+1 起**没有「全区」那一行**：
 * 旧首行 `{base, kSceneDataBytes}` 的 eStorageBuffer（= shader 端 `ByteAddressBuffer
 * sceneData` 的 binding 3）随读取面归零一起删除，buffer 本体保留、只被下面 12 条子范围
 * 描述符指入。子范围 address 对齐 = 基址 256（minAlignment）+
 * 区基址 256 台阶（本文件顶部布局红线）⟹ 恒过 VUID-12350（UBO）/ VUID-12351（SSBO）；
 * 闸门读数见 checkAlignmentGate。
 * 🔴 四张表的下发长度**含尾部哨兵**（`kSceneData*Bytes` = (容量+1)×步长，批 T0）：
 * 越界索引因此落在描述符范围内、落在 poison 值上，而不是落在 buffer 外（后者是
 * 未定义行为，VVL 报的是"越界"而不是"越界读到哨兵"这条我们想要的信号）。
 */
inline void SceneDataService::writeRegionDescriptors() {
	const uint64_t base = sceneDataBuffer.getDeviceAddress();
	// 顺序 = ensure 领槽顺序；逐槽地址在下面按槽号现取（不靠下标配对，见其注释）
	// 🔴 行数必须与 kRegionSlotCount 一致（漏一行 = 该槽描述符从未被写过 = 读到 heap 槽里
	// 的上一个描述符，VVL 未必出声）——四张表都以本常量取尺寸，加区时只改它一处。
	std::array<vk::DeviceAddressRangeEXT, kRegionSlotCount> ranges{{
		{base + kSceneDataHeaderOffset, sizeof(SceneHeaderBlock)},  // scene.header — binding 22
		{base + kSceneDataWetOffset, sizeof(WetParamsBlock)},  // scene.wet — binding 32
		{base + kSceneDataIrradianceOffset, sizeof(IrradianceBlock)},  // — binding 27
		{base + kSceneDataLightArrayOffset, sizeof(LightArrayBlock)},  // — binding 24
		{base + kSceneDataShadowMatrixOffset, sizeof(ShadowViewProjBlock)},  // — binding 28
		{base + kSceneDataShadowParamsOffset, sizeof(ShadowParamsBlock)},  // — binding 29
		{base + kSceneDataSsrOffset, sizeof(SsrParamsBlock)},  // — binding 31
		{base + kSceneDataTaaOffset, sizeof(TaaParamsBlock)},  // — binding 30
		{base + kSceneDataObjectMatrixOffset, kSceneDataObjectMatrixBytes},  // — binding 23
		{base + kSceneDataMvpTableOffset, kSceneDataMvpTableBytes},  // — binding 25
		{base + kSceneDataDrawTableOffset, kSceneDataDrawTableBytes},  // — binding 26
		{base + kSceneDataClusterRangeOffset, kSceneDataClusterRangeBytes}}};  // — binding 33
	std::array<vk::DescriptorType, kRegionSlotCount> types{{
		vk::DescriptorType::eUniformBuffer,  // 头区
		vk::DescriptorType::eUniformBuffer,  // 湿面
		vk::DescriptorType::eUniformBuffer,  // 辐照度
		vk::DescriptorType::eUniformBuffer,  // 灯数组
		vk::DescriptorType::eUniformBuffer,  // 阴影光空间 VP
		vk::DescriptorType::eUniformBuffer,  // 阴影采样参数
		vk::DescriptorType::eUniformBuffer,  // SSR
		vk::DescriptorType::eUniformBuffer,  // TAA
		vk::DescriptorType::eStorageBuffer,  // 对象世界矩阵表
		vk::DescriptorType::eStorageBuffer,  // 对象 MVP 表
		vk::DescriptorType::eStorageBuffer,  // 对象绘制属性表
		vk::DescriptorType::eStorageBuffer}};  // 对象簇区间表
	std::array<uint32_t, kRegionSlotCount> slots{{sceneHeaderSlot, sceneWetSlot,
		sceneIrradianceSlot, sceneLightArraySlot, sceneShadowViewProjSlot, sceneShadowParamsSlot,
		sceneSsrSlot, sceneTaaSlot, sceneObjectMatrixSlot,
		sceneObjectMvpSlot, sceneObjectDrawSlot, sceneObjectClusterRangeSlot}};
	std::array<vk::ResourceDescriptorInfoEXT, kRegionSlotCount> descs{};
	std::array<vk::HostAddressRangeEXT, kRegionSlotCount> dsts{};
	const vk::DeviceSize heapStride = kernel.getDescriptorHeap().getDescriptorSize();
	for (size_t i = 0; i < ranges.size(); ++i) {
		descs[i].type = types[i];
		descs[i].data.pAddressRange = &ranges[i];  // ranges 存活到本次写完成，合法
		auto addr = kernel.getDescriptorHeap().getSlotCpuAddress(slots[i]);
		if (!addr)
			throw std::runtime_error(addr.error().describe());
		dsts[i] = vk::HostAddressRangeEXT{*addr, heapStride};
	}
	device.getDevice().writeResourceDescriptorsEXT(descs, dsts);
}

}  // namespace RGL
