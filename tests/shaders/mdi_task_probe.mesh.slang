// mdi_task_probe.mesh.slang —— task 路探针的 mesh 入口：**不声明 SV_DrawIndex**
//
// 全部行信息（列号 + 数值）经 payload 进来。几何与 mdi_groups 同构（一列一个纵向条带，
// y 向铺满 [-1,1]、绕序不进判据、cullMode NONE）。
//
// 变异红线（与 tests/test_mdi_task_probe.cppm 文件头声明对应）：
//   · col/value 交换 → task_probe_drawindex_increments 红（列对值错）
//   · payload 字段读成常量 → 同上两列同值，逐列断言红

struct TaskPayload
{
    uint col;
    uint value;
};

static const uint kGroupsPerRow = 3u;   // 与 task 侧/C++ 侧三方手工同源
static const uint kTotalCols = 16u;     // RT 宽（列 0..11 = MDI 数据列，列 12 = 守卫列，见 .task 文件头）

struct Vert
{
    float4 position : SV_Position;
    nointerpolation uint value : TEXCOORD0;
};

[shader("mesh")]
[numthreads(1, 1, 1)]
[outputtopology("triangle")]
void main(
    payload TaskPayload tp,
    out vertices Vert verts[4],
    out indices uint3 triangles[2])
{
    SetMeshOutputCounts(4, 2);

    const float band = 2.0f / float(kTotalCols);
    const float x0 = float(tp.col) * band - 1.0f;
    const float x1 = x0 + band;

    verts[0].position = float4(x0, -1.0f, 0.5f, 1.0f);
    verts[1].position = float4(x1, -1.0f, 0.5f, 1.0f);
    verts[2].position = float4(x1,  1.0f, 0.5f, 1.0f);
    verts[3].position = float4(x0,  1.0f, 0.5f, 1.0f);
    for (uint i = 0u; i < 4u; ++i)
        verts[i].value = tp.value;

    triangles[0] = uint3(0u, 1u, 2u);
    triangles[1] = uint3(0u, 2u, 3u);
}
