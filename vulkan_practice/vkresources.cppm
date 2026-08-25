module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <vk_mem_alloc.h>
export module vk_resources;

import rlstd;
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

// 资产层：每个已上传 primitive 一条记录（GPU 顶点 buffer + heap slot + 三角形数 + 管线条目引用），
// 以稳定 id（vector 下标，上传顺序分配）索引。registry entity 经 MeshRef{asset}（scene.cppm）
// 引用资产，五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为本表
// 一处映射（E1.1）。生命周期：Resources 成员（与 descriptor heap / cube buffer 同层），
// buffer 随 SharedAllocator 控制块存活，不再靠 main.cpp 局部变量的隐式 RAII 保证（E1.2/L6）。
export struct MeshAsset {
	VmaBuffer vertexBuffer;
	uint32_t heapSlot = 0;                            // 绝对 heap slot（= reservedSlots() + 偏移，见 uploadModelMeshes）
	uint32_t triangleCount = 0;                       // primitive 三角形数（group 数派生用）
	uint32_t pipelineIndex = kInvalidPipelineIndex;   // 管线表下标（Renderer::addModelPipeline 写入）
};

export class AssetTable {
   public:
	// 注册资产（上传顺序调用）→ 资产 id
	uint32_t addMesh(VmaBuffer vertexBuffer, uint32_t heapSlot, uint32_t triangleCount) {
		meshes.push_back(MeshAsset{std::move(vertexBuffer), heapSlot, triangleCount, kInvalidPipelineIndex});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	[[nodiscard]] const MeshAsset& get(uint32_t id) const { return meshes[id]; }
	// 管线条目引用（Renderer::addModelPipeline 在创建管线条目后写回）
	void setPipelineIndex(uint32_t id, uint32_t pipelineIndex) { meshes[id].pipelineIndex = pipelineIndex; }
	[[nodiscard]] uint32_t size() const { return static_cast<uint32_t>(meshes.size()); }

   private:
	std::vector<MeshAsset> meshes;
};

export class Resources {
   public:
	// heap 容量与场景内容解耦（E1.3，批次 3）：固定裕量 kUserSlotMargin，不再按
	// primitive 数精确预分配（原 modelPrimitiveCount 构造参数删除）。用户槽位布局
	//（相对 reserved range 起点）：
	//   [0, 8)   cube（顶点 slot 0 / 索引 slot 4，types_generated）
	//   [8, 8+N) 模型顶点 slot（每 primitive 一个，slot = reserved + HeapSlotModelVertexBase + i，
	//   见 model_importer.cppm），其余为裕量；运行时扩容 = 重建 heap + 重写全部描述符，
	//   归 Phase 3 TexturePool 一并设计（统一 slot 分配器）
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
		  heapSlotVertex(reservedSlots() + HeapSlotVertexOffset),
		  heapSlotIndex(reservedSlots() + HeapSlotIndexOffset) {
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
	const RGL::VmaAllocatorRAII& getAllocator() const { return allocator; }
	vk::DeviceSize getReservedRangeOffset() const { return 0; }
	vk::DeviceSize getReservedRangeSize() const { return descriptorHeap.getReservedSlots() * descriptorHeap.getDescriptorSize(); }
	vk::DeviceAddress getVertexBufferAddress() const { return vertexBuffer.getDeviceAddress(); }
	vk::DeviceAddress getIndexBufferAddress() const { return indexBuffer.getDeviceAddress(); }
	uint32_t getHeapSlotVertex() const { return heapSlotVertex; }
	uint32_t getHeapSlotIndex() const { return heapSlotIndex; }
	// 资产表（已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用；
	// registry entity 经 MeshRef{asset} 引用，见 AssetTable 注释）
	RGL::AssetTable& getAssetTable() { return assetTable; }
	RGL::VmaAllocatorRAII& getAllocator() { return allocator; }

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

	// 容量 = reserved + 用户槽位。用户槽位布局（相对 reserved range 起点）：
	//   cube 顶点 slot 0、索引 slot 4（types_generated 的 HeapSlotVertexOffset/HeapSlotIndexOffset）
	//   模型顶点 slot 基址 HeapSlotModelVertexBase=8（model_types_generated），每 primitive 一个；
	//   剩余裕量 kUserSlotMargin（固定，与场景内容解耦——E1.3）
	std::expected<RGL::VmaDescriptorHeap, Error> createDescriptorHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		uint32_t descriptorSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize;
		vk::DeviceSize resourceHeapAlignment = heapProps.resourceHeapAlignment;
		uint32_t userSlots = HeapSlotModelVertexBase + kUserSlotMargin;
		uint32_t capacity = userSlots + reservedSlots;

		return allocator.createDescriptorHeap(
			vkDevice.getDevice(),
			capacity, descriptorSize,
			vk::BufferUsageFlagBits::eDescriptorHeapEXT,
			reservedSlots,
			resourceHeapAlignment);
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
	uint32_t heapSlotVertex;
	uint32_t heapSlotIndex;
	// 资产表（E1.1/E1.2：已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用）
	RGL::AssetTable assetTable;
	// 用户槽位固定裕量（与场景内容解耦；descriptorSize 通常 16-64B，1024 槽为 KB 级开销）
	static constexpr uint32_t kUserSlotMargin = 1024;
};

}  // namespace RGL
