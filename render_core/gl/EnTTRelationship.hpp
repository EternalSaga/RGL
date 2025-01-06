
#pragma once
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "EnttRegistry.hpp"
namespace RGL {
namespace glcore {
struct Transform {
    glm::mat4 modelMatrix;
    glm::vec3 position;
    glm::quat rotation;
    glm::vec3 scale;
    Transform();
    void updataModelMatrix();
};
struct Relationship {
    entt::entity parent = entt::null;
    entt::entity next = entt::null;
};

void updateTransforms();

}}
