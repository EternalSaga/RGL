module;
#include <vulkan/vulkan.hpp>
#include <vk_mem_alloc.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
export module model_importer;

import rlstd;
import rllogger;
import vkerror;
import VmaRaii;
import vk_device;
import vk_resources;
import scene;
import gltf_loader;
import model_types_generated;

namespace RGL {

// GPU 上传：每 primitive 建一个 BDA storage buffer（CPU→GPU mapped 写入顶点数据），
// 描述符写进 heap 槽位（vkWriteResourceDescriptorsEXT，同 cube 的写法），并把 buffer +
// slot + 三角形数注册进 Resources 资产表（一条 MeshAsset 记录，E1.1）。
// 资产数据生命周期归资产表（Resources 成员），不再经返回值持有（E1.2/L6）。
// 返回资产 id 列表（与 model.primitives 下标对齐：上传顺序 = primitive 顺序）。
export [[nodiscard]] std::expected<std::vector<uint32_t>, Error> uploadModelMeshes(
	RGL::DeviceContext& vkDevice, RGL::Resources& res, const RGL::GltfModel& model) {
	auto* logger = RLLogger::getInstance();
	auto& device = vkDevice.getDevice();
	auto& heap = res.getDescriptorHeap();
	auto& assets = res.getAssetTable();

	std::vector<uint32_t> assetIds;
	const uint32_t primitiveCount = static_cast<uint32_t>(model.primitives.size());
	assetIds.reserve(primitiveCount);

	for (uint32_t i = 0; i < primitiveCount; ++i) {
		const auto& prim = model.primitives[i];
		const auto& vertices = prim.vertices;
		const vk::DeviceSize size = static_cast<vk::DeviceSize>(vertices.size() * sizeof(ModelVertex));

		vk::BufferCreateInfo bufferInfo{};
		bufferInfo.size = size;
		bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
			vk::BufferUsageFlagBits::eStorageBuffer;
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
			VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		VmaAllocationInfo allocData;
		auto bufResult = res.getAllocator().createBufferWithAddress(device, bufferInfo, allocInfo, &allocData);
		if (!bufResult)
			return std::unexpected(std::move(bufResult).error());
		auto buffer = std::move(*bufResult);

		std::memcpy(allocData.pMappedData, vertices.data(), size);

		// 描述符：storage buffer → heap 槽位（统一 slot 分配器，Phase 3：res.allocSlot 连续分配；
		// 布局约定 = reserved + HeapSlotModelVertexBase + i（schema 生成的基址常量），违例 = 契约漂移）
		const uint32_t expectedSlot = heap.getReservedSlots() + HeapSlotModelVertexBase + i;
		auto slotResult = res.allocSlot("model.vertex");
		if (!slotResult)
			return std::unexpected(std::move(slotResult).error());
		const uint32_t slot = *slotResult;
		if (slot != expectedSlot)
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				"heap slot layout drift: model.vertex[" + std::to_string(i) + "]"});
		auto slotAddr = heap.getSlotCpuAddress(slot);
		if (!slotAddr)
			return std::unexpected(std::move(slotAddr).error());

		vk::DeviceAddressRangeEXT range{buffer.getDeviceAddress(), size};
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eStorageBuffer;
		desc.data.pAddressRange = &range;
		vk::HostAddressRangeEXT dst{*slotAddr, heap.getDescriptorSize()};
		device.writeResourceDescriptorsEXT(desc, dst);

		const uint32_t id = assets.addMesh(std::move(buffer), slot, prim.triangleCount);
		if (logger)
			logger->debug("model primitive {}: {} vertices ({}B) → heap slot {} / asset {}",
				i, vertices.size(), size, slot, id);
		assetIds.push_back(id);
	}
	return assetIds;
}

// 模型 → ECS：场景根（Relationship parent = null；Bounds 组件 = 模型整体包围球）
// + 每 primitive 一个 entity（Transform/MeshRef{asset}/MaterialRef，Relationship parent 挂根）。
// assetIds 与 model.primitives 下标对齐（uploadModelMeshes 返回，上传顺序 = primitive 顺序）。
// Phase 2 无层级嵌套：node 变换在 gltf_loader 里已扁平化为 translation，world = local。
// 包围球经 Bounds 组件进 registry，由 boundsSystem 聚合进 SceneBounds（自适应 near/far，
// 见 ecs.cppm）——cube 将来入 ECS 时挂自己的 Bounds 即自动纳入聚合。
export entt::entity registerScene(entt::registry& rg, const RGL::GltfModel& model,
	std::span<const uint32_t> assetIds) {
	auto sceneRoot = rg.create();
	rg.emplace<Relationship>(sceneRoot);  // parent = null（根）；children 不存储（单向，E5.2）
	rg.emplace<Bounds>(sceneRoot, model.bounds.center, model.bounds.radius);

	for (size_t i = 0; i < model.primitives.size(); ++i) {
		const auto& prim = model.primitives[i];
		auto e = rg.create();

		glm::mat4 local = glm::translate(glm::mat4(1.0f), prim.translation);
		auto& tf = rg.emplace<Transform>(e);
		tf.local = local;
		tf.world = local;

		rg.emplace<MeshRef>(e).asset = assetIds[i];
		rg.emplace<MaterialRef>(e).baseColor = prim.baseColor;
		rg.emplace<Relationship>(e).parent = sceneRoot;
	}
	return sceneRoot;
}

}  // namespace RGL
