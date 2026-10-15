export module offgraph_contract;

import rlstd;
import vkerror;
import render_graph;

namespace RGL {

/**
 * **图外工作申报面**（批次 B1 第一刀 → `docs/compute_调研.md` §7）。
 *
 * ## 这一层解决什么
 *
 * 渲染图只跟 image attachment 的布局链（`RgAttachmentKind` 全是 image），所以四个逐帧主 CB
 * compute（meshlet_cull / IZB / EVSSM / FSR）全在图外：它们**自己录、自己发 barrier**，
 * 执行器既看不见也不负责。后果是三条位置契约只活在注释里、buffer 侧同步零对账（债-75），
 * 而「缺 barrier VVL 会报」这个指望在本项目**实测不成立**（同文 §4 / N3）。
 *
 * 本层把**挂点**从注释变成**数据**，并做两件机器可查的事：
 * ① **位置契约**——工作项必须在它申报的相位被录制，挪位置就出声；
 * ② **覆盖对账**——门开着却整帧没录（静默旁路）、门关了却录了（逐位不变红线），都出声。
 *
 * 读写面与 buffer 侧 barrier 对账（债-75）是 **B1-b**，本层只立挂点这一半。
 *
 * ## 🔴 本层不做什么（plan-only 红线，`docs/compute_调研.md` §6 D1 第 2 条）
 *
 * **不发 barrier、不代做同步、不改 pass kind、不接管执行**。barrier 仍由模块自己发、
 * 仍由模块返回真实布局。理由：一旦图代发，模块就无从返回真相，A 档 `layoutDrift` 那类
 * 对账会**变成恒真**——那正是 14.4b B 档被否决的形态（**声明不能替代"发 barrier 的人返回真相"**）。
 *
 * ## 为什么住 rlbase 之外的 rlscene 却仍可离线测
 *
 * 判据是「需不需要一个 `VkDevice` 才能跑」，不是「import 没 import rlvk」（见
 * `tests/CMakeLists.txt` 头注释）。本层全是值类型 + 一次查表，**零设备调用** ⟹ 进
 * `just test` 的无设备子集。
 */

/**
 * 图外工作的挂点：帧首段（pass 循环之前），或某个 pass 之前。
 *
 * `beforePass` 的匹配口径**照抄被它替代的那段硬编码判据**（原先是
 * `pass.kind == RgPassKind::drawLight && pass.state == "light"` 这类字符串比较）：
 * 两个条件都必须命中。匹配到 0 个或多个 pass 都是申报错误，在**启动期**出声——
 * 编译图构造完就不再变，所以这类错没有"运行期才知道"的理由。
 *
 * 例外只有一档（债-128）：`mayBeAbsent = true` 的锚点允许命中 0。同一张申报表要服务
 * 多张编译图（forward 图没有 drawLight "light"），命中 0 的含义从「装配错」变成
 * 「该工作项在本图**不可调度**」（见 `OffGraphSchedule`）。🔴 它必须**逐条显式**声明，
 * 没有全局容忍档——全局容忍 0 命中会把上面那条启动期护栏整个拆掉（state 改名 = 装配错
 * 就再也报不出来，B1 第一刀立它的理由）。
 */
export struct RgOffGraphAnchor {
	/** 挂点种类：帧首段，或某个 pass 之前。 */
	enum class Kind : uint8_t {
		framePrologue,  ///< 帧首段：`recordFixedDataPath` 之后、pass 循环之前
		beforePass,     ///< 某个 pass 之前（按 kind + state 匹配编译图）
	};
	Kind kind = Kind::framePrologue;
	RgPassKind passKind = RgPassKind::drawLight;  ///< 仅 beforePass 有意义
	std::string passState;                        ///< 仅 beforePass 有意义（空 = 不按 state 匹配）
	/**
	 * 仅 beforePass 有意义：锚点允许在本编译图命中 0 个 pass（命中 0 = 该工作项在本图
	 * 不可调度，`enter` 打上来必 Error、帧末门开着必 Error）。
	 * 🔴 逐条显式声明，无全局档；帧首段锚点不查编译图、恒可解析，`build` **拒绝**
	 * `framePrologue + mayBeAbsent` 这种无意义组合（静默忽略会让"以为声明了"与
	 * "忘了声明"不可区分）。命中**多个**不受本字段豁免——歧义与缺席是两件事。
	 */
	bool mayBeAbsent = false;
};

/**
 * 一条图外工作申报。
 *
 * 🔴 **B1 第一刀只有挂点**。读写面（读/写哪些资源、期望真实布局）**刻意不在这里**：它必须
 * 逐条照抄各模块真实发出的 barrier 才有意义，而那份对账是 B1-b 的验收内容。现在就填一份
 * "看起来对"的名单比不填更糟——它会被后来人读成"这条已经接线"。
 */
export struct RgOffGraphWorkDecl {
	std::string name;         ///< 唯一名（日志归因用，如 "meshlet_cull"）
	RgOffGraphAnchor anchor;  ///< 挂点
};

/**
 * 帧内**相位**模型（位置契约的度量）。
 *
 * ```text
 * 相位 0            = beginFrame 之后、帧首段之前
 * 相位 1            = 帧首段（prologuePhase）
 * 相位 2 + 2i       = 第 i 个 pass 之前（beforePassPhase(i)）
 * 相位 2 + 2i + 1   = 第 i 个 pass 体内（inPassPhase(i)，beginRendering 之后）
 * 相位 2 + 2N       = pass 循环之后（frameEndPhase）
 * ```
 *
 * 为什么用相位而不是布尔：把 meshlet_cull 的调用点从帧首段挪到 pass 循环**之后**，
 * 布尔式的"在不在循环里"会重新变成 false（循环已经结束）⟹ 那种判据抓不到这条变异
 * （§7 判据 2 要求的正是它）。相位是单调的，挪到哪一相位都对不上。
 *
 * 为什么 pass 还要**再分两档**（之前 / 体内）：三个 sandwich 的契约是「录在 pass **外**的
 * 头部」——IZB 自己 beginMaskPass、FSR 发 barrier 与 compute，这些命令都不能出现在
 * dynamic rendering 实例内。只分"第几个 pass"一档的话，把挂点从 `beginRendering` 之前
 * 挪到之后就完全 invisible，而那是最典型的一种挂点漂移。
 */
export class OffGraphSchedule {
   public:
	/**
	 * 启动期建表：把申报表对着**编译图**解析成相位表（实现见 `offgraph_contract_impl.cppm`）。
	 *
	 * 失败即返回（调用方 fail-fast，不留到运行期）：申报重名、`beforePass` 锚点
	 * 命中 0 个 pass（图里没有那个 pass / state 改名了）、命中多个（锚点有歧义）、
	 * `framePrologue + mayBeAbsent` 无意义组合。
	 * 例外（债-128）：`anchor.mayBeAbsent` 的锚点命中 0 **不是**失败——该项标成不可调度
	 * （相位 = `kUnschedulablePhase`）并打一条启动期 warn（不静默）；命中多个仍失败。
	 * @param decls 申报的工作项列表（顺序 = `checkCoverage` 的 active 位序）
	 * @param passes 编译图的 pass 计划（`CompiledRenderGraph::passes`）
	 * @return 相位表；申报与图不一致时返回 RenderGraphInvalid
	 */
	static std::expected<OffGraphSchedule, Error>
	build(std::span<const RgOffGraphWorkDecl> decls, const std::vector<RgPassPlan>& passes);

	/** 帧首（drawFrame 开场）：清调用记录、相位归 0。 */
	void beginFrame();

	/**
	 * 执行器推进相位（帧首段 / 每个 pass 之前 / 帧末各调一次）。
	 * @param phase 目标相位（`prologuePhase` / `beforePassPhase(i)` / `frameEndPhase`）
	 */
	void advanceTo(uint32_t phase) { currentPhase = phase; }

	/**
	 * 图外工作项开录：查位置契约。
	 * @param name 申报名
	 * @return 空值 = 位置正确（并已记账）；未申报 / 位置不对 / 同帧重复录制 /
	 *         打在不可调度项上（债-128：没有锚点却录了 = 挂点错）= Error（带归因）
	 */
	std::expected<void, Error> enter(std::string_view name);

	/**
	 * 帧末覆盖对账：申报了却整帧没录、门关了却录了，都出声。
	 *
	 * `active` 由调用方在**帧末**求值（不是帧首——IZB / EVSSM 是首帧 `drawFrame` 内懒构造，
	 * 帧首求值会把它们误判成"不该发生"），且**必须是关于门与输入的事实**，
	 * 不能是"刚才 enter 过没有"——后者会让这条断言自证。
	 * 不可调度项（债-128）走**同一条**判据不另造：门开着 = 每帧点名"门开着却整帧没录"
	 * （门开着却没有锚点可录 = 静默旁路），门关 = 不响（forward 的正常旁路形态）。
	 * @param active 与申报表同序的启用位（门关 = false）
	 * @return 空值 = 覆盖一致；否则 Error（首条违规，消息含工作项名）
	 */
	std::expected<void, Error> checkCoverage(std::span<const bool> active) const;

	/**
	 * 该申报在**本编译图**里是否可调度（债-128）。
	 * @param name 申报名
	 * @return true = 锚点命中 1 个 pass；false = 可缺席锚点命中 0，或名字不在表
	 *         （查询、不是事务；与 `OffGraphWorkTable::isActive` 同形态）
	 */
	bool isSchedulable(std::string_view name) const;

	/** @return 帧首段相位 */
	static constexpr uint32_t prologuePhase() { return kProloguePhase; }
	/**
	 * @param passIndex 编译图里的 pass 下标
	 * @return 该 pass **之前**（pass 外）的相位
	 */
	static constexpr uint32_t beforePassPhase(uint32_t passIndex) {
		return kFirstPassPhase + 2u * passIndex;
	}
	/**
	 * @param passIndex 编译图里的 pass 下标
	 * @return 该 pass **体内**（beginRendering 之后）的相位
	 */
	static constexpr uint32_t inPassPhase(uint32_t passIndex) {
		return beforePassPhase(passIndex) + 1u;
	}
	/** @return pass 循环之后的相位（= 帧末） */
	uint32_t frameEndPhase() const { return kFirstPassPhase + 2u * passCount; }
	/** @return 申报的工作项数（= `checkCoverage` 的 active 长度要求） */
	uint32_t workCount() const { return static_cast<uint32_t>(entries.size()); }
	/** @return 当前相位（诊断与单测用） */
	uint32_t phase() const { return currentPhase; }

	/** 相位常量：帧首段。 */
	static constexpr uint32_t kProloguePhase = 1;
	/** 相位常量：第 0 个 pass 之前。 */
	static constexpr uint32_t kFirstPassPhase = 2;
	/**
	 * 相位哨兵（债-128）：可缺席锚点命中 0 = 本图**不可调度**。
	 * 任何真实相位（≤ 帧末 = 2+2N）都到不了它 ⟹ `enter` 打上来必红；判据请问
	 * `isSchedulable`，不要拿它做算术。
	 */
	static constexpr uint32_t kUnschedulablePhase = ~uint32_t{0};

   private:
	/** 一条申报解析后的运行时条目。 */
	struct Entry {
		std::string name;
		uint32_t phase = 0;   ///< 申报要求的相位（`kUnschedulablePhase` = 本图无锚点）
		bool called = false;  ///< 本帧是否录过
	};

	std::vector<Entry> entries;
	uint32_t passCount = 0;
	uint32_t currentPhase = 0;
};

/**
 * 图外工作的**在册表**（批次 B2-5）：规范序 + 在场位 + 按锚点批量撤 / 复。
 *
 * ## 为什么需要它（B1 的申报表为什么不够）
 *
 * B1 的 `offGraphWorkDecls()` 是一个 static 函数：申报集在编译期就定死了，`build` 只在启动期
 * 与重编译时吃它。B2-5 让 pass 能在安全点进出图，于是出现一个新的失效形态：
 *
 * > 一条申报的锚点是「drawLight `light` 之前」，而 `light` 这个 pass 被动态撤掉了。
 *
 * 此时 `build` 会报「命中 0 个 pass」——**报得对，但报错了地方**：图与申报表本来就该一起变，
 * 让安全点每帧红等于把「事务没做全」伪装成「装配错」。所以申报面必须也有撤回这一档，
 * 与 pass 级同相位、同事务地生效。
 *
 * ## 三条不显然的口径
 *
 *  - **规范序永不变**：`entries` 的顺序 = 构造入参顺序，撤回只清在场位、**不删条目**。
 *    帧末门位数组（`checkCoverage` 的 active）按规范序求值，若撤回把条目删了，
 *    剩余项的位序就整体前移，而门位数组是按名字写的 ⟹ 两者错位 = 对账指错工作项。
 *  - **幂等**：撤两次 / 复两次都是空操作（复合事务重放同一意图不该炸）；
 *    但**名字不在册 = Error**——拼错的名字静默成功 = 以为撤了其实没撤，正是本层要抓的形态。
 *  - **批量按锚点**（`withdrawAnchoredTo`）：调用方只知道「哪个 pass 要没了」，而申报写的是
 *    `(kind, state)`。把「锚点解析到哪个 pass」这件事放在本层做（锚点语义的持有者），
 *    而不是让 Renderer 自己比对 state 字符串——那会把锚点口径复制一份出去，
 *    两份口径漂移时判据不会红（AGENTS 红线一：访问路径必须消费那个来源）。
 *
 * 零设备调用 ⟹ 与 `OffGraphSchedule` 同进 `just test` 的无设备子集。
 */
export class OffGraphWorkTable {
  public:
	/**
	 * @param decls 全部申报（顺序 = 规范序 = 帧末门位数组的位序；此后不重排）
	 */
	explicit OffGraphWorkTable(std::vector<RgOffGraphWorkDecl> decls);

	/** @return 全部申报（含已撤回的；规范序） */
	[[nodiscard]] std::vector<RgOffGraphWorkDecl> all() const;

	/**
	 * @return 在场申报（规范序子集）—— `OffGraphSchedule::build` 的唯一合法输入
	 */
	[[nodiscard]] std::vector<RgOffGraphWorkDecl> activeDecls() const;

	/**
	 * @param name 申报名
	 * @return 该申报是否在场（名字不在册 = false；与 withdraw 的「未知名字 = Error」是两回事，
	 *         本函数是查询、不是事务）
	 */
	[[nodiscard]] bool isActive(std::string_view name) const;

	/**
	 * 撤回一条申报（幂等）。
	 * @param name 申报名
	 * @return 空值 = 已撤回或本就不在场；未知名字 = Error（点名 + 在册名单）
	 */
	[[nodiscard]] std::expected<void, Error> withdraw(std::string_view name);

	/**
	 * 恢复一条申报（幂等；与 `withdraw` 成对）。
	 * @param name 申报名
	 * @return 空值 = 已恢复或本就在场；未知名字 = Error
	 */
	[[nodiscard]] std::expected<void, Error> redeclare(std::string_view name);

	/**
	 * 撤掉所有「锚点解析到 passName」的在场申报（复合事务用；记下撤回原因供反向恢复）。
	 *
	 * 🔴 必须对着**撤 pass 之前**的编译图调用：pass 已经不在图里时锚点解析不出下标，
	 * 那时调用会返回 Error（而不是静默什么都不撤 —— 静默不撤 = 下一帧安全点报「命中 0 个
	 * pass」，把事务内部没做全伪装成装配错）。
	 * @param passes   当前（旧）编译图的 pass 计划
	 * @param passName 即将离场的 pass 名
	 * @return 被撤的名字（规范序；空 = 没有申报挂在这个 pass 上，highlight 就是这一档）
	 */
	[[nodiscard]] std::expected<std::vector<std::string>, Error>
	withdrawAnchoredTo(const std::vector<RgPassPlan>& passes, std::string_view passName);

	/**
	 * 恢复当初因 `passName` 撤掉的申报（复合事务用；与上面那条成对）。
	 * @param passName 重新进场的 pass 名
	 * @return 被恢复的名字（规范序；空 = 当初就没撤过）
	 */
	[[nodiscard]] std::expected<std::vector<std::string>, Error>
	redeclareAnchoredTo(std::string_view passName);

	/** @return 在册申报项数（= 帧末门位数组的长度要求） */
	[[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(entries.size()); }

  private:
	/** 一条在册申报 + 在场位 + 撤回原因（= 因哪个 pass 离场而被撤；空 = 非批量撤）。 */
	struct Entry {
		RgOffGraphWorkDecl decl;
		bool active = true;
		std::string withdrawnBy;
	};

	/**
	 * 锚点解析出的 pass 下标（与 `OffGraphSchedule::build` 同一套匹配口径）。
	 * @param decl   待解析的申报
	 * @param passes 编译图的 pass 计划
	 * @return pass 下标；帧首段锚点或解析不出唯一命中时 nullopt
	 */
	static std::optional<uint32_t>
	anchorPassIndex(const RgOffGraphWorkDecl& decl, const std::vector<RgPassPlan>& passes);

	/**
	 * 在册名单（错误消息归因用；成员函数——`Entry` 是私有嵌套类型，自由函数点不到它的字段）。
	 * @param items 待列出的条目（通常是全部在册项）
	 * @return "a,b,c" 形态
	 */
	static std::string knownNames(const std::vector<Entry>& items);

	std::vector<Entry> entries;  ///< 规范序（构造入参序；撤回只翻在场位，**不删项**）
};

}  // namespace RGL
