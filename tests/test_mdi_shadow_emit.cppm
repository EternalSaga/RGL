module;
#include <doctest/doctest.h>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <cstddef>           // offsetof（constexpr 桥的期望值侧）
#include <cmath>
#include <glm/glm.hpp>

/**
 * test_mdi_shadow_emit —— ★M-1b 真 MDI 阴影段的**无 GPU 采集判据**（docs/mdi_plan.md §19.2 D4 / §19.5）。
 *
 * 被测对象 = `render_list:mdi` 的 `emitShadowCasters`（生效档形态：`buildMdiShadowPlans` →
 * 同源 pair → 每级联一条 MDI DrawCall）+ schema 生成常量与 `mdi_rows` 常量的 **constexpr 桥**。
 * 设备侧（载体 upload / 真 MDI 展开）归 `test_meshlet_shadow_args`（已有）与实机判据
 * A3/A4/A5（不在单测射程——无 VkDevice 就打不了执行器分支，写在这里也只会是假绿）。
 *
 * ## 三处对账闭环里本文件承担的那一环
 * 行表规格（24 / 12 / 16）的拼写点：① `mdi_rows.cppm`（唯一真相源 = sizeof + static_assert）；
 * ② `meshlet_cull_schema.lua constants` → `meshlet_cull_types_generated`（checks 射程里的第二
 * 拼写，`meshlet_spec_checks` 对账 shader 字面量）；③ **本文件的 constexpr 桥**：生成常量 ↔
 * `kMdiShadowRowStride`/`kMdiShadowCommandBytes`/offsetof(MdiShadowRow)。三处合起来 = 无第四形态。
 *
 * ## 输入形态（沿 test_shadow_lod_emit 的闭式口径，期望值出处 = 本文件写死的字面量）
 * 光空间体 view=identity、xyRadius 逐级 1/2/3/4、t ±1e6 ⟹ 可见性闭式 `|x| ≤ 1+c+r`（r=0.5）。
 * 候选：A(slot 2, x=0)、B(slot 5, x=2.5)、C(slot 9, x=4.5) 三枚可切（带 MDI 宿主）；
 * N(slot 3, x=0.2) 簇可切但**无 MDI 宿主**（混合形态的另一半）；D(slot 4, x=0.4) 全旧路径。
 * 级联可见集（闭式手算，逐级 c1/c2/c3）：A 恒可见；B 全可见（|2.5|≤2.5）；
 * C 仅 c3（4.5 ≤ 4+0.5）⟹ 逐行 gcX 期望 = c1 [8,2,0] / c2 [8,2,0] / c3 [8,2,14]
 * （taskCount = 2×簇数：A 4→8、B 1→2、C 7→14）。
 * 同源 pair 闭式：sliceRows=12、frameSlot=1 ⟹ row = 1×12 + firstRow，firstRow 逐级 0/3/6
 * ⟹ rowBase = 12/15/18、argsOffset = row×24 = 288/360/432（**全部写死字面**，
 *  不在期望侧调用 mdiShadowRowLocation = 不把被测映射抄进期望）。
 *
 * ## 变异红线声明（每条断言「哪个变异让我红」——变异实测记录见交付报告）
 *   M1 `absorbMdiCandidate` 里 `c < 1u` 改成 `c < 0u` → 红「c0 恒旧路径：MDI 条目数为 0」
 *   M2 `!plans[c].drawn` 判据短路（恒真 = 每候选都插一条）→ 红「每级联 MDI 条目恰 1」
 *   M3 `argsOffset = static_cast<uint32_t>(seg.loc.argsOffset)` 换成 `seg.loc.rowBase`
 *      （两数 12 vs 288 不相等，M4 教训的换位探测）→ 红「argsOffset == 288/360/432」
 *   M4 push 的 word0 写成 objectIndex（旧语义忘切行基址）→ 红「push word0 == rowBase」
 *   M5 段尾补发循环整个摘掉 → 红「全不可见级联照发一条（段尾）」
 *   M6 `mdiScanGate` 容量判据 `>` 弱化成 `>=`…（弱化半边，见 fallback 组各条）→ 红对应回落用例
 *   M7 `rowsPerCascade < 2u` 改成 `< 1u` → 红「单可切候选回落 = 零段 + 无 drawCount>1」
 *   M8 桥常量被漂移（schema 改数字不改 shader，或反之）→ 桥用例直接编译期红
 *   M9 `firstRow += gate.rowsPerCascade` 写成 `+= 1` → 红「c2/c3 的 pair 数字 360/432」
 *   M10 构建器输入 `in.visible = casterInShadowVolume(...)` 恒置 true（剔除判据失效）
 *      → 红「逐行 gcX == [8,2,0]/[8,2,0]/[8,2,14]」
 *
 * ## 已知覆盖缺口（诚实声明）
 *   · 执行器 `prepareMdiShadowRows` 的四道 fail-fast 与 `drawOne` 的 MDI 分支要真设备 +
 *     Renderer 对象，单测打不到 ⟹ 归 A3/A4/A6（实机）+ 变异 harness（临时复现档）。
 *   · 「回落时 WARNING 出声」断言的是**形态**（segments 清空 + 逐条 draw 原样），文案归因
 *     靠人工读日志（RLLogger 是进程单例，不适合在单测里劫持）。
 */
export module test_mdi_shadow_emit;

import rlstd;
import render_list;                   // emitShadowCasters / MdiShadowEmitContext / ShadowCandidate
import frame_draw_list;               // DrawCall / MdiShadowSegment / ShadowMeshletDrawArgs
import scene_context;                 // ShadowVolume
import scene_data;                    // kActiveShadowCascades
import asset_table;                   // kInvalidPipelineIndex
import mdi_rows;                      // MdiShadowRow / kMdiShadowRowStride / kMdiShadowCommandBytes
import meshlet_cull_types_generated;  // schema 侧的三枚生成常量（桥的另一端）

namespace RGL {
namespace {

constexpr uint32_t kMdiEntry = 475;   // MDI 条目下标（假值即可，被测侧只搬运）
constexpr uint32_t kClusterEntry = 77;  // N 的簇条目（混合形态的另一半）
constexpr uint32_t kOldEntry = 12;    // 全旧路径条目

/** 行表规格常量（offsetof 是**期望值侧**的独立量：编译器实查，不经任何被测代码）。 */
constexpr uint32_t kObjOff = static_cast<uint32_t>(offsetof(MdiShadowRow, objectIndex));
constexpr uint32_t kStartOff = static_cast<uint32_t>(offsetof(MdiShadowRow, clusterStart));

std::array<ShadowVolume, kActiveShadowCascades> makeVolumes() {
	std::array<ShadowVolume, kActiveShadowCascades> vols{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		vols[c].viewProj = glm::mat4(1.0f);
		vols[c].view = glm::mat4(1.0f);
		vols[c].xyRadius = 1.0f + static_cast<float>(c);
		vols[c].eyeOffset = 0.0f;
		vols[c].tMin = -1.0e6f;
		vols[c].tMax = 1.0e6f;
	}
	return vols;
}

/** 闭式可见性（期望值出处，不调 casterInShadowVolume——与 test_shadow_lod_emit 同纪律）。 */
bool visClosed(uint32_t c, float x, float r) {
	return std::abs(x) <= 1.0f + static_cast<float>(c) + r;
}

ShadowCandidate cand(uint32_t slot, float x, uint32_t oldPipeline, uint32_t meshletPipeline,
	uint32_t mdiPipeline, uint32_t clusters, uint32_t start) {
	ShadowCandidate cd;
	cd.center = glm::vec3{x, 0.0f, 0.0f};
	cd.radius = 0.5f;
	cd.objectSlot = slot;
	cd.pipeline = oldPipeline;
	cd.groups = 9u;  // 旧路径 groupCount（本文件不关心其值，占非零）
	cd.meshletPipeline = meshletPipeline;
	cd.meshletMdiPipeline = mdiPipeline;
	cd.meshletGroups = clusters;  // 选级簇数（emit 侧 ×2）
	cd.clusterStart = start;
	return cd;
}

/** 标准候选集：A/B/C 可切（带 MDI），N 簇可切无 MDI，D 全旧。 */
std::vector<ShadowCandidate> makeCands() {
	return {
		cand(2u, 0.0f, kOldEntry, kClusterEntry, kMdiEntry, 4u, 3u),   // A
		cand(3u, 0.2f, kOldEntry, kClusterEntry, kInvalidPipelineIndex, 5u, 0u),  // N
		cand(4u, 0.4f, kOldEntry, kInvalidPipelineIndex, kInvalidPipelineIndex, 0u, 0u),  // D
		cand(5u, 2.5f, kOldEntry, kClusterEntry, kMdiEntry, 1u, 0u),  // B
		cand(9u, 4.5f, kOldEntry, kClusterEntry, kMdiEntry, 7u, 11u),  // C
	};
}

struct MdiFixture {
	std::vector<RGL::ShadowDrawArgs> old8;
	std::vector<RGL::ShadowMeshletDrawArgs> meshlet12;
	std::array<std::vector<RGL::DrawCall>, kActiveShadowCascades> out{};
	std::vector<RGL::ShadowMeshletDrawArgs> mdiPush;
	std::vector<RGL::MdiShadowRow> rows;
	std::vector<RGL::MdiShadowSegment> segments;
	MdiShadowEmitContext ctx{
		.sliceRows = 12u, .slices = 2u, .frameSlot = 1u,
		.rows = &rows, .segments = &segments, .pushArgs = &mdiPush,
	};
	/// 按接缝前置把帧局部存储摆到位（resize-before-loop 是调用方契约，fixture 负责履约）。
	void prepare(uint32_t objectSlotCount) {
		old8.resize(static_cast<size_t>(kActiveShadowCascades) * objectSlotCount);
		meshlet12.resize(static_cast<size_t>(kActiveShadowCascades) * objectSlotCount);
		mdiPush.resize(kActiveShadowCascades);
	}
	void emit(std::span<const ShadowCandidate> cands, bool withCtx = true) {
		emitShadowCasters(cands, makeVolumes(), 10u, old8, meshlet12, out,
			withCtx ? &ctx : nullptr);
	}
};

template <typename T>
const T& peek(std::span<const std::byte> bytes) {
	REQUIRE(bytes.size() == sizeof(T));
	return *reinterpret_cast<const T*>(bytes.data());
}

/// 本级联第一条 drawCount>1 的条目下标（nullopt = 该级联没有 MDI draw）。
std::optional<size_t> firstMdi(const std::vector<RGL::DrawCall>& seg) {
	for (size_t i = 0; i < seg.size(); ++i)
		if (seg[i].drawCount > 1u)
			return i;
	return std::nullopt;
}

uint32_t countMdi(const std::vector<RGL::DrawCall>& seg) {
	uint32_t n = 0;
	for (const auto& cd : seg)
		if (cd.drawCount > 1u)
			++n;
	return n;
}

}  // namespace

TEST_CASE("★M-1b constexpr 桥：schema 生成常量 ↔ mdi_rows 真相源") {
	// 变异红线 M8：三处拼写任何一处漂移（schema 改了 shader 没改 / mdi_rows 加字段没改
	// schema）→ 这条直接红。期望侧 = offsetof/sizeof（编译器实查），不取任何被测运行时值。
	CHECK(kMdiShadowRowStrideBytes == kMdiShadowRowStride);
	CHECK(kMdiShadowRowStrideBytes == sizeof(MdiShadowRow));
	CHECK(kMdiShadowObjectIndexByteOffset == kObjOff);
	CHECK(kMdiShadowClusterStartByteOffset == kStartOff);
	CHECK(kMdiShadowRowStrideBytes == 24u);  // §19.2 D1 定案值（字面钉死，防「两边一起漂」）
	CHECK(kMdiShadowObjectIndexByteOffset == 12u);
	CHECK(kMdiShadowClusterStartByteOffset == 16u);
	// 命令本体与步长是两个数（VUID-07088/07090 算式里不可互换）
	CHECK(kMdiShadowCommandBytes == 12u);
	CHECK(kMdiShadowCommandBytes != kMdiShadowRowStride);
}

TEST_CASE("生效档：每级联恰一条 MDI DrawCall + 同源 pair 闭式字面") {
	// 变异红线 M2/M3/M4/M9：吸收判据、pair 字段、push word0、firstRow 累加各钉在这里。
	MdiFixture f;
	f.prepare(10u);
	const auto cands = makeCands();
	f.emit(std::span<const ShadowCandidate>{cands});

	REQUIRE(f.segments.size() == 3u);  // c1..c3，c0 恒旧
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c)
		REQUIRE(f.out[c].size() >= 1u);
	// 每级联的 MDI 条目**恰一条**（多插 = drawn 判据短路；不插 = 分家）
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c)
		CHECK(countMdi(f.out[c]) == 1u);
	CHECK(countMdi(f.out[0]) == 0u);  // 🔴 c0 恒旧路径（M1 的正面锚点）

	// 同源 pair = 写死字面（frameSlot 1 × sliceRows 12 + firstRow 0/3/6 → ×24）
	const uint32_t expRow[3] = {12u, 15u, 18u};
	const uint32_t expArgs[3] = {288u, 360u, 432u};
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c) {
		const auto idx = firstMdi(f.out[c]);
		REQUIRE(idx.has_value());
		const auto& call = f.out[c][*idx];
		const uint32_t i = c - 1u;
		CHECK(call.pipeline == kMdiEntry);
		CHECK(call.drawCount == 3u);            // 行数 = 可切候选数（含不可见行）
		CHECK(call.groupCount == 0u);           // MDI 下被忽略（置 0 = 没人误读旧字段）
		CHECK(call.argsOffset == expArgs[i]);   // pair.argsOffset（M3 的换位探测面）
		CHECK(static_cast<uint64_t>(call.argsOffset)
			== static_cast<uint64_t>(expRow[i]) * kMdiShadowRowStride);  // 同源不变量
		const auto& push = peek<RGL::ShadowMeshletDrawArgs>(call.pushData);
		CHECK(push.objectIndex == expRow[i]);   // word0 = rowBase（M4：写 objectIndex 就红）
		CHECK(push.cascadeIndex == c);
		CHECK(push.clusterStart == 0u);         // reserved
	}

	// 位置 = 首个**可见**可切候选处：A(slot 2) 是级联首位候选 ⟹ MDI 在段头；
	// N（簇可切无 MDI 宿主）与 D（全旧）继续逐条（混合形态）。
	const auto idx = firstMdi(f.out[1]);
	CHECK(*idx == 0u);
	CHECK(f.out[1][1].pipeline == kClusterEntry);  // N 的簇 draw 原样
	CHECK(f.out[1][2].pipeline == kOldEntry);      // D 的旧 8B draw 原样
	CHECK(f.out[1][1].drawCount == 1u);
	CHECK(f.out[1][2].drawCount == 1u);
	CHECK(peek<RGL::ShadowMeshletDrawArgs>(f.out[1][1].pushData).objectIndex == 3u);  // N 的槽
}

TEST_CASE("生效档：行内容 = 构建器闭式（可见 gcX、不可见 0 且身份字段照写）") {
	// 变异红线 M10：`in.visible` 恒真（剔除判据失效）→ c1/c2 的第三行必须红。
	MdiFixture f;
	f.prepare(10u);
	const auto cands = makeCands();
	f.emit(std::span<const ShadowCandidate>{cands});
	REQUIRE(f.rows.size() == 9u);  // 3 级联 × 3 可切候选（上界口径见回落组的容量用例）

	const uint32_t expGc[3][3] = {
		{8u, 2u, 0u},   // c1：A、B 可见；C(x=4.5) 不可见
		{8u, 2u, 0u},   // c2：同上
		{8u, 2u, 14u},  // c3：全可见（4.5 ≤ 4+0.5）
	};
	// 逐行独立重推可见性（期望值出处 = visClosed + 2×簇数），行序 = 候选序（A,B,C 的 slot）
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c)
		for (uint32_t k = 0u; k < 3u; ++k) {
			const auto& row = f.rows[(c - 1u) * 3u + k];
			const uint32_t slot = (k == 0u) ? 2u : (k == 1u ? 5u : 9u);
			const uint32_t clusters = (k == 0u) ? 4u : (k == 1u ? 1u : 7u);
			const float x = (k == 0u) ? 0.0f : (k == 1u ? 2.5f : 4.5f);
			CHECK(row.groupCountX == (visClosed(c, x, 0.5f) ? 2u * clusters : 0u));
			CHECK(row.groupCountX == expGc[c - 1u][k]);
			CHECK(row.groupCountY == 1u);
			CHECK(row.groupCountZ == 1u);
			CHECK(row.objectIndex == slot);      // 身份字段照写（M-0 口径：红了能定位是哪一行）
			CHECK(row.reserved0 == 0u);
		}
	// clusterStart 逐行对位（A=3、B=0、C=11 —— 不可见行也写，孔洞流语义）
	for (uint32_t c = 0u; c < 3u; ++c) {
		CHECK(f.rows[c * 3u + 0u].clusterStart == 3u);
		CHECK(f.rows[c * 3u + 1u].clusterStart == 0u);
		CHECK(f.rows[c * 3u + 2u].clusterStart == 11u);
	}
	// 段的 rows span 必须落进本帧 rows 存储（resize-before-loop：span 悬空 = 录制期 UAF）
	for (const auto& seg : f.segments) {
		REQUIRE(seg.rows.size() == 3u);
		CHECK(seg.rows.data() == f.rows.data() + seg.firstRow);
		CHECK(seg.frameSlot == 1u);
	}
}

TEST_CASE("生效档：行序 = 候选序（打乱输入必须跟着变，别只钉聚合量）") {
	MdiFixture f;
	f.prepare(10u);
	std::vector<ShadowCandidate> shuffled = {
		cand(5u, 2.5f, kOldEntry, kClusterEntry, kMdiEntry, 1u, 0u),   // B 先
		cand(2u, 0.0f, kOldEntry, kClusterEntry, kMdiEntry, 4u, 3u),   // A
		cand(9u, 4.5f, kOldEntry, kClusterEntry, kMdiEntry, 7u, 11u),  // C
	};
	f.emit(std::span<const ShadowCandidate>{shuffled});
	REQUIRE(f.rows.size() == 9u);
	for (uint32_t c = 0u; c < 3u; ++c) {
		CHECK(f.rows[c * 3u + 0u].objectIndex == 5u);   // 行 0 跟着候选序走
		CHECK(f.rows[c * 3u + 1u].objectIndex == 2u);
		CHECK(f.rows[c * 3u + 2u].objectIndex == 9u);
	}
	// MDI draw 位置 = 首个可见可切候选 = B（B 在 c1 可见）→ 仍在 A 之前（段头）
	REQUIRE(firstMdi(f.out[1]).has_value());
	CHECK(*firstMdi(f.out[1]) == 0u);
}

TEST_CASE("生效档：全不可见级联 = 孔洞流照发一条（段尾），gcX 全 0") {
	// 变异红线 M5：段尾补发循环摘掉 → 本用例的 drawCount>1 条目消失 = 红。
	MdiFixture f;
	f.prepare(10u);
	std::vector<ShadowCandidate> far = {
		cand(2u, 100.0f, kOldEntry, kClusterEntry, kMdiEntry, 4u, 3u),
		cand(5u, -100.0f, kOldEntry, kClusterEntry, kMdiEntry, 1u, 0u),
		cand(9u, 100.0f, kOldEntry, kClusterEntry, kMdiEntry, 7u, 11u),
	};
	f.emit(std::span<const ShadowCandidate>{far});
	REQUIRE(f.segments.size() == 3u);
	CHECK(f.rows.size() == 9u);
	for (const auto& row : f.rows)
		CHECK(row.groupCountX == 0u);  // 全空转行（身份字段照写，前面用例钉的是形态）
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c) {
		REQUIRE(countMdi(f.out[c]) == 1u);
		CHECK(*firstMdi(f.out[c]) == f.out[c].size() - 1u);  // 挂在**段尾**
		CHECK(f.out[c][*firstMdi(f.out[c])].drawCount == 3u);
	}
	CHECK(f.out[0].empty());  // 级联 0 无可见候选、也恒不走 MDI
}

TEST_CASE("回落组：五种不过的形态 = segments 清空 + 零 drawCount>1（不静默半切）") {
	// 各档共用：逐条形态必须原样存在（回落 ≠ 不画）。
	const auto legacyOk = [](auto& f) {
		CHECK(f.segments.empty());
		CHECK(f.rows.empty());
		for (uint32_t c = 1u; c < kActiveShadowCascades; ++c)
			CHECK(countMdi(f.out[c]) == 0u);
	};
	// ① 可切 < 2（M7）
	{
		MdiFixture f;
		f.prepare(10u);
		std::vector<ShadowCandidate> one = {
			cand(2u, 0.0f, kOldEntry, kClusterEntry, kMdiEntry, 4u, 3u),
			cand(4u, 0.4f, kOldEntry, kInvalidPipelineIndex, kInvalidPipelineIndex, 0u, 0u),
		};
		f.emit(std::span<const ShadowCandidate>{one});
		legacyOk(f);
		CHECK(f.out[1][0].pipeline == kClusterEntry);  // 唯一可切候选回到逐条簇 draw
		CHECK(f.out[1][0].groupCount == 8u);
	}
	// ② 条目不一致（全场景 1 条目不变式破 = 烘焙分家）
	{
		MdiFixture f;
		f.prepare(10u);
		std::vector<ShadowCandidate> split = {
			cand(2u, 0.0f, kOldEntry, kClusterEntry, kMdiEntry, 4u, 3u),
			cand(5u, 2.5f, kOldEntry, kClusterEntry, kMdiEntry + 1u, 1u, 0u),
		};
		f.emit(std::span<const ShadowCandidate>{split});
		legacyOk(f);
	}
	// ③ 容量不足：rows 3 × cascades 3 = 9 > sliceRows 8（M6 的容量半边）
	{
		MdiFixture f;
		f.ctx.sliceRows = 8u;
		f.prepare(10u);
		f.emit(std::span<const ShadowCandidate>{makeCands()});
		legacyOk(f);
	}
	// ④ 轮转前提：frameSlot 2 ≥ slices 2（雷 3 采集期预检）
	{
		MdiFixture f;
		f.ctx.frameSlot = 2u;
		f.prepare(10u);
		f.emit(std::span<const ShadowCandidate>{makeCands()});
		legacyOk(f);
	}
	// ⑤ pushArgs 没先 resize（span 悬空红线同族）
	{
		MdiFixture f;
		f.old8.resize(static_cast<size_t>(kActiveShadowCascades) * 10u);
		f.meshlet12.resize(static_cast<size_t>(kActiveShadowCascades) * 10u);
		f.mdiPush.resize(1u);  // 破契约：少于级联数
		f.emit(std::span<const ShadowCandidate>{makeCands()});
		legacyOk(f);
	}
	// ⑥ 反向对照（「没触发」与「坏掉了」之分）：同样候选，**不传 ctx** → 逐条原样、零段
	{
		MdiFixture f;
		f.prepare(10u);
		f.emit(std::span<const ShadowCandidate>{makeCands()}, false);
		legacyOk(f);
		uint32_t clusterDraws = 0;
		for (uint32_t c = 1u; c < kActiveShadowCascades; ++c)
			for (const auto& call : f.out[c])
				if (call.pipeline == kClusterEntry)
					++clusterDraws;
		CHECK(clusterDraws > 0u);  // A/B/C 逐条簇 draw 还在（门关 = 今天的形态）
	}
}

TEST_CASE("两帧轮转：frameSlot 0 的第二帧 pair 数字 = 闭式另一半") {
	// 同源 pair 的另一半（argsOffset 落在**偏移**不在句柄，§19.5）：slot 0 时
	// rowBase = 0×12+firstRow = 0/3/6、argsOffset = 0/72/144。
	MdiFixture f;
	f.ctx.frameSlot = 0u;
	f.prepare(10u);
	const auto cands = makeCands();
	f.emit(std::span<const ShadowCandidate>{cands});
	REQUIRE(f.segments.size() == 3u);
	const uint32_t expRow[3] = {0u, 3u, 6u};
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c) {
		const auto idx = firstMdi(f.out[c]);
		REQUIRE(idx.has_value());
		CHECK(f.out[c][*idx].argsOffset == expRow[c - 1u] * kMdiShadowRowStride);
		CHECK(peek<RGL::ShadowMeshletDrawArgs>(f.out[c][*idx].pushData).objectIndex == expRow[c - 1u]);
		CHECK(f.segments[c - 1u].loc.rowBase == expRow[c - 1u]);
		CHECK(f.segments[c - 1u].loc.argsOffset == expRow[c - 1u] * kMdiShadowRowStride);
		// 段的 pair 与 draw 的字段**同一次产出**：两处必须一字不差
		CHECK(f.out[c][*idx].argsOffset
			== static_cast<uint32_t>(f.segments[c - 1u].loc.argsOffset));
	}
}

}  // namespace RGL
