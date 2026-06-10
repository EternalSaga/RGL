// ==========================================
// 1. 基础结构体定义 (干净的 C++ 风格)
// ==========================================
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
    Vertex* vertices; // 真正的 BDA 顶点指针
    uint* indices;  // 真正的 BDA 索引指针
};

// 完美的零 Descriptor 绑定，直接挂载到 Vulkan Push Constant
[vk::push_constant]
PushConstants push;