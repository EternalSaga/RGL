// 统一 push constant 契约（Phase 4：所有 shader 风格共享同一 DrawArgs 布局）
//
// 单一真相源：本文件的 struct 字段/顺序是 shader 端布局契约；
// material_schema.lua 的 pushConstants 段声明同名字段（validator 对反射 JSON 校验）；
// 生成物 material_types_generated.cppm 的 DrawArgs 经 static_assert 钉死 4B
//（scalar 布局，slangc -fvk-use-scalar-layout）。
//
// Phase 7b+（NV DGC 规避，最终形态）：DrawArgs 只剩 objectIndex（4B）。原 mvp /
// totalTriangles / vertexOffset / baseColor / roughnessMetallic 全部迁入 scene data
// per-object 表（scene.slang readObjectMvp / readObjectDraw，binding 3 按 objectIndex 读）。
// 原因：NV 驱动 DGC PUSH token 在"push 字节数 × 序列数"较大时于命令生成阶段挂死
//（Xid 109 CTX SWITCH TIMEOUT，实测 100B→~112 序列崩、36B→~1200 崩、4B→1591 全量不崩）；
// 压到 4B 彻底绕开。所有值仍是 CPU 同一份（render_list 算好写表），shader 不做矩阵乘法，
// 位级与旧 push 路径一致。
//
// 各风格只消费自己需要的字段（未读字段无害）：
//   全部 — objectIndex（唯一 push 字段，索引 scene data 各表）
//
// Phase 5/7b bindless 原则：push constant 冻结在"纯索引"级别——场景级/每帧变化的数据
//（光照、对象矩阵、MVP、材质）一律进 descriptor heap（scene.slang / ensureSceneData）。
//
// 契约文件独立反射（不 import 本文件的契约：types.slang / model.slang 只声明
// buffer slot / 顶点布局段，避免 push constant 参数重复进多份反射 JSON）；
// mesh shader 同时 import 本文件与自身契约（cube.mesh.slang = import types + import material）。

struct DrawArgs
{
    uint objectIndex;       // scene buffer 各 per-object 表下标 = MeshRef.objectSlot
                            // （实例级槽，Phase 6 instancing；Phase 7b+ 唯一 push 字段，
                            //  经 readObjectMvp/readObjectDraw 索引其余全部数据）
};

[[vk::push_constant]] DrawArgs drawArgs;
