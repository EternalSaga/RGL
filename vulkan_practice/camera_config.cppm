module;
#include <sol/sol.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <SDL3/SDL_keyboard.h>
#include <fmt/format.h>
export module camera_config;

import rlstd;
import rllogger;
import vkerror;
import camera;

namespace RGL {

// 一次性截图配置（configs/camera.lua）：
//   llm_debug = true/false 纯开关（缺省 false，交互模式零变化）——截图只改这一行；
//   llm_shot  = {file, size, delay_frames} 常驻截图配置（全可选回落默认）。
// 向后兼容：llm_debug 仍接受 table（旧格式 = 启用 + 细字段，覆盖 llm_shot）。
// 相机位置不走额外字段——agent 直接编辑 camera.lua 顶层的
// position/target/fovDegrees（loadCameraConfig 现有解析），无 presets 层。
// 2026-09-02 从 llm_debug 移入：配置是配置（camera_config 解析产出），执行是 llm_debug
//（captureFrame）——拆开后 camera_config/ecs 不再 import llm_debug（依赖图假边，
// llm_debug 拖 vk_renderer 全链，不该在 camera/ecs 的编译前置里）。
export struct LlmDebugConfig {
	bool enabled = false;
	uint32_t delayFrames = 0;  // 第 N 帧渲染完成后截图（0 = 首帧；为动态场景预留）
	std::string file;          // PNG 路径（相对 CWD）；空 = screenshots/llm_<时间戳>.png
	uint32_t width = 1280;    // llm_debug 模式的离屏窗口尺寸（隐藏窗，永不显示）
	uint32_t height = 720;
};

// 相机配置包（loadCameraConfig 返回值）：相机姿态 + llm_debug 一次性截图开关。
// llm_debug 的相机位置不走额外字段：agent 直接编辑本配置的 position/target/fovDegrees
// （顶层字段，现有解析），无 presets 层。
// 2026-09-02 拆自 camera：配置加载（sol2 沙箱 + 键名映射 + llm 段解析）与运行时
// cameraSystem 解耦——camera 模块不再依赖 llm_debug（原 import 仅为 LlmDebugConfig
// 类型，依赖图假边，编译串行链上的无谓环节）。
export struct CameraConfig {
	Camera camera;
	RGL::LlmDebugConfig llm;
};

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
			fmt::format("camera config ({}): {}", path, msg)});
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
					fmt::format("camera config: unknown mode '{}' (want 'trackball' or 'fps')", *mode)});
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
					fmt::format("camera config ({}): llm_debug: want bool or table", path)});
			}
		}

		return CameraConfig{camera, llm};
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("camera config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
