module;
#include <glm/glm.hpp>        // glm::vec4（planes 形参 / MeshletCullPush 字段；GMF include 不随 import 传播）
#include <vulkan/vulkan.h>    // VK_* 宏/C 函数（rlvk 不含宏；各 impl 同款自含）
#include <vk_mem_alloc.h>     // VMA C API（VmaRaii 只导出 C++ 包装）
#include <fmt/format.h>
module meshlet_cull_service:impl;

import meshlet_cull_service;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import scene_data;
import VmaRaii;
import shader;  // makeMappedCompute（binding→heap 槽一对一的 compute shader object 工厂）
import meshlet_cull_types_generated;

namespace RGL {
namespace {

/**
 * 剔除 compute 的工作组边长（🔴 必须 == `meshlet_cull.comp.slang` 的 `[numthreads(64,1,1)]`）。
 * 漂移的两个方向不对称：多派 = 尾线程全部落进 shader 的 `tid.x >= itemCount` 守卫，无害；
 * **少派**才是事故——尾部剔除项没算到 = 标志停在上一帧的值 = 少画一块且没有任何错误信号。
 */
constexpr uint32_t kCullWorkgroupSize = 64;

/** 向上取整到 16 的整数倍（VMA 分配粒度，`meshlet_geometry_impl.cppm::alignUp` 同式）。 */
vk::DeviceSize alignUp16(vk::DeviceSize value) {
	return (value + 15) / 16 * 16;
}

}  // namespace

MeshletCullService::MeshletCullService(DeviceContext& device, Resources& resources,
	SceneDataService& sceneData, uint32_t clusterHeaderSlot, uint32_t clusterCapacity)
	: device(device),
	  resources(resources),
	  sceneData(sceneData),
	  clusterHeaderSlot(clusterHeaderSlot),
	  clusterCapacity(clusterCapacity) {
	allocRuntimeSlots();
	createVisibleFlags();
	createCullShader();
	if (auto* logger = RLLogger::getInstance())
		logger->info("meshlet: 逐簇剔除服务已装配（★1-a2-i）—— 标志容量={} 条（槽 {}，compute "
					 "binding 4 = mesh binding 21 同槽）；簇头复用槽 {}（mesh binding 18）；"
					 "对象矩阵表子范围槽 {}（compute binding 23）；items 槽 {}（uploadItems 时建 buffer）",
			clusterCapacity, visibleBufferSlot, clusterHeaderSlot, objectMatrixSlot, itemsSlot);
}

void MeshletCullService::allocRuntimeSlots() {
	auto ensureSlot = [&](const char* name) -> uint32_t {
		auto slot = resources.allocSlot(SlotSegment::Runtime, name);
		if (!slot)
			throw std::runtime_error(slot.error().describe());
		return *slot;
	};
	itemsSlot = ensureSlot("meshlet.cull.items");
	visibleBufferSlot = ensureSlot("meshlet.cull.visible");
	// 批 T1：对象矩阵表子范围槽（幂等取号，内部先 ensure 全区；ShaderRegistry / SceneDataService
	// 早已建过，这里只读号不重建）。旧的「全区字节读」槽随 compute binding 2 一起退役。
	objectMatrixSlot = sceneData.objectMatrixSlot();
}

void MeshletCullService::createVisibleFlags() {
	auto& vkDevice = device.getDevice();
	// 容量口径 → 类注释「为什么按总簇数而不是 itemCount」。空表也要一个 buffer
	//（VkBufferCreateInfo::size 必须 > 0），故下限取 16B。
	const vk::DeviceSize bytes = std::max<vk::DeviceSize>(
	    alignUp16(static_cast<vk::DeviceSize>(clusterCapacity) * 4), 16);
	vk::BufferCreateInfo info{};
	info.size = bytes;
	// eTransferDst 只为启动期那一次 vkCmdFillBuffer；运行期唯一写者是 compute。
	info.usage = vk::BufferUsageFlagBits::eStorageBuffer |
	    vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst;
	info.sharingMode = vk::SharingMode::eExclusive;
	// device-local：每帧被 compute 写；host 映射形态会在无 large-BAR 的独显上把它落到系统内存
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	auto created = resources.getAllocator().createBufferWithAddress(vkDevice, info, alloc, nullptr,
		"meshlet_cull/visible_flags");
	if (!created)
		throw std::runtime_error(created.error().describe());
	visibleBuffer = std::move(*created);

	// 一次性填 1（= 「全可见」）。🔴 这不是可选的初始化：门关时 compute 根本不 dispatch，
	// 这张表就是 mesh binding 21 的全部内容，未初始化的 device-local 内存里随便一个 0 都会让
	// 那一簇凭空消失（误剔没有任何错误信号）。一次性命令缓冲 + fence 的形态照
	// `Resources::ensureEvssmSampleSlot` 的 fallback 初始化（raii CommandBuffers 出作用域自动 free）。
	{
		auto cmdBufs = vkDevice.allocateCommandBuffers(
		    {*device.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
		auto& cmd = cmdBufs[0];
		cmd.begin({});
		cmd.fillBuffer(visibleBuffer.buffer, 0, bytes, 1u);
		cmd.end();
		vk::raii::Fence fence(vkDevice, vk::FenceCreateInfo{});
		const VkCommandBuffer raw = *cmd;
		VkSubmitInfo si{};
		si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		si.commandBufferCount = 1;
		si.pCommandBuffers = &raw;
		if (::vkQueueSubmit(device.getGraphicsQueue(), 1, &si, *fence) != VK_SUCCESS)
			throw std::runtime_error("meshlet cull: 可见标志初值提交失败");
		if (vkDevice.waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX)
		    != vk::Result::eSuccess)
			throw std::runtime_error("meshlet cull: 可见标志初值 fence 超时");
	}
	writeSlotDescriptor(visibleBuffer.getDeviceAddress(), bytes, visibleBufferSlot);
}

void MeshletCullService::createCullShader() {
	// binding 号 ≠ 槽号：四张表各自领/借来的槽号在此**逐 binding 烘死**（vkCreateShadersEXT 之后
	// 不可改）。键写 binding **变量名**（号查构建期接口表）——本 compute 的 0/1/4 是私有布局，
	// 挪号历史上要 shader 与这里同步改两处，现在只改 shader。簇头表走的是 mesh binding 18 那个槽
	// （复用，不新建描述符）；标志表走本服务领的槽，mesh 侧 binding 21 烘的也是同一个号
	// ⟹ 读写两侧天然一致。
	// 「单份 buffer 跨帧复用」不是省事的选择，是这里的形状决定的（→ 类注释）。
	cullShader = makeMappedCompute(device.getDevice(), "generated_shaders/meshlet_cull.comp.spv",
	    std::vector<std::pair<std::string_view, RGL::HeapSlot>>{
	        {"cullItems", RGL::HeapSlot{itemsSlot}}, {"clusters", RGL::HeapSlot{clusterHeaderSlot}},
	        {"clusterVisible", RGL::HeapSlot{visibleBufferSlot}},
	        {"objectMatrixTable", RGL::HeapSlot{objectMatrixSlot}}},
	    resources.getDescriptorHeap().getDescriptorSize(), &resources.computeAccessLog());
}

void MeshletCullService::writeSlotDescriptor(vk::DeviceAddress address, vk::DeviceSize bytes,
	uint32_t slot) const {
	vk::DeviceAddressRangeEXT range{address, bytes};
	vk::ResourceDescriptorInfoEXT desc{};
	desc.type = vk::DescriptorType::eStorageBuffer;
	desc.data.pAddressRange = &range;  // 同步 host 写，局部临时量存活到调用返回即够
	auto slotAddr = resources.getDescriptorHeap().getSlotCpuAddress(slot);
	if (!slotAddr)
		throw std::runtime_error(slotAddr.error().describe());
	device.getDevice().writeResourceDescriptorsEXT(desc,
	    vk::HostAddressRangeEXT{*slotAddr, resources.getDescriptorHeap().getDescriptorSize()});
}

void MeshletCullService::uploadItems(std::span<const MeshletCullItem> items) {
	if (items.empty())
		return;  // 没有剔除项 = 没有簇（或扫描全数回落）⟹ itemCount 保持 0 ⟹ recordDispatch 零命令
	// 🔴 容量闸门：标志 buffer 的容量按「总簇数」给（构造期），而 `cullBase + 簇内序` 的上界
	// 就是 itemCount（`meshlet_ranges::buildClusterCullItems` 的前缀和口径）。项数超过容量说明
	// 区间表本身已不自洽，写越界会踩到 buffer 外的设备内存——不报错、不触发 VVL，只在画面上
	// 留下解释不了的垃圾，所以必须在装配期拦下来出声（抛 = 启动期失败，同本类其余路径）。
	if (items.size() > clusterCapacity)
		throw std::runtime_error(fmt::format(
		    "meshlet cull: 剔除项 {} 条 > 标志表容量 {} 条（= 簇头表条数）—— 区间表与簇表不自洽",
		    items.size(), clusterCapacity));
	// host 映射直写（照 GpuCull 的 cullTable 形态：CPU_TO_GPU + MAPPED + SEQUENTIAL_WRITE，
	// 启动期一次 memcpy，无 staging）。meshlet_geometry_impl 的教训：映射内存**不保证**
	// host-coherent（无 large-BAR 的独显会落到非 coherent 系统内存），必须显式 flush。
	const vk::DeviceSize bytes = alignUp16(items.size_bytes());
	vk::BufferCreateInfo info{};
	info.size = bytes;
	info.usage = vk::BufferUsageFlagBits::eStorageBuffer |
	    vk::BufferUsageFlagBits::eShaderDeviceAddress;
	info.sharingMode = vk::SharingMode::eExclusive;
	VmaAllocationCreateInfo alloc{};
	alloc.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	alloc.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
	    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo allocData{};
	auto created =
	    resources.getAllocator().createBufferWithAddress(device.getDevice(), info, alloc, &allocData,
	    "meshlet_cull/items");
	if (!created)
		throw std::runtime_error(created.error().describe());
	auto* dst = static_cast<std::byte*>(allocData.pMappedData);
	if (dst == nullptr)
		throw std::runtime_error("meshlet cull: 剔除项表映射指针为空（MAPPED_BIT 没生效？）");
	std::memcpy(dst, items.data(), items.size_bytes());
	std::memset(dst + items.size_bytes(), 0, static_cast<size_t>(bytes - items.size_bytes()));
	// 先取地址、先 flush，再 move——move 之后 created 内的句柄被清空（防二次销毁）。
	const vk::DeviceAddress address = created->deviceAddress;
	vmaFlushAllocation(created->allocatorBlock->handle, created->allocation, 0, bytes);
	itemsBuffer = std::move(*created);
	writeSlotDescriptor(address, bytes, itemsSlot);
	nItems = static_cast<uint32_t>(items.size());
	if (auto* logger = RLLogger::getInstance())
		logger->info("meshlet: 逐簇剔除项表已上传 GPU —— items={}（{}B，槽 {}），dispatch 组数={}",
		    nItems, bytes, itemsSlot, (nItems + kCullWorkgroupSize - 1) / kCullWorkgroupSize);
}

void MeshletCullService::recordDispatch(vk::raii::CommandBuffer& cmd,
	std::span<const glm::vec4, 6> planes) {
	if (nItems == 0)
		return;  // uploadItems 没跑过 = 没有任何剔除项，一条命令都不该发（门关路径零命令）
	const auto& sceneBuffer = sceneData.getBuffer();

	// ── dispatch 前的依赖（一次 pipelineBarrier2 装三条，源点相同 ⟹ 合并成一条命令）────────
	// (a) **host 写 / Transfer 写 → compute 读 + heap 读**：`recordFixedDataPath` 那两条 barrier
	//     的 dstStageMask 都只有 `eAllGraphics`，**不含 `eComputeShader`**——heap barrier 覆盖
	//     描述符的 host 写、post-copy barrier 覆盖 scene data 的 Transfer 写，两者都只保证到
	//     图形阶段。compute 读 heap 描述符与 scene data 的这条依赖必须自己补（GpuCull 在独立
	//     cull CB 里同样自带一套，见 gpu_cull_impl.cppm::recordCullPass）。
	// (b) **scene data：Transfer 写 → compute 读**（同 (a) 的理由，落到具体 buffer 区间）。
	// (c) **可见标志：写前依赖**（上一帧 mesh 读 / 上一帧 compute 写 → 本帧 compute 写）。
	//     单份 buffer 跨 frames-in-flight 复用的 WAR/WAW 补齐，选型理由 → 类注释。pipeline
	//     barrier 的 first 作用域含同队列更早提交的命令（跨 submission），故一条补齐两帧之间
	//     的读写序；缺它的症状是 syncval 的 WRITE_AFTER_* hazard（scene data 同型实测）。
	vk::MemoryBarrier2 toCompute{};
	toCompute.srcStageMask = vk::PipelineStageFlagBits2::eHost | vk::PipelineStageFlagBits2::eTransfer;
	toCompute.srcAccessMask = vk::AccessFlagBits2::eHostWrite | vk::AccessFlagBits2::eTransferWrite;
	toCompute.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	toCompute.dstAccessMask =
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eResourceHeapReadEXT;
	std::vector<vk::BufferMemoryBarrier2> preBuffers;
	preBuffers.reserve(2);
	if (sceneBuffer.buffer != VK_NULL_HANDLE) {
		vk::BufferMemoryBarrier2 sceneToCompute{};
		sceneToCompute.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
		sceneToCompute.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
		sceneToCompute.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
		sceneToCompute.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
		sceneToCompute.buffer = sceneBuffer.buffer;
		sceneToCompute.size = kSceneDataBytes;
		preBuffers.push_back(sceneToCompute);
	}
	vk::BufferMemoryBarrier2 flagsBeforeWrite{};
	flagsBeforeWrite.srcStageMask =
	    vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader;
	flagsBeforeWrite.srcAccessMask =
	    vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
	flagsBeforeWrite.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	flagsBeforeWrite.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;
	flagsBeforeWrite.buffer = visibleBuffer.buffer;
	flagsBeforeWrite.size = VK_WHOLE_SIZE;
	preBuffers.push_back(flagsBeforeWrite);
	vk::DependencyInfo preDep{};
	preDep.setMemoryBarriers(toCompute);
	preDep.setBufferMemoryBarriers(preBuffers);
	cmd.pipelineBarrier2(preDep);

	// dispatch（push = MeshletCullPush 100B ≤ maxPushDataSize；planes 与 CPU 剔除同源同值 =
	// 对账基准一致，与 cull.comp 那条线同一条纪律）。vkCmdPushDataEXT 在**录制期**把数据拷进
	// 命令流（GpuCull 传栈上临时量既证），故 push 是本函数的局部量而非成员。
	MeshletCullPush push{};
	std::copy(planes.begin(), planes.end(), push.planes.begin());
	push.itemCount = nItems;
	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> handles = {*cullShader};
	cmd.bindShadersEXT(stages, handles);
	cmd.pushDataEXT(
	    vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(MeshletCullPush))));
	cmd.dispatch((nItems + kCullWorkgroupSize - 1) / kCullWorkgroupSize, 1, 1);

	// ── compute 写标志 → mesh shader 读（`eAllGraphics` 含 MeshShaderStage）─────────────────
	// 消费方 = meshlet.mesh.slang binding 21 的早退。缺这条 = mesh 可能读到上一帧的标志
	// （边界簇闪烁），且 VVL BUFFER_SYNC 直接报 hazard。
	vk::BufferMemoryBarrier2 flagsToMesh{};
	flagsToMesh.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	flagsToMesh.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
	flagsToMesh.dstStageMask = vk::PipelineStageFlagBits2::eAllGraphics;
	flagsToMesh.dstAccessMask = vk::AccessFlagBits2::eShaderRead;
	flagsToMesh.buffer = visibleBuffer.buffer;
	flagsToMesh.size = VK_WHOLE_SIZE;
	vk::DependencyInfo outDep{};
	outDep.setBufferMemoryBarriers(flagsToMesh);
	cmd.pipelineBarrier2(outDep);
}

}  // namespace RGL
