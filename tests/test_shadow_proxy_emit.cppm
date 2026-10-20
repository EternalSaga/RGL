module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>       // glm::mat4 / vec3（搭光空间体与候选中心；GMF 只放第三方头）
#include <initializer_list>  // modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）

/**
 * test_shadow_proxy_emit —— shadow proxy 批次 N10 的 **P3**：钉段 draw 的采集与发射（CPU 侧）。
 *
 * 被测对象 = `render_list:impl` 的 `collectShadowCasters`（钉段四字段的填充）+
 * `render_list:mdi` 的 `emitShadowCasters` / `emitPinnedShadowDraw`（第二条 draw）+
 * `render_list:impl` 的 `shadowTexelSize`（三处消费者共用的那条纹素式子）。
 *
 * ## 这批为什么是「第一次变画面」，以及本文件钉的是哪一半
 * P2 把钉段表建好放进 ctx 但零消费者；P3 接上消费点 = 含钉段的候选在 **c1..c3 各多发一条
 * 选级恒 0 的 draw**。像素那一半归 imgqa（`shadow-caster-survival` 等三条）与四臂 GPU 实测；
 * **形状**这一半（谁多发、发几条、push 里装什么、门关时一条都不多）只能在这里钉 —— 截图
 * 看不出"多发的这条到底是不是钉段那一段"（它画的簇是常规段的子集，depth-only 图集下甚至
 * 大概率不改变任何纹素）。
 *
 * ## 🔴 draws 上限是 3 段而不是 4 段（c0 恒旧路径）
 * 任务书那条「13 prim × 3 = 39 而不是 52」的机器形态就在正向用例里：c0 那一条 draw 走的是
 * 旧管线（8B push，IZB mask 重放按 8B 读它），钉段与它**无关**。写死成"逐级都多一条"会让
 * c0 也多发 = 白付 1/4 的 draws 且把 IZB 重放的 push 尺寸契约踩坏。
 *
 * ## 输入形态（不依赖 GPU / 不依赖真产物）
 * 与 `test_shadow_lod_emit` 同法：光空间体手工搭（view = identity、t 范围 ±1e6、只让
 * `xyRadius` 逐级 = 1/2/3/4）。texel 因此有闭式 `xyRadius·2/1024`（分母 = 图集**单块**边长，
 * 手抄自 `render_graph` 的 `kShadowMapSize=2048` 与 `shadowAtlasColumns(4)=2`，
 * 🔴 不取被测函数当期望值出处）。钉段半径取 1.0（世界米，直径 2.0）与 1e-4（直径 2e-4）
 * 两档，把「过门 / 不过门」拉开到与 1024 无关也不会翻的余量 —— 但**边界本身**由
 * `test_shadow_proxy_types` 的闭式翻转点用例钉（那里是判据本体），这里只钉接线。
 *
 * ## 已知不可达 / 缺口（写出来，别让「全绿」被读成「全验过」）
 *   · `emitShadowCasters` 的 `pinnedArgs` 与 `meshletArgs` **必须是两块存储**这条，本文件用
 *     指针身份断言钉（钉段那条的 pushData 必须指进 pinnedArgs）。但「同一 (级联, 槽) 两条
 *     draw 共用一块会互相覆盖」这个失效形态，指针断言只挡住了**当前**布局；真正的护栏是
 *     `resizeFrameStorage` 里那两行相邻的 resize（调用点契约，无接缝可打）。
 *   · MDI 生效档的钉段行（行照发、texel 门只决定 gcX）归 `test_mdi_shadow_emit`（那里有
 *     行表 fixture，本文件不复制一份第二套 fixture）。
 *   · 「启动期读数表的活跃条目数」与「运行期实际多发的 draw 数」相等，需要真产物 + 真相机，
 *     属验收判据（四臂实测那节），不在单测射程内。
 *
 * ## 变异红线声明（每条"哪个变异让我红"；**七条全部实测** 2026-10-20，7/7 有红）
 * 执行方式 = `/tmp/opencode/n10p3/mutate{,3}.py`（配方留这里，别只留指针：`shutil.copyfile`
 * 备份/还原不带 mtime 坑、禁 git checkout、每条显式 `cmake --build build --target rgl_tests`
 * 并核对**构建自身**返回码、全部还原后重跑全量断言全绿）。执行记录同目录 `mutation_results*.txt`
 * （⚠ `/tmp` 随重启即失，下面逐条写的是**改在源码上的那一行**，照抄即可重放）。
 *   ① `pinActive(pin, worldScale, texel)` 弱化成只看 `count`（丢掉纹素那一维）
 *      → 红 2 条：「texel 门不过 → 一条都不多发」「texel 门与 emit 的口径一致」
 *   ② 钉段 task 数 `meshletTaskCount(cd.pinnedCount)` 改成裸 `cd.pinnedCount` → 红正向用例
 *   ③ 钉段 push 的 `clusterStart = cd.pinnedStart` 改成 `cd.clusterStart`（抄错字段）
 *      → 红正向用例 + 指针身份那条
 *   ④ 调用点实参 `shadowMeshletPinnedArgs` 换成 `shadowMeshletArgs`（两条 draw 共用 idx）
 *      → 红正向用例 + 指针身份那条
 *   ⑤ collect 侧把钉段填充整块**挪出**可切判据那个 `if`（拆掉宿主嵌套）→ 红「宿主不成立」那条
 *   ⑥ `c >= 1u` 改 `c >= 0u`（c0 也发钉段）→ 红 5 条（本文件 2 条 + `test_shadow_lod_emit` 3 条）
 *   ⑦ `shadowTexelSize` 分母 `shadowTileSize(shadowAtlasColumns(4))` 改成 `kShadowMapSize`
 *      → 红 2 条（本文件接缝闭式 + 既有 `fillShadowData` 那条，同一条式子两处守）
 * ⚠ 两条**踩坑记录**（下次写变异别再这样）：① 的初版是 `if (false) return;`、④ 的初版只换实参
 * —— 两版都**编译不过**（`-Werror,-Wunused-*`），而"编不过"不等于"测试守得住"。本批的形态是把
 * 判据**弱化**到仍在使用所有操作数、并给 ④ 补 `(void)`，先让它编得过，再问测试红不红。
 */
export module test_shadow_proxy_emit;

import rlstd;
import render_list;   // collectShadowCasters / emitShadowCasters / ShadowCandidate / shadowTexelSize
import frame_draw_list;  // DrawCall / ShadowDrawArgs / ShadowMeshletDrawArgs
import scene_context;    // ShadowVolume
import scene_data;       // kActiveShadowCascades
import asset_table;      // MeshAsset / MaterialAsset / kInvalidPipelineIndex
import shadow_config;    // ShadowConfig / ShadowMode / setShadowConfig（drawAtlas 门两态）
import meshlet_lod;      // MeshletShadowLodTable
import shadow_proxy_types;  // ShadowProxyTable / pinActive / ShadowProxyTable::Entry
import meshlet_cull_types_generated;  // kSlotsPerCluster（×2 契约的 CPU 端规格常量）

namespace RGL {
namespace {

/** 未被 emit 写过的哨兵值（0 是合法值，挡不住"这一行本帧不该被碰"）。 */
constexpr uint32_t kSentinel = 0x5A5A5A5Au;
/** 图集**单块**边长（手抄自 render_graph：kShadowMapSize 2048 / shadowAtlasColumns(4) = 2）。 */
constexpr float kTile = 1024.0f;

ShadowDrawArgs sentinelOld() {
	return ShadowDrawArgs { kSentinel, kSentinel };
}
ShadowMeshletDrawArgs sentinelNew() {
	return ShadowMeshletDrawArgs { kSentinel, kSentinel, kSentinel };
}

/** 4 个光空间体：只让 xyRadius 逐级 = 1/2/3/4（其余取恒真值，剔除退化成 |x| ≤ 1+c+r）。 */
std::array<ShadowVolume, kActiveShadowCascades> makeVolumes() {
	std::array<ShadowVolume, kActiveShadowCascades> vols {};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		vols[c].viewProj = glm::mat4(1.0f);
		vols[c].view = glm::mat4(1.0f);
		vols[c].xyRadius = 1.0f + static_cast<float>(c);
		vols[c].tMin = -1.0e6f;
		vols[c].tMax = 1.0e6f;
	}
	return vols;
}

/** 第 c 级的纹素尺寸（期望值出处 = 手抄的 kTile，不调 shadowTexelSize）。 */
float texelClosed(uint32_t c) {
	return (1.0f + static_cast<float>(c)) * 2.0f / kTile;
}

template <typename T>
const T& peek(std::span<const std::byte> bytes) {
	REQUIRE(bytes.size() == sizeof(T));
	return *reinterpret_cast<const T*>(bytes.data());
}

/**
 * 一条**可切**候选（c1..c3 走簇管线），钉段参数由入参给。
 * 中心 x=0、r=0.5 ⟹ 四级联全可见（闭式 |0| ≤ 1+c+0.5 恒成立），把"可见性"这个变量摘干净，
 * 本文件只测钉段这一维。
 */
ShadowCandidate makeSwitchable(uint32_t slot, uint32_t pinnedStart, uint32_t pinnedCount,
	float pinnedRadius, float worldScale) {
	ShadowCandidate cd;
	cd.center = glm::vec3 { 0.0f, 0.0f, 0.0f };
	cd.radius = 0.5f;
	cd.objectSlot = slot;
	cd.pipeline = 7u;      // 旧管线（c0 恒用它）
	cd.groups = 9u;
	cd.meshletPipeline = 21u;  // 可切
	cd.meshletGroups = 5u;     // 选级簇数（主条 task = ×2）
	cd.clusterStart = 3u;      // 主条的 clusterStart（与钉段的 0 刻意不同，防抄错字段蒙混）
	cd.pinnedStart = pinnedStart;
	cd.pinnedCount = pinnedCount;
	cd.pinnedRadius = pinnedRadius;
	cd.pinnedWorldScale = worldScale;
	return cd;
}

/** emit 的全部出参（三块 push 存储 + 逐级清单），一次调用摆齐。 */
struct EmitOut {
	static constexpr uint32_t slots = 4u;
	std::vector<ShadowDrawArgs> oldArgs = std::vector<ShadowDrawArgs>(
	    static_cast<size_t>(kActiveShadowCascades) * slots, sentinelOld());
	std::vector<ShadowMeshletDrawArgs> meshletArgs = std::vector<ShadowMeshletDrawArgs>(
	    static_cast<size_t>(kActiveShadowCascades) * slots, sentinelNew());
	std::vector<ShadowMeshletDrawArgs> pinnedArgs = std::vector<ShadowMeshletDrawArgs>(
	    static_cast<size_t>(kActiveShadowCascades) * slots, sentinelNew());
	std::array<std::vector<DrawCall>, kActiveShadowCascades> out {};

	void run(std::span<const ShadowCandidate> cands) {
		const auto vols = makeVolumes();
		emitShadowCasters(cands, std::span<const ShadowVolume> { vols.data(), vols.size() }, slots,
			oldArgs, meshletArgs, pinnedArgs, out);
	}
};

/** 找本级联里 clusterStart == want 的那条簇 draw（钉段与主条同管线，只能靠 push 内容区分）。 */
const DrawCall* findMeshlet(const EmitOut& e, uint32_t c, uint32_t wantStart) {
	for (const auto& dc : e.out[c])
		if (dc.pipeline == 21u && peek<ShadowMeshletDrawArgs>(dc.pushData).clusterStart == wantStart)
			return &dc;
	return nullptr;
}

MeshAsset makeAsset() {
	MeshAsset a;
	a.shadowPipelineIndex = 7u;
	a.groupCount = 9u;
	a.shadowMeshletPipelineIndex = 21u;
	return a;
}

/** 门开（drawAtlas = true）——collectShadowCasters 读的是 shadowConfig() 单例。 */
void enableShadowAtlas() {
	ShadowConfig on;
	on.mode = ShadowMode::csm;
	setShadowConfig(on);
}

/** 一张只有 `slot` 有选级条目（可切）的 LOD 表。 */
MeshletShadowLodTable lodTableWith(uint32_t slot, uint32_t count = 5u, uint32_t start = 3u) {
	MeshletShadowLodTable t;
	t.bySlot.resize(4u);
	t.bySlot[slot] = MeshletShadowLodTable::Entry { .start = start, .count = count };
	return t;
}

/** 一张只有 `slot` 有钉段的 proxy 表。 */
ShadowProxyTable proxyTableWith(uint32_t slot, uint32_t start, uint32_t count, float radius) {
	ShadowProxyTable t;
	t.bySlot.resize(4u);
	t.bySlot[slot] = ShadowProxyTable::Entry { .start = start, .count = count, .radius = radius };
	return t;
}

}  // namespace

// ─── texel 接缝（P2 遗留的收口，三处消费者的第一处）─────────────────────────────

TEST_CASE("shadowTexelSize：闭式 = 2·XY 半宽 / 图集**单块**边长（分母不是图集整幅）") {
	// 变异红线 ⑦：分母改成 kShadowMapSize（2048）→ 本条红（纹素算小一半 ⟹ 门判松 ⟹ 多钉）。
	const auto vols = makeVolumes();
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		INFO("c" << c);
		CHECK(shadowTexelSize(vols[c].xyRadius) == texelClosed(c));
	}
	// 阳性对照：0 半宽 → 0 纹素（不是 NaN / 不是分母）
	CHECK(shadowTexelSize(0.0f) == 0.0f);
	// 单调：半宽翻倍纹素翻倍（把"除以常数"写成"减去常数"之类的变异在此红）
	CHECK(shadowTexelSize(2.0f) == doctest::Approx(shadowTexelSize(1.0f) * 2.0f));
}

// ─── emit：第二条 draw 的形状 ───────────────────────────────────────────────────

TEST_CASE("emit：过 texel 门的钉段候选 → c0 恒一条、c1..c3 各两条（draws 上限 ×3 不是 ×4）") {
	enableShadowAtlas();
	const std::vector<ShadowCandidate> cands { makeSwitchable(0u, 0u, 2u, 1.0f, 1.0f) };
	EmitOut e;
	e.run(std::span<const ShadowCandidate> { cands });

	// c0：恒旧路径，钉段与它无关（🔴 这条就是"39 不是 52"的机器形态）
	REQUIRE(e.out[0].size() == 1u);
	CHECK(e.out[0][0].pipeline == 7u);
	CHECK(e.out[0][0].pushData.size() == sizeof(ShadowDrawArgs));
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c) {
		INFO("c" << c);
		REQUIRE(e.out[c].size() == 2u);  // 主条 + 钉段条
		// 顺序：主条在前、钉段在后（顺序本身不进像素判据，但它是"门关 = 后一条不执行"的可读性）
		CHECK(peek<ShadowMeshletDrawArgs>(e.out[c][0].pushData).clusterStart == 3u);
		CHECK(e.out[c][0].groupCount == kSlotsPerCluster * 5u);
		const auto* pin = findMeshlet(e, c, 0u);
		REQUIRE(pin != nullptr);
		CHECK(pin->pipeline == 21u);  // 同一条簇管线（钉段不新建条目）
		CHECK(pin->groupCount == kSlotsPerCluster * 2u);  // 🔴 task 数走 meshletTaskCount(钉段簇数)
		const auto args = peek<ShadowMeshletDrawArgs>(pin->pushData);
		CHECK(args.objectIndex == 0u);
		CHECK(args.cascadeIndex == c);
		CHECK(args.clusterStart == 0u);  // 变异红线 ③：写成 cd.clusterStart 在此红
		CHECK(pin->drawCount == 1u);     // 逐条形态（`drawCount > 1` 是 MDI 的唯一判据，见 DrawCall）
	}
}

TEST_CASE("emit：钉段那条的 push 必须落在**独立一块**存储（与主条共用 idx 会互相覆盖）") {
	enableShadowAtlas();
	const std::vector<ShadowCandidate> cands { makeSwitchable(1u, 0u, 2u, 1.0f, 1.0f) };
	EmitOut e;
	e.run(std::span<const ShadowCandidate> { cands });
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c) {
		INFO("c" << c);
		const auto* pin = findMeshlet(e, c, 0u);
		REQUIRE(pin != nullptr);
		const size_t idx = static_cast<size_t>(c) * EmitOut::slots + 1u;
		// 指针身份：钉段条指进 pinnedArgs，主条指进 meshletArgs（两块同一分块公式）。
		// 变异红线 ④：把钉段也写进 meshletArgs → 这两条断言一条都过不了。
		CHECK(pin->pushData.data()
		    == reinterpret_cast<const std::byte*>(&e.pinnedArgs[idx]));
		CHECK(peek<ShadowMeshletDrawArgs>(e.out[c][0].pushData).objectIndex == 1u);
		CHECK(e.out[c][0].pushData.data()
		    == reinterpret_cast<const std::byte*>(&e.meshletArgs[idx]));
		// 主条的内容没被钉段覆盖（两条 draw 各自成立）
		CHECK(peek<ShadowMeshletDrawArgs>(e.out[c][0].pushData).clusterStart == 3u);
	}
}

TEST_CASE("emit：texel 门不过 → 一条都不多发，且钉段那块存储一个字节都没写") {
	enableShadowAtlas();
	// 直径 = 2·1e-4·1 = 2e-4 世界米，最细一级纹素 = 2·2/1024 ≈ 3.9e-3 ⟹ 恒不过门
	const std::vector<ShadowCandidate> cands { makeSwitchable(0u, 0u, 2u, 1e-4f, 1.0f) };
	EmitOut e;
	e.run(std::span<const ShadowCandidate> { cands });
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		CHECK(e.out[c].size() == 1u);
	for (const auto& a : e.pinnedArgs)
		CHECK(a.objectIndex == kSentinel);  // 哨兵未破 = 那条 draw 真的没发
}

TEST_CASE("emit：pinnedCount == 0（绝大多数候选）→ 与 P2 逐条同形，一条不多发") {
	enableShadowAtlas();
	const std::vector<ShadowCandidate> cands { makeSwitchable(0u, 0u, 0u, 1.0f, 1.0f) };
	EmitOut e;
	e.run(std::span<const ShadowCandidate> { cands });
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		CHECK(e.out[c].size() == 1u);
	for (const auto& a : e.pinnedArgs)
		CHECK(a.objectIndex == kSentinel);
}

TEST_CASE("emit：不可切候选（无簇条目）带着钉段也只发一条旧 draw —— 钉段是簇 draw，无处可发") {
	enableShadowAtlas();
	auto cd = makeSwitchable(0u, 0u, 2u, 1.0f, 1.0f);
	cd.meshletPipeline = kInvalidPipelineIndex;  // 表说有钉段，但本候选没有能画簇的条目
	const std::vector<ShadowCandidate> cands { cd };
	EmitOut e;
	e.run(std::span<const ShadowCandidate> { cands });
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		INFO("c" << c);
		REQUIRE(e.out[c].size() == 1u);
		CHECK(e.out[c][0].pipeline == 7u);  // 恒旧管线
	}
	for (const auto& a : e.pinnedArgs)
		CHECK(a.objectIndex == kSentinel);
}

TEST_CASE("emit：多候选混合（有钉段 / 无钉段 / 不可切）逐级条数 = 闭式，钉段只加在该槽那一级") {
	enableShadowAtlas();
	std::vector<ShadowCandidate> cands;
	cands.push_back(makeSwitchable(0u, 0u, 2u, 1.0f, 1.0f));  // 有钉段
	cands.push_back(makeSwitchable(1u, 0u, 0u, 1.0f, 1.0f));  // 可切无钉段
	auto noPin = makeSwitchable(2u, 0u, 2u, 1.0f, 1.0f);
	noPin.meshletPipeline = kInvalidPipelineIndex;  // 不可切
	cands.push_back(noPin);
	EmitOut e;
	e.run(std::span<const ShadowCandidate> { cands });
	// c0：三条候选各一条旧 draw = 3；c1..c3：可切两条各 1 + 槽 0 的钉段 1 + 不可切 1 = 4
	CHECK(e.out[0].size() == 3u);
	for (uint32_t c = 1u; c < kActiveShadowCascades; ++c)
		CHECK_MESSAGE(e.out[c].size() == 4u, "c", c, " 的条数不是闭式 4");
}

// ─── collect：钉段四字段的填充与宿主 ───────────────────────────────────────────

TEST_CASE("collect：门关（proxyTable = nullptr）→ 钉段四字段全 0 = 逐位不变的构造性前提") {
	enableShadowAtlas();
	auto lod = lodTableWith(0u);
	std::vector<ShadowCandidate> out;
	collectShadowCasters(makeAsset(), MaterialAsset {}, glm::vec3 { 0.0f }, 0.5f, 0u, &lod, nullptr,
		0.01f, out);
	REQUIRE(out.size() == 1u);
	CHECK(out[0].meshletPipeline == 21u);  // 前置：可切成立（否则"全 0"是空表给的假绿）
	CHECK(out[0].pinnedStart == 0u);
	CHECK(out[0].pinnedCount == 0u);
	CHECK(out[0].pinnedRadius == 0.0f);
	CHECK(out[0].pinnedWorldScale == 0.0f);
}

TEST_CASE("collect：表在但该槽无钉段（count==0，1591 槽里 1578 槽如此）→ 全 0，不是错误") {
	enableShadowAtlas();
	auto lod = lodTableWith(0u);
	auto proxy = proxyTableWith(1u, 0u, 2u, 1.0f);  // 钉的是**别的**槽
	std::vector<ShadowCandidate> out;
	collectShadowCasters(makeAsset(), MaterialAsset {}, glm::vec3 { 0.0f }, 0.5f, 0u, &lod, &proxy,
		0.01f, out);
	REQUIRE(out.size() == 1u);
	CHECK(out[0].pinnedCount == 0u);
	// worldScale 是**实体**的事实（不是表的事实），候选可切就搬 —— 起作用的那一维是 count：
	// 门（`pinActive`）自带 `count > 0`，所以 count==0 时无论 worldScale 填什么都不发。
	CHECK_FALSE(pinActive(
	    ShadowProxyTable::Entry { .start = out[0].pinnedStart, .count = out[0].pinnedCount,
	        .radius = out[0].pinnedRadius },
	    out[0].pinnedWorldScale, texelClosed(1u)));
}

TEST_CASE("collect：可切不成立（lodTable = nullptr）而 proxy 表有条目 → 四字段仍全 0（宿主嵌套）") {
	// 变异红线 ⑤：把 collect 里的钉段填充挪出可切判据那个 `if` → 本条红（会填出一套
	// "有钉段但没有簇条目"的候选，emit 侧只能靠第二道判据兜住 = 同一判据两处表达）。
	enableShadowAtlas();
	auto proxy = proxyTableWith(0u, 0u, 2u, 1.0f);
	std::vector<ShadowCandidate> out;
	collectShadowCasters(makeAsset(), MaterialAsset {}, glm::vec3 { 0.0f }, 0.5f, 0u,
		/*lodTable=*/nullptr, &proxy, 0.01f, out);
	REQUIRE(out.size() == 1u);
	CHECK(out[0].meshletPipeline == kInvalidPipelineIndex);
	CHECK(out[0].pinnedCount == 0u);
	CHECK(out[0].pinnedWorldScale == 0.0f);
}

TEST_CASE("collect：门开且可切 → 四字段**原样**搬运 + worldScale 填的是入参") {
	enableShadowAtlas();
	auto lod = lodTableWith(2u);
	auto proxy = proxyTableWith(2u, 5u, 3u, 1.75f);
	std::vector<ShadowCandidate> out;
	collectShadowCasters(makeAsset(), MaterialAsset {}, glm::vec3 { 0.0f }, 0.5f, 2u, &lod, &proxy,
		0.01f, out);
	REQUIRE(out.size() == 1u);
	CHECK(out[0].pinnedStart == 5u);
	CHECK(out[0].pinnedCount == 3u);
	CHECK(out[0].pinnedRadius == 1.75f);
	CHECK(out[0].pinnedWorldScale == 0.01f);  // bistro 的 scene.lua scale
	// 主条字段没被钉段串台（两套事实各搬各的）
	CHECK(out[0].clusterStart == 3u);
	CHECK(out[0].meshletGroups == 5u);
}

TEST_CASE("collect：槽越出 proxy 表长度 → 不越界读，四字段全 0") {
	enableShadowAtlas();
	auto lod = lodTableWith(3u);
	ShadowProxyTable proxy;
	proxy.bySlot.resize(3u);  // 表比槽数短（装配层 resize 口径漂了的探针）
	std::vector<ShadowCandidate> out;
	collectShadowCasters(makeAsset(), MaterialAsset {}, glm::vec3 { 0.0f }, 0.5f, 3u, &lod, &proxy,
		0.01f, out);
	REQUIRE(out.size() == 1u);
	CHECK(out[0].pinnedCount == 0u);
}

TEST_CASE("端到端：门关（无表）与门开（表全 0）的逐级条数逐条相等 —— 构造性保证的形状版") {
	// 这条是"门关逐位不变"在 CPU 侧的形状证据：截图锚只验最终像素，验不出"根本没多发射"。
	enableShadowAtlas();
	auto lod = lodTableWith(0u);
	ShadowProxyTable empty;
	empty.bySlot.resize(4u);  // 表在，但一条钉段都没有（= v1 产物 / 门槛 0 的形态）
	std::vector<ShadowCandidate> closed, opened;
	collectShadowCasters(makeAsset(), MaterialAsset {}, glm::vec3 { 0.0f }, 0.5f, 0u, &lod, nullptr,
		0.01f, closed);
	collectShadowCasters(makeAsset(), MaterialAsset {}, glm::vec3 { 0.0f }, 0.5f, 0u, &lod, &empty,
		0.01f, opened);
	REQUIRE(closed.size() == 1u);
	REQUIRE(opened.size() == 1u);
	EmitOut a, b;
	a.run(std::span<const ShadowCandidate> { closed });
	b.run(std::span<const ShadowCandidate> { opened });
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		REQUIRE(a.out[c].size() == b.out[c].size());
		for (size_t i = 0; i < a.out[c].size(); ++i) {
			CHECK(a.out[c][i].pipeline == b.out[c][i].pipeline);
			CHECK(a.out[c][i].groupCount == b.out[c][i].groupCount);
		}
	}
}

TEST_CASE("texel 门与 emit 的口径一致：判据说钉 ⟹ 真发第二条（同一 texel 来源，不各算一遍）") {
	// 把 pinActive 与 emit 摆在一起对：判据给 true 的每一级都必须有第二条 draw。
	// 若 emit 自己另算一份纹素（P2 遗留的形态），这条会在"两处公式漂开"时红。
	enableShadowAtlas();
	const auto vols = makeVolumes();
	for (float radius : { 1.0f, 0.001f, 0.02f }) {
		INFO("radius=" << radius);
		const std::vector<ShadowCandidate> cands { makeSwitchable(0u, 0u, 2u, radius, 0.01f) };
		EmitOut e;
		e.run(std::span<const ShadowCandidate> { cands });
		for (uint32_t c = 1u; c < kActiveShadowCascades; ++c) {
			const bool want = pinActive(
			    ShadowProxyTable::Entry { .start = 0u, .count = 2u, .radius = radius }, 0.01f,
			    shadowTexelSize(vols[c].xyRadius));
			CHECK_MESSAGE(e.out[c].size() == (want ? 2u : 1u), "c", c, " 发了 ", e.out[c].size(),
			    " 条，判据要 ", want ? 2u : 1u, " 条");
		}
	}
}

}  // namespace RGL
