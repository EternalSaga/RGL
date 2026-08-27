// Model 契约（Phase 3：baseColor × baseColorTexture 采样 + position/normal/uv）
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

// 纹理采样（Phase 3）：Texture2D<float4> + SamplerState（SPIR-V OpTypeImage + OpTypeSampler 对）。
// 元素类型必须 float4（RGBA 纹理）：标量 Texture2D<float> 的 Sample 按 Slang 语义返回标量
//（SPIR-V 抽首分量），albedo 变 (R,R,R) 广播——2026-08-27 三驱动交叉测试复盘，
// 见 docs/heap_texture_research.md §九.2。
// binding 1 → descriptor heap 的 SAMPLED_IMAGE 描述符槽（vkWriteResourceDescriptorsEXT 写入）；
// binding 2 → 内嵌 sampler（pEmbeddedSampler 替代 sampler heap 查找，spike 机制 spec 对
// OpTypeSampler 同等适用，见 docs/heap_texture_research.md §二）。
// 无纹理/无 uv 的 primitive 绑白 1×1 纹理（TexturePool 固定槽，shader 路径单一）。
[[vk::binding(1, 0)]]
Texture2D<float4> baseColorTexture;

[[vk::binding(2, 0)]]
SamplerState baseColorSampler;
