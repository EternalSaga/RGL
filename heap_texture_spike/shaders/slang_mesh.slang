// spike Slang mesh shader：全屏三角形 + uv（与 mesh.glsl 同构）。
// 用于分离绑定测试（OpTypeImage + OpTypeSampler 对，主工程同款 shader 形态）。
struct SpikeVertex
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void main(
    out vertices SpikeVertex verts[3],
    out indices uint3 triangles[1])
{
    // 与 mesh.glsl 相同的 clip 空间全屏三角形 + uv（采样端 CLAMP_TO_EDGE，uv 可越界）
    verts[0].position = float4(-1.0, -1.0, 0.0, 1.0);
    verts[1].position = float4( 3.0, -1.0, 0.0, 1.0);
    verts[2].position = float4(-1.0,  3.0, 0.0, 1.0);
    verts[0].uv = float2(0.0, 0.0);
    verts[1].uv = float2(2.0, 0.0);
    verts[2].uv = float2(0.0, 2.0);
    triangles[0] = uint3(0, 1, 2);
    SetMeshOutputCounts(3, 1);
}
