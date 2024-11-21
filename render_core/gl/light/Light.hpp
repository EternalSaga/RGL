#pragma once
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include "Shader.hpp"

#include "EnttRegistry.hpp"
namespace RGL {
namespace glcore {

struct Direction {
    glm::vec3 direction;
};

struct CommonLightComponent {
    
    glm::vec3 lightColor;
    glm::vec3 ambientColor;
    // 镜面反射强度
    float specularIntensity;
};

class DirectionalLight : public SingleReg {
    entt::entity entity;

   public:
    DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity, std::shared_ptr<Shader> shader);
    ~DirectionalLight() = default;
};



}  // namespace glcore

}  // namespace RGL
