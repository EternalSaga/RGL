#include "RenderQueue.hpp"
#include <glad/glad.h>
#include <entt/core/fwd.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include "EnttRegistry.hpp"
#include "Entity.hpp"
#include "EnTTRelationship.hpp"
#include "CameraECS.hpp"
#include "DataPipeline.hpp"
#include "GLCheckError.hpp"
#include "GLObj.hpp"
#include "Helpers.hpp"
#include "Shader.hpp"
#include "rllogger.hpp"

#include <entt/core/hashed_string.hpp>
using namespace entt::literals;
#include "Mesh.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>
#include "UBO.hpp"

#include "Material.hpp"
#include "InstanceComponent.hpp"
namespace RGL {
using namespace glcore;
void executeSingleDraw(entt::entity entity, entt::registry& reg) {
    const auto& vertArray = reg.get<VertArrayComponent>(entity);

    const auto& shader = reg.get<ShaderRef>(entity);


    ScopeShader shaderScope(*shader);
    VAOScope vaoScope(*vertArray.vao);
    glCall(glDrawElements, GL_TRIANGLES, vertArray.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(vertArray.idxOffset));
}
void executeDrawInstanced(entt::entity entity, entt::registry& reg) {
    const auto& vertArray = reg.get<VertArrayComponent>(entity);

    const auto& shader = reg.get<ShaderRef>(entity);
    const auto& instanceComponent = reg.get<RenderTags::Instanced>(entity);
    ScopeShader shaderScope(*shader);
    VAOScope vaoScope(*vertArray.vao);
    glCall(glDrawElementsInstanced, GL_TRIANGLES, vertArray.vertCount, GL_UNSIGNED_INT,
           reinterpret_cast<void*>(vertArray.idxOffset), instanceComponent.instanceCount);
}



void RenderQueues::clear() {
    opaqueQueue.clear();
    transparentQueue.clear();
    shadowQueue.clear();
    skyboxQueue.clear();
    uiQueue.clear();
    shadowCasterQueue.clear();
    shadowReceiverQueue.clear();
    disableCullingQueue.clear();
    instanceQueue.clear();
}
void RenderQueueSystem::populateRenderqueues(RenderQueues& queues) {
    auto singleReg = EnttReg::getPrimaryRegistry();
    queues.clear();

    auto renderableView = singleReg->view<Transform, VertArrayComponent, ShaderRef, RenderTags::Renderable>();

    renderableView.each([&](entt::entity entity, Transform& transform, VertArrayComponent& vertArray,
			    ShaderRef& shaderRef, RenderTags::Renderable& renderTag) {
	if (singleReg->all_of<RenderTags::Opaque>(entity)) {
	    queues.opaqueQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::Transparent>(entity)) {
	    queues.transparentQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::Shadow>(entity)) {
	    queues.shadowQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::Skybox>(entity)) {
	    queues.skyboxQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::UIElement>(entity)) {
	    queues.uiQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::ShadowCaster>(entity)) {
	    queues.shadowCasterQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::ShadowReceiver>(entity)) {
	    queues.shadowReceiverQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::DisableCulling>(entity)) {
	    queues.disableCullingQueue.push_back(entity);
	} else if (singleReg->all_of<RenderTags::Instanced>(entity)) {
	    queues.instanceQueue.push_back(entity);
	}
    });
    // 对透明队列进行排序，通常是根据距离摄像机的距离从远到近排序
    // Transform内的modelMatrix在执行updateTransforms()之后会更新成世界坐标系
    if (!queues.transparentQueue.empty()) {
	const glm::vec3 cameraPos = singleReg->ctx().get<glm::vec3>("cameraPos"_hs);
	std::sort(queues.transparentQueue.begin(), queues.transparentQueue.end(),
	    [&](entt::entity a, entt::entity b) {
		const auto& transformA = singleReg->get<Transform>(a);
		const auto& transformB = singleReg->get<Transform>(b);

		glm::vec3 posA = glm::vec3(transformA.modelMatrix[3]);
		glm::vec3 posB = glm::vec3(transformB.modelMatrix[3]);
		return glm::distance2(posA, cameraPos) > glm::distance2(posB, cameraPos);  // 从远到近
	    });
    }
}

void RenderQueueSystem::processOpaqueQueue(const std::vector<entt::entity>& queue) {
    auto singleReg = EnttReg::getPrimaryRegistry();
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);  // 允许写入深度缓冲
    glDisable(GL_BLEND);

    for (entt::entity entity : queue) {
        executeSingleDraw(entity, *singleReg);
    }
}
void RenderQueueSystem::processTransparentQueue(const std::vector<entt::entity>& queue) {
    auto singleReg = EnttReg::getPrimaryRegistry();
    // 设置半透明物体通用渲染状态
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);  // 通常禁止写入深度缓冲，避免物体被自身遮挡或后方物体遮挡前方透明部分
			    // (或者根据具体需求，有时可能需要写入)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);	// 标准Alpha混合

    for (entt::entity entity : queue) {
	executeSingleDraw(entity, *singleReg);
    }
    glDepthMask(GL_TRUE);  // 恢复深度写入
    glDisable(GL_BLEND);
}
void RenderQueueSystem::processSkyboxQueue(const std::vector<entt::entity>& queue) {
    auto singleReg = EnttReg::getPrimaryRegistry();
}

void RenderQueueSystem::processDisableCullingQueue(const std::vector<entt::entity>& queue) {
    auto singleReg = EnttReg::getPrimaryRegistry();
    glCall(glDisable, GL_CULL_FACE);
    for (entt::entity entity : queue) {
	    executeSingleDraw(entity, *singleReg);
    }
    glCall(glEnable, GL_CULL_FACE);
}


void RenderQueueSystem::processInstanceQueue(const std::vector<entt::entity>& queue){
	auto singleReg = EnttReg::getPrimaryRegistry();
    glCall(glDisable, GL_CULL_FACE);
    for (entt::entity entity : queue) {
	    executeDrawInstanced(entity, *singleReg);
    }
    glCall(glEnable, GL_CULL_FACE);
}


}  // namespace RGL