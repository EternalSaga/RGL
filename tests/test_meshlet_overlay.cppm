module;
#include <doctest/doctest.h>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <filesystem>        // 临时 lua 落盘（GMF 例外清单成员）
#include <fstream>

/**
 * test_meshlet_overlay —— **债-105**：α 影子叠加段自己的门 `meshlet.overlay`（默认关）。
 *
 * 被测对象 = `rgl/render_list_impl.cppm` 的 `collectMeshletOverlay` + `meshletTaskCount`
 * （债-105 抬为 render_list 导出接缝，`collectShadowCasters` 的 ★18-B2 先例：接口声明 export、
 * impl partition 定义、测试直打）+ `rgl/runtime_config_impl.cppm` 的 `meshlet` 块新键。
 * **不是** `TEST_SUITE("gpu")`：draw 采集是纯 CPU 判据，一个设备都不碰。
 *
 * ## 为什么值得测（这笔债的性质决定的）
 * 还债的动作 = **摘掉一段一直在跑的东西**。这类改动最坏的失效不是"摘多了"（截图会红、看得见），
 * 而是**摘少了**：门写了、默认也写了 false，但采集判据没接上 ⟹ 出厂态照旧白付 +1571 draws，
 * 而截图**逐位不变**（叠加段本来就一像素不写 = 本批 A1 判据红不了）。所以"少画了一点"必须用
 * **条数判据**钉，不是像素判据：
 *   · 门关 ⟹ **零 draw**（不是"少几条"）；
 *   · 门开 ⟹ **恰一条**且 task 数 = 2×L0 簇数（摘开关不能把 ×2 也顺手摘掉）；
 *   · 互斥抑制（β-1 switch）与既有四条判据**一条都不能被第五枚挤掉**——`collectMeshletOverlay`
 *     的六条判据里任何一条短路，画面都不报错，只在命令流上多/少几条。
 * 加上配置键那一半：`overlay` 的默认**是 false**，所以探针必须取 **true**（§18.11 的
 * 「默认翻方向，探针就得翻方向」——探针与默认同值时「键被静默忽略」与「读到默认」不可区分 = 假绿）。
 *
 * ## 🔴 两枚布尔入参不用同值探针（参数序坑的同族）
 * `collectMeshletOverlay` 现在吃两枚布尔（`overlayActive` / `meshletInPlace`）。**两枚都给 true
 * 或都给 false 的用例测不出实参互换**（`kMdiShadowRowStride` 那两个数相等就白测的同族教训）。
 * ⟹ 本文件把**真值表四格全覆盖**：`关+取代`、`关+不取代`、`开+取代`、`开+不取代` 各一条，
 * 其中只有一格产 draw（0/0/0/1）⟹ **函数体内**把两枚实参写反必红（实测 ② 红 7 条）。
 * ⚠ **调用点**的实参互换单测抓不到（实测 M13 存活，见下方「已知覆盖缺口」第二条）。
 *
 * ## 变异红线声明（每条"哪个变异让我红"；**全部实测** 2026-10-13 本批）
 * harness = `/tmp/opencode/d105_mutate_overlay.py`（第一遍，M02/M09 那两条还原后的全量重跑撞了
 * clang 并行模块编译崩溃 ⟹ 读数作废重跑）+ `/tmp/opencode/d105_mutate_pass2.py`（第二遍 -j6，
 * 两条都「变异红 → 还原 → 构建 0 → 404 全绿」走完）+ `/tmp/opencode/d105_which_red.py`
 * （把 ⑩ 的 7 条红**逐条点名**，不写过claim）。五条防线全实现：`shutil.copyfile` 备份还原
 * （**不带 mtime**）+ 还原后 `os.utime` 抬高、显式 `cmake --build build --target rgl_tests`
 * （tests/ 是 EXCLUDE_FROM_ALL）、核对**构建自身**返回码（构建红 ≠ 判据红，那种读数作废）、
 * 每条还原后重跑全量基线断言全绿、**还原后逐字节复核 sha256 与进场快照相同**（不符即抛
 * 「现场污染」并作废读数——主 agent 会在我离场后在同一棵树上跑自己的变异，这条是给你的防线）。
 * 「红 N 条」= 失败的 TEST_CASE 条数（本文件共 9 条 TEST_CASE，全树 404 条 + 27 条 gpu 套件跳过）。
 *   ① `(!overlayActive && false)`（第五枚短路 = **摘少了**，本批最想要的探针）→ 红 **3** 条
 *      （门关主判据 + 真值表 + 属性用例）
 *   ② `(!meshletInPlace || overlayActive ||…)`（两枚布尔在函数内互换）→ 红 **7** 条
 *      ——真值表四格用**不同值**当探针的兑现处：两枚同值的写法在这里完全测不出互换
 *   ③ `(meshletInPlace && false)`（互斥抑制删掉）→ 红 **2** 条（真值表「开+取代」格 + 互斥回归）
 *   ④ `(asset.meshletPipelineIndex == kInvalidPipelineIndex && false)`（判据 1 短路）→ 红 **2** 条
 *      （四条反向 + 属性用例）
 *   ⑤ `(mat.alphaModeBlend && false)`（判据 2 短路）→ 红 **2** 条（同上两条）
 *   ⑥ `objectSlot > table->bySlot.size()`（判据 3 的越界分支**弱化**，只挡真越界）→ 红 **1** 条
 *      ——探针槽 = resize 后留下的**已知内容**（l0Count=13），弱化后多出一条 draw，不靠 UB 撞运气
 *   ⑦ `if (l0Count == 0 && false)`（判据 4 短路）→ 红 **1** 条（四条反向：零簇那条）
 *   ⑧ `(table == nullptr && false)`（判据 3 的空表分支短路）→ 红 **1** 条（FATAL：解引用空表，
 *      进程在「四条反向」里崩 ⟹ 读数只算「抓到了」，不算归因）
 *   ⑨ `pushData, l0Count}`（×2 摘掉）→ 红 **4** 条（阳性锚点两条 task 数断言 + ×2 逐槽 + 属性）
 *   ⑩ `kSlotsPerCluster + clusterCount`（**本地正确的另一半**：n=2 时加与乘同值）→ 红 **7** 条，
 *      逐条点名（实测列出，不是推测）：本文件 5 条（阳性锚点 / 四条反向 / 规格常量 / ×2 逐槽 /
 *      属性）+ `tests/test_shadow_lod_emit.cppm` 的 2 条（emit 主用例 + collect+emit 属性）
 *      ⟹ 这就是「单一真相源」的兑现：改它，三个消费者各自的用例一起红
 *   ⑪ 解析器键名 `overlay → overlay_x`（键被静默忽略）→ 红 **1** 条（配置用例；探针取 true 才红，
 *      与默认同值则整条绿 = §18.11 的 M5 形态）
 *   ⑫ 成员初始化 `meshletOverlay = true`（默认翻向 = 出厂又白付一次）→ 红 **1** 条（配置用例默认断言）
 *
 * ## 已知覆盖缺口（写出来，别让「全绿」被读成「全验过」）
 *   · 🔴 **调用点的实参互换测不到**（上面 M13 的实测读数）。单测直打函数本体，调用点在
 *     `renderListSystem`（要 registry + Resources = 设备级），进不了 `just test`。兜住这条的
 *     不是单测而是
 *     **A4 的 counters**（互换 ⟹ 叠加段不再采 draw ⟹ `overlay=true` 臂的 `draw.count` 从 8172
 *     掉回 6601）与 **A6 的变图**（debug/tint 臂根本不会变）。⟹ 交验收时 A4/A6 不能省。
 *   · ~~**装配层那三步进不了单测**~~ 之一已收口（债-129 S2）：`pickMeshletMeshVariant` 原住
 *     `module main:meshlets` 实现分区（rgl_tests 链不到 main = 当时不可达的真根因，与 registry
 *     无关——函数本体是纯的），已下移到 shader_registry，三档选档 + overlay 关强制 base
 *     现由本文件「七」节直打。`wireMeshletOverlayActivation` 的 emplace/出声仍要 registry +
 *     设备级资产表 + 日志单例（防线 = 验收判据 A6b 与 A8，不变）。
 *   · **烘焙不受 overlay 门管辖**这件事（β-1 取代段的宿主不能丢）在单测里**测不到**——
 *     `bakeMeshletEntries` 要 ShaderRegistry/设备。防线 = 该函数头的钉死注释 + 一条**专门跑
 *     `switch=true + overlay=false` 的臂**（若烘焙被误挂到 overlay 门下，那臂会静默退回旧路径：
 *     `draw.count` 仍 = 6601 与出厂态相同，所以**必须看日志里 switch 的「取代图元 N 个」是否为 0**，
 *     只看 counters 抓不到）。
 *   · 叠加段「真的被执行器画出来」（`executeMeshletOverlay` / 动态状态那侧）不在本文件射程，
 *     归 ★0.5-b2 的四臂判据与 A3/A6。
 */
export module test_meshlet_overlay;

import rlstd;
import test_support;                     // Lcg（写死种子，禁真随机）
import render_list;                      // collectMeshletOverlay / meshletTaskCount（债-105 接缝）
import frame_draw_list;                  // DrawCall
import asset_table;                      // MeshAsset / MaterialAsset / kInvalidPipelineIndex
import meshlet_ranges;                   // ClusterRangeTable
import scene_forward_types_generated;    // ObjectClusterRange（区间表的条目类型，反射生成）
import material_types_generated;         // DrawArgs（push 字节的真实类型，判 pushData 身份）
import meshlet_cull_types_generated;     // kSlotsPerCluster（×2 契约的 CPU 端规格常量）
import runtime_config;                   // 配置键 meshlet.overlay 的解析契约
import shader_registry;                  // pickMeshletMeshVariant / MeshletMeshVariant（债-129 S2 下移到本模块 = 单测可达）

namespace RGL {
namespace {

/** 阳性锚点的叠加管线条目下标（非 0 = 0 是合法条目下标，用它当哨兵会读成"没换条目"）。 */
constexpr uint32_t kAnchorPipeline = 42u;
/** 阳性锚点的 objectSlot（区间表下标 = shader objectIndex）。 */
constexpr uint32_t kAnchorSlot = 3u;

/**
 * 造一份**内容已知**的区间表：槽 i 的 l0Count = 10 + i，槽数 = slots。
 * 期望条数与 task 数全部由调用方按这个闭式算，不抄被测码。
 */
ClusterRangeTable makeTable(uint32_t slots) {
	ClusterRangeTable table;
	for (uint32_t i = 0; i < slots; ++i)
		table.bySlot.push_back(ObjectClusterRange{.base = 100u + i, .count = 50u + i,
			.l0Count = 10u + i, .cullBase = 7u * i});
	return table;
}

/** 造一个「四条既有判据全满足」的资产（唯一可变量 = 条目下标）。 */
MeshAsset anchorAsset() {
	MeshAsset asset;
	asset.pipelineIndex = 5u;
	asset.groupCount = 9u;
	asset.meshletPipelineIndex = kAnchorPipeline;
	return asset;
}

std::span<const std::byte> pushOf(const DrawArgs& args) {
	return std::as_bytes(std::span<const DrawArgs>{&args, 1});
}

/**
 * 单条采集：**只把 overlay 门与互斥开关当变量**，其余四条判据保持"全满足"。
 * 🔴 缺省资产/材质在**本函数**各持一份（MeshAsset 因 `optional<VmaBuffer>` 禁拷贝，
 * 三元表达式里两边一个左值一个临时量会直接编不过）。
 * @return 采集到的 draw 条数（调用方按真值表断言）
 */
size_t collectOne(bool overlayActive, bool meshletInPlace, const ClusterRangeTable* table,
	uint32_t objectSlot, const DrawArgs& args, std::vector<DrawCall>& out,
	const MeshAsset* asset = nullptr, const MaterialAsset* mat = nullptr) {
	const MeshAsset fallbackAsset = anchorAsset();
	const MaterialAsset fallbackMat;
	collectMeshletOverlay(asset ? *asset : fallbackAsset, mat ? *mat : fallbackMat, table,
		objectSlot, overlayActive, meshletInPlace, pushOf(args), out);
	return out.size();
}

/** 临时 lua 配置（构造写盘、析构删除；先例 test_scene_config / test_mdi_rows 的 TempLua）。 */
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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 一、门的两态：阳性锚点与门关零 draw（本批主判据）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("collectMeshletOverlay：门开 + 四条判据满足 ⟹ 恰 1 条 draw，task 数 = 同一入口的 2×L0") {
	// 变异红线 ②（门开反而不采）、⑩（×2 摘掉）在此红。
	const auto table = makeTable(kAnchorSlot + 1u);
	const DrawArgs args{.objectIndex = kAnchorSlot};
	std::vector<DrawCall> out;

	REQUIRE(collectOne(true, false, &table, kAnchorSlot, args, out) == 1u);  // 阳性锚点本身
	REQUIRE(out.size() == 1u);                                               // 防空集合假绿
	CHECK(out[0].pipeline == kAnchorPipeline);  // 管线条目 = 叠加条目（不是旧 pipelineIndex=5）
	// 🔴 task 数的**两条**期望：同一入口（meshletTaskCount）+ 独立闭式（规格常量 × 簇数）。
	// 只钉第一条会漏掉"meshletTaskCount 自己被改坏"（两处一起坏 = 断言恒真），故两条都要。
	CHECK(out[0].groupCount == meshletTaskCount(table.bySlot[kAnchorSlot].l0Count));
	CHECK(out[0].groupCount == kSlotsPerCluster * 13u);  // 槽 3 的 l0Count 闭式 = 10 + 3 = 13
	CHECK(out[0].groupCount > 0u);
	// pushData 身份：与本体 draw 同一个 DrawArgs 的地址与字节数（换了 = shader 读错槽）
	CHECK(out[0].pushData.data() == reinterpret_cast<const std::byte*>(&args));
	CHECK(out[0].pushData.size() == sizeof(DrawArgs));
}

TEST_CASE("collectMeshletOverlay：门关 ⟹ **零 draw**（债-105 的主判据，出厂态拿回那 1.59ms）") {
	// 变异红线 ①（第五枚判据短路 = "摘少了"）在此红——本批唯一能抓住"门写了但没接上"的用例。
	const auto table = makeTable(kAnchorSlot + 1u);
	const DrawArgs args{.objectIndex = kAnchorSlot};

	std::vector<DrawCall> off;
	CHECK(collectOne(false, false, &table, kAnchorSlot, args, off) == 0u);
	// 🔴 阳性对照（同一份输入、只差那枚门）：没有这条，"零 draw"可能是四条既有判据给的假绿。
	std::vector<DrawCall> on;
	REQUIRE(collectOne(true, false, &table, kAnchorSlot, args, on) == 1u);
	CHECK(off.size() == 0u);
	CHECK(on.size() == 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 二、两枚布尔的真值表（四格全覆盖 ⟹ 实参互换测不出来这件事不成立）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("collectMeshletOverlay：overlay × inPlace 真值表四格，只有「开 + 不取代」产 draw") {
	// 变异红线 ③（互斥判据删掉）、④（两枚互换）在此红。
	// 🔴 四格里的期望条数是 **0/1/0/0**（两格同值但输入不同）：任何一格把两枚布尔当同一枚用，
	// 至少一格红。单看「开+取代」与「关+取代」两格同期望（0），是因为它们**各自独立**被 ③/④ 打到。
	const auto table = makeTable(kAnchorSlot + 1u);
	const DrawArgs args{.objectIndex = kAnchorSlot};

	struct Row {
		bool overlay;
		bool inPlace;
		size_t expect;
	};
	const std::initializer_list<Row> truthTable{
		{false, false, 0u},  // 门关 + 不取代 = 不采（债-105 的出厂态）
		{false, true, 0u},   // 门关 + 取代 = 不采（两枚都关住）
		{true, false, 1u},   // 门开 + 不取代 = 采（唯一产 draw 的格）
		{true, true, 0u},    // 门开 + 取代 = 不采（β-1 互斥：同一批几何不画两遍）
	};
	size_t ones = 0;
	for (const auto& row : truthTable) {
		std::vector<DrawCall> out;
		const size_t got = collectOne(row.overlay, row.inPlace, &table, kAnchorSlot, args, out);
		CHECK(got == row.expect);
		CHECK(out.size() == row.expect);  // 与返回值同源复核（collectOne 自己别写错）
		ones += (row.expect == 1u) ? 1u : 0u;
	}
	CHECK(ones == 1u);  // 前置守卫：真值表里**必须**只有一格产 draw（否则本用例的判别力是编的）
}

TEST_CASE("collectMeshletOverlay：互斥回归——门开但 switch 语义生效 ⟹ 零 draw（既有抑制没被挤掉）") {
	// 变异红线 ③ 的**专门**用例（A6 的互斥口径）：本批加的第五枚不能把 ★1-a1ii-β-1 那条抑制顶掉。
	const auto table = makeTable(kAnchorSlot + 1u);
	const DrawArgs args{.objectIndex = kAnchorSlot};
	std::vector<DrawCall> suppressed;
	CHECK(collectOne(true, true, &table, kAnchorSlot, args, suppressed) == 0u);
	// 阳性对照：把取代标记拿掉、其余一字不动 ⟹ 必须产 1 条（否则"抑制"是四条判据给的假绿）
	std::vector<DrawCall> active;
	REQUIRE(collectOne(true, false, &table, kAnchorSlot, args, active) == 1u);
	CHECK(active[0].pipeline == kAnchorPipeline);
}

// ─────────────────────────────────────────────────────────────────────────────
// 三、既有四条判据各一条反向（+ 每条自己的阳性对照）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("collectMeshletOverlay：四条既有判据逐条反向——无条目 / blend / 无区间表 / 零簇 / 槽越界") {
	// 变异红线 ⑤⑥⑦⑧⑨ 在此红。门**保持开**：反向用例必须在门开的输入形态下才有判别力
	// （门关时四条本来就不可能被走到，那种"绿"是门给的 = 假绿）。
	const auto table = makeTable(kAnchorSlot + 1u);
	const DrawArgs args{.objectIndex = kAnchorSlot};

	// 前置：四条全满足的基线形态确实产 1 条（后面每条反例都只改这一个变量）
	std::vector<DrawCall> base;
	REQUIRE(collectOne(true, false, &table, kAnchorSlot, args, base) == 1u);

	{  // 判据 1：资产没有叠加管线条目（门关 / 无产物 / 多产物回落）
		auto noEntry = anchorAsset();
		noEntry.meshletPipelineIndex = kInvalidPipelineIndex;
		std::vector<DrawCall> out;
		collectMeshletOverlay(noEntry, MaterialAsset{}, &table, kAnchorSlot, true, false,
			pushOf(args), out);
		CHECK(out.empty());
	}
	{  // 判据 2：blend 材质（透明段按距离²排序，叠加不参与）
		MaterialAsset blend;
		blend.alphaModeBlend = true;
		std::vector<DrawCall> out;
		collectMeshletOverlay(anchorAsset(), blend, &table, kAnchorSlot, true, false,
			pushOf(args), out);
		CHECK(out.empty());
		// 反向的反向：MASK **不在**判据里（与 α 现行一致，别顺手把 discard 材质也剔掉）
		MaterialAsset mask;
		mask.alphaModeMask = true;
		std::vector<DrawCall> outMask;
		collectMeshletOverlay(anchorAsset(), mask, &table, kAnchorSlot, true, false,
			pushOf(args), outMask);
		CHECK(outMask.size() == 1u);
	}
	{  // 判据 3：区间表 nullptr（门关 = 装配层根本不建）
		std::vector<DrawCall> out;
		collectMeshletOverlay(anchorAsset(), MaterialAsset{}, nullptr, kAnchorSlot, true, false,
			pushOf(args), out);
		CHECK(out.empty());
	}
	{  // 判据 3 的另一半：槽号越界（表比实例矩阵槽短）。🔴 探针 = resize 后**已知内容**那一格：
		// bySlot 被填过 4 格再 resize 到 3 ⟹ size==3 而容量不变，槽 3 的字节仍在同一次分配里，
		// 越界判据被删时会**确定性**读到 l0Count=13（多一条 draw），而不是撞堆垃圾的运气。
		ClusterRangeTable shortTable;
		for (uint32_t i = 0; i < 4u; ++i)
			shortTable.bySlot.push_back(ObjectClusterRange{.base = 0u, .count = 0u,
				.l0Count = 10u + i, .cullBase = 0u});
		shortTable.bySlot.resize(3u);
		REQUIRE(shortTable.bySlot.size() == 3u);
		std::vector<DrawCall> out;
		collectMeshletOverlay(anchorAsset(), MaterialAsset{}, &shortTable, 3u, true, false,
			pushOf(args), out);
		CHECK(out.empty());
		// 同一张表的槽 2（界内）必须产 draw——证明"槽 3 零 draw"来自越界判据而不是表是空的
		std::vector<DrawCall> inRange;
		collectMeshletOverlay(anchorAsset(), MaterialAsset{}, &shortTable, 2u, true, false,
			pushOf(args), inRange);
		REQUIRE(inRange.size() == 1u);
		CHECK(inRange[0].groupCount == kSlotsPerCluster * 12u);  // 槽 2 的 l0Count 闭式 = 12
	}
	{  // 判据 4：本槽 L0 簇数 = 0（无簇 = 零 group，连空 draw 都不发）
		auto zeroTable = makeTable(kAnchorSlot + 1u);
		zeroTable.bySlot[kAnchorSlot].l0Count = 0u;
		std::vector<DrawCall> out;
		collectMeshletOverlay(anchorAsset(), MaterialAsset{}, &zeroTable, kAnchorSlot, true, false,
			pushOf(args), out);
		CHECK(out.empty());
		// 阳性对照：同一张表只把 l0Count 从 0 改成 1 ⟹ 必须产 draw（挡住"零簇槽与零条目槽混绿"）
		zeroTable.bySlot[kAnchorSlot].l0Count = 1u;
		std::vector<DrawCall> one;
		collectMeshletOverlay(anchorAsset(), MaterialAsset{}, &zeroTable, kAnchorSlot, true, false,
			pushOf(args), one);
		REQUIRE(one.size() == 1u);
		CHECK(one[0].groupCount == kSlotsPerCluster * 1u);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 四、×2 口径（抬成接缝的那半：测试用同一入口 + 独立闭式双钉）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("meshletTaskCount：导出接缝与规格常量同式，kSlotsPerCluster == 2（×2 契约的 CPU 端）") {
	// 变异红线 ⑪ 在此红（`kSlotsPerCluster *` 改成 `+ 1u` 这种"本地正确的另一半"）。
	// 三条不同簇数各钉一次：1 → 2、5 → 10、682 → 1364（682 = 实测单 draw 最大簇数）。
	CHECK(kSlotsPerCluster == 2u);
	CHECK(meshletTaskCount(1u) == 2u);
	CHECK(meshletTaskCount(5u) == 10u);
	CHECK(meshletTaskCount(682u) == 1364u);
	for (const uint32_t n : {0u, 1u, 2u, 13u, 682u})
		CHECK(meshletTaskCount(n) == kSlotsPerCluster * n);  // 同一契约的两端
}

TEST_CASE("collectMeshletOverlay：门开时 task 数随 l0Count 走 2×，而不是资产旧 groupCount") {
	// 变异红线 ⑩（叠加段也用裸 l0Count）在此红。同一资产、同一张表，只换槽号（l0Count 已知）。
	const auto table = makeTable(6u);
	const DrawArgs args{.objectIndex = 0u};
	const uint32_t expected[6] = {20u, 22u, 24u, 26u, 28u, 30u};  // 闭式 = 2 × (10 + slot)
	for (uint32_t slot = 0u; slot < 6u; ++slot) {
		std::vector<DrawCall> out;
		collectMeshletOverlay(anchorAsset(), MaterialAsset{}, &table, slot, true, false,
			pushOf(args), out);
		REQUIRE(out.size() == 1u);
		CHECK(out[0].groupCount == expected[slot]);
		CHECK(out[0].groupCount == meshletTaskCount(table.bySlot[slot].l0Count));
	}
	// 反证锚：旧 mesh 管线的 groupCount = 9，与上面任何一条都不相等（挡住"抄了 asset.groupCount"）
	CHECK(anchorAsset().groupCount == 9u);
}

// ─────────────────────────────────────────────────────────────────────────────
// 五、属性用例（Lcg 写死种子）：随机场景序列下的条数与内容闭式对账
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("collectMeshletOverlay：随机实体序列 ⟹ 条数与逐条内容等于闭式重算（门开/门关各一遍）") {
	// 变异红线 ①⑤⑥⑨⑩ 在随机组合下再加一遍网（手搭用例覆盖不到的组合：零簇槽 × blend ×
	// 无条目 × 槽号越界同时出现）。
	testing::Lcg rng(20261013u);
	constexpr uint32_t kSlots = 24u;
	auto table = makeTable(kSlots);

	struct Item {
		uint32_t slot;
		uint32_t pipeline;
		bool blend;
	};
	std::vector<Item> items;
	for (uint32_t i = 0; i < 40u; ++i) {
		Item it;
		it.slot = static_cast<uint32_t>(rng.uniformInt(0, static_cast<int>(kSlots) - 1));
		it.pipeline = (rng.uniformInt(0, 3) == 0) ? kInvalidPipelineIndex : (200u + i);
		it.blend = rng.uniform01() < 0.25;
		items.push_back(it);
	}
	std::vector<DrawArgs> argsStorage(items.size());
	std::vector<DrawCall> overlayOff;
	std::vector<DrawCall> overlayOn;
	for (size_t i = 0; i < items.size(); ++i) {
		const Item& it = items[i];
		argsStorage[i].objectIndex = it.slot;
		MeshAsset asset = anchorAsset();
		asset.meshletPipelineIndex = it.pipeline;
		MaterialAsset mat;
		mat.alphaModeBlend = it.blend;
		const auto bytes = pushOf(argsStorage[i]);
		// 门关臂：实体循环照跑，只是不该有任何一条落进叠加向量
		collectMeshletOverlay(asset, mat, &table, it.slot, false, false, bytes, overlayOff);
		// 门开臂：期望条数与内容由下面的闭式重算给出
		collectMeshletOverlay(asset, mat, &table, it.slot, true, false, bytes, overlayOn);
	}

	// 闭式重算（期望值出处 = 本文件的表构造 + 输入序列，不读被测码的判据）
	std::vector<size_t> expectedIndex;
	for (size_t i = 0; i < items.size(); ++i) {
		const Item& it = items[i];
		const bool ok = it.pipeline != kInvalidPipelineIndex && !it.blend
		                && it.slot < table.bySlot.size() && table.bySlot[it.slot].l0Count > 0u;
		if (ok)
			expectedIndex.push_back(i);
	}
	CHECK(overlayOff.empty());  // 🔴 门关 = 一条都不采（本批主判据在随机序列上的另一半）
	CHECK(!expectedIndex.empty());  // 前置守卫：随机序列必须真的产得出 draw，否则上面两条恒真
	REQUIRE(overlayOn.size() == expectedIndex.size());
	for (size_t k = 0; k < expectedIndex.size(); ++k) {
		const Item& it = items[expectedIndex[k]];
		CHECK(overlayOn[k].pipeline == it.pipeline);
		CHECK(overlayOn[k].groupCount == kSlotsPerCluster * table.bySlot[it.slot].l0Count);
		CHECK(overlayOn[k].pushData.data()
		      == reinterpret_cast<const std::byte*>(&argsStorage[expectedIndex[k]]));
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 六、配置键 meshlet.overlay（默认 false ⟹ 探针必须取 true）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("runtime_config：meshlet.overlay 默认 false、探针 true 命中、非布尔即失败、无扁平双读") {
	// 变异红线 ⑫（键名打错 = 静默忽略）、⑬（默认翻成 true）在此红。
	// 🔴 探针取 **true**：默认是 false，探针与默认同值时「键被读到」与「键被静默忽略」不可区分
	// （§18.11 那条纪律，本仓 M5 实测坐实过）。
	{  // 成员初始化默认路径（不看 lua 文件）= 债-105 的"出厂不白付"全靠这一枚默认
		CHECK(RuntimeConfig{}.meshletOverlay == false);
	}
	{  // 缺块 / 缺键 = 保持默认 false（门不会因为 assets 或别的键被顺手打开）
		TempLua f("rt_ovl_default", "return {}\n");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletOverlay == false);
		TempLua g("rt_ovl_nokey", R"lua(return { meshlet = { assets = true } })lua");
		auto r2 = loadRuntimeConfig(g.pathStr());
		REQUIRE(r2.has_value());
		CHECK(r2->meshletOverlay == false);
	}
	{  // 块内键名命中（探针 true）
		TempLua f("rt_ovl_on", R"lua(return { meshlet = { overlay = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletOverlay == true);
	}
	{  // 显式 false 也读得到（与「缺键回落」同读数，键名命中路径由上一条钉）
		TempLua f("rt_ovl_off", R"lua(return { meshlet = { overlay = false } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletOverlay == false);
	}
	{  // 本键**不**与 assets 焊死：assets=false + overlay=true 读到的仍是 overlay=true
		// （孤立组合由装配层按事实出声，配置层不做隐含耦合 = 交付物 (1) 的红线）
		TempLua f("rt_ovl_orphan", R"lua(return { meshlet = { assets = false, overlay = true } })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletOverlay == true);
		CHECK(r->meshletAssets == false);
	}
	{  // 非布尔 = **启动失败**点名键（meshlet 块全部布尔键同纪律：静默回落 = 截图判据整批假绿）
		for (const char* bad : {R"lua(return { meshlet = { overlay = "yes" } })lua",
		                        R"lua(return { meshlet = { overlay = 1 } })lua"}) {
			TempLua f("rt_ovl_bad", bad);
			CHECK_FALSE(loadRuntimeConfig(f.pathStr()).has_value());
		}
	}
	{  // 🔴 **没有**扁平键的兼容双读（两个真相源 = 本仓反复登记的反模式）
		TempLua f("rt_ovl_flat", R"lua(return { meshlet_overlay = true })lua");
		auto r = loadRuntimeConfig(f.pathStr());
		REQUIRE(r.has_value());
		CHECK(r->meshletOverlay == false);
	}
}

// ─────────────────────────────────────────────────────────────────────────────
// 七、pickMeshletMeshVariant 三档选档（债-129 S2：函数下移到 shader_registry 后单测可达）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("pickMeshletMeshVariant：instrument 三档各返回正确变体；overlay 关 = 一律 base（让位分支已删）") {
	// 🔴 三档返回语义与合并前**逐字不变**（约束红线）：none→base、offset→debugOffset、tint→tint。
	// 原「tint 与 debug 同真 ⟹ tint 优先」的用例**整删**——两布尔已合并为一枚三值键，
	// 该输入**不可构造**，让位分支连同规则一起消失（「消灭的是一个真实可达态」的实测证据 =
	// 合并前基线第四行：overlay+debug+tint 三真截图与 tint 单开**逐位同** = /tmp/opencode/
	// o1_premerge_anchors.txt，合并后这个实验永远做不了 = 旧写法配不出来）。
	// 变异红线：① tint/offset 两档 return 互换 → overlay 开两断言红；
	//           ② overlay 闸门删掉 → overlay 关三断言全红（债-105 归属裁决 = 仪器渗进产品路）；
	//           ③ none 档改成返回 debugOffset → none 断言红（出厂叠加 = 正常档）。
	RuntimeConfig on;
	on.meshletOverlay = true;
	CHECK(pickMeshletMeshVariant(on) == MeshletMeshVariant::base);  // instrument = none（默认）
	on.meshletInstrument = MeshletInstrument::offset;
	CHECK(pickMeshletMeshVariant(on) == MeshletMeshVariant::debugOffset);
	on.meshletInstrument = MeshletInstrument::tint;
	CHECK(pickMeshletMeshVariant(on) == MeshletMeshVariant::tint);
	// 🔴 overlay 关 = 一律 base（债-105 的归属裁决：变体位归叠加段，三档全钉）
	for (const MeshletInstrument instr : {MeshletInstrument::none, MeshletInstrument::offset,
	                                      MeshletInstrument::tint}) {
		RuntimeConfig off;  // overlay = false（出厂默认）
		off.meshletInstrument = instr;
		CHECK(pickMeshletMeshVariant(off) == MeshletMeshVariant::base);
	}
}

}  // namespace RGL
