module;
#include <glm/glm.hpp>  // RenderListState 成员 std::vector<glm::mat4> 需 glm::mat4 名可见
#include <entt/entt.hpp>
export module render_list;

import rlstd;
import material_types_generated;  // Phase 4：统一 DrawArgs 超集（Phase 4 时 scalar 100B，Phase 7b+ 起 4B = 仅 objectIndex）
import scene_forward_types_generated;  // 前向头 SceneLight + ObjectDraw（Phase 10 批 10.2a 单源化）
import frame_draw_list;
import mdi_rows;    // ★M-1b：MdiShadowRow/MdiShadowSegment（采集上下文的存储类型；rlbase 纯 CPU）
import vk_resources;
import environment;  // Environment（irradianceSH，架构债务续账步骤 3）
import asset_table;
import scene_context;
import scene_data;    // kActiveShadowCascades（阴影段清单的级数；阴影 draw 构造接缝入参）
import meshlet_lod;   // MeshletShadowLodTable（★18-B2 阴影远级联 LOD 选级表，collectShadowCasters 入参）
import meshlet_ranges;  // ClusterRangeTable（债-105 起 collectMeshletOverlay 抬为接缝，接口侧要这个入参类型）
import shadow_proxy_types;  // ShadowProxyTable（N10 P3 钉段：collectShadowCasters 入参，rlbase 纯 CPU）
import runtime_config;  // SsrTuning / WetTuning（批 15.1 参数面 + 批 15.2A 湿面参数：系数唯一默认值载体，fill*Params 入参）
import camera;

namespace RGL {

// renderlist system（ViewProjection + 场景实体 + 场景旋转 → FrameDrawList）拆出为独立
// 模块（2026-09-02）：ecs 帧循环骨架与最大块的 system 解耦——场景数据流
// （Transform/MeshRef/MaterialRef/材质资产/透明排序/光照接线/天空盒）集中一处。

/**
 * 每帧 push constant 存储（帧数据：活过 renderSystem 循环，DrawCall 的 span 指向其中；
 * vkCmdPushDataEXT 录制时读取，只要求内存活到录制结束）。
 * Phase 4 归一（H1 解决）：单一统一 DrawArgs 超集（material_types_generated，Phase 4 时
 * scalar 布局 100B；Phase 7b+ bindless 化后 4B 仅剩 objectIndex，见 args 注释）
 * ——删旧 cube 单例(64B) + model 每资产(96B，Phase 5 前) 双容器与 AssetKind 分支。
 * Phase 6 instancing：args/matrices 按 **objectSlot**（MeshRef.objectSlot，实例级——同
 * asset 多实例各持独立变换；容量 = NextObjectSlot.next，main 启动期契约检查
 * ≤ kSceneMatrixCapacity）索引，**非**资产 id。
 */
export struct RenderListState {
	std::vector<RGL::DrawArgs> args;  // 每 objectSlot 一份（Phase 7b+ 起 4B = 仅 objectIndex；实例级索引）
	// 对象矩阵表（Phase 5 批次 2，bindless）：objectSlot 索引（实例级，Phase 6
	// instancing）的 spin·world，DrawArgs.objectIndex
	// 进 shader 索引本表（世界空间光照）。帧局部存储（FrameDrawList.sceneData 持 span，
	// 录制期拷贝进 staging，生命周期规则同 args）
	std::vector<glm::mat4> matrices;
	// Phase 7b+（NV DGC 规避）：per-objectSlot MVP 表（scene data 尾区上传，
	// 填值 = args[slot].mvp 同一份，位级一致；shader readObjectMvp 消费）
	std::vector<glm::mat4> mvps;
	// Phase 7b+（NV DGC 规避）：per-objectSlot 材质/几何表（scene data 尾区上传，
	// shader readObjectDraw 消费；DrawArgs 仅剩 objectIndex）
	std::vector<ObjectDraw> draws;
	// Phase 10 批 10.7：per-objectSlot 材质 id 表（**纯 CPU 侧，不进 scene data**）——
	// 高亮 pass 按材质过滤 draw（选中材质 = 该材质全部实体一起亮，非单实例）。不进
	// GPU 契约是刻意的：避免为「哪个物体被选中」新增 per-object 字节字段、触发
	// ObjectDraw 布局变更与 just reflect 重生成。帧局部存储，生命周期同 draws。
	std::vector<uint32_t> materialBySlot;
	// Phase 5.5 批次 4 天空盒 draw 的 push data（skyboxArgs）已随 forward 绘制机制删除
	// （2026-10-18 刀②.2）。
	// Phase 11 批 11.2：阴影段 push data（ShadowDrawArgs 8B，索引 = 分块号 × objectSlot）。
	// 每帧只写一次且值不随时间变（objectIndex 永丸、cascadeIndex 永丸）——
	// 故只 resize 不重填：resize 保留已有内容（对同一场景的相邻帧不会丢值）。
	// 但仍保留“每帧重填”的可读性：下方渲染时明确赋 cascadeIndex，不依赖初始值。
	std::vector<RGL::ShadowDrawArgs> shadowArgs;
	// 簇化轴 ★18-B2：阴影段**簇路径** push data（ShadowMeshletDrawArgs 12B，索引 = 分块号 ×
	// objectSlot，与 shadowArgs 同一分块公式）。c1..c3 切簇路径后的 draw 指向它；🔴 与
	// shadowArgs 同一条 **resize-before-loop** 红线——实体循环内 realloc 会让先前存入
	// DrawCall 的 span 悬空（帧 0 确定性 UAF，见 renderListSystem 里那条原话注释）。
	std::vector<RGL::ShadowMeshletDrawArgs> shadowMeshletArgs;
	// N10 P3 钉段 draw 的 12B push（🔴 与上面**分开两块**：同一 (级联, objectSlot) 可能有
	// 「常规选级段」+「钉段」两条 draw，共用 idx 会互相覆盖 clusterStart）。分块公式与
	// shadowArgs 同一条，同样受 resize-before-loop 红线约束（→ resizeFrameStorage）。
	std::vector<RGL::ShadowMeshletDrawArgs> shadowMeshletPinnedArgs;
	// ── ★M-1b 真 MDI 阴影段（三件，全部帧局部，span 生命周期规则同 pushData）──
	// MDI 级联的 push 存储（**每级联 1 槽**，下标 = 分块号；内容是 {rowBase, cascade, 0}，
	// rowBase = 载体同源 pair 的 rowBase）。resize-before-loop：循环前固定 kActiveShadowCascades
	// 槽，emit 内不再 realloc（同 shadowArgs 那条 UAF 红线）。
	std::vector<RGL::ShadowMeshletDrawArgs> shadowMeshletMdiArgs;
	// 行表存储：各级联段**首尾相接**的 24B 行（FrameDrawList::mdiSegments 的 rows span 指这里）。
	std::vector<RGL::MdiShadowRow> mdiRows;
	// 各级联段描述（emit 产，renderListSystem 搬进 FrameDrawList::mdiSegments）。
	std::vector<RGL::MdiShadowSegment> mdiSegments;
	// Phase 7 批次 4：CPU frustum culling 可见统计（renderListSystem 每帧填；可见/剔除计数）
	uint32_t visibleCount = 0;
	uint32_t culledCount = 0;
};

/**
 * ViewProjection + 场景实体 + 场景旋转 → FrameDrawList（registry ctx）context（单一 draw
 * 路径）。
 *
 * Phase 4 归一：统一 DrawArgs 超集（所有 shader 风格共享同一布局，material.slang 单一真相源，
 * Phase 4 时 scalar 布局 100B；Phase 7b+ bindless 化后 push 只剩 objectIndex 4B）——单一写入
 * 路径，零 AssetKind 分支（旧 cube 64B / model 96B
 * 双 struct 分支删除，ecs_compliance_review H1 注记解决）。各风格经 objectIndex 进 scene data
 * 表取自己要的字段（cube 取 mvp 每顶点色；model 取 mvp/totalTriangles/vertexOffset/baseColor）。
 * groupCount 经资产静态字段（MeshAsset.groupCount，上传期按 shader 契约派生，D6）：
 * model=ceil(tri/kTrianglesPerGroup)、cube=1 固定单 group、0=不渲染——每帧零推导零分支。
 * vertexOffset 经资产静态字段（MeshAsset.vertexOffset，批次 2 拼接顶点 buffer per-primitive
 * 偏移，上传期写入）；roughnessMetallic（批次 3 pbr 材质）写材质资产值（pbr frag 消费）。
 * 画不画 = 实体有无（数据驱动）。
 *
 * Phase 5 透明排序（roadmap 决策：排序在 renderSystem 内做，不单独建 system）：
 * 经 MaterialRef → MaterialAsset.alphaModeBlend（glTF alphaMode == "BLEND"）分桶——
 * 不透明段保持实体迭代序（深度写正确），透明段按相机距离² 降序（back-to-front）稳定排序，
 * 产出 [opaque | transparent 远→近] 单列表 + transparentStart 边界
 *（drawFrame 段首各 applyDynamicState 一次，见 vkrenderer.cppm；原 skybox 段与
 * skyboxStart 边界已随 forward 绘制机制删除，2026-10-18 刀②.2）。MASK 材质留在不透明段
 *（Phase 8 C1：alpha-test discard 在 shader 内，cutoff 经 ObjectDraw.emissive.w；doubleSided
 * 的 cull 切换见 C2）；排序距离 = primitive 实体 world 平移列
 * 到相机位置的 distance2（无包围盒时的标准近似，legacy RenderQueue 同款思路）。
 *
 * Phase 7 批次 4 CPU frustum culling：视锥 6 面从 proj·view 提取（每帧一次），逐 entity
 * node-local 剔除球（CullSphere 组件）经 modelMatrix（spin·world）变换到世界后测——
 * 保守：跨界判可见，绝不误剔；可见/剔除计数写 state（仅计数变化时打日志）。
 *
 * 灯 entity 化（Phase 7，ECS 规范 3）：灯 = entity（Light 组件，画不画 = 实体有无），
 * 收集按 Light.index 落槽（不依赖 view 迭代序）：primary = index 0（写 scene data 头 @0
 * 单灯），延迟灯数组按 index 填（≤ kMaxLights）。
 *
 * @param rg registry（读 ViewProjection/SceneSpin/Camera/SceneEnv/NextObjectSlot，
 *   遍历 Transform+MeshRef+MaterialRef+CullSphere 实体 view + Light 组件 view，写 ctx FrameDrawList）
 * @param resources 资源层（资产表：mesh/material 记录解析）
 * @param env 环境服务（irradianceSH 每帧拷入 scene data，架构债务续账步骤 3）
 * @param assets 资产表（mesh/material 记录解析；F 拆出不再经内核取）
 * @param state 每帧 push constant 存储（args/matrices 填充 + culling 统计）
 * @param frameSlot ★M-1b：本帧**录制槽**号（= `FrameSession::currentFrameSlot()`，由调用它的
 *   帧循环在 drawFrame **之前**取——slot 只在 drawFrame 末尾 advance ⟹ 采集与录制同值）。
 *   仅 MDI 生效档消费（同源 pair 的一半输入）；门关时传什么都不影响画面。
 */
export void renderListSystem(entt::registry& rg, RGL::Resources& resources,
	RGL::Environment& envService, const RGL::AssetTable& assets, RenderListState& state,
	uint32_t frameSlot);

/**
 * 每帧填阴影 scene data（尾区阴影 VP 区 + 4 个 vec4 采样参数子区）。定义在
 * render_list_impl（partition impl unit，实现侧不写 export）。导出为**可测接缝**
 * （批 12.3d）：阴影总开关经 shadowPassEnabled 显式入参，测试无需单例即可打 enabled
 * 两态；调用点（renderListSystem）传形态决策表 `ShadowFormPlan::drawAtlas`。
 * 契约红线（未生效槽 splits 不得为 0 / texel 分母 = 单块边长 / params.w = shadowFar）
 * 随实现体注释，回归断言 = tests/test_render_list_shadow。
 * ⚠ 软影两区（shadowPcss / shadowEvssm）的调参**在实现内读 shadowConfig() 的两块**
 * （按档分立，批 N6b / 债-16⑤），不走本接缝的入参——入参承载"draw 开关"与"强制选级诊断键"
 * 两件事：这两枚的判据都是**写进 scene data 的字节**，无单例直打比 setShadowConfig 再读回
 * 更省一轮，也免掉测试改全局态（软影调参是"实现内读单例"的历史形态，别顺手把本键也并进去）。
 * @param out 出参：本帧 draw list（写 sceneData.shadowViewProj / Splits / TexelSizes / Params / Atlas）
 * @param shadowNear/shadowFar **量化后**的切分深度界（与 renderListSystem 建切片同源——
 *   splits 阈值与级联内容错位 = 边界处阴影闪跳）
 * @param volumes 各级光空间体（VP + XY 半宽；与剔除用同一批，先建体再同时用于剔除与矩阵）
 * @param shadowPassEnabled 阴影总开关（调用点 = 形态决策表 drawAtlas；false 只影响
 *   shadowParams.z，其余字段照常填写）
 * @param forceCascade **诊断键**（`csm.force_cascade`）：>= 0 把全场景钉到该级采样，只覆写
 *   写进 scene data 的 splits（→ forceShadowCascadeSplits）。🔴 **级联几何一律不受影响**——
 *   入参 shadowNear/shadowFar 与 volumes 是真实几何，本键改的只是"片元被允许落在哪一级"，
 *   否则测的就不是"同一套级联几何下换采样源"。-1 = 关：函数原样返回 splits ⟹ **写进
 *   scene data 的字节逐位不变**（本仓「门关逐位不变」红线，锚 = just shot 出厂图 sha256）
 */
export void fillShadowData(FrameDrawList& out, float shadowNear, float shadowFar,
	std::span<const ShadowVolume> volumes, bool shadowPassEnabled, int forceCascade);

/**
 * 级联包围球（4 级，与写进 scene data 的 texel 尺寸同一套几何）。定义在 render_list_impl
 * （partition impl unit，实现侧不写 export）。
 *
 * 导出为**接缝**（N10 P2）：启动期的「texel 门在 8 档 × 4 级联下的活跃条目数」读数表
 * （`main:meshlet_tables` 的 shadow proxy INFO）必须与运行期吃**同一套**级联几何，否则那张表
 * 读的是另一个世界的数——`quantizeShadowDistance` / `cascadeSplitDistance` /
 * `frustumBoundingSphere` 三段组合各写一遍就是三个可漂点（抬出先例 = `meshletTaskCount`，
 * 因同款理由导出）。🔴 运行期唯一消费者仍是 `renderListSystem`，本批零行为变化。
 * @param viewProj 相机（**只用 proj 里的 fov/aspect**：视锥 8 角共球、半径与相机姿态无关，
 *        见 `frustumBoundingSphere` 闭式解那条注释 ⟹ 启动期可以喂规范机位）
 * @param shadowNear/shadowFar **量化后**的切分深度界（与 `fillShadowData` 同源）
 */
export std::array<Bounds, kActiveShadowCascades> computeCascadeSpheres(
	const ViewProjection& viewProj, float shadowNear, float shadowFar);

/**
 * 阴影投影体候选（★18-B2 抬为可测接缝的契约类型；实体遍历内先攒，循环结束后才定光空间体）。
 *
 * 为何要两段：光空间盒的 Z 下界依赖**全部投影体**的 AABB，而剔除要用这个盒——顺序上必须
 * 先知道盒才能判"谁在盒内"。候选表只存标量，1591 个 primitive 也只有几十 KB。
 * 簇路径三字段（★18-B2）：`meshletPipeline == kInvalidPipelineIndex` = **本图元不切**
 * （c0 恒旧路径与本字段无关；c1..c3 只在它有效时才走簇管线）。
 */
export struct ShadowCandidate {
	glm::vec3 center;     // 世界球心
	float radius;         // 世界球半径
	uint32_t objectSlot;  // = MeshRef.objectSlot（push.objectIndex）
	uint32_t pipeline;    // = asset.shadowPipelineIndex（旧 mesh 管线，c0 恒用、回落也用）
	uint32_t groups;      // = asset.groupCount（旧管线的 task 数）
	// ── 簇路径（★18-B2 远级联 LOD）：发射条件四条 → collectShadowCasters ──
	uint32_t meshletPipeline = kInvalidPipelineIndex;  ///< = asset.shadowMeshletPipelineIndex（无效 = 不切）
	/** ★M-1b：真 MDI 条目下标（= asset.shadowMeshletMdiPipelineIndex）。kInvalid = 本帧没有
	 *  MDI 宿主（门关未烘 / 载体未建）⟹ emitShadowCasters 对可切候选继续走逐条簇 draw。 */
	uint32_t meshletMdiPipeline = kInvalidPipelineIndex;
	/** 🔴 **选级簇数**（LOD 表 bySlot[slot].count），**不是** task 数——×2 换算唯一真相源 =
	 *  render_list_impl 的 meshletTaskCount（emitShadowCasters 消费点执行）。 */
	uint32_t meshletGroups = 0;
	uint32_t clusterStart = 0;  ///< 相对 ObjectClusterRange::base 的偏移（原样进 12B push）
	// ── N10 P3 阴影代理**钉段**（`ShadowProxyTable` 的搬运，四字段全 0 = 本候选没有钉段）──
	// 🔴 表不存在（门关）= 装配层根本不建 = 这里全 0 = **一条 draw 都不多发**（"存在即生效"，
	// 与上面 meshletPipeline 的 kInvalid 同构）。判据本体不在这里（`pinActive`，rlbase 纯函数），
	// 本结构只搬**事实**：产物局部半径 + 世界换算，不搬"该不该钉"的结论（结论逐级联变）。
	uint32_t pinnedStart = 0;      ///< 相对 ObjectClusterRange::base（与 clusterStart 同口径）
	uint32_t pinnedCount = 0;      ///< 被钉簇数（0 = 无钉段；≥1 才可能多发一条 c1..c3 的 draw）
	float pinnedRadius = 0.0f;     ///< 被钉簇段的包围球半径（**产物局部单位**，未乘 worldScale）
	float pinnedWorldScale = 0.0f;  ///< 产物局部单位 → 世界米（实体矩阵基向量长度，见 collect 侧）
};

/**
 * 收集阴影投影体候选（★18-B2 抬为可测接缝，fillShadowData 12.3d 先例：接口声明 export、
 * impl partition 定义、测试直打）。发射条件与簇路径四条判据 → 定义处注释；阴影总开关
 * （形态决策表 drawAtlas）仍在实现内读 shadowConfig() 单例（测试经 setShadowConfig 两态直打，
 * 先例 = tests/test_render_list_shadow 的按档分立用例）。
 * @param lodTable 阴影远级联 LOD 选级表（registry ctx；nullptr = 门关 = 候选全部不可切）
 * @param proxyTable N10 P3 阴影代理钉段表（registry ctx；nullptr = 门关 = 钉段四字段全 0）。
 *        🔴 **不新建收集函数**（二值形态候选集不变：谁投影不变，变的只是每个投影发几条），
 *        本函数是 ShadowCandidate 的唯一产地，钉段字段在这里填。
 * @param worldScale 实体矩阵的世界换算（产物局部单位 → 米）。🔴 调用点传的是它**已经算好的**
 *        那个 scale（同一个值也在算候选的世界包围半径），不在这里重算一遍（第二处公式迟早漂，
 *        口径 → scene_context 的 worldScaleOf）。
 */
export void collectShadowCasters(const RGL::MeshAsset& asset, const RGL::MaterialAsset& mat,
	const glm::vec3& worldCenter, float worldRadius, uint32_t objectSlot,
	const MeshletShadowLodTable* lodTable, const ShadowProxyTable* proxyTable, float worldScale,
	std::vector<ShadowCandidate>& out);

/**
 * 一枚级联的纹素尺寸（世界米）= `2·光空间体 XY 半宽 / 图集单块边长`。定义在 render_list_impl
 * （partition impl unit，实现侧不写 export）。
 *
 * 导出为**接缝**（N10 P3 收口 P2 遗留）：这条式子原本写了两遍（`fillShadowData` 写进 scene data
 * 的 `shadowTexelSizes` 与 P2 启动期读数表），P3 的 emit 门是第三个消费者——三处各写一遍 =
 * 「采样端看到的纹素」与「CPU 判钉段用的纹素」可以静默分家，而分家的表现是"影子该钉的没钉"，
 * 画面上看不出来。分母取**单块边长**而不是图集整幅（5a-6 那条：按 2048 网格 = 吸到半纹素位置）。
 * 🔴 三处消费者：`fillShadowData` / `emitShadowCasters`（钉段 texel 门）/ `main:shadow_proxy`。
 */
export float shadowTexelSize(float xyRadius);

/**
 * ★M-1b 真 MDI 采集上下文（docs/mdi_plan.md §19.2 D4 / §19.5）。非空 = 生效档事实
 * （ctx 里有 `MeshletMdiShadowActivation`，装配层判定一次，本层不重述条件——
 * `collectMeshletOverlay` 判据 5/6 同纪律）。三个数/两个口的来源各有红线：
 *  · `sliceRows` = 载体的 `sliceRows()`（装配层从 ring 读入——**别自己凑**，容量口径的
 *    唯一真相源是 `wireMeshletShadowArgs` 的闭式锚）；
 *  · `frameSlot` = `FrameSession::currentFrameSlot()`（ecs 帧循环在 drawFrame **之前**取，
 *    slot 只在 drawFrame 末尾 advance ⟹ 采集期与录制期同值；失配有机器核对，见执行器）；
 *  · `slices` = 载体的 `slices()`（帧槽轮转分母；**采集期**就查 `frameSlot < slices`——
 *    ring×2 的写安全前提是"只写本帧槽"，不留到录制期才现形，★M-1b 雷 3 处置）；
 *  · `rows` / `segments` = `RenderListState` 的帧局部存储（span 生命周期规则同 pushData，
 *    容量闸门在 `buildMdiShadowRows` 的调用点判，越界段整帧回落逐条 draw）。
 */
export struct MdiShadowEmitContext {
	uint32_t sliceRows = 0;
	uint32_t slices = 0;
	uint32_t frameSlot = 0;
	std::vector<MdiShadowRow>* rows = nullptr;
	std::vector<MdiShadowSegment>* segments = nullptr;
	/** MDI push 存储（固定 kActiveShadowCascades 槽、循环前 resize——同 shadowMeshletArgs 的
	 *  resize-before-loop 红线；下标 = 分块号）。内容 = {rowBase, cascadeIndex, 0}。 */
	std::vector<ShadowMeshletDrawArgs>* pushArgs = nullptr;
};

/**
 * ★M-1b：MDI 阴影段的**生效**载体（registry ctx，存在即生效，同 `MeshletSwitchOn` 一族的
 * 「装配层判定一次、消费者只搬事实」纪律）。emplace 者 = 装配层 bake 步骤（条目烘成 +
 * 行表载体已建 + binding 34 已烘），消费者 = `renderListSystem`。
 * 只带采集侧需要的闭式数，不带服务指针（render_list 不 import 设备层服务 = 接缝可无设备单测）。
 */
export struct MeshletMdiShadowActivation {
	uint32_t sliceRows = 0;  ///< 载体每帧槽行数（= ring->sliceRows()，容量闸门输入）
	uint32_t slices = 0;     ///< 帧槽数（= ring->slices() = FrameSession::kFramesInFlight，雷 3 采集期预检输入）
};

/**
 * 候选 × 逐级光空间体 → 阴影 draw 清单（★18-B2 抬为可测接缝，同上先例）。级主序、级内
 * 实体迭代序；**draw 条数与切不切无关**（原位换管线形状：只换 draw 内容，不增减）。
 * c0 恒旧路径（8B push）；c1..c3 且候选可切 → 簇管线 + meshletTaskCount(簇数) + 12B push。
 * @param shadowMeshletArgs 出参：填 ShadowMeshletDrawArgs（帧局部存储，生命周期同 pushData；
 *        调用方**必须在实体循环之前** resize 到位——循环内 realloc = 先前 span 悬空）
 * @param shadowMeshletPinnedArgs 出参：N10 P3 钉段 draw 的 12B push（🔴 **必须是另一块存储**：
 *        同一 (级联, objectSlot) 现在可能有两条 draw，共用 `idx` 会让第二条把第一条的
 *        clusterStart 覆盖掉 = 常规段静默画成钉段。分块公式与上面同一条，同样循环前 resize）
 * 🔴 ★M-1b `mdi` 非空时的形态（生效档，§19.2 D4 孔洞流）：c1..c3 每级联的行表 =
 * **全部可切候选**（含本级不可见的，行照发、`gcX = 0`）按候选序喂
 * `buildMdiShadowRows`（不许自己拼行），产出一条 `MdiShadowSegment` + 一条
 * `DrawCall{pipeline=MDI 条目, pushData={rowBase, cascadeIndex, 0}, drawCount=行数,
 * argsOffset=同源 pair}`；不可切候选继续逐条旧 draw（**混合形态**，别塞进行表）。
 * 行序 = 候选序 = 今天的 draw 相对序（§19.1 逐位不变的前提）；MDI 条目在 out[c] 里的
 * 位置 = 本级联**首个可见可切候选**的位置（该级联无一可见时挂段尾——depth-only 图集的
 * 逐纹素 min 与 draw 序无关，§19.1 第三行；行**序**契约在行表内部仍成立）。
 * **压紧/重排 = 立刻让判据失去依据**。
 * 容量不足（3 段总行数 > sliceRows）/ 构建器不变量破 = WARNING + 本帧整体回落逐条
 * draw（不静默半切）；`mdi->segments` 保持空 ⟹ 执行器零新命令。
 */
export void emitShadowCasters(std::span<const ShadowCandidate> cands,
	std::span<const ShadowVolume> volumes, uint32_t objectSlotCount,
	std::vector<ShadowDrawArgs>& shadowArgs,
	std::vector<ShadowMeshletDrawArgs>& shadowMeshletArgs,
	std::vector<ShadowMeshletDrawArgs>& shadowMeshletPinnedArgs,
	std::array<std::vector<DrawCall>, kActiveShadowCascades>& out,
	const MdiShadowEmitContext* mdi = nullptr);

/**
 * 簇路径单条 draw 的 task 数（★1-a1ii-β-1 抬出 = **单一真相源**；定义在 render_list_impl，
 * 实现侧不写 export）。导出为**可测接缝**（债-105 与 `collectMeshletOverlay` 同批抬出：
 * 叠加段的 ×2 口径要在无 GPU 的单测里钉「同一入口」，此前它藏在 impl 的匿名 namespace 里，
 * `tests/test_shadow_lod_emit.cppm` 只能写字面 2 并把这登记成已知缺口）。
 * 恒 L0 档下每簇固定 **2 个任务槽**（去索引发射把每任务三角数压到 85，而 L0 簇中位 121 三角、
 * 71.2% 的簇 > 85）；槽数取反射生成的规格常量 `kSlotsPerCluster`，不写字面量 2。
 * 🔴 **三个消费者必须同式**：α 影子叠加段（`collectMeshletOverlay`）、β-1 原位取代段
 * （`makeOpaqueCall`）、★18-B2 阴影段簇路径（`emitShadowCasters`，入参 = LOD 选级簇数）。
 * @param clusterCount 本 draw 要画的簇数（主段 = L0 簇数；阴影段 = LOD 选级簇数；> 0，调用方守）
 */
export uint32_t meshletTaskCount(uint32_t clusterCount);

/**
 * 簇化轴 ★1-a1ii-α：簇路径**影子叠加** draw 采集（实体遍历内调用，视锥剔除之后——叠加与本体
 * 同进同退）。定义在 render_list_impl（partition impl unit，实现侧不写 export）。导出为**可测
 * 接缝**（`collectShadowCasters` 同款先例）：债-105 给它加了第五枚条件（叠加段**自己的**门），
 * 而这块逻辑的全部判据只在这个纯函数里成立才有意义——回归断言 = `tests/test_meshlet_overlay`。
 *
 * 六条判据（任一不满足 = 零 draw）：
 *  1. 资产有叠加管线条目（门关 / 无产物 / 多产物回落 = `kInvalidPipelineIndex`）；
 *  2. 材质非 blend（透明段按距离排序，叠加不参与——画进独立向量会破坏其排序语义）；
 *  3. registry ctx 有簇区间表（门关 = 装配层根本不建）且不越界（表比实例矩阵槽短 = 不采）；
 *  4. 本 objectSlot 的 L0 簇数 > 0（无簇 = 零 group，连空 draw 都不发）；
 *  5. 🔴 **`meshlet.mode = "overlay"` 档生效**（债-105 新增，`overlayActive` 入参）——叠加段是 ★1-a1ii-α
 *     的验证脚手架（同批几何重画一遍、共面 + `eLess` 一像素不写），出厂默认不再连带它；
 *  6. β-1 原位取代**未**生效（`meshletInPlace` 入参）——同一批几何不能既原位取代又再叠加画第二遍。
 *
 * 🔴 判据 5/6 是**入参**而不是写在 `renderListSystem` 的调用点上，理由 = 条件集中在唯一可单测
 * 的纯函数里；调用点只负责把 ctx 的**生效事实**搬进来（`MeshletOverlayOn` / `MeshletSwitchOn`），
 * 不重述任何条件。
 * @param asset/mat 本实体资产与材质（判据 1 / 2）
 * @param table 簇区间表（nullptr = 门关 = 判据 3）
 * @param objectSlot = MeshRef.objectSlot（表下标 = shader objectIndex）
 * @param overlayActive 判据 5（ctx 有没有 `MeshletOverlayOn`）
 * @param meshletInPlace 判据 6（ctx 有没有 `MeshletSwitchOn`，与 `makeOpaqueCall` 同源）
 * @param pushData 与本体 draw 同源的 push 字节（帧局部存储，规则同 DrawCall::pushData）
 * @param out 叠加段收集向量（实体迭代序 = 确定性）
 */
export void collectMeshletOverlay(const RGL::MeshAsset& asset, const RGL::MaterialAsset& mat,
	const ClusterRangeTable* table, uint32_t objectSlot, bool overlayActive, bool meshletInPlace,
	std::span<const std::byte> pushData, std::vector<RGL::DrawCall>& out);

/**
 * 每帧填灯数据（scene data 头单灯 + 尾区灯数组），并返回主灯。定义在
 * render_list_impl（partition impl unit，实现侧不写 export）。导出为**可测接缝**
 * （批 12.3d）：测试栈上 registry 直打落槽契约（按 Light.index 落槽 = 不依赖 view
 * 迭代序；index ≥ kMaxLights 不落槽但计数），回归断言 = tests/test_render_list_light。
 * @param rg 引擎（读 Light 组件）
 * @param eyePos 相机世界位置（头单灯 eyePos 字段）
 * @param out 出参：本帧 draw list（写 sceneData.light / lightCount / lightArray）
 * @return 主灯（Light.index == 0；无灯 entity 时 = 默认 Light{}）——阴影矩阵与
 *         ambient 也要它（主灯 = 页面主光源）
 */
export Light fillLightData(entt::registry& rg, const glm::vec3& eyePos, FrameDrawList& out);

/**
 * 每帧填 SSR 正向矩阵（scene data 尾区 kSceneDataSsrOffset = 本帧**抖动后** proj·view，
 * 与 @64 skyInvViewProj 严格互逆；口径 → scene_data.cppm 该区注释）。定义在
 * render_list_impl（partition impl unit，实现侧不写 export）。导出为**可测接缝**
 * （12.3d fillShadowData 同款先例）：口径契约「取 viewProj.proj（注入抖动那一份）而非
 * projUnjittered」必须两态可直打才有判据，回归断言 = tests/test_ssr_viewproj。
 * @param out      出参：本帧 draw list（写 sceneData.ssrViewProj）
 * @param viewProj 本帧实际投影上下文（proj = 注入抖动那一份，taaSystem 之后）
 */
export void fillSsrViewProj(FrameDrawList& out, const ViewProjection& viewProj);

/**
 * 每帧填 SSR 尾区的**两个参数子区**（scene data @723776 系数 vec4 + @723792 门控 vec4；
 * 口径 → scene_data.cppm 各区注释）。定义在 render_list_impl（partition impl unit，实现侧
 * 不写 export）。导出为**可测接缝**（fillSsrViewProj 同款先例）：回归断言 =
 * tests/test_ssr_params（两源结构：写入侧 C++ 常量、读回侧字面量 723776/723792）；
 * 上游配置往返（lua 文本 → loadRuntimeConfig）另测于 tests/test_scene_config。
 *
 * **一次填两槽**是刻意的：两槽永远同源同帧（同一个 SsrTuning 派生），拆成两个接缝 =
 * 允许出现"系数是新的、门控是旧的"这种半帧状态。
 *
 * @param out    出参：本帧 draw list（写 sceneData.ssrParams / sceneData.ssrGate）
 * @param bounds 本帧场景聚合包围球（boundsSystem 产物，空场景 radius = 0 原样透传——
 *               兜底住在消费端 shader，CPU 不做第二真相源）
 * @param tuning 本帧 SSR 系数（registry ctx，启动期由 RuntimeConfig 拷入；CPU 侧**不**
 *               clamp、不校验不变式，理由 → readSsrBlock 注释）
 */
export void fillSsrParams(FrameDrawList& out, const SceneBounds& bounds,
	const SsrTuning& tuning);

/**
 * 每帧填湿面尾区的**两个参数子区**（scene data @723968 身份调制系数 vec4 + @723984
 * 空间变化 vec4；口径 → scene_data.cppm 各区注释）。定义在 render_list_impl（partition
 * impl unit，实现侧不写 export）。导出为**可测接缝**（fillSsrParams 同款先例）：回归断言 =
 * tests/test_wet_params（两源结构：写入侧 C++ 常量、读回侧字面量 723968/723984）；
 * 上游配置往返（lua 文本 → loadRuntimeConfig）另测于 tests/test_scene_config。
 *
 * **一次填两槽**是刻意的：两槽永远同源同帧（同一个 WetTuning 派生），拆成两个接缝 =
 * 允许出现"系数是新的、空间参数是旧的"这种半帧状态。
 *
 * @param out    出参：本帧 draw list（写 sceneData.wetParams / sceneData.wetSpatial）
 * @param tuning 本帧湿面参数（registry ctx，启动期由 RuntimeConfig 拷入；CPU 侧**不**
 *               clamp、不校验不变式——`strength = 0` 是合法消融档，理由 → readWetBlock 注释）
 */
export void fillWetParams(FrameDrawList& out, const WetTuning& tuning);

}  // namespace RGL
