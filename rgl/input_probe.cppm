module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
export module input_probe;

import rlstd;
import rllogger;
import diagnostics;  // 探针门（configs/diagnostics.lua：ui_click_probe / camera_probe）
import camera;       // Camera / CameraState / CameraMode（探针断言目标）

namespace RGL {

// ─── 脚本化输入探针（窗口模式输入链回归，2026-09-28；docs/调试历史.md）────────────
// 方法论同 picking_probe / texel_canary：配置门控（默认关 = 零行为变化）+ 帧号触发 +
// SDL_PushEvent 注入合成事件 + 日志 PASS/FAIL——无物理鼠标/键盘依赖，"点击面板→
// 键盘捕获"场景可脚本复现。帧计数 0 起（picking_probe 实际语义）；调用方（ecs
// renderSystem）须在 eventSystem 之后、cameraSystem 之前调用（合成事件次帧才被
// 轮询/消费）。不 import ecs 模块（registry 仅按引用透传；entt 走 GMF 直 include，
// 同 camera.cppm 先例）。

namespace {

/** 相机键盘探针阶段（cameraProbeTick 状态机：Idle→Holding→Released→Done）。 */
enum class CameraProbePhase { Idle, Holding, Released, Done };

/** 注入合成键事件到 SDL 队列（次帧 eventSystem 轮询）。 */
void probePushKey(uint32_t key, SDL_EventType type) {
	SDL_Event ev{};
	ev.type = type;
	ev.key.key = key;
	SDL_PushEvent(&ev);
}

/** 移动键 → 对应 CameraState level 标志位（未绑定 = nullptr）。 */
bool* probeStateFlag(const Camera& camera, CameraState& state, uint32_t key) {
	if (key == camera.keys.forward)
		return &state.keyForward;
	if (key == camera.keys.back)
		return &state.keyBack;
	if (key == camera.keys.left)
		return &state.keyLeft;
	if (key == camera.keys.right)
		return &state.keyRight;
	return nullptr;
}

/** 探针失败统一出口（error 级：回归护栏 FAIL 必须醒目）。 */
void probeFail(RLLogger* logger, std::string_view reason) {
	if (logger)
		logger->error("camera probe: FAIL——{}", reason);
}

/** Idle 阶段：键未绑定移动键 = warn 终止；首帧 = 记基准 + 注入 KeyDown 转 Holding。 */
CameraProbePhase cameraProbeIdle(RLLogger* logger, bool* flag, uint32_t key,
	uint32_t frameCount, uint32_t probe, uint32_t hold, const glm::vec3& position,
	glm::vec3& baseline) {
	if (flag == nullptr) {
		if (logger)
			logger->warn("camera probe: 键 0x{:x} 未绑定 Camera.keys 移动键（W/A/S/D），探针跳过",
				key);
		return CameraProbePhase::Done;
	}
	if (frameCount == probe) {
		baseline = position;  // 按下前基准（探针窗口内无其他相机移动源）
		probePushKey(key, SDL_EVENT_KEY_DOWN);
		if (logger)
			logger->info("camera probe: 第 {} 帧按下键 0x{:x}（保持 {} 帧）", probe, key, hold);
		return CameraProbePhase::Holding;
	}
	return CameraProbePhase::Idle;
}

/**
 * Holding 阶段：时序（tick 在 system 链前）——probe+1 KeyDown 被 cameraSystem 消费
 * → probe+2 首查置位（未到 = FAIL 断链）；按住期间状态丢失 = FAIL；probe+hold 注入
 * KeyUp（该帧 cameraSystem 仍是最后一段移动）转 Released。
 */
CameraProbePhase cameraProbeHolding(RLLogger* logger, bool* flag, uint32_t key,
	uint32_t frameCount, uint32_t probe, uint32_t hold) {
	if (frameCount == probe + 2) {
		if (!*flag) {
			probeFail(logger, "KeyDown 未到 CameraState（输入链 eventSystem→InputFilter→"
				"cameraSystem 断链，疑似被调试层吞没）");
			probePushKey(key, SDL_EVENT_KEY_UP);
			return CameraProbePhase::Done;
		}
		if (logger)
			logger->debug("camera probe: 链路完好（第 {} 帧 CameraState 已置位）", frameCount);
		return CameraProbePhase::Holding;
	}
	if (frameCount > probe + 2 && frameCount < probe + hold) {
		if (!*flag) {
			probeFail(logger, "按住期间键状态丢失（未注入 KeyUp）");
			return CameraProbePhase::Done;
		}
		return CameraProbePhase::Holding;
	}
	if (frameCount == probe + hold) {
		probePushKey(key, SDL_EVENT_KEY_UP);
		return CameraProbePhase::Released;
	}
	return CameraProbePhase::Holding;
}

/** Released 阶段：probe+hold+2 终查（KeyUp 于 hold+1 帧被消费）——清除 + fps 位移
 *  均对 = PASS，否则 FAIL；转 Done。 */
CameraProbePhase cameraProbeReleased(RLLogger* logger, bool* flag, uint32_t frameCount,
	uint32_t probe, uint32_t hold, const Camera& camera, const glm::vec3& baseline) {
	if (frameCount != probe + hold + 2)
		return CameraProbePhase::Released;
	const bool released = !*flag;
	const float distance = glm::length(camera.position - baseline);
	const bool moved = camera.mode != CameraMode::Fps || distance > 1e-4f;
	if (released && moved) {
		if (logger)
			logger->info("camera probe: PASS（保持 {} 帧，CameraState 置/清正确，fps 位移 {}）",
				hold, distance);
	} else {
		probeFail(logger, "KeyUp 清除=" + std::to_string(released) +
			" fps 位移=" + std::to_string(moved) + "（键事件链或移动路径异常）");
	}
	return CameraProbePhase::Done;
}

}  // namespace

/** 脚本化 UI 点击探针（diagnostics.ui_click_probe = {frame,x,y}）：第 frame 帧注入
 * 一次鼠标左键点击（down 于 frame、up 于 frame+1，跨一帧 NewFrame = imgui 判"点击"）
 * ——2026-09-28 "点面板后 WASD 失效" 场景的点击半边（键盘半边见 cameraProbeTick）。
 */
export void uiClickProbeTick() {
	const auto probe = diagnostics().uiClickProbeFrame;
	if (probe == 0)
		return;
	static uint32_t frameCount = 0;
	static bool downPushed = false;
	auto pushButton = [](SDL_EventType type) {
		SDL_Event ev{};
		ev.type = type;
		ev.button.button = SDL_BUTTON_LEFT;
		ev.button.x = static_cast<int>(diagnostics().uiClickProbeX);
		ev.button.y = static_cast<int>(diagnostics().uiClickProbeY);
		SDL_PushEvent(&ev);
	};
	if (!downPushed && frameCount == probe) {
		pushButton(SDL_EVENT_MOUSE_BUTTON_DOWN);
		downPushed = true;
		if (auto* logger = RLLogger::getInstance())
			logger->info("ui_click probe: 第 {} 帧合成左键点击 ({}, {})", probe,
				diagnostics().uiClickProbeX, diagnostics().uiClickProbeY);
	} else if (downPushed && frameCount == probe + 1) {
		pushButton(SDL_EVENT_MOUSE_BUTTON_UP);
	}
	++frameCount;
}

/**
 * 脚本化相机键盘探针（diagnostics.camera_probe = {frame,key,holdFrames}）：第 frame
 * 帧按住 Camera.keys 绑定的移动键 holdFrames 帧，验证 eventSystem→InputFilter→
 * cameraSystem 全链：KeyDown 后 CameraState 置位（+fps 模式相机位移）、KeyUp 后
 * 清除——PASS/FAIL 打日志。2026-09-28 "面板吞 WASD" 回归护栏：调试层若再吞键盘
 * 事件，首查即 FAIL。key 未绑定移动键 = warn 跳过；配置全关 = 早退零开销。
 * @param rg registry（读 Camera/CameraState）
 */
export void cameraProbeTick(entt::registry& rg) {
	const auto probe = diagnostics().cameraProbeFrame;
	const auto key = diagnostics().cameraProbeKey;
	if (probe == 0 || key == 0)
		return;
	static CameraProbePhase phase = CameraProbePhase::Idle;
	static uint32_t frameCount = 0;
	static glm::vec3 baseline{0.0f};
	auto* logger = RLLogger::getInstance();

	auto& camera = rg.ctx().get<Camera>();
	auto& state = rg.ctx().get<CameraState>();
	bool* flag = probeStateFlag(camera, state, key);
	const uint32_t hold = std::max<uint32_t>(1, diagnostics().cameraProbeHold);

	switch (phase) {
		case CameraProbePhase::Idle:
			phase = cameraProbeIdle(logger, flag, key, frameCount, probe, hold,
				camera.position, baseline);
			break;
		case CameraProbePhase::Holding:
			phase = cameraProbeHolding(logger, flag, key, frameCount, probe, hold);
			break;
		case CameraProbePhase::Released:
			phase = cameraProbeReleased(logger, flag, frameCount, probe, hold, camera,
				baseline);
			break;
		case CameraProbePhase::Done:
			break;
	}
	++frameCount;
}

}  // namespace RGL
