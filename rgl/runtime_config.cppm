export module runtime_config;

import rlstd;
import vkerror;

namespace RGL {

/**
 * 一次性截图配置（2026-09-08 自 camera_config 迁入——llm_debug 与相机语义无关，
 * 属运行模式）。llm_debug = true/false 纯开关（缺省 false，交互模式零变化）；
 * llm_shot = {file, size, delay_frames} 常驻截图配置（全可选回落默认）。
 */
export struct LlmDebugConfig {
	bool enabled = false;
	uint32_t delayFrames = 0;  // 第 N 帧渲染完成后截图（0 = 首帧）
	std::string file;          // PNG 路径（相对 CWD）；空 = screenshots/llm_<时间戳>.png
	uint32_t width = 1280;     // llm_debug 模式的离屏隐藏窗尺寸
	uint32_t height = 720;
};

/**
 * 窗口 present 模式偏好（configs/runtime.lua 的 present_mode 键，2026-09-27 批 9.3
 * 用户追加需求：FIFO/eImmediate 双模式对比测量）。仅**窗口路径**生效；headless
 * surface 无 eImmediate、present 为 no-op，恒走 eFifo 回落（忽略此偏好）。
 */
export enum class PresentModePref {
	immediate,  // 默认 = 现状：硬要求 eImmediate（缺失抛错，Wayland compositor 管同步）
	fifo,       // eFifo（spec 必保模式；vsync 阻塞、帧率 = 刷新率——present_timing 测量对比用）
};

/**
 * 运行/窗口模式（configs/runtime.lua）：替代 RGL_HEADLESS / RGL_HEADLESS_SIZE /
 * RGL_WIN_SIZE / RGL_SHOW_AFTER 环境变量 + camera.lua 的 llm_debug/llm_shot 段。
 * 全字段可选，缺文件 = 全默认（交互窗 800x600、不截图）。
 * 注：驱动级 env（VK_DRIVER_FILES / prime offload）不在此列——
 * 由 Justfile run-* 配方注入，非应用配置。Mesa ≥26.2.3 起 RADV 无需 RADV_EXPERIMENTAL。
 */
export struct RuntimeConfig {
	bool headless = false;            // VK_EXT_headless_surface 脱 SDL/compositor（CI/agent 冒烟）
	uint32_t headlessWidth = 1280;
	uint32_t headlessHeight = 720;
	uint32_t winWidth = 800;          // 交互窗尺寸（headless / llm 模式下被各自尺寸覆盖）
	uint32_t winHeight = 600;
	int showAfter = -1;               // DIAG：隐藏窗启动、第 N 帧后显示（-1 = 立即显示）
	PresentModePref presentMode = PresentModePref::immediate;  // 窗口 present 模式（headless 忽略）
	LlmDebugConfig llm;
	// imgui 面板字体（Phase 10b 批 11b.1）：路径空 = 自动探测链（imgui_layer_impl
	// kUiFontCandidates，MiSans Light 首选）；字号 <=0 = 回落 18px。仅 imgui 门开时用。
	std::string uiFont;
	float uiFontSize = 18.0f;
};

/**
 * 从 Lua 加载运行配置。文件缺失 → 默认 + warn；脚本/类型错误 → unexpected。
 * 沙箱限制同 camera_config（禁用 dofile/loadfile/package/require）。
 * @param path 配置文件路径
 * @return 解析好的 RuntimeConfig；失败返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<RuntimeConfig, Error> loadRuntimeConfig(std::string_view path);

}  // namespace RGL
