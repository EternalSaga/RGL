module;
#include <vulkan/vulkan.h>  // DGC C 结构体（v357 vulkan.hpp 无 vk:: 包装；C 头解析 ~0.4s）
export module vk_indirect_commands;

import rlstd;
import rlvk;
import rllogger;
import vk_device;
import shader_registry;
import gpu_cull;  // kCullSequenceStride（layout indirectStride / token 偏移同源）

namespace RGL {

/**
 * DGC（VK_EXT_device_generated_commands）命令生成子系统（Phase 7b 批次 5；**NVIDIA 路**——
 * main 仅在 DeviceContext::getDgcSupport().graphicsIndirect = true 时构造；RADV graphics
 * 能力 = false → 不构造，Renderer 走读回重放回退）。消费与 GpuCull 同一条 cull 序列
 *（CPU args + GPU 可见性，kCullSequenceStride 步长），执行集/序列地址/计数全经
 * VkGeneratedCommandsInfoEXT 随 execute 传递（v357 形态——无
 * vkCmdBindIndirectExecutionSet/SetSequenceAddress 命令，见 docs/device_generated_commands_research.md）。
 * 布局契约（layout TOC，indirectStride = kCullSequenceStride = 120，descriptor heap 下
 * pipelineLayout = null）：
 *   @0   EXECUTION_SET（shaderStages = MESH|FRAGMENT → 序列读 2 个连续 u32，
 *        LSB→MSB = frag@0 / mesh@4；execution set 下标 2j = 条目 j 的 frag、2j+1 = mesh，
 *        CPU writeSequenceStaging 按同约定写 execSlot）
 *   @8   PUSH_DATA（DrawArgs 4B = 仅 objectIndex，Phase 7b+ → 序列原始字节推入 shader push 区）
 *   @108 DRAW_MESH_TASKS（VkDrawMeshTasksIndirectCommandEXT；x=0 = 被剔 no-op）
 * 每帧单段（隐式 preprocess——isPreprocessed=FALSE 驱动内部完成生成+同步，显式两段路
 * 需为 preprocess 另备"录制中且绑好初始 shader 态"的 stateCommandBuffer，复杂度不值）：
 *   bindInitialShaders + execute：主 CB opaque pass 内（render pass 中）——
 *   VUID-11053 要求含 EXECUTION_SET token 的布局执行前，execution set 初始 shader 态
 *   （首条目 frag+mesh 对）已绑于该 CB；sequenceCountAddress = count buffer @4 =
 *   candidate 数（非紧凑，被剔序列 = x=0 no-op，仍消耗序列槽）
 */
export class DgcCommands {
   public:
	/**
	 * 构建 DGC 命令生成子系统（启动期一次性，管线表建完 + GpuCull 表定后）：
	 * layout（3 token）+ execution set（SHADER_OBJECTS，容量 = 2×管线表条目，初始态 =
	 * 首条目 frag+mesh，全槽 2j=frag / 2j+1=mesh 建后 update 写入）+ preprocess buffer
	 * （内存需求按 maxSequenceCount 查询，memoryTypeBits 严格匹配分配；**按帧槽 ×
	 * framesInFlight**——VUID-...-preprocessAddress-11142：同一 preprocess 区不得被
	 * 一条以上 execute 复用，隐式路每帧 execute 都写它）。
	 * @param device 设备层（layout/execution set 创建 + 内存查询 + physical device 内存属性）
	 * @param registry 管线表（execution set 容量与 shader 句柄来源）
	 * @param maxSequenceCount 序列容量（= GpuCull::candidateCount，非紧凑）
	 * @param framesInFlight preprocess buffer 槽数（= FrameSession::kFramesInFlight）
	 * @note 容量超 maxIndirectShaderObjectCount 抛 std::runtime_error（main 据此回落重放路）
	 */
	DgcCommands(RGL::DeviceContext& device, const RGL::ShaderRegistry& registry,
		uint32_t maxSequenceCount, uint32_t framesInFlight);
	/** 析构：销毁 preprocess buffer/内存 + execution set + layout（main 栈对象，析构先于 DeviceContext）。 */
	~DgcCommands();
	/** 禁止拷贝（GPU 资源句柄独占，main 栈对象唯一持有）。 */
	DgcCommands(const DgcCommands&) = delete;
	/** 禁止拷贝赋值（GPU 资源句柄独占，main 栈对象唯一持有）。 */
	DgcCommands& operator=(const DgcCommands&) = delete;
	/** @return 子系统是否就绪（layout 创建成功；Renderer 据此选 DGC execute / 读回重放）。 */
	[[nodiscard]] bool active() const { return layout != VK_NULL_HANDLE; }
	/** @return 序列容量（execute 的 maxSequenceCount；= 构造期 candidateCount）。 */
	[[nodiscard]] uint32_t maxSequenceCount() const { return seqCapacity; }
	/**
	 * 绑定 execution set 初始 shader 态（首条目 frag+mesh）到命令缓冲——
	 * recordExecute 前必调（VUID-vkCmdExecuteGeneratedCommandsEXT-...-11053）。
	 * @param cmd 命令缓冲（录制期）
	 */
	void bindInitialShaders(vk::raii::CommandBuffer& cmd) const;
	/**
	 * 录制 execute（主 CB，render pass 内——替换 opaque 段逐 draw 循环）。
	 * @param cmd 命令缓冲（录制期）
	 * @param sequenceAddress 序列 buffer device 地址
	 * @param sequenceCountAddress 计数 u32 device 地址（count buffer @4 = candidate 数，GPU 写）
	 * @param frameSlot 本帧 in-flight slot（选 preprocess 槽，11142 跨帧不重叠）
	 */
	void recordExecute(vk::raii::CommandBuffer& cmd, VkDeviceAddress sequenceAddress,
		VkDeviceAddress sequenceCountAddress, uint32_t frameSlot) const;

   private:
	// DGC 入口点运行时解析（loader 不导出设备级扩展全局符号——texture_upload
	// writeResourceDescriptorsEXT 同款先例：vkGetDeviceProcAddr；构造期一次性；
	// 7 入口点 = 隐式 preprocess 路所需，不含 vkCmdPreprocessGeneratedCommandsEXT）
	PFN_vkCreateIndirectCommandsLayoutEXT pfnCreateLayout = nullptr;
	PFN_vkDestroyIndirectCommandsLayoutEXT pfnDestroyLayout = nullptr;
	PFN_vkCreateIndirectExecutionSetEXT pfnCreateSet = nullptr;
	PFN_vkDestroyIndirectExecutionSetEXT pfnDestroySet = nullptr;
	PFN_vkUpdateIndirectExecutionSetShaderEXT pfnUpdateSetShader = nullptr;
	PFN_vkGetGeneratedCommandsMemoryRequirementsEXT pfnGetMemReq = nullptr;
	PFN_vkCmdExecuteGeneratedCommandsEXT pfnCmdExecute = nullptr;

	VkDevice device = VK_NULL_HANDLE;  // 原始句柄（dtor 销毁 layout/execution set/buffer）
	VkIndirectCommandsLayoutEXT layout = VK_NULL_HANDLE;
	VkIndirectExecutionSetEXT executionSet = VK_NULL_HANDLE;
	std::array<VkShaderEXT, 2> initialShaders_{};  // execution set 初始态（slot0=frag slot1=mesh）
	VkBuffer preprocessBuffer = VK_NULL_HANDLE;  // preprocess 工作缓冲（driver 读写，地址进 info）
	VkDeviceMemory preprocessMem = VK_NULL_HANDLE;
	VkDeviceAddress preprocessAddress = 0;
	VkDeviceSize preprocessSize = 0;
	VkDeviceSize preprocessSlotStride = 0;  // 单槽大小（= preprocessSize 对齐到 req.alignment）
	uint32_t preprocessSlots = 1;           // 槽数（= framesInFlight）
	uint32_t seqCapacity = 0;  // 序列容量（maxSequenceCount，构造期冻结）
};

}  // namespace RGL
