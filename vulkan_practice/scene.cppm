module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
export module scene;

import rlstd;

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
export struct MeshRef {
	uint32_t asset = 0;
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

}  // namespace RGL
