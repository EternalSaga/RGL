// ssr_apply.frag —— SSR **施加** pass（Phase 15 批 15.3 子步 B，与 ssr_trace.frag 成对）：
// 读 `scene_hdr`（Load）+ `ssr_trace`（半分辨率 premultiplied 反射贡献，Sample LINEAR）+
// `gbuffer_metalrough`（Load 取 roughness），做**粗糙度感知模糊**后加进成片，写 `ssr_applied`
// （消费者链零改动：tonemap / taa / ao_apply / FSR 读的还是 ssr_applied）。
//
// ── 形态（为什么拆成 trace + apply 两 pass）──
// 论证本体在 `ssr_trace.frag.slang` 文件头（成本 + 可模糊性 + premultiplied 契约），此处不
// 重复；本 pass 只负责「反射怎么合成」：`out.rgb = hdr.rgb + blur(ssr_trace, roughness).rgb`。
// premultiplied 均值让**加法与减法天然同源**——trace 写的是 weight·(F·reflected − iblSpec)
// 与 weight，模糊 = 逐通道均值，两项被同一个权重一起平均，事后不需要重建置信度。
//
// ── 影响域判据的前提（未作用像素逐位不变）──
// **唯一早退** = 模糊结果全零时 `return hdr`（Load 原值，不是 `hdr.rgb + 0`：加零对
// −0.0 不等位（−0.0 + 0.0 = +0.0），旁路必须是**真的 return**）。接收侧门控（下）把
// roughness ≥ gate.y 的 sum 乘成 ±0，**经同一个早退走逐位旁路**——门控不需要自己的
// return。影响域 = 「trace 非零 ∪ 其模糊半径」**∩ 门控域**（域边界 = 门控，不是模糊
// 半径：trace 的门控只关得掉本像素自己的贡献，模糊会把邻域光滑面的 trace 采进
// roughness ≥ gate.y 的像素——bistro 实测不加接收侧门控时域外变暗 6585 px，判据
// `diffstruct-dark-domain` 红）。
// 🔴 **形态红线：门控只准乘、不准在 Sample 循环前加 `return` 早退**。实测（15.3B，
// 本机 RADV）：循环前 `if (reflect == 0.0) return hdr;` 让**门内**像素（roughness
// 0.122、reflect 恒 1）在 quad 里邻居（roughness 0.40）被逐出 Sample 时读出幻影值
// ——闭式靶上柱旁一列 34 px 假反射（trace RT 实dump 为 0，imgqa 判据 B 泄漏 47.8px），
// 去掉早退只留乘法后逐位消失。逐 lane 终止 + 隐式 LOD `Sample` 的 quad 交互不追机制，
// 形态上绕开。直通路径与逐像素自身量一律 `Load`（整数纹素原值），只有模糊 taps 用
// `Sample`（旧单 pass 的取色器分工纪律原样继承 → ssr.frag 绑定面那条 ⚠ 的 git 历史）。
//
// ── 粗糙度感知模糊 = 螺旋 taps（主 agent 裁决：**偏离计划原文「mip chain」**）──
// 计划写「反射图走 mip chain（抄 Filament）」，但 Filament 的 mip chain 是为**任意粗糙度**
// 的通用解，而本引擎 roughness 门控（gate.y = 0.40）**封顶了所需模糊半径**（> 0.40 无 SSR），
// 湿区 0.05–0.35 区间螺旋 taps 视觉等价；mip chain 要 3 张额外 RT + compute 级联 + 逐 level
// 布局链，收益不抵复杂度。**若将来门控放宽，换 mip chain**。
// 形态 = 5 taps **黄金角螺旋**（无旋转偏置的等角分布，5 枚是「比 4 均匀、比 8 便宜」的折中）：
//   · 半径 = roughness × blurScale（**全分辨率 px**；blurScale = ssrGate.z，默认 8.0 ⟹
//     roughness 0.05 → 0.4px ≈ 无模糊、0.35 → 2.8px）；taps 按 sqrt 分布铺成等面积圆盘，
//     blurScale = 0 是合法消融档（半径 0 = 全部 tap 落中心 = 纯双线性上采样）。
//   · 在 **trace 图**上采（premultiplied 均值正确，见上）；**bilinear 上采样本身就是免费的
//     基座模糊**（半分辨率 → 全分辨率的 2×2 盒），螺旋 taps 叠在它上面，所以小 roughness
//     不需要任何特判。
//   · 模糊会**跨边界渗**（凸包外的判据 B 泄漏带 = 模糊半径 + 半分辨率纹素），靶判据的
//     `--edge-px` 豁免带按实测重定（→ docs/phase15_plan.md §3.3 子步 B）。
//
// ── 绑定面（与 CPU 侧 addSsrApplyEntry 成对）──
//   3 = sceneData（import scene；只读 @724560 ssrGate.z = blurScale，口径 → scene_data.cppm）
//   4 = scene_hdr（RGBA16F 线性 HDR 成片，**Load** 直通基准）
//   5 = ssr_trace（RGBA16F premultiplied 反射贡献，**半分辨率**，模糊 taps 用 Sample）
//   6 = gbuffer_metalrough（RGBA8_UNORM .g=rough，模糊半径的自变量）
//   7 = sampler（共享 LINEAR/ClampToEdge；ClampToEdge 让边缘 tap 不绕回对侧 = 不产生
//       对侧内容的假泄漏）

import scene;  // sceneData + readSsrGate

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;    // 本帧成片（RGBA16F，全分辨率）
[[vk::binding(5, 0)]] Texture2D<float4> ssrTraceTex;    // premultiplied 反射贡献（半分辨率）
[[vk::binding(6, 0)]] Texture2D<float4> metalRoughTex;  // G-buffer .r=metal .g=rough
[[vk::binding(7, 0)]] SamplerState blurSampler;         // 共享 LINEAR/ClampToEdge

struct SsrApplyInput
{
    float4 position : SV_Position;  // framebuffer 坐标（Load 用，全分辨率）
    float2 uv       : TEXCOORD0;    // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

// 模糊 taps 数与黄金角（π(3−√5) ≈ 137.508°）。两者都是**形态常量**不是画质旋钮：
// 画质侧只有一个 blurScale（configs/runtime.lua `ssr.blur_scale`），taps 数改小 = 分布
// 出现可见扇向、改大 = 纯成本，没有要扫它们的人（同 ssr_trace 留常量的四个的判据）。
static const int kBlurTaps = 5;
static const float kGoldenAngle = 2.39996323;

[shader("fragment")]
float4 main(SsrApplyInput input) : SV_Target
{
    // 直通基准：trace 全零的像素返回它（`Load` 整数纹素原值），保证"SSR 未作用"的像素
    // 与门关**逐位**相同（判据根据见文件头「影响域」段）。
    const int2 pix = int2(input.position.xy);
    const float4 hdr = sceneHdrTex.Load(int3(pix, 0));

    // 半径换算用**全分辨率**尺寸（blurScale 的口径 = 全分辨率 px；trace 图半分辨率，
    // 但 uv 空间两者一致，偏移按全分辨率换算后在 trace 上采即可）。
    uint pxW, pxH, mLvl;
    sceneHdrTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 px = float2(float(pxW), float(pxH));

    // 模糊半径 = roughness × blurScale（scene data @724560.z）。blurScale = 0 = 消融档：
    // 半径 0 ⟹ 5 枚 tap 全落像素中心 = 纯双线性上采样（不是"跳过模糊"，是它自然退化）。
    const float4 gate = readSsrGate();
    const float roughness = metalRoughTex.Load(int3(pix, 0)).g;
    // **接收侧门控**（与 trace 的 weight 同式 `1 - smoothstep(gate.x, gate.y, rough)`）：
    // trace 的门控只关得掉「本像素自己 march 出来的贡献」，管不到模糊从**邻域光滑面**
    // 采进来的 trace——bistro 实测（15.3B 验收）不加本门控时 roughness ≥ gate.y 的像素上
    // 变暗 6585 px（半径 = rough×8 恰好在门该关死处最大），`diffstruct-dark-domain`
    // （域外变暗恰 0）红。半径仍用**未钳制** roughness（gate.y 以上反正整项被门归零，
    // 半径在那边只是被丢弃的采样开销，不构成第二处门控语义）。
    const float reflect = 1.0 - smoothstep(gate.x, gate.y, roughness);
    const float radiusPx = roughness * gate.z;

    // 黄金角螺旋 5 taps，sqrt 分布 = 等面积圆盘（第 i 枚半径 = R·sqrt((i+0.5)/N)，
    // 最外 tap ≈ 0.95R）。premultiplied 均值：直接对 rgba 求和取平均，加/减两项同源平均。
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (int i = 0; i < kBlurTaps; ++i)
    {
        const float r = radiusPx * sqrt((float(i) + 0.5) / float(kBlurTaps));
        const float a = float(i) * kGoldenAngle;
        const float2 offset = float2(cos(a), sin(a)) * (r / px);
        sum += ssrTraceTex.Sample(blurSampler, input.uv + offset);
    }
    sum /= float(kBlurTaps);
    // 接收侧门控乘在**均值之后**（线性算子，先后等价）：rgba 一起乘 = premultiplied
    // 不变式（a==0 ⟹ rgb 恒 ±0）保持，下游若再消费 a 不会被破坏。reflect = 0 时 sum
    // 成 ±0 → 走下面的全零早退 = **逐位**旁路（这就是门控不加自己那条 return 的底气，
    // 为什么不加 → 文件头形态红线）。代价 = 门关死像素也付 5 次采样（早退省不掉正确性，
    // 只省成本，而成本侧门关死像素本就过不了 roughness 门控的多数场景）。
    sum *= reflect;

    // 影响域早退（文件头）：5 枚 tap 全零 ⟺ 覆盖本像素的 trace texel 全是「无贡献」
    // （premultiplied 不变式：a == 0 ⟹ rgb 恒 ±0）⟹ return Load 原值，逐位 = 门关。
    if (all(sum == float4(0.0, 0.0, 0.0, 0.0)))
        return hdr;
    return float4(hdr.rgb + sum.rgb, hdr.a);
}
