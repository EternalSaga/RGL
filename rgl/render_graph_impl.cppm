module;
#include <fmt/format.h>
module render_graph:impl;

import render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;
import vulkan_utils;  // createImageBarrier（物化用）

namespace RGL {

// compileRenderGraph 实现（接口只留类型与声明；2026-09-11 Phase 8 批次 ①拆债：
// 内建图描述 + renderGraphSelfCheck 迁 render_graph_builtin.cppm，compileRenderGraph
// 按步骤拆 registerAttachments/precheckPass/planPassUses/advanceCursors 四 helper，
// 各 <120 行——函数 >120 与嵌套 >5 阈值双消）。
// 算法：①名字解析与去重 ②逐 pass 预检（角色合法、引用解析、同 pass 读写冲突）
// ③布局链推导 = 每 attachment 一个"上次 usage"游标，跨 pass 生成 RgBarrierPlan
// ④storeOp 完整性（后续还被使用的写必须 store）⑤帧末 present 转换。

namespace {

/** usage 推导三元组：目标布局 + 管线 stage + access mask。 */
struct UsageInfo {
	vk::ImageLayout layout;
	vk::PipelineStageFlags2 stage;
	vk::AccessFlags2 access;
};

/**
 * usage → {layout, stage, access} 推导表（接口注释速览的权威实现，单一真相源）：
 * colorWrite→CAO/写/ColorAttachmentOutput；depthWrite→DSAO/读+写/EarlyFragmentTests；
 * sampleRead→SRO/ShaderRead/AllGraphics；transferRead→TsrcO/TransferRead/Transfer；
 * transferWrite→TdstO/TransferWrite/Transfer。
 * @param u 附件角色
 * @return 推导三元组（枚举穷尽，末条 return 不可达）
 */
UsageInfo usageInfo(RgUsage u) {
	using Stage = vk::PipelineStageFlagBits2;
	using Acc = vk::AccessFlagBits2;
	switch (u) {
		case RgUsage::colorWrite:
			return {vk::ImageLayout::eColorAttachmentOptimal, Stage::eColorAttachmentOutput,
				Acc::eColorAttachmentWrite};
		case RgUsage::depthWrite:
			return {vk::ImageLayout::eDepthStencilAttachmentOptimal, Stage::eEarlyFragmentTests,
				Acc::eDepthStencilAttachmentRead | Acc::eDepthStencilAttachmentWrite};
		case RgUsage::sampleRead:
			return {vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eAllGraphics, Acc::eShaderRead};
		case RgUsage::transferRead:
			return {vk::ImageLayout::eTransferSrcOptimal, Stage::eTransfer, Acc::eTransferRead};
		case RgUsage::transferWrite:
			return {vk::ImageLayout::eTransferDstOptimal, Stage::eTransfer, Acc::eTransferWrite};
	}
	return {vk::ImageLayout::eUndefined, Stage::eTopOfPipe, Acc::eNone};  // 不可达（枚举穷尽）
}

/** 写角色判定（colorWrite/depthWrite/transferWrite）。 */
bool isWriteUsage(RgUsage u) {
	return u == RgUsage::colorWrite || u == RgUsage::depthWrite || u == RgUsage::transferWrite;
}
/** 读角色判定（sampleRead/transferRead）。 */
bool isReadUsage(RgUsage u) {
	return u == RgUsage::sampleRead || u == RgUsage::transferRead;
}

/** 附件 kind → aspect mask（depth / depthOffscreen → eDepth，其余 → eColor）。 */
vk::ImageAspectFlags aspectOf(RgAttachmentKind kind) {
	return kind == RgAttachmentKind::depth || kind == RgAttachmentKind::depthOffscreen
		       ? vk::ImageAspectFlagBits::eDepth
		       : vk::ImageAspectFlagBits::eColor;
}
/** 是否"图属 RT"（执行器按名 ensure 的两类：颜色 offscreen + 深度 depthOffscreen）。 */
bool isRtBackedKind(RgAttachmentKind kind) {
	return kind == RgAttachmentKind::offscreen || kind == RgAttachmentKind::depthOffscreen;
}
/** 是否深度类附件（主深度图 / 图属深度 RT）——clearValue 取 depth 分量、aspect 走深度面。 */
bool isDepthKind(RgAttachmentKind kind) {
	return kind == RgAttachmentKind::depth || kind == RgAttachmentKind::depthOffscreen;
}

/** 统一错误构造：ErrorCode::RenderGraphInvalid + message。 */
Error rgError(std::string message) {
	return Error{ErrorCode::RenderGraphInvalid, std::move(message)};
}

/** 单个 slot 的帧内游标状态（compile 跨 pass 推进，驱动 barrier 推导与校验）。 */
struct SlotState {
	std::optional<RgUsage> lastUse;  // 帧内上次 usage；nullopt = 本帧尚未被引用
	bool everWritten = false;        // 本帧是否已有写（读角色前置校验；跨帧历史复用留后续）
	uint32_t useCount = 0;
	uint32_t lastWritePass = kRgNoAttachment;  // 最后一条"写"所在 pass（store 完整性检查）
};

/** RgLoadOp → vk::AttachmentLoadOp（clear→eClear，load→eLoad）。 */
vk::AttachmentLoadOp toVkLoad(RgLoadOp op) {
	return op == RgLoadOp::clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
}
/** RgStoreOp → vk::AttachmentStoreOp（store→eStore，dontCare→eDontCare）。 */
vk::AttachmentStoreOp toVkStore(RgStoreOp op) {
	return op == RgStoreOp::store ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;
}

/** 本 pass 引用了某 attachment 几次（角色重复检查）：outputs+inputs 合计。 */
uint32_t countRole(const RgPassDesc& pass, const std::string& name) {
	uint32_t n = 0;
	for (const auto& att : pass.outputs)
		if (att.attachment == name) ++n;
	for (const auto& att : pass.inputs)
		if (att.attachment == name) ++n;
	return n;
}

/** 编译共享状态：attachment 注册表 + 游标（compileRenderGraph 栈上构造，helper 间传递）。 */
struct CompileCtx {
	CompiledRenderGraph graph;
	std::map<std::string, RgAttachmentSlot> nameToSlot;
	std::vector<SlotState> slots;
	uint32_t swapchainCount = 0;

	/** attachment 名 → slot（未注册 = kRgNoAttachment，调用方报错定位）。 */
	RgAttachmentSlot resolve(const std::string& name) const {
		auto it = nameToSlot.find(name);
		return it == nameToSlot.end() ? kRgNoAttachment : it->second;
	}
};

/**
 * ① 附件格式按 kind 校验：**两表分立**——offscreen 查颜色词表、depthOffscreen 查深度词表
 * （不合并的原因见 render_graph.cppm::isKnownDepthRgFormat 注释）；swapchain/depth 的格式
 * 由执行器固定，只认 "inherit"。@return 失败带定位 Error
 */
std::expected<void, Error> validateAttachmentFormat(const RgAttachmentDesc& att) {
	if (att.kind == RgAttachmentKind::offscreen) {
		if (!isKnownRgFormat(att.format))
			return std::unexpected(rgError(fmt::format(
				"render graph: attachment '{}' has unknown format '{}'", att.name, att.format)));
		return {};
	}
	if (att.kind == RgAttachmentKind::depthOffscreen) {
		if (!isKnownDepthRgFormat(att.format))
			return std::unexpected(rgError(fmt::format(
				"render graph: depth attachment '{}' needs a depth format "
				"(d32_sfloat / d16_unorm), got '{}'",
				att.name, att.format)));
		return {};
	}
	if (att.format != "inherit")
		return std::unexpected(rgError(fmt::format(
			"render graph: {} attachment '{}' must use format 'inherit' (got '{}')",
			att.kind == RgAttachmentKind::swapchain ? "swapchain" : "depth", att.name,
			att.format)));
	return {};
}

/**
 * ① 独立 extent 按 kind 校验：只对图属 RT（offscreen / depthOffscreen）开放——swapchain
 * 尺寸由 surface 定、主深度图随 swapchain 重建（Resources 持有），给它们填尺寸 = 声明一个
 * 执行器不会遵守的意图。@return 失败带定位 Error
 */
std::expected<void, Error> validateAttachmentExtent(const RgAttachmentDesc& att) {
	if (isRtBackedKind(att.kind) || (att.extent[0] == 0u && att.extent[1] == 0u))
		return {};
	return std::unexpected(rgError(fmt::format(
		"render graph: {} attachment '{}' must leave extent {{0,0}} (follows swapchain)",
		att.kind == RgAttachmentKind::swapchain ? "swapchain" : "depth", att.name)));
}

/** ① 单 attachment 校验与落图：空名 / 重名 / swapchain 计数 / 格式（validateAttachmentFormat）/ 尺寸（validateAttachmentExtent）→ 名字→slot + 图记录 + 游标。@return 失败带定位 Error */
std::expected<void, Error> registerAttachment(const RgAttachmentDesc& att, CompileCtx& ctx) {
	if (att.name.empty())
		return std::unexpected(rgError("render_graph: attachment with empty name"));
	if (auto [it, ok] = ctx.nameToSlot.emplace(att.name,
		    static_cast<RgAttachmentSlot>(ctx.slots.size()));
		!ok)
		return std::unexpected(
			rgError(fmt::format("render_graph: duplicate attachment name '{}'", att.name)));
	if (att.kind == RgAttachmentKind::swapchain && ++ctx.swapchainCount > 1)
		return std::unexpected(rgError("render_graph: more than one swapchain attachment"));
	if (auto r = validateAttachmentFormat(att); !r)
		return std::unexpected(std::move(r).error());
	if (auto r = validateAttachmentExtent(att); !r)
		return std::unexpected(std::move(r).error());
	ctx.graph.attachments.push_back(RgCompiledAttachment{
		.name = att.name,
		.kind = att.kind,
		.format = att.format,
		.extent = att.extent,  // 独立 RT 尺寸透传（执行器装配消费；非图属 RT 恒 {0,0}）
		.aspect = aspectOf(att.kind),
		// 帧首丢弃规则：swapchain/depth/depthOffscreen 每帧内容按 loadOp 重建（现 ④ 的
		// oldLayout=eUndefined 约定；阴影图每帧整幅 clear 亦归此档）；
		// offscreen 持久图必须追踪真实布局
		.frameStartDiscard = att.kind != RgAttachmentKind::offscreen,
	});
	ctx.slots.emplace_back();
	return {};
}

/** ① attachment 注册总入口：逐个交 registerAttachment（校验 + 落图），任一失败即中断（保首个报错顺序）。 */
std::expected<void, Error> registerAttachments(const RgGraphDesc& desc, CompileCtx& ctx) {
	for (const auto& att : desc.attachments)
		if (auto r = registerAttachment(att, ctx); !r)
			return std::unexpected(std::move(r).error());
	return {};
}

/** drawMesh pass 预检：state/drawSource 必填、至少一个写附件（color 或 depth）、depthWrite ≤1、读只 sampleRead、写只渲染类。 */
std::expected<void, Error> precheckDrawPass(const RgPassDesc& pass) {
	if (pass.state.empty() || pass.drawSource.empty())
		return std::unexpected(rgError(fmt::format(
			"render_graph: draw pass '{}' requires non-empty state and drawSource", pass.name)));
	const bool hasColor = std::ranges::any_of(
		pass.outputs, [](const auto& a) { return a.usage == RgUsage::colorWrite; });
	uint32_t depthWrites = 0;
	for (const auto& a : pass.outputs)
		if (a.usage == RgUsage::depthWrite) ++depthWrites;
	// 原规则是"必须有 colorWrite"，把 **depth-only pass 整个挡在门外**（阴影深度 pass
	// 只写深度、不写色，批 11.2 硬阻塞 B1，见 docs/phase11_plan.md §10.1）。放宽为
	// "至少一个写附件"，并保留对偶校验：一个写都没有 = 空 pass，没有可渲染对象，
	// 让它在这里报错而不是到 GPU 上静默跑一趟 begin/endRendering。
	if (!hasColor && depthWrites == 0)
		return std::unexpected(rgError(fmt::format(
			"render_graph: draw pass '{}' has no write output (need colorWrite or depthWrite)",
			pass.name)));
	if (depthWrites > 1)
		return std::unexpected(rgError(fmt::format(
			"render_graph: draw pass '{}' has {} depthWrite outputs (max 1)", pass.name,
			depthWrites)));
	for (const auto& a : pass.inputs)
		if (a.usage != RgUsage::sampleRead)
			return std::unexpected(rgError(fmt::format(
				"render_graph: draw pass '{}' input '{}' must be sampleRead", pass.name,
				a.attachment)));
	for (const auto& a : pass.outputs)
		if (!isWriteUsage(a.usage))
			return std::unexpected(rgError(fmt::format(
				"render_graph: draw pass '{}' output '{}' must be a write usage", pass.name,
				a.attachment)));
	if (std::ranges::any_of(pass.outputs,
		    [](const auto& a) { return a.usage == RgUsage::transferWrite; }))
		return std::unexpected(rgError(fmt::format(
			"render_graph: draw pass '{}' cannot declare transferWrite outputs (blit pass only)",
			pass.name)));
	return {};
}

/** drawLight pass 预检（Phase 8 ①泛化：state ∈ kFullscreenPassStates）：恰一 colorWrite、无 depth、读只 sampleRead。 */
std::expected<void, Error> precheckFullscreenPass(const RgPassDesc& pass) {
	if (pass.state.empty())
		return std::unexpected(rgError(fmt::format(
			"render_graph: light pass '{}' requires non-empty state", pass.name)));
	uint32_t colorWrites = 0, depthWrites = 0;
	for (const auto& a : pass.outputs) {
		if (a.usage == RgUsage::colorWrite)
			++colorWrites;
		else if (a.usage == RgUsage::depthWrite)
			++depthWrites;
		else
			return std::unexpected(rgError(fmt::format(
				"render_graph: light pass '{}' output '{}' must be colorWrite", pass.name,
				a.attachment)));
	}
	if (colorWrites != 1)
		return std::unexpected(rgError(fmt::format(
			"render_graph: light pass '{}' must have exactly one colorWrite output (got {})",
			pass.name, colorWrites)));
	if (depthWrites != 0)
		return std::unexpected(rgError(fmt::format(
			"render_graph: light pass '{}' must have no depth output", pass.name)));
	for (const auto& a : pass.inputs)
		if (a.usage != RgUsage::sampleRead)
			return std::unexpected(rgError(fmt::format(
				"render_graph: light pass '{}' input '{}' must be sampleRead", pass.name,
				a.attachment)));
	return {};
}

/** ② 逐 pass 预检分派（kind 三形态；blit = 恰一 transferWrite + 一 transferRead）。 */
std::expected<void, Error> precheckPass(const RgPassDesc& pass, uint32_t passIdx) {
	if (pass.name.empty())
		return std::unexpected(rgError(fmt::format(
			"render_graph: pass #{} has empty name", passIdx)));
	if (pass.kind == RgPassKind::drawMesh)
		return precheckDrawPass(pass);
	if (pass.kind == RgPassKind::drawLight)
		return precheckFullscreenPass(pass);
	if (pass.outputs.size() != 1 || pass.inputs.size() != 1 ||
		pass.outputs[0].usage != RgUsage::transferWrite ||
		pass.inputs[0].usage != RgUsage::transferRead)
		return std::unexpected(rgError(fmt::format(
			"render_graph: blit pass '{}' must have exactly one transferWrite output and one "
			"transferRead input",
			pass.name)));
	return {};
}

/** ③ 单引用解析与合法性校验：未注册 / 同 pass 多角色（feedback 一并拒）/ usage-kind 匹配（depth↔depthWrite 双向）/ 读前置（本帧未写不可读）/ blit 两端不得 depth。检查顺序即原循环体相对顺序（保首个报错定位）。@return 解析出的 slot，失败带 Error */
std::expected<RgAttachmentSlot, Error> validatePassRef(const RgPassDesc& pass,
	const RgPassAttachment& a, CompileCtx& ctx) {
	const auto slot = ctx.resolve(a.attachment);
	if (slot == kRgNoAttachment)
		return std::unexpected(rgError(fmt::format(
			"render_graph: pass '{}' references unknown attachment '{}'", pass.name,
			a.attachment)));
	if (countRole(pass, a.attachment) > 1)
		return std::unexpected(rgError(fmt::format(
			"render_graph: pass '{}' references attachment '{}' more than once (read/write "
			"feedback or duplicate role)",
			pass.name, a.attachment)));
	const auto& compiled = ctx.graph.attachments[slot];
	// usage 与 kind 匹配：**写**角色上 depth 附件（主深度图或图属深度 RT）只能 depthWrite、
	// 非深度附件不能 depthWrite；**读**角色（sampleRead）不受此约束——depth 作被采样源是
	// 合法形态（Phase 11 批 11.4：ao pass 采深度；批 11.2：阴影图亦由光照 pass 采）。
	// 非 depth 附件读 sampleRead 仍由 precheckFullscreenPass / precheckBlitPass 各自限定。
	if (a.usage == RgUsage::depthWrite && !isDepthKind(compiled.kind))
		return std::unexpected(rgError(fmt::format(
			"render_graph: pass '{}' depthWrites non-depth attachment '{}'", pass.name,
			a.attachment)));
	// 读前置：本帧尚无写入的 attachment 不可读（跨帧历史复用——TAA 类——
	// 骨架版不支持，留后续）
	if (isReadUsage(a.usage) && !ctx.slots[slot].everWritten)
		return std::unexpected(rgError(fmt::format(
			"render_graph: pass '{}' reads '{}' before any write in the frame", pass.name,
			a.attachment)));
	// blit 两端不得是深度类附件（transfer 与深度面 layout/aspect 语义不兼容）
	if (pass.kind == RgPassKind::blit && isDepthKind(compiled.kind))
		return std::unexpected(rgError(fmt::format(
			"render_graph: blit pass '{}' cannot touch depth attachment '{}'", pass.name,
			a.attachment)));
	return slot;
}

/** ③ barrier 计划生成：读→读无需同步；其余（帧首 / 写后读 / 读后写 / 写后写）生成一条转换（同布局的写→写跨 pass 实例也发——identity barrier 合法，为 dynamic rendering 跨实例 LOAD 提供执行依赖）。 */
void planBarrier(const RgPassAttachment& a, RgAttachmentSlot slot,
	const RgCompiledAttachment& compiled, const UsageInfo& target, const SlotState& state,
	RgPassPlan& plan) {
	if (!(state.lastUse && isReadUsage(*state.lastUse) && isReadUsage(a.usage))) {
		RgBarrierPlan barrier{};
		barrier.attachment = slot;
		barrier.aspect = compiled.aspect;
		barrier.newLayout = target.layout;
		barrier.dstStage = target.stage;
		barrier.dstAccess = target.access;
		if (state.lastUse) {
			const auto prev = usageInfo(*state.lastUse);
			barrier.oldLayoutDiscard = false;
			barrier.srcStage = prev.stage;
			barrier.srcAccess = prev.access;
		} else {
			barrier.oldLayoutDiscard = compiled.frameStartDiscard;
			barrier.srcStage = vk::PipelineStageFlagBits2::eTopOfPipe;
			barrier.srcAccess = vk::AccessFlagBits2::eNone;
		}
		plan.barriers.push_back(barrier);
	}
}

/** ③ beginRendering 写条目生成：写角色按 depth / color / blitTarget 落 plan；blit 的 transferRead 记 blitSource。 */
void planBeginRenderingEntry(const RgPassDesc& pass, const RgPassAttachment& a,
	RgAttachmentSlot slot, const RgCompiledAttachment& compiled, const UsageInfo& target,
	RgPassPlan& plan) {
	if (isWriteUsage(a.usage)) {
		RgAttachmentPlan ap{};
		ap.slot = slot;
		ap.layout = target.layout;
		ap.loadOp = toVkLoad(a.loadOp);
		ap.storeOp = toVkStore(a.storeOp);
		ap.clearValue = isDepthKind(compiled.kind)
			? vk::ClearValue{vk::ClearDepthStencilValue{a.clear.depth, 0u}}
			: vk::ClearValue{std::array<float, 4>{a.clear.color}};
		if (a.usage == RgUsage::depthWrite)
			plan.depth = ap;
		else if (a.usage == RgUsage::colorWrite)
			plan.colors.push_back(ap);
		else
			plan.blitTarget = slot;  // transferWrite（blit pass）
	} else if (a.usage == RgUsage::transferRead && pass.kind == RgPassKind::blit) {
		plan.blitSource = slot;
	}
}

/** ③④ 单 pass 全部引用：逐条 validatePassRef（解析/校验）→ planBarrier（barrier 计划）→ planBeginRenderingEntry（beginRendering 条目）；本帧使用对回传（游标延后统一推进）。 */
std::expected<void, Error> planPassUses(const RgPassDesc& pass, CompileCtx& ctx,
	RgPassPlan& plan, std::vector<std::pair<RgAttachmentSlot, RgUsage>>& thisPassUses) {
	// 本 pass 引用集合（outputs 后 inputs 的发射序；同 attachment 多角色 = 冲突，
	// 同 pass 读写同 attachment = feedback loop，两者一并拒绝）
	std::vector<const RgPassAttachment*> refs;
	refs.reserve(pass.outputs.size() + pass.inputs.size());
	for (const auto& a : pass.outputs)
		refs.push_back(&a);
	for (const auto& a : pass.inputs)
		refs.push_back(&a);

	for (const auto* a : refs) {
		auto slot = validatePassRef(pass, *a, ctx);
		if (!slot)
			return std::unexpected(std::move(slot).error());
		const auto& compiled = ctx.graph.attachments[*slot];
		const auto target = usageInfo(a->usage);
		planBarrier(*a, *slot, compiled, target, ctx.slots[*slot], plan);
		planBeginRenderingEntry(pass, *a, *slot, compiled, target, plan);
		thisPassUses.emplace_back(*slot, a->usage);
	}
	return {};
}

/** ④ 在 pass 的 beginRendering 条目里找引用该 slot 的 plan（colors 优先、depth 次之）。同 pass 内每 slot 至多一条（countRole 已拒重复引用），故 find 首命中即唯一命中，与原"末命中"手写循环等价。@return 命中条目指针，无则 nullptr */
const RgAttachmentPlan* findPlanEntry(const RgPassPlan& pass, RgAttachmentSlot slot) {
	const auto it = std::ranges::find(pass.colors, slot, &RgAttachmentPlan::slot);
	if (it != pass.colors.end())
		return &*it;
	if (pass.depth && pass.depth->slot == slot)
		return &*pass.depth;
	return nullptr;
}

/** ④ store 完整性校验：该 attachment 上次写的 pass（异于当前 pass）若 storeOp=DontCare 但后续仍被使用 → 报错（内容会丢）。 */
std::expected<void, Error> checkPriorWriteStore(const CompiledRenderGraph& graph,
	const SlotState& state, uint32_t passIdx, RgAttachmentSlot slot) {
	if (state.lastWritePass == kRgNoAttachment || state.lastWritePass == passIdx)
		return {};
	const auto& prevPass = graph.passes[state.lastWritePass];
	const auto* prevPlan = findPlanEntry(prevPass, slot);
	if (prevPlan && prevPlan->storeOp == vk::AttachmentStoreOp::eDontCare)
		return std::unexpected(rgError(fmt::format(
			"render_graph: attachment '{}' written in pass '{}' with DontCare store but used "
			"later (contents would be lost)",
			graph.attachments[slot].name, prevPass.name)));
	return {};
}

/** 游标推进 + store 完整性延后校验（先 checkPriorWriteStore 后推进游标，保原检查顺序：报错先于写更新）。 */
std::expected<void, Error> advanceCursors(const CompiledRenderGraph& graph,
	std::vector<SlotState>& slots, uint32_t passIdx,
	const std::vector<std::pair<RgAttachmentSlot, RgUsage>>& thisPassUses) {
	for (const auto& [slot, usage] : thisPassUses) {
		if (auto r = checkPriorWriteStore(graph, slots[slot], passIdx, slot); !r)
			return std::unexpected(std::move(r).error());
		auto& state = slots[slot];
		if (isWriteUsage(usage)) {
			state.lastWritePass = passIdx;
			state.everWritten = true;
		}
		state.lastUse = usage;
		++state.useCount;
	}
	return {};
}

/** ⑤ 帧末 present 转换 + swapchainWaitStage 派生（swapchain 最后 usage 必须是写）。 */
std::expected<void, Error> finishPresent(CompileCtx& ctx) {
	if (ctx.swapchainCount != 1)
		return {};  // 无 swapchain 的纯离屏图：presentTransition 留 nullopt
	const auto swapSlot = static_cast<RgAttachmentSlot>(std::ranges::find(
	                          ctx.graph.attachments, RgAttachmentKind::swapchain,
	                          &RgCompiledAttachment::kind) -
	                      ctx.graph.attachments.begin());
	const auto& state = ctx.slots[swapSlot];
	if (!state.lastUse || !isWriteUsage(*state.lastUse))
		return std::unexpected(rgError(
			"render_graph: swapchain attachment is never written (nothing to present)"));
	const auto prev = usageInfo(*state.lastUse);
	ctx.graph.presentTransition = RgBarrierPlan{
		.attachment = swapSlot,
		.aspect = vk::ImageAspectFlagBits::eColor,
		.oldLayoutDiscard = false,
		.newLayout = vk::ImageLayout::ePresentSrcKHR,
		.srcStage = prev.stage,
		.dstStage = vk::PipelineStageFlagBits2::eBottomOfPipe,
		.srcAccess = prev.access,
		.dstAccess = vk::AccessFlagBits2::eNone,
	};
	// submit wait stage：命令流中首个触碰 swapchain 的 barrier 的 dstStage
	//（= swapchain 首次实际使用阶段；直渲三段 = ColorAttachmentOutput、
	// blit 上屏图 = Transfer——旧手工 offscreen 分支的 waitStage 选段等价）
	for (const auto& pass : ctx.graph.passes) {
		auto it = std::ranges::find(pass.barriers, swapSlot, &RgBarrierPlan::attachment);
		if (it != pass.barriers.end()) {
			ctx.graph.swapchainWaitStage = it->dstStage;
			break;
		}
	}
	return {};
}

}  // namespace

/**
 * 格式词表（小写下划线 → vk::Format，接口注释是语义权威）：表驱动，扩展 = 加一行。
 * "inherit" 不在表内——语义 = 随 swapchain 格式，由 resolveRgFormat 单独短路。
 * @return 14 项词表（引用，静态存储）
 */
const std::array<std::pair<std::string_view, vk::Format>, 14>& rgFormatTable() {
	static const std::array<std::pair<std::string_view, vk::Format>, 14> table = {
		std::pair{"rgba8_srgb", vk::Format::eR8G8B8A8Srgb},
		std::pair{"rgba8_unorm", vk::Format::eR8G8B8A8Unorm},
		std::pair{"b8g8r8a8_srgb", vk::Format::eB8G8R8A8Srgb},
		std::pair{"rgb8_srgb", vk::Format::eR8G8B8Srgb},
		std::pair{"rg16_snorm", vk::Format::eR16G16Snorm},
		std::pair{"rg16_unorm", vk::Format::eR16G16Unorm},
		std::pair{"rgba16_sfloat", vk::Format::eR16G16B16A16Sfloat},
		std::pair{"rgb16_sfloat", vk::Format::eR16G16B16Sfloat},
		std::pair{"r32g32b32_sfloat", vk::Format::eR32G32B32Sfloat},
		std::pair{"rgba32_sfloat", vk::Format::eR32G32B32A32Sfloat},
		std::pair{"r32_sfloat", vk::Format::eR32Sfloat},
		std::pair{"rg32_sfloat", vk::Format::eR32G32Sfloat},
		// Phase 10 批 10.5：object-id attachment（picking pass 写 objectIndex+1）
		std::pair{"r32_uint", vk::Format::eR32Uint},
		// Phase 11 批 11.4：SSAO 遮蔽量（ao pass 写、tonemap 采；单通道 8bit 足够——
		// 强度曲线已在 shader 端 pow(·,2) 压过，8bit 量化误差 <0.4%）
		std::pair{"r8_unorm", vk::Format::eR8Unorm},
	};
	return table;
}

/** isKnownRgFormat 定义（接口注释见 render_graph.cppm）："inherit" 短路 + 词表 any_of。 */
bool isKnownRgFormat(const std::string& name) {
	if (name == "inherit")
		return true;
	return std::ranges::any_of(rgFormatTable(),
		[&](const auto& entry) { return entry.first == name; });
}

/**
 * 深度附件格式词表（kind=depthOffscreen 专用；与 rgFormatTable 颜色词表**分立**的原因见
 * render_graph.cppm::isKnownDepthRgFormat 注释）。与颜色词表同住在实现单元：
 * `vk::Format::eXxx` 枚举常量在模块接口分区不可见（rlvk GMF 头可达但名字不可见）。
 */
const std::array<std::pair<std::string_view, vk::Format>, 2>& rgDepthFormatTable() {
	static const std::array<std::pair<std::string_view, vk::Format>, 2> table = {
		// 主用档 = 与主深度图同格式（D32_SFLOAT 是 Vulkan 保证存在的深度格式；
		// **没有 D32_UNORM 这个格式**——写错时 clang 的"did you mean eD16Unorm"极易误导，
		// 2026-09-29 实测踩过）。精度对单级 1024² cascade 足够。
		std::pair{"d32_sfloat", vk::Format::eD32Sfloat},
		std::pair{"d16_unorm", vk::Format::eD16Unorm},  // 省带宽档（多级 cascade 的省内存选择）
	};
	return table;
}

/** isKnownDepthRgFormat 定义（接口注释见 render_graph.cppm）：深度词表 any_of（"inherit" 不算）。 */
bool isKnownDepthRgFormat(const std::string& name) {
	return std::ranges::any_of(rgDepthFormatTable(),
		[&](const auto& entry) { return entry.first == name; });
}

/** resolveRgFormat 定义（接口注释见 render_graph.cppm）："inherit"→fallback，具名先查颜色
 *  词表再查深度词表（kDepthRgFormats），漏网 eUndefined（调用方保证已 isKnown*）。 */
vk::Format resolveRgFormat(const std::string& name, vk::Format inheritFallback) {
	if (name == "inherit")
		return inheritFallback;
	for (const auto& [text, format] : rgFormatTable())
		if (text == name)
			return format;
	for (const auto& [text, format] : rgDepthFormatTable())
		if (text == name)
			return format;
	return vk::Format::eUndefined;  // 不可达（调用方保证已 isKnown*Format）
}

/** compileRenderGraph 定义（接口注释见 render_graph.cppm）：五步 = ①registerAttachments ②precheckPass ③④planPassUses（barrier + beginRendering 条目）+ advanceCursors（游标/store 完整性）⑤finishPresent。 */
std::expected<CompiledRenderGraph, Error> compileRenderGraph(const RgGraphDesc& desc) {
	CompileCtx ctx;
	if (auto r = registerAttachments(desc, ctx); !r)
		return std::unexpected(std::move(r).error());

	for (uint32_t passIdx = 0; passIdx < desc.passes.size(); ++passIdx) {
		const auto& pass = desc.passes[passIdx];
		if (auto r = precheckPass(pass, passIdx); !r)
			return std::unexpected(std::move(r).error());

		RgPassPlan plan;
		plan.name = pass.name;
		plan.kind = pass.kind;
		plan.state = pass.state;
		plan.drawSource = pass.drawSource;
		plan.viewport = pass.viewport;  // Phase 11 批 11.2：图集分块子矩形（默认全零 = 跟随）

		std::vector<std::pair<RgAttachmentSlot, RgUsage>> thisPassUses;
		if (auto r = planPassUses(pass, ctx, plan, thisPassUses); !r)
			return std::unexpected(std::move(r).error());
		ctx.graph.passes.push_back(std::move(plan));

		if (auto r = advanceCursors(ctx.graph, ctx.slots, passIdx, thisPassUses); !r)
			return std::unexpected(std::move(r).error());
	}

	if (auto r = finishPresent(ctx); !r)
		return std::unexpected(std::move(r).error());

	for (uint32_t i = 0; i < ctx.slots.size(); ++i)
		if (ctx.slots[i].useCount == 0)
			ctx.graph.warnings.push_back(fmt::format(
				"render_graph: attachment '{}' is never used by any pass",
				ctx.graph.attachments[i].name));

	return std::move(ctx.graph);
}

/** materializeBarrier 定义（接口注释见 render_graph.cppm）：经 createImageBarrier 物化（queue family 均 IGNORED）。 */
vk::ImageMemoryBarrier2 materializeBarrier(const RgBarrierPlan& plan, VkImage image,
	vk::ImageLayout oldLayout) {
	return RGL::createImageBarrier(image, oldLayout, plan.newLayout, plan.aspect,
		plan.srcStage, plan.dstStage, plan.srcAccess, plan.dstAccess);
}

}  // namespace RGL
