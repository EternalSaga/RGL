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
	    vk::HostAddressRangeEXT { *slotAddr, resources.getDescriptorHeap().getDescriptorSize() });
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
