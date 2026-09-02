export module gpu_canary;

import rlstd;
import rlvk;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;

namespace RGL {

// GPU 侧回读金丝雀：把"时好时坏"的 GPU 侧 UAF（ASan 结构性失明，docs/archive/heap_texture_research.md
// §12.7）变成"帧 N 必炸"。texelCanaryCheck = 白纹理已知值回读（重构批次 4 首落；
// 2026-09-02 拆自 texture_pool——诊断逻辑与上传逻辑解耦，ecs 帧循环不再依赖整个
// 纹理上传栈）。
//
// texel 回读金丝雀：第 N 帧重读白纹理 texel (0,0) 比对已知值 255——纹理 BO 死了
// （freed/VA 复用）回读立即偏/fault。白纹理已知值 = 免启动基线回读；白纹理与图片
// 纹理同层同寿命，它死 = 全死。
// 门控：RGL_TEXEL_CANARY=N（帧号，默认关）；调用点在 renderSystem 帧 N 的 drawFrame 前。
export void texelCanaryCheck(RGL::Resources& resources, RGL::DeviceContext& device,
	const vk::raii::CommandPool& cmdPool, uint64_t frame);

}  // namespace RGL
