// ao_apply.frag —— **AO 施加专用**全屏 pass（Phase 14 批 14.5 第三批）。
//
// 【这个 pass 为什么存在】
// 「AO 必须在**时域滤波之前**施加」是批 13.6 定下的不变量：AO 值在深度不连续处随抖动相位剧变
// = **真实锯齿**，抖动 + 时域平均正是消它的工具；施加在时域之后 = 噪声零平均，实测占残余
// shimmer **62%**（AO 开 6.899% / 关 2.639%，且按梯度分层 100% 住在边缘）。
// 但此前这条不变量被**硬编码成「时域滤波器 == taa pass」**——AO 乘法直接长在 taa.frag 里。
// Phase 14 要换时域滤波器（FSR upscaler 取代 TAA），照原样就会退回 13.6 修掉的那个形态：
// FSR 读 scene_hdr（无 AO）→ AO 只能落回 tonemap = 时域**之后**。
// 本 pass 把施加点抽成独立一步：**与"谁做时域"无关**。
//
// 【何时插】只在「图里有一个**不是 TAA** 的时域滤波器（= FSR）且 taa pass 不在图里」时由
// `decorateAoApplyPass` 插入。三种组合各有唯一施加点，绝不双施：
//   · TAA 在图里       → AO 仍由 taa.frag 施加（本 pass 不插，Phase 13 冻结文件一字不动）
//   · 只有 FSR 在图里  → 本 pass 施加，FSR 的色输入 = 本 pass 输出
//   · 两个都不在（门关）→ **本 pass 不插**，AO 仍由 tonemap 施加 = 图逐位不变（红线）
// 施加点是否已在本 pass 之前发生，经 scene data 的标志位告诉 tonemap（它见 1 就跳过自己的
// 乘法）；CPU 侧写的是**实际图形态**，不是配置键。
//
// 【采样纪律：两条，从 taa.frag 批 13.6 原样继承，一条都不能少】
//   ① **色采 `input.uv`、AO 采 `baseUv`**。本 pass **不改变纹理空间**（输出与输入同空间），
//      而 aoTex 的纹素 q 装的是**抖动像素 q** 的遮蔽量 = 基位置 q − c_t 的遮蔽量 ⟹ 要基位置
//      p 的 AO 必须采 p + c_t。采 `input.uv` 会引入一个**随相位变的 ≤0.5px 错位**。
//      今天 TAA 关时 c_t 恒 0 ⟹ `baseUv == input.uv`，与 tonemap 现在的采法**逐位等价**；
//      这条不是为今天写的，是为了**接 FSR 抖动线那天**：届时色会带抖动而 AO 不会，
//      少这一句就是相位相关的错位（`docs/FSR输入契约.md` §3 同题）。
//   ② **AO 必须线性采**：baseUv 是分数坐标，点采会随相位**跳纹素** ⟹ AO 仍是相位阶跃函数，
//      时域平均收不拢；线性采把它变成相位的连续函数才谈得上被平均。共享 sampler 是
//      linear / mipLod 0，满足（ao 是 r8_unorm 可过滤，与 D32 深度不可过滤那条不同，别混）。
//
// 【精度口径】输出写 RGBA16F ⟹ 相比 tonemap 内的 float32 乘法**量化一次**。这与今天 TAA 路径
// 完全一致（taa_out 同为 RGBA16F），不是本 pass 新引入的损失。
//
// 【逐位旁路】`ao.strength = 0` 时 `lerp(1.0, ao, 0) ≡ 1.0`（IEEE 精确）⟹ 本 pass 退化成
// 一次纯拷贝，与 ao pass / tonemap 侧同一技巧。

import scene;  // 头区 readAoStrength（binding 22）+ TAA 区 readTaaParams（binding 30）

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;   // 本帧成片（RGBA16F 线性 HDR）
[[vk::binding(5, 0)]] SamplerState hdrSampler;         // 共享 sampler（linear / mipLod 0）
[[vk::binding(6, 0)]] Texture2D<float> aoTex;          // SSAO 可见度（r8_unorm，1 = 通透）

struct AoApplyInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;  // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

[shader("fragment")]
float4 main(AoApplyInput input) : SV_Target
{
    // 纪律①前半：色留在它本来的空间（抖动空间），本 pass 不做 un-jitter——那是时域滤波器的活
    //（或它之前的那一步）。在这里搬 = 与下游再搬一次叠成双重 un-jitter。
    const float4 hdr = sceneHdrTex.Sample(hdrSampler, input.uv);

    // 纪律①后半：AO 采基空间坐标。c_t 来自 `readTaaParams().zw`（口径 =
    // `taa_jitter.cppm::contentShiftPx` = `(+jx, −jy)`，**不是** jitterPx，差一个 y 轴翻号）；
    // TAA 关 = 恒 0。**必须 saturate**：共享 sampler 的 wrap 是 REPEAT（平铺 UV 定案），
    // 屏幕空间 uv 越界会绕回采到**对侧**像素 = 1px 假边；|c_t| ≤ 0.5px 正好触发。
    // 钳到 [0,1] 等价 CLAMP_TO_EDGE，不改内部像素一个 bit（论证同 taa.frag 批 13.5b）。
    const float4 taaParams = readTaaParams();
    uint pxW, pxH, mLvl;
    sceneHdrTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));
    const float2 baseUv = saturate(input.uv + taaParams.zw * texel);

    // 纪律②：线性采（`hdrSampler` 即 linear / mipLod 0），Level 显式 0 避免隐式 LOD 选 mip。
    const float ao = aoTex.SampleLevel(hdrSampler, baseUv, 0.0).r;

    // AO 在曝光**之前**相乘：遮蔽是入射光的衰减，属物理量，曝光是显示映射（口径同 tonemap）。
    // ao 是**可见度乘数**（1 = 不遮蔽）：strength=1 且 ao=0 才压成全黑。
    return float4(hdr.rgb * lerp(1.0, ao, readAoStrength()), hdr.a);
}
