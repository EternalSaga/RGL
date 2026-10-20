module;
#include <doctest/doctest.h>
#include <cstddef>        // offsetof（ModelVertex 布局对账 static_assert；测试 TU 不受样式规则 B2 管）
#include <initializer_list>  // modules 坑：range-for / 返回的花括号列表要真 STL 头（只 include doctest 不够）

/**
 * test_meshlet_hand —— UV 手性 CPU 参考实现 + 簇→三角寻址遍历（簇化轴 ★0.5-c）
 * 被测对象 = `rgl/meshlet_hand.cppm` + `rgl/meshlet_hand_impl.cppm`（rlbase，纯 CPU 无设备）。
 *
 * ## 这批为什么值得单独一层测试（而不是等 ★1-a1ii 的 shader 一起验）
 * mesh shader 沿簇寻址 + 逐三角重算手性，错了**不报错、只是图像静默错**。本模块是它的对账
 * 伙伴，伙伴自己必须先是对的——判据只有闭式解与 loader 同式公式（模块文件头）。
 *
 * ## 输入 = 合成产物字节流经 `parseMeshletAsset`（不手搭 span）
 * 与 `test_meshlet_ranges` 同法：合成缓冲按 format.h 段序铺字节、走**真实读取路径**。理由在
 * 本批多一条：断言形态（局部索引 < vertCount）负责的正是「**读取侧不查**、只有打包器
 * --verify ② 保证」的那条 ⟹ 反向用例的输入必须是「parse 认得的合法产物」——若手搭 span，
 * 就分不清「模块抓住了坏索引」与「parse 先拒了坏字节流」。
 * 🔴 不把 184MB 的 bistro 产物当测试输入；真产物的两个统计锚（total == l0TriangleCount =
 * 2,832,120 / negative = 2,322,319）由引擎启动 INFO 常驻出声（`main:meshlets` 的
 * `reportHandAnchor`），口径 → 模块文件头。
 *
 * ## 期望值出处（全部闭式手算，不取被测码、不取 loader）
 * 五个正向锚点的 UV 全部取**二进制精确**值，det 逐条手算（推导写在每条用例注释里）：
 *   正手 P = ((0,0),(1,0),(0,1))：s1=(1,0) s2=(0,1)，det = 1·1−0·0 = **+1** ⟹ +1
 *   反手 N = ((0,0),(0,1),(1,0))：s1=(0,1) s2=(1,0)，det = 0·0−1·1 = **−1** ⟹ −1
 *   共线 C = ((0,0),(1,2),(2,4))：s1=(1,2) s2=(2,4)，det = 1·4−2·2 = **0** ⟹ +1（det==0 计正）
 *   重合 D = ((0.5,0.25)×3)：    s1=s2=(0,0)，        det = **0**            ⟹ +1（同上）
 *   NaN  X = ((NaN,0),(1,0),(0,1))：det = NaN，NaN<0 恒假 ⟹ **+1**（loader 现行语义；
 *          写成 `!(det >= 0)` 形态会翻成 −1 —— 这条用例钉的就是算符形状本身）
 *
 * ## 公式对拍（判据 3）：随机 UV 三元组 vs **独立手算**，Lcg 写死种子
 * UV 取 0.25 格点（|值| ≤ 12）⟹ s1/s2 分量是 0.25 的倍数、乘积是 0.0625 的倍数且 |积| ≤ 576
 * ——全部落在 float32 的**精确**区间（576/0.0625 = 9216 ≪ 2²⁴）⟹ float det 与 double det
 * **逐位同号**，对拍没有容差问题。每第 7 条强制共线（uv2 = 2·uv1 − uv0 ⟹ det 恰 0）保证
 * 零行列式形态在样本里非空。🔴 对拍的是**公式**（测试里独立写出的 `det < 0 → −1 否则 +1`），
 * 不 import loader 那个 static 函数（它在 impl 里、也不该被 import）。
 *
 * ## 跨簇不串（经典 off-by-one 的专门哨兵）
 * 「把 A 簇的第三点错接到 B 簇」在本批的形态 = triOffset/remapOffset 的**字节 vs 元素**口径混
 * 淆与**逐簇基址丢失**。用例把四种偏移混淆变异全部设计成**干净红**（读到的仍是界内字节、只是
 * 手性翻号），期望序列 [+1… 全 −1] 的每个位置都可归因：
 *   · 忘加 triOffset（恒从段首读）        → B 读到自己的正手序 → +1 ≠ 期望 −1
 *   · triOffset 当元素偏移（×3）          → B 读到段尾 0 填充 → 三点重合 det=0 → +1
 *   · 忘加 remapOffset（恒从段首查表）    → B 用 A 的顶点反序 → 反手的反手 = +1
 *   · remapOffset 当字节偏移（×4）        → B 读到 C 的 remap（预置成反读恰为正手）→ +1
 *
 * ## 变异红线（哪个变异让我红；**全部实测** 2026-10-12，基线 = 348 用例 / 69309 断言全绿；
 * harness = /tmp/opencode/mutation_hand.py：`cp` 备份还原（不保 mtime）、构建返回码显式核对、
 * **每条变异还原后重跑基线全绿**（13/13 条实测通过这道闸门））
 * M1  公式符号翻转（`< 0.0f` → `> 0.0f`）      → 红 **7**：正手 + 反手 + triCount0 + 跨簇 + L0 + 对拍 + 形态一致
 * M2  det 两乘项互换（`s2x·s1y − s1x·s2y`）     → 红 **7**：同 M1（不同代码形态、同一失效面）
 * M3  `det <= 0`（det==0 划给负手）             → 红 **3**：共线 + 重合 + 对拍（每第 7 条强制零 det）
 * M4  NaN 形态翻转（`!(det >= 0)` 改写）        → 红 **1**：仅 NaN 锚点（其余用例无 NaN，归因精确到算符形状）
 * M5  忘加 triOffset（恒从段首读）              → 红 **3**：跨簇 + L0 + 形态一致
 * M6  triOffset 当元素偏移（再 ×3）             → 红 **3**：跨簇 + 对拍 + L0
 * M7  忘加 remapOffset（恒从段首查表）          → 红 **2**：跨簇 + 对拍（其余夹具 remapOffset 恒 0，不红 = 归因正确）
 * M8  remapOffset 当字节偏移（×4）              → 红 **3**（跨簇 + 对拍 + L0）+ **崩溃**（rc=−11：对拍夹具
 *       里 ×4 越出 Remap 段 → SIGSEGV，其后用例静默不跑 = ★1-a0 记过的形态；判定仍红，红名单不完整）
 * M9  三角基址 off-by-one（`3k` → `3k+1`）      → 红 **4**：跨簇（两簇同时翻号）+ 对拍 + L0 + 形态一致
 * M10 L0 判据取反（`refined == -1` 才跳过）     → 红 **1**：L0 筛选（kL0Only 撞上垃圾簇报错）
 * M11 断言形态短路（`&& false`）                → 红 **2**（断言形态 + L0）+ **崩溃**（255 索引不再被拦 →
 *       remap 越界读 SIGSEGV；「期望 error」变成崩溃仍是红，同 M8 注）
 * M12 uv 字节偏移 24 → 0（把 pos 当 uv）        → 红 **5**：反手 + 跨簇 + L0 + 对拍 + 形态一致（pos 恒零 → det 恒 0）
 * M13 区间前置短路（越界不再报错）              → 红 **2**：区间越界 + 断言形态（后者借越界区间构造）
 * —— 以下两条 = **主 agent 的对账契约面变异**（子 agent 的 M1-M13 全绿时它们也全绿 ⟹ 真缺口）——
 * M14 逐三角载体改前插（全局逆序；统计锚不变）  → 红 **3**：L0 筛选 + 公式对拍 + 形态一致
 *     （**GPU 对账吃的是序不是计数**，序错 = 逐元素静默错位，统计锚照样命中）
 * M15 删掉 `if (wantPerTriangle)`（false 也填）  → 🔴 **首跑存活**；补「统计形态不填载体」断言后红。
 *     原 `perTriangle.empty()` 那条断言长在 **count=0** 的空区间上 = 恒真，钉不住这条契约。
 *     教训与 ★1-a1i 的 ⑫ 同源：**断言要长在"这条契约能被违反"的输入形态上**。
 * 覆盖缺口明写：M8/M11 的崩溃形态下红名单不完整（进程先死）；「remapOffset 当字节偏移」在
 * **全部**夹具上都干净红做不到——跨簇夹具已为它专门垫了界内字节，其余夹具触发的是越界读。
 */
export module test_meshlet_hand;

import rlstd;
import meshlet_asset;    // MeshletAsset / parseMeshletAsset / kMeshletVertexStride
import meshlet_hand;     // traverseClusterHandedness / MeshletHandFilter / 常量
import model_types_generated;  // ModelVertex（只做一件事：把 uv 偏移常量钉到反射产物上）
import test_support;     // Lcg（确定性采样器，种子写死）

namespace RGL {
namespace {

// 布局常量 ↔ 反射产物对账：模块常量若与 model_schema.lua 经反射生成的 ModelVertex 分家，
// 编译不过（引擎侧因样式规则 B2 拿不到 offsetof 宏，这条联动钉在测试面，见模块 impl 注释）。
static_assert(kMeshletHandUvByteOffset == offsetof(ModelVertex, uv),
    "kMeshletHandUvByteOffset 与 ModelVertex.uv 反射偏移分家");
static_assert(kMeshletVertexStride == sizeof(ModelVertex), "顶点步长与 ModelVertex 分家");

// ─── 合成产物构造器（段序/对齐按 format.h 头注释手抄 = 期望值的独立出处，不取被测模块）─────
constexpr size_t kHdrBytes = 192, kPrimBytes = 16, kClusterBytes = 44, kGroupBytes = 36,
                 kVertexBytes = 48;

/** 写侧段对齐（format.h `kSectionAlign` 口径的手抄）。 */
uint64_t alignUp16(uint64_t v) { return (v + 15) / 16 * 16; }

/** 一个合成顶点：只有 uv 被被测模块读到，其余字节铺 0（tangent.w = +1 占位，本模块不读）。 */
struct VertexDesc {
	float u;  ///< ModelVertex.uv.x（字节偏移 24）
	float v;  ///< ModelVertex.uv.y（字节偏移 28）
};

/** 一个合成簇：locals = triCount×3 个 8bit 局部索引（triCount 由长度推导），remap = vertCount 个全局下标。 */
struct ClusterDesc {
	int32_t refined = -1;             ///< -1 = L0（被测模块的筛选判据）
	uint32_t group = 0;               ///< 所属 group 下标
	std::vector<uint8_t> locals;      ///< 三角形局部索引（每三角 3 个）
	std::vector<uint32_t> remap;      ///< 全局顶点下标（vertCount 条）
};

/** 一个合成 group（读取侧要求 Σ clusterCount 恰覆盖簇表；prim 恒 0）。 */
struct GroupDesc {
	int32_t depth = 0;         ///< 0 = L0 级
	uint32_t clusterOffset = 0;
	uint32_t clusterCount = 0;
};

/** 一张合成产物的规格。 */
struct ProductSpec {
	std::vector<VertexDesc> vertices;
	std::vector<ClusterDesc> clusters;
	std::vector<GroupDesc> groups;  ///< 必须恰覆盖簇表（读取侧 Σ 对账）
};

/**
 * 把规格铺成 `parseMeshletAsset` 认得的字节流（段序同 format.h：头 → Prim → Vertex →
 * Clusters → Tris → Remap → Groups；nodeCount = 0；段间 16B 对齐 0x00 填充）。
 * triOffset（**字节**）/ remapOffset（**元素**）按簇序累加——与被测模块的寻址口径同源不同实现，
 * 铺字节这边若写错，正向用例先红（构造器自己也被钉着）。
 */
std::vector<std::byte> buildProduct(const ProductSpec& spec) {
	const uint32_t clusterCount = static_cast<uint32_t>(spec.clusters.size());
	const uint32_t groupCount = static_cast<uint32_t>(spec.groups.size());
	const uint32_t vertexCount = static_cast<uint32_t>(spec.vertices.size());

	uint64_t triBytes = 0, remapElems = 0;
	uint64_t l0Tris = 0, allTris = 0;
	for (const ClusterDesc& c : spec.clusters) {
		triBytes += c.locals.size();
		remapElems += c.remap.size();
		allTris += c.locals.size() / 3;
		if (c.refined == -1)
			l0Tris += c.locals.size() / 3;
	}

	uint64_t off = kHdrBytes;
	const uint64_t offPrim = off;
	off = alignUp16(off + kPrimBytes);  // primCount = 1
	const uint64_t offVertex = off;
	off = alignUp16(off + uint64_t(vertexCount) * kVertexBytes);
	const uint64_t offCluster = off;
	off = alignUp16(off + uint64_t(clusterCount) * kClusterBytes);
	const uint64_t offTris = off;
	off = alignUp16(off + triBytes);
	const uint64_t offRemap = off;
	off = alignUp16(off + remapElems * 4);
	const uint64_t offGroup = off;
	off = alignUp16(off + uint64_t(groupCount) * kGroupBytes);
	const uint64_t offNode = off;  // nodeCount = 0

	std::vector<std::byte> bytes(static_cast<size_t>(off), std::byte { 0 });
	auto* base = bytes.data();

	auto* h = reinterpret_cast<MeshletDiskHeader*>(base);
	std::memcpy(h->magic, kMeshletMagic, sizeof(kMeshletMagic));
	h->formatVersion = 1;
	h->toolVersion = 1;
	h->primCount = 1;
	h->vertexCount = vertexCount;
	h->clusterCount = clusterCount;
	h->groupCount = groupCount;
	h->nodeCount = 0;
	h->levelCount = 1;
	h->nodeWidth = 8;
	h->l0TriangleCount = l0Tris;
	h->totalTriangleCount = allTris;
	h->offPrimTable = offPrim;
	h->offVertexPool = offVertex;
	h->offClusters = offCluster;
	h->offTris = offTris;
	h->offRemap = offRemap;
	h->offGroups = offGroup;
	h->offNodes = offNode;

	reinterpret_cast<MeshletDiskPrim*>(base + offPrim)[0] = MeshletDiskPrim { 0, vertexCount, 3, 0 };

	auto* verts = base + offVertex;
	for (uint32_t v = 0; v < vertexCount; ++v) {
		const size_t vbase = size_t(v) * kVertexBytes;
		std::memcpy(verts + vbase + kMeshletHandUvByteOffset, &spec.vertices[v].u, 4);
		std::memcpy(verts + vbase + kMeshletHandUvByteOffset + 4, &spec.vertices[v].v, 4);
	}

	auto* clusters = reinterpret_cast<MeshletDiskCluster*>(base + offCluster);
	auto* tris = base + offTris;
	auto* remap = reinterpret_cast<uint32_t*>(base + offRemap);
	uint32_t triOff = 0, remapOff = 0;
	for (uint32_t i = 0; i < clusterCount; ++i) {
		const ClusterDesc& c = spec.clusters[i];
		clusters[i] = MeshletDiskCluster {};
		clusters[i].group = c.group;
		clusters[i].refined = c.refined;
		clusters[i].triCount = static_cast<uint32_t>(c.locals.size() / 3);
		clusters[i].vertCount = static_cast<uint32_t>(c.remap.size());
		clusters[i].triOffset = triOff;
		clusters[i].remapOffset = remapOff;
		// 🔴 空簇必须跳过这次 memcpy：`c.locals` 为空时 `data()` 是 nullptr，而
		// `memcpy(dst, nullptr, 0)` 按 C 标准是 UB（两个指针实参即使在 n=0 时也必须是
		// 可解引用的地址）。UBSan 实测点名本行（2026-10-20，本文件「triCount = 0 的簇」
		// 用例）；今天不出事只因为 glibc 的 memcpy 对 n=0 直接返回，换 libc 或换优化档
		// 就是未定义行为——本用例造的正是空簇，绕不开。
		if (!c.locals.empty())
			std::memcpy(tris + triOff, c.locals.data(), c.locals.size());
		for (size_t r = 0; r < c.remap.size(); ++r)
			remap[remapOff + r] = c.remap[r];
		triOff += static_cast<uint32_t>(c.locals.size());
		remapOff += static_cast<uint32_t>(c.remap.size());
	}

	auto* groups = reinterpret_cast<MeshletDiskGroup*>(base + offGroup);
	for (uint32_t i = 0; i < groupCount; ++i) {
		groups[i] = MeshletDiskGroup {};
		groups[i].depth = spec.groups[i].depth;
		groups[i].prim = 0;
		groups[i].clusterOffset = spec.groups[i].clusterOffset;
		groups[i].clusterCount = spec.groups[i].clusterCount;
	}
	return bytes;
}

/**
 * 合成产物 → parse。**parse 必须成功**（REQUIRE）：反向用例验的是「读取器认得的合法产物里，
 * 读取器**不查**的那条（局部索引）被本模块抓住」；parse 自己先红 = 合成数据写错了，不许假绿。
 */
MeshletAsset parseProduct(const ProductSpec& spec) {
	auto parsed = parseMeshletAsset(buildProduct(spec));
	REQUIRE(parsed.has_value());
	return std::move(*parsed);
}

/** 单簇单三角产物（五个正向锚点的公共形状）：locals 恒 (0,1,2)，remap 恒 [0,1,2]，L0。 */
ProductSpec oneTriProduct(std::initializer_list<VertexDesc> uv3) {
	ProductSpec spec;
	spec.vertices = std::vector<VertexDesc>(uv3);
	ClusterDesc c;
	c.locals = { 0, 1, 2 };
	c.remap = { 0, 1, 2 };
	spec.clusters = { c };
	spec.groups = { GroupDesc { 0, 0, 1 } };
	return spec;
}

// 五个闭式 UV 三元组（推导见文件头「期望值出处」）。
const VertexDesc kPos[3] = { { 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };
const VertexDesc kNeg[3] = { { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 0.0f } };
const VertexDesc kCol[3] = { { 0.0f, 0.0f }, { 1.0f, 2.0f }, { 2.0f, 4.0f } };
const VertexDesc kDup[3] = { { 0.5f, 0.25f }, { 0.5f, 0.25f }, { 0.5f, 0.25f } };
const float kNan = std::numeric_limits<float>::quiet_NaN();
const VertexDesc kNanUv[3] = { { kNan, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };

/** 把 initializer_list 的 UV 三元组铺进 ProductSpec（oneTriProduct 的数组版）。 */
ProductSpec triProduct3(const VertexDesc (&uv3)[3]) {
	return oneTriProduct({ uv3[0], uv3[1], uv3[2] });
}

/** 跑一次遍历（统计形态）。 */
std::expected<MeshletHandResult, std::string> traverseStats(const MeshletAsset& asset,
	uint32_t base, uint32_t count, MeshletHandFilter filter = MeshletHandFilter::kAll) {
	return traverseClusterHandedness(asset, base, count, filter, false);
}

}  // namespace

// ─── 正向锚点（闭式手算，五种形态各一条）─────────────────────────────────────────────

TEST_CASE("正向锚点 · UV 正手：s1=(1,0) s2=(0,1)，det = 1·1−0·0 = +1 → +1") {
	const MeshletAsset asset = parseProduct(triProduct3(kPos));
	const auto r = traverseStats(asset, 0, 1);
	REQUIRE(r.has_value());
	CHECK(r->total == 1);
	CHECK(r->negative == 0);
	const auto p = traverseClusterHandedness(asset, 0, 1, MeshletHandFilter::kAll, true);
	REQUIRE(p.has_value());
	CHECK(p->perTriangle == std::vector<int8_t> { kMeshletHandPositive });
}

TEST_CASE("正向锚点 · UV 反手：s1=(0,1) s2=(1,0)，det = 0·0−1·1 = −1 → −1") {
	const MeshletAsset asset = parseProduct(triProduct3(kNeg));
	const auto r = traverseStats(asset, 0, 1);
	REQUIRE(r.has_value());
	CHECK(r->total == 1);
	CHECK(r->negative == 1);
	const auto p = traverseClusterHandedness(asset, 0, 1, MeshletHandFilter::kAll, true);
	REQUIRE(p.has_value());
	CHECK(p->perTriangle == std::vector<int8_t> { kMeshletHandNegative });
}

TEST_CASE("正向锚点 · 退化（三 UV 重合）：s1 = s2 = (0,0)，det 恰 0 → +1（det==0 计正，loader 现行语义）") {
	const MeshletAsset asset = parseProduct(triProduct3(kDup));
	const auto r = traverseStats(asset, 0, 1);
	REQUIRE(r.has_value());
	CHECK(r->total == 1);
	CHECK(r->negative == 0);
}

TEST_CASE("正向锚点 · 三点共线 UV（互异点）：s1=(1,2) s2=(2,4)，det = 1·4−2·2 = 0 → +1") {
	// 与「重合」是两种 det==0 形态：重合 = 两条边都是零向量；共线 = 两条边非零但平行。
	// 分开写是因为它们对「det 判据写成 `<= 0`」的变异（M3）是**同一个入口**——合并写会少一层归因。
	const MeshletAsset asset = parseProduct(triProduct3(kCol));
	const auto r = traverseStats(asset, 0, 1);
	REQUIRE(r.has_value());
	CHECK(r->total == 1);
	CHECK(r->negative == 0);
}

TEST_CASE("正向锚点 · NaN UV：det = NaN，NaN < 0 恒假 → +1（钉算符形状 `det < 0 ? −1 : +1`）") {
	// loader 现行语义：`(s1x*s2y − s2x*s1y) < 0.0f ? −1 : +1`——NaN 走 +1 是**比较恒假**的
	// 直接推论。把它「等价改写」成 `!(det >= 0) ? −1 : +1` 会把 NaN 翻成 −1（NaN >= 0 也恒假）
	// ——本条是那个改写唯一的红点（其余用例无 NaN），归因精确到算符形状。
	const MeshletAsset asset = parseProduct(triProduct3(kNanUv));
	const auto r = traverseStats(asset, 0, 1);
	REQUIRE(r.has_value());
	CHECK(r->total == 1);
	CHECK(r->negative == 0);
}

// ─── 边界与反向 ────────────────────────────────────────────────────────────────────

TEST_CASE("triCount = 0 的簇不贡献三角（空簇夹在正常簇中间也不扰动计数）") {
	// 三簇：A = 0 三角 3 顶点、B = 1 正手三角、C = 0 三角 0 顶点。
	// 闭式期望：total = 1、negative = 0、perTriangle 长度 1（空簇不产条目）。
	ProductSpec spec;
	spec.vertices = std::vector<VertexDesc>(kPos, kPos + 3);
	ClusterDesc a;
	a.locals = {};
	a.remap = { 0, 1, 2 };
	ClusterDesc b;
	b.locals = { 0, 1, 2 };
	b.remap = { 0, 1, 2 };
	ClusterDesc c;  // 0 三角 0 顶点
	spec.clusters = { a, b, c };
	spec.groups = { GroupDesc { 0, 0, 3 } };
	const MeshletAsset asset = parseProduct(spec);

	const auto r = traverseStats(asset, 0, 3);
	REQUIRE(r.has_value());
	CHECK(r->total == 1);
	CHECK(r->negative == 0);
	const auto p = traverseClusterHandedness(asset, 0, 3, MeshletHandFilter::kAll, true);
	REQUIRE(p.has_value());
	CHECK(p->perTriangle == std::vector<int8_t> { kMeshletHandPositive });
}

TEST_CASE("簇区间为空：count = 0 → 全 0 结果（含 base == clusterCount 的表尾空区间）") {
	const MeshletAsset asset = parseProduct(triProduct3(kNeg));
	// 表头空区间
	const auto head = traverseClusterHandedness(asset, 0, 0, MeshletHandFilter::kAll, true);
	REQUIRE(head.has_value());
	CHECK(head->total == 0);
	CHECK(head->negative == 0);
	CHECK(head->perTriangle.empty());
	// 表尾空区间：base + count **恰等于**簇表条数是合法的（end 取等号不越界）。
	const auto tail = traverseStats(asset, 1, 0);
	REQUIRE(tail.has_value());
	CHECK(tail->total == 0);
}

TEST_CASE("区间越出簇表 → 可归因错误（本模块自己的 API 前置，不归读取侧管）") {
	const MeshletAsset asset = parseProduct(triProduct3(kPos));
	const auto over = traverseStats(asset, 0, 2);  // 只有 1 簇
	REQUIRE_FALSE(over.has_value());
	CHECK(over.error().find("out of bounds") != std::string::npos);
	const auto pastEnd = traverseStats(asset, 2, 0);  // base 已在表外
	REQUIRE_FALSE(pastEnd.has_value());
	CHECK(pastEnd.error().find("out of bounds") != std::string::npos);
}

TEST_CASE("跨簇不串：逐簇 triOffset（字节）/ remapOffset（元素）基址，四种偏移混淆变异全红") {
	// 顶点（下标 = 全局顶点号）：
	//   A 三件套 g0=(0,0) g1=(0,1) g2=(1,0) —— 顺序 (0,1,2) 是**反手**（det = −1）。
	//   B 三件套 g3=(0,0) g4=(1,0) g5=(0,1) —— 顺序 (3,4,5) 是正手，**反读 (5,4,3) 是反手**。
	// 簇：A locals (0,1,2) remap [0,1,2]；B locals (2,1,0) remap [3,4,5]；
	//     C 0 三角、9 条 remap（把 Remap 段垫到 ≥ 15 元素，让「remapOffset ×4」的误读**落在界内**），
	//     其中 C 的 remap[6..8]（段内元素 12,13,14）预置为 (5,4,3)：×4 变异反读它们 = 正读 (3,4,5) = 正手。
	// 闭式期望：A = 反手 −1；B = (g5,g4,g3) 反读正手三件套 = 反手 −1 ⟹ total=2, negative=2。
	// 四种「A 的第三点错接到 B」同族变异（忘加 triOffset / triOffset ×3 / 忘加 remapOffset /
	// remapOffset ×4）都会把 B（或 A）翻成 +1 —— 文件头「跨簇不串」段逐条推导。
	ProductSpec spec;
	spec.vertices = { { 0.0f, 0.0f }, { 0.0f, 1.0f }, { 1.0f, 0.0f },
		{ 0.0f, 0.0f }, { 1.0f, 0.0f }, { 0.0f, 1.0f } };
	ClusterDesc a;
	a.locals = { 0, 1, 2 };
	a.remap = { 0, 1, 2 };
	ClusterDesc b;
	b.locals = { 2, 1, 0 };
	b.remap = { 3, 4, 5 };
	ClusterDesc c;
	c.locals = {};
	c.remap = { 0, 0, 0, 0, 0, 0, 5, 4, 3 };  // 元素 12,13,14 = (5,4,3)
	spec.clusters = { a, b, c };
	spec.groups = { GroupDesc { 0, 0, 3 } };
	const MeshletAsset asset = parseProduct(spec);

	const auto p = traverseClusterHandedness(asset, 0, 3, MeshletHandFilter::kAll, true);
	REQUIRE(p.has_value());
	CHECK(p->total == 2);
	CHECK(p->negative == 2);
	CHECK(p->perTriangle == std::vector<int8_t> { kMeshletHandNegative, kMeshletHandNegative });
}

TEST_CASE("L0 筛选（refined == −1）：被滤簇一条字节都不读（垃圾 tris 不响），kAll 才响") {
	// 三簇：c0 = L0 正手、c1 = 非 L0（group 1 depth 1，refined → group 0）且 tris 填 255 垃圾、
	// c2 = L0 反手。闭式期望：kL0Only → total=2, negative=1, perTriangle=[+1,−1]（垃圾不读 ⟹
	// 断言形态不触发）；kAll → 断言形态报错（局部索引 255 ≥ vertCount 3）。
	// 「过滤形态不读被过滤数据」若哪天变成「先读后滤」，kL0Only 这条会先红。
	ProductSpec spec;
	spec.vertices = std::vector<VertexDesc>(kNeg, kNeg + 3);  // (0,1,2) 顺序 = 反手
	ClusterDesc c0;
	c0.locals = { 2, 1, 0 };  // 反读反手三件套 = 正手
	c0.remap = { 0, 1, 2 };
	ClusterDesc c1;
	c1.refined = 0;
	c1.group = 1;
	c1.locals = { 255, 255, 255 };
	c1.remap = { 0, 1, 2 };
	ClusterDesc c2;
	c2.group = 2;
	c2.locals = { 0, 1, 2 };  // 反手
	c2.remap = { 0, 1, 2 };
	spec.clusters = { c0, c1, c2 };
	spec.groups = { GroupDesc { 0, 0, 1 }, GroupDesc { 1, 1, 1 }, GroupDesc { 0, 2, 1 } };
	const MeshletAsset asset = parseProduct(spec);

	const auto l0 = traverseClusterHandedness(asset, 0, 3, MeshletHandFilter::kL0Only, true);
	REQUIRE(l0.has_value());
	CHECK(l0->total == 2);
	CHECK(l0->negative == 1);
	CHECK(l0->perTriangle ==
		std::vector<int8_t> { kMeshletHandPositive, kMeshletHandNegative });

	const auto all = traverseStats(asset, 0, 3);
	REQUIRE_FALSE(all.has_value());
	CHECK(all.error().find("8-bit local index contract") != std::string::npos);
}

TEST_CASE("断言形态 · 局部索引 ≥ 128 必须不可能：parse 放行（读取侧不查），遍历必须红") {
	// 8bit 可解性用例：局部索引 200（≥ 128，也 ≥ vertCount=3）。读取侧只查**计数** ≤ 128 与
	// tri/remap 范围，**不查局部索引值**——parse 必须成功（REQUIRE 在 parseProduct 里），
	// 于是「遍历报错」就是本模块断言形态的独立证据；若它短路（变异 M11），这里拿到的是
	// value 而不是 error ⟹ 红。
	// 三种形态：200（≥128）、3（≥ vertCount 但 < 128，更严形态）、正对照 (0,2,1) 必须绿。
	auto specWith = [](std::vector<uint8_t> locals) {
		ProductSpec spec;
		spec.vertices = std::vector<VertexDesc>(kPos, kPos + 3);
		ClusterDesc c;
		c.locals = std::move(locals);
		c.remap = { 0, 1, 2 };
		spec.clusters = { c };
		spec.groups = { GroupDesc { 0, 0, 1 } };
		return spec;
	};

	const MeshletAsset bad128 = parseProduct(specWith({ 0, 1, 200 }));
	const auto r128 = traverseStats(bad128, 0, 1);
	REQUIRE_FALSE(r128.has_value());
	CHECK(r128.error().find("8-bit local index contract") != std::string::npos);
	CHECK(r128.error().find("cluster 0 triangle 0") != std::string::npos);

	const MeshletAsset badVert = parseProduct(specWith({ 0, 3, 1 }));
	const auto rVert = traverseStats(badVert, 0, 1);
	REQUIRE_FALSE(rVert.has_value());
	CHECK(rVert.error().find("8-bit local index contract") != std::string::npos);

	// 正对照：同夹具把坏索引换回界内值必须绿（否则上面两条的「红」不说明任何事）。
	const MeshletAsset good = parseProduct(specWith({ 0, 2, 1 }));
	const auto rg = traverseStats(good, 0, 1);
	REQUIRE(rg.has_value());
	CHECK(rg->total == 1);

	// 两种失败形态错误串不同 = 可归因（与「区间越界」也两两不同）。
	const auto oob = traverseStats(bad128, 0, 9);
	REQUIRE_FALSE(oob.has_value());
	CHECK(r128.error() != rVert.error());  // 点名的局部值不同
	CHECK(r128.error() != oob.error());
	CHECK(oob.error().find("out of bounds") != std::string::npos);
}

// ─── 公式对拍（判据 3）与输出形态一致性 ─────────────────────────────────────────────

TEST_CASE("公式对拍：384 个随机 UV 三角（Lcg 种子 20261012，0.25 格点）vs 独立手算逐条一致") {
	// 期望值 = 测试里**独立写出**的 loader 语义：det = s1x·s2y − s2x·s1y（double 算），
	// det < 0 → −1 否则 +1。格点论证（文件头）：float det 与 double det 逐位同号，零容差。
	// 每第 7 条强制共线（uv2 = 2·uv1 − uv0 ⟹ det 恰 0），保证 det==0 形态在样本里非空。
	constexpr uint32_t kTriCount = 384;
	constexpr uint32_t kTrisPerCluster = 42;  // 126 顶点/簇 ≤ 128（8bit 契约内）
	testing::Lcg rng(20261012u);
	const auto grid = [&rng]() { return static_cast<float>(rng.uniformInt(-16, 16)) * 0.25f; };

	ProductSpec spec;
	std::vector<int8_t> expected;
	expected.reserve(kTriCount);
	uint32_t nPos = 0, nNeg = 0, nZero = 0;

	ClusterDesc cur;
	for (uint32_t t = 0; t < kTriCount; ++t) {
		const float u0 = grid(), v0 = grid(), u1 = grid(), v1 = grid();
		float u2, v2;
		if (t % 7 == 0) {
			u2 = 2.0f * u1 - u0;  // 强制共线：det 恰 0
			v2 = 2.0f * v1 - v0;
		} else {
			u2 = grid();
			v2 = grid();
		}
		const uint32_t l0 = static_cast<uint32_t>(cur.locals.size());
		cur.locals.push_back(static_cast<uint8_t>(l0));
		cur.remap.push_back(static_cast<uint32_t>(spec.vertices.size()));
		spec.vertices.push_back({ u0, v0 });
		cur.locals.push_back(static_cast<uint8_t>(l0 + 1));
		cur.remap.push_back(static_cast<uint32_t>(spec.vertices.size()));
		spec.vertices.push_back({ u1, v1 });
		cur.locals.push_back(static_cast<uint8_t>(l0 + 2));
		cur.remap.push_back(static_cast<uint32_t>(spec.vertices.size()));
		spec.vertices.push_back({ u2, v2 });

		// —— 独立手算（double；格点论证 ⟹ 与 float 判号一致）——
		const double s1x = double(u1) - double(u0), s1y = double(v1) - double(v0);
		const double s2x = double(u2) - double(u0), s2y = double(v2) - double(v0);
		const double det = s1x * s2y - s2x * s1y;
		expected.push_back(det < 0.0 ? kMeshletHandNegative : kMeshletHandPositive);
		if (det < 0.0)
			++nNeg;
		else if (det == 0.0)
			++nZero;
		else
			++nPos;

		if (cur.locals.size() / 3 == kTrisPerCluster || t + 1 == kTriCount) {
			spec.clusters.push_back(cur);
			cur = ClusterDesc {};
		}
	}
	// 前提守卫：生成器没退化（三种符号都出现过；样本里 det==0 由每第 7 条强制保证）。
	CHECK(nPos > 0);
	CHECK(nNeg > 0);
	CHECK(nZero > 0);
	spec.groups = { GroupDesc { 0, 0, static_cast<uint32_t>(spec.clusters.size()) } };

	const MeshletAsset asset = parseProduct(spec);
	const auto r = traverseClusterHandedness(asset, 0,
		static_cast<uint32_t>(spec.clusters.size()), MeshletHandFilter::kAll, true);
	REQUIRE(r.has_value());
	CHECK(r->total == kTriCount);
	CHECK(r->perTriangle.size() == expected.size());
	for (size_t t = 0; t < expected.size(); ++t) {
		INFO("tri " << t << " expected " << int(expected[t]) << " got "
		            << (t < r->perTriangle.size() ? int(r->perTriangle[t]) : int(99)));
		REQUIRE(r->perTriangle[t] == expected[t]);  // REQUIRE：逐条对拍中途崩了要立刻停
	}
	// 聚合计数与**期望序列**（不是被测输出）对账。
	CHECK(r->negative == uint64_t(nNeg));
}

TEST_CASE("两种输出形态一致：统计形态与逐三角形态聚合相等、条目数 == total") {
	// GPU 对账走 perTriangle、统计锚走 total/negative——两条路必须同源（哪天 perTriangle
	// 变成「第二份实现」并漂了，这条先红；「别只给计数把路堵死」的反面是给了但和计数打架）。
	ProductSpec spec;
	spec.vertices = std::vector<VertexDesc>(kNeg, kNeg + 3);
	ClusterDesc a;
	a.locals = { 0, 1, 2, 0, 1, 2 };  // 同簇两三角，反手 ×2
	a.remap = { 0, 1, 2 };
	ClusterDesc b;
	b.locals = { 2, 1, 0 };  // 反读 = 正手
	b.remap = { 0, 1, 2 };
	spec.clusters = { a, b };
	spec.groups = { GroupDesc { 0, 0, 2 } };
	const MeshletAsset asset = parseProduct(spec);

	const auto stats = traverseStats(asset, 0, 2);
	const auto per = traverseClusterHandedness(asset, 0, 2, MeshletHandFilter::kAll, true);
	REQUIRE(stats.has_value());
	REQUIRE(per.has_value());
	CHECK(per->total == stats->total);
	CHECK(per->negative == stats->negative);
	CHECK(per->perTriangle.size() == stats->total);
	// 🔴 **`wantPerTriangle=false` 的契约半边**（主 agent 补）：统计形态必须**不填**逐三角载体。
	// 缺它时「把 `if (wantPerTriangle)` 删掉」这种变异**全绿存活**（实测）——而这条 flag 的意义正是
	// 启动期统计锚（真产物 2.83M 三角）不要为省下来的读数先分配 2.83M 个条目；★1-a1ii 的调用方
	// 也会按「false ⟹ 空」用。`total > 0` 是本断言能生效的前提（空区间上"空"是恒真，钉不住）。
	CHECK(stats->total > 0);
	CHECK(stats->perTriangle.empty());
	// 闭式期望：[−1, −1, +1]（同簇第二三角与第一三角同值 = 逐三角读、不是逐簇读的反证）。
	CHECK(per->perTriangle == std::vector<int8_t> { kMeshletHandNegative, kMeshletHandNegative,
		                          kMeshletHandPositive });
}

}  // namespace RGL
