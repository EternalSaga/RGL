module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
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

// 管线表下标的"未设置"值（资产记录的 pipelineIndex 初始值；Renderer::addModelPipeline 写入真实下标）
export inline constexpr uint32_t kInvalidPipelineIndex = std::numeric_limits<uint32_t>::max();

// 纹理资产 id 的"未设置"值（同 kInvalidPipelineIndex 模式；批次 2 材质资产的 textureId 用它 = 无纹理 → 白）
export inline constexpr uint32_t kInvalidTextureId = std::numeric_limits<uint32_t>::max();

// 资产种类（批次 1 引入：白纹理 slot 契约断言只数模型顶点槽；批次 3 cube 实体化用它做
// renderListSystem 数据分支——push constant 布局随 kind（Cube=DrawArgs 64B / Model=ModelDrawArgs 96B），
// 完整归一待 Phase 4 ShaderRegistry）
export enum class AssetKind : uint8_t {
	Model,
	Cube,
};

// 资产层：每个已上传 primitive 一条记录（GPU 顶点 buffer + heap slot + 三角形数 + 管线条目引用），
// 以稳定 id（vector 下标，上传顺序分配）索引。registry entity 经 MeshRef{asset}（scene.cppm）
// 引用资产，五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为本表
// 一处映射（E1.1）。生命周期：Resources 成员（与 descriptor heap / cube buffer 同层），
// buffer 随 SharedAllocator 控制块存活，不再靠 main.cpp 局部变量的隐式 RAII 保证（E1.2/L6）。
export struct MeshAsset {
	AssetKind kind = AssetKind::Model;
	VmaBuffer vertexBuffer;
	uint32_t heapSlot = 0;                            // 绝对 heap slot（= reservedSlots() + 偏移，见 uploadModelMeshes）
	uint32_t triangleCount = 0;                       // primitive 三角形数（group 数派生用）
	uint32_t pipelineIndex = kInvalidPipelineIndex;   // 管线表下标（Renderer::addModelPipeline 写入）
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
	// 描述符指针链（自/兄弟引用，入 deque 后设置——元素地址稳定保证指针有效）
	vk::ImageDescriptorInfoEXT imageDesc{};        // pView = &viewCreate，layout = eShaderReadOnlyOptimal
	vk::ResourceDescriptorInfoEXT resourceDesc{};  // type = eSampledImage，data.pImage = &imageDesc
	vk::HostAddressRangeEXT dst{};                 // 描述符写入目标 slot 范围
};

export class AssetTable {
   public:
	// mesh（模型 primitive：去索引顶点流，无索引 buffer）
	// 注册资产（上传顺序调用）→ 资产 id
	uint32_t addMesh(VmaBuffer vertexBuffer, uint32_t heapSlot, uint32_t triangleCount) {
		meshes.push_back(
			MeshAsset{AssetKind::Model, std::move(vertexBuffer), heapSlot, triangleCount, kInvalidPipelineIndex});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	[[nodiscard]] const MeshAsset& get(uint32_t id) const { return meshes[id]; }
	// 管线条目引用（Renderer::addModelPipeline 在创建管线条目后写回）
	void setPipelineIndex(uint32_t id, uint32_t pipelineIndex) { meshes[id].pipelineIndex = pipelineIndex; }
	[[nodiscard]] uint32_t meshSize() const { return static_cast<uint32_t>(meshes.size()); }
	// 模型顶点槽计数（白纹理 slot 契约断言用："纹理首槽 = 白 = 模型顶点槽之后"；
	// 批次 3 cube 资产与本容器共用但不占模型顶点槽，按 kind 计数）
	[[nodiscard]] uint32_t modelMeshCount() const {
		uint32_t n = 0;
		for (const auto& m : meshes)
			if (m.kind == AssetKind::Model) ++n;
		return n;
	}

	// 纹理（deque：元素地址稳定 = 描述符指针链的稳定存储介质，见 TextureAsset 注释）
	// 放置资产记录（deque）→ 调用方在返回引用上设置自/兄弟引用后写描述符 → 资产 id
	TextureAsset& emplaceTexture(TextureAsset asset) {
		textures.push_back(std::move(asset));
		return textures.back();
	}
	[[nodiscard]] const TextureAsset& getTexture(uint32_t id) const { return textures[id]; }
	[[nodiscard]] uint32_t textureSize() const { return static_cast<uint32_t>(textures.size()); }

   private:
	std::vector<MeshAsset> meshes;
	std::deque<TextureAsset> textures;
};

export class Resources {
   public:
	// heap 容量与场景内容解耦（E1.3，批次 3）：固定裕量 kUserSlotMargin，不再按
	// primitive 数精确预分配（原 modelPrimitiveCount 构造参数删除）。用户槽位布局
	//（相对 reserved range 起点，Phase 3 统一 slot 分配器 allocSlot 连续分配）：
	//   [0, 2)    cube（顶点 slot 0 / 索引 slot 1，types_generated）
	//   [2, 2+N)  模型顶点 slot（每 primitive 一个，slot = reserved + HeapSlotModelVertexBase + i，
	//   见 model_importer.cppm）
	//   [2+N, …)  纹理 slot（texture_pool.cppm uploadModelTextures：2+N = 白 1×1 特例（契约断言），
	//   其余按上传序，资产记录在 AssetTable，见 texture_pool.cppm）
	//   其余为裕量；运行时扩容 = 重建 heap + 重写全部描述符
	Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain)
		: vkInstance(vkInstance),
		  vkDevice(vkDevice),
		  vkSwapchain(vkSwapchain),
		  allocator(*vkInstance.getInstance(), *vkDevice.getPhysicalDevice(), *vkDevice.getDevice(),
			  vkInstance.getApiVersion()),
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
	vk::DeviceAddress getVertexBufferAddress() const { return vertexBuffer.getDeviceAddress(); }
	vk::DeviceAddress getIndexBufferAddress() const { return indexBuffer.getDeviceAddress(); }
	uint32_t getHeapSlotVertex() const { return heapSlotVertex; }
	uint32_t getHeapSlotIndex() const { return heapSlotIndex; }
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
				"descriptor heap capacity exceeded (" + std::to_string(nextSlot) + " >= " +
				std::to_string(descriptorHeap.getCapacity()) + "): " + name});
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
	//   模型顶点 slot 基址 HeapSlotModelVertexBase=2（model_types_generated），每 primitive 一个；
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

		vk::DeviceAddressRangeEXT vertexRange{
			getVertexBufferAddress(),
			RGL::cubeVertices.size() * sizeof(RGL::Vertex)};
		vk::DeviceAddressRangeEXT indexRange{
			getIndexBufferAddress(),
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
	RGL::VmaBuffer vertexBuffer;
	RGL::VmaBuffer indexBuffer;
	RGL::VmaDescriptorHeap descriptorHeap;
	RGL::VmaDescriptorHeap samplerHeap;  // Phase 3：OpTypeSampler 独立 heap
	// 共享 sampler 的 create info 稳定存储（ensureSharedSampler 写入；
	// 本机 RADV 写时即解析、spec 允许驱动延迟解释，生命周期须覆盖运行期，见其注释）
	vk::SamplerCreateInfo samplerCreateInfo{};
	uint32_t sharedSamplerSlot = 0;   // 共享 sampler 在 sampler heap 的槽号（ensureSharedSampler 启动期写入）
	bool sharedSamplerReady = false;
	uint32_t heapSlotVertex;
	uint32_t heapSlotIndex;
	// 统一 slot 分配器（Phase 3，纹理资产并案）：单调分配游标，从 reservedSlots 起
	uint32_t nextSlot;
	// 资产表（E1.1/E1.2：已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用）
	RGL::AssetTable assetTable;
	// 用户槽位固定裕量（与场景内容解耦；Phase 3 统一步长 = max(buffer, image) descriptorSize，
	// RADV 32B，1024 槽为 KB 级开销）
	static constexpr uint32_t kUserSlotMargin = 1024;
};

}  // namespace RGL
