// mdi_groups.mesh.slang —— per-draw groupCount 探针（docs/mdi_plan.md §6）
//
// ## 为什么需要第二个探针文件（而不是给 mdi_spike 加个宏）
//
// `mdi_spike.mesh.slang` 里所有 args 行的 `groupCountX` 都是 1 ⟹ **它从没验证过"GPU 逐 draw
// 读到自己那一行的 groupCount"**。而 Phase A 的现实是 1591 个 draw 的 groupCount **各不相同**
// （= ceil(totalTriangles/10)），那才是真正要担保的东西。本文件把 groupCount 变成判据本体：
// 每 draw 发 K 个 workgroup，每个 workgroup 占一**独立列**、写一个**独立值**，读错数量就露形。
//
// 不用宏而另开文件：本探针的列几何与 kCols 语义都和上一个不同（一 draw 占 K 列），
// 分支化会让两个判据共享一份代码、一处改坏互相污染。
//
// ## 几何（K = 3 列/draw，4 个 draw → 12 列）
//
//   列 = row * K + gid.x      row = push.rowBase + SV_DrawIndex
//   值 = tag + gid.x          tag = args 行 word[3]
//
// 于是期望是**闭式**的：某行 groupCountX = n ⟹ 该 draw 只覆盖它的 n 列（其余保持 clear 哨兵），
// 且第 i 列的值恰为 tag + i。n 读大 → 多出列被画（哨兵破）；n 读小 → 该有值的列保持哨兵（红）。
//
// ## y 向 / 绕序
//
// 同 mdi_spike.mesh.slang：纵向铺满 [-1,1]（翻转不进判据）、管线侧 cullMode = NONE。

[[vk::binding(0, 0)]]
ByteAddressBuffer args;

struct MdiPush
{
    uint rowBase;
};
[[vk::push_constant]] MdiPush push;

static const uint kRowBytes = 16u;   // 与 C++ kMdiRowBytes 手工同源
static const uint kTagBytes = 12u;   // tag = word[3]
static const uint kGroupsPerDraw = 3u;   // = C++ kGroupsPerDraw（RT 宽 = 行数 × 本值）
static const uint kDraws = 4u;       // args 行数（= C++ kGroupRows）

struct Vert
{
    float4 position : SV_Position;
    nointerpolation uint value : TEXCOORD0;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void main(
    uint3 gid : SV_GroupID,
    uint drawID : SV_DrawIndex,
    out vertices Vert verts[4],
    out indices uint3 triangles[2])
{
    SetMeshOutputCounts(4, 2);

    const uint row = push.rowBase + drawID;
    // ← 每列一个 workgroup：gid.x 超出该行的 groupCountX 时本 shader 根本不会被派到那么多个
    //   workgroup，故"少画列"直接由 GPU 读到的 x 值决定（这就是被测点）
    const uint col = row * kGroupsPerDraw + gid.x;
    const uint value = args.Load(row * kRowBytes + kTagBytes) + gid.x;

    const float band = 2.0f / float(kDraws * kGroupsPerDraw);   // 一列的 NDC 宽度
    const float x0 = float(col) * band - 1.0f;
    const float x1 = x0 + band;

    verts[0].position = float4(x0, -1.0f, 0.5f, 1.0f);
    verts[1].position = float4(x1, -1.0f, 0.5f, 1.0f);
    verts[2].position = float4(x1,  1.0f, 0.5f, 1.0f);
    verts[3].position = float4(x0,  1.0f, 0.5f, 1.0f);
    for (uint i = 0u; i < 4u; ++i)
        verts[i].value = value;

    triangles[0] = uint3(0u, 1u, 2u);
    triangles[1] = uint3(0u, 2u, 3u);
}
