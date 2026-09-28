// Phase 7b+（spike）：静态 shader DGC 的 per-entry 序列流扇出 kernel
// （docs/radv_static_dgc_spike.md §3.3）。独立契约（本头注释 + gpu_cull.cppm C++ 侧），
// 不接反射管线；字节偏移 = kCullSequenceStride 布局，cull.comp.slang 先例。
//
// 职责：把基础序列（固定槽、candidate 序，cull kernel 已写可见性）展开为 N 条
// per-管线条目"满槽孔洞流"：
//   stream[j][i] = candidate i ∈ 条目 j 且可见 → 拷基础行 @8..120（DrawArgs 槽区 + meshCmd）
//                  否则 → meshCmd @108..120 写零（x=0 = NV/RADV 均原生 no-op）
// 条目内顺序 = candidate 序（确定性——原子分桶会破坏，spike 不采用）；
// sequenceCountAddress 全条目复用 count buffer @4（= candidate 数）。
// dispatch = (ceil(candidates/64), entryCount)；仅静态 DGC 模式 dispatch
// （流 buffer 未启用 = 本 shader 不创建，NV/重放路零开销）。
//
// 数据布局（与 gpu_cull.cppm static_assert 同源）：
//   cullTable (binding 0)：CullEntry×N 60B，@20 pipelineIndex
//   sequences (binding 1)：基础序列 120B，@108 meshCmd.x（cull kernel 覆盖后
//                          = visible ? groupCount : 0——本 kernel 只读）
//   streams   (binding 2)：RW 流 N×candidates×120B，行布局同基础序列
//                          （@0..8 execSlot 字节不写——静态路 layout 无 EXECUTION_SET token）
[[vk::binding(0, 0)]]
readonly ByteAddressBuffer cullTable;
[[vk::binding(1, 0)]]
ByteAddressBuffer sequences;
[[vk::binding(2, 0)]]
RWByteAddressBuffer streams;

struct FanoutPush
{
    uint sequenceCount;  // candidate 数（grid.x 上取整 64，尾线程守卫）
};
[[vk::push_constant]] FanoutPush push;

static const uint kEntryStride = 60u;
static const uint kSequenceStride = 120u;

[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint i = tid.x;
    uint j = tid.y;
    if (i >= push.sequenceCount)
        return;

    uint streamBase = (j * push.sequenceCount + i) * kSequenceStride;
    uint pipelineIndex = cullTable.Load(i * kEntryStride + 20u);
    uint meshX = sequences.Load(i * kSequenceStride + 108u);

    if (pipelineIndex == j && meshX != 0u)
    {
        [unroll]
        for (uint o = 8u; o < 120u; o += 4u)
            streams.Store(streamBase + o, sequences.Load(i * kSequenceStride + o));
    }
    else
    {
        streams.Store(streamBase + 108u, 0u);
        streams.Store(streamBase + 112u, 0u);
        streams.Store(streamBase + 116u, 0u);
    }
}
