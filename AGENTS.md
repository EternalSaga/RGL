# vulkan_practice

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。  
核心代码位于vulkan_practice目录下。  
legacy目录下的代码是陈旧的OpenGL项目已经不是当前工作重点，一般不用看，但是有些思路可以进行参考。  

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`）。

```bash
just config              # CMake 配置（默认 clang, Release）
just config gcc Debug    # 指定编译器和构建类型
just build               # 增量编译（需先 config）
just build-release       # 一键配置 + 编译 Release
just build-debug         # 一键配置 + 编译 Debug
just run                 # 运行程序（自动设置 RADV_EXPERIMENTAL=heap）
just clean               # CMake 增量清理
just clean-all           # 删除整个 build 目录
just all                 # clean-all + build-release
```

CMake 无独立构建目录 — 必须从根目录构建（`cmake -B build -S .`）。

产物: `build/vulkan_practice/vulkan_practice`

> **运行要求：** `RADV_EXPERIMENTAL=heap` — RADV 驱动下启用 `VK_EXT_descriptor_heap` 必需。`just run` 已自动设置。

## 运行要求

- `RADV_EXPERIMENTAL=heap` — RADV 驱动下启用 `VK_EXT_descriptor_heap` 必需

## 命名规范

**所有 C++ 符号均在 `namespace RGL` 中。** 每个 `.cppm` 模块的导出均包裹在 `namespace RGL { ... }` 内。`main.cpp` 使用 `using namespace RGL;` 简化写法。

初始化层类之间的跨模块引用（如 `DeviceContext` → `AppContext`）使用 `RGL::` 前缀，因为每个类本身已在命名空间内。调用工具模块的函数/类型（如 `RGL::pickPhysicalDevice()`、`RGL::QueueFamilyManager`、`RGL::FeatureChain`）同样使用 `RGL::` 前缀。

## 模块结构（C++20 modules, .cppm）

所有导出均在 `namespace RGL` 内。

### 初始化层（构造顺序 = main.cpp 顺序）

| 模块 | 文件 | 用途 |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger、apiVersion |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VmaAllocatorRAII、depth image+view、顶点/索引缓冲、VmaDescriptorHeap（slot 8=顶点，slot 12=索引） |
| `vk_renderer` | vkrenderer.cppm | 渲染管线、同步对象、命令缓冲、渲染循环 |

### 工具模块

| 模块 | 文件 | 用途 |
|---|---|---|
| `vulkan_utils` | utils.cppm | 层检查、debug messenger、GPU 选择器、barrier 辅助、present mode |
| `vulkan_window` | sdl3window.cppm | SDL3 窗口、surface、HDR/SDR 格式选择 |
| `queue_family_manager` | QueueFamilyManager.cppm | Graphics/Compute/Transfer 队列分配 |
| `features` | features.cppm | FeatureChain 类型定义、buildRequestedFeatures、validateFeatures、checkDeviceExtensions、自定义异常 |
| `cube` | cube.cppm | 顶点/索引数据、DrawArgs 结构体（通过 pushDataEXT 推送） |
| `shader` | shader.cppm | SPV 加载器、ShaderEffect、RenderPipeline（ShaderObject + 动态状态） |
| `VmaRaii` | VmaBuffer.cppm | VmaBuffer、VmaImage、VmaDescriptorHeap、VmaAllocatorRAII 封装 |

`main.cpp` 按顺序构造 5 个初始化层类，然后调用 `vkRenderer.render(window)`。`vam_impl.cpp` 包含 `#define VMA_IMPLEMENTATION`。

### 构造依赖图

```
SDLWindow → AppContext → DeviceContext → Swapchain → Resources → Renderer
```

main.cpp 按逆序析构（RAII，无需手动清理）。

### 重要成员生命周期规则

- `vkinstance.cppm`：`engineInfo_`、`instanceCreateInfo_`、`layers_`、`instanceExtensions_` 均为类成员 — `pApplicationInfo` 不能悬空
- `vkdevice.cppm`：`featuresChain_` 为类成员 — `DeviceCreateInfo.pNext` 不能悬空
- 所有 `vk::raii::*` 对象必须通过**成员初始化列表**初始化，绝不在函数体内初始化（默认构造函数已删除）
- 成员声明顺序必须与初始化依赖顺序一致

## 着色器编译

`vulkan_practice/shaders/` 下的 Slang 着色器 → SPV 输出到 `build/vulkan_practice/generated_shaders/`。

- CMake `compile_slang()` 函数在构建时处理编译
- Profile: `sm_6_6+spirv_1_6`，capability: `spvDescriptorHeapEXT`
- 运行时从 `generated_shaders/` 相对路径读取 SPV — 必须从 build 目录运行或提供正确路径

## 隐式扩展依赖

`features.cppm` 中请求的 `VK_EXT_DEVICE_GENERATED_COMMANDS`、`VK_EXT_DESCRIPTOR_BUFFER` 等扩展当前无显式使用，但被其他已启用的扩展隐式依赖。移除它们可能导致驱动层面功能异常，不可随意删除。

`vkinstance.cppm` 中请求的 `VK_EXT_SWAPCHAIN_COLOR_SPACE` 等实例扩展同理。

**KHR/EXT 别名说明：** `VK_KHR_swapchain_maintenance1` promotion 后，`VkSwapchainPresentFenceInfoKHR` 和 `VkSwapchainPresentFenceInfoEXT` 是同一结构（typedef alias, `vulkan_core.h:18411`），sType 值相同。代码使用 EXT 命名形式，功能等价于 KHR。

## 外部依赖（通过父 CMake 链接）

- `rllogger` — `../logger/` 同级库，`RGL::RLLogger` 单例
- `spdlog` — 通过 rllogger 间接使用
- `glm` — 数学库（强制 depth zero-to-one，弧度制）
- `SDL3` — 窗口/surface
- `VulkanMemoryAllocator` — 内存管理
- `slangc` — 着色器编译器（`$HOME/program/slang/build/Release/bin/slangc`）

## 关键文件位置

| 位置 | 说明 |
|---|---|
| `vkresources.cppm:121-154` | `writeDescriptors()`，使用 `vkWriteResourceDescriptorsEXT` |
| `vkrenderer.cppm:143-153` | `colorToDrawBarrier`（Undefined → ColorAttachmentOptimal, srcStage=eTopOfPipe） |
| `vkrenderer.cppm:176-197` | Heap barrier、`bindResourceHeapEXT`、`pushDataEXT`（均在 `beginRendering` 之前） |
| `vkrenderer.cppm:225` | `SwapchainPresentFenceInfoEXT`（KHR 别名），位于 `presentInfo.pNext` |
| `shader.cppm:38-56` | `DescriptorSetAndBindingMappingEXT`，映射 heap slots 8 和 12 |
| `shaders/types.slang:20-24` | 两个缓冲均使用 `ByteAddressBuffer`（绝不用 `StructuredBuffer`） |
| `shaders/cube.mesh.slang:15` | `mul(drawArgs.mvp, ...)` MVP 变换 |

## 已知问题

- `pickPhysicalDevice` 读取 `VK_AMD_shader_core_properties` 时未启用对应扩展 — 非 AMD 上为 UB，AMD 上可能读到垃圾数据。当前在 AMD Strix Halo 上开发；未来兼容 NVIDIA Blackwell 等需要正确的扩展检查回退
- `computeCommandPool`、`transferCommandPool`、`computeQueue`、`transferQueue` 已创建但保留给未来使用
- `VmaBuffer`/`VmaImage` 存储原始 `VmaAllocator` 指针 — 仅在 allocator 生命周期长于所有分配时安全；可能是为避免 vk-hpp RAII double-free
- `utils.cppm` — Vulkan ERROR 触发 `int $3`（SIGTRAP）用于调试器体验；从功能角度应改为 throw
- `VmaDescriptorHeap` 已启用 — 实例化在 `vkresources.cppm`，管理单个 VMA buffer 存放 descriptor 数据

## 其他文档

- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、shader 缓冲访问规则
- `docs/调试历史.md` — 灰屏问题修复时间线、已解决的争议
- `docs/DESCRIPTOR_HEAP_SPEC.md` — 有关DescriptorHeap 扩展的排坑详细记录
- `docs/next_roadmap.md` — 后续开发计划
- `docs/shader_reflection_plans.md` — shader反射开发计划的一些技术调研
- `docs/快捷键配置.md` — Zed 编辑器快捷键、任务和调试配置说明
