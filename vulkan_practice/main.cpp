#include "vulkan/vulkan.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdint>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <vector>
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
import vulkan_utils;
import vulkan_window;
import queue_family_manager;
import features;
import cube;
import shader;
import VmaRaii;
using namespace RGL;
int main() {
  try {
    vk::raii::Context context;

    SDLWindow window(800, 600, "Vulkan Practice");

    auto [surfaceExtensions, surfaceCount] = window.getSurfaceExtensions();

    auto apiVersion = context.enumerateInstanceVersion();

    vk::ApplicationInfo engineInfo;
    engineInfo.applicationVersion = vk::makeVersion(0, 1, 0);
    engineInfo.apiVersion = apiVersion;
    engineInfo.engineVersion = vk::makeVersion(0, 0, 1);
    engineInfo.pEngineName = "practice";

    std::vector<const char *> layers = {"VK_LAYER_KHRONOS_validation"};
    std::vector<const char *> instanceExtensions = {
        VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};

    for (uint32_t i = 0; i < surfaceCount; i++) {
      instanceExtensions.push_back(surfaceExtensions[i]); // add surface
    }

    const auto availableLayers = context.enumerateInstanceLayerProperties();

    if (!findMissingValidationLayser(availableLayers, layers).empty()) {
      throw std::runtime_error("Missing validation layers!");
    }

    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo.pApplicationInfo = &engineInfo;

    instanceCreateInfo.setPEnabledLayerNames(layers);
    instanceCreateInfo.setPEnabledExtensionNames(instanceExtensions);

    vk::raii::Instance instance(context, instanceCreateInfo);

    auto debugMessenger = setupDebugMessenger(instance);

    vk::raii::PhysicalDevices physicalDevices(instance);

    const vk::raii::PhysicalDevice &bestPhysicalDevice =
        pickPhysicalDevice(physicalDevices);

    QueueManager queueManager(bestPhysicalDevice);
    checkDeviceExtensions(bestPhysicalDevice);

    auto featuresChain = buildRequestedFeatures();
    validateFeatures(bestPhysicalDevice, featuresChain);

    vk::DeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.setPNext(
        &featuresChain.get<vk::PhysicalDeviceFeatures2>());

    deviceCreateInfo.setQueueCreateInfos(queueManager.getQueueCreateInfos());
    deviceCreateInfo.setPEnabledExtensionNames(getDeviceExtensions());

    vk::raii::Device device(bestPhysicalDevice, deviceCreateInfo);
    vk::Queue graphicsQueue =
        queueManager.getQueue(*device, QueueFamilyRole::Graphics);
    vk::Queue computeQueue =
        queueManager.getQueue(*device, QueueFamilyRole::Compute);
    vk::Queue transferQueue =
        queueManager.getQueue(*device, QueueFamilyRole::Transfer);

    vk::CommandPoolCreateInfo graphicsPoolInfo{};
    graphicsPoolInfo.flags =
        vk::CommandPoolCreateFlagBits::eResetCommandBuffer; // 允许单独重置
                                                            // CommandBuffer
    graphicsPoolInfo.queueFamilyIndex =
        queueManager.getFamilyIndex(QueueFamilyRole::Graphics);

    vk::raii::CommandPool graphicsCommandPool(device, graphicsPoolInfo);

    vk::CommandPoolCreateInfo computePoolInfo{};
    computePoolInfo.flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    computePoolInfo.queueFamilyIndex =
        queueManager.getFamilyIndex(QueueFamilyRole::Compute);

    vk::raii::CommandPool computeCommandPool(device, computePoolInfo);

    vk::CommandPoolCreateInfo transferPoolInfo{};

    transferPoolInfo.flags = vk::CommandPoolCreateFlagBits::eTransient |
                             vk::CommandPoolCreateFlagBits::eResetCommandBuffer;
    transferPoolInfo.queueFamilyIndex =
        queueManager.getFamilyIndex(QueueFamilyRole::Transfer);

    vk::raii::CommandPool transferCommandPool(device, transferPoolInfo);

    auto surface = window.createSurface(instance);
    queueManager.checkPresentAbility(surface);
    auto surfaceFormats = bestPhysicalDevice.getSurfaceFormatsKHR(*surface);
    auto properFormat = window.selectFormat(surfaceFormats);

    const auto presentModes =
        bestPhysicalDevice.getSurfacePresentModesKHR(*surface);
    vk::PresentModeKHR selectedPresentMode = selectPresentMode(presentModes);

    auto surfaceCapabilities =
        bestPhysicalDevice.getSurfaceCapabilitiesKHR(*surface);

    vk::Extent2D swapchainExtent =
        window.getSwapchainExtend(surfaceCapabilities);
    uint32_t imageCount = window.getImageCount(
        surfaceCapabilities.minImageCount, surfaceCapabilities.maxImageCount);

    vk::SwapchainCreateInfoKHR swapchainCreateInfo{};
    swapchainCreateInfo.surface = *surface;
    swapchainCreateInfo.minImageCount = imageCount;
    swapchainCreateInfo.imageFormat = properFormat.format;
    swapchainCreateInfo.imageColorSpace = properFormat.colorSpace;
    swapchainCreateInfo.imageExtent = swapchainExtent;
    swapchainCreateInfo.imageArrayLayers = 1;
    swapchainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;

    uint32_t graphicsFamily =
        queueManager.getFamilyIndex(QueueFamilyRole::Graphics);
    swapchainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
    swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
    swapchainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    swapchainCreateInfo.presentMode = selectedPresentMode;
    swapchainCreateInfo.clipped = VK_TRUE;
    swapchainCreateInfo.oldSwapchain = nullptr;

    vk::raii::SwapchainKHR swapchain(device, swapchainCreateInfo);

    std::vector<vk::Image> swapchainImages = swapchain.getImages();
    std::vector<vk::raii::ImageView> swapchainImageViews;
    swapchainImageViews.reserve(swapchainImages.size());
    for (const auto &image : swapchainImages) {
      vk::ImageViewCreateInfo viewCreateInfo{};
      viewCreateInfo.image = image;
      viewCreateInfo.viewType = vk::ImageViewType::e2D;
      viewCreateInfo.format = properFormat.format;

      viewCreateInfo.components.r = vk::ComponentSwizzle::eIdentity;
      viewCreateInfo.components.g = vk::ComponentSwizzle::eIdentity;
      viewCreateInfo.components.b = vk::ComponentSwizzle::eIdentity;
      viewCreateInfo.components.a = vk::ComponentSwizzle::eIdentity;

      viewCreateInfo.subresourceRange.aspectMask =
          vk::ImageAspectFlagBits::eColor;
      viewCreateInfo.subresourceRange.baseMipLevel = 0;
      viewCreateInfo.subresourceRange.levelCount = 1;
      viewCreateInfo.subresourceRange.baseArrayLayer = 0;
      viewCreateInfo.subresourceRange.layerCount = 1;

      swapchainImageViews.emplace_back(device, viewCreateInfo);
    }
    // 初始化VMA Allocator
    VmaAllocatorRAII allocator(*instance, *bestPhysicalDevice, *device,
                               apiVersion);

    vk::ImageCreateInfo depthImageInfo{};
    depthImageInfo.imageType = vk::ImageType::e2D;
    depthImageInfo.format = vk::Format::eD32Sfloat; // 深度格式
    depthImageInfo.extent =
        vk::Extent3D(swapchainExtent.width, swapchainExtent.height, 1);
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.samples = vk::SampleCountFlagBits::e1;
    depthImageInfo.tiling = vk::ImageTiling::eOptimal;
    depthImageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;

    VmaAllocationCreateInfo depthAllocInfo{};
    depthAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    depthAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VmaImage depthImage = allocator.createImage(depthImageInfo, depthAllocInfo);

    vk::ImageViewCreateInfo depthViewInfo{};
    depthViewInfo.image = *depthImage;
    depthViewInfo.viewType = vk::ImageViewType::e2D;
    depthViewInfo.format = vk::Format::eD32Sfloat;
    depthViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.layerCount = 1;
    vk::raii::ImageView depthImageView(device, depthViewInfo);

    vk::BufferCreateInfo bufferCreateInfo{};
    bufferCreateInfo.size = cubeVertices.size() * sizeof(Vertex);
    bufferCreateInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer |
                             vk::BufferUsageFlagBits::eShaderDeviceAddress;

    vk::BufferCreateInfo indexBufInfo{};
    indexBufInfo.size = cubeIndices.size() * sizeof(uint32_t);
    indexBufInfo.usage = vk::BufferUsageFlagBits::eStorageBuffer |
                         vk::BufferUsageFlagBits::eShaderDeviceAddress;

    VmaAllocationCreateInfo allocCreateInfo{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO};

    VmaAllocationInfo allocInfo;
    VmaBuffer vertexBuffer =
        allocator.createBuffer(bufferCreateInfo, allocCreateInfo, &allocInfo);
    std::memcpy(allocInfo.pMappedData, cubeVertices.data(),
                bufferCreateInfo.size);

    VmaAllocationInfo indexAllocInfo;
    VmaBuffer indexBuffer =
        allocator.createBuffer(indexBufInfo, allocCreateInfo, &indexAllocInfo);
    std::memcpy(indexAllocInfo.pMappedData, cubeIndices.data(),
                indexBufInfo.size);
    ShaderEffect cubeEffect;
    cubeEffect.meshStageSpv = loadShaderSpv("generated_shaders/cube.mesh.spv");
    cubeEffect.fragStageSpv = loadShaderSpv("generated_shaders/cube.frag.spv");

    RenderPipeline cubePipeline(device, cubeEffect);
    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{
        vk::FenceCreateFlagBits::eSignaled}; // 初始为激活状态

    const int MAX_FRAMES_IN_FLIGHT = 2;
    std::vector<vk::raii::Semaphore> imageAvailableSemaphores;
    std::vector<vk::raii::Semaphore> renderFinishedSemaphores;
    std::vector<vk::raii::Fence> inFlightFences;
    std::vector<vk::raii::Fence> presentFences;

    imageAvailableSemaphores.reserve(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.reserve(MAX_FRAMES_IN_FLIGHT);
    inFlightFences.reserve(MAX_FRAMES_IN_FLIGHT);
    presentFences.reserve(MAX_FRAMES_IN_FLIGHT);
    vk::FenceCreateInfo presentFenceInfo{vk::FenceCreateFlagBits::eSignaled};

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
      imageAvailableSemaphores.emplace_back(device, semaphoreInfo);
      renderFinishedSemaphores.emplace_back(device, semaphoreInfo);
      inFlightFences.emplace_back(device, fenceInfo);
      presentFences.emplace_back(device, presentFenceInfo);
    }

    uint32_t currentFrame = 0;
    PushConstants pushConstants{};
    pushConstants.vertexBufferAddress = vertexBuffer.getAddress(device);
    pushConstants.indexBufferAddress = indexBuffer.getAddress(device);

    // --- [设置一个真正的 MVP 矩阵] ---
    glm::mat4 model = glm::rotate(glm::mat4(1.0f), glm::radians(45.0f),
                                  glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 view =
        glm::lookAt(glm::vec3(0.0f, 0.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 proj = glm::perspective(glm::radians(45.0f),
                                      (float)swapchainExtent.width /
                                          (float)swapchainExtent.height,
                                      0.1f, 10.0f);
    proj[1][1] *= -1; // Vulkan Y-axis flip
    pushConstants.mvp = proj * view * model;
    vk::CommandBufferAllocateInfo allocInfoCmd{
        *graphicsCommandPool, vk::CommandBufferLevel::ePrimary,
        static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};

    std::vector<vk::raii::CommandBuffer> commandBuffers =
        device.allocateCommandBuffers(allocInfoCmd);

    commandBuffers[0].begin(vk::CommandBufferBeginInfo{
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    vk::ImageMemoryBarrier2 depthBarrier = createImageBarrier(
        *depthImage, vk::ImageLayout::eUndefined,
        vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::ImageAspectFlagBits::eDepth, vk::PipelineStageFlagBits2::eTopOfPipe,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests,
        vk::AccessFlagBits2::eNone,
        vk::AccessFlagBits2::eDepthStencilAttachmentRead |
            vk::AccessFlagBits2::eDepthStencilAttachmentWrite);

    vk::DependencyInfo depthDepInfo;
    depthDepInfo.setImageMemoryBarriers(depthBarrier);
    commandBuffers[currentFrame].pipelineBarrier2(depthDepInfo);

    commandBuffers[currentFrame].end();

    vk::SubmitInfo initSubmit{};
    initSubmit.setCommandBuffers(*commandBuffers[currentFrame]);
    graphicsQueue.submit(initSubmit, nullptr);
    graphicsQueue.waitIdle();
    commandBuffers[currentFrame].reset();

    bool shouldClose = false;
    bool framebufferResized = false;
    while (!shouldClose) {

      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
          shouldClose = true;
        }
      }
      float time = SDL_GetTicks() / 1000.0f; // 获取秒数

      glm::mat4 model =
          glm::rotate(glm::mat4(1.0f), time, glm::vec3(0.5f, 1.0f, 0.0f));
      glm::mat4 view =
          glm::lookAt(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, 0.0f),
                      glm::vec3(0.0f, 1.0f, 0.0f));
      glm::mat4 proj = glm::perspective(glm::radians(45.0f),
                                        (float)swapchainExtent.width /
                                            (float)swapchainExtent.height,
                                        0.1f, 10.0f);

      pushConstants.mvp = proj * view * model;
      auto waitResult = device.waitForFences(
          {*inFlightFences[currentFrame], *presentFences[currentFrame]},
          VK_TRUE, std::numeric_limits<uint64_t>::max());

      device.resetFences(
          {*inFlightFences[currentFrame], *presentFences[currentFrame]});

      auto [acquireResult, imageIndex] = swapchain.acquireNextImage(
          std::numeric_limits<uint64_t>::max(),
          *imageAvailableSemaphores[currentFrame], nullptr);

      commandBuffers[currentFrame].reset();
      vk::CommandBufferBeginInfo beginInfo{
          vk::CommandBufferUsageFlagBits::eOneTimeSubmit};

      commandBuffers[currentFrame].begin(beginInfo);
      vk::ImageMemoryBarrier2 colorToDrawBarrier = createImageBarrier(
          swapchainImages[imageIndex], vk::ImageLayout::eUndefined,
          vk::ImageLayout::eColorAttachmentOptimal,
          vk::ImageAspectFlagBits::eColor,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::PipelineStageFlagBits2::eColorAttachmentOutput,
          vk::AccessFlagBits2::eNone,
          vk::AccessFlagBits2::eColorAttachmentWrite);
      vk::DependencyInfo colorToDrawDepInfo;
      colorToDrawDepInfo.setImageMemoryBarriers(colorToDrawBarrier);
      commandBuffers[currentFrame].pipelineBarrier2(colorToDrawDepInfo);

      vk::RenderingAttachmentInfo colorAttachment{};
      colorAttachment.imageView = *swapchainImageViews[imageIndex];
      colorAttachment.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
      colorAttachment.loadOp = vk::AttachmentLoadOp::eClear; // 清屏
      colorAttachment.storeOp = vk::AttachmentStoreOp::eStore;
      colorAttachment.clearValue = vk::ClearValue{
          std::array<float, 4>{0.1f, 0.1f, 0.1f, 1.0f}}; // 暗灰背景

      vk::RenderingAttachmentInfo depthAttachment{};
      depthAttachment.imageView = *depthImageView;
      depthAttachment.imageLayout =
          vk::ImageLayout::eDepthStencilAttachmentOptimal;
      depthAttachment.loadOp = vk::AttachmentLoadOp::eClear;
      depthAttachment.storeOp = vk::AttachmentStoreOp::eDontCare;
      depthAttachment.clearValue =
          vk::ClearValue{vk::ClearDepthStencilValue{1.0f, 0}};

      vk::RenderingInfo renderingInfo{};
      renderingInfo.renderArea = vk::Rect2D({0, 0}, swapchainExtent);
      renderingInfo.layerCount = 1;
      renderingInfo.setColorAttachments(colorAttachment);
      renderingInfo.setPDepthAttachment(&depthAttachment);

      commandBuffers[currentFrame].beginRendering(renderingInfo);

      cubePipeline.recordRenderCommands(commandBuffers[currentFrame],
                                        swapchainExtent.width,
                                        swapchainExtent.height);
      commandBuffers[currentFrame].pushDataEXT(vk::PushDataInfoEXT(
          0,
          vk::HostAddressRangeConstEXT(&pushConstants, sizeof(PushConstants))));

      commandBuffers[currentFrame].drawMeshTasksEXT(1, 1, 1);

      commandBuffers[currentFrame].endRendering();

      vk::ImageMemoryBarrier2 colorToPresentBarrier = createImageBarrier(
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

      vk::PipelineStageFlags waitStages[] = {
          vk::PipelineStageFlagBits::eColorAttachmentOutput};
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
      presentInfo.setSwapchains(*swapchain);
      presentInfo.setImageIndices(imageIndex);

      auto presentResult = graphicsQueue.presentKHR(presentInfo);

      currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    device.waitIdle();

  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << std::endl;
    return -1;
  }

  return 0;
}