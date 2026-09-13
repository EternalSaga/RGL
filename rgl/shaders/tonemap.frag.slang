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

import scene;  // sceneData buffer（binding 3，仅 Load(56)）

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;
[[vk::binding(5, 0)]] SamplerState hdrSampler;

// 曝光（ACES 输入前的线性倍率）：scene data @56，configs/scene.lua exposure，
// 1.0 = 直通（默认，与旧写死常量逐位一致）。
float readExposure()
{
    return asfloat(sceneData.Load(56));
}

// Narkowicz 2015 "ACES Filmic Tone Mapping Curve" 近似（per-channel 有理二次式，
// RRT+ODT 的曲线拟合；输出仍在显示线性域，交给 sRGB attachment 硬件编码）。
// 矩阵 = ACESInputMat/ACESOutputMat 常数（行主序，mul(v, M) 行向量约定）。
static const float3x3 kACESInput = {
    { 0.59719, 0.07600, 0.02840 },
    { 0.21113, 0.75515, 0.14358 },
    { 0.02413, 0.07502, 0.63504 },
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
    float3 hdr = sceneHdrTex.Sample(hdrSampler, input.uv).rgb;
    float3 mapped = acesTonemap(hdr * readExposure());
    return float4(mapped, 1.0);
}
