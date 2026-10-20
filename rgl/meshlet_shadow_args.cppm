module;
#include <vulkan/vulkan.h>  // VkBuffer 裸句柄（设备侧回读校验的 vkCmdCopyBuffer src；rlvk 不含宏）
export module meshlet_shadow_args;

import rlstd;
import rlvk;
import vkerror;
import vk_device;     // DeviceContext（描述符写入取 device 句柄）
import vk_resources;  // Resources（allocSlot / descriptor heap 槽地址 / VMA 分配器）
import VmaRaii;       // VmaBuffer / VmaAllocatorRAII
import mdi_rows;      // MdiShadowRow / kMdiShadowRowStride + 尺寸/偏移算术与同源 pair
import shader;        // BakedBindingMapping（Ⓐ 烘入 mapping 对账；同层无环：shader 不 import 本模块）

/**
 * meshlet_shadow_args —— 真 MDI 阴影段的**行表 GPU 载体**（docs/mdi_plan.md §19.5 ★M-1a，rlscene）
 *
 * ## 这批只做载体，一条 draw 都不接（🔴 零消费者）
 * 建一张 host 映射的行表 buffer + 领一个 descriptor heap 槽 + 把尺寸/偏移算术接上设备侧
 * 回读校验。**不改任何渲染路径**：没有 SPV 变体、没有管线条目、执行器一行不动。
 * 形态先例 = ★1-a0「只上传未消费，图像不变」（§10.5）与 ★18-B1「只建条目不接 draw」（§18.9）。
 * 于是出厂档截图必须仍逐位 = `95edc8cc…`，而日志里必须有本服务的 INFO 闭式锚 ⟹ 位比之前
 * 先证明「建了」（否则测的是两次「没建」）。
 *
 * ## ring×2 = **一张 buffer、一个槽**，轮转落在偏移上（§19.5 的设计收获）
 * 一张 buffer = `slices`（= `kFramesInFlight`）个等长 slice 首尾相接，描述符覆盖**整张**；
 * GPU 侧 `argsOffset` 与 shader 侧 `rowBase` 都由同一个 `frameSlot` 推出来（唯一产地 =
 * `mdiShadowRowLocation`）⟹ 分帧轮转不改句柄、不改描述符、不改管线条目。这正是
 * `MeshletCullService` 注释里骂过的「ring×2 要把条目表翻倍」那个代价，本形态绕开了。
 *
 * ## 🔴 为什么**不**套 `MeshletCullService` 的「单份 + 写前依赖 barrier」（§19.2 D4）
 * 那套的写者是 **GPU**：dispatch 前一条 barrier 排开上一帧的读。本表的写者是 **CPU**——
 * barrier 排不开主机写与 GPU 读（`eHost` 阶段的写只能靠内存可见性动作 + 分帧轮转排序），
 * 只有 ring×2 逐帧交替才是正确的形态（主机 fence 那是把同步点搬回来 = 本批要避免的代价）。
 *
 * ## 内存 = host-visible | host-coherent，CPU 直写映射（无 staging）
 * 分配口径照 `meshlet_geometry_impl.cppm::uploadSection`（= 现行大块只读 storage buffer 的
 * 唯一先例）：`VMA_MEMORY_USAGE_CPU_TO_GPU` + `MAPPED` + `HOST_ACCESS_SEQUENTIAL_WRITE`，
 * `memcpy` 进映射指针后**必刷** `vmaFlushAllocation`（映射内存不保证 coherent——独显无
 * large-BAR 时会落到非 coherent 系统内存；coherent 上这次调用是 VMA 内部提前返回，代价 0）。
 * usage 四件：`eIndirectBuffer`（VUID-buffer-02709 硬要求）+ `eStorageBuffer`（★M-1b 的
 * binding 22 让 shader 按 `rowBase + SV_DrawIndex` 读行）+ `eShaderDeviceAddress`（描述符
 * 走 BDA）+ `eTransferSrc`（只为设备级回读校验：本机统一内存上**映射回读是恒真判据**，
 * 真设备往返才证明「GPU 看到的那一行就是我写的那一行」——★1-a0 记过这条）。
 *
 * ## 🔴 尺寸/偏移算术**不在本模块重算**
 * slice 字节数、整张容量、`argsOffset`/`rowBase` 全取 `mdi_rows` 的具名公式（§19.5 末两条
 * buffer 边界 VUID 只能由一条闭式判据守）。本模块只做「按公式校验 → 不过就出声拒写」，
 * 自算第二个偏移 = 造第二个真相源，而它漂了的症状是画错几何而不是报错。
 *
 * ## 生命周期红线（AGENTS §生命周期红线 / docs/archive/issue_history.md 的 UAF 先例）
 * 本服务持一张 GPU buffer，**必须比 Renderer 活得久**（★M-1b 起 Renderer/执行器只持裸指针）。
 * 装配层声明序 = `wireMeshletPostRegistration` 返回的 `unique_ptr`，落在 main 的 try 块里
 * **先于 `backEnd`** 声明（析构序在其后）；🔴 不得放进 `main()` 的局部块——块尾析构 =
 * 第 0 帧起 GPU 侧引用已释放的 VA。
 *
 * ## 建不建 = 阴影簇路径的**宿主条件**（不是 `meshlet.mdi_shadow`）
 * 载体在 `meshlet.assets` 开 + `meshlet.shadow_lod ≥ 0` 且阴影簇路径**真生效**（ctx 里有
 * `MeshletShadowLodTable` = 有条目 474 的宿主）时才建；门关 = **连号都不领**（先例 =
 * `wireMeshletBindings` 首行 `if (!meshletAssets) return std::nullopt;`）。
 * 为什么载体的门不是 `mdi_shadow`：载体是「行表住在 GPU 里」这件事，与「阴影 draw 改用 MDI
 * 形态」这件事（那条才是 `mdi_shadow` = 选 SPV 变体/选条目，D2）分开 ⟹ 出厂默认态（mdi_shadow
 * = false）也建载体，但没有任何一条 draw 引用它 = 图像逐位不变。
 */

namespace RGL {

/**
 * ring×2 行表 buffer 的**裸载体**（不含 descriptor heap 槽）——设备级用例走这一层。
 *
 * 为什么单独成一层（而不是只有 `MeshletShadowArgsService`）：领槽要 `Resources`，而
 * `Resources` 构造要 Swapchain（= 拖进窗口/深度图/两个 heap，`test_meshlet_geometry` 文件头
 * 明确反对的做法）。设备侧真正要验的是「按 `rowBase`/`argsOffset` 取到的那一行是不是我写进去
 * 的那一行」，那件事只需要分配器 + 设备。槽由服务层领，描述符内容 = 本层的 `deviceAddress`。
 */
export struct MeshletShadowArgsRing {
	/** 整张行表（析构即 vmaDestroyBuffer；默认构造 = 空状态）。 */
	VmaBuffer buffer;
	/** 每个帧槽的行数（= `mdiShadowRowLocation` 的 `sliceRows` 入参，两处同一个变量）。 */
	uint32_t sliceRows = 0;
	/** 帧槽数（= `kFramesInFlight`；🔴 不抄字面 2，由调用方取那个唯一真相源）。 */
	uint32_t slices = 0;
	/** 整张字节容量 = `mdiShadowArgsCapacityBytes(sliceRows, slices)`（描述符 range 取本值）。 */
	uint64_t capacityBytes = 0;
	/** BDA 设备地址（0 = 未创建；句柄真值判据用它，不是 `capacityBytes > 0`）。 */
	vk::DeviceAddress deviceAddress = 0;
	/** host 映射指针（CPU 直写的目的端；nullptr = 未映射 = 载体不可用）。 */
	std::byte* mapped = nullptr;

	/**
	 * 一次 MDI 调用的同源 pair（🔴 只做转发，不在这里算偏移：产地 = `mdiShadowRowLocation`）。
	 *
	 * 前置校验（任一不成立返回的 pair **不可用**，调用方必须走 `upload` 那条带错误串的路或
	 * 自己先查）：`frameSlot < slices`、`firstRow < sliceRows`。
	 *
	 * @param frameSlot 帧槽号（与 GPU 侧同一个数，不许另取）
	 * @param firstRow  本段（本级联）在 slice 内的起始行号
	 * @return { argsOffset（字节）, rowBase（行号）}，两者同源于同一个 row
	 */
	[[nodiscard]] MdiShadowRowLocation location(uint32_t frameSlot, uint32_t firstRow) const;

	/**
	 * 本次调用在行表里**实际被设备读到**的字节跨度 = `mdiShadowTailBytes(…) − argsOffset`
	 * （= VUID-07090 的 `stride × (drawCount − 1) + 12`；`drawCount == 0` = 0）。
	 *
	 * 它不是「行数 × 步长」：每行只有前 12B 被读走，多出来的保留 word 不算进跨度。
	 * 用途 = 设备侧回读校验要拷多少字节，以及 `upload` 的边界判据输入。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param drawCount 本次 MDI 调用的行数
	 */
	[[nodiscard]] uint64_t argsBytes(uint32_t frameSlot, uint32_t firstRow, uint32_t drawCount) const;

	/**
	 * 把 `rows` 写进 `(frameSlot, firstRow)` 起的那段行表（memcpy + flush，🔴 不做设备侧同步）。
	 *
	 * 四道前置（顺序 = 由外而内，每条一个**两两不同**的错误串前缀 `meshlet shadow args:`）：
	 *  1. `frameSlot < slices`（越界 = 写进 buffer 尾外的设备内存，不报错只画垃圾）；
	 *  2. `firstRow + rows.size() ≤ sliceRows`（越界 = 踩进下一个帧槽 = 读回上一帧的行）；
	 *  3. `mdiShadowRangeFits(capacityBytes, argsOffset, rows.size())`（07089/07090 合一的
	 *     buffer 边界判据，与容量同一公式）；
	 *  4. 映射指针非空（`MAPPED_BIT` 没生效 = 主机写写进 null，必崩，早退成可归因错误）。
	 * 任一不过 = **一个字节都不写**（半张行表比整张旧行表更坏：逐行读歪没有信号）。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param rows      本段的行（行序 = 候选序，契约在 `mdi_rows` 文件头）；空 span = 合法空操作
	 * @return nullopt = 已写入并 flush；有值 = 可归因错误串（★M-1b 把它打进 WARNING，不抛——
	 *         这是每帧路径）
	 */
	[[nodiscard]] std::optional<std::string> upload(uint32_t frameSlot, uint32_t firstRow,
		std::span<const MdiShadowRow> rows) const;
};

/**
 * 建 ring×2 行表 buffer（host 映射直写形态；设备级用例与装配层共用这一条路径）。
 *
 * 创建后**整张清 0**：VMA 不清内存，未写入区留脏字节 ⟹「按 rowBase 取到的行」在空洞段上
 * 没有确定读数（★M-1b 的孔洞流靠 `gcX = 0` 空转，而 `objectIndex`/`clusterStart` 是垃圾时
 * 空转行也可能被 shader 读走）。清 0 让「没写过的行」成为可判据的事实，也让设备回读用例
 * 能把「未写入区」当反向锚点用。
 *
 * 两道闸门（不过 = 返回 `Error`，不建 buffer）：
 *  · `sliceRows > 0 && slices > 0`（`VkBufferCreateInfo::size` 必须 > 0，零容量无意义）；
 *  · `sliceRows × slices ≤ UINT32_MAX`（`rowBase` 是 push 里的 32 位行号；越界 = 静默截断
 *    = shader 读错行，正是本模块要避免的那类失效）。
 *
 * @param allocator VMA 分配器（引擎侧 = `Resources::getAllocator()`；测试自建第二分配器走同一条路）
 * @param device    逻辑设备（BDA 查询用）
 * @param sliceRows 每个帧槽的行数（= 一个级联段的行数上限，装配层给闭式锚）
 * @param slices    帧槽数（= `FrameSession::kFramesInFlight`）
 * @return 载体（含 `capacityBytes` / `deviceAddress` / `mapped`）；失败 = 可归因 `Error`
 */
export [[nodiscard]] std::expected<MeshletShadowArgsRing, Error> createMeshletShadowArgsRing(
	const VmaAllocatorRAII& allocator, const vk::raii::Device& device, uint32_t sliceRows,
	uint32_t slices);

/**
 * 审计执行账的一行汇总（`diagnostics.heap_slot_audit` 开过才出声；调用点 = main 退出收口）。
 *
 * 为什么需要：审计**通过时不逐帧出声**，于是"日志里零 ERROR"分不清是"审了 N 帧都没问题"
 * 还是"一次都没审"——两者对归因的含义相反。构造期那行证明**装上了**，这本账证明**跑过**。
 */
export void reportHeapSlotAuditSummary();

/**
 * 描述符堆**槽内容**对账的结论（债-140 第三击；纯数据 = 无设备可单测）。
 *
 * ## 为什么需要它（而两网都不够）
 * `writeSlotDescriptor()` 只在构造期写一次，之后**无人复核**。devcoredump 实测 fault 地址恰 =
 * 0x0（空指针签名），而它的直接对应形态就是"**这个槽里现在是 0**"——task shader 经 binding 34
 * 读行表时基址为 0，`Load(0)` 落在 VA 0。ASan 看不见（描述符堆是 mmap 的设备可见内存，不在
 * 它的 CPU 侧插桩面里），GPU-AV 也看不见（它查描述符**索引**越界，不查槽里存的**地址值**）。
 * 所以只能自己读回来对账。
 *
 * ## 为什么主判据是"与启动快照逐字节相同"而不是"等于 deviceAddress"
 * 描述符的内存布局是**驱动私有**的（RADV 的 storage buffer 描述符里除了 8B 地址还有 size/
 * 控制位，NVIDIA 又是另一套）。拿"槽前 8 字节 == BDA"当判据 = 造一条"换台机器就红"的假判据。
 * 快照对账与布局无关：**我们写进去的那 32/64 个字节，现在还是不是那些字节**。
 * `containsExpectedAddress` 只作**附带信息**报出（"我们写的那个地址还在不在槽里"），
 * 不参与 `ok()` 的判定——布局不含裸地址的驱动上它恒 false，那是对的，不是错误。
 */
export struct HeapSlotAudit {
	bool sizeMismatch = false;          ///< 两次读数长度不同 = 调用方 bug（不是驱动问题）
	bool matchesSnapshot = true;        ///< 与启动快照逐字节相同
	bool allZero = false;               ///< 🔴 当前全零 = fault@0 的直接对应形态
	bool containsExpectedAddress = false;  ///< 期望 BDA 以 8B 小端出现在槽内（仅信息，不判红）
	uint32_t slot = 0;                  ///< 被审的 heap 槽号
	uint64_t expectedAddress = 0;       ///< 我们写进描述符的那个 BDA
	bool hasDiff = false;               ///< 与快照有差异（长度相同时才有意义）
	uint32_t firstDiffByte = 0;         ///< 首个差异所在字节下标
	uint32_t snapshotSize = 0;          ///< 快照长度（长度不符时用来点名两边各多大）
	std::string currentHex;             ///< 当前槽内容 hex；🔴 **只在判负时填**（拼 hex 比整条对账还贵，见 impl）
	/**
	 * 对账是否通过。
	 *
	 * 🔴 `allZero` 单独把结果判负，**即使它与快照相同**：我们写进去的是一个真实 BDA，槽里
	 * 因此不可能全零。"快照全零且当前也全零"不是"稳定"，而是"**构造期那次描述符写入根本没
	 * 生效**"——那正是 fault@0 最直接的成因，绝不能被"与基准相同"洗成通过。
	 */
	[[nodiscard]] bool ok() const { return !sizeMismatch && matchesSnapshot && !allZero; }
};

/**
 * 对账本体（**纯函数**：两段字节 + 期望地址进，结论出）。
 * @param current  当前从 heap 槽读回的字节
 * @param snapshot 构造期写完描述符后立刻拍的快照（判据基准）
 * @param expectedDeviceAddress 写进描述符的 BDA（0 = 载体未创建，此时不查"地址在不在"）
 * @param slot 槽号（只为让结论能点名）
 */
export HeapSlotAudit auditHeapSlot(std::span<const std::byte> current,
	std::span<const std::byte> snapshot, uint64_t expectedDeviceAddress, uint32_t slot);

/**
 * 结论 → 一行可读文本（ERROR 正文：槽号 / 期望 BDA / 当前 16 字节十六进制 / 判定）。
 *
 * 🔴 三种失败形态的措辞**必须两两可分**，它们指向不同的下一步：
 *  ① 全零 → "槽内容全零（fault@0 的直接对应形态）"，下一步查谁把 0 写进了这个槽；
 *  ② 非零但与快照不同 → "与启动快照不符 = 有人改写了这个槽"，下一步查谁复用了这个槽号；
 *  ③ 长度不同 → 调用方 bug（本模块自己算错了 descriptorSize）。
 */
export std::string describeHeapSlotAudit(const HeapSlotAudit& audit);

/**
 * **烘入 mapping** 对账的结论（债-140 第四击 Ⓐ；纯数据 = 无设备可单测）。
 *
 * ## 它补的是上一击的哪半个盲区
 * `heap_slot_audit` 审的是**服务认为自己那枚**槽（`argHeapSlot`）里的字节。但 GPU 读的不是
 * "服务认为的那枚"，而是**条目烘进去的那枚** —— 两者由两条独立代码路径给出：
 *   · 服务：`writeSlotDescriptor()` 把 BDA 写进 `argHeapSlot`；
 *   · 条目：`addShadowMeshletMdiEntry(clusterSlots, argsSlot)` → `bakeBindingMappings`
 *     把 `slotBytes(argsSlot, descriptorSize)` 烘成 binding 34 的 heapOffset。
 * 只要这两条路给出不同的槽号，症状就是**服务往 Y 写、GPU 从 X 读**：X 里可能是 0，
 * 也可能是别的 buffer 的描述符 —— 而 CPU 侧对 Y 的审计**永远看不到这件事**。
 * 本对账把另一半补上。
 *
 * ## 换算式（🔴 必须是 `slotBytes` 的逆运算，且要顶住"除不尽"）
 * 烘的时候是 `heapOffset = slot * descriptorSize`（唯一合法拼写 =
 * `shaderDetail::slotBytes`，债-13 的坑 5 护栏）。所以读回来是
 * `slot = heapOffset / descriptorSize`，**且仅当** `heapOffset % descriptorSize == 0`。
 * 除不尽 = 这条 mapping 的 heapOffset 不是资源堆槽偏移（例如误传了 sampler 堆的字节偏移），
 * 那要单独点名 —— 拿它整除出一个槽号去比"槽号不符"，会把单位错误报成接线错误。
 *
 * ## 为什么判据是"与烘之前记录的期望比"而不是"与闭式 BDA 比"
 * 上一击刚踩过：描述符的内存布局是驱动私有的。mapping 的**值**虽然是我们自己算的，
 * 但"期望值"仍然只能来自**同一条代码路径记下的期望**（服务的槽号 × 步长），
 * 不能另起一处闭式重算 —— 那正是本项目"单一来源不承重"那条要堵的形态。
 */
export struct BakedMappingAudit {
	bool bindingFound = false;    ///< 烘入 mapping 里**有**这个 binding（没有 = 声明与 mapping 分家）
	bool sizeUsable = false;      ///< descriptorSize > 0（= 0 时换算式无从执行）
	bool strideMatches = false;   ///< heapArrayStride == descriptorSize（否则这条 mapping 描述的不是一枚资源槽）
	bool offsetSlotAligned = false;  ///< heapOffset 是 descriptorSize 的整数倍
	bool slotMatches = false;     ///< 换算出的槽号 == 服务写入的槽号
	uint32_t binding = 0;         ///< 被审的 binding 号（本引擎 = 34）
	uint32_t bakedSlot = 0;       ///< heapOffset / descriptorSize（仅在对齐时有意义）
	uint32_t expectedSlot = 0;    ///< 服务的 argHeapSlot
	uint32_t descriptorSize = 0;  ///< 一枚描述符占的字节数
	uint64_t bakedHeapOffset = 0;    ///< 烘进去的字节偏移
	uint64_t expectedHeapOffset = 0; ///< 期望的字节偏移（expectedSlot × descriptorSize）
	/** 对账是否通过（五项全真）。 */
	[[nodiscard]] bool ok() const {
		return bindingFound && sizeUsable && strideMatches && offsetSlotAligned && slotMatches;
	}
};

/**
 * 对账本体（**纯函数**：烘入 mapping 列表 + 期望进，结论出）。
 * @param baked 条目实际烘进去的 mapping（`RenderPipeline::bakedMappings()`）
 * @param binding 要审的 binding 号（行表 = 34）
 * @param expectedSlot 服务实际写入描述符的那枚槽（`MeshletShadowArgsService::argSlot()`）
 * @param descriptorSize 描述符步长（同上来源，🔴 不在这里另算）
 */
export BakedMappingAudit auditBakedRowMapping(std::span<const BakedBindingMapping> baked,
	uint32_t binding, uint32_t expectedSlot, uint32_t descriptorSize);

/**
 * 结论 → 一行可读文本（五种现场措辞**两两不同**，各指一个不同的下一步）。
 *
 * 🔴 尤其不许混的两条：① "条目根本没烘 binding 34"（GPU 读未映射 binding，基址来源
 * 未定义 —— fault@0 的一条**正面**来路）；② "烘的是 X 而服务写的是 Y"（两条路分家）。
 * ① 的下一步是查批 M 的声明过滤与 SPV，② 的下一步是查装配层的参数传递。
 */
export std::string describeBakedRowMappingAudit(const BakedMappingAudit& audit);

/**
 * 对账 + 出声（启动期一次性；调用点 = `wireMeshletMdiShadowActivation`）。
 *
 * 🔴 **判负路径不经门**：五项里任何一项不符都是**接线 bug**，判据两侧的值来自同一条代码
 * 路径、与驱动无关 ⟹ 不存在"换台机器就红"的假阳性，把它做成可关的开关只会让它在需要它的
 * 那次跑里恰好没开。门（`heap_slot_audit`）只管**通过时**的那行 INFO 正读数。
 * @param baked 条目实际烘进去的 mapping
 * @param entryName 条目名（进正文，多个条目时能认出是哪条）
 * @param binding 被审 binding 号
 * @param expectedSlot 服务写入的槽号
 * @param descriptorSize 描述符步长
 */
export void logBakedRowMappingAudit(std::span<const BakedBindingMapping> baked,
	std::string_view entryName, uint32_t binding, uint32_t expectedSlot,
	uint32_t descriptorSize);

/**
 * 装配层持有的**行表载体 + 一个 heap 槽**（★M-1a 的对外服务；ring×2 只在这一个对象里）。
 *
 * 构造做三件事（顺序 = 先领号后建 buffer 都行，但描述符必须在 buffer 地址已知之后写）：
 *  ① `resources.allocSlot(SlotSegment::Runtime, "meshlet.gpu.shadowArgs")` 领**一枚**槽
 *     （名字带 `meshlet.gpu.` 前缀 = 既有命名法，`wireMeshletBindings` 同源）；
 *  ② `createMeshletShadowArgsRing` 建整张 buffer；
 *  ③ 把 BDA 写成 storage buffer 描述符，**range = 整张容量**（覆盖所有 slice ⟹ 逐帧轮转
 *     不改描述符），host 直写不进命令流（`MeshletCullService::writeSlotDescriptor` 同法）。
 * 失败一律抛 `std::runtime_error`（启动期一次性路径，同 `MeshletCullService`）。
 *
 * 🔴 本批**没有任何消费者**：`argSlot()` 是给 ★M-1b 烘 binding 22 用的（届时条目侧声明
 * binding 与领槽必须同向，见 §19.5 的「声明 / mapping 必须同向」那条），`bufferAddress()`
 * 是给 `vkCmdDrawMeshTasksIndirectEXT` 的 `buffer` 参数用的（同一个 BDA 走两条路：
 * vkCmd 收句柄 + offset，shader 收描述符里的 address range）。
 */
export class MeshletShadowArgsService {
   public:
	/**
	 * 构造（启动期一次，由装配层的宿主条件决定建不建）。
	 *
	 * @param device    设备层（描述符写入取 device 句柄）
	 * @param resources 资源层（VMA 分配器 / allocSlot / descriptor heap 槽地址与步长）
	 * @param sliceRows 每个帧槽的行数 = 一个级联段的行数上限（装配层给**闭式锚**，见 INFO）
	 * @param slices    帧槽数（= `FrameSession::kFramesInFlight`，🔴 不在本模块写死 2）
	 * @throw std::runtime_error 领槽 / buffer 创建 / 描述符写入失败（启动期一次性路径）
	 */
	MeshletShadowArgsService(DeviceContext& device, Resources& resources, uint32_t sliceRows,
		uint32_t slices);
	/** 拷贝删除：本类持一张 GPU buffer + 一个 heap 槽，所有权唯一（同 `MeshletCullService`）。 */
	MeshletShadowArgsService(const MeshletShadowArgsService&) = delete;
	/** 拷贝赋值删除：同上。 */
	MeshletShadowArgsService& operator=(const MeshletShadowArgsService&) = delete;
	/** 移动删除：槽号届时已烘进管线条目（★M-1b），挪窝 = 条目指向的槽不变而 buffer 换了主人。 */
	MeshletShadowArgsService(MeshletShadowArgsService&&) = delete;
	/** 移动赋值删除：同上。 */
	MeshletShadowArgsService& operator=(MeshletShadowArgsService&&) = delete;

	/**
	 * 一次 MDI 调用的同源 pair（转发环层，🔴 不在调用点自己算偏移——见 `mdiShadowRowLocation`）。
	 *
	 * @param frameSlot 帧槽号（= `FrameSession::currentFrame`）
	 * @param firstRow  本段（本级联）在该帧槽内的起始行号
	 */
	[[nodiscard]] MdiShadowRowLocation location(uint32_t frameSlot, uint32_t firstRow) const;

	/**
	 * 本次调用的字节跨度（= `mdiShadowTailBytes − argsOffset`，用途见 `MeshletShadowArgsRing::argsBytes`）。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param drawCount 本次 MDI 调用的行数
	 */
	[[nodiscard]] uint64_t argsBytes(uint32_t frameSlot, uint32_t firstRow, uint32_t drawCount) const;

	/**
	 * 写一段行进 ring（每帧路径；🔴 失败返回错误串而不抛，四道前置见环层注释）。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param rows      本段的行（空 span = 合法空操作）
	 */
	[[nodiscard]] std::optional<std::string> upload(uint32_t frameSlot, uint32_t firstRow,
		std::span<const MdiShadowRow> rows) const;

	/** @return 行表的 descriptor heap 槽号（★M-1b 烘进 MDI 变体的 binding 22）。 */
	[[nodiscard]] uint32_t argSlot() const { return argHeapSlot; }
	/** @return 整张行表的 BDA 设备地址（描述符内容；0 = 未创建，句柄真值判据用它）。 */
	[[nodiscard]] vk::DeviceAddress bufferAddress() const { return ring.deviceAddress; }
	/** @return 整张容量字节数（= 描述符 range，也是 07089/07090 的判据输入）。 */
	[[nodiscard]] uint64_t capacityBytes() const { return ring.capacityBytes; }
	/** @return 每个帧槽的行数（闭式锚，INFO 与 ★M-1b 的对账都读它）。 */
	[[nodiscard]] uint32_t sliceRows() const { return ring.sliceRows; }
	/** @return 帧槽数（轮转分母）。 */
	[[nodiscard]] uint32_t slices() const { return ring.slices; }

	/**
	 * @return 裸 VkBuffer 句柄。
	 * 🔴 只给**设备级回读校验**用（`vkCmdCopyBuffer` 的 src 收句柄而不是地址）；渲染路径
	 * 一律走 `argSlot()` 的描述符或 `bufferAddress()`，别拿这个句柄去建第二条访问路。
	 */
	[[nodiscard]] VkBuffer buffer() const { return ring.buffer.buffer; }
	/**
	 * @return host 映射指针（CPU 写侧目的端）。
	 * ⚠ 统一内存上「读回它」是**恒真判据**（★1-a0 记过）：只作设备往返之外的辅助锚点。
	 */
	[[nodiscard]] const std::byte* mappedData() const { return ring.mapped; }

	/**
	 * 读回本服务那个 heap 槽的当前内容并与构造期快照对账（债-140 第三击 ①）。
	 *
	 * 只在 `diagnostics.heap_slot_audit=true` 时被调用（调用点 = `Renderer::prepareMdiShadowRows`，
	 * 每帧一次，在 upload 之前）。返回 nullopt = 快照未拍（构造未完成或 descriptorSize 读不到），
	 * 此时**调用方必须出声**——"审计没跑"与"审计说没问题"是两件事。
	 *
	 * 成本：一次 `getSlotCpuAddress` + 一次 ≤ descriptorSize 的 memcmp（本机 32B）。描述符堆是
	 * host-visible 映射，读它不碰总线、不进命令流（开销实测 → `docs/还债实测数据.md` §4.11）。
	 */
	[[nodiscard]] std::optional<HeapSlotAudit> auditSlot() const;

	/**
	 * 一枚描述符占的字节数（= 描述符堆的 descriptorSize，构造期取一次存下）。
	 * 🔴 Ⓐ 对账的期望值必须用**这一个**数：烘那一步用的也是它
	 *（`slotBytes(argsSlot, descriptorSize)`），两处各查一次 = 两个真相源。
	 */
	[[nodiscard]] uint32_t descriptorBytes() const { return descriptorSize; }

	/**
	 * 登记"本条目实际会读的全部 heap 槽"（债-140 夜间批）。
	 *
	 * 🔴 为什么Ⓐ不够：Ⓐ 只比对了 binding 34 一枚槽。但条目烘了 8 条 mapping（scene data
	 * 子范围 + 簇 buffer 17-20 + 行表 34），**任何一枚是零描述符 = 基址 0 = fault@0 的直接
	 * 来源**，而 devcoredump 从来没说 fault 是哪条 binding 触发的。本登记让"逐帧审计"与
	 * "device-lost 现场 dump"两条路都能覆盖全部 8 枚。
	 * @param baked 条目实际烘进去的 mapping（`RenderPipeline::bakedMappings()`）
	 */
	void registerEntrySlots(std::span<const BakedBindingMapping> baked);

	/**
	 * 全部已登记槽的内容读数（每枚一行 `binding N 槽 M [hex]`，全零的标 `🔴零描述符`）。
	 *
	 * 用途两处：① `diagnostics.heap_slot_audit` 开时逐帧查（发现零描述符**只报一次**，
	 * 避免每帧刷）；② **device-lost 抓取点无条件 dump** —— 这是 CPU 侧能拿到的、最接近
	 * "GPU 当时看到什么"的东西（槽内容在 host 可见映射上，device lost 之后仍可读）。
	 */
	[[nodiscard]] std::string describeEntrySlots() const;

	/** 逐帧审计入口（门 = `heap_slot_audit`）：发现全零描述符即 ERROR，每个 binding 只报一次。 */
	void auditEntrySlotsOnce() const;

	/** 构造期拍的槽内容快照（审计的判据基准；空 = 未拍）。 */
	[[nodiscard]] std::span<const std::byte> slotSnapshot() const { return slotSnapshotBytes; }

	/**
	 * 审计 + 出声的一等入口（债-140 第三击 ①；调用条件 = `diagnostics.heap_slot_audit`）。
	 *
	 * 三条出口，措辞两两不同：① 无基准（快照为空）→ warn「无读数，不可读成对账通过」；
	 * ② 对账通过 → **不逐帧出声**（每帧一行"没问题"会冲掉日志），改由**首次调用打一行**
	 *    装桩证据（含基准快照 hex）——上批刚踩过"审计/校验其实没跑而日志零 ERROR"的坑，
	 *    没有这行，"零 ERROR"就分不清是"零调用"还是"全部通过"，两者对归因的含义相反；
	 * ③ 不符 → ERROR 全文（槽号 / 期望 BDA / 当前 hex / 首个差异字节）。
	 * @param frameSlot 本帧录制槽（进正文，把现场对到某一帧）
	 * @param segments 本帧 MDI 段数（同上）
	 */
	void auditAndLog(uint32_t frameSlot, uint32_t segments) const;

   private:
	/** 构造期：领 Runtime 段的一枚 heap 槽（ring×2 一张 buffer ⟹ 一槽，见类注释③）。 */
	void allocArgSlot();
	/**
	 * 构造期：建整张 ring×2 buffer（分配口径与四道 usage 见文件头；失败抛，启动期一次性路径）。
	 *
	 * @param sliceRows 每个帧槽的行数
	 * @param slices    帧槽数
	 */
	void createRing(uint32_t sliceRows, uint32_t slices);
	/**
	 * 构造期：把 ring 的 BDA 写成槽的 storage buffer 描述符（range = 整张容量）。
	 *
	 * 描述符内容 = 同一张 buffer 的地址，与 `MeshletCullService::writeSlotDescriptor` 同款
	 * （`writeResourceDescriptorsEXT` + `vk::DeviceAddressRangeEXT`，host 直写）。
	 */
	void writeSlotDescriptor();

	DeviceContext& device;    ///< 设备层（描述符写入）
	Resources& resources;     ///< 资源层（分配器 / allocSlot / heap 槽地址与步长）
	MeshletShadowArgsRing ring;  ///< 整张 ring×2 行表（本类唯一 GPU 资源，析构即销毁）
	uint32_t argHeapSlot = 0; ///< 行表 heap 槽号（★M-1b 的 binding 22 取它）
	/**
	 * 写完描述符后**立刻**从 host 映射读回的槽内容快照（`auditSlot()` 的判据基准）。
	 * 恒拍（不看诊断门）：成本 = 构造期一次 ≤64B 拷贝，换来"审计随时可开而基准不依赖门"。
	 */
	std::vector<std::byte> slotSnapshotBytes;
	/** 条目实际会读的全部 (binding, 槽号)（`registerEntrySlots` 填；换算式同 Ⓐ）。 */
	std::vector<std::pair<uint32_t, uint32_t>> entrySlots;
	uint32_t descriptorSize = 0;  ///< 一枚描述符的字节数（构造期取；Ⓐ 换算式的分母）
};

/**
 * INFO 的**闭式锚**（★M-1a 要求「让人能一眼对账」的那五个数），全部由 `mdi_rows` 的具名
 * 公式算出——**不许**从 `MeshletShadowArgsRing` 的成员读（那是被测对象自己报数，
 * 与公式分家时日志仍然自洽 = 假绿；同 `mdi_rows` 的「聚合锚期望值独立重算」纪律）。
 *
 * 最坏形态（`firstRow = 0, drawCount = sliceRows` 落进**末个**帧槽）是容量必须覆盖的那一格：
 * 它 = `mdiShadowArgsCapacityBytes` 减 `stride` 再加 12（`stride ≥ 12` 恒成立 ⟹ 永不越界，
 * 且容量少 1 字节立刻红，见 tests/test_mdi_rows）。
 */
export struct MeshletShadowArgsAnchors {
	uint32_t rows = 0;      ///< 每个帧槽的行数（闭式锚 = 级联数 × 本值 = 启动对账的 Σrows）
	uint32_t slices = 0;    ///< 帧槽数（= `kFramesInFlight`）
	uint32_t stride = 0;    ///< 行步长（= `kMdiShadowRowStride`，VUID-07088 的 stride）
	uint64_t capacity = 0;  ///< 整张字节容量（= `mdiShadowArgsCapacityBytes`）
	uint64_t tail = 0;      ///< 末行 `offset + 12`（= `mdiShadowTailBytes`，07089/07090 的左边）
};

/**
 * 算出上面那五个数（纯公式，无设备）。
 * @param sliceRows 每个帧槽的行数
 * @param slices    帧槽数
 */
export [[nodiscard]] MeshletShadowArgsAnchors meshletShadowArgsAnchors(uint32_t sliceRows,
	uint32_t slices);

}  // namespace RGL
