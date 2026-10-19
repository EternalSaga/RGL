module;
#include <chrono>                // wait_for 超时量（GMF 例外清单成员）
#include <httplib.h>             // cpp-httplib（header-only，只活在本 impl 单元 GMF）
#include <nlohmann/json.hpp>     // 运输层错误体（tools/ 同款约定）
module debug_rpc:impl;

import debug_rpc;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import rpc_schema_generated;  // 路由表（注册与超时的唯一来源；真相源 = rgl/rpc_schema.lua）

namespace RGL {

namespace {

/**
 * 运输层错误体（统一形状 `{"ok":false,"error":...}`；命令级错误由 applier 自行构造）。
 * @param message 人类可读原因（已含调用方上下文）
 * @return 单行 JSON 文本
 */
std::string errorBody(std::string_view message) {
	return nlohmann::json{{"ok", false}, {"error", std::string(message)}}.dump();
}

/** 写 JSON 应答体（content-type 固定；状态码沿用 `res.status`，错误路由由 error handler 置）。 */
void respondBody(httplib::Response& res, const std::string& body) {
	res.set_content(body, "application/json; charset=utf-8");
}

/** 写 JSON 应答（显式状态码）。 */
void respondStatus(httplib::Response& res, int status, const std::string& body) {
	res.status = status;
	respondBody(res, body);
}

/** 写应答载荷（状态码 + JSON 体；transport 层唯一出口）。 */
void respondReply(httplib::Response& res, const RpcReply& reply) {
	respondStatus(res, reply.status, reply.body);
}

}  // namespace

/**
 * 传输层实现体（pimpl 体——httplib 类型只在此出现，接口零 httplib 依赖）。
 *
 * 线程归属：`queue`/`inFlight`/`applier` 由 `mutex` 保护（HTTP 线程 ↔ 渲染线程）；
 * `started` 只在主线程读写（start/stop/running/poll 同为主线程 API），故不加锁。
 */
struct DebugRpcServer::Impl {
	/** 队列条目（原始请求 + 待兑现 promise——超时/停机按 promise 指针认领结算）。 */
	struct Pending {
		RpcRequest request;
		std::shared_ptr<std::promise<RpcReply>> reply;
	};

	httplib::Server server;      // 同步版监听（bind_to_port → listen_after_bind）
	std::thread listener;        // 监听线程（listen_after_bind 阻塞到 stop()）
	std::mutex mutex;            // 保护 queue / inFlight / applier
	std::deque<Pending> queue;   // 待帧边界消费（渲染线程 poll() drain）
	std::vector<std::shared_ptr<std::promise<RpcReply>>> inFlight;  // 在途（HTTP 线程自注销）
	Applier applier;             // 帧边界执行器（装配期设置，start 后不再改）
	bool started = false;        // 主线程状态位（running/stop/poll 早退判断）
};

DebugRpcServer::DebugRpcServer() : impl(std::make_unique<Impl>()) {}

DebugRpcServer::~DebugRpcServer() {
	stop();  // 幂等兜底（未 start = 早退）；unique_ptr 需 Impl 完整 → 析构必须落本 TU
}

void DebugRpcServer::setApplier(Applier applier) {
	// 装配期单线程写（start 之前）——poll() 据此在锁外读是同一线程，无数据竞争
	if (impl)
		impl->applier = std::move(applier);
}

bool DebugRpcServer::running() const {
	return impl && impl->started;
}

bool DebugRpcServer::start(std::string host, int port) {
	if (!impl || impl->started)
		return running();
	Impl* p = impl.get();

	// 路由：**遍历生成表逐条注册**（手写的路径字面量在这里不存在 = rules_rpc 面 2 扫到即红）。
	// 表 = `rgl/rpc_schema.lua` 的生成物；一条记录同时给出方法、路径与该端点的超时——注册期把
	// 身份绑进闭包，等待侧不必再按路径反查（少一次查表，也少一个"路径不在表里"的悬空状态）。
	// 未知路由/方法由 error handler 兜 JSON 404/405。pattern 走 std::regex_match 全串匹配。
	auto serve = [this](RpcRouteId id, const httplib::Request& req, httplib::Response& res) {
		try {
			respondReply(res, awaitReply(RpcRequest{req.method, req.path, req.body}, id));
		} catch (const std::exception& e) {
			respondStatus(res, 500, errorBody(e.what()));
		}
	};
	for (const auto& route : kRpcRoutes) {
		auto handler = [serve, id = route.id](const httplib::Request& req, httplib::Response& res) {
			serve(id, req, res);
		};
		// 不写 default：方法只有生成表里那两档，第三档（schema 写错）在生成物里就编不过
		switch (route.method) {
		case RpcRouteMethod::get:
			p->server.Get(std::string(route.path), handler);
			break;
		case RpcRouteMethod::post:
			p->server.Post(std::string(route.path), handler);
			break;
		}
	}
	// httplib 对**所有** status≥400 都回调 error handler（返回值只控制 range 应用），
	// 故必须只补空体：我们 handler 自产的 400/404/409/503/504 载荷不能被覆盖。
	// 非空体 = 已有语义（自家 dispatch 或 httplib 自置），空体 = httplib 兜的 404/405/431 等。
	p->server.set_error_handler([](const httplib::Request&, httplib::Response& res) {
		if (res.body.empty())
			respondBody(res, errorBody("unknown route"));
	});

	// bind 前探端口：cpp-httplib 在 Linux 启用 SO_REUSEPORT，多实例可同时"bind 成功"，
	// 而连接由内核按四元组哈希分流到不同进程——症状是请求随机打到别的实例、画面与
	// 预期机位对不上，且两边日志都显示"监听成功"（2026-09-29 实测三实例同时在跑）。
	// 这里先用一次带超时的探测请求占位：能连上 = 已有监听者（多半是另一个 rgl 实例），
	// 直接拒绝启动并说清原因，胜过静默分流。回包内容不解析，只看连不连得上。
	{
		httplib::Client probe(host, port);
		probe.set_connection_timeout(std::chrono::seconds(1));
		probe.set_read_timeout(std::chrono::seconds(1));
		const auto res = probe.Get("/");
		if (res) {
			if (auto* logger = RLLogger::getInstance())
				logger->error(
					"debug_rpc: {}:{} 已有进程在监听（HTTP {}），本次不启动 RPC 通道。"
					"多实例共存会让请求被内核分流（cpp-httplib 的 SO_REUSEPORT）；"
					"请先停掉旧实例（pgrep -cx rgl）",
					host, port, res->status);
			return false;
		}
	}

	// bind 同步做 = 返回值可信（端口占用/无权限 → 调用方日志降级关，不崩）
	if (!p->server.bind_to_port(host, port)) {
		if (auto* logger = RLLogger::getInstance())
			logger->error("debug_rpc: bind 失败 {}:{}（端口占用或无权限），RPC 通道关闭",
				host, port);
		return false;
	}
	p->listener = std::thread([p] { p->server.listen_after_bind(); });
	p->started = true;
	if (auto* logger = RLLogger::getInstance())
		logger->info("debug_rpc: HTTP RPC 监听 {}:{}（仅 loopback；{} 条路由，根路由回包给出发现面）",
			host, port, kRpcRouteCount);
	return true;
}

void DebugRpcServer::stop() {
	if (!impl || !impl->started)
		return;
	Impl* p = impl.get();
	{
		// 先结算后关监听：在途请求立刻拿 503（含已交 applier 但未兑现的），不等超时
		std::lock_guard<std::mutex> lock(p->mutex);
		for (const auto& reply : p->inFlight) {
			try {
				reply->set_value(RpcReply{503, errorBody("server stopping")});
			} catch (const std::future_error&) {
				// 已被 applier 兑现：幂等 no-op
			}
		}
		p->queue.clear();  // 未交 applier 的条目一并丢弃（promise 已在上面 503）
		p->inFlight.clear();
	}
	p->server.stop();
	if (p->listener.joinable())
		p->listener.join();
	p->started = false;
	if (auto* logger = RLLogger::getInstance())
		logger->info("debug_rpc: 已停机（在途请求全部 503）");
}

void DebugRpcServer::poll() {
	if (!impl || !impl->started)
		return;
	Impl* p = impl.get();
	std::deque<Impl::Pending> batch;
	{
		std::lock_guard<std::mutex> lock(p->mutex);
		if (p->queue.empty())
			return;  // 空队列零开销（每帧帧首必经）
		batch.swap(p->queue);
	}
	// applier 在锁外调用（装配期已设、不再改）——applier 内不得回调本对象，防死锁
	for (auto& pending : batch) {
		if (!p->applier) {
			try {
				pending.reply->set_value(
					RpcReply{503, errorBody("no applier (server not wired)")});
			} catch (const std::future_error&) {
			}
			continue;
		}
		p->applier(pending.request, RpcResponder(pending.reply));
	}
}

RpcReply DebugRpcServer::awaitReply(const RpcRequest& request, RpcRouteId route) {
	if (!impl || !impl->started)
		return RpcReply{503, errorBody("server not started")};
	Impl* p = impl.get();
	auto reply = std::make_shared<std::promise<RpcReply>>();
	auto future = reply->get_future();
	{
		std::lock_guard<std::mutex> lock(p->mutex);
		if (!p->applier)
			return RpcReply{503, errorBody("no applier (server not wired)")};
		p->inFlight.push_back(reply);
		p->queue.push_back(Impl::Pending{request, reply});
	}

	// 超时 = 该端点在能力清单里登记的那一档（注册期把身份绑进闭包，故这里不必按路径反查）。
	// 帧边界没跑到就撤单 + 504；跨帧捕获与整批连拍各要多久由 schema 说话，本文件不再抄一份。
	const auto timeout = std::chrono::milliseconds(rpcRouteById(route).timeoutMs);
	if (future.wait_for(timeout) != std::future_status::ready) {
		// 尽力撤单：还躺在队列里就丢弃（已交 applier 的撤不掉——晚一步套用，无害）
		std::lock_guard<std::mutex> lock(p->mutex);
		auto queued = std::ranges::find_if(p->queue,
			[&reply](const Impl::Pending& e) { return e.reply == reply; });
		if (queued != p->queue.end())
			p->queue.erase(queued);
		auto tracked = std::ranges::find(p->inFlight, reply);
		if (tracked != p->inFlight.end())
			p->inFlight.erase(tracked);
		return RpcReply{504, errorBody("timeout: frame boundary did not run in time")};
	}
	{
		std::lock_guard<std::mutex> lock(p->mutex);
		auto tracked = std::ranges::find(p->inFlight, reply);
		if (tracked != p->inFlight.end())
			p->inFlight.erase(tracked);
	}
	try {
		return future.get();
	} catch (const std::future_error&) {
		return RpcReply{503, errorBody("reply dropped before it was produced")};
	}
}

}  // namespace RGL
