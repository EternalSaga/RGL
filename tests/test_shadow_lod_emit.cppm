module;
#include <doctest/doctest.h>
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <cmath>
#include <glm/glm.hpp>  // glm 值类型（rlstd 不导出 glm；与 test_render_list_shadow 同形，GMF 直接进）

/**
 * test_shadow_lod_emit —— 簇化轴 §18 批 B2：阴影段 c1..c3 的 draw 构造（collect/emit 接缝）。
 *
 * 被测对象 = `rgl/render_list_impl.cppm` 的 `collectShadowCasters` + `emitShadowCasters`
 * （★18-B2 抬为 render_list 导出接缝，12.3d `fillShadowData` 先例：接口声明 export、
 * impl partition 定义、测试直打）。
 *
 * ## 为什么值得测（画面对了也可能是假的那一类）
 * 三条红线全部是「截图看不出来」的形态：
 *   · **c0 恒旧路径**：IZB mask 重放（replayIzbMaskSegment）只重放 c0 段且按 8B push 读——
 *     c0 一旦切簇，坏的是「VVL 报 argsSize 不符」或「重放读错字节」，而截图可能完全正常；
 *   · **×2 唯一真相源**：task 数写成 `2 × 2 × 簇数` 只多发一倍空 task（shader 内早退），
 *     写成 `1 ×` 则**一半几何静默不画**，阴影图里少一半几何要眼力才看得出来；
 *   · **draw 条数不变**（原位换管线）：条数变了 = 候选序对账与 shadowBoundaries 全漂。
 * 加上「门关逐位不变」的构造性前提 = 门关时候选里根本没有簇路径字段，这条也只能在这里钉
 * （逐位截图只验最终像素）。
 *
 * ## 输入形态（不依赖 GPU / 不依赖真产物）
 * 光空间体手工搭：`view = identity`、t 范围 ±1e6、只让 `xyRadius` 逐级 = 1/2/3/4
 * ⟹ 「谁落进哪几级」有**闭式判据** `|x| ≤ xyRadius(c) + r`（本文件自己写死，**不调**
 * `casterInShadowVolume` 当 oracle——它是 scene_context 的既有函数、另有 test_shadow_geometry
 * 覆盖，这里再拿它当期望值出处 = 把被测码抄进期望）。
 * 三个候选的 x = 0 / 2.5 / 4.5、r = 0.5 ⟹ 逐级条数闭式 = **1 / 2 / 2 / 3**（合计 8 条）。
 *
 * ## 变异红线声明（每条"哪个变异让我红"；**全部实测** 2026-10-12，13/13 红）
 * 执行方式 = `/tmp/opencode/mutate_b2.py`（`cp` 语义用 `shutil.copyfile` **不带 mtime**、禁 git
 * checkout；每条显式 `cmake --build build --target rgl_tests` 并核对**构建自身**返回码；
 * 每条还原后重跑全量基线并断言全绿）。执行记录 = `/tmp/opencode/mutation_b2_results.txt`。
 * ⚠ 上面两个 `/tmp` 路径随重启已失——**留的是那段可复现配方**（缺了它，"13/13 红"就退化成
 * 一句无法重跑的自述），不是留指针。
 * 「helper 内 REQUIRE」= 失败点在 `peek` 的尺寸契约上（8B/12B 读错那份立刻红）。
 * 逐条写法 = 变异**改在源码上的那一行**（不是意译），照抄即可重放。
 *   ① drawAtlas 门短路（`(!plan.drawAtlas && false)`）→ 红 1 条（门关两态）
 *   ② doubleSided 判据短路（整项删掉）→ 红 1 条（四条判据）
 *   ③ 🔴 **弱化**：`!mat.doubleSided` 改 `!mat.alphaModeMask`（本地正确的另一半）→ 红 1 条
 *      （四条判据里「MASK 仍可切」那条反向断言）
 *   ④ `lodTable->bySlot[slot].count > 0` 弱化成恒真（`(count > 0) || true`）→ 红 1 条
 *      （四条判据：count==0 是合法「不切」不是错误）
 *   ⑤ `objectSlot < lodTable->bySlot.size()` 弱化成恒真 → 红 1 条（四条判据：表用 resize(3)
 *      留的**已知内容**越界探针读到 {41,9} ⟹ 不靠 UB 撞运气）
 *   ⑥ ×2 提前换算（collect 侧 `cand.meshletGroups = lod.count` 改成 `kSlotsPerCluster * lod.count`）
 *      → 红 1 条（四条判据的正例 `meshletGroups == 5`）——
 *      🔴 emit 侧用例**不红**（那里的候选是手搭的、不经 collect），故两侧各自钉一条是必须的
 *   ⑦ task 数少一半（emit 里 `meshletTaskCount(cd.meshletGroups)` 改裸 `cd.meshletGroups`）
 *      → 红 2 条（emit 主用例 + Lcg 属性）
 *   ⑧ c0 也切簇（`c >= 1u` 改 `c >= 0u`）→ 红 3 条 + helper REQUIRE（c0 行哨兵、push 尺寸）
 *   ⑨ 簇路径分支不 `continue`（先写 12B 再**也**写 8B 旧 draw）→ 红 3 条 + helper REQUIRE
 *      （逐级条数 1/2/2/3 被打破）
 *   ⑩ 索引丢分块（`c * objectSlotCount + cd.objectSlot` 改成 `cd.objectSlot`）→ 红 3 条
 *      （push 指向预期槽地址的指针身份断言）
 *   ⑪ clusterStart 丢传（`margs.clusterStart = cd.clusterStart` 写成 `0`）→ 红 2 条
 *      （12B push 内容 + Lcg 属性）
 *   ⑫ 不可切候选在 c>=1 被 `continue` 丢掉（忘回退旧路径）→ 红 3 条（切与不切条数逐条相等）
 *   ⑬ `margs.cascadeIndex = c` 写死 0 → 红 3 条（push 内容里的逐级递增 + 切与不切 + Lcg）
 *
 * ## 已知覆盖缺口（写出来，别让「全绿」被读成「全验过」）
 *   · `collectShadowCasters` 的 `drawAtlas` 门读 `shadowConfig()` 单例（形态决策表在实现内），
 *     本文件经 `setShadowConfig` 两态直打（先例 = test_render_list_shadow 的按档分立用例）
 *     ——**测得到两态，测不到「配置解析 → 该单例」那一段**（归 test_scene_config）。
 *   · `meshletTaskCount` 在 impl 匿名 namespace 内不可直连，本文件用**字面 2** 写期望并另钉
 *     `kSlotsPerCluster == 2`；契约的另一端（shader 里 `cluster = base + (groupID.x >> 1)`）
 *     不在单测射程内，归 §18.6 判据 2 的 L0 臂阳性对照。
 *   · 「阴影段总条数与改前逐条相等」在真场景上的读数属验收判据 5（profiler draw 数），
 *     本文件只钉**形状**（同一批候选，切与不切两次 emit 的逐级条数逐条相等）。
 *   · `RenderListState::shadowMeshletArgs` 的 resize-before-loop 红线是调用点契约
 *     （renderListSystem 内），无接缝可打；靠 §18.6 判据 1/2 的实机跑 + VVL 三档兜。
 */
export module test_shadow_lod_emit;

import rlstd;
import test_support;                  // Lcg（写死种子，禁真随机）
import render_list;                   // collectShadowCasters / emitShadowCasters / ShadowCandidate
import frame_draw_list;               // DrawCall / ShadowDrawArgs / ShadowMeshletDrawArgs
import scene_context;                 // ShadowVolume
import scene_data;                    // kActiveShadowCascades
import asset_table;                   // MeshAsset / MaterialAsset / kInvalidPipelineIndex
import shadow_config;                 // ShadowConfig / ShadowMode / setShadowConfig（门两态）
import meshlet_lod;                   // MeshletShadowLodTable
import meshlet_cull_types_generated;  // kSlotsPerCluster（×2 契约的 CPU 端规格常量）

namespace RGL {
namespace {

/** 未被 emit 写过的哨兵值：断言「这一行本帧不该被碰」的唯一手段（0 是合法值，挡不住）。 */
constexpr uint32_t kSentinel = 0x5A5A5A5Au;

ShadowDrawArgs sentinelOld() {
	return ShadowDrawArgs{kSentinel, kSentinel};  // 两字段全填哨兵，任一被写都看得见
}
ShadowMeshletDrawArgs sentinelNew() {
	return ShadowMeshletDrawArgs{kSentinel, kSentinel, kSentinel};
}

/**
 * 造 4 个光空间体：view = identity、t 范围 ±1e6，只让 xyRadius 逐级 = 1/2/3/4。
 * ⟹ 剔除退化成闭式 `|x| ≤ 1 + c + r`（本文件期望值的出处，见文件头）。
 */
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

/** 闭式「候选 center.x 是否落进第 c 级」（期望值出处，不调既有剔除函数）。 */
bool inCascadeClosedForm(uint32_t c, float x, float r) {
	return std::abs(x) <= 1.0f + static_cast<float>(c) + r;
}

/** 造一个「旧路径会出阴影 draw」的资产（shadowPipelineIndex 有效 + 非 blend 是两条前置守卫）。 */
MeshAsset makeAsset(uint32_t shadowPipeline, uint32_t groups, uint32_t shadowMeshletPipeline) {
	MeshAsset a;
	a.shadowPipelineIndex = shadowPipeline;
	a.groupCount = groups;
	a.shadowMeshletPipelineIndex = shadowMeshletPipeline;
	return a;
}

/**
 * 读回 push 字节。emit 存的是**帧局部数组元素的 span**（指回 shadowArgs / shadowMeshletArgs），
 * 故尺寸先 REQUIRE 再还原——尺寸本身也是契约（8B/12B 与烘焙期 argsSize 同源）。
 */
template <typename T>
const T& peek(std::span<const std::byte> bytes) {
	REQUIRE(bytes.size() == sizeof(T));
	return *reinterpret_cast<const T*>(bytes.data());
}

/** 把一条候选的期望形态摆成表（emit 之后逐条比对，避免期望值散在断言里）。 */
struct Expectation {
	uint32_t slot;
	uint32_t oldPipeline;
	uint32_t oldGroups;
	uint32_t newPipeline;  ///< kInvalidPipelineIndex = 不可切
	uint32_t clusters;     ///< 选级簇数（= ShadowCandidate::meshletGroups）
	uint32_t start;        ///< clusterStart
};

/** 表 → 候选向量（x 由调用方给的数组按序取，r 恒 0.5）。 */
std::vector<ShadowCandidate> makeCands(const std::vector<Expectation>& exp,
	const std::vector<float>& xs) {
	std::vector<ShadowCandidate> cands;
	for (size_t i = 0; i < exp.size(); ++i) {
		ShadowCandidate c;
		c.center = glm::vec3{xs[i], 0.0f, 0.0f};
		c.radius = 0.5f;
		c.objectSlot = exp[i].slot;
		c.pipeline = exp[i].oldPipeline;
		c.groups = exp[i].oldGroups;
		c.meshletPipeline = exp[i].newPipeline;
		c.meshletGroups = exp[i].clusters;
		c.clusterStart = exp[i].start;
		cands.push_back(c);
	}
	return cands;
}

/** 门开（drawAtlas = true）——collectShadowCasters 读的是 shadowConfig() 单例。 */
void enableShadowAtlas() {
	ShadowConfig on;
	on.mode = ShadowMode::csm;  // 出厂默认 = 交付形态
	setShadowConfig(on);
}

}  // namespace

TEST_CASE("collectShadowCasters：阴影总开关两态（门关零候选 = 逐位不变的构造性前提）") {
	// 变异红线 ①：把 `!shadowFormPlan(...).drawAtlas` 短路 → 本用例红（关态也攒候选）。
	const auto asset = makeAsset(/*shadowPipeline=*/7, /*groups=*/13, kInvalidPipelineIndex);
	const MaterialAsset mat;
	std::vector<ShadowCandidate> out;

	ShadowConfig off;
	off.mode = ShadowMode::none;  // → drawAtlas = false
	setShadowConfig(off);
	collectShadowCasters(asset, mat, glm::vec3{0.0f}, 0.5f, /*objectSlot=*/0, nullptr, out);
	CHECK(out.empty());  // 反向用例：关态必须零候选（否则阴影 pass 关不掉）

	enableShadowAtlas();
	collectShadowCasters(asset, mat, glm::vec3{1.5f, 2.0f, 3.0f}, 0.5f, /*objectSlot=*/4, nullptr,
		out);
	REQUIRE(out.size() == 1);
	// 门开 = 五个标量**原样**搬运（错位/漏字段在这里红，不在像素上红）。
	CHECK(out[0].center == glm::vec3(1.5f, 2.0f, 3.0f));
	CHECK(out[0].radius == 0.5f);
	CHECK(out[0].objectSlot == 4u);
	CHECK(out[0].pipeline == 7u);
	CHECK(out[0].groups == 13u);
	// 门关（lodTable = nullptr）时簇字段必须保持「不可切」形态（§18.6 判据 1 的验收档）。
	CHECK(out[0].meshletPipeline == kInvalidPipelineIndex);
	CHECK(out[0].meshletGroups == 0u);
	CHECK(out[0].clusterStart == 0u);

	setShadowConfig(ShadowConfig{});  // 还原进程级单例（谁改谁收尾）
}

TEST_CASE("collectShadowCasters：两条既有守卫（无效阴影管线 / blend）各配反向用例") {
	// 这两条守卫是**改前就有**的（collect 的第一道闸），本批补上反向用例——之前无测试：
	// 把 `asset.shadowPipelineIndex == kInvalid` 判据删掉，旧形态也只是攒进一条
	// pipeline = 0xFFFFFFFF 的候选，到执行器才炸成越界管线号。
	const MaterialAsset mat;
	std::vector<ShadowCandidate> out;
	enableShadowAtlas();  // 前置：门必须开，否则三条用例全绿是「门关」给的 = 假绿

	// ① 无阴影管线的资产 → 不攒
	collectShadowCasters(makeAsset(kInvalidPipelineIndex, 3u, 99u), mat, glm::vec3{0.0f}, 0.5f, 0u,
		nullptr, out);
	CHECK(out.empty());

	// ② blend 材质 → 不攒（透明体不投影；blend 守卫在簇判据**之前**，簇路径同样不切）
	MaterialAsset blend;
	blend.alphaModeBlend = true;
	collectShadowCasters(makeAsset(7u, 3u, 99u), blend, glm::vec3{0.0f}, 0.5f, 0u, nullptr, out);
	CHECK(out.empty());

	// 阳性对照：同一资产换回不透明材质就攒（挡住「反向用例其实是被恒假判据喂绿的」）
	collectShadowCasters(makeAsset(7u, 3u, 99u), mat, glm::vec3{0.0f}, 0.5f, 0u, nullptr, out);
	CHECK(out.size() == 1);

	setShadowConfig(ShadowConfig{});
}

TEST_CASE("collectShadowCasters：可切四条判据逐条反向 + 正例钉「存簇数不存 task 数」") {
	// 变异红线 ②③④⑤⑥ 在此。
	enableShadowAtlas();
	MeshletShadowLodTable table;
	// 前 3 条 = 可见表（槽 0 可切 / 槽 1 count==0 / 槽 2 可切）。第 4 条是**越界探针**：
	// resize(3) 之后 size == 3 而容量不变、第 4 条的字节仍在同一次分配内 ⟹ 「边界判据被删/弱化」
	// 的变异会**确定性**读到 {41,9}（count>0 ⟹ 切成簇路径）而不会读到不确定的堆垃圾（假绿），
	// 也不会越出分配（不靠 UB 撞运气）。
	table.bySlot = {{11u, 5u}, {0u, 0u}, {31u, 7u}, {41u, 9u}};
	table.bySlot.resize(3);

	const auto switchableAsset = makeAsset(7u, 13u, /*shadowMeshletPipeline=*/42u);
	const MaterialAsset mat;

	auto collectOne = [&](const MeshAsset& a, const MaterialAsset& m, uint32_t slot,
							const MeshletShadowLodTable* t) {
		std::vector<ShadowCandidate> out;
		collectShadowCasters(a, m, glm::vec3{0.0f}, 0.5f, slot, t, out);
		REQUIRE(out.size() == 1);  // 前置：候选必须产出，否则「不可切」是空表给的假绿
		return out[0];
	};

	// 正例（四条全满足）：🔴 meshletGroups == **簇数**（5），不是 task 数（10）——
	// ×2 换算的唯一真相源在 emit（变异红线 ⑥：在这里提前换算 → 本断言红）。
	const auto good = collectOne(switchableAsset, mat, 0u, &table);
	CHECK(good.meshletPipeline == 42u);
	CHECK(good.meshletGroups == 5u);
	CHECK(good.clusterStart == 11u);

	// 判据 1：lodTable == nullptr（门关 / 设备规格不够 / 对账失败 = 装配层根本不建）
	CHECK(collectOne(switchableAsset, mat, 0u, nullptr).meshletPipeline == kInvalidPipelineIndex);
	// 判据 4：count == 0 是**合法的不切**（与 ClusterRangeTable 全 0 条目同形）
	const auto zeroCount = collectOne(switchableAsset, mat, 1u, &table);
	CHECK(zeroCount.meshletPipeline == kInvalidPipelineIndex);
	CHECK(zeroCount.meshletGroups == 0u);
	// 判据 4 的另一半：槽越界（表比实例矩阵槽短）不得越界读——探针见上方 resize(3)
	const auto oob = collectOne(switchableAsset, mat, 3u, &table);
	CHECK(oob.meshletPipeline == kInvalidPipelineIndex);
	CHECK(oob.meshletGroups == 0u);
	CHECK(oob.clusterStart == 0u);
	// 判据 2：资产没有阴影簇管线条目（无产物 / 本档关 / 多产物回落）
	CHECK(collectOne(makeAsset(7u, 13u, kInvalidPipelineIndex), mat, 0u, &table)
	          .meshletPipeline == kInvalidPipelineIndex);
	// 判据 3：doubleSided（🔴 正确性闸门：产物里没有 loader 的背面副本，§16.2）
	MaterialAsset twoSided;
	twoSided.doubleSided = true;
	const auto ds = collectOne(switchableAsset, twoSided, 0u, &table);
	CHECK(ds.meshletPipeline == kInvalidPipelineIndex);
	CHECK(ds.meshletGroups == 0u);
	// MASK **不在**判据里（背面本就被 alpha 消掉，与 β-1 同源）——反向钉「别把 doubleSided
	// 判据顺手扩到 MASK」：同一张表下 MASK 仍可切（变异红线 ③ 的输入形态）。
	MaterialAsset mask;
	mask.alphaModeMask = true;
	CHECK(collectOne(switchableAsset, mask, 2u, &table).meshletPipeline == 42u);

	setShadowConfig(ShadowConfig{});
}

TEST_CASE("emitShadowCasters：c0 恒旧路径 + c1..c3 簇路径 + 逐级条数闭式 1/2/2/3") {
	// 变异红线 ⑦⑧⑨⑩⑪⑬ 在此。
	enableShadowAtlas();
	const auto vols = makeVolumes();
	const uint32_t slots = 4u;  // objectSlotCount（分块 stride）
	// 三条候选：槽 0 可切（全 4 级）、槽 1 可切（c1..c3）、槽 2 不可切（只 c3）
	const std::vector<Expectation> exp{{0u, 7u, 13u, 42u, 5u, 11u},
		{1u, 8u, 3u, 43u, 9u, 21u}, {2u, 9u, 4u, kInvalidPipelineIndex, 0u, 0u}};
	const std::vector<float> xs{0.0f, 2.5f, 4.5f};
	const auto cands = makeCands(exp, xs);

	std::vector<ShadowDrawArgs> oldArgs(static_cast<size_t>(kActiveShadowCascades) * slots,
		sentinelOld());
	std::vector<ShadowMeshletDrawArgs> newArgs(static_cast<size_t>(kActiveShadowCascades) * slots,
		sentinelNew());
	std::array<std::vector<DrawCall>, kActiveShadowCascades> out;
	emitShadowCasters(cands, std::span<const ShadowVolume>{vols.data(), vols.size()}, slots, oldArgs,
		newArgs, out);

	// ① 逐级条数 = 闭式（1/2/2/3，合计 8）。变异红线 ⑨（簇路径之外**也**写一条旧 draw）在此红。
	const std::array<uint32_t, kActiveShadowCascades> kExpectCounts{1u, 2u, 2u, 3u};
	uint32_t closedTotal = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		uint32_t closed = 0;
		for (size_t i = 0; i < exp.size(); ++i)
			closed += inCascadeClosedForm(c, xs[i], 0.5f) ? 1u : 0u;
		CHECK(out[c].size() == kExpectCounts[c]);  // 写死的第二判据：闭式与表同时被改也拦得住
		CHECK(out[c].size() == closed);
		closedTotal += closed;
	}
	CHECK(closedTotal == 8u);

	// ② 逐条内容：管线号 / task 数 / push 尺寸 / push 指向的槽地址 / 三个字段
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		size_t inCascade = 0;
		for (size_t i = 0; i < exp.size(); ++i) {
			if (!inCascadeClosedForm(c, xs[i], 0.5f))
				continue;
			REQUIRE(inCascade < out[c].size());
			const DrawCall& dc = out[c][inCascade++];
			const Expectation& e = exp[i];
			const size_t idx = static_cast<size_t>(c) * slots + e.slot;
			const bool useNew = c >= 1u && e.newPipeline != kInvalidPipelineIndex;
			if (useNew) {
				// 12B push + 簇管线 + task 数 = **字面 2** × 簇数（不取 meshletTaskCount；
				// 变异红线 ⑦：少乘一半在此红）
				CHECK(dc.pipeline == e.newPipeline);
				CHECK(dc.groupCount == 2u * e.clusters);
				const auto& a = peek<ShadowMeshletDrawArgs>(dc.pushData);
				CHECK(a.objectIndex == e.slot);
				CHECK(a.cascadeIndex == c);      // 变异红线 ⑬
				CHECK(a.clusterStart == e.start);  // 变异红线 ⑪
				// 🔴 索引公式 = 分块号 × objectSlotCount + slot（变异红线 ⑩：丢分块 → 跨级同槽
				// 互相覆盖，本断言红）
				CHECK(dc.pushData.data() == reinterpret_cast<const std::byte*>(&newArgs[idx]));
				// 簇路径那条**不写**旧数组（变异红线 ⑨ 的另一半）
				CHECK(oldArgs[idx].objectIndex == kSentinel);
			}
			else {
				CHECK(dc.pipeline == e.oldPipeline);
				CHECK(dc.groupCount == e.oldGroups);
				const auto& a = peek<ShadowDrawArgs>(dc.pushData);
				CHECK(a.objectIndex == e.slot);
				CHECK(a.cascadeIndex == c);
				CHECK(dc.pushData.data() == reinterpret_cast<const std::byte*>(&oldArgs[idx]));
				// 🔴 c0 恒旧路径的另一半：c0 那一行**绝不**被写进 12B 数组
				// （变异红线 ⑧：`c >= 1u` 放宽成 `c >= 0u` → 本断言红）
				if (c == 0u)
					CHECK(newArgs[idx].objectIndex == kSentinel);
			}
		}
		CHECK(inCascade == out[c].size());  // 前置守卫：闭式与实到的条数一致（防空集合假绿）
	}

	setShadowConfig(ShadowConfig{});
}

TEST_CASE("emitShadowCasters：切与不切的 draw 条数逐条相等（原位换管线，不增减）") {
	// 变异红线 ⑫：不可切候选在 c>=1 被 continue 丢掉 → 本用例红（条数不等）。
	enableShadowAtlas();
	const auto vols = makeVolumes();
	const uint32_t slots = 3u;
	const std::vector<float> xs{0.0f, 2.5f, 4.5f};

	auto build = [&](bool switchable) {
		std::vector<Expectation> exp;
		for (size_t i = 0; i < xs.size(); ++i) {
			const uint32_t s = static_cast<uint32_t>(i);
			exp.push_back(Expectation{.slot = s,
				.oldPipeline = 7u + s,
				.oldGroups = 3u + s,
				.newPipeline = switchable ? 42u + s : kInvalidPipelineIndex,
				.clusters = switchable ? 5u + s : 0u,
				.start = switchable ? 11u + s : 0u});
		}
		return exp;
	};

	std::vector<ShadowDrawArgs> oldArgs(static_cast<size_t>(kActiveShadowCascades) * slots);
	std::vector<ShadowMeshletDrawArgs> newArgs(static_cast<size_t>(kActiveShadowCascades) * slots);
	std::array<std::vector<DrawCall>, kActiveShadowCascades> off{};
	std::array<std::vector<DrawCall>, kActiveShadowCascades> on2{};
	const auto candsOff = makeCands(build(false), xs);
	const auto candsOn = makeCands(build(true), xs);
	emitShadowCasters(candsOff, std::span<const ShadowVolume>{vols.data(), vols.size()}, slots,
		oldArgs, newArgs, off);
	emitShadowCasters(candsOn, std::span<const ShadowVolume>{vols.data(), vols.size()}, slots,
		oldArgs, newArgs, on2);

	uint32_t total = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		// 条数逐条相等 + 段内序（objectIndex / cascadeIndex 序列）逐条相等 = 「原位换管线」形状。
		CHECK(off[c].size() == on2[c].size());
		REQUIRE(!off[c].empty());  // 前置守卫：空段会让上面那条恒真
		for (size_t i = 0; i < off[c].size(); ++i) {
			CHECK(off[c][i].pushData.size() == sizeof(ShadowDrawArgs));
			const bool newPush = c >= 1u;
			CHECK(on2[c][i].pushData.size()
				== (newPush ? sizeof(ShadowMeshletDrawArgs) : sizeof(ShadowDrawArgs)));
			const auto& a = peek<ShadowDrawArgs>(off[c][i].pushData);
			// 12B 那份的前两个字段与 8B 那份**同序同宽**（B1 的扩法：原两字段原样 + 追加
			// clusterStart），故这里按各自类型读前两个字段比对，不做类型双关。
			if (newPush) {
				const auto& b = peek<ShadowMeshletDrawArgs>(on2[c][i].pushData);
				CHECK(a.objectIndex == b.objectIndex);
				CHECK(a.cascadeIndex == b.cascadeIndex);
			}
			else {
				const auto& b = peek<ShadowDrawArgs>(on2[c][i].pushData);
				CHECK(a.objectIndex == b.objectIndex);
				CHECK(a.cascadeIndex == b.cascadeIndex);
			}
		}
		total += static_cast<uint32_t>(on2[c].size());
	}
	CHECK(total == 8u);  // 与上一条用例同一形态的总数（闭式 1/2/2/3）

	setShadowConfig(ShadowConfig{});
}

TEST_CASE("规格常量：kSlotsPerCluster == 2 + 两份 push 的 8B/12B（×2 与 argsSize 的锚）") {
	// 为什么单独一条：emit 的 task 数期望在上一条用例里写死**字面 2**，而 meshletTaskCount
	// 乘的是这个规格常量。若有人把 schema 里的 kSlotsPerCluster 改成 1/4 而没同步 shader 的
	// `cluster = base + (groupID.x >> 1)`，本条红（且不改测试就红）。
	CHECK(kSlotsPerCluster == 2u);
	// 12B 那份是 B1 新增；改了 = 烘焙期 argsSize 与实际 push 字节数不符（VVL 报错或读脏字节）
	CHECK(sizeof(ShadowDrawArgs) == 8u);
	CHECK(sizeof(ShadowMeshletDrawArgs) == 12u);
}

TEST_CASE("collect+emit 属性（Lcg 写死种子）：随机槽位/簇数/可切性下的六项不变量") {
	// 种子写死（禁真随机）。覆盖 hand-picked 用例覆盖不到的组合：x 跨各级边界、可切性随机、
	// 簇数与槽号互质分布。
	// 🔴 objectSlot **必须逐候选唯一**（= 引擎事实：一个 MeshRef.objectSlot 一个实体）——两条
	// 候选共用同槽时，两条 draw 的 push 指向**同一行**（后写的 clusterStart 覆盖前者），
	// 那是 push 存储按槽分块的既有语义，不是本批引入的形态，故不在这里造。
	enableShadowAtlas();
	const auto vols = makeVolumes();
	const uint32_t slots = 24u;
	testing::Lcg rng(20261012u);

	std::vector<ShadowCandidate> cands;
	std::vector<float> xs;
	std::vector<uint32_t> newPipelines;
	std::vector<uint32_t> clusters;
	std::vector<uint32_t> starts;
	std::vector<uint32_t> slotOf;
	for (int i = 0; i < 24; ++i) {
		ShadowCandidate c;
		const float x = static_cast<float>(i % 9) - 4.0f;  // [-4, 4]，跨各级边界
		const bool switchable = rng.uniform01() < 0.6;
		c.center = glm::vec3{x, 0.0f, 0.0f};
		c.radius = 0.5f;
		c.objectSlot = static_cast<uint32_t>(i) % slots;
		c.pipeline = 100u + static_cast<uint32_t>(i);
		c.groups = 1u + static_cast<uint32_t>(i);
		c.meshletPipeline = switchable ? 500u + static_cast<uint32_t>(i) : kInvalidPipelineIndex;
		c.meshletGroups = switchable ? 1u + static_cast<uint32_t>(i % 5) : 0u;
		c.clusterStart = switchable ? 7u * static_cast<uint32_t>(i) : 0u;
		cands.push_back(c);
		xs.push_back(x);
		newPipelines.push_back(c.meshletPipeline);
		clusters.push_back(c.meshletGroups);
		starts.push_back(c.clusterStart);
		slotOf.push_back(c.objectSlot);
	}

	std::vector<ShadowDrawArgs> oldArgs(static_cast<size_t>(kActiveShadowCascades) * slots,
		sentinelOld());
	std::vector<ShadowMeshletDrawArgs> newArgs(static_cast<size_t>(kActiveShadowCascades) * slots,
		sentinelNew());
	std::array<std::vector<DrawCall>, kActiveShadowCascades> out{};
	emitShadowCasters(cands, std::span<const ShadowVolume>{vols.data(), vols.size()}, slots, oldArgs,
		newArgs, out);

	uint32_t total = 0;
	uint32_t newDraws = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		size_t seen = 0;
		for (size_t i = 0; i < cands.size(); ++i) {
			if (!inCascadeClosedForm(c, xs[i], 0.5f))
				continue;
			REQUIRE(seen < out[c].size());
			const DrawCall& dc = out[c][seen++];
			const bool useNew = c >= 1u && newPipelines[i] != kInvalidPipelineIndex;
			const size_t idx = static_cast<size_t>(c) * slots + slotOf[i];
			if (useNew) {
				++newDraws;
				CHECK(dc.pipeline == newPipelines[i]);
				CHECK(dc.groupCount == 2u * clusters[i]);  // 字面 2（不取被测函数）
				CHECK(dc.groupCount > 0u);                 // 可切 ⟹ 至少 2 个 task
				const auto& a = peek<ShadowMeshletDrawArgs>(dc.pushData);
				CHECK(a.objectIndex == slotOf[i]);
				CHECK(a.cascadeIndex == c);
				CHECK(a.clusterStart == starts[i]);
				CHECK(dc.pushData.data() == reinterpret_cast<const std::byte*>(&newArgs[idx]));
			}
			else {
				CHECK(dc.pipeline == cands[i].pipeline);
				CHECK(dc.groupCount == cands[i].groups);
				const auto& a = peek<ShadowDrawArgs>(dc.pushData);
				CHECK(a.objectIndex == slotOf[i]);
				CHECK(a.cascadeIndex == c);
				CHECK(dc.pushData.data() == reinterpret_cast<const std::byte*>(&oldArgs[idx]));
				if (c == 0u)
					CHECK(newArgs[idx].objectIndex == kSentinel);  // c0 恒旧路径
			}
		}
		CHECK(seen == out[c].size());  // 闭式与实到条数一致（防「少发不红」）
		total += static_cast<uint32_t>(out[c].size());
	}
	CHECK(total > 0u);      // 前置守卫：一条 draw 都没有 = 全绿也是假绿
	CHECK(newDraws > 0u);   // 前置守卫：簇路径一条都没走到 = 上面一半断言是空转
	CHECK(total > newDraws);  // 至少也走到过旧路径（否则「旧路径」那半边没测）

	setShadowConfig(ShadowConfig{});
}

}  // namespace RGL
