export module meshlet_lod;

import rlstd;
import meshlet_asset;
import meshlet_ranges;

/**
 * meshlet_lod —— 从簇产物扫出**逐 primitive 的选级簇区间**（簇化轴 §18 远级联 LOD 第一刀 · 批 A，
 * rlbase，纯 CPU 无设备）
 *
 * ## 它解决的那一件事（docs/mdi_plan.md §18.2 / §18.6）
 * 阴影段走 LOD = 每 object 画 `min(D, maxD_自己)` 的**单一整级**。产物簇表排序 =
 * (primitive, DAG 级, group 序)、每簇恰在其出生级输出一次 ⟹ **同一 prim 内每一级是连续子区间**
 * （§18.2 事实基线，真产物 1591/1591 复核）。本模块把「prim 的第 D 级」扫成
 * `(start, count, level, triangles)`——start 是**相对 `MeshletPrimRange::base` 的偏移**，
 * 批 B 直接塞 push constant，shader 侧 `cluster = base + start + (groupID.x >> 1)`
 * （§18.3 扩法 ①：零 scene data 变更）。
 *
 * ## 选级语义（照 §18.2 实测事实，不是发明）
 * - 某簇的级 = `asset.groups[cluster.group].depth`（int32，0 = L0）；判据住在 group，不住簇。
 * - 该 prim **存在**级 `levelD` → 用它；否则按回落策略：
 *   `deepest` = 本 prim 内 ≤ levelD 的**最深存在级**（levelD 超过自身最深级时即最深级——
 *   §18.2 的成本主导项：561/1591 个 object 停在 L0，回落级比 D 选几更重要）；
 *   `l0` = 级 0（§18.2 对照臂：D=7 时它比现状全量重画还贵 34.97%，就是给这张表留的读数口）。
 *
 * ## 🔴 五条产物结构不变量 = 扫描的断言，同时是「打包工具漂了」的探测器
 * 读日志即可归因：每条一个**两两不同**的错误串，前缀 `meshlet lod: invariant N`。
 * 前置：`ranges` 必须是**同一份 asset** 过 `scanClusterRanges` 的产物（区间越界另有点名错误串）。
 *
 * | # | 不变量 | 不成立时意味着 |
 *|---|---|---|
 * | 1 | 视图大小 == header 计数（clusters/groups 各钉一遍，照 `scanClusterRanges` 的做法）| 调用方绕过读取侧喂了不自洽的视图 |
 * | 2 | 每 prim 的簇区间内 `depth` **单调不减**（=「级内连续」，§18.2 明写要补的那条）| 打包工具排序口径变了，级内子区间不再存在 |
 * | 3 | 每 prim 的 `depth==0` 连续段长度 == `ranges.perPrim[p].l0Count` | 🔴 两条**独立 L0 口径**（`refined == -1` 与 `groups[].depth == 0`）分家 = 工具语义变了 |
 * | 4 | 每 prim（有簇者）至少一个 L0 簇 | `l0` 回落策略失去定义域 |
 * | 5 | 选中级非空 ⟺ 该 prim 有簇 | 选级逻辑与区间扫描自相矛盾（本扫描器自己的哨兵）|
 *
 * 检查顺序 = 1 → 逐 prim（区间越界 → 2 → 3 → 选级 → 5 → 4）。5 排在 4 前是刻意的：
 * `l0` 策略对无级 0 段的 prim 交出空选，先由 5 点名「选级与区间矛盾」，prim 缺 L0 的根因
 * 由 4 在其余形态下点名——两条各有一条经真读取路径可达的反向用例（tests/test_meshlet_lod）。
 *
 * ## 无簇 prim（`ranges.perPrim[p].count == 0`）不是错误
 * 输出 `{0, 0, 0, 0}`——与 `ClusterRangeTable` 的「无簇产物实例 = 全 0 条目 = 天然零 group」同形。
 *
 * ## costTable = §18.2 那张成本表的常驻机器版
 * 对**每个候选 D**（0..maxLevelInProduct）按**当前回落策略**重算全表 clusters/triangles。
 * §18.6 判据 6 要求把一次性读数变启动日志常驻输出——批 B 把它打进 INFO，与 §18.2 表对账。
 * 自洽契约：`costTable[levelD] == (drawnClusters, drawnTriangles)`（同一选择器，两处真相源
 * 迟早漂 ⟹ 只算一处、另一处取行）。
 *
 * ## 🔴 maxClustersPerDraw **不乘 task 槽数**
 * task 数的单一真相源 = `render_list_impl.cppm::meshletTaskCount`（`kSlotsPerCluster ×` 的口径
 * 住在那里，§18.6 明写「那条漂移红线注释要扩到第三个消费者」）。本模块若再写一遍 `2×` 就是
 * 第三个真相源——批 B 的 `emitShadowCasters` 调 `meshletTaskCount(本字段)`，别在这里乘。
 *
 * ## 为什么在 rlbase
 * 判据 = 「需不需要一个 VkDevice 才能跑」：不需要（只读 clusters/groups 两个 span）⟹ rlbase，
 * `just test`（无 GPU 无窗口）可钉。形状照 `meshlet_ranges`（同为簇产物的纯 CPU 消费者，
 * 纯扫描**不携带布局身份**：转换点 = 批 B 装配层一处赋值）。
 */

namespace RGL {

/** 回落策略（D 超过某 prim 自身最深级时怎么办）。 */
export enum class MeshletLodFallback : uint8_t {
	deepest = 0,  ///< 回落本 prim 内 ≤ D 的最深存在级（§18.2 数据倾向的默认）。
	l0 = 1,       ///< 回落级 0（§18.2 对照臂：D 大时比现状全量重画还贵）。
};

/**
 * 单 prim 的选级结果（批 B 的 push constant 原料；无簇 prim = 全 0）。
 *
 * 与 `MeshletPrimRange` 的差别：那是**区间**（绝对 base），这是**选级**（相对 base 的偏移）——
 * 两者拼起来才是 shader 的簇下标，缺一个都会错在「同资产多 prim / 多实例」的形态上。
 */
export struct MeshletLodPrimRange {
	/** 相对该 prim 簇区间起点（`MeshletPrimRange::base`）的偏移，**不是**全局簇下标。 */
	uint32_t start = 0;
	/** 本级簇数（0 = 该 prim 无簇）。 */
	uint32_t count = 0;
	/** 实际选中的 DAG 级（回落时 ≠ requestedLevel）。 */
	uint32_t level = 0;
	/** 本级三角合计（Σ triCount；单 prim 上界 = 该 prim L0 三角 < 2^32，bistro 实测远小于）。 */
	uint32_t triangles = 0;
};

/** 成本表一行（某个候选 D 的全表代价，§18.2 那张表的常驻机器版）。 */
export struct MeshletLodCostRow {
	/** 候选 D（行号 == 本值）。 */
	uint32_t level = 0;
	/** 该 D 下全表被画簇数。 */
	uint64_t clusters = 0;
	/** 该 D 下全表被画三角数（「占 L0」= 本值 / l0Triangles，批 B 打日志时算）。 */
	uint64_t triangles = 0;
};

/**
 * 一次扫描产物（逐 prim 选级 + §18.6 判据 6 要的启动期对账量）。
 *
 * 合计量不是便利品，是**判据**：批 B 把它们打进启动日志与 §18.2 实测对账
 * （bistro：l0Clusters = 27,487 / l0Triangles = 2,832,120 / D=4 ≈ 3,670 簇 7.88% /
 * D=5 ≈ 2,566 簇 5.10%）——合成用例只验逻辑，验不出真产物形态。
 */
export struct MeshletLodScan {
	/** 逐 prim 选级，**下标 = prim**（长度 == `ranges.perPrim.size()`）。 */
	std::vector<MeshletLodPrimRange> perPrim;
	/** 请求级 D（回显）。 */
	uint32_t requestedLevel = 0;
	/** 回落策略（回显）。 */
	MeshletLodFallback fallback = MeshletLodFallback::deepest;
	/** Σ 被画簇数（== costTable[requestedLevel].clusters）。 */
	uint64_t drawnClusters = 0;
	/** Σ 被画三角数（== costTable[requestedLevel].triangles）。 */
	uint64_t drawnTriangles = 0;
	/** Σ 各级产物里的 L0 簇数（`depth==0` 口径；不变量 3 把它与 `ranges.totalL0Clusters` 钉死）。 */
	uint64_t l0Clusters = 0;
	/** Σ L0 三角数（真产物 bistro = 2,832,120 = header.l0TriangleCount，闭式锚）。 */
	uint64_t l0Triangles = 0;
	/** 产物里实际出现过的最深级（costTable 行数 = 本值 + 1）。 */
	uint32_t maxLevelInProduct = 0;
	/** 本次选级实际用到的最深级（≤ max(maxD_自己, D) 内的实际值）。 */
	uint32_t maxLevelUsed = 0;
	/** 因 D 超过自身最深级而回落的 prim 数（§18.2：D=5 实测 1,212/1,591）。 */
	uint64_t fallbackPrimCount = 0;
	/** 单 draw 最大簇数。🔴 不乘 task 槽数——task 数唯一真相源 = meshletTaskCount（文件头）。 */
	uint32_t maxClustersPerDraw = 0;
	/** 每级被选中的 prim 数（size == header.levelCount；无簇 prim **不计数**，和 == 有簇 prim 数）。 */
	std::vector<uint64_t> levelHistogram;
	/** D = 0..maxLevelInProduct 的全表代价（按**当前** fallback 策略算，§18.2 表的机器版）。 */
	std::vector<MeshletLodCostRow> costTable;
};

/**
 * 扫出逐 prim 选级区间并校验五条产物结构不变量（`meshlet_ranges` 的区间表 → 选级表）。
 *
 * 一趟线性扫 clusters（真产物 61,518 条，微秒级）+ 每 prim 段表上对每个候选 D 重算成本表
 * （O(prim × 级数)，常驻输出的一次性代价）；只在装配期跑一次，不进帧循环。
 *
 * @param asset 经 `parseMeshletAsset` 校验过的只读视图（本函数不重复读取侧校验，但自己钉一遍
 *              不变量 1 的「视图大小 == header 计数」——两侧不一致时错的是静默的选级）
 * @param ranges **同一份 asset** 过 `scanClusterRanges` 的产物（区间越界 = 点名错误串，不静默）
 * @param levelD 请求的 DAG 级（0 = 恒 L0 档，与 §10.1 的 `(base, l0Count)` 档同读数）
 * @param fallback D 在该 prim 不存在时的回落策略
 * @return 逐 prim 选级 + 对账量；任一不变量不成立 → 以 `meshlet lod: invariant N …` 开头的
 *         可归因错误串（五条前缀两两不同）
 */
export [[nodiscard]] std::expected<MeshletLodScan, std::string> scanMeshletLod(
	const MeshletAsset& asset, const MeshletRangeScan& ranges,
	uint32_t levelD, MeshletLodFallback fallback);

}  // namespace RGL
