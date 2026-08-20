# vulkan_practice

Vulkan 现代特性学习项目。Mesh shader 渲染管线，使用 ShaderObject + DescriptorHeap + DynamicState。  
核心代码位于vulkan_practice目录下。  
legacy目录下的代码是陈旧的OpenGL项目已经不是当前工作重点，一般不用看，但是有些思路可以进行参考。  

## 给大模型
优先使用中文回复用户，除非用户说使用英文回复。

## 构建

项目使用 `just` 管理构建流程（参见 `Justfile`）。

```bash
just config              # CMake 配置（默认 clang, Release）
just config gcc Debug    # 指定编译器和构建类型
just build               # 增量编译（需先 config）
just build-release       # 一键配置 + 编译 Release
just build-debug         # 一键配置 + 编译 Debug
just run                 # 运行程序（自动检测：AMD VGA 存在时设置 RADV_EXPERIMENTAL=heap）
just run-igpu            # 强制核显（AMD 集显原生渲染，无 offload）
just run-dgpu            # 强制独显（NVIDIA prime offload，PROVIDER=3）
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

## 代码规范

### 模块优先

所有新代码**必须**使用 C++20 modules (`.cppm`)，而非传统的 `.hpp/.cpp` 头文件对。
- `.cppm` 模块声明于 `namespace RGL { ... }` 内
- `main.cpp` 等入口文件通过 `import` 导入模块
- 不要在模块外部使用 `#include` 引入项目内部头文件

### 优先使用 rlstd 代替 #include STL

**所有 `.cppm` 模块必须 `import rlstd;` 获取 STL 类型，而非在 GMF 中 `#include` STL 头文件。**

- GMF 中只保留第三方库头文件（Vulkan、SDL、glm、fmt 等）
- 所有 STL 类型（`std::vector`、`std::string`、`std::map` 等）通过 `import rlstd;` 获取
- **例外：** `<ranges>` 的 `std::views::` 因 libstdc++ 定义为 namespace alias 无法 re-export，需保留 `#include <ranges>`；`<filesystem>` 同理
- 新加 STL include 时，先检查是否应补入 `rlstd.cppm` 而非直接加到 GMF

### 函数式 Ranges

优先使用 C++23 `std::ranges` 进行函数式编程：
- 使用 `std::views::filter`、`std::views::transform` 等视图适配器
- 使用 `std::ranges::for_each` 等算法替换原始循环
- 链式组合视图操作，避免中间容器

### 成员变量命名

成员变量使用自然命名法，不加 `_` 或 `m_` 前缀后缀：
```cpp
class Foo {
    int count;      // ✅
    int _count;     // ❌
    int m_count;    // ❌
    int count_;     // ❌
};
```

### 构建工具模块

`tools/` 下的构建工具也统一使用 C++20 modules：

| 模块 | 文件 | 用途 |
|---|---|---|
| `schema_loader` | tools/schema_loader.cppm | Schema 结构体 + Lua schema 加载 |
| `validator` | tools/validator.cppm | 将 reflection JSON 与 schema 校验一致性 |
| `generator` | tools/generator.cppm | 使用 inja 模板生成 `.cppm`，接入 `rllogger`。新增 `renderTemplate()` 支持任意 JSON + inja 模板的渲染 |

`reflection_tool` 通过 `import rllogger;` 使用项目统一日志库，替代 `std::cout`/`std::cerr`。

## 模块结构（C++20 modules, .cppm）

所有导出均在 `namespace RGL` 内。

### 初始化层（构造顺序 = main.cpp 顺序）

| 模块 | 文件 | 用途 |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context、Instance、DebugMessenger、apiVersion |
| `vk_device` | vkdevice.cppm | PhysicalDevice 选择、QueueFamilyManager、Device、CommandPools、FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface、Swapchain、ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VmaAllocatorRAII、depth image+view、顶点/索引缓冲、VmaDescriptorHeap（slot 运行时动态分配：`reservedSlots() + HeapSlot*Offset`，偏移由 `types_generated` 生成） |
| `vk_renderer` | vkrenderer.cppm | 渲染管线、同步对象、命令缓冲、渲染循环 |

### 工具模块

| 模块 | 文件 | 用途 |
|---|---|---|
| `vulkan_utils` | utils.cppm | 层检查、debug messenger、GPU 选择器、barrier 辅助、present mode |
| `vkerror` | vkerror.cppm | `Error`/`ErrorCode` 类型定义、`fromVkResult` |
| `ecs` | ecs.cppm | `Viewport` context、eventSystem/resizeSystem/renderSystem 渲染循环（entt registry，返回 `std::expected`） |
| `vulkan_window` | sdl3window.cppm | SDL3 窗口、surface、HDR/SDR 格式选择 |
| `queue_family_manager` | QueueFamilyManager.cppm | Graphics/Compute/Transfer 队列分配 |
| `features` | features.cppm | FeatureChain 类型定义、buildRequestedFeatures、validateFeatures、checkDeviceExtensions、自定义异常 |
| `cube` | cube.cppm | 顶点/索引数据 |
| `shader` | shader.cppm | SPV 加载器、ShaderEffect、RenderPipeline（ShaderObject + 动态状态） |
| `VmaRaii` | VmaBuffer.cppm | VmaBuffer、VmaImage、VmaDescriptorHeap、VmaAllocatorRAII 封装。VmaBuffer/VmaImage 通过 `SharedAllocator` (`std::shared_ptr<AllocatorControlBlock>`) 共享 allocator 所有权 |
| `rlstd` | rlstd/rlstd.cppm | C++ 标准库模块封装。libstdc++ 不支持 `import std`，因此通过 GMF `#include` 所有 STL 头文件并 `export` 常用名称。所有项目模块通过 `import rlstd;` 获取 STL 类型，而非在 GMF 中 `#include` STL 头。注意：`std::views::` 因 libstdc++ 将其定义为 namespace alias 而无法通过模块导出，仍需在 GMF 中 `#include <ranges>` |
| `types_generated` | generated/types_generated.cppm | auto-generated（由 `reflection_tool` + inja 模板生成），导出 `DrawArgs`、`HeapSlotVertexOffset`、`HeapSlotIndexOffset`（相对 heap reserved range 起点的偏移，运行时 slot = `reservedSlots() + offset`） |
| `features_generated` | generated/features_generated.cppm | auto-generated（由 `reflection_tool render` + `features_config.json` 生成），导出 `FeatureChain`、`buildRequestedFeatures()`、`validateFeatures()` |

`main.cpp` 先创建栈上 `entt::registry`（持有 `Viewport` context），按顺序构造 5 个初始化层类，然后调用 `RGL::renderSystem(registry, vkRenderer, window)`。`vam_impl.cpp` 包含 `#define VMA_IMPLEMENTATION`。

### 构造依赖图

```
SDLWindow → AppContext → DeviceContext → Swapchain → Resources → Renderer
                └────────────── entt::registry（Viewport context）──────────┘
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

- `rlstd` — `rlstd/` 根目录共享库，C++ 标准库模块封装
- `rllogger` — `logger/` 根目录共享库，`RGL::RLLogger` 单例
- `spdlog` — 通过 rllogger 间接使用
- `glm` — 数学库（强制 depth zero-to-one，弧度制）
- `SDL3` — 窗口/surface
- `VulkanMemoryAllocator` — 内存管理
- `slangc` — 着色器编译器（`$HOME/program/slang/build/Release/bin/slangc`）

## 关键文件位置

| 位置 | 说明 |
|---|---|
| `vkresources.cppm:139` | `writeDescriptors()`，使用 `vkWriteResourceDescriptorsEXT` |
| `vkrenderer.cppm:143` | `colorToDrawBarrier`（Undefined → ColorAttachmentOptimal, srcStage=eTopOfPipe） |
| `vkrenderer.cppm:188-206` | Heap barrier、`bindResourceHeapEXT`、`pushDataEXT`（均在 `beginRendering` 之前） |
| `vkrenderer.cppm:251` | `SwapchainPresentFenceInfoEXT`（KHR 别名），位于 `presentInfo.pNext` |
| `shader.cppm:38-56` | `DescriptorSetAndBindingMappingEXT`，映射顶点/索引 heap slot（编号运行时动态，见 `vkresources.cppm` 的 `getHeapSlotVertex/Index`） |
| `shader.cppm:6` | `import types_generated;` 提供 `HeapSlotVertexOffset`/`HeapSlotIndexOffset` |
| `vkrenderer.cppm:21` | `import types_generated;` 提供 `DrawArgs` 结构体（用于 :49 `updateDrawArgs`、:101 成员） |
| `shaders/types.slang:20-24` | 两个缓冲均使用 `ByteAddressBuffer`（绝不用 `StructuredBuffer`） |
| `shaders/cube.mesh.slang:19` | `mul(drawArgs.mvp, ...)` MVP 变换 |

## 已知问题

- `computeCommandPool`、`transferCommandPool`、`computeQueue`、`transferQueue` 已创建但保留给未来使用

## 其他文档

- `docs/技术概念.md` — 核心 Vulkan 模式、设计决策、shader 缓冲访问规则
- `docs/调试历史.md` — 灰屏问题修复时间线、已解决的争议
- `docs/DESCRIPTOR_HEAP_SPEC.md` — 有关DescriptorHeap 扩展的排坑详细记录
- `docs/next_roadmap.md` — 后续开发计划
- `docs/shader_reflection_plan.md` — shader反射开发计划的一些技术调研
- `docs/nvidia_compatibility_report.md` — NVIDIA 兼容性排查（heap 对齐、Aftermath 集成、mesh shader 崩溃根因）
- `docs/快捷键配置.md` — Zed 编辑器快捷键、任务和调试配置说明

## min_mesh_crash（最小复现子项目）

独立 CMake 的单文件最小复现程序（2026-08 调查 NVIDIA mesh shader 崩溃时建立）：

- `main.cpp` — vertex+fragment 与 mesh shader 的 shader object 测试（GPU 选择写死 vendor 0x10de；强制 RADV 用 `VK_DRIVER_FILES`）
- `shaders/` — `mesh.glsl`（mesh-only，无 task）、`vert.glsl`、`frag.glsl`
- `README.md` — 根因权威记录：mesh shader 对象**必须**带 `VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT`（TASK stage 绑 NULL 时），否则 NVIDIA 上 Xid 13/engine reset；VVL 查不出来是 spec 漏洞（VUID 08694 需 taskShader+meshShader 双 feature）
- 构建/运行：`cmake -B build -S . && cmake --build build`，运行需 prime offload 环境变量（见其 README）
