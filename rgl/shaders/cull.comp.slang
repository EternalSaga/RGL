// Phase 7b（批次 5）：GPU frustum culling compute
//
// 布局出处（本刀换轨）：三张 buffer 的**元素类型**在 `cull_tables.slang`（可 import 的契约单元），
// 经 `cull_tables_schema.lua` 反射成 `generated/cull_tables_types_generated.cppm` 给 C++ 侧用。
// 于是本文件不再出现任何字节字面量：步长在元素类型里、字段在字段名里，两侧不可能再各抄各的。
// 迁移前的形态 = 两个手抄步长常量（60 / 120）+ 五处裸字节偏移读写（R5 棘轮基线里那 5 条）。
// 🔴 本文件自此**零**字节地址 buffer、零裸偏移读写（判据 grep 见任务书：那三类符号在本文件
// 必须零命中）。这条就是那条棘轮的收账凭据（判据出处 = tools/checks/byte_offset_baseline.json）。
//
// 换到 `StructuredBuffer` 还买到一件比"反射友好"更硬的东西：**元素越界是设备可检的**（读到尾部
// 哨兵/越界由驱动判），而"任意字节偏移"那种形态不是——本项目实测「缺 barrier / 越界 VVL 会报」
// 这个指望不成立（scene_tables.slang 头注释记着 GPU-AV 在 descriptor heap 架构下同样抓不到，
// 所以这里也不吹它，只说它比裸偏移可检）。
//
// **数据分工（BIT-IDENTICAL 契约）**：序列的 execSlot/DrawArgs/meshCmd 基值由 CPU 每帧
// 写进 seq staging（mvp 同为 CPU 算、现进 scene data per-object 表——GPU 若自算 mvp，FMA 收缩序与 CPU 无位级契约，
// 叶缘光栅 ±1 LSB 不可控）；GPU 只覆盖可见性：meshCmdX（groupCount 或 0 = no-op）
// 与 count（CullCount.reserved 保留未用（旧 visible atomicAdd 无人清零已删，2026-09-15）|
// CullCount.total 每帧 thread0 写，DGC sequenceCountAddress 指向它；可见数 = CPU 读回序列 meshCmd 位统计）。
// DGC 执行路与读回重放路消费同一序列内容（CPU args + GPU 可见性），跨平台位级一致。
//
// 矩阵约定（无行/列主序歧义）：对象矩阵按"内存 chunk j = 数学列 j"处理（C++ 侧 glm
// 列主序 + MVP 生产契约已证——Phase 7b+ 后该值进 scene data per-object 表，原 DrawArgs.mvp），
// 世界球用列组合展开（scene.slang worldPos 同款）
// ——不依赖 Slang 矩阵构造函数 / operator[] 语义。
//
// 三张表（元素布局见 cull_tables.slang，逐字段偏移由 tests/test_cull_tables_layout.cppm 钉住）：
//   cullTable (binding 0)：`StructuredBuffer<CullEntry>` × N（启动期一次性上传，host 写）
//   sceneData → **binding 23 `StructuredBuffer<ObjectMatrix>`**（批 T1 类型化迁出字节读）：
//     区基址（旧 @256）由描述符子范围承载、步长由元素类型承载，本文件零字节字面量。
//     [objectSlot] = spin·world，render_list 每帧写全量——被剔对象不留陈值
//   sequences (binding 2)：`RWStructuredBuffer<CullSequence>`，per-candidate **固定槽**
//     （非紧凑——不可见序列 meshCmdX = 0 = no-op draw，顺序 = candidate 序 = CPU entity 迭代序
//     → 重放序与 CPU 完全一致，截图 BIT-IDENTICAL 前提；紧凑化是后续优化，会破坏顺序确定性）。
//     GPU 只写 meshCmdX，其余字段 = CPU 基值。
//   count (binding **4**)：`RWStructuredBuffer<CullCount>`，[0].total 由 thread0 每帧写
//     push.sequenceCount（DGC sequenceCountAddress 指向此处，非紧凑 = candidate 数；
//     可见数 = CPU 读回序列 meshCmd 位统计）
//     🔴 批 T1 起本 binding 从 3 挪到 4：本文件 `import scene;`（取 ObjectMatrix 类型）后，
//     【历史】scene.slang 的 `sceneData` 声明曾占着 binding 3，同占一个显式 binding = slangc
//     `warning[E39001] explicit binding overlap`（sceneData 未用到 → 不进 SPV，但警告
//     恒出声）。挪号只动本 compute 自己的私有布局（C++ 侧 gpu_cull_impl.cppm 同步）。
import scene;         // ObjectMatrix 类型（批 T1：逐对象表的结构体仍在 scene.slang = 反射契约）
import scene_tables; // readObjectMatrix（binding 23 StructuredBuffer<ObjectMatrix>）
import cull_tables;  // CullEntry / CullSequence / CullCount（三张表的元素契约）

[[vk::binding(0, 0)]]
readonly StructuredBuffer<CullEntry> cullTable;
[[vk::binding(2, 0)]]
RWStructuredBuffer<CullSequence> sequences;
[[vk::binding(4, 0)]]
RWStructuredBuffer<CullCount> countBuf;

struct CullPush
{
    // @0 视锥 6 面（left/right/bottom/top/near/far，归一化、法线朝内——CPU
    // extractFrustum 产物同源同值；mvp 不经 push——CPU 算进序列）
    float4 planes[6];
    // @96 candidate 数（DGC execute 的 sequence count 来源）
    uint sequenceCount;
};
[[vk::push_constant]] CullPush push;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    // 尾线程守卫（stream_fanout.comp.slang 同款）：dispatch 上取整 64，尾线程 i ≥
    // sequenceCount 会越界读写表/序列 buffer。🔴 换成 StructuredBuffer 之后这条**照旧必须**：
    // 元素越界从"任意字节偏移"降级成"越界访问"（可检多了），但可检 ≠ 合法——robustBufferAccess
    // 只保证"丢弃或改写绑定内存内值"，不可依赖
    if (i >= push.sequenceCount)
        return;
    CullEntry e = cullTable[i];
    float3 c = float3(e.centerX, e.centerY, e.centerZ);
    float r = e.radius;
    uint objectSlot = e.objectSlot;
    uint groupCount = e.groupCount;

    // 对象世界矩阵 M（binding 23 类型化读，批 T1——旧「scene data @256 + slot×64，4 列显式
    // Load4」的字节形态已删：区基址在描述符子范围里、步长在元素类型里）
    ObjectMatrix m = readObjectMatrix(objectSlot);
    float4 m0 = m.c0;
    float4 m1 = m.c1;
    float4 m2 = m.c2;
    float4 m3 = m.c3;

    // 世界球（CPU 同式：中心 = M·(c,1) 列组合，半径 = r × max(三列长度)）
    float3 wc = (m0 * c.x + m1 * c.y + m2 * c.z + m3).xyz;
    float scale = max(length(m0.xyz), max(length(m1.xyz), length(m2.xyz)));
    float wr = r * scale;

    // 6 面测试（保守：球心到任平面距离 < −r 才剔——跨界/含内含均判可见；
    // 与 CPU sphereInFrustum 同式同值，对账基准一致）
    bool visible = true;
    for (uint p = 0u; p < 6u; ++p)
    {
        float4 pl = push.planes[p];
        if (dot(pl.xyz, wc) + pl.w < -wr)
        {
            visible = false;
            break;
        }
    }

    // GPU 只覆盖可见性（序列其余字段 = CPU 基值）：meshCmdX
    sequences[i].meshCmdX = visible ? groupCount : 0u;
    if (tid.x == 0u)
        countBuf[0].total = push.sequenceCount;
}
