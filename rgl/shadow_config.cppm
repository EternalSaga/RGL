export module shadow_config;

import rlstd;
import vkerror;

namespace RGL {

// 批 N6a（docs/shadow_routes_plan.md §2 定案）：阴影路线配置（configs/shadow.lua）。
// 一个枚举（软影滤波轴）+ 每档自己的调参块 + 一个正交的 IZB 修饰（硬影 mask）。
// **N6a 过渡期**：本模块只解析 + 回显，**不回灌**——6 处消费点仍读 diagnostics() 的
// 旧字段（旧字段默认值 = 交付形态，旧键已从 configs/diagnostics.lua 删除并 fail-fast）。
// 消费点切到新门 = N6b（本文件不抄形态决策表，红线 4）。

/**
 * 阴影**软影滤波轴**（configs/shadow.lua 的 `mode`，四值互斥）。
 *
 * IZB **不进枚举**（docs/shadow_routes_plan.md §2 红线 1）：交付路径就是 csm + izb
 * 同时开，且「IZB 近场细影 + 远场软影」混接要求两者共存 ⟹ 枚举管软影滤波、`izb` 管
 * 硬影 mask，两条轴正交。
 *
 * 失效形态决定未知值必须 fail-fast（`cull_replay` 同一条纪律）：拼错的值若静默回落
 * 默认档，读数就变成"你以为在测 EVSSM，其实跑的是 CSM"，而两边画面看起来都"合理"。
 */
export enum class ShadowMode {
	none,  ///< 无阴影（旧门 shadow_pass=false 的对应形态）
	csm,   ///< 默认：级联阴影 + 4 抽头 PCF（交付形态）
	pcss,  ///< 批 11.5：接触硬化软阴影（blocker 搜索 + 变半径加权 PCF）
	evssm, ///< 批 11.6：指数级联阴影矩（远场软影；生效性边界 → docs/shadow_routes_plan.md §4）
};

/**
 * 反查 `ShadowMode` 的配置名（与 `mode` 取值表一致，回显/日志/报错共用；
 * tests/test_scene_config.cppm 的「回显 = 配置名」断言钉住与 parseShadowMode 同集合）。
 * @param mode 阴影路线
 * @return 配置名字符串
 */
export const char* shadowModeName(ShadowMode mode);

/**
 * `mode` 字符串 → 枚举（**未知值返回 error 并列出全部合法值，不静默回落**）。
 * @param name 配置里的字符串值
 * @return 枚举值；非法串返回 ConfigLoadFailed（message 列出 "none"/"csm"/"pcss"/"evssm"）
 */
export [[nodiscard]] std::expected<ShadowMode, Error> parseShadowMode(std::string_view name);

/**
 * 软影调参块（pcss / evssm 两档各持一份，**按档分立**——消掉旧门「两套门共用一套
 * `pcss_*`」的债-16⑤）。clamp 口径沿用旧 `pcss_*` 三键（diagnostics_impl 既有实现）：
 * sun_degrees clamp [0, 5]，两个半径 clamp [1, 64]（成本护栏）。
 */
export struct ShadowTuning {
	double sunDegrees = 0.53;   ///< 太阳视直径（度；物理真值 0.53；写大 = 艺术化夸张半影）
	double searchRadius = 16.0; ///< blocker 搜索半径上限（图集纹素；clamp [1, 64]）
	double maxPenumbra = 32.0;  ///< 半影半径上限（图集纹素；clamp [1, 64]）
};

/**
 * 阴影配置（configs/shadow.lua → ShadowConfig）。**代码默认 = 交付形态**
 * （mode=csm、izb=true——「承认 IZB 出厂开」的落点，docs/shadow_routes_plan.md §1 拍板）。
 * izbGrid / izbDumpFrame 是 izb 子块的诊断两键（语义沿用旧 izb_grid / izb_dump_frame）。
 */
export struct ShadowConfig {
	ShadowMode mode = ShadowMode::csm;  ///< 软影滤波轴（四值互斥）
	bool izb = true;                     ///< 正交修饰：光空间硬影 mask（要求 mode != none，
	                                      ///< 该组合门由 gate_lint 在 N6b 接管，本批不接）
	uint32_t izbGrid = 512;              ///< 光空间 IZB 网格边长 G（izb.grid；clamp [64, 2048]）
	uint32_t izbDumpFrame = 0;           ///< 第 N 帧起回读 head 直方图打日志（izb.dump_frame；0=关）
	ShadowTuning pcss{};                 ///< PCSS 档调参（mode=pcss 时消费）
	ShadowTuning evssm{};                ///< EVSSM 档调参（mode=evssm 时消费）
};

/**
 * 解析器键轨迹（同 `DiagnosticsKeyTrace` 的用途：回显覆盖率护栏 + 未知键 lint 输入）。
 * 键名书写与 Lua 路径一致，子表字段用 `表名.字段`（如 `pcss.sun_degrees`）。
 */
export struct ShadowKeyTrace {
	std::vector<std::string> read;     ///< 解析器读过的键（解析器总是全量问一遍）
	std::vector<std::string> present;  ///< 文件里实际出现的顶层键名（子表只记表名）
};

/**
 * 回显表的全部键名（与 `ShadowKeyTrace::read` 同拼写）——单测据此做覆盖率断言，
 * 表本体在实现单元（格式化要用 fmt，不把 fmt 拉进本模块接口）。
 * @return 键名列表（顺序 = 回显顺序）
 */
export std::vector<std::string> shadowConfigEchoKeys();

/**
 * 门回显：只列**偏离默认值**的键（`key=value` 空格分隔）——启动日志 info 级用。
 * @param cfg 解析后的配置
 * @param defaults 比较基准（调用方传 `ShadowConfig{}`）
 * @return 一行文本；全默认时返回空串（调用方据此跳过打印）
 */
export std::string describeShadowGates(const ShadowConfig& cfg, const ShadowConfig& defaults);

/**
 * 门回显：全量键（`debug` 级用；排障时"这台机器的阴影门到底是什么状态"的一行真相）。
 * @param cfg 解析后的配置
 * @return 一行文本（`key=value` 空格分隔，顺序 = 回显表顺序）
 */
export std::string describeAllShadowGates(const ShadowConfig& cfg);

/**
 * 从 Lua 加载阴影配置（sol2 沙箱同 diagnostics：缺文件 = 回落交付默认 + warn；
 * 脚本/类型错误 = unexpected；`mode` 非法值 = unexpected 并列出合法值）。
 * 纯解析（不回灌任何单例），N6a 的 main 只打日志。
 * @param path 配置文件路径
 * @param trace 可选出参：键轨迹（覆盖率护栏与未知键 lint 的输入；传 nullptr = 不记录）
 * @return 解析好的 ShadowConfig；失败返回 Error
 */
export [[nodiscard]] std::expected<ShadowConfig, Error> loadShadowConfig(
	std::string_view path, ShadowKeyTrace* trace = nullptr);

}  // namespace RGL
