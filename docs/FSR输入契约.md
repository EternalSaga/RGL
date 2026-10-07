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
| 唯一的线性化函数 | `GetViewSpaceDepth(d) = e / (d − c)`，系数来自上面的查表；**不存在** `ConvertToDepth` / `LinearizeDepth` | `common.h:309-320` ✅ |
| near/far | 先取 min/max 再按 inverted 交换，注释明说「与参数谁大谁小无关，由 flag 决定变换」 | `:713-722` ✅ |

⚠ **一条别引用的死代码**：`GetMaxDistanceInMeters()`（`common.h:340`）里写着「inverted 时
无穷远 = device depth 0.0f」，但**全 vendor 树只有定义、没有任何调用点**（已复核）。
拿它当现行契约就是读了一行不会执行的代码。

## 3. jitterOffset

| 项 | 口径 | 出处 |
|---|---|---|
| 类型 | `FfxFloatCoords2D`（两个 float），host 侧**原样拷进 CB，不做任何归一化** | `ffx_fsr3upscaler.h:202`、`ffx_fsr3upscaler.cpp:945-946` ✅ |
| 单位 | **渲染像素**（官方原文：in unit pixel space）；Halton 生成器值域 ±0.5 像素 | `ffx_fsr3upscaler.h:502-503`、`ffx_fsr3upscaler.cpp:1338-1339` ✅ |
| UV 化的分母 | 一律 `RenderSize()` / `PreviousFrameRenderSize()`，**没有一处用 `UpscaleSize()`** | `accumulate.h:110`、`luma_instability.h:77-78`、`shading_change.h:52`、`prepare_reactivity.h:243` ✅ |
| 上一帧值 | 每帧**无条件**移位得到，与 reset 无关 | `ffx_fsr3upscaler.cpp:943-944` ✅ |

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
Y 的负号只属于投影矩阵，它存在的意义就是抵消 NDC-Y-up 与 UV-Y-down。⬜ 待实机反向用例背书。

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
| jitter Y 符号 | 单帧 jitter + 无运动，量输出相对 jitter=0 基线的位移方向 | ⬜ 未做（§3 只有几何推导，**推导不算验收**） |
| depth inverted / infinite | 合成深度靶，对 `GetViewSpaceDepth` 闭式解 | ⬜ 未做 |

**「读源码认为一致」不算验收**（phase14_plan §2 批 14.2 判据 ②）。**14.2 未收口**：
四条里两条已实机钉死，jitter 与 depth 两条还欠。
