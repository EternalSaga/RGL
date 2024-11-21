#include "PointLight.hpp"
#include "Light.hpp"
#include "Entity.hpp"
namespace RGL {
namespace glcore {

PointLight::PointLight(const glm::vec3& lightColor, const glm::vec3& ambientColor, float specularIntensity, float mK2, float mK1, float mKC, glm::vec3 position, std::shared_ptr<Shader> shader) : entity(singleReg->create()) {
    singleReg->emplace<PointLightComponent>(entity, mK2, mK1, mKC);
    singleReg->emplace<PositionComponent>(entity, position);
    singleReg->emplace<CommonLightComponent>(entity, lightColor, ambientColor, specularIntensity);
}
}  // namespace glcore
}  // namespace RGL
