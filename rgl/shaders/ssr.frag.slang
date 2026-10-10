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
//     （批 15.1 收尾：三个量仍走世界单位口径，但定值改从 SceneBounds.radius 与
//     worldPerPixel **导出**——米制常量在 scale=0.01 的交付场景下不可解读，参数区有账。）
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
//   3 = sceneData（import scene；@722272 ssrViewProj = 本帧**抖动后** proj·view；
//       @722336 ssrParams = 尺度锚点 + 三个导出系数；@722352 ssrGate = 粗糙度门控区间。
//       后两槽的口径与默认值真相源 → scene_data.cppm / configs/runtime.lua `ssr` 块）
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

import scene;  // sceneData + readSsrViewProj + readSsrParams + octDecode + worldFromDepth + uvFromClip + readSceneLight

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

// ── 参数（**全部来自 scene data 尾区**，配置面 = configs/runtime.lua `ssr` 块）──
// 本 shader 里**没有一枚画质旋钮的默认值**：默认值的唯一真相源 = C++ 侧 `RuntimeConfig`
// 成员初始化值（两处定义迟早漂移，这是本项目反复登记的反模式）。缺块/缺键 = 结构体默认，
// 走的是同一条填充代码 ⟹ 「门关」与「门开但参数默认」共用同一条路径。
// 旧版这里是三枚按闭式靶（米制）调的世界单位常量，而交付场景 bistro 是 scale=0.01
// （世界包围球 r≈1.16）⟹ 同一常量不可能同时服务两者（2026-10-10 实测：靶
// SceneBounds.radius=31.06 / bistro radius=1.16）⟹ 三个量全部改从引擎已有的量导出，
// 系数本身进配置面。导出量：maxDistance = sceneRadius·maxDistanceRadii，
// thickness/bias = 像素数 × worldPerPixel（采样点口径，见 sampleAlong）。
//
// ⚠ **退化系数一律不 clamp**（自己定并写理由，2026-10-10）：
//   · `thickness_px = 0` 与 `bias_px >= thickness_px` 是**合法消融档**（判据 = 闭式覆盖率
//     恰为 0），clamp 掉它们 = 把反向消融静默改成另一档 = 消融失效；
//   · 逐条查过每条除法，**没有一条会 NaN/除零**：maxDistance ≤ 0 ⟹ tMax ≤ 0 ⟹ 循环里
//     `tStop > tCur` 恒假 ⟹ 永不命中（`tHit/maxDistance` 那条除法根本到不了），而
//     `tSing/maxDistance` 的 ±inf 被 `saturate` 收成 [0,1]；
//   · 系数为负 = 命中窗口 [−thickness, −bias] 变空或变宽，两种都"照字面执行"，
//     比"看起来正常其实被悄悄改过"好。
//
// ⚠ **两条 shader 侧不变式**（配置层不校验，理由 → `SsrTuning` 字段注释与
//   `readSsrBlock`）：
//   `biasPx < thicknessPx`（否则命中窗口为空 = 永不命中）、`roughFull < roughCutoff`
//   （否则门控**反向**：e0 > e1 时除数为负 ⟹ 光滑面 gate=0、粗糙面 gate=1；e0 == e1 时
//   仅 roughness 恰等于 e0 的像素出 0/0 = NaN，其余按 ±inf 被 clamp 收成 0/1）。违反都是
//   **可见**失效（实测反向档：凸包内覆盖率**恰 0**、凸包外泄漏 0 = "SSR 整个不起作用"）
//   且 `just ssr-check` 判据 A 直接红，不是静默漂移。
//
// 下面四个**刻意留在 shader 里当常量**：`kCellPx`（DDA 格边长）、`kMaxSteps`（步数上界）、
// `kRefineIters`（二分次数）、`kEdgeFadePx`（屏幕边界淡出宽度）。前三个是 **shader 形态与
// 成本决策**（循环上界与网格边长进配置 = 编译器拿不到常量 ⟹ 循环展开/提前退出全废，
// 且"步数上限"根本不是画质旋钮而是兜底保险丝）；第四个是屏幕像素域的经验常数，
// 与场景尺度无关，本批没有要扫它的人。
// DDA 格边长（像素）。2 = 步数减半；靶的倒影边界是硬边，格粗于 2 会把边界量化出来。
static const float kCellPx = 2.0;
// 步数上限。**刻意不抄 Filament 的 max(res)**（§1.3 红线：那是移动端小分辨率的宽松上界）。
// 行程上限 = kMaxSteps·kCellPx = 1024 px（1280 宽 = 整屏宽）——它是**兜底**不是预算：
// 真实行程先被 maxDistance / 出屏 / 命中截断，只有横穿全屏的射线才可能把步数走完
// （靶的倒影需要 ~240 px 行程里的大部分：射线只需走到"被反射内容的像"，不必走完整个
// 倒影区）。扫描表见 §3.2。
static const int kMaxSteps = 512;
// 命中点二分细化次数（在 [上一格穿越 t, 本格穿越 t] 上解 along=0）。
static const int kRefineIters = 4;
// 命中点离屏幕边界多近开始淡出（像素）。边界外的内容根本没画，越靠边反射越不可信。
static const float kEdgeFadePx = 24.0;
// 天空判据：深度 = 远平面清值（skybox 大三角 z=1 靠 depth clear + LEQUAL，不写深度）。
// 阈值与 ao.frag:106 同值同因——两处必须一起改，否则 AO 当天空而 SSR 当几何（或反之）。
static const float kFarDepth = 0.9999;

// 沿射线求交的一个采样结果。
struct MarchSample
{
    float along;          // 世界空间有符号距离：>0 几何还在射线前方，<0 射线已到几何后方
    bool  usable;         // false = 该处无法求交（出屏 / 落到相机平面之后）⟹ 调用方必须停走
    float worldPerPixel;  // **采样点**视深处一像素的世界长度（thickness/bias 是作用在采样点
                          // 上的判据，必须用它、不是起点 C0.w 那份——两者可差数倍）
};

// 屏幕某处的世界点相对射线的位置（判据本体，见文件头 ②）。
MarchSample sampleAlong(float4 c0, float4 cd, float t, float3 P, float3 R, float2 px, float tanHalfFovY)
{
    MarchSample s;
    const float4 c = c0 + cd * t;
    if (c.w <= 1e-5)  // 落到相机平面之后：投影翻转，结果无意义
    {
        s.along = 0.0;
        s.usable = false;
        s.worldPerPixel = 0.0;
        return s;
    }
    // 视深 c.w（沿相机**前向**，不是到相机的距离）处一像素的世界长度：
    //     worldPerPixel = 2·c.w·tan(fovY/2)/px.y
    // tanHalfFovY 的来源（**别退回 vp.c1.y**）：viewProj 的 row1 = proj[1][1]·(view 的 row1)，
    // 而 view 是刚体 ⟹ 旋转部分每行是**单位向量** ⟹ `length(row1.xyz) = proj[1][1] =
    // cot(fovY/2)` 精确（2026-10-10 数值验证相对误差 1.8e-16）。直接用 vp.c1.y 会多带一个
    // view[1][1] = up.y = cos(pitch) 因子（靶 pitch 8° → 偏 1%，交付机位 bistro pitch≈24° →
    // 偏 **9.5%**），等于让 thickness/bias 随相机俯仰漂。抖动只改 proj 的 row2，不污染 row1。
    s.worldPerPixel = 2.0 * c.w * tanHalfFovY / px.y;
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
    // 门控区间 = scene data @722352（configs/runtime.lua `ssr.rough_full / rough_cutoff`）：
    // 低于 x = 满强度、高于 y = 完全不反射、中间 smoothstep 过渡（硬切会在材质边界出现可见的
    // 开关线）。⚠ 要求 x < y：反了 = 门控反向（光滑面全关、粗糙面满反射，实测靶上覆盖率恰 0），
    // x == y 时只有 roughness 恰等于该值的像素会 0/0 = NaN（其余被 clamp 收成 0/1）。
    const float4 gate = readSsrGate();
    const float roughGate = 1.0 - smoothstep(gate.x, gate.y, roughness);
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

    // ── 尺度无关参数（scene data @722336；口径 → scene_data.cppm 该区注释）──
    // 一次读满 vec4：x = sceneRadius（尺度锚点）、y = maxDistanceRadii、z = thicknessPx、
    // w = biasPx（configs/runtime.lua `ssr` 块，**shader 侧无默认值**）。
    const float4 ssrParams = readSsrParams();
    // 空场景兜底：SceneBounds.radius = 0 是它的"空"哨兵 ⟹ maxDistance 会是 0，而下面
    // `tSing / maxDistance` 是除法。1e-6 下限**不是调定值**、是防零除守卫——空场景没有像素
    // 能过深度门（全是天空，早已早退），这条路径实际到不了。
    const float maxDistance = max(ssrParams.x, 1e-6) * ssrParams.y;
    // cot(fovY/2) = |viewProj 的 row1.xyz|（view 刚体 ⟹ 单位 up 行 × proj[1][1]），
    // 精确且抖动无关；口径与"为什么不用 vp.c1.y" → sampleAlong 内注释。
    const float tanHalfFovY = 1.0 / length(float3(vp.c0.y, vp.c1.y, vp.c2.y));
    // 起点处 worldPerPixel：起点用的两处 bias（奇异平面截断、零长度步判据）都是射线**起点**
    // 的判据；命中窗口（bias/thickness）用**采样点**那份（sampleAlong 带出），两者可差数倍。
    const float bias0 = ssrParams.w * (2.0 * C0.w * tanHalfFovY / px.y);

    // 射线朝相机时会在 t = −C0.w/Cd.w 处撞上 w=0 奇异平面。那里投影翻转、屏幕坐标乱跳，
    // 必须硬截断；同时把"还剩多少可用行程"当淡出——这是**尺度无关**的朝相机淡出，刻意不用
    // Filament 的 `1 − max(0, vsDir.z)`（那要假设 proj 的 w 行是单位向量，本引擎的投影惯例
    // 不该被 shader 这样依赖）。
    float tMax = maxDistance;
    float singularityFade = 1.0;
    if (Cd.w < 0.0)
    {
        const float tSing = -C0.w / Cd.w;
        if (tSing <= bias0)
            return hdr;
        tMax = min(tMax, tSing);
        singularityFade = saturate(tSing / maxDistance);
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
    //   像素整条射线走到预算耗尽、且**漏与不漏按行交替**）。`along < −bias` 留了条死区
    //   (−bias, 0]（bias = biasPx·采样点 worldPerPixel）：采样一旦落进死区，"上一个为正"就
    //   **永久失效**，之后无论穿多深都再也判不出命中。本靶恰好把这条放大到必然发生——平坦镜面
    //   的 `along` 零点与**被反射物轮廓的入口**重合（射线到达柱面的那一刻才进柱子轮廓），所以
    //   "零点附近那一次采样"必然出现。
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
        if (tStop > tCur && tStop > bias0)
        {
            const MarchSample s = sampleAlong(C0, Cd, 0.5 * (tCur + tStop), P, R, px, tanHalfFovY);
            if (!s.usable)
                break;                       // 出屏 / 奇异平面之后：再走也没有意义
            if (s.along > 0.0)
            {
                sawFront = true;
                tFront = 0.5 * (tCur + tStop);
            }
            else if (sawFront && s.along < -ssrParams.w * s.worldPerPixel)
            {
                // ⚠ **防 "sticking" 假命中**（本批实测：删掉这条 ⟹ 靶上柱子**两侧**的地面多出
                //   34170 px 的反射，幅度 p50=0.022 与真倒影同量级）。那些射线是在柱子**后方几米**
                //   经过的，只是屏幕路径压过了柱子的轮廓；`along` 的表现为**一步从 +很大跳到 −很大**
                //   （Q 从远处地面跳到近处柱面），而真命中的 `along` 是**平滑降到 0** 的，一步最多
                //   跨 Δt ≈ tMax/kMaxSteps。所以"这一步穿进几何的深度"就是两者的判别量。
                //   ⟹ 判据必须作用在**细化之前**的采样上：二分只会把任何跳变都收敛到 along≈0，
                //   细化之后再判就永远看不出区别了。
                if (-s.along > ssrParams.z * s.worldPerPixel)
                    break;
                // 二分细化到 along = 0，把命中点从"格中点"提到亚像素。下界取**最后一次
                // along > 0 的参数**（不是 tCur：tCur 处的采样可能正落在上面那条死区里，
                // 拿它当正侧端点会让二分从头到尾没有异号区间、收敛到错误的一侧）。
                float a = tFront, b = 0.5 * (tCur + tStop);
                for (int i = 0; i < kRefineIters; ++i)
                {
                    const float m = 0.5 * (a + b);
                    const MarchSample r = sampleAlong(C0, Cd, m, P, R, px, tanHalfFovY);
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
    const float u = saturate(tHit / maxDistance);
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
