module;
export module render_target_cache;

import rlstd;
import rlvk;
import vkerror;
import VmaRaii;
import vk_device;
import vk_resources;  // 架构债务续账步骤 2：RT 缓存从内核拆出——缓存向内核引 allocator/heap/槽，单向
import pending_evictions;  // B2-4③′ 债-124：frames-in-flight 逐出台账（rlbase 叶子）

namespace RGL {

/**
 * RT 附件的**一个描述符附件项**（多描述符能力单元；EVSSM 矩图迁入时长出）。
 *
 * 一个附件可以有多个描述符（例：矩图 = 每 mip 一个 STORAGE_IMAGE + 一个全 mip
 * SAMPLED_IMAGE），它们共用同一张图、各自持自己的 view 子资源范围 / 描述符类型 /
 * image layout。**本结构体是描述符链的唯一持有者**——原先"一个附件一个采样描述符"
 * 的三个平铺字段（heapSlot / hasSampleDescriptor / sampleViewCreate）已收敛进
 * RtAttachment::descriptors，不留两份真相源。
 *
 * 🔴 指针链生命周期前提（v341 writeResourceDescriptors 语义，同 TextureAsset）：
 * imageDesc.pView → 本项的 viewCreate、resourceDesc.data.pImage → 本项的 imageDesc，
 * 三者都指向**本结构体内部**，所以本结构体的地址必须终身稳定。它住在
 * RtAttachment::descriptors（std::vector）里，而该 vector **只在首建时定容、此后
 * 永不 resize**（重建只原地改字段）——reallocation 会让已交给驱动的指针链悬空。
 * 该不变式由 ensure 的规格对账（项数不符即报错）机器守住，不靠注释自觉。
 */
export struct RtDescriptor {
	vk::ImageViewCreateInfo viewCreate{};  // imageDesc.pView 的稳定指向目标（重建只换 .image 句柄）
	vk::ImageDescriptorInfoEXT imageDesc{};  // data.pImage 链上游（v341：pView 指 create info）
	vk::ResourceDescriptorInfoEXT resourceDesc{};  // 描述符定义（type + data.pImage 链）
	vk::HostAddressRangeEXT dst{};  // 描述符写入目标槽范围（getSlotCpuAddress 解析）
	uint32_t heapSlot = 0;  // 本项的 heap 槽号（首建定号、跨重建复用）
	// B2-4③′ 债-124：本项槽号是否由**缓存**在首建时领取（true = 缓存自有，erase/retire
	// 到期时归还 Resources::freeSlot；false = 外部台账预发号（spec.heapSlot 有值，
	// EVSSM 矩图类），缓存只写描述符，逐出时**不**归还——槽的归属没变）。
	bool slotOwnedByCache = false;
	vk::DescriptorType type = vk::DescriptorType::eSampledImage;  // 描述符类型（采样项的判据）
	vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal;  // 描述符声明的 image layout
	vk::ImageSubresourceRange range{};  // 本项 view 的子资源范围（不随重建变）
};

/**
 * 一个描述符附件项的**规格**（调用方申报，缓存照此领号 + 写描述符）。
 */
export struct RtDescriptorSpec {
	vk::ImageSubresourceRange range{};  // 本项 view 的子资源范围（aspect + mip 段）
	vk::DescriptorType type = vk::DescriptorType::eSampledImage;  // 描述符类型
	vk::ImageLayout layout = vk::ImageLayout::eShaderReadOnlyOptimal;  // 描述符 image layout
	/**
	 * 预发号（有值 = 用外部台账已发的槽号，缓存**不**再领号）。
	 * 存在的理由 = 时序：装配期要把槽号烘进管线条目，而图可能首帧才建（EVSSM 矩图的
	 * 采样槽即此，见 Resources::ensureEvssmSampleSlot）。nullopt = 首建时由缓存
	 * 在 Runtime 段领号（模块侧不再自领槽）。
	 */
	std::optional<uint32_t> heapSlot{};
};

/**
 * 附件规格（image 侧形态 + 描述符清单）。走这个入口 = 多 mip / 自定义 usage /
 * 多描述符。**老四参入口 = 本规格的一份取值**（mipLevels 1 + 默认 usage 档 +
 * 一个全图采样项），它逐字节复刻过去靠把这份取值传进同一个实现来保证——
 * 老路径没有第二条实现可漂。
 */
export struct RtAttachmentSpec {
	vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor;  // 附件主 view 的 aspect
	uint32_t mipLevels = 1;  // 图 mip 数（老入口恒 1）
	/** image usage；**空 = 按 aspect 取既有默认档**（颜色档 = COLOR_ATTACHMENT|TRANSFER_SRC
	 * |TRANSFER_DST|SAMPLED，深度档 = DEPTH_STENCIL_ATTACHMENT|SAMPLED）——老入口正是靠
	 * 这条拿到与过去逐字节相同的 usage，默认档在本文件只写一次 */
	vk::ImageUsageFlags usage{};
	bool dedicatedMemory = false;  // VMA 独占内存（采样图先例；老入口恒 false）
	const char* allocationName = "render_target_cache/rt";  // VMA 调试名（vma 日志归属用）
	/** 描述符清单（逐项 = 领号 + 建 view create info + 写描述符）；**空 = 本附件不写任何
	 * 描述符**（老入口不靠这个默认值，它显式给出那一个全图 SAMPLED_IMAGE 项） */
	std::vector<RtDescriptorSpec> descriptors{};
};

/**
 * offscreen RT 缓存条目（Phase 6 批次 4；架构债务续账步骤 2 随 RenderTargetCache 拆出，
 * 原 Resources::RtAttachment）。key = attachment 名，幂等创建 / extent 失配重建（随
 * swapchain resize，重建前 waitIdle——双帧在飞下旧 RT 可能仍被另一帧命令引用，同
 * recreateDepthImage 规则）。
 * 默认（不传规格）usage = COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST | SAMPLED
 *（blit 上屏与 shader 采样两条消费路都预留）；format 由调用点解析（"inherit" =
 * swapchain 格式，blit 到后缓冲要求同格式类，LINEAR filter 的 VUL 兼容前提）。
 * 存于 std::map，节点地址稳定（view 的 raii 对象与描述符指针链不 move）。
 */
export struct RtAttachment {
	RGL::VmaImage image;  // offscreen RT 图（extent/format 漂移时重建）
	std::optional<vk::raii::ImageView> view;  // RT view（aspect 随 ensure 参数：颜色 / 深度）
	vk::Extent2D extent{};  // 图 extent（重建命中判断条件之一）
	vk::Format format = vk::Format::eUndefined;  // v341 raii::ImageView 无 getFormat，自建记录
	// 重建计数：extent/format 漂移换新图（真实布局回 Undefined）——消费方
	//（Renderer 布局追踪）据此重置，旧值失配 = spec 违例
	uint64_t generation = 0;
	/**
	 * 本附件的全部描述符附件项（**唯一真相源**；规格顺序 = 本向量顺序）。
	 * 🔴 首建定容后永不 resize（指针链稳定前提，见 RtDescriptor 头注释）；重建只
	 * 原地换 viewCreate.image 并重写描述符，**槽号跨重建复用**（管线 baked 的槽号
	 * 因此永久有效——这也是"图迁入缓存"的结构性收益来源）。
	 */
	std::vector<RtDescriptor> descriptors;

	/**
	 * @return 本附件的 SAMPLED_IMAGE 描述符（heapSlot 查询面的语义本体）；无采样项 →
	 * nullptr。不是"取第 0 项"的碰运气查找：一个附件至多一个采样描述符是 ensure 的
	 * 规格校验守住的不变式（多要即报错），所以这个谓词至多一元。
	 */
	[[nodiscard]] const RtDescriptor* sampleDescriptor() const {
		auto it = std::ranges::find_if(descriptors,
			[](const RtDescriptor& d) { return d.type == vk::DescriptorType::eSampledImage; });
		return it == descriptors.end() ? nullptr : &*it;
	}
};

/**
 * offscreen RT 缓存（2026-09-19 架构债务续账步骤 2：原 Resources::ensureRtAttachment /
 * findRtAttachment / rtHeapSlot + rtAttachments map 整体搬出——通用"名字 → RT"缓存，
 * 内核只留"写描述符进 heap 槽"这类内容中性操作）。**不认识任何具体 pass**：deferred
 * G-buffer / scene_hdr 的装配函数在 render_graph（rtslot partition，契约表
 * kGBufferAttachments 本家）。生命周期：main 栈对象，构造于 Resources 之后、
 * Renderer/drawFrame 之前。
 */
export class RenderTargetCache {
   public:
	/**
	 * @param kernel 资源内核（VMA 分配器 / descriptor heap / allocSlot 领槽）
	 * @param device 设备层（waitIdle / 描述符写入）
	 */
	RenderTargetCache(RGL::Resources& kernel, RGL::DeviceContext& device)
		: kernel(kernel), device(device) {}

	/**
	 * 幂等创建（或 extent/format 失配时重建）一个 offscreen RT，返回 map 节点稳定指针
	 *（std::expected 不支持引用类型，AGENTS 规范）。行为契约见 RtAttachment 注释。
	 *
	 * **本重载 = 老行为逐字节不变**：mipLevels=1、usage 按 aspect 取颜色/深度档、
	 * 一个全图 SAMPLED_IMAGE 描述符（eShaderReadOnlyOptimal）、槽由缓存在 Runtime 段领。
	 *
	 * **深度 RT（批 11.2 基建）**：aspect = eDepth 时三处随深度面走——① image usage 用
	 * `DEPTH_STENCIL_ATTACHMENT | SAMPLED`（**不加** transfer 两端：深度格式对 transfer
	 * 的支持按格式而异，且本缓存无需求，多要一个 flag 就多一份驱动差异面）；② RT view 与
	 * 采样 view 的 subresourceRange.aspectMask = eDepth（漏这一步 = 颜色 view 指向深度图，
	 * invalid usage）；③ 采样描述符 layout 仍 eShaderReadOnlyOptimal（深度**独占**格式允许
	 * 该布局，与主深度图的 SSAO 采样描述符同约定，见 Resources::writeDepthSampleDescriptor）。
	 * @param name attachment 名（缓存 key；同名同 extent 同格式命中复用——
	 *   drawFrame 图循环按图声明每帧调用）
	 * @param extent RT extent（= swapchain extent，或图属独立尺寸如阴影图）
	 * @param format RT 格式（调用点解析，"inherit" = swapchain 格式；深度档走 kDepthRgFormats）
	 * @param aspect view aspect（eColor 默认 = 既有颜色 RT 行为逐位不变；eDepth = 深度 RT）
	 * @return 稳定指针（extent/format 漂移重建：waitIdle 后换 image/view、generation+1、
	 *   采样槽号不变；Error = VMA 图像创建 / allocSlot / 槽地址查询失败）
	 */
	[[nodiscard]] std::expected<RtAttachment*, Error> ensure(const std::string& name,
		vk::Extent2D extent, vk::Format format,
		vk::ImageAspectFlags aspect = vk::ImageAspectFlagBits::eColor);

	/**
	 * 规格入口（多 mip / 自定义 usage / 多描述符）。幂等语义与老入口完全相同：
	 * 命中 = 已存在且 extent/format 均匹配；重建 = waitIdle 后换 image/view、
	 * generation+1、**描述符槽号跨重建复用**。
	 *
	 * 规格是调用方的**终身契约**：同名二次调用若换了描述符项数 / 类型 / layout /
	 * 子资源范围，ensure 直接报错而不是静默改——烘进管线条目的槽号只对"规格不变、
	 * 只有图在换"这一种漂移有意义，别的漂移必须点名。
	 * @param name 缓存 key（也是领号时写进槽台账的 owner 名）
	 * @param extent 图 extent
	 * @param format 图格式
	 * @param spec 附件规格（aspect / mip 数 / usage / 描述符清单，见 RtAttachmentSpec）
	 * @return 稳定指针；Error = 规格漂移 / 采样描述符多于一个 / VMA 图像创建 /
	 *   allocSlot / 槽地址查询失败
	 */
	[[nodiscard]] std::expected<RtAttachment*, Error> ensure(const std::string& name,
		vk::Extent2D extent, vk::Format format, const RtAttachmentSpec& spec);

	/** @return RT 的 SAMPLED_IMAGE heap 槽号（ensure 首建分配、跨 resize
	 * 复用，管线 baked 永久有效）；尚未 ensure 的 RT 名 → nullopt（调用方须先 ensure
	 * 该 RT）；已 ensure 但规格里没有采样描述符 → 同样 nullopt（= 老
	 * hasSampleDescriptor==false 的形态） */
	[[nodiscard]] std::optional<uint32_t> heapSlot(const std::string& name) const {
		auto it = rtAttachments.find(name);
		if (it == rtAttachments.end())
			return std::nullopt;
		const auto* sampled = it->second.sampleDescriptor();
		return sampled == nullptr ? std::nullopt : std::optional<uint32_t>{sampled->heapSlot};
	}

	/** @return 已存在 RT 的稳定指针（帧内捕获直读 GB image/extent/format；未建 =
	 * nullptr——deferred 图每帧 ensure，正常路径必命中） */
	RtAttachment* find(const std::string& name) {
		auto it = rtAttachments.find(name);
		return it == rtAttachments.end() ? nullptr : &it->second;
	}

	/**
	 * @return 这个名字的 RT 当前**在不在活动表**（= 「这张附件的 RT 建没建」的一次查询）。
	 *
	 * 用途 = 能力观测（`docs/能力观测_plan.md` §3.3）：调用方**遍历编译图的附件、逐个问
	 * 本缓存**，能力名单的唯一来源仍是图。刻意只提供按名问，不提供 `names()` / `size()`
	 * 之类的枚举口——把缓存自己的表端出去，等于把「缓存里有什么」当成能力真相（§3.3 的
	 * 方向约束；缓存里可能有孤儿条目，图里可能有还没 ensure 的附件，两个方向都会错）。
	 *
	 * 口径 = 活动表（`retire` 后、到期销毁前进的是 `retired` 区，本查询答 false——
	 * 在役 RT 才是「建了」；退役窗口内下帧 ensure 会按新规格重建）。
	 * @param name attachment 名（编译图里的附件名，非空）
	 */
	[[nodiscard]] bool isBuilt(std::string_view name) const {
		return rtAttachments.contains(std::string { name });
	}

	/**
	 * **退役**一个 RT（B2-4③′ 债-124；extent 漂移的正规路径）：立刻从活动表摘除
	 *（下帧 ensure 按新尺寸重建），GPU 资源进退役区，**到期帧的安全点**才销毁 +
	 * 归还自有堆槽。in-flight 帧继续安全地引用退役区的旧图直到 due。
	 *
	 * 🔴 「有 in-flight 用户逐出必须炸」= 同名未到期再次 retire → Error
	 *（`PendingEvictions::schedule` 的不变式，单测正反两半都在）。
	 * @param name     活动表里的 RT 名
	 * @param dueFrame 销毁帧号（调用方给 = 发现帧 + frames-in-flight）
	 * @return 空值 = 已退役；Error = 名不在活动表 / 已在退役台账
	 */
	[[nodiscard]] std::expected<void, Error> retire(const std::string& name, uint64_t dueFrame);

	/**
	 * 收割到期退役项（安全点每帧调；销毁图 + 归还**自有**堆槽，预发号槽不归还）。
	 * @param frame 当前帧号
	 */
	void processRetired(uint64_t frame);

	/**
	 * **立即**逐出并销毁一个 RT（毁图/view + 归还自有堆槽；不经退役窗口）。
	 * 帧路径请用 `retire` + `processRetired`；本入口给「确定无 in-flight 用户」的
	 * 场景与单测（槽复用判据）用。
	 * @param name RT 名
	 * @return true = 找到并销毁；false = 名不存在
	 */
	bool erase(const std::string& name);

	/** @return 名字是否在退役台账里（调用方避免重复 retire） */
	bool retiredPending(const std::string& name) const { return evictions.pending(name); }

   private:
	/** 归还一个附件的全部**自有**堆槽（slotOwnedByCache 项；预发号项跳过——归属没变） */
	static void freeOwnedSlots(RGL::Resources& kernel, RtAttachment& att);

	RGL::Resources& kernel;  // 资源内核引用（allocator / descriptorHeap / allocSlot）
	RGL::DeviceContext& device;  // 设备层引用（waitIdle / writeResourceDescriptorsEXT）
	std::map<std::string, RtAttachment> rtAttachments;  // std::map 节点地址稳定（RtAttachment 指针链前提）
	// B2-4③′ 债-124：退役区（活动表摘除 ≠ 销毁；到期帧才毁图 + 还槽）+ 到期台账
	std::map<std::string, RtAttachment> retired;
	PendingEvictions evictions;
	// generation 连续性台账（B2-4③′）：retire/erase 时记下该名的最后 generation，
	// ensure 重建时以其为地板（+1）——否则「退役后重建」会复用旧 gen 值，与 Renderer 的
	// 跨帧快照相撞（快照判据 = generation 变化，相撞 = 新图布局不重置 → 静默错布局）。
	std::map<std::string, uint64_t> generationFloor;
};

}  // namespace RGL
