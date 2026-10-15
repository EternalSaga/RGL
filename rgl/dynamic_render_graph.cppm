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
 * frames-in-flight **逐出台账**已下沉 rlbase 叶子模块 `pending_evictions`（债-124；
 * `RenderTargetCache` 也要吃它，而 rlgfx 不能 import rlscene）。本模块的文档口径：
 * 逐出必须等 in-flight 用户的判据本体在 `pending_evictions.cppm`。
 */

/**
 * 脏驱动的图派生件（D5 的载体；执行器侧唯一写者 = `Renderer::recompileGraphIfNeeded`）。
 *
 * 生命周期：main 装配期把**初始 desc** move 进来（初始图已在 main 编译并校验过，
 * 本类不重复首编译）；运行期模块经 `declareBufferProductions` 追加 buffer 附件与
 * 生产者声明（幂等 + 试编译 fail-fast），Renderer 在安全点 `recompile()`。
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
	 * 重编译（安全点调用；dirty 为假直接返回 Error——调用方先查 isDirty）。
	 * 成功：清脏、计数 +1、日志逐名打印 buffer 附件的注册字节（机器可观测，判据 5 的
	 * 观测面）；失败：脏位**不清**（下帧安全点重试），返回编译 Error。
	 * @return 新编译图（调用方负责按名结转逐 slot 表并替换执行器持有的编译图）
	 */
	[[nodiscard]] std::expected<CompiledRenderGraph, Error> recompile();

   private:
	RgGraphDesc graphDesc;  ///< 图描述（本类真相源；追加只走 declareBufferProductions）
	bool dirty = false;     ///< 脏位（markDirty 置位；recompile 成功清零）
	uint32_t recompiles = 0;  ///< 重编译计数（出厂档判据 = 0）
};

}  // namespace RGL
