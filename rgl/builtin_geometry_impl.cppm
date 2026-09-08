module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <fmt/format.h>
module builtin_geometry:impl;

import builtin_geometry;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import VmaRaii;
import vk_device;
import vk_resources;
import asset_table;
import scene_context;
import cube;  // 几何数据源（内容层 → 内容数据，方向正）

namespace RGL {

// uploadBuiltinCube / registerBuiltinCube 定义（架构债务 1 方案 B）：GPU buffer 创建 +
// 描述符写入 + 资产表登记移出接口 TU；接口只留声明，本单元是叶子边。

namespace {

// 内建几何 = 与模型同规格的 BDA storage buffer（CPU→GPU mapped，创建期一次写满）
std::expected<RGL::VmaBuffer, Error> makeGeometryBuffer(RGL::Resources& res,
	vk::raii::Device& device, const void* data, vk::DeviceSize bytes, const char* name) {
	vk::BufferCreateInfo info{};
	info.size = bytes;
	info.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
		vk::BufferUsageFlagBits::eStorageBuffer;
	info.sharingMode = vk::SharingMode::eExclusive;
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
		VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo allocData;
	auto result = res.getAllocator().createBufferWithAddress(device, info, alloc, &allocData);
	if (!result)
		return std::unexpected(std::move(result).error());
	std::memcpy(allocData.pMappedData, data, bytes);
	if (auto* logger = RLLogger::getInstance())
		logger->debug("builtin geometry {}: {}B", name, bytes);
	return std::move(*result);
}

// 一个几何 heap 槽 + storage buffer 描述符写入（期望值 = reserved + 已占几何槽数，
// 违例 = 几何段中间插了别的分配 = 契约漂移；计数的意义在下游"纹理首槽 = 白 =
// 全部几何槽之后"断言，见 ensureWhiteTexture）
std::expected<uint32_t, Error> allocGeometrySlot(RGL::Resources& res, vk::raii::Device& device,
	const RGL::VmaBuffer& buffer, vk::DeviceSize bytes, const char* name) {
	auto& assets = res.getAssetTable();
	auto& heap = res.getDescriptorHeap();
	const uint32_t expectedSlot = assets.expectedGeometrySlot(heap.getReservedSlots());
	auto slotResult = res.allocSlot(name);
	if (!slotResult)
		return std::unexpected(std::move(slotResult).error());
	if (*slotResult != expectedSlot)
		return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
			fmt::format("heap slot layout drift: {} (slot {} != expected {})",
				name, *slotResult, expectedSlot)});
	assets.incrementGeometrySlotCount();
	auto slotAddr = heap.getSlotCpuAddress(*slotResult);
	if (!slotAddr)
		return std::unexpected(std::move(slotAddr).error());
	const vk::DeviceAddressRangeEXT range{buffer.getDeviceAddress(), bytes};
	vk::ResourceDescriptorInfoEXT desc{};
	desc.type = vk::DescriptorType::eStorageBuffer;
	desc.data.pAddressRange = &range;
	const vk::HostAddressRangeEXT dst{*slotAddr, heap.getDescriptorSize()};
	device.writeResourceDescriptorsEXT(desc, dst);
	return *slotResult;
}

}  // namespace

std::expected<uint32_t, Error> uploadBuiltinCube(
	RGL::DeviceContext& vkDevice, RGL::Resources& res) {
	auto& device = vkDevice.getDevice();
	auto& assets = res.getAssetTable();
	auto* logger = RLLogger::getInstance();

	const vk::DeviceSize vertexBytes =
		static_cast<vk::DeviceSize>(cubeVertices.size() * sizeof(RGL::Vertex));
	const vk::DeviceSize indexBytes =
		static_cast<vk::DeviceSize>(cubeIndices.size() * sizeof(uint32_t));

	auto vertexResult = makeGeometryBuffer(res, device, cubeVertices.data(), vertexBytes, "cube.vertices");
	if (!vertexResult)
		return std::unexpected(std::move(vertexResult).error());
	auto indexResult = makeGeometryBuffer(res, device, cubeIndices.data(), indexBytes, "cube.indices");
	if (!indexResult)
		return std::unexpected(std::move(indexResult).error());

	auto vertexSlot = allocGeometrySlot(res, device, *vertexResult, vertexBytes, "cube.vertices");
	if (!vertexSlot)
		return std::unexpected(std::move(vertexSlot).error());
	auto indexSlot = allocGeometrySlot(res, device, *indexResult, indexBytes, "cube.indices");
	if (!indexSlot)
		return std::unexpected(std::move(indexSlot).error());

	const uint32_t assetId = assets.addIndexedMesh(std::move(*vertexResult), *vertexSlot,
		std::move(*indexResult), *indexSlot,
		static_cast<uint32_t>(cubeIndices.size() / 3));
	if (logger)
		logger->info("builtin cube: 资产 {} / 顶点槽 {} 索引槽 {} / {} 三角形",
			assetId, *vertexSlot, *indexSlot, cubeIndices.size() / 3);
	return assetId;
}

entt::entity registerBuiltinCube(entt::registry& rg, uint32_t meshAsset,
	uint32_t materialAsset, const glm::mat4& rootTransform) {
	auto e = rg.create();
	auto& tf = rg.emplace<Transform>(e);
	tf.local = rootTransform;
	tf.world = rootTransform;  // cube 几何已在原点 ±0.5
	glm::vec3 center{0.0f};
	float radius = 0.0f;
	for (const auto& v : cubeVertices)
		radius = std::max(radius, glm::length(glm::vec3(v.position[0], v.position[1], v.position[2])));
	rg.emplace<Bounds>(e, glm::vec3(rootTransform * glm::vec4(center, 1.0f)),
		radius * glm::length(glm::vec3(rootTransform[0])));
	auto& meshRef = rg.emplace<MeshRef>(e);
	meshRef.asset = meshAsset;
	meshRef.objectSlot = rg.ctx().get<NextObjectSlot>().next++;
	rg.emplace<MaterialRef>(e).material = materialAsset;
	rg.emplace<CullSphere>(e, center, radius);  // Phase 7 批次 4：node-local 剔除球（原点 ±0.5）
	rg.emplace<Relationship>(e);  // parent = null（根）
	return e;
}

}  // namespace RGL
