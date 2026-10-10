// ssr.frag —— 屏幕空间反射（Phase 15 批 15.1）：**本文件当前是占位实现**。
//
// ── 占位声明（先读完这段再读代码）──
// 本批 fragment 体 = 一次 `Load` 整数纹素拷贝：输出 = `scene_hdr` 的原值，**零算术**。
// 它存在的唯一理由 = 让「门开（本 pass 在图里）= 门关（图里没有本 pass）逐位相同」成为
// **管道正确性**的判据：占位 shader 不做任何算法，画面若变了，只可能是管道接错（附件/
// 布局/描述符堆槽/消费者输入），而不可能是算法错——这一步把「管道」与「算法」两种失效
// 彻底分开。真正的 DDA ray march 在批 15.1 后半替换本函数体：**接口（binding 面 + 输入
// 结构）不动**，届时算法面 = McGuire-Mara（`docs/phase15_plan.md` §1.3），反射源 =
// `scene_hdr`，权重从 G-buffer 现成量算（Schlick fresnel 与 light.frag 同式、roughness
// 门控、背面/边界 fade）。
//
// ── 为什么取整数纹素用 `Load` 而**不许** `Sample` ──
// 即使 uv 落在纹素中心，过滤与 uv 舍入都是多余的风险面；`Load` 没有滤波器、没有 uv 数学，
// 是唯一能保证**逐位**等于输入的做法（velocity.frag:67 同款取法，含显式 int2 免
// slang warning[E30081] 那条坑）。
//
// ── 绑定面（与 CPU 侧 addSsrEntry 的 extra 映射成对；G-buffer 通道号镜像 light.frag）──
//   3 = sceneData（import scene；@722272 ssrViewProj = 本帧**抖动后** proj·view，
//       readSsrViewProj() 已备好，占位体不消费、ray march 用它把命中点投回屏幕 uv）
//   4 = scene_hdr（RGBA16F 线性 HDR，反射源）
//   5 = gbuffer_normal（RG16_SNORM oct 编码，解法同 light.frag）
//   6 = gbuffer_position（RGBA32_SFLOAT 世界坐标）
//   7 = gbuffer_metalrough（RGBA8_UNORM .r=metal .g=rough）
//   8 = depth（eD32Sfloat 深度图，eDepth aspect；Load 直取 ⟹ 无 sampler 绑定，
//       同 addVelocityEntry 的理由：不占 = 不参与 sampler 槽相撞的风险面）
// ⚠ 色输入恒为 scene_hdr（不是 taa_out/ao_applied）：decorateSsrPass 插在 velocity/taa/
//   ao_apply **之前**，它读的是 light 的产物；SSR 的产物才是它们的输入。
//
// ── 纹理空间口径 ──
// 本 pass **不改变纹理空间**（输出与输入同空间、同网格）：占位拷贝天然满足；15.1 的
// ray march 也在本帧抖动空间求交（深度缓冲/投影矩阵同为抖动系），不做 un-jitter——那是
// 下游时域滤波器的活（口径同 ao_apply.frag 纪律①，别在这里抢着搬）。

import scene;  // sceneData buffer（binding 3：@722272 ssrViewProj 尾区）

[[vk::binding(4, 0)]] Texture2D<float4> sceneHdrTex;      // 本帧成片（RGBA16F 线性 HDR）
[[vk::binding(5, 0)]] Texture2D<float2> normalTex;        // G-buffer oct 法线（rg16_snorm）
[[vk::binding(6, 0)]] Texture2D<float4> positionTex;      // G-buffer 世界坐标（rgba32_sfloat）
[[vk::binding(7, 0)]] Texture2D<float4> metalRoughTex;    // G-buffer .r=metal .g=rough
[[vk::binding(8, 0)]] Texture2D<float> depthTex;          // eD32Sfloat 深度图（eDepth aspect）

struct SsrInput
{
    float4 position : SV_Position;  // framebuffer 坐标（Load 用）
    float2 uv       : TEXCOORD0;    // light.mesh 产出 = framebuffer 对齐 uv（1:1）
};

[shader("fragment")]
float4 main(SsrInput input) : SV_Target
{
    // 占位实现 = 全部：整数纹素原值拷贝（红线判据「门开 = 门关逐位相同」的算术根据）。
    // 15.1 后半在此函数体内替换为 DDA ray march + 合成，binding 面与输入结构不动。
    return sceneHdrTex.Load(int3(int2(input.position.xy), 0));
}
