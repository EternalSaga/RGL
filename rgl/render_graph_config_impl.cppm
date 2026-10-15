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
 * 可选枚举串字段读取（load/store 两段共用）：字段存在才经 parseEnum 词表解析写回
 * （缺失 = 回落默认；串非法 = 透传 parseEnum Error，消息格式逐字相同）。
 * @param item 配置表条目
 * @param key 字段名
 * @param into 写入目标（字段存在才覆盖）
 * @param table (字符串, 枚举值) 候选表
 * @param what 字段名（错误信息用）
 * @param path 配置路径（错误信息用）
 * @return 成功 = void；枚举串非法 = Error
 */
template <typename E>
std::expected<void, Error> readOptionalEnum(sol::table item, const char* key, E& into,
	std::initializer_list<std::pair<std::string_view, E>> table, std::string_view what,
	std::string_view path) {
	sol::optional<std::string> text = item[key];
	if (!text)
		return {};
	auto result = parseEnum(*text, table, what, path);
	if (!result)
		return std::unexpected(std::move(result).error());
	into = *result;
	return {};
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
			{"transferWrite", RgUsage::transferWrite},
			// storage 两档 = buffer 附件的唯一合法引用方式（kind 侧双向规则见
			// validateAttachmentBufferName）。词表漏一条的后果不是"配不出来"而是
			// "配置期报词表错、根因却要读到 compile 层才发现"，故与枚举同步加。
			{"storageRead", RgUsage::storageRead},
			{"storageWrite", RgUsage::storageWrite}},
		"usage", path);
	if (!usageResult)
		return std::unexpected(std::move(usageResult).error());
	att.usage = *usageResult;

	if (auto load = readOptionalEnum(item, "load", att.loadOp,
			std::initializer_list<std::pair<std::string_view, RgLoadOp>>{
				{"clear", RgLoadOp::clear}, {"load", RgLoadOp::load}},
			"load op", path); !load)
		return std::unexpected(std::move(load).error());
	if (auto store = readOptionalEnum(item, "store", att.storeOp,
			std::initializer_list<std::pair<std::string_view, RgStoreOp>>{
				{"store", RgStoreOp::store}, {"dontCare", RgStoreOp::dontCare}},
			"store op", path); !store)
		return std::unexpected(std::move(store).error());
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

/**
 * 附件 format 的**词表前置校验**（按 kind 分派；从 readAttachments 里抽出来是为了让那条
 * 主路径的认知复杂度留在红线内 —— 但更主要的原因是"按 kind 分派词表"本身是一件独立的事，
 * 混在解析循环里正好是它当年被漏掉的那次的原因）。
 * @param att  已填好 kind 的附件描述（就地写回 format）
 * @param fmt  配置里写的格式串
 * @param path 配置路径（错误信息用）
 * @return 失败带 Error（消息含 kind 归因：同一个格式串在颜色档合法、在深度档非法）
 */
std::expected<void, Error> validateLoadedFormat(RgAttachmentDesc& att,
	const std::string& fmt, std::string_view path) {
	// 🔴 词表**必须按 kind 分派**，这是债-122 的另一半：只补 kind 词表而这里仍单查
	// 颜色表，`kind = "depthOffscreen"` 依旧写不出来（配 `d32_sfloat` 报"未知格式"，
	// 配 `inherit` 又被 compile 层的深度词表拒 ⟹ 该档在 Lua 里**根本不可表示**）。
	// 与 compile 层的 `validateAttachmentFormat` 同口径：offscreen 查颜色表、
	// depthOffscreen 查深度表、其余只认 "inherit"（那一半仍归 compile，这里不重复）。
	const bool formatOk = att.kind == RgAttachmentKind::depthOffscreen
		? isKnownDepthRgFormat(fmt)
		: isKnownRgFormat(fmt);
	if (!formatOk)
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("render graph config ({}): attachment '{}' (kind={}) has unknown "
				"format '{}'",
				path, att.name, kindName(att.kind), fmt)});
	att.format = std::string(fmt);
	return {};
}

/**
 * 解析顶层 attachments 数组（name 必填；kind 走 parseEnum 词表；format 经
 * validateLoadedFormat 的按-kind 词表前置校验——结构规则如 swapchain/depth 必须 inherit
 * 仍归 compileRenderGraph）。
 * @param root 配置根表（attachments 缺失/非表 = 空列表，透传给 compile 判定）
 * @param path 配置路径（错误信息用）
 * @return 附件描述列表；缺 name/词表串非法返回 Error
 */
std::expected<std::vector<RgAttachmentDesc>, Error> readAttachments(sol::table root,
	std::string_view path) {
	std::vector<RgAttachmentDesc> out;
	sol::table attachments = root["attachments"];
	if (!attachments.valid() || !attachments.is<sol::table>())
		return out;
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
					// 债-122：`depthOffscreen` 在枚举里住了很久（阴影图类），词表却一直没挂号——
					// 症状是"配置里写 depthOffscreen 报词表错"，而内建图用同一个 kind 跑得好好的，
					// 于是"图支持这个 kind"与"配置能写出这个 kind"是两条独立的路，只通了一条。
					{"depthOffscreen", RgAttachmentKind::depthOffscreen},
					{"offscreen", RgAttachmentKind::offscreen},
					// buffer 档：不持资源的名字申报（资源归模块申请），见 render_graph.cppm 契约注释
					{"buffer", RgAttachmentKind::buffer}},
				"attachment kind", path);
			if (!kindResult)
				return std::unexpected(std::move(kindResult).error());
			att.kind = *kindResult;
		}
		sol::optional<std::string> format = item["format"];
		if (format)
			if (auto r = validateLoadedFormat(att, *format, path); !r)
				return std::unexpected(std::move(r).error());
		out.push_back(std::move(att));
	}
	return out;
}

/**
 * 解析单条 pass 条目（name 必填；kind 词表 draw/blit/drawLight；state/draws 直取
 * 字符串；outputs/inputs 经 readRoleList；错误消息逐字保持拆分前内联文案）。
 * @param item 条目表
 * @param path 配置路径（错误信息用）
 * @param index 条目序号（1 起，错误信息用）
 * @return 解析出的 RgPassDesc；缺 name/词表串非法返回 Error
 */
std::expected<RgPassDesc, Error> readPass(sol::table item, std::string_view path,
	int index) {
	RgPassDesc pass;
	sol::optional<std::string> name = item["name"];
	if (!name)
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("render graph config ({}): pass[{}] missing 'name'", path, index)});
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
	return pass;
}

/**
 * 解析顶层 passes 数组（name 必填；kind 词表 draw/blit/drawLight；state/draws 直取
 * 字符串；outputs/inputs 经 readRoleList；逐条目调 readPass）。
 * @param root 配置根表（passes 缺失/非表 = 空列表，透传给 compile 报"no passes"）
 * @param path 配置路径（错误信息用）
 * @return pass 描述列表；缺 name/词表串非法返回 Error
 */
std::expected<std::vector<RgPassDesc>, Error> readPasses(sol::table root,
	std::string_view path) {
	std::vector<RgPassDesc> out;
	sol::table passes = root["passes"];
	if (!passes.valid() || !passes.is<sol::table>())
		return out;
	for (int i = 1; i <= static_cast<int>(passes.size()); ++i) {
		sol::table item = passes[i];
		auto pass = readPass(item, path, i);
		if (!pass)
			return std::unexpected(std::move(pass).error());
		out.push_back(std::move(*pass));
	}
	return out;
}

/**
 * 顶层 variant 字段选路（替代旧 RGL_DEFERRED/RGL_OFFSCREEN env）：deferred/offscreen
 * 直返内建图配置（exclusive，文件 passes 忽略）；"forward"/缺失 = nullopt
 * （解析文件 passes）；未知串 = ConfigLoadFailed（消息逐字保持拆分前内联文案）。
 * @param root 配置根表
 * @param path 配置路径（错误信息用）
 * @return 内建图配置 = deferred/offscreen 命中；nullopt = forward 或缺失；非法 variant = Error
 */
std::expected<std::optional<RenderGraphConfig>, Error> resolveVariantConfig(sol::table root,
	std::string_view path) {
	sol::optional<std::string> variant = root["variant"];
	if (!variant)
		return std::nullopt;
	if (*variant == "deferred")
		return RenderGraphConfig{RgGraphVariant::deferred, deferredRenderGraphDesc()};
	if (*variant == "offscreen")
		return RenderGraphConfig{RgGraphVariant::offscreen, offscreenRenderGraphDesc()};
	if (*variant != "forward")
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("render graph config ({}): unknown variant '{}' "
				"(want forward/deferred/offscreen)", path, *variant)});
	return std::nullopt;
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
		// 选路判定见 resolveVariantConfig（上方匿名 namespace）。
		auto preset = resolveVariantConfig(root, path);
		if (!preset)
			return std::unexpected(std::move(preset).error());
		if (*preset)
			return std::move(**preset);

		// attachments/passes 逐条解析见 readAttachments/readPasses（上方匿名 namespace）
		RgGraphDesc desc;
		if (auto atts = readAttachments(root, path))
			desc.attachments = std::move(*atts);
		else
			return std::unexpected(std::move(atts).error());
		if (auto ps = readPasses(root, path))
			desc.passes = std::move(*ps);
		else
			return std::unexpected(std::move(ps).error());

		// 空表透传（"只写 attachments 不写 passes"类半截配置 = 意图不明，
		// 交给 compileRenderGraph 报"no passes"比静默回落默认图更可诊断）
		return RenderGraphConfig{RgGraphVariant::forward, std::move(desc)};
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("render graph config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
