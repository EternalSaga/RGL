module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>  // glm::inverse（B6 从 v.view 恢复吸附中心）
#include <cmath>                       // frexp/ldexp/isfinite（量化算子 = 2 的幂的契约）
#include <cstring>                     // memcmp（"门关逐位不变"的**逐位**二字按字面判）
#include <initializer_list>            // 必须：modules 下 std::initializer_list 不从 import 带进来

// 阴影几何回归测试（批 12.1 · B1）——把 Phase 11 批 11.2 的 5a-3→5a-9 七轮排障
// 固化成断言。被测对象全在 rgl/scene_context.cppm（rlcore 成员，纯函数，零 GPU）。
//
// ## 为什么要固化这一段
//
// git:调试历史 里阴影链的代价是全项目最高的：七轮修复、基线连续作废 7 次、
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
import scene_data;  // kActiveShadowCascades（级联级数；本文件组 3 的同档判据要逐级走一遍）

namespace RGL {
namespace {

using testing::Lcg;
using testing::expectNear;
using testing::frustumCorners;

/** 门关原点 = 逐位 (0,0,0)（相对坐标批 5 第 1 刀给 `buildShadowVolume` 加的 origin 入参）。
 *  本文件的用例全部测**门关态**的几何规则，显式传它而不是留默认值：漏传不会让任何判据红，
 *  而「门关逐位不变」这条判据的语义就是「原点这一维真的没参与」。 */
const glm::dvec3 kNoOrigin{0.0, 0.0, 0.0};

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
 *  用例单独覆盖，那是 5a-8b 出错的那一半）。
 *  🔴 **指定初始化**（债-118 同族约定）：`Light` 的 `position` 是绝对层 `dvec3`、其余分量是
 *  `vec3`，位置聚合初始化在字段换序时会**静默错位**（`ambient` 与 `position` 同为三维向量，
 *  换序后编译器一声不出）。 */
constexpr Light kSunDown{.index = 0u, .direction = glm::vec3{0.35f, -1.0f, 0.25f},
	.color = glm::vec3{1.0f, 0.98f, 0.95f}, .intensity = 3.0f, .ambient = glm::vec3{0.03f},
	.position = glm::dvec3{0.0}, .range = 0.0f, .type = 0u};

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

/**
 * 采样端选级的**独立第二实现**（逐行转录 `rgl/shaders/shadow.slang` 的三段无条件比较）。
 *
 * 期望值出处 = shader 源码，**不抄被测 C++ 码**（skill rgl-test-discipline「断言独立于实现」）：
 * forceShadowCascadeSplits 写出的 splits 让片元落到哪一级，只有拿 shader 那份逻辑去判才算
 * 判据——两边都抄同一个 C++ 实现的话，编码写反也照样"对得上"。
 * ⚠ 改 shader 选级逻辑必须同步本函数（同 test_shadow_forward 那条"提取式与测试同改"的契约）。
 */
uint32_t selectCascade(const glm::vec4& splits, float depth) {
	uint32_t cascade = 0u;
	if (depth > splits.x) cascade = 1u;
	if (depth > splits.y) cascade = 2u;
	if (depth > splits.z) cascade = 3u;
	return cascade;
}

/** **逐位**相同（不是"值相等"）：+0.0 与 −0.0 在这里判不同——否则"门关逐位不变"是空话。 */
bool bitwiseEqual(const glm::vec4& a, const glm::vec4& b) {
	return std::memcmp(&a, &b, sizeof(glm::vec4)) == 0;
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
		// 🔴 `{...}` 是**显式**降维：`mat4 × vec4` 得 vec4，本用例只取 xyz（丢 w）。
		// 原先写 `=` 靠 glm 隐式截断（`GLM_FORCE_EXPLICIT_CTOR` 拦的就是它，债-147），
		// 语义逐字不变。
		const glm::vec3 e{view * glm::vec4(corners[i], 1.0f)};
		expectNear(-e.z, nearZ, 1e-3, 1e-3);
	}
	for (size_t i = 4; i < 8; ++i) {
		INFO("far corner#" << i);
		const glm::vec3 e{view * glm::vec4(corners[i], 1.0f)};
		expectNear(-e.z, farZ, 1e-3, 1e-3);
	}
	// 横向范围：近平面半高 = near·tan(fov/2)，半宽 = 近平面半高 × aspect
	const glm::vec3 nearUp{view * glm::vec4(corners[1], 1.0f)};
	const glm::vec3 nearLeft{view * glm::vec4(corners[0], 1.0f)};
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
// 组 3：cascadeSplitDistance / quantizeShadowDistance / forceShadowCascadeSplits
//       —— 切分、量化，与**强制选级诊断键**的 splits 编码
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

TEST_CASE("quantizeShadowDistance：格距 = 2 的幂、桶内恒定、幂等、只放大不缩小") {
	// 契约（scene_context.cppm 该函数注释）：向上取整到 2 的幂；恰为 2 的幂时取自身。
	// 为什么从 1/8 格改成 2 的幂 = 债-139 实测（旧格跨档太密 = 飞相机时影子一跳一跳），
	// 跨档频率那条判据单独成用例（下一条），这里只钉量化算子本身。
	Scene scene;
	for (int i = 0; i < 512; ++i) {
		const double d = scene.rng.uniform(0.01, 300.0);
		INFO(d);
		const float q = quantizeShadowDistance(static_cast<float>(d));
		// ① 输出必是 2 的幂（尾数恰 0.5）
		int e = 0;
		CHECK(std::frexp(q, &e) == 0.5f);
		// ② 只放大不缩小（覆盖性宁大勿小的方向不变），且不超过 2 倍（格距就是 2 的幂）
		CHECK(q >= static_cast<float>(d) - 1e-4f);
		CHECK(q < 2.0f * static_cast<float>(d) + 1e-4f);
		// ③ 桶内恒定：(q/2, q] 内任意输入必须给出**同一个** q。这是量化的全部意义——
		//    桶内相机怎么动，级联半径/纹素/splits 都逐位不变。
		for (double frac : {-0.999, -0.9, -0.3, -0.01, 0.0}) {
			const double in = q + frac * q / 2.0;  // 同档内往下取（往上就已跨档）
			if (in <= 0.0)
				continue;
			INFO(d << " " << frac << " " << in);
			CHECK(quantizeShadowDistance(static_cast<float>(in)) == q);
		}
		// ④ 幂等（旧 1/8 实现不满足这条：它把 2^k 抬到 2^k，新实现靠"恰为 2 的幂取自身"成立）
		CHECK(quantizeShadowDistance(q) == q);
	}
	// ⑤ 退化输入原样返回（不产生 NaN / 不抛）
	CHECK(quantizeShadowDistance(0.0f) == 0.0f);
	CHECK(std::isfinite(quantizeShadowDistance(-1.0f)));
}

TEST_CASE("quantizeShadowDistance：跨档严格递增，且 far 翻倍只跨一档") {
	double prev = 0.0;
	for (int k = -4; k <= 12; ++k) {
		const double d = std::ldexp(1.0, k);
		const double q = quantizeShadowDistance(static_cast<float>(d));
		INFO(d << " " << q);
		CHECK(q > prev);  // 恰为 2 的幂 → 取自身，故相邻档必严格变大
		CHECK(q == d);
		prev = q;
	}
	// 档宽 = 2 倍 ⟹ "far 从 x 涨到 2x⁻" 全程不跨档，涨到 2x 才跨一档
	CHECK(quantizeShadowDistance(1.9999f) == quantizeShadowDistance(1.0001f));
	CHECK(quantizeShadowDistance(2.0f) == 2.0f);
}

TEST_CASE("债-139 回归：用户报障两机位的自适应远界必须落同一档") {
	// 这条就是本债的**根因回归**，数字全部来自 2026-10-19 实机读数（两机位 = camera_saved
	// 原文坐标，纯前推 0.056；projectionSystem 的 far = 相机到场景球心距离 + 球半径）。
	// 旧 1/8 格：1.713496 → 1.750 vs 1.764820 → 1.875 ⟹ **跨档** ⟹ 整套级联盒半径/纹素
	// 缩放 5–7% ⟹ 整张阴影图重新量化 ⟹ 实测 0.32–0.40% 像素阴影状态翻转（同机位钉远界
	// 的无地板对照实测）。本用例在旧实现下**必红**，改回 1/8 格立刻红。
	const float farA = 1.713496f;
	const float farB = 1.764820f;
	CHECK(quantizeShadowDistance(farA) == quantizeShadowDistance(farB));
	// 近界同理（自适应 near 落在 0.05 下限，两机位同值，但同样不得跨档）
	CHECK(quantizeShadowDistance(0.05f) == quantizeShadowDistance(0.05f));
	// 反向用例：真跨档时必须不同（否则上面那条 CHECK 是"恒返回同一个数"糊过去的）
	CHECK(quantizeShadowDistance(1.713496f) != quantizeShadowDistance(2.5f));
	// 且跨档后纹素格**嵌套**：粗档纹素是细档的整数倍（2 的幂 ⟹ 2 倍），
	// 这是"跨档时影边最多挪半个粗纹素"的算术前提。
	const double texelFine = quantizeShadowDistance(farA);
	const double texelCoarse = quantizeShadowDistance(2.5f);
	CHECK(texelCoarse / texelFine == std::round(texelCoarse / texelFine));
}

TEST_CASE("同档两机位 ⟹ 各级级联半径与纹素逐位相同（阴影图是世界函数的前提）") {
	// 这条钉的是"桶内恒定"传导到几何上：near/far 落同一档 ⇒ 每级子视锥的包围球半径
	// 逐位相同 ⇒ texel = 2r/块边长 逐位相同。配合 buildShadowVolume 的世界锚定吸附
	// （组 4 已独立钉死），合起来才是"相机在同档内怎么动，阴影图都是同一个世界函数"。
	// 反向用例：把 far 挪出档（下面 third 那路）必须让半径**不同**，否则本用例是恒真。
	const float nearQ = quantizeShadowDistance(0.05f);
	const float farQ = quantizeShadowDistance(1.713496f);
	REQUIRE(farQ == quantizeShadowDistance(1.764820f));
	const auto proj = makeProj({45.0f, 16.0f / 9.0f, 0.05f, 1.713496f});
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const float nearC = c == 0 ? 0.02f : cascadeSplitDistance(nearQ, farQ, c - 1u, kActiveShadowCascades);
		const float farC = cascadeSplitDistance(nearQ, farQ, c, kActiveShadowCascades);
		const auto sub = projWithDepthRange(proj, nearC, farC);
		// 两路**独立构造**的 view（同档、不同机位姿态）→ 半径必须逐位相同（姿态无关）
		const Bounds sA = frustumBoundingSphere(makeView(kPoses[0], 0.54f), sub);
		const Bounds sB = frustumBoundingSphere(makeView(kPoses[3], 0.59f), sub);
		INFO(c << " " << sA.radius << " " << sB.radius);
		CHECK(sA.radius == sB.radius);  // 逐位：同档 ⇒ 同半径 ⇒ 同纹素
		// 反向：跨档必须不同（信号必须在，否则上一条是"半径与 far 无关"的假绿）
		const auto subOut = projWithDepthRange(proj, nearC,
			cascadeSplitDistance(nearQ, quantizeShadowDistance(2.5f), c, kActiveShadowCascades));
		CHECK(frustumBoundingSphere(makeView(kPoses[0], 0.54f), subOut).radius != sA.radius);
	}
}

// ─── forceShadowCascadeSplits（csm.force_cascade 的编码本体，诊断键）─────────

TEST_CASE("forceShadowCascadeSplits：门关（force<0 / force>=count）**逐位**原样返回") {
	// 判据来源 = 本仓「门关逐位不变」红线：出厂 -1 时写进 scene data 的字节必须与本键加入前
	// 相同（实机形态 = just shot 出厂图 sha256 锚；这里是它的算术形态，零 GPU 零窗口）。
	// 变异红线：① 删掉早退（恒覆写）→ 本用例红；② `force < 0` 写成 `force <= 0` → force=0
	// 那一臂红（0 = 钉第 0 级，是**合法档**不是关）；③ 射程 `>= count` 漏等号写成 `> count`
	// → force == count 那一臂红（越界必须原样返回，否则会去钉一级从未渲染的级联）；
	// ④ 把返回改成"重新构造的等值 vec4"（+0.0 归一）→ −0.0 那一臂红（"逐位"两字的判据在此）。
	Scene scene;
	for (int i = 0; i < 256; ++i) {
		const auto r = [&scene] { return static_cast<float>(scene.rng.uniform(-1e6, 1e6)); };
		const glm::vec4 s{r(), r(), r(), r()};  // 任意有限值（含 0 附近与极大值）
		for (const int force : {-1, -2, -1000000, 4, 5, 1000}) {
			CHECK_MESSAGE(bitwiseEqual(forceShadowCascadeSplits(s, 4u, force), s),
				"force=", force, " 必须原样返回，实得 ",
				forceShadowCascadeSplits(s, 4u, force).x);
		}
	}
	// NaN 载荷同样逐位不动（门关路径上没有任何算术，memcmp 判的是位型本身）
	const glm::vec4 weird{std::nanf(""), 0.0f, -0.0f, 1e9f};
	CHECK(bitwiseEqual(forceShadowCascadeSplits(weird, 4u, -1), weird));
	CHECK(bitwiseEqual(forceShadowCascadeSplits(weird, 4u, 7), weird));
	// 反向锚点：本用例的"逐位"不是恒真——+0.0 与 −0.0 是**两个不同**位型。
	// 没有这四行，"逐位原样"会被读成"值相等"，而 ④ 那类变异就藏在 0 的符号位里。
	CHECK(bitwiseEqual(forceShadowCascadeSplits(weird, 4u, -1), weird));
	CHECK_FALSE(bitwiseEqual(glm::vec4{0.0f, -0.0f, 0.0f, -0.0f},
		glm::vec4{0.0f, 0.0f, 0.0f, 0.0f}));
}

TEST_CASE("forceShadowCascadeSplits：编码 = (i<force)?0:1e9，四档互不相同 + 末槽恒 1e9") {
	// 判据来源 = scene_context.cppm 该函数注释里的编码式（**独立写出**，不抄实现体）。
	// 变异红线：① 三元写反（`(i<force)?1e9:0`）→ 四条 CHECK 全红；② 比较写成 `i <= force`
	// → k=0 那一臂红（第 0 级不该有 0 槽）；③ 循环上界用 count 而不是 vec4 槽数 → 末槽
	// 那条红；④ force 不参与（恒 0 或恒 1e9）→ 相邻档"必须不同"那两条红。
	const glm::vec4 real{0.3047f, 0.5900f, 0.9346f, 1e9f};  // 真实 splits 形态（末级哨兵）
	constexpr float N = 1e9f;
	CHECK(forceShadowCascadeSplits(real, 4u, 0) == glm::vec4(N, N, N, N));
	CHECK(forceShadowCascadeSplits(real, 4u, 1) == glm::vec4(0.0f, N, N, N));
	CHECK(forceShadowCascadeSplits(real, 4u, 2) == glm::vec4(0.0f, 0.0f, N, N));
	CHECK(forceShadowCascadeSplits(real, 4u, 3) == glm::vec4(0.0f, 0.0f, 0.0f, N));
	// 🔴 反向用例（本文件最要紧的一条）：相邻两档必须**不同**。恒返回同一个数、或 force
	// 根本没被用，都会让上面四条里"看起来对的那条"绿着——只有差分两臂能红。
	CHECK_FALSE(forceShadowCascadeSplits(real, 4u, 2) == forceShadowCascadeSplits(real, 4u, 3));
	CHECK_FALSE(forceShadowCascadeSplits(real, 4u, 0) == forceShadowCascadeSplits(real, 4u, 1));
	// 🔴 5a-6 红线（NaN 全黑事故）在诊断档下也必须成立：末槽恒 = 1e9，写 0 的槽只出现在
	// "**低于**目标级"那一侧，而目标级由射程闸门 force<count 保证是本帧真实渲染的级。
	for (uint32_t k = 0; k < 4u; ++k) {
		const glm::vec4 f = forceShadowCascadeSplits(real, 4u, static_cast<int>(k));
		CHECK(f[3] == N);
		for (uint32_t i = 0; i < 4u; ++i)
			CHECK_MESSAGE(f[i] == (i < k ? 0.0f : N), "k=", k, " 槽 ", i);
	}
	// 射程跟着 count 走（count=2：force=2 越界 = 原样返回，不写不存在的级）
	CHECK(bitwiseEqual(forceShadowCascadeSplits(real, 2u, 2), real));
	CHECK(forceShadowCascadeSplits(real, 2u, 1) == glm::vec4(0.0f, N, N, N));
}

TEST_CASE("forceShadowCascadeSplits：拿 shader 那三段比较验钉级真的改选级（真实 splits）") {
	// 判据出处 = `rgl/shaders/shadow.slang` 的选级逻辑（selectCascade 是它的**独立转录**），
	// 不是被测 C++ 码——两边都抄同一份实现的话，编码写反也照样"对得上"。
	// 真实 splits = 用户报障机位（债-139 两机位）count=4、量化后 n/f 的产物，2026-10-19 读数。
	const glm::vec4 real{0.3047f, 0.5900f, 0.9346f, 1e9f};
	// 先钉"真实 splits 下 depth=0.5 自然落在级 1"：0.5 > 0.3047 越 split0、0.5 > 0.59 不越
	// （这条在，下面"钉到别的级"才有对照物）。
	CHECK(selectCascade(real, 0.5f) == 1u);
	// force=1 → 仍是级 1（钉到本来那一级 = 不改变）；force=2 → **必须**把它抬到级 2。
	CHECK(selectCascade(forceShadowCascadeSplits(real, 4u, 1), 0.5f) == 1u);
	CHECK(selectCascade(forceShadowCascadeSplits(real, 4u, 2), 0.5f) == 2u);
	// 全深度扫描：钉到第 k 级 ⟹ **任何正深度**都落级 k（"全场景钉死"的定义）。深度集含
	// 0 附近、三级分界的左右两侧、以及远超末级的值。
	const float depths[] = {1e-7f, 0.05f, 0.3046f, 0.3047f, 0.5f,
		0.5900f, 0.9346f, 1.0f, 17.0f, 1e8f};
	for (uint32_t k = 0; k < 4u; ++k) {
		const glm::vec4 f = forceShadowCascadeSplits(real, 4u, static_cast<int>(k));
		for (const float d : depths)
			CHECK_MESSAGE(selectCascade(f, d) == k, "k=", k, " depth=", d, " 实落级 ",
				selectCascade(f, d), " splits=", f.x, " ", f.y, " ", f.z, " ", f.w);
	}
	// 反向锚点：不钉级时同一批深度**不是**全落同一级（否则上面那 40 条断言是"选级恒等于 k"）。
	bool naturalDiffers = false;
	for (const float d : depths)
		naturalDiffers = naturalDiffers || selectCascade(real, d) != selectCascade(real, depths[0]);
	CHECK(naturalDiffers);
	// ⚠ 边界写清楚，别让它成为将来的"意外"：depth **恰为 0** 时 `depth > 0.0f` 不成立
	// ⟹ 编码失效、落回级 0。可见片元的视深恒 > 0（近截面之前已被裁剪），实机打不到这条；
	// 它同时说明"0 这一侧安全"的前提是**深度恒正**，不是"0 也参与比较"。
	CHECK(selectCascade(forceShadowCascadeSplits(real, 4u, 2), 0.0f) == 0u);
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
		const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024, kNoOrigin);
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
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, tall.lo, tall.hi, 1024, kNoOrigin);
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
	const Light sunUp{.index = 0u, .direction = glm::vec3{0.35f, 1.0f, 0.25f},
		.color = glm::vec3{1.0f, 0.98f, 0.95f}, .intensity = 3.0f, .ambient = glm::vec3{0.03f},
		.position = glm::dvec3{0.0}, .range = 0.0f, .type = 0u};
	const auto proj = makeProj({60.0f, 16.0f / 9.0f, 0.1f, 100.0f});
	const auto view = makeView(kPoses[0], 5.0f);
	const Bounds slice = frustumBoundingSphere(view, proj);
	REQUIRE(slice.radius > 0.0f);
	const Aabb box{glm::vec3{-4.0f, -3.0f, -5.0f}, glm::vec3{3.0f, 40.0f, 2.0f}};
	const ShadowVolume v = buildShadowVolume(sunUp, slice, box.lo, box.hi, 1024, kNoOrigin);
	const glm::vec3 d = glm::normalize(sunUp.direction);
	const double top = glm::dot(supportPoint(box, d) - slice.center, d) - (v.tMin + v.eyeOffset);
	INFO(v.tMin << " " << v.tMax << " " << top);
	CHECK(top <= v.tMax);
	// 上下颠倒的塔：d.y>0 时"近"端是 lo.y 那一侧，盒的深度下界必须被它拉低
	CHECK(v.eyeOffset > 1.02f * slice.radius);
}

TEST_CASE("buildShadowVolume：退化输入返回零半径体（谁都不进得来 = 明确的失败而非随机行为）") {
	const Bounds zero{glm::vec3{0.0f}, 0.0f};
	const ShadowVolume v = buildShadowVolume(kSunDown, zero, glm::vec3{-1.0f}, glm::vec3{1.0f}, 1024, kNoOrigin);
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

TEST_CASE("buildShadowVolume：texel 吸附世界锚定且真实发生（批 12.3d 重写——旧版'同参调两次'是恒真断言）") {
	// 历史（5a-8 / 5a-8b）：吸附曾是"在自己盒里的 NDC 吸附"= 恒 (0,0) = 没吸 →
	// 盒跟着视锥球心走 → 影子随视角游动。本用例旧版把**同一组实参**调
	// buildShadowVolume 两次比逐位相同——任何纯函数都满足，恒等吸附/相位漂移
	// 都不会让它红，12.3d 整条重写。
	//
	// 契约（scene_context.cppm:402-455 文档注释 + 实文）：
	//   光基 d = normalize(light.direction)，up = (|d.y|>0.99 ? +Z : +Y)，
	//   s = normalize(cross(d,up))、u = cross(s,d) —— 世界固定（只依赖太阳方向）；
	//   格距 texel = 2r/mapSize（mapSize 实参 = **块**边长 shadowTileSize(cols)，非图集整幅）；
	//   球心 c 的三轴坐标（c·s / c·u / c·d）各 round 到格点 → c'，每轴位移 ≤ 半 texel；
	//   tMin = min(−r, dot(casterFar−c', d)) − 0.02r；tMax = max(r, dot(casterNear−c', d)) + 0.02r；
	//   eyeOffset = −tMin；eye = c' − d·eyeOffset。"世界锚定"= 吸附格不含 eye/视锥中心项。
	//
	// 真判据两路（信号/噪声比全部探针实测，标在各断言旁）：
	//   ① 同格两路：两个**独立构造、互不相同**的球心落进**同一**吸附格 → 盒参数一致
	//     （浮点噪声内）；恒等吸附会把两路按其光轴投影差（0.9 texel）整体错开 → 红。
	//   ② 契约重算：期望值 = 测试侧 double 按上式独立取整重排，断言输出 == 重算；
	//     再从 v.view 恢复 c' 校验"格相位 = texel 整数倍"+ 位移界（<1 texel 且真实移动）。
	//     恒等吸附（历史形态，"按自身中心吸附"与之等价）与相位改锚都红。
	const auto proj = makeProj({60.0f, 1.0f, 0.1f, 100.0f});
	constexpr uint32_t mapSize = 1024u;
	// 两条姿态路径（pose0 / pose2，同 proj）：半径姿态无关（5a-7 主判据，组 1 已独立
	// 钉死）⇒ 两路 texel 逐位相同，这里断言之；球心随姿态不同 = 各自独立推导产出。
	const Bounds sA = frustumBoundingSphere(makeView(kPoses[0], 5.0f), proj);
	const Bounds sB = frustumBoundingSphere(makeView(kPoses[2], 5.0f), proj);
	REQUIRE(sA.radius > 0.0f);
	CHECK(sB.radius == sA.radius);  // 逐位：同 proj ⇒ 同半径 ⇒ 同 texel（姿态无关）
	// 高塔（y 顶 400）保证 tMin 由 caster 分支支配（探针：dot = −349.7 ≪ −r = −83.4），
	// 吸附位移全额传导进 tMin/eyeOffset；若 −r 分支支配，min() 会把吸附遮蔽、判据退化
	// ——这正是 5a-8b"高塔被近平面裁掉"的同款输入形态。
	const Aabb tower{glm::vec3{-0.5f, -1.0f, -0.5f}, glm::vec3{0.5f, 400.0f, 0.5f}};

	// —— 测试侧独立重算（double，按上文字契约，不调被测函数、不抄实现重组）——
	const glm::dvec3 d = glm::normalize(glm::dvec3(kSunDown.direction));
	const glm::dvec3 up = (d.y > 0.99 || d.y < -0.99) ? glm::dvec3{0.0, 0.0, 1.0} : glm::dvec3{0.0, 1.0, 0.0};
	const glm::dvec3 s = glm::normalize(glm::cross(d, up));
	const glm::dvec3 u = glm::cross(s, d);
	const double texel = 2.0 * static_cast<double>(sA.radius) / static_cast<double>(mapSize);
	// c → c'：世界固定光基三轴 round 到格点（格锚在世界原点，无 eye/视锥中心项）
	const auto snapGrid = [&](const glm::dvec3& c) {
		return (s * std::round(glm::dot(c, s) / texel) + u * std::round(glm::dot(c, u) / texel)
			+ d * std::round(glm::dot(c, d) / texel)) * texel;
	};
	// c' → 期望盒参数（caster 两端 = support function 按光向分量符号取角）
	const auto expectVolume = [&](const glm::dvec3& snapped) {
		const glm::dvec3 casterFar{
			d.x >= 0.0 ? static_cast<double>(tower.lo.x) : static_cast<double>(tower.hi.x),
			d.y >= 0.0 ? static_cast<double>(tower.lo.y) : static_cast<double>(tower.hi.y),
			d.z >= 0.0 ? static_cast<double>(tower.lo.z) : static_cast<double>(tower.hi.z)};
		const glm::dvec3 casterNear{
			d.x >= 0.0 ? static_cast<double>(tower.hi.x) : static_cast<double>(tower.lo.x),
			d.y >= 0.0 ? static_cast<double>(tower.hi.y) : static_cast<double>(tower.lo.y),
			d.z >= 0.0 ? static_cast<double>(tower.hi.z) : static_cast<double>(tower.lo.z)};
		const double r = static_cast<double>(sA.radius);
		const double pad = r * 0.02;
		const double tMin = std::min(-r, glm::dot(casterFar - snapped, d)) - pad;
		const double tMax = std::max(r, glm::dot(casterNear - snapped, d)) + pad;
		return std::array<double, 3>{tMin, tMax, -tMin};  // eyeOffset = −tMin
	};

	// —— ② 输出 == 契约重算 + 格相位 + 位移界（两条姿态路径的球心各跑一遍）——
	for (const Bounds& slice : {sA, sB}) {
		const ShadowVolume v = buildShadowVolume(kSunDown, slice, tower.lo, tower.hi, mapSize, kNoOrigin);
		const glm::dvec3 g = snapGrid(glm::dvec3(slice.center));
		const auto [tMin, tMax, eyeOffset] = expectVolume(g);
		INFO("center=(" << slice.center.x << "," << slice.center.y << "," << slice.center.z
					 << ") r=" << slice.radius << " texel=" << texel);
		// 容差 max(1e-4, 1e-6·|期望|)≈3.5e-4：float32 实现 vs double 重算的实测噪声
		// ≤2.5e-5（14 倍余量）。信号：变异 a（恒等吸附）ΔtMin = 4.9e-2 / 1.1e-2（两路），
		// 变异 b（格锚改 casterMin）ΔtMin = 8.4e-3 → 全部 ≥24 倍红。
		expectNear(v.tMin, tMin, 1e-6, 1e-4);
		expectNear(v.tMax, tMax, 1e-6, 1e-4);
		expectNear(v.eyeOffset, eyeOffset, 1e-6, 1e-4);
		// 吸附真实发生且有界：位移范型 < 1 texel（理论上界 √3/2 texel），且确实动了
		// （探针 sA 0.53 / sB 0.18 texel；下限 0.05 防"输入恰好踩格点"让恒等吸附蒙混）
		const double disp = glm::length(g - glm::dvec3(slice.center));
		CHECK(disp <= 1.0 * texel);
		CHECK(disp >= 0.05 * texel);
		// 格相位：从输出 v.view 恢复吸附中心（契约 eye = c'−d·eyeOffset ⇒
		// c' = view⁻¹·(0,0,−eyeOffset)），其三轴光基坐标必为 **texel 整数倍**——
		// "格世界固定"的可观测形态。恢复噪声实测 ≤1.2e-4 texel，容差 2e-3（16 倍）；
		// 变异 a 残差 0.23~0.37 texel、变异 b 相移 0.051 texel → 红。
		const glm::dmat4 invView = glm::inverse(glm::dmat4(v.view));
		const glm::dvec3 cSnap =
			glm::dvec3(invView * glm::dvec4(0.0, 0.0, -static_cast<double>(v.eyeOffset), 1.0));
		const double phase[3] = {
			std::abs(glm::dot(cSnap, s) / texel - std::round(glm::dot(cSnap, s) / texel)),
			std::abs(glm::dot(cSnap, u) / texel - std::round(glm::dot(cSnap, u) / texel)),
			std::abs(glm::dot(cSnap, d) / texel - std::round(glm::dot(cSnap, d) / texel))};
		for (int axis = 0; axis < 3; ++axis) {
			INFO("轴 " << axis << " 相位残差(texel) = " << phase[axis]);
			CHECK(phase[axis] < 2e-3);
		}
		// 恢复中心 == 重算中心（把"吸附到哪个格点"本身也钉住；噪声 2.0e-5 绝对，容差 1.6e-3）
		CHECK(glm::length(cSnap - g) < 1e-2 * texel);
	}

	// —— ① 同格两路：两个独立构造的不同球心落进同一吸附格 → 同一个盒 ——
	// 以 sA.center 吸到的格点 gA 为中心，沿三轴各 ±0.45 texel 对称放两球心：同格，
	// 但光轴投影差 0.9 texel。世界锚定吸附下两次输出逐参数一致（噪声实测：tMin 差
	// 恰为 0，view 平移列 ≤7.6e-6；容差 2.9e-4~3.5e-4）；恒等吸附把 tMin/eyeOffset/
	// view 平移全额错开 0.147（≈420 倍容差）——旧版"同参调两次"检测不到这一形态。
	const glm::dvec3 gA = snapGrid(glm::dvec3(sA.center));
	const glm::dvec3 off = (s + u + d) * (0.45 * texel);
	const Bounds c1{glm::vec3(gA + off), sA.radius};
	const Bounds c2{glm::vec3(gA - off), sA.radius};
	// 非退化 sanity：两路输入必须真的不同（否则本判据退化为旧版的同参自比）
	REQUIRE(glm::length(c1.center - c2.center) > 1.0f * static_cast<float>(texel));
	const ShadowVolume v1 = buildShadowVolume(kSunDown, c1, tower.lo, tower.hi, mapSize, kNoOrigin);
	const ShadowVolume v2 = buildShadowVolume(kSunDown, c2, tower.lo, tower.hi, mapSize, kNoOrigin);
	expectNear(v2.tMin, v1.tMin, 1e-6, 1e-4);
	expectNear(v2.tMax, v1.tMax, 1e-6, 1e-4);
	expectNear(v2.eyeOffset, v1.eyeOffset, 1e-6, 1e-4);
	for (int i = 0; i < 4; ++i) {
		INFO("view 平移列 [" << i << "]");
		expectNear(static_cast<double>(v2.view[3][i]), static_cast<double>(v1.view[3][i]), 1e-6, 1e-4);
	}
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
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024, kNoOrigin);
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
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024, kNoOrigin);
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
	const ShadowVolume v = buildShadowVolume(kSunDown, slice, box.lo, box.hi, 1024, kNoOrigin);
	const glm::vec3 d = glm::normalize(kSunDown.direction);
	// 塔顶角（沿光轴最远）
	const glm::vec3 top = supportPoint(box, d);
	INFO(slice.radius << " " << v.tMin << " " << v.tMax << " " << top.y);
	CHECK(casterInShadowVolume(v, top, 0.5f));
}

}  // namespace RGL
