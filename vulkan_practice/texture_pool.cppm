module;
#include <stb_image.h>
#include <fmt/format.h>
export module texture_pool;

import rlstd;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import asset_table;
import texture_upload;
import gltf_loader;
import model_types_generated;

namespace RGL {

// 纹理公共 API（启动期一次性）：纹理解码（stb_image）+ 白纹理契约 + 模型纹理批量上传 +
// env cubemap 文件加载。2026-09-02 拆为薄壳：GPU 上传核心（uploadTexture/uploadCubeMap，
// linear staging → optimal 拷贝 + mipmap 链 blit，~530 行）在 texture_upload 模块；
// 本模块只管"解码/校验/调用序契约/去重"。
//
// 批次 1 重构（2026-08-28）：原 TexturePool 类持有资产 + 描述符存储（main 局部对象 = UAF
// 根因：块尾析构第 0 帧前释放纹理 BO，docs/archive/docs/archive/heap_texture_research.md §12.6）。现为无状态
// 工具函数，注册产物（TextureAsset）落 Resources::AssetTable（初始化层 owner，所有权规则
// §12.7 ① 在此首次落地）；共享 sampler 同步移入 Resources::ensureSharedSampler。


// glb 内嵌图（buffer view byte span）→ 纹理资产 id；解码强制 RGBA8 4 通道
export [[nodiscard]] std::expected<uint32_t, Error> loadEmbedded(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* data, size_t size, const char* name) {
	int w = 0, h = 0, ch = 0;
	uint8_t* rgba = stbi_load_from_memory(data, static_cast<int>(size), &w, &h, &ch, 4);
	if (!rgba) {
		return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
			fmt::format("stb_image decode failed: {}", name)});
	}
	// DIAG（Phase 3 4K 黑屏排查 2026-08-27）：环境变量驱动的实验开关（免重编译跑矩阵）：
	//   TEXTURE_FORCE_SIZE=N  近邻缩放强制 N×N（测尺寸阈值：16MB vs 64MB）
	//   TEXTURE_FORCE_WHITE=1 全白填充（测"均匀内容不 fault"假设）
	//   TEXTURE_FORCE_UNORM=1 upload 强制 UNORM（测 sRGB 嫌疑）
	const char* forceSize = std::getenv("TEXTURE_FORCE_SIZE");
	const char* forceWhite = std::getenv("TEXTURE_FORCE_WHITE");
	const uint32_t fw = forceWhite ? static_cast<uint32_t>(w) : 0;
	const uint32_t fh = forceWhite ? static_cast<uint32_t>(h) : 0;
	const uint32_t target = forceSize ? static_cast<uint32_t>(std::stoul(forceSize)) : 0;
	if (target > 0 && (static_cast<uint32_t>(w) < target || static_cast<uint32_t>(h) < target)) {
		std::vector<uint8_t> up(static_cast<size_t>(target) * target * 4,
			forceWhite ? 255 : 0);
		for (uint32_t y = 0; y < target; ++y)
			for (uint32_t x = 0; x < target; ++x) {
				if (forceWhite) continue;
				const uint8_t* s = rgba + (static_cast<size_t>(y * h / target) * w + x * w / target) * 4;
				std::copy_n(s, 4, up.data() + static_cast<size_t>(y) * target * 4 + x * 4);
			}
		stbi_image_free(rgba);
		return uploadTexture(resources, device, up.data(), target, target, name);
	}
	if (forceWhite) {
		std::fill(rgba, rgba + static_cast<size_t>(w) * h * 4, 255);
	}
	auto result = uploadTexture(resources, device, rgba, static_cast<uint32_t>(w), static_cast<uint32_t>(h), name);
	stbi_image_free(rgba);
	return result;
}

// 1×1 白纹理（无纹理/无 uv primitive 的共用特例，D4）。
// 幂等（纹理首槽 = 白，调用序契约：先于任何图片纹理取）。
// 布局契约断言（批次 1 新增，Phase 4 批次 2 改按模型数）：slot = reserved +
// HeapSlotModelVertexBase + 模型顶点槽数（拼接 buffer = 每模型一个 slot，getModelSlotCount）
//（违例 = 契约漂移报错——原靠"别提前取白"纪律，2026-08-28 排查踩坑：白纹理抢 slot 2
// → model.vertex[0] drift 抛错）
export uint32_t ensureWhiteTexture(RGL::Resources& resources, RGL::DeviceContext& device) {
	auto& assets = resources.getAssetTable();
	if (assets.textureSize() > 0)
		return 0;  // 首个纹理资产 = 白（调用序契约）
	const uint8_t rgba[4] = {255, 255, 255, 255};
	auto result = uploadTexture(resources, device, rgba, 1, 1, "texture.white");
	if (!result)
		throw std::runtime_error(fmt::format("white texture: {}", result.error().describe()));
	const uint32_t expected = resources.getDescriptorHeap().getReservedSlots() +
		HeapSlotModelVertexBase + assets.getModelSlotCount();
	const uint32_t slot = assets.getTexture(*result).heapSlot;
	if (slot != expected)
		throw std::runtime_error(fmt::format(
			"heap slot layout drift: texture.white (slot {} != expected {})", slot, expected));
	return *result;
}

// 模型纹理上传（批次 1：main.cpp 循环迁入）：模型顶点槽分配完（importer）后调用——
// 先取白纹理，再按 primitive 的 baseColorTexture 加载（image 下标去重，同图不重复上传）；
// 无纹理/无 uv → 白纹理。返回纹理资产 id 列表（与 model.primitives 下标对齐，
// 供管线烘焙 / registerScene 材质注册消费）。
export [[nodiscard]] std::expected<std::vector<uint32_t>, Error> uploadModelTextures(
	RGL::Resources& resources, RGL::DeviceContext& device, const RGL::GltfModel& model) {
	const uint32_t whiteId = ensureWhiteTexture(resources, device);
	std::vector<uint32_t> primitiveTextureIds(model.primitives.size(), whiteId);
	std::map<int, uint32_t> imageTextureCache;  // image 下标 → 纹理资产 id（去重）
	for (size_t i = 0; i < model.primitives.size(); ++i) {
		const int imageIndex = model.primitives[i].baseColorTextureIndex;
		if (imageIndex < 0 || !model.primitives[i].hasUv)
			continue;
		auto cached = imageTextureCache.find(imageIndex);
		if (cached != imageTextureCache.end()) {
			primitiveTextureIds[i] = cached->second;
			continue;
		}
		const auto& image = model.images[imageIndex];
		if (image.data.empty()) {
			auto* logger = RLLogger::getInstance();
			if (logger)
				logger->warn("纹理: image {} 无内嵌数据（多 buffer .gltf 不支持），primitive {} 用白纹理",
					imageIndex, i);
			continue;
		}
		auto textureResult = loadEmbedded(resources, device,
			image.data.data(), image.data.size(),
			fmt::format("texture.image.{}", imageIndex).c_str());
		if (!textureResult)
			return std::unexpected(std::move(textureResult).error());
		primitiveTextureIds[i] = *textureResult;
		imageTextureCache[imageIndex] = *textureResult;
	}
	auto* logger = RLLogger::getInstance();
	if (logger) {
		const uint32_t textureCount = resources.getAssetTable().textureSize();
		uint32_t texturedPrimitives = 0;
		for (uint32_t id : primitiveTextureIds)
			if (id != whiteId) texturedPrimitives++;
		logger->info("textures: {} 资产（白 + {} 图片），{} / {} primitive 带纹理",
			textureCount, textureCount - 1, texturedPrimitives, model.primitives.size());
	}
	return primitiveTextureIds;
}

// 环境 cubemap 加载（Phase 5.5）：6 个图片文件路径（已按 Vulkan 层序 0-5 排列）→ stb 解码
// RGBA8 + 校验（方形/2 的幂/六面同尺寸）→ uploadCubeMap。面文件顺序权威记录 =
// docs/archive/docs/archive/phase5_5_plan.md 素材定案表（nightSky: 1,3,5,6,2,4；rainbow 方案 B: ft,bk,up,dn,rt,lf）。
export [[nodiscard]] std::expected<void, Error> loadEnvCubeMapFiles(RGL::Resources& resources,
	RGL::DeviceContext& device, const std::array<std::string, 6>& paths, const char* name);

[[nodiscard]] std::expected<void, Error> loadEnvCubeMapFiles(RGL::Resources& resources,
	RGL::DeviceContext& device, const std::array<std::string, 6>& paths, const char* name) {
	std::array<std::vector<uint8_t>, 6> pixels;
	uint32_t size = 0;
	for (uint32_t face = 0; face < 6; ++face) {
		int w = 0, h = 0, ch = 0;
		uint8_t* rgba = stbi_load(paths[face].c_str(), &w, &h, &ch, 4);
		if (!rgba)
			return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
				fmt::format("stb_image decode failed: {}", paths[face])});
		const bool squarePow2 = w == h && w > 0 && (w & (w - 1)) == 0;
		if (!squarePow2 || (size != 0 && w != static_cast<int>(size))) {
			stbi_image_free(rgba);
			return std::unexpected(Error{ErrorCode::TextureDecodeFailed,
				fmt::format("{}: face {} ({}x{}) 须为方形 2 的幂且六面同尺寸", name, face, w, h)});
		}
		size = static_cast<uint32_t>(w);
		pixels[face].assign(rgba, rgba + static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
		stbi_image_free(rgba);
	}
	std::array<const uint8_t*, 6> ptrs;
	for (uint32_t face = 0; face < 6; ++face) ptrs[face] = pixels[face].data();
	return uploadCubeMap(resources, device, ptrs, size, name);
}

}  // namespace RGL
