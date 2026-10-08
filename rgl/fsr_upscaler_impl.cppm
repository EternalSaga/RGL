module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）
#include <vk_mem_alloc.h>   // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 的 C 面）
#include <fmt/format.h>
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

	// raii 局部 cmd/fence 函数尾自动销毁（texture_upload::submitUploadAndWait 同款）
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
		auto img = resources->getAllocator().createImage(ii, ai);
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
	vk::Extent2D renderExtent, vk::Extent2D upscaleExtent)
	: impl(std::make_unique<Impl>()) {
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
	// JITTER_CANCELLATION：速度场带 TAA 抖动，抵消项必须开（口径 → docs/FSR输入契约.md §3）。
	// **不开** AUTO_EXPOSURE（我们没有曝光图，exposure 传 NULL 走 vendor 内置默认）。
	cd.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE
	         | FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
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

	if (auto* lg = RLLogger::getInstance())
		lg->info("FsrUpscaler: context 就绪 render={}x{} upscale={}x{} vram={} MiB",
			renderExtent.width, renderExtent.height, upscaleExtent.width,
			upscaleExtent.height, vramBytes() / (1024 * 1024));
}

FsrUpscaler::~FsrUpscaler() {
	if (impl->ctx)
		ffxFsr3UpscalerContextDestroy(impl->ctx.get());
	// 私有图随 VmaImage 析构回 VMA；context 必须先销毁（它还持有这些图的 view）。
}

void FsrUpscaler::recordFrame(vk::raii::CommandBuffer& cmd, const FsrFrameInput& input) {
	const uint32_t w = impl->renderExtent.width;
	const uint32_t h = impl->renderExtent.height;
	// 三个图属输入申报 COMPUTE_READ（= SRO）：布局护栏在调用方，见 FsrFrameInput 头注释。
	constexpr auto srv = static_cast<FfxResourceUsage>(FFX_RESOURCE_USAGE_READ_ONLY);
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
	dd.jitterOffset      = {input.jitterX, input.jitterY};
	// motionVectorScale = (1,1)：速度场按「渲染分辨率像素」计，vendor 侧再除以 renderSize
	// （它是**分子**不是分母 —— 契约探针 1 实测，docs/FSR输入契约.md §1）。
	dd.motionVectorScale = {1.0f, 1.0f};
	dd.renderSize        = {w, h};
	dd.upscaleSize       = {impl->upscaleExtent.width, impl->upscaleExtent.height};
	dd.reset             = input.reset;
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
