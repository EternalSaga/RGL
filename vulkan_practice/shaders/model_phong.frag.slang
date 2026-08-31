import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）
import scene;     // 场景级 bindless 数据（Phase 5：光照 + 对象矩阵表，binding 3）

// Phase 5 批次 3：基础 Phong（ambient + Lambert diffuse + Blinn 近似高光），与
// model_pbr（简化 PBR）并存的第二条光照路线——同 DrawArgs 契约、同 mesh SPV，
// 切换只改 scene.lua styles.pbr = "model_phong" 一行（管线表数据驱动，零 C++ 改动）。
// roughnessMetallic 借用语义：shininess = mix(128, 4, roughness)，metallic 仅染高光色。
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
    float3 L = -light.direction;

    float roughness = clamp(drawArgs.roughnessMetallic.x, 0.0, 1.0);
    float metallic = clamp(drawArgs.roughnessMetallic.y, 0.0, 1.0);
    float shininess = lerp(128.0, 4.0, roughness);
    float3 specColor = lerp(float3(1.0), albedo, metallic);

    float NdotL = saturate(dot(N, L));
    float3 H = normalize(V + L);
    float spec = pow(saturate(dot(N, H)), shininess) * step(1e-4, NdotL);

    float3 color = light.ambient * albedo
        + light.color * (albedo * NdotL + specColor * spec * NdotL);
    return float4(color, alpha);
}
