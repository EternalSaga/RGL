module;
#include <fmt/format.h>
module render_graph:impl;

import render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;
import vulkan_utils;  // createImageBarrier（物化用）

namespace RGL {

// compileRenderGraph / defaultRenderGraph 实现（接口只留类型与声明）。
// 算法：①名字解析与去重 ②逐 pass 预检（角色合法、引用解析、同 pass 读写冲突）
// ③布局链推导 = 每 attachment 一个"上次 usage"游标，跨 pass 生成 RgBarrierPlan
// ④storeOp 完整性（后续还被使用的写必须 store）⑤帧末 present 转换。

namespace {

struct UsageInfo {
	vk::ImageLayout layout;
	vk::PipelineStageFlags2 stage;
	vk::AccessFlags2 access;
};

// usage → {layout, stage, access} 推导表（接口注释速览的权威实现，单一真相源）
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

bool isWriteUsage(RgUsage u) {
	return u == RgUsage::colorWrite || u == RgUsage::depthWrite || u == RgUsage::transferWrite;
}
bool isReadUsage(RgUsage u) {
	return u == RgUsage::sampleRead || u == RgUsage::transferRead;
}

vk::ImageAspectFlags aspectOf(RgAttachmentKind kind) {
	return kind == RgAttachmentKind::depth ? vk::ImageAspectFlagBits::eDepth
					       : vk::ImageAspectFlagBits::eColor;
}

Error rgError(std::string message) {
	return Error{ErrorCode::RenderGraphInvalid, std::move(message)};
}

struct SlotState {
	std::optional<RgUsage> lastUse;  // 帧内上次 usage；nullopt = 本帧尚未被引用
	bool everWritten = false;        // 本帧是否已有写（读角色前置校验；跨帧历史复用留后续）
	uint32_t useCount = 0;
	uint32_t lastWritePass = kRgNoAttachment;  // 最后一条"写"所在 pass（store 完整性检查）
};

vk::AttachmentLoadOp toVkLoad(RgLoadOp op) {
	return op == RgLoadOp::clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad;
}
vk::AttachmentStoreOp toVkStore(RgStoreOp op) {
	return op == RgStoreOp::store ? vk::AttachmentStoreOp::eStore : vk::AttachmentStoreOp::eDontCare;
}

// 本 pass 引用了某 attachment 几次（角色重复检查）：outputs+inputs 合计
uint32_t countRole(const RgPassDesc& pass, const std::string& name) {
	uint32_t n = 0;
	for (const auto& att : pass.outputs)
		if (att.attachment == name) ++n;
	for (const auto& att : pass.inputs)
		if (att.attachment == name) ++n;
	return n;
}

}  // namespace

std::expected<CompiledRenderGraph, Error> compileRenderGraph(const RgGraphDesc& desc) {
	CompiledRenderGraph graph;

	// ① attachment 注册：名字 → slot；swapchain 类至多一个（present 语义单 surface）
	std::map<std::string, RgAttachmentSlot> nameToSlot;
	std::vector<SlotState> slots;
	uint32_t swapchainCount = 0;
	for (const auto& att : desc.attachments) {
		if (att.name.empty())
			return std::unexpected(rgError("render_graph: attachment with empty name"));
		if (auto [it, ok] = nameToSlot.emplace(att.name, static_cast<RgAttachmentSlot>(slots.size()));
			!ok)
			return std::unexpected(
				rgError(fmt::format("render_graph: duplicate attachment name '{}'", att.name)));
		if (att.kind == RgAttachmentKind::swapchain && ++swapchainCount > 1)
			return std::unexpected(rgError("render_graph: more than one swapchain attachment"));
		graph.attachments.push_back(RgCompiledAttachment{
			.name = att.name,
			.kind = att.kind,
			.format = att.format,
			.aspect = aspectOf(att.kind),
			// 帧首丢弃规则：swapchain/depth 每帧内容按 loadOp 重建（现 ④ 的
			// oldLayout=eUndefined 约定）；offscreen 持久图必须追踪真实布局
			.frameStartDiscard = att.kind != RgAttachmentKind::offscreen,
		});
		slots.emplace_back();
	}

	auto resolve = [&](const std::string& name) -> RgAttachmentSlot {
		auto it = nameToSlot.find(name);
		return it == nameToSlot.end() ? kRgNoAttachment : it->second;
	};

	// ②③④ 逐 pass：预检 → barrier 计划 → beginRendering 条目 → 游标推进
	for (uint32_t passIdx = 0; passIdx < desc.passes.size(); ++passIdx) {
		const auto& pass = desc.passes[passIdx];
		if (pass.name.empty())
			return std::unexpected(rgError(fmt::format(
				"render_graph: pass #{} has empty name", passIdx)));

		if (pass.kind == RgPassKind::drawMesh) {
			if (pass.state.empty() || pass.drawSource.empty())
				return std::unexpected(rgError(fmt::format(
					"render_graph: draw pass '{}' requires non-empty state and drawSource", pass.name)));
			const bool hasColor = std::ranges::any_of(
				pass.outputs, [](const auto& a) { return a.usage == RgUsage::colorWrite; });
			if (!hasColor)
				return std::unexpected(rgError(fmt::format(
					"render_graph: draw pass '{}' has no colorWrite output", pass.name)));
			uint32_t depthWrites = 0;
			for (const auto& a : pass.outputs)
				if (a.usage == RgUsage::depthWrite) ++depthWrites;
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
		} else {
			if (pass.outputs.size() != 1 || pass.inputs.size() != 1 ||
				pass.outputs[0].usage != RgUsage::transferWrite ||
				pass.inputs[0].usage != RgUsage::transferRead)
				return std::unexpected(rgError(fmt::format(
					"render_graph: blit pass '{}' must have exactly one transferWrite output and one "
					"transferRead input",
					pass.name)));
		}

		RgPassPlan plan;
		plan.name = pass.name;
		plan.kind = pass.kind;
		plan.state = pass.state;
		plan.drawSource = pass.drawSource;

		// 本 pass 引用集合（outputs 后 inputs 的发射序；同 attachment 多角色 = 冲突，
		// 同 pass 读写同 attachment = feedback loop，两者一并拒绝）
		std::vector<const RgPassAttachment*> refs;
		refs.reserve(pass.outputs.size() + pass.inputs.size());
		for (const auto& a : pass.outputs)
			refs.push_back(&a);
		for (const auto& a : pass.inputs)
			refs.push_back(&a);

		std::vector<std::pair<RgAttachmentSlot, RgUsage>> thisPassUses;  // ④ 后统一推进游标
		for (const auto* a : refs) {
			const auto slot = resolve(a->attachment);
			if (slot == kRgNoAttachment)
				return std::unexpected(rgError(fmt::format(
					"render_graph: pass '{}' references unknown attachment '{}'", pass.name,
					a->attachment)));
			if (countRole(pass, a->attachment) > 1)
				return std::unexpected(rgError(fmt::format(
					"render_graph: pass '{}' references attachment '{}' more than once (read/write "
					"feedback or duplicate role)",
					pass.name, a->attachment)));
			const auto& compiled = graph.attachments[slot];

			// usage 与 kind 匹配：depth 附件只能 depthWrite；非 depth 附件不能 depthWrite
			if ((a->usage == RgUsage::depthWrite) != (compiled.kind == RgAttachmentKind::depth))
				return std::unexpected(rgError(fmt::format(
					"render_graph: pass '{}' usage of '{}' inconsistent with its kind", pass.name,
					a->attachment)));

			const auto target = usageInfo(a->usage);
			auto& state = slots[slot];

			// 读前置：本帧尚无写入的 attachment 不可读（跨帧历史复用——TAA 类——
			// 骨架版不支持，留后续）；blit 两端不得是 depth 附件
			if (isReadUsage(a->usage) && !state.everWritten)
				return std::unexpected(rgError(fmt::format(
					"render_graph: pass '{}' reads '{}' before any write in the frame", pass.name,
					a->attachment)));
			if (pass.kind == RgPassKind::blit && compiled.kind == RgAttachmentKind::depth)
				return std::unexpected(rgError(fmt::format(
					"render_graph: blit pass '{}' cannot touch depth attachment '{}'", pass.name,
					a->attachment)));

			// barrier 计划：读→读无需同步；其余（帧首 / 写后读 / 读后写 / 写后写）
			// 生成一条转换（同布局的写→写跨 pass 实例也发——identity barrier 合法，
			// 为 dynamic rendering 跨实例 LOAD 提供执行依赖）
			if (!(state.lastUse && isReadUsage(*state.lastUse) && isReadUsage(a->usage))) {
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

			// beginRendering 条目（写角色）
			if (isWriteUsage(a->usage)) {
				RgAttachmentPlan ap{};
				ap.slot = slot;
				ap.layout = target.layout;
				ap.loadOp = toVkLoad(a->loadOp);
				ap.storeOp = toVkStore(a->storeOp);
				ap.clearValue = compiled.kind == RgAttachmentKind::depth
					? vk::ClearValue{vk::ClearDepthStencilValue{a->clear.depth, 0u}}
					: vk::ClearValue{std::array<float, 4>{a->clear.color}};
				if (a->usage == RgUsage::depthWrite)
					plan.depth = ap;
				else if (a->usage == RgUsage::colorWrite)
					plan.colors.push_back(ap);
				else
					plan.blitTarget = slot;  // transferWrite（blit pass）
			} else if (a->usage == RgUsage::transferRead && pass.kind == RgPassKind::blit) {
				plan.blitSource = slot;
			}
			thisPassUses.emplace_back(slot, a->usage);
		}
		graph.passes.push_back(std::move(plan));

		// 游标推进 + 记录"最后写所在 pass"（④ 的延后校验：该 attachment 之后还有
		// 任何使用——读或写——时，那条写的 storeOp 不得 DontCare）
		for (const auto& [slot, usage] : thisPassUses) {
			auto& state = slots[slot];
			if (state.lastWritePass != kRgNoAttachment && state.lastWritePass != passIdx) {
				const auto& prevPass = graph.passes[state.lastWritePass];
				const RgAttachmentPlan* prevPlan = nullptr;
				for (const auto& cp : prevPass.colors)
					if (cp.slot == slot) prevPlan = &cp;
				if (!prevPlan && prevPass.depth && prevPass.depth->slot == slot)
					prevPlan = &*prevPass.depth;
				if (prevPlan && prevPlan->storeOp == vk::AttachmentStoreOp::eDontCare)
					return std::unexpected(rgError(fmt::format(
						"render_graph: attachment '{}' written in pass '{}' with DontCare store but used "
						"later (contents would be lost)",
						graph.attachments[slot].name, prevPass.name)));
			}
			if (isWriteUsage(usage)) {
				state.lastWritePass = passIdx;
				state.everWritten = true;
			}
			state.lastUse = usage;
			++state.useCount;
		}
	}

	// ⑤ 帧末 present 转换：swapchain attachment 的最后 usage 必须是写
	if (swapchainCount == 1) {
		const auto swapSlot = static_cast<RgAttachmentSlot>(std::ranges::find(
		                              graph.attachments, RgAttachmentKind::swapchain,
		                              &RgCompiledAttachment::kind) -
		                          graph.attachments.begin());
		const auto& state = slots[swapSlot];
		if (!state.lastUse || !isWriteUsage(*state.lastUse))
			return std::unexpected(rgError(
				"render_graph: swapchain attachment is never written (nothing to present)"));
		const auto prev = usageInfo(*state.lastUse);
		graph.presentTransition = RgBarrierPlan{
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
		for (const auto& pass : graph.passes) {
			auto it = std::ranges::find(pass.barriers, swapSlot, &RgBarrierPlan::attachment);
			if (it != pass.barriers.end()) {
				graph.swapchainWaitStage = it->dstStage;
				break;
			}
		}
	}

	for (uint32_t i = 0; i < slots.size(); ++i)
		if (slots[i].useCount == 0)
			graph.warnings.push_back(fmt::format(
				"render_graph: attachment '{}' is never used by any pass", graph.attachments[i].name));

	return graph;
}

const RgGraphDesc& defaultRenderGraphDesc() {
	// 现 drawFrame 三段的图表达（批次 2 diff=0 验收基准）。与单 render pass 实例
	// 的等价性靠"中间 scope store 保内容"实现：depth 仅末段 DontCare（现 ⑤ 语义
	// 是整帧 DontCare，中途 store 是三段拆分的必要代价，像素结果不受影响）。
	static const RgGraphDesc desc = [] {
		auto writeOut = [](std::string name, RgUsage usage, RgLoadOp load, RgStoreOp store) {
			return RgPassAttachment{.attachment = std::move(name), .usage = usage, .loadOp = load,
				.storeOp = store};
		};
		return RgGraphDesc{
			.attachments =
				{
					RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
					RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
				},
			.passes =
				{
					RgPassDesc{.name = "opaque", .state = "opaque", .drawSource = "opaque",
						.outputs = {writeOut("swapchain", RgUsage::colorWrite, RgLoadOp::clear,
							     RgStoreOp::store),
							writeOut("depth", RgUsage::depthWrite, RgLoadOp::clear,
							     RgStoreOp::store)}},
					RgPassDesc{.name = "skybox", .state = "skybox", .drawSource = "skybox",
						.outputs = {writeOut("swapchain", RgUsage::colorWrite, RgLoadOp::load,
							     RgStoreOp::store),
							writeOut("depth", RgUsage::depthWrite, RgLoadOp::load,
							     RgStoreOp::store)}},
					RgPassDesc{.name = "transparent", .state = "transparent",
						.drawSource = "transparent",
						.outputs = {writeOut("swapchain", RgUsage::colorWrite, RgLoadOp::load,
							     RgStoreOp::store),
							writeOut("depth", RgUsage::depthWrite, RgLoadOp::load,
							     RgStoreOp::dontCare)}},
				},
		};
	}();
	return desc;
}

const CompiledRenderGraph& defaultRenderGraph() {
	static const CompiledRenderGraph graph = [] {
		auto compiled = compileRenderGraph(defaultRenderGraphDesc());
		if (!compiled)
			throw std::runtime_error(
				fmt::format("defaultRenderGraph: {}", compiled.error().describe()));
		return *compiled;
	}();
	return graph;
}

vk::ImageMemoryBarrier2 materializeBarrier(const RgBarrierPlan& plan, VkImage image,
	vk::ImageLayout oldLayout) {
	return RGL::createImageBarrier(image, oldLayout, plan.newLayout, plan.aspect,
		plan.srcStage, plan.dstStage, plan.srcAccess, plan.dstAccess);
}

std::vector<std::string> renderGraphSelfCheck() {
	std::vector<std::string> failures;
	auto expectOk = [&](const char* what, const RgGraphDesc& desc) {
		if (auto r = compileRenderGraph(desc); !r)
			failures.push_back(fmt::format("{}: unexpected failure: {}", what, r.error().describe()));
	};
	auto expectErr = [&](const char* what, const RgGraphDesc& desc) {
		if (auto r = compileRenderGraph(desc); r)
			failures.push_back(fmt::format("{}: expected failure, compiled OK", what));
	};
	auto colorOut = [](std::string name) {
		return RgPassAttachment{.attachment = std::move(name), .usage = RgUsage::colorWrite};
	};
	auto scAtt = [] { return RgAttachmentDesc{.name = "swapchain"}; };

	expectOk("default", defaultRenderGraphDesc());

	// blit 图：offscreen RT 渲三段 + blit 上屏（批次 4 的转正拓扑）
	expectOk("offscreen blit", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
			RgAttachmentDesc{.name = "scene_rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {
			RgPassDesc{.name = "scene", .state = "opaque", .drawSource = "opaque",
				.outputs = {colorOut("scene_rt")}},
			RgPassDesc{.name = "composite", .kind = RgPassKind::blit,
				.outputs = {RgPassAttachment{.attachment = "swapchain",
					.usage = RgUsage::transferWrite, .storeOp = RgStoreOp::store}},
				.inputs = {RgPassAttachment{.attachment = "scene_rt",
					.usage = RgUsage::transferRead}}},
		},
	});

	// 非法样本：引用不存在 / 读先于写 / 同 pass feedback / DontCare 后还有使用 /
	// draw pass 缺段名 / blit 拓扑错误 / swapchain 永不写
	expectErr("unknown attachment", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("nope")}}},
	});
	expectErr("read before write", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {RgPassDesc{.name = "p", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("swapchain")},
			.inputs = {RgPassAttachment{.attachment = "rt", .usage = RgUsage::sampleRead}}}},
	});
	expectErr("same-pass feedback", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("swapchain")},
			.inputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::sampleRead}}}},
	});
	expectErr("dontCare then use", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {
			RgPassDesc{.name = "a", .state = "opaque", .drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = "rt", .usage = RgUsage::colorWrite,
					.storeOp = RgStoreOp::dontCare}}},
			RgPassDesc{.name = "b", .state = "opaque", .drawSource = "opaque",
				.outputs = {colorOut("swapchain")},
				.inputs = {RgPassAttachment{.attachment = "rt", .usage = RgUsage::sampleRead}}},
		},
	});
	expectErr("draw pass without drawSource", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .state = "opaque",
			.outputs = {colorOut("swapchain")}}},
	});
	expectErr("blit wrong topology", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .kind = RgPassKind::blit,
			.outputs = {colorOut("swapchain")}}},
	});
	expectErr("swapchain never written", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {RgPassDesc{.name = "a", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("rt")}}},
	});

	// 结构断言：默认图 = 3 pass；首 pass 两条 discard 帧首转换；后续 pass 对
	// color/depth 各发一条 identity barrier（跨 render pass 实例 LOAD 的执行依赖，
	// old==new 合法）；present 转换来自 transparent 段的 colorWrite（CAO→PresentSrc）
	{
		const auto& g = defaultRenderGraph();
		if (g.passes.size() != 3) failures.push_back("default: expected 3 passes");
		if (g.passes.size() == 3) {
			const auto& first = g.passes[0];
			if (first.barriers.size() != 2 ||
				!first.barriers[0].oldLayoutDiscard || !first.barriers[1].oldLayoutDiscard)
				failures.push_back("default: first pass should carry 2 discard barriers");
			if (!first.colors.empty() &&
				first.colors[0].loadOp != vk::AttachmentLoadOp::eClear)
				failures.push_back("default: first color load should be Clear");
			const auto& mid = g.passes[1];
			if (mid.barriers.size() != 2 || mid.barriers[0].oldLayoutDiscard ||
				mid.barriers[1].oldLayoutDiscard)
				failures.push_back("default: mid pass should carry 2 tracked identity barriers");
			if (!mid.colors.empty() && mid.colors[0].loadOp != vk::AttachmentLoadOp::eLoad)
				failures.push_back("default: mid color load should be Load");
		}
		if (!g.presentTransition ||
			g.presentTransition->newLayout != vk::ImageLayout::ePresentSrcKHR)
			failures.push_back("default: missing present transition");
	}
	return failures;
}

}  // namespace RGL
