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
| P11 | `include/FidelityFX/gpu/fsr3upscaler/ffx_fsr3upscaler_callbacks_glsl.h`（`rw_luma_history`） | VVL 每次 dispatch 一条：storage image 的 `OpTypeImage` Format 与 `VkImageView` 不符，判「**整个 image 读写值未定义**」 | 写侧声明 `layout(..., rgba8)`，host 侧却把资源建成 `FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT`（`ffx_fsr3upscaler.cpp:592/595`）。**权威口径 = HLSL 回调的 `RWTexture2D<FfxFloat32x4>`**（DX 的 UAV 格式取自资源、限定符被忽略 ⟹ Windows 永远看不见这条）⟹ 改限定符对齐资源（`rgba16f`），**不是**反过来把资源降成 8-bit：存进去的是 4 槽 luma 历史，HDR 下 luma 可远大于 1，UNORM 会同时截顶并把精度压到 1/255 |
| P12 | `src/backends/vk/ffx_vk.cpp`（`findMemoryTypeIndex`） | VVL 每帧数十条 `VUID-vkAllocateMemory-deviceCoherentMemory-02790` | 本后端从不启用 `deviceCoherentMemory` 特性，而 RADV 把某个 memory type 同时标成 `DEVICE_LOCAL\|DEVICE_COHERENT` ⟹ 「只要 device-local」那条匹配选中它 = 未启用特性就分配 coherent 内存。跳过该位（VMA 同做法）。<br>⚠ 这条在 `just fsr-smoke` 里原本是**白名单**条目；渲染器侧接 FSR（批 14.5）后它会按帧刷屏， 故就地修掉而不是继续白名单 | 
| P13 | `src/backends/vk/ffx_vk.cpp`（clear job 的 `VkImageSubresourceRange`） | `vkCmdClearColorImage` 报 `VUID-VkImageSubresourceRange-layerCount-01721`（9 条/次） | `layerCount` 直接取 `resourceDescription.depth`，而 **TEXTURE2D 的 `depth` 是未填的 0** ⟹ 整条 clear 无效。`depth ? depth : 1`。同 P12：原本是 `just fsr-smoke` 白名单条目，渲染器侧会按帧刷屏，故修而不白名单 | 
| P14 | `src/components/fsr3upscaler/ffx_fsr3upscaler.cpp`（`lanczos2()`） | **没有任何机制出声**：FSR 时域档画面发糊（1:1 静止机位边缘能量只有不接 FSR 的 46%）。不崩、无 VVL、警告被降级 | `return abs(value) < FFX_EPSILON ? 1.f : <闭式>` 是**非限定调用**，而 libstdc++ 的 `<cmath>` 只把浮点 `abs` 重载放进 `std::`，全局作用域只有 `<stdlib.h>` 的 `::abs(int)` ⟹ **浮点参数被截断成整数**：`|v| < 1` 一律截成 0 ⟹ `0 < 1e-6` 成立 ⟹ **`lanczos2(v)` 在整个 `|v| < 1` 区间返回 1.0**。转折点精确落在 `|v| = 1`（整数截断的签名；`FFX_EPSILON = 1e-06f` 排除了"epsilon 本身很大"这个竞争假设）。<br>**后果链**：host 生成的 128×1 Lanczos LUT 退化成**阶跃**（实测 i=0…63 全存 `32767`，i=64 起才回到真核值）⟹ **Lanczos2 在 x=1 的零点没了** ⟹ VK 后端 `SampleLanczos2Weight` 采它做历史读取权重 ⟹ t=0 的核从 `[0 1 0]` 变成 **`[¼½¼]`**（σ=0.707 的 3×3 低通）⟹ 再被 accumulate 的 α≈2.3% EMA **放大 ~44 倍**。<br>**上游为什么在 Windows 上不出事**：MSVC 的 `<math.h>`/`<corecrt.h>` 在**全局命名空间**提供 inline 浮点 `abs` 重载 ⟹ `abs(float)` 正常。本机实测 **gcc 与 clang 在 libstdc++ 下都出错**；clang 会发 `-Wabsolute-value`，但本 target 按「vendor 警告不背账」加了 `-Wno-error`（`third_party/fsr/CMakeLists.txt:61`）⟹ **这条警告我们的构建一直在打印，只是没人读**。修法 = `fabsf`。同类面已扫：vendor host 代码里非限定 `abs(` 现 **0 命中** |

**P1–P6 = 编译期（clang/libc++ 与 MSVC 的差异）；P7 = 链接期；P8–P10 = 运行期崩溃；
P11–P13 = VVL 会出声的语义/参数错；P14 = 第一条「谁都不出声、只是结果错」的**——这类最危险：
没有任何机制会替你发现它，只能靠**判据**（本例 = 时域档锐度回归）。而 P14 同时暴露一个自伤：
`-Wno-error` 是整块压的，把编译器**默认就开**的 `-Wabsolute-value` 一起吞了（护栏见 §P14 配套）。
**P9/P10 只在 `-O2` 暴露**——这就是「`-O0` 冒烟过了」不能当验收的原因。P11 = 语义正确性。

### P9/P10 的终局确认（UBSan 双向）

「UBSan 报 0 条」有两种成因，所以两头都要验：

| 构型 | 结果 |
|---|---|
| 现状（P9 + P10 在位），`-O2 -fsanitize=undefined -fno-sanitize-recover=undefined` | **0 条 runtime error**，冒烟 PASS，rc=0 |
| 变异：`FFX_VK_SCRATCH_ALIGN` 64→4（并临时摘掉 P10 的 static_assert，否则护栏先拦下这个变异） | `ffx_vk.cpp:1806: member access within misaligned address ... for type 'EffectContext', which requires 32 byte alignment`，rc=1 |

⟹ 探测器接得上，「0 条」是真零。复现手法：手编一份 UBSan 版（**别走 CMake**——命令行传
`-DCMAKE_CXX_FLAGS` 会覆盖项目自己设的 modules 扫描标志，configure 直接 Generate 失败）。

⚠ 别把这两条误认成 FSR 常见的那两类 UB：① 「调用方没把结构体归零 / `header.type` 填错」是
**新版 ffx-api（1.2+）**的形态，1.1.4 的 C API 里 `FfxInterface` 与
`FfxFsr3UpscalerContextDescription` **根本没有 `header`/`pNext` 字段**（且冒烟全部 `{}` 值初始化）；
② 「DXC 在 -O2 下 miscompile」用不上——本路径是 glslang，且崩的是 **host 侧 C++ 的 memset**，
发生在任何 shader 跑之前。

### P14 配套：让这一类 bug **必须让构建失败**

P14 的类别特征 = **没有任何机制出声**。所以配套护栏的目标不是"再提醒一次"，而是**把它变成构建失败**。
clang 本来就发 `-Wabsolute-value`，问题从来不是缺诊断，而是**两道把诊断咽下去的自伤**：

| 咽掉诊断的地方 | 修法 |
|---|---|
| vendor target 整块 `-Wno-error`（本文件原注释写"逐条静音、保留其余可见"，**实现其实没做逐条**） | 保留 `-Wno-error`（风格类警告不该让我们构建失败），但把**"这是 bug 不是风格"**那一类用 `-Werror=<名>` 提升回来。顺序敏感：后面的 `-Werror=<名>` 覆盖前面的 `-Wno-error`。表在 `third_party/fsr/CMakeLists.txt` 的 `FFX_BUG_CLASS_WARNINGS` |
| **`just build` 吞退出码** —— `subprocess.call(...)` 的返回值没人接，**编译失败也返回 0**（同文件的 `config` 配方一直是 `sys.exit(result.returncode)`，只有 `build` 漏了） | 透传退出码。**这条比上面那条更重要**：不修它，任何"让构建失败"的护栏都退化成"打印一行没人必须读的输出"——正是 P14 的失败模式在工具层重演 |

**护栏入选判据**（加旗前逐条实测，三条都要满足）：① 诊断的是**错误**而非风格/可移植性偏好；
② **当前 vendor 树零命中**（否则本改动自己就把构建点着）；③ 旗名在本编译器**存在**——不存在的旗
每次编译刷一条 `-Wunknown-warning-option`，"每次刷、没人读"又是 P14 的形态。
实测 `maybe-uninitialized` / `nonnull-compare` 是 **GCC 专有**，已按编译器 gate。

> ⚠ **探针坑（写这条时踩过）**：clang 对未知 `-Werror=X` 只**警告**不失败，
> **靠退出码探测旗名会全部漏判**。必须读 stderr。

**变异验证（双向，判据有效性）**：

| 构型 | 结果 |
|---|---|
| 现状（`fabsf` 在位） | `just build` rc=**0**，vendor 重编零 `error:`、零 `-Wunknown-warning-option` |
| 变异：`fabsf(value)` → `abs(value)` | rc=**1**，`ffx_fsr3upscaler.cpp:166:12: error: using integer absolute value function 'abs' when argument is of floating point type [-Werror,-Wabsolute-value]` |
| 恢复 | rc=**0** |

**为什么没有配套的 `just test` 单测**：`lanczos2()` 是 vendor `.cpp` 里的 `static` 函数，测试够不着。
唯一写得出来的单测是"在测试里重算一遍闭式去比对期望"——那测的是我的副本，**vendor 真退化了它照样绿**，
违反阳性锚点（"这段代码从未生效，断言会绿吗"→ 会绿）。所以这一类的正确归宿是 **GUARD（编译器）**，
不是 TEST。要真做运行时断言，得先扩大 vendor patch 把 LUT 暴露出来——不值得，见上表编译器已经拦住了。

**沉淀地址**：`GUARD: -Werror=absolute-value on ffx_fsr3upscaler_vk`（+ `just build` 退出码透传）。
同类面已扫：vendor host 代码里非限定 `abs(` 现 **0 命中**。

## 已知上游问题（VVL 实测，非本仓引入）

`just fsr-smoke` 在 **AMD Radeon 8060S Graphics (RADV STRIX_HALO) / Mesa 26.2.3 + VVL** 上
现存 **13 条 / 3 类**，全部归因上游；冒烟程序对它们建**白名单 + 计数**（`smoke/fsr_smoke.cpp`），
**白名单外的新报错直接 FAIL**，上游升级时靠这个发现回归。

| 报错 | 次数 | 上游根因 |
|---|---|---|
| `VUID-vkAllocateMemory-deviceCoherentMemory-02790` | 10 | 后端选到带 `VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD` 的 memory type，却没启用 `deviceCoherentMemory` 特性。RADV 上该位与 `DEVICE_LOCAL` 同型，选类型时没排除 |
| `VUID-VkImageSubresourceRange-layerCount-01721` | 2 | `ffx_vk.cpp:4223` 的 `vkCmdClearColorImage` 传了 `layerCount = 0` 的 subresource range |
| `VUID-vkDestroyDevice-device-05137` | 1 | `ffxFsr3UpscalerDeinitialize` 之后仍有 18 个对象未销毁（后端内部 `VkDeviceMemory` / `VkImage` / `VkCommandBuffer` 泄漏） |

### 已修的一条（记在这里防止重踩）

`storage image ... doesn't match the VkImageView`（×2）—— luma history 的 `rgba8` vs
`R16G16B16A16F`，**已由 P11 修复并从冒烟白名单里删除**。删白名单条目是刻意的：留着就等于
「修没修都绿」。若上游升级把它带回来，会以「白名单外」的形式把冒烟打红。

**防线不止冒烟**：`gen_ffx_shader_blobs.py::validate_image_formats()` 在 `just fsr-check`
与每次生成时全量对账「GLSL typed image 的格式限定符 ↔ host 侧资源格式」，不匹配即 rc=1。
1.1.4 实测：22 个 typed image 里 9 个两边都查得到，8 个本来就对，**唯一那条就是 luma history**
⟹ 上游的孤立笔误，不是普遍形态。剩下 13 个格式由 app 提供，无从比对（护栏对此明确跳过，
但**可比对数为 0 会当失败**——防止解析漂了以后靠空集合假绿）。

## 与 phase14_plan 的对应

- §1.4「`-reflection` 输出对 VK 路径无用」**是错的**：`ffx_vk.cpp:3330+` 用
  `FfxShaderBlob::boundSRVTextures/Counts/Spaces` 逐条搭 `VkPipelineLayout`，反射表必需。
  生成器因此自带 SPIR-V 反射（不引 spirv-reflect）。
- §1.4「≈40 个 SPIR-V 产物」**低估了**：排列是 6 个二值选项的笛卡尔积 = 64/pass，
  10 pass × 4 variant = 2560 次编译；靠内容 MD5 去重后塌到 **80 个唯一模块 / 1.7 MiB**。
- §5 风险 2（1.1.4 VK 后端在 RADV 上跑不起来）**已排除**：`just fsr-smoke` 两帧
  `ffxFsr3UpscalerContextDispatch` 全部 dispatched + executed，VVL 无白名单外报错。
