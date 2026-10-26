module;
#include <doctest/doctest.h>
#include <entt/entt.hpp>     // 栈上 registry 驱动 cameraSystem / projectionSystem（先例 test_render_origin）
#include <SDL3/SDL_mouse.h>  // SDL_BUTTON_LEFT（cameraSystem 的 dragging 判据用它，不靠抄数值 1）
#include <glm/glm.hpp>
#include <cmath>         // std::sqrt / std::asin / std::atan2 / std::fabs / std::round / std::isfinite
#include <cstdio>        // std::snprintf（lua 字面量 %.20g）
#include <cstring>       // std::memcmp（modules 下自由函数不从 import 带来）
#include <filesystem>    // 临时 lua 落盘（AGENTS GMF 例外清单成员）
#include <fstream>       // std::ofstream
#include <initializer_list>

// 债-151 的守卫：绝对层量**不许先收窄成 float32 再相减**（一族，不是一处）。
//
// ## 被测缺陷（台账 债-151，2026-10-26 修）
//
// `glm::vec3(a) - glm::vec3(b)`（a、b 都是绝对层 dvec3）与 `glm::dvec3(glm::vec3(t) + offset)`
// 是**两个不同方向**的错，各要一条独立判据：
//  ① 相减侧：把**差向量**量化到 float32 网格（|p| = 1e6 处 ULP = 0.0625 m），而差向量本身只有
//     几米 ⟹ 视向偏 0.20°、1600×900 画面整体平移 ~5 px（门关门开同吃，收窄在配置读取期）。
//  ② 落库侧：把 **float 舍过的位置写回 double 字段** ⟹ 每拖一次位置被永久量化一次，且累积。
//     这条比 ① 更隐蔽：没有任何一帧"看起来"出错。
//
// ## 为什么本文件的判据能是「逐位」而不是「容差」
//
// 被测不变量是**平移不变性**：(eye, target) 一起平移 Δ，视向不该有任何变化。要让"不该有任何变化"
// 逐位成立，四个数 eye、target、eye+Δ、target+Δ 必须**在 double 里都精确可表示**
// ⟹ 分量一律取 **2^-20 的整数倍**（|分量| < 4、|Δ| < 2^22；double 在 2^22 量级的 ULP = 2^-30
// ≤ 2^-20 ⟹ 精确）。此时差向量在平移两侧**逐位相同**（|差| ~1、粒度 2^-20 ⟹ 22 bit ≪ 53），
// 于是"逐位相同"是**可成立的**，不需要容差。
// 选 2^-20 还有一层：float32 在 1e6 处的 ULP 是 2^-4，2^-20 远小于它 ⟹ **先收窄再相减一定把它
// 抹掉**（阳性锚点因此成立）；而 float32 在分量自身量级（~1）处又能精确表示 2^-20 ⟹ 不平移
// 那一侧的基准值是干净的。
// ⚠ 反过来说：**任意十进制机位（出厂的 1.1 / 0.14）在 Δ = 1e6 下做不到逐位不变**——
//   fl(p + 1e6) 的表示误差（≤ 5.8e-11）会原样进差向量，那是"输入的误差"，double 域也消不掉。
//   那种机位的判据一律走闭式对账（组 1-C、组 4-B），别对十进制机位写逐位断言。
//
// ## 阳性锚点（rgl-test-discipline：先问"代码从未生效时这条会绿吗"）
//
// 每条逐位不变断言都配一条**独立算的**锚点：「先收窄再相减」这个被禁止的形态在本组输入上
// 平移前后**必须改变差向量**（逐分量 memcmp）。锚点不成立 = 输入对该缺陷不敏感 = 断言是摆设，
// 当场红并点名"输入选错了"。锚点用 glm 的 float 运算在测试里重算，不读被测码。
// 另配"分辨力"锚点（组 1-C / 4-B）：钉**先收窄形态与闭式解之差远超本用例的界**——界松到连
// 错误形态都过不了关，才轮到正确形态的通过算数。
//
// ## 变异红线声明（哪个变异让我红；主 agent 的质检变异形态可与本处不同）
//
//  M1 `cameraAimAt` 的减法改回两侧先收窄：
//     `const glm::dvec3 fwd = glm::dvec3(glm::vec3(target)) - glm::dvec3(glm::vec3(camera.position));`
//     → 组 1-A + 组 1-B + 组 1-C + 组 1-E **四条独立红**（1-A/1-B 逐位、1-C 闭式、1-E 随机）
//  M2 **只收窄一侧**（弱化变异，"本地正确的另一半"）：
//     `const glm::dvec3 fwd = target - glm::dvec3(glm::vec3(camera.position));`
//     → 实测红 **1-A + 1-B + 1-C + 1-E**（四条，与 M1 同宽）⟹ 断言钉的是"两侧同域"，
//       不是"至少一侧在 double"
//  M3 `applyTrackballDrag` 落库改回先收窄后加：`camera.position = glm::dvec3(glm::vec3(camera.target) + offset);`
//     → 实测红 **2-A + 2-B + 2-C**
//  M4 `applyTrackballDrag` 的相减改回先收窄、**落库保持 double**：
//     `const glm::dvec3 offsetAbs = glm::dvec3(glm::vec3(camera.position)) - glm::dvec3(glm::vec3(camera.target));`
//     → 实测红 **2-B + 2-C，而 2-A 不红**（单次拖动的 offset 恰为 (0,0,1)，先收窄不丢东西）
//     🔴 **2-A 与 M4 的这一格之差就是"两半各自独立承重"的证明**：
//        落库半边（②）只有 2-A 抓得到（M4 抓不到它）；相减半边（①）由 2-B/2-C 抓着
//        （2-B 抓的是"累积"：M4 下第二次拖动起 offset 被重新收窄 ⟹ 位置再也动不了）。
//        摘掉任何一条判据，就有一类缺陷静默通过。
//  M5 `applyMovement` 的 trackball 缩放落库改回先收窄后加：
//     `camera.position = glm::dvec3(glm::vec3(camera.target) + radius * (1.0f - clamped * camera.zoomSpeed));`
//     → 组 3-A 红
//  M6 `projectionSystem` 的 dist 改回 `glm::length(glm::vec3(camera.position) - sceneBounds.center)`
//     → 组 4-A（逐位平移不变）+ 组 4-B（闭式单重舍入）**两条独立红**
//  M7 `cameraAimAt` 的退化守卫从 `<= 1e-6` 改成 `<= 0.0`（恒不触发）
//     → 组 1-D 红（反向半边：退化时朝向必须逐位不动）
//  ⚠ 答不出红哪条的断言不配进门；本文件每条断言都在上面列出的变异名下有归属。
export module test_camera_absolute_domain;

import rlstd;
import test_support;
import camera;         // Camera / cameraAimAt / cameraSystem / cameraUp / InputEvents
import camera_config;  // loadCameraConfig（readCameraPlacement 的唯一公开入口）
import scene_context;  // SceneBounds / ViewProjection / RenderOrigin
import ecs;            // projectionSystem（near/far 的生产者）/ RenderExtent
import vkerror;

namespace RGL {
namespace {

using testing::Lcg;

/** 本文件唯一写死种子（rgl-test-discipline §确定性）。 */
constexpr uint32_t kSeed = 20261026u;

/**
 * 平移量：三分量都 < 2^22（double 在该量级 ULP = 2^-30），且都是 float32 精确整数
 * （< 2^24）——后者让「Δ 自己引入误差」这个混淆项从判据里消失。
 */
constexpr glm::dvec3 kDelta{1e6, 2e6, 3e6};

/** 机位分量的最小粒度 2^-20（理由见文件头「为什么能逐位」；写成精确十进制）。 */
constexpr double kTick = 1.0 / 1048576.0;

/** 由 2^-20 的整数倍铺一个机位分量（保证在 |x|+|Δ| < 2^22 内 double 精确可表示）。 */
constexpr glm::dvec3 tickPose(double mx, double my, double mz) {
	return glm::dvec3{mx * kTick, my * kTick, mz * kTick};
}

/** 逐位相同（memcmp：+0.0 与 −0.0 判不同——「逐位」两字不许读成「值相等」）。 */
bool bitwiseEqual(const glm::dvec3& a, const glm::dvec3& b) {
	return std::memcmp(&a, &b, sizeof(glm::dvec3)) == 0;
}
bool bitwiseEqual(const glm::vec3& a, const glm::vec3& b) {
	return std::memcmp(&a, &b, sizeof(glm::vec3)) == 0;
}
bool bitwiseEqual(float a, float b) {
	return std::memcmp(&a, &b, sizeof(float)) == 0;
}

/**
 * 阳性锚点（独立于被测码）：断言「两个绝对层量各自先收窄、再相减」这个**被禁止的形态**在本组
 * 输入上确实会因平移而改变结果。锚点不成立 ⟹ 后面的逐位断言是恒真的摆设，当场红。
 * @return 三个分量里「先收窄再相减」在平移前后**不同**的个数（0 = 输入不敏感）
 */
int narrowThenSubtractSensitivity(const glm::dvec3& eye, const glm::dvec3& target,
	const glm::dvec3& delta) {
	const glm::vec3 base = glm::vec3(target) - glm::vec3(eye);
	const glm::vec3 moved = glm::vec3(target + delta) - glm::vec3(eye + delta);
	int differs = 0;
	for (int k = 0; k < 3; ++k)
		if (!bitwiseEqual(base[k], moved[k]))
			++differs;
	return differs;
}

/** 临时 lua 配置（loader 只吃路径；构造写盘、析构删除）。 */
class TempLua {
  public:
	TempLua(std::string_view tag, std::string_view body)
	: path_(std::filesystem::temp_directory_path() / ("rgl_cfg_" + std::string(tag) + ".lua")) {
		std::ofstream out(path_);
		out << body;
	}
	TempLua(const TempLua&) = delete;
	TempLua& operator=(const TempLua&) = delete;
	~TempLua() {
		std::error_code ec;
		std::filesystem::remove(path_, ec);
	}
	/** 传给 loader 的路径字符串（生命周期随本对象）。 */
	std::string pathStr() const { return path_.string(); }

  private:
	std::filesystem::path path_;
};

/** dvec3 → Lua 表字面量（%.20g 保证 double 逐位往返）。 */
std::string luaVec(const glm::dvec3& v) {
	char buf[192];
	std::snprintf(buf, sizeof(buf), "{ %.20g, %.20g, %.20g }", v.x, v.y, v.z);
	return buf;
}

/** 一次驱动 projectionSystem 的最小 registry（装配面同 tests/test_render_origin.cppm 的 Rig）。 */
struct ProjRig {
	entt::registry rg;
	RenderExtent& extent;
	Camera& camera;
	SceneBounds& bounds;
	ViewProjection& viewProj;
	RenderOrigin& origin;

	ProjRig()
	: extent(rg.ctx().emplace<RenderExtent>()), camera(rg.ctx().emplace<Camera>()),
	  bounds(rg.ctx().emplace<SceneBounds>()), viewProj(rg.ctx().emplace<ViewProjection>()),
	  origin(rg.ctx().emplace<RenderOrigin>()) {
		extent.width = 1280u;
		extent.height = 720u;
	}
};

/** 一次驱动 cameraSystem 的最小 registry（Camera / CameraState / InputEvents 三件）。 */
struct DragRig {
	entt::registry rg;
	Camera& camera;
	CameraState& state;
	InputEvents& events;

	DragRig()
	: camera(rg.ctx().emplace<Camera>()), state(rg.ctx().emplace<CameraState>()),
	  events(rg.ctx().emplace<InputEvents>()) {}
};

/** 塞「左键按住 + 一次鼠标增量」进事件流，跑一帧 cameraSystem（trackball 拖拽入口）。 */
void dragOneFrame(DragRig& rig, float dx, float dy) {
	rig.events.events.push_back(InputEvent{
		.type = InputEvent::Type::MouseButton, .button = SDL_BUTTON_LEFT, .pressed = true});
	rig.events.events.push_back(
		InputEvent{.type = InputEvent::Type::MouseDelta, .delta = glm::vec2{dx, dy}});
	cameraSystem(rig.rg, 0.016f);
}

/** 塞「滚轮一格」进事件流并跑一帧（左键不按：只走 applyMovement 的 trackball 分支）。 */
void wheelOneFrame(DragRig& rig, float wheel) {
	rig.events.events.push_back(InputEvent{.type = InputEvent::Type::Wheel, .amount = wheel});
	cameraSystem(rig.rg, 0.016f);
}

/**
 * 闭式期望：由「eye → target」的差向量解出 (yaw, pitch)，**用 long double 独立实现**。
 * 出处 = cameraForward/cameraUp 的逆（forward = (−sin yaw·cos pitch, sin pitch,
 * −cos yaw·cos pitch) ⟹ pitch = asin(f.y)、yaw = atan2(−f.x, −f.z)），不是抄被测码：
 * 被测码走 double，本函数走 80-bit，两者只差一次舍入。
 */
void closedFormAim(const glm::dvec3& eye, const glm::dvec3& target, long double& yaw,
	long double& pitch) {
	const long double d[3] = {static_cast<long double>(target.x) - static_cast<long double>(eye.x),
		static_cast<long double>(target.y) - static_cast<long double>(eye.y),
		static_cast<long double>(target.z) - static_cast<long double>(eye.z)};
	const long double len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	const long double fx = d[0] / len, fy = d[1] / len, fz = d[2] / len;
	pitch = std::asin(fy);
	yaw = std::atan2(-fx, -fz);
}

/** 出厂机位量级的**十进制**姿态（组 1-C 用；逐位判据不许用它，理由见文件头）。 */
constexpr glm::dvec3 kFactoryEye{1.1, 0.75, 1.05};
constexpr glm::dvec3 kFactoryTarget{0.14, 0.12, 0.02};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：视向对「相机与目标一起平移」不变（债-151 的直接病灶）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("组 1-A cameraAimAt：(eye,target) 一起平移 1e6 量级 ⟹ yaw/pitch/up 逐位不变（M1/M2）") {
	// 出厂交付机位（configs/camera.lua）的 2^-20 量化副本：eye≈(1.1,0.80,1.05)、
	// target≈(0.14,0.12,0.02)、视距 |target−eye| ≈ 1.54 m（与实机同量级）。
	// ⚠ 分量必须**全部带 2^-20 的尾巴**：某个分量若恰好是 float32 在其量级处的精确值
	//（如 0.75 = 6·0.125），「先收窄再相减」在**该分量上**就不丢东西，阳性锚点会数不满 3。
	const glm::dvec3 eye = tickPose(1153434, 838861, 1101005);
	const glm::dvec3 target = tickPose(146801, 125829, 20972);

	// 阳性锚点：先证明这组输入对「先收窄再相减」是敏感的（不敏感 = 本用例是摆设）
	CHECK_MESSAGE(narrowThenSubtractSensitivity(eye, target, kDelta) == 3,
		"输入选错了：先收窄再相减在这组机位上平移前后不改变差向量，逐位断言恒真");

	Camera base;
	base.mode = CameraMode::Trackball;
	base.position = eye;
	cameraAimAt(base, target);
	// 阳性锚点：基准姿态本身不许是退化值（否则「两侧都等于同一个默认值」也能通过不变性）
	CHECK(std::isfinite(base.yaw));
	CHECK(std::isfinite(base.pitch));
	// 基准姿态非退化：两侧都恰为同一个默认值的「不变」没有分辨力
	CHECK(base.yaw != 0.0f);

	Camera moved;
	moved.mode = CameraMode::Trackball;
	moved.position = eye + kDelta;
	cameraAimAt(moved, target + kDelta);

	CHECK_MESSAGE(bitwiseEqual(moved.yaw, base.yaw),
		"yaw 随整机位平移变了（债-151 复发）：" << base.yaw << " -> " << moved.yaw);
	CHECK_MESSAGE(bitwiseEqual(moved.pitch, base.pitch),
		"pitch 随整机位平移变了（债-151 复发）：" << base.pitch << " -> " << moved.pitch);
	// up 由 yaw/pitch 派生（readCameraPlacement 用它做 world-up 对齐、trackball 的 lookAt 吃它）
	CHECK(bitwiseEqual(cameraUp(moved), cameraUp(base)));
	// target 字段本身也要落对（绝对层 ↔ 绝对层，不经 float）
	CHECK(bitwiseEqual(moved.target, target + kDelta));
}

TEST_CASE("组 1-B 配置路径：camera.lua 的 position/target 一起平移 ⟹ 朝向逐位不变（M1）") {
	// readCameraPlacement 是债-151 的**实测病灶**（收窄发生在配置读取期，门关门开同吃）。
	// 它现在委托 cameraAimAt；本组把"委托"钉成可执行判据：走公开入口 loadCameraConfig 拿到的
	// 朝向，必须与组 1-A 同样平移不变、且与直调 cameraAimAt 逐位相同。
	const glm::dvec3 eye = tickPose(1153434, 838861, 1101005);
	const glm::dvec3 target = tickPose(146801, 125829, 20972);
	CHECK(narrowThenSubtractSensitivity(eye, target, kDelta) == 3);

	const std::string body = "return { mode = \"trackball\", position = " + luaVec(eye) +
		", target = " + luaVec(target) + " }\n";
	const std::string bodyMoved = "return { mode = \"trackball\", position = " +
		luaVec(eye + kDelta) + ", target = " + luaVec(target + kDelta) + " }\n";
	TempLua f("cad_base", body);
	TempLua g("cad_moved", bodyMoved);
	const auto a = loadCameraConfig(f.pathStr());
	const auto b = loadCameraConfig(g.pathStr());
	REQUIRE(a.has_value());
	REQUIRE(b.has_value());

	// 阳性锚点：配置真的被解析了（否则是两份默认值互比，恒真）
	CHECK(a->camera.position != glm::dvec3{0.0, 0.0, 3.0});
	CHECK(a->camera.target != glm::dvec3{0.0, 0.0, 0.0});
	CHECK(std::isfinite(a->camera.yaw));
	CHECK(std::isfinite(a->camera.pitch));

	CHECK_MESSAGE(bitwiseEqual(b->camera.yaw, a->camera.yaw), "配置路径的 yaw 随平移变了");
	CHECK_MESSAGE(bitwiseEqual(b->camera.pitch, a->camera.pitch), "配置路径的 pitch 随平移变了");
	CHECK(bitwiseEqual(b->camera.up, a->camera.up));
	// 与直调 cameraAimAt 同约定（"两条路径反推结果相同"从注释变成判据）
	Camera direct;
	direct.position = eye;
	cameraAimAt(direct, target);
	CHECK(bitwiseEqual(a->camera.yaw, direct.yaw));
	CHECK(bitwiseEqual(a->camera.pitch, direct.pitch));
}

TEST_CASE("组 1-C 现实十进制机位：视向与 long double 闭式解之差 ≪ 先收窄形态的误差（M1）") {
	// 出厂机位是十进制（1.1 / 0.14），在 Δ = 1e6 下**做不到**逐位不变（文件头）。所以这条走
	// 闭式对账：与 long double 独立解之比。
	// 界 1e-7 rad 的出处：yaw/pitch 是 **float 字段**，一次 float 舍入 ≤ 半 ULP = 3e-8
	//（ULP(0.78) = 5.96e-8）——界必须容得下它，否则测的是"角度量住 float"而不是本债。
	// 而先收窄形态的误差 = float32 在 1e6 处的半格 0.03125 m ÷ 视距 1.54 m ≈ 2e-2 rad，
	// 在界之上 **5 个数量级** ⟹ 界两侧都留有余量（不是"调到通过"）。
	// ⚠ 姿态三分量都不许取成 float32 在其量级处的精确值（如 −3.25 / 1.5），否则「先收窄」
	//   那一侧不产生误差，下面的分辨力锚点会红（实测踩过：dyadic 的"反向取景"锚点 5e-20）。
	const glm::dvec3 poses[][2] = {
		{kFactoryEye, kFactoryTarget},                       // 出厂交付机位
		{kFactoryEye, glm::dvec3{0.0, 0.0, 0.0}},            // target 落在世界原点
		{glm::dvec3{-3.25, 1.5, 4.7}, glm::dvec3{0.5, -0.25, -1.53}},  // 反向取景
	};
	for (const auto& pose : poses) {
		for (const glm::dvec3& shift : {glm::dvec3{0.0}, kDelta}) {
			Camera cam;
			cam.position = pose[0] + shift;
			cameraAimAt(cam, pose[1] + shift);
			long double yawWant, pitchWant;
			closedFormAim(pose[0] + shift, pose[1] + shift, yawWant, pitchWant);
			const long double dyaw = std::fabs(static_cast<long double>(cam.yaw) - yawWant);
			const long double dpitch = std::fabs(static_cast<long double>(cam.pitch) - pitchWant);
			INFO("eye=(" << pose[0].x << "," << pose[0].y << "," << pose[0].z
						<< ") shift=(" << shift.x << "," << shift.y << "," << shift.z << ")");
			CHECK(static_cast<double>(dyaw) < 1e-7);
			CHECK(static_cast<double>(dpitch) < 1e-7);
			if (shift.x == 0.0)
				continue;
			// 分辨力锚点（与被测码无关）：**先收窄再相减**的形态在同一输入上必须超界 1e-3 rad。
			// 这条钉的是"这个界真的抓得住该抓的错"——否则上面的通过什么也没证明。
			const glm::vec3 narrow = glm::vec3(pose[1] + shift) - glm::vec3(pose[0] + shift);
			const float len = glm::length(narrow);
			REQUIRE(len > 0.0f);
			const long double narrowYaw = std::atan2(
				-static_cast<long double>(narrow.x) / static_cast<long double>(len),
				-static_cast<long double>(narrow.z) / static_cast<long double>(len));
			CHECK_MESSAGE(std::fabs(narrowYaw - yawWant) > 1e-3,
				"分辨力锚点失效：先收窄形态在本组输入上竟然不超界，说明界太松");
		}
	}
}

TEST_CASE("组 1-D 退化守卫反向半边：target≈position 时朝向逐位不动（M7）") {
	// 守卫分支两侧都要有用例，否则区分不了"没触发"和"坏掉了"（rgl-test-discipline）。
	Camera cam;
	cam.mode = CameraMode::Fps;
	cam.position = kDelta + glm::dvec3{1.0, 2.0, 3.0};
	cam.yaw = 0.7f;
	cam.pitch = -0.3f;
	const float yaw0 = cam.yaw, pitch0 = cam.pitch;
	// 锚点：铺一个"守卫若失效就必然算出别的值"的旧朝向，铺完立刻回读逐字节确认在场
	CHECK(cam.yaw == 0.7f);
	CHECK(cam.pitch == -0.3f);
	cameraAimAt(cam, cam.position + glm::dvec3{1e-7, 0.0, 0.0});  // 间距 1e-7 < 1e-6 界
	CHECK(cam.yaw == yaw0);
	CHECK(cam.pitch == pitch0);
	// 反向对照：同样起点、正常间距 ⟹ 朝向必须变（守卫不是恒真早退）
	cameraAimAt(cam, cam.position + glm::dvec3{1.0, 0.0, 0.0});
	CHECK(cam.yaw != yaw0);
}

TEST_CASE("组 1-E 随机 dyadic 机位：平移不变逐位成立 + 敏感度计数（M1）") {
	// 随机性 = Lcg + 写死种子（rgl-test-discipline §确定性）。机位取 2^-20 整数倍的理由见文件头。
	Lcg rng{kSeed};
	int tested = 0, sensitive = 0;
	auto snap = [](double v) { return std::round(v / kTick) * kTick; };
	for (int i = 0; i < 256; ++i) {
		const glm::dvec3 eye{snap(rng.uniform(-5.0, 5.0)), snap(rng.uniform(-5.0, 5.0)),
			snap(rng.uniform(-5.0, 5.0))};
		const glm::dvec3 target{snap(rng.uniform(-5.0, 5.0)), snap(rng.uniform(-5.0, 5.0)),
			snap(rng.uniform(-5.0, 5.0))};
		if (glm::length(target - eye) < 0.5)
			continue;  // 近退化机位不是本组形状（视向对它本身病态）
		// 锚点先回读自证：不敏感的输入直接跳过——把它们计入分母就是稀释判据
		if (narrowThenSubtractSensitivity(eye, target, kDelta) == 0)
			continue;
		++sensitive;
		Camera a, b;
		a.position = eye;
		b.position = eye + kDelta;
		cameraAimAt(a, target);
		cameraAimAt(b, target + kDelta);
		INFO("i=" << i << " eye=(" << eye.x << "," << eye.y << "," << eye.z << ") target=("
					<< target.x << "," << target.y << "," << target.z << ")");
		CHECK(bitwiseEqual(b.yaw, a.yaw));
		CHECK(bitwiseEqual(b.pitch, a.pitch));
		++tested;
	}
	// 对空集合的断言要加前提守卫：先钉"真的测到了东西"
	CHECK_MESSAGE(tested >= 100, "随机机位几乎全被跳过 = 本组退化成恒真（tested=" << tested << "）");
	CHECK(sensitive == tested);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：trackball 拖动（① 相减侧 与 ② 落库侧 各自独立承重）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("组 2-A 拖一次：位置动 1e-4 m（比 float32 半格小 312 倍）且不再是 float 可表示值（M3）") {
	// 闭式期望：offset = position − target = (0,0,1)，绕世界 Y 转角 a = −dx·sensitivity
	//   = −0.02·0.005 = −1e-4 rad；rotateAroundAxis 给 offset′ = (sin a, 0, cos a)
	//   ⟹ position′ = target + (sin a, 0, cos a)，**x 位移 = −1e-4 m**。
	// 而 float32 在 |x| = 1e6 处的 ULP = 0.0625 m、半格 0.03125 m —— 期望位移比它小 312 倍：
	// 先收窄后相加的旧写法把它整个舍回 1e6（位移恰为 0），double 域相加才留得住。
	// 容差 1e-9 的出处：double 相加的舍入 ≤ 半 ULP(1e6) = 5.8e-11，float sin 的误差 ≤ 3.6e-12。
	DragRig rig;
	rig.camera.mode = CameraMode::Trackball;
	rig.camera.target = glm::dvec3{1e6, 0.0, 0.0};
	rig.camera.position = glm::dvec3{1e6, 0.0, 1.0};
	const glm::dvec3 before = rig.camera.position;
	// 阳性锚点：起点位置本身就是 float 可表示的（1e6 与 1 都是）⟹ "改后不再是 float 可表示值"
	// 这条判据有对照基准，不是靠运气落在网格外
	CHECK(bitwiseEqual(glm::dvec3(glm::vec3(before)), before));

	dragOneFrame(rig, 0.02f, 0.0f);

	const glm::dvec3 after = rig.camera.position;
	// ① 位置**动了**（旧写法：先收窄后相加 ⟹ 逐位等于原值，此处红）
	CHECK_MESSAGE(!bitwiseEqual(after, before),
		"拖动一帧相机位置逐位不变 = 位移被 float32 网格吃掉（债-151 的落库半边复发）");
	// ② 位移量对得上闭式
	CHECK_MESSAGE(std::fabs(after.x - 1e6 + 1e-4) < 1e-9,
		"x 位移 = " << (after.x - 1e6) << "，闭式期望 −1e-4");
	// ③ 落库结果**不是** float32 可表示的数（旧写法必然落在 0.0625 的网格上）
	CHECK_MESSAGE(!bitwiseEqual(glm::dvec3(glm::vec3(after)), after),
		"位置仍是 float32 可精确表示的值 = 被永久量化到 float 网格");
	// ④ 公转半径不变（旋转等距；float 域旋转的相对误差 ~1e-8）
	CHECK(std::fabs(glm::length(after - rig.camera.target) - 1.0) < 1e-6);
	// ⑤ target 不许被拖动改动（公转中心是常量）
	CHECK(bitwiseEqual(rig.camera.target, glm::dvec3{1e6, 0.0, 0.0}));
}

TEST_CASE("组 2-B 连拖 4 次：每次都动——旧写法的量化是**累积**的，一次归零就永远不动（M3）") {
	DragRig rig;
	rig.camera.mode = CameraMode::Trackball;
	rig.camera.target = glm::dvec3{1e6, 0.0, 0.0};
	rig.camera.position = glm::dvec3{1e6, 0.0, 1.0};

	glm::dvec3 prev = rig.camera.position;
	int movedTimes = 0;
	for (int i = 0; i < 4; ++i) {
		dragOneFrame(rig, 0.02f, 0.0f);
		if (!bitwiseEqual(rig.camera.position, prev))
			++movedTimes;
		prev = rig.camera.position;
	}
	// 旧写法：第一次就把位置吸到 1e6 网格上，之后每帧的 offset 完全相同 ⟹ 一次都不动
	CHECK_MESSAGE(movedTimes == 4, "只有 " << movedTimes << "/4 次拖动真的改变了位置");
	// 闭式：每帧绕 Y 转 −1e-4 rad，4 帧后 x 位移 ≈ −4e-4（仍远小于 float32 半格 0.03125）
	CHECK(rig.camera.position.x < 1e6 - 3e-4);
	CHECK(rig.camera.position.x > 1e6 - 5e-4);
}

TEST_CASE("组 2-C 差向量本身也要在 double 域：公转半径逐位等于闭式（M4 的唯一判据）") {
	// 形状：offset 的三个分量各带一个 2^-20 的细节（float32 在 1e6 处存不下它）。
	// dx = dy = 0 让旋转退化成恒等 ⟹ 本用例只测「相减 → 落库」这条最短路径，
	// 与组 2-A/2-B（旋转非恒等）分开，M3 与 M4 才能各自独立红。
	const glm::dvec3 target = kDelta;
	const glm::dvec3 offset{1.0 + kTick, 0.5 + 2.0 * kTick, 2.0 + 3.0 * kTick};
	// 阳性锚点（本组形状专属，与组 1 的「平移敏感性」不同）：先收窄再相减会让差向量的
	// 2^-20 尾巴**整个消失**（float32 在 1e6 处只能表示 0.0625 的整数倍）。锚点不成立
	// = 这组 offset 先收窄也不丢东西 = 下面的闭式对账恒真。
	{
		const glm::vec3 narrowed = glm::vec3(target + offset) - glm::vec3(target);
		const glm::vec3 exactTail = glm::vec3(offset);
		int lost = 0;
		for (int k = 0; k < 3; ++k)
			if (!bitwiseEqual(narrowed[k], exactTail[k]))
				++lost;
		CHECK_MESSAGE(lost == 3, "offset 选错了：先收窄再相减没有抹掉它的尾巴，本用例恒真");
	}

	// 闭式期望：|offset| 用 long double 独立算（不读被测码）
	const long double ox = static_cast<long double>(offset.x);
	const long double oy = static_cast<long double>(offset.y);
	const long double oz = static_cast<long double>(offset.z);
	const double wantRadius = static_cast<double>(std::sqrt(ox * ox + oy * oy + oz * oz));

	DragRig rig;
	rig.camera.mode = CameraMode::Trackball;
	rig.camera.target = target;
	rig.camera.position = target + offset;
	dragOneFrame(rig, 0.0f, 0.0f);
	const double gotRadius = glm::length(rig.camera.position - rig.camera.target);
	// 容差 1e-12 的出处：double 域相减 + 落库相加的残差 ≤ 1e-16 相对；而先收窄形态丢掉整个
	// 2^-20 尾巴（半径相对误差 ~7e-7，绝对 1.5e-6）——界离两侧都远。
	CHECK_MESSAGE(std::fabs(gotRadius - wantRadius) < 1e-12,
		"半径 = " << gotRadius << "，闭式 " << wantRadius << "（差 " << (gotRadius - wantRadius) << "）");
}

TEST_CASE("组 2-D 退化守卫反向半边：|position−target| ≤ 1e-4 时位置逐位不动") {
	DragRig rig;
	rig.camera.mode = CameraMode::Trackball;
	rig.camera.target = glm::dvec3{1e6, 0.0, 0.0};
	rig.camera.position = glm::dvec3{1e6 + 5e-5, 0.0, 0.0};  // 长度 5e-5 ≤ 1e-4 界
	const glm::dvec3 before = rig.camera.position;
	dragOneFrame(rig, 50.0f, 50.0f);
	CHECK(bitwiseEqual(rig.camera.position, before));
	// 反向对照：刚过界（2e-4）就必须动 —— 守卫不是恒真早退
	rig.camera.position = glm::dvec3{1e6 + 2e-4, 0.0, 0.0};
	dragOneFrame(rig, 50.0f, 0.0f);
	CHECK(!bitwiseEqual(rig.camera.position, before));
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：滚轮缩放（同形态第三处：落库把 float 舍过的值写回 double 字段）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("组 3-A 滚轮缩放：半径缩 6e-4 m（< float32 半格）后位置仍非 float 可表示（M5）") {
	// 闭式期望：radius = position − target = (1,0,0)，缩放因子 = 1 − wheel·zoomSpeed
	//   = 1 − 0.02·0.03 = 1 − 6e-4 ⟹ position′ = target + (1 − 6e-4, 0, 0)，
	//   **x 位移 = −6e-4 m**，比 float32 在 1e6 处的半格 0.03125 小 52 倍。
	// 容差 1e-7 的出处：float 表示 0.9994 的误差 ≤ 半 ULP = 3e-8（ULP(0.9994) = 5.96e-8）。
	DragRig rig;
	rig.camera.mode = CameraMode::Trackball;
	rig.camera.target = glm::dvec3{1e6, 0.0, 0.0};
	rig.camera.position = glm::dvec3{1e6 + 1.0, 0.0, 0.0};
	const glm::dvec3 before = rig.camera.position;
	CHECK(bitwiseEqual(glm::dvec3(glm::vec3(before)), before));  // 锚点：起点是 float 可表示的

	wheelOneFrame(rig, 0.02f);

	const glm::dvec3 after = rig.camera.position;
	CHECK_MESSAGE(!bitwiseEqual(after, before), "滚轮缩放没改变位置 = 位移被 float32 吃掉");
	CHECK_MESSAGE(std::fabs(after.x - (1e6 + 1.0) + 6e-4) < 1e-7,
		"x 位移 = " << (after.x - (1e6 + 1.0)) << "，闭式期望 −6e-4");
	CHECK_MESSAGE(!bitwiseEqual(glm::dvec3(glm::vec3(after)), after),
		"位置仍是 float32 可精确表示的值 = 被永久量化到 float 网格");
	// 手感契约（相对缩放的语义不许被本刀改掉）：新半径 / 半径 = 1 − wheel·zoomSpeed
	const double ratio = glm::length(after - rig.camera.target);
	CHECK(std::fabs(ratio - (1.0 - 0.02 * 0.03)) < 1e-6);
}

TEST_CASE("组 3-B 滚轮 = 0 的反向半边：早退分支不动位置") {
	DragRig rig;
	rig.camera.mode = CameraMode::Trackball;
	rig.camera.target = glm::dvec3{1e6, 0.0, 0.0};
	rig.camera.position = glm::dvec3{1e6 + 1.0, 0.0, 0.0};
	const glm::dvec3 before = rig.camera.position;
	wheelOneFrame(rig, 0.0f);
	CHECK(bitwiseEqual(rig.camera.position, before));
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：near/far 的距离不再额外收窄（projectionSystem）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("组 4-A near/far 对「相机与包围球一起平移」逐位不变（M6）") {
	// center 住 float（Bounds 是 float 落库），所以两侧都取 **0.0625 的整数倍**：这样 center
	// 在 1e6 量级也精确可表示，残差只可能来自相机那一侧的收窄。机位仍取 2^-20 整数倍。
	const glm::dvec3 eye = tickPose(1153434, 838861, 1101005);
	const glm::vec3 center{0.125f, 0.5f, 0.25f};
	constexpr float kRadius = 0.5f;
	CHECK(narrowThenSubtractSensitivity(eye, glm::dvec3(center), kDelta) == 3);

	ProjRig a, b;
	a.camera.position = eye;
	a.bounds.center = center;
	a.bounds.radius = kRadius;
	projectionSystem(a.rg);

	b.camera.position = eye + kDelta;
	b.bounds.center = center + glm::vec3(1e6f, 2e6f, 3e6f);
	b.bounds.radius = kRadius;
	projectionSystem(b.rg);

	// 阳性锚点：真的走了自适应分支（radius>0 生效），否则 near/far 是配置常量、恒真通过
	CHECK(a.viewProj.farPlane != a.camera.farPlane);
	CHECK(a.viewProj.nearPlane != a.camera.nearPlane);
	CHECK(std::isfinite(a.viewProj.nearPlane));
	CHECK(std::isfinite(a.viewProj.farPlane));

	CHECK_MESSAGE(bitwiseEqual(b.viewProj.nearPlane, a.viewProj.nearPlane),
		"near 随整机位平移变了：" << a.viewProj.nearPlane << " -> " << b.viewProj.nearPlane);
	CHECK_MESSAGE(bitwiseEqual(b.viewProj.farPlane, a.viewProj.farPlane),
		"far 随整机位平移变了：" << a.viewProj.farPlane << " -> " << b.viewProj.farPlane);
}

TEST_CASE("组 4-B near/far 与闭式解之差 = 一次 float 舍入（十进制机位，M6）") {
	// 出厂 SceneBounds 的实测量级（2026-10-26 实机日志 center≈(0.1437,0.0856,0.0298)、
	// r≈1.1626），整机推到 1e6。闭式期望 = long double 的 |p − c| 再 ± r，**各只舍一次 float**。
	const glm::dvec3 eye{1000000.3, 0.75, 1.05};
	const glm::vec3 center{1000000.0f, 0.0f, 0.0f};
	constexpr float kRadius = 0.4f;

	ProjRig rig;
	rig.camera.position = eye;
	rig.bounds.center = center;
	rig.bounds.radius = kRadius;
	projectionSystem(rig.rg);

	const long double dx = static_cast<long double>(eye.x) - 1000000.0L;
	const long double dist = std::sqrt(dx * dx + 0.75L * 0.75L + 1.05L * 1.05L);
	// r 用**存储里那个 float**（0.4f ≠ 0.4，差 5.96e-9）——闭式对账要对的是同一个实数
	const long double rr = static_cast<long double>(kRadius);
	const double farWant = static_cast<double>(dist + rr);
	const double nearWant = static_cast<double>(dist - rr);
	// 界 = 2 个 float ULP（在**期望值**处取）。出处：本刀把距离改成「double 域算完、落库时
	// 舍一次 float」，所以总误差只可能是那一次舍入（≤ 半 ULP）加上 double 距离自身的
	// 1e-16 相对误差；2 ULP 留了 4 倍余量，而先收窄形态的误差是 0.0125 m ≈ 1e5 ULP。
	// ⚠ 这里**不逐位**：期望值走 long double，被测码的距离走 double，两者差 1e-16 就足以
	//   让结果落在 float 舍入边界两侧（实测 near 就差 1 ULP）。逐位判据住在组 4-A。
	// ⚠ ULP 必须按 **float** 取（把值转成 double 再 nextafter 会拿到 double 的 ULP，
	// 小 1e8 倍，界就成了一条永远不成立的假界）。
	const float farGot = rig.viewProj.farPlane;
	const float nearGot = rig.viewProj.nearPlane;
	const double farUlp = static_cast<double>(std::nextafter(farGot, 1e30f)) -
		static_cast<double>(farGot);
	const double nearUlp = static_cast<double>(std::nextafter(nearGot, 1e30f)) -
		static_cast<double>(nearGot);
	CHECK(farUlp > 0.0);   // 界自己要先响：取到 0 = 上面那行又写错了
	CHECK(nearUlp > 0.0);
	CHECK(std::fabs(static_cast<double>(farGot) - farWant) <= 2.0 * farUlp);
	CHECK(std::fabs(static_cast<double>(nearGot) - nearWant) <= 2.0 * nearUlp);
	// 分辨力锚点：先收窄形态在同一输入上把 1000000.3 舍成 1000000.3125（float 在 1e6 处
	// ULP = 0.0625）⟹ 距离多算 0.0125 m —— 比上面的界大 4 个数量级
	const float narrowDist = glm::length(glm::vec3(eye) - center);
	CHECK_MESSAGE(std::fabs(narrowDist + kRadius - farWant) > 1e-3,
		"分辨力锚点失效：先收窄形态与闭式几乎相同，本用例抓不到东西");
}

TEST_CASE("组 4-C 反向半边：radius = 0 时 near/far 用配置值（自适应分支不是恒开）") {
	ProjRig rig;
	rig.camera.position = kDelta;
	rig.bounds.center = glm::vec3{0.0f};
	rig.bounds.radius = 0.0f;
	rig.camera.nearPlane = 0.1f;
	rig.camera.farPlane = 10.0f;
	projectionSystem(rig.rg);
	CHECK(rig.viewProj.nearPlane == 0.1f);
	CHECK(rig.viewProj.farPlane == 10.0f);
}

}  // namespace RGL
