module;
#include <vulkan/vulkan.h>  // VK_* 宏（VK_ERROR_DEVICE_LOST 等返回值比较）
export module frame_session;

import rlstd;
import rlvk;
import vk_device;
import vk_swapchain;
import vk_resources;
import vkerror;

namespace RGL {

/**
 * 帧生命周期会话（Phase 10 批次 0 手术 4：自 Renderer::drawFrame ①②/⑬⑭⑮ 摘出）。
 *
 * 持有"帧 slot"概念的全部资源：四套同步对象（双 fence 设计见 createSyncObjects 注释）、
 * 每帧 slot 一个的 primary 命令缓冲、当前 slot 游标与 acquire 到的后缓冲下标。
 * Renderer 由此退化为"录制内容"的执行器：beginFrame 取到可录制的命令缓冲 →
 * 数据通路固定段 + pass 循环 → submit/present/advance。
 * swapchain 失效（acquire/present out-of-date）的重建职责也在本类
 * （recreateSwapchainAndDepth：swapchain + 深度图 extent 同步重建）。
 * noPresent（diagnostics.lua）为真时：① 只等 inFlight fence（present fence 永不信号）、
 * ⑭ 跳 present——摘出前语义逐条保留。
 */
export class FrameSession {
   public:
	/** 双缓冲：最多 2 帧同时在飞（当前帧录制 + 上一帧 GPU 执行）。 */
	static constexpr uint32_t kFramesInFlight = 2;

	/** beginFrame 三态结果（旧 drawFrame ② 的 acquire 分类语义）。 */
	enum class FrameStart {
		began,     // 正常：命令缓冲已 begin，可录制
		skipped,   // acquire 100ms 超时（compositor 停发 present）：本帧不渲染，帧循环存活
		recreated, // out-of-date：已重建 swapchain+depth，本帧跳过（下帧重来）
	};

	/**
	 * 构造即建同步对象与命令缓冲（Renderer 构造末站调用，设备层已就绪）。
	 * @param vkDevice 设备层（device/graphicsQueue）
	 * @param vkSwapchain 交换链（acquire/recreate/images）
	 * @param vkResources 资源层（深度图随 swapchain extent 同步重建）
	 * @param noPresent DIAG：true = 永不 present（见类注释）
	 */
	FrameSession(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain,
		RGL::Resources& vkResources, bool noPresent);

	/**
	 * ①②③：等上一帧同 slot 完全结束（双 fence，noPresent 只等 inFlight）→ reset →
	 * acquire 后缓冲（100ms 超时）→ 命令缓冲 reset+begin。
	 * @return began/skipped/recreated；fence wait 失败或 acquire 硬错误 → Error(DeviceLost/
	 *   SwapchainAcquireFailed)；out-of-date 重建深度失败 → Error
	 */
	std::expected<FrameStart, Error> beginFrame();

	/**
	 * ⑬ 提交主命令缓冲到图形队列（裸 vkQueueSubmit——RAII submit 重载不支持 fence）。
	 * wait = imageAvailable（acquire 信号），waitStage = 图派生 swapchain 首用阶段；
	 * signal = renderFinished；fence = inFlight。
	 * @param waitStage acquire 信号量的等待阶段（renderGraph.swapchainWaitStage）
	 * @return void；DEVICE_LOST → Error（不可恢复，调用方退出帧循环）；其余失败仅留
	 *   观测（fence 可能永不信号，下帧 ① 报错退出），返回成功
	 */
	std::expected<void, Error> submit(vk::PipelineStageFlags2 waitStage);

	/**
	 * ⑭ 上屏：等 renderFinished → presentKHR（SwapchainPresentFenceInfoEXT 挂
	 * present fence，VK_KHR_swapchain_maintenance1）。noPresent = 整体跳过。
	 * @return void；DEVICE_LOST → Error；out-of-date/suboptimal → 重建 swapchain+depth
	 *   （重建失败 → Error）；其余错误码留观测后返回成功（present fence 仍信号不死锁）
	 */
	std::expected<void, Error> present();

	/** ⑮ 推进到下一个帧 slot（双缓冲游标）。 */
	void advance();

	/** @return 本帧 slot 的命令缓冲（beginFrame 后、submit 前录制期使用） */
	vk::raii::CommandBuffer& commandBuffer() { return commandBuffers[currentFrame]; }
	/** @return 本帧 inFlight fence 裸句柄（cull submit / 捕获收口共用本帧 fence） */
	vk::Fence currentInFlightFence() const { return *inFlightFences[currentFrame]; }
	/** @return 本帧 slot 下标（staging ring / cull CB 索引） */
	uint32_t currentFrameSlot() const { return currentFrame; }
	/** @return 本帧 acquire 到的后缓冲下标（beginFrame began 后有效） */
	uint32_t imageIndex() const { return acquiredImageIndex; }
	/** @return 全局帧计数（诊断日志上下文；beginFrame 时自增，本帧号 = 自增前值） */
	uint64_t frameNumber() const { return currentFrameNumber; }

   private:
	/**
	 * 创建 kFramesInFlight 套同步对象（每个帧 slot 一套）。四个角色：
	 *   imageAvailableSemaphores — acquireNextImage 信号，queueSubmit 等待
	 *   renderFinishedSemaphores — queueSubmit 信号，presentKHR 等待
	 *   inFlightFences — 挂 queueSubmit，追踪"命令缓冲执行完"
	 *   presentFences — 经 SwapchainPresentFenceInfoEXT 挂 presentInfo.pNext，
	 *                   追踪"present 完成"（VK_KHR_swapchain_maintenance1）
	 * 为什么是双 fence：单 fence 只挂 queueSubmit 时，triple-buffer（KWin）下 fence
	 * 信号时 present 可能仍在异步等待，下一帧重用 semaphore 会违反 VUID-00067。
	 * 见 docs/技术概念.md 的"双 Fence"一节。
	 */
	void createSyncObjects();
	/** 批量分配 kFramesInFlight 个 primary command buffer（每帧 slot 一个，
	 * 每帧 reset() 后重新录制）。 */
	void allocateCommandBuffers();
	/**
	 * 窗口尺寸变化（resize）或 acquire/present 返回 out-of-date 时重建 swapchain +
	 * 深度图。深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。
	 * @param context 日志上下文（调用点标识，如 "acquire out-of-date"）
	 * @return void；recreate 后 extent 未变 = 深度图仍有效跳过重建（静默死循环只报错一次）；
	 *   深度重建失败 → Error
	 */
	[[nodiscard]] std::expected<void, Error> recreateSwapchainAndDepth(const char* context);

	RGL::DeviceContext& device;
	RGL::Swapchain* swapchain;
	RGL::Resources* resources;
	bool noPresent;
	uint32_t currentFrame = 0;        // 当前帧 slot（0/1；advance() 模 kFramesInFlight 推进）
	uint32_t acquiredImageIndex = 0;  // 本帧 acquire 的后缓冲下标
	uint64_t frameCount = 0;          // DIAG：device lost 帧号定位（beginFrame 自增）
	uint64_t currentFrameNumber = 0;  // 本帧号 = frameCount 自增前值（frameNumber() 暴露）
	std::vector<vk::raii::Semaphore> imageAvailableSemaphores;  // acquire 信号（submit 等待）
	std::vector<vk::raii::Semaphore> renderFinishedSemaphores;  // submit 信号（present 等待）
	std::vector<vk::raii::Fence> inFlightFences;  // 命令缓冲执行完（挂 queueSubmit）
	std::vector<vk::raii::Fence> presentFences;   // present 完成（presentInfo.pNext 挂）
	std::vector<vk::raii::CommandBuffer> commandBuffers;  // primary 命令缓冲（每帧 slot 一个）
};

}  // namespace RGL
