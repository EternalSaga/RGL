module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
export module vk_instance;

import rlstd;
import rlvk;
import rllogger;
import diagnostics;  // no_vvl 诊断单例（configs/diagnostics.lua，main 启动期已 setDiagnostics）
import vulkan_utils;
import vulkan_window;
import vkerror;

namespace RGL {

export class AppContext {
   public:
	// headless（VK_EXT_headless_surface）：脱离 SDL/compositor——SDL 窗口可不建，
	// instance 扩展用 VK_EXT_headless_surface 替代 SDL surface 扩展（surface 由
	// createHeadlessSurfaceEXT 创建，见 vkswapchain.cppm）。headless=false 时行为不变
	AppContext(RGL::SDLWindow* window, bool headless = false)
		: headless(headless), context(), apiVersion(context.enumerateInstanceVersion()), instance(context, buildInstanceCreateInfo(window, headless)), debugMessenger(buildDebugMessenger(instance)) {}

	vk::raii::Instance& getInstance() { return instance; }
	const vk::raii::Instance& getInstance() const { return instance; }
	uint32_t getApiVersion() const { return apiVersion; }
	bool isHeadless() const { return headless; }

   private:
	bool headless;

	vk::InstanceCreateInfo buildInstanceCreateInfo(RGL::SDLWindow* window, bool headless) {
		engineInfo.applicationVersion = vk::makeVersion(0, 1, 0);
		engineInfo.apiVersion = apiVersion;
		engineInfo.engineVersion = vk::makeVersion(0, 0, 1);
		engineInfo.pEngineName = "practice";

		instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
			VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
			VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
			VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
		if (headless) {
			// headless：无 SDL surface 扩展，但 surface 族扩展（maintenance1/capabilities2/
			// colorspace/headless）都依赖 VK_KHR_surface，须显式启用
			instanceExtensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
			instanceExtensions.push_back(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME);
		} else {
			auto extensionsResult = window->getSurfaceExtensions();
			if (!extensionsResult) {
				std::string msg = extensionsResult.error().describe();
				throw std::runtime_error(msg);
			}
			auto [surfaceExtensions, surfaceCount] = *extensionsResult;
			std::ranges::copy_n(surfaceExtensions, surfaceCount,
				std::back_inserter(instanceExtensions));
		}

		const auto availableLayers = context.enumerateInstanceLayerProperties();
		if (!RGL::findMissingValidationLayers(availableLayers, layers).empty()) {
			throw std::runtime_error("Missing validation layers!");
		}

		instanceCreateInfo.setPApplicationInfo(&engineInfo);
		// DIAG（configs/diagnostics.lua no_vvl，原 RGL_NO_VVL env 2026-09-08 转配置）：
		// 关 VVL——VVL 的 descriptor heap wrapper 对同一 slot 双写（先零化后写实际描述符），
		// 排查黑屏/fault 时隔离嫌疑；默认 false = VVL 常开
		if (!diagnostics().noVvl)
			instanceCreateInfo.setPEnabledLayerNames(layers);
		instanceCreateInfo.setPEnabledExtensionNames(instanceExtensions);
		return instanceCreateInfo;
	}

	vk::raii::DebugUtilsMessengerEXT buildDebugMessenger(vk::raii::Instance& inst) {
		auto result = RGL::setupDebugMessenger(inst);
		if (!result) {
			throw std::runtime_error(result.error().describe());
		}
		return std::move(*result);
	}

	vk::raii::Context context;
	uint32_t apiVersion;
	vk::ApplicationInfo engineInfo;
	vk::InstanceCreateInfo instanceCreateInfo;
	std::vector<const char*> layers = {"VK_LAYER_KHRONOS_validation"};
	std::vector<const char*> instanceExtensions;
	vk::raii::Instance instance;
	vk::raii::DebugUtilsMessengerEXT debugMessenger;
};

}  // namespace RGL