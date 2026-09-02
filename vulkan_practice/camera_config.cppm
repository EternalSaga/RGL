export module camera_config;

import rlstd;
import rllogger;
import vkerror;
import camera;

namespace RGL {

// 一次性截图配置（configs/camera.lua）：
//   llm_debug = true/false 纯开关（缺省 false，交互模式零变化）——截图只改这一行；
//   llm_shot  = {file, size, delay_frames} 常驻截图配置（全可选回落默认）。
// 向后兼容：llm_debug 仍接受 table（旧格式 = 启用 + 细字段，覆盖 llm_shot）。
// 相机位置不走额外字段——agent 直接编辑 camera.lua 顶层的
// position/target/fovDegrees（loadCameraConfig 现有解析），无 presets 层。
// 2026-09-02 从 llm_debug 移入：配置是配置（camera_config 解析产出），执行是 llm_debug
//（captureFrame）——拆开后 camera_config/ecs 不再 import llm_debug（依赖图假边，
// llm_debug 拖 vk_renderer 全链，不该在 camera/ecs 的编译前置里）。
export struct LlmDebugConfig {
	bool enabled = false;
	uint32_t delayFrames = 0;  // 第 N 帧渲染完成后截图（0 = 首帧；为动态场景预留）
	std::string file;          // PNG 路径（相对 CWD）；空 = screenshots/llm_<时间戳>.png
	uint32_t width = 1280;    // llm_debug 模式的离屏窗口尺寸（隐藏窗，永不显示）
	uint32_t height = 720;
};

// 相机配置包（loadCameraConfig 返回值）：相机姿态 + llm_debug 一次性截图开关。
// llm_debug 的相机位置不走额外字段：agent 直接编辑本配置的 position/target/fovDegrees
// （顶层字段，现有解析），无 presets 层。
// 2026-09-02 拆自 camera：配置加载（sol2 沙箱 + 键名映射 + llm 段解析）与运行时
// cameraSystem 解耦——camera 模块不再依赖 llm_debug（原 import 仅为 LlmDebugConfig
// 类型，依赖图假边，编译串行链上的无谓环节）。
export struct CameraConfig {
	Camera camera;
	RGL::LlmDebugConfig llm;
};

// 键名（configs/camera.lua 的 keys 字段值）→ SDL 键码：单字母/数字不区分大小写，
// 方向键用 Up/Down/Left/Right，另有 Space/Enter。无法识别 → nullopt（配置错误，调用方报错退出）
//（键名映射仅 loadCameraConfig 内部使用，定义随实现单元迁出）

// 从 Lua 配置初始化相机（字段可选，缺省回落默认值；fovDegrees 为角度，加载时转弧度；
// keys 字段可选，缺省 WASD，未知键名 → 报错退出；llm_debug 字段可选，false/缺省 = 关闭）
// 文件缺失 → 返回默认 CameraConfig + warn；脚本/类型错误 → std::unexpected
// 沙箱限制与 tools/schema_loader 一致：禁用 dofile/loadfile/package/require
export [[nodiscard]] std::expected<CameraConfig, Error> loadCameraConfig(std::string_view path);

}  // namespace RGL
