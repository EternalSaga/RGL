module;
/**
 * test_taa_cross_frame —— 相对坐标批 4 第一刀的**接线**判据（被测 = `taaSystem`，
 * rgl/ecs.cppm 声明、ecs:impl 定义）
 *
 * ## 为什么需要这个文件（纯函数测不到的那一格）
 * `motionWithOriginShift` 的闭式判据在 tests/test_coordinate_rebase.cppm，它只保证
 * 「给定两个原点，算得对」。它**不**保证：
 *  ① 调用点把「上一帧原点」和「本帧原点」传对了顺序（传反 = ΔO 变号，静默错位）；
 *  ② `TaaState::prevOrigin` 真的在帧尾被推进（忘推进 = 只有第一帧有 ΔO，之后恒 0）；
 *  ③ 原点差不被 `historyValid` / `taaMotionCompensation` 一起门掉（velocity.frag → FSR
 *    **不吃 historyValid**，门掉就是重定位当帧给 FSR 喂假运动）；
 *  ④ 门关态 motion 逐位 = 改前那条 `prevSpin·inverse(curSpin)`。
 * 这四条全是「结构上写了、行为上没接上」的藏身处，也正是批 3 留下的那条覆盖边界
 * （「生产点的变异单测不红，只有出厂截图锚能抓」）。本文件把它堵成单测。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 门关逐位锚的期望 = **改动前**的表达式 `prevSpin * glm::inverse(curSpin)` 本身
 *    （回归锚点，不是抄新实现；新实现是它的推广，门关必须退化到它）；
 *  - ΔO 方向锚的期望 = 闭式：静止场景（R = I）时 `W_{t−1} = W_t + (O_t − O_{t−1})`
 *    ⟹ motion 的平移列**恰为** `O_t − O_{t−1}`，符号由「本帧减上一帧」定死；
 *    构造量取 2 的幂 ⟹ float 精确 ⟹ 逐位断言，不许容差；
 *  - 自旋 + 移动原点的期望 = `worldByDefinition`：绝对点 P 在第 t 帧的世界坐标
 *    `S_t·P − O_t`，motion 的定义 = 满足 `motion·W_t = W_{t−1}` 的矩阵（推导
 *    → coordinate_rebase.cppm 头注释）。
 *
 * ## 变异红线（每条实测；读数 → 交付报告。回滚只用不带 -p 的 cp，还原后重跑基线）
 *  - `motionWithOriginShift` 里删掉恒等早退（门关靠「加 0」兜）      → 门关逐位那条红
 *  - `motionWithOriginShift` 的 ΔO 项摘掉（直接返回 spinRatio）      → ΔO 方向 / 自旋复合 /
 *    historyValid 不门控 三条全红
 *  - taaSystem 里两个原点参数**传反**（origin.position, taa.prevOrigin）→ ΔO 方向那条红
 *  - taaSystem 帧尾删掉 `taa.prevOrigin = origin.position;`          → 「推进」那条红
 *  - 把 `motionWithOriginShift(...)` 整个塞进 `(comp && historyValid)` 分支里 → ③ 那条红
 *  - 生产点（render_list_impl 的 spin 复合顺序）改回 `spin.matrix * toRelative(...)`
 *    ⚠ **本文件不红**：生产点不在 taaSystem 里，而 spin=false 的出厂态两种顺序逐位相同
 *    ⟹ 单测与截图锚都抓不到。守它的是 tests/test_coordinate_rebase.cppm 里「spin 锚点在
 *    绝对原点」那条闭式（它按定义算 W_t，顺序错则期望不匹配）+ 交付报告的如实记账。
 */
#include <doctest/doctest.h>
#include <entt/entt.hpp>  // entt 非模块化（main 同款 GMF include）
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <initializer_list>

export module test_taa_cross_frame;

import rlstd;
import test_support;    // testing::expectNear
import scene_context;   // SceneEnv / SceneSpin / TaaState / RenderOrigin / ViewProjection / SceneBounds
import ecs;             // taaSystem / RenderExtent
import camera;         // Camera

namespace RGL {
namespace {

/** 逐分量判 float 矩阵 16 个分量逐位相等（== 对有限 float 即逐位相同；NaN 恒 false = 必红，
 *  正是「逐位不变」判据要的语义——不许用 Approx/容差，容差会被谈到通过为止）。 */
void checkBitwise(const glm::mat4& actual, const glm::mat4& expected) {
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			CHECK(actual[col][row] == expected[col][row]);
}

/** 按**定义**算世界坐标：绝对点 P 在第 t 帧 = spin·P − origin（double，全程不碰被测码）。 */
glm::dvec3 worldByDefinition(const glm::mat4& spin, const glm::dvec3& pointAbs,
                             const glm::dvec3& origin) {
	return glm::dvec3(glm::dmat4(spin) * glm::dvec4(pointAbs, 1.0)) - origin;
}

}  // namespace

TEST_CASE("🔴 taaSystem 门关逐位锚：两帧原点都是 0 ⟹ payloadMotion 逐位 = prevSpin·inverse(curSpin)") {
	// 本批主判据（门关逐位不变）在**接线层**的对应：出厂截图锚守整条链，这条守
	// 「taaSystem 在门关时没有对 motion 动任何手」。期望 = 改动前那条表达式本身。
	// 覆盖 4 组 spin 变化（含 identity→identity、identity→旋转、旋转→旋转、旋转→identity），
	// 每组跑两帧（第一帧 historyValid=false、第二帧 true，两种门态都要逐位）。
	entt::registry rg;
	rg.ctx().emplace<ViewProjection>();
	auto& taa = rg.ctx().emplace<TaaState>();
	rg.ctx().emplace<SceneEnv>(SceneEnv{.taaEnabled = true});  // 历史可用性开起来（第二帧才走 spinRatio 分支）
	auto& spin = rg.ctx().emplace<SceneSpin>();
	rg.ctx().emplace<Camera>();
	rg.ctx().emplace<RenderExtent>();
	rg.ctx().emplace<SceneBounds>();
	auto& origin = rg.ctx().emplace<RenderOrigin>();

	const glm::mat4 rotA = glm::rotate(glm::mat4{1.0f}, 0.7f, glm::vec3{0.3f, 1.0f, -0.4f});
	const glm::mat4 rotB = glm::rotate(glm::mat4{1.0f}, -1.9f, glm::vec3{-0.8f, 0.2f, 0.5f});
	const glm::mat4 cases[][2] = {
		{glm::mat4{1.0f}, glm::mat4{1.0f}},
		{glm::mat4{1.0f}, rotA},
		{rotA, rotB},
		{rotB, glm::mat4{1.0f}},
	};
	for (const auto& pair : cases) {
		origin.position = glm::dvec3{0.0};  // 门关：projectionSystem 每帧钉成这个值（批 5 第 1 刀起）
		spin.matrix = pair[0];
		taaSystem(rg);
		spin.matrix = pair[1];
		taaSystem(rg);
		const glm::mat4 expect = pair[0] * glm::inverse(pair[1]);
		INFO("spin " << pair[0][3][0] << " -> " << pair[1][3][0]);
		checkBitwise(taa.payloadMotion, expect);
		CHECK(taa.prevOrigin == glm::dvec3{0.0});  // 门关簿记也是逐位零
	}
	// ⚠ 恒等早退（门关靠早退而不是靠「加 0」）**不在本文件测**，理由记在这里免得被
	// 当成漏测：本文件的入参是 `prevSpin * glm::inverse(curSpin)`，而那次矩阵乘的累加
	// 从 `0 + …` 起步，会把 −0.0 归一成 +0.0（实测 20000 组随机旋转比，平移列带 −0.0 的
	// 比例 = 0/20000）。也就是说经 taaSystem 这条路径，早退要防的那个形态**根本进不来**。
	// 早退是函数契约级的守卫，判据在 tests/test_coordinate_rebase.cppm（直接喂带 −0.0
	// 平移列的矩阵）。第一版把锚点写在本文件，M1 变异（删早退）实测 0 红 = 空判据。
}

TEST_CASE("🔴 ΔO 接线方向：本帧原点减上一帧原点（传反 = 变号，这条是唯一判据）") {
	// 静止场景（spin 恒单位阵）⟹ 闭式期望：motion 的平移列**恰为** ΔO = O_t − O_{t−1}。
	// 构造量全取 2 的幂 ⟹ float 精确 ⟹ 逐位断言。
	// 这条同时钉参数顺序：把 (prevOrigin, currentOrigin) 传反 ⟹ 平移列变号 ⟹ 立刻红。
	entt::registry rg;
	rg.ctx().emplace<ViewProjection>();
	auto& taa = rg.ctx().emplace<TaaState>();
	rg.ctx().emplace<SceneEnv>();  // taaEnabled = false（默认）：本条不依赖历史可用性
	rg.ctx().emplace<SceneSpin>();
	rg.ctx().emplace<Camera>();
	rg.ctx().emplace<RenderExtent>();
	rg.ctx().emplace<SceneBounds>();
	auto& origin = rg.ctx().emplace<RenderOrigin>();

	// 帧 1：原点 (0,0,0)（与 TaaState::prevOrigin 的初值一致 ⟹ ΔO = 0）
	origin.position = glm::dvec3{0.0};
	taaSystem(rg);
	CHECK(taa.payloadMotion == glm::mat4{1.0f});
	CHECK(taa.prevOrigin == glm::dvec3{0.0});

	// 帧 2：原点跳到 (1024, -256, 8) ⟹ ΔO = 那个向量本身
	origin.position = glm::dvec3{1024.0, -256.0, 8.0};
	taaSystem(rg);
	glm::mat4 expect{1.0f};
	expect[3] = glm::vec4{1024.0f, -256.0f, 8.0f, 1.0f};
	checkBitwise(taa.payloadMotion, expect);
	// 符号单独钉一次（上面整体 == 在「两个错号互相抵消」的形态下仍可能绿）
	CHECK(taa.payloadMotion[3][0] > 0.0f);
	CHECK(taa.payloadMotion[3][1] < 0.0f);
	CHECK(taa.payloadMotion[3][2] > 0.0f);
	// 簿记推进自证：本帧原点已经变成 prevOrigin
	CHECK(taa.prevOrigin == glm::dvec3{1024.0, -256.0, 8.0});

	// 帧 3：原点**没动** ⟹ ΔO = 0 ⟹ motion 逐位回到单位阵。
	// 🔴 这条是「帧尾忘推进 prevOrigin」的唯一判据：忘了就会把帧 2 的 ΔO 再算一遍。
	taaSystem(rg);
	checkBitwise(taa.payloadMotion, glm::mat4{1.0f});

	// 帧 4：原点往回跳 ⟹ ΔO 变号（钉「不是只认第一次那个方向」）
	origin.position = glm::dvec3{0.0};
	taaSystem(rg);
	glm::mat4 expectBack{1.0f};
	expectBack[3] = glm::vec4{-1024.0f, 256.0f, -8.0f, 1.0f};
	checkBitwise(taa.payloadMotion, expectBack);
}

TEST_CASE("🔴 原点差不许被 historyValid / taaMotionCompensation 一起门掉（velocity 链不吃门）") {
	// velocity.frag → FSR **不吃 historyValid**，每帧照算照输出 ⟹ 重定位当帧的 ΔO 必须在场。
	// 而 spinRatio 那两项门控是 TAA 自己的消融旋钮，与坐标系换算无关。
	// 本条把「ΔO 在两个门都关时仍然复合进去」钉成断言：把 motionWithOriginShift 塞进
	// `(comp && historyValid)` 分支里的变异，立刻在这里红。
	entt::registry rg;
	rg.ctx().emplace<ViewProjection>();
	auto& taa = rg.ctx().emplace<TaaState>();
	auto& env = rg.ctx().emplace<SceneEnv>();
	auto& spin = rg.ctx().emplace<SceneSpin>();
	rg.ctx().emplace<Camera>();
	rg.ctx().emplace<RenderExtent>();
	rg.ctx().emplace<SceneBounds>();
	auto& origin = rg.ctx().emplace<RenderOrigin>();

	env.taaEnabled = false;        // ⟹ historyValid 恒 false（taaHistoryUsable 第一条就否）
	env.taaMotionCompensation = false;  // 自旋补偿消融门关
	spin.matrix = glm::rotate(glm::mat4{1.0f}, 1.1f, glm::vec3{0.0f, 1.0f, 0.0f});
	origin.position = glm::dvec3{0.0};
	taaSystem(rg);  // 帧 1：ΔO = 0，且 spinRatio 被两个门关成单位阵

	spin.matrix = glm::mat4{1.0f};
	origin.position = glm::dvec3{512.0, 0.0, -128.0};
	taaSystem(rg);  // 帧 2：ΔO ≠ 0

	// 门控生效的部分：自旋比必须被关成单位阵（本门语义没被我改坏）
	CHECK(taa.historyValid == false);
	// 门控**不该**管的部分：平移列必须恰为 ΔO（= 512, 0, −128），逐位
	CHECK(taa.payloadMotion[3][0] == 512.0f);
	CHECK(taa.payloadMotion[3][1] == 0.0f);
	CHECK(taa.payloadMotion[3][2] == -128.0f);
	CHECK(taa.payloadMotion[3][3] == 1.0f);
	// 反向半边：若把整条 motion 塞进门里，上面会全塌成 0 —— 用「非零」把这条锚点钉实
	CHECK(taa.payloadMotion[3][0] != 0.0f);
}

TEST_CASE("taaSystem 自旋 + 移动原点：payloadMotion 对上按定义算的两帧世界坐标") {
	// 一般形态的接线判据（闭式在 test_coordinate_rebase 里测函数本体，这条测调用点）。
	// 期望 = `worldByDefinition`：绝对点 P 在第 t 帧的世界坐标 = spin_t·P − O_t，
	// motion 的定义 = 满足 `motion·W_t = W_{t−1}`。全程不碰被测表达式。
	entt::registry rg;
	rg.ctx().emplace<ViewProjection>();
	auto& taa = rg.ctx().emplace<TaaState>();
	auto& env = rg.ctx().emplace<SceneEnv>(SceneEnv{.taaEnabled = true});
	auto& spin = rg.ctx().emplace<SceneSpin>();
	rg.ctx().emplace<Camera>();
	rg.ctx().emplace<RenderExtent>();
	rg.ctx().emplace<SceneBounds>();
	auto& origin = rg.ctx().emplace<RenderOrigin>();

	const glm::mat4 spinPrev = glm::mat4{1.0f};
	const glm::mat4 spinCur = glm::rotate(glm::mat4{1.0f}, 0.35f, glm::vec3{0.2f, 1.0f, -0.3f});
	const glm::dvec3 originPrev{2.0, 0.0, -1.0};
	const glm::dvec3 originCur{6.0, 0.0, 1.0};

	origin.position = originPrev;
	spin.matrix = spinPrev;
	taaSystem(rg);  // 帧 1
	spin.matrix = spinCur;
	origin.position = originCur;
	taaSystem(rg);  // 帧 2

	REQUIRE(env.taaEnabled == true);
	const glm::mat4 motion = taa.payloadMotion;
	int checked = 0;
	for (double x : {-7.0, -1.5, 0.0, 2.5, 9.0}) {
		for (double y : {-4.0, 0.0, 3.0}) {
			for (double z : {-11.0, 0.0, 5.0, 14.0}) {
				const glm::dvec3 p{x, y, z};
				const glm::dvec3 wCur = worldByDefinition(spinCur, p, originCur);
				const glm::dvec3 wPrev = worldByDefinition(spinPrev, p, originPrev);
				const glm::dvec4 got = glm::dmat4(motion) * glm::dvec4(wCur, 1.0);
				INFO("p = " << p.x << "," << p.y << "," << p.z);
				for (int axis = 0; axis < 3; ++axis)
					testing::expectNear(got[axis], wPrev[axis], 1e-5, 1e-3);
				++checked;
			}
		}
	}
	CHECK(checked == 60);  // 空转守卫（5 × 3 × 4）
	// 前提守卫：本样本的 ΔO 与 spin 都真的在动（否则退化成门关，上面那条成了自比恒等）
	CHECK(originCur != originPrev);
	CHECK(spinCur != spinPrev);
}

TEST_CASE("反向用例：payloadMotion 不是「只把 spinRatio 抄一遍」（防接了但没接上）") {
	// 批 4 判据表第 ② 条的单元级替身：门关态抓不到「接了但没接上」，而这条抓得到 ——
	// 它直接断言门开态的 payloadMotion **不等于**同输入下的纯旋转比。
	entt::registry rg;
	rg.ctx().emplace<ViewProjection>();
	auto& taa = rg.ctx().emplace<TaaState>();
	rg.ctx().emplace<SceneEnv>();
	auto& spin = rg.ctx().emplace<SceneSpin>();
	rg.ctx().emplace<Camera>();
	rg.ctx().emplace<RenderExtent>();
	rg.ctx().emplace<SceneBounds>();
	auto& origin = rg.ctx().emplace<RenderOrigin>();

	origin.position = glm::dvec3{0.0};
	spin.matrix = glm::mat4{1.0f};
	taaSystem(rg);
	origin.position = glm::dvec3{256.0, 64.0, -32.0};
	taaSystem(rg);

	const glm::mat4 pureSpinRatio = taa.prevSpin * glm::inverse(spin.matrix);  // 改前的式子
	CHECK(taa.payloadMotion != pureSpinRatio);
	// 且差值恰为 ΔO（不是「差不多」）：平移列之差逐位等于 (256, 64, −32)
	for (int row = 0; row < 3; ++row)
		CHECK(taa.payloadMotion[3][row] - pureSpinRatio[3][row] ==
		      static_cast<float>(origin.position[row]));
}

}  // namespace RGL
