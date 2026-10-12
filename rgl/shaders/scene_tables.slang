// scene_tables.slang — 逐对象四张表的**类型化读取面**（scene data 类型化迁移 批 T1）。
//
// 【为什么单独一个模块，而不是并进 scene.slang】
// scene.slang 是反射管线（`rgl_reflect_slang(shaders/scene.slang shaders/scene_forward_schema.lua …)`）
// 的输入单元，且被 `cull.comp.slang` / `meshlet_cull.comp.slang` 两个 compute **整份 import 过**
// ——它们自带 binding 3 的独立小 buffer，与 scene.slang 的 `sceneData` 显式撞号（见两文件头注释）。
// 把 `StructuredBuffer` 声明放进 scene.slang 等于把这条撞号重新埋回去；把三个结构体搬出 scene.slang
// 又动反射输入。故本模块只做**消费者侧**：结构体仍留在 scene.slang（反射契约零改动），本模块
// `import scene;` 取用它们，四张表以 `StructuredBuffer<T>` 落在各自独立 binding。
//
// 【类型化买到什么】
//  - 元素步长由类型系统给（`buf[id]` 自动 ×stride），C++ 侧 `sizeof` 与 shader 侧元素步长在
//    编译期对账——改结构体加字段忘改 C++ 写入 stride 是**编译错误**，不是静默读错行；
//  - 越界访问落在 buffer 之外，可被 GPU-AV 的 buffer 越界检查覆盖（`ByteAddressBuffer`
//    的任意偏移 Load 对它完全透明）；
//  - R5 的裸字节偏移计数在这四处归零（`sceneData.Load*(基址 + id * stride + 字段偏移)` 整式消失）。
//
// 【哨兵契约（批 T0）】
// 每区尾部多留 1 个元素（id == capacity == 4096）作 poison 哨兵，C++ 侧 `writeSceneSentinels`
// 在 staging 分配时填一次、随每帧整区 DtoD 复制进设备 buffer。本模块的读取函数**不做边界钳制**
// ——钳制会把越界重新藏起来；越界照抄哨兵，画面出现 NaN 即"索引越界"的显式形态。
// 机器守卫 = `tests/test_scene_sentinel.cppm`（含"哨兵被删则测试必须变红"的变异红线声明）。
import scene;

// 逐对象世界矩阵表（旧 sceneData 字节 256..262400，stride 64 × 4096 + 1 哨兵）
[[vk::binding(23, 0)]] StructuredBuffer<ObjectMatrix> objectMatrixTable;
// 逐对象 MVP 表（旧 sceneData 字节 263424..525568，stride 64 × 4096 + 1 哨兵）
// 与世界矩阵表**同容量、同 id 域**（都按 objectIndex 索引），但 MVP 是 CPU 每帧算好写进来的：
// GPU float4x4 的 FMA 收缩序与 glm 无位级契约，故保留 CPU 现算（AGENTS「GPU culling 现行」）。
[[vk::binding(25, 0)]] StructuredBuffer<ObjectMatrix> objectMvpTable;
// 逐对象绘制属性表（旧 sceneData 字节 525824..722432，stride 48 × 4096 + 1 哨兵）
[[vk::binding(26, 0)]] StructuredBuffer<ObjectDraw> objectDrawTable;
// 逐对象簇区间表（旧 sceneData 字节 724992..790528，stride 16 × 4096 + 1 哨兵）
[[vk::binding(33, 0)]] StructuredBuffer<ObjectClusterRange> objectClusterRangeTable;

// 以下四个读取函数**沿用批 T1 之前的名字与语义**，消费端只需多一句 `import scene_tables;`。
// 函数体不再出现任何字节字面量：基址由 binding 承载，步长由元素类型承载。

/// 按实例 id 读世界矩阵（列主序 4 列）。id 合法域 [0, 4096)；越界读到 poison 哨兵。
ObjectMatrix readObjectMatrix(uint objectIndex)
{
    return objectMatrixTable[objectIndex];
}

/// 按实例 id 读 CPU 侧算好的 MVP（列主序 4 列，消费方用 clipPos 列组合）。
ObjectMatrix readObjectMvp(uint objectIndex)
{
    return objectMvpTable[objectIndex];
}

/// 按实例 id 读绘制属性（材质参数 + 自发光 + alphaCutoff）。
ObjectDraw readObjectDraw(uint objectIndex)
{
    return objectDrawTable[objectIndex];
}

/// 按实例 id 读簇区间条目（meshlet 路径）。
ObjectClusterRange readObjectClusterRange(uint objectIndex)
{
    return objectClusterRangeTable[objectIndex];
}
