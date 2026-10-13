module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）
#include <vk_mem_alloc.h>   // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 的 C 面）
#include <fmt/format.h>
#include <chrono>
// vendor：include 面由 ffx_fsr3upscaler_vk 的 SYSTEM PUBLIC 传播；头文件**无**
// `#if defined(FFX_FSR3UPSCALER)` 守卫（该宏是那个 target 的 PRIVATE 编译宏，只影响
// .cpp 侧的排列表实例化），所以 rgl 侧不需要重复定义它。
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
module fsr_upscaler:impl;

import fsr_upscaler;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_device;
import vk_resources;
import VmaRaii;

namespace RGL {

/** RCAS 锐化强度（0 = 不额外锐化，1 = 最大）。**0.2 = AMD 样例的常用默认**，不是本机
 *  调出来的值——真正的三轴扫描在批 14.7（`phase14_plan.md` §2），这里只是「不能是 0」。 */
inline constexpr float kFsrSharpness = 0.2f;

namespace {

/** FfxSurfaceFormat → VkFormat。只覆盖 FSR3-upscaler 实际会要的那几个；命中不了直接
 *  抛——静默退回 eUndefined 就是冒烟第一版「image view 格式未定义 → 一串 VUID 连带炸」
 *  的根因（third_party/fsr/smoke/fsr_smoke.cpp 的 toVkFormat 同款纪律）。 */
vk::Format toVkFormat(FfxSurfaceFormat f) {
	switch (f) {
		case FFX_SURFACE_FORMAT_R32_FLOAT: return vk::Format::eR32Sfloat;
		case FFX_SURFACE_FORMAT_R16G16_FLOAT: return vk::Format::eR16G16Sfloat;
		case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return vk::Format::eR16G16B16A16Sfloat;
		case FFX_SURFACE_FORMAT_R8_UNORM: return vk::Format::eR8Unorm;
		case FFX_SURFACE_FORMAT_R8_UINT: return vk::Format::eR8Uint;
		case FFX_SURFACE_FORMAT_R32_UINT: return vk::Format::eR32Uint;
		case FFX_SURFACE_FORMAT_R16_UINT: return vk::Format::eR16Uint;
		case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return vk::Format::eB10G11R11UfloatPack32;
		default: throw std::runtime_error(
			fmt::format("FsrUpscaler: 未映射的 FfxSurfaceFormat={}（去 toVkFormat 补）",
				static_cast<int>(f)));
	}
}

/** 一次性把私有图从 UNDEFINED 推到 GENERAL（EvssmShadow 同款；此后帧内零 layout 转移）。 */
void toGeneral(RGL::DeviceContext& vkDevice, VkImage image) {
	auto& dev = vkDevice.getDevice();
	vk::CommandBufferAllocateInfo cai{*vkDevice.getGraphicsCommandPool(),
		vk::CommandBufferLevel::ePrimary, 1};
	auto buffers = dev.allocateCommandBuffers(cai);
	auto& cb = buffers[0];
	vk::CommandBufferBeginInfo bi{};
	bi.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit;
	cb.begin(bi);
	vk::ImageMemoryBarrier2 b{};
	b.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
	b.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
	b.dstAccessMask = vk::AccessFlagBits2::eShaderWrite;
	b.oldLayout    = vk::ImageLayout::eUndefined;
	b.newLayout    = vk::ImageLayout::eGeneral;
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.image        = image;
	b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	vk::DependencyInfo dep{};
	dep.setImageMemoryBarriers(b);
	cb.pipelineBarrier2(dep);
	cb.end();

	// raii 局部 cmd/fence 函数尾自动销毁（vulkan_utils::submitCmdBufferAndWait 同款机制，
	// 但本站失败 = throw，启动期 fail-fast 契约，不走 expected）
	vk::raii::Fence fence(dev, vk::FenceCreateInfo{});
	const VkCommandBuffer raw = *cb;
	VkSubmitInfo si{};
	si.sType          = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	si.commandBufferCount = 1;
	si.pCommandBuffers = &raw;
	if (::vkQueueSubmit(static_cast<VkQueue>(vkDevice.getGraphicsQueue()), 1, &si, *fence)
		!= VK_SUCCESS)
		throw std::runtime_error("FsrUpscaler: 一次性 layout 转换提交失败");
	if (dev.waitForFences(std::array<vk::Fence, 1>{*fence}, true, UINT64_MAX)
		!= vk::Result::eSuccess)
		throw std::runtime_error("FsrUpscaler: 一次性 layout 转换 fence 超时");
}

}  // namespace

struct FsrUpscaler::Impl {
	RGL::DeviceContext* deviceCtx = nullptr;
	RGL::Resources* resources = nullptr;
	vk::Extent2D renderExtent{};
	vk::Extent2D upscaleExtent{};

	/// FSR 后端的 scratch（`fpGetDeviceCapabilities` 等直接把它当 BackendContext_VK 用）
	/// ⟹ **必须比 context 活得久**，且不能移动其内容（生命周期红线同 pApplicationInfo）。
	std::vector<uint8_t> scratch;
	FfxInterface iface{};
	/// vendor 的 context 结构约 1 MiB（PATCHES.md P6）⟹ 堆上，别放栈/成员值。
	std::unique_ptr<FfxFsr3UpscalerContext> ctx;

	// ── 四张 FSR 私有图（GENERAL 常驻；usage 含 STORAGE，RT 缓存给不了）──
	VmaImage output, dilatedDepth, dilatedMv, reconDepth;
	/** 上面四张图**各自真实的**格式（= 建图时 vendor 给的那份描述里的格式）。
	 *  必须存下来：`Impl::wrap` 报给 vendor 的格式若与 image view 不符，VVL 判
	 *  「OpTypeImage 的 Format 与 VkImageView 不符 ⟹ 该变量所有读写值未定义」，
	 *  而 vendor 会按报过去的格式建 view（实测：reconstructedPrevNearestDepth 的真实
	 *  格式是 R32_UINT，按 R32_FLOAT 报 = 10 条建 view 报错 + 14 条 dispatch 报错）。 */
	/** 输出图的 view create info——**必须常驻成员**：descriptor heap 语义下
	 *  `VkImageDescriptorInfoEXT::pView` 存的是指针（EvssmShadow 的 `sampleViewCreate`
	 *  同一坑），栈上临时量不报错、用到才炸。 */
	/** 上一次 dispatch 的时刻（算 `frameTimeDelta`）。vendor 拿它做时域累积的时间常数
	 *  （`deltaTime = clamp(ms / 1000, 0, 1)`，`ffx_fsr3upscaler.cpp:1025`）——**喂 0 等于
	 *  把时间常数废掉**，实测后果是静态场景下输出比双线性放大还糊（边缘能量 7.95 → 3.60）。
	 *  ⚠ 这是墙钟量 ⟹ **门开时 FSR 的输出不是帧号纯函数**（红线只管门关，但 FSR 侧的
	 *  逐位回归判据因此不成立，14.6 立判据时要按这个口径来）。 */
	std::optional<std::chrono::steady_clock::time_point> lastDispatch;
	std::optional<vk::ImageViewCreateInfo> outputViewCreate;
	uint32_t outputSlot = 0;
	/** 时域累积开关（构造期定）。false = **每帧 reset** = 只用 EASU+RCAS 的纯空间上采样。
	 *  默认关的实测理由（本机、静态场景、renderScale=0.5，边缘能量 mean|∇|）：
	 *    双线性 7.95 < 本路径 8.43，而开时域累积掉到 **3.78**（比双线性还糊）；
	 *    开运动（frame_yaw）时域档 6.14 仍 < 双线性 8.06。逐帧单调衰减 8.43→5.37→4.71
	 *    →4.07→3.78 = 每次累积都在糊，且**一次**累积就掉 36% ⟹ 历史读取在零运动下
	 *    不是恒等重采样。已排除：速度场恒 0（实测）、字节布局（改 .zw 无影响）、
	 *    HDR 往返（关掉 3.78→4.11，非主因）、相机参数（实测填对）、共享图尺寸/格式
	 *    （全取 vendor 描述）、DEBUG_CHECKING（无输出）。全文 → plan §1.14。
	 *  ⚠ 关掉时域 = 输出是**帧号纯函数** ⟹ FSR 档可以做逐位回归（时域档做不到，
	 *    vendor 还要 frameTimeDelta 这个墙钟量）。 */
	bool temporal = false;
	FfxSurfaceFormat outputFormat = FFX_SURFACE_FORMAT_UNKNOWN,
		dilatedDepthFormat = FFX_SURFACE_FORMAT_UNKNOWN,
		dilatedMvFormat = FFX_SURFACE_FORMAT_UNKNOWN,
		reconDepthFormat = FFX_SURFACE_FORMAT_UNKNOWN;

	/** 按 vendor 给的描述建一张私有图并推到 GENERAL。
	 *  @param what 出错信息里的资源名
	 *  @note 尺寸/格式**照它给的填**（它内部按自己的假设读写，填错是"跑通了但结果错"）。 */
	VmaImage createShared(RGL::DeviceContext& vkDevice,
		const FfxCreateResourceDescription& want, const char* what) {
		if (want.resourceDescription.type != FFX_RESOURCE_TYPE_TEXTURE2D)
			throw std::runtime_error(fmt::format("FsrUpscaler: {} 不是 TEXTURE2D", what));
		vk::ImageCreateInfo ii{};
		ii.imageType   = vk::ImageType::e2D;
		ii.format      = toVkFormat(want.resourceDescription.format);
		ii.extent      = vk::Extent3D(want.resourceDescription.width,
		                              want.resourceDescription.height, 1);
		ii.mipLevels   = 1;
		ii.arrayLayers = 1;
		ii.samples     = vk::SampleCountFlagBits::e1;
		ii.tiling      = vk::ImageTiling::eOptimal;
		// STORAGE：FSR 以 writeonly image2D 写它（UAV）；SAMPLED：它自己也会回读。
		// TRANSFER_DST：vendor 的 clear job 走 `vkCmdClearColorImage`（ffx_vk.cpp:4239），
		//   并在它前面自己发 barrier 到 COPY_DEST（:4216）——少申请这个位 = 那两条双双违
		//   VUID（实测 19 条）。RT 缓存给不了这些 usage，正是 FSR 私有图自建的理由。
		ii.usage       = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled
		                 | vk::ImageUsageFlagBits::eTransferDst;
		ii.sharingMode = vk::SharingMode::eExclusive;
		VmaAllocationCreateInfo ai{};
		ai.usage = VMA_MEMORY_USAGE_AUTO;
		ai.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
		auto img = resources->getAllocator().createImage(ii, ai, nullptr, "fsr_upscaler/private_rt");
		if (!img)
			throw std::runtime_error(
				fmt::format("FsrUpscaler: {} 创建失败: {}", what, img.error().describe()));
		toGeneral(vkDevice, **img);
		return std::move(*img);
	}

	/** 把一张**已在本模块之外**建好的图包成 vendor 的资源句柄。
	 *
	 * `ffxGetResourceVK` 是**纯值包装**（不注册、不占池），真正的注册在每次 dispatch 里
	 * 由 `fpRegisterResource` 做，而它经 `copyResourceState`（ffx_vk.cpp:1103）**每帧**
	 * 用我们传的 state 重刷它的 `currentState` 记账 ⟹ 这里可以、也应当每帧现造，
	 * 申报**此刻的真实布局**。resize 换图因此天然跟得上，无需缓存/失效逻辑。
	 *
	 * @param state 该图在 dispatch 时刻的**真实**布局对应的 vendor state
	 */
	static FfxResource wrap(vk::Image image, uint32_t w, uint32_t h,
		FfxSurfaceFormat format, FfxResourceUsage usage, FfxResourceStates state) {
		FfxResourceDescription d{};
		d.type     = FFX_RESOURCE_TYPE_TEXTURE2D;
		d.width    = w;
		d.height   = h;
		d.mipCount = 1;
		d.usage    = usage;
		d.flags    = FFX_RESOURCE_FLAGS_NONE;
		d.format   = format;
		FfxResource r = ffxGetResourceVK(
			reinterpret_cast<void*>(static_cast<VkImage>(image)), d, L"rgl_fsr", state);
		r.description = d;
		return r;
	}
};

FsrUpscaler::FsrUpscaler(RGL::DeviceContext& vkDevice, RGL::Resources& resources,
	vk::Extent2D renderExtent, vk::Extent2D upscaleExtent, uint32_t outputSlot, bool temporal)
	: impl(std::make_unique<Impl>()) {
	impl->outputSlot = outputSlot;
	impl->temporal = temporal;
	impl->deviceCtx     = &vkDevice;
	impl->resources     = &resources;
	impl->renderExtent  = renderExtent;
	impl->upscaleExtent = upscaleExtent;

	// ── 一次性档（顺序即依赖，范例 = third_party/fsr/smoke/fsr_smoke.cpp:502-511）──
	const VkPhysicalDevice phys = *vkDevice.getPhysicalDevice();
	const size_t scratchSize = ffxGetScratchMemorySizeVK(phys, FFX_FSR3UPSCALER_CONTEXT_COUNT);
	impl->scratch.resize(scratchSize);

	// 生命周期红线同 pApplicationInfo：ffxGetDeviceVK 存的是**指针**（sVkDeviceContext =
	// *vkDeviceContext 是拷贝，但取地址的临时量本身必须在调用点存活）⟹ 用具名局部量。
	VkDeviceContext devCtx{static_cast<VkDevice>(*vkDevice.getDevice()), phys,
		vkGetDeviceProcAddr};
	const FfxDevice dev = ffxGetDeviceVK(&devCtx);
	if (const auto e = ffxGetInterfaceVK(&impl->iface, dev, impl->scratch.data(),
			impl->scratch.size(), FFX_FSR3UPSCALER_CONTEXT_COUNT);
		e != FFX_OK)
		throw std::runtime_error(fmt::format("FsrUpscaler: ffxGetInterfaceVK 失败 ({})",
			static_cast<int>(e)));

	FfxFsr3UpscalerContextDescription cd{};
	// HDR：我们的 scene_hdr 是 pre-exposure 线性 HDR（tonemap 才做 ACES）。
	// **不开** AUTO_EXPOSURE（我们没有曝光图，exposure 传 NULL 走 vendor 内置默认）。
	//
	// ⚠ **JITTER_CANCELLATION 批 14.5 第四批改成「不开」**（此前一直开着，是个潜伏错）：
	// 这个标志的语义是「**输入速度场带抖动**，请替我抵消」。vendor 做的事是
	// `mv_uv −= (previousJitter − curJitter)/motionVectorsTargetSize`
	// （`ffx_fsr3upscaler.cpp:1000-1003`，`docs/FSR输入契约.md` §3 已实机回读
	// `dilatedMotionVectors` 钉死公式/符号/分母）。而我们的 `velocity.frag` 输出的
	// 是**基空间**速度——它自己已经走过 `toBaseSpace` 那一步（文件头第 4 条 + `:85-87`
	// 写明了配对：基空间 ⟹ 本标志**不开**）。开着 = 对一个不含抖动项的量再减一次抖动
	// ⟹ 静止表面被判成在动。
	//
	// **为什么现在改是零风险**：抖动此前恒 0（TAA 默认关）⟹ 抵消项恒 0 ⟹ 开着与关着一模
	// 一样，所以这个配对错**从没暴露过**。批 14.5 第四批把抖动打开，它立刻就会变成真错。
	// 判据 = `just fsr-smoke` 的 §3 探针（它测的正是这一项）。
	cd.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
	cd.maxRenderSize  = {renderExtent.width, renderExtent.height};
	cd.maxUpscaleSize = {upscaleExtent.width, upscaleExtent.height};
	cd.backendInterface = impl->iface;
	impl->ctx = std::make_unique<FfxFsr3UpscalerContext>();
	if (const auto e = ffxFsr3UpscalerContextCreate(impl->ctx.get(), &cd); e != FFX_OK)
		throw std::runtime_error(fmt::format("FsrUpscaler: ContextCreate 失败 ({})",
			static_cast<int>(e)));

	// 三张共享图**按 vendor 自己给的描述**建（它内部按自己的假设读写）。
	FfxFsr3UpscalerSharedResourceDescriptions shared{};
	if (const auto e = ffxFsr3UpscalerGetSharedResourceDescriptions(impl->ctx.get(), &shared);
		e != FFX_OK)
		throw std::runtime_error(fmt::format("FsrUpscaler: GetSharedResourceDescriptions 失败 ({})",
			static_cast<int>(e)));
	impl->reconDepth        = impl->createShared(vkDevice, shared.reconstructedPrevNearestDepth,
		"reconstructedPrevNearestDepth");
	impl->reconDepthFormat  = shared.reconstructedPrevNearestDepth.resourceDescription.format;
	impl->dilatedDepth      = impl->createShared(vkDevice, shared.dilatedDepth, "dilatedDepth");
	impl->dilatedDepthFormat = shared.dilatedDepth.resourceDescription.format;
	impl->dilatedMv         = impl->createShared(vkDevice, shared.dilatedMotionVectors,
		"dilatedMotionVectors");
	impl->dilatedMvFormat   = shared.dilatedMotionVectors.resourceDescription.format;

	// 输出图：与输入同格式（HDR 线性），上屏分辨率。FSR 以 UAV 写 ⟹ 必须 STORAGE，
	// 这正是它**不能**走 RenderTargetCache 的原因（RT 缓存的 usage 无 STORAGE）。
	{
		FfxCreateResourceDescription outDesc{};
		auto& rd = outDesc.resourceDescription;
		rd.type   = FFX_RESOURCE_TYPE_TEXTURE2D;
		rd.format = FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT;
		rd.width  = upscaleExtent.width;
		rd.height = upscaleExtent.height;
		rd.depth  = 1;
		rd.mipCount = 1;
		rd.flags  = FFX_RESOURCE_FLAGS_NONE;
		rd.usage  = static_cast<FfxResourceUsage>(
			FFX_RESOURCE_USAGE_UAV | FFX_RESOURCE_USAGE_READ_ONLY);
		impl->output       = impl->createShared(vkDevice, outDesc, "fsr_output");
		impl->outputFormat = outDesc.resourceDescription.format;
	}

	// 输出图的采样描述符写进装配层预发的槽（EvssmShadow 的 momentsSampleSlot 同族），
	// tonemap 的色输入 binding 就指向它——**零拷贝**：FSR 直接写这张图，tonemap 直接读。
	// ⚠ layout 申报 **GENERAL**：本图常驻 GENERAL（FSR 以 UAV 写它），而 heap 语义下描述符
	//   里的 layout 必须与真实布局一致。采样一个 GENERAL 图合法（SAMPLED_IMAGE 描述符的
	//   允许布局含 GENERAL），前提是图有 SAMPLED usage——createShared 里申请了。
	impl->outputViewCreate = vk::ImageViewCreateInfo{};
	impl->outputViewCreate->image = *impl->output;
	impl->outputViewCreate->viewType = vk::ImageViewType::e2D;
	impl->outputViewCreate->format = toVkFormat(impl->outputFormat);
	impl->outputViewCreate->subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	{
		vk::ImageDescriptorInfoEXT imgDesc{};
		imgDesc.pView = &*impl->outputViewCreate;
		imgDesc.layout = vk::ImageLayout::eGeneral;
		vk::ResourceDescriptorInfoEXT resDesc{};
		resDesc.type = vk::DescriptorType::eSampledImage;
		resDesc.data.pImage = &imgDesc;
		const auto addr = resources.getDescriptorHeap().getSlotCpuAddress(impl->outputSlot);
		if (!addr)
			throw std::runtime_error(fmt::format("FsrUpscaler: 槽 {} 地址解析失败",
				impl->outputSlot));
		vkDevice.getDevice().writeResourceDescriptorsEXT(resDesc,
			vk::HostAddressRangeEXT{*addr, static_cast<VkDeviceSize>(
				resources.getDescriptorHeap().getDescriptorSize())});
	}

	if (auto* lg = RLLogger::getInstance())
		lg->info("FsrUpscaler: context 就绪（输出槽 {}）render={}x{} upscale={}x{} vram={} MiB",
			impl->outputSlot, renderExtent.width, renderExtent.height, upscaleExtent.width,
			upscaleExtent.height, vramBytes() / (1024 * 1024));
}

FsrUpscaler::~FsrUpscaler() {
	if (impl->ctx)
		ffxFsr3UpscalerContextDestroy(impl->ctx.get());
	// 私有图随 VmaImage 析构回 VMA；context 必须先销毁（它还持有这些图的 view）。
}

/** `fsrInputLayout()` 的实现体（声明与语义见接口分区）。**故意住在 impl 而不是接口**：本函数
 *  与下面 `recordFrame` 里的 `srvState` 是同一个事实的两半（「向 vendor 申报什么 state」与
 *  「那个 state 对应哪个 VkImageLayout」），拆到两个文件 = 调用方读的布局与真实申报各说各话。
 *  对端映射的厂商出处 = ffx_vk.cpp 的 `getVKImageLayoutFromResourceState`：
 *  `FFX_RESOURCE_STATE_COMPUTE_READ → VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL`。 */
vk::ImageLayout fsrInputLayout() {
	return vk::ImageLayout::eShaderReadOnlyOptimal;
}

void FsrUpscaler::recordFrame(vk::raii::CommandBuffer& cmd, const FsrFrameInput& input) {
	const uint32_t w = impl->renderExtent.width;
	const uint32_t h = impl->renderExtent.height;
	// 三个图属输入申报 COMPUTE_READ（= SRO）：布局护栏在调用方，见 FsrFrameInput 头注释。
	constexpr auto srv = static_cast<FfxResourceUsage>(FFX_RESOURCE_USAGE_READ_ONLY);
	// ⚠ `srvState` 与上面那个 `fsrInputLayout()` **成对修改**：调用方（velocity 的转换目标 +
	//   进 FSR 前的布局护栏）读的是那个函数，这里改了不改它 = 让护栏在错误的布局上放行。
	const auto srvState = FFX_RESOURCE_STATE_COMPUTE_READ;
	const auto uav = static_cast<FfxResourceUsage>(
		FFX_RESOURCE_USAGE_UAV | FFX_RESOURCE_USAGE_READ_ONLY);
	const auto uavState = FFX_RESOURCE_STATE_UNORDERED_ACCESS;

	FfxFsr3UpscalerDispatchDescription dd{};
	dd.commandList = ffxGetCommandListVK(static_cast<VkCommandBuffer>(*cmd));
	dd.color = Impl::wrap(input.color, w, h, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, srv, srvState);
	// 🔴 DEPTHTARGET 是申报"这是深度"的**唯一**途径：不申报 ⟹ vendor 按 COLOR 建 view
	// ⟹ 在 D32_SFLOAT 上建 COLOR aspect view = 7 条 VVL 报错（just fsr-smoke 探针 4 实测）。
	dd.depth = Impl::wrap(input.depth, w, h, FFX_SURFACE_FORMAT_R32_FLOAT,
		static_cast<FfxResourceUsage>(srv | FFX_RESOURCE_USAGE_DEPTHTARGET), srvState);
	dd.motionVectors = Impl::wrap(input.velocity, w, h, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT,
		srv, srvState);
	// exposure 传 NULL ⟹ vendor 用内置默认（= 不预曝光）。我们没有曝光图，且 scene_hdr 是
	// pre-exposure 线性值；luma 相关阈值的口径调整归批 14.6（调参）。
	dd.exposure = FfxResource{};
	// 三张共享图 + 输出图的格式一律取**建图时那份**（见上面四个 format 成员的注释）。
	dd.dilatedDepth = Impl::wrap(*impl->dilatedDepth, w, h, impl->dilatedDepthFormat,
		uav, uavState);
	dd.dilatedMotionVectors = Impl::wrap(*impl->dilatedMv, w, h, impl->dilatedMvFormat,
		uav, uavState);
	dd.reconstructedPrevNearestDepth = Impl::wrap(*impl->reconDepth, w, h,
		impl->reconDepthFormat, uav, uavState);
	dd.output = Impl::wrap(*impl->output, impl->upscaleExtent.width,
		impl->upscaleExtent.height, impl->outputFormat, uav, uavState);
	// ── dispatch 的**标量**输入（批 14.5 第二批补漏）──────────────────────────────
	// ⚠ 这三个字段原本全被零初始化成了 0/false，而 14.2 的《FSR输入契约》只写了四个
	//   **输入资源**的口径、一个标量都没写 ⟹ 照着那份文档接线就把它们全漏了。后果实测：
	//   静态场景 + 1:1 档边缘能量 16.98 → 7.67（比双线性还糊）。逐条：
	//   · enableSharpening：关 ⟹ **RCAS 那个 pass 根本不录**，EASU 的软化毫无补偿。
	//   · sharpness：0 = 不额外锐化。0.2 = AMD 样例的常用默认，真正的调参在批 14.7。
	//   · preExposure：vendor 注释写「must be > 0.0f」，:256 对 0 直接打 ERROR 并回落 1.0。
	const auto now = std::chrono::steady_clock::now();
	const float frameMs = impl->lastDispatch
		? std::chrono::duration<float, std::milli>(now - *impl->lastDispatch).count()
		: 16.667f;
	impl->lastDispatch = now;
	dd.enableSharpening = true;
	dd.sharpness        = kFsrSharpness;
	// vendor 要求 >= 1.0f（否则打 warning），而它内部再 clamp 到 [0,1] 秒 ⟹ 这里钳区间
	// 而不是把 warning 当噪声：headless 冒烟的单帧间隔可以是亚毫秒，也可以是一百毫秒。
	dd.frameTimeDelta   = std::clamp(frameMs, 1.0f, 1000.0f);
	dd.preExposure      = std::max(input.exposure, 1e-4f);
	dd.jitterOffset      = {input.contentShiftX, input.contentShiftY};
	// (1,1) = 「输入按渲染分辨率像素计」的**正确**填法，不是占位。vendor 在
	// `ffx_fsr3upscaler.cpp:996-997` 就把本字段除以 `motionVectorsTargetSize` 再交给 shader
	// （shader 侧 `LoadInputMotionVector` 只做乘法），所以像素口径的输入要填 1。
	// ⚠ 本行 2026-10-10 曾被"读了一半代码"改掉过一次：只看到 callbacks 里的乘法就断定
	//   vendor 不除，改成 1/尺寸 —— 那会让速度场**小 1000 倍**（相机一动就鬼影）。
	//   正确答案在 `docs/FSR输入契约.md` §1 里，且那条结论是**喂常量 MV 回读
	//   dilatedMotionVectors 对拍 + 变异验红**得来的。**改这行前先跑那个探针，别读代码。**
	dd.motionVectorScale = {1.0f, 1.0f};
	dd.renderSize        = {w, h};
	dd.upscaleSize       = {impl->upscaleExtent.width, impl->upscaleExtent.height};
	// 纯空间档：每帧 reset，vendor 因此不读历史（实测数据见 Impl::temporal）。
	dd.reset             = impl->temporal ? input.reset : true;
	dd.cameraNear        = input.nearPlane;
	dd.cameraFar         = input.farPlane;
	dd.cameraFovAngleVertical = input.fovVertical;
	dd.viewSpaceToMetersFactor = 0.0f;

	// 🔴 返回码**必须**查：renderSize 超 maxRenderSize 是硬失败（FFX_ERROR_OUT_OF_RANGE），
	// 只 warn 不查 = "返回错误但画面照旧" = 静默不升频（§1.12 第 4 条）。
	if (const auto e = ffxFsr3UpscalerContextDispatch(impl->ctx.get(), &dd); e != FFX_OK)
		throw std::runtime_error(fmt::format(
			"FsrUpscaler: dispatch 失败 ({}) —— render={}x{} upscale={}x{}。"
			"若为 OUT_OF_RANGE：内部共享图按 maxRenderSize 定尺寸，改分辨率必须重建本对象",
			static_cast<int>(e), w, h, impl->upscaleExtent.width, impl->upscaleExtent.height));
}

vk::Image FsrUpscaler::outputImage() const {
	return *impl->output;
}

vk::Extent2D FsrUpscaler::getRenderExtent() const {
	return impl->renderExtent;
}

uint64_t FsrUpscaler::vramBytes() const {
	FfxEffectMemoryUsage vram{};
	if (const auto e = ffxFsr3UpscalerContextGetGpuMemoryUsage(impl->ctx.get(), &vram);
		e != FFX_OK)
		return 0;
	// aliasableUsageInBytes 单列（14.8 的「外部分配器」科目要区分「可别名」部分）。
	return vram.totalUsageInBytes;
}

}  // namespace RGL
