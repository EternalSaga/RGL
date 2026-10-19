export module debug_rpc;

import rlstd;
import rpc_schema_generated;  // RpcRouteId（注册期绑进 handler 闭包，等待侧据此取该端点超时）

namespace RGL {

/** RPC 请求（HTTP 线程构造 → 入队 → 帧边界渲染线程消费；body = 原始 UTF-8 JSON）。 */
export struct RpcRequest {
	std::string method;  // HTTP 方法（GET / POST）
	std::string path;    // 路径（端点名单 = rgl/rpc_schema.lua，本文件不抄）
	std::string body;    // 请求体（无 body = 空串）
};

/** RPC 应答载荷（promise 传递：HTTP 状态码 + JSON 响应体）。 */
export struct RpcReply {
	int status = 200;
	std::string body;
};

/**
 * 响应句柄（**可跨帧持有**——`/shot` 本帧收单、下帧捕获完成才结算）。
 *
 * 幂等性由 promise 语义保证：重复 resolve 抛 `promise_already_satisfied` 就地吞掉；
 * 持有方析构而未兑现 = 等待方拿 `broken_promise` 兜底（HTTP 侧转 503）；
 * `DebugRpcServer::stop()` 会主动把所有未兑现请求结算成 503，正常停机不等超时。
 */
export class RpcResponder {
  public:
	/** 空句柄（未绑定；resolve 为 no-op）。 */
	RpcResponder() = default;

	/**
	 * 绑定待兑现的 promise（传输层构造用）。
	 * @param reply promise 共享状态（非空）
	 */
	explicit RpcResponder(std::shared_ptr<std::promise<RpcReply>> reply)
		: reply(std::move(reply)) {}

	/**
	 * 兑现响应（幂等；任意线程可调，HTTP 线程在等它）。
	 * @param status HTTP 状态码（200/400/404/409/500…）
	 * @param body JSON 响应体（UTF-8）
	 */
	void resolve(int status, std::string body) const {
		if (!reply)
			return;
		try {
			reply->set_value(RpcReply{status, std::move(body)});
		} catch (const std::future_error&) {
			// 已兑现 / 已破坏：幂等 no-op（stop() 与 applier 竞态的正常出口）
		}
	}

	/** @return true = 已绑定（可兑现）。 */
	[[nodiscard]] bool bound() const { return reply != nullptr; }

  private:
	std::shared_ptr<std::promise<RpcReply>> reply;  // 共享状态（跨线程兑现）
};

/**
 * loopback HTTP 调试通道（Phase 10b 批 11b.2；docs/archive/phase10b_plan.md §4 批 11b.2）。
 *
 * **纯传输层**：只负责收请求 / 入队 / 超时与停机结算，不认识 registry、Vulkan、
 * 也不解析 JSON（协议执行 + 装配在 `debug_rpc_exec` 模块的 applier 里，同
 * ConfigReloadHook / PickingHook 函数注入模式——本模块**不 import ecs**）。
 *
 * 线程模型：httplib 连接线程收包 → [mutex] 队列 + `future.wait_for(超时)` →
 * 渲染线程 `poll()`（挂在 ConfigReloadHook 帧边界）drain 队列喂 applier →
 * applier 兑现 responder → HTTP 线程拿到应答。**registry 只在渲染线程被碰**。
 *
 * 生命周期：`start()` 前必须先 `setApplier()`；`stop()` 幂等（析构兜底）。
 * 门控在调用方（`diagnostics.rpc`，默认关 = 不构造，零行为变化）。
 */
export class DebugRpcServer {
  public:
	/** 帧边界执行器（渲染线程调用）：解析请求 → 套用 → 兑现 responder（可留到下帧）。 */
	using Applier = std::function<void(const RpcRequest&, RpcResponder)>;

	/** 默认构造 = 空壳（`impl` 已建、未 `start()`：无监听、请求不进队）。 */
	DebugRpcServer();
	/** 析构兜底 = stop()（异常路径不泄漏监听线程）。 */
	~DebugRpcServer();

	/** 禁拷贝（持有监听线程与队列，进程内唯一端点）。 */
	DebugRpcServer(const DebugRpcServer&) = delete;
	/** @see 禁拷贝 */
	DebugRpcServer& operator=(const DebugRpcServer&) = delete;

	/**
	 * 绑定端口 + 起监听线程（bind 同步做，故返回值可信）。
	 * @param host 绑定地址（调试面恒 loopback 127.0.0.1）
	 * @param port 端口（configs diagnostics.lua rpc_port）
	 * @return false = bind 失败（端口占用/无权限）→ 调用方日志降级关，不崩
	 */
	bool start(std::string host, int port);

	/** 停机（幂等）：未兑现请求全 503 → 关监听 → join 线程。正常路径帧循环后显式调用。 */
	void stop();

	/**
	 * 帧边界挂点（ConfigReloadHook 组合链，渲染线程）：drain 队列 → applier 逐条喂。
	 * 空队列 = 零开销。**applier 未装配时请求直接 503**（不入队悬挂）。
	 */
	void poll();

	/**
	 * 执行器装配（`start()` 之前；缺省空 = 全部请求 503）。
	 * @param applier 帧边界回调（捕获 registry/资产表/渲染器由调用方负责存活）
	 */
	void setApplier(Applier applier);

	/** @return 监听线程是否在跑（stop 后 = false）。 */
	[[nodiscard]] bool running() const;

  private:
	/** 传输层实现体（httplib 服务 + 队列 + 未兑现登记；只在 impl 单元可见）。 */
	struct Impl;
	std::unique_ptr<Impl> impl;  // pimpl：httplib 头只活在 impl 单元 GMF

	/**
	 * HTTP 线程侧等待（`start()` 注册的 handler 唯一实体）：入队 → 等 applier 兑现。
	 * 超时 = 该端点在能力清单里登记的那一档（到点撤单 + 504）；停机/无执行器 → 503。
	 * @param request 原始请求（方法 / 路径 / 体）
	 * @param route 命中的路由身份（注册期从生成表绑进闭包）
	 * @return 应答载荷（状态码 + JSON 体；HTTP 线程据此写响应）
	 */
	RpcReply awaitReply(const RpcRequest& request, RpcRouteId route);
};

}  // namespace RGL
