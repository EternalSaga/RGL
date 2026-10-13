// shadow_meshlet.task.slang —— 阴影段真 MDI 的 **task 入口**（docs/mdi_plan.md ★M-3T-a / ★M-3T-c）
//
// ## 它干两件活
//
//  1. **行寻址**：`row = rowBase(push.word0) + SV_DrawIndex`，从 binding 34 的 24B 行表里读出本
//     draw 的身份（`objectIndex` / `clusterStart`）与要发多少 mesh 组（`meshGroupCount`）。
//  2. **一次发完整批**：`DispatchMesh(meshGroupCount, 1, 1, payload)` 把本对象本级联的全部 mesh
//     组发出去，身份经 payload 转交 ⟹ **mesh 侧完全不认识行表**（契约只剩本文件一份 shader 拼写）。
//
// ## 它存在的**原始**理由：把 `SV_DrawIndex` 从 mesh 入口搬到这里
//
// §19.9 实测：AMD Radeon 8060S（RADV STRIX_HALO / gfx1151 / Mesa 26.2.3）上，**mesh** 入口一声明
// `SV_DrawIndex`，同提交里后续 mesh draw 就不产出像素（主视图那栋 bistro 整块消失）。§19.15 又实测：
// 同一个 builtin 声明在 **task** 入口 ⟹ 不触发（主视图 G-buffer albedo/metalrough 逐位不变）。
// ⟹ 本文件把 DrawIndex 的声明处从 mesh 换到 task。门 `meshlet.mdi_shadow` 默认关；门关用的还是
// `shadow_meshlet.mesh.spv` 那份没挂 task 的 SPV（§19.2 D2 的构造性护栏）。
//
// ## 🔴 为什么一个 draw 只发**一枚** task（★M-3T-c；改前是"每 mesh 组一枚"）
//
// MDI 行的 `gcX` 挂上 task 后**单位变成 task 数**。★M-3T-a 的初版让每 task 恰好发 1 个 mesh 组
// ⟹ 数值与旧的 mesh 组数恒等、行表零改动，代价是 task 数 = mesh 组数（实测每级联 5132），
// task 自身开销把 MDI 的命令数收益在 GPU 侧**倒赔**约 0.5ms（§19.16 代价读数）。本批收成每 draw
// 一枚：`gcX = 可见 ? 1 : 0`，mesh 组数进行表 word[5] 由 task 读出来当 `DispatchMesh` 的 X
// ⟹ 每级联 task 数 5132 → 1530 量级（3.4×）。`sumMeshGroupCount`（mesh 组总数）跨形态不变，
// 是"task 化只改谁发、不改发多少"的锚（`rgl/mdi_rows.cppm` 不变量 3b 钉每可见行恰一枚 task）。
//
// ## 🔴 `SV_GroupID` 本批起**不再声明**（★M-3T-a 那版必须有）
//
// spec：mesh 阶段的 `WorkgroupId` = "position within the **current mesh task**"。改前每 task 只发
// 1 组 ⟹ mesh 的 `groupID.x` 恒 0，旧派发契约里它承担的那一维（半区号 + 簇内偏移）必须由 task 的
// `SV_GroupID` 经 payload 转交，否则每个 draw 只画第一个簇。本批每 task 发 `meshGroupCount` 组
// ⟹ mesh 的 `groupID.x` 取值域 = `[0, meshGroupCount)`，**逐值等于**旧的 mesh 组号 ⟹ mesh 直接
// 用自己的 `groupID.x`，与非 MDI 路径回到**同一条算式**。task 再声明 `SV_GroupID` 就只是个恒 0
// 的死输入，删。
//
// ## 行表契约在 shader 侧只剩这一份拼写
//
// 唯一真相源 = `rgl/mdi_rows.cppm`（`MdiShadowRow` + `kMdiShadowRowStride` +
// `kMdiShadowCommandBytes`）。改前 mesh 侧还有一份（步长 + 两个偏移），行寻址搬进来后那份退役。
// 数值对账 = `meshlet_spec_checks`（schema `meshlet_cull_schema.lua constants` ↔ 本文件字面量）；
// 跨语言对账 = `tests/test_meshlet_shadow_args` 的设备自证回路（C++ 写行 / shader 读行逐字节对拍）
// + `tests/test_mdi_shadow_emit` 的 constexpr 桥。**别在这里加第四份形态**（手算常量 / 宏等）。
//
// ## 为什么 push 只声明 4B 而不复述 12B 的 `ShadowMeshletDrawArgs`
//
// task 只读 word0 = 行基址 rowBase。复述整个 12B 块 = 把"另外两个字段的名字与顺序"也变成 task 的
// 契约，而 task 一个都不读；只声明首字段则**只有 word0 的位置**是它的契约（那正是它依赖的全部）。
// push 走 descriptor heap 的 push data set（`vkCmdPushDataEXT`，12B），task 读其 offset 0..4。
// 🔴 word0 在 MDI 变体下语义切换成 rowBase（§19.2 D1）—— 这条切换的 CPU 侧对偶 =
// `frame_draw_list.cppm::ShadowMeshletDrawArgs` + `emitShadowCasters` 填 rowBase 那处。
//
// ## 为什么单独成模块
//
// slang#10123：task 与 mesh 编进同一模块时 payload 变量被**复制成两份**（spirv-val 放行、NVIDIA
// 实测越界读）⟹ 必须独立编译（`tests/shaders/mdi_task_probe.*` 同一条纪律）。
//
// ## 验收看产物，不看"编译过了"
//
// 触发源必须真在 SPV 接口里，否则"没毒化"是必然不是结论：
//     spirv-dis build/rgl/generated_shaders/shadow_meshlet.task.spv | grep -c "BuiltIn DrawIndex"
// 必须 = 1（🔴 注意 spirv-dis 输出是**空格**不是点，写 `BuiltIn.DrawIndex` 恒 0）。★M-3T-a 那版
// 这条判据是 2（DrawIndex + WorkgroupId），本批 WorkgroupId 退役 ⟹ 判据随之改 1，别拿旧数字对。
// DrawIndex 活着不靠人为守卫：它喂 rowBytes → 三次 Load → payload，是真实数据流。

// 🔴 本 struct 是 payload 契约在 task 侧的拼写，mesh 侧（`shadow_meshlet.mesh.slang` 的
// `MDI_SHADOW` 分支）有一份**逐字段同构**的第二份。跨模块无法共享声明（两份 .slang 各编各的）
// ⟹ 改一侧必须同改另一侧，字段序即契约。
struct MdiShadowPayload
{
    uint objectIndex;   // 行 word[3] = MeshRef.objectSlot ⟹ mesh 侧 readObjectClusterRange 的下标
    uint clusterStart;  // 行 word[4] = 🔴 **相对 ObjectClusterRange::base 的偏移**（不是全局簇下标）
};

// task 侧的 push 视图：只有 word0（= 行基址 rowBase），理由见文件头。
struct MdiTaskPush
{
    uint rowBase;
};

[[vk::push_constant]] MdiTaskPush mdiTaskPush;

// 行表载体 = ★M-1a 的 ring×2 一张 buffer（rgl/meshlet_shadow_args），heap 槽由
// MeshletShadowArgsService 领号、装配烘进本条目 binding 34。🔴 声明与 mapping 同向：本批起是
// **task** 声明 34（改前是 mesh），条目级 mapping 取的是各 stage 声明的并集 ⟹ 烘法不变。
[[vk::binding(34, 0)]]
ByteAddressBuffer mdiShadowArgs;

[shader("amplification")]
// 1 线程 = 每 task 恰好一次 DispatchMesh。取 1 有两个独立理由：① 本 task 的活是"读三个 word +
// 发一次"，多线程只会让其余线程空转（★M-3T-c 后仍成立：读的行数与线程数无关，都是 1 行）；
// ② NV 的 task 上限是 128（§19.11），1 恒合规。
[numthreads(1, 1, 1)]
void main(uint drawID : SV_DrawIndex)
{
    // 行表规格常量（口径与对账链 → 文件头；写法 = 函数内 `const uint`，slang 的全局 const 需 static，
    // 照 meshlet.mesh.slang 先例放 main 体内）。
    const uint kMdiShadowRowStrideBytes = 24u;               // = kMdiShadowRowStride（VUID-07088 的 stride）
    const uint kMdiShadowObjectIndexByteOffset = 12u;        // 行 word[3]
    const uint kMdiShadowClusterStartByteOffset = 16u;       // 行 word[4]
    const uint kMdiShadowMeshGroupCountByteOffset = 20u;     // 行 word[5]

    // 🔴 row = rowBase + drawID（§3 的统一公式；push.word0 在 MDI 变体下语义 = rowBase）。
    // 行序 = 候选序（构建器契约）；不可见行 gcX = 0 根本不发射 ⟹ 被调度到的行恒为可见行，
    // 而可见行的 meshGroupCount 必 > 0（构建器不变量 2 保证）⟹ 这里不会 DispatchMesh(0)。
    const uint rowBytes = (mdiTaskPush.rowBase + drawID) * kMdiShadowRowStrideBytes;
    const uint meshGroupCount = mdiShadowArgs.Load(rowBytes + kMdiShadowMeshGroupCountByteOffset);

    MdiShadowPayload p;
    p.objectIndex = mdiShadowArgs.Load(rowBytes + kMdiShadowObjectIndexByteOffset);
    p.clusterStart = mdiShadowArgs.Load(rowBytes + kMdiShadowClusterStartByteOffset);
    // 一次发完整批：mesh 侧 groupID.x 的取值域因此 = [0, meshGroupCount)，逐值等于旧的 mesh 组号。
    DispatchMesh(meshGroupCount, 1u, 1u, p);
}
