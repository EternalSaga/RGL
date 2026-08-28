module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <sol/sol.hpp>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
export module camera;

import rlstd;
import rllogger;
import vkerror;
import llm_debug;

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

export glm::vec3 cameraUp(const Camera& camera) {
	return glm::vec3(std::sin(camera.yaw) * std::sin(camera.pitch),
		std::cos(camera.pitch),
		-std::cos(camera.yaw) * std::sin(camera.pitch));
}

export glm::mat4 cameraView(const Camera& camera) {
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
			// Trackball：相机绕 target 公转，object 始终居屏中心（旋转 position-target
			// 偏移向量，而不是原地转朝向）。手感约定（同 three.js OrbitControls）：
			// 拖右→object 正面朝右（相机向左公转）；拖下→object 顶部朝向观察者（相机上移）
			glm::vec3 offset = camera.position - camera.target;
			if (glm::length(offset) > 1e-4f) {
				offset = rotateAroundAxis(offset, -dx * camera.trackballSensitivity,
					glm::vec3(0.0f, 1.0f, 0.0f));
				offset = rotateAroundAxis(offset, -dy * camera.trackballSensitivity,
					cameraRight(camera));
				camera.position = camera.target + offset;
				// 由新位置反推 yaw/pitch：forward 恒指向 target
				glm::vec3 forward =
					glm::normalize(camera.target - camera.position);
				camera.pitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
				camera.yaw = std::atan2(-forward.x, -forward.z);
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

// 键名（configs/camera.lua 的 keys 字段值）→ SDL 键码：单字母/数字不区分大小写，
// 方向键用 Up/Down/Left/Right，另有 Space/Enter。无法识别 → nullopt（配置错误，调用方报错退出）
[[nodiscard]] std::optional<uint32_t> keyNameToSdlKey(std::string_view name) {
	if (name.size() == 1) {
		const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
		if (c >= 'A' && c <= 'Z')
			return static_cast<uint32_t>(SDLK_A + (c - 'A'));
		if (c >= '0' && c <= '9')
			return static_cast<uint32_t>(SDLK_0 + (c - '0'));
	}
	if (name == "Up") return static_cast<uint32_t>(SDLK_UP);
	if (name == "Down") return static_cast<uint32_t>(SDLK_DOWN);
	if (name == "Left") return static_cast<uint32_t>(SDLK_LEFT);
	if (name == "Right") return static_cast<uint32_t>(SDLK_RIGHT);
	if (name == "Space") return static_cast<uint32_t>(SDLK_SPACE);
	if (name == "Enter" || name == "Return") return static_cast<uint32_t>(SDLK_RETURN);
	return std::nullopt;
}

// 相机配置包（loadCameraConfig 返回值）：相机姿态 + llm_debug 一次性截图开关。
// llm_debug 的相机位置不走额外字段：agent 直接编辑本配置的 position/target/fovDegrees
// （顶层字段，现有解析），无 presets 层。
export struct CameraConfig {
	Camera camera;
	RGL::LlmDebugConfig llm;
};

// 从 Lua 配置初始化相机（字段可选，缺省回落默认值；fovDegrees 为角度，加载时转弧度；
// keys 字段可选，缺省 WASD，未知键名 → 报错退出；llm_debug 字段可选，false/缺省 = 关闭）
// 文件缺失 → 返回默认 CameraConfig + warn；脚本/类型错误 → std::unexpected
// 沙箱限制与 tools/schema_loader 一致：禁用 dofile/loadfile/package/require
export [[nodiscard]] std::expected<CameraConfig, Error> loadCameraConfig(std::string_view path) {
	auto* logger = RLLogger::getInstance();

	{
		std::string pathStr{std::string(path)};
		std::ifstream probe{pathStr};
		if (!probe.is_open()) {
			if (logger)
			logger->warn("Camera config not found ({}), using defaults",
				std::string(path));
		return CameraConfig{};
		}
	}

	sol::state lua;
	lua.open_libraries(sol::lib::base, sol::lib::table);
	lua["dofile"] = sol::nil;
	lua["loadfile"] = sol::nil;
	lua["package"] = sol::nil;
	lua["require"] = sol::nil;

	auto result = lua.do_file(std::string(path).c_str());
	if (!result.valid()) {
		sol::optional<sol::error> err = result.get<sol::optional<sol::error>>();
		std::string msg = err ? err->what() : std::string("unknown lua error");
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			std::string("camera config (") + std::string(path) + "): " + msg});
	}

	try {
		// 注意：此版本 sol2 的 stack_proxy→sol::table 隐式转换有坑，用 schema_loader 同款 .as<>
		sol::table root = result[0].as<sol::table>();

		Camera camera{};
		sol::optional<std::string> mode = root["mode"];
		if (mode) {
			if (*mode == "fps") {
				camera.mode = CameraMode::Fps;
			} else if (*mode != "trackball") {
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					"camera config: unknown mode '" + *mode + "' (want 'trackball' or 'fps')"});
			}
		}

		auto readVec3 = [&](const char* key) -> std::optional<glm::vec3> {
			sol::table v = root[key];
			if (v.valid() && v.is<sol::table>()) {
				sol::optional<float> x = v[1];
				sol::optional<float> y = v[2];
				sol::optional<float> z = v[3];
				if (x && y && z)
					return glm::vec3(*x, *y, *z);
			}
			return std::nullopt;
		};
		if (auto pos = readVec3("position"))
			camera.position = *pos;
		if (auto tgt = readVec3("target"))
			camera.target = *tgt;

		// 初始朝向：面向 target（与 trackball 拖拽时"forward 恒指向 target"的反推约定一致）。
		// llm_debug 工作流"编辑 position/target 即定机位"依赖此行为——此前朝向固定 yaw=0/-z，
		// 导致只改 position/target 时相机并不看向 target。
		{
			glm::vec3 fwd = camera.target - camera.position;
			if (glm::length(fwd) > 1e-6f) {
				fwd = glm::normalize(fwd);
				camera.pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
				camera.yaw = std::atan2(-fwd.x, -fwd.z);
			}
		}

		sol::optional<float> fov = root["fovDegrees"];
		if (fov)
			camera.fov = *fov * glm::pi<float>() / 180.0f;
		sol::optional<float> nearV = root["near"];
		if (nearV)
			camera.nearPlane = *nearV;
		sol::optional<float> farV = root["far"];
		if (farV)
			camera.farPlane = *farV;

		auto readModeTable = [&](const char* key) -> std::optional<sol::table> {
			sol::table t = root[key];
			if (t.valid() && t.is<sol::table>())
				return t;
			return std::nullopt;
		};
		if (auto tb = readModeTable("trackball")) {
			sol::optional<float> s = (*tb)["sensitivity"];
			if (s)
				camera.trackballSensitivity = *s;
			sol::optional<float> z = (*tb)["zoom"];
			if (z)
				camera.zoomSpeed = *z;
		}
		if (auto fp = readModeTable("fps")) {
			sol::optional<float> s = (*fp)["speed"];
			if (s)
				camera.fpsSpeed = *s;
			sol::optional<float> sens = (*fp)["sensitivity"];
			if (sens)
				camera.fpsSensitivity = *sens;
		}

		// 键位绑定（可选字段回落默认 WASD；字段存在但键名无法识别 → 报错退出）
		sol::table keysTable = root["keys"];
		if (keysTable.valid() && keysTable.is<sol::table>()) {
			auto readKey = [&](const char* field, uint32_t def) -> std::expected<uint32_t, std::string> {
				sol::optional<std::string> v = keysTable[field];
				if (!v)
					return def;
				auto mapped = keyNameToSdlKey(*v);
				if (!mapped)
					return std::unexpected(std::string("unknown key name '") + *v +
						"' (want single letter/digit or Up/Down/Left/Right/Space/Enter)");
				return *mapped;
			};
			if (auto k = readKey("forward", camera.keys.forward))
				camera.keys.forward = *k;
			else
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					std::string("camera config (") + std::string(path) + "): keys.forward: " +
						std::move(k).error()});
			if (auto k = readKey("back", camera.keys.back))
				camera.keys.back = *k;
			else
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					std::string("camera config (") + std::string(path) + "): keys.back: " +
						std::move(k).error()});
			if (auto k = readKey("left", camera.keys.left))
				camera.keys.left = *k;
			else
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					std::string("camera config (") + std::string(path) + "): keys.left: " +
						std::move(k).error()});
			if (auto k = readKey("right", camera.keys.right))
				camera.keys.right = *k;
			else
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					std::string("camera config (") + std::string(path) + "): keys.right: " +
						std::move(k).error()});
		}

		// llm_debug 一次性截图（离屏隐藏窗渲染 → 延迟 N 帧 → 截图 → 退出）：
		//   llm_debug = true/false 纯开关（缺省 false）——截图只改这一行；
		//   llm_shot  = {file, size, delay_frames} 常驻截图配置（全可选回落默认，
		//     改文件名/尺寸只动 llm_shot，开关不变）。
		// 向后兼容：llm_debug 仍接受 table（旧格式 = 启用 + 细字段，覆盖 llm_shot）。
		RGL::LlmDebugConfig llm;
		sol::table shotTable = root["llm_shot"];
		if (shotTable.valid() && shotTable.is<sol::table>()) {
			sol::optional<uint32_t> delayFrames = shotTable["delay_frames"];
			if (delayFrames)
				llm.delayFrames = *delayFrames;
			sol::optional<std::string> file = shotTable["file"];
			if (file)
				llm.file = *file;
			sol::table sizeTable = shotTable["size"];
			if (sizeTable.valid() && sizeTable.is<sol::table>()) {
				sol::optional<uint32_t> w = sizeTable[1];
				if (w)
					llm.width = *w;
				sol::optional<uint32_t> h = sizeTable[2];
				if (h)
					llm.height = *h;
			}
		}
		sol::object llmObj = root["llm_debug"];
		if (llmObj.valid()) {
			if (llmObj.is<bool>()) {
				llm.enabled = llmObj.as<bool>();
			} else if (llmObj.is<sol::table>()) {
				// 旧格式（向后兼容）：llm_debug = table → 启用 + 细字段（覆盖 llm_shot）
				sol::table t = llmObj.as<sol::table>();
				llm.enabled = true;
				sol::optional<uint32_t> delayFrames = t["delay_frames"];
				if (delayFrames)
					llm.delayFrames = *delayFrames;
				sol::optional<std::string> file = t["file"];
				if (file)
					llm.file = *file;
				sol::table sizeTable = t["size"];
				if (sizeTable.valid() && sizeTable.is<sol::table>()) {
					sol::optional<uint32_t> w = sizeTable[1];
					if (w)
						llm.width = *w;
					sol::optional<uint32_t> h = sizeTable[2];
					if (h)
						llm.height = *h;
				}
			} else {
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					std::string("camera config (") + std::string(path) +
						"): llm_debug: want bool or table"});
			}
		}

		return CameraConfig{camera, llm};
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			std::string("camera config (") + std::string(path) + "): " + e.what()});
	}
}

}  // namespace RGL
