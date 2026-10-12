module;
#include <doctest/doctest.h>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）

/**
 * test_meshlet_lod —— 簇化轴 §18 批 A：逐 prim 选级扫描（远级联 LOD 第一刀的纯函数层）。
 *
 * 被测对象 = `rgl/meshlet_lod.cppm` + `rgl/meshlet_lod_impl.cppm`（rlbase）。
 *
 * ## 输入 = 合成产物字节流经 `parseMeshletAsset` + `scanClusterRanges` 进选级器
 * 与 `test_meshlet_ranges` 同法：合成缓冲按 format.h 段序铺字节，走**真实读取路径**；
 * 反向用例的输入是「读取器与区间扫描都认得的合法产物」——五条不变量之所以要单独验，
 * 正因为它们**不在**读取器（也不在区间扫描）的校验清单里。🔴 不依赖 184MB 真产物；
 * 真产物读数（27,487 / 2,832,120 / D=4 ≈ 3,670 簇 7.88% / D=5 ≈ 2,566 簇 5.10%）由启动
 * 日志常驻出声（批 B），口径 → `meshlet_lod.cppm` 文件头。
 * 每条合成产物 = 每簇 1 顶点 / triCount 逐簇给值（三角锚点要非平凡求和），毫秒级。
 *
 * ## 正向锚点（期望值手算写死，出处 = 下方 anchor() 推导，不抄被测码）
 * 四条 prim 的形态（多级 / 只有 L0 / 无簇）+ gap 产物（级集 {0,2}，deepest 与 l0 分家的
 * 最小形态）+ 全部聚合量手算见 anchor() 注释。`start` 的**相对性**与 **perPrim 下标↔prim
 * 映射**由 base-shift 产物单独钉（同 prim 换 base ⟹ start/level/triangles 不变、base 变）——
 * 聚合量抓不到「下标↔prim 错位」与「start 写成绝对下标」（后者在 anchor 上对 prim 0 恰好
 * 同值 base=0 会蒙混，必须换 base 才抓得住）。
 *
 * ## 五条不变量各配反向用例，且**断言错误串关键词**
 * 五条前缀两两不同（`meshlet lod: invariant 1..5`），只断言「失败」会让恒假判据与坏输入
 * 无法区分，故逐条钉关键词，并另有一条「全部错误串两两不同」的独立用例（先例 =
 * test_meshlet_ranges / test_meshlet_asset）。不变量 3 的**另一半**（depth0 段 > l0Count）
 * 经 parse 不可达（读取侧 DAG 校验挡住：级 0 簇只能 refined==-1），用**手搭 ranges** 构造
 * （ranges 本就是选级器的合法第二输入）——两条半边合起来把 `!=` 弱化成任一半边的变异都
 * 按得住（教训先例 = test_meshlet_ranges 变异⑫「判据短路 ≠ 判据弱化」）。
 *
 * ## 变异红线（哪个变异让我红；**全部实测** 2026-10-12，`cp` 备份还原（不带 mtime）、
 * 禁 git checkout；每条显式 `cmake --build build --target rgl_tests` 并核对构建返回码；
 * 每条还原后重跑基线全绿。执行记录 = /tmp/opencode/mutation_lod_results.txt）
 *   ① 不变量 1 clusters 判据短路 → 红 2 条（前置守卫 + 两两不同）
 *   ② 不变量 2 判据短路 → 红 2 条（反向表 inv2 + 两两不同）
 *   ③ 不变量 3 判据短路 → 红 3 条（反向表 inv3 + 手搭 ranges 另一半 + 两两不同）
 *   ④ 不变量 4 判据短路 → 红 2 条（反向表 inv4 + 两两不同）
 *   ⑤ 不变量 5 判据短路 → 红 2 条（反向表 inv5 + 两两不同）
 *   ⑥ 🔴 **弱化**：不变量 3 的 `!=` 改 `>`（只挡 depth0Len > l0Count 半边）→ 红 2 条
 *      （反向表 inv3 的 1<2 形态漏网 + 两两不同）；手搭 ranges 用例（2>0）仍绿 = 预期，
 *      它钉的是另一半边
 *   ⑦ 🔴 **弱化反方向**：不变量 3 的 `!=` 改 `<`（只挡 depth0Len < l0Count 半边）→ 红 1 条
 *      （手搭 ranges 用例：inv3 漏网后 inv4 抢先点名，"invariant 3" 关键词红）
 *   ⑧ 选级取反：deepest 的 `s.depth > levelD` 改 `>=`（D 本身被排除、恒回落浅一级）→ 红 7 条
 *      （D=1 锚点 / D=0 / D=5 / base-shift / Lcg 属性 / 两两不同 / 坏形态逐条点名——后两条红
 *      是因为 inv4 的可达形态本就靠 deepest 选级非空，选级一歪它就不触发）
 *   ⑨ 偏移错位：段 start 写成 `start + 1`（off-by-one）→ 红 6 条（D=1 锚点 / D=0 / D=5 /
 *      l0 对照 / base-shift / Lcg 属性）
 *   ⑩ histogram 落点错位（`[entry.level]` 改 `[0]`）→ 红 3 条（D=1 锚点 / D=5 / l0 对照）
 *   ⑪ l0 回落分支短路（`fallback == l0` 恒假，l0 退化成 deepest）→ 红 4 条
 *      （l0 对照 / Lcg 属性 / 两两不同 / 坏形态逐条点名——inv5 的可达形态本就靠 l0 空选）
 *   ⑫ 区间越界前置短路 → 红 1 条（手搭 ranges 越界用例；实测**判据红**非崩溃：越界读落在
 *      簇表尾部垃圾上先被 depth 前置点名，关键词不匹配即红）
 *   ⑬ depth 范围前置短路 → 红 1 条（levelCount 篡改用例点名「declared DAG levels disagree」；
 *      本轮实测未触发崩溃形态）
 *
 * ## 已知覆盖缺口（写出来，别让「全绿」被读成「全验过」）
 *   · `cluster.group` 越界前置经 parse **不可达**（读取侧「cluster.group 有界」先挡住），
 *     且无手搭入口（span 无写面）——它是「不假设调用方过了读取侧」的自保，不虚报成「已验」。
 *   · 不变量 5 在「不变量 1-4 全过」的输入上数学不可达（3+4 ⟹ 级 0 段存在 ⟹ 两种回落都
 *     非空）；可达形态 = `l0` 策略 × prim 缺级 0 段（此时 5 先于 4 点名，顺序理由见 impl
 *     scanOnePrim 注释）。反向用例钉的是这条可达形态，不是「5 普遍可达」。
 *   · 真产物对账（bistro 184MB）是**一次性**验证（数字在交付报告），不做常驻用例
 *     （测试不依赖构建产物，同 test_meshlet_ranges 的裁决）。
 *   · histogram 逐格落点只在正向锚点钉（Lcg 组只钉 size 与合计）——落点错位的变异 ⑩ 由
 *     正向锚点按死，Lcg 不重复。
 */
export module test_meshlet_lod;

import rlstd;
import meshlet_asset;    // MeshletAsset / parseMeshletAsset / 段规格常量
import meshlet_ranges;   // scanClusterRanges / MeshletRangeScan
import meshlet_lod;      // scanMeshletLod / MeshletLodScan
import test_support;     // Lcg（写死种子，禁真随机）

namespace RGL {
namespace {

// ─── 合成产物（规格常量按 format.h 头注释手抄 = 期望值的独立出处，不取被测模块常量）─────
constexpr size_t kHdrBytes = 192, kPrimBytes = 16, kClusterBytes = 44, kGroupBytes = 36,
                 kNodeBytes = 32, kVertexBytes = 48;

/** 写侧段对齐（format.h `kSectionAlign` 口径的手抄）。 */
uint64_t alignUp16(uint64_t v) { return (v + 15) / 16 * 16; }

/** 一条 group 的规格（包围体等其余字段选级不读，铺零）。 */
struct GroupDesc {
	uint32_t prim;           ///< MeshletDiskGroup::prim
	int32_t depth;           ///< MeshletDiskGroup::depth（0 = L0 级；选级器读的唯一簇级来源）
	uint32_t clusterOffset;  ///< 连续簇区间起点
	uint32_t clusterCount;   ///< 连续簇区间长度（0 = 无簇 prim 的合法形态）
};

/** 一条簇的规格。`refined = -1` = L0（区间扫描判据）；非 -1 必须指向**更浅** group（读取侧要求）。 */
struct ClusterDesc {
	uint32_t group;
	int32_t refined;
	uint32_t triCount;  ///< 三角锚点要非平凡求和，不再恒 1
};

/** 一张合成产物的规格（计数全由两张表推导，杜绝「表与计数不符」这种测试自身 bug）。 */
struct ProductSpec {
	uint32_t primCount;
	uint32_t levelCount;  ///< 选级器按它分配 histogram（篡改它 = 前置用例）
	std::vector<GroupDesc> groups;
	std::vector<ClusterDesc> clusters;
};

/**
 * 把规格铺成 `parseMeshletAsset` 认得的字节流（段序同 format.h：头 → Prim → Vertex →
 * Clusters → Tris → Remap → Groups → Nodes，段间 16B 对齐 0x00 填充）。
 *
 * 每簇 1 顶点（remap 恒指 0 号）、triCount 逐簇给值：读取侧「Σ triCount×3 == Tris 段字节」
 * 「remap 下标 < vertexCount」「refined 指向更浅 group」恒成立（refined 由规格自己守），
 * 本文件只测读取器与区间扫描**都不查**的那五条。
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
	h->levelCount = spec.levelCount;
	h->nodeWidth = 8;
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
	uint32_t triCursor = 0;  // 逐簇 triOffset = 前缀和（读取侧「tri 范围落在段内」靠它）
	for (uint32_t i = 0; i < clusterCount; ++i) {
		clusters[i] = { spec.clusters[i].group, spec.clusters[i].refined,
			spec.clusters[i].triCount, 1, triCursor, i, { 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f };
		triCursor += spec.clusters[i].triCount * 3;
	}
	auto* groups = reinterpret_cast<MeshletDiskGroup*>(base + offGroup);
	for (uint32_t i = 0; i < groupCount; ++i)
		groups[i] = { spec.groups[i].depth, spec.groups[i].prim, { 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f,
			spec.groups[i].clusterOffset, spec.groups[i].clusterCount };
	reinterpret_cast<MeshletDiskNode*>(base + offNode)[0] = {
		{ 0.0f, 0.0f, 0.0f }, 1.0f, 0.0f, -1, 0, 0 };
	return bytes;
}

/** 已解析资产 + 已扫区间（选级器的两个输入；MeshletAsset 禁拷贝 ⟹ 移动持有）。 */
struct Parsed {
	MeshletAsset asset;
	MeshletRangeScan ranges;
};

/**
 * 合成产物 → parse → scanClusterRanges，两步都 **必须**成功：反向用例验的是「读取器与区间
 * 扫描都认得的合法产物仍违反选级侧不变量」，前置失败说明合成数据自己写错了，REQUIRE 直接红。
 */
Parsed parseRanges(const ProductSpec& spec) {
	auto parsed = parseMeshletAsset(buildProduct(spec));
	REQUIRE(parsed.has_value());
	auto ranges = scanClusterRanges(*parsed);
	REQUIRE(ranges.has_value());
	return Parsed { std::move(*parsed), std::move(*ranges) };
}

/** 全路径：合成产物 → parse → 区间扫描 → 选级扫描。 */
std::expected<MeshletLodScan, std::string> scanLod(
	const ProductSpec& spec, uint32_t levelD, MeshletLodFallback fallback) {
	Parsed p = parseRanges(spec);
	return scanMeshletLod(p.asset, p.ranges, levelD, fallback);
}

/** 逐字段比较（被测结构体不带 operator==——接口按任务书原样，比较器住测试侧）。 */
bool entryEq(const MeshletLodPrimRange& a, const MeshletLodPrimRange& b) {
	return a.start == b.start && a.count == b.count && a.level == b.level
		&& a.triangles == b.triangles;
}
bool rowEq(const MeshletLodCostRow& a, const MeshletLodCostRow& b) {
	return a.level == b.level && a.clusters == b.clusters && a.triangles == b.triangles;
}

// ─── 正向锚点（期望值手算，出处 = 下面这段推导，不取被测码）─────────────────────────
/*
 * 四条 prim / 七条 group / 十二条簇（levelCount = 4，产物最深级 = 2）：
 *   prim 0：簇 [0,6)：d0 {c0:3,c1:4} + d1 {c2:5,c3:5,c4:2} + d2 {c5:7}（g0/g1/g2）
 *   prim 1：簇 [6,7)：只有 d0 {c6:6}（g3）
 *   prim 2：簇 [7,12)：d0 {c7:1,c8:2,c9:3} + d1 {c10:4,c11:9}（g4/g5）
 *   prim 3：无簇（g6 = [12,0)）——区间扫描产出 {12,0,0}，选级输出 {0,0,0,0}
 * 区间扫描期望：perPrim = {0,6,2} {6,1,1} {7,5,3} {12,0,0}；Σ簇 12；ΣL0 6。
 *
 * 选级手算（deepest）：
 *   D=1：p0 有 d1 → {start 2, count 3, level 1, tri 5+5+2=12}
 *        p1 无 d1 → 回落 ≤1 最深 = d0 → {0,1,0,6}（fallback +1）
 *        p2 有 d1 → {3,2,1,4+9=13}；p3 无簇 → {0,0,0,0}
 *        画簇 3+1+2=6；画三角 12+6+13=31；L0 合计 6 簇 / 3+4+6+1+2+3=19 三角；
 *        maxLevelInProduct 2；maxLevelUsed 1；maxClustersPerDraw max(3,1,2)=3；
 *        histogram[4] = [1,2,0,0]（p1@0，p0/p2@1；无簇 p3 不计数）；
 *        costTable(D=0..2) = {0:6簇/19三角, 1:6/31, 2:(1+1+2)=4簇/(7+6+13)=26三角}
 *   D=5（超过产物最深级）：p0→d2 {5,1,2,7}、p1→d0 {0,1,0,6}、p2→d1 {3,2,1,13}；
 *        画簇 4 / 三角 26；fallback 3；maxLevelUsed 2；histogram [1,1,1,0]；
 *        maxClustersPerDraw 2；costTable 与 D=1 同（表只依赖 fallback 策略）。
 *   D=0：全走 d0：{0,2,0,7} {0,1,0,6} {0,3,0,6} {0,0,0,0}；画簇 6 = l0Clusters、
 *        画三角 19 = l0Triangles（恒 L0 档与 §10.1 (base,l0Count) 档同读数）；
 *        fallback 0；histogram [3,0,0,0]；maxClustersPerDraw 3。
 * 选级手算（l0，D=2）：p0 有 d2 用它 {5,1,2,7}；p1/p2 无 d2 → **级 0**：{0,1,0,6} {0,3,0,6}；
 *        画簇 5 / 三角 7+6+6=19；fallback 2；histogram [2,0,1,0]；
 *        costTable(l0) = {0:6/19, 1:(3+1+2)=6簇/31三角, 2:5/19}。
 */
const ProductSpec& anchor() {
	static const ProductSpec spec {
		4,
		4,
		{ GroupDesc { 0, 0, 0, 2 }, GroupDesc { 0, 1, 2, 3 }, GroupDesc { 0, 2, 5, 1 },
		    GroupDesc { 1, 0, 6, 1 }, GroupDesc { 2, 0, 7, 3 }, GroupDesc { 2, 1, 10, 2 },
		    GroupDesc { 3, 0, 12, 0 } },
		{ ClusterDesc { 0, -1, 3 }, ClusterDesc { 0, -1, 4 }, ClusterDesc { 1, 0, 5 },
		    ClusterDesc { 1, 0, 5 }, ClusterDesc { 1, 0, 2 }, ClusterDesc { 2, 1, 7 },
		    ClusterDesc { 3, -1, 6 }, ClusterDesc { 4, -1, 1 }, ClusterDesc { 4, -1, 2 },
		    ClusterDesc { 4, -1, 3 }, ClusterDesc { 5, 4, 4 }, ClusterDesc { 5, 4, 9 } },
	};
	return spec;
}

/*
 * gap 产物（levelCount = 3，prim 0 的级集 = {0, 2}，级 1 缺）：
 *   簇 [0,2) d0 {1,1} + [2,3) d2 {8}；c2.refined = g0（d0 < d2，读取侧 DAG 校验放行）。
 * 手算：D=3 deepest → 无 d3、≤3 最深 = d2 → {2,1,2,8}，fallback 1，画簇 1 / 三角 8；
 *       D=3 l0 → 无 d3 → 级 0 → {0,2,0,2}，fallback 1，画簇 2 / 三角 2；
 *       D=1 deepest → 无 d1、≤1 最深 = d0 → {0,2,0,2}（回落 ≠ 0 的形态由 anchor p0→d2 钉）。
 * 🔴 这条产物是 deepest 与 l0 **分家**的最小形态之一（anchor 上 D=2 两策略也分家，但 gap
 * 更极端：回落目标差两级）——回落策略参数被忽略（变异 ⑪）在这里红。
 */
const ProductSpec& gapProduct() {
	static const ProductSpec spec {
		1,
		3,
		{ GroupDesc { 0, 0, 0, 2 }, GroupDesc { 0, 2, 2, 1 } },
		{ ClusterDesc { 0, -1, 1 }, ClusterDesc { 0, -1, 1 }, ClusterDesc { 1, 0, 8 } },
	};
	return spec;
}

/**
 * base-shift 产物 = anchor 整体后移 2 簇，头部插一条 primX（d0 两簇 tri 1+1）。
 * 用途只有一个：同 prim 结构换全局 base ⟹ **start/level/triangles 逐字段不变、base 变**。
 * 「start 写成绝对簇下标」的变异（⑨）在 anchor 上对 prim 0 恰好同值（base=0），
 * 必须靠这条产物按 prim 逐个抓；perPrim 下标↔prim 的映射也在这里钉（错位一格即红）。
 */
const ProductSpec& shiftedProduct() {
	static const ProductSpec spec {
		5,
		4,
		{ GroupDesc { 0, 0, 0, 2 }, GroupDesc { 1, 0, 2, 2 }, GroupDesc { 1, 1, 4, 3 },
		    GroupDesc { 1, 2, 7, 1 }, GroupDesc { 2, 0, 8, 1 }, GroupDesc { 3, 0, 9, 3 },
		    GroupDesc { 3, 1, 12, 2 }, GroupDesc { 4, 0, 14, 0 } },
		{ ClusterDesc { 0, -1, 1 }, ClusterDesc { 0, -1, 1 }, ClusterDesc { 1, -1, 3 },
		    ClusterDesc { 1, -1, 4 }, ClusterDesc { 2, 1, 5 }, ClusterDesc { 2, 1, 5 },
		    ClusterDesc { 2, 1, 2 }, ClusterDesc { 3, 2, 7 }, ClusterDesc { 4, -1, 6 },
		    ClusterDesc { 5, -1, 1 }, ClusterDesc { 5, -1, 2 }, ClusterDesc { 5, -1, 3 },
		    ClusterDesc { 6, 5, 4 }, ClusterDesc { 6, 5, 9 } },
	};
	return spec;
}

// ─── 反向用例表（同一张表喂「逐条归因」与「两两不同」两条用例，杜绝两处漂移）──────────
struct BadCase {
	const char* name;               ///< 坏形态名（INFO 定位）
	ProductSpec (*build)();         ///< 合成产物规格
	uint32_t levelD;                ///< 请求级
	MeshletLodFallback fallback;    ///< 回落策略
	const char* needle;             ///< 期望错误串的唯一关键词
};

/** 不变量 2：级序回退（0,2,1）。读取侧放行（c1/c2 的 refined 都指 d0 < 各自级），区间扫描
 *  也放行（L0 前缀 = {c0}），只有选级侧的「单调不减」能抓。 */
ProductSpec badLevelOrder() {
	return { 1, 3,
		{ GroupDesc { 0, 0, 0, 1 }, GroupDesc { 0, 2, 1, 1 }, GroupDesc { 0, 1, 2, 1 } },
		{ ClusterDesc { 0, -1, 1 }, ClusterDesc { 1, 0, 1 }, ClusterDesc { 2, 0, 1 } } };
}
/** 不变量 3（depth0Len < l0Count 半边）：c1 在 d1 组却写 refined=-1 —— `refined==-1` 口径
 *  数出 l0Count=2，`depth==0` 口径只有 1 条。读取侧放行（refined==-1 无 DAG 约束），
 *  区间扫描放行（L0 恰为前缀），只有交叉对账能抓 = 本批最值钱的锚。 */
ProductSpec badL0Criteria() {
	return { 1, 2,
		{ GroupDesc { 0, 0, 0, 1 }, GroupDesc { 0, 1, 1, 2 } },
		{ ClusterDesc { 0, -1, 1 }, ClusterDesc { 1, -1, 1 }, ClusterDesc { 1, 0, 1 } } };
}
/** 不变量 4 / 5 共用产物：prim 1 只有 d1 簇（c1.refined = g0 指 prim 0 的 d0 组，读取侧
 *  只查「更浅」不查同 prim ⟹ 放行）。prim 1 的 l0Count=0：
 *  D=1 deepest → 选级非空但缺 L0 ⟹ 不变量 4；D=5 l0 → 无 d5 也无 d0 ⟹ 空选 ⟹ 不变量 5。 */
ProductSpec badNoL0Prim() {
	return { 2, 2,
		{ GroupDesc { 0, 0, 0, 1 }, GroupDesc { 1, 1, 1, 1 } },
		{ ClusterDesc { 0, -1, 1 }, ClusterDesc { 1, 0, 1 } } };
}

const std::vector<BadCase>& badCases() {
	static const std::vector<BadCase> table = {
		{ "级序回退（0,2,1）：级内连续破了", badLevelOrder, 1, MeshletLodFallback::deepest,
			"invariant 2" },
		{ "两条 L0 口径分家（depth0 段 1 ≠ l0Count 2）", badL0Criteria, 1,
			MeshletLodFallback::deepest, "invariant 3" },
		{ "prim 缺 L0（deepest 选级非空）", badNoL0Prim, 1, MeshletLodFallback::deepest,
			"invariant 4" },
		{ "prim 缺 L0（l0 回落交出空选）", badNoL0Prim, 5, MeshletLodFallback::l0,
			"invariant 5" },
	};
	return table;
}

// ─── Lcg 随机产物（写死种子；期望值 = 从 asset+ranges 独立重算的契约量，不抄被测码）────
/**
 * 造一张合法多级产物：prim 数 1..6、全局最深级 0..4；每 prim 每级要么跳过（造 gap）
 * 要么给 1..上一级大小 的簇数（**非增** = 真实 LOD DAG 的形态，deepest 成本单调性的前提）；
 * prim 有 1/8 概率整条无簇。triCount ∈ [1,128]（8bit 契约内）。
 * levelCount 由实际最深级推（杜绝「声明级数 > 簇数 + 1」触发被测前置的规格自相矛盾）。
 */
ProductSpec buildRandomProduct(testing::Lcg& rng) {
	ProductSpec spec;
	spec.primCount = static_cast<uint32_t>(rng.uniformInt(1, 6));
	const int maxDepth = rng.uniformInt(0, 4);
	uint32_t groupCursor = 0;
	uint32_t clusterCursor = 0;
	int deepestSeen = 0;
	for (uint32_t p = 0; p < spec.primCount; ++p) {
		if (rng.uniformInt(0, 7) == 0) {  // 1/8 无簇 prim（区间扫描产出 {cursor,0,0}）
			spec.groups.push_back(GroupDesc { p, 0, clusterCursor, 0 });
			++groupCursor;
			continue;
		}
		uint32_t prevSize = 0;
		uint32_t shallowerGroup = 0xFFFFFFFF;  // 本 prim 最近的**非空浅级** group（refined 目标）
		for (int depth = 0; depth <= maxDepth; ++depth) {
			uint32_t size = 0;
			if (depth == 0)
				size = static_cast<uint32_t>(rng.uniformInt(1, 4));
			else if (shallowerGroup != 0xFFFFFFFF && rng.uniformInt(0, 5) != 0)
				size = static_cast<uint32_t>(
					rng.uniformInt(1, static_cast<int>(prevSize)));
			if (size == 0)
				continue;  // gap：本级跳过（refined 仍指最近非空浅级，读取侧放行）
			spec.groups.push_back(GroupDesc { p, depth, clusterCursor, size });
			for (uint32_t k = 0; k < size; ++k) {
				const int32_t refined = depth == 0 ? -1 : static_cast<int32_t>(shallowerGroup);
				spec.clusters.push_back(ClusterDesc {
					groupCursor, refined, static_cast<uint32_t>(rng.uniformInt(1, 128)) });
				++clusterCursor;
			}
			++groupCursor;
			prevSize = size;
			shallowerGroup = groupCursor - 1;
			deepestSeen = std::max(deepestSeen, depth);
		}
	}
	spec.levelCount = static_cast<uint32_t>(deepestSeen + 1);
	return spec;
}

/** 从 asset 独立重算：prim 区间内 [startRel, startRel+count) 窗口的 Σ triCount（契约复述）。 */
uint64_t sumTrisFromAsset(
	const MeshletAsset& asset, const MeshletPrimRange& range, uint32_t startRel, uint32_t count) {
	uint64_t sum = 0;
	for (uint32_t k = 0; k < count; ++k)
		sum += asset.clusters[range.base + startRel + k].triCount;
	return sum;
}

/** 从 asset 独立重算：prim 区间内 depth == level 的簇是否存在（fallbackPrimCount 的判据）。 */
bool primHasLevel(const MeshletAsset& asset, const MeshletPrimRange& range, uint32_t level) {
	for (uint32_t k = 0; k < range.count; ++k)
		if (static_cast<uint32_t>(asset.groups[asset.clusters[range.base + k].group].depth)
			== level)
			return true;
	return false;
}

/** 从 asset 独立重算：全表最大级（maxLevelInProduct 的判据）。 */
uint32_t maxDepthFromAsset(const MeshletAsset& asset) {
	uint32_t best = 0;
	for (const MeshletDiskCluster& c : asset.clusters)
		best = std::max(best, static_cast<uint32_t>(asset.groups[c.group].depth));
	return best;
}

/** 从 asset 独立重算：`refined == -1` 口径的三角合计（l0Triangles 的**独立口径**交叉）。 */
uint64_t l0TrisByRefined(const MeshletAsset& asset) {
	uint64_t sum = 0;
	for (const MeshletDiskCluster& c : asset.clusters)
		if (c.refined == -1)
			sum += c.triCount;
	return sum;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 正向锚点（期望值 = 上方手算推导）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("正向锚点：D=1 deepest 的逐 prim 选级与全部聚合量等于手算常量") {
	const auto scan = scanLod(anchor(), 1, MeshletLodFallback::deepest);
	REQUIRE(scan.has_value());

	const MeshletLodPrimRange expected [] {
		{ 2u, 3u, 1u, 12u },  // p0 有 d1：start 2（L0 前缀 2 之后），tri 5+5+2
		{ 0u, 1u, 0u, 6u },   // p1 无 d1 → deepest ≤1 = d0（回落）
		{ 3u, 2u, 1u, 13u },  // p2 有 d1：start 3（L0 前缀 3 之后），tri 4+9
		{ 0u, 0u, 0u, 0u },   // p3 无簇 = 全 0（不是错误）
	};
	REQUIRE(scan->perPrim.size() == std::size(expected));
	for (size_t p = 0; p < std::size(expected); ++p) {
		INFO("prim ", p);
		CHECK(entryEq(scan->perPrim[p], expected[p]));
	}
	CHECK(scan->requestedLevel == 1u);
	CHECK(scan->fallback == MeshletLodFallback::deepest);
	CHECK(scan->drawnClusters == 6u);    // 3+1+2
	CHECK(scan->drawnTriangles == 31u);  // 12+6+13
	CHECK(scan->l0Clusters == 6u);       // 2+1+3
	CHECK(scan->l0Triangles == 19u);     // 3+4+6+1+2+3
	CHECK(scan->maxLevelInProduct == 2u);
	CHECK(scan->maxLevelUsed == 1u);
	CHECK(scan->fallbackPrimCount == 1u);  // 只有 p1 回落
	CHECK(scan->maxClustersPerDraw == 3u);
	const uint64_t expectedHist [] = { 1u, 2u, 0u, 0u };  // p1@0；p0/p2@1；无簇 p3 不计数
	REQUIRE(scan->levelHistogram.size() == std::size(expectedHist));  // == header.levelCount
	for (size_t d = 0; d < std::size(expectedHist); ++d)
		CHECK(scan->levelHistogram[d] == expectedHist[d]);
	// costTable 逐行手算（§18.2 表的机器版）：D=2 行 = p0 d2(1,7) + p1 d0(1,6) + p2 d1(2,13)
	const MeshletLodCostRow expectedCost [] = {
		{ 0u, 6u, 19u }, { 1u, 6u, 31u }, { 2u, 4u, 26u }
	};
	REQUIRE(scan->costTable.size() == std::size(expectedCost));
	for (size_t d = 0; d < std::size(expectedCost); ++d) {
		INFO("costTable 行 ", d);
		CHECK(rowEq(scan->costTable[d], expectedCost[d]));
	}
	// 自洽契约：costTable[requestedLevel] == 主读数（批 B 打日志两处取数必须同源）
	CHECK(scan->costTable[scan->requestedLevel].clusters == scan->drawnClusters);
	CHECK(scan->costTable[scan->requestedLevel].triangles == scan->drawnTriangles);
}

TEST_CASE("正向：D 超过产物最深级（D=5）逐 prim 回落到各自最深级，costTable 与策略同源") {
	const auto scan = scanLod(anchor(), 5, MeshletLodFallback::deepest);
	REQUIRE(scan.has_value());
	const MeshletLodPrimRange expected [] {
		{ 5u, 1u, 2u, 7u },   // p0 最深 = d2（start 5 = 2+3）
		{ 0u, 1u, 0u, 6u },   // p1 最深 = d0
		{ 3u, 2u, 1u, 13u },  // p2 最深 = d1
		{ 0u, 0u, 0u, 0u },
	};
	REQUIRE(scan->perPrim.size() == std::size(expected));
	for (size_t p = 0; p < std::size(expected); ++p) {
		INFO("prim ", p);
		CHECK(entryEq(scan->perPrim[p], expected[p]));
	}
	CHECK(scan->drawnClusters == 4u);
	CHECK(scan->drawnTriangles == 26u);
	CHECK(scan->fallbackPrimCount == 3u);  // 三条有簇 prim 全部回落（§18.2：回落级才是成本主导项）
	CHECK(scan->maxLevelUsed == 2u);
	CHECK(scan->maxClustersPerDraw == 2u);
	const uint64_t expectedHist [] = { 1u, 1u, 1u, 0u };
	REQUIRE(scan->levelHistogram.size() == std::size(expectedHist));
	for (size_t d = 0; d < std::size(expectedHist); ++d)
		CHECK(scan->levelHistogram[d] == expectedHist[d]);
	// costTable 只依赖 fallback 策略、不依赖请求 D ⟹ 与 D=1 那次逐行同值
	REQUIRE(scan->costTable.size() == 3u);
	CHECK(rowEq(scan->costTable[2], MeshletLodCostRow { 2u, 4u, 26u }));
}

TEST_CASE("正向：D=0（恒 L0 档）读数 == L0 基线合计（与 §10.1 (base,l0Count) 档同读数）") {
	const auto scan = scanLod(anchor(), 0, MeshletLodFallback::deepest);
	REQUIRE(scan.has_value());
	const MeshletLodPrimRange expected [] {
		{ 0u, 2u, 0u, 7u },  // p0 d0：tri 3+4
		{ 0u, 1u, 0u, 6u },
		{ 0u, 3u, 0u, 6u },  // p2 d0：tri 1+2+3
		{ 0u, 0u, 0u, 0u },
	};
	REQUIRE(scan->perPrim.size() == std::size(expected));
	for (size_t p = 0; p < std::size(expected); ++p) {
		INFO("prim ", p);
		CHECK(entryEq(scan->perPrim[p], expected[p]));  // 全 0 条目同时钉「无簇 prim 不是错误」
	}
	CHECK(scan->drawnClusters == scan->l0Clusters);  // 恒 L0 档的定义
	CHECK(scan->drawnTriangles == scan->l0Triangles);
	CHECK(scan->drawnClusters == 6u);
	CHECK(scan->drawnTriangles == 19u);
	CHECK(scan->fallbackPrimCount == 0u);  // D=0 恒存在（不变量 4 的定义域）
	CHECK(scan->maxLevelUsed == 0u);
	const uint64_t expectedHist [] = { 3u, 0u, 0u, 0u };
	REQUIRE(scan->levelHistogram.size() == std::size(expectedHist));
	for (size_t d = 0; d < std::size(expectedHist); ++d)
		CHECK(scan->levelHistogram[d] == expectedHist[d]);
}

TEST_CASE("正向：l0 回落策略（anchor D=2 手算 + gap 产物上 deepest 与 l0 **分家**）") {
	// anchor D=2 l0：p0 有 d2 用它；p1/p2 无 d2 回落**级 0**（不是各自最深级）
	const auto l0 = scanLod(anchor(), 2, MeshletLodFallback::l0);
	REQUIRE(l0.has_value());
	const MeshletLodPrimRange expected [] {
		{ 5u, 1u, 2u, 7u },  // p0 d2 存在 → 用它（回落策略只在缺级时生效）
		{ 0u, 1u, 0u, 6u },
		{ 0u, 3u, 0u, 6u },  // p2 无 d2 → 级 0（deepest 会选 d1={3,2,1,13}，两策略在此分家）
		{ 0u, 0u, 0u, 0u },
	};
	REQUIRE(l0->perPrim.size() == std::size(expected));
	for (size_t p = 0; p < std::size(expected); ++p) {
		INFO("prim ", p);
		CHECK(entryEq(l0->perPrim[p], expected[p]));
	}
	CHECK(l0->drawnClusters == 5u);
	CHECK(l0->drawnTriangles == 19u);
	CHECK(l0->fallbackPrimCount == 2u);
	const uint64_t expectedHist [] = { 2u, 0u, 1u, 0u };
	REQUIRE(l0->levelHistogram.size() == std::size(expectedHist));
	for (size_t d = 0; d < std::size(expectedHist); ++d)
		CHECK(l0->levelHistogram[d] == expectedHist[d]);
	const MeshletLodCostRow expectedCost [] = {
		{ 0u, 6u, 19u }, { 1u, 6u, 31u }, { 2u, 5u, 19u }
	};
	REQUIRE(l0->costTable.size() == std::size(expectedCost));
	for (size_t d = 0; d < std::size(expectedCost); ++d) {
		INFO("costTable 行 ", d);
		CHECK(rowEq(l0->costTable[d], expectedCost[d]));
	}

	// gap 产物（级集 {0,2}）：D=3 时 deepest 取 d2、l0 取 d0 —— 同输入两策略不同读数
	const auto deep = scanLod(gapProduct(), 3, MeshletLodFallback::deepest);
	REQUIRE(deep.has_value());
	CHECK(entryEq(deep->perPrim[0], MeshletLodPrimRange { 2u, 1u, 2u, 8u }));
	CHECK(deep->drawnClusters == 1u);
	CHECK(deep->drawnTriangles == 8u);
	CHECK(deep->fallbackPrimCount == 1u);
	CHECK(deep->maxLevelUsed == 2u);
	const auto toL0 = scanLod(gapProduct(), 3, MeshletLodFallback::l0);
	REQUIRE(toL0.has_value());
	CHECK(entryEq(toL0->perPrim[0], MeshletLodPrimRange { 0u, 2u, 0u, 2u }));
	CHECK(toL0->drawnClusters == 2u);
	CHECK(toL0->drawnTriangles == 2u);
	CHECK(toL0->fallbackPrimCount == 1u);
	// gap 产物 D=1：deepest 回落 ≤1 最深 = d0（「回落 ≠ 0」由 anchor p0→d2 钉，这里钉不越级）
	const auto d1 = scanLod(gapProduct(), 1, MeshletLodFallback::deepest);
	REQUIRE(d1.has_value());
	CHECK(entryEq(d1->perPrim[0], MeshletLodPrimRange { 0u, 2u, 0u, 2u }));
}

TEST_CASE("🔴 序/映射锚点：同 prim 换 base ⟹ start/level/triangles 不变、base 变、下标↔prim 对齐") {
	// shifted = anchor 整体后移 2 簇 + 头部插一条 primX ⟹ 原 prim p 的选级读数必须**逐字段
	// 原样**出现在 perPrim[p+1]，而区间 base 全部 +2。「start 写成绝对簇下标」在 anchor 上对
	// prim 0 恰好同值（base=0）蒙混过关，只有这条产物按 prim 逐个抓得住。
	Parsed base = parseRanges(anchor());
	Parsed shift = parseRanges(shiftedProduct());
	REQUIRE(shift.ranges.perPrim.size() == base.ranges.perPrim.size() + 1u);
	for (size_t p = 0; p < base.ranges.perPrim.size(); ++p) {
		INFO("prim ", p);
		CHECK(shift.ranges.perPrim[p + 1].base == base.ranges.perPrim[p].base + 2u);
		CHECK(shift.ranges.perPrim[p + 1].count == base.ranges.perPrim[p].count);
		CHECK(shift.ranges.perPrim[p + 1].l0Count == base.ranges.perPrim[p].l0Count);
	}

	for (const uint32_t d : { 0u, 1u, 5u }) {
		const auto a = scanMeshletLod(base.asset, base.ranges, d, MeshletLodFallback::deepest);
		const auto s = scanMeshletLod(shift.asset, shift.ranges, d, MeshletLodFallback::deepest);
		REQUIRE(a.has_value());
		REQUIRE(s.has_value());
		REQUIRE(s->perPrim.size() == a->perPrim.size() + 1u);
		for (size_t p = 0; p < a->perPrim.size(); ++p) {
			INFO("D = ", d, " prim ", p);
			CHECK(entryEq(s->perPrim[p + 1], a->perPrim[p]));  // start 相对 ⟹ 换 base 不动
		}
		// 头部 primX（d0 两簇 tri 1+1）：D=0 用它；D>0 只有 d0 可回落
		CHECK(entryEq(s->perPrim[0],
			MeshletLodPrimRange { 0u, 2u, 0u, 2u }));  // 两形态同读数（level 0）
		CHECK(s->drawnClusters == a->drawnClusters + 2u);
		CHECK(s->drawnTriangles == a->drawnTriangles + 2u);
		CHECK(s->fallbackPrimCount == a->fallbackPrimCount + (d == 0u ? 0u : 1u));
	}
}

TEST_CASE("maxClustersPerDraw 不乘 task 槽数（== max 单 prim 选中簇数；2× 住 meshletTaskCount）") {
	// §18.6 漂移红线：task 数唯一真相源 = render_list_impl.cppm::meshletTaskCount。
	// 本字段若在这里乘了 kSlotsPerCluster(=2)，anchor D=1 的读数会从 3 变 6、D=5 从 2 变 4。
	const auto d1 = scanLod(anchor(), 1, MeshletLodFallback::deepest);
	REQUIRE(d1.has_value());
	CHECK(d1->maxClustersPerDraw == 3u);  // max(3,1,2)，不是 6
	const auto d5 = scanLod(anchor(), 5, MeshletLodFallback::deepest);
	REQUIRE(d5.has_value());
	CHECK(d5->maxClustersPerDraw == 2u);  // max(1,1,2)，不是 4
	// 结构性判据：== max perPrim.count（与逐 prim 条目同源，不是第二份算式）
	uint32_t maxCount = 0;
	for (const MeshletLodPrimRange& e : d1->perPrim)
		maxCount = std::max(maxCount, e.count);
	CHECK(d1->maxClustersPerDraw == maxCount);
}

TEST_CASE("边界：空产物（0 prim / 0 簇 / levelCount 0）→ 成功、costTable 单行全 0") {
	const auto scan = scanLod(ProductSpec { 0, 0, {}, {} }, 3, MeshletLodFallback::deepest);
	REQUIRE(scan.has_value());
	CHECK(scan->perPrim.empty());
	CHECK(scan->drawnClusters == 0u);
	CHECK(scan->drawnTriangles == 0u);
	CHECK(scan->l0Clusters == 0u);
	CHECK(scan->l0Triangles == 0u);
	CHECK(scan->maxLevelInProduct == 0u);
	CHECK(scan->maxLevelUsed == 0u);
	CHECK(scan->levelHistogram.empty());
	REQUIRE(scan->costTable.size() == 1u);
	CHECK(rowEq(scan->costTable[0], MeshletLodCostRow { 0u, 0u, 0u }));
}

// ─────────────────────────────────────────────────────────────────────────────
// 反向：五条不变量各一条（关键词逐条钉死）+ 两两不同 + 前置守卫
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("反向：全部坏形态各自报出各自的不变量（关键词逐条钉死）") {
	for (const auto& bad : badCases()) {
		const auto scan = scanLod(bad.build(), bad.levelD, bad.fallback);
		INFO("坏形态：", bad.name);
		REQUIRE_FALSE(scan.has_value());
		CHECK(scan.error().find(bad.needle) != std::string::npos);
	}
}

/** 篡改 header 的视图大小（clean 那份出配对 ranges，tampered 那份当输入）→ 不变量 1 错误串。 */
std::string inv1Error(uint32_t bumpGroups, uint32_t bumpClusters) {
	auto clean = parseMeshletAsset(buildProduct(anchor()));
	REQUIRE(clean.has_value());
	auto ranges = scanClusterRanges(*clean);
	REQUIRE(ranges.has_value());
	auto tampered = parseMeshletAsset(buildProduct(anchor()));
	REQUIRE(tampered.has_value());
	tampered->header.groupCount += bumpGroups;
	tampered->header.clusterCount += bumpClusters;
	const auto err = scanMeshletLod(*tampered, *ranges, 1, MeshletLodFallback::deepest);
	REQUIRE_FALSE(err.has_value());
	return err.error();
}

TEST_CASE("反向：五条不变量的错误串两两不同（可归因判据本身也要有牙）") {
	std::vector<std::string> errors;
	for (const auto& bad : badCases()) {
		const auto scan = scanLod(bad.build(), bad.levelD, bad.fallback);
		INFO("坏形态：", bad.name);
		REQUIRE_FALSE(scan.has_value());
		errors.push_back(scan.error());
	}
	// 不变量 1 的两条（groups/clusters 视图大小）：同一前缀、不同消息，也必须两两不同
	errors.push_back(inv1Error(1, 0));
	errors.push_back(inv1Error(0, 1));
	REQUIRE(errors.size() == 6u);
	for (size_t i = 0; i < errors.size(); ++i)
		for (size_t j = i + 1; j < errors.size(); ++j)
			CHECK_FALSE(errors[i] == errors[j]);
	for (const char* prefix :
		{ "invariant 1", "invariant 2", "invariant 3", "invariant 4", "invariant 5" }) {
		INFO("缺少不变量：", prefix);
		CHECK(std::ranges::any_of(errors,
			[&](const std::string& e) { return e.find(prefix) != std::string::npos; }));
	}
}

TEST_CASE("反向：不变量 3 的**另一半**（depth0 段 > l0Count）经 parse 不可达，手搭 ranges 钉住") {
	// 读取侧 DAG 校验（refined 必须指向更浅 group）挡住了「级 0 簇带 refined」的字节流形态
	// ⟹ 这条半边构造不出 parse 过的产物。ranges 本就是选级器的合法第二输入（装配层之外没有
	// 别人喂它），手搭 = 唯一激励。它与反向表 inv3（1 < 2 半边）合起来把 `!=` 弱化成
	// `>` / `<` 的两个变异各按死一条：本用例（2 > 0）专杀 `!=`→`<`。
	Parsed p = parseRanges(anchor());
	MeshletRangeScan tampered = p.ranges;
	tampered.perPrim[0].l0Count = 0;  // 谎称 prim 0 无 L0（产物实际有 2 条级 0 簇）
	const auto scan = scanMeshletLod(p.asset, tampered, 1, MeshletLodFallback::deepest);
	REQUIRE_FALSE(scan.has_value());
	CHECK(scan.error().find("invariant 3") != std::string::npos);
	CHECK(scan.error().find("two independent L0 criteria diverged") != std::string::npos);
}

TEST_CASE("前置守卫：header 篡改（视图大小 / levelCount 越界 / histogram 分配界）逐条点名") {
	// MeshletAsset 禁拷贝 ⟹ 各 parse 一份。篡改 header 后**不**过 scanClusterRanges
	// （区间扫描会先按篡改值自己报错），ranges 用未篡改那份配对——模拟「调用方喂进不自洽
	// 视图」的形态，钉的是选级器自己的不变量 1 与两条前置。
	CHECK(inv1Error(1, 0).find("groups view holds") != std::string::npos);
	CHECK(inv1Error(0, 1).find("clusters view holds") != std::string::npos);
	{
		// levelCount 收到 1：产物实际最深级 2 ⟹ depth 前置点名（histogram 越界写的前哨）
		auto tampered = parseMeshletAsset(buildProduct(anchor()));
		REQUIRE(tampered.has_value());
		tampered->header.levelCount = 1;
		auto clean = parseMeshletAsset(buildProduct(anchor()));
		REQUIRE(clean.has_value());
		auto ranges = scanClusterRanges(*clean);
		REQUIRE(ranges.has_value());
		const auto err = scanMeshletLod(*tampered, *ranges, 1, MeshletLodFallback::deepest);
		REQUIRE_FALSE(err.has_value());
		CHECK(err.error().find("outside [0, levelCount") != std::string::npos);
	}
	{
		// levelCount 顶到簇数 + 2：histogram 分配面被 header 裸字段牵着走 ⟹ 拒
		auto tampered = parseMeshletAsset(buildProduct(anchor()));
		REQUIRE(tampered.has_value());
		tampered->header.levelCount = 14;  // 簇数 12 ⟹ 界 = 13
		auto clean = parseMeshletAsset(buildProduct(anchor()));
		REQUIRE(clean.has_value());
		auto ranges = scanClusterRanges(*clean);
		REQUIRE(ranges.has_value());
		const auto err = scanMeshletLod(*tampered, *ranges, 1, MeshletLodFallback::deepest);
		REQUIRE_FALSE(err.has_value());
		CHECK(err.error().find("histogram allocation") != std::string::npos);
	}
}

TEST_CASE("前置守卫：手搭 ranges 区间越界 ⟹ 点名「beyond the cluster table」（不静默不越界读）") {
	// ranges 与 asset 不同源（这里 = 同资产但 count 被写坏）是批 B 装配层最可能犯的错。
	Parsed p = parseRanges(anchor());
	MeshletRangeScan tampered = p.ranges;
	tampered.perPrim[0].count = 99;  // 簇表只有 12 条
	const auto scan = scanMeshletLod(p.asset, tampered, 1, MeshletLodFallback::deepest);
	REQUIRE_FALSE(scan.has_value());
	CHECK(scan.error().find("beyond the cluster table") != std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Lcg 属性组（写死种子；期望值 = 从 asset+ranges 独立重算的契约量）
// ─────────────────────────────────────────────────────────────────────────────

TEST_CASE("Lcg 属性组（种子 20261012）：40 张随机产物 × D 0..6 × 两策略的契约全量钉") {
	testing::Lcg rng(20261012);
	int products = 0;
	for (int iter = 0; iter < 40; ++iter) {
		const ProductSpec spec = buildRandomProduct(rng);
		Parsed p = parseRanges(spec);
		REQUIRE(p.ranges.perPrim.size() == spec.primCount);
		REQUIRE_FALSE(p.ranges.perPrim.empty());  // 空集合 ⟹ 下面的遍历恒真是假绿
		++products;

		uint64_t primsWithClusters = 0;
		for (const MeshletPrimRange& r : p.ranges.perPrim)
			if (r.count > 0u)
				++primsWithClusters;

		for (const uint32_t d : { 0u, 1u, 2u, 3u, 4u, 5u, 6u }) {
			for (const MeshletLodFallback fb :
				{ MeshletLodFallback::deepest, MeshletLodFallback::l0 }) {
				const auto scan = scanMeshletLod(p.asset, p.ranges, d, fb);
				REQUIRE(scan.has_value());
				INFO("iter ", iter, " D ", d, " fallback ",
					fb == MeshletLodFallback::deepest ? "deepest" : "l0");
				REQUIRE(scan->perPrim.size() == p.ranges.perPrim.size());

				uint64_t sumDrawn = 0, expectFallback = 0, expectMaxDraw = 0;
				for (size_t prim = 0; prim < p.ranges.perPrim.size(); ++prim) {
					const MeshletPrimRange& range = p.ranges.perPrim[prim];
					const MeshletLodPrimRange& sel = scan->perPrim[prim];
					// ① 空/非空与区间扫描同真同假；start 相对性上界
					CHECK((sel.count > 0u) == (range.count > 0u));
					CHECK(sel.start + sel.count <= range.count);
					if (range.count == 0u)
						continue;
					// ② 选中窗口独立重算：窗口首簇的 group.depth == 报告 level；窗口三角 ==
					//    asset 上同窗口 Σ triCount；窗口是该级的**极大段**（前后邻簇不同级）
					//    ——「start/count 错位」在这里红
					const uint32_t firstDepth = static_cast<uint32_t>(
						p.asset.groups[p.asset.clusters[range.base + sel.start].group].depth);
					CHECK(firstDepth == sel.level);
					CHECK(sel.triangles == sumTrisFromAsset(p.asset, range, sel.start, sel.count));
					if (sel.start > 0u)
						CHECK(static_cast<uint32_t>(p.asset.groups[p.asset.clusters
							[range.base + sel.start - 1].group].depth) != sel.level);
					if (sel.start + sel.count < range.count)
						CHECK(static_cast<uint32_t>(p.asset.groups[p.asset.clusters
							[range.base + sel.start + sel.count].group].depth) != sel.level);
					// ③ 回落语义：deepest 恒 level ≤ D；l0 恒 level ∈ {D, 0}
					//（doctest 判据面只放二元比较，|| 先落 bool 变量）
					const bool levelOk = fb == MeshletLodFallback::deepest
						? sel.level <= d
						: (sel.level == d || sel.level == 0u);
					CHECK(levelOk);
					// ④ 回落计数独立重算：prim 区间内**不存在**级 D ⟺ 该 prim 回落
					const bool hasLevel = primHasLevel(p.asset, range, d);
					if (hasLevel)
						CHECK(sel.level == d);
					else if (fb == MeshletLodFallback::deepest)
						CHECK(sel.level < d);
					else
						CHECK(sel.level == 0u);
					sumDrawn += sel.count;
					expectFallback += hasLevel ? 0u : 1u;
					expectMaxDraw = std::max<uint64_t>(expectMaxDraw, sel.count);
				}
				// ⑤ 聚合量与逐 prim 独立和一致；L0 合计与**区间扫描口径**交叉（不变量 3 的
				//    聚合影子：refined==-1 合计 == depth==0 合计），三角再与 refined 口径交叉
				CHECK(scan->drawnClusters == sumDrawn);
				CHECK(scan->fallbackPrimCount == expectFallback);
				CHECK(scan->maxClustersPerDraw == expectMaxDraw);  // 🔴 无 2×
				CHECK(scan->l0Clusters == p.ranges.totalL0Clusters);
				CHECK(scan->l0Triangles == l0TrisByRefined(p.asset));
				CHECK(scan->maxLevelInProduct == maxDepthFromAsset(p.asset));
				// ⑥ histogram：size == levelCount、和 == 有簇 prim 数
				REQUIRE(scan->levelHistogram.size() == spec.levelCount);
				uint64_t histSum = 0;
				for (const uint64_t n : scan->levelHistogram)
					histSum += n;
				CHECK(histSum == primsWithClusters);
				// ⑦ costTable 自洽 + 行数 = 最深级 + 1；deepest 策略下全表簇数随 D 非增。
				//    自洽只在 D ≤ maxLevelInProduct 时有行可对（表覆盖 D = 0..最深级）。
				REQUIRE(scan->costTable.size() == scan->maxLevelInProduct + 1u);
				if (d <= scan->maxLevelInProduct) {
					CHECK(scan->costTable[d].clusters == scan->drawnClusters);
					CHECK(scan->costTable[d].triangles == scan->drawnTriangles);
				}
				if (fb == MeshletLodFallback::deepest)
					for (size_t row = 1; row < scan->costTable.size(); ++row)
						CHECK(scan->costTable[row].clusters
							<= scan->costTable[row - 1].clusters);
			}
		}
	}
	CHECK(products == 40);  // 前提守卫：一条都没造出来 = 上面全是空遍历假绿
}

}  // namespace RGL
