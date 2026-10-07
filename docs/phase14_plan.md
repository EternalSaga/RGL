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
  绕 VMA"（AGENTS 已知问题 / `vulkan_extensions_2025_调研.md` §十二）。同一个权衡，别双标。

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

### 1.7 渲染图的 barrier 覆盖面现状（2026-10-07 核实；决定 FSR 落位的风险面）

**自动推导只覆盖 image attachment 的布局链，且图外访问靠"进出同值"的约定**：

| 事实 | 出处 |
|---|---|
| 图自动推导 = attachment 布局链（`usage → {layout, stage, access}` 查表） | `render_graph.cppm:21-24, 32-37` |
| **明确不进图**：descriptor/sampler heap host-write barrier、scene data staging copy barrier、acquire/present、fence | `render_graph.cppm:26-30` |
| `RgAttachmentKind` = swapchain / depth / offscreen / depthOffscreen —— **无 buffer kind** ⟹ **buffer 侧同步完全在图外** | `render_graph.cppm:61-64` |
| 图外 compute 家族手写 barrier：`gpu_cull` / `IzbShadow` / `EvssmShadow` / `env_prefilter` / `mip_gen` / `texture_upload` / `gpu_canary` | `izb_shadow.cppm:27`「render_graph 无 compute kind」、`evssm_shadow.cppm:20` |
| IzbShadow 每帧 ~10 条 buffer barrier + **2 条深度图布局 barrier** | `izb_shadow_impl.cppm:256, 280` |
| 深度布局的处理方式 = **进出同值**，注释自陈「**渲染图布局追踪表不可见**，零漂移」 | `izb_shadow.cppm:52`、`izb_shadow_impl.cppm:279`；`frame_capture_impl.cppm:162` 同构 |
| 追踪表 = `Renderer::attachmentLayouts`，图外路径**读出并还原**，但**没有任何断言校验还原真的发生了** | `vkrenderer.cppm:320`、`vkrenderer_passrec.cppm:268` |

⟹ **缺口不是"漏了几条 barrier"，是"约定无断言"**：布局若真漂移，VVL 不报（布局本身合法），
症状推迟到**下一帧首 barrier 的 `oldLayout` 失配**才炸，根因与症状隔一整帧。债-32（mask 永不清零
却逐位相同）就是同一形态：**约定成立时一切正常，所以没人知道它会不会不成立。**

**FSR 是第三个图外 compute**，且它比 IzbShadow 更麻烦：它的产物里有三张**由我们分配、由它写入**的
图（`dilatedDepth` / `dilatedMotionVectors` / `reconstructedPrevNearestDepth`），还有一张我们每帧
写的 velocity buffer ⟹ 手写 barrier 的面积会再扩大。**所以护栏必须先立，再扩面。**

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
   「`-O0` 冒烟过了」在这类 bug 上**不提供任何信息** → `调试历史.md` 2026-10-08。
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
  布局由编译图负责，单一真相源已存在）；**不碰**闲置的 `computeQueue`（债-24）；
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
  抬升是逐字搬移，**但用 `just shot` 逐位对拍证明过**（时机判据与验证方式 → 调试历史
  2026-10-08 批 14.4 第 2 条）。
- **闭合式**：`vel_base = (prevUv − input.uv)/texel − c_{t−1}`——代入后 `c_t` 自己消掉。
  `c_t` 与 `c_{t−1}` 这一对由 `render_list_impl.cppm:263/:285` 受**同一个** `taa.base_space`
  门控写 0 ⟹ 永远成对，门关时自动退化为自洽的抖动空间速度（不是"半修正"的错误形态）。
- **判据（实测）**：门开 VVL **0 错 0 警**、RT = 渲染网格（`smoke.py --velocity-field`
  双向核对，含"静默没落图"第三档）；门开/门关截图**逐位相同**（无消费者 ⟹ 本应如此）；
  活性 = 把输出附件临时改指 `scene_hdr` 的变异，画面平均绝对差 **98.74**。
  ⚠ **"逐位相同"对新 pass 是盲区不是证据**，推广见调试历史同批第 1 条。
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
- **判据 ②（反向）**：现状 `izb_shadow = true`（默认开、每帧都过护栏）→ **0 条**漂移告警。
  ⚠ 这条只有在确认 IZB 真在跑时才有意义（先查启动行 `izb: 开门 grid=512`，否则"0 误报"
  是空转出来的）。
- **判据 ③（红线）**：护栏只读不写命令 → 截图与基线**逐位相同**。
- **B 档未做**（本批只做 A 档，与 plan 一致）。

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
   否则 `first_frame` 统计又漏掉 FSR 全部显存——这是 `调试历史.md:1958-1965` 记过的同一病灶。
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

## 2. 批次拆分与验收判据

> 顺序按「失败最便宜」排，不是按依赖最紧排。**14.1 与 14.3 可并行**（14.3 不碰 FSR）。
> 每批收口 = 在本节划掉 + 记 `调试历史.md`。

| 批 | 内容 | 为什么在这个位置 | 验收判据 |
|---|---|---|---|
| **14.0** | **TAA 冻结收口**（前置，别跳）：① 臂 13 转交付（`configs/scene.lua` `clip_mode = 13` + 重立红线 B + 六机位阶跃补跑）**或**把判据 ④ `temporal-tail-step-clip-holds` 显式降级为 advisory 并写明理由；② 债-67 默认语义拍板（推荐：imgqa 读 `scene.lua` 的 `clip_mode` 当默认）；③ `phase13_plan.md` §2 表逐条给三态终局；④ 还债总账把 TAA 线标**降级 + 触发条件** | **不转交付就必留一条常红判据**，而债-61 已诊断过这个反模式：一条永远红的闸门 = 没有闸门，且永久污染"全绿"这个信号。臂 13 静止侧逐位不变已被三台仪器独立确认 ⟹ 转交付风险≈0 | ① `imgqa selftest` 全绿**且无常红项**（rc 可当闸门）；② 红线 A 复现 `fdece9d0…`（TAA 关逐位不变）；③ 台账里 TAA 相关债**每条有终态**，无"看过一眼就算完" |
| ~~**14.1**~~ ✅ **已收口 2026-10-08** | **vendor 骨架 + 编译成功**：从 `c6efa6b` 捞 `sdk/{include,src/backends/vk,src/backends/shared,src/components}` 的 FSR3-upscaler 闭包 → 写 shader 排列生成器（glslang）→ 产物签入 + SHA1 护栏 → `ffx_vk.cpp` 在 Linux/clang 编过（`<codecvt>` 死分支、警告） | 用户方向第一条。**编译不过，后面全是空谈**；且这一步**不碰引擎**，失败可整棵删掉 | ① 独立 target（`third_party/fsr/`）**能编出静态库**，Linux clang + Windows MSVC 双平台 configure 通过 —— ⚠ **Windows 侧未验**（本机无 Windows；改动全在 `#ifndef _WIN32` 或新增 TU，是推理非实测，见 §1.8）；② SPIR-V 排列产物齐（**实际 80 个唯一模块**，非 40）、`just fsr-check` SHA1 护栏覆盖 —— ✅ 变异验过非空转；③ **交付路径逐位不变** —— ✅ `just smoke` PASS errors=0 warnings=0（与开工前基线同）；④ `just test` 全绿 —— ✅ 206/206。**收口记录** → §1.8 + `调试历史.md` 2026-10-08 + `third_party/fsr/PATCHES.md` |
| ~~**14.2**~~ ✅ **已收口 2026-10-08** | **输入面研究（只出文档，不改渲染）**：`jitterOffset` / `motionVectors` / `depth` / `reset` 四个量的**单位、坐标系、符号、采样时刻**逐条与 FSR 源码对拍，写成表；顺带定 `ENABLE_DEPTH_INVERTED` / `_DEPTH_INFINITE` / `_MOTION_VECTORS_JITTER_CANCELLATION` / `_DISPLAY_RESOLUTION_MOTION_VECTORS` 的取值 | 用户方向第二条。**这是本项目第三次栽在"参考/闭式的观测坐标系与被测项不一致"的位置**（`phase13_plan` §1.6 撤回条、§1.17 结论 5 的 φ 反号）——先定口径再写代码 | ① 一张四行表，每行给「FSR 要的口径 / 我们现在的口径 / 差什么 / 差值怎么验」——✅ `docs/FSR输入契约.md`；② **每条差值有一条可跑的对拍**（合成靶或解析式），不接受"读源码认为一致" —— ✅ 四条全部实机对拍（载体 = `just fsr-smoke` 的三条契约探针，常驻非常驻可选）；③ 符号项必须有**反向用例**（符号取反必须更差）—— ✅ MV ±dx、jitter 期望取反变异、depth **真开 `DEPTH_INVERTED`** 后判据行 0.2000→1.0000。**残留**：jitter 在 accumulate 路径的定量判据做不出来（合法量级 ±0.5 px 低于重建滤波器量化），已记为 14.5 第一嫌疑 |
| ~~**14.3**~~ ✅ **已收口 2026-10-08** | **内部分辨率与窗口拉伸解耦**：现在 render extent ≡ swapchain extent ≡ 窗口像素（`Resources` 深度图 extent = swapchain extent；全仓**无 `renderScale` 概念**，只有 `vkswapchain.cppm` 的 `windowScale` 诊断）。加一个独立的 render extent，与 swapchain extent、与债-37 的 HPD 分数缩放三者正交 | 用户方向第四条。**它是 FSR 的硬前置，但本身独立可验收、且净收益**（DRS / 性能实验都缺它）；与 14.1 并行 | ① `renderScale ∈ {1.0, 0.75, 0.5}` 下画面正确、resize 不崩、imgui 与**点选**不受影响（债-37 三条连带不许回退）—— ✅ 三档实机出图（0.5 档读图：内容/取景/比例正确，仅整体变软 = 双线性放大预期）；imgui 恒在后缓冲像素、点选两级换算收口，债-37 三条连带无回退（renderScale=1 时比值恰为 1）；**live resize 未手工验证**，改为把取整/钳制写成单测断言覆盖；② `renderScale = 1.0` 时**逐位等于**今天的基线（红线）—— ✅ 1280×720 与 1600×900 两档与开工前基线逐位相同（同会话增量构建），且**结构上**成立：图形状与 renderScale 无关（§1.9）；③ 截图/判据口径显式记录 render extent（否则 imgqa 所有像素类判据静默换含义）—— ✅ 启动日志 + 截图成功日志 + RPC `/state` 新增 `render` 段 + `just shot` 网格行；`--render-scale` **核对引擎实际生效值**（变异验红：请求 0.1 → 钳到 0.25 → FAIL）；④ Wayland 175% 缩放下实测一次 —— ⚠ **未做**（本机无 Wayland 会话）；默认档行为逐位同前，风险仅在显式改档。**收口记录** → §1.9 |
| **14.4** 🕳 **进行中**（纯函数层 + pass 侧已落地） | **逐像素速度场**（债-71 / 债-35，**本 phase 最大的一块**）：compute pass 由两帧相机矩阵 + `depthTex` 解析算出每像素速度（本项目唯一运动是全局刚体 ⟹ 便宜），输出 FSR 口径的 `motionVectors`；同时把 TAA 的重投影从"全局矩阵"改成"读这张场" | 用户方向第三条。**它是天花板唯一的出口**，且能独立验收（不必等 FSR 接完）——臂 14 = 门改吃真实逐像素速度 ⟹ 直接问跷跷板是否松绑 | ① 速度场与**独立解析参考**逐像素对拍（含 pitch；用 `temporal_yaw_remap` 那套带 pitch 的金标准，2.3e-13px）；② **阳性锚点** = 静止机位必须恒 0；③ **反向用例** = 符号取反必须让 §1.22 的 R 变差；④ 用它重跑 `temporal-tail --motion step` 与 `temporal-motion`，报 R 是否 < 1（**这是"运动中 TAA 有害"能不能翻案的判决**） |
| **14.4b** ✅ **A 档已落地**（B 档待 14.5 后重裁，见 §1.11） | **图外资源挂号 + 布局对账护栏**（FSR 前置，见 §1.7）。做两档、明确不做第三档：**A 护栏**（便宜）= 图外 pass 进出点比对 `attachmentLayouts[slot]` 与预期值，不符即 error 出声，把"进出同值"从**约定**变**断言**；**B 挂号**（中等）= `RgGraphDesc` 增加 external-use 声明（attachment × usage × 插在哪个 pass 之后/之前），GraphCompiler 据此把 barrier 并入计划，`IzbShadow` / `EvssmShadow` / FSR 三处不再手写 **image** barrier；**C 不做** = buffer 侧依赖自动推导（图只跟 image attachment，加进去 = 半个 memory manager） | **排在 14.5 之前**：FSR 是第三个图外 compute，先立护栏再扩面，否则 14.5 的 VVL 判据要替"约定"背锅，而债-32 已经证明这类缺口 VVL 不报 | ① **阳性锚点** = 故意让某图外 pass 不还原布局 ⟹ 护栏必须 error（**变异验过非空转**）；② **反向** = 现状全绿（护栏零误报，否则等于没有）；③ 红线 = 画面逐位不变（护栏只读不写命令）；④ B 档做完 = `izb_shadow_impl.cppm:256,280` 两处 `depthLayoutBarrier` 删除且画面逐位不变 |
| **14.5** 🕳 **前置已清**（深度格式探针已落地，集成面见 §1.12） | **FSR 接线**：落位 = **图外 compute 编舞**（`RgPassKind` 只有 `drawMesh/blit/drawLight`，**无 compute kind**；先例 = IzbShadow / EvssmShadow，见 `evssm_shadow.cppm:20`）。HDR 域位置 = **tonemap 之前**（scene_hdr@render res → FSR → hdr@display res → tonemap → swapchain），`ENABLE_HIGH_DYNAMIC_RANGE` 开 | 唯一架构决策，先写死再动手，免得中途返工 | ① **FSR 关 = 逐位不变**（与 TAA 同款红线，用基线配置）；② VVL 零报错（含跨 submission 依赖与布局链——债-32 那条"图外编舞"的坑）；⚠ **布局漂移 VVL 抓不到**（布局本身合法，症状隔一整帧），这一项靠 **14.4b 的护栏**，不是靠 VVL；③ 显存账新增"外部分配器"科目且**闭合断言重新有意义**；④ 本批**只点亮、不调参**，`NATIVEAA` + `renderScale=1.0` 起步 |
| **14.6** | **判据迁移**：直接复用（`temporal-tail --motion step` 含闭式锚臂 E、`temporal-motion` 的 M0/M1/M2/M3x 消融族、理想 N 相位平均参考、往返地板、`aa-coverage`、`temporal` 的 ρ 对闭式、alignment/ablation 两条**元判据**）／换口径（ρ 六机位表：**算子留、阈值全重立**，闭式 ρ\*=0.0827 是 EMA 的闭式，FSR 不是 EMA）／随 TAA 作废（`clip_mode` 臂族、gate 断点锚点、`diag_reject` alpha 解码） | 用户方向最后一条"跑一堆 imgqa"的前置。**不先分类就会拿 TAA 的阈值判 FSR** | ① 三张清单落到 `imgqa` 的判据注册表（**不在文档里抄清单**，抄名单反模式）；② 每条"复用"项跑一次并记录新基线；③ 每条"作废"项**先确认不是护栏**（债-68 三条件） |
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
- **BIT-IDENTICAL 不是不做 FSR 的理由**：债-46 的三个位级出口（`lag-16` 同相位逐位相同 ⇒ 相位可锁、
  `temporal-identity` 恒等式门、`warm=2 vs 128` 预热无关性）对 FSR 同样成立，**先落地成基准**。
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
   UB，而 **VVL 抓不到布局漂移**（布局本身合法，症状隔一整帧才出现，见 §1.7）。
   缓解 = **14.4b 先行**（债-72 的 A 档护栏把约定变断言），VVL 只当第二道。
4. **imgqa 采集层不收敛**（38s/组、五份副本）→ 14.6/14.7 的档位扫描"存在却没人跑"，与没判据等价。
5. **显存账闭合断言失效**（§1.5）→ 不是崩溃，是**账不再证明东西**；14.5 判据③专门盯它。
6. 我们其实更需要的是**内容**（水体 / 粒子 / per-entity 动画）而不是又一个时域滤波器——若 14.4
   做完发现场景里仍只有全局刚体，速度场的价值会被低估。缓解：14.4 判据④用现有激励也要出数。

---

## 6. 记债继承（本 phase 的账，不自动清偿）

| 债 | 归属 | 说明 |
|---|---|---|
| **债-71**（门唯一输入是相机派生量 = 天花板） | **14.4 清偿** | 逐像素速度场 |
| **债-35**（无 per-pixel 速度 / 无膨胀深度） | 14.4 + 14.5 | 膨胀深度由 FSR 内部产出（§1.6） |
| **债-46**（位级基准未落地） | 14.5 前置 | 三个出口对 FSR 同样成立 |
| **债-64**（采集层未收敛 / 并行未做） | **14.6/14.7 前置** | 否则"调旋钮"仍是空话 |
| **债-40**（本帧 HDR firefly） | 14.7 首个旋钮 | `FVELOCITYFACTOR → 0` 是它的现成候选修法 |
| **债-72**（图外 barrier 靠约定且无断言 / buffer 侧完全在图外） | **14.4b 清偿** | **护栏先于扩面**：FSR 是第三个图外 compute |
| **债-73**（FSR 上游 4 类 spec 违规） | ③ **已结案（P11）**；①②④ 记噪声 | ③ 不处置 = 时域历史是未定义值。防线 = `just fsr-check` 的格式对账 + 冒烟白名单外即 FAIL |
| **债-16**（两套执行世界观） | 14.5 加重、14.4b 缓解 | FSR = 图外 compute 第三例 |
| **债-24**（compute/transfer 队列死重） | **本 phase 明确不用**（见 §3 红线） | "用上 compute shader" ≠ "用上 compute 队列" |
| **债-67 / 债-69 / 债-70** | 14.0 收口 | 冻结的账面前提 |
| **债-68**（臂集瘦身） | 冻结，触发条件不变 | 14.6 判"作废"时按它的三条件核 |
