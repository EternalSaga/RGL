// dheap_ubo_wrong.comp.slang —— 阳性对照体（🔴 判据 2）
//
// 与 dheap_ubo.comp.slang **同 binding、同 descriptor、同 heap 布局**，唯一区别 =
// 读"错误"成员 / "错误"元素索引 / "错误"字节偏移。断言方向：本 shader 写回的槽位值
// 必须 **≠** 主判据 shader 写回同槽位的值。
//
// 为什么必须有它：主判据"读回来等于 magic"有两种成因——
//   ① shader 真的按 Offset 装饰读到了正确字节；
//   ② 读回来的是零初始化/未初始化值，而 magic 恰好等于它。
// 本对照把 ② 排除掉：若驱动/编译把 UBO 的读取整体平移了一格（scalar layout 与
// std140/std430 混淆的典型症状），主判据槽位 0 会拿到 roughness 的 magic，
// 而本 shader 的槽位 0 会拿到 exposure 的 magic —— 两条断言同时把这种"错位一格"
// 钉死（单靠主判据 + 非零 magic 只能排除全零，排不了置换）。
//
// 错位设计（每槽位恰好错一格，与主判据一一对应）：
//   0 ← hdr.roughness（主判据读 exposure）
//   1 ← hdr.flags 位转（主判据读 roughness）
//   2 ← hdr.count（主判据读 flags）
//   3 ← hdr.tint.z 位转（主判据读 count）
//   4 ← hdr.tint.w（主判据读 tint.y）
//   5 ← hdr.scale（主判据读 dir.z）
//   6 ← hdr.exposure（主判据读 scale，绕回头部）
//   7 ← mvps[2].c2.x（主判据读 mvps[1]，**错误元素索引**）
//   8 ← raw.Load(8)（主判据读 Load(4)，**错误字节偏移**）
//   9 ← mvps[1].c0.x（主判据读 mvps[0]）
//  10 ← raw.Load(4)（主判据读 Load(0)）
//  11 ← 立即数锚点（与主判据体同值同位置：本对照体也必须自证"dispatch 真跑了"，
//        否则"≠ 主判据值"可能只是"什么都没写"造成的假阳性对照）

struct Header {
    float  exposure;
    float  roughness;
    uint   flags;
    uint   count;
    float4 tint;
    float3 dir;
    float  scale;
};

struct Mvp {
    float4 c0;
    float4 c1;
    float4 c2;
    float4 c3;
};

[[vk::binding(0)]] ConstantBuffer<Header>   hdr;
[[vk::binding(1)]] StructuredBuffer<Mvp>    mvps;
[[vk::binding(2)]] ByteAddressBuffer        raw;
[[vk::binding(3)]] RWStructuredBuffer<uint> outBuf;

/// 派发锚点（与主判据体同值同位置）：本对照体也必须证明"dispatch 真跑了"，
/// 否则"槽位值 ≠ 主判据值"可能只是"什么都没写"造成的假阳性。
static const uint kDispatchAnchor = 0xBEEFCAFEu;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    outBuf[11] = kDispatchAnchor;
    outBuf[0] = asuint(hdr.roughness);
    outBuf[1] = asuint(hdr.flags);
    outBuf[2] = hdr.count;
    outBuf[3] = asuint(hdr.tint.z);
    outBuf[4] = asuint(hdr.tint.w);
    outBuf[5] = asuint(hdr.scale);
    outBuf[6] = asuint(hdr.exposure);
    outBuf[7] = asuint(mvps[2].c2.x);
    outBuf[8] = raw.Load(8);
    outBuf[9] = asuint(mvps[1].c0.x);
    outBuf[10] = raw.Load(4);
}
