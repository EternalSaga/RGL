export module blackbox;

import rlstd;

/**
 * blackbox —— 债-140 **A1 黑匣子**与 **A2 自证谓词**的纯逻辑面（rlbase，零设备依赖）
 *
 * ## 它解决的那一件事
 * 债-140 是**概率性** GPU 挂死（CP timeout → MODE2 全复位），而 CPU 侧此前没有任何
 * "案发前每帧现场"的记录。devcoredump 只有复位**之后**的寄存器快照（§13.10 已实测：
 * 那里所有 fault 锁存都是复位值）。本模块把**案发前最后 N 帧**的 MDI 阴影段事实落成
 * ndjson，让"挂的那一轮"能与"同帧的 COUNT/行内容"对上。
 *
 * ## 为什么住在 rlbase
 * 判据 = 「需不需要一个 VkDevice 才能跑」：编码、摘要、谓词全是纯 CPU 换算 ⟹ rlbase，
 * `just test`（无 GPU 无窗口）可钉。形状照 `mdi_rows`（同为纯 CPU、纯搬运）。
 *
 * ## 🔴 陈旧行才是本案的形态（决定了谓词长什么样）
 * 不可见候选写 `gcX = 0` ⟹ **那一行根本不启动 task** ⟹ "读到从未写过的全零行"从 task
 * 内部**不可观测**（观测不到 = 不是判据）。真正可观测、且能直接解释 `fault @ VA 0x0` 的是：
 * task 读到的行**内容是陈旧的或读歪了**——ring×2 按 `frameSlot = frame % 2` 跨帧复用同一
 * 批槽位，而"行读歪"（步长/偏移漂移）会让 task 把别的字节当六个 word 读。两种形态都会
 * 让 task 带着**错的** `objectIndex` / `clusterStart` 启动，mesh 侧
 * `readObjectClusterRange(objectIndex)` 拿到错基址 → 越界取址 → 0x0。
 *
 * ⟹ 谓词的重心在**行内容自检**（2-5 条），而不是"行号有没有越界"：
 *  · `gcY`/`gcZ` 恒 1 是行布局的硬事实（`MdiShadowRow` 默认值），读歪时几乎必然破坏它
 *    ⟹ 谓词 5 是"行读歪"的当场指纹；
 *  · `objectIndex` 必须落在对象表内 ⟹ 谓词 4 直接钉住 fault@0 那条取址通路；
 *  · `gcX ∈ {0,1}`（不变量 3b）与 `meshGroupCount > 0`（不变量 2）是构建器契约的 GPU 侧复述。
 *
 * 🔴 已当场排除的一条候选根因（写下来免得后来人重查）：`mdiBuildShadowSegments` 里
 * `rowSpan` 在 `mdi.rows->insert` **之前**创建，靠 `reserve(totalRows)` 精确才不悬垂。
 * 实测两侧计数式**逐条同构**（`mdiScanGate` 与 `mdiCascadeInputs` 用同一个
 * `meshletPipeline/meshletMdiPipeline` 过滤 + 同样的 `pinnedCount > 0 ? 1 : 0` 加行）
 * ⟹ `table->rows.size() == gate.rowsPerCascade` 恒成立 ⟹ reserve 精确、无 realloc、
 * span 不悬垂。**这条不是本案根因。**
 *
 * ## 覆盖面（🔴 引用本模块结论时不许读宽过这张表）
 * 五条谓词全部只依赖**行表内容 + push 里的两个界**，因此 shader 与 CPU 镜像能共用同一条判据：
 *
 * | # | 判据 | 不成立时意味着 |
 *|---|---|---|
 * | 1 | `drawID ≥ rowsWritten`（本段本帧写入行数）| 🔴 **当前构造下恒不成立**（`drawCount`、`rows.size()`、`rowCount` 三者出自同一表达式 `table->rows.size()`）——它的价值是把这条**隐含前提变成 GPU 侧的当场断言**：一旦有人改动让"上传的行数"与"上传的间接计数"分家，printf 立刻响，而不是让 GPU 静默读两帧前的陈旧行 |
 * | 2 | `groupCountX != 1` | 不变量 3b 破（★M-3T-c：每可见行恰一枚 task）|
 * | 3 | `meshGroupCount == 0` | 不变量 2 破（会发 `DispatchMesh(0)` 的空 task）|
 * | 4 | `objectIndex ≥ objectCapacity` | mesh 侧 `readObjectClusterRange` 直接越界 = fault@0 通路 |
 * | 5 | `groupCountY != 1 ∥ groupCountZ != 1` | **行读歪**（步长/偏移漂移的当场指纹：Y/Z 恒 1）|
 *
 * 🔴 **覆盖不到的**（写清楚，别让"零违例"被读成"已排除"）：
 *  · `clusterStart` 的**真**上界是 `ObjectClusterRange::count`，task 入口没声明 binding 33
 *    读不到那张表 ⟹ 本谓词判不了"clusterStart 越过本 object 区间尾"；
 *  · `meshGroupCount` 的**上**界（只判 0）；
 *  · 行内容合法但**语义**错（陈旧行恰好指向一个仍然存活的对象）——这条只能靠 A1 的事后复盘；
 *  · mesh 入口自己的取址链（本批只自证 task 读到的行）。
 *
 * ## 观测者效应
 * 记录本身会改时序（本案唯一已证实能改挂率的变量是速度）。因此：
 *  · 生产端只做一次 POD 拷贝进环形缓冲，**不格式化、不加锁、不分配**；
 *  · 格式化与写盘在独立线程（双缓冲），GPU 路径零阻塞；
 *  · 环形缓冲满 = **覆盖最旧**并计数（黑匣子要保的是"最后 N 帧"，不是"前 N 帧"）。
 * 挂率因此只能读成"没抓到"，不能读成"已排除"。
 */

namespace RGL {

/** 一帧最多记几段（= 阴影级联数上限；c0 恒旧路径，实际到 3）。定死是为了让记录变 POD。 */
export constexpr uint32_t kBlackboxMaxSegments = 4u;
/** 一帧最多记几枚 heap 槽（债-140 实测条目烘 8 条 mapping，留余量）。 */
export constexpr uint32_t kBlackboxMaxHeapSlots = 16u;

/**
 * 一行 MDI 参数的**摘要**（六个 word，原样抄，不改名不换序）。
 *
 * 为什么不存 `MdiShadowRow` 本体：黑匣子的读者是"事后看日志的人"，六个裸字比一个
 * 需要 import mdi_rows 才能解释的结构体更好读，且让记录保持 POD（环形缓冲里 memcpy）。
 */
export struct BlackboxRowSummary {
	uint32_t words[6] = {0u, 0u, 0u, 0u, 0u, 0u};  ///< gcX/gcY/gcZ/objectIndex/clusterStart/meshGroupCount
};

/** 一段（一个级联）的每帧事实。 */
export struct BlackboxSegmentFacts {
	uint32_t cascadeIndex = 0u;  ///< 阴影分块号
	uint32_t frameSlot = 0u;     ///< 本段落的帧槽号（ring×2 的轮转位）
	uint32_t firstRow = 0u;      ///< 本段在 slice 内的起始行号
	uint32_t rowCount = 0u;      ///< 🔴 本帧**写入**的行数（= A2 谓词 1 的上界）
	uint32_t taskCount = 0u;     ///< 本段 task 数 = Σ gcX
	uint32_t pipeline = 0u;      ///< MDI 管线条目下标
	uint64_t argsOffset = 0u;    ///< GPU 侧间接命令起点（字节）
	uint32_t rowBase = 0u;       ///< shader 侧行基址（与 argsOffset 同源 pair）
	uint32_t drawCount = 0u;     ///< 🔴 上传的间接计数（COUNT）
	uint32_t drawCountSource = 0u;  ///< COUNT 的来源值（= 段 rowCount，独立记一份以便对账）
	uint64_t sliceDigest = 0u;   ///< 本段写入那截行表的摘要（`blackboxDigest`）
	uint32_t headRowIndex = 0u;  ///< 摘要对应的首行**全局行号**
	uint32_t tailRowIndex = 0u;  ///< 摘要对应的末行**全局行号**
	BlackboxRowSummary head;     ///< 首行六字
	BlackboxRowSummary tail;     ///< 末行六字
};

/** 一帧的黑匣子记录（POD：进环形缓冲就是 memcpy，生产端零分配零格式化）。 */
export struct BlackboxFrameRecord {
	uint64_t frame = 0u;         ///< 帧号（引擎口径）
	uint64_t timestampNs = 0u;   ///< 单调时钟纳秒（与 ndjson 落盘时刻对得上）
	uint32_t segmentCount = 0u;  ///< 本帧有效段数（≤ kBlackboxMaxSegments）
	std::array<BlackboxSegmentFacts, kBlackboxMaxSegments> segments{};
	uint32_t heapSlotCount = 0u;  ///< 本帧条目会读的 heap 槽数
	std::array<uint32_t, kBlackboxMaxHeapSlots> heapSlots{};  ///< 槽号台账
	uint32_t dgcActive = 0u;      ///< DGC 状态（0 = 关/未构造，1 = 已构造）
	uint32_t selfCheckOn = 0u;    ///< A2 门是否同开（复盘时要能区分"两门都开"）
};

/**
 * 行表字节区间的摘要 = **FNV-1a 64 位**（**纯函数**，无设备依赖）。
 *
 * 为什么是摘要而不是整片落盘：一段行表 1591×24B ≈ 38KB，每帧三段 × 每帧写盘 = 114KB/帧，
 * 观测者效应直接压过信号。摘要 + 首末行六字 = 单帧几百字节，够回答"这一帧这段写进去的
 * 到底是什么"，且**跨帧可比**（同一摘要连续出现 = 该段根本没被重写）。
 *
 * 为什么 FNV-1a 而不是 CRC32：要的是"两帧内容不同"的**可辨性**，不是纠错；FNV-1a 无表、
 * 64 位碰撞概率对本场景（比较相邻帧）足够，且纯函数好钉单测。
 * @param bytes 行表区间（空 span = 返回 FNV 偏移基准，**不是 0**，见实现）
 * @return 64 位摘要
 */
export [[nodiscard]] uint64_t blackboxDigest(std::span<const std::byte> bytes);

/**
 * 把一帧记录编码成**一行 ndjson**（**纯函数**）。
 *
 * 🔴 为什么"纯"这件事值得单独钉：黑匣子的全部价值在"引擎死了也要保住案发前最后几帧"，
 * 而编码器一旦依赖设备/配置/全局态，落盘线程就可能在被杀前卡住或读到半口数据。
 * 编码 = 入参的纯函数 ⟹ 生产端可以在任意时刻（含 upload 之后、提交之前）同步算好字符串
 * 的所有输入，写盘线程只做 I/O。
 *
 * 形态：一行一个 JSON 对象，键序**固定**（便于 grep/diff 与逐轮对账）。
 * @param rec 帧记录
 * @return 以 '\n' 结尾的一行 ndjson
 */
export [[nodiscard]] std::string encodeBlackboxFrameNdjson(const BlackboxFrameRecord& rec);

/**
 * 编码一行的六个 word 为 ndjson 片段 `[a,b,c,d,e,f]`（**纯函数**，供帧编码复用）。
 * @param s 行摘要
 * @return 形如 `[1,1,1,7,0,2]` 的数组字面量
 */
export [[nodiscard]] std::string encodeBlackboxRowSummary(const BlackboxRowSummary& s);

/**
 * A2 自证 push 字（**纯函数**）：把两个界打包进 MDI push 的 **word2**。
 *
 * 🔴 为什么是 word2：12B push 在 MDI 变体下 word0 = rowBase、word1 = cascadeIndex，
 * **word2 是保留位**（`shadow_meshlet.mesh.slang` 的 `ShadowMeshletDrawArgs::clusterStart`
 * 注释原文："MDI_SHADOW 变体下此位 = reserved"）⟹ 复用它不动 argsSize、不动执行器通用
 * push 路径、mesh 侧一个字节都不读。门关时这里恒 0 = 逐位不变。
 *
 * 为什么打包成 16+16 而不是扩 push：扩 push 要动 `.argsSize`（条目 argsSize 一字节不动
 * 是 ★M-1b 立的规矩）。两个界各自 ≤ 65535（实测 sliceRows ~4773、对象表容量 4096），
 * 打包无损；**超限由本函数出声**（截断 = 界变小 = 假阴，正是本案最怕的失效形态）。
 *
 * @param rowsWritten    本段本帧写入的行数（谓词 1 的上界）
 * @param objectCapacity 对象簇区间表容量（谓词 4 的上界）
 * @return 打包后的 word2；任一界 > 65535 → 返回 `0`（= 两个界都是"永不违例"的形态，
 *         调用方必须出声；宁可自证失效也不静默误判）
 */
export [[nodiscard]] uint32_t encodeSelfCheckPush(uint32_t rowsWritten, uint32_t objectCapacity);

/** 自证 push 字拆出来的两个界（`decodeSelfCheckPush` 的返回）。 */
export struct SelfCheckPush {
	uint32_t rowsWritten = 0u;     ///< 低 16 位
	uint32_t objectCapacity = 0u;  ///< 高 16 位
};

/** `encodeSelfCheckPush` 的逆（**纯函数**，单测据此钉往返一致；shader 侧同一条拆法）。
 *  @param word push word2 的原始值
 *  @return 两个界（低 16 位 = rowsWritten，高 16 位 = objectCapacity）
 */
export [[nodiscard]] SelfCheckPush decodeSelfCheckPush(uint32_t word);

/**
 * A2 自证谓词的**位段**（CPU 镜像与 shader 共用同一组编号）。
 *
 * 为什么是位段而不是枚举返回：一帧里同一行可能同时犯多条（陈旧行往往同时触发 1 与 4），
 * 全都要报出来才好归因；且 shader 侧一个 `uint` 就能带出来。
 * 为什么是**导出常量**而不是 `enum class : uint32_t` 或裸 `enum`：位段要 `|`/`&`，
 * `enum class` 得额外造一组运算符；裸 `enum` 被 style-check 拦（禁裸 enum）；
 * `export namespace` 也只给 rlvk 豁免。常量集三个坑都不踩，代价是名字要带前缀。
 */
export inline constexpr uint32_t kShadowViolNone = 0u;
export inline constexpr uint32_t kShadowViolRowBeyondWritten = 1u << 0;  ///< 谓词 1：`drawID ≥ rowsWritten`（当前构造下恒不成立，见文件头）
export inline constexpr uint32_t kShadowViolGcxNotOne = 1u << 1;  ///< 谓词 2：不变量 3b（每可见行恰一枚 task）
export inline constexpr uint32_t kShadowViolMeshGroupCountZero = 1u << 2;  ///< 谓词 3：不变量 2（不发空 DispatchMesh）
export inline constexpr uint32_t kShadowViolObjectIndexOob = 1u << 3;  ///< 谓词 4：mesh 侧 readObjectClusterRange 会越界
export inline constexpr uint32_t kShadowViolRowMisaligned = 1u << 4;  ///< 谓词 5：gcY/gcZ 非 1 = 行读歪（步长漂移的指纹）

/** 自证谓词的输入（CPU 镜像；字段与 shader 里能拿到的东西一一对应）。 */
export struct ShadowRowCheckInput {
	uint32_t drawId = 0u;          ///< 🔴 **段内相对**序号 = SV_DrawID（不是绝对行号！绝对行号 = rowBase + drawId）
	uint32_t rowsWritten = 0u;     ///< 本段本帧写入行数（谓词 1 的界，同为段内相对口径）
	uint32_t groupCountX = 0u;     ///< 行 word[0]
	uint32_t groupCountY = 0u;     ///< 行 word[1]（恒 1）
	uint32_t groupCountZ = 0u;     ///< 行 word[2]（恒 1）
	uint32_t objectIndex = 0u;     ///< 行 word[3]
	uint32_t meshGroupCount = 0u;  ///< 行 word[5]
	uint32_t objectCapacity = 0u;  ///< 谓词 4 的界
};

/**
 * 五条自证谓词的 **CPU 镜像**（**纯函数**；shader 侧是同一判据的第二份拼写）。
 *
 * 🔴 为什么 CPU 侧要有一份：shader 里的判据**没法单测**（要 GPU、要真崩溃），而它一旦写错
 * （比如边界用 `>` 而不是 `>=`、或者把 word 下标数错），读数就是"零违例"——与"确实没违例"
 * 完全同形。CPU 镜像让同一条判据进 `just test`，变异它测试必须红。
 *
 * @param in 行内容与两个界
 * @return `ShadowRowViolationBits` 的按位或
 */
export [[nodiscard]] uint32_t shadowRowViolations(const ShadowRowCheckInput& in);

/**
 * 违例位段 → 可归因短串（**纯函数**；进 printf 文本与 ndjson 复盘）。
 * @param bits `shadowRowViolations` 的返回
 * @return 形如 `row_beyond_written|object_index_oob`；无位 = `none`
 */
export [[nodiscard]] std::string describeShadowRowViolations(uint32_t bits);

/**
 * 从行字节里取第 `i` 个 word（**纯函数**，越界返回 0）。
 *
 * 为什么要它而不是直接 `reinterpret_cast`：行表是裸字节算术（`ByteAddressBuffer.Load` 的
 * CPU 对偶），未对齐的 `reinterpret_cast` 是 UB；逐字节组装小端字既无对齐问题也与
 * shader 的 `Load` 语义**逐位同**。
 * @param row 一行的字节（不足 4×(i+1) 时返回 0，不越界读）
 * @param i   word 下标
 */
export [[nodiscard]] uint32_t blackboxRowWord(std::span<const std::byte> row, uint32_t i);

/**
 * 一段行表的**三件读数**（**纯函数**产物）：整段摘要 + 首行 + 末行。
 *
 * 为什么三件一起出：它们读的是同一段字节，分开算就是三遍扫描三个循环边界（三个可错点）。
 */
export struct BlackboxSegmentDigest {
	uint64_t digest = 0u;      ///< 整段摘要（`blackboxDigest`）
	BlackboxRowSummary head{};  ///< 首行六字
	BlackboxRowSummary tail{};  ///< 末行六字
	bool hasRows = false;      ///< 段内至少一行（false = 两个摘要无意义，不许读成"全零行"）
};

/**
 * 一次算出整段摘要与首/末行摘要（**纯函数**）。
 *
 * 为什么吃**裸字节 + 步长**而不是 `MdiShadowRow`：黑匣子要能记"ring 里实际躺着什么"，
 * 而 ring 是裸字节 buffer（`ByteAddressBuffer` 的 CPU 对偶）。吃结构体就等于假设
 * "行数组与 ring 内容一致"——那正是本案要验的事，不能当前提。
 *
 * @param bytes  本段在 ring 里对应的那截字节（upload **之后**读，记的是真落盘内容）
 * @param stride 行步长（= `kMdiShadowRowStride`，由调用方给，本模块不抄 24）
 * @return 三件读数；`bytes` 不足一行 → `hasRows = false`
 */
export [[nodiscard]] BlackboxSegmentDigest blackboxDigestSegment(std::span<const std::byte> bytes,
	uint32_t stride);

/**
 * 黑匣子落盘统计（🔴 复盘时必须与记录一起读，否则"最后一条记录"会被读成"案发前一帧"）。
 */
export struct BlackboxStats {
	uint64_t recorded = 0u;      ///< 生产端调用次数
	uint64_t written = 0u;       ///< 已落盘并 flush 的行数
	uint64_t overwritten = 0u;   ///< 🔴 环形缓冲满被覆盖的记录数（>0 = "最后 N 帧"不完整）
	uint64_t openFailures = 0u;  ///< 打开文件失败次数（失败 = 整个黑匣子空转）
	bool open = false;           ///< 当前是否在录
};

/**
 * 打开黑匣子（幂等：已开则直接返回既有状态）。
 *
 * 🔴 失败**不抛**：这是每帧路径的配套设施，启动期打不开文件（目录不存在/权限）应该
 * **出声并让黑匣子空转**，而不是把引擎带崩——"黑匣子没录"与"录了但零违例"是两件事，
 * 前者由 `blackboxGetStats().open == false` 与 `openFailures > 0` 明确区分。
 * @param path ndjson 落盘路径（父目录须已存在）
 * @return true = 正在录制
 */
export bool blackboxOpen(std::string_view path);

/**
 * 记一帧（生产端；**GPU 路径零阻塞**）。
 *
 * 实现约束（观测者效应的下界就在这几条上）：无堆分配、无锁、无格式化、无系统调用 ——
 * 只做一次 POD 拷贝 + 一个 release 原子自增。环形缓冲满则**覆盖最旧**并累加 `overwritten`。
 * 未打开 = 直接返回。
 * @param rec 帧记录
 */
export void blackboxRecord(const BlackboxFrameRecord& rec);

/**
 * 按门试开一次（幂等，首帧之后是纯原子读）。
 *
 * 🔴 为什么需要单独暴露这一枚，而不是让 `blackboxRecord` 顺带开：调用方在"本帧没有段可记"
 * 时会先判空早退（回落逐条 draw 的帧、或 `meshlet.mdi_shadow=false` 的对照臂），于是
 * **永远不产生任何记录，也永远不打开文件** ⟹ "黑匣子坏了"与"黑匣子没东西可记"在日志里
 * 完全同形，而后者恰恰是对照臂要区分出来的两件事。开录提到判空之前，"开录 →"那行就恒为
 * 装桩证据，与"有没有内容"彻底解耦。
 */
export void blackboxEnsureStarted();

/**
 * 同步排空到磁盘（退出收口 / device-lost 抓取点调用）。
 *
 * 🔴 为什么需要显式调：黑匣子的判据是"挂的那轮最后一条记录 = 案发前一帧"，而写盘线程
 * 是异步的 —— 不排空就可能停在"内存里有、文件里没有"，那正是本项目最忌的**看起来已完工的
 * 半吊子**。device-lost 路径必须调它。
 */
export void blackboxFlush();

/** 收尾：排空 + 关文件 + 停线程（幂等）。 */
export void blackboxClose();

/** @return 当前统计快照（读锁内取值，可与记录一起落日志） */
export [[nodiscard]] BlackboxStats blackboxGetStats();

/**
 * 单调时钟纳秒（填 `BlackboxFrameRecord::timestampNs` 用）。
 *
 * 为什么由本模块给而不是让调用点自己 `steady_clock::now()`：时钟选择与单位是**记录格式的
 * 一部分**（复盘时要知道它是单调的、不是墙钟），散在调用点就是每个调用点各定一次口径；
 * 顺带把 `<chrono>` 留在本模块 GMF（例外清单成员），不把时钟依赖扩散进 rlframe。
 * @return 自 epoch 的单调纳秒数（只用于求差与排序，不对应墙上时间）
 */
export [[nodiscard]] uint64_t blackboxNowNs();

/**
 * ## 「录上了没有」这本账住在哪里（🔴 别把它读成"没账"）
 * 黑匣子**通过时不逐帧出声**，所以"日志里零 ERROR"分不清「录了 N 帧都没问题」与
 * 「一次都没录上」。三处出声点全部**前置**在事件发生的时刻，而不是等退出收口：
 *   ① 开录成功 → INFO（含路径与缓冲容量）——这行就是"装桩证据"；
 *   ② 门开着但打不开 → ERROR（明说"本轮实验没有任何黑匣子产物"）；
 *   ③ 环形缓冲溢出 → WARN（只报首次，明说"最后若干帧不完整"）。
 *
 * 🔴 为什么不在退出路径打总结行：那需要往 `registerDiagnosticReporters` 加一行，而
 * `main_impl.cppm` 正卡在 500 代码行红线上（499/500）——为一行日志去拆一个无关文件
 * 是更坏的交易。前置出声反而更强：进程半路死掉时，启动那行仍然在日志里。
 * 落了多少行的硬证据 = ndjson 文件本身的行数（与 ① 那行的容量口径对账）。
 *
 * 开录时机 = **首次 `blackboxRecord` 时按门惰性打开**（不在 main 里开）：同样为了不把
 * 门读取扩散到 main，且"第一帧就开不成"与"启动就开不成"在归因上等价，日志都在最前面。
 */

/**
 * mesh 取址链自证的位段（**第二批摄像头**，覆盖面清单里"最疼的两条"就在这里）。
 *
 * 🔴 为什么必须下移到 mesh：task 只把行表三个 word 抄进 payload 就完事，它**读不到**
 * `ObjectClusterRange`（没声明 binding 33），所以"clusterStart 的真上界"在 task 里根本判不了；
 * 而真正拿这些值去解引用的是 mesh —— `range.base + clusterStart + 簇内序` → 簇条目 →
 * 条目**内部**的 `triOffset` / `remapOffset` → 顶点池。故障发生在链的末端，上一批的摄像头
 * 装在链的首端。
 *
 * 与 task 那五条的关键区别：**这七条全是真上界，没有一条是恒不成立的哨兵。**
 * mesh 手里就有四张 buffer 的 `GetDimensions` 和它自己要用的那个 `range`，界是拿得到的。
 */
export inline constexpr uint32_t kMeshViolNone = 0u;
export inline constexpr uint32_t kMeshViolObjectIndexOob = 1u << 0;  ///< objectIndex ≥ 对象表容量（mesh 两次解引用对象表）
export inline constexpr uint32_t kMeshViolClusterStartOob = 1u << 1;  ///< 🔴 clusterStart ≥ range.count（本批补上的"真上界"，覆盖面清单第 1 条）
export inline constexpr uint32_t kMeshViolClusterPastRange = 1u << 2;  ///< clusterStart 本身在区间内，但派发序号把它推出区间（×2/选级算术错）
export inline constexpr uint32_t kMeshViolClusterEntryOob = 1u << 3;  ///< 簇条目越过簇表尾（引擎**已有**这道守卫，但它静默早退 = 不画也不出声）
export inline constexpr uint32_t kMeshViolTriSectionOob = 1u << 4;  ///< 🔴 条目里的 triOffset + 本簇三角段越过 Tris buffer（原码**无守卫**）
export inline constexpr uint32_t kMeshViolRemapSectionOob = 1u << 5;  ///< 🔴 条目里的 remapOffset + vertCount 越过 Remap buffer（原码**无守卫**）
export inline constexpr uint32_t kMeshViolVertexPoolOob = 1u << 6;  ///< 🔴 vb + 顶点步长越过顶点池 = fault@0 最直接的一条（原码**无守卫**）
export inline constexpr uint32_t kMeshViolLocalIdxPastCluster = 1u << 7;  ///< 三角局部索引 ≥ vertCount（产物契约破；它决定 remap 读哪个元素）
export inline constexpr uint32_t kMeshViolClusterBaseWrapped = 1u << 8;  ///< 🔴 `clusterIdx × 44` 在 32 位下**回绕**——引擎那道簇表守卫是 32 位算术，回绕会让它判不出来（守卫可被绕过本身就是要抓的东西）

/**
 * mesh 取址链一条链上的全部被验值（CPU 镜像的输入；字段与 shader 里的**latch 后局部**一一对应）。
 *
 * 🔴 段级两判（Tris / Remap）的开关**不是装饰而是控制流**：shader 只有在簇条目**在表内**时
 * 才读得到 `triCount`/`triOffset`/`remapOffset`，条目越界时那些值根本不存在（读它是未定义）。
 * 镜像因此内部按"条目可读"门控这两判（= P3 不成立才评），与 shader 的控制流**同形**；
 * 若不加门控，条目越界时会拿一堆 0 去判段越界，要么恒判成违例、要么与 shader 判得不一样。
 */
export struct MeshChainCheckInput {
	uint32_t objectIndex = 0u;      ///< payload 带来的对象槽号
	uint32_t objectCapacity = 0u;   ///< 对象表容量（push word2 高 16 位）
	uint32_t rangeCount = 0u;       ///< 🔴 `ObjectClusterRange::count` = 本对象簇区间长度（真上界）
	uint32_t clusterStart = 0u;     ///< payload 带来的区间内起点
	uint32_t intraCluster = 0u;     ///< 派发派生 = `latchedSlot >> 1`（latch 后的 groupID.x）
	uint32_t clusterIdx = 0u;       ///< `range.base + clusterStart + intraCluster`
	uint32_t clusterStride = 0u;    ///< 44（DiskCluster），由调用方给，本模块不抄
	uint32_t clustersBytes = 0u;    ///< `meshletClusters.GetDimensions`
	uint32_t triOffset = 0u;        ///< 条目 word@16：Tris 段**字节**偏移
	uint32_t triCount = 0u;         ///< 条目 word@8
	uint32_t trisBytes = 0u;        ///< `meshletTris.GetDimensions`
	uint32_t remapOffset = 0u;      ///< 条目 word@20：Remap 段**元素**偏移
	uint32_t vertCount = 0u;        ///< 条目 word@12
	uint32_t remapBytes = 0u;       ///< `meshletRemap.GetDimensions`
	uint32_t vertexPoolBytes = 0u;  ///< `meshletVertexPool.GetDimensions`
	uint32_t vertexStride = 0u;     ///< 48（ModelVertex），由调用方给，本模块不抄
	bool vertexValid = false;       ///< 是否带逐顶点那两判（mesh 循环内才评）
	uint32_t localIdx = 0u;         ///< 三角的局部顶点索引（来自 Tris 段的 u8）
	uint32_t vb = 0u;               ///< remap 换算出的顶点池**字节**偏移 = 下一次 Load3 的地址
};

/**
 * mesh 取址链七条判据的 **CPU 镜像**（**纯函数**；shader 侧是同一判据的第二份拼写）。
 *
 * 🔴 算术一律走 64 位：`triOffset + 3*triCount`、`(remapOffset + localIdx)*4`、`vb + stride`
 *    在 32 位下都可能**回绕成小值**，于是"越界"被判成"没越界"——那是假阴，正是本案最怕的
 *    失效形态（上一批 `encodeSelfCheckPush` 的"超限不截断"是同一条纪律）。
 *
 * @param in 链上各跳的被验值（latch 后的那一份）
 * @return `kMeshViol*` 的按位或
 */
export [[nodiscard]] uint32_t meshChainViolations(const MeshChainCheckInput& in);

/**
 * mesh 链违例位段 → 可归因短串（**纯函数**）。
 * @param bits `meshChainViolations` 的返回
 * @return 形如 `cluster_start_oob|vertex_pool_oob`；无位 = `none`；有位无名 = `unknown_bits`
 */
export [[nodiscard]] std::string describeMeshChainViolations(uint32_t bits);

}  // namespace RGL
