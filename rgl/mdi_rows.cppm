export module mdi_rows;

import rlstd;

/**
 * mdi_rows —— 真 MDI 阴影段的**行表构建器**（docs/mdi_plan.md §19.3 ★M-0，rlbase，纯 CPU 无设备）
 *
 * ## 它解决的那一件事（§19.2 D1）
 * 阴影段簇路径下每级联本来有**一堆逐候选 draw**（`emitShadowCasters` 逐候选 × 逐级发
 * `DrawCall`）。真 MDI 把它们换成**一次** `drawMeshTasksIndirectEXT`：每候选一行 24B 参数，
 * GPU 逐行读自己的 `groupCountX`（spike 的 `per_draw_groupcount` 已实测）。本模块只做
 * 「候选序列 → 行数组 + 聚合锚」这一步纯 CPU 换算，**零消费者**（★M-1 才接 GPU 载体与 shader）。
 *
 * ## 行布局（§19.2 D1，逐字节照做）
 * ```
 * 行 i = [ gcX | 1 | 1 | objectIndex | clusterStart | 0 ]   共 24B = 6 word
 *         └──── 前 12B 恰是 VkDrawMeshTasksIndirectCommandEXT ────┘└ shader 读 word[3]/word[4] ┘
 * ```
 * 为什么 24 不是 20：`clusterStart` 逐 object 不同（LOD 回落）⟹ 16B 的单个 word[3] 装不下两个
 * 索引；24 = 8 的倍数（对齐友好）+ 留**一枚**保留 word 给 ★M-3（模型段那一维）。代价 = 比 20B
 * 每级联多 6KB（1591 行 × 4B）。🔴 计划正文的 ASCII 图把保留位画成三枚 0（那是 32B），与同节
 * 标题「行布局 = 24B」、D3 的 `drawMeshTasksIndirectEXT(…, 24)` 与「候选数 × 24B × 3 级联 ≈
 * 114KB」三处冲突 ⟹ 本模块按 **24B** 落地（三处对一处），差异已写进交付报告。
 * 🔴 `cascadeIndex` **不进行、走 push**（一次 MDI 调用 = 一个级联 = 一个常量），故
 * `ShadowMeshletDrawArgs` 的 12B 契约与执行器通用 push 路径一字节不动。
 *
 * ## 🔴 前 12B 的对账**不在本模块**做（rlbase 不许 import rlvk）
 * 分层护栏（`tools/checks/rules_layers.py`）把「rlbase 整层无设备依赖」变成机器断言，所以这里
 * 只有裸 `uint32_t` 字段、没有 `VkDrawMeshTasksIndirectCommandEXT` 这个名字。"前 12B 恰是该
 * 结构"这条由 **tests/test_mdi_rows** 做第二钉（`tests/` 链全部四层库，可以 `import rlvk;`）——
 * 计划里写的「`static_assert` + 用例**双钉**」= 本文件的 24B/stride 断言 + 测试里的逐字段偏移对账。
 *
 * ## 🔴 task 槽数不在本模块乘
 * `taskCount` 由**调用方算好传进来**，本模块只搬运。唯一真相源 =
 * `render_list_impl.cppm::meshletTaskCount`（`meshlet_lod.cppm` 文件头已为这条立过规矩：那里
 * 不乘、这里也不乘，第三个消费者出现时仍然只有一处乘）。本模块出现槽数常量 = 造第二个真相源。
 *
 * ## 行序 = 输入候选序，逐元素对应（**这是契约，不是实现细节**）
 * 「逐位不变」的前提就是这条：★M-1 的 A3 主判据是「MDI 形态 vs 逐条 draw 形态终图逐位相同」，
 * 而 depth-only 图集的 min 语义只在**同一覆盖集**上才与图元序无关。行序一旦变成「按可见性压紧」
 * 或「按 objectSlot 重排」，那条判据立刻失去依据。压紧/前缀和那笔代价本刀**不付**（§19.2 D4：
 * spike 的 `zero_groupcount_noop` 实测 0 计数不跳号 ⟹ 孔洞流合法）。
 *
 * ## 不可见候选写 `gcX = 0`，**其余字段照写**
 * 身份字段（`objectIndex` / `clusterStart`）在不可见行上照样落值：红了要能定位是**哪一行**，
 * 而「整行清零」让行表失去可归因性（且与「身份字段与可见性无关」这条 §19.2 D4 口径一致）。
 *
 * ## 四条不变量 = 构建器/对账器的断言，每条一个**两两不同**的错误串
 * 前缀 `mdi rows: invariant N`（读日志即可归因，不必回来读代码；同 `meshlet_ranges` /
 * `meshlet_lod` 的纪律）。任务书要求「至少覆盖」三条（下表 1-3），第 4 条是**行序契约**本身——
 * 它不在聚合量上留痕迹（两行互换 ⟹ 三个锚一位不变），不钉就等于没写。
 *
 * | # | 不变量 | 不成立时意味着 |
 * |---|---|---|
 * | 1 | 行数 == 候选数（且 `rowCount` 锚 == 行数）| 构建器自己漏行/多行，或有人给行表做了压紧 |
 * | 2 | **可见**候选的 `taskCount > 0` | 上游契约破了（`meshletTaskCount(0)` 或选级表 count 为 0 却仍判可见）；宁可报错也不发一条「可见但零 task」的行 |
 * | 3 | 聚合锚自洽：`Σ gcX` == 逐行求和、`emptyRows` == 行数中 `gcX==0` 者、且后者 == **不可见候选数** | 锚与行分家 = ★M-1 的 `mdi.rows` 闭式锚失效，收益判据读不到数 |
 * | 4 | 行 i 恰是候选 i 的行（逐元素：gcX / objectIndex / clusterStart）| 行序不再是候选序（排序/互换/错位），A3 的逐位论证失去依据 |
 *
 * 不变量 2 只在**构建器**判（它是输入契约）；1/3/4 住在 `validateMdiShadowRows`——构建器返回前
 * 跑它（自检），★M-4「GPU 写行表 vs CPU 构建器逐元素对账」也跑它（同一份判据，两处真相源迟早漂）。
 *
 * ## 为什么在 rlbase
 * 判据 = 「这段逻辑需不需要一个 VkDevice 才能跑」：不需要（只读一段候选 span）⟹ rlbase，
 * `just test`（无 GPU 无窗口）可钉。形状照 `meshlet_lod` / `meshlet_ranges`（同为纯 CPU 扫描器，
 * 纯搬运**不携带 Vulkan 布局身份**）。
 */

namespace RGL {

/**
 * 一行 MDI 参数（§19.2 D1 的 24B 行；前 12B 与 `VkDrawMeshTasksIndirectCommandEXT` 逐字段同）。
 *
 * 字段名取 Vulkan 那个结构的名字（`groupCountX/Y/Z`）而不是「taskCount」之类的别名：GPU 读的就是
 * 那三个字段，改名只会让「前 12B 恰是该结构」这条对账多一次翻译。Y/Z 恒 1（§19.1：簇路径的
 * 派发是一维的），故它们只在默认值里出现一次。
 */
export struct MdiShadowRow {
	/** 本行的 task 数（= 调用方给的 `taskCount`）；🔴 不可见候选 = 0（GPU 空转，spike 实测不跳号）。 */
	uint32_t groupCountX = 0u;
	uint32_t groupCountY = 1u;  ///< 恒 1（一维派发）
	uint32_t groupCountZ = 1u;  ///< 恒 1（一维派发）
	/** = `MeshRef.objectSlot` = shader 的 objectIndex（word[3]，shader 直读）。 */
	uint32_t objectIndex = 0u;
	/**
	 * 🔴 **相对 `ObjectClusterRange::base` 的偏移**，不是全局簇下标（口径 = `MeshletLodPrimRange::start`
	 * / `ShadowMeshletDrawArgs::clusterStart`）。shader 侧 `cluster = base + clusterStart + 半区`，
	 * base 由 `readObjectClusterRange(objectIndex)` 提供；传全局下标 = base 被加两遍 =
	 * 「同资产多 prim / 多实例」上画错几何。
	 */
	uint32_t clusterStart = 0u;
	/**
	 * 保留 word（§19.2 D1：24B = 20B 载荷 + 本 word，留给 ★M-3 模型段那一维）。现在恒 0，
	 * 非 0 = 有人抢跑 ★M-3。🔴 只有**一枚**：24B 装得下 6 个 word，前 5 个已被上面占用
	 * （计划正文 ASCII 图里的三枚 0 与「留 3 个保留 word」按 24B 口径收敛成一枚，见交付报告）。
	 */
	uint32_t reserved0 = 0u;
};
static_assert(sizeof(MdiShadowRow) == 24,
	"MdiShadowRow layout drifted from docs/mdi_plan.md §19.2 D1 (24B MDI row: the first 12B "
	"must stay exactly VkDrawMeshTasksIndirectCommandEXT)");
static_assert(alignof(MdiShadowRow) == alignof(uint32_t),
	"MdiShadowRow must stay a plain uint32 word array (no padding, no over-alignment)");

/**
 * MDI 行步长（VUID-07088 里的 `stride`：必为 4 的倍数且 ≥ `sizeof(VkDrawMeshTasksIndirectCommandEXT)`
 * = 12）。与 `sizeof(MdiShadowRow)` **同一个常量**（不是抄一份 24）：行表是「按 stride 逐行跳」的
 * 裸字节算术，两处各写一遍 ⟹ 有人给行结构加字段时只改一处 = GPU 逐行读歪，画面表现为「随机少画
 * 一块几何」且没有任何错误信号。合法性（4 的倍数且 ≥ 12）另有一条用例钉（tests/test_mdi_rows）。
 */
export constexpr uint32_t kMdiShadowRowStride = static_cast<uint32_t>(sizeof(MdiShadowRow));
static_assert(kMdiShadowRowStride == 24,
	"kMdiShadowRowStride is no longer 24 — §19.2 D1 fixed the row layout at 24B (first 12B must "
	"stay exactly VkDrawMeshTasksIndirectCommandEXT)");

/**
 * 一个候选（一个级联内的一条阴影投影体候选，字段口径 = `ShadowCandidate` 的可切那半）。
 *
 * 🔴 **本结构不携带 task 槽数口径**：`taskCount` 是「本行要发多少 task」的**成品数**，由调用方
 * 经 `meshletTaskCount(簇数)` 算好（唯一真相源，文件头）。构建器只搬运它。
 */
export struct MdiShadowCandidate {
	/** = `MeshRef.objectSlot`（进 word[3]，与 `ObjectClusterRange` 等 per-object 表同一条索引）。 */
	uint32_t objectSlot = 0u;
	/** 本候选的 task 数（成品数，🔴 不在本模块乘槽数）；可见时必须 > 0（不变量 2）。 */
	uint32_t taskCount = 0u;
	/** 🔴 相对 `ObjectClusterRange::base` 的偏移（进 word[4]，与 push 里的同值同源）。 */
	uint32_t clusterStart = 0u;
	/** 本候选在本级联是否落进光空间体（`casterInShadowVolume` 的结论）。 */
	bool visible = true;
};

/**
 * 一次构建的产物：行数组 + 三个聚合锚。
 *
 * 三个锚不是便利品，是**判据**（§19.3 ★M-0 的「聚合锚」= §19.2 D5 的 profiler 口径原料）：
 * `rowCount` 是 ★M-1 判据 A5 里 `mdi.rows` 的闭式锚，`sumGroupCountX` 是「MDI 形态总共发了多少
 * task」的对账量（与逐条 draw 形态的 Σ groupCount 必须相等），`emptyRows` 是「孔洞流」的规模。
 * 三者与行数组的自洽 = 不变量 3。
 */
export struct MdiShadowRowTable {
	/** 行数组，**下标 = 候选下标**（行序 = 候选序，文件头契约）。 */
	std::vector<MdiShadowRow> rows;
	/** Σ `groupCountX`（含 0 行；== 逐条 draw 形态的 Σ task）。 */
	uint64_t sumGroupCountX = 0;
	/** `groupCountX == 0` 的行数（= 本级联不可见候选数）。 */
	uint64_t emptyRows = 0;
	/** 行数（== 候选数，不变量 1）。 */
	uint64_t rowCount = 0;
};

/**
 * 候选序列 → 24B 行数组 + 聚合锚（§19.3 ★M-0 的唯一入口）。
 *
 * 一趟线性扫（真产物一个级联 ≤ 候选数 1591 量级，纳秒级/行），★M-1 里每帧每级联调一次。
 * 无堆外副作用、不碰设备、不读配置——门 `meshlet.mdi_shadow` 的**消费**在 ★M-1（本批零消费者）。
 *
 * @param cands 一个级联内的候选序列（顺序即行序；空序列 = 合法的空表，不是错误）
 * @return 行数组 + 聚合锚；任一不变量不成立 → 以 `mdi rows: invariant N …` 开头的可归因错误串
 *         （各条前缀两两不同）
 */
export [[nodiscard]] std::expected<MdiShadowRowTable, std::string> buildMdiShadowRows(
	std::span<const MdiShadowCandidate> cands);

/**
 * 行表与它声称的那份候选清单**逐元素**对账（不变量 1 与 3；错误串与构建器同源）。
 *
 * 两个消费者，一份判据：① `buildMdiShadowRows` 返回前自检；② ★M-4「GPU 直写 `gcX`」与 CPU
 * 构建器逐元素对账（§19.3 那一行的「照 `meshlet_hand` 的『序，不是计数』口径」）。
 * 判据面 = 行数 / 逐行 `gcX` 与身份字段 / 三个聚合锚，**不**只看聚合量——GPU 版将来要逐元素
 * 对得上，序本身就是契约的一部分（`test_meshlet_lod` 的 base-shift 教训：聚合量抓不到错位）。
 *
 * @param table 待验行表（行数组 + 锚）
 * @param cands 它声称来自的那份候选清单
 * @return nullopt = 自洽；有值 = 首个可归因错误串
 */
export [[nodiscard]] std::optional<std::string> validateMdiShadowRows(
	const MdiShadowRowTable& table, std::span<const MdiShadowCandidate> cands);

}  // namespace RGL
