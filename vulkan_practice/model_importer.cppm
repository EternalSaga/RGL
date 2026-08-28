module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
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

// GPU 上传：拼接顶点 buffer（每模型一个 BDA storage buffer，CPU→GPU mapped 按各 primitive
// 偏移写入全部顶点数据）+ 一个 heap 描述符槽（整段拼接 buffer = 单一地址范围），注册进
// Resources 资产表（每 primitive 一条 MeshAsset 记录，共享拼接 buffer 的 shared_ptr +
// per-primitive vertexOffset，E1.1）。资产数据生命周期归资产表（Resources 成员），不再经
// 返回值持有（E1.2/L6）。返回资产 id 列表（与 model.primitives 下标对齐：上传顺序 = primitive 顺序）。
//
// Phase 4 批次 2：管线去重依赖"同 (顶点 slot, 纹理 slot) 组合共享一条管线条目"——binding
// mapping 是 shader object 级状态，per-primitive 各占 slot → 392 条；拼接后同模型全 primitive
// 共享一个顶点 slot，仅纹理 slot 差异决定条目拆分（cottage 12 个纹理 slot → 12 条）。
// per-primitive 偏移进 push constant DrawArgs.vertexOffset（model.mesh.slang Load 前缀）。
struct ChunkPrim { uint32_t chunk; uint32_t offset; };

// DIAG（Phase 4 批次 2 判别）：RGL_VTX_CANARY=1 读回拼接 buffer 顶点数据比对 CPU，
// 确认 GPU 上数据是否正确（artifact 根因判别：数据错=上传/memcpy 问题；数据对=读取/
// descriptor/shader 问题）。每块 VkBuffer 基址 32B（= 该块首个 primitive 首顶点）经
// vkCmdCopyBuffer 拷进 staging 回读比对（独立 cmd + fence，启动期一次 GPU 停顿）。
static void verifyVertexUpload(RGL::DeviceContext& vkDevice, RGL::Resources& res,
	const RGL::GltfModel& model, const std::vector<std::shared_ptr<VmaBuffer>>& chunkBuffers,
	const std::vector<ChunkPrim>& primChunk) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	auto& device = vkDevice.getDevice();
	const uint32_t chunkCount = static_cast<uint32_t>(chunkBuffers.size());
	const uint32_t primitiveCount = static_cast<uint32_t>(model.primitives.size());
	// staging（32B/primitive）
	const vk::DeviceSize total = static_cast<vk::DeviceSize>(primitiveCount) * 32;
	vk::BufferCreateInfo si{};
	si.size = total;
	si.usage = vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer;
	si.sharingMode = vk::SharingMode::eExclusive;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
	ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	VmaAllocationInfo ad;
	auto sr = res.getAllocator().createBuffer(si, ai, &ad);
	if (!sr) {
		logger->error("vtx canary: staging alloc FAIL");
		return;
	}
	auto staging = std::move(*sr);
	auto cmdAlloc = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
		*vkDevice.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
	auto cmd = std::move(cmdAlloc[0]);
	cmd.begin(vk::CommandBufferBeginInfo{});
	// 每 primitive 读回首顶点（32B）自其块内偏移（primChunk[i].offset）——查全部非零偏移
	for (uint32_t i = 0; i < primitiveCount; ++i) {
		const VkBufferCopy region{primChunk[i].offset, static_cast<vk::DeviceSize>(i) * 32, 32};
		::vkCmdCopyBuffer(*cmd, **chunkBuffers[primChunk[i].chunk], *staging, 1, &region);
	}
	cmd.end();
	auto fence = vk::raii::Fence(device, vk::FenceCreateInfo{});
	VkCommandBuffer cb = *cmd;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cb;
	::vkQueueSubmit(vkDevice.getGraphicsQueue(), 1, &submitInfo, *fence);
	device.waitForFences(*fence, true, UINT64_MAX);
	uint8_t* back = static_cast<uint8_t*>(ad.pMappedData);
	bool allPass = true;
	uint32_t failCount = 0;
	for (uint32_t i = 0; i < primitiveCount; ++i) {
		const uint8_t* cpu = reinterpret_cast<const uint8_t*>(model.primitives[i].vertices.data());
		const bool pass = std::memcmp(back + i * 32, cpu, 32) == 0;
		if (!pass) {
			++failCount;
			allPass = false;
			if (failCount <= 8)
				logger->info("vtx canary prim {} (chunk {} off {:x}): FAIL (cpu={:08x} gpu={:08x})",
					i, primChunk[i].chunk, primChunk[i].offset,
					*reinterpret_cast<const uint32_t*>(cpu),
					*reinterpret_cast<const uint32_t*>(back + i * 32));
		}
	}
	logger->info("vtx canary: {} ({}/{} primitives, {} chunks)",
		allPass ? "ALL PASS" : "FAIL", primitiveCount - failCount, primitiveCount, chunkCount);
}

export [[nodiscard]] std::expected<std::vector<uint32_t>, Error> uploadModelMeshes(
	RGL::DeviceContext& vkDevice, RGL::Resources& res, const RGL::GltfModel& model) {
	auto* logger = RLLogger::getInstance();
	auto& device = vkDevice.getDevice();
	auto& heap = res.getDescriptorHeap();
	auto& assets = res.getAssetTable();

	const uint32_t primitiveCount = static_cast<uint32_t>(model.primitives.size());

	// Phase 4 批次 2 分块拼接（2026-08-28 判别定案）：单一 82MB 拼接 buffer 的 descriptor
	// range（82MB）在 RADV 上导致局部渲染 artifact（740px 内容→黑，确定性，cottage 特定；
	// grass 小 buffer 正常）。high VA 非根因（4-80MB 各尺寸 BDA buffer 均落 high VA，旧
	// 392 小 buffer 同样 high VA 却正常）；waitIdle 亦无效（非 flush 时序）。分块（每块
	// kVertexChunkBytes，独立 buffer + 独立 descriptor range + 独立 slot）规避大 range，
	// 每块仍被同块内 primitive 共享 vertex slot → 管线去重部分保留（条目 = 块数 × 纹理 slot）。
	// per-primitive：heapSlot = 所在块的 slot，vertexOffset = 块内字节偏移（进 push constant）。
	// DIAG：RGL_CHUNK_MB 可覆盖块大小（MB），测 descriptor range 尺寸阈值（默认 16MB）
	constexpr vk::DeviceSize kDefaultChunkBytes = 16ull * 1024 * 1024;
	const vk::DeviceSize kVertexChunkBytes = [] {
		if (const char* env = std::getenv("RGL_CHUNK_MB")) {
			const unsigned mb = std::strtoul(env, nullptr, 10);
			if (mb >= 1)
				return static_cast<vk::DeviceSize>(mb) * 1024 * 1024;
		}
		return kDefaultChunkBytes;
	}();

	// ① 分块：per-primitive (块序号, 块内偏移) + 每块总字节
	std::vector<ChunkPrim> primChunk(primitiveCount);
	std::vector<vk::DeviceSize> chunkBytes;
	{
		vk::DeviceSize curOffset = 0;
		uint32_t curChunk = 0;
		chunkBytes.push_back(0);
		// DIAG（Phase 4 批次 2 判别）：RGL_ONE_PRIM_PER_CHUNK=1 强制每 primitive 独占一块
		//（等效原 per-primitive 设计但走分块路径 + vertexOffset）——测"多 primitive 共享一块
		// descriptor（多偏移读）"是否是 artifact 根因
		const bool onePrimPerChunk = std::getenv("RGL_ONE_PRIM_PER_CHUNK") != nullptr;
		for (uint32_t i = 0; i < primitiveCount; ++i) {
			const vk::DeviceSize primBytes =
				static_cast<vk::DeviceSize>(model.primitives[i].vertices.size()) * sizeof(ModelVertex);
			if (onePrimPerChunk ? (curOffset > 0)
				: (curOffset > 0 && curOffset + primBytes > kVertexChunkBytes)) {
				++curChunk;
				curOffset = 0;
				chunkBytes.push_back(0);
			}
			primChunk[i] = {curChunk, static_cast<uint32_t>(curOffset)};
			curOffset += primBytes;
			chunkBytes[curChunk] += primBytes;
		}
	}
	const uint32_t chunkCount = static_cast<uint32_t>(chunkBytes.size());

	// ② 每块一个 buffer + 一个 slot（descriptor range = 块大小）；块序连续分配，
	// 首块首 slot 契约 = reserved + HeapSlotModelVertexBase（违例 = 契约漂移）
	std::vector<std::shared_ptr<VmaBuffer>> chunkBuffers(chunkCount);
	std::vector<uint32_t> chunkSlots(chunkCount);
	for (uint32_t c = 0; c < chunkCount; ++c) {
		vk::BufferCreateInfo bufferInfo{};
		bufferInfo.size = chunkBytes[c];
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
		char* mapped = static_cast<char*>(allocData.pMappedData);
		// 块内 primitive 按块内偏移写入
		for (uint32_t i = 0; i < primitiveCount; ++i) {
			if (primChunk[i].chunk != c)
				continue;
			const auto& vertices = model.primitives[i].vertices;
			std::memcpy(mapped + primChunk[i].offset, vertices.data(),
				static_cast<vk::DeviceSize>(vertices.size()) * sizeof(ModelVertex));
		}
		const uint32_t expectedSlot = heap.getReservedSlots() + HeapSlotModelVertexBase + assets.getModelSlotCount();
		auto slotResult = res.allocSlot("model.vertices.chunk");
		if (!slotResult)
			return std::unexpected(std::move(slotResult).error());
		const uint32_t slot = *slotResult;
		if (slot != expectedSlot)
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				"heap slot layout drift: model.vertices.chunk[" + std::to_string(c) + "] (slot " +
				std::to_string(slot) + " != expected " + std::to_string(expectedSlot) + ")"});
		auto slotAddr = heap.getSlotCpuAddress(slot);
		if (!slotAddr)
			return std::unexpected(std::move(slotAddr).error());
		vk::DeviceAddressRangeEXT range{buffer.getDeviceAddress(), chunkBytes[c]};
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eStorageBuffer;
		desc.data.pAddressRange = &range;
		vk::HostAddressRangeEXT dst{*slotAddr, heap.getDescriptorSize()};
		device.writeResourceDescriptorsEXT(desc, dst);
		chunkBuffers[c] = std::make_shared<VmaBuffer>(std::move(buffer));
		chunkSlots[c] = slot;
		assets.incrementModelSlotCount();
	}

	// DIAG（Phase 4 批次 2 判别）：临时跳过 slot 把纹理推回 batch1 布局（394-405），
	// 测"纹理 slot 位置（8-19 vs 394-405）"是否是 artifact 根因。配合 texture_pool 白契约临时改 394
	if (std::getenv("RGL_SKIP_TO_TEX394") != nullptr) {
		const uint32_t target = heap.getReservedSlots() + HeapSlotModelVertexBase + 392;  // 白应落此槽（reserved+394，batch1 布局）
		while (true) {
			auto r = res.allocSlot("DIAG.skip");
			if (!r)
				return std::unexpected(std::move(r).error());
			if (*r == target - 1)
				break;  // nextSlot 现为 target，白纹理将落于 target
		}
	}

	// ③ 注册 per-primitive 资产记录（所在块的 shared_ptr + 块 slot + 块内偏移）
	std::vector<uint32_t> assetIds;
	assetIds.reserve(primitiveCount);
	for (uint32_t i = 0; i < primitiveCount; ++i) {
		const auto& prim = model.primitives[i];
		const uint32_t id = assets.addMesh(chunkBuffers[primChunk[i].chunk], chunkSlots[primChunk[i].chunk],
			prim.triangleCount, primChunk[i].offset);
		assetIds.push_back(id);
	}
	if (logger)
		logger->info("model: {} primitives → {} 块拼接顶点 buffer (每块 ≤{}KB) / {} heap slot",
			primitiveCount, chunkCount, kVertexChunkBytes / 1024, chunkCount);
	if (std::getenv("RGL_VTX_CANARY") != nullptr)
		verifyVertexUpload(vkDevice, res, model, chunkBuffers, primChunk);
	return assetIds;
}

// 模型 → ECS：场景根（Relationship parent = null；Bounds 组件 = 模型整体包围球）
// + 每 primitive 一个 entity（Transform/MeshRef{asset}/MaterialRef{material}，Relationship parent 挂根）。
// assetIds / primitiveTextureIds 均与 model.primitives 下标对齐（uploadModelMeshes /
// uploadModelTextures 返回，上传顺序 = primitive 顺序）。
// Phase 2 无层级嵌套：node 变换在 gltf_loader 里已扁平化为 translation，world = local。
// 包围球经 Bounds 组件进 registry，由 boundsSystem 聚合进 SceneBounds（自适应 near/far，
// 见 ecs.cppm）——cube 同理（批次 3 registerCubeEntity 挂自己的 Bounds，自动纳入聚合）。
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
			MaterialAsset{prim.baseColor, textureId, prim.alphaModeBlend,
				prim.metallic, prim.roughness, prim.unlit});
	}

	// per-primitive 材质引用 + 一致性金丝雀（glTF 格式保证同 materialIndex = 同材质字段：
	// baseColorFactor/alphaMode/metallicFactor/roughnessFactor/unlit 扩展；违例 = loader bug，
	// 启动期直接报错，不静默带病渲染）。
	// 不校验 textureId：材质可引用有数据的 image 但全部 primitive 无 uv（有效纹理 = 白），
	// 该投影含 per-primitive 的 hasUv，不是材质级格式保证。
	std::vector<uint32_t> primitiveMaterialIds(n);
	for (size_t i = 0; i < n; ++i) {
		const auto& prim = model.primitives[i];
		const auto& mat = assets.getMaterial(materialOfIndex[prim.materialIndex]);
		if (mat.baseColor != prim.baseColor || mat.alphaModeBlend != prim.alphaModeBlend ||
			mat.metallic != prim.metallic || mat.roughness != prim.roughness ||
			mat.unlit != prim.unlit) {
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
