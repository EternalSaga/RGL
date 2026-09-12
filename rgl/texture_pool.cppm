module;
#include <fmt/format.h>
#include <chrono>             // env hdr 重投影耗时日志（GMF 例外清单成员）
#include <glm/glm.hpp>        // 重投影向量/角度数学（clamp/mix 核心面）
#include <glm/gtc/packing.hpp>  // packHalf2x16（RGBA16F 半精度打包）
#include <glm/gtc/type_ptr.hpp>  // make_vec3（源图 float 指针 → vec3）
export module texture_pool;

import rlstd;
import rllogger;
import image_codec;  // 图片解码（PNG/JPEG → RGBA8，替代旧 stb_image）
import diagnostics;  // texture_force_* 诊断单例（configs/diagnostics.lua）
import vkerror;
import vk_device;
import vk_resources;
import asset_table;
import texture_upload;
import gltf_loader;
import model_types_generated;

namespace RGL {

// 纹理公共 API（启动期一次性）：纹理解码（image_codec：libpng/libjpeg，2026-09-11 替代
// stb_image）+ 白纹理契约 + 模型纹理批量上传 + env cubemap 文件加载。2026-09-02 拆为薄壳：
// GPU 上传核心（uploadTexture/uploadCubeMap，linear staging → optimal 拷贝 + mipmap 链
// blit，~530 行）在 texture_upload 模块；本模块只管"解码/校验/调用序契约/去重"。
//
// 批次 1 重构（2026-08-28）：原 TexturePool 类持有资产 + 描述符存储（main 局部对象 = UAF
// 根因：块尾析构第 0 帧前释放纹理 BO，docs/archive/heap_texture_research.md §12.6）。现为无状态
// 工具函数，注册产物（TextureAsset）落 Resources::AssetTable（初始化层 owner，所有权规则
// §12.7 ① 在此首次落地）；共享 sampler 同步移入 Resources::ensureSharedSampler。


/**
 * glb 内嵌图（buffer view byte span）→ 纹理资产 id；解码强制 RGBA8 4 通道（image_codec，
 * 替代旧 stb_image）。
 * DIAG（configs/diagnostics.lua texture_force_*）：texture_force_size=N 近邻缩放强制
 * N×N（仅原图任一边小于 N 时上采，测尺寸阈值）、texture_force_white 全白填充
 *（测"均匀内容不 fault"假设）。
 * @param resources 资源层（VMA 分配器 + 资产表 + slot 分配器 + descriptor heap）
 * @param device 设备层（格式查询 / 命令池 / 队列 / fence）
 * @param data 图片字节数据（glb buffer view span）
 * @param size 数据字节数
 * @param linear 线性数据纹理（normal/ORM，UNORM 上传禁 sRGB 解码）
 * @param name 资产 / slot / 日志名
 * @return 纹理资产 id；image_codec 解码失败 / 上传失败 → Error
 */
export [[nodiscard]] std::expected<uint32_t, Error> loadEmbedded(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* data, size_t size, bool linear,
	const char* name) {
	auto image = decodeRgba8(data, size);
	if (!image) {
		return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
			fmt::format("image_codec decode failed: {}", name)});
	}
	const uint32_t w = image->width;
	const uint32_t h = image->height;
	uint8_t* rgba = image->rgba.data();
	// DIAG（configs/diagnostics.lua texture_force_*，原 TEXTURE_FORCE_* env 2026-09-08 转配置）：
	//   texture_force_size=N   近邻缩放强制 N×N（测尺寸阈值）
	//   texture_force_white    全白填充（测"均匀内容不 fault"假设）
	const uint32_t target = diagnostics().textureForceSize;
	const bool forceWhite = diagnostics().textureForceWhite;
	if (target > 0 && (w < target || h < target)) {
		std::vector<uint8_t> up(static_cast<size_t>(target) * target * 4,
			forceWhite ? 255 : 0);
		for (uint32_t y = 0; y < target; ++y)
			for (uint32_t x = 0; x < target; ++x) {
				if (forceWhite) continue;
				const uint8_t* s = rgba + (static_cast<size_t>(y * h / target) * w + x * w / target) * 4;
				std::copy_n(s, 4, up.data() + static_cast<size_t>(y) * target * 4 + x * 4);
			}
		return uploadTexture(resources, device, up.data(), target, target, linear, name);
	}
	if (forceWhite) {
		std::fill(rgba, rgba + static_cast<size_t>(w) * h * 4, 255);
	}
	return uploadTexture(resources, device, rgba, w, h, linear, name);
}

/**
 * 1×1 白纹理（无纹理/无 uv primitive 的共用特例，D4）。
 * 幂等（纹理首槽 = 白，调用序契约：先于任何图片纹理取；已存在纹理资产 → 直接返回 0）。
 * 布局契约断言（批次 1 新增，Phase 4 批次 2 改按模型数，架构债务 1 方案 B 改按几何槽数）：
 * slot = reserved + HeapSlotGeometryBase + 几何槽数（内建几何顶点/索引槽 + 模型每 16MB
 * 拼接块（kVertexChunkBytes）一个槽，getGeometrySlotCount）
 *（违例 = 契约漂移报错——原靠"别提前取白"纪律，2026-08-28 排查踩坑：白纹理抢 slot 2
 * → model.vertex[0] drift 抛错）。
 * @param resources 资源层（资产表 + slot 分配器 + descriptor heap + VMA 分配器）
 * @param device 设备层
 * @return 白纹理资产 id（= 0，纹理首槽）
 * @note 布局漂移抛 runtime_error（"heap slot layout drift"）
 */
export uint32_t ensureWhiteTexture(RGL::Resources& resources, RGL::DeviceContext& device) {
	auto& assets = resources.getAssetTable();
	if (assets.textureSize() > 0)
		return 0;  // 首个纹理资产 = 白（调用序契约）
	const uint8_t rgba[4] = {255, 255, 255, 255};
	// 白纹理走 sRGB 路：(1,1,1,1) 是 sRGB 解码不动点（采样后仍 1），base/orm 回落共用
	auto result = uploadTexture(resources, device, rgba, 1, 1, false, "texture.white");
	if (!result)
		throw std::runtime_error(fmt::format("white texture: {}", result.error().describe()));
	const uint32_t expected = assets.expectedGeometrySlot(
		resources.getDescriptorHeap().getReservedSlots());
	const uint32_t slot = assets.getTexture(*result).heapSlot;
	if (slot != expected)
		throw std::runtime_error(fmt::format(
			"heap slot layout drift: texture.white (slot {} != expected {})", slot, expected));
	return *result;
}

/**
 * 模型纹理上传产物（Phase 8 批次 ③a；③ 扩 emissive）：per-primitive 纹理资产 id 四元组，
 * 四向量均与 model.primitives 下标对齐（供管线烘焙 binding 1/6/7/8 + registerScene 材质注册消费）：
 *   base     = baseColorTexture image（无纹理/无 uv → 白纹理）
 *   normal   = material.normalTexture image（无 normal 贴图/无 uv → 中性法线纹理，
 *              (0.5,0.5,1,1) flip-G 解码不动点，shader 端零特判）
 *   orm      = pbrMetallicRoughness.metallicRoughnessTexture image（glTF ORM 约定
 *              R=Occlusion/G=Roughness/B=Metalness；无贴图/无 uv → 白纹理 orm=(1,1,1,1)，
 *              metallic/roughness 回落材质 factor）
 *   emissive = material.emissiveTexture image（Phase 8 ③：无贴图/无 uv → 白纹理；白×
 *              emissiveFactor，factor=0 时该项恒黑 = 不发光正确）
 */
export struct ModelTextureIds {
	std::vector<uint32_t> base;
	std::vector<uint32_t> normal;
	std::vector<uint32_t> orm;
	std::vector<uint32_t> emissive;  // Phase 8 ③：emissiveTexture（binding 8，白纹理回落）
};

/**
 * 1×1 中性法线纹理（Phase 8 批次 ③a：无 normal 贴图的 primitive 回落槽，RGBA8
 * (128,128,255,255) = (0.5,0.5,1,1)）：是 shader "flip-G 后 *2-1" 解码（normalmap.slang
 * decodeNormalDx）的不动点（flip 后仍 (0.5,0.5) → (0,0) → z=1），导数 TBN 退化时亦回落
 * 顶点法线——无 normal 贴图的 primitive 绑它 = shader 路径单一。
 * 幂等（调用序契约：紧随 ensureWhiteTexture——纹理第 2 槽；textureSize() > 1 = 已建，
 * 直接返回 1）。
 * @param resources 资源层（资产表 + slot 分配器 + descriptor heap + VMA 分配器）
 * @param device 设备层
 * @return 中性法线纹理资产 id（= 1，纹理第 2 槽）
 * @note 调用序违例（白纹理未先建）抛 runtime_error
 */
export uint32_t ensureNeutralNormalTexture(RGL::Resources& resources, RGL::DeviceContext& device) {
	auto& assets = resources.getAssetTable();
	if (assets.textureSize() > 1)
		return 1;  // 已建（幂等；调用序契约保证第 2 纹理槽 = 中性法线）
	if (assets.textureSize() != 1)
		throw std::runtime_error("heap slot layout drift: 中性法线纹理须紧随白纹理（调用序契约）");
	const uint8_t rgba[4] = {128, 128, 255, 255};
	// 中性法线 = 线性数据纹理（UNORM）：sRGB 解码会把 (0.5,0.5,1) 变成 (0.216,0.216,1)，
	// 破坏 flip-G 解码不动点契约
	auto result = uploadTexture(resources, device, rgba, 1, 1, true, "texture.normal.neutral");
	if (!result)
		throw std::runtime_error(fmt::format("neutral normal texture: {}", result.error().describe()));
	return *result;
}

/**
 * 单类纹理上传（Phase 8 批次 ③a 自 uploadModelTextures 拆出）：image 下标 → 纹理资产
 * id（cache 按 image 下标去重，同图不重复上传）；无 image（下标 -1）/ 无内嵌数据
 *（多 buffer .gltf 不支持，warn）→ fallback id。
 * @param resources 资源层
 * @param device 设备层
 * @param model 已解析 glTF 模型（images 内嵌数据）
 * @param imageIndex image 下标（-1 = 无此类贴图）
 * @param linear 线性数据纹理（normal/orm 传 true）
 * @param cache image 下标 → 纹理资产 id 去重缓存（base/normal/orm 各自独立）
 * @param fallback 无贴图回落 id（白 / 中性法线）
 * @param primitiveIndex 请求 primitive 下标（仅 warn 日志用）
 * @return 纹理资产 id（回落 = fallback）；解码/上传失败 → Error
 */
static std::expected<uint32_t, Error> uploadImageSlot(RGL::Resources& resources,
	RGL::DeviceContext& device, const RGL::GltfModel& model, int imageIndex, bool linear,
	std::map<int, uint32_t>& cache, uint32_t fallback, size_t primitiveIndex) {
	if (imageIndex < 0)
		return fallback;
	auto cached = cache.find(imageIndex);
	if (cached != cache.end())
		return cached->second;
	const auto& image = model.images[imageIndex];
	if (image.data.empty()) {
		auto* logger = RLLogger::getInstance();
		if (logger)
			logger->warn("纹理: image {} 无内嵌数据（多 buffer .gltf 不支持），primitive {} 用回落纹理",
				imageIndex, primitiveIndex);
		return fallback;
	}
	auto textureResult = loadEmbedded(resources, device,
		image.data.data(), image.data.size(), linear,
		fmt::format("texture.image.{}", imageIndex).c_str());
	if (!textureResult)
		return std::unexpected(std::move(textureResult).error());
	cache[imageIndex] = *textureResult;
	return *textureResult;
}

/**
 * 模型纹理上传（批次 1：main.cpp 循环迁入；Phase 8 批次 ③a 扩三元组 base/normal/orm；
 * ③ 再扩 emissive 成四元组）：模型顶点槽分配完（importer）后调用——先取白纹理 + 中性法线
 * 纹理，再按 primitive 的 baseColorTexture/normalTexture/metallicRoughnessTexture/
 * emissiveTexture 分别加载（四类各自独立 image 下标去重缓存，同图不重复上传）；无纹理/无 uv
 * → 各自回落（白 / 中性法线 / 白 / 白）。
 * 返回 ModelTextureIds（四向量与 model.primitives 下标对齐，供管线烘焙 binding 1/6/7/8 +
 * registerScene 材质注册消费）。
 * @param resources 资源层
 * @param device 设备层
 * @param model 已解析 glTF 模型（primitives 的四个 TextureIndex/hasUv + images 内嵌数据）
 * @return per-primitive 纹理资产 id 四元组（与 primitives 下标对齐；回落规则见
 *   ModelTextureIds 注释）；解码/上传失败 → Error
 * @note 多 buffer .gltf（image 无内嵌数据）不支持 → warn 日志 + 该 primitive 回落
 */
export [[nodiscard]] std::expected<ModelTextureIds, Error> uploadModelTextures(
	RGL::Resources& resources, RGL::DeviceContext& device, const RGL::GltfModel& model) {
	const uint32_t whiteId = ensureWhiteTexture(resources, device);
	const uint32_t neutralNormalId = ensureNeutralNormalTexture(resources, device);
	const size_t n = model.primitives.size();
	ModelTextureIds ids{std::vector<uint32_t>(n, whiteId),
		std::vector<uint32_t>(n, neutralNormalId), std::vector<uint32_t>(n, whiteId),
		std::vector<uint32_t>(n, whiteId)};  // emissive 回落白（白×factor，factor=0 = 黑）
	// image 下标 → 纹理资产 id（去重）；四类贴图各自独立缓存（同 image 下标空间 =
	// model.images，但 base/normal/orm/emissive 语义独立）
	std::map<int, uint32_t> baseCache, normalCache, ormCache, emissiveCache;
	for (size_t i = 0; i < n; ++i) {
		const auto& prim = model.primitives[i];
		if (!prim.hasUv)
			continue;  // 无 uv = 无纹理（base 白 / normal 中性 / orm 白 / emissive 白，初值即回落）
		// 格式语义（glTF）：baseColor/emissive = sRGB（硬件解码颜色），normal/ORM = 线性数据
		//（UNORM 上传，sRGB 解码会污染数值——批次 ③a G-buffer 实证）
		auto r = uploadImageSlot(resources, device, model, prim.baseColorTextureIndex,
			false, baseCache, whiteId, i);
		if (!r) return std::unexpected(std::move(r).error());
		ids.base[i] = *r;
		r = uploadImageSlot(resources, device, model, prim.normalTextureIndex,
			true, normalCache, neutralNormalId, i);
		if (!r) return std::unexpected(std::move(r).error());
		ids.normal[i] = *r;
		r = uploadImageSlot(resources, device, model, prim.metallicRoughnessTextureIndex,
			true, ormCache, whiteId, i);
		if (!r) return std::unexpected(std::move(r).error());
		ids.orm[i] = *r;
		// Phase 8 ③：emissive 是颜色 → sRGB 路（linear=false，同 baseColor），回落白纹理
		//（白×emissiveFactor，factor=0 时该项恒黑 = 不发光正确）
		r = uploadImageSlot(resources, device, model, prim.emissiveTextureIndex,
			false, emissiveCache, whiteId, i);
		if (!r) return std::unexpected(std::move(r).error());
		ids.emissive[i] = *r;
	}
	auto* logger = RLLogger::getInstance();
	if (logger) {
		const uint32_t textureCount = resources.getAssetTable().textureSize();
		uint32_t basePrims = 0, normalPrims = 0, ormPrims = 0, emissivePrims = 0;
		for (size_t i = 0; i < n; ++i) {
			if (ids.base[i] != whiteId) basePrims++;
			if (ids.normal[i] != neutralNormalId) normalPrims++;
			if (ids.orm[i] != whiteId) ormPrims++;
			if (ids.emissive[i] != whiteId) emissivePrims++;
		}
		logger->info("textures: {} 资产（白 + 中性 normal + {} 图片），{}/{} primitive 带 base / "
			"{} normal / {} orm / {} emissive",
			textureCount, textureCount - 2, basePrims, n, normalPrims, ormPrims, emissivePrims);
	}
	return ids;
}

/**
 * 环境 cubemap 加载（Phase 5.5）：6 个图片文件路径（已按 Vulkan 层序 0-5 排列）→
 * image_codec 解码 RGBA8 + 校验（方形/2 的幂/六面同尺寸）→ uploadCubeMap。面文件顺序权威记录
 * = docs/archive/phase5_5_plan.md 素材定案表（nightSky: 1,3,5,6,2,4；rainbow 方案 B:
 * ft,bk,up,dn,rt,lf）。
 * @param resources 资源层（VMA 分配器 + env 单例 + sky sampler）
 * @param device 设备层
 * @param paths 6 个面文件路径（Vulkan 层序 0-5 = +X,−X,+Y,−Y,+Z,−Z）
 * @param name 日志名
 * @return void（成功）；解码失败 / 非方形非 2 的幂 / 六面不同尺寸 → Error
 */
export [[nodiscard]] std::expected<void, Error> loadEnvCubeMapFiles(RGL::Resources& resources,
	RGL::DeviceContext& device, const std::array<std::string, 6>& paths, const char* name);

namespace {

/** 圆周率（M_PI 是 <cmath> 宏、不随 import 传播，本地常量替代）。 */
constexpr float kPi = 3.14159265358979323846f;

/**
 * 单面重投影：cubemap 面 (x,y)（top-down，s/t ∈ [−1,1]）→ 方向向量 → 等距柱状经纬度。
 * 面方向表 = Vulkan/D3D/GL 立方体贴图标准（+X:(1,−t,−s) −X:(−1,−t,s) +Y:(s,1,t)
 * −Y:(s,−1,−t) +Z:(s,−t,1) −Z:(−s,−t,−1)）；经度 = atan2(x, −z)（图像中心朝 −Z，
 * GL 惯例），纬度 = asin(y)。
 * @param face 面号 0-5（+X,−X,+Y,−Y,+Z,−Z）
 * @param x 面内列（0 = 左）
 * @param y 面内行（0 = 上）
 * @param size 面边长
 * @return 归一化方向（未归一——经纬度换算只需比例，asin 前 clamp）
 */
glm::vec3 cubeFaceDir(uint32_t face, uint32_t x, uint32_t y, uint32_t size) {
	const float s = 2.0f * (static_cast<float>(x) + 0.5f) / static_cast<float>(size) - 1.0f;
	const float t = 2.0f * (static_cast<float>(y) + 0.5f) / static_cast<float>(size) - 1.0f;
	switch (face) {
		case 0: return {1.0f, -t, -s};
		case 1: return {-1.0f, -t, s};
		case 2: return {s, 1.0f, t};
		case 3: return {s, -1.0f, -t};
		case 4: return {s, -t, 1.0f};
		default: return {-s, -t, -1.0f};
	}
}

/**
 * 等距柱状 float RGB 图双线性采样（经度环绕、纬度钳制，像素中心对齐）。
 * @param src 源图（top-down，W×H×3 float）
 * @param dir 采样方向（任意长度）
 * @return 采样 rgb（越界钳制，无 NaN 源假设）
 */
glm::vec3 sampleEquirect(const DecodedImageF& src, glm::vec3 dir) {
	dir = glm::normalize(dir);
	const float lon = std::atan2(dir.x, -dir.z);           // [−π, π]
	const float u = (lon + kPi) / (2.0f * kPi);
	const float v = 0.5f - std::asin(std::clamp(dir.y, -1.0f, 1.0f)) / kPi;
	const float W = static_cast<float>(src.width);
	const float H = static_cast<float>(src.height);
	const float cx = u * W - 0.5f;
	const float cy = v * H - 0.5f;
	const float fx = cx - std::floor(cx);
	const float fy = cy - std::floor(cy);
	const auto wrap = [W](float c) { return static_cast<uint32_t>(std::fmod(std::fmod(c, W) + W, W)); };
	const auto rowY = [H](float c) {
		return static_cast<uint32_t>(std::clamp(std::floor(c), 0.0f, H - 1.0f));
	};
	const uint32_t x0 = wrap(cx);
	const uint32_t x1 = wrap(cx + 1.0f);
	const uint32_t y0 = rowY(cy);
	const uint32_t y1 = rowY(cy + 1.0f);
	const auto at = [&](uint32_t x, uint32_t y) {
		return glm::make_vec3(&src.rgb[(static_cast<size_t>(y) * src.width + x) * 3]);
	};
	return glm::mix(glm::mix(at(x0, y0), at(x1, y0), fx),
		glm::mix(at(x0, y1), at(x1, y1), fx), fy);
}

/** 球谐 9 基（real SH，归一化常数烘焙进基函数——与 scene.slang irradiance 求值逐式相同，
 * 两端必须一致）。dir 须归一化；输出 9 项 Y00..Y22。 */
inline std::array<float, 9> shBasis(glm::vec3 d) {
	return {
		0.282095f,
		0.488603f * d.y,
		0.488603f * d.z,
		0.488603f * d.x,
		1.092548f * d.x * d.y,
		1.092548f * d.y * d.z,
		0.315392f * (3.0f * d.z * d.z - 1.0f),
		1.092548f * d.x * d.z,
		0.546274f * (d.x * d.x - d.y * d.y)};
}

/**
 * 等距柱状 radiance → 漫反射辐照度 SH9（Phase 8 批次 ④a）。逐 texel 按 equirect 立体角
 * 余弦无关投影 radiance  onto SH（权重 = dΩ = cosθ·dφ·dθ，θ=纬度）；返回的 9 系数是
 * **radiance 投影**——余弦卷积（辐照度）在 shader `irradiance(N)` 里做（π·Y0·L0 +
 * (2π/3)·Σ_l1 + (π/4)·Σ_l2，与 learnopengl RadianceToIrradiance 同款，CPU/GPU 逐式对齐）。
 * @param src decodeHdr 产物（linear float RGB）
 * @return 9 × vec4 radiance SH 系数（.xyz=系数、.w 留白；未做辐照度卷积）
 */
inline std::array<glm::vec4, 9> projectRadianceSH9(const DecodedImageF& src) {
	std::array<glm::vec3, 9> acc{};
	const float W = static_cast<float>(src.width);
	const float H = static_cast<float>(src.height);
	const float kPi = 3.14159265358979323846f;
	const float dOmega = (2.0f * kPi / W) * (kPi / H);  // 每 texel 立体角标称（θ 向 sin 另乘）
	for (uint32_t y = 0; y < src.height; ++y) {
		const float v = (static_cast<float>(y) + 0.5f) / H;
		const float lat = (0.5f - v) * kPi;              // [π/2, −π/2]
		const float cl = std::cos(lat);
		const float dy = std::sin(lat);
		const float weight = cl * dOmega;                // = sin(v·π)·dΩ（cosθ = sin(vπ)）
		for (uint32_t x = 0; x < src.width; ++x) {
			const float u = (static_cast<float>(x) + 0.5f) / W;
			const float lon = u * 2.0f * kPi - kPi;      // [−π, π]
			const glm::vec3 dir{cl * std::sin(lon), dy, -cl * std::cos(lon)};
			const glm::vec3 rgb = glm::make_vec3(&src.rgb[(static_cast<size_t>(y) * src.width + x) * 3]);
			const auto basis = shBasis(dir);
			for (int i = 0; i < 9; ++i) acc[i] += rgb * basis[i] * weight;
		}
	}
	std::array<glm::vec4, 9> sh;
	for (int i = 0; i < 9; ++i) sh[i] = glm::vec4{acc[i], 0.0f};
	return sh;
}

/** PNG faces 回落路径的 SH 近似：6 面 RGBA8（sRGB 编码）→ 逐面 texel 中心方向投影，
 * 立体角按面内 [−1,1] 坐标的 cube 近似权重 1/(1+x²+y²)^{3/2}（fallback 精度够用）。 */
inline std::array<glm::vec4, 9> projectFacesSH9(const std::array<const uint8_t*, 6>& faces,
	uint32_t size) {
	std::array<glm::vec3, 9> acc{};
	const float toLinear = 1.0f / 255.0f;
	for (uint32_t face = 0; face < 6; ++face)
		for (uint32_t y = 0; y < size; ++y)
			for (uint32_t x = 0; x < size; ++x) {
				const glm::vec3 dir = glm::normalize(cubeFaceDir(face, x, y, size));
				const size_t idx = (static_cast<size_t>(y) * size + x) * 4;
				const glm::vec3 rgb(static_cast<float>(faces[face][idx + 0]) * toLinear,
					static_cast<float>(faces[face][idx + 1]) * toLinear,
					static_cast<float>(faces[face][idx + 2]) * toLinear);
				const float r2 = 1.0f + dir.x * dir.x + dir.y * dir.y;  // 面内近似
				const float w = 1.0f / (r2 * std::sqrt(r2));
				const auto basis = shBasis(dir);
				for (int i = 0; i < 9; ++i) acc[i] += rgb * basis[i] * w;
			}
	std::array<glm::vec4, 9> sh;
	for (int i = 0; i < 9; ++i) sh[i] = glm::vec4{acc[i], 0.0f};
	return sh;
}

}  // namespace

/**
 * HDR 环境加载（Phase 8 批次 ②b）：Radiance .hdr（等距柱状 float RGB，decodeHdr 产物）→
 * CPU 重投影 6 面 RGBA16F（面边长 faceSize，双线性）→ uploadCubeMap16F。启动期一次性
 * （2048²×6 texel 双线性 ≈ 秒级）。源非等距柱状（W ≠ 2H）也按 u/v ∈ [0,1] 全图映射处理。
 * @param resources 资源层（VMA 分配器 + env 单例 + sky sampler）
 * @param device 设备层
 * @param hdrPath .hdr 文件路径（CWD 相对，同 configs 惯例）
 * @param faceSize 重投影面边长（2 的幂）
 * @param name 日志名
 * @return void（成功）；解码失败 / 上传失败 → Error
 */
export [[nodiscard]] std::expected<void, Error> loadEnvCubeMapHdr(RGL::Resources& resources,
	RGL::DeviceContext& device, const std::string& hdrPath, uint32_t faceSize,
	const char* name) {
	auto* logger = RLLogger::getInstance();
	const auto t0 = std::chrono::steady_clock::now();
	auto src = decodeHdr(hdrPath);
	if (!src)
		return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
			fmt::format("hdr decode failed: {}", hdrPath)});
	// Phase 8 ④a：等距柱状 radiance → 漫反射辐照度 SH9（存 Resources，renderListSystem 每帧
	// 拷进 scene data 尾区 irradiance(N) 消费）。
	resources.setEnvIrradianceSH(projectRadianceSH9(*src));
	if (const auto* env = resources.getEnvTexture(); env && env->width != faceSize) {
		// 理论不可达（env 单例启动期只建一次）——防御性提示，resize 留后
		if (logger)
			logger->warn("env: {}² 与已入库 env {}² 不一致，以后者为准", faceSize, env->width);
	}
	std::array<std::vector<uint8_t>, 6> faces;
	for (uint32_t face = 0; face < 6; ++face) {
		faces[face].resize(static_cast<size_t>(faceSize) * faceSize * 8);
		auto* dst = reinterpret_cast<glm::u32vec2*>(faces[face].data());
		for (uint32_t y = 0; y < faceSize; ++y)
			for (uint32_t x = 0; x < faceSize; ++x) {
				const glm::vec3 rgb = sampleEquirect(*src, cubeFaceDir(face, x, y, faceSize));
				*dst++ = glm::u32vec2{glm::packHalf2x16({rgb.x, rgb.y}),
					glm::packHalf2x16({rgb.z, 1.0f})};
			}
	}
	std::array<const uint8_t*, 6> ptrs;
	for (uint32_t face = 0; face < 6; ++face) ptrs[face] = faces[face].data();
	auto result = uploadCubeMap16F(resources, device, ptrs, faceSize, name);
	if (result && logger)
		logger->info("env hdr: {} ({}) → {}²×6 RGBA16F 重投影+上传 {:.2f}s", hdrPath,
			src->width, faceSize,
			std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count());
	return result;
}

/** 定义（契约见上方声明）：6 面 image_codec 解码 → 逐面方形 2 的幂 + 同尺寸校验（违例立即
 * 返回）→ uploadCubeMap（mipmap 链 + env 单例入库，见 texture_upload）。 */
[[nodiscard]] std::expected<void, Error> loadEnvCubeMapFiles(RGL::Resources& resources,
	RGL::DeviceContext& device, const std::array<std::string, 6>& paths, const char* name) {
	std::array<std::vector<uint8_t>, 6> pixels;
	uint32_t size = 0;
	for (uint32_t face = 0; face < 6; ++face) {
		auto image = decodeRgba8File(paths[face]);
		if (!image)
			return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
				fmt::format("image_codec decode failed: {}", paths[face])});
		const uint32_t w = image->width;
		const uint32_t h = image->height;
		const bool squarePow2 = w == h && w > 0 && (w & (w - 1)) == 0;
		if (!squarePow2 || (size != 0 && w != size)) {
			return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
				fmt::format("{}: face {} ({}x{}) 须为方形 2 的幂且六面同尺寸", name, face, w, h)});
		}
		size = w;
		pixels[face] = std::move(image->rgba);
	}
	std::array<const uint8_t*, 6> ptrs;
	for (uint32_t face = 0; face < 6; ++face) ptrs[face] = pixels[face].data();
	resources.setEnvIrradianceSH(projectFacesSH9(ptrs, size));  // Phase 8 ④a：回落面近似 SH
	return uploadCubeMap(resources, device, ptrs, size, name);
}

}  // namespace RGL
