module;
#include <vulkan/vulkan.h>  // VK_* 句柄类型；rlvk 不含
#include <fmt/format.h>     // fsrInputsReady 的降级告警格式化  // VK_* 句柄类型；rlvk 不含
module vk_renderer:offgraph;

/**
 * vk_renderer:offgraph —— **图外 compute 编舞**（批 14.3 自 :passrec 拆出）
 *
 * 职责边界（这就是它不留在 :passrec 的理由，不是"文件太长挪点过去"）：
 * - `:passrec` = **渲染图内**的 pass 录制：执行器盲消费编译图（barrier / load-store /
 *   布局全部来自图，手写布局转换清零）。
 * - 本分区 = **渲染图外**的编舞：IZB 与 EVSSM 两个阴影图在图外自己录 compute、自己发
 *   barrier、自己管布局回正。它们的命令不出现在编译图里，图内执行器**看不见也不负责**
 *   它们的布局正确性（这条不对称正是批 14.4b 勘察出的根因）。
 *
 * 为什么现在拆：批 14.5 的 FSR-upscaler 是**第三个**图外 compute，接线代码天然长在本
 * 分区。留在 :passrec 会让它一边涨过文件长度红线、一边把两类布局责任混进一个文件——
 * 而这两类布局的**对账方式不同**（图内查编译图，图外只能查执行器现场）。
 *
 * 搬移性质：**纯搬移，非重写**。五个函数体逐行原样（git diff 可逐字核），只换了所在
 * 分区的 import 面。
 */

import vk_renderer;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_swapchain;     // izbPreFrame 按渲染 extent 重建 mask（批 14.3）
import vk_resources;
import diagnostics;
import profiler;         // zone（IZB / EVSSM 帧首段观测）
import frame_draw_list;  // segmentSpan（mask 重放段名 → draw 序列）
import scene_context;    // shadowDrawSource（段名单一来源，批 11.3-1d）
import render_graph;     // RgAttachmentKind（重放后布局回正）
import izb_shadow;
import evssm_shadow;
import fsr_upscaler;
import pass_vocabulary;  // kTonemapState（批 14.5 FSR 认 tonemap pass）

namespace RGL {

// 11.3-1d IZB 帧首段（声明注释见 vkrenderer.cppm）。deferred 图判据 = 存在
// drawLight "light" pass；重建前 waitIdle（旧 buffer 可能被上一帧命令引用，RT 缓存
// 同款规则）。门关 = 本函数不被进入（izbShadow 恒空 + 单布尔判断）。
void Renderer::izbPreFrame() {
	auto z = zone("drawFrame.izbPre");  // 批 9.5 观测：IZB 帧首段（dump 收割 + 门控重建）
	if (izbShadow)
		izbShadow->drainReadback();
	++izbFrameNo;
	if (!diagnostics().izbShadow)
		return;
	const bool deferred = graphHasDeferredLight(renderGraph.passes);
	const auto depthIt = std::ranges::find_if(renderGraph.attachments,
		[](const auto& a) { return a.kind == RgAttachmentKind::depth; });
	// 非 deferred 在此静默早退是**合法运行期形态**（图判定不该在录制路径打日志）；
	// 「izb_shadow=true 配了却必然无效」由 gate_lint 启动期 error 喊（R-02，2026-10-02）。
	if (!deferred || depthIt == renderGraph.attachments.end())
		return;
	depthAttSlot = static_cast<int32_t>(depthIt - renderGraph.attachments.begin());
	const auto extent = swapchain->getRenderExtent();  // 批 14.3：IZB mask 与颜色附件同网格
	if (!izbShadow || izbShadow->getExtent() != extent) {
		vkDevice.getDevice().waitIdle();
		izbShadow = std::make_unique<RGL::IzbShadow>(
			vkDevice, *resources, extent, diagnostics().izbGrid);
	}
}

// 11.3-1d IZB sandwich（声明注释见 vkrenderer.cppm；门关 = izbShadow 恒空早退）。
void Renderer::izbLightPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
	const FrameDrawList& list) {
	if (!izbShadow || pass.kind != RgPassKind::drawLight || pass.state != "light")
		return;
	// 批 14.4b A 档护栏：图外 sandwich 的「进出同值」从**约定**变**断言**。
	// 为什么必须有：追踪表 `attachmentLayouts` 的唯一写者就是执行器自己，图外模块发的
	// barrier 它一概不知 ⟹ 漂移后执行器下一跳会拿**错的 oldLayout** 去转布局，而 VVL
	// **不报**（漂移后的布局本身合法，只是与表不符），症状隔一整帧才现（债-72）。
	// 14.5 的 FSR 是第二个这样的 sandwich，先在这里把模式立起来。
	const auto depthSlotIdx = static_cast<size_t>(depthAttSlot);
	const auto trackedLayout = attachmentLayouts[depthSlotIdx];
	const auto leftLayout = izbShadow->recordFrame(cmd,
		IzbFrameInput{list.sceneData.skyInvViewProj, list.sceneData.shadowViewProj[0]},
		trackedLayout,
		// N 帧起每 30 帧转一次（ppm 覆盖写 + 直方图日志行）——一次性判等会被 RPC
		// 截图时序甩开（imgqa izb 判据要求 mask 与截图同相机状态，机位是 RPC 后设的）
		diagnostics().izbDumpFrame != 0 && izbFrameNo >= diagnostics().izbDumpFrame
			&& (izbFrameNo - diagnostics().izbDumpFrame) % 30 == 0);
	if (leftLayout != trackedLayout) {
		if (auto* logger = RLLogger::getInstance())
			logger->error("izb: 深度附件布局漂移 —— 进入 = {}，离开 = {}，追踪表 = {}。"
				"渲染图不知道这次转换，下一跳会拿错的 oldLayout 转布局（VVL 不报，"
				"症状隔一整帧）；recordFrame 的返回值必须由实际发出的 barrier 推进",
				static_cast<uint32_t>(trackedLayout), static_cast<uint32_t>(leftLayout),
				static_cast<uint32_t>(trackedLayout));
	}
	replayIzbMaskSegment(cmd, list);
}

// 批 11.6d-2 EVSSM 帧首段（声明注释见 vkrenderer.cppm；门关 = 一次布尔判断早退）。
// 两条 warn-once 走文件级 helper（assembleBackEnd 复杂度红线先例：分支不入函数体）。
namespace {
/** warn-once 日志（批 11.6d-2，EvssmShadow 两条依赖告警共用；flag = 调用方 static）。
 *  @param flag 已告警标志（首次调用后置位）
 *  @param msg 告警文本 */
void evssmWarnOnce(bool& flag, const char* msg) {
	if (flag)
		return;
	flag = true;
	if (auto* logger = RLLogger::getInstance())
		logger->warn("{}", msg);
}
}  // namespace

void Renderer::evssmPreFrame() {
	auto z = zone("drawFrame.evssmPre");  // 批 9.5 观测：EVSSM 帧首段（门 = shadow_evssm）
	if (evssmShadow || !diagnostics().shadowEvssm || !diagnostics().shadowPass)
		return;
	// 非 deferred 组合由 gate_lint 启动期 error（R-03，2026-10-02 从这里的 warn-once
	// 迁走——不走到这一帧就永远不喊的病灶收口）；运行期早退保留（消费点在 deferred
	// light pass，判据不变）。
	if (!graphHasDeferredLight(renderGraph.passes))
		return;
	// 阴影图槽 = RT 缓存按名查（启动期 ensureShadowMapSlot 无条件建，deferred 判据
	// 在其后）；矩图 sampled 槽 = Resources 预发号（binding 15 已 bake 同号）。
	const auto shadowSlot = rtCache->heapSlot(std::string{kShadowMapName}.c_str());
	const auto momentsSlot = resources->ensureEvssmSampleSlot();
	if (!shadowSlot || !momentsSlot) {
		static bool warnedNoSlots = false;
		evssmWarnOnce(warnedNoSlots,
			"EvssmShadow 依赖缺失（shadow map slot / moments slot），EVSSM 关闭");
		return;
	}
	vkDevice.getDevice().waitIdle();  // 矩图链构造不得与在飞帧重叠（preFrame 首构造可能跑在
	// GPU 执行上一帧期间），同 IzbShadow 重建规则。旧注释称「热开门（RPC 拨开关）」
	// 已不成立：setDiagnostics 全仓仅启动期一处调用，门进程内不可变（2026-10-02 核对）。
	evssmShadow = std::make_unique<RGL::EvssmShadow>(
		vkDevice, *resources, kShadowMapSize,
		shadowAtlasColumns(kActiveShadowCascades), kEvssmMipLevels, 5.5f,
		*shadowSlot, *momentsSlot);
}


// ── 批 14.5 FSR ────────────────────────────────────────────────────────────────
// 按名字现查图附件 slot（**不缓存下标**，同 TAA 锚点「现搜」的纪律：装饰器插入顺序变了
// 不会让这里认错附件）。FSR 的三个输入全部来自编译图本体 = 单一真相源。
int32_t Renderer::findAttachmentSlot(std::string_view name) const {
	const auto it = std::ranges::find_if(renderGraph.attachments,
		[name](const auto& a) { return a.name == name; });
	return it == renderGraph.attachments.end() ? -1
	                                           : static_cast<int32_t>(it - renderGraph.attachments.begin());
}

/** warn-once 日志（EvssmShadow 的 evssmWarnOnce 同族；FSR 两条降级告警共用）。
 *
 * 消息由调用方**先格式化好**再传进来：`if (flag)` 与 `if (logger)` 两个分支收在本函数里，
 * `fsrInputsReady` 才不会因为打日志而超认知复杂度红线。
 * @param flag 已告警标志（首次调用后置位）
 * @param msg  告警文本 */
void fsrWarnOnce(bool& flag, const std::string& msg) {
	if (flag)
		return;
	flag = true;
	if (auto* logger = RLLogger::getInstance())
		logger->warn("{}", msg);
}

// FSR 能不能真的接起来：设备能力 + 三个输入附件是否都在图里。失败时**自己把门关掉**
// （调用方只看返回值），从而降级路径与「门关」共用同一条零命令路径。
bool Renderer::fsrInputsReady() {
	// 能力前置：FSR 的 SPIR-V 声明 Float16 + Int16 capability（见 vkdevice.cppm 同批
	// 注释）。缺位时降级不构造，而不是让 vendor 在建 pipeline 时炸——同 samplerAnisotropy
	// → 不开 AF 的降级模式。
	if (!vkDevice.shader16Supported()) {
		fsrWarnOnce(fsrInputMissingWarned,
			"fsr: 设备缺 shaderFloat16 / shaderInt16（FSR 的 SPIR-V 声明了这两个 capability）"
			"⟹ FSR 关闭。设备扩展清单不必加 VK_EXT_subgroup_size_control——Vulkan 上 "
			"FORCE_WAVE64 恒不入选，见 docs/phase14_plan.md §1.12");
		return false;
	}
	// 输入齐备性判据 = **实际图形态**（附件在不在图里），不看配置键。色输入优先 taa_out
	// （TAA 在图里时 tonemap 采的是它，FSR 必须接同一张，否则等于把 TAA 旁路掉）。
	if (fsrColorSlot < 0) {
		fsrColorSlot = findAttachmentSlot(kTaaOutName);
		if (fsrColorSlot < 0)
			fsrColorSlot = findAttachmentSlot(kSceneHdrName);
		fsrVelocitySlot = findAttachmentSlot(kVelocityName);
		fsrDepthSlot = findAttachmentSlot("depth");
	}
	if (fsrColorSlot < 0 || fsrVelocitySlot < 0 || fsrDepthSlot < 0) {
		fsrWarnOnce(fsrInputMissingWarned, fmt::format(
			"fsr: 输入附件不齐（color={} velocity={} depth={}）⟹ FSR 关闭。"
			"色输入 = taa_out 或 scene_hdr，速度场 = velocity（需 velocity_field 开门）",
			fsrColorSlot, fsrVelocitySlot, fsrDepthSlot));
		return false;
	}
	return true;
}

// 门开才构造（门关 = 一次布尔判断，fsr 恒空 = 逐位红线，同 izb/evssm）。
// 位臵在 evssmPreFrame **之后**、logFirstFrameMemory 之前：三个图外模块都是首帧懒构造，
// 而 logStartupBaseline 在帧循环之前 ⟹ 晚于本函数打统计才不会漏账（批 9.5 同一病灶）。
void Renderer::fsrPreFrame() {
	auto z = zone("drawFrame.fsrPre");
	if (!fsrEnabled)
		return;
	const auto extent = swapchain->getRenderExtent();
	if (fsr && fsr->getRenderExtent() == extent)
		return;  // 已构造且渲染网格未变
	if (!fsrInputsReady()) {
		fsrEnabled = false;
		return;
	}
	// 重建前 waitIdle（同 RT 缓存 / izb 规则）。vendor 的内部共享图按 maxRenderSize 定
	// 尺寸 ⟹ 渲染网格一变**必须**重建，且时域历史全丢（不是可选项，见 §1.12 第 4 条）。
	vkDevice.getDevice().waitIdle();
	fsr = std::make_unique<RGL::FsrUpscaler>(vkDevice, *resources, extent,
		swapchain->getExtent());
}

// FSR sandwich：只认 tonemap pass（其余 pass 零命令）。录在它**之前**、且此刻 tonemap
// 自己的 barrier 已在 :482 物化完 ⟹ 三个输入的真实布局就是追踪表里的值。
void Renderer::fsrTonemapPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
	const FrameDrawList& list) {
	if (!fsr || pass.kind != RgPassKind::drawLight || pass.state != kTonemapState)
		return;
	// 两个图属输入按**编译图里的附件名**去 RT 缓存取（drawFrame 的 ensureOffscreenAttachments
	// 已在 pass 循环之前跑完 ⟹ 此处必已存在；取不到就是装配错，直接不录而不是崩）。
	// 不在调用点传 VkImage 的理由：那要把本文件私有的 SlotResolver 暴露进接口分区。
	const auto attImage = [this](int32_t slot) -> vk::Image {
		if (slot < 0)
			return {};
		auto* att = rtCache->find(renderGraph.attachments[static_cast<size_t>(slot)].name);
		return att ? static_cast<vk::Image>(att->image.image) : vk::Image{};
	};
	const vk::Image colorImage = attImage(fsrColorSlot);
	const vk::Image velocityImage = attImage(fsrVelocitySlot);
	if (!colorImage || !velocityImage)
		return;

	// ① 速度场在挂点处于 COLOR_ATTACHMENT_OPTIMAL —— 它是全图唯一不达 FSR 要求的输入
	//   （velocity pass 刻意没有下游消费者，布局停在 CAO 且因 attachmentLayouts 跨帧持久
	//   而下帧继续被当 oldLayout 用）。这里转成 SRO 并**同步写追踪表**：漏写 = 债-72
	//   形态（下帧 velocity 写 barrier 拿错 oldLayout，症状隔一整帧、报错点无辜）。
	//   自己转而不是让 vendor 转，是为了让「FSR 不改动图属资源布局」成为**结构性**事实，
	//   不依赖 vendor 内部对 SRV 选哪个布局（那会随上游版本漂）。
	const auto velSlot = static_cast<size_t>(fsrVelocitySlot);
	if (attachmentLayouts[velSlot] != vk::ImageLayout::eShaderReadOnlyOptimal) {
		vk::ImageMemoryBarrier2 b{};
		b.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput;
		b.srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite;
		b.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
		b.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
		b.oldLayout = attachmentLayouts[velSlot];
		b.newLayout = vk::ImageLayout::eShaderReadOnlyOptimal;
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = velocityImage;
		b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		vk::DependencyInfo dep{};
		dep.setImageMemoryBarriers(b);
		cmd.pipelineBarrier2(dep);
		attachmentLayouts[velSlot] = vk::ImageLayout::eShaderReadOnlyOptimal;
	}

	// ② 护栏（14.4b 同款，从约定变断言）：三个输入的**追踪表布局必须已是 SRO**，因为
	//   FsrUpscaler 向 vendor 申报的 state 就是 SRO，而 vendor 只在
	//   `currentState != newState` 时才发 barrier（ffx_vk.cpp:1237）——申报错 = 它跳过
	//   转换 = shader 在错误布局下读写。VVL 抓得到布局漂移，但报点在下一次使用处，
	//   归因成本高，所以在这里先喊出「进 FSR 之前就已经错」。
	if (auto* lg = RLLogger::getInstance()) {
		const std::pair<const char*, int32_t> want[] = {
			{"色输入", fsrColorSlot}, {"深度", fsrDepthSlot}, {"速度场", fsrVelocitySlot}};
		for (const auto& [what, slot] : want)
			if (attachmentLayouts[static_cast<size_t>(slot)]
				!= vk::ImageLayout::eShaderReadOnlyOptimal)
				lg->error("fsr: {} 附件进 FSR 时布局 = {}，而 FsrUpscaler 向 vendor 申报的是 "
				          "SRO（COMPUTE_READ）⟹ vendor 会跳过布局转换。要么补转换、要么改申报",
				          what,
				          static_cast<uint32_t>(
					          attachmentLayouts[static_cast<size_t>(slot)]));
	}

	FsrFrameInput in{};
	in.color = colorImage;
	in.depth = *resources->getDepthImage();
	in.velocity = velocityImage;
	in.nearPlane = list.fsrNear;
	in.farPlane = list.fsrFar;
	in.fovVertical = list.fsrFovVertical;
	in.reset = !fsrDispatchedOnce;  // 首帧 = 时域重置（vendor 靠它清内部历史）
	fsrDispatchedOnce = true;
	fsr->recordFrame(cmd, in);
	// vendor 的 VK 后端逐 job 走 `vkCmdBindDescriptorSets` ⟹ CB 已被切进 classic 模式，
	// heap 绑定作废（坑 9 的新实例，VVL 会点名下一次 mesh draw「heap was not bound」）。
	// tonemap 之后还有 pass（imgui 叠加层等），所以这里**立刻**重绑，不能等下一帧的
	// recordFixedDataPath。
	bindDescriptorHeaps(cmd);
}

// 批 9.5 补：显存账的**懒构造盲区**修补。logStartupBaseline 在帧循环之前打统计，而
// IzbShadow / EvssmShadow 都是首帧 drawFrame 内懒构造（izbPreFrame / evssmPreFrame）
// ⟹ startup 那次统计系统性漏掉这两个模块的全部资源（实测：开/关 shadow_evssm 的
// vma[startup] 逐字节相同）。此处在两个 preFrame 之后补打一次，让显存账真的含它们。
// 一次性；抽成方法而非内联 if 是为了不把 drawFrame 的认知复杂度推过 15 红线。
void Renderer::logFirstFrameMemory() {
	if (memStatsLogged)
		return;
	memStatsLogged = true;
	resources->logMemoryStats("first_frame");
}

// 批 11.6d-2：EVSSM sandwich（与 izbLightPassPre 同位；门空早退 = 零命令）。
void Renderer::evssmLightPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass) {
	if (!evssmShadow || pass.kind != RgPassKind::drawLight || pass.state != "light")
		return;
	evssmShadow->recordFrame(cmd);
}

// IZB mask pass（批 11.3-1d 后半）：beginRendering(G×G 零附件) + izb_mask 动态态
// （OVERESTIMATE/双面/无深度）+ shadow0 段逐 draw 重放——管线条目按 vertexSlot
// 换 izb_mask 形态，push = ShadowDrawArgs 8B 原样 + gridDim 4B（render_list 零改动）。
void Renderer::replayIzbMaskSegment(vk::raii::CommandBuffer& cmd, const FrameDrawList& list) {
	const auto seg = segmentSpan(shadowDrawSource(0), list);
	if (seg.empty()) {
		if (!izbMaskSegmentWarned) {
			izbMaskSegmentWarned = true;
			if (auto* lg = RLLogger::getInstance())
				lg->warn("izb: shadow0 段为空（shadow_pass 关或无投影体）→ mask 无内容");
		}
		return;
	}
	const uint32_t gridDim = izbShadow->getGridDim();
	izbShadow->beginMaskPass(cmd);
	const vk::Rect2D gridRect{{0, 0}, {gridDim, gridDim}};
	RGL::applyDynamicState(cmd, gridRect, stateKit.stateFor("izb_mask"),
		stateKit.depthClipDynamicEnabled(), 0);
	// 保守光栅（EDS3 动态位）：**只在 mask pass 内 set、出 pass 立即复原 DISABLED**
	// ——不在 applyDynamicState 里给所有 pass 写（RADV 显式 DISABLED ≠ 从不 set，
	// 会污染 shadow 光栅红线，2026-10-02 复盘）；能力缺失 = 永不 set = 逐位不变
	const bool conservative = stateKit.conservativeDynamicEnabled()
		&& stateKit.stateFor("izb_mask").conservative;
	if (conservative)
		cmd.setConservativeRasterizationModeEXT(vk::ConservativeRasterizationModeEXT::eOverestimate);
	uint32_t maskDraws = 0;  // 批 9.5：本段 draw 数（循环外一次性入账，热循环只多一次自增）
	for (const auto& call : seg) {
		if (call.pushData.size() < sizeof(ShadowDrawArgs))
			continue;  // 契约破裂防御（同 replayClonedPass 的越界跳策略）
		const auto maskIdx =
			registry.izbMaskEntryFor(registry.entry(call.pipeline).bindings.slotVertex);
		if (!maskIdx)
			continue;  // 门关（条目未建）
		const auto& entry = registry.entry(*maskIdx);
		std::array<std::byte, 12> push{};
		std::ranges::copy_n(call.pushData.begin(), 8, push.begin());
		*reinterpret_cast<uint32_t*>(push.data() + 8) = gridDim;
		cmd.pushDataEXT(vk::PushDataInfoEXT(entry.pushDataSet,
			vk::HostAddressRangeConstEXT(push.data(), push.size())));
		entry.pipeline.bindShaders(cmd);
		cmd.drawMeshTasksEXT(call.groupCount, 1, 1);
		++maskDraws;
	}
	profilerAddDraws(maskDraws);  // 批 9.5：IZB mask 重放段（= shadow0 全量重放的 draw 数）
	// OVERESTIMATE 复原必须在 mask pass 内（动态态命令不可出 renderpass；light/tonemap
	// 继承 DISABLED = 与默认等值，实测该路径无红线扰动）
	if (conservative)
		cmd.setConservativeRasterizationModeEXT(vk::ConservativeRasterizationModeEXT::eDisabled);
	izbShadow->endMaskPass(cmd);
}

// offscreen attachment RT 幂等 ensure（批次 4；extent 随 resize 重建含 waitIdle，录制期调用安全；format 经词表解析，"inherit"=swapchain 格式、具名=显式 RT）。出参 offscreenRt 按 slot 填（非 offscreen 槽留 null）。
std::expected<void, Error> Renderer::ensureOffscreenAttachments(vk::Extent2D extent,
	std::vector<RtAttachment*>& offscreenRt) {
	auto z = zone("drawFrame.offscreen");  // 批 9.5 观测：offscreen RT 幂等 ensure（稳态 = 查表）
	offscreenRt.assign(renderGraph.attachments.size(), nullptr);
	// RT 重建检测：generation 变化（新图布局 Undefined）→ 同步重置 attachmentLayouts，否则下帧首 barrier oldLayout 失配（resize 必现）。
	// ⚠ rtGenerations 是**成员**（跨帧快照），不是本函数的局部量——见 vkrenderer.cppm 成员
	// 注释：局部量每帧归零会让本判断恒真，把每个 offscreen 附件每帧抹成 eUndefined，
	// 摧毁唯一的跨帧消费者 taa_hist（批 12.6 T4 实测：TAA 历史恒黑）。
	for (uint32_t s = 0; s < renderGraph.attachments.size(); ++s) {
		const auto& att = renderGraph.attachments[s];
		// 图属 RT 两类同走本缓存（批 11.2 基建）：offscreen = 颜色档，
		// depthOffscreen = 深度档（阴影图，aspect 随格式走深度面）
		const bool depthRt = att.kind == RgAttachmentKind::depthOffscreen;
		if (!depthRt && att.kind != RgAttachmentKind::offscreen)
			continue;
		const auto fmt = RGL::resolveRgFormat(att.format, swapchain->getFormat().format);
		// 尺寸：声明了独立 extent 就用它，否则跟随 swapchain。阴影图必须独立——绑在窗口
		// 尺寸上会让阴影 texel 密度随窗口变，且每次 resize 白白重建整张图
		const vk::Extent2D rtExtent{att.extent[0] != 0u ? att.extent[0] : extent.width,
			att.extent[1] != 0u ? att.extent[1] : extent.height};
		auto rtResult = rtCache->ensure(att.name, rtExtent, fmt,
			depthRt ? vk::ImageAspectFlagBits::eDepth : vk::ImageAspectFlagBits::eColor);
		if (!rtResult)
			return std::unexpected(std::move(rtResult).error());
		offscreenRt[s] = *rtResult;
		if ((*rtResult)->generation != rtGenerations[s]) {
			rtGenerations[s] = (*rtResult)->generation;
			attachmentLayouts[s] = vk::ImageLayout::eUndefined;
		}
	}
	return {};
}
}  // namespace RGL
