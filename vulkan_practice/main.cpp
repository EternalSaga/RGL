#include "vulkan/vulkan.hpp"
#include <SDL3/SDL_events.h>
#include <SDL3/SDL.h>
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

    auto featuresChain =
        vk::StructureChain<vk::PhysicalDeviceFeatures2, // 1.0
                                                        // 的扩展基类（链表头）
                           vk::PhysicalDeviceVulkan11Features, // 1.1
                           vk::PhysicalDeviceVulkan12Features, // 1.2
                           vk::PhysicalDeviceVulkan13Features, // 1.3
                           vk::PhysicalDeviceVulkan14Features, // 1.4
                           vk::PhysicalDeviceMeshShaderFeaturesEXT,
                           vk::PhysicalDeviceDescriptorHeapFeaturesEXT>();

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
    swapchainCreateInfo.imageArrayLayers = 1; // 每个帧缓冲区的只有一张2D图片
    swapchainCreateInfo.imageUsage =
        vk::ImageUsageFlagBits::eColorAttachment; // 作为fragment shader的out
                                                  // vec4 outColor

    uint32_t graphicsFamily =
        queueManager.getFamilyIndex(QueueFamilyRole::Graphics);
    swapchainCreateInfo.imageSharingMode = vk::SharingMode::
        eExclusive; // 因为Graphics队列支持Present，所以使用独占队列

    swapchainCreateInfo.preTransform =
        surfaceCapabilities.currentTransform; // 不旋转
    swapchainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::
        eOpaque; // 渲染画面不参与桌面合成，不透明
    swapchainCreateInfo.presentMode =
        selectedPresentMode;                    // eMailbox或者eImmediate
    swapchainCreateInfo.clipped = VK_TRUE;      // 被遮挡不绘制
    swapchainCreateInfo.oldSwapchain = nullptr; // 一次创建交换链，背后没有前任

    vk::raii::SwapchainKHR swapchain(device, swapchainCreateInfo);
    // 注意：Swapchain 返回的是原生的 vk::Image 列表，因为生命周期由 Swapchain
    // 管理，不需要 RAII 释放
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
    VmaAllocatorCreateInfo allocatorInfo{.physicalDevice = *bestPhysicalDevice,
                                         .device = *device,
                                         .instance = *instance,
                                         .vulkanApiVersion = apiVersion};

    VmaAllocator allocator;
    vmaCreateAllocator(&allocatorInfo, &allocator);
    // 使用 VMA 创建 Depth Buffer (3D 旋转立方体必备)
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
    // VMA 内存配置：告诉 VMA 这个图是 GPU 独占的高速内存
    VmaAllocationCreateInfo depthAllocInfo{};
    depthAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    depthAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

    VkImage c_depthImage;
    VmaAllocation depthAllocation;
    // VMA 一步到位：创建 Image 并分配/绑定内存
    vmaCreateImage(allocator,
                   reinterpret_cast<VkImageCreateInfo *>(&depthImageInfo),
                   &depthAllocInfo, &c_depthImage, &depthAllocation, nullptr);

    // 将 C 风格的 VkImage 包装进 RAII，方便后续管理，并创建 ImageView
    vk::raii::Image depthImage(device, c_depthImage);

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

    VmaAllocationCreateInfo allocCreateInfo{};
    allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;

    allocCreateInfo.flags =
        VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
        VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer cubeBuffer;
    VmaAllocation allocation;
    VmaAllocationInfo allocInfo;
    vmaCreateBuffer(allocator,
                    reinterpret_cast<VkBufferCreateInfo *>(&bufferCreateInfo),
                    &allocCreateInfo, &cubeBuffer, &allocation, &allocInfo);

    std::memcpy(allocInfo.pMappedData, cubeVertices.data(),
                bufferCreateInfo.size);

    VkBuffer cubeIndexBuffer;
    VmaAllocation indexAllocation;
    VmaAllocationInfo indexAllocInfo;
    vmaCreateBuffer(
        allocator, reinterpret_cast<VkBufferCreateInfo *>(&indexBufInfo),
        &allocCreateInfo, &cubeIndexBuffer, &indexAllocation, &indexAllocInfo);
    std::memcpy(indexAllocInfo.pMappedData, cubeIndices.data(),
                indexBufInfo.size);

    vk::raii::Buffer vertexBuffer(device, cubeBuffer);
    vk::BufferDeviceAddressInfo vertexBufferAddressInfo(vertexBuffer);
    auto vertexBufferAddress = device.getBufferAddress(vertexBufferAddressInfo);

    vk::raii::Buffer indexBuffer(device, cubeIndexBuffer);
    vk::BufferDeviceAddressInfo indexBufferAddressInfo(indexBuffer);
    auto indexBufferAddress = device.getBufferAddress(indexBufferAddressInfo);

    ShaderEffect cubeEffect;
    cubeEffect.meshStageSpv = loadShaderSpv("generated_shaders/cube.mesh.spv");
    cubeEffect.fragStageSpv = loadShaderSpv("generated_shaders/cube.frag.spv");
    cubeEffect.pushConstantSize = sizeof(PushConstants);

    RenderPipeline cubePipeline(device, cubeEffect);
    vk::SemaphoreCreateInfo semaphoreInfo{};
    vk::FenceCreateInfo fenceInfo{
        vk::FenceCreateFlagBits::eSignaled}; // 初始为激活状态

    vk::raii::Semaphore imageAvailableSemaphore(device, semaphoreInfo);
    vk::raii::Semaphore renderFinishedSemaphore(device, semaphoreInfo);
    vk::raii::Fence inFlightFence(device, fenceInfo);

    // 2. 模拟一个极简的 MVP 矩阵 (这里先用单位矩阵，或者让它转起来)
    PushConstants pushConstants{};
    pushConstants.vertexBufferAddress = vertexBufferAddress;
    pushConstants.indexBufferAddress = indexBufferAddress;
    pushConstants.mvp = glm::mat4(
        1.0f); // 生产环境用 glm::perspective * glm::lookAt * glm::rotate

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

      // 等待上一帧渲染结束
      auto waitResult = device.waitForFences(
          {*inFlightFence}, VK_TRUE, std::numeric_limits<uint64_t>::max());
      device.resetFences({*inFlightFence});

      // 从交换链获取一张图片
      auto [acquireResult, imageIndex] =
          swapchain.acquireNextImage(std::numeric_limits<uint64_t>::max(),
                                     *imageAvailableSemaphore, nullptr);

      // 重置并开始录制命令
      graphicsCommandPool.reset();
      vk::CommandBufferBeginInfo beginInfo{
          vk::CommandBufferUsageFlagBits::eOneTimeSubmit};

      // 拿到当前帧的 CommandBuffer (这里 raii 需要分配或者你单独管理的 cmd)
      // 假设你分配好了一个单独的 cmd
      vk::CommandBufferAllocateInfo allocInfoCmd{
          *graphicsCommandPool, vk::CommandBufferLevel::ePrimary, 1};
      auto cmds = device.allocateCommandBuffers(allocInfoCmd);
      vk::raii::CommandBuffer cmd = std::move(cmds[0]);

      cmd.begin(beginInfo);

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

      // 图像管线屏障：现将交换链图像转为渲染状态
      // (省略了利用 vk::ImageMemoryBarrier2 的转换，建议加上以通过 Validation
      // Layer)

      // 开始动态渲染
      cmd.beginRendering(renderingInfo);

      // 绑定你写在 shader.cppm 里的那堆动态状态
      cubePipeline.recordRenderCommands(cmd, swapchainExtent.width,
                                        swapchainExtent.height);

      // 压入 BDA 地址和 MVP 矩阵到 Push Constants
      cmd.pushDataEXT(vk::PushDataInfoEXT(
          0,
          vk::HostAddressRangeConstEXT(&pushConstants, sizeof(PushConstants))));

      // 【最核心的临门一脚】：调用 Mesh Shader 的绘制命令！
      // 你的立方体有 12 个三角形，每个工作组处理 1 个（或者你在 Slang 里写了 ti
      // < totalPrimitives） 对应着执行 1 个工作组就够拉取 12 个面了
      cmd.drawMeshTasksEXT(1, 1, 1);

      cmd.endRendering();
      cmd.end();

      // 提交到 Graphics 队列
      vk::PipelineStageFlags waitStages[] = {
          vk::PipelineStageFlagBits::eColorAttachmentOutput};
      vk::SubmitInfo submitInfo{};
      submitInfo.setWaitSemaphores(*imageAvailableSemaphore);
      submitInfo.setWaitDstStageMask(waitStages);
      submitInfo.setCommandBuffers(*cmd);
      submitInfo.setSignalSemaphores(*renderFinishedSemaphore);

      graphicsQueue.submit(submitInfo, *inFlightFence);

      // Present 呈现到屏幕上
      vk::PresentInfoKHR presentInfo{};
      presentInfo.setWaitSemaphores(*renderFinishedSemaphore);
      presentInfo.setSwapchains(*swapchain);
      presentInfo.setImageIndices(imageIndex);

      auto presentResult = graphicsQueue.presentKHR(presentInfo);
    }

    // 退出前等 GPU 忙完
    device.waitIdle();
    vmaDestroyAllocator(allocator);
  } catch (const std::exception &e) {
    std::cerr << "错误: " << e.what() << std::endl;
    return -1;
  }

  return 0;
}