import scene;  // binding 3：场景光照（readSceneLight，与 forward 同一数据源）

// Phase 7 D1：deferred light pass —— 全屏单平行光光照，读 G-buffer 4 RT 重建 albedo/法线/
// 世界位置/metal-rough，套用与 model_pbr.frag **逐式相同**的简化 PBR（GGX Cook-Torrance）。
// 因材质数据与 forward 同源，本 pass 输出应与 forward 单灯结果等价（差异仅 G-buffer 量化：
// oct 法线 rg16、albedo rgba8——非位精确，视觉等价）。tonemap 不做（forward 亦无），保持可比。
//
// 绑定：3=sceneData（scene.slang import）；4=albedo(RGBA8_SRGB，采样返 linear)；
//   5=normal(RG16_SNORM，oct)；6=worldPos(RGB32_SFLOAT)；7=metalRough(RGBA8_UNORM .r=metal .g=rough)；
//   8=sampler（共享 sampler，sampler heap）。4-8 经 PipelineBindings.extra 映射到 G-buffer RT 堆槽。

[[vk::binding(4, 0)]] Texture2D<float4> albedoTex;
[[vk::binding(5, 0)]] Texture2D<float2> normalTex;
[[vk::binding(6, 0)]] Texture2D<float4> positionTex;
[[vk::binding(7, 0)]] Texture2D<float4> metalRoughTex;
[[vk::binding(8, 0)]] SamplerState gbufferSampler;

struct LightInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// octahedral 解码（与 deferred_geometry.frag octEncode 逐位对称）
float3 octDecode(float2 e)
{
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, -1.0, 1.0);
    return normalize(n);
}

[shader("fragment")]
float4 main(LightInput input) : SV_Target0
{
    float2 uv = input.uv;
    float3 albedo = albedoTex.Sample(gbufferSampler, uv).rgb;
    float3 N = octDecode(normalTex.Sample(gbufferSampler, uv).xy);
    float3 P = positionTex.Sample(gbufferSampler, uv).xyz;
    float4 mr = metalRoughTex.Sample(gbufferSampler, uv);
    float metallic = clamp(mr.r, 0.0, 1.0);
    float roughness = clamp(mr.g, 0.04, 1.0);

    SceneLight light = readSceneLight();
    float3 V = normalize(light.eyePos - P);
    float3 L = -light.direction;

    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 1e-4);
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // GGX 法线分布（epsilon 相对化 1e-7·a2，与 model_pbr.frag 一致，见其注释/教训）
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d + 1e-7 * a2);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float k = a * 0.5;
    float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
    float3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-4);

    float3 diffuse = albedo * (1.0 - metallic) / 3.14159265;
    float3 color = light.ambient * albedo * (1.0 - metallic)
        + light.color * NdotL * (diffuse + specular);
    return float4(color, 1.0);
}
