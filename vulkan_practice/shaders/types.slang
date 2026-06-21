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
    uint32_t vertexOffset;
    uint32_t indexOffset;
    uint64_t _padding;
    uint64_t heapAddress;
};

[[vk::push_constant]] DrawArgs drawArgs;
