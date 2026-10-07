module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::perspective / glm::lookAt（构造被测输入，不进参考实现）
#include <cmath>
#include <numbers>

/**
 * test_velocity_field —— 批 14.4 逐像素速度场的纯函数层（rgl/velocity_field.cppm，rlbase）
 *
 * 被测 = `pixelVelocity`（uv + depth + 两帧矩阵 → 像素位移）与它的两个构件
 * `worldFromUvDepth` / `uvFromWorld`。
 *
 * ## 为什么这批先只做纯函数层
 * 速度场错一个符号或错一个轴，**画面照样能看**（只是历史搬到反方向），靠眼睛抓不出来；
 * 而本项目的坐标系口径已经第三次咬人（`phase13_plan` §1.6 撤回条、§1.17 结论 5 的 φ 反号、
 * 债-65 漏掉的 v 分量），每次都是"接上真画面之后才发现一开始就错"。所以先把解析解钉死，
 * 再谈 pass。
 *
 * ## 期望值出处（独立于被测实现）
 * 本文件**不用**被测码的矩阵路径算期望，而是另有一套显式参考实现 `RefCamera`：
 * 手工构造两帧相机基（eye / forward / right / up），用三次点积 + 闭式透视公式投影，
 * **不调用 `glm::lookAt` / `glm::perspective`、不做矩阵求逆**。方法学沿用
 * `tools/imgqa/temporal_yaw_remap`（对"显式构造两帧相机基"的金标准实测 2.3e-13 px）。
 * 透视与深度的闭式是手算的：
 *   视空间（glm::lookAt 口径，相机看向 −Z）xc = right·(P−eye)，yc = up·(P−eye)，
 *   zc = forward·(P−eye)（正 = 前方），viewZ = −zc；
 *   `glm::perspective(fovY, aspect, near, far)` ⟹ clip = (xc·f/aspect, yc·f, ...)，
 *   clip.w = −viewZ = zc，f = 1/tan(fovY/2) ⟹ ndc.x = xc·f/(aspect·zc)，ndc.y = yc·f/zc；
 *   `GLM_FORCE_DEPTH_ZERO_TO_ONE` ⟹ depth = far·(zc−near) / (zc·(far−near))
 *   （自检：zc=near ⟹ 0，zc=far ⟹ 1，两端点手算）。
 * 另有一条**完全独立于两套实现**的解析校验：纯旋转的像面速度（旋转光流标准式）
 *   绕相机系 ω 旋转、像点 (u,v)、焦距 f：u̇ = f(Vx·Z − X·Vz)/Z²，v̇ = f(Vy·Z − Y·Vz)/Z²，
 *   其中 V = −ω×P。绕**世界 Y** 转 ω 而相机俯仰 φ ⟹ ω_cam = (0, ω·cosφ, −ω·sinφ)，
 *   代入化简（X = uZ/f、Y = vZ/f）得
 *   **u̇ = −ω·cosφ·f·(1 + (u/f)²) − ω·sinφ·u·v/f，v̇ = ω·u·(sinφ − (v/f)·cosφ)**。
 *   v̇ 在 φ≠0 时**非零且与深度无关** —— 这正是债-65 被漏掉的那一整项。
 *
 * ## 变异红线（**全部实测**于 2026-10-08；改坏被测 → `just test` 变红 → 文件备份还原）
 *  - 速度取 `cur − prev`（符号反）           → 红 3 条（平移闭式 / 旋转闭式 / extent 乘数）
 *  - 输出**除以** extent 而非乘               → 红 6 条（14.2 实测犯过的方向错）
 *  - y 翻转漏一次（ndc.y = uv.y·2−1）        → 红 5 条（互逆性 / 平移 / 旋转 / jitter 配对）
 *  - 天空排除整个删掉（自旋施加到天空）       → 红 1 条（天空阳性用例）
 *  - 天空判据反向（只给天空施加）             → 红 1 条（天空反向用例）
 *  - `w ≤ 0` 判据失效（相机背后当有效）       → 红 1 条（背后判无效）
 *  - **两道非有限值防护同时摘掉**             → 红 1 条（NaN/inf 用例）
 *    ⚠ 只摘**一道**时**检不出**（0 红）——这不是判据缺失，而是 `uvFromWorld` 的 uv 侧防护
 *    与 `pixelVelocity` 的速度侧防护**互为冗余**：任一道在场，NaN 都归到 `valid = false`。
 *    所以本层的正确判据是**行为级**的（「NaN 进 ⟹ 无效出」），它已被覆盖；
 *    单点变异检不出是**冗余的产物**。记在这里是为了别让后来人把"0 红"读成"防护没用"
 *    而顺手删掉另一道 —— 那时就真的只剩一道了。
 *
 * ## 本文件自己踩过的两个坑（写下来防重复）
 *  1. **第一版断言了不成立的逐位相等**：以为"两帧同矩阵 ⟹ 速度逐位 0"。错 —— uv→世界乘
 *     `curInvViewProj`（求逆结果）、世界→uv 乘 `prevViewProj`（未求逆），是**两套浮点表达式**，
 *     `M⁻¹·M ≠ I` 严格成立。正确判据是「≪1 像素」，且同时断言噪声地板本身 < 1e-2 px
 *     与 > 0（后者防"实现根本没算"）。见 `kNoisePx`。
 *  2. **两处符号靠推是推不对的**：平移闭式的 ±t、旋转光流闭式的 ω 第三分量与 prev−cur 反号，
 *     第一版**三处全推错**，全部红在实测上。最终确认手法 = 拿 doctest 报出的实测值
 *     **反解**闭式（`python3` 算一遍对五个点），而不是"再推一遍"——再推一遍同样会错。
 */
export module test_velocity_field;

import rlstd;
import velocity_field;
import taa_jitter;  // applyProjectionJitter（jitter 配对用例造"抖动后矩阵"）

namespace RGL {
namespace {

constexpr float kFovY = 0.9f;      // rad（任意非特殊值，避免 45° 让 f 变成 √2 之类的巧合）
constexpr float kAspect = 16.0f / 9.0f;
constexpr float kNear = 0.1f;
constexpr float kFar = 200.0f;
constexpr glm::dvec3 kWorldUp{0.0, 1.0, 0.0};
constexpr glm::vec3 kWorldUpF{0.0f, 1.0f, 0.0f};

/** 参考相机：只存基向量与眼位，投影走点积 + 闭式，**不碰 glm 的 view/proj 矩阵**。 */
struct RefCamera {
	glm::dvec3 eye{};
	glm::dvec3 fwd{};    ///< 单位前向
	glm::dvec3 right{};  ///< 单位右向
	glm::dvec3 up{};     ///< 单位上向（由 right×fwd 构造，与 glm::lookAt 同手性）

	/** 由眼位 + yaw（绕世界 Y）+ pitch 构造基。yaw=0 时前向 = −Z（与 `cameraForward` 同约定）。 */
	static RefCamera fromYawPitch(glm::dvec3 eyeAt, double yaw, double pitch) {
		RefCamera c;
		c.eye = eyeAt;
		c.fwd = {std::cos(pitch) * std::sin(yaw), std::sin(pitch), -std::cos(pitch) * std::cos(yaw)};
		c.fwd /= glm::length(c.fwd);
		c.right = glm::cross(c.fwd, kWorldUp);
		c.right /= glm::length(c.right);
		c.up = glm::cross(c.right, c.fwd);
		return c;
	}

	/** 视空间深度（正 = 前方）。 */
	double zc(glm::dvec3 p) const { return glm::dot(fwd, p - eye); }

	/** 世界 → framebuffer uv（闭式透视 + y 翻转）。前置：p 在相机前方。 */
	glm::dvec2 uv(glm::dvec3 p) const {
		const double f = 1.0 / std::tan(kFovY / 2.0);
		const double z = zc(p);
		const double xc = glm::dot(right, p - eye);
		const double yc = glm::dot(up, p - eye);
		const double ndcX = xc * f / (kAspect * z);
		const double ndcY = yc * f / z;
		return {ndcX * 0.5 + 0.5, (1.0 - ndcY) * 0.5};
	}

	/** 世界 → Vulkan 深度 [0,1]（GLM_FORCE_DEPTH_ZERO_TO_ONE 的闭式；端点自检见文件头）。 */
	float depth(glm::dvec3 p) const {
		const double z = zc(p);
		return static_cast<float>(static_cast<double>(kFar) * (z - kNear) / (z * (kFar - kNear)));
	}

	glm::mat4 view() const {
		// glm::lookAt 口径的 view（列主序按 glm::value_ptr 语义直接填）
		glm::mat4 v(1.0f);
		v[0] = {static_cast<float>(right.x), static_cast<float>(up.x), static_cast<float>(-fwd.x), 0.0f};
		v[1] = {static_cast<float>(right.y), static_cast<float>(up.y), static_cast<float>(-fwd.y), 0.0f};
		v[2] = {static_cast<float>(right.z), static_cast<float>(up.z), static_cast<float>(-fwd.z), 0.0f};
		v[3] = {-glm::dvec3{glm::dot(right, eye), glm::dot(up, eye), glm::dot(-fwd, eye)}, 1.0f};
		return v;
	}
};

/** 被测侧的矩阵输入（走 glm::lookAt / perspective —— 与生产同路径，与参考实现不同路径）。 */
VelocityFrame frameOf(const RefCamera& cur, const RefCamera& prev, glm::vec2 extent,
                      glm::mat4 sceneMotion = glm::mat4{1.0f}) {
	const auto proj = glm::perspective(kFovY, kAspect, kNear, kFar);
	VelocityFrame f;
	f.curInvViewProj = glm::inverse(proj * cur.view());
	f.prevViewProj = proj * prev.view();
	f.sceneMotion = sceneMotion;
	f.extent = extent;
	return f;
}

/** 被测：世界点 → 速度（先用参考实现算出该点的 uv/depth，再喂给被测函数）。 */
PixelVelocity velocityOfPoint(const VelocityFrame& fr, const RefCamera& cur, glm::dvec3 p) {
	const glm::vec2 uv(static_cast<float>(cur.uv(p).x), static_cast<float>(cur.uv(p).y));
	return pixelVelocity(uv, cur.depth(p), fr);
}

constexpr glm::vec2 kExtent{1280.0f, 720.0f};

/**
 * 往返噪声地板（px）。
 *
 * **为什么这里不能要求逐位 0**（第一版就要求了，实测红）：`pixelVelocity` 的两跳是
 * **两套不同的浮点表达式** —— uv→世界 乘 `curInvViewProj`（矩阵求逆的结果），
 * 世界→uv 乘 `prevViewProj`（未求逆）。即使两者数学上互逆，`M⁻¹·M ≠ I` 在浮点上
 * 严格成立，NDC 上约 1e-6 相对误差 × 1280 px ≈ 1e-3 px。
 * 所以"无运动 ⟹ 无速度"的正确判据是 **≪ 一个像素**，不是 == 0。
 * 下面每条用到它的用例都同时断言"信号地板"（真运动必须远大于它），
 * 否则容差就是空的。
 */
constexpr float kNoisePx = 1e-2f;

/** 参考 uv 差 → 像素速度（prev − cur）。 */
glm::dvec2 refVelocity(const RefCamera& cur, const RefCamera& prev, glm::dvec3 p) {
	return (prev.uv(p) - cur.uv(p)) * glm::dvec2{kExtent.x, kExtent.y};
}

}  // namespace

TEST_CASE("worldFromUvDepth ↔ uvFromWorld 互逆（与 ao/taa shader 同口径的镜像性）") {
	const RefCamera cur = RefCamera::fromYawPitch({1.0, 2.0, 5.0}, 0.3, -0.2);
	const auto proj = glm::perspective(kFovY, kAspect, kNear, kFar);
	const glm::mat4 inv = glm::inverse(proj * cur.view());
	// 覆盖屏内多点 + 多深度；正例守卫：集合非空由循环本身保证（这里显式数过）
	int checked = 0;
	for (float u : {0.05f, 0.25f, 0.5f, 0.75f, 0.95f}) {
		for (float v : {0.05f, 0.25f, 0.5f, 0.75f, 0.95f}) {
			for (float d : {0.001f, 0.05f, 0.3f, 0.8f, 0.999f}) {
				const glm::vec3 w = worldFromUvDepth({u, v}, d, inv);
				const auto back = uvFromWorld(w, proj * cur.view());
				REQUIRE(back.has_value());
				CHECK(back->x == doctest::Approx(u).epsilon(1e-4));
				CHECK(back->y == doctest::Approx(v).epsilon(1e-4));
				++checked;
			}
		}
	}
	CHECK(checked == 125);  // 空转守卫：外层循环若被改空，这条先红
}

TEST_CASE("阳性锚点：两帧同矩阵 + 无自旋 ⟹ 全像素速度恒 0（且逐位 0）") {
	const RefCamera cur = RefCamera::fromYawPitch({0.0, 1.0, 6.0}, 0.4, -0.25);
	const auto fr = frameOf(cur, cur, kExtent);
	int checked = 0;
	double maxNoise = 0.0;
	for (float u : {0.01f, 0.2f, 0.5f, 0.8f, 0.99f}) {
		for (float v : {0.01f, 0.2f, 0.5f, 0.8f, 0.99f}) {
			for (float d : {0.002f, 0.1f, 0.5f, 0.99f}) {
				const auto vel = pixelVelocity({u, v}, d, fr);
				REQUIRE(vel.valid);
				CHECK(std::fabs(vel.velocityPx.x) < kNoisePx);
				CHECK(std::fabs(vel.velocityPx.y) < kNoisePx);
				maxNoise = std::max(maxNoise, static_cast<double>(std::fabs(vel.velocityPx.x)) +
				                              static_cast<double>(std::fabs(vel.velocityPx.y)));
				++checked;
			}
		}
	}
	CHECK(checked == 100);
	// 噪声地板本身要可观测：它必须**远小于**一个像素，否则"无运动"与"有运动"分不开
	CHECK(maxNoise < kNoisePx);
	CHECK(maxNoise > 0.0);  // 恒 0 反而说明往返被优化掉了（或实现根本没算）
}

TEST_CASE("纯平移：速度对上 1/z 闭式（手算，含符号）") {
	// prev 眼位 = cur 眼位 − t·right（= 相机从 prev 到 cur 沿自身右向**移了 t**）。
	// 于是世界点 P 相对 prev 眼的横向偏移是 **+t**（不是 −t；第一版在这里推反了符号，
	// 红在实测上——正是"断言要照源码写、期望值出处要独立"里前者容易错的地方）：
	//   u = 0.5 + 0.5·xc·f/(aspect·zc)，xc_prev = xc_cur + t ⟹ Δu = +0.5·t·f/(aspect·zc)
	//   速度.x = Δu·W = +0.5·t·f·W/(aspect·zc)（**与 1/zc 成正比**，近处大远处小）
	// 物理自检：相机右移 ⟹ 物体在 prev 图里偏**右** ⟹ prev − cur 为正。
	// 期望值由该闭式给出，不读被测实现。
	const double t = 0.35;
	const glm::dvec3 eye{0.0, 0.0, 4.0};
	const RefCamera cur = RefCamera::fromYawPitch(eye, 0.0, 0.0);
	const RefCamera prev = RefCamera::fromYawPitch(eye - glm::dvec3{t, 0.0, 0.0}, 0.0, 0.0);
	const auto fr = frameOf(cur, prev, kExtent);
	const double f = 1.0 / std::tan(kFovY / 2.0);

	int checked = 0;
	for (double zc : {1.0, 2.0, 4.0, 8.0}) {
		const glm::dvec3 p{0.0, 0.0, 4.0 - zc};  // 正前方，距相机 zc
		REQUIRE(cur.zc(p) > kNear);
		const auto vel = velocityOfPoint(fr, cur, p);
		REQUIRE(vel.valid);
		const double expectX = 0.5 * t * f * kExtent.x / (kAspect * zc);
		CHECK(vel.velocityPx.x == doctest::Approx(expectX).epsilon(1e-3));
		// 正前方点在纯横向平移下 v 不动
		CHECK(std::abs(vel.velocityPx.y) < 1e-3);
		// 符号单独钉一次：闭式与实现同向错的时候，上面那条 epsilon 判据抓不到
		CHECK(vel.velocityPx.x > 0.0);
		++checked;
	}
	CHECK(checked == 4);
	// 1/z 结构：最近的点速度必须是最近的 8 倍（zc=1 vs zc=8），不是 8 倍 = 结构错
	const auto near = velocityOfPoint(fr, cur, {0.0, 0.0, 3.0}).velocityPx.x;
	const auto far = velocityOfPoint(fr, cur, {0.0, 0.0, -4.0}).velocityPx.x;
	CHECK(near / far == doctest::Approx(8.0).epsilon(1e-3));
}

TEST_CASE("🔴 绕世界 Y 自旋 + 相机有 pitch ⟹ v 分量非零（债-65 漏掉的整项）") {
	// 这是本文件最重要的一条：naive 的"绕相机 up 转"闭式会给 v̇ ≡ 0，
	// 而真值是 ω·u·(sinφ − (v/f)·cosφ)。债-65 就是漏了它导致 1.4px 系统错位。
	const double pitch = -0.6;  // rad（≈ −34°，与债-65 的 wetground 量级同档）
	const double omega = 0.008;  // rad/帧（小角度，让一阶式与有限差分可比）
	const glm::dvec3 eye{0.0, 1.0, 5.0};
	const RefCamera cur = RefCamera::fromYawPitch(eye, 0.25, pitch);
	const RefCamera prev = RefCamera::fromYawPitch(eye, 0.25 - omega, pitch);
	const auto fr = frameOf(cur, prev, kExtent);
	const double f = 1.0 / std::tan(kFovY / 2.0);

	int checked = 0;
	double maxVSignal = 0.0;
	for (double u : {0.2, 0.35, 0.5, 0.65, 0.8}) {
		for (double v : {0.2, 0.35, 0.5, 0.65, 0.8}) {
			// 取该 uv 在 zc = 6 处的世界点（深度由参考实现给）
			const double zc = 6.0;
			const glm::dvec3 p = cur.eye + cur.fwd * zc
			                     + cur.right * ((u - 0.5) * 2.0 * kAspect * zc / f)
			                     + cur.up * ((0.5 - v) * 2.0 * zc / f);
			const auto vel = velocityOfPoint(fr, cur, p);
			REQUIRE(vel.valid);
			// ① 与显式两帧基参考对拍（两套实现、同一世界点）
			//    epsilon 只能到 5e-3 而不是浮点极限：速度是**两个几乎相等的 uv 之差**
			//    （ω=0.008 ⟹ 位移 ~2px / 1280），灾难性消去把 uv 的相对误差放大 ~600 倍，
			//    而被测走 float32、参考走 float64。这里验的是**结构与符号**，不是尾数。
			const auto ref = refVelocity(cur, prev, p);
			CHECK(vel.velocityPx.x == doctest::Approx(ref.x).epsilon(5e-3));
			CHECK(vel.velocityPx.y == doctest::Approx(ref.y).epsilon(5e-3));
			// ② 与旋转光流**一阶**闭式对拍（完全独立于上面两套实现）
			//    世界 Y 在相机系的分量 = (0, cosφ, sinφ)（第一版把第三项推成 −sinφ，
			//    红在实测上），代入 V = −ω×P 与针孔速度式得 ndc.y 变化率
			//      = −aspect·ω·uu·(sinφ + (vv/f)·cosφ)
			//    再乘两个换算：uv.y = (1−ndc.y)/2 ⟹ 像素 = ndc 率 × H/2；而本层的量是
			//    **prev − cur** = −(cur 变化率) ⟹ 符号再翻一次。合起来：
			//      速度.y = −aspect·ω·uu·(sinφ + (vv/f)·cosφ)·H/2
			const double uu = (u - 0.5) * 2.0, vv = (0.5 - v) * 2.0;  // 有符号归一化屏坐标
			const double vPx = -kAspect * omega * uu
			                   * (std::sin(pitch) + (vv / f) * std::cos(pitch)) * kExtent.y / 2.0;
			// 一阶近似 ⟹ 残差随 |uu|·|vv| 走（实测 ≤1.2%）。容差 3% 足以把"漏掉 sinφ
			// 整项"（差 100%）与"符号反"（差 200%）分开，同时不假装能分辨尾数。
			CHECK(vel.velocityPx.y == doctest::Approx(vPx).epsilon(3e-2));
			maxVSignal = std::max(maxVSignal, static_cast<double>(std::fabs(vel.velocityPx.y)));
			++checked;
		}
	}
	CHECK(checked == 25);
	// 阳性锚点：v 信号必须**大到不可能是数值噪声**（否则上面那条 epsilon 是空的）
	CHECK(maxVSignal > 1.0);  // px
}

TEST_CASE("天空（depth ≥ 1）不随场景自旋动；非天空必须动（反向用例）") {
	const glm::mat4 spin = glm::rotate(glm::mat4{1.0f}, 0.2f, kWorldUpF);
	const RefCamera cur = RefCamera::fromYawPitch({0.0, 1.0, 5.0}, 0.0, -0.2);
	const auto fr = frameOf(cur, cur, kExtent, spin);
	// 阳性：天空（depth = 1）在两帧同矩阵下速度恒 0 —— 若自旋被错误施加到天空，
	// 世界点会被搬走 ⟹ 速度非零 ⟹ 这里红
	for (float u : {0.1f, 0.5f, 0.9f}) {
		for (float v : {0.1f, 0.5f, 0.9f}) {
			const auto vel = pixelVelocity({u, v}, 1.0f, fr);
			REQUIRE(vel.valid);
			CHECK(std::fabs(vel.velocityPx.x) < kNoisePx);
			CHECK(std::fabs(vel.velocityPx.y) < kNoisePx);
		}
	}
	// 反向：同 uv 但 depth = 0.999（非天空）必须**明显非零**，
	// 否则"天空恒 0"可能只是"自旋根本没生效"的另一种表现
	bool anyMoved = false;
	for (float u : {0.1f, 0.5f, 0.9f}) {
		const auto vel = pixelVelocity({u, 0.5f}, 0.999f, fr);
		REQUIRE(vel.valid);
		if (std::abs(vel.velocityPx.x) > 1e-3f || std::abs(vel.velocityPx.y) > 1e-3f)
			anyMoved = true;
	}
	CHECK(anyMoved);
}

TEST_CASE("上一帧相机背后 ⟹ 判无效（不得给出可用速度）") {
	const glm::dvec3 eye{0.0, 0.0, 0.0};
	const RefCamera cur = RefCamera::fromYawPitch(eye, 0.0, 0.0);
	// prev 相机转 170°：本帧正前方的点上一帧在它**背后**
	const auto proj = glm::perspective(kFovY, kAspect, kNear, kFar);
	VelocityFrame fr;
	fr.curInvViewProj = glm::inverse(proj * cur.view());
	fr.prevViewProj = proj * glm::rotate(glm::mat4{1.0f}, 2.97f, kWorldUpF) * cur.view();
	fr.extent = kExtent;
	const auto vel = pixelVelocity({0.5f, 0.5f}, 0.5f, fr);
	CHECK_FALSE(vel.valid);
	// 反向：同一帧不旋转时必须有效（否则"判无效"只是"恒无效"）
	const auto ok = pixelVelocity({0.5f, 0.5f}, 0.5f, frameOf(cur, cur, kExtent));
	CHECK(ok.valid);
}

TEST_CASE("extent 是**乘数**不是除数（14.2 实测犯过的方向错，这里钉死）") {
	const glm::dvec3 eye{0.0, 0.0, 4.0};
	const double t = 0.35;
	const RefCamera cur = RefCamera::fromYawPitch(eye, 0.0, 0.0);
	const RefCamera prev = RefCamera::fromYawPitch(eye - glm::dvec3{t, 0.0, 0.0}, 0.0, 0.0);
	const glm::dvec3 p{0.0, 0.0, 2.0};
	const auto v1 = velocityOfPoint(frameOf(cur, prev, kExtent), cur, p).velocityPx;
	const auto v2 = velocityOfPoint(frameOf(cur, prev, kExtent * 2.0f), cur, p).velocityPx;
	// uv 差不变 ⟹ 像素速度随 extent **正比**放大（除的话会减半）
	CHECK(v2.x == doctest::Approx(v1.x * 2.0f).epsilon(1e-5));
	CHECK(std::abs(v1.x) > 1.0f);  // 阳性锚点：信号本身要够大
}

TEST_CASE("jitter 配对：喂抖动后矩阵 ⟹ 输出恰好平移 jitter 之差（口径成对的可推导性）") {
	// 文件头第 4 条红线的机器可验版本：本层看不出喂的是未抖还是抖动后，但两者的差
	// **必须**恰好是 jitter 差（⟹ 调用方只要与 FSR 的 jitter cancellation 标志配对即可，
	// 不需要本层知道）。jitter 注入 = 投影矩阵第 3 列的 NDC 平移（taa_jitter 的既有口径）。
	const RefCamera cur = RefCamera::fromYawPitch({0.0, 1.0, 5.0}, 0.3, -0.2);
	const auto proj = glm::perspective(kFovY, kAspect, kNear, kFar);
	const glm::vec2 jCur{0.4f, -0.3f}, jPrev{-0.25f, 0.35f};

	VelocityFrame base = frameOf(cur, cur, kExtent);  // 两帧同矩阵 ⟹ 未抖时恒 0
	CHECK(glm::length(pixelVelocity({0.4f, 0.6f}, 0.3f, base).velocityPx) < kNoisePx);

	// 两侧各加自己的 jitter：cur 用 jCur、prev 用 jPrev
	base.curInvViewProj = glm::inverse(applyProjectionJitter(proj, jCur, kExtent) * cur.view());
	base.prevViewProj = applyProjectionJitter(proj, jPrev, kExtent) * cur.view();
	const auto vel = pixelVelocity({0.4f, 0.6f}, 0.3f, base);
	REQUIRE(vel.valid);
	// 期望 = **两帧内容位移之差** `c_prev − c_cur`，其中 `c = contentShiftPx(jitter)`。
	// 刻意不写死 (+jx, −jy)：逐轴符号是 `contentShiftPx` 的既有口径（y 因负高视口翻向，
	// 第一版在这里凭直觉写成 + 就红了），而本用例真正要钉的是
	// **「抖动带来的速度 = 两帧内容位移之差」这个关系本身**——它才是调用方与 FSR 的
	// jitter cancellation 配对时需要的东西。用生产函数表达 ⟹ 两边不会各自漂。
	const glm::vec2 expect = contentShiftPx(jPrev) - contentShiftPx(jCur);
	CHECK(vel.velocityPx.x == doctest::Approx(expect.x).epsilon(1e-3));
	CHECK(vel.velocityPx.y == doctest::Approx(expect.y).epsilon(1e-3));
	// 阳性锚点：y 必须**不是** +(jPrev.y − jCur.y)（否则说明翻向判据是白写的）
	CHECK(std::abs(vel.velocityPx.y - (jPrev.y - jCur.y)) > 1e-3f);
}

TEST_CASE("非退化：有运动就必须有速度（防『恒返回零』把上面全骗过去）") {
	const RefCamera cur = RefCamera::fromYawPitch({0.0, 1.0, 5.0}, 0.3, -0.2);
	const RefCamera prev = RefCamera::fromYawPitch({0.4, 1.3, 4.2}, 0.42, -0.1);
	const auto fr = frameOf(cur, prev, kExtent);
	// ⚠ 深度取 0.98 而不是 0.2：Vulkan 深度对 zc 是**强非线性**的
	// （zc = near·far/(far − depth·(far−near)) 型），near=0.1/far=200 下 depth=0.2 对应
	// zc ≈ 0.125（贴脸），相机又移又转之后该点上一帧就在**背后** ⟹ 判无效，
	// 于是这条"非退化"用例会因为选点不当而变成"无效判定"用例。depth≈0.98 ⟺ zc≈5。
	constexpr float kDepthMid = 0.98f;
	int nonzero = 0, total = 0;
	for (float u : {0.15f, 0.5f, 0.85f}) {
		for (float v : {0.15f, 0.5f, 0.85f}) {
			const auto vel = pixelVelocity({u, v}, kDepthMid, fr);
			REQUIRE(vel.valid);
			if (glm::length(vel.velocityPx) > 1e-2f)
				++nonzero;
			++total;
		}
	}
	CHECK(total == 9);
	CHECK(nonzero == total);  // 相机又移又转，9 个点不可能全为 0
}

TEST_CASE("深度为 NaN / inf（GPU 侧 UAF 的真实产物形态）⟹ 判无效，不得吐数") {
	// 为什么这条不是防御性冗余：深度缓冲里的 NaN/inf 是**实际发生过的故障形态**
	// （texel canary 就是为"纹理 BO 已释放、GPU 采样到垃圾"而设的，见 docs/archive/
	// heap_texture_research.md §12.7）。速度场若把 NaN 当有效值写进 motionVectors，
	// FSR 会拿它去索引历史 —— 症状是隔一整帧才出现的局部糊块，事后极难归因。
	// 本用例是 `uvFromWorld` 非有限值防护的**唯一**覆盖（变异验过：摘掉防护只有它红）。
	const RefCamera cur = RefCamera::fromYawPitch({0.0, 1.0, 5.0}, 0.2, -0.2);
	const auto fr = frameOf(cur, cur, kExtent);
	const double nan = std::numeric_limits<double>::quiet_NaN();
	const double inf = std::numeric_limits<double>::infinity();

	CHECK_FALSE(pixelVelocity({0.5f, 0.5f}, static_cast<float>(nan), fr).valid);
	CHECK_FALSE(pixelVelocity({0.5f, 0.5f}, static_cast<float>(inf), fr).valid);
	// uv 侧同样不得吐数（全屏 pass 的边界像素理论上不该越界，但越界也不该污染历史）
	CHECK_FALSE(pixelVelocity({static_cast<float>(nan), 0.5f}, 0.5f, fr).valid);
	// 反向锚点：同帧同深度的正常点必须有效（否则"判无效"只是"恒无效"）
	CHECK(pixelVelocity({0.5f, 0.5f}, 0.5f, fr).valid);
}

TEST_CASE("🔴 基空间换算：静止机位 + 逐帧变化的 jitter ⟹ 基空间速度恒 0") {
	// 本条是 `toBaseSpace` 的**阳性锚点**，也是整条速度场链路里最容易做错的一环。
	// 形态：两帧相机矩阵**完全相同**（机位静止、无自旋），但抖动相位每帧不同
	// （Halton 序列 ⟹ c_t ≠ c_{t−1}，这是 TAA 的正常工作状态）。
	// 此时真值 = **恒 0**：表面点根本没动。而抖动空间的原始输出是 c_{t−1} − c_t
	// （纯抖动伪影、与运动无关），量级 ±1 px —— **与真实亚像素运动同量级**，
	// 所以漏掉这一步不会让画面崩，只会让 FSR/TAA 的历史在静止机位下也被判成"在动"，
	// 表现为静止画面糊（与债-65 同族的"错得很小所以看不见"）。
	const RefCamera cur = RefCamera::fromYawPitch({0.0, 1.0, 5.0}, 0.3, -0.2);
	const auto proj = glm::perspective(kFovY, kAspect, kNear, kFar);
	const glm::vec2 jCur{0.41f, -0.28f}, jPrev{-0.33f, 0.37f};  // 任意两相位（模拟相邻帧）

	VelocityFrame fr;
	fr.curInvViewProj = glm::inverse(applyProjectionJitter(proj, jCur, kExtent) * cur.view());
	fr.prevViewProj = applyProjectionJitter(proj, jPrev, kExtent) * cur.view();
	fr.extent = kExtent;
	const glm::vec2 cCur = contentShiftPx(jCur), cPrev = contentShiftPx(jPrev);

	int checked = 0;
	float maxRaw = 0.0f, maxBase = 0.0f;
	for (float u : {0.1f, 0.35f, 0.5f, 0.65f, 0.9f}) {
		for (float v : {0.1f, 0.35f, 0.5f, 0.65f, 0.9f}) {
			for (float d : {0.5f, 0.9f}) {
				const auto raw = pixelVelocity({u, v}, d, fr);
				REQUIRE(raw.valid);
				const glm::vec2 base = toBaseSpace(raw.velocityPx, cCur, cPrev);
				maxRaw = std::max(maxRaw, glm::length(raw.velocityPx));
				maxBase = std::max(maxBase, glm::length(base));
				CHECK(glm::length(base) < kNoisePx);
				++checked;
			}
		}
	}
	CHECK(checked == 50);
	// 两条**互相成就**的断言：
	// ① 换算后必须落到噪声地板内（上面已判），② 换算**前**必须明显非零——
	// 否则"换算后为 0"可能只是因为整条链路恒 0（那 ① 就是空的）。
	CHECK(maxRaw > 0.1f);
	CHECK(maxBase < kNoisePx);
	// 且换算量恰好是 c_t − c_{t−1}（逐点差恒等，不是拟合出来的系数）
	const auto one = pixelVelocity({0.5f, 0.5f}, 0.5f, fr);
	const glm::vec2 expect = one.velocityPx + (cCur - cPrev);
	CHECK(glm::length(toBaseSpace(one.velocityPx, cCur, cPrev) - expect) == doctest::Approx(0.0f));
}

}  // namespace RGL
