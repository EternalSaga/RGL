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

export class Renderer;

export class Renderer {
public:
    Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
               entt::registry& registry)
        : vkDevice(vkDevice),
          vkSwapchainPtr(&vkSwapchain),
          resourcesPtr(&vkResources),
          windowPtr(nullptr),
           registry(&registry),
          graphicsQueue(vkDevice.getGraphicsQueue()),
          graphicsCommandPool(vkDevice.getGraphicsCommandPool()),
           cubePipeline(createRenderPipeline(vkDevice.getDevice(), vkResources.getDescriptorHeap().getDescriptorSize())),
           heapAddress(vkResources.getDescriptorHeap().getGpuAddress())
    {
        createSyncObjects();
        allocateCommandBuffers();
        initDepthBarrier();
    }

    RGL::Swapchain& getSwapchainRef() const { return *vkSwapchainPtr; }
    RGL::Resources& getResourcesRef() const { return *resourcesPtr; }
    RGL::SDLWindow*   getWindowRef()    const { return windowPtr; }
    void              updateDrawArgs(const glm::mat4& mvp) { drawArgs.mvp = mvp; }
    void              waitForIdle() { vkDevice.getDevice().waitIdle(); }
    void drawFrame();
    void setWindow(SDLWindow* w) { windowPtr = w; }
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

        imageAvailableSemaphores.reserve(MAX_FRAMES_IN_FLIGHT);
        renderFinishedSemaphores.reserve(MAX_FRAMES_IN_FLIGHT);
        inFlightFences.reserve(MAX_FRAMES_IN_FLIGHT);
        presentFences.reserve(MAX_FRAMES_IN_FLIGHT);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            imageAvailableSemaphores.emplace_back(vkDevice.getDevice(), semaphoreInfo);
            renderFinishedSemaphores.emplace_back(vkDevice.getDevice(), semaphoreInfo);
            inFlightFences.emplace_back(vkDevice.getDevice(), fenceInfo);
            presentFences.emplace_back(vkDevice.getDevice(), presentFenceInfo);
        }
    }

    void allocateCommandBuffers() {
        vk::CommandBufferAllocateInfo allocInfoCmd{
            *graphicsCommandPool, vk::CommandBufferLevel::ePrimary,
            static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
        commandBuffers = vkDevice.getDevice().allocateCommandBuffers(allocInfoCmd);
    }

    void initDepthBarrier() {
        commandBuffers[0].begin(vk::CommandBufferBeginInfo{
            vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

        vk::ImageMemoryBarrier2 depthBarrier = RGL::createImageBarrier(
            *resourcesPtr->getDepthImage(), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eDepthStencilAttachmentOptimal,
            vk::ImageAspectFlagBits::eDepth, vk::PipelineStageFlagBits2::eTopOfPipe,
            vk::PipelineStageFlagBits2::eEarlyFragmentTests,
            vk::AccessFlagBits2::eNone,
            vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

        vk::DependencyInfo depthDepInfo;
        depthDepInfo.setImageMemoryBarriers(depthBarrier);
        commandBuffers[0].pipelineBarrier2(depthDepInfo);
        commandBuffers[0].end();

        vk::SubmitInfo initSubmit{};
        initSubmit.setCommandBuffers(*commandBuffers[0]);
        graphicsQueue.submit(initSubmit, nullptr);
        graphicsQueue.waitIdle();
        commandBuffers[0].reset();
    }

    RGL::DeviceContext& vkDevice;
    RGL::Swapchain*  vkSwapchainPtr;
    RGL::Resources*  resourcesPtr;
    RGL::SDLWindow*  windowPtr;
    entt::registry*  registry;
    vk::Queue graphicsQueue;
    const vk::raii::CommandPool& graphicsCommandPool;
    RGL::RenderPipeline cubePipeline;
    RGL::DrawArgs drawArgs{};
    vk::DeviceAddress heapAddress = 0;

    static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
    uint32_t currentFrame = 0;
    std::vector<vk::raii::Semaphore> imageAvailableSemaphores;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphores;
    std::vector<vk::raii::Fence> inFlightFences;
    std::vector<vk::raii::Fence> presentFences;
    std::vector<vk::raii::CommandBuffer> commandBuffers;
};

void Renderer::recreateDepthBarrier() {
    commandBuffers[0].reset();
    commandBuffers[0].begin(vk::CommandBufferBeginInfo{
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    vk::ImageMemoryBarrier2 depthBarrier = RGL::createImageBarrier(
        *resourcesPtr->getDepthImage(), vk::ImageLayout::eUndefined,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageAspectFlagBits::eDepth, vk::PipelineStageFlagBits2::eTopOfPipe,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests,
        vk::AccessFlagBits2::eNone,
        vk::AccessFlagBits2::eDepthStencilAttachmentRead |
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

    vk::DependencyInfo depthDepInfo;
    depthDepInfo.setImageMemoryBarriers(depthBarrier);
    commandBuffers[0].pipelineBarrier2(depthDepInfo);
    commandBuffers[0].end();

    vk::SubmitInfo initSubmit{};
    initSubmit.setCommandBuffers(*commandBuffers[0]);
    graphicsQueue.submit(initSubmit, nullptr);
    graphicsQueue.waitIdle();
    commandBuffers[0].reset();
}

void Renderer::drawFrame() {
    // Command recording order is critical:
    // 1. Wait fences → 2. Acquire image → 3. Color barrier → 4. Heap barrier
    // 5. bindResourceHeapEXT → 6. pushDataEXT → 7. beginRendering → 8. bind shaders → 9. draw
    // Steps 4-6 must occur BEFORE beginRendering (HOST stage cannot be used inside render pass)
    auto& device = vkDevice.getDevice();

    (void)device.waitForFences(
        {*inFlightFences[currentFrame], *presentFences[currentFrame]},
        VK_TRUE, std::numeric_limits<uint64_t>::max());
    device.resetFences({*inFlightFences[currentFrame], *presentFences[currentFrame]});

    auto [acquireResult, imageIndex] = vkSwapchainPtr->getSwapchain().acquireNextImage(
        std::numeric_limits<uint64_t>::max(),
        *imageAvailableSemaphores[currentFrame], nullptr);

    commandBuffers[currentFrame].reset();
    commandBuffers[currentFrame].begin(
        vk::CommandBufferBeginInfo{});

    auto& swapchainImages = vkSwapchainPtr->getImages();
    auto& swapchainImageViews = vkSwapchainPtr->getImageViews();

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
    commandBuffers[currentFrame].pipelineBarrier2(colorToDrawDepInfo);

    vk::RenderingAttachmentInfo colorAttachment{};
    colorAttachment.imageView = *swapchainImageViews[imageIndex];
    colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
    colorAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
    colorAttachment.clearValue = vk::ClearValue{
        std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f}};

    vk::RenderingAttachmentInfo depthAttachment{};
    depthAttachment.imageView = *resourcesPtr->getDepthImageView();
    depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
    depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
    depthAttachment.storeOp = vk::AttachmentStoreOp::eDontCare;
    depthAttachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

    vk::RenderingInfo renderingInfo{};
    renderingInfo.renderArea = vk::Rect2D({0, 0}, vkSwapchainPtr->getExtent());
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
    commandBuffers[currentFrame].pipelineBarrier2(heapDepInfo);

    vk::DeviceAddressRangeEXT heapRange{};
    heapRange.address = heapAddress;
    heapRange.size = resourcesPtr->getDescriptorHeap().getDescriptorSize() * resourcesPtr->getDescriptorHeap().getCapacity();
    commandBuffers[currentFrame].bindResourceHeapEXT(vk::BindHeapInfoEXT(
        heapRange,
        resourcesPtr->getReservedRangeOffset(),
        resourcesPtr->getReservedRangeSize()
    ));

    commandBuffers[currentFrame].pushDataEXT(vk::PushDataInfoEXT(
        0, vk::HostAddressRangeConstEXT(&drawArgs, sizeof(RGL::DrawArgs))));

    commandBuffers[currentFrame].beginRendering(renderingInfo);

    cubePipeline.recordRenderCommands(commandBuffers[currentFrame],
                                       vkSwapchainPtr->getExtent().width,
                                       vkSwapchainPtr->getExtent().height);
    commandBuffers[currentFrame].drawMeshTasksEXT(1, 1, 1);
    commandBuffers[currentFrame].endRendering();

    vk::ImageMemoryBarrier2 colorToPresentBarrier = RGL::createImageBarrier(
        swapchainImages[imageIndex], vk::ImageLayout::eColorAttachmentOptimal,
        vk::ImageLayout::ePresentSrcKHR, vk::ImageAspectFlagBits::eColor,
        vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        vk::PipelineStageFlagBits2::eBottomOfPipe,
        vk::AccessFlagBits2::eColorAttachmentWrite,
        vk::AccessFlagBits2::eNone);
    vk::DependencyInfo colorToPresentDepInfo;
    colorToPresentDepInfo.setImageMemoryBarriers(colorToPresentBarrier);
    commandBuffers[currentFrame].pipelineBarrier2(colorToPresentDepInfo);
    commandBuffers[currentFrame].end();

    vk::PipelineStageFlags waitStages[] = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
    vk::SubmitInfo submitInfo{};
    submitInfo.setWaitSemaphores(*imageAvailableSemaphores[currentFrame]);
    submitInfo.setWaitDstStageMask(waitStages);
    submitInfo.setCommandBuffers(*commandBuffers[currentFrame]);
    submitInfo.setSignalSemaphores(*renderFinishedSemaphores[currentFrame]);
    graphicsQueue.submit(submitInfo, *inFlightFences[currentFrame]);

    vk::SwapchainPresentFenceInfoEXT presentFenceInfoEXT{};
    presentFenceInfoEXT.setFences(*presentFences[currentFrame]);
    vk::PresentInfoKHR presentInfo{};
    presentInfo.setPNext(&presentFenceInfoEXT);
    presentInfo.setWaitSemaphores(*renderFinishedSemaphores[currentFrame]);
    presentInfo.setSwapchains(*vkSwapchainPtr->getSwapchain());
    presentInfo.setImageIndices(imageIndex);
    (void)graphicsQueue.presentKHR(presentInfo);

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
}

}
