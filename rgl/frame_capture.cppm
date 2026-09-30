module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
export module frame_capture;

import rlstd;
import rlvk;
import VmaRaii;
import render_graph;
import vk_resources;
import render_target_cache;  // RenderTargetCache（架构债务续账步骤 2）

namespace RGL {

/**
 * 帧内捕获产物：请求帧的 drawFrame 在 present 前录一条回读拷贝（后缓冲，或
 * deferred G-buffer 第 N 号 RT = 诊断 dump_gbuf），submit 后同步等本帧 inFlight
 * fence 搬进结果——截图恒等于真实上屏内容（2026-09-10 Phase 10 批次 0 手术 2：
 * 自 Renderer 嵌套类型平迁入独立模块，Renderer 内保留同名 alias 供消费方）。
 */
export struct FrameCapture {
	uint32_t width = 0;
	uint32_t height = 0;
	vk::Format format = vk::Format::eUndefined;
	int gbufChannel = -1;          // >=0 = G-buffer 直读（dump_gbuf）；-1 = swapchain 后缓冲
	std::string attachmentName;    // 非空 = 按附件名捕获（object_id）；空 = 后缓冲 / G-buffer
	VkDeviceSize bytes = 0;
	std::vector<uint8_t> pixels;   // 原格式回读（行紧凑），消费方转 RGBA8
};

/**
 * 帧内回读捕获器（llm_debug 截图 / dump_gbuf 诊断的唯一载体）。
 *
 * 生命周期跨帧三步：request（drawFrame 前挂请求，一次性）→ record（请求帧 ⑫
 * presentTransition 前录回读拷贝，消费布局追踪表）→ collect（⑬ submit 后同步等
 * 本帧 inFlight fence、把 mapped 字节搬进结果——停顿只发生在请求帧，正常帧零影响）。
 * acquire 超时跳帧时请求保持 pending（record 未执行），消费方经 has() 决定重试。
 * 截图 = 真实上屏内容：与渲染图演进零分歧、零平行渲染器维护负担（原离屏重渲
 * captureFrame 已于批次 5 退役）。
 */
export class FrameCapturer {
   public:
	/**
	 * 构造并定位 swapchain attachment slot（捕获时读写其布局追踪值）。
	 * @param resources 资源层（staging 分配器）
	 * @param rts offscreen RT 缓存（RT 查找 find，架构债务续账步骤 2）
	 * @param graph 编译渲染图（附件名单：GB 通道名 → slot 反查）
	 */
	FrameCapturer(RGL::Resources& resources, RGL::RenderTargetCache& rts,
		const RGL::CompiledRenderGraph& graph);

	/**
	 * drawFrame 前调用（gbufChannel = diagnostics().dumpGbuf）；一次性——acquire 超时
	 * 跳帧时请求保持 pending，直到真正录制的帧消费。
	 * @param gbufChannel -1 = swapchain 后缓冲；>=0 = G-buffer 通道号（dump_gbuf，
	 *   deferred 图专属，前向图无对应 RT 时跳过并留 warn）
	 */
	void request(int gbufChannel = -1) {
		captureRequest = gbufChannel;
		captureRequestName.clear();  // 覆盖互斥：与 requestAttachment 二者只留其一
	}

	/**
	 * drawFrame 前调用（**按附件名**捕获，Phase 12 批 12.2）：目前唯一消费方是
	 * 自动化 QA 层要的 `object_id`（picking pass 产物，r32_uint）——把 picking 的
	 * 1-texel 回读扩成**全分辨率** dump，让图像断言有可精确解码的锚点（"该 primitive
	 * 覆盖了多少像素"），不必依赖人眼或机器学习。
	 *
	 * 与 `request` 互斥且**覆盖**语义一致（同帧连取最后一个）：置本请求时清掉
	 * `request` 的通道选择，反之亦然。
	 *
	 * 布局安全性：record 点在 `pickingRecord` **之前**（vkrenderer_passrec ⑪.5 vs
	 * picking 段），故 object_id 此刻仍是 picking pass 写完的
	 * `ColorAttachmentOptimal`，source 依赖取"颜色附件写"。摘录
	 * `PickingService::record` 留下的 `TransferSrcOptimal` 不会被本路径看到。
	 * @param name 编译后图里的附件名（如 `PickingService` 的 `kObjectIdAttachmentName`）；
	 *   图中无此附件 / RT 未建时 record 期 warn 一次并跳过（不静默出图）
	 */
	void requestAttachment(std::string_view name) {
		captureRequestName = name;
		captureRequest = -2;
	}

	/**
	 * drawFrame 后调用（fence 已在 collect 内等待）。
	 *
	 * **取走即清请求**（Phase 12 批 12.2 修的既有缺陷）：此前 `captureRequest` 挂上后
	 * 永不复位，`record` 每帧都录一条回读。llm_debug 路径靠"每帧重新 request"（ecs
	 * `requestBeforeDraw`）且截完即退，把这个问题盖住了；RPC `/shot` 让进程继续跑，
	 * 于是**第一张之后每帧都在做全分辨率回读**（1280×720 r32_uint ≈ 3.7MB/帧）。
	 * 清在 take 而非 record：record 未命中（acquire 跳帧）时请求须保持 pending 重试。
	 * `dump_gbuf` 的连续直读语义不受影响——它每帧重新 request。
	 * @return 本帧捕获产物；空 = 本帧无捕获产物
	 */
	std::optional<FrameCapture> take() {
		captureRequest = -2;
		captureRequestName.clear();
		return std::move(captureResult);
	}

	/**
	 * 本帧是否已产出捕获（acquire 超时跳帧时 drawFrame 不录拷贝、请求保持 pending，
	 * 消费方据此决定重试而非误读空）。
	 * @return true = 捕获产物就绪（take 可取）
	 */
	[[nodiscard]] bool has() const { return captureResult.has_value(); }

	/**
	 * ⑪.5：presentTransition 前录制回读拷贝（请求存在时）。后缓冲：追踪布局 →
	 * TransferSrc 拷贝 → 追踪表推进为 TransferSrc（⑫ 天然转 PresentSrc，无恢复
	 * barrier）；GB 通道（dump_gbuf）：帧末追踪布局（SRO）→ TS 拷贝 → 转回（追踪值不变）。
	 * @param cmd 主命令缓冲（录制期）
	 * @param swapchainImage 本帧后缓冲
	 * @param swapchainExtent 后缓冲尺寸
	 * @param swapchainFormat 后缓冲格式（captureBpp 输入）
	 * @param layouts 附件布局追踪表（读写：swapchainSlot / GB slot 的布局推进）
	 */
	void record(vk::raii::CommandBuffer& cmd, VkImage swapchainImage,
		vk::Extent2D swapchainExtent, vk::Format swapchainFormat,
		std::vector<vk::ImageLayout>& layouts);

	/**
	 * 捕获收口（⑬ submit 后调用）：同步等本帧命令（含回读拷贝）完成，把 mapped 字节
	 * 搬进结果。fence 不在此 reset（下帧 ① waitAll 对已信号 fence 立即通过 + 照常
	 * reset，语义不变）。无请求帧 = no-op。
	 * @param device 逻辑设备（waitForFences）
	 * @param inFlightFence 本帧 inFlight fence 裸句柄
	 */
	void collect(vk::raii::Device& device, vk::Fence inFlightFence);

   private:
	/**
	 * 捕获回读缓冲按需增长（GPU_TO_CPU + 常驻映射，同 scene staging 的创建模式反方向）；
	 * 重建安全：上一捕获的搬移在请求帧 drawFrame 内已完成（fence 已等），本函数只在
	 * 新拷贝录制前调用，无飞行中写冲突。
	 * @param bytes 所需字节数（w×h×captureBpp）
	 * @return true = 缓冲就绪（既有容量足够或已重建并自存 mapped 指针）；
	 *   false = VMA 分配失败（error 日志）
	 */
	[[nodiscard]] bool ensureCaptureStaging(VkDeviceSize bytes);

	RGL::Resources& resources;
	RGL::RenderTargetCache& rts;  // offscreen RT 缓存（find，架构债务续账步骤 2）
	const RGL::CompiledRenderGraph& graph;
	uint32_t swapchainSlot = kRgNoAttachment;  // 构造期从图定位（捕获读写其布局追踪值）
	int captureRequest = -2;                    // -2 = 无请求；-1 = 后缓冲；>=0 = GB 通道号
	std::string captureRequestName;             // 非空 = 按附件名捕获（优先于 captureRequest）
	bool captureArmed = false;                  // 本请求帧已录拷贝（submit 后等待搬移）
	FrameCapture captureInFlight;               // 录制期元数据（bytes/extent/format/channel）
	std::optional<FrameCapture> captureResult;  // take() 消费
	RGL::VmaBuffer captureStaging;              // host-visible 回读缓冲（按需增长重建）
	VkDeviceSize captureStagingSize = 0;
	void* captureMapped = nullptr;              // 常驻映射指针（createBuffer 不回传 mapped，随分配自存）
};

}  // namespace RGL
