module;
#include <glm/glm.hpp>        // 三维参数值（vec3，color / drag3 条目）
#include <fmt/format.h>       // 类型不符的错误串拼装
#include <nlohmann/json.hpp>  // 协议 JSON（第三方头，不受 GMF STL 白名单约束）

export module debug_rpc_payload;

import rlstd;
import debug_rpc;       // RpcResponder（rpcReject 的兑现出口）
import param_registry;  // ParamPage（参数页结构）
import rpc_schema_generated;  // kRpcRoutes（路由回包的唯一来源）

namespace RGL {

/**
 * RPC 的**应答形状 + 参数页编解码**（批次 B2-5 从 `debug_rpc_exec` 拆出）。
 *
 * 为什么单独成模块（rlbase 层，零 ECS / 零渲染器）：这一簇全是「JSON 形状」的判据本体——
 * 应答必须是对象（`rpcOk` 那道闸拦的就是 nlohmann 的静默退化）、错误体必须统一形状、
 * 参数页 ↔ `entries` 数组的双向映射与 value 类型校验。宿主 `debug_rpc_exec` 是 app 层大
 * 文件（协议 + 装配 + 帧边界钩子），判据住在那里既顶破文件行数红线、又让形状守卫只能起
 * 引擎才验得了（先例 = `debug_rpc_burst`，同一条理由）。
 *
 * 分层依据：只 import rlstd + debug_rpc（运输层的 RpcResponder）+ param_registry ⟹
 * 不认设备，归 `RGL_BASE_MODULE_SOURCES`。
 */

/** 协议应答结果（成功 = 应答体；失败 = HTTP 状态 + 错误体）。 */
export struct RpcOutcome {
	int status = 200;
	nlohmann::json body;
};

/** 错误体（统一形状 `{"ok":false,"error":...}`；运输层错误体另见 debug_rpc_impl）。 */
export nlohmann::json rpcErrorJson(std::string message) {
	return nlohmann::json{{"ok", false}, {"error", std::move(message)}};
}

/**
 * 成功应答（端点自行补字段）。
 *
 * ⚠ **这里断言 body 必须是对象**，因为 nlohmann 的 `json(init)` 构造有个静默失败模式：
 * initializer-list 里**只要有一个元素不是 `{"key", value}` 二元对**，整个值就退化成
 * **数组**——不报错、不警告。`/state` 的 `render` 段就这么坏过（写成 `{{"render", {...}}}`，
 * 外层多一对花括号；引入 = 批 14.3，同日发现）。
 *
 * 为什么仍值得建这道闸（当时的实际表现并非"假绿"，别据此以为无害）：
 * - `temporal-flicker` / `temporal-motion` 确实**响亮地**失败（退出码 1），但抛的是
 *   `type_error.304 cannot use at() with array`——**指向调用侧用错 API**，而真因在服务端；
 * - `/state` 本身仍回 **HTTP 200**，`render` 段对任何按对象解析的新客户端**不可达且不出声**
 *   ——而那段存在的目的正是"判据若不知道自己看的是哪档就会误读"（批 14.3 原话）。
 * 所有成功应答都流经这一个函数，所以闸建在这里而不是每个端点各写一遍：退化时抛
 * `type_error`（由外层 catch 转成 400 + 消息），**在坏的那一侧坏**。
 *
 * 注意别误伤：`{"routes", {"a","b"}}`、`{"position", {x,y,z}}` 这类**值本身是数组**的写法
 * 是合法的（元素仍是二元对），本断言只拦「整个应答不是对象」。
 */
export RpcOutcome rpcOk(nlohmann::json body) {
	if (!body.is_object())
		throw nlohmann::json::type_error::create(
			301,
			"rpcOk 的应答体必须是 JSON 对象，实际是 " + std::string(body.type_name())
				+ "——通常是 initializer-list 里混进了非 {key, value} 二元对的元素"
				  "（如把某个键值对多包了一层花括号），nlohmann 于是把整个应答退化成数组",
			&body);
	return RpcOutcome{200, std::move(body)};
}

/** 失败应答。 */
export RpcOutcome rpcFail(int status, std::string message) {
	return RpcOutcome{status, rpcErrorJson(std::move(message))};
}

/**
 * 根路由的回包（agent / 客户端据此发现端点；frame = 装配态帧计数，纯观测）。
 *
 * 🔴 `routes` 数组**逐条从生成表拼装**，本文件不写路径字面量——这里曾经是手写名单的第三份
 * 副本（注册、分发之外），在它里面加/删一条，构建与全部检查都不会红。现在加端点只改
 * `rgl/rpc_schema.lua`，回包自己跟着长。
 * 元素形态 = `"<METHOD> <path>"`（与重构前逐字节相同，老客户端的字符串匹配继续成立）。
 * @param frame 装配态帧计数
 * @return 回包对象（`{"ok":true,"frame":N,"routes":[…]}`）
 */
export nlohmann::json rpcRoutesJson(uint32_t frame) {
	nlohmann::json routes = nlohmann::json::array();
	for (const auto& route : kRpcRoutes)
		routes.push_back(fmt::format("{} {}", route.methodText, route.path));
	return nlohmann::json{{"ok", true}, {"frame", frame}, {"routes", std::move(routes)}};
}

/** 直接兑现失败（不经 RpcOutcome 的早退路径：/shot 的 409 拒绝等）。 */
export void rpcReject(const RpcResponder& responder, int status, std::string message) {
	responder.resolve(status, rpcErrorJson(std::move(message)).dump());
}

/**
 * 一页 → `entries` 数组（kind 与面板控件一一对应，`/param` 据 kind 校验 value 类型）。
 * 取值经条目 get 回调读源头（组件/ctx/材质表），故 /state 永远是当前真实值。
 * @param page 参数页
 * @return 条目数组（JSON）
 */
export nlohmann::json rpcEntriesJson(const ParamPage& page) {
	nlohmann::json out = nlohmann::json::array();
	for (const auto& e : page.sliders)
		out.push_back({{"kind", "slider"}, {"label", e.label}, {"min", e.minValue},
			{"max", e.maxValue}, {"speed", e.speed}, {"value", e.get()}});
	for (const auto& e : page.intSliders)
		out.push_back({{"kind", "int"}, {"label", e.label}, {"min", e.minValue},
			{"max", e.maxValue}, {"value", e.get()}});
	for (const auto& e : page.colors) {
		const glm::vec3 v = e.get();
		out.push_back({{"kind", "color"}, {"label", e.label},
			{"value", {v.x, v.y, v.z}}});
	}
	for (const auto& e : page.drags) {
		const glm::vec3 v = e.get();
		out.push_back({{"kind", "drag3"}, {"label", e.label}, {"min", e.minValue},
			{"max", e.maxValue}, {"speed", e.speed}, {"value", {v.x, v.y, v.z}}});
	}
	for (const auto& t : page.texts)
		out.push_back({{"kind", "text"}, {"value", t.text()}});
	return out;
}

/**
 * 页内按 label 写入（label 页内唯一 = `param_semantics.json` 单一事实来源，
 * 这就是 D11 的寻址约定）。
 * @param page 目标页
 * @param label 条目标签（须来自 /state 发现）
 * @param value 新值（类型须与 kind 相符：slider/int = 数字，color/drag3 = 三维数组）
 * @return nullopt = 本页无此 label；有 = 已写入的应答（或类型不符的 400）
 */
export std::optional<RpcOutcome> rpcWriteParamPage(
	const ParamPage& page, const std::string& label,
	const nlohmann::json& value) {
	auto byLabel = [&label](const auto& entry) { return entry.label == label; };
	auto ok = [&label, &value] { return rpcOk({{"ok", true}, {"label", label}, {"value", value}}); };

	if (auto it = std::ranges::find_if(page.sliders, byLabel); it != page.sliders.end()) {
		if (!value.is_number())
			return rpcFail(400, fmt::format("{}: value must be a number", label));
		it->set(value.get<float>());
		return ok();
	}
	if (auto it = std::ranges::find_if(page.intSliders, byLabel); it != page.intSliders.end()) {
		if (!value.is_number())
			return rpcFail(400, fmt::format("{}: value must be a number", label));
		it->set(static_cast<int>(value.get<double>()));  // 向零取整（离散 id 类，语义可接受）
		return ok();
	}
	if (auto it = std::ranges::find_if(page.colors, byLabel); it != page.colors.end()) {
		if (!value.is_array() || value.size() != 3)
			return rpcFail(400, fmt::format("{}: value must be [r,g,b]", label));
		it->set(glm::vec3{value.at(0).get<float>(), value.at(1).get<float>(),
			value.at(2).get<float>()});
		return ok();
	}
	if (auto it = std::ranges::find_if(page.drags, byLabel); it != page.drags.end()) {
		if (!value.is_array() || value.size() != 3)
			return rpcFail(400, fmt::format("{}: value must be [x,y,z]", label));
		it->set(glm::vec3{value.at(0).get<float>(), value.at(1).get<float>(),
			value.at(2).get<float>()});
		return ok();
	}
	return std::nullopt;
}

}  // namespace RGL
