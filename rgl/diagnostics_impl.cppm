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
 * cullReplayName 定义：三态 ↔ `cull_replay` 配置名的唯一映射（回显与告警共用）。
 * 与 applyCullReplayKey 的解析表**必须同集合**——tests/test_scene_config.cppm 的
 * "回显 = 配置名" 断言钉住这一层（改一边就红）。
 * @param mode 剔除驱动方式
 * @return 配置名
 */
const char* cullReplayName(CullReplayMode mode) {
	switch (mode) {
		case CullReplayMode::cpu: return "cpu";
		case CullReplayMode::gpuLagged: return "gpu_lagged";
		case CullReplayMode::gpuSync: return "gpu_sync";
	}
	return "?";  // 不可达（枚举穷尽）；写出来只为不调用 UB 兜底路径的 printf 风格断言
}

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
 * 标量门读取段（bool + rpc_port；2026-10-02 从主函数拆出守认知复杂度
 * 阈值）。每个键都问一遍（缺省回落 cfg 当前值 = 结构体默认），并记键轨迹。
 * N6b 前这里还读 pcss 三个数值门，随字段一起迁走了。
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
	// N6b：阴影九键（shadow_pass / shadow_pcss / shadow_evssm / pcss_* ×3 / izb_* ×3）
	// 的读取段已随字段一起删除——配置面 = configs/shadow.lua，旧键出现即由
	// checkMigratedShadowKeys 启动失败。
	cfg.rpc = readBool("rpc", cfg.rpc);
	cfg.rpcPort = readRpcPort(root, trace);
	cfg.faultPoll = readBool("fault_poll", cfg.faultPoll);
}

/**
 * `cull_replay` 字符串枚举读取段（债-30 方案 C，docs/cpu_perf_plan.md §4）：三态
 * cpu / gpu_lagged / gpu_sync，**未知值 = 解析期 fail-fast**（present_mode / aa 的既有
 * 纪律：静默回落的失效形态 = 你以为在测 GPU 剔除路，其实跑的是 CPU 路，而两边读数
 * 看起来都"合理"）。抽成函数 = 认知复杂度红线（先例 applyAaKey），也为了让单测单独打它。
 * @param root diagnostics.lua 顶层表
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 present_mode 同口径带配置文件路径）
 * @param trace 键轨迹（缺键也记 = 解析器读过它，覆盖率护栏的输入）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyCullReplayKey(const sol::table& root,
	DiagnosticsConfig& cfg, std::string_view path, DiagnosticsKeyTrace& trace) {
	trace.read.emplace_back("cull_replay");
	sol::optional<std::string> raw = root["cull_replay"];
	if (!raw)
		return std::nullopt;
	if (*raw == "cpu")
		cfg.cullReplay = CullReplayMode::cpu;
	else if (*raw == "gpu_lagged")
		cfg.cullReplay = CullReplayMode::gpuLagged;
	else if (*raw == "gpu_sync")
		cfg.cullReplay = CullReplayMode::gpuSync;
	else
		return fmt::format(
			"diagnostics config ({}): cull_replay must be \"cpu\", \"gpu_lagged\" or \"gpu_sync\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * 旧阴影键 fail-fast（批 N6a，docs/shadow_routes_plan.md §2 红线 2）：这 9 个键的配置面
 * 已迁到 configs/shadow.lua（rgl/shadow_config.cppm），**出现在 diagnostics.lua = 启动
 * 失败并指路新键**，不做静默映射——理由：本项目最怕的失效形态是「配置写了、什么都没
 * 发生」，静默映射正是它的制造机。旧字段本身仍是消费点真相（N6b 才切），值由
 * DiagnosticsConfig 默认值（= 交付形态）提供。
 * @param root diagnostics.lua 顶层表
 * @param path 仅用于错误消息（与 cull_replay 同口径带配置文件路径）
 * @return nullopt = 文件里一个旧键都没有；有值 = 错误（点名旧键 + 新落点）
 */
[[nodiscard]] static std::optional<Error> checkMigratedShadowKeys(const sol::table& root,
	std::string_view path) {
	// 旧键 → 新落点（configs/shadow.lua 内的键路径；指路要指到能直接抄的程度）
	struct MovedKey {
		const char* key;
		const char* target;
	};
	static constexpr std::array<MovedKey, 9> kMoved{{
		{"shadow_pass", "mode = \"csm\"（无影 = \"none\"）"},
		{"shadow_pcss", "mode = \"pcss\""},
		{"shadow_evssm", "mode = \"evssm\""},
		{"pcss_sun_degrees", "pcss.sun_degrees / evssm.sun_degrees（按档分立）"},
		{"pcss_search_radius", "pcss.search_radius / evssm.search_radius（按档分立）"},
		{"pcss_max_penumbra", "pcss.max_penumbra / evssm.max_penumbra（按档分立）"},
		{"izb_shadow", "izb（布尔）或 izb.on"},
		{"izb_grid", "izb.grid"},
		{"izb_dump_frame", "izb.dump_frame"},
	}};
	for (const auto& moved : kMoved) {
		sol::object o = root[moved.key];
		if (o.valid() && o.get_type() != sol::type::lua_nil)
			return Error{ErrorCode::ConfigLoadFailed, fmt::format(
				"diagnostics config ({}): key '{}' has moved to configs/shadow.lua "
				"({}), remove it from diagnostics.lua — see docs/shadow_routes_plan.md",
				path, moved.key, moved.target)};
	}
	return std::nullopt;
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
	// N6b：izb_grid / izb_dump_frame 的读取段已删（配置面 = shadow.lua 的 izb.grid /
	// izb.dump_frame，clamp 也搬去 shadow_config_impl；旧键出现即启动失败）。
}

// ———— 门回显（2026-10-02 派生化，docs/archive/gate_lint_plan.md §3-B）————

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
const std::array<GateEcho, 31> kGateEcho{{
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
	{"cull_replay", [](const DiagnosticsConfig& c) { return std::string(cullReplayName(c.cullReplay)); }},
	{"frame_stats", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.frameStats); }},
	{"profiler", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.profiler); }},
	{"imgui", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.imgui); }},
	{"config_reload", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.configReload); }},
	{"shader_reload", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.shaderReload); }},
	{"picking", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.picking); }},
	// N6b：阴影九行已删（izb_* / shadow_* / pcss_*）——回显面 = configs/shadow.lua 的
	// loader 自己那行（shadow_config_impl 打印 mode / izb.on / 各档数值）。
	{"rpc", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.rpc); }},
	{"rpc_port", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.rpcPort); }},
	{"fault_poll", [](const DiagnosticsConfig& c) { return fmt::format("{}", c.faultPoll); }},
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
 * 回显表是否声明了 `块.` 前缀的点分键（= 该块是**固定字段块**，可下钻；债-129 第三刀，
 * runtime_config_impl 同构）。三张探针表（picking_probe/ui_click_probe/camera_probe）
 * 全部子键都在回显表里 ⟹ 下钻安全；开放 map 段键表只有裸表名 ⟹ 不钻——护栏不许喊错。
 * @param block 顶层键名（候选块名）
 */
bool declaresNestedKeys(std::string_view block) {
	for (const auto& e : kGateEcho) {
		const auto dot = e.key.find('.');
		if (dot != std::string_view::npos && e.key.substr(0, dot) == block)
			return true;
	}
	return false;
}

/**
 * 单键已知判据（lintUnknownDiagnosticsKeys 的判据本体，债-11 + 债-129 第三刀）：
 * **含 `.`** 的键（下钻采集的 `块.子键`）🔴 必须是回显表的**精确成员**；**不含 `.`**
 * 的键保持原判据（回显表任一键名的「`.` 前缀」等于它即已知——探针表名 = 前缀命中）。
 * 抽成函数是认知复杂度红线（先例 applyAaKey）。
 * @param key present 里的键名
 */
bool isKnownDiagnosticsKey(std::string_view key) {
	const bool dotted = key.find('.') != std::string::npos;
	for (const auto& e : kGateEcho) {
		const bool hit = dotted
			? (e.key == key)                               // 点分名 = 精确成员
			: (e.key.substr(0, e.key.find('.')) == key);   // 无 `.` 时 = 全名比对
		if (hit)
			return true;
	}
	return false;
}

/**
 * 未知键 lint（债-11 + 债-129 第三刀）：present 里每个键过 isKnownDiagnosticsKey
 * （顶层 = 前缀判据、点分名 = 精确成员，判据本体见该函数）。判据来自回显表本身
 * （= 解析器键名真相源），不另立名单——覆盖率护栏（测试）保证回显表 ⊥ 解析器不会漂移。
 * @param trace 解析期键轨迹
 */
void lintUnknownDiagnosticsKeys(const DiagnosticsKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present)
		if (!isKnownDiagnosticsKey(key))
			logger->warn("diagnostics: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
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


/**
 * sol2 沙箱执行 + 顶层表提取（从 loadDiagnosticsConfig 拆出，守认知复杂度阈值；
 * 先例 applyAaKey/applyCullReplayKey）。沙箱口径同 camera_config：禁 dofile/loadfile/
 * package/require；脚本错误 = ConfigLoadFailed。
 * @param lua 沙箱状态（**调用方持有**——返回的 root 表引用这台 lua 机）
 * @param path 配置文件路径（错误信息用）
 * @return 顶层表；脚本错误返回 Error
 */
[[nodiscard]] static std::expected<sol::table, Error> doFileRoot(sol::state& lua,
	std::string_view path) {
	auto result = lua.do_file(std::string(path).c_str());
	if (!result.valid()) {
		sol::optional<sol::error> err = result.get<sol::optional<sol::error>>();
		std::string msg = err ? err->what() : std::string("unknown lua error");
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("diagnostics config ({}): {}", path, msg)});
	}
	return result[0].as<sol::table>();
}

/**
 * 「文件里实际出现的键」采集（未知键 lint 的输入；present 来自文件本体不来自读取动作。
 * 从 loadDiagnosticsConfig 拆出，守认知复杂度阈值）。顶层键照旧记；债-129 第三刀：
 * 回显表声明了 `块.` 点分前缀的块（三张探针表）**下钻一层**，子键以 `块.子键` 追加。
 * @param root 配置根表
 * @param trace 键轨迹（present 追加目标）
 */
static void collectPresentKeys(const sol::table& root, DiagnosticsKeyTrace& trace) {
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

/** loadDiagnosticsConfig 定义（接口注释见 diagnostics.cppm）。要点：缺文件 = 全默认 + warn；旧阴影键出现 = 启动失败（checkMigratedShadowKeys）；`mode` 类字符串枚举 fail-fast；数值门负数 = 显式"关"哨兵；dump_gbuf 越界 = warn + 关闭；解析完成后打门回显（差集 info + 全量 debug）。 */
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
	auto root = doFileRoot(lua, path);
	if (!root)
		return std::unexpected(std::move(root).error());

	try {
		sol::table& table = *root;
		collectPresentKeys(table, *trace);
		// 旧阴影键 fail-fast（N6a 红线 2）：出现即启动失败并指路 configs/shadow.lua
		if (auto err = checkMigratedShadowKeys(table, path); err)
			return std::unexpected(std::move(*err));
		applyScalarGates(table, cfg, *trace);
		applyProbeAndNumericGates(table, cfg, *trace);
		// 字符串枚举门（fail-fast：笔误不静默回落，见 applyCullReplayKey 注释）
		if (auto err = applyCullReplayKey(table, cfg, path, *trace); err)
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed, std::move(*err)});

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
