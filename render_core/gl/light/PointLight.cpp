#include "PointLight.hpp"
#include "Light.hpp"
#include "Entity.hpp"
namespace RGL {
namespace glcore {


using namespace entt::literals;
void updatePointLight(entt::registry& reg) {
    auto view = reg.view<const CommonLight, PointLightComponent, PositionComponent, UBOs>();

    view.each([&reg](const CommonLight& common, PointLightComponent& pointLight, PositionComponent& pos, UBOs& ubos) {
	auto pointLightBlock = (*ubos)["pointLight"];

	updateLightCommon(common, pointLightBlock);

	pointLightBlock->updateCpuUbo("k1", pointLight.mK1);
	pointLightBlock->updateCpuUbo("k2", pointLight.mK2);
	pointLightBlock->updateCpuUbo("kc", pointLight.mKC);

	pointLightBlock->updateCpuUbo("lightPosition", pos.position);

	const glm::vec3 camPosition = reg.ctx().get<glm::vec3>("cameraPos"_hs);
	pointLightBlock->updateCpuUbo("cameraPos", camPosition);
    }

    );
}

}  // namespace glcore
}  // namespace RGL
