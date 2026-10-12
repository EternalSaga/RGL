import types;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集）
import scene;     // Phase 7b+：readObjectMvp（binding 25 对象 MVP 表，mvp 从 heap 读不走 push）
import scene_tables;  // 批 T1：逐对象四张表 StructuredBuffer 读取面（binding 23/25/26/33）

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    out vertices VertexOutput verts[8],
    out indices uint3 triangles[12])
{
    SetMeshOutputCounts(8, 12);

    if (ti < 8) {
        uint vertexOffset = ti * 24;
        uint3 posRaw = vertexBuffer.Load3(vertexOffset);
        uint3 colRaw = vertexBuffer.Load3(vertexOffset + 12);
        float3 pos = asfloat(posRaw);
        float3 col = asfloat(colRaw);
        verts[ti].position = clipPos(readObjectMvp(drawArgs.objectIndex), pos);
        verts[ti].color = col;
    }

    if (ti < 12) {
        uint baseIdx = ti * 3;
        triangles[ti] = uint3(indexBuffer.Load(baseIdx * 4), indexBuffer.Load((baseIdx+1) * 4), indexBuffer.Load((baseIdx+2) * 4));
    }
}
