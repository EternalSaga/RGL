#pragma once
#include <entt/entity/fwd.hpp>
#include <entt/entt.hpp>
#include <vector>
#include "EnttRegistry.hpp"
namespace RGL {

namespace RenderTags {

struct Opaque {};
struct Transparent {};
struct Shadow {};
struct Skybox {};
struct UIElement {};
struct ShadowCaster {};
struct ShadowReceiver {};
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
    void clear() {
        opaqueQueue.clear();
        transparentQueue.clear();
        shadowQueue.clear();
        skyboxQueue.clear();
        uiQueue.clear();
        shadowCasterQueue.clear();
        shadowReceiverQueue.clear();
    }
};

class RenderQueueSystem {
    public:
    static void populateRenderqueues(RenderQueues& queues){
        auto singleReg = EnttReg::getPrimaryRegistry();
        queues.clear();

        

    }
};

}  // namespace RGL