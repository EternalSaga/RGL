#include "EnTTRelationship.hpp"
namespace RGL {
namespace glcore {
void updateTransforms() {
    auto reg = EnttReg::getPrimaryRegistry();
    // 先更新没有父节点的entity
    auto view = reg->view<Transform, Relationship>();
    view.each([](Transform& transform, Relationship& relationship) {
	if (relationship.parent == entt::null) {
	    transform.updataModelMatrix();
	}
    });

    // 更新有父节点的entity
    std::vector<entt::entity> entitiesToUpdate;
    for (auto entity : view) {
	if (reg->get<Relationship>(entity).parent != entt::null) {
	    entitiesToUpdate.push_back(entity);
	}
    }
    while (!entitiesToUpdate.empty()) {
	std::vector<entt::entity> nextLevelEntities;
	for (auto entity : entitiesToUpdate) {
	    auto& childTransform = reg->get<Transform>(entity);
	    auto& relationship = reg->get<Relationship>(entity);
	    auto& parentTransform = reg->get<Transform>(relationship.parent);
	    // 使用父节点的modelMatrix更新子节点的modelMatrix
	    childTransform.modelMatrix = parentTransform.modelMatrix * childTransform.modelMatrix;
	    childTransform.updataModelMatrix();
	    // 收集下一层的子节点
	    for (auto child : view) {
		if (reg->get<Relationship>(child).parent == entity) {
		    nextLevelEntities.push_back(child);
		}
	    }
	    entitiesToUpdate = nextLevelEntities;
	}
    }
}
}  // namespace glcore
RGL::glcore::Transform::Transform() : modelMatrix(1.0f), position(0.0f), rotation(glm::identity<glm::quat>()), scale(1.0f) {}
void glcore::Transform::updataModelMatrix() {
    modelMatrix = glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(rotation) * glm::scale(glm::mat4(1.0f), scale);
}
}  // namespace RGL
