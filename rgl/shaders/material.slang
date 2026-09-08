// 统一 push constant 契约（Phase 4：所有 shader 风格共享同一 DrawArgs 布局）
//
// 单一真相源：本文件的 struct 字段/顺序是 shader 端布局契约；
// material_schema.lua 的 pushConstants 段声明同名字段（validator 对反射 JSON 校验）；
// 生成物 material_types_generated.cppm 的 DrawArgs 经 static_assert 钉死 100B
//（scalar 布局，slangc -fvk-use-scalar-layout：全字段对齐 4，mvp@0 / totalTriangles@64 /
//  vertexOffset@68 / baseColor@72 / roughnessMetallic@88 / objectIndex@96 / 总尺寸 100，
//  零 padding——C++ glm 自然布局逐字段吻合，见 AGENTS.md 着色器编译节）。
//
// 各风格只消费自己需要的字段（未读字段无害）：
//   cube  — mvp（每顶点色着色，材质字段不消费）
//   model — mvp + totalTriangles + vertexOffset + baseColor（flat unlit × 纹理采样）
//   model_pbr — 同 model + roughnessMetallic + objectIndex（Phase 5 光照：objectIndex 索引
//     scene buffer 的 bindless 对象矩阵表，见 scene.slang）
//
// Phase 5 bindless 原则：push constant 冻结在"mvp + 索引"级别——场景级/每帧变化的数据
//（光照、对象矩阵）一律进 descriptor heap（scene.slang / Resources::ensureSceneData），
// 不再扩 DrawArgs。
//
// 契约文件独立反射（不 import 本文件的契约：types.slang / model.slang 只声明
// buffer slot / 顶点布局段，避免 push constant 参数重复进多份反射 JSON）；
// mesh shader 同时 import 本文件与自身契约（cube.mesh.slang = import types + import material）。

struct DrawArgs
{
    float4x4 mvp;
    uint totalTriangles;
    uint vertexOffset;      // 拼接顶点 buffer 内 per-primitive 字节偏移（Phase 4 批次 2 启用）
    float4 baseColor;
    float2 roughnessMetallic;  // x=roughness y=metallic
    uint objectIndex;       // scene buffer 对象矩阵表下标 = 资产 id（Phase 5 光照，bindless）
};

[[vk::push_constant]] DrawArgs drawArgs;
