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
import lua_read;  // 批 0（相对坐标计划）：readVec3 的唯一一份实现（readCameraPlacement 内原本另有一份逐行同构的 lambda）

namespace RGL {

// loadCameraConfig + keyNameToSdlKey 定义（2026-09-02 编译提速 A 方案）：sol2 解析
// 实现整体移出接口 TU，sol/glm/fmt GMF 不再进接口；本单元是叶子边。

namespace {

/**
 * 键表 = 「解析器认识哪些键」的单一真相源（diagnostics 的 kGateEcho 同角色，债-11）：
 * ① 未知键 lint 的判据表；② tests 里「读过的键集合 == 键表键集合」双向覆盖率断言的输入。
 * 子表字段用 `表名.字段`（keys.forward / trackball.sensitivity / fps.speed …）。
 * ⚠ **原生数组**（尺寸自动推导）不是 `std::array<T, N>`——shadow_config_impl 同款教训。
 */
constexpr std::string_view kCameraKeys[]{
	"mode",
	"position",
	"target",
	"fovDegrees",
	"near",
	"far",
	"frame_yaw",
	"trackball.sensitivity",
	"trackball.zoom",
	"fps.speed",
	"fps.sensitivity",
	"keys.forward",
	"keys.back",
	"keys.left",
	"keys.right",
};

/**
 * 键表是否声明了 `块.` 前缀的点分键（= 该块是**固定字段块**，可下钻；债-129 第三刀，
 * runtime_config_impl 同构）。开放 map 段键表只有裸表名 ⟹ 不钻——护栏不许喊错。
 * @param block 顶层键名（候选块名）
 */
bool declaresNestedKeys(std::string_view block) {
	for (const auto& k : kCameraKeys) {
		const auto dot = k.find('.');
		if (dot != std::string_view::npos && k.substr(0, dot) == block)
			return true;
	}
	return false;
}

/**
 * 「文件里实际出现的键」采集（diagnostics 的 collectPresentKeys 同构，未知键 lint
 * 的输入；present 来自文件本体不来自读取动作）。顶层键照旧记；债-129 第三刀：键表声明了
 * `块.` 点分前缀的块（trackball/fps/keys）**下钻一层**，子键以 `块.子键` 追加；
 * 非表值（position/target 是数组形）与数字子键天然不贡献。
 * @param root 配置根表
 * @param trace 键轨迹（present 追加目标）
 */
void collectPresentKeys(const sol::table& root, CameraKeyTrace& trace) {
	for (auto&& kv : root) {
		sol::optional<std::string> k = kv.first.as<sol::optional<std::string>>();
		if (!k)
			continue;
		trace.present.push_back(*k);
		if (!declaresNestedKeys(*k) || kv.second.get_type() != sol::type::table)
			continue;
		const sol::table sub = kv.second.as<sol::table>();
		for (auto&& child : sub)
			if (sol::optional<std::string> ck = child.first.as<sol::optional<std::string>>())
				trace.present.push_back(fmt::format("{}.{}", *k, *ck));
	}
}

/**
 * 单键已知判据（lintUnknownCameraKeys 的判据本体，债-11 + 债-129 第三刀）：**含 `.`**
 * 的键（下钻采集的 `块.子键`）🔴 必须是键表的**精确成员**；**不含 `.`** 的键保持原判据
 * （键表任一键名的「`.` 前缀」等于它即已知）。抽成函数是认知复杂度红线（先例 applyAaKey）。
 * @param key present 里的键名
 */
bool isKnownCameraKey(std::string_view key) {
	const bool dotted = key.find('.') != std::string::npos;
	for (const auto& k : kCameraKeys) {
		const bool hit = dotted ? (k == key)                      // 点分名 = 精确成员
		                        : (k.substr(0, k.find('.')) == key);  // 无 `.` 时 = 全名比对
		if (hit)
			return true;
	}
	return false;
}

/**
 * 未知键 lint（lintUnknownDiagnosticsKeys 同判据，债-11 + 债-129 第三刀）：present 里
 * 每个键过 isKnownCameraKey（顶层 = 前缀判据、点分名 = 精确成员，判据本体见该函数）。
 * 判据来自键表本身（= 解析器键名真相源），不另立名单——覆盖率护栏（测试）保证键表 ⊥
 * 解析器不会漂移，本函数因此只需查一张表。
 * @param trace 解析期键轨迹
 */
void lintUnknownCameraKeys(const CameraKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present)
		if (!isKnownCameraKey(key))
			logger->warn("camera: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
}

}  // namespace

/** cameraConfigKnownKeys 定义（接口注释见 camera_config.cppm）。 */
std::vector<std::string> cameraConfigKnownKeys() {
	std::vector<std::string> keys;
	keys.reserve(std::size(kCameraKeys));
	for (const auto& k : kCameraKeys)
		keys.emplace_back(k);
	return keys;
}

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
 * 键轨迹无条件记 mode（解析器全量问一遍 = 覆盖率护栏的输入）。
 * @param root 配置根表
 * @param camera 写入目标（仅 mode 存在且合法时写）
 * @param trace 键轨迹
 * @return 成功 = void；mode 串非法 = ConfigLoadFailed
 */
[[nodiscard]] std::expected<void, Error> readCameraMode(sol::table root, Camera& camera,
	CameraKeyTrace& trace) {
	trace.read.emplace_back("mode");
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
 * 键轨迹无条件记 position/target（解析器全量问一遍 = 覆盖率护栏的输入）。
 * @param root 配置根表
 * @param camera 写入目标（position/target/pitch/yaw/up）
 * @param trace 键轨迹
 */
void readCameraPlacement(sol::table root, Camera& camera, CameraKeyTrace& trace) {
	trace.read.emplace_back("position");
	trace.read.emplace_back("target");
	// 批 0（相对坐标计划）：这里原本有一份与 scene_config_impl 逐行同构的 readVec3 lambda，
	// 已收敛进 lua_read::readVec3（两份各改一遍 = 亲手制造两份新漂移）。
	// 「缺字段 = 不改」的语义由 fallback = 现值承担（回落时写回同一个值，等价于不写）。
	// position/target 是**绝对层**（dvec3 ↔ dvec3）：Lua 数字原样落 double，不经 float。
	camera.position = readVec3(root, "position", camera.position);
	camera.target = readVec3(root, "target", camera.target);

	// 🔴 朝向反推**委托 cameraAimAt**（债-151，2026-10-26）：本函数与 cameraAimAt 自述是"同一
	// 反推约定"，却各写了一遍减法——两份实现可以静默分家（AGENTS 哲学 §1：像单一来源的副本不是
	// 单一来源），而实测错的正是这里那份。收敛成一条调用后，"两条路径反推结果相同"从注释变成
	// 结构事实。
	// 为什么必须在 double 域相减（cameraAimAt 内部保证）：position/target 都是绝对层的点，各自
	// 先收窄成 float32 再相减 = 用 ULP 0.0625 m（|p| = 1e6 处）的格子量一个几米长的差向量
	// ⟹ 视向偏 0.20°、画面整体平移 ~5 px。🔴 **本债的实测病灶就在这条收窄上，且门关门开同吃**
	// ——收窄发生在配置读取期，那时渲染原点还不存在，rebase 门碰不到它。
	// ⚠ 旧注释写的理由（"先 double 减再收窄会把 float32 的减法舍入丢掉，读数会漂"）是**对的**
	// ——读数确实漂了，三枚截图锚随本刀重立。用"漂一个已知 5 px 的错画面"换"逐位不变"是本债
	// 结案时用户拍掉的账。
	const glm::dvec3 aim = camera.target;  // 先取副本：cameraAimAt 会写 camera.target
	if (glm::length(aim - camera.position) > 1e-6) {
		cameraAimAt(camera, aim);
		// trackball 携带 up 初始化：world-up 对齐（无 roll），与旧闭式 cameraUp 一致
		camera.up = cameraUp(camera);
	}
}

/**
 * fovDegrees/near/far/frame_yaw 标量字段读取（仅 loadCameraConfig 内部使用）：缺字段回落
 * camera 当前值；fov 度转弧度。键轨迹无条件记四枚标量键（解析器全量问一遍 =
 * 覆盖率护栏的输入）。
 * @param root 配置根表
 * @param camera 写入目标（fov/nearPlane/farPlane/frameYaw）
 * @param trace 键轨迹
 */
void readCameraScalars(sol::table root, Camera& camera, CameraKeyTrace& trace) {
	trace.read.emplace_back("fovDegrees");
	trace.read.emplace_back("near");
	trace.read.emplace_back("far");
	trace.read.emplace_back("frame_yaw");
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
 * 键轨迹无条件记四枚字段键（解析器全量问一遍 = 覆盖率护栏的输入）。
 * @param root 配置根表
 * @param camera 写入目标（trackballSensitivity/zoomSpeed/fpsSpeed/fpsSensitivity）
 * @param trace 键轨迹
 */
void readCameraModeParams(sol::table root, Camera& camera, CameraKeyTrace& trace) {
	trace.read.emplace_back("trackball.sensitivity");
	trace.read.emplace_back("trackball.zoom");
	trace.read.emplace_back("fps.speed");
	trace.read.emplace_back("fps.sensitivity");
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

/** loadCameraConfig 定义（接口注释见 camera_config.cppm）。要点：初始朝向由 position→target 反推（yaw/pitch + trackball up 对齐 world-up）；keys 四字段逐一校验，任一未知键名整体失败。键轨迹 + 未知键 lint 同 diagnostics（债-11）。 */
[[nodiscard]] std::expected<CameraConfig, Error> loadCameraConfig(
	std::string_view path, CameraKeyTrace* trace) {
	auto* logger = RLLogger::getInstance();
	// 键轨迹**总是**记录（未知键 lint 在启动期就要工作，main 传 nullptr 也不例外）；
	// 调用方给了出参就带出去，没给就落在本地局部量（diagnostics 同口径）。
	CameraKeyTrace fallback;
	if (!trace)
		trace = &fallback;

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
		collectPresentKeys(root, *trace);

		Camera camera{};
		if (auto mode = readCameraMode(root, camera, *trace); !mode)
			return std::unexpected(std::move(mode).error());

		readCameraPlacement(root, camera, *trace);
		readCameraScalars(root, camera, *trace);
		readCameraModeParams(root, camera, *trace);

		// 键位绑定（可选字段回落默认 WASD；字段存在但键名无法识别 → 报错退出）。
		// keys.* 四键在**表存在性判断之外**记：解析器全量问一遍，缺表也记（覆盖率护栏
		// 的输入与文件内容无关，diagnostics 同口径）。
		trace->read.emplace_back("keys.forward");
		trace->read.emplace_back("keys.back");
		trace->read.emplace_back("keys.left");
		trace->read.emplace_back("keys.right");
		sol::table keysTable = root["keys"];
		if (keysTable.valid() && keysTable.is<sol::table>())
			if (auto keys = readCameraKeys(keysTable, camera, path); !keys)
				return std::unexpected(std::move(keys).error());

		// 未知键 lint（债-11：camera.lua 里拼错的键此前完全静默）
		lintUnknownCameraKeys(*trace);

		return CameraConfig{camera};
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("camera config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
