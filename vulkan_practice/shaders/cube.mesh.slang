import types;

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    out vertices VertexOutput verts[8],
    out indices uint3 triangles[12])
{
    SetMeshOutputCounts(8, 12);

    Vertex* vertexBuf = Ptr<Vertex>(drawArgs.heapAddress + drawArgs.vertexOffset);
    uint* indexBuf = Ptr<uint>(drawArgs.heapAddress + drawArgs.indexOffset);

    if (ti < 8) {
        Vertex v = vertexBuf[ti];
        verts[ti].position = mul(drawArgs.mvp, float4(v.position, 1.0));
        verts[ti].color = v.color;
    }

    if (ti < 12) {
        uint baseIdx = ti * 3;
        triangles[ti] = uint3(indexBuf[baseIdx], indexBuf[baseIdx+1], indexBuf[baseIdx+2]);
    }
}
