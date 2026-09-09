export module cull_driver;

import rlstd;
import rlvk;
import VmaRaii;
import vk_device;
import vk_resources;
import gpu_cull;
import vk_indirect_commands;
import shader_registry;
import frame_draw_list;
import vkerror;

namespace RGL {

/**
 * GPU culling 帧编舞者（Phase 10 批次 0 手术 3：自 Renderer::drawFrame ④c 段与
 * opaque 段双路提交摘出——Renderer 回归"盲执行器"，culling 的提交时序知识集中本类）。
 *
 * 持 GpuCull（数据通路：entry 表/序列/count/staging）与可选 DgcCommands（NVIDIA 路），
 * 负责三件事：④c 帧内 cull CB 录制→提交→同步等待→读回→CPU/GPU 对账；opaque 段
 * 双路提交（DGC execute / 读回重放）；对账日志节流。
 * gpuCull 为 null 或 inactive = 全路径不碰 culling（行为中性，与摘出前 Renderer 语义一致）。
 * 时序契约（摘出前即成立）：cull submit 与主 CB 共用本帧 inFlight fence——submit 信号后
 * 立即 reset（vkdQueueSubmit 要求 fence 非 signaled 态），主 CB 稍后重新挂同一 fence。
 */
export class CullDriver {
   public:
	/**
	 * 构造（main 先建 GpuCull/DgcCommands 再建 Renderer，本类在 Renderer 构造体内建）。
	 * @param vkDevice 设备层（device/graphicsQueue/graphicsCommandPool）
	 * @param vkGpuCull culling 数据通路（nullptr = culling 不存在）
	 * @param vkDgc DGC 命令生成（nullptr = 恒走读回重放；NVIDIA 机才有）
	 * @param framesInFlight 帧 slot 数（= Renderer::MAX_FRAMES_IN_FLIGHT，cull CB 按 slot 分配）
	 */
	CullDriver(RGL::DeviceContext& vkDevice, RGL::GpuCull* vkGpuCull, RGL::DgcCommands* vkDgc,
		uint32_t framesInFlight);

	/**
	 * culling 是否激活（null/inactive = false，drawFrame 全路径不碰 culling）。
	 * @return true = 有候选表且 GPU 表已建
	 */
	[[nodiscard]] bool active() const;

	/**
	 * ④c 帧内 culling 数据通路：cull CB 录制（staging 序列拷贝 + dispatch [+ DGC
	 * preprocess]）→ 提交 → 同帧同步等待 → 读回 → 对账。性能优化（去同步等待）=
	 * 后续注记，摘出保持行为不变。
	 * @param frameSlot 本帧 in-flight slot（cull CB / staging 索引）
	 * @param frame 全局帧计数（仅日志诊断上下文，与摘出前 "frame {}" 文案一致）
	 * @param list 每帧绘制列表（cullInput 序列计数 + cullArgs CPU 位级真相源）
	 * @param sceneStaging 本帧 scene staging（recordCullPass 读矩阵算 mvp）
	 * @param inFlightFence 本帧 inFlight fence 裸句柄（共用时序契约见类注释）
	 * @return void；submit/wait 失败 → Error（DeviceLost/Unknown，与摘出前一致）
	 */
	std::expected<void, Error> runFrame(uint32_t frameSlot, uint64_t frame,
		const FrameDrawList& list, RGL::VmaBuffer& sceneStaging, vk::Fence inFlightFence);

	/**
	 * opaque 段双路提交（pass 循环 drawSource=="opaque" 且 active 时调用）：
	 * DGC execute（NVIDIA 路，GPU 直接消费序列）或读回重放（RADV 路，逐 candidate
	 * 按可见位重放，args = CPU 位级真相源 → 截图 BIT-IDENTICAL）。
	 * @param cmd 主命令缓冲（录制期，已在 beginRendering 内）
	 * @param list 每帧绘制列表（cullArgs 重放参数源）
	 * @param registry 管线表（重放路径 bindShaders/pushDataSet 查找）
	 */
	void executeOpaqueSegment(vk::raii::CommandBuffer& cmd, const FrameDrawList& list,
		RGL::ShaderRegistry& registry);

   private:
	/**
	 * culling 独立命令缓冲分配（构造期，仅 active；inactive = 空）。与主 CB 分离的
	 * 原因 = 读回重放需要"提交 cull → 同步等 → 读回 → 再录 opaque 重放"的帧内停顿，
	 * 同一 CB 无法 end-后-续录。
	 */
	void allocateCommandBuffers();

	/**
	 * GPU/CPU 剔除结果对账：逐 candidate 可见集合 vs CPU opaque 段（objectSlot 多重集）
	 * 比对 + 对齐自检（cullArgs[i] ↔ entries[i] 同 objectSlot 顺序契约的运行时证明）；
	 * 计数/失配变化才打日志（静止不刷屏）。
	 * @param list 每帧绘制列表（CPU opaque 段 [0, skyboxStart)）
	 */
	void reconcile(const FrameDrawList& list);

	RGL::DeviceContext& device;
	RGL::GpuCull* gpuCull;
	RGL::DgcCommands* dgc;
	uint32_t framesInFlight;                    // cull CB 数（= Renderer 帧 slot 数）
	std::vector<vk::raii::CommandBuffer> cullCommandBuffers;  // 每帧 slot 一个（inactive = 空）
	uint32_t lastReconcileVisible = 0xFFFFFFFF;  // 对账日志节流（计数/失配变化才打）
	bool lastReconcileMatch = false;
};

}  // namespace RGL
