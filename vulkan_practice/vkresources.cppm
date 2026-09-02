module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <fmt/format.h>
export module vk_resources;

import rlstd;
import rllogger;
import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import cube;
import vkerror;
import types_generated;
import model_types_generated;

namespace RGL {

// 管线表下标的"未设置"值（资产记录的 pipelineIndex 初始值；ShaderRegistry::addModelEntry / bindPipelineToAsset 写入真实下标）
export inline constexpr uint32_t kInvalidPipelineIndex = std::numeric_limits<uint32_t>::max();

// ===== SceneData：场景级每帧动态数据的 bindless 契约（Phase 5 批次 2）=====
// 单一 storage buffer（heap slot 经 allocSlot 运行时分配——纹理槽数随场景动态，
// light 槽无法钉编译期偏移，同 slotTexture 模式），布局（字节偏移，shader 端
// scene.slang 字面量必须一致，kTrianglesPerGroup 先例）：
//   @0   LightDataGpu（48B：direction/eyePos/color/ambient 各 vec3）
//   @48  Phase 5.5 env 参数（原 16B 保留区启用）：envIntensity float + envMipRange
//        float（= mipLevels−1，0 = 无环境）+ 8B 留白——scene.slang/env.slang 字面量一致
//   @64  Phase 5.5 批次 4 skyboxInvViewProj（mat4 列主序 = inverse(proj·view)，
//        sky.slang skyClipToWorld 消费）
//   @128 对象矩阵表：mat4 列主序 × kSceneMatrixCapacity（[assetId] = spin·world，
//        DrawArgs.objectIndex 索引；世界空间光照 + Phase 7 shadow/culling 复用）
// 内容每帧变 = 全项目首个动态 GPU 数据：CPU 写 staging ring（×MAX_FRAMES_IN_FLIGHT，
// drawFrame ① 已 wait 本 slot fence → 写安全）+ 录制期 vkCmdCopyBuffer 拷进本 buffer
//（命令流内天然有序；描述符静态指向本 buffer 永不改动，见 vkrenderer.cppm）。
export struct LightDataGpu {
	glm::vec3 direction{0.0f, -1.0f, 0.0f};  // @0 世界空间光方向（归一化，照射方向）
	glm::vec3 eyePos{0.0f};                    // @12 世界空间相机位置（高光视向）
	glm::vec3 color{1.0f};                     // @24 光色 × intensity（CPU 预乘）
	glm::vec3 ambient{0.1f};                   // @36 环境光
};
static_assert(sizeof(LightDataGpu) == 48);

// Phase 5.5：env 参数偏移（envIntensity float @48 + envMipRange float @52；
// env.slang readEnvParams 的 Load(48)/Load(52) 字面量必须与本值一致——scene.slang 先例）
export inline constexpr uint32_t kSceneDataEnvOffset = sizeof(LightDataGpu);
// Phase 5.5 批次 4：skyboxInvViewProj mat4 列主序 @64（inverse(proj·view)，
// sky.slang skyClipToWorld 的 Load4(64/80/96/112) 字面量必须与本值一致）
export inline constexpr uint32_t kSceneDataSkyOffset = 64;
export inline constexpr uint32_t kSceneDataHeaderBytes = 128;   // light 48 + env 16 + sky invVP 64
static_assert(kSceneDataEnvOffset == 48 && kSceneDataSkyOffset == 64 &&
	kSceneDataSkyOffset + sizeof(glm::mat4) == kSceneDataHeaderBytes);
// mat4 列主序 stride：派生自 sizeof(glm::mat4) 而非裸字面量（C++ 侧漂移即编译失败；
// shader 端 scene.slang 的字面量 64u 须与本值一致——无反射端对应，见 scene.slang 头注释）
export inline constexpr uint32_t kSceneMatrixStride = sizeof(glm::mat4);
export inline constexpr uint32_t kSceneMatrixCapacity = 1024;   // 对象矩阵表容量（main 启动期契约检查）
export inline constexpr vk::DeviceSize kSceneDataBytes =
	kSceneDataHeaderBytes + vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride;

// 纹理资产 id 的"未设置"值（同 kInvalidPipelineIndex 模式；批次 2 材质资产的 textureId 用它 = 无纹理 → 白）
export inline constexpr uint32_t kInvalidTextureId = std::numeric_limits<uint32_t>::max();

// 资产种类（批次 1 引入：白纹理 slot 契约断言只数模型顶点槽；批次 3 cube 实体化数据源）。
// Phase 4 已归一 push constant（统一 DrawArgs 超集单一布局）：renderListSystem 零 AssetKind
// 分支，kind 不再参与布局/写入路径选择（历史 64B/96B 双 struct 分支已删）
export enum class AssetKind : uint8_t {
	Model,
	Cube,
};

// 资产层：每个已上传 primitive 一条记录（GPU 顶点 buffer + heap slot + 三角形数 + 管线条目引用），
// 以稳定 id（vector 下标，上传顺序分配）索引。registry entity 经 MeshRef{asset}（scene.cppm）
// 引用资产，五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为本表
// 一处映射（E1.1）。生命周期：Resources 成员（与 descriptor heap / cube buffer 同层），
// buffer 随 SharedAllocator 控制块存活，不再靠 main.cpp 局部变量的隐式 RAII 保证（E1.2/L6）。
// 顶点 buffer 经 shared_ptr 共享（Phase 4 批次 2：拼接顶点 buffer——每模型一个 buffer +
// 一个 heap slot，同模型 N 个 primitive 资产记录持同一 shared_ptr，末引用析构释放；
// VmaBuffer 本体 move-only 不可拷贝，故经 shared_ptr 共享所有权）。
export struct MeshAsset {
	AssetKind kind = AssetKind::Model;
	std::shared_ptr<VmaBuffer> vertexBuffer;          // 顶点 buffer（模型 = 拼接 buffer 共享；cube = 独占 shared_ptr）
	uint32_t heapSlot = 0;                            // 绝对 heap slot（= reservedSlots() + 偏移，见 uploadModelMeshes）
	uint32_t triangleCount = 0;                       // primitive 三角形数
	uint32_t groupCount = 0;                          // mesh task group 数（上传期按 shader 契约派生，D6：
	                                                 //   model = ceil(triangleCount / kTrianglesPerGroup)，cube = 1 固定单 group——
	                                                 //   cube mesh shader 恒输出 8 顶点/12 三角形，不套 ceil 公式（group>1 会越界读）；
	                                                 //   0 = 不渲染。renderListSystem 每帧零推导零分支直接取）
	uint32_t pipelineIndex = kInvalidPipelineIndex;   // 管线表下标（ShaderRegistry::addModelEntry / bindPipelineToAsset 写入）
	uint32_t vertexOffset = 0;                        // 拼接顶点 buffer 内 per-primitive 字节偏移（Phase 4 批次 2；
	                                                 //   进 push constant 的 DrawArgs.vertexOffset，model.mesh.slang Load 前缀；cube = 0）
	// 索引 buffer（批次 3：cube 资产带索引；模型 primitive 去索引展开 = nullopt）
	std::optional<VmaBuffer> indexBuffer;
	std::optional<uint32_t> indexHeapSlot;
};

// 纹理资产（Phase 3 → 批次 1 迁入资产表）：GPU image + view + heap 描述符槽 + 描述符指针链。
// 原 owner = main 局部 TexturePool 对象（UAF 根因：块尾析构第 0 帧前释放纹理 BO，
// docs/heap_texture_research.md §12.6）；现活在 AssetTable（Resources 成员），VmaImage
// 包装体随 Resources 存活——所有权规则（§12.7 ①）在此首次落地。
// viewCreate 必须保留：v341 的 VkImageDescriptorInfoEXT::pView 要 image view 的
// create info（值拷贝），不是句柄（docs/heap_texture_research.md §二 条款表）。
// 描述符指针链（imageDesc/resourceDesc/dst）折进记录本体：记录活在 std::deque
//（push_back 元素地址稳定），自/兄弟引用在入表后设置——跨驱动防御稳定存储的单一介质
//（替代原 TexturePool 的 4 个 deque；本机 RADV 写时即解析，spec 允许驱动延迟解释，
// 见 docs/heap_texture_research.md §12.2）。
export struct TextureAsset {
	VmaImage image;
	vk::ImageViewCreateInfo viewCreate{};  // hpp create info（值保留）：v341 VkImageDescriptorInfoEXT::pView 要它
	vk::raii::ImageView view;
	uint32_t heapSlot = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	// Phase 5.5：mip 级数（2D 路径 = 1；cubemap = 1+log2(size)，sky sampler maxLod 由此派生）
	uint32_t mipLevels = 1;
	// 描述符指针链（自/兄弟引用，入 deque 后设置——元素地址稳定保证指针有效）
	vk::ImageDescriptorInfoEXT imageDesc{};        // pView = &viewCreate，layout = eShaderReadOnlyOptimal
	vk::ResourceDescriptorInfoEXT resourceDesc{};  // type = eSampledImage，data.pImage = &imageDesc
	vk::HostAddressRangeEXT dst{};                 // 描述符写入目标 slot 范围
};

// 材质资产（批次 2：MaterialRef 真引用，E5.3 解决）：glTF 材质身份（按 materialIndex 去重，
// -1 = 无材质 → 默认材质）。纹理→primitive 映射获得数据归属——材质记录引用纹理资产 id
//（可查询，Phase 5 透明排序 / Phase 7 culling 的材质分类数据基础）；glTF 材质身份不再
// 导入期丢弃。注意区分：textureId = 材质**引用**的纹理（baseColorTexture，可缺省）；
// primitive 的**有效**纹理 = 有 uv 且图有内嵌数据 ? material.textureId : 白（hasUv 是
// per-primitive 网格属性，同材质可因缺 uv 走白——有效选择留在上传返回值，见 model_importer）。
// GPU 侧仍走 per-primitive 管线 binding mapping 烘焙（行为不变，去重归 Phase 4 ShaderRegistry）。
export struct MaterialAsset {
	glm::vec4 baseColor{1.0f};
	uint32_t textureId = kInvalidTextureId;
	bool alphaModeBlend = false;  // glTF alphaMode == "BLEND"（Phase 5 透明用，预留）
	// Phase 4 批次 3：材质风格字段（registerScene 金丝雀保证同 materialIndex 一致）
	float metallic = 1.0f;        // pbrMetallicRoughness.metallicFactor（缺省 1.0）
	float roughness = 1.0f;       // pbrMetallicRoughness.roughnessFactor（缺省 1.0）
	bool unlit = false;           // KHR_materials_unlit 扩展存在性（风格 = unlit vs pbr）
};

export class AssetTable {
   public:
	// mesh（模型 primitive：去索引顶点流，无索引 buffer）
	// 注册资产（上传顺序调用）→ 资产 id
	// groupCount 按 model shader 契约派生（D6：ceil(tri/kTrianglesPerGroup)，0 = 不渲染）
	// vertexBuffer = 拼接顶点 buffer 的 shared_ptr（同模型 N 个 primitive 共享同一 buffer，
	// Phase 4 批次 2）；vertexOffset = 本 primitive 在拼接 buffer 内的字节偏移（进 push constant）
	uint32_t addMesh(std::shared_ptr<VmaBuffer> vertexBuffer, uint32_t heapSlot,
		uint32_t triangleCount, uint32_t vertexOffset) {
		const uint32_t groupCount = (triangleCount + kTrianglesPerGroup - 1) / kTrianglesPerGroup;
		meshes.push_back(
			MeshAsset{AssetKind::Model, std::move(vertexBuffer), heapSlot, triangleCount, groupCount,
				kInvalidPipelineIndex, vertexOffset});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	[[nodiscard]] const MeshAsset& get(uint32_t id) const { return meshes[id]; }
	// 管线条目引用（ShaderRegistry::addModelEntry / bindPipelineToAsset 在创建管线条目后写回）
	void setPipelineIndex(uint32_t id, uint32_t pipelineIndex) { meshes[id].pipelineIndex = pipelineIndex; }
	// 带索引 mesh（批次 3：cube 资产——顶点 + 索引双 buffer/双 slot）
	// groupCount = 1 固定（cube mesh shader 恒输出 8 顶点/12 三角形，单 group，不套 ceil 公式）
	uint32_t addIndexedMesh(VmaBuffer vertexBuffer, uint32_t vertexSlot, VmaBuffer indexBuffer,
		uint32_t indexSlot, uint32_t triangleCount) {
		meshes.push_back(MeshAsset{AssetKind::Cube, std::make_shared<VmaBuffer>(std::move(vertexBuffer)),
			vertexSlot, triangleCount, 1, kInvalidPipelineIndex, 0, std::move(indexBuffer), indexSlot});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	[[nodiscard]] uint32_t meshSize() const { return static_cast<uint32_t>(meshes.size()); }
	// 模型顶点槽计数（Phase 4 批次 2：拼接顶点 buffer = 每模型一个 slot，白纹理 slot 契约断言用：
	// "纹理首槽 = 白 = 模型顶点槽之后"）。按模型数（非 primitive 数）计数——uploadModelMeshes
	// 每模型上传期递增一次；批次 3 cube 资产与本容器共用但不占模型顶点槽
	uint32_t modelSlotCount = 0;
	[[nodiscard]] uint32_t getModelSlotCount() const { return modelSlotCount; }
	void incrementModelSlotCount() { ++modelSlotCount; }

	// 纹理（deque：元素地址稳定 = 描述符指针链的稳定存储介质，见 TextureAsset 注释）
	// 放置资产记录（deque）→ 调用方在返回引用上设置自/兄弟引用后写描述符 → 资产 id
	TextureAsset& emplaceTexture(TextureAsset asset) {
		textures.push_back(std::move(asset));
		return textures.back();
	}
	[[nodiscard]] const TextureAsset& getTexture(uint32_t id) const { return textures[id]; }
	[[nodiscard]] uint32_t textureSize() const { return static_cast<uint32_t>(textures.size()); }

	// 材质（批次 2：MaterialRef{material} 真引用；registerScene 按 glTF materialIndex 去重注册）
	uint32_t addMaterial(MaterialAsset material) {
		materials.push_back(std::move(material));
		return static_cast<uint32_t>(materials.size() - 1);
	}
	[[nodiscard]] const MaterialAsset& getMaterial(uint32_t id) const { return materials[id]; }
	[[nodiscard]] uint32_t materialSize() const { return static_cast<uint32_t>(materials.size()); }

   private:
	std::vector<MeshAsset> meshes;
	std::deque<TextureAsset> textures;
	std::vector<MaterialAsset> materials;
};

export class Resources {
   public:
	// heap 容量与场景内容解耦（E1.3，批次 3）：固定裕量 kUserSlotMargin，不再按
	// primitive 数精确预分配（原 modelPrimitiveCount 构造参数删除）。用户槽位布局
	//（相对 reserved range 起点，Phase 3 统一 slot 分配器 allocSlot 连续分配）：
	//   [0, 2)    cube（顶点 slot 0 / 索引 slot 1，types_generated）
	//   [2, 2+N)  模型顶点 slot（每模型一个——拼接顶点 buffer，Phase 4 批次 2；
	//   slot = reserved + HeapSlotModelVertexBase + 模型序，见 model_importer.cppm）
	//   [2+N, …)  纹理 slot（texture_pool.cppm uploadModelTextures：2+N = 白 1×1 特例（契约断言），
	//   其余按上传序，资产记录在 AssetTable，见 texture_pool.cppm）
	//   其余为裕量；运行时扩容 = 重建 heap + 重写全部描述符
	Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain)
		: vkInstance(vkInstance),
		  vkDevice(vkDevice),
		  vkSwapchain(vkSwapchain),
 	  // flags 显式传 BDA（第 5 参）——VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT 不可省略，
 	  // 否则所有 eShaderDeviceAddress buffer 创建返回 VK_ERROR_INITIALIZATION_FAILED
 	  allocator(*vkInstance.getInstance(), *vkDevice.getPhysicalDevice(), *vkDevice.getDevice(),
 		  vkInstance.getApiVersion(),
 		  VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT),
		  depthImage(createDepthImage(allocator, vkSwapchain.getExtent()).value()),
		  depthImageView(vkDevice.getDevice(), buildDepthView(*depthImage)),
		  vertexBuffer(createGpuBuffer(
			  RGL::cubeVertices.size() * sizeof(RGL::Vertex),
			  vk::BufferUsageFlagBits::eStorageBuffer,
			  RGL::cubeVertices.data())
				  .value()),
		  indexBuffer(createGpuBuffer(
			  RGL::cubeIndices.size() * sizeof(uint32_t),
			  vk::BufferUsageFlagBits::eStorageBuffer,
			  RGL::cubeIndices.data())
				  .value()),
		  descriptorHeap(createDescriptorHeap().value()),
		  samplerHeap(createSamplerHeap().value()),
		  nextSlot(reservedSlots()) {
		// cube 槽位经统一分配器（Phase 3）：布局约定由反射生成的偏移常量钉死，违例 = 契约漂移
		auto vertexSlotResult = allocSlot("cube.vertex");
		if (!vertexSlotResult)
			throw std::runtime_error(vertexSlotResult.error().describe());
		heapSlotVertex = *vertexSlotResult;
		if (heapSlotVertex != reservedSlots() + HeapSlotVertexOffset)
			throw std::runtime_error("heap slot layout drift: cube.vertex");
		auto indexSlotResult = allocSlot("cube.index");
		if (!indexSlotResult)
			throw std::runtime_error(indexSlotResult.error().describe());
		heapSlotIndex = *indexSlotResult;
		if (heapSlotIndex != reservedSlots() + HeapSlotIndexOffset)
			throw std::runtime_error("heap slot layout drift: cube.index");
		writeDescriptors();
		// 批次 3：cube 一等资产——buffer 移入资产记录（kind=Cube，带索引），包装体随
		// Resources 存活（与模型 mesh 同层）；成员 vertexBuffer/indexBuffer move 后为空，
		// 访问一律经 cubeAssetId() → 资产表（删了直接访问器防 move 后误用）
		cubeAsset = assetTable.addIndexedMesh(std::move(vertexBuffer), heapSlotVertex,
			std::move(indexBuffer), heapSlotIndex,
			static_cast<uint32_t>(RGL::cubeIndices.size() / 3));
	}

	const RGL::VmaImage& getDepthImage() const { return depthImage; }
	const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }

	[[nodiscard]] std::expected<void, Error> recreateDepthImage(vk::Extent2D extent) {
		vkDevice.getDevice().waitIdle();

		auto result = createDepthImage(allocator, extent);
		if (!result) return std::unexpected(std::move(result).error());
		depthImage = std::move(*result);
		depthImageView = vk::raii::ImageView(vkDevice.getDevice(), buildDepthView(*depthImage));
		return {};
	}

	const RGL::VmaDescriptorHeap& getDescriptorHeap() const { return descriptorHeap; }
	const RGL::VmaDescriptorHeap& getSamplerHeap() const { return samplerHeap; }
	const RGL::VmaAllocatorRAII& getAllocator() const { return allocator; }
	vk::DeviceSize getReservedRangeOffset() const { return 0; }
	vk::DeviceSize getReservedRangeSize() const { return descriptorHeap.getReservedSlots() * descriptorHeap.getDescriptorSize(); }
	// sampler heap 绑定的保留区（同 resource heap 语义：reserved 槽声明为不可用）
	vk::DeviceSize getSamplerReservedRangeSize() const { return samplerHeap.getReservedSlots() * samplerHeap.getDescriptorSize(); }
	// 共享 sampler（批次 1：owner 从 TexturePool 构造移入 Resources——初始化层对象持有）：
	// 一份 sampler 描述符（linear + CLAMP_TO_EDGE，无 mipmap）写 sampler heap 首个可用槽，
	// 全模型管线映射指向同一槽。幂等（首次调用写入，之后返回既有槽号）；调用点 = 创建
	// textured 管线前（main 启动期）。
	// sampler create info 存成员（稳定存储）：本机 RADV 写时即解析
	//（docs/heap_texture_research.md §12.2，"采样期解引用"旧归因已证伪），
	// 但 spec 允许驱动延迟解释，生命周期须覆盖运行期
	[[nodiscard]] uint32_t ensureSharedSampler() {
		if (sharedSamplerReady)
			return sharedSamplerSlot;
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.anisotropyEnable = VK_FALSE;
		info.compareEnable = VK_FALSE;
		info.minLod = 0.0f;
		info.maxLod = 0.0f;
		info.unnormalizedCoordinates = VK_FALSE;
		info.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
		sharedSamplerSlot = samplerHeap.getReservedSlots();
		auto slotAddr = samplerHeap.getSlotCpuAddress(sharedSamplerSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, samplerHeap.getDescriptorSize()};
		samplerCreateInfo = vk::SamplerCreateInfo(info);
		// hpp raii 内部 resultCheck（失败抛 LogicError）
		vkDevice.getDevice().writeSamplerDescriptorsEXT(samplerCreateInfo, dst);
		sharedSamplerReady = true;
		return sharedSamplerSlot;
	}
	// Phase 5.5 sky/env sampler：linear mipmap（mipmapMode=LINEAR + maxLod=mipLevels−1），
	// slot = sampler heap reserved + 1（共享 sampler 之后第二槽，capacity reserved+8 足够）。
	// 幂等（ensureSharedSampler 同模式）。不动共享 sampler——模型纹理路径零影响。
	// maxLod 烘焙进描述符 → 调用点须已知环境贴图 mip 级数（env 上传期）。
	[[nodiscard]] uint32_t ensureSkySampler(uint32_t mipLevels) {
		if (skySamplerReady)
			return skySamplerSlot;
		VkSamplerCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
		info.magFilter = VK_FILTER_LINEAR;
		info.minFilter = VK_FILTER_LINEAR;
		info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
		info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
		info.anisotropyEnable = VK_FALSE;
		info.compareEnable = VK_FALSE;
		info.minLod = 0.0f;
		info.maxLod = static_cast<float>(mipLevels - 1);
		info.unnormalizedCoordinates = VK_FALSE;
		info.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
		skySamplerSlot = samplerHeap.getReservedSlots() + 1;
		auto slotAddr = samplerHeap.getSlotCpuAddress(skySamplerSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, samplerHeap.getDescriptorSize()};
		skySamplerCreateInfo = vk::SamplerCreateInfo(info);
		vkDevice.getDevice().writeSamplerDescriptorsEXT(skySamplerCreateInfo, dst);
		skySamplerReady = true;
		return skySamplerSlot;
	}
	// Phase 5.5 环境 cubemap（场景级单例资产，texture_pool::uploadCubeMap 产出入库）：
	// 与材质纹理集（AssetTable::textures）分离——env 绑定场景而非材质，不占纹理 id 空间
	//（白纹理 id 0 / 首槽契约不受影响）。optional 内部存储地址稳定（Resources = main 栈
	// 对象永不 move），描述符指针链自引用在 emplace 之后设置即稳定。
	[[nodiscard]] TextureAsset& emplaceEnvTexture(TextureAsset asset) {
		envAsset.emplace(std::move(asset));
		return *envAsset;
	}
	[[nodiscard]] const TextureAsset* getEnvTexture() const {
		return envAsset ? &*envAsset : nullptr;
	}
	// SceneData buffer（Phase 5 批次 2）：幂等创建设备端 storage buffer（eTransferDst——
	// 每帧内容由 staging 拷入）+ 描述符写 allocSlot 运行时槽（同 slotTexture 模式，
	// 纹理槽数随场景动态、light 槽无编译期偏移）。调用点 = 建模型管线条目前
	//（ShaderRegistry::addModelEntry，同 ensureSharedSampler 时序）。
	// 返回 resource heap 绝对槽号（binding mapping 烘焙用）。
	[[nodiscard]] uint32_t ensureSceneData() {
		if (sceneDataReady)
			return sceneDataSlot;
		vk::BufferCreateInfo info{};
		info.size = kSceneDataBytes;
		info.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
			vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
		VmaAllocationCreateInfo alloc{};
		// device-local：CPU 不直接写（每帧唯一写入方 = staging copy），勿用 createGpuBuffer 的
		// CPU_TO_GPU+MAPPED 形态
		alloc.usage = VMA_MEMORY_USAGE_AUTO;
		auto bufResult = allocator.createBufferWithAddress(vkDevice.getDevice(), info, alloc);
		if (!bufResult)
			throw std::runtime_error(bufResult.error().describe());
		sceneDataBuffer = std::move(*bufResult);

		auto slotResult = allocSlot("scene.data");
		if (!slotResult)
			throw std::runtime_error(slotResult.error().describe());
		sceneDataSlot = *slotResult;

		vk::DeviceAddressRangeEXT range{sceneDataBuffer.getDeviceAddress(), kSceneDataBytes};
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eStorageBuffer;
		desc.data.pAddressRange = &range;
		auto slotAddr = descriptorHeap.getSlotCpuAddress(sceneDataSlot);
		if (!slotAddr)
			throw std::runtime_error(slotAddr.error().describe());
		vk::HostAddressRangeEXT dst{*slotAddr, descriptorHeap.getDescriptorSize()};
		vkDevice.getDevice().writeResourceDescriptorsEXT(
			std::array<vk::ResourceDescriptorInfoEXT, 1>{desc},
			std::array<vk::HostAddressRangeEXT, 1>{dst});
		sceneDataReady = true;
		return sceneDataSlot;
	}
	// scene data buffer（drawFrame 每帧 staging copy 的目标；未创建 = 空句柄，跳过拷贝）
	const RGL::VmaBuffer& getSceneDataBuffer() const { return sceneDataBuffer; }

	// cube 一等资产（批次 3）：资产记录 = cube 顶点/索引 buffer + slot 的唯一 holder
	uint32_t cubeAssetId() const { return cubeAsset; }
	// 资产表（已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用；
	// registry entity 经 MeshRef{asset} 引用，见 AssetTable 注释）
	RGL::AssetTable& getAssetTable() { return assetTable; }
	RGL::VmaAllocatorRAII& getAllocator() { return allocator; }
	// 统一 slot 分配器（Phase 3，纹理资产并案）：从 reservedSlots 起单调分配，
	// name→slot 记日志（slot 表排查用）；调用方以生成的偏移常量校验布局契约（见构造器 / model_importer）。
	// 越界（≥ capacity，含 reserved）→ 错误
	[[nodiscard]] std::expected<uint32_t, Error> allocSlot(const char* name) {
		if (nextSlot >= descriptorHeap.getCapacity())
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				fmt::format("descriptor heap capacity exceeded ({} >= {}): {}",
					nextSlot, descriptorHeap.getCapacity(), name)});
		auto* logger = RLLogger::getInstance();
		const uint32_t slot = nextSlot++;
		if (logger) logger->debug("descriptor heap slot {}: {}", slot, name);
		return slot;
	}

   private:
	uint32_t reservedSlots() const { return descriptorHeap.getReservedSlots(); }

   private:
	static std::expected<RGL::VmaImage, Error> createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent) {
		vk::ImageCreateInfo depthImageInfo{};
		depthImageInfo.imageType = vk::ImageType::e2D;
		depthImageInfo.format = vk::Format::eD32Sfloat;
		depthImageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
		depthImageInfo.mipLevels = 1;
		depthImageInfo.arrayLayers = 1;
		depthImageInfo.samples = vk::SampleCountFlagBits::e1;
		depthImageInfo.tiling = vk::ImageTiling::eOptimal;
		depthImageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;

		VmaAllocationCreateInfo depthAllocInfo{};
		depthAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		depthAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

		return allocator.createImage(depthImageInfo, depthAllocInfo);
	}

	static vk::ImageViewCreateInfo buildDepthView(VkImage image) {
		vk::ImageViewCreateInfo depthViewInfo{};
		depthViewInfo.image = image;
		depthViewInfo.viewType = vk::ImageViewType::e2D;
		depthViewInfo.format = vk::Format::eD32Sfloat;
		depthViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
		depthViewInfo.subresourceRange.levelCount = 1;
		depthViewInfo.subresourceRange.layerCount = 1;
		return depthViewInfo;
	}

	std::expected<RGL::VmaBuffer, Error> createGpuBuffer(vk::DeviceSize size, vk::BufferUsageFlags extraUsage, const void* initData = nullptr) {
		vk::BufferCreateInfo bufferInfo{};
		bufferInfo.size = size;
		bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | extraUsage;
		bufferInfo.sharingMode = vk::SharingMode::eExclusive;

		VmaAllocationCreateInfo allocInfo{};
		allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
		allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

		VmaAllocationInfo allocData;
		auto result = allocator.createBufferWithAddress(vkDevice.getDevice(), bufferInfo, allocInfo, &allocData);
		if (!result) return std::unexpected(result.error());

		auto buf = std::move(*result);

		if (initData && size > 0) {
			std::memcpy(allocData.pMappedData, initData, size);
		}

		return buf;
	}

	// 容量 = reserved + 用户槽位。用户槽位布局（相对 reserved range 起点，allocSlot 连续分配）：
	//   cube 顶点 slot 0、索引 slot 1（types_generated 的 HeapSlotVertexOffset/HeapSlotIndexOffset）
	//   模型顶点 slot 基址 HeapSlotModelVertexBase=2（model_types_generated），每模型一个（拼接顶点 buffer，Phase 4 批次 2）；
	//   纹理 slot 自 2+N 起（texture_pool.cppm，资产记录在 AssetTable）；
	//   剩余裕量 kUserSlotMargin（固定，与场景内容解耦——E1.3）
	std::expected<RGL::VmaDescriptorHeap, Error> createDescriptorHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		// Phase 3 统一步长：image 描述符（SAMPLED_IMAGE）比 buffer 描述符宽
		//（RADV 32B > 16B，docs/heap_texture_research.md Q1），slot 步长取 max（运行时查询，勿硬编码）
		uint32_t bufferSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
		uint32_t imageSize = static_cast<uint32_t>(heapProps.imageDescriptorSize);
		uint32_t descriptorSize = std::max(bufferSize, imageSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize;
		vk::DeviceSize resourceHeapAlignment = heapProps.resourceHeapAlignment;
		uint32_t userSlots = HeapSlotModelVertexBase + kUserSlotMargin;
		uint32_t capacity = userSlots + reservedSlots;
		if (auto* logger = RLLogger::getInstance())
			logger->info("descriptor heap: bufferDesc={}B imageDesc={}B stride={}B reservedSlots={} alignment={}B capacity={}",
				bufferSize, imageSize, descriptorSize, reservedSlots, resourceHeapAlignment, capacity);

		return allocator.createDescriptorHeap(
			vkDevice.getDevice(),
			capacity, descriptorSize,
			vk::BufferUsageFlagBits::eDescriptorHeapEXT,
			reservedSlots,
			resourceHeapAlignment);
	}

	// sampler heap（Phase 3）：OpTypeSampler 的独立 heap（vkCmdBindSamplerHeapEXT 每帧绑定）。
	// 背景：pEmbeddedSampler 对 Slang 的 OpTypeImage+OpTypeSampler 分离对未获 RADV 采纳
	//（spec 声称同等适用，实测采样恒 0；spike 只验证了 GLSL 组合 sampler2D=OpTypeSampledImage），
	// 回退链（docs/phase3_plan.md R1）= 真 sampler heap。容量 = reserved + 固定小裕量
	//（共享一份 sampler 描述符，全模型管线映射指向同一槽）。
	std::expected<RGL::VmaDescriptorHeap, Error> createSamplerHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		uint32_t samplerSize = static_cast<uint32_t>(heapProps.samplerDescriptorSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minSamplerHeapReservedRange + samplerSize - 1) / samplerSize;
		uint32_t capacity = reservedSlots + 8;
		if (auto* logger = RLLogger::getInstance())
			logger->info("sampler heap: samplerDesc={}B reservedSlots={} alignment={}B capacity={}",
				samplerSize, reservedSlots, heapProps.samplerHeapAlignment, capacity);

		return allocator.createDescriptorHeap(
			vkDevice.getDevice(),
			capacity, samplerSize,
			vk::BufferUsageFlagBits::eDescriptorHeapEXT,
			reservedSlots,
			heapProps.samplerHeapAlignment);
	}

	void writeDescriptors() {
		auto& device = vkDevice.getDevice();

		// 构造体内调用（cube buffer 移入资产表前）：直接用成员取地址
		vk::DeviceAddressRangeEXT vertexRange{
			vertexBuffer.getDeviceAddress(),
			RGL::cubeVertices.size() * sizeof(RGL::Vertex)};
		vk::DeviceAddressRangeEXT indexRange{
			indexBuffer.getDeviceAddress(),
			RGL::cubeIndices.size() * sizeof(uint32_t)};

		vk::ResourceDescriptorInfoEXT vertexDescInfo{};
		vertexDescInfo.type = vk::DescriptorType::eStorageBuffer;
		vertexDescInfo.data.pAddressRange = &vertexRange;

		vk::ResourceDescriptorInfoEXT indexDescInfo{};
		indexDescInfo.type = vk::DescriptorType::eStorageBuffer;
		indexDescInfo.data.pAddressRange = &indexRange;

		auto vertexSlotResult = descriptorHeap.getSlotCpuAddress(heapSlotVertex);
		if (!vertexSlotResult) {
			throw std::runtime_error(vertexSlotResult.error().describe());
		}

		auto indexSlotResult = descriptorHeap.getSlotCpuAddress(heapSlotIndex);
		if (!indexSlotResult) {
			throw std::runtime_error(indexSlotResult.error().describe());
		}

		vk::HostAddressRangeEXT vertexDst{
			*vertexSlotResult,
			descriptorHeap.getDescriptorSize()};
		vk::HostAddressRangeEXT indexDst{
			*indexSlotResult,
			descriptorHeap.getDescriptorSize()};

		device.writeResourceDescriptorsEXT(
			std::array<vk::ResourceDescriptorInfoEXT, 2>{vertexDescInfo, indexDescInfo},
			std::array<vk::HostAddressRangeEXT, 2>{vertexDst, indexDst});
}

	RGL::AppContext& vkInstance;
	RGL::DeviceContext& vkDevice;
	RGL::Swapchain& vkSwapchain;
	RGL::VmaAllocatorRAII allocator;
	RGL::VmaImage depthImage;
	vk::raii::ImageView depthImageView;
	// cube 顶点/索引 buffer（批次 3：构造体移入 cube 资产记录后为空，访问经 cubeAssetId）
	RGL::VmaBuffer vertexBuffer;
	RGL::VmaBuffer indexBuffer;
	RGL::VmaDescriptorHeap descriptorHeap;
	RGL::VmaDescriptorHeap samplerHeap;  // Phase 3：OpTypeSampler 独立 heap
	// 共享 sampler 的 create info 稳定存储（ensureSharedSampler 写入；
	// 本机 RADV 写时即解析、spec 允许驱动延迟解释，生命周期须覆盖运行期，见其注释）
	vk::SamplerCreateInfo samplerCreateInfo{};
	uint32_t sharedSamplerSlot = 0;   // 共享 sampler 在 sampler heap 的槽号（ensureSharedSampler 启动期写入）
	bool sharedSamplerReady = false;
	// Phase 5.5：sky/env sampler（第二槽，ensureSkySampler 幂等写入）+ 环境 cubemap 单例
	vk::SamplerCreateInfo skySamplerCreateInfo{};  // create info 稳定存储（同 samplerCreateInfo 规则）
	uint32_t skySamplerSlot = 0;
	bool skySamplerReady = false;
	std::optional<TextureAsset> envAsset;
	uint32_t heapSlotVertex;
	uint32_t heapSlotIndex;
	// SceneData（Phase 5 批次 2）：设备端动态数据 buffer + 运行时槽（ensureSceneData 幂等写入）
	RGL::VmaBuffer sceneDataBuffer;
	uint32_t sceneDataSlot = 0;
	bool sceneDataReady = false;
	// 统一 slot 分配器（Phase 3，纹理资产并案）：单调分配游标，从 reservedSlots 起
	uint32_t nextSlot;
	// cube 一等资产 id（批次 3：构造体写入）
	uint32_t cubeAsset = 0;
	// 资产表（E1.1/E1.2：已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用）
	RGL::AssetTable assetTable;
	// 用户槽位固定裕量（与场景内容解耦；Phase 3 统一步长 = max(buffer, image) descriptorSize，
	// RADV 32B，1024 槽为 KB 级开销）
	static constexpr uint32_t kUserSlotMargin = 1024;
};

}  // namespace RGL
