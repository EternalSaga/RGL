module;
#include <png.h>      // libpng 简单 API（PNG 解码/编码；GMF 第三方 C 头）
#include <jpeglib.h>  // libjpeg(-turbo)（JPEG 内存源解码 + JCS_EXT_RGBA）
#include <FreeImage.h>  // FreeImage（.hdr 解码）
#include <setjmp.h>   // JPEG 致命错误 setjmp/longjmp（libjpeg 标准错误处理模式）
module image_codec:impl;

import image_codec;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;

namespace RGL {

namespace {

/** JPEG 解码错误状态（setjmp 落点 + 标准 error_mgr 前置）：致命错误经 error_exit
 *  回调 longjmp 回调用点——损坏文件返回 nullopt，而非默认处理器直接 exit 进程。 */
struct JpegErrorState {
	jpeg_error_mgr pub;
	jmp_buf jump;
};

/** 致命错误 → longjmp 回调用方 setjmp 点（libjpeg 文档标准模式）。 */
void jpegErrorExit(j_common_ptr cinfo) {
	// pub 是首成员、无虚基：指针值同构，C 库惯例 reinterpret_cast
	auto* err = reinterpret_cast<JpegErrorState*>(cinfo->err);
	longjmp(err->jump, 1);
}

/** 静默 libjpeg 控制台输出（解码诊断走返回值，不刷日志）。 */
void jpegOutputMessage(j_common_ptr) {}

/**
 * PNG 内存解码（libpng 简单 API）：begin_read 读头（得原始 format）→ 请求
 * PNG_FORMAT_RGBA 输出（libpng 处理灰度/RGB 扩通道、16→8、调色板展开、隔行）→
 * finish_read 解出紧凑行缓冲。无 alpha 源图显式补 alpha=255（对齐 stb，
 * 不依赖 libpng 默认值）。
 * @param data PNG 字节数据
 * @param size 字节数
 * @return RGBA8；头不可读 / 解码失败 → nullopt
 */
std::optional<DecodedImage> decodePngMemory(const uint8_t* data, size_t size) {
	png_image image;
	std::memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&image, data, size))
		return std::nullopt;
	const bool hasAlpha = (image.format & PNG_FORMAT_FLAG_ALPHA) != 0;
	image.format = PNG_FORMAT_RGBA;
	DecodedImage out;
	out.width = image.width;
	out.height = image.height;
	out.rgba.resize(static_cast<size_t>(image.width) * image.height * 4);
	if (!png_image_finish_read(&image, nullptr, out.rgba.data(),
	        static_cast<png_int_32>(image.width * 4), nullptr))
		return std::nullopt;
	if (!hasAlpha)
		for (size_t i = 3; i < out.rgba.size(); i += 4)
			out.rgba[i] = 255;
	return out;
}

/**
 * JPEG 内存解码（libjpeg-turbo jpeg_mem_src + JCS_EXT_RGBA：RGB + A=0xFF，
 * 与 stb 补 alpha=255 对齐）。
 * @param data JPEG 字节数据
 * @param size 字节数
 * @return RGBA8；头不可读 / 解码中途致命错误（setjmp 捕获）→ nullopt
 */
std::optional<DecodedImage> decodeJpegMemory(const uint8_t* data, size_t size) {
	jpeg_decompress_struct cinfo;
	JpegErrorState err;
	cinfo.err = jpeg_std_error(&err.pub);
	err.pub.error_exit = jpegErrorExit;
	err.pub.output_message = jpegOutputMessage;
	if (setjmp(err.jump)) {
		jpeg_destroy_decompress(&cinfo);
		return std::nullopt;
	}
	jpeg_create_decompress(&cinfo);
	jpeg_mem_src(&cinfo, data, size);
	jpeg_read_header(&cinfo, TRUE);
	cinfo.out_color_space = JCS_EXT_RGBA;
	jpeg_start_decompress(&cinfo);
	DecodedImage out;
	out.width = cinfo.output_width;
	out.height = cinfo.output_height;
	out.rgba.resize(static_cast<size_t>(out.width) * out.height * 4);
	while (cinfo.output_scanline < cinfo.output_height) {
		auto* row = out.rgba.data() + static_cast<size_t>(cinfo.output_scanline) * out.width * 4;
		jpeg_read_scanlines(&cinfo, &row, 1);
	}
	jpeg_finish_decompress(&cinfo);
	jpeg_destroy_decompress(&cinfo);
	return out;
}

/** FreeImage 生命周期守卫（magic static，线程安全）：首调 decodeHdr 时 Initialise，
 *  进程退出时 DeInitialise。 */
struct FreeImageLifecycle {
	/** 惰性初始化（静态变量构造时调用一次）。 */
	FreeImageLifecycle() { FreeImage_Initialise(); }
	/** 进程退出释放（静态变量析构时调用一次）。 */
	~FreeImageLifecycle() { FreeImage_DeInitialise(); }
};

}  // namespace

/** 实现（契约见接口声明）：magic 嗅探（PNG `89 50 4E 47` / JPEG `FF D8`）→ 分发解码。 */
std::optional<DecodedImage> decodeRgba8(const uint8_t* data, size_t size) {
	if (size >= 4 && data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
		return decodePngMemory(data, size);
	if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8)
		return decodeJpegMemory(data, size);
	return std::nullopt;
}

/** 实现（契约见接口声明）：整读文件 → decodeRgba8（tellg 长度 auto 推导——
 *  streamoff/streamsize 不在 rlstd 导出面，避免命名类型）。 */
std::optional<DecodedImage> decodeRgba8File(const std::string& path) {
	std::ifstream in(path, std::ios::binary);
	if (!in)
		return std::nullopt;
	in.seekg(0, std::ios::end);
	const auto length = in.tellg();
	if (length < 0)
		return std::nullopt;
	in.seekg(0, std::ios::beg);
	std::vector<uint8_t> bytes(static_cast<size_t>(length));
	if (length > 0 && !in.read(reinterpret_cast<char*>(bytes.data()), length))
		return std::nullopt;
	return decodeRgba8(bytes.data(), bytes.size());
}

/** 实现（契约见接口声明）：libpng 简单 API 写盘（8 位 RGBA，行距 = width×4）。 */
bool writePng(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba) {
	png_image image;
	std::memset(&image, 0, sizeof(image));
	image.version = PNG_IMAGE_VERSION;
	image.width = width;
	image.height = height;
	image.format = PNG_FORMAT_RGBA;
	return png_image_write_to_file(&image, path.c_str(), 0, rgba,
	        static_cast<png_int_32>(width * 4), nullptr) != 0;
}

/** 实现（契约见接口声明）：FreeImage_Load(FIF_HDR) → FIT_RGBF 校验 → 反序读 scanline
 *  （文件第 y 行在 scanline height−1−y，反序还原 top-down）→ 逐像素取 RGB 三通道。 */
std::optional<DecodedImageF> decodeHdr(const std::string& path) {
	static FreeImageLifecycle lifecycle;
	FIBITMAP* bitmap = FreeImage_Load(FIF_HDR, path.c_str(), 0);
	if (!bitmap)
		return std::nullopt;
	if (FreeImage_GetImageType(bitmap) != FIT_RGBF) {
		FreeImage_Unload(bitmap);
		return std::nullopt;
	}
	const uint32_t width = FreeImage_GetWidth(bitmap);
	const uint32_t height = FreeImage_GetHeight(bitmap);
	DecodedImageF out;
	out.width = width;
	out.height = height;
	out.rgb.resize(static_cast<size_t>(width) * height * 3);
	for (uint32_t y = 0; y < height; ++y) {
		const auto* line = reinterpret_cast<const FIRGBF*>(
			FreeImage_GetScanLine(bitmap, height - 1 - y));
		for (uint32_t x = 0; x < width; ++x) {
			out.rgb[(static_cast<size_t>(y) * width + x) * 3 + 0] = line[x].red;
			out.rgb[(static_cast<size_t>(y) * width + x) * 3 + 1] = line[x].green;
			out.rgb[(static_cast<size_t>(y) * width + x) * 3 + 2] = line[x].blue;
		}
	}
	FreeImage_Unload(bitmap);
	return out;
}

}  // namespace RGL
