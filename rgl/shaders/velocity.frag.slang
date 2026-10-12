// Phase 14 批 14.4：逐像素速度场（motion vectors）—— 输出「本像素表面对应点在上一帧的
// 像素位移」，口径 = **渲染分辨率像素 · prev − cur · y 向下**（= `docs/FSR输入契约.md`
// 实机对拍钉死的 FSR 输入口径；`motionVectorScale` 是**分子**，所以像素单位的速度在 CPU
// 侧直接填 (1,1)，不要再除分辨率）。
//
// 为什么要有这张纹理：FSR 3.1 upscaler 把 `motionVectors` 当**硬要求**（它不接受"你在
// shader 内部自己算"），而本项目此前只有 `taaMotion = prevSpin·inverse(curSpin)` 一条
// 全局矩阵。债-35 的另一半（per-entity 动画）同样以此为前置。
//
// ── 与 CPU 侧的关系：本文件是 `rgl/velocity_field.cppm` 的 GPU 镜像 ──
// 三步与 `pixelVelocity` 逐式相同（① uv+深度反投影 ② 非天空才施加自旋补偿 ③ 投到上一帧），
// 末尾再走 `toBaseSpace` 那一步。CPU 那份有 10 组变异验红的单测（含独立推导的旋转光流
// 闭式），本文件**刻意不复述推导**——两份推导必然漂移成两个版本（本项目反复踩过）。
// 改这里的任何一行，先问"CPU 那份要不要同步改"。
//
// ── 为什么落点是全屏 pass 而不是 compute pass ──
// tonemap / ao / taa 三个 pass 全是「全屏 draw 读纹理 → 写 RGBA16F offscreen RT」这个形状
// （`pass_vocabulary` 的 `fullscreenPass` 档 + `light.mesh` 全屏大三角）。走它就不需要给
// `RgPassKind` 新增 compute kind、不依赖批 14.4b 的图外布局护栏、也不碰闲置的 computeQueue。
// 勘察全文 → `docs/phase14_plan.md` §1.10。
//
// ── 消费的 scene data 槽位**全部既有**，本 shader 不新增任何契约字段 ──
//   @64  skyInvViewProj mat4 = inverse(proj·view)（**抖动后**，与深度缓冲配套）
//   @723456 prevViewProj mat4（**抖动后**）
//   @723520 motion mat4 = prevSpin·inverse(curSpin)（spin 关 = 单位阵）
//   @723584 zw = 本帧内容位移 c_t 像素
//   @723600 xy = 上一帧内容位移 c_{t−1} 像素
// ⚠ zw / xy 这一对由 `render_list_impl.cppm` 受**同一个** `taa.base_space` 门控写 0
//   （:263 与 :285 同门）⟹ 永远成对。门关时两者皆 0 ⟹ 本 shader 自动退化为**抖动空间**
//   速度（自洽：重建与投影都在抖动系），不是"半修正"的错误形态。
//
// ⚠ 与 `taa.reprojection` 消融门的连带：该门关时 CPU 把 prevViewProj 换成**本帧**矩阵、
//   motion 换单位阵 ⟹ 本 shader 输出恒 0（"完全不重投影"的一致语义）。这是消融门应有的
//   行为，不是缺陷，但拿速度场做判据时要记得这条。
//
// 天空像素（深度 ≥ 0.9999）：**不施加自旋补偿**但仍做相机重投影（与 taa.frag:533 同判据）
// —— 天空在无穷远，相机旋转时它在屏幕上确实会动，FSR 需要这个速度。

import scene;  // 头区 UBO（binding 22）+ 共享的 worldFromDepth / mulM / uvFromClip（批 14.4 抬升）

[[vk::binding(4, 0)]] Texture2D<float> depthTex;  // eD32Sfloat 深度图（eDepth aspect）

struct VelocityInput
{
    float4 position : SV_Position;  // framebuffer 坐标（深度 Load 用）
    float2 uv       : TEXCOORD0;    // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

[shader("fragment")]
float4 main(VelocityInput input) : SV_Target
{
    // 一个纹素的 uv 尺寸。**不查纹理尺寸**（ao.frag 同款免分辨率技巧：1:1 对齐的插值器 uv
    // 满足 fwidth(uv) = (1/width, 1/height)），因此 `/texel` 就是「uv 差 → 渲染分辨率像素差」，
    // 且 renderScale ≠ 1 时自动取**渲染**网格而非后缓冲网格（批 14.3 起两者可不等；
    // 写成窗口尺寸会让 0.5 档速度错 2 倍）。
    const float2 texel = fwidth(input.uv);

    const float2 cCur = readTaaParams().zw;          // 本帧内容位移 c_t（像素）
    const float2 cPrev = readTaaPrevContentShift().xy;  // 上一帧内容位移 c_{t−1}（像素）

    // ⚠ 重建用 baseUv 不用 input.uv —— 与 taa.frag:520 的批 13.5 裁决**同一条**：抖动投影下
    //   「基位置 β 的世界点」出现在像素 β + c_t，而深度按 input.position 取是本像素的表面
    //   深度（对的），所以重建喂 baseUv 才解出「基位置 = input.uv」的那个世界点。
    const float2 baseUv = input.uv + cCur * texel;
    // ⚠ 显式 int2：`input.position.xy` 是 float2，隐式转 int2 会出 slang warning[E30081]
    //   （这条警告在 taa.frag 潜伏过：增量构建不重编文件就一直不报）。
    const float depth = depthTex.Load(int3(int2(input.position.xy), 0)).r;

    const float3 world = worldFromDepth(baseUv, depth);
    float4 worldPrev = float4(world, 1.0);
    if (depth < 0.9999)  // 天空不随场景自旋（motion 是场景旋转补偿）；阈值同 ao.frag
        worldPrev = mulM(readTaaMotion(), worldPrev);
    const float4 prevClip = mulM(readTaaPrevViewProj(), worldPrev);

    // 上一帧相机背后：「搬到哪里」没有定义。写**零速度**（调用方策略，与 CPU 侧
    // `PixelVelocity::valid == false` 对应——CPU 层保证无效时不吐数，GPU 层的对应形态
    // 就是让 FSR 把它当静止像素，宁可少一次历史混合也不要搬错方向）。
    // .z = 有效标志（1/0）：FSR 只读 .xy，这一位纯给 llm_debug / RPC 回读留的可视化。
    if (prevClip.w <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    const float2 prevUv = uvFromClip(prevClip);
    // 抖动空间速度（= CPU 侧 `pixelVelocity(uv = baseUv)` 的逐位对应式）
    const float2 velocityJitteredPx = (prevUv - baseUv) / texel;
    // → 基空间（= CPU 侧 `toBaseSpace`）：静止表面点在抖动网格里的位移是 c_{t−1} − c_t
    //   的**纯抖动伪影**（与运动无关、量级 ±1 px，与真实亚像素运动同量级），加回
    //   c_t − c_{t−1} 才归零。漏掉这一步不会让画面崩，只会让静止画面被判定"在动"而发糊。
    const float2 velocityBasePx = velocityJitteredPx + (cCur - cPrev);

    if (any(abs(velocityBasePx) > 1.0e6))  // 非有限/爆量兜底（与 CPU 侧 isfinite 防护同职责）
        return float4(0.0, 0.0, 0.0, 0.0);
    return float4(velocityBasePx, 1.0, 0.0);
}
