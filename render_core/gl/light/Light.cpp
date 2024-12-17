#include "Light.hpp"
namespace RGL {
namespace glcore {
DirectionalLight::DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity)
    : Light(lightColor,ambientColor,specularIntensity) {
    this->direction = lightDirection;

}

void DirectionalLight::setShaderUniforms(UniformComponent& uniforms) const {
    uniforms["ambient"] = ambientColor;
    uniforms["specularIntensity"] = specularIntensity;
    uniforms["lightColor"] = lightColor;
    uniforms["globalLightDirection"] = direction;
}



Light::Light(const glm::vec3& lightColor, const glm::vec3& ambientColor, float intensity) : lightColor(lightColor), ambientColor(ambientColor), specularIntensity(intensity) {
}

}  // namespace glcore
}  // namespace RGL
