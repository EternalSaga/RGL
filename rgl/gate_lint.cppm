export module gate_lint;

import rlstd;
import diagnostics;
import shadow_config;          // ShadowConfig（N6b：阴影门与 diagnostics 门同表申报）
import render_graph;         // RgPassPlan / graphHasDeferredLight
import render_graph_config;  // RgGraphVariant

namespace RGL {

/**
 * 门组合自检（docs/gate_lint_plan.md §3-C）：**纯函数**申报式规则表——「配了但无效」
 * 的判定从每帧录制路径（passrec warn-once）搬到启动期一次跑完。
 * 不碰设备、不碰单例（一切事实作入参 → `just test` 毫秒级可测）。
 *
 * 三条防腐条款（判据纪律，违反任何一条的改动都应被拒）：
 *   1. 判据来自**申报**（每门一行谓词），不来自组合枚举、不来自字符串/子串匹配
 *      （validateEnvironmentContract 的 contains 误杀史：main.cppm:125-137）；
 *   2. 每条规则必有「合法组合不得喊」的反向用例（tests/test_gate_lint.cppm）；
 *   3. 禁止把实现细节写成断言（pass 下标/名字表/"tonemap 必须末位"式契约）。
 */

/** findings 分级（§4 判据表：不是「哪个门」而是「哪一类」）。 */
export enum class GateSeverity : uint8_t {
	inertNote,  ///< 设计上合法的惰性（debug 级；喊它 = 教人不信任日志）
	warn,       ///< 部分生效 / 被让位 / 设备能力不符
	error       ///< 明确开了 X 而 X **必然完全无效**，且没有其他任何信号
};

/**
 * lint 的「世界」侧事实（调用方填；测试自由构造）。
 * 刻意**不含**设备资源存在性（阴影图槽等 = 运行期事实，留 passrec），也刻意不含
 * config_reload / shader_reload 的前提（源 configs 目录 / slangc 缺失 = 消费者
 * wireHotReload / ShaderHotReloader::init 已有各自降级 warn，重复喊 = 影子真相源）。
 */
export struct GateEnv {
	bool headless = false;                  ///< runtime.lua headless（输入探针/ imgui 的消费前提）
	bool llmCapture = false;                ///< runtime.lua llm_debug（dump_gbuf 的消费方）
	bool staticGraphicsIndirect = false;    ///< 设备能力（dgc_static 的门槛）
};

/** 一条 lint 结论（gate = 两份回显表的键名拼写，含 shadow.lua 的 `表.字段` 形态）。 */
export struct GateFinding {
	std::string_view gate;                    ///< 涉事门（"variant" = 意图/图事实交叉校验）
	GateSeverity severity = GateSeverity::warn;
	std::string message;
};

/**
 * 跑全量规则表（41 门 = diagnostics 侧 30 + shadow 侧 11，一门可多条规则——两个根因各
 * 一行消息；覆盖率钉去重门数 + variant 交叉校验）。
 * 判据基准：bool 门 = true 才参与判定；数值/字符串门 = **偏离 defaults** 才参与
 * （configs/diagnostics.lua 显式写着 ~30 个默认值键，按「写了」喊就是噪音）。
 * 依赖方向：lint 不读单例（`diagnostics()` / `shadowConfig()`）——两份配置与各自的
 * 默认值都是入参，测试可任意构造「假的世界」。
 * @param cfg 生效的 diagnostics 配置
 * @param defaults diagnostics 比较基准（正常调用方传 `DiagnosticsConfig{}`）
 * @param shadow 生效的阴影配置（configs/shadow.lua）
 * @param shadowDefaults 阴影比较基准（正常调用方传 `ShadowConfig{}`）
 * @param passes 编译图 pass 序列（deferred 判据 = graphHasDeferredLight，图事实层）
 * @param variant 配置层路线意图（与图事实的**分歧**在此交叉校验，不互相抹平）
 * @param env 设备/模式事实
 * @return findings（error 排前不保证——由 logGateFindings 按级别分发）
 */
export std::vector<GateFinding> lintDiagnosticsGates(const DiagnosticsConfig& cfg,
	const DiagnosticsConfig& defaults, const ShadowConfig& shadow,
	const ShadowConfig& shadowDefaults, std::span<const RgPassPlan> passes,
	RgGraphVariant variant, const GateEnv& env);

/**
 * 规则表覆盖的门名（= 覆盖率护栏输入；单测断言其与 `diagnosticsEchoKeys()` +
 * `shadowConfigEchoKeys()` 的并集相等——加门不答「生效前置是什么」，`just test` 红，
 * 同 pass_vocabulary 的收口范式）。
 * @return 门名列表（顺序 = 表序）
 */
export std::vector<std::string> gateLintRuleGates();

/**
 * findings → 日志分发（error/warn/inertNote → logger 的 error/warn/debug）。
 * lint **不阻止启动**（诊断门不该让程序起不来）；error 计数仅作摘要用。
 * @param findings lintDiagnosticsGates 的产物
 * @return error 级条数
 */
export uint32_t logGateFindings(std::span<const GateFinding> findings);

}  // namespace RGL
