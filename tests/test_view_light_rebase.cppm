module;
/**
 * test_view_light_rebase —— 相对坐标批 4 第二刀：view 矩阵 + `SceneLight::eyePos` +
 * `GpuLight::position` 三出口接渲染原点（被测 = camera.cppm::cameraView 及其接线
 * projectionSystem、render_list_impl.cppm::fillLightData）
 *
 * ## 本文件守的不变量（一个不变量的三个出口，不是三件独立的事）
 * 批 3 起进 scene data 的世界坐标是「相对渲染原点」的；本刀起**相机与灯的世界位置**
 * 也相对原点。三处必须同批同空间：只接一半 = 「半接状态」——门关无差异（测不出来）、
 * 门开时 view 对了 eyePos 错，光照/视锥/SSAO 全错位且比全不接更难查。
 *
 * ## 期望值出处（独立于被测实现）
 *  - 门关恒等锚的期望 = **改动前**的表达式本身（`glm::lookAt(glm::vec3{position}, ...)` /
 *    `glm::vec3(L.position)`）——回归锚点，新实现是它的推广，门关必须退化到它
 *    （先例 = test_taa_cross_frame 的门关逐位锚）；
 *  - 精度收益锚的期望 = 闭式两半：① lookAt 的平移列 = (−s·eye, −u·eye, +f·eye)，
 *    (s,u,f) 正交归一 ⟹ **|c3| = |eye|**（门关 |eye| ≈ 3.7e6、减完原点 = 0 的凭据）；
 *    ② 契约「eye/target 各在 double 域减原点、再收窄进 float lookAt」直接写成测试侧的
 *    `glm::vec3{absolute - origin}` + `glm::lookAt(...)`（出处 = 批 4 任务书对改法的定死，
 *    不是抄被测码——被测路径走 toRelative 的矩阵委托，测试路径走裸算式，两条路径必须同值）；
 *  - 三出口同空间的期望 = 「绝对值 − 原点」闭式 + 跨出口判据 `view · (eyePos,1) = (0,0,0,1)`
 *    （eyePos 与 view 同空间 ⟺ 眼点被自己的 view 映到原点；eyePos 若还是绝对坐标，
 *    这个量是 1e6 量级不是 0）；
 *  - 1e6 处 float32 ULP = 2^(19−23) = 0.0625、2e6/3e6 处 = 0.25（1e6 ∈ [2^19,2^20)）——
 *    「先收窄后减」与「先减后收窄」的可区分性由此手算（逐分量验证写在用例的区分力守卫里）。
 *
 * ## 变异红线声明（哪个变异让我红；全部实测，读数 → 交付报告。回滚只用不带 -p 的 cp）
 *  - toRelative 点版改「先收窄后减」（`vec3{absolute} - vec3{origin}`）
 *      → 「顺序承重」那条红 + 三出口闭式那条红（门关恒等**不红**：原点 0 两序皆恒等，设计如此）
 *  - cameraView 不减原点（eye 改回 `glm::vec3{camera.position}`）
 *      → 精度锚 / 顺序承重 / 三出口 / 不漏改 / 接线 五条全红。
 *      ⚠ 朴素写法（直接弃用 origin）**编译就红**（-Werror,-Wunused-parameter）= 批 4 计划
 *      预告的「单消费者期编译器免费保护」在本出口实测成立；实跑加了 `(void)origin`
 *      保留参数使用，让**判据**而非编译器出声（skill「谓词类变异」同款理由）。
 *  - fillLightData 的 eyePos 不减原点（直接用 cameraPosition 收窄）
 *      → 三出口（eyePos 闭式 + 跨出口判据）/ 不漏改 两条红
 *  - fillLightData 的灯位置不减原点（改回 `glm::vec3(L.position)`）
 *      → 三出口（lightArray 闭式）/ 不漏改 两条红
 *  - projectionSystem 的 origin 实参改成 dvec3{0}（「接了但没接上」）
 *      → 接线锚那条红（本文件其余不红——它就是为这一格存在的唯一判据）
 *  - projectionSystem 的 near/far 距离也减原点（顺手改坏「不许改」表）
 *      → 接线锚的 near/far 不变性断言红（过度修改的半边各要一条，防只验一半）
 */
#include <doctest/doctest.h>
#include <entt/entt.hpp>  // entt 非模块化（main 同款 GMF include）
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt（测试侧闭式期望）
#include <initializer_list>              // modules 坑：花括号列表不从 import 带进来
#include <cmath>                         // std::isfinite

export module test_view_light_rebase;

import rlstd;
import test_support;             // testing::Lcg / expectNear
import camera;                   // Camera / CameraMode / cameraView / cameraForward / cameraUp
import ecs;                      // projectionSystem（接线）/ RenderExtent
import scene_context;            // Light / SceneBounds / ViewProjection / RenderOrigin
import render_list;              // fillLightData（12.3d 接缝）
import frame_draw_list;          // FrameDrawList
import lights_types_generated;   // GpuLight / kMaxLights

namespace RGL {
namespace {

using testing::Lcg;

/** 门关原点 = 出厂默认（减法恒等）。门开态的判据全部显式传非零原点。 */
constexpr glm::dvec3 kNoOrigin{0.0, 0.0, 0.0};

/** 随机位姿种子（写死，可重放）。 */
constexpr uint32_t kSeed = 20261025u;

/** 逐分量 float ==（== 对有限 float 即逐位相同；NaN 恒 false = 必红，正是逐位判据的语义）。 */
void checkBitwise(const glm::mat4& actual, const glm::mat4& expected) {
	for (int col = 0; col < 4; ++col)
		for (int row = 0; row < 4; ++row)
			CHECK(actual[col][row] == expected[col][row]);
}

void checkBitwise(const glm::vec3& actual, const glm::vec3& expected) {
	for (int k = 0; k < 3; ++k)
		CHECK(actual[k] == expected[k]);
}

/** view 平移列（c3 的 xyz）。 */
glm::vec3 translationCol(const glm::mat4& view) { return glm::vec3{view[3]}; }

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

/** 造一个 fps 相机（position 绝对层直写，yaw/pitch 给弧度）。 */
Camera makeFps(const glm::dvec3& position, float yaw, float pitch) {
	Camera cam;
	cam.position = position;
	cam.yaw = yaw;
	cam.pitch = pitch;
	cam.mode = CameraMode::Fps;
	return cam;
}

/** 造一个 trackball 相机（target 与默认 up=(0,1,0) 不近平行，避开 lookAt 退化）。 */
Camera makeTrackball(const glm::dvec3& position, const glm::dvec3& target) {
	Camera cam;
	cam.position = position;
	cam.target = target;
	cam.mode = CameraMode::Trackball;
	return cam;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 判据 ①（门关逐位不变）的单元级对应
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cameraView 门关恒等：origin = 0 ⟹ 两模式逐位 = 改前 lookAt(收窄) 式（网格 + 256 随机位姿）") {
	// 期望 = **改动前**的表达式本身（回归锚点）。门关态 toRelative 的减法 = 减 +0.0 逐位
	// 返回自身 ⟹ 新路径与它唯一的差别应为 0 个 ulp——逐位断言，不许容差。
	// 随机位姿用 Lcg 写死种子（确定性）；trackball 侧拒绝近竖直视线（up ∥ 视线 = glm
	// 无护栏的 NaN 输入，不是本判据的射程）。
	Camera base;
	base.position = glm::dvec3{1.5, -2.0, 3.25};
	Lcg rng{kSeed};
	int checked = 0;
	for (int i = 0; i < 256; ++i) {
		const glm::dvec3 eye{rng.uniform(-800.0, 800.0), rng.uniform(-800.0, 800.0),
			rng.uniform(-800.0, 800.0)};
		const float yaw = static_cast<float>(rng.uniform(-3.1, 3.1));
		const float pitch = static_cast<float>(rng.uniform(-1.5, 1.5));
		// fps：期望 = 批 2 第一刀的原式
		const Camera fps = makeFps(eye, yaw, pitch);
		const glm::vec3 eyeF{eye};
		checkBitwise(cameraView(fps, kNoOrigin),
			glm::lookAt(eyeF, eyeF + cameraForward(fps), cameraUp(fps)));
		// trackball：target = eye + 随机方向（拒绝近竖直：|dir.y| > 0.9|dir| 时 up∥视线）
		glm::dvec3 dir{rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0), rng.uniform(-1.0, 1.0)};
		if (std::abs(dir.y) > 0.9 * glm::length(dir))
			continue;
		Camera tb = makeTrackball(eye, eye + dir * rng.uniform(1.0, 50.0));
		tb.up = base.up;  // 世界 up（默认值，与生产 config 加载后的形态一致）
		checkBitwise(cameraView(tb, kNoOrigin),
			glm::lookAt(eyeF, glm::vec3{tb.target}, tb.up));
		++checked;
	}
	CHECK(checked >= 128);  // 空转守卫：拒绝分支不得是主路径
}

TEST_CASE("fillLightData 门关恒等：origin = 0 ⟹ eyePos/灯位置逐位 = 改前收窄式（1e6 量级下不多一次舍入）") {
	// 门关红线在灯这一侧的对应：减法恒等 ⟹ 落库 = 对绝对层原值做**一次**收窄，
	// 与改前 `glm::vec3(camera.position)` / `glm::vec3(L.position)` 逐位相同。
	// 输入刻意取 1e6 量级带小数——若门关路径混进任何「先减后舍」之外的第二次舍入，这里红。
	entt::registry rg;
	Light L;
	L.index = 0;
	L.position = glm::dvec3(1.0e6 + 0.1, -2.0e6 + 0.3, 3.0e6 + 0.7);
	rg.emplace<Light>(rg.create(), L);
	const glm::dvec3 camPos{1.0e6 + 0.2, -2.0e6 + 0.5, 3.0e6 + 0.9};

	FrameDrawList list;
	fillLightData(rg, camPos, kNoOrigin, list);

	checkBitwise(list.sceneData.light.eyePos, glm::vec3{camPos});  // 改前原式
	CHECK(list.sceneData.lightArray[0].position
		== glm::vec4(glm::vec3(L.position), 0.0f));  // 改前原式（逐位）
}

// ═══════════════════════════════════════════════════════════════════════════
// 本刀的灵魂：精度收益锚点（没有它，本刀就只是把减法挪了个地方）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 精度收益锚点：|camera| ≈ 1e6 且 origin 取同一位置 ⟹ view 平移列从 1e6 塌到 0（闭式）") {
	// 闭式：lookAt 的平移列 = (−s·eye, −u·eye, +f·eye)，(s,u,f) 正交归一 ⟹ |c3| = |eye|。
	//  ⟹ 门关 |c3| = |float(camPos)| ≈ 3.7e6（债-85 ① 的抖动主导项本体）；
	//     origin = camPos ⟹ eye_rel = 0 ⟹ |c3| = 0。
	// 「远小于」不是趋势描述而是可逐位断言的两端：0 与 >1e6。
	const glm::dvec3 camPos{1.0e6 + 0.1, -2.0e6 + 0.3, 3.0e6 + 0.7};
	const glm::dvec3 target = camPos + glm::dvec3{0.5, -1.5, 2.0};
	for (const bool fps : {true, false}) {
		const Camera cam = fps ? makeFps(camPos, 0.7f, -0.3f) : makeTrackball(camPos, target);
		const glm::mat4 closed = cameraView(cam, camPos);  // 原点 = 相机位置本身
		const glm::mat4 gateOff = cameraView(cam, kNoOrigin);
		INFO("mode=" << (fps ? "fps" : "trackball"));
		// 塌到 0（−0.0 == 0.0 在 IEEE 下成立，判据取「恰为零」而非「很小」）
		CHECK(translationCol(closed)[0] == 0.0f);
		CHECK(translationCol(closed)[1] == 0.0f);
		CHECK(translationCol(closed)[2] == 0.0f);
		// 门关对照：同机位不减原点，平移列就是 1e6 量级（区分力锚——若哪天 |c3| 恒 0，
		// 上面三条会因「实现退化」而假绿，这条把它钉死）
		CHECK(glm::length(translationCol(gateOff)) > 1.0e6);
	}
}

TEST_CASE("🔴 顺序承重（「先收窄后减」的变异本体）：origin 离相机有 sub-ULP 差 ⟹ view 逐位 = 先减后收窄闭式") {
	// 上面那条抓不到本变异：origin = camPos 时 float(x)−float(x) 也恰为 0。
	// 这条把 origin 挪到**同量级不同点**，让两种顺序在 float32 里必然分家：
	//   先减后收窄（契约）：float(1e6+0.1 − 1e6) = float(0.1) ≈ 0.100000001
	//   先收窄后减（变异）：float(1e6+0.1) − 1e6 = 1000000.125 − 1000000 = 0.125
	// （ULP(1e6) = 0.0625、ULP(2e6/3e6) = 0.25 ⟹ 0.1/0.3/0.7 都不是格点倍数，手算闭式。）
	const glm::dvec3 origin{1.0e6, -2.0e6, 3.0e6};
	const glm::dvec3 camPos = origin + glm::dvec3{0.1, 0.3, 0.7};
	const glm::dvec3 target = camPos + glm::dvec3{2.0, -1.0, 4.0};

	// 区分力守卫（判据自己的判据）：本输入下两种顺序**必须**不同，否则这条锚点
	// 退化成自比恒等——上一批实测过「注入量无区分力」的坑，这里结构性地防住。
	CHECK(glm::vec3{camPos - origin} != glm::vec3{camPos} - glm::vec3{origin});
	CHECK(glm::vec3{target - origin} != glm::vec3{target} - glm::vec3{origin});

	// fps：期望 = 契约文字直接写成裸算式（double 减 → 收窄 → lookAt），不抄被测的矩阵委托
	const Camera fps = makeFps(camPos, 0.4f, -0.2f);
	const glm::vec3 eyeRel{camPos - origin};
	checkBitwise(cameraView(fps, origin),
		glm::lookAt(eyeRel, eyeRel + cameraForward(fps), cameraUp(fps)));
	// trackball：eye 与 target 各自先减后收窄
	const Camera tb = makeTrackball(camPos, target);
	checkBitwise(cameraView(tb, origin),
		glm::lookAt(eyeRel, glm::vec3{target - origin}, tb.up));
	// 量级塌缩（0.77 而非 3.7e6）——「远小于门关」的定量半边
	CHECK(glm::length(translationCol(cameraView(tb, origin))) < 1.0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 三出口同空间 + 不漏改（半接状态 = 本刀要防的形态）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 三出口同空间：eyePos / GpuLight::position / view 都等于「绝对 − 同一个原点」（闭式逐位）") {
	// 三个出口吃**同一个** origin 值：view 由 cameraView 减、eyePos 与灯位置由 fillLightData
	// 减同一个入参（生产调用点传的都是 projectionSystem 本帧写的 RenderOrigin::position，
	// 帧序 camera→origin→…→projection→taa→renderlist 保证同帧同值）。
	// 跨出口判据（不依赖具体原点值）：eyePos 与 view 同空间 ⟺ view·(eyePos,1) = (0,0,0,1)。
	const glm::dvec3 origin{1000.5, -2000.25, 4000.125};
	const glm::dvec3 camPos = origin + glm::dvec3{0.1, 0.2, 0.3};
	const glm::dvec3 lightPos = origin + glm::dvec3{1.5, -2.5, 3.5};

	entt::registry rg;
	Light L;
	L.index = 0;
	L.position = lightPos;
	rg.emplace<Light>(rg.create(), L);
	FrameDrawList list;
	fillLightData(rg, camPos, origin, list);

	// 出口 2：eyePos 闭式 = float(camPos − origin)（先减后收窄，逐位）
	checkBitwise(list.sceneData.light.eyePos, glm::vec3{camPos - origin});
	// 出口 3：灯位置闭式 = float(L.position − origin)（float 契约，w = 0 不动）
	CHECK(list.sceneData.lightArray[0].position
		== glm::vec4(glm::vec3{lightPos - origin}, 0.0f));
	// 出口 1：view 闭式（同 origin、同 fps 相机）
	const Camera fps = makeFps(camPos, 1.1f, 0.25f);
	const glm::vec3 eyeRel{camPos - origin};
	checkBitwise(cameraView(fps, origin),
		glm::lookAt(eyeRel, eyeRel + cameraForward(fps), cameraUp(fps)));

	// 跨出口：眼点被自己的 view 映到原点（eyePos 若漏接 = 绝对坐标，这个量是 1e3 量级）
	const glm::vec4 q = cameraView(fps, origin) * glm::vec4(list.sceneData.light.eyePos, 1.0f);
	for (int k = 0; k < 3; ++k)
		testing::expectNear(q[k], 0.0, 0.0, 1e-3);
	testing::expectNear(q.w, 1.0, 1e-5, 1e-6);
	// 阳性对照：同一个 view 乘**绝对**相机位必须**不**是原点（否则上面三条是空判据）
	const glm::vec4 qAbs = cameraView(fps, origin) * glm::vec4(glm::vec3{camPos}, 1.0f);
	CHECK(glm::length(glm::vec3{qAbs}) > 1.0e2);
}

TEST_CASE("🔴 不许漏改：origin ≠ 0 时三处**都**必须变（漏一处 = 半接状态，正是本刀要防的）") {
	// 与上一条分工：上一条验「接得对不对」，这一条验「接没接」。三处逐一对比
	// origin = 0 与 origin ≠ 0 两次落库，任何一处纹丝不动 = 该出口没吃到原点。
	const glm::dvec3 origin{1000.0, 0.0, -1000.0};
	const glm::dvec3 camPos{1.0e6 + 0.1, 5.0, -3.0};
	const glm::dvec3 lightPos{2.0e6 + 0.3, 1.0, 1.0};

	entt::registry rg;
	Light L;
	L.index = 0;
	L.position = lightPos;
	rg.emplace<Light>(rg.create(), L);

	const Camera fps = makeFps(camPos, 0.3f, 0.1f);
	const glm::mat4 view0 = cameraView(fps, kNoOrigin);
	const glm::mat4 view1 = cameraView(fps, origin);
	bool viewMoved = false;
	for (int c = 0; c < 4 && !viewMoved; ++c)
		for (int r = 0; r < 4; ++r)
			if (view0[c][r] != view1[c][r])
				viewMoved = true;
	CHECK(viewMoved);

	FrameDrawList a, b;
	fillLightData(rg, camPos, kNoOrigin, a);
	fillLightData(rg, camPos, origin, b);
	CHECK(a.sceneData.light.eyePos != b.sceneData.light.eyePos);
	CHECK(a.sceneData.lightArray[0].position != b.sceneData.lightArray[0].position);
	// 且变化量 = 原点的收窄像（不是「随便动了动」）：eyePos 差 ≈ origin 在 float 的舍入界内
	for (int k = 0; k < 3; ++k)
		testing::expectNear(
			static_cast<double>(b.sceneData.light.eyePos[k]) - static_cast<double>(a.sceneData.light.eyePos[k]),
			-origin[k], 0.0, 1.0);  // |camPos| ~1e6 处 float ULP = 0.0625，容差 1.0 是它的 16 倍
	// 方向锚：eyePos 变小（减掉 +1000 的 x 分量），不是加号错成减号
	CHECK(b.sceneData.light.eyePos.x < a.sceneData.light.eyePos.x);
}

// ═══════════════════════════════════════════════════════════════════════════
// 接线判据（cameraView 只保证「给了原点就减对」，不保证调用点真传了）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("🔴 projectionSystem 接线：view 吃**本帧刚生产的** RenderOrigin；near/far 不吃") {
	// 「接了但没接上」的单元级判据：调用点把 origin 实参改成 dvec3{0}（编译照过、
	// 门关照绿、门开静默错位）只有这条红。反向的「过度修改」也钉：near/far 的距离
	// 是两绝对量之差（平移不变），任务书「明确不许改」表点名——若有人顺手给它减原点，
	// near/far 不变性断言红。
	//
	// 🔴 **批 5 第 1 刀改了本用例门开那一半的形态**（原写法是「外面铺一个
	// origin.position = camera.position，再看 view 吃没吃」）。原点现在由本 system
	// **自己生产**（粒度 ← near/far ← 本 system），外面铺的值会被覆盖 —— 于是判据换成
	// 「门开态生产出的原点必须非零、且 view 的平移列跟着塌到相对量级」。原本要抓的失效
	// 形态（调用点传 dvec3{0}）在这里照样红：那 ⟹ 平移列留在 1e6 量级。
	entt::registry rg;
	auto& extent = rg.ctx().emplace<RenderExtent>();
	extent.width = 1280;
	extent.height = 720;
	auto& camera = rg.ctx().emplace<Camera>();
	camera.position = glm::dvec3{1.0e6 + 0.1, -2.0e6 + 0.3, 3.0e6 + 0.7};
	camera.mode = CameraMode::Fps;
	auto& bounds = rg.ctx().emplace<SceneBounds>();
	bounds.center = glm::vec3{0.0f, 0.0f, 0.0f};
	bounds.radius = 50.0f;  // 非空场景 = 自适应 near/far 分支生效
	auto& viewProj = rg.ctx().emplace<ViewProjection>();
	auto& origin = rg.ctx().emplace<RenderOrigin>();

	// 门关：view 逐位 = 改前原式（接线层的门关锚）
	origin.enabled = false;
	origin.position = kNoOrigin;
	projectionSystem(rg);
	const glm::mat4 viewGateOff = viewProj.view;
	const glm::vec3 eyeF{camera.position};
	checkBitwise(viewGateOff,
		glm::lookAt(eyeF, eyeF + cameraForward(camera), cameraUp(camera)));
	const float near0 = viewProj.nearPlane;
	const float far0 = viewProj.farPlane;
	CHECK(near0 > 0.0f);
	CHECK(far0 > near0);
	CHECK(origin.position == kNoOrigin);  // 门关：生产者也把原点钉回逐位零

	// 门开：本 system 生产原点（非零 = 接上了），view 的平移列跟着塌到相对量级
	origin.enabled = true;
	projectionSystem(rg);
	const double originNorm = glm::length(origin.position);
	const double cameraNorm = glm::length(camera.position);
	MESSAGE("【读数】门开接线：|origin| = " << originNorm << "，|camera| = " << cameraNorm
	          << "，view 平移列 = " << translationCol(viewProj.view)[0] << ","
	          << translationCol(viewProj.view)[1] << "," << translationCol(viewProj.view)[2]);
	CHECK(originNorm > 1.0e5);  // 生产出的原点跟相机同量级（恒 0 = 没接上）
	// 平移列量级 = |eye|（(s,u,f) 正交归一）。门关态它是 3.7e6 量级；门开必须塌到
	// 「相机离原点多远」量级 = 半格 × √3。⚠ **不许把上界写死成 1.0**：本用例的场景是
	// 「相机离场景球心 3.7e6 而球半径只有 50」，near/far 挤在同一档 ⟹ 级联纹素被抬到
	// 8192 m（实测 G），半格 × √3 ≈ 7e3。判据取**比值**：塌了两个数量级以上才算接上，
	// 而「origin 传 dvec3{0}」那个要抓的变异给出比值 = 1，立刻红。
	const double translationNorm = glm::length(translationCol(viewProj.view));
	MESSAGE("【读数】view 平移列量级 = " << translationNorm << "，|camera| = " << cameraNorm
	          << "，比值 = " << translationNorm / cameraNorm);
	CHECK(translationNorm < cameraNorm * 0.01);
	// near/far **不许**跟着原点走（两项皆绝对量，差平移不变）：逐位不变
	CHECK(viewProj.nearPlane == near0);
	CHECK(viewProj.farPlane == far0);

	// 幂等回退：门关再跑一次逐位回到门关那份（门关态生产者每帧钉回 0 的形态）
	origin.enabled = false;
	projectionSystem(rg);
	checkBitwise(viewProj.view, viewGateOff);
	CHECK(isBitwiseZero(origin.position));
}

}  // namespace RGL
