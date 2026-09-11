module;
#include <vulkan/vulkan.h>  // VkImage/VkImageView/VkDescriptorSet 句柄（rlvk 不含 C 宏层，vkerror 先例）
export module mip_gen;

import rlstd;
import rlvk;
import vkerror;
import vk_device;

namespace RGL {

// Phase 8 批次 ③b：2D 纹理 mip 链生成（消除平面墙/广场缩小走样 speckle——mipLevels=1
// 采样无 mip → 细密噪点）。双路径（与 bindless 渲染路径隔离，均录进纹理自己的上传 CB）：
//   - **UNORM 纹理 = compute 链（recordChain，mip_gen.comp.slang）**：传统
//     VkDescriptorSet + 经典 compute pipeline（bindless shader object 的 binding 是一次
//     性烘焙的，逐 dispatch 换 src/dst 视图别扭；mip-gen CB 不 bindResourceHeapEXT，与
//     渲染 CB 混用合法，互不污染）。per-level 视图：每目标级一对（sampled 只覆盖
//     level−1、storage 只覆盖 level）——视图范围 = 精确访问子资源，VVL 逐 subresource
//     布局检查天然干净（全链视图会在链生成中期看到 UNDEFINED/GENERAL 级 → 误报）。
//   - **sRGB 纹理 = LINEAR blit 链（recordMipBlitChain）**：R8G8B8A8_SRGB 在 RADV 无
//     STORAGE_IMAGE 能力（格式+usage 查询 FORMAT_NOT_SUPPORTED → sRGB 图不能带 eStorage
//     usage；且本 spec 视图格式须支持图的全部 usage → SRGB 视图 + eStorage 图必被 VVL
//     拒）→ compute 存储不可行，走 blit（字节保真 + 硬件双线性降采样，计划文档"先
//     LINEAR blit 过渡"路径；gamma 空间插值为标准近似）。
//   - **生命周期**：owner = Resources 成员（启动期一次性，覆盖全部上传；先于 device
//     析构）。per-level 视图/描述符集存成员容器（须活过提交 + fence wait——驱动可
//     延迟解析描述符）。本模块不 import vk_resources（Resources 持本类型成员，无环）。

/**
 * MipGenPush：mip_gen.comp.slang 的 push constant（12B，3×u32 scalar 布局）。
 * 契约单一真相源 = shader 同名 struct（12B 字面量一致，任一侧漂移本 static_assert /
 * shader 布局错配——CullPush 先例，无反射管线对应）。
 */
export struct MipGenPush {
	uint32_t srcMip = 0;      // 源 mip 级（被降采样；= 目标级 − 1）
	uint32_t dstWidth = 0;    // 目标 mip 宽（= max(1, srcW>>1)，C++/shader 同式）
	uint32_t dstHeight = 0;   // 目标 mip 高（= max(1, srcH>>1)）
};
static_assert(sizeof(MipGenPush) == 12, "MipGenPush 须与 mip_gen.comp.slang push 布局一致（12B）");

/**
 * MipGen：2D 纹理 compute mip 链生成器（Phase 8 ③b，启动期一次性基础设施）。
 * 持有传统 compute 管线全栈（set layout 3 binding / pipeline layout 16B push /
 * pipeline / 专用 sampler / 可扩展 descriptor pool）+ per-level 视图/描述符集成员
 * 容器（生命周期覆盖全部提交）。owner = Resources 成员（构造于 Resources 构造期，
 * 析构先于 device）。
 */
export class MipGen {
   public:
	/**
	 * 创建 mip-gen 基础设施（启动期一次性）：加载 generated_shaders/mip_gen.comp.spv +
	 * shader module + 描述符集布局（0=sampled 1=storage 2=sampler，均 COMPUTE）+
	 * pipeline layout（COMPUTE push 12B）+ compute 管线 + 专用 sampler（LINEAR/CLAMP，
	 * 非渲染共享 sampler——避免污染模型纹理采样）+ 初始 descriptor pool（1024 套，
	 * 池尽自动扩，FREE_DESCRIPTOR_SET_BIT——raii 析构归还）。同时校验
	 * R8G8B8A8_UNORM 的 STORAGE_IMAGE 能力（UNORM compute 链的前提）。
	 * @param device 设备层（device / physical device 能力查询）
	 * @note SPV 加载 / 格式无存储能力 / 管线创建失败 = 抛 runtime_error（启动期一次性
	 *   路径，同 GpuCull 构造器）
	 */
	explicit MipGen(RGL::DeviceContext& device);
	/** 拷贝删除：raii GPU 句柄成员（所有权唯一）。 */
	MipGen(const MipGen&) = delete;
	/** 拷贝赋值删除：同上。 */
	MipGen& operator=(const MipGen&) = delete;
	/** 移动删除：Resources 值成员原地构造，无需移动。 */
	MipGen(MipGen&&) = delete;
	/** 移动赋值删除：同上。 */
	MipGen& operator=(MipGen&&) = delete;

	/**
	 * 把 compute mip 链（level 1..mipLevels−1，每级一个 dispatch）录进 cmd——调用点 =
	 * uploadTexture（**仅 UNORM 纹理**；sRGB 走 recordMipBlitChain，见模块头注释），
	 * cmd = 该纹理的一次性上传 CB（mip0 拷贝 + mip0→SHADER_READ_ONLY 已先行录制，
	 * 本方法续录后由调用方统一 end + submit + fence）。
	 * 布局链（每级 i）：level i UNDEFINED→GENERAL（compute 写）→ dispatch（sampled
	 * view 覆盖 level i−1 的 SRO）→ level i GENERAL→SHADER_READ_ONLY（下级的采样源 /
	 * 渲染端最终布局）。本方法创建的 per-level 视图/描述符集存成员容器（活过提交 +
	 * fence wait）。
	 * @param cmd 目标命令缓冲（录制中；不 bindResourceHeapEXT——纯传统 set）
	 * @param image 目标图（mip0 已在 SHADER_READ_ONLY；level ≥1 为 UNDEFINED；
	 *   创建时须含 eStorage usage）
	 * @param format 目标图格式（UNORM——本链不处理 sRGB）
	 * @param w mip0 宽
	 * @param h mip0 高
	 * @param mipLevels 总 mip 级数（1 = 无需生成，直接返回成功）
	 * @param name 日志名
	 * @return void（成功）；Error = 描述符集分配失败（池扩展仍失败）
	 */
	[[nodiscard]] std::expected<void, Error> recordChain(vk::raii::CommandBuffer& cmd, VkImage image,
		vk::Format format, uint32_t w, uint32_t h, uint32_t mipLevels, const char* name);

    private:
	/** 建一个 descriptor pool（1024 套 × {sampled,storage,sampler}）追加进 pools
	 * （构造期首次调用；池耗尽时再调用）。 */
	void createPool();
	/** 从 pools 尾部起逐池分配一个描述符集（VK_ERROR_OUT_OF_POOL_MEMORY = 建新池重试）；
	 * @return 分配出的 set 句柄（raii 本体存 sets 成员，覆盖全部提交）。 */
	[[nodiscard]] VkDescriptorSet allocateSet();

	vk::raii::Device& device;  // 设备引用（非拥有；初始化列表首位）
	vk::raii::ShaderModule shaderModule;  // mip_gen.comp.spv
	// create-info 指针目标（vkinstance.cppm 先例：pXxx 不能悬空——驱动在 create 调用时
	// 经 raii 构造器解引用；局部数组/句柄在 helper 返回后已死 → 悬垂 UB）：
	std::array<vk::DescriptorSetLayoutBinding, 3> setLayoutBindings;  // pBindings 目标
	vk::raii::DescriptorSetLayout setLayout;  // 3 binding（sampled/storage/sampler）
	VkDescriptorSetLayout setLayoutHandle;  // pSetLayouts 目标（C 句柄值拷贝，活过对象）
	VkPushConstantRange pushRange;  // pPushConstantRanges 目标（COMPUTE push 12B）
	vk::raii::PipelineLayout pipelineLayout;  // 上述 layout + COMPUTE push 12B
	vk::raii::Pipeline pipeline;  // compute 管线（传统，非 shader object）
	vk::raii::Sampler mipSampler;  // 专用 LINEAR/CLAMP sampler（非渲染共享 sampler）
	std::vector<vk::raii::DescriptorPool> pools;  // descriptor pool 列表（可扩展）
	std::vector<vk::raii::DescriptorSet> sets;  // per-level 描述符集（生命周期覆盖全部提交）
	std::vector<vk::raii::ImageView> sampledViews;  // per-level sampled 视图（base=i−1，SRO）
	std::vector<vk::raii::ImageView> storageViews;  // per-level storage 视图（base=i，UNORM）
};

/**
 * sRGB 纹理的 LINEAR blit mip 链（recordChain 的 sRGB 替代路径，见模块头注释）——
 * 每目标级一次 vkCmdBlitImage（LINEAR 过滤 = 硬件双线性降采样，字节保真保持 sRGB
 * 编码）。调用点/前置同 recordChain（mip0 拷贝 + mip0→SHADER_READ_ONLY 已录）。
 * 布局链（每级 i）：level i−1 SRO→TRANSFER_SRC + level i UNDEFINED→TRANSFER_DST →
 * blit → level i TRANSFER_DST→SHADER_READ_ONLY（下级的 blit 源 / 渲染端最终布局）。
 * @param cmd 目标命令缓冲（录制中，同 recordChain 前置）
 * @param image 目标图（mip0 已在 SHADER_READ_ONLY；level ≥1 为 UNDEFINED；
 *   usage 含 eTransferSrc|eTransferDst，**不含** eStorage）
 * @param w mip0 宽
 * @param h mip0 高
 * @param mipLevels 总 mip 级数（1 = 无需生成，直接返回）
 */
export void recordMipBlitChain(vk::raii::CommandBuffer& cmd, VkImage image, uint32_t w, uint32_t h,
	uint32_t mipLevels);

}  // namespace RGL
