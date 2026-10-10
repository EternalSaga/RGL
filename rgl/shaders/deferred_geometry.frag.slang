import model;
import material;  // 统一 push constant（DrawArgs 超集）
import scene;     // 场景级 bindless（binding 3：对象矩阵表，worldPos/worldNormal）
import normalmap;  // Phase 8 方案 A：normal 贴图标准 TBN + DX flip-G 解码（binding 6/7 消费）

// Phase 7 D1：G-buffer 几何 pass fragment。复用 model.mesh.slang（同 ModelVertexOutput、
// 同绑定 binding0 顶点/1 baseColorTex/2 sampler/3 sceneData/6 normalTex/7 ormTex），
// **不做光照**——只把材质几何/外观属性写进 4 个 MRT，供 light pass 光照。材质数据与
// forward 严格同源（albedo = baseColor.rgb × baseColorTexture；法线 = 导数 TBN 后世界
// 法线 oct 编码（Phase 8 ③a normal 贴图，DX flip-G + Z 重建）；metal/rough = 材质 factor
// × ORM 贴图 B/G 通道（Phase 8 ③a））——故 D1 判据 = deferred 与 forward 单灯着色等价。
//
// Phase 8 批次 F：材质类别宏 → emissive.a 通道（light pass 按类分支复刻 forward 专属
// shader 视觉：1=植被 wrap+透射、2=光源本色；light.frag 头注）。本文件 = 标准 PBR 本体
//（class 0）；类别变体 = deferred_geometry_vegetation/light.frag.slang 先 #define 再
// #include 本文件（常量折叠进 SPV，实测包装器编译成立）。路由 = scene.lua
// material_styles（材质名→风格）+ deferred_styles（风格→本组变体规格名），零 heap 契约改动。
//
// ── 湿面调制（Phase 15 批 15.2 子步 B，main 内「湿面调制」段）──
// **为什么住 G-buffer（本设计的全部理由）**：三项调制（粗糙度空间变化 / 水膜法线扰动 /
// albedo 压暗）写进 metalrough/normal/albedo 三张 G-buffer，light.frag 与 ssr.frag 读
// G-buffer 自动同源 ⟹ 结构上不可能出现「反射了但没压暗」「SSR 采了未扰动的法线」这类
// 错配；若改在 light pass 做，SSR 就得自己再复制一遍同样的调制（两个真相源）。
// **deferred-only**：forward 不经过本文件 ⟹ forward 无湿面 = phase15 §0.1 的同一 D1
// 豁免（SSR 亦 deferred-only，两条路一起没有，不新增对等缺口）。
// **确定性**：噪声只吃世界 xz，**没有任何时间输入**——帧间静止是 `regress burst` 静止
// 基线与 TAA 红线的前提；动画水膜不是本 phase 的事。
// 已知边界：① 扰动后的法线同时进直接光 NdotL 与 AO（G-buffer 法线只有一套消费者）——
// 这是「微水膜纹理」的语义，可接受；② 跨场景「光滑 ≠ 湿」误伤（抛光桌面会被误压暗）
// 需材质白名单兜底 → docs/phase15_plan.md §2.4 修正块。
// 类别变体自动带上且无害：vegetation/light 的 roughness ≈ 1.0 在门控区间上方 ⟹ wet ≡ 0
// ⟹ k = 0 恒走旁路分支 = 数学与逐位双重 no-op。
#ifndef MATERIAL_CLASS
#define MATERIAL_CLASS 0.0
#endif

// MRT 布局（= deferred 图 pass.colors 顺序 = color index，格式见 render_graph deferred desc）：
//   SV_Target0 albedo      RGBA8_SRGB         (linear 值写入，硬件 sRGB 编码)
//   SV_Target1 normal      RG16_SNORM         (octahedral 编码，范围 [-1,1])
//   SV_Target2 worldPos    RGBA32_SFLOAT      (世界空间位置，.xyz；RGBA32 系 RADV 对 RGB32_SFLOAT
//                                              采样报 FORMAT_NOT_SUPPORTED 的规避——spec 上 RGB32 可采样，见 devlog 勘误)
//   SV_Target3 metalRough  RGBA8_UNORM        (.r=metallic .g=roughness，[0,1] 直存 unorm)
//   SV_Target4 emissive    RGBA16_SFLOAT      (Phase 8 剩余批次 B：HDR 自发光 = emissiveFactor ×
//                                              emissiveTexture，与 forward 相加项同源；16F 保 HDR 域)

// octahedral 编码（McTaggart **标准形 +signNotZero**——z<0 半区折进同象限菱形外区，
// 码面连续；与 light.frag octDecode 同式自反——两端必须一致。旧 −signNotZero 乘子
// 折到对角象限，码面撕裂且在赤道带 z≈0⁻ 处产生 180° 翻转，2026-09-15 勘误）
float2 octEncode(float3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return n.xy;
}

// ── 湿面调制辅助（15.2B）──
//
// 格点值噪声的角点哈希（Wang hash 惯用形态）。⚠ 与 ao.frag 的 hash12 **不是同一个函数、
// 别抄那个**：那是逐像素抖动相位（整数像素进、随机数出，天生不连续没关系），这里是
// value noise 的格点角值——要与 frac/lerp 双线性插值配套才连续，抄过来得到的是
// 格点不连续的白噪声。
float wetHash(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

// 2 oct 值噪声，输出 [0,1)。oct 数**不是配置旋钮**（旋钮只有 WetTuning 六枚）：1 oct
// 水洼太团状规则；第 3 oct 的高频被下游 smoothstep(0.45, 0.65) 的形状截断吃掉，只多花
// 采样。幅值 0.6/0.4 保输出在 [0,1)；第二 oct 点阵平移错开，避免与第一 oct 同相。
float wetFbm(float2 p)
{
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);  // smootherstep 插值权重：格点处 C1，无线性插值脊
    float a = lerp(lerp(wetHash(i), wetHash(i + float2(1.0, 0.0)), u.x),
                  lerp(wetHash(i + float2(0.0, 1.0)), wetHash(i + float2(1.0, 1.0)), u.x), u.y);
    float2 p2 = p * 2.0 + float2(17.3, 9.7);
    float2 i2 = floor(p2);
    float2 f2 = frac(p2);
    float2 u2 = f2 * f2 * (3.0 - 2.0 * f2);
    float b = lerp(lerp(wetHash(i2), wetHash(i2 + float2(1.0, 0.0)), u2.x),
                  lerp(wetHash(i2 + float2(0.0, 1.0)), wetHash(i2 + float2(1.0, 1.0)), u2.x), u2.y);
    return 0.6 * a + 0.4 * b;
}

// wetFbm 的中心差分。eps = 基频点阵的 1/8：小到不糊掉第二 oct 结构，大到 float 精度
// 不会把差分淹成 0。⚠ **没有除 2·eps**——返回的是「差」不是严格梯度：幅度天然被
// wetFbm 的值域 [0,1) 界住，ripple_amp 因此直接是水膜法线切向偏移的上界（法线单位），
// 调参语义可预测（严格梯度会再乘 1/(2·eps) = 4 倍，幅度含义随 eps 漂）。
float2 wetFbmGrad(float2 p)
{
    const float e = 0.125;
    return float2(wetFbm(p + float2(e, 0.0)) - wetFbm(p - float2(e, 0.0)),
                  wetFbm(p + float2(0.0, e)) - wetFbm(p - float2(0.0, e)));
}

[shader("fragment")]
void main(
    ModelVertexOutput input,
    bool frontFace : SV_IsFrontFace,
    out float4 outAlbedo     : SV_Target0,
    out float2 outNormal     : SV_Target1,
    out float4 outWorldPos   : SV_Target2,
    out float4 outMetalRough : SV_Target3,
    out float4 outEmissive   : SV_Target4)
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    const ObjectDraw od = readObjectDraw(drawArgs.objectIndex);  // Phase 7b+：材质进 heap
    float3 albedo = od.baseColor.rgb * texel.rgb;
    // Phase 8 C1：MASK cutout（同 forward model_pbr_ibl；cutoff = od.emissive.w，0 = 不测）
    if (od.alphaDiscard(od.baseColor.a * texel.a))
        discard;

    ObjectMatrix M = readObjectMatrix(drawArgs.objectIndex);
    float3 P = worldPos(M, input.positionOS);
    float3 geoN = worldNormal(M, normalize(input.normal));  // 几何法线（方案 C AA 用）
    float3 N = geoN;

    // Phase 8 ③a：ORM 贴图（G=roughness B=metallic，R=AO 采样不应用，白纹理回落 factor）
    // + normal 贴图（导数 TBN，DX flip-G + Z 重建）——公式与 forward model_pbr_ibl 严格
    // 同源（G-buffer 与 forward 等价性依赖此）
    float4 orm = ormTexture.Sample(baseColorSampler, input.uv);
    float3 tangentNormal = decodeNormalDx(normalTexture.Sample(baseColorSampler, input.uv));
    // Phase 8 方案 A：标准 TBN（CPU 烘切线 + GS + 手性，无导数）——掠射角白噪点根治
    N = applyNormalMapTBN(N, worldTangent(M, input.tangentOS), tangentNormal);
    // Phase 8 C1：doubleSided 背面法线翻向（同 forward；cull 开时恒真 = 无副作用）
    if (!frontFace)
        N = -N;
    // ObjectDraw.roughnessMetallic: x=roughness y=metallic（scene.slang readObjectDraw）
    float metallic = clamp(od.roughnessMetallic.y * orm.b, 0.0, 1.0);
    float roughness = clamp(od.roughnessMetallic.x * orm.g, 0.04, 1.0);
    // Phase 8 方案 C：geometric specular AA 写进 G-buffer（light pass 用过滤后 roughness，
    // 与 forward 逐式同源保 D1；几何法线导数在 geometry pass 求，干净信号）
    roughness = normalFiltering(roughness, geoN);

    // ── 湿面调制（Phase 15 批 15.2 子步 B；设计论证见文件头「湿面调制」小节）──
    // **身份** = 与 SSR 粗糙度门控同区间同源（readSsrGate）：湿面资产标定写在 roughness
    // 通道里（湿 0.122 / 干 ~0.74/1.0，phase15 §2.4），零材质白名单、零契约改动。
    // ⚠ wet 必须用**调制前**的 roughness（= 本会写进 G-buffer 的原值）；在覆写之后
    // 再算会自我反馈（调制结果喂回身份）。
    float4 gate = readSsrGate();
    float4 wp = readWetParams();    // (strength, dark, roughLo, roughHi)
    float4 ws = readWetSpatial();   // (rippleAmp, puddleFreqRadii, -, -)
    float wet = 1.0 - smoothstep(gate.x, gate.y, roughness);
    float k = wp.x * wet;
    if (k > 0.0) {   // 干面 / 门关（strength=0）恒跳过：数学与逐位双重精确 no-op
                     //（strength=0 消融档 = 逐位回旧红线 A，机器可验的恒等旁路）
        // 水洼掩码 = 低频值噪声(世界 xz) × 朝上程度。噪声频率 = puddleFreqRadii / sceneRadius
        //（尺度无关，锚点 = readSsrParams().x，与 SSR 同一颗 SceneBounds.radius 球）；
        // 1e-6 是空场景防零除守卫不是调定值（同 ssr.frag 的 maxDistance 守卫）。
        float freq = ws.y / max(readSsrParams().x, 1e-6);
        float2 q = P.xz * freq;
        float n = wetFbm(q);
        // 0.45/0.65（内部常数，不是旋钮）：0.45 以下是干石、0.65 以上满水洼，带宽 0.2 ≈
        // 噪声特征尺寸的 1/3，再窄出水洼硬边、再宽水洼轮廓糊掉。upness 0.3/0.7：水只聚
        // 在朝上面，43°–73° 过渡带盖住「近竖直的墙不聚水」的视觉边界。
        float upness = smoothstep(0.3, 0.7, N.y);
        float puddle = smoothstep(0.45, 0.65, n) * upness;
        // ① 粗糙度空间变化：水洼 → roughLo(0.05)、湿石 → roughHi(0.35)。资产湿面均匀
        //    0.122 给不了这个结构（§2.4）。SSR 门控读 G-buffer 自动跟随 ⟹ 反射向水洼集中。
        roughness = lerp(roughness, lerp(wp.w, wp.z, puddle), k);
        // ② 水膜法线扰动：同一 fbm 的梯度（与粗糙度场结构相关，**不是**第二副独立噪声），
        //    ∝ wet·(1−puddle)——水洼 = 静水镜面，波纹只给湿石部分。
        float2 g = wetFbmGrad(q);
        N = normalize(N + float3(g.x, 0.0, g.y) * (ws.x * k * (1.0 - puddle)));
        // ③ albedo 压暗（水填微观凹凸 → 漫反射压暗；结案 AGENTS「湿面只加高光不压暗」）。
        //    F0 不受影响：湿面是电介质（metallic=0），F0 = lerp(0.04, albedo, 0) = 0.04
        //    与 albedo 无关，压暗不会连带改镜面反射的底色。
        albedo = albedo * lerp(1.0, wp.y, k);
    }

    outAlbedo = float4(albedo, 1.0);
    outNormal = octEncode(N);
    outWorldPos = float4(P, 0.0);
    outMetalRough = float4(metallic, roughness, 0.0, 1.0);
    // Phase 8 剩余批次 B：emissive HDR 写第 5 G-buffer（binding 8 已烘焙进模型条目，此前空转）
    // Phase 8 批次 F：.a = 材质类别（MATERIAL_CLASS 宏，clear 值 0 = 标准 PBR；light.frag 消费）
    outEmissive = float4(od.emissive.rgb * emissiveTexture.Sample(baseColorSampler, input.uv).rgb,
        MATERIAL_CLASS);
}
