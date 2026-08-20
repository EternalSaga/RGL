module;
#include <vk_mem_alloc.h>
#include <vulkan/vulkan_raii.hpp>
#include <cstdint>
export module VmaRaii;

import rlstd;
import vkerror;

namespace RGL {

struct AllocatorControlBlock {
	VmaAllocator handle;
};

export using SharedAllocator = std::shared_ptr<AllocatorControlBlock>;

export struct VmaBuffer {
	SharedAllocator allocatorBlock;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	vk::DeviceAddress deviceAddress = 0;
	void* pMappedData = nullptr;

	VmaBuffer() = default;
	VmaBuffer(SharedAllocator allocBlock, VkBuffer buf, VmaAllocation alloctn, vk::DeviceAddress addr = 0, void* mapped = nullptr)
		: allocatorBlock(std::move(allocBlock)), buffer(buf), allocation(alloctn), deviceAddress(addr), pMappedData(mapped) {}

	~VmaBuffer() {
		if (allocatorBlock && allocatorBlock->handle && buffer && allocation)
			vmaDestroyBuffer(allocatorBlock->handle, buffer, allocation);
	}

	VmaBuffer(const VmaBuffer&) = delete;
	VmaBuffer& operator=(const VmaBuffer&) = delete;

	VmaBuffer(VmaBuffer&& o) noexcept
		: allocatorBlock(std::move(o.allocatorBlock)), buffer(o.buffer), allocation(o.allocation), deviceAddress(o.deviceAddress), pMappedData(o.pMappedData) {
		o.buffer = VK_NULL_HANDLE;
		o.allocation = nullptr;
		o.deviceAddress = 0;
		o.pMappedData = nullptr;
	}

	VmaBuffer& operator=(VmaBuffer&& o) noexcept {
		if (this != &o) {
			if (allocatorBlock && allocatorBlock->handle && buffer && allocation)
				vmaDestroyBuffer(allocatorBlock->handle, buffer, allocation);
			allocatorBlock = std::move(o.allocatorBlock);
			buffer = o.buffer;
			allocation = o.allocation;
			deviceAddress = o.deviceAddress;
			pMappedData = o.pMappedData;
			o.buffer = VK_NULL_HANDLE;
			o.allocation = nullptr;
			o.deviceAddress = 0;
			o.pMappedData = nullptr;
		}
		return *this;
	}

	VkBuffer operator*() const { return buffer; }
	vk::DeviceAddress getDeviceAddress() const { return deviceAddress; }
	void* getMappedData() const { return pMappedData; }
};

export struct VmaImage {
	SharedAllocator allocatorBlock;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;

	VmaImage() = default;
	VmaImage(SharedAllocator allocBlock, VkImage img, VmaAllocation alloctn)
		: allocatorBlock(std::move(allocBlock)), image(img), allocation(alloctn) {}

	~VmaImage() {
		if (allocatorBlock && allocatorBlock->handle && image && allocation)
			vmaDestroyImage(allocatorBlock->handle, image, allocation);
	}

	VmaImage(const VmaImage&) = delete;
	VmaImage& operator=(const VmaImage&) = delete;

	VmaImage(VmaImage&& o) noexcept
		: allocatorBlock(std::move(o.allocatorBlock)), image(o.image), allocation(o.allocation) {
		o.image = VK_NULL_HANDLE;
		o.allocation = nullptr;
	}

	VmaImage& operator=(VmaImage&& o) noexcept {
		if (this != &o) {
			if (allocatorBlock && allocatorBlock->handle && image && allocation)
				vmaDestroyImage(allocatorBlock->handle, image, allocation);
			allocatorBlock = std::move(o.allocatorBlock);
			image = o.image;
			allocation = o.allocation;
			o.image = VK_NULL_HANDLE;
			o.allocation = nullptr;
		}
		return *this;
	}
	VkImage operator*() const { return image; }
};

export struct VmaDescriptorHeap {
	VmaBuffer bufferStorage;
	void* pMappedData = nullptr;
	uint64_t gpuAddress = 0;
	vk::DeviceSize gpuOffset = 0;
	uint32_t descriptorSize = 0;
	uint32_t capacity = 0;
	uint32_t reservedSlots = 0;

	VmaDescriptorHeap() = default;
	VmaDescriptorHeap(VmaBuffer&& buf, void* mapped, uint64_t gpuAddr, vk::DeviceSize gOffset, uint32_t descSize, uint32_t cap, uint32_t reserved = 0)
		: bufferStorage(std::move(buf)), pMappedData(mapped), gpuAddress(gpuAddr), gpuOffset(gOffset), descriptorSize(descSize), capacity(cap), reservedSlots(reserved) {}

	~VmaDescriptorHeap() = default;

	VmaDescriptorHeap(const VmaDescriptorHeap&) = delete;
	VmaDescriptorHeap& operator=(const VmaDescriptorHeap&) = delete;

	VmaDescriptorHeap(VmaDescriptorHeap&& o) noexcept
		: bufferStorage(std::move(o.bufferStorage)), pMappedData(o.pMappedData), gpuAddress(o.gpuAddress), gpuOffset(o.gpuOffset), descriptorSize(o.descriptorSize), capacity(o.capacity), reservedSlots(o.reservedSlots) {
		o.pMappedData = nullptr;
		o.gpuAddress = 0;
		o.gpuOffset = 0;
		o.descriptorSize = 0;
		o.capacity = 0;
		o.reservedSlots = 0;
	}

	VmaDescriptorHeap& operator=(VmaDescriptorHeap&& o) noexcept {
		if (this != &o) {
			bufferStorage = std::move(o.bufferStorage);
			pMappedData = o.pMappedData;
			gpuAddress = o.gpuAddress;
			gpuOffset = o.gpuOffset;
			descriptorSize = o.descriptorSize;
			capacity = o.capacity;
			reservedSlots = o.reservedSlots;

			o.pMappedData = nullptr;
			o.gpuAddress = 0;
			o.gpuOffset = 0;
			o.descriptorSize = 0;
			o.capacity = 0;
			o.reservedSlots = 0;
		}
		return *this;
	}

	VkBuffer operator*() const { return *bufferStorage; }
	uint64_t getGpuAddress() const { return gpuAddress; }
	uint32_t getDescriptorSize() const { return descriptorSize; }
	uint32_t getCapacity() const { return capacity; }
	uint32_t getReservedSlots() const { return reservedSlots; }
	vk::DeviceSize getHeapSize() const { return static_cast<vk::DeviceSize>(descriptorSize) * capacity - gpuOffset; }

	[[nodiscard]] std::expected<void*, Error> getSlotCpuAddress(uint32_t slot) const {
		if (slot >= capacity) {
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				"Descriptor heap slot out of bounds!"});
		}
		return static_cast<char*>(pMappedData) + (static_cast<size_t>(slot) * descriptorSize);
	}
};

export class VmaAllocatorRAII {
   private:
	SharedAllocator allocatorBlock;

   public:
	VmaAllocatorRAII(VkInstance instance, VkPhysicalDevice physicalDevice, VkDevice device, uint32_t apiVersion,
		VmaAllocatorCreateFlags flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT) {
		VmaAllocator raw;
		VmaAllocatorCreateInfo allocatorInfo{
			.flags = flags,
			.physicalDevice = physicalDevice,
			.device = device,
			.instance = instance,
			.vulkanApiVersion = apiVersion};
		if (vmaCreateAllocator(&allocatorInfo, &raw) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create VMA Allocator");
		}
		allocatorBlock = SharedAllocator(new AllocatorControlBlock{raw},
			[](AllocatorControlBlock* p) {
				if (p) {
					if (p->handle) vmaDestroyAllocator(p->handle);
					delete p;
				}
			});
	}

	~VmaAllocatorRAII() = default;

	VmaAllocatorRAII(const VmaAllocatorRAII&) = delete;
	VmaAllocatorRAII& operator=(const VmaAllocatorRAII&) = delete;
	VmaAllocatorRAII(VmaAllocatorRAII&& o) noexcept : allocatorBlock(std::move(o.allocatorBlock)) {}

	[[nodiscard]] std::expected<VmaBuffer, Error> createBuffer(const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
		VkBuffer buffer;
		VmaAllocation allocation;
		VkResult result = vmaCreateBuffer(allocatorBlock->handle, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, &buffer, &allocation, pAllocInfo);
		if (result != VK_SUCCESS) {
			return std::unexpected(Error{ErrorCode::VMASuccessFailed,
				"VMA failed to create buffer"});
		}
		return VmaBuffer(allocatorBlock, buffer, allocation);
	}

	[[nodiscard]] std::expected<VmaBuffer, Error> createBufferWithAddress(const vk::raii::Device& device, const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
		VkBuffer buffer;
		VmaAllocation allocation;
		VkResult result = vmaCreateBuffer(allocatorBlock->handle, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, &buffer, &allocation, pAllocInfo);
		if (result != VK_SUCCESS) {
			return std::unexpected(Error{ErrorCode::VMASuccessFailed, "VMA failed to create buffer"});
		}
		vk::BufferDeviceAddressInfo addrInfo{};
		addrInfo.buffer = buffer;
		vk::DeviceAddress addr = device.getBufferAddress(addrInfo);
		return VmaBuffer(allocatorBlock, buffer, allocation, addr, pAllocInfo ? pAllocInfo->pMappedData : nullptr);
	}

	[[nodiscard]] std::expected<VmaImage, Error> createImage(const vk::ImageCreateInfo& imageInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo = nullptr) const {
		VkImage image;
		VmaAllocation allocation;
		VkResult result = vmaCreateImage(allocatorBlock->handle, reinterpret_cast<const VkImageCreateInfo*>(&imageInfo), &allocInfo, &image, &allocation, pAllocInfo);
		if (result != VK_SUCCESS) {
			return std::unexpected(Error{ErrorCode::VMASuccessFailed, "VMA failed to create image"});
		}
		return VmaImage(allocatorBlock, image, allocation);
	}

	[[nodiscard]] std::expected<VmaDescriptorHeap, Error> createDescriptorHeap(
		const vk::raii::Device& device,
		uint32_t capacity,
		uint32_t descriptorSize,
		vk::BufferUsageFlags extraUsageFlags,
		uint32_t reservedSlots,
		vk::DeviceSize resourceHeapAlignment) const {
		vk::BufferCreateInfo bufferInfo{};
		bufferInfo.size = static_cast<vk::DeviceSize>(descriptorSize) * capacity;
		bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | extraUsageFlags;

		VmaAllocationCreateInfo allocCreateInfo{
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = VMA_MEMORY_USAGE_AUTO};

		VmaAllocationInfo allocInfo;
		auto storageResult = createBufferWithAddress(device, bufferInfo, allocCreateInfo, &allocInfo);
		if (!storageResult) return std::unexpected(storageResult.error());

		vk::DeviceAddress rawAddr = storageResult->getDeviceAddress();
		vk::DeviceSize rawMappedAddr = static_cast<vk::DeviceSize>(reinterpret_cast<uintptr_t>(allocInfo.pMappedData));
		vk::DeviceSize gpuOffset = (resourceHeapAlignment - (rawAddr % resourceHeapAlignment)) % resourceHeapAlignment;

		// 确保 CPU mapped data 也是 descriptorSize 对齐的
		vk::DeviceSize mappedMisalign = (rawMappedAddr + gpuOffset) % descriptorSize;
		vk::DeviceSize cpuCompensation = mappedMisalign ? (descriptorSize - mappedMisalign) : 0;
		vk::DeviceSize offset = gpuOffset + cpuCompensation;
		// cpuCompensation 可能破坏了 GPU 对齐，重新修正
		vk::DeviceSize gpuMisalign = (rawAddr + offset) % resourceHeapAlignment;
		if (gpuMisalign) {
			offset += resourceHeapAlignment - gpuMisalign;
			// 重新检查 CPU 对齐（resourceHeapAlignment 是 descriptorSize 的倍数，应自动满足）
		}

		return VmaDescriptorHeap(
			std::move(*storageResult),
			static_cast<char*>(allocInfo.pMappedData) + offset,
			rawAddr + offset,
			offset,
			descriptorSize,
			capacity,
			reservedSlots);
	}

	VmaAllocator get() const { return allocatorBlock ? allocatorBlock->handle : nullptr; }
};

}  // namespace RGL