# vulkan_practice

Vulkan modern-feature learning project. Mesh shader pipeline with ShaderObject + DescriptorHeap + DynamicState.

## Build

```bash
# From repo root /home/robin/program/RGL
cmake --build build --target vulkan_practice -j$(nproc)
# Binary: build/vulkan_practice/vulkan_practice
```

## Runtime requirements

- `RADV_EXPERIMENTAL=heap` — required on RADV to enable `VK_EXT_descriptor_heap`

CMake has no standalone build dir — always build from root (`cmake -B build -S .`).

## Naming convention

**All C++ symbols are in `namespace RGL`.** Every `.cppm` module wraps its exports in `namespace RGL { ... }`. `main.cpp` uses `using namespace RGL;` for brevity.

Cross-module references between init-layer classes (e.g., `VkDevice` → `VkInstance`) use `RGL::` prefix because each class is already inside the namespace. When calling functions/types from utility modules (e.g., `RGL::pickPhysicalDevice()`, `RGL::QueueManager`, `RGL::FeatureChain`), also use `RGL::` prefix.

## Module structure (C++20 modules, .cppm)

All exports are inside `namespace RGL`.

### Initialization layer (construction order = main.cpp order)

| Module | File | Purpose |
|---|---|---|
| `vk_instance` | vkinstance.cppm | Context, Instance, DebugMessenger, apiVersion |
| `vk_device` | vkdevice.cppm | PhysicalDevice selection, QueueManager, Device, CommandPools, FeatureChain |
| `vk_swapchain` | vkswapchain.cppm | Surface, Swapchain, ImageView[] format/extent/presentMode |
| `vk_resources` | vkresources.cppm | VmaAllocatorRAII, depth image+view, VmaDescriptorHeap (slot 0=vertices, slot 1=indices) |
| `vk_renderer` | vkrenderer.cppm | RenderPipeline, sync objects, command buffers, render loop |

### Utility modules

| Module | File | Purpose |
|---|---|---|
| `vulkan_utils` | utils.cppm | layer check, debug messenger, GPU picker, barrier helper, present mode |
| `vulkan_window` | sdl3window.cppm | SDL3 window, surface, HDR/SDR format selection |
| `queue_family_manager` | QueueFamilyManager.cppm | Graphics/Compute/Transfer queue allocation |
| `features` | features.cppm | FeatureChain typedef, buildRequestedFeatures, validateFeatures, checkDeviceExtensions, custom exceptions |
| `cube` | cube.cppm | Vertex/Index data, DrawArgs struct (pushed via pushDataEXT) |
| `shader` | shader.cppm | SPV loader, ShaderEffect, RenderPipeline (ShaderObject + dynamic state) |
| `VmaRaii` | VmaBuffer.cppm | VmaBuffer, VmaImage, VmaDescriptorHeap, VmaAllocatorRAII wrappers |

`main.cpp` constructs the 5 init-layer classes in order then calls `vkRenderer.render(window)`. `vam_impl.cpp` contains `#define VMA_IMPLEMENTATION`.

### Construction dependency graph

```
SDLWindow → VkInstance → VkDevice → VkSwapchain → VkResources → VkRenderer
```

main.cpp destructs in reverse stack order (RAII, no manual cleanup).

### Important member lifetime rules

- `vkinstance.cppm`: `engineInfo_`, `instanceCreateInfo_`, `layers_`, `instanceExtensions_` are class members — `pApplicationInfo` must not dangle
- `vkdevice.cppm`: `featuresChain_` is a class member — `DeviceCreateInfo.pNext` must not dangle
- All `vk::raii::*` objects are initialized via **member initializer lists**, never in body (default constructors deleted)
- Member declaration order must match initialization dependency order

## Shader compilation

Slang shaders in `shaders/` → SPV output to `build/vulkan_practice/generated_shaders/`.

- CMake `compile_slang()` function handles compilation at build time
- Profile: `sm_6_6+spirv_1_6`, capability: `spvDescriptorHeapEXT`
- Runtime reads SPV from `generated_shaders/` relative path — must run from build dir or provide correct path

## Implicit extension dependencies

`features.cppm` 和 `vkinstance.cppm` 中请求的 `VK_EXT_DEVICE_GENERATED_COMMANDS`、`VK_EXT_SWAPCHAIN_COLOR_SPACE` 等扩展当前无显式使用，但被其他已启用的扩展隐式依赖。移除它们可能导致驱动层面功能异常，不可随意删除。

## Key Vulkan patterns used

- **Synchronization2** — all barriers use `pipelineBarrier2` + `vk::DependencyInfo`, not legacy `pipelineBarrier`
- **Dynamic rendering** — no render passes, uses `beginRendering` + `RenderingInfo`
- **ShaderObject** — no pipeline creation, uses `bindShadersEXT` + dynamic state
- **Push data** — uses `vkCmdPushDataEXT` to push `DrawArgs` to descriptor heap offset 0
- **DescriptorHeap** — `VmaDescriptorHeap` manages a single VMA-allocated GPU-visible buffer; data written via CPU-mapped slots, pushed via `vkCmdPushDataEXT`

## Dependencies (external, linked via parent CMake)

- `rllogger` — `../logger/` sibling library, `RGL::RLLogger` singleton
- `spdlog` — used indirectly via rllogger
- `glm` — math (force depth zero-to-one, radians)
- `SDL3` — window/surface
- `VulkanMemoryAllocator` — memory management
- `slangc` — shader compiler (`$HOME/program/slang/build/Release/bin/slangc`)

## Known issues to watch

- `pickPhysicalDevice` reads `VK_AMD_shader_core_properties` without enabling the extension — UB on non-AMD, garbage data on AMD. Currently developing on AMD Strix Halo; future compatibility with NVIDIA Blackwell and others needs proper extension checking fallback.
- `computeCommandPool`, `transferCommandPool`, `computeQueue`, `transferQueue` created but reserved for future use.
- `VmaBuffer`/`VmaImage` store raw `VmaAllocator` pointer — safe only if allocator outlives all allocations; likely chosen to avoid vk-hpp RAII double-free.
- `utils.cppm` — Vulkan ERROR triggers `int $3` (SIGTRAP) for aesthetic debugger experience; functionally a throw would be more appropriate.
- `VmaDescriptorHeap` is now active — instantiated in `vkresources.cppm`, manages single VMA buffer for descriptor data.

## Design decisions

### GPU scoring: `pickPhysicalDevice` (utils.cppm)

Score weights: `coreCount * 1000` + `vramGB * 10` + discrete `+500` / integrated `+200`.

**Rationale:** Stream processor count is the dominant performance indicator, followed by VRAM, then GPU type. This prevents mis-ranking a strong iGPU behind a weak dGPU (e.g., a high-end integrated GPU paired with a basic "lights-on" discrete card).

### Present mode: `eImmediate` over `eMailbox` (utils.cppm:195)

**Rationale:** On Wayland, the compositor already manages triple-buffering and full-frame synchronization. Using `eImmediate` avoids redundant buffer stacking that `eMailbox` would add.

### Swapchain image count: Wayland不加 1 (sdl3window.cppm:130)

**Rationale:** Wayland compositors (e.g., KWin) already maintain their own triple-buffer. Adding +1 would create quadruple-buffering, increasing latency unnecessarily. X11 still gets +1 for backwards compatibility.

### Viewport Y-axis flip (shader.cppm:94-101)

`viewport(0, height, width, -height, 0, 1)` inverts Y.

**Rationale:** Vulkan's NDC Y-axis points upward; window/screen coordinates point downward. The flip aligns the viewport with the expected coordinate system.

### Dual fence: inFlight + present (vkrenderer.cppm)

Two fence sets (`inFlightFences_` + `presentFences_`) instead of a single fence.

**Problem:** With triple-buffer swapchain (KWin compositor), a single fence attached to `queueSubmit` cannot track the asynchronous `presentKHR` operation. When the fence signals, present may still be pending, causing semaphore reuse violation:

```
Swapchain image 0 was presented but was not re-acquired, so VkSemaphore ... may still be in use and cannot be safely reused with image index 1.
(VUID-vkQueueSubmit-pSignalSemaphores-00067)
```

**Solution:** Leverage `VK_KHR_swapchain_maintenance1`:
- `inFlightFences_` — attached to `queueSubmit`, tracks command buffer completion
- `presentFences_` — attached via `SwapchainPresentFenceInfoEXT` in `presentInfo.pNext`, tracks present completion

At the start of `drawFrame()`, both fences are waited and reset together, ensuring the corresponding semaphore and command buffer are safe to reuse only after submit and present have both completed.

### PresentFenceInfoEXT (vkrenderer.cppm:206)

Uses `vk::SwapchainPresentFenceInfoEXT` in `presentInfo.pNext`.

**Rationale:** Modernized synchronization — leverages the extension for cleaner present-side fence tracking.

### DrawArgs over PushConstants (cube.cppm)

`DrawArgs` is pushed via `vkCmdPushDataEXT`, **not** traditional Vulkan push constants. Naming matters: `PushConstants` implies the 128-byte limited `pushConstants` range, whereas `pushDataEXT` writes to the descriptor heap and is unbounded.

**Layout rule — Slang UBO alignment:** Slang applies std140-like UBO alignment to `[[vk::push_constant]]` structs. When a `uint32_t` precedes a `uint64_t`, the `uint64_t` gets padded to 8-byte alignment. C++ and Slang layouts must match exactly:

```cpp
// C++ — explicit padding
struct DrawArgs { glm::mat4 mvp; uint32_t vertexOffset; uint32_t indexOffset; uint64_t _padding; uint64_t heapAddress; };
```
```slang
// Slang — must match 1:1
struct DrawArgs { float4x4 mvp; uint32_t vertexOffset; uint32_t indexOffset; uint64_t _padding; uint64_t heapAddress; };
```

Omitting `_padding` causes mismatched byte ranges → Vulkan validation error `vkCmdPushDataEXT was never called for range [81, 87)`. Always keep C++ and shader struct layouts identical field-by-field.


