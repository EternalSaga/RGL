module;
#include <filesystem>
#include <chrono>
#include <fmt/format.h>
module llm_debug:capture;

import llm_debug;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import image_codec;  // PNG 写盘（替代旧 stb_image_write）
import rlvk;
import rllogger;
import vkerror;
import vk_renderer;
import runtime_config;

namespace RGL {

namespace {

/** uint32 位模式 → float（免 std::bit_cast，memcpy 走 rlstd 导出面）。 */
float u32ToFloat(uint32_t b) {
	float f;
	std::memcpy(&f, &b, sizeof(f));
	return f;
}

/**
 * IEEE binary16 → float（HDR 后缓冲 R16G16B16A16_SFLOAT 回读转换）。
 * denorm（exp=0）走规格化移位；exp=31 透传 ±Inf/NaN（frac 原样搬入尾数）。
 * @param h 16 位半精度位模式（调用端按小端拼合）
 * @return 对应单精度值
 */
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

/**
 * 线性 [0,1] → sRGB 字节（PNG 惯例存 sRGB 编码值；HDR/线性 UNORM 后缓冲需要编码）。
 * 标准 sRGB 传输函数（0.0031308 分段阈值），先 clamp 再四舍五入取整。
 * @param v 线性值（可越界，clamp 到 [0,1]）
 * @return sRGB 编码字节
 */
uint8_t toSrgb(float v) {
	v = std::clamp(v, 0.0f, 1.0f);
	const float s = v <= 0.0031308f ? v * 12.92f
	                                : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
	return static_cast<uint8_t>(s * 255.0f + 0.5f);
}

/** 诊断直出（GB float 通道：clamp 线性映射，不做 gamma——数据观察优先语义直观）。 */
uint8_t raw8(float v) {
	return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/** B8G8R8A8_SRGB → RGBA8：BGRA 字节序、值已 sRGB 编码，仅换 B/R 通道。 */
void convBgraSrgb(const uint8_t* src, uint8_t* out, size_t px) {
	for (size_t i = 0; i < px; ++i) {
		out[i * 4 + 0] = src[i * 4 + 2];
		out[i * 4 + 1] = src[i * 4 + 1];
		out[i * 4 + 2] = src[i * 4 + 0];
	}
}

/** B8G8R8A8_UNORM → RGBA8：换 B/R 通道 + gamma（线性 UNORM 后缓冲需编码）。 */
void convBgraUnorm(const uint8_t* src, uint8_t* out, size_t px) {
	for (size_t i = 0; i < px; ++i) {
		out[i * 4 + 0] = toSrgb(src[i * 4 + 2] / 255.0f);
		out[i * 4 + 1] = toSrgb(src[i * 4 + 1] / 255.0f);
		out[i * 4 + 2] = toSrgb(src[i * 4 + 0] / 255.0f);
	}
}

/** RGBA8_UNORM → RGBA8：逐通道 gamma；gbufRaw 诊断直出 = 原字节（无 gamma）。 */
void convRgbaUnorm(const uint8_t* src, uint8_t* out, size_t px, bool gbufRaw) {
	for (size_t i = 0; i < px; ++i) {
		for (int c = 0; c < 3; ++c) {
			const float v = src[i * 4 + c] / 255.0f;
			out[i * 4 + c] = gbufRaw ? static_cast<uint8_t>(src[i * 4 + c]) : toSrgb(v);
		}
	}
}

/** A2B10G10R10_UNORM_PACK32 → RGBA8：拆 10bit 通道归一（/1023）+ gamma。 */
void convA2B10G10R10(const uint8_t* src, uint8_t* out, size_t px) {
	for (size_t i = 0; i < px; ++i) {
		uint32_t p;
		std::memcpy(&p, src + i * 4, 4);
		out[i * 4 + 0] = toSrgb(static_cast<float>(p & 0x3FFu) / 1023.0f);
		out[i * 4 + 1] = toSrgb(static_cast<float>((p >> 10) & 0x3FFu) / 1023.0f);
		out[i * 4 + 2] = toSrgb(static_cast<float>((p >> 20) & 0x3FFu) / 1023.0f);
	}
}

/** R16G16B16A16_SFLOAT（HDR）→ RGBA8：half→float，gbufRaw 直出 raw8 / 否则 sRGB。
 * alpha = **raw8 直出**（数据不是颜色，与 convR32Uint/convR8Unorm 同纪律）：交付路径
 * alpha 恒 1.0 → 255 = 与旧行为（out 预填 255）逐字节相同，PNG 哈希不受影响；
 * taa_out 的拒收诊断码（`taa.diag_reject`）住在 alpha，raw 直出才可精确解码。 */
void convRgba16Half(const uint8_t* src, uint8_t* out, size_t px, bool gbufRaw) {
	for (size_t i = 0; i < px; ++i) {
		for (int c = 0; c < 3; ++c) {
			const float v = halfToFloat(
				static_cast<uint16_t>(src[i * 8 + c * 2] | (src[i * 8 + c * 2 + 1] << 8)));
			out[i * 4 + c] = gbufRaw ? raw8(v) : toSrgb(v);
		}
		out[i * 4 + 3] = raw8(halfToFloat(static_cast<uint16_t>(
			src[i * 8 + 6] | (src[i * 8 + 7] << 8))));
	}
}

/** R16G16_SNORM（GB normal）→ RGBA8：oct 编码 (x,y)∈[-1,1] → 中点 0.5 展开。 */
void convRg16Snorm(const uint8_t* src, uint8_t* out, size_t px) {
	for (size_t i = 0; i < px; ++i) {
		const auto x = static_cast<int16_t>(src[i * 4] | (src[i * 4 + 1] << 8));
		const auto y = static_cast<int16_t>(src[i * 4 + 2] | (src[i * 4 + 3] << 8));
		out[i * 4 + 0] = raw8(static_cast<float>(x) / 32767.0f * 0.5f + 0.5f);
		out[i * 4 + 1] = raw8(static_cast<float>(y) / 32767.0f * 0.5f + 0.5f);
	}
}

/** R32G32B32A32_SFLOAT（GB position）→ RGBA8：世界坐标 clamp 显示。 */
void convRgba32Sfloat(const uint8_t* src, uint8_t* out, size_t px) {
	for (size_t i = 0; i < px; ++i) {
		for (int c = 0; c < 3; ++c) {
			float v;
			std::memcpy(&v, src + i * 16 + c * 4, 4);
			out[i * 4 + c] = raw8(v);
		}
	}
}

/**
 * R8_UNORM（ao 附件，Phase 11 批 11.4）→ RGBA8：单通道灰度**原字节**展开到 RGB，A 恒 255。
 *
 * 与颜色格式的根本差别：AO 是**数据不是颜色**——它已是 [0,1] 线性遮蔽量，没有 sRGB 编码
 * 可解（过 `toSrgb` 会把 128 变成 188，读回再解一次又添取整误差），故一律 raw 直出。
 * 观测方（tools/imgqa）要问“这个像素的 AO 字节是多少”，且判据要能分辨 12 样本螺旋的
 * **离散台阶**（相邻台阶只差几个 LSB）⟹ 转换必须逐字节可逆。与 convR32Uint 同理：
 * **数据格式一律 raw，不走 gamma**。
 */
void convR8Unorm(const uint8_t* src, uint8_t* out, size_t px) {
	for (size_t i = 0; i < px; ++i) {
		const uint8_t v = src[i];  // r8_unorm = 1 字节/像素
		out[i * 4 + 0] = v;
		out[i * 4 + 1] = v;
		out[i * 4 + 2] = v;
	}
}

/**
 * R32_UINT（object_id，Phase 12 批 12.2）→ RGBA8：raw u32 像素 id **无损**编进
 * RGB24（小端字节序直接取前 3 字节，alpha 恒 255），不做 gamma / clamp。
 *
 * 为什么要有这个编码而不是让 QA 直接读裸 u32：回读产物唯一的落盘出口是 PNG
 * （`writeCapturePng`），而 QA 层拿到图后要能**逐像素精确解码** id——"这个 primitive
 * 覆盖了多少像素"是图像断言的锚点，容不得有损。
 *
 * **24 位上限**：id ≥ 2²⁴ 会静默回绕（高位丢弃）。当前图最大 objectIndex 来自
 * 场景 primitive 数（bistro 1591），离 1677 万极远；但若将来出现百万级 primitive 的
 * 场景，这条编码就不再有效——届时改走多图分片或 16bit 双图，不要在这里加"截断"分支。
 */
void convR32Uint(const uint8_t* src, uint8_t* out, size_t px) {
	for (size_t i = 0; i < px; ++i)
		std::memcpy(out + i * 4, src + i * 4, 3);  // R=id[7:0] G=[15:8] B=[23:16]
}

/**
 * 回读原格式 → RGBA8（行紧凑 stride 4；alpha 恒 255，输出初值即 255 白）。
 * gbufRaw = dump_gbuf 诊断直读（无 gamma）。覆盖 = swapchain 候选格式
 *（sdl3window selectFormat）+ G-buffer 词表 4 格式（albedo/metalRough 与
 * srgb/unorm 用例共用，normal rg16_snorm / position rgba32_sfloat 专用用例）+
 * object_id 的 r32_uint（批 12.2 QA 锚点源；**与 gbufRaw 无关**——id 是编码不是颜色，
 * 既不过 gamma 也不 clamp，见 convR32Uint）+ ao 附件的 r8_unorm（批 11.4 数据面，
 * 同样 raw 直出，见 convR8Unorm）；
 * 表外 = 返回空（调用方报错，不静默出图）。逐格式转换见上方 conv* 辅助。
 * @param cap 本帧捕获产物（width/height/format/pixels 原格式行紧凑）
 * @param gbufRaw true = dump_gbuf 直读通道（线性 clamp，不做 sRGB 编码）
 * @return RGBA8 字节序列（w×h×4）；空 = 该格式无转换规则
 */
std::vector<uint8_t> toRgba8(const Renderer::FrameCapture& cap, bool gbufRaw) {
	const size_t px = static_cast<size_t>(cap.width) * cap.height;
	std::vector<uint8_t> out(px * 4, 255);
	const uint8_t* src = cap.pixels.data();
	switch (cap.format) {
		case vk::Format::eB8G8R8A8Srgb:  // BGRA 字节序，值已 sRGB 编码：仅换通道
			convBgraSrgb(src, out.data(), px);
			return out;
		case vk::Format::eB8G8R8A8Unorm:  // BGRA 线性：换通道 + gamma
			convBgraUnorm(src, out.data(), px);
			return out;
		case vk::Format::eR8G8B8A8Srgb:  // 已 sRGB 编码且通道序一致：原样拷贝
			std::memcpy(out.data(), src, px * 4);
			return out;
		case vk::Format::eR8G8B8A8Unorm:
			convRgbaUnorm(src, out.data(), px, gbufRaw);
			return out;
		case vk::Format::eA2B10G10R10UnormPack32:
			convA2B10G10R10(src, out.data(), px);
			return out;
		case vk::Format::eR16G16B16A16Sfloat:  // HDR：half→float，clamp（旧 UNORM RT 同近似）
			convRgba16Half(src, out.data(), px, gbufRaw);
			return out;
		case vk::Format::eR16G16Snorm:  // GB normal：oct 编码 (x,y)∈[-1,1] → 中点 0.5 展开
			convRg16Snorm(src, out.data(), px);
			return out;
		case vk::Format::eR32G32B32A32Sfloat:  // GB position：世界坐标 clamp 显示
			convRgba32Sfloat(src, out.data(), px);
			return out;
		case vk::Format::eR32Uint:  // object_id（批 12.2 QA 锚点源）：raw id 无损编进 RGB24
			convR32Uint(src, out.data(), px);
			return out;
		case vk::Format::eR8Unorm:  // ao 附件（批 11.4）：数据非颜色，raw 灰度展开（见 convR8Unorm）
			convR8Unorm(src, out.data(), px);
			return out;
		default:
			return {};
	}
}

}  // namespace

/** 实现流水（语义/契约见接口声明）：takeFrameCapture（无产物 = Error）→
 * toRgba8（gbufRaw 由 gbufChannel 判定）→ 路径解析（gbuffer_N.png / attach_<名>.png /
 * cfg.file / 时间戳名）→ create_directories + image_codec::writePng → 返回绝对路径。 */
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
	else if (!cfg.file.empty())
		relPath = cfg.file;  // 显式路径优先（RPC 永远给；命名附件的默认名只是兜底）
	else if (!cap->attachmentName.empty())
		// 具名附件（批 12.2 QA）：object_id 走这条。llm_debug 一次性截图无 attachment，
		// 故此分支实际只服务"没给 file 的具名捕获"。
		relPath = fmt::format("screenshots/attach_{}.png", cap->attachmentName);
	else
		relPath = fmt::format("screenshots/llm_{}.png",
			std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::system_clock::now().time_since_epoch()).count());
	std::error_code ec;
	std::filesystem::create_directories(relPath.parent_path(), ec);
	if (!writePng(relPath.string(), cap->width, cap->height, rgba.data())) {
		if (logger)
			logger->error("llm_debug: writePng failed: {}", relPath.string());
		return std::unexpected(Error{ErrorCode::Unknown,
			fmt::format("llm_debug: failed to write PNG: {}", relPath.string())});
	}
	return std::filesystem::absolute(relPath).string();
}

}  // namespace RGL
