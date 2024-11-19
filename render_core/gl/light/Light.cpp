#include "Light.hpp"
namespace RGL{
namespace glcore {
DirectionalLight::DirectionalLight(const glm::vec3& lightDirection, const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float spotIntensity, std::shared_ptr<Shader> shader) :entity(singleReg->create()) {

	singleReg->emplace<Direction>(entity, lightDirection);
	singleReg->emplace<CommonLightComponent>(entity, lightColor, ambientColor, spotIntensity);
    singleReg->emplace<std::shared_ptr<Shader>>(entity,shader);
}


}  // namespace glcore
}
