// ==========================================
// 3. Fragment Shader 阶段
// ==========================================
import types;
[shader("fragment")]
float4 main(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}