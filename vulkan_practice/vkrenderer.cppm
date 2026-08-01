module;
#include <entt/entt.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/glm.hpp>
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <ranges>

export module vk_renderer;

import rlstd;
import rllogger;

import vulkan_utils;
import vulkan_window;
import vk_device;
import vk_swapchain;
import vk_resources;
import types_generated;
import shader;
import vkerror;

namespace RGL {

export class Renderer;

export class Renderer {
public:
    Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
               entt::registry& registry)
        : vkDevice(vkDevice),
          vkSwapchainPtr(&vkSwapchain),
          resourcesPtr(&vkResources),
          registry(&registry),
          graphicsQueue(vkDevice.getGraphicsQueue()),
          graphicsCommandPool(vkDevice.getGraphicsCommandPool()),
          cubePipeline(createRenderPipeline(vkDevice.getDevice(), vkResources.getDescriptorHeap().getDescriptorSize()))
    {
        heapAddress = vkResources.getDescriptorHeap().getGpuAddress();
        createSyncObjects();
        allocateCommandBuffers();
    }

    RGL::Swapchain& getSwapchainRef() const { return *vkSwapchainPtr; }
    RGL::Resources& getResourcesRef() const { return *resourcesPtr; }
    void              updateDrawArgs(const glm::mat4& mvp) { drawArgs.mvp = mvp; }
    void              waitForIdle() { vkDevice.getDevice().waitIdle(); }
    [[nodiscard]] std::expected<void, Error> drawFrame();

private:
    static RGL::RenderPipeline createRenderPipeline(vk::raii::Device& device, uint32_t descriptorSize) {
        auto meshResult = RGL::loadShaderSpv("generated_shaders/cube.mesh.spv");
        if (!meshResult) throw std::runtime_error(meshResult.error().describe());
        auto fragResult = RGL::loadShaderSpv("generated_shaders/cube.frag.spv");
        if (!fragResult) throw std::runtime_error(fragResult.error().describe());

        RGL::ShaderEffect cubeEffect;
        cubeEffect.meshStageSpv = std::move(*meshResult);
        cubeEffect.fragStageSpv = std::move(*fragResult);
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

        std::ranges::for_each(std::views::iota(0, MAX_FRAMES_IN_FLIGHT),
            [&](int) {
                imageAvailableSemaphores.emplace_back(vkDevice.getDevice(), semaphoreInfo);
                renderFinishedSemaphores.emplace_back(vkDevice.getDevice(), semaphoreInfo);
                inFlightFences.emplace_back(vkDevice.getDevice(), fenceInfo);
                presentFences.emplace_back(vkDevice.getDevice(), presentFenceInfo);
            });
    }

    void allocateCommandBuffers() {
        vk::CommandBufferAllocateInfo allocInfoCmd{
            *graphicsCommandPool, vk::CommandBufferLevel::ePrimary,
            static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
        commandBuffers = vkDevice.getDevice().allocateCommandBuffers(allocInfoCmd);
    }

    RGL::DeviceContext& vkDevice;
    RGL::Swapchain*  vkSwapchainPtr;
    RGL::Resources*  resourcesPtr;
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

std::expected<void, Error> Renderer::drawFrame() {
    auto& device = vkDevice.getDevice();

    (void)device.waitForFences(
        {*inFlightFences[currentFrame], *presentFences[currentFrame]},
        VK_TRUE, std::numeric_limits<uint64_t>::max());
    device.resetFences({*inFlightFences[currentFrame], *presentFences[currentFrame]});

    auto [acquireResult, imageIndex] = vkSwapchainPtr->getSwapchain().acquireNextImage(
        std::numeric_limits<uint64_t>::max(),
        *imageAvailableSemaphores[currentFrame], nullptr);

    if (acquireResult == vk::Result::eErrorOutOfDateKHR) {
        vkSwapchainPtr->recreate();
        resourcesPtr->recreateDepthImage(vkSwapchainPtr->getExtent());
        return {};
    }
    if (acquireResult != vk::Result::eSuccess &&
        acquireResult != vk::Result::eSuboptimalKHR) {
        return std::unexpected(Error{ErrorCode::SwapchainAcquireFailed,
            "Failed to acquire swapchain image!"});
    }

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

    vk::ImageMemoryBarrier2 depthToDrawBarrier = RGL::createImageBarrier(
        *resourcesPtr->getDepthImage(), vk::ImageLayout::eUndefined,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageAspectFlagBits::eDepth,
        vk::PipelineStageFlagBits2::eTopOfPipe,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests,
        vk::AccessFlagBits2::eNone,
        vk::AccessFlagBits2::eDepthStencilAttachmentRead |
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

    std::array<vk::ImageMemoryBarrier2, 2> drawBarriers = {colorToDrawBarrier, depthToDrawBarrier};
    vk::DependencyInfo drawDepInfo;
    drawDepInfo.setImageMemoryBarriers(drawBarriers);
    commandBuffers[currentFrame].pipelineBarrier2(drawDepInfo);

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

    std::array<vk::PipelineStageFlags, 1> waitStages = {vk::PipelineStageFlagBits::eColorAttachmentOutput};
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
    auto presentResult = graphicsQueue.presentKHR(presentInfo);

    if (presentResult == vk::Result::eErrorOutOfDateKHR ||
        presentResult == vk::Result::eSuboptimalKHR) {
        vkSwapchainPtr->recreate();
        resourcesPtr->recreateDepthImage(vkSwapchainPtr->getExtent());
    }

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    return {};
}

}

