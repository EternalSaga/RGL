// Phase 11 批 11.4：SSAO（depth-only 屏幕空间采样）—— 输出单通道**可见度**
// （1 = 完全通透、0 = 完全遮蔽），由 tonemap.frag 以 `lerp(1, ao, strength)` 直接相乘
// 施加（强度在 scene data @60，0 = 逐位旁路）。
//
// **语义是可见度不是遮蔽量**——内部循环累加的是 occlusion，出口取 1−x。写成遮蔽量会被
// tonemap 当乘数把开阔处（occlusion=0）直接乘成黑（实测 strength=1 整幅全黑）。存可见度
// 的另一好处：RT 本身就是一张可直接复合的乘数图，将来接模糊/升采样不必再翻转。
//
// 为什么 depth-only：本 pass 同时挂在 forward 与 deferred 两张图上，forward 没有 G-buffer
// 可读法线；加 depth prepass 等于把 283 万三角形的光栅化再跑一遍。法线改由"重建世界坐标的
// 屏幕空间导数叉积"给出——不增几何、不增附件。
//
// 消费的 scene data 槽位**全部既有**，本 shader 不新增任何契约字段：
//   @12 eyePos vec3（scene.slang 头内）—— 视线方向判"采样点是否更靠相机"；
//   @64 skyboxInvViewProj mat4 列主序 = inverse(proj·view)（sky.slang skyClipToWorld
//   同款读法）—— 深度 ↔ 世界坐标双向重建的唯一依据。
//
// 已知近似（有意取舍，docs/phase11_plan.md 批 11.4 记）：
//   · 屏幕空间螺旋采样 + 世界空间三测试，**没有** HBAO/SAO 那种"把半球采样点投影回屏幕
//     再比深度"的 range check —— 后者需要 proj·view，而 scene data 头只有逆矩阵。
//     代价 = 前景物体轮廓外一圈会出现 AO 光晕（前景物被误判为遮挡背景的半球）。
//     深度偏置 + 半径收紧可压到可接受；Bistro 街道以凸体为主，实测不明显。
//   · 法线来自 2×2 quad 导数，深度量化误差表现为轻微块状（1280×720 下不可见）。
//   · 深度 == 1.0（天空像素，skybox 不写深度）直接输出 0 = 不遮蔽。

import scene;  // 头区 UBO（binding 22）：readSceneEyePos / readAoStrength + worldFromDepth

[[vk::binding(4, 0)]] Texture2D<float> depthTex;  // eD32Sfloat 深度图（eDepth aspect）
[[vk::binding(5, 0)]] SamplerState pointSampler;  // AO 专用 NEAREST sampler（不可用共享
                                                  // sampler：那份是 LINEAR，而深度图不可过滤）

// ---- 调参常量（look-dev 值；configs/scene.lua 的 ao.strength 只控"施加多强"）----
// 采样数：GPU 预算硬约束（Phase 9：≤3 ms/帧余量）下 12 是 1280×720 的甜点，再加收益
// 递减而 pass 时长线性增长。
static const int kSampleCount = 12;
// 世界空间 AO 半径（米）。Bistro = 真实米（人行道宽约 3 m）；0.5 m 覆盖石缝 / 墙脚 /
// 花坛边这类"接触遮蔽"尺度，再大就吃到建筑尺度而把整条街压脏。
static const float kRadius = 0.5;
// 沿视线方向的深度偏置（米）：压掉自遮挡（否则平面自遮蔽成暗斑）+ 轮廓光晕。
static const float kDepthBias = 0.03;
// 半球约束余量：dot(dir, N) 需大于该值才算"在表面正半球内"的遮挡候选。
static const float kHemisphereCut = 0.1;
// 遮蔽增益：加权累加的单样本贡献只有 (1−d/R)·hemi（典型 0.2~0.35），不放大则直角角落
// 也只压暗 ~17%（实测 p1 = 0.933，肉眼无感）。×3 后角落约落到 0.5，与常见 SSAO 观感一致。
// （取代早先的 pow(x, 2) 曲线——那是在二值累加下才有意义，加权后会二次压制。）
static const float kGain = 3.0;

// `clipToWorld` / `worldFromDepth` 自批 14.4 起是 **scene.slang 的共享函数**（本文件曾是
// 它的出处之一，与 taa.frag 各存一份逐字相同的拷贝；抬升理由与口径见 scene.slang 段头）。

// 哈希 → [0,1)（Wang hash）。逐像素抖动采样起始相位，否则固定方向会织出可见摩尔纹。
float hash12(float2 p)
{
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

struct AoInput
{
    float4 position : SV_Position;  // 像素中心（framebuffer 坐标，xy 可直接当像素号）
    float2 uv       : TEXCOORD0;   // light.mesh 透传的 framebuffer 对齐 uv（1:1）
};

[shader("fragment")]
float main(AoInput input) : SV_Target
{
    const float2 pixel = input.position.xy;
    const float depth = depthTex.Sample(pointSampler, input.uv).r;
    const float3 P = worldFromDepth(input.uv, depth);

    // ===== 导数区：整体必须位于任何分支之前 =====
    // 求导函数要求 quad 内一致执行；"先 return 再 ddx"的写法会让 ddx 落入非一致控制流
    // = UB（天空/几何交界处那种半个 quad 是天空的情况最容易触发）。故本函数所有早退
    // 一律排在下面这段之后，天空像素这里算出的 N 是垃圾但不会被用到。
    //
    // 法线 = 重建世界坐标的屏幕空间导数叉积。SAO 论文的取法：不必额外采 4 个邻域，
    // ddx/ddy 在 2×2 quad 上求值即得面法线。
    float3 N = normalize(cross(ddx(P), ddy(P)));
    const float3 eye = readSceneEyePos();
    const float3 viewDir = normalize(eye - P);  // 视线：P → 相机
    // 叉积手性取决于 quad 内 uv 递增方向，叉出的法线可能朝里；统一翻成朝向相机。
    if (dot(N, viewDir) < 0.0)
        N = -N;

    // 屏幕空间步长：kRadius 在当前像素处的**世界尺度**下的像素数——远处自动稀疏、
    // 近处自动加密（相邻像素的世界间距 = |ddx(P)| + |ddy(P)| 即一像素多少米）。
    // 一个 texel 的 uv 尺寸。**不查纹理尺寸**（本项目 slang 目标面无 getDimensions
    // 成员，且 sky.slang 的既定纪律就是"免 resolution"）：1:1 对齐的插值器 uv 满足
    // fwidth(uv) = (1/width, 1/height)，正是所需——且对半分辨率 AO 也自动正确。
    const float2 texel = fwidth(input.uv);
    // 一个像素 ≈ 多少米（相邻像素的世界间距）。近处大、远处小。
    const float pixelWorld = max(length(ddx(P)) + length(ddy(P)), 1e-4);
    // 屏幕空间步长：把 kRadius 米折成 uv，且至少 1 texel（否则近处步长 <1 像素会退化成
    // 自采样、AO 恒 1）
    const float2 stepUv = max(kRadius / pixelWorld, 1.0) * texel;

    // ===== 分支区：以下不再有任何求导 =====
    // 强度 = 0 = 关闭（configs/scene.lua ao.strength 缺省 0 → 头区 envParams.w）。早退让
    // "默认关闭"真正零成本：12 采样核整段跳过，只留一次纹理读 + 导数。tonemap 侧按同一
    // strength 旁路，故关闭时输出逐位等于加 AO 之前（BIT-IDENTICAL 基线不受影响）。
    if (readAoStrength() <= 0.0)
        return 1.0;
    // 天空像素：深度 = 远平面清值（skybox 大三角 z=1 靠 depth clear + LEQUAL，不写深度）
    // → 无遮蔽。阈值 0.9999 而非 ==1.0：clear 值位级未必精确是 1。
    if (depth >= 0.9999)
        return 1.0;

    // 累加的是**加权**遮蔽（见下三测试后的权重说明），分母固定 kSampleCount：越界/落天空
    // 而被跳过的样本按"未遮蔽"计入，等于在屏幕边缘与轮廓处自动衰减 AO——这是有意的
    // （宁可边缘少遮，不要一圈假暗描边）。
    float occlusion = 0.0;
    const float goldenAngle = 2.39996323;  // 黄金角（弧度）：N 点在圆内均匀铺开
    [unroll]
    for (int i = 0; i < kSampleCount; ++i)
    {
        // 核方向：黄金角螺旋 + 逐像素随机起始相位（打散螺旋的规则性）
        const float fi = float(i) + hash12(pixel + float2(float(i), float(i) * 1.7));
        const float angle = fi * goldenAngle;
        const float radiusFrac = sqrt(fi / float(kSampleCount));
        const float2 dir = float2(cos(angle), sin(angle)) * radiusFrac * stepUv;

        const float2 sampleUv = input.uv + dir;
        // 越界（uv 越出 [0,1] = 采到屏幕外）直接跳过——border 未定义。
        if (any(sampleUv < 0.0) || any(sampleUv > 1.0))
            continue;
        const float sampleDepth = depthTex.Sample(pointSampler, sampleUv).r;
        if (sampleDepth >= 0.9999)
            continue;  // 采样点落在天空 = 那里没有遮挡物

        const float3 delta = worldFromDepth(sampleUv, sampleDepth) - P;
        const float dist = length(delta);
        // ① 半径测试：超出 AO 半径的遮挡物不计入
        if (dist > kRadius)
            continue;
        const float3 dirN = delta / max(dist, 1e-4);
        const float hemi = dot(dirN, N);
        // ② 半球测试：只在表面正半球方向找遮挡物（否则背面的墙会误遮正面）
        if (hemi < kHemisphereCut)
            continue;
        // ③ 视线测试：遮挡物必须**比被测点更靠近相机**，否则是被遮挡物自身后方的
        //    几何（同 delta 方向但更远），会把整个背景误判成被遮。
        if (dot(delta, viewDir) <= kDepthBias)
            continue;
        // 连续权重 = 距离衰减 × 半球衰减。二值 1.0 累加在 12 采样下会让"命中 1 个"与
        // "命中 0 个"之间跳变、遮蔽边界呈阶梯；连续衰减把单次命中的贡献压成
        // (1−d/R)·hemi 的小数，梯度随之连续。
        // （实测：二值版的本场景 AO 场高频能量已低于底图自身——立面 0.80、灌木 0.64、
        //  地面 0.00，比值 = AO 场相邻像素差 / 基线灰度相邻像素差；加权后更平滑，
        //  故**不需要**额外的 AO 模糊 pass。）
        occlusion += (1.0 - dist / kRadius) * hemi;
    }
    occlusion /= float(kSampleCount);
    // 出口翻成可见度：遮蔽量 × 增益取 1−x（见文件头"语义是可见度不是遮蔽量"）。
    return 1.0 - saturate(occlusion * kGain);
}
