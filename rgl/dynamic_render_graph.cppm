export module dynamic_render_graph;

import rlstd;
import vkerror;
import render_graph;

/**
 * dynamic_render_graph —— **脏驱动的渲染图派生件**（批次 B2-4③′，D5 裁决第一刀）。
 *
 * D5（`docs/compute_调研.md` §6 D5）把图从「启动期产物」降级为「脏驱动派生件」：
 * 资源事实（buffer 附件字节、RT 集合）变了 → 标脏 → 下个**安全点**（围栏后、录制前）
 * 重编译，重建延迟 ≤1 帧。本模块只做**纯逻辑**（零 VkDevice）：desc 持有、脏位、
 * 重编译计数、试编译 fail-fast、逐 slot 表按名结转、frames-in-flight 逐出台账。
 * 判据因此全部能在 `just test` 里跑（rlscene 分层允许 import render_graph）。
 *
 * B2-5 追加的 pass 级插拔（`declarePass` / `withdrawPass`）把「图是派生件」从 buffer 档
 * 推到 pass 档：申报面能改的不再只有附件与 usage，还有 **pass 序列本身**。申报与撤回必须
 * 成对且幂等，跨 pass 补丁要能还原成**原值**——往返逐位相同是这一档的杀手判据。
 *
 * 三条红线在这里的落点：
 *  - **plan-only**：重编译只重跑校验与 barrier 计划推导，barrier 发射权不变；
 *    buffer 档结构性不进 barrier（`render_graph_usage.cppm` 的 nullopt layout）。
 *  - **结转不是重置**：`carryOverPerSlot` 把执行器的逐 slot 跨帧表（布局追踪 /
 *    RT generation）按**附件名**搬到新 slot 空间——同名保旧值（重置 = 下帧首条
 *    barrier 拿 eUndefined 当 oldLayout，静默错布局），新名拿默认值。
 *  - **逐出必须等 in-flight 用户**：`PendingEvictions` 强制「同名未到期不得再次
 *    逐出」，到期帧由调用方（Renderer）按 fence 语义给出。
 */
namespace RGL {

export {
/**
 * 逐 slot 表的**按名结转**（重编译后 slot 空间可能追加，旧名 slot 不变由调用方核对）。
 *
 * 🔴 「结转」与「重置」的差别是判据级的：布局追踪表被重置成 eUndefined 后，下帧首条
 * barrier 会拿 Undefined 当 oldLayout 去转一个**有内容的**图（VVL 报点在下一次采样处，
 * 归因成本极高）。变异「结转改重置」必须让测试红（判据见 tests/test_dynamic_render_graph.cppm）。
 * @param oldNames    旧图的附件名序列（下标 = 旧 slot）
 * @param oldValues   旧表（与 oldNames 同长；短者按默认值补）
 * @param newNames    新图的附件名序列（下标 = 新 slot）
 * @param defaultValue 新名（或旧表缺项）的取值
 * @return 新表（与 newNames 同长；同名取旧值）
 */
template <typename T>
	[[nodiscard]] std::vector<T> carryOverPerSlot(const std::vector<std::string>& oldNames,
		const std::vector<T>& oldValues, const std::vector<std::string>& newNames,
		const T& defaultValue) {
	std::vector<T> result;
	result.reserve(newNames.size());
	for (const auto& name : newNames) {
		const auto it = std::ranges::find(oldNames, name);
		const size_t oldIndex = it == oldNames.end()
		                            ? ~size_t{0}
		                            : static_cast<size_t>(it - oldNames.begin());
		result.push_back(oldIndex < oldValues.size() ? oldValues[oldIndex] : defaultValue);
	}
	return result;
}
}  // export（doc-check 只跳 `template` 起始行，export 前缀走块式导出）

/**
 * 图声明面 ↔ 附件↔边 表的**双源对账**（批次 B2-4④′，与 `checkComputeHazard` 的双源
 * 对账同型；表本体在 `offgraph_sync`，本函数是消费它的判据核）。
 *
 * 两条规则（各有单测，`tests/test_attachment_sync.cppm`）：
 *  - **正向**：`kind=buffer` 附件在图里有 storageWrite 声明**且**有 storageRead 声明
 *    ⟹ 附件↔边 表必须有它的行。缺行 = Error（点名附件 + 生产者/消费者 pass + 表名）。
 *    这条抓的是「共享肉眼可见地成立、同步却没人记账」——buffer 档对图 plan-only
 *    （零 barrier 产物），图绿不代表同步存在。
 *  - **反向**：表行指向的附件**出现在 desc 里** ⟹ 两侧声明都必须存在。半申报（只有写
 *    或只有读）= Error——申报说「图里有一处读」而图看不见它，两个来源就各说各话。
 *    附件不在 desc 里 = 该条休眠（门关档 / 服务未装配），不报。
 *
 * 🔴 **不查边 mask 的方向**（同 hazard 表：只有方向、没有执行顺序的口径反过来同样成立——
 * 本判据只有「有没有」，没有「挡得对不对」）。方向谎报的红由编译 fail-fast 各自负责：
 * 消费声明排在生产之前 = 读前置拒；同 pass 读写 = countRole 拒。
 *
 * 运行点：`DynamicRenderGraph` 构造（失败抛 = 启动期）、`declareBufferConsumptions`
 * 的试编译之后（成对形态落账前）、`recompile()`（安全点兜底，半申报的脏图在这里红）。
 * `declareBufferProductions` **不**跑它——生产者单独在场是合法形态（IZB 的 head/nodes
 * 就是 producer-only + 无表行，反向规则对它休眠）。
 * @param desc 待对账的图描述
 * @return 空值 = 两向都闭合；否则 Error（首条违规）
 */
export std::expected<void, Error> reconcileAttachmentSyncEdges(const RgGraphDesc& desc);

/**
 * frames-in-flight **逐出台账**已下沉 rlbase 叶子模块 `pending_evictions`（债-124；
 * `RenderTargetCache` 也要吃它，而 rlgfx 不能 import rlscene）。本模块的文档口径：
 * 逐出必须等 in-flight 用户的判据本体在 `pending_evictions.cppm`。
 */

/**
 * pass 级动态插拔的申报单（`RgPassDeclaration`）与跨 pass 补丁（`RgStoreOpPatch`）
 * **住在 render_graph**——它们是 desc 层的数据，不是本类的私有概念：启动期装饰
 * （`planHighlightDecoration`）与运行期申报（`declarePass`）必须吃同一张单子。
 */

/**
 * 脏驱动的图派生件（D5 的载体；执行器侧唯一写者 = `Renderer::recompileGraphIfNeeded`）。
 *
 * 生命周期：main 装配期把**初始 desc** move 进来（初始图已在 main 编译并校验过，
 * 本类不重复首编译；构造期跑附件↔边 对账，不闭合 = **抛** = 启动期失败）；运行期模块经
 * `declareBufferProductions` / `declareBufferConsumptions` 追加 buffer 附件与
 * 生产者/消费者声明（幂等 + 试编译 fail-fast），Renderer 在安全点 `recompile()`。
 *
 * B2-5 起多一档**pass 级**申报：`declarePass` / `withdrawPass` 让一个真实 pass 在安全点
 * 被插进图中段 / 从中段摘掉（含跨 pass 补丁的生效与还原）。它与 buffer 申报共用同一套
 * 幂等 + 试编译 + 脏位机制，区别只在「改 pass 序列」与「补丁要记原值」。
 *
 * 🔴 附件**只准追加在末尾**（FrameCapturer/PickingService 构造期缓存 slot；
 * slot 稳定性由 Renderer 侧重编译路径机器核对，见 vkrenderer_impl）。
 */
export class DynamicRenderGraph {
   public:
	/** @param desc 初始图描述（move 进本类；此后 desc 是本类的真相源） */
	explicit DynamicRenderGraph(RgGraphDesc desc);

	/** @return 当前描述（只读；重编译的输入） */
	[[nodiscard]] const RgGraphDesc& desc() const { return graphDesc; }

	/** 标脏（资源事实变了；下个安全点重编译）。 */
	void markDirty() { dirty = true; }

	/** @return 是否脏 */
	[[nodiscard]] bool isDirty() const { return dirty; }

	/** @return 已重编译次数（出厂档判据 = 恒 0；IZB 开档 = 1，首建扩展一次） */
	[[nodiscard]] uint32_t recompileCount() const { return recompiles; }

	/**
	 * 声明「模块拥有的 buffer 附件出现在 pass X」：向 desc **末尾**追加 buffer 附件、
	 * 给目标 pass 追加 storageWrite 输出（生产者声明；buffer usage 结构性不进
	 * barrier/beginRendering，执行器行为零变化）。
	 *
	 * 幂等：附件名已在 desc 里 = 整条跳过（返回成功、不标脏）。
	 * 🔴 试编译 fail-fast：先对 desc 副本编译，失败**不落账**直接返回 Error——
	 * 注册忘字节 / 名字拼错 / 写上界之外的形态在变异现场就炸，而不是留下一个
	 * 编译不过的脏图每帧重试刷日志。
	 * @param passName         生产者 pass 名（如 "light"）
	 * @param attachmentNames  buffer 附件名（必须已 registerBufferAttachment 且 >0 字节）
	 */
	[[nodiscard]] std::expected<void, Error> declareBufferProductions(std::string_view passName,
		std::vector<std::string> attachmentNames);

	/**
	 * 声明「pass X 读模块拥有的 buffer 附件」（B2-4④′：图的第一批**真消费者**声明）：
	 * 给目标 pass 追加 storageRead 输入。与生产者声明的三个差别都是语义而非细节：
	 *  - 附件**必须已在图中**（生产者声明先行——读前置校验本就要求先写，这里把时序
	 *    提到 API 面上，错误消息直接指路而不是让试编译报一句间接的"读前置"）；
	 *  - 落账前跑 `reconcileAttachmentSyncEdges`：消费者声明是"成对形态完成"那一步，
	 *    完成后的 desc 必须过双源对账（缺边 = Error 在调用现场，而不是留给安全点）；
	 *  - 幂等口径同生产者侧（该 pass 已声明读同名附件 = 空操作、不标脏）。
	 * 试编译 fail-fast 同样在：同 pass 读写 feedback / 消费排在生产之前，都在调用现场炸。
	 * @param passName         消费者 pass 名（如首个 drawSource=opaque 的 drawMesh pass）
	 * @param attachmentNames  buffer 附件名（必须已由生产者声明进图）
	 */
	[[nodiscard]] std::expected<void, Error> declareBufferConsumptions(std::string_view passName,
		std::vector<std::string> attachmentNames);

	/**
	 * 重编译（安全点调用；dirty 为假直接返回 Error——调用方先查 isDirty）。
	 * 成功：清脏、计数 +1、日志逐名打印 buffer 附件的注册字节（机器可观测，判据 5 的
	 * 观测面）；失败：脏位**不清**（下帧安全点重试），返回编译 Error。
	 * @return 新编译图（调用方负责按名结转逐 slot 表并替换执行器持有的编译图）
	 */
	[[nodiscard]] std::expected<CompiledRenderGraph, Error> recompile();

	/**
	 * 首次申报「图里要有这个 pass」（B2-5 pass 级动态插拔的**入**向；出向 = `withdrawPass`）。
	 *
	 * 与 buffer 声明的两个差别都是语义而非细节：
	 *  - 它**改的是 pass 序列**（中段插入 ⇒ 后续 pass 下标全体 +1，相位表要按新序重推），
	 *    而 buffer 声明只往 desc 末尾追加附件、给既有 pass 加一条 usage；
	 *  - 它带**跨 pass 补丁**（`RgStoreOpPatch`）：插一个要 load 前驱产物的 pass，就得改前驱的
	 *    storeOp。补丁与插入在一张单子上 = 一次事务（要么都进、要么都不进）。
	 *
	 * 幂等：同名 pass 已在场 = 空操作（不标脏）。
	 * 🔴 已在册但已撤回 → **拒绝**并指路 `redeclarePass`。理由：重插必须吃**在册那份**申报
	 * （A→B→A 往返逐位相同才不是自证），若这里接受调用方新拼的 desc，拼错也照样「往返一致」。
	 *
	 * 试编译 fail-fast 同 buffer 声明：先对副本编译，失败不落账直接返回 Error。
	 * @param decl 申报单（`planHighlightDecoration` 的产物，或调用方按同形态自造）
	 */
	[[nodiscard]] std::expected<void, Error> declarePass(RgPassDeclaration decl);

	/**
	 * 重插一条**已在册但已撤回**的申报（B2-5；与 `withdrawPass` 成对）。
	 *
	 * 插的是在册那份原申报（pass / 插入点 / 补丁逐字段原样），不接受新 desc——
	 * 这是 A→B→A′ 逐位相同这条判据的实现前提（口径见 `declarePass`）。
	 * 幂等：已在场 = 空操作。不在册 = Error。
	 * @param passName 动态 pass 名
	 */
	[[nodiscard]] std::expected<void, Error> redeclarePass(std::string_view passName);

	/**
	 * 登记一个**启动期装饰已经插进 desc** 的 pass 为「可动态撤回」（B2-5）。
	 *
	 * 为什么需要它：highlight 是 main 在图编译**之前**装饰进 desc 的，本类从构造起就只看见
	 * 「图里本来就有它」。没有这条登记，撤回它就只能靠猜补丁原值（猜错不报错，画面也照样对，
	 * 是标准的静默半吊子）。有了它，原值走的是与启动期装饰**同一张申报单**
	 * （`RgHighlightPlan::decl` 随 `CompiledGraphResult` 带出来）。
	 *
	 * 不改 desc、不标脏；但**核对**申报与现实一致：pass 必须在场、插入点必须对得上、
	 * 每条补丁必须已经生效（目标当前值 = 补丁的 newStoreOp）。不一致 = Error（装饰与登记
	 * 之间有人改过图 = 两边对不上，宁可启动期炸）。
	 * @param decl 启动期装饰实际落图的那张申报单
	 */
	[[nodiscard]] std::expected<void, Error> adoptPass(RgPassDeclaration decl);

	/**
	 * 撤回一个动态 pass（B2-5 **出**向；与 `declarePass` 成对、同样幂等）。
	 *
	 * 三种情形：
	 *  - 在册且在场 → 摘掉 pass、把它的补丁**还原成申报单上的 `originalStoreOp`**、标脏、成功；
	 *  - 在册但已撤回 → 空操作返回成功（幂等：复合事务重放同一意图不该炸）；
	 *  - **不在册** → Error。名字在图里 = 那是启动期 pass 而没人登记过它可撤（`adoptPass`），
	 *    动态撤它 = 调用方逻辑错；名字根本不存在 = 拼错。两者都必须出声（静默成功 =
	 *    以为关了其实没关）。
	 *
	 * 附件**不跟着撤**（append-only 红线：FrameCapturer / PickingService 构造期缓存 slot）。
	 * 留下的孤儿附件编译期无人引用 = 只是多一个 RT，不改变任何 pass 的命令流。
	 * @param passName 动态 pass 名
	 */
	[[nodiscard]] std::expected<void, Error> withdrawPass(std::string_view passName);

	/**
	 * @return pass 名是否在当前 desc 里（复合事务据此判断"插"还是"撤"；不区分动态/启动期）
	 */
	[[nodiscard]] bool hasPass(std::string_view passName) const;

	/**
	 * @return pass 名是否**已在册**（曾 declarePass / adoptPass 过，含已撤回的）。
	 *
	 * 为什么单独要这条查询：开关事务的「进」向有两条来源（重插在册那份 / 用配方首插），
	 * 选哪条是**在册与否**的事实。靠"先试 redeclarePass，失败再退到 declarePass"也能跑，
	 * 但那要把 Error 当控制流读（错误分类靠消息字符串），是拿日志当接口的形态。
	 */
	[[nodiscard]] bool isDynamic(std::string_view passName) const;

	/**
	 * @return 在册的动态 pass 名（含已撤回的；观测面：RPC `/state` 与单测都读这个，
	 *         而不是去数 pass 总数——数总数会把「插了别的 pass」也读成同一个信号）
	 */
	[[nodiscard]] std::vector<std::string> dynamicPassNames() const;

	/**
	 * @return 最近一次成功重编译的耗时（ns）。0 = 还没重编译过。
	 *
	 * 为什么留这个读数：B2-5 的「逐帧 = 测试模态不是 SLA」要有**可观测的代价**才裁决得动
	 * （宪法：写不出机器判据的「必须」= 债）。日志行里有它，`just smoke` 的台账就能算分位；
	 * 本类不自己聚合分位（那是工具层的活）。
	 */
	[[nodiscard]] uint64_t lastRecompileNs() const { return lastRecompileDurationNs; }

   private:
	/**
	 * 一条动态 pass 的**往返记录**（B2-5）。
	 *
	 * 只有两项，因为补丁自己带着 `originalStoreOp`（撤回要还原的原值在申报单上，不在这里）。
	 * 这个"少一个字段"是刻意的：原值若另存一份，就会出现「申报单说 A、记录说 B」的两份真相，
	 * 而两者不一致时**没人报错**（AGENTS 红线一）。
	 */
	struct DynamicPassRecord {
		RgPassDeclaration decl;  ///< 申报单原样（重插时逐字段复用）
		bool inserted = false;   ///< 当前是否在场（撤回后置 false，记录保留）
	};

	/**
	 * 按名找在册记录。
	 * @param passName pass 名
	 * @return 记录指针，不在册返回 nullptr
	 */
	DynamicPassRecord* findRecord(std::string_view passName);

	/**
	 * 把一条在册记录落到 desc 副本上（插 pass + 打补丁），供 declarePass / redeclarePass 用。
	 * @param trial  待改的 desc 副本
	 * @param record 在册记录
	 */
	std::expected<void, Error> applyRecordTo(RgGraphDesc& trial,
		const DynamicPassRecord& record) const;

	/**
	 * 把一条在册记录从 desc 副本上摘掉（删 pass + 还原补丁），供 withdrawPass 用。
	 * @param trial  待改的 desc 副本
	 * @param record 在册记录
	 */
	std::expected<void, Error> revertRecordFrom(RgGraphDesc& trial,
		const DynamicPassRecord& record) const;

	/**
	 * 校验一张申报单与当前 desc 一致（pass 不在场 / 插入点存在 / 补丁目标当前值 =
	 * 各补丁的 `originalStoreOp`）。declarePass 与 adoptPass 共用——后者传的
	 * `expectPresent = true`，此时补丁要已经生效（当前值 = `newStoreOp`）。
	 * @param decl         待校验的申报单
	 * @param expectPresent true = 申报的 pass 必须已在图里（adopt 口径）
	 * @return 空值 = 一致；否则 Error（点名哪一项对不上）
	 */
	std::expected<void, Error> checkDeclaration(const RgPassDeclaration& decl,
		bool expectPresent) const;

	RgGraphDesc graphDesc;  ///< 图描述（本类真相源；追加只走 declare* 系列）
	bool dirty = false;     ///< 脏位（markDirty 置位；recompile 成功清零）
	uint32_t recompiles = 0;  ///< 重编译计数（出厂档判据 = 0）
	std::vector<DynamicPassRecord> dynamicPasses;  ///< 动态 pass 在册表（顺序 = 申报序）
	uint64_t lastRecompileDurationNs = 0;          ///< 最近一次重编译耗时（观测面）
};

}  // namespace RGL
