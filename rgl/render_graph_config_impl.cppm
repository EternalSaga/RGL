module;
#include <sol/sol.hpp>
#include <fmt/format.h>
module render_graph_config:impl;

import render_graph_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import vkerror;
import rllogger;
import render_graph;

namespace RGL {

// loadRenderGraphConfig 定义（sol2 实现移出接口 TU；本单元是叶子边）。
// 枚举字符串解析失败即 ConfigLoadFailed（结构合法性归 compileRenderGraph）。

namespace {

/**
 * 枚举字符串 → 枚举值查表（配置字符串解析；无匹配 = ConfigLoadFailed，
 * 错误信息带配置路径与字段名）。
 * @tparam E 目标枚举类型
 * @param name 配置中的字符串值
 * @param table (字符串, 枚举值) 候选表
 * @param what 字段名（错误信息用）
 * @param path 配置路径（错误信息用）
 * @return 枚举值；无匹配返回 Error
 */
template <typename E>
std::expected<E, Error> parseEnum(std::string_view name,
	std::initializer_list<std::pair<std::string_view, E>> table, std::string_view what,
	std::string_view path) {
	for (const auto& [text, value] : table)
		if (text == name)
			return value;
	return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
		fmt::format("render graph config ({}): unknown {} '{}'", path, what, name)});
}

/**
 * 解析一条 outputs/inputs 条目（attachment/usage 必填，load/store/clear/clearDepth
 * 可选，缺省回落 RgPassAttachment 默认值）。
 * @param item 配置表条目
 * @param path 配置路径（错误信息用）
 * @param passName 所属 pass 名（错误信息用）
 * @param role "outputs" 或 "inputs"（错误信息用）
 * @param index 条目序号（1 起，错误信息用）
 * @return 解析出的 RgPassAttachment；缺必填字段/枚举串非法返回 Error
 */
std::expected<RgPassAttachment, Error> readPassAttachment(sol::table item,
	std::string_view path, std::string_view passName, const char* role, int index) {
	RgPassAttachment att;
	sol::optional<std::string> name = item["attachment"];
	if (!name)
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("render graph config ({}): pass '{}' {}[{}] missing 'attachment'",
				path, passName, role, index)});
	att.attachment = *name;

	sol::optional<std::string> usage = item["usage"];
	if (!usage)
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("render graph config ({}): pass '{}' {}[{}] (attachment '{}') missing 'usage'",
				path, passName, role, index, att.attachment)});
	auto usageResult = parseEnum(*usage,
		std::initializer_list<std::pair<std::string_view, RgUsage>>{
			{"colorWrite", RgUsage::colorWrite}, {"depthWrite", RgUsage::depthWrite},
			{"sampleRead", RgUsage::sampleRead}, {"transferRead", RgUsage::transferRead},
			{"transferWrite", RgUsage::transferWrite}},
		"usage", path);
	if (!usageResult)
		return std::unexpected(std::move(usageResult).error());
	att.usage = *usageResult;

	if (sol::optional<std::string> load = item["load"]) {
		auto loadResult = parseEnum(*load,
			std::initializer_list<std::pair<std::string_view, RgLoadOp>>{
				{"clear", RgLoadOp::clear}, {"load", RgLoadOp::load}},
			"load op", path);
		if (!loadResult)
			return std::unexpected(std::move(loadResult).error());
		att.loadOp = *loadResult;
	}
	if (sol::optional<std::string> store = item["store"]) {
		auto storeResult = parseEnum(*store,
			std::initializer_list<std::pair<std::string_view, RgStoreOp>>{
				{"store", RgStoreOp::store}, {"dontCare", RgStoreOp::dontCare}},
			"store op", path);
		if (!storeResult)
			return std::unexpected(std::move(storeResult).error());
		att.storeOp = *storeResult;
	}
	// 清除值：clear={r,g,b,a}（颜色）/ clearDepth=f（深度），缺省回落 RgClearValue 默认
	if (sol::optional<sol::table> clear = item["clear"]) {
		for (int i = 1; i <= 4; ++i)
			if (sol::optional<float> v = (*clear)[i])
				att.clear.color[i - 1] = *v;
	}
	if (sol::optional<float> clearDepth = item["clearDepth"])
		att.clear.depth = *clearDepth;
	return att;
}

/**
 * 解析一条 outputs/inputs 列表（逐条目调 readPassAttachment）。
 * @param list 配置表数组（无效/非表 = 空列表，视为无该角色）
 * @param path 配置路径（错误信息用）
 * @param passName 所属 pass 名（错误信息用）
 * @param role "outputs" 或 "inputs"（错误信息用）
 * @return 解析出的附件引用列表；条目非表/字段错误返回 Error
 */
std::expected<std::vector<RgPassAttachment>, Error> readRoleList(sol::table list,
	std::string_view path, std::string_view passName, const char* role) {
	std::vector<RgPassAttachment> out;
	if (!list.valid() || !list.is<sol::table>())
		return out;
	for (int i = 1; i <= static_cast<int>(list.size()); ++i) {
		sol::table item = list[i];
		if (!item.valid() || !item.is<sol::table>())
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
				fmt::format("render graph config ({}): pass '{}' {}[{}] is not a table",
					path, passName, role, i)});
		auto att = readPassAttachment(item, path, passName, role, i);
		if (!att)
			return std::unexpected(std::move(att).error());
		out.push_back(std::move(*att));
	}
	return out;
}

}  // namespace

/**
 * loadRenderGraphConfig 定义（接口注释见 render_graph_config.cppm）：sol2 沙箱
 *（仅 base+table，dofile/loadfile/package/require 置 nil）执行脚本取根表；顶层
 * variant 定路线（deferred/offscreen 直返内建图，exclusive）；forward 逐条解析
 * attachments/passes（format 经 isKnownRgFormat 词表前置校验，结构规则仍归
 * compileRenderGraph）；sol2 抛异常统一捕获转 ConfigLoadFailed。
 */
std::expected<RenderGraphConfig, Error> loadRenderGraphConfig(std::string_view path) {
	auto* logger = RLLogger::getInstance();
	{
		std::ifstream probe{std::string(path)};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Render graph config not found ({}), using default 3-pass graph",
					std::string(path));
			return RenderGraphConfig{RgGraphVariant::forward, defaultRenderGraphDesc()};
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
			fmt::format("render graph config ({}): {}", path, msg)});
	}

	try {
		// 同 scene_config：此版本 sol2 的 stack_proxy→sol::table 隐式转换有坑，用 .as<>
		sol::table root = result[0].as<sol::table>();

		// variant 顶层字段（替代旧 RGL_DEFERRED/RGL_OFFSCREEN env）：deferred/offscreen
		// 直返内建图（exclusive，文件 passes 忽略）；"forward"/缺省 = 解析文件 passes。
		sol::optional<std::string> variant = root["variant"];
		if (variant) {
			if (*variant == "deferred")
				return RenderGraphConfig{RgGraphVariant::deferred, deferredRenderGraphDesc()};
			if (*variant == "offscreen")
				return RenderGraphConfig{RgGraphVariant::offscreen, offscreenRenderGraphDesc()};
			if (*variant != "forward")
				return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
					fmt::format("render graph config ({}): unknown variant '{}' "
						"(want forward/deferred/offscreen)", path, *variant)});
		}

		RgGraphDesc desc;

		sol::table attachments = root["attachments"];
		if (attachments.valid() && attachments.is<sol::table>()) {
			for (int i = 1; i <= static_cast<int>(attachments.size()); ++i) {
				sol::table item = attachments[i];
				RgAttachmentDesc att;
				sol::optional<std::string> name = item["name"];
				if (!name)
					return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
						fmt::format("render graph config ({}): attachment[{}] missing 'name'", path, i)});
				att.name = *name;
				sol::optional<std::string> kind = item["kind"];
				if (kind) {
					auto kindResult = parseEnum(*kind,
						std::initializer_list<std::pair<std::string_view, RgAttachmentKind>>{
							{"swapchain", RgAttachmentKind::swapchain},
							{"depth", RgAttachmentKind::depth},
							{"offscreen", RgAttachmentKind::offscreen}},
						"attachment kind", path);
					if (!kindResult)
						return std::unexpected(std::move(kindResult).error());
					att.kind = *kindResult;
				}
				sol::optional<std::string> format = item["format"];
				if (format) {
					// 词表前置校验（错误带配置路径；结构规则如 swapchain/depth
					// 必须 inherit 仍归 compileRenderGraph）
					if (!isKnownRgFormat(*format))
						return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
							fmt::format("render graph config ({}): attachment '{}' has unknown "
								"format '{}'",
								path, att.name, *format)});
					att.format = *format;
				}
				desc.attachments.push_back(std::move(att));
			}
		}

		sol::table passes = root["passes"];
		if (passes.valid() && passes.is<sol::table>()) {
			for (int i = 1; i <= static_cast<int>(passes.size()); ++i) {
				sol::table item = passes[i];
				RgPassDesc pass;
				sol::optional<std::string> name = item["name"];
				if (!name)
					return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
						fmt::format("render graph config ({}): pass[{}] missing 'name'", path, i)});
				pass.name = *name;
				sol::optional<std::string> kind = item["kind"];
				if (kind) {
					auto kindResult = parseEnum(*kind,
						std::initializer_list<std::pair<std::string_view, RgPassKind>>{
							{"draw", RgPassKind::drawMesh}, {"blit", RgPassKind::blit},
							{"drawLight", RgPassKind::drawLight}},
						"pass kind", path);
					if (!kindResult)
						return std::unexpected(std::move(kindResult).error());
					pass.kind = *kindResult;
				}
				sol::optional<std::string> state = item["state"];
				if (state)
					pass.state = *state;
				sol::optional<std::string> draws = item["draws"];
				if (draws)
					pass.drawSource = *draws;
				auto outputs = readRoleList(item["outputs"], path, pass.name, "outputs");
				if (!outputs)
					return std::unexpected(std::move(outputs).error());
				pass.outputs = std::move(*outputs);
				auto inputs = readRoleList(item["inputs"], path, pass.name, "inputs");
				if (!inputs)
					return std::unexpected(std::move(inputs).error());
				pass.inputs = std::move(*inputs);
				desc.passes.push_back(std::move(pass));
			}
		}

		// 空表透传（"只写 attachments 不写 passes"类半截配置 = 意图不明，
		// 交给 compileRenderGraph 报"no passes"比静默回落默认图更可诊断）
		return RenderGraphConfig{RgGraphVariant::forward, std::move(desc)};
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("render graph config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
