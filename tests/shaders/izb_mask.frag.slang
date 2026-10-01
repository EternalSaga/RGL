// izb_mask.frag.slang —— 批 11.3-1d：光空间 fragment × IZB 走链 → 可见性 mask
// （FTIZB pass5 核心循环的 spike：保守光栅 fragment 命中一个光空间 texel，走该
// texel 的 IZB 链表，对每个受光节点做 2D 棱柱判定 + 深度判定，置 mask 位）
//
// 判定语义（论文 §4 的点采样退化版，µQuad/LUT 亚像素细化留阶段 2）：
//   节点 p（光空间 uv_p、离光深度 d_p）被三角 t 遮 ⟺
//   ① uv_p 在 t 的 uv 投影三角形内（三边 edge function 同号，CCW 内 = 全 >0）
//   ② d_p > d_f（节点在三角之后；d_f = 该 fragment 的插值光深度——FTIZB 的
//     texel 粒度近似，生产按论文在节点里存 sub-texel 偏移再细化）
// 保守光栅的必要性在 golden 里成对钉死：sub-texel 条带（t4）不盖 cell 中心，
// 正常光栅 0 fragment（中心采样全盲），OVERESTIMATE 出 fragment 后**必须靠
// edge test 把不在棱柱内的节点拒掉**——coverage 粗、判定准。
//
// 偏移纪律（1c 血泪，两处 ByteAddressBuffer 都吃字节偏移）：
// head.Load(cell*4)、mask 原子(pixel*4)、pixelUV.Load2(p*8)。
//
// params2 布局 = izb_mask.mesh.slang 头注释（同一份，勿双源）。

struct IzbNode {
    float depth;
    uint  next;
};

// 与 izb_mask.mesh.slang 的 Vert 逐字段一致（mesh→frag 接口匹配靠声明序 + 修饰符）
struct Vert {
    float4 position : SV_Position;
    float depth;
    nointerpolation uint prim;
};

static const uint kNone = 0xFFFFFFFFu;
static const int kMaxSteps = 1024;  // 坏链保险（环检测判据在测试侧独立做）

[[vk::binding(0)]] ByteAddressBuffer head;
[[vk::binding(1)]] StructuredBuffer<IzbNode> nodes;
[[vk::binding(5)]] RWByteAddressBuffer mask;      // 按像素 ID 的 uint 位图（bit = 三角）
[[vk::binding(6)]] ByteAddressBuffer pixelUV;     // float2/像素：光空间 uv（生产打包进节点 payload）
[[vk::binding(7)]] ByteAddressBuffer params2;

float edge(in float2 a, in float2 b, in float2 p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

[shader("fragment")]
float4 main(in Vert v) : SV_Target
{
    // fb 行 = v·G（mesh 侧 NDC y = 1−2v + 管线负高度 viewport 的合成结果，1b 口径）
    const uint cx = uint(v.position.x);
    const uint cy = uint(v.position.y);
    const uint cell = cx + cy * uint(params2.Load(4));  // [4] = gridDim（[0] 是 triCount，勿混）

    uint h = head.Load(cell * 4);
    int steps = 0;
    while (h != kNone && steps < kMaxSteps) {
        const float dP = nodes[h].depth;
        if (dP > v.depth) {  // 深度先筛（便宜），再精确 2D 棱柱判定
            const float2 uvP = asfloat(pixelUV.Load2(h * 8));
            const int base = 16 + 64 * int(v.prim);
            const float2 v0 = asfloat(params2.Load2(base + 0));
            const float2 v1 = asfloat(params2.Load2(base + 8));
            const float2 v2 = asfloat(params2.Load2(base + 16));
            if (edge(v0, v1, uvP) > 0.0 && edge(v1, v2, uvP) > 0.0 && edge(v2, v0, uvP) > 0.0)
                mask.InterlockedOr(h * 4, 1u << v.prim);
        }
        h = nodes[h].next;
        ++steps;
    }
    return float4(0.0, 0.0, 0.0, 0.0);  // 色附件纯占位，判据全走 mask SSBO
}
