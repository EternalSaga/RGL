// izb_mask.mesh.slang —— 批 11.3-1d 后半：光空间保守光栅 mesh stage（IZB mask pass）
//
// 形状 = shadow.mesh.slang 逐段镜像（同拼接顶点流 / 同 ObjectDraw 表 / 同光空间
// shadowViewProj——投影体集合与阴影段完全一致，Renderer 直接重放 shadow0 段）。
// 唯一增量：把三角形的**光空间 uv 三顶点**以平摊属性传给 frag（edge 判定用，
// spike 版是 params SSBO，引擎版走 flat 属性零额外 buffer）。
//
// 深度口径：position.z = 正交光投影的窗口 z ∈ [0,1] = 线性光深度（w=1 → 仿射插值
// 精确），与 izb_create.comp.slang 节点里的 lightDepth **同一矩阵同一单位**，
// frag 直接比较。
//
// push：与 frag 同声明 12B（本 stage 只消费前两字段，gridDim 忽略）——两 stage
// push 范围一致，规避 vkCmdPushDataEXT 跨 stage 范围歧义（shadow.mesh 的 8B
// ShadowDrawArgs 前两字段位级一致 = replay 原样转发 + 尾缀 gridDim）。

import model;   // modelVertexBuffer（binding 0）+ ModelVertex 布局契约
import scene;   // sceneData（binding 3）+ readObjectDraw / readObjectMatrix / readShadowViewProj
import scene_tables;  // 批 T1：逐对象四张表 StructuredBuffer 读取面（binding 23/25/26/33）

struct IzbMaskPush
{
    uint objectIndex;   // = ShadowDrawArgs.objectIndex（位级一致）
    uint cascadeIndex;  // = ShadowDrawArgs.cascadeIndex
    uint gridDim;       // 光网格 G（本 stage 忽略，frag 消费）
};

[[vk::push_constant]] IzbMaskPush push;

struct IzbMaskVertexOutput
{
    float4 position : SV_Position;          // 光 ndc（z = 线性光深度）
    nointerpolation float4 uvA;             // 本三角三顶点 (uv.xy, lightDepth.z)：
    nointerpolation float4 uvB;             //  平摊属性按 provoking 语义——mesh 顶点逐
    nointerpolation float4 uvC;             //  三角独立，三顶点各写同一份整三角数据
};

[shader("mesh")]
[numthreads(32, 1, 1)]
[outputtopology("triangle")]
void main(
    uint ti : SV_GroupIndex,
    uint3 groupID : SV_GroupID,
    out vertices IzbMaskVertexOutput verts[32],
    out indices uint3 triangles[10])
{
    const uint kTrianglesPerGroup = 10;
    const uint kVertexStride = 48;  // model_schema.lua vertexLayouts 单一真相源

    const uint firstTriangle = groupID.x * kTrianglesPerGroup;
    const ObjectDraw od = readObjectDraw(push.objectIndex);
    const uint remaining = (firstTriangle >= od.totalTriangles) ? 0u
                        : od.totalTriangles - firstTriangle;
    const uint triangleCount = min(kTrianglesPerGroup, remaining);
    const uint vertexCount = triangleCount * 3;

    SetMeshOutputCounts(vertexCount, triangleCount);

    if (ti < vertexCount)
    {
        const uint vertexIndex = firstTriangle * 3 + ti;
        const uint byteOffset = od.vertexOffset + vertexIndex * kVertexStride;
        const float3 pos = asfloat(modelVertexBuffer.Load3(byteOffset));  // @0
        const float3 world = worldPos(readObjectMatrix(push.objectIndex), pos);
        const float4 clip = clipPos(readShadowViewProj(push.cascadeIndex), world);
        verts[ti].position = clip;

        // 平摊三 uv+深度：本三角的三个顶点（j0 起）逐个变换——同 group 同三角，
        // 命中 L1；uv 式与 izb_create.comp.slang 一致（ndc.xy·0.5+0.5），z 直接带
        // 光 ndc z（正交 = 线性），frag 用它做节点位置的插值深度判据
        const uint j0 = firstTriangle * 3 + (ti / 3u) * 3u;
        float2 uv0 = float2(0, 0), uv1 = float2(0, 0), uv2 = float2(0, 0);
        float d0 = 0.0, d1 = 0.0, d2 = 0.0;
        for (uint j = 0u; j < 3u; ++j)
        {
            const uint o = od.vertexOffset + (j0 + j) * kVertexStride;
            const float3 p3 = asfloat(modelVertexBuffer.Load3(o));
            const float3 w3 = worldPos(readObjectMatrix(push.objectIndex), p3);
            const float4 c3 = clipPos(readShadowViewProj(push.cascadeIndex), w3);
            const float2 uvj = c3.xy * 0.5 + 0.5;
            if (j == 0u) { uv0 = uvj; d0 = c3.z; }
            else if (j == 1u) { uv1 = uvj; d1 = c3.z; }
            else { uv2 = uvj; d2 = c3.z; }
        }
        verts[ti].uvA = float4(uv0, d0, 0.0);
        verts[ti].uvB = float4(uv1, d1, 0.0);
        verts[ti].uvC = float4(uv2, d2, 0.0);
    }

    if (ti < triangleCount)
    {
        const uint base = ti * 3;
        triangles[ti] = uint3(base, base + 1, base + 2);
    }
}
