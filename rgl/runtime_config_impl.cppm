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
		// pass）；fsr_upscaler（批 14.5）= FSR 总开关（缺键 = false = 零构造）。两者的
		// 「依赖 velocity」判据都不在这里——运行期取实际图形态，见 RuntimeConfig 注释。
		readBoolKey(root, "velocity_field", cfg.velocityField);
		readBoolKey(root, "fsr_upscaler", cfg.fsrUpscaler);
		readBoolKey(root, "fsr_temporal", cfg.fsrTemporal);
		readBoolKey(root, "fsr_jitter", cfg.fsrJitter);
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
