#include "SpotLight.hpp"

namespace RGL {
namespace glcore {



SpotLight::SpotLight(const glm::vec3& lightColor, const glm::vec3& ambientColor, float intensity, float innerAngle, float outerAngle, const glm::vec3& targetDirection, const glm::vec3& lightPostion) : Light(lightColor, ambientColor, intensity) {
    this->innerAngle = innerAngle;
    this->outerAngle = outerAngle;
    this->targetDirection = targetDirection;
    this->lightPostion = lightPostion;
    this->ambientColor = ambientColor;
}

void SpotLight::setShaderUniforms(UniformComponent& uniforms) const {
    uniforms["innerAngle"] = glm::radians( innerAngle);
    uniforms["outerAngle"] = glm::radians( outerAngle);
    uniforms["targetDirection"] = targetDirection;
    //uniforms["lightPostion"] = lightPostion;
    uniforms["lightColor"] = lightColor;
    uniforms["ambient"] = ambientColor;
    uniforms["spotIntensity"] = this->specularIntensity;
}

}  // namespace glcore
}  // namespace RGL
