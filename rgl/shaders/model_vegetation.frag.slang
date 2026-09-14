import model;
import material;
import scene;
import env;
import normalmap;
import lights;

// Phase 8 植被专属 shader（forward-only，L2）：model_pbr_ibl 派生，新增**背光透射 SSS**。
// 叶片是薄半透叶片（thin translucent leaf），背光时光穿透叶肉使其透亮且高饱和——这是植被
// 区别于普通表面的最大视觉特征；此前与墙体共用 PBR 显得"太平"。路由：bistro 叶片材质
// MASK+doubleSided → 本 shader（main.cpp forward 路 styleMap 派生，见批次记录）。
//
// 无 thickness 贴图（bistro 叶片材质仅 baseColor/normal/orm/occlusion 四件套）→ 用 albedo
// 亮度近似厚度（亮=叶肉厚=透光多），透射色用 albedo² 提饱和偏暖（叶透红黄，物理上叶绿素
// 透射谱偏红）。distortion/power/strength 为 look-dev 常量，截图迭代。
//
// binding/import 与 model_pbr_ibl.frag 严格一致（复用 model.mesh.spv + 同 4 纹理槽 + sampler），
// descriptor 才不会错位。forward-only：deferred 走 deferred_geometry，本 shader 不参与 D1 对拍。

static const float TRANSMIT_DISTORTION = 0.20;  // 出射光沿法线散射：放宽背光带（非严格共轴也透光）
static const float TRANSMIT_POWER = 3.0;        // 背光集中度（越大越贴合严格背光方向）
static const float TRANSMIT_STRENGTH = 1.5;     // 透射整体强度（look-dev：2.2 严格逆光过曝，回退 1.5）

// 单灯 GGX 直接光贡献，返回 NdotL·(diffuse + specular)——公式抄自 model_pbr_ibl.frag
//（独立副本，不与其源码耦合；植被材质 metallic 恒 0，见 main）。
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

    // 植被漫反射 = half-Lambert wrapped diffuse（Valve HF2 植被经典、UE vegetation 同款）：
    // 标准 GGX saturate(NdotL) 让单层叶片卡背阴面直接光恰好为 0 → 死黑面；真实树冠靠冠内
    // 多层叶互射（多重散射）发亮，单层卡无此信息。wrap 把阳光绕到晨昏线背光侧，廉价模拟
    // 冠内散射。specular 保持标准 saturate（高光不该绕）。
    float wrap = max(pow(saturate(dot(N, L) * 0.5 + 0.5), 2.0), 0.3);  // 保底 0.3 = 冠内弹射光基色（纯 wrap 背光面仍死黑）
    float3 diffuseWrap = albedo * (1.0 - metallic) * wrap / 3.14159265;
    return NdotL * specular + diffuseWrap;
}

// 背光透射：L = 指向光源方向；光从叶背射入、穿透到叶正面到达眼睛 ⇔ V ≈ 光行进方向(-L)。
// 出射方向把 -L 向 N 折 distortion（叶内散射），pow 控制背光带宽度，thickness 用 albedo
// 亮度近似。返回项由调用方乘 atten·光源色·strength。
float3 shadeTransmit(float3 N, float3 V, float3 albedo, float3 L)
{
    float3 T = normalize(-L + TRANSMIT_DISTORTION * N);
    float backLit = pow(saturate(dot(V, T)), TRANSMIT_POWER);
    // 厚度用 albedo 亮度近似但保底：深色叶区（椴树贴图大片深棕 lum≈0.2）逆光也应透光，
    // 否则树冠成死黑剪影；亮叶透更多，保底 0.45 让整片叶层泛金黄。
    float thickness = 0.45 + 0.55 * saturate(dot(albedo, float3(0.333)));
    // 透光色用 albedo（albedo² 把深叶压得几乎不透光）；饱和/暖偏移靠 strength + tonemap 收。
    return albedo * backLit * thickness * TRANSMIT_STRENGTH;
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
    float metallic = 0.0;  // 植被恒电介质（无植被是金属），去金属镜面更自然
    float3 F0 = float3(0.04);
    float NdotV = max(dot(N, V), 1e-4);

    // IBL 环境项（漫反射 SH9 + 镜面 split-sum，同 model_pbr_ibl）
    float3 R = reflect(-V, N);
    float3 irradiance = irradianceSH(N);
    float3 envSpec = envMap.SampleLevel(envSampler, R, roughness * env.mipRange).rgb;
    float3 color = (irradiance * albedo * (1.0 - metallic)
        + envSpec * envBrdfFactor(NdotV, roughness, F0) * 0.0) * env.intensity;
    // 植被 env 镜面 = 0：F0=0.04 掠射 Fresnel→1 出完整天空镜像（×0.2 在绿篱垂直面掠射机位
    // 仍出"油亮"湿感，探针置 0 后 matte 自然——薄叶蜡质层无可分辨环境像，diffuse+透射即主体）。

    // 直接光多灯循环 + 背光透射（透射项 = 叶片被光照亮背面时的透光）
    uint n = readLightCount();
    if (n == 0)
    {
        float3 L = -light.direction;
        color += light.color * (shadeDirect(N, V, albedo, metallic, roughness, L)
            + shadeTransmit(N, V, albedo, L));
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
            color += atten * gl.color.rgb * (shadeDirect(N, V, albedo, metallic, roughness, L)
                + shadeTransmit(N, V, albedo, L));
        }
    }
    float3 emissive = od.emissive.rgb * emissiveTexture.Sample(baseColorSampler, input.uv).rgb;
    color += emissive;
    return float4(color, alpha);
}
