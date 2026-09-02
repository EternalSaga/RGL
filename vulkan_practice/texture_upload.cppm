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

// RGBA8 像素 → GPU 纹理 + 资产记录（上传全流程；module-private 于 texture_pool 时代
// 的实现，拆出后 export 供 texture_pool 薄壳调用）
export [[nodiscard]] std::expected<uint32_t, Error> uploadTexture(RGL::Resources& resources,
	RGL::DeviceContext& device, const uint8_t* rgba, uint32_t w, uint32_t h, const char* name);

// Phase 5.5：6 份 RGBA8 面像素（已按 Vulkan 层序 0-5 = +X,−X,+Y,−Y,+Z,−Z）→ eCube image +
// mipmap 链 + eCube view + SAMPLED_IMAGE 描述符 → Resources env 单例 + sky sampler。
// size 必须 2 的幂（mip 链 = 1+log2(size) 级）。启动期路径，上传模式同 uploadTexture。
// 内建拷贝金丝雀：逐面回读 mip0 两 texel（角/中心）比对 CPU 源（帧 N 回读版 = gpu_canary
// 模块 texelCanaryCheck；UAF 教训——把"时好时坏"变"启动必炸"）；blit 降采样不做 CPU 比对。
export [[nodiscard]] std::expected<void, Error> uploadCubeMap(RGL::Resources& resources,
	RGL::DeviceContext& device, std::span<const uint8_t* const, 6> faces, uint32_t size,
	const char* name);



}  // namespace RGL
