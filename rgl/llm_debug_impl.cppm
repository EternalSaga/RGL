module;
#include <stb_image_write.h>
#include <filesystem>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
module llm_debug:capture;

import llm_debug;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_renderer;
import runtime_config;

namespace RGL {

namespace {

// uint32 位模式 → float（免 std::bit_cast，memcpy 走 rlstd 导出面）
float u32ToFloat(uint32_t b) {
	float f;
	std::memcpy(&f, &b, sizeof(f));
	return f;
}

// IEEE binary16 → float（HDR 后缓冲 R16G16B16A16_SFLOAT 回读转换）
float halfToFloat(uint16_t h) {
	const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
	int32_t exp = (h >> 10) & 0x1F;
	uint32_t frac = h & 0x3FFu;
	if (exp == 0) {
		if (frac == 0)
			return u32ToFloat(sign);
		exp = 127 - 15 + 1;  // denorm → 规格化
		while ((frac & 0x400u) == 0) {
			frac <<= 1;
			--exp;
		}
		frac &= 0x3FFu;
		return u32ToFloat(sign | (static_cast<uint32_t>(exp) << 23) | (frac << 13));
	}
	if (exp == 31)
		return u32ToFloat(sign | 0x7F800000u | (frac << 13));
	exp = exp - 15 + 127;
	return u32ToFloat(sign | (static_cast<uint32_t>(exp) << 23) | (frac << 13));
}

// 线性 [0,1] → sRGB 字节（PNG 惯例存 sRGB 编码值；HDR/线性 UNORM 后缓冲需要编码）
uint8_t toSrgb(float v) {
	v = std::clamp(v, 0.0f, 1.0f);
	const float s = v <= 0.0031308f ? v * 12.92f
	                                : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
	return static_cast<uint8_t>(s * 255.0f + 0.5f);
}

// 诊断直出（GB float 通道：clamp 线性映射，不做 gamma——数据观察优先语义直观）
uint8_t raw8(float v) {
	return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

// 回读原格式 → RGBA8（stride 4）。gbufRaw = dump_gbuf 诊断直读（无 gamma）。
// 覆盖 = swapchain 候选格式（sdl3window selectFormat）+ G-buffer 词表 4 格式；
// 表外 = 返回空（调用方报错，不静默出图）。
std::vector<uint8_t> toRgba8(const Renderer::FrameCapture& cap, bool gbufRaw) {
	const size_t px = static_cast<size_t>(cap.width) * cap.height;
	std::vector<uint8_t> out(px * 4, 255);
	const uint8_t* src = cap.pixels.data();
	switch (cap.format) {
		case vk::Format::eB8G8R8A8Srgb:  // BGRA 字节序，值已 sRGB 编码：仅换通道
			for (size_t i = 0; i < px; ++i) {
				out[i * 4 + 0] = src[i * 4 + 2];
				out[i * 4 + 1] = src[i * 4 + 1];
				out[i * 4 + 2] = src[i * 4 + 0];
			}
			return out;
		case vk::Format::eB8G8R8A8Unorm:  // BGRA 线性：换通道 + gamma
			for (size_t i = 0; i < px; ++i) {
				out[i * 4 + 0] = toSrgb(src[i * 4 + 2] / 255.0f);
				out[i * 4 + 1] = toSrgb(src[i * 4 + 1] / 255.0f);
				out[i * 4 + 2] = toSrgb(src[i * 4 + 0] / 255.0f);
			}
			return out;
		case vk::Format::eR8G8B8A8Srgb:  // 已 sRGB 编码且通道序一致：原样拷贝
			std::memcpy(out.data(), src, px * 4);
			return out;
		case vk::Format::eR8G8B8A8Unorm:
			for (size_t i = 0; i < px; ++i) {
				for (int c = 0; c < 3; ++c) {
					const float v = src[i * 4 + c] / 255.0f;
					out[i * 4 + c] = gbufRaw ? static_cast<uint8_t>(src[i * 4 + c]) : toSrgb(v);
				}
			}
			return out;
		case vk::Format::eA2B10G10R10UnormPack32:
			for (size_t i = 0; i < px; ++i) {
				uint32_t p;
				std::memcpy(&p, src + i * 4, 4);
				out[i * 4 + 0] = toSrgb(static_cast<float>(p & 0x3FFu) / 1023.0f);
				out[i * 4 + 1] = toSrgb(static_cast<float>((p >> 10) & 0x3FFu) / 1023.0f);
				out[i * 4 + 2] = toSrgb(static_cast<float>((p >> 20) & 0x3FFu) / 1023.0f);
			}
			return out;
		case vk::Format::eR16G16B16A16Sfloat:  // HDR：half→float，clamp（旧 UNORM RT 同近似）
			for (size_t i = 0; i < px; ++i) {
				for (int c = 0; c < 3; ++c) {
					const float v = halfToFloat(
						static_cast<uint16_t>(src[i * 8 + c * 2] | (src[i * 8 + c * 2 + 1] << 8)));
					out[i * 4 + c] = gbufRaw ? raw8(v) : toSrgb(v);
				}
			}
			return out;
		case vk::Format::eR16G16Snorm:  // GB normal：oct 编码 (x,y)∈[-1,1] → 中点 0.5 展开
			for (size_t i = 0; i < px; ++i) {
				const auto x = static_cast<int16_t>(src[i * 4] | (src[i * 4 + 1] << 8));
				const auto y = static_cast<int16_t>(src[i * 4 + 2] | (src[i * 4 + 3] << 8));
				out[i * 4 + 0] = raw8(static_cast<float>(x) / 32767.0f * 0.5f + 0.5f);
				out[i * 4 + 1] = raw8(static_cast<float>(y) / 32767.0f * 0.5f + 0.5f);
			}
			return out;
		case vk::Format::eR32G32B32A32Sfloat:  // GB position：世界坐标 clamp 显示
			for (size_t i = 0; i < px; ++i) {
				for (int c = 0; c < 3; ++c) {
					float v;
					std::memcpy(&v, src + i * 16 + c * 4, 4);
					out[i * 4 + c] = raw8(v);
				}
			}
			return out;
		default:
			return {};
	}
}

}  // namespace

std::expected<std::string, Error> writeCapturePng(RGL::Renderer& renderer, const LlmDebugConfig& cfg) {
	auto* logger = RLLogger::getInstance();
	auto cap = renderer.takeFrameCapture();
	if (!cap)
		return std::unexpected(Error{ErrorCode::Unknown, "llm_debug: no frame capture pending"});
	const bool gbufRaw = cap->gbufChannel >= 0;
	auto rgba = toRgba8(*cap, gbufRaw);
	if (rgba.empty()) {
		if (logger)
			logger->error("llm_debug: 回读格式 {} 无转换规则", static_cast<int>(cap->format));
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("llm_debug: unsupported capture format {}",
				static_cast<int>(cap->format))});
	}
	std::filesystem::path relPath;
	if (gbufRaw)
		relPath = fmt::format("screenshots/gbuffer_{}.png", cap->gbufChannel);
	else if (cfg.file.empty())
		relPath = fmt::format("screenshots/llm_{}.png",
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
	else
		relPath = cfg.file;
	std::error_code ec;
	std::filesystem::create_directories(relPath.parent_path(), ec);
	if (!stbi_write_png(relPath.string().c_str(), static_cast<int>(cap->width),
	        static_cast<int>(cap->height), 4, rgba.data(), static_cast<int>(cap->width) * 4)) {
		if (logger)
			logger->error("llm_debug: stbi_write_png failed: {}", relPath.string());
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("llm_debug: failed to write PNG: {}", relPath.string())});
	}
	return std::filesystem::absolute(relPath).string();
}

}  // namespace RGL
