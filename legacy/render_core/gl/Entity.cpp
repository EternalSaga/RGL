#include "Entity.hpp"
#include <spdlog/common.h>



#include <cassert>
#include "EnttRegistry.hpp"

#include "DataPipeline.hpp"


#include "EnTTRelationship.hpp"

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/string_cast.hpp>


#include "RenderQueue.hpp"
namespace RGL {
namespace glcore {



CommonRenderEntity::CommonRenderEntity(glm::vec3 position, float angleX, float angleY, float angleZ, glm::vec3 scale) : entity(singleReg->create()) {
    singleReg->emplace<Transform>(entity, position, glm::vec3(angleX,angleY,angleZ), scale);
}



RenderQueues CommonRenderEntity::renderQueues{};

void CommonRenderEntity::update() {
    
    updateTransforms();

	
	RenderQueueSystem::populateRenderqueues(renderQueues);

	RenderQueueSystem::processDisableCullingQueue(renderQueues.disableCullingQueue);
	RenderQueueSystem::processOpaqueQueue(renderQueues.opaqueQueue);
	RenderQueueSystem::processTransparentQueue(renderQueues.transparentQueue);
	
}
using namespace entt::literals;






}  // namespace glcore
}  // namespace RGL
