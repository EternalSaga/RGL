// evssm_moments.comp.slang —— 批 11.6d-1：EVSM 矩派生（C 方案：从 D32 阴影图，
// shadow pass 零改动；docs/evssm_plan.md §4.1）。正交光深度线性 → 矩 = 深度纯函数。
//
// 矩组 = Filament computeDepthMomentsVSM 同款：z = exp(c·(2d−1))，
// moments = (z, z², −1/z, 1/z²)。c 经 push.exponent 由 host kEvssmExponent 下发
// （唯一权威源；定值 5.5 由 fp16 上限 exp(2c)≤65504 双约束决定，Filament PCSS 同值）
// → z∈[0.0041, 244.7]、z²≤59874 ≤ fp16 max，逐分量天然不溢出。
// 空 texel（深度=1.0 far clear）→ 恰为 far 面最大矩——Filament「clear=最大矩」
// 语义由派生天然成立，无需特判 clear 路径。
//
// tile 布局：输出与阴影图集同构（同位置同 tile），零重排；per-tile 独立在降采样
// shader 保证，本 pass 位置映射恒等。

struct EvssmMomentsPush
{
    uint mapSize;   // 图集整幅边长（2048）
    uint cols;      // 级联分块列数（2）——本 pass 仅校验用，位置映射恒等
    float exponent; // c（5.5）
};

[[vk::push_constant]] EvssmMomentsPush push;

[[vk::binding(0)]] Texture2D<float> shadowDepth;   // 阴影图集 eDepth aspect（RO，PCF 同槽）
[[vk::binding(1)]] RWTexture2D<float4> moments;    // RGBA16F mip0 view（GENERAL）

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= push.mapSize || id.y >= push.mapSize)
        return;
    const float d = shadowDepth.Load(int3(id.xy, 0));
    const float z = exp(push.exponent * (d * 2.0 - 1.0));
    moments[id.xy] = float4(z, z * z, -1.0 / z, 1.0 / (z * z));
}
