import types;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集，cube 只消费 mvp）

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
        verts[ti].position = mul(drawArgs.mvp, float4(pos, 1.0));
        verts[ti].color = col;
    }

    if (ti < 12) {
        uint baseIdx = ti * 3;
        triangles[ti] = uint3(indexBuffer.Load(baseIdx * 4), indexBuffer.Load((baseIdx+1) * 4), indexBuffer.Load((baseIdx+2) * 4));
    }
}
