// Phase 8 批次 ①：HDR tonemap pass —— scene_hdr（RGBA16F，线性 HDR）→ 曝光 ×
// Narkowicz ACES 近似 → swapchain（B8G8R8A8_SRGB，store 硬件 linear→sRGB 编码，
// 故本 frag **不做** pow(1/2.2)——手动 gamma + 硬件编码 = 双重编码发白）。
// mesh 复用 light.mesh.spv（全屏大三角，uv = framebuffer 对齐 1:1），固定全屏单
// group draw（drawLight kind，state="tonemap"，执行器经 state 名解析管线条目）。
//
// 绑定：4=sceneHdr（Texture2D RGBA16F 线性）/ 5=hdrSampler（共享 sampler，1:1
//   全屏采样 maxLod 0 够用）。import scene 仅为读 scene data 头 @56 的曝光标量
//   （CPU 真相源 vkresources.cppm kSceneDataExposureOffset；configs/scene.lua
//   顶层 exposure 每帧经 SceneFrameData 上传——原写死 kExposure 的"调参再走
//   配置"预留兑现）。
//
// Phase 11 批 11.4 追加 AO 施加：binding 6 = ao pass（state="ao"）产出的 R8 遮蔽量，
//   lerp(1, ao, strength) 相乘后再进 ACES。strength 读 scene data @60（头部保留区
//   最后一格，CPU 真相源 scene_data.cppm kSceneDataAoOffset；configs/scene.lua 的
//   ao.strength 每帧经 SceneFrameData 上传）。**strength=0 时 lerp 位级 = 1.0**，
//   故默认配置逐位等于加 AO 之前（BIt-IDENTICAL 基线不受影响）。
//   施加点选在合成（此处）而非材质着色器：forward 无 G-buffer，AO 只在 opaque+skybox
//   之后产出，写在 tonemap 是唯一能对两变体一视同仁的位置（D1 对等）。代价 = AO 会
//   连直接光一起压（见 docs/phase11_plan.md 批 11.4 的偏差说明）。
//
// ── 批 13.5：本 pass **不再**做 un-jitter（批 13.2 的 `taa.base_space` 已删除）──
// 批 13.2 曾在这里按 `+c_t` 偏移采样 `taa_out` 与 `aoTex`，把 TAA 输出从抖动空间搬回基空间。
// 那是**只修显示那一半**：历史仍住抖动空间 ⟹ 累积内部每帧按 `δ_t` 亚像素重采样 ⟹
// 「固定频率 + 固定位置」的 16 帧周期 shimmer 原样留在交付画面里（实测 9.22% 像素 >2LSB）。
// 批 13.5 把 un-jitter **前移到 taa pass 的输入侧** ⟹ `taa_out` 天然就是基空间 ⟹ 本 pass
// 回到朴素 `input.uv`。**若在此处重新加偏移 = 双重 un-jitter，比 13.2 之前更糟。**
// 全案 → `docs/taa_history_space_plan.md`；核选型实测数据 → `taa.frag.slang` 输入侧注释。
//
// **纹理空间现状**（批 13.5 起三者**都在基空间**，无需任何偏移）：
//   ┌ 纹理（binding）  ─ 空间 ─────────────────────────────────────────────┐
//   │ sceneHdrTex (4)  基空间：TAA 开 = taa_out（输入侧已 un-jitter 的时域累积）；
//   │                  TAA 关 = scene_hdr（c_t 恒 0，抖动空间 == 基空间）
//   │ aoTex (6)        抖动空间（ao pass 以 1:1 像素位置读**抖动后**深度算遮蔽量）
//   └──────────────────────────────────────────────────────────────────────┘
// ⚠ **aoTex 与 sceneHdrTex 自批 13.5 起不同空间**（差一个 c_t ≤0.5px）。批 13.2 时代两者
//   同在抖动空间、共用同一个偏移 uv 所以对齐；现在 sceneHdrTex 搬走了而 AO 没有。
//   刻意**不**给 AO 补偏移：AO 是低频遮蔽场（相位差 ≤0.5px 的错位在 8bit 上不可见，
//   且 `imgqa ao-noise` 实测交付颗粒 0.203 LSB < 1 量化步长），而补偏移要给本 pass
//   加一次内容位移查询 + 一个 saturate 钳位。**已登记的独立议题**：AO 在 TAA 之后施加
//   ⟹ 不被时域滤波（→ `还债总账.md` 债-39，含「TAA 开时 AO 随相位变化从未被量化」的告警）。
// **基空间纹理（禁止偏移）**：swapchain 本身（输出网格）、imgui/UI 覆盖层（另走 imgui
// pass，直接画 swapchain）、object_id（picking 与抖动同口径，不经本 pass）。
// 深度不进本 pass（tonemap 不采深度），无需处理。
//
// **红线**：本 pass 的采样坐标自批 13.5 起与「TAA 关」时的本文件逐位相同（`input.uv`）。

import scene;  // sceneData buffer（binding 3：Load(56) 曝光 / Load(60) AO 强度 / @722224 偏移）

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;
[[vk::binding(5, 0)]] SamplerState hdrSampler;
[[vk::binding(6, 0)]] Texture2D<float> aoTex;  // SSAO **可见度**（r8_unorm，ao pass 产出，1=通透）

// 曝光（ACES 输入前的线性倍率）：scene data @56，configs/scene.lua exposure，
// 1.0 = 直通（默认，与旧写死常量逐位一致）。
float readExposure()
{
    return asfloat(sceneData.Load(56));
}

// AO 施加强度：scene data @60，configs/scene.lua ao.strength，0 = 逐位旁路。
float readAoStrength()
{
    return asfloat(sceneData.Load(60));
}

// Narkowicz 2015 "ACES Filmic Tone Mapping Curve" 近似（per-channel 有理二次式，
// RRT+ODT 的曲线拟合；输出仍在显示线性域，交给 sRGB attachment 硬件编码）。
// 矩阵存放/乘法约定：内层花括号 = 行（行主序字面量），mul(v, M) = 行向量×矩阵
// （v 乘第 i 行取分量 j）——此约定下"中性输入不变"要求 **M 的列和 = 1**。
// kACESInput = 标准 ACESFitted sRGB→AP1 矩阵（Narkowicz/MJP：行和为 1 的那张）的
// **转置**存放（列和 = 1 正是原矩阵行和），配 mul(x, M) 方向使用；kACESOutput =
// 标准 AP1→sRGB 矩阵的转置（原本就正确，勿动）。2026-09-15 勘误：旧值第 2/3 行
// 系转写错误（抄成行和≈1 的错版），白输入灰化 + 通道串扰方向反了。
static const float3x3 kACESInput = {
    { 0.59719, 0.07600, 0.02840 },
    { 0.35458, 0.90834, 0.13383 },
    { 0.04823, 0.01566, 0.83777 },
};
static const float3x3 kACESOutput = {
    {  1.60475, -0.10208, -0.00327 },
    { -0.53108,  1.10813, -0.07276 },
    { -0.07367, -0.00605,  1.07602 },
};

float3 acesTonemap(float3 x)
{
    float3 v = mul(x, kACESInput);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.432951) + 0.238081;
    return saturate(mul(a / b, kACESOutput));
}

struct TonemapInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;  // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

[shader("fragment")]
float4 main(TonemapInput input) : SV_Target
{
    // 采样坐标 = **朴素 input.uv**（批 13.5：taa_out 已在 taa pass 输入侧落回基空间，
    // 本 pass 不再偏移；此处再加偏移 = 双重 un-jitter，见文件头）。
    // ⚠ sceneHdrTex 与 aoTex 自批 13.5 起**不同空间**（差 c_t ≤0.5px），刻意不补偏移，
    //   理由与已登记议题 → 文件头「纹理空间现状」段（债-39）。
    const float2 uv = input.uv;
    float3 hdr = sceneHdrTex.Sample(hdrSampler, uv).rgb;
    // AO 在曝光**之前**相乘：遮蔽是入射光的衰减，属物理量，曝光是显示映射。
    // aoTex 与 sceneHdrTex 同为 1:1 framebuffer 对齐，hdrSampler（linear/mipLod 0）
    // 采 R8 可见度与采 HDR 图同条件——AO 本身已逐像素算好，无需额外滤波。
    // 注意 ao 是**可见度乘数**（1=不遮蔽）：strength=1 且 ao=0 才会压成全黑。
    const float ao = aoTex.Sample(hdrSampler, uv).r;
    hdr *= lerp(1.0, ao, readAoStrength());
    float3 mapped = acesTonemap(hdr * readExposure());
    return float4(mapped, 1.0);
}
