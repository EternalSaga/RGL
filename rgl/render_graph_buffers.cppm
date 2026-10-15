module;
#include <fmt/format.h>
module render_graph:buffers;

import render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;

// buffer 附件的**按名注册表**分区（批次 B2-3 本体；契约与"为什么是函数不是数值"、"为什么这张
// 表同时就是拒绝面"逐字写在 `render_graph.cppm` 的接口注释里，本文件只放数据面与四个访问器）。
//
// 为什么单独成一个分区而不是塞进 `render_graph_impl`：impl 已经 693 行（本仓 500 代码行红线早过，
// 靠 `render_graph_formats` 那次拆出才没继续恶化），而这张表**与编译算法零耦合**——它只被
// `registerAttachment` 的一个分支和模块侧的对账调用消费。把"资源事实"与"布局链算法"分文件，
// 下一个读 impl 的人不必先读一张无关的 map。
//
// 为什么表体住实现分区而不是接口：`std::function<vk::DeviceSize()>` 里的 `vk::DeviceSize` 来自
// rlvk 的 GMF 头，与 `vk::Format::eXxx` 同一个"可达但名字不可见"的限制（见
// `render_graph_formats.cppm` 头注释）。

namespace RGL {

namespace {

/**
 * 表本体：附件名 → 字节数来源。
 *
 * 用 `std::map` 而不是 `unordered_map`：条目数量级是"个位数到十几"，而 map 的遍历序稳定，
 * 出问题时打日志的次序可复现（本仓对"日志次序漂"的容忍度是零——逐字节断言的测试消息就吃过这个亏）。
 */
struct BufferRegistry {
	std::map<std::string, std::function<vk::DeviceSize()>> entries;
};

/** 进程级单例（启动期单线程写、运行期只读，理由 → 接口注释）。 */
BufferRegistry& registry() {
	static BufferRegistry instance;
	return instance;
}

}  // namespace

/**
 * 登记一张 buffer：附件名 → 字节数来源（契约与"为什么是函数不是数值"见接口注释）。
 * @param name  附件名（图侧 `kind=buffer` 的附件按它匹配）
 * @param sizeOf 字节数来源（**按值捕获**容量，勿捕获模块 `this`——注册表活得比模块久）
 */
void registerBufferAttachment(std::string_view name, std::function<vk::DeviceSize()> sizeOf) {
	registry().entries[std::string(name)] = std::move(sizeOf);
}

/**
 * 名字是否已登记（= `registerAttachment` 对 `kind=buffer` 的拒绝面用的那个谓词）。
 * @param name 附件名
 * @return true = 已登记
 */
bool isBufferAttachmentRegistered(std::string_view name) {
	const auto& entries = registry().entries;
	return entries.find(std::string(name)) != entries.end();
}

/**
 * 取登记字节数（调用时求值）。
 * @param name 附件名
 * @return 字节数；未登记 = nullopt（**不兜 0**，兜 0 会让对账拿编出来的数比大小）
 */
std::optional<vk::DeviceSize> bufferAttachmentBytes(std::string_view name) {
	const auto& entries = registry().entries;
	const auto it = entries.find(std::string(name));
	if (it == entries.end())
		return std::nullopt;
	return (it->second)();
}

/**
 * 对账「注册字节数 ≥ dispatch 写上界」（单向；启动期调用，契约见接口注释）。
 * @param name       附件名
 * @param writeBound 该 dispatch 一次最多写进这段 buffer 的字节数
 * @return 空值 = 通过；否则 Error（消息点名两侧字节数与差值）
 */
std::expected<void, Error> checkBufferWriteBound(std::string_view name, vk::DeviceSize writeBound) {
	const auto bytes = bufferAttachmentBytes(name);
	if (!bytes)
		return std::unexpected(Error{ErrorCode::RenderGraphInvalid, fmt::format(
			                               "buffer registry: '{}' 未注册就无法对账写上界（注册表 = "
			                               "rgl/render_graph_buffers.cppm，注册入口 "
			                               "registerBufferAttachment）",
			                               name)});
	if (*bytes >= writeBound)
		return {};
	// 消息里两侧字节数与差值都给全：这条是启动期抛出的，现场只剩这一行日志，
	// 让人一眼看出"是容量算小了"还是"是写上界申报大了"（两个方向的修复位置不同）。
	return std::unexpected(Error{ErrorCode::RenderGraphInvalid, fmt::format(
		                               "buffer registry: '{}' 的写上界 {}B 超过注册字节数 {}B（差 {}B）"
		                               "—— dispatch 会写到 buffer 之外的设备内存：不崩、不触发 VVL，"
		                               "只在画面上留下解释不了的垃圾。要么容量算漏了，要么写上界申报错"
		                               "了（对账方向是单向：注册 ≥ 写上界，注册偏大不判红）",
		                               name, writeBound, *bytes, writeBound - *bytes)});
}

}  // namespace RGL
