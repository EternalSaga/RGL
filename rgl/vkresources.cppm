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

namespace RGL {

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
	 * 重建深度图 + view（交换链 extent 变化后旧深度图不可复用）：先 device waitIdle
	 *（双帧在飞下旧深度图可能仍被另一帧命令引用），再按新 extent 重建 depthImage 并
	 * 同步重建 depthImageView。
	 * @param extent 新的交换链 extent
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
		return izbSlots;
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
	/** 深度采样描述符指针链（pView 指 create info，v341 契约——故存值不存指针）。 */
	vk::ImageViewCreateInfo depthSampleViewCreate{};
	vk::ImageDescriptorInfoEXT depthSampleImageDesc{};
	vk::ResourceDescriptorInfoEXT depthSampleResourceDesc{};
	RGL::VmaDescriptorHeap descriptorHeap;  // resource heap（用户槽 = 三段 allocSlot，布局契约见构造器注释）
	RGL::VmaDescriptorHeap samplerHeap;  // Phase 3：OpTypeSampler 独立 heap
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
