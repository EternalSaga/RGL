// 批 12.6 TAA（时域抗锯齿）——本帧成片 × 上一帧时域结果的自适应混合。
// mesh 复用 light.mesh.spv（全屏大三角，uv = framebuffer 对齐 1:1，同 tonemap/ao），
// state="taa"，执行器经 state 名解析管线条目（decorateTaaPass 插在 tonemap 之前）。
//
// ── 数据流（批 13.5 起：历史住**基空间**）──
//   本帧色（binding 4，jitter 投影渲出）+ 深度（binding 6，与 jitter 投影配套）
//   → **输入侧 un-jitter**（按本帧内容位移 c_t 双线性重建到基网格；批 13.5，见 main 内注释）
//   → 世界坐标重建（scene data @64 invViewProj，**抖动后**矩阵，与深度同口径 —— 深度缓冲
//     是抖动投影渲的，这一半边**不**跟着搬基空间）
//   → 场景运动补偿（motion = prevSpin·inverse(curSpin)，全场景同一旋转 ⇒ 一条 mat4
//     即精确补偿，优于逐物体运动矢量）
//   → 投到上一帧（prevViewProj = 上一帧**抖动后**矩阵 ⟹ 产出**抖动网格**坐标）
//   → **减 c_{t−1} 落回基空间历史网格**（批 13.5；与上一行的「抖动后」口径成对）
//   → Catmull-Rom 采历史（binding 5；静止机位下采样偏移恒 0 ⟹ 实际退化为点采样）
//   → 邻域裁剪（box5 AABB，Reinhard 域压 HDR 闪烁）→ 与历史混（feedback 常量）。
//
// ── 为什么历史必须住基空间（批 13.5 根因，别再改回去）──
//   改前：历史 = 上一帧 taa_out 逐像素 1:1 拷来，内容长在抖动空间 ⟹ 静止机位下每帧按
//   `δ_t = c_t − c_{t−1}`（闭式均值 0.667px）亚像素重采样历史。Halton 周期 16 ⟹ `δ_t` 只取
//   16 个离散值、且只依赖相位不依赖内容 ⟹ **输出对相位是 16 周期函数**，而重采样误差只在
//   高频处响应 ⟹ 钉死在几何边缘 = 用户报障的「固定频率 + 固定位置」shimmer（实测交付相邻帧
//   9.22% 像素 >2LSB）；老贡献被反复重采样 ⟹ 比理想 16 相位超采样还糊 1.5 倍（梯度比 0.671）。
//   改后：抖动在进入累积**之前**被消化 ⟹ 混合发生在基空间 ⟹ 静止机位历史零重采样。
//   Filament 同构（输入侧 Lanczos un-jitter + 两侧无抖矩阵），全案 → `docs/taa_history_space_plan.md`。
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
    const float4 params = readTaaParams();  // (feedback, historyValid, c_t.x px, c_t.y px)
    // ⚠ texel 必须先于一切采样取到：un-jitter 与出画判据都要它。
    uint pxW, pxH, mLvl;
    colorTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));

    // ── 输入侧 un-jitter（批 13.5）：把本帧从抖动网格搬到基网格 ──
    // 抖动后渲染的图像内容整体平移 +c_t（`I_t(x) = F(x − c_t)`）⟹ 按 `+c_t` 采样即落回
    // 基网格。**这是本 pass 时域稳定性的来源**：混合从此发生在基空间，历史不再每帧被
    // 亚像素重采样（静止机位下历史采样偏移恒 0）⟹ 16 帧周期固定位置 shimmer 的成因消失。
    // 核 = **双线性**（4 tap，权重恒 ∈[0,1]）。刻意不用 CR/Lanczos：输入侧重建每帧只做一次、
    // 不叠加，低通不复利；但**负瓣振铃会进历史并在历史里累积**。
    // 【实测背书，原样搬自批 13.2 的 resolve 核选型（同一族问题，数据勿丢）】
    //   本机 bistro 1280×720、交付配置 + 用户机位、fb=.12 js=1、2026-10-04：
    //     梯度能量（越高越锐）    不重采样 0.038272 ｜ 双线性 **0.034843** ｜ CR 0.036999
    //     `imgqa aa-coverage` 域内最大误差  0.138 列 ｜ 双线性 **0.119 列** ｜ CR 0.181 列
    //     黑晕「凭空多黑」        0.026 列 ｜ 双线性 0.016 列 ｜ CR **−0.030 列**
    //   ⟹ CR 确实更锐（+6.2% 梯度），但在**本项目已登记的抗锯齿判据**上退化（0.119 → 0.181，
    //     比不重采样基线 0.138 还差），且「凭空多黑」翻成**负号** = 负瓣在黑白边界造振铃。
    //     梯度能量这条**没有闭式参考**，单看它会得出相反结论（`taa_plan` §7 第 10 条同一课：
    //     拿没有闭式的量当主判据，结论随量的选择翻转）。故按有闭式的那条定案 = **双线性**。
    // ⚠ **必须显式 saturate 钳回 [0,1]**：共享 sampler 的 wrap 是 **REPEAT**
    //   （`sampler_library.cppm` 平铺 UV 定案，bistro 49.5% primitive 越界 [0,1]），而
    //   屏幕空间 uv 越界绕回 = 最外一圈采到**对侧**像素（1px 宽的假边）。|c_t| ≤ 0.5px
    //   ⟹ baseUv 最多越界半纹，正好触发。钳到 [0,1] 等价 CLAMP_TO_EDGE，双线性核对
    //   [0,1] 内的亚像素越界本就把取数钳到边缘纹素，**不改内部像素一个 bit**。
    //   （此坑原载于批 13.2 tonemap resolve 注释，随 un-jitter 前移一并搬来；
    //    邻域盒的 ±1 texel 抽头**不**钳——改前就如此，钳了会动基线。）
    const float2 baseUv = saturate(input.uv + params.zw * texel);
    const float3 cur = colorTex.Sample(linearSampler, baseUv).rgb;

    // 逐位旁路早退（首帧 / resize / 跳切；见头注释"逐位旁路"段）
    // ⚠ 返回的是**已 un-jitter** 那份，不是原始抖动帧：taa_out 每帧都 blit 进 taa_hist 当
    //   下一帧历史，早退帧若留在抖动空间，下一帧就会拿抖动空间图当基空间历史用（偏 c_{t−1}）。
    if (params.y < 0.5)
        return float4(cur, 1.0);

    // ── 邻域 AABB（box5 十字：中心 + 上下左右，Reinhard 域）──
    // 抽头绕 **baseUv**（不是 input.uv）：盒要与 cur/历史同处基空间，否则盒是抖动系的、
    // 被裁的值是基系的 ⟹ 裁剪边界系统性偏 c_t（批 13.5 前两者都在抖动空间，无此问题）。
    float3 mn = 1.0, mx = 0.0, centroid = 0.0;
    const int2 offs[5] = { {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
    for (int i = 0; i < 5; ++i)
    {
        const float3 t = clampHdr(colorTex.SampleLevel(linearSampler,
            baseUv + float2(offs[i]) * texel, 0.0).rgb);
        const float3 r = toReinhard(t);
        mn = min(mn, r);
        mx = max(mx, r);
        centroid += r;
    }
    centroid *= 0.2;

    // ── 重投影：本帧像素 → 上一帧屏幕 uv ──
    // ⚠ **uv 用 baseUv，不是 input.uv**（批 13.5，写错就白改）：混合用的颜色 `cur` 取自
    //   baseUv ⟹ 输出像素 input.uv 代表的是**基位置 input.uv**；而深度按 input.uv 取是
    //   对的（它就是本像素的表面深度），但**重建用的 uv 必须是基位置**——抖动投影下
    //   「基位置 β 的世界点」出现在像素 β + c_t，故喂 baseUv = input.uv + c_t 才解出
    //   基位置 = input.uv 的那个世界点。喂 input.uv 会解出基位置 = input.uv − c_t 的世界点
    //   ⟹ 历史整体偏 c_t（≤0.5px 且随相位变）⟹ shimmer 只是换个形式，没消掉。
    //   （单测 `test_taa_jitter` 的「静止机位历史落回本像素」不变量就是为这条而写。）
    const float depth = depthTex.Load(int3(input.position.xy, 0)).r;
    float3 world = worldFromDepth(baseUv, depth);
    float4 worldPrev = float4(world, 1.0);
    if (depth < 1.0)  // 天空不随场景自旋（motion 是场景旋转补偿）
        worldPrev = mulM(readTaaMotion(), worldPrev);
    const float4 prevClip = mulM(readTaaPrevViewProj(), worldPrev);
    if (prevClip.w <= 0.0)  // 上一帧相机背后：无历史可言
        return float4(cur, 1.0);
    const float2 prevNdc = prevClip.xy / prevClip.w;
    // ndc → uv 与 worldFromDepth 的 y 翻转互逆：uv.y = (1 − ndc.y)/2
    const float2 prevUv = float2(prevNdc.x * 0.5 + 0.5, (1.0 - prevNdc.y) * 0.5);
    // ── 基空间对齐（批 13.5）──
    // prevViewProj 是**抖动后**矩阵 ⟹ 上面的 prevUv 落在上一帧的**抖动**网格上
    // （= β_{t−1}(w) + c_{t−1}，β = 基网格位置），而历史图自批 13.5 起住**基空间**（按 β
    // 索引）⟹ 减 c_{t−1} 才对齐。静止机位下这一步把历史采样偏移从 c_{t−1} 拉回 **0**
    //（改前：每帧按 δ_t 亚像素重采样历史 = 「固定频率 + 固定位置」shimmer 的成因）。
    // ⚠ 与 prevViewProj 的「抖动后」口径**成对**：把它改成未抖矩阵就必须删掉本行
    //   （否则 = 多减一次 c_{t−1}，同样错）。见 scene.slang / scene_data.cppm 同段警告。
    const float2 prevUvBase = prevUv - readTaaPrevContentShift().xy * texel;
    // 出画判据带**半个纹容差**：uv 的合法域是纹素中心 [0.5·texel, 1 − 0.5·texel]，
    // 而判据原来写的是 [0,1] —— 最外一圈的纹素中心本来就"出界"半个纹，于是**整列 x=1279
    // 或整行 y=0** 被误拒（mode 8 诊断图实测，交付配置 + 用户机位 js=1：旧判据逐帧拒
    // 0…2199 像素、17 帧均值 0.086%，命中集合随抖动相位在最外一行/最外一列之间切换；
    // 半纹判据下同一序列 **逐帧恒为 0**）。半纹 = 「重投影仍落在最外圈纹素之内」的最紧
    // 正确边界；再宽就会把真正的出画（clamp 边缘拿错像素）放进来。
    // ⚠ 判据跑在 **prevUvBase**（历史真正被索引的坐标）上，不是抖动网格的 prevUv。
    const float2 outTol = 0.5 * texel;
    if (any(prevUvBase < -outTol) || any(prevUvBase > 1.0 + outTol))
        return float4(cur, 1.0);  // 出画（本帧视锥外）：clamp 边缘 = 拿错像素，拒绝

    // ── 历史裁剪 + 混合 ──
    // ⚠ **参数序 = feedback 是「本帧权重」**（configs/scene.lua taa.feedback 的文档口径 +
    // Filament `mix(history, filtered, feedback)` 同序）：本帧 0.12 / 历史 0.88 ⇒ ≈19 帧
    // 收敛。写成 lerp(cur, hist, f) 会把权重反过来（本帧 0.88），累积窗塌到 ≈1.14 帧 =
    // 时域滤波名存实亡。批 12.6 T4 的阳性锚点实测抓到：A/F = 1.01×（闭式预测 3.95×）、
    // 全图直方图只多出**一个**混合级（历史只渗进 12%，永不继续累积）。
    // 推论：f=0 ⇒ 输出 = 裁剪后的**历史**（完全丢本帧的退化反馈环），不是「无滤波」；
    // 要做「只抖不滤」的消融请用 f=1（imgqa temporal 的 S1 组）。
    // 批 13.5 后：静止机位下 prevUvBase 恒等于本像素中心 ⟹ CR 权重退化成 (0,1,0,0)
    // = **点采样**（这正是「历史不再被反复低通」的算术表达，也是糊度应回落的原因）。
    const float3 hist = clampHdr(sampleHistoryCatmullRom(prevUvBase).rgb);
    const float3 histR = toReinhard(hist);

    const float3 clipped = fromReinhard(clipToAabb(centroid, histR, mn, mx));
    return float4(lerp(clipped, cur, params.x), 1.0);
}
