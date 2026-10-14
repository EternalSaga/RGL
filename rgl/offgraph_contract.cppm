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
	 * 命中 0 个 pass（图里没有那个 pass / state 改名了）、命中多个（锚点有歧义）。
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
	 * @return 空值 = 位置正确（并已记账）；未申报 / 位置不对 / 同帧重复录制 = Error（带归因）
	 */
	std::expected<void, Error> enter(std::string_view name);

	/**
	 * 帧末覆盖对账：申报了却整帧没录、门关了却录了，都出声。
	 *
	 * `active` 由调用方在**帧末**求值（不是帧首——IZB / EVSSM 是首帧 `drawFrame` 内懒构造，
	 * 帧首求值会把它们误判成"不该发生"），且**必须是关于门与输入的事实**，
	 * 不能是"刚才 enter 过没有"——后者会让这条断言自证。
	 * @param active 与申报表同序的启用位（门关 = false）
	 * @return 空值 = 覆盖一致；否则 Error（首条违规，消息含工作项名）
	 */
	std::expected<void, Error> checkCoverage(std::span<const bool> active) const;

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

   private:
	/** 一条申报解析后的运行时条目。 */
	struct Entry {
		std::string name;
		uint32_t phase = 0;   ///< 申报要求的相位
		bool called = false;  ///< 本帧是否录过
	};

	std::vector<Entry> entries;
	uint32_t passCount = 0;
	uint32_t currentPhase = 0;
};

}  // namespace RGL
