module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>   // glm::inverse（造 spinRatio 的入参，不进期望值算式）
#include <glm/gtc/matrix_transform.hpp>  // glm::rotate（造随机 spin，同上）
#include <cmath>             // std::isfinite / std::isnan / std::isinf / std::frexp（宏与自由函数不经 BMI 保证传递，GMF 自带）
#include <initializer_list>  // 范围 for 的花括号列表（modules 下不从 import 带进来）

/**
 * test_coordinate_rebase —— 债-85 批 1 护栏 + 批 3 唯一换算入口
 * （被测模块 = rgl/coordinate_rebase.cppm，rlbase）
 *
 * 被测 = kVisibleCrawlCameraDistance / distanceFromOrigin / coordinateHeadroom /
 * rebaseSnapGranularity / snapOriginToGranularity 五个纯函数（零运行期消费者是故意的：
 * 护栏先于实现，让 origin rebasing 一落地就撞上它，而不是自己发明规则）
 * + toRelative（批 3 第一刀：全仓唯一「绝对 → 相对」换算入口，本刀同样只接单测，
 * 消费者接线在批 4）。
 * 🔴 删除本文件或 coordinate_rebase.cppm = 拆掉债-85 ② 的唯一机器防线。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 触发常数 7.6e5 与机位 |camera| ≈ 625 = docs/还债总账.md 债-85 行内量级表的实测读数；
 *  - 余量标定值 3.08493 / 0.88081 / -0.11919 = 对 7.6e5/625 = 1216、7.6e5/1e5 = 7.6、
 *    7.6e5/1e6 = 0.76 手算 log10（python 复核），不抄被测码的表达式；
 *  - 相机 ULP 对照值 2^-14 = 债-85 的台阶式 `2^(floor(log2|x|)-23)`（floor(log2 625) = 9）；
 *  - 粒度期望（0.5 / 0.03125 / …）= 「向上取到 2 的幂」的闭式手算；
 *  - snap 期望 = 逐轴「除以格距 → 四舍五入（半格远离零）→ 乘回格距」的手算，
 *    1e6 用例的期望 1000000.001953125 是 1e6 + 2·2^-10 的精确二进制值（可逐位断言）；
 *  - toRelative 期望 = IEEE 闭式事实：减 +0.0 逐位返回自身；float→double 注入式精确、
 *    再 float() 回原值；1e6 处 float32 ULP = 2^(19-23) = 0.0625（1e6 ∈ [2^19, 2^20)，
 *    14 位尾数）⟹ 1e6 + 2^-30 舍入到 1e6，而 double 域减掉 1e6 后 2^-30 原样保留。
 *
 * ## 变异声明（每条实测：改坏 coordinate_rebase.cppm → 点名用例红 → cp 备份还原；2026-10-25 读数）
 *  - 触发常数 7.6e5 → +Infinity（批 1a 点名的变异本体）→ 红 2 条（常数钉值 + 1e6 反向）
 *  - log10 → log（自然对数，数量级口径漂）        → 红 3 条（625/1e5/1e6 标定）
 *  - 原点特例的 +∞ 改成 NaN（相机在原点出 NaN）    → 红 1 条（原点合法用例）
 *  - distanceFromOrigin 退化成只取 |x|             → 红 1 条（勾股闭式）
 *  - 粒度取「第一个非零」或「最后一个」而非最大     → 红 2 条（最大值居中的两条用例）
 *  - 去掉 quantizeShadowDistance（返回原始最大值） → 红 2 条（0.3→0.5 用例 + 2 的幂契约）
 *  - 空输入/全零返回 0.0（假装合法粒度）           → 红 1 条（非法输入不出 0 用例）
 *  - snap 除法收窄成 float（1e6 处 ULP = 0.0625）  → 红 1 条（1e6 细格距用例）
 *  - 非法 granularity 返回 desired 原样（静默不 snap）→ 红 1 条（非法粒度全 NaN 用例）
 *  - toRelative 两行调换顺序（先收窄后减）→ 红 2 条（顺序承重阳性锚点 + 只动平移列的闭式期望）
 *  - toRelative 连 c3.w 一起减（dvec4(origin, origin.x)）→ 红 2 条（顺序承重的 w 断言 + 只动平移列）
 *  - toRelative 不减平移列（直接返回收窄）→ 红 2 条（顺序承重 + 只动平移列的 moved 守卫）
 *  - toRelative 减错列（动 c0 不动 c3）  → 红 2 条（只动平移列 + 顺序承重）
 *  - ⚠ 门关恒等与组合两条对以上四种 toRelative 变异**都不红**，这是设计如此：原点为 0 时
 *    错序也是恒等、组合两侧用同一实现自比恒等。抓住它们的是上面「顺序承重」与「只动平移列」
 *    两条，别把恒等/组合当成防变异的判据。
 *  - 生产点（model_importer_impl / builtin_geometry_impl 的 absolute 填法）**无单测覆盖**
 *    （设备层，just test 不跑）——守它的是出厂默认态截图锚 efa70587dc869e66…：
 *    实测「逐图元忘复合根位姿」（absolute = dmat4(local)）→ 截图变 a8d51f31…（红）。
 */
export module test_coordinate_rebase;

import rlstd;
import test_support;
import coordinate_rebase;

using RGL::coordinateHeadroom;
using RGL::distanceFromOrigin;
using RGL::kVisibleCrawlCameraDistance;
using RGL::motionWithOriginShift;
using RGL::rebaseSnapGranularity;
using RGL::toRelativeModel;
using RGL::snapOriginToGranularity;
using RGL::testing::expectNear;
using RGL::testing::Lcg;
using RGL::toRelative;

namespace {

/** 判据：v 是 2 的幂（frexp 尾数恰 0.5 ⟺ v = 2^k，闭式定义，不依赖被测码）。 */
bool isPowerOfTwo(double v) {
	if (!(v > 0.0) || !std::isfinite(v))
		return false;
	int exponent = 0;
	return std::frexp(v, &exponent) == 0.5;
}

/** 逐分量比较（glm 向量的 == 对 NaN 分量恒 false，测 NaN 出口要逐分量 isnan）。 */
void checkAllNan(const glm::dvec3& v) {
	CHECK(std::isnan(v.x));
	CHECK(std::isnan(v.y));
	CHECK(std::isnan(v.z));
}

/**
 * float 矩阵 16 分量逐位相等（== 对有限 float 即逐位相同；NaN 分量恒 false = 必红，
 * 正是「逐位不变」判据要的语义——不许用 Approx/容差，容差会被谈到通过为止）。
 */
void checkMat4BitwiseEqual(const glm::mat4& actual, const glm::mat4& expected) {
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			CHECK(actual[col][row] == expected[col][row]);
}

/** 用 Lcg 填随机 double 矩阵（分量量级 ±magnitude；c3.w 钉 1，与真实世界矩阵同形）。 */
glm::dmat4 randomDmat4(Lcg& rng, double magnitude) {
	glm::dmat4 m{1.0};
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			m[col][row] = rng.uniform(-magnitude, magnitude);
	m[3][3] = 1.0;
	return m;
}

}  // namespace

TEST_SUITE("coordinate_rebase") {

TEST_CASE("触发常数钉死为债-85 实测读数 7.6e5（不许就地重新推导）") {
	// 出处 = docs/还债总账.md 债-85 行：「0.5px 可见爬行的触发点 ≈ |camera| 7.6e5」。
	// 阳性锚点：常数被改成 +Infinity（批 1a 点名的变异）或任何别的数，这条立刻红。
	CHECK(kVisibleCrawlCameraDistance == 7.6e5);
	CHECK(std::isfinite(kVisibleCrawlCameraDistance));  // 反向：钉值同时排除「改成 inf 让余量恒大」
}

TEST_CASE("distanceFromOrigin：勾股闭式（含 3-4-5 与 625 标定机位）") {
	// 期望值 = 手算勾股，不抄被测码。若实现退化成只取 |x|，(3,4,0)→5 与 (1e3,2e3,2e3)→3e3 都红。
	CHECK(distanceFromOrigin(glm::dvec3{0.0, 0.0, 0.0}) == 0.0);
	CHECK(distanceFromOrigin(glm::dvec3{625.0, 0.0, 0.0}) == 625.0);
	CHECK(distanceFromOrigin(glm::dvec3{375.0, 500.0, 0.0}) == 625.0);  // 3-4-5 × 125
	CHECK(distanceFromOrigin(glm::dvec3{3.0, 4.0, 0.0}) == 5.0);
	CHECK(distanceFromOrigin(glm::dvec3{-3.0, 0.0, -4.0}) == 5.0);
	CHECK(distanceFromOrigin(glm::dvec3{1e3, 2e3, 2e3}) == 3e3);  // sqrt(9e6)，double 下逐位精确
}

TEST_CASE("coordinateHeadroom 标定：当前机位 625 → ≈3.08 且 ≥ 3（现在绿 = 批 1a 判据①）") {
	// 7.6e5/625 = 1216（整数除法精确），log10(1216) = 3.084933…（python 手算复核）。
	// 「≥ 3」= 债-85「本项目离触发差三个数量级」的机器化；反向半边在下一条用例。
	const double h = coordinateHeadroom(625.0);
	CHECK(std::isfinite(h));
	CHECK(h >= 3.0);
	expectNear(h, 3.084933, 1e-5);
}

TEST_CASE("coordinateHeadroom 反向：1e5 塌到 <1、1e6 塌到 <0（批 1a 判据②，阳性锚点）") {
	// 期望 = log10(7.6) = 0.880813…、log10(0.76) = -0.119186…（手算）。
	// 若触发常数被改成 +Infinity（变异本体），这两条一起红：余量不再塌。
	const double h5 = coordinateHeadroom(1e5);
	CHECK(h5 < 1.0);  // 进入开工凭据区（一个数量级以内）
	expectNear(h5, 0.880813, 1e-5);
	const double h6 = coordinateHeadroom(1e6);
	CHECK(h6 < 0.0);  // 已越过触发点
	expectNear(h6, -0.119186, 1e-5);
}

TEST_CASE("coordinateHeadroom 边界：相机恰在原点是合法输入 → +∞，不许 NaN") {
	// 债-85：抖动量级 = ULP(|camera|)，|camera| = 0 时没有抖动 ⟹ 余量是正无穷。
	// 阳性锚点：把这条特例改成返回 NaN（或让 0 落进 log10(7.6e5/0) 之外的分支出错值）即红。
	const double h = coordinateHeadroom(0.0);
	CHECK(!std::isnan(h));
	CHECK(std::isinf(h));
	CHECK(h > 0.0);
	// -0.0 与 0.0 比较相等 ⟹ 同走 +∞ 出口（IEEE 语义，写死防「用符号位判非法」的实现）
	CHECK(coordinateHeadroom(-0.0) == h);
}

TEST_CASE("coordinateHeadroom 非法输入：负距离 / NaN → NaN；+∞ → -∞（不假装合法）") {
	// 距离不可能为负 = 调用方 bug，必须出 NaN（比较恒 false = 响），而不是一个「看着合法」的数。
	CHECK(std::isnan(coordinateHeadroom(-625.0)));
	CHECK(std::isnan(coordinateHeadroom(-1e9)));
	CHECK(std::isnan(coordinateHeadroom(std::numeric_limits<double>::quiet_NaN())));
	// +∞ 走公式自然结果：无穷远 → 余量无穷负（照常触发开工凭据，不是 NaN 那种「不知道」）。
	const double h = coordinateHeadroom(std::numeric_limits<double>::infinity());
	CHECK(std::isinf(h));
	CHECK(h < 0.0);
}

TEST_CASE("rebaseSnapGranularity：取最粗级 = 最大值，且最大值刻意摆在数组中间") {
	// 反向用例（任务书点名）：最大值放中间，first / min / last 三种错法给出的粒度分别是
	// 2^-9 / 2^-9 / 2^-8（对第一组）与 2^-6 / 2^-7 / 2^-7（对第二组），全都不等于正确值 0.5。
	// 期望 = 「向上取到 2 的幂」闭式手算：0.5 与 0.3→0.5。
	const float midMax[] = {0.001f, 0.5f, 0.002f, 0.003f};
	CHECK(rebaseSnapGranularity(std::span<const float>(midMax)) == 0.5);
	const float midMaxFrac[] = {0.01f, 0.3f, 0.02f, 0.004f};
	CHECK(rebaseSnapGranularity(std::span<const float>(midMaxFrac)) == 0.5);
}

TEST_CASE("rebaseSnapGranularity：相机 ULP 错法对照（债-85 ② 的错法锚点，显著不同）") {
	// 正确值：texelSizes 最大值 0.03 → 向上取 2 的幂 = 2^-5 = 0.03125。
	// 错法值（§0.2 纠正二）：按相机 ULP 取粒度 = 2^(floor(log2 625)-23) = 2^-14
	//   = 6.103515625e-05（债-85 的台阶式，手算）。
	// 两者相差 512 倍 ⟹ 「按相机精度」与「按最粗纹素」在本判据下不可能混淆；
	// 若实现被改成吃相机距离（错法落地），本用例红。c0 读数 0.000545 出自债-139 实测。
	const float texelSizes[] = {0.000545f, 0.002f, 0.008f, 0.03f};
	const double correct = rebaseSnapGranularity(std::span<const float>(texelSizes));
	const double cameraUlp = 6.103515625e-05;  // 2^-14，手算
	CHECK(correct == 0.03125);
	CHECK(correct != cameraUlp);
	CHECK(correct / cameraUlp > 100.0);
}

TEST_CASE("rebaseSnapGranularity：输出恒为 2 的幂且落在 [max, 2·max)（Lcg 写死种子）") {
	// 「向上取到 2 的幂」的闭式契约：结果 m 满足 max ≤ m < 2·max 且 m 是 2 的幂
	// （格点嵌套性质的前提，snapOriginToGranularity 用例的另一半在这里钉）。
	Lcg rng(0xC0FFEE01u);  // 写死种子（确定性纪律）
	for (int i = 0; i < 200; ++i) {
		float steps[4];
		double maxStep = 0.0;
		for (float& s : steps) {
			s = static_cast<float>(rng.uniform(1e-4, 1e-1));
			maxStep = s > maxStep ? s : maxStep;
		}
		const double g = rebaseSnapGranularity(std::span<const float>(steps, 4));
		INFO("i = " << i << " maxStep = " << maxStep << " g = " << g);
		CHECK(isPowerOfTwo(g));
		CHECK(g >= maxStep);              // 只放大不缩小（覆盖性）
		CHECK(g < 2.0 * maxStep);         // 不跨过一整档（最粗级语义）
	}
}

TEST_CASE("rebaseSnapGranularity：非法输入出 NaN，🔴 不许返回 0 假装合法") {
	// 0 粒度会让 snap 退化成不 snap —— 正是本护栏要拦的形态，所以「不是 0」单独钉。
	const auto expectNanNotZero = [](std::span<const float> steps, const char* what) {
		const double g = rebaseSnapGranularity(steps);
		INFO("case = " << what << " g = " << g);
		CHECK(std::isnan(g));
		CHECK(g != 0.0);
	};
	expectNanNotZero(std::span<const float>{}, "empty");
	const float allZero[] = {0.0f, 0.0f, 0.0f, 0.0f};
	expectNanNotZero(std::span<const float>(allZero), "all-zero");
	const float withInf[] = {0.01f, std::numeric_limits<float>::infinity(), 0.02f};
	expectNanNotZero(std::span<const float>(withInf), "inf-entry");
	const float withNan[] = {std::numeric_limits<float>::quiet_NaN(), 0.01f};
	expectNanNotZero(std::span<const float>(withNan), "nan-entry");
	const float withNeg[] = {-1.0f, 0.5f};
	expectNanNotZero(std::span<const float>(withNeg), "negative-entry");
}

TEST_CASE("rebaseSnapGranularity：0 分量被跳过，既不污染「最粗级」也不把整组判成非法") {
	// 钉的是**防御语义**。⚠ 当前 `fillShadowData` 把四级全部填满，0 在现网并不出现——
	// **别照这条注释去改上游**。它防的是「级联数将来可配置 ⟹ 未生效槽留 0」那种形态：
	// 0 既不该被当成最粗级，也不该让整组判非法（两半边：混在正值里被跳过、
	// 全 0 才算无消费者 → 见上一条用例）。
	// 变异：删掉 `if (step == 0.0f) continue;` ⟹ 0 撞上 `!(step > 0)` 变成整组 NaN ⟹ 本条红
	//（主 agent 2026-10-25 独立验红）。
	const float withInactive[] = {0.0f, 0.25f, 0.0f};
	CHECK(rebaseSnapGranularity(std::span<const float>(withInactive)) == 0.25);
}

TEST_CASE("snapOriginToGranularity：逐轴格点闭式（含恰在格点上与半格向远离零取整）") {
	// 期望全部手算：v/g → 四舍五入（半格远离零）→ ×g。g = 16。
	// 1234.5/16 = 77.15625 → 77 → 1232；100/16 = 6.25 → 6 → 96；
	// 半格：8/16 = 0.5 → 1 → 16；-8 → -16；24/16 = 1.5 → 2 → 32（对称判据两半边都钉）。
	const double g = 16.0;
	glm::dvec3 s = snapOriginToGranularity(glm::dvec3{1234.5, -1234.5, 100.0}, g);
	CHECK(s.x == 1232.0);
	CHECK(s.y == -1232.0);
	CHECK(s.z == 96.0);
	s = snapOriginToGranularity(glm::dvec3{8.0, -8.0, 24.0}, g);
	CHECK(s.x == 16.0);
	CHECK(s.y == -16.0);
	CHECK(s.z == 32.0);
	// 恰在格点上是定点（幂等的前提）；0 也在格点上。
	s = snapOriginToGranularity(glm::dvec3{48.0, -32.0, 0.0}, g);
	CHECK(s.x == 48.0);
	CHECK(s.y == -32.0);
	CHECK(s.z == 0.0);
	// 幂等：snap(snap(x)) == snap(x)（对上面第一组结果再 snap 一次逐位不变）。
	const glm::dvec3 again = snapOriginToGranularity(s, g);
	CHECK(again.x == s.x);
	CHECK(again.y == s.y);
	CHECK(again.z == s.z);
}

TEST_CASE("snapOriginToGranularity：1e6 量级 + 2^-10 细格距 —— 除法必须走 double") {
	// 期望 = 1e6 + 2·2^-10 = 1000000.001953125（精确二进制值，可逐位断言）。
	// 输入 1e6 + 1.5·2^-10 在 double 下精确（32 个有效位）；x/g = 1024000001.5 →
	// 半格向远离零 → 1024000002 → ×g = 期望值。
	// 阳性锚点：除法若收窄成 float —— float 在 1e6 处 ULP = 0.0625 ≫ 2^-10，输入先被舍成
	// 1e6，结果 = 1e6 ≠ 期望，本用例红（差 0.001953125，是 float 舍入噪声的 3 倍）。
	const double g = 0.0009765625;  // 2^-10，精确二进制
	const glm::dvec3 s = snapOriginToGranularity(
		glm::dvec3{1e6 + 0.00146484375, 1e6, -1e6 - 0.00146484375}, g);
	CHECK(s.x == 1000000.001953125);
	CHECK(s.y == 1e6);   // 恰在格点上（1e6 = 2^10·… ×g 的整数倍：1e6/2^-10 = 1024000000）
	CHECK(s.z == -1000000.001953125);  // 负轴同规则：-1024000001.5 → -1024000002
}

TEST_CASE("snapOriginToGranularity：非法 granularity（0/负/NaN/inf）→ 全分量 NaN") {
	// 「0 粒度 = 不 snap」是 rebaseSnapGranularity 注释点名要拦的静默退化形态；
	// 本函数不许配合放行 —— 返回 desired 原样（静默不 snap）或返回 0 向量都算错法。
	const glm::dvec3 desired{1234.5, -678.0, 42.0};
	checkAllNan(snapOriginToGranularity(desired, 0.0));
	checkAllNan(snapOriginToGranularity(desired, -16.0));
	checkAllNan(snapOriginToGranularity(desired, std::numeric_limits<double>::quiet_NaN()));
	checkAllNan(snapOriginToGranularity(desired, std::numeric_limits<double>::infinity()));
}

TEST_CASE("snapOriginToGranularity：格点嵌套 —— 粗格线必落在细格线上（Lcg 写死种子）") {
	// 债-139 结案依赖的性质：格距同为 2 的幂（gCoarse = 8·gFine）⟹ 先 snap 到粗格的结果
	// 再 snap 到细格**逐位不动**；反向不成立（细格点大多不在粗格上）——两半边各钉：
	// ① 全部样本粗格结果在细格上；② 过半样本的细格结果不在粗格上（前提守卫，防输入退化假绿）。
	const double gFine = 0.0078125;    // 2^-7
	const double gCoarse = 0.0625;     // 2^-4 = 8·gFine
	Lcg rng(0x5EEDC0FFu);  // 写死种子
	int movedByFine = 0;
	for (int i = 0; i < 500; ++i) {
		const glm::dvec3 p{rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6)};
		const glm::dvec3 coarse = snapOriginToGranularity(p, gCoarse);
		const glm::dvec3 resnapped = snapOriginToGranularity(coarse, gFine);
		INFO("i = " << i << " p = " << p.x << "," << p.y << "," << p.z);
		CHECK(resnapped.x == coarse.x);  // 半边①：粗格线在细格线上
		CHECK(resnapped.y == coarse.y);
		CHECK(resnapped.z == coarse.z);
		const glm::dvec3 fine = snapOriginToGranularity(p, gFine);
		if (fine.x != coarse.x)
			++movedByFine;
	}
	CHECK(movedByFine > 250);  // 半边②的前提守卫：细/粗格确实会给出不同结果（500 样本过半）
}

TEST_CASE("接缝对账：granularity 产物直接喂 snap，落点恒为格点（两函数契约互锁）") {
	// rebaseSnapGranularity 的输出（2 的幂）作为 snapOriginToGranularity 的格距：
	// 任意期望原点 snap 后必须精确落在格点上（除以格距是整数 ⟹ 再 snap 逐位不动 = 幂等）。
	// 这条钉的是「rebase 唯一允许的粒度」与「唯一允许的 snap」的接缝，单独测任一条都盖不住。
	const float texelSizes[] = {0.000545f, 0.002f, 0.008f, 0.03f};
	const double g = rebaseSnapGranularity(std::span<const float>(texelSizes));
	CHECK(g == 0.03125);  // 手算：0.03 → 2^-5
	const glm::dvec3 desired{123456.789, -987654.321, 625.0};
	const glm::dvec3 snapped = snapOriginToGranularity(desired, g);
	for (int axis = 0; axis < 3; ++axis) {
		const double q = snapped[axis] / g;
		INFO("axis = " << axis << " q = " << q);
		CHECK(q == std::round(q));            // 闭式：落点 = 格距整数倍
	}
	const glm::dvec3 again = snapOriginToGranularity(snapped, g);
	CHECK(again.x == snapped.x);
	CHECK(again.y == snapped.y);
	CHECK(again.z == snapped.z);
}

TEST_CASE("平移精确性：snap(c + k·g) 逐位等于 snap(c) + k·g（g 为 2 的幂）—— 批 5 图集级零容差判据的数学内核") {
	// 出处：docs/相对坐标地基_plan.md §3 批 5「判据分两级」表 + coordinate_rebase.cppm
	// 「2 的幂倍平移是精确浮点运算」段。闭式推导（期望值出处 = 这条恒等式本身，不抄被测码）：
	//   round((c + k·g)/g)·g = round(c/g + k)·g = (round(c/g) + k)·g = snap(c) + k·g
	// 中间那步成立的前提 = `c/g + k` **不改变 c/g 的尾数** ⟺ k 为整数 **且 g 是 2 的幂**。
	// ⟹ 吸附后的光空间盒恰好平移 ΔO、与场景几何完全同构 ⟹ **阴影图集内容逐位不变**。
	// ⚠ 屏幕级**不**成立（对象矩阵 / MVP 平移列的值变了、浮点路径不同），那一级只能做
	//   「差异落在闭式精度界内」——两级别混。
	// 🔴 变异：摘掉 rebaseSnapGranularity 里的 quantizeShadowDistance（g 变非 2 的幂）→ 本条红。
	Lcg rng(0x524C2026u);  // 写死种子（可重跑，禁 rand/random_device）
	int nonzero = 0;
	int onGrid = 0;
	for (int i = 0; i < 3000; ++i) {
		const double g = std::ldexp(1.0, rng.uniformInt(-12, 6));  // 2 的幂格距：2^-12 … 2^6
		REQUIRE(isPowerOfTwo(g));                                   // 前提：本用例只在 2 的幂上求值
		const double k = rng.uniformInt(-5000, 5000);               // 整数倍平移量
		const glm::dvec3 c{rng.uniform(-8e5, 8e5), rng.uniform(-8e5, 8e5), rng.uniform(-8e5, 8e5)};
		const glm::dvec3 shift{k * g, k * g, k * g};
		const glm::dvec3 lhs = snapOriginToGranularity(c + shift, g);
		const glm::dvec3 rhs = snapOriginToGranularity(c, g) + shift;
		INFO("g = " << g << "  k = " << k << "  c = " << c.x << ", " << c.y << ", " << c.z);
		CHECK(lhs.x == rhs.x);  // == 对有限 double 即逐位相同
		CHECK(lhs.y == rhs.y);
		CHECK(lhs.z == rhs.z);
		// 前提守卫（防「函数恒返回 0」这类退化实现让恒等式两边同时塌成 0 ⟹ 全绿假绿）：
		if (lhs.x != 0.0 || lhs.y != 0.0 || lhs.z != 0.0)
			++nonzero;
		const double q = lhs.x / g;
		if (q == std::round(q))
			++onGrid;  // 落点恒为格距整数倍（snap 的闭式契约，本身即有效断言）
	}
	CHECK(nonzero > 2900);  // 几乎全部样本确实 snap 出了非零值
	CHECK(onGrid == 3000);
}

TEST_CASE("平移精确性的反向半边：g 不是 2 的幂时那条恒等式**确实**会破（证明「2 的幂」是承重前提，不是装饰）") {
	// 没有这条，上一条会被后来者读成「任何格距都成立」——而把粒度改成"更细的任意值"
	// 看起来像优化。真实失效形态：ΔO 不再是精确平移 ⟹ 吸附后的光空间盒与场景几何不同构
	// ⟹ 阴影图集内容变 ⟹ 批 5 那条零容差判据失效（而它是唯一能便宜抓住这件事的仪器）。
	Lcg rng(0x4F72696Eu);  // 写死种子
	int broken = 0;
	int evaluated = 0;
	for (int i = 0; i < 3000; ++i) {
		const double g = rng.uniform(0.001, 0.02);
		if (isPowerOfTwo(g))
			continue;  // 随机 double 恰为 2 的幂的概率≈0，但别让它污染计数
		++evaluated;
		const double k = rng.uniformInt(-5000, 5000);
		const glm::dvec3 c{rng.uniform(-8e5, 8e5), rng.uniform(-8e5, 8e5), rng.uniform(-8e5, 8e5)};
		const glm::dvec3 shift{k * g, k * g, k * g};
		const glm::dvec3 lhs = snapOriginToGranularity(c + shift, g);
		const glm::dvec3 rhs = snapOriginToGranularity(c, g) + shift;
		if (lhs.x != rhs.x || lhs.y != rhs.y || lhs.z != rhs.z)
			++broken;
	}
	REQUIRE(evaluated > 2900);  // 前提守卫：样本确实落在「非 2 的幂」上
	// 实测读数（本机 RADV 主机侧、写死种子 0x4F72696E、2026-10-25）：
	//   broken = **1690 / evaluated = 3000** ⟹ 56% 的样本恒等式破。
	// 阈值取 5%（= 实测的 1/11，留一个数量级余量）。它要抓的是两种形态：
	//   ① 完全不破 ⟹ 本条根本没在测东西（假绿）；
	//   ② 只剩零星破 ⟹ 信号弱到不能当判据。
	// 🔴 不把阈值贴到实测值上：换编译器 / 换 libm 就会假红，而这条要的是「量级性差异」的判据。
	CHECK(broken > evaluated / 20);
}

// ─── 批 3 第一刀：toRelative（全仓唯一「绝对 → 相对」换算入口）───────────────

TEST_CASE("toRelative 门关恒等：origin = 0 与 glm::mat4(A) 逐位相同（本批主判据的单元级对应）") {
	// 期望值出处 = IEEE 闭式事实：c3 各分量减 +0.0 逐位返回自身，收窄步同一次
	// ⟹ 两条路径必须逐位相同。覆盖 identity / 手构整数+大数矩阵 / Lcg 随机（写死种子，
	// 小量级与 1e6 量级两档）。实现若动了 c3.w、动了基向量列、或引入任何非「减零」的运算，
	// 本条红。门关态（原点恒 0）逐位不变 = docs/相对坐标地基_plan.md §2.2 的交付形态。
	const glm::dmat4 identity{1.0};
	checkMat4BitwiseEqual(toRelative(identity, glm::dvec3{0.0}), glm::mat4(identity));
	glm::dmat4 hand{1.0};
	hand[0] = glm::dvec4{2.0, 0.0, 0.0, 0.0};
	hand[1] = glm::dvec4{0.0, 3.0, 0.0, 0.0};
	hand[2] = glm::dvec4{0.0, 0.0, 4.0, 0.0};
	hand[3] = glm::dvec4{1e6, -2.5e5, 0.125, 1.0};
	checkMat4BitwiseEqual(toRelative(hand, glm::dvec3{0.0}), glm::mat4(hand));
	Lcg rng(0x6A7E2E00u);  // 写死种子（确定性纪律）
	for (int i = 0; i < 300; ++i) {
		const double magnitude = (i & 1) ? 1.0 : 1e6;  // 小量级与 1e6 级大数两档
		const glm::dmat4 a = randomDmat4(rng, magnitude);
		const glm::mat4 got = toRelative(a, glm::dvec3{0.0});
		INFO("i = " << i << " magnitude = " << magnitude);
		checkMat4BitwiseEqual(got, glm::mat4(a));
	}
}

TEST_CASE("toRelative 顺序承重：先减后收窄 ≠ 先收窄后减（阳性锚点，本函数唯一的存在理由）") {
	// 构造：|translation| ≈ 1e6 且 origin 与之接近。float32 在 1e6 处 ULP = 2^(19-23) = 0.0625
	// （1e6 ∈ [2^19, 2^20)，14 位尾数）⟹ 亚 ULP 偏移 2^-30/2^-29/2^-31 在 float32 里**不可见**：
	//   先减后收窄（本函数）：float((1e6 + 2^-30) − 1e6) = float(2^-30) = 2^-30 ——
	//     double 减法精确（1e6 + 2^-30 只需 51 个有效位 < 53），结果量级小，float 存得下；
	//   先收窄后减（错法）：float(1e6 + 2^-30) = 1e6（就近舍入），再 1e6 − 1e6 = 0 —— 信息已丢。
	// 期望值 = 上面两行闭式手算（出处 = ULP 定义，不抄被测实现）。
	// 🔴 没有「两种顺序结果不同」的后半边，把实现的两行调换顺序测试照样绿 ——
	// 而顺序正是本函数存在的全部理由（camera-relative 的精度收益所在）。
	glm::dmat4 a{1.0};
	a[3] = glm::dvec4{1e6 + 0x1.0p-30, 1e6 + 0x1.0p-29, 1e6 + 0x1.0p-31, 1.0};
	const glm::dvec3 origin{1e6};
	// 前提守卫：输入确实「float32 分不出」（这条红了 = 输入构造退化，本用例失去判据力）
	REQUIRE(glm::mat4(a)[3][0] == 1000000.0f);
	REQUIRE(glm::mat4(a)[3][2] == 1000000.0f);
	const glm::mat4 got = toRelative(a, origin);
	// 正确顺序的期望（手算）：亚 ULP 偏移原样活下来
	CHECK(got[3][0] == 0x1.0p-30f);
	CHECK(got[3][1] == 0x1.0p-29f);
	CHECK(got[3][2] == 0x1.0p-31f);
	CHECK(got[3][3] == 1.0f);  // w 不动
	// 错序半边：先收窄、再在 float 域减，三分量全部塌成 0，与正确结果逐分量不同
	const glm::vec4 narrowedFirst = glm::mat4(a)[3] - glm::vec4(glm::vec3(origin), 0.0f);
	CHECK(narrowedFirst == glm::vec4{0.0f, 0.0f, 0.0f, 1.0f});
	CHECK(got[3][0] != narrowedFirst.x);
	CHECK(got[3][1] != narrowedFirst.y);
	CHECK(got[3][2] != narrowedFirst.z);
}

TEST_CASE("toRelative 只动平移列：c0/c1/c2 与 c3.w 逐位不变（四列各断言，不只整体 ==）") {
	// 期望值 = 闭式定义：c<3 列 = float(A[c][r])（纯收窄，减法不参与）；
	// c3.xyz = float(A[3][i] − origin[i])（double 减后收窄）；c3.w = float(A[3][3])。
	// 阳性守卫：c3.xyz 确实动了 —— 否则「只动平移列」会被「什么都不动」的实现满足。
	Lcg rng(0x701EE500u);  // 写死种子
	int moved = 0;
	for (int i = 0; i < 300; ++i) {
		const glm::dmat4 a = randomDmat4(rng, 1e3);
		const glm::dvec3 origin{rng.uniform(-500.0, 500.0), rng.uniform(-500.0, 500.0),
		    rng.uniform(-500.0, 500.0)};
		const glm::mat4 got = toRelative(a, origin);
		const glm::mat4 narrowed = glm::mat4(a);  // 纯收窄对照（不含减法）
		INFO("i = " << i);
		// 基向量三列逐列断言：与纯收窄逐位相同
		for (int col = 0; col < 3; ++col)
			for (int row = 0; row < 4; ++row)
				CHECK(got[col][row] == narrowed[col][row]);
		// c3.w 逐位不变（dvec4(origin, 0.0) 的 w 恰是 +0.0，减零精确）
		CHECK(got[3][3] == narrowed[3][3]);
		// c3.xyz 闭式期望：double 域减完再收窄
		for (int row = 0; row < 3; ++row)
			CHECK(got[3][row] == static_cast<float>(a[3][row] - origin[row]));
		if (got[3][0] != narrowed[3][0] || got[3][1] != narrowed[3][1] ||
		    got[3][2] != narrowed[3][2])
			++moved;
	}
	CHECK(moved == 300);  // 原点非零且随机 ⟹ 每个样本的平移列都必须被减到（实测恰 300）
}

TEST_CASE("绝对层提升无损：glm::mat4(glm::dmat4(m)) 逐位等于 m（生产点填法的根据）") {
	// 期望值出处 = IEEE 闭式事实：float→double 是精确的单射，double→float 就近舍入回原值
	// ⟹ 往返逐位不变。这就是批 3 第一刀三个生产点（model_importer_impl 两处 /
	// builtin_geometry_impl 一处）「absolute = dmat4(现有 float 值)、world = mat4(absolute)」
	// 能逐位不变的全部理由。随机 float 矩阵（写死种子），量级拉到 1e6 档。
	Lcg rng(0x1055E000u);  // 写死种子
	for (int i = 0; i < 300; ++i) {
		glm::mat4 m{1.0f};
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				m[col][row] = static_cast<float>(rng.uniform(-1e6, 1e6));
		m[3][3] = 1.0f;
		checkMat4BitwiseEqual(glm::mat4(glm::dmat4(m)), m);
	}
}

TEST_CASE("toRelative 组合：减过一次原点后再减 0 逐位不变（门关路径对已相对矩阵同样恒等）") {
	// toRelative(toRelative(A, o1), dvec3(0)) 与 toRelative(A, o1) 逐位相同。
	// 外层入参要显式 glm::dmat4(once) 提升（float→double 精确；GLM_FORCE_EXPLICIT_CTOR
	// 下漏写提升 = 编译错误，编译器替这条站岗）。
	// 意义：门关态（原点 0）对**任何**输入都是恒等，「已经收窄落库」的矩阵再走一遍
	// 换算入口不引入任何新舍入 —— 批 4 消费者接线依赖这个性质。
	Lcg rng(0x2E50EE00u);  // 写死种子
	for (int i = 0; i < 300; ++i) {
		const glm::dmat4 a = randomDmat4(rng, 1e6);
		const glm::dvec3 o1{rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6)};
		const glm::mat4 once = toRelative(a, o1);
		const glm::mat4 twice = toRelative(glm::dmat4(once), glm::dvec3{0.0});
		INFO("i = " << i);
		checkMat4BitwiseEqual(twice, once);
	}
}

// ─── 批 4 第一刀：toRelativeModel + motionWithOriginShift（跨帧契约的两半）────────
//
// 期望值出处（**全部独立于被测实现**，逐条不抄 coordinate_rebase.cppm 的算式）：
//  渲染世界坐标的**定义**（docs/相对坐标地基_plan.md §3 批 3 末的 spin 锚点裁决 + 批 3 的
//  toRelative 契约）：第 t 帧上资产局部点 p 的世界坐标
//      W_t = S_t·A·p − O_t
//  S = 该帧 spin（绕绝对原点）、A = 图元绝对矩阵、O = 该帧渲染原点。
//  跨帧运动矩阵的定义 = 满足 `motion·W_t = W_{t−1}` 的那个矩阵。
//  ⟹ 下面每条用例的期望值都从这两行**定义**推出来，不从被测函数的表达式推。

namespace {

/** 造一个绕随机单位轴的随机角旋转（与 SceneSpin::matrix 同形：纯旋转、平移列全 0）。 */
glm::mat4 randomSpin(Lcg& rng) {
	const glm::vec3 axis{static_cast<float>(rng.uniform(-1.0, 1.0)),
		static_cast<float>(rng.uniform(-1.0, 1.0)), static_cast<float>(rng.uniform(-1.0, 1.0))};
	const float len = glm::length(axis);
	REQUIRE(len > 1e-3f);  // 前提守卫：轴太短 ⟹ glm::rotate 归一化除零出 NaN
	return glm::rotate(glm::mat4{1.0f}, static_cast<float>(rng.uniform(-3.1, 3.1)), axis / len);
}

/** 按**定义**算第 t 帧的世界坐标 `S·A·p − O`（double，全程不碰被测函数）。 */
glm::dvec3 worldByDefinition(const glm::mat4& spin, const glm::dmat4& asset, const glm::dvec3& p,
                             const glm::dvec3& origin) {
	const glm::dvec4 spun = glm::dmat4(spin) * (asset * glm::dvec4(p, 1.0));
	return glm::dvec3(spun) - origin;
}

/** float 矩阵 × double 点（w=1）→ double 点。被测矩阵是 float 落库形态，期望是 double。 */
glm::dvec3 applyPoint(const glm::mat4& m, const glm::dvec3& v) {
	const glm::dvec4 got = glm::dmat4(m) * glm::dvec4(v, 1.0);
	return glm::dvec3(got);
}

}  // namespace

TEST_CASE("🔴 跨帧契约两半互锁：motion · toRelativeModel(本帧) 逐点对上 toRelativeModel(上一帧)") {
	// 这条是**唯一**能抓住「实例矩阵生产顺序与 motion 推导不一致」的机器判据。
	// 两半原本分住两个文件、中间只有一条注释连着（AGENTS 哲学第 4 条：注释不是防线）；
	// 实测把生产顺序改回旧的 `spin.matrix * toRelative(...)`，**全部单测不红**
	// （2026-10-25 变异 M8）—— 因为门关 spin = 单位阵时两种顺序逐位相同，出厂截图锚也抓不到。
	// 收进同一个模块 + 这条互锁断言之后，顺序错 = 本条红。
	// 期望值出处 = 上面那两行**定义**：motion 的定义就是「满足 motion·W_t = W_{t−1}」。
	Lcg rng(0x4C6F636Bu);  // 写死种子（"lockn"）
	int checked = 0;
	double minWrongOrderGap = 1e30;
	for (int i = 0; i < 300; ++i) {
		const glm::mat4 spinCur = randomSpin(rng);
		const glm::mat4 spinPrev = randomSpin(rng);
		glm::dmat4 asset{1.0};
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				asset[col][row] = rng.uniform(-40.0, 40.0);
		asset[3][3] = 1.0;
		const glm::dvec3 p{rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0)};
		const glm::dvec3 originPrev{rng.uniform(-900.0, 900.0), rng.uniform(-900.0, 900.0),
		    rng.uniform(-900.0, 900.0)};
		const glm::dvec3 originCur{rng.uniform(-900.0, 900.0), rng.uniform(-900.0, 900.0),
		    rng.uniform(-900.0, 900.0)};

		// 两半各按自己的被测入口算，再对拍
		const glm::mat4 modelCur = toRelativeModel(spinCur, asset, originCur);
		const glm::mat4 modelPrev = toRelativeModel(spinPrev, asset, originPrev);
		const glm::mat4 motion = motionWithOriginShift(
		    spinPrev * glm::inverse(spinCur), originPrev, originCur);
		const glm::dvec3 got = applyPoint(motion, applyPoint(modelCur, p));
		const glm::dvec3 expect = applyPoint(modelPrev, p);
		INFO("i = " << i);
		for (int axis = 0; axis < 3; ++axis)
			expectNear(got[axis], expect[axis], 1e-5, 1e-3);

		// 同一样本量一下「生产顺序错」会差多远（旧顺序 = spin 加在减法之后）
		const glm::mat4 wrongModelCur = spinCur * toRelative(asset, originCur);  // 批 3 否掉的形态
		const glm::dvec3 wrong = applyPoint(motion, applyPoint(wrongModelCur, p));
		minWrongOrderGap = std::min(minWrongOrderGap, glm::length(wrong - expect));
		++checked;
	}
	CHECK(checked == 300);
	// 前提守卫：错顺序与正解**确实**分得开（否则上面那条容差是空的）。
	// 阈值只要求「远大于 1e-3 的容差地板」，不贴实测读数。
	CHECK(minWrongOrderGap > 1.0);
}

TEST_CASE("toRelativeModel 门关恒等：origin = 0 且 spin = 单位阵 ⟹ 逐位 = glm::mat4(absolute)") {
	// 出厂态（configs/scene.lua `spin = false` ⟹ spin.matrix 恒单位阵）的逐位锚：
	// 本批把生产点的复合顺序换了，这条钉「出厂态一个 bit 都没动」。
	// 期望 = 纯收窄 `glm::mat4(absolute)`（IEEE 闭式：乘单位阵只留 `x*1` 那一项，
	// 其余是 ±0，累加结果与直接收窄同值）。
	Lcg rng(0x4D6F646Cu);  // 写死种子（"Modl"）
	for (int i = 0; i < 300; ++i) {
		const glm::dmat4 asset = randomDmat4(rng, (i & 1) ? 1.0 : 1e6);
		checkMat4BitwiseEqual(toRelativeModel(glm::mat4{1.0f}, asset, glm::dvec3{0.0}),
		    glm::mat4(asset));
	}
}

TEST_CASE("motionWithOriginShift 门关恒等：两帧原点都是 0 ⟹ 逐位返回 spinRatio（Lcg 写死种子）") {
	// 本批主判据（门关逐位不变，计划 §3 批 4 表第 ① 条）的**单元级**对应。出厂截图锚守的是
	// 整条链，这条守的是「门关时 motion 一个 bit 都不动」本身。
	// 🔴 期望 = 入参**逐位相同**（不是 Approx）：门关态下游 taa.frag / velocity.frag 吃的
	// 就是这份 float 矩阵，任何一次多余舍入都会跨进程漂。
	// 覆盖三种 spinRatio：单位阵 / 真旋转比（`prevSpin·inverse(curSpin)`，它的平移列里
	// **实际带 −0.0**，正是恒等早退要守的那一档）/ Lcg 随机满矩阵。
	const auto gateClosed = [](const glm::mat4& ratio) {
		checkMat4BitwiseEqual(motionWithOriginShift(ratio, glm::dvec3{0.0}, glm::dvec3{0.0}), ratio);
	};
	gateClosed(glm::mat4{1.0f});
	gateClosed(glm::mat4{1.0f} * glm::mat4{1.0f});  // 乘出来的单位阵
	Lcg rng(0x4C657374u);  // 写死种子（确定性纪律）
	for (int i = 0; i < 400; ++i) {
		const glm::mat4 ratio = (i & 1) == 0
		    ? randomSpin(rng) * glm::inverse(randomSpin(rng))  // 生产同形：spinRatio
		    : [&] {
			    glm::mat4 m{1.0f};
			    for (int col = 0; col < 4; ++col)
				    for (int row = 0; row < 4; ++row)
					    m[col][row] = static_cast<float>(rng.uniform(-1e3, 1e3));
			    m[3][3] = 1.0f;
			    return m;
		    }();
		INFO("i = " << i);
		gateClosed(ratio);
	}
}

TEST_CASE("🔴 motionWithOriginShift 恒等早退的专属锚点：平移列带 −0.0 必须活着回来") {
	// 这条**只**守早退那两行，删掉早退（改成靠「加 0 也是恒等」兜）它必须红。
	// ## 为什么盯矩阵第 3 列（平移列），而不是「16 个分量里出现过 −0.0」
	// 本函数只改第 3 列，−0.0 藏在别的列根本不构成判据。第一版就是把锚点写成
	// 「16 分量里出现过 −0.0」，M1 变异（删早退）实测 **0 红** —— 空判据（2026-10-25 抓到）。
	// ## 为什么 IEEE 事实在这里是承重的
	// `x + 0.0f` 把 −0.0f 洗成 +0.0f，而 `x − 0.0f` 逐位返回自身（含 ±0）。本函数的复合步
	// 用的是**加法**（`absolute[3] = r[3] + R·O_t`），门关时加的就是 ±0 ⟹ 没有早退就会翻号。
	// ## ⚠ 作用域如实报（别把这条读成「生产现在就在漏 −0.0」）
	// 生产入参 `prevSpin * glm::inverse(curSpin)` 的那次矩阵乘，累加从 `0 + …` 起步，
	// 已经把 −0.0 归一成 +0.0（实测 20000 组随机旋转比：平移列带 −0.0 的比例 = 0/20000）。
	// ⟹ 本条是**函数契约级**守卫（本函数不许改入参里任何一位），不是现网缺陷的守卫。
	// 反过来 `glm::inverse` 单独是产 −0.0 的（实测 `rotate(...)·`… 的逆的平移列 4 个分量里
	// 有 2 个是 −0.0），所以「入参带 −0.0」这个输入形态离生产并不远 —— 少一次乘法就进来了。
	glm::mat4 negZeroTranslation{1.0f};
	negZeroTranslation[3] = glm::vec4{-0.0f, -0.0f, -0.0f, 1.0f};
	// 锚点回读自证（rgl-test-discipline §锚点回读）：铺进去的确实是 −0.0，且只有第 3 列有
	for (int row = 0; row < 3; ++row)
		REQUIRE(std::signbit(negZeroTranslation[3][row]));
	for (int col = 0; col < 3; ++col)
		for (int row = 0; row < 4; ++row)
			REQUIRE(!std::signbit(negZeroTranslation[col][row]));

	const glm::mat4 got = motionWithOriginShift(negZeroTranslation, glm::dvec3{0.0}, glm::dvec3{0.0});
	checkMat4BitwiseEqual(got, negZeroTranslation);
	for (int row = 0; row < 3; ++row)
		CHECK(std::signbit(got[3][row]));  // −0.0 原样出来 = 早退被走到（删早退 ⟹ 三条全红）

	// 反向半边：同一个入参喂**非零**原点，第 3 列必须被改到 —— 证明上面那条绿不是
	// 「函数什么都不做」（那正是「接了但没接上」的形态）。
	const glm::mat4 moved =
	    motionWithOriginShift(negZeroTranslation, glm::dvec3{0.0}, glm::dvec3{256.0, 0.0, 0.0});
	CHECK(moved[3][0] == 256.0f);
	CHECK(!std::signbit(moved[3][0]));  // 加上了 256 就不是 −0.0 了
	CHECK(moved[3][1] == 0.0f);
}

TEST_CASE("🔴 ΔO 承重阳性锚点：静止场景 + 移动原点 ⟹ motion 作用后的世界坐标差恰为 ΔO") {
	// 本批的灵魂断言（计划 §3 批 4 表第 ③ 条 = 门开态的独立判据）。
	// 期望值出处 = 定义的两行：R = I ⟹ W_{t−1} = W_t + (O_t − O_{t−1})，**逐位**成立
	// （构造量全取 float 可精确表示的二进制数，加法无舍入 ⟹ 不许用容差糊过去）。
	// 静止相机 + 静止场景 + 移动原点：世界坐标每帧都在变，而画面**不该**变。少了这一项，
	// 「世界坐标差 = 运动」的判据全部被骗 ⟹ TAA 把静止判成运动（历史被丢、时域噪声回来）。
	const glm::dvec3 prevOrigin{0.0, 0.0, 0.0};
	const glm::dvec3 curOrigin{1024.0, -256.0, 8.0};  // 2 的幂 ⟹ float 精确
	const glm::dvec3 delta = curOrigin - prevOrigin;
	const glm::mat4 motion = motionWithOriginShift(glm::mat4{1.0f}, prevOrigin, curOrigin);

	int checked = 0;
	for (double x : {-2048.0, -3.5, 0.0, 7.25, 512.0}) {
		for (double y : {-1024.0, 0.5, 31.0, -0.25}) {
			for (double z : {-64.0, 0.0, 1.5, 96.0}) {
				const glm::dvec3 w{x, y, z};  // 本帧世界坐标（float 可精确表示）
				const glm::dvec3 mapped = applyPoint(motion, w);
				INFO("w = " << w.x << "," << w.y << "," << w.z);
				// 「恰为 ΔO」= 逐位：期望 = w + delta，两侧都是 float 精确值
				CHECK(mapped.x == w.x + delta.x);
				CHECK(mapped.y == w.y + delta.y);
				CHECK(mapped.z == w.z + delta.z);
				++checked;
			}
		}
	}
	CHECK(checked == 80);  // 空转守卫：三层循环被改空立刻红
	// 反向半边（缺了它，「motion 恒等」的实现也能满足上面全部断言）：
	// 不复合原点（= 现行 `prevSpin·inverse(curSpin)` 那条纯旋转式子）时，同一个点的映射差
	// **必须**是 0 而不是 ΔO —— 差值恰为 ΔO 这件事才是本批新增的承重。
	const glm::dvec3 probe{7.25, 0.5, 1.5};
	const glm::dvec3 unmoved = applyPoint(glm::mat4{1.0f}, probe);
	CHECK(unmoved == probe);
	CHECK(applyPoint(motion, probe) - unmoved == delta);
}

TEST_CASE("motionWithOriginShift 反向用例：摘掉 ΔO 项（只喂纯旋转比）⟹ 静止被判成运动") {
	// 这条把「灵魂断言」的反向半边独立成案：同一段静止场景 + 移动原点，用**改前**的式子
	// （spinRatio 本身，不复合原点）算映射差，结果必须是 0，而真值是 ΔO ≠ 0。
	// ⟹ 断言的是「两者不同」，所以任何把 motionWithOriginShift 改成 `return spinRatio;` 的
	// 变异都会让上面那条红；而这条自己钉的是「错法给出的量级确实差一个 ΔO」，
	// 用来证明上面那条的容差不是空的（rgl-test-discipline：容差必须远小于信号）。
	const glm::dvec3 prevOrigin{-512.0, 256.0, 128.0};
	const glm::dvec3 curOrigin{512.0, 0.0, -128.0};
	const glm::dvec3 delta = curOrigin - prevOrigin;  // (1024, -256, 256)，全 float 精确
	REQUIRE(delta != glm::dvec3{0.0});
	const glm::dvec3 w{16.0, -8.0, 4.0};

	const glm::mat4 correct = motionWithOriginShift(glm::mat4{1.0f}, prevOrigin, curOrigin);
	const glm::dvec3 correctDiff = applyPoint(correct, w) - w;
	const glm::dvec3 naiveDiff = applyPoint(glm::mat4{1.0f}, w) - w;  // 改前的式子

	CHECK(correctDiff == delta);
	CHECK(naiveDiff == glm::dvec3{0.0});
	CHECK(correctDiff != naiveDiff);
	// 量级判据：错法与正解之差必须**巨大**（这里恰为 |ΔO| = 1024 世界单位），
	// 而不是「比浮点噪声大一点」。velocity 链每帧照算照输出（不吃 historyValid），
	// 所以这个量级的假运动是每帧都付的。
	CHECK(glm::length(correctDiff - naiveDiff) > 1e3);
}

TEST_CASE("motionWithOriginShift 全链闭式：含 spin 时 motion·W_t 对上按定义算的 W_{t−1}（Lcg）") {
	// 一般形态（R ≠ I 且两帧原点不同）的闭式对拍。期望值 = `worldByDefinition`，
	// 它只吃「W_t = S_t·A·p − O_t」这条**定义**，完全不走被测函数的表达式。
	// 容差 1e-5（相对）：motion 是 float 落库矩阵，一次 4×4 乘加 + 一次收窄，
	// 而量级 ~1e3 的世界坐标在 float32 处 ULP ≈ 6e-5 ⟹ 1e-5 相对 ≈ 0.01 单位，
	// 是浮点地板的几倍、是「摘掉 ΔO 项」那种错法（差 ~1e3 单位）的十万分之一。
	Lcg rng(0x4D6F7469u);  // 写死种子
	int checked = 0;
	double minSignal = 1e30;
	for (int i = 0; i < 300; ++i) {
		const glm::mat4 spinCur = randomSpin(rng);
		const glm::mat4 spinPrev = randomSpin(rng);
		glm::dmat4 asset{1.0};
		for (int col = 0; col < 4; ++col)
			for (int row = 0; row < 4; ++row)
				asset[col][row] = rng.uniform(-40.0, 40.0);
		asset[3] = glm::dvec4{asset[3].x, asset[3].y, asset[3].z, 1.0};
		const glm::dvec3 p{rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0)};
		const glm::dvec3 prevOrigin{rng.uniform(-900.0, 900.0), rng.uniform(-900.0, 900.0),
		    rng.uniform(-900.0, 900.0)};
		const glm::dvec3 curOrigin{rng.uniform(-900.0, 900.0), rng.uniform(-900.0, 900.0),
		    rng.uniform(-900.0, 900.0)};

		const glm::mat4 spinRatio = spinPrev * glm::inverse(spinCur);
		const glm::mat4 motion = motionWithOriginShift(spinRatio, prevOrigin, curOrigin);
		const glm::dvec3 wCur = worldByDefinition(spinCur, asset, p, curOrigin);
		const glm::dvec3 expectPrev = worldByDefinition(spinPrev, asset, p, prevOrigin);
		const glm::dvec3 got = applyPoint(motion, wCur);
		INFO("i = " << i);
		for (int axis = 0; axis < 3; ++axis)
			expectNear(got[axis], expectPrev[axis], 1e-5, 1e-3);

		// 同一样本上量一下「摘掉 ΔO 项」的错法差多远，并累计最小值 —— 它必须**始终**远大于
		// 上面的容差，否则容差就是空的（阳性锚点的另一半）。
		const double naiveGap = glm::length(applyPoint(spinRatio, wCur) - expectPrev);
		minSignal = std::min(minSignal, naiveGap);
		++checked;
	}
	CHECK(checked == 300);
	CHECK(minSignal > 1.0);  // 单位：世界单位。容差上限 1e-3，信号地板 >1 ⟹ 差三个数量级
}

TEST_CASE("🔴 spin 锚点在绝对原点：原点不动但在自旋时，纯旋转比**不够**（平移 = (R−I)·O）") {
	// 这条钉的是批 3 的 spin 锚点裁决（docs/相对坐标地基_plan.md §3 批 3 末）在 motion 里的
	// 后果，也是「`prevSpin·inverse(curSpin)` 在该裁决下是否仍然成立」这个问题的**机器答案**：
	// 不成立。期望值出处 = 闭式：O_t = O_{t−1} = O 时
	//     平移项 = R·O − O = (R − I)·O ≠ 0
	// 若把 spin 的枢轴当成绕**渲染原点**（批 3 否掉的形态），平移项会退化成 ΔO = 0，
	// 于是「断言它非零且恰为 (R−I)·O」这条立刻红 —— 本条就是那个错法的阳性判据。
	Lcg rng(0x414E4348u);  // 写死种子
	int checked = 0;
	for (int i = 0; i < 200; ++i) {
		const glm::mat4 spinCur = randomSpin(rng);
		const glm::mat4 spinPrev = randomSpin(rng);
		const glm::dvec3 origin{rng.uniform(-800.0, 800.0), rng.uniform(-800.0, 800.0),
		    rng.uniform(-800.0, 800.0)};
		const glm::mat4 spinRatio = spinPrev * glm::inverse(spinCur);
		const glm::mat4 motion = motionWithOriginShift(spinRatio, origin, origin);
		// 闭式期望：motion 的线性部分 = R（逐位），平移列 = R[3] + (R−I)·O
		const glm::dmat4 r{spinRatio};
		const glm::dvec3 expectTranslation = glm::dvec3(r * glm::dvec4(origin, 0.0)) - origin;
		REQUIRE(glm::length(expectTranslation) > 1e-3);  // 前提守卫：本样本确实在自旋
		for (int col = 0; col < 3; ++col)
			for (int row = 0; row < 4; ++row)
				CHECK(motion[col][row] == spinRatio[col][row]);  // 基向量三列不动
		for (int row = 0; row < 3; ++row)
			expectNear(static_cast<double>(motion[3][row]),
			    static_cast<double>(spinRatio[3][row]) + expectTranslation[row], 1e-6, 1e-4);
		CHECK(motion[3][3] == spinRatio[3][3]);  // w 不动
		// 反向半边：同一样本若按「枢轴在渲染原点」算，平移项会是 0 —— 与上面的期望不同。
		CHECK(glm::length(expectTranslation) > 0.0);
		++checked;
	}
	CHECK(checked == 200);
}

TEST_CASE("motionWithOriginShift 恒等早退的第二档：门开但原点未动 ⟹ 逐位返回（不引入新舍入）") {
	// 早退的判据取「平移是否恰为零」而不是「两原点是否都为零」，这条钉它的另一半：
	// R = I 且 O_t = O_{t−1} ≠ 0 ⟹ 平移 = O − O = 0 ⟹ 逐位返回入参。
	// 意义：门开态里原点**没动**的那些帧（相机在原点的同一格内游走 = 绝大多数帧）
	// 与门关走的是同一条代码路径，motion 一个 bit 不变。
	Lcg rng(0x4F726967u);  // 写死种子
	for (int i = 0; i < 200; ++i) {
		const glm::dvec3 o{rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6)};
		REQUIRE(o != glm::dvec3{0.0});  // 前提：这条测的是「门开但没动」，不是门关
		checkMat4BitwiseEqual(motionWithOriginShift(glm::mat4{1.0f}, o, o), glm::mat4{1.0f});
	}
}

}  // TEST_SUITE coordinate_rebase
