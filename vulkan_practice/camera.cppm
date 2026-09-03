module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
export module camera;

import rlstd;
import rllogger;

namespace RGL {

export enum class CameraMode {
	Trackball,
	Fps,
};

// 键位绑定（configs/camera.lua 的 keys 字段，sol2 沙箱 + 可选字段回落默认）：
// 动作 → SDL 键码；默认 WASD。
export struct KeyBindings {
	uint32_t forward = SDLK_W;
	uint32_t back = SDLK_S;
	uint32_t left = SDLK_A;
	uint32_t right = SDLK_D;
};

// 相机姿态：yaw/pitch 表示朝向（yaw=0, pitch=0 → forward=(0,0,-1)）。
// trackball 模式下相机绕 target 公转（object 始终居屏中心）：拖拽旋转
// position-target 偏移向量后，yaw/pitch 由新位置反推，保持朝向 target。
// fps 模式下 target 不参与计算（自由飞行）。
// 注意：项目启用 GLM_FORCE_RADIANS，glm::radians() 是恒等函数，角度→弧度必须手动换算。
export struct Camera {
	glm::vec3 position{0.0f, 0.0f, 3.0f};
	glm::vec3 target{0.0f, 0.0f, 0.0f};
	float yaw = 0.0f;
	float pitch = 0.0f;
	// trackball 携带的 up（basis 携带，同 legacy TrackballSystem）：随拖拽与 offset
	// 同步旋转，不反推 yaw/pitch → 极点无 atan2(0,0) 奇异、可翻过头顶。fps 模式不用它
	//（up 由 yaw/pitch 推导）。config 加载时按 world-up 对齐初始化。
	glm::vec3 up{0.0f, 1.0f, 0.0f};
	float fov = 45.0f * glm::pi<float>() / 180.0f;
	float nearPlane = 0.1f;
	float farPlane = 10.0f;
	CameraMode mode = CameraMode::Trackball;
	float fpsSpeed = 3.0f;
	float fpsSensitivity = 0.003f;
	float trackballSensitivity = 0.005f;
	float zoomSpeed = 0.03f;
	KeyBindings keys;  // 键位绑定（configs/camera.lua 的 keys 字段，E5.4：键位是数据不是硬编码）
};

// 输入事件（eventSystem 产生，cameraSystem 消费即清空）：
// 键事件带 SDL 键码——键位映射不在 eventSystem，消费方按 Camera.keys 解释
//（E5.4：输入是事件流不是共享 level-state 袋）；鼠标按键事件带 button + pressed，
// level 状态由 cameraSystem 自管（CameraState），不经输入共享。
export struct InputEvent {
	enum class Type {
		KeyDown,
		KeyUp,
		MouseButton,
		MouseDelta,
		Wheel,
	};
	Type type = Type::KeyDown;
	uint32_t key = 0;      // SDL 键码（KeyDown/KeyUp）
	uint8_t button = 0;    // SDL 鼠标按键（MouseButton）
	bool pressed = false;  // MouseButton：true = 按下
	glm::vec2 delta{};     // MouseDelta（xrel/yrel 增量）
	float amount = 0.0f;   // Wheel（y 增量）
};

// 输入事件流（registry context；eventSystem 只产事件，cameraSystem 消费即清空；
// 退出/窗口状态不走事件，见 ecs.cppm 的 AppExit/WindowExtent）
export struct InputEvents {
	std::deque<InputEvent> events;
};

// 相机内部状态（cameraSystem 独占：键/鼠标按键 level 状态，每帧由输入事件更新；
// 消费方显式管理，不与其他 system 共享——E5.4）
export struct CameraState {
	bool keyForward = false;
	bool keyBack = false;
	bool keyLeft = false;
	bool keyRight = false;
	bool mouseLeftDown = false;
	bool mouseRightDown = false;
};

export glm::vec3 cameraForward(const Camera& camera) {
	return glm::vec3(-std::sin(camera.yaw) * std::cos(camera.pitch),
		std::sin(camera.pitch),
		-std::cos(camera.yaw) * std::cos(camera.pitch));
}

export glm::vec3 cameraRight(const Camera& camera) {
	return glm::vec3(std::cos(camera.yaw), 0.0f, -std::sin(camera.yaw));
}

// 必须与 cameraForward 同约定：先绕世界 Y 转 yaw，再绕相机右轴转 pitch。
// 由该约定推得 up = (sin yaw·sin pitch, cos pitch, +cos yaw·sin pitch)——z 分量
// 符号写反时 up 与视线会在特定 (yaw, pitch) 处反平行（yaw=0, pitch=±45° 时
// cross(f,up)=0），lookAt 基退化 → 画面中途 180° 翻滚（2026-09-03 天旋地转根因）。
export glm::vec3 cameraUp(const Camera& camera) {
	return glm::vec3(std::sin(camera.yaw) * std::sin(camera.pitch),
		std::cos(camera.pitch),
		std::cos(camera.yaw) * std::sin(camera.pitch));
}

export glm::mat4 cameraView(const Camera& camera) {
	// trackball：视角恒指向 target，up 用携带状态（basis 携带，极点无奇异）；
	// fps：自由飞行，forward/up 由 yaw/pitch 推导，target 不参与
	if (camera.mode == CameraMode::Trackball)
		return glm::lookAt(camera.position, camera.target, camera.up);
	return glm::lookAt(camera.position, camera.position + cameraForward(camera),
		cameraUp(camera));
}

// Rodrigues 旋转公式：绕已归一化 axis 转 angle（弧度）。
// （GLM 1.0.x 的向量版 glm::rotate 在已弃用的 gtx/rotate_vector.hpp，不引入）
glm::vec3 rotateAroundAxis(glm::vec3 v, float angle, glm::vec3 axis) {
	return v * std::cos(angle) + glm::cross(axis, v) * std::sin(angle) +
		axis * glm::dot(axis, v) * (1.0f - std::cos(angle));
}

// pitch 限制 ±89°，防止万向翻转（manual 换算：GLM_FORCE_RADIANS 下 radians() 无效）
constexpr float kPitchLimit = 89.0f * glm::pi<float>() / 180.0f;
// 单帧输入增量限幅：Wayland 可能一次下发超大增量（合并的 pointer 事件 / 指针 grab），
// 不限幅时相机一帧内甩出几百度，目标物体直接飞出视野
constexpr float kMaxMouseDeltaPerFrame = 100.0f;
constexpr float kMaxWheelPerFrame = 10.0f;

export void cameraSystem(entt::registry& rg, float dt) {
	auto& camera = rg.ctx().get<Camera>();
	auto& state = rg.ctx().get<CameraState>();
	auto& events = rg.ctx().get<InputEvents>();
	auto* logger = RLLogger::getInstance();

	// 消费即清空：键事件/鼠标按键事件 → level 状态（cameraSystem 自管），
	// delta/wheel 事件 → 本帧累积
	glm::vec2 mouseDelta{0.0f};
	float wheel = 0.0f;
	const auto& keys = camera.keys;
	for (const auto& ev : events.events) {
		switch (ev.type) {
			case InputEvent::Type::KeyDown:
				if (ev.key == keys.forward) state.keyForward = true;
				else if (ev.key == keys.back) state.keyBack = true;
				else if (ev.key == keys.left) state.keyLeft = true;
				else if (ev.key == keys.right) state.keyRight = true;
				break;
			case InputEvent::Type::KeyUp:
				if (ev.key == keys.forward) state.keyForward = false;
				else if (ev.key == keys.back) state.keyBack = false;
				else if (ev.key == keys.left) state.keyLeft = false;
				else if (ev.key == keys.right) state.keyRight = false;
				break;
			case InputEvent::Type::MouseButton:
				if (ev.button == SDL_BUTTON_LEFT) state.mouseLeftDown = ev.pressed;
				else if (ev.button == SDL_BUTTON_RIGHT) state.mouseRightDown = ev.pressed;
				break;
			case InputEvent::Type::MouseDelta:
				mouseDelta += ev.delta;
				break;
			case InputEvent::Type::Wheel:
				wheel += ev.amount;
				break;
		}
	}
	events.events.clear();

	bool dragging = (camera.mode == CameraMode::Fps && state.mouseRightDown) ||
		(camera.mode == CameraMode::Trackball && state.mouseLeftDown);
	if (dragging) {
		float dx = std::clamp(mouseDelta.x, -kMaxMouseDeltaPerFrame,
			kMaxMouseDeltaPerFrame);
		float dy = std::clamp(mouseDelta.y, -kMaxMouseDeltaPerFrame,
			kMaxMouseDeltaPerFrame);
		if (dx == kMaxMouseDeltaPerFrame || dx == -kMaxMouseDeltaPerFrame ||
			dy == kMaxMouseDeltaPerFrame || dy == -kMaxMouseDeltaPerFrame) {
			if (logger)
				logger->debug("camera: clamped huge mouse delta ({}, {}) -> ({}, {})",
					mouseDelta.x, mouseDelta.y, dx, dy);
		}
		if (camera.mode == CameraMode::Fps) {
			// FPS 鼠标 look：拖右→向右看，拖上→向上看（相机原地转朝向）
			camera.yaw -= dx * camera.fpsSensitivity;
			camera.pitch -= dy * camera.fpsSensitivity;
		} else {
			// Trackball：basis 携带（同 legacy TrackballSystem）——offset 与 up 随拖拽
			// 同步旋转，不反推 yaw/pitch（避开 atan2(0,0) 极点奇异）。up 与视线同旋转
			// 保内积，恒垂直视线 → 极点不抖、可翻过头顶（roll 连续累积，物理转球手感）。
			// 手感约定（同 three.js OrbitControls）：拖右→object 正面朝右（相机向左公转）；
			// 拖下→object 顶部朝向观察者（相机上移）
			glm::vec3 offset = camera.position - camera.target;
			if (glm::length(offset) > 1e-4f) {
				const float s = camera.trackballSensitivity;
				// 竖直轴 = 屏幕右向量 forward × up（恒 ⊥ 二者，极点非零、无奇异）
				glm::vec3 forward = glm::normalize(camera.target - camera.position);
				glm::vec3 right = glm::normalize(glm::cross(forward, camera.up));
				// 水平：绕世界 Y（offset 与 up 同步旋转）
				offset = rotateAroundAxis(offset, -dx * s, glm::vec3(0.0f, 1.0f, 0.0f));
				camera.up = rotateAroundAxis(camera.up, -dx * s, glm::vec3(0.0f, 1.0f, 0.0f));
				// 竖直：绕屏幕右轴（offset 与 up 同步旋转）
				offset = rotateAroundAxis(offset, -dy * s, right);
				camera.up = rotateAroundAxis(camera.up, -dy * s, right);
				camera.position = camera.target + offset;
			}
		}
	}

	camera.pitch = std::clamp(camera.pitch, -kPitchLimit, kPitchLimit);

	if (camera.mode == CameraMode::Fps) {
		float fwd = (state.keyForward ? 1.0f : 0.0f) - (state.keyBack ? 1.0f : 0.0f);
		float strafe = (state.keyRight ? 1.0f : 0.0f) - (state.keyLeft ? 1.0f : 0.0f);
		if (fwd != 0.0f || strafe != 0.0f) {
			camera.position += (cameraForward(camera) * fwd +
					cameraRight(camera) * strafe) * (camera.fpsSpeed * dt);
		}
	} else if (wheel != 0.0f) {
		// 相对缩放（同 three.js OrbitControls：半径 ×(1∓zoom)）——加法式（position +=
		// forward*zoom*step）在远机位失效（cottage 机位半径 ~3535 vs zoomSpeed 0.03 =
		// 每格 0.0009%，2026-08-28 用户反馈"滚轮缩放没用了"）；滚近（y 正）半径缩小
		const float clamped = std::clamp(wheel, -kMaxWheelPerFrame, kMaxWheelPerFrame);
		camera.position = camera.target +
			(camera.position - camera.target) * (1.0f - clamped * camera.zoomSpeed);
	}
}


}  // namespace RGL
