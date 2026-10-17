module;
#include <sol/sol.hpp>
#include <fmt/format.h>
module runtime_config:impl;

import runtime_config;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;

namespace RGL {

// loadRuntimeConfig 定义（sol2 解析整体移出接口 TU，GMF 只 sol/fmt；本单元是叶子边）。

namespace {

/**
 * 键表 = 「解析器认识哪些键」的单一真相源（diagnostics 的 kGateEcho 同角色，债-11）：
 * ① 未知键 lint 的判据表；② tests 里「读过的键集合 == 键表键集合」双向覆盖率断言的输入。
 * 嵌套块字段用 `表名.字段`；数组形字段（headless_size/win_size 的 [1][2]）无键名只记
 * 表名；llm_debug 有布尔/表两形态（表内字段与 llm_shot 同名）只记一个键——lint 只看
 * 顶层，子表内部键不在射程（diagnostics 同边界）。
 * ⚠ **原生数组**（尺寸自动推导）不是 `std::array<T, N>`——shadow_config_impl 同款教训。
 */
constexpr std::string_view kRuntimeKeys[]{
	"headless",
	"headless_size",
	"win_size",
	"render_scale",
	"aa",
	"fsr.temporal",
	"fsr.jitter",
	"ssr.enabled",
	"ssr.max_distance_radii",
	"ssr.thickness_px",
	"ssr.bias_px",
	"ssr.rough_full",
	"ssr.rough_cutoff",
	"ssr.blur_scale",
	"wet.strength",
	"wet.dark",
	"wet.rough_lo",
	"wet.rough_hi",
	"wet.ripple_amp",
	"wet.puddle_freq_radii",
	"meshlet.enable",
	"meshlet.overlay",
	"meshlet.debug",
	"meshlet.tint",
	"meshlet.switch",
	"meshlet.cull",
	"meshlet.shadow_lod",
	"meshlet.shadow_lod_fallback",
	"meshlet.mdi_shadow",
	"meshlet.coplanar",
	"show_after",
	"present_mode",
	"ui_font",
	"ui_font_size",
	"llm_debug",
	"llm_shot.delay_frames",
	"llm_shot.file",
	"llm_shot.size",
};

/**
 * 「文件里实际出现的顶层键」采集（diagnostics 的 collectPresentKeys 同构，未知键 lint
 * 的输入；子表只记表名——present 来自文件本体不来自读取动作）。
 * @param root 配置根表
 * @param trace 键轨迹（present 追加目标）
 */
void collectPresentKeys(const sol::table& root, RuntimeKeyTrace& trace) {
	for (auto&& kv : root) {
		if (sol::optional<std::string> k = kv.first.as<sol::optional<std::string>>())
			trace.present.push_back(*k);
	}
}

/**
 * 未知键 lint（lintUnknownDiagnosticsKeys 同判据，债-11）：文件里出现的顶层键，凡键表
 * 键名的「`.` 前缀」都不等于它 = 解析器没人读它（拼错的键，此前完全静默）。判据来自
 * 键表本身（= 解析器键名真相源），不另立名单——覆盖率护栏（测试）保证键表 ⊥ 解析器
 * 不会漂移，本函数因此只需查一张表。
 * @param trace 解析期键轨迹
 */
void lintUnknownRuntimeKeys(const RuntimeKeyTrace& trace) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	for (const auto& key : trace.present) {
		bool known = false;
		for (const auto& k : kRuntimeKeys)
			if (k.substr(0, k.find('.')) == key) {  // 无 `.` 时 = 全名比对
				known = true;
				break;
			}
		if (!known)
			logger->warn("runtime: 未知键 '{}'（解析器不读它，拼错？已忽略）", key);
	}
}

}  // namespace

/** runtimeConfigKnownKeys 定义（接口注释见 runtime_config.cppm）。 */
std::vector<std::string> runtimeConfigKnownKeys() {
	std::vector<std::string> keys;
	keys.reserve(std::size(kRuntimeKeys));
	for (const auto& k : kRuntimeKeys)
		keys.emplace_back(k);
	return keys;
}

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
 * `aa` 键解析（批 14.6 抗锯齿三态开关）：字符串 → `AaMode`。
 *
 * 缺键 = 保持默认 off（出厂态 = 红线 A）；未知值 = 返回错误消息由调用方 fail-fast
 * （present_mode 的既有纪律：配置笔误不静默回落——静默回落的失效形态 = 你以为开着
 * 抗锯齿在量图，其实跑在关档上，而图看起来"只是糊了一点"）。
 * 抽成函数 = 认知复杂度红线（先例 applyRenderScale），也为了让 tests 能单独打它。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 present_mode 同口径带配置文件路径）
 * @param trace 键轨迹（缺键也记 = 解析器读过它，覆盖率护栏的输入）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyAaKey(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("aa");
	sol::optional<std::string> raw = root["aa"];
	if (!raw)
		return std::nullopt;
	if (*raw == "off")
		cfg.aa = AaMode::off;
	else if (*raw == "fsr") {
		// fsr 态**隐含打开**速度场（FSR 的 motionVectors 输入就是那张图）。债-129 S2.5-C：
		// velocity_field lua 键已撤并，velocityField 是**纯派生件**——本行是全项目唯一置重点
		// （消费侧对派生关系无感知）；off/taa 态不碰它 ⟹ 不会凭空冒出 velocity pass。
		cfg.aa = AaMode::fsr;
		cfg.velocityField = true;
	} else if (*raw == "taa")
		cfg.aa = AaMode::taa;
	else
		return fmt::format(
			"runtime config ({}): aa must be \"off\", \"fsr\" or \"taa\", got \"{}\"",
			path, *raw);
	return std::nullopt;
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
 * `meshlet.coplanar` 三态字符串键解析（★0.5-b2 共面双臂；形态先例 = `applyAaKey`）。
 *
 * 缺键 = 保持默认 `off`（= 不碰动态状态 = 门关/off 动态状态序列逐 bit 不变，A1 构造性
 * 保证）。**未知值 = 返回错误消息由调用方 fail-fast**，不静默回落（present_mode/aa 的既有
 * 纪律，本批尤其致命：主判据 = 四臂截图哈希全同，笔误值静默回落成 off 会让 equal/ne 两臂
 * 根本没跑而四臂照样全绿 = 整批假绿）。
 * @param root meshlet 块表（与 readMeshletBlock 同层）
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa/present_mode 同口径带配置文件路径）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyMeshletCoplanarKey(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["coplanar"];
	if (!raw)
		return std::nullopt;
	if (*raw == "off")
		cfg.meshletCoplanar = MeshletCoplanarMode::off;
	else if (*raw == "equal")
		cfg.meshletCoplanar = MeshletCoplanarMode::equal;
	else if (*raw == "ne")
		cfg.meshletCoplanar = MeshletCoplanarMode::ne;
	else
		return fmt::format(
			"runtime config ({}): meshlet.coplanar must be \"off\", \"equal\" or \"ne\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * 严格布尔键读取（★1-a1ii-β-1 起 **meshlet 块专用**；与 `readBoolKey` 的唯一差别 =
 * **值不是布尔时立即报错**，而不是静默当键缺失）。
 *
 * 为什么必须换（实测，不是推测）：`sol::optional<bool> v = root[key]` 在值是字符串或数字时
 * **静默给 nullopt** —— `meshlet.switch = "yes"` 与 `meshlet.switch = 1` 两条都按「键不存在」
 * 跑完整程，errors=0 warnings=0、日志零行。对 meshlet 块这是最坏的失效形态：β 的主判据是
 * 「截图哈希对基线」，键被静默吃掉 ⟹ 实际跑的是旧路径而判据全绿。`present_mode` / `aa` /
 * `meshlet.coplanar` 的「笔误不静默回落」纪律同向。
 * 缺键仍 = 保持结构体默认（门关 / off 护栏不动）。旧块（fsr/ssr/wet）不改——零回归面，
 * 严格化整张配置表是另一批的事。
 * @return nullopt = 缺键或类型正确；有值 = 错误消息（调用方 fail-fast）
 */
[[nodiscard]] static std::optional<std::string> readStrictBoolKey(const sol::table& root,
	const char* key, bool& dst, std::string_view path) {
	sol::object value = root[key];
	const auto type = value.get_type();
	if (type == sol::type::lua_nil)
		return std::nullopt;  // 缺键 = 出厂默认（门关护栏）
	if (type != sol::type::boolean)
		return fmt::format("runtime config ({}): meshlet.{} must be a boolean, got {}",
			path, key, sol::type_name(root.lua_state(), type));
	dst = value.as<bool>();
	return std::nullopt;
}

/**
 * 严格整数键读取（★18-B1 起 **meshlet 块专用**；与 `readStrictBoolKey` 同一条纪律，只是
 * 目标类型是 `int32_t`）：**类型不对立即报错点名键**，🔴 **不许**静默当键缺失。
 *
 * 理由不重抄——见 `readStrictBoolKey` 注释里那条实测（`sol::optional<T>` 在值类型不匹配时
 * **静默给 nullopt**，键被吃掉而 errors=0 warnings=0、日志零行 ⟹ 截图哈希判据整批假绿）。
 * 本键的失效形态同样致命：`shadow_lod = "5"` 被静默吃掉 = 跑的是关档，而"门开了、条目建了、
 * 图没变"会被读成「LOD 不影响图像」。
 * Lua 的 number 全是 double，故整数判据 = 「是 number 且无小数部分」；`5.0` 放行（Lua 里
 * `5` 与 `5.0` 是同一个值），`5.5` 报错而不是悄悄截断成 5。
 * @return nullopt = 缺键或类型正确；有值 = 错误消息（调用方 fail-fast）
 */
[[nodiscard]] static std::optional<std::string> readStrictIntKey(const sol::table& root,
	const char* key, int32_t& dst, std::string_view path) {
	sol::object value = root[key];
	const auto type = value.get_type();
	if (type == sol::type::lua_nil)
		return std::nullopt;  // 缺键 = 保持结构体默认（-1 = 本档关）
	if (type != sol::type::number)
		return fmt::format("runtime config ({}): meshlet.{} must be an integer, got {}",
			path, key, sol::type_name(root.lua_state(), type));
	const double raw = value.as<double>();
	// 整数判据 = 无小数部分；范围判据 = int32_t 可表示（越界不截断，报错——截断出来的
	// 是个语义完全不同的数，比"启动失败"难查得多）。
	constexpr double lo = static_cast<double>(std::numeric_limits<int32_t>::min());
	constexpr double hi = static_cast<double>(std::numeric_limits<int32_t>::max());
	if (raw != std::floor(raw) || raw < lo || raw > hi)
		return fmt::format(
			"runtime config ({}): meshlet.{} must be an integer (got {})", path, key, raw);
	dst = static_cast<int32_t>(raw);
	return std::nullopt;
}

/**
 * `meshlet.shadow_lod_fallback` 字符串键解析（★18-B1；形态先例 = `applyMeshletCoplanarKey`）。
 *
 * 缺键 = 保持默认 `deepest`。**未知值 = 返回错误消息由调用方 fail-fast**，不静默回落
 * （`meshlet.coplanar` 的理由在此更硬：§18.2 实测 D=7 下 deepest = 3.32% 而 l0 = 34.97%
 * 三角，笔误值静默回落成默认会让「成本判据」整批读到错的臂）。
 * ⚠ 覆盖面与先例同边界：只有**字符串**值进得了本判据，数字值被 sol 的 `optional<string>`
 * 读成 nullopt = 走缺键保默认（实机对照 `meshlet.coplanar = 0` 同形）。整数键的严格类型面
 * 在 `readStrictIntKey`（走 `get_type()`，另一条代码形状），两者不是一回事，别混着指望。
 * @param root meshlet 块表（与 readMeshletBlock 同层）
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa/present_mode 同口径带配置文件路径）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyMeshletShadowLodFallbackKey(
	const sol::table& root, RuntimeConfig& cfg, std::string_view path) {
	sol::optional<std::string> raw = root["shadow_lod_fallback"];
	if (!raw)
		return std::nullopt;
	if (*raw == "deepest")
		cfg.meshletShadowLodFallback = MeshletLodFallbackMode::deepest;
	else if (*raw == "l0")
		cfg.meshletShadowLodFallback = MeshletLodFallbackMode::l0;
	else
		return fmt::format("runtime config ({}): meshlet.shadow_lod_fallback must be "
			"\"deepest\" or \"l0\", got \"{}\"",
			path, *raw);
	return std::nullopt;
}

/**
 * `meshlet` 嵌套块解析（★0-b1 簇路径总门 + ★1-a1ii-α debug 档 + ★0.5-b2 coplanar/tint +
 * ★1-a1ii-β-1 switch 原位取代档 + ★1-a2-i cull + ★18-B1 shadow_lod 阴影段选级 +
 * ★M-0 mdi_shadow 真 MDI 档 + **债-105** overlay α 叠加段自己的门）：
 * `meshlet = { enable, overlay, debug, coplanar, tint, switch, cull, shadow_lod,
 * shadow_lod_fallback, mdi_shadow }`。
 * 缺块 / 缺键 = 保持结构体默认（🔴 ★18-C 起 `meshletEnable = true` + `meshletShadowLod = 5`
 * = 出厂即开簇路径与阴影远级联 LOD，2026-10-12 用户拍板。P5「门关 = 逐位不变」**仍是硬护栏**，
 * 只是门关态现在要显式 `enable = false` + `shadow_lod = -1`（实测精确复现旧锚 54e40665…）。
 * 逐键默认值唯一真相源 = RuntimeConfig 成员初始化，本行只列其余形态：`meshletOverlay = false` =
 * α 叠加段不采 draw（债-105，出厂不再白付 1.59ms）；`meshletDebug = false` =
 * 正常叠加档；`meshletCoplanar = off` = 不碰动态状态；`meshletTint = false` = 不选 tint 变体；
 * `meshletSwitch = false` = 不取代；`shadow_lod_fallback = deepest`；`meshletMdiShadow = false`
 * = 不走 MDI（★M-1b 起本键**有消费者**：键开则烘 MDI 条目并回填，宿主条件 → 字段注释）。
 * 形态先例 = readFsrBlock（嵌套块，无扁平键双读兼容）；coplanar / shadow_lod_fallback 两枚
 * 三态键先例 = applyAaKey（未知值 fail-fast，错误经返回值上抛给调用方）。六枚布尔走
 * readStrictBoolKey、shadow_lod 走 readStrictIntKey（类型错 = 启动失败，理由 → 各自注释）。
 * 债-106 已挂号：gate_lint 的 `Ctx` 现含 `RuntimeConfig` 与它的出厂默认，整块 10 枚键逐枚
 * 申报（规则表 → `gate_lint_impl.cppm` 的 meshlet 段；判据出处 = 各字段注释 + `wireMeshlet*`
 * 消费者）。★18-C 的就近 WARNING（`warnIfShadowLodOrphaned`）与叠加仪器的就近 warn
 * （`warnMeshletVariantInstruments`）判据是纯配置组合，已随迁移删除（同一根因两条喊 =
 * 影子真相源）；其余装配层 warn 依赖装配期事实（条目数 / 载体在不在），保留。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标
 * @param path 仅用于错误消息
 * @param trace 键轨迹（十枚键在块入口**无条件**记全 = 覆盖率护栏输入；两枚三态字符串键
 *              由本函数记，不进 applyMeshlet*Key 内部记——每键恰好记一次）
 * @return nullopt = 合法；有值 = 首个错误消息（调用方 fail-fast）
 */
[[nodiscard]] static std::optional<std::string> readMeshletBlock(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("meshlet.enable");
	trace.read.emplace_back("meshlet.overlay");
	trace.read.emplace_back("meshlet.debug");
	trace.read.emplace_back("meshlet.tint");
	trace.read.emplace_back("meshlet.switch");
	trace.read.emplace_back("meshlet.cull");
	trace.read.emplace_back("meshlet.shadow_lod");
	trace.read.emplace_back("meshlet.shadow_lod_fallback");
	trace.read.emplace_back("meshlet.mdi_shadow");
	trace.read.emplace_back("meshlet.coplanar");
	sol::table meshlet = root["meshlet"];
	if (!meshlet.valid() || !meshlet.is<sol::table>())
		return std::nullopt;
	// 顺序 = 键在 configs/runtime.lua 里的出现序；任一失败即返回（fail-fast，不收集后合并）
	if (auto err = readStrictBoolKey(meshlet, "enable", cfg.meshletEnable, path))
		return err;
	// 债-105 α 影子叠加段自己的门（默认 false ⟹ 出厂不再白付 +1.59ms）
	if (auto err = readStrictBoolKey(meshlet, "overlay", cfg.meshletOverlay, path))
		return err;
	if (auto err = readStrictBoolKey(meshlet, "debug", cfg.meshletDebug, path))
		return err;
	// ★0.5-b2 阳性对照变体（与 debug 同真时 tint 优先）
	if (auto err = readStrictBoolKey(meshlet, "tint", cfg.meshletTint, path))
		return err;
	// ★1-a1ii-β-1 原位取代档
	if (auto err = readStrictBoolKey(meshlet, "switch", cfg.meshletSwitch, path))
		return err;
	// ★1-a2-i 逐簇视锥剔除
	if (auto err = readStrictBoolKey(meshlet, "cull", cfg.meshletCull, path))
		return err;
	// ★18-B1 阴影段簇路径选级（-1 = 关 / 0 = 阳性对照 L0 臂 / N≥1 = uniform 选级 D=N）
	if (auto err = readStrictIntKey(meshlet, "shadow_lod", cfg.meshletShadowLod, path))
		return err;
	// ★18-B1 回落策略（三态字符串键，未知值 fail-fast）
	if (auto err = applyMeshletShadowLodFallbackKey(meshlet, cfg, path))
		return err;
	// ★M-0（§19.2 D2）真 MDI 阴影段档（本批零消费者 = 读到也不改变任何一帧行为）
	if (auto err = readStrictBoolKey(meshlet, "mdi_shadow", cfg.meshletMdiShadow, path))
		return err;
	return applyMeshletCoplanarKey(meshlet, cfg, path);
}

/**
 * fail-fast 键链（★0.5-b2，认知复杂度预算）：把「解析失败返回错误消息」的键串成一条，
 * `loadRuntimeConfig` 只留一个 if+return（两处各一个曾把认知复杂度顶到 16>15）。
 * 新增 fail-fast 键 = 在此加一行，不再往 loadRuntimeConfig 里添 if。
 * 顺序契约：applyAaKey 必须先于 readFsrBlock（aa=fsr 隐含打开速度场在其内一行）；
 * readMeshletBlock 只读 meshlet 块、与其他键零耦合，链尾即可。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标
 * @param path 仅用于错误消息
 * @param trace 键轨迹（透传给链上各键）
 * @return nullopt = 全部合法；有值 = 首个错误消息
 */
[[nodiscard]] static std::optional<std::string> applyFailFastKeys(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	if (auto err = applyAaKey(root, cfg, path, trace))
		return err;
	return readMeshletBlock(root, cfg, path, trace);  // ★0-b1 簇路径总门 + ★0.5-b2 coplanar 三态键
}

/**
 * `present_mode` 键解析（批 9.3 present 模式偏好；抽函数 = loadRuntimeConfig 认知
 * 复杂度红线，先例 applyAaKey）。缺键 = 保持默认 immediate；未知值 = 返回错误消息由
 * 调用方 fail-fast（与类型错误同级，不静默回落）。
 * @param root runtime.lua 顶层表
 * @param cfg  写入目标（仅在键存在且合法时被写）
 * @param path 仅用于错误消息（与 aa 同口径带配置文件路径）
 * @param trace 键轨迹（缺键也记 = 解析器读过它，覆盖率护栏的输入）
 * @return nullopt = 合法（含缺键）；有值 = 错误消息
 */
[[nodiscard]] static std::optional<std::string> applyPresentModeKey(const sol::table& root,
	RuntimeConfig& cfg, std::string_view path, RuntimeKeyTrace& trace) {
	trace.read.emplace_back("present_mode");
	sol::optional<std::string> presentMode = root["present_mode"];
	if (!presentMode)
		return std::nullopt;
	if (*presentMode == "immediate")
		cfg.presentMode = PresentModePref::immediate;
	else if (*presentMode == "fifo")
		cfg.presentMode = PresentModePref::fifo;
	else
		return fmt::format(
			"runtime config ({}): present_mode must be \"immediate\" or \"fifo\", got \"{}\"",
			path, *presentMode);
	return std::nullopt;
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
