
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
    StructuredBuffer<Vertex>.Handle verticesHandle; // 顶点缓冲在堆里的句柄
    StructuredBuffer<uint>.Handle   indicesHandle;  // 索引缓冲在堆里的句柄
};

[[vk::push_constant]] PushConstants push;