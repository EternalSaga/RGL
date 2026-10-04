// 批 12.6 TAA（时域抗锯齿）——本帧成片 × 上一帧时域结果的自适应混合。
// mesh 复用 light.mesh.spv（全屏大三角，uv = framebuffer 对齐 1:1，同 tonemap/ao），
// state="taa"，执行器经 state 名解析管线条目（decorateTaaPass 插在 tonemap 之前）。
//
// ── 数据流 ──
//   本帧色（binding 4，jitter 投影渲出）+ 深度（binding 6，与 jitter 投影配套）
//   → 世界坐标重建（scene data @64 invViewProj，**抖动后**矩阵，与深度同口径）
//   → 场景运动补偿（motion = prevSpin·inverse(curSpin)，全场景同一旋转 ⇒ 一条 mat4
//     即精确补偿，优于逐物体运动矢量）
//   → 投到上一帧（prevViewProj = 上一帧**抖动后**矩阵 = 渲染出历史图的那个矩阵；历史内容
//     本就长在抖动后的屏幕系上，用未抖那份会系统性偏掉 -jitter_prev）→ 双线性采历史（binding 5）
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
//
// ── 本批实测**否决**的两条候选（批 13.1e，别再照抄一遍）──
//   症状「box5 邻域盒退化 ⟹ clipToAabb 恒返回 centroid ⟹ 固定 5 抽头空间糊 + 时域零累积」
//   是真的（机制：clipToAabb 的 tMax 对三通道取 min，单通道 mn==mx 就把整条射线钉回 t=0），
//   但两条修法都**测不出收益**，故都不进主干：
//     (a) extent = max(extent, K·max(center, ε))（UE4 式相对下限），K ∈ {0.02,0.05,0.1,0.2,0.4}
//         + ε ∈ {1e-3,3e-3}；(b) 盒退化时跳过裁剪（全通道判据 / 任一通道判据，thr ∈ {1e-4,1e-3,1e-2}）。
//   2×2 因子实验（交付配置 + 用户机位，js=0 恒等式 mad / 逐像素最大 / 白屋顶区带号差 / 闪烁抑制比）：
//     取值域闸门旧(saturate) × {盒原样, 盒修法(b')} = 1.68808 / 1.68807，max 37 / 37，−18.506 / −18.506
//     取值域闸门新(只夹下界) × {盒原样, 盒修法(b')} = 0.02257 / 0.02259，max 3 / 3，−0.014 / −0.014
//   ⟹ **恒等式在 js=0 的稳态与裁剪算子无关**（收敛点由「历史怎么被夹」决定，不由「历史怎么被裁」
//   决定），盒的两条修法在压暗区**逐位不改**。而取值域闸门一修，退化率自己塌了：任一通道退化
//   11.299% → 1.881%、三通道全退化 7.749% → 0.010%、clip 塌回 centroid 9.799% → 0.381%
//   （mode 9 诊断图，js=1 同相位 17 帧）。剩下的 0.38% 落在真平坦区——糊一个平坦区还是那个平坦值，
//   所以给它加盒下限是**只付风险不拿收益**（盒变宽 = 鬼影保护变松）。诊断开关已随定案删除。

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
// 定义域。进本域之前的取值域闸门见下面 `clampHdr`——**只夹下界**（负通道理论不存在于
// 本管线成片，但 CR 负瓣会造出来；裁剪空间里非负是解线段参数法稳定性的前提），
// **绝不夹上界**（上界 = 把 HDR 亮部顶平，见 clampHdr 头注释的实测全案）。
float3 toReinhard(float3 x) { return x / (1.0 + x); }
float3 fromReinhard(float3 x) { return x / (1.0 - min(x, 0.9999)); }

// ── 进 Reinhard 域之前的取值域闸门 ──
// ⚠ **只准夹下界，不准夹上界**（批 13.1e 实测根因，2026-10-04）。本 pass 在 tonemap
// **之前**，colorTex 与历史都是线性 HDR，通道值 ≫ 1 是常态（用户机位的白屋顶实测到 3+）。
// 写成 saturate() 就把所有 HDR 亮部**顶平到 1.0**，于是时域不动点从 X = cur 变成
//     X = (1−α)·1.0 + α·cur   （α = feedback）
// —— 亮部被系统性压暗。实测（交付配置 configs/*.lua 原样 + 报障机位，js=0，64 帧收敛）：
// 白屋顶 8.83% 像素 mean Δ = **−18.5 LSB/255**（最大 −37），而该区域**之外** mean|Δ| 只有
// 0.059；fb=1（α=1 ⟹ 不动点 = cur）时压暗塌回 −2.4，fb=0.12 时 −18.5 —— 与上式逐条吻合。
// 判据读数：恒等式 mad 1.688 → **0.0226**、逐像素最大 37 → **3**、dev≥2 像素 10.373% →
// **0.0033%**（imgqa `temporal-identity`，交付配置 + 用户机位）。
// 为什么 Reinhard 不需要上界：x/(1+x) 的定义域本来就是 [0,∞) ⟹ 值域 [0,1)，顶平恰好
// 把它要保的高光信息全削掉。副作用还有第二条：顶平区 5 抽头全 = 1 ⟹ **邻域盒退化**，
// 于是「盒塌回 centroid」看着像独立缺陷；实测修掉本闸门后塌缩率 9.799% → **0.381%**
// （剩下的落在真平坦区，糊了也看不出来）。
// 下界仍要夹：CR 有负瓣，负值进 x/(1+x) 会翻越 x=−1 奇点，且 clipToAabb 解线段要求非负。
float3 clampHdr(float3 x) { return max(x, float3(0.0, 0.0, 0.0)); }

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

// ── 历史采样：精确 4×4 Catmull-Rom（16 次整数 Load）──
// 为什么不能双线性：**历史就是累积图**，每帧一次双线性重采样 = 老贡献被反复低通，糊度**逐帧
// 复利**。实测（phase13 §1.5）：糊度随历史年龄单调上升（feedback 1.0/0.5/0.12/0.02 →
// +0.1/−27.3/−49.7/−57.0%），而正确的相位平均只该 −24.2% 且对 feedback 不敏感。
// 为什么不是「4 次双线性折叠 CR」（= Slang sampleBiquadric 那类技巧，本版本无该内建）：
// **CR 有负瓣**，双线性抽头权重恒 ∈[0,1]，数学上表示不了负瓣 → 不是 CR（Python 对拍暴力
// 4×4 CR 最大误差 0.146 实测否决）。故 16 次 Load 精确求值。
float3 sampleHistoryCatmullRom(float2 uv)
{
    uint pxW, pxH, mLvl;
    historyTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));
    const float2 p = uv / texel - 0.5;              // 纹素中心坐标系（纹素 i 中心在 i+0.5）
    const int2 b = int2(floor(p));
    const float2 t = p - float2(b);

    float wx[4], wy[4];
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float2 a = t + 1.0 - float2(i);       // 到抽头 b−1+i 的有向距离 ∈ (−2, 2]
        // ⚠ **核关于 a 是偶函数**（只依赖到纹素中心的距离），故三次项必须取 |a|：a3 = a²·|a|。
        // 写成带符号的 a³ 会让 t=0 的权重从 (0,1,0,0) 变成 (0,1,−3,8)（Σ=6）——**恒等性破产**、
        // 2D 直流增益 ≈36×，当时历史再经 saturate 顶到上界（取值域闸门已于批 13.1e 改成只夹
        // 下界，见 clampHdr）⟹ 被 AABB 判成越界（phase13 §1.8）。
        // 恒等性正是 imgqa 判据 `temporal-identity` 的算术基础（零抖动 ⇒ 输出必须逐字节 = R）。
        const float2 m = abs(a), a2 = a * a, a3 = a2 * m;
        // Catmull-Rom 核 k(m)：m≤1 → 1.5m³−2.5m²+1；1<m<2 → −0.5m³+2.5m²−4m+2
        wx[i] = select(m.x > 1.0, -0.5 * a3.x + 2.5 * a2.x - 4.0 * m.x + 2.0,
                                  1.5 * a3.x - 2.5 * a2.x + 1.0);
        wy[i] = select(m.y > 1.0, -0.5 * a3.y + 2.5 * a2.y - 4.0 * m.y + 2.0,
                                  1.5 * a3.y - 2.5 * a2.y + 1.0);
    }
    float3 acc = 0.0;
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const int2 q = clamp(b - 1 + int2(i, j), int2(0, 0),
                                 int2(int(pxW) - 1, int(pxH) - 1));
            acc += wx[i] * wy[j] * historyTex.Load(int3(q, 0)).rgb;
        }
    }
    return acc;
}

[shader("fragment")]
float4 main(TaaInput input) : SV_Target
{
    const float4 params = readTaaParams();  // (feedback, historyValid, jitterX, jitterY)
    const float3 cur = colorTex.Sample(linearSampler, input.uv).rgb;

    // 逐位旁路早退（首帧 / resize / 跳切；见头注释"逐位旁路"段）
    if (params.y < 0.5)
        return float4(cur, 1.0);

    // ── 邻域 AABB（box5 十字：中心 + 上下左右，Reinhard 域）──
    // ⚠ texel 必须先于出画判据取到：出画判据要**半个纹容差**（见下）。
    uint pxW, pxH, mLvl;
    colorTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));

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
    // 出画判据带**半个纹容差**：uv 的合法域是纹素中心 [0.5·texel, 1 − 0.5·texel]，
    // 而判据原来写的是 [0,1] —— 最外一圈的纹素中心本来就"出界"半个纹，于是**整列 x=1279
    // 或整行 y=0** 被误拒（mode 8 诊断图实测，交付配置 + 用户机位 js=1：旧判据逐帧拒
    // 0…2199 像素、17 帧均值 0.086%，命中集合随抖动相位在最外一行/最外一列之间切换；
    // 半纹判据下同一序列 **逐帧恒为 0**）。半纹 = 「重投影仍落在最外圈纹素之内」的最紧
    // 正确边界；再宽就会把真正的出画（clamp 边缘拿错像素）放进来。
    const float2 outTol = 0.5 * texel;
    if (any(prevUv < -outTol) || any(prevUv > 1.0 + outTol))
        return float4(cur, 1.0);  // 出画（本帧视锥外）：clamp 边缘 = 拿错像素，拒绝

    float3 mn = 1.0, mx = 0.0, centroid = 0.0;
    const int2 offs[5] = { {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
    for (int i = 0; i < 5; ++i)
    {
        const float3 t = clampHdr(colorTex.SampleLevel(linearSampler,
            input.uv + float2(offs[i]) * texel, 0.0).rgb);
        const float3 r = toReinhard(t);
        mn = min(mn, r);
        mx = max(mx, r);
        centroid += r;
    }
    centroid *= 0.2;

    // ── 历史裁剪 + 混合 ──
    // ⚠ **参数序 = feedback 是「本帧权重」**（configs/scene.lua taa.feedback 的文档口径 +
    // Filament `mix(history, filtered, feedback)` 同序）：本帧 0.12 / 历史 0.88 ⇒ ≈19 帧
    // 收敛。写成 lerp(cur, hist, f) 会把权重反过来（本帧 0.88），累积窗塌到 ≈1.14 帧 =
    // 时域滤波名存实亡。批 12.6 T4 的阳性锚点实测抓到：A/F = 1.01×（闭式预测 3.95×）、
    // 全图直方图只多出**一个**混合级（历史只渗进 12%，永不继续累积）。
    // 推论：f=0 ⇒ 输出 = 裁剪后的**历史**（完全丢本帧的退化反馈环），不是「无滤波」；
    // 要做「只抖不滤」的消融请用 f=1（imgqa temporal 的 S1 组）。
    const float3 hist = clampHdr(sampleHistoryCatmullRom(prevUv).rgb);
    const float3 histR = toReinhard(hist);

    const float3 clipped = fromReinhard(clipToAabb(centroid, histR, mn, mx));
    return float4(lerp(clipped, cur, params.x), 1.0);
}
