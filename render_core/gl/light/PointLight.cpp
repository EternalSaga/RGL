#include "PointLight.hpp"
#include "Light.hpp"
#include "Entity.hpp"
#include "EnTTRelationship.hpp"
namespace RGL {
namespace glcore {


using namespace entt::literals;
void updatePointLight() {
    auto reg = EnttReg::getPrimaryRegistry();
    auto view = reg->view<const CommonLight, PointLightComponent, Transform, UBOs>();

    view.each([&reg](const CommonLight& common, PointLightComponent& pointLight, Transform& pos, UBOs& ubos) {
	auto pointLightBlock = (*ubos)["pointLight"];

	updateLightCommon(common, pointLightBlock);

	pointLightBlock->updateCpuUbo("k1", pointLight.mK1);
	pointLightBlock->updateCpuUbo("k2", pointLight.mK2);
	pointLightBlock->updateCpuUbo("kc", pointLight.mKC);

	pointLightBlock->updateCpuUbo("lightPosition", pos.getPosition());

	const glm::vec3 camPosition = reg->ctx().get<glm::vec3>("cameraPos"_hs);
	pointLightBlock->updateCpuUbo("cameraPos", camPosition);
    }

    );
}

}  // namespace glcore
}  // namespace RGL
