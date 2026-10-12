import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）
import scene;     // 场景级 bindless 数据（Phase 5：光照 + 对象矩阵表，binding 3）
import scene_tables;  // 批 T1：逐对象四张表 StructuredBuffer 读取面（binding 23/25/26/33）

// Phase 5 批次 3：简化 PBR（ambient + Lambert diffuse + 简化 GGX specular，Cook-Torrance
// 单平行光形态）。世界空间光照：normal/position 经对象矩阵表（DrawArgs.objectIndex）
// 变换；均匀缩放假定（法线取 mat3 直乘，见 scene.slang 注记）。
// roughnessMetallic：x=roughness y=metallic（Phase 4 契约字段，本批次接线消费）。
[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    const ObjectDraw od = readObjectDraw(drawArgs.objectIndex);  // Phase 7b+：材质进 heap
    float3 albedo = od.baseColor.rgb * texel.rgb;
    float alpha = od.baseColor.a * texel.a;

    SceneLight light = readSceneLight();
    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 N = worldNormal(M, normalize(input.normal));
    float3 P = worldPos(M, input.positionOS);
    float3 V = normalize(light.eyePos - P);
    float3 L = -light.direction;  // 照射方向取反 = 指向光源

    float roughness = clamp(od.roughnessMetallic.x, 0.04, 1.0);
    float metallic = clamp(od.roughnessMetallic.y, 0.0, 1.0);
    float3 F0 = lerp(float3(0.04), albedo, metallic);

    float NdotL = saturate(dot(N, L));
    float NdotV = max(dot(N, V), 1e-4);
    float3 H = normalize(V + L);
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    // GGX 法线分布（epsilon 相对化 1e-7·a2：绝对 epsilon 在光滑端淹没真实分母
    // π·d²（r=0.04 时仅 ~2e-11），把高光峰值削掉 ~5000 倍且使 D(1) 对 roughness
    // 非单调——抛光金属变天鹅绒。教训见 docs/archive/教程拆解发现_20260902.md ①）
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
    // ambient 扣 metallic：金属无彩色漫反射（与 model_pbr_ibl 的 envDiffuse 项语义对齐，
    // 否则纯金属在两条路线的 ambient 处理不一致，截图对比会被误读成算法差异）
    float3 color = light.ambient * albedo * (1.0 - metallic)
        + light.color * NdotL * (diffuse + specular);
    return float4(color, alpha);
}
