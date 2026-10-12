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
// **shadowFar 淡出强度 k**（5a-9）：atten = saturate(k·(1 − t²))，t = depth / shadowFar。
//   k=4 → t < 0.866 全影、最后 13% 距离平滑收尾；Filament 用 10（最后 5%），我们取更宽的
//   尾巴以覆盖量化远界的跳变。**当前 shadowFar ≡ 相机远平面**（切在几何之外），故默认机位
//   与街道机位实测差异 0 px —— 它是"将来把 shadowFar 调短换性能"（Phase 9 余量 3.1ms 的
//   主要 lever）时的必需件：那时远界落在画面中间，没有淡出就是一条硬边。管道阳性测试：
//   k 临时改 1 → 默认机位 5.998% 像素**只变亮 0 变暗**（淡出特征签名）。
static const float kShadowFarFadeK = 4.0;

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

import scene;  // readSceneLight / readSkyInvViewProj（头区 UBO binding 22）+ readShadowViewProj / readCascadeSplits / readShadowParams

#if defined(PCSS_SOFT) || defined(EVSSM_SOFT)
// 批 11.5：本 slangc 不自动提供 gl_FragCoord（实测 E30015；__system_value 亦移除），
// 经 glsl 兼容模块取（`public in vec4 gl_FragCoord : SV_Position`）。门关不 import =
// 原版编译单元 token 流零变化。
import glsl;
#endif

[[vk::binding(12, 0)]] Texture2D<float> shadowMap;  // eDepth aspect（cascade 0）
[[vk::binding(13, 0)]] SamplerState shadowSampler;  // NEAREST point sampler（非比较采样器，见头注）
#ifdef EVSSM_SOFT
// 批 11.6：矩图集（EvssmShadow 图外 compute 派生；全 mip 链 view、GENERAL；
// 逐 tile 手动 mip 链的对齐论证 = docs/evssm_plan.md §4.1）。15/16 = 两路皆空的高号段
// （14 已被 IZB 占，续排；门关不声明 = 原版资源接口零变化 = 红线构造性保证）。
[[vk::binding(15, 0)]] Texture2D momentsAtlas;
[[vk::binding(16, 0)]] SamplerState momentsSampler;  // LINEAR + CLAMP_TO_EDGE（mip 当高斯核）
#endif

#ifdef PCSS_SOFT
// ============================================================================
// 批 11.5：PCSS 接触硬化软阴影（blocker search + 变半径加权 PCF），#ifdef 门 =
// 门关编译不过这段 = 原版 4 抽头路径逐位不变（红线构造性保证，同 1e IZB 变体）。
//
// 物理：太阳视直径 θ（非零）→ 影边有半影，**全宽 = Δz·2tan(θ/2)**（Δz = 遮挡体→接收面
// 光深度差）→ 灯根（Δz≈0）锐、尖端软 = 接触硬化。CSM 固定 PCF 半径给不了（灯根也糊）。
// 换算零新数据：Δz_world = Δz_ndc / |lightVP.c2.z|（正交深度行 = 1/(f−n)）；1 图集纹素
// = texelSizes[cascade] 世界单位（fillShadowData 同源）。参数经 readShadowPcss()。
//
// ⚠️ 加权窗口设计教训（首版自查推翻的方案，勿回退）：**线性化窗口不能取 Δz**——自表面
// 纹素深度恰好落在 ref 间断点上（差 ≈ bias），Δz 量级的窗口会把「搜索半径内、阴影外」的
// 全亮地面所有自采样抽成 0.5 = 每个遮挡体周围一圈 50% 灰带。软化只来自**空间半径**
// （半影 = 泊松盘内「遮挡纹素占比」），深度窗口只取 ≈2×bias 做纹素边缘抗锯齿。
//
// 成本：全亮早退 = 12 采样（画面多数像素）；遮蔽区 = 12 搜索 + 12 滤波（旧 PCF = 4）。
// tile 边界：采样 uv clamp 到本级块内（邻块 = 别的 cascade 深度 = 垃圾数据），块边缘
// 圆核退化为 clamp 核（罕见，可接受）。确定性：固定泊松盘 + FragCoord 哈希旋转（无随机
// 状态 = 断言可复现，rgl-test-discipline）。
static const float2 kPoisson12[12] = {
    float2(-0.326, -0.406), float2(-0.840, -0.074), float2(-0.696, 0.457),
    float2(-0.203, 0.621),  float2(0.962, -0.195),  float2(0.473, -0.480),
    float2(0.519, 0.767),   float2(0.185, -0.893),  float2(0.507, 0.064),
    float2(0.896, 0.412),   float2(-0.322, -0.933), float2(-0.792, -0.598),
};

/** PCSS 主流程（uv = 图集 uv，refZ = 含 bias 比较深度，texelWorld = 本块世界纹素，
 *  bias = NDC 深度偏置（加权窗基准），atlas/lightVP/tile 见调用处）。返回 1 = 全亮。 */
float pcssCoverage(float2 uv, float refZ, float texelWorld, float bias,
    float4 atlas, ObjectMatrix lightVP, float2 tile)
{
    const float4 pcss = readShadowPcss();
    // 逐像素旋转（哈希角，屏幕稳定；同泊松旋转去采样纹理惯例）。
    // ⚠️ gl_FragCoord 经 import glsl（本 slangc 不自动提供，见文件头注）。
    const float2 fc = gl_FragCoord.xy;
    const float ang = 6.28318531 * fract(sin(dot(fc, float2(12.9898, 78.233))) * 43758.54531);
    const float2 cs = float2(cos(ang), sin(ang));
    const float halfUv = atlas.y / atlas.x;               // 单块边长（图集 uv 单位）
    const float2 lo = tile * halfUv + float2(0.5 / atlas.x);
    const float2 hi = lo + halfUv - float2(1.0 / atlas.x);
    const float invAtlas = 1.0 / atlas.x;

    // ① blocker 搜索（半径 = pcss.y 纹素）
    float blockerSum = 0.0;
    int blockerCnt = 0;
    for (int i = 0; i < 12; ++i) {
        const float2 p = kPoisson12[i] * (pcss.y * invAtlas);
        const float2 off = float2(p.x * cs.x - p.y * cs.y, p.x * cs.y + p.y * cs.x);
        const float d = shadowMap.Sample(shadowSampler, clamp(uv + off, lo, hi));
        if (d < refZ) { blockerSum += d; blockerCnt += 1; }
    }
    if (blockerCnt == 0)
        return 1.0;  // 全亮早退（与 4 抽头 PCF 全亮值同 → ⑦ 淡出阶段逐值等）

    // ② 半影半径（texel）：Δz_world·2tan(θ/2)/2 ÷ 世界纹素；下限 1 纹素 ≈ 旧 PCF 软化
    //    基底；上限 pcss.z 用 **soft clamp**（Filament EVSSM 同款 squash：超限部分指数
    //    衰减而非硬切——硬 clamp 会在超限区糊出一条等高模糊带，soft 版平滑收束）
    const float avgBlocker = blockerSum / float(blockerCnt);
    const float dzWorld = max((refZ - avgBlocker) / abs(lightVP.c2.z), 0.0);
    const float penumbraRaw = dzWorld * pcss.x * 0.5 / texelWorld;
    const float penumbraSquashed = penumbraRaw <= pcss.z
        ? penumbraRaw
        : pcss.z + 1.0 - exp(-(penumbraRaw - pcss.z));
    const float radiusUv = clamp(penumbraSquashed, 1.0, pcss.z * 1.5) * invAtlas;

    // ③ 变半径加权 PCF（窗口 ≈ 2×bias：自采样 d ≈ ref+bias → 恒全亮，见头注教训）
    const float w = max(2.0 * bias, 1e-5);
    float litSum = 0.0;
    for (int i = 0; i < 12; ++i) {
        const float2 p = kPoisson12[i] * radiusUv;
        const float2 off = float2(p.x * cs.x - p.y * cs.y, p.x * cs.y + p.y * cs.x);
        const float d = shadowMap.Sample(shadowSampler, clamp(uv + off, lo, hi));
        litSum += saturate((d - refZ) / w + 0.5);
    }
    return litSum * (1.0 / 12.0);
}
#endif

#ifdef EVSSM_SOFT
// ============================================================================
// 批 11.6：EVSSM 远场软阴影（指数矩 + O(1) mip 搜索 + soft clamp 半影 + 硬件 mip
// 高斯核 + quincunx 5 抽头）。#ifdef 门 = 门关编译不过这段 = 原版逐位不变（红线
// 构造性保证，同 11.5）。公式直抄 Filament surface_shadowing.fs 的
// ShadowSample_EVSSM（DIRECTIONAL 路）+ evaluateEVSM（ELVSM 双侧矩）。适配差异
// （docs/evssm_plan.md §4.1 C 方案）：
//   · 矩源 = 图外 compute 从 D32 图集派生（正交深度线性），采样 = 2D 图集 + 逐 tile
//     手动 mip 链（tile 边界各层级恒对齐 → box 过滤零互染），非 sampler2DArray；
//   · 解析方差（屏幕导数把纹素内斜率折进二阶矩）**不加**——矩在 compute 侧生成无
//     导数可用；抗 acne = 既有 normal offset + depth bias + LBR（v1 口径）；
//   · 半影上限 soft clamp 在**图集纹素域**（配置语义 pcss_max_penumbra 复用），
//     非 Filament 的无量纲 ratio 域——艺术角色相同、单位对美术友好；
//   · per-tap clamp inset 随 targetLod 放大（mip L 硬件 bilinear 读 ±2^(L−1) 纹素，
//     固定半纹素 inset 在粗 mip 会漏进邻块垃圾数据）。
// 成本：1 搜索 + 5 滤波 = 6 次矩采样（对照 PCF 4 次深度采样）；矩图集 linear 可过滤。

// c（EVSM 指数）不再是本文件字面量：经 scene data 常量块由 host kEvssmExponent
// （vk_resources 唯一权威源）下发，readShadowEvssmExponent() 读——生产/消费/fallback 同源。
static const float kEvssmLbr = 0.1;         // light-bleed reduction（Filament 默认同值）
static const float kEvssmEpsilon = 0.002;   // 动态方差地板系数（evaluateEVSM EPSILON_MULTIPLIER）

/** Chebyshev 上界 + 动态方差地板 + LBR（evaluateEVSM 内联件逐式转写）。
 *  ⚠️ 快路（depth ≤ anchor → 1.0）是**语义必需**不是性能优化（forward fallback
 *  实测坐实，2026-10-02）：无它则「receiver 严格在所有 caster 之前」与「receiver
 *  在 caster 之后」给出同向的 ≈0（d² 对负 d 同样增长）= 全亮区反而判全暗。
 *  ⚠️ 批 N6d：快路参照从 `moments.x`（**均值**）换成调用点传入的 `anchor`（**单纹素**）。
 *  均值是 `mean(exp(c·depth))` = 软最大深度，双线性一跨剪影就被远侧拉到 100× 量级，
 *  于是「receiver 在均值之前」几乎恒成立 = 沿整条剪影漏光（实测丢 40% 影）。单纹素锚点
 *  没有混合，「receiver 在本纹素最近表面之前」才是**又sound又tight**的判据。
 *  返回值语义 = **受光量**（1 = 全亮）。 */
float chebyshevUpperBound(float2 moments, float anchor, float depth, float minVariance, float lbr)
{
    if (depth <= anchor)
        return 1.0;  // 快路：receiver 在锚点深度之前 = 全亮（LBR 前返回）
    const float variance = max(moments.y - moments.x * moments.x, minVariance);
    const float d = depth - moments.x;
    const float pMax = variance / (variance + d * d);
    return saturate((pMax - lbr) / (1.0 - lbr));
}

/** ELVSM 双侧矩评估（矩 = (z, z², −1/z, 1/z²)，z = exp(c·(2d−1))；zReceiver = 光深度 [0,1]）。
 *  @param anchor 同一点的**单纹素**（未混合）矩，只服务两侧快路；两侧锚点条件在单纹素上
 *         退化成同一个式子（`nw ≤ anchor.z ⟺ pw ≤ anchor.x`），故传 anchor.x / anchor.z 即可。 */
float evaluateEvssm(float4 moments, float4 anchor, float zReceiver)
{
    const float depth = zReceiver * 2.0 - 1.0;
    const float c = readShadowEvssmExponent();
    const float pw = exp(c * depth);
    const float p = chebyshevUpperBound(moments.xy, anchor.x, pw, kEvssmEpsilon * (pw * pw), kEvssmLbr);
    const float nw = -1.0 / pw;
    const float n = chebyshevUpperBound(moments.zw, anchor.z, nw, kEvssmEpsilon * (nw * nw), kEvssmLbr);
    return min(p, n);
}

/** EVSSM 主流程（uv = 图集 uv，refZ = 含 bias 比较深度，texelWorld = 本块世界纹素，
 *  atlas/lightVP/tile 与 pcssCoverage 同源）。返回 1 = 全亮。 */
float evssmCoverage(float2 uv, float refZ, float texelWorld,
    float4 atlas, ObjectMatrix lightVP, float2 tile)
{
    const float4 evp = readShadowEvssm();  // (半坡 tan(θ/2), 搜索半径[纹素], 半影上限[纹素], 最大 mip)
    const float maxMip = evp.w;
    const float halfUv = atlas.y / atlas.x;
    const float invAtlas = 1.0 / atlas.x;
    const float coeff = abs(lightVP.c2.z);  // 正交深度行 = 1/(f−n)（PCSS 头注同款换算）

    // —— STEP 1：O(1) blocker 搜索（DIRECTIONAL：光锥随级联深度张开）——
    //   distanceToNearPlane = 归一化光深度 × (f−n) = 米制入深；搜索半径（世界）= 半坡 × 入深。
    const float distNear = refZ / coeff;
    float searchTexels = evp.x * distNear / texelWorld;
    searchTexels = min(searchTexels, evp.y);  // 上限护栏（Filament maxSearchRadius 同职）
    const float searchLod = clamp(log2(max(searchTexels, 1.0)), 0.0, maxMip);
    const float4 searchMoments = momentsAtlas.SampleLevel(momentsSampler, uv, searchLod);
    // 平均遮挡深度解码（Filament 逐式）：z̄ = exp(c(2zB−1)) ← moments.z = −1/z̄。
    // 空块（far 最大矩）解码 zBlocker≈1 → clamp 到 refZ → 比值为 0 → 全亮自然成立。
    const float negMoment = max(-searchMoments.z, 1e-8);
    const float zBlocker = clamp((log(negMoment) / -readShadowEvssmExponent() + 1.0) * 0.5, 0.0,
        refZ);

    // —— STEP 2：半影估计（Δz 世界米 × 半坡）+ soft clamp（纹素域，见头注）——
    const float dzWorld = max((refZ - zBlocker) / coeff, 0.0);
    const float penumbraTexels = dzWorld * evp.x / texelWorld;
    const float penumbraSquashed = penumbraTexels <= evp.z
        ? penumbraTexels
        : evp.z + 1.0 - exp(-(penumbraTexels - evp.z));

    // —— STEP 2.5：核宽下限（批 N6d G2）————————————————————————————————————————————
    // 物理半影在出厂太阳角 0.53° 下 **< 1 个图集纹素** ⟹ 抽点半径 r = 0.5 纹素 ⟹ 5 个抽点
    // 全落进同一个 2×2 双线性邻域（实测：单抽头变异与基线**逐位相同**、太阳角敏感度**恰好
    // 0.000%**）⟹ 核根本没张开，EVSSM 退化成「单纹素深度比较」= CSM 去掉 PCF。下限让核
    // 至少张到能比较多个纹素。
    // ⚠ 核宽是**覆盖 ↔ 不透明度**的单调权衡，实测（交付配置 / 逐位确定性，比 CSM 同臂；
    //   分母 = 修好 ⑥ 抽头单位之后的 CSM：覆盖率 6.88% / 本影深 4.990LSB / 带核比 0.281）：
    //   foot    0     4      8     16     32
    //   覆盖率比 .857  .903   .917  1.051  1.208
    //   黑度比  .882   .827   .774  .723   .621
    //   带核比  .725   1.309  1.851 3.478  7.574
    //   每影像素深度(EVSSM) 75 66 61 50 37
    // **全域唯一不随分母漂的读数 = 黑度比**：≤0.882 且随核宽单调下降 ⟹ **EVSSM 的本影永远比
    //   CSM 浅**，想要软度就得付这个价。覆盖率与带核比两列都是**比值**，它们的绝对位置由分母
    //   决定：覆盖率在 foot≥4 就已 ≥0.90、foot=16 达 1.051（**超过 CSM 本身**），"带核比 ≈ 1"
    //   的点落在 **foot ≈ 2.4**（0.725@0 ↔ 1.309@4 线性插值），不在 16。
    // 出厂仍取 16 = 让 `mode="evssm"` 这一档**演示它自己的卖点**（比 CSM 软 3.5 倍、覆盖不输），
    //   不是"贴上 CSM"——那个理由随 ⑥ 的单位错一起作废了。要"尽量像 CSM"就调低到 2~4。
    // 完整权衡与转正裁决 → docs/shadow_routes_plan.md §5.2。
    const float penumbraFloored = max(penumbraSquashed, readShadowEvssmFootprint());

    // —— STEP 3：半影滤波（5 抽头 quincunx，中心 0.5 + 四角 0.125；lod 偏置 −1 =
    //    Filament TAPS==5 式：靠空间抽头重建形状，绕过 mip 极端压缩）——
    const float idealLod = log2(max(penumbraFloored, 1.0)) - 1.0;
    const float targetLod = clamp(idealLod, 0.0, maxMip);
    const float lodDeficit = exp2(max(idealLod - targetLod, 0.0));

    // 旋转 + 锚点抖动（哈希角 = PCSS 分支同式，屏幕稳定、确定性可复现）。
    const float2 fc = gl_FragCoord.xy;  // import glsl（门见文件头注）
    const float ang = 6.28318531 * fract(sin(dot(fc, float2(12.9898, 78.233))) * 43758.54531);
    const float2 cs = float2(cos(ang), sin(ang));
    const float2 r = penumbraFloored * invAtlas * 0.5 * lodDeficit;
    const float2 rotX = float2(cs.x * r.x, cs.y * r.x);
    const float2 rotY = float2(-cs.y * r.y, cs.x * r.y);
    const float inset = 0.5 * exp2(targetLod) * invAtlas;
    const float2 lo = tile * halfUv + float2(inset);
    const float2 hi = lo + halfUv - float2(2.0 * inset);
    const float2 anchor = clamp(uv, lo, hi);

    const float4 c4 = momentsAtlas.SampleLevel(momentsSampler, anchor, targetLod);
    const float4 k1 = momentsAtlas.SampleLevel(momentsSampler, clamp(anchor + rotX + rotY, lo, hi), targetLod);
    const float4 k2 = momentsAtlas.SampleLevel(momentsSampler, clamp(anchor + rotX - rotY, lo, hi), targetLod);
    const float4 k3 = momentsAtlas.SampleLevel(momentsSampler, clamp(anchor - rotX + rotY, lo, hi), targetLod);
    const float4 k4 = momentsAtlas.SampleLevel(momentsSampler, clamp(anchor - rotX - rotY, lo, hi), targetLod);

    // —— STEP 5：**逐抽头评估再平均**（批 N6d 主修）————————————————————————————————
    // 「先把 5 个抽点的矩平均、再评估一次 Chebyshev」在跨剪影的 footprint 上必然退化：那种
    // footprint 的深度分布是**双峰**（遮挡体 + 远背景），双峰让 Var 爆量级、μ 被远侧抬走，
    // 上界 Var/(Var+d²) 直接趋 0 或趋 1（两个方向都错：漏光 / 痤疮，实测 5.89% 与 16.92%
    // 就是这两侧，归因 → docs/shadow_routes_plan.md §4.2）。
    // 逐抽头评估把双峰**拆成五个局部单峰**，每个抽点各自判「本抽点局部是否被遮」，再按权重
    // 平均受光量 —— 这正是 PCF 的结构，也是 EVSSM 本该有的形态（mip 链给的是核宽，不是把
    // 分布搅在一起的理由）。
    // ⚠ 快路参照**必须逐抽点各用自己的矩**，不能共用中心锚点：共用时中心纹素判「接收面在
    //   其之前」会让 5 个抽点全部返回全亮，核再宽也加不回影（实测 foot 扫到 8 只从 0.598
    //   涨到 0.620）。而 CSM 影域里 **37.9%** 的像素正是「中心锚点判在前、CSM 判影」——
    //   共用锚点等于把 PCF 靠邻域抽点找回来的那部分永久抹掉（集合对拍实测）。
    const float litSum = evaluateEvssm(c4, c4, refZ) + evaluateEvssm(k1, k1, refZ)
        + evaluateEvssm(k2, k2, refZ) + evaluateEvssm(k3, k3, refZ)
        + evaluateEvssm(k4, k4, refZ);
    return litSum / 5.0;
}
#endif

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
    //   splits = 每级远截止（线性视深）；末级写 1e9，永远不会越界到不存在的级。
    //
    //   **forward 从 invVP 的两行叉积取**（5a-8c 修）：skyboxInvViewProj（@64，列主序）=
    //   inverse(proj·view)。其第 0 列的前三分量 ∝ 相机右向量、第 1 列 ∝ 相机上向量
    //   （右/上向量在 invV 里就是这两列，透视只贡献正标量），故 forward = normalize(cross(up, right))
    //   ——对任意 pitch/yaw 精确、与 fov/aspect/n/f 无关。
    //   **踩过的坑**（5a-8 首版，勿回退）：曾用 invVP **第 2 列**的前三分量取反
    //   （旧字节形态 `-normalize(Load4(96).xyz)`，类型化后 = `-normalize(invVP.c2.xyz)`）
    //   当 forward，注释称"w=0 无平移项混入"——**实测 w = 7.76 ≠ 0**，方向与真 forward 点积
    //   −0.43（近乎垂直）。后果：视深算错 → 绝大多数片元被判成"未越过 split0" → 全落最小的
    //   c0（半径 0.29 的盒）→ 远处一律出块旁路全亮 → **远处整片没影子，且该旁路边界随相机
    //   推进在世界面上扫过 = 用户报的"远处影子边缘往前移动"**。探针 probe6b 逐候选实测：
    //   第 2 列 −0.43 / cross(col1,col0) 0.94 / cross(row1,row0) **1.000000**。
    const float3 eyePos = readSceneLight().eyePos;
    const ObjectMatrix invVP = readSkyInvViewProj();
    const float3 camRight = invVP.c0.xyz;   // invVP 第 0 列前三分量
    const float3 camUp = invVP.c1.xyz;      // invVP 第 1 列前三分量
    // ⚠️ 本提取式（cross(col1,col0)）的数学同一性由 tests/test_shadow_forward.cppm
    // 钉住（500 随机姿态 dot≥1−1e-5；错误形态 −col2 / 叉积倒序 / 偏移互换各验红）——
    // 改动本式必须同步该测试（12.3d-C）。
    const float3 forward = normalize(cross(camUp, camRight));
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

    // ④ 出块返回全亮：级联边界外 / 深度范围外不产生阴影（否则图集边缘拉出长条伪影）。
    //   与 ⑦ 淡出的关系：出块在**远处**与"淡出已收到 0"同值（1.0），两侧连续无硬边；
    //   近处出块（真出 bug 时）仍是硬亮斑——那是要修 bug，不是淡出能盖的。
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

    // ⑥⑦ 采样 + 淡出：原版 = 4 抽头 PCF；PCSS_SOFT 变体 = blocker 搜索 + 变半径加权
    //   PCF（批 11.5，pcssCoverage 头注）；EVSSM_SOFT 变体 = 指数矩 O(1) 搜索 + quincunx
    //   （批 11.6，evssmCoverage 头注；两门同开 = EVSSM 优先，11.5 冻结件让位）。
    //   **刻意保持各分支完整持有 ⑥+⑦**——
    //   曾把 ⑦ 提为共享汇合（litCoverage）即破坏原版 SPV 逐位（两次 dot 合一次 →
    //   OpConstantComposite 少一份 → 全文件 id 重排实测）。门关预处理后 token 流 =
    //   原版一字不差，才成立红线构造性保证。
#ifdef EVSSM_SOFT
    const float ref = ndc.z - params.x;
    const float litEvssm = evssmCoverage(uv, ref, texel, atlas,
        readShadowViewProj(cascade), tile);
    // ⑦ 淡出（与原版逐式同；EVSSM 分支复制件，见上注）
    const float shadowFarEvssm = params.w;
    if (shadowFarEvssm <= 0.0)
        return litEvssm;
    const float tEvssm = depth / shadowFarEvssm;
    const float attenEvssm = saturate(kShadowFarFadeK * (1.0 - tEvssm * tEvssm));
    return lerp(1.0, litEvssm, attenEvssm);
#elif defined(PCSS_SOFT)
    const float ref = ndc.z - params.x;
    const float litCoverage = pcssCoverage(uv, ref, texel, params.x, atlas,
        readShadowViewProj(cascade), tile);
    // ⑦ 淡出（与原版逐式同；PCSS 分支复制件，见上注）
    const float shadowFarPcss = params.w;
    if (shadowFarPcss <= 0.0)
        return litCoverage;
    const float tPcss = depth / shadowFarPcss;
    const float attenPcss = saturate(kShadowFarFadeK * (1.0 - tPcss * tPcss));
    return lerp(1.0, litCoverage, attenPcss);
#else
    // ⑥ 4 抽头 PCF（2×2，偏移半个纹素）：单抽头的阶梯状阴影边在 1024² 块上肉眼可见，
    // 4 抽头是"看不出锯齿"与"不多花 4 倍采样"的平衡点。偏移量用 texel 常量而非屏幕导数：
    // texelSizes 是 scene data 里的 uniform（每帧一样）而不是按像素变化的；用 ddx/ddy 会在
    // 分支内未定义（同 ao.frag 批 11.4 的教训）。
    // ⚠ **单位**：这里的偏移是 **uv**，而上面那个 `texel` 是**世界米/纹素**（= 2·r/块边长，
    //   ② 拿它乘法线做世界系偏移是对的）。半个纹素换算到 uv 只跟图集边长有关、与 r 无关：
    //   块是图集里像素对齐的一块 ⟹ 1 块纹素 = 1 图集纹素 = 1/atlas.x uv。
    //   曾写成 `texel * 0.5`（把世界米当 uv）⟹ 实际偏移 = 应有值的 **4r 倍**（r = 本块光空间
    //   半径，取米数）。bistro 交付档 r=0.57 m → 2.3 倍（PCF 核略宽，肉眼看不出，故长期潜伏）；
    //   r 上到几十米 → 抽头直接跑到遮挡体图像之外 → 采样到空图集（远深度）→ **整场景判成受光
    //   = 阴影消失**。PCSS/EVSSM 分支因 ① 有 `clamp(uv+off, lo, hi)` 夹回本块而免疫。
    //   护栏 = `just imgqa shadow-kernel-scale`（靶 = qa_scenes.py 的 penumbra_ladder，四块板
    //   只让间隙 h 变；r 是几十米量级，正是 bistro 尺度照不出这个错的那个盲点）。三条判据：
    //   CSM 必须有 umbra、CSM 半影宽度必须对 h 平、PCSS@5° 必须**不**平（阳性锚点）。
    const float halfTexelUv = 0.5 / atlas.x;
    const float2 o = float2(halfTexelUv, halfTexelUv);
    const float ref = ndc.z - params.x;
    const float d0 = shadowMap.Sample(shadowSampler, uv + float2(-o.x, -o.y));
    const float d1 = shadowMap.Sample(shadowSampler, uv + float2(o.x, -o.y));
    const float d2 = shadowMap.Sample(shadowSampler, uv + float2(-o.x, o.y));
    const float d3 = shadowMap.Sample(shadowSampler, uv + float2(o.x, o.y));
    // SamplerState 是普通采样器（非比较采样器），故手动比较：返回 1 = 受光
    const float4 lit = float4(ref <= d0 ? 1.0 : 0.0, ref <= d1 ? 1.0 : 0.0,
        ref <= d2 ? 1.0 : 0.0, ref <= d3 ? 1.0 : 0.0);
    // ⑦ shadowFar 淡出（5a-9）：末级切片只覆盖到 shadowFar（= 量化后的相机远平面），
    //   再远处走"出块=全亮"旁路 = **硬切齐刷刷断掉**（用户最早报的那类硬边）。这里按
    //   Filament 的做法（surface_light_directional.fs:66-71 + ColorPassDescriptorSet.cpp:315）
    //   在归一化视深上做二次淡出：atten = saturate(k·(1 − t²))，t = depth / shadowFar。
    //   k=4 → t < 0.866 全影（近中景零变化），最后 13% 距离平滑收到全亮；t ≥ 1 时
    //   atten = 0，与旁路返回值 1.0 **连续**（不留硬边）。
    //   params.w = shadowFar（世界单位，fillShadowData 填）；≤0 = 不淡出（防御）。
    //   为何放 params.w 而非新开 vec4：params 原就留了 w 槽（历史遗留），复用零 schema 改动。
    const float shadowFar = params.w;
    if (shadowFar <= 0.0)
        return dot(lit, 0.25);
    const float t = depth / shadowFar;
    const float atten = saturate(kShadowFarFadeK * (1.0 - t * t));
    return lerp(1.0, dot(lit, 0.25), atten);  // Slang 无 mix 内建（同文件族用 lerp）
#endif
}
