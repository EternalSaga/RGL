module;
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <ranges>
module render_list:impl;

import render_list;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import material_types_generated;
import lights_types_generated;
import scene_forward_types_generated;  // 前向头 SceneLight（C API 反射生成，替换原 LightDataGpu）
import frame_draw_list;
import scene_data;  // kShadowMapSize（阴影图边长；单向依赖）
import vk_resources;
import environment;  // Environment（irradianceSH，架构债务续账步骤 3）
import asset_table;
import scene_context;
import camera;
import render_graph;  // shadowAtlasColumns（图集列数，与装饰 extent 同源）
import evssm_shadow;  // kEvssmMipLevels（矩链级数；scene data w 从常量导出，禁双源写死）
import shadow_config;  // shadowConfig() / shadowFormPlan()（阴影形态：画不画图集 + 逐档调参）
import runtime_config;  // SsrTuning / WetTuning（批 15.1 参数面 + 批 15.2A 湿面参数，fill*Params 入参）
import taa_jitter;    // contentShiftPx（内容位移口径唯一出处；批 13.5 起 fillTaaData 要用）
import meshlet_ranges;  // ClusterRangeTable（★1-a1i 簇区间表的 registry ctx，装配层写、本层只搬 span）

namespace RGL {

namespace {

/** 待落段 draw（段装配前的暂存项）：call = 定稿 draw；distanceSq = 排序距离
 *  （透明段 back-to-front 稳定排序键，距离² 免 sqrt，单调等价）。抬到文件作用域
 *  （而非函数内局部）——阴影投影体采集辅助函数要用它。 */
struct Pending {
	RGL::DrawCall call;
	float distanceSq = 0.0f;
};

/** 阴影投影体候选：实体遍历内先攒，循环结束后才定光空间体。
 *
 * 为何要两段：光空间盒的 Z 下界依赖**全部投影体**的 AABB，而剔除要用这个盒 —— 顺序上
 * 必须先知道盒才能判"谁在盒内"。若在实体循环里边走边剔（像原来那样），盒就还没成形。
 * 候选表只存 5 个标量，1591 个 primitive 也只有几十 KB。
 */
struct ShadowCandidate {
	glm::vec3 center;     // 世界球心
	float radius;         // 世界球半径
	uint32_t objectSlot;  // = MeshRef.objectSlot（push.objectIndex）
	uint32_t pipeline;    // = asset.shadowPipelineIndex
	uint32_t groups;      // = asset.groupCount
};

/**
 * 收集阴影投影体候选（实体遍历内调用，只判材质/门控，不做级联剔除）。
 * 只收**不透明**材质：blend 体投出的影子会看不到遮挡体的背面（收下来只多花一遍光流，
 * 还会在透明体上留不可见的脊影）；MASK 背面本就被 alpha 消掉，不影响。
 * 不过 GPU culling：候选表按**主视棱**建立，用它会漏掉视野外屋顶的投影体。
 * 门控（形态决策表 `drawAtlas`）在**最前**：关时不产出任何 draw——图里的阴影 pass 仍然运行
 * （清深度），但无光栅化开销，且采样端因 enabled=0 直接返回全亮。
 * @param asset 该实体的网格资产（取 shadowPipelineIndex / groupCount）
 * @param mat 该实体绑定的材质（判 blend）
 * @param worldCenter 图元世界球心
 * @param worldRadius 图元世界球半径
 * @param objectSlot 该实体的 per-object 槽号
 * @param out 候选表（追加）
 */
void collectShadowCasters(const RGL::MeshAsset& asset, const RGL::MaterialAsset& mat,
    const glm::vec3& worldCenter, float worldRadius, uint32_t objectSlot,
    std::vector<ShadowCandidate>& out) {
    if (!shadowFormPlan(shadowConfig()).drawAtlas
        || asset.shadowPipelineIndex == kInvalidPipelineIndex || mat.alphaModeBlend)
        return;
    out.push_back(ShadowCandidate{worldCenter, worldRadius, objectSlot,
        asset.shadowPipelineIndex, asset.groupCount});
}

/**
 * 簇化轴 ★1-a1ii-α：簇路径**影子叠加** draw 采集（实体遍历内调用，视锥剔除之后——
 * 叠加与本体同进同退）。四个发射条件（任一不满足 = 零 draw）：
 *  1. 资产有叠加管线条目（门关 / 无产物 / 多产物回落 = kInvalidPipelineIndex）；
 *  2. 材质非 blend（透明段按距离排序，叠加不参与——画进独立向量会破坏其排序语义）；
 *  3. registry ctx 有簇区间表（门关 = 装配层根本不建，meshlet_ranges::ClusterRangeTable）；
 *  4. 本 objectSlot 的 L0 簇数 > 0（无簇 = 零 group，连空 draw 都不发）。
 * groupCount = l0Count（恒 L0 档，docs/mdi_plan.md §10.1：L0 恒为区间前缀）；pushData
 * 与本体 draw 同一个 DrawArgs（同 objectIndex ⟹ shader 读同一套材质/MVP 表项）。
 * @param asset/mat 本实体资产与材质（判据 1/2）
 * @param table 簇区间表（nullptr = 门关）
 * @param objectSlot = MeshRef.objectSlot（表下标 = shader objectIndex）
 * @param pushData 与本体 draw 同源的 push 字节（帧局部存储，规则同 DrawCall::pushData）
 * @param out 叠加段收集向量（实体迭代序 = 确定性）
 */
void collectMeshletOverlay(const RGL::MeshAsset& asset, const RGL::MaterialAsset& mat,
    const ClusterRangeTable* table, uint32_t objectSlot,
    std::span<const std::byte> pushData, std::vector<RGL::DrawCall>& out) {
    if (asset.meshletPipelineIndex == kInvalidPipelineIndex || mat.alphaModeBlend
        || table == nullptr || objectSlot >= table->bySlot.size())
        return;
    const uint32_t l0Count = table->bySlot[objectSlot].l0Count;
    if (l0Count == 0)
        return;
    out.push_back(RGL::DrawCall{asset.meshletPipelineIndex, pushData, l0Count});
}

/**
 * 候选 × 逐级光空间体 → 阴影 draw 清单（级主序，级内实体迭代序）。
 * 一个图元可以落多级（大物体跨越多个切片）——这正是逐级剔除能把总量压在单级之上的原因。
 * @param cands 投影体候选表
 * @param volumes 各级光空间体（判定用；XY 收紧、Z 覆盖全部投影体）
 * @param objectSlotCount shadowArgs 的分块 stride（分块号 × objectSlotCount + slot）
 * @param shadowArgs 出参：填 ShadowDrawArgs（帧局部存储，生命周期同 DrawCall.pushData）
 * @param out 出参：逐级 draw 清单
 */
void emitShadowCasters(std::span<const ShadowCandidate> cands,
    std::span<const ShadowVolume> volumes, uint32_t objectSlotCount,
    std::vector<RGL::ShadowDrawArgs>& shadowArgs,
    std::array<std::vector<Pending>, kActiveShadowCascades>& out) {
    for (const auto& cd : cands)
        for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
            if (!casterInShadowVolume(volumes[c], cd.center, cd.radius))
                continue;
            auto& sargs = shadowArgs[static_cast<size_t>(c) * objectSlotCount + cd.objectSlot];
            sargs.objectIndex = cd.objectSlot;
            sargs.cascadeIndex = c;
            out[c].push_back(Pending{RGL::DrawCall{cd.pipeline,
                std::as_bytes(std::span<const RGL::ShadowDrawArgs>{&sargs, 1}), cd.groups}, 0.0f});
        }
}

/**
 * 建逐级光空间体并逐级剔除出阴影 draw（返回体供 fillShadowData 写 scene data）。
 * 抽出来的原因：renderListSystem 的代码行与认知复杂度已顶到阈值，而这块自成一体。
 *
 * Z 界取自**全部候选的世界 AABB** —— 这正是"盒要装得下投影体"落到代码的地方。漏了它
 * 就会退回只按切片球建盒，楼房整批被剔、没人往阴影图里画（实测阴影开关只差 0.016%
 * 像素，等于没开）。无候选时 AABB 退化成一个点：盒只剩受光面那层，但也没有任何 draw
 * 要画，等价于无阴影。
 * @param light 主灯（光空间基）
 * @param slices 各级子视锥包围球（定 XY 半宽）
 * @param cands 投影体候选
 * @param objectSlotCount shadowArgs 分块 stride
 * @param shadowArgs 出参：填 ShadowDrawArgs
 * @param out 出参：逐级 draw 清单
 * @return 各级光空间体
 */
std::array<ShadowVolume, kActiveShadowCascades> buildShadowVolumes(const Light& light,
    const std::array<Bounds, kActiveShadowCascades>& slices,
    std::span<const ShadowCandidate> cands, uint32_t objectSlotCount,
    std::vector<RGL::ShadowDrawArgs>& shadowArgs,
    std::array<std::vector<Pending>, kActiveShadowCascades>& out) {
	glm::vec3 casterMin{0.0f}, casterMax{0.0f};
	for (const auto& cd : cands) {
		casterMin = glm::min(casterMin, cd.center - cd.radius);
		casterMax = glm::max(casterMax, cd.center + cd.radius);
	}
	std::array<ShadowVolume, kActiveShadowCascades> volumes{};
	// snap 格 = **实际块边长**（不是图集整幅）：4 级时块 1024，按 2048 网格吸附 = 吸附到
	// 半纹素位置，PCF 抽头随相机跨纹素边界 → 影缘抖动（5a-6 排查时埋的雷，游动修复一并拆）。
	const uint32_t snapGrid = shadowTileSize(shadowAtlasColumns(kActiveShadowCascades));
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		volumes[c] = buildShadowVolume(light, slices[c], casterMin, casterMax, snapGrid);
	emitShadowCasters(cands, volumes, objectSlotCount, shadowArgs, out);
	return volumes;
}

/**
 * 级联包围球（自 renderListSystem 抽出 = stylecheck 函数行数预算，纯搬移零行为变化）。
 * 级 0 的近平面用**固定小值**而非量化 near：自适应 near（下限实测 0.05）量化后可能
 * 抬高到 0.125，视深落在 [0.05, 0.125) 的近处片元（选级 = c0）会掉出子视锥 → 出块
 * 全亮旁路 → 脚下没影子。固定 0.02 恒低于任何自适应值，且姿态无关（不引入游动）。
 * nearC/farC **直接传进** frustumBoundingSphere，不从子 proj 反解（2026-09-30 批 12.1
 * 单测抓出）：float32 RH_ZO 透视矩阵在 near/far ≳1e3 时就编不准 far，反解误差被比值
 * 线性放大到超过拟合球的保守余量 → 球装不下子视锥。见 scene_context 4 参重载注释。
 */
std::array<Bounds, kActiveShadowCascades> computeCascadeSpheres(const ViewProjection& viewProj,
	float shadowNear, float shadowFar) {
	static constexpr float kShadowNearFloor = 0.02f;
	std::array<Bounds, kActiveShadowCascades> spheres{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const float nearC = c == 0 ? kShadowNearFloor
			: cascadeSplitDistance(shadowNear, shadowFar, c - 1u, kActiveShadowCascades);
		const float farC = cascadeSplitDistance(shadowNear, shadowFar, c, kActiveShadowCascades);
		spheres[c] = frustumBoundingSphere(viewProj.view,
			projWithDepthRange(viewProj.proj, nearC, farC), nearC, farC);
	}
	return spheres;
}

}  // namespace

/**
 * 定义（接口声明与 @param 契约见 render_list.cppm；12.3d 接缝 = shadowPassEnabled
 * 显式入参取代内部单例读取，调用点传形态决策表的 `drawAtlas`）。
 */
void fillShadowData(FrameDrawList& out, float shadowNear, float shadowFar,
    std::span<const ShadowVolume> volumes, bool shadowPassEnabled) {
    // 抽出函数体的原因同 collectShadowCasters / fillLightData：函数代码行阈值按函数
    // extent 计算（内嵌 lambda 也计入），不拆就过阈。
    for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
        out.sceneData.shadowViewProj[c] = volumes[c].viewProj;
    // splits = 每级远截止（采样端拿它选级；末级写 1e9 = “没有更远的级”，下级比较不会被越过）
    // **必须用与 CPU 切片同源的量化 n/f**（renderListSystem 传入）——阈值与内容错位 =
    // 片元选到不覆盖自己的级 → 边界处阴影随相机闪跳。
    // texelSizes = 2·r/块边长，每级各自从自己的球推。
    // params = (depthBias, normalOffsetScale, enabled, shadowFar)；enabled 由形态决策表
    // 驱动（12.3d 起经 shadowPassEnabled 显式传入），shadowFar 供采样端做末级淡出
    // （5a-9，见下方填写处）。
    // atlas = (图集边长, 单块边长, 分块列数, 保留)，采样端把分块 uv 叠图集用。
    const float kNoFar = 1e9f;
    const uint32_t cols = shadowAtlasColumns(kActiveShadowCascades);
    // **未生效的槽也必须写 kNoFar，不能留 0**：采样端选级是三段无条件比较
    // （distSq > splits[i]² 则进第 i+1 级），而 distSq 对任何真实片元都 > 0 —— 留 0 会被
    // 判成"已越过这一级的分界"，把片元一路推到最后一个槽。生效 1 级时若 splits =
    // {1e9, 0, 0, 0}，全场景都会落到 3 号槽，而它的光空间 VP 从未被写过（零矩阵）
    // → clip.w = 0 → 0/0 = NaN → 阴影因子恒 0（实测：整个广场全黑，与机位无关）。
    // 这里让"没生效的级"分界恒为无穷远 = 永远选不中，shader 侧无需知道生效级数。
    glm::vec4 splits{kNoFar, kNoFar, kNoFar, kNoFar};
    glm::vec4 texels{0.0f};
    for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
        splits[c] = c + 1u < kActiveShadowCascades
            ? cascadeSplitDistance(shadowNear, shadowFar, c, kActiveShadowCascades)
            : kNoFar;
        texels[c] = volumes[c].xyRadius * 2.0f / static_cast<float>(shadowTileSize(cols));
    }
    out.sceneData.shadowSplits = splits;
    out.sceneData.shadowTexelSizes = texels;
    // params = (depthBias, normalOffsetScale, enabled, shadowFar)。**w 从"预留 0"改为
    // shadowFar**（5a-9）：采样端用它做末级距离淡出（shadow.slang ⑦，Filament 同款），
    // 消掉"超出级联覆盖 → 出块旁路全亮"的硬边。传的是**量化后**的远界，与 splits/切片同源。
    out.sceneData.shadowParams = {kShadowDepthBias, kShadowNormalOffsetScale,
        shadowPassEnabled ? 1.0f : 0.0f, shadowFar};
    // 列数与图装饰的 extent 同源（shadowAtlasColumns）——两处不同步 = 采样端把块叠到错位置
    out.sceneData.shadowAtlas = {static_cast<float>(kShadowMapSize),
        static_cast<float>(shadowTileSize(cols)), static_cast<float>(cols), 0.0f};
    // ── 软影两区的调参（批 N6b 起**按档分立**，消债-16⑤）────────────────────────
    // 两个区都**照旧无条件写**（与改造前同形）：PCSS 变体只读 pcss 区、EVSSM 变体只读
    // evssm 区，各自看不到对方的值 ⟹ 改一档不串到另一档。改造前两个区共用一套
    // `pcss_*`，"EVSSM 的半影上限"只能借 PCSS 的键名写（docs/shadow_routes_plan.md §2 红线 3）。
    // 出厂 configs/shadow.lua 两块同值（0.53/16/32）⟹ 交付形态下写出的字节与改造前逐位相同。
    // 半影全宽系数取 2·tan(θ/2)（θ = 太阳视直径；小角下 ≈ tanθ，物理定义用半角式）。
    const auto degToHalfRad = [](double degrees) {
        return static_cast<float>(degrees) * 0.01745329252f * 0.5f;
    };
    const ShadowTuning& pcss = shadowConfig().pcss;
    out.sceneData.shadowPcss = {2.0f * std::tan(degToHalfRad(pcss.sunDegrees)),
        static_cast<float>(pcss.searchRadius), static_cast<float>(pcss.maxPenumbra), 0.0f};
    // 批 11.6 EVSSM 参数（只有 -DEVSSM_SOFT 变体读）：x = 半坡 tan(θ/2)（与 pcss 同形状但
    // 各读各的块，EVSSM 消费半坡而非全宽系数）；y/z = 搜索半径/半影上限（图集纹素 mip0 域）；
    // w = 矩链最大 mip（EvssmShadow 8 级链固定 7，docs/evssm_plan.md §3.1）。
    const ShadowTuning& evssm = shadowConfig().evssm;
    const float evssmHalfAngle = degToHalfRad(evssm.sunDegrees);
    out.sceneData.shadowEvssm = {std::tan(evssmHalfAngle),
        static_cast<float>(evssm.searchRadius), static_cast<float>(evssm.maxPenumbra),
        static_cast<float>(RGL::kEvssmMipLevels - 1u)};
    // 核宽下限（批 N6d）走常量区 .z（原留白）：它是**滤波形态参数**而不是逐帧几何，与指数 c
    // 同区同源。0 = 不设下限（= 单纹素比较形态）。⚠ .y（722068）曾为一致性闸门容差，实测
    // 净负作用已删，槽位恒 0 不动布局 —— 理由见 shadow.slang STEP 2.5 权衡表。
    out.sceneData.shadowEvssmConst
        = {kEvssmExponent, 0.0f, static_cast<float>(evssm.minFootprintTexels), 0.0f};
}

/**
 * 定义（接口声明与 @param 契约见 render_list.cppm；12.3d 起导出为接缝，测试栈上
 * registry 直打，不必经完整 renderListSystem）。
 */
Light fillLightData(entt::registry& rg, const glm::vec3& eyePos, FrameDrawList& out) {
	// 灯 entity 化（Phase 7，ECS 规范 3）：灯 = entity（Light 组件），画不画 = 实体有无。
	// 收集**不依赖 view 迭代序**（无保证）——按 Light.index 显式落槽：primary = index 0
	//（forward @0 头单灯，diff0）；延迟灯数组按 index 填（≤ kMaxLights，尾区
	// @kSceneDataLightArrayOffset，契约 lights.slang）。lightEntityCount 每帧算（非启动期常量）
	// = 留“运行时增删灯”接缝。
	std::array<Light, kMaxLights> lightSlot{};
	Light primary{};  // 无灯时 = 默认 Light{}（同旧 empty() 分支）
	uint32_t lightEntityCount = 0;
	rg.view<Light>().each([&](const Light& L) {
		++lightEntityCount;
		if (L.index == 0)
			primary = L;
		if (L.index < kMaxLights)
			lightSlot[L.index] = L;
	});
	const uint32_t nLights = std::min(lightEntityCount, kMaxLights);
	out.sceneData.light = SceneLight{glm::normalize(primary.direction), eyePos,
		primary.color * primary.intensity, primary.ambient};
	out.sceneData.lightCount = nLights;
	for (uint32_t i = 0; i < nLights; ++i) {
		const Light& L = lightSlot[i];
		GpuLight& G = out.sceneData.lightArray[i];
		G.direction = glm::vec4(glm::normalize(L.direction), 0.0f);
		G.position = glm::vec4(L.position, 0.0f);
		G.color = glm::vec4(L.color * L.intensity, 0.0f);
		G.params = glm::vec4(L.range, static_cast<float>(L.type), 0.0f, 0.0f);
	}
	return primary;
}


/** materialBySlot 哨兵 = 该 objectSlot 本帧无实体（批 10.7 高亮过滤表；被剔对象不在
 *  draw list 里故读不到，留哨兵避免读到上一帧陈值）。 */
constexpr uint32_t kNoMaterial = 0xFFFFFFFFu;

// renderListSystem 定义（2026-09-02 编译提速 A 方案）：帧函数体移出接口 TU，
// BMI 只含声明 + RenderListState；本单元是叶子边。

/** 实现流水（语义/契约见接口声明）：cull 分桶（opaque/transparent，Pending 暂存）
 * → 可见统计 → 段装配（opaque → skybox 单 draw → transparent 稳定排序）
 * → scene data 填充 → 写 ctx。 */
/** 批 14.5：填 FSR 的相机输入（执行器侧唯一消费者 = `Renderer::fsrTonemapPassPre`）。
 *
 * near/far 取 `ViewProjection`，**不是** `Camera` 原值：`projectionSystem` 按场景包围球做
 * 自适应 near/far 并把实际生效值写回 ViewProjection（ecs_impl.cppm:349-363），取配置原值
 * 会让 FSR 按错的平面线性化深度。fov 由 proj 矩阵反推（2·atan(1/proj[1][1])，
 * glm::perspective 恒等式）——读的就是本帧真正用了的那个投影，不存在第二套真相源。
 *
 * @param out      本帧绘制列表（写 fsrNear / fsrFar / fsrFovVertical）
 * @param viewProj 本帧实际投影上下文 */
void fillFsrCameraInput(FrameDrawList& out, const ViewProjection& viewProj) {
	out.fsrNear = viewProj.nearPlane;
	out.fsrFar = viewProj.farPlane;
	out.fsrFovVertical = 2.0f * std::atan(1.0f / viewProj.proj[1][1]);
	// 批 14.5 第四批：抖动**取的就是注入投影的那一份**（同一个 ViewProjection 字段，不是
	// 从配置重算）⟹ 「色输入带的抖动」与「申报给 FSR 的抖动」结构上不可能漂移。
	// 门关（TAA 关 + FSR 抖动关）时 taaSystem 不调 applyProjectionJitter ⇒ 本值恒 0。
	out.fsrJitterPx = viewProj.jitterPx;
}

/**
 * 批 12.6 TAA 的 scene data 载荷（尾区 @722080 / @722144 / @722208，生产端在此、
 * 消费端 = taa.frag.slang；字节布局与「两侧口径」见 scene_data.cppm kSceneDataTaa* 注释）。
 *
 * 一句话版：`prevViewProj` = 上一帧**抖动后** proj·view（产出**抖动网格**坐标，
 * 由 taa.frag 减 `prevContentShift` 落回基空间历史网格）；
 * `motion` = prevSpin·inverse(curSpin)
 * （全场景同一旋转 ⇒ 一个 mat4 精确补偿物体运动，spin 关 = 单位阵）；
 * `params` = (feedback, historyValid, **本帧内容位移 c_t** x/y px)，historyValid=0 时
 * taa.frag 直接输出本帧（已 un-jitter 那份）= **逐位旁路**。
 * `skyInvViewProj`（上一行填）与本帧的 prevViewProj 都是**抖动后**口径，只是分别属于
 * 「本帧」与「上一帧」——重建世界坐标的半边是它，投回上一帧的半边是 prevViewProj。
 */

void fillTaaData(FrameDrawList& out, const ViewProjection& viewProj, const SceneEnv& env,
	const TaaState& taa) {
	// 内容位移口径的唯一出处 = `taa_jitter.cppm::contentShiftPx`（逐轴符号推导 + 实测背书
	// 全在那儿；此处**不再重复推导**，重复的推导必然漂移成两个版本）。
	// 门 `taa.base_space` 关 ⇒ 写 0 ⇒ shader 的 `baseUv` 逐位 = `input.uv`（见下方门注释）。
	const glm::vec2 contentShift
		= env.taaBaseSpace ? contentShiftPx(viewProj.jitterPx) : glm::vec2{0.0f};
	out.sceneData.taaPrevViewProj = taa.payloadPrevViewProj;
	out.sceneData.taaMotion = taa.payloadMotion;
	// zw = **本帧内容位移 c_t**。⚠ 批 13.5 起 taa.frag 真读它做输入侧 un-jitter；
	// 此前这里写的是 jitterPx 而 shader 从不读 = 死载荷（口径差一个 y 轴翻号，读了就错）。
	// x = **本帧权重**（= env.taaFeedback，imgui 滑杆写的就是它；批 13.14b 删掉静态机位
	// 权重提升后没有中间镜像，直接取 env）。⚠ 方向红线：taa.frag: lerp(clipped, cur, x)
	// ⟹ **调小 = 更信历史**，别搞反。
	out.sceneData.taaParams = {env.taaFeedback, taa.historyValid ? 1.0f : 0.0f,
		contentShift.x, contentShift.y};
	// ── 批 13.5 基空间累积（门 `taa.base_space`，**语义已改义**）──
	// 旧义（批 13.2）：在 tonemap 加 `+c_t` 采样偏移把**输出**搬回基空间（只修显示那一半）。
	// 新义（批 13.5）：在 **taa pass 的输入侧**做 un-jitter + 历史 c_{t−1} 修正 ⟹ **累积**
	//   发生在基空间、历史不再每帧被亚像素重采样。tonemap 侧的偏移已删（留着 = 双重 un-jitter）。
	// 门判定**只在这一处**（shader 侧无门逻辑）：门关 ⇒ 两个载荷全写 0 ⇒ `baseUv` 逐位 =
	// `input.uv`、`prevUvBase` 逐位 = `prevUv`（IEEE：`x + 0 = x`、`x − 0 = x`，折成 FMA 亦
	// 成立）⟹ 门关态 = **改前的 `base_space = false` 形态**（输出在抖动空间 + 历史每帧重采样），
	// 是一个**合法的 A/B 对照点**：门开/门关之差 = 本次架构改造的净效果（同构建可测）。
	// 刻意**不**与 historyValid 关联：首帧/跳切那几帧 taa_out 的内容同样已 un-jitter
	// （shader 早退只是「不混历史」，不是「不搬基空间」），下一帧拿它当历史时同样要减
	// c_{t−1}；若此时写 0，同一场画面会一半在基空间一半在抖动空间。
	// TAA 关 / jitter_scale=0 ⇒ jitterPx = 0 ⇒ 门开门关逐位相同（门红线自动成立）。
	const glm::vec2 histShift = env.taaBaseSpace ? taa.payloadPrevContentShift : glm::vec2{0.0f};
	// z = **AO 已由 taa pass 施加**（批 13.6）：tonemap 见 1 就跳过自己的 AO 乘法，见 0 照旧施加。
	// 取 `env.aoAppliedPreTemporal`（= decorateTaaPass **实际**结果），**不是** `env.taaEnabled`（配置键）
	// ——offscreen 变体配置开了 TAA 也装饰失败，用配置派生就会「taa pass 不存在 + tonemap 又
	// 跳过」= AO 一次都不施加。两个分支位精确（lerp(x,1,1)≡1.0 / lerp(x,1,0)≡x）
	// ⟹ TAA 关时 tonemap 逐位不变（门红线）。本函数**无条件调用**，故门关态也保证写对 0。
	// w = **历史拒收诊断门标志**（`taa.diag_reject`，默认 0 = 今天）：1 ⟹ taa.frag 把逐像素
	// 拒收码写进 taa_out.a（RGB 一个 bit 不动；tonemap 与历史采样都只读 rgb ⟹ 交付图不变）。
	// 本槽位原为保留 0，复用 = **零布局变更**（static_assert 不动）。
	out.sceneData.taaPrevContentShift = {histShift, env.aoAppliedPreTemporal ? 1.0f : 0.0f,
		env.taaDiagReject ? 1.0f : 0.0f};
	// x = **输入侧 un-jitter 的重建核选择**（`taa.input_kernel`，批 13.8；0 = 双线性 = 默认）。
	// 本函数**无条件写**（门关 = 0），shader 侧 `k < 0.5` 走的就是改前那条 `SampleLevel`
	// ⟹ 门关逐位不变（门红线）。为什么是配置直传而不是图形态派生：它不是「有没有这条路」
	// 而是「同一条路用哪个核」，没有第二个真相源可对账（与 z/w 两个标志位性质不同）。
	out.sceneData.taaInputKernel = {env.taaInputKernel, 0.0f, 0.0f, 0.0f};
	// x = **历史裁剪消融门**（`taa.clip_mode`，批 13.11；0 = 现行 = 门关），
	// y = **软裁剪 λ**（`taa.clip_soft`，仅 mode=4 读；无量纲 = 每帧最多搬动 λ 份「到盒边界的距离」）。
	// z = **短窗端点**（臂 10 与臂 13 读，其余臂一个 bit 都不读它）。
	// 🔴 批 13.14b 删 boost 后这里**不能再填 `env.taaFeedback`**：那样它就和 `taaParams.x` 是同一个
	// 数，`feedback = a + (max(z,a) − a)·w ≡ a`（`0·w = 0` 精确）⟹ **臂 10 与臂 8 逐位相同**，
	// 逐像素 α 自适应死掉（债-66）。改填 **1.0** = 「盒判历史不可信（w=1）就这一帧完全丢历史」：
	//   w=0（盒说可信）⟹ α = taaParams.x = 长窗；w=1（盒说不可信）⟹ α = 1 = 无历史。
	// 零新常数，且正是本臂立论的极大端点（「接受域说不可信」应当真的能一票否决）。
	// 臂 13（批 13.15b）复用同一个端点，只是自变量从 `w` 换成 `1 − gate`。
	// ⚠ 代价：臂 10 的 boost 时代读数作废——它本来就欠着 doorframe/wetground/doorpanel 三个机位。
	// 与 input_kernel 同理：配置直传，不是图形态派生。门关（0）⟹ taa.frag 走改前那条硬裁剪，
	// 一个 bit 不动（红线 = `just shot` sha256）。档位语义 → `scene_data.cppm` 同槽注释。
	out.sceneData.taaClipControl = {env.taaClipMode, env.taaClipSoft, 1.0f, 0.0f};
}

/** Phase 15 批 15.1：填 SSR 正向矩阵（scene data 尾区 @722272）。与调用点上一帧
 *  skyInvViewProj **同一表达式**，唯一区别是不取逆——为什么不让 shader 自己求逆、
 *  抖动口径推导全在 scene_data.cppm kSceneDataSsrOffset 注释（重复的推导必然漂移成
 *  两个版本，此处不再重复）。可测接缝声明 → render_list.cppm。 */
void fillSsrViewProj(FrameDrawList& out, const ViewProjection& viewProj) {
	out.sceneData.ssrViewProj = viewProj.proj * viewProj.view;
}

/** Phase 15 批 15.1 参数面：填 SSR 的两个参数子区（scene data @722336 系数
 *  vec4 + @722352 门控 vec4）。
 *  · ssrParams = (SceneBounds.radius, maxDistanceRadii, thicknessPx, biasPx)——x 是尺度锚点
 *    （boundsSystem 每帧聚合），yzw 是 configs/runtime.lua `ssr` 块的三枚系数。ssr_trace.frag
 *    的尺度无关参数由它们导出，锚点选择与系数反推 → scene_data.cppm 该区注释。
 *  · ssrGate   = (roughFull, roughCutoff, blurScale, 0)——与场景尺度无关的门控区间 + 模糊
 *    半径系数（blurScale 批 15.3B 占用 z，消费者 = ssr_apply），故分槽。
 *  两槽**一次填、同源同帧**（拆两个接缝 = 允许"系数新、门控旧"的半帧状态）。
 *  无条件写（同尾区纪律：不按门分支、无消费者不改像素）；CPU 侧**不 clamp 不校验**两条
 *  shader 侧不变式（`bias < thickness` / `roughFull < roughCutoff`），理由 → readSsrBlock。
 *  可测接缝声明 → render_list.cppm。 */
void fillSsrParams(FrameDrawList& out, const SceneBounds& bounds, const SsrTuning& tuning) {
	out.sceneData.ssrParams = {bounds.radius, tuning.maxDistanceRadii, tuning.thicknessPx,
		tuning.biasPx};
	out.sceneData.ssrGate = {tuning.roughFull, tuning.roughCutoff, tuning.blurScale, 0.0f};
}

/** Phase 15 批 15.2 子步 A：填湿面尾区的两个参数子区（scene data @722368 身份调制系数
 *  vec4 + @722384 空间变化 vec4）。
 *  · wetParams  = (strength, dark, roughLo, roughHi)——身份调制系数；湿面**身份**不占参数
 *    （= 1 − smoothstep(ssrGate.x, ssrGate.y, roughness)，与 SSR 门控同区间同源，理由 →
 *    scene_data.cppm 湿面区注释）。
 *  · wetSpatial = (rippleAmp, puddleFreqRadii, 0, 0)——空间变化参数，与身份调制系数
 *    语义分组不同，故分槽。
 *  两槽**一次填、同源同帧**是刻意的（拆两个接缝 = 允许"系数新、空间参数旧"的半帧状态）。
 *  无条件写（同尾区纪律：不按门分支、无消费者不改像素）；CPU 侧**不 clamp 不校验**
 *  （`strength = 0` 是合法消融档、`roughLo < roughHi` 是 shader 侧不变式），理由 →
 *  readWetBlock。可测接缝声明 → render_list.cppm。 */
void fillWetParams(FrameDrawList& out, const WetTuning& tuning) {
	out.sceneData.wetParams = {tuning.strength, tuning.dark, tuning.roughLo, tuning.roughHi};
	out.sceneData.wetSpatial = {tuning.rippleAmp, tuning.puddleFreqRadii, 0.0f, 0.0f};
}

/** 每帧填 SSR + 湿面调参的 scene data 尾区（批 15.1 参数面 + 批 15.2 子步 A 湿面参数）：
 *  两个 fill*Params 各自是**可测接缝**（tests/test_ssr_params / tests/test_wet_params），
 *  本函数只是它们在 renderListSystem 里的**唯一调用点**——该函数已在 120 行代码红线
 *  （每新增一枚调参加一行必破线，main_impl wireSceneSpinContext 同款抽法）。两份调参
 *  都是每帧量、都由 main 启动期灌 ctx（同一 ctx 约定），口径与不变式 → scene_data.cppm
 *  各区注释。 */
void fillTuningTail(FrameDrawList& out, const SceneBounds& bounds, const SsrTuning& ssr,
	const WetTuning& wet) {
	fillSsrParams(out, bounds, ssr);
	fillWetParams(out, wet);
}

/** 簇化轴 ★1-a1i：把四张 **per-objectSlot 表**镜像进本帧 draw list（对象矩阵 / MVP / ObjectDraw
 *  / 簇区间）。抽出来的理由 = 上面 fillTuningTail 同一条：renderListSystem 已在 120 行代码红线，
 *  四张表同构（state 或 ctx → span，录制期读取即完成使命），一处镜像比四处散写更难漏掉一张。
 *  前三张的存储住在 `RenderListState`（本 system 每帧填）；第四张是**启动期静态数据**，住在
 *  registry ctx（装配层 `wireClusterRangeTable` 写，生产者与"为什么走 ctx" → ClusterRangeTable
 *  注释），本层只搬 span。门 `meshlet.enable` 关 = ctx 无该条目 = 空 span ⟹ 执行器把 scene data
 *  该区写成**全 0**（逐位不变）。 */
void mirrorObjectTables(FrameDrawList& out, entt::registry& rg, const RenderListState& state) {
	out.sceneData.objectMatrices = std::span<const glm::mat4>{state.matrices};
	out.sceneData.objectMvps = std::span<const glm::mat4>{state.mvps};
	out.sceneData.objectDraws = std::span<const ObjectDraw>{state.draws};
	if (const auto* ranges = rg.ctx().find<ClusterRangeTable>(); ranges != nullptr)
		out.sceneData.clusterRanges = std::span<const ObjectClusterRange>{ranges->bySlot};
}

void renderListSystem(entt::registry& rg, RGL::Resources&,
	RGL::Environment& envService, const RGL::AssetTable& assets, RenderListState& state) {
	auto& list = rg.ctx().get<FrameDrawList>();
	const auto& viewProj = rg.ctx().get<ViewProjection>();
	const auto& spin = rg.ctx().get<SceneSpin>();
	const auto& camera = rg.ctx().get<Camera>();
	const auto& env = rg.ctx().get<SceneEnv>();  // Phase 5.5（天空盒 draw 生成 + env 参数）
	const auto& taa = rg.ctx().get<TaaState>();  // 批 12.6（帧间载荷；taaSystem 本帧已算好）

	std::vector<Pending> opaque, transparent;
	// Phase 11 批 11.2：阴影 draw 清单。**不经过主视罞剔除**（见下方注释）。
	std::array<std::vector<Pending>, kActiveShadowCascades> shadowCasters;
	// 簇化轴 ★1-a1ii-α：簇路径影子叠加 draw（独立向量，不进 calls/不参与候选序对账；
	// 发射条件 → collectMeshletOverlay）。簇区间表 = 装配层启动期填的 registry ctx，
	// 门关 = 无该条目 = nullptr = 零 draw。
	std::vector<RGL::DrawCall> meshletPending;
	const auto* clusterTable = rg.ctx().find<ClusterRangeTable>();

	// 循环前一次性 resize（objectSlot 稠密 [0, NextObjectSlot.next)，Phase 6 instancing）：
	// 循环内 resize 触发 realloc 会使先前存入 DrawCall 的 span 悬空（帧 0 确定性 UAF——
	// span 活到 drawFrame 录制期读取，vkrenderer.cppm drawCall 的 pushData.data()）
	// Phase 6 instancing：args/matrices 按**实例矩阵槽**（MeshRef.objectSlot）索引而非
	// 资产 id——同 asset 多实例各持独立变换；容量 = NextObjectSlot.next（main 启动期
	// 契约检查 ≤ kSceneMatrixCapacity）
	const uint32_t objectSlotCount = rg.ctx().get<NextObjectSlot>().next;
	state.args.resize(objectSlotCount);
	state.matrices.resize(objectSlotCount, glm::mat4(1.0f));
	state.mvps.resize(objectSlotCount, glm::mat4(1.0f));
	state.draws.resize(objectSlotCount);
	// Phase 11 批 11.2：阴影 push 数组（分块号 × objectSlot，见 RenderListState::shadowArgs）
	state.shadowArgs.resize(static_cast<size_t>(kActiveShadowCascades) * objectSlotCount);
	// 批 10.7 高亮过滤表：按 objectSlot 存材质 id（kNoMaterial = 该槽本帧无实体；
	// 视锥剔除后不在 draw list 里故不会被读到，留哨兵免得读到陈值）
	state.materialBySlot.assign(objectSlotCount, kNoMaterial);

	// fresh 提前声明：光空间体要用主灯方向（fillLightData），而灯原本在循环后才取 ——
	// 故把 fillLightData 提到循环前，fresh 也得先于它存在。
	FrameDrawList fresh;
	// 阴影投影体候选（循环内攒，循环后配光空间体再逐级剔除，见 ShadowCandidate 注释）
	std::vector<ShadowCandidate> shadowCandidates;

	// Phase 7 批次 4：CPU frustum culling——视锥 6 面从 proj·view 提取（每帧一次），逐 entity
	// node-local 剔除球经 modelMatrix 变换到世界后测（保守：跨界判可见，绝不误剔）。
	const Frustum frustum = extractFrustum(viewProj.proj * viewProj.view);
	// Phase 11 批 11.2：级联光空间拟合球（每级一个，与对应 VP 同源）。
	// ① 每级的拟合目标 = 该级**子视锥**的精确外接球（闭式解，见 frustumBoundingSphere），
	//   **不是场景包围球**：后者实测半径 116.26（整条运河 + 街景）会把 texel 摊到 0.227
	//   世界单位，比单级视锥算差一个数量级。
	// ② 剔除用它而不用主视锥：投影体可以在当前视野之外（机位后的楼房照出来的阴影落在可见区内）
	//   ——按主视锥剔会漏掉它们，影子就会在场景中"活着"抬头。
	// ③ 切分深度界先**量化**（quantizeShadowDistance）：自适应 near/far 随相机连续变 →
	//   不量化则半径/splits 逐帧连续变 → 光空间投影缩放滑移 = 影子随视角"游动"
	//   （2026-09-29 用户实测：井盖阴影随相机平移时有时无）。fillShadowData 的 splits
	//   用同一对量化值——两处必须同源。
	const float shadowNear = quantizeShadowDistance(viewProj.nearPlane);
	const float shadowFar = quantizeShadowDistance(viewProj.farPlane);
	// 级 0 近平面固定值 / nearC·farC 直传理由 → computeCascadeSpheres 注释
	const auto cascadeSpheres = computeCascadeSpheres(viewProj, shadowNear, shadowFar);
	// Phase 7b：cullArgs 逐 candidate 填充（candidate 序 = main C3 表序——同 view 迭代序 +
	// 同守卫：pipelineIndex 有效 / groupCount>0 / 不透明；透明留 CPU 排序路径不入候选）
	std::vector<RGL::DrawArgs> cullArgs;
	rg.view<RGL::Transform, RGL::MeshRef, RGL::MaterialRef, RGL::CullSphere>().each(
		[&](const RGL::Transform& tf, const RGL::MeshRef& meshRef, const RGL::MaterialRef& matRef,
			const RGL::CullSphere& cull) {
			const auto& asset = assets.get(meshRef.asset);
			if (asset.pipelineIndex == kInvalidPipelineIndex)
				return;  // 资产无管线条目（防御：注册顺序异常，见 registerScene/bindPipelineToAsset）
			if (asset.groupCount == 0)
				return;  // 不渲染（0 三角形）
			const auto& mat = assets.getMaterial(matRef.material);  // 批次 2：材质资产表解析
			const bool candidate = !mat.alphaModeBlend;  // GPU culling 候选 = 不透明（与 main C3 同守卫）
			auto& args = state.args[meshRef.objectSlot];
			const glm::mat4 modelMatrix = spin.matrix * tf.world;  // 世界矩阵（无 view/proj）
			// 矩阵表先写全量再剔除（Phase 7b）：GPU culling 读同一表（scene data @128），
			// 被剔对象若留陈值（初值恒等）→ GPU 用错矩阵测试（背向机位 MISMATCH 根因）
			state.matrices[meshRef.objectSlot] = modelMatrix;
			// args 剔除测试**前**算（Phase 7b）：被剔候选也要有值（序列 CPU 侧全量装配）；
			// mvp = CPU 位级真相源（GPU 不自算 mvp——FMA 收缩序无跨 CPU/GPU 位级契约）。
			// Phase 7b+（NV DGC 规避）：mvp 写 scene data per-object MVP 表（不再进 DrawArgs），
			// shader readObjectMvp 读同一份值，位级与旧 push 路径一致
			state.mvps[meshRef.objectSlot] = viewProj.proj * viewProj.view * modelMatrix;
			// Phase 7b+（NV DGC 规避）：材质/几何进 per-object 表（不再进 DrawArgs），
			// DrawArgs 仅剩 objectIndex（4B push，绕 NV DGC push 挂死）
			auto& draw = state.draws[meshRef.objectSlot];
			draw.totalTriangles = asset.triangleCount;
			draw.vertexOffset = asset.vertexOffset;  // 拼接 buffer 内块字节偏移（Phase 4 批次 2）
			draw.baseColor = mat.baseColor;
			draw.roughnessMetallic = {mat.roughness, mat.metallic};  // Phase 5 接线（pbr frag 消费；10.2a 单源化 = 生成 ObjectDraw.vec2）
			// Phase 8 ③：自发光 xyz=factor；Phase 8 C1：w = MASK alphaCutoff（alphaDiscard：
			// w>0 且 alpha<w 才丢；非 MASK 写 0 = 恒不测，逐位不变）
			draw.emissive = glm::vec4(mat.emissive, mat.alphaModeMask ? mat.alphaCutoff : 0.0f);
			args.objectIndex = meshRef.objectSlot;  // bindless 各表下标（实例级，instancing）
			// 批 10.7：高亮过滤键（纯 CPU 侧）。放在剔除测试**之前**——虽然被剔对象不在
			// draw list 里读不到，但保持「表覆盖全部 objectSlot」的不变式，便于反查。
			state.materialBySlot[meshRef.objectSlot] = matRef.material;
			if (candidate)
				cullArgs.push_back(args);
			// 剔除（groupCount 守卫后：只统计真图元）——node-local 球 × modelMatrix → 世界球测 6 面
			const glm::vec3 wc = glm::vec3(modelMatrix * glm::vec4(cull.center, 1.0f));
			const float scale = std::max({glm::length(glm::vec3(modelMatrix[0])),
				glm::length(glm::vec3(modelMatrix[1])), glm::length(glm::vec3(modelMatrix[2]))});
			const float worldRadius = cull.radius * scale;
			collectShadowCasters(asset, mat, wc, worldRadius, meshRef.objectSlot, shadowCandidates);
			if (!sphereInFrustum(frustum, wc, worldRadius))
				return;
			const RGL::DrawCall call{
				asset.pipelineIndex,
				std::as_bytes(std::span<const RGL::DrawArgs>{&args, 1}), asset.groupCount};
			auto& bucket = mat.alphaModeBlend ? transparent : opaque;  // MASK 走 opaque（shader 内 discard，Phase 8 C1）
			const glm::vec3 d = glm::vec3(tf.world[3]) - camera.position;  // world 平移列（无 per-entity 包围盒时的排序代理）
			bucket.push_back(Pending{call, glm::dot(d, d)});  // 距离²（免 sqrt，排序单调等价）
			// 簇化轴 ★1-a1ii-α：影子叠加 draw（视锥剔除后 = 与本体同进同退；条件 → helper）。
			collectMeshletOverlay(asset, mat, clusterTable, meshRef.objectSlot,
				std::as_bytes(std::span<const RGL::DrawArgs>{&args, 1}), meshletPending);
		});

	// 主灯（阴影矩阵要它的方向）—— 提到此处：光空间体必须在剔除之前成形
	const Light primary = fillLightData(rg, camera.position, fresh);

	const std::array<ShadowVolume, kActiveShadowCascades> shadowVolumes = buildShadowVolumes(
		primary, cascadeSpheres, shadowCandidates, objectSlotCount, state.shadowArgs, shadowCasters);
	// 批 10.7 高亮通道：把 SelectedObject ctx 的选中材质 + 过滤表搬进 draw list（执行器
	// 据此过滤）。entity==null（未选中/已取消）→ hasSelection=false，pass 零 draw。
	if (const auto* sel = rg.ctx().find<SelectedObject>();
		sel && sel->entity != static_cast<entt::entity>(entt::null)) {
		fresh.hasSelection = true;
		fresh.selectedMaterial = sel->materialId;
	}
	fresh.materialBySlot = std::span<const uint32_t>{state.materialBySlot};
	for (const auto& p : opaque)
		fresh.calls.push_back(p.call);
	// Phase 5.5 批次 4 天空盒段：opaque 之后、透明之前（天空恒最远背景，透明在其上混合）；
	// 单 draw groupCount=1（大三角单 group）。enabled 但条目未建 = 契约漂移（main 保证同批回填）
	fresh.skyboxStart = static_cast<uint32_t>(fresh.calls.size());
	if (env.skybox) {
		if (env.skyboxPipeline == 0xFFFFFFFF)
			throw std::runtime_error("SceneEnv.skybox enabled but skybox pipeline not bound (main contract)");
		fresh.calls.push_back(RGL::DrawCall{env.skyboxPipeline,
			std::as_bytes(std::span<const RGL::DrawArgs>{&state.skyboxArgs, 1}), 1});
	}
	fresh.transparentStart = static_cast<uint32_t>(fresh.calls.size());
	// 稳定排序：同距离保持实体迭代序（确定性输出，截图可复现）
	std::ranges::stable_sort(transparent,
		[](const Pending& a, const Pending& b) { return a.distanceSq > b.distanceSq; });
	for (const auto& p : transparent)
		fresh.calls.push_back(p.call);
	// Phase 11 批 11.2：阴影段独立向量（不进 calls；见 FrameDrawList::shadowCalls）。顺序
	// = 实体迭代序不变（与 opaque 段同源，保证结构可复现）；不做距离排序
	//（深度测试与光方向无关，排序只多花 O(n log n) 且不改结果）。
	// 逐级拼成一条**级主序**的扁平序列 + 段边界（与 calls/skyboxStart 的既有风格同构）：
	// 采样端按段名后缀取子段（FrameDrawList::shadowSegment），故扁平序必须是级主序。
	fresh.shadowBoundaries[0] = 0;
	uint32_t total = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		total += static_cast<uint32_t>(shadowCasters[c].size());
	fresh.shadowCalls.reserve(total);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		for (const auto& p : shadowCasters[c])
			fresh.shadowCalls.push_back(p.call);
		fresh.shadowBoundaries[c + 1] = static_cast<uint32_t>(fresh.shadowCalls.size());
	}
	// 簇化轴 ★1-a1ii-α：叠加段整体搬入（实体迭代序 = 确定性；执行点 = opaque 段之后，
	// 见 vkrenderer_passrec executePassDraws）。门关 = 空向量。
	fresh.meshletCalls = std::move(meshletPending);

	// scene data（Phase 5 批次 2）：光方向归一化、color×intensity 预乘、eyePos=相机世界位置；
	// 对象矩阵表 span 指向 state.matrices（帧局部存储，drawFrame 录制期拷进 staging）
	// Phase 5.5 env 参数（scene data 头 @48）+ 天空盒 clip→world 矩阵（@64，
	// inverse(proj·view) 每帧一次——glm 列主序，shader 端列组合读）
	fresh.sceneData.envParams = {env.intensity, env.mipRange};
	fresh.sceneData.exposure = env.exposure;  // Phase 8 收尾：tonemap 曝光 @56
	fresh.sceneData.aoStrength = env.aoStrength;  // Phase 11 批 11.4：SSAO 强度 @60
	// Phase 8 批次 ④a：漫反射辐照度 SH9（env 加载期算好存 Resources，此处每帧拷入）
	fresh.sceneData.irradianceSH = envService.getIrradianceSH();
	fresh.sceneData.skyInvViewProj = glm::inverse(viewProj.proj * viewProj.view);
	// Phase 15 批 15.1：SSR 正向矩阵 = 上一行**同一表达式不取逆**（接缝可测；口径同源，
	// 抖动后、与 @64 严格互逆；填值表达式只此一处，避免两份口径漂移）
	fillSsrViewProj(fresh, viewProj);
	// Phase 15 批 15.1 收尾 + 批 15.2 子步 A：SSR 尺度锚点（SceneBounds.radius，boundsSystem 本帧已聚合，
	// 与 projectionSystem 的自适应 near/far 同一颗球）与两份调参（SsrTuning/WetTuning 由 main 启动期从
	// configs/runtime.lua 的 ssr/wet 块灌进 registry，每帧量；缺块时 ctx 里就是结构体默认值 = 出厂系数）
	// → fillTuningTail 内两个 fill*Params 各写 scene data 尾区。湿面消费者 = deferred_geometry
	// 湿面 G-buffer 调制（15.2B 接线，默认开）——无条件写；strength=0 消融档实测逐位 = 换锚前旧图。
	fillTuningTail(fresh, rg.ctx().get<SceneBounds>(), rg.ctx().get<SsrTuning>(), rg.ctx().get<WetTuning>());
	fillFsrCameraInput(fresh, viewProj);  // 批 14.5：FSR 相机输入
	// 批 12.6 TAA 载荷（三矩阵/参数，细节见 fillTaaData）
	fillTaaData(fresh, viewProj, env, taa);
	fillShadowData(fresh, shadowNear, shadowFar, shadowVolumes, shadowFormPlan(shadowConfig()).drawAtlas);
	// 四张 per-objectSlot 表（矩阵/MVP/ObjectDraw/簇区间）一次镜像，见 mirrorObjectTables。
	// 前三张住在 state（本 system 每帧填），第四张住在 ctx（装配层启动期填）。
	mirrorObjectTables(fresh, rg, state);
	fresh.cullArgs = std::move(cullArgs);

	// Phase 7b：GPU culling 每帧输入（与上方 CPU 剔除同源同值——planes = 同一 frustum
	// 对象，GPU/CPU 对账基准一致）；sequenceCount 由 Renderer 填
	//（= GpuCull::candidateCount，render_list 不感知 GPU 候选表）
	std::copy(frustum.planes, frustum.planes + 6, fresh.cullInput.planes);

	list = std::move(fresh);
}

}  // namespace RGL
