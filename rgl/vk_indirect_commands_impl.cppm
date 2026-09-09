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

// 构造器 / dtor / recordPreprocess / recordExecute 实现（契约见接口声明）。
// VU 审查基准 = 本地 Vulkan-Docs（v1.4.360）chapters/device_generated_commands +
// 系统头 v357 字段（docs/device_generated_commands_research.md 逐条对照，5d 入册）。

DgcCommands::DgcCommands(RGL::DeviceContext& vkDevice, const RGL::ShaderRegistry& registry,
	uint32_t vkMaxSequenceCount)
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
	  pfnCmdPreprocess(reinterpret_cast<PFN_vkCmdPreprocessGeneratedCommandsEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkCmdPreprocessGeneratedCommandsEXT"))),
	  pfnCmdExecute(reinterpret_cast<PFN_vkCmdExecuteGeneratedCommandsEXT>(
		  vkGetDeviceProcAddr(*vkDevice.getDevice(), "vkCmdExecuteGeneratedCommandsEXT"))),
	  device(*vkDevice.getDevice()),
	  seqCapacity(vkMaxSequenceCount) {
	// 入口点缺失 = 驱动声明了 feature 却没有实现（spec 违例）——fail-fast 回落重放路
	if (!pfnCreateLayout || !pfnDestroyLayout || !pfnCreateSet || !pfnDestroySet ||
	    !pfnUpdateSetShader || !pfnGetMemReq || !pfnCmdPreprocess || !pfnCmdExecute)
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
	pushToken.updateRange = {VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT,
		8, static_cast<uint32_t>(sizeof(RGL::DrawArgs))};
	VkIndirectCommandsLayoutTokenEXT tokens[3]{};
	tokens[0].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
	tokens[0].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_EXECUTION_SET_EXT;
	tokens[0].data.pExecutionSet = &execToken;
	tokens[0].offset = 0;  // 2×u32：frag@0（FRAGMENT = LSB）/ mesh@4
	tokens[1].sType = VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT;
	tokens[1].type = VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_DATA_EXT;
	tokens[1].data.pPushConstant = &pushToken;
	tokens[1].offset = 8;  // DrawArgs 100B（descriptor heap push 区）
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
	VkIndirectExecutionSetShaderInfoEXT shaderInfo{};
	shaderInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_SHADER_INFO_EXT;
	shaderInfo.shaderCount = 0;
	shaderInfo.pInitialShaders = nullptr;
	shaderInfo.pSetLayoutInfos = nullptr;
	shaderInfo.maxShaderCount = capacity;
	shaderInfo.pushConstantRangeCount = 0;
	shaderInfo.pPushConstantRanges = nullptr;  // push 经 PUSH_DATA token 的 updateRange
	VkIndirectExecutionSetCreateInfoEXT setInfo{};
	setInfo.sType = VK_STRUCTURE_TYPE_INDIRECT_EXECUTION_SET_CREATE_INFO_EXT;
	setInfo.type = VK_INDIRECT_EXECUTION_SET_INFO_TYPE_SHADER_OBJECTS_EXT;
	setInfo.info.pShaderInfo = &shaderInfo;
	if (pfnCreateSet(device, &setInfo, nullptr, &executionSet) != VK_SUCCESS)
		throw std::runtime_error("DgcCommands: vkCreateIndirectExecutionSetEXT failed");
	std::vector<VkWriteIndirectExecutionSetShaderEXT> writes;
	writes.reserve(capacity);
	for (uint32_t j = 0; j < entries.size(); ++j) {
		writes.push_back({VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_SHADER_EXT, nullptr,
			2 * j, entries[j].pipeline.fragShaderHandle()});
		writes.push_back({VK_STRUCTURE_TYPE_WRITE_INDIRECT_EXECUTION_SET_SHADER_EXT, nullptr,
			2 * j + 1, entries[j].pipeline.meshShaderHandle()});
	}
	pfnUpdateSetShader(device, executionSet, static_cast<uint32_t>(writes.size()), writes.data());

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
	const auto memProps = vkDevice.getPhysicalDevice().getMemoryProperties();
	uint32_t memType = std::numeric_limits<uint32_t>::max();
	for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
		if ((req.memoryRequirements.memoryTypeBits >> i) & 1) {
			memType = i;
			break;  // 首个满足位（preprocess 缓冲无性能敏感路径，不择优）
		}
	if (memType == std::numeric_limits<uint32_t>::max())
		throw std::runtime_error("DgcCommands: preprocess memory requirements match no memory type");
	VkBufferCreateInfo bufInfo{};
	bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufInfo.size = preprocessSize;
	bufInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (vkCreateBuffer(device, &bufInfo, nullptr, &preprocessBuffer) != VK_SUCCESS)
		throw std::runtime_error("DgcCommands: preprocess vkCreateBuffer failed");
	const VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, nullptr,
		preprocessSize, memType};
	if (vkAllocateMemory(device, &allocInfo, nullptr, &preprocessMem) != VK_SUCCESS)
		throw std::runtime_error("DgcCommands: preprocess vkAllocateMemory failed");
	vkBindBufferMemory(device, preprocessBuffer, preprocessMem, 0);
	const VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
		nullptr, preprocessBuffer};
	preprocessAddress = vkGetBufferDeviceAddress(device, &bda);  // core 1.2（loader 导出）
	if (auto logger = RLLogger::getInstance())
		logger->info("dgc: layout + execution set (capacity {}) + preprocess buffer ({}B) ready",
			capacity, preprocessSize);
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

void DgcCommands::recordPreprocess(vk::raii::CommandBuffer& cmd, VkDeviceAddress sequenceAddress) const {
	VkGeneratedCommandsInfoEXT info{};
	info.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
	info.shaderStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	info.indirectExecutionSet = executionSet;
	info.indirectCommandsLayout = layout;
	info.indirectAddress = sequenceAddress;
	info.indirectAddressSize = static_cast<VkDeviceSize>(seqCapacity) * kCullSequenceStride;
	info.preprocessAddress = preprocessAddress;
	info.preprocessSize = preprocessSize;
	info.maxSequenceCount = seqCapacity;
	info.sequenceCountAddress = 0;  // preprocess 不消费计数（execute 才用）
	info.maxDrawCount = seqCapacity;
	pfnCmdPreprocess(*cmd, &info, VK_NULL_HANDLE);
}

void DgcCommands::recordExecute(vk::raii::CommandBuffer& cmd, VkDeviceAddress sequenceAddress,
	VkDeviceAddress sequenceCountAddress) const {
	VkGeneratedCommandsInfoEXT info{};
	info.sType = VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT;
	info.shaderStages = VK_SHADER_STAGE_MESH_BIT_EXT | VK_SHADER_STAGE_FRAGMENT_BIT;
	info.indirectExecutionSet = executionSet;
	info.indirectCommandsLayout = layout;
	info.indirectAddress = sequenceAddress;
	info.indirectAddressSize = static_cast<VkDeviceSize>(seqCapacity) * kCullSequenceStride;
	info.preprocessAddress = preprocessAddress;
	info.preprocessSize = preprocessSize;
	info.maxSequenceCount = seqCapacity;
	info.sequenceCountAddress = sequenceCountAddress;  // count buffer @4 = candidate 数（GPU 写）
	info.maxDrawCount = seqCapacity;
	pfnCmdExecute(*cmd, VK_TRUE, &info);
}

}  // namespace RGL
