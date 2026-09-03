import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）
import scene;     // 场景级 bindless 数据（Phase 5：光照 + 对象矩阵表，binding 3）
import env;       // 环境 cubemap（Phase 5.5：binding 4/5 + readEnvParams）

// Phase 5.5：简化 PBR + IBL（model_pbr 的环境光照升级路线）。direct light 项与
// model_pbr 完全一致（简化 Cook-Torrance），唯一差异 = 常 ambient 项替换为环境贴图
// 采样。简化形态（无 pre-filtered cubemap / BRDF LUT，完整版留 Phase 7）：
//   diffuse 环境项 ≈ 最高 mip 采样（1×1 ≈ 全环境平均，irradiance 近似）
//   specular 环境项 ≈ 反射向量 + roughness→LOD（mip 近似 prefilter，NEAREST blit 链）
//   能量补偿 = Schlick-F + (1−roughness) 衰减（BRDF LUT 的粗替代，重校留完整版）
// 均匀缩放假定 / 世界空间光照沿用 Phase 5 已知近似（AGENTS.md）。
[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    float3 albedo = drawArgs.baseColor.rgb * texel.rgb;
    float alpha = drawArgs.baseColor.a * texel.a;

    SceneLight light = readSceneLight();
    EnvParams env = readEnvParams();
    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 N = worldNormal(M, normalize(input.normal));
    float3 P = worldPos(M, input.positionOS);
    float3 V = normalize(light.eyePos - P);
    float3 L = -light.direction;  // 照射方向取反 = 指向光源

    float roughness = clamp(drawArgs.roughnessMetallic.x, 0.04, 1.0);
    float metallic = clamp(drawArgs.roughnessMetallic.y, 0.0, 1.0);
    float3 F0 = lerp(float3(0.04), albedo, metallic);

    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 1e-4);
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // GGX 法线分布（epsilon 相对化 1e-7·a2，与 model_pbr 一致：绝对 epsilon 在光滑端
    // 淹没真实分母把高光峰值削掉 ~5000 倍且使 D(1) 非单调，见 docs/教程拆解发现_20260902.md ①）
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d + 1e-7 * a2);
    // Schlick-Fresnel
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    // 简化几何项（Smith Schlick 近似，k = a/2）
    float k = a * 0.5;
    float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
    float3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-4);

    float3 diffuse = albedo * (1.0 - metallic) / 3.14159265;

    // IBL 环境项（替换 model_pbr 的 light.ambient * albedo）
    float3 R = reflect(-V, N);
    float3 envDiffuse = envMap.SampleLevel(envSampler, N, env.mipRange).rgb;
    float3 envSpec = envMap.SampleLevel(envSampler, R, roughness * env.mipRange).rgb;
    float3 Fenv = F0 + (1.0 - F0) * pow(1.0 - saturate(dot(N, V)), 5.0);
    float3 ambient = (envDiffuse * albedo * (1.0 - metallic)
        + envSpec * Fenv * (1.0 - roughness)) * env.intensity;

    float3 color = ambient
        + light.color * NdotL * (diffuse + specular);
    return float4(color, alpha);
}
