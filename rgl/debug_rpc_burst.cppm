module;
#include <nlohmann/json.hpp>  // 协议 JSON（第三方头，不受 GMF STL 白名单约束）
export module debug_rpc_burst;

import rlstd;

namespace RGL {

/**
 * `/shotburst` 协议里**与设备无关**的两段判据（形状解析 + 锚点裁决）。
 *
 * 为什么单独成模块（rlbase 层，零 ECS / 零渲染器）：这两段是连拍语义的**判据本体**，
 * 而宿主 `debug_rpc_exec` 是 app 层大文件（协议 + 装配 + 帧边界钩子），判据住在那里既
 * 顶破文件行数红线、又让「锚点裁决」这种纯逻辑只能靠起引擎才能验。拆出来后它可以在
 * `just test`（无 GPU 无窗口）里逐条钉死（tests/test_burst_protocol.cppm）。
 *
 * 分层依据：只 import rlstd + nlohmann 头 ⟹ 不认设备，归 `RGL_BASE_MODULE_SOURCES`。
 */

/** `/shotburst` 请求体的解析结果（形状层；状态套用与回读登记留在调用侧）。 */
export struct BurstRequest {
	uint32_t n = 0;                        ///< 张数（[1, 1024]）
	std::string dir;                       ///< 输出目录（相对引擎 CWD）
	std::optional<std::string> prefix;     ///< 文件名前缀；未传 = 不改上一次的（现行为）
	std::string attachment;               ///< 附件名；空 = swapchain 后缓冲路
	int gbuf = -1;                         ///< 回读通道（-1 = 后缓冲）
	uint32_t startFrame = 0;               ///< 债-89 锚点绝对帧号；0 = 无锚（现行为逐位不变）
};

/**
 * `attachment` 字段（temporal-motion 的 taa_out alpha 掩膜连拍）。
 *
 * 与 `gbuf` 互斥，同 /shot 的约定——两者都是「回读哪个源」，同时给必是客户端 bug，
 * 静默取一个会拍出没要的那张图。白名单只两条：`swapchain`（= 不传，旧行为逐位不变）
 * 与 `taa_out`（具名附件回读）。
 * @return 错误消息（空 = 通过；未传字段也算通过，out 置空）
 */
[[nodiscard]] std::string parseBurstAttachment(const nlohmann::json& body, std::string& out) {
	out.clear();
	if (!body.contains("attachment"))
		return {};
	if (body.contains("gbuf"))
		return "'attachment' and 'gbuf' are mutually exclusive";
	if (!body["attachment"].is_string())
		return "'attachment' must be a string";
	out = body["attachment"].get<std::string>();
	if (out.empty())
		return "'attachment' must be a non-empty name";
	if (out != "swapchain" && out != "taa_out")
		return "'attachment' only supports swapchain | taa_out";
	return {};
}

/**
 * `start_frame` 字段（债-89：连拍锚 = **绝对帧号**）。
 *
 * 为什么必须有它：不带该字段的 burst，起始帧 = 请求恰好落在第几帧（= 墙钟），而抖动相位 =
 * Halton(绝对帧号)、frame_yaw 机位 = 帧号纯函数 ⟹ 相位与机位每次启动都是彩票（亲验：同
 * 配置三跑边缘列飘 ±2px、一次整组判「ROI 里没有高对比边缘」）。本函数只管形状（400 类）；
 * 「锚已过」是状态冲突（409 类），归 `resolveBurstAnchor`。
 * @return 错误消息（空 = 通过；未传 = 0 = 现行为逐位不变）
 */
[[nodiscard]] std::string parseBurstStartFrame(const nlohmann::json& body, uint32_t& out) {
	out = 0;
	if (!body.contains("start_frame"))
		return {};
	const auto& v = body["start_frame"];
	if (!v.is_number_integer() || v.get<int64_t>() < 1 || v.get<int64_t>() > 0xFFFFFFFFLL)
		return "'start_frame' must be an integer in [1, 2^32)";
	out = static_cast<uint32_t>(v.get<int64_t>());
	return {};
}

/**
 * 解析 `/shotburst` 请求体的**形状**（400 类失效的唯一来源）。
 *
 * 错误消息与拆分前逐字相同（客户端按文案归因，文案也是契约的一部分）。
 * @param body 请求体（须为对象，由调用侧先判）
 * @param out 通过时写入解析结果
 * @return 错误消息（空 = 通过）
 */
export [[nodiscard]] std::string parseBurstRequest(const nlohmann::json& body, BurstRequest& out) {
	if (!body.contains("n") || !body.at("n").is_number_integer() || body.at("n").get<int>() < 1
		|| body.at("n").get<int>() > 1024)
		return "'n' must be an integer in [1, 1024]";
	if (!body.contains("dir") || !body.at("dir").is_string()
		|| body.at("dir").get<std::string>().empty())
		return "'dir' must be a non-empty path";
	out.n = static_cast<uint32_t>(body.at("n").get<int>());
	out.dir = body.at("dir").get<std::string>();
	out.prefix = body.contains("prefix")
		? std::optional<std::string>{ body.at("prefix").get<std::string>() }
		: std::nullopt;
	if (auto err = parseBurstAttachment(body, out.attachment); !err.empty())
		return err;
	out.gbuf = body.contains("gbuf") ? body.at("gbuf").get<int>() : -1;
	if (auto err = parseBurstStartFrame(body, out.startFrame); !err.empty())
		return err;
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
