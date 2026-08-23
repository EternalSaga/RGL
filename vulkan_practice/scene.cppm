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
export struct Relationship {
	entt::entity parent = entt::null;
	std::vector<entt::entity> children;
};

// 指向已上传 GPU 的 primitive（index = model 内 primitive 序号，
// 与 heap slot / 管线表条目一一对应）
export struct MeshRef {
	uint32_t primitiveIndex = 0;
	uint32_t triangleCount = 0;  // 该 primitive 的三角形数（group 数派生用）
};

// Phase 2 材质 = baseColor（flat unlit；alpha 忽略，透明是 Phase 5）
export struct MaterialRef {
	glm::vec4 baseColor{1.0f};
};

// 模型包围球（registry context，registerScene 时写入）：
// renderSystem 据此自适应推导投影 near/far（见 ecs.cppm）。
export struct ModelBounds {
	glm::vec3 center{0.0f};
	float radius{0.0f};
	ModelBounds() = default;
	ModelBounds(glm::vec3 c, float r) : center(c), radius(r) {}
};

}  // namespace RGL
