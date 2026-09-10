module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
export module rlvk;

// rlvk：Vulkan C++ 头（vulkan.hpp / vulkan_raii.hpp）的具名模块封装（rlstd 同构）。
// 2026-09-02 编译提速 B 方案：vulkan.hpp 文本解析 ~3.1s/TU 是全项目模块 GMF 的共同
// 大头（-ftime-trace 实测），本模块让 hpp 全量编译只发生一次——消费方 `import rlvk;`
// load BMI（实测 ~18ms/58MB）。
//
// 导出名单 = 全代码库 vk::/vk::raii:: 实际用词 + 编译报错增补。
// 不导出：VK_* 宏与 vkXxx C 函数/C 类型（#define 不随模块导出）——需要者 GMF 保留
// 廉价的 #include <vulkan/vulkan.h>（vulkan_core.h 量级，实测 ~0.4s，vkerror 先例）。
//
// 维护：新用某 vk:: 名报 "not visible" → 对应块补一行 using（保持字典序）。

export namespace vk {
// Flags<Bit> 位运算与枚举/句柄比较运算符（vulkan.hpp namespace 作用域自由函数，
// 不随类序列化，必须逐个 using——同 rlstd 运算符导出教训）
using vk::operator&;
using vk::operator|;
using vk::operator^;
using vk::operator~;
using vk::AccessFlagBits2;
using vk::AccessFlags2;
using vk::ApplicationInfo;
using vk::AttachmentLoadOp;
using vk::AttachmentStoreOp;
using vk::BindHeapInfoEXT;
using vk::Bool32;
using vk::BufferCreateInfo;
using vk::BufferDeviceAddressInfo;
using vk::BufferMemoryBarrier2;
using vk::BufferUsageFlagBits;
using vk::BufferUsageFlags;
using vk::ClearDepthStencilValue;
using vk::ClearValue;
using vk::ColorBlendEquationEXT;
using vk::ColorComponentFlagBits;
using vk::ColorComponentFlags;
using vk::ColorSpaceKHR;
using vk::CommandBufferAllocateInfo;
using vk::CommandBufferBeginInfo;
using vk::CommandBufferLevel;
using vk::CommandPoolCreateFlagBits;
using vk::CommandPoolCreateFlags;
using vk::CommandPoolCreateInfo;
using vk::CompareOp;
using vk::ComponentSwizzle;
using vk::CompositeAlphaFlagBitsKHR;
using vk::CullModeFlagBits;
using vk::DebugUtilsMessageSeverityFlagBitsEXT;
using vk::DebugUtilsMessageTypeFlagBitsEXT;
using vk::DebugUtilsMessageTypeFlagsEXT;
using vk::DebugUtilsMessengerCallbackDataEXT;
using vk::DebugUtilsMessengerCreateInfoEXT;
using vk::DependencyInfo;
using vk::DescriptorType;
using vk::Device;
using vk::DeviceAddress;
using vk::DeviceAddressRangeEXT;
using vk::DeviceCreateInfo;
using vk::DeviceDiagnosticsConfigCreateInfoNV;  // RGL_AFTERMATH 诊断（vkdevice 条件链入）
using vk::DeviceDiagnosticsConfigFlagBitsNV;    // RGL_AFTERMATH 诊断
using vk::DeviceDiagnosticsConfigFlagsNV;       // RGL_AFTERMATH 诊断
using vk::DeviceQueueCreateFlags;
using vk::DeviceQueueCreateInfo;
using vk::DeviceSize;
using vk::Extent2D;
using vk::Extent3D;
using vk::Fence;
using vk::FenceCreateFlagBits;
using vk::FenceCreateInfo;
using vk::Format;
using vk::FormatFeatureFlagBits;
using vk::FormatFeatureFlags;
using vk::FrontFace;
using vk::HostAddressRangeConstEXT;
using vk::HostAddressRangeEXT;
using vk::Image;
using vk::ImageAspectFlagBits;
using vk::ImageAspectFlags;
using vk::ImageBlit;
using vk::ImageCreateFlagBits;
using vk::ImageCreateInfo;
using vk::ImageDescriptorInfoEXT;
using vk::ImageLayout;
using vk::ImageMemoryBarrier2;
using vk::ImageTiling;
using vk::ImageType;
using vk::ImageUsageFlagBits;
using vk::ImageViewCreateInfo;
using vk::ImageViewType;
using vk::InstanceCreateInfo;
using vk::LayerProperties;
using vk::MemoryBarrier2;
using vk::MemoryHeapFlagBits;
using vk::Offset3D;
using vk::PhysicalDeviceDescriptorBufferFeaturesEXT;
using vk::PhysicalDeviceDescriptorHeapFeaturesEXT;
using vk::PhysicalDeviceDescriptorHeapPropertiesEXT;
using vk::PhysicalDeviceDepthClipEnableFeaturesEXT;
using vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT;
using vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT;
using vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT;
using vk::PhysicalDeviceFeatures2;
using vk::PhysicalDeviceMeshShaderFeaturesEXT;
using vk::PhysicalDeviceProperties;
using vk::PhysicalDeviceProperties2;
using vk::PhysicalDeviceRobustness2FeaturesKHR;
using vk::PhysicalDeviceShaderCorePropertiesAMD;
using vk::PhysicalDeviceShaderObjectFeaturesEXT;
using vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR;
using vk::PhysicalDeviceSubgroupSizeControlProperties;
using vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR;
using vk::PhysicalDeviceType;
using vk::PhysicalDeviceVulkan11Features;
using vk::PhysicalDeviceVulkan12Features;
using vk::PhysicalDeviceVulkan13Features;
using vk::PhysicalDeviceVulkan14Features;
using vk::PipelineStageFlagBits2;
using vk::PipelineStageFlags2;
using vk::PolygonMode;
using vk::PresentInfoKHR;
using vk::PresentModeKHR;
using vk::PushDataInfoEXT;
using vk::Queue;
using vk::QueueFlagBits;
using vk::Rect2D;
using vk::RenderingAttachmentInfo;
using vk::RenderingInfo;
using vk::ResourceDescriptorInfoEXT;
using vk::Result;
using vk::SampleCountFlagBits;
using vk::SamplerCreateInfo;
using vk::SemaphoreCreateInfo;
using vk::ShaderCreateInfoEXT;
using vk::ShaderEXT;
using vk::ShaderStageFlagBits;
using vk::SharingMode;
using vk::StructureChain;
using vk::StructureType;
using vk::SurfaceCapabilitiesKHR;
using vk::SurfaceFormatKHR;
using vk::SwapchainCreateInfoKHR;
using vk::SwapchainPresentFenceInfoEXT;
using vk::SystemError;
using vk::Viewport;
using vk::makeVersion;
using vk::to_string;
}

export namespace vk::raii {
using vk::raii::CommandBuffer;
using vk::raii::CommandPool;
using vk::raii::Context;
using vk::raii::DebugUtilsMessengerEXT;
using vk::raii::Device;
using vk::raii::Fence;
using vk::raii::ImageView;
using vk::raii::Instance;
using vk::raii::PhysicalDevice;
using vk::raii::PhysicalDevices;
using vk::raii::Semaphore;
using vk::raii::ShaderEXT;
using vk::raii::SurfaceKHR;
using vk::raii::SwapchainKHR;
}

export {
using ::VkBuffer;
using ::VkBufferCopy;
using ::VkBufferCreateInfo;
using ::VkBufferImageCopy;
using ::VkBufferUsageFlags;
using ::VkColorBlendEquationEXT;
using ::VkCommandBuffer;
using ::VkDependencyInfo;
using ::VkDescriptorSetAndBindingMappingEXT;
using ::VkDevice;
using ::VkDeviceMemory;
using ::VkDeviceSize;
using ::VkFence;
using ::VkHeadlessSurfaceCreateInfoEXT;
using ::VkImage;
using ::VkImageBlit;
using ::VkImageCopy;
using ::VkImageCreateInfo;
using ::VkImageDescriptorInfoEXT;
using ::VkImageMemoryBarrier2;
using ::VkImageSubresource;
using ::VkImageViewCreateInfo;
using ::VkInstance;
using ::VkOffset3D;
using ::VkPhysicalDevice;
using ::VkPhysicalDeviceDescriptorHeapPropertiesEXT;
using ::VkPhysicalDeviceLimits;
using ::VkPipelineStageFlags;
using ::VkPushConstantRange;
using ::VkPushDataInfoEXT;
using ::VkResult;
using ::VkSamplerCreateInfo;
using ::VkSemaphore;
using ::VkShaderCreateInfoEXT;
using ::VkShaderDescriptorSetAndBindingMappingInfoEXT;
using ::VkShaderEXT;
using ::VkSubmitInfo;
using ::VkSubresourceLayout;
using ::VkSurfaceKHR;
}
