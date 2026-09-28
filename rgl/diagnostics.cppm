export module diagnostics;

import rlstd;
import vkerror;

namespace RGL {

/**
 * 诊断开关（configs/diagnostics.lua，全可选回落默认 = 全关）：散落在低层模块深处的
 * 调试旋钮，经**单例**承载（仿 RLLogger::getInstance，见 logger/）——避免给
 * uploadTexture/loadEmbedded/AppContext/Renderer 等一串签名加参数。main 启动最早期
 * loadDiagnosticsConfig → setDiagnostics 填充，此后只读。
 * 这些多为历史排查（4K 黑屏 UAF、可见窗 dmabuf fault、sRGB 嫌疑）留下的门，默认全关
 * 不影响正常路径；env 机制已于 2026-09-08 全面转配置文件（用户偏好：不用环境变量）。
 */
export struct DiagnosticsConfig {
	bool noVvl = false;                 // 关 VVL（descriptor heap 双写排查，vkinstance）
	bool noHdr = false;                 // 强制 SDR surface 格式（HDR 后缓冲 fault 排查，vulkan_window）
	bool noPresent = false;             // 渲染但永不 present（可见窗 dmabuf fault 排查，vkrenderer）
	bool vtxCanary = false;             // 顶点上传回读金丝雀（model_importer uploadModelMeshes）
	uint64_t texelCanary = ~uint64_t{0};  // 帧 N 重读白纹理比对（ecs renderSystem；MAX = 关）
	int dumpGbuf = -1;                  // deferred 截图路径直读第 N 个 G-buffer 通道（llm_debug；-1 = 关）
	bool styleDist = false;             // per-primitive 风格分布日志（main entities 循环）
	bool texDist = false;               // per-primitive 纹理 slot 分布日志（main entities 循环）
	uint32_t textureForceSize = 0;      // 近邻缩放强制 N×N（texture 上传路径；0 = 关）
	bool textureForceWhite = false;     // 纹理全白填充（"均匀内容不 fault"假设）
	bool textureForceUnorm = false;     // upload 强制 UNORM（sRGB 嫌疑隔离）
	bool dgcStatic = false;             // RADV 静态 shader DGC 路（staticGraphicsIndirect 设备的 spike 门，默认关，见 docs/archive/radv_static_dgc_spike.md）
	uint32_t frameStats = 0;            // 帧时间统计：每 N 帧打一行 avg/min/max（0 = 关；headless 吞吐 ≈ GPU-bound）
	uint32_t profiler = 0;              // Phase 9 zone profiler 帧窗口 N（0 = 关；最近 N 帧 → 退出 dump trace+stats，docs/phase9_plan.md）
	bool imgui = false;                 // Phase 10 IMGUI 调试面板总开关（帧末 UI 段 + 输入拦截；headless 无窗天然 no-op，docs/archive/phase10_plan.md）
	bool configReload = false;          // Phase 10 批 10.3 配置热重载总开关（帧边界轮询源 configs/：scene/camera/log_level 的 T1 字段；imgui 面板开关本身不可热载=需重建后端，docs/archive/phase10_plan.md）
	bool shaderReload = false;          // Phase 10 批 10.4 shader 热重载 L1（帧边界 fork slangc 重编源 shaders/ + 同签名守卫换入；slangc/flags/源目录缺失降级关，docs/archive/phase10_plan.md）
	bool picking = false;               // Phase 10 批 10.5 GPU id 点选（编译图期装饰 object_id pass，D10 默认关 = 图逐位同；面板点中实体 → 参数上下文，docs/archive/phase10_plan.md）
	uint32_t pickingProbeFrame = 0;     // 脚本化点选探针（同 texel_canary headless 驱动方法论）：第 N 帧（1 起）自动 requestProbe(x,y) 一次；0 = 关
	uint32_t pickingProbeX = 0;         // 探针像素列（窗口/RT 坐标）
	uint32_t pickingProbeY = 0;         // 探针像素行
};

/**
 * 单例访问器（main setDiagnostics 后，深栈点 diagnostics() 只读）。
 * @return 进程级 DiagnosticsConfig 引用
 */
export DiagnosticsConfig& diagnostics();
/**
 * 填充诊断单例（main 启动期调用一次，此后只读）。
 * @param cfg 解析好的诊断配置
 */
export void setDiagnostics(const DiagnosticsConfig& cfg);

/**
 * 从 Lua 加载诊断配置（sol2 沙箱同 camera_config：缺文件 = 全默认 + warn；类型错误 = unexpected）。
 * 纯解析（不触碰单例），main 负责 setDiagnostics(*result)。
 * @param path 配置文件路径
 * @return 解析好的 DiagnosticsConfig；脚本/类型错误返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<DiagnosticsConfig, Error> loadDiagnosticsConfig(std::string_view path);

}  // namespace RGL
