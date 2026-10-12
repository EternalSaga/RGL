export module meshlet_ranges;

import rlstd;
import meshlet_asset;
import scene_forward_types_generated;  // ObjectClusterRange（scene data 表条目，反射生成）
import meshlet_cull_types_generated;   // MeshletCullItem（★1-a2-i 剔除项，反射生成，🔴 勿在此手写）

/**
 * meshlet_ranges —— 从簇产物扫出**逐 primitive 的簇区间**（簇化轴 ★1-a1i，rlbase，纯 CPU 无设备）
 *
 * ## 它解决的那一件事（docs/mdi_plan.md §10.1）
 * 产物里的簇是**全局一张表**，而提交是**逐 primitive** 的（材质绑定原样不动）。mesh shader 要
 * `clusterBase`（去索引发射后是 `base + (groupID.x >> 1)`，每簇 2 槽；α″ 前曾是
 * `groupID.x + base`）。本模块把 `MeshletAsset` 的 groups 视图扫成
 * 「按 prim 序的连续分块」——每 prim 一个 `[base, count)` 加上该 prim 的 **L0 前缀长度**，
 * 装配层把它写进 scene data 尾区的 `ObjectClusterRange` 表（stride 16，按 objectIndex 索引）。
 *
 * ## 🔴 四条产物结构不变量 = 扫描的断言，**同时**是「打包工具漂了」的探测器
 * 主 agent 2026-10-12 在真产物（bistro：1591 prim / 61,518 簇 / 7,596 group）上实测四条全部成立。
 * 任何一条不成立 ⟹ 工具口径变了 ⟹ **返回可归因错误**（装配层 WARNING + 回落现行路径），
 * 绝不静默交出错误区间：区间错在图像上是「少画一块 / 画错一块」，没有任何错误信号。
 *
 * | # | 不变量 | 真产物实测 | 不成立时意味着 |
 * |---|---|---|---|
 * | 1 | group 表按 prim 切成**连续分段**，分段数 == primCount | 1591 == primCount | `clodBuild` 不再逐 prim 跑（分段被打散）|
 * | 2 | 每 prim 的簇区间**单一**（其 group 区间首尾相接成一段）| 拆成多段的 prim = 0 / 1591 | 一个 prim 需要多个 `[base,count)`，本表布局装不下 |
 * | 3 | 按 prim 序拼出的区间**铺满** `[0, clusterCount)`，0 重叠 0 空洞 | 是 | 有簇不属于任何 prim（画不到）或被两个 prim 重复画 |
 * | 4 | 每 prim 的 **L0 簇是其区间前缀** | 1591 / 1591 = 100% | 恒 L0 档就不能只看 `(base, l0Count)`，必须上传 Groups 逐簇选级 |
 *
 * 四条各带**各自不同**的错误串（前缀 `invariant N`），失效形态直接打进日志就能归因，不必读代码。
 * 注：**级内连续**（每 prim 区间内 `groups[cluster.group].depth` 单调不减）这条不变量由
 * `meshlet_lod` 扫描负责（错误串前缀 `meshlet lod: invariant 2`，§18.2「要补的那条」）。
 *
 * ## L0 判据 = `cluster.refined == -1`（"产出本簇的更细 group = 无" = 原始几何）
 * 真产物上该口径与「所属 group 的 `depth == 0`」两条**给出同一个合计 27,487**（★1-a1i 实测），
 * 取前者是因为它是 `MeshletDiskCluster` 注释里 L0 的定义，不依赖 group 视图的二次跳转。
 * 🔴 **恒 L0 档只要 `(base, l0Count)`**：全局 L0 簇**不是** `[0, 27487)` 前缀，而是**逐 prim 前缀的并**
 * ——别拿全局前缀偷懒（§10.1 明写）。
 *
 * ## 为什么在 rlbase
 * 判据 = 「这段逻辑需不需要一个 VkDevice 才能跑」：不需要（只读 groups/clusters 两个 span）⟹
 * 落 rlbase，能在 `just test`（无 GPU 无窗口）里测。形状照 `meshlet_cull`（同为簇产物的纯 CPU 消费者）。
 */

namespace RGL {

/**
 * 一个 primitive 的簇区间（扫描的输出条目；三个数都要，用途不同）。
 *
 * 与 scene data 表条目 `ObjectClusterRange` 的差别只有末尾的 `reserved`（★2 选级预算的补齐位）——
 * 本模块是**纯扫描**，不携带 scene data 的布局身份；转换点 = 装配层一处赋值。
 */
export struct MeshletPrimRange {
	/** 该 prim 的簇区间起点（全局簇下标）。 */
	uint32_t base = 0;
	/** 区间长度（**全部 DAG 级**；恒 L0 档只用前 `l0Count` 条）。 */
	uint32_t count = 0;
	/** 区间**前缀**里的 L0 簇条数（不变量 4 保证 L0 恰为前缀）。 */
	uint32_t l0Count = 0;
};

/**
 * 一次扫描的产物：逐 prim 区间 + 两个统计锚。
 *
 * 两个合计不是便利品，是**判据**：装配层把它们打进启动日志（`prims / totalClusters / l0Clusters`），
 * 与 `docs/mdi_plan.md` §10.1 的实测值（1591 / 61518 / 27487）对账——这是「扫描实现与真产物结构
 * 对得上」的唯一常驻证据（合成用例只验逻辑，验不出真产物的形态）。
 */
export struct MeshletRangeScan {
	/** 逐 prim 区间，**下标 = prim**（长度 == `header.primCount`，不变量 1 保证）。 */
	std::vector<MeshletPrimRange> perPrim;
	/** Σ `count`（== `header.clusterCount`，不变量 3 保证）。 */
	uint64_t totalClusters = 0;
	/** Σ `l0Count`（真产物 bistro = 27,487）。 */
	uint64_t totalL0Clusters = 0;
};

/**
 * 扫出逐 prim 簇区间并校验四条产物结构不变量（`meshlet_asset` 的只读视图 → 区间表）。
 *
 * 一次线性扫 groups（7,596 条）+ 一次线性扫 clusters（61,518 条，只为 L0 前缀计数），微秒级；
 * 只在装配期跑一次，不进帧循环。
 *
 * @param asset 经 `parseMeshletAsset` 校验过的只读视图（本函数**不重复**读取侧的段表/范围校验，
 *              但自己钉一遍「视图大小 == header 计数」——两侧不一致时区间错是静默的）
 * @return 逐 prim 区间 + 统计锚；任一不变量不成立 → 以 `meshlet ranges: invariant N …` 开头的
 *         可归因错误串（四条前缀两两不同）
 */
export [[nodiscard]] std::expected<MeshletRangeScan, std::string> scanClusterRanges(
	const MeshletAsset& asset);

/**
 * per-objectSlot 的簇区间表（**registry ctx**，装配层启动期填一次，`renderListSystem` 每帧
 * 只搬 span 进 `SceneFrameData::clusterRanges`）。
 *
 * 为什么走 ctx 而不是塞进 `RenderListState`：state 由帧循环（`renderSystem`）自己构造，装配层
 * 拿不到；而 ctx 是「装配层 → system」的既有唯一通道（`SceneEnv` / `SsrTuning` / `WetTuning`
 * 同路，ECS 规范 1：状态进 context，不在 registry 外平行维护）。
 *
 * 下标 = objectSlot（= `MeshRef.objectSlot` = shader 的 `objectIndex`），容量 = 实例矩阵槽数；
 * 无簇产物的实例留**全 0 条目**（`count = 0` ⟹ ★1-a1 的 mesh shader 天然零 group）。
 * 门 `meshlet.enable` 关 = 装配层根本不建本 ctx ⟹ span 空 ⟹ scene data 该区保持全 0。
 */
export struct ClusterRangeTable {
	/** 按 objectSlot 索引的表条目（与 `ObjectDraw` 等三张 per-object 表同一条索引）。 */
	std::vector<ObjectClusterRange> bySlot;
};

/**
 * 逐簇剔除项表（★1-a2-i，registry ctx，装配层启动期建一次）。
 *
 * 元素 `MeshletCullItem` 是**反射生成物**（`meshlet_cull.slang` → `generated/meshlet_cull_types_generated.cppm`，
 * clusterIdx@0 / objectSlot@4，sizeof 8 由生成物里的 static_assert 钉住），本模块只持有它——
 * 🔴 布局不在这里手写第二份（compute 契约走反射是红线，见 meshlet_cull.slang 头注释）。
 *
 * 为什么一个剔除项是「(簇, 实例) 对」而不是只存簇下标：同一个资产的簇会被多个 objectSlot 共享，而
 * 剔除判据里的世界变换来自**实例**（对象矩阵表按 objectSlot 索引）。只存簇下标 ⟹ 剔除器不知道该用
 * 哪个实例的矩阵。8B 一项 × 27,487 项 ≈ 220 KB，启动期一次性上传（同 `GpuCull` 的 cullTable 形态）。
 *
 * 与 `ClusterRangeTable` 的关系 = **同一批数据的两种视图**：区间表给 mesh shader 按 objectIndex
 * 查「我的簇从哪开始」，本表给剔除器**线性铺开**逐簇跑（GPU 上按 object 分段的二维派发不划算）。
 * 两者由 `buildClusterCullItems` **一次同时产出**（cullBase 与 items 的下标关系必须同生共死，
 * 分两处算 = 迟早漂）。
 */
export struct MeshletCullItemTable {
	/**
	 * 剔除项，**objectSlot 升序**、同 object 内簇内序升序（= `cullBase` 前缀和的展开序）。
	 * 字段口径：`clusterIdx` = 全局簇下标（= 该 object 的 `ObjectClusterRange.base` + 簇内序，
	 * 读簇包围球用）；`objectSlot` = 实例矩阵槽（= shader 的 `objectIndex`，读对象矩阵用）。
	 */
	std::vector<MeshletCullItem> items;
};

/**
 * 建逐簇剔除项表，并**就地**把 `cullBase` 写进 `table.bySlot`（★1-a2-i）。
 *
 * ## 下标空间这件事是本函数的全部风险，所以口径写死
 * 剔除标志数组按**剔除项下标**索引；mesh shader 侧要能算出同一个下标，公式是
 * `flag[range.cullBase + 簇内序]`。于是 `cullBase` 必须满足：
 * **按 objectSlot 升序的 `l0Count` 前缀和**。选 slot 升序而不是别的序，理由只有一个：
 * 它是 mesh shader 侧**唯一能复现**的序（shader 只有 objectIndex，没有别的可比量）。
 *
 * 🔴 为什么不用 `base + 簇内序` 当下标：`base` 是全局簇下标，同资产被两个 object 复用时两者
 * 相同 ⟹ 撞同一标志槽 = 一个对象的可见性替另一个决定 = **几何凭空消失**（误剔无错误信号）。
 *
 * 无簇产物的槽（`l0Count == 0`）不占剔除项，但**照样占号**（前缀和天然跳过 0 长度段），
 * 所以 mesh shader 的 `cullBase + 簇内序` 对任何槽都不会读到别人的标志。
 *
 * @param table 已装配完的区间表（本函数写其 `cullBase` 字段，其余字段只读）
 * @return 剔除项表，`items.size() == Σ l0Count`
 */
export MeshletCullItemTable buildClusterCullItems(ClusterRangeTable& table);

/**
 * 簇路径「opaque 段原位取代」的**生效**标记（★1-a1ii-β-1，`meshlet.switch`）。
 *
 * 住在 registry ctx 的理由与 `ClusterRangeTable` 完全同款——**同一个生产者/消费者一对**：
 * 装配层 `wireMeshletSwitch` 在启动期判定并 emplace，`renderListSystem` 每帧 find 只读。
 * 「生效」这件事**只判定一次**（`enable && switch && cull_replay == cpu && 逐资产对账通过`），
 * 消费者重述条件 = 两处真相源迟早漂（本项目的反复教训）。
 *
 * **存在即生效**（不 emplace = 不切）：与 `ClusterRangeTable` 的「门关 = ctx 无条目 = 执行器
 * 看到空 span」同构，门关路径连一个字节都不写进 ctx（逐位不变的构造性保证）。
 * 无字段 = 纯标记；本结构体不携带数据，簇区间仍从 `ClusterRangeTable` 取。
 */
export struct MeshletSwitchOn {};

}  // namespace RGL
