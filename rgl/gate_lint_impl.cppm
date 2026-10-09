module;
module gate_lint:impl;

import gate_lint;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import diagnostics;
import render_graph;
import render_graph_config;

namespace RGL {

namespace {

/** 谓词入参打包（表里每行一个非捕获 lambda → 函数指针）。 */
struct Ctx {
	const DiagnosticsConfig& cfg;
	const DiagnosticsConfig& defaults;
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

/** 偏离默认值判定缩写（40 行表里少写 60 处三元噪声）。 */
template <typename T>
constexpr bool deviated(const Ctx& c, const T DiagnosticsConfig::* field) {
	return c.cfg.*field != c.defaults.*field;
}

/**
 * 39 门 / 40 行申报表（与 DiagnosticsConfig 字段/回显表一一对应，覆盖率由
 * tests/test_gate_lint.cppm 钉死）。级别判据 = 计划 §4：error 只留给
 * 「必然完全无效且无其他信号」，拿不准一律 warn / inertNote。
 * **行数 ≠ 门数**：一门可以有多条规则（`izb_shadow` 两条 = 非 deferred 图 / shadow_pass 关，
 * 两个根因各自一条消息，合并会丢归因）；覆盖率护栏钉的是**去重后的门数** == 回显表键数。
 */
const std::array<GateRule, 40> kGateRules{{
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
	{"shadow_pass", GateSeverity::inertNote, "", nullptr},  // 本身是总开关
	{"rpc", GateSeverity::inertNote, "", nullptr},
	{"config_reload", GateSeverity::inertNote, "", nullptr},  // 前提缺失有消费者自己的 warn（GateEnv 注释）
	{"shader_reload", GateSeverity::inertNote, "", nullptr},  // 同上

	// ———— error：开了但必然完全无效，且无其他信号 ————
	{"dump_gbuf", GateSeverity::error,
		"dump_gbuf 需 runtime.lua llm_debug 开且图为 deferred（G-buffer 通道只在 deferred 截图路径存在）",
		[](const Ctx& c) { return c.cfg.dumpGbuf >= 0 && (!c.env.llmCapture || !c.deferredLight); }},
	{"izb_shadow", GateSeverity::error,
		"IZB 只接 deferred 光照 pass：非 deferred 图下 izbPreFrame 零命令（此前静默，EVSSM 同条件却喊——不对称收口）",
		[](const Ctx& c) { return c.cfg.izbShadow && !c.deferredLight; }},
	{"izb_shadow", GateSeverity::error,
		"IZB 的 mask 由 shadow0 段重放填充：shadow_pass=off 时段必空 ⟹ mask 无内容、IZB 一帧阴影也不产"
		"（mask 每帧清零 = 全不遮挡，债-32 收口；与 shadow_evssm 的 shadow_pass 判据同形）",
		[](const Ctx& c) { return c.cfg.izbShadow && !c.cfg.shadowPass; }},
	{"shadow_evssm", GateSeverity::error,
		"EVSSM 需 shadow_pass=on 且图为 deferred（EvssmShadow 不构造 = 门整体无效，消费点在 deferred light pass）",
		[](const Ctx& c) { return c.cfg.shadowEvssm && (!c.cfg.shadowPass || !c.deferredLight); }},
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
	{"shadow_pcss", GateSeverity::warn,
		"PCSS 让位：需 shadow_pass=on；与 shadow_evssm 同开 = EVSSM 优先（11.5 冻结件让位，见 evssm_plan）",
		[](const Ctx& c) { return c.cfg.shadowPcss && (!c.cfg.shadowPass || c.cfg.shadowEvssm); }},
	{"izb_dump_frame", GateSeverity::warn,
		"izb_shadow=off：izb_dump_frame 永不触发（回读的是从未创建的 IZB head）",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::izbDumpFrame) && !c.cfg.izbShadow; }},
	{"rpc_port", GateSeverity::warn,
		"rpc_port 偏离默认但 rpc=off：没有监听者，端口是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::rpcPort) && !c.cfg.rpc; }},

	// ———— inertNote：设计上合法的惰性（debug 级，不喊冤）————
	{"izb_grid", GateSeverity::inertNote,
		"izb_shadow=off：izb_grid 是死值（网格随 IZB 构造才存在）",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::izbGrid) && !c.cfg.izbShadow; }},
	{"pcss_sun_degrees", GateSeverity::inertNote,
		"软阴影门全关（shadow_pcss / shadow_evssm 均未开）：pcss_* 数值是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pcssSunDegrees)
			&& !c.cfg.shadowPcss && !c.cfg.shadowEvssm; }},
	{"pcss_search_radius", GateSeverity::inertNote,
		"软阴影门全关（shadow_pcss / shadow_evssm 均未开）：pcss_* 数值是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pcssSearchRadius)
			&& !c.cfg.shadowPcss && !c.cfg.shadowEvssm; }},
	{"pcss_max_penumbra", GateSeverity::inertNote,
		"软阴影门全关（shadow_pcss / shadow_evssm 均未开）：pcss_* 数值是死值",
		[](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pcssMaxPenumbra)
			&& !c.cfg.shadowPcss && !c.cfg.shadowEvssm; }},
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
}};

}  // namespace

/** gateLintRuleGates 定义（接口注释见 gate_lint.cppm）。 */
std::vector<std::string> gateLintRuleGates() {
	std::vector<std::string> gates;
	gates.reserve(kGateRules.size());
	for (const auto& r : kGateRules)
		gates.emplace_back(r.gate);
	return gates;
}

/** lintDiagnosticsGates 定义（接口注释见 gate_lint.cppm）。 */
std::vector<GateFinding> lintDiagnosticsGates(const DiagnosticsConfig& cfg,
	const DiagnosticsConfig& defaults, std::span<const RgPassPlan> passes,
	RgGraphVariant variant, const GateEnv& env) {
	const Ctx ctx{cfg, defaults, graphHasDeferredLight(passes), env};
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
