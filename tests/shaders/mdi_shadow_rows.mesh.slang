// mdi_shadow_rows.mesh.slang —— ★M-1a（docs/mdi_plan.md §19.5）行表载体的 **DrawIndex↔行** 探针
//
// ## 探针要回答的那一句话
// 「**GPU 按 `argsOffset` 展开的第 j 条 draw，与 shader 按 `rowBase + SV_DrawIndex` 读到的
//  那一行，是同一行**」。这是 §19.5 的核心红线（两个数必须同源于同一个 frameSlot 与同一个
// firstRow），而它的失效形态**不是崩溃也不是画面错**——是画错几何（读到的 objectIndex /
// clusterStart 是另一行的合法值）。截图判据看不见（本批零消费者 ⟹ 图像逐位不变），只能在这里量。
//
// ## 与 mdi_spike.mesh.slang 的区别（为什么要另开一份，而不是给 spike 加个宏）
//   · 行步长 **24B**（= `RGL::kMdiShadowRowStride`，6 word），spike 那份是 16B 旧形态；
//   · 载荷 = word[3] `objectIndex` **与** word[4] `clusterStart` **与** word[5] `meshGroupCount`
//     **三个**字段（spike 只读一枚 tag；🔴 ★M-3T-c 起 word[5] 是真载荷——真机上 task 读它当
//     `DispatchMesh` 的 X，读歪它 = 发错 mesh 组数）⟹ 「字段互换」与「整行读歪」两种坏形态可分辨；
//   · 行表是 **ring×2 的一张 buffer**（列数 = sliceRows × slices），于是「帧槽轮转落在偏移上」
//     这件事本身进了判据面：写 slice 1 的时候 slice 0 的列必须保持哨兵；
//   · 本用例的 4 行里混一条 `groupCountX = 0` 的孔洞行（§19.2 D4），孔洞列必须空白。
// 另开文件而不分支化：两个判据共享一份代码时，一处改坏会互相污染（同 mdi_groups.mesh.slang
// 头部那条理由）。
//
// ## 值与列的构造（判据本体）
//   · **画在哪一列** = `rowBase + SV_DrawIndex`（索引类信息；错 → 整列空白 / 串列）
//   · **写什么值**   = `objectIndex × 16 + clusterStart × 4 + meshGroupCount`（**同一行**的三个
//     数据字段；错 → 值与列不符）。三个乘数互错开 ⟹ 任一字段读歪一格 tag 必变。`×16/×4` 是为了
//     本探针的期望值与 clear 哨兵 0 天然可辨（C++ 侧手算期望 = tests/test_meshlet_shadow_args.cppm
//     的 `wantTag`）。
//
// ## 手工同源的常量（🔴 与 C++ 侧改动必须同步，否则列映射整体错位 = 本探针立刻红）
//   kRowBytes        = 24 = `RGL::kMdiShadowRowStride`
//   kObjBytes        = 12 = `offsetof(MdiShadowRow, objectIndex)`（word[3]）
//   kStartBytes      = 16 = `offsetof(MdiShadowRow, clusterStart)`（word[4]）
//   kMeshGroupsBytes = 20 = `offsetof(MdiShadowRow, meshGroupCount)`（word[5]，★M-3T-c 新增）
//   kCols            = 14 = C++ 侧 `kCaseSliceRows(7) × kCaseSlices(2)`（整张行表的行数 = RT 宽）
// 无反射管线（spike 先例）；C++ 侧用 `static_assert` + 逐列期望把这五个数钉住（见测试文件头）。
//
// ## y 向 / 绕序
// 每列纵向铺满 [-1,1] ⟹ viewport 的 y 翻向不进判据（对称覆盖，坐标系那条已由 izb_1b 结案）；
// C++ 侧管线 cullMode = NONE，绕序也不进判据。

[[vk::binding(0, 0)]]
ByteAddressBuffer args;

struct ShadowRowsPush
{
    uint rowBase;   // 本次 MDI 调用的行基址（= 同源 pair 的 shader 侧那一半，一次调用内均匀）
};
[[vk::push_constant]] ShadowRowsPush push;

static const uint kRowBytes = 24u;    // = C++ kMdiShadowRowStride
static const uint kObjBytes = 12u;    // word[3] = objectIndex
static const uint kStartBytes = 16u;  // word[4] = clusterStart
static const uint kMeshGroupsBytes = 20u;  // word[5] = meshGroupCount（★M-3T-c 起是真载荷）
static const uint kCols = 14u;        // = C++ 侧 sliceRows(7) × slices(2) = 整张行表的行数

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

    // ← 被测表达式：行号 = 均匀 push 基址 + 逐 draw 递增的 DrawIndex（§19.2 D1 的 MDI 路 word0 语义）
    const uint row = push.rowBase + drawID;
    const uint obj = args.Load(row * kRowBytes + kObjBytes);
    const uint start = args.Load(row * kRowBytes + kStartBytes);
    // 🔴 ★M-3T-c：word[5] = meshGroupCount（真机上 task 读它当 DispatchMesh 的 X）——自证面扩到这里
    const uint groups = args.Load(row * kRowBytes + kMeshGroupsBytes);

    // 第 row 列的 NDC 横带（x 左→右，与 framebuffer x 同向，不受 y 翻向影响）
    const float x0 = float(row) * (2.0f / float(kCols)) - 1.0f;
    const float x1 = x0 + (2.0f / float(kCols));

    verts[0].position = float4(x0, -1.0f, 0.5f, 1.0f);
    verts[1].position = float4(x1, -1.0f, 0.5f, 1.0f);
    verts[2].position = float4(x1,  1.0f, 0.5f, 1.0f);
    verts[3].position = float4(x0,  1.0f, 0.5f, 1.0f);
    for (uint i = 0u; i < 4u; ++i)
        verts[i].tag = obj * 16u + start * 4u + groups;

    triangles[0] = uint3(0u, 1u, 2u);
    triangles[1] = uint3(0u, 2u, 3u);
}
