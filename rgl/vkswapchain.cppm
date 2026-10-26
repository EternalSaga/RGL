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
import runtime_config;  // PresentModePref（窗口 present 模式偏好，批 9.3 双模式开关）
import features;        // presentTimingSupported 设备组门（批 9.3）
import diagnostics;     // diagnostics().profiler（批 9.3 激活门：默认 0 = 零行为变化）

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
 *   capability 钳制），imageCount 走 getImageCount（Wayland 不加 1），presentMode 按
 *   runtime.lua present_mode 偏好选择（默认 eImmediate 硬要求；"fifo" = eFifo，批 9.3
 *   双模式开关，见 utils.cppm selectPresentMode 与 docs/技术概念.md Present Mode 节）。
 * **成员生命周期规则**（AGENTS.md"重要成员生命周期规则"）：raii 对象（surface /
 * swapchain / swapchainImageViews 元素）无默认构造、不可拷贝，必须经成员初始化列表
 * 初始化，绝不在函数体内初始化；成员声明顺序 = 初始化依赖顺序（surface→format/
 * presentMode/presentTiming/surfaceCapabilities→extent→imageCount→swapchain→
 * swapchainImages→swapchainImageViews，前序成员在初始化列表中被后序成员消费）。
 */
export class Swapchain {
   public:
	/**
	 * 构建交换链层（全部逻辑在成员初始化列表，按成员声明序）：
	 * vkDevice → window → surface（headless ? createHeadlessSurface : createSurface）→
	 * format（headless ? selectHeadlessFormat : window->selectFormat）→ presentMode
	 * （headless ? selectHeadlessPresentMode : createPresentMode）→ surfaceCapabilities
	 * （查询）→ presentTiming（批 9.3 激活探测：设备组门 ∧ profiler>0 ∧ surface 能力，
	 * 门不过 = active false 零额外查询）→ extent（headless = {headlessWidth, headlessHeight} :
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
	 * @param presentModePref 窗口 present 模式偏好（runtime.lua present_mode；headless
	 *   忽略——恒走 eFifo 回落路径）
	 * @param renderScaleIn 内部渲染分辨率倍率（runtime.lua render_scale，默认 1.0 = 与窗口
	 *   逐像素对齐）。**参数名带 In 后缀是刻意的**：mem-initializer 里形参会遮蔽同名成员，
	 *   若写成 `renderScale` 则后续 `computeRenderExtent(extent, renderScale)` 读到的是
	 *   **未钳制的形参**而不是已钳制的成员——一个静默的钳制失效。
	 * @note 失败路径：surface 创建失败 / raii SwapchainKHR 构造失败抛异常（后者为
	 *   vk::SystemError）；构造体做 checkPresentAbility（graphics family 须可向
	 *   surface present），不可时抛 std::runtime_error（describe() 带 [Code]）
	 */
	Swapchain(RGL::DeviceContext& vkDevice, RGL::SDLWindow* window, bool headless,
		uint32_t headlessWidth = 0, uint32_t headlessHeight = 0,
		PresentModePref presentModePref = PresentModePref::immediate,
		float renderScaleIn = 1.0f)
		: vkDevice(vkDevice),
		  window(window),
		  headlessMode(headless),
		  // B2-4③′：headless 的「SDL 替身」= 这个请求尺寸（requestHeadlessExtent 改它，
		  // recreate() 读它）；初值 = 构造尺寸 → 没人请求就 recreate() 判 unchanged。
		  headlessRequestedExtent(headless ? vk::Extent2D{headlessWidth, headlessHeight}
		                                   : vk::Extent2D{}),
		  surface(headless ? createHeadlessSurface(vkDevice.getVkInstance().getInstance())
		                   : createSurface(vkDevice.getVkInstance().getInstance(), *window)),
		  format(headless ? selectHeadlessFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))
		                  : window->selectFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))),
		  presentModePref(presentModePref),
		  presentMode(headless ? selectHeadlessPresentMode(vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface))
		                      : createPresentMode(vkDevice, surface, presentModePref)),
		  presentTiming(probePresentTiming(vkDevice, surface, presentMode)),
		  surfaceCapabilities(vkDevice.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface)),
		  extent(headless ? vk::Extent2D{headlessWidth, headlessHeight}
		                 : window->getSwapchainExtend(surfaceCapabilities)),
		  renderScale(clampRenderScale(renderScaleIn)),
		  // 读上面两个成员：声明顺序保证它们已初始化（见成员块注释）
		  renderExtent(toExtent(computeRenderSize(extent.width, extent.height, renderScale))),
		  imageCount(headless ? surfaceCapabilities.minImageCount
		                     : window->getImageCount(surfaceCapabilities.minImageCount, surfaceCapabilities.maxImageCount)),
		  swapchain(vkDevice.getDevice(), buildSwapchainCreateInfo()),
		  swapchainImages(swapchain.getImages()),
		  swapchainImageViews(createImageViews(vkDevice.getDevice(), swapchainImages, format.format)) {
		auto result = vkDevice.getQueueManager().checkPresentAbility(surface);
		if (!result) {
			throw std::runtime_error(result.error().describe());
		}
		// 双模式验证证据（批 9.3：窗口 FIFO/eImmediate 对比跑要能看到实际选中项）
		if (auto* logger = RLLogger::getInstance())
			logger->info("Swapchain present mode: {}{}", vk::to_string(presentMode),
				headless ? " (headless)" : "");
		// 渲染网格保真度可观测（2026-10-03，机制与踩坑见 logRenderGridFidelity 注释）
		logRenderGridFidelity(headless, window, extent);
		// 批 9.3：激活时设结果队列容量 + 打激活日志（未激活 = no-op，零噪声）
		setupPresentTiming();
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
	/**
	 * @return 渲染 extent（= getExtent() × renderScale，批 14.3）；**深度图 / G-buffer /
	 * scene_hdr / ao / taa 历史 / IzbShadow / TAA jitter 一律用它**，而 imgui、点选、
	 * 鼠标、捕获到后缓冲的口径继续用 getExtent()。renderScale = 1.0 时两者逐像素相等。
	 * recreate() 与新 extent 同步刷新。
	 */
	vk::Extent2D getRenderExtent() const { return renderExtent; }
	/** @return 生效的渲染倍率（已钳制）；日志/RPC 回显用，判据靠它区分"哪档 scale"。 */
	float getRenderScale() const { return renderScale; }
	/** @return 交换链格式（format + colorSpace）；offscreen RT "inherit" 格式经它解析
	 * （Renderer），管线颜色格式同源。 */
	vk::SurfaceFormatKHR getFormat() const { return format; }
	/** @return 交换链 present 模式（headless 优先 eFifo；窗口路径按 presentModePref，默认 eImmediate）。 */
	vk::PresentModeKHR getPresentMode() const { return presentMode; }
	/** @return 批 9.3 present_timing 是否激活（探测三条件全过；false = 帧会话不挂 timing pNext 链）。 */
	bool isPresentTimingActive() const { return presentTiming.active; }
	/** @return 激活时的 stage 查询掩码（VK_PRESENT_STAGE_* 位集；present_timing attach 逐项填
	 * presentStageQueries，未激活 = 0 无意义）。 */
	vk::PresentStageFlagsEXT getPresentStageQueries() const { return presentTiming.stageQueries; }
	/** @return 结果队列容量（激活时 = imageCount×2，present_timing 用它限 in-flight 计数）。 */
	uint32_t getPresentTimingQueueCapacity() const { return imageCount * 2; }

	/**
	 * headless 模式请求新尺寸（B2-4③′ resize 冒烟；RPC /resize 的落点）：记下请求值，
	 * 由下一次 `recreate()` 真正重建（窗口路径不走这里——那条路 extent 由 SDL 重查）。
	 * @param width 新交换链宽
	 * @param height 新交换链高
	 * @return true = 请求已记（等 recreate()）；false = 非 headless（出声并忽略）
	 */
	bool requestHeadlessExtent(uint32_t width, uint32_t height) {
		if (!headlessMode) {
			if (auto* logger = RLLogger::getInstance())
				logger->warn("Swapchain: requestHeadlessExtent 只对 headless 模式有意义（窗口路径 "
				             "extent 由 SDL 重查），本次请求忽略");
			return false;
		}
		headlessRequestedExtent = vk::Extent2D{width, height};
		return true;
	}

	/**
	 * 重查 caps/format/presentMode/presentTiming 后按需重建。仅按 extent 判断（旧实现）会漏掉
	 * format/颜色空间/transform 漂移（如 HDR 状态切换）：acquire OUT_OF_DATE 后 extent 未变
	 * 时静默 no-op → 每帧再失败的死循环。changed = extent / format / colorSpace /
	 * presentMode / currentTransform / presentTiming.active 任一不同；重建 = device waitIdle → 销毁旧 swapchain
	 * （swapchain = nullptr：raii nullptr_t 临时 + 移动赋值，旧对象在语句末析构）→ 按
	 * 重查值（非构造期缓存）建新 swapchain + 刷新 images/views。
	 * @pre 非 headless 时 window 必非空（重查走 window 的 selectFormat / getSwapchainExtend /
	 *   getImageCount）；headless 时 extent 取 requestHeadlessExtent 的请求值、
	 *   format/presentMode 走构造期同款静态选择器（B2-4③′ resize 冒烟）
	 * @return true = 已重建（extent 可能已变，调用方须重建深度图等附属资源）；
	 *   false = 全部未变（重建无益），调用方负责观测（日志）且不得重建深度图等
	 *   附属资源
	 * @note 调用方：resizeSystem（窗口 resize，ecs:frame_input）/
	 *   Renderer::recreateSwapchainAndDepth（acquire OUT_OF_DATE / present
	 *   OUT_OF_DATE·SUBOPTIMAL）/ RPC /resize（headless，B2-4③′）
	 */
	[[nodiscard]] bool recreate() {
		const auto newCaps = vkDevice.getPhysicalDevice().getSurfaceCapabilitiesKHR(*surface);
		// B2-4③′（resize 冒烟）：headless 无 SDL 窗口——format/presentMode 走构造期同款
		// 静态选择器，extent 取 requestHeadlessExtent 的请求值（没请求过 = 现值 →
		// changed=false → no-op，与旧行为一致）。
		const auto newFormat = headlessMode
			? selectHeadlessFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface))
			: window->selectFormat(vkDevice.getPhysicalDevice().getSurfaceFormatsKHR(*surface));
		const auto newPresentMode = headlessMode
			? selectHeadlessPresentMode(
				vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface))
			: createPresentMode(vkDevice, surface, presentModePref);
		const auto newPresentTiming = probePresentTiming(vkDevice, surface, newPresentMode);
		const vk::Extent2D newExtent = headlessMode ? headlessRequestedExtent
		                                            : window->getSwapchainExtend(newCaps);

		const bool changed =
			newExtent.width != extent.width || newExtent.height != extent.height ||
			newFormat.format != format.format || newFormat.colorSpace != format.colorSpace ||
			newPresentMode != presentMode || newCaps.currentTransform != surfaceCapabilities.currentTransform ||
			newPresentTiming.active != presentTiming.active;  // 批 9.3：激活态翻转也须重建（flag 是创建期属性）
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
		presentTiming = newPresentTiming;  // 批 9.3：时间域 id 等运行态随新链作废（collect 侧按句柄变化自适应）
		extent = newExtent;
		// 渲染 extent 随新窗口尺寸重派生（renderScale 是创建期常量，不随 resize 变）。
		// 漏这一行 = resize 后深度图/RT 停在旧渲染尺寸而 swapchain 已是新尺寸，
		// 症状是"缩窗口后画面比例错/边缘漏画"，且只在 renderScale≠1 时出现。
		renderExtent = toExtent(computeRenderSize(extent.width, extent.height, renderScale));
		imageCount = headlessMode ? surfaceCapabilities.minImageCount
		                          : window->getImageCount(surfaceCapabilities.minImageCount,
		                                surfaceCapabilities.maxImageCount);
		swapchain = vk::raii::SwapchainKHR(vkDevice.getDevice(), buildSwapchainCreateInfo());
		setupPresentTiming();  // 批 9.3：新链重设结果队列容量（未激活 = no-op）
		swapchainImages = swapchain.getImages();
		swapchainImageViews = createImageViews(vkDevice.getDevice(), swapchainImages, format.format);
		return true;
	}

   private:
 	/** RenderSize（rlbase 纯算术）→ vk::Extent2D。转换只在这一处，见 runtime_config.cppm
 	 * 里 RenderSize 的「为什么不住 rlgfx」注释。 */
 	static vk::Extent2D toExtent(RenderSize s) { return {s.width, s.height}; }
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
	 * 窗口路径 present 模式：按 presentModePref 选择——
	 * fifo → find eFifo（Vulkan spec 必保模式，防御性判缺失）；
	 * immediate（默认）→ RGL::selectPresentMode（**硬要求 eImmediate**；Wayland 下
	 * compositor 已管理 triple-buffering 与帧同步，eImmediate 是正确选择，无需回退，
	 * 见 utils.cppm 与 docs/技术概念.md Present Mode 节）。
	 * @param vkDevice 设备层（经它取 PhysicalDevice 查模式列表）
	 * @param surface 查询目标 surface
	 * @param pref present 模式偏好（runtime.lua present_mode）
	 * @return 选中的 present 模式
	 * @note eFifo 缺失（理论不可能）/ eImmediate 不可用（compositor/平台不兼容且偏好
	 *   immediate）抛 std::runtime_error（describe() 带 [Code]）
	 */
	static vk::PresentModeKHR createPresentMode(RGL::DeviceContext& vkDevice,
		const vk::raii::SurfaceKHR& surface, PresentModePref pref) {
		auto modes = vkDevice.getPhysicalDevice().getSurfacePresentModesKHR(*surface);
		if (pref == PresentModePref::fifo) {
			auto it = std::ranges::find(modes, vk::PresentModeKHR::eFifo);
			if (it == modes.end())
				throw std::runtime_error(Error{ErrorCode::NoPresentMode,
					"eFifo not supported -- Vulkan spec mandates it"}.describe());
			return *it;
		}
		auto modeResult = RGL::selectPresentMode(modes);
		if (!modeResult) throw std::runtime_error(modeResult.error().describe());
		return *modeResult;
	}

	/**
	 * 批 9.3 present_timing 探测结果（成员 `presentTiming`；构造期与 recreate() 各重探一次）。
	 * 三态：不支持（设备组门/surface 能力任一缺）= active false，交换链行为与旧版完全一致；
	 * 支持 = active true，交换链带 ePresentTimingEXT flag 创建、结果队列容量 = imageCount×2
	 *（提案建议 ≥2×imageCount，覆盖在飞帧 + 一读回延迟）。时间域/刷新周期等运行态
	 * **不在本结构**——归 present_timing 模块自持（本类只管创建期机械）。
	 */
	struct PresentTimingSupport {
		bool active = false;  // true = 已激活（flag 已置 + 队列已设容量）
		vk::PresentStageFlagsEXT stageQueries{};  // surface 支持的 stage 查询掩码（attach 用）
	};

	/**
	 * 批 9.3 激活探测（三条件全过才 active）：
	 * ① 设备组门 RGL::presentTimingSupported（present_timing + present_id2 + KHR 校准）；
	 * ② profiler 帧窗口已开（diagnostics().profiler > 0）——默认关 = 探测都不做，
	 *    交换链创建参数逐位与旧版一致（BIT-IDENTICAL 红线，phase9_plan §8）；
	 * ③ surface 能力：VK_KHR_get_surface_capabilities2（vkinstance 基础扩展恒启用）经
	 *    getSurfaceCapabilities2KHR 的 timingCaps 查 presentTimingSupported 与
	 *    presentStageQueries（含 QUEUE_OPERATIONS_END）。**per-present-mode**——
	 *    用 SurfacePresentModeKHR 过滤当前选定模式查询（FIFO 与 eImmediate 支持位可能不同，
	 *    本机 RADV 实测见 nvidia_compatibility_report / git:调试历史）。
	 * headless surface 无 present_timing 支持 → 自然落选（不特判）。
	 * @param vkDevice 设备层（组门位 + PhysicalDevice 查询）
	 * @param surface 目标 surface
	 * @param mode 已选 present 模式（按模式过滤能力）
	 * @return 探测结果（active=false 时其余字段无意义）
	 */
	static PresentTimingSupport probePresentTiming(RGL::DeviceContext& vkDevice,
		const vk::raii::SurfaceKHR& surface, vk::PresentModeKHR mode) {
		if (!RGL::presentTimingSupported(vkDevice.getOptionalFeatures())
			|| RGL::diagnostics().profiler == 0) {
			return {};  // 门①②短路：不查 surface 能力（默认关 = 零额外查询）
		}
		vk::SurfacePresentModeKHR modeFilter{};
		modeFilter.presentMode = mode;
		vk::PhysicalDeviceSurfaceInfo2KHR surfaceInfo{};
		surfaceInfo.surface = *surface;
		surfaceInfo.pNext = &modeFilter;
		vk::PresentTimingSurfaceCapabilitiesEXT timingCaps{};
		vk::SurfaceCapabilities2KHR caps2{};
		caps2.pNext = &timingCaps;
		auto* logger = RLLogger::getInstance();
		auto capsResult = vkDevice.getPhysicalDevice().getSurfaceCapabilities2KHR(&surfaceInfo, &caps2);
		if (capsResult != vk::Result::eSuccess) {
			if (logger)
				logger->info("Present timing probe: caps2 query -> {} (mode {}), inactive",
					vk::to_string(capsResult), vk::to_string(mode));
			return {};  // 查询失败（理论罕见）= 降级不激活
		}
		// 探测结论入 log（phase9_plan §4.1"先 probe 再定深浅"的证据行；profiler>0 才走到这）
		if (logger)
			logger->info("Present timing probe: mode={} presentTimingSupported={} "
				"absoluteTime={} relativeTime={} stageQueries={:08x}",
				vk::to_string(mode), timingCaps.presentTimingSupported != 0,
				timingCaps.presentAtAbsoluteTimeSupported != 0, timingCaps.presentAtRelativeTimeSupported != 0,
				static_cast<uint32_t>(timingCaps.presentStageQueries));
		// 门槛 = presentTimingSupported + stage 掩码含 QUEUE_OPERATIONS_END（spec：supported 时
		// 至少 advertise 该位，缺失即驱动异常，降级）。注：VkSwapchainFlagsSurfaceCapabilitiesEXT
		// 的 flags-subset VUID-12448 只在启用 multisampled_render_to_swapchain 时生效（本项目
		// 恒不启用），故 ePresentTimingEXT 不经该结构把关，本机 RADV 实测该结构返 0
		//（2026-09-27，若按它筛会误判为不支持）。
		if (!timingCaps.presentTimingSupported)
			return {};
		auto stages = timingCaps.presentStageQueries;
		if (!(stages & vk::PresentStageFlagBitsEXT::eQueueOperationsEnd))
			return {};
		return PresentTimingSupport{true, stages};
	}

	/**
	 * 激活后置动作（构造体与 recreate() 共用）：设 present timing 结果队列容量
	 *（= imageCount × 2，提案建议值；容量满时 present 返回
	 * VK_ERROR_PRESENT_TIMING_QUEUE_FULL_EXT → 帧会话侧回退），并打激活日志
	 *（stageQueries 掩码 = 报告侧可查 stage 的证据）。
	 */
	void setupPresentTiming() {
		if (!presentTiming.active)
			return;
		auto* logger = RLLogger::getInstance();
		auto queueCapacity = imageCount * 2;  // 覆盖双在飞帧 + 一读回延迟
		if (logger)
			logger->info("Present timing active: queueCapacity={}, stageQueries={:#010x}",
				queueCapacity, static_cast<uint32_t>(presentTiming.stageQueries));
		auto result = swapchain.setPresentTimingQueueSizeEXT(queueCapacity);
		if (result != vk::Result::eSuccess && logger)
			logger->warn("vkSetSwapchainPresentTimingQueueSizeEXT({}) -> {}",
				queueCapacity, vk::to_string(result));
	}

	/**
	 * 渲染网格保真度可观测（2026-10-03）：交换链 extent 按**物理像素**创建
	 *（SDLWindow::getSwapchainExtend → pixelSize），这里把它与 SDL 逻辑窗尺寸、比值、
	 * windowScale 一并打出——「渲染缓冲 ≠ 屏幕像素网格」必须一眼可见。分数缩放桌面下
	 * 若像素尺寸 == 逻辑尺寸而 scale ≠ 1，合成器会替我们放大整张缓冲：画面非 1:1，
	 * 且按渲染像素定义的抖动/闪烁随该倍数放大（实测 KDE scale 1.75 时 TAA 游动
	 * 0.717 渲染像素 → 屏幕上 1.26 面板像素）。
	 * 两条**独立**判据取「或」触发 warning（任一 ≠ 1 即算缩放）：① SDL window scale（客户端
	 * 视角，SDL_GetWindowDisplayScale）；② 显示模式推算 scale = 当前显示模式物理尺寸 ÷ 显示
	 * 边界逻辑尺寸（显示层视角）。两来源都打进 info 日志——不一致本身就是信息（客户端与合成器
	 * 对缩放的认知不同）。
	 * ⚠ 本机实测（KDE Wayland scale 1.75，SDL 3.4.14）：默认配置下 SDL 对窗口 scale **失明**
	 * （windowScale=1.000 且 pixels==logical），单靠判据①会**假阴性**；但判据②仍报真实缩放——
	 * 关键坑：Wayland 下 SDL_GetCurrentDisplayMode 的 w/h 是**逻辑**尺寸（本机 1463×914，非物理
	 * 2560×1600），故「物理尺寸」= mode.w × pixel_density（本机 pixel_density=1.750），再除
	 * bounds.w（逻辑 1463）= 1.75，与 hint 报的 windowScale 吻合。设 SDL_VIDEO_WAYLAND_SCALE_TO_DISPLAY=1
	 * 后两来源对调（windowScale=1.75、推算=1.0），warning 由①触发（阳性锚点）。
	 * **根因归属（2026-10-04 协议报文重测修正，勿再怀疑桌面配置）**：KWin 已 advertise
	 * `wp_fractional_scale_manager_v1` 并下发 `wp_fractional_scale_v1.preferred_scale(210)`
	 *（=210/120=**1.75**），且 **SDL 自己就发** `wp_viewport.set_destination(逻辑宽,逻辑高)`。
	 * 缓冲给多大**只取决于窗口有没有 SDL_WINDOW_HIGH_PIXEL_DENSITY**：不开 → 缓冲 = 逻辑尺寸
	 * 被合成器放大 1.75（糊）；开 → 缓冲 = 逻辑×1.75，映射到同一 destination = **精确 1:1**。
	 * ⟹ 本函数当年判"SDL 收了分数缩放却从不按它给缓冲""SDL 从不写那个 viewport"**两处皆错**，
	 * 由它推出的"出路 = 引擎自己对 viewport 调 set_destination"**不需要做**。
	 * 那条 `nm --defined-only libSDL3.a` 找不到 `wp_viewporter_set_destination` 的论证是
	 * **方法学错误**：SDL 直接 `wl_proxy_marshal`，协议生成函数根本不参与链接，
	 * `--defined-only` 查不到是必然，证不了任何事。要证协议行为请用 `WAYLAND_DEBUG=1`。
	 * 修复与 UI 侧三条连带改动 → `git:调试历史` 2026-10-04「Wayland 175% 缩放」条。
	 * headless（无 SDL 窗口，extent = headless_size 配置）只打一行 extent，零 SDL 调用、零噪声。
	 * @param headless headless 模式（true 时不碰 SDL 窗口）
	 * @param window SDL 窗口（非 headless 必非空——类不变式，构造期已解引用）
	 * @param swapchainExtent 本交换链实际 extent（可能经 min/maxImageExtent 钳制）
	 */
	static void logRenderGridFidelity(bool headless, RGL::SDLWindow* window,
		vk::Extent2D swapchainExtent) {
		auto* logger = RLLogger::getInstance();
		if (!logger)
			return;
		if (headless) {
			logger->info("Swapchain extent: {}x{} (headless, headless_size config)",
				swapchainExtent.width, swapchainExtent.height);
			return;
		}
		int logicalW = 0, logicalH = 0;
		SDL_GetWindowSize(*window, &logicalW, &logicalH);
		// 不用结构化绑定取 array：跨模块 TU 里 std::tuple_size  specialization 不可见
		//（clang modules 已知坑，本文件实测报 "binds to 1 element"），下标访问等价
		const auto px = window->pixelSize();
		const int pixelW = px[0], pixelH = px[1];
		// 来源①：SDL 窗口 scale（客户端视角；KDE Wayland 默认配置实测失明返 1.000）
		const float windowScale = SDL_GetWindowDisplayScale(*window);
		// 来源②：显示模式推算 scale = 当前显示模式**物理**尺寸 ÷ 显示边界**逻辑**尺寸。
		// Wayland 下 SDL_GetCurrentDisplayMode 的 w/h 是逻辑尺寸（本机 1463，非物理 2560），
		// 物理 = mode.w × pixel_density（SDL 文档：scale converting size to pixels），本机
		// (1463×1.75)/1463 = 1.75——客户端对窗口 scale 失明时它是唯一能报出缩放的来源
		float displayScale = 1.0f;
		if (const SDL_DisplayID display = SDL_GetDisplayForWindow(*window); display != 0) {
			SDL_Rect bounds{};
			const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(display);
			if (mode && SDL_GetDisplayBounds(display, &bounds) && bounds.w > 0)
				displayScale = static_cast<float>(mode->w) * mode->pixel_density
					/ static_cast<float>(bounds.w);
		}
		const float ratioW = logicalW > 0 ? static_cast<float>(swapchainExtent.width) / static_cast<float>(logicalW) : 0.0f;
		const float ratioH = logicalH > 0 ? static_cast<float>(swapchainExtent.height) / static_cast<float>(logicalH) : 0.0f;
		logger->info("Swapchain extent: {}x{} (SDL logical {}x{}, buffer/logical ratio {:.2f}x{:.2f},"
			" windowScale={:.3f}, displayScale(mode/bounds)={:.3f})",
			swapchainExtent.width, swapchainExtent.height, logicalW, logicalH, ratioW, ratioH,
			windowScale, displayScale);
		// scale ≠ 1 判据留 1% 容差（浮点，1.00 附近不误报）；像素==逻辑 = 合成器缩放已坐实。
		// 两来源取「或」：客户端失明（windowScale=1）时靠 displayScale 兜住假阴性（本机默认配置）
		const bool windowScaleNeOne = windowScale < 0.99f || windowScale > 1.01f;
		const bool displayScaleNeOne = displayScale < 0.99f || displayScale > 1.01f;
		if (pixelW == logicalW && pixelH == logicalH && (windowScaleNeOne || displayScaleNeOne))
			logger->warn("Render buffer {}x{} equals SDL logical window size while scale is"
				" windowScale={:.3f} / displayScale(mode/bounds)={:.3f} -- the compositor will"
				" upscale it: image is NOT 1:1 and pixel-defined temporal instability is"
				" amplified by that factor. Cause is SDL, not the desktop: its Wayland backend"
				" receives the fractional scale (uses it for display geometry) but never sizes"
				" the buffer by it -- no wp_viewporter_set_destination / wl_surface_set_buffer_scale"
				" call exists in the library, so changing the desktop scale format does NOT help"
				" (KWin already advertises wp_fractional_scale_manager_v1, measured). Workaround"
				" today: set desktop scaling to 1.00. Real fix: size the swapchain by the scale and"
				" set the destination of SDL's own wp_viewport"
				" (SDL_PROP_WINDOW_WAYLAND_VIEWPORT_POINTER, non-null and never written by SDL).",
				pixelW, pixelH, windowScale, displayScale);
	}


	/**
	 * 按当前成员组装 SwapchainCreateInfo（构造器与 recreate() 共用，后者按重查值再调）：
	 * surface / minImageCount = imageCount / imageFormat + imageColorSpace = format /
	 * imageExtent = extent（headless = 唯一尺寸来源，v357 headless create info 无尺寸
	 * 字段，见 createHeadlessSurface）/ imageArrayLayers = 1 / imageUsage =
	 * COLOR_ATTACHMENT | TRANSFER_DST | TRANSFER_SRC（原因见体内注释）/
	 * imageSharingMode = EXCLUSIVE / preTransform = currentTransform / compositeAlpha =
	 * OPAQUE / presentMode / clipped = TRUE / oldSwapchain = nullptr（不走旧 swapchain
	 * 隐式重建路径）/ flags = 0 或批 9.3 激活时含 ePresentTimingEXT。
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
		// 批 9.3：激活才置 flag（未激活 = flags 保持默认 0，创建参数逐位同旧版）。
		// present timing 结果按 present id 关联 → 帧会话恒挂 VkPresentId2KHR，该结构要求
		// swapchain 带 ePresentId2 创建位（VUID-VkPresentInfoKHR-pPresentId…，VVL 实测报错），
		// 故两 flag 同进退；两 feature（presentTiming / presentId2）已在设备组门启用。
		if (presentTiming.active)
			swapchainCreateInfo.flags = vk::SwapchainCreateFlagBitsKHR::ePresentTimingEXT
				| vk::SwapchainCreateFlagBitsKHR::ePresentId2;
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
	// B2-4③′（resize 冒烟）：headless 标志 + 请求尺寸（headless 无 SDL，recreate() 的
	// extent/format 重查需要非 SDL 来源；requestHeadlessExtent 就是这个来源）
	bool headlessMode;
	vk::Extent2D headlessRequestedExtent{};
	vk::raii::SurfaceKHR surface;  // raii surface（GPU 成员中先于 format/presentMode/caps 初始化——后三者经它查询）
	vk::SurfaceFormatKHR format;  // 交换链格式（headless 优先 B8G8R8A8Srgb/eSrgbNonlinear；窗口路径 = SDLWindow::selectFormat）
	PresentModePref presentModePref;  // 窗口 present 模式偏好（runtime.lua present_mode；headless 忽略；recreate() 复选用）
	vk::PresentModeKHR presentMode;  // 交换链 present 模式（headless 优先 eFifo；窗口路径按 presentModePref，默认 eImmediate）
	PresentTimingSupport presentTiming;  // 批 9.3 present_timing 探测/激活态（门不过 = active false，行为同旧版；recreate() 重探）
	vk::SurfaceCapabilitiesKHR surfaceCapabilities;  // surface 能力快照（构造期查询；recreate() 重查刷新）
	vk::Extent2D extent;  // 交换链 extent（headless = headless_width/height；窗口路径 = getSwapchainExtend 钳制值）
	/**
	 * 内部渲染分辨率倍率（创建期常量，已钳到 [kRenderScaleMin, kRenderScaleMax]）。
	 * 声明在 extent 之后、renderExtent 之前：mem-initializer 按**声明顺序**求值，
	 * renderExtent 才能安全读到已初始化的这两个。顺序错 = 读未初始化内存（不报错）。
	 */
	float renderScale;
	/**
	 * 渲染 extent = extent × renderScale（各轴独立取整、下限 1）。**与 extent 同属本对象**
	 * 是刻意的：resize 只有一条路（recreate()），两个值同处一地就不可能失步——分成两个
	 * 所有者正是"深度图还是窗口尺寸、RT 已经是缩放尺寸"这类错的成因。
	 * 1.0 时逐像素等于 extent ⟹ 所有下游消费者行为与引入本概念前逐位相同（红线）。
	 */
	vk::Extent2D renderExtent;
	uint32_t imageCount;  // 图像数（headless = minImageCount；窗口路径 = getImageCount，Wayland 不加 1）
	vk::raii::SwapchainKHR swapchain;  // raii swapchain（无默认构造/不可拷贝，初始化列表构造；recreate() 经 = nullptr 销毁旧链）
	std::vector<vk::Image> swapchainImages;  // 交换链图像句柄快照（getImages；recreate() 同步刷新）
	std::vector<vk::raii::ImageView> swapchainImageViews;  // 每个后缓冲的 image view（createImageViews；recreate() 同步刷新）
};

}  // namespace RGL
