import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）
import scene;     // 场景级 bindless 数据（Phase 5：光照 + 对象矩阵表，binding 3）

// Phase 5 批次 3：简化 PBR（ambient + Lambert diffuse + 简化 GGX specular，Cook-Torrance
// 单平行光形态）。世界空间光照：normal/position 经对象矩阵表（DrawArgs.objectIndex）
// 变换；均匀缩放假定（法线取 mat3 直乘，见 scene.slang 注记）。
// roughnessMetallic：x=roughness y=metallic（Phase 4 契约字段，本批次接线消费）。
[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    float3 albedo = drawArgs.baseColor.rgb * texel.rgb;
    float alpha = drawArgs.baseColor.a * texel.a;

    SceneLight light = readSceneLight();
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

    // GGX 法线分布
    float a = roughness * roughness;
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float D = a2 / (3.14159265 * d * d + 1e-7);
    // Schlick-Fresnel
    float3 F = F0 + (1.0 - F0) * pow(1.0 - VdotH, 5.0);
    // 简化几何项（Smith Schlick 近似，k = a/2）
    float k = a * 0.5;
    float G = (NdotV / (NdotV * (1.0 - k) + k)) * (NdotL / (NdotL * (1.0 - k) + k));
    float3 specular = D * G * F / (4.0 * NdotV * NdotL + 1e-4);

    float3 diffuse = albedo * (1.0 - metallic) / 3.14159265;
    float3 color = light.ambient * albedo
        + light.color * NdotL * (diffuse + specular);
    return float4(color, alpha);
}
