export module texture_upload;

import rlstd;
import vkerror;
import vk_device;
import vk_resources;

namespace RGL {

// 纹理 GPU 上传核心（启动期一次性路径）：linear staging（host-visible 填充）→ optimal
// 拷贝 + UNDEFINED→SHADER_READ_ONLY，graphics 队列一次性命令缓冲 + fence waitIdle
//（D7：启动期路径，不启用闲置 transfer queue）。2D 路径无 mipmap；cubemap 路径带
// mipmap 链（blit 降采样 NEAREST——sRGB 格式 blit LINEAR 违 VUID-vkCmdBlitImage-
// filter-00239）。2026-09-02 拆自 texture_pool：上传实现（~530 行）与公共 API
//（解码/白纹理/模型纹理/env 文件加载）解耦——texture_pool 是薄壳，本模块是 GPU 侧。

/**
 * RGBA8 像素 → GPU 纹理 + 资产记录（上传全流程；module-private 于 texture_pool 时代
 * 的实现，拆出后 export 供 texture_pool 薄壳调用）。流程：格式策略（sRGB 优先——
 * glTF baseColor 约定，硬件完成 sRGB→线性；不支持采样回落 UNORM (R4)；DIAG
 * texture_force_unorm 强制 UNORM）→ 2D optimal 图（单 mip，独占 VkDeviceMemory）+
 * linear staging（host-visible 常驻映射，rowPitch 驱动给出）逐行填充 → 一次性命令
 * 缓冲（staging UNDEFINED→SRC + dst UNDEFINED→TRANSFER_DST → 拷贝 → dst 迁
 * SHADER_READ_ONLY）提交 + fence 等待 → DIAG 四位置 texel 回读日志 → allocSlot +
 * SAMPLED_IMAGE 描述符写入（v341 指针链模式）→ 资产表 emplaceTexture。
 * @param resources 资源层（VMA 分配器 + 资产表 + 统一 slot 分配器 + descriptor heap）
 * @param device 设备层（格式属性查询 / 命令池 / 队列 / fence）
 * @param rgba CPU 侧 RGBA8 像素数据（w×h×4B）
 * @param w 宽度
 * @param h 高度
 * @param name 资产 / slot / 日志名
 * @return 纹理资产 id（纹理表下标 = textureSize()-1；白纹理 = 首个上传的纹理 = id 0，
 *   texture_pool::ensureWhiteTexture 调用序契约）；失败 Error（VMA 图 / 回读缓冲创建、
 *   submit / fence）
 */
export [[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* rgba, uint32_t w, uint32_t h, const char* name);

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
 * @param resources 资源层（VMA 分配器 + env 单例 + 统一 slot 分配器 + sky sampler）
 * @param device 设备层（格式属性查询 / 命令池 / 队列 / fence）
 * @param faces 6 面像素指针（各 size×size×4B，Vulkan 层序 0-5 = +X,−X,+Y,−Y,+Z,−Z）
 * @param size 面边长（必须 2 的幂）
 * @param name 日志名
 * @note mipmap 链 blit 降采样用 NEAREST——sRGB 格式 blit LINEAR 违 VUID-vkCmdBlitImage-
 *   filter-00239（已知近似：无 prefiltered / BRDF LUT，见 AGENTS.md Phase 5.5 近似清单）
 * @return void（成功）；失败 Error（size 非 2 幂 / VMA 图创建 / submit / fence）
 */
export [[nodiscard]] std::expected<void, Error> uploadCubeMap(RGL::Resources& resources,
	RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const char* name);



}  // namespace RGL
