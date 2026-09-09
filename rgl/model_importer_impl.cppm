module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <entt/entt.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <fmt/format.h>
module model_importer:impl;

import model_importer;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import diagnostics;  // vtx_canary 诊断单例（configs/diagnostics.lua）
import vkerror;
import VmaRaii;
import vk_device;
import vk_resources;
import asset_table;
import scene_context;
import gltf_loader;
import model_types_generated;

namespace RGL {

// model_importer 实现单元（2026-09-02 编译提速 A 方案）：两个导出函数的函数体 +
// 文件级 static 辅助整体迁出——接口 TU 只留声明，大函数体 AST 不再序列化进 BMI；
// 本单元是叶子（无人 import），编译完全并行、不上关键路径。
// 设计语义注释留在接口声明处。

/**
 * per-primitive 块映射（Phase 4 批次 2 分块拼接）：chunk = 所属 16MB 块下标，
 * offset = 该 primitive 首顶点在块内的字节偏移（进 push constant DrawArgs.vertexOffset，
 * model.mesh.slang Load 前缀）。
 */
struct ChunkPrim { uint32_t chunk; uint32_t offset; };

/**
 * 顶点读回金丝雀：configs/diagnostics.lua vtx_canary=true 时读回拼接 buffer 顶点数据比对
 * CPU，确认上传保真度（数据错 = 上传/memcpy 问题；数据对 = 读取/shader 侧）。逐
 * primitive 读回首顶点 32B（自其块内偏移 primChunk[i].offset——查全部非零偏移），每
 * primitive 一个 staging 槽（32B），vkCmdCopyBuffer 拷进 staging 回读比对（独立 cmd +
 * fence，启动期一次 GPU 停顿）；PASS/FAIL 只打日志，不影响上传结果。
 * @param vkDevice 设备层（graphics 命令池 / 队列 / fence）
 * @param res 资源层（VMA 分配器：staging 回读缓冲）
 * @param model CPU 侧模型（比对源：每 primitive 顶点前 32B）
 * @param chunkBuffers 逐块顶点 buffer（读回源）
 * @param primChunk 逐 primitive (块下标, 块内偏移) 映射
 * @note waitForFences 返回值（nodiscard）在调用点被丢弃——既有 warning，代码层问题
 *   （诊断路径未处理），非注释可修
 */
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

[[nodiscard]] std::expected<std::vector<uint32_t>, Error> uploadModelMeshes(
	RGL::DeviceContext& vkDevice, RGL::Resources& res, const RGL::GltfModel& model) {
	auto* logger = RLLogger::getInstance();
	auto& device = vkDevice.getDevice();
	auto& heap = res.getDescriptorHeap();
	auto& assets = res.getAssetTable();

	const uint32_t primitiveCount = static_cast<uint32_t>(model.primitives.size());

	// Phase 4 批次 2 分块拼接：每模型 primitive 拼成 16MB 块，每块一个 BDA buffer +
	// 一个 descriptor + 一个 heap slot（cottage 392 primitive 顶点 slot → 6 块）。分块的
	// 动机是 **heap slot 收敛以服务业管线去重**（D5：条目 = 块数 × 纹理 slot），非规避
	// cottage artifact——artifact 根因 2026-08-30 定位为 gltf_loader uv 越界读（与 range
	// 尺寸/分块无关，见 docs/archive/phase4_plan.md 批次 2 定案注记）。每块仍被同块内 primitive 共享
	// vertex slot。per-primitive：heapSlot = 所在块的 slot，vertexOffset = 块内字节偏移（进 push constant）。
	constexpr vk::DeviceSize kVertexChunkBytes = 16ull * 1024 * 1024;  // 16MB 块

	// ① 分块：per-primitive (块序号, 块内偏移) + 每块总字节
	std::vector<ChunkPrim> primChunk(primitiveCount);
	std::vector<vk::DeviceSize> chunkBytes;
	{
		vk::DeviceSize curOffset = 0;
		uint32_t curChunk = 0;
		chunkBytes.push_back(0);
		for (uint32_t i = 0; i < primitiveCount; ++i) {
			const vk::DeviceSize primBytes =
				static_cast<vk::DeviceSize>(model.primitives[i].vertices.size()) * sizeof(ModelVertex);
			if (curOffset > 0 && curOffset + primBytes > kVertexChunkBytes) {
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
	// 首块首 slot 契约 = expectedGeometrySlot()（违例 = 契约漂移）
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
		const uint32_t expectedSlot = assets.expectedGeometrySlot(heap.getReservedSlots());
		auto slotResult = res.allocSlot("model.vertices.chunk");
		if (!slotResult)
			return std::unexpected(std::move(slotResult).error());
		const uint32_t slot = *slotResult;
		if (slot != expectedSlot)
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				fmt::format("heap slot layout drift: model.vertices.chunk[{}] (slot {} != expected {})",
					c, slot, expectedSlot)});
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
		assets.incrementGeometrySlotCount();
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
	if (diagnostics().vtxCanary)
		verifyVertexUpload(vkDevice, res, model, chunkBuffers, primChunk);
	return assetIds;
}

// 模型 → ECS：场景根（Relationship parent = null；Bounds 组件 = 模型整体包围球）
// + 每 primitive 一个 entity（Transform/MeshRef{asset}/MaterialRef{material}，Relationship parent 挂根）。
// assetIds / primitiveTextureIds 均与 model.primitives 下标对齐（uploadModelMeshes /
// uploadModelTextures 返回，上传顺序 = primitive 顺序）。
// Phase 2 无层级嵌套：node 变换在 gltf_loader 里已扁平化为 translation，world = local。
// 包围球经 Bounds 组件进 registry，由 boundsSystem 聚合进 SceneBounds（自适应 near/far，
// 见 ecs.cppm）——内建几何同理（registerBuiltinCube 挂自己的 Bounds，自动纳入聚合）。
// 批次 2：材质资产注册（按 glTF materialIndex 去重，-1 = 无材质 → 默认材质）+ MaterialRef
// 真引用（E5.3）——材质身份（baseColor/textureId/alphaMode）收敛进资产表，纹理→primitive
entt::entity registerScene(entt::registry& rg, const RGL::GltfModel& model,
	std::span<const uint32_t> assetIds, std::span<const uint32_t> primitiveTextureIds,
	RGL::Resources& resources, const glm::mat4& rootTransform) {
	auto sceneRoot = rg.create();
	rg.emplace<Relationship>(sceneRoot);  // parent = null（根）；children 不存储（单向，E5.2）
	// 场景根位姿（Phase 6 批次 5：多模型场景 entity 级 transform）：根 local =
	// rootTransform；Bounds 球心随根变换、半径乘均匀缩放分量（列 0 长度）
	auto& rootTf = rg.emplace<Transform>(sceneRoot);
	rootTf.local = rootTransform;
	rootTf.world = rootTransform;
	const float rootScale = glm::length(glm::vec3(rootTransform[0]));
	const glm::vec3 boundsCenter = glm::vec3(rootTransform *
		glm::vec4(model.bounds.center, 1.0f));
	rg.emplace<Bounds>(sceneRoot, boundsCenter, model.bounds.radius * rootScale);

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
			throw std::runtime_error(fmt::format(
				"glTF material inconsistency: primitive {} (materialIndex {})", i, prim.materialIndex));
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

		glm::mat4 local = prim.nodeWorld;  // Phase 7：node 世界矩阵（TRS + 父链，loader 合成，不烘顶点）
		auto& tf = rg.emplace<Transform>(e);
		tf.local = local;
		tf.world = rootTransform * local;  // 根位姿复合（批次 5；identity = 旧行为）

		auto& meshRef = rg.emplace<MeshRef>(e);
		meshRef.asset = assetIds[i];
		meshRef.objectSlot = rg.ctx().get<NextObjectSlot>().next++;  // 实例矩阵槽（instancing）
		rg.emplace<MaterialRef>(e).material = primitiveMaterialIds[i];  // 真引用（批次 2，E5.3）
		rg.emplace<Relationship>(e).parent = sceneRoot;
		rg.emplace<CullSphere>(e, prim.bounds.center, prim.bounds.radius);  // Phase 7 批次 4：node-local 剔除球
	}
	return sceneRoot;
}

}  // namespace RGL
