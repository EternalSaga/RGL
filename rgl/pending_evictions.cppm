module;
export module pending_evictions;

import rlstd;
import vkerror;

// frames-in-flight **逐出台账**（批次 B2-4③′ 债-124；**叶子模块**：纯逻辑、零设备、
// 零日志 → 属 rlbase，`just test` 毫秒级测，判据 = tests/test_dynamic_render_graph.cppm）。
//
// ## 它存在的唯一理由
//
// RT 因 extent 漂移退役时，GPU 侧资源**不能立刻毁**——in-flight 帧的命令流里还带着
// 它的描述符（`RenderTargetCache::ensure` 的旧 waitIdle 用整设备停顿换这件事，
// 代价是整条流水线卡死；D5 动态图把「逐出」变成常态后，这条不能每帧付）。
// 正确形态 = 发现帧 D 登记 due = D + framesInFlight，到期帧的**安全点**（围栏后）才销毁。
// "哪个帧号算到期"是纯算术，纯算术就该住 rlbase 并被单测锁死，而不是散在 Renderer 里。
//
// ## 两条不变式（每条都有单测）
//
//   1. 同名未到期再次 schedule → **Error**（「有 in-flight 用户逐出必须炸」——它抓的
//      是连续 resize 的真实形态：第二次的 due 会吞掉第一次的销毁记账 = 旧资源永久泄漏）；
//   2. popDue 只交 dueFrame ≤ frame 的项，且交完即销账（不重复交、不漏交）。
//
// frames-in-flight 数值由**调用方**传入（rlbase 不认识 FrameSession，也不该认识）。
namespace RGL {

/**
 * 逐出台账：名字 → 到期帧号。登记序保留（项数个位数；日志次序稳定可复现）。
 */
export class PendingEvictions {
   public:
	/**
	 * @param name     待逐出名（RT 附件名）
	 * @param dueFrame 到期帧号（= 发现帧 + frames-in-flight；到期帧的安全点才销毁）
	 * @return 空值 = 已登记；Error = 同名已在台账里（in-flight 用户未走清）
	 */
	[[nodiscard]] std::expected<void, Error> schedule(std::string_view name, uint64_t dueFrame) {
		if (pending(name))
			return std::unexpected(Error{ErrorCode::RenderGraphInvalid,
				std::string("pending evictions: '") + std::string(name)
					+ "' 已在逐出台账里（due 帧未到 = in-flight 用户还没走清）——"
					  "第二次逐出会吞掉第一次的销毁记账，旧资源永久泄漏"});
		entries.push_back(Entry{std::string(name), dueFrame});
		return {};
	}

	/** @return 名字是否已在台账（调用方据此避免重复 schedule） */
	bool pending(std::string_view name) const {
		return std::ranges::any_of(entries,
			[name](const Entry& e) { return e.name == name; });
	}

	/**
	 * 收割到期项（帧号单调；一次调用取走全部 dueFrame ≤ frame 的名字，登记序）。
	 * @param frame 当前帧号（调用点 = 安全点）
	 * @return 到期名列表；调用方负责销毁
	 */
	std::vector<std::string> popDue(uint64_t frame) {
		std::vector<std::string> due;
		for (auto it = entries.begin(); it != entries.end();) {
			if (it->dueFrame <= frame) {
				due.push_back(std::move(it->name));
				it = entries.erase(it);
			}
			else
				++it;
		}
		return due;
	}

	/** @return 台账项数（单测与日志用） */
	size_t size() const { return entries.size(); }

   private:
	/** 一条逐出登记 */
	struct Entry {
		std::string name;
		uint64_t dueFrame = 0;
	};
	std::vector<Entry> entries;
};

}  // namespace RGL
