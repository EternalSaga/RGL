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

/**
 * 每帧填阴影 scene data（尾区阴影 VP 区 + 4 个 vec4 采样参数子区）。抽出函数体的原因同
 * collectShadowCasters / fillLightData：函数代码行阈值按函数 extent 计算（内嵌 lambda 也计入），不拆就过阈。
 * @param out 出参：本帧 draw list（写 sceneData.shadowViewProj / Splits / TexelSizes / Params / Atlas）
 * @param light 主灯（取 direction 算光空间方向）
 * @param viewProj 当帧 viewProj（取 near/far 做切分；两者同源——projectionSystem 写入）
 * @param spheres 每级光空间拟合球（与上方剔除用同一批，先算球再同时用于剔除与矩阵）
 */
void fillShadowData(FrameDrawList& out, const Light& light, const ViewProjection& viewProj,
    std::span<const Bounds> spheres) {
    for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
        out.sceneData.shadowViewProj[c] = directionalShadowMatrix(light, spheres[c], kShadowMapSize);
    // splits = 每级远截止（采样端拿它选级；末级写 1e9 = “没有更远的级”，下级比较不会被越过）
    // texelSizes = 2·r/mapSize，每级各自从自己的球推。
    // params = (depthBias, normalOffsetScale, enabled, 保留)；enabled 由门控 diagnostics 驱动。
    // atlas = (图集边长, 单块边长, 分块列数, 保留)，采样端把分块 uv 叠图集用。
    const float kNoFar = 1e9f;
    const uint32_t cols = shadowAtlasColumns(kActiveShadowCascades);
    glm::vec4 splits{0.0f}, texels{0.0f};
    for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
        splits[c] = c + 1u < kActiveShadowCascades
            ? cascadeSplitDistance(viewProj.nearPlane, viewProj.farPlane, c, kActiveShadowCascades)
            : kNoFar;
        texels[c] = spheres[c].radius * 2.0f / static_cast<float>(shadowTileSize(cols));
    }
    out.sceneData.shadowSplits = splits;
    out.sceneData.shadowTexelSizes = texels;
    out.sceneData.shadowParams = {kShadowDepthBias, kShadowNormalOffsetScale,
        diagnostics().shadowPass ? 1.0f : 0.0f, 0.0f};
    // 列数与图装饰的 extent 同源（shadowAtlasColumns）——两处不同步 = 采样端把块叠到错位置
    out.sceneData.shadowAtlas = {static_cast<float>(kShadowMapSize),
        static_cast<float>(shadowTileSize(cols)), static_cast<float>(cols), 0.0f};
}

/**
 * 阴影投影体采集（在主视棱剔除**之前**调用）。抽成独立函数而非孤入子：样式检查的函数代码行
 * 阈值按 **函数 extent 计算（含内嵌 lambda）**——孤光并不能把体重从 renderListSystem 的 120 行阈值里带走。
 * @param asset 资产记录（取 shadowPipelineIndex / groupCount）
 * @param mat 材质记录（取 alphaModeBlend）
 * @param worldCenter/worldRadius 该图元的世界包围球
 * @param objectSlot 实例级槽（scene data 各表下标）
 * @param cascadeSpheres 每级光空间拟合球（与该级剔除用同一个）
 * @param objectSlotCount shadowArgs 的分块 stride（分块号 × objectSlotCount + slot）
 * @param shadowArgs 出参：填 ShadowDrawArgs（帧局部存储，生命周期同 DrawCall.pushData）
 * @param out 出参：候选 draw（按实体迭代序保持确定性）
 */
void collectShadowCasters(const RGL::MeshAsset& asset, const RGL::MaterialAsset& mat,
    const glm::vec3& worldCenter, float worldRadius, uint32_t objectSlot,
    std::span<const Bounds> cascadeSpheres, uint32_t objectSlotCount,
    std::vector<RGL::ShadowDrawArgs>& shadowArgs,
    std::array<std::vector<Pending>, kActiveShadowCascades>& out) {
    // 只收**不透明**材质：blend 体投出的影子会看不到遮挡体的背面（收下来只多花一遍光流，
    // 还会在透明体上留不可见的脊影）；MASK 背面本就被 alpha 消掉，不影响。
    // 不过过 GPU culling：候选表按**主视棱**建立，用它会漏掉靠谱面屋顶的投影体。
    // 门控（diagnostics.shadow_pass）在**最前**：关时不产出任何 draw——图里的阴影 pass 仍然运行
    // （清 4 MB 深度），但无光梲化开销，且采样端因 enabled=0 直接返回全亮。
    if (!diagnostics().shadowPass || asset.shadowPipelineIndex == kInvalidPipelineIndex
        || mat.alphaModeBlend)
        return;
    // 逐级判定：世界球与该级光空间球相交 → 该图元能影响该级图内任意点。**一个图元可以落多级**（大物体跨越多个
    // 切片）——这正是每级剔除能把总量压在单级之上的原因：4 级全量 = 4 × 单级，而实测上
    // 大多数对象只落在至少一级。
    for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
        const Bounds& sphere = cascadeSpheres[c];
        if (glm::length(worldCenter - sphere.center) > sphere.radius + worldRadius)
            continue;
        auto& sargs = shadowArgs[static_cast<size_t>(c) * objectSlotCount + objectSlot];
        sargs.objectIndex = objectSlot;
        sargs.cascadeIndex = c;
        out[c].push_back(Pending{RGL::DrawCall{asset.shadowPipelineIndex,
            std::as_bytes(std::span<const RGL::ShadowDrawArgs>{&sargs, 1}), asset.groupCount}, 0.0f});
    }
}

/**
 * 每帧填灯数据（scene data 头单灯 + 尾区灯数组），并返回主灯。抽出函数体的原因同
 * collectShadowCasters：函数代码行阈值按函数 extent 计算（内嵌 lambda 也计入），不拆就过阈。
 * @param rg 引擎（读 Light 组件）
 * @param eyePos 相机世界位置（头单灯 eyePos 字段）
 * @param out 出参：本帧 draw list（写 sceneData.light / lightCount / lightArray）
 * @return 主灯（Light.index == 0；无灯 entity 时 = 默认 Light{}）——阴影矩阵与
 *         ambient 也要它（主灯 = 页面主光源）
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

}  // namespace


/** materialBySlot 哨兵 = 该 objectSlot 本帧无实体（批 10.7 高亮过滤表；被剔对象不在
 *  draw list 里故读不到，留哨兵避免读到上一帧陈值）。 */
constexpr uint32_t kNoMaterial = 0xFFFFFFFFu;

// renderListSystem 定义（2026-09-02 编译提速 A 方案）：帧函数体移出接口 TU，
// BMI 只含声明 + RenderListState；本单元是叶子边。

/** 实现流水（语义/契约见接口声明）：cull 分桶（opaque/transparent，Pending 暂存）
 * → 可见统计 → 段装配（opaque → skybox 单 draw → transparent 稳定排序）
 * → scene data 填充 → 写 ctx。 */
void renderListSystem(entt::registry& rg, RGL::Resources&,
	RGL::Environment& envService, const RGL::AssetTable& assets, RenderListState& state) {
	auto& list = rg.ctx().get<FrameDrawList>();
	const auto& viewProj = rg.ctx().get<ViewProjection>();
	const auto& spin = rg.ctx().get<SceneSpin>();
	const auto& camera = rg.ctx().get<Camera>();
	const auto& env = rg.ctx().get<SceneEnv>();  // Phase 5.5（天空盒 draw 生成 + env 参数）

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

	// Phase 7 批次 4：CPU frustum culling——视锥 6 面从 proj·view 提取（每帧一次），逐 entity
	// node-local 剔除球经 modelMatrix 变换到世界后测（保守：跨界判可见，绝不误剔）。
	const Frustum frustum = extractFrustum(viewProj.proj * viewProj.view);
	// Phase 11 批 11.2：级联光空间拟合球（每级一个，与对应 VP 同源）。
	// ① 每级的拟合目标 = 该级**子视棱**的包围球（按实用切分的近远平分区间重建子 proj →
	//   取包围球），**不是场景包围球**：后者实测半径 116.26（整条运河 + 街景）会把 texel 摊到 0.227
	//   世界单位，比单级视棱算差一个数量级。
	// ② 剔除用它而不用主视棱：投影体可以在当前视野之外（机位后的楼房照出来的阴影落在可见区内）
	//   ——按主视棱剔会漏掉它们，影子就会在场景中“活着”抬头。
	std::array<Bounds, kActiveShadowCascades> cascadeSpheres{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const float n = viewProj.nearPlane;
		const float f = viewProj.farPlane;
		const float nearC = c == 0 ? n : cascadeSplitDistance(n, f, c - 1u, kActiveShadowCascades);
		const float farC = cascadeSplitDistance(n, f, c, kActiveShadowCascades);
		cascadeSpheres[c] = frustumBoundingSphere(
			projWithDepthRange(viewProj.proj, nearC, farC) * viewProj.view);
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
			collectShadowCasters(asset, mat, wc, worldRadius, meshRef.objectSlot, cascadeSpheres,
				objectSlotCount, state.shadowArgs, shadowCasters);
			if (!sphereInFrustum(frustum, wc, worldRadius))
				return;
			const RGL::DrawCall call{
				asset.pipelineIndex,
				std::as_bytes(std::span<const RGL::DrawArgs>{&args, 1}), asset.groupCount};
			auto& bucket = mat.alphaModeBlend ? transparent : opaque;  // MASK 走 opaque（shader 内 discard，Phase 8 C1）
			const glm::vec3 d = glm::vec3(tf.world[3]) - camera.position;  // world 平移列（无 per-entity 包围盒时的排序代理）
			bucket.push_back(Pending{call, glm::dot(d, d)});  // 距离²（免 sqrt，排序单调等价）
		});

	FrameDrawList fresh;
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
	const Light primary = fillLightData(rg, camera.position, fresh);
	// Phase 5.5 env 参数（scene data 头 @48）+ 天空盒 clip→world 矩阵（@64，
	// inverse(proj·view) 每帧一次——glm 列主序，shader 端列组合读）
	fresh.sceneData.envParams = {env.intensity, env.mipRange};
	fresh.sceneData.exposure = env.exposure;  // Phase 8 收尾：tonemap 曝光 @56
	fresh.sceneData.aoStrength = env.aoStrength;  // Phase 11 批 11.4：SSAO 强度 @60
	// Phase 8 批次 ④a：漫反射辐照度 SH9（env 加载期算好存 Resources，此处每帧拷入）
	fresh.sceneData.irradianceSH = envService.getIrradianceSH();
	fresh.sceneData.skyInvViewProj = glm::inverse(viewProj.proj * viewProj.view);
	fillShadowData(fresh, primary, viewProj, cascadeSpheres);
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
