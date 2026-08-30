import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集，model 风格消费 mvp/totalTriangles/baseColor）

// 无 task stage：一次 mesh 调用 = 一个 group，group 序号 = SV_GroupID.x
//（CPU 端 drawMeshTasksEXT(groupCount, 1, 1) 派生），每 group 处理
// kTrianglesPerGroup 个三角形。
//
// 契约常量（schema 为 source of truth，改动只改 model_schema.lua 并重新生成）：
//   kTrianglesPerGroup ↔ model_schema.lua 的 constants（C++ 侧消费生成常量
//     model_types_generated::kTrianglesPerGroup，group 数派生；本文件字面量必须一致）
//   kVertexStride / 字段偏移 ↔ model_schema.lua 的 vertexLayouts（ModelVertex，packed 32B）
// 顶点数据是平铺字节流，按显式偏移读取（不用 Slang struct 布局）。

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    out vertices ModelVertexOutput verts[32],
    out indices uint3 triangles[10])
{
    const uint kTrianglesPerGroup = 10;
    const uint kVertexStride = 32;

    uint groupIndex = groupID.x;
    uint firstTriangle = groupIndex * kTrianglesPerGroup;
    uint remaining = max(drawArgs.totalTriangles - firstTriangle, 0u);
    uint triangleCount = min(kTrianglesPerGroup, remaining);
    uint vertexCount = triangleCount * 3;

    SetMeshOutputCounts(vertexCount, triangleCount);

    if (ti < vertexCount) {
        uint vertexIndex = firstTriangle * 3 + ti;
        // 拼接顶点 buffer（Phase 4 批次 2）：本 primitive 在拼接 buffer 内的字节偏移
        //（DrawArgs.vertexOffset，CPU 端 renderListSystem 经资产表 MeshAsset.vertexOffset 写入）
        uint byteOffset = drawArgs.vertexOffset + vertexIndex * kVertexStride;
        float3 pos = asfloat(modelVertexBuffer.Load3(byteOffset));         // @0
        float3 nrm = asfloat(modelVertexBuffer.Load3(byteOffset + 12));   // @12
        float2 uv  = asfloat(modelVertexBuffer.Load2(byteOffset + 24));  // @24
        verts[ti].position = mul(drawArgs.mvp, float4(pos, 1.0));
        verts[ti].normal = nrm;
        verts[ti].uv = uv;
    }

    // 去索引展开：三角形 i 的 3 个连续顶点 = i*3..i*3+2，索引自然连续。
    // 注意：out indices 引用的是本组 verts 数组下标（0..vertexCount-1），不是全局顶点索引。
    // verts[ti] 存的已是全局顶点 firstTriangle*3+ti，故本组第 ti 个三角形 = verts[ti*3..ti*3+2]。
    // （旧写法 (firstTriangle+ti)*3 是全局索引，越界；RADV mod 回绕 30 恰好掩盖，NVIDIA 暴露崩形）
    if (ti < triangleCount) {
        uint base = ti * 3;
        triangles[ti] = uint3(base, base + 1, base + 2);
    }
}
