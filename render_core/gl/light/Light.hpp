#pragma once
#include <glm/fwd.hpp>
#include <glm/glm.hpp>
#include "ShaderManager.hpp"

#include "EnttRegistry.hpp"
namespace RGL {
namespace glcore {


class Light {
   public:
    virtual void setShaderUniforms(UniformComponent& uniforms) const = 0;
	Light() = default;
    virtual ~Light() = default;
};

class DirectionalLight : public SingleReg,public Light {
    glm::vec3 direction;
    glm::vec3 lightColor;
    glm::vec3 ambientColor;
    // 镜面反射强度
    float specularIntensity;
   public:
    DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity);
    ~DirectionalLight() = default;
    void setShaderUniforms(UniformComponent& uniforms) const override;


};



}  // namespace glcore

}  // namespace RGL
