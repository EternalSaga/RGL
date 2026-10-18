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
 * shadowFormPlan 定义（接口注释见 shadow_config.cppm）：**形态决策表的唯一实现体**
 * （docs/shadow_routes_plan.md §2 红线 4——表只住这里，文档与注释都不抄映射）。
 *
 * 逐条与改造前的消费点判据等价：
 * - `drawAtlas` = 旧 `diagnostics().shadowPass`：none 之外三档都要图首画投影体（pcss/evssm
 *   的软影全部建立在同一张 CSM 图集上，旧世界它们也必然与 shadow_pass=true 同开）。
 * - `spvSuffix` = 旧 `softShadowVariantSpec` 的「shadowEvssm 优先、shadowPcss 次之」让位链。
 *   枚举下两门同开表达不出来 ⟹ 让位链不是被修好，是被**消掉**（本函数没有优先级分支）。
 * - `buildEvssmMoments` = 旧 `shadowEvssm && shadowPass`：枚举下 mode==evssm 天然蕴含
 *   drawAtlas，故不必再掺第二个判据（与旧判据逐档等价）。
 * - `consumeIzbMask` = 旧 `izbShadow` **单独一个判据**：⚠ 刻意不掺 drawAtlas——IZB 的构造
 *   判据今天就只有 izbShadow（`IzbShadow::recordFrame` 无条件跑清零是债-32 的收口，掺了
 *   就把它拆了）。「izb × mode=none 必然一帧阴影也不产」是 gate_lint 的 error，不是这里的
 *   运行期早退——两者判据同形但职责不同（lint 喊人，运行期不改形态）。
 *
 * @param cfg 阴影配置（返回值引用其调参块，见接口生命周期条）
 * @return 本形态的四件事 + 本档调参
 */
ShadowFormPlan shadowFormPlan(const ShadowConfig& cfg) {
	std::string_view suffix;
	switch (cfg.mode) {
		case ShadowMode::none: break;  // 原版 SPV（门关红线：零后缀 = 零变体）
		case ShadowMode::csm: break;   // 同上：CSM 就是原版 4 抽头 PCF
		case ShadowMode::pcss: suffix = "_PCSS_SOFT"; break;
		case ShadowMode::evssm: suffix = "_EVSSM_SOFT"; break;
	}
	return ShadowFormPlan{
		.drawAtlas = cfg.mode != ShadowMode::none,
		.spvSuffix = suffix,
		.buildEvssmMoments = cfg.mode == ShadowMode::evssm,
		.consumeIzbMask = cfg.izb,
	};
}

/**
 * shadowConfig 定义：单例存储（进程级，main setShadowConfig 一次，此后只读）。
 * 默认构造 = 交付形态（mode=csm、izb=false，2026-10-10 换红线）⟹ 未接线的调用方（如
 * `just test` 里不加载配置的用例）拿到的就是出厂形态，与改造前 `diagnostics()` 的旧字段
 * 默认值同形。
 */
ShadowConfig& shadowConfig() {
	static ShadowConfig instance;
	return instance;
}

/** setShadowConfig 定义：整体赋值到单例。 */
void setShadowConfig(const ShadowConfig& cfg) { shadowConfig() = cfg; }

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
	// 核宽下限（批 N6d）：**0 = 不设下限**（回退到"半影多窄就多窄" = 单纹素比较的形态）
	into.minFootprintTexels = std::clamp(readF64("min_footprint_texels", into.minFootprintTexels), 0.0, 64.0);
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
 * `izb` 正交修饰读取：**只收子表形态**（债-129 第三刀护栏照出扁平布尔 = 「扁平键双读
 * 兼容 = 两个真相源」红线的活体违反，2026-10-17 主 agent 裁硬切；docs/shadow_routes_plan.md
 * §2 的布尔形状与 N6a 收编的子表形态不再并存）：
 *   `izb = { on = true, grid = 512, dump_frame = 0 }` = 三键各可选回落默认。
 * 扁平布尔与其他非表类型 = 报错（不静默）。grid clamp [64, 2048]、dump_frame 负数 = 0
 * （沿用旧键口径）。
 * @param root 配置根表
 * @param cfg 写入目标（izb / izbGrid / izbDumpFrame）
 * @param trace 键轨迹（izb.on / izb.grid / izb.dump_frame 总是记录）
 * @return 合法 = nullopt；有值 = 错误消息
 */
std::optional<std::string> applyIzbKey(sol::table& root, ShadowConfig& cfg,
	ShadowKeyTrace& trace) {
	trace.read.emplace_back("izb.on");
	trace.read.emplace_back("izb.grid");
	trace.read.emplace_back("izb.dump_frame");
	sol::object o = root["izb"];
	if (!o.valid() || o.get_type() == sol::type::lua_nil)
		return std::nullopt;
	if (o.get_type() != sol::type::table)
		return fmt::format("izb must be a table {{ on, grid, dump_frame }}, got {}",
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

// ———— 门回显（kGateEcho 同派生化，docs/archive/gate_lint_plan.md §3-B）————

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
// ⚠ **原生数组**（尺寸自动推导），不是 `std::array<T, N>`：N 写错时 std::array 不报错，
// 而是把剩余元素**零填充** ⟹ 空键名 + **空调用 std::function = SIGSEGV**（实测踩过：删两个
// 调参键忘改 N，症状是测试段错误而不是编译错，排查成本远高于本行注释）。
const ShadowGateEcho kShadowEcho[]{
	{"mode", [](const ShadowConfig& c) { return std::string(shadowModeName(c.mode)); }},
	{"izb.on", [](const ShadowConfig& c) { return fmt::format("{}", c.izb); }},
	{"izb.grid", [](const ShadowConfig& c) { return fmt::format("{}", c.izbGrid); }},
	{"izb.dump_frame", [](const ShadowConfig& c) { return fmt::format("{}", c.izbDumpFrame); }},
	{"csm", [](const ShadowConfig&) { return std::string("reserved"); }},
	{"pcss.sun_degrees", [](const ShadowConfig& c) { return fmt::format("{}", c.pcss.sunDegrees); }},
	{"pcss.search_radius", [](const ShadowConfig& c) { return fmt::format("{}", c.pcss.searchRadius); }},
	{"pcss.max_penumbra", [](const ShadowConfig& c) { return fmt::format("{}", c.pcss.maxPenumbra); }},
	{"pcss.min_footprint_texels",
		[](const ShadowConfig& c) { return fmt::format("{}", c.pcss.minFootprintTexels); }},
	{"evssm.sun_degrees", [](const ShadowConfig& c) { return fmt::format("{}", c.evssm.sunDegrees); }},
	{"evssm.search_radius", [](const ShadowConfig& c) { return fmt::format("{}", c.evssm.searchRadius); }},
	{"evssm.max_penumbra", [](const ShadowConfig& c) { return fmt::format("{}", c.evssm.maxPenumbra); }},
	{"evssm.min_footprint_texels",
		[](const ShadowConfig& c) { return fmt::format("{}", c.evssm.minFootprintTexels); }},
};

/**
 * 回显表是否声明了 `块.` 前缀的点分键（= 该块是**固定字段块**，可下钻；债-129 第三刀，
 * runtime_config_impl 同构）。izb 块的 `on` 子键自硬切起**就是**回显成员（回显/门名 =
 * `izb.on`，扁平布尔双读已删——「扁平键双读 = 两个真相源」红线的活体违反，2026-10-17
 * 主 agent 裁）。`csm` 是预留空块（键表无 `csm.` 声明）⟹ 不钻。已知存量：`scene.lua`
 * 的 `taa.enabled`（键随批 14.6 已删，env_band / nm_rig 两靶残留）会被本护栏 warn——
 * 已知存量，待裁决，见 docs/archive/配置正交性_plan.md；为凑「存量零告警」删靶或改 `aa` 是禁手。
 * @param block 顶层键名（候选块名）
 */
bool declaresNestedKeys(std::string_view block) {
	for (const auto& e : kShadowEcho) {
		const auto dot = e.key.find('.');
		if (dot != std::string_view::npos && e.key.substr(0, dot) == block)
			return true;
	}
	return false;
}

/**
 * 单键已知判据（lintUnknownShadowKeys 的判据本体，债-129 第三刀）：**含 `.`** 的键
 * （下钻采集的 `块.子键`）🔴 必须是回显表的**精确成员**；**不含 `.`** 的键保持原判据
 * （回显表任一键名的「`.` 前缀」等于它即已知）。抽成函数是认知复杂度红线（先例 applyAaKey）。
 * @param key present 里的键名
 */
bool isKnownShadowKey(std::string_view key) {
	const bool dotted = key.find('.') != std::string::npos;
	for (const auto& e : kShadowEcho) {
		const bool hit = dotted
			? (e.key == key)                               // 点分名 = 精确成员
			: (e.key.substr(0, e.key.find('.')) == key);   // 无 `.` 时 = 全名比对
		if (hit)
			return true;
	}
	return false;
}

/**
 * 未知键 lint（lintUnknownDiagnosticsKeys 同判据 + 债-129 第三刀）：present 里每个键过
 * isKnownShadowKey（顶层 = 前缀判据、点分名 = 精确成员，判据本体见该函数）。判据来自
 * 回显表本身，不另立名单。
 * @param trace 解析期键轨迹
 */
void lintUnknownShadowKeys(const ShadowKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present)
		if (!isKnownShadowKey(key))
			logger->warn("shadow: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
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
 * 「文件里实际出现的键」采集（diagnostics 同构；未知键 lint 的输入）。顶层键照旧记；
 * 债-129 第三刀：回显表声明了 `块.` 点分前缀的块（izb/pcss/evssm）**下钻一层**，
 * 子键以 `块.子键` 追加；非表值不钻（izb 扁平布尔由 applyIzbKey 的类型闸门报错）、
 * csm 预留块不钻（判据见 declaresNestedKeys）。
 * @param root 配置根表
 * @param trace 键轨迹（present 追加目标）
 */
void collectPresentKeys(const sol::table& root, ShadowKeyTrace& trace) {
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
 * 门回显打印（从 loadShadowConfig 拆出，守认知复杂度阈值）：路线行恒打（"这台机器跑的
 * 哪条阴影路"是排障第一问），差集 info（全默认不喊第二行），全量走 debug。
 * @param cfg 解析后的配置
 */
void logShadowGateEcho(const ShadowConfig& cfg) {
	auto* lg = RLLogger::getInstance();
	if (!lg)
		return;
	lg->info("shadow: mode={} izb.on={}", shadowModeName(cfg.mode), cfg.izb);
	if (const std::string changed = describeShadowGates(cfg, ShadowConfig{}); !changed.empty())
		lg->info("shadow: {}", changed);
	lg->debug("shadow all gates: {}", describeAllShadowGates(cfg));
}

}  // namespace

/** shadowConfigEchoKeys 定义（接口注释见 shadow_config.cppm）。 */
std::vector<std::string> shadowConfigEchoKeys() {
	std::vector<std::string> keys;
	keys.reserve(std::size(kShadowEcho));
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

/** loadShadowConfig 定义（接口注释见 shadow_config.cppm）。要点：缺文件 = 交付默认 + warn；`mode` 非法值 = fail-fast 并列出合法值；izb 只收子表形态（扁平布尔 = 报错，硬切 2026-10-17）；解析完成后打回显（路线行 info 恒打 + 差集 info + 全量 debug）。 */
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
				logger->warn("Shadow config not found ({}), defaults mode=csm izb.on=false",
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
	auto rootObj = doFileRoot(lua, path);
	if (!rootObj)
		return std::unexpected(std::move(rootObj).error());
	// 子表类型闸门覆盖的三张表（csm = 定案形状里的预留空块，无调参键）
	constexpr std::array<const char*, 3> kSubTables{{"csm", "pcss", "evssm"}};

	try {
		// 局部量**必须**叫 `root`：imgqa 的键名校验器（tools/imgqa/engine.cpp 的
		// knownRootKeys）按 `root["键"]` 形态从本文件抽「解析器认识的键」，改名 = 抽不到
		// `mode` = 校验器反过来拒绝合法生成的 shadow.lua（自检失效比没校验更糟）。
		sol::table& root = *rootObj;
		collectPresentKeys(root, *trace);

		// mode：字符串枚举门（fail-fast：笔误不静默回落，见 ShadowMode 注释）
		trace->read.emplace_back("mode");
		if (sol::optional<std::string> rawMode = root["mode"]; rawMode) {
			auto mode = parseShadowMode(*rawMode);
			if (!mode)
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					fmt::format("shadow config ({}): {}", path, mode.error().message)});
			cfg.mode = *mode;
		}

		if (auto err = applyIzbKey(root, cfg, *trace); err)
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
				fmt::format("shadow config ({}): {}", path, *err)});
		// csm/pcss/evssm：存在必须是表（类型错 = 报错不静默）。csm = 预留空块：只记读集
		// 不取值（pcss/evssm 的读集 = 各自三个字段键，表名本身不重复记——覆盖率护栏
		// 断言 read 集合 == 回显表键集合）。
		trace->read.emplace_back("csm");
		for (const char* sub : kSubTables) {
			if (auto err = checkTableOrAbsent(root, sub); err)
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					fmt::format("shadow config ({}): {}", path, *err)});
		}
		applyTuningBlock(root, "pcss", cfg.pcss, *trace);
		applyTuningBlock(root, "evssm", cfg.evssm, *trace);

		lintUnknownShadowKeys(*trace);

		logShadowGateEcho(cfg);  // 路线行 info 恒打 + 差集 info + 全量 debug（见 helper）
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("shadow config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
