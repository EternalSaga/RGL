module;
#include <vulkan/vulkan_core.h>
#include <string>
#include <fmt/format.h>
export module vkerror;

import rlstd;

namespace RGL {

// Error/ErrorCode 全项目错误契约。GMF 只用 vulkan_core.h（C 头）而非 vulkan.hpp——
// 2026-09-02 编译提速实测：vulkan.hpp 解析 ~3s，本模块只需 VkResult 枚举 + 名字映射，
// core.h 总编译 3.9s→0.6s（-ftime-trace：Frontend 3079→415ms）；vkerror 是全依赖链
// 第 2 个模块（每个模块都 import 它），省下的 parse 时间在全链复利。
// fromVkResult 收 VkResult（C int）——调用点勿再包 vk::Result(...)。

export enum class ErrorCode {
	VulkanError,
	MissingValidationLayers,
	NoPhysicalDevices,
	NoSuitableGPU,
	NoPresentMode,
	ExtensionUnavailable,
	FeatureUnavailable,
	QueueNotAllocated,
	SurfaceNotPresentable,
	ShdrvCreateFailed,
	SwapchainAcquireFailed,
	SDLError,
	ShaderLoadFailed,
	GltfLoadFailed,
	TextureDecodeFailed,
	VMASuccessFailed,
	DescriptorSlotOutOfBounds,
	DeviceLost,
	ConfigLoadFailed,
	RenderGraphInvalid,
	Unknown,
};

export struct Error {
	ErrorCode code;
	std::string message;

	static Error fromVkResult(VkResult result, const char* context = nullptr);

	std::string describe() const;
};

// VkResult → 名字（诊断用，覆盖项目实际会遇到的结果码；未列出的走 Unknown 兜底）。
// 不用 vulkan.hpp 的 vk::to_string（那会把整个 hpp 上下文拖进本模块 GMF）
export inline const char* vkResultName(VkResult result) {
	switch (result) {
		case VK_SUCCESS: return "VK_SUCCESS";
		case VK_NOT_READY: return "VK_NOT_READY";
		case VK_TIMEOUT: return "VK_TIMEOUT";
		case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
		case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
		case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
		case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
		case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
		case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
		case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
		case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
		case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR: return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
		case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
		case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
		default: return "VK_ERROR_UNKNOWN";
	}
}

inline Error Error::fromVkResult(VkResult result, const char* context) {
	Error err;
	err.code = ErrorCode::VulkanError;
	auto str = std::string{vkResultName(result)};
	if (context) {
		err.message = fmt::format("{}: {}", context, str);
	} else {
		err.message = str;
	}
	return err;
}

// code 名（诊断用）：describe() 带 [Code] 前缀——旧实现只返回 message，
// 20 种 code 在 throw/log 路径全部丢失（同 message 不同根因无法区分）
inline const char* errorCodeName(ErrorCode code) {
	switch (code) {
		case ErrorCode::VulkanError: return "VulkanError";
		case ErrorCode::MissingValidationLayers: return "MissingValidationLayers";
		case ErrorCode::NoPhysicalDevices: return "NoPhysicalDevices";
		case ErrorCode::NoSuitableGPU: return "NoSuitableGPU";
		case ErrorCode::NoPresentMode: return "NoPresentMode";
		case ErrorCode::ExtensionUnavailable: return "ExtensionUnavailable";
		case ErrorCode::FeatureUnavailable: return "FeatureUnavailable";
		case ErrorCode::QueueNotAllocated: return "QueueNotAllocated";
		case ErrorCode::SurfaceNotPresentable: return "SurfaceNotPresentable";
		case ErrorCode::ShdrvCreateFailed: return "ShdrvCreateFailed";
		case ErrorCode::SwapchainAcquireFailed: return "SwapchainAcquireFailed";
		case ErrorCode::SDLError: return "SDLError";
		case ErrorCode::ShaderLoadFailed: return "ShaderLoadFailed";
		case ErrorCode::GltfLoadFailed: return "GltfLoadFailed";
		case ErrorCode::TextureDecodeFailed: return "TextureDecodeFailed";
		case ErrorCode::VMASuccessFailed: return "VMASuccessFailed";
		case ErrorCode::DescriptorSlotOutOfBounds: return "DescriptorSlotOutOfBounds";
		case ErrorCode::DeviceLost: return "DeviceLost";
		case ErrorCode::ConfigLoadFailed: return "ConfigLoadFailed";
		case ErrorCode::Unknown: return "Unknown";
	}
	return "Unreachable";
}

inline std::string Error::describe() const {
	return fmt::format("[{}] {}", errorCodeName(code), message);
}

inline bool operator==(const Error& lhs, const Error& rhs) {
	return lhs.code == rhs.code && lhs.message == rhs.message;
}

}  // namespace RGL
