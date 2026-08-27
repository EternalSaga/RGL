module;
#include <SDL3/SDL.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
export module vk_instance;

import rlstd;
import rllogger;
import vulkan_utils;
import vulkan_window;
import vkerror;

namespace RGL {

export class AppContext {
   public:
	AppContext(RGL::SDLWindow& window)
		: context(), apiVersion(context.enumerateInstanceVersion()), instance(context, buildInstanceCreateInfo(window)), debugMessenger(buildDebugMessenger(instance)) {}

	vk::raii::Instance& getInstance() { return instance; }
	const vk::raii::Instance& getInstance() const { return instance; }
	uint32_t getApiVersion() const { return apiVersion; }

   private:
	vk::InstanceCreateInfo buildInstanceCreateInfo(RGL::SDLWindow& window) {
		auto extensionsResult = window.getSurfaceExtensions();
		if (!extensionsResult) {
			std::string msg = extensionsResult.error().describe();
			throw std::runtime_error(msg);
		}
		auto [surfaceExtensions, surfaceCount] = *extensionsResult;

		engineInfo.applicationVersion = vk::makeVersion(0, 1, 0);
		engineInfo.apiVersion = apiVersion;
		engineInfo.engineVersion = vk::makeVersion(0, 0, 1);
		engineInfo.pEngineName = "practice";

		instanceExtensions = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
			VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
			VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
			VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME};
		std::ranges::copy_n(surfaceExtensions, surfaceCount,
			std::back_inserter(instanceExtensions));

		const auto availableLayers = context.enumerateInstanceLayerProperties();
		if (!RGL::findMissingValidationLayers(availableLayers, layers).empty()) {
			throw std::runtime_error("Missing validation layers!");
		}

		instanceCreateInfo.setPApplicationInfo(&engineInfo);
		// DIAG（4K 黑屏排查 2026-08-27）：RGL_NO_VVL=1 关闭 VVL——VVL 的
		// descriptor heap wrapper 对同一 slot 双写（先零化后写实际描述符，
		// lldb watchpoint 实测），疑为黑屏/TCP fault 根因；验证后恢复无条件启用
		if (!std::getenv("RGL_NO_VVL"))
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