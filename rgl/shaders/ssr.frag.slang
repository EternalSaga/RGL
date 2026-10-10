// ssr.frag —— 屏幕空间反射（Phase 15 批 15.1 后半）：镜面 DDA ray march。
//
// ── 形态（为什么是 post-lighting 全屏 pass）──
// 输入 = `light` pass 写好的 `scene_hdr`（本帧成片）+ G-buffer 现成量，输出 = `ssr_applied`
// （成片 + 反射贡献）。图内不可能读同帧 SSR 结果（`light` 正在写 `scene_hdr`），所以形态
// 唯一（论证 → `docs/phase15_plan.md` §2.1）。**deferred-only**：权重全从 G-buffer 算，
// forward 图没有 G-buffer（D1 对等豁免 → §0.1，装饰器跳过条件保证本 shader 只在 deferred 跑）。
//
// ── 算法 ──
// McGuire & Mara, Efficient GPU Screen-Space Ray Tracing (JCGT 2014) 的 2.5D DDA：把反射
// 射线投进屏幕网格逐格走，用深度缓冲判命中。按 §1.3 的裁决**照论文自己实现**，不搬 Filament
// 的移植体（其 BSD-2 声明因此不需要随文件保留）。
//
// 与论文/常见移植的**两处刻意偏离**，都是本项目判据逼出来的，改回去之前先读 §1.3 与 §3.1：
//  ① **投影精确的 λ 参数化**，不做「屏幕坐标线性插值」近似。常见移植（含论文参考实现）把
//     射线两端投到屏幕后**线性**插值 z；而 ndc.z 沿世界直线不是线性的，中点误差量级 ≈ 视深
//     的调和/算术平均之差（本场景 5% 量级 ⟹ 反射位置系统性偏移十几像素）。批 15.0 的闭式
//     反射靶预测的是**精确**几何倒影，线性近似会让判据量到"实现有偏"的假信号。这里改走
//     投影式：clip(t) = C0 + t·Cd 对 t 线性，屏幕坐标是它的有理函数，格边界穿越解
//         t = (A·C0.w − C0.x) / (Cd.x − A·Cd.w)      （A = 目标 ndc 值）
//     一步求出，**精确**。代价 = 每格一次除法。（x,y 本身沿世界直线是线性的——透视把直线
//     映成直线——所以"走哪些格"两种做法相同，差的只是"格处的深度"。）
//  ② **命中判据用世界空间沿射线的有符号距离**，不比 ndc 深度。定义
//         along(t) = dot(Q(s(t)) − P, R) − t
//     其中 Q 是该屏幕像素的深度对应的世界点。along > 0 ⟺ 几何还在射线**前方**（未命中），
//     along < 0 ⟺ 射线已穿到几何**后方**。⟹ `thickness` / `bias` / `maxDistance` 三个参数
//     **直接是世界单位**，这正是 §1.3 那条红线（"世界单位参数一个都不许抄 Filament"）要求
//     的口径——若比 ndc 深度，这三个参数就得随投影参数标定，抄过来的数值含义不明。
//
// ── 纹理空间口径 ──
// 本 pass **不改变纹理空间**：输入输出同空间同网格，ray march 在**本帧抖动空间**求交
// （深度缓冲与 `readSsrViewProj()` 同为抖动后口径，见 scene_data.cppm kSceneDataSsrOffset）。
// 不做 un-jitter——那是下游时域滤波器的活（同 ao_apply 纪律）。
//
// ── 未命中的回退 = 什么都不加（**已知边界，不是遗漏**）──
// 未命中时反射贡献为 0，画面退回 `light.frag` 已经加过的 IBL specular。本批**不做**"命中处
// 把 IBL specular 减掉"的能量守恒（那要把 env 图 + BRDF LUT 也绑进来重算一遍）。后果 =
// 光滑面上 IBL specular 与 SSR **双计**，镜面偏亮。§1.3 说"回退质量比命中质量更决定观感"，
// 这条就是回退面的欠账 ⟹ 记在 `docs/phase15_plan.md` §3.2，15.2（湿面 = 最光滑的那批表面）
// 之前必须还。
//
// ── 绑定面（与 CPU 侧 addSsrEntry 成对；G-buffer 通道号镜像 light.frag）──
//   3 = sceneData（import scene；@722272 ssrViewProj = 本帧**抖动后** proj·view）
//   4 = scene_hdr（RGBA16F 线性 HDR，反射源）
//   5 = gbuffer_normal（RG16_SNORM oct，解法走 scene.slang `octDecode` 单一来源）
//   6 = gbuffer_position（RGBA32_SFLOAT 世界坐标）
//   7 = gbuffer_metalrough（RGBA8_UNORM .r=metal .g=rough）
//   8 = depth（eD32Sfloat 深度图，eDepth aspect；`Load` 直取，不占 sampler）
//   9 = gbuffer_albedo（RGBA8_SRGB，Schlick fresnel 的 F0 要它——金属地面的 F0 就是 albedo，
//       省掉它等于把所有表面当介质，靶的金属地面反射会整个不出来）
//  10 = sampler（共享 LINEAR/ClampToEdge；**只有**命中取色用它，见下条纪律）
//
// ⚠ **取色器分工是判据的一部分**：直通路径与逐像素自身量一律 `Load`（整数纹素原值），只有
//   命中点取反射色用 `Sample`（命中 uv 是亚像素的，必须过滤）。直通用 `Load` 保证
//   **SSR 未作用的像素与门关时逐位相同** ⟹ `imgqa switch` 的影响域判据（"门关影响域恰为 0"）
//   才有意义；否则影响域里混进"采样路径的浮点差异"，判据就分不清"SSR 没作用"与"SSR 作用了
//   但被浮点噪声淹没"。

import scene;  // sceneData + readSsrViewProj + octDecode + worldFromDepth + uvFromClip + readSceneLight

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;    // 本帧成片（RGBA16F 线性 HDR）
[[vk::binding(5, 0)]] Texture2D<float2> normalTex;      // G-buffer oct 法线（rg16_snorm）
[[vk::binding(6, 0)]] Texture2D<float4> positionTex;    // G-buffer 世界坐标（rgba32_sfloat）
[[vk::binding(7, 0)]] Texture2D<float4> metalRoughTex;  // G-buffer .r=metal .g=rough
[[vk::binding(8, 0)]] Texture2D<float> depthTex;        // eD32Sfloat 深度图（eDepth aspect）
[[vk::binding(9, 0)]] Texture2D<float4> albedoTex;      // G-buffer albedo（rgba8_srgb，F0 用）
[[vk::binding(10, 0)]] SamplerState hitSampler;         // 共享 LINEAR/ClampToEdge

struct SsrInput
{
    float4 position : SV_Position;  // framebuffer 坐标（Load 用）
    float2 uv       : TEXCOORD0;    // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

// ── 参数（**世界单位**；一个都不许抄 Filament，§1.3 红线：它是米制单模型标定，本场景
//    scale=0.01、包围球 r≈1.17 ⟹ 下面每个数都按本场景尺度定，扫描表见 §3.2）──
// 射线长度上限。场景跨度 ≈2.3，取 0.55 ≈ 跨度的四分之一：再长就大量反射到"屏幕里根本没有的
// 内容"（§1.3 心理阴影 ③：大场景未命中率高，宁可少反射也不要错反射）。
static const float kMaxDistance = 8.0;
// 命中窗口（世界单位）。**双重职责**：① 真命中那格的 along 最深约 −Δt（Δt ≈ tMax/kMaxSteps），
// 窗口必须 > Δt 才不会把真命中误杀；② 又必须远小于"射线从物体后方经过"时的穿透深度，否则治不了
// sticking（职责②的实测数据见主循环那条 ⚠）。当前 0.04 ≈ 5×Δt。
static const float kThickness = 0.04;
// 自交偏置：射线起点就在 G-buffer 表面上（along(0)=0），数值噪声会立刻给出 along<0 的假命中。
// 必须 < kThickness（否则窗口 [−thickness, −bias] 为空 = 永不命中）。
static const float kBias = 0.004;
// DDA 格边长（像素）。2 = 步数减半；靶的倒影边界是硬边，格粗于 2 会把边界量化出来。
static const float kCellPx = 2.0;
// 步数上限。**刻意不抄 Filament 的 max(res)**（§1.3 红线：那是移动端小分辨率的宽松上界）。
// 覆盖屏幕行程 ≈ kMaxSteps·kCellPx = 144 px；靶的倒影需要 ~240 px 行程里的大部分（射线只需
// 走到"被反射内容的像"，不必走完整个倒影区）。扫描表见 §3.2。
static const int kMaxSteps = 512;
// 命中点二分细化次数（在 [上一格穿越 t, 本格穿越 t] 上解 along=0）。
static const int kRefineIters = 4;
// 粗糙度门控：SSR 只对镜面成立，粗糙面反射的是"一片"而不是"一个方向"，屏幕空间采样给不出。
// 全反射上限 0.15 / 完全关闭 0.4（中间 smoothstep 过渡，硬切会在材质边界出现可见的开关线）。
static const float kRoughFull = 0.15;
static const float kRoughCutoff = 0.40;
// 命中点离屏幕边界多近开始淡出（像素）。边界外的内容根本没画，越靠边反射越不可信。
static const float kEdgeFadePx = 24.0;
// 天空判据：深度 = 远平面清值（skybox 大三角 z=1 靠 depth clear + LEQUAL，不写深度）。
// 阈值与 ao.frag:106 同值同因——两处必须一起改，否则 AO 当天空而 SSR 当几何（或反之）。
static const float kFarDepth = 0.9999;

// 沿射线求交的一个采样结果。
struct MarchSample
{
    float along;  // 世界空间有符号距离：>0 几何还在射线前方，<0 射线已到几何后方
    bool  usable; // false = 该处无法求交（出屏 / 落到相机平面之后）⟹ 调用方必须停走
};

// 屏幕某处的世界点相对射线的位置（判据本体，见文件头 ②）。
MarchSample sampleAlong(float4 c0, float4 cd, float t, float3 P, float3 R, float2 px)
{
    MarchSample s;
    const float4 c = c0 + cd * t;
    if (c.w <= 1e-5)  // 落到相机平面之后：投影翻转，结果无意义
    {
        s.along = 0.0;
        s.usable = false;
        return s;
    }
    const float2 uv = uvFromClip(c);
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        s.along = 0.0;
        s.usable = false;  // 出屏 = 屏幕里没有可反射的内容 = 未命中
        return s;
    }
    const int2 tp = clamp(int2(uv * px), int2(0, 0), int2(px) - 1);
    const float d = depthTex.Load(int3(tp, 0));
    if (d >= kFarDepth)
    {
        s.along = 1e30;  // 天空 = 无遮挡，射线继续往前
        s.usable = true;
        return s;
    }
    s.along = dot(worldFromDepth(uv, d) - P, R) - t;
    s.usable = true;
    return s;
}

// uv ↔ ndc 换算**必须走这两个函数**，不许在调用点手写 `2*uv-1`。
// ⚠ 两轴**不同式**：uv.x = ndc.x/2 + 1/2（同向），uv.y = (1 − ndc.y)/2（**反向**，framebuffer
//   uv 原点左上、NDC 原点左下，口径见 scene.slang「屏幕 ↔ 世界重建」段头）。本批第一版两轴都
//   写 `2*uv-1` ⟹ y 轴求的是**关于地平线镜像**的那条边界：march 只在横向走、永远不上行到被
//   反射物，靶上表现为"倒影像素首轮就收口"（2026-10-10 实测，症状与"射线朝消隐点收敛"几乎
//   一样，靠把 R 与可达性分别量出来才分开）。
float ndcXFromUv(float u) { return 2.0 * u - 1.0; }
float ndcYFromUv(float v) { return 1.0 - 2.0 * v; }

// 射线**下一次**走到「ndc 分量 == A」时的 t（投影精确，见文件头 ①）。不可达一律返回极大值，
// 让调用方的 min 自然取另一边 / 取到 tMax。
//
// 「不可达」有**两种**，第二种最容易漏：
//   · 分母趋零 = 射线与该轴平行；
//   · **解不在 tAfter 之后**。屏幕坐标是 t 的有理函数 (c + t·d)/(w0 + t·dw)，当该坐标朝某个
//     值**渐近收敛**（几何上 = 射线朝消隐点收敛，本靶的反射射线 x 分量≈0 正是这个形态）时，
//     那条边界永远到不了，可代数式照样算出一个解——方程的另一支，落在**过去**。
// 两种都必须当"不可达"，否则调用方 `min(tX, tY)` 会拿到一个身后的 t。
float nextCrossT(float4 c0, float4 cd, float A, int axis, float tAfter)
{
    const float c = axis == 0 ? c0.x : c0.y;
    const float d = axis == 0 ? cd.x : cd.y;
    const float den = d - A * cd.w;
    if (abs(den) < 1e-9)
        return 1e30;
    // ⚠ **非正解必须当"不可达"**（本函数最容易被漏掉的一半）。屏幕坐标是 t 的**有理**函数
    //   (c + t·d)/(w0 + t·dw)：当 |d/w0| 不足以穿过边界、而 w 又在单调增长时，该坐标是**渐近
    //   收敛**到某个值（几何上 = 射线朝消隐点收敛），那条边界根本到不了，可上面这个代数式照样
    //   算出一个**负 t**（方程的另一支）。调用方拿 `min(tX, tY)` 就会拿到一个身后的 t——2026-10-10 实测：靶上 48% 的倒影像素死在这一条（反射射线几乎垂直上行、x 分量
    //   ≈0，正是收敛形态）。真正的穿越永远在 t > 0。
    const float tcand = (A * c0.w - c) / den;
    // ⚠ 判据是「在 tAfter **之后**」而不是「> 0」。用 > 0 会把**已经越过**的边界当成有效穿越
    //   返回（值落在 (0, tCur]），调用方于是把 tCur **往回**设——实测形态：射线走了 200 多步
    //   而进度恒为 0（2026-10-10 本批踩过，且当时给本函数加了 tAfter 形参却忘了用它，
    //   slang 对未使用形参不出声，靠读数才发现）。
    return tcand > tAfter + max(1e-6, tAfter * 1e-4) ? tcand : 1e30;
}

[shader("fragment")]
float4 main(SsrInput input) : SV_Target
{
    // 直通基准：后面每条早退路径都返回它（`Load` 整数纹素原值），保证"SSR 未作用"的像素
    // 与门关**逐位**相同（判据根据见绑定面那条 ⚠）。
    const int2 pix = int2(input.position.xy);
    const float4 hdr = sceneHdrTex.Load(int3(pix, 0));

    uint pxW, pxH, mLvl;
    depthTex.GetDimensions(0u, pxW, pxH, mLvl);
    const float2 px = float2(float(pxW), float(pxH));

    // 天空不反射（它已经是"整个环境"，再叠一层 SSR 是双重计数且会糊掉天顶渐变）。
    if (depthTex.Load(int3(pix, 0)) >= kFarDepth)
        return hdr;

    const float4 mr = metalRoughTex.Load(int3(pix, 0));
    const float roughness = mr.g;
    // 粗糙度门控早退（**性能与正确性双重**：粗糙面本就不该有镜面反射，且这条早退让绝大多数
    // 像素根本不进 ray march —— 门控关掉的是整段 march 的成本，不是一个乘数）。
    const float roughGate = 1.0 - smoothstep(kRoughFull, kRoughCutoff, roughness);
    if (roughGate <= 0.0)
        return hdr;

    const float3 P = positionTex.Load(int3(pix, 0)).xyz;
    const float3 N = octDecode(normalTex.Load(int3(pix, 0)).xy);
    const float3 eyePos = readSceneLight().eyePos;
    const float3 V = normalize(eyePos - P);      // 表面 → 相机（与 light.frag 同向口径）
    const float3 R = reflect(-V, N);             // 反射射线方向（|R| = 1 ⟹ t 就是世界距离）
    if (dot(R, N) <= 0.0)
        return hdr;                              // 退化（数值上不该发生）：宁可不反射

    // 投进屏幕：clip(t) = C0 + t·Cd，对 t 线性（文件头 ①）。
    const ObjectMatrix vp = readSsrViewProj();
    const float4 C0 = mulM(vp, float4(P, 1.0));
    const float4 Cd = mulM(vp, float4(R, 0.0));

    // 射线朝相机时会在 t = −C0.w/Cd.w 处撞上 w=0 奇异平面。那里投影翻转、屏幕坐标乱跳，
    // 必须硬截断；同时把"还剩多少可用行程"当淡出——这是**尺度无关**的朝相机淡出，刻意不用
    // Filament 的 `1 − max(0, vsDir.z)`（那要假设 proj 的 w 行是单位向量，本引擎的投影惯例
    // 不该被 shader 这样依赖）。
    float tMax = kMaxDistance;
    float singularityFade = 1.0;
    if (Cd.w < 0.0)
    {
        const float tSing = -C0.w / Cd.w;
        if (tSing <= kBias)
            return hdr;
        tMax = min(tMax, tSing);
        singularityFade = saturate(tSing / kMaxDistance);
    }

    // ── DDA：沿屏幕网格走，逐格判 along 的符号翻转 ──
    const float2 cellPx = float2(kCellPx, kCellPx);
    const float2 cellSize = cellPx / px;                       // 一格的 uv 跨度
    int2 cell = int2(floor(uvFromClip(C0) / cellSize));
    // 行走方向：格按 **uv** 走，所以符号必须取 uv 的变化率，不是 ndc 的。
    // ⚠ **y 必须翻向**：本引擎 `ndc.y = 1 − uv.y·2`（framebuffer uv 原点左上、NDC 原点左下，
    //   口径见 scene.slang「屏幕 ↔ 世界重建」段头），两者单调**相反** ⟹ 拿 ndc.y 的变化率符号
    //   去走 uv 网格 = 永远朝反方向走，解出来的是**身后**那条边界
    //   立刻退出（2026-10-10 实测：靶的反射行程几乎全是垂直的，51% 命中率、15% 直接退化退出，
    //   全由这一条造成）。x 不翻（uv.x = ndc.x/2 + 1/2 同向）。
    const float dX = Cd.x * C0.w - C0.x * Cd.w;   // ∝ d(ndc.x)/dt，分母 C0.w² > 0 不影响符号
    const float dY = Cd.y * C0.w - C0.y * Cd.w;   // ∝ d(ndc.y)/dt
    const int stepX = dX >= 0.0 ? 1 : -1;
    const int stepY = dY >= 0.0 ? -1 : 1;         // ← 翻向

    float tCur = 0.0;
    float tHit = 0.0;
    bool hit = false;
    // 穿越判据的状态 = 「曾经采到 along > 0」+「那次正值的参数」（二分的下界）。
    // ⚠ **不要用「上一个采样 along > 0」当状态**（本批最贵的一条，实测形态 = 靶上 25.8% 的倒影
    //   像素整条射线走到预算耗尽、且**漏与不漏按行交替**）。`along < -kBias` 留了条死区
    //   (−kBias, 0]：采样一旦落进死区，"上一个为正"就**永久失效**，之后无论穿多深都再也判不出
    //   命中。本靶恰好把这条放大到必然发生——平坦镜面的 `along` 零点与**被反射物轮廓的入口**
    //   重合（射线到达柱面的那一刻才进柱子轮廓），所以"零点附近那一次采样"必然出现。
    //   而像素中心在格内的位置只有 0.25 / 0.75 两种（uv 像素中心 = (i+0.5)/res、格宽 = 2px），
    //   第一步长度随之不同 ⟹ 采样相位随行/列奇偶交替 ⟹ 死区命中与否也交替 = 横条纹。
    bool sawFront = false;
    float tFront = 0.0;

    // ⚠ **`tEnd <= tCur` 不是终止条件**（这条本批踩了三轮才出来，实测形态 = 靶上 42.6% 的倒影
    //   像素"第一步就退出"）。像素中心通常落在格边界附近（cell=2px 时距边界 0.25 格），第一条
    //   边界在 t≈0⁺ 就被到达；一旦把"这一步没前进"读成"射线走完了"，整条射线就在起点附近夭折。
    //   DDA 的正确做法：**零长度步不采样但继续走**，只有预算（tMax / 步数）才是终止条件。
    //   配套的两条：① `nextCrossT` 只认严格在 tCur **之后**的解（身后的解 = 不可达）；
    //   ② 推进时**两条轴各自判到达**（同时到达就都推），而不是"推较小的那条"——后者在一步跨过
    //   两格时会让格下标落后于实际位置，于是"下一条边界"变成已经越过的那条。
    for (int stepIdx = 0; stepIdx < kMaxSteps; ++stepIdx)
    {
        const float nextU = (stepX > 0 ? float(cell.x + 1) : float(cell.x)) * cellSize.x;
        const float nextV = (stepY > 0 ? float(cell.y + 1) : float(cell.y)) * cellSize.y;
        const float tX = nextCrossT(C0, Cd, ndcXFromUv(nextU), 0, tCur);
        const float tY = nextCrossT(C0, Cd, ndcYFromUv(nextV), 1, tCur);

        const float tEnd = min(min(tX, tY), tMax);
        const bool budgetDone = tEnd >= tMax;   // 两条轴都不可达时 tEnd == tMax：最后采一次再收口
        const float tStop = budgetDone ? tMax : tEnd;

        // 零长度步（tStop == tCur）不采样：它不贡献任何覆盖，采样只会把同一点当新格重复判。
        if (tStop > tCur && tStop > kBias)
        {
            const MarchSample s = sampleAlong(C0, Cd, 0.5 * (tCur + tStop), P, R, px);
            if (!s.usable)
                break;                       // 出屏 / 奇异平面之后：再走也没有意义
            if (s.along > 0.0)
            {
                sawFront = true;
                tFront = 0.5 * (tCur + tStop);
            }
            else if (sawFront && s.along < -kBias)
            {
                // ⚠ **防 "sticking" 假命中**（本批实测：删掉这条 ⟹ 靶上柱子**两侧**的地面多出
                //   34170 px 的反射，幅度 p50=0.022 与真倒影同量级）。那些射线是在柱子**后方几米**
                //   经过的，只是屏幕路径压过了柱子的轮廓；`along` 的表现为**一步从 +很大跳到 −很大**
                //   （Q 从远处地面跳到近处柱面），而真命中的 `along` 是**平滑降到 0** 的，一步最多
                //   跨 Δt ≈ tMax/kMaxSteps。所以"这一步穿进几何的深度"就是两者的判别量。
                //   ⟹ 判据必须作用在**细化之前**的采样上：二分只会把任何跳变都收敛到 along≈0，
                //   细化之后再判就永远看不出区别了。
                if (-s.along > kThickness)
                    break;
                // 二分细化到 along = 0，把命中点从"格中点"提到亚像素。下界取**最后一次
                // along > 0 的参数**（不是 tCur：tCur 处的采样可能正落在上面那条死区里，
                // 拿它当正侧端点会让二分从头到尾没有异号区间、收敛到错误的一侧）。
                float a = tFront, b = 0.5 * (tCur + tStop);
                for (int i = 0; i < kRefineIters; ++i)
                {
                    const float m = 0.5 * (a + b);
                    const MarchSample r = sampleAlong(C0, Cd, m, P, R, px);
                    if (!r.usable)
                        break;
                    if (r.along > 0.0)
                        a = m;
                    else
                        b = m;
                }
                tHit = 0.5 * (a + b);
                hit = true;
                break;
            }
        }
        if (budgetDone)
            break;

        if (tX <= tEnd)
            cell.x += stepX;
        if (tY <= tEnd)
            cell.y += stepY;
        tCur = tStop;
    }
    if (!hit)
        return hdr;

    const float2 hitUv = uvFromClip(C0 + Cd * tHit);
    if (hitUv.x < 0.0 || hitUv.x > 1.0 || hitUv.y < 0.0 || hitUv.y > 1.0)
        return hdr;

    // ── 权重（三条 fade 的语义照 §1.3 的裁决；数值按本场景重标定）──
    // ① 屏幕边界淡出：边界外的内容没画，越靠边越不可信。
    const float2 edgePx = min(hitUv * px, px - hitUv * px);
    const float edgeFade = saturate(min(edgePx.x, edgePx.y) / kEdgeFadePx);
    // ② 距离淡出：硬截在 maxDistance 会看到一条"反射突然消失"的等高线。
    const float u = saturate(tHit / kMaxDistance);
    const float distanceFade = 1.0 - smoothstep(0.6, 1.0, u);
    // ③ 朝相机淡出（上面算的，尺度无关）。
    const float weight = roughGate * edgeFade * distanceFade * singularityFade;
    if (weight <= 0.0)
        return hdr;

    // Schlick fresnel，与 light.frag 的 IBL specular **同式**（F0 = lerp(0.04, albedo, metal)），
    // 这样 15.2 要做"命中处减掉 IBL specular"时两边可以直接相消。
    const float3 albedo = albedoTex.Load(int3(pix, 0)).xyz;
    const float3 F0 = lerp(float3(0.04), albedo, mr.r);
    const float NoV = saturate(abs(dot(N, V)));
    const float3 F = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);

    const float3 reflected = sceneHdrTex.Sample(hitSampler, hitUv).rgb;
    return float4(hdr.rgb + F * weight * reflected, hdr.a);
}
