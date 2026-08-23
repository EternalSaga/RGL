import model;

[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    // Flat unlit：material 的 baseColor（alpha 忽略，透明是 Phase 5）
    return float4(modelDrawArgs.baseColor.rgb, 1.0);
}
