export module gpu_canary;

import rlstd;
import rlvk;
import rllogger;
import vkerror;
import VmaRaii;
import vulkan_utils;
import vk_device;
import vk_resources;
import asset_table;  // AssetTable（白纹理回读，F 拆出：不再经内核透传）

namespace RGL {

/**
 * texel 回读金丝雀（GPU 侧回读金丝雀，本模块唯一设施）：把"时好时坏"的 GPU 侧 UAF
 *（ASan 结构性失明，docs/archive/heap_texture_research.md §12.7）变成"帧 N 必炸"——
 * 第 N 帧重读白纹理 texel (0,0) 比对已知值 255：纹理 BO 死了（freed/VA 复用）回读
 * 立即偏/fault。白纹理已知值 = 免启动基线回读；白纹理与图片纹理同层同寿命，它死 = 全死。
 * 实现（见 impl 单元）：① waitIdle 排空 in-flight 采样（诊断路径，帧 N 单次 GPU 停顿
 * 可接受）② 16B 常驻映射回读缓冲 ③ SHADER_READ_ONLY→TRANSFER_SRC 过渡 + 拷贝 texel
 * (0,0) ④ 提交 + fence 等待 + 比对（白 = 255/255/255/255，sRGB 不影响全白）；PASS/FAIL
 * 只打日志不 throw。重构批次 4 首落；2026-09-02 拆自 texture_pool——诊断逻辑与上传逻辑
 * 解耦，ecs 帧循环不再依赖整个纹理上传栈。
 * @param resources 资源层（资产表：白纹理 = 纹理 id 0 / 纹理首槽调用序契约）
 * @param device 设备层（waitIdle / 提交 / fence 等待 / graphics 队列）
 * @param cmdPool 命令池（一次性回读命令缓冲）
 * @param frame 帧号（门控 diagnostics.lua texel_canary=N，默认关；调用点 = renderSystem
 *   帧 N 的 drawFrame 前；日志用）
 * @note 无纹理资产（未加载模型）→ warn 后跳过
 */
export void texelCanaryCheck(RGL::Resources& resources, const RGL::AssetTable& assets,
	RGL::DeviceContext& device, const vk::raii::CommandPool& cmdPool, uint64_t frame);

}  // namespace RGL
