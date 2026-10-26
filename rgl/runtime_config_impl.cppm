module;
#include <sol/sol.hpp>
#include <fmt/format.h>
module runtime_config:impl;

import runtime_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import :keys;       // 键名真相源 + 未知键 lint（collectPresentKeys / lintUnknownRuntimeKeys）
import :failfast;   // fail-fast 键链（applyFailFastKeys / applyPresentModeKey）
import rlstd;
import rllogger;
import vkerror;

namespace RGL {

// loadRuntimeConfig 定义（sol2 解析整体移出接口 TU，GMF 只 sol/fmt；本单元是叶子边）。

/**
 * render_scale 解析（Phase 14 批 14.3）：内部渲染分辨率倍率。
 *
 * 非有限值 / 越界 = 回落或钳制 + warn，**不 fail-fast**（与 ui_font_size 同处理：这是偏好量
 * 不是正确性契约，钳制后画面仍可看，而 fail-fast 会让一次笔误毁掉一次长跑）。
 * 缺键 = 保持传入值（即 `RuntimeConfig::renderScale` 的默认 1.0，单一真相源在结构体）。
 *
 * 抽成函数而非内联：`loadRuntimeConfig` 的认知复杂度已贴着规则上限，这段三分支内联会推过线。
 * @param root runtime.lua 根表
 * @param scale 写入目标（传入值 = 当前默认，缺键时原样保留）
 * @param logger 可为空（无日志器时静默按默认处理）
 * @param trace 键轨迹（缺键也记 = 解析器读过它，覆盖率护栏的输入）
 */
static void applyRenderScale(sol::table root, float& scale, RLLogger* logger,
	RuntimeKeyTrace& trace) {
	trace.read.emplace_back("render_scale");
	sol::optional<float> raw = root["render_scale"];
	if (!raw)
		return;
	const float v = *raw;
	// 钳制本体在 clampRenderScale（rlbase）——配置侧与 Swapchain 构造侧共用同一个函数，
	// 所以"这里钳过一次、构造时又钳一次"不可能给出两个答案。本函数只负责**怎么出声**。
	const float clamped = clampRenderScale(v, scale);
	if (clamped != v && logger)
		logger->warn("runtime.lua render_scale = {} 不可用（非有限值或越界 [{}, {}]），回落 {}", v,
			kRenderScaleMin, kRenderScaleMax, clamped);
	scale = clamped;
}

/**
 * llm_shot/llm_debug 表公共细字段填充（两段字段布局相同 {delay_frames, file, size}）：
 * 缺字段保留当前值 = 默认或前段已填值。
 * @param shot 源表（调用方保证为表）
 * @param llm 写入目标（delayFrames/file/width/height）
 */

static void applyShotFields(sol::table shot, LlmDebugConfig& llm) {
	sol::optional<uint32_t> delayFrames = shot["delay_frames"];
	if (delayFrames)
		llm.delayFrames = *delayFrames;
	sol::optional<std::string> file = shot["file"];
	if (file)
		llm.file = *file;
	sol::table sizeTable = shot["size"];
	if (sizeTable.valid() && sizeTable.is<sol::table>()) {
		sol::optional<uint32_t> w = sizeTable[1];
		if (w)
			llm.width = *w;
		sol::optional<uint32_t> h = sizeTable[2];
		if (h)
			llm.height = *h;
	}
}

/**
 * llm_debug/llm_shot 解析（自 camera_config 迁入，语义不变）：
 * llm_shot 常驻 {file,size,delay_frames} 先填，llm_debug 覆盖 enabled；
 * 向后兼容 llm_debug = table（旧格式 = 启用 + 细字段，覆盖 llm_shot）。
 * 键轨迹：llm_debug 记一个键（布尔/表两形态同名，子表内部键不在 lint 射程）；
 * llm_shot 记三个字段键（**无条件**记 = 覆盖率护栏输入，同 shadow applyTuningBlock 口径）。
 * @param root 配置根表
 * @param llm 输出：解析好的截图配置（缺字段保留当前值 = 默认或前段已填值）
 * @param trace 键轨迹
 */
static void parseLlm(sol::table& root, LlmDebugConfig& llm, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("llm_debug");
	trace.read.emplace_back("llm_shot.delay_frames");
	trace.read.emplace_back("llm_shot.file");
	trace.read.emplace_back("llm_shot.size");
	sol::table shotTable = root["llm_shot"];
	if (shotTable.valid() && shotTable.is<sol::table>())
		applyShotFields(shotTable, llm);
	sol::object llmObj = root["llm_debug"];
	if (llmObj.valid()) {
		if (llmObj.is<bool>()) {
			llm.enabled = llmObj.as<bool>();
		} else if (llmObj.is<sol::table>()) {
			sol::table t = llmObj.as<sol::table>();
			llm.enabled = true;
			applyShotFields(t, llm);
		}
	}
}

/**
 * imgui 面板字体字段解析（Phase 10b 批 11b.1）：ui_font 空串 = 系统字体探测链；
 * ui_font_size 非正 = 配置笔误/缺省，回落默认 18（纯显示项，不 fail-fast）。
 * 抽成小函数以守住主解析函数的认知复杂度阈值。
 * @param root 配置根表
 * @param cfg 输出（只写 uiFont / uiFontSize 两字段）
 * @param trace 键轨迹（两键**无条件**记 = 覆盖率护栏输入）
 */
static void readUiFont(sol::table& root, RuntimeConfig& cfg, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("ui_font");
	trace.read.emplace_back("ui_font_size");
	if (sol::optional<std::string> uiFont = root["ui_font"])
		cfg.uiFont = *uiFont;
	if (sol::optional<double> size = root["ui_font_size"]; size && *size > 0.0)
		cfg.uiFontSize = static_cast<float>(*size);
}

/** 读一个可选布尔键（缺键 = 保持默认值）。类型错误 = sol 直接报错（与 headless 等布尔键
 *  同处理，不静默回落）。抽成函数而不是就地 `if`：`loadRuntimeConfig` 的认知复杂度已贴红线。
 *  @param root runtime.lua 顶层表
 *  @param key  键名
 *  @param dst  目标字段（仅在键存在时被写） */
void readBoolKey(const sol::table& root, const char* key, bool& dst) {
	sol::optional<bool> value = root[key];
	if (value)
		dst = *value;
}

/** 读一个可选浮点键（缺键 = 保持默认值，与 readBoolKey 同形）。
 *  类型错误 = sol 直接报错（不静默回落，同 render_scale 之外的既有纪律）。
 *  @param root 配置表（本函数不关心是哪一层，嵌套块传块表即可）
 *  @param key  键名
 *  @param dst  目标字段（仅在键存在时被写） */
void readFloatKey(const sol::table& root, const char* key, float& dst) {
	sol::optional<float> value = root[key];
	if (value)
		dst = *value;
}

/**
 * `fsr` 嵌套块解析（批 14.6）：`fsr = { temporal = …, jitter = … }` 两枚调参。
 * 缺块 / 缺键 = 保持结构体默认（true/true = FSR 设计点，理由 → RuntimeConfig 字段注释）。
 * 键的归属就是本函数——**没有**扁平键的兼容双读：两个真相源是本项目反复登记的反模式。
 * 键轨迹在块入口**无条件**记全两枚调参键（覆盖率护栏输入，同 shadow applyTuningBlock 口径）。
 */
static void readFsrBlock(const sol::table& root, RuntimeConfig& cfg, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("fsr.temporal");
	trace.read.emplace_back("fsr.jitter");
	sol::table fsr = root["fsr"];
	if (!fsr.valid() || !fsr.is<sol::table>())
		return;
	readBoolKey(fsr, "temporal", cfg.fsrTemporal);
	readBoolKey(fsr, "jitter", cfg.fsrJitter);
}

/**
 * `ssr` 嵌套块解析（批 15.1 总开关 + 同批参数面）：`enabled` 一枚总开关 + 6 枚调参。
 * 缺块 / 缺键 = 保持结构体默认（`ssrEnabled = false` = 图里没有 ssr pass；6 枚系数 =
 * `SsrTuning` 成员初始化值 = **全项目唯一真相源**，shader 侧零 fallback）。与 `aa` 正交
 *（`aa` 管抗锯齿选路，本块管 SSR 特性；组合语义 → RuntimeConfig::ssrEnabled 注释）。
 * 形态先例 = readFsrBlock（嵌套块 + readBoolKey，无扁平键双读兼容）。
 *
 * ## 🔴 两条 shader 侧不变式，配置层**刻意不校验**（判断理由，2026-10-10 拍板）
 *
 * 不变式：`bias_px < thickness_px`（违反 = 命中窗口 `[-thickness, -bias]` 为空 = 永不命中）、
 * `rough_full < rough_cutoff`（违反 = 门控反向：光滑面全关、粗糙面满反射；实测靶上覆盖率
 * 恰 0、泄漏 0，形态是"没反射"而不是 NaN 垃圾画面——NaN 只在 e0 == e1 且 roughness 恰等
 * 于 e0 的像素上出现）。
 *
 * 1. **校验会直接堵死反向消融。** `bias_px >= thickness_px` 正是判据 6 要跑的档（闭式期望
 *    = 凸包内覆盖率**恰为 0**），`thickness_px = 0` 同理。配置层把它当非法值拒了/钳了，
 *    就等于"为了让判据过而改判据"——只是改在配置层，更难查。
 * 2. **配置层校验管不住真正的写入方。** 这两条不变式的语义住在 shader，将来 RPC / imgui
 *    若接进来就是绕过配置层的路径 ⟹ 只在配置层校验 = 给人"已经防住"的错觉（第二个真相源）。
 * 3. **两种违反都是可见失效**（整屏没反射 / 垃圾画面），且 `just ssr-check` 判据 A 直接红
 *    ⟹ 不是本项目最怕的"静默漂移"形态。与之对照：`render_scale` 走"钳制 + warn"是因为越界
 *    值仍想要一张能看的图；这里的"越界"本身就是**档**，钳制 = 静默换档，比笔误更坏。
 *
 * ⟹ 本函数**原样透传**，一条 warn 都不发（warn 会在每次跑消融档时刷屏，教人不信任日志）。
 * 不变式的权威措辞在 `SsrTuning` 字段注释与 `ssr.frag.slang` 参数段。
 *
 * ## 为什么**暂**未往 gate_lint 挂号（2026-10-17 重写：原结论的三条理由已全部失效）
 *
 * gate_lint 的覆盖率护栏 = 单测断言 `gateLintRuleGates()` 与 `diagnosticsEchoKeys() ∪
 * shadowConfigEchoKeys() ∪ runtimeConfigKnownKeys() 的 meshlet. 前缀键` 三方相等 ⟹ 它的
 * 管辖范围 = `configs/diagnostics.lua` 与 `configs/shadow.lua` 两份配置，外加
 * `configs/runtime.lua` 的 **meshlet 块**（债-106 整块接管，不挑键）。**其余** runtime.lua
 * 键（含本块 `enabled`、`aa` / `render_scale` / `fsr.*`）今天仍不在表里，但这**不再是裁决
 * 结论，而是未还的账**（债-129 S2 射程）。原标题写「防后来者顺手补上」——实测方向正好防反了：
 *
 * 1. ❌「其余块没有『子键开而总门关』的孤立形态」——`fsr.temporal` / `fsr.jitter` 开了而
 *    `aa ≠ "fsr"`、`clip_mode`（scene.lua）设了而 `aa ≠ "taa"` **就是这个形态**，且它一直
 *    由工具层替引擎守着（`smoke.py` 的 `guard_flags` 两条 `fail()`）。**工具层的存在即本条反证**。
 * 2. ❌「硬塞要给 runtime.lua 造第三张回显表 = 第二真相源」——那张表**已经有了**：
 *    `kRuntimeKeys[]` / `runtimeConfigKnownKeys()`，meshlet 块吃的就是它。扩射程 = 改测试里
 *    那个前缀过滤，**不新增真相源**。
 * 3. ❌「`enabled` 的前置已由 `decorateSsrPass` 自己出 warn，lint 无话可说」——该 warn 已随
 *    债-129 **S3-C 删除**（改为启动期抛 `std::runtime_error`）。本条是 2026-10-17 当天失效的。
 *
 * ✅ **仍然成立的一条**：不该把 runtime.lua 键混进 diagnostics 的**回显语义**（那张表按"诊断
 * 门"组织，画质系数不是）⟹ 要扩的是**规则表 + 前缀过滤**，不是把键塞进 diagnostics 回显行。
 * ⚠ 另有一条**真障碍**：`Ctx` 只吃 diagnostics / shadow / runtime 三份配置，**看不见
 * scene.lua** ⟹ 「`clip_mode` × `aa`」这类**跨文件**矛盾今天结构上就判不了。这是 lint 的
 * 真实盲区、不是借口——`clip_mode` 只会是第一个撞上来的（补 scene 配置进 `Ctx` 是 S2 的一步）。
 * 键轨迹在块入口**无条件**记全七枚键（覆盖率护栏输入，同 shadow applyTuningBlock 口径）。
 * 键轨迹在块入口**无条件**记全七枚键（覆盖率护栏输入，同 shadow applyTuningBlock 口径）。
 */
static void readSsrBlock(const sol::table& root, RuntimeConfig& cfg, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("ssr.enabled");
	trace.read.emplace_back("ssr.max_distance_radii");
	trace.read.emplace_back("ssr.thickness_px");
	trace.read.emplace_back("ssr.bias_px");
	trace.read.emplace_back("ssr.rough_full");
	trace.read.emplace_back("ssr.rough_cutoff");
	trace.read.emplace_back("ssr.blur_scale");
	sol::table ssr = root["ssr"];
	if (!ssr.valid() || !ssr.is<sol::table>())
		return;
	readBoolKey(ssr, "enabled", cfg.ssrEnabled);
	// 6 枚调参 = 每帧量（键名 ↔ scene data 分量的映射唯一落点 = fillSsrParams）
	readFloatKey(ssr, "max_distance_radii", cfg.ssr.maxDistanceRadii);
	readFloatKey(ssr, "thickness_px", cfg.ssr.thicknessPx);
	readFloatKey(ssr, "bias_px", cfg.ssr.biasPx);
	readFloatKey(ssr, "rough_full", cfg.ssr.roughFull);
	readFloatKey(ssr, "rough_cutoff", cfg.ssr.roughCutoff);
	// 批 15.3B：模糊半径系数（@723792.z，ssr_apply 消费）。0 = 合法消融档（纯双线性上采样），
	// 同上面两枚 px 系数一条纪律：不 clamp 不 warn（readSsrBlock 头注释的理由逐字适用）。
	readFloatKey(ssr, "blur_scale", cfg.ssr.blurScale);
}

/**
 * `wet` 嵌套块解析（批 15.2 子步 A 参数面）：6 枚调制系数/空间变化参数，**没有**创建期
 * 开关（湿面调制不是独立 pass，是 deferred_geometry 的 G-buffer 调制，不改图形态；`strength = 0`
 * 就是它的门关档）。缺块 / 缺键 = 保持结构体默认（`WetTuning` 成员初始化值 =
 * **全项目唯一真相源**，shader 侧零 fallback）。形态先例 = readSsrBlock（嵌套块 +
 * readFloatKey，无扁平键双读兼容——两个真相源是本项目反复登记的反模式）。
 *
 * ## 🔴 shader 侧不变式 `rough_lo < rough_hi`，配置层**刻意不校验**（判断同 readSsrBlock）
 *
 * 1. **校验会堵死消融**：`strength = 0` 是合法消融档（因果泄漏恒等式的门关档），配置层
 *    把它当非法值拒了/钳了 = 为了让判据过而改判据，只是改在配置层，更难查。
 * 2. **配置层校验管不住真正的写入方**：不变式语义住在 shader（15.2B 接线后违反的失效
 *    形态由 shader 定义），将来 RPC / imgui 接进来就是绕过配置层的路径 ⟹ 只在配置层
 *    校验 = 给人"已经防住"的错觉（第二真相源）。
 *
 * ⟹ 本函数**原样透传**，一条 warn 都不发（warn 会在每次跑消融档时刷屏，教人不信任日志）。
 * 不变式的权威措辞在 `WetTuning` 字段注释。gate_lint 不挂号的理由与 ssr 块完全同款
 * （runtime.lua 键不在 gate_lint 管辖的两张回显表里，→ readSsrBlock 末段）。
 * 键轨迹在块入口**无条件**记全六枚调参键（覆盖率护栏输入，同 shadow applyTuningBlock 口径）。
 */
static void readWetBlock(const sol::table& root, RuntimeConfig& cfg, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("wet.strength");
	trace.read.emplace_back("wet.dark");
	trace.read.emplace_back("wet.rough_lo");
	trace.read.emplace_back("wet.rough_hi");
	trace.read.emplace_back("wet.ripple_amp");
	trace.read.emplace_back("wet.puddle_freq_radii");
	sol::table wet = root["wet"];
	if (!wet.valid() || !wet.is<sol::table>())
		return;
	// 6 枚调参 = 每帧量（键名 ↔ scene data 分量的映射唯一落点 = fillWetParams）
	readFloatKey(wet, "strength", cfg.wet.strength);
	readFloatKey(wet, "dark", cfg.wet.dark);
	readFloatKey(wet, "rough_lo", cfg.wet.roughLo);
	readFloatKey(wet, "rough_hi", cfg.wet.roughHi);
	readFloatKey(wet, "ripple_amp", cfg.wet.rippleAmp);
	readFloatKey(wet, "puddle_freq_radii", cfg.wet.puddleFreqRadii);
}

/**
 * `coordinate` 嵌套块解析（相对坐标计划批 3：原点 rebase 门）。只有一枚布尔
 * `rebase`，缺块 / 缺键 = 保持结构体默认（**`coordinateRebase = true` = 门开**，2026-10-26
 * 随批 5 的两条图像判据转绿而翻；门关 = 显式写 `rebase = false`）。形态先例 = readFsrBlock
 * （嵌套块 + readBoolKey，无扁平键双读兼容——两个真相源是本项目反复登记的反模式）。
 *
 * 🔴 与 ssr/wet 的**每帧调参块**不同，本块只有一枚**创建期**门：它不搬系数，
 * 门经 main 启动期 emplace 进 `RenderOrigin` ctx，运行期由 projectionSystem 每帧写
 * position（门关 = 恒 0；门开 = 相机位置 snap 到级联纹素粒度，批 5 第 1 刀）。
 * 配置层**不拦** true（拦了 = 「写了 true 什么都不知道」的静默形态；交给
 * system 点名本键的响亮失败——粒度 NaN 才抛，tests 钉住那条）。
 * 键轨迹在块入口**无条件**记一枚键（覆盖率护栏输入，同 shadow applyTuningBlock 口径）。
 * @param root runtime.lua 顶层表
 * @param cfg 写入目标（缺块/缺键时原样保留默认）
 * @param trace 键轨迹
 */
static void readCoordinateBlock(const sol::table& root, RuntimeConfig& cfg, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("coordinate.rebase");
	sol::table coordinate = root["coordinate"];
	if (!coordinate.valid() || !coordinate.is<sol::table>())
		return;
	readBoolKey(coordinate, "rebase", cfg.coordinateRebase);
}

/** loadRuntimeConfig 定义（接口注释见 runtime_config.cppm）：headless/headless_size/win_size/show_after/present_mode/ui_font 逐一可选解析 + parseLlm。键轨迹 + 未知键 lint 同 diagnostics（债-11）。 */
[[nodiscard]] std::expected<RuntimeConfig, Error> loadRuntimeConfig(
	std::string_view path, RuntimeKeyTrace* trace) {
	auto* logger = RLLogger::getInstance();
	// 键轨迹**总是**记录（未知键 lint 在启动期就要工作，main 传 nullptr 也不例外）；
	// 调用方给了出参就带出去，没给就落在本地局部量（diagnostics 同口径）。
	RuntimeKeyTrace fallback;
	if (!trace)
		trace = &fallback;

	{
		std::ifstream probe{std::string(path)};
		if (!probe.is_open()) {
			if (logger)
				logger->warn("Runtime config not found ({}), using defaults (interactive window)",
					std::string(path));
			return RuntimeConfig{};
		}
	}

	sol::state lua;
	lua.open_libraries(sol::lib::base, sol::lib::table);
	lua["dofile"] = sol::nil;
	lua["loadfile"] = sol::nil;
	lua["package"] = sol::nil;
	lua["require"] = sol::nil;

	auto result = lua.do_file(std::string(path).c_str());
	if (!result.valid()) {
		sol::optional<sol::error> err = result.get<sol::optional<sol::error>>();
		std::string msg = err ? err->what() : std::string("unknown lua error");
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("runtime config ({}): {}", path, msg)});
	}

	try {
		sol::table root = result[0].as<sol::table>();
		RuntimeConfig cfg;
		collectPresentKeys(root, *trace);

		trace->read.emplace_back("headless");
		readBoolKey(root, "headless", cfg.headless);
		// 数组形尺寸表（{w, h}）无字段键名 ⟹ 键轨迹只记表名一个键（口径见 kRuntimeKeys 注释）
		trace->read.emplace_back("headless_size");
		trace->read.emplace_back("win_size");
		auto readSize = [&](const char* key, uint32_t& w, uint32_t& h) {
			sol::table t = root[key];
			if (t.valid() && t.is<sol::table>()) {
				sol::optional<uint32_t> vw = t[1];
				sol::optional<uint32_t> vh = t[2];
				if (vw)
					w = *vw;
				if (vh)
					h = *vh;
			}
		};
		readSize("headless_size", cfg.headlessWidth, cfg.headlessHeight);
		readSize("win_size", cfg.winWidth, cfg.winHeight);

		applyRenderScale(root, cfg.renderScale, logger, *trace);  // 批 14.3：内部渲染分辨率倍率
		// velocityField 不从键读：velocity_field 键已随债-129 S2.5-C 撤并，它是纯派生件，
		// 唯一置重点 = applyAaKey 内一行（aa=fsr 隐含打开）。
		// fail-fast 键链（aa 三态 + ★0.5-b2 meshlet.coplanar 三态；未知值 = 启动报错退出，
		// 不静默回落——四臂主判据的假绿防线，→ applyFailFastKeys 注释）
		if (auto err = applyFailFastKeys(root, cfg, path, *trace); err)
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed, *err});
		readFsrBlock(root, cfg, *trace);  // 批 14.6：FSR 调参收进嵌套块（无扁平键双读兼容）
		readSsrBlock(root, cfg, *trace);  // 批 15.1：SSR 总开关（嵌套块，缺块/缺键 = false = 图里没有 ssr pass）
		readWetBlock(root, cfg, *trace);  // 批 15.2：湿面参数（嵌套块，缺块/缺键 = WetTuning 默认；消费者 = deferred_geometry 湿面调制）
		// 相对坐标批 3：coordinate.rebase 门（嵌套块，缺块/缺键 = **true = 门开**，2026-10-26 翻；
		// 门关 = 显式 false = projectionSystem 把原点钉零；门开 = projectionSystem 每帧算动态原点，
		// 配置层不拦——理由 → readCoordinateBlock 注释）
		readCoordinateBlock(root, cfg, *trace);
		trace->read.emplace_back("show_after");
		sol::optional<int> showAfter = root["show_after"];
		if (showAfter)
			cfg.showAfter = *showAfter;

		// present_mode：窗口 present 模式偏好（PresentModePref；headless 忽略）。
		// 未知值 = 配置笔误，fail-fast（与类型错误同级，不静默回落；抽函数 = 认知复杂度红线）。
		if (auto err = applyPresentModeKey(root, cfg, path, *trace); err)
			return std::unexpected(Error{ErrorCode::ConfigLoadFailed, *err});

		parseLlm(root, cfg.llm, *trace);

		readUiFont(root, cfg, *trace);  // imgui 面板字体（Phase 10b 批 11b.1）

		// 未知键 lint（债-11：runtime.lua 里拼错的键此前完全静默）
		lintUnknownRuntimeKeys(*trace);
		return cfg;
	} catch (const std::exception& e) {
		return std::unexpected(Error{ErrorCode::ConfigLoadFailed,
			fmt::format("runtime config ({}): {}", path, e.what())});
	}
}

}  // namespace RGL
