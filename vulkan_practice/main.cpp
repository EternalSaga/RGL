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

    auto featuresChain = vk::StructureChain<
        vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
        vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
        vk::PhysicalDeviceVulkan14Features,
        vk::PhysicalDeviceMeshShaderFeaturesEXT,
        vk::PhysicalDeviceDescriptorHeapFeaturesEXT,
        vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR,
        vk::PhysicalDeviceShaderObjectFeaturesEXT>();

    featuresChain.get<vk::PhysicalDeviceFeatures2>()
        .features.robustBufferAccess = VK_TRUE;

    auto &f11 = featuresChain.get<vk::PhysicalDeviceVulkan11Features>();
    f11.shaderDrawParameters = VK_TRUE;
    f11.multiview = VK_TRUE;
    f11.storageBuffer16BitAccess = VK_TRUE;

    auto &f12 = featuresChain.get<vk::PhysicalDeviceVulkan12Features>();
    f12.descriptorIndexing = VK_TRUE;
    f12.shaderUniformBufferArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;
    f12.timelineSemaphore = VK_TRUE;
    f12.bufferDeviceAddress = VK_TRUE;

    auto &f13 = featuresChain.get<vk::PhysicalDeviceVulkan13Features>();
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderZeroInitializeWorkgroupMemory = VK_TRUE;

    auto &f14 = featuresChain.get<vk::PhysicalDeviceVulkan14Features>();
    f14.pushDescriptor = VK_TRUE;
    f14.dynamicRenderingLocalRead = VK_TRUE;
    f14.maintenance5 = VK_TRUE;
    f14.maintenance6 = VK_TRUE;

    auto &fShaderOjbect =
        featuresChain.get<vk::PhysicalDeviceShaderObjectFeaturesEXT>();
    fShaderOjbect.shaderObject = VK_TRUE;
    auto &fMesh = featuresChain.get<vk::PhysicalDeviceMeshShaderFeaturesEXT>();
    fMesh.meshShader = VK_TRUE;
    fMesh.taskShader = VK_FALSE;

    auto &swapchainMaintenance1Features =
        featuresChain.get<vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR>();
    swapchainMaintenance1Features.swapchainMaintenance1 = VK_TRUE;

    vk::DeviceCreateInfo deviceCreateInfo{};
    deviceCreateInfo.setPNext(
        &featuresChain.get<vk::PhysicalDeviceFeatures2>());

    deviceCreateInfo.setQueueCreateInfos(queueManager.getQueueCreateInfos());
    std::vector<const char *> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
        VK_EXT_MESH_SHADER_EXTENSION_NAME,
        VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
        VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
        VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME

    };

    deviceCreateInfo.setPEnabledExtensionNames(deviceExtensions);

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
    cubeEffect.pushConstantSize = sizeof(PushConstants);

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

    uint32_t currentFrame = 0; // 记录当前使用的是哪一套同步对象（0 或 1）

    // 2. 模拟一个极简的 MVP 矩阵 (这里先用单位矩阵，或者让它转起来)
    PushConstants pushConstants{};
    pushConstants.vertexBufferAddress = vertexBuffer.getAddress(device);
    pushConstants.indexBufferAddress = indexBuffer.getAddress(device);
    pushConstants.mvp = glm::mat4(
        1.0f); // 生产环境用 glm::perspective * glm::lookAt * glm::rotate
    vk::CommandBufferAllocateInfo allocInfoCmd{
        *graphicsCommandPool, vk::CommandBufferLevel::ePrimary,
        static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
    // raii 会自动返回一个包含 2 个 CommandBuffer 的 std::vector
    std::vector<vk::raii::CommandBuffer> commandBuffers =
        device.allocateCommandBuffers(allocInfoCmd);

    commandBuffers[0].begin(vk::CommandBufferBeginInfo{
        vk::CommandBufferUsageFlagBits::eOneTimeSubmit});

    vk::ImageMemoryBarrier depthTransitionBarrier{};
    depthTransitionBarrier.oldLayout = vk::ImageLayout::eUndefined;
    depthTransitionBarrier.newLayout =
        vk::ImageLayout::eDepthStencilAttachmentOptimal;
    depthTransitionBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthTransitionBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depthTransitionBarrier.image =
        *depthImage; // 填入你的 VmaImage 对应的 VkImage
    depthTransitionBarrier.subresourceRange.aspectMask =
        vk::ImageAspectFlagBits::eDepth;
    depthTransitionBarrier.subresourceRange.baseMipLevel = 0;
    depthTransitionBarrier.subresourceRange.levelCount = 1;
    depthTransitionBarrier.subresourceRange.baseArrayLayer = 0;
    depthTransitionBarrier.subresourceRange.layerCount = 1;
    depthTransitionBarrier.srcAccessMask = vk::AccessFlagBits::eNone;
    depthTransitionBarrier.dstAccessMask =
        vk::AccessFlagBits::eDepthStencilAttachmentRead |
        vk::AccessFlagBits::eDepthStencilAttachmentWrite;

    commandBuffers[currentFrame].pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eEarlyFragmentTests, {}, nullptr, nullptr,
        depthTransitionBarrier);

    commandBuffers[currentFrame].end();

    // 立即提交并等待完成，确保循环开始前布局已就绪
    vk::SubmitInfo initSubmit{};
    initSubmit.setCommandBuffers(*commandBuffers[currentFrame]);
    graphicsQueue.submit(initSubmit, nullptr);
    graphicsQueue.waitIdle();
    commandBuffers[currentFrame].reset();

    // 3. 游戏/渲染主循环
    bool shouldClose = false;
    bool framebufferResized = false;
    while (!shouldClose) {
      // 处理 SDL 事件，免得窗口卡死
      SDL_Event event;
      while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT) {
          shouldClose = true;
        }
      }

      // 1. ✨ 双重等待：确保 GPU 渲染完了 (inFlight)，且 Plasma
      // 已经把这一套的资源释放了 (present)
      auto waitResult = device.waitForFences(
          {*inFlightFences[currentFrame], *presentFences[currentFrame]},
          VK_TRUE, std::numeric_limits<uint64_t>::max());
      // 同时重置这两个 Fence
      device.resetFences(
          {*inFlightFences[currentFrame], *presentFences[currentFrame]});

      // 2. 获取图片，传入属于当前帧的信号量
      auto [acquireResult, imageIndex] = swapchain.acquireNextImage(
          std::numeric_limits<uint64_t>::max(),
          *imageAvailableSemaphores[currentFrame], nullptr);

      // 3. 重置当前槽位的 Command Buffer 并录制
      commandBuffers[currentFrame].reset();
      vk::CommandBufferBeginInfo beginInfo{
          vk::CommandBufferUsageFlagBits::eOneTimeSubmit};

      commandBuffers[currentFrame].begin(beginInfo);
      vk::ImageMemoryBarrier colorPresentToDrawBarrier{};
      colorPresentToDrawBarrier.oldLayout =
          vk::ImageLayout::eUndefined; // 使用 Undefined
                                       // 可以让硬件直接擦除旧数据，提升效率
      colorPresentToDrawBarrier.newLayout =
          vk::ImageLayout::eColorAttachmentOptimal;
      colorPresentToDrawBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      colorPresentToDrawBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      colorPresentToDrawBarrier.image = swapchainImages[imageIndex];
      colorPresentToDrawBarrier.subresourceRange.aspectMask =
          vk::ImageAspectFlagBits::eColor;
      colorPresentToDrawBarrier.subresourceRange.baseMipLevel = 0;
      colorPresentToDrawBarrier.subresourceRange.levelCount = 1;
      colorPresentToDrawBarrier.subresourceRange.baseArrayLayer = 0;
      colorPresentToDrawBarrier.subresourceRange.layerCount = 1;
      colorPresentToDrawBarrier.srcAccessMask = vk::AccessFlagBits::eNone;
      colorPresentToDrawBarrier.dstAccessMask =
          vk::AccessFlagBits::eColorAttachmentWrite;

      commandBuffers[currentFrame].pipelineBarrier(
          vk::PipelineStageFlagBits::eColorAttachmentOutput,
          vk::PipelineStageFlagBits::eColorAttachmentOutput, {}, nullptr,
          nullptr, colorPresentToDrawBarrier);

      // 现代 Vulkan 核心：使用 Dynamic Rendering (动态渲染) 代替 RenderPass
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

      // 开始动态渲染
      commandBuffers[currentFrame].beginRendering(renderingInfo);

      // 绑定你写在 shader.cppm 里的那堆动态状态
      cubePipeline.recordRenderCommands(commandBuffers[currentFrame],
                                        swapchainExtent.width,
                                        swapchainExtent.height);

      commandBuffers[currentFrame].pushConstants(
          cubePipeline.layout, // 1. PipelineLayout
          vk::ShaderStageFlagBits::eMeshEXT |
              vk::ShaderStageFlagBits::eFragment, // 2. Shader Stage
          0,                                      // 3. Offset
          vk::ArrayProxy<const PushConstants>(
              1, &pushConstants) // 4. Data (ArrayProxy)
      );

      // 【最核心的临门一脚】：调用 Mesh Shader 的绘制命令！
      // 你的立方体有 12 个三角形，每个工作组处理 1 个（或者你在 Slang 里写了 ti
      // < totalPrimitives） 对应着执行 1 个工作组就够拉取 12 个面了
      commandBuffers[currentFrame].drawMeshTasksEXT(1, 1, 1);

      commandBuffers[currentFrame].endRendering();

      vk::ImageMemoryBarrier colorDrawToPresentBarrier{};
      colorDrawToPresentBarrier.oldLayout =
          vk::ImageLayout::eColorAttachmentOptimal;
      colorDrawToPresentBarrier.newLayout = vk::ImageLayout::ePresentSrcKHR;
      colorDrawToPresentBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      colorDrawToPresentBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      colorDrawToPresentBarrier.image = swapchainImages[imageIndex];
      colorDrawToPresentBarrier.subresourceRange.aspectMask =
          vk::ImageAspectFlagBits::eColor;
      colorDrawToPresentBarrier.subresourceRange.baseMipLevel = 0;
      colorDrawToPresentBarrier.subresourceRange.levelCount = 1;
      colorDrawToPresentBarrier.subresourceRange.baseArrayLayer = 0;
      colorDrawToPresentBarrier.subresourceRange.layerCount = 1;
      colorDrawToPresentBarrier.srcAccessMask =
          vk::AccessFlagBits::eColorAttachmentWrite;
      colorDrawToPresentBarrier.dstAccessMask = vk::AccessFlagBits::eNone;

      commandBuffers[currentFrame].pipelineBarrier(
          vk::PipelineStageFlagBits::eColorAttachmentOutput,
          vk::PipelineStageFlagBits::eBottomOfPipe, {}, nullptr, nullptr,
          colorDrawToPresentBarrier);

      commandBuffers[currentFrame].end();

      // 提交到 Graphics 队列
      vk::PipelineStageFlags waitStages[] = {
          vk::PipelineStageFlagBits::eColorAttachmentOutput};
      vk::SubmitInfo submitInfo{};
      submitInfo.setWaitSemaphores(*imageAvailableSemaphores[currentFrame]);
      submitInfo.setWaitDstStageMask(waitStages);
      submitInfo.setCommandBuffers(*commandBuffers[currentFrame]);
      submitInfo.setSignalSemaphores(*renderFinishedSemaphores[currentFrame]);

      // 压入当前帧的 Fence
      graphicsQueue.submit(submitInfo, *inFlightFences[currentFrame]);

      // 5. ✨【现代 Vulkan 的绝活】：利用 pNext 挂载 Present Fence
      vk::SwapchainPresentFenceInfoEXT presentFenceInfoEXT{};
      presentFenceInfoEXT.setFences(
          *presentFences[currentFrame]); // 把当前槽位的 presentFence 拴上去

      vk::PresentInfoKHR presentInfo{};
      presentInfo.setPNext(&presentFenceInfoEXT); // 🌟 极其干净的扩展绑定
      presentInfo.setWaitSemaphores(
          *renderFinishedSemaphores[currentFrame]); // ✨ 回归 currentFrame
      presentInfo.setSwapchains(*swapchain);
      presentInfo.setImageIndices(imageIndex);

      auto presentResult = graphicsQueue.presentKHR(presentInfo);

      // 完美的帧交替
      currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    // 退出前等 GPU 忙完
    device.waitIdle();

  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << std::endl;
    return -1;
  }

  return 0;
}