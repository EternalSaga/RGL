module;
#include <sol/sol.hpp>
#include <fmt/format.h>
module shadow_config:impl;

import shadow_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;

namespace RGL {

/**
 * shadowModeName 定义：四态 ↔ `mode` 配置名的唯一映射（回显与告警共用）。
 * 与 parseShadowMode 的解析表**必须同集合**——tests/test_scene_config.cppm 的
 * 「回显 = 配置名」往返断言钉住这一层（改一边就红）。
 * @param mode 阴影路线
 * @return 配置名
 */
const char* shadowModeName(ShadowMode mode) {
	switch (mode) {
		case ShadowMode::none: return "none";
		case ShadowMode::csm: return "csm";
		case ShadowMode::pcss: return "pcss";
		case ShadowMode::evssm: return "evssm";
	}
	return "?";  // 不可达（枚举穷尽）；写出来只为不调用 UB 兜底路径的 printf 风格断言
}

/**
 * parseShadowMode 定义（接口注释见 shadow_config.cppm）。合法值清单与 shadowModeName
 * 同集合；未知值 fail-fast 的理由（静默回落 = "你以为在测 EVSSM 其实跑的是 CSM"）
 * → `ShadowMode` 注释（`cull_replay` 的同一条纪律）。
 * @param name 配置里的字符串值
 * @return 枚举值；非法串返回 ConfigLoadFailed
 */
std::expected<ShadowMode, Error> parseShadowMode(std::string_view name) {
	if (name == "none")
		return ShadowMode::none;
	if (name == "csm")
		return ShadowMode::csm;
	if (name == "pcss")
		return ShadowMode::pcss;
	if (name == "evssm")
		return ShadowMode::evssm;
	return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
		fmt::format("mode must be \"none\", \"csm\", \"pcss\" or \"evssm\", got \"{}\"", name)});
}

namespace {

/** sol::type → 可读类型名（错误消息用；sol2 自带 to_string 不覆盖 type，此处补齐）。 */
const char* solTypeName(sol::type t) {
	switch (t) {
		case sol::type::none: return "none";
		case sol::type::lua_nil: return "nil";
		case sol::type::string: return "string";
		case sol::type::number: return "number";
		case sol::type::thread: return "thread";
		case sol::type::boolean: return "boolean";
		case sol::type::function: return "function";
		case sol::type::userdata: return "userdata";
		case sol::type::lightuserdata: return "lightuserdata";
		case sol::type::table: return "table";
		default: return "other";
	}
}

/**
 * 软影调参块读取（pcss / evssm 两段共用；缺表 = 全默认）。三个数值键**总是**记入键
 * 轨迹（解析器全量问一遍 = 回显覆盖率护栏的输入，同 diagnostics 标量门的做法）；
 * clamp 口径沿用旧 `pcss_*` 三键（sun [0,5]、两半径 [1,64]）。
 * @param root 配置根表
 * @param table 子表名（"pcss" / "evssm"，键轨迹拼写 `表名.字段`）
 * @param into 写入目标（ShadowConfig 的对应调参块）
 * @param trace 键轨迹
 */
void applyTuningBlock(sol::table& root, std::string_view table, ShadowTuning& into,
	ShadowKeyTrace& trace) {
	const sol::optional<sol::table> block = root[std::string(table)];
	auto readF64 = [&](const char* key, double def) {
		trace.read.push_back(fmt::format("{}.{}", table, key));
		if (!block)
			return def;
		sol::optional<double> v = (*block)[key];
		return v ? *v : def;
	};
	into.sunDegrees = std::clamp(readF64("sun_degrees", into.sunDegrees), 0.0, 5.0);
	into.searchRadius = std::clamp(readF64("search_radius", into.searchRadius), 1.0, 64.0);
	into.maxPenumbra = std::clamp(readF64("max_penumbra", into.maxPenumbra), 1.0, 64.0);
}

/**
 * 子表存在性 + 类型闸门：键存在但不是表 = 配置笔误，**报错不静默回落**（本项目最怕
 * 的失效形态 = 「配置写了、什么都没发生」）。
 * @param root 配置根表
 * @param name 子表名（错误信息用）
 * @return 合法（含缺键）= nullopt；有值 = 错误消息
 */
std::optional<std::string> checkTableOrAbsent(const sol::table& root, std::string_view name) {
	sol::object o = root[std::string(name)];
	if (!o.valid() || o.get_type() == sol::type::lua_nil)
		return std::nullopt;
	if (o.get_type() == sol::type::table)
		return std::nullopt;
	return fmt::format("{} must be a table, got {}", name, solTypeName(o.get_type()));
}

/**
 * `izb` 正交修饰读取：两形态都合法（docs/shadow_routes_plan.md §2 的布尔形状 + N6a
 * 收编的 izb_grid/izb_dump_frame 两键 ⟹ 子表形态）：
 *   `izb = true|false`                    = 只开关，grid/dump_frame 保默认；
 *   `izb = { on = true, grid = 512, dump_frame = 0 }` = 三键各可选回落默认。
 * 其他类型 = 报错（不静默）。grid clamp [64, 2048]、dump_frame 负数 = 0（沿用旧键口径）。
 * @param root 配置根表
 * @param cfg 写入目标（izb / izbGrid / izbDumpFrame）
 * @param trace 键轨迹（izb / izb.grid / izb.dump_frame 总是记录）
 * @return 合法 = nullopt；有值 = 错误消息
 */
std::optional<std::string> applyIzbKey(sol::table& root, ShadowConfig& cfg,
	ShadowKeyTrace& trace) {
	trace.read.emplace_back("izb");
	trace.read.emplace_back("izb.grid");
	trace.read.emplace_back("izb.dump_frame");
	sol::object o = root["izb"];
	if (!o.valid() || o.get_type() == sol::type::lua_nil)
		return std::nullopt;
	if (o.get_type() == sol::type::boolean) {
		cfg.izb = o.as<bool>();
		return std::nullopt;
	}
	if (o.get_type() != sol::type::table)
		return fmt::format("izb must be a boolean or a table {{ on, grid, dump_frame }}, got {}",
			solTypeName(o.get_type()));
	const sol::table t = o.as<sol::table>();
	if (sol::optional<bool> on = t["on"])
		cfg.izb = *on;
	sol::optional<double> grid = t["grid"];
	cfg.izbGrid = static_cast<uint32_t>(std::clamp<int64_t>(
		static_cast<int64_t>(grid ? *grid : cfg.izbGrid), 64, 2048));
	sol::optional<double> dump = t["dump_frame"];
	cfg.izbDumpFrame = static_cast<uint32_t>(
		std::max<int64_t>(0, static_cast<int64_t>(dump.value_or(0))));
	return std::nullopt;
}

// ———— 门回显（kGateEcho 同派生化，docs/gate_lint_plan.md §3-B）————

/** 回显表条目：lua 键名 + 取值器（非捕获 lambda → 函数指针，格式化用 fmt）。 */
struct ShadowGateEcho {
	std::string_view key;
	std::string (*value)(const ShadowConfig& cfg);
};

/**
 * 门回显表 = 「阴影门 ↔ lua 键名 ↔ 怎么打印」的单一真相源（diagnostics 的 kGateEcho
 * 同构）：**加门 = 加一行**，回显与 shadowConfigEchoKeys() 自动跟上；忘加的人被
 * tests/test_scene_config.cppm 的覆盖率护栏拦下。`csm` 是定案形状里的**预留空块**
 * （CSM 档暂无调参键），回显恒 "reserved"（差集语义下永不进 info 行）。
 */
const std::array<ShadowGateEcho, 11> kShadowEcho{{
	{"mode", [](const ShadowConfig& c) { return std::string(shadowModeName(c.mode)); }},
	{"izb", [](const ShadowConfig& c) { return fmt::format("{}", c.izb); }},
	{"izb.grid", [](const ShadowConfig& c) { return fmt::format("{}", c.izbGrid); }},
	{"izb.dump_frame", [](const ShadowConfig& c) { return fmt::format("{}", c.izbDumpFrame); }},
	{"csm", [](const ShadowConfig&) { return std::string("reserved"); }},
	{"pcss.sun_degrees", [](const ShadowConfig& c) { return fmt::format("{}", c.pcss.sunDegrees); }},
	{"pcss.search_radius", [](const ShadowConfig& c) { return fmt::format("{}", c.pcss.searchRadius); }},
	{"pcss.max_penumbra", [](const ShadowConfig& c) { return fmt::format("{}", c.pcss.maxPenumbra); }},
	{"evssm.sun_degrees", [](const ShadowConfig& c) { return fmt::format("{}", c.evssm.sunDegrees); }},
	{"evssm.search_radius", [](const ShadowConfig& c) { return fmt::format("{}", c.evssm.searchRadius); }},
	{"evssm.max_penumbra", [](const ShadowConfig& c) { return fmt::format("{}", c.evssm.maxPenumbra); }},
}};

/**
 * 未知键 lint（lintUnknownDiagnosticsKeys 同判据）：文件里出现的顶层键，凡回显表键名的
 * 「`.` 前缀」都不等于它 = 解析器没人读它（拼错的键）。判据来自回显表本身，不另立名单。
 * @param trace 解析期键轨迹
 */
void lintUnknownShadowKeys(const ShadowKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present) {
		bool known = false;
		for (const auto& e : kShadowEcho)
			if (e.key.substr(0, e.key.find('.')) == key) {  // 无 `.` 时 = 全名比对
				known = true;
				break;
			}
		if (!known)
			logger->warn("shadow: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
	}
}

/**
 * sol2 沙箱执行 + 顶层表提取（loadDiagnosticsConfig 的 doFileRoot 同构，前缀换
 * "shadow config"；沙箱口径同 camera_config：禁 dofile/loadfile/package/require）。
 * @param lua 沙箱状态（**调用方持有**——返回的 root 表引用这台 lua 机）
 * @param path 配置文件路径（错误信息用）
 * @return 顶层表；脚本错误返回 Error
 */
[[nodiscard]] std::expected<sol::table, Error> doFileRoot(sol::state& lua, std::string_view path) {
	auto result = lua.do_file(std::string(path).c_str());
	if (!result.valid()) {
		sol::optional<sol::error> err = result.get<sol::optional<sol::error>>();
		std::string msg = err ? err->what() : std::string("unknown lua error");
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("shadow config ({}): {}", path, msg)});
	}
	return result[0].as<sol::table>();
}

/**
 * 「文件里实际出现的顶层键」采集（diagnostics 同构；未知键 lint 的输入，子表只记表名）。
 * @param root 配置根表
 * @param trace 键轨迹（present 追加目标）
 */
void collectPresentKeys(const sol::table& root, ShadowKeyTrace& trace) {
	for (auto&& kv : root) {
		if (sol::optional<std::string> k = kv.first.as<sol::optional<std::string>>())
			trace.present.push_back(*k);
	}
}

/**
 * 门回显打印（从 loadShadowConfig 拆出，守认知复杂度阈值）：路线行恒打（"这台机器跑的
 * 哪条阴影路"是排障第一问），差集 info（全默认不喊第二行），全量走 debug。
 * @param cfg 解析后的配置
 */
void logShadowGateEcho(const ShadowConfig& cfg) {
	auto* lg = RLLogger::getInstance();
	if (!lg)
		return;
	lg->info("shadow: mode={} izb={}", shadowModeName(cfg.mode), cfg.izb);
	if (const std::string changed = describeShadowGates(cfg, ShadowConfig{}); !changed.empty())
		lg->info("shadow: {}", changed);
	lg->debug("shadow all gates: {}", describeAllShadowGates(cfg));
}

}  // namespace

/** shadowConfigEchoKeys 定义（接口注释见 shadow_config.cppm）。 */
std::vector<std::string> shadowConfigEchoKeys() {
	std::vector<std::string> keys;
	keys.reserve(kShadowEcho.size());
	for (const auto& e : kShadowEcho)
		keys.emplace_back(e.key);
	return keys;
}

/** describeShadowGates 定义：按**格式化后字符串**比较差集（零类型机制；门全是标量）。 */
std::string describeShadowGates(const ShadowConfig& cfg, const ShadowConfig& defaults) {
	std::string out;
	for (const auto& e : kShadowEcho)
		if (const std::string v = e.value(cfg); v != e.value(defaults)) {
			if (!out.empty())
				out += ' ';
			out += fmt::format("{}={}", e.key, v);
		}
	return out;
}

/** describeAllShadowGates 定义（接口注释见 shadow_config.cppm）。 */
std::string describeAllShadowGates(const ShadowConfig& cfg) {
	std::string out;
	for (const auto& e : kShadowEcho) {
		if (!out.empty())
			out += ' ';
		out += fmt::format("{}={}", e.key, e.value(cfg));
	}
	return out;
}

/** loadShadowConfig 定义（接口注释见 shadow_config.cppm）。要点：缺文件 = 交付默认 + warn；`mode` 非法值 = fail-fast 并列出合法值；izb 两形态；解析完成后打回显（路线行 info 恒打 + 差集 info + 全量 debug）。 */
[[nodiscard]] std::expected<ShadowConfig, Error> loadShadowConfig(
	std::string_view path, ShadowKeyTrace* trace) {
	auto* logger = RLLogger::getInstance();
	ShadowConfig cfg;
	// 键轨迹**总是**记录（未知键 lint 在启动期就要工作，main 传 nullptr 也不例外）；
	// 调用方给了出参就带出去，没给就落在本地局部量。
	ShadowKeyTrace fallback;
	if (!trace)
		trace = &fallback;

	{
		std::ifstream probe{std::string(path)};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Shadow config not found ({}), defaults mode=csm izb=true",
					std::string(path));
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
	// 子表类型闸门覆盖的三张表（csm = 定案形状里的预留空块，无调参键）
	constexpr std::array<const char*, 3> kSubTables{{"csm", "pcss", "evssm"}};

	try {
		sol::table& table = *root;
		collectPresentKeys(table, *trace);

		// mode：字符串枚举门（fail-fast：笔误不静默回落，见 ShadowMode 注释）
		trace->read.emplace_back("mode");
		if (sol::optional<std::string> rawMode = table["mode"]; rawMode) {
			auto mode = parseShadowMode(*rawMode);
			if (!mode)
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					fmt::format("shadow config ({}): {}", path, mode.error().message)});
			cfg.mode = *mode;
		}

		if (auto err = applyIzbKey(table, cfg, *trace); err)
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
				fmt::format("shadow config ({}): {}", path, *err)});
		// csm/pcss/evssm：存在必须是表（类型错 = 报错不静默）。csm = 预留空块：只记读集
		// 不取值（pcss/evssm 的读集 = 各自三个字段键，表名本身不重复记——覆盖率护栏
		// 断言 read 集合 == 回显表键集合）。
		trace->read.emplace_back("csm");
		for (const char* sub : kSubTables) {
			if (auto err = checkTableOrAbsent(table, sub); err)
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					fmt::format("shadow config ({}): {}", path, *err)});
		}
		applyTuningBlock(table, "pcss", cfg.pcss, *trace);
		applyTuningBlock(table, "evssm", cfg.evssm, *trace);

		lintUnknownShadowKeys(*trace);

		logShadowGateEcho(cfg);  // 路线行 info 恒打 + 差集 info + 全量 debug（见 helper）
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("shadow config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
