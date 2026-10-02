// evssm_downsample.comp.slang —— 批 11.6d-1：EVSM 矩 mip 链降采样（1 级 1 dispatch，
// dispatch z = 级联 tile；docs/evssm_plan.md §4.1 C 方案）。
//
// 自定义 2×2 box（Filament vsmMipmap 同款，texelFetch 语义禁硬件 bilinear——
// 矩对插值精度极敏感）。**per-tile 独立过滤**：tile 边界在 mip 各层级恒对齐
// （tile 边长 = 1024>>level，2 的幂），box 读域永不跨块 = 零级联互染（11.6b 结论，
// 这也是免 sampler2DArray 的前提）。
//
// 层级依赖 = dispatch 间 barrier（宿主记录）；本 shader 只读 src mip、写 dst mip。

struct EvssmDownPush
{
    uint level;          // 目标 mip L（src = L−1）
    uint cols;           // tile 列数（2）
    uint baseTileTexels; // mip0 的 tile 边长（1024）
};

[[vk::push_constant]] EvssmDownPush push;

[[vk::binding(0)]] RWTexture2D<float4> mipSrc;  // mip L−1 view（GENERAL）
[[vk::binding(1)]] RWTexture2D<float4> mipDst;  // mip L view（GENERAL）

[numthreads(8, 8, 1)]
void main(uint3 g : SV_DispatchThreadID)
{
    const uint tile = g.z;
    const uint tx = tile % push.cols;
    const uint ty = tile / push.cols;
    const uint srcTile = push.baseTileTexels >> (push.level - 1u);
    const uint dstTile = push.baseTileTexels >> push.level;
    if (g.x >= dstTile || g.y >= dstTile)
        return;
    const uint2 src = uint2(tx, ty) * srcTile + g.xy * 2u;
    const float4 m0 = mipSrc[src];
    const float4 m1 = mipSrc[src + uint2(1u, 0u)];
    const float4 m2 = mipSrc[src + uint2(0u, 1u)];
    const float4 m3 = mipSrc[src + uint2(1u, 1u)];
    mipDst[uint2(tx, ty) * dstTile + g.xy] = (m0 + m1 + m2 + m3) * 0.25;
}
