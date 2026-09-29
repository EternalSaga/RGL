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
import vk_resources;
import environment;  // Environment（irradianceSH，架构债务续账步骤 3）
import asset_table;
import scene_context;
import camera;

namespace RGL {

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

	/** 待落段 draw（段装配前的暂存项）：call = 定稿 draw；distanceSq = 排序距离
	 *（透明段 back-to-front 稳定排序键，距离² 免 sqrt，单调等价）。 */
	struct Pending {
		RGL::DrawCall call;
		float distanceSq = 0.0f;
	};
	std::vector<Pending> opaque;
	std::vector<Pending> transparent;

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
	// 批 10.7 高亮过滤表：按 objectSlot 存材质 id（kNoMaterial = 该槽本帧无实体；
	// 视锥剔除后不在 draw list 里故不会被读到，留哨兵免得读到陈值）
	state.materialBySlot.assign(objectSlotCount, kNoMaterial);

	// Phase 7 批次 4：CPU frustum culling——视锥 6 面从 proj·view 提取（每帧一次），逐 entity
	// node-local 剔除球经 modelMatrix 变换到世界后测（保守：跨界判可见，绝不误剔）。
	const Frustum frustum = extractFrustum(viewProj.proj * viewProj.view);

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
			if (!sphereInFrustum(frustum, wc, cull.radius * scale))
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

	// scene data（Phase 5 批次 2）：光方向归一化、color×intensity 预乘、eyePos=相机世界位置；
	// 对象矩阵表 span 指向 state.matrices（帧局部存储，drawFrame 录制期拷进 staging）
	// 灯 entity 化（Phase 7，ECS 规范 3）：灯 = entity（Light 组件），画不画 = 实体有无。
	// 收集**不依赖 view 迭代序**（无保证）——按 Light.index 显式落槽：primary = index 0
	//（forward @0 头单灯，diff0）；延迟灯数组按 index 填（≤ kMaxLights，尾区
	// @kSceneDataLightArrayOffset，契约 lights.slang）。lightEntityCount 每帧算（非启动期
	// 常量）= 留"运行时增删灯"接缝。
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
	fresh.sceneData.light = SceneLight{
		glm::normalize(primary.direction),
		camera.position,
		primary.color * primary.intensity,
		primary.ambient};
	fresh.sceneData.lightCount = nLights;
	for (uint32_t i = 0; i < nLights; ++i) {
		const Light& L = lightSlot[i];
		GpuLight& G = fresh.sceneData.lightArray[i];
		G.direction = glm::vec4(glm::normalize(L.direction), 0.0f);
		G.position = glm::vec4(L.position, 0.0f);
		G.color = glm::vec4(L.color * L.intensity, 0.0f);
		G.params = glm::vec4(L.range, static_cast<float>(L.type), 0.0f, 0.0f);
	}
	// Phase 5.5 env 参数（scene data 头 @48）+ 天空盒 clip→world 矩阵（@64，
	// inverse(proj·view) 每帧一次——glm 列主序，shader 端列组合读）
	fresh.sceneData.envParams = {env.intensity, env.mipRange};
	fresh.sceneData.exposure = env.exposure;  // Phase 8 收尾：tonemap 曝光 @56
	fresh.sceneData.aoStrength = env.aoStrength;  // Phase 11 批 11.4：SSAO 强度 @60
	// Phase 8 批次 ④a：漫反射辐照度 SH9（env 加载期算好存 Resources，此处每帧拷入）
	fresh.sceneData.irradianceSH = envService.getIrradianceSH();
	fresh.sceneData.skyInvViewProj = glm::inverse(viewProj.proj * viewProj.view);
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
