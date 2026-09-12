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

/** loadRuntimeConfig 定义（接口注释见 runtime_config.cppm）：headless/headless_size/win_size/show_after 逐一可选解析 + parseLlm。 */
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
		sol::optional<int> showAfter = root["show_after"];
		if (showAfter)
			cfg.showAfter = *showAfter;

		parseLlm(root, cfg.llm);
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("runtime config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
