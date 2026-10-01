// izb_create.comp.slang —— 批 11.3-1c：IZB 创建 dispatch（grid-of-lists 原子插入）
//
// FTIZB 2015 pass3 的 spike 版（docs/hfts_izb_plan.md §4.1-1c；数据布局按 2016 TVCG：
// **地址即像素索引**——node 数组按像素 ID 索引，一个可见像素至多一个节点，无节点池
// 溢出问题；head/next 存像素 ID，NONE = 0xFFFFFFFF 哨兵）。
//
// 与生产的差异（1c 有意为之，1d 集成时替换输入/输出载体，算法本体不变）：
//   · 输入深度 = SSBO float[]（生产 = z-prepass/G-buffer 纹理）；
//   · 光空间网格 = 单层正交方向光（生产加 HFTS 两 frustum 分区）；
//   · payload = 裸 float 光深度（生产打包 subpixel/光源 index 进高位）。
//
// params SSBO 字节布局（**唯一声明点**，测试侧按此手写字节，勿再抄第三份）：
//   [0]   int   pixelRes     像素网格边长
//   [4]   int   gridDim      光空间网格边长 G
//   [8]   float emptyDepth   深度 ≥ 此值 = 空像素/无效，不建节点
//   [12]  int   maxSteps     链表游走步数上限（防坏链死循环；= pixelRes²）
//   [16 + 16*i]  float4  invViewProj 第 i 列（i=0..3，列主序）
//   [80 + 16*j]  float4  lightView   第 j 列
// 矩阵乘法用列组合（scene.slang clipPos 同款约定——避开 float4x4 类型布局歧义，
// 该歧义当年是「地面侥幸正常、建筑炸形」级事故的根因）。

struct IzbNode {
    float depth;  // 光源平面距离（大 = 远）
    uint  next;   // 同 cell 下一节点（像素 ID）；NONE 结尾
};

static const uint kNone = 0xFFFFFFFFu;

[[vk::binding(0)]] RWByteAddressBuffer head;               // G*G uint
[[vk::binding(1)]] RWStructuredBuffer<IzbNode> nodes;      // 按像素 ID 索引
[[vk::binding(2)]] ByteAddressBuffer depthIn;              // pixelRes² float
[[vk::binding(4)]] ByteAddressBuffer params;               // 布局见头注释（3 = primed，本核不读）

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    const int res = params.Load(0);
    const int gridDim = params.Load(4);
    const float emptyDepth = asfloat(params.Load(8));

    const uint p = tid.x;
    if (p >= uint(res * res))
        return;

    const float d = asfloat(depthIn.Load(p * 4));  // ByteAddressBuffer.Load 返 int，必须位转
    if (d >= emptyDepth)  // 空像素（含 FLT_MAX 哨兵）——负向用例走这条
        return;

    const float px = float(p % res);
    const float py = float(p / res);
    const float2 ndc = float2((px + 0.5) / res * 2.0 - 1.0, (py + 0.5) / res * 2.0 - 1.0);

    // world = invViewProj · (ndc, d, 1)（列组合）
    // ⚠️ ByteAddressBuffer.Load4 返回 int4——必须 asfloat 位转（引擎同款纪律：
    // cull.comp.slang / ao.frag.slang 全部 asfloat(x.Load4(...))）。直接赋 float4
    // 是**数值转换**（把浮点原始位当整数转浮点）= 矩阵变垃圾（1c 首跑全丢弃的根因）。
    const float4 c0 = asfloat(params.Load4(16 + 0 * 16));
    const float4 c1 = asfloat(params.Load4(16 + 1 * 16));
    const float4 c2 = asfloat(params.Load4(16 + 2 * 16));
    const float4 c3 = asfloat(params.Load4(16 + 3 * 16));
    const float4 w = c0 * ndc.x + c1 * ndc.y + c2 * d + c3;

    // 光空间（方向光正交）：uv = ls.xy 半区间化，深度 = -ls.z（光源距离，glm 视向 -z）
    const float4 l0 = asfloat(params.Load4(80 + 0 * 16));
    const float4 l1 = asfloat(params.Load4(80 + 1 * 16));
    const float4 l2 = asfloat(params.Load4(80 + 2 * 16));
    const float4 l3 = asfloat(params.Load4(80 + 3 * 16));
    const float4 ls = l0 * w.x + l1 * w.y + l2 * w.z + l3 * w.w;
    const float2 uv = float2(ls.x, ls.y) * 0.5 + 0.5;

    // 越出光空间网格 → 不建节点（负向用例；生产由 HFTS 分区保证覆盖）
    if (uv.x < 0.0 || uv.x >= 1.0 || uv.y < 0.0 || uv.y >= 1.0)
        return;

    const int2 cell = int2(floor(uv * gridDim));
    const uint idx = uint(cell.x + cell.y * gridDim);

    nodes[p].depth = -ls.z;
    uint prevHead;
    // ⚠️ ByteAddressBuffer 的原子偏移与 Load/Store 同为**字节**偏移——传元素索引 =
    // 写进 word idx/4（1c 实测：cell 2 的整条链挂进 head[0]，全表错位 4 倍）。
    head.InterlockedExchange(idx * 4, p, prevHead);  // 原子栈压入：next = 旧表头
    nodes[p].next = prevHead;
}
