import model;

[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    // Phase 3：baseColor × 纹理采样（sRGB 格式硬件线性化；alpha 忽略，透明是 Phase 5）。
    // 分离绑定（Slang Texture2D<float4> + SamplerState = OpTypeImage + OpTypeSampler 对）：
    // 2026-08-27 三驱动交叉测试验证 PASS（组合/分离、sampler heap/内嵌均支持，
    // 见 docs/heap_texture_research.md §九；早期"RADV 只认组合"定性已推翻）。
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    return float4(modelDrawArgs.baseColor.rgb * texel.rgb, 1.0);
}
