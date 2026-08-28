import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）

// Phase 4 占位：material 字段已接线（baseColor/roughnessMetallic 进 DrawArgs 超集），
// 光照 Phase 5——输出与 model.frag 完全一致（unlit 风格走 model 管线，pbr 风格走本管线）。
[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    return float4(drawArgs.baseColor.rgb * texel.rgb, 1.0);
}
