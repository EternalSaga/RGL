module;
#include <vulkan/vulkan.h>  // VK_* 句柄类型；rlvk 不含
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
