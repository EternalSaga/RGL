// 批 12.6 TAA（时域抗锯齿）——本帧成片 × 上一帧时域结果的自适应混合。
// mesh 复用 light.mesh.spv（全屏大三角，uv = framebuffer 对齐 1:1，同 tonemap/ao），
// state="taa"，执行器经 state 名解析管线条目（decorateTaaPass 插在 tonemap 之前）。
//
// ── 数据流（批 13.5 起：历史住**基空间**）──
//   本帧色（binding 4，jitter 投影渲出）+ 深度（binding 6，与 jitter 投影配套）
//   + AO 可见度（binding 9，批 13.6）
//   → **输入侧 un-jitter**（按本帧内容位移 c_t 双线性重建到基网格；批 13.5，见 main 内注释）
//   → **乘 AO**（批 13.6；采 baseUv + 线性采，两条纪律见 main 内注释）
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
//   · **AO 自批 13.6 起在本 pass 施加**（时域滤波之前，与 Filament 同构）⇒ AO 会被时域
//     糊一点，这是**设计意图**不是缺陷（Filament 靠半分辨率 + 重模糊 + 时域平均三件配合才
//     敢把 AO 做糊，本项目只有第三件）。改前 AO 在 tonemap（本 pass 之后）= 噪声零平均，
//     实测占残余 shimmer 62%。与 Filament 仍存的 gap：它只把 AO 乘间接光（IBL），我们全乘
//     ⟹ 边缘 AO 剧变会撑大本 pass 的邻域盒、历史被过度裁剪（债-39 续账）。
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
//   ⚠ **本否决的适用范围 = js=0 稳态，不得推广到闪烁**（批 13.11 复核，2026-10-06）：上面贴出的
//     读数全是 **js=0 恒等式**口径，而 js=0 时相机静止、**没有相位变化** ⟹ 该判据对「输出随抖动
//     相位移动」这条机制**结构性失明**（它量收敛点，闪烁量的是收敛点随相位怎么动）。注释里提到
//     的「闪烁抑制比」未贴读数，按现有记录无法声称它看见了闪烁。批 13.11 用闭式锚定比值 ρ 重测
//     （`imgqa temporal-residual --clip-abl`）：关盒 ρ = 0.0804 vs 闭式 ρ* = 0.0827 ⟹ **裁剪确实是
//     静止闪烁的主因**，与"盒的修法没用"并不矛盾（矛盾的是当年把它读成了"裁剪不影响时域累积"）。
//     「盒变宽 = 鬼影保护变松 ⟹ 只付风险不拿收益」这句同理只在"量不到收益"时成立。
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
[[vk::binding(9, 0)]] Texture2D<float> aoTex;        // SSAO 可见度（r8_unorm，ao pass 产出，1=通透）

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
/// 「本帧无历史 / 重投影出画」时写进 alpha 的值。
/// 门关与不携带状态的臂 = 1.0（同时是诊断码「本帧无历史」，与门关值同值，消费方按组别解读）；
/// **携带时域状态的臂（6 信任 / 7 记忆 / 8 mem 门控软裁剪 / 9 mem 接受 + ext 斜率 / 10 臂 8 + α
/// 逐像素）= 0.0** = 状态清零 ⟹ 跳切/出画处判据全副武装。
/// ⚠ 方向别写反：出画正是历史无效之处，写 1.0（满信任/满记忆）= 在最需要保护的时刻把保护关掉。
/// ⚠ **判据写 `>= 6` 而不是 `6..9` 闭区间**（2026-10-06 实测踩过）：臂 10 忘了扩上界 ⟹ 出画像素写
///   1.0 ⟹ 下一帧 `memPrev = 1.0` 顶满 `mem` ⟹ `w` 恒 0 = 历史全收，而 alpha **就是下一帧的 mem**
///   （不是纯诊断通道），错一个 bit 会跨帧复利。alpha 自 6 起就是这些臂共用的时域状态载体，
///   所以「≥ 6 一律清零」才是与机制对齐的写法；闭区间只会让每个新臂再踩一次。
float noHistoryAlpha(float mode)
{
    return mode >= 6.0 ? 0.0 : 1.0;
}

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

// ── Catmull-Rom 一维权重（**全文件唯一一份实现**，历史侧与输入侧共用）──
// ⚠ **核关于 a 是偶函数**（只依赖到纹素中心的距离），故三次项必须取 |a|：a3 = a²·|a|。
// 写成带符号的 a³ 会让 t=0 的权重从 (0,1,0,0) 变成 (0,1,−3,8)（Σ=6）——**恒等性破产**、
// 2D 直流增益 ≈36×，当时历史再经 saturate 顶到上界（取值域闸门已于批 13.1e 改成只夹
// 下界，见 clampHdr）⟹ 被 AABB 判成越界（phase13 §1.8）。
// 恒等性正是 imgqa 判据 `temporal-identity` 的算术基础（零抖动 ⇒ 输出必须逐字节 = R）。
void catmullRomWeights(float2 t, out float wx[4], out float wy[4])
{
    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        const float2 a = t + 1.0 - float2(i);  // 到抽头 b−1+i 的有向距离 ∈ (−2, 2]
        const float2 m = abs(a), a2 = a * a, a3 = a2 * m;
        // Catmull-Rom 核 k(m)：m≤1 → 1.5m³−2.5m²+1；1<m<2 → −0.5m³+2.5m²−4m+2
        wx[i] = select(m.x > 1.0, -0.5 * a3.x + 2.5 * a2.x - 4.0 * m.x + 2.0,
                                  1.5 * a3.x - 2.5 * a2.x + 1.0);
        wy[i] = select(m.y > 1.0, -0.5 * a3.y + 2.5 * a2.y - 4.0 * m.y + 2.0,
                                  1.5 * a3.y - 2.5 * a2.y + 1.0);
    }
}

// ── 精确 4×4 Catmull-Rom（16 次整数 Load，纹理作参数）──
// 为什么不能双线性：**历史就是累积图**，每帧一次双线性重采样 = 老贡献被反复低通，糊度**逐帧
// 复利**。实测（phase13 §1.5）：糊度随历史年龄单调上升（feedback 1.0/0.5/0.12/0.02 →
// +0.1/−27.3/−49.7/−57.0%），而正确的相位平均只该 −24.2% 且对 feedback 不敏感。
// 为什么不是「4 次双线性折叠 CR」（= Slang sampleBiquadric 那类技巧，本版本无该内建）：
// **CR 有负瓣**，双线性抽头权重恒 ∈[0,1]，数学上表示不了负瓣 → 不是 CR（Python 对拍暴力
// 4×4 CR 最大误差 0.146 实测否决）。故 16 次 Load 精确求值。
// ⚠ 取数走 **整数 Load + clamp**，不是采样器 ⟹ 共享 sampler 的 wrap=REPEAT 根本不参与
//   （输入侧换核时这条是越界风险的结构性解法，见 main 内「saturate」段）。
float3 sampleCatmullRom(Texture2D<float4> tex, float2 uv)
{
    uint pxW, pxH, mLvl;
    tex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));
    const float2 p = uv / texel - 0.5;              // 纹素中心坐标系（纹素 i 中心在 i+0.5）
    const int2 b = int2(floor(p));
    const float2 t = p - float2(b);

    float wx[4], wy[4];
    catmullRomWeights(t, wx, wy);
    float3 acc = 0.0;
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            const int2 q = clamp(b - 1 + int2(i, j), int2(0, 0),
                                 int2(int(pxW) - 1, int(pxH) - 1));
            acc += wx[i] * wy[j] * tex.Load(int3(q, 0)).rgb;
        }
    }
    return acc;
}

float3 sampleHistoryCatmullRom(float2 uv)
{
    return sampleCatmullRom(historyTex, uv);
}

// ── Lanczos3 一维权重（6 抽头，Filament 输入侧 un-jitter 用的那个核）──
// L(x) = sinc(x)·sinc(x/3)（|x| < 3），L(0) = 1、非零整数处解析零点 = 0。
// ⚠ **两条显式归零一条都不能省**：float32 的 sin(π) = 8.7e-8 ≠ 0，不显式归零则
//   「零偏移 = 精确插值」只在 1e-7 意义上成立 ⟹ 静止机位 + jitter_scale=0 的恒等式
//   （判据 `temporal-identity`：零抖动 ⇒ 输出逐字节 = R）**数值上破产**。x=0 那条是
//   sinc 的可去奇点（0/0），不是可选优化。
float lanczos3Weight(float x)
{
    const float ax = abs(x);
    if (ax < 1e-5)
        return 1.0;
    if (ax >= 3.0)
        return 0.0;
    const float r = round(ax);
    if (r >= 1.0 && abs(ax - r) < 1e-5)
        return 0.0;  // 解析零点（见上）
    const float px = 3.14159265358979323846 * x;
    const float p3 = px / 3.0;
    return (sin(px) / px) * (sin(p3) / p3);
}

// 精确 6×6 Lanczos3（36 次整数 Load + clamp，取数纪律同 sampleCatmullRom）。
// 支撑 = 双线性的一倍半、CR 再多一圈 ⟹ 边缘 3 像素内被整数 clamp 夹住（= CLAMP_TO_EDGE）。
float3 sampleLanczos3(Texture2D<float4> tex, float2 uv)
{
    uint pxW, pxH, mLvl;
    tex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));
    const float2 p = uv / texel - 0.5;
    const int2 b = int2(floor(p));
    const float2 t = p - float2(b);

    float wx[6], wy[6];
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        wx[i] = lanczos3Weight(t.x + 2.0 - float(i));  // 到抽头 b−2+i 的有向距离
        wy[i] = lanczos3Weight(t.y + 2.0 - float(i));
    }
    float3 acc = 0.0;
    [unroll]
    for (int j = 0; j < 6; ++j)
    {
        [unroll]
        for (int i = 0; i < 6; ++i)
        {
            const int2 q = clamp(b - 2 + int2(i, j), int2(0, 0),
                                 int2(int(pxW) - 1, int(pxH) - 1));
            acc += wx[i] * wy[j] * tex.Load(int3(q, 0)).rgb;
        }
    }
    return acc;
}

// ── 归一化 Lanczos3（核 3）：Lanczos3 的 6 抽头截断**不是单位分割**，把直流增益钉回 1 ──
// 解析（不是实测猜的）：Σᵢ L(t+2−i) = 1.000000 (t=0) → 0.994299 (t=0.5)，2D 可分离 ⟹
// 直流增益最多掉 **1.14%**，且**随亚像素偏移变**。作为「把内容从一个亚像素网格搬到另一个」
// 的重采样器这是缺陷：亮度随抖动相位泵动（每帧做一次、每帧相位不同）。归一化 = 每轴权重
// 除以该轴权重和。
// ⚠ 归一化**不破坏**「零偏移 = 精确插值」：t=0 时 Σw 恰为 1，除以它是恒等变换，权重仍是
//   (0,0,1,0,0,0)。这条性质是 `temporal-identity`（零抖动 ⇒ 输出逐字节 = R）的算术基础，
//   实测背书 = temporal-feedback 的零态锚点 F1.0js0/dev(M0)（核 2 = 0.999475、核 3 同）。
float3 sampleLanczos3Normalized(Texture2D<float4> tex, float2 uv)
{
    uint pxW, pxH, mLvl;
    tex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));
    const float2 p = uv / texel - 0.5;
    const int2 b = int2(floor(p));
    const float2 t = p - float2(b);

    float wx[6], wy[6];
    float sx = 0.0, sy = 0.0;
    [unroll]
    for (int i = 0; i < 6; ++i)
    {
        wx[i] = lanczos3Weight(t.x + 2.0 - float(i));
        wy[i] = lanczos3Weight(t.y + 2.0 - float(i));
        sx += wx[i];
        sy += wy[i];
    }
    float3 acc = 0.0;
    [unroll]
    for (int j = 0; j < 6; ++j)
    {
        [unroll]
        for (int i = 0; i < 6; ++i)
        {
            const int2 q = clamp(b - 2 + int2(i, j), int2(0, 0),
                                 int2(int(pxW) - 1, int(pxH) - 1));
            acc += (wx[i] / sx) * (wy[j] / sy) * tex.Load(int3(q, 0)).rgb;
        }
    }
    return acc;
}

/**
 * 批 13.8：输入侧 un-jitter 的**重建核选择**（门 `taa.input_kernel`，默认 0 = 双线性）。
 *
 * 为什么要有这个门：批 13.2 当年定案「双线性」用的两条判据（梯度能量 / `aa-coverage`）都是
 * **静止口径**，运动下从没重测过；而现在有了 `imgqa temporal-flicker` 的闭式刻度 ζ。本门
 * 存在的目的 = 让「换核」这件事能在**同一构建**里 A/B，而不是靠改代码重编。
 *
 * 三档：0 = 双线性（4 抽头，走采样器）｜ 1 = Catmull-Rom 4×4（复用历史侧同一份权重实现）｜
 *       2 = Lanczos3 6×6（Filament 输入侧那个）。
 * ⚠ **门关（0）走的是原样那一条 `SampleLevel`**，一个算术 bit 都没动（红线 = `just shot`
 *   sha256 逐位不变）。分支值来自 scene data（uniform），不是非一致分支。
 * ⚠ **1/2 两档的取数走整数 Load + clamp**，共享 sampler 的 wrap=REPEAT 不参与 ⟹ 越界风险
 *   不是「更大」而是**结构性消失**（论证见 main 内 saturate 段，那条只对本门的 0 档必要）。
 */
float3 sampleInputColor(float2 uv)
{
    const float k = readTaaInputKernel();
    if (k < 0.5)
        return colorTex.SampleLevel(linearSampler, uv, 0.0).rgb;  // 0 = 双线性（默认 = 今天）
    if (k < 1.5)
        return sampleCatmullRom(colorTex, uv);  // 1 = Catmull-Rom 4×4
    return sampleLanczos3(colorTex, uv);        // 2 = Lanczos3 6×6
}

/**
 * 批 13.6：本帧成片 × AO 可见度 = **时域滤波的真正输入**（AO 已烘进去，与 Filament 的
 * color pass 输出同构）。邻域盒与本帧值都必须走它，否则盒是 AO-前的、被裁的值是 AO-后的
 * ⟹ 遮蔽边缘会把本帧值裁出盒外（改前两者天然一致：中心抽头恒等于 cur）。
 * ⚠ 采样纪律两条缺一不可，推导见 main 内「AO 施加点」段（① uv 传基空间坐标
 *   ② 必须线性采——点采会让 AO 随相位跳纹素，时域平均不掉）。
 * 批 13.8：颜色那一路换走 `sampleInputColor`（核可选），**AO 那一路恒为线性采样**——
 * AO 是 r8 光滑场，把它也换成宽核 = 把「重建核」与「AO 模糊量」两个被测项混成一个，
 * 且批 13.6 的纪律②只要求「连续于相位」，线性采已满足。
 */
float3 sampleSceneAo(float2 uv, float aoStrength)
{
    const float ao = aoTex.SampleLevel(linearSampler, uv, 0.0).r;
    return sampleInputColor(uv) * lerp(1.0, ao, aoStrength);
}

[shader("fragment")]
float4 main(TaaInput input) : SV_Target
{
    const float4 params = readTaaParams();  // (feedback, historyValid, c_t.x px, c_t.y px)
    // 历史拒收诊断门（`taa.diag_reject`，默认 0 = 一切 alpha 恒 1.0 = 今天）：开 = 把
    // 「本像素的历史怎么了」编进输出 alpha（RGB 一个 bit 不动，tonemap / 历史采样只读 rgb）：
    //   0.0 = 原样收下（histR 在邻域盒内）｜ 0.25 = 被盒裁剪（部分保留）
    //         ⟹ **本桶才是「α 调多小都没用」的出处**：裁剪后的历史值 = 本帧 5 抽头邻域盒的
    //         **边界**（本帧相位的函数），α 再小也只是「本帧的函数」再乘个小权重
    //         ⟹ 累积被**短路**（不是"历史被拒"，是"历史变成了本帧"）
    //   0.5 = **塌回邻域盒 centroid = 拒收**（clipToAabb 返回 c，历史贡献变成本帧邻域均值）
    //   0.75 = 重投影出画拒收（w≤0 / 出界）｜ 1.0 = 本帧无历史（historyValid 旁路）
    // ⚠ 归因改判（2026-10-06，实测，**本注释此前把这条挂在 0.5 上是错的**）：用户报障机位
    //   （`imgqa temporal-residual --bistro --camera brickfence --reject`）五桶实测
    //   「塌回 centroid = **0.00%**、盒裁剪 = **6.60%**」⟹ 病灶在 **0.25** 不在 0.5。
    //   且短路是**部分**的（被夹档抑制比 ρ ≈ 0.30–0.35 而非 1 = 未夹帧仍在累积）。
    //   全案 → `调试历史.md` 2026-10-06 / 债-59。
    // 回读 = `POST /shot {"attachment":"taa_out"}`（alpha 经 raw8 直出，不走 gamma）。
    const float diag = readTaaDiagReject();
    // ⚠ texel 必须先于一切采样取到：un-jitter 与出画判据都要它。
    uint pxW, pxH, mLvl;
    colorTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 texel = 1.0 / float2(float(pxW), float(pxH));

    // ── 输入侧 un-jitter（批 13.5）：把本帧从抖动网格搬到基网格 ──
    // 抖动后渲染的图像内容整体平移 +c_t（`I_t(x) = F(x − c_t)`）⟹ 按 `+c_t` 采样即落回
    // 基网格。**这是本 pass 时域稳定性的来源**：混合从此发生在基空间，历史不再每帧被
    // 亚像素重采样（静止机位下历史采样偏移恒 0）⟹ 16 帧周期固定位置 shimmer 的成因消失。
    // 核 = **门 `taa.input_kernel` 选**（0 双线性 = 默认 = 逐位今天 / 1 CR 4×4 / 2 Lanczos3
    // 6×6，见 `sampleInputColor`）。批 13.2 当年定案双线性时写下的理由（数据原样搬在下面，
    // **勿丢**）当时是「刻意不用 CR/Lanczos」；批 13.8 把它变成可 A/B 的门，因为那两条判据
    // 全是**静止口径**，而运动下从未重测过——现在有了闭式刻度 ζ（`imgqa temporal-flicker`）。
    // 【实测背书，原样搬自批 13.2 的 resolve 核选型（同一族问题，数据勿丢）】
    //   本机 bistro 1280×720、交付配置 + 用户机位、fb=.12 js=1、2026-10-04：
    //     梯度能量（越高越锐）    不重采样 0.038272 ｜ 双线性 **0.034843** ｜ CR 0.036999
    //     `imgqa aa-coverage` 域内最大误差  0.138 列 ｜ 双线性 **0.119 列** ｜ CR 0.181 列
    //     黑晕「凭空多黑」        0.026 列 ｜ 双线性 0.016 列 ｜ CR **−0.030 列**
    //   ⟹ CR 确实更锐（+6.2% 梯度），但在**本项目已登记的抗锯齿判据**上退化（0.119 → 0.181，
    //     比不重采样基线 0.138 还差），且「凭空多黑」翻成**负号** = 负瓣在黑白边界造振铃。
    //     梯度能量这条**没有闭式参考**，单看它会得出相反结论（`taa_plan` §7 第 10 条同一课：
    //     拿没有闭式的量当主判据，结论随量的选择翻转）。故按有闭式的那条定案 = **双线性**。
    // 【批 13.8 运动态重决（ζ 闭式刻度 + 静止 aa-coverage 双判据）→ 本文件末尾的判决段】
    // ⚠ **必须显式 saturate 钳回 [0,1]**：共享 sampler 的 wrap 是 **REPEAT**
    //   （`sampler_library.cppm` 平铺 UV 定案，bistro 49.5% primitive 越界 [0,1]），而
    //   屏幕空间 uv 越界绕回 = 最外一圈采到**对侧**像素（1px 宽的假边）。|c_t| ≤ 0.5px
    //   ⟹ baseUv 最多越界半纹，正好触发。钳到 [0,1] 等价 CLAMP_TO_EDGE，双线性核对
    //   [0,1] 内的亚像素越界本就把取数钳到边缘纹素，**不改内部像素一个 bit**。
    //   （此坑原载于批 13.2 tonemap resolve 注释，随 un-jitter 前移一并搬来；
    //    邻域盒的 ±1 texel 抽头**不**钳——改前就如此，钳了会动基线。）
    // ⚠ **换核（1/2 档）时这条论证重做过一遍，结论是「saturate 留着，但它不再是唯一防线」**：
    //   ① CR/Lanczos 的取数是 **整数 Load + `clamp(q, 0, size−1)`**，**根本不经过采样器**
    //     ⟹ wrap=REPEAT 结构性不参与，「支撑更宽 ⟹ 越界风险更大」这条在 Load 路下不成立
    //     （整数 clamp 就是 CLAMP_TO_EDGE，且是**逐抽头**钳，比只钳中心坐标的 saturate 更紧）。
    //   ② 但 saturate **一条都不能删**：`baseUv` 同时喂给 aoTex 的 `SampleLevel`（批 13.6
    //     纪律①②）与 0 档的 `SampleLevel`，那两路**仍走 REPEAT 采样器**，钳没了就还是
    //     1px 假边。③ 副作用如实记：1/2 档下邻域盒的 ±1 抽头在最外 2–3 像素由「绕回对侧」
    //     变成「钳到边缘」——与 0 档不同，但**只在最外一圈**，且新行为才是本来的意图。
    const float2 baseUv = saturate(input.uv + params.zw * texel);
    // ── 批 13.6：AO 施加点搬进本 pass（= 时域滤波**之前**），tonemap 同帧跳过 ──
    // 【为什么搬】AO 值在深度不连续处随抖动相位剧变 = **真实锯齿**，而抖动 + 时域平均正是消
    //   它的工具。改前 AO 乘在 tonemap（本 pass **之后**）⟹ 一点没被平均：实测占残余 shimmer
    //   62%（AO 开 6.899% / AO 关 2.639%），且按梯度分层 **100% 住在边缘**（边缘带 29.6% /
    //   平坦区 0.088%）。Filament 同构：AO 在 color pass 内（`details/Renderer.cpp:1190` SSAO
    //   → `:1370` Color Pass → `:1426` 才起后处理/TAA）。全案 → docs/taa_history_space_plan.md。
    // 【两条采样纪律，少一条 = 白搬】
    //   ① **采 baseUv 不采 input.uv**：aoTex 纹素 q 装的是**抖动像素 q** 的遮蔽量 = 基位置
    //      q − c_t 的遮蔽量 ⟹ 要基位置 p 的 AO 必须采 p + c_t。采 input.uv 会引入一个**随相位
    //      变的 ≤0.5px 错位** = 把刚消掉的相位相关量从输入侧重新塞回来。
    //   ② **线性采，不点采**：baseUv 是分数坐标，点采会随相位**跳纹素** ⟹ AO 仍是相位阶跃
    //      函数，EMA 收不拢；线性采把它变成相位的连续函数才谈得上被平均。
    //      （批 13.5b 在 ao.frag 里搬 baseUv 之所以**零指标收益**，根因就是它只能点采——
    //       D32Sfloat 深度不可过滤。AO 是 r8_unorm，可过滤，本条才成立。两者别混为一谈。）
    // strength = 0 时 lerp(1, ao, 0) ≡ 1.0（IEEE 精确）⟹ 逐位旁路，与 ao pass 侧同一技巧。
    const float aoStrength = readAoStrength();
    const float3 cur = sampleSceneAo(baseUv, aoStrength);

    // 逐位旁路早退（首帧 / resize / 跳切；见头注释"逐位旁路"段）
    // ⚠ 返回的是**已 un-jitter** 那份，不是原始抖动帧：taa_out 每帧都 blit 进 taa_hist 当
    //   下一帧历史，早退帧若留在抖动空间，下一帧就会拿抖动空间图当基空间历史用（偏 c_{t−1}）。
    if (params.y < 0.5)
        // alpha：门关/非信任臂 = 1.0（同时是诊断码「本帧无历史」，与门关值同值）；
        // 信任门（mode 6）= **0.0** = 跳切/首帧后信任清零（裁剪重新全副武装），否则跳切瞬间
        // 会继承上一段画面的高信任 ⟹ 鬼影保护在最需要的时刻缺席。
        return float4(cur, noHistoryAlpha(readTaaClipControl().x));

    // ── 邻域 AABB（box5 十字：中心 + 上下左右，Reinhard 域）──
    // 消融门 `taa.clip_mode`（批 13.11；0 = 默认 = **逐位今天**，下面两个 if 都不进）：
    // 3 = box9（3×3 重算盒）。⚠ 默认那条 5 抽头循环**一字不动**，box9 走独立分支——
    //   把两条揉进一个循环会改累加序（centroid 是求和均值），门红线就没了。
    const float4 clipCtl = readTaaClipControl();
    const int clipMode = int(clipCtl.x + 0.5);
    float3 mn = 1.0, mx = 0.0, centroid = 0.0;
    const int2 offs[5] = { {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1} };
    // 抽头绕 **baseUv**（不是 input.uv）：盒要与 cur/历史同处基空间，否则盒是抖动系的、
    // 被裁的值是基系的 ⟹ 裁剪边界系统性偏 c_t（批 13.5 前两者都在抖动空间，无此问题）。
    for (int i = 0; i < 5; ++i)
    {
        // ⚠ 走 sampleSceneAo 而不是裸 colorTex（批 13.6）：被裁的值是 AO-后的，盒也必须是
        //   AO-后的，否则遮蔽边缘会把本帧值裁出盒外。改前两者天然一致（中心抽头恒等于 cur），
        //   AO 搬进来后这份一致性得自己维持。抽头 uv **不** saturate（改前就如此）。
        const float3 t = clampHdr(sampleSceneAo(baseUv + float2(offs[i]) * texel, aoStrength));
        const float3 r = toReinhard(t);
        mn = min(mn, r);
        mx = max(mx, r);
        centroid += r;
    }
    centroid *= 0.2;
    if (clipMode == 3)
    {
        mn = 1.0;
        mx = 0.0;
        centroid = 0.0;
        for (int dy = -1; dy <= 1; ++dy)
        {
            for (int dx = -1; dx <= 1; ++dx)
            {
                const float3 t = clampHdr(sampleSceneAo(baseUv + float2(dx, dy) * texel, aoStrength));
                const float3 r = toReinhard(t);
                mn = min(mn, r);
                mx = max(mx, r);
                centroid += r;
            }
        }
        centroid *= (1.0 / 9.0);
    }
    // 2 = 绕 centroid 把盒扩 1.5×（§1.7「尺寸不是杠杆」的重测臂：那条结论跑在 CR 核写成
    // 奇函数、历史被乘 36 的坏构建上，`hist` 本身是错的 ⟹ 已降为「未证」= 债-60）。
    // 5 = 绕 centroid 扩 **K×**（K = clipCtl.y，**参数化扩盒臂**）。为什么要有 5：首轮实测
    //   （2026-10-06）1.5× 与 box9 都几乎无效（ρ 0.2501→0.22/0.21）而关盒精确命中闭式
    //   （ρ = 0.0804 vs ρ* = 0.0827）⟹ 被夹历史不是「差一点出盒」而是「出盒很远」。本臂扫 K
    //   就是为了量「盒到底差多少倍」——那是时域膨胀 β 的设计输入，不是拿来当修法交付的
    //   （静态大盒 = 鬼影保护同样没了）。
    if (clipMode == 2 || clipMode == 5)
    {
        const float k = clipMode == 2 ? 1.5 : max(clipCtl.y, 1.0);
        mn = centroid + (mn - centroid) * k;
        mx = centroid + (mx - centroid) * k;
    }

    // ── 重投影：本帧像素 → 上一帧屏幕 uv ──
    // ⚠ **uv 用 baseUv，不是 input.uv**（批 13.5，写错就白改）：混合用的颜色 `cur` 取自
    //   baseUv ⟹ 输出像素 input.uv 代表的是**基位置 input.uv**；而深度按 input.uv 取是
    //   对的（它就是本像素的表面深度），但**重建用的 uv 必须是基位置**——抖动投影下
    //   「基位置 β 的世界点」出现在像素 β + c_t，故喂 baseUv = input.uv + c_t 才解出
    //   基位置 = input.uv 的那个世界点。喂 input.uv 会解出基位置 = input.uv − c_t 的世界点
    //   ⟹ 历史整体偏 c_t（≤0.5px 且随相位变）⟹ shimmer 只是换个形式，没消掉。
    //   （单测 `test_taa_jitter` 的「静止机位历史落回本像素」不变量就是为这条而写。）
    // ⚠ 显式 int2：`input.position.xy` 是 float2，隐式转 int2 会出 slang warning[E30081]。
    //   这条警告**潜伏了很久**——增量构建不重编本文件，`just build` 就一直不报（2026-10-05 记：
    //   "just build 0 警告" 在增量下只覆盖本轮真正重编的文件，别把它当全量结论）。
    const float depth = depthTex.Load(int3(int2(input.position.xy), 0)).r;
    float3 world = worldFromDepth(baseUv, depth);
    float4 worldPrev = float4(world, 1.0);
    if (depth < 1.0)  // 天空不随场景自旋（motion 是场景旋转补偿）
        worldPrev = mulM(readTaaMotion(), worldPrev);
    const float4 prevClip = mulM(readTaaPrevViewProj(), worldPrev);
    if (prevClip.w <= 0.0)  // 上一帧相机背后：无历史可言
        return float4(cur, diag > 0.5 ? 0.75 : 1.0);  // 诊断码 0.75 = 重投影拒收
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
        return float4(cur, diag > 0.5 ? 0.75 : noHistoryAlpha(readTaaClipControl().x));
        // 出画（本帧视锥外）：clamp 边缘 = 拿错像素，拒绝历史

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

    // ── 【实测裁决 2026-10-05】实验 M4「本帧上界夹」= 否决，勿再实现 ──
    // 症状（= HEAD 1a15240 的债-40：白点不是「边缘闪」，是本帧 HDR firefly 从不被裁剪）：
    //   glint 亮进历史后，即使本帧已经不亮，历史仍把它带回来；box5 AABB 拦不住——glint
    //   自己就在邻域中心抽头里，盒被它自己撑大。M4 改问「历史值是否超过**本帧该邻域的
    //   最大值**」（与盒无关），半径走 scene data 实验区。
    // 半径 2 实测（bistro 1280×720 湿面机位，TAA 开）：
    //   L≥250 0.48557% → 0.04178%（**−93%**，看着很好），但 **meanL 99.21 → 80.29（−19%）**、
    //   门板机位 meanL 131.4 → 107.6（−18%）、grain>2 32.19 → 50.82、>20 9.077 → 16.78、
    //   >64 1.824 → 4.039、**phase-lock 43 → 194**（全变差）。
    // ⟹ 那 93% 是**整体压暗 19% 换来的**，不是裁剪变准。机制：把历史夹到本帧邻域最大值
    //   = 系统性拒绝「比本帧亮的合法累积」，TAA 退化成"本帧直出 + 压暗"。半径 1 上一轮
    //   已否决（glint 的峰值相位是 2×2 块，邻域自己就亮），放大支撑只是把"拒绝历史"做得
    //   更狠，不是更准。**这条是"遮"且遮不干净。**
    const float3 hardR = clipToAabb(centroid, histR, mn, mx);
    // 诊断 alpha（门关 = 恒 1.0 = 今天；RGB 的 lerp 一个 bit 不动）。
    // 「拒收」判据 = clipToAabb **恰返回 centroid**（塌缩分支逐位返回 c，见其函数体两条
    // return c；此时历史贡献 = 本帧邻域均值 ⟹ 输出 ≈ 本帧，α 无意义）。
    // 「收下」判据 = histR 逐通道在盒内（**不能**拿 clipped == histR 判：c + d·1.0 在浮点
    // 上不逐位等于 p，盒内也会被判成裁剪——实测过的浮点陷阱）。
    // ⚠ 码口径 = **硬裁剪**结果，与 `clip_mode` 的 1/4 臂无关（那两臂盒没变、只是响应不同）
    //   ⟹ 拒收交叉表跨臂可比：它答的是「本像素的盒本会怎么判」，不是「本臂实际做了什么」。
    //   （2/3 两臂改的就是盒本身，码跟着盒走才是本意。）
    float alpha = 1.0;
    if (diag > 0.5)
    {
        const bool accepted = all(histR >= mn) && all(histR <= mx);
        const bool collapsed = all(hardR == centroid);
        alpha = accepted ? 0.0 : (collapsed ? 0.5 : 0.25);
    }
    // ── 消融/修法臂（批 13.11；mode 0 = 走 else 分支 = **逐位今天**）──
    // 1 = 关盒（短路效应的天花板参照）｜ 4 = **软裁剪**：历史每帧最多走完「到盒边界距离」的
    // λ 分之一（弹簧不是墙）。为什么软裁剪打的是根因：硬裁剪把「一次相位相关的误判」放大成
    // **整份历史重置**（`lerp(clipped, cur, α)` 里 clipped 的权重是 1−α），软裁剪把它压成
    // λ 分之一 ⟹ 注入的相位依赖方差 ∝ (p·λ)，而真鬼影仍收敛，只是慢 1/λ 帧。
    //   实测（2026-10-06）：λ=0.1 → ρ 0.1795（闭式 0.0827、基准 0.2501）= **有效但远不够**。
    // 6 = **历史信任门**（实测**否决**，理由见该分支）｜ 7 = **标量时域 luma 膨胀**（主推）。
    float3 clippedR = hardR;
    // 混合权重（本帧权重）。默认 = params.x（全画面一个数）；**只有臂 10 会改它** ⟹ 其余臂
    // 逐位不变（门红线）。臂 10 的论点见其分支。
    float feedback = params.x;
    if (clipMode == 1)
        clippedR = histR;
    else if (clipMode == 4)
        clippedR = histR + (hardR - histR) * clipCtl.y;
    else if (clipMode == 6 && diag < 0.5)
    {
        // 6 = **历史信任门**（修法臂，批 13.11）。把「本位置的盒最近有多常判收下」存进
        //   taa_out.a（由既有 taa_out→taa_hist 帧末 blit 携带 ⟹ **零新附件、零新 pass**），
        //   用它调制裁剪的**咬合深度**：clipped = lerp(hist, hardClip, 1 − trust)。
        //   trust = 1 → 完全不裁（数值上就是臂 1）；trust = 0 → **逐位今天的硬裁剪**。
        // 🔴 **实测否决（2026-10-06，brickfence）**：三档 γ_up ∈ {0.03, 1/16, 0.125} 只把 ρ 从
        //   0.2501 拖到 0.2457/0.2422/0.2385（3–5%），且**按 γ 单调**（⟹ 通道是活的，不是断链；
        //   真断链会逐位相同、被在场性守卫抓住）。根因是**机制的内生矛盾**：平衡点
        //   t* ≈ γ_up·p /(γ_up·p + γ_dn·(1−p))，而**需要救的像素正是通过率 p 低的那批** ⟹ t* 低
        //   ⟹ 咬合不松；γ_dn 为了鬼影保护必须快塌，进一步压低这批像素的信任。**换轴，见臂 7。**
        // 更新**非对称**（收下慢升、裁剪快塌）：
        //   收下 → trust += γ_up·(1−trust)   γ_up = 1/16，出处 = **抖动周期**：相位没走完
        //     一个周期，"误报率"根本无从估计（这不是可调超参，是采样论下的最短窗）
        //   裁剪 → trust *= (1−γ_dn)          γ_dn = 0.5（≈2 帧塌到 1/4），出处 = 鬼影保护必须在
        //     **最短累积窗**（交付 α=0.12 ⇒ ≈8 帧）的一小部分内恢复，否则鬼影先于保护出现
        // 为什么这是**通用**修法而不是 bistro 补丁：单帧信息**原理上**分不开「合法的亚像素细线」
        //   与「陈旧历史」——两者在单帧里逐字节同形（这正是扩盒/box9 被证伪的原因：它们想在
        //   单帧里解决一个时域问题）。要分开只能引入时域状态，而"这个位置最近有多常通过本测试"
        //   就是**本测试自己的误报率**，与内容、与场景、与机位都无关。
        const float trustPrev =
            saturate(historyTex.SampleLevel(pointSampler, prevUvBase, 0.0).a);
        const bool accepted = all(histR >= mn) && all(histR <= mx);
        const float gUp = clipCtl.y > 0.0 ? clipCtl.y : (1.0 / 16.0);  // y = 扫描覆盖位
        const float trust = accepted ? lerp(trustPrev, 1.0, gUp) : trustPrev * 0.5;
        clippedR = lerp(histR, hardR, 1.0 - trust);
        alpha = trust;  // 通道载体 = 交付中间图的 alpha（tonemap 只读 rgb，历史 blit 全 4 通道）
    }
    else if (clipMode == 7 && diag < 0.5)
    {
        // 7 = **标量时域 luma 膨胀**（修法臂，批 13.11；与臂 6 共用 alpha 载体 ⟹ 互斥、零新附件）。
        // 换轴：臂 6 折扣「测试该不该信」，本臂**扩大测试的接受域**——问的不是"要不要信这次裁剪"，
        //   而是"这个亮度**近几帧有没有在这里出现过**"：
        //       mem = max(本帧邻域盒的 luma 跨度, mem_prev · decay)
        //       收下 ⟺ |histLuma − centroidLuma| ≤ mem
        // 为什么它没有臂 6 的内生矛盾：臂 6 的信任平衡点 t* ≈ γ_up·p/(γ_up·p + γ_dn·(1−p))，
        //   需要救的像素正是通过率 p 低的那批 ⟹ t* 低 ⟹ 咬合不松（实测三档 γ 只动 3–5%，
        //   且按 γ 单调 = 通道是活的、机制是弱的）。本臂的 mem 由**实际出现过的跨度**撑大，
        //   与通过率无关：砖缝这种"本帧 5 抽头全落在暗面（ext≈0）、而合法历史是中间灰"的像素，
        //   mem 会被上一帧"抽头里有亮砖"的大 ext 撑住 ⟹ 历史被收下。
        // 同时它**不是关盒**：mem 有界（≤ 近几帧真实出现过的跨度），一个亮物体离开后 ext 塌回
        //   背景、mem 按 decay 几何衰减 ⟹ 几帧后鬼影因其**距离超过记忆**而被拒——这是距离判据，
        //   不像臂 6 只调咬合深度、不管历史偏离多远。
        // decay = 0.5 的出处（**无量纲**，非扫描所得）：鬼影保护须在最短路累积窗（交付 α=0.12
        //   ⇒ ≈8 帧）的一小部分内恢复 ⟹ 记忆长度取 ≈2 帧。y = 覆盖位，仅供扫描护栏用。
        const float memPrev = saturate(historyTex.SampleLevel(pointSampler, prevUvBase, 0.0).a);
        const float decay = clipCtl.y > 0.0 ? clipCtl.y : 0.5;
        const float3 ext3 = mx - mn;  // 盒的逐通道跨度（Reinhard 域），取最大当标量 slack
        const float ext = max(ext3.x, max(ext3.y, ext3.z));
        const float mem = max(ext, memPrev * decay);
        // luma 用 Reinhard 域等权均值：本臂只要求**同一域内可比**，不是显示亮度
        const float histL = dot(histR, float3(0.3333333));
        const float centL = dot(centroid, float3(0.3333333));
        clippedR = abs(histL - centL) <= mem ? histR : hardR;
        alpha = mem;
    }
    else if (clipMode == 8 && diag < 0.5)
    {
        // 8 = **mem 门控软裁剪**（批 13.11 续；专治臂 7 的极限环，判据与指纹 → `调试历史.md` §10）。
        // 臂 7 的**判据**是对的（接受域时域不变 ⟹ 静止内容零误报，四机位 ρ 全贴闭式），
        // 坏在**响应是硬开关**：`clipped = |histL−centL| ≤ mem ? histR : hardR`。
        // 阈值 mem 本身是 τ = 1/(1−d) = 16 帧的衰减量 ⟹ 「**带滞后的继电器反馈**」= 教科书级
        // 极限环发生器。实测指纹四条全中：幅度大（0.91 线性域 = 黑↔白）、面积小（278–857px =
        // 0.03–0.09%）、对 d 不敏感（0.9/0.9375/0.9688/0.975 全在 0.4 附近）、对热身不敏感
        // （×4 不塌 ⟹ 是吸引子不是瞬态）。
        // 本臂**只换响应、不换判据**：以 mem 为**下界**的相对带宽 r 的线性斜坡（无量纲、与场景
        // 亮度尺度无关），把环路增益从 ∞ 降到 ≈1/r。继电器连续化后极限环要靠增益过阈值才
        // 能维持 ⟹ 这条是**可证伪**的。实测（2026-10-06）：超 τ 像素数 278–857 → **0**，且在
        // r ∈ [0.25, 2] 全 range 成立 ⟹ 继电器机制坐实。
        const float memPrev =
            saturate(historyTex.SampleLevel(pointSampler, prevUvBase, 0.0).a);
        // decay 的出处 = **抖动周期**（记忆覆盖一整个相位支撑集，见 §1.13 ②/⑦），非扫描值。
        const float decay = clipCtl.y > 0.0 ? clipCtl.y : (1.0 - 1.0 / 16.0);
        const float3 ext3 = mx - mn;
        const float ext = max(ext3.x, max(ext3.y, ext3.z));
        const float mem = max(ext, memPrev * decay);
        const float histL = dot(histR, float3(0.3333333));
        const float centL = dot(centroid, float3(0.3333333));
        const float dist = abs(histL - centL);
        // ⚠ **过渡带必须整个落在接受域之外** = [mem, mem·(1+r)]，**不是**对称带
        //   [mem(1−r), mem(1+r)]。对称带实测（2026-10-06，brickfence，r = 0.25/0.5/1/2）把 ρ 从
        //   0.0909 单调推到 0.2104，只有 r ≤ 0.5 过线且 0.5 只剩 7% 余量 ⟹ 参数成了扫出来的。
        //   根因是**设计错误不是权衡**：零误报证明的内容是「静止内容上 dist ≤ mem」，而对称带在
        //   dist = mem 处给 w = 0.5 = **恰好在证明要求全接受的地方裁掉一半历史**。
        //   单边带 ⟹ 接受域内 w ≡ 0 ⟹ 静止内容与臂 7 逐位同（ρ 应 = 0.0958）、且 ρ 对 r 不敏感
        //   ⟹ r 退化成「被拒历史多快换掉」= 鬼影侧时间常数，不再是闪烁侧参数。
        // r = 1 是**唯一不含自由数值的取值**：过渡带宽度 = 记忆半径本身 ⟹ w = saturate(dist/mem − 1)，
        // 公式里没有任何可调常数（实测三点 r=0.25/0.5/1.0 的 ρ = 0.0816/0.0814/0.0814，与 r 脱钩）。
        // ⚠ r 的**下界是测出来的不是推出来的**（增益判据给不出干净常数，mem 自己在动）：实测
        //   r ≥ 0.5 稳定（超 τ = 0 px），r = 0.25 炸回 662 px = **阳性对照**（连续化不够，环路
        //   增益 ≈1/r 必须低于阈值）。选 r=1 = 距失效点 2× 余量、且 max swing 三点最低。
        const float r = 1.0;  // 相对带宽 ⟹ 过渡带 = [mem, mem·(1+r)]，接受域内恒为 0
        const float w = saturate((dist - mem) / (r * max(mem, 1e-6)));
        clippedR = lerp(histR, hardR, w);
        alpha = mem;  // 与臂 6/7 共用载体 ⟹ 互斥、零新附件（imgqa 侧对这三臂跳过拒收码表）
    }
    else if (clipMode == 9 && diag < 0.5)
    {
        // 9 = **接受半径用 mem（粘）、响应斜率用 ext（快）**（批 13.11 续；债-62 的修法）。
        // 臂 7/8 的教训是「一个名字两种量」：mem 一份标量在打两份冲突的工——闪烁要它**粘且宽**
        // （覆盖 16 个相位的支撑集），鬼影要它**快且窄**（几帧内清掉陈旧历史）。响应函数取硬端
        // （臂 7）治得了鬼影但起极限环，取粘端（臂 8）闪烁到闭式但鬼影≈关盒（R 3.028）。
        // 本臂把两份工**拆给两个已经算好的量**：
        //   · 接受判据 = `dist ≤ mem`（与臂 7/8 一字不改 ⟹ 闪烁性质原样保留，接受域内 w ≡ 0）
        //   · 斜坡宽度 = `ext`（**本帧**盒 luma 跨度，不粘）⟹ 物体离开后 ext 塌回背景的小跨度
        //     ⟹ 斜坡自动变紧 ⟹ 鬼影快速移除；静止边缘 ext 大 ⟹ 斜坡宽 ⟹ 环路增益低 ⟹ 不振荡。
        // 定量预测（跑前写死，d = 1−1/16）：完全替换的条件从臂 8 的 `dist > 2·mem` 变成
        //   `dist > mem + ext` ⟹ 鬼影移除延迟 ≈3.5 帧（臂 7 ≈2.9、臂 8 ≈13.6）⟹ 应接近臂 7。
        // 判据：鬼影 R ≤ 1.0（boost 开**且**交付档）**同时** ρ ≤ 0.09 **且** 折叠超 τ = 0 px，
        //   三条缺一不算；不算 ⟹ 单标量接受域结案，转 velocity / 深度遮挡探测。
        const float memPrev =
            saturate(historyTex.SampleLevel(pointSampler, prevUvBase, 0.0).a);
        const float decay = clipCtl.y > 0.0 ? clipCtl.y : (1.0 - 1.0 / 16.0);
        const float3 ext3 = mx - mn;
        const float ext = max(ext3.x, max(ext3.y, ext3.z));
        const float mem = max(ext, memPrev * decay);
        const float histL = dot(histR, float3(0.3333333));
        const float centL = dot(centroid, float3(0.3333333));
        const float dist = abs(histL - centL);
        // 分母取 ext（**不含自由常数**，k = 1）：ext → 0 的平坦像素上 dist 也 → 0，
        // 而「平坦地面被物体让开」正是 dist 大而 ext 小 ⟹ w = 1 = 立刻清掉陈旧历史（正确方向）。
        const float w = saturate((dist - mem) / max(ext, 1e-6));
        clippedR = lerp(histR, hardR, w);
        alpha = mem;  // 与臂 6/7/8/9 共用载体 ⟹ imgqa 侧对这五臂跳过拒收码表
    }
    else if (clipMode == 10 && diag < 0.5)
    {
        // 10 = **臂 8 的接受域 + 把 α 交还给逐像素证据**（批 13.11 续；债-62 的第三条路）。
        // 前九臂全在「一个标量接受域」里找平衡，而债-62 的三角证明那是死路。本臂的观察是：
        // **接受域之所以要同时打两份工，是因为 α 是全画面一个数。**
        //   `params.x` 由 CPU 按**相机**静止帧数算（`taaBoostedFeedback`），boost 开 = 0.01
        //   ⟹ 「100 帧长窗」是发给**每个像素**的，而"这个像素的历史还可不可信"的证据（w）
        //   就在同一行 `lerp(clipped, cur, α)` 里，却没被用来决定 α。
        // ⟹ 鬼影的真实来源不是"臂 8 太粘"，而是**机位一动就按 boost 的长窗洗历史**：臂 8 在
        //   boost 关时 R = 1.347（**优于**今天 boost 关的 1.751），boost 开才炸到 3.028。
        // 本臂把两份工交回各自该管的量：
        //   · 接受域 = 臂 8 一字不改（闪烁性质原样：接受域内 w ≡ 0 ⟹ 静止内容零误报）
        //   · 历史遗忘速度 = α_eff = mix(α_boost, α_base, w)（裁剪说"历史可信"才给长窗）
        // **零新常数**：两个 α 都是 CPU 已经算好的量，w 是臂 8 已经算好的量。
        // **结构保证（可自检）**：boost 只会把 α 调小 ⟹ `α_base ≥ α_boost` ⟹ 取
        //   `base = max(clipCtl.z, params.x)` 后 `α_eff ≥ α_boost` **恒成立** ⟹ 历史只会比
        //   臂 8 **丢得更快** ⟹ 本改动在数学上不可能让鬼影比臂 8 更差。实测若反而更差 =
        //   仪器错，不是机制错。附带好处：clipCtl.z 万一没接上（= 0），max 兜回 params.x
        //   ⟹ 退化成臂 8，不会退化成"无限历史"。
        // **反向用例（机制自证，必须逐位）**：boost 关 ⟹ α_base == α_boost ⟹ mix 是恒等
        //   ⟹ 臂 10 与臂 8 逐位相同。不逐位 = 接线错，不解释。
        // 判据（跑前写死，三条 AND，缺一不算）：① ρ ≤ 0.09（五 bistro 机位 + 合成靶全部）
        //   ② 折叠超 τ = 0 px（同上全部）③ 鬼影 R（boost 开，spin 靶）≤ **1.751** =
        //   今天 boost 关的鬼影（口径 = "不再拿鬼影付 boost 换来的安静"）。
        //   不过线 ⟹ 臂 10 否决、转 velocity / 深度遮挡探测，**不再在单标量接受域上花第三轮**。
        const float memPrev =
            saturate(historyTex.SampleLevel(pointSampler, prevUvBase, 0.0).a);
        const float decay = clipCtl.y > 0.0 ? clipCtl.y : (1.0 - 1.0 / 16.0);
        const float3 ext3 = mx - mn;
        const float ext = max(ext3.x, max(ext3.y, ext3.z));
        const float mem = max(ext, memPrev * decay);
        const float histL = dot(histR, float3(0.3333333));
        const float centL = dot(centroid, float3(0.3333333));
        const float dist = abs(histL - centL);
        const float r = 1.0;  // 同臂 8：过渡带宽度 = 记忆半径（唯一不含自由数值的取值）
        const float w = saturate((dist - mem) / (r * max(mem, 1e-6)));
        clippedR = lerp(histR, hardR, w);
        // ⚠ **必须写成 `a + (b−a)·t`，不能写 `lerp(a, b, t)`**：Slang 把 `lerp` 降成
        //   `a·(1−t) + b·t`，即使 a == b 也**不**逐位等于 a（两次乘法 + 一次加法各自舍入）。
        //   实测踩过（2026-10-06）：boost 关时臂 10 与臂 8 的截图**不**逐位相同，`cmp` 报差异，
        //   而这条正是「接线对不对」的 bit 级自证——写成减法形式后 `b == a` ⟹ `(b−a)` 恰为 `+0`
        //   ⟹ `0·w = 0` ⟹ `a + 0 = a` **精确成立**，反向用例才成立。
        const float base = max(clipCtl.z, params.x);
        feedback = params.x + (base - params.x) * w;
        alpha = mem;  // 载体同臂 6–9
    }
    else if (clipMode == 11 && diag < 0.5)
    {
        // 11 = **把"允许动用跨帧色彩记忆"接到重投影位移上**（批 13.11 续；债-62/债-63 的出口）。
        // 前十臂全部只读颜色距离，而债-62 的不可能性证明**自己的适用条件就写着**「响应 =
        // f(dist, mem, ext)、**不含跨帧运动**/空间信息」——它否的是"只用颜色"这一族，门一直开着。
        // 本项目没有 velocity buffer 是**因为不需要**：重投影是解析的（相机 prevViewProj + 场景
        // prevSpin），`prevUvBase` 早就算好了 ⟹ 现成的屏幕位移
        //     motion = ‖prevUvBase − baseUv‖ / texel      （单位 = 像素）
        // ⚠ 关键在于这个量在两种情形下不是"大小不同"而是 **0 与很大**：批 13.5 把历史搬进
        //   **基空间**后，静止机位下 prevUvBase ≡ baseUv（浮点 ~1e-7）⟹ motion **恰为 0**；
        //   而场景/机位真动时它是好几个像素。闪烁场景与鬼影场景在这个轴上**线性可分**。
        // 于是规则不再是"调一个折中点"，而是**给记忆加前提**：
        //     mem = max(ext, memPrev · decay · gate),  gate = saturate(2 − motion)
        // 两个断点都是**推的**，没有一个是扫出来的：
        //   · `motion ≤ 1` ⟹ **可证明静止**：Halton 抖动 ±0.5px，两帧抖动差上界恰为 1px。
        //     静止 ⟹ 记忆全开（gate=1）⟹ 与臂 8 **逐位同**（乘 1.0 精确）⟹ 闪烁侧全部读数应一 bit 不动。
        //   · `motion ≥ 2` ⟹ 可证明非静止 ⟹ 记忆清零 ⟹ mem 退回**本帧瞬时** ext ⟹ 硬裁剪
        //     ⟹ 鬼影侧拿臂 0 的行为（臂 0 不鬼正是因为它的盒没有记忆）。
        //   · 中间线性，**斜坡宽度 = 1 像素 = 采样格周期**（唯一自然单位；不是自由参数）。
        // 为什么这次不是调参：跷跷板的支点从"响应函数形状"换成了"一个在一种情形下恒为 0、
        // 在另一种情形下恒为大的物理量"。臂 10 的 α 耦合之所以救不了鬼影，是因为 `mem` 在场景
        // 转动时会被 `ext` 撑大 ⟹ `w` 塌成 0 ⟹ α_eff 退回 boost；**根子是记忆本身在动的时候
        // 还在给豁免**，本臂关掉的正是这个豁免。
        const float memPrev =
            saturate(historyTex.SampleLevel(pointSampler, prevUvBase, 0.0).a);
        const float decay = clipCtl.y > 0.0 ? clipCtl.y : (1.0 - 1.0 / 16.0);
        const float3 ext3 = mx - mn;
        const float ext = max(ext3.x, max(ext3.y, ext3.z));
        const float gate = saturate(2.0 - length((prevUvBase - baseUv) / texel));
        const float mem = max(ext, memPrev * decay * gate);
        const float histL = dot(histR, float3(0.3333333));
        const float centL = dot(centroid, float3(0.3333333));
        const float dist = abs(histL - centL);
        const float r = 1.0;  // 同臂 8
        const float w = saturate((dist - mem) / (r * max(mem, 1e-6)));
        clippedR = lerp(histR, hardR, w);
        alpha = mem;  // 载体同臂 6–10
    }
    return float4(lerp(fromReinhard(clippedR), cur, feedback), alpha);
}
