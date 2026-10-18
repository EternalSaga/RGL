module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）；offsetof 经其 stddef.h 提供
#include <fmt/format.h>
module env_prefilter:impl;

import env_prefilter;  // clang 不给 partition impl unit 隐式 import 主接口
import compute_push_types_generated;  // EnvPrefilterPush（反射产物，布局有出处；不进接口 unit）
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import VmaRaii;       // VmaDescriptorHeap（heap 引用类型 + getSlotCpuAddress/getDescriptorSize）
import vulkan_utils;  // createImageBarrier（per-level 布局物化）
import shader;        // loadShaderSpv（env_prefilter.comp.slang）

namespace RGL {

// 构造器 / init / addLevel / writeImageDesc / recordPrefilter 定义（契约见接口声明）。本单元是
// 叶子边（不 import vk_resources——Resources 持本模块类型，无环），编译不上关键路径。
// descriptor heap + shader object 版（mip_gen 的 cube 变体）；逐 level 的 shader object 创建
// 已收成共享点 shader.cppm::makePerLevelComputeShader（与 mip_gen 同一份实现）。

namespace {

/** env_prefilter.comp.spv 码流（加载失败 = 启动期错误，抛出——同 MipGen/GpuCull SPV 加载路径）。 */
std::vector<uint32_t> loadEnvPrefilterSpv() {
	auto spv = loadShaderSpv("generated_shaders/env_prefilter.comp.spv");
	if (!spv)
		throw std::runtime_error(fmt::format("env_prefilter: {}", spv.error().describe()));
	return *spv;
}

/** 本模块 push data 布局（24B）：@0 EnvPrefilterPush（20B shader push constant，5×u32，**类型
 * 来自反射生成物**）+ @20 heap push index（u32，恒 0）。descriptor heap 下 push constant 经
 * vkCmdPushDataEXT 供给（vkCmdPushConstants 与 descriptor heap 互斥——见 descriptorheaps.adoc :962-965）。
 * static_assert 锁死 heapIndex 落在 kPushIndexByte（共享常量，现居 shader.cppm；漂移即编译失败）。
 *
 * 🔴 下面两条 assert **不是**同义反复，别跟着 §7.13 那批一起删：它们量的对象（heapIndex
 * 落在 @20 + 24B 总长）在 shader 里**根本不存在**——descriptor heap 的 push data 分帧是纯 C++ 侧
 * 事实。内层 push 契约一旦变长，heapIndex 会被顶离 offset 20，这两条必须炸。判据：断言量的是
 * shader 里有的东西 → 同义反复，删；量的是 shader 里没有的 C++ 侧布局 → 真不变量，留。
 *
 * 2026-10-18：内层 push 由 12B（3×u32）长到 20B（5×u32，逐样本 LOD 加 srcFaceSize/srcBaseMip）
 * ⟹ 原先的 `pad[2]` 变 `pad[0]`（本文件直接不留成员），heapIndex 才仍落在 @20。留 pad 的话
 * 下面第一条 assert 立刻红——这正是它存在的意义：内层契约变长时**逼**你重排外层而不是静默错位。 */
struct EnvPrefilterPushData {
	EnvPrefilterPush push;
	uint32_t heapIndex;
};
static_assert(offsetof(EnvPrefilterPushData, heapIndex) == kPushIndexByte,
	"EnvPrefilterPushData.heapIndex 必须落在 push data offset 20（与 mapping.pushOffset 同频）");
static_assert(sizeof(EnvPrefilterPushData) == 24, "EnvPrefilterPushData 须 24B（覆盖 heapIndex@20）");

}  // namespace

/** 契约见接口注释。实现：初始化列表加载 SPV + 存设备引用；体内校验 RGBA16_SFLOAT 的
 * STORAGE_IMAGE 能力（GGX 预滤波 compute 写 dst 前提）+ 日志。**不建 shader object**
 * （heap 槽由 Resources 后续喂入）。 */
EnvPrefilter::EnvPrefilter(RGL::DeviceContext& device)
	: device(device.getDevice()), code(loadEnvPrefilterSpv()) {
	// dst（cube 第 level 级 storage 2DArray view）格式 = RGBA16_SFLOAT，须有 STORAGE_IMAGE 能力
	const auto props = device.getPhysicalDevice().getFormatProperties(
		vk::Format::eR16G16B16A16Sfloat);
	if ((props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eStorageImage)
		== vk::FormatFeatureFlags{})
		throw std::runtime_error(
			"env_prefilter: RGBA16_SFLOAT 无 STORAGE_IMAGE 能力（GGX 预滤波 compute 写 dst 前提）");
	if (auto* logger = RLLogger::getInstance())
		logger->info("env_prefilter: SPV 就绪（{} 字节），heap shader object 待 Resources 喂槽后建",
			code.size() * sizeof(uint32_t));
}

/** 契约见接口注释。实现：仅记 heap 引用 + sampler 槽（不拥有 heap）。幂等由调用方
 * （Environment::ensurePrefilterReady 的 envPrefilterBound 守卫）保证——重复调用覆盖同值无害。 */
void EnvPrefilter::init(const VmaDescriptorHeap& resourceHeapIn,
	const VmaDescriptorHeap& samplerHeapIn, uint32_t samplerSlotIn) {
	resourceHeap = &resourceHeapIn;
	samplerHeap = &samplerHeapIn;
	samplerSlot = samplerSlotIn;
}

/** 契约见接口注释。实现：经共享点 shader.cppm::makePerLevelComputeShader 建 shader object
 * （heap 尺寸由本类持有的两个 heap 现取）+ 登记 src/dst 槽 + 追加 per-level 描述符链
 * （deque 尾追加——元素地址不迁移，pView 指针稳定活过提交）。level index = mip level
 * （0 占位不用于预滤波生成，但同批建好以便按下标直接取）。 */
std::expected<void, Error> EnvPrefilter::addLevel(uint32_t srcSlot, uint32_t dstSlot) {
	auto shader = makePerLevelComputeShader(device, code, srcSlot, dstSlot, samplerSlot,
		resourceHeap->getDescriptorSize(), samplerHeap->getDescriptorSize(), levels, "env_prefilter");
	if (!shader)
		return std::unexpected(std::move(shader).error());
	shaders.push_back(std::move(*shader));
	srcSlots.push_back(srcSlot);
	dstSlots.push_back(dstSlot);
	srcChains.emplace_back();
	dstChains.emplace_back();
	++levels;
	return {};
}

/** 契约见接口注释。实现：填 chain（viewCreate/imageDesc/resourceDesc/dst 全在 chain 内，互指
 * 地址稳定）+ vkWriteResourceDescriptorsEXT 写进 resource heap 该 level 固定槽。pView 指
 * chain.viewCreate（v341 契约：create info 值，非 VkImageView 句柄）。viewType/baseMip/
 * levelCount/baseLayer/layerCount 由调用方按 src（cube [srcBaseMip, L)）/ dst（2DArray 第
 * level 级）传入。🔴 src 的 levelCount 必须 > 1：视图只含一级时逐样本 LOD 恒等于读那一级，
 * **静默退回旧行为**（画面只是"糊一点/闪一点"，没有任何报错）。 */
std::expected<void, Error> EnvPrefilter::writeImageDesc(DescChain& chain, uint32_t slot,
	VkImage image, vk::Format format, vk::ImageViewType viewType, uint32_t baseMip,
	uint32_t levelCount, uint32_t baseLayer, uint32_t layerCount, vk::DescriptorType type,
	vk::ImageLayout layout) {
	chain.viewCreate = vk::ImageViewCreateInfo{};
	chain.viewCreate.image = image;
	chain.viewCreate.viewType = viewType;
	chain.viewCreate.format = format;
	chain.viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, baseMip, levelCount, baseLayer, layerCount};
	chain.imageDesc = vk::ImageDescriptorInfoEXT{};
	chain.imageDesc.pView = &chain.viewCreate;
	chain.imageDesc.layout = layout;
	chain.resourceDesc = vk::ResourceDescriptorInfoEXT{};
	chain.resourceDesc.type = type;
	chain.resourceDesc.data.pImage = &chain.imageDesc;
	auto addr = resourceHeap->getSlotCpuAddress(slot);
	if (!addr)
		return std::unexpected(std::move(addr).error());
	chain.dst = vk::HostAddressRangeEXT{*addr, resourceHeap->getDescriptorSize()};
	device.writeResourceDescriptorsEXT(chain.resourceDesc, chain.dst);
	return {};
}

/** 契约见接口注释。实现：先各 level 的 src（cube [srcBase, L) sampled SRO，srcBase > level）
 * /dst（2DArray 第 level 级 storage GENERAL）描述符一次性写各自固定槽（同 submission 内独立、
 * 互不覆盖），再 heap host barrier → 一次 bind resource/sampler heap → 逐 level { 第 level 级
 * SRO→GENERAL → bindShadersEXT（该 level）→ pushDataEXT（20B EnvPrefilterPush + @20 push index
 * 0）→ dispatch (ceil(fs/8),ceil(fs/8),6) → 第 level 级 GENERAL→SRO }。视图范围精确到
 * 「src = 本级之后、dst = 本级」，两者不重叠 ⟹ VVL 逐 subresource 布局检查干净，且逐样本 LOD
 * 结构上读不到正在写的级。**前置**：调用方已把 level 1..L−1 填成盒式降采样链并留在 SRO
 *（texture_upload_cubemap::recordGgxMipChain 的 LINEAR blit 段）。仅 float（RGBA16F，
 * sRGB cube 不预滤波走 blit）。 */
std::expected<void, Error> EnvPrefilter::recordPrefilter(vk::raii::CommandBuffer& cmd,
	VkImage cubeImage, vk::Format format, uint32_t faceSize, uint32_t mipLevels, const char* name) {
	if (mipLevels <= 1)
		return {};
	if (resourceHeap == nullptr || samplerHeap == nullptr)
		return std::unexpected(Error{ErrorCode::Unknown,
			"env_prefilter: heap 未 init（ensureEnvPrefilterReady 未跑）"});
	if (levelCount() < mipLevels)
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("env_prefilter: level shader 不足（有 {} 需 {}）", levelCount(), mipLevels)});

	// 源视图区间（**逐样本 LOD 的全部风险都在这几行**，理由 → env_prefilter.comp.slang 文件头）：
	// 起点恒 > level ⟹ 视图结构上看不到正在写的那一级，"同一 dispatch 内读写同一纹素"写不出来。
	// 末两级没有"更糊的下一级"可用（level+1 越界 / 只剩自身），退到读 mipLevels−2：
	// level = L−2 读末级（盒式，本 pass 未碰过）、level = L−1 读 L−2（上一轮 GGX 结果，已 SRO）。
	// 描述符循环与 dispatch 循环共用本 lambda —— 两处各写一遍公式，一处改了另一处静默错位
	//（视图起点与 push 里的 srcBaseMip 不一致，shader 照 push 减，画面只是"糊得不对"）。
	struct SrcView {
		uint32_t base;   ///< 源视图起始 mip
		uint32_t count;  ///< 源视图 mip 跨度
	};
	const auto srcView = [mipLevels](uint32_t level) -> SrcView {
		if (level + 1u < mipLevels)
			return {level + 1u, mipLevels - (level + 1u)};
		return {mipLevels - 2u, 1u};
	};

	// 阶段 1：各 level 的 src（cube [srcBase, L) sampled SRO）/dst（2DArray 第 level 级 storage
	// GENERAL）image 描述符写进各自固定槽（host 写；同 submission 内各 level 独立槽互不覆盖）
	for (uint32_t level = 1; level < mipLevels; ++level) {
		const SrcView view = srcView(level);
		// src：cube 自 view.base 起的全部剩余级（viewType=Cube，6 层，采样 SRO）
		// 🔴 levelCount 不是 1：= 1 时逐样本 LOD **静默失效**（恒读那唯一一级），见 writeImageDesc
		auto s = writeImageDesc(srcChains[level], srcSlots[level], cubeImage, format,
			vk::ImageViewType::eCube, view.base, view.count, 0, 6, vk::DescriptorType::eSampledImage,
			vk::ImageLayout::eShaderReadOnlyOptimal);
		if (!s)
			return s;
		// dst：cube 第 level 级（viewType=2DArray，baseMip=level，6 层，storage GENERAL）
		auto d = writeImageDesc(dstChains[level], dstSlots[level], cubeImage, format,
			vk::ImageViewType::e2DArray, level, 1, 0, 6, vk::DescriptorType::eStorageImage,
			vk::ImageLayout::eGeneral);
		if (!d)
			return d;
	}

	const auto emitBarrier = [&](const vk::ImageMemoryBarrier2& b) {
		VkDependencyInfo dep{};
		dep.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dep.imageMemoryBarrierCount = 1;
		dep.pImageMemoryBarriers = reinterpret_cast<const VkImageMemoryBarrier2*>(&b);
		::vkCmdPipelineBarrier2(*cmd, &dep);
	};

	// heap host barrier（host 写描述符 + 共享 sampler → compute heap 读；eHost 不能在 render
	// pass 内，本上传 CB 全程 pass 外——坑 8）
	vk::MemoryBarrier2 heapBarrier{};
	heapBarrier.srcStageMask = vk::PipelineStageFlagBits2::eHost;
	heapBarrier.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
	heapBarrier.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	heapBarrier.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT |
		vk::AccessFlagBits2::eSamplerHeapReadEXT;
	vk::DependencyInfo heapDep{};
	heapDep.setMemoryBarriers(heapBarrier);
	cmd.pipelineBarrier2(heapDep);

	// bind heap + sampler heap（一次，覆盖本 CB 全部 dispatch；reserved 声明 [0, reservedSlots)
	// 不可用——本模块用的都是 ≥ reservedSlots 的用户槽）
	const uint32_t descriptorSize = resourceHeap->getDescriptorSize();
	vk::DeviceAddressRangeEXT heapRange{resourceHeap->getGpuAddress(), resourceHeap->getHeapSize()};
	cmd.bindResourceHeapEXT(vk::BindHeapInfoEXT(heapRange, 0,
		resourceHeap->getReservedSlots() * descriptorSize));
	vk::DeviceAddressRangeEXT samplerRange{samplerHeap->getGpuAddress(), samplerHeap->getHeapSize()};
	cmd.bindSamplerHeapEXT(vk::BindHeapInfoEXT(samplerRange, 0,
		samplerHeap->getReservedSlots() * samplerHeap->getDescriptorSize()));

	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	for (uint32_t level = 1; level < mipLevels; ++level) {
		const uint32_t levelFs = std::max(1u, faceSize >> level);
		const SrcView view = srcView(level);

		// level i：SHADER_READ_ONLY→GENERAL（本级的**唯一**用途转换；初值 SRO = 调用方的盒式
		// blit 链留下的，不再是 UNDEFINED——level ≥1 现在必须先有内容，逐样本 LOD 才读得到）。
		// srcAccess 带 SampledRead：本级在**更早的 dispatch** 里被当作源读过（level < i 的视图
		// 覆盖 [level+1, L) ⊇ {i}）⟹ 这是 WAR 依赖，缺它 = 读写重叠（syncval hazard）。
		emitBarrier(RGL::createImageBarrier(cubeImage, vk::ImageLayout::eShaderReadOnlyOptimal,
			vk::ImageLayout::eGeneral, vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eComputeShader,
			vk::AccessFlagBits2::eShaderSampledRead, vk::AccessFlagBits2::eShaderWrite,
			level, 1, 0, 6));
		// bind 该 level 的 shader object（烘焙到本 level 的固定 src/dst 槽）+ push data + dispatch
		std::array<vk::ShaderEXT, 1> handles = {*shaders[level]};
		cmd.bindShadersEXT(stages, handles);
		// 内层 EnvPrefilterPush 用指定初始化器 = 让"shader 里改字段名/换序"在**本调用点**编译期红
		//（位置聚合初始化与字段名无关，实测不红；C++20 另要求指定初始化按声明序 ⟹ 换序也红）。
		// 外层 24B 包装不是生成物类型（@20 是本模块 heap push index），保持位置初始化不动。
		const EnvPrefilterPushData pushData{
		    EnvPrefilterPush{.level = level, .mipLevels = mipLevels, .faceSize = levelFs,
		        .srcFaceSize = faceSize, .srcBaseMip = view.base},
		    0u};
		cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(
			reinterpret_cast<const std::byte*>(&pushData), sizeof(pushData))));
		cmd.dispatch((levelFs + 7) / 8, (levelFs + 7) / 8, 6);
		// level i：GENERAL→SHADER_READ_ONLY（渲染端最终布局；也是**下一个 dispatch 能把它当
		// 盒式源读**的前提，与上面那条 SRO→GENERAL 成对）
		emitBarrier(RGL::createImageBarrier(cubeImage, vk::ImageLayout::eGeneral,
			vk::ImageLayout::eShaderReadOnlyOptimal, vk::ImageAspectFlagBits::eColor,
			vk::PipelineStageFlagBits2::eComputeShader, vk::PipelineStageFlagBits2::eFragmentShader,
			vk::AccessFlagBits2::eShaderWrite, vk::AccessFlagBits2::eShaderSampledRead,
			level, 1, 0, 6));
	}
	if (auto* logger = RLLogger::getInstance())
		logger->debug("env_prefilter {}: {} 级 GGX 预滤波（{} 次 dispatch，heap shader object，2DArray dst，"
			"逐样本 LOD 源 = 盒式 mip 链）",
			name, mipLevels - 1, mipLevels - 1);
	return {};
}

}  // namespace RGL
