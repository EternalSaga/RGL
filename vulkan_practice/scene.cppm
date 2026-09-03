module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
export module scene;

import rlstd;
import cube;

namespace RGL {

// 场景组件（Phase 2 最小集）：模型 primitive 的 ECS 表示。
// 单独成模块（而非放 ecs.cppm / model_importer.cppm）是为避免
// ecs ↔ model_importer 的循环 import。

export struct Transform {
	glm::mat4 local{1.0f};
	glm::mat4 world{1.0f};  // Phase 2 无层级嵌套：world = local（注册时写入）
};

// 父子关系：与 legacy 同款手写组件（legacy/render_core/gl/EnTTRelationship.hpp，
// entt 3.16 无内置 Relationship 头）。parent = entt::null 表示根。
// 单向（E5.2）：children 不存储，由 rg.view<Relationship> 按 parent 过滤派生
//（同一关系不再存两份，一致性不靠写入方自觉）。
export struct Relationship {
	entt::entity parent = entt::null;
};

// 引用已上传 GPU 的资产（asset id = Resources 资产表的下标，见 vkresources.cppm 的 AssetTable）：
// GPU 数据 / heap slot / 三角形数 / 管线条目引用全部收敛在资产表记录里，
// entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args 的五方约定收敛为一处映射（E1.1/E5.1）
// objectSlot（Phase 6 instancing）：**场景实例**级对象矩阵表下标（scene data @128，
// shader readObjectMatrix 消费）。注册期从 NextObjectSlot 单调领取，同 asset 多实例
//（同文件重复 entity = 共享上传的 instancing）各持独立槽——资产身份与实例变换分离
//（ECS 规范 5）。
export struct MeshRef {
	uint32_t asset = 0;
	uint32_t objectSlot = 0;
};

// 对象矩阵槽分配器（registry context，Phase 6 instancing；main 启动期 emplace）：
// 容量契约 = kSceneMatrixCapacity（scene data @128 矩阵表，main 注册完成后检查）。
export struct NextObjectSlot {
	uint32_t next = 0;
};

// 材质真引用（批次 2，E5.3 解决；roadmap Phase 2"暂时只存引用"兑现）：material =
// Resources 资产表 materials 容器的下标（MaterialAsset，vkresources.cppm）——
// baseColor / textureId / alphaMode 全部收敛进材质资产记录，组件只持纯数据引用
//（项目"组件只存数据引用"约定）。每帧渲染经资产表解析（ecs.cppm renderListSystem）。
// 无纹理/无 uv 的 primitive 有效纹理 = 白 1×1（有效选择不在组件，见 MaterialAsset 注释）。
export struct MaterialRef {
	uint32_t material = 0;
};

// 包围球（组件，挂在有包围球的 entity 上）：现状 = 模型整体球挂场景根（registerScene 写入）；
// 聚合进 SceneBounds 由 boundsSystem 完成（ecs.cppm）。
export struct Bounds {
	glm::vec3 center{0.0f};
	float radius{0.0f};
	Bounds() = default;
	Bounds(glm::vec3 c, float r) : center(c), radius(r) {}
};

// 场景内容聚合包围球（registry context，boundsSystem 每帧由全部 Bounds 组件 union 推导；
// radius=0 = 空场景）。projectionSystem 据此自适应推导投影 near/far（ecs.cppm）。
export struct SceneBounds {
	glm::vec3 center{0.0f};
	float radius{0.0f};
};

// 定向光（registry context，Phase 5 批次 2；main 启动期从 configs/scene.lua light 段
// emplace，仿 SceneSpin）。renderListSystem 每帧转成 LightDataGpu 写 scene data
//（direction 归一化、color×intensity 预乘、eyePos=相机位置，世界空间光照）。
// 单灯是本期范围（roadmap Phase 5"Directional Light"）；多灯留 Phase 7 延迟光照。
export struct Light {
	glm::vec3 direction{0.35f, -1.0f, 0.25f};  // 照射方向（世界空间，消费时归一化）
	glm::vec3 color{1.0f, 0.98f, 0.95f};
	float intensity = 3.0f;
	glm::vec3 ambient{0.03f, 0.03f, 0.035f};
};

// 环境贴图参数（registry context，Phase 5.5；main 启动期从 configs/scene.lua
// environment 段 + env cubemap 上传结果 emplace，仿 Light）。renderListSystem 每帧
// 写进 scene data 头 @48（SceneFrameData.envParams），env.slang readEnvParams 消费。
// mipRange = cubemap mipLevels−1（上传期才知道，故 main 在 loadEnvCubeMapFiles 后填）；
// 默认全 0 = 无环境（IBL 风格下环境项黑，direct light 不受影响）。
export struct SceneEnv {
	float intensity = 0.0f;
	float mipRange = 0.0f;
	// Phase 5.5 批次 4 天空盒：enabled = env 已上传且 scene.lua environment.skybox=true；
	// pipeline = ShaderRegistry "skybox" 条目下标（main 启动期 addSkyEntry 后回填；
	// 0xFFFFFFFF = 未建条目——renderListSystem 见 enabled 但 pipeline 无效 = 契约漂移）
	bool skybox = false;
	uint32_t skyboxPipeline = 0xFFFFFFFF;
};

// 场景整体时间旋转（registry context，main 启动期 emplace；animationSystem 每帧更新
// angle/matrix）：speed=0 = 静止（configs/scene.lua 的 spin 开关）。
// 现状承载"全场景同一旋转"（MVP = proj·view·matrix·world）；per-entity 旋转需求出现时
// 改为写 Transform 组件，结构同构无需二次重构（ecs_compliance_review E4.1）。
export struct SceneSpin {
	glm::vec3 axis{0.5f, 1.0f, 0.0f};
	float speed = 1.0f;   // rad/s
	float angle = 0.0f;   // 累积角（animationSystem: angle += dt * speed）
	glm::mat4 matrix{1.0f};  // = rotate(axis, angle)，animationSystem 每帧更新
};

// 每帧 view/proj（registry context，projectionSystem 由 Camera + SceneBounds + WindowExtent 推导）
export struct ViewProjection {
	glm::mat4 view{1.0f};
	glm::mat4 proj{1.0f};
};

// cube 一等资产注册（批次 3，E1.4/H1/M1）：cube entity = Transform +
// Bounds（包围球由顶点半径派生，经 boundsSystem 自动纳入 SceneBounds 聚合，L2 解决）
// + MeshRef{cube 资产 id} + MaterialRef{材质 id} + Relationship（根，parent = null）。
// 画不画 = 实体有无（数据驱动：showCube 配置由 main 决定是否注册，无构造期 gating）。
// rootTransform（Phase 6 批次 5）：cube 与模型同等 entity 位姿（local=world=根矩阵；
// 默认 identity = 几何原点 ±0.5 旧行为）。
// 注意：cube 着色 = 每顶点色（cube.frag 从顶点读 color），MaterialRef 的 baseColor
// 不被 cube 管线消费（Phase 4 归一：统一 DrawArgs 超集单一布局，各风格只消费自己字段）。
export entt::entity registerCubeEntity(entt::registry& rg, uint32_t meshAsset, uint32_t materialAsset,
	const glm::mat4& rootTransform = glm::mat4(1.0f)) {
	auto e = rg.create();
	auto& tf = rg.emplace<Transform>(e);
	tf.local = rootTransform;
	tf.world = rootTransform;  // cube 几何已在原点 ±0.5
	glm::vec3 center{0.0f};
	float radius = 0.0f;
	for (const auto& v : cubeVertices)
		radius = std::max(radius, glm::length(glm::vec3(v.position[0], v.position[1], v.position[2])));
	rg.emplace<Bounds>(e, glm::vec3(rootTransform * glm::vec4(center, 1.0f)),
		radius * glm::length(glm::vec3(rootTransform[0])));
	auto& meshRef = rg.emplace<MeshRef>(e);
	meshRef.asset = meshAsset;
	meshRef.objectSlot = rg.ctx().get<NextObjectSlot>().next++;
	rg.emplace<MaterialRef>(e).material = materialAsset;
	rg.emplace<Relationship>(e);  // parent = null（根）
	return e;
}

}  // namespace RGL
