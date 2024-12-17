#pragma once
#include "Light.hpp"
#include <glm/glm.hpp>
namespace RGL {
namespace glcore {
class SpotLight : public Light {
    float innerAngle;
    float outerAngle;
    glm::vec3 targetDirection;
    glm::vec3 lightPostion;

   public:
    SpotLight(const glm::vec3& lightColor, const glm::vec3& ambientColor, float intensity, float innerAngle, float outerAngle, const glm::vec3& targetDirection, const glm::vec3& lightPostion);
    virtual void setShaderUniforms(UniformComponent& uniforms) const;
};
}  // namespace glcore
}  // namespace RGL
