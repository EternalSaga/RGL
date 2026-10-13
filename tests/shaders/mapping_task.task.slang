// mapping_task.task.slang —— 纯判据产物：一份**声明了 mesh 侧不会有的 binding** 的 task 码流。
//
// ## 为什么仓内既有 task 产物不够用
//
// 被测对象 = `rgl/shader.cppm` 的 `declaredBindingsUnion`（批 M：只给 SPV 真声明的 binding 烘
// mapping）。要钉住"task 的声明有没有进并集"，task 必须声明一个**同用例的 mesh/frag 都不声明**的
// binding —— 否则并集不增，"task 被漏掉"和"task 进了并集"两种实现给出**同一个集合**，测不出来。
// 实测：`mdi_task_probe.task.slang` 声明 binding 0，而 tests 里每个 mesh 产物也都声明 binding 0
// ⟹ 拿它测这一维是假绿。本文件因此声明 **binding 3**（tests 里没有任何 mesh/frag 用它）。
//
// 🔴 `tbl` 必须**真读一次**：slang 会把没读过的资源从入口接口整个裁掉（docs/mdi_plan.md §19.15
// 实测 payload 同一条），裁掉就等于本文件什么都没声明、用例的期望集合跟着失真。
//
// 本产物不参与任何渲染路径，也不进 `TEST_SUITE("gpu")`——用例只解析它的码流，不建设备对象。

[[vk::binding(3, 0)]]
ByteAddressBuffer tbl;

struct Payload
{
    uint v;
};

[shader("amplification")]
[numthreads(1, 1, 1)]
void main()
{
    Payload p;
    p.v = tbl.Load(0);  // 真读：保 binding 3 留在接口里（见文件头）
    DispatchMesh(1u, 1u, 1u, p);
}
