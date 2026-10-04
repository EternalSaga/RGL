// Phase 8 批次 ①：HDR tonemap pass —— scene_hdr（RGBA16F，线性 HDR）→ 曝光 ×
// Narkowicz ACES 近似 → swapchain（B8G8R8A8_SRGB，store 硬件 linear→sRGB 编码，
// 故本 frag **不做** pow(1/2.2)——手动 gamma + 硬件编码 = 双重编码发白）。
// mesh 复用 light.mesh.spv（全屏大三角，uv = framebuffer 对齐 1:1），固定全屏单
// group draw（drawLight kind，state="tonemap"，执行器经 state 名解析管线条目）。
//
// 绑定：4=sceneHdr（Texture2D RGBA16F 线性）/ 5=hdrSampler（共享 sampler，1:1
//   全屏采样 maxLod 0 够用）。import scene 仅为读 scene data 头 @56 的曝光标量
//   （CPU 真相源 vkresources.cppm kSceneDataExposureOffset；configs/scene.lua
//   顶层 exposure 每帧经 SceneFrameData 上传——原写死 kExposure 的"调参再走
//   配置"预留兑现）。
//
// Phase 11 批 11.4 追加 AO 施加：binding 6 = ao pass（state="ao"）产出的 R8 遮蔽量，
//   lerp(1, ao, strength) 相乘后再进 ACES。strength 读 scene data @60（头部保留区
//   最后一格，CPU 真相源 scene_data.cppm kSceneDataAoOffset；configs/scene.lua 的
//   ao.strength 每帧经 SceneFrameData 上传）。**strength=0 时 lerp 位级 = 1.0**，
//   故默认配置逐位等于加 AO 之前（BIt-IDENTICAL 基线不受影响）。
//   施加点选在合成（此处）而非材质着色器：forward 无 G-buffer，AO 只在 opaque+skybox
//   之后产出，写在 tonemap 是唯一能对两变体一视同仁的位置（D1 对等）。代价 = AO 会
//   连直接光一起压（见 docs/phase11_plan.md 批 11.4 的偏差说明）。
//
// ── 批 13.2 基空间输出（un-jittered resolve，门 `taa.base_space`）──
// 症状与机制（全案 → docs/phase13_plan.md §1.9 + 调试历史 2026-10-03/10-04）：TAA 的
// 输出**长在抖动空间**——静止机位下收敛后 `O_t(x) = F_t(x − c_t)`（c_t = 本帧内容位移），
// 于是相邻两帧是精确平移 `|c_t − c_{t−1}|`（实测 0.717px 均值 / 1.051px 最大，闭式满幅
// 0.667px），**与 feedback 无关** ⟹ 时域累积在原理上消不掉它，只能改输出口径。
// 本 pass 就是那个输出口径：按 `+c_t` 偏移采样即得 `F_t(x)`，落回基网格，游动从定义上消失。
// 同族做法 = FSR2 / XeSS / Unity TAAU / Unreal TSU（抖动渲染 → 累积 → resolve 到输出网格）。
//
// **为什么折进本 pass 的采样坐标 = 零额外 pass**（先核实 pass 序再动手）：真实执行序 =
// opaque → skybox → transparent → ao → **taa** → **tonemap** → taa_commit(blit)。
// `decorateTaaPass` 把 taa pass 插在「写 swapchain 且 sampleRead 色附件」的 pass 之前，
// 并把那个 pass（= 本 tonemap）的色输入从 scene_hdr **改写成 taa_out**；taa_commit 只是
// 帧末 taa_out → taa_hist 的整幅拷贝（blit 带不了偏移采样，但它不是输出口径）。
// ⟹ 本 pass 本来就是全屏 1:1 消费 taa_out 的唯一最终消费者，偏移折进它的采样坐标
//    不增加任何 pass、不增加任何纹理。
//
// **抖动空间纹理表**（凡在此表者必须**一起**偏移，否则色彩与 AO 错开半像素）：
//   ┌ 纹理（binding）        ─ 为什么在抖动空间 ─────────────────────────────┐
//   │ sceneHdrTex (4)  恒在抖动空间：TAA 开 = taa_out（抖动投影渲的成片时域累积），
//   │                  TAA 关 = scene_hdr（同样由抖动投影渲，只是 c_t 恒 0）
//   │ aoTex (6)        恒在抖动空间：ao pass 以 1:1 像素位置读**抖动后的深度**算遮蔽量，
//   │                  其像素网格与 sceneHdrTex 同源
//   └──────────────────────────────────────────────────────────────────────┘
// **基空间纹理（禁止偏移）**：swapchain 本身（输出网格）、imgui/UI 覆盖层（另走 imgui
// pass，直接画 swapchain）、object_id（picking 与抖动同口径，不经本 pass）。
// 深度不进本 pass（tonemap 不采深度），无需处理。
//
// **门关红线**：`readTaaResolveOffset().z < 0.5`（= `taa.base_space` 关，CPU 侧写全 0）
// ⟹ 走**原样返回 uv** 这条分支，采样坐标与本文件改动前逐位相同（不是「加 0」，是不调用
// 偏移计算——同 ao.strength=0 的旁路纪律，但更强）。

import scene;  // sceneData buffer（binding 3：Load(56) 曝光 / Load(60) AO 强度 / @722224 偏移）

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;
[[vk::binding(5, 0)]] SamplerState hdrSampler;
[[vk::binding(6, 0)]] Texture2D<float> aoTex;  // SSAO **可见度**（r8_unorm，ao pass 产出，1=通透）

// 曝光（ACES 输入前的线性倍率）：scene data @56，configs/scene.lua exposure，
// 1.0 = 直通（默认，与旧写死常量逐位一致）。
float readExposure()
{
    return asfloat(sceneData.Load(56));
}

// AO 施加强度：scene data @60，configs/scene.lua ao.strength，0 = 逐位旁路。
float readAoStrength()
{
    return asfloat(sceneData.Load(60));
}

// ── 基空间输出：把「本帧内容位移」换算成 uv 偏移（批 13.2，见文件头注释）──
// 门关（`taa.base_space = false` ⟹ CPU 写全 0 ⟹ z = 0）时**原样返回 uv**，
// 采样坐标与本文件加此函数之前逐位相同（红线）。
float2 baseSpaceUv(float2 uv)
{
    const float4 r = readTaaResolveOffset();  // (offsetX px, offsetY px, 门 1/0, 保留)
    if (r.z < 0.5)
        return uv;
    uint w, h, lvl;
    sceneHdrTex.GetDimensions(0u, w, h, lvl);
    // ⚠ 必须显式钳回 [0,1]：共享 sampler 的 wrap 是 **REPEAT**（`sampler_library.cppm`
    // 平铺 UV 定案，bistro 49.5% primitive 越界 [0,1]），而屏幕空间 uv 越界绕回 = 最外
    // 一圈采到**对侧**像素（1px 宽的假边）。钳到 [0,1] 等价于 CLAMP_TO_EDGE——双线性核
    // 对 [0,1] 内的亚像素越界本就把取数钳到边缘纹素，不改内部像素一个 bit。
    return saturate(uv + float2(r.x, r.y) / float2(float(w), float(h)));
}

// ── 基空间重采样核 = 双线性（选型结论；备选核的实测数据在下面，别再重新试一遍）──
// 曾实现并实测过精确 4×4 Catmull-Rom（16 次整数 Load，与 `taa.frag.slang::
// sampleHistoryCatmullRom` 同核），**否决**。本机 bistro 1280×720、交付配置 + 用户机位、
// fb=.12 js=1、2026-10-04：
//   梯度能量（真实场景，越高越锐）  门关 0.038272 ｜ 门开双线性 **0.034843** ｜ CR 0.036999
//   `imgqa aa-coverage` 域内最大误差 门关 0.138 列 ｜ 门开双线性 **0.119 列** ｜ CR 0.181 列
//   黑晕「凭空多黑」                门关 0.026 列 ｜ 门开双线性 0.016 列 ｜ CR **−0.030 列**
// ⟹ CR 确实更锐（+6.2% 梯度），但在**本项目已登记的抗锯齿判据**上退化（0.119 → 0.181 列，
//   比门关基线 0.138 还差），且「凭空多黑」翻成**负号** = 负瓣在黑白边界造出振铃（杆边凭空
//   变亮）。双线性两个口径都优于门关基线。梯度能量这条**没有闭式参考**，单看它会得出相反
//   结论——与 `taa_plan` §7 第 10 条同一课：拿没有闭式的量当主判据，结论随量的选择翻转。
//   故按有闭式的那条（逐杆覆盖率对拍闭式）定案 = **双线性**。
// 代价如实记录：相对「TAA 关」的梯度能量保留率 71.8% → 65.4%（糊倍数 1.39× → 1.53×，
//   resolve 自己再加约 9.8% 糊）。它**不逐帧复利**——偏移只加在最终消费处，历史仍留在抖动
//   空间、每帧不被重采样（复利糊化的教训 = phase13 §1.6，已修，勿倒退）。

float3 sampleHdrBaseSpace(float2 uv)
{
    return sceneHdrTex.Sample(hdrSampler, uv).rgb;
}

// Narkowicz 2015 "ACES Filmic Tone Mapping Curve" 近似（per-channel 有理二次式，
// RRT+ODT 的曲线拟合；输出仍在显示线性域，交给 sRGB attachment 硬件编码）。
// 矩阵存放/乘法约定：内层花括号 = 行（行主序字面量），mul(v, M) = 行向量×矩阵
// （v 乘第 i 行取分量 j）——此约定下"中性输入不变"要求 **M 的列和 = 1**。
// kACESInput = 标准 ACESFitted sRGB→AP1 矩阵（Narkowicz/MJP：行和为 1 的那张）的
// **转置**存放（列和 = 1 正是原矩阵行和），配 mul(x, M) 方向使用；kACESOutput =
// 标准 AP1→sRGB 矩阵的转置（原本就正确，勿动）。2026-09-15 勘误：旧值第 2/3 行
// 系转写错误（抄成行和≈1 的错版），白输入灰化 + 通道串扰方向反了。
static const float3x3 kACESInput = {
    { 0.59719, 0.07600, 0.02840 },
    { 0.35458, 0.90834, 0.13383 },
    { 0.04823, 0.01566, 0.83777 },
};
static const float3x3 kACESOutput = {
    {  1.60475, -0.10208, -0.00327 },
    { -0.53108,  1.10813, -0.07276 },
    { -0.07367, -0.00605,  1.07602 },
};

float3 acesTonemap(float3 x)
{
    float3 v = mul(x, kACESInput);
    float3 a = v * (v + 0.0245786) - 0.000090537;
    float3 b = v * (0.983729 * v + 0.432951) + 0.238081;
    return saturate(mul(a / b, kACESOutput));
}

struct TonemapInput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;  // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

[shader("fragment")]
float4 main(TonemapInput input) : SV_Target
{
    // 采样坐标 = 基空间偏移后的 uv（门关时逐位 = input.uv，见 baseSpaceUv）。
    // ⚠ 两张抖动空间纹理**必须用同一个 uv**（sceneHdrTex 与 aoTex 同网格，见文件头
    //   「抖动空间纹理表」）——分开算会因浮点结合序差出亚像素错位。
    const float2 uv = baseSpaceUv(input.uv);
    float3 hdr = sampleHdrBaseSpace(uv);
    // AO 在曝光**之前**相乘：遮蔽是入射光的衰减，属物理量，曝光是显示映射。
    // aoTex 与 sceneHdrTex 同为 1:1 framebuffer 对齐，hdrSampler（linear/mipLod 0）
    // 采 R8 可见度与采 HDR 图同条件——AO 本身已逐像素算好，无需额外滤波。
    // 注意 ao 是**可见度乘数**（1=不遮蔽）：strength=1 且 ao=0 才会压成全黑。
    const float ao = aoTex.Sample(hdrSampler, uv).r;
    hdr *= lerp(1.0, ao, readAoStrength());
    float3 mapped = acesTonemap(hdr * readExposure());
    return float4(mapped, 1.0);
}
