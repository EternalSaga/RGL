// ==========================================
// 2. Mesh Shader 阶段 (协作式拉取架构)
// ==========================================
import types;

[shader("mesh")]
[numthreads(32, 1, 1)]
void main( // 改成 main，slangc 瞎子都能认出来
    uint ti : SV_GroupIndex,
    out vertices VertexOutput verts[8],
    out indices uint3 triangles[12])
{
    uint totalVertices   = 8;
    uint totalPrimitives = 12;

    SetMeshOutputCounts(totalVertices, totalPrimitives);

    if (ti < totalVertices)
    {
        Vertex v = push.vertices[ti]; 
        verts[ti].position = mul(push.mvp, float4(v.position, 1.0));
        verts[ti].color    = v.color;
    }

    if (ti < totalPrimitives)
    {
        uint baseIdx = ti * 3;
        triangles[ti] = uint3(
            push.indices[baseIdx + 0],
            push.indices[baseIdx + 1],
            push.indices[baseIdx + 2]
        );
    }
}