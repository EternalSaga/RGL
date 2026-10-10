module;
#include <sol/sol.hpp>
#include <fmt/format.h>
module runtime_config:impl;

import runtime_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;

namespace RGL {

// loadRuntimeConfig 定义（sol2 解析整体移出接口 TU，GMF 只 sol/fmt；本单元是叶子边）。

/**
 * render_scale 解析（Phase 14 批 14.3）：内部渲染分辨率倍率。
 *
 * 非有限值 / 越界 = 回落或钳制 + warn，**不 fail-fast**（与 ui_font_size 同处理：这是偏好量
 * 不是正确性契约，钳制后画面仍可看，而 fail-fast 会让一次笔误毁掉一次长跑）。
 * 缺键 = 保持传入值（即 `RuntimeConfig::renderScale` 的默认 1.0，单一真相源在结构体）。
 *
 * 抽成函数而非内联：`loadRuntimeConfig` 的认知复杂度已贴着规则上限，这段三分支内联会推过线。
 * @param root runtime.lua 根表
 * @param scale 写入目标（传入值 = 当前默认，缺键时原样保留）
 * @param logger 可为空（无日志器时静默按默认处理）
 */
static void applyRenderScale(sol::table root, float& scale, RLLogger* logger) {
	sol::optional<float> raw = root["render_scale"];
	if (!raw)
		return;
	const float v = *raw;
	// 钳制本体在 clampRenderScale（rlbase）——配置侧与 Swapchain 构造侧共用同一个函数，
	// 所以"这里钳过一次、构造时又钳一次"不可能给出两个答案。本函数只负责**怎么出声**。
	const float clamped = clampRenderScale(v, scale);
	if (clamped != v && logger)
		logger->warn("runtime.lua render_scale = {} 不可用（非有限值或越界 [{}, {}]），回落 {}", v,
			kRenderScaleMin, kRenderScaleMax, clamped);
	scale = clamped;
}

/**
 * llm_shot/llm_debug 表公共细字段填充（两段字段布局相同 {delay_frames, file, size}）：
 * 缺字段保留当前值 = 默认或前段已填值。
 * @param shot 源表（调用方保证为表）
 * @param llm 写入目标（delayFrames/file/width/height）
 */

static void applyShotFields(sol::table shot, LlmDebugConfig& llm) {
	sol::optional<uint32_t> delayFrames = shot["delay_frames"];
	if (delayFrames)
		llm.delayFrames = *delayFrames;
	sol::optional<std::string> file = shot["file"];
	if (file)
		llm.file = *file;
	sol::table sizeTable = shot["size"];
	if (sizeTable.valid() && sizeTable.is<sol::table>()) {
		sol::optional<uint32_t> w = sizeTable[1];
		if (w)
			llm.width = *w;
		sol::optional<uint32_t> h = sizeTable[2];
		if (h)
			llm.height = *h;
	}
}

/**
 * llm_debug/llm_shot 解析（自 camera_config 迁入，语义不变）：
 * llm_shot 常驻 {file,size,delay_frames} 先填，llm_debug 覆盖 enabled；
 * 向后兼容 llm_debug = table（旧格式 = 启用 + 细字段，覆盖 llm_shot）。
 * @param root 配置根表
 * @param llm 输出：解析好的截图配置（缺字段保留当前值 = 默认或前段已填值）
 */
static void parseLlm(sol::table& root, LlmDebugConfig& llm) {
	sol::table shotTable = root["llm_shot"];
	if (shotTable.valid() && shotTable.is<sol::table>())
		applyShotFields(shotTable, llm);
	sol::object llmObj = root["llm_debug"];
	if (llmObj.valid()) {
		if (llmObj.is<bool>()) {
			llm.enabled = llmObj.as<bool>();
		} else if (llmObj.is<sol::table>()) {
			sol::table t = llmObj.as<sol::table>();
			llm.enabled = true;
			applyShotFields(t, llm);
		}
	}
}

/**
 * imgui 面板字体字段解析（Phase 10b 批 11b.1）：ui_font 空串 = 系统字体探测链；
 * ui_font_size 非正 = 配置笔误/缺省，回落默认 18（纯显示项，不 fail-fast）。
 * 抽成小函数以守住主解析函数的认知复杂度阈值。
 * @param root 配置根表
 * @param cfg 输出（只写 uiFont / uiFontSize 两字段）
 */
static void readUiFont(sol::table& root, RuntimeConfig& cfg) {
	if (sol::optional<std::string> uiFont = root["ui_font"])
		cfg.uiFont = *uiFont;
	if (sol::optional<double> size = root["ui_font_size"]; size && *size > 0.0)
		cfg.uiFontSize = static_cast<float>(*size);
}

/** 读一个可选布尔键（缺键 = 保持默认值）。类型错误 = sol 直接报错（与 headless 等布尔键
 *  同处理，不静默回落）。抽成函数而不是就地 `if`：`loadRuntimeConfig` 的认知复杂度已贴红线。
 *  @param root runtime.lua 顶层表
 *  @param key  键名
 *  @param dst  目标字段（仅在键存在时被写） */
void readBoolKey(const sol::table& root, const char* key, bool& dst) {
	sol::optional<bool> value = root[key];
	if (value)
		dst = *value;
}

/** 读一个可选浮点键（缺键 = 保持默认值，与 readBoolKey 同形）。
 *  类型错误 = sol 直接报错（不静默回落，同 render_scale 之外的既有纪律）。
 *  @param root 配置表（本函数不关心是哪一层，嵌套块传块表即可）
 *  @param key  键名
 *  @param dst  目标字段（仅在键存在时被写） */
void readFloatKey(const sol::table& root, const char* key, float& dst) {
	sol::optional<float> value = root[key];
	if (value)
		dst = *value;
}

/**
 * `aa` 键解析（批 14.6 抗锯齿三态开关）：字符串 → `AaMode`。
 *
 * 缺键 = 保持默认 off（出厂态 = 红线 A）；未知值 = 返回错误消息由调用方 fail-fast
 * （present_mode 的既有纪律：配置笔误不静默回落——静默回落的失效形态 = 你以为开着
 * 抗锯齿在量图，其实跑在关档上，而图看起来"只是糊了一点"）。
 * 抽成函数 = 认知复杂度红线（先例 applyRenderScale），也为了让 tests 能单独打它。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 present_mode 同口径带配置文件路径）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyAaKey(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["aa"];
	if (!raw)
		return std::nullopt;
	if (*raw == "off")
		cfg.aa = AaMode::off;
	else if (*raw == "fsr") {
		// fsr 态**隐含打开** velocity_field（FSR 的 motionVectors 输入就是那张图）。
		// 隐含只发生在本处这一行（解析期归一化 ⟹ velocityField 仍是单一真相源，
		// 消费侧对隐含关系无感知）；off/taa 态不碰它 ⟹ 不会凭空冒出 velocity pass。
		cfg.aa = AaMode::fsr;
		cfg.velocityField = true;
	} else if (*raw == "taa")
		cfg.aa = AaMode::taa;
	else
		return fmt::format(
			"runtime config ({}): aa must be \"off\", \"fsr\" or \"taa\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * `fsr` 嵌套块解析（批 14.6）：`fsr = { temporal = …, jitter = … }` 两枚调参。
 * 缺块 / 缺键 = 保持结构体默认（true/true = FSR 设计点，理由 → RuntimeConfig 字段注释）。
 * 键的归属就是本函数——**没有**扁平键的兼容双读：两个真相源是本项目反复登记的反模式。
 */
static void readFsrBlock(const sol::table& root, RuntimeConfig& cfg) {
	sol::table fsr = root["fsr"];
	if (!fsr.valid() || !fsr.is<sol::table>())
		return;
	readBoolKey(fsr, "temporal", cfg.fsrTemporal);
	readBoolKey(fsr, "jitter", cfg.fsrJitter);
}

/**
 * `ssr` 嵌套块解析（批 15.1 总开关 + 同批参数面）：`enabled` 一枚总开关 + 5 枚调参。
 * 缺块 / 缺键 = 保持结构体默认（`ssrEnabled = false` = 图里没有 ssr pass；5 枚系数 =
 * `SsrTuning` 成员初始化值 = **全项目唯一真相源**，shader 侧零 fallback）。与 `aa` 正交
 *（`aa` 管抗锯齿选路，本块管 SSR 特性；组合语义 → RuntimeConfig::ssrEnabled 注释）。
 * 形态先例 = readFsrBlock（嵌套块 + readBoolKey，无扁平键双读兼容）。
 *
 * ## 🔴 两条 shader 侧不变式，配置层**刻意不校验**（判断理由，2026-10-10 拍板）
 *
 * 不变式：`bias_px < thickness_px`（违反 = 命中窗口 `[-thickness, -bias]` 为空 = 永不命中）、
 * `rough_full < rough_cutoff`（违反 = 门控反向：光滑面全关、粗糙面满反射；实测靶上覆盖率
 * 恰 0、泄漏 0，形态是"没反射"而不是 NaN 垃圾画面——NaN 只在 e0 == e1 且 roughness 恰等
 * 于 e0 的像素上出现）。
 *
 * 1. **校验会直接堵死反向消融。** `bias_px >= thickness_px` 正是判据 6 要跑的档（闭式期望
 *    = 凸包内覆盖率**恰为 0**），`thickness_px = 0` 同理。配置层把它当非法值拒了/钳了，
 *    就等于"为了让判据过而改判据"——只是改在配置层，更难查。
 * 2. **配置层校验管不住真正的写入方。** 这两条不变式的语义住在 shader，将来 RPC / imgui
 *    若接进来就是绕过配置层的路径 ⟹ 只在配置层校验 = 给人"已经防住"的错觉（第二个真相源）。
 * 3. **两种违反都是可见失效**（整屏没反射 / 垃圾画面），且 `just ssr-check` 判据 A 直接红
 *    ⟹ 不是本项目最怕的"静默漂移"形态。与之对照：`render_scale` 走"钳制 + warn"是因为越界
 *    值仍想要一张能看的图；这里的"越界"本身就是**档**，钳制 = 静默换档，比笔误更坏。
 *
 * ⟹ 本函数**原样透传**，一条 warn 都不发（warn 会在每次跑消融档时刷屏，教人不信任日志）。
 * 不变式的权威措辞在 `SsrTuning` 字段注释与 `ssr.frag.slang` 参数段。
 *
 * ## 为什么**不**往 gate_lint 挂号（判断结论，防后来者"顺手补上"）
 *
 * gate_lint 的覆盖率护栏 = 单测断言 `gateLintRuleGates()` 与 `diagnosticsEchoKeys() ∪
 * shadowConfigEchoKeys()` 相等 ⟹ 它的管辖范围**只有** `configs/diagnostics.lua` 与
 * `configs/shadow.lua` 两份配置。`configs/runtime.lua` 的键（含本块的 `enabled`，也含
 * `aa` / `render_scale` / `fsr.*` 这些既有键）从来不在那张表里，本批因此**没有欠账**：
 * 硬塞进去要么给 runtime.lua 造第三张回显表（= 与 `RuntimeConfig` 字段并存的第二真相源），
 * 要么把 runtime.lua 键混进 diagnostics 的回显语义（那张表按"诊断门"组织，画质系数不是）。
 * 本块键的"生效前置"本来也不依赖设备/图形态（`enabled` 的前置 = 渲染图支持 ssr，那条已由
 * `decorateSsrPass` 自己出 warn），lint 无话可说。
 */
static void readSsrBlock(const sol::table& root, RuntimeConfig& cfg) {
	sol::table ssr = root["ssr"];
	if (!ssr.valid() || !ssr.is<sol::table>())
		return;
	readBoolKey(ssr, "enabled", cfg.ssrEnabled);
	// 5 枚调参 = 每帧量（键名 ↔ scene data 分量的映射唯一落点 = fillSsrParams）
	readFloatKey(ssr, "max_distance_radii", cfg.ssr.maxDistanceRadii);
	readFloatKey(ssr, "thickness_px", cfg.ssr.thicknessPx);
	readFloatKey(ssr, "bias_px", cfg.ssr.biasPx);
	readFloatKey(ssr, "rough_full", cfg.ssr.roughFull);
	readFloatKey(ssr, "rough_cutoff", cfg.ssr.roughCutoff);
}

/**
 * `wet` 嵌套块解析（批 15.2 子步 A 参数面）：6 枚调制系数/空间变化参数，**没有**创建期
 * 开关（湿面调制不是独立 pass，是 deferred_geometry 的 G-buffer 调制，不改图形态；`strength = 0`
 * 就是它的门关档）。缺块 / 缺键 = 保持结构体默认（`WetTuning` 成员初始化值 =
 * **全项目唯一真相源**，shader 侧零 fallback）。形态先例 = readSsrBlock（嵌套块 +
 * readFloatKey，无扁平键双读兼容——两个真相源是本项目反复登记的反模式）。
 *
 * ## 🔴 shader 侧不变式 `rough_lo < rough_hi`，配置层**刻意不校验**（判断同 readSsrBlock）
 *
 * 1. **校验会堵死消融**：`strength = 0` 是合法消融档（因果泄漏恒等式的门关档），配置层
 *    把它当非法值拒了/钳了 = 为了让判据过而改判据，只是改在配置层，更难查。
 * 2. **配置层校验管不住真正的写入方**：不变式语义住在 shader（15.2B 接线后违反的失效
 *    形态由 shader 定义），将来 RPC / imgui 接进来就是绕过配置层的路径 ⟹ 只在配置层
 *    校验 = 给人"已经防住"的错觉（第二真相源）。
 *
 * ⟹ 本函数**原样透传**，一条 warn 都不发（warn 会在每次跑消融档时刷屏，教人不信任日志）。
 * 不变式的权威措辞在 `WetTuning` 字段注释。gate_lint 不挂号的理由与 ssr 块完全同款
 * （runtime.lua 键不在 gate_lint 管辖的两张回显表里，→ readSsrBlock 末段）。
 */
static void readWetBlock(const sol::table& root, RuntimeConfig& cfg) {
	sol::table wet = root["wet"];
	if (!wet.valid() || !wet.is<sol::table>())
		return;
	// 6 枚调参 = 每帧量（键名 ↔ scene data 分量的映射唯一落点 = fillWetParams）
	readFloatKey(wet, "strength", cfg.wet.strength);
	readFloatKey(wet, "dark", cfg.wet.dark);
	readFloatKey(wet, "rough_lo", cfg.wet.roughLo);
	readFloatKey(wet, "rough_hi", cfg.wet.roughHi);
	readFloatKey(wet, "ripple_amp", cfg.wet.rippleAmp);
	readFloatKey(wet, "puddle_freq_radii", cfg.wet.puddleFreqRadii);
}

/** loadRuntimeConfig 定义（接口注释见 runtime_config.cppm）：headless/headless_size/win_size/show_after/present_mode/ui_font 逐一可选解析 + parseLlm。 */
[[nodiscard]] std::expected<RuntimeConfig, Error> loadRuntimeConfig(std::string_view path) {
	auto* logger = RLLogger::getInstance();

	{
		std::ifstream probe{std::string(path)};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Runtime config not found ({}), using defaults (interactive window)",
					std::string(path));
			return RuntimeConfig{};
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
			fmt::format("runtime config ({}): {}", path, msg)});
	}

	try {
		sol::table root = result[0].as<sol::table>();
		RuntimeConfig cfg;

		sol::optional<bool> headless = root["headless"];
		if (headless)
			cfg.headless = *headless;
		auto readSize = [&](const char* key, uint32_t& w, uint32_t& h) {
			sol::table t = root[key];
			if (t.valid() && t.is<sol::table>()) {
				sol::optional<uint32_t> vw = t[1];
				sol::optional<uint32_t> vh = t[2];
				if (vw)
					w = *vw;
				if (vh)
					h = *vh;
			}
		};
		readSize("headless_size", cfg.headlessWidth, cfg.headlessHeight);
		readSize("win_size", cfg.winWidth, cfg.winHeight);

		applyRenderScale(root, cfg.renderScale, logger);  // 批 14.3：内部渲染分辨率倍率
		// velocity_field（批 14.4）= 逐像素速度场总开关（缺键 = false = 图里没有 velocity
		// pass）。「FSR 依赖 velocity」的判据不在这里——运行期取实际图形态，见 RuntimeConfig
		// 注释；批 14.6 起 aa 为 fsr 时本键被隐含打开（唯一发生点 = applyAaKey 内一行）。
		readBoolKey(root, "velocity_field", cfg.velocityField);
		if (auto err = applyAaKey(root, cfg, path))
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed, *err});
		readFsrBlock(root, cfg);  // 批 14.6：FSR 调参收进嵌套块（无扁平键双读兼容）
		readSsrBlock(root, cfg);  // 批 15.1：SSR 总开关（嵌套块，缺块/缺键 = false = 图里没有 ssr pass）
		readWetBlock(root, cfg);  // 批 15.2：湿面参数（嵌套块，缺块/缺键 = WetTuning 默认；消费者 = deferred_geometry 湿面调制）
		sol::optional<int> showAfter = root["show_after"];
		if (showAfter)
			cfg.showAfter = *showAfter;

		// present_mode：窗口 present 模式偏好（PresentModePref；headless 忽略）。
		// 未知值 = 配置笔误，fail-fast（与类型错误同级，不静默回落）。
		sol::optional<std::string> presentMode = root["present_mode"];
		if (presentMode) {
			if (*presentMode == "immediate")
				cfg.presentMode = PresentModePref::immediate;
			else if (*presentMode == "fifo")
				cfg.presentMode = PresentModePref::fifo;
			else
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					fmt::format("runtime config ({}): present_mode must be \"immediate\" or \"fifo\", got \"{}\"",
						path, *presentMode)});
		}

		parseLlm(root, cfg.llm);

		readUiFont(root, cfg);  // imgui 面板字体（Phase 10b 批 11b.1）
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("runtime config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
