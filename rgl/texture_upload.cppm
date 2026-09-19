export module texture_upload;

import rlstd;
import vkerror;
import vk_device;
import vk_resources;
import sampler_library;  // mip-gen 供给（uploadTexture UNORM 链，架构债务续账步骤 4a/4b）
import environment;  // env 单例入库 + sky sampler + GGX 预滤波供给（架构债务续账步骤 3）
import asset_table;  // AssetTable（纹理资产入库，F 拆出）

namespace RGL {

// 纹理 GPU 上传核心（启动期一次性路径）：linear staging（host-visible 填充）→ optimal
// 拷贝 + UNDEFINED→SHADER_READ_ONLY，graphics 队列一次性命令缓冲 + fence waitIdle
//（D7：启动期路径，不启用闲置 transfer queue）。2D 路径 Phase 8 ③b 起带 mip 链（全链
// 1+floor(log2(max(w,h)))，mip_gen 模块录进同一上传 CB——消除缩小采样无 mip 的走样
// speckle；双路径 = UNORM compute 链（eStorage）/ sRGB LINEAR blit 链（RADV SRGB 无
// STORAGE_IMAGE 能力，sRGB 图不能带 eStorage；spec 1.4 起 sRGB blit LINEAR 合法且
// sRGB-aware——旧 VUID-vkCmdBlitImage-filter-00239 已废除）；cubemap 路径带 blit mip
// 链（NEAREST——保守沿用旧规则，见 uploadCubeMap 注释）。2026-09-02 拆自
// texture_pool：上传实现（~530 行）与公共 API（解码/白纹理/模型纹理/env 文件加载）
// 解耦——texture_pool 是薄壳，本模块是 GPU 侧。

/**
 * RGBA8 像素 → GPU 纹理 + 资产记录（上传全流程；module-private 于 texture_pool 时代
 * 的实现，拆出后 export 供 texture_pool 薄壳调用）。流程：格式策略（sRGB 优先——
 * glTF baseColor 约定，硬件完成 sRGB→线性；不支持采样回落 UNORM (R4)；DIAG
 * texture_force_unorm 强制 UNORM）→ 2D optimal 图（全 mip 链 1+floor(log2(max(w,h)))，
 * Phase 8 ③b；独占 VkDeviceMemory；usage = sampled+transferSrc/Dst，UNORM 另加 eStorage
 * （compute 链 storage view；sRGB 不能带——RADV SRGB 无 STORAGE_IMAGE 能力）+ 全链 view
 * + linear staging（host-visible 常驻映射，rowPitch 驱动给出）逐行填充 → 一次性命令
 * 缓冲（staging UNDEFINED→SRC + mip0 UNDEFINED→TRANSFER_DST → 拷贝 → mip0 迁
 * SHADER_READ_ONLY → mipLevels>1 时 mip_gen 录 mip 链 level 1..L−1：UNORM = compute
 * 链 / sRGB = LINEAR blit 链）提交 + fence 等待 → allocSlot Texture + SAMPLED_IMAGE 描述符
 * 写入（v341 指针链模式，view 覆盖全 mip 链）→ 资产表 emplaceTexture（mipLevels 入库）。
 * 1×1 纹理（白/中性法线）= 单 mip，跳过 mip 生成。
 * @param resources 资源层（VMA 分配器 + 资产表 + 分用途 slot 分配器 + descriptor heap）
 * @param samplers 共享 sampler 库（UNORM compute mip 链供给 ensureMipGenReady）
 * @param device 设备层（格式属性查询 / 命令池 / 队列 / fence）
 * @param rgba CPU 侧 RGBA8 像素数据（w×h×4B）
 * @param w 宽度
 * @param h 高度
 * @param linear true = 线性数据纹理（eR8G8B8A8Unorm，采样无 sRGB 解码——glTF
 *   normal/metallicRoughness 等数据纹理必须线性，sRGB 传输函数会污染数值，
 *   Phase 8 批次 ③a G-buffer 实证）；false = sRGB 优先（glTF baseColor 约定）
 * @param name 资产 / slot / 日志名
 * @return 纹理资产 id（纹理表下标 = textureSize()-1；白纹理 = 首个上传的纹理 = id 0，
 *   texture_pool::ensureWhiteTexture 调用序契约）；失败 Error（VMA 图 / 回读缓冲创建、
 *   submit / fence）
 */
export [[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
	RGL::SamplerLibrary& samplers, RGL::AssetTable& assets, RGL::DeviceContext& device,
	const uint8_t* rgba, uint32_t w, uint32_t h, bool linear, const char* name);

/**
 * Phase 5.5：6 份 RGBA8 面像素（已按 Vulkan 层序 0-5 = +X,−X,+Y,−Y,+Z,−Z）→ eCube image
 *（e2D + arrayLayers=6 + eCubeCompatible，单图持 6 面）+ mipmap 链（blit 降采样）+
 * eCube view + SAMPLED_IMAGE 描述符 → Resources env 单例（**非** AssetTable textures——
 * env 绑场景不绑材质，不占纹理 id 空间，白纹理 id 0 / 首槽契约不受影响）+ sky sampler
 *（maxLod = mipLevels−1）。
 * size 必须 2 的幂（mip 链 = 1+log2(size) 级），否则 TextureDecodeFailed。启动期路径，
 * 上传模式同 uploadTexture（6 张独立 2D linear staging——linear tiling 三条硬限，见 impl；
 * 一次性命令缓冲 per-face copy + 逐 level blit 链）。
 * 内建拷贝金丝雀：逐面回读 mip0 两 texel（角 (0,0) / 中心）比对 CPU 源（违例只 error
 * 日志、不改返回值；帧 N 回读版 = gpu_canary 模块 texelCanaryCheck；UAF 教训——把
 * "时好时坏"变"启动必炸"）；blit 降采样不做 CPU 比对。
 * @param resources 资源层（VMA 分配器 + 统一 slot 分配器 + descriptor heap）
 * @param env 环境服务（env 单例入库 + sky sampler，架构债务续账步骤 3）
 * @param device 设备层（格式属性查询 / 命令池 / 队列 / fence）
 * @param faces 6 面像素指针（各 size×size×4B，Vulkan 层序 0-5 = +X,−X,+Y,−Y,+Z,−Z）
 * @param size 面边长（必须 2 的幂）
 * @param name 日志名
 * @note mipmap 链 blit 降采样用 NEAREST——sRGB 格式 blit LINEAR 违 VUID-vkCmdBlitImage-
 *   filter-00239（已知近似：无 prefiltered / BRDF LUT，见 AGENTS.md Phase 5.5 近似清单）
 * @return void（成功）；失败 Error（size 非 2 幂 / VMA 图创建 / submit / fence）
 */
export [[nodiscard]] std::expected<void, Error> uploadCubeMap(RGL::Resources& resources,
	RGL::Environment& env, RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces,
	uint32_t size, const char* name);

/**
 * Phase 8 批次 ②b：HDR 环境 cubemap 上传——**RGBA16_SFLOAT** 版 uploadCubeMap。
 * 面像素 = 6 份 size×size×8B（每 texel 4 half，R,G,B,A，top-down，Vulkan 层序 0-5）。
 * float 格式 mip 链 blit 可用 LINEAR（sRGB VUID-00239 限制不适用；设备无
 * SAMPLED_IMAGE_FILTER_LINEAR 能力时回落 NEAREST + 警告）；无采样能力 = Error
 *（不回落——HDR 语义无法用 8-bit 面字节替代）。金丝雀/入库同 8-bit 路径。
 * @param resources 资源层（VMA 分配器 + 统一 slot 分配器 + descriptor heap）
 * @param env 环境服务（env 单例入库 + sky sampler + GGX 预滤波供给，架构债务续账步骤 3）
 * @param device 设备层（格式属性查询 / 命令池 / 队列 / fence）
 * @param faces 6 面 half 像素指针（各 size×size×8B）
 * @param size 面边长（必须 2 的幂）
 * @param name 日志名
 * @return void（成功）；失败 Error（size 非 2 幂 / 格式无采样能力 / VMA / submit / fence）
 */
export [[nodiscard]] std::expected<void, Error> uploadCubeMap16F(RGL::Resources& resources,
	RGL::Environment& env, RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces,
	uint32_t size, const char* name);



}  // namespace RGL
