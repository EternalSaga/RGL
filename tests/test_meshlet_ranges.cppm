module;
#include <doctest/doctest.h>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）

/**
 * test_meshlet_ranges —— 簇化轴 ★1-a1i：逐 prim 簇区间扫描（四条产物不变量）+ scene data
 * 尾区写入（纯 CPU、无设备）。
 *
 * 被测对象 = `rgl/meshlet_ranges.cppm` + `rgl/meshlet_ranges_impl.cppm`（rlbase）与
 * `rgl/scene_data.cppm` 的 `writeClusterRangeRegion`（可测接缝）。
 *
 * ## 输入 = 合成产物字节流经 `parseMeshletAsset` 进扫描器（不手搭 span）
 * 与 `test_meshlet_asset` 同法：合成缓冲按 format.h 段序铺字节，走**真实读取路径**。
 * 好处有两条：① 反向用例的输入是「读取器认得的合法产物」——四条不变量之所以要单独验，
 * 正因为它们**不在**读取器的校验清单里（读取器只查 Σ 覆盖，不查逐 prim 相接/铺满/L0 前缀）；
 * ② 避开「span 指向临时 vector」的生命周期坑（MeshletAsset 的 span 依赖自身镜像地址，
 * 且它禁拷贝——想改 header 只能各自 parse 一份）。
 * 每条合成产物 = 每簇 1 三角 / 1 顶点 / remap 恒指 0 号顶点，规模 ≤ 10 簇 ≈ 1KB，毫秒级。
 * 🔴 不把 184MB 的 bistro 产物当测试输入（测试不依赖构建产物）；真产物的三个统计锚
 * （1591 / 61518 / 27487）由引擎启动日志常驻出声（`wireClusterRangeTable` 的 INFO），
 * 口径 → `meshlet_ranges.cppm` 文件头。
 *
 * ## 四条不变量各配反向用例，且**断言错误串关键词**
 * 四条的错误串前缀两两不同（`invariant 1..4`），装配层把它原样打进 WARNING。只断言
 * 「扫描失败」会让「恒假的检查」与「坏掉的输入」无法区分，也会让两条检查互换而无人发现，
 * 故逐条钉关键词，并另有一条「全部错误串两两不同」的独立用例（先例 = test_meshlet_asset）。
 *
 * ## 正向锚点（期望值手算写死，出处 = 下方推导，不取被测码）
 * 三条 prim 的形态与期望区间手算见 kAnchor 注释；两个合计（10 / 6）同样写死。
 * 「扫描整段失效」时反向用例红，「扫描误杀一切」时正向用例红——两个方向都不恒绿。
 *
 * ## 变异红线（哪个变异让我红；**全部实测** 2026-10-12，`cp` 备份还原、每条还原后基线全绿）
 *   ① `scanPrimRuns` 段序判据 `prim != perPrim.size()` 短路（`&& false`）→ 红 2 条
 *      （全部坏形态 + 两两不同）
 *   ② `prim >= primCount` 判据短路 → 红 1 条（全部坏形态）
 *   ③ 循环后「段数 == primCount」判据短路 → 红 2 条（全部坏形态 + 两两不同）
 *   ④ `mergeRunIntoRange` 的 `off != end` 判据短路 → 红 2 条（全部坏形态 + 两两不同）
 *   ⑤ `fillL0Prefixes` 前缀判据短路（`isL0Cluster(...) && false`）→ 红 2 条（同上两条）
 *   ⑥ `isL0Cluster` 取反（`refined == -1` → `refined != -1`）→ 红 3 条（正向锚点 + 逆序书写
 *      + 全 L0 产物）——三个正向用例同时红 = 「扫描对 L0 的整个口径失效」的形态
 *   ⑦ `checkTiling` 的 `base != expect` 短路 → 红 1 条（全部坏形态）
 *   ⑧ `writeClusterRangeRegion` 容量判据 `>` 写成 `>=` → 红 1 条（容量边界）
 *   ⑨ `writeClusterRangeRegion` 删掉有效段之后的清零 memset → 红 2 条（逐字段落位 + 门关）
 *   ⑩ `writeClusterRangeRegion` 写入基址误用 `kSceneDataWetOffset` → 红 3 条（逐字段落位
 *      + 门关 + 容量边界）
 *   ⑪ `mergeRunIntoRange` 删掉 `std::sort` → 红 1 条（逆序书写：表序首段起点 2 ≠ 区间起点，
 *      第二段起点 0 撞上 end → invariant 2 误报）
 *   ⑫ 🔴 `checkTiling` 的 `base != expect` **弱化**成 `base > expect`（只挡空洞、放过重叠）
 *      → **原十一种变异全绿时这条也全绿 = 真覆盖缺口**（主 agent 实测）。补 `badOverlap` 后红 2 条。
 *      注意它**不是**"完全没响"：重叠仍会被末尾 `expect != clusterCount` 抓到，但**不点名 prim**
 *      ⟹ 本用例钉的是**归因**（见 badOverlap 注释）。教训两条：**(a) 判据短路 ≠ 判据弱化**——
 *      只测"摘掉整条判据"会漏掉"判据写反一半"，而后者才是 `!=` 比较最真实的漂移形态；
 *      **(b) 反向用例要按"这条判据负责什么"逐半边构造**，一条"空洞"用例会被读成"铺满已验"。
 *
 * 12 条变异全部被捕获，且**没有一条靠"整条测试挂掉"蒙过去**——每条都点名到具体用例。
 *
 * ## 已知覆盖缺口（写出来，别让「全绿」被读成「全验过」）
 *   · `mergeRunIntoRange` 的 `end > clusterCount` 与 `checkTiling` 的 `expect != clusterCount`
 *     两条**经 parse 合法产物不可达**：读取侧「逐 group 区间有界」+「Σ group.clusterCount ==
 *     clusterCount」两条对账已先挡住（构造不出输入）。它们是扫描器对**直接调用方**的自保
 *     （扫描器不假设调用方过了读取侧），本文件不覆盖，也不虚报成「已验」。
 *   · 装配层 `wireClusterRangeTable` 的两道对账（prim 数 / material 身份）住在 app 模块匿名
 *     namespace、依赖 tinygltf 模型对象，**不在单测面**；护栏 = 引擎启动日志无 WARNING
 *     （★1-a1i 实测真产物通过）。缺口账见交付报告。
 */
export module test_meshlet_ranges;

import rlstd;
import meshlet_asset;    // MeshletAsset / parseMeshletAsset / 段规格常量
import meshlet_ranges;   // scanClusterRanges / MeshletPrimRange
import scene_data;       // writeClusterRangeRegion / kSceneDataClusterRange* / kSceneDataBytes
import scene_forward_types_generated;  // ObjectClusterRange（表条目，反射生成）

namespace RGL {
namespace {

// ─── 合成产物（规格常量按 format.h 头注释手抄 = 期望值的独立出处，不取被测模块常量）─────
constexpr size_t kHdrBytes = 192, kPrimBytes = 16, kClusterBytes = 44, kGroupBytes = 36,
                 kNodeBytes = 32, kVertexBytes = 48;

/** 写侧段对齐（format.h `kSectionAlign` 口径的手抄）。 */
uint64_t alignUp16(uint64_t v) { return (v + 15) / 16 * 16; }

/** 一条 group 的规格（包围体等其余字段扫描不读，铺零）。 */
struct GroupDesc {
	uint32_t prim;            ///< MeshletDiskGroup::prim
	int32_t depth;            ///< MeshletDiskGroup::depth（0 = L0 级）
	uint32_t clusterOffset;   ///< 连续簇区间起点
	uint32_t clusterCount;    ///< 连续簇区间长度
};

/** 一条簇的规格。`refined = -1` = L0（扫描判据）；非 -1 必须指向**更浅** group（读取侧要求）。 */
struct ClusterDesc {
	uint32_t group;
	int32_t refined;
};

/** 一张合成产物的规格（三个计数全由两张表推导，杜绝「表与计数不符」这种测试自身 bug）。 */
struct ProductSpec {
	uint32_t primCount;
	std::vector<GroupDesc> groups;
	std::vector<ClusterDesc> clusters;
};

/**
 * 把规格铺成 `parseMeshletAsset` 认得的字节流（段序同 format.h：头 → Prim → Vertex →
 * Clusters → Tris → Remap → Groups → Nodes，段间 16B 对齐 0x00 填充）。
 *
 * 每簇固定 1 三角 / 1 顶点（triOffset = i×3、remapOffset = i、remap 值恒 0）——读取侧的
 * 「Σ triCount×3 == Tris 段字节」「remap 下标 < vertexCount」两条因此恒成立，本文件只测
 * 它**不查**的那四条。
 */
std::vector<std::byte> buildProduct(const ProductSpec& spec) {
	const uint32_t clusterCount = static_cast<uint32_t>(spec.clusters.size());
	const uint32_t groupCount = static_cast<uint32_t>(spec.groups.size());
	const uint32_t vertexCount = 1;

	uint64_t off = kHdrBytes;
	const uint64_t offPrim = off;
	off = alignUp16(off + spec.primCount * kPrimBytes);
	const uint64_t offVertex = off;
	off = alignUp16(off + vertexCount * kVertexBytes);
	const uint64_t offCluster = off;
	off = alignUp16(off + clusterCount * kClusterBytes);
	const uint64_t offTris = off;
	off = alignUp16(off + clusterCount * 3);
	const uint64_t offRemap = off;
	off = alignUp16(off + clusterCount * 4);
	const uint64_t offGroup = off;
	off = alignUp16(off + groupCount * kGroupBytes);
	const uint64_t offNode = off;
	off = alignUp16(off + kNodeBytes);

	std::vector<std::byte> bytes(static_cast<size_t>(off), std::byte { 0 });
	auto* base = bytes.data();

	auto* h = reinterpret_cast<MeshletDiskHeader*>(base);
	std::memcpy(h->magic, kMeshletMagic, 8);
	h->formatVersion = 1;
	h->toolVersion = 1;
	h->primCount = spec.primCount;
	h->vertexCount = vertexCount;
	h->clusterCount = clusterCount;
	h->groupCount = groupCount;
	h->nodeCount = 1;
	h->levelCount = 2;
	h->nodeWidth = 8;
	h->l0TriangleCount = clusterCount;
	h->totalTriangleCount = clusterCount;
	h->offPrimTable = offPrim;
	h->offVertexPool = offVertex;
	h->offClusters = offCluster;
	h->offTris = offTris;
	h->offRemap = offRemap;
	h->offGroups = offGroup;
	h->offNodes = offNode;

	for (uint32_t p = 0; p < spec.primCount; ++p)
		reinterpret_cast<MeshletDiskPrim*>(base + offPrim)[p] = { 0, vertexCount, 3, p };
	auto* clusters = reinterpret_cast<MeshletDiskCluster*>(base + offCluster);
	for (uint32_t i = 0; i < clusterCount; ++i)
		clusters[i] = { spec.clusters[i].group, spec.clusters[i].refined, 1, 1, i * 3, i,
			{ 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f };
	auto* groups = reinterpret_cast<MeshletDiskGroup*>(base + offGroup);
	for (uint32_t i = 0; i < groupCount; ++i)
		groups[i] = { spec.groups[i].depth, spec.groups[i].prim, { 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f,
			spec.groups[i].clusterOffset, spec.groups[i].clusterCount };
	reinterpret_cast<MeshletDiskNode*>(base + offNode)[0] = {
		{ 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f, -1, 0, 0 };
	return bytes;
}

/**
 * 合成产物 → 解析 → 扫描。parse **必须**成功：反向用例验的是「读取器认得的合法产物仍违反
 * 逐 prim 结构不变量」，parse 就失败说明合成数据自己写错了，此时 REQUIRE 直接红（不是假绿）。
 */
std::expected<MeshletRangeScan, std::string> scanBuilt(const ProductSpec& spec) {
	auto parsed = parseMeshletAsset(buildProduct(spec));
	REQUIRE(parsed.has_value());
	return scanClusterRanges(*parsed);
}

// ─── 正向锚点（期望值手算，出处 = 下面这段推导，不取被测码）─────────────────────────
/*
 * 三条 prim / 五条 group / 十条簇：
 *   prim 0：簇 [0,5) = L0 {0,1} + 深 {2,3,4}；group 0 = [0,2) depth0、group 1 = [2,5) depth1
 *   prim 1：簇 [5,6) = L0 {5}；              group 2 = [5,1) depth0
 *   prim 2：簇 [6,10) = L0 {6,7,8} + 深 {9}；group 3 = [6,3) depth0、group 4 = [9,1) depth1
 * 手算期望：perPrim = {0,5,2} {5,1,1} {6,4,3}；合计簇 = 5+1+4 = 10；L0 合计 = 2+1+3 = 6。
 */
const ProductSpec& anchor() {
	static const ProductSpec spec {
		3,
		{ GroupDesc { 0, 0, 0, 2 }, GroupDesc { 0, 1, 2, 3 }, GroupDesc { 1, 0, 5, 1 },
		    GroupDesc { 2, 0, 6, 3 }, GroupDesc { 2, 1, 9, 1 } },
		{ ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 }, ClusterDesc { 1, 0 }, ClusterDesc { 1, 0 },
		    ClusterDesc { 1, 0 }, ClusterDesc { 2, -1 }, ClusterDesc { 3, -1 }, ClusterDesc { 3, -1 },
		    ClusterDesc { 3, -1 }, ClusterDesc { 4, 3 } },
	};
	return spec;
}

// ─── 反向用例表（同一张表喂「逐条归因」与「两两不同」两条用例，杜绝两处漂移）──────────
struct BadCase {
	const char* name;          ///< 坏形态名（INFO 定位）
	ProductSpec (*build)();    ///< 合成产物规格
	const char* needle;        ///< 期望错误串的唯一关键词
};

/** 段序错乱：prim 1 的 run 排在 prim 0 之前（表序不是 0,1,2,…）。 */
ProductSpec badRunOrder() {
	return { 2,
		{ GroupDesc { 1, 0, 0, 1 }, GroupDesc { 0, 0, 1, 1 } },
		{ ClusterDesc { 0, -1 }, ClusterDesc { 1, -1 } } };
}
/** prim 下标越界：唯一一条 group 写着 prim 5，而 primCount = 1。 */
ProductSpec badPrimOob() {
	return { 1, { GroupDesc { 5, 0, 0, 1 } }, { ClusterDesc { 0, -1 } } };
}
/** 某 prim 没有 group：primCount = 2 但表里只有 prim 0 的 run（该 prim 没有区间）。 */
ProductSpec badMissingPrim() {
	return { 2, { GroupDesc { 0, 0, 0, 1 } }, { ClusterDesc { 0, -1 } } };
}
/**
 * 同 prim 簇区间不相接：prim 0 的两段是 [0,3) 与 [5,2)，中间 [3,5) 属于 prim 1。
 * 🔴 这条**只有 invariant 2 能抓**：Σ group.clusterCount = 3+2+2 = 7 = clusterCount，摘掉
 * ④ 之后 prim 0 会算成 [0,7)（end 取最后一段的 off+cnt = 7）——invariant 3 直到 prim 1
 * 才响，错误串是 invariant 3，本用例钉的 "not contiguous" 关键词照样红。
 */
ProductSpec badNonContiguous() {
	return { 2,
		{ GroupDesc { 0, 0, 0, 3 }, GroupDesc { 0, 1, 5, 2 }, GroupDesc { 1, 0, 3, 2 } },
		{ ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 },
		    ClusterDesc { 2, -1 }, ClusterDesc { 2, -1 }, ClusterDesc { 1, 0 },
		    ClusterDesc { 1, 0 } } };
}
/** 逐 prim 区间有空洞：prim 0 = [0,3)、prim 1 = [4,6)（簇 3 不属于任何 prim）。 */
ProductSpec badHole() {
	return { 3,
		{ GroupDesc { 0, 0, 0, 3 }, GroupDesc { 1, 0, 4, 2 }, GroupDesc { 2, 0, 4, 1 } },
		{ ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 },
		    ClusterDesc { 2, -1 }, ClusterDesc { 1, -1 }, ClusterDesc { 1, -1 } } };
}
/**
 * 逐 prim 区间有**重叠**（另一半，主 agent 补）：prim 0 = [0,3)、prim 1 = [2,4)（簇 2、3 被两个
 * prim 各画一次；簇 4 无人认领）。每个 prim 各自连续 ⟹ invariant 2 放行。
 *
 * 🔴 **本用例钉的是「归因」不是「有没有响」**：把 `base != expect` 弱化成本地正确的 `base > expect`
 * （只挡空洞）时，重叠**仍会被末尾的 `expect != clusterCount` 抓到**（重叠 k 个 ⟹ 末点少 k），
 * 但错误串变成"没铺满到 clusterCount"而**不点名是哪个 prim**——实测：弱化后本用例的关键词判据红。
 * 所以「可归因」这条性质在这台判据上是**可测的**，而这条用例就是它的锚。
 *
 * ⚠ 合成数据必须满足读取侧的 `clusterCount == Σ group.clusterCount`（它不查区间互不相交）
 * ⟹ 簇数取 5 而不是 4，否则 parse 先拒（我第一版就栽在这里）。
 */
ProductSpec badOverlap() {
	return { 2,
		{ GroupDesc { 0, 0, 0, 3 }, GroupDesc { 1, 0, 2, 2 } },
		{ ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 },
		    ClusterDesc { 1, -1 }, ClusterDesc { 1, -1 } } };
}
/** L0 不是区间前缀：prim 0 的簇序 = L0, 非 L0, L0, 非 L0（前缀吃掉 1 条后仍冒出 L0）。 */
ProductSpec badL0Order() {
	return { 1,
		{ GroupDesc { 0, 0, 0, 2 }, GroupDesc { 0, 1, 2, 2 } },
		{ ClusterDesc { 0, -1 }, ClusterDesc { 1, 0 }, ClusterDesc { 1, -1 }, ClusterDesc { 1, 0 } } };
}

const std::vector<BadCase>& badCases() {
	static const std::vector<BadCase> table = {
		{ "段序错乱（prim 1 的 run 在前）", badRunOrder, "table order" },
		{ "prim 下标越界（prim 5 > primCount）", badPrimOob, "primCount is 1" },
		{ "某 prim 没有 group（段数 != primCount）", badMissingPrim, "owns no group" },
		{ "同 prim 簇区间不相接", badNonContiguous, "not contiguous" },
		{ "逐 prim 区间有空洞", badHole, "hole or overlap" },
		{ "逐 prim 区间有重叠（钉归因：弱化版会改报\"没铺满\"）", badOverlap, "hole or overlap" },
		{ "L0 不是 prim 区间的前缀", badL0Order, "L0 is not a prefix" },
	};
	return table;
}

// ─── 尾区写入（两源结构：读回侧偏移 = 手抄 scene.slang 将用的字面量）─────────────────
constexpr uint32_t kSlangClusterRangeLiteral = 722400;  ///< 手抄 = scene_data 的尾区基址
constexpr uint32_t kSlangClusterRangeStride = 16;       ///< 手抄 = ObjectClusterRange 的 stride
static_assert(kSceneDataClusterRangeOffset == kSlangClusterRangeLiteral,
	"尾区基址与本文件手抄值分家（scene.slang 侧字面量跟着漂）");
static_assert(kSceneDataClusterRangeStride == kSlangClusterRangeStride,
	"ObjectClusterRange stride 与本文件手抄值分家");
static_assert(kSceneDataClusterRangeOffset + kSceneDataClusterRangeBytes == kSceneDataBytes,
	"尾区必须是最后一个区（否则「越界写」的形态是踩下一个区，本测试的哨兵判据判不住）");

/** staging 副本：整块填 0x5a + 尾部 64B 哨兵（本区是尾区，越界即写出 buffer 尾）。 */
std::vector<std::byte> makeStaging() {
	return std::vector<std::byte>(kSceneDataBytes + 64, std::byte { 0x5a });
}

uint32_t readU32(const std::vector<std::byte>& buffer, uint64_t byteOffset) {
	uint32_t value = 0;
	std::memcpy(&value, buffer.data() + byteOffset, sizeof(value));
	return value;
}

/** [from, to) 全 0？ */
bool regionAllZero(const std::vector<std::byte>& buffer, uint64_t from, uint64_t to) {
	for (uint64_t i = from; i < to; ++i)
		if (buffer[i] != std::byte { 0 })
			return false;
	return true;
}

/** [from, to) 内第一个 != fill 的下标，没有则 -1（一次 CHECK 就报出首个差异位置）。 */
int64_t firstByteNot(const std::vector<std::byte>& buffer, uint64_t from, uint64_t to,
	std::byte fill) {
	for (uint64_t i = from; i < to; ++i)
		if (buffer[i] != fill)
			return static_cast<int64_t>(i);
	return -1;
}

}  // namespace

TEST_CASE("正向锚点：逐 prim 区间与两个合计等于手算常量（扫描误杀一切时这条先红）") {
	const auto scan = scanBuilt(anchor());
	REQUIRE(scan.has_value());

	const MeshletPrimRange expected [] {
		{ 0u, 5u, 2u },  // prim 0：簇 [0,5)，L0 前缀 2
		{ 5u, 1u, 1u },  // prim 1：簇 [5,6)，L0 前缀 1
		{ 6u, 4u, 3u },  // prim 2：簇 [6,10)，L0 前缀 3
	};
	REQUIRE(scan->perPrim.size() == std::size(expected));
	for (size_t i = 0; i < std::size(expected); ++i) {
		CHECK(scan->perPrim[i].base == expected[i].base);
		CHECK(scan->perPrim[i].count == expected[i].count);
		CHECK(scan->perPrim[i].l0Count == expected[i].l0Count);
	}
	CHECK(scan->totalClusters == 10u);   // 5 + 1 + 4
	CHECK(scan->totalL0Clusters == 6u);  // 2 + 1 + 3
}

TEST_CASE("group 表内同 prim 的两段逆序书写 → 区间不变（merge 自己按起点排序）") {
	// 表序把**深**段写在**浅**段之前（簇的 group/refined 跟着改，否则读取侧的
	// 「refined 必须指向更浅 group」先拒掉，到不了扫描器）。摘掉 mergeRunIntoRange 里的
	// std::sort ⟹ base 取表序首段起点 2、第二段起点 0 != end 5 → invariant 2 误报，本用例红。
	const auto scan = scanBuilt(ProductSpec { 1,
		{ GroupDesc { 0, 1, 2, 3 }, GroupDesc { 0, 0, 0, 2 } },
		{ ClusterDesc { 1, -1 }, ClusterDesc { 1, -1 }, ClusterDesc { 0, 1 }, ClusterDesc { 0, 1 },
		    ClusterDesc { 0, 1 } } });
	REQUIRE(scan.has_value());
	CHECK(scan->perPrim[0].base == 0u);
	CHECK(scan->perPrim[0].count == 5u);
	CHECK(scan->perPrim[0].l0Count == 2u);
	CHECK(scan->totalClusters == 5u);
	CHECK(scan->totalL0Clusters == 2u);
}

TEST_CASE("空产物（0 prim / 0 簇 / 0 group）→ 成功且两个合计为 0（不是漏扫的伪装）") {
	const auto scan = scanBuilt(ProductSpec { 0, {}, {} });
	REQUIRE(scan.has_value());
	CHECK(scan->perPrim.empty());
	CHECK(scan->totalClusters == 0u);
	CHECK(scan->totalL0Clusters == 0u);
}

TEST_CASE("全 L0 产物（无细分层）→ l0Count == count（前缀 = 整段的边界形态）") {
	const auto scan = scanBuilt(ProductSpec { 1, { GroupDesc { 0, 0, 0, 3 } },
		{ ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 }, ClusterDesc { 0, -1 } } });
	REQUIRE(scan.has_value());
	REQUIRE(scan->perPrim.size() == 1u);
	CHECK(scan->perPrim[0].l0Count == 3u);
	CHECK(scan->perPrim[0].l0Count == scan->perPrim[0].count);
	CHECK(scan->totalL0Clusters == 3u);
}

TEST_CASE("反向：全部坏形态各自报出各自的不变量（关键词逐条钉死）") {
	for (const auto& bad : badCases()) {
		const auto scan = scanBuilt(bad.build());
		INFO("坏形态：", bad.name);
		REQUIRE_FALSE(scan.has_value());
		CHECK(scan.error().find(bad.needle) != std::string::npos);
	}
}

TEST_CASE("反向：四条不变量的错误串两两不同（可归因判据本身也要有牙）") {
	std::vector<std::string> errors;
	for (const auto& bad : badCases()) {
		const auto scan = scanBuilt(bad.build());
		INFO("坏形态：", bad.name);
		REQUIRE_FALSE(scan.has_value());
		errors.push_back(scan.error());
	}
	REQUIRE(errors.size() == badCases().size());
	for (size_t i = 0; i < errors.size(); ++i)
		for (size_t j = i + 1; j < errors.size(); ++j)
			CHECK_FALSE(errors[i] == errors[j]);
	// 四条不变量各至少一条：前缀 invariant 1..4 必须都在集合里出现过
	for (const char* prefix : { "invariant 1", "invariant 2", "invariant 3", "invariant 4" }) {
		INFO("缺少不变量：", prefix);
		CHECK(std::ranges::any_of(errors,
			[&](const std::string& e) { return e.find(prefix) != std::string::npos; }));
	}
}

TEST_CASE("前置守卫：视图大小与 header 计数不符 → 扫描器自己出声（不静默交出错区间）") {
	// 正常路径由 parseMeshletAsset 保证两侧一致；本用例直接改**已解析视图**的 header 计数，
	// 模拟「调用方绕过读取侧直接喂视图」——此时错的是静默的区间，必须被前置挡住。
	// （MeshletAsset 禁拷贝 ⟹ 两次各自 parse，不复制视图。）
	auto groupsHeader = parseMeshletAsset(buildProduct(anchor()));
	REQUIRE(groupsHeader.has_value());
	groupsHeader->header.groupCount += 1;
	const auto groupsErr = scanClusterRanges(*groupsHeader);
	REQUIRE_FALSE(groupsErr.has_value());
	CHECK(groupsErr.error().find("groups view holds") != std::string::npos);

	auto clustersHeader = parseMeshletAsset(buildProduct(anchor()));
	REQUIRE(clustersHeader.has_value());
	clustersHeader->header.clusterCount += 1;
	const auto clustersErr = scanClusterRanges(*clustersHeader);
	REQUIRE_FALSE(clustersErr.has_value());
	CHECK(clustersErr.error().find("clusters view holds") != std::string::npos);
	CHECK(groupsErr.error() != clustersErr.error());
}

TEST_CASE("尾区写入：逐字段落位 + 有效段之外清零 + 不踩前一个尾区与 buffer 尾") {
	auto staging = makeStaging();
	const std::array<ObjectClusterRange, 2> ranges {
		ObjectClusterRange { 0u, 5u, 2u, 0u },
		ObjectClusterRange { 5u, 1u, 1u, 0u },
	};
	REQUIRE(writeClusterRangeRegion(staging, ranges));

	// ① 逐字段落位（读回侧偏移 = 本文件手抄字面量，两源对账）
	for (size_t i = 0; i < ranges.size(); ++i) {
		const uint64_t slot = kSlangClusterRangeLiteral + i * kSlangClusterRangeStride;
		CHECK(readU32(staging, slot + 0) == ranges[i].base);
		CHECK(readU32(staging, slot + 4) == ranges[i].count);
		CHECK(readU32(staging, slot + 8) == ranges[i].l0Count);
		CHECK(readU32(staging, slot + 12) == ranges[i].reserved);
	}
	// ② 有效段之后清零（★1-a2 的「无簇产物实例 = 空区间」依赖这条）
	const uint64_t usedEnd = kSlangClusterRangeLiteral + ranges.size() * kSlangClusterRangeStride;
	const uint64_t regionEnd = kSlangClusterRangeLiteral + kSceneDataClusterRangeBytes;
	CHECK(regionAllZero(staging, usedEnd, regionEnd));
	// ③ 尾区之前（含前一个尾区湿面 @722368）一个字节都没被踩（抓「基址用错区」）
	CHECK(firstByteNot(staging, 0, kSlangClusterRangeLiteral, std::byte { 0x5a }) == -1);
	// ④ 没写出 buffer 尾（哨兵区）
	CHECK(firstByteNot(staging, kSceneDataBytes, staging.size(), std::byte { 0x5a }) == -1);
}

TEST_CASE("尾区写入：门关（表空）⟹ 整区全 0，其余字节逐字节不动（红线 A 的构造性保证）") {
	auto staging = makeStaging();
	REQUIRE(writeClusterRangeRegion(staging, std::span<const ObjectClusterRange> {}));
	CHECK(regionAllZero(staging, kSceneDataClusterRangeOffset,
		kSceneDataClusterRangeOffset + kSceneDataClusterRangeBytes));
	CHECK(firstByteNot(staging, 0, kSceneDataClusterRangeOffset, std::byte { 0x5a }) == -1);
}

TEST_CASE("尾区写入：容量边界 = 恰好满表必须成功（`>` 写成 `>=` 的漏检形态）") {
	auto full = std::vector<ObjectClusterRange>(kSceneDataClusterRangeCapacity);
	for (size_t i = 0; i < full.size(); ++i)
		full[i] = ObjectClusterRange { static_cast<uint32_t>(i), 1u, 1u, 0u };

	auto staging = makeStaging();
	REQUIRE(writeClusterRangeRegion(staging, full));
	// 最后一槽真的写进去了（不是「返回成功但少写一条」）
	const uint64_t last = kSceneDataClusterRangeOffset
		+ (full.size() - 1) * kSceneDataClusterRangeStride;
	CHECK(readU32(staging, last) == static_cast<uint32_t>(full.size() - 1));
	CHECK(readU32(staging, last + 4) == 1u);

	auto overflow = full;
	overflow.push_back(ObjectClusterRange { 0u, 0u, 0u, 0u });
	auto staging2 = makeStaging();
	CHECK_FALSE(writeClusterRangeRegion(staging2, overflow));
}

}  // namespace RGL
