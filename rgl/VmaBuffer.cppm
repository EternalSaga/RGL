module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <cstdint>
export module VmaRaii;

import rlstd;
import rlvk;
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

// Descriptor heap 封装：把带 eDescriptorHeapEXT usage 的 BDA buffer 视为连续的"描述符槽位数组"。
// 与普通 buffer（VmaBuffer）的本质区别在"对齐记账"：
//   1. 传给 vkCmdBindResourceHeapEXT 的范围起始地址必须按设备的 resourceHeapAlignment
//      对齐（NVIDIA 32B / AMD 4B），VMA 原始分配地址不保证满足且不能重新分配，
//      只能"从分配内部偏移进去"，因此 heap 是 buffer 内带自己 base/size 的子范围；
//   2. CPU mapped 指针需独立偏移，且槽位寻址要求它按 descriptorSize 对齐；
//   3. heap 存在厂商特定的 reserved 槽位：字节 [0, reservedSlots * descriptorSize)
//      由驱动管理（NVIDIA 6048 槽 / AMD 0 槽），不可使用。
// 对齐的计算细节见下方 VmaAllocatorRAII::createDescriptorHeap。
export struct VmaDescriptorHeap {
	VmaBuffer bufferStorage;       // 底层 buffer（RAII：随 SharedAllocator 控制块由 VMA 统一销毁）
	void* pMappedData = nullptr;   // 偏移后的 CPU 地址（descriptorSize 对齐），slot 0 入口，写描述符用
	uint64_t gpuAddress = 0;       // 偏移后的 GPU 设备地址（resourceHeapAlignment 对齐），vkCmdBindResourceHeapEXT 用
	vk::DeviceSize gpuOffset = 0;  // heap 范围起点相对 buffer 原始起点的偏移（GPU/CPU 指针均加此偏移）
	uint32_t descriptorSize = 0;   // 单个 buffer 描述符槽位大小，查询自 bufferDescriptorSize（两台 GPU 实测 16B，勿硬编码）
	uint32_t capacity = 0;         // heap 槽位总数（含 reserved），buffer 分配大小 = capacity * descriptorSize
	uint32_t reservedSlots = 0;    // 驱动保留槽位数，用户可用槽位为 [reservedSlots, capacity)

	VmaDescriptorHeap() = default;
	// 唯一的非默认构造入口：由 VmaAllocatorRAII::createDescriptorHeap 产出，
	// 保证不变式：gpuAddress 按 resourceHeapAlignment 对齐、pMappedData 按 descriptorSize 对齐，
	// 且两者指向同一个 "slot 0"。
	VmaDescriptorHeap(VmaBuffer&& buf, void* mapped, uint64_t gpuAddr, vk::DeviceSize gOffset, uint32_t descSize, uint32_t cap, uint32_t reserved = 0)
		: bufferStorage(std::move(buf)), pMappedData(mapped), gpuAddress(gpuAddr), gpuOffset(gOffset), descriptorSize(descSize), capacity(cap), reservedSlots(reserved) {}

	~VmaDescriptorHeap() = default;

	VmaDescriptorHeap(const VmaDescriptorHeap&) = delete;
	VmaDescriptorHeap& operator=(const VmaDescriptorHeap&) = delete;

	// 移动语义（RAII）：移动后源对象被重置为已销毁状态，防止 buffer 被二次销毁
	VmaDescriptorHeap(VmaDescriptorHeap&& o) noexcept
		: bufferStorage(std::move(o.bufferStorage)), pMappedData(o.pMappedData), gpuAddress(o.gpuAddress), gpuOffset(o.gpuOffset), descriptorSize(o.descriptorSize), capacity(o.capacity), reservedSlots(o.reservedSlots) {
		o.pMappedData = nullptr;
		o.gpuAddress = 0;
		o.gpuOffset = 0;
		o.descriptorSize = 0;
		o.capacity = 0;
		o.reservedSlots = 0;
	}

	// 移动赋值：先销毁本对象持有的旧 buffer，再接管源对象的资源
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
	uint64_t getGpuAddress() const { return gpuAddress; }       // heap 范围起点的 GPU 地址
	uint32_t getDescriptorSize() const { return descriptorSize; }
	uint32_t getCapacity() const { return capacity; }
	uint32_t getReservedSlots() const { return reservedSlots; }
	// heap 范围可用大小（字节）= 槽位总字节数 - 范围起点偏移。
	// 必须用它作为 vkCmdBindResourceHeapEXT 的 size，而不是 descriptorSize * capacity：
	// 曾漏扣偏移导致验证层报错 + device lost（见 docs/nvidia_compatibility_report.md）
	vk::DeviceSize getHeapSize() const { return static_cast<vk::DeviceSize>(descriptorSize) * capacity - gpuOffset; }

	// 指定槽位的 CPU 地址（写描述符用），带越界检查。
	// 调用方 slot = Resources::allocSlot 单调分配（reservedSlots() + 游标；几何段基址常量
	// HeapSlotGeometryBase 由 model_types_generated 生成）
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
		VkResult result = vmaCreateBuffer(allocatorBlock->handle,
			reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo,
			&buffer, &allocation, pAllocInfo);
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

	// 创建 descriptor heap：分配带 eDescriptorHeapEXT usage 的 BDA buffer 并修正双端对齐。
	//
	// 对齐约束（两条，互相独立）：
	//   GPU 端：传给 vkCmdBindResourceHeapEXT 的 heap 范围起始地址必须按
	//           resourceHeapAlignment 对齐（NVIDIA 32B / AMD 4B）；
	//   CPU 端：槽位寻址用的 mapped 指针必须按 descriptorSize 对齐。
	// VMA 原始分配地址两者都不保证，且无法重新分配，因此策略是"从分配内部偏移"：
	// 分配大小保持 descriptorSize * capacity 不变，把 heap 范围起点推进 buffer 内部
	// offset 字节，尾部恰好落在 buffer 末尾（可用大小见 getHeapSize）。
	[[nodiscard]] std::expected<VmaDescriptorHeap, Error> createDescriptorHeap(
		const vk::raii::Device& device,
		uint32_t capacity,
		uint32_t descriptorSize,
		vk::BufferUsageFlags extraUsageFlags,
		uint32_t reservedSlots,
		vk::DeviceSize resourceHeapAlignment) const {
		vk::BufferCreateInfo bufferInfo{};
		bufferInfo.size = static_cast<vk::DeviceSize>(descriptorSize) * capacity;  // 总分配 = 槽位数 × 槽大小，不超额分配
		// 两个 usage flag 必须同时设置：缺 eShaderDeviceAddress 或 eDescriptorHeapEXT
		// 任一个都是 undefined behavior（spec 3.3）
		bufferInfo.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress | extraUsageFlags;

		// 主机顺序写 + 常驻映射：描述符由 CPU 端经 vkWriteResourceDescriptorsEXT 写入
		VmaAllocationCreateInfo allocCreateInfo{
			.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
			.usage = VMA_MEMORY_USAGE_AUTO};

		VmaAllocationInfo allocInfo;
		auto storageResult = createBufferWithAddress(device, bufferInfo, allocCreateInfo, &allocInfo);
		if (!storageResult) return std::unexpected(storageResult.error());

		vk::DeviceAddress rawAddr = storageResult->getDeviceAddress();
		vk::DeviceSize rawMappedAddr = static_cast<vk::DeviceSize>(reinterpret_cast<uintptr_t>(allocInfo.pMappedData));
		// 第一步（GPU 对齐）：推进到下一个 resourceHeapAlignment 边界所需的偏移量；
		// 外层 % alignment 处理已经对齐的情况（偏移为 0）
		vk::DeviceSize gpuOffset = (resourceHeapAlignment - (rawAddr % resourceHeapAlignment)) % resourceHeapAlignment;

		// 第二步（CPU 对齐）：加上 gpuOffset 后 CPU 指针可能不再按 descriptorSize 对齐，
		// 再额外推进（cpuCompensation）
		vk::DeviceSize mappedMisalign = (rawMappedAddr + gpuOffset) % descriptorSize;
		vk::DeviceSize cpuCompensation = mappedMisalign ? (descriptorSize - mappedMisalign) : 0;
		vk::DeviceSize offset = gpuOffset + cpuCompensation;
		// 第三步（GPU 再修正）：CPU 侧的推进可能破坏 GPU 对齐，再推进到下一个
		// resourceHeapAlignment 边界。（假设 resourceHeapAlignment 是 descriptorSize
		// 的倍数，则此时 CPU 对齐自动保持，无需再检查）
		vk::DeviceSize gpuMisalign = (rawAddr + offset) % resourceHeapAlignment;
		if (gpuMisalign) {
			offset += resourceHeapAlignment - gpuMisalign;
		}

		// 偏移后 CPU/GPU 指针指向同一个 "slot 0" 位置；
		// heap 可用大小由 getHeapSize() 按 descriptorSize * capacity - offset 计算
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