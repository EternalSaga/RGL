module;
#include <sol/sol.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <SDL3/SDL_keyboard.h>
#include <fmt/format.h>
module camera_config:impl;

import camera_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;
import camera;

namespace RGL {

// loadCameraConfig + keyNameToSdlKey 定义（2026-09-02 编译提速 A 方案）：sol2 解析
// 实现整体移出接口 TU，sol/glm/fmt GMF 不再进接口；本单元是叶子边。

/**
 * 键名（configs/camera.lua 的 keys 字段值）→ SDL 键码（仅 loadCameraConfig 内部使用）。
 * 单字母/数字不区分大小写（A-Z / 0-9）；方向键用 Up/Down/Left/Right；另有
 * Space/Enter（"Return" 同义）。无法识别 → nullopt（配置错误，调用方报错退出）。
 *
 * ⚠️ **SDL3 的字母键码是 ASCII 小写连续块**：`SDLK_A == 0x61 == 'a'`
 * （SDL_keycode.h:108），`SDLK_Z == 0x7a`。所以下面 `SDLK_A + (c - 'A')` 是把
 * 'A'..'Z' 映射到**连续的** 'a'..'z' 块——这个算式依赖的正是"连续"这条不变量。
 * 数字键同理（`SDLK_1 == 0x31 == '1'`，落在数字自身的 ASCII 位上）。
 * 旧注释曾写"SDL3 字母键码 = ASCII 大写"，与头文件不符，功能却一直对（因为算式
 * 只用到连续性，不用到大小写）。**别把基准改成 0x41**——那会全错。
 *
 * @param name 键名字符串
 * @return SDL 键码；无法识别返回 nullopt
 */
[[nodiscard]] std::optional<uint32_t> keyNameToSdlKey(std::string_view name) {
	if (name.size() == 1) {
		const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
		if (c >= 'A' && c <= 'Z')
			// 依赖 SDLK_A..SDLK_Z 连续（见上方注释）
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

/**
 * keys 键位绑定段读取（仅 loadCameraConfig 内部使用）：四字段逐个可选、缺省回落
 * camera 当前值（默认 WASD）；字段存在但键名无法识别 → ConfigLoadFailed（消息格式
 * 与拆分前的内联实现逐字相同）。
 * @param keysTable keys 子表（调用方保证为表）
 * @param camera 写入目标（字段存在才覆盖对应键）
 * @param path 配置路径（错误信息用）
 * @return 成功 = void；失败 = ConfigLoadFailed
 */
[[nodiscard]] std::expected<void, Error> readCameraKeys(sol::table keysTable,
	Camera& camera, std::string_view path) {
	auto readKey = [&](const char* field, uint32_t def) -> std::expected<uint32_t, std::string> {
		sol::optional<std::string> v = keysTable[field];
		if (!v)
			return def;
		auto mapped = keyNameToSdlKey(*v);
		if (!mapped)
		return std::unexpected(fmt::format(
			"unknown key name '{}' (want single letter/digit or Up/Down/Left/Right/Space/Enter)", *v));
		return *mapped;
	};
	if (auto k = readKey("forward", camera.keys.forward))
		camera.keys.forward = *k;
	else
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("camera config ({}): keys.forward: {}", path, std::move(k).error())});
	if (auto k = readKey("back", camera.keys.back))
		camera.keys.back = *k;
	else
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("camera config ({}): keys.back: {}", path, std::move(k).error())});
	if (auto k = readKey("left", camera.keys.left))
		camera.keys.left = *k;
	else
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("camera config ({}): keys.left: {}", path, std::move(k).error())});
	if (auto k = readKey("right", camera.keys.right))
		camera.keys.right = *k;
	else
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("camera config ({}): keys.right: {}", path, std::move(k).error())});
	return {};
}

/**
 * mode 字段读取（仅 loadCameraConfig 内部使用）：缺失 = 保持默认 trackball；
 * "fps" 切换模式；其余串 = ConfigLoadFailed（消息格式逐字保持拆分前内联实现）。
 * @param root 配置根表
 * @param camera 写入目标（仅 mode 存在且合法时写）
 * @return 成功 = void；mode 串非法 = ConfigLoadFailed
 */
[[nodiscard]] std::expected<void, Error> readCameraMode(sol::table root, Camera& camera) {
	sol::optional<std::string> mode = root["mode"];
	if (mode) {
		if (*mode == "fps") {
			camera.mode = CameraMode::Fps;
		} else if (*mode != "trackball") {
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
				fmt::format("camera config: unknown mode '{}' (want 'trackball' or 'fps')", *mode)});
		}
	}
	return {};
}

/**
 * 初始位置/朝向段读取（仅 loadCameraConfig 内部使用）：position/target 可选字段
 * （缺失回落 camera 当前值）+ 朝向由 position→target 反推（yaw/pitch + trackball up
 * 对齐 world-up）。与 trackball 拖拽时"forward 恒指向 target"的反推约定一致——
 * llm_debug 工作流"编辑 position/target 即定机位"依赖此行为——此前朝向固定
 * yaw=0/-z，导致只改 position/target 时相机并不看向 target。
 * @param root 配置根表
 * @param camera 写入目标（position/target/pitch/yaw/up）
 */
void readCameraPlacement(sol::table root, Camera& camera) {
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

	glm::vec3 fwd = camera.target - camera.position;
	if (glm::length(fwd) > 1e-6f) {
		fwd = glm::normalize(fwd);
		camera.pitch = std::asin(std::clamp(fwd.y, -1.0f, 1.0f));
		camera.yaw = std::atan2(-fwd.x, -fwd.z);
		// trackball 携带 up 初始化：world-up 对齐（无 roll），与旧闭式 cameraUp 一致
		camera.up = cameraUp(camera);
	}
}

/**
 * fovDegrees/near/far/frame_yaw 标量字段读取（仅 loadCameraConfig 内部使用）：缺字段回落
 * camera 当前值；fov 度转弧度。
 * @param root 配置根表
 * @param camera 写入目标（fov/nearPlane/farPlane/frameYaw）
 */
void readCameraScalars(sol::table root, Camera& camera) {
	sol::optional<float> fov = root["fovDegrees"];
	if (fov)
		camera.fov = *fov * glm::pi<float>() / 180.0f;
	sol::optional<float> nearV = root["near"];
	if (nearV)
		camera.nearPlane = *nearV;
	sol::optional<float> farV = root["far"];
	if (farV)
		camera.farPlane = *farV;
	// 帧驱动 yaw（诊断激励门，默认 0 = 关 = 逐位今天；判据本体 = frameDrivenCameraYaw）
	sol::optional<float> frameYaw = root["frame_yaw"];
	if (frameYaw)
		camera.frameYaw = *frameYaw;
}

/**
 * 两模式参数子表读取（仅 loadCameraConfig 内部使用）：trackball.sensitivity/zoom、
 * fps.speed/sensitivity；子表缺失/非表或字段缺失 = 回落 camera 当前值。
 * @param root 配置根表
 * @param camera 写入目标（trackballSensitivity/zoomSpeed/fpsSpeed/fpsSensitivity）
 */
void readCameraModeParams(sol::table root, Camera& camera) {
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
}

/** loadCameraConfig 定义（接口注释见 camera_config.cppm）。要点：初始朝向由 position→target 反推（yaw/pitch + trackball up 对齐 world-up）；keys 四字段逐一校验，任一未知键名整体失败。 */
[[nodiscard]] std::expected<CameraConfig, Error> loadCameraConfig(std::string_view path) {
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
			fmt::format("camera config ({}): {}", path, msg)});
	}

	try {
		// 注意：此版本 sol2 的 stack_proxy→sol::table 隐式转换有坑，用 schema_loader 同款 .as<>
		sol::table root = result[0].as<sol::table>();

		Camera camera{};
		if (auto mode = readCameraMode(root, camera); !mode)
			return std::unexpected(std::move(mode).error());

		readCameraPlacement(root, camera);
		readCameraScalars(root, camera);
		readCameraModeParams(root, camera);

		// 键位绑定（可选字段回落默认 WASD；字段存在但键名无法识别 → 报错退出）
		sol::table keysTable = root["keys"];
		if (keysTable.valid() && keysTable.is<sol::table>())
			if (auto keys = readCameraKeys(keysTable, camera, path); !keys)
				return std::unexpected(std::move(keys).error());

		return CameraConfig{camera};
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("camera config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
