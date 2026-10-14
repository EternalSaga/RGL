export module offgraph_sync;

import rlstd;
import rlvk;
import vkerror;
import compute_access_log;  // B2-3：RgComputeHazardClass（rlbase 派生的 hazard，本层只做对账）

namespace RGL {

/**
 * **图外同步的申报即发射**（批次 B1b 第一刀 → `docs/compute_调研.md` §7.2 / §7.3）。
 *
 * ## 这一层解决什么
 *
 * 图外 compute 各自手发 buffer / global barrier（实测逐帧 33 条目 / 31 条命令），而
 * **「缺 barrier 反正 VVL 会报」这个指望在本项目实测不成立**（同文 §4：syncval 对 shader 侧
 * buffer 访问 0 条，GPU-AV 连越界读都不报）。债-75 登记的就是这笔"零对账"。
 *
 * ## 🔴 为什么是「申报即发射」而不是「另写一份申报」
 *
 * 申报若是**另写一份**，就得到第二份命令流，两份事实必然漂，而漂了没人查的护栏比没有更坏。
 * 所以本层把三件事收进**同一次调用**：① 由 usage 词表**物化** barrier 参数 ② 发命令
 * ③ 记账。于是「申报了却没发」与「发了但挡错 mask」在写法上不可能。
 *
 * 同型先例（不是新发明）：`Renderer::transitionAndTrack` 的注释原话就是「两个独立字面量一旦
 * 分家，表就可能与真实布局脱钩，而**读表的护栏随之退化成同义反复**」；`usageInfo`
 * （`render_graph_impl.cppm`）则是「命令参数由查表物化」的现成形状。
 *
 * ## 🔴 本层**不**做什么
 *
 * - **只抓 compute ↔ compute 那部分的「忘写一条边」**（B2-3 起，见下方 hazard 表）。
 *   declare-by-emit 本身只保证**已发的**一致，没发的它看不见；补上那一格的是**访问对推导**
 *   （读写方向从 SPV 码流里反射出来，不是人抄）：`compute_access_log` 把「反射方向 ×
 *   `makeMappedCompute` 的堆槽绑定」相交成 hazard，本模块的 hazard 表拿它对账，未覆盖即
 *   **构造期抛**（挂点 = 工厂，它是唯一同时知道 (shader, param → 槽) 全事实的地方）。
 *   🔴 **射程边界**：两个参与者**都是 compute** 的访问对才看得见——反射的收录面只有
 *   `*.comp.slang` 的入口参数。非 compute 参与者（frag / mesh / 拷贝 / host）没有条目
 *   ⟹「矩链 compute 写 → light.frag 采样」这类边仍只由 `expectedPerFrame` 的应发次数守，
 *   hazard 表管不到它们。
 *   本层的 L1 仍是「应发次数」：忘发 / 少发 / 多发 / 门关却发。
 * - **不接管发射权**（plan-only 红线，§5.2 仍有效的 14.4b B 档否决）：发射仍由模块发起，
 *   模块侧保留一行 `emit(...)`（一个边名，不是六个 mask 字面量）。这是红线的价格。
 * - **不认识任何具体模块**：边表按归属命名，但机制本身零模块知识（期望次数由模块构造期
 *   `expectCount` 供给，见 `EvssmShadow` 的用法）。hazard 表是**这张规则唯一的例外**——
 *   它的存在理由就是把「哪两枚 compute 经哪个 binding 共享资源」写成人能复核的申报，
 *   零模块知识在这里不是优点而是漏报。
 */

/**
 * 同步**原子 usage**：`{stage, access}` 的唯一来源。
 *
 * 🔴 **原子 = 语义角色，不是"观测到的掩码集合"**。后者会让"词表覆盖全部形态"变成恒真的废话
 * （18 个匿名掩码换个地方存而已）。
 *
 * **宽档必须单独命名并写明为什么宽**，否则就是静默的过度同步。本词表现有六个宽档，
 * 宽的**原因**各不相同，这才是它们不能并成一个"随便一个宽档"的理由：
 *
 * | 宽档 | 为什么宽 |
 * |---|---|
 * | `anyWriteGlobal` | 生产者**本模块无句柄**（EVSSM 的阴影图归 RT 缓存）⟹ 表达不成 buffer 档 |
 * | `allCommandsWrite` | 生产者是一条 **API 命令**（`vkCmdFillBuffer`），它没有"只写这一档"的窄 access 可给 |
 * | `priorAccessWideCs` / `Gfx` | **跨帧**依赖的源侧：上一帧最后访问点取决于当时的门（dump 开没开），拿不准的一律纳入 |
 * | `heapReadWide` | 消费者是 **EXT 描述符堆**——它是一个扩展对象、不是本层能命名句柄的 buffer ⟹ 表达不成 buffer 档，只能走无句柄执行序 |
 * | `priorAccessSceneData` | **跨帧 + 跨命令缓冲**依赖的源侧：scene data 单份跨 frames-in-flight 共享且每帧**两个写者**（主 CB 与 cull CB），本帧 copy 与"上一帧的读 / 另一 CB 的写"之间必须自建依赖 |
 *
 * 两条 `priorAccessWide*` 只差 compute 一档，**是刻意分开的**：head 被 compute 写过、mask 没有。
 * 合成一个"最宽的"确实更省事，但省掉的那一位正是"这个资源到底被谁碰过"这条信息本身。
 * 同理 `priorAccessSceneData` 也不是 `priorAccessWideCs` 的复制品——两者 stage 同宽，但 access
 * **方向不同**：scene data 只被 shader 读、被**另一条 CB** 拷写 ⟹ 少 `ShaderWrite` / `TransferRead`
 * 却多 `TransferWrite`（Cs 那一支的源侧没有"另一个写者"，因为 head 只有一个写者）。
 */
export enum class RgSyncUsage : uint8_t {
	computeWrite,      ///< compute 写 storage（CS / ShaderWrite）
	computeRead,       ///< compute 读（CS / ShaderRead）
	computeReadWrite,  ///< 下一个 dispatch **既读又写**同一资源（CS / ShaderRead|ShaderWrite）
	fragmentRead,      ///< fragment 采样读（**FS** / ShaderRead）
	graphicsWrite,     ///< 图形阶段写（**AG** / ShaderWrite）——IZB mask frag 的 atomic 写
	graphicsRead,      ///< 图形阶段读（**AG** / ShaderRead）
	graphicsReadWrite, ///< 图形阶段既写又读（AG / ShaderRead|ShaderWrite）
	transferRead,      ///< 拷贝**读**它（作为拷贝源；TR / TransferRead）
	transferWrite,     ///< 拷贝**写**它（作为拷贝目的；TR / TransferWrite）
	hostRead,          ///< host 读 mapped 内存（Host / HostRead）
	hostWrite,         ///< host **写**它（本层唯一 host 写侧原子；cull 的 CPU 侧填 descriptor heap）
	anyWriteGlobal,    ///< 宽档兜底：无句柄的全局写序（AG / MemoryWrite）
	allCommandsWrite,  ///< 宽档兜底：**任意阶段**的写（AllCommands / MemoryWrite）
	priorAccessWideCs, ///< 宽档兜底：上一帧「图形 + compute + 传输读」的全部访问（跨帧依赖）
	priorAccessWideGfx,///< 宽档兜底：同上但**不含 compute**（该资源不经 compute）
	heapReadWide,      ///< 宽档兜底：图形阶段读 **EXT 描述符堆**（AG / ResHeapRead|SamplerHeapRead|TransferRead|ShaderRead）
	priorAccessSceneData, ///< 宽档兜底：scene data 的跨帧 + 跨 CB 前序访问（AG|CS|TR / ShaderRead|TransferWrite）
	count,             ///< 原子数（单测遍历词表用，不是原子）
};

/** 一个 usage 原子物化出的 barrier 掩码对。 */
export struct RgSyncMasks {
	vk::PipelineStageFlags2 stage = {};
	vk::AccessFlags2 access = {};
};

/**
 * usage 词表（唯一真相源，实现见 `offgraph_sync_impl.cppm`）。
 *
 * 期望值出处 = 迁移前各模块代码里的**字面量**（`tests/test_offgraph_sync.cppm` 逐位钉死），
 * 所以"词表没偷偷改语义"是被测出来的，不是被相信的。
 * @param usage 原子
 * @return 该原子的 stage / access 掩码
 */
export RgSyncMasks syncUsageMasks(RgSyncUsage usage);

/** 一条同步边覆盖的资源档。 */
export enum class RgSyncScope : uint8_t {
	executionOrder,  ///< 无 buffer 引用的全局执行依赖（`MemoryBarrier2`）
	buffer,          ///< 覆盖某个 buffer（`BufferMemoryBarrier2`，句柄由发射点给）
};

/** 一条边归属的**门**（帧末覆盖对账按它取启用位；口径同 B1a 的工作项门）。 */
export enum class RgSyncGate : uint8_t {
	evssm,          ///< `EvssmShadow` 在场（Renderer 的 `evssmShadow` 非空）
	izb,            ///< `IzbShadow` 在场（门关 = 对象根本不被构造 = 零分配零命令）
	izbDump,        ///< 本帧是 izb 的 dump 帧（`shadow.lua` 的 `izb.dump_frame`）
	izbMaskSegment, ///< 本帧 shadow0 段非空 ⟹ mask pass 真的录了（段空 = 整段跳过，债-32）
	cull,           ///< GPU cull 数据通路本档要录 + 提交 cull CB（`CullDriver::gpuPassRuns()`）
	cullFanout,     ///< 且静态 DGC 流已建（`GpuCull::entryStreamsEnabled()`）——条件边挂自己的门
};

/**
 * 门的数量（= 帧末 `checkEmission` 的 active 长度要求）。
 *
 * 🔴 **门之间不独立**：`izbDump ⊂ izb`、`izbMaskSegment ⊂ izb`、`cullFanout ⊂ cull`。帧末门位表
 * 必须自己带上这个包含关系（`izbOn && …` / `cullOn && …`），否则"运行中途把 IZB 关掉"会让 dump
 * 门按配置算成开、而 barrier 一条没发 ⟹ **假阳性**。这类"门位算错"由单测的门开/门关两条用例分别守。
 */
export constexpr uint32_t kSyncGateCount = 6;

/** 已登记的同步边（顺序 = 内部计数数组下标）。 */
export enum class RgSyncEdgeId : uint8_t {
	evssmDepthToCompute,  ///< 阴影图 depth-write → 矩派生 read（无句柄纯执行序）
	evssmMomentsChain,    ///< 矩链级间 compute→compute（**每帧 mipLevels−1 次**，构造期申报）
	evssmMomentsToLight,  ///< 矩链 compute write → light pass fragment 采样
	// —— 以下 13 条 = IzbShadow（批次 B1b 第二刀；资源档全是 buffer）——
	izbHeadPriorToClear,   ///< ② head 清零**前**的跨帧依赖（宽档，见 usage 表）
	izbHeadClearToCreate,  ///< ② FillBuffer 写 → create dispatch 写
	izbHeadCreateToPrime,  ///< ③ create 写 → ⑤ prime 读（head）
	izbNodesCreateToPrime, ///< ④ 同上但资源是 nodes（与上一条 masks 全同、资源不同）
	izbMaskPriorToClear,   ///< ⑧ mask 清零前的跨帧依赖（含 dump 帧的 Transfer 读 = WAR）
	izbMaskClearToPass,    ///< ⑧ mask 清零 → mask pass 写 / light.frag 读（段空时靠它排序）
	izbDumpHead,           ///< ⑦ dump：head compute 写 → 拷出读
	izbDumpNodes,          ///< ⑦ dump：nodes compute 写 → 拷出读
	izbDumpMask,           ///< ⑦ dump：mask 图形写 → 拷出读
	izbDumpStagingToHost,  ///< ⑦ dump：staging 拷入 → host 读（mapped 内存）
	izbHeadToMaskPass,     ///< mask pass 开：head compute 写 → frag 读（跨 pass 边界）
	izbNodesToMaskPass,    ///< 同上但资源是 nodes
	izbMaskToLight,        ///< mask pass 收：frag atomic 写 → light.frag Load 读
	// —— 以下 8 条 = GpuCull（批次 B1b 第三刀；6 条 buffer 档 + 2 条无句柄执行序档）。
	// 序号 ①②③… 沿用 `recordCullPass` 里的注释序号，红了能直接对着那段代码读。
	cullHeapHostToReads,    ///< ① host 写 heap → 图形阶段读 heap（无句柄：heap 不是 buffer）
	cullScenePriorToCopy,   ///< ② scene data 拷贝**前**的跨帧 + 跨 CB 依赖（宽档，见 usage 表）
	cullSceneCopyToReads,   ///< ③ scene data 拷入 → 图形阶段读
	cullSeqUploadToCreate,  ///< ④ 序列 staging 拷入 → cull dispatch 写
	cullSeqWriteToReadback, ///< ⑤ cull dispatch 写 → 读回拷出（序列）
	cullCountWriteToReadback,///< ⑤ 同上但资源是 count（与上一条 masks 全同、资源不同）
	cullReadbackToHost,     ///< ⑥ 读回拷入 → host 读 mapped 内存（无句柄执行序）
	cullSeqFanoutChain,     ///< ⑤.5 序列 compute 写 → fanout dispatch 既读又写（静态 DGC 流才发）
	count,
};

/** 一条边的静态描述（命令流由它 + 词表物化，模块侧不写 mask）。 */
export struct RgSyncEdgeDesc {
	std::string_view name;      ///< 日志归因名
	RgSyncScope scope = RgSyncScope::executionOrder;
	RgSyncUsage src = RgSyncUsage::computeWrite;
	RgSyncUsage dst = RgSyncUsage::computeRead;
	RgSyncGate gate = RgSyncGate::evssm;
	uint32_t expectedPerFrame = 1;  ///< 可被模块构造期 `expectCount` 覆写
};

/**
 * 边表（唯一真相源）。
 *
 * 新增一条边 = 在这里加一行 + 在归属模块里发一次 `emit`。**只加行不发射**会被帧末对账抓
 * （门开着却 0 次），**只发射不加行**编译不过（没有边名可发）。
 * @param id 边
 * @return 边的静态描述
 */
export const RgSyncEdgeDesc& syncEdgeDesc(RgSyncEdgeId id);

/** 边的下标（计数数组用；导出 = impl unit 也靠它，非导出名在 import 侧不可见）。 */
export constexpr uint32_t syncEdgeIndex(RgSyncEdgeId edge) {
	return static_cast<uint32_t>(edge);
}

/**
 * 一条 **compute ↔ compute hazard 的申报**（本刀唯一新增的人工半边）。
 *
 * ## 为什么这一张表是「人工半边」而不是又一份注释
 *
 * hazard 的**存在**由机器派生（`compute_access_log`：反射方向 × 工厂的堆槽绑定），本表只回答
 * 「这条 hazard 靠哪条边同步」。两侧缺一侧都不成立：只有表 = 注释（shader 改名/删 binding
 * 没人红），只有派生 = 不知道哪条边算覆盖。互锁由 `tests/test_compute_hazard.cppm` 的双源
 * 对账用例执行（每条声明的 writer 必须在生成物里 `writable`、reader 必须 `readable`）。
 *
 * ## 🔴 匹配是**对称**的（已知限制，别假装它管）
 *
 * 我们只有**方向**、没有**执行顺序**（反射不知道谁先跑，本层刻意不引入相位模型——见
 * `compute_access_log` 文件头），所以查表按 `(shader, param)` 的**无序对**：只要这一对之间
 * 存在任意一条申报边就**通过**。
 * ⟹ **边的 src/dst mask 是否与真实先后匹配，本判据不管**。那由两件事各自负责：模块自己的
 * 发射点（它知道顺序）+ 现有 `expectedPerFrame` 的应发次数对账（忘发/少发/多发/门关却发）。
 * `writer*` / `reader*` 的命名因此只是**给双源对账用的角色声明**（谁该 writable、谁该 readable），
 * 不参与匹配。
 *
 * ## 🔴 本表**没有**「不需要边」的豁免位
 *
 * 没有 reason 字段、没有「声明这条 hazard 无需同步」的口子。理由：一条「补一行带 reason 就绿」
 * 的登记表没有牙（本仓刚因此撤回过一条规则；判据 = 双相对账成立的前提是两个来源各自携带对方
 * 推不出的信息，而人写的 reason 不是第二个来源）。真出现不需要边的新 hazard 时，那是另一次
 * 设计讨论，不是在这里预留形状的地方。
 */
export struct RgComputeHazardEdge {
	std::string_view writerShader;  ///< 写侧 shader 文件名（"izb_create.comp.slang"）
	std::string_view writerParam;   ///< 写侧 binding 变量名（"head"）
	std::string_view readerShader;  ///< 读侧 shader 文件名
	std::string_view readerParam;   ///< 读侧 binding 变量名
	RgSyncEdgeId edge;              ///< 覆盖它的同步边
};

/**
 * hazard 表（唯一真相源的数据面在 `offgraph_sync_impl.cppm`，本函数是访问器）。
 *
 * 条目数由表本身给出（不另开一个 constexpr 计数——那会给「有人偷偷加了一条」多一个可以不动的
 * 地方；数量锚直接钉 `computeHazardEdges().size()`）。
 * @return 全部申报条目
 */
export std::span<const RgComputeHazardEdge> computeHazardEdges();

/**
 * 对账一条 hazard 类：它有没有被 hazard 表里的某条边覆盖。
 *
 * 🔴 **只在 shader object 创建期调用**（工厂内），不每帧重算——EVSSM / IZB 是首帧懒构造，
 * 对账就在那一刻发生，之后日志与表都不再变。
 * @param hazard 日志派生出的一条 hazard 类
 * @return 覆盖它的边；未覆盖 = Error（消息点名**两侧**的 shader + param + 槽号并指路表名）
 */
export std::expected<RgSyncEdgeId, Error> checkComputeHazard(const RgComputeHazardClass& hazard);

/**
 * 帧内发射账（Renderer 持有一个实例，按引用传给各图外模块）。
 *
 * 判据形状与 B1a 的 `OffGraphSchedule::checkCoverage` **同构**（门开 ⟺ 发生过），差别只在
 * 这里比的是"次数"而不是"有没有"——因为一条边可以合法地每帧发 N 次（EVSSM 的 mip 链）。
 */
export class OffGraphSync {
   public:
	/** 边数（单测与诊断用）。 */
	static constexpr uint32_t kEdgeCount = static_cast<uint32_t>(RgSyncEdgeId::count);

	/** 帧首（drawFrame 开场）：清本帧发射计数（期望次数是启动期事实，跨帧保留）。 */
	void beginFrame();

	/**
	 * 记账：本帧这条边发过一次。
	 *
	 * 🔴 **由 `emit` 调用，也是单测驱动判据核的入口**——`emit` 要 `CommandBuffer`（设备级），
	 * 对账逻辑本身却零设备，所以两者必须能分开测。代价说清楚：单测测不到"`emit` 里有没有
	 * 忘调本函数"，那一条由**真程序变异**覆盖（删一次 `emit` ⟹ 帧末必须红，口径同 §7.1）。
	 * @param edge 边
	 */
	void recordEmission(RgSyncEdgeId edge) { ++counts[syncEdgeIndex(edge)]; }

	/**
	 * 启动期覆写某条边的应发次数（模块构造期调用）。
	 *
	 * 为什么放在**构造期**而不是录制期：录制期申报 = 模块自己出题自己答，那条判据就自证了。
	 * 构造期申报的是"我的循环跑 `mipLevels-1` 次"这种**关于模块结构**的事实，
	 * 而"本帧真的跑了这么多次吗"仍然是独立检查（把 barrier 挪出循环立刻红）。
	 * @param edge 边
	 * @param expected 每帧应发次数
	 */
	void expectCount(RgSyncEdgeId edge, uint32_t expected_);

	/**
	 * 发射一条**全局执行序**边（`MemoryBarrier2`）：物化 + 发命令 + 记账，不可分割。
	 * @param cmd 录制中的命令缓冲
	 * @param edge 边（scope 必须是 `executionOrder`）
	 */
	void emit(vk::raii::CommandBuffer& cmd, RgSyncEdgeId edge);

	/**
	 * 发射一条 **buffer** 边（`BufferMemoryBarrier2`，整块覆盖）。
	 *
	 * 没有 `size` 参数不是偷懒：现场勘察里全部 buffer barrier 都是 `VK_WHOLE_SIZE`
	 * （`izb_shadow_impl.cppm` 的 `bufferBarrier` 写死整块），而 `VK_WHOLE_SIZE` 是 C 宏，
	 * 在接口单元里不可见——**宁可等真需要子区间时再加，也不在这里重述一个常量**。
	 *
	 * 🔴 **两个重载都不校验 `desc.scope` 与自身匹配**，这是有意的：错配的两个方向不对称。
	 * 把 buffer 档的边发给**全局**那个重载 = 得到一条 `VkMemoryBarrier2`（无句柄 = 覆盖全部
	 * 资源）= **过度同步**，白付成本但不会错；反方向要调用方**主动交出一个 buffer 句柄**，
	 * 手里没句柄就发不出来。所以错配的典型后果是"慢"而不是"漏同步"，不值得为它在录制热路径
	 * 上加分支、也不值得把边表拆成两份枚举（本仓不用 `assert`，Release 下等于没有）。
	 * 真出现"该窄的档发宽了"由谁抓？= B2 的依赖图与 L2（同文 §7.3），不是这里。
	 * @param cmd    录制中的命令缓冲
	 * @param edge   边（scope 必须是 `buffer`）
	 * @param buffer 覆盖的 buffer 句柄（句柄归模块所有，词表不持）
	 */
	void emit(vk::raii::CommandBuffer& cmd, RgSyncEdgeId edge, vk::Buffer buffer);

	/**
	 * @param edge 边
	 * @return 本帧该边已发次数（诊断与单测用）
	 */
	uint32_t emitted(RgSyncEdgeId edge) const { return counts[syncEdgeIndex(edge)]; }

	/**
	 * 帧末对账：实发次数 vs 应发次数，**两个半边各一条**。
	 *
	 * @param gateActive 与 `RgSyncGate` 同序的启用位（门关 = false）
	 * @return 空值 = 一致；否则 Error（首条违规，消息点名边 / 实发 / 应发）
	 */
	std::expected<void, Error> checkEmission(std::span<const bool> gateActive) const;

   private:
	/** 本帧发射计数（下标 = `RgSyncEdgeId`）。 */
	std::array<uint32_t, kEdgeCount> counts{};
	/** 应发次数（初值取边表，模块构造期可覆写）。 */
	std::array<uint32_t, kEdgeCount> expected = [] {
		std::array<uint32_t, kEdgeCount> table{};
		for (uint32_t i = 0; i < kEdgeCount; ++i)
			table[i] = syncEdgeDesc(static_cast<RgSyncEdgeId>(i)).expectedPerFrame;
		return table;
	}();
};

}  // namespace RGL
