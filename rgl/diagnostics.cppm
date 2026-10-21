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
	uint32_t profiler = 0;              // Phase 9 zone profiler 帧窗口 N（0 = 关；最近 N 帧 → 退出 dump trace+stats，docs/archive/phase9_plan.md）
	bool imgui = false;                 // Phase 10 IMGUI 调试面板总开关（帧末 UI 段 + 输入拦截；headless 无窗天然 no-op，docs/archive/phase10_plan.md）
	bool configReload = false;          // Phase 10 批 10.3 配置热重载总开关（帧边界轮询源 configs/：scene/camera/log_level 的 T1 字段；imgui 面板开关本身不可热载=需重建后端，docs/archive/phase10_plan.md）
	bool shaderReload = false;          // Phase 10 批 10.4 shader 热重载 L1（帧边界 fork slangc 重编源 shaders/ + 同签名守卫换入；slangc/flags/源目录缺失降级关，docs/archive/phase10_plan.md）
	bool picking = false;               // Phase 10 批 10.5 GPU id 点选（编译图期装饰 object_id pass，D10 默认关 = 图逐位同；面板点中实体 → 参数上下文，docs/archive/phase10_plan.md）
	// ── 阴影字段组：**已于批 N6b 整体删除**（原 9 字段：shadow_pass / shadow_pcss /
	//    shadow_evssm / pcss_* ×3 / izb_shadow / izb_grid / izb_dump_frame）。配置面与
	//    消费点真相统一 = configs/shadow.lua（rgl/shadow_config.cppm 的 ShadowConfig +
	//    shadowFormPlan）。旧键出现在 diagnostics.lua 仍由 checkMigratedShadowKeys
	//    fail-fast（永久护栏，指路新键路径）。
	uint32_t pickingProbeFrame = 0;     // 脚本化点选探针（同 texel_canary headless 驱动方法论）：第 N 帧（1 起）自动 requestProbe(x,y) 一次；0 = 关
	uint32_t pickingProbeX = 0;         // 探针像素列（窗口/RT 坐标）
	uint32_t pickingProbeY = 0;         // 探针像素行
	uint32_t uiClickProbeFrame = 0;     // 脚本化 UI 点击探针（窗口模式）：第 N 帧（0 起，同 picking_probe 实际语义）合成一次鼠标左键点击 (x,y) 灌入完整输入链；0 = 关（2026-09-28 WASD 回归场景复现用，见 git:调试历史）
	uint32_t uiClickProbeX = 0;         // 点击像素列（窗口坐标）
	uint32_t uiClickProbeY = 0;         // 点击像素行（窗口坐标）
	uint32_t cameraProbeFrame = 0;      // 脚本化相机键盘探针（窗口模式）：第 N 帧（0 起）按住移动键 holdFrames 帧，验证 eventSystem→InputFilter→cameraSystem 输入链并打 PASS/FAIL；0 = 关（2026-09-28 "面板吞 WASD" 回归护栏）
	uint32_t cameraProbeKey = 0;        // 探针 SDL 键码（须为 Camera.keys 绑定的 W/A/S/D 之一；lua 里写单字母，解析期映射）
	uint32_t cameraProbeHold = 0;       // 按键保持帧数（<1 解析期回落 8）
	bool rpc = false;                   // Phase 10b HTTP POST RPC 通道（只绑 127.0.0.1；独立于 imgui 门，headless 可用，docs/archive/phase10b_plan.md）
	uint32_t rpcPort = 8788;            // RPC 监听端口（bind 失败 = 日志降级关，不崩）
	/**
	 * GPU fault **masked 轮询**（device_fault 模块；默认关）。
	 *
	 * 开 = 每帧 `vkGetDeviceFaultReportsKHR(timeout=0)` 轮一次，把驱动**自己内部恢复掉、
	 * 没上报 device lost** 的 masked fault 打成 ERROR。🔴 语义代价（拍板前必读）：启用
	 * `deviceFaultReportMasked` 后驱动**可以**选择内部恢复而不报 device lost —— 也就是
	 * 说开这道门有可能把"炸给你看"变成"悄悄恢复 + 一条 ERROR"。这正是查偶发 hang 想要的
	 * 数据，但它不是免费的，所以默认关、且 `deviceFaultDeviceLostOnMasked` 恒不请求。
	 * 关 = 零 Vulkan 调用（device lost 后的那次抓取不受本门影响，恒尝试）。
	 */
	bool faultPoll = false;
	/**
	 * VVL **GPU-AV 兼容模式**（默认关；只在诊断跑里开）。
	 *
	 * 做的事只有一件：创建 shader object 时**不带** `VK_SHADER_CREATE_INDIRECT_BINDABLE_BIT_EXT`。
	 * 为什么需要：GPU-AV 装不了带该 bit 的 shader object（它内部要用
	 * `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC` 塞进 `VkIndirectExecutionSetEXT`），会打一条
	 * Internal Error 然后**自行关闭 GPU-AV** —— 于是"开了 GPU-AV"与"没开"在日志上几乎同形，
	 * 是典型的静默假绿。摘掉后 GPU-AV 正常装桩（实测 DescriptorHeapPass / MeshShading 都有计数）。
	 *
	 * 🔴 语义代价：该 bit 是进 DGC execution set 的 shader 的硬要求（VUID-11032/11154）。
	 * 本引擎默认配置不走 DGC execution set，所以开了它 errors=0；但**与 `dgc_static` 同开 = 违例**
	 * （gate_lint 出 warn）。它改变 shader object 的创建标志 ⟹ **本构建下的任何像素/性能读数
	 * 都不代表交付形态**，只用于抓 GPU 侧的越界与空描述符。
	 */
	bool gpuavCompat = false;
	/**
	 * **描述符堆槽内容对账**（债-140 第三击 ①；默认关）。
	 *
	 * 开 = 每帧在 MDI 行表 upload 之前，把行表绑定的那个 heap 槽**读回来**与构造期快照逐字节
	 * 对账（`MeshletShadowArgsService::auditSlot`）。抓的是"槽里现在是 0 / 是陈旧值"——
	 * devcoredump 实测 fault 地址恰 = 0x0 的直接对应形态。
	 *
	 * 为什么两网都替代不了它：ASan 不插桩描述符堆（mmap 的设备可见内存，不在它的 CPU 侧
	 * 插桩面里）；GPU-AV 查描述符**索引**越界，不查槽里存的**地址值**。
	 *
	 * 成本 = 每帧一次 ≤64B 的 host 映射 memcmp（实测 → `docs/还债实测数据.md` §4.11）。
	 * 只在 MDI 行表载体存在时才有对象（`meshlet.mdi_shadow=false` 时零调用，gate_lint 出 warn）。
	 */
	bool heapSlotAudit = false;
	/**
	 * **GPU 内存分配时毒化**（债-140 H3 实验，docs/债140_跨驱动与上游取证.md §13.5；默认关）。
	 *
	 * 开 = 以下三类 GPU 内存**分配那一刻**整片填可辨识的非零毒值（子集由 `poisonScope` 选）：
	 *   ① bindless 描述符堆（resource + sampler heap 的**用户槽区**，避开驱动 reserved 槽）
	 *   ② MDI 行表（persistent-mapped 的 ring×2）
	 *   ③ cull 数据链（candidate_table / sequences / sequence_count / entry_streams）
	 *
	 * 判据为什么成立：毒只在**从未被写过**的地方活到 GPU 读取那一刻——CPU 全量写的 buffer
	 * 毒会被 memcpy 覆盖（对它是 no-op），GPU 每帧写满的 buffer 同理。于是"fault 地址从
	 * `0x0` 变成毒值形态"就是「读了从未写过的槽/行」的当场定罪；毒字按偏移递增
	 * （`gpuPoisonWordAt`）故地址还能反解出**是哪个四字组**。
	 *
	 * 🔴 只做**分配时**毒化，不做逐帧重毒：逐帧重毒会改掉 ring 的复用语义（那是 v2，本批不碰）。
	 * 门开时的任何像素/性能读数都不代表交付形态；门关 = 一条 `if` 都不进（逐位不变，出厂锚亲验）。
	 */
	bool poisonGpuMem = false;
	/**
	 * 毒化范围（分 buffer 归因用，改这个键**不用重编**）：`"all"` 或
	 * `heap` / `rowtable` / `cull` 的逗号子集。
	 *
	 * 🔴 未知名 = 启动失败（`cull_replay` 同一条纪律）：拼错一个名字就少毒一块，而
	 * "没毒到"与"毒到了没炸"在 fault 地址读数上**完全同形**，静默回落等于把实验做成假负。
	 */
	std::string poisonScope = "all";
	/**
	 * **A1 黑匣子**（债-140 锁存路线判死后的数据侧摄像头，docs/债140_跨驱动与上游取证.md §13.11；默认关）。
	 *
	 * 开 = 每帧把 MDI 阴影段的现场事实（帧号 / 每段已写行数 / 上传的间接计数 COUNT 及其来源 /
	 * 行表首末行六字 + 整段摘要 / heap 槽台账 / DGC 状态）投进环形缓冲，由**独立线程**异步
	 * 落 ndjson（路径 = `blackbox_path`）。
	 *
	 * 为什么需要它：债-140 是概率性挂死，而 devcoredump 只有复位**之后**的寄存器快照
	 * （§13.10 实测那里所有 fault 锁存都是复位值）。要归因就得自己留一份"案发前最后 N 帧"。
	 *
	 * 🔴 观测者效应：本案唯一已证实能改挂率的变量是速度，而记录本身会改时序。因此生产端
	 * 只做一次 POD 拷贝（无锁/无分配/无格式化），且**挂率读数只能读成"没抓到"**。
	 * 环形缓冲溢出会**覆盖最旧**并计数 —— 复盘时必须连 `blackboxGetStats()` 一起读，
	 * 否则"最后一条记录"会被误读成"案发前一帧"。
	 *
	 * 门关 = 一条 `if` 都不进（出厂锚逐位不变的前提）。
	 */
	bool blackbox = false;
	/**
	 * 黑匣子 ndjson 落盘路径（**绝对路径**为宜；父目录须已存在）。
	 *
	 * 为什么要单独一枚键而不是写死：逐轮实验各要一份产物，写死就得为改路径重编一次
	 * （同 `poison_scope` 的加键理由）。`blackbox=true` 而本键为空 = 什么都没发生，
	 * 由 gate_lint 出声。
	 */
	std::string blackboxPath;
	/**
	 * **A2 着色器自证**（债-140 数据侧摄像头的 GPU 侧；默认关）。
	 *
	 * 开 = 阴影 MDI 的 **task 入口**换用 `-DSHADOW_SELFCHECK` 变体 SPV
	 * （`shadow_meshlet.task_SHADOW_SELFCHECK.spv`），在每次读行表后自证五条判据
	 * （行号越界 / gcX≠1 / meshGroupCount==0 / objectIndex 越界 / gcY‖gcZ≠1），
	 * 违例走 `printf` → SPIR-V `NonSemantic.DebugPrintf` → RADV 每 submit 打 stderr。
	 * 两个界经 push **word2**（MDI 变体下的保留位，mesh 侧一字节不读）传入，
	 * 打包式 = `blackbox` 模块的 `encodeSelfCheckPush`（CPU 镜像同处一份）。
	 *
	 * 🔴 为什么必须是**第二个 SPV 变体**而不是运行时分支：`light.frag` 的 IZB_CONSUME
	 * 教训（2026-10-02 实测 105401 像素漂移）—— 新代码"出现在编译单元里"这件事本身就会
	 * 让 slang 重排 SSA / fp 融合，运行时 flag 挡不住逐位漂移。门关选原版 SPV =
	 * 新指令连存在都不存在。
	 *
	 * 运行开关在驱动侧：`RADV_PRINTF_BUFFER_SIZE=16777216`（不设 = printf 是 no-op，
	 * 于是"门开了但零输出"分不清"没违例"与"没缓冲"——矩阵脚本必须每轮核对它）。
	 */
	bool shaderSelfcheck = false;
};

/**
 * 毒值基准字（债-140 H3）。为什么是 `0xDEADBEEF` 而不是全 1 / 全 0：
 * **全 0 与"从未写过的内存"同形**——而本案要抓的正是它，填 0 等于什么都没做；
 * 全 1 会把硬件描述符的所有位域一次性点亮（含 stride / DSPCC / 类型位），
 * 失效形态难归因。`0xDEADBEEF` 非零、可辨识、且高位不全 1。
 */
export constexpr uint32_t kGpuPoisonBaseWord = 0xDEADBEEFu;

/**
 * 字节偏移处的毒值字 = `kGpuPoisonBaseWord + 四字组下标`（**纯函数**，无设备依赖）。
 *
 * 为什么不是常量：常量毒值只能说"读到了毒"，递增毒值还能说"**读到的是第几个四字组**"。
 * 描述符堆步长 32B ⟹ `(fault 低 32 位 − kGpuPoisonBaseWord) × 4` = 堆内字节偏移。
 * 🔴 回绕成 0 的那一格必须改口（0 = 与未写内存同形 = 判据静默失效），守卫由单测反向用例钉。
 * @param byteOffset 目标起点的字节偏移（非 4 的倍数时向下取整到四字组）
 * @return 该处的毒值字，**恒非零**
 */
export [[nodiscard]] uint32_t gpuPoisonWordAt(uint64_t byteOffset);

/**
 * 按毒值填充 `[dst, dst+bytes)`（尾部不足一字的按字内字节序补齐——**不留未毒字节**，
 * 否则尾部那几字节仍是"与未写内存同形"，毒面出现判据盲区）。
 *
 * 🔴 "有没有毒到"的判据是**字级不是字节级**：`base + idx` 会产出 `0xDEADBF00` 这种含
 * `0x00` 字节的字（idx 从 0x11 起就有）。肉眼在 hexdump 里看到 0 字节**不等于**没毒到，
 * 要按 4 字节组读；这也正是 GPU 侧唯一可能的读粒度（`ByteAddressBuffer.Load` 取 4B）。
 * @param dst  目标首地址；nullptr = 直接返回（不崩、不越界写）
 * @param bytes 字节数；0 = 直接返回
 */
export void fillGpuPoison(void* dst, size_t bytes);

/**
 * 毒化目标（`poisonActiveFor` 的入参）。用 `enum class` 而非字符串：调用点写
 * `poisonActiveFor(PoisonTarget::heap)`，拼错是**编译错误**；字符串分派的未知名会静默
 * 返回"不毒"，正好复刻本实验最怕的假负。新增枚举值时实现里的穷尽 switch（**无 default**）
 * 会编译失败逼人补上——这是"让错的东西编不过"，不是注释里写"记得同步"。
 */
export enum class PoisonTarget {
	heap,      ///< bindless 描述符堆（resource + sampler 两个 heap 的用户槽区）
	rowTable,  ///< MDI 行表（persistent-mapped 的 ring×2）
	cullChain, ///< cull 数据链四张 buffer
};

/**
 * 综合判据：门 `poisonGpuMem` 开着**且** `poisonScope` 含本目标？三处分配点共用这一个
 * 判据，避免"门开了但某处忘了看 scope"。每次调用重解析 scope 字符串（分配期各调一次，
 * 成本可忽略），换来配置面只有一个真相源。
 * @param target 待判目标
 * @return true = 该分配点应铺毒值
 */
export bool poisonActiveFor(PoisonTarget target);

/**
 * 毒化范围（`poisonScope` 的解析结果）。用三个独立布尔而非位掩码：调用点写成
 * `if (scope.heap)` 就够，位掩码要额外造 `operator|` 且写错位没人拦。
 */
export struct PoisonScope {
	bool heap = false;       ///< ① bindless 描述符堆（resource + sampler 两个 heap）
	bool rowTable = false;   ///< ② MDI 行表 ring×2
	bool cullChain = false;  ///< ③ cull 数据链四张 buffer
};

/**
 * 解析 `poison_scope`（**纯函数**，无设备依赖）。
 * @param spec `"all"`，或 `heap` / `rowtable` / `cull` 的逗号子集（大小写敏感，空格容忍）
 * @return 解析结果；空串 / 空 token / 未知名 = unexpected（启动失败，不回落）
 */
export [[nodiscard]] std::expected<PoisonScope, Error> parsePoisonScope(std::string_view spec);

/**
 * 驱动侧 printf 缓冲是否已开（= 环境变量 `RADV_PRINTF_BUFFER_SIZE` 非空且非 "0"）。
 *
 * 为什么住在这里而不是让调用点自己 `getenv`：这是**一枚判据**而不是一个字符串读取——
 * "设了但设成 0" 与 "没设" 在驱动侧行为相同（都不分配缓冲），把这条口径写两遍迟早漂。
 * 消费者 = gate_lint 的 `shader_selfcheck` 生效前置（没开缓冲时"自证零输出"不可归因）。
 * @return true = 驱动会分配 printf 缓冲并把 DebugPrintf 打到 stderr
 */
export [[nodiscard]] bool printfBufferEnabled();

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
 * 解析器键轨迹（2026-10-02 门回显派生的覆盖率护栏输入，docs/archive/gate_lint_plan.md §3-B）：
 * 记录解析器**实际读过**的键与 Lua 文件里**实际出现**的顶层键。两个用途：
 *   ① 单测断言「读过的键集合 == 回显表键集合」——漏一个方向都红（回显抄漏 = 撒谎行，
 *      回显多写 = 陈旧键）；
 *   ② 未知键检查：文件里出现而解析器不读 = 拼错的键，今天**完全静默**。
 * 键名书写与 Lua 路径一致，子表字段用 `表名.字段`（如 `camera_probe.key`）。
 */
export struct DiagnosticsKeyTrace {
	std::vector<std::string> read;     ///< 解析器读过的键（含未出现在文件里的——解析器总是全量问一遍）
	std::vector<std::string> present;  ///< 文件里实际出现的键名：顶层 + 固定字段块下钻一层的 `块.子键`（债-129 第三刀；开放 map 只记表名）
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
