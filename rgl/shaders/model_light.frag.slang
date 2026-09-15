import model;
import material;
import scene;
import env;
import normalmap;
import lights;

// Phase 8 光源本色 shader（forward-only）：bistro 灯具材质（emissive 非零：串灯/路灯/店招/
// 聚光玻璃）路由到本 shader。
//
// 问题：bistro 132 材质 131 个带 metallicRoughnessTexture 走金属路，灯泡被当抛光镜面——
// F0=albedo 的环境镜像 + 锐高光把它读成"镀铬球发光"，emissive 被高光压过，看不到灯的本来
// 颜色。真实光源表面（玻璃壳/灯丝/荧光粉）= **朗伯发射体 + 电介质，镜面反射近乎零**。
//
// 设计（物理动机）：
//   ① metallic 恒 0 → F0=0.04 固定，去掉金属那层 baseColor 染色强反射（"金属球"主因）；
//   ② emission × (1/strength) 还原到 glTF 原值——strength=4.0 是 C 批次 look-dev 猜值、对白天过强
//      （路灯 baseColor 纯黑 + 绿 emissive 纹理 → 绿通道 2.0 = 纯绿霓虹），还原后灯罩显本色不炸；
//   ③ 保留漫反射 + F0=0.04 玻璃反射 → 本体色 = albedo + 弱玻璃 Fresnel，tonemap 链收。
//
// binding/import 与 model_pbr_ibl.frag 严格一致（复用 model.mesh.spv + 同 4 纹理槽 + sampler）。
// forward-only：deferred 走 deferred_geometry，本 shader 不参与 D1 对拍。

float3 shadeDirect(float3 N, float3 V, float3 albedo, float metallic, float roughness, float3 L)
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
    return NdotL * (diffuse + specular);
}

[shader("fragment")]
float4 main(ModelVertexOutput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    const ObjectDraw od = readObjectDraw(drawArgs.objectIndex);
    float3 albedo = od.baseColor.rgb * texel.rgb;
    float alpha = od.baseColor.a * texel.a;
    if (od.alphaDiscard(alpha))
        discard;

    SceneLight light = readSceneLight();
    EnvParams env = readEnvParams();
    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 P = worldPos(M, input.positionOS);
    float3 geoN = worldNormal(M, normalize(input.normal));
    float3 N = geoN;

    float4 orm = ormTexture.Sample(baseColorSampler, input.uv);
    float3 tangentNormal = decodeNormalDx(normalTexture.Sample(baseColorSampler, input.uv));
    N = applyNormalMapTBN(N, worldTangent(M, input.tangentOS), tangentNormal);
    if (!frontFace)
        N = -N;

    float3 V = normalize(light.eyePos - P);
    float roughness = clamp(od.roughnessMetallic.x * orm.g, 0.04, 1.0);
    roughness = normalFiltering(roughness, geoN);
    float metallic = 0.0;  // ① 光源表面是电介质，非金属——去掉 chrome-ball 镜面
    float3 F0 = float3(0.04);
    float NdotV = max(dot(N, V), 1e-4);

    // IBL 环境项（漫反射 SH9 + 镜面 split-sum；金属度=0 后镜面本就很弱）
    float3 R = reflect(-V, N);
    float3 irradiance = irradianceSH(N);
    float3 envSpec = envMap.SampleLevel(envSampler, R, roughness * env.mipRange).rgb;
    // Lambert 1/π：irradianceSH 输出真辐照度 E，出射 = E·albedo/π（与直接光 shadeDirect 的 albedo/π 一致；Filament 把该 1/π 烘进 CPU SH 系数，我们在求值端）
    float3 color = (irradiance / 3.14159265 * albedo * (1.0 - metallic)
        + envSpec * envBrdfFactor(NdotV, roughness, F0)) * env.intensity;

    uint n = readLightCount();
    if (n == 0)
    {
        color += light.color * shadeDirect(N, V, albedo, metallic, roughness, -light.direction);
    }
    else
    {
        for (uint i = 0; i < n; ++i)
        {
            GpuLight gl = readGpuLight(i);
            float3 L;
            float atten = 1.0;
            if (gl.params.y < 0.5)
            {
                L = -gl.direction.xyz;
            }
            else
            {
                float3 toLight = gl.position.xyz - P;
                float dist = length(toLight);
                L = toLight / max(dist, 1e-4);
                atten = gl.params.x > 0.0 ? saturate(1.0 - dist / gl.params.x) : 0.0;
            }
            if (atten <= 0.0)
                continue;
            color += atten * gl.color.rgb * shadeDirect(N, V, albedo, metallic, roughness, L);
        }
    }

    // ② emission look-dev：strength=4.0（C 批次回填的 look-dev 猜值）对白天场景过强——实测
    //    路灯 baseColor=纯黑、绿 100% 来自 emissive 绿纹理，od.emissive=0.5×4=2.0 乘绿纹理后
    //    绿通道 1.47，若独占像素 = 纯绿霓虹。金属球版是黑金属底把绿压暗了才"看着还行"。
    //    本 shader 按 1/strength 把 emission 还原到 glTF 原值 0.5（=strength 语义的"未加强"），
    //    灯罩显 emissive 本色 + 保留玻璃反射（F0=0.04），白天不发光成霓虹。forward-only 不碰 D1。
    float3 emission = od.emissive.rgb * emissiveTexture.Sample(baseColorSampler, input.uv).rgb;
    color += emission * (1.0 / 4.0);
    return float4(color, alpha);
}
