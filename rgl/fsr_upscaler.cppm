module;
#include <vulkan/vulkan.h>  // VK_* 句柄类型（rlvk 不含 C 句柄）
export module fsr_upscaler;

import rlstd;
import rlvk;
import vk_device;
import vk_resources;
import VmaRaii;

namespace RGL {

/**
 * 一帧喂给 FSR-upscaler 的输入（批 14.5）。
 *
 * @note 三个 image 的**布局是契约的一部分**，不是"随便传个句柄"：FSR 的 VK 后端在
 *       每个 compute job 绑定时按 SRV/UAV 自动排 layout barrier，但它靠的是
 *       `ffxGetResourceVK(..., state)` 里我们**申报**的那个 state（记账种子）——
 *       `addBarrier` 只在 `currentState != newState` 时才发（`ffx_vk.cpp:1237`）。
 *       本模块对三个输入一律申报 `FFX_RESOURCE_STATE_COMPUTE_READ`
 *       （= `VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`），所以**调用方必须保证它们在
 *       dispatch 时刻真的处于 SRO**。渲染图在挂点处已把 scene_hdr/taa_out 与深度转成
 *       SRO；velocity 是唯一不达标者（它无下游消费者，停在 CAO），由调用方在挂点转
 *       ——见 `Renderer::fsrTonemapPassPre` 的布局护栏。
 */
export struct FsrFrameInput {
	vk::Image color = nullptr;     ///< HDR 颜色（渲染分辨率，SRO）
	vk::Image depth = nullptr;     ///< D32_SFLOAT 深度（SRO）
	vk::Image velocity = nullptr;  ///< 逐像素速度场 rgba16_sfloat（SRO）
	float jitterX = 0.0f;          ///< 当前帧 TAA 抖动的 x（像素单位的 UV 口径，见 §3 契约）
	float jitterY = 0.0f;          ///< 同上 y
	float nearPlane = 0.05f;       ///< 投影**实际生效**的 near（取 ViewProjection，非 Camera 原值）
	float farPlane = 1000.0f;      ///< 同上 far
	float fovVertical = 1.0f;      ///< 垂直 fov，**弧度**（Camera::fov，GLM_FORCE_RADIANS）
	/** 本帧曝光（= tonemap 将施加的那个值，scene data @56）。FSR 的输入是**未曝光**的线性
	 *  HDR，所以要把曝光告诉它，它的 luma 相关判据（reactivity / flicker / instability）
	 *  才是在正确的亮度尺度上算的。vendor 明确 **0.0f 是非法值**（`ffx_fsr3upscaler.cpp:256`
	 *  直接打 ERROR 并回落 1.0）⟹ 本值被钳到正数。 */
	float exposure = 1.0f;
	bool reset = false;            ///< 时域重置（首帧 / 机位突变 / 重建后）
};

/**
 * AMD FSR 3.1.4 upscaler 的 Vulkan 后端封装（Phase 14 批 14.5）。
 *
 * 图外 compute（与 izb_shadow / evssm_shadow 同族编舞，render_graph 无 compute kind）：
 * 它在命令流里占一段**自己录制**的 compute pass 序列，编译图看不见它。
 *
 * 生命周期 = Renderer 在 runtime.fsr_upscaler 开门时构造；
 * **门关 = 本对象根本不被构造**（逐位旁路红线，同 izb_shadow / evssm_shadow）。
 *
 * 布局策略（与另两个图外模块相反，值得注意）：本模块**自己不发任何 layout barrier**，
 * 也不做"进出同值"回正——FSR 的 VK 后端会发（见 `FsrFrameInput` 头注释）。本模块的
 * 责任是**申报真实 state**，使它的记账与真实布局从第 0 帧起就一致，从而
 * **一帧都不需要转换**。三个 FSR 私有输出图常驻 GENERAL（= UAV 要的布局，同样零转换）。
 *
 * vendor 侧三条已实测的硬约束（出处逐条 → docs/phase14_plan.md §1.12）：
 *  - 深度输入必须申报 `FFX_RESOURCE_USAGE_DEPTHTARGET`，否则 FSR 在 D32_SFLOAT 上建
 *    COLOR aspect view = 7 条 VVL 报错（`just fsr-smoke` 探针 4 实测）。
 *  - `renderSize` 超过 `maxRenderSize` 是**硬失败**（`FFX_ERROR_OUT_OF_RANGE`）⟹
 *    `recordFrame` 必须检查返回码，否则 resize 超上限 = 静默不升频。
 *  - 内部共享图按 `maxRenderSize` 在 create 时定尺寸 ⟹ 改分辨率必须重建本对象
 *    （**时域历史全丢**），重建前 `waitIdle`。
 */
export class FsrUpscaler {
   public:
	/**
	 * @param vkDevice      设备层（建私有图 / 一次性 CB 转 GENERAL）
	 * @param resources     VMA 分配器归属（显存计入 Resources 台账，别绕过）
	 * @param renderExtent  内部渲染分辨率（= FSR 的 renderSize 与 maxRenderSize）
	 * @param upscaleExtent 上屏分辨率（= FSR 的 upscaleSize 与 maxUpscaleSize）
	 * @param outputSlot    `Resources::ensureFsrSampleSlot()` 预发的采样槽；**构造即写入
	 *        描述符**——tonemap 的色输入槽号是启动期烘进管线条目的，所以「槽已烘好却还
	 *        没人写描述符」的窗口必须是零（这是本对象在 Renderer 构造期建、而不是像
	 *        izb/evssm 那样首帧懒构造的唯一理由）
	 * @note 构造会 `ffxFsr3UpscalerContextCreate`；vendor 的 context 结构约 1 MiB
	 *       ⟹ **不在栈上**（本类经 pimpl 把它放堆上，见 PATCHES.md P6 同一坑）。
	 */
	FsrUpscaler(RGL::DeviceContext& vkDevice, RGL::Resources& resources,
		vk::Extent2D renderExtent, vk::Extent2D upscaleExtent, uint32_t outputSlot,
		bool temporal = false);
	/** 销毁 vendor context（先于私有图的 VmaImage 析构——它持有这些图的 view）。 */
	~FsrUpscaler();

	FsrUpscaler(const FsrUpscaler&) = delete;
	/** 禁拷贝：持有 vendor context（内含指向 `scratch` 的后端记账）与独占 GPU 资源。 */
	FsrUpscaler& operator=(const FsrUpscaler&) = delete;

	/**
	 * 录制本帧的 FSR pass 序列（一次调用 = vendor 内部一整块，**没有** per-pass 录制 API）。
	 * @param cmd   pass 循环所在命令缓冲（heap 已绑定）
	 * @param input 本帧输入，布局要求见 `FsrFrameInput`
	 * @throw std::runtime_error dispatch 返回错误码（含 renderSize 超上限的硬失败）
	 */
	void recordFrame(vk::raii::CommandBuffer& cmd, const FsrFrameInput& input);

	/** @return FSR 输出图（GENERAL 常驻；本批尚无消费者，见 §1.12 的接法决策）。 */
	vk::Image outputImage() const;

	/**
	 * @return FSR 侧显存占用（字节），来自 vendor 自己的 `GetGpuMemoryUsage`。
	 * @note 这是 14.8「外部分配器」科目的读数来源。**私有图走 Resources 分配器**
	 *       ⟹ 已计入 VMA 台账，本函数只作交叉核对，不得再加进总数里（双计）。
	 */
	uint64_t vramBytes() const;

	/** @return 构造时锁定的渲染分辨率（重建判据用，同 IzbShadow::getExtent）。 */
	vk::Extent2D getRenderExtent() const;

   private:
	struct Impl;  ///< vendor 类型不外泄（context 1 MiB + 5 张私有图 + 资源包装缓存）
	std::unique_ptr<Impl> impl;
};

}  // namespace RGL
