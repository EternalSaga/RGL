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

struct DrawArgs
{
    float4x4 mvp;
};

[[vk::push_constant]] DrawArgs drawArgs;

[[vk::binding(0, 0)]]
StructuredBuffer<Vertex> vertexBuffer;

[[vk::binding(1, 0)]]
ByteAddressBuffer indexBuffer;
