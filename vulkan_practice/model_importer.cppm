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
// + 每 primitive 一个 entity（Transform/MeshRef{asset}/MaterialRef{material}，Relationship parent 挂根）。
// assetIds / primitiveTextureIds 均与 model.primitives 下标对齐（uploadModelMeshes /
// uploadModelTextures 返回，上传顺序 = primitive 顺序）。
// Phase 2 无层级嵌套：node 变换在 gltf_loader 里已扁平化为 translation，world = local。
// 包围球经 Bounds 组件进 registry，由 boundsSystem 聚合进 SceneBounds（自适应 near/far，
// 见 ecs.cppm）——cube 将来入 ECS 时挂自己的 Bounds 即自动纳入聚合。
// 批次 2：材质资产注册（按 glTF materialIndex 去重，-1 = 无材质 → 默认材质）+ MaterialRef
// 真引用（E5.3）——材质身份（baseColor/textureId/alphaMode）收敛进资产表，纹理→primitive
// 映射获得数据归属；同 materialIndex 的材质字段一致性做导入期金丝雀（违例 = loader bug）。
export entt::entity registerScene(entt::registry& rg, const RGL::GltfModel& model,
	std::span<const uint32_t> assetIds, std::span<const uint32_t> primitiveTextureIds,
	RGL::Resources& resources) {
	auto sceneRoot = rg.create();
	rg.emplace<Relationship>(sceneRoot);  // parent = null（根）；children 不存储（单向，E5.2）
	rg.emplace<Bounds>(sceneRoot, model.bounds.center, model.bounds.radius);

	const size_t n = model.primitives.size();
	auto& assets = resources.getAssetTable();

	// 材质资产（按 materialIndex 去重；-1 = 无材质 → 默认材质 {白 baseColor, 无纹理, OPAQUE}）
	std::map<int, uint32_t> materialOfIndex;
	std::map<int, size_t> firstPrimOfIndex;  // 每个 materialIndex 的首个 primitive（取字段样本）
	for (size_t i = 0; i < n; ++i)
		firstPrimOfIndex.emplace(model.primitives[i].materialIndex, i);
	for (const auto& [index, first] : firstPrimOfIndex) {
		const auto& prim = model.primitives[first];
		// 材质引用的纹理（baseColorTexture 的 image → 纹理资产 id；image 去重缓存保证
		// 同图同资产，取任一"该图 + 有 uv"的 primitive 的有效 id 即得）；缺纹理/无内嵌
		// 数据/全模型无 uv 可用 → kInvalidTextureId（有效选择 = 白，见 MaterialAsset 注释）
		uint32_t textureId = kInvalidTextureId;
		if (prim.baseColorTextureIndex >= 0 && !model.images[prim.baseColorTextureIndex].data.empty()) {
			for (size_t j = 0; j < n; ++j)
				if (model.primitives[j].baseColorTextureIndex == prim.baseColorTextureIndex &&
					model.primitives[j].hasUv) {
					textureId = primitiveTextureIds[j];
					break;
				}
		}
		materialOfIndex[index] = assets.addMaterial(
			MaterialAsset{prim.baseColor, textureId, prim.alphaModeBlend});
	}

	// per-primitive 材质引用 + 一致性金丝雀（glTF 格式保证同 materialIndex = 同材质字段：
	// baseColorFactor/alphaMode；违例 = loader bug，启动期直接报错，不静默带病渲染）。
	// 不校验 textureId：材质可引用有数据的 image 但全部 primitive 无 uv（有效纹理 = 白），
	// 该投影含 per-primitive 的 hasUv，不是材质级格式保证。
	std::vector<uint32_t> primitiveMaterialIds(n);
	for (size_t i = 0; i < n; ++i) {
		const auto& prim = model.primitives[i];
		const auto& mat = assets.getMaterial(materialOfIndex[prim.materialIndex]);
		if (mat.baseColor != prim.baseColor || mat.alphaModeBlend != prim.alphaModeBlend) {
			throw std::runtime_error("glTF material inconsistency: primitive " + std::to_string(i) +
				" (materialIndex " + std::to_string(prim.materialIndex) + ")");
		}
		primitiveMaterialIds[i] = materialOfIndex[prim.materialIndex];
	}

	{
		auto* logger = RLLogger::getInstance();
		uint32_t texturedMaterials = 0;
		for (uint32_t i = 0; i < assets.materialSize(); ++i)
			if (assets.getMaterial(i).textureId != kInvalidTextureId) texturedMaterials++;
		if (logger)
			logger->info("materials: {} 资产（{} 带纹理引用），{} primitive",
				assets.materialSize(), texturedMaterials, n);
	}

	for (size_t i = 0; i < n; ++i) {
		const auto& prim = model.primitives[i];
		auto e = rg.create();

		glm::mat4 local = glm::translate(glm::mat4(1.0f), prim.translation);
		auto& tf = rg.emplace<Transform>(e);
		tf.local = local;
		tf.world = local;

		rg.emplace<MeshRef>(e).asset = assetIds[i];
		rg.emplace<MaterialRef>(e).material = primitiveMaterialIds[i];  // 真引用（批次 2，E5.3）
		rg.emplace<Relationship>(e).parent = sceneRoot;
	}
	return sceneRoot;
}

}  // namespace RGL
