#pragma once
#include "Light.hpp"
#include <glm/glm.hpp>
namespace RGL {
namespace glcore {

struct SpotLightComponnet {
    float innerAngle;
    float outerAngle;
    glm::vec3 targetDirection;
};

void updateSpotLight(entt::registry& reg);


}  // namespace glcore
}  // namespace RGL
