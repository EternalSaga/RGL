// izb_mask.frag.slang —— 批 11.3-1d 后半：光空间保守光栅 frag（IZB mask pass 判定体）
//
// 语义 = FTIZB pass5 的 v1 退化版（golden = tests/test_izb_mask.cppm，2D 棱柱三边
// 同号 + 节点位置插值光深度）：**每个 fragment = 一个遮挡候选三角**，走本 cell 的
// IZB 节点链；某节点被本三角"压住"（节点在三角之后 + 节点 uv 在三角投影内）
// → 该节点所属屏幕像素 = 被遮挡 → 置 mask 位（bit = 像素 ID，地址即像素）。
//
// 深度判据的自遮挡处理：三角是平面（正交光），节点 uv 处的三角深度 = 三顶点深度
// 按 2D barycentric 插值——同一平面的自身像素精确相等，**严格 > + 微偏置**排除
// （偏置 2e-4 ndc ≈ 毫米级；正确性判据 = 1e 的 imgqa 对拍，spike 无同面用例）。
//
// y 向口径：fb 行 = 上→下（mesh 出 D3D 口径 y + 引擎负 viewport 补偿后的标准
// Vulkan fb），而 izb_create 的 uv.y 走 ndc 直式（下→上）→ **cell.y = G-1-fb.y**
// （唯一一次翻转在这钉死；对拍若整图上下镜像 = 这里翻错）。
//
// 平摊属性：uvA/uvB/uvC = 本三角三顶点 (uv.xy, lightDepth.z)——三顶点写同一份
// （mesh 顶点逐三角独立），provoking 取值即整三角。
//
// ⚠️ ByteAddressBuffer 一切偏移 = 字节偏移；节点 16B = (u, v, lightDepth, next)。

static const uint kNone = 0xFFFFFFFFu;
static const float kSelfShadowBias = 2e-4;  // ndc 光深度偏置（正交 = 线性，见头注释）

struct IzbMaskPush
{
    uint objectIndex;   // = ShadowDrawArgs.objectIndex（mesh 消费；本 stage 忽略）
    uint cascadeIndex;  // = ShadowDrawArgs.cascadeIndex（同上）
    uint gridDim;       // 光网格 G（create 同值；cell 索引基数）
};

[[vk::binding(5)]] ByteAddressBuffer head;
[[vk::binding(6)]] ByteAddressBuffer nodes;
[[vk::binding(7)]] RWByteAddressBuffer mask;  // 1 bit/屏幕像素（word = pid>>5）

[[vk::push_constant]] IzbMaskPush push;

struct In
{
    float4 position : SV_Position;
    nointerpolation float4 uvA;
    nointerpolation float4 uvB;
    nointerpolation float4 uvC;
};

[shader("fragment")]
void main(In v)
{
    const int gx = int(push.gridDim);
    const int cx = int(v.position.x);
    const int cy = gx - 1 - int(v.position.y);  // fb 行 → uv 行（唯一翻转，见头注释）
    if (cx < 0 || cx >= gx || cy < 0 || cy >= gx)
        return;

    uint h = head.Load(uint(cx + cy * gx) * 4u);
    if (h == kNone)
        return;  // 本 cell 无候选（绝大多数 fragment 早退）

    // 2D 投影退化（三角正对光线 = uv 面积 0）：不遮任何光（标准近似）
    const float2 e0 = v.uvB.xy - v.uvA.xy;
    const float2 e1 = v.uvC.xy - v.uvA.xy;
    const float den = e0.x * e1.y - e1.x * e0.y;
    if (abs(den) < 1e-9)
        return;

    int steps = 0;
    while (h != kNone && steps < (1 << 26))
    {
        const float2 nodeUv = asfloat(nodes.Load2(h * 16u));
        const float nodeD = asfloat(nodes.Load(h * 16u + 8u));

        // barycentric（除以 den 自带朝向归一：inside = 三重心坐标均 ≥ 0 对两种绕序同判）
        const float2 q = nodeUv - v.uvA.xy;
        const float b1 = (q.x * e1.y - e1.x * q.y) / den;
        const float b2 = (e0.x * q.y - q.x * e0.y) / den;
        const float b0 = 1.0 - b1 - b2;
        // FTIZB 判定（golden = tests/test_izb_mask.cppm 同款）：inTri = 节点 uv 在本
        // 三角光投影内（edge 判据，barycentric 除 den 自带朝向归一）；occl = 节点在
        // 三角之后——深度在**节点 uv 处**按平面插值（同平面自身像素精确相等 → 严格 >
        // + bias 排除自遮挡），两判据同真 = 该像素光源方向被本三角遮挡
        const bool inTri = b0 >= 0.0 && b1 >= 0.0 && b2 >= 0.0;
        const float dF = v.uvA.z * b0 + v.uvB.z * b1 + v.uvC.z * b2;
        const bool occl = nodeD > dF + kSelfShadowBias;
        if (inTri && occl)
            mask.InterlockedOr((h >> 5u) * 4u, 1u << (h & 31u));  // 该像素被遮
        h = nodes.Load(h * 16u + 12u);
        ++steps;
    }
}
