module;
#include <vulkan/vulkan.h>  // DGC C 结构体 + PFN（v357 vulkan.hpp 无 vk:: 包装）
#include <fmt/format.h>  // 契约违例/失败报错
module vk_indirect_commands:impl;

import vk_indirect_commands;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_device;
import shader_registry;
import gpu_cull;
import material_types_generated;  // DrawArgs（PUSH_DATA token size 契约）

namespace RGL {

// 构造器 / dtor / bindInitialShaders / recordExecute 实现（契约见接口声明）。
// VU 审查基准 = 本地 Vulkan-Docs（v1.4.360）chapters/device_generated_commands +
// 系统头 v357 字段（docs/device_generated_commands_research.md 逐条对照，5d 入册）。

DgcCommands::DgcCommands(RGL::DeviceContext& vkDevice, const RGL::ShaderRegistry& registry,
	uint32_t vkMaxSequenceCount, uint32_t framesInFlight)
	: pfnCreateLayout(reinterpret_cast<PFN_vkCreateIndirectCommandsLayoutEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkCreateIndirectCommandsLayoutEXT"))),
	  pfnDestroyLayout(reinterpret_cast<PFN_vkDestroyIndirectCommandsLayoutEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkDestroyIndirectCommandsLayoutEXT"))),
	  pfnCreateSet(reinterpret_cast<PFN_vkCreateIndirectExecutionSetEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkCreateIndirectExecutionSetEXT"))),
	  pfnDestroySet(reinterpret_cast<PFN_vkDestroyIndirectExecutionSetEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkDestroyIndirectExecutionSetEXT"))),
	  pfnUpdateSetShader(reinterpret_cast<PFN_vkUpdateIndirectExecutionSetShaderEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkUpdateIndirectExecutionSetShaderEXT"))),
	  pfnGetMemReq(reinterpret_cast<PFN_vkGetGeneratedCommandsMemoryRequirementsEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkGetGeneratedCommandsMemoryRequirementsEXT"))),
	  pfnCmdExecute(reinterpret_cast<PFN_vkCmdExecuteGeneratedCommandsEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkCmdExecuteGeneratedCommandsEXT"))),
	  device(*vkDevice.getDevice()),
	  seqCapacity(vkMaxSequenceCount) {
	// 入口点缺失 = 驱动声明了 feature 却没有实现（spec 违例）——fail-fast 回落重放路
	if (!pfnCreateLayout || !pfnDestroyLayout || !pfnCreateSet || !pfnDestroySet ||
	    !pfnUpdateSetShader || !pfnGetMemReq || !pfnCmdExecute)
		throw std::runtime_error("DgcCommands: DGC entry point missing from driver (feature/impl drift)");
	const auto& entries = registry.entriesView();
	const uint32_t capacity = static_cast<uint32_t>(entries.size()) * 2;
	// 容量契约：execution set ≤ 设备上限（超限 = 场景管线组合过多，回落重放路）
	const auto& dgc = vkDevice.getDgcSupport();
	if (capacity > dgc.maxIndirectShaderObjectCount)
		throw std::runtime_error(fmt::format(
			"DgcCommands: capacity {} exceeds maxIndirectShaderObjectCount {}",
			capacity, dgc.maxIndirectShaderObjectCount));

	// ① layout（3 token；TOC 处理序 = 序列偏移升序 = state token 先于 draw token）
	VkIndirectCommandsExecutionSetTokenEXT execToken{};
	execToken.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_SHADER_OBJECTS_EXT;
	execToken.shaderStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	VkIndirectCommandsPushConstantTokenEXT pushToken{};
	// VUID-VkIndirectCommandsLayoutTokenEXT-type-11333：PUSH_DATA 的 updateRange
	// .shaderStages 必须 VK_SHADER_STAGE_ALL。updateRange 描述**目标 shader push 区**
	// 的落点（spec "push constant range that will be updated"），非序列源偏移——
	// DrawArgs 在 shader 侧 @0，offset 必须 0（序列源 = token.offset 起 sizeof 字节）。
	// Phase 7b+：DrawArgs 已瘦至 objectIndex（4B）——NV 驱动 DGC push token 在
	// "push 字节数 × 序列数"较大时命令生成阶段挂死（详见 docs/调试历史 2026-09-10）
	pushToken.updateRange = {VK_SHADER_STAGE_ALL, 0, static_cast<uint32_t>(sizeof(RGL::DrawArgs))};
	VkIndirectCommandsLayoutTokenEXT tokens[3]{};
	tokens[0].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
	tokens[0].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT;
	tokens[0].data.pExecutionSet = &execToken;
	tokens[0].offset = 0;  // 2×u32：frag@0（FRAGMENT = LSB）/ mesh@4
	tokens[1].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
	tokens[1].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_DATA_EXT;
	tokens[1].data.pPushConstant = &pushToken;
	tokens[1].offset = 8;  // DrawArgs（descriptor heap push 区，Phase 7b+ 起 4B）
	tokens[2].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
	tokens[2].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT;
	tokens[2].offset = 108;  // VkDrawMeshTasksIndirectCommandEXT 12B
	VkIndirectCommandsLayoutCreateInfoEXT layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT;
	layoutInfo.flags = 0;
	layoutInfo.shaderStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutInfo.indirectStride = kCullSequenceStride;  // 120（序列步长单一真相源 = gpu_cull）
	layoutInfo.pipelineLayout = VK_NULL_HANDLE;  // descriptor heap 模式允许 null（spec :119）
	layoutInfo.tokenCount = 3;
	layoutInfo.pTokens = tokens;
	if (pfnCreateLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		throw std::runtime_error("DgcCommands: vkCreateIndirectCommandsLayoutEXT failed");

	// ② execution set（SHADER_OBJECTS；容量 = 2×条目；2j = entry j 的 frag、2j+1 = mesh——
	// 与序列 execSlot 约定（writeSequenceStaging）同源）
	// 两条 VUID 联合约束（NVIDIA 实测 2026-09-10，RADV 走不到此路故此前从未暴露）：
	//   - shaderCount-arraylength：初始 shader 数必须 >0（不允许空建）
	//   - stage-11023：pInitialShaders 各元素 stage 必须唯一（不能逐条目重复 mesh/frag）
	// → 初始 = 首条目 frag+mesh 一对（即"初始 shader 态"，execute 前绑到 CB 满足 11053），
	//   全 2N 槽建好后 vkUpdateIndirectExecutionSetEXT 一次写入
	if (entries.empty())
		throw std::runtime_error("DgcCommands: no pipeline entries (execution set needs initial shaders)");
	std::vector<VkShaderEXT> initialShaders{
		entries[0].pipeline.fragShaderHandle(), entries[0].pipeline.meshShaderHandle()};
	VkIndirectExecutionSetShaderInfoEXT shaderInfo{};
	shaderInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_SHADER_INFO_EXT;
	shaderInfo.shaderCount = static_cast<uint32_t>(initialShaders.size());
	shaderInfo.pInitialShaders = initialShaders.data();
	shaderInfo.pSetLayoutInfos = nullptr;
	shaderInfo.maxShaderCount = capacity;
	shaderInfo.pushConstantRangeCount = 0;
	shaderInfo.pPushConstantRanges = nullptr;  // push 经 PUSH_DATA token 的 updateRange
	VkIndirectExecutionSetCreateInfoEXT setInfo{};
	setInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT;
	setInfo.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_SHADER_OBJECTS_EXT;
	setInfo.info.pShaderInfo = &shaderInfo;
	const VkResult setResult = pfnCreateSet(device, &setInfo, nullptr, &executionSet);
	if (setResult != VK_SUCCESS)
		throw std::runtime_error(fmt::format(
			"DgcCommands: vkCreateIndirectExecutionSetEXT failed: {} (shaderCount={} maxShaderCount={})",
			vk::to_string(static_cast<vk::Result>(setResult)), shaderInfo.shaderCount, shaderInfo.maxShaderCount));
	std::vector<VkWriteIndirectExecutionSetShaderEXT> writes;
	writes.reserve(capacity);
	for (uint32_t j = 0; j < entries.size(); ++j) {
		writes.push_back({VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_SHADER_EXT, nullptr,
			2 * j, entries[j].pipeline.fragShaderHandle()});
		writes.push_back({VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_SHADER_EXT, nullptr,
			2 * j + 1, entries[j].pipeline.meshShaderHandle()});
	}
	pfnUpdateSetShader(device, executionSet, static_cast<uint32_t>(writes.size()), writes.data());
	initialShaders_[0] = initialShaders[0];
	initialShaders_[1] = initialShaders[1];

	// ③ preprocess buffer（内存需求按 layout+execution set+maxSequenceCount 查询；
	// memoryTypeBits 严格匹配——VMA 自动选型不保证满足驱动要求位）
	VkGeneratedCommandsMemoryRequirementsInfoEXT memReq{};
	memReq.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT;
	memReq.indirectExecutionSet = executionSet;
	memReq.indirectCommandsLayout = layout;
	memReq.maxSequenceCount = seqCapacity;
	memReq.maxDrawCount = seqCapacity;  // 每序列 1 个 DRAW_MESH_TASKS
	VkMemoryRequirements2 req{};
	req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
	pfnGetMemReq(device, &memReq, &req);
	preprocessSize = std::max(req.memoryRequirements.size, VkDeviceSize(1));
	// 每帧槽步长 = size 上对齐到 alignment；×framesInFlight 槽（11142：preprocess 区
	// 不得被一条以上 execute 复用——隐式路每帧 execute 都写它，跨在途帧必须分开）
	const VkDeviceSize align = std::max(req.memoryRequirements.alignment, VkDeviceSize(1));
	preprocessSlotStride = (preprocessSize + align - 1) / align * align;
	preprocessSlots = framesInFlight == 0 ? 1 : framesInFlight;
	const VkDeviceSize totalPreprocessSize = preprocessSlotStride * preprocessSlots;
	const auto memProps = vkDevice.getPhysicalDevice().getMemoryProperties();
	uint32_t memType = std::numeric_limits<uint32_t>::max();
	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
		if ((req.memoryRequirements.memoryTypeBits >> i) & 1) {
			memType = i;
			break;  // 首个满足位（preprocess 缓冲无性能敏感路径，不择优）
		}
	if (memType == std::numeric_limits<uint32_t>::max())
		throw std::runtime_error("DgcCommands: preprocess memory requirements match no memory type");
	// preprocess 工作缓冲（隐式 preprocess——execute isPreprocessed=FALSE 时驱动内部
	// 读写此区）：usage 含 bit-31 的 PREPROCESS_BUFFER 必须走 flags2 pNext
	// （VUID-VkBufferCreateInfo-pNext-09040：usage 须为 0）
	VkBufferUsageFlags2CreateInfoKHR bufUsage2{};
	bufUsage2.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR;
	bufUsage2.usage = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT |
		VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;  // BDA 查询需 DEVICE_ADDRESS（02601）
	VkBufferCreateInfo bufInfo{};
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.pNext = &bufUsage2;
	bufInfo.size = totalPreprocessSize;
	if (vkCreateBuffer(device, &bufInfo, nullptr, &preprocessBuffer) != VK_SUCCESS)
		throw std::runtime_error("DgcCommands: preprocess vkCreateBuffer failed");
	// 带 DEVICE_ADDRESS usage 的 buffer，内存须 DEVICE_ADDRESS 分配位（VUID-03339，BDA）
	const VkMemoryAllocateFlagsInfo allocFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
		nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
	const VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &allocFlags,
		totalPreprocessSize, memType};
	if (vkAllocateMemory(device, &allocInfo, nullptr, &preprocessMem) != VK_SUCCESS)
		throw std::runtime_error("DgcCommands: preprocess vkAllocateMemory failed");
	vkBindBufferMemory(device, preprocessBuffer, preprocessMem, 0);
	const VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		nullptr, preprocessBuffer};
	preprocessAddress = vkGetBufferDeviceAddress(device, &bda);  // core 1.2（loader 导出）
	if (auto logger = RLLogger::getInstance())
		logger->info("dgc: layout + execution set (capacity {}) + preprocess buffer ({}B ×{} slots) ready",
			capacity, preprocessSize, preprocessSlots);
}

DgcCommands::~DgcCommands() {
	if (preprocessBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, preprocessBuffer, nullptr);
		preprocessBuffer = VK_NULL_HANDLE;
	}
	if (preprocessMem != VK_NULL_HANDLE) {
		vkFreeMemory(device, preprocessMem, nullptr);
		preprocessMem = VK_NULL_HANDLE;
	}
	if (executionSet != VK_NULL_HANDLE) {
		pfnDestroySet(device, executionSet, nullptr);
		executionSet = VK_NULL_HANDLE;
	}
	if (layout != VK_NULL_HANDLE) {
		pfnDestroyLayout(device, layout, nullptr);
		layout = VK_NULL_HANDLE;
	}
}

void DgcCommands::bindInitialShaders(vk::raii::CommandBuffer& cmd) const {
	// VUID-vkCmdExecuteGeneratedCommandsEXT-indirectCommandsLayout-11053：含
	// EXECUTION_SET token 的布局执行前，execution set 的初始 shader 态必须已绑于该 CB
	// （每序列 token 随后逐 draw 覆盖为各自管线）。stages 与 pInitialShaders 序一致：
	// slot0=frag、slot1=mesh
	std::array<vk::ShaderStageFlagBits, 2> stages = {
		vk::ShaderStageFlagBits::eFragment, vk::ShaderStageFlagBits::eMeshEXT};
	std::array<vk::ShaderEXT, 2> handles = {
		vk::ShaderEXT(initialShaders_[0]), vk::ShaderEXT(initialShaders_[1])};
	cmd.bindShadersEXT(stages, handles);
}

void DgcCommands::recordExecute(vk::raii::CommandBuffer& cmd, VkDeviceAddress sequenceAddress,
	VkDeviceAddress sequenceCountAddress, uint32_t frameSlot) const {
	VkGeneratedCommandsInfoEXT info{};
	info.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
	info.shaderStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	info.indirectExecutionSet = executionSet;
	info.indirectCommandsLayout = layout;
	info.indirectAddress = sequenceAddress;
	info.indirectAddressSize = static_cast<VkDeviceSize>(seqCapacity) * kCullSequenceStride;
	info.preprocessAddress = preprocessAddress + preprocessSlotStride * frameSlot;
	info.preprocessSize = preprocessSize;
	info.maxSequenceCount = seqCapacity;
	info.sequenceCountAddress = sequenceCountAddress;  // count buffer @4 = candidate 数（GPU 写）
	info.maxDrawCount = seqCapacity;
	pfnCmdExecute(*cmd, VK_FALSE, &info);  // 隐式 preprocess（驱动内部生成+同步）
}

// ─── DgcStaticCommands（spike：docs/radv_static_dgc_spike.md）─────────────────

DgcStaticCommands::DgcStaticCommands(RGL::DeviceContext& vkDevice, const RGL::ShaderRegistry& registry,
	uint32_t vkMaxSequenceCount, uint32_t vkEntryCount, uint32_t framesInFlight)
	: pfnCreateLayout(reinterpret_cast<PFN_vkCreateIndirectCommandsLayoutEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkCreateIndirectCommandsLayoutEXT"))),
	  pfnDestroyLayout(reinterpret_cast<PFN_vkDestroyIndirectCommandsLayoutEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkDestroyIndirectCommandsLayoutEXT"))),
	  pfnGetMemReq(reinterpret_cast<PFN_vkGetGeneratedCommandsMemoryRequirementsEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkGetGeneratedCommandsMemoryRequirementsEXT"))),
	  pfnCmdExecute(reinterpret_cast<PFN_vkCmdExecuteGeneratedCommandsEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkCmdExecuteGeneratedCommandsEXT"))),
	  device(*vkDevice.getDevice()),
	  seqCapacity(vkMaxSequenceCount) {
	if (!pfnCreateLayout || !pfnDestroyLayout || !pfnGetMemReq || !pfnCmdExecute)
		throw std::runtime_error("DgcStaticCommands: DGC entry point missing from driver (feature/impl drift)");
	const auto& entries = registry.entriesView();
	if (entries.empty() || vkEntryCount == 0 || vkEntryCount > entries.size())
		throw std::runtime_error(fmt::format(
			"DgcStaticCommands: bad entryCount {} (registry has {})", vkEntryCount, entries.size()));
	if (seqCapacity == 0)
		throw std::runtime_error("DgcStaticCommands: zero sequence capacity");

	// 逐条目 {frag, mesh} 句柄（execute pNext 静态 shader 态；C3 段后的条目全为 mesh 管线）
	for (uint32_t j = 0; j < vkEntryCount; ++j)
		entryShaders.push_back({entries[j].pipeline.fragShaderHandle(),
			entries[j].pipeline.meshShaderHandle()});

	// ① layout（2 token，对比 NV IES 路去 @0 EXECUTION_SET；VU-11091：shaderStages
	// 只要求 ⊆ supportedIndirectCommandsShaderStages **全集**——RADV 14 stage 含 MESH）
	VkIndirectCommandsPushConstantTokenEXT pushToken{};
	// VU-11333 同 NV：updateRange 描述目标 shader push 区落点（DrawArgs shader 侧 @0）
	pushToken.updateRange = {VK_SHADER_STAGE_ALL, 0, static_cast<uint32_t>(sizeof(RGL::DrawArgs))};
	VkIndirectCommandsLayoutTokenEXT tokens[2]{};
	tokens[0].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
	tokens[0].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_DATA_EXT;
	tokens[0].data.pPushConstant = &pushToken;
	tokens[0].offset = 8;  // DrawArgs（descriptor heap push 区，4B）
	tokens[1].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
	tokens[1].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_MESH_TASKS_EXT;
	tokens[1].offset = 108;  // VkDrawMeshTasksIndirectCommandEXT 12B
	VkIndirectCommandsLayoutCreateInfoEXT layoutInfo{};
	layoutInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT;
	layoutInfo.flags = 0;
	layoutInfo.shaderStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	layoutInfo.indirectStride = kCullSequenceStride;
	layoutInfo.pipelineLayout = VK_NULL_HANDLE;  // descriptor heap 模式允许 null
	layoutInfo.tokenCount = 2;
	layoutInfo.pTokens = tokens;
	if (pfnCreateLayout(device, &layoutInfo, nullptr, &layout) != VK_SUCCESS)
		throw std::runtime_error("DgcStaticCommands: vkCreateIndirectCommandsLayoutEXT failed");

	// ② preprocess 内存需求（IES=NULL + pNext 条目 0 ShaderInfo——VU-11012 对查询生效；
	// 逐条目 layout 相同、尺寸一致，取条目 0 的查询值）
	VkGeneratedCommandsShaderInfoEXT memShaderInfo{};
	memShaderInfo.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_SHADER_INFO_EXT;
	memShaderInfo.shaderCount = 2;
	memShaderInfo.pShaders = entryShaders[0].data();
	VkGeneratedCommandsMemoryRequirementsInfoEXT memReq{};
	memReq.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT;
	memReq.pNext = &memShaderInfo;
	memReq.indirectCommandsLayout = layout;
	memReq.indirectExecutionSet = VK_NULL_HANDLE;  // VU-11011：无 EXECUTION_SET token → NULL
	memReq.maxSequenceCount = seqCapacity;
	memReq.maxDrawCount = seqCapacity;  // 每序列 1 个 DRAW_MESH_TASKS
	VkMemoryRequirements2 req{};
	req.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2;
	pfnGetMemReq(device, &memReq, &req);
	preprocessSize = std::max(req.memoryRequirements.size, VkDeviceSize(1));
	// 槽 = entryCount × framesInFlight（VU-11142：每次 execute 独占 preprocess 区，
	// 帧内 N 次 + 跨在途帧不得重叠；槽号 = entry×FIS + frameSlot 与 recordExecute 同式）
	const VkDeviceSize align = std::max(req.memoryRequirements.alignment, VkDeviceSize(1));
	preprocessSlotStride = (preprocessSize + align - 1) / align * align;
	framesPerEntry = framesInFlight == 0 ? 1 : framesInFlight;
	preprocessSlots = vkEntryCount * framesPerEntry;
	const VkDeviceSize totalPreprocessSize = preprocessSlotStride * preprocessSlots;
	// VU-11064：内存必须来自 memReq.memoryTypeBits（VMA 自动选型不保证 → 原生分配，
	// DgcCommands 同款）
	const auto memProps = vkDevice.getPhysicalDevice().getMemoryProperties();
	uint32_t memType = std::numeric_limits<uint32_t>::max();
	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
		if ((req.memoryRequirements.memoryTypeBits >> i) & 1) {
			memType = i;
			break;
		}
	if (memType == std::numeric_limits<uint32_t>::max())
		throw std::runtime_error("DgcStaticCommands: preprocess memory requirements match no memory type");
	VkBufferUsageFlags2CreateInfoKHR bufUsage2{};
	bufUsage2.sType = VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR;
	bufUsage2.usage = VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT |
		VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT;
	VkBufferCreateInfo bufInfo{};
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.pNext = &bufUsage2;
	bufInfo.size = totalPreprocessSize;
	if (vkCreateBuffer(device, &bufInfo, nullptr, &preprocessBuffer) != VK_SUCCESS)
		throw std::runtime_error("DgcStaticCommands: preprocess vkCreateBuffer failed");
	const VkMemoryAllocateFlagsInfo allocFlags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
		nullptr, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
	const VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &allocFlags,
		totalPreprocessSize, memType};
	if (vkAllocateMemory(device, &allocInfo, nullptr, &preprocessMem) != VK_SUCCESS)
		throw std::runtime_error("DgcStaticCommands: preprocess vkAllocateMemory failed");
	vkBindBufferMemory(device, preprocessBuffer, preprocessMem, 0);
	const VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		nullptr, preprocessBuffer};
	preprocessAddress = vkGetBufferDeviceAddress(device, &bda);
	if (auto logger = RLLogger::getInstance())
		logger->info("dgc static: layout (2 token) + preprocess buffer ({}B ×{} slots = {} entries ×{} frames) ready",
			preprocessSize, preprocessSlots, vkEntryCount, framesInFlight);
}

DgcStaticCommands::~DgcStaticCommands() {
	if (preprocessBuffer != VK_NULL_HANDLE) {
		vkDestroyBuffer(device, preprocessBuffer, nullptr);
		preprocessBuffer = VK_NULL_HANDLE;
	}
	if (preprocessMem != VK_NULL_HANDLE) {
		vkFreeMemory(device, preprocessMem, nullptr);
		preprocessMem = VK_NULL_HANDLE;
	}
	if (layout != VK_NULL_HANDLE) {
		pfnDestroyLayout(device, layout, nullptr);
		layout = VK_NULL_HANDLE;
	}
}

void DgcStaticCommands::recordExecute(vk::raii::CommandBuffer& cmd, uint32_t entryIndex,
	VkDeviceAddress streamAddress, VkDeviceAddress sequenceCountAddress,
	uint32_t frameSlot) const {
	// 先绑本条目 frag+mesh shader object（NV 路 bindInitialShaders 同款思路）：
	// VUID-vkCmdExecuteGeneratedCommandsEXT-None-11154/11155——execute 时 CB 必须处于
	// "graphics pipeline bound 或 shader object 已绑"态（NV 驱动不校验此条故 IES 路
	// 仅按 11053 绑初始态即可；RADV 走 VVL 全量校验）。绑定与本条目 pNext 静态 shader
	// 同对象，语义自洽
	// VVL 实测（RADV 2026-09-10）：mesh 管线 execute 还要求 vertex/task 阶段被显式
	// VK_NULL_HANDLE 绑过（"Even if you are using a mesh shader, a VK_NULL_HANDLE must
	// be bound"）——四阶段全绑，null 项清阶段
	std::array<vk::ShaderStageFlagBits, 4> stages = {
		vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eTaskEXT,
		vk::ShaderStageFlagBits::eFragment, vk::ShaderStageFlagBits::eMeshEXT};
	std::array<vk::ShaderEXT, 4> handles = {
		vk::ShaderEXT(nullptr), vk::ShaderEXT(nullptr),
		vk::ShaderEXT(entryShaders[entryIndex][0]), vk::ShaderEXT(entryShaders[entryIndex][1])};
	cmd.bindShadersEXT(stages, handles);
	// 静态 shader 态（VU-11080：IES NULL → pNext 必带 Pipeline/Shader Info；
	// RADV radv_dgc_get_shader(pipeline_info, eso_info, …) 正读此 pNext）
	VkGeneratedCommandsShaderInfoEXT shaderInfo{};
	shaderInfo.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_SHADER_INFO_EXT;
	shaderInfo.shaderCount = 2;
	shaderInfo.pShaders = entryShaders[entryIndex].data();  // {frag, mesh}
	VkGeneratedCommandsInfoEXT info{};
	info.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
	info.pNext = &shaderInfo;
	info.shaderStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	info.indirectExecutionSet = VK_NULL_HANDLE;  // VU-10241：无 EXECUTION_SET token → NULL
	info.indirectCommandsLayout = layout;
	info.indirectAddress = streamAddress;  // 该条目流（VU-12407 INDIRECT_BUFFER usage 在 GpuCull 侧置位）
	info.indirectAddressSize = static_cast<VkDeviceSize>(seqCapacity) * kCullSequenceStride;
	info.preprocessAddress = preprocessAddress +
		preprocessSlotStride * (static_cast<VkDeviceSize>(entryIndex * framesPerEntry + frameSlot));
	info.preprocessSize = preprocessSize;
	info.maxSequenceCount = seqCapacity;
	info.sequenceCountAddress = sequenceCountAddress;  // count buffer @4 = candidate 数（全条目共用）
	info.maxDrawCount = seqCapacity;
	pfnCmdExecute(*cmd, VK_FALSE, &info);  // 隐式 preprocess
}

}  // namespace RGL
