// 透明（BLEND）图元的前向着色 frag —— deferred 单路线下的**唯一**透明着色路
//
// 为什么只能前向着色：透明 pass 只挂 scene_hdr 一张色附件、按远→近逐层 alpha 混合，而
// 一像素一套 G-buffer ⟹ deferred 表达不了多层叠加（第二层没有地方存它的 albedo/normal/alpha）。
// 论证与失效条件 → docs/透明混合管线_plan.md §2。
//
// 🔴 **算式不在本文件**：GGX 直接光、植被 wrap+透射、IBL 环境光、多灯循环全在 pbr.slang，
// 与 deferred 的 light.frag 同吃那一份（2026-10-18 刀0 抽出，逐位探针四档全绿）。本项目历史上
// 有过一次「两份实现 + 严格同源注释」（已删的 forward model_pbr_ibl.frag 与 light.frag），
// 注释不阻止漂移——所以这里**不抄第二份**。本文件负责的是前向侧独有的三件事：
//   ① 材质从 heap 的 per-object 表读（light.frag 从 G-buffer 读，透明体不写 G-buffer）；
//   ② alpha 出像素（混合 factors 在 applyDynamicState 的 transparentState 接线，见 shader.cppm）；
//   ③ 材质类别由**编译期宏**给（light.frag 从 emissive.a 通道读——透明体没写那张通道）。
//
// 材质类别（MATERIAL_CLASS，wrapper 先 #define 再 #include 本文件，常量折叠进各自 SPV）：
//   0 = 标准 PBR（本体）  1 = 植被（wrap 漫反射 + 背光透射）  2 = 光源（本色 ×1/4）  3 = unlit
// 与 opaque 侧 deferred_geometry 那一族**同一套类别语义**，差别只在类别怎么传达到着色处
//（G-buffer 通道 vs 编译期宏）。路由 = scene_config 的 resolvePipelineSpecName：
// blend 图元走 transparent_styles 表，其余走 deferred_styles。
//
// 与 opaque 侧的**已知不等价**（逐条带理由，账挂 docs/透明混合管线_plan.md §4）：
//   · 湿面调制不参与：那三项调制（粗糙度空间变化 / 水膜法线扰动 / albedo 压暗）住在
//     deferred_geometry 写 G-buffer 的路上，透明体不写 G-buffer。透明资产（玻璃/水膜片）
//     本来也不该被当成"湿石头"。
//   · 阴影只有 CSM 单档：light.frag 的 IZB / PCSS / EVSSM 是**变体编译**（-D 选 SPV），
//     本文件一族第一版不做变体 ⟹ 恒 sampleShadow1Tap。已删的 forward 三个阴影 frag 同样
//     从没进过那个变体族，所以这不是新缺口，是原样继承。
//   · 不写 velocity：速度场走深度重建（velocity.frag），透明体不写深度 ⟹ 运动的透明体在
//     TAA / FSR 下会拖影。静态场景无关。
//   · MASK（alphaMode="MASK"）不走本文件：它留在 opaque 段由 shader 内 discard 处理
//     （Phase 8 C1），且 alphaMode 三值互斥 ⟹ blend 图元的 alphaCutoff 恒 0，
//     这里**不需要** alphaDiscard 分支（写了也是死码）。

import model;      // ModelVertexOutput + 纹理绑定（1 baseColor / 2 sampler / 6 normal / 7 orm / 8 emissive）
import material;   // 统一 push constant（DrawArgs 超集，Phase 4）
import scene;      // 场景级 bindless：readSceneLight（eyePos）+ readObjectMatrix + worldPos/worldNormal/worldTangent
import scene_tables;  // 逐对象四张表读取面（binding 23/25/26/33），readObjectDraw 在此
import normalmap;  // 标准 TBN + DX flip-G 解码 + 几何法线导数 roughness AA（与 deferred_geometry 同源）
import env;        // 环境 cubemap 采样源（binding 4/5；本 pass 不占 G-buffer 槽位，与 light.frag 不同）
import lights;     // 多灯载体（binding 24）；调用点在 pbr
import shadow;     // 级联阴影采样（binding 12/13）
import pbr;        // 🔴 共享着色核：EnvParams/readEnvParams + shadeAmbient + shadeLights

#ifndef MATERIAL_CLASS
#define MATERIAL_CLASS 0.0
#endif

[shader("fragment")]
float4 main(ModelVertexOutput input, bool frontFace : SV_IsFrontFace) : SV_Target
{
    // ── 材质与外观（① 前向侧：从 heap 的 per-object 表读，不经过 G-buffer）──
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    const ObjectDraw od = readObjectDraw(drawArgs.objectIndex);
    float3 albedo = od.baseColor.rgb * texel.rgb;
    // alpha = 材质 baseColor.a × 纹理 a（Phase 5 定的口径，与已删的 forward model_pbr_ibl 同式）。
    // 混合语义 = SRC_ALPHA / ONE_MINUS_SRC_ALPHA（非预乘，见 shader.cppm applyDynamicState 的
    // colorBlend 分支）⟹ 本文件输出**未预乘**的颜色。
    const float alpha = od.baseColor.a * texel.a;

    // class 3 = unlit（glTF KHR_materials_unlit）：baseColor 原样出像素，无光照/无自发光相加/
    // 无阴影。与 deferred 的 class 3 分支（light.frag isUnlit）同语义——那条分支当年是探针重立
    // 到 deferred 时由 aa-coverage 电平自检抓出的 D1 破口（缺表回落 = 被静默当 PBR 打光），
    // 透明侧第一版就把这格填上，不重演一次。
    if (MATERIAL_CLASS > 2.5)
        return float4(albedo, alpha);

    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 P = worldPos(M, input.positionOS);
    float3 geoN = worldNormal(M, normalize(input.normal));  // 几何法线（specular AA 用）
    float3 N = geoN;

    // ORM（G=roughness B=metallic，R=AO 采样不应用，白纹理回落 factor）+ normal 贴图（标准 TBN，
    // CPU 烘切线 + 手性，无导数）+ doubleSided 背面翻向 —— 全部与 deferred_geometry 逐式同源，
    // 差别只是结果不进 G-buffer 而直接进光照。
    float4 orm = ormTexture.Sample(baseColorSampler, input.uv);
    float3 tangentNormal = decodeNormalDx(normalTexture.Sample(baseColorSampler, input.uv));
    N = applyNormalMapTBN(N, worldTangent(M, input.tangentOS), tangentNormal);
    if (!frontFace)
        N = -N;

    float metallic = clamp(od.roughnessMetallic.y * orm.b, 0.0, 1.0);
    float roughness = clamp(od.roughnessMetallic.x * orm.g, 0.04, 1.0);
    roughness = normalFiltering(roughness, geoN);  // 几何法线导数估方差抬 roughness（掠射角白噪点）

    // 类别语义与 light.frag 的 emissive.a 分支**同判据同处置**：植被/光源强制电介质。
    const bool isVeg = MATERIAL_CLASS > 0.5 && MATERIAL_CLASS < 1.5;
    const bool isLamp = MATERIAL_CLASS > 1.5 && MATERIAL_CLASS < 2.5;
    if (isVeg || isLamp)
        metallic = 0.0;

    // ── 光照（② 算式在 pbr，与 light.frag 同源）──
    SceneLight shared = readSceneLight();   // 共享项：eyePos@12、ambient@36
    float3 V = normalize(shared.eyePos - P);
    EnvParams env = readEnvParams();        // @48 intensity / @52 mipRange
    float3 color = shadeAmbient(envMap, envSampler, N, V, albedo, metallic, roughness,
        isVeg, env, shared.ambient);
    // 阴影只乘直接光（环境光 IBL 无方向 → 不乘；自发光不乘），与 light.frag 同口径。
    const float shadowFactor = sampleShadow1Tap(P, N);
    color = shadeLights(color, N, V, P, albedo, metallic, roughness, isVeg, shadowFactor, shared);

    // 自发光相加（emissiveFactor × emissiveTexture，HDR 域，tonemap 链收）。光源类 ×1/4 = 还原
    // strength 预乘，**与 light.frag 的 isLamp 分支同值**——透明玻璃与不透明玻璃的自发光电平
    // 必须一致，否则"换个 alphaMode 就变亮"。⚠ 这个"同值"是**照抄现役行为**，不是独立判断：
    // 该 0.25 当年补偿的 pack 强度已归位成 1.0 ⟹ 净 4 倍压暗，口径待裁 → 债-131（两处一起改）。
    color += od.emissive.rgb * emissiveTexture.Sample(baseColorSampler, input.uv).rgb
        * (isLamp ? 0.25 : 1.0);
    return float4(color, alpha);
}
