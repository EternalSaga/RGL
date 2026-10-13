// shadow_meshlet.task.slang —— 阴影段真 MDI 的 **task 入口**（docs/mdi_plan.md ★M-3T-a）
//
// ## 它存在的唯一理由：把 `SV_DrawIndex` 从 mesh 入口搬到这里
//
// §19.9 实测：AMD Radeon 8060S（RADV STRIX_HALO / gfx1151 / Mesa 26.2.3）上，**mesh** 入口一声明
// `SV_DrawIndex`，同提交里后续 mesh draw 就不产出像素（主视图那栋 bistro 整块消失）。§19.15 又实测：
// 同一个 builtin 声明在 **task** 入口 ⟹ 不触发（主视图 G-buffer albedo/metalrough 逐位不变）。
// ⟹ 本文件把 DrawIndex 的声明处从 mesh 换到 task，mesh 侧改收 payload。门 `meshlet.mdi_shadow`
// 默认关；门关用的还是 `shadow_meshlet.mesh.spv` 那份一字节没动的 SPV（§19.2 D2 的构造性护栏）。
//
// ## 🔴 为什么它不是"多余的一层"：`SV_GroupID` 的语义在 task 之下会变
//
// spec：mesh 阶段的 `WorkgroupId` = "position within the **current mesh task**"，而 task 阶段的
// `WorkgroupId` = 本 draw 任务网格内的 task 序号。本管线每 task 只发 **1** 个 mesh group ⟹ mesh 侧
// `groupID.x` **恒 0**（不再是旧的 0..2×簇数-1）。所以旧派发契约里 `groupID.x` 承担的那一维
// （半区号 + 簇内偏移）**必须**由 task 的 `SV_GroupID` 经 payload 转交，否则每个 draw 只会画第一个簇。
// 这条语义差是 §19.15 探针"阴影故意画错"的根因，也是本文件必须存在、而不是"task 只搬 DrawIndex 就够"的原因。
//
// ## 为什么行表 / 执行器 / 采集侧 / profiler 零改动
//
// MDI 行的 `gcX` 原本 = 该 draw 的 **mesh 组**数（= 2×选级后簇数，每簇固定 2 槽）。挂上 task 后
// `vkCmdDrawMeshTasksIndirectEXT` 的 groupCount 单位变成 **task** 数，而"每 task 恰好发 1 个 mesh
// group"让两个数值**恒等** ⟹ 行表构建器、`DrawCall`、`emitShadowCasters`、`mdi.count`/`mdi.rows`
// 口径、A5 闭式锚全部不用动。task 的 `SV_GroupID.x` 取值域因此也恰好等于旧 `groupID.x` 的取值域。
//
// ## 为什么本文件不读行表（读行表是下一批的事）
//
// 读行表要在 task 里再拼一遍行步长 24 与字段偏移 —— 那是把行表契约从两份（`rgl/mdi_rows.cppm` +
// mesh 侧）扩成三份，且 `meshlet_spec_checks` 的对账面跟着变大。收益（每 draw 只发 1 个 task、
// `gcX` 语义换成 task 数）属于**性能优化批**，与"绕开毒化"这件事正交，本批不付。
//
// ## 为什么单独成模块
//
// slang#10123：task 与 mesh 编进同一模块时 payload 变量被**复制成两份**（spirv-val 放行、NVIDIA
// 实测越界读）⟹ 必须独立编译（`tests/shaders/mdi_task_probe.*` 同一条纪律）。
//
// ## 验收看产物，不看"编译过了"
//
// 触发源（两个 builtin）必须真在 SPV 接口里，否则"没毒化"是必然不是结论：
//     spirv-dis build/rgl/generated_shaders/shadow_meshlet.task.spv | grep -cE "BuiltIn.(DrawIndex|WorkgroupId)"
// 必须 = 2。实测（pinned slangc 2026.16）：**payload 输出存储本身就足以让两者活着**，不需要
// §19.14 探针期那条人为的不可折叠守卫——那条防的是"只喂死存储"，而 task payload 是输出接口、
// slang 看不见 mesh 侧就不敢裁。Slang 的 task 拼写是 `[shader("amplification")]` + `DispatchMesh`
// （**没有** `EmitMeshTasksEXT` 这个名字），产物仍是 `OpEntryPoint TaskEXT`，见 §19.14 纠错。

// 🔴 本 struct 是 payload 契约在 task 侧的拼写，mesh 侧（`shadow_meshlet.mesh.slang` 的
// `MDI_SHADOW` 分支）有一份**逐字段同构**的第二份。跨模块无法共享声明（两份 .slang 各编各的）
// ⟹ 改一侧必须同改另一侧，字段序即契约。
struct MdiShadowPayload
{
    uint drawID;   // 本 draw 在本次 MDI 调用内的序号（= task 入口的 SV_DrawIndex）
                   // ⟹ mesh 侧算行号 row = rowBase(push.word0) + drawID
    uint slot;     // 本 task 在本 draw 内的序号（= task 入口的 SV_GroupID.x）
                   // ⟹ mesh 侧用它替代已经恒 0 的 groupID.x：cluster 偏移 = slot>>1、半区 = slot&1
};

[shader("amplification")]
// 1 线程 = 每 task 恰好一次 DispatchMesh。取 1 有两个独立理由：① 本 task 的活只有"发一次 + 填
// 两个数"，多线程只会让其余线程空转；② NV 的 task 上限是 128（§19.11），1 恒合规。
[numthreads(1, 1, 1)]
void main(uint drawID : SV_DrawIndex, uint3 groupID : SV_GroupID)
{
    MdiShadowPayload p;
    p.drawID = drawID;
    p.slot = groupID.x;
    // 恒发 1 个 mesh group：mesh 组数由行表 gcX 决定（= task 数），一一对应，见文件头。
    DispatchMesh(1u, 1u, 1u, p);
}
