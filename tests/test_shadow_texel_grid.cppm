module;
/**
 * test_shadow_texel_grid —— 相对坐标批 5 第 0 刀：**只测量，不施工**
 *
 * ## 本文件要回答的唯一问题
 * 「渲染原点吸附格」与「阴影 texel 格」是否相容？具体判据 = 原点跨一格之后，同一块世界表面
 * 在阴影图集里落到的**纹素坐标**是否逐位不变（图集级判据的 CPU 等价形式，不需要 GPU）。
 *
 * ## 为什么这件事有判据、而它此前只是推理
 * 批 4 把渲染原点接进实例矩阵 / view / 灯 / TAA 之后，门开态原点会随相机移动；每移动一次，
 * 所有相对世界坐标整体平移 −ΔO。阴影图集的稳定性靠 `buildShadowVolume` 的 texel 吸附
 * （切片球心在固定光基下三坐标各 round 到 `2r/mapSize` 格点）。把「图集内容不变」的条件写开
 * （记 `C = dot(切片球心绝对坐标, a)`、`D = dot(ΔO, a)`、`g` = 该级该轴用的 texel 格距）：
 * ```
 * 图集坐标 ∝ dot(p_abs, a) − D − round((C − D)/g)·g
 * 不变 ⟺ round((C − D)/g)·g = round(C/g)·g − D ⟺ D 是 g 的整数倍（每一级、每一根轴都要成立）
 * ```
 * ⟹ 需要「原点吸附格」与「每一级 texel 格」互相整除。本文件把这句话变成读数。
 *
 * ## 读数口径（🔴 三条，别混）
 *  ① **原始错位 δ**（单位 = 纹素）= `[q_a(0) − q_a(O₁) − D_a] / g`。它是「盒相对世界挪了多少」。
 *  ② **有效错位** = δ 到**最近整数**的距离。整张图集挪动恰好 1 个纹素是**无害**的（采样格没变），
 *     所以真正决定画质的是 ②，不是 ①。由 `round` 的定义 δ ∈ [−1, 1] ⟹ 有效错位 ≤ 0.5 纹素。
 *  ③ **逐位相同**（memcmp）比 ② 更严：它连「浮点路径重舍入」也算分家。本文件把它单独钉一条
 *     （「测 4 比较算子的区分力」的半边③），因为它决定批 5 的「图集级逐位不变」判据到底做得
 *     成做不成 —— 实测：**即使格条件完全满足，256/256 个样本仍逐位分家**（残余 2.7e-4 纹素）。
 *
 * ## 期望值出处（不抄被测码的算式）
 *  - 吸附后盒心的光基坐标 `q_a`：**不重算吸附**，而是从真实 `buildShadowVolume` 返回的 `view`
 *    矩阵里**读回来**（glm::lookAt 的平移列 = −dot(轴, 眼点)，眼点 = 盒心 − 光向·eyeOffset，
 *    三行推导写在 `readbackSnappedCenter` 注释里）。被测者吐什么，判据就读什么。
 *  - 有效错位的**闭式模型** = 上面那条 `round` 恒等式（纯算术，出处 = IEEE 取整定义）。
 *    测 2 断言「按模型算出的 δ」与「按真实两帧产物逐点量出的位移」对上（容差 1e-3 纹素 =
 *    浮点重舍入噪声量级，而跳变本身 0.1–0.5 纹素，两边相差两个数量级以上）——
 *    模型先算，实机后读。
 *  - 拟合球半径的独立第二实现 = 「视锥 8 角共球」这个几何定义本身（在 double 域解等距方程，
 *    出处 = 球心到近角与远角等距），**不是** `frustumBoundingSphere` 的那两行代数。
 *  - 出厂配置读数：fov / 机位 / 太阳方向 = `configs/camera.lua`、`configs/scene.lua`；
 *    宽高比 = `configs/runtime.lua` 的 `headless_size = {1280, 720}`；
 *    级联深度界 = 债-139 实机读数（`docs/还债总账.md` 债-139 行，自适应 near 落在 0.05 下限、
 *    远界 1.713496）。运行期的 near/far 要 SceneBounds（要装模型），单测拿不到 ⟹ 取台账里
 *    **唯一一组实机读数**，取值依据写进下面的常量注释。
 *
 * ## 变异红线声明（「改坏什么 → 哪条用例红」，全部**实测**，括号里是实测的红断言数）
 *  - **M1** 测 2 的比较算子 `bitwiseEqual` 从逐位（memcmp）换成容差 1e-3 纹素
 *      → 「测 4 比较算子的区分力」红 1 条（**只有**亚容差那半边③抓到它）。
 *      🔴 任务书原本只要求「脱格原点偏差 ≠ 0」那半边（= 本条的半边②）能抓这条变异，
 *      实测**抓不到**：脱格原点的偏差 ≈ 0.42 纹素，远大于 1e-3，容差化之后照样判「分家」。
 *      ⟹ 补了半边③（格条件满足、偏差 2.7e-4 纹素落在「逐位看得见 / 容差看不见」的窗口里），
 *      M1 才有抓手。这条缺口是跑出来的，不是想出来的。
 *  - **M2** 生产 `rebaseSnapGranularity` 改成取**最细**一级（`step > coarsest` → 取小）
 *      → 本文件红 2 条用例 / 13 条断言（「测 1 出厂态」4 条：G 的区间断言 + 三条 G/texel > 1；
 *        「测 1 的稳健性」9 条：九组深度界各自的「G ≥ 末级纹素」）；
 *        全库红 7 条用例 / 210 条断言（另 5 条在 test_coordinate_rebase / test_rebase_consumers）。
 *      ⚠ 首轮这条被读成「全绿」= **harness 自己的假绿**：构建命令写成 `cmake ... | tail -25`
 *        ⟹ 拿到的是 tail 的退出码，而那次构建其实 RC=1（clang 22 SIGSEGV 抖动）。
 *        去掉管道 + 加「同 TU 反复崩就先 `just build` 重建引擎库」的重试后重跑，才拿到上面的红。
 *        skill `rgl-test-discipline` 第 ③ 条写的就是这个坑，本轮又踩一次，可见它多容易复发。
 *  - **M3** 测 3 的 H1 臂偷偷退回世界轴吸附（`snapInLightBasis` → `snapOriginToGranularity`）
 *      → 「测 3 三假设对照」红 3 条（H1 吸附自检：dot(结果, 每根光基轴) 必须是 G 的整数倍）
 *  - **M4** 量具的 readback 把 d 轴符号写反（`-row(2)` → `row(2)`，行 2 存的是 −f）
 *      → 「量具自检」红 8 条（手性 + 三条「恰在格点上」+ 三条「≤ 半纹素」+ 深度那条）
 *  - **M5** 测 3 的 H2 臂忘了放大盒（喂 `nullptr` 而不是 `radii.data()`）
 *      → 「测 3 三假设对照」红 5 条（四条「放大后的盒吐出目标纹素」+ 那条归零判决）
 *  - **M6** 有效错位算子弱化（`fracDistance` 忘掉 `round`，退化成 |δ|）
 *      → 「测 2 决定性测量」红 1 条（0.5 纹素的闭式上界：原始 δ 最大 0.87 纹素，越界）；
 *        全库红 3 条用例 / 13 条断言。这条是「整纹素平移无害」这句话的承重证明。
 *  ⚠ **M4 那个符号/取法问题首轮就红过一次（真 bug，不是变异），两条都在此记账**：
 *     ① readback 第一版取的是矩阵的**列**而不是行 —— 列三件套同样单位正交、det 也同样是
 *        +1（正交矩阵的列组与行组都正交），**手性与正交性两条检查都拦不住它**；真正抓到它
 *        的是「|q_a − dot(球心, 轴 a)| ≤ 半纹素」那条硬界（q 按行读、轴按列读 ⟹ 两者配不上对）。
 *     ② 手性检查自己一开始也写错了：期望 +1，而生产的光基是 u = s×d ⟹ s×u = **−d**。
 *     教训：量具的自检必须包含「把两个独立读出的量对上一条硬界」，只查读回对象自身的性质
 *     （正交 / 单位长）拦不住「读对了矩阵、配错了轴」这种错。
 *
 * ## 覆盖边界（如实报）
 *  - 本文件**零生产改动**，全部判据长在「真实函数的产物」上；生产点（`projectionSystem` 的
 *    原点生产分支、
 *    `buildShadowVolumes` 的调用点）没有用例，门开态仍只有出厂截图锚 + 批 5 那台仪器能验
 *    （与批 3/4 登记的「生产点变异单测不红」同一形态）。
 *  - 测 3 的 H1/H2 是**测试侧的假想形态**（生产没有光基吸附、也没有放大盒），
 *    它们的读数只能回答「这样改行不行」，不保证改出来的生产码与测试侧那份逐式相同。
 */
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt / perspective / radians
#include <cmath>                         // std::round / std::frexp / std::isfinite / std::abs
#include <cstring>                       // memcmp（「逐位」按字面判）
#include <initializer_list>              // modules 坑：花括号列表不从 import 带进来

export module test_shadow_texel_grid;

import rlstd;
import test_support;       // testing::Lcg / expectNear
import scene_context;      // buildShadowVolume / quantizeShadowDistance / ShadowVolume / Light / Bounds
import scene_data;         // kActiveShadowCascades
import render_graph;       // shadowAtlasColumns / shadowTileSize / kShadowMapSize
import render_list;        // computeCascadeSpheres / shadowTexelSize
import coordinate_rebase;  // rebaseSnapGranularity / snapOriginToGranularity / toRelative

namespace RGL {
namespace {

using testing::Lcg;

/** 门关原点 = 逐位 (0,0,0)（相对坐标批 5 第 1 刀给 `buildShadowVolume` 加的 origin 入参）。
 *  本文件的用例全部测**门关态**的几何规则，显式传它而不是留默认值：漏传不会让任何判据红，
 *  而「门关逐位不变」这条判据的语义就是「原点这一维真的没参与」。 */
const glm::dvec3 kNoOrigin{0.0, 0.0, 0.0};

/** 属性测试种子（写死；换种子 = 换一组输入，不是「重试」）。 */
constexpr uint32_t kSeed = 20261103u;

// ─── 出厂配置读数（每条带出处；改配置 = 本文件的读数跟着重立，这是有意的）───────

/** `configs/camera.lua` 的 `fovDegrees = 45.0`（⚠ 不是 60 —— 见交付报告「与任务书不符」条）。 */
constexpr float kFovYDeg = 45.0f;
/** `configs/runtime.lua` 的 `headless_size = { 1280, 720 }` ⟹ 渲染网格宽高比 16:9。 */
constexpr float kAspect = 16.0f / 9.0f;
/** `configs/camera.lua` 的交付机位（绝对层，double）。 */
const glm::dvec3 kEyeAbs{1.1, 0.75, 1.05};
const glm::dvec3 kTargetAbs{0.14, 0.12, 0.02};
/** `configs/scene.lua` 的 `light.direction`（消费时归一化）。 */
const glm::vec3 kSunDir{0.5f, -0.75f, -0.45f};

/**
 * 级联深度界的出厂读数（出处 = `docs/还债总账.md` 债-139 行的实机记录）。
 *
 * 生产链路：`projectionSystem` 按 SceneBounds 现算 `near = max(0.05, dist − r)` /
 * `far = dist + r`，`renderListSystem` 再 `quantizeShadowDistance` 成 shadowNear/shadowFar。
 * SceneBounds 要装完模型才有 ⟹ 无设备单测拿不到「出厂那一帧」的真值，取台账里唯一一组
 * 实机读数：near 落在 0.05 下限、far = 1.713496（`tests/test_shadow_geometry.cppm` 的
 * 「同档两机位」那条用的是同一组数，本文件不另立一套口径）。
 * ⚠ 本文件的**结论**不依赖这组数的具体值：测 1 的整除性另有一条用例扫 6 组深度界。
 */
constexpr float kNearRaw = 0.05f;
constexpr float kFarRaw = 1.713496f;

/** 合成投影体 AABB（与出厂场景同量级：bistro `scale = 0.01`，世界包围半径 ≈ 1.17）。
 *  它只进 `buildShadowVolume` 的 Z 界（tMin/tMax），**不进** XY 吸附的判据。 */
constexpr glm::dvec3 kCasterMinAbs{-1.5, -0.5, -1.5};
constexpr glm::dvec3 kCasterMaxAbs{1.5, 2.0, 1.5};

/** 图集单块边长 = 出厂态 snap 格（`buildShadowVolumes` 里那个 `snapGrid`）。 */
const uint32_t kTile = shadowTileSize(shadowAtlasColumns(kActiveShadowCascades));

/** 出厂主光（只 direction 参与光空间基）。 */
const Light kSun{.index = 0u, .direction = kSunDir, .color = glm::vec3{1.0f, 0.98f, 0.95f},
                 .intensity = 12.0f, .ambient = glm::vec3{0.25f}, .position = glm::dvec3{0.0},
                 .range = 0.0f, .type = 0u};

// ─── 真实链路的帧产物 ────────────────────────────────────────────────────────

/**
 * 「渲染原点 = origin」这一帧的级联产物，全部来自**生产函数**：
 * `computeCascadeSpheres`（拟合球）→ `buildShadowVolume`（texel 吸附 + 建盒）→
 * `shadowTexelSize`（写进 scene data 的那枚纹素）。
 *
 * 🔴 相对坐标的减法一律走 `toRelative`（全仓唯一换算入口），本文件不自己减原点。
 * @param radiusOverride 非空 = 用给定半径替换拟合球半径（测 3 的 H2 假设要「texel 取 2 的幂」
 *        的盒，那是**假想的生产改法**，只能改入参不能改生产码）；空 = 真实几何。
 */
struct CascadeFrame {
	std::array<Bounds, kActiveShadowCascades> slices;
	std::array<ShadowVolume, kActiveShadowCascades> volumes;
	std::array<float, kActiveShadowCascades> texels;
};

CascadeFrame buildFrame(const glm::dvec3& origin, const float* radiusOverride = nullptr) {
	const glm::mat4 view = glm::lookAt(toRelative(kEyeAbs, origin), toRelative(kTargetAbs, origin),
	                                   glm::vec3{0.0f, 1.0f, 0.0f});
	ViewProjection vp;
	vp.view = view;
	// proj 用**未量化**的 near/far（与 projectionSystem 同形）；级联切分才用量化值。
	// 拟合球半径只吃 proj 的 fov/aspect 两行 + 显式 nearC/farC，所以这里给未量化值不影响半径。
	vp.proj = glm::perspective(glm::radians(kFovYDeg), kAspect, kNearRaw, kFarRaw);
	vp.nearPlane = kNearRaw;
	vp.farPlane = kFarRaw;

	CascadeFrame frame;
	frame.slices = computeCascadeSpheres(vp, quantizeShadowDistance(kNearRaw),
	                                     quantizeShadowDistance(kFarRaw));
	if (radiusOverride != nullptr)
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
			frame.slices[c] = Bounds{frame.slices[c].center, radiusOverride[c]};

	const glm::vec3 casterMin = toRelative(kCasterMinAbs, origin);
	const glm::vec3 casterMax = toRelative(kCasterMaxAbs, origin);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		frame.volumes[c] = buildShadowVolume(kSun, frame.slices[c], casterMin, casterMax, kTile, kNoOrigin);
		frame.texels[c] = shadowTexelSize(frame.volumes[c].xyRadius);
	}
	return frame;
}

/** 从光基三轴算「原点位移 ΔO 在该轴上的投影」（double；D 是判据里的 ΔO 分量）。 */
double axisShift(const glm::dvec3& origin, const glm::dvec3& axis) {
	return glm::dot(origin, axis);
}

/**
 * 从真实 `ShadowVolume::view` 里读回**光基**与**吸附后的盒心坐标**（不重算吸附）。
 *
 * 推导（glm::lookAtRH 的平移列 = `−dot(s, eye)` / `−dot(u, eye)` / `+dot(f, eye)`，
 * 行 0/1/2 = `s` / `u` / `−f`）：眼点 = 盒心 − 光向·eyeOffset，且 s、u 与光向正交 ⟹
 * ```
 * dot(s, 盒心) = dot(s, 眼点) = −view[3][0]
 * dot(u, 盒心) = −view[3][1]
 * dot(d, 盒心) = dot(d, 眼点) + eyeOffset = +view[3][2] + eyeOffset
 * ```
 * 🔴 d 轴那个 `+` 与 `+eyeOffset` 是这条读回里唯一会写错的地方（行 2 存的是 `−f`），
 * 写反了不报错、只会让「量具自己错」—— 所以「量具自检」那条用例用 `|q_d − dot(球心,d)|
 * ≤ texel/2` 这条硬界拦它（符号写反会差出 2·|dot(盒心,d)|，量级 1e0 = 上千纹素）。
 */
struct LightFrameReadback {
	glm::dvec3 s, u, d;  ///< 光基三轴（单位长、右手系）
	glm::dvec3 q;        ///< 吸附后盒心在 (s,u,d) 下的坐标
};

LightFrameReadback readbackSnappedCenter(const ShadowVolume& v) {
	// 🔴 取的是**行**（glm 是列主序存储 ⟹ 行 r = view[0][r], view[1][r], view[2][r]）。
	// 取成列不会编译错、列也照样正交，只有手性会反（列三件套是 (s, u, −d)，det = −1）——
	// 所以「量具自检」里那条 det = +1 的检查是这条笔误的唯一抓手，实测它真的抓到了。
	const auto row = [&v](uint32_t r) {
		return glm::dvec3{static_cast<double>(v.view[0][r]), static_cast<double>(v.view[1][r]),
		                  static_cast<double>(v.view[2][r])};
	};
	LightFrameReadback out;
	out.s = row(0);
	out.u = row(1);
	out.d = -row(2);  // 行 2 存的是 −f
	out.q = glm::dvec3{-static_cast<double>(v.view[3][0]), -static_cast<double>(v.view[3][1]),
	                   static_cast<double>(v.view[3][2]) + static_cast<double>(v.eyeOffset)};
	return out;
}

/** 光基三轴的数组形态（逐轴循环用，顺序 = (s, u, d)）。 */
std::array<glm::dvec3, 3> axesOf(const LightFrameReadback& r) {
	return {r.s, r.u, r.d};
}

/** 轴名（失败信息里点名是哪根轴）。⚠ doctest 2.5.3 把 `const char*` 打成指针、把 `char`
 *  打成整数，所以读数里写的是**轴序号**：0 = s、1 = u、2 = d。 */
constexpr std::array<char, 3> kAxisNames{'s', 'u', 'd'};

/**
 * 世界点 `pAbs` 落在该级图集的哪个纹素（x, y, 深度三个分量，单位 = 纹素）。
 *
 * 这就是「图集内容」的 CPU 等价量：两张图集内容相同 ⟺ 每个世界点落在同一个纹素上。
 * 深度分量按 `[tMin, tMax] → [0, kTile]` 线性映射（深度图的纹素网格）。
 * 矩阵乘在 **double** 域做（入参是 float 矩阵与 float 相对坐标，都是被测者的真实产物），
 * 这样「量具自己的舍入」不会混进读数 —— 读数是「盒相对世界挪了多少」，不是「float 加噪」。
 */
glm::dvec3 atlasTexelOf(const ShadowVolume& v, float texel, const glm::dvec3& pAbs,
                        const glm::dvec3& origin) {
	const glm::vec3 pRel = toRelative(pAbs, origin);
	const glm::dvec4 ls = glm::dmat4(v.view) * glm::dvec4(glm::dvec3{pRel}, 1.0);
	const double u = ls.x, w = ls.y;
	const double t = -ls.z - static_cast<double>(v.eyeOffset);
	const double half = static_cast<double>(kTile) * 0.5;
	const double depth = (t - static_cast<double>(v.tMin)) /
	                     (static_cast<double>(v.tMax) - static_cast<double>(v.tMin)) *
	                     static_cast<double>(kTile);
	return glm::dvec3{u / static_cast<double>(texel) + half, w / static_cast<double>(texel) + half,
	                  depth};
}

/** 逐位相同（memcmp：+0.0 与 −0.0 判不同 —— 「逐位」两字不许读成「值相等」）。 */
bool bitwiseEqual(const glm::dvec3& a, const glm::dvec3& b) {
	return std::memcmp(&a, &b, sizeof(glm::dvec3)) == 0;
}

/** 分量取用（k = 0/1/2 → x/y/z）。不用 `&v.x + k`：那是 glm 内部布局的未定义用法。 */
double comp(const glm::dvec3& v, int k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }

/** 到最近整数的距离（= 「有效错位」：整纹素平移无害，只有小数部分会改采样格）。 */
double fracDistance(double v) { return std::abs(v - std::round(v)); }

/** 一批测试点：铺在出厂场景的受光面附近（写死种子），覆盖四级级联的盒。 */
std::vector<glm::dvec3> makeTestPoints() {
	std::vector<glm::dvec3> out;
	Lcg rng{kSeed};
	for (int i = 0; i < 64; ++i)
		out.push_back(glm::dvec3{rng.uniform(-1.2, 1.2), rng.uniform(-0.4, 0.4),
		                         rng.uniform(-1.2, 1.2)});
	return out;
}

/** 测 2/测 3 的读数本体。 */
struct JumpReading {
	double maxRaw = 0.0;         ///< 最大原始错位 |δ|（纹素）
	double maxEffective = 0.0;   ///< 最大有效错位（纹素，= δ 到最近整数的距离）
	int worstCascade = -1;       ///< 有效错位最大者出现在哪一级
	int worstAxis = -1;          ///< 哪根轴
	double perCascadeAxis[4][3]{};
	double perCascadeAxisEffective[4][3]{};
	double pointSpread = 0.0;    ///< 逐点测量的「同一 (级,轴) 内位移的最大−最小」= 刚性平移检验
	double maxPointDelta = 0.0;  ///< 逐点测量的最大位移（纹素，x/y 两分量）
};

/**
 * 量「原点从 0 变成 `origin` 后，图集相对世界挪了多少」。
 *
 * 两条独立路径给同一个数，本函数把两条都算出来（对不上 = 量具坏了，不是代码坏了）：
 *  ① **闭式**：`δ_a = q_a(0) − q_a(O₁) − dot(O₁, a)`，q 从两帧真实产物读回；
 *  ② **逐点**：对一批世界点分别算两帧的图集纹素坐标，取差。
 * ② 与点无关（差是一个常数平移）本身就是判据：它说明门开后的后果是**一次跳变**，
 * 不是「影子随视角连续游动」—— 后者才是 5a-8b 花一整批消掉的东西。
 */
JumpReading measureJump(const CascadeFrame& base, const CascadeFrame& moved,
                        const glm::dvec3& origin) {
	JumpReading out;
	const auto points = makeTestPoints();
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const auto frame = readbackSnappedCenter(moved.volumes[c]);
		const auto baseFrame = readbackSnappedCenter(base.volumes[c]);
		const auto ax = axesOf(frame);
		const double g = static_cast<double>(base.texels[c]);
		REQUIRE(base.texels[c] == moved.texels[c]);  // 前提：两帧同一套 texel（半径与原点无关）
		for (int a = 0; a < 3; ++a) {
			const double delta = baseFrame.q[a] - frame.q[a] - axisShift(origin, ax[a]);
			out.perCascadeAxis[c][a] = delta / g;
			out.perCascadeAxisEffective[c][a] = fracDistance(delta / g);
			if (out.perCascadeAxisEffective[c][a] > out.maxEffective) {
				out.maxEffective = out.perCascadeAxisEffective[c][a];
				out.worstCascade = static_cast<int>(c);
				out.worstAxis = a;
			}
			out.maxRaw = std::max(out.maxRaw, std::abs(delta / g));
		}
		// ② 逐点。散布**按分量各自算**再取最大：x 与 y 的常数位移本来就不相等（δ_s ≠ δ_u），
		// 混在一起量到的是「两根轴的差」而不是「同一根轴随点的散布」。
		for (int k = 0; k < 2; ++k) {
			double lo = 1e300, hi = -1e300;
			for (const glm::dvec3& p : points) {
				const glm::dvec3 a0 = atlasTexelOf(base.volumes[c], base.texels[c], p, glm::dvec3{0.0});
				const glm::dvec3 a1 = atlasTexelOf(moved.volumes[c], moved.texels[c], p, origin);
				const double d = comp(a1, k) - comp(a0, k);
				lo = std::min(lo, d);
				hi = std::max(hi, d);
				out.maxPointDelta = std::max(out.maxPointDelta, std::abs(d));
			}
			out.pointSpread = std::max(out.pointSpread, hi - lo);
		}
	}
	return out;
}

/**
 * 把一份 JumpReading 打到 doctest 输出（读数就是本批的交付物，必须打得出来）。
 *
 * ⚠ 臂名走 `switch` + **字符串字面量**，不走 `const char*` 形参：实测 doctest 2.5.3 在本
 * 构建（STL 走 modules）下把 `const char*` 变量打成指针、把 `std::string` 打成字节数组，
 * 只有字面量正常 —— 读数打不出臂名等于没打。
 * @param arm 0 = H0 现状、1 = H1 光基、2 = H2 光基+纹素抬 2 的幂、3 = 测 4 脱格原点
 */
void reportJump(int arm, const JumpReading& r) {
	switch (arm) {
	case 0: MESSAGE("【读数】H0 现状（世界轴吸附 + G）"); break;
	case 1: MESSAGE("【读数】H1 只改吸附基（光基）+ G"); break;
	case 2: MESSAGE("【读数】H2 光基 + 每级纹素抬 2 的幂"); break;
	default: MESSAGE("【读数】测 4 脱格原点（onGrid + (0.317,0,0)）"); break;
	}
	MESSAGE("  最大原始错位 = " << r.maxRaw << " 纹素，最大有效错位 = " << r.maxEffective
	          << " 纹素（出现在级 " << r.worstCascade << " 轴#" << r.worstAxis
	          << "），逐点位移的级内散布 = " << r.pointSpread);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		MESSAGE("  c" << c << " 原始 δ/texel (s,u,d) = " << r.perCascadeAxis[c][0] << ", "
		          << r.perCascadeAxis[c][1] << ", " << r.perCascadeAxis[c][2]
		          << " ⟹ 有效 " << r.perCascadeAxisEffective[c][0] << ", "
		          << r.perCascadeAxisEffective[c][1] << ", " << r.perCascadeAxisEffective[c][2]);
}

/**
 * 假想改法用的「光基吸附」：把期望原点沿光基三轴各 snap 到 granularity 的整数倍。
 *
 * 生产没有这个函数（现状 `snapOriginToGranularity` 按**世界 xyz 分量**吸），本文件是量具批、
 * 不许改生产码 ⟹ 假想形态在测试侧写。它的契约只有一条，且单独有用例钉：
 * `dot(结果, 每根轴)` 都是 granularity 的整数倍。
 */
glm::dvec3 snapInLightBasis(const glm::dvec3& desired, double granularity,
                            const std::array<glm::dvec3, 3>& axes) {
	glm::dvec3 out{0.0, 0.0, 0.0};
	for (const glm::dvec3& a : axes) {
		const double k = std::round(glm::dot(desired, a) / granularity);
		out += a * (k * granularity);
	}
	return out;
}

/** 2 的幂判定（`quantizeShadowDistance` 的档位定义：frexp 尾数恰为 0.5）。 */
bool isPowerOfTwo(double v) {
	int exponent = 0;
	return v > 0.0 && std::isfinite(v) && std::frexp(v, &exponent) == 0.5;
}

/** 整数倍判定（返回商，供失败信息点名「差多少」）。 */
double ratioToGrid(double granularity, double texel) { return granularity / texel; }

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 量具自检：读回算子自己要先证明没写错（否则下面所有读数都可能是量具的 bug）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("量具自检：从 view 矩阵读回的光基与吸附盒心，对上 lookAt 的定义与 round 的硬界") {
	// 为什么必须先有这条：本批全部读数都来自 `readbackSnappedCenter`，而它是**测试侧**从
	// glm::lookAt 的平移列反推的。d 轴符号写反（行 2 存的是 −f）不会编译错、不会崩，只会让
	// 读数整体漂掉上千纹素 —— skill「辅助函数也要有独立判据，且判据的数值噪声必须远小于
	// 它要检测的信号」。这里用三条**不调用被测吸附算式**的硬界拦它。
	const CascadeFrame frame = buildFrame(glm::dvec3{0.0});
	REQUIRE(kTile == 1024u);  // 前提：出厂 4 级 ⟹ 2×2 图集 ⟹ 单块 1024

	int snappedAwayFromCenter = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const auto rb = readbackSnappedCenter(frame.volumes[c]);
		const auto ax = axesOf(rb);
		const double g = static_cast<double>(frame.texels[c]);
		REQUIRE(g > 0.0);

		// ① 光基必须是单位正交右手系（lookAt 的定义；符号写反会让 det = −1 或点积不为 0）
		for (int a = 0; a < 3; ++a) {
			INFO("c = " << c << " 轴 " << kAxisNames[a]);
			testing::expectNear(glm::length(ax[a]), 1.0, 1e-6, 1e-6);
			for (int b = 0; b < 3; ++b)
				testing::expectNear(glm::dot(ax[a], ax[b]), a == b ? 1.0 : 0.0, 1e-6, 1e-6);
		}
		// 手性：生产的光基是 u = s×d ⟹ s×u = s×(s×d) = **−d**（不是右手系）。
		// 真正的右手三件套是 view 的三行 (s, u, −d)。这条把「d 轴符号写反」钉死：
		// 写反后点积变 +1，本条红。
		testing::expectNear(glm::dot(glm::cross(rb.s, rb.u), rb.d), -1.0, 1e-5, 1e-5);

		// ② 读回的盒心坐标必须**恰在 texel 格点上**（吸附真的按这枚格距做的机器证据）
		for (int a = 0; a < 3; ++a) {
			const double onGrid = std::round(rb.q[a] / g) * g;
			INFO("c = " << c << " 轴 " << kAxisNames[a] << " q = " << rb.q[a] << " g = " << g);
			testing::expectNear(rb.q[a], onGrid, 0.0, 1e-3 * g);
		}

		// ③ 吸附位移 ≤ 半个纹素（round 的定义 ⟹ 硬界）。符号写反会差出 2·|dot(盒心,轴)|。
		const glm::dvec3 center{glm::dvec3{frame.slices[c].center}};
		for (int a = 0; a < 3; ++a) {
			const double raw = glm::dot(center, ax[a]);
			INFO("c = " << c << " 轴 " << kAxisNames[a] << " 球心坐标 = " << raw
			           << " 读回盒心 = " << rb.q[a] << " 半纹素 = " << 0.5 * g);
			CHECK(std::abs(rb.q[a] - raw) <= 0.5 * g + 1e-3 * g);
			if (std::abs(rb.q[a] - raw) > 0.01 * g)
				++snappedAwayFromCenter;  // 吸附**真的动了**（不是恒等），阳性锚点
		}

		// ④ 切片球心在该级光空间里的深度 = dot(球心 − 盒心, d)，必须 ≤ 半纹素
		const glm::dvec4 ls = glm::dmat4(frame.volumes[c].view) *
		                      glm::dvec4(center, 1.0);
		const double t = -ls.z - static_cast<double>(frame.volumes[c].eyeOffset);
		CHECK(std::abs(t) <= 0.5 * g + 1e-3 * g);
	}
	// 阳性锚点：12 个 (级, 轴) 里必须**至少一个**被吸附挪动过（全 0 = 吸附恒等 = 下面所有
	// 「逐位不变」都是空判据）。先例 = test_shadow_geometry「texel 吸附世界锚定且真实发生」。
	CHECK(snappedAwayFromCenter > 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 1：出厂态四级 texel 的实际数值、G、以及「G 是否整除每一级」
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("测 1 出厂态四级纹素：G 是 2 的幂，但**没有任何一级**纹素整除 G（结论 B 的读数）") {
	// 真实链路：quantizeShadowDistance → computeCascadeSpheres → shadowTexelSize →
	// rebaseSnapGranularity。期望值出处 = 闭式（2 的幂的档位定义 = frexp 尾数 0.5；
	// 整除性 = 商恰为整数），不抄被测码的算式。
	const CascadeFrame frame = buildFrame(glm::dvec3{0.0});
	std::array<float, kActiveShadowCascades> texels = frame.texels;

	// 前提守卫：四级全填、逐级递增（否则「最粗级」这个词没有指代，下面的读数会失去意义）
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		CHECK(texels[c] > 0.0f);
		if (c + 1u < kActiveShadowCascades)
			CHECK(texels[c] < texels[c + 1u]);
	}

	const double G = rebaseSnapGranularity(std::span<const float>{texels.data(), texels.size()});
	MESSAGE("【读数】出厂四级纹素 (c0..c3) = " << texels[0] << ", " << texels[1] << ", " << texels[2]
	                                       << ", " << texels[3] << "；G = " << G);
	CHECK(std::isfinite(G));
	// G 是 2 的幂（rebaseSnapGranularity 的格点嵌套前提）
	CHECK(isPowerOfTwo(G));
	const double coarsest = texels[kActiveShadowCascades - 1];
	CHECK(G >= coarsest);
	CHECK(G < 2.0 * coarsest);

	// 🔴 本批读数之一：**每一级**纹素自己不是 2 的幂，且 G / 该级纹素不是整数。
	// 「不是整数」= 原点沿吸附格跳一格，该级图集相对世界就挪「非整数个纹素」⟹ 采样格错位。
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double ratio = ratioToGrid(G, static_cast<double>(texels[c]));
		MESSAGE("c = " << c << " texel = " << texels[c] << " 是 2 的幂？" << isPowerOfTwo(static_cast<double>(texels[c]))
		           << " G/texel = " << ratio << " 是整数？" << (ratio == std::round(ratio)));
		CHECK_FALSE(isPowerOfTwo(static_cast<double>(texels[c])));
		CHECK_FALSE(ratio == std::round(ratio));
		// 商必须 > 1（G 是最粗级的上取整，比细级纹素粗）
		CHECK(ratio > 1.0);
	}
}

TEST_CASE("测 1 的独立第二实现：纹素 = 2·闭式拟合球半径/块边长（半径由 8 角共球解出，不抄被测代数）") {
	// 上面那条读的是生产链的产物；这条证明**产物本身没算错**，否则「G 不整除 texel」
	// 可能只是半径算错。期望值 = 从「视锥 8 角到视轴上某点等距」这个几何定义在 double 域
	// 解出的半径（推导写在函数里），**不是** `frustumBoundingSphere` 那两行。
	// 生产半径带 ×1.001 的保守余量（写在 scene_context 注释里），所以判据是
	// 「生产 ≥ 闭式」且「比值 < 1.002」，不是逐位。
	const CascadeFrame frame = buildFrame(glm::dvec3{0.0});
	const double tanHalf = std::tan(static_cast<double>(glm::radians(kFovYDeg)) * 0.5);
	const double aspect = static_cast<double>(kAspect);
	const float nearQ = quantizeShadowDistance(kNearRaw);
	const float farQ = quantizeShadowDistance(kFarRaw);
	REQUIRE(farQ > nearQ);

	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		// 该级子视锥的深度界（切分算式本身是「实用切分」的定义，出处 = cascadeSplitDistance
		// 的函数头；这里只**用**它取 n/f，判据长在半径上）
		const float farC = cascadeSplitDistance(nearQ, farQ, c, kActiveShadowCascades);
		const float nearC = c == 0u ? 0.02f : cascadeSplitDistance(nearQ, farQ, c - 1u, kActiveShadowCascades);
		// 闭式：角点 (±t·tanHalf·aspect, ±t·tanHalf, −t) 到 (0,0,−zc) 等距
		//   n²·(1+cs²) + 2·zc·n·… 直接解线性方程：|corner(t) − center|² 与 t 无关
		//   ⟹ zc = (f² − n² + rf² − rn²) / (2(f − n))，cs = tanHalf·√(aspect²+1)
		const double n = nearC, f = farC;
		const double cs = tanHalf * std::sqrt(aspect * aspect + 1.0);
		const double rn = n * cs, rf = f * cs;
		const double zc = (f * f - n * n + rf * rf - rn * rn) / (2.0 * (f - n));
		const double rNear = std::sqrt((n - zc) * (n - zc) + rn * rn);
		const double rFar = std::sqrt((f - zc) * (f - zc) + rf * rf);
		// 等距自检：近角与远角到解出的球心**同距**（这是「8 角共球」的定义，也是 zc 解对了的证据）
		testing::expectNear(rNear, rFar, 1e-9);
		const double rClosed = rNear;
		const double rActual = frame.volumes[c].xyRadius;
		INFO("c = " << c << " 闭式半径 = " << rClosed << " 生产半径 = " << rActual
		           << " 比值 = " << rActual / rClosed);
		CHECK(rActual >= rClosed);            // 保守方向：宁大勿小（漏覆盖 = 漏画投影体）
		CHECK(rActual / rClosed < 1.002);     // 且只大那一点（×1.001 余量）
		// 纹素 = 2r/块边长（与 shadowTexelSize 同口径的**独立**换算）
		testing::expectNear(static_cast<double>(frame.texels[c]),
		                    2.0 * rActual / static_cast<double>(kTile), 1e-6);
	}
}

TEST_CASE("测 1 的光基：出厂太阳方向下三轴**不退化**成世界轴对齐（结论 A 的前提）") {
	const CascadeFrame frame = buildFrame(glm::dvec3{0.0});
	const auto rb = readbackSnappedCenter(frame.volumes[0]);
	const auto ax = axesOf(rb);
	MESSAGE("【读数】光基 s = " << rb.s.x << "," << rb.s.y << "," << rb.s.z
	                       << "  u = " << rb.u.x << "," << rb.u.y << "," << rb.u.z
	                       << "  d = " << rb.d.x << "," << rb.d.y << "," << rb.d.z);

	// 不退化 = 没有任何一根轴与世界 xyz 平行（平行的判据：最大分量模长 == 1）。
	// 太阳方向不沿坐标轴 ⟹ s 在 xz 平面内斜指、u 三个分量都非零。
	for (int a = 0; a < 3; ++a) {
		const glm::dvec3 v = ax[a];
		const double maxAbs = std::max({std::abs(v.x), std::abs(v.y), std::abs(v.z)});
		int nonzero = 0;
		for (const double component : {v.x, v.y, v.z})
			if (std::abs(component) > 1e-3)
				++nonzero;
		INFO("轴 " << kAxisNames[a] << " 最大分量 = " << maxAbs << " 非零分量数 = " << nonzero);
		CHECK(nonzero >= 2);           // 只有一根分量非零 = 与世界轴平行 = 退化
		CHECK(maxAbs < 0.99);
	}

	// 🔴 结论 A 的直接读数：世界轴吸附给出的原点位移是 G·整数向量，它在光基轴上的投影
	// dot(G·e, a) = G·(e·a) = G·a_分量，**不是** G 的整数倍（因为 a 的分量不是整数）。
	const double G = rebaseSnapGranularity(
		std::span<const float>{frame.texels.data(), frame.texels.size()});
	for (int a = 0; a < 3; ++a)
		for (int e = 0; e < 3; ++e) {
			const double proj = comp(ax[a], e) * G;  // dot(G·e_e, axis_a)
			const double ratio = proj / static_cast<double>(frame.texels[0]);
			INFO("世界轴 e" << e << " 在光基轴 " << kAxisNames[a] << " 上的投影 / c0 texel = " << ratio
			               << " 整数？" << (ratio == std::round(ratio)));
			// 只要该分量非零，商就不是整数（G·a_分量 / texel 落在整数上要求 a_分量 极特殊）
			if (std::abs(comp(ax[a], e)) > 1e-3)
				CHECK_FALSE(ratio == std::round(ratio));
		}
}

TEST_CASE("测 1 的稳健性：扫 9 组深度界，「G 不整除任何一级纹素」不是某一组配置的巧合") {
	// 出厂那一帧的 near/far 要 SceneBounds（要装模型），无设备单测拿不到 ⟹ 本批只能取台账
	// 读数。这条把结论 B 从「一组配置的读数」抬到「跨深度界档位的普适读数」。
	// 扫的两族：① near 落在自适应下限（贴脸机位，= 债-139 那组）；② near 明显大于下限
	// （交付机位那一族：camera.lua 机位离场景球心 ≈ 1.54、球半径 ≈ 1.17 ⟹ near ≈ 0.37、
	// far ≈ 2.71，量化后落 0.5 / 4 档）。
	struct BandPair {
		float nearRaw;
		float farRaw;
	};
	int bandsChecked = 0, bandsWithAnyDivisible = 0;
	for (const BandPair bp : {BandPair{0.05f, 0.9f}, BandPair{0.05f, 1.713496f}, BandPair{0.05f, 3.3f},
		                      BandPair{0.05f, 6.5f}, BandPair{0.05f, 13.0f}, BandPair{0.05f, 30.0f},
		                      BandPair{0.37f, 2.71f}, BandPair{0.5f, 4.0f}, BandPair{1.2f, 9.0f}}) {
		const float farQ = quantizeShadowDistance(bp.farRaw);
		const float nearQ = quantizeShadowDistance(bp.nearRaw);
		REQUIRE(farQ > nearQ);
		ViewProjection vp;
		vp.view = glm::lookAt(glm::vec3{glm::dvec3{kEyeAbs}}, glm::vec3{glm::dvec3{kTargetAbs}},
		                      glm::vec3{0.0f, 1.0f, 0.0f});
		vp.proj = glm::perspective(glm::radians(kFovYDeg), kAspect, bp.nearRaw, bp.farRaw);
		vp.nearPlane = bp.nearRaw;
		vp.farPlane = bp.farRaw;
		const auto spheres = computeCascadeSpheres(vp, nearQ, farQ);
		std::array<float, kActiveShadowCascades> texels{};
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
			texels[c] = shadowTexelSize(spheres[c].radius);
		const double G = rebaseSnapGranularity(std::span<const float>{texels.data(), texels.size()});
		// 前提守卫：粒度必须真的来自**最粗**一级（否则「整除性」读的是别的量）
		CHECK(G >= static_cast<double>(texels[kActiveShadowCascades - 1]));
		int divisible = 0;
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const double ratio = static_cast<double>(G) / static_cast<double>(texels[c]);
			if (ratio == std::round(ratio))
				++divisible;
			MESSAGE("【读数】near 档 " << nearQ << " far 档 " << farQ << " 级 " << c
			          << " texel = " << texels[c] << " G/texel = " << ratio);
		}
		// 末级纹素与 G 同档时**可能**整除（商恰为 1），其余不许。三级里只要有一级整除，
		// 「跨格跳变」就在那一级消失 —— 实测 9 组里三级都不整除。
		CHECK(divisible <= 1);
		CHECK_FALSE(texels[0] == texels[kActiveShadowCascades - 1]);  // 前提：四级不是同一个数
		++bandsChecked;
		bandsWithAnyDivisible += divisible > 0 ? 1 : 0;
	}
	CHECK(bandsChecked == 9);
	MESSAGE("【读数】跨 9 组深度界：出现「至少一级整除 G」的档位数 = " << bandsWithAnyDivisible);
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 2：决定性测量 —— 原点跨一格（现状 H0）后图集是否逐位不变
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 测 2 决定性测量：现状吸附下原点跨一格，图集相对世界**确实错位**（读数 = 跳变量）") {
	// 这就是批 5「阴影图集逐位不变」判据的 CPU 等价形式。逐位相同**不成立** ⟹ 本用例把它
	// 钉成「不成立」的判据，并把跳变量报出来（任务书：这个数比红/绿有用）。
	// 期望值出处 = 闭式 round 恒等式（文件头那条推导），先算模型再看真实产物。
	const CascadeFrame base = buildFrame(glm::dvec3{0.0});
	const double G = rebaseSnapGranularity(
		std::span<const float>{base.texels.data(), base.texels.size()});
	REQUIRE(isPowerOfTwo(G));

	// 出厂态原点的来源 = 相机位置 snap 到格（批 3 契约）。desired 取交付机位本身。
	const glm::dvec3 origin = snapOriginToGranularity(kEyeAbs, G);
	INFO("【读数】G = " << G << " 期望原点 = " << kEyeAbs.x << "," << kEyeAbs.y << ","
	                   << kEyeAbs.z << " ⟹ 吸附后 = " << origin.x << "," << origin.y << ","
	                   << origin.z);
	// 前提：吸附真的动了（否则两套是同一帧，「逐位相同」是空的）
	CHECK(glm::length(origin - kEyeAbs) > 0.0);

	const CascadeFrame moved = buildFrame(origin);
	const JumpReading r = measureJump(base, moved, origin);
	reportJump(0, r);

	// ① 量具自洽：逐点位移与闭式模型对上（模型先算，实机后读；对不上 = 模型或量具错）
	const double modelMax = r.perCascadeAxisEffective[r.worstCascade][r.worstAxis];
	CHECK(modelMax > 0.0);
	// 逐点位移的「级内散布」远小于位移本身 ⟹ 整张图集**刚性平移**（不是逐点游动）。
	// 散布不归零的原因是浮点重舍入（每个点的相对坐标各自收窄成 float32），量级 1e-4 纹素。
	// 这是「一次跳变」这个说法的机器形态。
	CHECK(r.pointSpread < 1e-3);
	CHECK(r.pointSpread * 100.0 < r.maxPointDelta);

	// ② 🔴 主读数：跳变量。出厂态实测最大有效错位 ≈ 0.46 纹素（级 3 的 s 轴）。
	// 闭式上界 = 0.5 纹素（δ ∈ [−1,1] 纹素 ⟹ 到最近整数 ≤ 0.5）。
	CHECK(r.maxEffective > 0.1);       // 现状读数锚：门开后的阴影跳变**在**这里
	CHECK(r.maxEffective <= 0.5 + 1e-3);
	// 逐点位移与模型给出的原始 δ 同量级（差在「整纹素平移」上，所以比有效错位）
	CHECK(r.maxPointDelta > 0.1);

	// ③ 逐位（memcmp）：图集坐标不许相同。这条是任务书「断言两套逐位相同」的**反面**——
	// 实测不成立，故钉成「不相同」；哪天成立说明格条件被修好，必须回来重立本读数。
	int identical = 0, differ = 0;
	for (const glm::dvec3& p : makeTestPoints()) {
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const glm::dvec3 a0 = atlasTexelOf(base.volumes[c], base.texels[c], p, glm::dvec3{0.0});
			const glm::dvec3 a1 =
				atlasTexelOf(moved.volumes[c], moved.texels[c], p, origin);
			if (bitwiseEqual(a0, a1))
				++identical;
			else
				++differ;
		}
	}
	CHECK(differ > 0);
	CHECK(identical == 0);  // 一个点一级都不许相同（刚性平移 + 非整纹素 ⟹ 全体分家）
}

TEST_CASE("测 2 的闭式对账：实测跳变 = round 恒等式预测的 δ（模型先算，实机后读）") {
	// 这条把「测 2 的读数」与「文件头那条闭式推导」逐 (级, 轴) 对上。
	// 期望值出处 = 纯算术：δ_a = q_a(0) − q_a(O₁) − dot(O₁, a)，其中 q 从真实产物读回。
	// 判据不是「δ = 0」（那是希望），而是「实测逐点位移 == δ」—— 它证明我对失效机理的理解
	// 与现场一致，而不是只看见了一堆非零数。
	const CascadeFrame base = buildFrame(glm::dvec3{0.0});
	const double G = rebaseSnapGranularity(
		std::span<const float>{base.texels.data(), base.texels.size()});
	const glm::dvec3 origin = snapOriginToGranularity(kEyeAbs, G);
	const CascadeFrame moved = buildFrame(origin);
	const JumpReading r = measureJump(base, moved, origin);

	const auto points = makeTestPoints();
	REQUIRE(!points.empty());
	int compared = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		// 逐点位移（x/y 两个分量各自对应一根光基轴）
		for (int a = 0; a < 2; ++a) {
			double measured = 0.0;
			for (const glm::dvec3& p : points) {
				const glm::dvec3 a0 = atlasTexelOf(base.volumes[c], base.texels[c], p, glm::dvec3{0.0});
				const glm::dvec3 a1 = atlasTexelOf(moved.volumes[c], moved.texels[c], p, origin);
				const double d = comp(a1, a) - comp(a0, a);
				if (std::abs(d) > std::abs(measured))
					measured = d;
			}
			INFO("c = " << c << " 轴 " << kAxisNames[a] << " 逐点位移 = " << measured
			           << " 闭式 δ = " << r.perCascadeAxis[c][a]);
			// 逐点位移 ≈ 那个常数 δ（刚性平移 ⟹ 所有点同值）。容差 1e-3 纹素 = 浮点重舍入
			// 噪声量级（相对坐标先收窄成 float32 再进吸附），而跳变本身 0.1–0.5 纹素。
			testing::expectNear(measured, r.perCascadeAxis[c][a], 1e-3, 1e-3);
			++compared;
		}
	}
	CHECK(compared == 8);  // 空转守卫：4 级 × 2 根 XY 轴
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 3：三种假设的对照
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 测 3 三假设对照：H0 世界轴 / H1 光基 / H2 光基 + 每级纹素抬 2 的幂（只有 H2 归零）") {
	// H0 = 现状（世界轴吸附，粒度 G = 最粗级纹素抬 2 的幂）
	// H1 = 只改吸附基（光基），粒度仍取 G
	// H2 = 光基 + 每级纹素各自抬 2 的幂后的公共粗格（= 让 G 真能整除每一级）
	//     H2 的盒要放大（纹素变粗）⟹ 生产没有这个形态，本用例用「改入参」的方式喂给
	//     **真实** buildShadowVolume：半径取 512·g' 时函数内部算出的 texel 恰为 g'（幂次缩放
	//     在 IEEE 下精确），所以 H2 不需要在测试侧重写吸附算式。
	const CascadeFrame base = buildFrame(glm::dvec3{0.0});
	const double G = rebaseSnapGranularity(
		std::span<const float>{base.texels.data(), base.texels.size()});
	const auto axes = axesOf(readbackSnappedCenter(base.volumes[0]));

	// ── H0 ──
	const glm::dvec3 o0 = snapOriginToGranularity(kEyeAbs, G);
	const JumpReading h0 = measureJump(base, buildFrame(o0), o0);
	reportJump(0, h0);

	// ── H1 ──
	const glm::dvec3 o1 = snapInLightBasis(kEyeAbs, G, axes);
	// 光基吸附自己的契约：dot(结果, 每根轴) 是 G 的整数倍（不满足则 H1 这一臂没在测它以为的东西）
	for (int a = 0; a < 3; ++a) {
		const double k = glm::dot(o1, axes[a]) / G;
		INFO("H1 吸附自检 轴#" << a << " k = " << k << " 离整数 = " << fracDistance(k));
		// 容差而不是逐位：光基是从 float 矩阵读回的，正交性只有 ~1e-7，dot(Σ m·G·b, a) 会带上
		// 非同轴项的残量（实测 ~1e-5 个 G）。逐位判整数会把这条自检变成假红。
		CHECK(fracDistance(k) < 1e-3);
	}
	const JumpReading h1 = measureJump(base, buildFrame(o1), o1);
	reportJump(1, h1);

	// ── H2 ──
	std::array<float, kActiveShadowCascades> raised{};
	std::array<double, kActiveShadowCascades> cost{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		raised[c] = quantizeShadowDistance(base.texels[c]);  // 每级纹素抬到 2 的幂
		cost[c] = static_cast<double>(raised[c]) / static_cast<double>(base.texels[c]);
		CHECK(isPowerOfTwo(static_cast<double>(raised[c])));
		CHECK(raised[c] >= base.texels[c]);  // 量化只向上（盒只放大不缩小）
	}
	double coarseRaised = 0.0;
	for (const float g : raised)
		coarseRaised = std::max(coarseRaised, static_cast<double>(g));
	// 公共粗格必须真能整除每一级（H2 的定义，也是它唯一的可验之处）
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double ratio = coarseRaised / static_cast<double>(raised[c]);
		INFO("H2 整除自检 c = " << c << " G'/texel = " << ratio);
		CHECK(ratio == std::round(ratio));
	}
	// 半径放大到 512·g'（= kTile/2 · g'）⟹ 函数内部 texel = 2r/kTile 恰为 g'
	std::array<float, kActiveShadowCascades> radii{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		radii[c] = static_cast<float>(static_cast<double>(raised[c]) * 0.5 * static_cast<double>(kTile));
	const CascadeFrame h2Base = buildFrame(glm::dvec3{0.0}, radii.data());
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		CHECK(h2Base.texels[c] == raised[c]);  // 放大后的盒确实吐出目标纹素（逐位）
	const glm::dvec3 o2 = snapInLightBasis(kEyeAbs, coarseRaised, axes);
	const JumpReading h2 = measureJump(h2Base, buildFrame(o2, radii.data()), o2);
	reportJump(2, h2);

	// ── 三条判决 ──
	MESSAGE("【读数】H0 最大有效错位 = " << h0.maxEffective << " 纹素；H1 = " << h1.maxEffective
	        << " 纹素；H2 = " << h2.maxEffective << " 纹素");
	CHECK(h0.maxEffective > 0.1);  // 现状：跳变在
	CHECK(h1.maxEffective > 0.1);  // 🔴 只改吸附基**不解决问题**（结论 B 成立）
	CHECK(h2.maxEffective < 1e-3); // 格条件满足后只剩浮点重舍入噪声
	CHECK(h2.maxEffective * 100.0 < h1.maxEffective);

	// H2 的画质代价（盒半径放大倍数 = 纹素变粗倍数 = 分辨率损失倍数）
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		MESSAGE("【读数】H2 代价 级 " << c << " 盒半径/纹素放大倍数 = " << cost[c]);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		CHECK(cost[c] >= 1.0);
		CHECK(cost[c] < 2.0);  // 量化向上取 2 的幂 ⟹ 严格小于 2 倍
	}
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 4：反向半边 —— 证明比较算子有区分力（不是「两套路径压根没差别」）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 测 4 比较算子的区分力：同帧必逐位相同 / 脱格原点必分家 / 亚容差偏差只有逐位能看见") {
	// 任务书要求这条防的是「逐位相同可能只是因为两套代码路径压根没差别」。本条做**三**半边，
	// 第三半边是任务书那条变异（把比较算子换成容差 1e-3）的抓手 —— 只按任务书字面写前两边，
	// 那条变异**不会红**（实测，读数 → 交付报告），因为脱格原点的偏差远大于 1e-3。
	const CascadeFrame base = buildFrame(glm::dvec3{0.0});
	const double G = rebaseSnapGranularity(
		std::span<const float>{base.texels.data(), base.texels.size()});
	const glm::dvec3 onGrid = snapOriginToGranularity(kEyeAbs, G);
	const auto points = makeTestPoints();
	REQUIRE(points.size() == 64);

	// ── 半边 ①：同一个原点**独立重建两帧** ⟹ 必须逐位相同 ──
	// 这条是算子的阴性对照：算子若恒报「不同」，下面两条的「分家」毫无内容。
	// 它同时把「引擎对同一入参逐位确定」当判据用（先例 = 债-139 的阴性对照：同机位跑两次
	// 逐位相同，才让后面的所有读数不是噪声）。
	const CascadeFrame first = buildFrame(onGrid);
	const CascadeFrame second = buildFrame(onGrid);
	int identicalSame = 0;
	for (const glm::dvec3& p : points)
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
			identicalSame += bitwiseEqual(atlasTexelOf(first.volumes[c], first.texels[c], p, onGrid),
			                             atlasTexelOf(second.volumes[c], second.texels[c], p, onGrid))
			                     ? 1
			                     : 0;
	CHECK(identicalSame == 64 * static_cast<int>(kActiveShadowCascades));

	// ── 半边 ②：故意脱格的原点 ⟹ 偏差必须不为 0，且远大于容差 ──
	const glm::dvec3 offGrid = onGrid + glm::dvec3{0.317, 0.0, 0.0};
	const CascadeFrame moved = buildFrame(offGrid);
	const JumpReading r = measureJump(base, moved, offGrid);
	reportJump(3, r);
	int differOff = 0;
	double maxOff = 0.0;
	for (const glm::dvec3& p : points)
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const glm::dvec3 a0 = atlasTexelOf(base.volumes[c], base.texels[c], p, glm::dvec3{0.0});
			const glm::dvec3 a1 = atlasTexelOf(moved.volumes[c], moved.texels[c], p, offGrid);
			if (!bitwiseEqual(a0, a1))
				++differOff;
			maxOff = std::max({maxOff, std::abs(a1.x - a0.x), std::abs(a1.y - a0.y)});
		}
	CHECK(differOff == 64 * static_cast<int>(kActiveShadowCascades));
	CHECK(maxOff > 1e-3);  // 远大于容差 ⟹ 这一半边对「容差化变异」不敏感（见半边 ③）

	// ── 半边 ③：格条件满足、但偏差**小于 1e-3 纹素**的那一臂 ──
	// 出厂态实测：H2（格条件满足）的残余偏差 ≈ 1e-4 纹素（浮点重舍入），逐位判「不同」，
	// 容差 1e-3 判「相同」。⟹ 把比较算子换成容差 1e-3，这一臂立刻从「分家」读成「相同」
	// ⟹ 本用例红。这才是「比较算子有区分力」的完整含义：它必须能看见**小于容差**的偏差。
	std::array<float, kActiveShadowCascades> raised{};
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		raised[c] = quantizeShadowDistance(base.texels[c]);
	std::array<float, kActiveShadowCascades> radii{};
	double coarseRaised = 0.0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		radii[c] = static_cast<float>(static_cast<double>(raised[c]) * 0.5 * static_cast<double>(kTile));
		coarseRaised = std::max(coarseRaised, static_cast<double>(raised[c]));
	}
	const CascadeFrame h2Base = buildFrame(glm::dvec3{0.0}, radii.data());
	const auto axes = axesOf(readbackSnappedCenter(h2Base.volumes[0]));
	const glm::dvec3 o2 = snapInLightBasis(kEyeAbs, coarseRaised, axes);
	const CascadeFrame h2Moved = buildFrame(o2, radii.data());
	int bitwiseDiffer = 0;
	double maxSmall = 0.0;
	for (const glm::dvec3& p : points)
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const glm::dvec3 a0 = atlasTexelOf(h2Base.volumes[c], h2Base.texels[c], p, glm::dvec3{0.0});
			const glm::dvec3 a1 = atlasTexelOf(h2Moved.volumes[c], h2Moved.texels[c], p, o2);
			for (int k = 0; k < 3; ++k)
				maxSmall = std::max(maxSmall, std::abs(comp(a1, k) - comp(a0, k)));
			if (!bitwiseEqual(a0, a1))
				++bitwiseDiffer;
		}
	MESSAGE("【读数】格条件满足时：逐位分家的样本数 = " << bitwiseDiffer << " / "
	                                                << 64 * static_cast<int>(kActiveShadowCascades)
	                                                << "，最大偏差 = " << maxSmall << " 纹素");
	// 🔴 读数本身是判据：**即使格条件完全满足，图集坐标也不逐位相同**，残余来自
	// 「相对坐标先收窄成 float32、再进吸附」这条重舍入路径（量级 1e-4 纹素）。
	// ⟹ 批 5 的「图集级逐位不变」判据要成立，必须先把这条重舍入消掉，否则零容差做不到。
	CHECK(bitwiseDiffer > 0);
	CHECK(maxSmall > 0.0);
	CHECK(maxSmall < 1e-3);  // 落在「逐位看得见、容差 1e-3 看不见」的窗口里
}

TEST_CASE("测 4 的补充：吸附格距与纹素**互质**时跳变量随原点连续变化（不是常数）") {
	// 这条防的是「测 2 的 0.46 纹素只是这一个原点的巧合」。做法：沿光基 s 轴扫一批原点
	// （写死种子），逐报有效错位；判据 = ① 绝大多数非零，② 读数有分布（不是同一个常数），
	// ③ 上界不超过 0.5 纹素（闭式）。
	const CascadeFrame base = buildFrame(glm::dvec3{0.0});
	const double G = rebaseSnapGranularity(
		std::span<const float>{base.texels.data(), base.texels.size()});
	const auto axes = axesOf(readbackSnappedCenter(base.volumes[0]));

	Lcg rng{kSeed + 3u};
	int samples = 0, nonzero = 0;
	double lo = 1e300, hi = -1e300;
	for (int i = 0; i < 12; ++i) {
		const glm::dvec3 desired{kEyeAbs.x + rng.uniform(-4.0, 4.0), kEyeAbs.y,
		                         kEyeAbs.z + rng.uniform(-4.0, 4.0)};
		const glm::dvec3 origin = snapInLightBasis(desired, G, axes);
		const JumpReading r = measureJump(base, buildFrame(origin), origin);
		MESSAGE("【读数】样本 " << i << " 原点 = " << origin.x << "," << origin.y << "," << origin.z
		                   << " 最大有效错位 = " << r.maxEffective << " 纹素");
		CHECK(r.maxEffective <= 0.5 + 1e-3);
		nonzero += r.maxEffective > 1e-3 ? 1 : 0;
		lo = std::min(lo, r.maxEffective);
		hi = std::max(hi, r.maxEffective);
		++samples;
	}
	CHECK(samples == 12);
	CHECK(nonzero >= 11);  // 允许一个「恰好整纹素」的巧合样本
	CHECK(hi - lo > 0.05);  // 读数确实在变（恒等于同一个数 = 扫的是一维假样本）
}

}  // namespace RGL
