// izb_prime.comp.slang —— 批 11.3-1c：early-Z priming（FTIZB pass4 的 spike 版）
//
// 每个光空间 cell 走一遍 IZB 链表：**非空 = 最远节点深度（= 最大「离光距离」），
// 空 = 0**。生产里写进光空间深度缓冲当 priming 值，pass5 的遮挡三角形以 LESS 测早退：
// 空 texel 谁都进不来；非空 texel 挡在「最远受光点」之外的三角形整体被硬件 z-cull
// （论文收益 30–50%）。spike 写 r32 SSBO，测试逐 cell 断言 = max(链深)/0。
//
// params 布局与 izb_create.comp.slang 头注释同一份（勿双源）。步数上限 [12] 防坏链
// 死循环：真出环属插入侧 bug，测试侧另有独立环检测判据，不靠这里兜。

struct IzbNode {
    float depth;
    uint  next;
};

static const uint kNone = 0xFFFFFFFFu;

[[vk::binding(0)]] ByteAddressBuffer head;
[[vk::binding(1)]] StructuredBuffer<IzbNode> nodes;
[[vk::binding(3)]] RWByteAddressBuffer primed;             // 槽位表与 izb_create 同一套：2 = depthIn（本核不读）
[[vk::binding(4)]] ByteAddressBuffer params;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const int gridDim = params.Load(4);
    const int maxSteps = params.Load(12);

    const uint c = tid.x;
    if (c >= uint(gridDim * gridDim))
        return;

    float best = 0.0;
    uint h = head.Load(c * 4);
    int steps = 0;
    while (h != kNone && steps < maxSteps) {
        best = max(best, nodes[h].depth);
        h = nodes[h].next;
        ++steps;
    }
    primed.Store(c * 4, asuint(best));
}
