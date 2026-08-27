// spike 对照实验：无 sampler 路径（纯 Load fetch，不触发 OpSampledImage 组合）。
// 验证 Slang SPIR-V 下 b0（OpTypeImage 描述符）与 b2（OpTypeBuffer 描述符）
// 的 resource heap 映射本身是否正确——若 Load 正确而 Sample 错误，
// 则问题定位在 OpImage+OpSampler -> OpSampledImage 的驱动组合路径，而非 image 映射。
// 元素类型 float4（RGBA8 四通道）：标量 Texture2D<float> 的 Load 按 Slang 语义
// 返回标量（SPIR-V 抽首分量），隐式转 float4 时 R 通道广播——spike 首版因此
// 三驱动一致输出 (R+0.1)×4，误判为驱动问题（2026-08-27 交叉测试复盘）。
struct SpikeVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]]
Texture2D<float4> spikeTex;

[[vk::binding(2, 0)]]
ByteAddressBuffer spikeData;

[shader("fragment")]
float4 main(SpikeVertex input) : SV_Target
{
    // uv.x = px_x/256：屏幕左半（px<128）取固定左半纹素 (64,64)，右半取 (192,128)。
    // Load 坐标是 int3（Slang 设计：第 3 分量 = mip level，对齐 OpImageFetch (coord, Lod)）
    float4 texel;
    if (input.uv.x < 0.5f)
        texel = spikeTex.Load(int3(64, 64, 0));
    else
        texel = spikeTex.Load(int3(192, 128, 0));
    return texel + float4(asfloat(spikeData.Load(0)));
}
