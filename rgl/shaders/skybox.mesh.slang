// 天空盒 mesh shader（Phase 5.5 批次 4）：单 group 输出一个覆盖全屏的大三角
//
// NDC 顶点 (-1,-1)/(3,-1)/(-1,3)（clip 空间 z=1=far 平面，Vulkan 深度范围 [0,1]），
// 不读任何 buffer——管线 binding 0/1 烘焙为占位映射（shader 不声明 = 空转无副作用，
// Phase 3/5 既证行为）。groupCount=1（MeshAsset 形态外的独立 draw，见 renderListSystem）。

import sky;

[shader("mesh")]
[numthreads(3, 1, 1)]
[outputtopology("triangle")]
void main(
    out vertices SkyVertexOutput verts[3],
    out indices uint3 triangles[1])
{
    SetMeshOutputCounts(3, 1);
    triangles[0] = uint3(0, 1, 2);

    float2 ndc[3] = { float2(-1.0, -1.0), float2(3.0, -1.0), float2(-1.0, 3.0) };
    for (uint i = 0; i < 3; ++i)
    {
        verts[i].position = float4(ndc[i], 1.0, 1.0);  // z=1 far，w=1
        verts[i].ndc = ndc[i];
    }
}
