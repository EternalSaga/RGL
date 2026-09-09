export module camera_config;

import rlstd;
import rllogger;
import vkerror;
import camera;

namespace RGL {

/**
 * 相机配置（configs/camera.lua）：仅相机姿态（一个 Camera 值）。
 * 2026-09-08：llm_debug/llm_shot 段与 LlmDebugConfig 类型迁至 runtime_config
 *（截图属运行模式，与相机语义无关）；本模块回归纯相机。
 * 相机位置不走额外字段——agent 直接编辑 camera.lua 顶层的
 * position/target/fovDegrees（loadCameraConfig 现有解析），无 presets 层。
 */
export struct CameraConfig {
	Camera camera;
};

// 键名（configs/camera.lua 的 keys 字段值）→ SDL 键码：单字母/数字不区分大小写，
// 方向键用 Up/Down/Left/Right，另有 Space/Enter。无法识别 → nullopt（配置错误，调用方报错退出）
//（键名映射仅 loadCameraConfig 内部使用，定义随实现单元迁出）

/**
 * 从 Lua 配置（configs/camera.lua）初始化相机：字段可选，缺省回落默认值；
 * fovDegrees 为角度，加载时转弧度；keys 字段可选，缺省 WASD，
 * 未知键名 → 报错退出。llm_debug/llm_shot 不在本模块（2026-09-08 迁至 runtime_config）。
 * 文件缺失 → 返回默认 CameraConfig + warn；脚本/类型错误 → std::unexpected。
 * 沙箱限制与 tools/schema_loader 一致：禁用 dofile/loadfile/package/require。
 * @param path 配置文件路径
 * @return 解析好的 CameraConfig；失败返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<CameraConfig, Error> loadCameraConfig(std::string_view path);

}  // namespace RGL
