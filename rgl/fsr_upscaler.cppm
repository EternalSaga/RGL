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
 * 向 vendor 申报的输入资源 state（impl 里的 `srvState`）所**对应**的 VkImageLayout。
 *
 * vendor 只在 `currentState != newState` 时发 barrier（ffx_vk.cpp:1237）⟹ 图属资源进 FSR
 * 前必须已处于本函数所指布局，否则 vendor 跳过转换 = shader 在错误布局下读写。
 *
 * 为什么是**函数**而不是接口分区里的一个 constexpr：constexpr 的字面量就落在本文件，而它的
 * 对端（`srvState`）在 impl 分区——**同一个事实跨文件各写一份**正是批 14.4b 要消灭的形态
 * （velocity 那条恒假护栏就是这么长出来的）。实现体住在 impl 里紧挨 `srvState`，改申报的人
 * 必然看见它。
 *
 * ⚠ 必须与 `fsr_upscaler_impl.cppm` 里的 `srvState` **成对修改**。
 *
 * @return 三个图属输入（color / depth / velocity）申报的 state 对应的镜像布局
 */
export vk::ImageLayout fsrInputLayout();

/**
 * 一帧喂给 FSR-upscaler 的输入（批 14.5）。
 *
 * @note 三个 image 的**布局是契约的一部分**，不是"随便传个句柄"：FSR 的 VK 后端在
 *       每个 compute job 绑定时按 SRV/UAV 自动排 layout barrier，但它靠的是
 *       `ffxGetResourceVK(..., state)` 里我们**申报**的那个 state（记账种子）——
 *       `addBarrier` 只在 `currentState != newState` 时才发（`ffx_vk.cpp:1237`）。
 *       本模块对三个输入一律申报 `FFX_RESOURCE_STATE_COMPUTE_READ`（对应布局 =
 *       `fsrInputLayout()`，单一真相源），所以**调用方必须保证它们在 dispatch 时刻真的处于
 *       那个布局**。渲染图在挂点处已把 scene_hdr/taa_out 与深度转成 SRO；velocity 是唯一
 *       不达标者（它无下游消费者，停在 CAO），由调用方在挂点转——见
 *       `Renderer::fsrTonemapPassPre` 的布局护栏。
 */
export struct FsrFrameInput {
	vk::Image color = nullptr;     ///< HDR 颜色（渲染分辨率，SRO）
	vk::Image depth = nullptr;     ///< D32_SFLOAT 深度（SRO）
	vk::Image velocity = nullptr;  ///< 逐像素速度场 rgba16_sfloat（SRO）
	// ⚠ **不是「抖动」，是「内容位移」**——这两个名字混用过一次，代价是一整个 Phase 的
	// 时域档（`phase14_plan.md` §1.19）。FSR 全部消费者把本字段当**渲染内容在纹理空间的实际
	// 平移**用（`accumulate.h:110` `fLrUvJittered = fHrUv + Jitter()/RenderSize()`、
	// `upsample.h:307`、`common.h:298`），而**投影注入量与它 Y 轴反号**：本引擎给
	// `proj[2][1]` 减 `2jy/H`（`taa_jitter.cppm::applyProjectionJitter`），官方伪码是加
	// （`ffx_fsr3upscaler.h:513-517`），再叠加负高视口 ⟹ `c = (jx, −jy)`。
	// 唯一合法来源 = `taa_jitter.cppm::contentShiftPx`；地面真值实测（回读 `scene_hdr` +
	// Lucas-Kanade，阳性锚点残差 0.019）：c = (−0.0995, +0.2648) vs contentShiftPx 声称
	// (−0.125, +0.278) 残差 0.029，vs 原始 jitterPx 残差 0.543。
	float contentShiftX = 0.0f;    ///< 本帧内容位移 x（渲染像素单位，= `contentShiftPx(jitterPx).x`）
	float contentShiftY = 0.0f;    ///< 同上 y
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
 * 生命周期 = Renderer 在 runtime.aa 选 fsr（批 14.6 三态）时构造；
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
