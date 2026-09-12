module;
#include <vulkan/vulkan.h>  // VkImage/VkImageView 句柄（rlvk 不含 C 宏层，vkerror 先例）
export module env_prefilter;

import rlstd;
import rlvk;
import vkerror;
import vk_device;
import VmaRaii;  // VmaDescriptorHeap（heap 引用：bind + 写描述符 + 步长/槽地址查询，无环）

namespace RGL {

// Phase 8 批次 ④b：GGX 预滤波环境 cubemap（把 HDR env mip 链从 LINEAR blit box 平均换成
// 按粗糙度 GGX 重要性采样卷积——消除金属表面锐利的橙↔紫镜面条纹）。**descriptor heap +
// shader object 基础设施 = mip_gen（Phase 8 ③b 刚迁完 heap）的直接变体**（同 heap 模式：
// 每 level index 一 compute shader object + 一对独立 heap 槽，binding 0=srcSampled(cube
// mip0)/1=dstStorage(2DArray 第 level 级)→resource heap push-index、2=sampler→sampler heap
// constant-offset，vkWriteResourceDescriptorsEXT 写 image 描述符 + bindResourceHeapEXT +
// bindShadersEXT + dispatch + per-dispatch dst 布局屏障）。
//
// 与 mip_gen 的两处本质差异：
//   - **dst = cube 的第 level 级**（`VK_IMAGE_VIEW_TYPE_2D_ARRAY` 覆盖 6 层，RWTexture2DArray，
//     dispatch (ceil(faceSize/8), ceil(faceSize/8), 6)，z=face），非 2D 单图；src = 同一 cube 的
//     mip0 sharp 源（`VK_IMAGE_VIEW_TYPE_CUBE` 覆盖 mip0 六层，采样 SampleLevel(_, L, 0)）。
//   - **每 level 只读 mip0**（非链式读 level−1），故各 level 相互独立、无链式布局依赖；
//     mip0 在 recordPrefilter 前由调用方迁到 SHADER_READ_ONLY，dst 各级 UNDEFINED/
//     TRANSFER_DST→GENERAL（compute 写）→SHADER_READ_ONLY（渲染端最终态）。
// 全走 descriptor heap，零 descriptor pool / set layout / pipeline layout / pipeline。
//
// **生命周期**：owner = Resources 成员（启动期一次性，env 16F 上传时喂 level，先于 device 析构）。
// heap 槽由 Resources 经 allocSlot 领取后喂给本类型（本模块不 import vk_resources——Resources
// 持本类型成员，无环；heap 引用经 init 注入）。per-level 视图 create info 存成员 deque
// （不 realloc → pView 指针稳定，活过提交 + fence wait——驱动可延迟解析）。

/**
 * EnvPrefilterPush：env_prefilter.comp.slang 的 push constant（12B，3×u32 scalar 布局）。
 * 契约单一真相源 = shader 同名 struct（12B 字面量一致，任一侧漂移本 static_assert /
 * shader 布局错配——MipGenPush 先例，无反射管线对应）。descriptor heap 下 push constant
 * 经 vkCmdPushDataEXT 的 push data 区供给（offset 0..11），heap push index 落在 offset 20。
 */
export struct EnvPrefilterPush {
	uint32_t level = 0;       // 目标 mip 级（1..mipLevels−1）
	uint32_t mipLevels = 0;   // 总级数（roughness = level/(mipLevels−1)，与采样端一致）
	uint32_t faceSize = 0;    // 目标级面边长（= max(1, mip0Size >> level)，C++/shader 同式）
};
static_assert(sizeof(EnvPrefilterPush) == 12, "EnvPrefilterPush 须与 env_prefilter.comp.slang push 布局一致（12B）");

/**
 * EnvPrefilter：GGX 预滤波环境 cubemap 生成器（Phase 8 ④b，启动期一次性基础设施）。
 * 持有 env_prefilter.comp SPV + **每 level index 一个 compute shader object**（heap +
 * push-index mapping，非传统管线/pipeline layout）+ 每 level 一对 heap 描述符槽（src sampled
 * cube mip0 / dst storage 2DArray，由 Resources 领取喂入）+ per-level 视图 create info 稳定
 * 容器（pView 指向，生命周期覆盖全部提交）。无 descriptor pool / set layout / pipeline layout /
 * pipeline（全走 descriptor heap）。owner = Resources 成员（构造于 Resources 构造期，析构先于 device）。
 */
export class EnvPrefilter {
   public:
	/**
	 * 加载 env_prefilter.comp.spv 进成员（启动期一次性）+ 校验 RGBA16_SFLOAT 的 STORAGE_IMAGE
	 * 能力（GGX 预滤波 compute 写 dst 前提）。**不建 shader object**——shader object 需 heap 槽，
	 * 槽由 Resources 于 env 16F 上传前经 init + addLevel 喂入（避免构造期抢占 heap 游标）。
	 * @param device 设备层（device / physical device 能力查询 + 后续 createShadersEXT）
	 * @note SPV 加载失败 / 格式无存储能力 = 抛 runtime_error（启动期一次性路径，同 MipGen）
	 */
	explicit EnvPrefilter(RGL::DeviceContext& device);
	/** 拷贝删除：raii GPU 句柄成员（所有权唯一）。 */
	EnvPrefilter(const EnvPrefilter&) = delete;
	/** 拷贝赋值删除：同上。 */
	EnvPrefilter& operator=(const EnvPrefilter&) = delete;
	/** 移动删除：Resources 值成员原地构造，无需移动。 */
	EnvPrefilter(EnvPrefilter&&) = delete;
	/** 移动赋值删除：同上。 */
	EnvPrefilter& operator=(EnvPrefilter&&) = delete;

	/**
	 * 注入 heap 引用与共享 sampler 槽（幂等首建，由 Resources::ensureEnvPrefilterReady 驱动）。
	 * 仅记录引用（不拥有 heap）+ sampler heap 槽号；shader object 待 addLevel 建。
	 * @param resourceHeap resource descriptor heap（src/dst image 描述符落此，bind 此）
	 * @param samplerHeap sampler heap（sampler 描述符所在，bind 此）
	 * @param samplerSlot 共享 sampler 在 sampler heap 的槽号（ensureSharedSampler 产物，
	 *   LINEAR/CLAMP，全 level 共用一份——SampleLevel 显式 lod=0 读 mip0）
	 */
	void init(const VmaDescriptorHeap& resourceHeap, const VmaDescriptorHeap& samplerHeap,
		uint32_t samplerSlot);
	/**
	 * 为一个 mip level index 建独立 compute shader object（heap + push-index mapping，binding
	 * 0=srcSampled / 1=dstStorage 各烘焙到独立槽，2=sampler）+ 登记 src/dst 槽号 + 追加
	 * per-level 描述符链稳定存储。由 Resources::ensureEnvPrefilterReady 逐 level 调用（env 单例
	 * 上传时把 level 数补齐到 mipLevels，之后每 level 复用同批）。level index = mip level
	 * （0 占位不用于预滤波生成，但同批建好以便按下标直接取）。
	 * @param srcSlot src sampled image 描述符在 resource heap 的槽号（Resources allocSlot 领取）
	 * @param dstSlot dst storage image 描述符在 resource heap 的槽号（同上）
	 * @return void（成功）；Error = shader object 创建失败（createShadersEXT 抛 → 转 unexpected）
	 */
	[[nodiscard]] std::expected<void, Error> addLevel(uint32_t srcSlot, uint32_t dstSlot);
	/** @return 是否已 init（heap 引用有效）。 */
	[[nodiscard]] bool isInitialized() const { return resourceHeap != nullptr; }
	/** @return 已建的 level shader 数（= 可生成的最大 mip level 上界，含 index 0 占位）。 */
	[[nodiscard]] uint32_t levelCount() const { return static_cast<uint32_t>(levels); }

	/**
	 * 把 GGX 预滤波链（level 1..mipLevels−1，每级一个 dispatch (ceil(fs/8),ceil(fs/8),6)）录进
	 * cmd——调用点 = cubemap 上传 CB（16F/ggxPrefilter 分支，**mip0 已 copy 完并先行迁到
	 * SHADER_READ_ONLY**，本方法续录后由调用方统一 end + submit + fence）。
	 * **前置**：Resources::ensureEnvPrefilterReady(mipLevels) 已把 level shader/槽建齐（本方法
	 * 不分配槽，只用 level 1..mipLevels−1）。
	 * 录制序：先把各 level 的 src（cube mip0 sampled SRO）/dst（2DArray 第 level 级 storage
	 * GENERAL）image 描述符一次性写进各自固定槽（同 submission 内各 level 独立槽、互不覆盖）
	 * → 一次 heap host barrier（eHost→heap read）→ 一次 bind resource/sampler heap → 逐 level
	 * { dst 第 level 级 →GENERAL（从调用方遗留布局）→ bindShadersEXT（该 level shader object）
	 * → pushDataEXT（12B EnvPrefilterPush + @20 的 heap push index 0）→ dispatch → dst 第 level
	 * 级 GENERAL→SHADER_READ_ONLY }。本方法把当级视图 create info 写进 per-level 稳定链（活过
	 * 提交 + fence wait）。
	 * @param cmd 目标命令缓冲（录制中；bindResourceHeapEXT，非 descriptor set——坑 9 互斥）
	 * @param cubeImage 目标 cube 图（mip0 已在 SHADER_READ_ONLY；level 1..L−1 待预滤波；
	 *   创建时须含 eStorage usage）
	 * @param format 目标图格式（RGBA16_SFLOAT——本 compute 只处理 float，不处理 sRGB）
	 * @param faceSize mip0 面边长（目标级 fs = max(1, faceSize >> level)）
	 * @param mipLevels 总 mip 级数（1 = 无需生成，直接返回成功）
	 * @param name 日志名
	 * @return void（成功）；Error = level 不足（ensureEnvPrefilterReady 未建够）或槽越界
	 */
	[[nodiscard]] std::expected<void, Error> recordPrefilter(vk::raii::CommandBuffer& cmd,
		VkImage cubeImage, vk::Format format, uint32_t faceSize, uint32_t mipLevels,
		const char* name);

   private:
	/** per-level 描述符写入链的稳定存储（deque 元素地址不随 push_back 迁移——v341 契约：
	 * ImageDescriptorInfoEXT::pView 指 ImageViewCreateInfo 值，非 VkImageView 句柄）。 */
	struct DescChain {
		vk::ImageViewCreateInfo viewCreate{};      // per-level 视图描述（cube mip0 / 2DArray 第 level 级）
		vk::ImageDescriptorInfoEXT imageDesc{};     // pView = &viewCreate，layout = SRO / GENERAL
		vk::ResourceDescriptorInfoEXT resourceDesc{};  // type = Sampled / Storage，data.pImage = &imageDesc
		vk::HostAddressRangeEXT dst{};             // 描述符写入目标 slot 范围（getSlotCpuAddress 解析）
	};

	/** 往 resource heap 的某 level 链写一个 image 描述符（sampled cube=SRO / storage 2DArray=GENERAL）。 */
	[[nodiscard]] std::expected<void, Error> writeImageDesc(DescChain& chain, uint32_t slot,
		VkImage image, vk::Format format, vk::ImageViewType viewType, uint32_t baseMip,
		uint32_t baseLayer, uint32_t layerCount, vk::DescriptorType type, vk::ImageLayout layout);
	/** 为一个 level 建 compute shader object（3 mapping：0/1 = heap push-index，2 = sampler）。 */
	[[nodiscard]] std::expected<vk::raii::ShaderEXT, Error> createLevelShader(uint32_t srcSlot,
		uint32_t dstSlot);

	vk::raii::Device& device;  // 设备引用（非拥有；初始化列表首位）
	std::vector<uint32_t> code;  // env_prefilter.comp.spv 码流（构造期加载，逐 level 建 shader object 复用）

	const VmaDescriptorHeap* resourceHeap = nullptr;  // init 注入（非拥有；Resources 成员）
	const VmaDescriptorHeap* samplerHeap = nullptr;   // init 注入（非拥有）
	uint32_t samplerSlot = 0;  // 共享 sampler 在 sampler heap 的槽号（init 注入）

	uint32_t levels = 0;  // 已建 level shader 数（= 可生成最大 mip level 上界）
	std::vector<vk::raii::ShaderEXT> shaders;  // 每 level 一个 compute shader object（index = mip level）
	std::vector<uint32_t> srcSlots;  // 每 level src sampled 描述符槽号
	std::vector<uint32_t> dstSlots;  // 每 level dst storage 描述符槽号
	std::deque<DescChain> srcChains;  // per-level src 描述符链（deque = pView 指针稳定）
	std::deque<DescChain> dstChains;  // per-level dst 描述符链
};

}  // namespace RGL
