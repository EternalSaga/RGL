# Phase 14 · 时域线换轨：vendor FSR 3.1.4（VK 后端考古）+ 逐像素速度场

> **状态**：现行（2026-10-07 立项）。承接 `docs/phase13_plan.md`（TAA 收口，**本 phase 开工即冻结**）。
> **一句话**：Phase 13 证明「门唯一的输入是相机派生量」是天花板（§1.22 三次失明），本 phase
> 不再修那扇门，而是**换输入面**：先做逐像素速度场（债-71），再把 AMD FSR 3.1.4 的 Vulkan
> 后端从 1.1.4 里捞出来接上。
> **本 phase 的存在理由不是"FSR 更好"，是三条**：① 自研 TAA 的可行域已被实测证明为空（阈值
> 上下都被封死，见 §0.2）；② FSR 3.1 的五个旋钮恰好覆盖我们自研的三条线（记忆 / 遗忘 / 细
> 特征，见 §1.6）；③ 速度场是**任何**时域方案的共同前置，做了不白干（债-35/71）。

---

## 0. 冻结裁决与立论

### 0.1 TAA 冻结（2026-10-07 用户拍板）

- **冻结的是"继续调自研 TAA"**，不是"删掉 TAA"。`rgl/shaders/taa.frag.slang` 与全部 imgqa
  时域判据**保留**——它们是下一根线的尺子（§4 迁移表）。
- 台账终态 = **降级**（不是否决）：触发条件 = 逐像素速度场落地（= 本 phase 批 14.4）。
- **不许做的三件事**：① 删 `clip_mode` 消融臂集（债-68 已裁：该删的是"默认"不是"选择器"，
  臂 1 是阳性锚点不是候选）；② 删仪器与判据；③ 把"运动中 R>1"记成"TAA 没用"——准确表述是
  **「运动中 TAA 应当闭嘴」**，而臂 13 恰好就是让它在运动中闭嘴的东西。措辞决定下一个人会不会
  去把整条线删掉。

### 0.2 为什么可行域为空（本 phase 的立论，比"三次失明"更硬）

`gate = saturate(2 − motion)`（`taa.frag.slang:833,882`）：

- 抖动自身制造的伪 `motion` 上界**恰 1px**，而臂 13 静止侧"逐位不变"完全依赖 `motion ≤ 1px`
  时 `saturate` **精确返回 1.0**；
- 真实慢速运动住在 1–2px 的斜坡上（`:820` 自陈"每帧 1.5px 落在斜坡上却累积 150px"）；
- ⟹ 断点**往上挪** = 慢运动漏过去（债-61 缺陷②）；**往下挪** = 静止抖动期就冲掉历史 = 退化成
  "关掉 TAA"（判据 ② 专门防的退化解）。

**断点上下都被封死 ⟹ 阈值这一整族修法被一次性排除。** 这不是"还没调到"。

---

## 1. 事实基线（2026-10-07 本机核实；出处 = `~/program/FidelityFX-SDK` @ `60f4ea8` 及其 git 历史）

> 这一节的作用是**防止下次重新查一遍**。每条都核到文件/行号或 git 提交。

### 1.1 "SDK 支持 Vulkan" 是过期话，但代码层面 Vulkan 没死

- 当前 2.3.0 readme **自相矛盾**：第一行 "DirectX® 12 or Vulkan® applications"，末尾 Known
  Issues 写 **"Vulkan is currently not supported in AMD FSR SDK 2.3"**。
- Vulkan 后端是**被删的，不是没做过**。文件名含 `vk` 的文件数：1.1.4 = **245** → 2.0.0（`f4c1da8`）
  = 3 → 2.2.0（`e236f23`）= 1 → 2.3.0 = 0（只剩 3rdparty 的 `vk_mem_alloc.h`）。被删内容包括
  `sdk/src/backends/vk/ffx_vk.cpp`（**4692 行**后端本体）、`ffx_api_vk.h`、整个 Cauldron
  `render/vk/`、预编译 `PrebuiltSignedDLL/amd_fidelityfx_vk.dll`。
- **官方给的路线就是 1.1.4**（blog 原话："our existing FidelityFX SDK features will continue to
  be available as v1.1.4"）。所以 whats-new 里 `vulkan` 零命中不是遗漏，是"1.x 继续服务"。

### 1.2 为什么砍 Vulkan：分发模型，不是 DX 高级 API

- 2.3 的 DX12 后端只碰 11 个接口（`ID3D12Device/CommandList/CommandQueue/GraphicsCommandList/
  CommandSignature/DescriptorHeap/Heap/PipelineState/QueryHeap/Resource/RootSignature`）——
  **无 DXR、无 Work Graphs、无 Mesh Shader、无 State Object**。
- 真因（blog 原话）：**"Use of pre-built, signed AMD FidelityFX API DLLs is now required."** +
  "driver releases can update the version of ML-based technologies used in-game by default"
  ⟹ 要的是「签名 + 驱动可热替换」，那是 Windows PE + Authenticode 的机制，Vulkan 侧无对应物。
- 推论：**FSR3.1.5 在 SDK 2.x 里的身份是 FSR4 的兼容垫背**，不是产品。我们要的 FSR3 权威载体
  = **1.1.4（Upscaler 3.1.4 / FSR2 2.3.3）**；3.1.4 → 3.1.5 的全部算法差异 = changelog 一句
  "Fix for possible negative rcas output"。

### 1.3 VK 后端形态（决定集成难度，四条全是好消息）

| 事实 | 出处 | 对我们的意义 |
|---|---|---|
| **compute-only**（全程 `VK_PIPELINE_BIND_POINT_COMPUTE`） | `ffx_vk.cpp:3944` | **不要 `VkRenderPass` 句柄**，录制点由我们控 |
| wave64 是**管线创建期**挂 `RequiredSubgroupSizeCreateInfoEXT(64)`，且条件式 `waveLaneCountMin≤64≤Max` | `ffx_vk.cpp:3624-3633` | ⚠ **14.1 实测修正：这条在 FSR3-upscaler 上是死路**。挂 wave64 的前提是 `ffxIsWave64(effect, permutationOptions, …)` 为真，而 `ffx_fsr3upscaler.cpp` **从不给 `permutationOptions` 赋值**（`FfxPipelineDescription = {}` 后零赋值，全文件 `permutationOptions` 零命中）⟹ **VK 后端上 wave64 分支永不进**。NVIDIA 的 32..32 因此**不构成风险**，但反过来：wave64 那半套排列**运行时永远用不到**（生成它们只是为了填 accessor 的索引表） |
| shader 是**真 GLSL 450** 单源双语（`FFX_GLSL` 宏切 `_callbacks_glsl.h` / `_hlsl.h`） | `backends/vk/shaders/fsr3upscaler/*.glsl` | 3.1.5 那行 rcas 修复搬到 GLSL 侧 = 小 diff，不是重写 |
| 编译链 = **glslang**：`-compiler=glslang -e CS --target-env vulkan1.2 -S comp -Os -DFFX_GLSL=1` | `CMakeShadersFSR3Upscaler.txt` | Linux 现成；且 GLSL 分支下 wave 参数**为空**，矩阵只有 `-DFFX_HALF=0/1` |

### 1.4 构建期唯一的墙：`ffx_sc`（以及为什么绕开它比移植它便宜）

- `sdk/tools/ffx_shader_compiler/` 是构建期排列编译器，CMakeLists **MSVC-only**（`if(MSVC_TOOLSET_VERSION
  VERSION_LESS 142) → FATAL_ERROR`），还 `add_subdirectory(libs/dxc)` + `libs/glslangValidator`（Windows .lib）。
- 但它的产物格式**蠢到可复现**：`static const uint32_t g_<name>_size` + `static const unsigned char
  g_<name>_data[]`，外加 `g_<shader>_PermutationInfo[]` / `g_<shader>_IndirectionTable[]` 两张表。
  **MD5 只是它内部去重键，不必复现。**
- ~~**pipeline layout 不是生成的** ⟹ `-reflection` 输出对 VK 路径无用，少一件事。~~
  ❌ **14.1 实测推翻**：`ffx_vk.cpp:3330+` 逐条读 `FfxShaderBlob::boundSRVTextures` /
  `boundUAVTextures`（含 Counts / Spaces）来搭 `VkPipelineLayout` ⟹ **反射表是必需的**，
  少这一件事变成"必须自己反射"。手写的那份 layout 只是 effect 侧的 binding **号**表，
  不替代反射。且 binding **名字**也是功能正确性的一部分：`ffx_fsr3upscaler.cpp:322-344`
  用 `wcscmp` 把名字对进 `srvTextureBindingTable` / `uavTextureBindingTable` 填
  `resourceIdentifier`，对不上直接 `FFX_ERROR_INVALID_ARGUMENT`。
  ⟹ 生成器自带 SPIR-V 反射（不引 spirv-reflect），并把「反射出的名字 ⊆ 绑定表」做成
  **阳性锚点**（1.1.4 实测 691 条全命中；空名反向用例确认会被拦下）。
  ⚠ glslang 的 `-Os` **保留** `OpName`（rcas 里 8 条变量名）——反射不依赖 `-g`。
- 签入的 `ffx_fsr3upscaler_shaderblobs.cpp` 按名 include `<ffx_<pass>_pass_permutations.h>` 与
  `<ffx_<pass>_pass_wave64_permutations.h>`，共 **10 个 pass**：`accumulate` / `autogen_reactive` /
  `debug_view` / `luma_instability` / `luma_pyramid` / `prepare_inputs` / `prepare_reactivity` /
  `rcas` / `shading_change` / `shading_change_pyramid`。
  ~~≈ **40 个 SPIR-V 产物**。~~ ❌ **低估了**：排列是 **6 个二值选项的笛卡尔积 = 64/pass**，
  10 pass × 4 variant（`FFX_HALF` × wave32/64）= **2560 次 glslang 调用**（-j16 约 27 s）。
  靠内容 MD5 去重后才塌到 **80 个唯一 SPIR-V 模块 / 1.7 MiB**（产物头 9.1 MiB）。
  去重比预想更狠的两个事实：`rcas` 对 `FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING` **不敏感**
  （64 个排列全同，锐化是 host 侧门）；GLSL 分支下 wave32/wave64 编译参数**完全相同**
  （`CMakeCompileShaders.txt` 的 `HLSL_WAVE*_ARGS` 只在 .hlsl 分支赋值）⟹ 跨 variant 共享字节。
- 处置 = **生成一次、产物签进仓库、构建开头 SHA1 护栏比对**——与 `generated/*.cppm` +
  `reflection_check` 完全同构。工具语言纪律：C++ + vcpkg，或 stdlib-only Python。

### 1.5 它不用 VMA、自建 descriptor pool（以及为什么先别改）

- 全文件 `vkAllocateMemory(` **只有 4 处**（`:932` 在漏斗函数 `allocateDeviceMemory()` 内、
  `:1301` / `:1591` uniform buffer 池、`:4256`），且它自建 `VkDescriptorPool`（含 bindless pool）。
- 这是**库的立场**，不是偷懒：接 VMA = 依赖我们的 VMA 版本 + 被我们的分块策略绑住 + 崩溃归因模糊。
  跟它自建描述符池是同一个决定的两面。
- **真正的代价是账本口径**：我们显存账的招牌性质是「逐内存类型求和与 heap **完全闭合**」
  （3300.88 / 2926.15 MiB / 461 allocations）。FFX 一加进来，**"闭合"这个断言当场失效**——
  不是少个数字，是那本账不再证明任何东西（债-34 同形态）。
- 先记账、别先动刀：用它自带的 `GetEffectGpuMemoryUsageVK` 补一行科目。改那 4 行的触发条件见 §2 批 14.8。
- ⚠ 我们**自己就干过反方向的事**：VMA + zero-init 冲突那次是"真实需求走原生 `vkAllocateMemory`
  绕 VMA"（`vulkan_extensions_2025_调研.md` §12.3/§13.1）。同一个权衡，别双标。

### 1.6 输入契约（`ffx_fsr3upscaler.h` @ 1.1.4，权威字段表）

**每帧必需**：`color`（render res）、`depth`（32bit，render res）、`motionVectors`（2D，render res）、
`output`（upscale res）。**可选**：`exposure`（1×1）、`reactive`、`transparencyAndComposition`。
**FSR 自己产出、我们分配并可共享**：`dilatedDepth`、`dilatedMotionVectors`、
`reconstructedPrevNearestDepth` ⟹ **深度膨胀是内部的，不用我们写膨胀 pass**。
**每帧参数**：`jitterOffset`、`motionVectorScale`、`renderSize`、`upscaleSize`、**`reset`**
（"camera moved discontinuously" ⟹ 对应我们已有的 `cameraJumps` 概念）。
**init flags**：`ENABLE_HIGH_DYNAMIC_RANGE` / `_DISPLAY_RESOLUTION_MOTION_VECTORS` /
`_MOTION_VECTORS_JITTER_CANCELLATION` / `_DEPTH_INVERTED` / `_DEPTH_INFINITE` / `_AUTO_EXPOSURE` /
`_DYNAMIC_RESOLUTION` / `_DEBUG_CHECKING`。
**reactive 可以自动生**：`FfxFsr3UpscalerGenerateReactiveDescription`（`colorOpaqueOnly` vs
`colorPreUpscale` → `outReactive`）⟹ **债-36 那句"reactive 要材质标记，我们一个都没有"被这条
推翻**，代价是 render-res 多画一遍 opaque-only。
**quality mode 含 `NATIVEAA`（1.0x）** ⟹ 可以只在原生分辨率做时域 AA，不必真降分辨率。

**五个旋钮（`FfxFsr3UpscalerConfigureKey`）——这就是"不如去 FSR 里调旋钮"的具体内容**：

| key | 语义（doc 原文摘要） | 对应我们哪条债 |
|---|---|---|
| `FVELOCITYFACTOR`（默认 1.0，→0.0） | **改善亮像素的时域稳定性** | **债-40**（本帧 HDR firefly / 孤立白点） |
| `FACCUMULATIONADDEDPERFRAME`（默认 0.333） | 去遮挡处 / reactive>0 处**每帧累积增量** | 债-69 的"遗忘"轴 |
| `FMINDISOCCLUSIONACCUMULATION`（默认 −0.333） | 去遮挡像素**延迟到 N+2 帧**才开始累积 | 债-69/71（"刚动过、现在静止"那个状态） |
| `FSHADINGCHANGESCALE`（默认 1.0） | 放大 FSR3.1 **自己算的 shading change** | 我们臂族想造的"时域可信度"，它内置了 |
| `FREACTIVENESSSCALE`（默认 1.0） | 写进 reactive 的增益 | 债-62 的响应函数族 |

---

### 1.7 渲染图的 barrier 覆盖面现状（决定 FSR 落位的风险面）

**自动推导只覆盖 image attachment 的布局链，图外访问靠"进出同值"的约定——该约定已由批 14.4b 的 A 档护栏变成断言**：

| 事实 | 出处 |
|---|---|
| 图自动推导 = attachment 布局链（`usage → {layout, stage, access}` 查表） | `render_graph.cppm` 头注释「usage → {layout, stage, access} 推导规则」段 |
| **明确不进图**：descriptor/sampler heap host-write barrier、scene data staging copy barrier、acquire/present、fence | `render_graph.cppm` 头注释「图的边界」段 |
| `RgAttachmentKind` = swapchain / depth / offscreen / depthOffscreen —— **无 buffer kind** ⟹ **buffer 侧同步完全在图外** | `RgAttachmentKind`（`render_graph.cppm`） |
| 自发 `CmdPipelineBarrier2` 的模块（执行器 `vkrenderer_passrec` / `vkrenderer_offgraph` 除外）实测 **12 个**：`gpu_cull` / `IzbShadow` / `EvssmShadow` / `env_prefilter` / `mip_gen` / `texture_upload`(+cubemap) / `gpu_canary` / `fsr_upscaler` / `frame_capture` / `picking` / `vkresources`。其中做**逐帧 image 布局 sandwich** 的只有 `IzbShadow` 与 `fsr_upscaler` 两处，其余的 barrier 全是 buffer / 全局 barrier（§1.11 修正一） | `izb_shadow.cppm`（`IzbShadow` 类头注释「render_graph 无 compute kind」）、`evssm_shadow.cppm`（`EvssmShadow` 类头注释同构） |
| IzbShadow 逐帧 barrier（`izb_shadow` 开且 shadow0 段非空）= **8 条 buffer + 2 条深度图布局**：`recordFrame` 3+2、`beginMaskPass` 4、`endMaskPass` 1；dump 帧额外走 `recordDumpCopy` **+4 条 buffer**（旧数「~10」= 把全文件 12 个 `bufferBarrier` 调用点当成了每帧数） | `IzbShadow::recordFrame` / `beginMaskPass` / `endMaskPass` / `recordDumpCopy`（`izb_shadow_impl.cppm`） |
| 深度布局的处理方式 = **进出同值**，注释自陈「**渲染图布局追踪表不可见**，零漂移」 | `izb_shadow.cppm`（`IzbShadow::recordFrame` 文档注释）、`IzbShadow::recordFrame` ⑥ 还原 barrier 注释；`frame_capture_impl.cppm`「追踪表原值进出」注释同构 |
| 追踪表 = `Renderer::attachmentLayouts`，图外路径**读出并还原**，且自批 14.4b A 档护栏起**有断言校验还原真的发生了**（不符即 error，机制与变异验证 → §1.11）。⚠ 两处护栏**防的不是同一件事**（批 14.4b 收账修正）：IZB 那条对账「实际发出的 barrier 推进出的布局 ↔ 表」；FSR 那两条查「色输入 / 深度进 vendor 前是否已在申报布局」。FSR 原先那条 velocity 护栏是**恒假断言**（读的是上一行刚写进表的值），已删，velocity 改由 `Renderer::transitionAndTrack` 的**结构**保证 → §1.21 | `Renderer::attachmentLayouts` / `Renderer::transitionAndTrack`（`vkrenderer.cppm`）；`Renderer::izbLightPassPre` / `Renderer::fsrTonemapPassPre`（`vkrenderer_offgraph.cppm`）；判定与消息 = `layoutDrift`（`utils.cppm`） |

⟹ **缺口不是"漏了几条 barrier"，是"约定无断言"**（断言已由 14.4b 补上）：布局若真漂移，VVL **能**报
（§1.11 实测：删 IZB 还原 barrier 的变异里 VVL 确实响了），但报在**执行器下一次 barrier 那个无辜
调用点**、根因与症状隔一整帧，且只在调试层在场时存在。债-32（mask 永不清零
却逐位相同）就是同一形态：**约定成立时一切正常，所以没人知道它会不会不成立。**

**FSR（批 14.5 已接入）是第三个图外 compute**，它比 IzbShadow 更麻烦：它的产物里有三张**由我们
分配、由它写入**的图（`dilatedDepth` / `dilatedMotionVectors` / `reconstructedPrevNearestDepth`），
还有一张我们每帧写的 velocity buffer ⟹ 手写 barrier 的面积确实再扩大——护栏**先立后扩面**已照此执行（A 档，见 §1.11）。

> **修正记录**：本节初稿写于 2026-10-07；「布局漂移 VVL 不报」「图外 compute 家族 7 个」「IzbShadow
> 每帧 ~10 条 buffer barrier」已被 §1.11 与批 14.4b / 步 0 实测修正，上表即修正后的现状（被推翻的
> 旧结论直接删，不留双层）。

### 1.8 批 14.1 落地事实（2026-10-08 实测；`third_party/fsr/`）

**结论：风险 2 排除** —— 1.1.4 的 VK 后端在 RADV 上跑得起来。

| 项 | 实测 |
|---|---|
| vendor 树 | `third_party/fsr/vendor/` = `c6efa6b`（tag v1.1.4）的 **include 闭包裁剪版**，95 文件 / 1.8 MiB（原 `sdk/` 13 MiB）。闭包由传递性解析 `#include` 算出 |
| 排列产物 | `tools/gen_ffx_shader_blobs.py`（stdlib-only）→ `blobs/` 41 个头，**80 个唯一 SPIR-V 模块 / 1.7 MiB**。`just fsr-blobs` 重生成，`just fsr-check` 比对 38 输入 / 41 产物 SHA1 |
| 静态库 | `ffx_fsr3upscaler_vk`，Linux/clang 22 + libc++ 干净通过。**全量编译 1.3 s**（-j16），故留在 `just build` 里当"vendor 还编得过"的闸门，成本可忽略 |
| 冒烟 | `just fsr-smoke` = 最小 device + 2 帧 `ffxFsr3UpscalerContextDispatch`，**AMD Radeon 8060S Graphics (RADV STRIX_HALO) / Mesa 26.2.3**，dispatched + executed，VVL **白名单外 0 条** |
| vendor 改动 | **P1–P10**，逐条根因 → `third_party/fsr/PATCHES.md`。P1–P6 编译期（MSVC vs clang/libc++）、P7 链接期、**P8–P10 运行期**，其中 P9/P10 **只在 `-O2` 暴露** |
| 上游 VVL 问题 | 现存 **3 类 13 条**（coherent memory type / `layerCount=0` / deinitialize 泄漏 18 对象）。冒烟建白名单 + 计数，**白名单外即 FAIL**（变异验过非空转）。第 4 类「luma history 格式不匹配」**已由 P11 修复并从白名单删除** |

三条要带给后续批次的事实：

1. **冒烟必须跑交付优化档**。同一份代码 `-O0` PASS、`-O2` 段错误，两次，都是对齐 UB（P9/P10）。
   「`-O0` 冒烟过了」在这类 bug 上**不提供任何信息** → `git:调试历史` 2026-10-08。
2. ~~**luma history 的格式不一致是接真实画面前必须还的债**~~ ✅ **当晚已还（P11 / 债-73 ③ 结案）**：
   写侧 `layout(..., rgba8)`、host 侧建 `R16G16B16A16_FLOAT`，VVL 判"整个 image 读写值未定义"。
   处置 = 改**限定符**对齐资源（权威口径 = HLSL 侧 `RWTexture2D<FfxFloat32x4>`；反过来降资源精度是
   错的——存的是 4 槽 luma 历史，HDR 下可远大于 1，UNORM 会截顶还把精度压到 1/255）。
   **防线做成了对账护栏**：`just fsr-check` 全量比对 22 个 typed image 里两边都查得到的 9 个
   （1.1.4 实测 8 个本来就对 ⟹ 上游孤立笔误，不是普遍形态）。
3. **冒烟把 image 一律推到 `VK_IMAGE_LAYOUT_GENERAL`**，是在**回避**而不是预演布局簿记。
   真实接线的布局对账仍归 **14.4b 的护栏**，14.5 的 VVL 判据不能替它背锅。
4. **排列矩阵的绝大部分运行时到不了**：`ffx_fsr3upscaler.cpp` 从不给 `permutationOptions`
   赋值（§1.3 修正）⟹ 每个 pass 实际只用 base 排列，wave64 那半套**永不索引**。我们仍然必须
   生成它们（accessor 的 `IndirectionTable` 按 index 查，缺项 = 空 blob），但**这是个可以
   大幅缩小的生成面**（2560 次 glslang → 理论上 10 次 + 表占位）。要不要缩 = 14.6 之后的
   构建期账，不在本 phase 的关键路径上。

**14.1 未达成的一条**：验收 ① 要求「Linux clang + Windows MSVC 双平台 configure 通过」——
本机只有 Linux，**Windows 侧未验**。P1–P6 全是"非-Windows 缺 include"方向的补丁，
理论上不动 Windows 路径（改动都在 `#ifndef _WIN32` 或新增 TU），但**这是推理不是实测**。

---

### 1.9 批 14.3 落地事实（2026-10-08 实测；`render_scale`）

**放大是免费的，图形状与 renderScale 无关。** 这是本批最重要的事实，它把"加内部分辨率"
从一次渲染图改造降级成一次口径改造：

- tonemap pass = 全屏大三角 + `sceneHdrTex.Sample(hdrSampler, input.uv)`，uv 归一化、
  sampler 是 linear ⟹ **scene_hdr 落在渲染分辨率、tonemap 仍在后缓冲分辨率画，天然得到
  双线性放大**。不需要新 pass、不需要新 RT、不需要 blit。
- 推论：默认档逐位不变是**结构保证**（renderExtent == swapchainExtent 时所有分支同值），
  不是"实测碰巧对上了"。
- 批 14.5 的 FSR 在**同一位置**替换这个隐式放大（届时才需要图外编舞 + 真 pass）。

**口径分叉表**（谁吃哪个 extent；漏一处 = 该资源停在窗口网格上）：

| 吃**渲染** extent | 吃**后缓冲** extent |
|---|---|
| 深度图（构造 + 两处 resize 重建） | 写 swapchain 附件的 pass（`renderRectFor` 第 ③ 档） |
| 四个启动期 RT slot（G-buffer / scene_hdr / ao / taa） | imgui 叠加层（坐标系本就钉成后缓冲像素） |
| IzbShadow、drawFrame 的 offscreen ensure | 捕获到后缓冲（frame_capture） |
| `projectionSystem` 宽高比、`taaSystem` 抖动与历史判据 | 点选的**输入**是窗口像素，但出口换算成渲染像素 |

**三个不显然的坑**（都已在代码注释就地登记）：

1. `Swapchain` 的 mem-initializer 里**形参遮蔽同名成员** —— 若成员叫 `renderScale`，
   后面 `computeRenderSize(extent, renderScale)` 读到的是**未钳制的形参**，钳制静默失效。
   参数因此命名 `renderScaleIn`。
2. `RenderExtent` 的每帧同步必须挂在 `resizeSystem` 的**"尺寸没变就早退"之前** ——
   acquire/present 的 OUT_OF_DATE 路径会重建 swapchain 改渲染 extent，却**不改**
   `WindowExtent`，挂在早退之后那条路径下会永久停在旧值。
3. blit 两端原先共用同一个 extent（注释写着"1:1 同尺寸同格式"）—— renderScale≠1 时
   会把放大**静默截成左上角拷贝**。现改为各取自自己那个 slot 的实际 extent。

**点选口径收口**：`pendingX/Y` 交进 `PickingService` 时已是渲染像素
（逻辑点 →×`WindowScale`→ 窗口像素 →×`RenderExtent/WindowExtent`→ 渲染像素）。
第 ② 级用 **extent 比值**而不是直接乘 renderScale：取整后（1001×0.75=751）比例仍精确，
不会与 RT 尺寸差半个像素。`picking_impl.cppm` 里 2026-10-03 登记的口径风险就此结案。
债-37 的三条连带（imgui 钉像素 / 字号 ×scale / 鼠标点选 ×scale）**无回退**：
renderScale=1 时比值恰为 1，行为逐位同前。

**纯算术住 rlbase**：`computeRenderSize` / `clampRenderScale` 落 `runtime_config`
（它本就持有区间常量 `kRenderScaleMin/Max`）⟹ `just test` 覆盖得到、配置侧与构造侧
共用同一函数。放 rlgfx 就得把 SDL 链进测试；返回 `vk::Extent2D` 会把 rlvk 拖进
rlbase（分层红线），故用 `RenderSize` 值类型，转换只在 `Swapchain::toExtent` 一处。

**顺带被文件长度红线逼出来的两个分区**（搬移均逐字核对为原样，只差 `export` 关键字）：
`vk_renderer:offgraph` = 图外 compute 编舞（IZB / EVSSM；**批 14.5 的 FSR 接线代码的家**，
图内/图外两类布局的对账方式不同这件事正是批 14.4b 勘察出的根因）·
`ecs:frame_diag` = 帧循环诊断旁路（截图时序 / 帧时间统计）。

**验收缺口（诚实记录）**：④ Wayland 175% 实测未做（本机无 Wayland 会话）；live resize
未手工验证 —— 改为把取整/钳制逻辑写成 `tests/test_render_scale.cppm` 的断言覆盖，
比点一次窗口更能防回归。

### 1.10 批 14.4 落点勘察（2026-10-08 夜实测；**含对 plan 原文两处描述的修正**）

#### 修正一：落点不是 compute pass，是**全屏 `fullscreenPass`**

plan 原文写「compute pass 由两帧相机矩阵 + depthTex 解析算出每像素速度」。勘察后改判：

- `tonemap` / `ao` / `taa` 三个 pass **全是**「全屏 draw 读纹理 → 写 RGBA16F offscreen RT」
  这个形状（`render_graph_builtin.cppm` 的速构 + `pass_vocabulary` 的 `fullscreenPass` 档）。
  速度场 pass 与它们同形。
- 走它的收益（每条都是 compute 路线要付的代价）：**不需要**给 `RgPassKind` 新增 compute
  kind（现只有 `drawMesh/blit/drawLight`）；**不依赖批 14.4b** 的图外布局护栏（图内 pass 的
  布局由编译图负责，单一真相源已存在）；**不涉及独立计算队列**（⚠ 本句旧版写「不碰闲置的
  `computeQueue`（债-24）」是误导——那条队列**从来不存在**（三角色恒坍缩成同一条），成员已删，
  判据 → `docs/compute_调研.md` §3）；
  加一个状态 = `pass_vocabulary::kAllPassStates` 加一行 + StateKit 加成员（漏实现编译不过）。
- 唯一损失是"用 compute 更省"这一条未量化的假设——本 pass 是每像素几条矩阵乘法，
  与 taa pass 同量级，而 taa 就在 fragment 里跑得好好的。

#### 修正二：TAA 的重投影**已经是逐像素**的，"全局矩阵"只是自旋补偿

plan 原文写「同时把 TAA 的重投影从『全局矩阵』改成『读这张场』」。实测：

```
taa.frag.slang:530-540   depth = depthTex.Load(像素)
                         world   = worldFromDepth(baseUv, depth)      ← 逐像素反投影
                         worldPrev = mulM(readTaaMotion(), worldPrev) ← 只有这条是全局的
                         prevClip  = mulM(readTaaPrevViewProj(), worldPrev)
```

**重投影本身就是逐像素的**（深度 + 两帧矩阵）。"全局"的只有 `readTaaMotion()` =
`prevSpin·inverse(curSpin)` 一条 mat4，而它对**当前场景**（只有全局 spin、`animationSystem`
不动别的）是**精确补偿而非近似**——`ecs_impl.cppm:427` 的注释自己就这么写。

⟹ **对债-71 判据 ④ 的后果（必须现在写死，否则 14.5 之后会被误读成回归）**：
「臂 14 = 门改吃真实逐像素速度 ⟹ 直接问跷跷板是否松绑」这个前提**在当前场景下不成立**——
逐像素速度场携带的信息量**并不比现有的 (depth, prevViewProj, spin) 三元组多**，因为场就是
由它们算出来的。门改吃它 = 换一个输入表示，不增加信息。
**预期：臂 14 不翻案**（运动中 R 仍 ≥ 1）。真正能松绑的是**引入 per-entity 运动**
（债-35 的另一半），那时场才带来新信息。
所以 14.4 的**不可省的理由是 ①②，不是 ④**：① FSR 硬要求一张 `motionVectors` 纹理
（它不接受"你在 shader 内部自己算"）；② per-entity 动画的硬前置。
判据 ④ 照跑，但**结论预期为否**，且那是**信息论事实**不是实现缺陷。

#### 本批已落地：纯函数层（`rgl/velocity_field.cppm`，rlbase）+ 9 用例

- 口径四条（framebuffer uv / Vulkan 深度 / 像素·prev−cur·Y向下 / 两帧矩阵同口径成对）
  逐条有用例；FSR 侧口径出处 = `docs/FSR输入契约.md`。
- 期望值出处 = **另一套显式实现**（手工构造两帧相机基 + 闭式透视，不调 `glm::lookAt` /
  `perspective`、不求逆，方法学同 `temporal_yaw_remap` 金标准）**加上**一条完全独立的
  旋转光流一阶闭式。**后者当场抓出我自己推错的三处符号**——详见测试文件头两个坑。
- 7 组变异全部实测验红（含"两道非有限防护互为冗余 ⟹ 单点变异检不出"这条如实记录）。
- 交付路径逐位不变（新模块**尚无调用方**）。

#### 本批第二批已落地：pass 侧（门 `runtime.lua velocity_field`，默认关）

**零新契约**——速度场要的四个输入**全都已经在 scene data 里**：`skyInvViewProj@64`
（抖动后逆，与深度配套）+ `prevViewProj@722080` + `motion@722144` + `c_t@722208.zw` /
`c_{t−1}@722224.xy`。不新增字段 ⟹ 不动布局、不改 buffer 大小。

- **落点 = 装饰器**（`decorateVelocityPass`，与 taa 同锚点、**先于** taa 调），不是 ao 那种
  「无条件写死在图里 + shader 侧旁路」。差别是红线机制的强度：**门关 = 图里根本没有这个
  pass**（不领 RT、不建条目），结构性逐位不变，而不是"有但旁路"。
  ⚠ 顺带纠正一个容易踩的前提：**运行时生效的图是 `configs/render_graph.lua`，不是 C++
  内建图**（后者只服务 selfCheck / 回落）——但**装饰器路线绕开了改 4 份 lua**（configs +
  qa/configs/{analytic,env_band,nm_rig}），这是它相对 ao 路线的第二重便宜。
- **格式复用 `rgba16_sfloat`**（不新开 rg16_sfloat）：新格式要在 `render_graph_formats`
  词表 + `frame_capture` 的 `captureBpp` + `llm_debug` 通道转换器**三处**同时挂号，
  而漏掉后两处**不崩不报错**（staging 按错字节数算 ⟹ 捕获尾部是上一次残留）。
  FP16 是**相对**精度，速度量级 1–300 px 时 ulp ≈ 0.001–0.15 px，够用。
- **共享重建函数**：`clipToWorld` / `worldFromDepth` 原在 ao.frag 与 taa.frag 各存一份
  逐字相同的拷贝，速度场是第三家 ⟹ 抬进 `scene.slang`（并新增 `uvFromClip` / `uvFromWorld`）。
  抬升是逐字搬移，**但用 `just shot` 逐位对拍证明过**（时机判据与验证方式 → git:调试历史
  2026-10-08 批 14.4 第 2 条）。
- **闭合式**：`vel_base = (prevUv − input.uv)/texel − c_{t−1}`——代入后 `c_t` 自己消掉。
  `c_t` 与 `c_{t−1}` 这一对由 `render_list_impl.cppm:263/:285` 受**同一个** `taa.base_space`
  门控写 0 ⟹ 永远成对，门关时自动退化为自洽的抖动空间速度（不是"半修正"的错误形态）。
- **判据（实测）**：门开 VVL **0 错 0 警**、RT = 渲染网格（`smoke.py --velocity-field`
  双向核对，含"静默没落图"第三档）；门开/门关截图**逐位相同**（无消费者 ⟹ 本应如此）；
  活性 = 把输出附件临时改指 `scene_hdr` 的变异，画面平均绝对差 **98.74**。
  ⚠ **"逐位相同"对新 pass 是盲区不是证据**，推广见git:调试历史同批第 1 条。
- 顺手两处：`decorateTaaPass` 的锚点判据抽成共享 `swapchainColorConsumerIndex`（两份必然
  漂移）；`main_scene.cppm` 里 10 处逐字同构的解包 throw 收成 `orThrow`（超 500 行红线
  2 行是契机，去重本身是正当改动）。

#### 剩余工作（14.4 未完）

① **GPU 侧逐像素对拍通道**——**已挂号债-74**：按附件名回读产物是 PNG，而 RGBA16F 走
`convRgba16Half` 的 **clamp [0,1]**，带符号可达数百像素的速度量经这条链**表达不了**
⟹ 本批判据 ① 只做到 CPU 半边，GPU 半边缺桥；
② TAA 门改吃场（判据 ④，**预期不翻案**，理由见上面"修正二"）；③ imgqa 臂 14 注册。
**红线**：`velocity_field` 关 = 逐位不变（已实测，机制 = 图里没有这个 pass）。

### 1.11 批 14.4b A 档护栏已落地（2026-10-08 晨实测；**含对 plan/债-72 一条理由的修正**）

#### 修正一：逐帧的**图附件**布局 sandwich 全项目只有 **IZB 一处**，不是"IZB + EVSSM 两处"

plan 与债-72 把 EVSSM 与 FSR 并列为图外布局用户。实测：

- `EvssmShadow` 的 `eUndefined → eGeneral` 在**建图期**（`evssm_shadow_impl.cppm:165-166`），
  不是逐帧 sandwich，且不碰 `Renderer::attachmentLayouts`。
- 其余自发 `CmdPipelineBarrier2` 的模块（env_prefilter / mip_gen / texture_upload(+cubemap) /
  gpu_canary）里 **`oldLayout` / `newLayout` 一次都没出现**——图布局转换必须填这两个字段，
  ⟹ 它们的 barrier 全是 buffer / 全局 memory barrier，不做任何 image 布局转换。

⟹ A 档护栏的**现状作用域 = IZB 一处**；14.5 的 FSR 是**第二个**。B 档（挂号声明）的真实
收益面因此比 plan 估的小一个量级：**建议 14.5 之后重新裁决 B 档是否还值得做**（只有两个
用户时，"照抄 IZB 的先例"可能比"给图加一套声明机制"更便宜）。

> **后续（批 14.4b 收账，2026-10-09）**：上面两句都已走到终局——FSR 确实接上了第二个，
> **但接的那一条是恒假断言**（断言读的是上一行刚写进表的值），本批删掉它、velocity 改由
> `Renderer::transitionAndTrack` 的结构保证；B 档已**否决并降级挂触发条件**（理由 →
> `docs/还债总账.md` §3）。→ 成因、永久/临时 sandwich 的分界、syncval 查证全在 **§1.21**。

#### 修正二：「布局漂移 VVL 抓不到」这条**理由不成立**——护栏的价值要换成另外三条

plan 判据 ② 与债-72 都写着"布局漂移 VVL 不报，所以靠护栏"。做阳性锚点时实测：把 IZB 的
还原 barrier 删掉，**VVL 确实报了**——

```
vkCmdPipelineBarrier2(): pImageMemoryBarriers[1].image cannot transition the layout of
aspect=2, level=0, layer=0 from VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
when the previous known layout is VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
```

VVL 自己维护 per-image 的 last-known layout，而图外模块发的 barrier 它**看得见**，
于是执行器下一次拿旧 `oldLayout` 转布局时矛盾就暴露。所以"VVL 抓不到"是**说过头了**。

护栏仍然要留，但理由换成这三条（都不是"VVL 抓不到"）：

1. **归因**：VVL 报在**执行器下一次 barrier 那个无辜调用点**，要人反推"是谁没还原"；
   护栏直接点名肇事模块并给出进入/离开布局（实测输出 `进入 = 3，离开 = 5，追踪表 = 3`）。
2. **不依赖调试层**：VVL 只在 validation 开启时存在，护栏在交付配置里也在。
3. **早一帧**：VVL 要等到下一次有人 barrier 这个 image 才响。

⚠ **这不等于债-72 作废**：VVL 覆盖的是"barrier 的 `oldLayout` 与 VVL 已知布局矛盾"这一支。
漂移之后若**再没人** barrier 这个 image（那就确实无害），或漂移发生在 VVL 不建模的维度
（跨 submission / queue family 所有权），VVL 不响。债-72 仍然开放，但**引用它时别再写
"VVL 抓不到"**，写上面三条。

#### A 档机制（已落地，判据三条全过）

`depthLayoutBarrier` **返回它实际写入的布局** → `IzbShadow::recordFrame` 把两条深度 barrier
串成 `depth = barrier(..., depth, ...)` 链并返回 → `Renderer::izbLightPassPre` 拿返回值与
`attachmentLayouts[depthAttSlot]` 比对，不符即 `logger->error`。

**机关在"返回值由发命令那一处产出"**：若让 `recordFrame` 自己写个局部量记布局，那个量与
barrier 的 `to` 参数就是两个独立事实——删掉还原 barrier 而忘了改局部量，护栏永远为真。
"把约定变断言"最容易做成的就是这种**假护栏**（断言的输入必须来自被断言的那个事实本身）。

- **判据 ①（阳性锚点）**：删掉还原 barrier 的变异 → 护栏响 3 次（每帧一次），
  消息带进入/离开/追踪表三个值。**非空转已验**。
- **判据 ②（反向）**：现状 `configs/shadow.lua` 的 `izb` 出厂开（每帧都过护栏）→ **0 条**漂移告警。
  ⚠ 这条只有在确认 IZB 真在跑时才有意义（先查启动行 `izb: 开门 grid=512`，否则"0 误报"
  是空转出来的）。
  ⚠ **2026-10-10 更新**：`izb` 出厂已改成 **false**（缺陷与裁决 → `shadow_routes_plan.md` §7）
  ⟹ 本判据②在交付配置下**不再被触发**（IZB 不构造 = 没有布局可漂，护栏无事可做）。要复现本
  判据必须显式 `izb = true` 并核对启动行；**别把"交付跑完零告警"读成"护栏过了"**——那是空转。
- **判据 ③（红线）**：护栏只读不写命令 → 截图与基线**逐位相同**。
- **B 档**：本批未做；批 14.4b 收账时**否决并降级挂触发条件**（不是"还没做"）→
  `docs/还债总账.md` §3 + §1.21 六。

### 1.12 批 14.5 前置：FSR 集成面勘察 + 最大未知数已归零（2026-10-08 晨，全部本机实测/复核）

> 本节 = 14.5 动手前的**事实基线**。以下每条要么本机实测、要么主 agent 自己复核过生成物/源码行号
> （勘察报告里有断言被我改过措辞，见"硬失败"那条），别再重查一遍。

#### 最大未知数已归零：真实深度格式怎么喂（第 4 条契约探针）

勘察判出的最大风险是「depth 输入的**深度格式**路径 smoke 一条没跑过」——FSR 给 app 资源建
view 时 format/aspect **全取自我们填的 `FfxResourceDescription`**（`ffx_vk.cpp:2799`/`:2804`），
`getImageAspect` 只认 `FFX_RESOURCE_USAGE_DEPTHTARGET`（`:781-788`）。已补第 4 条探针实测：
**真 `VK_FORMAT_D32_SFLOAT` 图 + 申报 `R32_FLOAT + DEPTHTARGET` ⟹ FSR 读它与此前 R32F 颜色图
结果逐位相同**（同一张分层靶、同一组 4 条期望值）。变异验红两条：摘掉 `DEPTHTARGET` →
**7 条白名单外 VVL 报错**；分层靶上下对调 → 4 条期望里 2 条不符。
**这条变异顺带修正了本文档反复引用的一个前提**：「FSR 零格式校验 ⟹ 填错不报错」只对
**格式串**成立；**aspect 填错 VVL 抓得到** ⟹ 判据 ②「VVL 零报错」在深度输入这里**有效**，
不必另造判据。口径与造靶细节 → `docs/FSR输入契约.md` §2「真实深度格式怎么申报」。

#### 七条会决定接线写法的 vendor 事实（逐条复核过）

1. **FSR 3.1.4 的 upscaler 没有 per-pass 录制 API**——全树无 `ContextRecord`/`RecordDescription`，
   只有 `ffxFsr3UpscalerContextDispatch` **一次调用录完全部 pass**。⟹ 本 phase 说的「图外 compute
   编舞」在 FSR 这一侧的粒度就是**一整块**：我们只能决定它插在命令缓冲的哪个位置、外面包什么
   barrier，**不能**挑它内部 pass 的顺序或插自己的命令进去。
2. **`RenderTargetCache` 建的图不能当 FSR 的输出**：RT 缓存的 usage =
   `ColorAttachment|TransferSrc|TransferDst|Sampled`（`render_target_cache.cppm:141-146`），
   **没有 `STORAGE`**；而 FSR 的 output 落 `uavResources`（`writeonly image2D`）⟹ 必须自己建图。
   ⚠ 别照抄 `ensureVelocitySlot`。
3. **`ffx_fsr3upscaler_vk` target 已存在**（STATIC，`third_party/fsr/CMakeLists.txt:27`，
   `SYSTEM PUBLIC` 带 vendor include），但 `FFX_FSR3UPSCALER` 编译宏是 **PRIVATE**（`:41`）
   ⟹ 链进来拿不到该宏，rgl 侧若依赖它要自己加。
4. 🔴 **dispatch 超 `maxRenderSize` 是硬失败**：`FFX_RETURN_ON_ERROR(... FFX_ERROR_OUT_OF_RANGE)`
   （`ffx_fsr3upscaler.cpp:1253-1258`）。⚠ 勘察报告原话是"只警告"——**不准确**：`:235-238` 那条
   才是警告，dispatch 处是返回错误码。⟹ **必须检查 `ContextDispatch` 的返回码**，否则 resize
   超上限会"返回错误但画面照旧"= 静默不升频。内部共享图按 `maxRenderSize` 在 create 时定尺寸
   （`:823/825/827`）⟹ 改 renderScale / resize 超上限必须重建 context（**时域历史全丢**，
   见 `docs/FSR输入契约.md`）+ 重建前要 `waitIdle`（同 `vkrenderer_offgraph.cppm:58-59` 的规则）。
5. **显存科目有现成读数**：`ffxFsr3UpscalerContextGetGpuMemoryUsage`（`ffx_fsr3upscaler.h:341`）
   ⟹ 14.8 要的「外部分配器 X MiB」一行可以做，源就是它。
   ⚠ **context 构造必须早于 `logFirstFrameMemory()`**（调用点 `vkrenderer_passrec.cppm:446`），
   否则 `first_frame` 统计又漏掉 FSR 全部显存——这是 `git:调试历史:1958-1965` 记过的同一病灶。
6. **shader 排列是确定的「最低配」，渲染器与冒烟必然一致**（14.5 动手前的关键排除项）。
   排列由 `getPipelinePermutationFlags`（`ffx_fsr3upscaler.cpp:426-449`）决定，输入 `useLut` /
   `fp16` 来自遍历 `vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, ...)`
   （`ffx_vk.cpp:1593/2070`）。本机实测 `wave=32/32 fp16=0` ⟹ **两条扩展都没命中**，走最低配。
   原因不是驱动不支持，而是**那张表按 instance API 版本过滤掉已进核心的扩展**
   （`float16_int8` 1.2 进核心、`subgroup_size_control` 1.3 进核心，而 instance 必然 ≥1.3）。
   两条独立充分的理由保证 **Vulkan 上 `FORCE_WAVE64` 永不进**：后端硬写
   `maximumSupportedShaderModel = 5_1`（`:2055`）⟹ `haveShaderModel66` 恒假；且 wave 停在
   32/32 过不了 `==64`。⟹ **永不挂 `RequiredSubgroupSizeCreateInfoEXT`**（`:3703-3712`）
   ⟹ **不需要给 VkDevice 启用 `VK_EXT_subgroup_size_control`**。
   推论：`_16bit` / `_wave64` 那半批排列在本项目配置下**运行时永不入选**（别去调
   `FFX_FSR3UPSCALER_OPTION_*` 想开它）。排列**缺失**的失败形态 = `ContextCreate` 直接失败
   （`fpGetPermutationBlobByIndex` 报错 → `FFX_VALIDATE` 上抛），**不是**静默用错 shader
   ⟹ 硬闸门本来就在。`just fsr-smoke` 现每机会打 `permutation inputs: ...` 一行，跨机复测不必猜。
7. **5 个 configure key 代码里零解析**（含 smoke）：`FFX_FSR3UPSCALER_CONFIGURE_UPSCALE_KEY_`
   {`FVELOCITYFACTOR`, `FREACTIVENESSSCALE`, `FSHADINGCHANGESCALE`, `FACCUMULATIONADDEDPERFRAME`,
   `FMINDISOCCLUSIONACCUMULATION`}（`ffx_fsr3upscaler.h:176-183`），写入 API
   `ffxFsr3UpscalerSetConstant`（`:587`）。批 14.6 的"调参"目前**没有任何现成接线**可改。

#### 14.5 动手前的两条硬结论（一条翻掉我此前的判断、一条是我方唯一的静默雷）

**① FSR 自己插 layout barrier——不需要手工 sandwich**（此前我读组件层零调用
`fpResourceBarrier` 就下了「FSR 从不做布局转换」的结论，**错了**：VK 后端在
`executeGpuJobCompute`（`ffx_vk.cpp:3806`）里**逐绑定点自动排**——texture UAV →
`UNORDERED_ACCESS`（= `VK_IMAGE_LAYOUT_GENERAL`，`:1068-1069`）、texture SRV →
`COMPUTE_READ`（= `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`，`:1070-1073`），
经 `addBarrier`（`:1237`）排入、`flushBarriers`（`:1305`）随 job 发出。
⟹ 契约变成：**SRV 输入要处于 SRO、UAV 输出要处于 GENERAL**，而
`ffxGetResourceVK(..., state)` 传的 state 是**记账种子**——`addBarrier` 只在
`currentState != newState` 时发，**种子填错 ⟹ 首个 barrier 被跳过 ⟹ shader 在错误布局下读写**。
好消息：布局漂移 VVL 抓得到（14.4b 已实测 VVL 维护 per-image last-known layout），
所以这条不是静默雷，但**种子必须填真实布局**这条纪律要守住。

**② 🔴 挂点处唯一不达标的输入是 velocity RT：它在 `COLOR_ATTACHMENT_OPTIMAL`，不是 SRO。**
`decorateVelocityPass` 刻意不接任何下游（`render_graph_decorate.cppm:305-311`：只有
`colorWrite` 输出 + depth `sampleRead` 输入），velocity slot 的最后一条 barrier 就是它自己的
写 barrier（`newLayout = CAO`），而 `attachmentLayouts` 是**跨帧持久**成员
（`vkrenderer.cppm:321`）且 velocity 是 offscreen（`frameStartDiscard = false`）
⟹ 布局**停在 CAO 且下帧继续被当 oldLayout 用**。对照：scene_hdr/taa_out 与深度在挂点**已经**
是 SRO（tonemap 的 `sampleRead` barrier 在 `vkrenderer_passrec.cppm:482` 已物化；深度由 ao pass
转 SRO，之后 velocity/taa 的「同布局读→读」不再发 barrier）。
**两种修法各带一个不报错的雷**：

| 修法 | 雷 |
|---|---|
| 图外自己发 CAO→SRO | **必须同时写 `attachmentLayouts[velocitySlot]`**，否则下帧 velocity 写 barrier 的 oldLayout 失配 = 债-72 形态（症状隔一整帧、报错点无辜） |
| 改图给 tonemap 加 velocity `sampleRead` 输入让编译器发 barrier | **必须 append 在既有 inputs 之后**：`sampledColorName` 取「**首个** sampleRead 色附件」（`render_graph_decorate.cppm:41-46`），而 `decorateTaaPass` 正是用它决定换哪个输入 ⟹ velocity 排前面会让 TAA 去换 velocity。**TAA 关时不报错，TAA 开时静默错渲染** |

**③ 我方接口面（勘察 + 逐条复核，全部有先例或纯增量）**
- **私有常驻图照抄 `EvssmShadow`**（`evssm_shadow_impl.cppm:139-174`）：`eStorage|eSampled` +
  `DEDICATED_MEMORY` + 一次性 CB `UNDEFINED→GENERAL` + 此后帧内零转移；**GENERAL 常驻图可以被
  sampled descriptor 采**（`imgDesc.layout = eGeneral`，`:181`）本项目已在跑——这正好是 FSR
  要的 UAV 布局。分配入口 `Resources::getAllocator().createImage(...)`（`vkresources.cppm:298`）。
  持有位置 = `Renderer` 的 `unique_ptr` 成员，首帧 `*PreFrame()` 懒构造 + 构造前 `waitIdle()`
  （`vkrenderer_offgraph.cppm:110-136`）。⚠ 别照抄 `ensureVelocitySlot`（走 RT 缓存，无 STORAGE）。
- **exposure 图不存在，纯新增**：曝光在本项目是 scene data 偏移 56 的**标量**
  （`scene_data.cppm:46` → `tonemap.frag.slang:57-60` 的 `asfloat(sceneData.Load(56))`），
  全仓 `eR32Sfloat` 只在格式词表出现过一次。FSR 要的 1×1 R32_FLOAT 图 = 建图 + 每帧搬标量，无先例。
- **`cameraNear/cameraFar` 必须取 `ViewProjection::nearPlane/farPlane`**
  （`scene_context.cppm:589-590`），**不能取 `Camera::nearPlane/farPlane`**——
  `projectionSystem` 做自适应 near/far（按场景包围球）并把**实际生效值写回** ViewProjection
  （`ecs_impl.cppm:349-363`，注释原文「外部若直接拿 camera 的会切错分界」）。
  `cameraFovAngleVertical` = `Camera::fov`，**弧度**（`camera.cppm:49`，`GLM_FORCE_RADIANS`）。
- **门放 `runtime.lua`**（与 `velocity_field` 同形，7 处链路照抄 `velocity_field`）：
  runtime.lua 的门**不在 gate_lint 覆盖面内**（`gate_lint_impl.cppm` 里 velocity/taa/render_scale
  零命中）⟹ 零 lint 改动。放 diagnostics.lua 则要四处同步 + 把两个测试里的 `38u` 改 `39u`。
  「FSR 依赖 velocity」的现成表达 = 新增 `fsrDecorated` 标志，判据取**实际图形态**
  （`门开 && velocityDecorated`），**别拿配置键当判据**（AO 施加点的先例，`main_scene.cppm:437-441`）。

#### 落点（勘察 + 复核后的结论，动手时照此）

调用链 `renderSystem`(`ecs.cppm:166`) → `drawFrame`(`vkrenderer_passrec.cppm:394`) →
`izbPreFrame`(`:444`) → pass 循环(`:479`) → `izbLightPassPre`(`:485`)。
FSR 的两个挂点 = **`:445/446` 之间**（一次性构造，早于显存统计）与 **`:486/487` 之间**
（每帧录制，此时 tonemap 色输入的 barrier 已物化）；实现体放 `vkrenderer_offgraph.cppm`
（其头注释已点名 FSR 是第三个图外 compute），布局护栏照抄本批 14.4b 的 `:70-91`。
换 tonemap 色输入槽 = 照抄 `main_scene.cppm:404-413`（taa 换槽先例）。
⚠ `"tonemap"` 这个 state 名**目前没有常量**（只在词表里），而 `"light"` 有
`kDeferredLightState` + `static_assert` 绑定可照抄 ⟹ 接线时先补 `kTonemapState`。

### 1.13 批 14.5 第一批落地事实（2026-10-08 上午，全部本机实测；FSR 已在渲染器内每帧跑通）

**范围**：`FsrUpscaler` 模块 + 门 + 常驻图 + 每帧 dispatch 已接完，**输出尚无消费者**
（tonemap 仍读 `taa_out`/`scene_hdr`）。所以下面第一条判据是「门开也逐位不变」——这是本批
**故意**的边界，不是漏了。换 tonemap 色输入 = 第二批。

**六条实测才拿到的事实**（§1.12 的推演没覆盖到，动手前若知道能省一整轮）：

1. **FSR 的 SPIR-V 声明 `Float16` + `Int16` capability，与它选哪个 fp16 数学排列无关。**
   生成期 `FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF = 1` 让 shader 用 half
   **采样数据**，那条路径恒开 ⟹ 即使排列是「最低配 fp16=0」也照样声明。缺位时
   `vkCreateShaderModule` 报 `VUID-VkShaderModuleCreateInfo-pCode-08740`。
   ⚠ 这**推翻**了 §1.12 里「fp16 不支持 ⟹ 无关」的推断（那条推断只对「数学指令」成立）。
2. **这两个位分居两处，且都不能走 `features_config.json` 的 optional 机制**：`shaderInt16`
   在 `VkPhysicalDeviceFeatures`（基结构），`shaderFloat16` **只在**
   `VkPhysicalDeviceVulkan12Features`（基结构里根本没有它）。给配置挂
   `VK_KHR_shader_float16_int8` 去探 = **永远探不到**——该扩展 1.2 已进核心，被 instance 的
   API 版本从 `enumerateDeviceExtensionProperties` 的表里过滤掉了（与 §1.12 第 6 条
   「FSR 排列探测恒选最低配」是同一个机制）。⟹ 走 `DeviceContext::probeShader16Supported`
   直接读 `Vulkan12Features`，置位也写回同一个结构；语义仍是 **optional**（缺位的机器
   只是跑不了 FSR，不该连应用都起不来），消费侧 `fsrInputsReady` 探测后降级。
3. **FSR 的 VK 后端用 classic descriptor sets，会把我们的 descriptor heap 绑定作废。**
   VVL 原话："The command buffer is currently in 'Classic Descriptor' mode from previous call
   to `vkCmdBindDescriptorSets` which invalidated the previous 'Descriptor Heap' mode"。
   ⟹ **每次 FSR dispatch 之后必须重绑两个 heap**，否则同帧其后每个 mesh draw 报
   「heap was not bound」（实测 10 条）。这是 `docs/DESCRIPTOR_HEAP_SPEC.md` §坑 9 的**新实例**，
   已把绑定抽成 `Renderer::bindDescriptorHeaps` 供两处共用。
4. **vendor 的 clear job 走 `vkCmdClearColorImage`**（`ffx_vk.cpp:4239`，前面还自己发一条到
   `COPY_DEST` 的 barrier）⟹ 我们替它建的共享图**必须申请 `TRANSFER_DST`**，
   少这个位 = clear + 两条 barrier 双双违 VUID（实测 19 条）。
5. **共享图的格式必须照 vendor 给的原样建、并原样报回去。**
   `reconstructedPrevNearestDepth` 的真实格式是 **`R32_UINT`**（不是直觉上的 R32_FLOAT）；
   按 float 报 ⟹ vendor 按报的格式建 view ⟹ 「`OpTypeImage` 的 Format 与 `VkImageView` 不符
   = 该变量所有读写值未定义」+ 「storage image 要 UINT 组件类型」（实测 24 条）。
   ⟹ 四张私有图各自存一份 `FfxSurfaceFormat`，wrap 时取它，**不写字面量**。
6. **FSR 侧显存 = 44 MiB**（`ffxFsr3UpscalerContextGetGpuMemoryUsage`，1280×720 档）。
   14.8 的「外部分配器」科目读数来源确认存在且可用。

**两条新 vendor 补丁**（`third_party/fsr/PATCHES.md`）：P12 = `findMemoryTypeIndex` 不跳
`DEVICE_COHERENT` 内存类型（RADV 上选中它 = 未启用 `deviceCoherentMemory` 就分配，
`VUID-vkAllocateMemory-deviceCoherentMemory-02790`）；P13 = clear 的 `layerCount` 直接取
`resourceDescription.depth`，而 **TEXTURE2D 的 `depth` 是未填的 0**。两条原本都是
`just fsr-smoke` 的**白名单**条目；渲染器侧接 FSR 后它们按帧刷屏，故**就地修掉而不是继续
白名单**（白名单在冒烟里是"知道上游有问题"，在渲染器里等于把 VVL 输出彻底淹没）。

**验收结果**：① 门关逐位不变 —— ✅ 截图 SHA256 = `fdece9d0…` = 跨会话红线基线；
② 门开**也**逐位不变（输出无消费者）—— ✅ 同 SHA；③ VVL 零错 —— ✅ 门开 `just smoke
--fsr-upscaler` PASS errors=0 warnings=0；④ **活性判据两条变异均验红** ——
M1 把 depth 申报成 GENERAL（真实 SRO）⟹ VVL 逐帧响在 depth 图上（证明 FSR 真的按我们申报的
state 绑并读我们的深度图，而不是"跑通了但没碰图"）；M2 转了布局但漏写追踪表 ⟹
**我们自己的护栏** error 出声（证明 14.4b 那道断言是活的）。还原后复绿。

**方法论**：本批又是「推演被实测推翻」的一次（第 4 次，见 §1.12 末）。§1.12 的三条结论里
「不需要给 VkDevice 加 `VK_EXT_subgroup_size_control」仍然成立，但「fp16 无关」被推翻。
新登记的通用教训：**vendor 的 capability 声明面 ≠ 它的排列选择面**——查"要不要某特性"要
看**编出来的 SPIR-V 声明了什么**，不能只看排列表怎么选的。

## 1.14 批 14.5 第二批实测：接进 tonemap 之后，**时域档在我们的输入口径下退化**（本机 STRIX_HALO）

**本批落地的接法 = 零拷贝**：`Resources::ensureFsrSampleSlot()` 发号 + `FsrUpscaler` 构造时
把私有输出图的 view 写进该槽（`EvssmShadow` 的 `momentsSampleSlot` 同族），装配层把
tonemap 的色输入槽号换成它。原计划的「copy 进图附件」被放弃，两个理由：① FSR 的 UAV 输出
必须常驻 GENERAL，现有 usage→layout 表表达不了；② 图里没有 `fsr_out` 的写者，编译器对
「读一个无图内写者的 offscreen 附件」的判定是额外风险。**代价 = 零**（少一次每帧 3.7 MB 拷贝）。

**判据口径**：本批第一次让画面真的变化，红线基线 `fdece9d0…` 只约束**门关**那一档（实测
门关仍逐位 = 基线 ✅）。门开的量化用**边缘能量 mean|∇|**（PIL `FIND_EDGES` 均值）——
它是**报告值不是红线**（`rgl-test-discipline` 的口径），用途仅限"FSR 该比双线性锐"这一条
方向性判断。

| 配置（1280×720 上屏，静态场景） | 边缘能量 |
|---|---|
| renderScale=1，FSR 关（= 红线基线） | 16.98 |
| renderScale=0.5，FSR 关（双线性放大） | 7.95 |
| renderScale=0.5，**FSR 纯空间**（本批默认） | **8.43** ✅ 优于双线性 |

**时域档是坏的**。二分链（每步都是实测，不是推演）：

1. **不是空间路径**：每帧 `reset`（= 只用 EASU+RCAS）拿到 8.43 > 双线性 7.95 ⟹ EASU/RCAS 正确。
2. **不是速度场**：把速度图当颜色喂进 FSR 直读，R=G=**恒 0**（14.4 的静止正锚点没坏）。
3. **不是字节布局**：怀疑 vendor 按 2×float32 读我们 4×float16 的纹素，把速度输出 `.zw`
   从 `(1,0)` 改成 `(0,0)` ⟹ 指标**一位不动**，排除（字节布局错不会随帧时变化，
   这条排除不受下面"数字作废"影响）。
4. **不是相机参数**：日志实测 `near=0.386 far=2.71 fov=0.785 exposure=1`，是投影矩阵真值。
5. **不是共享图尺寸/格式**：三张私有图全部按 vendor 的 `FfxCreateResourceDescription` 建并
   原样报回（`:823-827` 三张均 `maxRenderSize`，历史图 `:581/584` 均 `maxUpscaleSize`）✅。
6. **不是 HDR 往返**：`Accumulate()` 每帧对历史做 `Tonemap → … → InverseTonemap`，怀疑 fp16
   往返压对比度；关 `ENABLE_HIGH_DYNAMIC_RANGE` 只动一点点，**不是主因**
   （第五批在修正帧时后重测：14.97 vs 14.70，同样只动一点，结论不变）。
7. **不是漏调 sc 每帧 API**：`frameIndex` 由 vendor 在 `:1028-1030` 自管（reset 归零 / 否则 +1）。
8. `ENABLE_DEBUG_CHECKING` 打开后**无任何输出**（这条是真编进去之后测的——第一次"没输出"
   是我补丁本身有语法错误、根本没编进去，见下）。

> **本节原有的一张「时域档」表与一整段「形态学证据」已删除**（2026-10-08 第五批）。它们的
> 测量方式不成立：拿 `delay_frames` 分次跑比"帧 N"，而时域档的 `frameTimeDelta` 吃墙钟
> ⟹ 跨运行的"帧 N"根本不是同一个量。由此得出的「逐帧单调衰减」「一次累积掉 36%」
> 「高斯拟合 σ≈1.4px」全部作废。当前结论与正确的测量方式 → **§1.17**。
> 当时据此提出的「输入没有亚像素抖动 = 设计点外」假设也已被否证（§1.16）。

**本批抓到的两条真问题**：

- **dispatch 的 6 个标量字段原本全是零初始化**（`enableSharpening=false` ⟹ **RCAS 那个 pass
  根本不录**；`sharpness=0`；`frameTimeDelta=0` ⟹ `deltaTime=clamp(ms/1000,0,1)=0` 把时域
  时间常数废掉；`preExposure=0` 被 vendor `:256` 判 invalid 并回落）。根因 = **14.2 的
  《FSR输入契约》只写了四个"输入资源"的口径，一个标量都没写**，照那份文档接线就全漏。
  ⟹ 契约文档已补 dispatch 标量一节。
- **一次"读半截代码推翻探针"的错误改动**（已回退）：只看到 `LoadInputMotionVector` 里
  `fUvMotionVector = fSrcMotionVector * MotionVectorScale()` 是**乘法**，就断定 vendor 不除、
  把 `motionVectorScale` 从 `(1,1)` 改成 `1/尺寸` —— 而 `ffx_fsr3upscaler.cpp:996-997` 早把
  本字段除以 `motionVectorsTargetSize`，改完等于让速度场**小 1000 倍**（相机一动就鬼影）。
  正确答案在 `FSR输入契约.md` §1，且那条是**喂常量 MV 回读 `dilatedMotionVectors` 对拍 +
  变异验红**得来的。**教训：探针验过的结论 > 一次不完整的代码阅读；要推翻它必须重跑探针，
  不是重读代码。** 这条反过来是 14.2 那次投入的正面证据——文档把结论保住了，是我差点
  把它改错。

**方法论（本批新增，第 5 次）**：本轮又出现「我以为在做的实验其实没编进去」（语法错误被
`>/dev/null` 吞掉 ⟹ 拿旧二进制得出结论）。**做实验时不许把 build 输出丢进 /dev/null**；
判据说"没输出"之前，先确认改动真的进了二进制。

## 1.15 批 14.5 第三批实测：AO 施加点从 TAA 里拆出来（2026-10-08，本机 STRIX_HALO）

> 触发 = 用户一句话："之前 SSAO 和 TAA 合并了，如何处理下把它拿出来"。直觉完全正确，
> 但**被合并的不是 SSAO，是「AO 的施加点」**——这个区分决定了改动范围（ao pass 本身一行没动）。

### 耦合的真实形态（先查清再动手，不猜）

| 配置 | AO 从哪来 | AO 在哪施加 | 为什么 |
|---|---|---|---|
| TAA 开 | 独立的 `ao` pass（**没被合并**） | **taa.frag 内**，时域滤波**之前** | 批 13.6：AO 乘在时域之后 = 噪声零平均，实测占残余 shimmer **62%** |
| TAA 关 | 同上 | tonemap 内 | 没有时域滤波器，乘在哪都一样 |
| **FSR 开 + TAA 关（本批要的路）** | 同上 | ⚠ 只能落回 tonemap = 时域**之后** | **正好退回 13.6 修掉的形态** |

切换机制 = scene data @722224.z 一个**运行时标量**（`tonemap.frag` 的
`lerp(lerp(1,ao,strength), 1, flag)`），CPU 侧写的是 `decorateTaaPass` **实际结果**而非配置键。
⟹ 真正的病灶 = **不变量「AO 必须在时域滤波器之前施加」被硬编码成了「时域滤波器 == taa pass」**。

### 三条路的取舍（用户拍板甲）

| 路 | 代价 | 门关档逐位不变？ |
|---|---|---|
| **甲：独立 `ao_apply` 全屏 pass** ✅ | +1 全屏 draw + 一个 pass 词表项 | ✅ **天然守住**（门关 = 不插） |
| 乙：AO 施加搬进 `ao` pass（零新增 draw） | 省一次 draw | ❌ AO 乘法从 tonemap 内 float32 变成写 RGBA16F 时量化一次 ⟹ 红线要重立 |
| 丙：改 `taa.frag` 支持「只施加 AO 不做时域」 | 零新 shader | ⚠ 精度同今天，但要动 Phase 13 冻结文件，回归面最大 |

甲的红线之所以是**结构性**守住（不是靠小心）：施加点由**图形态**决定，门关时装饰器根本不插，
tonemap 继续自己施加 = 图与命令流零变化。

### 落地形态（八处）

- **shader** `rgl/shaders/ao_apply.frag.slang`：色采 `input.uv`（留在抖动空间）、AO 采
  `baseUv = saturate(uv + readTaaParams().zw * texel)` + **线性采**。两条纪律从 taa.frag 批 13.6
  原样继承。**TAA 关时 c_t 恒 0 ⟹ baseUv == input.uv**，与 tonemap 今天的采法逐位等价——
  这句不是为今天写的，是为了**接 FSR 抖动线那天**：届时色带抖动而 AO 不会。
- **词表** `kAllPassStates` 12→13（fullscreen 档 5→6）；StateKit 加成员 + 槽位表（`static_assert`
  逼着两处同步，漏一处编译不过）。
- **装饰器** `decorateAoApplyPass`：一附件（`ao_applied`, rgba16_sfloat, **渲染**网格）+ 一 pass
  + 把 tonemap 的色输入换成 `ao_applied`。**三条跳过条件**：① 图里已有 `taa` pass（防双施）
  ② 无 `ao` 附件 ③ 无「写 swapchain 且采色」的 pass。**次序契约** = 必须在 picking / highlight /
  taa 之后装饰（本 pass 采色，不像 velocity 那样靠"不采色"天然免疫锚点误认）。
- **管线条目** `addAoApplyEntry`（binding 4 色 / 5 共享 sampler / 6 ao 可见度；mesh 复用大三角）。
- **RT 领槽** `ensureAoAppliedSlot`（仅 `aoApplyDecorated` 时领 = 门关堆槽零差异）。
- **装配** tonemap 烘进去的堆槽优先级 **ao_applied > taa_out > scene_hdr**；FSR 开时整条被它的
  零拷贝输出槽顶掉（两者写同一个 binding、互斥）。
- **FSR 色输入**优先级 **ao_applied > taa_out > scene_hdr**（`initFsr`）。
- **标志改名** `readTaaAoApplied` → `readAoAppliedPreTemporal` / `SceneEnv::aoAppliedPreTemporal`：
  施加点从「只有 taa」变成两个来源，旧名会误导。**零布局变更**（复用 @722224.z 保留位）——
  `just reflect` 后生成物**逐字节未变**，只有 manifest 的输入 SHA1 变了。

### 判据（全部本机实测）

| 判据 | 结果 |
|---|---|
| 门关（默认）逐位不变 | ✅ SHA256 `fdece9d0…` = 红线基线 |
| 三档图形态 | ✅ FSR 单开 → 插（`ao_applied … slot 8256`）；**FSR+TAA → 不插**；TAA 单开 → 不插 |
| VVL | ✅ 四档全 PASS errors=0 warnings=0 |
| **AO 恰好施一次**（本批承重判据） | ✅ **量级比 1.007**（见下） |
| `just test` | ✅ 226/226（新增 2 条装饰器 TEST_CASE） |
| style / doc-check | ✅ 各 0 警告 |
| 装饰器判据变异 | ✅ **四条全验红**（防双施护栏 / 输入顺序 / ao 输入声明 / 消费者改写） |

**「AO 恰好施一次」的判据长什么样**（这条同时钉住两个相反的失效方向，值得记下来）：
跑四张图 —— {FSR, 门关} × {ao.strength 0.5, 0}，比 **AO 净变暗的量级**：

| | AO 开 | AO 关 | 净变暗 |
|---|---|---|---|
| 门关（tonemap 施） | 78.264 | 79.547 | 1.284 |
| FSR（ao_apply 施） | 78.402 | 79.695 | 1.293 |

**比值 1.007**。零施 ⟹ 比值 ≈ 0（FSR 若读无 AO 的 scene_hdr 而 tonemap 又跳过，AO 直接消失）；
双施 ⟹ 量级翻倍。单看"画面有没有 AO"分不清这两种失效，**只有差分分得开**。

### 顺带还掉的两笔（被 500 行红线逼出来的，不是顺手洁癖）

- **`main_scene.cppm` 拆出 `main_wiring.cppm`**（`module main:wiring;`）：本批给它加 9 行代码时
  撞上「单文件代码行 ≤500」（该文件此前已顶在 500）。红线不是要绕的、是要**找缝**的——
  `wireHotReload` / `wireShaderHotReload` / `wirePicking` 是整文件里唯一**零引用本文件 file-static**、
  不碰 GPU 资源创建的一段（只做「构造服务 + emplace ctx + 闭包桥」）。拆后 509→**400** / 129，
  并摘掉 6 条随之失效的 import（留假边 = 让慢模块白白成为编译前置）。
- **`assembleBackEnd` 18 参 → 15 参**：`CompiledRenderGraph&& + deferredMode + taaDecorated +
  velocityDecorated + aoApplyDecorated` 五个散装参数并成一个 `CompiledGraphResult&&`。散装标志是
  **真漂移源**（加一个装饰标志要改三处，签名缩进已经歪了）；顺带把 `main()` 从 122 行压回 ≤120
  （红线），而不是靠拉一条 155 字符的长行绕过去。

**方法论（本批，第 6 次踩同一坑）**：变异脚本里我把 `cmake --build` 的输出丢进了 `>/dev/null`，
四条变异里**两条其实根本没编译**（语法错误被吞），差点得出"判据抓不住"的相反结论。
**判据变异必须看编译输出**——"没红"有两种成因：判据无效 / 变异没进二进制。

## 1.16 批 14.5 第四批实测：抖动接完了，**§1.14 那条最优假设被否证**（2026-10-08，本机 STRIX_HALO）

> 本批的**主要产出是一条负结果**。它把 14.5 时域缺陷的排查空间砍掉了一整块，
> 并且顺带修掉一个潜伏错。先说结论：**FSR 时域档退化与"输入没有亚像素抖动"无关。**

### 病灶第三例：抖动也硬编码在 `env.taaEnabled` 上

`ecs_impl.cppm` 的 `taaSystem` 里，`jitterPx` 与 `applyProjectionJitter` 都挂在
`env.taaEnabled` 上——与 AO 施加点（§1.15）**完全同构**的硬编码。解法也同构：抖动注入的
开关改成「TAA 在 **或** FSR 要抖动」，且 **TAA 在时一切走原路**（周期 16、幅度
`taa_jitter_scale`）⟹ TAA 档逐位不变（实测见下，不是推理）。

### 省下一整轮口径研究的关键事实：我们的抖动序列**就是** FSR 的抖动序列

`ffxFsr3UpscalerGetJitterOffset`（`ffx_fsr3upscaler.cpp:1326-1344`）=
`halton((i % phaseCount) + 1, 2) - 0.5` / base 3 取 y。与我们的 `taaJitterPixels`
（`haltonSequence` 定义式 + `% period + 1`）**逐位同式**（两处 `halton` 的 `f` 首次除法
顺序等价）。**唯一差别是周期**：我们 16，vendor 用 `8·(上屏宽/渲染宽)²`（2× 档 = 32）。

周期不是可选参数：vendor 在 dispatch 里**自己算** `jitterPhaseCount`
（`ffx_fsr3upscaler.cpp:1010`，不开放成入参），shader 拿它算历史寿命衰减
`fLifetimeDecrease = (0.1f / JitterSequenceLength()) * …`（`accumulate.h:86`）。
我们生成抖动的周期必须等于它 ⟹ app 侧必须复刻那个公式。
**复刻的判据不靠"我读了它的实现"**：`ffx_fsr3upscaler.h:467-473` 把五档对照表写在**公开
头文件**里（1.5x→18 / 1.7x→23 / 2.0x→32 / 3.0x→72），单测按那张表判。实现刻意用
`std::pow(n, 2.0f)` 而不是 `n*n`——要匹配的是 vendor 的实现，外层是**截断**取整，
差一个 ULP 就从 18 掉到 17。

### 顺带修掉一个潜伏错：cancellation 标志与速度场空间不配对

`velocity.frag` 输出的是**基空间**速度（它自己走过 `toBaseSpace`，文件头第 4 条写明配对：
基空间 ⟹ `ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` **不开**），而我们的 context 一直
**开着**这个标志。开着 = vendor 对一个不含抖动项的量再减一次 `(prevJitter − curJitter)/size`。

**为什么现在改是零风险**：抖动此前恒 0 ⟹ 抵消项恒 0 ⟹ 开着与关着一模一样，所以这个配对错
**从没暴露过**；把抖动打开它立刻变成真错（静止表面被判成在动）。判据 = `just fsr-smoke` 的
§3 探针（它测的正是这一项，且自建 context 不受本改动影响，改后仍全 PASS）。

### 实验结果：**否证**

renderScale 0.5（渲染 640×360 → 上屏 1280×720，相位 32，满幅 ±0.5px，cancellation 已修）：

| 配置 | 边缘能量 | 均值 |
|---|---|---|
| 0.5 双线性（无 FSR） | 7.97 | 79.10 |
| 0.5 FSR 纯空间 | 8.46 | 78.34 |
| 0.5 FSR 纯空间 + 抖动 | 8.30 | 78.08 |
| 0.5 FSR 时域（无抖动） | 3.54 | 79.03 |
| **0.5 FSR 时域 + 抖动** ★ | **3.50** | 78.85 |

**对照组**（排除"没收敛"与"档位不对"两种解释）：

| 配置 | 边缘能量 | 排除掉什么 |
|---|---|---|
| frame 32 时域 + 抖动（跑满一个完整抖动周期） | **3.10** | 「8 帧不够、要一整个周期才收敛」——**反而更差** |
| frame 32 时域 无抖动 | 3.47 | 抖动开/关**没有差别**（3.50 vs 3.47） |
| frame 32 纯空间 + 抖动 | 8.11 | 空间路径始终健康，坏的只有累积 |

⟹ §1.14 写的「**时域档在我们的输入口径外**（没有亚像素抖动）」这条**当前最优假设被否证**。
它同时把 §1.14 的形态学结论**加强**了：静态机位 + 正确申报的抖动 + 正确的速度场空间下，
逐帧单调变糊依旧存在 ⟹ 只剩「vendor 的历史读取本身不是恒等重采样」这一类解释。

### 仪器有效性自证（否则这条负结果不算数）

"没改善"有两种成因：假设错了 / 抖动根本没到 FSR。逐帧打印申报值确认：

```
jitter=(0,-0.1667) (-0.25,0.1667) (0.25,-0.3889) (-0.375,-0.0556) (0.125,0.2778) …
```

= 标准 Halton(2,3) 周期 32、逐帧在 ±0.5 内变化。相位数日志 `相位 32 = 渲染 640 / 上屏 1280`
与 vendor 公式一致。另外 `mip` 假设（历史图 LOD 选错会糊）经查**排除**：我们替 vendor 建的
资源 `mipLevels` / `mipCount` 全是 1；深度口径 §2/§3 早由探针带反向用例钉过。

### 下一步的具体仪器（这次真的能问到点上）

**历史图与三张私有图是 `fpCreateResource` 回调里我们自己分配的** ⟹ **全部可回读**。
所以不必再靠输出图猜：直接回读累积后的历史，问「它是上一帧输出的恒等拷贝，还是一张被
亚像素重采样过的糊图」。§3 探针早就用过同一手法（回读 `dilatedMotionVectors`），
这是本项目少数几个能**直接看 vendor 内部状态**的杠杆，14.5 时域线的下一批应该建在这上面，
而不是再猜第 9 个假设。

**方法论（本批，第 7、8 次踩同一坑，换了两个新马甲）**：
① 临时实验补丁花括号不平衡 ⟹ build 失败被 `>/dev/null` 吞 ⟹ 我拿旧二进制跑完了整组实验，
是在**刚写完**"这条该升级成硬断言"之后发生的。② 变异测试用 `just build` ⟹ **`tests/` 是
`EXCLUDE_FROM_ALL`，`just build` 根本不编 `rgl_tests`** ⟹ 变异同样没进二进制。
本批起实验一律走带断言的 build 包装（失败即停 + 二进制新鲜度检查），变异一律
`cmake --build build --target rgl_tests`。**"没红"的两种成因必须用工具消灭，不是用提醒。**

## 1.17 批 14.5 第五批（诊断批，**无产品代码改动**）：时域档的**测量方式**本身是错的，纠正后病灶收窄到一处（2026-10-08，本机 STRIX_HALO）

### 一、先推翻自己：此前所有时域数字都测在退化帧时上

`dd.frameTimeDelta` → vendor `deltaTime = clamp(ms/1000, 0, 1)` 是时域滤波器的**时间常数**。
而 `llm_debug` 截图路径**不出图、无 present 节流**，帧间隔可至亚毫秒 ⟹ `deltaTime ≈ 0.001`
⟹ 每帧累积权重趋零 ⟹ 输出几乎等于历史。把 `frameTimeDelta` 钉成 16.667ms、其余全不动：

| 1:1 时域档 | 边缘能量 |
|---|---|
| 墙钟 dt（亚毫秒，`llm_debug` 原样） | 3.47 |
| **dt 钉成 16.667ms** | **6.79** |
| `/shotburst`（headless eFifo，自然 ~16.7ms） | 7.03 |

钉一下跳一倍，并与另一条采集路径对齐。**空间档在两条路径下完全一致（均 14.97）** ⟹ 差异
不是采集路径本身，是**帧时**，而只有时域档吃帧时。

**由此作废、已从 §1.14 / §1.16 删除的结论**：逐帧单调衰减、「一次累积掉 36%」、高斯拟合
σ≈1.4px、「frame 32 对照组」。共同错误 = **拿 `delay_frames` 分次跑比「帧 N」**，
而时域档的「帧 N」跨运行不可比（帧时不同 ⟹ 累积权重不同）。

### 二、正确的时域量具（项目里早就有，之前没用）

`POST /shotburst`（RPC；`diagnostics.rpc` 默认关）。它与 `/shot` 的本质区别 = 「下一张」由
**帧首结算钩子**登记 ⟹ 帧号连续；`/shot` 每张的帧号取决于请求落在帧首钩子前还是后（= 墙钟）。
skill `rgl-verify` 明写了这条（"凡被测项本身是时域的，必须走 shotburst 连拍"），本批是撞上才用对的。

**对照连拍是判据成立的前提**：同配置时域**关**连拍 4 帧，相邻帧**逐位相同**（差异像素 0）
⟹ 场景确实静止、采集机制不引入差异。缺这条，下面所有数字都没有意义。

### 三、当前结论（全部为单运行内连拍 + 已知帧时）

| 项 | 结论 | 怎么量的 |
|---|---|---|
| 时域档坏不坏 | **坏，且是实的** | 同路径对照：时域关 14.97 / 开 7.03（47%） |
| 形态 | **收敛到不动点**，不是渐进衰减 | 单运行连拍 14 帧：6.80→6.90 单调**微升**，相邻帧差 0.059→0.040 单调降 |
| 是不是重投影错位 | **不是** | 相邻帧相位相关位移精确 `(0.00, 0.00)`，相关峰/均值 91 万 |
| 抖动能不能救 | **不能**（修正帧时后重测仍成立） | 1:1：6.79 → 加抖动 4.92（更差）；0.5：3.54 → 3.50 |
| 0.5 档是不是帧时假象 | **不是** | 钉 dt：3.50 → 3.54，几乎不动 |

**定位**：`out = (1−w)·H_sample + w·Cur`。静态零运动下稳态 `out = H`，而稳态比 `Cur` 糊
⟹ **`H_sample` 是 `H` 的一个模糊版本**。已排除：位移（相位相关）、mip/LOD（我们建的资源
mip 全 1）、HDR 标志（±0.2）、Lanczos 核型（见下）。⟹ 只剩**历史读取所用的采样滤波本身**
（半纹素偏移的双线性，或一个显式邻域 gather）。

### 三·五、退化图像的**定量指纹**（下一步的搜索空间收窄成一句话）

拿同路径、同机位、跨运行对齐已验（相位相关 (0,0)）的两张图对拍：A = 时域关，B = 时域开收敛态。

| 拟合 | 结果 |
|---|---|
| 纯增益 `B = a·A + b` | a = **0.983**，残差 84.1 |
| 模糊+增益 `B = a·(G_σ⊛A) + b` | **σ ≈ 0.75–1.0**（残差极小值平坦：31.6@0.75 / 32.2@1.0），a = **1.007**，残差 **31.6** |

**独立判据**（不依赖拟合，增益改不了它）——多尺度边缘能量比值 `edge(G_σ⊛B)/edge(G_σ⊛A)`：

| 预模糊 σ | 0.0 | 0.5 | 1.0 | 2.0 |
|---|---|---|---|---|
| 比值 | 0.470 | 0.512 | 0.717 | **0.928** |

细尺度差一倍、粗尺度几乎没差 ⟹ **真的高频损失（空间模糊）**，不是对比度被压（纯增益在所有
尺度上比值恒定）。各向同性检查：x 方向比值 0.545、y 方向 0.557 ⟹ **各向同性的 2D 核**，不是单轴。

**指纹 = 各向同性、σ≈0.8、增益 1.0 的低通，只在时域路径上。** 对照候选核解析 σ：
双线性@半纹素 0.500 / Catmull-Rom@半纹素 0.456 / **[¼½¼]⊗[¼½¼] 0.707** / **3×3 box 0.816** /
5×5 box 1.414。⟹ **量级上是"一个 3×3 低通"，不是半纹素偏移**（后者只有 0.5，且会各向异性地
偏向单轴）。

**由此排除的两条**（原本都在嫌疑名单上）：
- **不是对比度 / YCoCg 或 HDR 往返**：增益 = 1.000，R² 0.9915。
- **不是历史采样的半纹素偏置**：σ 量级不对（0.5 vs 0.8），且半纹素偏置解释不了各向同性。
  另外历史采样用的是 FFX **双三次**重采样器（`reproject.h:34` 的 `DeclareCustomTextureSample`，
  融进 accumulate pass），零运动**本该**落在整数纹素 = 恒等；`previousFrameUpscaleSize`
  也确认每帧维护（`ffx_fsr3upscaler.cpp:964-965`）。
  > ⚠ 本批据此写的"`REPROJECT_USE_LANCZOS_TYPE` 的 0=Reference / 1=LUT **两者都是正规 Lanczos**"
  > **已被 §1.18 实测推翻**——VK 后端两个分支都读同一张 LUT 纹理，而那张 LUT 就是病灶。
  > **"本该恒等"不等于"实测恒等"，这条推理链的断点正是下一批的入口。**

**下一步要问的问题因此被收窄成一句**：accumulate 写回历史的那个值，什么时候会变成一个 3×3
邻域均值？（FSR 的邻域裁剪本来就要在 accumulate 里算 3×3 的 min/max box —— 量级吻合不是巧合，
但**这条只是量级对照，不是结论**，读到历史图才算。）

### 四、顺手排除的两条 + 一条死旋钮

- **EASU 在 1:1 掉 19% 不是 bug**：只 EASU（RCAS 不录）= 81%，EASU+RCAS(0.2) = 88%，基线 100%。
  核半径确实随缩放走（`fKernelSizeBias = 1+(1/downscale−1)`，1:1 时 = 1.0 = 最窄），
  1:1 本就不是 EASU 的设计点。**RCAS 是净加分**（+7%），锐化在正常工作。
- **Lanczos 核型是死旋钮，别再试**：`FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE` 从 2
  （近似）改成 0（真 Lanczos2，整数偏移处为零点 ⟹ 1:1 本该恒等），重生成 2560 排列 + 重编 + 跑
  ⟹ **输出字节完全相同**。**它不在 vendor 里，在我们自己的
  `third_party/fsr/tools/gen_ffx_shader_blobs.py` 的 `BASE_DEFINES`**——vendor 头那行有
  `#ifndef` 守卫、命令行 `-D` 优先，**改 vendor 头是空操作**（本批先踩了这个空，两次测量白做）。
- 还原 vendor 与 blobs 后 `fsr-check` 护栏通过（38 输入 / 41 产物）、门关 SHA256 逐位不变。

### 五、下一步（原计划已被替代，见 §1.18）

原计划 = 包住 vendor 的 `fpCreateResource`（`FfxInterface` 是函数指针表：拷一份、换掉一项、
其余转发）截获 `INTERNAL_UPSCALED_COLOR_1/2` 回读历史，问"恒等拷贝还是被糊掉的图"。
**该动作未执行**：§1.18 改用**更便宜的两条**（vendor shader 消融 + accumulate pass 运行时探针，
每轮 41s 重排 + 一次 `/shotburst`）达到了同样的二选一判决，且顺带把嫌疑逐个二分排除。
**教训：回读产物不是唯一出路——能在 shader 里做单变量消融时，它比截获资源更便宜也更可归因。**

### 六、`llm_debug` 截图路径 = 删除候选（工具账，不是判据账）

RPC 的 `/shot` + `/shotburst` 覆盖得了 `llm_debug` 的截图职责，且**只有 RPC 路径测得了时域**
（帧号连续、采集时机可控）。`llm_debug` 剩下的独有价值只有"截图时顺带直读某个 G-buffer 通道"
（`dump_gbuf`），而那件事 RPC 也挂得上。

**倾向：未来删掉 `llm_debug` 截图路径（含 `delay_frames`），截图统一走 RPC。** 理由不是它质量差，
而是**两条采集路径 = 两个测量口径**，本批正是被这个双轨坑的（同配置时域档 3.47 vs 7.03）。
**在册待办、未开工**；动它之前要确认 `just smoke` / `just shot` / imgqa 的调用面迁移干净。

> `delay_frames` **不单独写使用纪律**：它的命运是随 `llm_debug` 一起删，不是被更正确地使用。
> 要测时域，直接用 `/shotburst`。

**方法论（本批）**：带断言的 build 包装第一次真正拦住一次事故（临时补丁触发
`-Werror,-Wunused-variable`，脚本直接拒绝继续 ⟹ 没有第 10 次拿旧二进制得结论）。
但同批仍有两次「改了个空操作旋钮并据此下结论」——**根因相同：改完没先证明"改动可观测"**。
下次任何"改个参数看效果"的实验，第一步是问**这个改动有没有可能不影响输出**，
而不是先看输出变没变。

## 1.18 批 14.5 第六批（消融批，**无产品代码改动**）：时域档发糊的根因 = **VK 后端的 Lanczos 权重 LUT 采样**（2026-10-08，本机 STRIX_HALO）

### 一、结论一句话（**根因 = vendor host 侧一个 `abs` 的名字解析 bug**）

`ffx_fsr3upscaler.cpp:164` 的 `static float lanczos2(float value)` 写的是
`abs(value) < FFX_EPSILON ? 1.f : <闭式>`。该函数在**全局作用域**调**非限定** `abs`，而 libstdc++ 的
`<cmath>` 只把浮点重载放进 `std::`，全局只有 `<stdlib.h>` 的 `::abs(int)` ⟹ **浮点被截断成整数**：
`|value| < 1` 一律截成 0 ⟹ `0 < 1e-6` 成立 ⟹ **返回 1.0**。转折点恰在 `|value| = 1`。

于是 host 生成的那张 128×1 `R16_SNORM` Lanczos LUT **退化成一个阶跃**（实测：i=0…63 全存
`32767` = 1.0，i=64 起才回到真核值），**Lanczos2 在 x=1 的零点没了**。VK 后端的
`SampleLanczos2Weight`（`callbacks_glsl.h:735`）不是闭式核而是**采样这张 LUT**，历史读取（reproject）
的 4×4 权重全部来自它 ⟹ t=0 处的实际权重变成

| | 中心 | 左邻 | 右邻 |
|---|---|---|---|
| vendor 本意 | 1.0 | 0 | 0 |
| **实测（LUT 阶跃）** | **0.5012** | **0.2494** | **0.2494** |

**= 一个 [¼½¼]，x/y 各一个 ⟹ σ=0.707 的 3×3 低通。** 而 accumulate 的当前帧权重 α ≈ **2.3%**
（`fUpsampledWeight = 1/43 × fKernelBiasWeight`，与 worker bench 实测 0.0231 吻合）⟹ EMA 把这个
低通**放大 ~44 倍** ⟹ 画面发糊。

**修法 = 一个 token：`abs` → `fabsf`（host 侧，不用重排 shader blob）。** 实测边缘能量
**6.01 → 13.7~13.9**，即无 FSR 参照的 **104~105%**。（该指标到第 8 帧**仍在单调上升**，
未完全收敛 —— 两次跑分别 13.72 / 13.90，差异就是收敛程度，不是噪声。）

> **与 §1.17 闭环**：当年从**图像域**独立拟合出的退化指纹是"σ≈0.75–1.0 的各向同性低通"，
> 并把 `[¼½¼]⊗[¼½¼]`（σ=0.707）列为候选核。**本批从 host 侧 dump 出的 LUT 精确命中该核。**
> 两条毫不相干的测量收敛到同一个数 —— 这是本批最强的证据形式。

### 二、判决表（静止机位、render_scale=1.0、抖动关、warm 30 + n 8、`/shotburst` 连拍；指标 = mean|∇|）

每一行都过**两道可观测性断言**：① SPIR-V 产物字节相对 HEAD 变了 ② `libffx_fsr3upscaler_vk.a`
与 `rgl` 二进制都比产物新。基线本轮**复跑得 6.014**（上一批 6.01）⟹ 判据可复现。

| 配置 | 边缘能量 | 相对无 FSR | 判什么 |
|---|---|---|---|
| 不接 FSR（参照） | 13.18 | 100% | — |
| **FIX-LUT：host `abs`→`fabsf`（一个 token）** | **13.72~13.90 ↑** | **104~105%** | **根因 = 修法** |
| **消融 C：绕开整个历史 gather** | **13.93 ↑** | **106%** | 根因（独立复现） |
| **消融 L：shader 核改闭式，不读 LUT** | **13.81 ↑** | **105%** | 根因（独立复现） |
| 消融 K：喂**精确** UV，保留 gather | 6.037 | 46% | UV/几何无罪，罪名在权重 |
| 基线（FSR 时域档） | 6.014 | 46% | — |
| 消融 I：α 强制 0.2（放大 9×） | 7.87 | 60% | α 是**放大器**不是源 |
| 消融 J：关去振铃 clamp | 6.016 | 46% | 排除 |
| FIX-E / FIX-E2：索引消抖 | 6.59 / 5.97 | 50% / 45% | 排除（见下） |
| 纯空间档（每帧 reset） | 10.93 | 83% | 上一批 |

**三条机制完全不同的修法各自独立地收敛到 13.8–13.9**（绕开 gather 13.93 / shader 侧闭式核 13.81 /
host 侧一个 token 13.90），且都**超过**了不接 FSR 的 13.18（RCAS 0.2 的净加分，与 §1.17"RCAS 是
净加分 +7%"一致）。另：消融 F（权重恒取中心纹素 `fColor11` = 9.18）与消融 C2（按
`floor(uv*size-0.5)` 直取 = 9.16）**在数学上本应等价、实测相差 0.02** ⟹ 测量链自洽。

### 三、推翻本批自己前两轮的**两条无效排除**（结构性无效，不是数值错）

- **「消融 D：`useLut=false` → 无变化 ⟹ LUT 排除」是错的。** VK 上 `SAMPLER_1D_0`（Reference
  `Lanczos2`）与 `SAMPLER_1D_1`（`Lanczos2LUT`）**都调用 `SampleLanczos2Weight`，也就是都读那张
  LUT 纹理**（`sample.h:134-138` + `:238-241`）。切 `useLut` 在 VK 上**换不掉 LUT**，这个实验
  在结构上不可能排除它。
- **「消融 E：关 `REPROJECT_SAMPLERS_USE_DATA_HALF` → 无变化 ⟹ half 路径排除」同理无效**——
  若该分支本就是运行时所选，"关掉它"就是空操作，"无变化"是必然结果。
- **通用纪律（本批最值得留的一条）**：一个消融的"无变化"**只有在先证明"这个开关确实能改变
  被测量"之后才构成排除**。否则它什么也没说。§1.17 已经写过"改完没先证明改动可观测"，本批
  是它的**进阶形态：不光要证明改动可观测，还要证明这个旋钮接在被测路径上**。

**由此打开的门**（这条事实本身仍有用）：唯一不读 LUT 的是 `SAMPLER_1D_2 = Lanczos2Approx`
（`sample.h:622-624`），而 `REPROJECT_USE_LANCZOS_TYPE` 在生成器里是 **1 bit**（`PERM_OPTIONS[0]`，
`total_keys = 1<<6`；host 侧 `FSR3UPSCALER_SHADER_PERMUTATION_USE_LANCZOS_TYPE = (1<<0)`）
⟹ **type 2 压根不在 41 个产物里**。但本批最终**没走"加排列"这条路**——真正的根因在 host 侧
（第六节），一个 token 就够，不必动 vendor shader 面。

### 四、正面排除清单（探针实测，勿重复排查）

| 嫌疑 | 判据 | 结论 |
|---|---|---|
| 速度场 / 抖动接线 | accumulate 内打点：`\|MV\|·UpscaleSize() > 0.25` 纹素的像素占比 = **0.0000** | **严格为零** |
| `fPxFrac` 有真分数偏移 | 二值阶梯：`frac.x>0.02`、`>0.9`、`>0.99` 占比**同为 72.48%**；`frac.y>0.02` 与 `>0.9` 同为 29.84% | **恰好双峰 {0.0, 1.0}**，核收到的是 delta 自变量 |
| 索引往返 `floor(((i+0.5)/S)·S−0.5)` 抖动 | 实测 68.3% 像素整型索引 ≠ `iPxHrPos`，**但 `frac` 恰为 1.0 补偿回来**；强行只改 `floor`（FIX-E）反而把配对拆坏 ⟹ 相邻帧差从 **0 变成 0.031**（时域不稳定） | **vendor 的 base/frac 配对自洽，不是 bug** |
| 去振铃 clamp（2×2 盒子偏向 +(0,0) 侧） | 消融 J 关掉 → 6.016 | 排除 |
| 第 0 帧种子残留 | warm 30 → 6.014，warm 400 → 6.023 | **真不动点** |
| 尺寸相关的 UV 舍入 | CPU float32 复算：S=1280 **0%**、S=720 5.6%、S=1920 1.4%、2 的幂全 0 | 真实存在但**是 GPU `fdiv`（`v_frcp`+牛顿，非正确舍入）与 CPU IEEE 的差异**，且被 frac 补偿 |

**顺带纠正 worker 合成靶台的一个方法学缺陷**：它选 256×256 测 `fPxFrac` 恒 0 并据此判"历史读取
几何恒等"——**256 是 2 的幂，`(i+0.5)/256·256` 精确，结构上看不见 1280（=2⁸×5）的舍入抖动**；
且它只跑 30 帧，自身 delta peak 0.53→0.019 仍在单调衰减**未收敛**。**几何这条它测对了结论
（确实无罪）但推理不成立**，本批的运行时探针才是有效证据。

### 五、两条探针方法学教训（本批各踩一次，都差点导致错结论）

1. **诊断不能写 `data.fHistoryColor`** —— 它经 `StoreInternalColorAndWeight` 写进的内部色
   **就是历史本身**。第一版探针因此把自己的被测收敛态永久清零，报出"**差恒为 0**"的**假阴性**，
   差点据此判"历史读取是恒等"。正确做法：**只写 `StoreUpscaledOutput`**，并把
   `dd.enableSharpening = false`（`fsr_upscaler_impl.cppm:362`）让"历史存储"与"输出"两条写分离
   —— 关 RCAS 时 accumulate 直接 `StoreUpscaledOutput`，输出图 == 历史图，正是读诊断的关键。
2. **渲染器对 FSR 输出做 tonemap + sRGB 编码**，**多电平编码不能线性反解**。第一版把 5 电平
   偏移编码按线性解码，把主峰读成"历史读取平移了 (+1,+1) 纹素"——**一个不存在的根因**。按秩
   聚类重解后主峰是偏移 **(0,0)**。**二值（0/255）编码在这条管线上无歧义，优先用它。**

### 六、根因是怎么挖出来的（**"内容 vs 绑定"这个问法本身是错的**）

先按"绑定错"查：`R16_SNORM → VK_FORMAT_R16_SNORM` 映射正确；`accumulate_pass.glsl:38` 确实定义了
`FSR3UPSCALER_BIND_SRV_LANCZOS_LUT 4`（排除"未绑定 → `#else return 0.f` → 权重全 0 → 0/0 = NaN"）；
`u = x/2` 对 128 宽 LUT 的纹素中心误差 < 半个纹素。**三条都对 ⟹ 一度以为矛盾无解。**

于是改用**运行时二值探针直接问 LUT 采样返回什么**（在 accumulate pass 里调 `SampleLanczos2Weight`，
只写 `StoreUpscaledOutput`）：

| x | 实测 SW(x) | 闭式 Lanczos2(x) |
|---|---|---|
| 0.0 | > 0.50 | 1.0 |
| 0.5 | > 0.70 | 0.573 |
| **1.0** | **∈ (0.40, 0.50]** | **0（零点！）** |
| 1.5 | ≤ 0.30 | −0.150 |

先立了两个假设（"LUT 正确"、"x 轴只覆盖 [0,1] ⟹ `SW(x)=L(x/2)`"），**三分歧判据跑出 (0,1,0)，
两个都被否证**。真正破局的是换问题：**别再问 GPU，这张 LUT 是 host CPU 侧的数组，一条 `fprintf`
就能把 128 个值打出来**（`FSRLUTDUMP` 门控，验完即删）。实测：

```
i=  0 x=0.00000 stored= 32767      i= 63 x=0.99213 stored=   167(修后)
i= 16 x=0.25197 stored= 32767  ←阶跃  i= 64 x=1.00787 stored=  -162
i= 32 x=0.50394 stored= 32767  ←阶跃  i= 96 x=1.51181 stored= -2014
i= 48 x=0.75591 stored= 32767  ←阶跃  i=127 x=2.00000 stored=     0
```

**i=0…63 全被钳成 1.0。** 用它反推上面四个探针读数：`SW(1.0)` = LUT[63] 与 LUT[64] 线性插值
= `(1.0 + (−0.0049))/2` = **0.4975** ∈ (0.40, 0.50] ✓；`SW(0.5)`=1.0 ✓；`SW(1.5)`≈−0.046 ✓；
`SW(0.0)`=1.0 ✓。**四个读数全部命中 ⟹ 诊断闭合，无残余假设。**

**定性为 `abs` 的名字解析**（不是 epsilon）：`FFX_EPSILON = 1e-06f`（`ffx_util.h:46`）排除了
"epsilon 本身是 1.0"这个竞争假设，而**转折点精确落在 `|value| = 1`** 正是整数截断的签名。
`<cmath>` 虽然包了（`:24`，注释还写着 "for fabs, abs"），但浮点重载只在 `std::` 里，全局作用域的
非限定 `abs(float)` 落到 `<stdlib.h>` 的 `::abs(int)`。

> **本批最值钱的一条方法学**：查"GPU 采样结果不对"时，先问**这份数据是在 CPU 还是 GPU、
> 能不能直接打出来**。本例数据从头到尾就是一个 host `int16_t` 数组，一条 `fprintf` 顶过
> 三条 GPU 侧推断；而我前两条路（查 format、查绑定、立映射假设）全部走空。

**红线可分析成立**：`lanczos2()` 全文件**只有一个调用点**（`:546`，LUT 生成循环），而该循环只在
FSR context 创建时跑 ⟹ FSR 关时根本不执行、逐位不变。**形式化的门关 SHA256 红线复验提交前补跑。**

### 七、修法落地状态与待办

**已落地并提交**（`a03d77c`）：`third_party/fsr/vendor/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp`
一处 —— `lanczos2()` 里 `abs(value)` → `fabsf(value)`。**补丁面 = 1 文件 / +1 −1，不动 vendor shader，
`fsr-blobs` 产物与 HEAD 逐位一致，`fsr-check` 护栏 OK（38 输入 / 41 产物）。**
配套护栏 = `632603a`（`just build` 退出码）+ `d76a4e6`（选择性 `-Werror`）。

**提交前必须补的三条**（提交时状态）：

1. ~~**红线复验**~~ ✅ **已做**：FSR 关截图 SHA256 = `fdece9d0…` = §1.14 登记基线，**逐位相同**，
   且是**跨会话**仍与登记值一致（比"同会话双构建"更强）。
2. ~~**真实用法验收**~~ ✅ **已做 = 通过（在 §1.19 修好申报 Y 符号之后）**：本批全部数字是
   **render_scale=1.0 + 抖动关** 的**诊断档**，而 FSR 的设计点是 `render_scale < 1` + 抖动开。
   到设计点验收的结论与判据**都在 §1.19**——那一批同时推翻了本条最初的"不通过"读数，**根因有两层**：
   ① 真有 bug（申报给 FSR 的是投影注入量，FSR 要的是内容位移，Y 反号）；② **判据本身在升频区间
   方向是反的**（`mean|∇|` 把块状伪影排在正确重建之上），且最初量在 1:1 = **非设计点**。
   修复后 0.5 档交付档**两轴同时占优**（比双线性更锐、且更接近原生参照）⟹ 通过。
   （时域档测量**不可并行**：`dd.frameTimeDelta` 是墙钟量。）
3. ~~**记 PATCHES.md**~~ ✅ **已做**：P14 条目 + §P14 配套护栏节。**同类面已扫完**：vendor host
   代码（`src/components` / `src/backends/vk` / `src/shared` / `include/FidelityFX/host`）里
   非限定 `abs(` 在本处修好后 **0 命中** ⟹ 这是唯一一处，不存在同族待爆雷。

**顺带结案的两条旧账**：① §1.17 的 σ≈0.8 指纹**已由本批解释**（[¼½¼]⊗[¼½¼]，σ=0.707）；
② worker 合成靶台报的 `useLut` 探测口径矛盾**升级后已结案**——`useLut` 正是选 reproject 核的那一位，
它的工具推导 `false` 而渲染器实测 `true`，**两边跑的是不同的核**，这解释了靶台为何看不见病灶。

### 八、判据载体（**一次性，不进仓库**）

`/tmp/opencode/fsrlab.py`（渲染器侧 shotburst 实验台：铺一次性 `RGL_CONFIG_DIR` → headless +
`diagnostics.rpc=true` → 一次连拍 warm+n 帧 → numpy 算 mean|∇| 与相邻帧差）、`/tmp/opencode/abl.sh`
（消融闭环 = 重排 → 编译 → 两道可观测性断言 → 跑 → 指标）、`/tmp/opencode/b.sh`（带断言的 build
包装）。**符合工具语言纪律**：留在 Python 的只用 stdlib + PIL/numpy，且**不入库**；要入库的判据
另说。worker 的 `third_party/fsr/smoke/fsr_lab.cpp`（合成靶台）已在册，但**本批的结论不依赖它**
（见第四节末对其方法学缺陷的说明）。

### 九、护栏的**归宿裁决**：这一类是 GUARD，不是 TEST

LUT 是 host 生成的、直接决定时域锐度的常量，却没有任何护栏。**但"补个单测"在这里是错的**：
`lanczos2()` 是 vendor `.cpp` 里的 **`static` 函数，测试根本够不着**。唯一写得出来的单测是
"在测试里重算一遍闭式去比对期望"——那测的是**我的副本**，vendor 真退化了它**照样绿**，
**违反阳性锚点**（"这段代码从未生效，这条断言会绿吗"→ 会绿）。**假护栏比没有护栏更糟**，不写。

真正的防线 = **编译器**：clang **默认就发** `-Wabsolute-value`，P14 之所以能活一整个 phase，
缺的从来不是诊断，而是**两道把诊断咽下去的自伤**，两道都已修：

| 自伤 | 修法 |
|---|---|
| vendor target 整块 `-Wno-error`（原注释写"逐条静音、保留其余可见"，**实现其实没做逐条**） | 保留 `-Wno-error`，用 `-Werror=<名>` 把 bug 类逐条提升回来（表 = `FFX_BUG_CLASS_WARNINGS`） |
| **`just build` 吞编译退出码**——`subprocess.call` 返回值没人接，**编译失败也返回 0**（同文件 `config` 配方一直是对的，只有 `build` 漏） | 透传退出码 |

> **本批最该记住的一条**：第二条比第一条重要。**判据不拦住 = 判据不存在**。
> 我这几轮排查全程得自己写 `b.sh` 去 grep `error:` —— 那就是在给构建入口打补丁。
> 任何"让构建失败"的护栏，在这个漏洞之上都退化成"打印一行没人必须读的输出"，
> 而"打印了没人读"正是 P14 的失败模式本身。

护栏变异验证（双向）：`fabsf`→`abs` ⟹ `just build` rc=**1** 且点名 `[-Werror,-Wabsolute-value]`；
恢复 ⟹ rc=**0**。`just test` 228 用例 / 62803 断言全过。
**沉淀地址**：`GUARD: -Werror=absolute-value on ffx_fsr3upscaler_vk` + `just build` 退出码透传。

## 1.19 批 14.5 第七批：**交付档验收 = 通过**（根因 = 申报 Y 反号；判据换轨）（2026-10-08，本机 STRIX_HALO）

§1.18 的**七、待办 ②**：全部数字是 `render_scale=1.0 + 抖动关` 的**诊断档**，而 FSR 是**抖动相机
升频器**，设计点 = `render_scale < 1` + 抖动开。本批到设计点验收，**结论 = 通过**（修复后）。

### 一、判据换轨（本批最贵的一条，先写它因为**它让其余结论全部改写**）

**`mean|∇|` 在「各臂块状程度不同」时与质量反相关。** 实测：0.5 档 edge **最高**的 `temp_nojit`
（11.72）读图是**块状锯齿最差**的一张，edge 7.29 的 `temp_jit` 反而最干净。机制直白——
双线性/无抖动的块状输出，**每个块边本身就是一处大梯度**，而正确的重建是**平滑**的。

它在 1:1 有效只是因为那时各臂都平滑、高频的**种类**相同；一旦跨臂出现"块状 vs 平滑"，梯度类
指标就把伪影排在细节之前。**推论：任何跨 `render_scale` 的梯度类读数比较一律无效**（§1.14 /
§1.17 / §1.18 里跨档比较的数字只当历史，别引用）。

**换成的判据 = 到「原生 1.0、不接 FSR」参照的 L1 距离**（参照是信息量上界，且不是任何升频器的
输出；各臂输出与参照同为 1280×720 ⟹ 逐像素 L1 有意义）。

⚠ **但总账早已否决过 SSIM/PSNR，理由就是「L1 对模糊守恒」**——L1 偏袒糊，单独用它不算数。
⟹ **本批的裁决一律要求「两轴同时占优」**：既比对照**更锐**（mean|∇| 高）**又更接近参照**（L1 低）。
两个判据互相制衡，被同时支配的结论才收。

### 二、根因：申报给 FSR 的是**投影注入量**，而 FSR 要的是**内容位移**（Y 反号）

`accumulate.h:110` `fLrUvJittered = fHrUv + Jitter()/RenderSize()`（另 `upsample.h:307`、
`common.h:298`）——FSR 全部消费者把 `jitterOffset` 当**渲染内容在纹理空间的实际平移**用。
而本引擎的注入符号**与官方伪码相反**：`applyProjectionJitter` 给 `proj[2][1]` **减** `2jy/H`
（官方是加），`clip.w = −v.z` 再翻一次 ⟹ `Δndc = (+2jx/W, **+**2jy/H)`；叠加**负高视口**
⟹ **内容位移 `c = (jx, −jy)`** = `taa_jitter.cppm::contentShiftPx`。我们申报的是原始 `(jx, jy)`。

申报错 ⟹ 每帧贡献按**不同**的错位落进基空间 ⟹ 混合即糊；**相位冻结 ⟹ 错位恒定 ⟹ 混合自洽**。
这个形态预测被实测命中（§三 的冻结臂）。

**修复**：申报点改走 `contentShiftPx(list.fsrJitterPx)`，并把 `FsrFrameInput::jitterX/Y`
**改名**为 `contentShiftX/Y`——**名字本身就是护栏**：这个 bug 的成因就是「raw jitter」与
「content shift」两个量在接口上同名。`GUARD: FsrFrameInput::contentShiftX/Y 命名`。
口径与地面真值数字写进字段注释 + `docs/FSR输入契约.md` §3。

### 三、验收数字（修复后；warm=150 + n=6，`/shotburst` 连拍）

| scale | 臂 | mean\|∇\| | L1 到原生参照 | 读图 |
|---|---|---|---|---|
| **0.5**（交付区间） | **temp_jit（交付档）** | **7.286** | **4.514** | **屋脊平直、拱形干净、立面连贯** |
| | spatial | 6.576 | 5.615 | 块状 |
| | off（双线性） | 6.774 | 5.812 | 块状、锯齿 |
| | temp_nojit | 11.719 | 6.776 | **最差**（硬块 + 锯齿） |
| **1.0**（非设计点） | off | 13.18 | 0（=参照本身） | — |
| | spatial | 10.93 | 1.166 | |
| | temp_nojit | 15.34 | 1.990 | |
| | temp_jit | 11.462 | 2.977 | |

**0.5 档交付档两轴同时占优**：比双线性更锐（7.286 > 6.774）**且**更接近参照（4.514 < 5.812）；
比纯空间档也是两轴同时占优 ⟹ **验收通过**。1:1 档所有 FSR 臂都不如不接 —— **这正是预期**，
1:1 没有东西可升频，重建滤波只会丢信息，**1:1 不是 FSR 的设计点**。

修复量级：`temp_jit` 1:1 从 10.45 → **11.46**（+9.7%）、0.5 从 6.85 → **7.29**（+6.4%）。
独立佐证：`spat_jit` 的相邻帧差从 5.5/7.29 掉到 **3.0/3.36**（**残余相位相关性降一半**）⟹
EASU 的 un-jitter 开始起作用；修复前那个更高的边缘值是**错位振铃**，不是锐。

**冻结相位臂**（`RGL_FSR_JFREEZE`，1:1）：14.44 / 14.49（两种申报约定无差别，因为相位恒定时
两侧用同一个 J，误差自动抵消）。它的作用是**把病灶定位到「逐帧抖动差」**而非抖动本身。
⚠ 它**不能**用来判申报值对不对——恒定误差只是整体平移，梯度类指标对它近似平移不变。

### 四、地面真值仪器（申报值的裁决依据，**不再靠推**）

在**输出图**上量位移是无效仪器（契约「一个无效仪器的记录」）。正确观测点 = 回读 **`scene_hdr`**
（mesh pass 写完、`ao_apply`/FSR/tonemap **之前**，几何未经任何重建环节）。两档对拍：FSR 开 +
抖动关 vs FSR 开 + 抖动开 + 相位冻结。⚠ **FSR 不能关**：`env.fsrJitter = fsrUsable && fsrJitter`
（`main_scene.cppm:86`）⟹ 关 FSR 后抖动根本不注入（实测两图 md5 逐位相同）。

估计器 = **预滤波去走样 + Lucas-Kanade 闭式解**。仪器自检（缺了读数就不可信）：
**阳性锚点** = 傅里叶平移造人造位移 `(−0.125, +0.278)` 测回 `(−0.133, +0.295)`，残差 **0.019**；
**反向用例** = 同图自比 `(0.0000, 0.0000)`。⚠ **预滤波是判据成立的前提**：不滤波时同一锚点被
高估到 **1.55×**（走样破坏 LK 的亮度恒定假设）——自检当场拦下了这一版仪器。

冻结相位引擎实报 `jitterPx = (−0.125, −0.2778)`：

| 候选 | 期望 | 对实测 `c=(−0.0995, +0.2648)` 的残差 |
|---|---|---|
| `contentShiftPx` 声称 `(jx, −jy)` | (−0.125, **+0.278**) | **0.029** ✅ |
| 曾申报的原始 `(jx, jy)` | (−0.125, **−0.278**) | **0.543** ❌ |

残差差 **19 倍**，不是仪器精度能含混的量级。

**为什么"验过符号"还是漏了**：此前两次"验证"都在**输出图**上做（契约自己标为无效仪器的那条路），
所以验不出。而 `docs/FSR输入契约.md` §3 与 `frame_draw_list.cppm` 的注释**两份权威文档都在
从官方伪码往下推**，一处推错、另一处照抄 ⟹ **文档一致性不构成正确性**，交叉印证必须有一路是
**实测**，且观测点要在被测环节**之前**。

### 五、正面排除清单（每条都实测，勿重复排查）

| 嫌疑 | 判据 | 结论 |
|---|---|---|
| **申报 Y 符号** | 回读 `scene_hdr` + LK 地面真值（§四） | ✗ **就是它**（残差 0.029 vs 0.543） |
| 申报单位（渲染像素 vs 窗口像素） | 注入端除的是 `RenderExtent`（`ecs_impl.cppm:414`） | ✓ 对。⚠ 1:1 时两者相等，**这类错在 1:1 天然不可见** |
| 申报幅度倍数（注入是否被做两次） | 申报幅度扫 ×0.5/×1/×2（两轴） | ✗ 不是。最优不在 ×2 ⟹ 注入没被做两次 |
| FSR 输出→tonemap 缺执行依赖（竞态） | 补 `pipelineBarrier2` 后重测：6.778→6.849 | ✗ 不是病灶。**但 barrier 该留**（缺执行依赖是 UB，与"是不是病灶"无关） |
| 深度标志 `DEPTH_INVERTED` / `_DEPTH_INFINITE` | 深度是标准 LEQUAL 非反向；near/far 是 `SceneBounds` 自适应紧区间（0.386/2.711） | ✓ 两个都不设是对的 |
| 抖动相位数 | 实机日志「相位 = 8·(上屏/渲染)²」= 契约公式 | ✓ 对 |
| MV / `JITTER_CANCELLATION` 配对 | 开着更差（10.51→8.74）+ debug view 顶左格恒 0 | ✓ **速度场确为基空间**，成对红线首次拿到实测背书 |
| `dd.reset` 被强制 true | `in.reset = !fsrDispatchedOnce`（只首帧） | ✓ 历史确实在用 |
| Lanczos LUT | §1.18 已修 | ✓ 已修，本批在其之上 |
| AO 基空间错位（契约 §4.6 第 3 条） | 旧排除（strength 0.5→0：10.45→10.55）**做在主误差遮蔽下**，不构成排除 | ⬜ **待重做**（申报修好后重测） |

### 六、遗留：**TAA 与 FSR 时域互斥，没有任何护栏**（结构缺陷，未修）

色输入选择 = `ao_applied > taa_out > scene_hdr`，判据是**"哪个 pass 恰好在图里"**；而申报给 FSR
的抖动是**另一条独立链路**算的。两条链路无耦合、无断言。三个分支里 `taa_out` 自批 13.5 起**已在
基空间**（已 un-jitter），而申报仍报 `≠0` ⟹ **FSR 再 un-jitter 一次**。当前五臂 TAA 全关
（日志只有 `ao_applied`、渲染图无 taa pass），**所以它不是本批根因**，但它是同一个结构缺陷的
下一个出口：**"这张图在哪个空间"这件事没有主人**，VVL 不报、vendor 零校验、症状只是糊。

更深一层：FSR3-upscaler 自己就是带累积的时域升频器，接在 TAA 后面**就算抖动簿记全对也是双重
时域滤波 = 双重模糊**。所以正确的护栏不是"TAA 在时申报清零"，而是**两条都已落地**：

1. **硬门** = `main_scene.cppm::resolveFsrTemporal`：`fsrUsable && fsrTemporal && taaDecorated`
   → warn + 强制 `fsrTemporal=false`。判据取 `taaDecorated`（**实际图形态**）而非配置键，
   与 AO 施加点 / `fsrUsable` 同源。这不是可调画质项，是互斥关系；让它可配就是等着被人配出
   "画面糊了"再当调参问题去调。
2. **申报与空间绑死** = `Renderer::fsrColorBaseSpace`（`initFsr` 选中 `taa_out` 时为真）：
   申报值**由"选中的色输入处于哪个空间"推导**，而不是独立算一遍。这一条同时封掉 `taa_out`
   分支和将来任何新增的"已 un-jitter 的中间图"分支——把不变量从"三处注释各写各的"变成
   "只有一个来源"。

**两条的判据**（`/tmp/opencode/guard1.py`，阳性锚点 + 反向用例都过）：

| | TAA 开 + `fsr_temporal` | TAA 关 + `fsr_temporal` |
|---|---|---|
| warn | **出现** ✓ | 不出现 ✓（否则护栏恒真） |
| TAA 在图里 | 是 ✓（`taa (RGBA16F)` RT） | 否 ✓（否则反向用例前提不成立） |
| 实报 `contentShift` | **(0, 0)** ✓（基空间，护栏 2 生效） | **(0, 0.1667)** ✓（非零，否则护栏 2 恒真） |

### 七、判据载体（**一次性，不进仓库**）

`/tmp/opencode/matrix.py`（五臂矩阵 + 三道断言）、`l1rank.py`（L1 到原生参照 + 两轴一致性对账）、
`gt_shift.py`（地面真值仪器，含阳性锚点与反向用例）、`sweep.py`（申报幅度/冻结相位扫描）、
`cmpcrop.py`（裁区放大并排供读图）、`dvpanels.py`（debug view 逐格量化）、`fsrlab.py`（沿用 §1.18）。
`edgeprof.py` **已判废**（噪点纹理上估计器量到的就是纹理本身，噪声 ≈ 信号）。

> ⚠ **两条必踩的基础档坑**：`runtime.headless` 默认 **false**（= 开真窗，尺寸随桌面 ⟹ 分辨率
> 不可比）、`diagnostics.rpc` 默认 **false**。矩阵里已把这两条做成 `BASE` 常量强制铺上。
> 另：`fsrlab.py` 的 `metrics` 子命令**不接** `--skip`（`run` 自己已按 warm 跳过并打印 JSON）。

**临时探针已全部撤除**（三个文件 `grep -c RGL_FSR` = 0）。要复现本批的两个关键观测，加法是：

- **debug view**：在 `fsr_upscaler_impl.cppm` 的 `dd.jitterOffset` 之后加
  `if (getenv("RGL_FSR_DEBUG_VIEW")) dd.flags |= FFX_FSR3UPSCALER_DISPATCH_DRAW_DEBUG_VIEW;`
- **冻结抖动相位**（**本批定位病灶的关键仪器**：它把问题从"抖动本身"切到"逐帧抖动差"）：
  在 `ecs_impl.cppm::taaSystem` 里把 `taaJitterPixels(taa.frameIndex, 1.0f, …)` 的帧号换成
  常量 5（双轴皆非零），并用 env 门控。相位恒定 ⟹ 抖动差 `prev − cur` 恒 0 ⟹ 两条检测路径
  两侧用同一个 J、误差自动抵消，**只剩申报的绝对值在起作用**。

**别把这两条做成常驻诊断门**——它们没有判据，只是眼睛。

> debug view 六格语义（本批逐格量化过，别再靠缩略图猜）：底行左 = disocclusion（交付档实测
> **恒 0.000**）、底行**中** = **SHADING_CHANGE**（函数名 `drawReactiveness` 误导人）、底行右 =
> detail-protect takedown。**右格在 MV≡0 时恒 0，才是"速度场是否真为 0"的判据**；底行左/中两格
> 对抖动输入**数学上必然点亮**（`shading_change_pyramid.h:123` 与 `:148-149` 两侧各加**本帧自己的**
> jitter），所以它们**不是**"历史被拿下"的证据。覆盖率实测仅 1.7%~3.3%，量级上也撑不起全局损失。


## 1.20 批 14.6（配置面收口）：抗锯齿从**三个布尔**收成**一个三态键**（2026-10-09，本机 STRIX_HALO）

> **编号撞车已结案**：本节「批 14.6」= 代码注释里已写死的批次号（18 个文件 55 处），与 §2 表里
> 那行「判据迁移」是两件事（那行未开工）。裁决 = **让号给已落地的一方**：把表里那行未开工的改成
> **14.9**，代码注释一字不动。理由是成本不对称——挪表一行改一处，挪注释改 55 处且每处都可能碰
> 到别的批次史。
>
> 🔴 **一条本批的自查账（护栏要有人跑才算护栏）**：上一批给判据注册表加了两条判据，`selftest`
> 里写死的「判据总数」没抬 ⟹ `imgqa selftest` **在 HEAD 上就是红的**，而提交前跑的是
> `just build-tools`——**它不跑 selftest**，所以没人听见。本批已把 `imgqa selftest` 接进
> `build-tools`（不要设备、毫秒级、退出码 `c.bad ? 1 : 0` 可直接当闸门）。教训不是"下次记得跑"，
> 而是：**凡是只在某个工具里自检的护栏，必须检查"谁在自动跑它"**；没人跑的红线等于没有红线。
> 同族先例 = 债-68「失效必须出声」——出声的前提是有人在听的那个位置放着耳朵。

**动机不是洁癖。** 「谁做抗锯齿」原先由三枚布尔表达，且分家在两个文件：`scene.lua taa.enabled`
× `runtime.lua fsr_upscaler` × `fsr_temporal`。三布尔有 8 种写法、只有 3 种有意义，其余靠运行期
守卫拒绝或**静默挑一个**（`main_scene.cppm` 那条 "fsr_temporal 与 TAA 互斥" 的 warn 就是给
「两个都开然后引擎静默关掉一个」打的补丁）。这与 §1.19 的根因同族：**接口允许写出的组合多于
语义合法的组合**，写配置的人不知道自己跑在哪一档。

**改完的形状**（渲染代码零改动，只动配置解析 + 两处接线 + 工具）：

- `runtime.lua aa = "off" | "fsr" | "taa"` → `AaMode` + 两个唯一推导点 `aaWantsTaa()/aaWantsFsr()`；
  未知值 **fail-fast**（`present_mode` 同纪律：静默回落的失效形态 = 你以为开着抗锯齿在量图）。
- FSR 调参收进嵌套块 `fsr = { temporal, jitter }`（出厂 true/true = 设计点）。**无扁平键双读兼容**
  ——两个真相源是本项目反复登记的反模式。
- `scene.lua taa.enabled` 与 `SceneConfig::taaEnabled` **删除**，`taa` 块从此只剩调参。
- `velocity_field` 保持**独立键**（批 14.4 的逐像素重投影线是它的非 FSR 消费者），但 `aa = "fsr"`
  在**解析期隐含打开**它，唯一发生点 = `applyAaKey` 内一行 ⟹ `velocityField` 仍是单一真相源、
  消费侧对隐含关系无感知；`off/taa` 态不碰它 ⟹ 不会凭空冒出 velocity pass。

### 红线：四条截图逐位复现（`just shot --frames 8`，基底 = 仓库 `configs/`）

| 档 | 命令尾串 | SHA256 前 8 |
|---|---|---|
| `aa = off` | `--aa off` | `fdece9d0` |
| `aa = taa` | `--aa taa` | `360ce0d2` |
| `aa = fsr` 纯空间 @0.75 | `--aa fsr --render-scale 0.75` | `866c0e6b` |
| `aa = fsr` 时域+抖动 @0.75 | `--aa fsr --fsr-temporal --fsr-jitter --render-scale 0.75` | `4d2c21e9` |

四条在**动引擎前**用旧 binary + 旧工具复现一次，改完后用新 binary + 新工具再复现一次，逐位相同
（串行跑，起跑前 `pgrep -cx rgl` = 0）。其余闸门：`just test` 230 例 / **62839** 断言全绿（基线
62803，新增 aa 与 fsr 块两条用例 +36）· `just smoke` PASS errors=0 warnings=0 · style-check
.cppm 0 警告 · doc-check 0 警告 · imgqa 工具侧 69 警告（与基线持平）· `imgqa selftest` 0 失败。

**变异（新增断言逐条验红，形态 = 改坏被测实现而不是改测试）**：删 `cfg.velocityField = true`
一行 → 两条隐含断言红 · 未知值改成静默回落 off → fail-fast 断言红 · `off` 支写错枚举成员 →
三值命中断言红 · `fsr` 块内键名打错 → 键名契约断言红 · 出厂默认 `aa` / `fsrTemporal` 漂移 →
默认画像断言红。`TEST: tests/test_scene_config.cppm`（runtime_config 段两条新用例）。

### 教训（就近留，能落断言的已落）

1. **`tools/imgqa/engine.cpp` 的 `replaceValue` 是无锚点子串搜索** ⟹ `configs/*.lua` 的**注释里
   不许出现 `aa = ` / `temporal = ` / `jitter = ` 这种「键 + 空格 + =」子串**，否则交付配置模式的
   覆写打在注释上（静默改不到真键）。`smoke.py::patch_lua` 是**行锚定**的，注释安全——两个工具
   口径不同，不许互相推断。`GUARD: 待补`（可做成 checks 规则：configs 注释里禁止出现解析器认识
   的键形状），本轮只靠人工核对「每个键形状恰好出现一次」。
2. **枚举比守卫强**：`aa` 单槽位让「TAA 与 FSR 时域同开」**写不出来**，imgqa 侧因此删掉两条运行期
   守卫（`checkFsrOpts` 三条 → 一条）。拒绝非法组合不如让它不可表示。
3. **隐含依赖只准落在解析期一行**：若改成消费侧各判 `aa == fsr || velocityField`，就是第二个
   真相源（且 `velocity_field` 的独立消费者会被牵连）。
4. **工具默认与引擎默认相反时必须显式写键**：出厂 `fsr` 块 = true/true（设计点），而工具想量的
   第一条臂是**纯空间**（false/false）。「默认不写键」的纪律只适用于**引擎默认 = 工具默认**的键，
   两者相反时不写键 = 静默换臂 ⟹ `fsrKeyValues` 在 `aa == Fsr` 时**总是**显式写两枚子键。
5. **报错文本的形状不是证据**：argparse 把多个 leftover 参数**用空格拼成一条**报
   （`unrecognized arguments: --set runtime.lua aa = off`），本轮据此误判 `just` 的 `*ARGS` 不做
   词分割，并顺着这个误判编出一条根本不存在的 `--set` 旗标去"验收"。最小复现（临时 justfile 打印
   `sys.argv`）一步就推翻它：`just shot --frames 8 --name x --aa off` 一直是好的。

### 残留（登记，不在本批做）

- 引擎日志锚点仍写「配置键 `fsr_temporal` = …」（`smoke.py` 按这条文本 grep，改名要两边同步）。
- imgqa 的 CLI 仍叫 `--fsr`（选臂开关），只有 `smoke.py` 换成了 `--aa`。两侧不同名是**已知**的，
  统一到 `--aa` 要连 `cli_table.cpp` 的默认值回显一起做。

## 1.21 批 14.4b（步 0–4 收账）：恒假护栏的成因、永久/临时 sandwich 的分界、syncval 查证（2026-10-09，本机 STRIX_HALO）

> 本节 = 批 14.4b 的收口记录（改动本体见 `629568c` / `396b959`，数字修正见 `6ee43e2`）。
> 这里只留**可复用的结论**；被实测推翻的表述就地删，不留双层。债的终态 → `docs/还债总账.md` 债-72 / 债-75。

### 一、恒假护栏是怎么长出来的：断言读的是「别人写的记录」，不是被断言的那个事实

FSR 的 velocity 护栏（改前）：转换块把 barrier 的 `newLayout` 与写进 `attachmentLayouts[slot]` 的值写成
**两个独立字面量**，而护栏读**表**与常量比对 ⟹ 它做的是「表 vs 常量」的**同义反复**；转换块无论走不走
早退都已把该槽写成申报布局 ⟹ **断言恒假**。

镜像探针实测（步 0）：改 barrier 的 `newLayout` → 护栏**不响**（只有 VVL 在 vendor 使用处报，归因成本高）；
改表 → 护栏响。⟹ 它检的从来不是"barrier 转对了吗"，而是"我上一行写进表的数是不是我上一行写的那个数"。

**教训的通用形态**：断言的输入必须来自**被断言的那个事实本身**；读一份「别人写的记录」——尤其是同一段
代码上一行刚写下的记录——不算。

⚠ **同一条纪律在同一批里第二次踩**：这机关正是 §1.11 为 IZB 那条护栏写下的（原话"返回值由发命令那一处
产出……若让 `recordFrame` 自己写个局部量记布局，那个量与 barrier 的 `to` 参数就是两个独立事实"），
却在另一个模块被违反。**为什么写了说明还会踩** = §1.11 把它写成了"IZB 那条护栏**为什么有效**的说明"，
而不是"**任何**护栏怎么才算有效的判据"——说明是回顾性的，不会主动去拦新写的代码。
⟹ 能落成防线的只有两样：`Renderer::transitionAndTrack`（把「发 barrier + 写表 + 返回实际写入布局」收成
一次调用，"转成 A 却记成 B"在写法上不可能）+ `tests/test_layout_drift.cppm`（判定与消息的常驻阳性锚点）。
**覆盖面边界已写进测试头注释**：它测判定与消息，**不测调用点还在不在**——调用点被删它照样全绿。

### 二、永久 vs 临时 sandwich 的分界（本批最有复用价值的一条）

| | 谁写 `Renderer::attachmentLayouts` | 护栏靠什么 | 现有用户 |
|---|---|---|---|
| **永久转换**（转完停在那儿，下帧继续当 `oldLayout`） | **必须写表**，且必须由发 barrier 那一处写 ⟹ `Renderer::transitionAndTrack` | **结构**：barrier 的 `newLayout` 与写表的值是同一个局部量 ⟹「转成 A 却记成 B」写不出来 | `Renderer::fsrTonemapPassPre` 的 velocity |
| **临时 sandwich**（进出同值，图追踪表不可见） | **必须不写表** | **断言**：`IzbShadow::recordFrame` 返回由**实际发出的** barrier 推进出的布局，调用方拿它跟表对账 | `IzbShadow::recordFrame` 的深度图 |

**临时的一律不得用 `transitionAndTrack`**：一旦它写表，表就自洽 ⟹ 那条漂移护栏永远为真 = 又一个假护栏。
⟹ 这里"复用同一个原语"是**错的方向**——两类 sandwich 对表的要求恰好相反。分界写进了原语的声明处注释。

### 三、步 0 变异实测的两条反直觉结果

1. **删掉还原 barrier（变异 2a）也响**（进入=3 / 离开=5）。反直觉在于"删掉还原 = 返回值与表都停在 SRO，
   应当自洽"——漏了**返回值链自己就漂**：删掉后 `depth` 停在进入 barrier 的返回值 SRO，与追踪表的 DSO
   不等 ⟹ 护栏响。⟹ 这条护栏的强度来自"返回值 = 实际发出的 barrier 推进的链"，**不是**来自"进出同值"。
   还原目标写错（变异 2b）也响（离开=2）。
2. **两个变异下画面逐位不变**（红线 A `fdece9d0…` 精确复现）⟹ **本机无像素症状，只有日志能抓**。
   这正是债-32 的形态（约定成立时一切正常）。⟹ 在这类问题上，"改完截图没变所以没事"**不提供信息**。
3. 顺带修掉一个诊断缺陷：IZB 那条消息的三个占位符里「进入」与「追踪表」是**同一个变量恒等**——读者以为
   拿到两个独立事实，其实只有一个。`tests/test_layout_drift.cppm` 把"消息里恰好两个数字串"钉成判据防复发。

### 四、syncval 查证：「buffer 侧缺 barrier，VVL 抓不到」在本项目成立到什么程度

四种预期结果（开→报/不开→不报 ｜ 两边都报 ｜ 两边都不报 ｜ 未做成）里，实测**同时命中两种**：
**开关本身确实生效**（下面第 2 条），但**对本批那两个变异是「两边都不报」**（第 3 条）。逐条：

1. **本项目 VVL 默认不开 syncval**（不是推断，是本机层清单自述）：
   `/usr/share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json` 里 `validate_sync` 的
   `default = false`（同表 `validate_core` / `check_image_layout` / `thread_safety` = true）。
   `vkinstance.cppm` 只 `setPEnabledLayerNames({"VK_LAYER_KHRONOS_validation"})`、无 `vk_layer_settings`
   ⟹ 吃默认值。
2. **开关能生效**（阳性锚点不靠变异）：`VK_LAYER_VALIDATE_SYNC=1` 下，**未变异的现行代码**报出
   **3 类 10 条 hazard**（+1 条 duplicate_message_limit 提示 = smoke 计数 11），而同一份代码不开 syncval 时
   `errors=0` ⟹ 开关打开的是一个平时**完全不存在**的子系统。
   ⚠ **上面这 11 条是当日读数，同日已全部修至 0**（`053e38a`：`usageInfo(depthWrite)` 的 stage 漏
   `LATE_FRAGMENT_TESTS` ×5 / scene data 单份跨帧共享 buffer 两个写者零依赖 ×4 / present 转换看不见
   图外捕获段的 Transfer 读 ×1 / 提示 ×1），并把 syncval 做成了常驻闸门 `just smoke --sync`
   （带阳性锚点与 `no_vvl` 生效核对，`db2fe38`）。**本节下面那条「VVL 抓不到」的结论不受影响** ——
   修掉的是 API 轴那批，shader 轴那条支路一条没动。
3. **但对本批指定的变异（删 `IzbShadow::recordFrame` 里 FillBuffer→compute 那条 buffer barrier），
   开与不开都不报**：

   | 环境 | 结果 |
   |---|---|
   | 不开 syncval（现状配置） | `just smoke` **PASS errors=0 warnings=0** |
   | 开 syncval | 11 条，与**未变异**代码**逐条同集**（5 深度图 WAW + 4 CopyBuffer↔CopyBuffer WAW + 1 CopyImageToBuffer WAR + 1 提示），**没有一条**提到 `vkCmdFillBuffer` / dispatch / izb 的 buffer |

   ⟹ **「buffer 侧缺 barrier，VVL 抓不到」这句话在本项目成立，且开了 syncval 也不改变。**
4. **但它不是普遍真理**——同一个 syncval 在现行代码上抓到了一条 **API 级 buffer 竞争**
   （`vkQueueSubmit(): WRITE_AFTER_WRITE … vkCmdCopyBuffer … previously written by another vkCmdCopyBuffer`，
   4 次）。⟹ 不对称的轴是**访问由谁发出**：**CPU 侧 API 命令之间**的竞争（实测样本 = CopyBuffer↔CopyBuffer）
   syncval 看得见；**只要有一端是 shader 经 descriptor heap 发出的访问，它就看不见**（两个变异都是这一类，
   均 0 条）。
   - **机制假设（未证）**：本项目走 shader object + 应用自写 descriptor heap，VVL 无从把 dispatch 的访问
     归到具体 `VkBuffer`。旁证 = 本机层清单里 `descriptor_hashing` 的自述（"When enabled and using
     VK_EXT_descriptor_heap/… this will attempt to track all descriptors seen such that **GPU-AV and GPU
     Dump** can detect more errors"，默认 false，且只提 GPU-AV/GPU-Dump，不提 syncval）。
   - **什么实验推翻它**：开 `descriptor_hashing`（或改走常规 descriptor set）后重跑变异 2，若报出
     `SYNC-HAZARD-READ-AFTER-WRITE` ⟹ 假设成立但可绕；仍不报 ⟹ 换解释（例如 syncval 不建模 compute↔compute
     的 storage buffer 竞争）。
   - **仍然要做的部分**（别把作用域读宽）：本条否的是"VVL 能兜住 buffer 侧同步"，**不否**"该写的 barrier
     照样得写"，也**不否**"将来值得开一次 syncval 专抓 API 级竞争"——它已经抓到一条现行存在的。
5. **第二个变异**（删 create 写 → prime 读那条 compute↔compute 的 head buffer barrier）**同样开与不开都
   0 条**。加它只为排除"FillBuffer 是特例"这个竞争解释，不是为了多拿一个数。

**方法（两条坑先记，下次别再撞）**

- **任务里给的开关写法在本机无效**：`VK_LAYER_ENABLES=VK_ENABLE_SYNCHRONIZATION_VALIDATION` →
  `PASS errors=0`（**no-op**，那个值名在本机清单里不存在）。有效的两种：`VK_LAYER_VALIDATE_SYNC=1`（现代）
  与 `VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT`（清单里仍在、标
  DEPRECATED）。⟹ **开关名查本机清单，别抄 SDK 文档。**
 - ✅ **一开 syncval 引擎必崩**（本批发现**并已修**，2026-10-09）：VVL 回调 `vulkanDebugCallback`
   （`rgl/utils.cppm`）曾把 `pMessage` 直接当 **fmt 格式串**——`RLLogger::log` 里的 `fmt::runtime`
   只绕开编译期检查、运行时照旧解析花括号——而 syncval 的消息体带 `pImageMemoryBarriers[6]: {`
   ⟹ `std::runtime_error("invalid format string")` 直接 terminate（实测崩在**第一条** syncval
   消息：`what(): invalid format string [Vulkan-Validation(0)]`）。**任何含花括号的 VVL 消息都会
   崩进程**，只是 core validation 的消息恰好没带过花括号。修法 = 消息改当**参数**：
   `log(lvl, "{}", pMessage)`。同批清掉**同病根**的 4 处 `critical(errMsg)`
   （`sdl3window.cppm` ×3 / `QueueFamilyManager.cppm` ×1；那里的 `errMsg` 是**已格式化**的字符串，
   再被二次当格式串解析，SDL 错误消息带花括号即炸）。
   **复验判据（阳性锚点）**：`VK_LAYER_VALIDATE_SYNC=1 just smoke` 修前 terminate、修后跑完并完整
   打出当时的 11 条 hazard（`pImageMemoryBarriers[6]: {` 那条多行消息逐字段落盘 = 崩溃触发点本身；
   那 11 条同日已修至 0，见上面第 2 条）；
   不开 syncval 的常规路径 `PASS errors=0`、截图逐位不变、`just test` 全绿。
   ⚠ 本条**未硬凑单测**：回调体没有可断言的逻辑，能断言的只有「含花括号的消息不炸」，而那要真跑
   VVL 才量得到 ⟹ 验收走上面的端到端判据（还债总账 §3「给写不出断言的东西硬凑测试」）。
- 读消息原文的**零重建**办法（不改产品代码时用得上）：binary 带 debug_info，
  `lldb -b -o "breakpoint set --name RGL::vulkanDebugCallback" -o "breakpoint command add -F …"`；参数在
  `-O2` 下无 DWARF 位置（`frame variable` 为空），按 SysV ABI 从 `$rdx` 读
  `VkDebugUtilsMessengerCallbackDataEXT`（`pMessageIdName` 偏移 24、`pMessage` 40）。

**现行代码 syncval 报的 3 类 hazard（本批不裁决是否真 UB，只登记不重查）**

| 类 | 条数 | 原文要点 |
|---|---|---|
| 深度图 WAW | 5 | 图布局转换的 `srcStageMask = EARLY_FRAGMENT_TESTS`，未覆盖 render pass 结束时 attachment storeOp 的深度写（VVL 建议含 `LATE_FRAGMENT_TESTS`） |
| CopyBuffer↔CopyBuffer WAW | 4 | 同 queue 上两个 `vkCmdCopyBuffer` 写同一 `VkBuffer`，中间无依赖（**buffer 侧、API 级**——正是 syncval 唯一看得见的那一类） |
| CopyImageToBuffer WAR | 1 | 图布局转换未等此前 `vkCmdCopyImageToBuffer` 的读 |

### 五、§1.11 那句「FSR 是第二个」的后续

§1.11 写「A 档护栏的**现状作用域 = IZB 一处**；14.5 的 FSR 是**第二个**」。后续：FSR 确实接了，
**但接上的那一条是恒假的**（本节一），本批删掉它、velocity 改由结构保证。**真正活着的两处** =
`Renderer::izbLightPassPre` 的深度 sandwich 返回值对账 + `Renderer::fsrTonemapPassPre` 的**色输入 / 深度**
两支（后者防的是另一件事：图把附件留在非申报布局就进 FSR，职责一字未动）。
⟹ 以后引用"A 档覆盖两处 sandwich"时，两处的**含义**与本批前不同，以这两个函数现码为准。

### 六、B 档否决（裁决结论，理由与触发条件在账上）

三条理由 + 触发条件已进 `docs/还债总账.md` §3 明确不做表（此处不重述以免成第三副本）。
一句话版：**B 档不是 A 档的升级，是拿编译期声明换掉运行期断言**——它的判据「删掉 `depthLayoutBarrier`」
恰好拆掉 A 档护栏的断言输入（barrier 不由模块发，模块就无从返回真实布局）。

### 七、判据与沉淀地址

`just build` style-check .cppm **0 警告** / doc-check **0 警告**（工具侧 69 = 基线持平）·
`just test` **241 例 / 67353 断言**全绿 · 红线 A `just shot --name s4-redline` = `fdece9d0…07ff7`
逐位复现 · `just smoke` 与 `just smoke --aa fsr` 均 **PASS errors=0 warnings=0**（FSR 门真开：日志含 FSRDIAG）。

- `TEST: tests/test_layout_drift.cppm`（判定 + 值可读化；M1/M2 变异验过非空转）
- `GUARD: Renderer::transitionAndTrack`（永久转换的原子原语，"转成 A 却记成 B"写不出来）
- `GUARD: fsrInputLayout()`（向 vendor 申报的 state ↔ 布局的单一真相源，实现体紧挨 `srvState`）
- `DOC: 本节`（工具/硬件观察、决策记录、带失效条件的负知识——断言写不出来的那部分）

---

> 顺序按「失败最便宜」排，不是按依赖最紧排。**14.1 与 14.3 可并行**（14.3 不碰 FSR）。
> 每批收口 = 在本节划掉 + 把教训写进**本文**对应批次那节（教训就近写；被实测推翻的旧教训**直接删**，
> 不叠加"旧结论 + 更正"注记 —— 全局调试历史已于 2026-10-08 删除，写法见 AGENTS.md「教训写哪」）。

| 批 | 内容 | 为什么在这个位置 | 验收判据 |
|---|---|---|---|
| **14.0** | **TAA 冻结收口**（前置，别跳）：① 臂 13 转交付（`configs/scene.lua` `clip_mode = 13` + 重立红线 B + 六机位阶跃补跑）**或**把判据 ④ `temporal-tail-step-clip-holds` 显式降级为 advisory 并写明理由；② 债-67 默认语义拍板（推荐：imgqa 读 `scene.lua` 的 `clip_mode` 当默认）；③ `phase13_plan.md` §2 表逐条给三态终局；④ 还债总账把 TAA 线标**降级 + 触发条件** | **不转交付就必留一条常红判据**，而债-61 已诊断过这个反模式：一条永远红的闸门 = 没有闸门，且永久污染"全绿"这个信号。臂 13 静止侧逐位不变已被三台仪器独立确认 ⟹ 转交付风险≈0 | ① `imgqa selftest` 全绿**且无常红项**（rc 可当闸门）；② 红线 A 复现 `fdece9d0…`（TAA 关逐位不变）；③ 台账里 TAA 相关债**每条有终态**，无"看过一眼就算完" |
| ~~**14.1**~~ ✅ **已收口 2026-10-08** | **vendor 骨架 + 编译成功**：从 `c6efa6b` 捞 `sdk/{include,src/backends/vk,src/backends/shared,src/components}` 的 FSR3-upscaler 闭包 → 写 shader 排列生成器（glslang）→ 产物签入 + SHA1 护栏 → `ffx_vk.cpp` 在 Linux/clang 编过（`<codecvt>` 死分支、警告） | 用户方向第一条。**编译不过，后面全是空谈**；且这一步**不碰引擎**，失败可整棵删掉 | ① 独立 target（`third_party/fsr/`）**能编出静态库**，Linux clang + Windows MSVC 双平台 configure 通过 —— ⚠ **Windows 侧未验**（本机无 Windows；改动全在 `#ifndef _WIN32` 或新增 TU，是推理非实测，见 §1.8）；② SPIR-V 排列产物齐（**实际 80 个唯一模块**，非 40）、`just fsr-check` SHA1 护栏覆盖 —— ✅ 变异验过非空转；③ **交付路径逐位不变** —— ✅ `just smoke` PASS errors=0 warnings=0（与开工前基线同）；④ `just test` 全绿 —— ✅ 206/206。**收口记录** → §1.8 + `git:调试历史` 2026-10-08 + `third_party/fsr/PATCHES.md` |
| ~~**14.2**~~ ✅ **已收口 2026-10-08** | **输入面研究（只出文档，不改渲染）**：`jitterOffset` / `motionVectors` / `depth` / `reset` 四个量的**单位、坐标系、符号、采样时刻**逐条与 FSR 源码对拍，写成表；顺带定 `ENABLE_DEPTH_INVERTED` / `_DEPTH_INFINITE` / `_MOTION_VECTORS_JITTER_CANCELLATION` / `_DISPLAY_RESOLUTION_MOTION_VECTORS` 的取值 | 用户方向第二条。**这是本项目第三次栽在"参考/闭式的观测坐标系与被测项不一致"的位置**（`phase13_plan` §1.6 撤回条、§1.17 结论 5 的 φ 反号）——先定口径再写代码 | ① 一张四行表，每行给「FSR 要的口径 / 我们现在的口径 / 差什么 / 差值怎么验」——✅ `docs/FSR输入契约.md`；② **每条差值有一条可跑的对拍**（合成靶或解析式），不接受"读源码认为一致" —— ✅ 四条全部实机对拍（载体 = `just fsr-smoke` 的三条契约探针，常驻非常驻可选）；③ 符号项必须有**反向用例**（符号取反必须更差）—— ✅ MV ±dx、jitter 期望取反变异、depth **真开 `DEPTH_INVERTED`** 后判据行 0.2000→1.0000。**残留**：jitter 在 accumulate 路径的定量判据做不出来（合法量级 ±0.5 px 低于重建滤波器量化），已记为 14.5 第一嫌疑 |
| ~~**14.3**~~ ✅ **已收口 2026-10-08** | **内部分辨率与窗口拉伸解耦**：现在 render extent ≡ swapchain extent ≡ 窗口像素（`Resources` 深度图 extent = swapchain extent；全仓**无 `renderScale` 概念**，只有 `vkswapchain.cppm` 的 `windowScale` 诊断）。加一个独立的 render extent，与 swapchain extent、与债-37 的 HPD 分数缩放三者正交 | 用户方向第四条。**它是 FSR 的硬前置，但本身独立可验收、且净收益**（DRS / 性能实验都缺它）；与 14.1 并行 | ① `renderScale ∈ {1.0, 0.75, 0.5}` 下画面正确、resize 不崩、imgui 与**点选**不受影响（债-37 三条连带不许回退）—— ✅ 三档实机出图（0.5 档读图：内容/取景/比例正确，仅整体变软 = 双线性放大预期）；imgui 恒在后缓冲像素、点选两级换算收口，债-37 三条连带无回退（renderScale=1 时比值恰为 1）；**live resize 未手工验证**，改为把取整/钳制写成单测断言覆盖；② `renderScale = 1.0` 时**逐位等于**今天的基线（红线）—— ✅ 1280×720 与 1600×900 两档与开工前基线逐位相同（同会话增量构建），且**结构上**成立：图形状与 renderScale 无关（§1.9）；③ 截图/判据口径显式记录 render extent（否则 imgqa 所有像素类判据静默换含义）—— ✅ 启动日志 + 截图成功日志 + RPC `/state` 新增 `render` 段 + `just shot` 网格行；`--render-scale` **核对引擎实际生效值**（变异验红：请求 0.1 → 钳到 0.25 → FAIL）；④ Wayland 175% 缩放下实测一次 —— ⚠ **未做**（本机无 Wayland 会话）；默认档行为逐位同前，风险仅在显式改档。**收口记录** → §1.9 |
| **14.4** 🕳 **进行中**（纯函数层 + pass 侧已落地） | **逐像素速度场**（债-71 / 债-35，**本 phase 最大的一块**）：compute pass 由两帧相机矩阵 + `depthTex` 解析算出每像素速度（本项目唯一运动是全局刚体 ⟹ 便宜），输出 FSR 口径的 `motionVectors`；同时把 TAA 的重投影从"全局矩阵"改成"读这张场" | 用户方向第三条。**它是天花板唯一的出口**，且能独立验收（不必等 FSR 接完）——臂 14 = 门改吃真实逐像素速度 ⟹ 直接问跷跷板是否松绑 | ① 速度场与**独立解析参考**逐像素对拍（含 pitch；用 `temporal_yaw_remap` 那套带 pitch 的金标准，2.3e-13px）；② **阳性锚点** = 静止机位必须恒 0；③ **反向用例** = 符号取反必须让 §1.22 的 R 变差；④ 用它重跑 `temporal-tail --motion step` 与 `temporal-motion`，报 R 是否 < 1（**这是"运动中 TAA 有害"能不能翻案的判决**） |
| **14.4b** ✅ **已收口**（A 档落地并修正恒假臂 / B 档**否决**挂触发条件 / C 维持不做 / buffer 侧另立 **债-75**）→ 收账记录 **§1.21** | **图外资源挂号 + 布局对账护栏**（FSR 前置，见 §1.7）。做两档、明确不做第三档：**A 护栏**（便宜）= 图外 pass 进出点比对 `attachmentLayouts[slot]` 与预期值，不符即 error 出声，把"进出同值"从**约定**变**断言**；**B 挂号**（中等）= `RgGraphDesc` 增加 external-use 声明（attachment × usage × 插在哪个 pass 之后/之前），GraphCompiler 据此把 barrier 并入计划，`IzbShadow` / `EvssmShadow` / FSR 三处不再手写 **image** barrier；**C 不做** = buffer 侧依赖自动推导（图只跟 image attachment，加进去 = 半个 memory manager） | **排在 14.5 之前**：FSR 是第三个图外 compute，先立护栏再扩面，否则 14.5 的 VVL 判据要替"约定"背锅——布局漂移 VVL **能**报，但报得晚且报在无辜调用点（§1.11 实测修正旧结论"VVL 不报"）；债-32 那类 **buffer 侧**缺口才是 VVL 真不报的（⚠ 批 14.4b 查证**收窄**：VVL 默认不开 syncval；开了之后 **shader 经 descriptor heap 的 buffer 访问它仍看不见**（两个变异 0 条），但 **API 命令之间**的 buffer 竞争它抓得到（现行代码就报 4 条）→ §1.21 四） | ① **阳性锚点** = 故意让某图外 pass 不还原布局 ⟹ 护栏必须 error（**变异验过非空转**）；② **反向** = 现状全绿（护栏零误报，否则等于没有）；③ 红线 = 画面逐位不变（护栏只读不写命令）；④ ~~B 档做完 = `IzbShadow::recordFrame` 的两条 `depthLayoutBarrier` 删除且画面逐位不变~~ **随 B 档否决作废**（该判据本身正是问题：barrier 不由模块发，模块就无从返回真实布局 ⟹ 拆掉 A 档护栏的断言输入）。**收口实测**：红线 A `fdece9d0…` 逐位复现、`just test` 241 例全绿、两个 smoke 均 errors=0；恒假臂的镜像探针与 syncval 查证 → §1.21 |
| **14.5** 🕳 **七批已落地**（第一批 = FSR 每帧跑通 VVL 零错、输出无消费者，见 §1.13；第二批 = **输出接进 tonemap**，零拷贝换槽，**默认只开纯空间档**，见 §1.14；第三批 = **AO 施加点从 taa.frag 拆成独立 `ao_apply` pass**，让"AO 乘在时域之前"与"谁做时域"解耦，见 §1.15；第四批 = **亚像素抖动与 TAA 解耦并申报给 FSR**，见 §1.16——**主实验是否证：时域退化与"没有抖动"无关**；
第五批 = **诊断批（无产品代码改动）：推翻本 Phase 自己的时域测量方式**，纠正后病灶收窄到
「历史读取的采样滤波」一处，见 §1.17；第六批 = **根因定位并修好（vendor host 侧 `abs(float)` 落到 `::abs(int)`）**，见 §1.18；第七批 = **真实档验收 = 通过**：根因 = **申报给 FSR 的是投影注入量、而 FSR 要的是内容位移（Y 反号）**，地面真值实测裁决；同时**换掉失效判据**（`mean|∇|` 在升频区间与质量反相关），见 §1.19；
 第八批 = **配置面收口（无渲染改动）**：抗锯齿三布尔 → 一个三态键 `aa`，四条截图逐位复现，见 §1.20；代码里编号 = **批 14.6**） | **FSR 接线**：落位 = **图外 compute 编舞**（`RgPassKind` 只有 `drawMesh/blit/drawLight`，**无 compute kind**；先例 = IzbShadow / EvssmShadow，见 `evssm_shadow.cppm:20`）。HDR 域位置 = **tonemap 之前**（scene_hdr@render res → FSR → hdr@display res → tonemap → swapchain），`ENABLE_HIGH_DYNAMIC_RANGE` 开。**第二批改判**：原计划「copy 进图附件 `fsr_out`」**不做**——FSR 的 UAV 输出必须常驻 GENERAL（现有 usage→layout 表表达不了），且图内无写者的附件是额外风险；改走 `Resources::ensureFsrSampleSlot()` + 模块自写描述符（`EvssmShadow::momentsSampleSlot` 同族），**零拷贝、零图词表改动** | 唯一架构决策，先写死再动手，免得中途返工 | ① **FSR 关 = 逐位不变**（与 TAA 同款红线，用基线配置）—— ✅ 门关 SHA256 = `fdece9d0…` = 红线基线；② VVL 零报错（含跨 submission 依赖与布局链——债-32 那条"图外编舞"的坑）—— ✅ 门开 PASS errors=0 warnings=0；⚠ **布局漂移 VVL 报得晚且报错点**（报在执行器下一次 barrier 那个无辜调用点、只在调试层在场时存在，§1.11），这一项靠 **14.4b 的护栏**，不是靠 VVL；③ 显存账新增"外部分配器"科目且**闭合断言重新有意义**；④ 本批**只点亮不调参** —— ✅（`kFsrSharpness = 0.2` 取 AMD 样例默认，调参归 14.7）；⑤ **第二批新增：画面判据**——纯空间档 0.5 档边缘能量 **8.43 > 双线性 7.95** ✅，且输出是**帧号纯函数**（同参数两次跑 SHA256 相同 ⟹ FSR 档可做逐位回归）；**时域档实测退化**，默认关；**根因已定位并修好 = vendor host 侧 `lanczos2()` 的非限定 `abs(float)` 落到 `::abs(int)`，Lanczos LUT 退化成阶跃 ⟹ 历史读取核变成 [¼½¼]**（一个 token `fabsf` ⟹ 6.01→13.90 = 无 FSR 的 105%），全链见 **§1.18**；⑥ **第三批新增：AO 施加点解耦**——门关逐位不变 ✅、三档图形态正确 ✅、**AO 净变暗量级比 1.007**（恰好施一次，同时排除零施与双施）✅、装饰器四条判据变异全验红 ✅，见 §1.15；⑦ **第四批新增：抖动解耦**——门关逐位不变 ✅、
**TAA 开时逐位不变**（本批最大回归面，`git stash` 重建对拍 `360ce0d2…` 两次相同）✅、
相位数对 vendor **公开五档表**（18/23/32/72）✅、三条变异验红 ✅、**仪器有效性自证**
（逐帧申报值轨迹 = 标准 Halton 周期 32）✅；**主实验判据 = 否证**（时域+抖动 3.50 ≈ 时域无抖动
3.47，跑满一个周期反而 3.10）——判据本身通过（它问的问题得到了明确答案），见 §1.16 |
| **14.9**（原写 14.6，已让号给 §1.20） | **判据迁移**：直接复用（`temporal-tail --motion step` 含闭式锚臂 E、`temporal-motion` 的 M0/M1/M2/M3x 消融族、理想 N 相位平均参考、往返地板、`aa-coverage`、`temporal` 的 ρ 对闭式、alignment/ablation 两条**元判据**）／换口径（ρ 六机位表：**算子留、阈值全重立**，闭式 ρ\*=0.0827 是 EMA 的闭式，FSR 不是 EMA）／随 TAA 作废（`clip_mode` 臂族、gate 断点锚点、`diag_reject` alpha 解码） | 用户方向最后一条"跑一堆 imgqa"的前置。**不先分类就会拿 TAA 的阈值判 FSR** | ① 三张清单落到 `imgqa` 的判据注册表（**不在文档里抄清单**，抄名单反模式）；② 每条"复用"项跑一次并记录新基线；③ 每条"作废"项**先确认不是护栏**（债-68 三条件） |
| **14.7** | **旋钮调参**：§1.6 那 5 个 configure key × 三轴判据（静止闪烁 / 平移阶跃鬼影 / 运动中 R） | 有判据才有旋钮；顺序必须在 14.6 之后 | 每个 key 给一张扫描表 + **一次消融证明它真的在动**（阳性锚点），并写清代价轴 |
| **14.8** | **VMA 裁决**：默认**不改**，只补账本科目。改那 4 处 `vkAllocateMemory` 的触发条件（任一）：① 外部分配器条数让 `maxMemoryAllocationCount` 逼近上限；② 预算/OOM 需要统一降级；③ 显存账科目精度不够用 | 见 §1.5。**账本口径问题用账本解决，不用 patch 解决** | 若不动：显存日志里 FSR 的 VRAM 有独立一行且 `logMemoryStats` 的闭合断言重新成立（口径写明"含外部分配器 X MiB"） |

> **14.4b 落点已勘察（2026-10-08 夜，别按"便宜"估）**：
> - **图内侧有单一真相源**：`usage → {layout, stage, access}` 查表在 `render_graph_impl` 的
>   `usageInfo`，跨帧布局 = `Renderer::attachmentLayouts`（`vkrenderer.cppm:320`，执行器跨帧追踪、
>   RT generation 变化时重置）。⟹ 图内 attachment 的布局不是缺口。
> - **缺口在图外，且不止 plan 里点名的三处**：自己发 `vkCmdPipelineBarrier2` 的模块实测有
>   **12 个**（env_prefilter / mip_gen / texture_upload(+cubemap) / gpu_canary / izb_shadow /
>   evssm_shadow / frame_capture / imgui_layer …）。
> - ⚠ **A 档的落点不是"在执行器加一条断言"**：`attachmentLayouts` 的**唯一写者就是执行器自己**，
>   图外模块改了布局它并不知道 ⟹ 执行器处的值"自洽但陈旧"，在那里比对**检不出带外修改**。
>   真正的断言点是**图外模块发 barrier 之前**：它要能读到 `attachmentLayouts[slot]` 并声明自己的预期。
>   ⟹ 先要定的是**接口**（Renderer 暴露什么查询/断言入口、12 个模块里哪些必须接），再谈护栏。
>   这一格定错，A 档会做成一条永远为真的断言——正是 §2 判据①"变异验过非空转"要防的形态。

---

## 3. 红线与纪律

- **关 = 逐位不变**：TAA 关（沿用 `fdece9d0…`）与 FSR 关各自一条，**必须用基线配置**
  （`cp configs/*.lua build/rgl/configs/` 之后）。
- **FSR 走 graphics 队列，不用那个闲置的 compute 队列**（债-24 的 compute/transfer 队列与命令池
  全仓零调用，本 phase **明确仍然不用**）。理由：FSR 的输入（scene_hdr / depth / velocity）与输出
  全在 graphics 侧，同队列执行序天然有序（`izb_shadow_impl.cppm:353` 原话），换队列 = 引入跨队列
  ownership barrier 这一整类新 bug 却拿不到任何收益。**"用上 compute shader" ≠ "用上 compute 队列"**
  ——前者是本 phase 的前提，后者是债-24 的死重清理，两件事别混。若将来确要用，先补 §1.7 的 B 档挂号。
- **BIT-IDENTICAL 不是不做 FSR 的理由，但"位级出口"不能想当然**：债-46 曾记三个出口"都已实测存在、
  只差做成基准"。2026-10-09 把出口 ① 落地成 `just regress burst --lag 16` 之后**实测不成立**——
  同相位仍差 1-2 像素 / 恰 1 LSB（跨进程亦然），原"逐位相同"是从残差指标推断的。
  **位比可用面已实测划清**（`just regress floor --frames 1200 --runs 3`）：`aa=off` 与 `aa=fsr`
  **纯空间**档逐位稳定（后者正是 `configs/runtime.lua` fsr 块"输出是帧号纯函数"那句承诺，成立）；
  两档**带时域历史**的不逐位确定（`aa=taa` 差 1-8 像素 / 1 LSB，`--aa fsr --fsr-temporal` 差
  25 像素 / 14 LSB）⟹ **14.5 的"逐位不变"验收只适用于纯空间档**，时域档走 imgqa 统计判据，
  **别给带历史的档立位比闸门**（那条闸门从第一天起就常红）。
- **口径先于代码**（本 phase 的头号纪律）：任何"闭式/参考/判据"必须先核对**适用域与观测坐标系**
  与被测项一致。已三次栽在这里（`phase13_plan` §1.6 撤回、§1.17 结论 5/7、债-60 坏构建读数）。
- **消融实验设计完先自问：被动的量在这个实验里真的会动吗**（box9 教训）。
- **有效性检查不得套在消融组上**（债-70：M3 是按设计就该坏的组）。
- **并行红线**：imgqa 采集可并行，但 BIT-IDENTICAL 类跨进程逐位比对**不许并行**（债-64）。
  14.6/14.7 要扫大量档位，**采集层收敛 + 并行是这两批的前置投资，不是 TAA 遗产维护**。
- 性能数字绑机器；三机轮换，跨机先复测再比。

---

## 4. 明确不做

- **FSR4 / ML-Upscaler / Ray Regeneration / Radiance Caching**：RDNA4 + 预编译签名二进制，换路线了。
- **Frame Generation**：`frame-interpolation-swap-chain` 是 **DXGI/DX12** 的 present 替换，Vulkan 上
  present 节奏要自己接；且它是本 phase 之外的独立目标。
- **DX12 路径**、**SDK 2.x 的 loader/签名 DLL 架构**：不跟。
- **reactive 的材质标记通道**：先用 `GenerateReactive`（opaque-only vs pre-upscale 差分）；真需要
  语义级 reactive 再挂 Phase 10 的 `object_id` pass 骨架。
- **把 FFX 改成用 VMA / 用我们的 DescriptorHeap**：见 14.8 触发条件，默认不做。
- **继续调自研 TAA 的 α / 阈值 / 响应函数形状**：§0.2 已证可行域为空。

---

## 5. 风险清单（按"会让本计划作废"排序）

1. **速度场做不出正确结果**（14.4）→ 那 FSR 与自研都无解，时域线整体转"只做静态机位 AA"。
   缓解：14.4 的判据①②③不依赖 FSR，失败也拿得到结论。
2. ~~**1.1.4 的 VK 后端在 RADV / anv 上跑不起来**~~ ✅ **14.1 已排除（RADV）**：最小 device + 2 帧
   `ffxFsr3UpscalerContextDispatch` 在 STRIX_HALO 上全部 dispatched + executed，VVL 白名单外 0 条
   （`just fsr-smoke`）。descriptor indexing + 自建 pool + pipeline 创建组合成立。**wave64 分支运行时
   永不进**（见 §1.3 修正），所以它不是风险项。**anv 未验**（风险残留，14.5 前用 `just run-intel`
   那台补一次）。踩到的三个运行期坑（P8 跳 0x0、P9/P10 对齐 UB）→ `third_party/fsr/PATCHES.md`。
3. **图外 compute 编舞把债-16"两套执行世界观"从 2 例变 3 例**，且 FSR 的输入是图内产物
   （scene_hdr / depth）、产物里有三张**我们分配、它写入**的图 ⟹ 跨边界布局转换出错就是债-32 型
   UB，而 **布局漂移 VVL 报得晚且报错点**（报在执行器下一次 barrier 那个无辜调用点、根因与症状隔
   一整帧，且只在调试层在场时存在，§1.11 实测修正，见 §1.7）。
   缓解 = **14.4b 先行**（债-72 的 A 档护栏把约定变断言），VVL 只当第二道。
4. **imgqa 采集层不收敛**（38s/组、五份副本）→ 14.6/14.7 的档位扫描"存在却没人跑"，与没判据等价。
5. **显存账闭合断言失效**（§1.5）→ 不是崩溃，是**账不再证明东西**；14.5 判据③专门盯它。
6. 我们其实更需要的是**内容**（水体 / 粒子 / per-entity 动画）而不是又一个时域滤波器——若 14.4
   做完发现场景里仍只有全局刚体，速度场的价值会被低估。缓解：14.4 判据④用现有激励也要出数。

7. **本 device 现在启用了 `shaderFloat16` + `shaderInt16`**（批 14.5 第一批，FSR 的 SPIR-V
   声明了这两个 capability）。已按 **optional** 处理（探测不到就不置位、FSR 侧降级不构造），
   所以理论上不支持的机器只是跑不了 FSR；但**这是设备创建路径的改动，只在 STRIX_HALO 上验过**。
   另两台机器若 device 创建失败或 FSR 静默降级，先看启动日志的
   `Device optional features: … shader16=` 行（`docs/phase14_plan.md` §1.13 第 2 条 = 为什么
   它不走 `features_config.json` 的 optional 机制）。

---

## 6. 记债继承（本 phase 的账，不自动清偿）

| 债 | 归属 | 说明 |
|---|---|---|
| **债-71**（门唯一输入是相机派生量 = 天花板） | **14.4 清偿** | 逐像素速度场 |
| **债-35**（无 per-pixel 速度 / 无膨胀深度） | 14.4 + 14.5 | 膨胀深度由 FSR 内部产出（§1.6） |
| ~~**债-46**（位级基准未落地）~~ | **否决**（2026-10-09） | 出口 ① 落地成 `just regress burst --lag 16` 后被判红 ⟹ "让时域档可位比"这个目标随口径作废（带历史的档不做位比）。**位比可用面见 §5 那条** |
| **债-64**（采集层未收敛 / 并行未做） | **14.6/14.7 前置** | 否则"调旋钮"仍是空话。⚠ `tools/regress.py` 复用 `smoke.py` 采集层是**收敛方向**的一次实际动作 |
| **债-40**（本帧 HDR firefly） | 14.7 首个旋钮 | `FVELOCITYFACTOR → 0` 是它的现成候选修法 |
| **债-72**（图外 barrier 靠约定，边界外无断言） | ✅ **14.4b 收口**：A 结案 / B **否决** / C 维持不做（→ §1.21） | **护栏先于扩面**已执行；buffer 侧另立 **债-75** |
| **债-75**（buffer 侧同步完全在图外、零对账；VVL 连开 syncval 也看不见 shader 侧访问） | ✅ **已结案（2026-10-14，批次 B1 三刀）** | 状态权威 = 总账 §1 的结案行 + `docs/compute_调研.md` §7.2–§7.9（本行不重述机制，只留 14.4b 当时的开户理由：严重度依据 = §1.21 四的实测）。**残留缺口另立债-117** |
| **债-73**（FSR 上游 4 类 spec 违规） | ③ **已结案（P11）**；①②④ 记噪声 | ③ 不处置 = 时域历史是未定义值。防线 = `just fsr-check` 的格式对账 + 冒烟白名单外即 FAIL |
| **债-16**（两套执行世界观） | 14.5 加重、14.4b 缓解 | FSR = 图外 compute 第三例 |
| **债-24**（compute/transfer 队列死重） | **本 phase 明确不用**（见 §3 红线） | "用上 compute shader" ≠ "用上 compute 队列" |
| **债-67 / 债-69 / 债-70** | ~~14.0 收口~~ **随总账 §2.6 整节删除（2026-10-09）** | 三条都是 TAA 消融臂的账面前提；时域线已换轨 FSR、总账里那三行连同 §2.6 一起下线 ⟹ **14.0 的 ②（债-67 默认语义拍板）与 ④（总账标降级）随之作废**（④ 改成了"整节删除"）。读数与裁决在 `git log -p docs/还债总账.md` |
| **债-68**（臂集瘦身） | ~~冻结，触发条件不变~~ 随 §2.6 下线 | 14.9 判据迁移判"作废"时，**护栏身份要回 `taa.frag.slang` 的臂注释核**（臂 1 = 关盒阳性锚点那条 guard 仍在代码里活着，不依赖总账那一行） |
