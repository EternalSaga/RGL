module;
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <fmt/format.h>
export module vk_resources;

import rlstd;
import rlvk;
import rllogger;
import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import vkerror;
import slot_ledger;  // sampler heap 用户槽申报制台账（2026-10-04 槽相撞事故护栏，纯逻辑在 rlbase）

namespace RGL {

/** EVSM 指数 c（批 11.6 EVSSM，**唯一权威源**）：矩编码 z = exp(c·(2d−1))。
 *  定值 5.5 由 RGBA16F 上限双约束 exp(2c) ≤ 65504 决定（Filament PCSS 同值，
 *  docs/evssm_plan.md §1）。消费方：EvssmShadow push constant（moments compute）、
 *  fallback far 最大矩（ensureEvssmSampleSlot 下方）；shadow.slang 消费端经
 *  scene data 阴影参数区的 evssmConst.x（迁移批 S-2 前是独立常量块，现经
 *  readShadowEvssmExponent）取本值
 *  ——shader 侧写字面量即双源红线。 */
export inline constexpr float kEvssmExponent = 5.5f;

/** descriptor heap 用户槽用途段（段间隔离，段内单调分配）。 */
export enum class SlotSegment : uint8_t {
	Geometry,  // 内建几何与模型顶点块（AssetTable::expectedGeometrySlot 契约段）
	Texture,   // 纹理 / cubemap 采样描述符（首槽 = 白纹理契约）
	Runtime,   // scene data、RT 采样槽、mip-gen、env-prefilter、cull 等运行期辅助槽
};

/**
 * IZB 四描述符槽号（批 11.3-1d 后半，`Resources::ensureIzbSlots` 发号）。
 * 哨兵 = 全 0xFFFFFFFF（未领）；槽号终身稳定（管线条目 bake 依赖，见 ensureIzbSlots 注释）。
 */
export struct IzbSlots {
	uint32_t head = 0xFFFFFFFFu;    ///< head[cell]（uint[G²]，NONE 哨兵）
	uint32_t nodes = 0xFFFFFFFFu;   ///< 节点 16B/像素 (u, v, lightDepth, next)
	uint32_t primed = 0xFFFFFFFFu;  ///< early-Z 每 cell 最大光深度（1e 消费）
	uint32_t mask = 0xFFFFFFFFu;    ///< 遮挡位图 1 bit/屏幕像素（izb_mask.frag 写）

	/**
	 * 四槽是否均已发放。
	 * @return true = ensureIzbSlots 已完成发放
	 */
	[[nodiscard]] bool allocated() const { return head != 0xFFFFFFFFu; }
};

/**
 * GPU 资源内核（main 启动流初始化层构造顺序第 4 站：AppContext→DeviceContext→Swapchain→
 * Resources→ShaderRegistry→Renderer，见 main_impl.cppm）：持有 VmaAllocatorRAII + 深度图/
 * view + 两个 descriptor heap（resource / sampler，VK_EXT_descriptor_heap）。
 * **内核不认识任何内容**（架构债务 1+2 重构）：
 * 构造期不创建任何内容资产——几何（内建 cube / gltf 模型）、纹理、RT 采样槽全部由
 * 内容层（builtin_geometry / model_importer / texture_pool / ShaderRegistry）经
 * allocSlot 按用途段领取。已拆出的寄生职责各有服务（架构债务续账 2026-09-19）：
 * SceneData buffer → SceneDataService（scene_data）、共享 sampler + mip-gen 供给 →
 * SamplerLibrary（sampler_library）、env/IBL/sky sampler/EnvPrefilter →
 * Environment（environment）、AssetTable 实例 → main 装配层（架构债务续账 F）。
 * **成员生命周期规则**（AGENTS.md"重要成员生命周期规则"）：raii/GPU 对象
 *（depthImage / depthImageView / descriptorHeap / samplerHeap）必须经成员初始化
 * 列表初始化，绝不在函数体内初始化；成员声明顺序 = 初始化依赖顺序（三个引用 →
 * allocator → depthImage → depthImageView → descriptorHeap / samplerHeap →
 * slot 分配段）。长生命周期 GPU 资源不得声明在 main() 局部块内——块尾析构 =
 * 第 0 帧起 GPU 采样已释放 VA（UAF 教训，见 docs/archive/heap_texture_research.md §12）。
 */
export class Resources {
   public:
	/**
	 * 构建资源层（全部逻辑在成员初始化列表，按成员声明序）：vkInstance → vkDevice →
	 * vkSwapchain → allocator（VMA，BDA flag）→ depthImage（createDepthImage，extent =
	 * swapchain）→ depthImageView → descriptorHeap（createDescriptorHeap）→
	 * samplerHeap（createSamplerHeap）→
	 * slot 分配段（geometry / texture / runtime 三段游标）。
	 * heap 容量与场景内容解耦（E1.3，批次 3）：按用途段固定容量，不按内容精确预分配。
	 * 用户槽位布局（相对 reserved range 起点）：
	 *   [0, kGeometrySlotCapacity)                    几何段（内建几何顶点/索引 + 模型拼接
	 *                                                 顶点块；基址契约见 AssetTable::expectedGeometrySlot）
	 *   [kGeometrySlotCapacity, +kTextureSlotCapacity) 纹理段（首槽 = 白 1×1 特例，契约断言）
	 *   [kGeometrySlotCapacity + kTextureSlotCapacity, +kRuntimeSlotCapacity) 运行时段
	 *                                                 （scene data / RT 采样槽 / mip-gen / env-prefilter / cull）
	 *   运行时扩容 = 重建 heap + 重写全部描述符
	 * @param vkInstance 实例层（allocator 创建依赖其 raii Instance）
	 * @param vkDevice 设备层（heap 属性查询 / 描述符写入 / 深度图创建）
	 * @param vkSwapchain 交换链层（深度图 extent）
	 * @note createDescriptorHeap / createSamplerHeap 失败经 .value() 抛出（启动期一次性路径）
	 */
	Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain);

	/** @return 深度图 const 引用（VmaImage，eD32Sfloat，extent = 交换链 extent）；
	 * Renderer 作 depth 附件渲染目标。 */
	const RGL::VmaImage& getDepthImage() const { return depthImage; }
	/** @return 深度图 view const 引用（raii ImageView，eDepth aspect）；Renderer 绑定
	 * depth 附件用。 */
	const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }

	/**
	 * 重建深度图 + view（**渲染** extent 变化后旧深度图不可复用）：先 device waitIdle
	 *（双帧在飞下旧深度图可能仍被另一帧命令引用），再按新 extent 重建 depthImage 并
	 * 同步重建 depthImageView。
	 * @param extent 新的**渲染** extent（= `Swapchain::getRenderExtent()`，批 14.3 起不再
	 *   等于交换链 extent；传交换链 extent 会让深度图与颜色附件网格不一致）
	 * @return expected<void, Error>；Error = VMA 图像创建失败（createDepthImage 透传）
	 * @note 调用方：resizeSystem（ecs_impl，窗口 resize 且 swapchain.recreate 返回
	 *   true 后）/ Renderer::recreateSwapchainAndDepth（acquire/present OUT_OF_DATE）
	 */
	[[nodiscard]] std::expected<void, Error> recreateDepthImage(vk::Extent2D extent) {
		vkDevice.getDevice().waitIdle();

		auto result = createDepthImage(allocator, extent);
		if (!result) return std::unexpected(std::move(result).error());
		depthImage = std::move(*result);
		depthImageView = vk::raii::ImageView(vkDevice.getDevice(), buildDepthView(*depthImage));
		if (depthSampleSlot != kNoDepthSampleSlot)  // SSAO 槽已领（startup 早于首次 resize）
			writeDepthSampleDescriptor();
		return {};
	}

	/**
	 * 深度图采样描述符槽（Phase 11 批 11.4 SSAO）：把 depthImageView 以
	 * SAMPLED_IMAGE（eDepth aspect）写进 resource heap 的 Runtime 段槽，供 ao.frag
	 * 读深度重建世界坐标。**幂等**（二次调用返回同槽号），槽号终身有效——resize 换图
	 * 时由 recreateDepthImage 重写描述符，已 bake 进管线条目的 binding 不失效。
	 * @return 槽号；Error = 槽段耗尽 / 描述符地址解析失败
	 */
	[[nodiscard]] std::expected<uint32_t, Error> ensureDepthSampleSlot() {
		if (depthSampleSlot != kNoDepthSampleSlot)
			return depthSampleSlot;
		auto slot = allocSlot(RGL::SlotSegment::Runtime, "depth.sample");
		if (!slot)
			return std::unexpected(std::move(slot).error());
		depthSampleSlot = *slot;
		if (auto addr = descriptorHeap.getSlotCpuAddress(depthSampleSlot); !addr)
			return std::unexpected(std::move(addr).error());
		else
			writeDepthSampleDescriptor();
		return depthSampleSlot;
	}

	/**
	 * IZB 四描述符槽（批 11.3-1d 后半）：head/nodes/primed/mask 的 Runtime 段槽号。
	 * **只发号不写描述符**（内容 = IzbShadow 的 buffer，随 extent 重建换 BDA 重写同槽）
	 * ——槽号必须终身稳定：管线条目（addIzbMaskEntry）启动期 bake 槽号，早于且晚于
	 * 任意次 IzbShadow 重建。幂等。
	 * @return 四槽号；Error = 槽段耗尽
	 */
	[[nodiscard]] std::expected<IzbSlots, Error> ensureIzbSlots() {
		if (izbSlots.allocated())
			return izbSlots;
		struct { uint32_t* dst; const char* name; } takes[] = {
			{&izbSlots.head, "izb.head"}, {&izbSlots.nodes, "izb.nodes"},
			{&izbSlots.primed, "izb.primed"}, {&izbSlots.mask, "izb.mask"},
		};
		for (auto& take : takes) {
			auto slot = allocSlot(RGL::SlotSegment::Runtime, take.name);
			if (!slot)
				return std::unexpected(std::move(slot).error());
			*take.dst = *slot;
		}
		// mask 槽 fallback 描述符（批 11.3-1e）：light.frag binding 14 **无条件绑定**
		// （VVL 静态活性），门关 = IzbShadow 不构造时读 16B 零 buffer（flag 分支也
		// 不读值，但 descriptor 必须有效）。IzbShadow 构造时以真位图覆写同槽。
		if (!izbMaskFallback) {
			vk::BufferCreateInfo fi{};
			fi.size = 16;
			fi.usage = vk::BufferUsageFlagBits::eStorageBuffer
				| vk::BufferUsageFlagBits::eShaderDeviceAddress;
			fi.sharingMode = vk::SharingMode::eExclusive;
			VmaAllocationCreateInfo fa{};
			fa.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
			fa.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
			VmaAllocationInfo fad{};
			auto buf = allocator.createBufferWithAddress(vkDevice.getDevice(), fi, fa, &fad);
			if (!buf)
				return std::unexpected(std::move(buf).error());
			auto* bytes = static_cast<std::byte*>(fad.pMappedData);
			for (uint32_t i = 0; i < 16; ++i)
				bytes[i] = std::byte{};
			vk::DeviceAddressRangeEXT range{buf->getDeviceAddress(), 16};
			vk::ResourceDescriptorInfoEXT desc{};
			desc.type = vk::DescriptorType::eStorageBuffer;
			desc.data.pAddressRange = &range;
			auto addr = descriptorHeap.getSlotCpuAddress(izbSlots.mask);
			if (!addr)
				return std::unexpected(std::move(addr).error());
			vkDevice.getDevice().writeResourceDescriptorsEXT(desc,
				vk::HostAddressRangeEXT{*addr, descriptorHeap.getDescriptorSize()});
			izbMaskFallback = std::move(*buf);
		}
		return izbSlots;
	}

	/**
	 * EVSSM 矩图采样槽号（批 11.6d-2，ensureIzbSlots 同盘：只发号不写描述符——
	 * 内容 = EvssmShadow 的矩图，其构造时以真 view 覆写同槽；重建换图重写同槽）。
	 * 槽号终身稳定（管线条目 binding 15 启动期 bake）。幂等。
	 * 门关 = 槽存在但 variant SPV 不声明 binding 15 = 批 M 过滤不烘（binding 4/5 先例）。
	 * @return 槽号；Error = 槽段耗尽
	 */
	[[nodiscard]] std::expected<uint32_t, Error> ensureEvssmSampleSlot() {
		if (evssmSampleSlot != kNoEvssmSampleSlot)
			return evssmSampleSlot;
		auto slot = allocSlot(RGL::SlotSegment::Runtime, "evssm.moments");
		if (!slot)
			return std::unexpected(std::move(slot).error());
		evssmSampleSlot = *slot;
		// fallback 描述符（izbMaskFallback 同思路，批 11.3-1e）：-DEVSSM_SOFT 变体
		// **静态**声明 binding 15（VVL 静态活性），而 EvssmShadow 仅在门开 + deferred +
		// shadow pass 时构造——forward 等配置组合下槽也必须有效。
		// ⚠️ fallback 内容 = **far 面最大矩 (e^c, e^2c, −e^−c, e^−2c)**，不是零！
		// 「无遮挡 = 全亮」的数学身份就是最大矩：receiver pw ≤ e^c 恒成立 → Chebyshev
		// 快路（depth ≤ moments.x）命中 → 受光量 1。零矩是反方向（m0=0 → d=pw>0
		// 处处成立 → 恒暗），forward 门开实测 176821 px **变暗**坐实（2026-10-02；
		// 本注释初版写「零矩=全亮」是错的）。矩值由 kEvssmExponent（本文件顶部，
		// 唯一权威源）现场推导，无字面量。
		if (!evssmFallback) {
			vk::ImageCreateInfo fi{};
			fi.imageType = vk::ImageType::e2D;
			fi.format = vk::Format::eR16G16B16A16Sfloat;
			fi.extent = vk::Extent3D(1, 1, 1);
			fi.mipLevels = 1;
			fi.arrayLayers = 1;
			fi.samples = vk::SampleCountFlagBits::e1;
			fi.tiling = vk::ImageTiling::eOptimal;
			fi.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst;
			fi.sharingMode = vk::SharingMode::eExclusive;
			VmaAllocationCreateInfo fa{};
			fa.usage = VMA_MEMORY_USAGE_AUTO;
			auto img = allocator.createImage(fi, fa);
			if (!img)
				return std::unexpected(std::move(img).error());
			evssmFallback = std::move(*img);
			// 一次性：UNDEFINED→GENERAL + clear **far 面最大矩** + 保持 GENERAL
			// （descriptor 声明 GENERAL，采样 GENERAL 合法——与 EvssmShadow 矩图同布局策略）
			const float zFar = std::exp(kEvssmExponent);  // e^c ≈ 244.69
			auto cmdBufs = vkDevice.getDevice().allocateCommandBuffers(
				{*vkDevice.getGraphicsCommandPool(), vk::CommandBufferLevel::ePrimary, 1});
			auto& cmd = cmdBufs[0];
			cmd.begin({});
			vk::ImageMemoryBarrier2 toGeneral{};
			toGeneral.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
			toGeneral.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
			toGeneral.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
			toGeneral.oldLayout = vk::ImageLayout::eUndefined;
			toGeneral.newLayout = vk::ImageLayout::eGeneral;
			toGeneral.image = evssmFallback->image;
			toGeneral.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			vk::DependencyInfo dep{};
			dep.setImageMemoryBarriers(toGeneral);
			cmd.pipelineBarrier2(dep);
			cmd.clearColorImage(evssmFallback->image, vk::ImageLayout::eGeneral,
				vk::ClearColorValue(zFar, zFar * zFar, -1.0f / zFar, 1.0f / (zFar * zFar)),
				vk::ImageSubresourceRange{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
			cmd.end();
			vk::raii::Fence fence(vkDevice.getDevice(), vk::FenceCreateInfo{});
			const VkCommandBuffer raw = *cmd;
			VkSubmitInfo si{};
			si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
			si.commandBufferCount = 1;
			si.pCommandBuffers = &raw;
			if (::vkQueueSubmit(vkDevice.getGraphicsQueue(), 1, &si, *fence) != VK_SUCCESS)
				return std::unexpected(Error{ErrorCode::VulkanError, "EVSSM fallback 提交失败"});
			if (vkDevice.getDevice().waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX)
				!= vk::Result::eSuccess)
				return std::unexpected(Error{ErrorCode::VulkanError, "EVSSM fallback fence 超时"});
			vk::ImageViewCreateInfo viewCreate{};
			viewCreate.image = evssmFallback->image;
			viewCreate.viewType = vk::ImageViewType::e2D;
			viewCreate.format = vk::Format::eR16G16B16A16Sfloat;
			viewCreate.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
			vk::ImageDescriptorInfoEXT imgDesc{};
			imgDesc.pView = &viewCreate;
			imgDesc.layout = vk::ImageLayout::eGeneral;
			vk::ResourceDescriptorInfoEXT resDesc{};
			resDesc.type = vk::DescriptorType::eSampledImage;
			resDesc.data.pImage = &imgDesc;
			const auto addr = descriptorHeap.getSlotCpuAddress(evssmSampleSlot);
			if (!addr)
				return std::unexpected(std::move(addr).error());
			vkDevice.getDevice().writeResourceDescriptorsEXT(resDesc,
				vk::HostAddressRangeEXT{*addr, descriptorHeap.getDescriptorSize()});
		}
		return evssmSampleSlot;
	}

	/**
	 * FSR 输出图的采样堆槽号（Phase 14 批 14.5 第二批）。
	 *
	 * 与 `ensureEvssmSampleSlot` 同族：**只发号不写描述符**——内容 = `FsrUpscaler` 的私有
	 * 输出图，由它构造时以真 view 覆写同槽（渲染网格变化重建后重写同槽）。
	 *
	 * 与 EVSSM 那条的**关键差别 = 不写 fallback 描述符**：EVSSM 的 binding 15 在 SPV 里是
	 * **静态**声明的（任何配置组合下都必须有效），而本槽是**启动期烘进 tonemap 管线条目**
	 * 的色输入槽号——装配层只在全部前置都已核实（门开 + velocity 进图 + shader16 + 三个
	 * 输入附件都在图里）时才烘它，所以「槽已烘进条目却无人写描述符」这个形态被装配层
	 * 挡死，不需要 fallback 兜底（`FsrUpscaler` 因此改在 Renderer 构造期建，不留窗口）。
	 *
	 * 幂等。@return 槽号；Error = 槽段耗尽
	 */
	[[nodiscard]] std::expected<uint32_t, Error> ensureFsrSampleSlot() {
		if (fsrSampleSlot != kNoFsrSampleSlot)
			return fsrSampleSlot;
		auto slot = allocSlot(RGL::SlotSegment::Runtime, "fsr.output");
		if (!slot)
			return std::unexpected(std::move(slot).error());
		fsrSampleSlot = *slot;
		return fsrSampleSlot;
	}

	/** @return resource heap const 引用（VmaDescriptorHeap：BDA buffer 视为连续描述符槽
	 * 数组，步长 = max(buffer, image) descriptorSize 运行时查询）；Renderer 每帧绑定，
	 * 内容层经 allocSlot + getSlotCpuAddress 写描述符。 */
	const RGL::VmaDescriptorHeap& getDescriptorHeap() const { return descriptorHeap; }
	/** @return sampler heap const 引用（OpTypeSampler 独立 heap，Renderer 每帧经
	 * vkCmdBindSamplerHeapEXT 绑定）；共享 sampler（SamplerLibrary）/ sky sampler
	 *（Environment）槽位在其中。 */
	const RGL::VmaDescriptorHeap& getSamplerHeap() const { return samplerHeap; }
	/** @return VMA 分配器 const 引用（const 重载，与下方非 const 重载同一对象）；
	 * 创建 GPU 资源走非 const 重载。 */
	const RGL::VmaAllocatorRAII& getAllocator() const { return allocator; }
	/**
	 * 显存账（批 9.5 观测面）：把"谁吃了多少显存"打成**可回归的启动日志**，而不是散在
	 * 四份文档注释里的预算数字（EVSSM 44.7MB / 阴影图集 16MB / …）。
	 *
	 * 三行：① 总量（VkDeviceMemory 块数 / 块字节 / 分配字节 / 空洞）② 逐 heap
	 * （含 VK_EXT_memory_budget 的 usage/budget，缺扩展时为 0）③ 逐内存类型
	 * （只打非空类型，带 property flags 位）。数据源 = `vmaCalculateStatistics`
	 * （比 `vmaBuildStatsString` 的整棵 JSON 树紧凑，且无需 vmaFreeStatsString）。
	 *
	 * 成本：VMA 侧全块遍历（"slower to calculate"），**只准启动期/显式诊断调用，
	 * 禁止进帧循环**。
	 * @param stage 标签（进日志前缀，如 "startup" / "after-frame1"）
	 */
	void logMemoryStats(std::string_view stage) const;
	/** @return resource heap 保留区偏移，恒 0：reserved 槽始终在 heap 头部
	 * [0, reservedSlots)（VmaDescriptorHeap 布局，docs/DESCRIPTOR_HEAP_SPEC.md §九.2）。 */
	vk::DeviceSize getReservedRangeOffset() const { return 0; }
	/** @return resource heap 保留区字节数 = reservedSlots × descriptorSize（≥ 设备
	 * minResourceHeapReservedRange；vkCmdBindResourceHeapEXT 的 reservedRange 声明为
	 * 不可用，如 NVIDIA 96768B）。 */
	vk::DeviceSize getReservedRangeSize() const { return descriptorHeap.getReservedSlots() * descriptorHeap.getDescriptorSize(); }
	/** @return sampler heap 绑定的保留区字节数 = reservedSlots × samplerDescriptorSize
	 *（同 resource heap 语义：reserved 槽声明为不可用，vkCmdBindSamplerHeapEXT 用）。 */
	vk::DeviceSize getSamplerReservedRangeSize() const { return samplerHeap.getReservedSlots() * samplerHeap.getDescriptorSize(); }

	/** @return VMA 分配器引用（非 const 重载，与上方 const 重载同一对象）；内容层上传
	 * 路径经它创建 buffer/image（builtin_geometry / model_importer / texture_upload /
	 * gpu_canary / Renderer staging）。 */
	/**
	 * 分用途段 slot 分配器：每个 SlotSegment 在 descriptor heap 内拥有独立连续段，
	 * 段内单调分配；跨段互不抢占，避免运行时辅助槽插入几何/纹理契约段。
	 * @param segment 槽用途段
	 * @param name 槽用途名（仅日志，如 "scene.data" / RT 名）
	 * @return 槽号；段容量越界 → DescriptorSlotOutOfBounds 错误
	 */
	[[nodiscard]] std::expected<uint32_t, Error> allocSlot(SlotSegment segment, const char* name) {
		const size_t i = segmentIndex(segment);
		if (nextSlots[i] >= segmentEnds[i])
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				fmt::format("{} slot capacity exceeded ({} >= {}): {}",
					segmentName(segment), nextSlots[i], segmentEnds[i], name)});
		auto* logger = RLLogger::getInstance();
		const uint32_t slot = nextSlots[i]++;
		if (logger) logger->debug("descriptor heap slot {}: {} ({})", slot, name, segmentName(segment));
		return slot;
	}

	/** @return 指定用途段的起始槽号（如纹理段首槽 = 白纹理契约槽）。 */
	[[nodiscard]] uint32_t segmentBegin(SlotSegment segment) const { return segmentBegins[segmentIndex(segment)]; }

	/**
	 * sampler heap 用户槽**申报制**领槽（2026-10-04 env sampler 槽相撞事故的护栏）。
	 *
	 * 旧形态是各使用者自己算 `reservedSlots + N`：`Environment::ensureSkySampler` 与
	 * `SamplerLibrary::ensurePointSampler` 双双算出 `+1`，两处注释都自称"共享 sampler 之后
	 * 第二槽"，谁都没错、撞了没人管；而两个 ensure* 的幂等守卫让**后写者永久赢** ⟹ env
	 * 镜面项拿到 AO 的 NEAREST + maxLod=0 sampler，GGX 预滤波 mip 链从未被采样（判据：
	 * `env.mipRange` 扫 0..20 画面逐字节不变，而同一次 memcpy 里的 `env.intensity` 活得好
	 * 好的）。根因是形态不是笔误，所以这里换机制：**归属者只报名字，槽号由台账发**，
	 * 不同 owner 结构上不可能拿到同一个槽。台账本体 = `slot_ledger`（rlbase，有单测）。
	 *
	 * 幂等：同一 owner 重复领返回同一槽（调用方的 `if (ready) return slot;` 守卫因此
	 * 只是省一次描述符写入，不再承担防相撞职责）。
	 * @param owner 归属者名（进日志、区分大小写整串比较，如 "sampler.skyEnv"）
	 * @return 绝对槽号；容量耗尽 → DescriptorSlotOutOfBounds 并打 error 日志（**不静默**）
	 */
	[[nodiscard]] std::expected<uint32_t, Error> claimSamplerSlot(std::string_view owner) {
		auto slot = samplerSlots.claimSlot(owner);
		if (!slot) {
			if (auto* logger = RLLogger::getInstance())
				logger->error("sampler heap 领槽失败（owner={}，已发放 {} / 容量 {}）：{}",
					owner, samplerSlots.used(), samplerSlots.capacity(), slot.error().message);
			return slot;
		}
		if (auto* logger = RLLogger::getInstance())
			logger->info("sampler heap slot {} := {}（台账现 {} 项：{}）",
				*slot, owner, samplerSlots.used(), samplerSlots.describe());
		return slot;
	}

	/** @return sampler heap 槽位台账全表摘要（启动末尾回归比对用；"shared→0 skyEnv→1 …"）。 */
	[[nodiscard]] std::string samplerSlotTable() const { return samplerSlots.describe(); }

   private:
	/** @return 驱动保留槽位数（用户槽起点，三段分配器基址；NVIDIA 3024 /
	 * RADV·Intel 0，见 createDescriptorHeap）。 */
	uint32_t reservedSlots() const { return descriptorHeap.getReservedSlots(); }

	/** @return SlotSegment 的稳定数组下标（与 enum class 定义顺序一致）。 */
	static constexpr size_t segmentIndex(SlotSegment segment) { return static_cast<size_t>(segment); }

	/** @return 用途段的日志名。 */
	static constexpr const char* segmentName(SlotSegment segment) {
		switch (segment) {
			case SlotSegment::Geometry: return "geometry";
			case SlotSegment::Texture: return "texture";
			case SlotSegment::Runtime: return "runtime";
		}
		return "unknown";
	}

	/** 初始化三段槽位（reserved 之后连续排列；容量 = 段容量常量之和）。 */
	void initializeSlotSegments(uint32_t reserved) {
		segmentBegins[segmentIndex(SlotSegment::Geometry)] = reserved;
		segmentEnds[segmentIndex(SlotSegment::Geometry)] = reserved + kGeometrySlotCapacity;
		segmentBegins[segmentIndex(SlotSegment::Texture)] = segmentEnds[segmentIndex(SlotSegment::Geometry)];
		segmentEnds[segmentIndex(SlotSegment::Texture)] =
			segmentBegins[segmentIndex(SlotSegment::Texture)] + kTextureSlotCapacity;
		segmentBegins[segmentIndex(SlotSegment::Runtime)] = segmentEnds[segmentIndex(SlotSegment::Texture)];
		segmentEnds[segmentIndex(SlotSegment::Runtime)] =
			segmentBegins[segmentIndex(SlotSegment::Runtime)] + kRuntimeSlotCapacity;
		for (size_t i = 0; i < kSlotSegmentCount; ++i) nextSlots[i] = segmentBegins[i];
		if (segmentEnds[segmentIndex(SlotSegment::Runtime)] > descriptorHeap.getCapacity())
			throw std::runtime_error(fmt::format(
				"descriptor heap segment overflow: segment end {} > capacity {}",
				segmentEnds[segmentIndex(SlotSegment::Runtime)], descriptorHeap.getCapacity()));
		if (auto* logger = RLLogger::getInstance())
			logger->debug("descriptor heap segments: geometry [{}, {}) texture [{}, {}) runtime [{}, {})",
				segmentBegins[segmentIndex(SlotSegment::Geometry)], segmentEnds[segmentIndex(SlotSegment::Geometry)],
				segmentBegins[segmentIndex(SlotSegment::Texture)], segmentEnds[segmentIndex(SlotSegment::Texture)],
				segmentBegins[segmentIndex(SlotSegment::Runtime)], segmentEnds[segmentIndex(SlotSegment::Runtime)]);
	}

   private:
	/** 深度图采样槽"未领取"哨兵（kNoDepthSampleSlot 与 allocSlot 段容量无关的独立值）。 */
	static constexpr uint32_t kNoDepthSampleSlot = 0xFFFFFFFFu;
	/** EVSSM 矩图采样槽"未领取"哨兵（批 11.6d-2，同 kNoDepthSampleSlot 惯例）。 */
	static constexpr uint32_t kNoEvssmSampleSlot = 0xFFFFFFFFu;
	static constexpr uint32_t kNoFsrSampleSlot = 0xFFFFFFFFu;

	/**
	 * 把当前 depthImageView 写进 depthSampleSlot 槽（SAMPLED_IMAGE / eDepth aspect）。
	 * 描述符指针链（pView 指 create info，v341 契约）存于成员，随 recreateDepthImage
	 * 换图后重写——槽号不变，故已 bake 的管线 binding 永久有效。
	 */
	void writeDepthSampleDescriptor() {
		depthSampleViewCreate = vk::ImageViewCreateInfo{};
		depthSampleViewCreate.image = *depthImage;
		depthSampleViewCreate.viewType = vk::ImageViewType::e2D;
		depthSampleViewCreate.format = vk::Format::eD32Sfloat;
		depthSampleViewCreate.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
		depthSampleImageDesc = vk::ImageDescriptorInfoEXT{};
		depthSampleImageDesc.pView = &depthSampleViewCreate;  // v341：pView 指 create info
		depthSampleImageDesc.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
		depthSampleResourceDesc = vk::ResourceDescriptorInfoEXT{};
		depthSampleResourceDesc.type = vk::DescriptorType::eSampledImage;
		depthSampleResourceDesc.data.pImage = &depthSampleImageDesc;
		const auto addr = descriptorHeap.getSlotCpuAddress(depthSampleSlot);
		if (!addr)
			return;  // 槽地址解析失败 = heap 契约破损；启动期路径，此处不抛（见调用点）
		vkDevice.getDevice().writeResourceDescriptorsEXT(depthSampleResourceDesc,
			vk::HostAddressRangeEXT{*addr, descriptorHeap.getDescriptorSize()});
	}

	/**
	 * 创建深度图（eD32Sfloat，单采样，eDepthStencilAttachment，VMA 专用内存）。
	 * @param allocator VMA 分配器
	 * @param extent 图 extent（= swapchain extent）
	 * @return VmaImage；VMA 失败 = VMASuccessFailed
	 */
	static std::expected<RGL::VmaImage, Error> createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent);

	/**
	 * 组装深度图 view create info（e2D / eD32Sfloat / eDepth aspect / levelCount =
	 * layerCount = 1）。
	 * @param image 深度图句柄
	 * @return 填好的 ImageViewCreateInfo（局部临时，仅供 raii ImageView 构造消费）
	 */
	static vk::ImageViewCreateInfo buildDepthView(VkImage image);

	/**
	 * 创建 resource descriptor heap。容量 = reserved + 三个用户段容量之和。用户槽位 =
	 * allocSlot 按用途段分配：几何段（内建几何 + 模型顶点块，AssetTable::geometrySlotCount
	 * 计数）→ 纹理段（首槽 = 白，texture_pool.cppm）→ 运行时段（scene data / RT 采样槽 /
	 * mip-gen / env-prefilter / cull）。内核不预留任何具体资产专属槽位，只按用途划分段
	 * （架构债务 1：cube 归还 builtin_geometry）。
	 * @return VmaDescriptorHeap（步长 = max(buffer, image) descriptorSize，运行时查询，
	 *   见 impl）；底层 buffer 创建失败 = VMASuccessFailed
	 */
	std::expected<RGL::VmaDescriptorHeap, Error> createDescriptorHeap();

	/**
	 * 创建 sampler heap（Phase 3）：OpTypeSampler 的独立 heap（vkCmdBindSamplerHeapEXT
	 * 每帧绑定）。背景：pEmbeddedSampler 对 Slang 的 OpTypeImage+OpTypeSampler 分离对
	 * 未获 RADV 采纳（spec 声称同等适用，实测采样恒 0；spike 只验证了 GLSL 组合
	 * sampler2D=OpTypeSampledImage），回退链（docs/archive/phase3_plan.md R1）= 真
	 * sampler heap。容量 = reserved + 固定小裕量（共享一份 sampler 描述符，全模型管线
	 * 映射指向同一槽）。
	 * @return VmaDescriptorHeap（步长 = samplerDescriptorSize，运行时查询，见 impl）；
	 *   底层 buffer 创建失败 = VMASuccessFailed
	 */
	std::expected<RGL::VmaDescriptorHeap, Error> createSamplerHeap();

	RGL::AppContext& vkInstance;  // 实例层引用（最先初始化）；allocator 创建依赖其 raii Instance
	RGL::DeviceContext& vkDevice;  // 设备层引用；heap 属性查询 / 描述符写入 / 深度图创建
	RGL::Swapchain& vkSwapchain;  // 交换链层引用；构造期深度图 extent（resize 后重建经 recreateDepthImage）
	RGL::VmaAllocatorRAII allocator;  // VMA 分配器（BDA flag，见 impl 构造器）；全部 buffer/image/heap 经它创建
	RGL::VmaImage depthImage;  // 深度图（eD32Sfloat；swapchain resize 后重建，重建前 waitIdle）
	vk::raii::ImageView depthImageView;  // 深度图 view（raii，不可拷贝——成员初始化列表构造；与 depthImage 同步重建）
	/** SSAO 深度采样槽（Runtime 段，kNoDepthSampleSlot = 未领）；槽号终身有效，
	 * recreateDepthImage 后经 writeDepthSampleDescriptor 重写指向新 view。 */
	uint32_t depthSampleSlot = kNoDepthSampleSlot;
	/** IZB 四槽号（批 11.3-1d 后半，ensureIzbSlots 发号；内容描述符由 IzbShadow 写）。 */
	IzbSlots izbSlots{};
	/** EVSSM 矩图采样槽号（批 11.6d-2，ensureEvssmSampleSlot 发号；binding 15 无条件
	 * 传入，批 M 按声明过滤烘焙；真描述符由 EvssmShadow 构造时覆写，门关 = variant
	 * 不声明 = 不烘）。 */
	uint32_t evssmSampleSlot = kNoEvssmSampleSlot;
	uint32_t fsrSampleSlot = kNoFsrSampleSlot;  ///< 批 14.5：FSR 输出采样槽（ensureFsrSampleSlot 发号）
	/** EVSSM 矩图槽 fallback 1×1 RGBA16F 零图（批 11.6d-2，发号时创建，同
	 * izbMaskFallback 思路：variant 静态活性 vs EvssmShadow 条件构造的缺口兜底）。 */
	std::optional<RGL::VmaImage> evssmFallback;
	/** mask 槽 fallback 零 buffer（批 11.3-1e，ensureIzbSlots 发号时创建；
	 *  light.frag binding 14 无条件绑定的兜底，IzbShadow 构造后覆写同槽描述符）。 */
	std::optional<RGL::VmaBuffer> izbMaskFallback;
	/** 深度采样描述符指针链（pView 指 create info，v341 契约——故存值不存指针）。 */
	vk::ImageViewCreateInfo depthSampleViewCreate{};
	vk::ImageDescriptorInfoEXT depthSampleImageDesc{};
	vk::ResourceDescriptorInfoEXT depthSampleResourceDesc{};
	RGL::VmaDescriptorHeap descriptorHeap;  // resource heap（用户槽 = 三段 allocSlot，布局契约见构造器注释）
	RGL::VmaDescriptorHeap samplerHeap;  // Phase 3：OpTypeSampler 独立 heap
	/** sampler heap 用户槽申报制台账（2026-10-04 槽相撞护栏）。基址/容量在构造期从
	 * samplerHeap 取（reserved 区不可用），此后只由 claimSamplerSlot 追加。 */
	RGL::SlotLedger samplerSlots;
	/** 三段 slot 分配器：每段独立 [begin, next, end) 游标，跨段不抢占。 */
	static constexpr size_t kSlotSegmentCount = 3;
	std::array<uint32_t, kSlotSegmentCount> segmentBegins{};
	std::array<uint32_t, kSlotSegmentCount> segmentEnds{};
	std::array<uint32_t, kSlotSegmentCount> nextSlots{};
	/** 用户段固定容量（与场景内容解耦；统一步长 = max(buffer, image) descriptorSize，
	 * 运行时查询）。纹理段保留 4096 以容纳 Phase 8 ③a 四元组去重后的 bistro 纹理槽。 */
	static constexpr uint32_t kGeometrySlotCapacity = 4096;
	static constexpr uint32_t kTextureSlotCapacity = 4096;
	static constexpr uint32_t kRuntimeSlotCapacity = 1024;
};

}  // namespace RGL
