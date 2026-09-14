import scene;    // binding 3：sceneData + SceneLight/readSceneLight（共享 eyePos/ambient）
import lights;   // 多灯：readLightCount/readGpuLight（lights 内部亦 import scene）
import sky;      // skybox 射线重建：skyClipToWorld（读 @64 invViewProj）——skybox 背景合成

// Phase 7 D1：deferred light pass —— 全屏逐灯光照，读 G-buffer 4+1 RT 重建 albedo/法线/
// 世界位置/metal-rough，套用与 model_pbr.frag **逐式相同**的简化 PBR（GGX Cook-Torrance）。
// Phase 7 D3：升级为多灯循环——灯数据源 = scene data 尾区 GpuLight[]（lights.slang）；
// 共享 eyePos(@12)/ambient(@36) 仍来自 scene.slang readSceneLight（与 forward 同源）。
// 点光（type=1）线性衰减 1-dist/range（无物理 1/r²，够用）；定向（type=0）atten=1。
//
// 批次 2 refine（Phase 7 收尾）：
//   ① 简化 IBL——env.mipRange>0 时用环境贴图项替换常 ambient（与 model_pbr_ibl.frag 逐式
//      相同：diffuse=最高 mip 全环境平均、specular=反射向量+roughness→LOD、Schlick-F
//      能量补偿）；mipRange=0（无环境）回落常 ambient（旧行为，BIT 兼容）。
//   ② skybox 背景合成——天空像素（G-buffer position 清 0 = 无几何）重建世界射线采 env
//      cubemap（与 forward skybox.frag 同式，免 resolution）；mipRange=0 = 黑天。
//
// 绑定：3=sceneData（scene.slang import）；4=albedo(RGBA8_SRGB)；5=normal(RG16_SNORM oct)；
//   6=worldPos(RGBA32_SFLOAT)；7=metalRough(RGBA8_UNORM .r=metal .g=rough)；8=sampler（共享）；
//   9=envMap(TextureCube) / 10=envSampler(sky sampler)（批次 2 refine 新增）；
//   **11=emissive(RGBA16_SFLOAT)**（Phase 8 剩余批次 B：G-buffer 第 5 通道——排在尾部是因
//   4-10 已被旧布局占用，勿按 kGBufferAttachments 序推断 binding）。4-11 经
//   PipelineBindings.extra 映射到 G-buffer RT / env 堆槽。
// 注：不 import env.slang（其 envMap/envSampler 声明在 4/5，与本 pass G-buffer 冲突）——
//   env 绑定本地声明在 9/10，env 参数（@48/@52）从 sceneData 直读（env.slang 同款布局）。

[[vk::binding(4, 0)]] Texture2D<float4> albedoTex;
[[vk::binding(5, 0)]] Texture2D<float2> normalTex;
[[vk::binding(6, 0)]] Texture2D<float4> positionTex;
[[vk::binding(7, 0)]] Texture2D<float4> metalRoughTex;
[[vk::binding(8, 0)]] SamplerState gbufferSampler;
[[vk::binding(9, 0)]] TextureCube<float4> envMapL;
[[vk::binding(10, 0)]] SamplerState envSamplerL;
[[vk::binding(11, 0)]] Texture2D<float4> emissiveTex;

// env 参数（scene data @48 intensity / @52 mipRange，env.slang readEnvParams 同款布局直读）
struct EnvParamsL
{
    float intensity;
    float mipRange;
};
EnvParamsL readEnvParamsL()
{
    EnvParamsL e;
    e.intensity = asfloat(sceneData.Load(48));
    e.mipRange  = asfloat(sceneData.Load(52));
    return e;
}

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
    EnvParamsL env = readEnvParamsL();        // @48 intensity / @52 mipRange

    // ② skybox 背景合成：天空像素 = G-buffer position 清 0（无几何，dot(P,P)=0）。
    // 重建世界射线（与 forward skybox.frag 同式：near/far 两点差，免 resolution）采 env。
    // uv→ndc：light.mesh 写 uv=(ndc.x*0.5+0.5, 0.5-ndc.y*0.5)（framebuffer Y 翻转）→ 反解。
    // env 采样必须在 sky/几何分支**之前**（全 quad 活跃）：隐式 LOD 采样放在分支内时，
    // 天空/几何混 quad 的几何 lane 提前 return = partial quad 偏导未定义 → 边界天空像素
    // mip 漂移（2026-09-13 对拍石锤；red-probe 证实差异像素全走 sky 分支、SampleLevel(0)
    // 全局变差证实 forward 隐式 LOD ≠ 固定 mip）。方向向量的偏导与几何无关 = 等价 forward
    // skybox pass（其天空盒大三角覆盖全屏恒全 quad）。不乘 env.intensity——forward
    // skybox.frag 输出裸 env radiance（intensity 是 IBL 光照标定系数不是显示增益，
    // 本分支曾乘 intensity，②续重校准 0.2 起破坏 D1，本次一并以 forward 为准移除）。
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 far  = skyClipToWorld(float4(ndc, 1.0, 1.0));
    float4 near = skyClipToWorld(float4(ndc, 0.0, 1.0));
    float3 dir  = normalize(far.xyz / far.w - near.xyz / near.w);
    float3 envSky = envMapL.Sample(envSamplerL, dir).rgb;
    if (dot(P, P) < 1e-10)
    {
        if (env.mipRange > 0.0)
            return float4(envSky, 1.0);
        return float4(0.0, 0.0, 0.0, 1.0);  // 无环境 = 黑天（旧行为：清 0 G-buffer 光照亦黑）
    }

    // 几何像素 ambient 项（Phase 8 批次 ④a 完整版 IBL，与 forward model_pbr_ibl 逐式同源）：
    // 漫反射 = SH9 辐照度 irradianceSH(N)（scene.slang），镜面 = prefiltered env × BRDF
    // split-sum（envBrdfFactor）。env.mipRange=0（无环境）回落常 ambient（旧行为 BIT 兼容）。
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float NdotV = max(dot(N, V), 1e-4);
    float3 color;
    if (env.mipRange > 0.0)
    {
        float3 R = reflect(-V, N);
        float3 irradiance = irradianceSH(N);
        float3 envSpec    = envMapL.SampleLevel(envSamplerL, R, roughness * env.mipRange).rgb;
        color = (irradiance * albedo * (1.0 - metallic)
            + envSpec * envBrdfFactor(NdotV, roughness, F0)) * env.intensity;
    }
    else
    {
        color = shared.ambient * albedo * (1.0 - metallic);
    }

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
    // Phase 8 剩余批次 B：自发光相加（deferred_geometry SV_Target4 写入的第 5 G-buffer 通道，
    // 与 forward model_pbr_ibl 的 od.emissive.rgb × emissiveTexture 相加项同源；天空分支已提前返回）
    color += emissiveTex.Sample(gbufferSampler, uv).rgb;
    return float4(color, 1.0);
}
