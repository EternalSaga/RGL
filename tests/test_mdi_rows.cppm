module;
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>  // VkDrawMeshTasksIndirectCommandEXT（C 名；rlvk 只导出 vk:: 系，先例 test_mdi_drawindex:73）
#include <cstring>          // std::memcpy（行首 12B → Vk 结构）
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <filesystem>        // 临时 lua 落盘（GMF 例外清单成员）
#include <fstream>

/**
 * test_mdi_rows —— ★M-0（docs/mdi_plan.md §19.3）：真 MDI 阴影段的行表构建器（纯 CPU 套件）。
 *
 * 被测对象 = `rgl/mdi_rows.cppm` + `rgl/mdi_rows_impl.cppm`（rlbase）。**不是** `TEST_SUITE("gpu")`：
 * 构建器只读一段候选 span，一个设备都不碰（判据 = 「需不需要一个 VkDevice 才能跑」）。
 *
 * ## 为什么值得测（截图看不出来的那类）
 * ★M-1 的主判据是「MDI 形态 vs 逐条 draw 形态终图**逐位**相同」，而那条判据的前提全在本模块：
 *   · **行序 = 候选序**：一旦有人「顺手压紧」不可见行，depth-only 图集的 min 语义就不再与
 *     逐条 draw 等价 ⟹ A3 红，而红的原因在两个文件之外（最难查的一种红）；
 *   · **前 12B 恰是 `VkDrawMeshTasksIndirectCommandEXT`**：GPU 按裸字节读，字段错位 = 把
 *     objectIndex 当 groupCountX 用 = 发几十万倍 task 或一个都不发；
 *   · **stride 合法性（VUID-07088）**：不合法是 validation error / 未定义行为，不是"画面差一点"。
 * 这三条在门关态的截图里**一条都看不见**（本批零消费者 ⟹ 图必然不变），只能在这里钉。
 *
 * ## 不变量 1/3/4 的反向用例走 `validateMdiShadowRows`，不走构建器（可达性写清楚）
 * 构建器自己产出的行表**按构造**满足 1/3/4（同一趟循环写行与锚）⟹ 那三条在构建器入口上是
 * **哨兵**（防的是"以后有人改坏构建器"，变异 ①②③⑨⑩ 与三条判据短路 ⑤⑦⑧ 实测让它们各自变红）。
 * 要让它们有**可达的**反向用例，激励必须来自外部：行表 + 候选清单两个入参都是调用方给的
 * （★M-4 的 GPU 行表对账就是这个形态），于是导出的 `validateMdiShadowRows` 是它们真正的入口。
 * 不变量 2 相反——它是
 * **输入**契约，只有构建器判（对账器里同形态由不变量 3 的「空行数 vs 不可见候选数」点名，
 * 那条也配了一条手搭行表的反向用例）。
 *
 * ## 🔴 数值纪律：`kMdiShadowRowStride`(=24) 与本文件任何用例的行数/偏移都不相等
 * spike §6.3 的 M4 教训（`test_mdi_drawindex.cppm` 文件头记着全文）：两个数相等 ⟹ 参数序写反
 * 也测不出东西。本文件的行数 = 5 / 4 / 3 / 1、候选数 = 6 / 5、clusterStart = 3/11/13/17/1001/7、
 * 字节偏移 = 0/4/8/12/16/20 ——**没有一个是 24**（12 只作为 VUID 下界与 `sizeof(Vk…)` 出现，
 * 不作为行数/偏移）。
 * 「stride 与这些数都不相等」本身也钉成断言（`strideDistinctFromCaseNumbers`），凑数即红。
 *
 * ## 变异红线声明（哪个变异让我红；**十三条全部实测** 2026-10-13，编号 = harness 的 M01…M13）
 * 方法学四条防线：备份/还原用 `shutil.copyfile`（**不带 mtime**）+ 还原后 `os.utime` 抬 mtime
 * （🔴 禁 `git checkout`/`git stash`：本批三个新文件是**未跟踪**的，`stash -u` 会把它们一起卷走）、
 * 显式 `cmake --build build --target rgl_tests`（tests/ 是 EXCLUDE_FROM_ALL，`just build` 不编它）
 * 并核对**构建自身**的返回码（构建红 ≠ 判据红，那种读数作废）、每条还原后重跑全量基线断言全绿。
 * 下面「红 N 条」= 该变异下失败的 TEST_CASE 条数（本文件共 11 条 TEST_CASE）。
 *
 *   ① 压紧不可见行（`out.rows.push_back(row)` 外套 `if (row.groupCountX != 0u)`）→ 红 **8** 条
 *      （正向锚点 + 行序 + 不可见行身份 + Lcg + 前 12B 对账 + inv2 边界 + 两条反向表）
 *   ② 不可见行整行清零（`rowFor` 里不可见即 `return row`，身份字段一起归零）→ 红 **5** 条
 *      （正向锚点 + 不可见行身份 + 行序 + Lcg + 前 12B 对账）。🔴 **不含任何反向用例**：整行清零
 *      仍留 2 条空行 ⟹ 三个聚合锚一位不变，只有逐字段判据（不变量 4）抓得到
 *   ③ 身份字段错位（`row.objectIndex = c.clusterStart`）→ 红 **6** 条（正向锚点 + 行序 +
 *      不可见行身份 + Lcg + 前 12B 对账 + 全部坏形态）
 *   ④ 可见性判据弱化（`c.visible ? c.taskCount : 0u` → 恒 `c.taskCount`）→ 红 **8** 条（同 ①）
 *   ⑤ `rowCount` 锚判据短路（`&& false`）→ 红 **2** 条（全部坏形态 + 两两不同）
 *   ⑥ 🔴 行数判据**弱化**（`rows.size() != candidateCount` → `>`，只挡多行、放过少行）→ 红 **2**
 *      条（全部坏形态 + 两两不同），且**全部来自形态 A′**（该变异下全树仅失败 2 条断言 = A′ 在这
 *      两条用例里各触发一次 `REQUIRE(err.has_value())`）。形态 A（`pop_back` 不重算锚）**抓不到**
 *      它：漏行时姊妹判据 `rowCount != rows.size()` 会出声且归因正确（invariant 1）。⟹ 只有
 *      `badRowDroppedHonestAnchors`（删**最后一行** + 三个锚按剩下的行**重算** + 补进来的候选可见）
 *      把这条弱化变成**完全静默**——同 `test_meshlet_ranges` ⑫ 同型的「弱化不只测删除」缺口
 *   ⑦ 空行数锚判据短路 → 红 **2** 条（同 ⑤）
 *   ⑧ 逐行对账判据弱化（比较后恒 `continue`）→ 红 **2** 条（同 ⑤）
 *   ⑨ 构建后按 `objectIndex` 排序（"看着更像序"）→ 红 **8** 条（同 ①）
 *   ⑩ `Σ gcX` 只加可见行（锚与行分家）→ 红 **8** 条（同 ①）
 *   ⑪ 配置解析键名打错（`"mdi_shadow"` → `"_mdi_shadow"`，键被静默忽略）→ 红 **1** 条（配置键
 *      用例的「显式命中」半边）——🔴 探针必须取 **true**（≠ 默认 false）才抓得住：探针取默认同值时
 *      「键没读到」与「读到了默认值」不可区分，该变异会全绿（§18.11 纪律）
 *   ⑫ 配置默认值翻成 true → 红 **1** 条（同一条配置用例，内含 4 处断言失败）
 *   ⑬ 不变量 2 判据短路（`visible && taskCount == 0` → `&& false`）→ 红 **3** 条（inv2 只在构建器
 *      + 全部坏形态 + 两两不同）
 *
 * ## 已知覆盖缺口（写出来，别让「全绿」被读成「全验过」）
 *   · 真产物对账（1591 候选 × 3 级联的 Σ gcX 闭式锚）不在这里做：本批零消费者，没有真候选序列
 *     可喂（装配层不产它）。★M-1 接上后由启动日志常驻出声（同 `meshlet_lod` 批 B 的裁决）。
 *   · 「GPU 逐行读自己的 groupCountX」是设备级事实，spike 已实测（`per_draw_groupcount`），
 *     本文件只钉字节布局；跨层那一条 = ★M-1 判据 A3/A4。
 */
export module test_mdi_rows;

import rlstd;
import mdi_rows;         // MdiShadowRow / MdiShadowCandidate / MdiShadowRowTable / build / validate
import runtime_config;   // 配置键 meshlet.mdi_shadow 的解析契约
import test_support;     // Lcg（写死种子，禁真随机）

namespace RGL {
namespace {

/**
 * 正向锚点的候选序列（5 条：3 可见 2 不可见）。
 *
 * 手算期望（出处 = §19.2 D1 的行布局，不抄被测码）：
 *
 * | i | objectSlot | taskCount | clusterStart | visible | ⟹ gcX |
 * |---|---|---|---|---|---|
 * | 0 | 7    | 6  | 3    | true  | 6  |
 * | 1 | 21   | 0  | 11   | false | 0  |
 * | 2 | 19   | 9  | 17   | true  | 9  |
 * | 3 | 1000 | 2  | 1001 | false | 0  |
 * | 4 | 3    | 11 | 7    | true  | 11 |
 *
 * 聚合锚：Σ gcX = 6+9+11 = **26**；空行数 = **2**；行数 = **5**。
 * 三个设计点：① 候选 1 的 `taskCount = 0` 与候选 3 的 `taskCount = 2` 都是不可见 ⟹ 钉「不可见
 * 时不看 taskCount，`visible = false` 说话」；② 每行 objectSlot ≠ clusterStart（字段互换必红），
 * 而候选 3 的 1000 / 1001 只差 1（写错一位也红）；③ 候选 4 的 objectSlot = 3 与候选 0 的
 * clusterStart = 3 同值 ⟹ 「行内字段错位」与「跨行错位」是两种不同的红，互不掩盖。
 */
const std::vector<MdiShadowCandidate>& anchor() {
	static const std::vector<MdiShadowCandidate> cands {
		MdiShadowCandidate { 7u, 6u, 3u, true },
		MdiShadowCandidate { 21u, 0u, 11u, false },
		MdiShadowCandidate { 19u, 9u, 17u, true },
		MdiShadowCandidate { 1000u, 2u, 1001u, false },
		MdiShadowCandidate { 3u, 11u, 7u, true },
	};
	return cands;
}

/** 锚点里第 i 条候选**该**发出的行（期望值 = 上面那张表，逐字段独立写死）。 */
MdiShadowRow expectedRow(size_t i) {
	static constexpr uint32_t gcX [] { 6u, 0u, 9u, 0u, 11u };
	static constexpr uint32_t obj [] { 7u, 21u, 19u, 1000u, 3u };
	static constexpr uint32_t start [] { 3u, 11u, 17u, 1001u, 7u };
	MdiShadowRow row;
	row.groupCountX = gcX[i];
	row.objectIndex = obj[i];
	row.clusterStart = start[i];
	return row;  // Y/Z = 1、reserved = 0 由默认成员初始化给（= 契约的一部分，正向用例逐字段钉）
}

/** 逐字段比较（被测结构体不带 operator==；比较器住测试侧，先例 test_meshlet_lod）。 */
bool rowEq(const MdiShadowRow& a, const MdiShadowRow& b) {
	return a.groupCountX == b.groupCountX && a.groupCountY == b.groupCountY
		&& a.groupCountZ == b.groupCountZ && a.objectIndex == b.objectIndex
		&& a.clusterStart == b.clusterStart && a.reserved0 == b.reserved0;
}

/** 把行表逐行摊成「objectIndex 序列」（行序判据的读图：错位一格立刻看出来）。 */
std::vector<uint32_t> objectIndexSequence(const MdiShadowRowTable& table) {
	std::vector<uint32_t> out;
	out.reserve(table.rows.size());
	for (const MdiShadowRow& r : table.rows)
		out.push_back(r.objectIndex);
	return out;
}

/** 按置换取候选（行序判据的期望值出处）。 */
std::vector<MdiShadowCandidate> permuted(const std::vector<size_t>& order) {
	std::vector<MdiShadowCandidate> out;
	out.reserve(order.size());
	for (size_t i : order)
		out.push_back(anchor()[i]);
	return out;
}

/** 临时 lua 配置（构造写盘、析构删除；先例 test_scene_config 的 TempLua）。 */
class TempLua {
  public:
	TempLua(std::string_view tag, std::string_view body)
	: path_(std::filesystem::temp_directory_path() / ("rgl_cfg_" + std::string(tag) + ".lua")) {
		std::ofstream out(path_);
		out << body;
	}
	TempLua(const TempLua&) = delete;
	TempLua& operator=(const TempLua&) = delete;
	~TempLua() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	/** 传给 loader 的路径字符串（生命周期随本对象）。 */
	std::string pathStr() const { return path_.string(); }

  private:
	std::filesystem::path path_;
};

// ─── 反向用例表（同一张表喂「逐条归因」与「两两不同」两条用例，杜绝两处漂移）──────────
/** 一个坏形态 = 一个造错误串的入口（构建器 or 对账器）+ 期望错误串的唯一关键词。 */
struct BadCase {
	const char* name;                            ///< 坏形态名（INFO 定位）
	std::optional<std::string> (*run)();         ///< 造出错误串（nullopt = 没报错 = 用例红）
	const char* needle;                          ///< 期望错误串的唯一关键词
};

/** 锚点候选的构建结果（构建失败 = 测试自己错了，REQUIRE 直接红）。 */
MdiShadowRowTable builtAnchor() {
	auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	return *built;
}

/** 不变量 2（**构建器路径**）：可见候选 taskCount = 0 —— 上游契约破了，宁可报错也不发空行。 */
std::optional<std::string> badVisibleNoTasks() {
	std::vector<MdiShadowCandidate> cands = anchor();
	cands[2].taskCount = 0u;  // 可见 + 零 task
	const auto built = buildMdiShadowRows(cands);
	return built ? std::nullopt : std::optional<std::string> { built.error() };
}
/** 不变量 1（形态 A：漏行）：行数组少一行（构建器将来若「顺手压紧」就是这个形状）。 */
std::optional<std::string> badRowDropped() {
	MdiShadowRowTable table = builtAnchor();
	table.rows.pop_back();
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 1（形态 B：压紧）：把两条不可见行删掉 —— 看着像优化，破的是「行序 = 候选序」。 */
std::optional<std::string> badRowCompacted() {
	MdiShadowRowTable table = builtAnchor();
	std::vector<MdiShadowRow> kept;
	for (const MdiShadowRow& r : table.rows)
		if (r.groupCountX != 0u)
			kept.push_back(r);
	table.rows.swap(kept);
	return validateMdiShadowRows(table, anchor());
}
/**
 * 不变量 1（形态 A′：**漏行而三个锚全部诚实**）——只有「行数 == 候选数」这一条能抓的形态。
 *
 * 🔴 这条是变异 M06（`rows.size() != candidateCount` 弱化成 `>`，只挡多行放过少行）逼出来的，
 * 而它**两次**差点写成假隔离，两个坑都记在这里：
 *   ① 锚不重算 ⟹ 漏行时出声的是不变量 3（`sumGroupCountX` 与剩下的行对不上），"弱化行数判据"
 *      这件事本身没有专属信号（形态 A `badRowDropped` 就是这个形状，它靠邻居救场）。
 *   ② 删**中间**那一行 ⟹ 后面的行整体前移，出声的是不变量 4（"行 i 不是候选 i"）——实测该形态
 *      下 M06 只红 1 条且点名的是错的不变量。⟹ 必须删**最后一行**，剩下的行才与候选 0..3 对齐。
 * 于是本用例：删最后一行（前提 `anchor()[4].visible`，钉成 REQUIRE）+ 三个锚按剩下的行**重算**
 * + 补进来的候选可见（`emptyRows == 不可见候选数` 仍成立）⟹ 判据面只剩「行数 == 候选数」一条，
 * 弱化即**静默**（skill `rgl-test-discipline`：「变异要测弱化，不只测删除」）。
 * 形态也真实：将来真做压紧的人一定连锚一起重算，不会留着一个谎报的 rowCount。
 */
std::optional<std::string> badRowDroppedHonestAnchors() {
	REQUIRE(anchor()[4].visible);  // 前提：末位候选可见 ⟹ 删它那一行不改变「不可见候选数」
	std::vector<MdiShadowCandidate> cands = anchor();
	cands.push_back(MdiShadowCandidate { 4242u, 5u, 13u, true });  // 可见 ⟹ 不可见候选数不变
	MdiShadowRowTable table = builtAnchor();
	table.rows.pop_back();  // 🔴 删**最后一行**：剩下的 4 行仍与候选 0..3 逐位对齐
	// （删中间行会让后面的行整体错位 ⟹ 出声的是不变量 4「行 i 不是候选 i」，本用例就白写了）
	// 🔴 三个锚按剩下的行**重算**（诚实）——谎报是形态 C 的职责。
	table.rowCount = table.rows.size();
	table.sumGroupCountX = 0u;
	table.emptyRows = 0u;
	for (const MdiShadowRow& r : table.rows) {
		table.sumGroupCountX += r.groupCountX;
		table.emptyRows += r.groupCountX == 0u ? 1u : 0u;
	}
	return validateMdiShadowRows(table, cands);
}
/** 不变量 1（形态 C：锚与数组分家）：`rowCount` 谎报。 */
std::optional<std::string> badRowCountAnchor() {

	MdiShadowRowTable table = builtAnchor();
	table.rowCount = 3u;  // 数组仍是 5 行
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 3（Σ gcX 锚谎报）。 */
std::optional<std::string> badSumAnchor() {
	MdiShadowRowTable table = builtAnchor();
	table.sumGroupCountX += 1u;
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 3（空行数锚谎报）。 */
std::optional<std::string> badEmptyAnchor() {
	MdiShadowRowTable table = builtAnchor();
	table.emptyRows = 1u;  // 实际 2 条空行
	return validateMdiShadowRows(table, anchor());
}
/**
 * 不变量 3（**前两条查不到的那格**）：一个可见候选发出了空行，而两个锚都与行数组对得上。
 *
 * 手搭行表 = 唯一激励（构建器路径下不变量 2 先拦），而 ★M-4 的 GPU 行表正是「行表与候选清单
 * 分别由两个实现产出」的形态 ⟹ 这条不是假想敌。它与「锚谎报」两条合起来把不变量 3 的三个
 * 分支各钉一处（`!=` 弱化成任一半边的变异由逐条独立用例分别按死）。
 */
std::optional<std::string> badVisibleEmittedEmptyRow() {
	const std::vector<MdiShadowCandidate> cands { MdiShadowCandidate { 5u, 0u, 3u, true } };
	MdiShadowRowTable table;
	table.rows.push_back(MdiShadowRow { 0u, 1u, 1u, 5u, 3u, 0u });  // 可见候选的「合法」行
	table.sumGroupCountX = 0u;   // 与行数组对得上
	table.emptyRows = 1u;        // 与行数组对得上
	table.rowCount = 1u;
	return validateMdiShadowRows(table, cands);
}
/** 不变量 4（序）：两行互换 —— 三个聚合锚**一位不变**，只有逐行判据抓得到。 */
std::optional<std::string> badRowsSwapped() {
	MdiShadowRowTable table = builtAnchor();
	std::swap(table.rows[1], table.rows[2]);
	return validateMdiShadowRows(table, anchor());
}
/** 不变量 4（身份字段错位）：行序对、第 3 行的 objectIndex 被写成别的槽。 */
std::optional<std::string> badIdentityRewritten() {
	MdiShadowRowTable table = builtAnchor();
	table.rows[3].objectIndex = 4242u;
	return validateMdiShadowRows(table, anchor());
}

const std::vector<BadCase>& badCases() {
	static const std::vector<BadCase> table = {
		{ "可见候选 taskCount = 0（构建器路径）", badVisibleNoTasks, "invariant 2" },
		{ "行数组少一行（漏行）", badRowDropped, "invariant 1" },
		{ "漏行且三个锚全部诚实（只有行数判据抓得到）", badRowDroppedHonestAnchors,
			"but the candidate list holds" },
		{ "压紧不可见行（行序破了）", badRowCompacted, "invariant 1" },
		{ "rowCount 锚谎报", badRowCountAnchor, "rowCount says" },
		{ "sumGroupCountX 锚谎报", badSumAnchor, "sumGroupCountX says" },
		{ "emptyRows 锚谎报", badEmptyAnchor, "emptyRows says" },
		{ "可见候选发出空行（两锚都对得上）", badVisibleEmittedEmptyRow,
			"a visible candidate emitted an empty row" },
		{ "两行互换（聚合量一位不变）", badRowsSwapped, "invariant 4" },
		{ "行内身份字段被改写", badIdentityRewritten, "invariant 4" },
	};
	return table;
}

/** 跑一条坏形态并 REQUIRE 它真的报错（两两不同用例用，坏形态静默 = 用例红）。 */
std::string errorOf(const BadCase& bad) {
	auto err = bad.run();
	INFO("坏形态：", bad.name);
	REQUIRE(err.has_value());
	return *err;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 正向锚点（期望值 = 上面那张手算表）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("正向锚点：五行候选的行数组逐字段 + 三个聚合锚等于手算常量") {
	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	REQUIRE(built->rows.size() == 5u);  // 前提守卫：下面的逐行遍历不能是空遍历
	for (size_t i = 0; i < built->rows.size(); ++i) {
		INFO("行 ", i);
		CHECK(rowEq(built->rows[i], expectedRow(i)));
		// 逐字段再钉一遍（rowEq 是「全等」，这里钉的是「哪个字段是什么」——只靠 rowEq，
		// 一个把两个字段同时写歪的变异可能撞上另一个用例，失败信息也指不到字段名）
		CHECK(built->rows[i].groupCountY == 1u);   // 恒 1（一维派发）
		CHECK(built->rows[i].groupCountZ == 1u);   // 恒 1（一维派发）
		CHECK(built->rows[i].reserved0 == 0u);     // 保留位恒 0（★M-3 才启用）
		CHECK(built->rows[i].objectIndex == anchor()[i].objectSlot);
		CHECK(built->rows[i].clusterStart == anchor()[i].clusterStart);
	}
	CHECK(built->sumGroupCountX == 26u);  // 6+9+11
	CHECK(built->emptyRows == 2u);        // 候选 1、3
	CHECK(built->rowCount == 5u);
	// 自洽契约：锚 == 逐行独立重算（★M-1 的 mdi.count / mdi.rows 直接取这两个锚，分家 = 收益判据读错臂）
	uint64_t sum = 0, empty = 0;
	for (const MdiShadowRow& r : built->rows) {
		sum += r.groupCountX;
		empty += r.groupCountX == 0u ? 1u : 0u;
	}
	CHECK(built->sumGroupCountX == sum);
	CHECK(built->emptyRows == empty);
	CHECK(built->rowCount == built->rows.size());
	// 构建器返回前自检过 ⟹ 对外再验一次必须自洽（同一份判据，两处调用点必须同结论）
	CHECK_FALSE(validateMdiShadowRows(*built, anchor()).has_value());
}

TEST_CASE("🔴 行序 = 候选序：换三种顺序 ⟹ 行数组逐元素跟着换（压紧/重排必须红）") {
	// 三条置换：恒等 / 整体倒序 / 打乱。期望 = 按同一置换重排的正向锚点行。
	// 「聚合量抓不到错位」是 test_meshlet_lod 的 base-shift 教训：Σ gcX 与空行数在三条置换下
	// **一位不变**（本用例末尾把它钉成断言，读的人不必自己推）。
	const std::vector<std::vector<size_t>> orders {
		{ 0u, 1u, 2u, 3u, 4u },
		{ 4u, 3u, 2u, 1u, 0u },
		{ 2u, 0u, 4u, 1u, 3u },
	};
	std::vector<std::vector<uint32_t>> seen;
	for (const std::vector<size_t>& order : orders) {
		const auto built = buildMdiShadowRows(permuted(order));
		REQUIRE(built.has_value());
		REQUIRE(built->rows.size() == order.size());
		for (size_t i = 0; i < order.size(); ++i) {
			INFO("置换 ", order[0], " 行 ", i, "（候选 ", order[i], "）");
			CHECK(rowEq(built->rows[i], expectedRow(order[i])));
		}
		// 聚合量与序无关（这三条**不是**行序判据，写在这里是为了让「只有逐行断言抓得到」可读）
		CHECK(built->sumGroupCountX == 26u);
		CHECK(built->emptyRows == 2u);
		CHECK(built->rowCount == 5u);
		seen.push_back(objectIndexSequence(*built));
	}
	// 前提守卫：三条置换确实给出了三种行序（若构建器内部排序/压紧，三条会并成一条 ⟹ 这里红）
	CHECK_FALSE(seen[0] == seen[1]);
	CHECK_FALSE(seen[0] == seen[2]);
	CHECK_FALSE(seen[1] == seen[2]);
}

TEST_CASE("不可见行：gcX = 0 而 objectIndex / clusterStart 照写（红了要能定位是哪一行）") {
	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	REQUIRE(built->rows.size() == 5u);
	for (size_t i = 0; i < anchor().size(); ++i) {
		const MdiShadowCandidate& c = anchor()[i];
		if (c.visible)
			continue;
		INFO("不可见候选 ", i);
		CHECK(built->rows[i].groupCountX == 0u);
		// 🔴 身份字段照写（整行清零 = 红了无法定位是哪一行，且与 §19.2 D4 口径不符）
		CHECK(built->rows[i].objectIndex == c.objectSlot);
		CHECK(built->rows[i].clusterStart == c.clusterStart);
		CHECK(built->rows[i].groupCountY == 1u);
		CHECK(built->rows[i].groupCountZ == 1u);
		CHECK(built->rows[i].reserved0 == 0u);
	}
	// 候选 1 不可见且 taskCount = 0、候选 3 不可见但 taskCount = 2 ⟹ 两条同读数（visible 说话，
	// 不可见时**不看** taskCount；调用方不必特意清零）
	CHECK(built->rows[1].groupCountX == 0u);
	CHECK(built->rows[3].groupCountX == 0u);
	// 反向半边：可见候选的 taskCount 必须**原样**进 gcX（不多不少，尤其不乘任何槽数）
	CHECK(built->rows[0].groupCountX == anchor()[0].taskCount);
	CHECK(built->rows[2].groupCountX == anchor()[2].taskCount);
	CHECK(built->rows[4].groupCountX == anchor()[4].taskCount);
}

TEST_CASE("🔴 前 12B 逐字段对账 VkDrawMeshTasksIndirectCommandEXT（rlbase 不能 import rlvk 的第二钉）") {
	// 双钉的另一半（static_assert 那半在 rgl/mdi_rows.cppm）：本文件链全部四层库，可以拿真
	// Vulkan 结构体来对。rlbase 里**不许**出现这个名字（分层护栏 rules_layers 扫 import 边）。
	static_assert(sizeof(VkDrawMeshTasksIndirectCommandEXT) == 12,
		"VkDrawMeshTasksIndirectCommandEXT is no longer 12B — §19.2 D1's row layout premise changed");
	static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountX) == 0,
		"Vk command groupCountX moved off offset 0");
	static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountY) == 4,
		"Vk command groupCountY moved off offset 4");
	static_assert(offsetof(VkDrawMeshTasksIndirectCommandEXT, groupCountZ) == 8,
		"Vk command groupCountZ moved off offset 8");
	static_assert(sizeof(MdiShadowRow) == 24, "row is not 24B — §19.2 D1");
	static_assert(offsetof(MdiShadowRow, objectIndex) == 12,
		"shader reads word[3] = byte offset 12 (objectIndex)");
	static_assert(offsetof(MdiShadowRow, clusterStart) == 16,
		"shader reads word[4] = byte offset 16 (clusterStart)");
	static_assert(offsetof(MdiShadowRow, reserved0) == 20, "reserved word must be word[5] of 24B");

	const auto built = buildMdiShadowRows(anchor());
	REQUIRE(built.has_value());
	REQUIRE(built->rows.size() == 5u);
	for (size_t i = 0; i < built->rows.size(); ++i) {
		INFO("行 ", i);
		// 前 12B 拷进真结构体 ⟹ 逐字段必须等于候选的 (taskCount|0, 1, 1)
		VkDrawMeshTasksIndirectCommandEXT cmd {};
		std::memcpy(&cmd, &built->rows[i], sizeof(cmd));
		const uint32_t wantX = anchor()[i].visible ? anchor()[i].taskCount : 0u;
		CHECK(cmd.groupCountX == wantX);
		CHECK(cmd.groupCountY == 1u);
		CHECK(cmd.groupCountZ == 1u);
		// 后 12B：word[3]/word[4] 是 shader 直读的两个索引，word[5] 保留
		const auto* words = reinterpret_cast<const uint32_t*>(&built->rows[i]);
		CHECK(words[3] == anchor()[i].objectSlot);   // 不是 taskCount（字段互换的变异在这里红）
		CHECK(words[4] == anchor()[i].clusterStart);
		CHECK(words[5] == 0u);
	}
	// 行表是**连续**的 24B 行（★M-1 直接把 rows.data() memcpy 进 ring buffer，按 stride 逐行跳）
	CHECK(sizeof(MdiShadowRow) == kMdiShadowRowStride);
	CHECK(&built->rows[4] - &built->rows[0] == 4);  // 相邻行无 padding（指针差按元素算）
}

TEST_CASE("stride 合法性（VUID-07088：4 的倍数且 ≥ 12）+ 🔴 与用例里的行数/偏移都不相等") {
	CHECK(kMdiShadowRowStride % 4u == 0u);   // VUID-07088 前半
	CHECK(kMdiShadowRowStride >= 12u);       // VUID-07088 后半（≥ sizeof(Vk…)）
	CHECK(kMdiShadowRowStride == sizeof(MdiShadowRow));
	CHECK(kMdiShadowRowStride == 24u);       // §19.2 D1 的定案值
	CHECK(kMdiShadowRowStride > sizeof(VkDrawMeshTasksIndirectCommandEXT));  // 24 > 12：多出来的正是载荷
	// 🔴 spike §6.3 的 M4 教训钉成断言：stride 与本文件任何用例的行数/偏移都不相等 ⟹ 「行数 ↔ 步长」
	// 参数序写反在每条用例上都可检测（两个数相等时互换测不出东西）。凑数即红。
	constexpr uint32_t caseNumbers [] {
		5u, 3u, 1u,               // 用例里的行数 / 候选数
		3u, 11u, 17u, 1001u, 7u,  // 用例里的 clusterStart（偏移）
		0u, 4u, 8u, 12u, 16u, 20u  // 用例里的字节偏移
	};
	for (uint32_t n : caseNumbers) {
		INFO("用例数值 ", n);
		CHECK(kMdiShadowRowStride != n);
	}
}

TEST_CASE("边界：空候选序列 = 合法空表（不是错误），且对账自洽") {
	const auto built = buildMdiShadowRows(std::span<const MdiShadowCandidate> {});
	REQUIRE(built.has_value());
	CHECK(built->rows.empty());
	CHECK(built->sumGroupCountX == 0u);
	CHECK(built->emptyRows == 0u);
	CHECK(built->rowCount == 0u);
	CHECK_FALSE(validateMdiShadowRows(*built, {}).has_value());
	// 前提守卫（本用例自己也要能失败）：空表**不是**上面几条断言的假绿来源——正向锚点用例
	// 已钉非空形态，这里钉的是「空输入不报错」这条契约（★M-1 里 = 某级联一个候选都不落）。
}

// ─────────────────────────────────────────────────────────────────────────────
// 反向：四条不变量各配可达用例（关键词逐条钉死）+ 错误串两两不同
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("反向：全部坏形态各自报出自己的不变量（关键词逐条钉死）") {
	REQUIRE(badCases().size() == 10u);  // 前提守卫：表被删空 = 下面的遍历恒真是假绿
	for (const BadCase& bad : badCases()) {
		const std::string err = errorOf(bad);
		INFO("坏形态：", bad.name, " 错误串：", err);
		CHECK(err.find("mdi rows: invariant") != std::string::npos);  // 统一前缀 = 可归因
		CHECK(err.find(bad.needle) != std::string::npos);
	}
}

TEST_CASE("反向：十条错误串两两不同（可归因判据本身也要有牙）") {
	std::vector<std::string> errors;
	errors.reserve(badCases().size());
	for (const BadCase& bad : badCases())
		errors.push_back(errorOf(bad));
	REQUIRE(errors.size() == 10u);
	for (size_t i = 0; i < errors.size(); ++i)
		for (size_t j = i + 1; j < errors.size(); ++j) {
			INFO("错误串 ", i, " 与 ", j, " 相同：", errors[i]);
			CHECK_FALSE(errors[i] == errors[j]);
		}
	// 四条不变量各至少出现一次（只钉「两两不同」会让「四条其实只实现了一条」照样绿）
	for (const char* prefix : { "invariant 1", "invariant 2", "invariant 3", "invariant 4" }) {
		INFO("缺少不变量：", prefix);
		CHECK(std::ranges::any_of(errors,
			[&](const std::string& e) { return e.find(prefix) != std::string::npos; }));
	}
}

TEST_CASE("反向：不变量 2 只在构建器判（对账器不重复判输入契约），且不可见候选的 taskCount 不被看") {
	// 半边 A：可见 + 零 task ⟹ 构建器拒绝（宁可报错也不发一条「可见但零 task」的行）
	std::vector<MdiShadowCandidate> cands = anchor();
	cands[4].taskCount = 0u;
	const auto bad = buildMdiShadowRows(cands);
	REQUIRE_FALSE(bad.has_value());
	CHECK(bad.error().find("invariant 2") != std::string::npos);
	CHECK(bad.error().find("visible with taskCount 0") != std::string::npos);
	// 半边 B：不可见 + 零 task ⟹ 合法（**不许**把它也当错误：那是把「不可见」读成「上游坏了」）
	std::vector<MdiShadowCandidate> okInvisible = anchor();
	okInvisible[1].taskCount = 0u;
	okInvisible[1].visible = false;
	const auto ok = buildMdiShadowRows(okInvisible);
	REQUIRE(ok.has_value());
	CHECK(ok->rows[1].groupCountX == 0u);
	// 半边 C：不可见 + **非零** task ⟹ 同样合法（gcX 仍是 0；调用方不必特意清零）
	std::vector<MdiShadowCandidate> okNonZero = anchor();
	okNonZero[1].taskCount = 77u;
	okNonZero[1].visible = false;
	const auto ok2 = buildMdiShadowRows(okNonZero);
	REQUIRE(ok2.has_value());
	CHECK(ok2->rows[1].groupCountX == 0u);
	CHECK(ok2->sumGroupCountX == 26u);  // 77 不进 Σ（不可见 ⟹ gcX 0）
}

// ─────────────────────────────────────────────────────────────────────────────
// 配置门 meshlet.mdi_shadow（★M-0 零消费者，但键必须**被真读到**）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("runtime_config：meshlet.mdi_shadow 默认 false、探针 true 命中、非布尔即失败、无扁平双读") {
	// 🔴 探针值取 **true**（≠ 默认 false）：取 true 时「键被读到」与「键被静默忽略」结果相同
	// = 用例失去判别力却照样绿（§18.11 翻默认那条纪律，本仓 M5 实测坐实过）。
	{  // 成员初始化默认路径（不看 lua 文件）
		CHECK(RuntimeConfig {}.meshletMdiShadow == false);
	}
	{  // 缺块 / 缺键 = 保持默认 false
		TempLua f("rt_mdi_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == false);
		TempLua g("rt_mdi_nokey", R"lua(return { meshlet = { enable = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletMdiShadow == false);
	}
	{  // 块内键名命中（探针 true）
		TempLua f("rt_mdi_on", R"lua(return { meshlet = { mdi_shadow = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == true);
	}
	{  // 显式 false 也读得到（与「缺键回落」同读数，但键名命中路径由上一条钉）
		TempLua f("rt_mdi_off", R"lua(return { meshlet = { mdi_shadow = false } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == false);
	}
	{  // 非布尔 = **启动失败**点名键（meshlet 块五枚布尔同纪律：静默回落 = 截图判据整批假绿）
		for (const char* bad : {R"lua(return { meshlet = { mdi_shadow = "yes" } })lua",
		                        R"lua(return { meshlet = { mdi_shadow = 1 } })lua"}) {
			TempLua f("rt_mdi_bad", bad);
			CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		}
	}
	{  // 🔴 **没有**扁平键的兼容双读（两个真相源 = 本仓反复登记的反模式）
		TempLua f("rt_mdi_flat", R"lua(return { meshlet_mdi_shadow = true })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletMdiShadow == false);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// Lcg 属性组（写死种子；期望值 = 闭式契约，不抄被测码）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Lcg 属性组（种子 20261013）：随机候选序列的闭式契约 + 打乱后行序跟着走") {
	testing::Lcg rng(20261013);
	int cases = 0, nonEmpty = 0;
	for (int iter = 0; iter < 60; ++iter) {
		const size_t n = static_cast<size_t>(rng.uniformInt(0, 9));
		std::vector<MdiShadowCandidate> cands;
		cands.reserve(n);
		for (size_t i = 0; i < n; ++i) {
			MdiShadowCandidate c;
			c.objectSlot = static_cast<uint32_t>(rng.uniformInt(0, 4000));
			// 可见候选的 taskCount 必须 > 0（不变量 2 的定义域）；不可见的故意给 0..4000 两态
			const bool visible = rng.uniformInt(0, 1) == 1;
			c.taskCount = static_cast<uint32_t>(visible ? rng.uniformInt(1, 4000)
			                                            : rng.uniformInt(0, 4000));
			c.clusterStart = static_cast<uint32_t>(rng.uniformInt(0, 4000));
			c.visible = visible;
			cands.push_back(c);
		}
		INFO("iter ", iter, " 候选数 ", n);
		const auto built = buildMdiShadowRows(cands);
		REQUIRE(built.has_value());
		++cases;
		if (n > 0)
			++nonEmpty;

		// ① 行数 == 候选数 + 逐行闭式期望（gcX = visible ? taskCount : 0，身份字段照抄）
		REQUIRE(built->rows.size() == n);
		uint64_t wantSum = 0, wantEmpty = 0;
		for (size_t i = 0; i < n; ++i) {
			const MdiShadowRow& r = built->rows[i];
			const MdiShadowCandidate& c = cands[i];
			const uint32_t wantX = c.visible ? c.taskCount : 0u;
			INFO("iter ", iter, " 行 ", i);
			CHECK(r.groupCountX == wantX);
			CHECK(r.objectIndex == c.objectSlot);
			CHECK(r.clusterStart == c.clusterStart);
			CHECK(r.groupCountY == 1u);
			CHECK(r.groupCountZ == 1u);
			CHECK(r.reserved0 == 0u);
			wantSum += wantX;
			wantEmpty += wantX == 0u ? 1u : 0u;
		}
		// ② 聚合锚 == 独立重算（期望值出处 = 上面那份闭式 wantX，不是被测累加器）
		CHECK(built->sumGroupCountX == wantSum);
		CHECK(built->emptyRows == wantEmpty);
		CHECK(built->rowCount == n);
		// ③ 构建器产物必过对账（同一份判据的两个调用点）
		CHECK_FALSE(validateMdiShadowRows(*built, cands).has_value());

		// ④ 打乱输入 ⟹ 行数组按同一置换重排，而三个聚合锚**不变**（序是契约、聚合量不是）
		if (n >= 2u) {
			std::vector<size_t> order(n);
			for (size_t i = 0; i < n; ++i)
				order[i] = i;
			for (size_t i = n - 1; i > 0; --i) {  // Fisher–Yates（Lcg，禁真随机）
				const size_t j = static_cast<size_t>(rng.uniformInt(0, static_cast<int>(i)));
				std::swap(order[i], order[j]);
			}
			std::vector<MdiShadowCandidate> shuffled;
			shuffled.reserve(n);
			std::vector<uint32_t> wantOrder;
			for (size_t i = 0; i < n; ++i) {
				shuffled.push_back(cands[order[i]]);
				wantOrder.push_back(cands[order[i]].objectSlot);
			}
			const auto shuf = buildMdiShadowRows(shuffled);
			REQUIRE(shuf.has_value());
			CHECK(objectIndexSequence(*shuf) == wantOrder);
			CHECK(shuf->sumGroupCountX == built->sumGroupCountX);
			CHECK(shuf->emptyRows == built->emptyRows);
			CHECK(shuf->rowCount == built->rowCount);
		}
	}
	CHECK(cases == 60);     // 前提守卫：一条都没造出来 = 上面全是空遍历假绿
	CHECK(nonEmpty > 40);   // 非空形态必须占多数（否则「行序」那组断言整批空跑）
}

}  // namespace RGL
