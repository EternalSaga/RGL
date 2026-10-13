// mdi_task_probe.task.slang —— ★M-2 task 路机制探针的 task 入口（docs/mdi_plan.md §19.12）
//
// ## 被测命题（与 mdi_groups 那族 mesh 直读 DrawIndex 的形态相对）
//
// `SV_DrawIndex` **只在 task 入口声明**：task 查行表（row = rowBase + DrawIndex），
// 把「第几列、什么值」经 payload 交给 mesh；mesh 入口（mdi_task_probe.mesh.slang）
// **一个 DrawIndex 都不碰**。这就是 §19.9 选项② / §19.12 拍的"task 传统形态"。
//
// ## 为什么必须单独成模块（编译纪律，勿"顺手合并"）
//
// slang#10123：task 与 mesh 编进同一模块时 payload 变量被**复制成两份**，
// spirv-val 放行而 NVIDIA 实测越界读。探针若合并 = 在 NV 上测的是驱动的
// 编译器缺陷而不是 task 语义，判据整体作废。
//
// ## 列语义（与 C++ 手工同源，RT 宽 = 行数 × kColsPerRow + 1 守卫列）
//
//   task 的 SV_GroupID.x = 本 draw 内的第几列（args 行 x = 该 draw 的 task 数）
//   列 col = row * kColsPerRow + tid.x        （索引类信息，经 payload 传 mesh）
//   值 value = args 行 word[3]（tag） + tid.x （数据类信息）
//
// 每 task 恰好一个 DispatchMesh（列由 task 数量决定）；mesh 侧 gid 恒 0，
// 一切位置/数值信息只能来自 payload ⟹ "payload 通路本身"也被同源自证。

[[vk::binding(0, 0)]]
ByteAddressBuffer args;

struct MdiPush
{
    uint rowBase;
};
[[vk::push_constant]] MdiPush push;

static const uint kRowBytes = 16u;     // 与 C++ kRowBytes 手工同源（前 12B = args 命令）
static const uint kColsPerRow = 3u;    // 与 C++ kColsPerRow 手工同源

struct TaskPayload
{
    uint col;
    uint value;
};

[shader("amplification")]
[numthreads(1, 1, 1)]
void main(uint3 tid : SV_GroupID, uint drawID : SV_DrawIndex)
{
    const uint row = push.rowBase + drawID;
    TaskPayload p;
    p.col = row * kColsPerRow + tid.x;
    p.value = args.Load(row * kRowBytes + 12u) + tid.x;
    DispatchMesh(1u, 1u, 1u, p);
}
