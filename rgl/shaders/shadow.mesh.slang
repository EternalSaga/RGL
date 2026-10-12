// Phase 11 批 11.2：级联阴影 depth-only mesh stage（形状同 model.mesh.slang，唯一差别 =
// 变换矩阵换成光空间 VP）。
//
// **为什么独立文件而不复用 model.mesh.slang + 宏**：model.mesh.slang 的输出结构
// ModelVertexOutput 带 5 个字段（normal/uv/tangentOS/positionOS），depth-only pass 用不到，
// 而 mesh stage 的输出会被 frag stage 的入口签名约束——留 5 个无人消费的 varying 只增加
// 寄存器与插值开销。独立文件让阴影 mesh 只输出 position，frag 也就只收一个 SV_Position。
//
// 变换链：**两段矩阵相乘在 CPU 侧已经做完**（shadowViewProj = lightProj·lightView，且
// lightView 之前先乘了对象的 spin·world？——否）。这里读两条表：
//   clip = clipPos(readShadowViewProj(cascade), worldPos(readObjectMatrix(objectIndex), pos))
// 逐段用列组合（scene.slang clipPos / worldPos），与 model.mesh.slang 的
// clipPos(readObjectMvp(...), pos) 同一套约定。**为什么不把 world 也预乘进 VP**：
// VP 区是每帧 4 槽定长（kSceneShadowCascadeCount），装不下 per-object 的 N 个矩阵；
// 而 world 在对象矩阵表 @128 已有现成的一份，GPU 端两次列组合的成本远低于扩表。
//
// 契约常量与 model.mesh.slang 同源（同一份拼接顶点流、同一份 ObjectDraw 表）：
//   kTrianglesPerGroup = 10 / kVertexStride = 48（model_schema.lua 单一真相源）
//   ObjectDraw 表基址 525824（scene.slang readObjectDraw）
//
// push constant：**不 import material.slang**，自带 ShadowDrawArgs（objectIndex + cascadeIndex，
// 8B）。material.slang 的 drawArgs 是 4B 单字段且被全部既有管线共享，阴影要多带一个
// cascadeIndex 又不能改那份共享契约（改了 = 1591 条模型条目全部重建）——故本管线自带
// push 块。它仍然只承载"纯索引"，符合 AGENTS 的 bindless 原则。

import model;   // modelVertexBuffer（binding 0）+ ModelVertex 布局契约
import scene;   // sceneData（binding 3）+ readObjectDraw / readObjectMatrix / readShadowViewProj

struct ShadowDrawArgs
{
    uint objectIndex;  // scene data 各 per-object 表下标 = MeshRef.objectSlot（同 DrawArgs）
    uint cascadeIndex; // 阴影图分块号（0..kSceneShadowCascadeCount-1），选 readShadowViewProj 槽
};

[[vk::push_constant]] ShadowDrawArgs shadowDrawArgs;

struct ShadowVertexOutput
{
    float4 position : SV_Position;
};

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    out vertices ShadowVertexOutput verts[32],
    out indices uint3 triangles[10])
{
    const uint kTrianglesPerGroup = 10;
    const uint kVertexStride = 48;  // model_schema.lua vertexLayouts 单一真相源

    const uint firstTriangle = groupID.x * kTrianglesPerGroup;
    const ObjectDraw od = readObjectDraw(shadowDrawArgs.objectIndex);
    // 条件式前置（与 model.mesh.slang 同款）：无符号回绕救不回来，先判后减。
    const uint remaining = (firstTriangle >= od.totalTriangles) ? 0u
                        : od.totalTriangles - firstTriangle;
    const uint triangleCount = min(kTrianglesPerGroup, remaining);
    const uint vertexCount = triangleCount * 3;

    SetMeshOutputCounts(vertexCount, triangleCount);

    if (ti < vertexCount)
    {
        const uint vertexIndex = firstTriangle * 3 + ti;
        const uint byteOffset = od.vertexOffset + vertexIndex * kVertexStride;
        const float3 pos = asfloat(modelVertexBuffer.Load3(byteOffset));  // @0（stride 内只取位置）
        const float3 world = worldPos(readObjectMatrix(shadowDrawArgs.objectIndex), pos);
        verts[ti].position = clipPos(readShadowViewProj(shadowDrawArgs.cascadeIndex), world);
    }

    if (ti < triangleCount)
    {
        const uint base = ti * 3;
        triangles[ti] = uint3(base, base + 1, base + 2);
    }
}
