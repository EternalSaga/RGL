module;
/**
 * test_root_transform_absolute —— 相对坐标批 3 **第二刀**的判据：绝对层是真 double 位姿
 *
 * ## 这一刀修掉的缺陷（一句话：`dmat4 absolute` 这个类型曾经骗人）
 * 批 3 第一刀把 `Transform::absolute` 换成了 `glm::dmat4`，但**产生**它的那条路是
 * `配置 dvec3 position ──glm::vec3(...) 收窄──▶ float mat4 ──glm::dmat4(...) 提升──▶ absolute`。
 * 收窄发生在进 double 层**之前** ⟹ float32 在 1e6 处的分辨率（0.0625 m）就是绝对层的分辨率，
 * 之后 `toRelative(absolute, origin)` 在 double 域减得再干净也救不回来（要减掉的精度已经没了）。
 * 本文件把这件事变成**闭式断言**：给定一个跨 float 分辨率台阶的配置位置，走生产路径
 * （`rootTransformOf` → 绝对层落库 → `toRelative`），读回来的相对值是配置值、还是 float
 * 圆过的值。
 * 🔴 **本文件写于第二刀落地之前，先跑红再跑绿**（改前实测读数 → docs/相对坐标地基_plan.md §3
 * 批 3 第二刀的落地记录）：同一组断言在改前给出 0.3125 / 0 / 0.125 / 0.6875，改后给出
 * 0.3 / 0.03125 / 0.09375 / 0.7。`floatShadowRoot`（被禁止形态的测试内转录）就是那条
 * 「改前」的活体，它永远给出旧数——主断言与它逐样本分家，才叫「拿得回」。
 *
 * ## 为什么判据能是「逐位」而不是「容差」
 * 全部样本的绝对位置取 **1e6 + offset**，而 1e6 = 2⁶·5⁶ 在 float32 里**精确可表示**
 * （14 bit 尾数），渲染原点也取精确的 1e6（真实原点 snap 到 2 的幂格点，同样精确）
 * ⟹ 「绝对 − 原点」这一步在 double 域是**精确减法**（Sterbenz：两操作数同量级 ⟹ 差精确），
 * 收窄只发生一次、对象是 offset 这个小量 ⟹ 判据不需要容差。
 * 期望值一律写成**二进制精确的十进制字面量**（0.3125 = 5·2⁻⁴、0.03125 = 2⁻⁵、0.125 = 2⁻³），
 * 不用 Approx。
 *
 * ## 样本怎么选（跨台阶，不是随便挑几个小数）
 * ULP(1e6) = 2¹⁹⁻²³ = 0.0625 m。offset 取 0.3 / 0.03125 / 0.09375 / 0.7 四枚：
 *  - 0.3 → float(1000000.3) = 1000000.3125 ⟹ 影子路径的相对值 0.3125（**圆过**，差 0.0125 m）
 *  - 0.03125 = 半个 ULP → 就近取偶落到 1000000.0 ⟹ 影子路径的相对值**恰为 0**（小数整个没了）
 *  - 0.09375 → 1000000.125 ⟹ 相对值 0.125（差 0.03125 m）
 *  - 0.7 → 1000000.6875 ⟹ 相对值 0.6875（差 0.0125 m）
 * 另配两枚**在网格上**的控制样本（0.0625 = 1 ULP、0.5 = 8 ULP，float 精确可表示）：
 * 这两枚两条路径必须给出同一个数。它们的作用是**否掉「断言恒真」**——如果主断言在控制样本上
 * 也「分家」，说明量具坏了（减法不精确 / 期望值算错），而不是精度有问题。
 *
 * ## 变异红线声明（改坏什么 → 哪条红；全部实测，读数 → 交付报告）
 *  M1 把 `rootTransformOf` 的 double 起步改回 float 起步（= 第二刀回退）
 *     → 「保真度」+「绝对层生产点落库」两条用例红；「顺序同构」**不红**（它钉的是顺序，不是精度）
 *  M2 换序（`RS·T` 而不是 `T·RS`）→ 「顺序同构」红（收窄逐位那条 + 与换序形态分家那条）
 *  M3 生产点把 absolute 再收窄回 float（`tf.absolute = glm::dmat4(glm::mat4(root))`）
 *     → 「绝对层生产点落库」红（world / Bounds 那两枚不红：它们本来就是 float，本批不动）
 *  M4 `toRelative` 改成先收窄后减 → 本文件**不红**（那是 coordinate_rebase 自己的判据，已在那儿
 *     钉死；本文件只喂它精确的原点，故意不重复钉——重复一遍 = 多一个会漂的副本）
 *
 * ## 射程（不覆盖什么，如实报）
 *  - `Transform::world` 仍是 float、`Bounds::center` 仍是 `glm::vec3`：本批不动它们，本文件只
 *    钉「world 是 absolute 的收窄派生」这条既有契约，并把它们**各自还吞掉的精度**读成断言
 *    （两枚 1000000.3125f，改前改后都绿）。
 *  - 逐图元复合（`rootTransform · local`）住在 `registerScene`，它要 `Resources&` 才能调用
 *    （设备级），不在 `just test` 的无设备子集里；那条链由截图锚 + imgqa 仪器守
 *    （批 3 已登记同一覆盖边界：「生产点的变异单测不红，只有出厂截图锚能抓」）。
 *    本文件只把那条链的**算术引理**（对角根矩阵下 float 复合与 double 复合的值逐分量相同）
 *    写成断言，用来解释「出厂截图锚为什么可以不改」——引理不守生产码，别把它读成守卫。
 */
#include <doctest/doctest.h>
#include <entt/entt.hpp>                  // 栈上 registry 驱动 registerBuiltinCube（先例 test_camera_absolute_domain）
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>   // 测试侧独立构造对账伙伴（translate/rotate/scale）
#include <cmath>                          // std::fabs / std::isfinite
#include <cstring>                        // std::memcmp（「逐位」按字面判）
#include <initializer_list>               // modules 坑：花括号列表不从 import 带进来

export module test_root_transform_absolute;

import rlstd;
import test_support;       // testing::Lcg（写死种子的属性扫描）
import scene_config;       // SceneEntityDesc / rootTransformOf（**被测的生产路径本体**）
import coordinate_rebase;  // toRelative（全仓唯一的「绝对 → 相对」减法点）
import scene_context;      // Transform / Bounds / NextObjectSlot
import builtin_geometry;   // registerBuiltinCube（绝对层三个生产点之一，设备无关可单测）

namespace RGL {
namespace {

/** 远工作点：|position| = 1e6（债-85 的「该开工」量级；float32 在此处 ULP = 0.0625 m）。 */
constexpr double kFar = 1e6;

/** ULP(1e6) = 2^(19−23) = 0.0625 m —— 本文件全部「台阶」论证的尺子。 */
constexpr double kUlpFar = 0.0625;

/** ULP(0.3) = 2^(−2−23) = 2⁻²⁵ ≈ 2.98e-8 m —— 「相对值是小量」那一侧的分辨率（本刀的界）。 */
constexpr double kUlpNear = 2.9802322387695312e-8;

/** 属性扫描种子（写死；换种子 = 换一组输入，不是「重试」）。 */
constexpr uint32_t kSeed = 20261026u;

/**
 * 保真度样本：配置位置 = kFar + offset，期望值按「float 圆过 / 没圆过」逐枚写死。
 *
 * 🔴 期望值出处 = IEEE 754 就近取整的定义（手算，见文件头「样本怎么选」），**不是**跑一遍
 * 被测码抄下来的数；`legacyRel` 与「配置小数」两列互为对照，相等的那些（onGrid）就是控制样本。
 */
struct FidelitySample {
	const char* name;
	double offset;     ///< 配置里的 position.x − 1e6（double 原样，不经 float）
	float legacyRel;   ///< float 影子路径的相对值 = float(1e6 + offset) − 1e6
	bool onGrid;       ///< true = offset 恰在 float 网格上 ⟹ 两条路径必须重合（控制样本）
};

constexpr FidelitySample kSamples[] = {
	{"0.3（跨台阶·圆上）", 0.3, 0.3125f, false},
	{"0.03125（半个 ULP·整个丢掉）", 0.03125, 0.0f, false},
	{"0.09375（1.5 个 ULP·圆上）", 0.09375, 0.125f, false},
	{"0.7（跨台阶·圆下）", 0.7, 0.6875f, false},
	{"0.0625（恰 1 ULP·在网格上）", 0.0625, 0.0625f, true},
	{"0.5（恰 8 ULP·在网格上）", 0.5, 0.5f, true},
};

/** 一条只有位姿差异的 entity 声明（rotation/scale 取恒等，隔离出平移这一维）。 */
SceneEntityDesc entityAt(double x, double y = 0.0, double z = 0.0) {
	SceneEntityDesc ent;
	ent.position = glm::dvec3{x, y, z};
	return ent;
}

/**
 * 「float 影子」形态的**独立转录** = 第二刀之前那条生产式，逐字照抄。
 *
 * 它在测试里的身份是**被禁止形态的对照**（阳性锚点，rgl-test-discipline：先问「代码从未
 * 生效时这条会绿吗」）：主断言「拿得回 0.3」只有在这条转录**拿不回**的前提下才算数，
 * 否则「拿得回」可能只是量具看不见误差。第二刀落地后生产式不再长成这样，本函数**不许删**。
 */
glm::mat4 floatShadowRoot(const SceneEntityDesc& ent) {
	glm::mat4 t = glm::translate(glm::mat4(1.0f), glm::vec3(ent.position));
	t = glm::rotate(t, glm::radians(ent.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
	t = glm::rotate(t, glm::radians(ent.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
	t = glm::rotate(t, glm::radians(ent.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
	return glm::scale(t, glm::vec3(ent.scale));
}

/**
 * 旋转/缩放的 float 链（`I·Rx·Ry·Rz·S`，与 SceneEntityDesc 头注的应用序契约同序）。
 * 顺序同构用例的对账伙伴：生产式应当 = `T(position) · 本函数`，本函数**不含平移**。
 */
glm::mat4 floatRotScale(const SceneEntityDesc& ent) {
	glm::mat4 r{1.0f};
	r = glm::rotate(r, glm::radians(ent.rotation.x), glm::vec3(1.0f, 0.0f, 0.0f));
	r = glm::rotate(r, glm::radians(ent.rotation.y), glm::vec3(0.0f, 1.0f, 0.0f));
	r = glm::rotate(r, glm::radians(ent.rotation.z), glm::vec3(0.0f, 0.0f, 1.0f));
	return glm::scale(r, glm::vec3(ent.scale));
}

/** 走**生产路径**取相对矩阵：rootTransformOf → 绝对层（`Transform::absolute` 的落库值）→ toRelative。
 *  写成 `glm::dmat4 abs{...}` 是有意的：direct-list-init 对「返回 float 矩阵」与「返回 double 矩阵」
 *  两种签名都成立 ⟹ 第二刀前后本函数逐字不改，改的只有期望值（读起来就是「同一条路，换了答案」）。 */
glm::mat4 productionRelative(const SceneEntityDesc& ent, const glm::dvec3& origin) {
	const glm::dmat4 abs{rootTransformOf(ent)};
	return toRelative(abs, origin);
}

/** 两个同形矩阵是否**逐位**相同（double 侧先收窄再比——直接 memcmp 一个 double 与一个 float
 *  比的是那个 double 的低位 32 位，实测会给出「值相同却全红」的假差异，别这么写）。 */
int bitwiseDiffCount(const glm::dmat4& a, const glm::mat4& b) {
	int diff = 0;
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			if (const float narrowed = static_cast<float>(a[col][row]);
			    std::memcmp(&narrowed, &b[col][row], sizeof(float)) != 0)
				++diff;
	return diff;
}

}  // namespace

TEST_CASE("绝对位姿保真度：1e6 工作点上生产路径**拿得回**亚 0.0625 m 的小数（第二刀已落地）") {
	const glm::dvec3 origin{kFar, 0.0, 0.0};  // 精确可表示（真实原点 snap 到 2 的幂格点，同样精确）

	for (const auto& s : kSamples) {
		const SceneEntityDesc ent = entityAt(kFar + s.offset);
		const glm::mat4 rel = productionRelative(ent, origin);
		const double got = rel[3].x;
		const double wantExact = static_cast<float>(s.offset);  // 「拿得回」= 只剩 float(0.3) 这种小量舍入

		// ① 主断言（**第二刀兑现**）：读回的是配置小数的 float 落库值，不是 float 圆过的值。
		//    逐位成立（无容差）：绝对层存的是 p 本身，减法在 double 域精确（Sterbenz：两操作数
		//    同量级），收窄只发生一次、对象是 offset 这个小量。
		CHECK(got == wantExact);
		// ② 反向半边（**这条才是「量具生效」的证明**）：同一条链换成 float 影子形态，必须
		//    给出**另一个**数——被圆过的那个。生产值与影子值分家 ⟹ ① 的「拿得回」不是量具瞎。
		const glm::mat4 legacyRel = toRelative(glm::dmat4(floatShadowRoot(ent)), origin);
		CHECK(static_cast<double>(legacyRel[3].x) == static_cast<double>(s.legacyRel));
		if (!s.onGrid)
			CHECK(got != static_cast<double>(legacyRel[3].x));  // 亚格小数：两条路径必须分家
		else
			CHECK(got == static_cast<double>(legacyRel[3].x));  // 控制样本：本来就没东西可丢，必须重合
		// ③ 误差量级：生产路径的误差 ≤ 小量自身的 float 分辨率（ULP(0.3) = 2⁻²⁵），
		//    影子路径的误差 ≤ 半个 ULP(1e6) = 0.03125 m。两个界差 6 个数量级 = 本刀的收益。
		const double err = std::fabs(got - wantExact);
		const double errLegacy = std::fabs(static_cast<double>(legacyRel[3].x) - wantExact);
		CHECK(err == 0.0);  // 逐位：收窄一次的正确结果就是 float(offset)
		if (s.onGrid)
			CHECK(errLegacy == 0.0);
		else
			CHECK(errLegacy <= 0.5 * kUlpFar);
		MESSAGE("【读数】position.x = 1e6 + " << s.offset << "（" << s.name << "）→ 生产相对值 " << got
		                          << "，float 影子形态 " << legacyRel[3].x << "，配置小数 " << wantExact);
	}

	// ④ 逐位对账（「配置值 vs 相对值 + 原点」的单元级对应）：相对值 + 原点 对上配置值了吗？
	//    改前实测差 0.0125 m（只能对上到 0.0625 m 那一档）；改后残余 = 小量自身的 float 分辨率。
	const SceneEntityDesc ent = entityAt(kFar + 0.3);
	const glm::mat4 rel = productionRelative(ent, origin);
	const double roundTrip = static_cast<double>(rel[3].x) + origin.x;
	const double err = std::fabs(roundTrip - (kFar + 0.3));
	CHECK(rel[3].x == 0.30000001192092896f);  // = float(0.3)，二进制精确的期望值
	CHECK(err <= kUlpNear);                   // 改后的界：小量的 float 分辨率（2⁻²⁵ ≈ 3e-8 m）
	// ⑤ 收益的数量级（本批的交付数字，不是「非零」这种弱断言）：改前的误差 0.0125 m 与本步
	//    的界之比 = 本刀买到的精度倍数。取 ≥ 1e5（闭式：ULP(1e6)/ULP(0.3) = 0.0625/2⁻²⁵ ≈ 2.1e6）。
	CHECK(0.0125 / err >= 1.0e5);
	MESSAGE("【读数】逐位对账：rel + origin = " << roundTrip << "，配置 = " << (kFar + 0.3)
	                                          << "，残余 " << err << " m（改前 0.0125 m，倍数 "
	                                          << 0.0125 / err << "）");
}

TEST_CASE("rootTransformOf 的乘法顺序 = T·Rx·Ry·Rz·S（第二刀前后都必须绿；换序当场红）") {
	SceneEntityDesc ent = entityAt(kFar + 0.3, -12.25, 7.5);
	ent.rotation = glm::vec3{17.0f, -29.0f, 43.0f};
	ent.scale = 0.32f;

	const glm::dmat4 root{rootTransformOf(ent)};

	// ① 顺序同构的**主判据**：生产式收窄回 float 后与「今天那条 float 影子链」**逐位相同**
	//    （全 16 个分量，含 ±0 的位）。成立的理由是结构性的：右乘一个不含平移的矩阵（旋转/缩放）
	//    不改变平移列，而平移不参与线性部分的合成 ⟹ 把平移挪进 double 只可能改「平移列在 double
	//    里存了多少位」，收窄之后一位都不差。
	//    ⚠ 改前这条是恒等式（同一个函数体），**改后它才是「没换序」的证明**——这正是它现在写在这里的理由。
	CHECK(bitwiseDiffCount(root, floatShadowRoot(ent)) == 0);

	// ② 属性扫描（写死种子，4000 组任意位姿）：上面那条不是只对本样本成立。
	//    位置刻意取**任意 double**（不是 float 可表示的数）：收窄后两侧都是 float(p)，
	//    与 p 能不能被 float 表示无关。
	{
		testing::Lcg rng(kSeed);
		int cases = 0;
		for (int i = 0; i < 4000; ++i) {
			SceneEntityDesc e = entityAt(rng.uniform(-1e6, 1e6), rng.uniform(-1e6, 1e6),
			                             rng.uniform(-1e6, 1e6));
			e.rotation = glm::vec3{static_cast<float>(rng.uniform(-360.0, 360.0)),
			                       static_cast<float>(rng.uniform(-360.0, 360.0)),
			                       static_cast<float>(rng.uniform(-360.0, 360.0))};
			e.scale = static_cast<float>(rng.uniform(0.01, 4.0));
			const glm::dmat4 r{rootTransformOf(e)};
			CHECK(bitwiseDiffCount(r, floatShadowRoot(e)) == 0);
			++cases;
		}
		MESSAGE("【读数】顺序同构属性扫描：样本 " << cases << " 组 × 16 分量，逐位差异 = 0");
	}

	// ③ 线性部分 = 纯 float 的 Rx·Ry·Rz·S（平移不参与线性部分）；w 行必须是零值。
	//    用 `== 0.0f` 而不是 memcmp：±0 在两条链里可以不同符号，而它们在任何乘法里都给出同一个数。
	const glm::mat4 rs = floatRotScale(ent);
	for (int col = 0; col < 3; ++col) {
		for (int row = 0; row < 3; ++row)
			CHECK(static_cast<float>(root[col][row]) == rs[col][row]);
		CHECK(root[col][3] == 0.0f);
	}
	// ④ 区分力守卫（阳性锚点）：**换序**形态 `RS·T` 的平移列必须与生产值分家。
	//    没有这一条，①②③ 里「平移列 = p」这一维就是没被测过的——T 在左时它是 p，
	//    T 在右时它变成 RS 的线性部分乘 p（量级差 5 倍以上，本样本实测 2.0e5 vs 1.0e6）。
	const glm::dmat4 swapped = glm::dmat4(rs) * glm::translate(glm::dmat4(1.0), ent.position);
	CHECK(root[3].x != swapped[3].x);
	CHECK(root[3].y != swapped[3].y);
	CHECK(root[3].z != swapped[3].z);
	MESSAGE("【读数】平移列 = (" << root[3].x << ", " << root[3].y << ", " << root[3].z
	                           << ")，换序形态 = (" << swapped[3].x << ", " << swapped[3].y
	                           << ", " << swapped[3].z << ")");

	// ⑤ 出厂形态（position 恒等、scale 对角）：float 影子链与 double 链逐位相同。
	//    🔴 这条 + 下面那条算术引理，合起来是「出厂截图锚可以不改」的机器理由——出厂
	//    configs/scene.lua 唯一的 entity 不写 position（= 恰 (0,0,0)）、scale = 0.01（对角根矩阵），
	//    两者都不给精度路径任何可丢的东西。**它不是「本刀无影响」的证明**：远工作点由
	//    「保真度」那条用例与 imgqa 仪器负责，那里有东西可丢。
	const SceneEntityDesc factory = [] {
		SceneEntityDesc e;
		e.scale = 0.01f;
		return e;
	}();
	CHECK(bitwiseDiffCount(glm::dmat4{rootTransformOf(factory)}, floatShadowRoot(factory)) == 0);
	CHECK(rootTransformOf(factory)[3].x == 0.0);  // 出厂根平移恰零 ⟹ float 与 double 是同一个数

	// ⑥ 算术引理（**不守生产码**，只解释出厂图为什么不动）：逐图元复合 `root · local` 在
	//    **对角根矩阵**（出厂 scale = 0.01、无旋转、零平移）下，float 域乘与 double 域乘的
	//    **值**逐分量相同（只有 ±0 的位差）。理由：对角矩阵乘一列 = 单个乘积加三个 ±0 项，
	//    两种精度都只发生一次有效舍入，结果都是同一个正确舍入值。
	//    🔴 double 根必须写成 float 根的**无损提升**（`glm::dmat4(rootF)`），与生产式同形。
	//    实测踩过的坑：若改从 double 字面量 `dvec3(0.01)` 起步，两侧乘的就不是同一个数
	//    （0.01_double ≠ float(0.01)），500 组里会读出 1612 处**假**值差——那是引理写错了，
	//    不是精度路径有问题。
	//    ⚠ 生产式在 registerScene 里（要 Resources&，设备级），本条钉不到它——覆盖边界与
	//    批 3 登记的同一条一致：「生产点的变异单测不红，只有出厂截图锚能抓」。
	{
		const glm::mat4 diagRootF = glm::scale(glm::mat4(1.0f), glm::vec3(0.01f));
		const glm::dmat4 diagRoot{diagRootF};
		testing::Lcg rng(kSeed + 1u);
		int valueDiff = 0, bitDiff = 0;
		for (int i = 0; i < 500; ++i) {
			glm::mat4 local = glm::translate(
				glm::rotate(glm::scale(glm::mat4(1.0f),
					glm::vec3{static_cast<float>(rng.uniform(0.1, 5.0)),
							  static_cast<float>(rng.uniform(0.1, 5.0)),
							  static_cast<float>(rng.uniform(0.1, 5.0))}),
					static_cast<float>(rng.uniform(-3.14, 3.14)), glm::vec3{0.3f, 0.6f, -0.7f}),
				glm::vec3{static_cast<float>(rng.uniform(-100.0, 100.0)),
						  static_cast<float>(rng.uniform(-100.0, 100.0)),
						  static_cast<float>(rng.uniform(-100.0, 100.0))});
			const glm::mat4 inFloat = diagRootF * local;
			const glm::dmat4 inDouble = diagRoot * glm::dmat4(local);
			for (int col = 0; col < 4; ++col)
				for (int row = 0; row < 4; ++row) {
					const float d = static_cast<float>(inDouble[col][row]);
					if (d != inFloat[col][row])
						++valueDiff;
					if (std::memcmp(&d, &inFloat[col][row], sizeof(float)) != 0)
						++bitDiff;
				}
		}
		CHECK(valueDiff == 0);  // 值必须全同；位差只允许是 ±0（下面单独读出来）
		MESSAGE("【读数】对角根复合引理：500 组 × 16 分量，值差 = " << valueDiff
		                          << "，位差（全是 ±0）= " << bitDiff);
	}
}

TEST_CASE("绝对层生产点 registerBuiltinCube：absolute 落库逐位 = 配置的 double 位姿") {
	entt::registry rg;
	rg.ctx().emplace<NextObjectSlot>();  // registerBuiltinCube 要领取对象矩阵槽

	const SceneEntityDesc ent = entityAt(kFar + 0.3, 0.0, 0.0);
	const auto e = registerBuiltinCube(rg, 7u, 3u, rootTransformOf(ent));
	const auto& tf = rg.get<Transform>(e);

	// ① 绝对层落库 = **配置值本身**（逐位，不是「配置值的 float 影子」）。
	//    改前实测这一位是 1000000.3125（float 圆过），改后是 1000000.29999999995343…
	CHECK(tf.absolute[3].x == kFar + 0.3);
	CHECK(tf.absolute[3].x != 1000000.3125);  // 圆过的痕迹必须消失（反向半边）
	// ② world 是 absolute 的收窄派生（本批不动 world 的类型）。逐分量按字面比，不用 glm::equal：
	//    那要 gtc/epsilon.hpp，而「派生」这件事本来就是逐位事实，容差反而是松。
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			CHECK(tf.world[col][row] == static_cast<float>(tf.absolute[col][row]));
	// ③ Bounds::center 仍是 `glm::vec3`（本批不动它）：球心 = 根平移的 float 落库值。
	//    🔴 ②③ 两枚 1000000.3125f 就是「第二刀之后**仍然没买到**的那部分精度」的读数，
	//    它们改前改后都必须绿（本刀改的是 absolute 这一维，不是这两处）——下一刀的靶子。
	CHECK(tf.world[3].x == 1000000.3125f);
	CHECK(rg.get<Bounds>(e).center.x == 1000000.3125f);
	// ④ 绝对层的 w 行与线性部分逐位 = float 影子链（第二刀只动平移列在 double 里存多少位）。
	CHECK(bitwiseDiffCount(tf.absolute, floatShadowRoot(ent)) == 0);
	MESSAGE("【读数】absolute[3].x = " << tf.absolute[3].x << "，world[3].x = " << tf.world[3].x
	                                  << "，配置 = " << (kFar + 0.3));
}

}  // namespace RGL
