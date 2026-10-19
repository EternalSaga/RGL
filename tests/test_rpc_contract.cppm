module;
#include <doctest/doctest.h>
#include <nlohmann/json.hpp>  // 被测回包/请求体的类型（第三方头，走 GMF include）

/**
 * test_rpc_contract —— RPC 能力单源生成物的**内部契约** + 请求形状判据
 * （rgl/generated/rpc_schema_generated.cppm、rgl/debug_rpc_shape.cppm）
 *
 * 被测 = 「一张路由表 + 两个查表函数 + 一份路由回包 + 每端点的请求字段表 + 形状校验」。
 * 它们取代了原先散在注册 / 分发 / 超时 / 回包 / 各处理函数手写解析里的字面量，所以本文件钉的
 * 是**这张表能不能当唯一来源**：表非空、身份与路径各自唯一、超时可用、查表双向可逆、回包逐条
 * 来自表；批 2 起再加：字段表逐条被 `rpcValidateRequest` 消费，且消费结果与 schema 声明一致。
 *
 * ## 期望值出处（独立于被测实现）
 * 断言的判据是**路由表与形状约束这一层的契约**（唯一性、正数超时、方法只有两档、查表可逆、
 * 回包元素形态 `"<METHOD> <path>"`、"声明了约束就必须能拒掉违反它的输入"），不是"生成物今天
 * 长什么样"。回包元素的期望串在测试里按契约式自己拼（`methodText + " " + path`），不抄被测函数
 * 的拼接结果——拼接写坏了才需要它红。
 *
 * 🔴 唯一例外：形状用例里那几条**期望文案**是从 `rgl/rpc_schema.lua` 现读的（`f.error`），
 * 不是手抄。为什么这里必须抄被测侧：本用例判的是"生成表里的约束**真的被执行了**"，而文案本身
 * 是不是那条契约串由 `tests/test_burst_protocol.cppm` 逐字钉（那份才是文案的行为锚）。两处都
 * 手抄 = 同一条文案有三个真相源。
 *
 * ## 阳性锚点与反向用例
 *  - 先钉 `kRpcRouteCount > 0` 再遍历（空表会让下面每条 for 都恒真 = 假绿）；
 *  - 每条路径都能查回自己的 id（正向），`/nope` 与空串查不到（反向 = nullopt 而不是第 0 条）；
 *  - 回包 routes 数组**逐条**与按契约式拼出的期望相等且顺序一致（对账伙伴必须钉序，
 *    只钉集合会让注册顺序与回包顺序静默错位）；
 *  - 回包是**对象**而不是数组（`/state` 栽过的 nlohmann 单元素退化形态，同 rpcOk 那道闸）；
 *  - 批 2 正向锚：一条**完全合法**的请求必须过闸（否则"全拒"也能让下面所有反向用例绿）；
 *  - 批 2 反向：缺必填 / 类型错 / 下界 / 上界 / 空串 / 白名单外 / 互斥 / 未知字段，八类各一条，
 *    且**每条都断言归因指向该指的那个字段**（只断"非空"= 任何一条错都能让它绿）；
 *  - 批 2 边界：`manualShape` 端点（/pass）必须**不**执行表内约束（止损线语义），但未知字段
 *    闸门必须照响（那道闸不依赖约束声明）。
 *
 * ## 变异红线（哪条变异让我红）
 *  路由层：把 `rpcRouteByPath` 的未命中分支从 `return {}` 改成 `return kRpcRoutes[0].id` →
 *  反向用例红（未知路径查到第 0 条 = 404 变 200）；把回包拼接从 `"{} {}"` 改成 `"{}"` →
 *  逐条断言红；把 routes 的 push 改成只推前 N−1 条 → 条数与逐条两条都红；把表里两条的 path
 *  改成相同 → 唯一性断言红（生成物的编译期 static_assert 也会先拦下，本条是它的运行期对账伙伴）。
 *  形状层（批 2，这几条就是"schema 会不会变装饰"的判据）：
 *   ① 把 `rpcCheckField` 的 `if (field.required)` 改成 `if (false && field.required)` →
 *      「缺必填」那条红；② 把区间判据的 `num < *min` 改成 `num > *max`（上界半边）→
 *      只有下界那条红、上界那条也红（对称判据各钉一条的意义）；③ 把 `rpcValidateRequest`
 *      的未知字段循环整个删掉 → 未知字段那条红，而其余全绿（= 拍板 5 那道闸单独失守）；
 *   ④ 把 `rpcFieldTypeOk` 一律 `return true` → 类型错那条红；⑤ 把白名单比较改成恒通过 →
 *      白名单那条红；⑥ 把互斥那条 `body.contains(field.exclusive)` 摘掉 → 互斥那条红，
 *      且**归因顺序**那条红（互斥必须先于类型判，否则 attachment 的错会抢走归因）。
 */
export module test_rpc_contract;

import rlstd;
import debug_rpc_payload;     // rpcRoutesJson（路由回包拼装）
import debug_rpc_shape;       // 被测：请求形状校验
import rpc_schema_generated;  // 被测生成物

namespace RGL {

namespace {

/** 表内某条的上线拼写（`"GET /state"`）——契约式，测试自己拼，不取被测函数的产物。 */
std::string routeText(const RpcRouteDef& route) {
	return std::string(route.methodText) + " " + std::string(route.path);
}

/** 按身份取该端点声明的某条字段（找不到 = 断言当场红，不许静默跳过）。 */
const RpcFieldDef* findField(RpcRouteId id, std::string_view name) {
	const auto& fields = rpcRouteById(id).requestFields;
	auto it = std::ranges::find(fields, name, &RpcFieldDef::name);
	return it == fields.end() ? nullptr : &*it;
}

/** 一条完全合法的 `/shotburst` 请求（正向锚的载体：全拒的实现过不了它）。 */
nlohmann::json validBurst() {
	return nlohmann::json{{"n", 4}, {"dir", "screenshots"}};
}

}  // namespace

TEST_CASE("rpc 路由表：非空 + 身份/路径唯一 + 超时可用 + 方法只有两档") {
	// 前提守卫：空表会让下面每个 for 恒真（对空集合的断言 = 假绿）
	REQUIRE(kRpcRouteCount > 0);
	REQUIRE(kRpcRoutes.size() == kRpcRouteCount);

	for (size_t i = 0; i < kRpcRoutes.size(); ++i) {
		const auto& route = kRpcRoutes[i];
		// id 与枚举一一对应：表序 = 枚举序（分发 switch 的 case 靠它，乱序会让 rpcRouteById 错位）
		CHECK(route.id == static_cast<RpcRouteId>(i));
		CHECK(route.timeoutMs > 0u);  // 0 = 请求必然当场 504
		CHECK(!route.path.empty());
		CHECK(route.path.front() == '/');
		CHECK(!route.summary.empty());  // 空说明这条事实只是占位，没人在读它
		CHECK((route.method == RpcRouteMethod::get || route.method == RpcRouteMethod::post));
		CHECK(route.methodText == (route.method == RpcRouteMethod::get ? "GET" : "POST"));
		for (size_t j = i + 1; j < kRpcRoutes.size(); ++j) {
			// 重复路径 = 后一条永远查不到，而"注册了却不可达"没有任何运行期症状
			CHECK(kRpcRoutes[i].path != kRpcRoutes[j].path);
			CHECK(kRpcRoutes[i].id != kRpcRoutes[j].id);
		}
	}
}

TEST_CASE("rpc 查表：每条路径查回自己的 id；未登记路径返回空而不是第 0 条") {
	REQUIRE(kRpcRouteCount > 0);
	for (const auto& route : kRpcRoutes) {
		const auto hit = rpcRouteByPath(route.path);
		REQUIRE(hit.has_value());
		CHECK(*hit == route.id);
		CHECK(&rpcRouteById(*hit) == &route);  // 身份 → 条目可逆（分发与超时共用同一条目）
	}
	// 反向：未登记的路径必须是 nullopt（分发侧据此出 404）
	CHECK(!rpcRouteByPath("/nope").has_value());
	CHECK(!rpcRouteByPath("").has_value());
	CHECK(!rpcRouteByPath("/sta").has_value());    // 前缀不算命中（不是子串匹配）
	CHECK(!rpcRouteByPath("state").has_value());   // 少斜杠不算命中
}

TEST_CASE("rpc 路由回包：逐条来自生成表、顺序一致、形状是对象") {
	REQUIRE(kRpcRouteCount > 0);
	const auto body = rpcRoutesJson(7u);
	// 对象而不是数组（nlohmann 的 initializer-list 退化形态，同 rpcOk 那道闸拦的东西）
	REQUIRE(body.is_object());
	CHECK(body.at("ok").get<bool>());
	CHECK(body.at("frame").get<uint32_t>() == 7u);
	REQUIRE(body.at("routes").is_array());
	// 条数锚：表里几条就回几条（少一条 = 有人把回包写成了硬编码子集）
	REQUIRE(body.at("routes").size() == kRpcRouteCount);

	const auto& routes = body.at("routes");
	for (size_t i = 0; i < routes.size(); ++i) {
		// 逐条 + 同序：只钉集合会漏掉"注册顺序与回包顺序不一致"（顺序 = schema 记录顺序）
		CHECK(routes[i].get<std::string>() == routeText(kRpcRoutes[i]));
	}
}

// ─── 批 2：请求字段表 + 形状校验 ──────────────────────────────────────────────────────

TEST_CASE("rpc 字段表：POST 端点有字段表、字段名端点内唯一、声明了约束就必须带文案") {
	REQUIRE(kRpcRouteCount > 0);
	size_t declaredFields = 0;
	size_t postWithFields = 0;
	for (const auto& route : kRpcRoutes) {
		if (route.method == RpcRouteMethod::post && !route.requestFields.empty())
			++postWithFields;
		for (size_t i = 0; i < route.requestFields.size(); ++i) {
			const auto& f = route.requestFields[i];
			++declaredFields;
			CHECK(!f.name.empty());
			// 端点内重名 = 未知字段闸门的 any_of 只命中第一条，后一条静默失效
			for (size_t j = i + 1; j < route.requestFields.size(); ++j)
				CHECK(f.name != route.requestFields[j].name);
			const bool checked = f.required || f.nonEmpty || !f.exclusive.empty()
			    || !f.oneOf.empty() || f.minValue.has_value() || f.maxValue.has_value()
			    || f.kind != RpcFieldKind::any;
			if (checked)
				CHECK(!f.error.empty());  // 有约束没文案 = 拒了请求却给不出归因
			// 覆盖文案要么不写（落回 error），要么写了就得是另一条（写了个空串 = 归因变空）
			if (!f.emptyError.empty() || !f.oneOfError.empty() || !f.exclusiveError.empty())
				CHECK(!f.error.empty());
		}
		// manualShape 的语义边界：这类端点只登记字段名，**不**声明约束（声明了就是没人执行的
		// 第二份副本 —— 生成物的 static_assert 也钉了这条，这里是它的运行期对账伙伴）
		if (route.manualShape) {
			for (const auto& f : route.requestFields) {
				CHECK(f.kind == RpcFieldKind::any);
				CHECK(!f.required);
				CHECK(!f.nonEmpty);
				CHECK(f.oneOf.empty());
				CHECK(f.exclusive.empty());
				CHECK(!f.minValue.has_value());
				CHECK(!f.maxValue.has_value());
			}
		}
	}
	// 前提守卫（对空集合断言的另一半）：字段表整块没生成时上面每一条 for 都恒真 = 假绿
	CHECK(declaredFields > 0);
	CHECK(postWithFields > 0);
}

TEST_CASE("rpc 形状校验：正向锚 —— 合法请求必须过闸（全拒的实现过不了这条）") {
	CHECK(rpcValidateRequest(RpcRouteId::shotburst, validBurst()).empty());
	// ⚠ 这条正向用例第一次写就**写错了**：同时给了 attachment 与 gbuf，而它俩互斥 ⟹ 被拒。
	// 记在这儿是因为它正好证明这道闸在跑。互斥两侧各给一条合法请求，才配得上"正向锚"。
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 1024}, {"dir", "d"}, {"prefix", "p"}, {"gbuf", 2},
			{"start_frame", 528}})
		.empty());
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 1024}, {"dir", "d"}, {"prefix", "p"},
			{"attachment", "taa_out"}, {"start_frame", 528}})
		.empty());
	CHECK(rpcValidateRequest(RpcRouteId::camera, nlohmann::json::object()).empty());
	CHECK(rpcValidateRequest(RpcRouteId::camera,
		nlohmann::json{{"mode", "trackball"}, {"position", {1, 2, 3}},
			{"yawDegrees", 12.5}, {"yaw_at_frame", 90}})
		.empty());
	CHECK(rpcValidateRequest(RpcRouteId::param,
		nlohmann::json{{"label", "x"}, {"value", 1.0}})
		.empty());
	CHECK(rpcValidateRequest(RpcRouteId::resize,
		nlohmann::json{{"width", 640}, {"height", 480}})
		.empty());
	CHECK(rpcValidateRequest(RpcRouteId::quit, nlohmann::json::object()).empty());
	// /pass 是 manualShape：三条形状检查留在处理函数，表里只登记名字 ⟹ 任何组合都过得了表
	CHECK(rpcValidateRequest(RpcRouteId::pass, nlohmann::json::object()).empty());
	CHECK(rpcValidateRequest(RpcRouteId::pass,
		nlohmann::json{{"name", "highlight"}})
		.empty());
}

TEST_CASE("rpc 形状校验：反向八类各归各的字段（只断非空 = 任何一条错都能让它绿）") {
	// ① 缺必填
	CHECK(rpcValidateRequest(RpcRouteId::shotburst, nlohmann::json{{"dir", "d"}})
		== rpcRouteById(RpcRouteId::shotburst).requestFields[0].error);
	// ② 类型不符（要整数给了字符串）
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", "8"}, {"dir", "d"}})
		== findField(RpcRouteId::shotburst, "n")->error);
	// ③ 下界 / ④ 上界 —— 对称判据各钉一条：只钉一边，另一边改成 `>` 也照样绿
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 0}, {"dir", "d"}})
		== findField(RpcRouteId::shotburst, "n")->error);
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 1025}, {"dir", "d"}})
		== findField(RpcRouteId::shotburst, "n")->error);
	// 恰在边界上必须过（闭区间；写成 `<` / `>` 的弱化变异由这两条兜）
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 1}, {"dir", "d"}})
		.empty());
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 1024}, {"dir", "d"}})
		.empty());
	// ⑤ 空串（nonEmpty 走 emptyError，没写才落回 error）
	const auto* dir = findField(RpcRouteId::shotburst, "dir");
	REQUIRE(dir != nullptr);
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 4}, {"dir", ""}})
		== (dir->emptyError.empty() ? dir->error : dir->emptyError));
	// ⑥ 白名单外。**主体 = /camera 的 mode**（批 3 换的）：原先挂在 shotburst 的 attachment 上，
	// 而那个白名单已删——「哪个附件名合法」由运行期的图与门决定，静态白名单是**能力伪装成请求
	// 校验**（会拒掉引擎本可以服务的请求，见 rpc_schema.lua 里那段注释与 docs/能力观测_plan.md §4）。
	// 本条锚保护的是「oneOf 这条校验分支确实在跑」，不是「某个字段必须有白名单」⟹ 换主体不换判据。
	const auto* mode = findField(RpcRouteId::camera, "mode");
	REQUIRE(mode != nullptr);
	REQUIRE(!mode->oneOf.empty());
	CHECK(rpcValidateRequest(RpcRouteId::camera, nlohmann::json{{"mode", "orbit"}})
		== (mode->oneOfError.empty() ? mode->error : mode->oneOfError));
	// 白名单内每一档都必须过（少一档 = 白名单被改窄而没人发现）
	for (const auto allowed : mode->oneOf)
		CHECK(rpcValidateRequest(RpcRouteId::camera, nlohmann::json{{"mode", allowed}}).empty());
	// ⑦ 互斥（attachment 与 gbuf 二选一——这条**没**随白名单一起动：互斥是请求自相矛盾，
	// 属协议层；白名单才是能力，两者不是一回事）
	const auto* att = findField(RpcRouteId::shotburst, "attachment");
	REQUIRE(att != nullptr);
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 4}, {"dir", "d"}, {"attachment", "swapchain"}, {"gbuf", 1}})
		== (att->exclusiveError.empty() ? att->error : att->exclusiveError));
	// 互斥只给一个必须过（反向用例：guard 不触发 ≠ 坏掉了）
	auto single = validBurst();
	single["gbuf"] = 1;
	CHECK(rpcValidateRequest(RpcRouteId::shotburst, single).empty());
	// ⑧ 未知字段：报的必须是**那个键**，且状态文案里带上端点路径（客户端归因靠这两样）
	const auto unknown = rpcValidateRequest(RpcRouteId::camera,
		nlohmann::json{{"nope", 1}});
	CHECK(unknown.find("nope") != std::string::npos);
	CHECK(unknown.find("/camera") != std::string::npos);
	CHECK(rpcValidateRequest(RpcRouteId::quit, nlohmann::json{{"anything", 1}})
		.find("anything") != std::string::npos);
}

TEST_CASE("rpc 形状校验：body 不是对象 → 逐字那条文案（各处理函数当年就是这条）") {
	CHECK(rpcValidateRequest(RpcRouteId::camera, nlohmann::json::array())
		== "body must be a JSON object");
	CHECK(rpcValidateRequest(RpcRouteId::camera, nlohmann::json(3))
		== "body must be a JSON object");
	// 空对象必须过（imgqa 用 `POST /camera {}` 做只读回显，这里红了它就起不来）
	CHECK(rpcValidateRequest(RpcRouteId::camera, nlohmann::json::object()).empty());
}

TEST_CASE("rpc 形状校验：归因顺序 = 契约（先字段后未知、先互斥后类型）") {
	// 一个坏字段 + 一个未知键：必须报**坏字段**。反了的话客户端会去改一个没写错的键。
	const auto both = rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 0}, {"dir", "d"}, {"bogus", 1}});
	CHECK(both == findField(RpcRouteId::shotburst, "n")->error);
	CHECK(both.find("bogus") == std::string::npos);
	// 两个坏字段：按 fields 的书写顺序报**前一个**（n 在 dir 之前）。顺序反了 = 归因漂移，
	// 而两条文案都还"正确"，只有逐条钉顺序才测得出来。
	const auto first = rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 0}, {"dir", ""}});
	CHECK(first == findField(RpcRouteId::shotburst, "n")->error);
	// 互斥必须先于类型：attachment 给了非字符串**且**同时给 gbuf → 报互斥（拆分前就是这个序）
	const auto* att = findField(RpcRouteId::shotburst, "attachment");
	REQUIRE(att != nullptr);
	REQUIRE(!att->exclusiveError.empty());
	CHECK(rpcValidateRequest(RpcRouteId::shotburst,
		nlohmann::json{{"n", 4}, {"dir", "d"}, {"attachment", 5}, {"gbuf", 1}})
		== att->exclusiveError);
}

TEST_CASE("rpc 形状校验：manualShape 端点不执行表内约束，但未知字段闸门照响") {
	// 止损线语义的两半，缺一半就没人发现：
	// ① 表里没声明约束 ⟹ 什么都不拒（/pass 的三条检查在处理函数里）
	CHECK(rpcValidateRequest(RpcRouteId::pass, nlohmann::json::object()).empty());
	CHECK(rpcValidateRequest(RpcRouteId::pass,
		nlohmann::json{{"toggle_frames", 0}})  // 处理函数会拒，但**表**不拒
		.empty());
	// ② 未知字段闸门不依赖约束声明 ⟹ 必须仍然拒
	CHECK(rpcValidateRequest(RpcRouteId::pass,
		nlohmann::json{{"name", "highlight"}, {"zz", 1}})
		.find("zz") != std::string::npos);
}

TEST_CASE("rpc 形状校验：GET 端点的字段表为空（表非空 = 有人给 GET 端点声明了请求字段）") {
	// GET 端点在分发层根本不解析 body，所以它们的字段表必须是空的；哪天有人往 /state 上加
	// 字段，那条声明**永远不会被执行**（分发只查 POST）= 装饰。这条断言把它变成红的。
	CHECK(rpcRouteById(RpcRouteId::routes).requestFields.empty());
	CHECK(rpcRouteById(RpcRouteId::state).requestFields.empty());
	// 反向锚：POST 端点里确实有非空表（否则上面两条对"整张表都是空的"也成立 = 假绿）
	bool anyPostHasFields = false;
	for (const auto& route : kRpcRoutes)
		if (route.method == RpcRouteMethod::post && !route.requestFields.empty())
			anyPostHasFields = true;
	CHECK(anyPostHasFields);
}

}  // namespace RGL
