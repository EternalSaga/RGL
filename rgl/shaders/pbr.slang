// pbr.slang — GGX 直接光 + IBL 环境光 + 多灯循环（**deferred 与透明共用的那一份着色核**）
//
// 2026-10-18 透明混合管线刀0（docs/透明混合管线_plan.md §1-2）自 light.frag **搬字不改式**抽出：
// 表达式文本、运算次序、累加次序逐字保留。为什么必须抽而不是再抄一份——本项目历史上
// 「两份实现 + 严格同源注释」的形态已经出现过一次（已删的 forward model_pbr_ibl.frag 与本文件
// 的关系，注释原话「逐式相同」），而注释不阻止漂移（AGENTS 项目哲学 1）。透明 pass 只能前向着色
// （一像素一套 G-buffer ⟹ deferred 表达不了多层混合），它需要的正是这同一套算式。
//
// 🔴 **累加次序是语义，不是风格**：shadeLights 收 ambient 作初值、逐灯 `color +=`，
// 与抽出前 main 里的 `color = ambient; for: color += …` 逐次同序。若改成「先累加直接光、
// 最后加 ambient」，浮点结合律不成立 ⟹ 逐位漂移（`just regress ab` 会红）。
//
// 🔴 **不 import env.slang**（同 light.frag 当年的理由）：env.slang 把 envMap/envSampler 声明在
// binding 4/5，而 deferred light pass 的 4/5 是 G-buffer 的 albedo/normal。解法 = 本文件**一个绑定
// 都不声明**，env 资源经**函数参数**传入（实测 Slang 支持资源类型作函数参数，且调用方各自的
// [[vk::binding]] 号原样保留进 SPIR-V）⟹ light.frag 传它的 9/10、透明 frag 传它的 4/5。
//
// 🔴 **不碰阴影**：IZB 消费（ByteAddressBuffer izbMask + izbShadowFactor）留在 light.frag——
// 它是 light pass 专属形态，且 R7 护栏对「新增 ByteAddressBuffer 声明」是 ERROR 级。
// 本文件收的是**已经算好的 shadowFactor 标量**，各调用方自己决定怎么得到它。

import scene;    // irradianceSH / envBrdfFactor / readSceneLight（SceneLight）/ readEnvIntensity / readEnvMipRange
import lights;   // 多灯：readLightCount / readGpuLight（lights 自带 binding 24 载体）

// env 参数（头区契约字段 SceneHeaderBlock::envParams.xy，scene.slang 共用读取面；本结构只是
// 本地成对取值，无字节偏移）。名字与字段承自 env.slang 的同名结构——刀0 把**读取面**收进着色核
//（谁算 IBL 谁读参数），env.slang 自此只剩 binding 4/5 的声明。
// 🔴 **全仓只准这一份** `readEnvParams()`：scene.slang 当年就是为躲开 env.slang 的同名函数
// 才只提供两个标量读取（写 `readEnvParams().x` 会 ambiguous overload，见 scene.slang 注释）。
// 再造第二份 = 同时 import 两侧的文件当场编译失败——那是**好事**，别靠改名绕开。
struct EnvParams
{
    // 环境强度（scene.lua environment.intensity）。**作用点在 GPU 端**：采样后乘（env 颜色只存在于
    // GPU，无法像定向光那样 CPU 预乘进 SceneLight::color）——与定向光 intensity 是同一类调光参数的
    // 两种落地形态（git:教程拆解发现_20260902.md ⑤）。
    float intensity;
    float mipRange;   // 可采样 mip 跨度（L−1，roughness→LOD 的满量程）
};
EnvParams readEnvParams()
{
    EnvParams e;
    e.intensity = readEnvIntensity();
    e.mipRange  = readEnvMipRange();
    return e;
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

// 植被单灯贡献（对齐已删除的 forward model_vegetation 的 shadeDirect + shadeTransmit，
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

// 环境光项（Phase 8 批次 ④a 完整版 IBL）：漫反射 = SH9 辐照度 irradianceSH(N)（scene.slang），
// 镜面 = prefiltered env × BRDF split-sum（envBrdfFactor）。env.mipRange=0（无环境）回落常
// ambient（旧行为 BIT 兼容）。植被 env 镜面 = 0（掠射天空镜像 = 油亮湿感，探针定案）。
// Lambert 1/π：irradianceSH 输出真辐照度 E，出射 = E·albedo/π（与直接光 shadeLight 的 albedo/π
// 一致；Filament 把该 1/π 烘进 CPU SH 系数，我们在求值端）。
float3 shadeAmbient(TextureCube<float4> envMap, SamplerState envSampler,
                    float3 N, float3 V, float3 albedo, float metallic, float roughness,
                    bool isVeg, EnvParams env, float3 ambientConstant)
{
    float3 F0 = lerp(float3(0.04), albedo, metallic);
    float NdotV = max(dot(N, V), 1e-4);
    if (env.mipRange > 0.0)
    {
        float3 R = reflect(-V, N);
        float3 irradiance = irradianceSH(N);
        float3 envSpec    = envMap.SampleLevel(envSampler, R, roughness * env.mipRange).rgb;
        return (irradiance / 3.14159265 * albedo * (1.0 - metallic)
            + envSpec * envBrdfFactor(NdotV, roughness, F0) * (isVeg ? 0.0 : 1.0)) * env.intensity;
    }
    return ambientConstant * albedo * (1.0 - metallic);
}

// 直接光多灯循环（Phase 8 ③）：灯数组 = scene data 尾区 GpuLight[]；point（type=1）线性衰减
// 1-dist/range、dir（type=0）无衰减。count=0 回落 @0 头单灯（无显式 Light entity 的旧默认场景
// 行为，保直接光不丢）。
// 🔴 color 作**初值累加**（不是「返回直接光再相加」）——见文件头「累加次序是语义」。
// shadowFactor 由调用方给（只乘直接光：环境光 IBL 无方向 → 不乘；自发光不乘）。
float3 shadeLights(float3 color, float3 N, float3 V, float3 P, float3 albedo,
                   float metallic, float roughness, bool isVeg, float shadowFactor,
                   SceneLight shared)
{
    uint n = readLightCount();
    if (n == 0)
    {
        // count=0 回落 @0 头单灯（shadeLightVeg 已含植被透射项 ≈ 其 forward 回落的
        // shadeDirect+shadeTransmit）
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
    return color;
}
