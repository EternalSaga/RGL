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
using vk::ArrayProxy;
using vk::AttachmentLoadOp;
using vk::AttachmentStoreOp;
using vk::BindHeapInfoEXT;
using vk::Bool32;
using vk::BorderColor;
using vk::Buffer;
using vk::BufferCreateInfo;
using vk::BufferDeviceAddressInfo;
using vk::BufferImageCopy;
using vk::BufferMemoryBarrier2;
using vk::BufferUsageFlagBits;
using vk::BufferUsageFlags;
using vk::CalibratedTimestampInfoKHR;
using vk::ClearColorValue;
using vk::ClearDepthStencilValue;
using vk::ClearValue;
using vk::ColorBlendEquationEXT;
using vk::ColorComponentFlagBits;
using vk::ColorComponentFlags;
using vk::ColorSpaceKHR;
using vk::CommandBufferAllocateInfo;
using vk::CommandBufferBeginInfo;
using vk::CommandBufferLevel;
using vk::CommandBufferSubmitInfo;
using vk::CommandBufferUsageFlagBits;
using vk::CommandBufferUsageFlags;
using vk::CommandPoolCreateFlagBits;
using vk::CommandPoolCreateFlags;
using vk::CommandPoolCreateInfo;
using vk::CompareOp;
using vk::ComponentSwizzle;
using vk::CompositeAlphaFlagBitsKHR;
using vk::ComputePipelineCreateInfo;
using vk::ConservativeRasterizationModeEXT;  // 批 11.3-1b 保守光栅（tests + 后续 izb pass）
using vk::CullModeFlagBits;
using vk::DebugUtilsMessageSeverityFlagBitsEXT;
using vk::DebugUtilsMessageTypeFlagBitsEXT;
using vk::DebugUtilsMessageTypeFlagsEXT;
using vk::DebugUtilsMessengerCallbackDataEXT;
using vk::DebugUtilsMessengerCreateInfoEXT;
using vk::DependencyInfo;
using vk::DescriptorBufferInfo;  // 批 11.3-1c：测试侧 SSBO 描述符
using vk::DescriptorImageInfo;
using vk::DescriptorPoolCreateFlagBits;
using vk::DescriptorPoolCreateInfo;
using vk::DescriptorPoolSize;
using vk::DescriptorSet;
using vk::DescriptorSetAllocateInfo;
using vk::DescriptorSetLayoutBinding;
using vk::DescriptorSetLayoutCreateInfo;
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
using vk::DynamicState;  // 批 11.3-1b：eConservativeRasterizationModeEXT 进动态名单
using vk::Extent2D;
using vk::Extent3D;
using vk::Fence;
using vk::FenceCreateFlagBits;
using vk::FenceCreateFlags;
using vk::FenceCreateInfo;
using vk::Filter;
using vk::Format;
using vk::FormatFeatureFlagBits;
using vk::FormatFeatureFlags;
using vk::FrontFace;
using vk::GraphicsPipelineCreateInfo;  // 批 11.3-1b：测试侧传统管线（mesh + 保守光栅状态）
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
using vk::ImageSubresourceRange;
using vk::ImageTiling;
using vk::ImageType;
using vk::ImageUsageFlagBits;
using vk::ImageUsageFlags;
using vk::ImageView;
using vk::ImageViewCreateInfo;
using vk::ImageViewType;
using vk::InstanceCreateInfo;
using vk::LayerProperties;
using vk::makeVersion;
using vk::MemoryAllocateInfo;
using vk::MemoryBarrier2;
using vk::MemoryHeapFlagBits;
using vk::MemoryMapFlags;
using vk::MemoryPropertyFlagBits;
using vk::MemoryPropertyFlags;
using vk::MemoryRequirements;
using vk::Offset2D;
using vk::Offset3D;
using vk::PastPresentationTimingEXT;          // Phase 9.3 present_timing 结果项
using vk::PastPresentationTimingInfoEXT;      // Phase 9.3 collect 输入描述
using vk::PastPresentationTimingPropertiesEXT;  // Phase 9.3 collect 输出缓冲描述
using vk::PhysicalDeviceConservativeRasterizationPropertiesEXT;  // 批 11.3-1b：§9 能力 golden
using vk::PhysicalDeviceDepthClipEnableFeaturesEXT;
using vk::PhysicalDeviceDescriptorBufferFeaturesEXT;
using vk::PhysicalDeviceDescriptorHeapFeaturesEXT;
using vk::PhysicalDeviceDescriptorHeapPropertiesEXT;
using vk::PhysicalDeviceDeviceGeneratedCommandsFeaturesEXT;
using vk::PhysicalDeviceDeviceGeneratedCommandsPropertiesEXT;
using vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT;
using vk::PhysicalDeviceFeatures2;
using vk::PhysicalDeviceMemoryProperties;
using vk::PhysicalDeviceMeshShaderFeaturesEXT;
using vk::PhysicalDeviceMeshShaderPropertiesEXT;  // ★1-a2-i 簇路径发射规格核对（main_meshlets）
using vk::PhysicalDevicePresentId2FeaturesKHR;  // Phase 9.3 feature 结构（vkdevice 成员）
using vk::PhysicalDevicePresentTimingFeaturesEXT;  // Phase 9.3 feature 结构（vkdevice 成员）
using vk::PhysicalDeviceProperties;
using vk::PhysicalDeviceProperties2;
using vk::PhysicalDeviceRobustness2FeaturesKHR;
using vk::PhysicalDeviceShaderCorePropertiesAMD;
using vk::PhysicalDeviceShaderObjectFeaturesEXT;
using vk::PhysicalDeviceShaderUntypedPointersFeaturesKHR;
using vk::PhysicalDeviceSubgroupSizeControlProperties;
using vk::PhysicalDeviceSurfaceInfo2KHR;      // Phase 9.3 caps2 查询输入
using vk::PhysicalDeviceSwapchainMaintenance1FeaturesKHR;
using vk::PhysicalDeviceType;
using vk::PhysicalDeviceVulkan11Features;
using vk::PhysicalDeviceVulkan12Features;
using vk::PhysicalDeviceVulkan13Features;
using vk::PhysicalDeviceVulkan14Features;
using vk::PipelineBindPoint;
using vk::PipelineColorBlendAttachmentState;  // 批 11.3-1b：测试侧传统管线（下同）
using vk::PipelineColorBlendStateCreateInfo;
using vk::PipelineDynamicStateCreateInfo;
using vk::PipelineLayoutCreateInfo;
using vk::PipelineMultisampleStateCreateInfo;
using vk::PipelineRasterizationConservativeStateCreateInfoEXT;
using vk::PipelineRasterizationStateCreateInfo;
using vk::PipelineRenderingCreateInfo;
using vk::PipelineShaderStageCreateFlags;
using vk::PipelineShaderStageCreateInfo;
using vk::PipelineStageFlagBits2;
using vk::PipelineStageFlags2;
using vk::PipelineViewportStateCreateInfo;  // 批 11.3-1b
using vk::PolygonMode;
using vk::PresentId2KHR;                      // Phase 9.3 present id pNext
using vk::PresentInfoKHR;
using vk::PresentModeKHR;
using vk::PresentStageFlagBitsEXT;            // Phase 9.3 stage 位（VK_PRESENT_STAGE_*）
using vk::PresentStageFlagsEXT;               // Phase 9.3 stage 查询/结果 flags
using vk::PresentStageTimeEXT;                // Phase 9.3 per-stage 时间对
using vk::PresentTimingInfoEXT;               // Phase 9.3 present pNext 链主结构
using vk::PresentTimingQueueFullEXTError;       // Phase 9.3：raii presentKHR 遇队列满抛此异常
using vk::PresentTimingsInfoEXT;              // Phase 9.3 present pNext 链头（数组描述）
using vk::PresentTimingSurfaceCapabilitiesEXT;  // Phase 9.3 surface 能力（stageQueries）
using vk::PushConstantRange;
using vk::PushDataInfoEXT;
using vk::QueryPool;
using vk::QueryPoolCreateInfo;
using vk::QueryResultFlagBits;
using vk::QueryResultFlags;
using vk::QueryType;
using vk::Queue;
using vk::QueueFamilyProperties;
using vk::QueueFlagBits;
using vk::Rect2D;
using vk::RenderingAttachmentInfo;
using vk::RenderingInfo;
using vk::ResourceDescriptorInfoEXT;
using vk::Result;
using vk::SampleCountFlagBits;
using vk::SamplerAddressMode;
using vk::SamplerCreateInfo;
using vk::SamplerMipmapMode;
using vk::SemaphoreCreateInfo;
using vk::ShaderCreateInfoEXT;
using vk::ShaderEXT;
using vk::ShaderModuleCreateInfo;
using vk::ShaderStageFlagBits;
using vk::SharingMode;
using vk::StructureChain;
using vk::StructureType;
using vk::SubmitInfo;
using vk::SurfaceCapabilities2KHR;            // Phase 9.3 caps2 查询输出
using vk::SurfaceCapabilitiesKHR;
using vk::SurfaceFormatKHR;
using vk::SurfacePresentModeKHR;              // Phase 9.3 caps2 查询输入（按 present 模式过滤）
using vk::SwapchainCalibratedTimestampInfoEXT;  // Phase 9.3 时间域校准 pNext
using vk::SwapchainCreateFlagBitsKHR;         // Phase 9.3 ePresentTimingEXT 创建 flag
using vk::SwapchainCreateInfoKHR;
using vk::SwapchainKHR;                       // Phase 9.3 交换链句柄（重建检测）
using vk::SwapchainPresentFenceInfoEXT;
using vk::SwapchainTimeDomainPropertiesEXT;   // Phase 9.3 时间域 id 列表输出
using vk::SwapchainTimingPropertiesEXT;       // Phase 9.3 refreshDuration/Interval
using vk::SystemError;
using vk::TimeDomainEXT;
using vk::TimeDomainKHR;                      // Phase 9.3 eSwapchainLocalEXT / eClockMonotonic
using vk::to_string;
using vk::Viewport;
using vk::WriteDescriptorSet;
}

export namespace vk::raii {
using vk::raii::Buffer;
using vk::raii::CommandBuffer;
using vk::raii::CommandPool;
using vk::raii::Context;
using vk::raii::DebugUtilsMessengerEXT;
using vk::raii::DescriptorPool;
using vk::raii::DescriptorSet;
using vk::raii::DescriptorSetLayout;
using vk::raii::Device;
using vk::raii::DeviceMemory;
using vk::raii::Fence;
using vk::raii::Image;
using vk::raii::ImageView;
using vk::raii::Instance;
using vk::raii::PhysicalDevice;
using vk::raii::PhysicalDevices;
using vk::raii::Pipeline;
using vk::raii::PipelineLayout;
using vk::raii::QueryPool;
using vk::raii::Sampler;
using vk::raii::Semaphore;
using vk::raii::ShaderEXT;
using vk::raii::ShaderModule;
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
using ::VkDescriptorSet;
using ::VkDescriptorSetAndBindingMappingEXT;
using ::VkDevice;
using ::VkDeviceMemory;
using ::VkDeviceSize;
using ::VkFence;
using ::VkFormat;
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
