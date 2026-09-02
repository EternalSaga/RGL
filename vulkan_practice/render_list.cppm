module;
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // glm::inverse（Phase 5.5 批次 4 skyboxInvViewProj）
#include <ranges>  // std::ranges::stable_sort（libstdc++ namespace alias，无法经模块 re-export）
export module render_list;

import rlstd;
import material_types_generated;  // Phase 4：统一 DrawArgs 超集（scalar 布局 100B，Phase 5 +objectIndex）
import frame_draw_list;
import vk_resources;
import asset_table;
import scene;
import camera;

namespace RGL {

// renderlist system（ViewProjection + 场景实体 + 场景旋转 → FrameDrawList）拆出为独立
// 模块（2026-09-02）：ecs 帧循环骨架与最大块的 system 解耦——场景数据流
// （Transform/MeshRef/MaterialRef/材质资产/透明排序/光照接线/天空盒）集中一处。

// 每帧 push constant 存储（帧数据：活过 renderSystem 循环，DrawCall 的 span 指向其中；
// vkCmdPushDataEXT 录制时读取，只要求内存活到录制结束）
// Phase 4 归一（H1 解决）：单一统一 DrawArgs 超集（material_types_generated，scalar 布局 100B）
// 按资产 id 索引——删旧 cube 单例(64B) + model 每资产(96B，Phase 5 前) 双容器与 AssetKind 分支。
export struct RenderListState {
	std::vector<RGL::DrawArgs> args;  // 每资产一份（100B；资产 id 索引，cube/model 共用同一路径）
	// 对象矩阵表（Phase 5 批次 2，bindless）：资产 id 索引的 spin·world，DrawArgs.objectIndex
	// 进 shader 索引本表（世界空间光照）。帧局部存储（FrameDrawList.sceneData 持 span，
	// 录制期拷贝进 staging，生命周期规则同 args）
	std::vector<glm::mat4> matrices;
	// Phase 5.5 批次 4：天空盒 draw 的 push data（shader 不声明 push constants，
	// 推默认零值 DrawArgs 占位——pushDataEXT 与 shader 声明无关，见 DrawCall 契约）
	RGL::DrawArgs skyboxArgs{};
};

// ViewProjection + 场景实体 + 场景旋转 → FrameDrawList context（单一 draw 路径）。
//
// Phase 4 归一：统一 DrawArgs 超集（所有 shader 风格共享同一布局，material.slang 单一真相源，
// scalar 布局 100B）——单一写入路径写满字段，零 AssetKind 分支（旧 cube 64B / model 96B
// 双 struct 分支删除，ecs_compliance_review H1 注记解决）。各风格只消费自己需要的字段
//（未读字段无害）：cube 只读 mvp（每顶点色），model 读 mvp/totalTriangles/baseColor。
// groupCount 经资产静态字段（MeshAsset.groupCount，上传期按 shader 契约派生，D6）：
// model=ceil(tri/kTrianglesPerGroup)、cube=1 固定单 group、0=不渲染——每帧零推导零分支。
// vertexOffset 经资产静态字段（MeshAsset.vertexOffset，批次 2 拼接顶点 buffer per-primitive
// 偏移，上传期写入）；roughnessMetallic（批次 3 pbr 材质）当前写占位值，对应批次启用真实数据源。
// 画不画 = 实体有无（数据驱动）。
//
// Phase 5 透明排序（roadmap 决策：排序在 renderSystem 内做，不单独建 system）：
// 经 MaterialRef → MaterialAsset.alphaModeBlend（glTF alphaMode == "BLEND"）分桶——
// 不透明段保持实体迭代序（深度写正确），透明段按相机距离² 降序（back-to-front）稳定排序，
// 产出 [opaque | transparent 远→近] 单列表 + transparentStart 边界（drawFrame 两段
// 各 applyDynamicState 一次，见 vkrenderer.cppm）。MASK 材质暂按不透明处理（glTF 规范
// 要求 alphaTest 剔除，未实现）；排序距离 = primitive 实体 world 平移列到相机位置的
// distance2（无包围盒时的标准近似，legacy RenderQueue 同款思路）。
export void renderListSystem(entt::registry& rg, RGL::Resources& resources, RenderListState& state) {
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
	state.args.resize(assets.meshSize());
	state.matrices.resize(assets.meshSize(), glm::mat4(1.0f));

	rg.view<RGL::Transform, RGL::MeshRef, RGL::MaterialRef>().each(
		[&](const RGL::Transform& tf, const RGL::MeshRef& meshRef, const RGL::MaterialRef& matRef) {
			const auto& asset = assets.get(meshRef.asset);
			if (asset.pipelineIndex == kInvalidPipelineIndex)
				return;  // 资产无管线条目（防御：注册顺序异常，见 registerScene/bindPipelineToAsset）
			if (asset.groupCount == 0)
				return;  // 不渲染（0 三角形）
			auto& args = state.args[meshRef.asset];
			const glm::mat4 modelMatrix = spin.matrix * tf.world;  // 世界矩阵（无 view/proj）
			args.mvp = viewProj.proj * viewProj.view * modelMatrix;
			args.totalTriangles = asset.triangleCount;
			args.vertexOffset = asset.vertexOffset;  // 拼接 buffer 内块字节偏移（Phase 4 批次 2）
			const auto& mat = assets.getMaterial(matRef.material);  // 批次 2：材质资产表解析
			args.baseColor = mat.baseColor;
			args.roughnessMetallic = {mat.roughness, mat.metallic};  // Phase 5 接线（pbr frag 消费）
			args.objectIndex = meshRef.asset;  // bindless 对象矩阵表下标（Phase 5 批次 2）
			state.matrices[meshRef.asset] = modelMatrix;
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
