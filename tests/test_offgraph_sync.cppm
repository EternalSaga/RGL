module;
/**
 * test_offgraph_sync —— 图外同步的申报即发射（批次 B1b 第一刀 → docs/compute_调研.md §7.2/§7.3）
 *
 * 被测 = `rgl/offgraph_sync.cppm`（rlgfx 成员）里**零设备**的那三面：usage 词表（掩码物化）、
 * 边表（命令流的唯一来源）、发射账（`recordEmission` / `checkEmission`）。`emit` 本体要
 * `CommandBuffer` ⟹ 不进本文件，由真程序变异覆盖（口径见文末）。
 *
 * ## 为什么值得测
 *
 * 这一层的卖点是「barrier 参数由词表物化，模块不写 mask 字面量」。这句话本身**就是一次语义
 * 改写**——迁移时只要有一个 bit 抄错，画面可能今天不变、换个驱动/换台机器才变。所以本文件的
 * 主要职责不是"测逻辑"，而是**钉死"迁移没改语义"**。
 *
 * ## 期望值的出处（不抄被测码）
 *
 * 全部掩码期望值 = **迁移前** `rgl/evssm_shadow_impl.cppm` 里 `stageBarrier(...)` 三处调用点的
 * 字面量（本文件写作时它们已从被测码删除，git 可查）。应发次数同理：`mipLevels − 1` 来自 ③
 * 那个循环的圈数（`level = 1; level < mipLevels`）。
 *
 * ## 变异红线（哪个变异让我红；答不出 = 摆设）
 *
 *   1. **词表改一位**：`computeReadWrite` 的 access 去掉 `eShaderWrite`（或 `anyWriteGlobal`
 *      的 stage 从 `eAllGraphics` 改成 `eComputeShader`）→ 「词表逐位」与「边物化出的掩码对」
 *      两条同时红。这条是本次迁移的核心风险，必须有。
 *   2. **边的 src/dst 互换**：`evssm_depth_to_compute` 的 src/dst 对调 → 边表用例红
 *      （对调后与旧代码的 src=AG / dst=CS 不符）。
 *   3. **对账少半边**：删 `active && got != want` → 「门开少发」红；删 `!active && got != 0`
 *      → 「门关却发」红（后者守逐位不变红线：门关必须零命令）。
 *   4. **只比"有没有"不比次数**：把 `got != want` 弱化成 `got == 0` → 「门开多发」那条红。
 *      它保证 L1 真的是**次数**账，而不是 B1a 那个布尔账的复制品。
 *   5. `beginFrame` 不清账 → 跨帧那条红（第二帧少发必须红）。
 *   6. `expectCount` 不生效（或 `expected` 初值被 `beginFrame` 一起清掉）→ 「构造期申报 7 次」
 *      那组里的两条红（发 7 次反而报违规 / 发 1 次就绿）。
 *   7. 门位表长度不校验 → 「长度不符」那条红（新增门而帧末没跟上 = 静默漏对账）。
 *   8. 词表养闲原子（加了没人用的条目）→ 「每个原子都被某条边用到」那条红。
 *
 * 🔴 **本文件测不到的**（说清楚，别把"全绿"读成"同步对了"）：`emit` 里有没有忘调
 * `recordEmission`，以及"该发 barrier 的地方根本没写边"。前者由真程序变异覆盖（删一次
 * `sync.emit` ⟹ 帧末必须红，门开配方见 §7.1）；后者是 L2 的活，本层明确不做（§7.3）。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
#include <string_view>

export module test_offgraph_sync;

import rlstd;
import rlvk;  // vk::PipelineStageFlagBits2 / vk::AccessFlagBits2（期望值字面量要用）
import offgraph_sync;
import vkerror;  // Error（expected 的失败侧）

namespace RGL {
namespace {

using Stage = vk::PipelineStageFlagBits2;
using Acc = vk::AccessFlagBits2;

/** 失败消息（绿 = 空串）；口径同 `test_offgraph_contract`。 */
std::string messageOf(const std::expected<void, Error>& result) {
	return result.has_value() ? std::string{} : result.error().message;
}

/** 发 N 次某条边（不碰命令缓冲：判据核经 `recordEmission` 驱动，见接口注释）。 */
void emitTimes(OffGraphSync& sync, RgSyncEdgeId edge, uint32_t times) {
	for (uint32_t i = 0; i < times; ++i) sync.recordEmission(edge);
}

/** 门位数组（本批次只有一个门 = EVSSM 在场）。 */
constexpr std::array<bool, kSyncGateCount> gates(bool evssmActive) {
	return {evssmActive};
}

}  // namespace

TEST_CASE("offgraph_sync: usage 词表逐位钉死（期望值 = 迁移前 evssm_shadow_impl 字面量）") {
	// E1 的 src：旧代码 `stageBarrier(cmd, eAllGraphics, eMemoryWrite, ...)`
	CHECK(syncUsageMasks(RgSyncUsage::anyWriteGlobal).stage == Stage::eAllGraphics);
	CHECK(syncUsageMasks(RgSyncUsage::anyWriteGlobal).access == Acc::eMemoryWrite);
	// E1 的 dst：`eComputeShader, eShaderRead`
	CHECK(syncUsageMasks(RgSyncUsage::computeRead).stage == Stage::eComputeShader);
	CHECK(syncUsageMasks(RgSyncUsage::computeRead).access == Acc::eShaderRead);
	// E2 / E3 的 src：`eComputeShader, eShaderWrite`
	CHECK(syncUsageMasks(RgSyncUsage::computeWrite).stage == Stage::eComputeShader);
	CHECK(syncUsageMasks(RgSyncUsage::computeWrite).access == Acc::eShaderWrite);
	// E2 的 dst：`eComputeShader, eShaderRead | eShaderWrite`（**两个位都要在**）
	CHECK(syncUsageMasks(RgSyncUsage::computeReadWrite).stage == Stage::eComputeShader);
	CHECK(syncUsageMasks(RgSyncUsage::computeReadWrite).access ==
		(Acc::eShaderRead | Acc::eShaderWrite));
	// E3 的 dst：`eFragmentShader, eShaderRead`
	CHECK(syncUsageMasks(RgSyncUsage::fragmentRead).stage == Stage::eFragmentShader);
	CHECK(syncUsageMasks(RgSyncUsage::fragmentRead).access == Acc::eShaderRead);

	// 变异 8：词表不养闲原子——每个原子至少被某条边的某一侧用到。加原子而没人用 = 白占一个
	// "看起来已覆盖"的名额，B2 铺 IZB / GpuCull 时会被当成现成的档乱用。
	for (uint32_t u = 0; u < static_cast<uint32_t>(RgSyncUsage::count); ++u) {
		const auto usage = static_cast<RgSyncUsage>(u);
		bool used = false;
		for (uint32_t e = 0; e < OffGraphSync::kEdgeCount; ++e) {
			const auto& desc = syncEdgeDesc(static_cast<RgSyncEdgeId>(e));
			used = used || desc.src == usage || desc.dst == usage;
		}
		CHECK_MESSAGE(used, "usage 原子未被任何边使用（要么接上客户，要么删掉这条）");
	}
}

TEST_CASE("offgraph_sync: 边表逐条钉死 = 旧代码三处 stageBarrier 的等价形态") {
	struct Expect {
		RgSyncEdgeId id;
		std::string_view name;
		vk::PipelineStageFlags2 srcStage;  // 用 Flags<> 而非位枚举：`a | b` 物化出的就是 Flags<>
		vk::AccessFlags2 srcAccess;
		vk::PipelineStageFlags2 dstStage;
		vk::AccessFlags2 dstAccess;
	};
	// 逐字段出处：迁移前 evssm_shadow_impl.cppm 的 ①/③/④ 三处调用点。
	constexpr Expect expects[] = {
		{RgSyncEdgeId::evssmDepthToCompute, "evssm_depth_to_compute", Stage::eAllGraphics,
			Acc::eMemoryWrite, Stage::eComputeShader, Acc::eShaderRead},
		{RgSyncEdgeId::evssmMomentsChain, "evssm_moments_chain", Stage::eComputeShader,
			Acc::eShaderWrite, Stage::eComputeShader, Acc::eShaderRead | Acc::eShaderWrite},
		{RgSyncEdgeId::evssmMomentsToLight, "evssm_moments_to_light", Stage::eComputeShader,
			Acc::eShaderWrite, Stage::eFragmentShader, Acc::eShaderRead},
	};
	// 边数也要钉：加边不写用例 = 新边没人验（这条红比"某条边写错"更常见）。
	REQUIRE(std::size(expects) == OffGraphSync::kEdgeCount);

	for (const auto& want : expects) {
		INFO("edge #" << static_cast<int>(want.id) << " = " << want.name);
		const auto& desc = syncEdgeDesc(want.id);
		CHECK(desc.name == want.name);
		// EVSSM 三条全是无句柄纯执行序（矩图 GENERAL 零转移、阴影图本模块无句柄）
		CHECK(desc.scope == RgSyncScope::executionOrder);
		CHECK(desc.gate == RgSyncGate::evssm);
		// 🔴 判据本体：边**物化出的四条掩码** == 旧代码那四条。不是只钉"边指向哪两个原子"——
		// 那还要再经过一次词表，红了得两处对照才知道哪一处错。
		CHECK(syncUsageMasks(desc.src).stage == want.srcStage);
		CHECK(syncUsageMasks(desc.src).access == want.srcAccess);
		CHECK(syncUsageMasks(desc.dst).stage == want.dstStage);
		CHECK(syncUsageMasks(desc.dst).access == want.dstAccess);
	}
}

TEST_CASE("offgraph_sync: 门开着 ⟺ 发够次数（L1 的两个半边）") {
	const auto on = gates(true);
	OffGraphSync sync;

	// 阳性锚点：默认应发次数（边表 1/1/1）全发够 ⟹ 必须绿
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight, 1);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());

	// 半边 A：门开着少发 = 漏同步 = 未定义行为，必须当场红并点名边
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight, 1);
	const auto low = messageOf(sync.checkEmission(std::span<const bool>{on}));
	REQUIRE_FALSE(low.empty());
	CHECK(low.find("evssm_moments_chain") != std::string::npos);

	// 变异 4：把"次数"弱化成"有没有"，下面这条是唯一能抓住它的用例
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 2);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight, 1);
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{on}).has_value());
}

TEST_CASE("offgraph_sync: 门关着必须零命令（逐位不变红线的同步侧）") {
	const auto off = gates(false);
	OffGraphSync sync;
	// 门关 + 一条没发 = 唯一合法形态
	sync.beginFrame();
	CHECK(sync.checkEmission(std::span<const bool>{off}).has_value());

	// 门关却发了 = 门关时命令流多了一条 barrier = 破逐位不变
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight, 1);
	const auto bad = messageOf(sync.checkEmission(std::span<const bool>{off}));
	REQUIRE_FALSE(bad.empty());
	CHECK(bad.find("evssm_moments_to_light") != std::string::npos);
}

TEST_CASE("offgraph_sync: 构造期申报的次数跨帧生效，本帧计数每帧清零") {
	const auto on = gates(true);
	OffGraphSync sync;
	// EVSSM 构造期那句 `expectCount(chain, mipLevels - 1)` 的等价现场（mipLevels = 8）
	sync.expectCount(RgSyncEdgeId::evssmMomentsChain, 7);

	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 7);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight, 1);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());
	CHECK(sync.emitted(RgSyncEdgeId::evssmMomentsChain) == 7);

	// 变异 5：`beginFrame` 若不清账，第二帧会拿上一帧剩下的 7 被读成"够"
	sync.beginFrame();
	CHECK(sync.emitted(RgSyncEdgeId::evssmMomentsChain) == 0);
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 6);  // 循环少跑一圈（mip 数被改动）
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{on}).has_value());

	// 变异 6：应发次数是**启动期**事实，若被 beginFrame 一起清回 1，这条会误判成绿
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight, 1);
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{on}).has_value());
}

TEST_CASE("offgraph_sync: 门位表长度不符即错（新增门而帧末没跟上 = 静默漏对账）") {
	const auto on = gates(true);
	OffGraphSync sync;
	// 先造一个"次数全发够"的现场，好让下面两条红只可能来自长度这一件事。
	sync.beginFrame();
	emitTimes(sync, RgSyncEdgeId::evssmDepthToCompute, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsChain, 1);
	emitTimes(sync, RgSyncEdgeId::evssmMomentsToLight, 1);
	CHECK(messageOf(sync.checkEmission(std::span<const bool>{on})).empty());

	// 多一位 = 新增了门而帧末门位表没扩。此时次数**是够的**，所以"删掉长度校验"这个变异
	// 会让下面这条变绿（gateActive[0] 仍读到 true）⟹ 它真的在守东西。
	const auto tooMany = [] {
		std::array<bool, kSyncGateCount + 1> a{};
		a[0] = true;
		return a;
	}();
	const auto bad = messageOf(sync.checkEmission(std::span<const bool>{tooMany}));
	REQUIRE_FALSE(bad.empty());
	// 消息必须点名**长度**，不能和"次数不符"共用一句——否则红了还得猜是哪种错
	CHECK(bad.find("gateActive") != std::string::npos);

	// 空数组同理（长度校验被删时这里会读越界 ⟹ 这条守的是"别静默"）
	CHECK_FALSE(sync.checkEmission(std::span<const bool>{}).has_value());
}

}  // namespace RGL
