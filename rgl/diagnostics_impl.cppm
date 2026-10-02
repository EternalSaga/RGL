module;
#include <sol/sol.hpp>
#include <fmt/format.h>
#include <SDL3/SDL_keyboard.h>  // camera_probe 单字母键名 → SDLK 键码
// ⚠️ SDL3 的字母键码是 **ASCII 小写连续块**：SDLK_A = 0x61 = 'a'（实测 SDL_keycode.h:108，
// 不是大写 0x41）。旧注释写"= ASCII 大写"是错的，功能却一直正确——因为
// camera_config_impl::keyNameToSdlKey 用 `SDLK_A + (c - 'A')` 把 'A'..'Z' 映射到
// **连续的** 'a'..'z' 块（见该函数注释）。别把那个算式"修正"成 0x41 基准。
module diagnostics:impl;

import diagnostics;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;
import gbuffer_contract;  // kGBufferAttachments（dump_gbuf 越界闸门按表判界）。**刻意不 import
	// render_graph**：表是纯 constexpr 数据，2026-09-30 抽到叶子模块 gbuffer_contract，
	// 避免"配置解析器 → 渲染后端"这条反向依赖边（AGENTS.md 分层原则；也正是它一度
	// 让本模块无法进单元测试——链上它就得链整个 Vulkan 栈）。

namespace RGL {

/** 诊断配置单例存储（进程级，main setDiagnostics 一次，此后只读）。 */
DiagnosticsConfig& diagnostics() {
	static DiagnosticsConfig instance;
	return instance;
}

/** setDiagnostics 定义：整体赋值到单例。 */
void setDiagnostics(const DiagnosticsConfig& cfg) { diagnostics() = cfg; }

/**
 * rpc_port 解析（Phase 10b 批 11b.2）：缺省 / 越界（1..65535 之外）= 8788 回落，
 * 调试面配置笔误不 fail-fast。抽成小函数以守住主解析函数的认知复杂度阈值。
 * @param root 配置根表
 * @return 合法端口
 */
static uint32_t readRpcPort(sol::table& root) {
	sol::optional<double> value = root["rpc_port"];
	if (!value)
		return 8788;
	const auto port = static_cast<int64_t>(*value);
	if (port < 1 || port > 65535)
		return 8788;
	return static_cast<uint32_t>(port);
}

/** 探针表数值字段读取（缺省回落默认；同 picking_probe 解析惯例）。 */
static uint32_t readProbeNum(const sol::table& t, const char* k, uint32_t def) {
	sol::optional<double> v = t[k];
	return v ? static_cast<uint32_t>(*v) : def;
}

/**
 * camera_probe 键名解析（2026-09-28 WASD 回归护栏）：单字母不区分大小写，按 SDL3
 * 字母键码 = ASCII 大写约定映射；非单字母 warn 并忽略（探针是调试面，配置笔误不
 * fail-fast）。
 * @param t camera_probe 子表
 * @param cfg 写入目标（cameraProbeKey）
 */
static void readCameraProbeKey(const sol::table& t, DiagnosticsConfig& cfg) {
	if (sol::optional<std::string> key = t["key"]) {
		const std::string& s = *key;
		const bool isLetter = s.size() == 1 &&
			((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= 'A' && s[0] <= 'Z'));
		if (!isLetter) {
			if (auto* logger = RLLogger::getInstance())
				logger->warn("diagnostics: camera_probe.key 须为单字母（如 \"W\"），已忽略");
		} else {
			const char c = (s[0] >= 'a' && s[0] <= 'z') ? s[0] - 'a' + 'A' : s[0];
			cfg.cameraProbeKey = static_cast<uint32_t>(SDLK_A + (c - 'A'));
		}
	}
}

/**
 * 脚本化探针表解析（ui_click_probe / camera_probe，2026-09-28 WASD 回归护栏）：
 * 缺表 = 全关（字段保持 0）；数值缺省回落默认（holdFrames 缺省 8）；key 解析见
 * readCameraProbeKey。抽成小函数以守住主解析函数认知复杂度阈值。
 * @param root 配置根表
 * @param cfg 写入目标（uiClickProbe* / cameraProbe* 字段）
 */
static void readInputProbes(sol::table& root, DiagnosticsConfig& cfg) {
	if (sol::optional<sol::table> probe = root["ui_click_probe"]) {
		const sol::table& t = *probe;
		cfg.uiClickProbeFrame = readProbeNum(t, "frame", 0);
		cfg.uiClickProbeX = readProbeNum(t, "x", 0);
		cfg.uiClickProbeY = readProbeNum(t, "y", 0);
	}
	if (sol::optional<sol::table> probe = root["camera_probe"]) {
		const sol::table& t = *probe;
		cfg.cameraProbeFrame = readProbeNum(t, "frame", 0);
		cfg.cameraProbeHold = readProbeNum(t, "holdFrames", 8);
		readCameraProbeKey(t, cfg);
	}
}

/** loadDiagnosticsConfig 定义（接口注释见 diagnostics.cppm）。要点：数值门（texel_canary/dump_gbuf/texture_force_size）负数 = "关"哨兵；dump_gbuf 越界（> kGBufferAttachments 最大通道号）warn 一句并置 -1（关，不再静默改判为 0）；解析完成后打一行 info 汇总。 */
[[nodiscard]] std::expected<DiagnosticsConfig, Error> loadDiagnosticsConfig(std::string_view path) {
	auto* logger = RLLogger::getInstance();
	DiagnosticsConfig cfg;

	{
		std::ifstream probe{std::string(path)};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Diagnostics config not found ({}), all off", std::string(path));
			return cfg;
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
			fmt::format("diagnostics config ({}): {}", path, msg)});
	}

	try {
		sol::table root = result[0].as<sol::table>();
		auto readBool = [&](const char* key, bool def) {
			sol::optional<bool> v = root[key];
			return v ? *v : def;
		};

		cfg.noVvl = readBool("no_vvl", cfg.noVvl);
		cfg.noHdr = readBool("no_hdr", cfg.noHdr);
		cfg.noPresent = readBool("no_present", cfg.noPresent);
		cfg.vtxCanary = readBool("vtx_canary", cfg.vtxCanary);
		cfg.styleDist = readBool("style_dist", cfg.styleDist);
		cfg.texDist = readBool("tex_dist", cfg.texDist);
		cfg.textureForceWhite = readBool("texture_force_white", cfg.textureForceWhite);
		cfg.textureForceUnorm = readBool("texture_force_unorm", cfg.textureForceUnorm);
		cfg.dgcStatic = readBool("dgc_static", cfg.dgcStatic);
		cfg.imgui = readBool("imgui", cfg.imgui);
		cfg.configReload = readBool("config_reload", cfg.configReload);
		cfg.shaderReload = readBool("shader_reload", cfg.shaderReload);
		cfg.picking = readBool("picking", cfg.picking);
		cfg.shadowPass = readBool("shadow_pass", cfg.shadowPass);
		cfg.izbShadow = readBool("izb_shadow", cfg.izbShadow);
		// 批 11.5：PCSS（门 = 变体选择，非运行时 flag——slang fp 重排教训同 1e，门关必须原版 SPV）
		cfg.shadowPcss = readBool("shadow_pcss", cfg.shadowPcss);
		auto readF64 = [&](const char* key, double def) {
			sol::optional<double> v = root[key];
			return v ? *v : def;
		};
		cfg.pcssSunDegrees = std::clamp(readF64("pcss_sun_degrees", cfg.pcssSunDegrees), 0.0, 5.0);
		cfg.pcssSearchRadius = std::clamp(readF64("pcss_search_radius", cfg.pcssSearchRadius), 1.0, 64.0);
		cfg.pcssMaxPenumbra = std::clamp(readF64("pcss_max_penumbra", cfg.pcssMaxPenumbra), 1.0, 64.0);
		cfg.rpc = readBool("rpc", cfg.rpc);

		// picking_probe = { frame = 30, x = 640, y = 360 }（脚本化点选：第 frame 帧自动
		// 请求一次 (x,y) 点选并打反查日志——headless 验证 picking 全链，无鼠标依赖）
		if (sol::optional<sol::table> probe = root["picking_probe"]) {
			auto num = [&probe](const char* k, uint32_t def) {
				sol::optional<double> v = (*probe)[k];
				return v ? static_cast<uint32_t>(*v) : def;
			};
			cfg.pickingProbeFrame = num("frame", 0);
			cfg.pickingProbeX = num("x", 0);
			cfg.pickingProbeY = num("y", 0);
		}

		// 脚本化输入探针（窗口模式输入链回归，2026-09-28；见 readInputProbes 注释）
		readInputProbes(root, cfg);

		// 数值门：负数 = 显式"关"哨兵（配置文件写 -1 直观）；dump_gbuf 越界 = warn + 关闭
		auto readI64 = [&](const char* key, int64_t def) -> std::optional<int64_t> {
			sol::optional<double> v = root[key];
			if (!v)
				return def;
			return static_cast<int64_t>(*v);
		};
		const int64_t texel = readI64("texel_canary", -1).value_or(-1);
		cfg.texelCanary = texel >= 0 ? static_cast<uint64_t>(texel) : ~uint64_t{0};
		cfg.textureForceSize = static_cast<uint32_t>(
			std::max<int64_t>(0, readI64("texture_force_size", 0).value_or(0)));
		cfg.dumpGbuf = static_cast<int>(readI64("dump_gbuf", -1).value_or(-1));
		constexpr int kMaxGbufChannel = kGBufferChannelCount - 1;
		if (cfg.dumpGbuf > kMaxGbufChannel) {
			if (logger)
				logger->warn("diagnostics: dump_gbuf={} 超出 G-buffer 通道数（0..{}），已关闭",
					cfg.dumpGbuf, kMaxGbufChannel);
			cfg.dumpGbuf = -1;
		}
		cfg.frameStats = static_cast<uint32_t>(
			std::max<int64_t>(0, readI64("frame_stats", 0).value_or(0)));
		cfg.profiler = static_cast<uint32_t>(
			std::max<int64_t>(0, readI64("profiler", 0).value_or(0)));

		// 批 11.3 IZB 数值门（izb_shadow=true 才有意义；clamp 防手滑建巨型缓冲）
		cfg.izbGrid = static_cast<uint32_t>(std::clamp<int64_t>(
			readI64("izb_grid", cfg.izbGrid).value_or(cfg.izbGrid), 64, 2048));
		cfg.izbDumpFrame = static_cast<uint32_t>(std::max<int64_t>(
			0, readI64("izb_dump_frame", 0).value_or(0)));

		cfg.rpcPort = readRpcPort(root);

		if (auto* lg = RLLogger::getInstance())
			lg->info("diagnostics: no_vvl={} no_present={} vtx_canary={} texel_canary={} "
				"dump_gbuf={} style_dist={} tex_dist={} tex_force(size={} white={} unorm={}) dgc_static={}",
				cfg.noVvl, cfg.noPresent, cfg.vtxCanary,
				cfg.texelCanary == ~uint64_t{0} ? "off" : std::to_string(cfg.texelCanary),
				cfg.dumpGbuf, cfg.styleDist, cfg.texDist,
				cfg.textureForceSize, cfg.textureForceWhite, cfg.textureForceUnorm, cfg.dgcStatic);
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("diagnostics config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
