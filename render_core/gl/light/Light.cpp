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

void DirectionalLight::update() {

	static ShaderManager* manager = ShaderManager::getInstance();
    static entt::registry* singleReg = EnttReg::getPrimaryRegistry();
    auto directionalLightEntitis = singleReg->view<const CommonLightComponent, const Direction>();


    directionalLightEntitis.each([](const CommonLightComponent& directLight, const Direction& direction) {
	manager->updateUniform("ambient", directLight.ambientColor);
	manager->updateUniform("specularIntensity", directLight.specularIntensity);
	manager->updateUniform("lightColor", directLight.lightColor);
	manager->updateUniform("globalLightDirection", direction.direction);
    });
}

}  // namespace glcore
}  // namespace RGL
