# FSR vendor 改动清单

状态：**现行**（随 vendor 树走）

vendor = `third_party/fsr/vendor/`，来源 = FidelityFX-SDK tag **v1.1.4**（commit `c6efa6b`）的
`include/` + `src/` **include 闭包裁剪版**（95 文件 / 1.8 MiB，原 `sdk/` 13 MiB）。

## 改法约定（重要）

**所有对 vendor 源码的改动都以 `// RGL-PATCH(Pn)` 注释就地标出**，本文件是这些标记的根因索引。
两条纪律：

1. **不逐处改调用点**。MSVC-isms 一律收进 `compat/ffx_posix_compat.h`，由 `CMakeLists.txt`
   `force-include` 进本 target——这样 vendor 树本身保持可与上游 diff。
2. **升级 vendor 时**：先 `grep -rn RGL-PATCH vendor/` 拿到全部改动点，逐条对上游重放。
   本文件里每条都写了「上游为什么在 Windows 上不出事」，判断上游是否已修时要用到它。

## 补丁逐条

| # | 位置 | 症状 | 根因 |
|---|---|---|---|
| P1 | `include/FidelityFX/host/ffx_util.h` | 编译失败：`std::popcount` 未声明 | `ffxCountBitsSet` 用 `std::popcount` 却没 `#include <bit>`；`ffx_vk.cpp` / `ffx_fsr3upscaler.cpp` 用 `memset`/`memcpy` 却没 `<cstring>`。MSVC 的头传递性带进来，clang + libc++ 不带 |
| P2 | `src/backends/vk/ffx_vk.cpp`（`ConvertUTF*`） | 编译失败：`<codecvt>` 不存在 | 非-Windows 分支用 `std::wstring_convert<std::codecvt_utf8<wchar_t>>`——该类 C++17 起弃用、**libc++ 19+ 已删除**、头本身 C++26 已撤。改成手写 UTF-8 ↔ UTF-32。<br>⚠ phase14_plan §1.4 原写「`<codecvt>` 是死分支」**说反了**：它在非-Windows 分支，Windows 分支走 `MultiByteToWideChar` |
| P3 | `compat/ffx_posix_compat.h`（新文件） | 编译失败：`strcpy_s` / `wcscpy_s` 等未声明 | MSVC 安全 CRT 只在 `<windows.h>` 里。非-Windows 给最小替身，靠 force-include 注入，不改 vendor 调用点 |
| P4 | `src/backends/vk/ffx_vk.cpp` | 编译失败：`placement new` / `log2` 未声明 | 缺 `<new>` / `<cmath>`，同 P1 的头传递性差异 |
| P5 | `src/shared/ffx_message.cpp` | 编译失败：`FFX_UNUSED` 未声明 | 非-Windows 分支用 `FFX_UNUSED` 却没 include 定义它的 `ffx_util.h`（Windows 分支靠 `<windows.h>` 带进来） |
| P6 | `include/FidelityFX/host/ffx_types.h`（`FFX_SDK_DEFAULT_CONTEXT_SIZE`） | 编译失败：`static_assert` 炸（`ffx_fsr3upscaler.cpp:1205`） | 不透明 context 的容量按 **MSVC ABI** 定：那里 `wchar_t` 是 2 字节。`FfxPipelineState` 含 4×64+3 个 `FfxResourceBinding`，每个带 `wchar_t name[64]` ⟹ Linux（`wchar_t`=4）下单条 pipeline 从 ~35 KB 涨到 ~70 KB，FSR3-upscaler 的 private context（12 条 pipeline，837,848 B）撑爆 524,288 B。非-Windows 抬到 1 MiB。<br>⚠ **ABI 常量**：所有分配 `FfxFsr3UpscalerContext` 的编译单元必须取到同一个值（故只能在此头里改，不能在 target 里 `-D`） |
| P7 | `port/ffx_swapchain_framegen_stub.cpp`（新文件） | 链接失败：缺 `ffxSetFrameGenerationConfigToSwapchainVK` | `ffx_vk.cpp:349` 无条件取它的地址，而实现在 `FrameInterpolationSwapchain/`——本 vendor 树**故意不含**（DXGI/DX12 的 present 替换，§4 明确不做）。给**显式失败**的桩而非空实现：调用它 = 用了没带的功能，必须出声 |
| P8 | `src/backends/vk/ffx_vk.cpp`（函数表填充） | **运行期跳 0x0** | `vkGetBufferMemoryRequirements2KHR` 在 Vulkan 1.1 已进核心，`...2KHR` 只是别名兼容名——**没启用 `VK_KHR_get_memory_requirements2` 时 `vkGetDeviceProcAddr` 对它返回 NULL**（Windows loader 宽容地给别名，Linux loader 1.4 不给）。原代码不检查就在 `:1720` 调用。退回核心名，PFN 同签名 |
| P9 | `include/FidelityFX/host/ffx_fsr3upscaler.h`（`FfxFsr3UpscalerContext`） | **-O2 段错误，-O0 正常** | 原 `typedef struct { uint32_t data[N]; }` **对齐只有 4**，而 `ffx_fsr3upscaler.cpp:511` 把它直接当 `FfxFsr3UpscalerContext_Private*` 用（含指针成员、要求 8 对齐）⟹ UB。`-O0` 侥幸不炸，`-O2` 下 clang 按对齐假设发对齐 SSE 存指令，静态对象落在 4-mod-8 地址就 SIGSEGV。改 union 把对齐抬到 8（`sizeof` 不变，兼容 C） |
| P10 | `src/backends/vk/ffx_vk.cpp`（`ffxGetScratchMemorySizeVK` + 切分处） | **-O2 段错误，-O0 正常**（UBSan：`member access within misaligned address ... 'EffectContext' requires 32 byte alignment`） | scratch arena 是一整块按字节切分的裸内存，上游每个子数组只 `FFX_ALIGN_UP(..., sizeof(uint32_t))`、总量只对齐到 `sizeof(uint64_t)`；而 `BackendContext_VK::EffectContext` 声明了 `alignas(32)` ⟹ `pEffectContexts` 常落在 8 对齐地址。统一按 64 对齐每个切片（`FFX_VK_SCRATCH_ALIGN`），基址先上取整，总量预留余量。**尺寸计算与实际切分两处必须同步**，故共用该常量 |

P1–P6 = 编译期（clang/libc++ 与 MSVC 的差异）；P7 = 链接期；P8–P10 = 运行期，且
**P9/P10 只在 `-O2` 暴露**——这就是「`-O0` 冒烟过了」不能当验收的原因。

## 已知上游问题（VVL 实测，非本仓引入）

`just fsr-smoke` 在 **AMD Radeon 8060S Graphics (RADV STRIX_HALO) / Mesa 26.2.3 + VVL** 上
15 条报错，全部归因上游；冒烟程序对它们建**白名单 + 计数**（`smoke/fsr_smoke.cpp`），
**白名单外的新报错直接 FAIL**，上游升级时靠这个发现回归。

| 报错 | 次数 | 上游根因 |
|---|---|---|
| `VUID-vkAllocateMemory-deviceCoherentMemory-02790` | 10 | 后端选到带 `VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD` 的 memory type，却没启用 `deviceCoherentMemory` 特性。RADV 上该位与 `DEVICE_LOCAL` 同型，选类型时没排除 |
| `VUID-VkImageSubresourceRange-layerCount-01721` | 2 | `ffx_vk.cpp:4223` 的 `vkCmdClearColorImage` 传了 `layerCount = 0` 的 subresource range |
| storage image 格式不匹配（`which doesn't match the VkImageView`） | 2 | **luma history 的读写格式不一致**：写侧 `ffx_fsr3upscaler_callbacks_glsl.h:398` 声明 `layout(..., rgba8) uniform image2D rw_luma_history`，而 host 侧 `ffx_fsr3upscaler.cpp:592/595` 把资源建成 `FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT`。VVL 判「整个 image 的读写值未定义」。<br>DX12 上 UAV 格式取自资源、`rgba8` 限定符被忽略，所以 Windows 路径看不出问题——**这是 GLSL/Vulkan 路径独有的**。接真实画面前必须处置（要么改 shader 的 layout 限定符，要么改资源格式），否则 luma history 是 UB |
| `VUID-vkDestroyDevice-device-05137` | 1 | `ffxFsr3UpscalerDeinitialize` 之后仍有 18 个对象未销毁（后端内部 `VkDeviceMemory` / `VkImage` / `VkCommandBuffer` 泄漏） |

## 与 phase14_plan 的对应

- §1.4「`-reflection` 输出对 VK 路径无用」**是错的**：`ffx_vk.cpp:3330+` 用
  `FfxShaderBlob::boundSRVTextures/Counts/Spaces` 逐条搭 `VkPipelineLayout`，反射表必需。
  生成器因此自带 SPIR-V 反射（不引 spirv-reflect）。
- §1.4「≈40 个 SPIR-V 产物」**低估了**：排列是 6 个二值选项的笛卡尔积 = 64/pass，
  10 pass × 4 variant = 2560 次编译；靠内容 MD5 去重后塌到 **80 个唯一模块 / 1.7 MiB**。
- §5 风险 2（1.1.4 VK 后端在 RADV 上跑不起来）**已排除**：`just fsr-smoke` 两帧
  `ffxFsr3UpscalerContextDispatch` 全部 dispatched + executed，VVL 无白名单外报错。
