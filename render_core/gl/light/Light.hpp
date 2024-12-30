#pragma once
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include "ShaderManager.hpp"
#include "UBO.hpp"
#include "EnttRegistry.hpp"
namespace RGL {
namespace glcore {


struct IsLight {};
struct CommonLight {
    glm::vec3 lightColor;
    glm::vec3 ambientColor;
    // 镜面反射强度
    float specularIntensity;
};

struct DirectionalCompnent {
    glm::vec3 direction;
};

void updateLightCommon(const CommonLight& common, std::shared_ptr<UBO> ubo);

void updateDirLight(entt::registry& reg);



}  // namespace glcore

}  // namespace RGL
