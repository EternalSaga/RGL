module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏；C 头解析 ~0.4s）
#include <fmt/format.h>
#include <fmt/ranges.h>
export module features;

import rlstd;
import rlvk;
import vkerror;
import features_generated;

export import features_generated;

namespace RGL {

// 设备扩展单一声明点（check 与 enable 共用——旧实现两处手工重复同一组 8 个，
// 加一处漏一处 = 校验/启用不一致）
inline const std::vector<const char*>& kDeviceExtensions() {
	static const std::vector<const char*> extensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_EXT_MESH_SHADER_EXTENSION_NAME,
		VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
		VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME};
	return extensions;
}

export [[nodiscard]] std::expected<void, Error> checkDeviceExtensions(const vk::raii::PhysicalDevice& device) {
	auto available = device.enumerateDeviceExtensionProperties();
	const auto& required = kDeviceExtensions();

	std::vector<const char*> missing;
	for (const char* ext : required) {
		bool found = std::ranges::any_of(available, [ext](const auto& prop) {
			return std::string_view(prop.extensionName.data()) == ext;
		});
		if (!found) {
			missing.push_back(ext);
		}
	}

	if (!missing.empty()) {
		std::string msg = fmt::format("Required extension not supported by device: {}",
			fmt::join(missing, ", "));
		return std::unexpected(Error{ErrorCode::ExtensionUnavailable, msg});
	}
	return {};
}

export std::vector<const char*> getDeviceExtensions() {
	return kDeviceExtensions();
}

// 可选扩展：仅在对应 optional feature 探测可用时追加。扩展启用与 feature 置位同源
// （queryOptionalFeatures 先查扩展可用再读 feature 字段，缺一即 false = 整体 no-op），
// 故此处按 OptionalFeatureSupport 位逐项追加即可——跨驱动降级不破坏启动。
export std::vector<const char*> getDeviceExtensions(const OptionalFeatureSupport& opt) {
	auto extensions = kDeviceExtensions();
	if (opt.depthClipEnable)
		extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
	if (opt.depthClipBase)  // edS3 的 depth clip 动态化以旧扩展为底座（VUID-07451，
		extensions.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);  // 两者须同开）
	return extensions;
}

}  // namespace RGL
