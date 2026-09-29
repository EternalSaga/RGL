// Phase 11 批 11.2：阴影图**采样**契约（forward 与 deferred 两条光照路共用）。
//
// 绑定编号的选择（本项目 binding 号是管线内唯一命名空间，两条路各自的低号段已被占满，
// 故阴影统一排 12/13 —— 见下表与 shader_registry 的 extra 映射）：
//   forward 模型条目：0 顶点 / 1 baseColor / 2 sampler / 3 sceneData / 4 env / 5 envSampler
//                      / 6 normal / 7 orm / 8 emissive  → 9,10 空着但**保留给未来扩展**，
//                      阴影固定用 12/13（两路一致，改一处只需改一处 mapping 逻辑）
//   deferred light 条目：0,1 占位 / 3 sceneData / 4-7 G-buffer / 8 sampler / 9 env /
//                      10 envSampler / 11 emissive → 12/13 同样空闲
// 结论：12/13 在**两条路上都空闲**，故取 12/13，让 shadow.slang 成为一个真正共用的契约
// （若两条路取不同号，同一份采样代码就得复制两份 = D1 破口的老路）。
//
// 深度图用 eDepth aspect 采样（RT 缓存 ensure(..., eDepth) 建 view）。sampler 用
// **SamplerLibrary::ensurePointSampler**（NEAREST + CLAMP_TO_EDGE），不用 ensureShared 的 LINEAR：
// D32Sfloat 在 Vulkan 里无 filterable 能力，linear 采样深度 = invalid usage。
//
// **为何不用比较采样器（SamplerComparisonState）改成普通 sampler + 手动比较**：
// 4 抽头 PCF 需要四个**不同 uv** 的采样，而比较采样器在 NEAREST 下与普通
// 采样器等价（都是取单个纹素），换成比较采样器不换来任何好处，反而多一层
// SPIR-V 语义（SampledImage+Dref vs SampledImage）与无意义混合。手动比较也让 PCF 的加权形式在同一行
// 代码里可见（当前 4 抽头等权，改 9 抽头只动一个数）。本项目已有先例：
// ao.frag 同样用普通 pointSampler 采深度后手动比较。

import scene;  // binding 3：sceneData + readShadowViewProj / readCascadeSplits / readShadowParams

[[vk::binding(12, 0)]] Texture2D<float> shadowMap;  // eDepth aspect（cascade 0）
[[vk::binding(13, 0)]] SamplerState shadowSampler;  // NEAREST point sampler（非比较采样器，见头注）

// 各向同性单次比较采样（4 抽头 PCF 在此展开，见下）。返回 1 = 全亮，0 = 全遮。
//
// **uv 的 y 翻转**：光空间 NDC 的 y 向上（+1 顶），framebuffer 采样 uv 的 y 向下（0 顶），
// 故 uv.y = 0.5 - ndc.y*0.5。与 ao.frag / light.frag 的 ndc↔uv 约定同款。
// **深度比较**：读回的 depth 是 Vulkan 零到一 [0,1]，与 shadow mesh 写入的值同域，直接比。
float sampleShadow1Tap(float3 worldPos, float3 N)
{
    const float4 params = readShadowParams();
    if (params.z < 0.5)
        return 1.0;  // 总开关关：全亮（与 tonemap 的 ao.strength=0 旁路同款）

    // ① 选级联：按片元的**视深**（沿相机 forward 的深度），与 CPU 切分口径**完全一致**——
    //   级联切片是按视深切的子视锥（projWithDepthRange 只改深度行）。若按径向距离选级，
    //   屏幕边缘/底部片元（径向 > 视深）会被分到更深的级，而它的视深落在该级子视锥之外
    //   → 出块 → 全亮旁路 → 边缘处没影子。
    //   forward 从 skyboxInvViewProj（@64）第 2 列取：invVP 第 2 列 = invV·(0,0,p,0)
    //   = p·(view +z 的世界方向) = −p·forward（w=0 无平移项混入）→ 取反归一化即 forward。
    //   splits = 每级远截止（线性视深）；末级写 1e9，永远不会越界到不存在的级。
    const float3 eyePos = readSceneLight().eyePos;
    const float3 forward = -normalize(asfloat(sceneData.Load4(96)).xyz);
    const float depth = dot(worldPos - eyePos, forward);
    const float4 splits = readCascadeSplits();
    uint cascade = 0u;
    if (depth > splits.x) cascade = 1u;
    if (depth > splits.y) cascade = 2u;
    if (depth > splits.z) cascade = 3u;

    // ② normal offset：沿法线把采样点外推 k 个 texel。这是抗 acne 的**主力**手段——
    // 比 depthBias 稳（不改深度比较值 = 不引入 peter-panning 与深度非线性敏感度），
    // 代价是接触阴影略收缩（k 越大越明显），k=1.5 是常见甜点。
    const float4 texelSizes = readCascadeTexelSizes();
    const float texel = texelSizes[cascade];
    const float3 offsetPos = worldPos + N * (texel * params.y);

    // ③ 投影到光空间 → **块内** uv + 参考深度
    const float4 clip = clipPos(readShadowViewProj(cascade), offsetPos);
    const float3 ndc = clip.xyz / clip.w;  // 正交投影下 w 恒 1，仍写除法保持与 sky 同款
    const float2 uvTile = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);   // y 翻转（framebuffer 原点在左上）

    // ④ 出块返回全亮：级联边界外 / 深度范围外不产生阴影（否则图集边缘拉出长条伪影）
    if (uvTile.x < 0.0 || uvTile.x > 1.0 || uvTile.y < 0.0 || uvTile.y > 1.0 || ndc.z > 1.0)
        return 1.0;

    // ⑤ 块内 uv → 图集 uv。4 级共用一张 2×2 图集，每级占一块（readShadowAtlas 从 scene data
    // 取图集/块边长与列数，**不在本文件写死**——改图集尺寸只改 C++ 一处）。tile 排布与图装饰的
    // shadowTile 同款：cascade c → (c%cols, c/cols)。
    const float4 atlas = readShadowAtlas();
    const float cols = atlas.z;
    const uint colsU = uint(cols);
    const float2 tile = float2(float(cascade % colsU), float(cascade / colsU));
    const float2 uv = (tile + uvTile) * atlas.y / atlas.x;

    // ⑥ 4 抽头 PCF（2×2，偏移半个纹素）：单抽头的阶梯状阴影边在 1024² 块上肉眼可见，
    // 4 抽头是"看不出锯齿"与"不多花 4 倍采样"的平衡点。偏移量用 texel 常量而非屏幕导数：
    // texelSizes 是 scene data 里的 uniform（每帧一样）而不是按像素变化的；用 ddx/ddy 会在分支内
    // 未定义（同 ao.frag 批 11.4 的教训）。
    const float2 o = float2(texel * 0.5, texel * 0.5);
    const float ref = ndc.z - params.x;
    const float d0 = shadowMap.Sample(shadowSampler, uv + float2(-o.x, -o.y));
    const float d1 = shadowMap.Sample(shadowSampler, uv + float2(o.x, -o.y));
    const float d2 = shadowMap.Sample(shadowSampler, uv + float2(-o.x, o.y));
    const float d3 = shadowMap.Sample(shadowSampler, uv + float2(o.x, o.y));
    // SamplerState 是普通采样器（非比较采样器），故手动比较：返回 1 = 受光
    const float4 lit = float4(ref <= d0 ? 1.0 : 0.0, ref <= d1 ? 1.0 : 0.0,
        ref <= d2 ? 1.0 : 0.0, ref <= d3 ? 1.0 : 0.0);
    return dot(lit, 0.25);
}
