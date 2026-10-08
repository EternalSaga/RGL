module;
#include <doctest/doctest.h>  // doctest 宏声明（实现见 test_main.cppm；GMF 收敛：第三方头）
#include <glm/glm.hpp>        // glm 值类型（rlstd 不导出 glm；测试 TU 与引擎同形，GMF 直接进）
#include <glm/gtc/matrix_transform.hpp>  // 测试内构造 view/proj（与 scene_context 同款入口）
#include <glm/gtc/matrix_inverse.hpp>   // glm::inverse（反投影 NDC→世界，见 frustumCorners）

// 测试支撑模块（tests/test_support.cppm）——测试代码也走 C++20 modules，与项目同形状。
//
// 三块内容：
//   ① 共享断言（expectNear）——失败信息必须能独立定位问题，历史教训元模式「验证工具
//      自己错」：工具错时会把 FAIL 读成 PASS，所以实际值/期望值/容差全部打在明面上，
//      读者不必信任任何黑箱。
//   ② 确定性采样器（LCG）——元模式「验证工具自己错」的另一半：随机测试必须可复现，
//      否则失败无法重跑，且"上次跑过"不等于"这次验的是同一组输入"。
//   ③ 视锥角点工具——阴影几何测试反复需要"8 个视锥角点"，集中一处避免每个测试
//      各写一遍（各写一遍 = 各错一遍）。
//
// **不要在本模块 define DOCTEST_CONFIG_IMPLEMENT**：实现只能有一个 TU（test_main），
// 这里只放断言函数与采样器。
export module test_support;

import rlstd;

namespace RGL::testing {

/**
 * 浮点近似相等断言（相对 + 绝对双阈）。
 *
 * **为什么不直接用 doctest 的 Approx**：历史教训元模式 #6——验证工具自己错时会
 * 把 FAIL 读成 PASS。这里把"实际值/期望值/相对误差/绝对阈值"全部打出来，判据
 * 摆在明面上，读者不必信任任何黑箱。
 *
 * @param actual 实测值
 * @param expected 期望值
 * @param relTol 相对容差（默认 1e-4：GLM 的 float32 路径，实测相对误差 ~1e-5 量级）
 * @param absTol 绝对容差（默认 0 = 纯相对；期望值接近 0 时必须给绝对阈）
 */
export inline void expectNear(double actual, double expected, double relTol = 1e-4,
							  double absTol = 0.0) {
	const double diff = actual - expected;
	const double scale = std::max({std::abs(expected), 1e-12});
	const bool ok = std::abs(diff) <= std::max(absTol, relTol * scale);
	INFO("actual=" << actual << " expected=" << expected << " diff=" << diff
				 << " rel=" << (std::abs(diff) / scale) << " relTol=" << relTol
				 << " absTol=" << absTol);
	CHECK(ok);
}

/**
 * 确定性 LCG 采样器（Numerical Recipes 参数：a=1664525, c=1013904223, m=2^32）。
 *
 * **为什么不用 `std::random_device` / `rand()`**：属性测试的价值在于"同一组输入
 * 永远给出同一结论"。真随机的失败只能靠"再跑一次看看还崩不崩"（而它可能这次
 * 就过了 = 假绿），且无法在 CI 上复现。固定种子的 LCG 让失败可重放，也让"这个
 * 断言在本周的 512 组输入下全过"这句话有意义。
 *
 * 用法：`Lcg rng(12345); rng.uniform(-1,1);` —— 种子写死在测试里，**不要**取
 * 时间/地址。
 */
export class Lcg {
  public:
	/** @param seed 初值（测试里写死具体数字，见各 TEST_CASE 注释） */
	explicit Lcg(uint32_t seed) : state(seed) {}

	/** [0,1) 均匀分布（32 位精度）。 */
	[[nodiscard]] double uniform01() {
		state = state * 1664525u + 1013904223u;
		return static_cast<double>(state) / 4294967296.0;
	}

	/** @param lo 下界 @param hi 上界 @return [lo,hi) 均匀分布 */
	[[nodiscard]] double uniform(double lo, double hi) { return lo + (hi - lo) * uniform01(); }

	/** @param lo 下界 @param hi 上界 @return [lo,hi] 整数均匀分布 */
	[[nodiscard]] int uniformInt(int lo, int hi) {
		const double span = static_cast<double>(hi - lo + 1);
		return lo + static_cast<int>(uniform01() * span) % (hi - lo + 1);
	}

  private:
	uint32_t state;  ///< LCG 状态（32 位环绕是设计的一部分，不是溢出 bug）
};

/**
 * 从 proj 矩阵反解出视空间视锥参数（**double 精度**）。
 *
 * ## 为什么要 double、为什么单独一个函数
 *
 * 拟合球的判据本质是"8 个视锥角点到球心的距离 ≤ 半径"，而这个判据在**视空间**做
 * 最干净：视锥角点是 (±t·tanHalf·aspect, ±t·tanHalf, −t)，球心是 (0,0,−zc)——纯
 * 标量算术，没有矩阵、没有求逆、没有世界坐标。
 *
 * 而在世界空间做（`frustumCorners` 那条路）会在极端 near/far 比下病态：float32 的
 * `inverse(proj*view)` 在 far=3834/near=0.15 时角点位置误差达 10.9 单位（0.28%），
 * 足以把"球装不下视锥"的假象造出来——**症状与真 bug 不可区分**。这正是元模式
 * 「验证工具自己错」：判据的数值噪声必须远小于它要检测的信号。
 *
 * 反解关系（GLM `FORCE_DEPTH_ZERO_TO_ONE` 的 perspective）：
 *   `A = proj[2][2] = f/(n−f)`、`B = proj[3][2] = f·n/(n−f)` ⟹ `n = B/A`、`f = B/(1+A)`。
 * ⚠️ `1+A` 在 A≈−1 时是**相消**（两个 ~1 的数相减）：float32 下 `far` 的相对误差
 * 在 near/far=1e4 时可达 1e-3 量级。被测函数 `frustumBoundingSphere` 也走这条路，
 * 所以它有同一条精度限制——这不是测试的问题，是**契约的一部分**，故单列一条
 * TEST_CASE 单独钉住（见 test_shadow_geometry「proj 反解精度」）。
 *
 * @param proj 投影矩阵（透视）
 * @return {near, far, tanHalf(fovY 半角), aspect}
 */
export struct EyePlanes {
	double nearZ;
	double farZ;
	double tanHalf;
	double aspect;
};

export EyePlanes eyePlanes(const glm::mat4& proj) {
	const double A = static_cast<double>(proj[2][2]);
	const double B = static_cast<double>(proj[3][2]);
	const double p11 = static_cast<double>(proj[1][1]);
	const double p00 = static_cast<double>(proj[0][0]);
	return EyePlanes{B / A, B / (1.0 + A), 1.0 / p11, p11 / p00};
}

/**
 * 视空间 8 角点（无矩阵运算，纯标量；判据用这个，见 `eyePlanes` 的病态说明）。
 *
 * @param nearZ 真实近平面深度（**由调用方给真值**，不从 proj 反解——反解值带相消误差，
 *   用它构造角点会让"球装不下真视锥"这个真 bug 被自己的噪声掩盖）
 * @param farZ 真实远平面深度（同上）
 * @param tanHalf fovY 半角的正切
 * @param aspect 宽高比
 * @return 8 个视空间角点（前 4 = 近平面，后 4 = 远平面；x/y 象限顺序同 frustumCorners）
 */
export std::vector<glm::vec3> frustumCornersEye(double nearZ, double farZ, double tanHalf,
	double aspect) {
	std::vector<glm::vec3> out;
	out.reserve(8);
	for (const double t : {nearZ, farZ})
		for (const double x : {-1.0, 1.0})
			for (const double y : {-1.0, 1.0})
				out.push_back(glm::vec3(static_cast<float>(x * t * tanHalf * aspect),
					static_cast<float>(y * t * tanHalf), static_cast<float>(-t)));
	return out;
}

/** 由 `EyePlanes` 构造角点的便捷重载（自洽性用途；覆盖性判据请用显式真值版）。 */
export std::vector<glm::vec3> frustumCornersEye(const EyePlanes& ep) {
	return frustumCornersEye(ep.nearZ, ep.farZ, ep.tanHalf, ep.aspect);
}

/**
 * 视锥 8 角点（**世界坐标**）——把 NDC 角点反投影回世界。
 *
 * ## 矩阵顺序：必须是 `proj * view`，不是 `view * proj`
 *
 * 这一条在本项目被踩了**两次**，症状相同，所以在这里写死：
 *   1. 被测代码：git:调试历史 5a-5「NDC 角点当世界点乘**正向** viewProj」；
 *   2. 本辅助函数的首版：写成 `inverse(view * proj)`。
 *
 * 原因：约定是**列向量**约定（`v' = M·v`），世界→裁剪是"先 view 后 proj"，故合成
 * 矩阵为 `proj * view`（右乘的先执行）。写反了不编译错、不崩，只是得到一个
 * "先投影后变换"的错误矩阵——而**当 view 恰为单位阵时两者完全相同**，于是用简单
 * 机位验证会通过，换个真机位就全错（本辅助函数就是这样先被"view=identity 下 8 个
 * 角点全对"骗过，再在真机位上炸出 4241 条失败）。
 *
 * ⚠️ **精度限制**：本函数走 `inverse(proj*view)`，在 near/far 比 >1e3 时角点位置误差
 * 可达 0.1~0.3%（float32 求逆的条件数）。故它**只用于自洽性判据**（往返投影），
 * 覆盖性判据请用 `frustumCornersEye`。
 *
 * @param view 相机 view 矩阵（世界 → 视空间）
 * @param proj 投影矩阵（视空间 → 裁剪，须为透视）
 * @return 8 个世界坐标角点（顺序：z∈{0,1}（RH_ZO：0=近）各 4 个 x/y 象限）
 */
export std::vector<glm::vec3> frustumCorners(const glm::mat4& view, const glm::mat4& proj) {
	const glm::mat4 invViewProj = glm::inverse(proj * view);
	std::vector<glm::vec3> out;
	out.reserve(8);
	for (const float z : {0.0f, 1.0f}) {  // RH_ZO：ndc.z ∈ [0,1]，0 = 近平面
		for (const float x : {-1.0f, 1.0f}) {
			for (const float y : {-1.0f, 1.0f}) {
				const glm::vec4 w = invViewProj * glm::vec4(x, y, z, 1.0f);
				out.push_back(glm::vec3(w) / w.w);
			}
		}
	}
	return out;
}

}  // namespace RGL::testing
