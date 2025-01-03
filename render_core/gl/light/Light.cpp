#include "Light.hpp"
namespace RGL {
namespace glcore {

	using namespace entt::literals;
void updateLightCommon(const CommonLight& common, std::shared_ptr<UBO> ubo) {
    ubo->updateCpuUbo("ambient", common.ambientColor);
    ubo->updateCpuUbo("spotIntensity", common.specularIntensity);
    ubo->updateCpuUbo("lightColor", common.lightColor);
}
void updateDirLight() {
    auto reg = EnttReg::getPrimaryRegistry();
    auto view = reg->view<const CommonLight, const DirectionalCompnent, UBOs>();

    view.each([&reg](const CommonLight& commonlight, const DirectionalCompnent& dirctionalComponent, UBOs& ubos) {
	auto directionalLight = (*ubos)["DirectionalLight"];

	updateLightCommon(commonlight, directionalLight);


	directionalLight->updateCpuUbo("globalLightDirection", dirctionalComponent.direction);

	const glm::vec3 camPosition = reg->ctx().get<glm::vec3>("cameraPos"_hs);
	directionalLight->updateCpuUbo("cameraPos", camPosition);
    });
}

}  // namespace glcore
}  // namespace RGL
