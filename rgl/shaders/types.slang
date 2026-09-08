struct Vertex
{
    float3 position;
    float3 color;
};

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color    : COLOR0;
};

// push constant 契约已移至 material.slang（Phase 4 统一 DrawArgs 超集，所有风格共享）——
// 本契约只拥有 buffer slot 段（vertex/index）；cube.mesh.slang 同时 import types + material。

[[vk::binding(0, 0)]]
ByteAddressBuffer vertexBuffer;

[[vk::binding(1, 0)]]
ByteAddressBuffer indexBuffer;
