module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>  // glm::pi

/**
 * test_camera_frame_yaw —— 相机帧驱动 yaw 的纯函数层（rgl/camera.cppm::frameDrivenCameraYaw，rlbase）
 *
 * 被测 = 「yaw = yawBase + yawPerFrame·frameIndex」这一条闭式。它是门 `frame_yaw`
 * （相机运动诊断激励，默认关）的判据本体：imgqa temporal-motion 的**跨进程 A/B 对齐**
 * 完全建立在「同一 frameIndex 在任何运行给出同一朝向」上——函数若掺进累积/时间/隐藏
 * 状态，候选图与超采样参考量的就不是同一机位序列，整条相机运动判据作废
 * （dt 累积不可复现的教训写死在 tools/imgqa/temporal.cpp 头注释；同族先例 =
 * test_scene_spin 钉 frameDrivenSpinAngle）。
 *
 * ## 期望值出处（独立于被测实现）
 * 全部断言的期望值 = 手算闭式 base + n·a（右侧就是数学式本身，不抄实现）；
 * 步长取 2 的幂倒数 ⟹ float32 缩放精确，闭式断言写成**恰等**。
 *
 * ## 阳性锚点与反向用例
 *  - yaw(0) **恰为 yawBase**：帧 0 = 基准（跨进程对齐的零点约定；`frameIndex±1` 类
 *    off-by-one 变异当场红）；
 *  - yaw(a=0) **恰为 yawBase**：门关值不造角度（门关 = 逐位不变的算术表达）；
 *  - 纯函数性 = 同输入两次调用**逐位相同** + 与调用顺序无关（先大后小 vs 先小后大
 *    给出同一张表）——堵「偷偷累积」的变异；
 *  - 10⁴ 帧精度账 = double 闭式对照（float32 舍入界 < 0.01px 等效像素，写死在注释）。
 *
 * ## 变异红线（哪条变异让我红）
 *  `+` 改 `−`（基准被减）→ 基准非零的闭式全红；`*` 改 `+` → n=1 起全红；
 *  `frameIndex` 改 `frameIndex−1` → yaw(0) ≠ base 红；改成 static 累积 → 纯函数性红；
 *  丢掉 yawBase（= spin 版）→ base ≠ 0 的断言红（相机版与 spin 版的**唯一差别**就是基准项，
 *  RPC 重定基全靠它）。
 */
export module test_camera_frame_yaw;

import rlstd;
import camera;
import test_support;  // testing::expectNear

namespace RGL {

TEST_CASE("camera_frame_yaw：yaw = 基准 + 每帧角 × 帧号（闭式手算；恰等断言靠 2 的幂）") {
	constexpr float a = 0.00349066f;  // 0.2°/帧（imgqa temporal-motion 默认激励步长）
	constexpr float base = 1.25f;     // 任意非零基准（RPC 重定基后的形态）
	// 零点约定：帧 0 = 基准本身（跨进程对齐的基准，容不得 off-by-one）
	CHECK(frameDrivenCameraYaw(base, a, 0u) == base);
	CHECK(frameDrivenCameraYaw(0.0f, a, 0u) == 0.0f);
	// 门关值：每帧角 = 0 ⟹ 任何帧号都停在基准（门关 = 逐位不变的算术表达）
	CHECK(frameDrivenCameraYaw(base, 0.0f, 999u) == base);
	// 2 的幂缩放 float32 精确 ⟹ 用 a2 = 2^-9 做**恰等**闭式断言
	constexpr float a2 = 0.001953125f;  // 2^-9 rad/帧
	CHECK(frameDrivenCameraYaw(0.0f, a2, 1u) == a2);
	CHECK(frameDrivenCameraYaw(base, a2, 2u) == base + 2.0f * a2);
	CHECK(frameDrivenCameraYaw(base, a2, 512u) == base + 1.0f);  // 512·2^-9 = 1 精确
	// 一般步长：期望值 = 数学闭式本身（单次舍入，与实现同式不同源）
	CHECK(frameDrivenCameraYaw(base, a, 100u) == base + a * 100.0f);
	// 负步长（反向 yaw）同样成立
	CHECK(frameDrivenCameraYaw(base, -a, 16u) == base - 16.0f * a);
}

TEST_CASE("camera_frame_yaw：纯函数性（同输入逐位相同、与调用顺序无关）+ 10⁴ 帧精度账") {
	constexpr float a = 0.00349066f;
	constexpr float base = -2.4f;
	// 纯函数性：先大后小 vs 先小后大给出同一张表（堵「偷偷累积」——累积版两种遍历顺序
	// 的表**必然不同**，因为大帧号会把小帧号的返回值污染掉）。
	float forward[17];
	float backward[17];
	for (uint32_t i = 0; i <= 16u; ++i)
		forward[i] = frameDrivenCameraYaw(base, a, i);
	for (int i = 16; i >= 0; --i)
		backward[static_cast<size_t>(i)] = frameDrivenCameraYaw(base, a, static_cast<uint32_t>(i));
	for (uint32_t i = 0; i <= 16u; ++i)
		CHECK(forward[i] == backward[i]);  // 逐位相同（同输入两次调用，无容差）
	// 10⁴ 帧精度账（与 frameDrivenSpinAngle 同账）：10⁴ 帧 × 3.5e-3 rad/帧 ≈ 35 rad，
	// float32 ULP ≈ 4e-6 rad；焦距 ≈869px（fov 45°/1280px 宽）下等效位移 ≈0.003px——
	// 远低于任何可测信号（对齐 argmin 一帧 = 3–5px），无需 double。
	constexpr uint32_t n = 10000u;
	const double exact = static_cast<double>(base) + static_cast<double>(a) * n;
	CHECK(std::abs(static_cast<double>(frameDrivenCameraYaw(base, a, n)) - exact) < 1e-4);
}

/**
 * 机位锚点（债-89，RPC `yaw_at_frame` 的算术本体）。
 *
 * ## 期望值出处（不抄实现）
 * 约定 = 「帧首钩子先 `++帧计数`、cameraSystem 先自增 `frameIndex` 再取值」⟹ 第 X 帧渲染
 * 的 yaw 用索引 `i_now + (X − n_now) + 1`。本用例把该索引**手算成常量**（525 = 40+484+1），
 * 再用已单独钉死的 `frameDrivenCameraYaw` 求值——实现若把 `+1` 或 `(X−n_now)` 写错，
 * 手算索引处就对不上 `pin`。步长取 2^-9 ⟹ 全程 float32 精确，断言写成恰等。
 *
 * ## 变异红线（哪条变异让我红）
 *  丢 `+1u`（先加后用读成后用先加）→ 全部恰等断言红；`(pinnedFrame − engineFrameNow)`
 *  写成 `pinnedFrame`（把「帧计数与 yaw 索引的偏移」当成 0）→ 偏移非 1 的两条红；
 *  `yaw` 分量取成 `yawPinned`（而非本帧期望值）→ 「不重定基」不变式红 = 紧随的
 *  cameraSystem 当场把锚点抹掉（`applyFrameDrivenYaw` 以「yaw ≠ 期望值」判外部写入）；
 *  符号反（base = pin + a·idx）→ 全红。
 */
TEST_CASE("camera_frame_yaw：yaw_at_frame 锚点 = 第 N 帧渲染值恰等于钉入值（含当帧边界与计数器偏移）") {
	constexpr float a2 = 0.001953125f;  // 2^-9 rad/帧（float32 精确缩放）
	constexpr float pin = 0.5f;
	// ① 常规锚点：yaw 索引 40 / 帧计数 44（**两者不同**，实测本机就是这种偏移）→ 第 528 帧
	//   渲染用的索引 = 40 + (528 − 44) + 1 = 525（手算）。
	const YawPin p = frameDrivenYawPin(pin, a2, 40u, 44u, 528u);
	CHECK(frameDrivenCameraYaw(p.yawBase, a2, 525u) == pin);
	//   锚点只钉一帧：相邻索引必须**不是** pin（钉歪一帧 = 0.125px/帧量级的机位彩票）。
	CHECK(frameDrivenCameraYaw(p.yawBase, a2, 524u) != pin);
	CHECK(frameDrivenCameraYaw(p.yawBase, a2, 526u) != pin);
	//   不重定基不变式：写回的 yaw 必须恰等于本帧期望值，否则 applyFrameDrivenYaw 判成
	//   外部写入 → 重定基 → 锚点当场作废。
	CHECK(p.yaw == frameDrivenCameraYaw(p.yawBase, a2, 40u));
	// ② 当帧边界（pinnedFrame == 帧计数）：本帧的 cameraSystem 还没跑 ⟹ 还来得及，
	//   渲染索引 = 40 + 0 + 1 = 41。
	const YawPin now = frameDrivenYawPin(pin, a2, 40u, 44u, 44u);
	CHECK(frameDrivenCameraYaw(now.yawBase, a2, 41u) == pin);
	CHECK(now.yaw == frameDrivenCameraYaw(now.yawBase, a2, 40u));
	// ③ 反向用例：偏移 = 0（两个计数器恰好相等）时锚点仍落在第 N 帧——堵「实现里偷偷
	//   假设 yawFrameIndex == 帧计数 − 1」的写法（那样这条必然差一帧）。
	const YawPin off0 = frameDrivenYawPin(pin, a2, 44u, 44u, 528u);
	CHECK(frameDrivenCameraYaw(off0.yawBase, a2, 529u) == pin);  // 44 + 484 + 1
	CHECK(frameDrivenCameraYaw(off0.yawBase, a2, 528u) != pin);
	// ④ 门关值（每帧角 = 0）：任何帧都停在钉入值，锚点退化成「就是这一角」。
	const YawPin gateOff = frameDrivenYawPin(pin, 0.0f, 7u, 9u, 528u);
	CHECK(gateOff.yawBase == pin);
	CHECK(gateOff.yaw == pin);
}

}  // namespace RGL
