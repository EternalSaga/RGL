// izb_create.comp.slang —— 批 11.3-1d：IZB 创建 dispatch（引擎版，图外 compute）
//
// FTIZB pass3 + tests/ 1c spike 语义（护栏 = tests/test_izb_create.cppm，原子链/哨兵/
// 步数上限的判据全在那边）；本版换输入/输出载体：
//   · 深度 = D32Sfloat 深度图（与 SSAO 共用 depthSampleSlot，eShaderReadOnlyOptimal）；
//   · 光空间 = **cascade-0 阴影 viewProj**（正交 → ndc.z 天然线性，与 CSM 光向同源；
//     v1 只覆盖 split0 范围，越界像素不建节点 = 近场 IZB，HFTS 分区留阶段 2）；
//   · payload 打包进节点：(u, v, lightDepth, next) 16B/像素——mask pass 的 edge
//     判定直接读节点（spike 的独立 pixelUV SSBO 不再需要，1d 头注释预告的收编）。
//
// 节点布局（**地址即像素 ID**，1c 定案）：nodes 16B/像素 =
//   [p*16 + 0,4,8] float u, v, lightDepth（= lightVP·world 的 ndc.z ∈ [0,1]）
//   [p*16 + 12]    uint next（同 cell 下一节点 = 像素 ID；NONE 结尾）
// head = G² uint，NONE 哨兵；**每帧由 CPU 侧 vkCmdFillBuffer 0xFFFFFFFF 清空**。
//
// ⚠️ ByteAddressBuffer 一切偏移 = 字节偏移（1c 坑②）；asuint/asfloat 位转纪律同。
// 矩阵乘用列组合（scene.slang 同款），规避 float4x4 类型布局歧义。

static const uint kNone = 0xFFFFFFFFu;

struct IzbCreatePush {
    float4 invVPM[4];      // scene data @64 skyInvViewProj（clip→world）
    float4 lightVP[4];     // shadowViewProj[cascade0]（world→光 ndc，正交）
    int pixelW;
    int pixelH;
    int gridDim;           // 光空间网格边长 G
    float emptyDepth;      // 深度 ≥ 此值 = far 面 miss（1.0），不建节点
};

[[vk::binding(0)]] RWByteAddressBuffer head;
[[vk::binding(1)]] RWByteAddressBuffer nodes;
[[vk::binding(2)]] Texture2D<float> depthTex;  // eD32Sfloat（eDepth aspect view）

[[vk::push_constant]] IzbCreatePush push;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const uint p = tid.x;
    if (p >= uint(push.pixelW * push.pixelH))
        return;

    const int px = int(p % uint(push.pixelW));
    const int py = int(p / uint(push.pixelW));
    const float d = depthTex.Load(int3(px, py, 0));
    if (d >= push.emptyDepth || d <= 0.0)  // far miss / 无效深度 → 无节点
        return;

    const float2 ndc = float2((px + 0.5f) / push.pixelW * 2.0 - 1.0,
        (py + 0.5f) / push.pixelH * 2.0 - 1.0);
    const float4 w = push.invVPM[0] * ndc.x + push.invVPM[1] * ndc.y
        + push.invVPM[2] * d + push.invVPM[3];
    const float4 ls = push.lightVP[0] * w.x + push.lightVP[1] * w.y
        + push.lightVP[2] * w.z + push.lightVP[3] * w.w;

    const float2 uv = ls.xy * 0.5 + 0.5;
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0)
        return;  // cascade-0 范围外（v1 语义；非 bug）

    const int2 cell = int2(floor(uv * push.gridDim));
    const uint idx = uint(cell.x + cell.y * push.gridDim);

    nodes.Store(p * 16, asuint(uv.x));
    nodes.Store(p * 16 + 4, asuint(uv.y));
    nodes.Store(p * 16 + 8, asuint(ls.z));
    uint prevHead;
    head.InterlockedExchange(idx * 4, p, prevHead);  // 原子栈压入（字节偏移！）
    nodes.Store(p * 16 + 12, prevHead);
}
