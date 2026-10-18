// Model 契约（Phase 3：baseColor × baseColorTexture 采样 + position/normal/uv）
//
// 顶点缓冲是平铺字节流，字段偏移/stride 以 model_schema.lua 的 vertexLayouts
// 声明为准（ModelVertex: position@0 normal@12 uv@24 tangent@32 stride=48，packed 布局）。
// 下方 ModelVertex 结构体仅作文档用途——实际读取用显式字节偏移
//（见 model.mesh.slang），不依赖 Slang struct 的内存布局
//（Slang float3 按 16B 对齐，与 CPU 侧 packed 布局不一致）。

struct ModelVertex
{
    float3 position;
    float3 normal;
    float2 uv;
    float4 tangent;   // Phase 8 方案 A：切线 xyz + 手性 w（loader 推，见 schema 注释）
};

struct ModelVertexOutput
{
    float4 position : SV_Position;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD0;
    float3 positionOS : TEXCOORD1;  // object-space 位置（Phase 5 光照：frag 经对象矩阵表
                                    // 变换到世界空间求视向 V，见 scene.slang；unlit 不消费）
    float4 tangentOS : TANGENT;     // Phase 8 方案 A：object-space 切线 xyz + 手性 w（对象→
                                    // 世界与 normal 同路在 frag 做，standard TBN 消费见
                                    // normalmap.slang applyNormalMapTBN）
};

// push constant 契约已移至 material.slang（Phase 4 统一 DrawArgs 超集，所有风格共享；
// Phase 7b+ 起 push 只剩 objectIndex，model 风格经它读 scene data 表取
// mvp/totalTriangles/vertexOffset/baseColor，roughnessMetallic 同表由
// model_pbr 消费）——本契约只拥有 buffer slot 段 +
// ModelVertex 顶点布局（schema 单一真相源）+ kTrianglesPerGroup 常量；
// model.mesh.slang / model.frag.slang 同时 import model + material。

[[vk::binding(0, 0)]]
ByteAddressBuffer modelVertexBuffer;

// 纹理采样（Phase 3）：Texture2D<float4> + SamplerState（SPIR-V OpTypeImage + OpTypeSampler 对）。
// 元素类型必须 float4（RGBA 纹理）：标量 Texture2D<float> 的 Sample 按 Slang 语义返回标量
//（SPIR-V 抽首分量），albedo 变 (R,R,R) 广播——2026-08-27 三驱动交叉测试复盘，
// 见 docs/archive/heap_texture_research.md §九.2。
// binding 1 → descriptor heap 的 SAMPLED_IMAGE 描述符槽（vkWriteResourceDescriptorsEXT 写入）；
// binding 2 → 内嵌 sampler（pEmbeddedSampler 替代 sampler heap 查找，spike 机制 spec 对
// OpTypeSampler 同等适用，见 docs/archive/heap_texture_research.md §二）。
// 无纹理/无 uv 的 primitive 绑白 1×1 纹理（纹理首槽 = 白，ensureWhiteTexture 带契约断言，shader 路径单一）。
[[vk::binding(1, 0)]]
Texture2D<float4> baseColorTexture;

[[vk::binding(2, 0)]]
SamplerState baseColorSampler;

// 贴图扩展（Phase 8 批次 ③a）：normal + ORM 各占一个 SAMPLED_IMAGE 堆槽（运行时
// allocSlot，同 baseColorTexture；sampler 复用 baseColorSampler binding 2，不新增
// sampler binding）。不 import 本文件的风格（cube/light 等；skybox 已随刀②.2 删除）
// 不声明 = 无影响；import 但不消费的风格（原 model.frag unlit 等，同刀删除）Slang 剔除未用全局 = SPV 无此描述符，
// 烘焙的 mapping 空转（既证无害，同 binding 4/5 对不 import env 的风格）。
// normal 贴图：DirectX 约定（Y-up，README "Normal (DirectX)"）+ 可能 BC5 压缩无 Z——
// flip-G 与 Z 重建在 normalmap.slang（decodeNormalDx）处理，本处只声明采样源；
// 无 normal 贴图的 primitive 绑中性法线纹理 (0.5,0.5,1,1)（flip-G 解码不动点，
// texture_pool::ensureNeutralNormalTexture），shader 路径单一。
[[vk::binding(6, 0)]]
Texture2D<float4> normalTexture;

// ORM 约定（glTF metallicRoughnessTexture = R=Occlusion / G=Roughness / B=Metalness，
// bistro README 石锤）：shader 消费 G=roughness B=metallic，R（AO）采样不应用（留后）；
// 无 ORM 贴图的 primitive 绑白纹理 orm=(1,1,1,1) → metallic/roughness 回落材质 factor。
[[vk::binding(7, 0)]]
Texture2D<float4> ormTexture;

// Phase 8 ③：emissive 贴图（binding 8，复用 baseColorSampler，allocSlot 分配，白纹理回落；无 emissive 材质 factor=0 → 该项恒黑）
[[vk::binding(8, 0)]]
Texture2D<float4> emissiveTexture;
