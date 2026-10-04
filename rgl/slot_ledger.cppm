module;
export module slot_ledger;

import rlstd;
import vkerror;

// 按归属者键控的 descriptor-heap 槽位台账（**叶子模块**：纯逻辑、无 Vulkan 类型、无日志、
// 无设备依赖 → 属 rlbase，可在 `just test` 里毫秒级测）。
//
// ## 它存在的唯一理由（2026-10-04 env sampler 槽相撞事故）
//
// sampler heap 的用户槽原本由每个使用者**自己算**：
//
//     skySamplerSlot   = samplerHeap.getReservedSlots() + 1;   // environment.cppm
//     pointSamplerSlot = samplerHeap.getReservedSlots() + 1;   // sampler_library.cppm
//
// 两处注释都自称"共享 sampler 之后第二槽"，**谁都没错，撞了没人管**。而两个 ensure* 都有
// 幂等守卫（`if (ready) return slot;`）⟹ 后写者永久赢、先写者再也写不回来：env 镜面项的
// sampler 被 AO 的 NEAREST + maxLod=0 覆写，GGX 预滤波 mip 链自 Phase 11 批 11.4 起一次
// 都没被采样过（`env.mipRange` 扫 0..20 画面逐字节不变 = 该 bug 的签名）。
//
// 根因不是那个 `+1` 写错了，是**"槽号由使用者各自算出"这个形态**没有任何机制阻止两个
// 归属者算出同一个数。所以本模块把领槽变成**申报制**：归属者只报名字，槽号由台账发。
//
// ## 三条不变式（每条都有单测，见 tests/test_slot_ledger.cppm）
//
//   1. 同一 owner 重复领 → 恒返回同一槽（保持既有 ensure* 的幂等语义）；
//   2. 不同 owner → 槽号两两不同（**结构上不可能相撞**，这是护栏本体）；
//   3. 超出容量 → 返回 Error 且**不消耗槽号**（先查界后发号；失败绝不静默越界）。
//
// ## 分配顺序 = 首次申报的先后（确定性契约）
//
// 槽号会烘进管线（`shader_registry_impl.cppm` 的 `envSamplerHeapOffset` 等），所以跨运行
// 必须稳定。本台账不排序、不哈希，纯按**首次申报顺序**发号 ⟹ 槽号由启动流程的调用顺序
// 唯一决定，而启动流程是单线程固定顺序。`Resources::claimSamplerSlot` 每次发放打一行日志、
// 启动末尾可 dump 全表 ⟹ 顺序若被改动，日志现场即可回归比对。

namespace RGL {

/** 一条已发放槽位的申报记录（归属者名 + 实际槽号）。 */
export struct SlotClaim {
	std::string owner;  ///< 归属者名（人话，进日志；如 "sampler.skyEnv"）
	uint32_t slot = 0;  ///< 台账发放的绝对槽号（含 heap reserved 偏移）
};

/**
 * 按归属者键控的 descriptor-heap 槽位台账（申报制领槽，纯逻辑）。
 *
 * 用途边界：适合**数量少、终身稳定、名字已知**的槽（sampler heap 用户槽容量只有 8，全是
 * 这一类）。数量随场景内容涨的槽（模型顶点 / 纹理 / 运行期辅助槽）仍走
 * `Resources::allocSlot` 的分段游标——那条已经有段间隔离，且不需要"按名字幂等"。
 *
 * 线程模型：非线程安全（启动期单线程领槽，与 `Resources::allocSlot` 同前提）。
 */
export class SlotLedger {
    public:
	/**
	 * 建空台账。
	 * @param firstSlot 首个可领的绝对槽号（= heap 的 reservedSlots：驱动保留区
	 *   [0, reservedSlots) 声明为不可用，领进去 = 覆写驱动数据）
	 * @param slotCount 可领槽总数（= heap capacity − reservedSlots）
	 */
	SlotLedger(uint32_t firstSlot, uint32_t slotCount) : firstSlot(firstSlot), slotCount(slotCount) {}

	/**
	 * 按归属者领槽（申报制）。
	 *
	 * 幂等：同一 owner 第二次及以后领槽**不检查容量**、直接返回既有槽号（否则"重复领"会被
	 * 误报成"容量不足"，而幂等恰恰是调用方 ensure* 守卫撤掉后唯一的保护）。
	 * 新 owner 才占一个新槽，槽号 = firstSlot + 已发放数（首次申报顺序决定）。
	 * @param owner 归属者名（区分大小写、整串比较——"ao" 与 "aoPoint" 是两个归属者）
	 * @return 绝对槽号；容量耗尽 → DescriptorSlotOutOfBounds（**不消耗槽号**，
	 *   已发放的申报也不受影响）
	 */
	[[nodiscard]] std::expected<uint32_t, Error> claimSlot(std::string_view owner) {
		for (const auto& claim : claims) {
			if (claim.owner == owner)
				return claim.slot;
		}
		const uint32_t index = static_cast<uint32_t>(claims.size());
		if (index >= slotCount)
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				"sampler heap slot capacity exceeded: owner '" + std::string(owner)
					+ "' 想要第 " + std::to_string(index + 1) + " 个用户槽，容量仅 "
					+ std::to_string(slotCount) + "（首槽 " + std::to_string(firstSlot) + "）"});
		const uint32_t slot = firstSlot + index;
		claims.push_back(SlotClaim{std::string(owner), slot});
		return slot;
	}

	/** @return 已发放槽位数（= 下一个新 owner 会拿到的序号）。 */
	[[nodiscard]] uint32_t used() const {
		return static_cast<uint32_t>(claims.size());
	}

	/** @return 可领槽总数（不含驱动保留区）。 */
	[[nodiscard]] uint32_t capacity() const {
		return slotCount;
	}

	/** @return 首个可领的绝对槽号（= heap reservedSlots）。 */
	[[nodiscard]] uint32_t base() const {
		return firstSlot;
	}

	/** @return 全部申报记录（按发放顺序），供启动日志 dump 与单测断言。 */
	[[nodiscard]] const std::vector<SlotClaim>& entries() const {
		return claims;
	}

	/**
	 * 全表人话摘要（"shared→0 skyEnv→1 aoPoint→2"），启动日志用。
	 *
	 * 为什么值得打：槽号会烘进管线，而它只由**调用顺序**决定。没有这张表，顺序被改动时
	 * 现场看不出任何异常（画面不变、VVL 不响）；有了它，跨运行的 diff 就是一条日志的事。
	 * @return 按发放顺序拼的字符串；空台账返回 "(none)"
	 */
	[[nodiscard]] std::string describe() const {
		if (claims.empty())
			return "(none)";
		std::string out;
		for (const auto& claim : claims) {
			if (!out.empty())
				out += " ";
			out += claim.owner + "→" + std::to_string(claim.slot);
		}
		return out;
	}

    private:
	uint32_t firstSlot = 0;  ///< 首个可领绝对槽号（构造期 = sampler heap reservedSlots）
	uint32_t slotCount = 0;  ///< 可领槽总数（构造期 = capacity − reservedSlots）
	std::vector<SlotClaim> claims;  ///< 申报台账（按首次申报顺序追加，永不重排/删除）
};

}  // namespace RGL
