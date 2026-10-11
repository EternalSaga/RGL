module;
#include <vk_mem_alloc.h>  // VMA C API（VmaRaii 只导出 C++ 包装，usage/flags 宏在这里）
#include <fmt/format.h>
module meshlet_geometry:impl;

import meshlet_geometry;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import VmaRaii;
import vkerror;
import meshlet_asset;
import rllogger;

namespace RGL {
namespace {

/** 向上取整到 `align` 的整数倍（`align` 非 0；本文件唯一调用点传常量 16）。 */
vk::DeviceSize alignUp(vk::DeviceSize value, vk::DeviceSize align) {
	return (value + align - 1) / align * align;
}

/**
 * 建一段 buffer 并把 CPU 侧字节**直写进映射指针**（无 staging，理由见接口文件头）。
 *
 * 三步各有一个非显然点：
 *  1. **usage 三件**：`eStorageBuffer`（shader 读）+ `eShaderDeviceAddress`（与先例同形，
 *     ★1-a1 若要 buffer 寻址免重建）+ `eTransferSrc`（**只为设备级回读校验**：映射指针回读
 *     在统一内存的机器上是恒真判据，`vkCmdCopyBuffer` 的真设备往返才抓得到"字节挪位"。
 *     先例 `uploadChunk` 缺这个 flag，它的 canary 因此只能在 VVL 之外跑——本批不再复制这个缺陷）。
 *  2. **补齐区清 0**：VMA 不清零内存，尾巴留着未初始化字节，"整块 buffer 逐字节回读"就没有
 *     确定期望值。只清 `[bytes, allocBytes)`（≤ 15B），不整块 memset（顶点池 138MB）。
 *  3. **`vmaFlushAllocation`**：映射内存**不保证** host-coherent（独显无 large-BAR 时
 *     CPU_TO_GPU 会落到非 coherent 的系统内存）。先例没刷——它在三台机器上恰好都是 coherent
 *     类型所以没症状。coherent 内存上这次调用是 VMA 内部的提前返回，代价为零。
 *
 * @param name 段名（只进错误串，失效形态可直接归因）
 */
std::expected<MeshletGeometrySection, Error> uploadSection(const VmaAllocatorRAII& allocator,
                                                            const vk::raii::Device& device,
                                                            std::string_view name, const void* src,
                                                            vk::DeviceSize bytes) {
	// 空段也要一个 buffer（VkBufferCreateInfo::size 必须 > 0）：大小取对齐粒度，数据字节数记 0。
	const vk::DeviceSize allocBytes =
	    std::max(alignUp(bytes, kMeshletBufferAlign), kMeshletBufferAlign);
	vk::BufferCreateInfo bufferInfo{};
	bufferInfo.size = allocBytes;
	bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
	    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferSrc;
	bufferInfo.sharingMode = vk::SharingMode::eExclusive;
	// 与 uploadChunk 逐字段同形：CPU_TO_GPU + 常驻映射 + 顺序写（= 映射直写，无 staging，峰值 1×）。
	VmaAllocationCreateInfo allocInfo{};
	allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
	    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo allocData;
	auto created = allocator.createBufferWithAddress(device, bufferInfo, allocInfo, &allocData);
	if (!created)
		return std::unexpected(std::move(created).error());
	auto* dst = static_cast<std::byte*>(allocData.pMappedData);
	if (dst == nullptr)
		return std::unexpected(Error{ErrorCode::VMASuccessFailed,
		    fmt::format("meshlet geometry: {} section mapped pointer is null — MAPPED_BIT honored?",
		        name)});
	if (allocBytes > bytes)
		std::memset(dst + bytes, 0, static_cast<size_t>(allocBytes - bytes));
	if (bytes > 0)
		std::memcpy(dst, src, static_cast<size_t>(bytes));
	// 先取地址、先 flush，再 move——move 之后 created 内的句柄被清空（防二次销毁），
	// 届时 allocatorBlock/allocation/deviceAddress 都读不到了。
	const vk::DeviceAddress deviceAddress = created->deviceAddress;
	vmaFlushAllocation(created->allocatorBlock->handle, created->allocation, 0, allocBytes);
	return MeshletGeometrySection{
	    .buffer = std::move(*created),
	    .bytes = bytes,
	    .allocBytes = allocBytes,
	    .deviceAddress = deviceAddress,
	};
}

}  // namespace

std::expected<MeshletGeometry, Error> uploadMeshletGeometry(const VmaAllocatorRAII& allocator,
                                                            const vk::raii::Device& device,
                                                            const MeshletAsset& asset,
                                                            std::string_view sourceLabel) {
	// 四段的字节数**全部来自视图**（size_bytes 不硬编码 stride；Tris 视图含段间填充、
	// Remap 视图按元素数取整不含填充——两者都是"视图给多少传多少"，本层不重新推导布局）。
	// 顺序固定 = 产物落盘子集；四段各自独立失败（第一个失败即返回，已建 buffer 由 RAII 回收）。
	struct SourceSpan {
		std::string_view name;
		const void* src;
		vk::DeviceSize bytes;
	};
	const SourceSpan sources[4] = {
	    {"VertexPool", asset.vertexPool.data(),
	        static_cast<vk::DeviceSize>(asset.vertexPool.size_bytes())},
	    {"Clusters", asset.clusters.data(),
	        static_cast<vk::DeviceSize>(asset.clusters.size_bytes())},
	    {"Tris", asset.tris.data(), static_cast<vk::DeviceSize>(asset.tris.size_bytes())},
	    {"Remap", asset.remap.data(), static_cast<vk::DeviceSize>(asset.remap.size_bytes())},
	};

	MeshletGeometry out;
	out.clusterCount = asset.header.clusterCount;
	out.vertexCount = asset.header.vertexCount;
	// 成员指针表：与 sources 同序，避免"四份几乎相同的上传块"（改一处忘三处的经典形态）。
	MeshletGeometrySection* targets[4] = {&out.vertexPool, &out.clusters, &out.tris, &out.remap};
	for (size_t i = 0; i < 4; ++i) {
		auto section = uploadSection(allocator, device, sources[i].name, sources[i].src,
		    sources[i].bytes);
		if (!section)
			return std::unexpected(std::move(section).error());
		*targets[i] = std::move(*section);
	}

	if (auto* logger = RLLogger::getInstance())
		logger->info(
		    "meshlet: {} → GPU 4 buffer（★1-a0 只上传未消费，图像不变）: clusters={} vertices={} "
		    "| vertexPool={}B clusters={}B tris={}B remap={}B",
		    sourceLabel, out.clusterCount, out.vertexCount, out.vertexPool.allocBytes,
		    out.clusters.allocBytes, out.tris.allocBytes, out.remap.allocBytes);
	return out;
}

}  // namespace RGL
