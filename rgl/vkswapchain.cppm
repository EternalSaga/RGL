module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <SDL3/SDL_video.h>
export module vk_swapchain;

import rlstd;
import rlvk;
import rllogger;
import vulkan_utils;
import vulkan_window;
import vk_instance;
import vk_device;
import vkerror;

namespace RGL {

/**
 * 交换链层（main.cpp 初始化层构造顺序第 3 站：AppContext→DeviceContext→
 * Swapchain→Resources→ShaderRegistry→Renderer，见 main.cpp）：surface + swapchain +
 * ImageView[]，及 format/extent/presentMode/imageCount 四项选择。两条路径：
 * ① headless（VK_EXT_headless_surface）：window 为空，surface 经 vkCreateHeadlessSurfaceEXT
 *   创建（脱离 compositor）；extent = 指定 width/height（v357 headless create info 无尺寸
 *   字段，见 createHeadlessSurface），format 优先 B8G8R8A8Srgb/eSrgbNonlinear（回落首个
 *   可用），presentMode 优先 eFifo（回落首个可用），imageCount = minImageCount（无窗口
 *   属性可参考）；
 * ② 窗口（非 headless）：window 必非空，surface 经 SDL_Vulkan_CreateSurface 创建；format 走
 *   SDLWindow::selectFormat（HDR/SDR 偏好序），extent 走 getSwapchainExtend（surface
 *   capability 钳制），imageCount 走 getImageCount（Wayland 不加 1），presentMode 硬要求
 *   eImmediate（见 utils.cppm selectPresentMode 与 docs/技术概念.md Present Mode 节）。
 * **成员生命周期规则**（AGENTS.md"重要成员生命周期规则"）：raii 对象（surface /
 * swapchain / swapchainImageViews 元素）无默认构造、不可拷贝，必须经成员初始化列表
 * 初始化，绝不在函数体内初始化；成员声明顺序 = 初始化依赖顺序（surface→format/
 * presentMode/surfaceCapabilities→extent→imageCount→swapchain→swapchainImages→
 * swapchainImageViews，前序成员在初始化列表中被后序成员消费）。
 */
export class Swapchain {
   public:
	/**
	 * 构建交换链层（全部逻辑在成员初始化列表，按成员声明序）：
	 * vkDevice → window → surface（headless ? createHeadlessSurface : createSurface）→
	 * format（headless ? selectHeadlessFormat : window->selectFormat）→ presentMode
	 * （headless ? selectHeadlessPresentMode : createPresentMode）→ surfaceCapabilities
	 * （查询）→ extent（headless = {headlessWidth, headlessHeight} :
	 * window->getSwapchainExtend）→ imageCount（headless = minImageCount :
	 * window->getImageCount）→ swapchain（buildSwapchainCreateInfo）→
	 * swapchainImages（getImages）→ swapchainImageViews（createImageViews）。
	 * headless：SDL 窗口可为空，surface 经 vkCreateHeadlessSurfaceEXT 创建（脱离
	 * compositor）；extent = 指定 width/height，format 优先 B8G8R8A8Srgb/eSrgbNonlinear
	 * （回落首个可用），imageCount = minImageCount（无窗口属性可参考）。
	 * 非 headless 时行为不变（window 必非空）。
	 * @param vkDevice 设备层（经它取 raii Instance / PhysicalDevice / Device / QueueFamilyManager）
	 * @param window SDL 窗口（非 headless 必非空；headless 传 nullptr）
	 * @param headless true = headless 模式（脱离 SDL/compositor）
	 * @param headlessWidth headless 交换链宽（runtime.lua headless_size，默认 1280；
	 *   headless+llm 时 main 对齐 llm_shot.size；非 headless 忽略）
	 * @param headlessHeight headless 交换链高（同上，默认 720）
	 * @note 失败路径：surface 创建失败 / raii SwapchainKHR 构造失败抛异常（后者为
	 *   vk::SystemError）；构造体做 checkPresentAbility（graphics family 须可向
	 *   surface present），不可时抛 std::runtime_error（describe() 带 [Code]）
	 */
	Swapchain(RGL::DeviceContext& vkDevice, RGL::SDLWindow* window, bool headless,
		uint32_t headlessWidth = 0, uint32_t headlessHeight = 0)
		: vkDevice(vkDevice),
		  window(window),
		  surface(headless ? createHeadlessSurface(vkDevice.getVkInstance().getInstance())
		                   : createSurface(vkDevice.getVkInstance().getInstance(), *window)),
		  format(headless ? selectHeadlessFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))
		                  : window->selectFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))),
		  presentMode(headless ? selectHeadlessPresentMode(vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface))
		                      : createPresentMode(vkDevice, surface)),
		  surfaceCapabilities(vkDevice.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface)),
		  extent(headless ? vk::Extent2D{headlessWidth, headlessHeight}
		                 : window->getSwapchainExtend(surfaceCapabilities)),
		  imageCount(headless ? surfaceCapabilities.minImageCount
		                     : window->getImageCount(surfaceCapabilities.minImageCount, surfaceCapabilities.maxImageCount)),
		  swapchain(vkDevice.getDevice(), buildSwapchainCreateInfo()),
		  swapchainImages(swapchain.getImages()),
		  swapchainImageViews(createImageViews(vkDevice.getDevice(), swapchainImages, format.format)) {
		auto result = vkDevice.getQueueManager().checkPresentAbility(surface);
		if (!result) {
			throw std::runtime_error(result.error().describe());
		}
	}

	/** @return raii surface const 引用（headless = vkCreateHeadlessSurfaceEXT，否则
	 * SDL_Vulkan_CreateSurface）；下游读 surface 句柄。 */
	const vk::raii::SurfaceKHR& getSurface() const { return surface; }
	/** @return raii swapchain const 引用；下游（Renderer acquire/present 等）入口。 */
	const vk::raii::SwapchainKHR& getSwapchain() const { return swapchain; }
	/** @return 交换链图像句柄向量 const 引用（getImages 快照）；Renderer drawFrame 按
	 * imageIndex 取图像（渲染附件 / 帧内捕获回读源）。 */
	const std::vector<vk::Image>& getImages() const { return swapchainImages; }
	/** @return 每个后缓冲的 raii ImageView 向量 const 引用（每图像 1 个，2D/color）；
	 * Renderer drawFrame 按 imageIndex 取 view 绑 swapchain 颜色附件。 */
	const std::vector<vk::raii::ImageView>& getImageViews() const { return swapchainImageViews; }
	/** @return 交换链 extent（按值）；Resources 用它创建深度图，recreate() 可能改变它。 */
	vk::Extent2D getExtent() const { return extent; }
	/** @return 交换链格式（format + colorSpace）；offscreen RT "inherit" 格式经它解析
	 * （Renderer），管线颜色格式同源。 */
	vk::SurfaceFormatKHR getFormat() const { return format; }
	/** @return 交换链 present 模式（headless 优先 eFifo；窗口路径恒 eImmediate）。 */
	vk::PresentModeKHR getPresentMode() const { return presentMode; }

	/**
	 * 重查 caps/format/presentMode 后按需重建。仅按 extent 判断（旧实现）会漏掉
	 * format/颜色空间/transform 漂移（如 HDR 状态切换）：acquire OUT_OF_DATE 后 extent 未变
	 * 时静默 no-op → 每帧再失败的死循环。changed = extent / format / colorSpace /
	 * presentMode / currentTransform 任一不同；重建 = device waitIdle → 销毁旧 swapchain
	 * （swapchain = nullptr：raii nullptr_t 临时 + 移动赋值，旧对象在语句末析构）→ 按
	 * 重查值（非构造期缓存）建新 swapchain + 刷新 images/views。
	 * @pre window 必非空（重查全走 window 的 selectFormat / getSwapchainExtend /
	 *   getImageCount）——仅非 headless 路径可用
	 * @return true = 已重建（extent 可能已变，调用方须重建深度图等附属资源）；
	 *   false = 全部未变（重建无益），调用方负责观测（日志）且不得重建深度图等
	 *   附属资源
	 * @note 调用方：resizeSystem（窗口 resize，ecs_impl）/
	 *   Renderer::recreateSwapchainAndDepth（acquire OUT_OF_DATE / present
	 *   OUT_OF_DATE·SUBOPTIMAL）
	 */
	[[nodiscard]] bool recreate() {
		auto newCaps = vkDevice.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface);
		auto newFormat = window->selectFormat(
			vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface));
		auto newPresentMode = createPresentMode(vkDevice, surface);
		const vk::Extent2D newExtent = window->getSwapchainExtend(newCaps);

		const bool changed =
			newExtent.width != extent.width || newExtent.height != extent.height ||
			newFormat.format != format.format || newFormat.colorSpace != format.colorSpace ||
			newPresentMode != presentMode || newCaps.currentTransform != surfaceCapabilities.currentTransform;
		if (!changed) {
			return false;
		}

		// 等待 GPU 空闲
		vkDevice.getDevice().waitIdle();

		// 销毁旧 swapchain
		swapchain = nullptr;

		// 重建（按重查值，非构造期缓存）
		surfaceCapabilities = newCaps;
		format = newFormat;
		presentMode = newPresentMode;
		extent = newExtent;
		imageCount = window->getImageCount(surfaceCapabilities.minImageCount,
			surfaceCapabilities.maxImageCount);
		swapchain = vk::raii::SwapchainKHR(vkDevice.getDevice(), buildSwapchainCreateInfo());
		swapchainImages = swapchain.getImages();
		swapchainImageViews = createImageViews(vkDevice.getDevice(), swapchainImages, format.format);
		return true;
	}

   private:
	/**
	 * 窗口路径 surface 创建（委托 SDLWindow::createSurface，SDL_Vulkan_CreateSurface；
	 * surface 生命周期绑定窗口，raii 包装随本对象析构释放）。
	 * @param instance surface 绑定的实例
	 * @param window SDL 窗口（非 headless 路径，必非空）
	 * @return 创建好的 raii surface
	 * @note 创建失败抛 std::runtime_error（describe() 带 [Code]）
	 */
	static vk::raii::SurfaceKHR createSurface(vk::raii::Instance& instance, RGL::SDLWindow& window) {
		auto result = window.createSurface(instance);
		if (!result) throw std::runtime_error(result.error().describe());
		return std::move(*result);
	}

	/**
	 * headless surface（VK_EXT_headless_surface）：脱离 compositor 创建虚拟 surface。
	 * v357 的 VkHeadlessSurfaceCreateInfoEXT 仅 sType/pNext/flags（无尺寸/格式——尺寸由
	 * swapchain 的 imageExtent 决定，见 buildSwapchainCreateInfo 的 extent）。present 为
	 * no-op（无真实显示），swapchain image 可回读（离屏渲染/CI 截图）。
	 * @param instance surface 绑定的实例（其扩展须已启用 VK_KHR_surface +
	 *   VK_EXT_headless_surface，见 vkinstance.cppm）
	 * @return 创建好的 raii surface
	 * @note vkCreateHeadlessSurfaceEXT 失败抛 std::runtime_error；成功打 info 日志
	 */
	static vk::raii::SurfaceKHR createHeadlessSurface(vk::raii::Instance& instance) {
		VkHeadlessSurfaceCreateInfoEXT createInfo{};
		createInfo.sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT;
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		if (vkCreateHeadlessSurfaceEXT(*instance, &createInfo, nullptr, &surface) != VK_SUCCESS) {
			throw std::runtime_error("Failed to create headless surface (vkCreateHeadlessSurfaceEXT)");
		}
		if (auto* logger = RLLogger::getInstance())
			logger->info("Headless surface created (VK_EXT_headless_surface)");
		return vk::raii::SurfaceKHR(instance, surface);
	}

	/**
	 * headless 格式选择：优先 B8G8R8A8Srgb + eSrgbNonlinear（SDR 8-bit，离屏渲染/截图
	 * 足够），回落首个可用。
	 * @param availableFormats surface 可用格式（调用方保证非空）
	 * @return 命中的格式（availableFormats 中一项；未命中 = 首个）
	 */
	static vk::SurfaceFormatKHR selectHeadlessFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
		for (const auto& available : availableFormats) {
			if (available.format == vk::Format::eB8G8R8A8Srgb && available.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
				return available;
			}
		}
		return availableFormats[0];
	}

	/**
	 * headless present mode：present 为 no-op（无真实显示），任何模式都行。优先 eFifo
	 *（最通用），回落首个可用——不走 selectPresentMode 的 eImmediate 硬要求（headless
	 * surface 无此模式，见 docs/vulkan_extensions_2025_调研.md §12.4 headless 关键坑）。
	 * @param presentModes surface 支持的 present 模式列表（调用方保证非空）
	 * @return 命中的 present 模式（presentModes 中一项；无 eFifo = 首个）
	 */
	static vk::PresentModeKHR selectHeadlessPresentMode(const std::vector<vk::PresentModeKHR>& presentModes) {
		auto it = std::ranges::find(presentModes, vk::PresentModeKHR::eFifo);
		if (it != presentModes.end()) {
			return *it;
		}
		return presentModes[0];
	}

	/**
	 * 窗口路径 present 模式：查询 surface 支持的模式列表 → RGL::selectPresentMode
	 * （**硬要求 eImmediate**；Wayland 下 compositor 已管理 triple-buffering 与帧同步，
	 * eImmediate 是正确选择，无需回退，见 utils.cppm 与 docs/技术概念.md Present Mode 节）。
	 * @param vkDevice 设备层（经它取 PhysicalDevice 查模式列表）
	 * @param surface 查询目标 surface
	 * @return 选中的 present 模式（恒 eImmediate）
	 * @note eImmediate 不可用（compositor/平台不兼容）抛 std::runtime_error（describe()
	 *   带 [Code]）
	 */
	static vk::PresentModeKHR createPresentMode(RGL::DeviceContext& vkDevice, const vk::raii::SurfaceKHR& surface) {
		auto modes = vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface);
		auto modeResult = RGL::selectPresentMode(modes);
		if (!modeResult) throw std::runtime_error(modeResult.error().describe());
		return *modeResult;
	}

	/**
	 * 按当前成员组装 SwapchainCreateInfo（构造器与 recreate() 共用，后者按重查值再调）：
	 * surface / minImageCount = imageCount / imageFormat + imageColorSpace = format /
	 * imageExtent = extent（headless = 唯一尺寸来源，v357 headless create info 无尺寸
	 * 字段，见 createHeadlessSurface）/ imageArrayLayers = 1 / imageUsage =
	 * COLOR_ATTACHMENT | TRANSFER_DST | TRANSFER_SRC（原因见体内注释）/
	 * imageSharingMode = EXCLUSIVE / preTransform = currentTransform / compositeAlpha =
	 * OPAQUE / presentMode / clipped = TRUE / oldSwapchain = nullptr（不走旧 swapchain
	 * 隐式重建路径）。
	 * @return 填好的 SwapchainCreateInfo（局部临时，仅供 raii 构造消费）
	 */
	vk::SwapchainCreateInfoKHR buildSwapchainCreateInfo() const {
		vk::SwapchainCreateInfoKHR swapchainCreateInfo{};
		swapchainCreateInfo.surface = *surface;
		swapchainCreateInfo.minImageCount = imageCount;
		swapchainCreateInfo.imageFormat = format.format;
		swapchainCreateInfo.imageColorSpace = format.colorSpace;
		swapchainCreateInfo.imageExtent = extent;
		swapchainCreateInfo.imageArrayLayers = 1;
		// Phase 6 批次 4：offscreen 图变体的 composite blit pass 以后缓冲为
		// transferWrite 目标（VVL：blit dst 需 TRANSFER_DST usage；旧 DIAG 手工通路
		// 时代即缺此位，属漏网）。帧内捕获（llm_debug 回读，2026-09-08）：present 前
		// CopyImageToBuffer 源 → 还需 TRANSFER_SRC
		swapchainCreateInfo.imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
			vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
		swapchainCreateInfo.imageSharingMode = vk::SharingMode::eExclusive;
		swapchainCreateInfo.preTransform = surfaceCapabilities.currentTransform;
		swapchainCreateInfo.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
		swapchainCreateInfo.presentMode = presentMode;
		swapchainCreateInfo.clipped = VK_TRUE;
		swapchainCreateInfo.oldSwapchain = nullptr;
		return swapchainCreateInfo;
	}

	/**
	 * 为每个 swapchain 图像创建 2D image view（format = 交换链格式、全通道 identity
	 * swizzle、eColor aspect、levelCount = layerCount = 1）——后缓冲颜色附件的入口。
	 * @param device 创建来源设备
	 * @param images 交换链图像句柄列表
	 * @param format 交换链格式（view format 须与图像一致）
	 * @return raii ImageView 向量（每图像 1 个，与 images 同序）
	 */
	static std::vector<vk::raii::ImageView> createImageViews(vk::raii::Device& device,
		const std::vector<vk::Image>& images,
		vk::Format format) {
		std::vector<vk::raii::ImageView> views;
		views.reserve(images.size());
		for (const auto& image : images) {
			vk::ImageViewCreateInfo viewCreateInfo{};
			viewCreateInfo.image = image;
			viewCreateInfo.viewType = vk::ImageViewType::e2D;
			viewCreateInfo.format = format;
			viewCreateInfo.components.r = vk::ComponentSwizzle::eIdentity;
			viewCreateInfo.components.g = vk::ComponentSwizzle::eIdentity;
			viewCreateInfo.components.b = vk::ComponentSwizzle::eIdentity;
			viewCreateInfo.components.a = vk::ComponentSwizzle::eIdentity;
			viewCreateInfo.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
			viewCreateInfo.subresourceRange.levelCount = 1;
			viewCreateInfo.subresourceRange.baseArrayLayer = 0;
			viewCreateInfo.subresourceRange.layerCount = 1;
			views.emplace_back(device, viewCreateInfo);
		}
		return views;
	}

	RGL::DeviceContext& vkDevice;  // 设备层引用（最先初始化）；surface/swapchain/images/views 均经它创建
	RGL::SDLWindow* window;  // headless 时为空（surface 经 headless 扩展创建）；窗口路径必非空，recreate() 重查依赖它
	vk::raii::SurfaceKHR surface;  // raii surface（GPU 成员中先于 format/presentMode/caps 初始化——后三者经它查询）
	vk::SurfaceFormatKHR format;  // 交换链格式（headless 优先 B8G8R8A8Srgb/eSrgbNonlinear；窗口路径 = SDLWindow::selectFormat）
	vk::PresentModeKHR presentMode;  // 交换链 present 模式（headless 优先 eFifo；窗口路径恒 eImmediate）
	vk::SurfaceCapabilitiesKHR surfaceCapabilities;  // surface 能力快照（构造期查询；recreate() 重查刷新）
	vk::Extent2D extent;  // 交换链 extent（headless = headless_width/height；窗口路径 = getSwapchainExtend 钳制值）
	uint32_t imageCount;  // 图像数（headless = minImageCount；窗口路径 = getImageCount，Wayland 不加 1）
	vk::raii::SwapchainKHR swapchain;  // raii swapchain（无默认构造/不可拷贝，初始化列表构造；recreate() 经 = nullptr 销毁旧链）
	std::vector<vk::Image> swapchainImages;  // 交换链图像句柄快照（getImages；recreate() 同步刷新）
	std::vector<vk::raii::ImageView> swapchainImageViews;  // 每个后缓冲的 image view（createImageViews；recreate() 同步刷新）
};

}  // namespace RGL
