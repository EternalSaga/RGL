module;
#include <nlohmann/json.hpp>  // 协议 JSON（第三方头，不受 GMF STL 白名单约束）
export module debug_rpc_burst;

import rlstd;
import rpc_schema_generated;  // 字段名常量 + 请求形状表（真相源 = rgl/rpc_schema.lua）
import debug_rpc_shape;       // rpcValidateRequest（形状判据本体）

namespace RGL {

/**
 * `/shotburst` 协议里**与设备无关**的两段判据（形状校验 + 锚点裁决）。
 *
 * 为什么单独成模块（rlbase 层，零 ECS / 零渲染器）：这两段是连拍语义的**判据本体**，
 * 而宿主 `debug_rpc_exec` 是 app 层大文件（协议 + 装配 + 帧边界钩子），判据住在那里既
 * 顶破文件行数红线、又让「锚点裁决」这种纯逻辑只能靠起引擎才能验。拆出来后它可以在
 * `just test`（无 GPU 无窗口）里逐条钉死（tests/test_burst_protocol.cppm）。
 *
 * 分层依据：只 import rlstd + 生成表 + nlohmann 头 ⟹ 不认设备，归 `RGL_BASE_MODULE_SOURCES`。
 *
 * 批 2 换血：形状（哪些字段、什么类型、什么区间、谁与谁互斥、白名单收哪几档）搬进
 * `rgl/rpc_schema.lua`，本文件不再写任何一条形状文案；留在这里的是**取值**（把已验过的
 * JSON 搬进 `BurstRequest`）与**锚点裁决**（依赖运行期帧号，不是请求形状，进不了 schema）。
 */

/** `/shotburst` 请求体的解析结果（形状已由生成表验过；本结构只装值）。 */
export struct BurstRequest {
	uint32_t n = 0;                        ///< 张数（[1, 1024]）
	std::string dir;                       ///< 输出目录（相对引擎 CWD）
	std::optional<std::string> prefix;     ///< 文件名前缀；未传 = 不改上一次的（现行为）
	std::string attachment;               ///< 附件名；空 = swapchain 后缓冲路
	int gbuf = -1;                         ///< 回读通道（-1 = 后缓冲）
	uint32_t startFrame = 0;               ///< 债-89 锚点绝对帧号；0 = 无锚（现行为逐位不变）
};

/**
 * 解析 `/shotburst` 请求体的**形状**并取值。
 *
 * 形状判据 = `rgl/rpc_schema.lua` 的 shotburst 段（字段名 / 类型 / 必填 / 区间 / 互斥 /
 * 白名单 + 六条归因文案），经生成表由 `rpcValidateRequest` 执行。错误消息与拆分前逐字相同
 * （客户端按文案归因，文案也是契约的一部分）。
 *
 * 🔴 本函数**必须**自己调一次校验，不能只信分发层：`tests/test_burst_protocol.cppm` 不经
 * HTTP 直接喂 body 给它，形状判据少一次调用 = 六条文案当场全红（这条锚就是用来发现
 * "schema 变装饰"的）。分发层那次兜的是另一头：新端点忘在 handler 里调校验。
 *
 * @param body 请求体（须为对象，由生成表那条 is_object 闸门管）
 * @param out 通过时写入解析结果
 * @return 错误消息（空 = 通过）
 */
export [[nodiscard]] std::string parseBurstRequest(const nlohmann::json& body, BurstRequest& out) {
	if (auto err = rpcValidateRequest(RpcRouteId::shotburst, body); !err.empty())
		return err;
	out.n = static_cast<uint32_t>(body.at(rpc_field::shotburst::n).get<int>());
	out.dir = body.at(rpc_field::shotburst::dir).get<std::string>();
	out.prefix = body.contains(rpc_field::shotburst::prefix)
		? std::optional<std::string>{ body.at(rpc_field::shotburst::prefix).get<std::string>() }
		: std::nullopt;
	// attachment 与 gbuf 的互斥、attachment 的白名单都在表里；走到这里两者可以共存地读
	out.attachment = body.contains(rpc_field::shotburst::attachment)
		? body.at(rpc_field::shotburst::attachment).get<std::string>()
		: std::string{};
	out.gbuf = body.contains(rpc_field::shotburst::gbuf)
		? body.at(rpc_field::shotburst::gbuf).get<int>()
		: -1;
	out.startFrame = body.contains(rpc_field::shotburst::start_frame)
		? static_cast<uint32_t>(body.at(rpc_field::shotburst::start_frame).get<int64_t>())
		: 0u;
	return {};
}

/**
 * 锚点裁决（409 类失效的唯一来源）：把请求锚点换算成「装配态里要不要挂等待」。
 *
 * `== frameNow` 还来得及：请求在帧首钩子里 drain，本帧 drawFrame 尚未跑。锚已过 = 采集点
 * 已经丢了，按相对锚交图 = 把彩票藏进读数里 ⟹ 报错让调用侧当场炸，**绝不静默退回相对**。
 * @param frameNow 引擎当前帧计数（帧首钩子里的值）
 * @param startFrame 请求锚点（0 = 无锚）
 * @param armed 写入要存进装配态的等待锚点（0 = 无锚/立即起跑）
 * @return 错误消息（空 = 通过）
 */
export [[nodiscard]] std::string resolveBurstAnchor(uint32_t frameNow, uint32_t startFrame,
	uint32_t& armed) {
	if (startFrame != 0 && startFrame < frameNow)
		return "start_frame " + std::to_string(startFrame) + " already passed (frame "
			+ std::to_string(frameNow)
			+ ") — burst 锚点是绝对帧号：更早发请求或抬高锚点，不许静默退回相对";
	armed = startFrame > frameNow ? startFrame : 0u;
	return {};
}

}  // namespace RGL
