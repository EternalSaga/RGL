// mdi_spike.mesh.slang —— Multi Draw Indirect 的 DrawIndex 语义探针（docs/mdi_plan.md §6）
//
// ## 探针要回答的问题（spike 专用，不入引擎 SPV 面）
//
// Phase A 的全部前提 = 「indirect 第 j 个 draw 里，`SV_DrawIndex` 恰好等于 j」。spec 允许
// ≠ 驱动实现正确，所以这条必须在真设备上量出来。
//
// ## 自证结构：两条信息同源自证
//
//   · **画在哪一列** = `rowBase + drawID`（索引类信息，错 → 整列空白/串列）
//   · **写什么值**   = args 同一行的 word[3]（数据类信息，错 → 值与列不符）
//
// 两者都从同一个 `row` 推出来：若 DrawIndex 恒 0 → 16 个 draw 全画第 0 列、其余列保持 clear
// （= 空白，立刻红）；若只是 +12 字节偏移算错 → 列对但值错（可分辨是"索引坏"还是"偏移坏"）。
//
// ## args buffer 布局（**刻意 16B 步长**）
//
// 行 = [ groupCountX | groupCountY | groupCountZ | tag ]，前 12B 恰是
// `VkDrawMeshTasksIndirectCommandEXT`，第 4 个字是**只有本探针会读**的 per-draw 载荷。
// 这一步长就是 Phase A 的核心手法（GPU 读前 12B、shader 读 word[3]，**一张 buffer 两个读者**），
// 所以本文件同时是在验「stride 16 的 indirect buffer 能否被 shader 当 SSBO 读」这件事本身。
// 字节偏移 = C++ 侧 kMdiRowBytes / kMdiTagBytes 手工同源（无反射管线，spike 先例）。
//
// ## y 向
//
// 每列纵向铺满 [-1,1]，故引擎那对负 viewport 翻转对本探针**无影响**（对称覆盖）——
// 刻意如此：探针只测 DrawIndex，不重复测 izb_1b 已经量过的坐标系（那道已结案）。
//
// ## 绕序
//
// cullMode = NONE（C++ 侧管线/动态态都设 None），绕序不进判据。

[[vk::binding(0, 0)]]
ByteAddressBuffer args;

struct MdiPush
{
    uint rowBase;   // 本批 draw 在 args 里的起始行号（Phase A 里 = 桶基址，一次 MDI 调用内均匀）
};
[[vk::push_constant]] MdiPush push;

static const uint kRowBytes = 16u;   // = C++ kMdiRowBytes
static const uint kTagBytes = 12u;   // = C++ kMdiTagBytes（word[3]）
static const uint kCols = 12u;       // RT 宽 = args 行数

struct Vert
{
    float4 position : SV_Position;
    nointerpolation uint tag : TEXCOORD0;  // 平摊：整列同值，避免插值把 tag 变成非整数
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

    // ← 本探针的唯一被测表达式。DrawIndex 语义（interfaces.adoc:2780）：indirect 下从 0 起、
    //   每个 draw +1。rowBase 让它同时验"push 均匀基址 + drawID 递增"能否相加。
    const uint row = push.rowBase + drawID;
    const uint tag = args.Load(row * kRowBytes + kTagBytes);

    // 第 row 列的 NDC 横带（x 左→右，与 framebuffer x 同向，不受 y 翻转影响）
    const float x0 = float(row) * (2.0f / float(kCols)) - 1.0f;
    const float x1 = x0 + (2.0f / float(kCols));

    verts[0].position = float4(x0, -1.0f, 0.5f, 1.0f);
    verts[1].position = float4(x1, -1.0f, 0.5f, 1.0f);
    verts[2].position = float4(x1,  1.0f, 0.5f, 1.0f);
    verts[3].position = float4(x0,  1.0f, 0.5f, 1.0f);
    for (uint i = 0u; i < 4u; ++i)
        verts[i].tag = tag;

    triangles[0] = uint3(0u, 1u, 2u);
    triangles[1] = uint3(0u, 2u, 3u);
}
