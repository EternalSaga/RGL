module;
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <ranges>
module render_list:impl;

import render_list;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import material_types_generated;
import frame_draw_list;
import vk_resources;
import asset_table;
import scene;
import camera;

namespace RGL {

// renderListSystem 定义（2026-09-02 编译提速 A 方案）：帧函数体移出接口 TU，
// BMI 只含声明 + RenderListState；本单元是叶子边。

void renderListSystem(entt::registry& rg, RGL::Resources& resources, RenderListState& state) {
	auto& list = rg.ctx().get<FrameDrawList>();
	const auto& viewProj = rg.ctx().get<ViewProjection>();
	const auto& spin = rg.ctx().get<SceneSpin>();
	const auto& camera = rg.ctx().get<Camera>();
	const auto& light = rg.ctx().get<Light>();
	const auto& env = rg.ctx().get<SceneEnv>();  // Phase 5.5（天空盒 draw 生成 + env 参数）
	const auto& assets = resources.getAssetTable();

	struct Pending {
		RGL::DrawCall call;
		float distanceSq = 0.0f;
	};
	std::vector<Pending> opaque;
	std::vector<Pending> transparent;

	// 循环前一次性 resize（资产 id 稠密 [0, meshSize)，vkresources.cppm meshSize）：
	// 循环内 resize 触发 realloc 会使先前存入 DrawCall 的 span 悬空（帧 0 确定性 UAF——
	// span 活到 drawFrame 录制期读取，vkrenderer.cppm drawCall 的 pushData.data()）
	// Phase 6 instancing：args/matrices 按**实例矩阵槽**（MeshRef.objectSlot）索引而非
	// 资产 id——同 asset 多实例各持独立变换；容量 = NextObjectSlot.next（main 启动期
	// 契约检查 ≤ kSceneMatrixCapacity）
	const uint32_t objectSlotCount = rg.ctx().get<NextObjectSlot>().next;
	state.args.resize(objectSlotCount);
	state.matrices.resize(objectSlotCount, glm::mat4(1.0f));

	rg.view<RGL::Transform, RGL::MeshRef, RGL::MaterialRef>().each(
		[&](const RGL::Transform& tf, const RGL::MeshRef& meshRef, const RGL::MaterialRef& matRef) {
			const auto& asset = assets.get(meshRef.asset);
			if (asset.pipelineIndex == kInvalidPipelineIndex)
				return;  // 资产无管线条目（防御：注册顺序异常，见 registerScene/bindPipelineToAsset）
			if (asset.groupCount == 0)
				return;  // 不渲染（0 三角形）
			auto& args = state.args[meshRef.objectSlot];
			const glm::mat4 modelMatrix = spin.matrix * tf.world;  // 世界矩阵（无 view/proj）
			args.mvp = viewProj.proj * viewProj.view * modelMatrix;
			args.totalTriangles = asset.triangleCount;
			args.vertexOffset = asset.vertexOffset;  // 拼接 buffer 内块字节偏移（Phase 4 批次 2）
			const auto& mat = assets.getMaterial(matRef.material);  // 批次 2：材质资产表解析
			args.baseColor = mat.baseColor;
			args.roughnessMetallic = {mat.roughness, mat.metallic};  // Phase 5 接线（pbr frag 消费）
			args.objectIndex = meshRef.objectSlot;  // bindless 对象矩阵表下标（实例级，instancing）
			state.matrices[meshRef.objectSlot] = modelMatrix;
			const RGL::DrawCall call{
				asset.pipelineIndex,
				std::as_bytes(std::span<const RGL::DrawArgs>{&args, 1}), asset.groupCount};
			auto& bucket = mat.alphaModeBlend ? transparent : opaque;  // MASK 暂按不透明（见函数注释）
			const glm::vec3 d = glm::vec3(tf.world[3]) - camera.position;  // world 平移列（无 per-entity 包围盒时的排序代理）
			bucket.push_back(Pending{call, glm::dot(d, d)});  // 距离²（免 sqrt，排序单调等价）
		});

	FrameDrawList fresh;
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
	fresh.sceneData.light = LightDataGpu{
		glm::normalize(light.direction),
		camera.position,
		light.color * light.intensity,
		light.ambient};
	// Phase 5.5 env 参数（scene data 头 @48）+ 天空盒 clip→world 矩阵（@64，
	// inverse(proj·view) 每帧一次——glm 列主序，shader 端列组合读）
	fresh.sceneData.envParams = {env.intensity, env.mipRange};
	fresh.sceneData.skyInvViewProj = glm::inverse(viewProj.proj * viewProj.view);
	fresh.sceneData.objectMatrices = std::span<const glm::mat4>{state.matrices};

	list = std::move(fresh);
}

}  // namespace RGL
