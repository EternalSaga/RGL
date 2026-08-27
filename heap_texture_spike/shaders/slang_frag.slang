// spike Slang fragment shader：分离绑定（OpTypeImage + OpTypeSampler 对，主工程同款形态）。
// 资源在全局作用域声明（SPIR-V 不支持入口点参数上的 binding 装饰，主工程同模式）：
//   binding 0: Texture2D<float4>（OpTypeImage）  -> resource heap SAMPLED_IMAGE 描述符槽
//   binding 1: SamplerState（OpTypeSampler）    -> sampler heap 槽 或 pEmbeddedSampler（SPIKE_SAMPLER 选择）
//   binding 2: ByteAddressBuffer（OpTypeBuffer）-> resource heap STORAGE_BUFFER 槽（pAddressRange）
// 元素类型必须是 float4（RGBA8 四通道）：标量 Texture2D<float> 的 Sample 按 Slang 语义
// 返回标量（SPIR-V 抽首分量），隐式转 float4 时 R 通道广播到全部 4 通道——spike 首版
// 因此三驱动一致输出 (R+0.1)×4，误判为驱动问题（2026-08-27 交叉测试复盘）。
struct SpikeVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[[vk::binding(0, 0)]]
Texture2D<float4> spikeTex;

[[vk::binding(1, 0)]]
SamplerState spikeSmp;

[[vk::binding(2, 0)]]
ByteAddressBuffer spikeData;

[shader("fragment")]
float4 main(SpikeVertex input) : SV_Target
{
    return spikeTex.Sample(spikeSmp, input.uv) + float4(asfloat(spikeData.Load(0)));
}
