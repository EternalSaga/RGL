import types;

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    out vertices VertexOutput verts[8],
    out indices uint3 triangles[12])
{
    uint totalVertices   = 8;
    uint totalPrimitives = 12;

    SetMeshOutputCounts(totalVertices, totalPrimitives);

    // 【核心改变】通过 .Get() 或者是 Slang 内部的实例化方法，从句柄中解出真正的 Buffer
    // 此时 Slang 后端会立刻生成 spvDescriptorHeapEXT 寻址指令
    StructuredBuffer<Vertex> vertices = push.verticesHandle;
    StructuredBuffer<uint>   indices  = push.indicesHandle;

    if (ti < totalVertices)
    {
        Vertex v = vertices[ti]; // 优雅地通过大堆直接索引
        verts[ti].position = mul(push.mvp, float4(v.position, 1.0));
        verts[ti].color    = v.color;
    }

    if (ti < totalPrimitives)
    {
        uint baseIdx = ti * 3;
        triangles[ti] = uint3(
            indices[baseIdx + 0],
            indices[baseIdx + 1],
            indices[baseIdx + 2]
        );
    }
}