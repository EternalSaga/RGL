// izb_prime.comp.slang —— 批 11.3-1d：early-Z priming（引擎版；语义护栏 = tests/
// test_izb_create.cppm 判据⑤：primed[cell] = 链上 lightDepth 最大值，空 = 0）
//
// 生产用途（阶段 2）：光空间保守光栅 pass 的 frag 用 primed 做「整 cell 无一节点在
// 三角之前」的早退；1d 现阶段先建起来（读侧 = 1e 消费），行为零风险。
//
// 与 izb_create.comp.slang 的节点/head 布局契约同源（头注释）；字节偏移纪律同。

static const uint kNone = 0xFFFFFFFFu;
static const int kMaxSteps = 1 << 26;  // 坏链保险（链最长 = 像素数，正常永不触达）

struct IzbPrimePush {
    int gridDim;
    int pixelTotal;  // 步数上限的独立第二来源（判据在测试侧，这里只兜底防死循环）
    int _pad0;
    int _pad1;
};

[[vk::binding(0)]] ByteAddressBuffer head;
[[vk::binding(1)]] ByteAddressBuffer nodes;
[[vk::binding(3)]] RWByteAddressBuffer primed;

[[vk::push_constant]] IzbPrimePush push;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint cellCount = uint(push.gridDim) * uint(push.gridDim);
    const uint c = tid.x;
    if (c >= cellCount)
        return;

    uint h = head.Load(c * 4);
    float maxDepth = 0.0;
    int steps = 0;
    while (h != kNone && steps < kMaxSteps) {
        const float d = asfloat(nodes.Load(h * 16 + 8));
        maxDepth = max(maxDepth, d);
        h = nodes.Load(h * 16 + 12);
        ++steps;
    }
    primed.Store(c * 4, asuint(maxDepth));  // 空 cell = 0（「无节点可比」哨义）
}
