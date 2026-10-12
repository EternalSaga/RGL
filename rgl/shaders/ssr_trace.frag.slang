// ssr_trace.frag —— 屏幕空间反射的 **march 本体**（Phase 15 批 15.3 子步 B：自 ssr.frag
// 拆出；march/减法/三条 fade 原样搬，取色加法与粗糙度模糊搬进 ssr_apply.frag）。
//
// ── 形态（为什么拆成 trace + apply 两 pass）──
// 本 pass 只做「反射是什么」：DDA ray march + 命中处置信度（含减 IBL specular），将结果以
// **premultiplied** 形态写进半分辨率的 `ssr_trace`；「反射怎么合成」= 粗糙度感知模糊 +
// 加法，全在 apply pass（`ssr_apply.frag`，全分辨率）。两件事此前长在同一枚全分辨率 pass 里
// （批 15.1 后半的单 pass 形态），拆开买两样东西：
//  ① **成本**：march 的成本 = 每像素采样次数（§1.1 推论①），半分辨率 = 1/4 像素，而反射是
//     低频信号（本来就靠粗糙度模糊看），半分辨率不亏画质；
//  ② **可模糊性**：模糊必须作用在「反射贡献」这张独立图上，且**加法减法必须同源**——
//     premultiplied（rgb 已乘 weight、a = weight）让模糊 = 逐通道均值，加/减两项被同一个
//     权重一起平均，不需要事后重建置信度。
// 输入 = `light` pass 写好的 `scene_hdr`（反射源，全分辨率）+ G-buffer 现成量，输出 =
// `ssr_trace`（半分辨率 RGBA16F）。图内不可能读同帧 SSR 结果（`light` 正在写 `scene_hdr`），
// 所以形态唯一（论证 → `docs/phase15_plan.md` §2.1）。**deferred-only**：权重全从 G-buffer
// 算，forward 图没有 G-buffer（D1 对等豁免 → §0.1，装饰器跳过条件保证本 shader 只在 deferred 跑）。
//
// ── premultiplied 约定（两 pass 之间的契约，改任何一侧都要同步）──
//     rgb = weight * (F * reflected - iblSpec)      a = weight
// 所有未命中 / 早退路径写 `float4(0, 0, 0, 0)`（= 「本 texel 无贡献」，apply 侧的
// 「trace 采样全零 ⟹ 直接 return hdr」早退据此成立 = 影响域判据的前提）。weight 恒 > 0
// （weight ≤ 0 走早退），所以 a == 0 ⟺ rgb 恒 ±0，premultiplied 不变式自洽。
// 旧形态（单 pass 直接 `hdr.rgb + weight·(…)` 写 ssr_applied）里「未作用像素逐位 = hdr」
// 靠每条早退 `return hdr`；新形态里同样的纪律搬到 apply（早退 return Load 的 hdr），本 pass
// 的早退只是写零——**加法不再发生在本 pass**，`scene_hdr` 的 Load 直通也随之一并移除。
//
// ── 半分辨率对齐口径（确定性约定，别改成过滤）──
// 本 pass 的 viewport = 渲染网格 × 0.5（`RgAttachmentDesc::extentScale`，装配侧
// render_graph.cppm kSsrTraceScale）。读全分辨率的 G-buffer / depth / scene_hdr 时取
// **左上纹素**：`int2(pos.xy) * 2`。半分辨率像素 (i+0.5) 覆盖全分辨率 [2i, 2i+1)，取 2i
// 是约定不是采样理论——它确定（同输入同输出逐位）、无过滤（不引入第二张采样网格），且
// 奇数窗口下 2·floor(W/2) ≤ W 恒不越界。
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
// ray march 在**本帧抖动空间**求交（深度缓冲与 `readSsrViewProj()` 同为抖动后口径，见
// scene_data.cppm kSceneDataSsrOffset）。本 pass 的 viewport 是半分辨率，但 **uv 空间不变**
// （pos/extent 各自归一化到 [0,1]，march 起点走 uvFromClip、深度采样走全分辨率纹素），
// 抖动网格与全分辨率 pass 同一张。不做 un-jitter——那是下游时域滤波器的活（同 ao_apply 纪律）。
//
// ── 命中处 = 置信度混合（15.2 子步 C 起；本步只把「加法」搬去 apply，减法留本 pass）──
// 命中且 weight>0 的路径写 premultiplied 的 `weight * (F * reflected - iblSpec)`。
// `scene_hdr` 里已含 light.frag 加的 IBL specular，SSR 命中 ⟹ 那一项该被反射内容**顶替**
// 而不是叠加（旧形态双计偏亮，且「反射到暗物」永远无法让像素变暗 = SSR 开/关差分恒 ≥0）。
// `iblSpec` = light.frag 那一项的**逐式镜像重算**（本 pass 是 post-lighting，拿不到 light
// pass 的中间量；重算比加一张 RT 便宜且无新附件），相消条件逐条对齐 → 最终 return 处注释。
// 减法**留在 trace** 的裁决：premultiplied 均值对「加性两项」一视同仁（同一 weight 一起
// 平均），减法放 apply 反而要 apply 重读 env cubemap + 重算 envBrdf（绑定面翻倍、且相消
// 条件跨 pass 对齐比同 pass 脆弱得多）。
//
// ⚠ **已知边界（跨场景，植被/光源过减）**：light.frag 对植被类（emissive.a∈(0.5,1.5)）把
// envSpec 置 0（light.frag:226 isVeg 分支）、对光源类强制 metallic=0（light.frag:172-173），
// 本 pass 不认识 materialClass ⟹ 若这两类过粗糙度门控会**过减**（减掉 light 根本没加的
// 能量）。本场景安全：植被 roughness 1.0 ≫ gate.y=0.40 ⟹ weight=0，到不了减法行。跨场景
// 若引入光滑植被，减法要随 materialClass 走（届时再还，别提前加绑定、别在此早退）。
//
// ── 绑定面（与 CPU 侧 addSsrTraceEntry 成对；G-buffer 通道号镜像 light.frag）──
//   3 = 空位（批 N+1 起 sceneData 退役；ssrViewProj/ssrParams/ssrGate 走 binding 31；
//       @723776 ssrParams = 尺度锚点 + 三个导出系数；@723792 ssrGate = 粗糙度门控区间
//       （本 pass 只读 x/y，z = blurScale 是 apply 的旋钮）。
//       各槽口径与默认值真相源 → scene_data.cppm / configs/runtime.lua `ssr` 块）
//   4 = scene_hdr（RGBA16F 线性 HDR，反射源；**全分辨率**，命中取色 Sample）
//   5 = gbuffer_normal（RG16_SNORM oct，解法走 scene.slang `octDecode` 单一来源）
//   6 = gbuffer_position（RGBA32_SFLOAT 世界坐标）
//   7 = gbuffer_metalrough（RGBA8_UNORM .r=metal .g=rough）
//   8 = depth（eD32Sfloat 深度图，eDepth aspect；`Load` 直取，不占 sampler）
//   9 = gbuffer_albedo（RGBA8_SRGB，Schlick fresnel 的 F0 要它——金属地面的 F0 就是 albedo，
//       省掉它等于把所有表面当介质，靶的金属地面反射会整个不出来）
//  10 = sampler（共享 LINEAR/ClampToEdge；**只有**命中取反射色用它——其余逐像素自身量全走
//       `Load` 整数纹素。旧单 pass 里「直通 Load 保证未作用像素逐位 = hdr」的判据纪律随
//       直通路径一起搬去 ssr_apply；本 pass 没有直通——输出是新图，早退 = 写零）
//  11 = envMapL(TextureCube) / 12 = envSamplerL(sky sampler)（15.2 子步 C：命中处减 IBL
//       specular 的镜像重算源；light.frag 的 9/10 在本 pass 号接 4-10 之后。env 未上传 =
//       占位 0/0，shader 端 mipRange=0 不采样的契约，light.frag 同款）

import scene;  // readSsrViewProj + readSsrParams + readSsrGate + octDecode + worldFromDepth + uvFromClip + readSceneLight

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;    // 本帧成片（RGBA16F 线性 HDR，全分辨率）
[[vk::binding(5, 0)]] Texture2D<float2> normalTex;      // G-buffer oct 法线（rg16_snorm）
[[vk::binding(6, 0)]] Texture2D<float4> positionTex;    // G-buffer 世界坐标（rgba32_sfloat）
[[vk::binding(7, 0)]] Texture2D<float4> metalRoughTex;  // G-buffer .r=metal .g=rough
[[vk::binding(8, 0)]] Texture2D<float> depthTex;        // eD32Sfloat 深度图（eDepth aspect）
[[vk::binding(9, 0)]] Texture2D<float4> albedoTex;      // G-buffer albedo（rgba8_srgb，F0 用）
[[vk::binding(10, 0)]] SamplerState hitSampler;         // 共享 LINEAR/ClampToEdge
[[vk::binding(11, 0)]] TextureCube<float4> envMapL;     // 环境立方图（GGX 预滤波 mip 链，减法项）
[[vk::binding(12, 0)]] SamplerState envSamplerL;        // sky sampler（maxLod = mipLevels-1）

// env 参数（头区契约字段 SceneHeaderBlock::envParams.xy，light.frag EnvParamsL 同款本地
// 成对取值；偏移与读取面在 scene.slang readEnvIntensity / readEnvMipRange 一处）。
// **为什么不 import env.slang**：其 envMap/envSampler 声明在 binding 4/5，与本 pass 的
// sceneHdr(4) 冲突（light.frag:25 同款理由）——env 绑定本地声明在 11/12，参数走头区 UBO。
struct EnvParamsS
{
    float intensity;
    float mipRange;
};
EnvParamsS readEnvParamsS()
{
    EnvParamsS e;
    e.intensity = readEnvIntensity();
    e.mipRange  = readEnvMipRange();
    return e;
}

struct SsrInput
{
    float4 position : SV_Position;  // **半分辨率** framebuffer 坐标（×2 = 全分辨率左上纹素）
    float2 uv       : TEXCOORD0;    // light.mesh 产出 = framebuffer 对齐 uv（归一后与全分辨率同网格）
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
// DDA 格边长（**半分辨率**像素，15.3B 起）。2 = 半分辨率格 = 全分辨率 4px：格按本 pass 的
// 像素网格走（cellSize 换算见 main），步数与全分辨率时代比再减半——**这就是半分辨率的
// 成本来源**。靶的倒影边界是硬边，格粗于 2 会把边界量化出来（半分辨率下边界量化误差本身
// 已有 2 全分辨率 px，格不再加第三层）。
static const float kCellPx = 2.0;
// 步数上限。**刻意不抄 Filament 的 max(res)**（§1.3 红线：那是移动端小分辨率的宽松上界）。
// 行程上限 = kMaxSteps·kCellPx = 1024 **半分辨率** px = 全分辨率 2048 px（≥ 整屏宽）——
// 它是**兜底**不是预算：真实行程先被 maxDistance / 出屏 / 命中截断，只有横穿全屏的射线才
// 可能把步数走完（靶的倒影需要 ~240 px 行程里的大部分：射线只需走到"被反射内容的像"，
// 不必走完整个倒影区）。扫描表见 §3.2。
static const int kMaxSteps = 512;
// 命中点二分细化次数（在 [上一格穿越 t, 本格穿越 t] 上解 along=0）。
static const int kRefineIters = 4;
// 命中点离屏幕边界多近开始淡出（**全分辨率**像素——edgeFade 用全分辨率 px 算，视觉宽度与
// 拆 pass 前一致）。边界外的内容根本没画，越靠边反射越不可信。
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
// ⚠ 深度采样**恒走全分辨率纹素**（uv × 全分辨率 px）——半分辨率只改「谁执行这段 march」，
// 不改「march 里的求交精度」；深度图本来就是全分辨率的，没有理由降它。
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
    //   算出一个**负 t**（方程的另一支）。调用方拿 `min(tX, tY)` 就会拿到一个身后的 t——2026-10-10
    //   实测：靶上 48% 的倒影像素死在这一条（反射射线几乎垂直上行、x 分量≈0，正是收敛形态）。
    //   真正的穿越永远在 t > 0。
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
    // 半分辨率像素 → 全分辨率**左上纹素**（对齐口径 → 文件头「半分辨率对齐」）。本 pass 里
    // 所有对全分辨率资源（G-buffer / depth）的逐像素 `Load` 一律走 gpix。
    const int2 gpix = int2(input.position.xy) * 2;

    uint pxW, pxH, mLvl;
    depthTex.GetDimensions(0u, pxW, pxH, mLvl);
    // px = **全分辨率**尺寸（depth/G-buffer/scene_hdr 都是全分辨率）：worldPerPixel、
    // edgeFade、sampleAlong 的深度纹素、DDA 格换算全部用它——拆半分辨率后这些口径**不变**，
    // 变的只有 DDA 格（cellPx 乘 2，见下）。
    const float2 px = float2(float(pxW), float(pxH));

    // 天空不反射（它已经是"整个环境"，再叠一层 SSR 是双重计数且会糊掉天顶渐变）。
    if (depthTex.Load(int3(gpix, 0)) >= kFarDepth)
        return float4(0.0, 0.0, 0.0, 0.0);

    const float4 mr = metalRoughTex.Load(int3(gpix, 0));
    const float roughness = mr.g;
    // 粗糙度门控早退（**性能与正确性双重**：粗糙面本就不该有镜面反射，且这条早退让绝大多数
    // 像素根本不进 ray march —— 门控关掉的是整段 march 的成本，不是一个乘数）。
    // 门控区间 = scene data @723792（configs/runtime.lua `ssr.rough_full / rough_cutoff`）：
    // 低于 x = 满强度、高于 y = 完全不反射、中间 smoothstep 过渡（硬切会在材质边界出现可见的
    // 开关线）。⚠ 要求 x < y：反了 = 门控反向（光滑面全关、粗糙面满反射，实测靶上覆盖率恰 0），
    // x == y 时只有 roughness 恰等于该值的像素会 0/0 = NaN（其余被 clamp 收成 0/1）。
    const float4 gate = readSsrGate();
    const float roughGate = 1.0 - smoothstep(gate.x, gate.y, roughness);
    if (roughGate <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    const float3 P = positionTex.Load(int3(gpix, 0)).xyz;
    const float3 N = octDecode(normalTex.Load(int3(gpix, 0)).xy);
    const float3 eyePos = readSceneLight().eyePos;
    const float3 V = normalize(eyePos - P);      // 表面 → 相机（与 light.frag 同向口径）
    const float3 R = reflect(-V, N);             // 反射射线方向（|R| = 1 ⟹ t 就是世界距离）
    if (dot(R, N) <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);       // 退化（数值上不该发生）：宁可不反射

    // 投进屏幕：clip(t) = C0 + t·Cd，对 t 线性（文件头 ①）。
    const ObjectMatrix vp = readSsrViewProj();
    const float4 C0 = mulM(vp, float4(P, 1.0));
    const float4 Cd = mulM(vp, float4(R, 0.0));

    // ── 尺度无关参数（scene data @723776；口径 → scene_data.cppm 该区注释）──
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
            return float4(0.0, 0.0, 0.0, 0.0);
        tMax = min(tMax, tSing);
        singularityFade = saturate(tSing / maxDistance);
    }

    // ── DDA：沿屏幕网格走，逐格判 along 的符号翻转 ──
    // 格边长换算（15.3B 半分辨率口径）：kCellPx 是**半分辨率** px，而本 shader 拿不到自己
    // RT 的尺寸（fragment 无 viewport 查询），按「1 半分辨率 px = 2 全分辨率 px」换算成
    // 全分辨率 px 再除 px——偶数窗口下与 2/traceW **精确相等**；奇数窗口（traceW =
    // floor(W/2)）差 ≤ 1/traceW 相对量。格尺寸只是**遍历粒度**不是正确性契约（沿射线的
    // 穿越解恒精确），所以这个近似无害；**别**为它加 uniform（多一条每帧参数 = 多一个口径）。
    const float2 cellPx = float2(kCellPx * 2.0, kCellPx * 2.0);
    const float2 cellSize = cellPx / px;                         // 一格的 uv 跨度
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
        return float4(0.0, 0.0, 0.0, 0.0);

    const float2 hitUv = uvFromClip(C0 + Cd * tHit);
    if (hitUv.x < 0.0 || hitUv.x > 1.0 || hitUv.y < 0.0 || hitUv.y > 1.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    // ── 权重（三条 fade 的语义照 §1.3 的裁决；数值按本场景重标定）──
    // ① 屏幕边界淡出：边界外的内容没画，越靠边越不可信（edgePx 用全分辨率 px，见常量注释）。
    const float2 edgePx = min(hitUv * px, px - hitUv * px);
    const float edgeFade = saturate(min(edgePx.x, edgePx.y) / kEdgeFadePx);
    // ② 距离淡出：硬截在 maxDistance 会看到一条"反射突然消失"的等高线。
    const float u = saturate(tHit / maxDistance);
    const float distanceFade = 1.0 - smoothstep(0.6, 1.0, u);
    // ③ 朝相机淡出（上面算的，尺度无关）。
    const float weight = roughGate * edgeFade * distanceFade * singularityFade;
    if (weight <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    // Schlick fresnel（**加法**那项的镜面权重）。F0 = lerp(0.04, albedo, metal) 与 light.frag:215
    // 同式；NoV 口径 `saturate(abs(dot(N,V)))` 只服务这一项（背面像素的 fresnel 不该塌成 1）。
    const float3 albedo = albedoTex.Load(int3(gpix, 0)).xyz;
    const float3 F0 = lerp(float3(0.04), albedo, mr.r);
    const float NoV = saturate(abs(dot(N, V)));
    const float3 F = F0 + (1.0 - F0) * pow(1.0 - NoV, 5.0);

    // ── 命中处减 IBL specular（15.2 子步 C；15.3B 裁决：减法留在 trace，见文件头）──
    // iblSpec = light.frag 加进 scene_hdr 的那一项（light.frag:218-226 的 env.mipRange>0 分支）
    // 的**逐式镜像重算**，相消条件逐条对齐（行号 = light.frag）：
    //   · R = reflect(-V, N)           —— light.frag:220 同式，且与本文件射线用的 R 同一个
    //   · LOD = roughness·mipRange     —— light.frag:222；mipRange=0 时 light 走常 ambient 回落、
    //     根本没加 envSpec ⟹ 外层 if 让整项置 0（env 未上传的占位绑定因此恒不采样）
    //   · roughness = clamp(mr.g, 0.04, 1.0) —— light.frag:164 同款钳制（本文件**门控**用的是
    //     未钳制那份，两口径别混；出厂资产双峰 0.122/≥0.74 均 > 0.04，本场景两口径逐位等价，
    //     跨场景 mr.g < 0.04 时才分家——届时不钳 = 减法与 light 不相消）
    //   · envBrdfFactor(NdotV, roughness, F0)，NdotV = max(dot(N,V), 1e-4) —— light.frag:216。
    //     ⚠ **不是**上面给 Schlick F 用的 saturate(abs(...))：abs 只留给加法那项的 fresnel，
    //     减法必须镜像 light 的口径，否则背面像素不相消
    //   · × env.intensity              —— light.frag:226
    // 已知边界（植被/光源过减）→ 文件头。
    const EnvParamsS env = readEnvParamsS();
    const float roughClamped = clamp(roughness, 0.04, 1.0);
    float3 iblSpec = float3(0.0);
    if (env.mipRange > 0.0)
    {
        const float3 envSpec =
            envMapL.SampleLevel(envSamplerL, R, roughClamped * env.mipRange).rgb;
        iblSpec = envSpec * envBrdfFactor(max(dot(N, V), 1e-4), roughClamped, F0) * env.intensity;
    }

    // premultiplied 输出（契约 → 文件头）：rgb 已乘 weight、a = weight。加法在 ssr_apply。
    const float3 reflected = sceneHdrTex.Sample(hitSampler, hitUv).rgb;
    return float4(weight * (F * reflected - iblSpec), weight);
}
