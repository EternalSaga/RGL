#include "EnTTRelationship.hpp"
#include <spdlog/spdlog.h>
#include <queue>
#include <set>
#include "EnttRegistry.hpp"
#include "rllogger.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>
namespace RGL {
namespace glcore {

void updateTransforms() {
    auto logger = RLLogger::getInstance();
    auto reg = EnttReg::getPrimaryRegistry();

    auto rootView = reg->view<Transform, Relationship>();
    // 广度优先级遍历
    std::queue<entt::entity> queue;
    for (auto& root : rootView) {
	if (reg->get<Relationship>(root).parent == entt::null) {
	    queue.push(root);
	}
    }

    while (!queue.empty()) {
	entt::entity parentNode = queue.front();
	queue.pop();
	// 如果是根节点，那么直接更新modelMatrix，子节点的modelMatrix是父节点的modelMatrix * 子节点的modelMatrix，在下面更新
	if (reg->get<Relationship>(parentNode).parent == entt::null) {
	    reg->get<Transform>(parentNode).doLocalTransform();
	}

	auto parentModelMatrix = reg->get<Transform>(parentNode).modelMatrix;
	auto& currentChildren = reg->get<Relationship>(parentNode).children;

	logger->log_every_n(spdlog::level::debug, 60, format_with_location("Paraent entity id {}, Parent modelMatrix after updataModelMatrix: {}"), entt::to_integral(parentNode), glm::to_string(parentModelMatrix));

	for (entt::entity& child : currentChildren) {
	    auto& childTransform = reg->get<Transform>(child);
	    childTransform.doLocalTransform();
	    logger->log_every_n(spdlog::level::trace, 30, "ChildID {}, Child modelMatrix after updataModelMatrix: {}", entt::to_integral(child), glm::to_string(childTransform.modelMatrix));
	    // 更新子节点的modelMatrix
	    // 只更新了model matrix，如果子节点的是探照灯，那么还需要更新探照灯方向
	    // 当然还有更合适的做法，就是探照灯不应该作为子节点，探照灯本身应该绑定到一个模型entity上，然后模型entity作为子节点
	    // 然后探照灯方向根据模型entity方向更新
	    reg->get<Transform>(child).modelMatrix = parentModelMatrix * childTransform.modelMatrix;
	    logger->log_every_n(spdlog::level::trace, 30, "ChildID {}, Child modelMatrix after parentModelMatrix * childTransform.modelMatrix: {}", entt::to_integral(child), glm::to_string(reg->get<Transform>(child).modelMatrix));
	    queue.push(child);
	}
    }
}

Transform::Transform(glm::vec3 position, glm::vec3 rotation, glm::vec3 scale) : position(position), rotation(rotation), scale(scale), modelMatrix(glm::identity<glm::mat4>()) {
    logger = RLLogger::getInstance();
}

Transform::Transform(glm::vec3 position)
: position(position), rotation(glm::vec3(0.0f)), scale(glm::vec3(1.0f)), modelMatrix(glm::identity<glm::mat4>()) 
{
    
}

void Transform::setRotation(glm::vec3 rotation) {
    this->rotation = rotation;
}
void Transform::setScale(glm::vec3 scale) {
    this->scale = scale;
}
void Transform::setPosition(glm::vec3 position) {
    this->position = position;
}
void Transform::addPosition(glm::vec3 position) {
    this->position += position;
}
void Transform::addRotation(glm::vec3 rotation) {
    this->rotation += rotation;
}
void Transform::addScale(glm::vec3 scale) {
    this->scale += scale;
}
glm::vec3 Transform::getPosition() const {
    return position;
}

void Transform::formToAABB(const AABB& modelLocalAABB,const float desiredSize){
    const float modelLongestEdge = modelLocalAABB.getLongestEdgeLength();

    float scaleFactor = 1.0f;

    if (modelLongestEdge > 1e-5) { //防止除零错误
        scaleFactor = desiredSize / modelLongestEdge;
    }else {
        RLLogger::getInstance()->error("Model has no valid AABB");
        throw std::runtime_error("Model has no valid AABB");
    }

    //模型相对于世界坐标系的中心偏移量
    glm::vec3 centerOffset = -modelLocalAABB.getCenter();
    glm::mat4 tempModelMatrix = glm::mat4(1.0f);
    tempModelMatrix = glm::scale(modelMatrix, glm::vec3(scaleFactor));
    tempModelMatrix = glm::translate(tempModelMatrix, centerOffset);
    
    modelMatrix = tempModelMatrix * modelMatrix;

}

void Transform::doLocalTransform() {
    auto localModelMatrix = glm::identity<glm::mat4>();	 // Reset model matrix

    assert(scale.x != 0.0f && scale.y != 0.0f && scale.z != 0.0f);
    localModelMatrix = glm::scale(localModelMatrix, scale);

    localModelMatrix = glm::rotate(localModelMatrix, glm::radians(rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
    localModelMatrix = glm::rotate(localModelMatrix, glm::radians(rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
    localModelMatrix = glm::rotate(localModelMatrix, glm::radians(rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));

    this->modelMatrix = glm::translate(glm::identity<glm::mat4>(), position) * localModelMatrix;
}

}  // namespace glcore
}  // namespace RGL
