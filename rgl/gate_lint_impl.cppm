module;
module gate_lint:impl;

import gate_lint;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import diagnostics;
import shadow_config;
import runtime_config;  // 债-106：meshlet 块十枚键的申报规则（RuntimeConfig 成员指针取字段）
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
	const RuntimeConfig& runtime;      ///< 债-106：runtime.lua 的 meshlet 块与它自己的默认值
	const RuntimeConfig& runtimeDefaults;
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

/** 块内单枚键：只回答「本键配了吗」，宿主条件由块给。 */
struct BlockKey {
	std::string_view gate;            ///< 回显表拼写，如 "meshlet.mode"
	bool (*present)(const Ctx&);      ///< 本键偏离默认 / 为真
	std::string_view message{};       ///< 空 = 用块级消息；非空 = 本键自己的归因
};

/**
 * 块级宿主声明：块内所有键共享同一条「宿主不成立 ⟹ 本键无效」。
 * 「子键配了但宿主不成立」这条关系本就住在配置结构体里（`ShadowTuning pcss{}` 的成员身份、
 * `meshlet.*` 前缀 + `meshletAssets`），旧表把它逐键手抄（债-129 第二刀 D 的塌缩对象）。
 * 🔴 块只管 owner：额外根因（跨档前置 / 兄弟键 / 结构性错位）留独立 `GateRule` 行，
 * 且那些行里必须**删掉 owner 项**——同一个根因只准有一处出声（不双报）。
 */
struct GateBlock {
	std::string_view prefix;              ///< "meshlet." / "pcss." / …
	std::string_view owner;               ///< 宿主门名（进消息、进覆盖率对账）
	bool (*ownerActive)(const Ctx&);      ///< 宿主成立？
	GateSeverity severity;                ///< 块内默认级别
	std::string_view message;             ///< 块级**缺省**原因；有逐键增量的键在 BlockKey::message 覆盖
	std::span<const BlockKey> keys;
};

/** 偏离默认值判定缩写（省掉表里成片的 `x != defaults.x ? … : …` 三元噪声）。 */
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
 * runtime.lua 侧同款（债-106，meshlet 块）：指针所属类不同 ⟹ 重载按实参分辨。
 * 两枚枚举字段（MeshletCoplanarMode / MeshletLodFallbackMode）是 scoped enum，
 * `!=` 是内建比较，无需像 `ShadowTuning` 那样逐字段写（那是聚合体没有 operator!=）。
 */
template <typename T>
constexpr bool deviated(const Ctx& c, const T RuntimeConfig::* field) {
	return c.runtime.*field != c.runtimeDefaults.*field;
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

// ———— meshlet 逐键消息常量（债-129 第二刀 D 收尾修正）————
// 「残留独立行 ∪ 块条目」共享的五份归因文本。块级消息只是**缺省**，逐键语义不是重复项：
// 残留行报「owner 成立、本键另有根因」，块条目报「owner 不成立」——两个发射面同一条
// 债-106 判据，措辞必须逐字一致，所以住同一个常量（各抄一份必漂；消息级金标准
// tests/golden/gate_lint_findings_msg.txt 逐字钉死，改措辞 = 金标准红）。

/** instrument 的落空机制是两条独立路（overlay 关 / assets 关），消息按并集写 = 债-106 原文
 *  骨架（债-129 S2：原 kMsgMeshletDebug/kMsgMeshletTint 两条随两布尔合并为这一条；
 *  原「tint 与 debug 同真 ⟹ 让位」的第三条规则**整条删除**——三值键下该形态写不出来）。 */
constexpr std::string_view kMsgMeshletInstrument{
	"instrument 是叠加段的**条目级** SPV 变体位（三值 none/offset/tint，债-129 S2 由原 debug/tint "
	"两布尔合并），两条独立机制都会让它落空（判据按消费者写，不按本表口径写）："
	"① mode ≠ \"overlay\" ⟹ `pickMeshletMeshVariant` 直接返回 base（它**只看 overlay 档**，不看 assets）；"
	"② assets = false ⟹ `bakeMeshletEntries` 吃 `slots = nullopt` 整段早退 = **一条条目都不建**，"
	"变体位没有条目可烘。变体烘进的是原位取代共用的那批条目（让叠加仪器改条目 = 诊断渗进产品路）"
	"⟹ 要么置 meshlet.mode = \"overlay\" 让仪器有消费者，要么把它归 \"none\""
	"（原就近 warn = warnMeshletVariantInstruments，债-106 迁入）"};

/** coplanar 的两个失效面（assets 关 / mode 不是 overlay）写在一条消息里 = 债-106 原文骨架
 *  （债-129 S2：overlay 布尔并入 mode 三值后，原「第三条死路（switch 生效清空叠加段）」
 *  升级为**配置可判**——取代档下叠加段本就不存在，谓词按 mode ≠ "overlay" 直接吃进）。 */
constexpr std::string_view kMsgMeshletCoplanar{
	"coplanar 双臂只作用**叠加段**那批 draw：assets = false（无条目）或 mode ≠ \"overlay\"（零采集，"
	"含 \"switch\" 档——该档下叠加段不存在，不是被清空）⟹ 没有作用对象。生效档 INFO 在 "
	"wireMeshletOverlayDepthCompare（同因不同层：那条是「setter 真的设了」的生效证据）。"
	"⚠ 取代档自身的三条回落路（设备规格 / cull 档 / 对账）是装配期事实，判不了——但它们只影响"
	"「取代切没切」，不影响本判据（叠加段在 mode ≠ \"overlay\" 下恒零采集）"};

/** cull 的真实宿主 = assets && mode="switch"（比 meshlet 块的 owner 更强），块消息表达不出来 = 债-106 原文。 */
constexpr std::string_view kMsgMeshletCull{
	"cull 只作用于**已被簇路径接管**的 draw（旧 mesh 管线读不到标志数组）⟹ 需 assets && "
	"mode = \"switch\"，单独开 = 无消费者。装配层 wireMeshletCull 还按「取代档是否生效」的事实出声"
	"（同因不同层：取代档的回落路不写配置）"};

/** mdi_shadow 的宿主 = assets + shadow_lod ≥ 0（两脚），消息写全等式 = 债-106 原文。 */
constexpr std::string_view kMsgMeshletMdiShadow{
	"mdi_shadow 的宿主（★M-1b：行表载体 + 簇 buffer 槽）的成立条件 = meshlet.assets + shadow_lod ≥ 0"
	"（wireMeshletShadowArgs 第①步）⟹ 本组合下条目必然烘不出来。装配层 wireMeshletMdiShadowActivation "
	"还按「载体在不在」的**事实**出声（同因不同层：选级表全零 / 无产物回落只有装配期看得见）"};

/**
 * 独立申报行（按来源分三段：diagnostics / shadow / runtime.lua **meshlet 块**）。
 * 🔴 行数与门数不在本注释抄写（抄一次漂一次）——覆盖率护栏 = `tests/test_gate_lint.cppm`，
 * 它钉「本表行 ∪ 块条目 == 三方来源键表」+ 逐表计数，本表动而护栏不红 = 判据失效。
 * 债-106 整块挂号不挑键；债-129 第二刀 D 后「宿主 = assets」的键塌进 kGateBlocks，
 * 本表只留跨档前置 / 链式前置 / 结构性错位的行；债-129 S2 把 meshlet.debug/tint 三条
 * ——两条残留 + 一条让位——合并成 meshlet.instrument 一条，meshlet.overlay + meshlet.switch
 * 合并成 meshlet.mode 一条——其中**互斥申报行整删**，「同真」态在三值键下写不出来。
 * 与三份配置结构体 / 两份回显表 + 一份键表一一对应。
 * 级别判据 = 计划 §4：error 只留给「必然完全无效且无其他信号」，拿不准一律 warn / inertNote。
 * **行数 ≠ 门数**：一门可以有多条规则（`izb` 两条 = 非 deferred 图 / mode="none"，
 * 两个根因各自一条消息，合并会丢归因）；覆盖率护栏钉的是**去重后的门数** == 三方来源键数之和。
 *
 * 阴影侧的**枚举口径**（N6b 换轨要点）：旧表按"三个独立布尔怎么组合"写判据，于是长出
 * 「PCSS 让位 EVSSM」这类**只在旧门形下才存在**的规则。枚举下两门同开表达不出来 ⟹
 * 让位规则整条删除，剩下的判据一律写成 `mode == X` / `mode != X`。
 *
 * meshlet 侧（债-106）判据出处 = `runtime_config.cppm` 的逐字段注释 + 消费者代码
 * （`wireMeshlet*` / `wireClusterRangeTable`）。**本块不用 error**：§4 的 error 要求
 * 「没有其他任何信号」，而本块多数键在装配层另有按**运行期事实**（条目数 / 载体在不在 /
 * 产物存在性）出声的 warn——lint 只接管其中纯配置的那部分，拿不准一律 warn / inertNote。
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
	{"izb.on", GateSeverity::error,
		"IZB 只接 deferred 光照 pass：非 deferred 图下 izbPreFrame 零命令（此前静默，EVSSM 同条件却喊——不对称收口）",
		[](const Ctx& c) { return c.shadow.izb && !c.deferredLight; }},
	{"izb.on", GateSeverity::error,
		"IZB 的 mask 由 shadow0 段重放填充：shadow.lua 的 mode = \"none\" 时段必空 ⟹ mask 无内容、"
		"IZB 一帧阴影也不产（mask 每帧清零 = 全不遮挡，债-32 收口；与 mode = \"evssm\" 的 deferred 判据同形）",
		[](const Ctx& c) { return c.shadow.izb && c.shadow.mode == ShadowMode::none; }},
	{"izb.on", GateSeverity::warn,
		"IZB 是**近场 v1 spike**，交付默认已关（2026-10-10 换红线）。两个结构性缺陷："
		"① 只覆盖 cascade-0 光空间盒、盒外回落 CSM ⟹ 盒边界是一条**随相机在世界表面爬行的接缝**"
		"（默认机位实测 1.76% 屏幕判反、亮度差中位 16 LSB / 最高 160 LSB）；"
		"② 管辖范围内系统性欠遮挡（`just imgqa izb` 的「CSM 遮而 IZB 放」= 405048 px，rig 占屏 44%）。"
		"关掉是测出来的：CSM 单独一档 `just imgqa swim` 影场世界不变（0/63835）。"
		"读数与复现配方 → docs/shadow_routes_plan.md §7",
		[](const Ctx& c) { return c.shadow.izb; }},
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
	// pcss.* 三键与 evssm.* 四键的「mode != 本档 ⟹ 调参块没人读」塌进 kGateBlocks 的
	// pcss / evssm 两块（宿主 = 选张器 mode）。
	// 🔴 pcss.min_footprint_texels 是**全表唯一一条结构性错位键**判据：它**不含任何 owner 条件**
	// ——判的是「这枚键写在错的块里、任何 mode 下都不生效」，与「宿主不成立」无关，
	// 不许被块吞掉（吞了 = 丢掉这条判据）。
	{"pcss.min_footprint_texels", GateSeverity::warn,
		"滤波核宽下限（min_footprint_texels）是 **EVSSM 矩滤波专用件**：PCSS 的半径由 Δz 现算，"
		"没有「核窄到 0.5 纹素 ⟹ 抽点全落同一双线性邻域」这个失效 ⟹ 写在 pcss 块里"
		"**任何 mode 下都不生效**。要调核宽请写进 evssm 块",
		[](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::minFootprintTexels); }},
	{"csm", GateSeverity::inertNote, "", nullptr},  // 预留空块（CSM 档暂无调参键）
	{"mode", GateSeverity::inertNote, "", nullptr},  // 本身是软影滤波轴总开关
	// 探针子键（picking_probe.x/y、ui_click_probe.x/y、camera_probe.key/holdFrames）的
	// 「frame=0（宿主）⟹ 子键是死值」塌进 kGateBlocks 的三个探针块（宿主 = 各自 frame 键）。

	// ———— runtime.lua 的 meshlet 块（债-106：整块 8 枚键挂号，「子键开了但资源门关」从装配层
	//      就近告警升级成启动期申报式护栏；债-129 第二刀 D：块宿主 = assets 的 6 枚键塌进
	//      kGateBlocks 的 meshlet 块，本表只留**非 owner 根因**的残留行——owner 项已按
	//      「同一根因只准一处出声」从残留行里删掉；🔴 assets 是**资源门**不是 8 键总门（生效前置
	//      不含它的只有 mode 的 "overlay" 档，逐键消息见 kMeshletKeys）；
	//      判据出处 = 字段注释 + wireMeshlet*）————
	{"meshlet.assets", GateSeverity::inertNote, "", nullptr},  // 资源门本身：无生效前置（= meshlet 块的宿主，进覆盖率对账；不是总门——mode="overlay" 档的生效前置不含它）
	{"meshlet.instrument", GateSeverity::warn, kMsgMeshletInstrument,
		// 残留行只报 ①（mode 不是 overlay）；②（assets 关）由 meshlet 块出声——本行**谓词**已删掉该
		// owner 项（不双报），但**消息**保持并集原文（金标准钉文本，逐键归因不并入块消息）。
		// 债-129 S2：原 meshlet.debug / meshlet.tint 两条残留行 + 一条让位行（tint 与 debug
		// 同真 ⟹ tint 优先）随两布尔合并为这一条——让位断的形态在三值键下**不可表示**，整删
		[](const Ctx& c) { return deviated(c, &RuntimeConfig::meshletInstrument)
			&& c.runtime.meshletAssets && !c.runtime.modeIsOverlay(); }},
	{"meshlet.coplanar", GateSeverity::warn, kMsgMeshletCoplanar,
		// 残留行只报 mode 不是 overlay 那条；assets 关（无条目）由 meshlet 块出声（谓词删 owner、
		// 消息留债-106 并集原文，同 meshlet.instrument）
		[](const Ctx& c) { return deviated(c, &RuntimeConfig::meshletCoplanar)
			&& c.runtime.meshletAssets && !c.runtime.modeIsOverlay(); }},
	{"meshlet.mode", GateSeverity::warn,
		"mode = \"switch\" 档只在 diagnostics.cull_replay = \"cpu\" 生效：非默认档的 opaque 段由 cullDriver "
		"按候选表重放（DGC 的 push 与 task 数按旧 mesh 管线烘），段内换管线不在那条路上。装配层 "
		"wireMeshletSwitch 真走到那一档被 cull 档挡下时也 warn（同因不同层：它排在设备规格闸门之后）",
		// 债-129 S2：原 meshlet.switch 两条行 → 本条（cull 档前置）+ **互斥行整删**——一枚三值键
		// 写不出 overlay/switch 同真，互斥不再由运行期裁决，那条申报行连同装配层互斥 warn
		// （wireMeshletSwitch 的「switch 生效 ⟹ 叠加段清空」）一起结构性消失。消灭的是**实测可达
		// 态**：同开截图与 switch 单开逐位同（dc846396…）而 warnings 1→2，增量就是互斥告警
		// （/tmp/opencode/o3b_premerge_anchors.txt，合并后此实验永远做不了 = 旧写法配不出来）。
		[](const Ctx& c) { return c.runtime.modeIsSwitch() && c.runtime.meshletAssets
			&& c.cfg.cullReplay != CullReplayMode::cpu; }},
	{"meshlet.cull", GateSeverity::warn, kMsgMeshletCull,
		// 残留行只报取代档关那条；assets 关由 meshlet 块出声（原判据 assets && mode="switch" 的
		// 谓词已删 owner 项；消息留原文「需 assets && mode = "switch"」——cull 的真实宿主比块 owner
		// 强，块消息表达不出这条逐键语义，金标准钉文本）
		[](const Ctx& c) { return c.runtime.meshletCull
			&& c.runtime.meshletAssets && !c.runtime.modeIsSwitch(); }},
	{"meshlet.cull", GateSeverity::warn,
		"cull 与取代档同款限制：只在 diagnostics.cull_replay = \"cpu\" 生效（它挂在取代档上，"
		"非默认档下取代本身就不生效）",
		[](const Ctx& c) { return c.runtime.meshletCull && c.runtime.meshletAssets
			&& c.runtime.modeIsSwitch() && c.cfg.cullReplay != CullReplayMode::cpu; }},
	{"meshlet.shadow_lod_fallback", GateSeverity::inertNote,
		"shadow_lod_fallback 仅当 shadow_lod ≥ 0 且 assets 开时才有消费者（逐模型选级住在 "
		"wireClusterRangeTable）—— 本组合下是死值（调参残留，同 izb.grid 形态）",
		// 链式前置（lod < 0 || !assets）整体留本行：级别 = inertNote ≠ meshlet 块的 warn，塞不进块
		[](const Ctx& c) { return deviated(c, &RuntimeConfig::meshletShadowLodFallback)
			&& (c.runtime.meshletShadowLod < 0 || !c.runtime.meshletAssets); }},
	{"meshlet.mdi_shadow", GateSeverity::warn, kMsgMeshletMdiShadow,
		// 残留行只报 shadow_lod < 0 那条；assets 关由 meshlet 块出声（谓词已删 owner 项；
		// 消息留原文等式「= meshlet.assets + shadow_lod ≥ 0」——宿主两脚写全才不误导读者）
		[](const Ctx& c) { return c.runtime.meshletMdiShadow
			&& c.runtime.meshletAssets && c.runtime.meshletShadowLod < 0; }},
};

// ———— 块内键表（kGateBlocks 的 keys 指这里；独立出来是 std::span 不吃花括号列表的寿命要求）————

const BlockKey kPcssKeys[]{
	{"pcss.sun_degrees", [](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::sunDegrees); }},
	{"pcss.search_radius", [](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::searchRadius); }},
	{"pcss.max_penumbra", [](const Ctx& c) { return deviated(c, &ShadowConfig::pcss, &ShadowTuning::maxPenumbra); }},
};

const BlockKey kEvssmKeys[]{
	{"evssm.sun_degrees", [](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::sunDegrees); }},
	{"evssm.search_radius", [](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::searchRadius); }},
	{"evssm.max_penumbra", [](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::maxPenumbra); }},
	{"evssm.min_footprint_texels", [](const Ctx& c) { return deviated(c, &ShadowConfig::evssm, &ShadowTuning::minFootprintTexels); }},
};

const BlockKey kPickingProbeKeys[]{
	{"picking_probe.x", [](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pickingProbeX); }},
	{"picking_probe.y", [](const Ctx& c) { return deviated(c, &DiagnosticsConfig::pickingProbeY); }},
};

const BlockKey kUiClickProbeKeys[]{
	{"ui_click_probe.x", [](const Ctx& c) { return deviated(c, &DiagnosticsConfig::uiClickProbeX); }},
	{"ui_click_probe.y", [](const Ctx& c) { return deviated(c, &DiagnosticsConfig::uiClickProbeY); }},
};

// 两枚键各带逐键消息（消息级金标准钉死）：块消息「子键是死值」是并写，旧归因分别点名
// 「按键」/「保持帧数」——读者按哪个键喊就该看见哪个键名。
const BlockKey kCameraProbeKeys[]{
	{"camera_probe.key", [](const Ctx& c) { return deviated(c, &DiagnosticsConfig::cameraProbeKey); },
		"camera_probe.frame=0（探针关）：按键是死值"},
	{"camera_probe.holdFrames", [](const Ctx& c) { return deviated(c, &DiagnosticsConfig::cameraProbeHold); },
		"camera_probe.frame=0（探针关）：保持帧数是死值"},
};

/**
 * meshlet 块 6 枚键（宿主 = meshlet.assets）。present 口径 = 「本键配了」：
 * 布尔键 = 为真；coplanar / instrument = **偏离默认**（off / none；🔴 不是「偏离出厂档」
 * 的歧义——两枚都是三态字符串键，deviated 即「配了仪器/双臂」）；shadow_lod = **≥ 0**
 * （🔴 不是「偏离默认」——门关档 -1 也偏离默认 5，写成 deviated 会打破「门关 grep meshlet = 0」
 * 护栏，tests 红线 A 用例钉住这条）。
 * 🔴 6 枚键**全部**自带消息（块消息只是新键缺省）：逐键归因不是重复项——cull 的「真实宿主 =
 * assets && mode="switch"（比块 owner 强）」这类微妙区别块消息表达不出来，塌成一条 =
 * 抹掉拍板 A 要消费的语义。与残留行共享的文本走 kMsgMeshlet* 常量（单一来源）。
 * 🔴 债-129 S2 按档分立（拍板 D 约束②「owner 不只有布尔」③「块吞不得」的落地处）：
 * overlay + switch 两布尔合并成一枚 mode 后，**两条不同的生效前置必须按档各自成立**——
 * 本表的 `meshlet.mode` 条目 present **只吃 switch 档**（它需 assets：簇条目没烘 ⟹ 无 draw
 * 可原位取代）。`mode = "overlay"` 档**不进本表**：它的生效条件**不含** assets（宿主是叠加段
 * 自己），把它申报进 owner = assets 的块门下 = **假申报**（比没申报更坏——那会让
 * `mode = "overlay"` × `assets = false` 这个合法组合被判成死值）。该组合的实际落空
 * （slots = nullopt ⟹ 没有条目可采、零 draw）由装配层 `wireMeshletOverlayActivation` 按
 * 「有没有条目可采」的**事实**出声（无产物 / 多产物回落 / 零模型命中只有装配期看得见），
 * 不静默空开关——原 meshlet.overlay 块条目的谓词（present = overlay 布尔 ⟺ 在 !assets 时
 * 发射）就是在把 overlay 吞进 assets 门下，随合并删除。
 */
const BlockKey kMeshletKeys[]{
	{"meshlet.mode", [](const Ctx& c) { return c.runtime.modeIsSwitch(); },
		"mode = \"switch\" 需 assets：簇条目根本没烘（makeOpaqueCall 读 asset.meshletPipelineIndex）"
		"⟹ 无 draw 可原位取代。⚠ mode = \"overlay\" 档**不**在本块申报——它的生效条件不含 assets"
		"（宿主是叠加段自己），零条目可采的落空事实归装配层 wireMeshletOverlayActivation 出声"},
	{"meshlet.instrument", [](const Ctx& c) { return deviated(c, &RuntimeConfig::meshletInstrument); },
		kMsgMeshletInstrument},
	{"meshlet.coplanar", [](const Ctx& c) { return deviated(c, &RuntimeConfig::meshletCoplanar); },
		kMsgMeshletCoplanar},
	{"meshlet.cull", [](const Ctx& c) { return c.runtime.meshletCull; }, kMsgMeshletCull},
	{"meshlet.shadow_lod", [](const Ctx& c) { return c.runtime.meshletShadowLod >= 0; },
		"shadow_lod ≥ 0 但 assets = false：簇四段 buffer 的描述符槽由 wireMeshletBindings 提供（宿主 = "
		"assets）⟹ 槽没人给，阴影段 LOD 整体无效（债-106 把 ★18-C 的就近 WARNING 迁到此处）。"
		"门关档 shadow_lod = -1 不满足「≥ 0」⟹ 不喊 = 「门关 grep meshlet = 0」护栏保留"},
	{"meshlet.mdi_shadow", [](const Ctx& c) { return c.runtime.meshletMdiShadow; }, kMsgMeshletMdiShadow},
};

// ⚠ 同 kGateRules：原生数组——显式 N 写错会零填充出**空 keys**（块静默少键）。
/**
 * 块级宿主声明表（债-129 第二刀 D）。发射 = 对 `present && !ownerActive` 的每枚键出一条
 * finding（门名 = 键名、级别 = 块级、消息 = 键级覆盖优先缺省落块级）。与独立行的分工见 GateBlock 注释：
 * 块只报「宿主不成立」，额外根因在 kGateRules 的残留行里（owner 项已删，不双报）。
 * 宿主不只有布尔：pcss / evssm 的宿主是**选张器** `mode == X`（ownerActive 是 lambda，
 * 这正是它不收 bool 成员指针的原因）。
 */
const GateBlock kGateBlocks[]{
	{"pcss.", "mode", [](const Ctx& c) { return c.shadow.mode == ShadowMode::pcss; },
		GateSeverity::inertNote,
		"mode != \"pcss\"：pcss.* 调参块没人读（按档分立，evssm 档读的是 evssm.*）",
		std::span{kPcssKeys}},
	{"evssm.", "mode", [](const Ctx& c) { return c.shadow.mode == ShadowMode::evssm; },
		GateSeverity::inertNote,
		"mode != \"evssm\"：evssm.* 调参块没人读（按档分立，pcss 档读的是 pcss.*）",
		std::span{kEvssmKeys}},
	{"picking_probe.", "picking_probe.frame",
		[](const Ctx& c) { return c.cfg.pickingProbeFrame != 0; },
		GateSeverity::inertNote, "picking_probe.frame=0（探针关）：坐标是死值",
		std::span{kPickingProbeKeys}},
	{"ui_click_probe.", "ui_click_probe.frame",
		[](const Ctx& c) { return c.cfg.uiClickProbeFrame != 0; },
		GateSeverity::inertNote, "ui_click_probe.frame=0（探针关）：坐标是死值",
		std::span{kUiClickProbeKeys}},
	{"camera_probe.", "camera_probe.frame",
		[](const Ctx& c) { return c.cfg.cameraProbeFrame != 0; },
		GateSeverity::inertNote, "camera_probe.frame=0（探针关）：子键是死值",
		std::span{kCameraProbeKeys}},
	{"meshlet.", "meshlet.assets", [](const Ctx& c) { return c.runtime.meshletAssets; },
		GateSeverity::warn,
		"meshlet.assets = false ⟹ 簇条目不烘（bakeMeshletEntries 吃 slots = nullopt 整段早退），"
		"本键没有作用对象。装配层 wireMeshlet* 还按「有没有条目可采 / 载体在不在」的**事实**出声"
		"（同因不同层：无产物 / 多产物回落 / 零模型命中只有装配期看得见）",
		std::span{kMeshletKeys}},
};

/**
 * 块级发射（债-129 第二刀 D）：宿主不成立 ⟹ 块内每枚「配了」的键各出一条 finding
 * （门名 = 键名、级别 = 块级；消息 = 键级覆盖优先、缺省落块级——逐键归因不是重复项，
 * 见 kMeshletKeys 头注释）。与独立行的分工见 GateBlock 注释（不双报）。
 * 抽成函数而非内联：lintDiagnosticsGates 的认知复杂度阈值 = 15，双层嵌套循环 + 双守卫
 * 内联会把它推过线。
 */
void appendBlockFindings(std::vector<GateFinding>& findings, const Ctx& ctx) {
	for (const auto& b : kGateBlocks)
		if (!b.ownerActive(ctx))
			for (const auto& k : b.keys)
				if (k.present(ctx))
					findings.push_back({k.gate, b.severity,
						std::string{k.message.empty() ? b.message : k.message}});
}

}  // namespace

/** gateLintRuleGates 定义（接口注释见 gate_lint.cppm）。 */
std::vector<std::string> gateLintRuleGates() {
	std::vector<std::string> gates;
	gates.reserve(std::size(kGateRules));
	for (const auto& r : kGateRules)
		gates.emplace_back(r.gate);
	return gates;
}

/** gateLintBlockCoverage 定义（接口注释见 gate_lint.cppm）。 */
std::vector<GateBlockCoverage> gateLintBlockCoverage() {
	std::vector<GateBlockCoverage> blocks;
	blocks.reserve(std::size(kGateBlocks));
	for (const auto& b : kGateBlocks) {
		GateBlockCoverage out{std::string{b.prefix}, std::string{b.owner}, {}};
		out.keys.reserve(b.keys.size());
		for (const auto& k : b.keys)
			out.keys.emplace_back(k.gate);
		blocks.push_back(std::move(out));
	}
	return blocks;
}

/** lintDiagnosticsGates 定义（接口注释见 gate_lint.cppm）。 */
std::vector<GateFinding> lintDiagnosticsGates(const DiagnosticsConfig& cfg,
	const DiagnosticsConfig& defaults, const ShadowConfig& shadow,
	const ShadowConfig& shadowDefaults, const RuntimeConfig& runtime,
	const RuntimeConfig& runtimeDefaults, std::span<const RgPassPlan> passes,
	RgGraphVariant variant, const GateEnv& env) {
	const Ctx ctx{cfg, defaults, shadow, shadowDefaults, runtime, runtimeDefaults,
		graphHasDeferredLight(passes), env};
	std::vector<GateFinding> findings;
	for (const auto& r : kGateRules)
		if (r.violated && r.violated(ctx))
			findings.push_back({r.gate, r.severity, std::string{r.message}});
	appendBlockFindings(findings, ctx);  // 块级宿主声明（判据见 appendBlockFindings 注释）

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
