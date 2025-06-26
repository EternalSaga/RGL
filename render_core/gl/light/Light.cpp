#include "Light.hpp"
#include <spdlog/common.h>
#include "rllogger.hpp"
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
    RLLogger::getInstance()->log_if(spdlog::level::warn, (view.size_hint() == 0), "Current Directional light view is empty");
    view.each([&reg](const CommonLight& commonlight, const DirectionalCompnent& dirctionalComponent, UBOs& ubos) {
	auto directionalLight = (*ubos)["DirectionLight"];

	updateLightCommon(commonlight, directionalLight);


	directionalLight->updateCpuUbo("globalLightDirection", dirctionalComponent.direction);

	const glm::vec3 camPosition = reg->ctx().get<glm::vec3>("cameraPos"_hs);
	directionalLight->updateCpuUbo("cameraPos", camPosition);
    });
}

}  // namespace glcore
}  // namespace RGL
