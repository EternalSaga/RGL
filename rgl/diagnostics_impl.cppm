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
 * @param trace 键轨迹（解析器总是记录，见 loadDiagnosticsConfig）
 * @return 合法端口
 */
static uint32_t readRpcPort(sol::table& root, DiagnosticsKeyTrace& trace) {
	trace.read.emplace_back("rpc_port");
	sol::optional<double> value = root["rpc_port"];
	if (!value)
		return 8788;
	const auto port = static_cast<int64_t>(*value);
	if (port < 1 || port > 65535) {
		if (auto* logger = RLLogger::getInstance())
			logger->warn("diagnostics: rpc_port={} 越界（1..65535），回落 8788", port);
		return 8788;
	}
	return static_cast<uint32_t>(port);
}

/**
 * 探针表数值字段读取（缺省回落默认；同 picking_probe 解析惯例）。
 * @param t 探针子表
 * @param k 字段名
 * @param def 缺省值
 * @param trace 键轨迹
 * @param table 子表名（键轨迹拼写 `表名.字段`）
 */
static uint32_t readProbeNum(const sol::table& t, const char* k, uint32_t def,
	DiagnosticsKeyTrace& trace, std::string_view table) {
	trace.read.push_back(fmt::format("{}.{}", table, k));
	sol::optional<double> v = t[k];
	return v ? static_cast<uint32_t>(*v) : def;
}

/**
 * camera_probe 键名解析（2026-09-28 WASD 回归护栏）：单字母不区分大小写，按 SDL3
 * 字母键码 = ASCII 大写约定映射；非单字母 warn 并忽略（探针是调试面，配置笔误不
 * fail-fast）。
 * @param t camera_probe 子表
 * @param cfg 写入目标（cameraProbeKey）
 * @param trace 键轨迹
 */
static void readCameraProbeKey(const sol::table& t, DiagnosticsConfig& cfg,
	DiagnosticsKeyTrace& trace) {
	trace.read.emplace_back("camera_probe.key");
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
 * @param trace 键轨迹（缺表时其字段不进读集——覆盖率测试因此写全三张表）
 */
static void readInputProbes(sol::table& root, DiagnosticsConfig& cfg,
	DiagnosticsKeyTrace& trace) {
	if (sol::optional<sol::table> probe = root["ui_click_probe"]) {
		const sol::table& t = *probe;
		cfg.uiClickProbeFrame = readProbeNum(t, "frame", 0, trace, "ui_click_probe");
		cfg.uiClickProbeX = readProbeNum(t, "x", 0, trace, "ui_click_probe");
		cfg.uiClickProbeY = readProbeNum(t, "y", 0, trace, "ui_click_probe");
	}
	if (sol::optional<sol::table> probe = root["camera_probe"]) {
		const sol::table& t = *probe;
		cfg.cameraProbeFrame = readProbeNum(t, "frame", 0, trace, "camera_probe");
		cfg.cameraProbeHold = readProbeNum(t, "holdFrames", 8, trace, "camera_probe");
		readCameraProbeKey(t, cfg, trace);
	}
}

/**
 * 标量门读取段（bool + pcss 数值 + rpc_port；2026-10-02 从主函数拆出守认知复杂度
 * 阈值）。每个键都问一遍（缺省回落 cfg 当前值 = 结构体默认），并记键轨迹。
 * @param root 配置根表
 * @param cfg 写入目标
 * @param trace 键轨迹
 */
static void applyScalarGates(sol::table& root, DiagnosticsConfig& cfg,
	DiagnosticsKeyTrace& trace) {
	auto readBool = [&](const char* key, bool def) {
		trace.read.emplace_back(key);
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
	cfg.shadowEvssm = readBool("shadow_evssm", cfg.shadowEvssm);
	auto readF64 = [&](const char* key, double def) {
		trace.read.emplace_back(key);
		sol::optional<double> v = root[key];
		return v ? *v : def;
	};
	cfg.pcssSunDegrees = std::clamp(readF64("pcss_sun_degrees", cfg.pcssSunDegrees), 0.0, 5.0);
	cfg.pcssSearchRadius = std::clamp(readF64("pcss_search_radius", cfg.pcssSearchRadius), 1.0, 64.0);
	cfg.pcssMaxPenumbra = std::clamp(readF64("pcss_max_penumbra", cfg.pcssMaxPenumbra), 1.0, 64.0);
	cfg.rpc = readBool("rpc", cfg.rpc);
	cfg.rpcPort = readRpcPort(root, trace);
}

/**
 * 探针 + 数值门读取段（picking_probe / 输入探针 / texel_canary / dump_gbuf /
 * frame_stats / profiler / IZB 数值；2026-10-02 从主函数拆出守认知复杂度阈值）。
 * 数值门负数 = 显式"关"哨兵（配置文件写 -1 直观）；dump_gbuf 越界 = warn + 关闭。
 * @param root 配置根表
 * @param cfg 写入目标
 * @param trace 键轨迹
 */
static void applyProbeAndNumericGates(sol::table& root, DiagnosticsConfig& cfg,
	DiagnosticsKeyTrace& trace) {
	// picking_probe = { frame = 30, x = 640, y = 360 }（脚本化点选：第 frame 帧自动
	// 请求一次 (x,y) 点选并打反查日志——headless 验证 picking 全链，无鼠标依赖）
	if (sol::optional<sol::table> probe = root["picking_probe"]) {
		const sol::table& t = *probe;
		cfg.pickingProbeFrame = readProbeNum(t, "frame", 0, trace, "picking_probe");
		cfg.pickingProbeX = readProbeNum(t, "x", 0, trace, "picking_probe");
		cfg.pickingProbeY = readProbeNum(t, "y", 0, trace, "picking_probe");
	}
	// 脚本化输入探针（窗口模式输入链回归，2026-09-28；见 readInputProbes 注释）
	readInputProbes(root, cfg, trace);

	auto readI64 = [&](const char* key, int64_t def) -> std::optional<int64_t> {
		trace.read.emplace_back(key);
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
		if (auto* logger = RLLogger::getInstance())
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
}

// ———— 门回显（2026-10-02 派生化，docs/gate_lint_plan.md §3-B）————

namespace {

/** 回显表条目：lua 键名 + 取值器（非捕获 lambda → 函数指针，格式化用 fmt）。 */
struct GateEcho {
	std::string_view key;
	std::string (*value)(const DiagnosticsConfig& cfg);
};

/**
 * 门回显表 = 「门 ↔ lua 键名 ↔ 怎么打印」的单一真相源。此前回显是手写子集
 * （38 门只列 ~11 个，`shadow_pass` 默认开都不出现），漂移靠这里收口：
 * **加门 = 加一行**，回显与 diagnosticsEchoKeys() 自动跟上；忘加的人被
 * tests/test_scene_config.cppm 的覆盖率护栏拦下（解析器读了而表里没有 → 红）。
 * 哨兵值出可读文本：texel_canary MAX = "off"、camera_probe.key 0 = "none"。
 */
const std::array<GateEcho, 38> kGateEcho{{
	{"no_vvl", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.noVvl); }},
	{"no_hdr", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.noHdr); }},
	{"no_present", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.noPresent); }},
	{"vtx_canary", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.vtxCanary); }},
	{"texel_canary", [](const DiagnosticsConfig& c) {
		return c.texelCanary == ~uint64_t{0} ? std::string("off")
		                                     : fmt::format("{}", c.texelCanary); }},
	{"dump_gbuf", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.dumpGbuf); }},
	{"style_dist", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.styleDist); }},
	{"tex_dist", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.texDist); }},
	{"texture_force_size", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.textureForceSize); }},
	{"texture_force_white", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.textureForceWhite); }},
	{"texture_force_unorm", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.textureForceUnorm); }},
	{"dgc_static", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.dgcStatic); }},
	{"frame_stats", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.frameStats); }},
	{"profiler", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.profiler); }},
	{"imgui", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.imgui); }},
	{"config_reload", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.configReload); }},
	{"shader_reload", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.shaderReload); }},
	{"picking", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.picking); }},
	{"izb_shadow", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.izbShadow); }},
	{"izb_grid", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.izbGrid); }},
	{"izb_dump_frame", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.izbDumpFrame); }},
	{"shadow_pass", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.shadowPass); }},
	{"shadow_pcss", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.shadowPcss); }},
	{"shadow_evssm", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.shadowEvssm); }},
	{"pcss_sun_degrees", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.pcssSunDegrees); }},
	{"pcss_search_radius", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.pcssSearchRadius); }},
	{"pcss_max_penumbra", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.pcssMaxPenumbra); }},
	{"rpc", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.rpc); }},
	{"rpc_port", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.rpcPort); }},
	{"picking_probe.frame", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.pickingProbeFrame); }},
	{"picking_probe.x", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.pickingProbeX); }},
	{"picking_probe.y", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.pickingProbeY); }},
	{"ui_click_probe.frame", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.uiClickProbeFrame); }},
	{"ui_click_probe.x", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.uiClickProbeX); }},
	{"ui_click_probe.y", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.uiClickProbeY); }},
	{"camera_probe.frame", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.cameraProbeFrame); }},
	{"camera_probe.key", [](const DiagnosticsConfig& c) {
		return c.cameraProbeKey == 0 ? std::string("none")
		                             : std::string(1, static_cast<char>(c.cameraProbeKey)); }},
	{"camera_probe.holdFrames", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.cameraProbeHold); }},
}};

/**
 * 未知键 lint：文件里出现的顶层键，凡回显表键名的「`.` 前缀」都不等于它 = 解析器没人读它
 * （拼错的键，今天完全静默）。判据来自回显表本身（= 解析器键名真相源），不另立名单——
 * 覆盖率护栏（测试）保证回显表 ⊥ 解析器不会漂移，本函数因此只需查一张表。
 * @param trace 解析期键轨迹
 */
void lintUnknownDiagnosticsKeys(const DiagnosticsKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present) {
		bool known = false;
		for (const auto& e : kGateEcho)
			if (e.key.substr(0, e.key.find('.')) == key) {  // 无 `.` 时 = 全名比对
				known = true;
				break;
			}
		if (!known)
			logger->warn("diagnostics: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
	}
}

}  // namespace

/** diagnosticsEchoKeys 定义（接口注释见 diagnostics.cppm）。 */
std::vector<std::string> diagnosticsEchoKeys() {
	std::vector<std::string> keys;
	keys.reserve(kGateEcho.size());
	for (const auto& e : kGateEcho)
		keys.emplace_back(e.key);
	return keys;
}

/** describeDiagnosticsGates 定义：按**格式化后字符串**比较差集（零类型机制；门全是标量）。 */
std::string describeDiagnosticsGates(const DiagnosticsConfig& cfg,
	const DiagnosticsConfig& defaults) {
	std::string out;
	for (const auto& e : kGateEcho)
		if (const std::string v = e.value(cfg); v != e.value(defaults)) {
			if (!out.empty())
				out += ' ';
			out += fmt::format("{}={}", e.key, v);
		}
	return out;
}

/** describeAllDiagnosticsGates 定义（接口注释见 diagnostics.cppm）。 */
std::string describeAllDiagnosticsGates(const DiagnosticsConfig& cfg) {
	std::string out;
	for (const auto& e : kGateEcho) {
		if (!out.empty())
			out += ' ';
		out += fmt::format("{}={}", e.key, e.value(cfg));
	}
	return out;
}


/** loadDiagnosticsConfig 定义（接口注释见 diagnostics.cppm）。要点：数值门（texel_canary/dump_gbuf/texture_force_size）负数 = "关"哨兵；dump_gbuf 越界（> kGBufferAttachments 最大通道号）warn 一句并置 -1（关，不再静默改判为 0）；解析完成后打门回显（差集 info + 全量 debug）。 */
[[nodiscard]] std::expected<DiagnosticsConfig, Error> loadDiagnosticsConfig(
	std::string_view path, DiagnosticsKeyTrace* trace) {
	auto* logger = RLLogger::getInstance();
	DiagnosticsConfig cfg;
	// 键轨迹**总是**记录（未知键 lint 在启动期就要工作，main 传 nullptr 也不例外）；
	// 调用方给了出参就带出去，没给就落在本地局部量（记录成本 = 38 个字符串，一次）。
	DiagnosticsKeyTrace fallback;
	if (!trace)
		trace = &fallback;

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
		// 「文件里实际出现的顶层键」：子表只记表名（未知键 lint 的输入；
		// 解析器漏读某键不影响这里——present 来自文件本体不来自读取动作）
		for (auto&& kv : root) {
			if (sol::optional<std::string> k = kv.first.as<sol::optional<std::string>>())
				trace->present.push_back(*k);
		}
		applyScalarGates(root, cfg, *trace);
		applyProbeAndNumericGates(root, cfg, *trace);

		// 未知键 lint（普查附带收获：diagnostics.lua 里拼错的键**今天完全静默**）
		lintUnknownDiagnosticsKeys(*trace);

		// 门回显（派生自 kGateEcho，替代此前手写 11 键子集）：info 只列偏离默认值的门，
		// 全量 38 键走 debug（排障时"这台机器的门到底是什么状态"的一行真相）。
		if (auto* lg = RLLogger::getInstance()) {
			const std::string changed = describeDiagnosticsGates(cfg, DiagnosticsConfig{});
			lg->info("diagnostics: {}", changed.empty() ? "all gates default" : changed);
			lg->debug("diagnostics all gates: {}", describeAllDiagnosticsGates(cfg));
		}
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("diagnostics config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
