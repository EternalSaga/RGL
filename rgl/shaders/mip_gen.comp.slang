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
// **降采样公式（单 tap LINEAR = 4-tap box）**：dst 像素 (x,y) 对应 src 的 2×2 texel 块
// (2x,2y)/(2x+1,2y)/(2x,2y+1)/(2x+1,2y+1)，块中心 texel 坐标 = (2x+0.5, 2y+0.5)，归一化
// uv = ((x+0.25)/dstW, (y+0.25)/dstH)。硬件双线性在该点恰好取 2×2 texel 各 25%
//（CLAMP_TO_EDGE 越界钳制为标准边缘近似）——一次 SampleLevel(srcMip) 即精确 box，
// 无需显式 4 tap。
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
    // 2×2 块中心采样点（见文件头公式）：单 tap LINEAR = 精确 box
    float2 uv = (float2(gid) + 0.25f) / float2(push.dstWidth, push.dstHeight);
    dstTex.Store(gid, srcTex.SampleLevel(linearSampler, uv, (float)push.srcMip));
}
