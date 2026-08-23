// Model 契约（Phase 2：flat unlit baseColor + position/normal/uv）
//
// 顶点缓冲是平铺字节流，字段偏移/stride 以 model_schema.lua 的 vertexLayouts
// 声明为准（ModelVertex: position@0 normal@12 uv@24 stride=32，packed 布局）。
// 下方 ModelVertex 结构体仅作文档用途——实际读取用显式字节偏移
//（见 model.mesh.slang），不依赖 Slang struct 的内存布局
//（Slang float3 按 16B 对齐，与 CPU 侧 packed 布局不一致）。

struct ModelVertex
{
    float3 position;
    float3 normal;
    float2 uv;
};

struct ModelVertexOutput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
};

struct ModelDrawArgs
{
    float4x4 mvp;
    uint totalTriangles;
    float4 baseColor;
};

[[vk::push_constant]] ModelDrawArgs modelDrawArgs;

[[vk::binding(0, 0)]]
ByteAddressBuffer modelVertexBuffer;
