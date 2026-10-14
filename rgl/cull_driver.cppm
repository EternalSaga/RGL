export module cull_driver;

import rlstd;
import rlvk;
import VmaRaii;
import vk_device;
import vk_resources;
import gpu_cull;
import vk_indirect_commands;
import shader_registry;
import frame_draw_list;
import vkerror;
import diagnostics;
import offgraph_sync;  // 批次 B1b：把 Renderer 的发射账转交给 GpuCull 的录制点（本类不发 barrier）

namespace RGL {

/**
 * GPU culling 帧编舞者（Phase 10 批次 0 手术 3：自 Renderer::drawFrame ④c 段与
 * opaque 段双路提交摘出——Renderer 回归"盲执行器"，culling 的提交时序知识集中本类）。
 *
 * 持 GpuCull（数据通路：entry 表/序列/count/staging）与可选 DgcCommands（NVIDIA 路），
 * 负责三件事：④c 帧内 cull CB 录制→提交→同步等待→读回→CPU/GPU 对账；opaque 段
 * 双路提交（DGC execute / 读回重放）；对账日志节流。
 * gpuCull 为 null 或 inactive = 全路径不碰 culling（行为中性，与摘出前 Renderer 语义一致）。
 *
 * **三档驱动方式**（`diagnostics().cullReplay`，语义 → `CullReplayMode`，定价 →
 * docs/cpu_perf_plan.md §4）决定本类做多少事：
 *
 * | 档 | cull CB | 读回时机 | opaque 段由谁驱动 | 对账 |
 *|---|---|---|---|---|
 *| `cpu` | 不录不提交 | — | CPU 剔除（通用 segment 逐 draw） | 无（GPU 集没跑） |
 *| `gpu_lagged` | 录 + 提交 | **下一帧**帧首 | CPU 剔除（同上） | 逐帧，比**上一帧**的 CPU 表 |
 *| `gpu_sync` | 录 + 提交 | 本帧帧内 | GPU 序列（DGC / 读回重放） | 逐帧，比本帧 CPU 表 |
 *
 * 时序契约（2026-10-09 改，此前与主 CB 共用 inFlight fence）：**cull 提交挂本类自持的
 * `cullFences[slot]`**，不再借用 `session.currentInFlightFence()`。原共用契约要求"submit
 * 信号后立即 reset"（vkdQueueSubmit 要求 fence 非 signaled 态）——那是一处只有本类知道的
 * 隐式时序耦合，且它让"滞后一帧收割"根本无从表达（fence 已被主 CB 复用）。自持之后：
 * 帧 F 首等的是**帧 F-1 的 cull CB**，而它排在帧 F-1 主 CB **之前**入队 ⟹ 收割几乎不等待，
 * 这正是 `gpu_lagged` 能消掉帧首硬同步的机制（`gpu_sync` 等的是本帧 cull CB，排在帧 F-1
 * 整条主 CB 之后 ⟹ 等 = 等上一帧整帧排空 ≈ gpu.frame，即债-30 的账）。
 *
 * 读回 ring 安全：`GpuCull::readback` 按 slot 分槽（×framesInFlight），滞后收割发生在
 * 本帧 cull 提交**之前**，且收割的是上一帧的 slot ⟹ 不与本帧 GPU 写冲突。
 */
export class CullDriver {
   public:
	/**
	 * 构造（main 先建 GpuCull/DgcCommands 再建 Renderer，本类在 Renderer 构造体内建）。
	 * 驱动方式在构造期从 `diagnostics()` 快照（该单例在 main 最早期填充，此后只读），
	 * 之后不再读配置 = 每帧热路径少一次全局量访问。
	 * @param vkDevice 设备层（device/graphicsQueue/graphicsCommandPool）
	 * @param vkGpuCull culling 数据通路（nullptr = culling 不存在）
	 * @param vkDgc DGC 命令生成 IES 路（nullptr = 非 NVIDIA 全 DGC 路；优先级最高）
	 * @param vkDgcStatic 静态 shader DGC 路（nullptr = 关；RADV spike，
	 *   docs/archive/radv_static_dgc_spike.md——IES 路缺席时接管 opaque 段，两者皆 null 走重放）
	 * @param framesInFlight 帧 slot 数（= Renderer::MAX_FRAMES_IN_FLIGHT，cull CB/fence 按 slot 分配）
	 * @param vkSync 图外同步发射账（Renderer 成员，声明在 `cullDriver` **之前** ⟹ 构造期已存在；
	 *   本类只把它转交给 `GpuCull::recordCullPass`，自己不发边）
	 */
	CullDriver(RGL::DeviceContext& vkDevice, RGL::GpuCull* vkGpuCull, RGL::DgcCommands* vkDgc,
		RGL::DgcStaticCommands* vkDgcStatic, uint32_t framesInFlight, OffGraphSync& vkSync);

	/**
	 * culling 数据通路是否存在（null/inactive = false）。
	 * @return true = 有候选表且 GPU 表已建
	 */
	[[nodiscard]] bool active() const;

	/**
	 * 本档是否需要录 + 提交 cull CB（`cpu` 档 = false ⟹ `runFrame` 根本不必被调用）。
	 * @return true = `gpu_lagged` 或 `gpu_sync` 且数据通路存在
	 */
	[[nodiscard]] bool gpuPassRuns() const {
		return active() && replayMode != CullReplayMode::cpu;
	}

	/**
	 * 本帧是否**应当**录静态 DGC 的扇出 dispatch（= 同步对账 `cullFanout` 子门的启用位）。
	 *
	 * 判据是「门 + 结构」（cull 档在跑 且 构造期建了 per-entry 流），🔴 **不许**改成「刚才调过
	 * `recordFanoutDispatch` 没有」——那会让帧末对账自证（流没建时两边一起不成立 = 什么都测不到，
	 * 口径同 `Renderer::offGraphEndFrame` 里 mask 段那条「重算段跨度」的注释）。
	 * @return true = cull 数据通路在跑且静态 DGC 流已建
	 */
	[[nodiscard]] bool fanoutRuns() const {
		return gpuPassRuns() && gpuCull && gpuCull->entryStreamsEnabled();
	}

	/**
	 * opaque 段是否由 GPU 序列驱动（false = 落回通用 segment 逐 draw 路径）。
	 *
	 * 这是 Renderer 侧唯一需要问的判据：`cpu` / `gpu_lagged` 两档 draw 仍由 CPU 剔除结果
	 * 驱动，走 `segmentSpan("opaque")` + `drawOne` 的通用路（与 GPU 重放**逐位同**——
	 * 两者参数同源 CPU args，实测截图 SHA256 相同，docs/cpu_perf_plan.md §4）。
	 * @return true = `gpu_sync` 档且数据通路存在
	 */
	[[nodiscard]] bool drivesDraws() const {
		return active() && replayMode == CullReplayMode::gpuSync;
	}

	/**
	 * ④c 帧内 culling 数据通路（按档做其中一部分）：收割上一帧滞后的读回并对账 →
	 * cull CB 录制（staging 序列拷贝 + dispatch [+ DGC preprocess]）→ 提交 →
	 * （仅 `gpu_sync`）本帧同步等待 + 读回 + 对账。
	 * @param frameSlot 本帧 in-flight slot（cull CB / staging / fence 索引）
	 * @param frame 全局帧计数（仅日志诊断上下文，与摘出前 "frame {}" 文案一致）
	 * @param list 每帧绘制列表（cullInput 序列计数 + cullArgs CPU 位级真相源 + 对账 CPU 侧）
	 * @param sceneStaging 本帧 scene staging（recordCullPass 读矩阵算 mvp）
	 * @return void；submit/wait 失败 → Error（DeviceLost/Unknown，与摘出前一致）
	 */
	std::expected<void, Error> runFrame(uint32_t frameSlot, uint64_t frame,
		const FrameDrawList& list, RGL::VmaBuffer& sceneStaging);

	/**
	 * opaque 段双路提交（pass 循环 drawSource=="opaque" 且 drivesDraws() 时调用）：
	 * DGC execute（NVIDIA 路，GPU 直接消费序列）或读回重放（RADV 路，逐 candidate
	 * 按可见位重放，args = CPU 位级真相源 → 截图 BIT-IDENTICAL）。
	 * @param cmd 主命令缓冲（录制期，已在 beginRendering 内）
	 * @param list 每帧绘制列表（cullArgs 重放参数源）
	 * @param registry 管线表（重放路径 bindShaders/pushDataSet 查找）
	 */
	void executeOpaqueSegment(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
		RGL::ShaderRegistry& registry, uint32_t frameSlot);

   private:
	/**
	 * culling 独立命令缓冲分配（构造期，仅 active；inactive = 空）。与主 CB 分离的
	 * 原因 = 读回重放需要"提交 cull → 同步等 → 读回 → 再录 opaque 重放"的帧内停顿，
	 * 同一 CB 无法 end-后-续录。
	 */
	void allocateCommandBuffers();

	/**
	 * cull 专用 fence 分配（构造期，仅 `gpuPassRuns()`；**建成非 signaled**——首提交前没有
	 * 等待动作，带 eSignaled 出厂 = 提交已信号态 fence，VVL 直接 error）。
	 * 每 slot 一个，只挂 cull 提交（不与主 CB 共用，理由见类注释时序契约段）。
	 */
	void allocateFences();

	/**
	 * 录 + 提交本帧 cull CB（挂 `cullFences[frameSlot]`），并把本帧 CPU opaque 表记进
	 * 待收清单（供滞后档下一帧对账；任意时刻至多一条在途，见 `pendingCpuSet` 注释）。
	 * @param frameSlot 本帧 slot
	 * @param frame 全局帧计数（日志上下文）
	 * @param list 每帧绘制列表
	 * @param sceneStaging 本帧 scene staging
	 * @return 失败 → Error（与摘出前同口径）
	 */
	std::expected<void, Error> submitCullPass(uint32_t frameSlot, uint64_t frame,
		const FrameDrawList& list, RGL::VmaBuffer& sceneStaging);

	/**
	 * 收割指定 slot 的读回（阻塞等该 slot 的 cull fence）+ 按档对账 + 清 pending。
	 * @param slot 要收割的 cull fence/readback 槽位
	 * @param list 本帧绘制列表（对齐自检恒用它；可见集合的 CPU 侧按档选源，见下）
	 * @return wait 失败 → Error（DeviceLost）
	 */
	std::expected<void, Error> harvest(uint32_t slot, const FrameDrawList& list);

	/**
	 * GPU/CPU 剔除结果对账（同帧口径，`gpu_sync` 档）：逐 candidate 可见集合 vs CPU
	 * opaque 段（objectSlot 多重集）比对 + 对齐自检（cullArgs[i] ↔ entries[i] 同
	 * objectSlot 顺序契约的运行时证明）。
	 * @param list 每帧绘制列表（CPU opaque 段 [0, skyboxStart)）
	 */
	void reconcile(const FrameDrawList& list);

	/**
	 * 滞后对账（`gpu_lagged` 档）：GPU 可见集合 vs **提交那一帧**记下的 CPU 表。
	 *
	 * 为什么不能拿本帧的 CPU 表比：GPU 结果晚一帧，而 CPU 剔除每帧都跟着相机动 ⟹
	 * 相机一动就必然失配，对账变成永久告警 = 失去意义。比"同一帧的两侧、只是晚一帧报"
	 * 才是有效的等价性检查（docs/cpu_perf_plan.md §4 判据⑤的阳性锚点靠它工作）。
	 * @param list 本帧绘制列表（只用于顺序契约自检——该契约是结构性的，与滞后无关）
	 */
	void reconcileLagged(const FrameDrawList& list);

	/**
	 * 对账结论落地（债-22 门控 + 日志节流两层，两层判据不同、缺一不可）：
	 *
	 * 1. **门控**（本条债要补的那层）：本轮两侧序列与上一轮**逐元素相同** ⟹ 排序后的多重集
	 *    相同 ⟹ 比对结论相同 ⟹ 两次 `sort` 与比对整体免做。判据取"被比的那两份数据本身"，
	 *    不取上游版本号（相机 / spin / 候选表各记一号 = 三处都要记得 bump，漏一处对账就
	 *    **永久失明且不出声**）；它与被跳过的计算同源，构造上没有"漏"这个口子。
	 *    🔴 不失明的论证：结论是这两份序列的**纯函数**（排序 + `==`，无状态 / 无时钟 / 无
	 *    RNG），输入未变 ⟹ 结论未变 ⟹ 上一轮的告警（若有）已经打过，跳过不产生新的静默。
	 *    反过来，任何**新**失配必然伴随至少一侧序列变化 ⟹ 必然走到比对。首帧无基准 ⟹ 必算。
	 * 2. **日志节流**（既有，不动）：计数 / 失配状态变化才打，静止不刷屏。门控命中时连这层
	 *    都不进，且两者判据等价（输入相同 ⟹ 节流状态相同），故日志行为与门控前逐帧一致。
	 *
	 * 顺序契约自检 `checkCullAlignment` **不在本函数、也不受本门控影响**——它是结构性守卫，
	 * 每帧无条件跑（理由见 reconcile / reconcileLagged 注释）。
	 * @param cpuSlots CPU 侧 objectSlot 序列（调用方每帧重建的采集缓冲；**本函数就地排序**）
	 * @param gpuSlots GPU 侧 objectSlot 序列（同上）
	 */
	void reportReconcile(std::vector<uint32_t>& cpuSlots, std::vector<uint32_t>& gpuSlots);

	RGL::DeviceContext& device;
	OffGraphSync& sync;  // 图外同步发射账（Renderer 成员，声明序在 cullDriver 之前；转交给录制点）
	RGL::GpuCull* gpuCull;
	RGL::DgcCommands* dgc;
	RGL::DgcStaticCommands* dgcStatic;  // 静态 shader DGC 路（nullptr = 关；IES 路优先）
	uint32_t framesInFlight;                    // cull CB 数（= Renderer 帧 slot 数）
	CullReplayMode replayMode;                  // 构造期快照的门（见类注释三档表）
	std::vector<vk::raii::CommandBuffer> cullCommandBuffers;  // 每帧 slot 一个（`cpu` 档 = 空）
	std::vector<vk::raii::Fence> cullFences;    // 每帧 slot 一个（仅 gpuPassRuns()；不挂主 CB）
	// 滞后档的待收清单。为什么只需一份而非每 slot 一份：每帧最多提交一次 cull CB，
	// 而 `gpu_lagged` 在下一帧开头就把它收走 ⟹ 任意时刻至多一条在途（与 framesInFlight
	// 具体是几无关）。pendingCpuSet = 提交那一帧的 CPU opaque 表（滞后对账的 CPU 侧）。
	bool harvestPending = false;
	uint32_t pendingSlot = 0;
	std::vector<uint32_t> pendingCpuSet;
	uint32_t lastReconcileVisible = 0xFFFFFFFF;  // 对账日志节流（计数/失配变化才打）
	bool lastReconcileMatch = false;
	// ── 债-22 对账门控的上一轮输入（判据与不失明论证见 reportReconcile 声明注释）──
	// 存的是**采集序**（GPU 侧按 candidate 升序、CPU 侧按 opaque 段序），不是排序后的多重集：
	// 门控要在 sort 之前判，排序后存就没有"免 sort"这回事了。两侧各留一份本轮采集缓冲 +
	// 一份上一轮基准 = 四个 vector 常驻，稳态零堆分配（原形态是每帧 4 次分配：两个 collect
	// 各一次 + reportReconcile 按值形参各拷一次）。
	std::vector<uint32_t> cpuSlotsScratch;  ///< 本轮 CPU 侧序列缓冲（collect 写入，就地排序）
	std::vector<uint32_t> gpuSlotsScratch;  ///< 本轮 GPU 侧序列缓冲（同上）
	std::vector<uint32_t> memoCpuSlots;     ///< 上一轮 CPU 侧序列（采集序）
	std::vector<uint32_t> memoGpuSlots;     ///< 上一轮 GPU 侧序列（采集序）
	bool reconcileMemoValid = false;        ///< false = 还没有上一轮（首帧），本轮必算
};

}  // namespace RGL
