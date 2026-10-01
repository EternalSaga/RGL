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

/**
 * 设备扩展单一声明点（check 与 enable 共用——旧实现两处手工重复同一组 8 个，
 * 加一处漏一处 = 校验/启用不一致）。
 * @note VK_EXT_descriptor_buffer / VK_KHR_shader_untyped_pointers 当前代码无显式使用，
 * 但被其他已启用扩展隐式依赖，不可随意删除。
 * @note VK_EXT_device_generated_commands 显式使用（Phase 7b GPU culling：vk_indirect_commands
 * 模块 + culling compute，见 docs/device_generated_commands_research.md）；双已知驱动
 * （RADV/NVIDIA）均无条件 advertise，required 语义 fail-fast；Intel 平台 advertise 未实测。
 * @return 必需扩展名列表（静态存储，进程级）
 */
inline const std::vector<const char*>& kDeviceExtensions() {
	static const std::vector<const char*> extensions = {
		VK_KHR_SWAPCHAIN_EXTENSION_NAME,
		VK_EXT_MESH_SHADER_EXTENSION_NAME,
		VK_EXT_SHADER_OBJECT_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_HEAP_EXTENSION_NAME,
		VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
		VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME,
		VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_SHADER_UNTYPED_POINTERS_EXTENSION_NAME,
		VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME};
	return extensions;
}

/**
 * 校验设备是否支持全部必需扩展（kDeviceExtensions 清单）。
 * @param device 待查询的物理设备
 * @return 全部支持返回空值；缺失时返回 Error(ExtensionUnavailable, 缺失扩展名列表)
 */
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

/** @return 必需设备扩展列表（与 check 同源，供 vkCreateDevice 启用）。 */
export std::vector<const char*> getDeviceExtensions() {
	return kDeviceExtensions();
}

/**
 * Phase 9.3 present_timing 设备侧**整组门**：三扩展（VK_EXT_present_timing +
 * VK_KHR_present_id2 + VK_KHR_calibrated_timestamps）全探测可用才视为支持。
 * 同一判定供 getDeviceExtensions（启用）、vkdevice 条件链 feature 结构、
 * vkswapchain surface 探测门三处消费——同源不可分叉（部分启用 = 启用但永不使用，
 * 白增设备开销且违背"全可用才启用"验收口径）。
 * @param opt queryOptionalFeatures 探测结果
 * @return true = 三扩展齐备（注意：还须实例侧 surface_capabilities2 与
 *   diagnostics.profiler>0 与 surface/模式探测（vkswapchain 侧），才是完整激活）
 */
export bool presentTimingSupported(const OptionalFeatureSupport& opt) {
	return opt.presentTiming && opt.presentId2 && opt.calibratedTimestampsKhr;
}

/**
 * edS3 depthClip 动态状态**双条件能力门**（同 presentTimingSupported 家族）：
 * extendedDynamicState3DepthClipEnable 只是把旧扩展的 depth clip 静态状态"动态化"，
 * VUID-vkCmdSetDepthClipEnableEXT-depthClipEnable-07451 查的是旧扩展
 * VK_EXT_depth_clip_enable 的 depthClipEnable feature——只暴露 edS3 而无 base 的驱动上
 * 单条件放行 = 调用违 VUID（docs/archive/issue_history.md 2026-09 条目拍板"缺一即不调，
 * 干净降级"；vkrenderer 构造消费本门喂 StateKit，vkdevice 侧两 feature 结构条件链接同源）。
 * @param opt queryOptionalFeatures 探测结果
 * @return true = edS3 动态位与旧扩展 base 位齐备（才可动态调 vkCmdSetDepthClipEnableEXT）
 */
export bool depthClipDynamicSupported(const OptionalFeatureSupport& opt) {
	return opt.depthClipEnable && opt.depthClipBase;
}

/**
 * 设备创建用的必需 + 可选扩展列表。可选扩展：仅在对应 optional feature 探测可用时
 * 追加。扩展启用与 feature 置位同源（queryOptionalFeatures 先查扩展可用再读 feature
 * 字段，缺一即 false = 整体 no-op），故此处按 OptionalFeatureSupport 位逐项追加即可——
 * 跨驱动降级不破坏启动。depthClipBase 依赖 extended_dynamic_state_3 为底座
 *（VUID-07451，两者须同开，见函数内逐条追加）。
 * @param opt queryOptionalFeatures 探测结果
 * @return 待启用扩展列表（必需 + 命中的可选）
 */
export std::vector<const char*> getDeviceExtensions(const OptionalFeatureSupport& opt) {
	auto extensions = kDeviceExtensions();
	if (opt.depthClipEnable)
		extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
	if (opt.depthClipBase)  // edS3 的 depth clip 动态化以旧扩展为底座（VUID-07451，
		extensions.push_back(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);  // 两者须同开）
	if (opt.robustBufferAccess2 || opt.nullDescriptor)  // 与 vkdevice 条件链接同源
		extensions.push_back(VK_KHR_ROBUSTNESS_2_EXTENSION_NAME);
	if (opt.calibratedTimestamps)  // Phase 9.2 GPU 时间戳：设备↔CLOCK_MONOTONIC 校准
		extensions.push_back(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);  //（无 feature 结构，纯扩展启用）
	if (presentTimingSupported(opt)) {  // Phase 9.3 present_timing：三扩展全有才整组启用
		extensions.push_back(VK_EXT_PRESENT_TIMING_EXTENSION_NAME);
		extensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
		if (opt.calibratedTimestampsKhr)  // KHR 校准（present timing 时间域换算，与 EXT 同名异入口）
			extensions.push_back(VK_KHR_CALIBRATED_TIMESTAMPS_EXTENSION_NAME);
	}
	return extensions;
}

}  // namespace RGL
