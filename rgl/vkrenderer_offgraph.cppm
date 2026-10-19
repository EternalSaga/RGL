module;
#include <vulkan/vulkan.h>  // VK_* 句柄类型；rlvk 不含
// VK_* 句柄类型；rlvk 不含
#include <fmt/format.h>     // initFsr 的异常文本格式化
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
import frame_session;    // B2-4③′ 债-124：kFramesInFlight（RT 退役 due 帧计算）
import shadow_config;    // shadowConfig() / shadowFormPlan()（IZB 与 EVSSM 两扇图外门，批 N6b）
import profiler;         // zone（IZB / EVSSM 帧首段观测）
import frame_draw_list;  // segmentSpan（mask 重放段名 → draw 序列）
import scene_context;    // shadowDrawSource（段名单一来源，批 11.3-1d）
import render_graph;     // RgAttachmentKind（重放后布局回正）+ B2-4③′：registerBufferAttachment / checkBufferWriteBound
import dynamic_render_graph;  // B2-4③′：graph 成员类型（syncIzbGraphAttachments 调 declareBufferProductions）
import vulkan_utils;     // layoutDrift（图外布局对账的判定 + 值可读化，批 14.4b 步 2）
import izb_shadow;
import evssm_shadow;
import fsr_upscaler;
import meshlet_cull_service;  // ★1-a2-i：逐簇视锥剔除（帧首图外 compute）
import pass_vocabulary;  // kTonemapState（批 14.5 FSR 认 tonemap pass）
import taa_jitter;        // contentShiftPx（批 14.5 第七批：申报给 FSR 的是内容位移，非投影注入量）

namespace RGL {

namespace {

/**
 * 四个图外工作的**申报名**（批次 B1）。
 *
 * 申报表（`Renderer::offGraphWorkDecls`）与四个 `offGraphEnter` 调用点共用这批常量：
 * 名字一旦两处各写一份字面量，拼错的那一份会退化成「未申报」运行期报错，
 * 而不是编译期或启动期发现。
 */
constexpr std::string_view kWorkMeshletCull = "meshlet_cull";
constexpr std::string_view kWorkIzbMask = "izb_mask";
constexpr std::string_view kWorkEvssmMoments = "evssm_moments";
constexpr std::string_view kWorkFsrUpscale = "fsr_upscale";

/** 违规出声（A 档口径：只点名不改行为，交付路径命令流逐位不变）。 */
void reportOffGraphViolation(const Error& err) {
	if (auto* lg = RLLogger::getInstance())
		lg->error("{}", err.describe());  // describe() 自带 [Code] 前缀，消息本体已含归因
}

/**
 * extent 漂移 → **退役**（B2-4③′ 债-124；ensureOffscreenAttachments 的判据本体）。
 *
 * 发现帧 D：立刻摘出活动表（下帧 ensure 按新尺寸重建），GPU 资源到 D + frames-in-flight
 * 才销毁——in-flight 帧继续安全引用退役区旧图。「有 in-flight 用户逐出必须炸」= 台账拒
 * 重复 retire（Error 出声；连续 resize 的销毁记账吞并形态在这里红）。
 * 三个否定分支一行早退（认知复杂度：ensure 主循环是逐附件布局表唯一重置入口，红线留给它）。
 */
void requestRetireOnDrift(RenderTargetCache& cache, const std::string& name,
                          vk::Extent2D want, uint64_t frame) {
	auto* live = cache.find(name);
	if (!live || live->extent == want || cache.retiredPending(name))
		return;
	if (auto r = cache.retire(name, frame + FrameSession::kFramesInFlight); !r)
		reportOffGraphViolation(r.error());
}

}  // namespace

// 11.3-1d IZB 帧首段（声明注释见 vkrenderer.cppm）。deferred 图判据 = 存在
// drawLight "light" pass；重建前 waitIdle（旧 buffer 可能被上一帧命令引用，RT 缓存
// 同款规则）。门关 = 本函数不被进入（izbShadow 恒空 + 单布尔判断）。
void Renderer::izbPreFrame() {
	auto z = zone("drawFrame.izbPre");  // 批 9.5 观测：IZB 帧首段（dump 收割 + 门控重建）
	if (izbShadow)
		izbShadow->drainReadback();
	++izbFrameNo;
	// ⚠ 构造判据**只有** `consumeIzbMask`（= configs/shadow.lua 的 `izb`），刻意不掺
	// drawAtlas：mask 每帧无条件清零是债-32 的收口，掺第二个判据就把它拆了。
	if (!shadowFormPlan(shadowConfig()).consumeIzbMask)
		return;
	const bool deferred = graphHasDeferredLight(renderGraph.passes);
	// 深度按 **kind** 查（IZB 要的是「那张深度附件」，与它叫什么无关）——与 FSR 的按名查
	// 是两条不同轴，理由见 `kDepthName` 头注释。债-129 S1a：现查收进图事实查询面。
	const auto depthAtt = graphAttachmentSlotByKind(renderGraph.attachments, RgAttachmentKind::depth);
	// 非 deferred 在此静默早退是**合法运行期形态**（图判定不该在录制路径打日志）；
	// 「izb 配了却必然无效」由 gate_lint 启动期 error 喊（规则按门名 `izb` 登记，2026-10-02）。
	if (!deferred || !depthAtt)
		return;
	depthAttSlot = static_cast<int32_t>(*depthAtt);
	const auto extent = swapchain->getRenderExtent();  // 批 14.3：IZB mask 与颜色附件同网格
	if (!izbShadow || izbShadow->getExtent() != extent) {
		vkDevice.getDevice().waitIdle();
		izbShadow = std::make_unique<RGL::IzbShadow>(
			vkDevice, *resources, extent, shadowConfig().izbGrid, offGraphSync);
		// B2-4③′ 迁入：首建/漂移重建后把 head/nodes 声明进图（注册 + 写上界对账 +
		// declareBufferProductions，幂等）；标脏后**下个安全点**重编译吃到（延迟 ≤1 帧，
		// D5 第 5 条明确允许）。门关 = 本函数不被进入 = 图里永远没有 izb 附件。
		syncIzbGraphAttachments();
	}
}

// B2-4③′ IZB 附件的图侧同步（声明注释见 vkrenderer.cppm）。注册动作住 rlframe 而不是
// izb_shadow（rlgfx）的理由：注册表在 render_graph（rlscene），逆向 import 立刻链接失败；
// 尺寸/写上界公式仍住 izb_shadow 接口（唯一真相源，两侧同吃）。
void Renderer::syncIzbGraphAttachments() {
	const auto extent = izbShadow->getExtent();
	const auto grid = izbShadow->getGridDim();
	const vk::DeviceSize headBytes = izbHeadBytes(grid);
	const vk::DeviceSize nodesBytes = izbNodesBytes(extent);
	// 同名替换 = extent 漂移的正常路径（B2-4③′ 契约④）；**按值捕获字节**，不捕获
	// izbShadow——注册表是进程级单例，比模块活得久。
	registerBufferAttachment(kIzbHeadAttachment, [headBytes] { return headBytes; });
	registerBufferAttachment(kIzbNodesAttachment, [nodesBytes] { return nodesBytes; });
	// 🔴 写上界对账（判据 5）：注册字节 ≥ dispatch 写上界，失败 = throw 在**变异现场**
	//（消息点名两侧字节数与差值；meshlet B2-4② 的先例口径）。
	if (auto bound = checkBufferWriteBound(kIzbHeadAttachment, izbHeadWriteBound(grid)); !bound)
		throw std::runtime_error(fmt::format("Renderer: IZB head 附件对账失败：{}",
			bound.error().describe()));
	if (auto bound = checkBufferWriteBound(kIzbNodesAttachment, izbNodesWriteBound(extent)); !bound)
		throw std::runtime_error(fmt::format("Renderer: IZB nodes 附件对账失败：{}",
			bound.error().describe()));
	// 生产者声明（幂等 + 试编译 fail-fast；pass 名 = builtin 的 drawLight "light"）。
	// 失败只出声不落脏位：试编译拒了说明注册/声明本身错，落脏位只会让每帧安全点重复失败刷日志。
	if (auto declared = graph.declareBufferProductions("light",
			{ std::string{kIzbHeadAttachment}, std::string{kIzbNodesAttachment} });
		!declared)
		reportOffGraphViolation(declared.error());
}

// B2-4④′ meshlet 标志表的图侧同步（声明注释见 vkrenderer.cppm）。与 IZB 的差别不是细节：
// IZB 是 producer-only（图里没有消费者，barrier 全走图外边表），meshlet 是**图的第一批
// 真消费者声明**——storageRead 进 pass inputs 后，「off-graph 写 × in-graph 读」这份共享
// 对图可见，并由 declareBufferConsumptions 落账前的附件↔边对账机器核对（缺边 = 下面的
// throw 在装配现场，消息点名附件 + 两侧 pass + 表名）。两条声明都**不产任何命令**
// （buffer 档 storage usage 结构性不进 barrier / beginRendering），逐位不变量不受影响。
void Renderer::syncMeshletGraphAttachments() {
	const auto& passes = graph.desc().passes;  // desc = 真相源（初始图已在 main 装饰并编译）
	if (passes.empty())
		throw std::runtime_error("Renderer: meshlet 图声明失败——desc 里没有 pass（装配错）");
	// 生产者锚点 = 图首 pass（cull dispatch 在帧首 prologue = 一切 pass 之前；图能表达的
	// 最强事实是「先于一切消费者」，shadow 级 0 无条件前插 = 任何形态的 pass 0）。
	const std::string producerPass = passes.front().name;
	const auto consumer = std::ranges::find_if(passes, [](const RgPassDesc& p) {
		return p.kind == RgPassKind::drawMesh && p.drawSource == "opaque";
	});
	if (consumer == passes.end())
		throw std::runtime_error("Renderer: meshlet 图声明失败——图里没有 drawSource=opaque 的 "
		                         "drawMesh pass（meshlet 取代段无处落点，装配错）");
	if (auto r = graph.declareBufferProductions(producerPass,
			{ std::string{kMeshletVisibleFlagsAttachment} });
		!r)
		throw std::runtime_error(fmt::format("Renderer: meshlet 标志表生产者声明失败：{}",
			r.error().describe()));
	if (auto r = graph.declareBufferConsumptions(consumer->name,
			{ std::string{kMeshletVisibleFlagsAttachment} });
		!r)
		throw std::runtime_error(fmt::format("Renderer: meshlet 标志表消费者声明失败：{}",
			r.error().describe()));
}

// 11.3-1d IZB sandwich（声明注释见 vkrenderer.cppm；门关 = izbShadow 恒空早退）。
void Renderer::izbLightPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
	const FrameDrawList& list) {
	if (!izbShadow || pass.kind != RgPassKind::drawLight || pass.state != "light")
		return;
	// 批次 B1：位置契约申报（挂点 = 编译图里 drawLight "light" 之前，见 offGraphWorkDecls）。
	// 上面那半条 kind/state 判据在 B1 之后是**冗余**的（锚点唯一性由构造期保证，图里出现
	// 第二个 drawLight "light" 会直接启动失败），留着是因为它零成本且读起来更直给。
	offGraphEnter(kWorkIzbMask);
	// 批 14.4b A 档护栏：图外 sandwich 的「进出同值」从**约定**变**断言**。
	// 为什么必须有：追踪表 `attachmentLayouts` 的唯一写者就是执行器自己，图外模块发的
	// barrier 它一概不知 ⟹ 漂移后执行器下一跳会拿**错的 oldLayout** 去转布局（债-72）。
	// VVL **能**报这类漂移（批 14.4b 阳性锚点实测推翻旧结论「VVL 不报」，详见
	// docs/phase14_plan.md §1.11），护栏仍有三条独立价值：**归因**（VVL 报在下一次
	// barrier 那个无辜调用点，本护栏直接点名肇事模块）、**不依赖调试层**（交付配置里
	// VVL 不在场）、**早一帧**。
	// 14.5 的 FSR 是第二个这样的 sandwich，先在这里把模式立起来。
	const auto depthSlotIdx = static_cast<size_t>(depthAttSlot);
	const auto trackedLayout = attachmentLayouts[depthSlotIdx];
	const auto leftLayout = izbShadow->recordFrame(cmd,
		IzbFrameInput{list.sceneData.skyInvViewProj, list.sceneData.shadowViewProj[0]},
		trackedLayout,
		// N 帧起每 30 帧转一次（ppm 覆盖写 + 直方图日志行）——一次性判等会被 RPC
		// 截图时序甩开（imgqa izb 判据要求 mask 与截图同相机状态，机位是 RPC 后设的）
		// 门在 shadow.lua 的 izb.dump_frame（N6b：原 diagnostics.izb_dump_frame）
		izbDumpDue());
	// 判定与值可读化走 `vulkan_utils::layoutDrift`（批 14.4b 步 2 收成单点纯函数），
	// **后果说明留在本处**——它与下面 FSR 那段的诊断语义不同（一个是"执行器下一跳拿错
	// oldLayout"，一个是"vendor 跳过转换"），不该并成一份。
	// 诊断缺陷修正（步 2 顺手修）：原来这条打的是「进入 = X，离开 = Y，追踪表 = X」，
	// 而**进入与追踪表是同一个变量**（`trackedLayout` 既当进入值又当追踪表值，恒等）
	// ⟹ 三个占位符里有一个是冗余的，读者会以为拿到了两个独立事实（恒等式日志 = 有一个
	// 分支从未生效）。现在只报两个真正独立的量：期望（追踪表）/ 实际（recordFrame 返回）。
	if (auto* logger = RLLogger::getInstance()) {
		if (const auto drift = layoutDrift("izb: 深度附件", trackedLayout, leftLayout))
			logger->error("{}。渲染图不知道这次转换，下一跳会拿错的 oldLayout 转布局"
				"（VVL 要等下一次 barrier 才响，且报在那个无辜调用点、只在调试层在场时存在）；"
				"recordFrame 的返回值必须由实际发出的 barrier 推进",
				*drift);
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
	auto z = zone("drawFrame.evssmPre");  // 批 9.5 观测：EVSSM 帧首段（门 = shadow.lua mode=="evssm"）
	// N6b：判据收成形态决策表一项（= 旧 `shadow_evssm && shadow_pass`，等价性见
	// shadowFormPlan 的注释）。**注意不要把 buildEvssmMoments 换成 drawAtlas**：
	// mode==pcss 时图集照画但不建矩图。
	if (evssmShadow || !shadowFormPlan(shadowConfig()).buildEvssmMoments)
		return;
	// 非 deferred 组合由 gate_lint 启动期 error（规则按门名 `mode` 的 evssm 档登记，
	// 2026-10-02 从这里的 warn-once 迁走——不走到这一帧就永远不喊的病灶收口）；
	// 运行期早退保留（消费点在 deferred light pass，判据不变）。
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
		vkDevice, *resources, *rtCache, kShadowMapSize,
		shadowAtlasColumns(kActiveShadowCascades), kEvssmMipLevels, 5.5f,
		*shadowSlot, *momentsSlot, offGraphSync);
}


// ── 批 14.5 FSR ────────────────────────────────────────────────────────────────
// 批 14.5：FSR 的构造（Renderer ctor 调用；抽成方法是为了不把 ctor 的认知复杂度推过红线）。
// 判据与装配层 `assembleBackEnd` 的 fsrUsable **必须一致**——不一致的后果是「tonemap 的色
// 输入槽已烘进管线条目却没人写描述符」= 读到未初始化描述符，画面出垃圾且不指名，
// 所以这里**抛**而不是静默不构造。
void Renderer::initFsr() {
	const auto slot = resources->ensureFsrSampleSlot();
	if (!slot)
		throw std::runtime_error("Renderer: FSR 采样槽领取失败");
	// 三个输入 = 图事实，唯一来源 = `render_graph` 的查询面（债-129 S1a）。色输入优先级链
	// `ao_applied > taa_out > scene_hdr` 与「选 taa_out ⟹ 已在基空间 ⟹ jitterOffset 必须取
	// 0」这条不变量**整段搬进 `graphFsrColorInput`**（链与它的理由都在那儿，本处不重述）。
	// 收口前这里是三次现查 + 两次比较，而装配层 `computeFsrUsable` 另写一份**漏了
	// ao_applied 档**的条件集（4 条 vs 3 条）——两份判据各说各话正是这次正交化的病本体。
	// 🔴 判齐备只准用 `complete()`：那才是与装配层共享的**判据本体**，不是共享函数名。
	const auto inputs = graphFsrInputs(renderGraph.attachments);
	if (!inputs.complete())
		throw std::runtime_error(fmt::format(
			"Renderer: fsrEnabled 但编译图里缺 FSR 输入附件（color={} velocity={} depth={}）"
			"——装配层的 fsrUsable 判据与本处不一致 = 两处判据漂移",
			inputs.color ? static_cast<int32_t>(inputs.color->slot) : -1,
			inputs.velocity ? static_cast<int32_t>(*inputs.velocity) : -1,
			inputs.depth ? static_cast<int32_t>(*inputs.depth) : -1));
	fsrColorSlot = static_cast<int32_t>(inputs.color->slot);
	fsrColorBaseSpace = inputs.color->baseSpace;
	fsrVelocitySlot = static_cast<int32_t>(*inputs.velocity);
	fsrDepthSlot = static_cast<int32_t>(*inputs.depth);
	fsr = std::make_unique<RGL::FsrUpscaler>(vkDevice, *resources,
		swapchain->getRenderExtent(), swapchain->getExtent(), *slot, fsrTemporal);
}

// 只做**渲染网格变化后的重建**。构造不在这里（见 initFsr 的理由）；门关 = 一次指针判空。
// 位臵仍在 evssmPreFrame 之后、logFirstFrameMemory 之前：重建是懒的，重建出来的资源要进
// first_frame 那笔账才不漏（批 9.5 同一病灶）。
void Renderer::fsrPreFrame() {
	auto z = zone("drawFrame.fsrPre");
	if (!fsr)
		return;
	const auto extent = swapchain->getRenderExtent();
	if (fsr->getRenderExtent() == extent)
		return;
	// vendor 的内部共享图按 maxRenderSize 定尺寸 ⟹ 渲染网格一变**必须**重建，且时域历史
	// 全丢（不是可选项，见 §1.12 第 4 条）。重建前 waitIdle（同 RT 缓存 / izb 规则）；
	// 重建往**同一个槽**重写输出描述符，管线条目里烘的槽号因此终身稳定。
	vkDevice.getDevice().waitIdle();
	initFsr();
}

// 图外编舞的**永久布局转换**原子原语（声明注释见 vkrenderer.cppm：为什么 barrier 与追踪表
// 必须由同一个量产出、以及为什么临时 sandwich 不得用它）。目前唯一用户 = 下面的 FSR velocity。
vk::ImageLayout Renderer::transitionAndTrack(vk::raii::CommandBuffer& cmd, size_t slot,
	vk::Image image, vk::ImageLayout newLayout, vk::ImageAspectFlags aspect,
	vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
	vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
	const auto before = attachmentLayouts[slot];
	// 同布局早退：一条命令都不发。判据的宽窄 = 「是否发 barrier」的判据，别改（声明处有理由）。
	if (before == newLayout)
		return before;
	// ⚠ barrier 的 newLayout 与写进追踪表的值**必须是同一个变量**——本原语存在的全部理由：
	//   一旦拆成两个字面量，表就与真实布局脱钩，而读表的护栏跟着退化成恒真的同义反复。
	const auto written = newLayout;
	vk::ImageMemoryBarrier2 b{};
	b.srcStageMask = srcStage;
	b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage;
	b.dstAccessMask = dstAccess;
	b.oldLayout = before;
	b.newLayout = written;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image = image;
	b.subresourceRange = {aspect, 0, 1, 0, 1};
	vk::DependencyInfo dep{};
	dep.setImageMemoryBarriers(b);
	cmd.pipelineBarrier2(dep);
	attachmentLayouts[slot] = written;
	return written;
}

// FSR sandwich：只认 tonemap pass（其余 pass 零命令）。录在它**之前**、且此刻 tonemap
// 自己的 barrier 已在 :482 物化完 ⟹ 三个输入的真实布局就是追踪表里的值。
void Renderer::fsrTonemapPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass,
	const FrameDrawList& list) {
	if (!fsr || pass.kind != RgPassKind::drawLight || pass.state != kTonemapState)
		return;
	// 两个图属输入的解析已提为 `fsrInputs()`：帧末覆盖对账要问「这帧到底该不该录」，
	// 判据必须与这里的早退**是同一个表达式**，否则「输入取不到所以没录」会被读成静默旁路。
	const auto inputs = fsrInputs();
	if (!inputs)
		return;
	const vk::Image colorImage = inputs->color;
	const vk::Image velocityImage = inputs->velocity;
	offGraphEnter(kWorkFsrUpscale);  // 批次 B1：门 = fsr 且两输入可解析，两者都过了才申报

	// ① 速度场在挂点处于 COLOR_ATTACHMENT_OPTIMAL —— 它是全图唯一不达 FSR 要求的输入
	//   （velocity pass 刻意没有下游消费者，布局停在 CAO 且因 attachmentLayouts 跨帧持久
	//   而下帧继续被当 oldLayout 用）。转换走 `transitionAndTrack` ⟹ barrier 的 newLayout 与
	//   写进追踪表的值是同一个变量，「漏写表」（债-72 形态：下帧 velocity 写 barrier 拿错
	//   oldLayout，症状隔一整帧、报错点无辜）在写法上不可能。
	//   自己转而不是让 vendor 转，是为了让「FSR 不改动图属资源布局」成为**结构性**事实，
	//   不依赖 vendor 内部对 SRV 选哪个布局（那会随上游版本漂）。
	//   转换目标取 `fsrInputLayout()`（= 向 vendor 申报的输入 state 所对应的布局，两者在
	//   fsr_upscaler_impl 里成对定义）⟹「转成 A 却申报 B」同样写不出来。
	transitionAndTrack(cmd, static_cast<size_t>(fsrVelocitySlot), velocityImage,
		fsrInputLayout(), vk::ImageAspectFlagBits::eColor,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);

	// ② 护栏（14.4b 同款，从约定变断言）：色输入与深度进 FSR 前**必须已处于申报布局**，因为
	//   vendor 只在 `currentState != newState` 时才发 barrier（ffx_vk.cpp:1237）——申报错 =
	//   它跳过转换 = shader 在错误布局下读写。VVL 抓得到布局漂移，但报点在下一次使用处，
	//   归因成本高，所以在这里先喊出「进 FSR 之前就已经错」。
	// ⚠ 这里**没有**「速度场」那一支，不是漏写：velocity 的布局正确性现在由**结构**保证
	//   ——转换目标就是 `fsrInputLayout()`，而它与向 vendor 申报的 state 成对定义。原来那一支
	//   读的是 velocity 自己的追踪表槽，而上面那个转换块**无论走不走早退**都已把该槽写成申报
	//   布局 ⟹ 断言恒假。恒假由批 14.4b 步 0 的镜像探针实测确认（改 barrier 的 newLayout 护栏
	//   不响、改表才响）。删掉它没有减少任何保护：velocity 的洞改由**结构**堵（见上），而不是
	//   由一条恒假断言假装在堵。保留的这两支防的是**另一件事**：图把色/深度留在非申报布局就进
	//   FSR（图侧没转，FSR 管不着），那条职责本批一字未动。
	// 判定与值可读化走 `vulkan_utils::layoutDrift`（与上面 IZB 那处共用，批 14.4b 步 2）；
	// 后果说明（vendor 跳过转换）留在本处，不与 IZB 那份并。
	if (auto* lg = RLLogger::getInstance()) {
		const std::pair<std::string_view, int32_t> want[] = {
			{"fsr: 色输入附件", fsrColorSlot}, {"fsr: 深度附件", fsrDepthSlot}};
		for (const auto& [what, slot] : want)
			if (const auto drift = layoutDrift(what, fsrInputLayout(),
					attachmentLayouts[static_cast<size_t>(slot)]))
				lg->error("{} ⟹ vendor 会跳过布局转换。要么补转换、要么改申报", *drift);
	}

	FsrFrameInput in{};
	in.color = colorImage;
	in.depth = *resources->getDepthImage();
	in.velocity = velocityImage;
	in.nearPlane = list.fsrNear;
	in.farPlane = list.fsrFar;
	in.fovVertical = list.fsrFovVertical;
	in.exposure = list.sceneData.exposure;  // tonemap 将施加的曝光（scene data @56）
	// 批 14.5 第四批：把投影里那份抖动原样报给 FSR（口径 → FSR输入契约 §3）。
	// 取 FrameDrawList 而不是重算 = 申报值与实际注入值同源，不可能漂移。
	// 申报给 FSR 的**不是**投影注入量，而是它造成的**内容位移**（Y 轴反号）。
	// 申报 = **色输入实际带的内容位移**（唯一来源 = `initFsr` 的分支选择，见 fsrColorBaseSpace）。
	// 色输入是 `taa_out` 时它已在基空间、位移为 0（照投影抖动申报会让 FSR 再 un-jitter 一次）；
	// 其余分支带抖动。此前申报走**另一条独立链路**，与这里选中哪个分支毫无耦合。
	const auto colorShift = contentShiftPx(list.fsrJitterPx);
	const float shiftScale = fsrColorBaseSpace ? 0.0f : 1.0f;
	in.contentShiftX = colorShift.x * shiftScale;
	in.contentShiftY = colorShift.y * shiftScale;
	in.reset = !fsrDispatchedOnce;  // 首帧 = 时域重置（vendor 靠它清内部历史）
	if (!fsrDispatchedOnce) {
		if (auto* lg = RLLogger::getInstance())
			// 申报量进这条诊断的理由（批 14.5 第四批）：它是**成对约束的一半**，而错了
			// 不报错——vendor 对 jitterOffset 零校验（只在 |j|>1 时 warn）。开了 fsr_jitter
			// 却因链路某处断掉而恒报 0，画面只是"抖动白加了"，没有任何信号会喊。
			// ⚠ 打印的是**内容位移**（不是投影注入量，两者 Y 反号）——第七批曾因读成注入量而误判。
			lg->info("FSRDIAG near={} far={} fov={} exposure={} contentShift=({},{})",
				in.nearPlane, in.farPlane, in.fovVertical, in.exposure,
				in.contentShiftX, in.contentShiftY);
		fsrDispatchedOnce = true;
	}
	fsr->recordFrame(cmd, in);
	// ③ FSR 输出 → tonemap 采样读：**必须补执行依赖**（缺了是 UB，不是"可能慢一点"）。
	//   为什么图属 barrier 覆盖不到：FSR 的输出图不是图附件（零拷贝烘采样槽），而本 pass
	//   的全部图 barrier 都物化在 `fsrTonemapPassPre` **之前**（`vkrenderer_passrec.cppm`
	//   的 `emitBarriers(pass.barriers)` → 本函数 → `beginPassRendering`，后者体内零 barrier）。
	//   vendor 侧也不补：它只在每个 job **前** flush（`ffx_vk.cpp:4026`），最后一个写 UAV 的
	//   job 之后什么都不发，`ffx_fsr3upscaler.cpp` 里 grep 不到任何 barrier/sync。
	//   ⚠ **VVL 抓不到**：布局本身是对的（输出常驻 GENERAL，GENERAL 可采样），缺的只是
	//   compute-write → fragment-read 的**顺序**。
	//   曾怀疑它是"时域档发糊"的根因（失效形态确实吻合：静止 + 无抖动时「读到上一帧」与
	//   「读到本帧」内容相同 ⟹ 看不出问题；开抖动则上一帧 = 另一个亚像素相位）。
	//   **实测否证**：补完后交付档 6.778→6.849，没动。真根因是申报 Y 反号，见 §1.19 §二。
	//   **但它必须留着**——缺执行依赖是 UB，与"是不是某个症状的病灶"无关；驱动换个调度
	//   就可能真读到上一帧，而那种错一次只会让人以为是着色器问题。
	{
		vk::ImageMemoryBarrier2 b{};
		b.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
		b.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
		b.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader;
		b.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
		b.oldLayout = b.newLayout = vk::ImageLayout::eGeneral;  // 不做布局转换，只要顺序
		b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		b.image = fsr->outputImage();
		b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
		vk::DependencyInfo dep{};
		dep.setImageMemoryBarriers(b);
		cmd.pipelineBarrier2(dep);
	}
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

// 簇化轴 ★1-a2-i：逐簇视锥剔除的帧首 dispatch（位置契约三条 → vkrenderer.cppm 声明注释；
// barrier 与 push 的内容全在 MeshletCullService::recordDispatch 里，本函数只做门控与计数）。
// 门关 = 恒 nullptr = 一次指针判空、**零命令**（逐位不变的构造性保证：判据是「有没有指针」，
// 不是「dispatch 了但标志全是 1」）。
void Renderer::recordMeshletCullDispatch(vk::raii::CommandBuffer& cmd, const FrameDrawList& list) {
	if (meshletCull == nullptr)
		return;
	auto z = zone("drawFrame.meshletCull");
	// 批次 B1：位置契约从注释变成断言——本函数被挪到 pass 循环之内/之后，帧末对账之前就会
	// error 出声（申报相位 = 帧首段，见 offGraphWorkDecls）。
	offGraphEnter(kWorkMeshletCull);
	meshletCull->recordDispatch(cmd, list.cullInput.planes, offGraphSync);
	// vkcmd.count 口径：3 条 pipelineBarrier2（B2-4② 起一条边一条命令，原先是一条装三条）
	// + bindShadersEXT + pushDataEXT + dispatch + 1 条 pipelineBarrier2 = 7。
	// 改 MeshletCullService::recordDispatch 的命令流必须同步改这里（同 recordFixedDataPath 的纪律）。
	profilerAddCommands(7);
}

// 批 11.6d-2：EVSSM sandwich（与 izbLightPassPre 同位；门空早退 = 零命令）。
void Renderer::evssmLightPassPre(vk::raii::CommandBuffer& cmd, const RgPassPlan& pass) {
	if (!evssmShadow || pass.kind != RgPassKind::drawLight || pass.state != "light")
		return;
	offGraphEnter(kWorkEvssmMoments);  // 批次 B1：位置契约申报（同 izbLightPassPre 挂点）
	evssmShadow->recordFrame(cmd);
}

// IZB mask pass（批 11.3-1d 后半）：beginRendering(G×G 零附件) + izb_mask 动态态
// （OVERESTIMATE/双面/无深度）+ shadow0 段逐 draw 重放——管线条目按 vertexSlot
// 换 izb_mask 形态，push = ShadowDrawArgs 8B 原样 + gridDim 4B（render_list 零改动）。
// izb dump 帧判据（声明处写了"为什么只准有一处"：它同时是**行为**的开关与**护栏**的门位）。
bool Renderer::izbDumpDue() const {
	const uint32_t from = shadowConfig().izbDumpFrame;
	return from != 0 && izbFrameNo >= from && (izbFrameNo - from) % 30 == 0;
}

void Renderer::replayIzbMaskSegment(vk::raii::CommandBuffer& cmd, const FrameDrawList& list) {
	const auto seg = segmentSpan(shadowDrawSource(0), list);
	if (seg.empty()) {
		// 债-32：段空**不再需要**走 mask pass——mask 的每帧清零已搬到 `IzbShadow::recordFrame`
		// 第 ⑧ 步（`izbLightPassPre` 每帧无条件先调它，再调本函数），所以这里早退不会让
		// light.frag 读到未初始化显存：mask 此刻恒为全 0 = 「无遮挡」，与 shadow.lua 的
		// `mode = "none"` 本应给出的画面一致（清零留在 beginMaskPass 之内时，这条早退 = 整帧不写
		// mask = 真 UB，本机此前逐位相同纯属新页读零的侥幸）。组合本身无意义由 gate_lint 启动期
		// error 喊（判据 = `izb.on × mode == "none"`，N6b 起两键同在 shadow.lua）。
		if (!izbMaskSegmentWarned) {
			izbMaskSegmentWarned = true;
			if (auto* lg = RLLogger::getInstance())
				lg->warn("izb: shadow0 段为空（shadow.mode = \"none\" 或无投影体）→ 本帧 mask 无内容 = 无遮挡");
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
// B2-4③′ 债-124：extent 漂移改走**退役窗口**（retire → due 帧销毁 + 还槽），不再依赖
// ensure 原地重建的 waitIdle；布局表在 due 前**不动**（旧 RT 仍在役，重置 = 下条 barrier
// 拿错 oldLayout），重建那一帧由 generation 失配自然触发重置。
std::expected<void, Error> Renderer::ensureOffscreenAttachments(vk::Extent2D extent,
		uint64_t frame, std::vector<RtAttachment*>& offscreenRt) {
	auto z = zone("drawFrame.offscreen");  // 批 9.5 观测：offscreen RT 幂等 ensure（稳态 = 查表）
	offscreenRt.assign(renderGraph.attachments.size(), nullptr);
	// 到期退役收割（安全点语义：本函数跑在 beginFrame 围栏之后，due 帧的 GPU 引用已终结）
	rtCache->processRetired(frame);
	// RT 重建检测：generation 变化（新图布局 Undefined）→ 同步重置 attachmentLayouts，否则下帧首 barrier oldLayout 失配（resize 必现）。
	// ⚠ rtGenerations 是**成员**（跨帧快照），不是本函数的局部量——见 vkrenderer.cppm 成员
	// 注释：局部量每帧归零会让本判断恒真，把每个 offscreen 附件每帧抹成 eUndefined，
	// 摧毁唯一的跨帧消费者 taa_hist（批 12.6 T4 实测：TAA 历史恒黑）。
	for (uint32_t s = 0; s < renderGraph.attachments.size(); ++s) {
		const auto& att = renderGraph.attachments[s];
		// 图属 RT 两类同走本缓存（批 11.2 基建）：offscreen = 颜色档，
		// depthOffscreen = 深度档（阴影图，aspect 随格式走深度面）。
		// 🔴 为什么写成穷尽 switch 而不是原来的「两个 kind 白名单 + 其余 continue」：本函数
		// 是逐附件布局追踪表 `attachmentLayouts` **唯一的重置入口**（RT 重建 → 该槽归零）。
		// 白名单形态下第五个 kind 静默落进 continue ⟹ 它的布局槽永不随资源重建归零，下帧首
		// 条 barrier 拿陈旧 oldLayout —— 与批 12.6 T4 那条「每帧抹成 eUndefined」是同一个
		// 变量的反方向漂移（那次是抹太狠，这次是漏抹），症状同样只在重建帧现形。
		// 穷尽 switch 把「这一档占不占 RT 缓存、走哪个 aspect」变成编译期必答。
		vk::ImageAspectFlags rtAspect = {};
		switch (att.kind) {
			case RgAttachmentKind::swapchain:  // 后缓冲：录制期按 imageIndex 解析，不占 RT
			case RgAttachmentKind::depth:      // 主深度图：Resources 持有
				continue;
			case RgAttachmentKind::offscreen:
				rtAspect = vk::ImageAspectFlagBits::eColor;
				break;
			case RgAttachmentKind::depthOffscreen:
				rtAspect = vk::ImageAspectFlagBits::eDepth;
				break;
			// buffer 档不占 RT 缓存（资源归模块申请），故与 swapchain/depth 同侧 continue。
			// 上面那条"漏抹布局"的担心对它不成立：`attachmentLayouts[s]` 只在 planBarrier
			// 生成的 image barrier 计划里被读，而 buffer 档的 usage 带 `layout == nullopt`
			// ⟹ 它的槽**结构性不会**进任何计划（见 render_graph_impl.cppm 的 UsageInfo）。
			// 也就是说这里 continue 的安全性来自那条不变式，而不是"记得写 continue"。
			case RgAttachmentKind::buffer:
				continue;
		}
		const auto fmt = RGL::resolveRgFormat(att.format, swapchain->getFormat().format);
		// 尺寸：声明了独立 extent 就用它，否则跟随 swapchain（× extentScale，批 15.3B：
		// ssr_trace 半分辨率——缩放口径与 ensureSsrTraceSlot 共用 scaleRtExtent，两处各写
		// 一份 floor = 每帧重建）。阴影图必须独立——绑在窗口尺寸上会让阴影 texel 密度随
		// 窗口变，且每次 resize 白白重建整张图。能力观测批 1 起「独立优先」也收进
		// resolvedRtExtent（第二个消费者 = /state 能力段的 width/height，公式不再有两份）。
		const vk::Extent2D rtExtent = resolvedRtExtent(att.extent, att.extentScale, extent);
		// B2-4③′ 债-124：extent 漂移 → 退役（判据与本体见 requestRetireOnDrift）。
		requestRetireOnDrift(*rtCache, att.name, rtExtent, frame);
		auto rtResult = rtCache->ensure(att.name, rtExtent, fmt, rtAspect);
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
// ── 批次 B1：图外工作申报面接线（定案 → docs/compute_调研.md §6 D1；批次与判据 → §7）──

std::vector<RgOffGraphWorkDecl> Renderer::offGraphWorkDecls() {
	// 顺序 = 帧末门位数组的顺序（offGraphEndFrame），项数 = kOffGraphWorkCount（构造期核对）。
	// 锚点用 pass_vocabulary 的状态名常量，不用字面量：状态名一旦改名，这里跟着走，
	// 而原先写在 C++ 里的字符串比较会静默失配（判据 = 启动期 throw，不是画面悄悄不对）。
	// 🔴 三条 beforePass 全部标了 mayBeAbsent（债-128：申报集不随图形态缩小——无光照的自定义图
	// 没有 drawLight "light"（内建 forward 图曾是其生产者，刀②.2 已删），blit 上屏的自定义图没有
	// tonemap），meshlet_cull 挂帧首段恒可解析（且
	// framePrologue + mayBeAbsent 被 build 拒）。容忍仍是**逐条显式**：每条自带理由与兜底
	// （见各条注释），新申报默认必须命中 ⟹「state 改名 = 装配错」的启动期护栏对所有未标
	// 项活着（单元红点 = tests 债-128 段 ④；扩到第四条的读数 → §7.21 M-d/M-e）。
	return {
		RgOffGraphWorkDecl{
			.name = std::string{kWorkMeshletCull},
			.anchor = RgOffGraphAnchor{.kind = RgOffGraphAnchor::Kind::framePrologue},
		},
		RgOffGraphWorkDecl{
			.name = std::string{kWorkIzbMask},
			// 债-128：自定义图可以没有 drawLight "light" ⟹ 锚点**可缺席**（= 本图不可调度）。
			// 「izb 在无 light pass 的图上被静默打开」不会发生：gate_lint 门名 `izb` 的启动期 error
			// 拦配置（配了却必然无效），izbPreFrame 的非 deferred 早退保证 izbShadow 恒空
			// = 门恒关 = 帧末对账不响。
			.anchor = RgOffGraphAnchor{
				.kind = RgOffGraphAnchor::Kind::beforePass,
				.passKind = RgPassKind::drawLight,
				.passState = std::string{kDeferredLightState},
				.mayBeAbsent = true,
			},
		},
		RgOffGraphWorkDecl{
			.name = std::string{kWorkEvssmMoments},
			// 债-128：同 izb_mask（gate_lint 门名 `mode` 的 evssm 规则 + evssmPreFrame 的
			// deferred 判据保证无 light pass 的图上 evssmShadow 恒空）。
			.anchor = RgOffGraphAnchor{
				.kind = RgOffGraphAnchor::Kind::beforePass,
				.passKind = RgPassKind::drawLight,
				.passState = std::string{kDeferredLightState},
				.mayBeAbsent = true,
			},
		},
		RgOffGraphWorkDecl{
			.name = std::string{kWorkFsrUpscale},
			// 债-128 第二刀：无 tonemap pass 的图（原 offscreen 内建图，2026-10-18 刀①已删，
			// 该形态现仅自定义 blit 上屏图可写）⟹ FSR 在该图本无落点。**拿什么换了什么**：
			// 换掉的 = 本条申报「锚点命中 0 = 启动期 throw」；换回的 = 该形态能起跑（带 tonemap
			// pass 的图——如原 forward 图形态，其本体已随刀②.2 删除——该形态下本条**仍可调度**，
			// 单测钉住）。残余风险（输入齐备却没有
			// tonemap pass）不静默，三条兜底：
			// ① kTonemapState 是编译期常量——状态名改名走编译，不是运行期静默改名；
			// ② computeFsrUsable（main_scene）启动期 warn 点名原因位（velocityDecorated /
			//    shader16 / 输入齐备），输入不齐 = 不构造 fsr = 门恒关（Renderer::initFsr
			//    是同一判据的另一半，不一致即抛）；
			// ③ 输入齐备**且**无 tonemap pass = 门开而无锚点 ⟹ 帧末对账每帧点名
			//    「门开着却整帧没录 = 静默旁路」——响亮的位置从启动期挪到帧末，归因不变。
			.anchor = RgOffGraphAnchor{
				.kind = RgOffGraphAnchor::Kind::beforePass,
				.passKind = RgPassKind::drawLight,
				.passState = std::string{kTonemapState},
				.mayBeAbsent = true,
			},
		},
	};
}

void Renderer::offGraphEnter(std::string_view name) {
	if (auto r = offGraph.enter(name); !r)
		reportOffGraphViolation(r.error());
}

// 门位规则（B2-5 起按名字问；每条都必须关于**门与输入**，不许写成「刚才 enter 过没有」
// ——后者让覆盖对账自证。名字常量与 offGraphWorkDecls 同源，不写字面量）。
std::optional<bool> Renderer::offGraphGateActive(
	std::string_view name, const FrameDrawList& /*list*/) {
	if (name == kWorkMeshletCull)
		return meshletCull != nullptr;
	if (name == kWorkIzbMask)
		return izbShadow != nullptr;
	if (name == kWorkEvssmMoments)
		return evssmShadow != nullptr;
	if (name == kWorkFsrUpscale)
		return fsr != nullptr && fsrInputs().has_value();
	// 在册却没有规则 = 申报表加了项而门位没跟上。返回 nullopt 让调用方出声：
	// 原先的 `std::array<bool, N>` 形态下这种漏法是**编译不过**（长度不符），改成
	// 按名求值后必须留一条运行期的等价护栏，否则「新增图外工作静默不参与对账」复活。
	return std::nullopt;
}

void Renderer::offGraphEndFrame(const FrameDrawList& list) {
	offGraph.advanceTo(offGraph.frameEndPhase());
	// 门位在**帧末**求值：IZB / EVSSM 是首帧 drawFrame 内懒构造（izbPreFrame / evssmPreFrame
	// 在 pass 循环之前），帧首求值会把它们误判成「不该发生」。
	// 位序 = 在册表的**在场投影**（与 OffGraphSchedule::build 的输入同一个表达式，
	// 两处各写一份就会漂 ⟹ checkCoverage 的「同序同长」要求靠同源而不是靠自觉）。
	std::array<bool, kOffGraphWorkCount> gate{};
	size_t gateCount = 0;
	bool missingGateRule = false;
	for (const auto& decl : offGraphWorks.activeDecls()) {
		if (const auto g = offGraphGateActive(decl.name, list); g)
			gate[gateCount++] = *g;
		else {
			missingGateRule = true;
			reportOffGraphViolation(Error{ErrorCode::RenderGraphInvalid, fmt::format(
				"off-graph contract: 在册申报 '{}' 没有门位规则（offGraphGateActive 少一条）"
				"——只挂号不加门位 = 这一项静默不参与覆盖对账，比不挂号更糟", decl.name)});
		}
	}
	if (!missingGateRule)
		if (auto r = offGraph.checkCoverage(
			    std::span<const bool>{gate.data(), gateCount}); !r)
			reportOffGraphViolation(r.error());
	// 批次 B1b：图外**同步边**的帧末对账（实发次数 vs 应发次数）。与上面那条独立出声——
	// 一个查「工作项有没有在挂点录」，一个查「barrier 有没有发够」，两件事的修复位置不同。
	const bool izbOn = izbShadow != nullptr;
	const bool cullOn = cullDriver.gpuPassRuns();
	const std::array<bool, kSyncGateCount> syncActive{
		evssmShadow != nullptr,
		izbOn,
		// 门之间不独立（dump ⊂ izb、mask 段 ⊂ izb）：不带上 `izbOn`，运行中途关掉 IZB 会让
		// 这两道门按**配置/场景**算成开、而 barrier 一条没发 ⟹ 假阳性（§7.5 第 1 条）。
		izbOn && izbDumpDue(),
		// mask pass 的门 = shadow0 段非空。**重算段跨度**而不是记「刚才调过 beginMaskPass 没有」：
		// 后者让这条断言自证（段真被跳过时它也"没调过"，两边一起消失 = 什么都测不到）。
		izbOn && !segmentSpan(shadowDrawSource(0), list).empty(),
		// cull 数据通路的门 = 本档真的录 + 提交 cull CB（`cpu` 档 = false ⟹ 一条边都不该发）。
		// 判据是**档**而不是「刚才录过没有」，理由同上一条。
		cullOn,
		// 扇出边挂**子门**（cullFanout ⊂ cull）：带父门的理由与 izbOn 那两条完全相同。
		cullOn && cullDriver.fanoutRuns(),
		// 逐簇剔除的门 = 服务在场 **且** 有剔除项。两个半边都是"门与输入"而不是"刚才录过没有"：
		// 前者是装配期事实（门关 = 恒 nullptr），后者是 uploadItems 的产物（空表 = recordDispatch
		// 第一行早退 = 零命令）。少掉后半边会让"注册了服务但一个剔除项都没有"的档帧末假阳性。
		// 🔴 四条边全挂这一道门，scene data 那条**不另挂子门**，理由见 RgSyncGate::meshlet 注释。
		meshletCull != nullptr && meshletCull->itemCount() > 0,
	};
	if (auto r = offGraphSync.checkEmission(std::span<const bool>{syncActive}); !r)
		reportOffGraphViolation(r.error());
}

std::optional<Renderer::FsrInputs> Renderer::fsrInputs() {
	// 两个图属输入按**编译图里的附件名**去 RT 缓存取（drawFrame 的 ensureOffscreenAttachments
	// 已在 pass 循环之前跑完 ⟹ 此处必已存在；取不到就是装配错，直接不录而不是崩）。
	// 不在调用点传 VkImage 的理由：那要把本文件私有的 SlotResolver 暴露进接口分区。
	const auto attImage = [this](int32_t slot) -> vk::Image {
		if (slot < 0)
			return {};
		auto* att = rtCache->find(renderGraph.attachments[static_cast<size_t>(slot)].name);
		return att ? static_cast<vk::Image>(att->image.image) : vk::Image{};
	};
	const FsrInputs inputs{attImage(fsrColorSlot), attImage(fsrVelocitySlot)};
	if (!inputs.color || !inputs.velocity)
		return {};
	return inputs;
}

}  // namespace RGL
