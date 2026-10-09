export module diagnostics;

import rlstd;
import vkerror;

namespace RGL {

/**
 * 遮挡剔除结果的**驱动方式**（configs/diagnostics.lua 的 `cull_replay`，债-30 方案 C）。
 *
 * 为什么是三态而不是布尔：中间档 `gpuLagged` 不是过渡装饰，它**同时解两个死结**——
 * GPU 集要驱动本帧 draw 就必须本帧读回（那是硬同步，正是债-30 的账），而剔除结果
 * 一旦滞后一帧，CPU 对照表若也跟着滞后就**永远比不过**（对账失去意义）。滞后档让
 * CPU 仍驱动 draw、GPU 集照常跑并逐帧与「提交那一帧」的 CPU 表对账 ⟹ 想保留 GPU 读回
 * 路的真机持续验证时用它，不必退回每帧硬同步（默认 `cpu` 档下 GPU 集根本不跑）。
 *
 * 失效形态决定这里必须 fail-fast：拼错的值若静默回落默认档，读数就变成"你以为在测
 * GPU 路，其实跑的是 CPU 路"（`present_mode` / `aa` 的同一条纪律）。
 */
export enum class CullReplayMode {
	cpu,       // 默认：CPU 剔除驱动 draw，GPU 集整条不上 CB（省 dispatch + 读回拷贝 + 一次提交）
	gpuLagged, // GPU 集照跑、结果滞后一帧收割并与上一帧的 CPU 表对账；draw 仍由 CPU 驱动
	gpuSync,   // 本帧等读回并由 GPU 集驱动 draw（现行主路形态，预期含硬同步）
};

/**
 * 反查 `CullReplayMode` 的配置名（与 `cull_replay` 取值表一致，回显/日志用）。
 * @param mode 剔除驱动方式
 * @return 配置名字符串
 */
export const char* cullReplayName(CullReplayMode mode);

/**
 * 诊断开关（configs/diagnostics.lua，全可选回落默认 = 全关）：散落在低层模块深处的
 * 调试旋钮，经**单例**承载（仿 RLLogger::getInstance，见 logger/）——避免给
 * uploadTexture/loadEmbedded/AppContext/Renderer 等一串签名加参数。main 启动最早期
 * loadDiagnosticsConfig → setDiagnostics 填充，此后只读。
 * 这些多为历史排查（4K 黑屏 UAF、可见窗 dmabuf fault、sRGB 嫌疑）留下的门，默认全关
 * 不影响正常路径；env 机制已于 2026-09-08 全面转配置文件（用户偏好：不用环境变量）。
 */
export struct DiagnosticsConfig {
	bool noVvl = false;                 // 关 VVL（descriptor heap 双写排查，vkinstance）
	bool noHdr = false;                 // 强制 SDR surface 格式（HDR 后缓冲 fault 排查，vulkan_window）
	bool noPresent = false;             // 渲染但永不 present（可见窗 dmabuf fault 排查，vkrenderer）
	bool vtxCanary = false;             // 顶点上传回读金丝雀（model_importer uploadModelMeshes）
	uint64_t texelCanary = ~uint64_t{0};  // 帧 N 重读白纹理比对（ecs renderSystem；MAX = 关）
	int dumpGbuf = -1;                  // deferred 截图路径直读第 N 个 G-buffer 通道（llm_debug；-1 = 关）
	bool styleDist = false;             // per-primitive 风格分布日志（main entities 循环）
	bool texDist = false;               // per-primitive 纹理 slot 分布日志（main entities 循环）
	uint32_t textureForceSize = 0;      // 近邻缩放强制 N×N（texture 上传路径；0 = 关）
	bool textureForceWhite = false;     // 纹理全白填充（"均匀内容不 fault"假设）
	bool textureForceUnorm = false;     // upload 强制 UNORM（sRGB 嫌疑隔离）
	bool dgcStatic = false;             // RADV 静态 shader DGC 路（staticGraphicsIndirect 设备的 spike 门，默认关，见 docs/archive/radv_static_dgc_spike.md）
	CullReplayMode cullReplay = CullReplayMode::cpu;  // 剔除结果驱动方式（cull_replay；三态语义 → `CullReplayMode` 注释，方案与定价 → docs/cpu_perf_plan.md §4）。默认 cpu = 交付路径不让 CPU 等 GPU 读回
	uint32_t frameStats = 0;            // 帧时间统计：每 N 帧打一行 avg/min/max（0 = 关；headless 吞吐 ≈ GPU-bound）
	uint32_t profiler = 0;              // Phase 9 zone profiler 帧窗口 N（0 = 关；最近 N 帧 → 退出 dump trace+stats，docs/phase9_plan.md）
	bool imgui = false;                 // Phase 10 IMGUI 调试面板总开关（帧末 UI 段 + 输入拦截；headless 无窗天然 no-op，docs/archive/phase10_plan.md）
	bool configReload = false;          // Phase 10 批 10.3 配置热重载总开关（帧边界轮询源 configs/：scene/camera/log_level 的 T1 字段；imgui 面板开关本身不可热载=需重建后端，docs/archive/phase10_plan.md）
	bool shaderReload = false;          // Phase 10 批 10.4 shader 热重载 L1（帧边界 fork slangc 重编源 shaders/ + 同签名守卫换入；slangc/flags/源目录缺失降级关，docs/archive/phase10_plan.md）
	bool picking = false;               // Phase 10 批 10.5 GPU id 点选（编译图期装饰 object_id pass，D10 默认关 = 图逐位同；面板点中实体 → 参数上下文，docs/archive/phase10_plan.md）
	// ── 阴影字段组（批 N6a）：configs/diagnostics.lua 的旧键已删除且出现即启动失败，
	//    配置面 = configs/shadow.lua（rgl/shadow_config.cppm）；N6a 过渡期本组字段仍由
	//    「默认值 = 交付形态」提供、仍是 6 处消费点的真相（消费点切到新门 = N6b）。
	bool izbShadow = true;             // 批 11.3-1d：HFTS/FTIZB 硬阴影（deferred 图外 IZB 创建 dispatch；docs/hfts_izb_plan.md）。**N6a 起默认开 = 交付形态**（「承认 IZB 出厂开」落点，docs/shadow_routes_plan.md §1 拍板；旧键 izb_shadow 已从 configs/diagnostics.lua 删除并 fail-fast，本字段仍是消费点真相直到 N6b）
	uint32_t izbGrid = 512;             // 光空间 IZB 网格边长 G（izb_grid；clamp [64, 2048]）
	uint32_t izbDumpFrame = 0;          // 第 N 帧起每 30 帧回读 head 直方图打日志 + 覆盖写 izb_mask.ppm（izb_dump_frame；0=关）——1d 冒烟阳性锚点，N+1 帧起输出
	bool shadowPass = true;             // Phase 11 批 11.2：定向光 CSM 阴影总开关（**5a-9 起默认开**，configs/diagnostics.lua 同值）。true = 图首 4 个 depth-only shadow pass 画投影体进 2048² 图集（4 级各占 1024² 块）+ 采样端 4 抽头 PCF + 末级 shadowFar 淡出；false = 阴影段零 draw + 采样端 enabled=0 全亮 → 画面逐位不变（回归对照用）。本默认值与 config 同为"开"：缺文件回落也走有影路径（早期"段内零 draw = 逐位不变"的中间态已结束，见 docs/phase11_plan.md §10/§12.8）
	bool shadowPcss = false;            // 批 11.5：PCSS 接触硬化软阴影（门 = 选 -DPCSS_SOFT 变体 frag SPV，同 1e IZB 红线构造性保证：关 = 原版 4 抽头 PCF 逐位不变；开 = blocker 搜索 + 变半径加权 PCF，半影宽 = Δz·tanθ 物理接触硬化）
	bool shadowEvssm = false;           // 批 11.6：EVSSM 远场软阴影（门 = 构造 EvssmShadow 矩图链 + 选 -DEVSSM_SOFT 变体；数值键复用 pcss_sun_degrees/pcss_search_radius/pcss_max_penumbra（docs/evssm_plan.md §4.1）；与 shadowPcss 同开 = EVSSM 优先（11.5 冻结件让位）；关 = 零构造零变体 = 逐位红线）
	double pcssSunDegrees = 0.53;       // 太阳视直径（度；物理真值 0.53，写大 = 艺术化夸张半影；clamp [0, 5]）
	double pcssSearchRadius = 16.0;     // blocker 搜索半径上限（图集纹素；clamp [1, 64]，成本护栏）
	double pcssMaxPenumbra = 32.0;      // 半影半径上限（图集纹素；clamp [1, 64]，成本护栏）
	uint32_t pickingProbeFrame = 0;     // 脚本化点选探针（同 texel_canary headless 驱动方法论）：第 N 帧（1 起）自动 requestProbe(x,y) 一次；0 = 关
	uint32_t pickingProbeX = 0;         // 探针像素列（窗口/RT 坐标）
	uint32_t pickingProbeY = 0;         // 探针像素行
	uint32_t uiClickProbeFrame = 0;     // 脚本化 UI 点击探针（窗口模式）：第 N 帧（0 起，同 picking_probe 实际语义）合成一次鼠标左键点击 (x,y) 灌入完整输入链；0 = 关（2026-09-28 WASD 回归场景复现用，见 git:调试历史）
	uint32_t uiClickProbeX = 0;         // 点击像素列（窗口坐标）
	uint32_t uiClickProbeY = 0;         // 点击像素行（窗口坐标）
	uint32_t cameraProbeFrame = 0;      // 脚本化相机键盘探针（窗口模式）：第 N 帧（0 起）按住移动键 holdFrames 帧，验证 eventSystem→InputFilter→cameraSystem 输入链并打 PASS/FAIL；0 = 关（2026-09-28 "面板吞 WASD" 回归护栏）
	uint32_t cameraProbeKey = 0;        // 探针 SDL 键码（须为 Camera.keys 绑定的 W/A/S/D 之一；lua 里写单字母，解析期映射）
	uint32_t cameraProbeHold = 0;       // 按键保持帧数（<1 解析期回落 8）
	bool rpc = false;                   // Phase 10b HTTP POST RPC 通道（只绑 127.0.0.1；独立于 imgui 门，headless 可用，docs/phase10b_plan.md）
	uint32_t rpcPort = 8788;            // RPC 监听端口（bind 失败 = 日志降级关，不崩）
};

/**
 * 单例访问器（main setDiagnostics 后，深栈点 diagnostics() 只读）。
 * @return 进程级 DiagnosticsConfig 引用
 */
export DiagnosticsConfig& diagnostics();
/**
 * 填充诊断单例（main 启动期调用一次，此后只读）。
 * @param cfg 解析好的诊断配置
 */
export void setDiagnostics(const DiagnosticsConfig& cfg);

/**
 * 解析器键轨迹（2026-10-02 门回显派生的覆盖率护栏输入，docs/gate_lint_plan.md §3-B）：
 * 记录解析器**实际读过**的键与 Lua 文件里**实际出现**的顶层键。两个用途：
 *   ① 单测断言「读过的键集合 == 回显表键集合」——漏一个方向都红（回显抄漏 = 撒谎行，
 *      回显多写 = 陈旧键）；
 *   ② 未知键检查：文件里出现而解析器不读 = 拼错的键，今天**完全静默**。
 * 键名书写与 Lua 路径一致，子表字段用 `表名.字段`（如 `camera_probe.key`）。
 */
export struct DiagnosticsKeyTrace {
	std::vector<std::string> read;     ///< 解析器读过的键（含未出现在文件里的——解析器总是全量问一遍）
	std::vector<std::string> present;  ///< 文件里实际出现的顶层键名（子表只记表名）
};

/**
 * 回显表的全部键名（与 `DiagnosticsKeyTrace::read` 同拼写）——单测据此做覆盖率断言，
 * 表本体在实现单元（格式化要用 fmt，不把 fmt 拉进本模块接口）。
 * @return 键名列表（顺序 = 回显顺序）
 */
export std::vector<std::string> diagnosticsEchoKeys();

/**
 * 门回显：只列**偏离默认值**的门（`key=value` 空格分隔）——启动日志 `info` 级用。
 * 基准 = 入参 `defaults`（调用方传 `DiagnosticsConfig{}`），不是"文件里写了没有"：
 * `configs/diagnostics.lua` 显式写着 30 个键且多数等于默认值，按"写了"喊就是噪音。
 * @param cfg 解析后的配置
 * @param defaults 比较基准（默认构造 = 全默认）
 * @return 一行文本；全默认时返回空串（调用方据此跳过打印）
 */
export std::string describeDiagnosticsGates(const DiagnosticsConfig& cfg,
	const DiagnosticsConfig& defaults);

/**
 * 门回显：全量 39 键（`debug` 级用；排障时"这台机器的门到底是什么状态"的一行真相）。
 * @param cfg 解析后的配置
 * @return 一行文本（`key=value` 空格分隔，顺序 = 回显表顺序）
 */
export std::string describeAllDiagnosticsGates(const DiagnosticsConfig& cfg);

/**
 * 从 Lua 加载诊断配置（sol2 沙箱同 camera_config：缺文件 = 全默认 + warn；类型错误 = unexpected）。
 * 纯解析（不触碰单例），main 负责 setDiagnostics(*result)。
 * @param path 配置文件路径
 * @param trace 可选出参：键轨迹（覆盖率护栏与未知键 lint 的输入；传 nullptr = 不记录）
 * @return 解析好的 DiagnosticsConfig；脚本/类型错误返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<DiagnosticsConfig, Error> loadDiagnosticsConfig(
	std::string_view path, DiagnosticsKeyTrace* trace = nullptr);

}  // namespace RGL
