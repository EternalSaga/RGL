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

struct PushConstants
{
    float4x4 mvp;
    uint64_t vertexBufferAddress;
    uint64_t indexBufferAddress;
};

[[vk::push_constant]] PushConstants push;
