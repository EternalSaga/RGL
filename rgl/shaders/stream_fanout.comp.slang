// Phase 7b+（spike）：静态 shader DGC 的 per-entry 序列流扇出 kernel
// （docs/archive/radv_static_dgc_spike.md §3.3）。
//
// 布局出处（本刀换轨）：候选表与序列行的元素类型来自 `cull_tables.slang`（可 import 的契约单元，
// 与 cull.comp.slang **同一份**），经反射同时喂给 C++ 侧。本文件不再出现步长常量与字节偏移：
// 迁移前的两个手抄步长常量（60 / 120）与一段按字节区间拷贝、三处按字节偏移写零
// （R5 棘轮基线里那 5 条）整片消失。🔴 本文件自此零字节地址 buffer、零裸偏移读写。
//
// 职责：把基础序列（固定槽、candidate 序，cull kernel 已写可见性）展开为 N 条
// per-管线条目"满槽孔洞流"：
//   stream[j][i] = candidate i ∈ 条目 j 且可见 → 拷基础行的 drawArgs 槽区 + meshCmd 三量
//                  否则 → meshCmd 三量写零（x=0 = NV/RADV 均原生 no-op）
// 条目内顺序 = candidate 序（确定性——原子分桶会破坏，spike 不采用）；
// sequenceCountAddress 全条目复用 count 的 CullCount.total（= candidate 数）。
// 🔴 行的 @0..8（execSlot 两个 u32）**刻意不写**：静态路 layout 无 EXECUTION_SET token，
// 那两个下标在本路上没有消费者；写它等于把未初始化字节搬进 indirect 载体。这也是本 kernel
// 不整体赋值 `streams[..] = row` 的唯一原因——整体赋值会连那 8 字节一起写。
// dispatch = (ceil(candidates/64), entryCount)；仅静态 DGC 模式 dispatch
// （流 buffer 未启用 = 本 shader 不创建，NV/重放路零开销）。
import cull_tables;  // CullEntry / CullSequence（元素契约，与 cull.comp.slang 同源）

[[vk::binding(0, 0)]]
readonly StructuredBuffer<CullEntry> cullTable;
[[vk::binding(1, 0)]]
StructuredBuffer<CullSequence> sequences;
[[vk::binding(2, 0)]]
RWStructuredBuffer<CullSequence> streams;

struct FanoutPush
{
    uint sequenceCount;  // candidate 数（grid.x 上取整 64，尾线程守卫）
};
[[vk::push_constant]] FanoutPush push;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    uint j = tid.y;
    // 尾线程守卫：dispatch 上取整 64，尾线程 i ≥ sequenceCount 会越界读写表/流 buffer
    if (i >= push.sequenceCount)
        return;

    uint streamIndex = j * push.sequenceCount + i;
    uint pipelineIndex = cullTable[i].pipelineIndex;
    CullSequence row = sequences[i];

    if (pipelineIndex == j && row.meshCmdX != 0u)
    {
        // 逐字段拷（不是整行赋值，理由见文件头 🔴）：DrawArgs 槽区（含保留宽度）+ meshCmd 三量
        streams[streamIndex].drawArgs = row.drawArgs;
        streams[streamIndex].drawArgsSlack = row.drawArgsSlack;
        streams[streamIndex].meshCmdX = row.meshCmdX;
        streams[streamIndex].meshCmdY = row.meshCmdY;
        streams[streamIndex].meshCmdZ = row.meshCmdZ;
    }
    else
    {
        // 孔洞：只把 meshCmd 三量清零 = 该槽是 no-op draw，行内其余字节本路无消费者
        streams[streamIndex].meshCmdX = 0u;
        streams[streamIndex].meshCmdY = 0u;
        streams[streamIndex].meshCmdZ = 0u;
    }
}
