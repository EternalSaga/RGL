#pragma once
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
}  // namespace RenderTags

struct RenderQueues {
    std::vector<entt::entity> opaqueQueue;
    std::vector<entt::entity> transparentQueue;
    std::vector<entt::entity> shadowQueue;
    std::vector<entt::entity> skyboxQueue;
    std::vector<entt::entity> uiQueue;
    std::vector<entt::entity> shadowCasterQueue;
    std::vector<entt::entity> shadowReceiverQueue;

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
    static void processSkyboxQueue(const std::vector<entt::entity>& queue);
    static void updatePBOUniforms();
};

}  // namespace RGL