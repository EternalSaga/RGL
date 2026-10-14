module;
#include <glm/glm.hpp>
export module izb_shadow;

import rlstd;
import rlvk;
import vk_device;
import vk_resources;
import VmaRaii;
import offgraph_sync;  // 批次 B1b：图外同步发射账（13 条 buffer barrier 由边表物化）

namespace RGL {

/**
 * IZB 每帧输入（批 11.3-1d；调用点从 FrameDrawList 取：skyInvViewProj + shadowViewProj[0]）。
 *
 * 光空间 = cascade-0 阴影 viewProj（正交 → 窗口 z 天然线性；与 CSM 光向同源，
 * 深度比较是仿射不变的 → 不需要 near/far，v1 语义见 docs/hfts_izb_plan.md §4.1-1d）。
 */
export struct IzbFrameInput {
	glm::mat4 invViewProj{1.0f};  ///< scene data @64（clip→world）
	glm::mat4 lightViewProj{1.0f}; ///< shadowViewProj[cascade0]（world→光 ndc）
};

/**
 * FTIZB 硬阴影的 IZB 数据层（批 11.3-1d；docs/hfts_izb_plan.md）。
 *
 * 图外 compute（cull/gpu_cull 编舞先例，render_graph 无 compute kind）：
 * 深度图（与 SSAO 共用 depthSampleSlot）→ 光空间 grid-of-lists（head/nodes，
 * 「地址即像素 ID」布局 = tests/test_izb_create.cppm 护栏同款）+ early-Z priming。
 *
 * 生命周期 = Renderer 的 deferred 路线 + shadow.lua 的 izb 开门时存在；
 * **门关 = 本对象根本不被构造**（逐位旁路红线：零分配零命令）。
 * 消费侧（光空间保守光栅 mask pass）= 1d 下半程；1d 本模块只建数据，画面不变。
 *
 * 描述符铁纪律（2026-10-01 用户拍板）：全部走 Resources 全量 descriptor heap
 * 用户槽（Runtime 段），老式 DescriptorPool 禁入（docs/DESCRIPTOR_HEAP_SPEC.md）。
 */
export class IzbShadow {
public:
	/**
	 * @param vkDevice 设备（shader object / barrier / fill 用 getDevice()）
	 * @param resources descriptor heap + depthSampleSlot（SSAO 同款深度采样描述符共享）
	 * @param extent 像素分辨率（深度图尺寸；resize 由调用方重建本对象）
	 * @param gridDim 光空间网格边长 G（shadow.lua 的 izb.grid）
	 * @param sync 图外同步发射账（批次 B1b：13 条 buffer barrier 的参数由 `offgraph_sync`
	 *        的 usage 词表物化，本模块只写边名 + buffer 句柄，不写 stage/access 字面量）
	 */
	IzbShadow(RGL::DeviceContext& vkDevice, RGL::Resources& resources,
		vk::Extent2D extent, uint32_t gridDim, OffGraphSync& sync);

	/**
	 * 图外 sandwich（录在 light pass 的 pass 循环迭代顶部）：
	 * 深度 layout→RO → FillBuffer 清 head → create dispatch → 链 barrier → prime
	 * dispatch → 深度 layout 还原（进出同值 = 渲染图布局追踪表不可见，零漂移）
	 * →（可选 dump 拷贝）→ **FillBuffer 清 mask（每帧无条件，债-32）**。
	 * mask 清零住这里是**刻意的**：本函数是 izbShadow 非空时每帧的唯一入口，而 mask pass
	 * （`beginMaskPass`）会被调用点整段跳过（shadow0 段为空）——清零跟着 pass 走 = 消费侧
	 * （light.frag 的 `izbShadowFactor`，它只看 izb flag）读到未初始化显存。
	 * @param cmd pass 循环所在命令缓冲（heap 已由 ⑦ 绑定）
	 * @param input 本帧矩阵（见 IzbFrameInput）
	 * @param depthCurrentLayout 深度图当前布局（追踪表读出，还原目标）
	 * @param dumpHead true = 本帧 head→staging 拷贝（下一帧 drainReadback 打日志）
	 * @return **离开时深度图真实所处的布局**（由实际发出的 barrier 逐跳推进，不是复述
	 *         入参）。调用方（`Renderer::izbLightPassPre`）拿它与渲染图布局追踪表比对，
	 *         不符即 error 出声 —— 批 14.4b A 档护栏，把「进出同值」从约定变断言。
	 *         逐帧的**图附件**布局 sandwich 全项目只有这一处（EVSSM 的 eUndefined→eGeneral
	 *         是建图期一次性、其余自发 barrier 模块只动自己的图不碰图附件），
	 *         所以本函数也是 14.5 FSR sandwich 要照抄的**唯一**先例。
	 */
	vk::ImageLayout recordFrame(vk::raii::CommandBuffer& cmd, const IzbFrameInput& input,
		vk::ImageLayout depthCurrentLayout, bool dumpHead);

	/** 上一帧 dump 的收割（帧首调用；fence 已过、映射可读）：无 pending 零开销。 */
	void drainReadback();

	/** mask 位图导出 PPM（批 11.3-1e 判据工具，dump 帧写 izb_mask.ppm；白=未遮
	 *  黑=遮——与 llm_debug 截图目检对位）。 */
	static void dumpMaskPpm(const uint32_t* words, uint32_t w, uint32_t h);

	/**
	 * 构造期钉死的像素分辨率（深度图尺寸 = swapchain extent；漂移由调用方重建）。
	 * @return 本实例的 extent
	 */
	[[nodiscard]] vk::Extent2D getExtent() const { return extent; }
	/** 光空间网格边长 G（mask pass viewport 同源）。 @return gridDim */
	[[nodiscard]] uint32_t getGridDim() const { return gridDim; }
	/** 光空间网格像素数 G²（beginRendering renderArea 用）。 @return G×G */
	[[nodiscard]] vk::Extent2D getGridExtent() const { return {gridDim, gridDim}; }

	/**
	 * mask pass 录制体（1d 后半；sandwich 内 create/prime 之后 begin，Renderer
	 * applyDynamicState + 重放 shadow0 段 draws，然后 end）。
	 * begin = head/nodes 的 compute→frag 读 barrier + beginRendering（**零附件**、
	 * renderArea = G×G）；end = endRendering + mask 写→读 barrier（light.frag 消费）。
	 * ⚠ mask 的每帧清零**不在本函数**（债-32）——本 pass 会被调用点整段跳过，
	 * 清零跟着 pass 走就等于「跳过 pass = 跳过清零 = shader 读未初始化显存」。
	 * @param cmd 命令缓冲（heap 已绑定）
	 */
	void beginMaskPass(vk::raii::CommandBuffer& cmd);
	/** mask pass 收尾（beginMaskPass 的配平 endRendering）。 */
	void endMaskPass(vk::raii::CommandBuffer& cmd);

private:
	/**
	 * 光空间数据两个 compute dispatch 的录制体（recordFrame 拆分；heap 已绑定前提）：
	 * create = 深度→IZB 链插入；prime = early-Z 每 cell 最大深度。
	 */
	void recordCreateDispatch(vk::raii::CommandBuffer& cmd, const IzbFrameInput& input);
	/** early-Z priming dispatch（语义护栏 = tests/test_izb_create.cppm 判据⑤）。 */
	void recordPrimeDispatch(vk::raii::CommandBuffer& cmd);
	/** head+nodes → staging[parity]（dump 帧专用；含 compute→transfer→host barrier 对）。 */
	void recordDumpCopy(vk::raii::CommandBuffer& cmd);

	RGL::DeviceContext* deviceCtx = nullptr;
	RGL::Resources* resources = nullptr;
	/// 图外同步发射账（批次 B1b；Renderer 持有，生命周期覆盖本模块）
	OffGraphSync& sync;
	vk::Extent2D extent{};
	uint32_t gridDim = 0;

	RGL::VmaBuffer headBuffer;    ///< G² uint（NONE 哨兵；每帧 FillBuffer 清）
	RGL::VmaBuffer nodesBuffer;   ///< W·H × 16B (u, v, lightDepth, next)
	RGL::VmaBuffer primedBuffer;  ///< G² float
	RGL::VmaBuffer maskBuffer;    ///< ⌈W·H/32⌉ word 位图（1 bit/像素 = 被遮；每帧由 recordFrame 清 0、mask pass 写）
	RGL::VmaBuffer staging[2];    ///< head 回读 ring（host-visible mapped）
	void* stagingMapped[2] = {nullptr, nullptr};
	bool dumpPending[2] = {false, false};
	uint32_t parity = 0;

	uint32_t headSlot = 0;
	uint32_t nodesSlot = 0;
	uint32_t primedSlot = 0;
	uint32_t maskSlot = 0;
	uint32_t depthSlot = 0;

	// raii 无默认构造（工程红线），optional 承载「构造期分步初始化」
	std::optional<vk::raii::ShaderEXT> createShader;
	std::optional<vk::raii::ShaderEXT> primeShader;
};

}  // namespace RGL
