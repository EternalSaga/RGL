// Phase 7 D1：light pass mesh shader —— 单 group 全屏大三角（仿 skybox.mesh.slang，
// 该文件已随刀②.2 删除）。覆盖 [0,1] uv 的屏幕，不读任何 buffer——管线 binding 0/1
// （textured=false 基础映射的 vertex/index）在 shader 不声明 = 空转无副作用
// （skybox 条目既证，同其一并删除，结论留在 git history）。groupCount=1。
// z=1（far）、w=1：light pass 无深度测试/写入（终图直写 swapchain），深度值不参与。

struct LightVertexOutput
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;  // 屏幕 [0,1] 纹理坐标（大三角插值后覆盖可视屏）
};

[shader("mesh")]
[numthreads(3, 1, 1)]
[outputtopology("triangle")]
void main(
    out vertices LightVertexOutput verts[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    triangles[0] = uint3(0, 1, 2);

    float2 ndc[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    for (uint i = 0; i < 3; ++i)
    {
        verts[i].position = float4(ndc[i], 1.0, 1.0);  // z=1 far，w=1
        // uv.y 取反：geometry 用负高 viewport（framebuffer Y 翻转）写入 GB，采样须按 framebuffer
        // 坐标对齐——uv.y = framebuffer_y/height = 0.5 - ndc.y*0.5（uv.x 符号本就对）
        verts[i].uv = float2(ndc[i].x * 0.5 + 0.5, 0.5 - ndc[i].y * 0.5);
    }
}
