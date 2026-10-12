import model;
import material;  // 统一 push constant（Phase 4：DrawArgs 超集；Phase 7b+ push 仅 objectIndex，
                  // totalTriangles/baseColor 等经 scene data ObjectDraw 表读取）
import scene;     // Phase 7b+：readObjectMvp（binding 25 对象 MVP 表，mvp 从 heap 读不走 push）
import scene_tables;  // 批 T1：逐对象四张表 StructuredBuffer 读取面（binding 23/25/26/33）

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
    const uint kVertexStride = 48;  // Phase 8 方案 A：+tangent vec4 @32（model_schema.lua 同步）

    uint groupIndex = groupID.x;
    uint firstTriangle = groupIndex * kTrianglesPerGroup;
    const ObjectDraw od = readObjectDraw(drawArgs.objectIndex);  // Phase 7b+：几何字段进 heap
    // 条件式前置：max(uint, 0) 救不回无符号回绕——firstTriangle > totalTriangles 时
    // 减法先回绕成巨大值，triangleCount = 10 → 越界读字节（静默垃圾而非空组）。
    // 当前 CPU 侧 groupCount = ceil(total/10) 恒正确不可触发，但 groupCount 一旦算错
    // （资产改动 / Phase 7 indirect 路径）症状难查（docs/archive/教程拆解发现_20260902.md ②）
    uint remaining = (firstTriangle >= od.totalTriangles) ? 0u
                 : od.totalTriangles - firstTriangle;
    uint triangleCount = min(kTrianglesPerGroup, remaining);
    uint vertexCount = triangleCount * 3;

    SetMeshOutputCounts(vertexCount, triangleCount);

    if (ti < vertexCount) {
        uint vertexIndex = firstTriangle * 3 + ti;
        // 拼接顶点 buffer（Phase 4 批次 2）：本 primitive 在拼接 buffer 内的字节偏移
        //（ObjectDraw.vertexOffset，Phase 7b+ 走 scene data 表；值源 = CPU 端 renderListSystem
        // 经资产表 MeshAsset.vertexOffset 写入）
        uint byteOffset = od.vertexOffset + vertexIndex * kVertexStride;
        float3 pos = asfloat(modelVertexBuffer.Load3(byteOffset));         // @0
        float3 nrm = asfloat(modelVertexBuffer.Load3(byteOffset + 12));   // @12
        float2 uv  = asfloat(modelVertexBuffer.Load2(byteOffset + 24));  // @24
        float4 tan = asfloat(modelVertexBuffer.Load4(byteOffset + 32));  // @32（方案 A）
        verts[ti].position = clipPos(readObjectMvp(drawArgs.objectIndex), pos);
        verts[ti].normal = nrm;
        verts[ti].uv = uv;
        verts[ti].tangentOS = tan;
        verts[ti].positionOS = pos;  // Phase 5 光照（object→world 变换在 frag 做，unlit 不消费）
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
