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

// 每个 primitive 一个顶点 buffer（RAII：随 SharedAllocator 控制块由 VMA 统一销毁）。
// 描述符已写入 descriptor heap 对应槽位：slot = reservedSlots() + HeapSlotModelVertexBase + i。
export struct ModelMeshAssets {
	std::vector<VmaBuffer> vertexBuffers;
	uint32_t primitiveCount = 0;
};

// GPU 上传：每 primitive 建一个 BDA storage buffer（CPU→GPU mapped 写入顶点数据），
// 并把描述符写进 heap 槽位（vkWriteResourceDescriptorsEXT，同 cube 的写法）。
export [[nodiscard]] std::expected<ModelMeshAssets, Error> uploadModelMeshes(
	RGL::DeviceContext& vkDevice, RGL::Resources& res, const RGL::GltfModel& model) {
	auto* logger = RLLogger::getInstance();
	auto& device = vkDevice.getDevice();
	auto& heap = res.getDescriptorHeap();

	ModelMeshAssets assets;
	assets.vertexBuffers.reserve(model.primitives.size());
	assets.primitiveCount = static_cast<uint32_t>(model.primitives.size());

	for (uint32_t i = 0; i < assets.primitiveCount; ++i) {
		const auto& vertices = model.primitives[i].vertices;
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

		// 描述符：storage buffer → heap 槽位 i（基址 HeapSlotModelVertexBase，
		// 避开 cube 占用的 0/4；槽位容量在 Resources 构造期按 primitive 数扩过）
		const uint32_t slot = heap.getReservedSlots() + HeapSlotModelVertexBase + i;
		auto slotAddr = heap.getSlotCpuAddress(slot);
		if (!slotAddr)
			return std::unexpected(std::move(slotAddr).error());

		vk::DeviceAddressRangeEXT range{buffer.getDeviceAddress(), size};
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eStorageBuffer;
		desc.data.pAddressRange = &range;
		vk::HostAddressRangeEXT dst{*slotAddr, heap.getDescriptorSize()};
		device.writeResourceDescriptorsEXT(desc, dst);

		if (logger)
			logger->debug("model primitive {}: {} vertices ({}B) → heap slot {}",
				i, vertices.size(), size, slot);
		assets.vertexBuffers.push_back(std::move(buffer));
	}
	return assets;
}

// 模型 → ECS：场景根（Relationship，parent = null）+ 每 primitive 一个 entity
//（Transform/MeshRef/MaterialRef，双向 Relationship 挂到根下）。
// Phase 2 无层级嵌套：node 变换在 gltf_loader 里已扁平化为 translation，world = local。
export entt::entity registerScene(entt::registry& rg, const RGL::GltfModel& model) {
	// 模型在场 → 自适应空间范围（renderSystem 的 try_get<ModelBounds> 命中后
	// 按包围球推导 near/far）
	rg.ctx().emplace<ModelBounds>(model.bounds.center, model.bounds.radius);

	auto sceneRoot = rg.create();
	auto& rootRel = rg.emplace<Relationship>(sceneRoot);

	for (size_t i = 0; i < model.primitives.size(); ++i) {
		const auto& prim = model.primitives[i];
		auto e = rg.create();

		glm::mat4 local = glm::translate(glm::mat4(1.0f), prim.translation);
		auto& tf = rg.emplace<Transform>(e);
		tf.local = local;
		tf.world = local;

		auto& meshRef = rg.emplace<MeshRef>(e);
		meshRef.primitiveIndex = static_cast<uint32_t>(i);
		meshRef.triangleCount = prim.triangleCount;

		rg.emplace<MaterialRef>(e).baseColor = prim.baseColor;

		rootRel.children.push_back(e);
		rg.emplace<Relationship>(e).parent = sceneRoot;
	}
	return sceneRoot;
}

}  // namespace RGL
