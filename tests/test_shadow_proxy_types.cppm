module;
#include <doctest/doctest.h>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）

/**
 * test_shadow_proxy_types —— shadow proxy 批次 N10 的 P2：钉段表建表纯函数 + texel 门。
 *
 * 被测对象 = `rgl/shadow_proxy_types.cppm` + `rgl/shadow_proxy_types_impl.cppm`（rlbase）。
 *
 * ## 输入 = 合成 v2 产物字节流经 `parseMeshletAsset` + `scanClusterRanges` 进建表器
 * 与 `test_meshlet_lod` 同法：合成缓冲按 format.h 段序铺字节（v2 尾部多一段 MustL0），走**真实
 * 读取路径**。反向用例的输入是「读取器与区间扫描都认得的合法产物」——五条不变量之所以要单独验，
 * 正因为它们**不在**读取器（也不在区间扫描）的校验清单里（读取器只判 MustL0 段的**字节边界**）。
 * 🔴 不依赖 184MB 真产物；真产物读数（13 条 / Σ 被钉簇数）由启动日志常驻出声（装配层）。
 *
 * ## 正向锚点（期望值手算写死，出处 = 下方 anchor() 注释，不抄被测码）
 * 三个 prim 的形态：prim 0 = 钉段 2 簇 + 未钉 L0 1 簇 + 粗级 1 簇；prim 1 = 钉段 1 簇；
 * prim 2 = 有 L0 但**无钉段**（常态）。五个 objectSlot 覆盖三种合法条目形态：有钉段 /
 * 同 prim 的第二个实例（多实例）/ 不属于本产物（kShadowProxyNoPrim）/ 本产物但无钉段。
 * **radius 的闭式期望** = max_i(|center_i − center_0| + radius_i)：prim 0 的段内首簇球心在原点、
 * 半径 1，第二簇球心 (4,0,0) 半径 2 ⟹ max(0+1, 4+2) = **6**。这个数**同时**钉住三件事：
 * 球心取首簇（不是取 max radius = 2）、加了簇半径（不是只算球心距 = 4）、取的是 max（不是求和）。
 *
 * ## 五条不变量各配反向用例，且**断言错误串关键词**
 * 五条前缀两两不同（`shadow proxy: invariant 1..5`），只断言「失败」会让恒假判据与坏输入无法
 * 区分，故逐条钉关键词，并另有一条「全部错误串两两不同」的独立用例（先例 = test_meshlet_lod /
 * test_meshlet_asset）。两条**前置**错误串（两份输入不同源 / primOfSlot 越界）也各一条用例。
 *
 * ## 已知不可达（写出来，别让「全绿」被读成「全验过」）
 *   · invariant 5 在「parse + scanClusterRanges 同源产物」上**数学不可达**（①②+ 区间扫描的
 *     「prim 区间铺满全表」⟹ 钉段必然落在簇表内）。可达形态 = **手搭 ranges**（ranges 本就是
 *     建表器的合法第二输入，test_meshlet_lod 的 inv3 另一半同法）——反向用例钉的是这条手搭形态。
 *   · 前置 B（`clusters.size() != header.clusterCount`）经 parse **不可达**（读取侧按 header 建
 *     span），且无手搭入口（span 无写面）——它是「不假设调用方过了读取侧」的自保，不虚报成已验。
 *
 * ## R2 的两条机器形态（口径 → shadow_proxy_types.cppm 的 pinActive 注释）
 *   ① **签名里没有相机**：一条函数指针声明 `bool (*const)(const Entry&, float, float) =
 *      &pinActive;` 把入参面钉成**逐字相同**的类型 —— 加必传参数、改类型、以及「加一枚带默认值
 *      的第四参数」（`is_invocable_v` 会放过的那种）全部编不过。用例共用这个指针，判据与调用
 *      同一份类型，不存在「测的是另一个重载」的缝。
 *   ② **档内恒定 / 跨档必变**：同一 texel 反复求值逐次相同（挡隐藏状态），
 *      沿 2 的幂档序列（texel 逐档翻倍）扫 8 档 ⟹ 结果单调且**恰翻一次**，且翻转点闭式
 *      = 2·radius·worldScale（把 `>=` 改 `>`、把 K 改成 2、把 radius 换成 count 都会让它红）。
 *
 * ## R7 的机器形态 = 本文件全部用例
 * 建表器与 texel 门都在**栈上**构造入参直接调用（无 VkDevice、无单例、无配置）——能调通本身
 * 就是边界成立；再加一条 `import` 面判据：本文件不 import 任何设备模块（`rules_layers` 的
 * 逆向 import 会在链接期点名，那条是 GUARD 不在这里重复）。
 *
 * ## 变异红线（哪个变异让我红；**全部实测** 2026-10-20，`cp` 备份还原（不带 mtime）、
 * 禁 git checkout；每条显式 `cmake --build build --target rgl_tests` 并核对构建返回码；
 * 每条还原后重跑基线全绿。执行记录 = /tmp/opencode/n10p2/mutation_results.txt）
 *   ① invariant 1 判据短路（`e.clusterOffset != range.base` 前加 `false &&`）
 *     → 红 2 条（反向表 inv1 + 两两不同）
 *   ② invariant 3 判据 `<=` 改 `<`（只挡逆序不挡重复）→ 红 1 条（反向表 inv3 的重复形态漏网；
 *     「两两不同」那条**不红** —— 漏网后它落到 invariant 1 上，错误串与 mutPrefix 那条数字不同，
 *     两两不同判据看不出来 ⟹ 这条变异只由 inv3 反向用例单独守）
 *   ③ radius 公式漏加簇半径（`+ c.radius` 删掉）→ 红 1 条（正向锚点 6→4，3 个断言）
 *     ⚠ R2 那两条**不红**：它们用手工 Entry，不经建表器 ⟹ radius 的闭式只由正向锚点守
 *   ④ texel 门 `>=` 改 `>` → 红 1 条（闭式翻转点的「恰等」半边）
 *   ⑤ `count > 0` 前置短路（恒真）→ 红 1 条（无钉段条目在 texel=0 下必须恒 false）
 *   ⑥ 输入同源前置短路 → 红 1 条（手搭 ranges 的 primCount 不符用例）
 *
 * ## 真产物端到端变异（引擎侧，2026-10-20 实测；改 build/rgl/models/ 下的 v2 产物本体）
 *   · 头部 `mustL0Count` 改 0 → INFO「MustL0 条目=0 被钉 objectSlot=0 Σ被钉簇数=0」+ WARNING
 *     「门开了但产物没有 MustL0 段」（= 空表不是错误这条在真产物上成立）
 *   · 第一条 `clusterOffset` 0→1 → ERROR「invariant 1 … prim 696 pins from cluster 1 but its
 *     cluster range starts at 22443」+ 表不建，**截图仍是出厂锚 3801ad63…**（= 「不 emplace
 *     与门关逐位相同」的构造性保证在真产物上成立）
 *   · 整段 13 条清零 → ERROR「invariant 3 … entry 1 names prim 0 while entry 0 already named 0」
 *     （= 读取侧只判字节边界、语义判据真在建表器里，不在读取器里）
 */
export module test_shadow_proxy_types;

import rlstd;
import meshlet_asset;      // MeshletAsset / parseMeshletAsset / MeshletDiskMustL0
import meshlet_ranges;     // scanClusterRanges / MeshletRangeScan
import shadow_proxy_types; // buildShadowProxyTable / ShadowProxyTable / pinActive

namespace RGL {
namespace {

// ─── 合成产物（规格常量按 format.h 头注释手抄 = 期望值的独立出处，不取被测模块常量）─────
constexpr size_t kHdrBytes = 192, kPrimBytes = 16, kClusterBytes = 44, kGroupBytes = 36,
                 kNodeBytes = 32, kVertexBytes = 48, kMustL0Bytes = 12;

/** 写侧段对齐（format.h `kSectionAlign` 口径的手抄）。 */
uint64_t alignUp16(uint64_t v) { return (v + 15) / 16 * 16; }

struct GroupDesc {
	uint32_t prim;
	int32_t depth;
	uint32_t clusterOffset;
	uint32_t clusterCount;
};

/** 簇规格带**包围体**：radius 闭式判据的正向锚点要非平凡球心距与簇半径（恒 0/1 会蒙混）。 */
struct ClusterDesc {
	uint32_t group;
	int32_t refined;
	uint32_t triCount;
	float cx, cy, cz, radius;
};

struct MustL0Desc {
	uint32_t prim, clusterOffset, clusterCount;
};

struct ProductSpec {
	uint32_t primCount;
	uint32_t levelCount;
	std::vector<GroupDesc> groups;
	std::vector<ClusterDesc> clusters;
	std::vector<MustL0Desc> mustL0;
	bool writeMustL0 = true;  ///< false = v1 形态（无尾部段，头部那两枚字段留 0）
};

/**
 * 把规格铺成 `parseMeshletAsset` 认得的字节流（段序同 format.h，v2 尾部追加 MustL0 段）。
 * 每簇 1 顶点（remap 恒指 0 号）、triCount 恒 1：读取侧的段字节对账 / refined 更浅 /
 * Σ group 覆盖 恒成立（由规格自己守），本文件只测读取器与区间扫描**都不查**的那五条。
 */
std::vector<std::byte> buildProduct(const ProductSpec& spec) {
	const uint32_t clusterCount = static_cast<uint32_t>(spec.clusters.size());
	const uint32_t groupCount = static_cast<uint32_t>(spec.groups.size());
	const uint32_t vertexCount = 1;
	uint64_t triBytes = 0;
	for (const ClusterDesc& c : spec.clusters)
		triBytes += c.triCount * 3;

	uint64_t off = kHdrBytes;
	const uint64_t offPrim = off;
	off = alignUp16(off + spec.primCount * kPrimBytes);
	const uint64_t offVertex = off;
	off = alignUp16(off + vertexCount * kVertexBytes);
	const uint64_t offCluster = off;
	off = alignUp16(off + clusterCount * kClusterBytes);
	const uint64_t offTris = off;
	off = alignUp16(off + triBytes);
	const uint64_t offRemap = off;
	off = alignUp16(off + clusterCount * 4);
	const uint64_t offGroup = off;
	off = alignUp16(off + groupCount * kGroupBytes);
	const uint64_t offNode = off;
	off = alignUp16(off + kNodeBytes);
	const uint64_t offMustL0 = off;
	off += spec.writeMustL0 ? spec.mustL0.size() * kMustL0Bytes : 0;

	std::vector<std::byte> bytes(static_cast<size_t>(off), std::byte { 0 });
	auto* base = bytes.data();

	auto* h = reinterpret_cast<MeshletDiskHeader*>(base);
	std::memcpy(h->magic, kMeshletMagic, 8);
	h->formatVersion = spec.writeMustL0 ? 2 : 1;
	h->toolVersion = 1;
	h->primCount = spec.primCount;
	h->vertexCount = vertexCount;
	h->clusterCount = clusterCount;
	h->groupCount = groupCount;
	h->nodeCount = 1;
	h->levelCount = spec.levelCount;
	h->nodeWidth = 8;
	h->offPrimTable = offPrim;
	h->offVertexPool = offVertex;
	h->offClusters = offCluster;
	h->offTris = offTris;
	h->offRemap = offRemap;
	h->offGroups = offGroup;
	h->offNodes = offNode;
	h->offMustL0 = spec.writeMustL0 ? offMustL0 : 0;
	h->mustL0Count = spec.writeMustL0 ? static_cast<uint32_t>(spec.mustL0.size()) : 0;

	for (uint32_t p = 0; p < spec.primCount; ++p)
		reinterpret_cast<MeshletDiskPrim*>(base + offPrim)[p] = { 0, vertexCount, 3, p };
	auto* clusters = reinterpret_cast<MeshletDiskCluster*>(base + offCluster);
	uint32_t triCursor = 0;
	for (uint32_t i = 0; i < clusterCount; ++i) {
		const ClusterDesc& c = spec.clusters[i];
		clusters[i] = { c.group, c.refined, c.triCount, 1, triCursor, i,
			{ c.cx, c.cy, c.cz }, c.radius, 0.0f };
		triCursor += c.triCount * 3;
	}
	auto* groups = reinterpret_cast<MeshletDiskGroup*>(base + offGroup);
	for (uint32_t i = 0; i < groupCount; ++i)
		groups[i] = { spec.groups[i].depth, spec.groups[i].prim, { 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f,
			spec.groups[i].clusterOffset, spec.groups[i].clusterCount };
	reinterpret_cast<MeshletDiskNode*>(base + offNode)[0] = {
		{ 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f, -1, 0, 0 };
	if (spec.writeMustL0) {
		auto* pins = reinterpret_cast<MeshletDiskMustL0*>(base + offMustL0);
		for (size_t i = 0; i < spec.mustL0.size(); ++i)
			pins[i] = { spec.mustL0[i].prim, spec.mustL0[i].clusterOffset,
				spec.mustL0[i].clusterCount };
	}
	return bytes;
}

/**
 * 锚点产物（口径 → 文件头「正向锚点」）。区间扫描的期望（手算，不抄被测码）：
 * prim 0 = base 0 / count 4 / l0Count 3；prim 1 = base 4 / count 2 / l0Count 2；
 * prim 2 = base 6 / count 1 / l0Count 1。
 */
ProductSpec anchorSpec() {
	return ProductSpec {
		.primCount = 3,
		.levelCount = 2,
		.groups = { { 0, 0, 0, 3 }, { 0, 1, 3, 1 }, { 1, 0, 4, 2 }, { 2, 0, 6, 1 } },
		.clusters = {
			{ 0, -1, 1, 0.0f, 0.0f, 0.0f, 1.0f },   // c0  prim0 L0  段内首簇（球心原点）
			{ 0, -1, 1, 4.0f, 0.0f, 0.0f, 2.0f },   // c1  prim0 L0  球心距 4 + 半径 2 = 6
			{ 0, -1, 1, 0.0f, 0.0f, 0.0f, 0.5f },   // c2  prim0 L0  未钉（段长只到 2）
			{ 1, 0, 1, 0.0f, 0.0f, 0.0f, 1.0f },    // c3  prim0 粗级（refined → g0，更浅）
			{ 2, -1, 1, 1.0f, 2.0f, 2.0f, 0.25f },  // c4  prim1 L0  被钉（单簇段 → radius 0.25）
			{ 2, -1, 1, 0.0f, 0.0f, 0.0f, 0.5f },   // c5  prim1 L0  未钉
			{ 3, -1, 1, 0.0f, 0.0f, 0.0f, 1.0f },   // c6  prim2 L0  该 prim 无钉段
		},
		.mustL0 = { { 0, 0, 2 }, { 1, 4, 1 } },
	};
}

/** 已解析资产 + 已扫区间（建表器的两个输入；MeshletAsset 禁拷贝 ⟹ 移动持有）。 */
struct Parsed {
	MeshletAsset asset;
	MeshletRangeScan ranges;
};

/** 合成产物 → parse → scanClusterRanges，两步都**必须**成功（前置失败 = 合成数据自己写错了）。 */
Parsed parseRanges(const ProductSpec& spec) {
	auto parsed = parseMeshletAsset(buildProduct(spec));
	REQUIRE(parsed.has_value());
	auto ranges = scanClusterRanges(*parsed);
	REQUIRE(ranges.has_value());
	return Parsed { std::move(*parsed), std::move(*ranges) };
}

/** 锚点场景的槽位反查表：5 个 objectSlot（口径 → 文件头「正向锚点」）。 */
std::vector<uint32_t> anchorSlots() {
	return { 0, kShadowProxyNoPrim, 0, 1, 2 };
}

/** 逐字段比较（被测结构体不带 operator==——接口按任务书原样，比较器住测试侧）。 */
bool entryEq(const ShadowProxyTable::Entry& a, const ShadowProxyTable::Entry& b) {
	return a.start == b.start && a.count == b.count && a.radius == b.radius;
}

// ─── 变异表（反向用例的唯一真相源：单条用例与「两两不同」用例共用，杜绝两处漂移）──────
struct Mutation {
	const char* name;
	void (*apply)(ProductSpec&);
	std::string needle;  ///< 期望错误串的关键词（各条**两两不同**）
};

void mutPrefix(ProductSpec& s) { s.mustL0[0].clusterOffset = 1; }  // ① 不再是前缀
void mutTooLong(ProductSpec& s) { s.mustL0[1].clusterCount = 3; }  // ② 超出 l0Count(1)=2
void mutDuplicate(ProductSpec& s) { s.mustL0[1].prim = 0; }        // ③ 同 prim 两条
void mutDescending(ProductSpec& s) {
	s.mustL0 = { { 1, 4, 1 }, { 0, 0, 2 } };  // ③ 逆序
}
void mutPrimOob(ProductSpec& s) { s.mustL0[1].prim = 7; }          // ④ prim 越界
// ⑤ 不在这张表里：parse + scanClusterRanges 同源的产物上它**数学不可达**（①②+ 区间铺满全表
// ⟹ 钉段必然落在簇表内），改 mustL0 的字节只会先撞 ①。它的可达形态 = 手搭 ranges，
// 由独立用例钉（见 TEST_CASE「invariant 5 …（手搭 ranges 才可达）」）。
const std::vector<Mutation>& mutations() {
	static const std::vector<Mutation> table = {
		{ "invariant 1：钉段起点挪 1（不再是 prim 簇区间前缀）", mutPrefix,
		    "invariant 1 (pinned run is not a prefix" },
		{ "invariant 2：钉段簇数超出该 prim 的 L0 段", mutTooLong,
		    "invariant 2 (pinned cluster count exceeds" },
		{ "invariant 3：同一 prim 出现两条", mutDuplicate,
		    "invariant 3 (MustL0 entries are not strictly ascending" },
		{ "invariant 3：条目按 prim 逆序", mutDescending,
		    "invariant 3 (MustL0 entries are not strictly ascending" },
		{ "invariant 4：prim 下界越出 primCount", mutPrimOob,
		    "invariant 4 (MustL0 entry references prim out of range" },
	};
	return table;
}

const Mutation& mutationByName(const char* name) {
	for (const Mutation& m : mutations())
		if (std::strcmp(m.name, name) == 0)
			return m;
	FAIL("变异表里查无此名: " << name);
	return mutations().front();
}

/** 跑一条变异：建表必须失败，且错误串含该变异自己的关键词（= 可归因）。 */
void checkMutationRejected(const Mutation& m) {
	ProductSpec spec = anchorSpec();
	m.apply(spec);
	Parsed p = parseRanges(spec);
	auto built = buildShadowProxyTable(p.asset, p.ranges, anchorSlots());
	INFO("变异: " << std::string(m.name));
	REQUIRE_FALSE(built.has_value());
	INFO("错误串: " << built.error());
	CHECK(built.error().find(m.needle) != std::string::npos);
}

}  // namespace

// ─── 正向 ────────────────────────────────────────────────────────────────────────

TEST_CASE("正向：合成 v2 产物 → 逐字段对上（含 radius 闭式、多实例、三种空条目）") {
	// 规格常量对账（手抄自 format.h；被测镜像漂了这几行先红——两侧 static_assert 互为对账）。
	CHECK(sizeof(MeshletDiskMustL0) == kMustL0Bytes);
	CHECK(sizeof(MeshletDiskCluster) == kClusterBytes);
	CHECK(sizeof(MeshletDiskGroup) == kGroupBytes);

	ProductSpec spec = anchorSpec();
	Parsed p = parseRanges(spec);
	// 区间扫描的期望先钉一遍（它是 invariant 1/2 的对照面，错了后面全是空中楼阁）
	REQUIRE(p.ranges.perPrim.size() == 3u);
	CHECK(p.ranges.perPrim[0].base == 0u);
	CHECK(p.ranges.perPrim[0].l0Count == 3u);
	CHECK(p.ranges.perPrim[1].base == 4u);
	CHECK(p.ranges.perPrim[1].l0Count == 2u);
	CHECK(p.ranges.perPrim[2].base == 6u);
	CHECK(p.asset.mustL0.size() == 2u);

	auto built = buildShadowProxyTable(p.asset, p.ranges, anchorSlots());
	REQUIRE(built.has_value());
	REQUIRE(built->bySlot.size() == 5u);
	// slot 0 / slot 2 = 同 prim 的两个实例（多实例形态必须各得一份同样的条目，不是只有一个非零）
	const ShadowProxyTable::Entry pinned { .start = 0, .count = 2, .radius = 6.0f };
	CHECK(entryEq(built->bySlot[0], pinned));
	CHECK(entryEq(built->bySlot[2], pinned));
	// slot 1 = 不属于本产物；slot 3 = prim 1（单簇段 → radius 恰为该簇半径 0.25）；
	// slot 4 = prim 2 有 L0 但无钉段。三种「全 0 条目」形态各自可达。
	CHECK(entryEq(built->bySlot[1], ShadowProxyTable::Entry {}));
	CHECK(entryEq(built->bySlot[3], (ShadowProxyTable::Entry { .start = 0, .count = 1, .radius = 0.25f })));
	CHECK(entryEq(built->bySlot[4], ShadowProxyTable::Entry {}));
}

TEST_CASE("正向：MustL0 段为空（v1 产物 / 门槛关着打包）→ 空表，不是错误") {
	// v1 形态：头部那两枚字段位在 reserved 里 = 0，连 offMustL0 都不校验（v1 兼容的全部要点）。
	ProductSpec v1 = anchorSpec();
	v1.writeMustL0 = false;
	v1.mustL0.clear();
	Parsed p1 = parseRanges(v1);
	CHECK(p1.asset.mustL0.empty());
	auto built = buildShadowProxyTable(p1.asset, p1.ranges, anchorSlots());
	REQUIRE(built.has_value());
	REQUIRE(built->bySlot.size() == 5u);
	for (const auto& e : built->bySlot)
		CHECK(entryEq(e, ShadowProxyTable::Entry {}));

	// v2 但零条（打包时门槛 0 = 不排序）：同一形态的另一条产地，也必须空表不报错。
	ProductSpec v2empty = anchorSpec();
	v2empty.mustL0.clear();
	Parsed p2 = parseRanges(v2empty);
	REQUIRE(p2.asset.mustL0.empty());
	auto built2 = buildShadowProxyTable(p2.asset, p2.ranges, anchorSlots());
	REQUIRE(built2.has_value());
	CHECK(built2->bySlot.size() == 5u);
}

// ─── 五条不变量反向（逐条钉关键词，先例 = test_meshlet_lod）─────────────────────────

TEST_CASE("反向：invariant 1 前缀性被破（钉段起点挪 1）→ 点名 invariant 1") {
	checkMutationRejected(mutationByName("invariant 1：钉段起点挪 1（不再是 prim 簇区间前缀）"));
}
TEST_CASE("反向：invariant 2 钉段超出 L0 段 → 点名 invariant 2") {
	checkMutationRejected(mutationByName("invariant 2：钉段簇数超出该 prim 的 L0 段"));
}
TEST_CASE("反向：invariant 3 同 prim 两条 → 点名 invariant 3") {
	checkMutationRejected(mutationByName("invariant 3：同一 prim 出现两条"));
}
TEST_CASE("反向：invariant 3 条目逆序 → 点名 invariant 3") {
	checkMutationRejected(mutationByName("invariant 3：条目按 prim 逆序"));
}
TEST_CASE("反向：invariant 4 prim 越界 → 点名 invariant 4") {
	checkMutationRejected(mutationByName("invariant 4：prim 下界越出 primCount"));
}

TEST_CASE("反向：invariant 5 钉段越出簇表（手搭 ranges 才可达）→ 点名 invariant 5") {
	// 为什么必须手搭：parse + scanClusterRanges 同源的产物上，①②+「prim 区间铺满全表」
	// 数学蕴含 ⑤ 成立（见文件头「已知不可达」）。手搭 ranges 是建表器的**合法输入形态**
	// （签名就收一个 MeshletRangeScan），不是绕过判据。
	// 判据顺序是 ①②③④ 先于 ⑤，所以光把 mustL0 的 offset 改大只会先撞 ①；要走到 ⑤，
	// 手搭的那一段必须**自洽地**把 base 也抬到同一个越界值（①② 过、⑤ 红）。
	ProductSpec spec = anchorSpec();
	spec.mustL0 = { { 0, 1000, 2 }, { 1, 4, 1 } };  // 读取侧只判 MustL0 的字节边界，照收
	Parsed p = parseRanges(spec);
	MeshletRangeScan fake;
	fake.perPrim.resize(spec.primCount);
	fake.perPrim[0] = MeshletPrimRange { .base = 1000, .count = 5, .l0Count = 5 };
	fake.perPrim[1] = MeshletPrimRange { .base = 4, .count = 2, .l0Count = 2 };
	auto built = buildShadowProxyTable(p.asset, fake, anchorSlots());
	REQUIRE_FALSE(built.has_value());
	INFO("错误串: " << built.error());
	CHECK(built.error().find("invariant 5 (pinned cluster range out of bounds") != std::string::npos);
}

TEST_CASE("反向：两份输入不同源（ranges 的 prim 数 ≠ header.primCount）→ 前置点名，不静默") {
	ProductSpec spec = anchorSpec();
	Parsed p = parseRanges(spec);
	MeshletRangeScan fake;
	fake.perPrim.resize(2);  // primCount = 3
	auto built = buildShadowProxyTable(p.asset, fake, anchorSlots());
	REQUIRE_FALSE(built.has_value());
	CHECK(built.error().find("range scan disagrees with the asset header") != std::string::npos);
}

TEST_CASE("反向：primOfSlot 给了不存在的 prim → 前置点名，不当成「该槽无钉段」放过") {
	ProductSpec spec = anchorSpec();
	Parsed p = parseRanges(spec);
	const std::vector<uint32_t> slots = { 0, 99 };
	auto built = buildShadowProxyTable(p.asset, p.ranges, slots);
	REQUIRE_FALSE(built.has_value());
	CHECK(built.error().find("primOfSlot[1] = prim 99 out of range") != std::string::npos);
}

TEST_CASE("反向：MustL0 段存在但 formatVersion=1 → 读取侧拒读（段表自相矛盾）") {
	// 读取侧那条（不是建表器的 invariant）：v1 产物的 mustL0Count 恒 0，非 0 = 头部自己打架。
	ProductSpec spec = anchorSpec();
	auto bytes = buildProduct(spec);
	reinterpret_cast<MeshletDiskHeader*>(bytes.data())->formatVersion = 1;
	auto parsed = parseMeshletAsset(std::move(bytes));
	REQUIRE_FALSE(parsed.has_value());
	CHECK(parsed.error().find("predates v2") != std::string::npos);
}

TEST_CASE("反向：MustL0 段被截掉尾部 → 读取侧拒读（不越界读）") {
	ProductSpec spec = anchorSpec();
	auto bytes = buildProduct(spec);
	// 砍掉最后一条 MustL0 条目的一半字节（段声明 2 条 = 24B，只留 18B）
	bytes.resize(bytes.size() - 6);
	auto parsed = parseMeshletAsset(std::move(bytes));
	REQUIRE_FALSE(parsed.has_value());
	CHECK(parsed.error().find("MustL0 section extends beyond file") != std::string::npos);
}

TEST_CASE("反向：全部变异的错误串两两不同（可归因判据本体）") {
	std::vector<std::string> errors;
	for (const Mutation& m : mutations()) {
		ProductSpec spec = anchorSpec();
		m.apply(spec);
		Parsed p = parseRanges(spec);
		auto built = buildShadowProxyTable(p.asset, p.ranges, anchorSlots());
		INFO("变异: " << std::string(m.name));
		REQUIRE_FALSE(built.has_value());
		errors.push_back(built.error());
	}
	// 三条前置/读取侧错误串也进同一张对照表（它们与五条 invariant 之间同样必须两两不同）
	{
		ProductSpec spec = anchorSpec();
		Parsed p = parseRanges(spec);
		MeshletRangeScan fake;
		fake.perPrim.resize(2);
		auto e1 = buildShadowProxyTable(p.asset, fake, anchorSlots());
		REQUIRE_FALSE(e1.has_value());
		errors.push_back(e1.error());
		auto e2 = buildShadowProxyTable(p.asset, p.ranges, std::vector<uint32_t> { 0, 99 });
		REQUIRE_FALSE(e2.has_value());
		errors.push_back(e2.error());
		MeshletRangeScan fake5;
		fake5.perPrim.resize(spec.primCount);
		fake5.perPrim[0] = MeshletPrimRange { .base = 1000, .count = 5, .l0Count = 5 };
		fake5.perPrim[1] = MeshletPrimRange { .base = 4, .count = 2, .l0Count = 2 };
		ProductSpec oob = spec;
		oob.mustL0 = { { 0, 1000, 2 }, { 1, 4, 1 } };  // ⑤ 的可达形态：①② 自洽地一起抬到越界值
		Parsed po = parseRanges(oob);
		auto e3 = buildShadowProxyTable(po.asset, fake5, anchorSlots());
		REQUIRE_FALSE(e3.has_value());
		errors.push_back(e3.error());
	}
	REQUIRE(errors.size() == 8u);
	for (size_t i = 0; i < errors.size(); ++i)
		for (size_t j = i + 1; j < errors.size(); ++j) {
			INFO("第 " << i << " 条与第 " << j << " 条塌成同一条串：\n  " << errors[i] << "\n  " << errors[j]);
			CHECK(errors[i] != errors[j]);
		}
}

// ─── R2：texel 门只吃离散量 ───────────────────────────────────────────────────────

// 🔴 签名里没有相机 / far / 时间：判据的入参面**恰好**是 (条目, 世界换算, 本级纹素)。
// 这条函数指针声明本身就是机器判据（编译期）：`&pinActive` 的类型必须**逐字**等于它——
// 加一枚必传参数、改任一参数类型、甚至**加一枚带默认值的第四参数**（函数指针类型仍带四个
// 槽位）都会让这行编不过。比 `is_invocable_v` 强（后者被默认参数放过），且不需要往 rlstd
// 补 <type_traits> 的名字。用例里通过 fnPinActive 调用它，判据与用例共用同一份类型。
bool (*const fnPinActive)(const ShadowProxyTable::Entry&, float, float) = &pinActive;

TEST_CASE("R2：texel 门档内恒定 + 2 的幂档序列上单调且恰翻一次，翻转点闭式 = 2·radius·worldScale") {
	// 期望值取**二进制精确**的组合：2 × 2.0f × 0.25f = 1.0f 恰为 2 的幂，
	// 「恰等 / 恰大 / 恰小」三个采样点都不引入舍入（0.04f 那种十进制数会把 `>=` 的边界
	// 变成浮点比较的运气问题）。
	const ShadowProxyTable::Entry e { .start = 0, .count = 4, .radius = 2.0f };
	const float worldScale = 0.25f;
	const float threshold = 2.0f * e.radius * worldScale;  // 闭式：1.0f
	CHECK(threshold == 1.0f);                              // 二进制精确这条自己也要被钉

	// ① 档内恒定：同一个 texel 反复求值必须逐次相同。不是恒真断言——它挡的是「把上次结果
	//    缓存进 static」「把级联号读进全局」这类隐藏状态形态（判据只准吃入参）。
	const bool first = fnPinActive(e, worldScale, 0.5f);
	for (int i = 0; i < 8; ++i)
		CHECK(fnPinActive(e, worldScale, 0.5f) == first);

	// ② 翻转点闭式（三个点把 `>=` / K=1 / 直径而非半径 三件事一起钉住）：
	CHECK(fnPinActive(e, worldScale, 0.5f));      // 明显小于阈值 → 钉
	CHECK(fnPinActive(e, worldScale, threshold)); // **恰等** → 钉（`>=`，不是 `>`）
	CHECK(fnPinActive(e, worldScale, 1.0f / 3.0f));  // 档内任意连续值 → 钉（阈值以下恒真）
	CHECK_FALSE(fnPinActive(e, worldScale, 1.0000001f));  // 恰大一点 → 不钉
	CHECK_FALSE(fnPinActive(e, worldScale, 2.0f));        // 明显大于 → 不钉
	// 半径而非直径（K 若被写成 2）：阈值必须恰在 2·r·s，不是 r·s
	CHECK(fnPinActive(e, worldScale, 0.9f));
	CHECK_FALSE(fnPinActive(e, worldScale, 1.1f));

	// ③ 跨档必变：texel 随 2 的幂档翻倍（`quantizeShadowDistance` 取 2 的幂 ⟹ r_c 与 texel
	//    同比例，计划 §3.2）。扫 8 档 = texel 逐档 ×2 ⟹ 结果单调、**恰翻一次**、
	//    且方向是「档越粗越不钉」。把判据改成读连续 far、或把 K 改成随档变的量，翻次数就不是 1。
	bool prev = false;
	int flips = 0;
	for (int k = 0; k < 8; ++k) {
		const float texel = threshold * static_cast<float>(1u << k) / 4.0f;  // 0.25…32，逐档 ×2
		const bool now = fnPinActive(e, worldScale, texel);
		if (k > 0 && now != prev)
			++flips;
		prev = now;
	}
	CHECK(flips == 1);
	CHECK(fnPinActive(e, worldScale, 0.25f));           // 首档（最细）必钉
	CHECK_FALSE(fnPinActive(e, worldScale, 32.0f));     // 末档（最粗）必不钉
}

TEST_CASE("R2 反向：count == 0 的条目在任何 texel 下恒 false（含 texel = 0 这种极端）") {
	// 阳性对照：同一 texel 下 count > 0 必须为 true，否则上面那条「恒 false」是恒真的假绿。
	const ShadowProxyTable::Entry none { .start = 0, .count = 0, .radius = 2.0f };
	const ShadowProxyTable::Entry pinned { .start = 0, .count = 1, .radius = 2.0f };
	CHECK_FALSE(fnPinActive(none, 0.25f, 0.0f));
	CHECK_FALSE(fnPinActive(none, 0.25f, 1e9f));
	CHECK(fnPinActive(pinned, 0.25f, 0.0f));  // ← 区分器：把 `count > 0` 短路掉这条就红
	CHECK_FALSE(fnPinActive(ShadowProxyTable::Entry {}, 0.25f, 0.0f));
}

}  // namespace RGL
