import model;

[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    // Phase 3：baseColor × 纹理采样（sRGB 格式硬件线性化；alpha 忽略，透明是 Phase 5）。
    // 分离绑定（Slang Texture2D<float> + SamplerState = OpTypeImage + OpTypeSampler 对）：
    // RADV descriptor heap 实测只映射组合 OpTypeSampledImage，分离对采样恒 garbage
    //（纹理数据回读已验证正确，根因见 docs/heap_texture_research.md §八）——NVIDIA 待测。
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    return float4(modelDrawArgs.baseColor.rgb * texel.rgb, 1.0);
}
