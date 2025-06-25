#pragma once
#include <cstddef>
#include <entt/entity/fwd.hpp>
#include <entt/entt.hpp>
#include <vector>

namespace RGL {

namespace RenderTags {
struct Renderable {
    bool visible = true;
};
struct Opaque {};
struct Transparent {};
struct Shadow {};
struct Skybox {};
struct UIElement {};
struct ShadowCaster {};
struct ShadowReceiver {};
struct StencilOutlined {};
struct DisableCulling {};
struct Instanced{size_t instanceCount = 0;};
}  // namespace RenderTags

struct RenderQueues {
    std::vector<entt::entity> opaqueQueue;
    std::vector<entt::entity> transparentQueue;
    std::vector<entt::entity> shadowQueue;
    std::vector<entt::entity> skyboxQueue;
    std::vector<entt::entity> uiQueue;
    std::vector<entt::entity> shadowCasterQueue;
    std::vector<entt::entity> shadowReceiverQueue;
    std::vector<entt::entity> disableCullingQueue;
    std::vector<entt::entity> instanceQueue; 
    RenderQueues() = default;
    RenderQueues(const RenderQueues&) = delete;
    RenderQueues& operator=(const RenderQueues&) = delete;
    RenderQueues(RenderQueues&&) = default;
    RenderQueues& operator=(RenderQueues&&) = default;
    ~RenderQueues() = default;
    void clear();
};

class RenderQueueSystem {
    public:
    static void populateRenderqueues(RenderQueues& queues);
    static void processOpaqueQueue(const std::vector<entt::entity>& queue);
    static void processTransparentQueue(const std::vector<entt::entity>& queue);
    static void processDisableCullingQueue(const std::vector<entt::entity>& queue);
    static void processSkyboxQueue(const std::vector<entt::entity>& queue);


    static void processInstanceQueue(const std::vector<entt::entity>& queue);
};

}  // namespace RGL