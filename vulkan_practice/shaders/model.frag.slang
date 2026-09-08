import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）

[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    // Phase 3：baseColor × 纹理采样（sRGB 格式硬件线性化）。
    // 分离绑定（Slang Texture2D<float4> + SamplerState = OpTypeImage + OpTypeSampler 对）：
    // 2026-08-27 三驱动交叉测试验证 PASS（组合/分离、sampler heap/内嵌均支持，
    // 见 docs/archive/heap_texture_research.md §九；早期"RADV 只认组合"定性已推翻）。
    // Phase 5：alpha = baseColor.a × texel.a——BLEND 材质走透明队列（混合 factors 在
    // applyDynamicState 接线）；白纹理 a=1，OPAQUE/unlit 输出不受影响。
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    return float4(drawArgs.baseColor.rgb * texel.rgb, drawArgs.baseColor.a * texel.a);
}
