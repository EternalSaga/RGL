export module shadow_config;

import rlstd;
import vkerror;

namespace RGL {

// 批 N6a（docs/shadow_routes_plan.md §2 定案）：阴影路线配置（configs/shadow.lua）。
// 一个枚举（软影滤波轴）+ 每档自己的调参块 + 一个正交的 IZB 修饰（硬影 mask）。
// **N6b 起本模块是阴影门的唯一真相源**：`shadowConfig()` 单例承载解析结果，6 处消费点
// 一律经 `shadowFormPlan()`（下面的形态决策表）读形态，`DiagnosticsConfig` 的旧阴影字段
// 已删除（旧键出现在 diagnostics.lua 仍 fail-fast = 永久护栏 `checkMigratedShadowKeys`）。
// 本文件不抄映射表（红线 4：表只住 `shadowFormPlan` 的实现体，抄一次漂一次）。

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
	/**
	 * 滤波核**宽度下限**（图集纹素；clamp [0, 64]；**0 = 不设下限**）。
	 *
	 * 批 N6d。出厂太阳角 0.53° 下 `penumbraTexels < 1` ⟹ 抽点半径 `r ≈ 0.5` 纹素，5 个抽点
	 * 全落进同一个 2×2 双线性邻域（实测：单抽头变异与基线**逐位相同**、太阳角敏感度**恰好
	 * 0.000%**）⟹ 核根本没张开，EVSSM 退化成「单纹素深度比较」= CSM 去掉 PCF。
	 * **默认 16 = 实测唯一贴上 CSM 形态的点**（带核比 1.000、每影像素深度 50 vs CSM 52 LSB）；
	 * 权衡表（覆盖 ↔ 不透明度单调互换）逐档读数在 `rgl/shaders/shadow.slang` STEP 2.5 注释。
	 * ⚠ 曾有第二个键 `coherence_texels`（跨断崖抽点一致性闸门），**实测净负作用**
	 *   （foot=32 时覆盖率比 0.845 → 0.526）已删；归因 → `docs/shadow_routes_plan.md` §4.2。
	 */
	double minFootprintTexels = 16.0;
};

/**
 * 阴影配置（configs/shadow.lua → ShadowConfig）。**代码默认 = 交付形态**
 * （mode=csm、**izb=false**）。izbGrid / izbDumpFrame 是 izb 子块的诊断两键（语义沿用旧
 * izb_grid / izb_dump_frame）。
 *
 * ### izb 默认 true → false（2026-10-10，用户放行换红线）
 *
 * 这里曾经是 `true`，理由只是「承认出厂开」（§1 拍板 3）——**那是记录现状，不是质量裁决**。
 * 实测两个结构性缺陷（读数与复现配方 → `docs/shadow_routes_plan.md` §7）：
 *
 * 1. **随相机爬行的接缝**：IZB 只覆盖 **cascade-0 光空间盒**（`izb_create.comp.slang` 头注自陈
 *    「v1 只覆盖 split0 范围…HFTS 分区留阶段 2」），盒外回落 `sampleShadow1Tap`（CSM 4 抽头 PCF）。
 *    两套算法在边界两侧给出**不同答案** ⟹ 边界成了一条在世界表面上爬的线。三色哨兵（盒内全黑 /
 *    盒外全白）一步定死：实测那条蓝色裂缝**正好压在盒边界上**。默认机位实测 **1.76% 屏幕判反**，
 *    亮度差中位 **16 LSB**、p95 41、最高 **160 LSB**（8 LSB 是本仓"刚好可觉"口径）。
 * 2. **管辖范围内系统性欠遮挡**：`imgqa izb` 自己打印的 `INFO 变亮（CSM 遮而 IZB 放）`=
 *    **405048 px**（rig，占屏 44%）。这条此前被登记成 INFO 放过去 = 判据记录了缺陷又放它过。
 *
 * 关掉是**测出来的选择**，不是妥协：CSM 单独一档已被 `imgqa swim` 证明影场世界不变（零位移失配
 * 0/63835），且 `light.frag.slang` 头注记着「同一太阳下 IZB 遮挡集 ⊆ CSM 遮挡集」⟹ 拿 CSM 单独
 * 作源不丢遮挡（它遮的是超集），只丢那条缝。**IZB 代码路径一律保留**（`mode` 与 `izb` 正交，
 * 开回来仍可用），gate_lint 对 `izb.on = true` 出 **warn** 把上面两个读数带在消息里，防止被随手打开。
 */
export struct ShadowConfig {
	ShadowMode mode = ShadowMode::csm;  ///< 软影滤波轴（四值互斥）
	bool izb = false;                   ///< 正交修饰：光空间硬影 mask（要求 mode != none）。
	                                      ///< **默认 false 的理由见上方整段**，别当笔误改回 true
	uint32_t izbGrid = 512;              ///< 光空间 IZB 网格边长 G（izb.grid；clamp [64, 2048]）
	uint32_t izbDumpFrame = 0;           ///< 第 N 帧起回读 head 直方图打日志（izb.dump_frame；0=关）
	ShadowTuning pcss{};                 ///< PCSS 档调参（mode=pcss 时消费）
	ShadowTuning evssm{};                ///< EVSSM 档调参（mode=evssm 时消费）
};

/**
 * 形态决策表（**唯一真相源**，docs/shadow_routes_plan.md §2 红线 4）：`mode` + `izb` →
 * 各消费点要的那四件事 + 本档调参。
 *
 * 为什么要有这张表：改造前"这条阴影路到底跑不跑"散在 6 处消费点各写一遍布尔组合
 * （`shadowEvssm && shadowPass`、`shadowEvssm 优先 / shadowPcss 次之` 的让位链…），
 * 加一档就要改六处、漏一处就是"配置写了、什么都没发生"。枚举 + 单点查表把选路判断
 * 收成一处：两门同开这类**根本不该存在**的组合从此表达不出来（让位链不是被"修好"，是被
 * 消掉）。`gate_lint` / `shader_registry` / `Renderer` 三处从它读，文档不抄（红线 4）。
 *
 * ⚠ **生命周期**：`tuning` 是入参 `cfg` 的引用 ⟹ 调用方必须让 `cfg` 活得比返回值久。
 * 消费点一律传 `shadowConfig()`（进程级单例），天然满足；**别拿临时量调它**。
 */
export struct ShadowFormPlan {
	bool drawAtlas = false;          ///< 图首 shadow0..3 段画不画投影体（= 旧 shadow_pass）
	std::string_view spvSuffix;      ///< "" | "_PCSS_SOFT" | "_EVSSM_SOFT"（采样 frag 变体后缀）
	bool buildEvssmMoments = false;  ///< 构不构造 EvssmShadow 矩图链
	bool consumeIzbMask = false;     ///< light 条目选不选 IZB_CONSUME 变体 / 构不构造 IzbShadow
	// **刻意没有 `tuning` 字段**（N6b 主 agent 裁决）：调参按档分立靠**消费点各读各的块**
	// （render_list 的 pcss 区读 `shadowConfig().pcss`、evssm 区读 `.evssm`），而不是靠
	// "按当前档挑一块"——两个 scene data 区同时被写、各自只有一个变体会读，"本档的那一块"
	// 这个量在运行期**没有消费者**；而返回 `const ShadowTuning&` 还是把决策表变成了
	// 悬垂引用发生器（`shadowFormPlan(ShadowConfig{})` 即悬空）。死值调参由 gate_lint 的
	// inertNote 喊（`pcss.*` 偏离默认但 mode≠pcss），不靠这里。
};

/**
 * 查形态决策表（纯函数，不读单例 ⟹ `just test` 毫秒级可测）。
 * @param cfg 阴影配置（消费点传 `shadowConfig()`；返回值引用它，见 `ShadowFormPlan` 生命周期条）
 * @return 本形态的四件事 + 本档调参
 */
export ShadowFormPlan shadowFormPlan(const ShadowConfig& cfg);

/**
 * 单例访问器（main setShadowConfig 后，深栈点 shadowConfig() 只读）。
 * 与 `diagnostics()` 同一条理由：消费点散在 render_list / shader_registry / Renderer /
 * main_scene 深处，给这一串签名加参数不值当（AGENTS「单例承载诊断/形态门」范式）。
 * @return 进程级 ShadowConfig 引用
 */
export ShadowConfig& shadowConfig();

/**
 * 填充阴影配置单例（main 启动期调用一次，此后只读——同 setDiagnostics）。
 * @param cfg 解析好的阴影配置
 */
export void setShadowConfig(const ShadowConfig& cfg);

/**
 * 解析器键轨迹（同 `DiagnosticsKeyTrace` 的用途：回显覆盖率护栏 + 未知键 lint 输入）。
 * 键名书写与 Lua 路径一致，子表字段用 `表名.字段`（如 `pcss.sun_degrees`）。
 */
export struct ShadowKeyTrace {
	std::vector<std::string> read;     ///< 解析器读过的键（解析器总是全量问一遍）
	std::vector<std::string> present;  ///< 文件里实际出现的键名：顶层 + 固定字段块下钻一层的 `块.子键`（债-129 第三刀；开放 map 只记表名）
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
