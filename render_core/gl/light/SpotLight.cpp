#include "SpotLight.hpp"
#include "Entity.hpp"
namespace RGL {
namespace glcore {



using namespace entt::literals;
void updateSpotLight() {
    auto reg = EnttReg::getPrimaryRegistry();
    auto view = reg->view<const CommonLight, SpotLightComponnet, PositionComponent, UBOs>();
	view.each(
	[&reg](const CommonLight& common, SpotLightComponnet& spotlight, PositionComponent& pos, UBOs& ubos) {
	    auto spotBlock = (*ubos)["SpotLight"];
	    updateLightCommon(common, spotBlock);

	    spotBlock->updateCpuUbo("innerAngle", glm::radians(spotlight.innerAngle));
	    spotBlock->updateCpuUbo("outerAngle", glm::radians(spotlight.outerAngle));
	    spotBlock->updateCpuUbo("targetDirection", spotlight.targetDirection);

		spotBlock->updateCpuUbo("lightPosition", pos.position);

	    const glm::vec3 camPosition = reg->ctx().get<glm::vec3>("cameraPos"_hs);
	    spotBlock->updateCpuUbo("cameraPos", camPosition);
		}
	);

}

}  // namespace glcore
}  // namespace RGL
