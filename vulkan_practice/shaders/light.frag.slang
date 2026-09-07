import scene;    // binding 3：sceneData + SceneLight/readSceneLight（共享 eyePos/ambient）
import lights;   // 多灯：readLightCount/readGpuLight（lights 内部亦 import scene）

// Phase 7 D1：deferred light pass —— 全屏逐灯光照，读 G-buffer 4 RT 重建 albedo/法线/
// 世界位置/metal-rough，套用与 model_pbr.frag **逐式相同**的简化 PBR（GGX Cook-Torrance）。
// Phase 7 D3：升级为多灯循环——灯数据源 = scene data 尾区 GpuLight[]（lights.slang）；
// 共享 eyePos(@12)/ambient(@36) 仍来自 scene.slang readSceneLight（与 forward 同源）。
// 单灯（lights=[主定向光]）时输出与旧 D1 单灯式等价（atten=1、gl.color 预乘 = forward
// light.color）；材质数据与 forward 同源故视觉等价（非位精确：oct 法线 rg16、albedo rgba8）。
// 点光（type=1）线性衰减 1-dist/range（无物理 1/r²，够用）；定向（type=0）atten=1。
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

// 单灯 GGX 贡献（与 model_pbr.frag 逐式相同，epsilon 相对化 1e-7·a2）；atten = 距离衰减
float3 shadeLight(float3 N, float3 V, float3 albedo, float metallic, float roughness,
                  float3 L, float3 radiance, float atten)
{
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 1e-4);
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d + 1e-7 * a2);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    float k = a * 0.5;
    float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
    float3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-4);

    float3 diffuse = albedo * (1.0 - metallic) / 3.14159265;
    return atten * radiance * NdotL * (diffuse + specular);
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

    SceneLight shared = readSceneLight();     // 共享项：eyePos@12、ambient@36
    float3 V = normalize(shared.eyePos - P);
    float3 color = shared.ambient * albedo * (1.0 - metallic);

    uint n = readLightCount();
    for (uint i = 0; i < n; ++i)
    {
        GpuLight gl = readGpuLight(i);
        float3 L;
        float atten = 1.0;
        if (gl.params.y < 0.5)  // 定向光：照射方向 = -direction，无衰减
        {
            L = -gl.direction.xyz;
        }
        else                    // 点光：指向光源，线性衰减 clamp(1 - dist/range)
        {
            float3 toLight = gl.position.xyz - P;
            float dist = length(toLight);
            L = toLight / max(dist, 1e-4);
            atten = gl.params.x > 0.0 ? saturate(1.0 - dist / gl.params.x) : 0.0;
        }
        if (atten <= 0.0)
            continue;
        color += shadeLight(N, V, albedo, metallic, roughness, L, gl.color.rgb, atten);
    }
    return float4(color, 1.0);
}
