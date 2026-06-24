module;
#include <rllogger.hpp>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vector>
#include <cstdint>
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

export class VkRenderer {
public:
    VkRenderer(RGL::VkDevice& vkDevice, RGL::VkSwapchain& vkSwapchain, RGL::VkResources& vkResources)
        : vkDevice_(vkDevice),
          vkSwapchain_(vkSwapchain),
          vkResources_(vkResources),
          graphicsQueue_(vkDevice_.getGraphicsQueue()),
          graphicsCommandPool_(vkDevice_.getGraphicsCommandPool()),
          cubePipeline(createRenderPipeline(vkDevice_.getDevice(), vkResources_.getDescriptorHeap().getDescriptorSize())),
          heapAddress_(vkResources_.getDescriptorHeap().getGpuAddress())
    {
        createSyncObjects();
        allocateCommandBuffers();
        initDepthBarrier();
    }

    void render(RGL::SDLWindow& window) {
        bool shouldClose = false;

        while (!shouldClose) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) {
                    shouldClose = true;
                }
            }

            float time = SDL_GetTicks() / 1000.0f;

            glm::mat4 model = glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.5f, 1.0f, 0.0f));
            glm::mat4 view = glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                                          glm::vec3(0.0f, 1.0f, 0.0f));
            glm::mat4 proj = glm::perspective(glm::radians(45.0f),
                                               (float)vkSwapchain_.getExtent().width / (float)vkSwapchain_.getExtent().height,
                                               0.1f, 10.0f);
            drawArgs_.mvp = proj * view * model;

            drawFrame(window);
        }

        vkDevice_.getDevice().waitIdle();
    }

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
            *vkResources_.getDepthImage(), vk::ImageLayout::eUndefined,
            vk::ImageLayout::eDepthStencilAttachmentOptimal,
            vk::ImageAspectFlagBits::eDepth, vk::PipelineStageFlagBits2::eTopOfPipe,
            vk::PipelineStageFlagBits2::eEarlyFragmentTests,
            vk::AccessFlagBits2::eNone,
            vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

        vk::DependencyInfo depthDepInfo;
        depthDepInfo.setImageMemoryBarriers(depthBarrier);
        commandBuffers_[currentFrame_].pipelineBarrier2(depthDepInfo);
        commandBuffers_[currentFrame_].end();

        vk::SubmitInfo initSubmit{};
        initSubmit.setCommandBuffers(*commandBuffers_[currentFrame_]);
        graphicsQueue_.submit(initSubmit, nullptr);
        graphicsQueue_.waitIdle();
        commandBuffers_[currentFrame_].reset();
    }

    void drawFrame(RGL::SDLWindow& window) {
        auto& device = vkDevice_.getDevice();

        (void)device.waitForFences(
            {*inFlightFences_[currentFrame_], *presentFences_[currentFrame_]},
            VK_TRUE, std::numeric_limits<uint64_t>::max());
        device.resetFences({*inFlightFences_[currentFrame_], *presentFences_[currentFrame_]});

        auto [acquireResult, imageIndex] = vkSwapchain_.getSwapchain().acquireNextImage(
            std::numeric_limits<uint64_t>::max(),
            *imageAvailableSemaphores_[currentFrame_], nullptr);

        commandBuffers_[currentFrame_].reset();
        commandBuffers_[currentFrame_].begin(
            vk::CommandBufferBeginInfo{});

        auto& swapchainImages = vkSwapchain_.getImages();
        auto& swapchainImageViews = vkSwapchain_.getImageViews();

        vk::ImageMemoryBarrier2 colorToDrawBarrier = RGL::createImageBarrier(
            swapchainImages[imageIndex], vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageAspectFlagBits::eColor,
            vk::PipelineStageFlagBits2::eColorAttachmentOutput,
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
        depthAttachment.imageView = *vkResources_.getDepthImageView();
        depthAttachment.imageLayout = vk::ImageLayout::eDepthStencilAttachmentOptimal;
        depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
        depthAttachment.storeOp = vk::AttachmentStoreOp::eDontCare;
        depthAttachment.clearValue = vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

        vk::RenderingInfo renderingInfo{};
        renderingInfo.renderArea = vk::Rect2D({0, 0}, vkSwapchain_.getExtent());
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
        heapRange.size = vkResources_.getDescriptorHeap().getDescriptorSize() * vkResources_.getDescriptorHeap().getCapacity();
        commandBuffers_[currentFrame_].bindResourceHeapEXT(vk::BindHeapInfoEXT(
            heapRange,
            vkResources_.getReservedRangeOffset(),
            vkResources_.getReservedRangeSize()
        ));

        commandBuffers_[currentFrame_].pushDataEXT(vk::PushDataInfoEXT(
            0, vk::HostAddressRangeConstEXT(&drawArgs_, sizeof(RGL::DrawArgs))));

        commandBuffers_[currentFrame_].beginRendering(renderingInfo);

        cubePipeline.recordRenderCommands(commandBuffers_[currentFrame_],
                                           vkSwapchain_.getExtent().width,
                                           vkSwapchain_.getExtent().height);
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
        presentInfo.setSwapchains(*vkSwapchain_.getSwapchain());
        presentInfo.setImageIndices(imageIndex);
        (void)graphicsQueue_.presentKHR(presentInfo);

        currentFrame_ = (currentFrame_ + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    RGL::VkDevice& vkDevice_;
    RGL::VkSwapchain& vkSwapchain_;
    RGL::VkResources& vkResources_;
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

}
