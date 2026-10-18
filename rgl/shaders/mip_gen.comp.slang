// Phase 8 批次 ③b：2D 纹理 compute mip 链生成（启动期一次性工具，与 bindless 渲染路径
// 隔离——传统 descriptor set + 经典 compute pipeline，不 bindResourceHeapEXT；C++ 侧
// mip_gen.cppm / MipGen::recordChain，契约单一真相源 = MipGenPush（12B，static_assert 同源））
//
// **仅 UNORM 纹理走本 compute 链**（normal/ORM 等 linear 数据）：R8G8B8A8_SRGB 在 RADV
// 无 STORAGE_IMAGE 能力（vkGetPhysicalDeviceImageFormatProperties2 = FORMAT_NOT_SUPPORTED，
// sRGB 图不能带 eStorage usage）→ sRGB 纹理（baseColor 等）的 mip 链走 LINEAR blit 链
//（recordMipBlitChain2d，同上传 CB，字节保真 + 双线性降采样）。计划文档"先 LINEAR blit
// 过渡"即此路径。
//
// **降采样公式（单 tap LINEAR = 4-tap box）**：dst 像素 (x,y) 覆盖 src 的 2×2 texel 块
// (2x,2y)/(2x+1,2y)/(2x,2y+1)/(2x+1,2y+1)。该块在 src **边坐标**下跨 [2x, 2x+2)，中心
// = 2x+1，即 texel 2x 与 2x+1 的**分界线**（不是任何纹素的中心），归一化
// uv = (2x+1)/srcW = **((x+0.5)/dstW, (y+0.5)/dstH)**。硬件双线性在该点恰好取 2×2 texel
// 各 25%——一次 SampleLevel(srcMip) 即精确 box，无需显式 4 tap。
// 🔴 **偏移量是 0.5，不是 0.25**（2026-10-18 债-137 定案）。原式 `(x+0.25)/dstW` 落到 src 边
// 坐标 2x+0.5 = **texel 2x 的正中心** ⟹ 双线性权重 1/0/0/0 = **点抽样** ⟹ 整条链零滤波
//（每级都是上一级的隔行抽取），缩小面逐像素读到相距 2^N 的孤立纹素 ⟹ 平面墙/广场白散斑
//（正是本模块立项要消除的东西，却因这个常数从未生效）。sRGB 走 blit 链不受影响 ⟹ 症状是
//「albedo 干净、normal 碎」，极易误诊成材质/光照问题（确实被误诊过一次 → normalmap.slang
// 末尾 2026-10-05 那两轮 specular-AA 实验）。判据：砖墙窗口成片亮点 86 → 2，法线相邻夹角
// p95 24.3° → 7.9°，而后者与**独立盒平均参考实现**算出的 ≈8° 对上（不是"调糊了"）。
//
// **布局契约（MipGenPush，12B scalar 布局；C++ mip_gen.cppm 同名 struct static_assert
// 12B，任一侧漂移编译失败——CullPush 先例）**：
//   @0  srcMip     u32 源 mip 级（被降采样；= dst 级 − 1）
//   @4  dstWidth   u32 目标 mip 宽（= max(1, srcW>>1)，C++/shader 同式）
//   @8  dstHeight  u32 目标 mip 高（= max(1, srcH>>1)）
[[vk::binding(0, 0)]]
Texture2D<float4> srcTex;
[[vk::binding(1, 0)]]
RWTexture2D<float4> dstTex;
[[vk::binding(2, 0)]]
SamplerState linearSampler;

struct MipGenPush
{
    uint srcMip;
    uint dstWidth;
    uint dstHeight;
};
[[vk::push_constant]] MipGenPush push;

// 每线程一个 dst texel（8×8 组）。越界线程（dst 尺寸非 8 倍数）直接返回。
[numthreads(8, 8, 1)]
void main(uint2 gid : SV_DispatchThreadID)
{
    if ((gid.x >= push.dstWidth) || (gid.y >= push.dstHeight))
    {
        return;
    }
    // 2×2 块中心采样点（见文件头公式）：单 tap LINEAR = 精确 box。0.5 = 块中心落在 texel
    // 分界线上；写成 0.25 会退化成点抽样（债-137）
    float2 uv = (float2(gid) + 0.5f) / float2(push.dstWidth, push.dstHeight);
    dstTex.Store(gid, srcTex.SampleLevel(linearSampler, uv, (float)push.srcMip));
}
