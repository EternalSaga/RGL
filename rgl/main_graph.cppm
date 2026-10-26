module;
#include <fmt/format.h>

// 入口模块 main 的分区实现单元（module main:graph;）——**渲染意图图这一族**：读
// configs/render_graph.lua → 按配置请求的能力装饰图 → compileRenderGraph 出可执行图。
//
// 【为什么从 main:impl 拆出来】main:impl 已顶在「单文件代码行 ≤ 500」上限（实测 499/500），
// 而它里面 `main()` 一个函数就占 120 行（上限也是 120）——**下一个启动步骤已经加不进去了**。
// 上限不是要绕的，是要找缝的。缝在这里：这一族回答的是"**这张渲染图长什么样**"（配置 →
// 装饰 → 编译），而 main:impl 剩下的部分回答的是"**进程怎么从零活起来**"（配置加载 → 建窗 →
// registry context → RAII 对象声明序 → 帧循环）。判据是**输入输出**：本单元的每个函数要么吃
// `RgGraphDesc&`（图描述本体）要么吐 `CompiledGraphResult`，**一次都不碰 registry、不碰设备、
// 不碰窗口**；main:impl 侧则反过来。缝在类型上，不是按行数切的。
//
// 🔴 本单元带着一条**启动期硬失败契约**（债-129 S3-C）：六个装饰失败出口（picking / highlight
//   在本单元的 compileGraphOrThrow 内，ssr / velocity / taa / ao_apply 在四个包装层里）全部是
//   「配置请求了该能力而本图接不了 ⟹ 抛 std::runtime_error 拒启动」，没有一处留 warn。理由与
//   逐条未命中判据 → decorationRefused 的注释。搬动这条链时**别把任何一格改回 warn**：
//   装饰器返回 false 的图 compileRenderGraph 照样编得过（能力没落图不是布局错误），warn +
//   继续跑 = 「配了能力却静默拿到出厂图」，而画面照出、smoke 也照绿。
//
// 🔴 装饰**次序**是判据而不是风格（详见 compileGraphOrThrow 体内注释）：shadow 无条件 →
//   ssr → velocity → taa → ao_apply。挪错序的形态是"图合法、布局合法、像素静默错"，
//   decorateSsrPass / decorateVelocityPass 的跳过条件把它从不可观测变成可观测失败（拒启动）。
//
// 可见性同姊妹分区：声明在 main.cppm（export），定义在本单元（不带 export）。
module main:graph;

import main;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import render_graph;
import render_graph_config;  // loadRenderGraphConfig（configs/render_graph.lua → RgGraphDesc）
import runtime_config;  // RuntimeConfig（四个图形态开关的唯一来源）
import scene_data;      // kActiveShadowCascades（阴影装饰日志的级联数读数）
import diagnostics;     // diagnostics().picking（object_id / highlight 装饰门）
import rllogger;

namespace RGL {

/** 渲染图编译器自检（失败项逐条日志；false = 启动期报错退出）。 */
bool checkRenderGraphSelfCheck() {
	auto failures = renderGraphSelfCheck();
	if (failures.empty())
		return true;
	auto* logger = RLLogger::getInstance();
	for (const auto& f : failures)
		if (logger) logger->error("RenderGraph self-check: {}", f);
	return false;
}

/** 渲染图配置加载 + compileRenderGraph（校验 + barrier 布局链推导，错误抛 std::runtime_error）+ deferredMode 求值。
 *  Phase 10 批 10.5：diagnostics.picking（默认关）= 编译前 decoratePickingPass 装饰
 *  object_id pass（D10 编译图期门控——默认关不调用 = 图逐位同 diff0）。
 *  🔴 债-129 S3-C：六个装饰失败出口（本函数内的 picking / highlight，与四个 decorateXxxOrThrow
 *  包装层里的 ssr / velocity / taa / ao_apply）全部改成「抛 std::runtime_error 拒启动」，
 *  与 compile 本身同一形态，没有一处留 warn。 */
/**
 * 「配置请求了该能力，而本图接不了它」的启动失败异常（债-129 S3-C：warn → **拒启动**）。
 *
 * 为什么必须抛而不能留 warn：装饰器返回 false 的图 `compileRenderGraph` **照样编得过**——能力没
 * 落图不是布局错误（实测：offscreen 变体 + aa="taa" 插不出 taa pass，图能编能画、TAA 零效果），
 * 所以 S3-A 那招「删 skip 让 compile 接手」在这一格拿不到收益。而 warn + 继续跑 = 「配了能力却
 * 静默拿到出厂图」= 本债的病本体（§项目哲学 4）。抛出与 compile 失败同一条路：main 的 catch 打
 * 「错误: {}」并返回 -1 ⟹ smoke 的「引擎非零退出必须 FAIL」直接吃到（工具侧不必再钉文案）。
 * 消息三段 = ①哪枚配置键 ②**具体哪条判据**没命中（判据函数名，不是"图形态不支持"）③怎么修。
 * @param capability 配置键/能力与请求值（如 `ssr.enabled = true`）
 * @param why 未命中的判据本体（装饰器里那条 `return false` 的判据名 + 它的语义）
 * @param fix 怎么改配置才起得来（换 variant / 关该能力）
 * @return 只为能被 `throw` 用（调用点 `throw decorationRefused(…)`，永不返回正常值）
 */
std::runtime_error decorationRefused(std::string_view capability, std::string_view why, std::string_view fix) {
	return std::runtime_error(fmt::format("{} 已请求，但本图接不了它 ⟹ 启动失败（不降级：静默跳过 = 本次运行没有"
		"该能力，而画面照出、compile 也不报错）。未命中的判据：{}。修法：{}。", capability, why, fix));
}

/**
 * 批 14.4 速度场图装饰（与 decorateTaaOrThrow 同构；抽函数控复杂度）。
 * 关（默认）= 不调 decorateVelocityPass = 图里根本没有这个 pass（**结构性**逐位不变，
 * 不是"有但旁路"）。🔴 债-129 S3-C：开而装饰器返回 false = 拒启动（理由见
 * `decorationRefused`）；装饰器本体保留 `bool` 契约（它是「落没落」的单一来源，S1b 已论证，
 * 且单测直接吃它），本包装层只把 false 换成失败。
 * 🔴 **债-129 S3：装饰器现在有两条判据**（① 次序 = `graphDecoratedTooLate` 命中，即 taa /
 * ao_apply 已在图里；② 锚点 = `swapchainColorConsumerIndex` 未命中），所以消息**必须按命中的
 * 那条选**——两条都报"锚点未命中"就是假消息，而 S3-C 的纪律是"消息点名哪条判据没命中"。
 * 选消息用的还是**同一个纯谓词**（对 `desc` 再求值一次，不是第二份判据），故本行净增 0 代码行。
 * @return 是否真正装饰落图（false 只可能是「没请求」）
 */
bool decorateVelocityOrThrow(RgGraphDesc& desc, bool velocityEnabled) {
	if (!velocityEnabled)
		return false;
	if (decorateVelocityPass(desc))
		return true;
	throw decorationRefused("velocity（aa=\"fsr\" 隐含打开）", graphDecoratedTooLate(desc) ? "次序判据 graphDecoratedTooLate 命中 = 图里已有 taa / ao_apply pass（**不是**锚点未命中）⟹ main_impl 的「velocity 先于 taa / ao_apply 装饰」调用序被挪错：本 pass 会夹在它们与 tonemap 之间、吃不到时域滤波，而 compile 照样过、画面静默错"
		: "swapchainColorConsumerIndex 未命中 = 图里没有 pass 同时「写 swapchain 附件」且「sampleRead 场景色附件」（速度场 pass 的落点）；自定义 blit 上屏图用 composite blit（transferRead ≠ sampleRead）必无落点",
		graphDecoratedTooLate(desc) ? "把 decorateVelocityOrThrow 的调用挪回 decorateTaaOrThrow / decorateAoApplyOrThrow 之前（本文件 compileGraphOrThrow 尾段；次序契约与判据本体 → render_graph.cppm 的 graphDecoratedTooLate）" : "删 render_graph.lua 的 attachments/passes 两键（回落内建 deferred 图），或 aa 改 \"off\"/\"taa\"（runtime.lua，速度场由 aa 派生）");
}

/**
 * 批 14.5 第三批：AO 施加专用 pass 的图装饰（抽函数控复杂度）。
 *
 * 门 = **FSR 门开**（`runtime.fsr_upscaler`）。为什么不是「FSR 真的接起来了」：FSR 的可用性
 * （shader16 / 三个输入附件）要到装配层才裁决完，而本装饰在图编译期。这个先后顺序**不影响
 * 正确性**——施加点标志取的是**实际图形态**（`aoApplyDecorated`），所以「本 pass 在图里但
 * FSR 最终没接」= AO 由本 pass 施加、tonemap 跳过，画面只是比 tonemap 路径多一次 RGBA16F
 * 量化，不会双施也不会零施。
 *
 * 装饰器自己还会查「taa 不在图里」（互斥）与「有采色上屏的消费者」（锚点）。🔴 债-129 S3-A：
 *  原第三条「本图有 ao 附件」**已删** —— 缺 ao 时照插、由 `compileRenderGraph` 硬拒（进程起不来）。
 * 🔴 债-129 S3-C：剩下两条真 skip 任一命中 = **拒启动**（旧形态是 **debug 级** + 继续跑——连 warn
 *  都不是，是七处出声点里最响的一记沉默：aa="fsr" 开着而 AO 施加点没搬进专用 pass，画面照出、
 *  只是与档位声明不符）。
 * @return 是否真正装饰落图（false 只可能是「没请求」）
 */
bool decorateAoApplyOrThrow(RgGraphDesc& desc, bool fsrEnabled) {
	if (!fsrEnabled)
		return false;
	if (decorateAoApplyPass(desc))
		return true;
	throw decorationRefused("aa = \"fsr\"", "decorateAoApplyPass 两条判据之一未过：① taa pass 已在图里 = 施加点已被它占（aa 三态互斥 ⟹ "
		"正常配置走不到 ①，走到就是 main_impl 的装饰顺序被挪错）② swapchainColorConsumerIndex 未命中 = 无 pass 同时「写 swapchain」且"
		"「sampleRead 场景色」（本 pass 要插在它前并把色输入换成 ao_applied）；自定义 blit 上屏图 blit 上屏故必然无落点",
		"删 render_graph.lua 的 attachments/passes 两键（回落内建 deferred 图），或 aa 改 \"off\"/\"taa\"");
}

/**
 * 批 12.6 TAA 图装饰（compileGraphOrThrow 尾段调用；抽函数控复杂度）。
 * 关（默认）= 不调 decorateTaaPass = 图逐位不变。
 * 🔴 债-129 S3-C：开而装饰器返回 false = **拒启动**（旧行为是 warn + 装配层据 false 整套不建 TAA
 *  RT（条目建了也无人执行）= 本次运行没有时域抗锯齿而画面逐位等于 aa = "off"）。装饰器本体保留
 *  bool 契约（单测直接吃它）。
 * @return 是否真正装饰落图（false 只可能是「没请求」）
 */
bool decorateTaaOrThrow(RgGraphDesc& desc, bool taaEnabled) {
	if (!taaEnabled)
		return false;
	if (decorateTaaPass(desc))
		return true;
	throw decorationRefused("aa = \"taa\"", "swapchainColorConsumerIndex 未命中 = 图里没有 pass 同时「写 swapchain 附件」且「sampleRead "
		"场景色附件」（= tonemap，taa pass 要插在它前并把它的色输入换成 taa_out）；自定义 blit 上屏图用 composite blit = 必无落点",
		"删 render_graph.lua 的 attachments/passes 两键（回落内建 deferred 图），或 aa 改 \"off\"/\"fsr\"");
}

/**
 * 批 15.1 前半：SSR 图装饰（与 decorateVelocityOrThrow 同构；抽函数控复杂度）。
 * 关（默认）= 不调 decorateSsrPass = 图里根本没有这个 pass（**结构性**逐位不变，
 * 不是"有但旁路"）。🔴 债-129 S3-A：原第四条「借不到可 store 的 depth」**已删** ——
 * 借不到时引用规范名照插、由 `compileRenderGraph` 硬拒（进程起不来），不再走本处的降级出声。
 * 🔴 债-129 S3-C：剩下三条真 skip 任一命中 = **拒启动**（旧行为是 warn + 装配层整套不建 = 本次
 *  运行没有屏幕空间反射）。三条里只有 ③（G-buffer 契约表）是 compile **放过**的形态（`emissive`
 *  不被任何 pass 引用 ⟹ 缺了也不报未知附件），①② 同样是「图合法而能力没落图」——三条都得拒。
 * @return 是否真正装饰落图（false 只可能是「没请求」）
 */
bool decorateSsrOrThrow(RgGraphDesc& desc, bool ssrEnabled) {
	if (!ssrEnabled)
		return false;
	if (decorateSsrPass(desc))
		return true;
	throw decorationRefused("ssr.enabled = true", "decorateSsrPass 三条判据之一未过：① taa / ao_apply 已在图里 = main_impl 的"
		"「SSR 先于它们装饰」调用序被挪错 ② swapchainColorConsumerIndex 未命中 = 无 pass 同时「写 swapchain」且「sampleRead 场景色」"
		"（自定义 blit 上屏图）③ G-buffer 契约表 kGBufferAttachments 通道不齐 = 这张图不是 deferred 图（SSR 是 deferred-only；"
		"当年 D1 对等豁免拍板 forward 不跟，docs/phase15_plan.md §0.1，forward 已随刀②.2 删除；含不被引用的 emissive ⟹ compile 不替你拒这条）", "删 render_graph.lua 的 attachments/passes 两键（回落内建 deferred 图），或 ssr.enabled = false");
}

/**
 * 加载 configs/render_graph.lua + `compileRenderGraph`（校验 + barrier 布局链推导）+
 * 按**图事实**求值 `deferredMode`（2026-10-18 刀②：原按配置层 variant，那枚枚举已删——
 * 装配路径消费图事实 `graphHasDeferredLight`，自定义图手写 light pass 也照样拿到装配）。
 *
 * 四个图形态开关全部由 `RuntimeConfig` 派生（`aaWantsTaa()` / `velocityField` / `aaWantsFsr()` /
 * `ssr.enabled`）——**单一真相源在 runtime_config**，调用点不逐项推导，省行也防漏传。
 *
 * 🔴 **失败即起不来**（债-129 S3-C，2026-10-17）：任何图错误**或任何「配置请求了本图接不了的能力」**
 * 都抛 `std::runtime_error`，由 `main` 的 catch 打 `错误: {}` 并返回 -1。后者取代了此前六个
 * 「warn + 继续启动」出口——静默跳过 = 本次运行没有该能力而画面照出，且 compile 不报错（图是**合法**
 * 的，只是没接线），正是本债的病。各出口的未命中判据见四个 `decorateXxxOrThrow` 的注释。
 *
 * @param path   render_graph.lua 的路径（调用点给 `configPath(...)`）
 * @param runtime 已解析的 runtime 配置（四个开关的唯一来源）
 * @return 编译图 + 各装饰是否落图（`CompiledGraphResult`；抛异常时调用方拿不到它）
 */
CompiledGraphResult compileGraphOrThrow(const char* path, const RuntimeConfig& runtime) {
	// 批 15.1 前半：四个图形态开关改由 RuntimeConfig 派生（= 原四形参的逐位同值来源，
	// 单一真相源在 runtime_config；main() 调用点不再逐项推导，省行也防漏传）。
	const bool taaEnabled = runtime.aaWantsTaa();
	const bool velocityEnabled = runtime.velocityField;
	const bool fsrEnabled = runtime.aaWantsFsr();
	const bool ssrEnabled = runtime.ssrEnabled;
	auto graphConfig = loadRenderGraphConfig(path);
	if (!graphConfig)
		throw std::runtime_error(graphConfig.error().describe());
	RgGraphDesc desc = std::move(*graphConfig);
	// 🔴 债-129 S3-C：装饰失败**不再出声了事**，改成抛 std::runtime_error 拒启动（与下面
	// compileRenderGraph 失败同一形态、同一层级——都在启动期、都不进帧循环）。理由：装饰器返回
	// false 的图 compile **照样编得过**（能力没落图不是布局错误），warn + 继续跑 = 配了能力却静默
	// 拿到出厂图 = 本债的病本体。消息必须点名「哪个配置键 / 哪条锚点判据没命中 / 怎么修」，
	// 让「为什么没插进去」不用读代码（旧 warn 已做到的那半保留）。两处同门 = diagnostics.picking：
	// object_id 的锚点内建图都有（缺它 = 自定义 passes 的事），高亮的锚点在 blit 上屏图必不命中
	// （blit 上屏 = 无 sampleRead 色消费者）——这两条事实从旧消息里挪到这里，消息本体保持短。
	if (diagnostics().picking && !decoratePickingPass(desc)) {
		throw decorationRefused("diagnostics.picking = true", "decoratePickingPass 锚点未命中 = 图里没有「kind = drawMesh 且 "
			"drawSource = opaque 且输出 depthWrite」的 pass（object_id pass 插在它后面、借它的 depth 附件测深）",
			"在 render_graph.lua 的 passes 里补一个 opaque + depth 的 draw pass，或 diagnostics.lua 的 picking = false");
	}
	// 批 10.7：高亮 pass 装饰（写既有 scene_hdr + depth，插图末；同门 = 默认关不调用）。
	// B2-5 起拆成 plan + apply 两步，且**计划无条件算**（纯函数、无副作用）：
	//  - 门开 → 落图，并把这张单子交给 Renderer 登记成「可撤回」；
	//  - 门关 → 不落图，但单子照样带出去当「从零插上它」的配方（RPC 打开关才有的可插）。
	// 为什么不能等运行期再算：撤回要还原的跨 pass 补丁**原值**只在「装饰落图之前」问得到
	// （见 RgStoreOpPatch::originalStoreOp），事后重算问到的已经是装饰后的值。
	CompiledGraphResult result;  // 只用来带 highlightPlan 出去（其余字段末尾聚合初始化）
	result.highlightPlan = planHighlightDecoration(desc);
	if (diagnostics().picking && !applyHighlightPlan(desc, result.highlightPlan)) {
		// 🔴 债-129 S3-C：这里原来还有一句 `result.highlightPlan = {};`（"没落图 = 没有可登记的
		// 东西"）—— 随 warn→throw 一起删：抛出去之后 result 整个作废，那句成了不可达的死码。
		throw decorationRefused("diagnostics.picking = true", "planHighlightDecoration 判不适用 = 图里没有「首个 sampleRead 场景色"
			"附件」的 pass（高亮要插在它之前、写同一张色附件叠加），或其前无 depth 写者可借；自定义 blit 上屏图 = 无色消费者",
			"删掉 render_graph.lua 的 attachments/passes 两键（回落内建 deferred 图），或 diagnostics.lua 的 picking = false");
	}
	// Phase 11 批 11.2：阴影 pass 装饰**无条件**（不再由 diagnostics 门控）。原因（实测驱动）：
	// 光照管线条目**无条件把阴影图的描述槽烘焙进去**，所以图里必须无条件地有这张图
	// 与其布局链（否则开关时描述绑定的图从未被任何 pass 转换布局 → 采样它就报 VVL）。
	// 门控改到渲染侧：关时阴影段**零 draw**（只清 4 MB 深度，近乎免费），图内全是
	// 1.0 = 无遮挡 → 采样结果恒为全亮 → 成片逐位不变。
	decorateShadowPass(desc, kShadowMapSize);
	if (auto* logger = RLLogger::getInstance())
		logger->info("shadow: 已装饰 depth-only 阴影 pass（{}² {}，{} 级；投影体清单由 "
			"shadow.lua 的 mode 门控（形态表 drawAtlas），mode = \"none\" 时段内零 draw = 画面逐位不变）",
			kShadowMapSize, kShadowMapFormat, kActiveShadowCascades);
	// 批 15.1 前半：SSR 装饰插在 shadow **之后**、velocity / taa / ao_apply **之前**。理由：
	//  ① SSR 读 scene_hdr（light pass 的产物），必须在其后——light 在图本体里，任何装饰都
	//     在它之后跑，shadow 之后自然满足；
	//  ② velocity / taa / ao_apply 三个装饰器都**动态**读「消费者当前的色附件」
	//     （sampledColorName）⟹ SSR 先插，它们自动读到 ssr_applied，链自己接成
	//     scene_hdr → ssr_applied → [taa_out | ao_applied] → tonemap。这是这些装饰器可
	//     复合的既有性质，不要破坏它。TAA 读到含 SSR 的成片 = 正确（时域滤波该滤最终画面）。
	// 批 15.1 后半：这条顺序不变量不再只由本注释承载——decorateSsrPass 跳过条件 1 在
	//   taa / ao_apply 已在图里时返回 false（= SSR 静默消失，smoke --ssr 双向核对可见），
	//   把「挪错顺序」从不可观测（SSR 静默建在时域滤波之后 = 图/布局/像素三层全无异常）
	//   变成可观测失败。挪错顺序的端到端兜底（aa=taa 档 SSR 开/关差分必须不同）→
	//   phase15_plan §3.2 缺口 1/2。
	// 默认关 = 不调用 = 图逐位不变（D10 纪律；门 = runtime.lua ssr.enabled，与 aa 正交）。
	const bool ssrDecorated = decorateSsrOrThrow(desc, ssrEnabled);
	// 批 12.6 TAA 装饰**必须最后**（在 picking / highlight / shadow 之后）：它把
	// 「写 swapchain 且采色」的 pass（tonemap）的输入换成时域结果，若 highlight 后插
	// 就会落在 taa 与 tonemap 之间而吃不到滤波。默认关 = 不装饰 = 图逐位不变（D10 纪律）。
	// 批 14.4 速度场装饰**先于** TAA（TAA 保持「必须最后」的既有纪律）。落点顺序
	// = [velocity, taa, tonemap]：两者都只 sampleRead depth，彼此无依赖，而 TAA 的锚点
	// 是**现搜**的（不缓存下标）⟹ velocity 先插不会让 TAA 认错消费者。
	const bool velocityDecorated = decorateVelocityOrThrow(desc, velocityEnabled);
	const bool taaDecorated = decorateTaaOrThrow(desc, taaEnabled);
	// 批 14.5 第三批：AO 施加 pass **必须在 taa 之后**装饰——它自己要先看「taa 在不在图里」
	// 决定跳过，而它本身是真正的成片消费者，先插会让前面的装饰认错锚点。
	const bool aoApplyDecorated = decorateAoApplyOrThrow(desc, fsrEnabled);
	auto compiledGraph = compileRenderGraph(desc);
	if (!compiledGraph)
		throw std::runtime_error(fmt::format("render graph compile failed: {}",
			compiledGraph.error().describe()));
	// deferredMode = **图事实**（刀②）：编译图里有没有人在跑 deferred 光照。装配层据此
	// 决定 G-buffer 槽 / light 条目 / HDR 链——消费图真值而非配置意图（旧 variant 双源
	// 及其对账 lint 已随 `docs/archive/图变体收敛_plan.md` §4 拆除）。
	const bool deferredMode = graphHasDeferredLight(compiledGraph->passes);
	return CompiledGraphResult{std::move(*compiledGraph), std::move(desc),
		deferredMode, taaDecorated, velocityDecorated, aoApplyDecorated, ssrDecorated,
		std::move(result.highlightPlan)};
}
}  // namespace RGL
