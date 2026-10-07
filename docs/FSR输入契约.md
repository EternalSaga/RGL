# FSR 3.1.4 upscaler 输入契约（批 14.2）

> **状态**：现行（`phase14_plan.md` 批 14.2 的交付物）
>
> 版本锚点 = FSR **3.1.4**（`ffx_fsr3upscaler.h:36,41,46`），vendor 树
> `third_party/fsr/vendor/`。下文路径省略该前缀；行号以该树为准。
>
> **本文件只写「口径」，不写「怎么接」**（接线是 14.5）。每行标对账状态：
> ✅ 源码已核（主 agent 亲自回源码复核过，不是只信调研） / ⬜ 待实机对拍。

## 0. 一句话结论（四条最容易错的）

1. **速度场进 shader 前是「像素」，出来是「UV」，且是 `prev − cur` 方向**——消费点全是
   `fUv + fMotionVector`，**全树没有一处减法**。
2. **`jitterOffset` 传「原始 Halton 值」（±0.5 像素），不是投影矩阵里那个取过负的值**。
   Y 的负号只属于投影矩阵，作用是抵消 NDC-Y-up 与 UV-Y-down 的轴反向。
3. **官方对四个输入量零格式校验**——没有 `CheckFormat`，只有 `ENABLE_DEBUG_CHECKING`
   门控下的**范围告警**。格式错了没人拦，症状是画面错而不是报错。
4. **`reset` 只有一个使用点**，且**不碰 reactivity、不碰 previousFrame\* 系列**。

## 1. motionVectors

| 项 | 口径 | 出处 |
|---|---|---|
| 读取 | `texelFetch(...).xy`（逐点取，非采样；多余通道被忽略） | `callbacks_glsl.h:369-371` ✅ |
| 单位换算 | `fUvMotionVector = fSrcMotionVector * MotionVectorScale()` | `callbacks_glsl.h:373` ✅ |
| 分母 | `motionVectorsTargetSize` = `ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS` ? `upscaleSize` : `renderSize` | `ffx_fsr3upscaler.cpp:992-994` ✅ |
| 分子 | app 传的 `params->motionVectorScale` —— **它是分子不是分母**，见下方「实测」 | `:996-997` ✅ |
| 方向 | `fReprojectedUv = fUv + fMotionVector` ⟹ **UV_prev = UV_cur + mv** ⟹ mv = prev − cur | `prepare_inputs.h:32`、`reproject.h:58`、`prepare_reactivity.h:30,73,206,257`、`luma_instability.h:79`、`shading_change_pyramid.h:149` ✅ |
| 反证「消费时已是 UV」 | `Get4KVelocity(mv) = length(mv * (3840, 2160))` —— 要乘 4K 尺寸才得到像素长度 | `common.h:124-127` ✅ |
| 中间产物 | `dilatedMotionVectors` 存的**已是 UV**，格式 `R16G16_FLOAT` | `prepare_inputs.h:144`、`ffx_fsr3upscaler.cpp:825` ✅ |
| jitter cancellation | 置位则 `mv_uv -= (previousJitter − curJitter) / motionVectorsTargetSize`；**分母是 MV 的 target size，不是 RenderSize** | `:1002-1006`、`callbacks_glsl.h:375-377` ✅ |
| 尺寸 | 头注释说「render resolution unless DISPLAY_RESOLUTION_MOTION_VECTORS」；**代码不校验** | `ffx_fsr3upscaler.h:194` ✅ |

### 实测钉死的三条（`just fsr-smoke` 的契约探针，RADV 实机）

契约是 `stored = input × params.motionVectorScale / motionVectorsTargetSize`。源码只给了式子，
**「app 该填什么」在 vendor 里没有任何一行文字背书** ⟹ 只能实测：

1. **像素单位的输入要填 `motionVectorScale = (1,1)`**。填 `renderSize` 会让 scale 变成 1、
   退化成恒等 ⟹ 等于声明「我的 MV 已经是 UV」。
   ⚠ **第一版探针就是填了 renderSize**，回读直接等于输入（8.0 而不是 0.0125），被这条对拍
   当场抓住——「读源码 + 想当然」在同一个晚上错了两次，这条就是记录。
2. **分母 = `renderSize`（未开 `DISPLAY_RESOLUTION_MOTION_VECTORS` 时）**：喂 8 px、scale (1,1)，
   回读 **0.012497 = 8/640**，不是 8/1280 = 0.00625。两档差 2 倍，且把期望改成 upscaleSize
   会让探针 **FAIL（rc=1）** ⟹ 不是容差松到都算过。
3. **链路上无隐藏取负**：±8 px → ∓0.0125 同号对应。

⟹ **我们的口径结论**：14.4 的速度场按「**渲染分辨率像素、prev − cur、Y 向下**」产出，
`motionVectorScale` 填 `(1,1)`，`jitter cancellation` 不开就先不管。

## 2. depth

| 项 | 口径 | 出处 |
|---|---|---|
| 通道 | shader 只读 `.r` | `callbacks_glsl.h:303-311` ✅ |
| 格式 | 头注释「32bit depth values」；**代码不校验**（无 `CheckFormat`，无格式白名单） | `ffx_fsr3upscaler.h:193` ✅ |
| 只有当前帧 | 不存在 `r_input_depth_prev`；「上一帧深度」是**内部产物** `reconstructedPrevNearestDepth`（我们分配、它每帧写） | `:891-892`、`callbacks_glsl.h:641-645` ✅ |
| 该产物每帧无条件清空 | 清值随 inverted 翻转（`bInverted ? 0.f : 1.f`），**不在 reset 分支内** | `:1071-1076` ✅ |
| `DEPTH_INVERTED` 改什么 | ① 9 点邻域找最近深度的比较方向；② 重建上一帧最近深度的**原子归约算子**（标准 min / 反转 max）；③ `deviceToViewDepth` 查表 | `prepare_inputs.h:96-102`、`callbacks_glsl.h:656-660`、`ffx_fsr3upscaler.cpp:732-747` ✅ |
| `DEPTH_INFINITE` 改什么 | **shader 侧零条件编译分支**；全部效果 = `deviceToViewDepth[0..1]` 查表选择 + 两条 debug 告警 | `:711`、`:732-747` ✅ |

### 实测钉死的：「最近」的方向

`StoreDilatedDepth(iPxPos, depthExtents.fNearest)`（`prepare_inputs.h:145`），而 `dilatedDepth`
**由我们分配** ⟹ 可回读。靶 = 上半 0.2（近）/ 下半 1.0（远）的水平分层
（`vkCmdClearColorImage` 没有矩形概念，得用区域拷贝造）。RADV 实机（`just fsr-smoke`）：

| 取样点 | 不开 flag（本配置） | **开 `DEPTH_INVERTED`** |
|---|---|---|
| 近区内部 row 172 | 0.2000 ✅ | 0.2000 |
| 远区内部 row 188 | 1.0000 ✅ | 1.0000 |
| **分界线下方第一行 row 180** | **0.2000** ✅ | **1.0000** ← 反向用例 |
| 分界线上方最后一行 row 179 | 0.2000 ✅ | 1.0000 |

⟹ 非 inverted 时 `fNearest` = 9 点邻域**最小** device depth（min 胜，跨分界线把近值拉进远区）；
开 inverted 后同一行读到 1.0（max 胜）。**两档给出相反答案 ⟹ 这条判据不是空断言**，
同时把 §2 表里「inverted 改什么」从「读源码」升级成「实测」。

**我们的配置**：标准 Vulkan 深度 [0,1]、0=近、1=远、有限远平面 ⟹ **两个 flag 都不开**，
`cameraNear/cameraFar` 传投影矩阵真值。
| 唯一的线性化函数 | `GetViewSpaceDepth(d) = e / (d − c)`，系数来自上面的查表；**不存在** `ConvertToDepth` / `LinearizeDepth` | `common.h:309-320` ✅ |
| near/far | 先取 min/max 再按 inverted 交换，注释明说「与参数谁大谁小无关，由 flag 决定变换」 | `:713-722` ✅ |

⚠ **一条别引用的死代码**：`GetMaxDistanceInMeters()`（`common.h:340`）里写着「inverted 时
无穷远 = device depth 0.0f」，但**全 vendor 树只有定义、没有任何调用点**（已复核）。
拿它当现行契约就是读了一行不会执行的代码。

### 实测钉死的：**真实深度格式怎么申报**（批 14.5 前置探针，2026-10-08）

上面那条探针用的是 `R32_FLOAT` **颜色**图当深度——它只验方向，**真实深度格式这条路径
此前一条探针都没跑过**，而 14.5 要接的恰恰是它（渲染器唯一深度 = `eD32Sfloat` +
`eDepth` aspect）。补的第 4 条探针把同一张分层靶搬到真 `VK_FORMAT_D32_SFLOAT` 图上，
RADV 实机结论：

| 结论 | 依据 |
|---|---|
| **深度输入必须申报 `FFX_RESOURCE_USAGE_DEPTHTARGET`**，格式串仍填 `FFX_SURFACE_FORMAT_R32_FLOAT` | 只有这个 usage 位会让 `getImageAspect` 给 `DEPTH_BIT`（`ffx_vk.cpp:781-788`），且 `R32_FLOAT + DEPTHTARGET` 才映射成 `VK_FORMAT_D32_SFLOAT`（`:808-813`）；view 的 format/aspect **全取自我们填的 `FfxResourceDescription`，FSR 不看 image 本体**（`:2799` `:2804`） |
| 🔴 **漏申报的后果不是"静默出错"而是 VVL 响**：摘掉该位实测 **7 条白名单外 VVL 报错** | 变异实测（`just fsr-smoke`）。⟹ 本行修正本文档反复引用的那条担心「官方零格式校验 ⟹ 填错不报错、只出画面」：**格式串填错**确实不校验，但 **aspect 填错这一支 VVL 抓得到**（在 D32_SFLOAT 上建 COLOR aspect view 是硬违规）。判据 ②「VVL 零报错」在这里**有效**，不必另造 |
| 申报正确 ⟹ FSR 读真深度图与读 R32F 颜色图**结果逐位相同**（4 条期望值全中） | 探针 3 与探针 4 用同一张靶、同一组期望值，两条都 PASS |
| 造分层深度靶**不能**用 `vkCmdClearDepthStencilImage` | 它的 `pRanges` 收的是 **`VkImageSubresourceRange` 数组**，不是矩形（矩形在 `vkCmdClearAttachments`，那要 render pass）。本探针改走 `vkCmdCopyBufferToImage` + `aspectMask = DEPTH_BIT`（D32_SFLOAT 4B/texel，行紧排满足默认 4B 行对齐） |

**探针非空转已变异验红两条**：① 摘掉 `DEPTHTARGET` → 7 条 VVL；② 分层靶上下对调 → 4 条
期望里 2 条不符（另 2 条因 9 点邻域取 min 在分界线两侧各吃掉一行而恰好仍中——**这正是
判据要放 4 个点而不是 1 个的原因**：单点判据在这种靶上会被邻域膨胀蒙混过去）。

## 3. jitterOffset

| 项 | 口径 | 出处 |
|---|---|---|
| 类型 | `FfxFloatCoords2D`（两个 float），host 侧**原样拷进 CB，不做任何归一化** | `ffx_fsr3upscaler.h:202`、`ffx_fsr3upscaler.cpp:945-946` ✅ |
| 单位 | **渲染像素**（官方原文：in unit pixel space）；Halton 生成器值域 ±0.5 像素 | `ffx_fsr3upscaler.h:502-503`、`ffx_fsr3upscaler.cpp:1338-1339` ✅ |
| **合法范围** | **±0.5 px 是工作点**，host 对 `abs(jitter) > 1.0` 告警（`:219-221`）。超出后重建滤波器的行为**不在设计范围内**，别拿它做实验变量 | `ffx_fsr3upscaler.cpp:219-221` ✅ |
| UV 化的分母 | 一律 `RenderSize()` / `PreviousFrameRenderSize()`，**没有一处用 `UpscaleSize()`** | `accumulate.h:110`、`luma_instability.h:77-78`、`shading_change.h:52`、`prepare_reactivity.h:243` ✅ |
| 上一帧值 | 每帧**无条件**移位得到，与 reset 无关 | `ffx_fsr3upscaler.cpp:943-944` ✅ |

### 实测钉死的：jitter 进速度场的符号与分母

开 `ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` 后 `mv_uv -= (previousJitter − curJitter)/motionVectorsTargetSize`，
而 `mv_uv` 会写进 `dilatedMotionVectors` ⟹ **可以精确回读，不经过重建滤波器**。输入 mv=0 时
回读值就是 `−(prev − cur)/renderSize`。RADV 实机（`just fsr-smoke`）：

| prev | cur | 回读 y | 预测 `−(prev−cur)/360` |
|---|---|---|---|
| 0 | +0.5 | **+0.001389** | +0.001389 ✅ |
| +0.5 | −0.5 | **−0.002777** | −0.002778 ✅ |

一次钉死四件事：公式、**符号**、分母 = `renderSize`（y 用 360 不是 720）、以及
`previous` 确实是上一帧传入的 jitter。变异（期望符号取反）→ 2 条 FAIL、rc=1。

### ⚠ 一个**无效仪器**的记录（14.5 别再建第二次）

最初的思路是「造一条水平边缘，扫 jitter，量输出图里边缘的位移」。跑出来是废的：

| jitter (render px) | −8 | −4 | −2 | 0 | +2 | +4 | +8 |
|---|---|---|---|---|---|---|---|
| 输出边缘行（display px） | 测不到 | 361.5 | 361.5 | 359.75 | 357.5 | 357.5 | 测不到 |

三个致命问题：① **±8 px 超出合法范围 16 倍**（工作点 ±0.5），行为本就不在设计内；
② 在合法量级上预测位移 ≤1 显示像素，而**重建滤波器自身的量化就有 ~2 px**（j=+2 与 +4 同值）；
③ |j|=8 时边缘直接测不出来。

对照组是干净的（同一套测量代码，jitter=0 扫色边缘：s=+4 → +8.00、s=+8 → +16.00，
斜率恰为 upscale 倍数 2）⟹ **坏的不是测量代码，是被测的量选错了**。
⟹ 结论：**jitter 符号要在 cancellation 这条可精确回读的路径上测**，不要在输出图上测。
输出图侧只留下一个**定性**佐证：jitter 增大时边缘单调**负向**移动，与 `uv + j/H` 一致。

### Y 轴符号：源码里裁决不了，几何上能推死

官方伪码（`ffx_fsr3upscaler.h:513-516`）把 jitter 合进投影矩阵时是：

```
jitterX =  2.0f * jitterX / renderWidth;
jitterY = -2.0f * jitterY / renderHeight;      // ← 只有这里取负
```

而 shader 消费时**不做任何 Y 翻转**（`fUv + Jitter()/RenderSize()`）。两者是否自洽，
取决于 NDC 与 UV 的 Y 轴关系，推一下就有唯一答案：

- 投影矩阵让画面在 **NDC** 里偏移 `(2·jx/W, −2·jy/H)`。
- 帧缓冲 / 纹理 UV 与 NDC 的关系是 `uv.y = (1 − ndc.y)/2`（Y 反向），故
  `Δuv = (Δndc.x/2, −Δndc.y/2) = (jx/W, jy/H)`。
- FSR 算的正是 `fUv + (jitterOffset)/(W,H)`。

⟹ **两边相等的唯一解：`dispatch.jitterOffset` 传原始 Halton 值（未取负的那个）**，
Y 的负号只属于投影矩阵，它存在的意义就是抵消 NDC-Y-up 与 UV-Y-down。
**实机佐证**：jitter 增大 → 输出内容单调负向移动（定性），且 cancellation 路径的符号
已精确对拍（见下）。⬜ accumulate 路径的**定量**判据仍缺（原因见「无效仪器」一节）。

## 4. reset

| 项 | 口径 | 出处 |
|---|---|---|
| 唯一使用点 | `resetAccumulation = params->reset \|\| context->firstExecution` | `ffx_fsr3upscaler.cpp:880` ✅ |
| 作用 1 | `frameIndex = 0`（否则逐帧 +1）；shader 侧**没有 `bReset` 常量**，reset 只经 `frameIndex == 0` 传导 | `:1027-1031`、`accumulate.h:123` ✅ |
| 作用 2 | 清累积缓冲 / SPD 亮度缓冲 / frame info | `:1043-1064` ✅ |
| 作用 3 | `jitterPhaseCount` 重新初始化（否则逐帧 ±1 渐变） | `:1013-1021` ✅ |
| **不做什么** | 不碰 reactivity（reactive 是纯每帧输入）；不碰 `previousFrameJitterOffset` / `previousFrameRenderSize` / `previousFrameUpscaleSize` | `:905-910`、`:943-949` ✅ |
| 其他打断历史的来源 | ① 首帧无条件清 ACCUMULATION_1/2 与 LUMA_1/2；② 累积/亮度缓冲是乒乓，队列深度 16 帧；③ context 销毁即历史消失 | `:844-863`、`:45,867-871,1166`、`ffx_fsr3upscaler.h:249-253` ✅ |
| `ENABLE_DEBUG_CHECKING` | **不打断历史**，只是 gate 掉一个纯检查函数 | `:836-839` ✅ |

## 5. 官方自己写的约束（= 免费的口径断言）

`fsr3upscalerDebugCheckDispatch`（`ffx_fsr3upscaler.cpp:184-315`）里与四个量直接相关的三条：

| 行 | 条件 | 消息 |
|---|---|---|
| `:219-221` | `abs(jitterOffset.x\|y) > 1.0` | `jitterOffset contains value outside of expected range [-1.0, 1.0]` |
| `:224-227` | `motionVectorScale > maxRenderSize` | `motionVectorScale contains scale value greater than maxRenderSize` |
| `:229-232` | `motionVectorScale == 0` | `motionVectorScale contains zero scale value` |

⚠ **整体受 `ENABLE_DEBUG_CHECKING` 门控**（`:836-839`）——不开这个 flag，填错了**一声不吭**。
⟹ 14.5 接线时开发期必须常开该 flag（`just fsr-smoke` 已开）。

另外 near/far 的四条告警（`:266-304`）等于把「inverted × infinite 四种组合下 near/far 该填什么」
写成了可执行约束：inverted 要求 `near > far`、inverted+infinite 要求 `near == FLT_MAX`、
非 inverted+infinite 要求 `far == FLT_MAX`、非 inverted 要求 `near >= 0.075`。

## 6. 对账进度（14.2 判据 ②③）

载体 = `third_party/fsr/smoke/fsr_smoke.cpp` 的契约探针（`just fsr-smoke` 每次都跑，
不是可选模式——不跑的判据等于没有判据）。

| 差值 | 可跑对拍 | 状态 |
|---|---|---|
| 速度场单位 / `motionVectorScale` 该填什么 / 分母是谁 | 喂常量 MV + 回读 `dilatedMotionVectors`（我们分配、可直接读） | ✅ **已对拍**：分母 = renderSize；scale 是分子；像素输入填 (1,1)。变异（期望改成 upscaleSize）验过会 FAIL |
| 速度场**符号** | 同上，±dx 各一次 | ✅ **已对拍**：链路上无隐藏取负。方向语义（`uv + mv`）是源码事实，两者合起来把符号钉死 |
| jitter 的符号 / 分母 / previous 语义 | 开 jitter cancellation，mv 输入清零，回读 `dilatedMotionVectors` = `−(prev−cur)/renderSize` | ✅ **已对拍（精确）**：+0.5→+0.001389、prev=+0.5/cur=−0.5→−0.002777。变异（符号取反）2 条 FAIL。⚠ accumulate 路径的**定量**判据缺（原因见 §3「无效仪器」），该路径只有定性佐证 |
| depth「最近」方向 | 水平分层深度靶 + 回读 `dilatedDepth`，判据 = 分界线下方第一行 | ✅ **已对拍 + 真反向用例**：不开 flag 读到 0.2000，开 `DEPTH_INVERTED` 读到 **1.0000**（rc=1）。两档相反 ⟹ 非空断言 |

**「读源码认为一致」不算验收**（phase14_plan §2 批 14.2 判据 ②）——四条全部有实机对拍，
符号项全部有反向用例（MV ±dx、jitter 期望取反变异、depth 真开 inverted flag）。

**残留一条，明确记着**：jitter 在 `accumulate.h:110` 那条路径上的**定量**判据没做出来
（合法量级低于重建滤波器量化，见 §3）。14.5 接线时若发现画面整体偏移半像素级别的问题，
**第一嫌疑是它，不是速度场**。
