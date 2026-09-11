module;
#include <FreeImage.h>  // 第三方 C 头（GMF 允许）：decodeHdr 的 FreeImage API 契约面
export module image_codec;

import rlstd;

namespace RGL {

/**
 * 解码后的 8 位图（恒 RGBA8、行序 top-down，与旧 stb_image 语义一致）：纹理解码的单一契约
 *（glb 内嵌图 / env cubemap 面 / 文件图共用）。
 */
export struct DecodedImage {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<uint8_t> rgba;  // width×height×4，行距 = width×4，行序 top-down
};

/**
 * 从内存字节解码图片（magic 嗅探定格式）：PNG = `89 50 4E 47` 前缀 → libpng 简单 API；
 * JPEG = `FF D8` 前缀 → libjpeg(-turbo) 内存源（JCS_EXT_RGBA，alpha 补 255 对齐 stb）。
 * 灰度/RGB 自动扩通道（无 alpha 图补 alpha=255，与 stb 对齐）、16 位转 8 位、调色板展开、
 * 隔行扫描全部由解码库处理；输出恒 RGBA8 top-down。
 * @param data 图片字节数据
 * @param size 字节数
 * @return 解码后的 RGBA8；格式不可识别 / 解码失败 → nullopt
 */
export [[nodiscard]] std::optional<DecodedImage> decodeRgba8(const uint8_t* data, size_t size);

/**
 * 解码图片文件（整读 → decodeRgba8）。
 * @param path 图片文件路径
 * @return 解码后的 RGBA8；读文件失败 / 解码失败 → nullopt
 */
export [[nodiscard]] std::optional<DecodedImage> decodeRgba8File(const std::string& path);

/**
 * 写 8 位 RGBA PNG（libpng 简单 API，行距 = width×4）。与旧 stbi_write_png 行为对齐：
 * 原值直写（不做 gamma 编码、不写 gAMA chunk）；文件字节与 stb 产物不必一致（编码器不同）。
 * @param path 输出文件路径
 * @param width 图宽
 * @param height 图高
 * @param rgba RGBA8 字节（width×height×4，行紧凑 top-down）
 * @return true = 写盘成功；false = 失败（打不开文件 / 编码错误）
 */
export bool writePng(const std::string& path, uint32_t width, uint32_t height, const uint8_t* rgba);

/**
 * 解码后的 HDR 图（Radiance .hdr）：3 通道 float、行序 top-down（FreeImage 的 HDR 插件
 * 把文件第 y 行写入 scanline height−1−y，读回时反序还原）。
 */
export struct DecodedImageF {
	uint32_t width = 0;
	uint32_t height = 0;
	std::vector<float> rgb;  // width×height×3，行距 = width×3，行序 top-down
};

/**
 * 解码 Radiance .hdr（FreeImage FIF_HDR，像素 = FIRGBF 96 位 RGB float）。
 * FreeImage 生命周期 = 函数内静态守卫（首调 Initialise、进程退出 DeInitialise，
 * magic static 线程安全）。
 * @param path .hdr 文件路径
 * @return 解码后的 RGB float；加载失败 / 像素类型非 FIT_RGBF → nullopt
 */
export [[nodiscard]] std::optional<DecodedImageF> decodeHdr(const std::string& path);

}  // namespace RGL
