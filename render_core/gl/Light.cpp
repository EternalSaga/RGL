#include "Light.hpp"
namespace RGL{
namespace glcore {
DirectionalLight::DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity):
lightDirection(lightDirection), lightColor(lightColor), ambientColor(ambientColor), specularIntensity(specularIntensity)
{

}

void DirectionalLight::setShaderUniforms(Shader* shader) const{
    shader->setUniform("ambient", ambientColor);
    shader->setUniform("specularIntensity", specularIntensity);
    shader->setUniform("lightColor", lightColor);
    shader->setUniform("globalLightDirection", lightDirection);
}
}
}