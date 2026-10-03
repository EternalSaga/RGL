// 批 12.6 TAA（时域抗锯齿）——本帧成片 × 上一帧时域结果的自适应混合。
// mesh 复用 light.mesh.spv（全屏大三角，uv = framebuffer 对齐 1:1，同 tonemap/ao），
// state="taa"，执行器经 state 名解析管线条目（decorateTaaPass 插在 tonemap 之前）。
//
// ── 数据流 ──
//   本帧色（binding 4，jitter 投影渲出）+ 深度（binding 6，与 jitter 投影配套）
//   → 世界坐标重建（scene data @64 invViewProj，**抖动后**矩阵，与深度同口径）
//   → 场景运动补偿（motion = prevSpin·inverse(curSpin)，全场景同一旋转 ⇒ 一条 mat4
//     即精确补偿，优于逐物体运动矢量）
//   → 投到上一帧（prevViewProj = 上一帧**未抖**矩阵，两侧口径统一 ⇒ 历史查找位不漂
//     jitter）→ 双线性采历史（binding 5）
//   → 邻域裁剪（box5 AABB，Reinhard 域压 HDR 闪烁）→ 与历史混（feedback 常量）。
//
// ── 算法出处（抄 Filament antiAliasing/taa/taa.mat，逐项对齐）──
//   · 邻域 min/max（box5 十字邻域）+ 历史点沿线段裁到 AABB（Lottes line clip，
//     taa.mat:219-238 clipAabBoxFromAabb 同款解线段参数法）；
//   · HDR 域：Reinhard 映射（x/(1+x)）后做 AABB 与裁剪（taa.mat "Reinhard" 色彩空间
//     分支），高亮 firefly 在映射域被压回邻域范围；
//   · 混合：mix(current, history, feedback)，feedback = configs/scene.lua taa.feedback
//     （scene data @722208.x；Filament 0.12 ⇒ 指数窗 ≈19 帧收敛）。
//   YCoCg 亮度加权 AABB（taa.mat rgbToYcoCg）本批**未抄**——用 Reinhard 后的 RGB
//   等权盒：本项目基线场景无高饱和闪烁源，YCoCg 的主要收益在高光去鬼影，列记债。
//
// ── 逐位旁路（门关/首帧语义）──
//   historyValid（params.y）= 0 时**只读本帧色一张图直接输出**，不碰深度与历史
//   （首帧/resize/相机跳切走这条；历史图内容未定义，采样它就是未初始化显存）。
//   TAA 整体关闭时本 pass 根本不在图里（decorateTaaPass 不调用），不依赖本早退。
//
// ── 已知边界（计划书 taa_plan.md 记债清单同项）──
//   · 深度 1.0 = 天空像素：重投影落远平面且**不乘 motion**（天空不随场景自旋），
//     快速甩头时天空鬼影靠 AABB 裁剪兜；
//   · 历史无双线性深度，遮挡边缘的 disocclusion 靠颜色 AABB 拒绝（无 motion-vector
//     dilation），边缘允许 1 帧量级的残影；
//   · AO 施加在 tonemap（本 pass 之后）⇒ AO 噪声不被时域滤波（计划书设计边界）。

import scene;  // sceneData buffer（binding 3：@64 invViewProj + TAA 尾区 @722080 起）

[[vk::binding(4, 0)]] Texture2D<float4> colorTex;    // 本帧成片（RGBA16F 线性 HDR）
[[vk::binding(5, 0)]] Texture2D<float4> historyTex;  // taa_hist = 上一帧 taa_out 帧末 blit
[[vk::binding(6, 0)]] Texture2D<float>  depthTex;    // 本帧深度（D32Sfloat，eDepth aspect）
[[vk::binding(7, 0)]] SamplerState linearSampler;    // 共享 sampler（历史亚像素查找）
[[vk::binding(8, 0)]] SamplerState pointSampler;     // 深度不可过滤（同 ao.frag 理由）

struct TaaInput
{
    float4 position : SV_Position;  // framebuffer 坐标（深度 Load 用）
    float2 uv       : TEXCOORD0;    // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

// ── scene data 读段 ──
// ObjectMatrix（列 c0..c3）× 列向量：clip/M 变换一律"列组合"写法（同 sky/ao 的
// clipToWorld 惯例；勿按行主序点乘——ObjectMatrix 存的是列）。
float4 mulM(ObjectMatrix m, float4 v)
{
    return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z + m.c3 * v.w;
}

// clip → world（sceneData@64 = inverse(proj·view)，**抖动后**口径；字面量 64/80/96/112
// 与 scene_data.cppm kSceneDataSkyOffset + 16*i 一致，ao.frag clipToWorld 同款）
float4 clipToWorld(float4 clip)
{
    float4 c0 = asfloat(sceneData.Load4(64));
    float4 c1 = asfloat(sceneData.Load4(80));
    float4 c2 = asfloat(sceneData.Load4(96));
    float4 c3 = asfloat(sceneData.Load4(112));
    return c0 * clip.x + c1 * clip.y + c2 * clip.z + c3 * clip.w;
}

// 屏幕 uv + 深度 → 世界坐标（ao.frag worldFromDepth 同款：y 翻转，Vulkan 深度直接用）
float3 worldFromDepth(float2 uv, float depth)
{
    const float4 clip = clipToWorld(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return clip.xyz / clip.w;
}

// ── Reinhard 域（AABB 与裁剪的工作空间；Filament taa.mat "Reinhard" 分支）──
// x/(1+x) 把 [0,∞) 压进 [0,1)：HDR 高亮不再支配 AABB 体积；逆变换 x/(1−x) 前先钳回
// 定义域。负通道（理论不存在于本管线成片）先 saturate——裁剪空间里非负是解线段
// 参数法稳定性的前提。
float3 toReinhard(float3 x) { return x / (1.0 + x); }
float3 fromReinhard(float3 x) { return x / (1.0 - min(x, 0.9999)); }

// ── Lottes 线段裁剪（taa.mat clipAabBoxFromAabb 同款）──
// 把点 p 沿线段 c→p 收进 AABB [mn,mx]：参数化 p(t) = c + t·(p−c)，取与盒的**首个
// 出盒交点**（离 p 最近的进入参数 tMax）。c 本身出盒（邻域噪声/centroid 数值）时
// 直接塌回 c。p 已在盒内 ⇒ 返回 p（不改变历史）。
float3 clipToAabb(float3 c, float3 p, float3 mn, float3 mx)
{
    const float3 d = p - c;
    float tMin = 0.0, tMax = 1.0;
    for (int i = 0; i < 3; ++i)
    {
        if (abs(d[i]) > 1e-6)
        {
            const float ta = (mn[i] - c[i]) / d[i];
            const float tb = (mx[i] - c[i]) / d[i];
            tMin = max(tMin, min(ta, tb));
            tMax = min(tMax, max(ta, tb));
        }
        else if (c[i] < mn[i] || c[i] > mx[i])
        {
            return c;  // 与盒轴平行且盒外：无处可交，拒绝历史
        }
    }
    if (tMin > tMax)
        return c;
    return c + d * tMax;
}

[shader("fragment")]
float4 main(TaaInput input) : SV_Target
{
    const float4 params = readTaaParams();  // (feedback, historyValid, jitterX, jitterY)
    const float3 cur = colorTex.Sample(linearSampler, input.uv).rgb;

    // 逐位旁路早退（首帧 / resize / 跳切；见头注释"逐位旁路"段）
    if (params.y < 0.5)
        return float4(cur, 1.0);

    // ── 重投影：本帧像素 → 上一帧屏幕 uv ──
    const float depth = depthTex.Load(int3(input.position.xy, 0)).r;
    float3 world = worldFromDepth(input.uv, depth);
    float4 worldPrev = float4(world, 1.0);
    if (depth < 1.0)  // 天空不随场景自旋（motion 是场景旋转补偿）
        worldPrev = mulM(readTaaMotion(), worldPrev);
    const float4 prevClip = mulM(readTaaPrevViewProj(), worldPrev);
    if (prevClip.w <= 0.0)  // 上一帧相机背后：无历史可言
        return float4(cur, 1.0);
    const float2 prevNdc = prevClip.xy / prevClip.w;
    // ndc → uv 与 worldFromDepth 的 y 翻转互逆：uv.y = (1 − ndc.y)/2
    const float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, (1.0 - prevNdc.y) * 0.5);
    if (any(prevUv < float2(0.0, 0.0)) || any(prevUv > float2(1.0, 1.0)))
        return float4(cur, 1.0);  // 出画（本帧视锥外）：clamp 边缘 = 拿错像素，拒绝

    // ── 邻域 AABB（box5 十字：中心 + 上下左右，Reinhard 域）──
    uint pxW, pxH, mLvl;
    colorTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));
    float3 mn = 1.0, mx = 0.0, centroid = 0.0;
    const int2 offs[5] = { {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
    for (int i = 0; i < 5; ++i)
    {
        const float3 t = saturate(colorTex.SampleLevel(linearSampler,
            input.uv + float2(offs[i]) * texel, 0.0).rgb);
        const float3 r = toReinhard(t);
        mn = min(mn, r);
        mx = max(mx, r);
        centroid += r;
    }
    centroid *= 0.2;

    // ── 历史裁剪 + 混合 ──
    const float3 hist = saturate(historyTex.SampleLevel(linearSampler, prevUv, 0.0).rgb);
    const float3 clipped = fromReinhard(clipToAabb(centroid, toReinhard(hist), mn, mx));
    return float4(lerp(cur, clipped, params.x), 1.0);
}
