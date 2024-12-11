#include "Light.hpp"
namespace RGL {
namespace glcore {
DirectionalLight::DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity)  {
    this->direction = lightDirection;
    this->lightColor = lightColor;
    this->ambientColor = ambientColor;
    this->specularIntensity = spotIntensity;
}

void DirectionalLight::setShaderUniforms(UniformComponent& uniforms) const {
    uniforms["ambient"] = ambientColor;
    uniforms["specularIntensity"] = specularIntensity;
    uniforms["lightColor"] = lightColor;
    uniforms["globalLightDirection"] = direction;
}



}  // namespace glcore
}  // namespace RGL
