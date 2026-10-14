module;
#include <vulkan/vulkan.h>  // VkImage/VkImageView 句柄（rlvk 不含 C 宏层，vkerror 先例）
export module mip_gen;

import rlstd;
import rlvk;
import vkerror;
import vk_device;
import VmaRaii;  // VmaDescriptorHeap（heap 引用：bind + 写描述符 + 步长/槽地址查询，无环）

namespace RGL {

// Phase 8 批次 ③b：2D 纹理 mip 链生成（消除平面墙/广场缩小走样 speckle——mipLevels=1
// 采样无 mip → 细密噪点）。双路径（均录进纹理自己的上传 CB，CB 内零 descriptor set）：
//   - **UNORM 纹理 = compute 链（recordChain，mip_gen.comp.slang）**：**descriptor heap +
//     shader object**（迁离旧 descriptor pool——与渲染/gpu_cull 同路，全项目零 pool）。
//     每 level index 一 shader object + 一对独立 heap 槽（srcSampledSlot/dstStorageSlot，
//     init 时一次性建好、每纹理复用同一批），binding 0/1 → resource heap（各槽固定，
//     非每 dispatch 重写同槽——同 submission 内重写同槽必被后续 host 写覆盖 = 错数据），
//     binding 2 → sampler heap（复用 SamplerLibrary::ensureShared 的 LINEAR sampler）。
//     shader object 的 image binding 用 VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_PUSH_INDEX_EXT
//     （RADV heap storage image 唯一验证可用配方，见 docs/DESCRIPTOR_HEAP_SPEC.md 坑 17），
//     push index 恒 0（pushOffset=5×4 处 push data）。per-level 视图：每目标级一对
//     （sampled 只覆盖 level−1、storage 只覆盖 level）——视图范围 = 精确访问子资源，VVL
//     逐 subresource 布局检查天然干净（全链视图会在链生成中期看到 UNDEFINED/GENERAL 级误报）。
//   - **sRGB 纹理 = LINEAR blit 链（recordMipBlitChain2d）**：R8G8B8A8_SRGB 在 RADV 无
//     STORAGE_IMAGE 能力（格式+usage 查询 FORMAT_NOT_SUPPORTED → sRGB 图不能带 eStorage
//     usage；且本 spec 视图格式须支持图的全部 usage → SRGB 视图 + eStorage 图必被 VVL
//     拒）→ compute 存储不可行，走 blit（字节保真 + 硬件双线性降采样）。本路径无描述符，
//     不受 heap 迁移影响。
//   - **生命周期**：owner = SamplerLibrary 成员（启动期一次性，覆盖全部上传；先于 device 析构）。
//     heap 槽由 SamplerLibrary 经 Resources::allocSlot(Runtime) 领取后喂给本类型（本模块不 import vk_resources——
//     SamplerLibrary 持本类型成员，无环；heap 引用经 init 注入）。per-level 视图 create info
//     存成员 deque（不 realloc → pView 指针稳定，活过提交 + fence wait——驱动可延迟解析）。

// MipGenPush（mip_gen.comp.slang 的 push constant，12B / 3×u32 scalar 布局）**不进本接口 unit**：
// 它现在是 compute-push 反射管线的产物（generated/compute_push_types_generated.cppm），只被
// `mip_gen:impl` import，接口因此不拖生成物的 GMF 宏面（glm / cstddef）——先例 = evssm_shadow.cppm。
// 本模块外部零消费者（唯一使用点在 impl 的 push data 包装里），故不再导出。
//
// 🔴 原先此处是手抄 `export struct MipGenPush {…}` + `static_assert(sizeof == 12)`，删它的理由是
// **那条 assert 是关于 C++ 侧的同义反复**：量的是手抄的 C++ struct，不是 shader ⟹ shader 把两个
// 同类型字段换序而 C++ 没跟着动时它照样全过，而 C++ 写的字节与 shader 读的字节已经错位，静默。
// 现在布局由反射派生、生成物自带 sizeof/offsetof assert；"期望值 = 去 shader 逐字段手算"这条独立
// 判据没有丢，它挪进了 tests/test_compute_push_layout.cppm。判据与红线 → docs/compute_调研.md §7.13。

/**
 * MipGen：2D 纹理 compute mip 链生成器（Phase 8 ③b，启动期一次性基础设施）。
 * 持有 mip_gen.comp SPV + **每 level index 一个 compute shader object**（heap + push-index
 * mapping，非传统管线/pipeline layout）+ 每 level 一对 heap 描述符槽（src sampled /
 * dst storage，由 SamplerLibrary 领取喂入）+ per-level 视图 create info 稳定容器（pView 指向，
 * 生命周期覆盖全部提交）。无 descriptor pool / set layout / pipeline layout / pipeline
 * （全走 descriptor heap）。owner = SamplerLibrary 成员（构造于 SamplerLibrary 构造期，析构先于 device）。
 */
export class MipGen {
   public:
	/**
	 * 加载 mip_gen.comp.spv 进成员（启动期一次性）+ 校验 R8G8B8A8_UNORM 的 STORAGE_IMAGE
	 * 能力（UNORM compute 链前提）。**不建 shader object**——shader object 需 heap 槽，
	 * 槽由 Resources 于首张 UNORM 纹理上传前经 init + addLevel 喂入（构造期不抢占
	 * descriptor heap 用户槽）。
	 * @param device 设备层（device / physical device 能力查询 + 后续 createShadersEXT）
	 * @note SPV 加载 / 格式无存储能力 = 抛 runtime_error（启动期一次性路径，同 GpuCull）
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
	 * 注入 heap 引用与共享 sampler 槽（幂等首建，由 SamplerLibrary::ensureMipGenReady 驱动）。
	 * 仅记录引用（不拥有 heap）+ sampler heap 槽号；shader object 待 addLevel 建。
	 * @param resourceHeap resource descriptor heap（src/dst image 描述符落此，bind 此）
	 * @param samplerHeap sampler heap（sampler 描述符所在，bind 此）
	 * @param samplerSlot 共享 sampler 在 sampler heap 的槽号（ensureSharedSampler 产物，
	 *   LINEAR/CLAMP，全 level 共用一份——SampleLevel 显式 lod，maxLod 20 覆盖任何链长）
	 */
	void init(const VmaDescriptorHeap& resourceHeap, const VmaDescriptorHeap& samplerHeap,
		uint32_t samplerSlot);
	/**
	 * 为一个 level index 建独立 compute shader object（heap + push-index mapping，binding
	 * 0=srcSampled / 1=dstStorage 各烘焙到独立槽，2=sampler）+ 登记 src/dst 槽号 + 追加
	 * per-level 描述符链稳定存储。由 SamplerLibrary::ensureMipGenReady 逐 level 调用（首张足够大的
	 * 纹理时把 level 数补齐到所需，之后每纹理复用同批——跨纹理由 fence 分隔，无覆盖）。
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
	 * 把 compute mip 链（level 1..mipLevels−1，每级一个 dispatch）录进 cmd——调用点 =
	 * uploadTexture（**仅 UNORM 纹理**；sRGB 走 recordMipBlitChain2d，见模块头注释），
	 * cmd = 该纹理的一次性上传 CB（mip0 拷贝 + mip0→SHADER_READ_ONLY 已先行录制，
	 * 本方法续录后由调用方统一 end + submit + fence）。
	 * **前置**：SamplerLibrary::ensureMipGenReady(mipLevels) 已把 level shader/槽 建齐（本方法
	 * 不分配槽，只用 level 0..mipLevels−1）。
	 * 录制序：先把各 level 的 src/dst image 描述符一次性写进各自固定槽（同 submission 内
	 * 各 level 独立槽、互不覆盖）→ 一次 heap host barrier（eHost→heap read）→ 一次 bind
	 * resource/sampler heap → 逐 level { dst level 视图 UNDEFINED→GENERAL → bindShadersEXT
	 * （该 level shader object）→ pushDataEXT（12B MipGenPush + @20 的 heap push index 0）
	 * → dispatch → dst level GENERAL→SHADER_READ_ONLY }。本方法把当级视图 create info 写进
	 * per-level 稳定链（活过提交 + fence wait）。
	 * @param cmd 目标命令缓冲（录制中；bindResourceHeapEXT，非 descriptor set——坑 9 互斥）
	 * @param image 目标图（mip0 已在 SHADER_READ_ONLY；level ≥1 为 UNDEFINED；
	 *   创建时须含 eStorage usage）
	 * @param format 目标图格式（UNORM——本链不处理 sRGB）
	 * @param w mip0 宽
	 * @param h mip0 高
	 * @param mipLevels 总 mip 级数（1 = 无需生成，直接返回成功）
	 * @param name 日志名
	 * @return void（成功）；Error = level 不足（ensureMipGenReady 未建够）或槽越界
	 */
	[[nodiscard]] std::expected<void, Error> recordChain(vk::raii::CommandBuffer& cmd, VkImage image,
		vk::Format format, uint32_t w, uint32_t h, uint32_t mipLevels, const char* name);

   private:
	/** per-level 描述符写入链的稳定存储（deque 元素地址不随 push_back 迁移——v341 契约：
	 * ImageDescriptorInfoEXT::pView 指 ImageViewCreateInfo 值，非 VkImageView 句柄）。 */
	struct DescChain {
		vk::ImageViewCreateInfo viewCreate{};      // per-level 视图描述（当级 image/format/子资源范围）
		vk::ImageDescriptorInfoEXT imageDesc{};     // pView = &viewCreate，layout = SRO / GENERAL
		vk::ResourceDescriptorInfoEXT resourceDesc{};  // type = Sampled / Storage，data.pImage = &imageDesc
		vk::HostAddressRangeEXT dst{};             // 描述符写入目标 slot 范围（getSlotCpuAddress 解析）
	};

	/** 往 resource heap 的某 level 链写一个 image 描述符（sampled=SRO / storage=GENERAL）。 */
	[[nodiscard]] std::expected<void, Error> writeImageDesc(DescChain& chain, uint32_t slot,
		VkImage image, vk::Format format, uint32_t mip, vk::DescriptorType type,
		vk::ImageLayout layout);

	vk::raii::Device& device;  // 设备引用（非拥有；初始化列表首位）
	std::vector<uint32_t> code;  // mip_gen.comp.spv 码流（构造期加载，逐 level 建 shader object 复用）

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
 *
 * @parallel_group mip_blit_chain —— 与 `recordMipBlitChainCube` **刻意不合并**。两者都是
 * 「逐 level blit 降采样」，但**布局状态机相反**：本函数源级每级在 SRO⇄TRANSFER 之间来回、
 * 目标级从 UNDEFINED 起；cube 版整条链留在 TRANSFER、末尾一次性转 SRO、源级从 TRANSFER_DST 起。
 * 合并要参数化 4 个互斥开关 + 滤镜 + 层数，得到「每个调用方只设一半旋钮」的函数——比留两份更糟。
 * 二者原名互为同词转置（`recordMipBlitChain` / `recordBlitMipChain`），读代码极易调错，
 * 2026-10-09 按形状加后缀消歧。
 */
export void recordMipBlitChain2d(vk::raii::CommandBuffer& cmd, VkImage image, uint32_t w, uint32_t h,
	uint32_t mipLevels);

}  // namespace RGL
