import scene;    // SceneLight/readSceneLight（共享 eyePos/ambient，头区 UBO binding 22）
import lights;   // 多灯：readLightCount/readGpuLight（lights 自带 binding 24 载体，不再 import scene）
import sky;      // 天穹射线重建：skyClipToWorld（读 @64 invViewProj）——sky 背景合成（@64 字段名 skyboxInvViewProj 的来历 = 已删的 forward skybox pass，契约字段保留）
import shadow;   // Phase 11 批 11.2：级联阴影采样（binding 12/13；forward 三个光照风格曾与本路共用，已随刀②.2 删除）

// Phase 7 D1：deferred light pass —— 全屏逐灯光照，读 G-buffer 4+1 RT 重建 albedo/法线/
// 世界位置/metal-rough，套用与已删除的 model_pbr.frag **逐式相同**（历史对偶，git history）的简化 PBR（GGX Cook-Torrance）。
// Phase 7 D3：升级为多灯循环——灯数据源 = scene data 尾区 GpuLight[]（lights.slang）；
// 共享 eyePos(@12)/ambient(@36) 仍来自 scene.slang readSceneLight（与已删除的 forward 路同源）。
// 点光（type=1）线性衰减 1-dist/range（无物理 1/r²，够用）；定向（type=0）atten=1。
//
// 批次 2 refine（Phase 7 收尾）：
//   ① 简化 IBL——env.mipRange>0 时用环境贴图项替换常 ambient（与已删除的 model_pbr_ibl.frag 逐式
//      相同：diffuse=最高 mip 全环境平均、specular=反射向量+roughness→LOD、Schlick-F
//      能量补偿）；mipRange=0（无环境）回落常 ambient（旧行为，BIT 兼容）。
//   ② skybox 背景合成——天空像素（G-buffer position 清 0 = 无几何）重建世界射线采 env
//      cubemap（与已删除的 forward skybox.frag 同式，免 resolution）；mipRange=0 或 intensity=0 = 黑天。
//
// 绑定：（批 N+1 起 3 号位**空出**——sceneData 全区读已退役）；4=albedo(RGBA8_SRGB)；
//      5=normal(RG16_SNORM oct)；
//   6=worldPos(RGBA32_SFLOAT)；7=metalRough(RGBA8_UNORM .r=metal .g=rough)；8=sampler（共享）；
//   9=envMap(TextureCube) / 10=envSampler(sky sampler)（批次 2 refine 新增）；
//   **11=emissive(RGBA16_SFLOAT)**（Phase 8 剩余批次 B：G-buffer 第 5 通道——排在尾部是因
//   4-10 已被旧布局占用，勿按 kGBufferAttachments 序推断 binding）。4-11 经
//   PipelineBindings.extra 映射到 G-buffer RT / env 堆槽。
// 注：不 import env.slang（其 envMap/envSampler 声明在 4/5，与本 pass G-buffer 冲突）——
//   env 绑定本地声明在 9/10，env 参数走 scene.slang 的 readEnvIntensity / readEnvMipRange。

[[vk::binding(4, 0)]] Texture2D<float4> albedoTex;
[[vk::binding(5, 0)]] Texture2D<float2> normalTex;
[[vk::binding(6, 0)]] Texture2D<float4> positionTex;
[[vk::binding(7, 0)]] Texture2D<float4> metalRoughTex;
[[vk::binding(8, 0)]] SamplerState gbufferSampler;
[[vk::binding(9, 0)]] TextureCube<float4> envMapL;
[[vk::binding(10, 0)]] SamplerState envSamplerL;
[[vk::binding(11, 0)]] Texture2D<float4> emissiveTex;
// 批 11.3-1e：IZB 消费 = **变体编译**——以下整块只存在于 -DIZB_CONSUME 变体 SPV
// （rgl_shader_defined，registry 按 izb_shadow 门选 spec）。无 -D 原版 = 1e 之前
// token 流 = 门关逐位红线构造性保证（运行时 flag 分支挡不住 slang 对源码差异的
// fp 重排——2026-10-02 实测 105401 像素漂移教训）。
#ifdef IZB_CONSUME
[[vk::binding(14, 0)]] ByteAddressBuffer izbMask;

// IZB 消费（批 11.3-1e，v1.5 起含覆盖外回落）：方向光硬阴影因子。参数 = ShadowParams
// 的 izb 字段（迁移批 S-2 起经 scene.slang `readShadowIzb()` 取，原手抄
// `sceneData.Load4(723456u)` + scene_data.cppm static_assert 的手写同步点已随之消失）
// = (flag, W, H, r)。
// flag=0 → 原样 sampleShadow1Tap（CSM 路径**逐位透传** = 门关红线）；flag=1 →
// cascade-0 IZB mask 接管方向光阴影（bit = 帧缓冲像素，与 izb_create 的 py·W+px
// 同向免翻转）。**patch 语义（v1.5，2026-10-02 用户拍板①）**：P 的光空间 uv 出
// cascade-0 盒 → 回落 sampleShadow1Tap（CSM 软影兜底）而非返回 1.0——c0 盒贴相机
// （CSM 本职），旧「界外无影」让影区边界随相机扫动 = 用户所见"影子游动"（swim 判据
// 结案，git:调试历史 2026-10-02 补账）；回落把边界变成硬→软过渡，影子永不整体消失。
// 替换而非相乘的实测依据：同一太阳下 IZB 遮挡集 ⊆ CSM 遮挡集，相乘画面零差异
// （补集判别 +177815 像素反证通路全通）。uv = light.mesh fb 归一化 y 上→下。
float izbShadowFactor(float3 P, float3 N, float2 uv)
{
    // ⚠️ 编译期陷阱（2026-10-02「门开无影」根因，SPV 反汇编实录）：
    // ByteAddressBuffer.Load 参数 = 字节偏移（与 izb_mask.frag InterlockedOr
    // 同口径）——传字索引 pid>>5 会被再 ÷4，读位置偏 4 倍。（当年同处的 sceneData
    // 浮点读数需 asfloat() 那条陷阱已随迁移批 S-2 消失：izb 现在是 ConstantBuffer
    // 的 float4 字段，不经 Load4，没有"位模式当整数转浮点"的降低路径。）
    const float4 izb = readShadowIzb();
    if (izb.x < 0.5)
        return sampleShadow1Tap(P, N);
    // 覆盖测试与 izb_create 同源：cascade-0 光空间（正交 w=1）；P = G-buffer 世界位
    const float2 luv = clipPos(readShadowViewProj(0u), P).xy * 0.5 + 0.5;
    if (luv.x < 0.0 || luv.x >= 1.0 || luv.y < 0.0 || luv.y >= 1.0)
        return sampleShadow1Tap(P, N);
    const uint pid = uint(uv.y * izb.z) * uint(izb.y) + uint(uv.x * izb.y);
    return (izbMask.Load((pid >> 5u) * 4u) & (1u << (pid & 31u))) != 0u ? 0.0 : 1.0;
}
#endif

// env 参数（头区契约字段 SceneHeaderBlock::envParams.xy，scene.slang readEnvIntensity /
// readEnvMipRange 共用读取面；本结构只是本地成对取值，无字节偏移）
struct EnvParamsL
{
    float intensity;
    float mipRange;
};
EnvParamsL readEnvParamsL()
{
    EnvParamsL e;
    e.intensity = readEnvIntensity();
    e.mipRange  = readEnvMipRange();
    return e;
}

struct LightInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

// octDecode（G-buffer oct 法线解码）批 15.1 后半起走 `import scene;` 单一来源——
// 本文件曾是唯一消费者，ssr.frag 是第二个；抬升理由与逐字搬移纪律见 scene.slang
// 「屏幕 ↔ 世界重建」段头（clipToWorld 先例）。

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

// Phase 8 批次 F：植被单灯贡献（对齐已删除的 forward model_vegetation 的 shadeDirect + shadeTransmit，
// 独立副本不与其源码耦合）：漫反射 = half-Lambert wrap（保底 0.3 = 冠内弹射），叠加背光透射
//（出射向 T=normalize(-L+0.2N)、backLit=pow(dot(V,T),3)、thickness 用 albedo 亮度近似保底 0.45）；
// specular 保持标准 saturate（高光不该绕）。metallic 恒 0（调用方已置）。
float3 shadeLightVeg(float3 N, float3 V, float3 albedo, float roughness,
                     float3 L, float3 radiance, float atten)
{
    float3 F0 = float3(0.04);
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

    float wrap = max(pow(saturate(dot(N, L) * 0.5 + 0.5), 2.0), 0.3);
    float3 diffuseWrap = albedo * wrap / 3.14159265;

    float3 T = normalize(-L + 0.20 * N);
    float backLit = pow(saturate(dot(V, T)), 3.0);
    float thickness = 0.45 + 0.55 * saturate(dot(albedo, float3(0.333)));
    float3 transmit = albedo * backLit * thickness * 1.5;

    return atten * radiance * (NdotL * specular + diffuseWrap + transmit);
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
    // Phase 8 批次 F：材质类别 = emissive.a 通道（deferred_geometry 类别变体写入，clear 0 = 标准
    // PBR）。1=植被 / 2=光源 → 强制电介质（已删除的 forward model_vegetation/model_light 均 metallic=0）。
    // 类别 0 恒不取分支 → 标准像素算式逐位不动（D1 基线保持）。
    float4 emis = emissiveTex.Sample(gbufferSampler, uv);
    const float materialClass = emis.a;
    const bool isVeg = materialClass > 0.5 && materialClass < 1.5;
    const bool isLamp = materialClass > 1.5 && materialClass < 2.5;
    // class 3（2026-10-18）：unlit = baseColor 原样出像素（glTF KHR_materials_unlit），
    // 对齐已删除的 forward model.frag（历史对偶）：无光照、无自发光相加、无阴影。此前 deferred 没有 unlit 路
    //（deferred_styles 缺该风格 → 静默回落 deferred_geometry 被当 PBR 打光 = D1 破口，
    // 探针重立时由 aa-coverage 电平自检抓出，docs/图变体收敛_plan.md）。早退在 env 采样前
    // 安全：SampleLevel 显式 LOD 不取偏导（上方 ② 的"现行理由"），sky 分支本就同款早退。
    const bool isUnlit = materialClass > 2.5;
    if (isUnlit)
        return float4(albedo, 1.0);
    if (isVeg || isLamp)
        metallic = 0.0;

    SceneLight shared = readSceneLight();     // 共享项：eyePos@12、ambient@36
    float3 V = normalize(shared.eyePos - P);
    EnvParamsL env = readEnvParamsL();        // @48 intensity / @52 mipRange

    // ② skybox 背景合成：天空像素 = G-buffer position 清 0（无几何，dot(P,P)=0）。
    // 重建世界射线（与已删除的 forward skybox.frag 同式：near/far 两点差，免 resolution）采 env。
    // uv→ndc：light.mesh 写 uv=(ndc.x*0.5+0.5, 0.5-ndc.y*0.5)（framebuffer Y 翻转）→ 反解。
    // env 采样必须在 sky/几何分支**之前**（全 quad 活跃）。**这条约束的历史前提与现行理由**：
    // ① 历史（2026-09-13）：本行当时是隐式 `Sample()`，隐式 LOD 放进分支会让天空/几何混
    //    quad 的几何 lane 提前 return = partial quad 偏导未定义 → 边界天空像素 mip 漂移
    //   （对拍石锤 + red-probe 证实差异像素全走 sky 分支）。
    // ② 现行（2026-10-05）：本行已改 `SampleLevel(…, 0.0)`，**显式 LOD 不取偏导**，①的
    //    机制在结构上不存在了。位置仍留在分支前，只剩"两路共用一条语句、少一份重复"的理由。
    // ⚠ **改写自一条曾被当成普适结论的旧断言**（原文：「SampleLevel(0) 全局变差证实
    //    forward 隐式 LOD ≠ 固定 mip」）。那条对拍的**前提**是「deferred 隐式 LOD、forward
    //    固定 mip」两路**不对称**——彼时 env sampler 槽号与 AO point sampler 相撞
    //   （`getReservedSlots()+1` 双写，AO 的 maxLod=0 后写覆盖），deferred 的隐式 `Sample()`
    //    实际被钳在 mip0，于是"改成 SampleLevel(0)"改变的是**唯一那一路的对称性**，
    //    测到的"变差"是**打破不对称**的结果，不是"隐式 LOD 等价于某个固定 mip"的证据。
    //    槽号相撞 2026-10-05 修好后，两路都拿到真 mip 链，隐式 LOD 会按方向向量偏导
    //    选到非零 mip ⟹ 天穹糊 49%（全图梯度 5.939 → 3.024）。**结论：天穹必须显式 mip0，
    //    且当时两路同时改才对称**（forward skybox.frag 同步；该文件已随刀②.2 删除，
    //    这条约束今天只剩 light.frag 一处）。判据史 = D1 两路对等。
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 far  = skyClipToWorld(float4(ndc, 1.0, 1.0));
    float4 near = skyClipToWorld(float4(ndc, 0.0, 1.0));
    float3 dir  = normalize(far.xyz / far.w - near.xyz / near.w);
    // 天穹 = **锐利**环境辐射。env 立方图 mip 1..L−1 是 GGX 预滤波（按 roughness 糊，
    // env_prefilter.comp.slang:25「level 0 = sharp 源不生成，1..L−1 生成」），**不是**
    // 分辨率金字塔 ⟹ 任何隐式 LOD 拿到的都是"按粗糙度糊"的结果，天穹必显式取 mip0。
    float3 envSky = envMapL.SampleLevel(envSamplerL, dir, 0.0).rgb;
    if (dot(P, P) < 1e-10)
    {
        // 天穹在场 = env 资产已卷积（mipRange>0）**且 env 总闸开**（intensity>0）。
        // ⚠ 2026-10-18 加读 intensity（探针重立定案）：本分支此前只门控 mipRange ⟹
        // 「intensity=0 关环境」在 deferred 上**永远不归零**天穹（forward 由 skybox 标志
        // 关天穹，该标志是 forward draw 侧 knob，随 forward 一起删）——imgqa 合成 profile
        // 的 temporal 抑制比 / aa-coverage 电平被逐相位变化的天穹污染实测抓出。
        // 语义定案：intensity **不乘**天穹亮度（它是 env 总闸 + IBL 标定系数，不是显示增益），
        // 但 =0 时天穹与 IBL 一起关（本引擎里没人要「IBL 全关只剩天穹」的组合；
        // 反向论证 = 本行改动前 deferred 连「env 全关」都表达不了）。
        if (env.mipRange > 0.0 && env.intensity > 0.0)
            return float4(envSky, 1.0);
        return float4(0.0, 0.0, 0.0, 1.0);  // 无环境 = 黑天（旧行为：清 0 G-buffer 光照亦黑）
    }

    // 几何像素 ambient 项（Phase 8 批次 ④a 完整版 IBL，与已删除的 forward model_pbr_ibl 逐式同源）：
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
        // 植被 env 镜面 = 0（同已删除的 forward model_vegetation：掠射天空镜像 = 油亮湿感，探针定案）
        // Lambert 1/π：irradianceSH 输出真辐照度 E，出射 = E·albedo/π（与直接光 shadeLight 的 albedo/π 一致；Filament 把该 1/π 烘进 CPU SH 系数，我们在求值端）
        color = (irradiance / 3.14159265 * albedo * (1.0 - metallic)
            + envSpec * envBrdfFactor(NdotV, roughness, F0) * (isVeg ? 0.0 : 1.0)) * env.intensity;
    }
    else
    {
        color = shared.ambient * albedo * (1.0 - metallic);
    }

    // Phase 11 批 11.2：阴影因子。当年**与 forward 三个光照风格同体**（同一份 shadow.slang，
    // forward 已随刀②.2 删除）——11.0c 的教训：两路只要有一路没接就是“只有一路有”的破口。
    // 只乘直接光（环境光 IBL 无方向 → 不乘；自发光不乘）。
    // 批 11.3-1e：× IZB 近场硬阴影（cascade-0 mask；门/flag 关 = ×1.0 逐位旁路）。
#ifdef IZB_CONSUME
    const float shadowFactor = izbShadowFactor(P, N, uv);
#else
    const float shadowFactor = sampleShadow1Tap(P, N);
#endif
    uint n = readLightCount();
    if (n == 0)
    {
        // count=0 回落 @0 头单灯（与已删除的 forward model_pbr_ibl/model_light/model_vegetation 同款；
        // shadeLightVeg 已含植被透射项 ≈ 其 forward 回落的 shadeDirect+shadeTransmit）
        float3 L0 = -shared.direction;
        color += (isVeg ? shadeLightVeg(N, V, albedo, roughness, L0, shared.color, 1.0)
                        : shadeLight(N, V, albedo, metallic, roughness, L0, shared.color, 1.0))
            * shadowFactor;
    }
    else
    {
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
            if (isVeg)
                color += shadeLightVeg(N, V, albedo, roughness, L, gl.color.rgb, atten) * shadowFactor;
            else
                color += shadeLight(N, V, albedo, metallic, roughness, L, gl.color.rgb, atten)
                    * shadowFactor;
        }
    }
    // Phase 8 剩余批次 B：自发光相加（deferred_geometry SV_Target4 写入的第 5 G-buffer 通道，
    // 与已删除的 forward model_pbr_ibl 的 od.emissive.rgb × emissiveTexture 相加项同源；天空分支已提前返回）
    // Phase 8 批次 F：光源类 ×1/4 = 还原 strength 预乘（同已删除的 forward model_light，暗绿玻璃定案）
    color += emis.rgb * (isLamp ? 0.25 : 1.0);
    return float4(color, 1.0);
}
