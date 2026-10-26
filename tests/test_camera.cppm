module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>          // glm::pi
#include <glm/gtc/matrix_transform.hpp>  // 测试侧构造 proj（camera 不导出 proj 构造，见组 6 注释）
#include <cmath>                         // std::isfinite（rlstd 未导出该数学名，GMF 直入）

// 相机数学回归（批 12.1 · B1）——被测对象 = rgl/camera.cppm（rlcore 成员，纯数学）。
//
// 钉住的历史坑：
//   · 2026-09-03「天旋地转」（事件原文见 git history，git:调试历史 现文无此条）：cameraUp 的 z 分量符号写反 → 在 (yaw=0, pitch=±45°)
//     处 cross(forward, up) = 0，lookAt 基退化 → 画面中途 180° 翻滚。组 1 用
//     **正交性网格**（刻意含 yaw=0 × pitch=±45）钉死"up 与 forward 恒垂直"这条
//     构造性不变量——它是本项目对 lookAt 无 NaN 护栏的唯一防线（见组 5）。
//   · Phase 10b 批 11.0e：fps 模式下 target 是**死写入**（view 只由 yaw/pitch 推导），
//     直接写 target 回显"已生效"而画面不动。组 4 两侧都断言：fps 改 target → view
//     **逐位不变**；trackball 改 target → view 必变（反向对照，证明测量本身有分辨力）。
//   · cameraAimAt 是"给 target 就该看向它"的唯一正确入口（yaw/pitch 反推），组 3
//     做往返：aimAt 写入 → cameraForward/cameraView 读出，两函数互为反函数校验。
export module test_camera;

import rlstd;
import test_support;
import camera;

namespace RGL {
namespace {

using testing::Lcg;
using testing::expectNear;

/** 属性/网格测试的种子（写死）。 */
constexpr uint32_t kSeed = 20261001u;

/** 门关原点（批 4 第二刀：cameraView 新增 origin 参数；本文件全部组都是**门关语义**，
 *  传 dvec3(0) = 不 rebase = 与签名改动前逐位相同的调用。门开态的判据在
 *  tests/test_view_light_rebase.cppm，不在这里混着放）。 */
constexpr glm::dvec3 kNoOrigin{0.0, 0.0, 0.0};

/** 度 → 弧度（GLM_FORCE_RADIANS 下 radians() 恒等，手动换算与 camera.cppm 同法）。 */
constexpr float deg2rad(float d) { return d * glm::pi<float>() / 180.0f; }

/** yaw/pitch 采样网格（度）：**含 (yaw=0, pitch=±45)**——2026-09-03 退化的精确坐标，
 *  含 ±90（naive "forward ∥ 世界 up"场景）与近 180（atan2 分支边界）。 */
constexpr std::array<float, 8> kYawDeg{{0.0f, 45.0f, 90.0f, 135.0f, 180.0f, -60.0f, -90.0f, 179.0f}};
constexpr std::array<float, 7> kPitchDeg{{0.0f, 45.0f, -45.0f, 89.0f, -89.0f, 90.0f, -90.0f}};

/** 矩阵 16 分量是否全部有限（NaN/Inf 检查）。 */
bool matFinite(const glm::mat4& m) {
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (!std::isfinite(m[c][r]))
				return false;
	return true;
}

/** 一个 base 姿态：eye 离原点有一定距离（纯原点 eye 会让平移分量退化为 0）。 */
Camera makeCam(CameraMode mode, const glm::vec3& eye, float yawDeg, float pitchDeg) {
	Camera cam;
	cam.position = eye;
	cam.yaw = deg2rad(yawDeg);
	cam.pitch = deg2rad(pitchDeg);
	cam.mode = mode;
	return cam;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：方向基（forward/right/up）——正交性与约定锚点
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cameraForward/Right/Up：yaw=0,pitch=0 的约定锚点 = (−z, +x, +y)") {
	const Camera cam = makeCam(CameraMode::Fps, glm::vec3{0.0f}, 0.0f, 0.0f);
	const glm::vec3 f = cameraForward(cam);
	const glm::vec3 r = cameraRight(cam);
	const glm::vec3 u = cameraUp(cam);
	for (int k = 0; k < 3; ++k) {
		INFO("f[" << k << "]");
		expectNear(f[k], k == 2 ? -1.0 : 0.0, 0.0, 1e-6);
		INFO("r[" << k << "]");
		expectNear(r[k], k == 0 ? 1.0 : 0.0, 0.0, 1e-6);
		INFO("u[" << k << "]");
		expectNear(u[k], k == 1 ? 1.0 : 0.0, 0.0, 1e-6);
	}
}

TEST_CASE("cameraUp 正交性网格：cross(f,u)=r 处处成立（2026-09-03 天旋地转回归）") {
	// 判据：|f|=|u|=1、|dot(f,u)|≈0、cross(f,u)≈r。**网格必须含 (yaw=0, pitch=±45)**：
	// 历史上 up 的 z 分量符号写反时，恰在那两点 cross(f,u)=0 → lookAt 基退化。
	// 若正交性再被改坏，这里以"dot≠0 / cross≈0"的红直接定位到 cameraUp。
	for (float yawDeg : kYawDeg)
		for (float pitchDeg : kPitchDeg) {
			const Camera cam = makeCam(CameraMode::Fps, glm::vec3{0.0f}, yawDeg, pitchDeg);
			const glm::vec3 f = cameraForward(cam);
			const glm::vec3 r = cameraRight(cam);
			const glm::vec3 u = cameraUp(cam);
			INFO("yaw=" << yawDeg << " pitch=" << pitchDeg);
			expectNear(glm::length(f), 1.0, 1e-5, 1e-6);
			expectNear(glm::length(u), 1.0, 1e-5, 1e-6);
			// 正交 = 退化闸门：这条若失效，cameraView 的 lookAt 基就是退化的
			expectNear(glm::dot(f, u), 0.0, 0.0, 1e-5);
			const glm::vec3 c = glm::cross(f, u);
			for (int k = 0; k < 3; ++k)
				expectNear(c[k], r[k], 1e-4, 1e-5);
		}
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：cameraView 基元性质
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cameraView：眼点映射到原点（两模式 × 8×7 网格），且矩阵全有限") {
	// 批 11.0e 的教训要求每条"没变化"断言都配"有变化"对照——本组的对照是：
	// 同一组网格换 eye 后 view 平移列必须变（见组 4 的 trackball 侧）。
	for (float yawDeg : kYawDeg)
		for (float pitchDeg : kPitchDeg)
			for (CameraMode mode : {CameraMode::Fps, CameraMode::Trackball}) {
				const glm::vec3 eye{1.5f, -2.0f, 3.25f};
				Camera cam = makeCam(mode, eye, yawDeg, pitchDeg);
				// trackball 需要一个不与 eye 重合的 target（重合 = glm::lookAt 的
				// 真退化输入，组 5 单独说明），这里给个远端点
				if (mode == CameraMode::Trackball)
					cam.target = eye + glm::vec3{0.3f, -1.0f, 0.7f};
				const glm::mat4 view = cameraView(cam, kNoOrigin);
				INFO("yaw=" << yawDeg << " pitch=" << pitchDeg
							<< " mode=" << (mode == CameraMode::Fps ? "fps" : "trackball"));
				CHECK(matFinite(view));
				const glm::vec4 q = view * glm::vec4(eye, 1.0f);
				expectNear(q.x, 0.0, 0.0, 1e-3);
				expectNear(q.y, 0.0, 0.0, 1e-3);
				expectNear(q.z, 0.0, 0.0, 1e-3);
				expectNear(q.w, 1.0, 1e-5, 1e-6);
			}
}

TEST_CASE("cameraView：fps 沿 forward 前视 d 单位的点投到 (0,0,−d)；trackball 的 target 投到 (0,0,−dist)") {
	// 正向锚点：view 的三轴符号约定错（如 −f 写成 +f）时 q.z 变号，本组直接红。
	const glm::vec3 eye{2.0f, 1.0f, -3.0f};
	for (float yawDeg : {0.0f, 37.0f, 123.0f, -95.0f})
		for (float pitchDeg : {0.0f, 30.0f, -60.0f}) {
			const Camera cam = makeCam(CameraMode::Fps, eye, yawDeg, pitchDeg);
			const glm::mat4 view = cameraView(cam, kNoOrigin);
			const float d = 2.5f;
			const glm::vec4 q = view * glm::vec4(eye + cameraForward(cam) * d, 1.0f);
			INFO("fps yaw=" << yawDeg << " pitch=" << pitchDeg);
			expectNear(q.x, 0.0, 0.0, 1e-3);
			expectNear(q.y, 0.0, 0.0, 1e-3);
			expectNear(q.z, -d, 1e-4, 1e-3);
		}
	Camera tb = makeCam(CameraMode::Trackball, eye, 0.0f, 0.0f);
	tb.target = glm::vec3{-1.0f, 4.0f, 2.0f};  // 与 yaw/pitch 无关，只与 target 有关
	const glm::mat4 view = cameraView(tb, kNoOrigin);
	const glm::vec4 q = view * glm::vec4(tb.target, 1.0f);
	// tb.target 是绝对层（dvec3），eye 是 float：收窄后再减，与 cameraView 内部喂给
	// glm::lookAt 的那一份是同一个 float 值（断言因此测的是「view 真的看向 target」）
	const float dist = glm::length(glm::vec3(tb.target) - eye);
	INFO("trackball dist=" << dist);
	expectNear(q.x, 0.0, 0.0, 1e-3);
	expectNear(q.y, 0.0, 0.0, 1e-3);
	expectNear(q.z, -dist, 1e-4, 1e-3);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：cameraAimAt 往返（写入 yaw/pitch ↔ 读出 forward/view）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cameraAimAt 往返：aimAt(T) 后 forward ≈ normalize(T−eye) 且 view 把 T 投到视轴上（256 组随机）") {
	// cameraAimAt 与 cameraForward/cameraView 是互逆的一对（atan2/asin ↔ sin/cos）。
	// 往返判据同时覆盖两条路径：forward 的 dot（角度域）与 view·T 的横偏（矩阵域）。
	Lcg rng{kSeed};
	int tested = 0;
	for (int i = 0; i < 256; ++i) {
		const glm::vec3 eye{rng.uniform(-5, 5), rng.uniform(-5, 5), rng.uniform(-5, 5)};
		// 随机方向（拒绝近零向量，保证 normalize 良态）
		glm::vec3 dir{static_cast<float>(rng.uniform(-1, 1)), static_cast<float>(rng.uniform(-1, 1)),
			static_cast<float>(rng.uniform(-1, 1))};
		const float len = glm::length(dir);
		if (len < 0.3f)
			continue;
		dir /= len;
		const float distance = static_cast<float>(rng.uniform(1.0, 10.0));
		const glm::vec3 target = eye + dir * distance;

		Camera cam = makeCam(CameraMode::Fps, eye, 12.3f, -4.5f);  // 先给一组无关旧朝向
		cameraAimAt(cam, glm::dvec3(target));
		INFO("i=" << i << " eye=(" << eye.x << "," << eye.y << "," << eye.z
					<< ") target=(" << target.x << "," << target.y << "," << target.z << ")");
		// ① target 字段被写入（RPC /camera 的 echo 语义依赖它）
		for (int k = 0; k < 3; ++k)
			expectNear(cam.target[k], target[k], 0.0, 1e-6);
		// ② 角度域往返：forward 与真实朝向夹角 ≈ 0
		const glm::vec3 f = cameraForward(cam);
		expectNear(glm::dot(f, glm::normalize(target - eye)), 1.0, 1e-5, 1e-5);
		// ③ 矩阵域往返：target 落在视轴上（x/y ≈ 0，z = −distance）
		const glm::vec4 q = cameraView(cam, kNoOrigin) * glm::vec4(target, 1.0f);
		expectNear(q.x, 0.0, 0.0, 1e-2);
		expectNear(q.y, 0.0, 0.0, 1e-2);
		expectNear(q.z, -distance, 1e-3, 1e-2);
		++tested;
	}
	// 阳性锚点：256 组里至少 200 组被真实检验（拒绝分支不是主路径）
	CHECK(tested >= 200);
}

TEST_CASE("cameraAimAt 退化守卫：target≈position 保持朝向**但 target 字段仍写入**；正常 target 必改朝向") {
	// 守卫分支（length ≤ 1e-6 → return）两侧都要有用例，否则区分不了"没触发"和"坏掉"。
	// 实际行为（读源码为准，camera.cppm:154-162）：`camera.target = target` 在守卫
	// **之前**——退化时 target 照样写，只有 yaw/pitch 保持。
	Camera cam = makeCam(CameraMode::Fps, glm::vec3{1.0f, 2.0f, 3.0f}, 0.7f, -0.3f);
	const float yaw0 = cam.yaw, pitch0 = cam.pitch;
	const glm::vec3 sameSpot{1.0f + 1e-9f, 2.0f, 3.0f - 1e-9f};
	cameraAimAt(cam, glm::dvec3(sameSpot));
	// 守卫触发：朝向逐位不动（0 容差——早退分支不该产生任何浮点写入）
	CHECK(cam.yaw == yaw0);
	CHECK(cam.pitch == pitch0);
	// target 字段仍写入（批 11.0e echo 语义：回显的 target 与实际字段一致）
	expectNear(cam.target.x, sameSpot.x, 0.0, 0.0);
	// 反向对照：同样起点，正常 target → 朝向必须变（守卫不是恒真早退）
	cameraAimAt(cam, glm::dvec3{-4.0, 0.5, 2.0});
	CHECK(cam.yaw != yaw0);
	CHECK(cam.pitch != pitch0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：模式语义（批 11.0e 的"死写入"坑）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("fps 模式 target 是死写入：改 target → view 逐位不变；trackball 改 target → view 必变") {
	// 历史症状：fps 下直写 target，RPC 回显"已生效"而画面不动。断言分两侧：
	//   fps：两个仅 target 不同的相机，cameraView **逐位**相等（target 不参与）；
	//   trackball：同构造只换 target → view 至少一格不同（测量有分辨力的阳性对照，
	//     防止"cameraView 恒返回常量"这类实现把两侧都喂成绿）。
	const glm::vec3 eye{0.0f, 1.6f, 4.0f};
	for (const auto [ya, pi] : {std::pair{0.0f, 0.0f}, std::pair{35.0f, -20.0f}, std::pair{-120.0f, 60.0f}}) {
		Camera fpsA = makeCam(CameraMode::Fps, eye, ya, pi);
		Camera fpsB = fpsA;
		fpsB.target = glm::vec3{100.0f, -50.0f, 7.0f};  // 天差地别的 target
		const glm::mat4 va = cameraView(fpsA, kNoOrigin), vb = cameraView(fpsB, kNoOrigin);
		INFO("fps yaw=" << ya << " pitch=" << pi);
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				CHECK(va[c][r] == vb[c][r]);  // 逐位

		Camera tbA = makeCam(CameraMode::Trackball, eye, ya, pi);
		Camera tbB = tbA;
		tbB.target = glm::vec3{-3.0f, 2.0f, 5.0f};
		const glm::mat4 ta = cameraView(tbA, kNoOrigin), tb = cameraView(tbB, kNoOrigin);
		bool anyDiff = false;
		for (int c = 0; c < 4; ++c)
			for (int r = 0; r < 4; ++r)
				if (ta[c][r] != tb[c][r])
					anyDiff = true;
		INFO("trackball yaw=" << ya);
		CHECK(anyDiff);
	}
}

TEST_CASE("fps 的 up 字段不参与（由 yaw/pitch 推导）；trackball 携带 up 生效（roll 可见）") {
	const glm::vec3 eye{1.0f, 0.0f, 2.0f};
	Camera fpsA = makeCam(CameraMode::Fps, eye, 20.0f, -10.0f);
	Camera fpsB = fpsA;
	fpsB.up = glm::normalize(glm::vec3{1.0f, 1.0f, 1.0f});  // 乱写 up
	const glm::mat4 va = cameraView(fpsA, kNoOrigin), vb = cameraView(fpsB, kNoOrigin);
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			CHECK(va[c][r] == vb[c][r]);  // fps：up 字段是死数据

	// trackball：up 参与 lookAt。给两个不平行于视线的 up（平行时 glm 无护栏 → NaN，
	// 见组 5 注释），roll 差必须反映到 view 矩阵。
	Camera tbA = makeCam(CameraMode::Trackball, eye, 0.0f, 0.0f);
	tbA.target = eye + glm::vec3{0.0f, 0.0f, -3.0f};  // 视线沿 −z
	tbA.up = glm::vec3{0.0f, 1.0f, 0.0f};
	Camera tbB = tbA;
	tbB.up = glm::vec3{0.7071f, 0.7071f, 0.0f};  // 绕视线（−z）滚 45°，仍 ⊥ 视线
	const glm::mat4 ta = cameraView(tbA, kNoOrigin), tb = cameraView(tbB, kNoOrigin);
	bool anyDiff = false;
	for (int c = 0; c < 4; ++c)
		for (int r = 0; r < 4; ++r)
			if (ta[c][r] != tb[c][r])
				anyDiff = true;
	CHECK(anyDiff);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 5：退化与 NaN
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("±90° pitch（naive up∥forward 场景）两模式全网格不产生 NaN") {
	// 任务要求"forward 与 up 平行时不得 NaN"。实际机制（读源码为准）：
	// glm::lookAt **没有**退化护栏（cross(f,up)=0 → normalize 出 NaN），camera 模块
	// 靠**构造性规避**：fps 的 up 用 cameraUp()（组 1 已证恒 ⊥ forward，随 pitch
	// 一起转，永远不会与 forward 平行）。因此本组的判据 = 网格（含 pitch=±90，
	// 即"forward ∥ 世界 up"的全部坐标）下 view 16 分量逐格 std::isfinite。
	// trackball 侧同样跑网格：lookAt 用携带 up（默认世界 up）+ 固定远 target，
	// 网格内 up 不与视线平行 → 同样必须全有限。
	for (float yawDeg : kYawDeg)
		for (float pitchDeg : kPitchDeg) {
			const glm::vec3 eye{0.5f, 1.5f, -2.5f};
			const Camera fps = makeCam(CameraMode::Fps, eye, yawDeg, pitchDeg);
			INFO("fps yaw=" << yawDeg << " pitch=" << pitchDeg);
			CHECK(matFinite(cameraView(fps, kNoOrigin)));
			Camera tbCam = makeCam(CameraMode::Trackball, eye, yawDeg, pitchDeg);
			tbCam.target = eye + glm::vec3{0.2f, 1.0f, -0.4f};
			INFO("trackball yaw=" << yawDeg << " pitch=" << pitchDeg);
			CHECK(matFinite(cameraView(tbCam, kNoOrigin)));
		}
	// 已知未护栏路径（记录、不断言 NaN——那是 glm 的行为不是本模块的契约）：
	// trackball 且 position == target 时 glm::lookAt 归一化零向量 → NaN；调用方
	// （camera_config 反推、拖拽路径）都以"target 与 position 重合 = 非法机位"约束。
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 6：与 test_support 视锥工具的自洽
//
// camera.cppm **不导出** proj 构造函数（fov/near/far 只是 Camera 字段，物化在
// 渲染侧），所以这里没有"camera proj == glm::perspective"可断言——不臆造 API。
// 改为：cameraView + 测试侧 glm::perspective 合成后，与 test_support 的
// frustumCorners（inverse(proj·view) 反投影）对账，锁相机 view 与视锥工具同一套
// 矩阵顺序/深度约定（RH_ZO）。
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cameraView + glm::perspective：aimAt 的目标落在视锥轴上，近平面中心 ≈ eye+f·near") {
	Lcg rng{kSeed ^ 0x5a5au};
	for (int i = 0; i < 32; ++i) {
		const glm::vec3 eye{static_cast<float>(rng.uniform(-3, 3)), static_cast<float>(rng.uniform(-3, 3)),
			static_cast<float>(rng.uniform(-3, 3))};
		glm::vec3 dir{static_cast<float>(rng.uniform(-1, 1)), static_cast<float>(rng.uniform(-1, 1)),
			static_cast<float>(rng.uniform(-1, 1))};
		if (glm::length(dir) < 0.3f)
			continue;
		dir = glm::normalize(dir);
		const float distance = static_cast<float>(rng.uniform(1.5, 8.0));

		Camera cam = makeCam(CameraMode::Fps, eye, 0.0f, 0.0f);
		cameraAimAt(cam, glm::dvec3(eye + dir * distance));
		const glm::mat4 view = cameraView(cam, kNoOrigin);
		const glm::mat4 proj = glm::perspective(cam.fov, 16.0f / 9.0f, cam.nearPlane, cam.farPlane);
		INFO("i=" << i << " distance=" << distance);

		// ① 目标点的 NDC：x/y ≈ 0（视轴上），z ∈ (0,1)（RH_ZO，且在近远之间）
		const glm::vec4 clip = proj * view * glm::vec4(cam.target, 1.0f);
		const double ndcX = clip.x / clip.w;
		const double ndcY = clip.y / clip.w;
		const double ndcZ = clip.z / clip.w;
		expectNear(ndcX, 0.0, 0.0, 1e-3);
		expectNear(ndcY, 0.0, 0.0, 1e-3);
		CHECK(ndcZ > 0.0);
		CHECK(ndcZ < 1.0);

		// ② test_support::frustumCorners 的前 4 角（近平面）均值 ≈ eye + f·near。
		//    该函数走 inverse(proj·view)，near/far=100 时条件数放大误差（其文档自述
		//    0.1~0.3%），容差按该精度契约给 1%。
		const auto corners = testing::frustumCorners(view, proj);
		REQUIRE(corners.size() == 8);
		glm::vec3 nearCenter{0.0f};
		for (size_t k = 0; k < 4; ++k)
			nearCenter += corners[k];
		nearCenter /= 4.0f;
		const glm::vec3 wantNear = eye + cameraForward(cam) * cam.nearPlane;
		// 期望向量模长 ~|eye|（可达数单位），用相对 eye 尺度的绝对容差 = 0.5% 场景尺度
		for (int k = 0; k < 3; ++k)
			expectNear(nearCenter[k], wantNear[k], 0.0, 0.05);
	}
}

}  // namespace RGL
