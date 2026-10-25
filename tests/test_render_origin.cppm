module;
/**
 * test_render_origin —— 相对坐标批 3 立、**批 5 第 1 刀改写**：`RenderOrigin` context
 * （rgl/scene_context.cppm）+ 它的生产者 `projectionSystem`（rgl/ecs.cppm 声明、ecs:impl 定义）
 *
 * ## 本文件为什么跟着「删掉 originSystem」一起重写
 * 原本 4 条用例里有 3 条测的是 `originSystem`（门关钉 0 / 门开必须抛 / 幂等）。批 5 第 1 刀把
 * 原点的生产搬进 `projectionSystem`（裁决：吸附粒度 ← 级联球半径 ← near/far，而 near/far 只在
 * projectionSystem 里算，同时 view 又要吃原点 —— 三者唯一同在处就是那个函数内部），
 * `originSystem` 连同「门开未实现」那条 throw 一起删除。本文件的被测对象随之换成
 * `projectionSystem`，**原判据的语义一条没丢**：
 *  - 「门关 = 逐位 (0,0,0)」→ 用例 ②（原本 ②）
 *  - 「门开不许静默按门关跑」→ 用例 ③/④（门开必须产出非零 snap 后的原点）+ ⑤（坏粒度必须抛）
 *  - 「幂等」→ 用例 ⑥
 *
 * ## 六条用例与期望值出处（全部独立于被测实现）
 *  ① `RenderOrigin{}` 默认 = 门关画像：position 逐位 (0,0,0) 且 enabled == false。
 *     出处 = docs/相对坐标地基_plan.md §2.2（门关 = 交付图逐位不变的前提）。
 *  ② 门关反向用例：先铺 Lcg 随机非零原点并**回读自证**，跑完 projectionSystem 后 position
 *     必须**恰为** dvec3(0)（memcmp 逐位，不是「接近」），且 view 逐位 = 改动前原式
 *     `glm::lookAt(glm::vec3{position}, ...)`（出处 = 批 4 第二刀之前的生产表达式本身）。
 *  ③ 🔴 门开原点 = `snapOriginToGranularity(camera.position, G)` 闭式，G 由**本文件独立**按
 *     「相机位置 → 级联球 → 纹素 → 最粗纹素取 2 的幂」重算（出处 = 债-85 ② 的定义，
 *     不是抄被测码）。三条子判据：非零 / 每分量是 G 的整数倍 / 与闭式逐位相同。
 *  ④ 🔴 **精度收益真的买到了**（不是「非零」就算数）：|camera| ≈ 1e6 工作点上
 *     `|camera.position − origin| ≤ G·√3/2`（snap 的定义 ⟹ 每轴误差 ≤ 半格），而
 *     `float32 ULP(|相对相机位|) × 1e3 < float32 ULP(|绝对相机位|)`。
 *     后者的期望值出处 = IEEE 事实「ULP(2^e ≤ x < 2^(e+1)) = 2^(e−23)」，两边指数差 20 以上
 *     ⟹ 比值 ≥ 1e6，取 1e3 是**大幅收紧后仍留三个数量级**的下界。
 *     ⚠ 只断言「原点非零」会放过一个「接了但把相机位置原样传下去」的实现（非零、但一分精度
 *     收益都没买到）—— 那正是本项目反复登记的「看起来已完工的半吊子」。
 *  ⑤ 🔴 两条粒度路径一致：`projectionSystem` 内部算的粒度（体现在它写出的原点上）必须 ==
 *     `rebaseSnapGranularity(fillShadowData 写进 scene data 的那份 texels)`。两条路径不一致
 *     = 单一来源破了（AGENTS 哲学第 1 条）。配**区分力守卫**：同一个相机位置在 G/2 与 G·2 下
 *     snap 出的原点必须都 ≠ 本值，否则「== G」这条抓不到粒度被换掉。
 *  ⑥ 🔴 粒度 NaN 必须响亮 throw（取代批 3 的「门开未实现」throw）：NaN 原点会让 view 的平移列
 *     全 NaN ⟹ 整个场景被裁掉、不报任何错（相机凭空消失）。构造 = near/far 退化
 *     （camera.nearPlane == camera.farPlane == 0 且空场景）⟹ 四级拟合球半径全 0 ⟹
 *     `rebaseSnapGranularity` 按契约返回 NaN。消息必须能归因到配置键 `coordinate.rebase`。
 *  ⑦ 幂等 / 可复现：同一输入连跑两次，门关与门开的 position 与 view 都逐位相同。
 *     🔴 这条同时是**批 5 第 2 刀（平地仪器 + 1e6 工作点）的接口**：那把仪器要拿「门开态
 *     跨进程复现同一个原点」当同相位前提，本条就是它的进程内对应物。
 *
 * ## 变异红线声明（「改坏什么 → 哪条用例红」，全部**实测**，读数见交付报告）
 *  - projectionSystem 的门开分支整个删掉（只留门关钉 0）→ ③④⑤⑦ 全红（原点恒 0）
 *  - 门开分支改成「不 snap，直接 position = camera.position」→ ③ 的「落在格上」红 + ⑤ 红；
 *    ⚠ **④ 不红**（相机位原样传下去时 |camera − origin| = 0 ≤ 半格也成立）——这条缺口是
 *    实测跑出来的，所以 ③ 的「每分量是 G 的整数倍」与 ⑤ 的粒度对账才是承重项，别删。
 *  - 门开分支改成「position = 上一帧那份」（吃陈旧值）→ ⑦ 红（第二次跑与第一次不同）
 *  - 粒度 NaN 的 throw 删掉 → ⑥ 红（本批主锚点之一）
 *  - throw 消息删掉 "coordinate.rebase" 字样 → ⑥ 红（失败不可归因到配置键）
 *  - 门关分支删掉 `position = dvec3{0.0}` → ② 红（垃圾值留场）
 *  - 门关分支改写非零量 dvec3{0.5} → ② 红（门关不再恒等）
 *  - 门关/门开判据反转（`origin.enabled` 改成 `!origin.enabled`）→ ②③④⑤⑥⑦ 全红
 *  - `rebaseOriginGranularity` 喂**真实 view**（而不是单位 view）→ 本文件不红（半径姿态无关，
 *    两条路径同值）；守「单位 view 这个捷径成立」的是 tests/test_rebase_consumers.cppm
 *    「texelSizes 整链平移不变」+ tests/test_shadow_geometry.cppm「frustumBoundingSphere
 *    姿态无关性」。**如实记这条覆盖边界**，别把本文件读成验了单位 view 的正确性。
 *  - `rebaseOriginGranularity` 另推一份闭式（不吃 computeCascadeSpheres/shadowTexelSize）
 *    → ⑤ 红（与 fillShadowData 那份 texels 推出的粒度分家）
 *  - RenderOrigin::enabled 默认翻成 true → ① 红
 *  - ⚠ **消费点不在本文件射程**：门关 origin = 0 时「绕过 toRelative 直接用绝对坐标」的消费点
 *    变异**逐位不可见**（截图锚也抓不到——它不改变像素）。守它的是批 3 契约「全仓唯一减法入口
 *    = toRelative」的结构约束 + 出厂截图锚抓**改值型**变异（批 3 实测：消费点原点换成
 *    dvec3{1,0,0} → 单测全绿而截图 efa70587… 变 e7f3d387…）。
 */
#include <doctest/doctest.h>
#include <entt/entt.hpp>  // entt 非模块化（main 同款 GMF include；include 面随 rlbase PUBLIC 传递）
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt（② 的改动前原式 = 独立期望值）
#include <cmath>       // std::isfinite / std::nextafter / std::logb / std::ldexp
#include <cstring>     // memcmp（逐位零判据；modules 下自由函数不从 import 带来）
#include <initializer_list>  // modules 坑：花括号列表不从 import 带进来
#include <stdexcept>   // std::runtime_error（REQUIRE_THROWS_AS 的期望类型）

export module test_render_origin;

import rlstd;
import test_support;    // testing::Lcg
import scene_context;   // RenderOrigin / SceneBounds / ViewProjection / Light / quantizeShadowDistance
import scene_data;      // kActiveShadowCascades
import render_graph;    // shadowAtlasColumns / shadowTileSize
import render_list;     // rebaseOriginGranularity / computeCascadeSpheres / buildShadowVolumes 侧接缝
import frame_draw_list;  // FrameDrawList（fillShadowData 的出参 = 粒度对账的另一条路径）
import coordinate_rebase;  // rebaseSnapGranularity / snapOriginToGranularity / toRelative
import camera;          // Camera / CameraMode / cameraForward / cameraUp
import ecs;             // projectionSystem（原点生产者）/ RenderExtent

namespace RGL {
namespace {

using testing::Lcg;

/** 逐位判 dvec3 全零（memcmp：浮点 == 对 −0.0/+0.0 放水，「恰为」不许放水）。 */
bool isBitwiseZero(const glm::dvec3& v) {
	static constexpr double kZero = 0.0;
	const auto* zero = reinterpret_cast<const unsigned char*>(&kZero);
	const auto* bytes = reinterpret_cast<const unsigned char*>(&v);
	for (int k = 0; k < 3; ++k)
		if (std::memcmp(bytes + k * sizeof(double), zero, sizeof(double)) != 0)
			return false;
	return true;
}

/** Lcg 固定种子铺一个三分量非零的 dvec3（回读自证在调用方，rgl-test-discipline）。 */
glm::dvec3 garbagePosition(Lcg& rng) {
	glm::dvec3 g{0.0};
	do {
		g = glm::dvec3{rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6)};
	} while (g.x == 0.0 || g.y == 0.0 || g.z == 0.0);  // 0 概率为 0，防御一下不靠运气
	return g;
}

/** 逐位相同（memcmp：+0.0 与 −0.0 判不同 —— 「逐位」两字不许读成「值相等」）。 */
bool bitwiseEqual(const glm::dvec3& a, const glm::dvec3& b) {
	return std::memcmp(&a, &b, sizeof(glm::dvec3)) == 0;
}
bool bitwiseEqual(const glm::mat4& a, const glm::mat4& b) {
	return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
}

/**
 * 一次驱动 projectionSystem 所需的最小 registry。
 * 装配面 = main 启动期那几枚 ctx（RenderExtent / Camera / SceneBounds / ViewProjection /
 * RenderOrigin）；**不装模型**（无设备单测拿不到 AssetTable），SceneBounds 由用例自己填。
 */
struct Rig {
	entt::registry rg;
	RenderExtent& extent;
	Camera& camera;
	SceneBounds& bounds;
	ViewProjection& viewProj;
	RenderOrigin& origin;

	Rig()
		: extent(rg.ctx().emplace<RenderExtent>()), camera(rg.ctx().emplace<Camera>()),
		  bounds(rg.ctx().emplace<SceneBounds>()), viewProj(rg.ctx().emplace<ViewProjection>()),
		  origin(rg.ctx().emplace<RenderOrigin>()) {
		// 出厂渲染网格（configs/runtime.lua headless_size = { 1280, 720 }）
		extent.width = 1280u;
		extent.height = 720u;
	}
};

/** 出厂交付机位（configs/camera.lua）+ 债-139 实测机位的场景包围球量级。 */
void setFactoryPose(Rig& rig) {
	rig.camera.position = glm::dvec3{1.1, 0.75, 1.05};
	rig.camera.target = glm::dvec3{0.14, 0.12, 0.02};
	rig.camera.mode = CameraMode::Trackball;
	rig.bounds.center = glm::vec3{0.0f, 0.4f, 0.0f};
	rig.bounds.radius = 0.9f;  // ⟹ 自适应 near/far 分支生效（与出厂实机同一形态）
}

/**
 * 独立重算本帧的原点吸附粒度（**另一条路径**：走 fillShadowData 写进 scene data 的那份 texels）。
 * 期望值出处 = 生产链本身（computeCascadeSpheres → buildShadowVolume → fillShadowData →
 * rebaseSnapGranularity），不是 projectionSystem 内部那三行 —— 两条路径同值才是「单一来源」。
 */
double granularityViaSceneData(const Rig& rig, const glm::dvec3& origin) {
	const float shadowNear = quantizeShadowDistance(rig.viewProj.nearPlane);
	const float shadowFar = quantizeShadowDistance(rig.viewProj.farPlane);
	const auto spheres = computeCascadeSpheres(rig.viewProj, shadowNear, shadowFar);
	const uint32_t snapGrid = shadowTileSize(shadowAtlasColumns(kActiveShadowCascades));
	// 主光 = configs/scene.lua 的 light.direction（消费时归一化）；只 direction 参与光基
	const Light sun{.index = 0u, .direction = glm::vec3{0.5f, -0.75f, -0.45f}};
	std::array<ShadowVolume, kActiveShadowCascades> volumes{};
	const glm::vec3 casterMin{-1.5f, -0.5f, -1.5f};
	const glm::vec3 casterMax{1.5f, 2.0f, 1.5f};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		volumes[c] = buildShadowVolume(sun, spheres[c], casterMin, casterMax, snapGrid, origin);
	FrameDrawList list;
	fillShadowData(list, shadowNear, shadowFar, {volumes.data(), volumes.size()}, true, -1);
	const glm::vec4& texels = list.sceneData.shadowTexelSizes;
	return rebaseSnapGranularity(std::span<const float>{&texels[0], 4});
}

/** float32 在 |x| 附近的 ULP（期望值出处 = IEEE 事实，用 nextafter 现算，不查表）。 */
double floatUlpNear(float x) {
	const float a = std::nextafter(std::abs(x), std::numeric_limits<float>::infinity());
	return static_cast<double>(a) - static_cast<double>(std::abs(x));
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// ① 类型侧：门关画像
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("RenderOrigin 默认构造 = 门关画像：position 逐位 (0,0,0) 且 enabled == false") {
	// 出厂态 = 交付图逐位不变的前提（计划 §2.2「门关 = 逐位不变」的类型侧对应）。
	// 变异红线：enabled 默认翻 true / position 默认初值漂 → 本条红。
	const RenderOrigin o{};
	CHECK(o.enabled == false);
	CHECK(isBitwiseZero(o.position));
}

// ═══════════════════════════════════════════════════════════════════════════
// ② 门关：projectionSystem 把原点钉回逐位零（原 originSystem 那条反向用例）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 门关 projectionSystem 反向用例：铺非零垃圾 → 跑完 position 恰为 (0,0,0) 且 view 逐位不变") {
	// 阳性锚点：先铺一个**被测动作绝不可能保留**的值并回读自证——若 system 什么都没写，
	// 垃圾值原样留场，本条红（rgl-test-discipline §锚点回读）。
	Rig rig;
	Lcg rng(20261025u);  // 写死种子（可重放）
	setFactoryPose(rig);
	const glm::dvec3 garbage = garbagePosition(rng);
	REQUIRE(garbage.x != 0.0);
	REQUIRE(garbage.y != 0.0);
	REQUIRE(garbage.z != 0.0);
	rig.origin.position = garbage;
	rig.origin.enabled = false;
	REQUIRE(rig.origin.position == garbage);  // 回读自证：铺进去的确实是垃圾

	projectionSystem(rig.rg);

	// 「恰为」不是「接近」：门关语义 = 恒等减法的前提是原点**逐位**为零。
	CHECK(isBitwiseZero(rig.origin.position));
	CHECK(rig.origin.enabled == false);  // system 不碰配置门
	// view 逐位 = **改动前**的原式（门关恒等锚；出处 = 批 4 第二刀之前的生产表达式本身）
	const glm::vec3 eyeF{rig.camera.position};
	CHECK(bitwiseEqual(rig.viewProj.view,
	                   glm::lookAt(eyeF, toRelative(rig.camera.target, glm::dvec3{0.0}),
	                               rig.camera.up)));
	// 近远平面与原点无关（两项皆绝对量，差平移不变）：必须落在自适应分支上
	CHECK(rig.viewProj.nearPlane > 0.0f);
	CHECK(rig.viewProj.farPlane > rig.viewProj.nearPlane);
}

// ═══════════════════════════════════════════════════════════════════════════
// ③④⑤ 门开：原点闭式 / 精度收益 / 两条粒度路径一致
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 门开原点的三条子判据（③）+ 粒度对账（⑤）。跑两个工作点：出厂机位与 |camera| ≈ 1e6。
 */
TEST_CASE("🔴 门开原点：非零 + 每分量落在 G 的格上 + == snapOriginToGranularity(camera.position, G)") {
	for (const bool farField : {false, true}) {
		Rig rig;
		setFactoryPose(rig);
		if (farField) {
			// 整个场景搬到 |camera| ≈ 1e6（rebase 真开工那天的量级；债-85 触发点 7.6e5）
			const glm::dvec3 shift{1.0e6, 2.5e5, -7.5e5};
			rig.camera.position += shift;
			rig.camera.target += shift;
			rig.bounds.center += glm::vec3{shift};
		}
		rig.origin.enabled = true;
		const glm::dvec3 desired = rig.camera.position;

		projectionSystem(rig.rg);

		const double G = granularityViaSceneData(rig, rig.origin.position);
		INFO("farField=" << farField << " G = " << G << " |camera| = " << glm::length(desired)
		                 << " origin = " << rig.origin.position.x << "," << rig.origin.position.y
		                 << "," << rig.origin.position.z);
		REQUIRE(std::isfinite(G));
		REQUIRE(G > 0.0);
		// ③-a 非零（门关 = 恒 0，门开必须不是它）
		CHECK(!isBitwiseZero(rig.origin.position));
		// ③-b 每分量落在 G 的格上（闭式：snap 的定义 = 分量是格距的整数倍）
		for (const double v : {rig.origin.position.x, rig.origin.position.y, rig.origin.position.z}) {
			const double steps = v / G;
			CHECK(std::abs(steps - std::round(steps)) <= 1e-6);  // 容差 = 格点量化自身的 double 残量
		}
		// ③-c 与闭式逐位相同（期望值 = snapOriginToGranularity(desired, G)，出处 = 债-85 ② 定义）
		CHECK(bitwiseEqual(rig.origin.position, snapOriginToGranularity(desired, G)));
		// ⑤ 两条粒度路径一致：若 projectionSystem 内部用的不是这个 G，上面 ③-c 就分家。
		// 🔴 区分力守卫（skill「对空集合的断言要加前提守卫」同族：抓不到的判据不叫判据）——
		// 同一个 desired 在 G/2 与 G·2 下 snap 出的原点必须**都不同**，否则「== G」这条对
		// 粒度被换成相邻档这件事结构性失明。
		const glm::dvec3 halfG = snapOriginToGranularity(desired, G * 0.5);
		const glm::dvec3 doubleG = snapOriginToGranularity(desired, G * 2.0);
		CHECK_FALSE(bitwiseEqual(rig.origin.position, halfG));
		CHECK_FALSE(bitwiseEqual(rig.origin.position, doubleG));
		MESSAGE("【读数】门开原点 farField=" << farField << "：G = " << G
		          << "，与 G/2、G·2 两档的 snap 结果均不同（区分力守卫通过）");
	}
}

TEST_CASE("🔴 门开的精度收益（不是「非零」就算数）：|camera| ≈ 1e6 处相对量级 ≪ 绝对量级") {
	Rig rig;
	setFactoryPose(rig);
	const glm::dvec3 shift{1.0e6, 2.5e5, -7.5e5};
	rig.camera.position += shift;
	rig.camera.target += shift;
	rig.bounds.center += glm::vec3{shift};
	rig.origin.enabled = true;
	const glm::dvec3 absolute = rig.camera.position;

	projectionSystem(rig.rg);

	const double G = granularityViaSceneData(rig, rig.origin.position);
	REQUIRE(G > 0.0);
	// ④-a 相机离原点的距离 ≤ 半格 × √3（snap 的定义：每轴误差 ≤ G/2，三轴合成 ≤ G·√3/2）
	const glm::dvec3 rel = rig.origin.position - absolute;
	const double relNorm = glm::length(rel);
	INFO("G = " << G << " |camera| = " << glm::length(absolute) << " |camera − origin| = " << relNorm);
	CHECK(relNorm <= G * 0.87 + 1e-9);  // √3/2 ≈ 0.866
	CHECK(relNorm > 0.0);               // 阳性锚点：原点没有恰好压在相机位上（那 = 没 snap）
	// ④-b **精度收益**：view 的平移列量级 ∝ |eye|（债-85 ① 点名的抖动主导项）。门关喂进
	// glm::lookAt 的是 float(1e6)（ULP = 0.0625 m），门开喂的是 float(rel)（ULP ≈ 4e-10）。
	// 期望值出处 = IEEE 事实 ULP(2^e ≤ x < 2^(e+1)) = 2^(e−23)，两边指数差 ≥ 20 ⟹ 比值 ≥ 1e6；
	// 断言取 1e3 = 比闭式界松三个数量级，仍然远大于「什么都没买到」（比值 = 1）。
	const double ulpAbsolute = floatUlpNear(static_cast<float>(glm::length(absolute)));
	const double ulpRelative = floatUlpNear(static_cast<float>(relNorm));
	MESSAGE("【读数】精度收益：float ULP(|绝对相机位|) = " << ulpAbsolute
	          << " m，ULP(|相对相机位|) = " << ulpRelative << " m，比值 = "
	          << ulpAbsolute / ulpRelative);
	CHECK(ulpRelative * 1.0e3 < ulpAbsolute);
	// ④-c 相对坐标真的进了 view：view 平移列量级 = |eye|（(s,u,f) 正交归一 ⟹ |c3| = |eye|）
	const glm::vec3 c3{rig.viewProj.view[3]};  // 列是 vec4，平移分量取 xyz（GLM 列主序）
	const double translationNorm =
	    std::sqrt(static_cast<double>(c3.x) * c3.x + static_cast<double>(c3.y) * c3.y +
	              static_cast<double>(c3.z) * c3.z);
	INFO("view 平移列量级 = " << translationNorm << "，|rel| = " << relNorm);
	CHECK(translationNorm < 1.0);  // 门关态这一项是 1e6 量级（tests/test_view_light_rebase 已钉）
	CHECK(translationNorm <= relNorm + 1e-3);
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑥ 粒度 NaN 必须响亮 throw（取代批 3 的「门开未实现」throw）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 门开 + 粒度 NaN 必须响亮失败：抛 std::runtime_error 且消息点名配置键") {
	// 构造坏 texels：near == far == 0 且空场景（SceneBounds.radius = 0 ⟹ 不进取自适应分支，
	// 直接用 camera 的 0/0）⟹ 四级拟合球半径全 0 ⟹ rebaseSnapGranularity 按契约返回 NaN。
	// 期望值出处 = coordinate_rebase.cppm rebaseSnapGranularity 的「空输入或全零 ⟹ NaN」那条。
	Rig rig;
	setFactoryPose(rig);
	rig.bounds.radius = 0.0f;  // 空场景 = 回落 camera 的固定 near/far
	rig.camera.nearPlane = 0.0f;
	rig.camera.farPlane = 0.0f;
	rig.origin.enabled = true;
	rig.origin.position = glm::dvec3{0.0};

	// 前提：这条构造**真的**产出了 NaN 粒度（否则 REQUIRE_THROWS_AS 是在验一件没发生的事）
	REQUIRE(computeCascadeSpheres(rig.viewProj, quantizeShadowDistance(0.0f),
	                              quantizeShadowDistance(0.0f))[0]
	            .radius == 0.0f);

	REQUIRE_THROWS_AS(projectionSystem(rig.rg), std::runtime_error);

	std::string message;
	try {
		projectionSystem(rig.rg);
		FAIL("粒度 NaN 时未抛出（上一条之外的二次确认，防 REQUIRE_THROWS_AS 语义误读）");
	} catch (const std::runtime_error& e) {
		message = e.what();
	}
	// 消息必须能归因：点名配置键（用户照着能直接改对）
	CHECK(message.find("coordinate.rebase") != std::string::npos);
	// 抛出前未写 position：门开路径**没有半执行**（不是「先写个 NaN 再抛」）
	CHECK(isBitwiseZero(rig.origin.position));
}

TEST_CASE("门开但粒度正常时**不许**抛（反向半边：把 throw 写成无条件 ⟹ 本条红") {
	// ⑥ 的反向半边：只验「坏输入抛」抓不到「无条件抛」，而后者会让门开路径永远跑不起来
	// （= 一个「接了但一开就死」的形态，比不接更难发现）。
	Rig rig;
	setFactoryPose(rig);
	rig.origin.enabled = true;
	CHECK_NOTHROW(projectionSystem(rig.rg));
	CHECK(!isBitwiseZero(rig.origin.position));
}

// ═══════════════════════════════════════════════════════════════════════════
// ⑦ 幂等 / 跨进程可复现的进程内对应物（批 5 第 2 刀的接口）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("幂等：同一输入连跑两次，门关与门开的 position / view / proj 都逐位相同") {
	// 「每帧重算」的生产者必须对同输入给同输出（帧间簿记类 bug 的最小暴露形态：第二帧起值漂）。
	// 🔴 门开那一半同时是**批 5 第 2 刀的接口凭据**：平地仪器要拿「门开态跨进程复现同一个原点」
	// 当同相位前提，本条是它的进程内对应物（跨进程那条由截图锚 becedb7c… 连跑两次复现另证）。
	for (const bool enabled : {false, true}) {
		Rig rig;
		setFactoryPose(rig);
		rig.origin.enabled = enabled;
		rig.origin.position = enabled ? glm::dvec3{0.0} : glm::dvec3{123.5, -0.25, 625.125};
		projectionSystem(rig.rg);
		const glm::dvec3 first = rig.origin.position;
		const glm::mat4 firstView = rig.viewProj.view;
		const glm::mat4 firstProj = rig.viewProj.proj;
		projectionSystem(rig.rg);
		INFO("enabled = " << enabled);
		CHECK(bitwiseEqual(rig.origin.position, first));
		CHECK(bitwiseEqual(rig.viewProj.view, firstView));
		CHECK(bitwiseEqual(rig.viewProj.proj, firstProj));
		if (!enabled)
			CHECK(isBitwiseZero(first));
	}
}

}  // namespace RGL
