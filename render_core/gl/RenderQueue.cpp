#include "RenderQueue.hpp"
#include <entt/core/fwd.hpp>
#include <entt/entity/entity.hpp>
#include <entt/entity/fwd.hpp>
#include "EnttRegistry.hpp"
#include "Entity.hpp"
#include "EnTTRelationship.hpp"
#include "CameraECS.hpp"
#include "DataPipeline.hpp"


#include <entt/core/hashed_string.hpp>
using namespace entt::literals;
#include "Mesh.hpp"
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>
#include "UBO.hpp"

#include "Material.hpp"

namespace RGL {
using namespace glcore;


void renderSingleEntity(entt::entity entity, entt::registry* reg, bool isSkybox = false) {



    bool hasUBOs = reg->all_of<UBOs>(entity);
    bool hasSimpleUniforms = reg->all_of<DiscreteUniforms>(entity) && !hasUBOs; // 确保不与UBOs冲突

    const auto& vertArray = reg->get<VertArrayComponent>(entity);
    auto shader = reg->get<ShaderRef>(entity);
    auto& samplers = reg->get<SamplerCreater::Samplers>(entity);
    const auto& transform = reg->get<Transform>(entity); // 需要 Transform 来计算 MVP

    ScopeShader scopeShader(*shader);

    // 更新UBO / Uniforms
    // 这部分逻辑可能需要从之前的 modelSystemUBO/Simple 迁移并针对单个实体
    const CameraProjection proj = reg->ctx().get<CameraProjection>("CameraProjection"_hs);
    glm::mat4 viewMatrix = proj.viewMat;
    if (isSkybox) {
        viewMatrix = glm::mat4(glm::mat3(proj.viewMat)); // 移除位移
    }
    const glm::mat4 MVP = proj.projMat * viewMatrix * transform.modelMatrix;

    bool hasPbrComponents = reg->all_of<PBRComponent>(entity);

    if (hasUBOs) {
        auto& ubos = reg->get<UBOs>(entity);
        // 更新Transform UBO
        auto transUbo = (*ubos)["Transforms"]; 
        if(transUbo) {
            const glm::mat3 inverseModelMatrix = glm::transpose(glm::inverse(glm::mat3(transform.modelMatrix)));
            transUbo->updateCpuUbo("modelMatrix", transform.modelMatrix);
            transUbo->updateCpuUbo("MVP", MVP);
            transUbo->updateCpuUbo("inverseModelMatrix", glm::mat4(inverseModelMatrix));
        }

        if (hasPbrComponents) {
            const auto& pbrComponent = reg->get<PBRComponent>(entity);
            const auto pbrUbo = (*ubos)["pbrUniformBlock"]; 
            if (pbrUbo) {
                pbrUbo->updateCpuUbo("baseColor", pbrComponent.baseColorFactor);
                pbrUbo->updateCpuUbo("metallicFactor", pbrComponent.metallicFactor);
                pbrUbo->updateCpuUbo("roughnessFactor", pbrComponent.roughnessFactor);
                pbrUbo->updateCpuUbo("u_hasBaseColorTexture",1); 
                pbrUbo->updateCpuUbo("u_hasSpecularTexture",  0);
            }
            else {
                auto logger = RLLogger::getInstance();
                logger->warn("UBO CPU for pbrUniformBlock not found,entity {} has no pbrUniformBlock", entt::to_integral(entity));
            }

        // 设置所有UBO
        for (auto& [blockName, ubo] : *ubos) {
            ubo->setUniform();
        }}
    } else if (hasSimpleUniforms) {
        auto& discreteUniforms = reg->get<DiscreteUniforms>(entity);
        discreteUniforms["MVP"] = MVP; // 假设简单物体也需要MVP
        // 可能还需要像 cameraPos 这样的 uniform
        const glm::vec3 camPosition = reg->ctx().get<glm::vec3>("cameraPos"_hs);
        discreteUniforms["cameraPos"] = camPosition; // 示例
        updateAllUniforms(shader, discreteUniforms);
    } else {
         // 可能有些实体既没有UBOs也没有DiscreteUniforms（例如纯粹的调试物体或特殊情况）
         // 或者这是一个逻辑错误，需要日志记录
    }


    glCall(glBindVertexArray, *vertArray.vao);
    SamplerCreater::UseTextures(samplers);
    for (const auto& sampler : samplers) {
        shader->setUniform(sampler.samplerName, sampler.textureUnit);
    }

    RLLogger::getInstance()->log_if(spdlog::level::err, !glCall(glIsVertexArray,*(vertArray.vao)), "Mesh vao is not valid for entity {}", entt::to_integral(entity));
    glCall(glDrawElements, GL_TRIANGLES, vertArray.vertCount, GL_UNSIGNED_INT, reinterpret_cast<void*>(vertArray.idxOffset));

    SamplerCreater::DisableTextures(samplers);
    glCall(glBindVertexArray, 0);
}

void RenderQueues::clear() {
    opaqueQueue.clear();
    transparentQueue.clear();
    shadowQueue.clear();
    skyboxQueue.clear();
    uiQueue.clear();
    shadowCasterQueue.clear();
    shadowReceiverQueue.clear();

}
void RenderQueueSystem::populateRenderqueues(RenderQueues& queues) {
    auto singleReg = EnttReg::getPrimaryRegistry();
    queues.clear();

    auto renderableView = singleReg->view<Transform, VertArrayComponent, ShaderRef,RenderTags::Renderable>();


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
            return glm::distance2(posA, cameraPos) > glm::distance2(posB, cameraPos); // 从远到近
        });
    }
}



    void RenderQueueSystem::processOpaqueQueue(const std::vector<entt::entity>& queue){
    auto singleReg = EnttReg::getPrimaryRegistry();
        glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE); // 允许写入深度缓冲
    glDisable(GL_BLEND);

    for (entt::entity entity : queue) {
        renderSingleEntity(entity, singleReg);
    }
    }
    void RenderQueueSystem::processTransparentQueue(const std::vector<entt::entity>& queue){
    auto singleReg = EnttReg::getPrimaryRegistry();
        // 设置半透明物体通用渲染状态
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE); // 通常禁止写入深度缓冲，避免物体被自身遮挡或后方物体遮挡前方透明部分
                          // (或者根据具体需求，有时可能需要写入)
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); // 标准Alpha混合

    for (entt::entity entity : queue) {
        renderSingleEntity(entity, singleReg);
    }
    glDepthMask(GL_TRUE); // 恢复深度写入
    glDisable(GL_BLEND);
    }
    void RenderQueueSystem::processSkyboxQueue(const std::vector<entt::entity>& queue){
    auto singleReg = EnttReg::getPrimaryRegistry();
    }

    void RenderQueueSystem::updatePBOUniforms() {
        auto singleReg = EnttReg::getPrimaryRegistry();
        auto viewForPbo = singleReg->view<UBOs,ShaderRef,PBRComponent>();

        viewForPbo.each([&](auto entity, UBOs& ubos, ShaderRef& shaderRef, PBRComponent& pbrComponent) {
            auto it = ubos->find("pbrUniformBlock");
            if (it != ubos->end()) {
                if (!pbrComponent.isEmpty) {
                it->second->updateCpuUbo("baseColor", pbrComponent.baseColorFactor);
                it->second->updateCpuUbo("metallicFactor", pbrComponent.metallicFactor);
                it->second->updateCpuUbo("roughnessFactor", pbrComponent.roughnessFactor);
                it->second->updateCpuUbo("u_hasBaseColorTexture", 1);
                it->second->updateCpuUbo("u_hasSpecularTexture", 0);


                }else {
                    auto logger = RLLogger::getInstance();
                    logger->error("PBR component is empty for entity {}", entt::to_integral(entity));
                }

                }
                
            }
        );

    }
    }  // namespace RGL