// dheap_stride_wrong.comp.slang —— StructuredBuffer stride 往返【🔴 阳性对照体】
//
// 与 dheap_stride.comp.slang **同 binding、同 descriptor、同 heap 布局、同 buffer 内容**，
// 唯一区别 = 读**错误的元素下标**。断言方向：本 shader 写回的每个槽位必须
// **≠** 主判据体同槽位的期望值。
//
// ## 缺这条时"主判据绿"排不掉的假绿
//
// 「读回来 == 期望值」有两种成因：① 驱动真按 `ArrayStride` 取数；② 读回来的根本不是
// 那块 buffer（描述符没生效 / 读到别处），而期望值恰好等于它。非零填充排掉"全零恰好
// 相等"，**排不掉置换**——只有"错位版 shader 必须拿到不同结果"才排得掉。
// 本项目 §3.4 第一条实测过：漏写描述符是完全静默的（不挂死、不 device lost、VVL 不报）。
//
// ## 错位设计（每槽位恰好错一个元素，与主判据一一对应）
//
//   0 ← it[0].clusterIdx（主判据读 it[1]，**元素 0 恰好不受 stride 影响** → 用它当"错"）
//   1 ← it[1].clusterIdx（主判据读 it[1].objectSlot，错在成员）
//   2 ← it[2].objectSlot（主判据读 it[2].clusterIdx）
//   3 ← h[0].group       （主判据读 h[1]）
//   4 ← h[0].remapOffset （主判据读 h[1].remapOffset）
//   5 ← h[0].center.x    （主判据读 h[1].center.x）
//   6 ← h[0].center.y    （主判据读 h[1].center.y）
//   7 ← h[0].center.z    （主判据读 h[1].center.z）
//   8 ← h[0].radius      （主判据读 h[1].radius）
//   9 ← h[0].error       （主判据读 h[1].error）
//  10 ← h[1].group       （主判据读 h[2].group）
//  11 ← 立即数锚点（与主判据体同值同位置：对照体也必须自证"dispatch 真跑了"，
//        否则"≠ 期望值"可能只是"什么都没写"造成的假阳性对照）
//
// 填充是"第 k 个字 = kWordBase + k"（两两不同），⟹ 上表每个槽位读到的字下标都与主判据
// 该槽位的字下标不同 ⟹ 值必不同 ⟹ 判据是 discriminative 的，不是碰运气。
//
// ⚠️ 本文件**只新增、不改任何现有 .slang**；产物落 build/tests/generated_shaders/。

struct MeshletCullItem {
    uint clusterIdx;
    uint objectSlot;
};

struct MeshletClusterHeader {
    uint   group;
    int    refined;
    uint   triCount;
    uint   vertCount;
    uint   triOffset;
    uint   remapOffset;
    float3 center;
    float  radius;
    float  error;
};

[[vk::binding(0)]] StructuredBuffer<MeshletCullItem>    items;
[[vk::binding(1)]] StructuredBuffer<MeshletClusterHeader> headers;
[[vk::binding(2)]] RWStructuredBuffer<uint>            outBuf;

/// 派发锚点（与主判据体同值同位置）。
static const uint kDispatchAnchor = 0xBEEFCAFEu;

[shader("compute")]
[numthreads(1, 1, 1)]
void main() {
    outBuf[11] = kDispatchAnchor;
    outBuf[0] = items[0].clusterIdx;
    outBuf[1] = items[1].clusterIdx;
    outBuf[2] = items[2].objectSlot;
    outBuf[3] = headers[0].group;
    outBuf[4] = headers[0].remapOffset;
    outBuf[5] = asuint(headers[0].center.x);
    outBuf[6] = asuint(headers[0].center.y);
    outBuf[7] = asuint(headers[0].center.z);
    outBuf[8] = asuint(headers[0].radius);
    outBuf[9] = asuint(headers[0].error);
    outBuf[10] = headers[1].group;
}
