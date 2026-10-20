module;
#include <vk_mem_alloc.h>  // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 只导出 C++ 包装）
#include <fmt/format.h>
module meshlet_shadow_args:impl;

import meshlet_shadow_args;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import VmaRaii;
import mdi_rows;
import diagnostics;  // heap_slot_audit 门（构造期装桩证据是否出声）

namespace RGL {
namespace {

/**
 * `upload` 四道前置各自的错误串前缀（🔴 **两两不同**是判据的一部分：★M-1b 把它打进 WARNING，
 * 读日志的人要能分清是哪道闸门拦下的，而不是回来读代码。同 `mdi_rows` 的不变量前缀纪律）。
 */
constexpr const char* kErrFrameSlot = "meshlet shadow args: frame slot out of range";
constexpr const char* kErrRowsOverSlice = "meshlet shadow args: rows exceed the slice";
constexpr const char* kErrBufferBounds = "meshlet shadow args: indirect range exceeds buffer";
constexpr const char* kErrNoMapping = "meshlet shadow args: mapped pointer is null";

/**
 * 描述符槽字节 → 空格分隔 hex（审计的两处日志共用：`auditHeapSlot` 的现场 hex 与
 * `auditAndLog` 的基准快照 hex。分两处写就会漂，而这两段是要**对读**的）。
 *
 * 🔴 上限必须盖得住整个描述符（本机 descriptorSize = 32）：截断的话，差异落在截断点之后时
 * ERROR 会报"不符"而附带的 hex 与基准长得一模一样 = 把现场拍糊了。
 */
std::string formatSlotBytes(std::span<const std::byte> bytes, size_t cap = 64) {
	std::string out;
	for (size_t i = 0; i < bytes.size() && i < cap; ++i)
		out += fmt::format("{}{:02x}", i == 0 ? "" : " ", static_cast<unsigned>(bytes[i]));
	return out;
}

/** 一段描述符字节是否全零（= 零描述符 = 基址 0，fault@0 的直接来源）。 */
bool bytesAllZero(std::span<const std::byte> bytes) {
	for (const auto b : bytes)
		if (b != std::byte{0})
			return false;
	return true;
}

/** binding 是否已经报过（表最多 8 项，线性扫；手写而不是 std::find——modules 坑）。 */
bool alreadyReported(const std::vector<uint32_t>& reported, uint32_t binding) {
	for (const uint32_t b : reported)
		if (b == binding)
			return true;
	return false;
}

/** 8 字节小端形态是否出现在 span 内**任意**字节偏移（`auditHeapSlot` 的附带信息专用）。 */
bool containsAddressLE(std::span<const std::byte> bytes, uint64_t form) {
	if (form == 0ull || bytes.size() < 8)
		return false;
	std::byte want[8] {};
	for (int i = 0; i < 8; ++i)
		want[i] = static_cast<std::byte>((form >> (8 * i)) & 0xFFull);
	for (size_t o = 0; o + 8 <= bytes.size(); ++o) {
		bool hit = true;
		for (int i = 0; i < 8; ++i)
			if (bytes[o + static_cast<size_t>(i)] != want[i]) {
				hit = false;
				break;
			}
		if (hit)
			return true;
	}
	return false;
}

/**
 * heap 槽审计的执行账（进程级；只有主线程的录制路径会写它，无并发）。
 *
 * 为什么需要单独一本账：审计**通过时不逐帧出声**（每帧一行"没问题"会把日志冲干净），
 * 于是"日志里零 ERROR"分不清是"审计跑了 N 帧都没问题"还是"审计一次都没跑"——
 * 两者对归因的含义相反。构造期那行证明**装上了**，这本账证明**跑了多少帧**。
 */
struct AuditStats {
	uint64_t frames = 0;      ///< 实际执行过对账的帧数
	uint64_t mismatch = 0;    ///< 判负次数（含三种现场）
	uint64_t noBaseline = 0;  ///< 无基准（快照为空）次数
};
/** 审计执行账的可变单例（函数内 static = 无静态初始化顺序问题；只在主线程的录制路径上写）。 */
AuditStats& auditStats() {
	static AuditStats stats;
	return stats;
}

}  // namespace

MdiShadowRowLocation MeshletShadowArgsRing::location(uint32_t frameSlot, uint32_t firstRow) const {
	// 🔴 只做转发，不在这里乘步长、不在这里加帧槽偏移（§19.5：`argsOffset` 与 `rowBase` 必须
	// 同源于同一个 frameSlot 与同一个 firstRow；两处各算一遍迟早漂，而漂的症状是画错几何）。
	return mdiShadowRowLocation(sliceRows, frameSlot, firstRow);
}

uint64_t MeshletShadowArgsRing::argsBytes(uint32_t frameSlot, uint32_t firstRow,
	uint32_t drawCount) const {
	const MdiShadowRowLocation loc = location(frameSlot, firstRow);
	// 跨度 = 尾字节 +1 减起点（`drawCount == 0` 时尾 == 起点 ⟹ 0，不回绕）。
	// 🔴 不是 `drawCount × stride`：每行只有前 12B 被设备读走，保留 word 不算进跨度。
	return mdiShadowTailBytes(loc.argsOffset, drawCount) - loc.argsOffset;
}

std::optional<std::string> MeshletShadowArgsRing::upload(uint32_t frameSlot, uint32_t firstRow,
	std::span<const MdiShadowRow> rows) const {
	// 顺序 = 由外而内（先「这一格存不存在」，再「这一格放不放得下」，最后才是映射可用性）。
	if (frameSlot >= slices)
		return fmt::format(
			"{}: frameSlot {} >= slices {} — refusing to write into the neighbour slot (that "
			"would read last frame's rows, not error out)", kErrFrameSlot, frameSlot, slices);
	const uint64_t endRow = static_cast<uint64_t>(firstRow) + rows.size();
	if (endRow > sliceRows)
		return fmt::format(
			"{}: firstRow {} plus {} rows exceed sliceRows {} — row order is the contract, "
			"the caller must size the cascade segments inside one slice", kErrRowsOverSlice,
			firstRow, rows.size(), sliceRows);
	if (rows.empty())
		return std::nullopt;  // 本级联一个候选都没有 = 合法空操作（0 行不读任何字节）
	const MdiShadowRowLocation loc = location(frameSlot, firstRow);
	// 🔴 VUID-…-drawCount-07089/07090 合一的 buffer 边界判据（违例的症状不是崩溃而是读越界
	// 当命令；门关档不跑 VVL 就不现形，所以这里必须自己拦，不指望验证层）。
	if (!mdiShadowRangeFits(capacityBytes, loc.argsOffset, static_cast<uint32_t>(rows.size())))
		return fmt::format(
			"{}: argsOffset {} with {} rows would read up to byte {} but the buffer holds {} — "
			"the capacity formula and the offset formula have diverged", kErrBufferBounds,
			loc.argsOffset, rows.size(),
			mdiShadowTailBytes(loc.argsOffset, static_cast<uint32_t>(rows.size())),
			capacityBytes);
	if (mapped == nullptr)
		return fmt::format(
			"{}: VMA_ALLOCATION_CREATE_MAPPED_BIT not honored — CPU direct write has no "
			"destination", kErrNoMapping);
	std::memcpy(mapped + loc.argsOffset, rows.data(), rows.size_bytes());
	// 映射内存**不保证** host-coherent（独显无 large-BAR 时 CPU_TO_GPU 落到非 coherent 系统
	// 内存）；coherent 上这次调用是 VMA 内部提前返回，代价为零（meshlet_geometry 同款）。
	vmaFlushAllocation(buffer.allocatorBlock->handle, buffer.allocation, loc.argsOffset,
		rows.size_bytes());
	return std::nullopt;
}

std::expected<MeshletShadowArgsRing, Error> createMeshletShadowArgsRing(
	const VmaAllocatorRAII& allocator, const vk::raii::Device& device, uint32_t sliceRows,
	uint32_t slices) {
	// 闸门①：零容量不建 buffer（`VkBufferCreateInfo::size` 必须 > 0）。这里不像 meshlet_geometry
	// 那样「空段也建 16B」——那张表是给 shader 读的**行表**，零行 = 装配层的宿主条件根本没成立，
	// 静默建一张 16B 的表会把「没生效」读成「生效了但没内容」。
	if (sliceRows == 0u || slices == 0u)
		return std::unexpected(Error{ErrorCode::VulkanError,
		    fmt::format("meshlet shadow args: refusing a zero-size row table (sliceRows={} slices={}) "
		                "— VkBufferCreateInfo::size must be > 0 and a carrier with no rows has no "
		                "host condition",
		        sliceRows, slices)});
	// 闸门②：`rowBase` 是 push 里的 32 位行号，越界 = 静默截断 = shader 读错行（不报错、
	// 不触发 VVL，正是本模块要避免的那类失效）⟹ 建之前就拦，而不是上传时才发现。
	const uint64_t totalRows = static_cast<uint64_t>(sliceRows) * slices;
	if (totalRows > UINT32_MAX)
		return std::unexpected(Error{ErrorCode::VulkanError,
		    fmt::format("meshlet shadow args: total rows {} exceeds the 32-bit rowBase "
		                "(sliceRows={} × slices={})",
		        totalRows, sliceRows, slices)});
	const uint64_t capacity = mdiShadowArgsCapacityBytes(sliceRows, slices);

	vk::BufferCreateInfo info{};
	info.size = capacity;
	// usage 四件（口径 → 文件头）：INDIRECT_BUFFER 是 VUID-buffer-02709 的硬要求；
	// STORAGE_BUFFER 让 ★M-1b 的 binding 22 能按描述符读行；SHADER_DEVICE_ADDRESS 供 BDA
	// 描述符；TRANSFER_SRC **只为设备级回读校验**（★1-a0 的教训：统一内存上映射回读是恒真
	// 判据，真设备往返才证明 GPU 看到的是我写的那一行）。
	info.usage = vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eStorageBuffer
	           | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferSrc;
	info.sharingMode = vk::SharingMode::eExclusive;
	// 写者是 CPU（每帧 memcpy）⟹ host-visible + 常驻映射；轮转靠 ring×2 而不是 barrier（D4）。
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
	           | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo allocData{};
	auto created = allocator.createBufferWithAddress(device, info, alloc, &allocData, "meshlet_shadow_args/ring");
	if (!created)
		return std::unexpected(std::move(created).error());
	auto* mapped = static_cast<std::byte*>(allocData.pMappedData);
	if (mapped == nullptr)
		return std::unexpected(Error{ErrorCode::VMASuccessFailed,
		    "meshlet shadow args: mapped pointer is null — MAPPED_BIT honored?"});
	// 整张清 0：VMA 不清内存，未写入区留脏字节 ⟹ 空洞段的 `objectIndex`/`clusterStart`
	// 没有确定读数（gcX=0 的空转行也会被 shader 读走那两个字段）。清 0 之后「没写过的行」
	// 才是可判据的事实（设备级用例把它当反向锚点）。
	std::memset(mapped, 0, static_cast<size_t>(capacity));
	// 先取地址、先 flush，再 move——move 之后 created 内的句柄被清空（防二次销毁），
	// allocatorBlock/allocation/deviceAddress 都读不到了（`uploadSection` 同款顺序）。
	const vk::DeviceAddress address = created->deviceAddress;
	vmaFlushAllocation(created->allocatorBlock->handle, created->allocation, 0, capacity);
	return MeshletShadowArgsRing {
		.buffer = std::move(*created),
		.sliceRows = sliceRows,
		.slices = slices,
		.capacityBytes = capacity,
		.deviceAddress = address,
		.mapped = mapped,
	};
}

MeshletShadowArgsAnchors meshletShadowArgsAnchors(uint32_t sliceRows, uint32_t slices) {
	const uint64_t capacity = mdiShadowArgsCapacityBytes(sliceRows, slices);
	if (sliceRows == 0u || slices == 0u)
		return MeshletShadowArgsAnchors { .rows = sliceRows, .slices = slices,
			.stride = kMdiShadowRowStride, .capacity = capacity };
	// 最坏那一格 = 末个帧槽、段起点 0、一次读完整个 slice。
	const MdiShadowRowLocation last = mdiShadowRowLocation(sliceRows, slices - 1u, 0u);
	return MeshletShadowArgsAnchors {
		.rows = sliceRows,
		.slices = slices,
		.stride = kMdiShadowRowStride,
		.capacity = capacity,
		.tail = mdiShadowTailBytes(last.argsOffset, sliceRows),
	};
}

MeshletShadowArgsService::MeshletShadowArgsService(DeviceContext& device, Resources& resources,
	uint32_t sliceRows, uint32_t slices)
	: device(device), resources(resources) {
	allocArgSlot();
	createRing(sliceRows, slices);
	writeSlotDescriptor();
	// 🔴 闭式锚（§19.5 的 A5 收益判据要有锚）：五个数全部由 `mdi_rows` 的具名公式现算，
	// **不取** ring 成员——两处读数不一致时日志自己就会露出矛盾（被测对象自己报数 = 假绿）。
	const MeshletShadowArgsAnchors anchors = meshletShadowArgsAnchors(sliceRows, slices);
	if (auto* logger = RLLogger::getInstance())
		logger->info(
			"meshlet: ★M-1a 阴影段 MDI 行表载体已装配（**零消费者**，一条 draw 都不接 ⟹ 图像"
			"逐位不变）—— rows/slice={} slices={} stride={}B capacity={}B 末行 offset+12={}B"
			"（VUID-…-drawCount-07089/07090 的边界：{} ≤ {}）；heap 槽 {}（★M-1b 烘 binding 22），"
			"BDA={:#x}",
			anchors.rows, anchors.slices, anchors.stride, anchors.capacity, anchors.tail,
			anchors.tail, anchors.capacity, argHeapSlot, ring.deviceAddress);
	// 🔴 装桩证据（**构造期**打，不在每帧的审计路径里打：它是一条启动期事实，放进被
	// profiler zone 计时的函数里会把一次性的日志 I/O 摊成"每帧 0.027ms"的假成本——实测踩过，
	// 见 `docs/还债实测数据.md` §4.11）。这行同时留下**基准快照的 hex**，将来报不符时可直接
	// 与 ERROR 里的现场 hex 对读。门开着才打（默认关 = 交付日志不多一行）。
	if (diagnostics().heapSlotAudit) {
		if (auto* logger = RLLogger::getInstance())
			logger->info("heap slot audit: 已启用 —— 槽 {}，基准快照 {} 字节 [{}]，期望 BDA={:#x}"
			             "（通过时逐帧不出声，不符才 ERROR；执行账见退出汇总）",
			    argHeapSlot, slotSnapshotBytes.size(), formatSlotBytes(slotSnapshotBytes),
			    ring.deviceAddress);
	}
}

HeapSlotAudit auditHeapSlot(std::span<const std::byte> current,
	std::span<const std::byte> snapshot, uint64_t expectedDeviceAddress, uint32_t slot) {
	HeapSlotAudit a;
	a.slot = slot;
	a.expectedAddress = expectedDeviceAddress;
	a.snapshotSize = static_cast<uint32_t>(snapshot.size());
	// 长度不同 = 本模块自己算错了 descriptorSize（调用方 bug），后面几项没有意义，早退。
	if (current.size() != snapshot.size()) {
		a.sizeMismatch = true;
		a.matchesSnapshot = false;
		a.currentHex = formatSlotBytes(current);  // 🔴 冷路径才拼 hex（见函数尾的说明）
		return a;
	}
	// 逐字节对账（手写循环而不是 std::ranges::equal：本文件是模块 impl unit，ranges 算法要
	// 真 STL 头才可见——modules 坑，见 tests/ 里同款注释）。
	bool same = true;
	bool zero = true;
	for (size_t i = 0; i < current.size(); ++i) {
		if (current[i] != snapshot[i]) {
			if (!a.hasDiff) {
				a.hasDiff = true;
				a.firstDiffByte = static_cast<uint32_t>(i);
			}
			same = false;
		}
		if (current[i] != std::byte{0})
			zero = false;
	}
	a.matchesSnapshot = same;
	a.allZero = zero;
	// 🔴 下面两样**只在判负时**才算（实测教训：本函数每帧都跑，无条件做 → 审计自身成本
	// 0.027 ms/帧，比它要保护的对账贵三个数量级；"读一次 host 映射应该几乎免费"的直觉
	// 是错的，实测说话 → `docs/还债实测数据.md` §4.11）。
	//  · hex：32 次 fmt::format；
	//  · 地址扫描：2 形态 × 最多 25 个偏移 × 8 字节的**分支密集**比较（预测失败的代价）。
	// 两者都只在解释一次失败时才用得上——通过时现场由构造期那行基准快照承担。
	if (!a.ok()) {
		// 附带信息（不判红）：期望 BDA 以 8B 小端出现在槽内**任意**字节偏移。
		// 🔴 不参与 ok()：描述符布局是驱动私有的，某些布局不放裸地址（放的是 buffer table
		// 索引），那时本项恒 false 而槽完全正确——拿它判红就是造一条"换驱动就红"的假判据。
		// 实测本机 RADV：槽前 8 字节 = `30 23 53 03 01 80 00 00`，而 BDA = `0xffff800103532330`
		// ⟹ **高 16 位被驱动掩掉了**（AMD GPU VA 是 48 位规范形式）。所以除了原值，还要拿
		// 掩掉高 16 位的形态去比，否则本项在唯一实测过的驱动上恒 false = 一条永远不亮的灯。
		a.containsExpectedAddress = containsAddressLE(current, expectedDeviceAddress)
		    || containsAddressLE(current, expectedDeviceAddress & 0x0000'FFFF'FFFF'FFFFull);
		a.currentHex = formatSlotBytes(current);
	}
	return a;
}

std::string describeHeapSlotAudit(const HeapSlotAudit& audit) {
	const std::string head = fmt::format(
		"heap 槽 {} 内容对账：期望 BDA={:#x}（槽内{}找到该地址）当前 [{}]", audit.slot,
		audit.expectedAddress, audit.containsExpectedAddress ? "" : "未", audit.currentHex);
	if (audit.sizeMismatch)
		return head + " —— 🔴 快照与当前读数**长度不同** = 本模块自己算错了 descriptorSize"
		              "（调用方 bug，不是驱动改写）";
	// 🔴 判序：全零必须判在"与快照相同"**之前**。"快照全零 + 当前也全零"不是稳定，而是构造期
	// 那次描述符写入没生效（我们写的是真实 BDA，槽里不可能全零）——把它洗成"通过"就放过了
	// fault@0 最直接的成因。
	if (audit.allZero)
		return head + " —— 🔴🔴 槽内容**全零** = fault@0 的直接对应形态（task 经 binding 34 读"
		              "行表时基址为 0，Load(0) 落在 VA 0）。"
		              + (audit.matchesSnapshot
		                     ? std::string("且**构造期快照本身就是零** ⟹ 那次 writeResource"
		                                   "DescriptorsEXT 没生效，不是后来被改写的")
		                     : std::string("下一步 = 查谁把这个槽清零 / 描述符写入是否根本没生效"));
	if (audit.matchesSnapshot)
		return head + " —— 与启动快照逐字节相同";
	// 非零但与快照不同 = 第三种现场：有人改写了这个槽。
	return head + fmt::format(
	    " —— 🔴 与启动快照不符但非零 = **有人改写了这个槽**（首个差异在第 {} 字节；槽号被复用？"
	    "别的 writeResourceDescriptorsEXT 越界写进来？）。下一步 = 把本帧前后的行表内容与"
	    "录制参数（argsOffset/rowCount）一起 dump",
	    audit.firstDiffByte);
}

BakedMappingAudit auditBakedRowMapping(std::span<const BakedBindingMapping> baked,
	uint32_t binding, uint32_t expectedSlot, uint32_t descriptorSize) {
	BakedMappingAudit a;
	a.binding = binding;
	a.expectedSlot = expectedSlot;
	a.descriptorSize = descriptorSize;
	a.sizeUsable = descriptorSize > 0u;
	a.expectedHeapOffset =
	    a.sizeUsable ? static_cast<uint64_t>(expectedSlot) * descriptorSize : 0ull;
	// 按 binding 号查（烘入顺序无含义：批 M 的声明过滤会改变条数与顺序）。
	const BakedBindingMapping* found = nullptr;
	for (const auto& m : baked)
		if (m.binding == binding) {
			found = &m;
			break;
		}
	a.bindingFound = found != nullptr;
	if (!a.bindingFound || !a.sizeUsable)
		return a;  // 无从换算，后面三项保持 false（措辞由这两条优先负责）
	a.bakedHeapOffset = found->heapOffset;
	a.strideMatches = found->heapArrayStride == descriptorSize;
	// 🔴 换算式 = `shaderDetail::slotBytes` 的逆运算，且**先验整除**（见接口注释）。
	a.offsetSlotAligned = found->heapOffset % descriptorSize == 0ull;
	if (!a.offsetSlotAligned)
		return a;
	a.bakedSlot = static_cast<uint32_t>(found->heapOffset / descriptorSize);
	a.slotMatches = a.bakedSlot == expectedSlot;
	return a;
}

std::string describeBakedRowMappingAudit(const BakedMappingAudit& audit) {
	const std::string head = fmt::format(
	    "binding {} 烘入 mapping 对账：烘入 heapOffset={:#x}（步长 {}）期望槽号 {} → 堆偏移 {:#x}"
	    "（descriptorSize={}）",
	    audit.binding, audit.bakedHeapOffset, audit.descriptorSize, audit.expectedSlot,
	    audit.expectedHeapOffset, audit.descriptorSize);
	// 🔴 判序 = 责任归属的判序，五种现场各指一个不同的下一步，措辞不许互相包含。
	if (!audit.bindingFound)
		return head + " —— 🔴🔴 条目**根本没烘 binding "
		    + std::to_string(audit.binding)
		    + "**（SPV 声明了它却没有 mapping，或两侧同向约束被破）。GPU 读未映射的 binding ⟹ "
		      "基址来源未定义，这正是 fault@0 的一条正面来路。下一步 = 查批 M 的声明过滤"
		      "（declaredBindingsUnion）与 task SPV，**不是**查行表内容";
	if (!audit.sizeUsable)
		return head + " —— 🔴 descriptorSize = 0，槽↔字节换算式无从执行 = 本模块或描述符堆"
		      "自己算错了步长（调用方 bug，与驱动无关）";
	if (!audit.strideMatches)
		return head + " —— 🔴 烘入的 heapArrayStride 与 descriptorSize 不等 = 这条 mapping 描述的"
		      "**不是一枚资源堆描述符**（sampler 堆偏移误接到资源 binding 上？单位错，不是接线错）";
	if (!audit.offsetSlotAligned)
		return head + " —— 🔴 烘入的 heapOffset 不是 descriptorSize 的整数倍 = 换算式被喂了"
		      "非槽起点的字节偏移（例如按元素而不是按槽算）。下一步 = 查 slotBytes 的调用点";
	if (!audit.slotMatches)
		return head + " —— 🔴🔴 **两条路指向不同槽**：烘入槽号 "
		    + std::to_string(audit.bakedSlot) + " ≠ 服务写入的槽号 "
		    + std::to_string(audit.expectedSlot) + "。服务把行表 BDA 写进后者，GPU 从前者读 —— "
		      "那里可能是 0，也可能是别的 buffer 的描述符。这就是 fault@0 的直接成因形态，"
		      "且 heap_slot_audit **看不见**（它审的是服务那枚）。下一步 = 查 "
		      "addShadowMeshletMdiEntry 的 argsSlot 实参传递链";
	return head + " —— 烘入槽号 == 服务写入槽号（两条路同源，CPU 侧无分家）";
}

void logBakedRowMappingAudit(std::span<const BakedBindingMapping> baked,
	std::string_view entryName, uint32_t binding, uint32_t expectedSlot,
	uint32_t descriptorSize) {
	const auto audit = auditBakedRowMapping(baked, binding, expectedSlot, descriptorSize);
	auto* logger = RLLogger::getInstance();
	if (logger) {
		// 🔴 判负**不经门**（理由见接口注释：判据两侧来自同一条代码路径，与驱动无关，
		// 不存在假阳性，所以没有"可以关掉"的余地）。门只管通过时那行正读数。
		if (!audit.ok())
			logger->error("baked mapping audit[{}]: {}", entryName,
			    describeBakedRowMappingAudit(audit));
		else if (diagnostics().heapSlotAudit)
			logger->info("baked mapping audit[{}]: {} —— 条目数 {}（正读数也要有出处，否则"
			             "「零 ERROR」分不清是「对过且相符」还是「根本没对」）",
			    entryName, describeBakedRowMappingAudit(audit), baked.size());
	}
}

void MeshletShadowArgsService::allocArgSlot() {
	auto slot = resources.allocSlot(SlotSegment::Runtime, "meshlet.gpu.shadowArgs");
	if (!slot)
		throw std::runtime_error("meshlet shadow args: 领槽失败 —— " + slot.error().describe());
	argHeapSlot = *slot;
}

void MeshletShadowArgsService::createRing(uint32_t sliceRows, uint32_t slices) {
	auto created = createMeshletShadowArgsRing(resources.getAllocator(), device.getDevice(),
		sliceRows, slices);
	if (!created)
		throw std::runtime_error("meshlet shadow args: 行表 buffer 创建失败 —— "
		                         + created.error().describe());
	ring = std::move(*created);
}

void MeshletShadowArgsService::writeSlotDescriptor() {
	// 步长取**一次**存成员：本函数（写描述符 + 拍快照）与 Ⓐ 的换算式共用同一个数，
	// 两处各查一次描述符堆 = 两个真相源（虽然值相同，但"相同"这件事没人守）。
	descriptorSize = resources.getDescriptorHeap().getDescriptorSize();
	// range = **整张容量**（覆盖所有 slice）⟹ 逐帧轮转只改 offset，描述符与槽号终身不动
	// （§19.5 的免第二槽解法）。写法 = `MeshletCullService::writeSlotDescriptor` 同款：
	// `writeResourceDescriptorsEXT` + `DeviceAddressRangeEXT`，host 直写不进命令流。
	vk::DeviceAddressRangeEXT range { ring.deviceAddress, ring.capacityBytes };
	vk::ResourceDescriptorInfoEXT desc {};
	desc.type = vk::DescriptorType::eStorageBuffer;
	desc.data.pAddressRange = &range;  // 同步 host 写，局部临时量存活到调用返回即够
	auto slotAddr = resources.getDescriptorHeap().getSlotCpuAddress(argHeapSlot);
	if (!slotAddr)
		throw std::runtime_error("meshlet shadow args: 槽地址解析失败 —— "
		                         + slotAddr.error().describe());
	device.getDevice().writeResourceDescriptorsEXT(desc,
	    vk::HostAddressRangeEXT { *slotAddr, descriptorSize });

	// 🔴 债-140 第三击 ①：写完**立刻**从 host 映射读回槽内容当快照（审计的判据基准）。
	// 恒拍，不看诊断门——成本是构造期一次 ≤64B 拷贝，换来"审计随时可开而基准与门无关"
	// （门控快照 = 门开的那一帧才开始有基准，之前几帧的改写它看不见）。
	// ⚠ 这里读的是**自己刚写的那块内存**，不是设备状态；host 可见映射上这是合法读。
	const auto* base = static_cast<const std::byte*>(*slotAddr);
	slotSnapshotBytes.assign(base, base + descriptorSize);
}

std::optional<HeapSlotAudit> MeshletShadowArgsService::auditSlot() const {
	// 快照为空 = 构造未完成或 descriptorSize 读不到。返回 nullopt 让调用方**出声**，
	// 不在这里编一个"通过"的结论（"审计没跑"被读成"审计说没问题"是假绿的标准形态）。
	if (slotSnapshotBytes.empty())
		return std::nullopt;
	const auto slotAddr = resources.getDescriptorHeap().getSlotCpuAddress(argHeapSlot);
	if (!slotAddr)
		return std::nullopt;
	const std::span<const std::byte> current {
		static_cast<const std::byte*>(*slotAddr), slotSnapshotBytes.size() };
	return auditHeapSlot(current, slotSnapshotBytes, ring.deviceAddress, argHeapSlot);
}

void MeshletShadowArgsService::registerEntrySlots(std::span<const BakedBindingMapping> baked) {
	entrySlots.clear();
	if (descriptorSize == 0u)
		return;  // 无从换算（Ⓐ 同款护栏：步长为 0 时不做除法）
	for (const auto& m : baked) {
		if (m.heapOffset % descriptorSize != 0ull)
			continue;  // 非槽起点的偏移（sampler 堆之类），不参与本审计
		entrySlots.emplace_back(m.binding,
			static_cast<uint32_t>(m.heapOffset / descriptorSize));
	}
}

std::string MeshletShadowArgsService::describeEntrySlots() const {
	std::string out;
	for (const auto& [binding, slot] : entrySlots) {
		const auto addr = resources.getDescriptorHeap().getSlotCpuAddress(slot);
		if (!addr) {
			out += fmt::format("\n  binding {} 槽 {} —— 🔴 槽地址解析失败", binding, slot);
			continue;
		}
		const std::span<const std::byte> bytes {
			static_cast<const std::byte*>(*addr), descriptorSize };
		out += fmt::format("\n  binding {:>2} 槽 {:>5} [{}]{}", binding, slot, formatSlotBytes(bytes),
		    bytesAllZero(bytes) ? "  🔴🔴 **零描述符 = 基址 0**" : "");
	}
	return out.empty() ? "（未登记任何条目槽）" : out;
}

void MeshletShadowArgsService::auditEntrySlotsOnce() const {
	// 每个 binding 只报一次：全零描述符若是常态（例如某条 binding 本引擎根本不写），每帧刷
	// 会把日志冲干净、反而盖掉真正的现场；若是竞态，第一次出现就是最有价值的一次。
	static std::vector<uint32_t> reported{};
	for (const auto& [binding, slot] : entrySlots) {
		if (alreadyReported(reported, binding))
			continue;
		const auto addr = resources.getDescriptorHeap().getSlotCpuAddress(slot);
		if (!addr)
			continue;  // 解析失败**不算**"零描述符"——两件事不许混进同一条读数
		if (!bytesAllZero({static_cast<const std::byte*>(*addr), descriptorSize}))
			continue;
		reported.push_back(binding);
		if (auto* logger = RLLogger::getInstance())
			logger->error("heap slot audit: 条目 binding {} 的堆槽 {} 内容**全零** = 零描述符 = "
			              "基址 0（fault@0 的直接来源）—— 该槽从未被写过或被清零了。全文:\n{}",
			    binding, slot, describeEntrySlots());
	}
}

void MeshletShadowArgsService::auditAndLog(uint32_t frameSlot, uint32_t segments) const {
	// 计数走函数内 static（本函数只在主线程的录制路径上跑，无并发）。
	// 🔴 "审计跑了多少帧"必须有出处：审计通过时不逐帧出声（否则每帧一行"没问题"冲掉日志），
	// 于是"零 ERROR"分不清是"零调用"还是"全部通过"——两者对归因的含义相反。上批刚在
	// GPU-AV 上踩过同一个坑。装桩证据 = 构造期那行基准快照，执行账 = 退出时那一行。
	auto& stats = auditStats();
	++stats.frames;
	const auto audit = auditSlot();
	if (!audit) {
		++stats.noBaseline;
		if (auto* logger = RLLogger::getInstance())
			logger->warn("heap slot audit: 审计无法执行（构造期快照为空或槽地址解析失败）——"
			             "本帧槽内容**无读数**，不可读成『对账通过』（帧槽 {}）",
			    frameSlot);
		return;
	}
	if (audit->ok())
		return;
	++stats.mismatch;
	if (auto* logger = RLLogger::getInstance())
		logger->error("heap slot audit: {}（帧槽 {}，本帧 MDI 段 {} 条）",
		    describeHeapSlotAudit(*audit), frameSlot, segments);
}

void reportHeapSlotAuditSummary() {
	auto& stats = auditStats();
	// 🔴 **只出声一次**，且必须在 device-lost 之路上也出：本汇总是"审计跑了多少帧"的唯一
	// 出处，而它原先只挂在 main 的退出收口里——那条路在 device lost 时**执行不到**（收尾的
	// waitIdle 抛 vk::SystemError，异常展开跳过 exitDiagnostics）。后果是挂的那几轮报
	// "审了 0 帧"，于是"审了 2005 帧零不符"这个数**系统性地排除了正在调查的那些轮**。
	// 现在两个点都调它 + 打出去重 = 正常退出与崩溃退出各出一次，不多不少。
	// 去重不再由本函数负责：注册表（`ShutdownLedger`）持有 printed，两条路各调一次
	// `shutdownLedger().run()`，谁先到谁结算（债-140 第四击 ③）。
	if (stats.frames == 0u)
		return;
	if (auto* logger = RLLogger::getInstance())
		logger->info("heap slot audit 执行账: 审了 {} 帧，不符 {} 次，无基准 {} 次", stats.frames,
		    stats.mismatch, stats.noBaseline);
}

MdiShadowRowLocation MeshletShadowArgsService::location(uint32_t frameSlot, uint32_t firstRow) const {
	return ring.location(frameSlot, firstRow);
}

uint64_t MeshletShadowArgsService::argsBytes(uint32_t frameSlot, uint32_t firstRow,
	uint32_t drawCount) const {
	return ring.argsBytes(frameSlot, firstRow, drawCount);
}

std::optional<std::string> MeshletShadowArgsService::upload(uint32_t frameSlot, uint32_t firstRow,
	std::span<const MdiShadowRow> rows) const {
	return ring.upload(frameSlot, firstRow, rows);
}

}  // namespace RGL
