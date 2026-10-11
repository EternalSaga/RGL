module;
#include <doctest/doctest.h>
#include <filesystem>  // loadMeshletAsset 磁盘往返的临时文件（GMF 例外清单成员）

/**
 * test_meshlet_asset —— meshlet_pack 产物**读取侧**的健壮性契约（★0-b1）
 * 被测对象 = `rgl/meshlet_asset.cppm` + `rgl/meshlet_asset_impl.cppm`（rlbase，纯 CPU 无设备）。
 *
 * ## 本文件验什么、不验什么（🔴 裁决 2 的口径，读之前先立住）
 * 验的是「**读取器对字节流的健壮性**」：最小合法合成缓冲必须过校验且各段视图正确；
 * 每一种坏字节形态必须被拒、且错误串两两不同（可归因）。
 * **不验两侧格式一致**——合成缓冲的布局抄自被测模块的镜像结构（而镜像的真相源是
 * `tools/meshlet_pack/format.h`，本批红线是引擎不 include tools/），两侧同时漂则本文件
 * 照样绿。「引擎读得懂真产物」那条证据只能由 ★0-b2 拿 meshlet_pack 真产物做端到端冒烟给出。
 * 规格常量（192/16/44/36/32/48、段序、16B 对齐）按 format.h 头注释**手抄**进本文件，
 * 与被测镜像的 static_assert 互为对账——镜像漂了，正向用例的 sizeof 断言先红。
 *
 * ## 阳性锚点
 * 正向用例（合法缓冲解析成功 + 首元素/stride/count 逐段断言）与全部反向用例配对：
 * 「校验整段失效」时反向用例红，「校验误杀一切」时正向用例红——两个方向都不可恒绿。
 * 每条变异都真的改变了缓冲（apply 若写成 no-op，`REQUIRE_FALSE(解析成功)` 立刻红）。
 *
 * ## 变异红线（**全部实测**于 2026-10-12，改 `meshlet_asset_impl.cppm` → `just test` 变红 → cp 还原）
 *  - M1 **逐簇校验循环整段失效**（`checkClusterAt(a, i)` 逗号表达式空转，读取器整段失效形态）
 *      → 红 6 条（tri 越界 / remap 范围越界 / group 越界 / refined 越界 / refined 同层与更深 /
 *        两两不同）
 *  - M2 magic 校验删掉（比较 `&& false`）      → 红 2 条（magic 用例 + 两两不同）
 *  - M3 `remap[i] >= vertexCount` 判据改 `>`   → 红 2 条（remap 边界用例——探针值取 == vertexCount
 *      正是为这条：999 这类远越界值抓不到等号漏检）
 *  - M4 Σ Tris 对账删掉                        → 红 2 条（Tris 段字节数不符 + 两两不同）
 *  - M5 段间重叠判据删掉                       → 红 2 条（段偏移重叠 + 两两不同）
 *  - M6 Σ 覆盖对账删掉                         → 红 2 条（group 覆盖不符 + 两两不同）
 *  - M7 formatVersion 兼容范围恒通过           → 红 2 条（formatVersion 用例 + 两两不同）
 * 「两两不同」用例在每条变异下同时红 = 多条校验被摘掉时错误串会塌缩到同一条模板，可归因
 * 判据本身也有牙。
 *
 * ## 输入规模纪律
 * 合成缓冲 = 1 prim / 6 顶点 / 2 簇 / 2 级 DAG / 2 节点 ≈ 1KB，毫秒级；🔴 **不**把 184MB
 * 的 bistro 产物当测试输入（测试不依赖构建产物）。
 */
export module test_meshlet_asset;

import rlstd;
import meshlet_asset;

namespace RGL {
namespace {

// ─── 规格常量（按 format.h 头注释手抄 = 期望值的独立出处，不取被测模块常量）─────────
constexpr size_t kHdrBytes = 192, kPrimBytes = 16, kClusterBytes = 44, kGroupBytes = 36,
                 kNodeBytes = 32, kVertexBytes = 48;

/** 写侧段对齐（format.h `kSectionAlign` 口径的手抄）。 */
uint64_t alignUp16(uint64_t v) { return (v + 15) / 16 * 16; }

/** 合成产物 = 字节缓冲 + 各段偏移（反向用例按偏移定位注入坏字节）。 */
struct BuiltAsset {
	std::vector<std::byte> bytes;
	uint64_t offPrim = 0, offVertex = 0, offCluster = 0, offTris = 0, offRemap = 0, offGroup = 0,
	         offNode = 0;
};

/**
 * 最小合法产物：1 prim / 6 顶点 / 2 簇（c0 = L0 refined=-1；c1 = depth1 refined=0）/
 * 2 group（depth 0/1）/ 2 节点。段布局按 format.h：头 → Prim → Vertex → Clusters → Tris
 * → Remap → Groups → Nodes，段间 16B 对齐 0x00 填充（与写侧 serialize.cpp 同法）。
 */
BuiltAsset makeValid() {
	BuiltAsset b;
	uint64_t off = kHdrBytes;
	b.offPrim = off;
	off = alignUp16(off + 1 * kPrimBytes);
	b.offVertex = off;
	off = alignUp16(off + 6 * kVertexBytes);
	b.offCluster = off;
	off = alignUp16(off + 2 * kClusterBytes);
	b.offTris = off;
	off = alignUp16(off + 6);  // 2 簇 × 1 三角 × 3 u8
	b.offRemap = off;
	off = alignUp16(off + 6 * 4);  // 2 簇 × 3 顶点 × u32
	b.offGroup = off;
	off = alignUp16(off + 2 * kGroupBytes);
	b.offNode = off;
	off = alignUp16(off + 2 * kNodeBytes);
	b.bytes.assign(size_t(off), std::byte{0});
	std::byte* base = b.bytes.data();

	auto* h = reinterpret_cast<MeshletDiskHeader*>(base);
	std::memcpy(h->magic, kMeshletMagic, 8);
	h->formatVersion = 1;
	h->toolVersion = 1;
	for (uint32_t i = 0; i < 20; ++i)
		h->sourceSha1[i] = uint8_t(i);  // 0x00..0x13 → hex "000102…13"
	h->configFingerprint = 0x12345678;
	h->primCount = 1;
	h->vertexCount = 6;
	h->clusterCount = 2;
	h->groupCount = 2;
	h->nodeCount = 2;
	h->levelCount = 2;
	h->nodeWidth = 8;
	h->l0TriangleCount = 1;
	h->totalTriangleCount = 2;
	h->offPrimTable = b.offPrim;
	h->offVertexPool = b.offVertex;
	h->offClusters = b.offCluster;
	h->offTris = b.offTris;
	h->offRemap = b.offRemap;
	h->offGroups = b.offGroup;
	h->offNodes = b.offNode;

	reinterpret_cast<MeshletDiskPrim*>(base + b.offPrim)[0] = {0, 6, 3, 7};
	// 顶点 i 首 4 字节 = 0xC0000000+i：逐顶点可辨识（stride/段偏移错 → 读 tag 即红）。
	for (uint32_t v = 0; v < 6; ++v) {
		const uint32_t tag = 0xC0000000u + v;
		std::memcpy(base + b.offVertex + v * kVertexBytes, &tag, 4);
	}
	auto* clusters = reinterpret_cast<MeshletDiskCluster*>(base + b.offCluster);
	clusters[0] = {0, -1, 1, 3, 0, 0, {1.0f, 2.0f, 3.0f}, 4.0f, 0.0f};
	clusters[1] = {1, 0, 1, 3, 3, 3, {5.0f, 6.0f, 7.0f}, 8.0f, 0.5f};
	const uint8_t tris[6] = {0, 1, 2, 0, 1, 2};
	std::memcpy(base + b.offTris, tris, sizeof(tris));
	const uint32_t remap[6] = {0, 1, 2, 3, 4, 5};
	std::memcpy(base + b.offRemap, remap, sizeof(remap));
	auto* groups = reinterpret_cast<MeshletDiskGroup*>(base + b.offGroup);
	groups[0] = {0, 0, {0.0f, 0.0f, 0.0f}, 1.0f, 0.0f, 0, 1};
	groups[1] = {1, 0, {0.0f, 1.0f, 0.0f}, 2.0f, 0.5f, 1, 1};
	auto* nodes = reinterpret_cast<MeshletDiskNode*>(base + b.offNode);
	nodes[0] = {{0.0f, 0.0f, 0.0f}, 10.0f, 0.5f, -1, 0, 0};
	nodes[1] = {{0.0f, 0.0f, 1.0f}, 5.0f, 0.0f, 1, 0, 0};
	return b;
}

/** 定位缓冲里的文件头（反向用例改字段用；vector 缓冲 16B 对齐，reinterpret 合法）。 */
MeshletDiskHeader& headerOf(BuiltAsset& b) {
	return *reinterpret_cast<MeshletDiskHeader*>(b.bytes.data());
}
uint32_t& remapSlot(BuiltAsset& b, size_t i) {
	return reinterpret_cast<uint32_t*>(b.bytes.data() + b.offRemap)[i];
}

// ─── 变异表（反向用例的唯一真相源：单条用例与「两两不同」用例共用，杜绝两处漂移）────
struct Mutation {
	const char* name;             ///< 变异名（INFO 定位）
	void (*apply)(BuiltAsset&);   ///< 注入坏字节
	const char* needle;           ///< 期望错误串的唯一关键词
};

void mutMagic(BuiltAsset& b) { headerOf(b).magic[3] = 'X'; }
void mutFormatVersion(BuiltAsset& b) { headerOf(b).formatVersion = 2; }
void mutToolVersion(BuiltAsset& b) { headerOf(b).toolVersion = 99; }
void mutTruncateTail(BuiltAsset& b) { b.bytes.resize(b.bytes.size() - 8); }
void mutTruncateHeader(BuiltAsset& b) { b.bytes.resize(100); }
void mutFirstOffsetInHeader(BuiltAsset& b) { headerOf(b).offPrimTable = 64; }
void mutOverlap(BuiltAsset& b) { headerOf(b).offClusters = b.offVertex; }
void mutMisalignRemap(BuiltAsset& b) { headerOf(b).offRemap -= 2; }
void mutOffsetWrap(BuiltAsset& b) {
	// 4 对齐的大偏移（UINT64_MAX−11 ≡ 0 mod 4）+ 64B 段 ⟹ 先过对齐、再撞绕回防护。
	headerOf(b).offNodes = std::numeric_limits<uint64_t>::max() - 11;
}
void mutTrisRemapInverted(BuiltAsset& b) {
	// Remap 段头落回 Tris 段头之前 ⟹ 变长段「偏移差 = 段长」的推导会绕回，先拦。
	headerOf(b).offRemap = b.offTris - 16;
}
void mutOffsetBeyondFile(BuiltAsset& b) {
	// 偏移本身越出文件尾（与「截断尾部」的末段尾越界是两条不同分支）。
	headerOf(b).offNodes = b.bytes.size() + 16;
}
void mutTrisBytesMismatch(BuiltAsset& b) {
	// Tris 段拉长 16B（offRemap 后移一格对齐）⟹ 段表仍自洽、对齐仍成立，
	// 只有「Tris 段字节 == Σ triCount×3（± <16B 填充）」这条对账能抓住。
	headerOf(b).offRemap += 16;
}
void mutRemapBytesMismatch(BuiltAsset& b) {
	// vertCount 3→8：Σ vertCount×4 = 44 > 段字节 32（超出填充容差）；逐簇 remap 范围
	// [0,8) 恰好仍落在段内 ⟹ 只有 Σ 对账能抓住。
	reinterpret_cast<MeshletDiskCluster*>(b.bytes.data() + b.offCluster)[0].vertCount = 8;
}
void mutTriOffsetOob(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskCluster*>(b.bytes.data() + b.offCluster)[1].triOffset = 100;
}
void mutRemapOffsetOob(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskCluster*>(b.bytes.data() + b.offCluster)[1].remapOffset = 100;
}
void mutTriCountOver8Bit(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskCluster*>(b.bytes.data() + b.offCluster)[0].triCount = 200;
}
void mutGroupRefOob(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskCluster*>(b.bytes.data() + b.offCluster)[0].group = 5;
}
void mutRefinedOob(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskCluster*>(b.bytes.data() + b.offCluster)[1].refined = 5;
}
void mutRefinedSameDepth(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskCluster*>(b.bytes.data() + b.offCluster)[1].refined = 1;
}
void mutRefinedDeeper(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskGroup*>(b.bytes.data() + b.offGroup)[0].depth = 2;
}
void mutGroupRangeOob(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskGroup*>(b.bytes.data() + b.offGroup)[1].clusterOffset = 5;
}
void mutCoverageMismatch(BuiltAsset& b) {
	reinterpret_cast<MeshletDiskGroup*>(b.bytes.data() + b.offGroup)[1].clusterCount = 0;
}
void mutRemapIndexOob(BuiltAsset& b) {
	// 边界值 = vertexCount 本身（不是 999）：`>=` 写成 `>` 的漏检形态只有等号能抓（M3）。
	remapSlot(b, 0) = 6;
}

const std::vector<Mutation>& mutations() {
	static const std::vector<Mutation> table = {
	    {"magic 改字节", mutMagic, "magic mismatch"},
	    {"formatVersion 抬到不支持", mutFormatVersion, "formatVersion"},
	    {"toolVersion 抬到不支持", mutToolVersion, "toolVersion"},
	    {"截断尾部（Nodes 越出文件）", mutTruncateTail, "extends beyond file"},
	    {"截断到不足文件头", mutTruncateHeader, "smaller than FileHeader"},
	    {"首段偏移落回头部内", mutFirstOffsetInHeader, "precedes header end"},
	    {"段偏移重叠（Clusters 压回 VertexPool）", mutOverlap, "overlaps"},
	    {"Remap 段偏移破坏元素对齐", mutMisalignRemap, "not a multiple"},
	    {"偏移 + 大小绕回 64 位", mutOffsetWrap, "wraps around 64-bit"},
	    {"Tris/Remap 段偏移倒挂", mutTrisRemapInverted, "not monotonic"},
	    {"段偏移本身越出文件尾", mutOffsetBeyondFile, "beyond file size"},
	    {"Tris 段字节数与 ΣtriCount×3 不符", mutTrisBytesMismatch, "sum(cluster.triCount)"},
	    {"Remap 段字节数与 ΣvertCount×4 不符", mutRemapBytesMismatch, "sum(cluster.vertCount)"},
	    {"簇 triOffset 越出 Tris 段", mutTriOffsetOob, "tri range"},
	    {"簇 remapOffset 越出 Remap 段", mutRemapOffsetOob, "remap range"},
	    {"簇 triCount 超 8bit 契约", mutTriCountOver8Bit, "8-bit index contract"},
	    {"簇 group 下标越界", mutGroupRefOob, "references group"},
	    {"簇 refined 下标越界", mutRefinedOob, "references refined group"},
	    {"refined 指向同层 group", mutRefinedSameDepth, "not shallower"},
	    {"refined 指向更深 group", mutRefinedDeeper, "not shallower"},
	    {"group 簇区间越出簇数组", mutGroupRangeOob, "cluster range"},
	    {"Σ group.clusterCount != clusterCount", mutCoverageMismatch, "group coverage mismatch"},
	    {"remap 下标 == vertexCount（边界）", mutRemapIndexOob, "remap index beyond vertex pool"},
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

/** 跑一条变异：必须被拒，且错误串含该变异自己的关键词（= 可归因）。 */
void checkMutationRejected(const Mutation& m) {
	BuiltAsset b = makeValid();
	m.apply(b);
	auto r = parseMeshletAsset(std::move(b.bytes));
	INFO("变异: " << std::string(m.name));
	REQUIRE_FALSE(r.has_value());
	INFO("错误串: " << r.error());
	CHECK(r.error().find(m.needle) != std::string::npos);
}

}  // namespace

// ─── 正向 ────────────────────────────────────────────────────────────────────────

TEST_CASE("正向：最小合法合成缓冲解析成功，各段 count/stride/首元素正确") {
	// 规格常量对账（手抄自 format.h；被测镜像漂了这几行先红——两侧 static_assert 互为对账）。
	CHECK(sizeof(MeshletDiskHeader) == kHdrBytes);
	CHECK(sizeof(MeshletDiskPrim) == kPrimBytes);
	CHECK(sizeof(MeshletDiskCluster) == kClusterBytes);
	CHECK(sizeof(MeshletDiskGroup) == kGroupBytes);
	CHECK(sizeof(MeshletDiskNode) == kNodeBytes);
	CHECK(kVertexBytes == 48);

	BuiltAsset b = makeValid();
	auto r = parseMeshletAsset(std::move(b.bytes));
	REQUIRE(r.has_value());
	const MeshletAsset& a = *r;
	// header 元数据透传
	CHECK(a.header.vertexCount == 6);
	CHECK(a.header.levelCount == 2);
	CHECK(a.header.totalTriangleCount == 2);
	// 各段视图 count（= header 计数）
	CHECK(a.prims.size() == 1);
	CHECK(a.vertexPool.size() == 6 * kVertexBytes);
	CHECK(a.clusters.size() == 2);
	// 变长段视图 = 段表字节数（含写侧 <16B 对齐填充，= alignUp16(Σ)，规格手抄）
	CHECK(a.tris.size() == alignUp16(6));
	CHECK(a.remap.size() == alignUp16(6 * 4) / 4);
	CHECK(a.groups.size() == 2);
	CHECK(a.nodes.size() == 2);
	// 首元素/尾元素内容（段偏移或 stride 错 → 这几行红）
	CHECK(a.prims[0].material == 7);
	CHECK(a.clusters[0].refined == -1);
	CHECK(a.clusters[1].refined == 0);
	CHECK(a.clusters[0].center[1] == 2.0f);
	CHECK(a.clusters[1].error == 0.5f);
	CHECK(a.tris[0] == 0);
	CHECK(a.tris[5] == 2);
	CHECK(a.remap[3] == 3);
	CHECK(a.groups[1].depth == 1);
	CHECK(a.groups[0].clusterCount == 1);
	CHECK(a.nodes[1].group == 1);
	uint32_t tag = 0;
	std::memcpy(&tag, a.vertexPool.data(), 4);
	CHECK(tag == 0xC0000000u);
	std::memcpy(&tag, a.vertexPool.data() + kVertexBytes, 4);  // 第 2 顶点 = 偏移 +48（stride 钉）
	CHECK(tag == 0xC0000001u);
	// sourceSha1 只作日志身份（裁决 1）：hex 化正确即可，不做任何校验。
	CHECK(a.sourceSha1Hex == "000102030405060708090a0b0c0d0e0f10111213");
}

TEST_CASE("正向：loadMeshletAsset 磁盘往返成功；不存在的文件被拒且错误可归因") {
	BuiltAsset b = makeValid();
	const auto path = std::filesystem::temp_directory_path() / "rgl_meshlet_asset_roundtrip.bin";
	{
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		out.write(reinterpret_cast<const char*>(b.bytes.data()), long(b.bytes.size()));
	}
	auto r = loadMeshletAsset(path.string());
	INFO("load 错误: " << (r ? std::string() : r.error()));
	REQUIRE(r.has_value());
	CHECK(r->header.vertexCount == 6);
	CHECK(r->clusters.size() == 2);
	std::filesystem::remove(path);

	auto missing = loadMeshletAsset(
	    (std::filesystem::temp_directory_path() / "rgl_meshlet_no_such_file.bin").string());
	REQUIRE_FALSE(missing.has_value());
	CHECK(missing.error().find("file not found") != std::string::npos);
}

// ─── 反向（校验清单 ↔ 用例逐条对应，见文件头与变异表）────────────────────────────

TEST_CASE("反向：magic 被拒") { checkMutationRejected(mutationByName("magic 改字节")); }
TEST_CASE("反向：formatVersion 抬到不支持被拒") {
	checkMutationRejected(mutationByName("formatVersion 抬到不支持"));
}
TEST_CASE("反向：toolVersion 抬到不支持被拒") {
	checkMutationRejected(mutationByName("toolVersion 抬到不支持"));
}
TEST_CASE("反向：截断尾部被拒（Nodes 段越出文件）") {
	checkMutationRejected(mutationByName("截断尾部（Nodes 越出文件）"));
}
TEST_CASE("反向：截断到不足 192B 文件头被拒") {
	checkMutationRejected(mutationByName("截断到不足文件头"));
}
TEST_CASE("反向：首段偏移落回头部内被拒") {
	checkMutationRejected(mutationByName("首段偏移落回头部内"));
}
TEST_CASE("反向：段偏移重叠被拒") { checkMutationRejected(mutationByName("段偏移重叠（Clusters 压回 VertexPool）")); }
TEST_CASE("反向：段偏移破坏元素对齐被拒") {
	checkMutationRejected(mutationByName("Remap 段偏移破坏元素对齐"));
}
TEST_CASE("反向：偏移 + 大小绕回 64 位被拒") {
	checkMutationRejected(mutationByName("偏移 + 大小绕回 64 位"));
}
TEST_CASE("反向：Tris/Remap 段偏移倒挂被拒") {
	checkMutationRejected(mutationByName("Tris/Remap 段偏移倒挂"));
}
TEST_CASE("反向：段偏移本身越出文件尾被拒") {
	checkMutationRejected(mutationByName("段偏移本身越出文件尾"));
}
TEST_CASE("反向：Tris 段字节数与 ΣtriCount×3 不符被拒") {
	checkMutationRejected(mutationByName("Tris 段字节数与 ΣtriCount×3 不符"));
}
TEST_CASE("反向：Remap 段字节数与 ΣvertCount×4 不符被拒") {
	checkMutationRejected(mutationByName("Remap 段字节数与 ΣvertCount×4 不符"));
}
TEST_CASE("反向：簇 triOffset 越出 Tris 段被拒") {
	checkMutationRejected(mutationByName("簇 triOffset 越出 Tris 段"));
}
TEST_CASE("反向：簇 remapOffset 越出 Remap 段被拒") {
	checkMutationRejected(mutationByName("簇 remapOffset 越出 Remap 段"));
}
TEST_CASE("反向：簇 triCount 超 8bit 契约被拒") {
	checkMutationRejected(mutationByName("簇 triCount 超 8bit 契约"));
}
TEST_CASE("反向：簇 group 下标越界被拒") { checkMutationRejected(mutationByName("簇 group 下标越界")); }
TEST_CASE("反向：簇 refined 下标越界被拒") { checkMutationRejected(mutationByName("簇 refined 下标越界")); }
TEST_CASE("反向：refined 指向同层或更深 group 被拒（结构不变量，--verify ④ 同口径）") {
	checkMutationRejected(mutationByName("refined 指向同层 group"));
	checkMutationRejected(mutationByName("refined 指向更深 group"));
}
TEST_CASE("反向：group 簇区间越出簇数组被拒") {
	checkMutationRejected(mutationByName("group 簇区间越出簇数组"));
}
TEST_CASE("反向：Σ group.clusterCount != clusterCount 被拒") {
	checkMutationRejected(mutationByName("Σ group.clusterCount != clusterCount"));
}
TEST_CASE("反向：remap 下标 == vertexCount（边界值）被拒") {
	checkMutationRejected(mutationByName("remap 下标 == vertexCount（边界）"));
}

TEST_CASE("反向：全部变异的错误串两两不同（可归因判据本体）") {
	const auto& table = mutations();
	std::vector<std::string> errors;
	for (const Mutation& m : table) {
		BuiltAsset b = makeValid();
		m.apply(b);
		auto r = parseMeshletAsset(std::move(b.bytes));
		INFO("变异: " << std::string(m.name));
		REQUIRE_FALSE(r.has_value());
		CHECK(r.error().find(m.needle) != std::string::npos);
		errors.push_back(r.error());
	}
	REQUIRE(errors.size() == table.size());
	for (size_t i = 0; i < errors.size(); ++i)
		for (size_t j = i + 1; j < errors.size(); ++j) {
			INFO(std::string(table[i].name) << " vs " << std::string(table[j].name) << ":\n  " << errors[i] << "\n  "
			                   << errors[j]);
			CHECK(errors[i] != errors[j]);
		}
}

}  // namespace RGL
