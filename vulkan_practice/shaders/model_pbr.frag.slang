import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）

// Phase 4 占位：baseColor 已接线进 DrawArgs；roughnessMetallic 契约字段虽在，但 CPU 侧
// 写占位 0（mat.roughness/metallic 接线留 Phase 5 真实光照）——输出与 model.frag 完全一致
//（unlit 风格走 model 管线，pbr 风格走本管线）。
[shader("fragment")]
float4 main(ModelVertexOutput input) : SV_Target
{
    float4 texel = baseColorTexture.Sample(baseColorSampler, input.uv);
    return float4(drawArgs.baseColor.rgb * texel.rgb, 1.0);
}
