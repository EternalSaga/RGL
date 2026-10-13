module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
export module VmaRaii;

import rlstd;
import rlvk;
import vkerror;

namespace RGL {

/**
 * 逐分配归因台账的一条记录（债-31）：一条 = 一个**当前存活**的 VMA 分配。
 *
 * `name` 指向分配点传入的**静态字面量**（台账只存指针，不拷贝、不拥有）——这就是"不得引入
 * 运行期字符串分配开销"这条约束的落点：分配点写字符串字面量，登记面零分配零格式化。
 * 与 VMA 本体里的名字（`vmaSetAllocationName`，VMA 内部会拷一份）是两条独立通道：前者给
 * 外部工具（RenderDoc / vmaBuildStatsString）看，后者给本项目的 `logMemoryStats` 逐模块
 * 点名用，两边同源同一个字面量。
 */
export struct AllocRecord {
	VmaAllocation allocation = nullptr;  ///< VMA 分配句柄（销账键；移动 VmaBuffer 不改它）
	const char* name = nullptr;          ///< 分配点挂号名（静态字面量；nullptr = 未挂号）
	VkDeviceSize size = 0;               ///< 分配字节数（与 VmaTotalStatistics::allocationBytes 同口径）
	uint32_t memoryType = 0xFFFFFFFF;    ///< VMA 内存类型号（heap / property 由它查）
};

/**
 * VMA 分配器控制块（shared_ptr 载荷）：持有裸 VmaAllocator 句柄，
 * 最后一个 SharedAllocator 引用消失时经 deleter 调 vmaDestroyAllocator。
 *
 * 顺带承载**逐分配归因台账**（债-31）：住在这里而不是全局，是为了让生命周期天然对齐——
 * VmaBuffer/VmaImage 都持本控制块的 shared_ptr，销毁时控制块必然还活着（不会拿到已析构
 * 的全局 vector），而台账语义恰好是"与该分配器同生共死的存活分配"。
 *
 * 🔴 台账**无锁**：登记/销账只发生在主线程（本项目不做多线程录制，见 AGENTS「渲染图边界」；
 * 全项目唯一线程 = debug_rpc，它不创建也不销毁任何 VMA 资源）。将来若引入多线程上传，
 * 这两个 vector 与 VMA 自身的线程安全性要一起重新裁决。
 */
struct AllocatorControlBlock {
	VmaAllocator handle;
	std::vector<AllocRecord> liveAllocs;  // 存活分配台账（建/销随分配同生命周期）
};

/** 共享分配器控制块（同进程内 VmaBuffer/VmaImage 共享一份；分配器随最后一个引用销毁）。 */
export using SharedAllocator = std::shared_ptr<AllocatorControlBlock>;

/**
 * 登记一条存活分配（三个 create* 出口调用）：读回 VMA 的 size / memoryType 落台账，并把
 * 名字写进 VMA 本体。
 *
 * 🔴 名字走 `vmaSetAllocationName` 而**不是** `VmaAllocationCreateInfo::pName`：本机 VMA
 * = 3.4.0（vcpkg `GPUOpen::VulkanMemoryAllocator`，头 = build/vcpkg_installed/.../
 * vk_mem_alloc.h，`VmaAllocationCreateInfo` 实测字段只有 flags/usage/requiredFlags/
 * preferredFlags/memoryTypeBits/pool/pUserData/priority/minAlignment，**没有 pName**；
 * pName 只在**输出**结构 `VmaAllocationInfo` 里，写入面只有 vmaSetAllocationName 这一条）。
 * 也**不需要**开 VMA_STATISTICS / 任何 debug 宏：`VMA_STATS_STRING_ENABLED` 默认 1，
 * 而本台账压根不读 vmaBuildStatsString（整棵 JSON 树进日志既读不动也没法回归，见
 * Resources::logMemoryStats 注释）。
 * 唯一成本 = VMA 内部对字面量拷一份**主机**字符串（走分配器的 VkAllocationCallbacks，
 * 本机未设 ⟹ 主机 new），发生在分配时（启动 / resize），**不进设备内存、不进每帧热路径**，
 * 也不计入 vmaCalculateStatistics 的显存口径（实测两级读数逐字节不变）。
 * @param block 分配器控制块
 * @param allocation 刚建好的分配句柄
 * @param name 分配点挂号名（静态字面量；nullptr = 未挂号，台账里点名成"未挂号"行）
 */
inline void registerAllocation(const SharedAllocator& block, VmaAllocation allocation,
	const char* name) {
	if (!block || !block->handle || !allocation)
		return;
	VmaAllocationInfo info{};
	vmaGetAllocationInfo(block->handle, allocation, &info);
	block->liveAllocs.push_back(AllocRecord{allocation, name, info.size, info.memoryType});
	if (name)
		vmaSetAllocationName(block->handle, allocation, name);
}

/**
 * 销账（VmaBuffer / VmaImage 的销毁路径调用）：按句柄线性查删。
 * O(存活数) 但只在分配销毁时跑（启动 / resize / 首帧懒构造），不在每帧热路径；swap-remove
 * 保序无意义（台账消费方自己按名字聚合排序）。
 * @param block 分配器控制块
 * @param allocation 要注销的分配句柄
 */
inline void unregisterAllocation(const SharedAllocator& block, VmaAllocation allocation) {
	if (!block || !allocation)
		return;
	const auto hit = std::ranges::find(block->liveAllocs, allocation, &AllocRecord::allocation);
	if (hit == block->liveAllocs.end())
		return;  // 未登记过（理论不可达：三个 create* 出口全登记）——**不得**误删他人记录
	*hit = std::move(block->liveAllocs.back());
	block->liveAllocs.pop_back();
}

/**
 * VMA 托管 buffer 的 RAII 包装：持有 (buffer, allocation) 对，析构时
 * vmaDestroyBuffer 一并销毁；move-only（拷贝删除）。deviceAddress =
 * vkGetBufferDeviceAddress 结果（BDA 用途，0 = 未查询）；pMappedData =
 * 主机映射指针（nullptr = 未映射）。
 */
export struct VmaBuffer {
	SharedAllocator allocatorBlock;
	VkBuffer buffer = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;
	vk::DeviceAddress deviceAddress = 0;
	void* pMappedData = nullptr;

	/** 默认构造：空状态（无 buffer，析构不做事）。 */
	VmaBuffer() = default;
	/**
	 * 从既有 VMA (buffer, allocation) 对构造。
	 * @param allocBlock 分配器控制块（共享，须比本对象长寿由 shared_ptr 保证）
	 * @param buf Vulkan buffer 句柄
	 * @param alloctn VMA 分配句柄
	 * @param addr BDA 设备地址（默认 0 = 未查询）
	 * @param mapped 主机映射指针（默认 nullptr = 未映射）
	 */
	VmaBuffer(SharedAllocator allocBlock, VkBuffer buf, VmaAllocation alloctn, vk::DeviceAddress addr = 0, void* mapped = nullptr)
		: allocatorBlock(std::move(allocBlock)), buffer(buf), allocation(alloctn), deviceAddress(addr), pMappedData(mapped) {}

	/** 析构：buffer/allocation 均有效时 vmaDestroyBuffer（经控制块的分配器句柄）+ 销账。 */
	~VmaBuffer() {
		if (allocatorBlock && allocatorBlock->handle && buffer && allocation) {
			unregisterAllocation(allocatorBlock, allocation);
			vmaDestroyBuffer(allocatorBlock->handle, buffer, allocation);
		}
	}

	/** 拷贝删除：GPU 资源所有权唯一（move-only）。 */
	VmaBuffer(const VmaBuffer&) = delete;
	/** 拷贝赋值删除：同上。 */
	VmaBuffer& operator=(const VmaBuffer&) = delete;

	/** 移动构造：接管资源，源对象重置为空状态（防二次销毁）。 */
	VmaBuffer(VmaBuffer&& o) noexcept
		: allocatorBlock(std::move(o.allocatorBlock)), buffer(o.buffer), allocation(o.allocation), deviceAddress(o.deviceAddress), pMappedData(o.pMappedData) {
		o.buffer = VK_NULL_HANDLE;
		o.allocation = nullptr;
		o.deviceAddress = 0;
		o.pMappedData = nullptr;
	}

	/** 移动赋值：先销毁本对象持有的旧 buffer（一并销账），再接管源对象资源并清空源。 */
	VmaBuffer& operator=(VmaBuffer&& o) noexcept {
		if (this != &o) {
			if (allocatorBlock && allocatorBlock->handle && buffer && allocation) {
				unregisterAllocation(allocatorBlock, allocation);
				vmaDestroyBuffer(allocatorBlock->handle, buffer, allocation);
			}
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

	/** 解引用：VkBuffer 句柄。 */
	VkBuffer operator*() const { return buffer; }
	/** BDA 设备地址。 */
	vk::DeviceAddress getDeviceAddress() const { return deviceAddress; }
	/** 主机映射指针（可能为 nullptr）。 */
	void* getMappedData() const { return pMappedData; }
};

/**
 * VMA 托管 image 的 RAII 包装：持有 (image, allocation) 对，析构时
 * vmaDestroyImage 一并销毁；move-only（拷贝删除）。
 */
export struct VmaImage {
	SharedAllocator allocatorBlock;
	VkImage image = VK_NULL_HANDLE;
	VmaAllocation allocation = nullptr;

	/** 默认构造：空状态（无 image，析构不做事）。 */
	VmaImage() = default;
	/**
	 * 从既有 VMA (image, allocation) 对构造。
	 * @param allocBlock 分配器控制块（共享）
	 * @param img Vulkan image 句柄
	 * @param alloctn VMA 分配句柄
	 */
	VmaImage(SharedAllocator allocBlock, VkImage img, VmaAllocation alloctn)
		: allocatorBlock(std::move(allocBlock)), image(img), allocation(alloctn) {}

	/** 析构：image/allocation 均有效时 vmaDestroyImage（经控制块的分配器句柄）+ 销账。 */
	~VmaImage() {
		if (allocatorBlock && allocatorBlock->handle && image && allocation) {
			unregisterAllocation(allocatorBlock, allocation);
			vmaDestroyImage(allocatorBlock->handle, image, allocation);
		}
	}

	/** 拷贝删除：GPU 资源所有权唯一（move-only）。 */
	VmaImage(const VmaImage&) = delete;
	/** 拷贝赋值删除：同上。 */
	VmaImage& operator=(const VmaImage&) = delete;

	/** 移动构造：接管资源，源对象重置为空状态（防二次销毁）。 */
	VmaImage(VmaImage&& o) noexcept
		: allocatorBlock(std::move(o.allocatorBlock)), image(o.image), allocation(o.allocation) {
		o.image = VK_NULL_HANDLE;
		o.allocation = nullptr;
	}

	/** 移动赋值：先销毁本对象持有的旧 image（一并销账），再接管源对象资源并清空源。 */
	VmaImage& operator=(VmaImage&& o) noexcept {
		if (this != &o) {
			if (allocatorBlock && allocatorBlock->handle && image && allocation) {
				unregisterAllocation(allocatorBlock, allocation);
				vmaDestroyImage(allocatorBlock->handle, image, allocation);
			}
			allocatorBlock = std::move(o.allocatorBlock);
			image = o.image;
			allocation = o.allocation;
			o.image = VK_NULL_HANDLE;
			o.allocation = nullptr;
		}
		return *this;
	}
	/** 解引用：VkImage 句柄。 */
	VkImage operator*() const { return image; }
};

/**
 * Descriptor heap 封装：把带 eDescriptorHeapEXT usage 的 BDA buffer 视为连续的"描述符槽位数组"。
 * 与普通 buffer（VmaBuffer）的本质区别在"对齐记账"：
 * 1. 传给 vkCmdBindResourceHeapEXT 的范围起始地址必须按设备的 resourceHeapAlignment
 *    对齐（NVIDIA 32B / AMD 4B），VMA 原始分配地址不保证满足且不能重新分配，
 *    只能"从分配内部偏移进去"，因此 heap 是 buffer 内带自己 base/size 的子范围；
 * 2. CPU mapped 指针需独立偏移，且槽位寻址要求它按 descriptorSize 对齐；
 * 3. heap 存在厂商特定的 reserved 槽位：字节 [0, reservedSlots * descriptorSize)
 *    由驱动管理（NVIDIA 保留区 96768B = 3024 槽 @32B 统一步长，16B 步长时代旧值 6048 槽；
 *    AMD 0 槽），不可使用。
 * 对齐的计算细节见下方 VmaAllocatorRAII::createDescriptorHeap。
 */
export struct VmaDescriptorHeap {
	VmaBuffer bufferStorage;       // 底层 buffer（RAII：随 SharedAllocator 控制块由 VMA 统一销毁）
	void* pMappedData = nullptr;   // 偏移后的 CPU 地址（descriptorSize 对齐），slot 0 入口，写描述符用
	uint64_t gpuAddress = 0;       // 偏移后的 GPU 设备地址（resourceHeapAlignment 对齐），vkCmdBindResourceHeapEXT 用
	vk::DeviceSize gpuOffset = 0;  // heap 范围起点相对 buffer 原始起点的偏移（GPU/CPU 指针均加此偏移）
	uint32_t descriptorSize = 0;   // 槽位步长（字节），运行时查询（resource heap = max(buffer, image) 描述符大小，sampler heap = samplerDescriptorSize；勿硬编码）
	uint32_t capacity = 0;         // heap 槽位总数（含 reserved），buffer 分配大小 = capacity * descriptorSize
	uint32_t reservedSlots = 0;    // 驱动保留槽位数，用户可用槽位为 [reservedSlots, capacity)

	/** 默认构造：空状态（无 buffer，析构不做事）。 */
	VmaDescriptorHeap() = default;
	/**
	 * 唯一的非默认构造入口：由 VmaAllocatorRAII::createDescriptorHeap 产出，
	 * 保证不变式：gpuAddress 按 resourceHeapAlignment 对齐、pMappedData 按
	 * descriptorSize 对齐，且两者指向同一个 "slot 0"。
	 * @param buf 底层 buffer（移动接管）
	 * @param mapped 偏移后的 CPU 地址
	 * @param gpuAddr 偏移后的 GPU 地址
	 * @param gOffset 偏移量（getHeapSize 扣减用）
	 * @param descSize 槽位步长
	 * @param cap 槽位总数（含 reserved）
	 * @param reserved 驱动保留槽位数
	 */
	VmaDescriptorHeap(VmaBuffer&& buf, void* mapped, uint64_t gpuAddr, vk::DeviceSize gOffset, uint32_t descSize, uint32_t cap, uint32_t reserved = 0)
		: bufferStorage(std::move(buf)), pMappedData(mapped), gpuAddress(gpuAddr), gpuOffset(gOffset), descriptorSize(descSize), capacity(cap), reservedSlots(reserved) {}

	/** 析构：默认（底层 VmaBuffer RAII 负责销毁 buffer）。 */
	~VmaDescriptorHeap() = default;

	/** 拷贝删除：GPU 资源所有权唯一（move-only）。 */
	VmaDescriptorHeap(const VmaDescriptorHeap&) = delete;
	/** 拷贝赋值删除：同上。 */
	VmaDescriptorHeap& operator=(const VmaDescriptorHeap&) = delete;

	/** 移动语义（RAII）：移动后源对象被重置为已销毁状态，防止 buffer 被二次销毁。 */
	VmaDescriptorHeap(VmaDescriptorHeap&& o) noexcept
		: bufferStorage(std::move(o.bufferStorage)), pMappedData(o.pMappedData), gpuAddress(o.gpuAddress), gpuOffset(o.gpuOffset), descriptorSize(o.descriptorSize), capacity(o.capacity), reservedSlots(o.reservedSlots) {
		o.pMappedData = nullptr;
		o.gpuAddress = 0;
		o.gpuOffset = 0;
		o.descriptorSize = 0;
		o.capacity = 0;
		o.reservedSlots = 0;
	}

	/** 移动赋值：先销毁本对象持有的旧 buffer（经 VmaBuffer 移动赋值），再接管源对象的资源。 */
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

	/** 解引用：底层 VkBuffer 句柄。 */
	VkBuffer operator*() const { return *bufferStorage; }
	/** heap 范围起点的 GPU 地址（vkCmdBindResourceHeapEXT 用）。 */
	uint64_t getGpuAddress() const { return gpuAddress; }
	/** 槽位步长（字节）。 */
	uint32_t getDescriptorSize() const { return descriptorSize; }
	/** heap 槽位总数（含 reserved）。 */
	uint32_t getCapacity() const { return capacity; }
	/** 驱动保留槽位数（用户可用槽位为 [reservedSlots, capacity)）。 */
	uint32_t getReservedSlots() const { return reservedSlots; }
	/**
	 * heap 范围可用大小（字节）= 槽位总字节数 - 范围起点偏移。
	 * 必须用它作为 vkCmdBindResourceHeapEXT 的 size，而不是 descriptorSize * capacity：
	 * 曾漏扣偏移导致验证层报错 + device lost（见 docs/nvidia_compatibility_report.md）。
	 */
	vk::DeviceSize getHeapSize() const { return static_cast<vk::DeviceSize>(descriptorSize) * capacity - gpuOffset; }

	/**
	 * 指定槽位的 CPU 地址（写描述符用），带越界检查。
	 * 调用方 slot = Resources::allocSlot 按用途段分配（reservedSlots() + 段内游标；几何段
	 * 基址常量 HeapSlotGeometryBase 由 model_types_generated 生成）。
	 * @param slot 槽位号
	 * @return 该槽的 mapped 指针；slot >= capacity 时返回 DescriptorSlotOutOfBounds
	 */
	[[nodiscard]] std::expected<void*, Error> getSlotCpuAddress(uint32_t slot) const {
		if (slot >= capacity) {
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				"Descriptor heap slot out of bounds!"});
		}
		return static_cast<char*>(pMappedData) + (static_cast<size_t>(slot) * descriptorSize);
	}
};

/**
 * VMA 分配器的 RAII 包装：构造时 vmaCreateAllocator（失败抛 std::runtime_error），
 * 析构/最后引用消失时经控制块 deleter vmaDestroyAllocator。move-only。
 * createBuffer/createBufferWithAddress/createImage/createDescriptorHeap 产出
 * 共享同一控制块的 VmaBuffer/VmaImage/VmaDescriptorHeap。
 */
export class VmaAllocatorRAII {
   private:
	SharedAllocator allocatorBlock;

    public:
	/**
	 * 创建 VMA 分配器。
	 * @param instance Vulkan 实例
	 * @param physicalDevice 物理设备
	 * @param device 逻辑设备
	 * @param apiVersion 实例 API version（VMA 据此启用 BDA 等特性）
	 * @param flags VMA 创建标志（默认 BUFFER_DEVICE_ADDRESS——本项目 buffer 全走 BDA 路径）
	 * @note vmaCreateAllocator 失败抛 std::runtime_error（初始化期异常路径，不走 Error 契约）
	 */
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
		allocatorBlock = SharedAllocator(new AllocatorControlBlock{raw, {}},
			[](AllocatorControlBlock* p) {
				if (p) {
					if (p->handle) vmaDestroyAllocator(p->handle);
					delete p;
				}
			});
	}

	/** 析构：默认（分配器本体由控制块 shared_ptr deleter 销毁）。 */
	~VmaAllocatorRAII() = default;

	/** 拷贝删除：分配器句柄所有权唯一（move-only）。 */
	VmaAllocatorRAII(const VmaAllocatorRAII&) = delete;
	/** 拷贝赋值删除：同上。 */
	VmaAllocatorRAII& operator=(const VmaAllocatorRAII&) = delete;
	/** 移动构造：接管控制块。 */
	VmaAllocatorRAII(VmaAllocatorRAII&& o) noexcept : allocatorBlock(std::move(o.allocatorBlock)) {}

	/**
	 * 创建普通 VMA buffer（不查询 BDA 地址）。
	 * @param bufferInfo buffer 创建参数
	 * @param allocInfo 分配参数（usage/flags）
	 * @param pAllocInfo 可选输出：分配信息（含 mapped 指针）
	 * @param name 逐分配归因名（债-31，**静态字面量**；登记进台账 + VMA 本体）。🔴 **无默认值**：
	 *        新增分配点漏挂号 = 编译失败，比运行期发现早一个阶段（运行期兜底 = logMemoryStats
	 *        的台账自检行，拦的是"显式传 nullptr"这种形态）
	 * @return VmaBuffer；VMA 失败返回 VMASuccessFailed
	 */
	[[nodiscard]] std::expected<VmaBuffer, Error> createBuffer(const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo, const char* name) const {
		VkBuffer buffer;
		VmaAllocation allocation;
		VkResult result = vmaCreateBuffer(allocatorBlock->handle, reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo, &buffer, &allocation, pAllocInfo);
		if (result != VK_SUCCESS) {
			return std::unexpected(Error{ErrorCode::VMASuccessFailed,
				"VMA failed to create buffer"});
		}
		registerAllocation(allocatorBlock, allocation, name);
		return VmaBuffer(allocatorBlock, buffer, allocation);
	}

	/**
	 * 创建 BDA buffer 并查询设备地址（vkGetBufferDeviceAddress）。
	 * @param device 设备句柄（查 device address 用）
	 * @param bufferInfo buffer 创建参数
	 * @param allocInfo 分配参数
	 * @param pAllocInfo 可选输出：分配信息（createDescriptorHeap 从 pMappedData 读映射地址）
	 * @param name 逐分配归因名（债-31，**静态字面量**；登记进台账 + VMA 本体）。🔴 **无默认值**：
	 *        新增分配点漏挂号 = 编译失败，比运行期发现早一个阶段（运行期兜底 = logMemoryStats
	 *        的台账自检行，拦的是"显式传 nullptr"这种形态）
	 * @return VmaBuffer（deviceAddress 与 pMappedData 均已填）；VMA 失败返回 VMASuccessFailed
	 */
	[[nodiscard]] std::expected<VmaBuffer, Error> createBufferWithAddress(const vk::raii::Device& device, const vk::BufferCreateInfo& bufferInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo, const char* name) const {
		VkBuffer buffer;
		VmaAllocation allocation;
		VkResult result = vmaCreateBuffer(allocatorBlock->handle,
			reinterpret_cast<const VkBufferCreateInfo*>(&bufferInfo), &allocInfo,
			&buffer, &allocation, pAllocInfo);
		if (result != VK_SUCCESS) {
			return std::unexpected(Error{ErrorCode::VMASuccessFailed, "VMA failed to create buffer"});
		}
		registerAllocation(allocatorBlock, allocation, name);
		vk::BufferDeviceAddressInfo addrInfo{};
		addrInfo.buffer = buffer;
		vk::DeviceAddress addr = device.getBufferAddress(addrInfo);
		return VmaBuffer(allocatorBlock, buffer, allocation, addr, pAllocInfo ? pAllocInfo->pMappedData : nullptr);
	}

	/**
	 * 创建 VMA image。
	 * @param imageInfo image 创建参数
	 * @param allocInfo 分配参数
	 * @param pAllocInfo 可选输出：分配信息
	 * @param name 逐分配归因名（债-31，**静态字面量**；登记进台账 + VMA 本体）。🔴 **无默认值**：
	 *        新增分配点漏挂号 = 编译失败，比运行期发现早一个阶段（运行期兜底 = logMemoryStats
	 *        的台账自检行，拦的是"显式传 nullptr"这种形态）
	 * @return VmaImage；VMA 失败返回 VMASuccessFailed
	 */
	[[nodiscard]] std::expected<VmaImage, Error> createImage(const vk::ImageCreateInfo& imageInfo, const VmaAllocationCreateInfo& allocInfo, VmaAllocationInfo* pAllocInfo, const char* name) const {
		VkImage image;
		VmaAllocation allocation;
		VkResult result = vmaCreateImage(allocatorBlock->handle, reinterpret_cast<const VkImageCreateInfo*>(&imageInfo), &allocInfo, &image, &allocation, pAllocInfo);
		if (result != VK_SUCCESS) {
			return std::unexpected(Error{ErrorCode::VMASuccessFailed, "VMA failed to create image"});
		}
		registerAllocation(allocatorBlock, allocation, name);
		return VmaImage(allocatorBlock, image, allocation);
	}

	/**
	 * 创建 descriptor heap：分配带 eDescriptorHeapEXT usage 的 BDA buffer 并修正双端对齐。
	 * 对齐约束（两条，互相独立）：
	 * - GPU 端：传给 vkCmdBindResourceHeapEXT 的 heap 范围起始地址必须按
	 *   resourceHeapAlignment 对齐（NVIDIA 32B / AMD 4B）；
	 * - CPU 端：槽位寻址用的 mapped 指针必须按 descriptorSize 对齐。
	 * VMA 原始分配地址两者都不保证，且无法重新分配，因此策略是"从分配内部偏移"：
	 * 分配大小保持 descriptorSize * capacity 不变，把 heap 范围起点推进 buffer 内部
	 * offset 字节，尾部恰好落在 buffer 末尾（可用大小见 getHeapSize）。
	 * @param device 设备句柄（BDA 查询用）
	 * @param capacity 槽位总数（含 reserved）
	 * @param descriptorSize 槽位步长（resource heap = max(buffer, image) 描述符大小；
	 *        sampler heap = samplerDescriptorSize；运行时查询，勿硬编码）
	 * @param extraUsageFlags 追加 usage flags（必须含 eDescriptorHeapEXT）
	 * @param reservedSlots 驱动保留槽位数（头部 [0, reservedSlots) 范围）
	 * @param resourceHeapAlignment 设备 heap 对齐
	 * @param name 逐分配归因名（债-31，**静态字面量**；透传给底层 buffer 分配，**必填**）
	 * @return VmaDescriptorHeap；底层 buffer 创建失败时透传 VMASuccessFailed
	 * @note eShaderDeviceAddress 与 eDescriptorHeapEXT 两个 usage flag 必须同时设置，
	 *       缺任一个都是 undefined behavior（spec 3.3）
	 */
	[[nodiscard]] std::expected<VmaDescriptorHeap, Error> createDescriptorHeap(
		const vk::raii::Device& device,
		uint32_t capacity,
		uint32_t descriptorSize,
		vk::BufferUsageFlags extraUsageFlags,
		uint32_t reservedSlots,
		vk::DeviceSize resourceHeapAlignment,
		const char* name) const {
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
		auto storageResult = createBufferWithAddress(device, bufferInfo, allocCreateInfo, &allocInfo, name);
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

	/** 裸 VmaAllocator 句柄（控制块为空时返回 nullptr）。 */
	VmaAllocator get() const { return allocatorBlock ? allocatorBlock->handle : nullptr; }

	/**
	 * 逐分配归因台账（债-31）：当前**存活**分配的挂号名 / 字节数 / 内存类型。
	 * 消费方 = `Resources::logMemoryStats`（按名字聚合成"哪个模块占多少显存"的点名表）。
	 * @return 台账只读引用；控制块为空（被移动走）时返回空表
	 */
	[[nodiscard]] const std::vector<AllocRecord>& allocations() const {
		static const std::vector<AllocRecord> kEmpty{};  // 空控制块的稳定空表
		return allocatorBlock ? allocatorBlock->liveAllocs : kEmpty;
	}
};

}  // namespace RGL