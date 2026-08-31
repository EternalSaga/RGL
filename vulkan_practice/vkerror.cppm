module;
#include <vulkan/vulkan.hpp>
#include <string>
export module vkerror;

import rlstd;

namespace RGL {

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
	Unknown,
};

export struct Error {
	ErrorCode code;
	std::string message;

	static Error fromVkResult(vk::Result result, const char* context = nullptr);

	std::string describe() const;
};

inline Error Error::fromVkResult(vk::Result result, const char* context) {
	Error err;
	err.code = ErrorCode::VulkanError;
	auto str = vk::to_string(result);
	if (context) {
		err.message = std::string(context) + ": " + str;
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
	return std::string("[") + errorCodeName(code) + "] " + message;
}

inline bool operator==(const Error& lhs, const Error& rhs) {
	return lhs.code == rhs.code && lhs.message == rhs.message;
}

}  // namespace RGL