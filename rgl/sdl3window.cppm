module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <fmt/format.h>
export module vulkan_window;

import rlstd;
import rlvk;
import rllogger;
import diagnostics;  // no_hdr 诊断单例（configs/diagnostics.lua）
import vkerror;

namespace RGL {

/**
 * SDL3 窗口 RAII（SDL_Window 创建/销毁 + Vulkan surface/格式/extent/图像数选择）。
 * 仅非 headless 路径使用：headless（runtime.lua headless=true）不建 SDL 窗口，
 * surface 经 vkCreateHeadlessSurfaceEXT（vkswapchain.cppm）；llm_debug / show_after
 * 隐藏窗（hidden=true）仍走本类——窗口存在但不上屏。
 */
export class SDLWindow {
   public:
	/**
	 * 接管已创建的 SDL 窗口（所有权移交；析构/移动时释放）。
	 * @param window 有效 SDL_Window*（create 创建）
	 * @param width 窗口创建时宽度（运行期 resize 不更新）
	 * @param height 窗口创建时高度（运行期 resize 不更新）
	 */
	SDLWindow(SDL_Window* window, int width, int height)
		: window(window), width(width), height(height), logger(RLLogger::getInstance()) {}

	/** 销毁窗口并退出 SDL video/events 子系统（window 为空时 no-op，移动后为空）。 */
	~SDLWindow() {
		if (window) {
			SDL_DestroyWindow(window);
			SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
		}
	}

	/** 不可拷贝（SDL_Window 为独占句柄，拷贝 = 双重销毁）。 */
	SDLWindow(const SDLWindow&) = delete;
	/** 不可拷贝（同上，赋值 = 双重销毁）。 */
	SDLWindow& operator=(const SDLWindow&) = delete;

	/** 移动构造：接管窗口句柄，源 window 置空（源析构变 no-op）。 */
	SDLWindow(SDLWindow&& o) noexcept
		: window(o.window), width(o.width), height(o.height), logger(o.logger) {
		o.window = nullptr;
	}

	/** 移动赋值：若本侧原持有窗口先释放（销毁 + 退子系统），再接管源句柄并置空源。 */
	SDLWindow& operator=(SDLWindow&& o) noexcept {
		if (this != &o) {
			if (window) {
				SDL_DestroyWindow(window);
				SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
			}
			window = o.window;
			width = o.width;
			height = o.height;
			logger = o.logger;
			o.window = nullptr;
		}
		return *this;
	}

	/**
	 * 创建窗口（SDL_Init(VIDEO|EVENTS) + SDL_CreateWindow；窗口标志 = VULKAN|RESIZABLE）。
	 * @param width 窗口创建宽度
	 * @param height 窗口创建高度
	 * @param title 窗口标题
	 * @param hidden true = 窗口存在但永不映射上屏（llm_debug / show_after DIAG：
	 *   surface/instance 扩展/队列选择全链路照常，只绕过"显示"；compositor 须存活
	 *   ——息屏可以，suspend 不行）
	 * @return SDLWindow 实例；SDL 初始化/建窗失败返回 Error(SDLError)（失败时已退子系统）
	 */
	[[nodiscard]] static std::expected<SDLWindow, Error>
	create(int width, int height, std::string title, bool hidden = false) {
		auto logger = RLLogger::getInstance();

		if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
			std::string errMsg = std::string("SDL init failed: ") + SDL_GetError();
			if (logger)
				logger->critical(errMsg);
			return std::unexpected(Error{ErrorCode::SDLError, errMsg});
		}

		// hidden（llm_debug 模式）：窗口存在但永不映射上屏——surface/instance 扩展/
		// 队列选择全链路照常，只绕过"显示"；compositor 须存活（息屏可以，suspend 不行）
		SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
		if (hidden)
			flags |= SDL_WINDOW_HIDDEN;
		SDL_Window* wnd = SDL_CreateWindow(title.c_str(), width, height, flags);

		if (!wnd) {
			std::string errMsg = std::string("Failed to create SDL Window: ") + SDL_GetError();
			if (logger)
				logger->critical(errMsg);
			SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
			return std::unexpected(Error{ErrorCode::SDLError, errMsg});
		}

		if (logger)
			logger->info("SDL window created ({}x{}){}", width, height,
				hidden ? " [hidden]" : "");

		return SDLWindow{wnd, width, height};
	}

	/**
	 * 创建 Vulkan 交换链 surface（SDL_Vulkan_CreateSurface；surface 生命周期绑定窗口，
	 * raii 包装随本对象或传入 instance 析构释放）。
	 * @param instance 绑定 surface 的 Vulkan instance
	 * @return raii surface；创建失败返回 Error(VulkanError)
	 */
	[[nodiscard]] std::expected<vk::raii::SurfaceKHR, Error> createSurface(vk::raii::Instance& instance) {
		VkSurfaceKHR surface = VK_NULL_HANDLE;

		if (!SDL_Vulkan_CreateSurface(window, *instance, nullptr, &surface)) {
			const std::string errMsg = fmt::format("Failed to create Vulkan Surface: {}", SDL_GetError());
			if (logger)
				logger->critical(errMsg);
			return std::unexpected(Error{ErrorCode::VulkanError, errMsg});
		}

		if (logger)
			logger->info("Vulkan surface created");

		return vk::raii::SurfaceKHR(instance, surface);
	}

	/**
	 * 获取创建 Vulkan surface 所需的 instance 扩展列表
	 *（SDL_Vulkan_GetInstanceExtensions，含 VK_KHR_surface 族）。
	 * @pre SDL video 子系统已初始化
	 * @return (扩展名数组, 个数)；数组归 SDL 所有，video 子系统存活期间有效；
	 * 失败返回 Error(SDLError)
	 */
	[[nodiscard]] std::expected<std::pair<const char* const*, uint32_t>, Error> getSurfaceExtensions() {
		if (!SDL_WasInit(SDL_INIT_VIDEO)) {
			if (logger)
				logger->critical("SDL Video subsystem not initialized!");
			return std::unexpected(Error{ErrorCode::SDLError, "SDL Video subsystem not initialized!"});
		}
		uint32_t extensionCount = 0;
		const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
		if (!sdlExtensions) {
			if (logger)
				logger->critical("Failed to get SDL surface extensions");
			return std::unexpected(Error{ErrorCode::SDLError, "Failed to get SDL surface extensions"});
		}
		return std::make_pair(sdlExtensions, extensionCount);
	}

	/** 隐式转换为原始 SDL 窗口句柄（供 SDL/事件系统直接取用）。 */
	inline operator SDL_Window*() { return window; }
	/** @return 窗口创建时宽度（运行期 resize 不更新；当前尺寸经 getSwapchainExtend 取）。 */
	inline int getWidth() const { return width; }
	/** @return 窗口创建时高度（运行期 resize 不更新；当前尺寸经 getSwapchainExtend 取）。 */
	inline int getHeight() const { return height; }

	/**
	 * 从 surface 可用格式中选择交换链格式（HDR 判定 = SDL 窗口属性
	 * SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN，即 compositor 能力）。
	 * 选择序：HDR 启用时先试 R16G16B16A16Sfloat/ExtendedSrgbLinear，再
	 * A2B10G10R10UnormPack32/Hdr10St2084，均不匹配回落 SDR；SDR 先
	 * B8G8R8A8Srgb、再 R8G8B8A8Srgb；仍无匹配取首个可用（warn）。
	 * DIAG：diagnostics().noHdr 强制 SDR（隔离 HDR R16G16B16A16Sfloat 后缓冲，
	 * 原 RGL_NO_HDR env 2026-09-08 转配置，见函数内）。
	 * @param availableFormats surface 可用格式（调用方保证非空）
	 * @return 命中的可用格式（availableFormats 中一项；未命中 = 首个）
	 */
	vk::SurfaceFormatKHR selectFormat(const std::vector<vk::SurfaceFormatKHR>& availableFormats) {
		SDL_PropertiesID windowProps = SDL_GetWindowProperties(window);
		bool isHdrEnabled = SDL_GetBooleanProperty(windowProps, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
		// DIAG（configs/diagnostics.lua no_hdr，原 RGL_NO_HDR env 2026-09-08 转配置）：强制 SDR 格式
		//（隔离 HDR R16G16B16A16Sfloat 后缓冲与纹理 high VA 落位/映射的关联，同 no_vvl 门模式）
		if (isHdrEnabled && diagnostics().noHdr) {
			isHdrEnabled = false;
			logger->info("[SDL3] diagnostics no_hdr=true: forcing SDR format");
		}
		std::vector<vk::SurfaceFormatKHR> preferredHdrFormats = {
			{vk::Format::eR16G16B16A16Sfloat, vk::ColorSpaceKHR::eExtendedSrgbLinearEXT},
			{vk::Format::eA2B10G10R10UnormPack32, vk::ColorSpaceKHR::eHdr10St2084EXT}};

		std::vector<vk::SurfaceFormatKHR> preferredSdrFormats = {
			{vk::Format::eB8G8R8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear},
			{vk::Format::eR8G8B8A8Srgb, vk::ColorSpaceKHR::eSrgbNonlinear}};

		if (isHdrEnabled) {
			logger->info("[SDL3] HDR enabled, matching HDR surface format...");
			for (const auto& preferred : preferredHdrFormats) {
				for (const auto& available : availableFormats) {
					if (available.format == preferred.format && available.colorSpace == preferred.colorSpace) {
						logger->info("[Vulkan] HDR format matched: {}", vk::to_string(available.format));
						return available;
					}
				}
			}
			logger->info("[Vulkan] No HDR format found, falling back to SDR.");
		}

		for (const auto& preferred : preferredSdrFormats) {
			for (const auto& available : availableFormats) {
				if (available.format == preferred.format && available.colorSpace == preferred.colorSpace) {
					return available;
				}
			}
		}

		logger->warn("[Vulkan] Warning: no ideal sRGB format found, using default {}",
			vk::to_string(availableFormats[0].format));
		return availableFormats[0];
	}

	/**
	 * 确定交换链 extent：currentExtent.width == UINT32_MAX（surface 处于"尺寸待定"态，
	 * 常见于可 resize 窗口）时取 SDL 窗口当前尺寸并 clamp 到 [minImageExtent,
	 * maxImageExtent]；否则直接用 currentExtent。
	 * @param surfaceCapabilities surface 能力（调用方每次重建前查询）
	 * @return 交换链 extent（resize 后重建 swapchain 用）
	 */
	vk::Extent2D getSwapchainExtend(const vk::SurfaceCapabilitiesKHR& surfaceCapabilities) const {
		vk::Extent2D swapchainExtent = surfaceCapabilities.currentExtent;

		if (surfaceCapabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
			swapchainExtent = surfaceCapabilities.currentExtent;
		} else {
			int w, h;
			SDL_GetWindowSize(window, &w, &h);
			swapchainExtent.width = std::clamp(static_cast<uint32_t>(w),
				surfaceCapabilities.minImageExtent.width,
				surfaceCapabilities.maxImageExtent.width);
			swapchainExtent.height = std::clamp(static_cast<uint32_t>(h),
				surfaceCapabilities.minImageExtent.height,
				surfaceCapabilities.maxImageExtent.height);
		}
		return swapchainExtent;
	}

	/**
	 * 确定交换链图像数：Wayland 窗口（带 WAYLAND_SURFACE_POINTER 属性）=
	 * minImageCount（不加冗余）；其他平台 = minImageCount+1，maxImageCount>0 时
	 * 以其为上限。
	 * @param minImageCount 驱动最小图像数
	 * @param maxImageCount 驱动最大图像数（0 = 不限）
	 * @return 采用的图像数
	 */
	uint32_t getImageCount(uint32_t minImageCount, uint32_t maxImageCount) const {
		SDL_PropertiesID props = SDL_GetWindowProperties(window);
		uint32_t imageCount = minImageCount;

		if (SDL_HasProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER)) {
			return imageCount;
		}

		imageCount = imageCount + 1;

		if (maxImageCount > 0 && imageCount > maxImageCount) {
			imageCount = maxImageCount;
		}

		return imageCount;
	}

   private:
	SDL_Window* window = nullptr;
	int width;
	int height;
	RLLogger* logger;
};

}  // namespace RGL