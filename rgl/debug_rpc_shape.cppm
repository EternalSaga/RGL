module;
#include <fmt/format.h>       // 未知字段的归因串（点名坏键 + 该端点的路径）
#include <nlohmann/json.hpp>  // 协议 JSON（第三方头，不受 GMF STL 白名单约束）

export module debug_rpc_shape;

import rlstd;
import rpc_schema_generated;  // 能力表（字段形状的唯一来源；真相源 = rgl/rpc_schema.lua）

namespace RGL {

/**
 * RPC **请求形状**的判据本体（`docs/rpc_单源_plan.md` 批 2）。
 *
 * 为什么单独成模块（rlbase 层，零 ECS / 零渲染器）：它做的事只有一件——拿 `rpc_schema.lua`
 * 生成的字段表去量一份 JSON，量不动就给出 schema 里声明的那条文案。宿主 `debug_rpc_exec`
 * 是 app 层大文件，判据住在那里既顶破文件行数红线、又让"形状校验"这种纯逻辑只能起引擎才
 * 验得了（先例 = `debug_rpc_burst` / `debug_rpc_payload`，同一条理由）。
 *
 * 分层依据：只 import rlstd + 生成表 + nlohmann 头 ⟹ 不认设备，归 `RGL_BASE_MODULE_SOURCES`。
 *
 * 🔴 **这条面为什么不是装饰**：处理函数读字段名吃的是生成物里的 `rpc_field::<端点>::<字段>`
 * 常量，改 schema 里的字段名 = 处理函数编译不过；必填/范围/白名单/互斥由本文件的循环逐条
 * 消费生成表，改 schema 里的 min/max = 实机越界应答跟着变。判据 = `tests/test_rpc_contract.cppm`
 * 的注入用例 + `docs/风格检查规则.md` §10.8 的实机读数。
 */

namespace {

/** 覆盖文案优先（`override` 非空用它，否则落回该字段的默认文案）。 */
std::string_view rpcPickMessage(std::string_view overrideText, std::string_view fallback) {
	return overrideText.empty() ? fallback : overrideText;
}

/** 类型档判定。`any` = 不查（该字段的类型失效由处理函数的 `get<T>()` 抛出，见 schema 头部）。 */
bool rpcFieldTypeOk(RpcFieldKind kind, const nlohmann::json& value) {
	// 不写 default：档位由生成物枚举穷尽，加档不写分支 = 编译不过
	switch (kind) {
	case RpcFieldKind::any: return true;
	case RpcFieldKind::string: return value.is_string();
	case RpcFieldKind::integer: return value.is_number_integer();
	case RpcFieldKind::number: return value.is_number();
	case RpcFieldKind::boolean: return value.is_boolean();
	case RpcFieldKind::vec3:
		return value.is_array() && value.size() == 3 && value[0].is_number()
		    && value[1].is_number() && value[2].is_number();
	}
	// 不可达：上面穷尽了枚举。写成 true 而不是 __builtin_unreachable，是为了让"哪天加档忘了
	// 加分支"退化成"该档不查类型"（少一条检查），而不是退化成 UB（编译器已先 -Werror 拦下）。
	return true;
}

/** 数值区间判定（非数值直接放过——类型档那条已经管了它，两条检查不抢同一条文案）。 */
bool rpcFieldInRange(const RpcFieldDef& field, const nlohmann::json& value) {
	if (!value.is_number() || (!field.minValue && !field.maxValue))
		return true;
	const double num = value.get<double>();
	if (field.minValue && num < *field.minValue)
		return false;
	if (field.maxValue && num > *field.maxValue)
		return false;
	return true;
}

/** 白名单判定（非字符串放过，同上一条：类型那条管）。 */
bool rpcFieldInWhitelist(const RpcFieldDef& field, const nlohmann::json& value) {
	if (field.oneOf.empty() || !value.is_string())
		return true;
	const std::string_view text = value.get<std::string_view>();
	return std::ranges::find(field.oneOf, text) != field.oneOf.end();
}

/** 单条字段的检查（顺序 = 契约，见 rgl/rpc_schema.lua 头部）。@return 空 = 通过 */
std::string_view rpcCheckField(const RpcFieldDef& field, const nlohmann::json& body) {
	const bool present = body.contains(field.name);
	// 互斥先查：两个回读源同时给 = 客户端 bug，报"互斥"比报"另一个字段类型不对"有用得多
	if (present && !field.exclusive.empty() && body.contains(field.exclusive))
		return rpcPickMessage(field.exclusiveError, field.error);
	if (!present)
		return field.required ? field.error : std::string_view{};
	const auto& value = body.at(field.name);
	if (!rpcFieldTypeOk(field.kind, value))
		return field.error;
	if (field.nonEmpty && value.is_string() && value.get<std::string_view>().empty())
		return rpcPickMessage(field.emptyError, field.error);
	if (!rpcFieldInWhitelist(field, value))
		return rpcPickMessage(field.oneOfError, field.error);
	if (!rpcFieldInRange(field, value))
		return field.error;
	return {};
}

}  // namespace

/**
 * 按能力表校验一份请求体的**形状**（含未知字段拒绝）。
 *
 * 检查顺序是契约的一部分（一条请求只报一个错，谁先红就是谁的归因）：先按 schema 的字段
 * 书写顺序逐条查，全查完才查未知字段——顺序写反了，`{"n":0,"bogus":1}` 会报"未知字段"而
 * 把真正该改的那条藏起来。
 *
 * 🔴 未知字段 = 400 是批 2 的**行为变更**（拍板 5）：以前拼错键静默忽略，客户端拿到 200 +
 * 一个"什么都没改"的应答。"看起来生效了其实没生效"正是本项目反复踩的那类假绿，所以宁可响。
 *
 * @param route 路由身份（决定查哪张字段表；来自 `rpcRouteByPath` 或注册期闭包）
 * @param body 已解析的请求体（非对象 = 直接 400 文案，与各处理函数原有那条逐字相同）
 * @return 错误文案（空 = 通过）。文案本身来自 schema，不在本文件抄写
 */
export [[nodiscard]] std::string rpcValidateRequest(RpcRouteId route,
	const nlohmann::json& body) {
	const auto& def = rpcRouteById(route);
	if (!body.is_object())
		return "body must be a JSON object";
	for (const auto& field : def.requestFields)
		if (const auto err = rpcCheckField(field, body); !err.empty())
			return std::string{err};
	for (auto it = body.begin(); it != body.end(); ++it) {
		const std::string_view key = it.key();
		const bool declared = std::ranges::any_of(def.requestFields,
			[key](const RpcFieldDef& f) { return f.name == key; });
		if (!declared)
			return fmt::format("unknown field '{}' for {}（字段清单 = rgl/rpc_schema.lua）",
				key, def.path);
	}
	return {};
}

}  // namespace RGL
