// cull_tables.slang — GPU culling 三张 buffer 的**元素契约**（可 import 的契约单元）。
//
// 【为什么单独一个模块，而不是留在两个入口单元里】
// `cull.comp.slang` 与 `stream_fanout.comp.slang` 共用同一份候选表 / 序列行布局。此前两侧各
// 手抄一份 `kEntryStride = 60u` / `kSequenceStride = 120u` 与裸字节偏移，C++ 侧再抄第三份
// （`gpu_cull.cppm` 的 `CullEntry` + `kCullSeq*` 四常量 + 三条 `static_assert`）——机器只比
// C++ 单边，shader 侧改布局静默。本模块把布局收成**一处声明 + 一条反射管线**
// （`cull_tables_schema.lua` → `generated/cull_tables_types_generated.cppm`），两侧都从它取。
//
// 【为什么必须是可 import 的文件而不是入口单元】（形态先例 = `scene_tables.slang`）
// slang 的模块名 = 文件名去 `.slang`，而 `import` 把 `.` 当路径分隔符（`import cull.comp` 去找
// `cull/comp.slang`）⟹ 入口单元导不进去，住在入口单元里的类型对 `tools/check_spirv_stride.py`
// 的 R4 探针**结构性不可达**（ArrayStride == sizeof 那条不变量对它空真）。放进本文件 ⟹ R4 才
// 真的开始守这三个类型。
//
// 【为什么这里只有 struct、没有 `StructuredBuffer` 声明】
// 两个消费者对同一份布局用了**不同的 binding 号**（cull: sequences=2 / count=4；fanout:
// sequences=1 / streams=2），声明一旦共享就撞号（slangc `warning[E39001]`）。故本模块只拥有
// 元素类型，`StructuredBuffer<T>` 仍各自落在入口单元自己的 binding 上。
//
// 【布局红线：逐字节等于迁移前的手抄形态】
// 本文件替换的是 `ByteAddressBuffer` + 字面量偏移，**不是**布局本身——`sequences` 同时喂 DGC
// execute 与 CPU 读回重放，步长/偏移动一个字节就改画面。期望值（60 / 0,12,16,24 与
// 120 / 0,4,8,108）由 `tests/test_cull_tables_layout.cppm` 独立钉住（判据不抄生成物）。
//
// 【🔴 中心为什么写成三个 float 而不是 `float3`】
// 本仓全量带 `-fvk-use-scalar-layout`，`float3` 在该口径下的对齐/步长是**可**凑出 12B 的，但
// 它是"编译器口径"而不是"契约声明"——一旦 SLANG_FLAGS 掉 flag（R4 的常驻检测项之一），整张
// 候选表就从 60B 漂成 64B 步长，而构建全绿、VVL 沉默。显式三个标量把 12B 写进声明本身，
// 掉 flag 时反射会直接在 schema 交叉校验处 [FAIL]，而不是静默换布局。

/// 静态候选表条目（`cullTable` 的元素，60B，启动期一次性上传、每帧不变）。
/// 字段序 = 迁移前 `gpu_cull.cppm::CullEntry` 的字段序，逐字段偏移见 schema。
struct CullEntry
{
    // @0..12 node-local 剔除球心（与 CullSphere 组件同源）。🔴 见文件头【中心】。
    float centerX;
    float centerY;
    float centerZ;
    float radius;        // @12
    uint  objectSlot;    // @16 scene data 对象矩阵表下标 = MeshRef.objectSlot（实例级）
    uint  pipelineIndex; // @20 管线表下标 = DGC execution set 槽 / 重放管线表下标
    uint  groupCount;    // @24 mesh task group 数 = MeshAsset.groupCount
    uint  totalTriangles;// @28 DrawArgs 透传字段（MeshAsset 同源；本刀只保布局，shader 未读）
    uint  vertexOffset;  // @32 同上
    float4 baseColor;    // @36 MaterialAsset 同源
    float2 roughnessMetallic;  // @52 x=roughness
};

/// 输出序列行（`sequences` / `streams` 的元素，120B）。
/// 固定槽、candidate 序（非紧凑）——不可见序列 meshCmdX = 0 = no-op draw，顺序确定性是
/// 截图 BIT-IDENTICAL 的前提。CPU 写 execSlot/drawArgs/meshCmd 基值，GPU 只覆盖 meshCmdX。
struct CullSequence
{
    // DGC EXECUTION_SET token 按 shaderStages 位 LSB→MSB 读本处 2 个连续 u32：
    // 第 1 位 = FRAGMENT（shaderStages 的 LSB）、第 2 位 = MESH。值 = 2×pipelineIndex / +1。
    uint execSlotFrag;  // @0
    uint execSlotMesh;  // @4
    // 🔴 本字段在 shader 侧的**真身**是反射生成物 `RGL::DrawArgs`（material.slang 契约，4B =
    // 仅 objectIndex）。这里刻意退化成 `uint` 而不是嵌套：schema 的嵌套契约名合法判据 =
    // "出现在本 schema 的 vertexLayouts 名单里"（schema_loader.cppm::vertexAttrCppType），
    // 把 `DrawArgs` 列进本表就等于**第二次生成** `RGL::DrawArgs`——与 material_types_generated
    // 撞同名导出（先例 = tests/test_compute_push_layout.cppm 变异红线 ③：两个模块导出同名
    // struct 时 clang 22 直接编译失败）。宽度与对齐逐字节相同，故退化不改布局；C++ 侧写入
    // 仍按 `RGL::DrawArgs` 发字节（gpu_cull_impl.cppm::writeSequenceStaging）。
    uint drawArgs;      // @8
    // @12..108 = DrawArgs 槽区**保留宽度**（96B）。它不是垃圾，是"宽度已定、内容未定"的契约位：
    // 迁移前 `@8..108 共 100B` 的保留段就是它，删掉 = 序列步长从 120 变 24 = 改画面。
    uint drawArgsSlack[24];
    // @108..120 = VkDrawMeshTasksIndirectCommandEXT 12B（CPU 写基值 x=groupCount y=z=1，
    // GPU 只覆盖 x；x=0 = NV/RADV 均原生 no-op）。
    uint meshCmdX;  // @108
    uint meshCmdY;  // @112
    uint meshCmdZ;  // @116
};

/// 序列计数区（`countBuf` 的元素，8B；buffer 本体 64B，余量是历史留白，见 kCullCountBytes）。
/// @0 保留未用（旧 visible atomicAdd 无人清零已删，2026-09-15）；@4 = 每帧 thread0 写的
/// candidate 数（DGC sequenceCountAddress 指向此处，非紧凑语义）。
struct CullCount
{
    uint reserved;  // @0
    uint total;     // @4
};
