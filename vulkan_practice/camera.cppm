module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <sol/sol.hpp>
export module camera;

import rlstd;
import rllogger;
import vkerror;

namespace RGL {

export enum class CameraMode {
	Trackball,
	Fps,
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
};

// 原始输入状态：eventSystem（中央轮询）写入，cameraSystem 每帧消费并重置累积器
export struct CameraInput {
	bool forward = false;
	bool back = false;
	bool left = false;
	bool right = false;
	bool mouseLeftDown = false;
	bool mouseRightDown = false;
	glm::vec2 mouseDelta{0.0f};
	float wheelDelta = 0.0f;
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
	auto& input = rg.ctx().get<CameraInput>();
	auto* logger = RLLogger::getInstance();

	bool dragging = (camera.mode == CameraMode::Fps && input.mouseRightDown) ||
		(camera.mode == CameraMode::Trackball && input.mouseLeftDown);
	if (dragging) {
		float dx = std::clamp(input.mouseDelta.x, -kMaxMouseDeltaPerFrame,
			kMaxMouseDeltaPerFrame);
		float dy = std::clamp(input.mouseDelta.y, -kMaxMouseDeltaPerFrame,
			kMaxMouseDeltaPerFrame);
		if (dx == kMaxMouseDeltaPerFrame || dx == -kMaxMouseDeltaPerFrame ||
			dy == kMaxMouseDeltaPerFrame || dy == -kMaxMouseDeltaPerFrame) {
			if (logger)
				logger->debug("camera: clamped huge mouse delta ({}, {}) -> ({}, {})",
					input.mouseDelta.x, input.mouseDelta.y, dx, dy);
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
		float fwd = (input.forward ? 1.0f : 0.0f) - (input.back ? 1.0f : 0.0f);
		float strafe = (input.right ? 1.0f : 0.0f) - (input.left ? 1.0f : 0.0f);
		if (fwd != 0.0f || strafe != 0.0f) {
			camera.position += (cameraForward(camera) * fwd +
					cameraRight(camera) * strafe) * (camera.fpsSpeed * dt);
		}
	} else if (input.wheelDelta != 0.0f) {
		float wheel = std::clamp(input.wheelDelta, -kMaxWheelPerFrame, kMaxWheelPerFrame);
		camera.position += cameraForward(camera) * (wheel * camera.zoomSpeed);
	}

	input.mouseDelta = glm::vec2(0.0f);
	input.wheelDelta = 0.0f;
}

// 从 Lua 配置初始化相机（字段可选，缺省回落默认值；fovDegrees 为角度，加载时转弧度）
// 文件缺失 → 返回默认 Camera + warn；脚本/类型错误 → std::unexpected
// 沙箱限制与 tools/schema_loader 一致：禁用 dofile/loadfile/package/require
export [[nodiscard]] std::expected<Camera, Error> loadCameraConfig(std::string_view path) {
	auto* logger = RLLogger::getInstance();

	{
		std::string pathStr{std::string(path)};
		std::ifstream probe{pathStr};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Camera config not found ({}), using defaults",
					std::string(path));
			return Camera{};
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

		return camera;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			std::string("camera config (") + std::string(path) + "): " + e.what()});
	}
}

}  // namespace RGL
