module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <fmt/format.h>
module vk_resources:impl;

import vk_resources;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import vkerror;
import slot_ledger;  // samplerSlots 成员的构造（SlotLedger 构造非默认，须在初始化列表给基址/容量）
import diagnostics;  // 债-140 H3 毒化实验（poisonActiveFor / fillGpuPoison），rlbase 单向向下

namespace RGL {

// Resources 重方法定义（ctor/sampler/sceneData/heap 创建，2026-09-02
// 编译提速 A 方案）：函数体移出接口 TU，BMI 只留类声明；本单元是叶子边。
//（ensure*/create* 均为启动期一次性路径，无跨 TU 内联价值）

/**
 * 初始化列表顺序与 heap 槽位布局契约见接口注释。实现补充：allocator 的 flags 显式传
 * BDA（第 5 参）——VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT 不可省略，否则所有
 * eShaderDeviceAddress buffer 创建返回 VK_ERROR_INITIALIZATION_FAILED（见体内注释）；
 * 内核不创建任何内容资产（架构债务 1 方案 B——几何/纹理/scene data/RT 采样槽全部由
 * 内容层经 allocSlot 按用途段领取，见体内注释）。
 */
Resources::Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain)
	: vkInstance(vkInstance),
		  vkDevice(vkDevice),
		  vkSwapchain(vkSwapchain),
 	  // flags 显式传 BDA（第 5 参）——VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT 不可省略，
 	  // 否则所有 eShaderDeviceAddress buffer 创建返回 VK_ERROR_INITIALIZATION_FAILED
 	  allocator(*vkInstance.getInstance(), *vkDevice.getPhysicalDevice(), *vkDevice.getDevice(),
 		  vkInstance.getApiVersion(),
 		  VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT),
		  // 深度图按**渲染** extent 分配（批 14.3）：它与 G-buffer / scene_hdr 同网格，
		  // 必须与颜色附件一致，而不是与后缓冲一致。renderScale=1.0 时两者相等。
		  depthImage(createDepthImage(allocator, vkSwapchain.getRenderExtent()).value()),
		  depthImageView(vkDevice.getDevice(), buildDepthView(*depthImage)),
		  descriptorHeap(createDescriptorHeap().value()),
		  samplerHeap(createSamplerHeap().value()),
		  // sampler heap 用户槽台账：基址 = reservedSlots（驱动保留区 [0, reserved) 声明为
		  // 不可用），容量 = capacity − reservedSlots（当前驱动 = 8 个用户槽）。成员声明序在
		  // samplerHeap 之后 ⟹ 此处读它已构造完成。
		  samplerSlots(samplerHeap.getReservedSlots(),
			  samplerHeap.getCapacity() - samplerHeap.getReservedSlots()) {
		initializeSlotSegments(reservedSlots());
		// 内核不创建任何内容资产（架构债务 1 方案 B）：几何（内建 cube / gltf 模型）、
		// 纹理、scene data、RT 采样槽全部由内容层经 allocSlot 按用途段领取
	}













/** 参数/返回见接口注释。实现：eD32Sfloat、单采样、eDepthStencilAttachment、
 * VMA 专用内存（VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT）。 */
std::expected<RGL::VmaImage, Error> Resources::createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent) {
		vk::ImageCreateInfo depthImageInfo{};
		depthImageInfo.imageType = vk::ImageType::e2D;
		depthImageInfo.format = vk::Format::eD32Sfloat;
		depthImageInfo.extent = vk::Extent3D(extent.width, extent.height, 1);
		depthImageInfo.mipLevels = 1;
		depthImageInfo.arrayLayers = 1;
		depthImageInfo.samples = vk::SampleCountFlagBits::e1;
		depthImageInfo.tiling = vk::ImageTiling::eOptimal;
		depthImageInfo.usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
			// Phase 11 批 11.4 SSAO：ao.frag 以 SAMPLED_IMAGE 读深度重建世界坐标
			//（见 Resources::ensureDepthSampleSlot）。此前无任何 shader 采深度，
			// 故本 flag 增量为纯行为中性（不改变既有渲染结果）。
			vk::ImageUsageFlagBits::eSampled;

		VmaAllocationCreateInfo depthAllocInfo{};
		depthAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
		depthAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;

		return allocator.createImage(depthImageInfo, depthAllocInfo, nullptr, "resources/depth");
	}

/** 契约见接口注释。实现：e2D / eD32Sfloat / eDepth aspect / levelCount = layerCount = 1
 *（局部临时，供构造器 / recreateDepthImage 的 raii ImageView 构造消费）。 */
vk::ImageViewCreateInfo Resources::buildDepthView(VkImage image) {
		vk::ImageViewCreateInfo depthViewInfo{};
		depthViewInfo.image = image;
		depthViewInfo.viewType = vk::ImageViewType::e2D;
		depthViewInfo.format = vk::Format::eD32Sfloat;
		depthViewInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eDepth;
		depthViewInfo.subresourceRange.levelCount = 1;
		depthViewInfo.subresourceRange.layerCount = 1;
		return depthViewInfo;
	}

/**
 * 债-140 H3：给 descriptor heap 的**用户槽区**铺毒值（门 `diagnostics.poison_gpu_mem`
 * 且 scope 含 `heap`；语义与判据原理 → `DiagnosticsConfig::poisonGpuMem` 注释）。
 *
 * 🔴 只毒 `[reservedSlots, capacity)`：字节 `[0, reservedSlots*stride)` 是厂商保留区
 * （契约见 VmaBuffer.cppm 的 VmaDescriptorHeap 类注释），host 写它是 UB —— 毒保留区等于
 * 自己制造一个不可归因的崩溃，那会污染本案唯一还在说话的取证面。
 * 已被引擎写过的槽随后被正常写覆盖 ⟹ **活到 GPU 读取那一刻的毒 = 从未上传的槽**（H2 形态）。
 *
 * 只走公开访问器（getSlotCpuAddress / getReservedSlots / getCapacity / getDescriptorSize），
 * 不碰 pMappedData 私有量：偏移算术已经由 getSlotCpuAddress 负责，再算一遍就是第二份真相源。
 * @param heap 刚建好的 heap（host 常驻映射）
 * @param name 仅用于日志点名（resource heap / sampler heap 是两个对象，别混成一条）
 */
static void poisonHeapUserSlots(RGL::VmaDescriptorHeap& heap, std::string_view name) {
	if (!poisonActiveFor(PoisonTarget::heap))
		return;
	const uint32_t first = heap.getReservedSlots();
	if (first >= heap.getCapacity())
		return;  // 没有用户槽（容量全被保留区吃掉）——本引擎配置下不会发生，静默跳过即可
	const auto addr = heap.getSlotCpuAddress(first);
	if (!addr)
		return;  // 越界已由 getSlotCpuAddress 报 Error；这里不重复出声
	const size_t bytes = static_cast<size_t>(heap.getCapacity() - first) * heap.getDescriptorSize();
	fillGpuPoison(*addr, bytes);
	if (auto* logger = RLLogger::getInstance())
		logger->warn("poison[heap]: {} 用户槽 [{}, {}) × {}B = {} 字节铺毒值（基准字 {:08X}，"
					 "反解：(fault 低 32 位 − {:08X}) × 4 = 用户区字节偏移）",
			name, first, heap.getCapacity(), heap.getDescriptorSize(), bytes,
			kGpuPoisonBaseWord, kGpuPoisonBaseWord);
}

/**
 * 容量构成与槽位分区契约见接口注释。实现：运行时查询
 * VkPhysicalDeviceDescriptorHeapPropertiesEXT——统一步长 = max(bufferDescriptorSize,
 * imageDescriptorSize)（image 描述符（SAMPLED_IMAGE）比 buffer 描述符宽：RADV 64B > 16B，
 * docs/archive/heap_texture_research.md Q1，32B 为旧 Mesa 首测值；运行时查询，勿硬编码）；
 * reservedSlots = ceil(minResourceHeapReservedRange / 步长)（NVIDIA = 96768B → 3024 槽
 * @32B 统一步长，16B 步长时代旧值 6048 槽；RADV/Intel = 0）；userSlots = geometry +
 * texture + runtime 三段固定容量；capacity = reserved + userSlots；实测值打日志。
 */
std::expected<RGL::VmaDescriptorHeap, Error> Resources::createDescriptorHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();

		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		// Phase 3 统一步长：image 描述符（SAMPLED_IMAGE）比 buffer 描述符宽
		//（RADV 64B > 16B，docs/archive/heap_texture_research.md Q1；32B 为旧 Mesa 首测值），slot 步长取 max（运行时查询，勿硬编码）
		uint32_t bufferSize = static_cast<uint32_t>(heapProps.bufferDescriptorSize);
		uint32_t imageSize = static_cast<uint32_t>(heapProps.imageDescriptorSize);
		uint32_t descriptorSize = std::max(bufferSize, imageSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize;
		vk::DeviceSize resourceHeapAlignment = heapProps.resourceHeapAlignment;
		// 用户槽位全为运行时分配（几何/纹理/scene data/RT 按用途段领取，无具体资产专属预留）
		uint32_t userSlots = kGeometrySlotCapacity + kTextureSlotCapacity + kRuntimeSlotCapacity;
		uint32_t capacity = userSlots + reservedSlots;
		if (auto* logger = RLLogger::getInstance())
			logger->info("descriptor heap: bufferDesc={}B imageDesc={}B stride={}B reservedSlots={} alignment={}B capacity={}",
				bufferSize, imageSize, descriptorSize, reservedSlots, resourceHeapAlignment, capacity);

		auto heap = allocator.createDescriptorHeap(
			vkDevice.getDevice(),
			capacity, descriptorSize,
			vk::BufferUsageFlagBits::eDescriptorHeapEXT,
			reservedSlots,
			resourceHeapAlignment,
			"resources/descriptor_heap");
	if (heap)
		poisonHeapUserSlots(*heap, "resource");
	return heap;
	}/**
 * 背景与容量构成见接口注释。实现：步长 = samplerDescriptorSize（运行时查询）；
 * reservedSlots = ceil(minSamplerHeapReservedRange / 步长)（NVIDIA = 512B → 16 槽 @32B，
 * RADV/Intel = 0）；capacity = reserved + 8（固定小裕量：用户槽由 `Resources::claimSamplerSlot`
 * 申报发放，现行归属者 3 个 = 共享 LINEAR / AO point / sky env，余量 5）；实测值打日志。
 * ⚠ 裕量随归属者数涨，领不到时 claimSamplerSlot 打 error + 返回 Error（不静默越界），
 * 改这里的同时记得看启动日志的 sampler heap 台账行。
 */
std::expected<RGL::VmaDescriptorHeap, Error> Resources::createSamplerHeap() {
		auto props = vkDevice.getPhysicalDevice().getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		auto& heapProps = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
		uint32_t samplerSize = static_cast<uint32_t>(heapProps.samplerDescriptorSize);
		uint32_t reservedSlots = static_cast<uint32_t>(heapProps.minSamplerHeapReservedRange + samplerSize - 1) / samplerSize;
		uint32_t capacity = reservedSlots + 8;
		if (auto* logger = RLLogger::getInstance())
			logger->info("sampler heap: samplerDesc={}B reservedSlots={} alignment={}B capacity={}",
				samplerSize, reservedSlots, heapProps.samplerHeapAlignment, capacity);

		auto heap = allocator.createDescriptorHeap(
			vkDevice.getDevice(),
			capacity, samplerSize,
			vk::BufferUsageFlagBits::eDescriptorHeapEXT,
			reservedSlots,
			heapProps.samplerHeapAlignment,
			"resources/sampler_heap");
	if (heap)
		poisonHeapUserSlots(*heap, "sampler");
	return heap;
	}

/**
 * 债-31 逐分配归因（logMemoryStats 的第四段）：把存活分配台账按挂号名聚合成
 * "哪个模块占了多少显存"的点名表，并与 vmaCalculateStatistics 的 allocationBytes 自检。
 *
 * 数据源是自己的台账（AllocatorControlBlock::liveAllocs，随分配建销）而**不是**
 * vmaBuildStatsString：后者给的是逐块/逐空洞的 JSON 树，点不出"谁分配的"，整棵树进日志
 * 也没法逐位回归。台账只存静态字面量指针，聚合只在此处（日志期）做，分配路径零开销。
 * @param stage 阶段标签（与前三段同 `vma[stage]:` 前缀）
 * @param records 存活分配台账（VmaAllocatorRAII::allocations()）
 * @param alloc 分配器句柄（查内存类型 property 用）
 * @param memProps 内存属性表（可为 nullptr）
 * @param typeCount 内存类型数（越界防护）
 * @param vmaAllocBytes vmaCalculateStatistics 的 allocationBytes（自检基准）
 * @param vmaAllocCount vmaCalculateStatistics 的 allocationCount（自检基准：条数差非 0 =
 *                      有分配点绕过了 registerAllocation，比任何单测都硬）
 */
void logAllocationAttribution(std::string_view stage, const std::vector<AllocRecord>& records,
	VmaAllocator alloc, const VkPhysicalDeviceMemoryProperties* memProps, uint32_t typeCount,
	uint64_t vmaAllocBytes, uint64_t vmaAllocCount) {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	constexpr double mib = 1024.0 * 1024.0;

	/** 一个挂号名聚合成一行。 */
	struct NameAgg {
		std::string_view name;       ///< 挂号名（"未挂号" = 绕过登记面的分配，正常应为 0 项）
		uint64_t bytes = 0;          ///< 该名下分配的字节合计
		uint32_t count = 0;          ///< 该名下的分配条数（循环 / 逐帧槽分配 >1）
		uint32_t memoryType = 0xFFFFFFFF;  ///< 首条记录的内存类型号（同名分配同类型，实测成立）
	};
	std::vector<NameAgg> aggs;
	uint64_t ledgerBytes = 0;
	for (const auto& rec : records) {
		ledgerBytes += rec.size;
		const std::string_view nm = rec.name ? std::string_view{rec.name} : std::string_view{"未挂号"};
		if (const auto hit = std::ranges::find(aggs, nm, &NameAgg::name); hit != aggs.end()) {
			hit->bytes += rec.size;
			++hit->count;
		}
		else
			aggs.push_back(NameAgg{nm, rec.size, 1u, rec.memoryType});
	}
	// 排序 = 字节降序、同字节按名升序：迭代序不确定的容器直接打日志会让逐位回归漂
	std::ranges::sort(aggs, [](const NameAgg& a, const NameAgg& b) {
		if (a.bytes != b.bytes)
			return a.bytes > b.bytes;
		return a.name < b.name;
	});
	logger->info("vma[{}]:   归因 {} 项 合计 {:.2f}MiB（台账自检 {} 条 {}B，与 vma 差 {} 条 {}B；"
		"差非 0 = 有分配点绕过登记面；未挂号分配单列成\"未挂号\"行）",
		stage, aggs.size(), static_cast<double>(ledgerBytes) / mib, records.size(), ledgerBytes,
		static_cast<int64_t>(vmaAllocCount) - static_cast<int64_t>(records.size()),
		static_cast<int64_t>(vmaAllocBytes) - static_cast<int64_t>(ledgerBytes));
	for (const auto& a : aggs) {
		VkMemoryPropertyFlags flags = 0;
		if (memProps && a.memoryType < typeCount)
			vmaGetMemoryTypeProperties(alloc, a.memoryType, &flags);
		logger->info("vma[{}]:     {:.2f}MiB x{:<4} {} [{}{} type{}]",
			stage, static_cast<double>(a.bytes) / mib, a.count, a.name,
			(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEV_LOCAL" : "HOST_ONLY",
			(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
			a.memoryType);
	}
}

/**
 * 显存账（批 9.5 观测面，声明注释见接口）。四段：总量 / 逐 heap / 逐内存类型 /
 * 逐分配归因（债-31）。前三段数据源 vmaCalculateStatistics（VMA 全块遍历，只准启动期
 * 调用）；heap 的 usage/budget 走 vmaGetHeapBudgets（VK_EXT_memory_budget 缺位时两者为 0，
 * 不是"没占显存"）。刻意不用 vmaBuildStatsString：整棵 JSON 树（含每块每空洞）进日志既
 * 读不动也没法回归。
 */
void Resources::logMemoryStats(std::string_view stage) const {
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	const VmaAllocator alloc = allocator.get();
	if (!alloc)
		return;  // 分配器未建（理论不可达：Resources 构造即建）

	VmaTotalStatistics st{};
	vmaCalculateStatistics(alloc, &st);
	constexpr double mib = 1024.0 * 1024.0;
	logger->info("vma[{}]: total blocks={} blockBytes={:.2f}MiB allocations={} allocBytes={:.2f}MiB "
		"unusedRanges={} unusedBytes={:.2f}MiB largestAlloc={:.2f}MiB",
		stage, st.total.statistics.blockCount,
		static_cast<double>(st.total.statistics.blockBytes) / mib,
		st.total.statistics.allocationCount,
		static_cast<double>(st.total.statistics.allocationBytes) / mib,
		st.total.unusedRangeCount,
		static_cast<double>(st.total.statistics.blockBytes - st.total.statistics.allocationBytes) / mib,
		static_cast<double>(st.total.allocationSizeMax) / mib);

	const VkPhysicalDeviceMemoryProperties* memProps = nullptr;
	vmaGetMemoryProperties(alloc, &memProps);
	const uint32_t heapCount = memProps ? memProps->memoryHeapCount : 0u;
	std::vector<VmaBudget> budgets(heapCount);
	if (heapCount > 0)
		vmaGetHeapBudgets(alloc, budgets.data());
	for (uint32_t h = 0; h < heapCount; ++h) {
		const auto& hs = st.memoryHeap[h].statistics;
		if (hs.blockBytes == 0)
			continue;  // 未使用的 heap 不打
		logger->info("vma[{}]:   heap{} blockBytes={:.2f}MiB allocBytes={:.2f}MiB budgetUsage={:.2f}MiB budget={:.2f}MiB",
			stage, h, static_cast<double>(hs.blockBytes) / mib,
			static_cast<double>(hs.allocationBytes) / mib,
			static_cast<double>(budgets[h].usage) / mib,
			static_cast<double>(budgets[h].budget) / mib);
	}
	const uint32_t typeCount = memProps ? memProps->memoryTypeCount : 0u;
	for (uint32_t t = 0; t < typeCount; ++t) {
		const auto& ts = st.memoryType[t].statistics;
		if (ts.blockBytes == 0)
			continue;  // 空内存类型不打（本机一般只 DEVICE_LOCAL 一两类）
		VkMemoryPropertyFlags flags = 0;
		vmaGetMemoryTypeProperties(alloc, t, &flags);
		logger->info("vma[{}]:   type{} heap{} {}{} blocks={} allocs={} blockBytes={:.2f}MiB allocBytes={:.2f}MiB",
			stage, t, memProps->memoryTypes[t].heapIndex,
			(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "DEV_LOCAL" : "HOST_ONLY",
			(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? " HOST_VISIBLE" : "",
			ts.blockCount, ts.allocationCount,
			static_cast<double>(ts.blockBytes) / mib,
			static_cast<double>(ts.allocationBytes) / mib);
	}

	// 第四段 = 债-31 逐分配归因（聚合与自检见 logAllocationAttribution）
	logAllocationAttribution(stage, allocator.allocations(), alloc, memProps, typeCount,
		st.total.statistics.allocationBytes, st.total.statistics.allocationCount);
}

}  // namespace RGL
