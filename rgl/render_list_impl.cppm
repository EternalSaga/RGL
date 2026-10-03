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
import diagnostics;  // diagnostics().shadowPass（阴影总开关）

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
 * 门控（diagnostics.shadowPass）在**最前**：关时不产出任何 draw——图里的阴影 pass 仍然运行
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
    if (!diagnostics().shadowPass || asset.shadowPipelineIndex == kInvalidPipelineIndex
        || mat.alphaModeBlend)
        return;
    out.push_back(ShadowCandidate{worldCenter, worldRadius, objectSlot,
        asset.shadowPipelineIndex, asset.groupCount});
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

}  // namespace

/**
 * 定义（接口声明与 @param 契约见 render_list.cppm；12.3d 接缝 = shadowPassEnabled
 * 显式入参取代内部 diagnostics() 单例读取，调用点传 diagnostics().shadowPass）。
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
    // params = (depthBias, normalOffsetScale, enabled, shadowFar)；enabled 由门控 diagnostics
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
    // 批 11.5 PCSS 参数（只有 -DPCSS_SOFT 变体读，原版 SPV 不引用 = 门关红线）：
    // 半影全宽系数取 2·tan(θ/2)（θ = 太阳视直径；小角下 ≈ tanθ，物理定义用半角式）。
    const float halfAngle = static_cast<float>(diagnostics().pcssSunDegrees) * 0.01745329252f * 0.5f;
    out.sceneData.shadowPcss = {2.0f * std::tan(halfAngle),
        static_cast<float>(diagnostics().pcssSearchRadius),
        static_cast<float>(diagnostics().pcssMaxPenumbra), 0.0f};
    // 批 11.6 EVSSM 参数（只有 -DEVSSM_SOFT 变体读）：x = 半坡 tan(θ/2)（与 pcss 同数值键，
    // EVSSM 消费半坡而非全宽系数）；y/z = 搜索半径/半影上限（图集纹素 mip0 域）；
    // w = 矩链最大 mip（EvssmShadow 8 级链固定 7，docs/evssm_plan.md §3.1）。
    out.sceneData.shadowEvssm = {std::tan(halfAngle),
        static_cast<float>(diagnostics().pcssSearchRadius),
        static_cast<float>(diagnostics().pcssMaxPenumbra),
        static_cast<float>(RGL::kEvssmMipLevels - 1u)};
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
/**
 * 批 12.6 TAA 的 scene data 载荷（尾区 @722080 / @722144 / @722208，生产端在此、
 * 消费端 = taa.frag.slang；字节布局与「两侧口径」见 scene_data.cppm kSceneDataTaa* 注释）。
 *
 * 一句话版：`prevViewProj` = 上一帧**未抖** proj·view；`motion` = prevSpin·inverse(curSpin)
 * （全场景同一旋转 ⇒ 一个 mat4 精确补偿物体运动，spin 关 = 单位阵）；
 * `params` = (feedback, historyValid, jitterX px, jitterY px)，historyValid=0 时
 * taa.frag 直接输出本帧 = **逐位旁路**。
 * 注意 `skyInvViewProj`（上一行填）用的是**抖动后** proj——重建世界坐标的半边是它，
 * 投回上一帧的半边是这里的 prevViewProj，两者口径不同是刻意的。
 */
void fillTaaData(FrameDrawList& out, const ViewProjection& viewProj, const SceneEnv& env,
	const TaaState& taa) {
	out.sceneData.taaPrevViewProj = taa.payloadPrevViewProj;
	out.sceneData.taaMotion = taa.payloadMotion;
	out.sceneData.taaParams = {env.taaFeedback, taa.historyValid ? 1.0f : 0.0f,
		viewProj.jitterPx.x, viewProj.jitterPx.y};
}

void renderListSystem(entt::registry& rg, RGL::Resources&,
	RGL::Environment& envService, const RGL::AssetTable& assets, RenderListState& state) {
	auto& list = rg.ctx().get<FrameDrawList>();
	const auto& viewProj = rg.ctx().get<ViewProjection>();
	const auto& spin = rg.ctx().get<SceneSpin>();
	const auto& camera = rg.ctx().get<Camera>();
	const auto& env = rg.ctx().get<SceneEnv>();  // Phase 5.5（天空盒 draw 生成 + env 参数）
	const auto& taa = rg.ctx().get<TaaState>();  // 批 12.6（帧间载荷；taaSystem 本帧已算好）

	std::vector<Pending> opaque;
	std::vector<Pending> transparent;
	// Phase 11 批 11.2：阴影 draw 清单。**不经过主视罞剔除**（见下方注释）。
	std::array<std::vector<Pending>, kActiveShadowCascades> shadowCasters;

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
	// 级 0 的近平面用**固定小值**而非量化 near：自适应 near（下限实测 0.05）量化后可能
	// 抬高到 0.125，视深落在 [0.05, 0.125) 的近处片元（选级 = c0）会掉出子视锥 → 出块
	// 全亮旁路 → 脚下没影子。固定 0.02 恒低于任何自适应值，且姿态无关（不引入游动）。
	static constexpr float kShadowNearFloor = 0.02f;
	std::array<Bounds, kActiveShadowCascades> cascadeSpheres{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const float nearC = c == 0 ? kShadowNearFloor
			: cascadeSplitDistance(shadowNear, shadowFar, c - 1u, kActiveShadowCascades);
		const float farC = cascadeSplitDistance(shadowNear, shadowFar, c, kActiveShadowCascades);
		// nearC/farC **直接传进去**，不从子 proj 反解（2026-09-30 批 12.1 单测抓出）：
		// float32 RH_ZO 透视矩阵在 near/far ≳1e3 时就编不准 far，反解误差被比值线性
		// 放大到超过拟合球的保守余量 → 球装不下子视锥。而这两个值就在上面，是真值。
		// 与 projWithDepthRange 的既有理由同源（"直接拿字段不会漂"）。见
		// scene_context::frustumBoundingSphere 的 4 参重载注释。
		cascadeSpheres[c] = frustumBoundingSphere(viewProj.view,
			projWithDepthRange(viewProj.proj, nearC, farC), nearC, farC);
	}
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
			collectShadowCasters(asset, mat, wc, worldRadius, meshRef.objectSlot,
				shadowCandidates);
			if (!sphereInFrustum(frustum, wc, worldRadius))
				return;
			const RGL::DrawCall call{
				asset.pipelineIndex,
				std::as_bytes(std::span<const RGL::DrawArgs>{&args, 1}), asset.groupCount};
			auto& bucket = mat.alphaModeBlend ? transparent : opaque;  // MASK 走 opaque（shader 内 discard，Phase 8 C1）
			const glm::vec3 d = glm::vec3(tf.world[3]) - camera.position;  // world 平移列（无 per-entity 包围盒时的排序代理）
			bucket.push_back(Pending{call, glm::dot(d, d)});  // 距离²（免 sqrt，排序单调等价）
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
	// 批 12.6 TAA 载荷（三矩阵/参数，细节见 fillTaaData）
	fillTaaData(fresh, viewProj, env, taa);
	fillShadowData(fresh, shadowNear, shadowFar, shadowVolumes, diagnostics().shadowPass);
	fresh.sceneData.objectMatrices = std::span<const glm::mat4>{state.matrices};
	fresh.sceneData.objectMvps = std::span<const glm::mat4>{state.mvps};
	fresh.sceneData.objectDraws = std::span<const ObjectDraw>{state.draws};
	fresh.cullArgs = std::move(cullArgs);

	// Phase 7b：GPU culling 每帧输入（与上方 CPU 剔除同源同值——planes = 同一 frustum
	// 对象，GPU/CPU 对账基准一致）；sequenceCount 由 Renderer 填
	//（= GpuCull::candidateCount，render_list 不感知 GPU 候选表）
	std::copy(frustum.planes, frustum.planes + 6, fresh.cullInput.planes);

	list = std::move(fresh);
}

}  // namespace RGL
