module render_graph:facts;

import render_graph;  // clang 不给 partition 实现单元隐式 import 主接口——必须显式
import rlstd;
import pass_vocabulary;  // kDeferredLightState

// 图事实查询面（债-129 S1a 自 render_graph_impl 拆出，解该文件 500 代码行红线；先例 =
// :formats / :decorate / :usage / :rtslot 四次同款拆分）。
//
// 为什么单独一个分区而不是留在 :impl：:impl 是**编译算法**（意图 → 执行序列的翻译，
// 改它要回答"布局链怎么推"），本分区是**编译产物的查询面**（"图长什么样"，改它要回答
// "谁在问、粒度对不对"）。两者的变更原因不同，混在一个文件里会让"加一条谓词"看起来像
// 动编译算法。
//
// 🔴 为什么必须是**实现**分区（不是 `export module`）：声明全在主接口 `render_graph.cppm`
// 里，本文件只给定义 ⟹ 公开面一个名字都不多。写成接口分区会造出第二份声明面。
//
// 🔴 每条谓词都必须有生产侧调用者（无问者 = `OffGraphSchedule::isSchedulable` 第二）。
// 判据不是纪律句 = source_checks 的 `graph_fact_predicate_checks`（按主接口里 `graph<大写>(`
// 的命名约定扫，含"被活谓词调用才算活"的传递闭包）。接口注释与裁决句 → render_graph.cppm。

namespace RGL {

/** graphHasDeferredLight 定义（接口注释见 render_graph.cppm：图事实层判据，收口原
 *  IZB / EVSSM 两处逐字相同的 any_of）。债-129 S1a 起 = `graphHasPassState` 的薄封装，
 *  本模块因此只剩**一份** pass 扫描（先例不能留成孤本，否则新谓词与它各扫一遍）。 */
bool graphHasDeferredLight(std::span<const RgPassPlan> passes) {
	return graphHasPassState(passes, RgPassKind::drawLight, kDeferredLightState);
}

/** 按名定位编译图附件（实现；接口注释 → render_graph.cppm）。 */
std::optional<RgAttachmentSlot> graphAttachmentSlot(
	std::span<const RgCompiledAttachment> attachments, std::string_view name) {
	const auto it = std::ranges::find_if(attachments,
		[name](const RgCompiledAttachment& a) { return a.name == name; });
	if (it == attachments.end())
		return std::nullopt;
	return static_cast<RgAttachmentSlot>(it - attachments.begin());
}

/** 按 kind 定位编译图附件（实现；与按名查不同轴 → `kDepthName`）。 */
std::optional<RgAttachmentSlot> graphAttachmentSlotByKind(
	std::span<const RgCompiledAttachment> attachments, RgAttachmentKind kind) {
	const auto it = std::ranges::find_if(attachments,
		[kind](const RgCompiledAttachment& a) { return a.kind == kind; });
	if (it == attachments.end())
		return std::nullopt;
	return static_cast<RgAttachmentSlot>(it - attachments.begin());
}

/** 图里是否有 (kind, state) 这一档 pass（实现；`graphHasDeferredLight` 的泛化）。 */
bool graphHasPassState(std::span<const RgPassPlan> passes, RgPassKind kind, std::string_view state) {
	return std::ranges::any_of(passes,
		[&](const RgPassPlan& p) { return p.kind == kind && p.state == state; });
}

/** FSR 色输入优先级链（实现；链的语义与它的历史事故 → render_graph.cppm 接口注释）。 */
std::optional<RgFsrColorInput> graphFsrColorInput(
	std::span<const RgCompiledAttachment> attachments) {
	// 优先级链本体（收口前在 `Renderer::initFsr` 里是三次按名现查 + 两次比较）。
	// 逐位不变式：ao_applied 在 → 取它且 baseSpace=false；否则 taa_out 在 → 取它且
	// baseSpace=true（taa_out 自批 13.5 起已在基空间）；否则 scene_hdr → baseSpace=false。
	if (const auto ao = graphAttachmentSlot(attachments, kAoAppliedName))
		return RgFsrColorInput{.slot = *ao, .baseSpace = false};
	if (const auto taa = graphAttachmentSlot(attachments, kTaaOutName))
		return RgFsrColorInput{.slot = *taa, .baseSpace = true};
	if (const auto hdr = graphAttachmentSlot(attachments, kSceneHdrName))
		return RgFsrColorInput{.slot = *hdr, .baseSpace = false};
	return std::nullopt;
}

/** 解析 FSR 三个输入附件（实现；`complete()` 才是两处判据共享的本体）。 */
RgFsrInputs graphFsrInputs(std::span<const RgCompiledAttachment> attachments) {
	return RgFsrInputs{
		.color = graphFsrColorInput(attachments),
		.velocity = graphAttachmentSlot(attachments, kVelocityName),
		// 深度按**名**查（`kDepthName`），与 IZB 按 kind 查不同轴 → 接口注释。
		.depth = graphAttachmentSlot(attachments, kDepthName),
	};
}

}  // namespace RGL
