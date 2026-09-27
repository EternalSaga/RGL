module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）
export module picking;

import rlstd;
import rlvk;
import VmaRaii;  // VmaBuffer（staging 成员类型）
import vk_resources;
import render_target_cache;
import render_graph;

namespace RGL {

/** object-id attachment 名（decoratePickingPass 注图的名字 = 本服务 RT/slot 反查键）。 */
export inline constexpr std::string_view kObjectIdAttachmentName = "object_id";

/**
 * GPU id 点选取参服务（Phase 10 批 10.5，D3；docs/phase10_plan.md §7）。
 *
 * 三步曲复用 frame_capture 先例（同帧边界时序契约）：request（输入侧挂坐标请求）→
 * record（帧末捕获段位录 1-texel 回读拷贝：object_id RT 该像素 → staging u32）→
 * collect（submit 后等本帧 fence 读回）。picking pass 以 objectIndex+1 写 r32ui
 * （0 = 未命中），本服务负责 +1 还原：take() 语义 = objectSlot（MeshRef.objectSlot
 * 同值），entity 反查在 ECS 侧（本模块零 entt）。
 *
 * 装配 = main 门控（diagnostics.picking，默认关 = 不构造，Renderer 零指针零开销）；
 * 请求坐标 = 窗口像素（越界 record 期 clamp 到 RT 尺寸）。每请求一次 = 一次 fence
 * 停顿（调试交互一次性语义，同 llm_debug 截图纪律）。
 */
export class PickingService {
   public:
	/**
	 * 构造并定位 object_id attachment slot（图未装饰 = 无此附件，record 期 warn 一次）。
	 * @param resources 资源层（staging 分配器）
	 * @param rts offscreen RT 缓存（object_id RT find）
	 * @param graph 编译渲染图（附件名 → slot 反查，布局追踪表索引）
	 */
	PickingService(RGL::Resources& resources, RGL::RenderTargetCache& rts,
		const RGL::CompiledRenderGraph& graph);

	/**
	 * 挂点选请求（输入侧/面板调用）：下一 record 帧回读该像素的 object id。
	 * 重复请求 = 覆盖（一次性语义：上一次请求若未录出即作废——同帧连点取最后）。
	 * @param x 窗口像素列（clamp 到 RT 宽度）
	 * @param y 窗口像素行（clamp 到 RT 高度）
	 */
	void request(uint32_t x, uint32_t y) {
		pendingX = x;
		pendingY = y;
		hasRequest = true;
	}

	/**
	 * 帧末捕获段录制（调用点 = drawFrame capturer.record 之后、presentTransition 前）：
	 * 请求存在时录 object_id RT 1-texel → staging 拷贝。布局：picking pass 末 =
	 * ColorAttachmentOptimal → TransferSrcOptimal 并**保持**（object_id 帧内无后续
	 * 消费者；下帧 picking pass 的 barrier oldLayout 从追踪表派生自动衔接）。
	 * @param cmd 主命令缓冲（录制期）
	 * @param layouts 附件布局追踪表（读写 object_id slot 项）
	 */
	void record(vk::raii::CommandBuffer& cmd, std::vector<vk::ImageLayout>& layouts);

	/**
	 * 收口（⑬ submit 后调用）：同步等本帧命令（含回读拷贝）完成后读 u32 raw id。
	 * 无请求/未录出 = no-op。
	 * @param device 逻辑设备（waitForFences）
	 * @param inFlightFence 本帧 inFlight fence 裸句柄
	 */
	void collect(vk::raii::Device& device, vk::Fence inFlightFence);

	/**
	 * 消费最近一次点选结果（ECS 侧帧首轮询）。
	 * @return objectSlot（raw−1）；nullopt = 无新结果或未命中（0 = 空像素）
	 */
	std::optional<uint32_t> take() {
		auto result = std::move(cachedResult);
		cachedResult = std::nullopt;
		return result;
	}

   private:
	/** staging 就绪（4B u32，GPU_TO_CPU 常驻映射，同 capture staging 反方向模式）。 */
	[[nodiscard]] bool ensureStaging();

	RGL::Resources& resources;
	RGL::RenderTargetCache& rts;
	const RGL::CompiledRenderGraph& graph;
	uint32_t objectIdSlot = kRgNoAttachment;  // 构造期定位（未装饰图 = 无附件）
	bool warnedMissing = false;               // RT 缺失 warn 只发一次
	bool hasRequest = false;                  // 请求 pending（record 消费）
	uint32_t pendingX = 0;
	uint32_t pendingY = 0;
	bool armed = false;                       // 本请求帧已录拷贝（collect 搬移）
	std::optional<uint32_t> cachedResult;     // raw−1 = objectSlot（0 = 未命中 → nullopt）
	RGL::VmaBuffer staging;
	void* stagingMapped = nullptr;            // 常驻映射指针（createBuffer 不回传 mapped）
};

}  // namespace RGL
