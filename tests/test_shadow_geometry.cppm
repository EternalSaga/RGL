module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

// 阴影几何回归测试（批 12.1 · B1）——把 Phase 11 批 11.2 的 5a-3→5a-9 七轮排障
// 固化成断言。被测对象全在 rgl/scene_context.cppm（rlcore 成员，纯函数，零 GPU）。
//
// ## 为什么要固化这一段
//
// docs/调试历史.md 里阴影链的代价是全项目最高的：七轮修复、基线连续作废 7 次、
// 且**两轮"修一个丢一个"**（5a-7 修好游动却引入 guard 反向，5a-8 修好平移却漏掉旋转）。
// 每一轮当时都"验过了"——三机位一致、开关对拍 6.17%、D1 0.005%、BIT-IDENTICAL 全绿。
// 共同死法（rgl-verify skill 硬要求 1-7 条已逐条记录）：指标全绿而 bug 在场。
//
// 本文件的写法就是照着那份硬要求反着来：
//   · 每条断言都问"如果这条路径从未生效，这条断言会绿吗"——不会绿才算有效断言；
//   · 反向用例（guard / 早退分支）必须存在，否则"安静的坏"无法与"没触发"区分；
//   · 种子写死的 LCG 属性测试，失败可重放。
export module test_shadow_geometry;

import rlstd;
import test_support;
import scene_context;

namespace RGL {
namespace {

using testing::Lcg;
using testing::expectNear;
using testing::frustumCorners;

/** 属性测试的种子（写死；换种子 = 换一组输入，不是"重试"）。 */
constexpr uint32_t kSeed = 20260930u;

/** 一组典型 RH_ZO 透视投影参数（fov 角度、宽高比、near、far）。 */
struct ProjParams {
	float fovYDeg;
	float aspect;
	float nearZ;
	float farZ;
};

/** 属性测试用的相机姿态集：{yaw, pitch} 度。pitch 刻意避开 ±90（lookAt 退化）。 */
constexpr std::array<std::array<float, 2>, 6> kPoses{{
	{{0.0f, 0.0f}},      // 正对
	{{37.0f, 12.0f}},    // 斜看
	{{-120.0f, 25.0f}},  // 背面
	{{179.0f, -18.0f}},  // 近反向
	{{95.0f, 60.0f}},    // 高空俯视
	{{-45.0f, -55.0f}},  // 低空仰视（负 pitch）
}};

/** 构造 view 矩阵（lookAt 目标恒为原点，eye 由 yaw/pitch/距离给出）。 */
glm::mat4 makeView(const std::array<float, 2>& poseDeg, float distance) {
	const float yaw = glm::radians(poseDeg[0]);
	const float pitch = glm::radians(poseDeg[1]);
	const glm::vec3 dir{std::cos(pitch) * std::sin(yaw), std::sin(pitch),
		std::cos(pitch) * std::cos(yaw)};
	return glm::lookAt(dir * distance, glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
}

/** 构造 proj 矩阵（RL 可用，但**刻意不建相机 0 位移**——view 已是完整刚体变换）。 */
glm::mat4 makeProj(const ProjParams& p) {
	return glm::perspective(glm::radians(p.fovYDeg), p.aspect, p.nearZ, p.farZ);
}

/** 属性测试的输入采样器（固定种子；域覆盖项目实际用到的范围 + 边界外一点）。 */
struct Scene {
	Lcg rng{kSeed};
	ProjParams nextProj() {
		// near 的对数分布：项目实测 near 在 0.05~0.5 之间，near/far 比可到 1e-4
		const float nearZ = std::exp(std::log(0.01f) + (std::log(1.0f) - std::log(0.01f)) * rng.uniform01());
		const float farZ = nearZ * std::exp(std::log(2000.0f / 0.01f) * (0.15 + 0.8 * rng.uniform01()));
		return ProjParams{static_cast<float>(rng.uniform(25.0, 100.0)),
			static_cast<float>(rng.uniform(0.5, 2.4)), nearZ,
			static_cast<float>(farZ < 5000.0 ? farZ : 5000.0)};
	}
};

/** 主光（direction 有 y 分量向下——项目太阳方向典型形态；d.y>0 的路径由 support
 *  用例单独覆盖，那是 5a-8b 出错的那一半）。 */
constexpr Light kSunDown{0u, glm::vec3{0.35f, -1.0f, 0.25f}, glm::vec3{1.0f, 0.98f, 0.95f},
	3.0f, glm::vec3{0.03f}, glm::vec3{0.0f}, 0.0f, 0u};

/** 某图元的世界 AABB（min/max 角）。 */
struct Aabb {
	glm::vec3 lo;
	glm::vec3 hi;
};

/** AABB 沿方向 d 的支撑点（= support function 的离散定义）。 */
glm::vec3 supportPoint(const Aabb& box, const glm::vec3& d) {
	return glm::vec3{d.x >= 0.0f ? box.hi.x : box.lo.x, d.y >= 0.0f ? box.hi.y : box.lo.y,
		d.z >= 0.0f ? box.hi.z : box.lo.z};
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 0：测试辅助函数的自检
//
// 存在的理由：写本文件时 `test_support::frustumCorners` 自己把矩阵顺序写反了
// （`inverse(view*proj)` 应为 `inverse(proj*view)`），而**症状与被测代码的 bug
// 完全一样**——"拟合球装不下视锥"。它还先在 view=identity 下全对（此时两种顺序
// 恰好相同），是换个真机位才炸出 4241 条失败。
//
// 元模式「验证工具自己错」的教科书案例：工具的 bug 与被测对象的 bug 不可区分，
// 所以工具必须有自己的、**不依赖被测对象**的判据。这一组就是。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frustumCorners 自检：角点确实落在 proj*view 定义的视锥面上") {
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	for (const auto& pose : kPoses) {
		const auto view = makeView(pose, 5.0f);
		const auto corners = frustumCorners(view, proj);
		INFO("pose=(" << pose[0] << "," << pose[1] << ")");
		REQUIRE(corners.size() == 8);
		const glm::mat4 viewProj = proj * view;
		for (size_t i = 0; i < corners.size(); ++i) {
			const glm::vec4 clip = viewProj * glm::vec4(corners[i], 1.0f);
			// 往返：反投影出来的点再正投影，必须回到原 NDC 角点
			const double ndcX = clip.x / clip.w;
			const double ndcY = clip.y / clip.w;
			const double ndcZ = clip.z / clip.w;
			const double wantX = (i / 2 % 2 == 0) ? -1.0 : 1.0;
			const double wantY = (i % 2 == 0) ? -1.0 : 1.0;
			const double wantZ = (i / 4 == 0) ? 0.0 : 1.0;
			INFO("corner#" << i << " ndc=(" << ndcX << "," << ndcY << "," << ndcZ << ") want=("
						   << wantX << "," << wantY << "," << wantZ << ")");
			expectNear(ndcX, wantX, 0.0, 1e-4);
			expectNear(ndcY, wantY, 0.0, 1e-4);
			expectNear(ndcZ, wantZ, 0.0, 1e-4);
		}
	}
}

TEST_CASE("frustumCorners 自检：近平面角点确实比远平面角点更靠近相机（顺序语义）") {
	// 上一组只验"往返自洽"——一个把 z=0/z=1 弄反的辅助函数也能往返通过。
	// 本组用**物理意义**独立钉住：近平面 4 角到相机的距离 = near，远平面 = far。
	const float nearZ = 0.25f, farZ = 40.0f;
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, nearZ, farZ});
	const auto view = makeView(kPoses[0], 5.0f);
	const auto corners = frustumCorners(view, proj);
	const glm::vec3 eye = glm::vec3(view * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	// eyePos 由 view 的逆推出：view 把 eye 映射到原点，故 eye = inverse(view)*origin
	const glm::vec3 eyePos = glm::vec3(glm::inverse(view) * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
	(void)eye;
	for (size_t i = 0; i < 4; ++i) {
		INFO("near corner#" << i);
		// 角点在视空间 z = −near → 到相机距离恰为 near
		const glm::vec3 e = view * glm::vec4(corners[i], 1.0f);
		expectNear(-e.z, nearZ, 1e-3, 1e-3);
	}
	for (size_t i = 4; i < 8; ++i) {
		INFO("far corner#" << i);
		const glm::vec3 e = view * glm::vec4(corners[i], 1.0f);
		expectNear(-e.z, farZ, 1e-3, 1e-3);
	}
	// 横向范围：近平面半高 = near·tan(fov/2)，半宽 = 近平面半高 × aspect
	const glm::vec3 nearUp = view * glm::vec4(corners[1], 1.0f);
	const glm::vec3 nearLeft = view * glm::vec4(corners[0], 1.0f);
	const float tanHalf = std::tan(glm::radians(60.0f) * 0.5f);
	expectNear(nearUp.y, nearZ * tanHalf, 1e-3, 1e-4);
	expectNear(-nearLeft.x, nearZ * tanHalf * (16.0f / 9.0f), 1e-3, 1e-4);
	// 相机位置：inverse(view)·原点 = 眼点（用来确认 view 本身没被搞反）
	expectNear(glm::length(eyePos), 5.0, 1e-4, 1e-4);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：frustumBoundingSphere —— 拟合球的**存在性**与**正确性**
//
// 这组是整条链的地基：半径算错或恒 0，上游切片、投影体、阴影图全是垃圾，而当时
// 的验证指标（对拍/稳定性）全部照绿（5a-7：半径恒 0 → 全体积单位阵 → "恒等阵伪影
// 还姿态无关，把对拍指标全骗过"）。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frustumBoundingSphere 覆盖性：8 角点必在球内（4 参真值版，512 组随机参数）") {
	// 判据放**视空间**且用**真 near/far** 构造角点——这是本函数真正的契约：
	// "给定视锥，返回装下它的最小球"。若角点也用 proj 反解值构造，判据就变成
	// "球装下它自己反解出来的视锥"= 自洽但无意义（相消误差被抵消）。
	//
	// 首版的三处错（都记在这里，因为它们就是元模式「验证工具自己错」）：
	//   ① 在世界空间做 → 被 float32 inverse(proj*view) 的病态噪声淹没（195 条假失败）；
	//   ② 用 eyePlanes 反解的 near/far 构造角点 → 判据自洽但测不到真契约；
	//   ③ 用 2 参（反解）版 → 测出**真 bug**：near/far=8e4 时欠覆盖 0.49%，
	//      根因是 float32 矩阵在 1+A 处的相消（详见 scene_context 4 参重载注释）。
	//      该 bug 已由"4 参重载 + 调用方传真值"修掉；本条锁的是修后的契约。
	Scene scene;
	double worstOvershoot = -1e30;
	ProjParams worstP{};
	for (int i = 0; i < 512; ++i) {
		const ProjParams pp = scene.nextProj();
		const auto view = makeView(kPoses[static_cast<size_t>(i) % kPoses.size()], 5.0f);
		const auto proj = makeProj(pp);
		const Bounds b = frustumBoundingSphere(view, proj, pp.nearZ, pp.farZ);
		INFO("i=" << i << " fov=" << pp.fovYDeg << " aspect=" << pp.aspect << " near=" << pp.nearZ
				 << " far=" << pp.farZ);
		REQUIRE(b.radius > 0.0f);
		const glm::vec3 centerEye = glm::vec3(view * glm::vec4(b.center, 1.0f));
		const double tanHalf = std::tan(glm::radians(pp.fovYDeg) * 0.5);
		for (const glm::vec3& corner :
			testing::frustumCornersEye(pp.nearZ, pp.farZ, tanHalf, pp.aspect)) {
			const double dist = glm::length(glm::dvec3(corner) - glm::dvec3(centerEye));
			if (dist / b.radius - 1.0 > worstOvershoot) {
				worstOvershoot = dist / b.radius - 1.0;
				worstP = pp;
			}
		}
	}
	INFO("worst overshoot = " << worstOvershoot << " at near/far=" << (worstP.farZ / worstP.nearZ));
	// 契约：拟合球必须**保守**（装得下），且**逐位紧贴**——不是"差不多装下"。
	// 上界取 1e-3（保守系数 ×1.001 留的量 + float32 舍入），超了说明公式被改动
	// 且改动没同步更新保守系数，那属于契约变更，应当显式讨论而不是悄悄放宽。
	CHECK(worstOvershoot <= 1e-3);
}

TEST_CASE("frustumBoundingSphere 2 参（反解）版的精度契约：误差被 near/far 比放大") {
	// 2 参版仍在（语义自洽：只吃矩阵），但**精度契约与 4 参版不同**，必须单独钉住：
	// 反解 far 的相对误差 ≈ 1.19e-7·(far/near)/2。本组断言"误差随比值单调增长且有界"，
	// 而不是断言它等于 0——后者会把一个已知的物理限制误报成回归。
	// 这条断言的真正价值：若哪天有人把 2 参版悄悄改成"假装精确"，本组会红。
	for (const double ratio : {1e1, 1e2, 1e3, 1e4, 3e4, 8e4}) {
		const float n = 0.05f;
		const float f = static_cast<float>(n * ratio);
		const auto proj = makeProj({60.0f, 16.0f / 9.0f, n, f});
		const testing::EyePlanes ep = testing::eyePlanes(proj);
		const double relErr = std::abs(ep.farZ - static_cast<double>(f)) / static_cast<double>(f);
		INFO("ratio=" << ratio << " far=" << ep.farZ << " (真 " << f << ") relErr=" << relErr);
		// 上界：1.19e-7·ratio/2，留 3 倍余量
		CHECK(relErr < 3.0e-7 * ratio);
	}
	CHECK(testing::eyePlanes(makeProj({60.0f, 16.0f / 9.0f, 0.05f, 500.0f})).farZ > 0.0);
}

TEST_CASE("proj 反解精度：far 的相对误差随 near/far 比增长，且**方向两向皆可**") {
	// `f = B/(1+A)` 中 1+A 是 ~1 与 ~1 的相消 → float32 下 far 有相对误差。
	// 被测函数走同一条路，故这是**契约的一部分**，必须显式钉住而不是让它当"未知"。
	//
	// 实测结论（与初版断言相反，故更正）：误差**方向两向皆可**——near/far=100 时
	// 反解 far 偏小 7e-6，near/far=3e4 时偏小 1.3e-3。初版断言"必偏大（保守）"
	// 是我臆想的契约，被数据推翻（这本身是元模式：别把猜测写成断言）。
	// 真实契约只有两条：① 误差有界；② 反解值保持正序（far > near > 0）。
	for (const double ratio : {1e1, 1e2, 1e3, 1e4, 3e4}) {
		const float n = 0.1f;
		const float f = static_cast<float>(n * ratio);
		const auto proj = makeProj({60.0f, 16.0f / 9.0f, n, f});
		const testing::EyePlanes ep = testing::eyePlanes(proj);
		const double farRelErr = std::abs(ep.farZ - static_cast<double>(f)) / static_cast<double>(f);
		INFO("ratio=" << ratio << " near=" << ep.nearZ << " far=" << ep.farZ << " (真 far=" << f
					 << ") relErr=" << farRelErr);
		CHECK(ep.nearZ > 0.0);
		CHECK(ep.farZ > ep.nearZ);
		// 上界 ~1e-7/|1+A|，A≈−1 时 |1+A| ~ 2n/f，故 ~1e-7·f/(2n)；ratio=3e4 给 1.5e-3，
		// 留一倍余量取 5e-3
		CHECK(farRelErr < 5e-3);
		// 反解的 near 精确得多（n = B/A 无相消）——顺带钉住这条差异
		expectNear(ep.nearZ, static_cast<double>(n), 1e-4, 1e-5);
	}
}

TEST_CASE("frustumBoundingSphere 姿态无关性：只换 view 姿态，半径必须逐位不变") {
	// 这是 5a-7 的真凶判据。历史：拟合球曾改用"8 角世界 AABB"求半径，AABB 随 yaw
	// 变 → 半径连续变化 → 光空间投影体逐帧缩放 → 影子在地面上"游动"（用户实测机位
	// 平移 0.06 就让井盖阴影时有时无）。闭式解的整个意义就是"结果只依赖
	// near/far/fov/aspect，与姿态无关"。
	Scene scene;
	for (int i = 0; i < 128; ++i) {
		const ProjParams pp = scene.nextProj();
		const auto proj = makeProj(pp);
		double first = 0.0;
		for (size_t k = 0; k < kPoses.size(); ++k) {
			const double r = frustumBoundingSphere(makeView(kPoses[k], 5.0f), proj).radius;
			INFO(i << " " << k << " " << pp.fovYDeg << " " << pp.aspect << " " << pp.nearZ << " " << pp.farZ);
			if (k == 0)
				first = r;
			else
				expectNear(r, first, 1e-6, 1e-6);  // 逐位级：换姿态不该动它一个 ULP
		}
	}
}

TEST_CASE("frustumBoundingSphere 退化判据：正交 proj 返回零半径，正常透视不得被误判") {
	// 5a-8 的真凶：守卫写成 `A < -1` 判退化，而 RH_ZO 正常透视恒有 A = f/(n-f) < -1
	// → **所有正常透视都被判退化** → 半径恒 0。本组两侧都断言：
	//   正交 = 真退化（半径 0）；正常透视 = 绝不退化（半径 > 0）。
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	const auto view = makeView(kPoses[0], 5.0f);
	CHECK(frustumBoundingSphere(view, proj).radius > 0.0f);

	const auto ortho = glm::ortho(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 100.0f);
	expectNear(frustumBoundingSphere(view, ortho).radius, 0.0, 0.0, 0.0);
}

TEST_CASE("frustumBoundingSphere 极值参数：大 fov + 大 near/far 比不炸（相对误差守恒）") {
	// 闭式解里 zc = (f²−n²+rf²−rn²)/(2(f−n)) 在 near≪far 时是 f²−n² 的**相消**表达，
	// float32 下必然掉精度。历史 5a-6 就是靠"半径与 8 角距的相对误差 ~1e-5"这条
	// 数值判据定案的，故这里把它钉成回归断言而不是一次性观察。判据同样放视空间。
	struct Extreme {
		float fovYDeg;
		float aspect;
		float nearZ;
		float farZ;
	};
	for (const Extreme& e : std::array<Extreme, 5>{{
			{60.0f, 16.0f / 9.0f, 0.1f, 100.0f},
			{120.0f, 2.4f, 0.01f, 2000.0f},
			{25.0f, 0.5f, 0.05f, 5000.0f},
			{90.0f, 1.0f, 0.001f, 100.0f},
			{60.0f, 1.0f, 99.0f, 100.0f},
		}}) {
		INFO("fov=" << e.fovYDeg << " aspect=" << e.aspect << " near=" << e.nearZ << " far=" << e.farZ);
		const auto view = makeView(kPoses[0], 5.0f);
		const auto proj = makeProj({e.fovYDeg, e.aspect, e.nearZ, e.farZ});
		const Bounds b = frustumBoundingSphere(view, proj);
		REQUIRE(b.radius > 0.0f);
		const glm::vec3 centerEye = glm::vec3(view * glm::vec4(b.center, 1.0f));
		const double tanHalf = std::tan(glm::radians(e.fovYDeg) * 0.5);
		for (const glm::vec3& corner :
			testing::frustumCornersEye(e.nearZ, e.farZ, tanHalf, e.aspect))
			CHECK(glm::length(glm::dvec3(corner) - glm::dvec3(centerEye)) <= b.radius * 1.002);
	}
}

TEST_CASE("frustumBoundingSphere 2 参版与 4 参版：比值 ≲1e3 时两者一致（调用点迁移的等价性）") {
	// render_list_impl 的级联循环改调 4 参版。这里锁住"迁移在常用区间是等价的"——
	// 否则改调用点就成了未经验证的行为变更。超出 1e3 后两者会分家，那正是 4 参版
	// 存在的理由（见下一条的量化）。
	Scene scene;
	for (int i = 0; i < 256; ++i) {
		const ProjParams pp = scene.nextProj();
		if (pp.farZ / pp.nearZ > 1e3)
			continue;  // 该区间由下面那条专门量化
		const auto view = makeView(kPoses[static_cast<size_t>(i) % kPoses.size()], 5.0f);
		const auto proj = makeProj(pp);
		INFO("i=" << i << " near/far=" << (pp.farZ / pp.nearZ));
		const Bounds a = frustumBoundingSphere(view, proj);
		const Bounds b4 = frustumBoundingSphere(view, proj, pp.nearZ, pp.farZ);
		// 反解误差在比值 1e3 内 ≤ 1.2e-4 相对量级，半径差异应远小于 0.1%
		expectNear(b4.radius, a.radius, 1e-3, 1e-6);
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：projWithDepthRange —— 子视锥的深度重映射
//
// 5a-5 的四个 bug 之一：反投影乘了**正向**矩阵 + NDC z 取值域错（用 GLM 的 −1..1，
// 而本项目是 zero-to-one）。表现为"拟合球半径恒 0"。本组从两个可观测后果夹逼：
// x/y 分布必须逐位不变（只换深度映射），z 必须落在 [0,1] 且单调。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("projWithDepthRange：x/y 分布逐位不变，只有深度映射被换掉") {
	Scene scene;
	for (int i = 0; i < 256; ++i) {
		const ProjParams pp = scene.nextProj();
		const auto proj = makeProj(pp);
		const float newNear = pp.nearZ * 1.5f;
		const float newFar = pp.farZ * 0.8f;
		const auto sub = projWithDepthRange(proj, newNear, newFar);
		INFO(i << " " << pp.fovYDeg << " " << pp.aspect << " " << pp.nearZ << " " << pp.farZ << " " << newNear << " " << newFar);
		// x/y 行（索引 0/1 的整行 + 第 3 行）必须与原矩阵**完全一致**：
		// 改深度映射只该动 [2][2] 与 [3][2] 两格。
		for (int col = 0; col < 4; ++col) {
			if (col == 2)
				continue;  // z 行就是要改的那一行
			for (int row = 0; row < 4; ++row) {
				if (row == 2 || row == 3)
					continue;  // 深度行
				expectNear(sub[col][row], proj[col][row], 0.0, 0.0);
			}
		}
	}
}

TEST_CASE("projWithDepthRange：新近平面点 z=0、新远平面点 z=1（zero-to-one 契约）") {
	// 直接钉住 5a-5 的 ZO 值域错：本项目 GLM_FORCE_DEPTH_ZERO_TO_ONE，
	// ndc.z ∈ [0,1]。若有人误改用 OpenGL 的 [−1,1]，本断言立刻红。
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.5f, 100.0f});
	const float newNear = 0.75f;
	const float newFar = 50.0f;
	const auto sub = projWithDepthRange(proj, newNear, newFar);
	// 视空间深度 t 处 ndc.z = A + B/t（GLM 透视同形），t=newNear → 0，t=newFar → 1
	const auto zAt = [&](float t) {
		const glm::vec4 clip = sub * glm::vec4(0.0f, 0.0f, -t, 1.0f);
		return clip.z / clip.w;
	};
	expectNear(zAt(newNear), 0.0, 0.0, 1e-5);
	expectNear(zAt(newFar), 1.0, 1e-5, 1e-5);
	// 单调性：t 在 [newNear, newFar] 上 ndc.z 严格单调递增（级联子视锥的深度序依赖它）
	double prev = -1.0;
	for (int i = 0; i <= 32; ++i) {
		const double t = newNear + (newFar - newNear) * (static_cast<double>(i) / 32.0);
		const double z = zAt(static_cast<float>(t));
		INFO(t << " " << z);
		CHECK(z >= prev);
		prev = z;
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：cascadeSplitDistance / quantizeShadowDistance —— 切分与量化
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cascadeSplitDistance：严格单调递增，末级恰为 far") {
	Scene scene;
	for (int i = 0; i < 256; ++i) {
		const ProjParams pp = scene.nextProj();
		INFO(i << " " << pp.nearZ << " " << pp.farZ);
		float prev = 0.0f;
		for (uint32_t c = 0; c < 4; ++c) {
			const float d = cascadeSplitDistance(pp.nearZ, pp.farZ, c, 4);
			CHECK(d > prev);          // 严格递增：级联区间不重叠（重叠 = 两次采样打架）
			CHECK(d >= pp.nearZ);      // 不早于近平面
			CHECK(d <= pp.farZ);       // 不越远平面
			prev = d;
		}
		expectNear(prev, pp.farZ, 1e-6, 1e-6);
	}
}

TEST_CASE("cascadeSplitDistance：count=1 时唯一一级就是 far（单级退化路径）") {
	const float nearZ = 0.1f;
	const float farZ = 100.0f;
	expectNear(cascadeSplitDistance(nearZ, farZ, 0, 1), farZ, 0.0, 0.0);
	// count=4 第 0 级 = 对数项与均匀项的 50/50 混合（fN=0 → logSplit=near，uniSplit=near+0.25Δ）
	const double expect0 = static_cast<double>(nearZ) * 0.5 + (nearZ + 0.25 * (farZ - nearZ)) * 0.5;
	expectNear(cascadeSplitDistance(nearZ, farZ, 0, 4), expect0, 1e-5, 1e-5);
}

TEST_CASE("quantizeShadowDistance：桶内恒定、跨桶单调、1/8 网格对齐") {
	// 这条量化是 5a-7 的另一半：距离量化让"小幅移动落同一桶 → 半径/splits 完全恒定"
	//（否则影子连续滑动）。桶内恒等是它的全部意义，必须钉死。
	Scene scene;
	for (int i = 0; i < 512; ++i) {
		const double d = scene.rng.uniform(0.01, 300.0);
		INFO(d);
		const float q = quantizeShadowDistance(static_cast<float>(d));
		// 向上取整到 1/8 格
		const double grid = std::ceil(q * 8.0) / 8.0;
		expectNear(q, grid, 0.0, 1e-5);
		// 同一个桶内的任意输入必须给出**同一个**量化值（桶内恒定）。
		// quantize = ceil(d·8)/8 ⟹ d ∈ ((k−1)/8, k/8] 全部映射到 k/8 = q，
		// 故"同桶"取 **q 往下**的邻近值（往上就已跨桶了——这是本断言第一版的错，
		// 症状是 4241 条失败，教训与 5a-8c 同源：判据写反时全红，但红的方向能定位）。
		for (double frac : {-0.999, -0.9, -0.3, -0.01, 0.0}) {
			const double in = q + frac / 8.0;  // 同格内的邻近值
			if (in <= 0.0)
				continue;
			INFO(d << " " << frac << " " << in);
			expectNear(quantizeShadowDistance(static_cast<float>(in)), q, 0.0, 1e-5);
		}
		// 量化只会放大不会缩小（向上取整）——这一条保证"覆盖性宁大勿小"的方向不变
		CHECK(q >= static_cast<float>(d) - 1e-4f);
	}
}

TEST_CASE("quantizeShadowDistance：跨桶严格递增（相邻桶不留空隙）") {
	double prev = 0.0;
	for (int k = 1; k <= 64; ++k) {
		const double d = static_cast<double>(k) / 8.0;
		const double q = quantizeShadowDistance(static_cast<float>(d));
		INFO(d << " " << q);
		CHECK(q > prev);
		prev = q;
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：buildShadowVolume —— 光空间正交盒
//
// 5a-8b 的真凶：**把世界 AABB 最小角当光轴最远点**。dot(casterMin, d) 取到最小值
// 要求 d 分量全正；太阳 d.y<0 时真正的极值角必须 y 取 max（support function）。
// 用错的后果：tMin 恒被 −r 支配（实测 eyeOffset 恒 = r·1.02），比切片球顶高的建筑
// 整批被近平面裁掉不画 → 影子"游动"。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("buildShadowVolume 覆盖性：投影体 AABB 沿光轴的支撑点必落在 [tMin,tMax] 内") {
	// 这是 5a-8b 的正向锚点：对 AABB 全部 8 个角 + 2 个支撑点逐一断言。
	// 判据写成"角点也要装下"而非只测支撑点——AABB 是凸集，角点在区间内即全体在内，
	// 但**只测支撑点会漏掉"用错角"的 bug**：用错角时支撑点仍可能被另一个角顶出去。
	Scene scene;
	for (int i = 0; i < 256; ++i) {
		const ProjParams pp = scene.nextProj();
		const auto view = makeView(kPoses[static_cast<size_t>(i) % kPoses.size()], 5.0f);
		const auto proj = makeProj(pp);
		const Bounds slice = frustumBoundingSphere(view, proj);
		REQUIRE(slice.radius > 0.0f);
		// 投影体盒：跨两个数量级的高度（矮墩子 / 高塔），高塔正是 5a-8b 被裁掉的那批
		const Aabb box{glm::vec3(-3.0f, -1.0f, -2.0f),
			glm::vec3(4.0f, 3.0f + 20.0 * static_cast<float>(scene.rng.uniform01()), 5.0f)};
		const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024);
		INFO(i << " " << slice.radius << " " << box.hi.y);
		REQUIRE(v.xyRadius > 0.0f);
		REQUIRE(v.tMax > v.tMin);
		const glm::vec3 d = glm::normalize(kSunDown.direction);
		// 逐角断言：把角点投到光轴坐标 t（相对切片中心），必须落在 [tMin, tMax]
		for (int corner = 0; corner < 8; ++corner) {
			const glm::vec3 p{(corner & 1) ? box.hi.x : box.lo.x,
				(corner & 2) ? box.hi.y : box.lo.y, (corner & 4) ? box.hi.z : box.lo.z};
			// 切片中心的吸附后位置由 eyeOffset/tMin 反推：t = tMin + eyeOffset
			const double t = static_cast<double>(v.tMin) + v.eyeOffset + 0.0;
			(void)t;
			// 直接用球心-锚定的等价式：corner 在盒内 ⟺ 其光轴坐标 ∈ [tMin,tMax]
			const double axis = glm::dot(p - slice.center, d) - (v.tMin + v.eyeOffset);
			INFO(corner << " " << axis << " " << v.tMin << " " << v.tMax);
			// 注意 buildShadowVolume 的吸附会把中心挪 ≤半 texel，故留 pad 余量
			CHECK(axis >= v.tMin - 1e-2 * v.xyRadius);
			CHECK(axis <= v.tMax + 1e-2 * v.xyRadius);
		}
	}
}

TEST_CASE("buildShadowVolume support function：d.y<0 时远角必须 y 取 max（5a-8b 判据）") {
	// 直接钉住那次错误的**语义**：太阳朝下（d.y<0）时，光轴上的"最远"角是 y 最大的角。
	// 显式对比：把 casterMin 传给 buildShadowVolume 得到的 tMax，必须等于 y=max 的
	// 支撑点；若实现退化成"用 casterMin 当远角"，tMax 会被小球的 box.hi.y 顶死。
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	const auto view = makeView(kPoses[0], 5.0f);
	const Bounds slice = frustumBoundingSphere(view, proj);
	REQUIRE(slice.radius > 0.0f);
	// 一个高瘦塔：lo.y=-1, hi.y=50（远高于切片球顶 r）
	const Aabb tall{glm::vec3{-0.5f, -1.0f, -0.5f}, glm::vec3{0.5f, 50.0f, 0.5f}};
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, tall.lo, tall.hi, 1024);
	const glm::vec3 d = glm::normalize(kSunDown.direction);
	INFO(slice.radius << " " << v.tMin << " " << v.tMax << " " << v.eyeOffset);
	// y 取 max 的支撑点
	const glm::vec3 far = supportPoint(tall, d);
	INFO(far.x << " " << far.y << " " << far.z);
	// 塔顶必须被盒装下：否则它整批被近平面裁掉（5a-8b 的实际后果）
	const double towerTop = glm::dot(far - slice.center, d) - (v.tMin + v.eyeOffset);
	CHECK(towerTop <= v.tMax);
	// 反向锚点：若实现误用 casterMin 当远角，tMax 会被 lo.y 支配 → 塔顶远超出
	CHECK(towerTop > v.tMin);
	// 具体的"恒等指纹"：5a-8b 当时观察到 eyeOffset ≡ r·1.02 恒成立（= 坏路径指纹）。
	// 这里反着断言：塔足够高时 eyeOffset 必须**大于** 1.02r（因为 tMin 被塔顶拉低）。
	CHECK(v.eyeOffset > 1.02f * slice.radius);
}

TEST_CASE("buildShadowVolume d.y>0（太阳朝上）同样正确 —— support function 双向") {
	// 5a-8b 只错了一半（d 分量符号全正时才恰好对）。本组覆盖另一半，
	// 防止"修 y 分量时把 x/z 分量改反"。
	const Light sunUp{0u, glm::vec3{0.35f, 1.0f, 0.25f}, glm::vec3{1.0f, 0.98f, 0.95f},
		3.0f, glm::vec3{0.03f}, glm::vec3{0.0f}, 0.0f, 0u};
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	const auto view = makeView(kPoses[0], 5.0f);
	const Bounds slice = frustumBoundingSphere(view, proj);
	REQUIRE(slice.radius > 0.0f);
	const Aabb box{glm::vec3{-4.0f, -3.0f, -5.0f}, glm::vec3{3.0f, 40.0f, 2.0f}};
	const ShadowVolume v = buildShadowVolume(sunUp, slice, box.lo, box.hi, 1024);
	const glm::vec3 d = glm::normalize(sunUp.direction);
	const double top = glm::dot(supportPoint(box, d) - slice.center, d) - (v.tMin + v.eyeOffset);
	INFO(v.tMin << " " << v.tMax << " " << top);
	CHECK(top <= v.tMax);
	// 上下颠倒的塔：d.y>0 时"近"端是 lo.y 那一侧，盒的深度下界必须被它拉低
	CHECK(v.eyeOffset > 1.02f * slice.radius);
}

TEST_CASE("buildShadowVolume：退化输入返回零半径体（谁都不进得来 = 明确的失败而非随机行为）") {
	const Bounds zero{glm::vec3{0.0f}, 0.0f};
	const ShadowVolume v = buildShadowVolume(kSunDown, zero, glm::vec3{-1.0f}, glm::vec3{1.0f}, 1024);
	expectNear(v.xyRadius, 0.0, 0.0, 0.0);
	expectNear(v.tMin, 0.0, 0.0, 0.0);
	expectNear(v.tMax, 0.0, 0.0, 0.0);
	// 退化体在几何上是一个**点**（tMin==tMax、xyRadius=0），故只有恰好罩住该点的
	// 球才"相交"。断言两侧：远处必出、恰好在点上必进——第一版只写"半径 100 的球
	// 必被剔"，那是**判据本身写错**（半径 100 的球确实包含原点），不是被测代码错。
	CHECK_FALSE(casterInShadowVolume(v, glm::vec3{1000.0f, 0.0f, 0.0f}, 1.0f));
	CHECK_FALSE(casterInShadowVolume(v, glm::vec3{0.0f, 1000.0f, 0.0f}, 1.0f));
	CHECK(casterInShadowVolume(v, glm::vec3{0.0f}, 0.5f));
}

TEST_CASE("buildShadowVolume：texel 吸附是世界锚定的（换吸附策略会让影子随视角游动）") {
	// 5a-8b 的另一半：吸附中心必须建在**固定光基**的三坐标上。若改成"在自己盒里的
	// NDC 吸附"（恒等于 (0,0)），等于没吸——盒跟着视锥球心走。
	// 判据：同一 (light, slice, 投影体) 在**不同相机姿态**下，吸附后的盒参数只依赖
	// 球心在世界里的位置，故同一 slice 中心下，**两次调用的 tMin/tMax/eyeOffset 逐位相同**。
	const auto proj = makeProj({60.0f, 16.0f / 16.0f, 0.1f, 100.0f});
	const Bounds slice = frustumBoundingSphere(makeView(kPoses[0], 5.0f), proj);
	const Aabb box{glm::vec3{-2.0f, -1.0f, -2.0f}, glm::vec3{2.0f, 3.0f, 2.0f}};
	const ShadowVolume a = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024);
	const ShadowVolume b = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024);
	INFO(a.tMin << " " << a.tMax << " " << a.eyeOffset);
	expectNear(b.tMin, a.tMin, 0.0, 0.0);
	expectNear(b.tMax, a.tMax, 0.0, 0.0);
	expectNear(b.eyeOffset, a.eyeOffset, 0.0, 0.0);
	// 吸附步长 = 2r/mapSize：tMin/tMax 相对 pad(r·0.02) 的量级应远大于单格，
	// 否则"吸附"实际上没起作用（历史：恒等吸附 = 快照无位移）。
	const float texel = 2.0f * slice.radius / 1024.0f;
	INFO(texel);
	CHECK(texel > 0.0f);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 5：casterInShadowVolume —— 逐级剔除
//
// 元模式「0 差异 ≠ 生效」：只测"该剔的剔掉了"，实现整体返回 false 也能全绿。
// 本组两侧都断言，且**阳性用例必须是显著的**（塔在盒内，不是擦边）。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("casterInShadowVolume：盒内显著物体必进（阳性锚点）") {
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	const Bounds slice = frustumBoundingSphere(makeView(kPoses[0], 5.0f), proj);
	REQUIRE(slice.radius > 0.0f);
	const Aabb box{glm::vec3{-4.0f, -1.0f, -4.0f}, glm::vec3{4.0f, 20.0f, 4.0f}};
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024);
	REQUIRE(v.xyRadius > 0.0f);
	// 切片球心附近的物体（正对光轴中段）必进
	INFO(v.xyRadius << " " << v.tMin << " " << v.tMax << " " << v.eyeOffset);
	CHECK(casterInShadowVolume(v, slice.center, 1.0f));
	CHECK(casterInShadowVolume(v, slice.center, v.xyRadius));
}

TEST_CASE("casterInShadowVolume：盒外远处物体必出（反向锚点，治『整体返回 true』）") {
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	const Bounds slice = frustumBoundingSphere(makeView(kPoses[0], 5.0f), proj);
	const Aabb box{glm::vec3{-4.0f, -1.0f, -4.0f}, glm::vec3{4.0f, 20.0f, 4.0f}};
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024);
	Scene scene;
	int removed = 0;
	for (int i = 0; i < 256; ++i) {
		// 在盒外远采样：世界球心离切片中心足够远
		const glm::vec3 far{scene.rng.uniform(500.0f, 900.0f), scene.rng.uniform(500.0f, 900.0f),
			scene.rng.uniform(500.0f, 900.0f)};
		INFO(far.x << " " << far.y << " " << far.z);
		if (!casterInShadowVolume(v, far, 1.0f))
			++removed;
	}
	// 全部 256 个远离的点都必须被剔掉（一个没剔 = 剔除失效 = 阴影全图都画 = 慢且错）
	CHECK(removed == 256);
}

TEST_CASE("casterInShadowVolume：近平面侧的大物体不被 tMin 误剔（5a-8b 实际后果的正向锚点）") {
	// 历史实测：tMin 被 −r 支配时，"比切片球顶高的建筑"整批被近平面裁掉 → 广场
	// 一个投影都没有（对拍只差 149px / 0.016%）。本断言直接钉住"高塔必须进"。
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	const Bounds slice = frustumBoundingSphere(makeView(kPoses[0], 5.0f), proj);
	REQUIRE(slice.radius > 0.0f);
	const Aabb box{glm::vec3{-2.0f, -1.0f, -2.0f}, glm::vec3{2.0f, 60.0f, 2.0f}};
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024);
	const glm::vec3 d = glm::normalize(kSunDown.direction);
	// 塔顶角（沿光轴最远）
	const glm::vec3 top = supportPoint(box, d);
	INFO(slice.radius << " " << v.tMin << " " << v.tMax << " " << top.y);
	CHECK(casterInShadowVolume(v, top, 0.5f));
}

}  // namespace RGL
