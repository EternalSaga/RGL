import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）
import scene;     // 场景级 bindless 数据（Phase 5：光照 + 对象矩阵表，binding 3；④a irradianceSH/envBrdfFactor）
import env;       // 环境 cubemap（Phase 5.5：binding 4/5 + readEnvParams）
import normalmap; // Phase 8 方案 A：normal 贴图标准 TBN + DX flip-G 解码（binding 6/7 消费）
import lights;    // Phase 8 ③：前向多灯循环（sceneData 尾区 GpuLight[]，无新绑定）

// Phase 5.5：简化 PBR + IBL。direct light = 简化 Cook-Torrance；IBL 环境项 Phase 8 ④a 升级
// 为完整版（漫反射 SH9 辐照度 + 镜面 split-sum），direct light 项 Phase 8 ③ 升级为多灯循环
// （与 deferred light.frag 逐式同源）。均匀缩放假定 / 世界空间光照沿用 Phase 5 已知近似。

// 单灯 GGX 直接光贡献，返回 NdotL·(diffuse + specular)（调用方乘 atten·radiance）——
// 公式与 deferred light.frag shadeLight 严格同源（epsilon 相对化 1e-7·a2，见 教程拆解发现 ①）。
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
    const ObjectDraw od = readObjectDraw(drawArgs.objectIndex);  // Phase 7b+：材质进 heap
    float3 albedo = od.baseColor.rgb * texel.rgb;
    float alpha = od.baseColor.a * texel.a;
    // Phase 8 C1：MASK cutout（cutoff 在 od.emissive.w；0 = 不测 → 非 MASK 恒 false 逐位不变）
    if (od.alphaDiscard(alpha))
        discard;

    SceneLight light = readSceneLight();  // eyePos@12（共享）+ 头单灯（count=0 回落用）
    EnvParams env = readEnvParams();
    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 P = worldPos(M, input.positionOS);
    float3 geoN = worldNormal(M, normalize(input.normal));  // 几何法线（方案 C AA 用）
    float3 N = geoN;

    // Phase 8 ③a：normal 贴图（DX flip-G + Z 重建，中性法线纹理不动点）+ ORM
    //（G=roughness B=metallic，R=AO 采样不应用；白纹理回落材质 factor）
    float4 orm = ormTexture.Sample(baseColorSampler, input.uv);
    float3 tangentNormal = decodeNormalDx(normalTexture.Sample(baseColorSampler, input.uv));
    // Phase 8 方案 A：标准 TBN（CPU 烘切线 + GS + 手性，无导数）——掠射角白噪点根治
    N = applyNormalMapTBN(N, worldTangent(M, input.tangentOS), tangentNormal);
    // Phase 8 C1：doubleSided 背面法线翻向（cull 开时 frontFace 恒真 = 无副作用；C2 关 cull 后生效）
    if (!frontFace)
        N = -N;

    float3 V = normalize(light.eyePos - P);
    float roughness = clamp(od.roughnessMetallic.x * orm.g, 0.04, 1.0);
    // Phase 8 方案 C：geometric specular AA（几何法线导数估方差抬 roughness，掠射角白噪点）
    roughness = normalFiltering(roughness, geoN);
    float metallic = clamp(od.roughnessMetallic.y * orm.b, 0.0, 1.0);
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float NdotV = max(dot(N, V), 1e-4);

    // IBL 环境项（Phase 8 ④a：完整版——漫反射 = SH9 辐照度（scene.slang irradianceSH，CPU 从
    // HDR 余弦投影，上向面拿到"上方亮天空"、不再被地面平均拉暗）；镜面 = prefiltered env（现
    // mip 近似）× BRDF split-sum（envBrdfFactor 解析拟合，替代旧 (1-roughness) hack，粗糙金属
    // 不再塌黑）。env.intensity 统一缩两路。
    float3 R = reflect(-V, N);
    float3 irradiance = irradianceSH(N);
    float3 envSpec = envMap.SampleLevel(envSampler, R, roughness * env.mipRange).rgb;
    // Lambert 1/π：irradianceSH 输出真辐照度 E，出射 = E·albedo/π（与直接光 shadeDirect 的 albedo/π 一致；Filament 把该 1/π 烘进 CPU SH 系数，我们在求值端）
    float3 color = (irradiance / 3.14159265 * albedo * (1.0 - metallic)
        + envSpec * envBrdfFactor(NdotV, roughness, F0)) * env.intensity;

    // Phase 8 ③：直接光多灯循环（与 deferred light.frag 同源）。灯数组 = scene data 尾区
    // GpuLight[]；point（type=1）线性衰减 1-dist/range、dir（type=0）无衰减。count=0 回落
    // @0 头单灯（无显式 Light entity 的旧默认场景行为，保直接光不丢）。
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
            if (gl.params.y < 0.5)  // 定向光
            {
                L = -gl.direction.xyz;
            }
            else                    // 点光：指向光源，线性衰减
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
    // Phase 8 ③：自发光（emissiveFactor × emissiveTexture，HDR 叠加，tonemap 链收；无 emissive 材质 = 恒黑）
    float3 emissive = od.emissive.rgb * emissiveTexture.Sample(baseColorSampler, input.uv).rgb;
    color += emissive;
    return float4(color, alpha);
}
