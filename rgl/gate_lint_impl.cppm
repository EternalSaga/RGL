module;
module gate_lint:impl;

import gate_lint;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import diagnostics;
import shadow_config;
import render_graph;
import render_graph_config;

namespace RGL {

namespace {

/** 谓词入参打包（表里每行一个非捕获 lambda → 函数指针）。 */
struct Ctx {
	const DiagnosticsConfig& cfg;
	const DiagnosticsConfig& defaults;
	const ShadowConfig& shadow;        ///< N6b：阴影门（configs/shadow.lua）与它自己的默认值
	const ShadowConfig& shadowDefaults;
	bool deferredLight;       ///< 图事实（graphHasDeferredLight，非 variant 意图）
	const GateEnv& env;
};

/**
 * 规则行：门名（回显表拼写）+ 触发级别 + 原因 + 谓词。
 * **触发约定**：bool 门 = true 才谈生效；其余门 = 偏离 defaults 才谈（`d(x)` 缩写）。
 * violated == nullptr = 显式「无生效前置」（加门时必须二选一：写谓词或写 nullptr + 理由）。
 */
struct GateRule {
	std::string_view gate;
	GateSeverity severity;
	std::string_view message;
	bool (*violated)(const Ctx&);
};

/** 偏离默认值判定缩写（45 行表里少写 60 处三元噪声）。 */
template <typename T>
constexpr bool deviated(const Ctx& c, const T DiagnosticsConfig::* field) {
	return c.cfg.*field != c.defaults.*field;
}

/** shadow.lua 侧同款（N6b）：指针所属类不同 ⟹ 重载可直接按实参分辨。 */
template <typename T>
constexpr bool deviated(const Ctx& c, const T ShadowConfig::* field) {
	return c.shadow.*field != c.shadowDefaults.*field;
}

/**
 * 调参块偏离默认判定（N6b 阴影侧）。`ShadowTuning` 没有 operator!= ⟹ 逐字段比；
 * 两个成员指针分开传（`&ShadowConfig::pcss` + `&ShadowTuning::sunDegrees`）而不是
 * 传"字段路径"抽象——后者要么引模板参数包要么引 lambda，都不比这行更好读。
 */
constexpr bool deviated(const Ctx& c, const ShadowTuning ShadowConfig::* block,
	const double ShadowTuning::* field) {
	return c.shadow.*block.*field != c.shadowDefaults.*block.*field;
}

/**
 * 43 行申报表 = diagnostics 侧 30 行 + shadow 侧 13 行（N6b）。与两份配置结构体 /
 * 两份回显表一一对应，覆盖率由 tests/test_gate_lint.cppm 钉死（去重后 43 门）。
 * 级别判据 = 计划 §4：error 只留给「必然完全无效且无其他信号」，拿不准一律 warn / inertNote。
 * **行数 ≠ 门数**：一门可以有多条规则（`izb` 两条 = 非 deferred 图 / mode="none"，
 * 两个根因各自一条消息，合并会丢归因）；覆盖率护栏钉的是**去重后的门数** == 两份回显表键数之和。
 *
 * 阴影侧的**枚举口径**（N6b 换轨要点）：旧表按"三个独立布尔怎么组合"写判据，于是长出
 * 「PCSS 让位 EVSSM」这类**只在旧门形下才存在**的规则。枚举下两门同开表达不出来 ⟹
 * 让位规则整条删除，剩下的判据一律写成 `mode == X` / `mode != X`。
 */
// ⚠ 原生数组（尺寸自动推导）——同 kShadowEcho：显式 N 写错会零填充出**空谓词**，
// 而本表**允许** nullptr 谓词（预留空块），所以连"空"都看不出来，只会静默少门。
const GateRule kGateRules[]{
	// ———— 无前置门（对着驱动/上传路径/日志本身工作，任何组合都生效）————
	{"no_vvl", GateSeverity::inertNote, "", nullptr},  // vkinstance 建 instance 时读
	{"no_hdr", GateSeverity::inertNote, "", nullptr},  // vulkan_window 选格式时读
	{"no_present", GateSeverity::inertNote, "", nullptr},
	{"vtx_canary", GateSeverity::inertNote, "", nullptr},  // 上传路径本身
	{"texel_canary", GateSeverity::inertNote, "", nullptr},  // 帧号可达性 = 运行期事实，启动判不了（计划 §2）
	{"style_dist", GateSeverity::inertNote, "", nullptr},
	{"tex_dist", GateSeverity::inertNote, "", nullptr},
	{"texture_force_size", GateSeverity::inertNote, "", nullptr},
	{"texture_force_white", GateSeverity::inertNote, "", nullptr},
	{"texture_force_unorm", GateSeverity::inertNote, "", nullptr},
	{"frame_stats", GateSeverity::inertNote, "", nullptr},
	{"profiler", GateSeverity::inertNote, "", nullptr},
	{"picking", GateSeverity::inertNote, "", nullptr},  // 自带图装饰，任何可装饰图都行
	{"rpc", GateSeverity::inertNote, "", nullptr},
	{"config_reload", GateSeverity::inertNote, "", nullptr},  // 前提缺失有消费者自己的 warn（GateEnv 注释）
	{"shader_reload", GateSeverity::inertNote, "", nullptr},  // 同上

	// ———— error：开了但必然完全无效，且无其他信号 ————
	{"dump_gbuf", GateSeverity::error,
		"dump_gbuf 需 runtime.lua llm_debug 开且图为 deferred（G-buffer 通道只在 deferred 截图路径存在）",
		[](const Ctx& c) { return c.cfg.dumpGbuf >= 0 && (!c.env.llmCapture || !c.deferredLight); }},
	{"izb", GateSeverity::error,
		"IZB 只接 deferred 光照 pass：非 deferred 图下 izbPreFrame 零命令（此前静默，EVSSM 同条件却喊——不对称收口）",
		[](const Ctx& c) { return c.shadow.izb && !c.deferredLight; }},
	{"izb", GateSeverity::error,
		"IZB 的 mask 由 shadow0 段重放填充：shadow.lua 的 mode = \"none\" 时段必空 ⟹ mask 无内容、"
		"IZB 一帧阴影也不产（mask 每帧清零 = 全不遮挡，债-32 收口；与 mode = \"evssm\" 的 deferred 判据同形）",
		[](const Ctx& c) { return c.shadow.izb && c.shadow.mode == ShadowMode::none; }},
	{"mode", GateSeverity::error,
		"mode = \"evssm\" 需图为 deferred（EvssmShadow 不构造 = 该档整体无效，消费点在 deferred light pass）",
		[](const Ctx& c) { return c.shadow.mode == ShadowMode::evssm && !c.deferredLight; }},
	{"picking_probe.frame", GateSeverity::error,
		"picking_probe 需 picking=on（object_id pass 由 picking 装饰进图，探针无的放矢）",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pickingProbeFrame) && !c.cfg.picking; }},
	{"ui_click_probe.frame", GateSeverity::error,
		"ui_click_probe 是窗口模式输入链探针：headless 下没有输入事件可合成",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::uiClickProbeFrame) && c.env.headless; }},
	{"camera_probe.frame", GateSeverity::error,
		"camera_probe 是窗口模式键盘探针：headless 下没有键盘事件可合成",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::cameraProbeFrame) && c.env.headless; }},

	// ———— warn：部分生效 / 让位 / 能力不符 ————
	{"dgc_static", GateSeverity::warn,
		"设备未 advertise staticGraphicsIndirect：dgc_static 无效（走 compute culling 读回重放）",
		[](const Ctx& c) { return c.cfg.dgcStatic && !c.env.staticGraphicsIndirect; }},
	{"cull_replay", GateSeverity::warn,
		"cull_replay 非 gpu_sync：opaque 段由 CPU 剔除驱动，dgc_static 的 execute 路根本不会被走到（要测 DGC 请同时设 cull_replay=\"gpu_sync\"，见 docs/cpu_perf_plan.md §4）",
		[](const Ctx& c) { return c.cfg.dgcStatic && c.cfg.cullReplay != CullReplayMode::gpuSync; }},
	{"imgui", GateSeverity::warn,
		"imgui 在 headless 下天然 no-op（无窗无输入，UI 段零调用）",
		[](const Ctx& c) { return c.cfg.imgui && c.env.headless; }},
	{"izb.dump_frame", GateSeverity::warn,
		"izb 关：izb.dump_frame 永不触发（回读的是从未创建的 IZB head）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::izbDumpFrame) && !c.shadow.izb; }},
	{"rpc_port", GateSeverity::warn,
		"rpc_port 偏离默认但 rpc=off：没有监听者，端口是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::rpcPort) && !c.cfg.rpc; }},

	// ———— inertNote：设计上合法的惰性（debug 级，不喊冤）————
	{"izb.grid", GateSeverity::inertNote,
		"izb 关：izb.grid 是死值（网格随 IZB 构造才存在）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::izbGrid) && !c.shadow.izb; }},
	{"pcss.sun_degrees", GateSeverity::inertNote,
		"mode != \"pcss\"：pcss.* 调参块没人读（按档分立，evssm 档读的是 evssm.*）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::sunDegrees)
			&& c.shadow.mode != ShadowMode::pcss; }},
	{"pcss.search_radius", GateSeverity::inertNote,
		"mode != \"pcss\"：pcss.* 调参块没人读（按档分立，evssm 档读的是 evssm.*）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::searchRadius)
			&& c.shadow.mode != ShadowMode::pcss; }},
	{"pcss.max_penumbra", GateSeverity::inertNote,
		"mode != \"pcss\"：pcss.* 调参块没人读（按档分立，evssm 档读的是 evssm.*）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::maxPenumbra)
			&& c.shadow.mode != ShadowMode::pcss; }},
	{"evssm.sun_degrees", GateSeverity::inertNote,
		"mode != \"evssm\"：evssm.* 调参块没人读（按档分立，pcss 档读的是 pcss.*）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::sunDegrees)
			&& c.shadow.mode != ShadowMode::evssm; }},
	{"evssm.search_radius", GateSeverity::inertNote,
		"mode != \"evssm\"：evssm.* 调参块没人读（按档分立，pcss 档读的是 pcss.*）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::searchRadius)
			&& c.shadow.mode != ShadowMode::evssm; }},
	{"evssm.max_penumbra", GateSeverity::inertNote,
		"mode != \"evssm\"：evssm.* 调参块没人读（按档分立，pcss 档读的是 pcss.*）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::maxPenumbra)
			&& c.shadow.mode != ShadowMode::evssm; }},
	{"evssm.min_footprint_texels", GateSeverity::inertNote,
		"mode != \"evssm\"：evssm.* 调参块没人读（按档分立，pcss 档读的是 pcss.*）",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::minFootprintTexels)
			&& c.shadow.mode != ShadowMode::evssm; }},
	{"pcss.min_footprint_texels", GateSeverity::warn,
		"滤波核宽下限（min_footprint_texels）是 **EVSSM 矩滤波专用件**：PCSS 的半径由 Δz 现算，"
		"没有「核窄到 0.5 纹素 ⟹ 抽点全落同一双线性邻域」这个失效 ⟹ 写在 pcss 块里"
		"**任何 mode 下都不生效**。要调核宽请写进 evssm 块",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::minFootprintTexels); }},
	{"csm", GateSeverity::inertNote, "", nullptr},  // 预留空块（CSM 档暂无调参键）
	{"mode", GateSeverity::inertNote, "", nullptr},  // 本身是软影滤波轴总开关
	{"picking_probe.x", GateSeverity::inertNote,
		"picking_probe.frame=0（探针关）：坐标是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pickingProbeX)
			&& c.cfg.pickingProbeFrame == 0; }},
	{"picking_probe.y", GateSeverity::inertNote,
		"picking_probe.frame=0（探针关）：坐标是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pickingProbeY)
			&& c.cfg.pickingProbeFrame == 0; }},
	{"ui_click_probe.x", GateSeverity::inertNote,
		"ui_click_probe.frame=0（探针关）：坐标是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::uiClickProbeX)
			&& c.cfg.uiClickProbeFrame == 0; }},
	{"ui_click_probe.y", GateSeverity::inertNote,
		"ui_click_probe.frame=0（探针关）：坐标是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::uiClickProbeY)
			&& c.cfg.uiClickProbeFrame == 0; }},
	{"camera_probe.key", GateSeverity::inertNote,
		"camera_probe.frame=0（探针关）：按键是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::cameraProbeKey)
			&& c.cfg.cameraProbeFrame == 0; }},
	{"camera_probe.holdFrames", GateSeverity::inertNote,
		"camera_probe.frame=0（探针关）：保持帧数是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::cameraProbeHold)
			&& c.cfg.cameraProbeFrame == 0; }},
};

}  // namespace

/** gateLintRuleGates 定义（接口注释见 gate_lint.cppm）。 */
std::vector<std::string> gateLintRuleGates() {
	std::vector<std::string> gates;
	gates.reserve(std::size(kGateRules));
	for (const auto& r : kGateRules)
		gates.emplace_back(r.gate);
	return gates;
}

/** lintDiagnosticsGates 定义（接口注释见 gate_lint.cppm）。 */
std::vector<GateFinding> lintDiagnosticsGates(const DiagnosticsConfig& cfg,
	const DiagnosticsConfig& defaults, const ShadowConfig& shadow,
	const ShadowConfig& shadowDefaults, std::span<const RgPassPlan> passes,
	RgGraphVariant variant, const GateEnv& env) {
	const Ctx ctx{cfg, defaults, shadow, shadowDefaults, graphHasDeferredLight(passes), env};
	std::vector<GateFinding> findings;
	for (const auto& r : kGateRules)
		if (r.violated && r.violated(ctx))
			findings.push_back({r.gate, r.severity, std::string{r.message}});

	// variant 交叉校验（计划 §3-A：配置意图与图事实两个真相源的**分歧在此喊**，
	// 不靠谁吞掉谁——自定义 Lua 图在 forward 变体下手写 light pass 是合法定制，
	// 但下游（IZB/EVSSM/dump_gbuf）按图事实走，这个错位值得喊一声）
	if (variant == RgGraphVariant::deferred && !ctx.deferredLight)
		findings.push_back({"variant", GateSeverity::error,
			"variant=deferred 但编译图没有 drawLight+light pass（内建 deferred 图被改坏？"
			"deferred 装配路径会因缺管线条目在 Renderer 构造处 throw）"});
	else if (variant != RgGraphVariant::deferred && ctx.deferredLight)
		findings.push_back({"variant", GateSeverity::warn,
			"图里有 deferred 光照 pass 但 variant 不是 deferred（自定义 passes 手写 light？"
			"IZB/EVSSM/dump_gbuf 按图事实生效，与 variant 意图不一致）"});
	return findings;
}

/** logGateFindings 定义（接口注释见 gate_lint.cppm；lint 不阻止启动——计划 §2 非目标）。 */
uint32_t logGateFindings(std::span<const GateFinding> findings) {
	auto* logger = RLLogger::getInstance();
	uint32_t errors = 0;
	for (const auto& f : findings) {
		switch (f.severity) {
		case GateSeverity::error:
			++errors;
			if (logger)
				logger->error("gate_lint[{}]: {}", f.gate, f.message);
			break;
		case GateSeverity::warn:
			if (logger)
				logger->warn("gate_lint[{}]: {}", f.gate, f.message);
			break;
		case GateSeverity::inertNote:
			if (logger)
				logger->debug("gate_lint[{}]: {}", f.gate, f.message);
			break;
		}
	}
	return errors;
}

}  // namespace RGL
