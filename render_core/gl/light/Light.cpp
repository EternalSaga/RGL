#include "Light.hpp"
namespace RGL {
namespace glcore {
DirectionalLight::DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity) : entity(singleReg->create()) {
    singleReg->emplace<Direction>(entity, lightDirection);
    singleReg->emplace<CommonLightComponent>(entity, lightColor, ambientColor, spotIntensity);
}

std::vector<std::string> DirectionalLight::uniforms() const{
    std::vector<std::string> uniforms;
    uniforms.push_back("ambient");
    uniforms.push_back("specularIntensity");
    uniforms.push_back("lightColor");

    uniforms.push_back("globalLightDirection");

    return uniforms;
}

}  // namespace glcore
}  // namespace RGL
