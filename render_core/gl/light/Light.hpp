#pragma once
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include "ShaderManager.hpp"

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

class Light {
   public:
    virtual std::vector<std::string> uniforms() const = 0;
};

class DirectionalLight : public SingleReg,public Light {
    entt::entity entity;
    
   public:
    DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity);
    ~DirectionalLight() = default;
    std::vector<std::string> uniforms()const override;

	static void update();

};



}  // namespace glcore

}  // namespace RGL
