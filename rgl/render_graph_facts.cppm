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

/** 图里是否有 pass 消费透明绘制段（实现；判据为何是 drawSource 而不是 state → 接口注释）。 */
bool graphDrawsTransparentSegment(std::span<const RgPassPlan> passes) {
	return std::ranges::any_of(passes,
		[](const RgPassPlan& p) { return p.drawSource == kTransparentDrawSource; });
}

/** FSR 色输入优先级链（实现；链的语义与它的历史事故 → render_graph.cppm 接口注释）。
 *  🔴 四档序 = `assembleHdrAoChain` 的 tonemap 色输入链**逐档同序**，两处必须一致；
 *  单测以「四枚附件在/不在的 16 种组合」钉住两者同解（见 tests/test_render_graph 组 10）。 */
std::optional<RgFsrColorInput> graphFsrColorInput(
	std::span<const RgCompiledAttachment> attachments) {
	// 优先级链本体（收口前在 `Renderer::initFsr` 里是三次按名现查 + 两次比较）。
	// 逐位不变式：ao_applied 在 → 取它且 baseSpace=false；否则 taa_out 在 → 取它且
	// baseSpace=true（taa_out 自批 13.5 起已在基空间）；否则 ssr_applied → baseSpace=false
	// （它由 scene_hdr 合成而来，抖动还在）；否则 scene_hdr → baseSpace=false。
	if (const auto ao = graphAttachmentSlot(attachments, kAoAppliedName))
		return RgFsrColorInput{.slot = *ao, .baseSpace = false};
	if (const auto taa = graphAttachmentSlot(attachments, kTaaOutName))
		return RgFsrColorInput{.slot = *taa, .baseSpace = true};
	// 🔴 这一档是 2026-10-16 补的**漏档修复**：`initFsr` 的原始三档链没有 `ssr_applied`，
	// 而装配层的四档链有 ⟹ 图里有 `ssr_applied` 而链头落不到它时，tonemap 读 `ssr_applied`
	// 而 FSR 读 `scene_hdr` = **SSR 整条静默丢掉**（不报错、VVL 不抓、症状只是"SSR 没生效"）。
	//
	// 🔴 **债-129 S3-A 之后本档已不可达**（补漏的那次改动与 S3-A 同一天，S3-A 把它的触发
	//  前提删掉了）。论证链：本档要被选中 ⟺ 图里有 `ssr_applied` 且 `ao_applied` / `taa_out`
	//  都不在 ⟹ ① ssr 装饰成功了（要锚点 + 全 G-buffer 契约表）；② 要有人拿这个答案就得 FSR
	//  真接线 ⟹ `aaWantsFsr()` 为真（`main_impl` 装饰侧与装配侧取的是**同一个** `aaWantsFsr()`，
	//  不存在「FSR 接了但 ao_apply 没被调」的配置）⟹ `decorateAoApplyOrThrow` 必被调；③ 而
	//  ao_apply 剩下的两条跳过条件在这里都不成立：B 类「taa 在图里」与 ssr 自己的 B 类同命中
	//  taa（不可能一个跳一个插），C 类锚点与 ssr 共用同一个 `swapchainColorConsumerIndex`
	//  （ssr 都过了）⟹ ao_apply **照插**（原 A 类「无 ao 附件」已删）。于是只剩两个出口：
	//  图里有 `ao` 附件 → `ao_applied` 在链头，本档轮不到；图里没有 → compile 硬拒、**进程起
	//  不来**。（运行期 `declarePass` 只能**增**附件、摘不掉 `ao_applied`，也造不出本形态。）
	//
	// ⚠ **仍然保留这一档，别顺手删**：① 装配层 `assembleHdrAoChain` 的 tonemap 链是四档，两条
	//  链必须**结构同序**，删了就重新打开上面那个漏档洞；② 「不可达」是**装饰门控**的性质而不是
	//  链的性质，门控会变（S3-A 本身就是同一条链上门控变过一次的反例）；③ 16 组合表把本档当
	//  纯函数契约在测，链的正确性不依赖可达性。
	if (const auto ssr = graphAttachmentSlot(attachments, kSsrAppliedName))
		return RgFsrColorInput{.slot = *ssr, .baseSpace = false};
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
