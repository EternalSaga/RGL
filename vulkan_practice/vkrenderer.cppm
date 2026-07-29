module;
#include <rllogger.hpp>
#include <entt/entt.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>

export module vk_renderer;

import vulkan_utils;
import vulkan_window;
import vk_device;
import vk_swapchain;
import vk_resources;
import cube;
import shader;

namespace RGL {

export class VkRenderer;

export class VkRenderer {
public:
    VkRenderer(RGL::VkDevice& vkDevice, RGL::VkSwapchain& vkSwapchain, RGL::VkResources& vkResources,
               entt::registry& registry)
        : vkDevice_(vkDevice),
          vkSwapchainPtr_(&vkSwapchain),
          resourcesPtr_(&vkResources),
          windowPtr_(nullptr),
           registry_(&registry),
          graphicsQueue_(vkDevice_.getGraphicsQueue()),
          graphicsCommandPool_(vkDevice_.getGraphicsCommandPool()),
           cubePipeline(createRenderPipeline(vkDevice_.getDevice(), vkResources.getDescriptorHeap().getDescriptorSize())),
           heapAddress_(vkResources.getDescriptorHeap().getGpuAddress())
    {
        createSyncObjects();
        allocateCommandBuffers();
        initDepthBarrier();
    }

    RGL::VkSwapchain& getSwapchainRef() const { return *vkSwapchainPtr_; }
    RGL::VkResources& getResourcesRef() const { return *resourcesPtr_; }
    RGL::SDLWindow*   getWindowRef()    const { return windowPtr_; }
    void              updateDrawArgs(const glm::mat4& mvp) { drawArgs_.mvp = mvp; }
    void              waitForIdle() { vkDevice_.getDevice().waitIdle(); }
    void drawFrame();
    void setWindow(SDLWindow* w) { windowPtr_ = w; }
    void recreateDepthBarrier();

private:
   static RGL::RenderPipeline createRenderPipeline(vk::raii::Device& device, uint32_t descriptorSize) {
        RGL::ShaderEffect cubeEffect;
        cubeEffect.meshStageSpv = RGL::loadShaderSpv("generated_shaders/cube.mesh.spv");
        cubeEffect.fragStageSpv = RGL::loadShaderSpv("generated_shaders/cube.frag.spv");
        return RGL::RenderPipeline(device, cubeEffect, descriptorSize);
    }

    void createSyncObjects() {
        vk::SemaphoreCreateInfo semaphoreInfo{};
        vk::FenceCreateInfo fenceInfo{vk::FenceCreateFlagBits::eSignaled};
        vk::FenceCreateInfo presentFenceInfo{vk::FenceCreateFlagBits::eSignaled};

        imageAvailableSemaphores_.reserve(MAX_FRAMES_IN_FLIGHT);
        renderFinishedSemaphores_.reserve(MAX_FRAMES_IN_FLIGHT);
        inFlightFences_.reserve(MAX_FRAMES_IN_FLIGHT);
        presentFences_.reserve(MAX_FRAMES_IN_FLIGHT);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            imageAvailableSemaphores_.emplace_back(vkDevice_.getDevice(), semaphoreInfo);
            renderFinishedSemaphores_.emplace_back(vkDevice_.getDevice(), semaphoreInfo);
            inFlightFences_.emplace_back(vkDevice_.getDevice(), fenceInfo);
            presentFences_.emplace_back(vkDevice_.getDevice(), presentFenceInfo);
        }
    }

    void allocateCommandBuffers() {
        vk::CommandBufferAllocateInfo allocInfoCmd{
            *graphicsCommandPool_, vk::CommandBufferLevel::ePrimary,
            static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
        commandBuffers_ = vkDevice_.getDevice().allocateCommandBuffers(allocInfoCmd);
    }

    void initDepthBarrier() {
        commandBuffers_[0].begin(vk::CommandBufferBeginInfo{
            vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        vk::ImageMemoryBarrier2 depthBarrier = RGL::createImageBarrier(
            *resourcesPtr_->getDepthImage(), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eDepthStencilAttachmentOptimal,
            vk::ImageAspectFlagBits::eDepth, vk::PipelineStageFlagBits2::eTopOfPipe,
            vk::PipelineStageFlagBits2::eEarlyFragmentTests,
            vk::AccessFlagBits2::eNone,
            vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

        vk::DependencyInfo depthDepInfo;
        depthDepInfo.setImageMemoryBarriers(depthBarrier);
        commandBuffers_[0].pipelineBarrier2(depthDepInfo);
        commandBuffers_[0].end();

        vk::SubmitInfo initSubmit{};
        initSubmit.setCommandBuffers(*commandBuffers_[0]);
        graphicsQueue_.submit(initSubmit, nullptr);
        graphicsQueue_.waitIdle();
        commandBuffers_[0].reset();
    }

    RGL::VkDevice& vkDevice_;
    RGL::VkSwapchain*  vkSwapchainPtr_;
    RGL::VkResources*  resourcesPtr_;
    RGL::SDLWindow*    windowPtr_;
    entt::registry*    registry_;
    vk::Queue graphicsQueue_;
    const vk::raii::CommandPool& graphicsCommandPool_;
    RGL::RenderPipeline cubePipeline;
    RGL::DrawArgs drawArgs_{};
    vk::DeviceAddress heapAddress_ = 0;

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    uint32_t currentFrame_ = 0;
    std::vector<vk::raii::Semaphore> imageAvailableSemaphores_;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphores_;
    std::vector<vk::raii::Fence> inFlightFences_;
    std::vector<vk::raii::Fence> presentFences_;
    std::vector<vk::raii::CommandBuffer> commandBuffers_;
};

void VkRenderer::recreateDepthBarrier() {
    commandBuffers_[0].reset();
    commandBuffers_[0].begin(vk::CommandBufferBeginInfo{
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    vk::ImageMemoryBarrier2 depthBarrier = RGL::createImageBarrier(
        *resourcesPtr_->getDepthImage(), vk::ImageLayout::eUndefined,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageAspectFlagBits::eDepth, vk::PipelineStageFlagBits2::eTopOfPipe,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests,
        vk::AccessFlagBits2::eNone,
        vk::AccessFlagBits2::eDepthStencilAttachmentRead |
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

    vk::DependencyInfo depthDepInfo;
    depthDepInfo.setImageMemoryBarriers(depthBarrier);
    commandBuffers_[0].pipelineBarrier2(depthDepInfo);
    commandBuffers_[0].end();

    vk::SubmitInfo initSubmit{};
    initSubmit.setCommandBuffers(*commandBuffers_[0]);
    graphicsQueue_.submit(initSubmit, nullptr);
    graphicsQueue_.waitIdle();
    commandBuffers_[0].reset();
}

void VkRenderer::drawFrame() {
    auto& device = vkDevice_.getDevice();

    (void)device.waitForFences(
        {*inFlightFences_[currentFrame_], *presentFences_[currentFrame_]},
        VK_TRUE, std::numeric_limits<uint64_t>::max());
    device.resetFences({*inFlightFences_[currentFrame_], *presentFences_[currentFrame_]});

    auto [acquireResult, imageIndex] = vkSwapchainPtr_->getSwapchain().acquireNextImage(
        std::numeric_limits<uint64_t>::max(),
        *imageAvailableSemaphores_[currentFrame_], nullptr);

    commandBuffers_[currentFrame_].reset();
    commandBuffers_[currentFrame_].begin(
        vk::CommandBufferBeginInfo{});

    auto& swapchainImages = vkSwapchainPtr_->getImages();
    auto& swapchainImageViews = vkSwapchainPtr_->getImageViews();

    vk::ImageMemoryBarrier2 colorToDrawBarrier = RGL::createImageBarrier(
        swapchainImages[imageIndex], vk::ImageLayout::eUndefined,
        vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageAspectFlagBits::eColor,
        vk::PipelineStageFlagBits2::eTopOfPipe,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::AccessFlagBits2::eNone,
        vk::AccessFlagBits2::eColorAttachmentWrite);
    vk::DependencyInfo colorToDrawDepInfo;
    colorToDrawDepInfo.setImageMemoryBarriers(colorToDrawBarrier);
    commandBuffers_[currentFrame_].pipelineBarrier2(colorToDrawDepInfo);

    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.imageView = *swapchainImageViews[imageIndex];
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue = vk::ClearValue{
        std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f}};

    vk::RenderingAttachmentInfo depthAttachment{};
    depthAttachment.imageView = *resourcesPtr_->getDepthImageView();
    depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    depthAttachment.storeOp = vk::AttachmentStoreOp::eDontCare;
    depthAttachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = vk::Rect2D({0, 0}, vkSwapchainPtr_->getExtent());
    renderingInfo.layerCount = 1;
    renderingInfo.setColorAttachments(colorAttachment);
    renderingInfo.setPDepthAttachment(&depthAttachment);

    vk::MemoryBarrier2 heapBarrier{};
    heapBarrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
    heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT;
    heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
    heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
    vk::DependencyInfo heapDepInfo{};
    heapDepInfo.setMemoryBarriers(heapBarrier);
    commandBuffers_[currentFrame_].pipelineBarrier2(heapDepInfo);

    vk::DeviceAddressRangeEXT heapRange{};
    heapRange.address = heapAddress_;
    heapRange.size = resourcesPtr_->getDescriptorHeap().getDescriptorSize() * resourcesPtr_->getDescriptorHeap().getCapacity();
    commandBuffers_[currentFrame_].bindResourceHeapEXT(vk::BindHeapInfoEXT(
        heapRange,
        resourcesPtr_->getReservedRangeOffset(),
        resourcesPtr_->getReservedRangeSize()
    ));

    commandBuffers_[currentFrame_].pushDataEXT(vk::PushDataInfoEXT(
        0, vk::HostAddressRangeConstEXT(&drawArgs_, sizeof(RGL::DrawArgs))));

    commandBuffers_[currentFrame_].beginRendering(renderingInfo);

    cubePipeline.recordRenderCommands(commandBuffers_[currentFrame_],
                                       vkSwapchainPtr_->getExtent().width,
                                       vkSwapchainPtr_->getExtent().height);
    commandBuffers_[currentFrame_].drawMeshTasksEXT(1, 1, 1);
    commandBuffers_[currentFrame_].endRendering();

    vk::ImageMemoryBarrier2 colorToPresentBarrier = RGL::createImageBarrier(
        swapchainImages[imageIndex], vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR, vk::ImageAspectFlagBits::eColor,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::PipelineStageFlagBits2::eBottomOfPipe,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::AccessFlagBits2::eNone);
    vk::DependencyInfo colorToPresentDepInfo;
    colorToPresentDepInfo.setImageMemoryBarriers(colorToPresentBarrier);
    commandBuffers_[currentFrame_].pipelineBarrier2(colorToPresentDepInfo);
    commandBuffers_[currentFrame_].end();

    vk::PipelineStageFlags waitStages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
    vk::SubmitInfo submitInfo{};
    submitInfo.setWaitSemaphores(*imageAvailableSemaphores_[currentFrame_]);
    submitInfo.setWaitDstStageMask(waitStages);
    submitInfo.setCommandBuffers(*commandBuffers_[currentFrame_]);
    submitInfo.setSignalSemaphores(*renderFinishedSemaphores_[currentFrame_]);
    graphicsQueue_.submit(submitInfo, *inFlightFences_[currentFrame_]);

    vk::SwapchainPresentFenceInfoEXT presentFenceInfoEXT{};
    presentFenceInfoEXT.setFences(*presentFences_[currentFrame_]);
    vk::PresentInfoKHR presentInfo{};
    presentInfo.setPNext(&presentFenceInfoEXT);
    presentInfo.setWaitSemaphores(*renderFinishedSemaphores_[currentFrame_]);
    presentInfo.setSwapchains(*vkSwapchainPtr_->getSwapchain());
    presentInfo.setImageIndices(imageIndex);
    (void)graphicsQueue_.presentKHR(presentInfo);

    currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
}

}
