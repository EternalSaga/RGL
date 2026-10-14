module;
#include <vulkan/vulkan.h>  // VkBuffer / VK_WHOLE_SIZE
#include <fmt/format.h>
module offgraph_sync:impl;

import offgraph_sync;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;

namespace RGL {

namespace {

/** 统一错误构造：`ErrorCode::RenderGraphInvalid` + message（同 B1a 的 `offgraph_contract`）。 */
Error syncError(std::string message) {
	return Error{ErrorCode::RenderGraphInvalid, std::move(message)};
}

}  // namespace

RgSyncMasks syncUsageMasks(RgSyncUsage usage) {
	using Stage = vk::PipelineStageFlagBits2;
	using Acc = vk::AccessFlagBits2;
	switch (usage) {
	case RgSyncUsage::computeWrite:
		return {Stage::eComputeShader, Acc::eShaderWrite};
	case RgSyncUsage::computeRead:
		return {Stage::eComputeShader, Acc::eShaderRead};
	case RgSyncUsage::computeReadWrite:
		return {Stage::eComputeShader, Acc::eShaderRead | Acc::eShaderWrite};
	case RgSyncUsage::fragmentRead:
		return {Stage::eFragmentShader, Acc::eShaderRead};
	case RgSyncUsage::anyWriteGlobal:
		// 宽档：EVSSM 的阴影图**本模块无句柄**（归 RT 缓存），depth-write → compute-read
		// 这条序只能表达成"全局写 → compute 读"，access 用 eMemoryWrite 通配。
		// ⚠ 不许悄悄当默认档用——它比真实访问宽，宽出来的部分是本模块白付的同步成本。
		return {Stage::eAllGraphics, Acc::eMemoryWrite};
	default:
		// 只可能是 `count`（遍历哨兵）。写出来而不是留空：新增原子忘了配掩码时，
		// 这里会静默给出 eTopOfPipe/eNone = 一条**什么都不挡**的 barrier，那种错必须靠
		// 单测「词表逐位」那条红抓住，而不是靠编译器（编译器只会给个 -Wswitch 警告）。
		return {Stage::eTopOfPipe, Acc::eNone};
	}
}

const RgSyncEdgeDesc& syncEdgeDesc(RgSyncEdgeId id) {
	// 顺序必须与 `RgSyncEdgeId` 逐行对齐。表长 = 枚举 `count` 是编译期绑定，但**少写一行不会
	// 编译失败**（剩余元素值初始化成空 name）⟹ 真正的护栏是单测「边表逐条钉死」：它按每个
	// 边 id 断言 name 与四条掩码，漏行会红在"name 是空的"上（`REQUIRE` 还钉住了行数）。
	static const std::array<RgSyncEdgeDesc, static_cast<size_t>(RgSyncEdgeId::count)> table = {
		RgSyncEdgeDesc{
			.name = "evssm_depth_to_compute",
			.scope = RgSyncScope::executionOrder,
			.src = RgSyncUsage::anyWriteGlobal,
			.dst = RgSyncUsage::computeRead,
			.gate = RgSyncGate::evssm,
			.expectedPerFrame = 1,
		},
		RgSyncEdgeDesc{
			.name = "evssm_moments_chain",
			.scope = RgSyncScope::executionOrder,
			.src = RgSyncUsage::computeWrite,
			.dst = RgSyncUsage::computeReadWrite,
			.gate = RgSyncGate::evssm,
			// 默认 1；真实次数 = mipLevels−1，由 `EvssmShadow` 构造期 `expectCount` 覆写
			// （构造期而不是录制期：录制期申报 = 自己出题自己答，判据会自证）。
			.expectedPerFrame = 1,
		},
		RgSyncEdgeDesc{
			.name = "evssm_moments_to_light",
			.scope = RgSyncScope::executionOrder,
			.src = RgSyncUsage::computeWrite,
			.dst = RgSyncUsage::fragmentRead,
			.gate = RgSyncGate::evssm,
			.expectedPerFrame = 1,
		},
	};
	return table[syncEdgeIndex(id)];
}

void OffGraphSync::beginFrame() {
	counts.fill(0);
}

void OffGraphSync::expectCount(RgSyncEdgeId edge, uint32_t expected_) {
	expected[syncEdgeIndex(edge)] = expected_;
}

void OffGraphSync::emit(vk::raii::CommandBuffer& cmd, RgSyncEdgeId edge) {
	const auto& desc = syncEdgeDesc(edge);
	const auto src = syncUsageMasks(desc.src);
	const auto dst = syncUsageMasks(desc.dst);
	vk::MemoryBarrier2 barrier{};
	barrier.srcStageMask = src.stage;
	barrier.srcAccessMask = src.access;
	barrier.dstStageMask = dst.stage;
	barrier.dstAccessMask = dst.access;
	vk::DependencyInfo dep{};
	dep.setMemoryBarriers(barrier);
	cmd.pipelineBarrier2(dep);
	recordEmission(edge);  // 记账与发射同一行距离 = 「申报了却没发」写不出来
}

void OffGraphSync::emit(vk::raii::CommandBuffer& cmd, RgSyncEdgeId edge, vk::Buffer buffer) {
	const auto& desc = syncEdgeDesc(edge);
	const auto src = syncUsageMasks(desc.src);
	const auto dst = syncUsageMasks(desc.dst);
	vk::BufferMemoryBarrier2 barrier{};
	barrier.srcStageMask = src.stage;
	barrier.srcAccessMask = src.access;
	barrier.dstStageMask = dst.stage;
	barrier.dstAccessMask = dst.access;
	barrier.buffer = buffer;
	barrier.size = VK_WHOLE_SIZE;  // 整块（口径出处见接口注释）
	vk::DependencyInfo dep{};
	dep.setBufferMemoryBarriers(barrier);
	cmd.pipelineBarrier2(dep);
	recordEmission(edge);  // 同上：buffer 档同样不可分割
}

std::expected<void, Error> OffGraphSync::checkEmission(std::span<const bool> gateActive) const {
	if (gateActive.size() != kSyncGateCount)
		return std::unexpected(syncError(fmt::format(
			"off-graph sync: 对账的 gateActive 长度 {} ≠ 门数 {}（新增门必须同步帧末门位表）",
			gateActive.size(), kSyncGateCount)));
	for (uint32_t i = 0; i < kEdgeCount; ++i) {
		const auto& desc = syncEdgeDesc(static_cast<RgSyncEdgeId>(i));
		const bool active = gateActive[static_cast<size_t>(static_cast<uint8_t>(desc.gate))];
		const uint32_t got = counts[i];
		const uint32_t want = expected[i];
		if (active && got != want)
			return std::unexpected(syncError(fmt::format(
				"off-graph sync: 边 '{}' 门开着却发了 {} 次，应发 {} 次"
				"（少发 = 漏同步 = 未定义行为；多发 = 白付同步成本；两种都要当场红）",
				desc.name, got, want)));
		if (!active && got != 0)
			return std::unexpected(syncError(fmt::format(
				"off-graph sync: 边 '{}' 门关着却发了 {} 次 = 破逐位不变红线（门关必须零命令）",
				desc.name, got)));
	}
	return {};
}

}  // namespace RGL
