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

inline std::string Error::describe() const {
	return message;
}

inline bool operator==(const Error& lhs, const Error& rhs) {
	return lhs.code == rhs.code && lhs.message == rhs.message;
}

}  // namespace RGL