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
 * 解析器键轨迹（`DiagnosticsKeyTrace` 同形，债-11：docs/archive/gate_lint_plan.md §3-B 的
 * 未知键 lint + 覆盖率护栏回灌 camera.lua）。键名书写与 Lua 路径一致，子表字段用
 * `表名.字段`（如 `keys.forward`）。本 loader 没有门回显行 ⟹ 判据表叫 KnownKeys 而非
 * EchoKeys：与 kGateEcho 同角色（未知键 lint 的判据表 + 覆盖率断言的输入），只是无回显输出。
 */
export struct CameraKeyTrace {
	std::vector<std::string> read;     ///< 解析器读过的键（解析器总是全量问一遍 = 与文件内容无关）
	std::vector<std::string> present;  ///< 文件里实际出现的键名：顶层 + 固定字段块下钻一层的 `块.子键`（债-129 第三刀；开放 map 只记表名）
};

/**
 * 键表的全部键名（与 `CameraKeyTrace::read` 同拼写）——单测据此做「读过的键集合 ==
 * 键表键集合」的双向覆盖率断言（解析器读了而表不列 = 表撒谎，表列了而解析器不读 =
 * 陈旧键）；表本体在实现单元（不把 fmt 拉进本模块接口）。
 * @return 键名列表（顺序 = 键表顺序）
 */
export std::vector<std::string> cameraConfigKnownKeys();

/**
 * 从 Lua 配置（configs/camera.lua）初始化相机：字段可选，缺省回落默认值；
 * fovDegrees 为角度，加载时转弧度；keys 字段可选，缺省 WASD，
 * 未知键名 → 报错退出。llm_debug/llm_shot 不在本模块（2026-09-08 迁至 runtime_config）。
 * 文件缺失 → 返回默认 CameraConfig + warn；脚本/类型错误 → std::unexpected。
 * 配置加载口径（其余 4 份 configs 下的 lua 同此，只在这里写全）：禁 dofile/loadfile/package/require，
 * 与 tools/schema_loader 一致。🔴 它买的是**被加载文件自足 + 是纯数据**——工具的单文件文本补丁与
 * 「改了没生效要响亮失败」那一类护栏靠这条前提；**它不是安全边界**（`base` 里的 `load`/`pcall`
 * 没禁 ⟹ chunk 内任意计算本来就能做，真正关掉的只有文件与模块可达性）。要组合走**生成器**
 * （tools/ 铺目录），不要在 Lua 里 require → 裁决全文 `docs/archive/配置正交性_plan.md` §3.7。
 * @param path 配置文件路径
 * @param trace 可选出参：键轨迹（覆盖率护栏与未知键 lint 的输入；传 nullptr = 不记录）
 * @return 解析好的 CameraConfig；失败返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<CameraConfig, Error> loadCameraConfig(
	std::string_view path, CameraKeyTrace* trace = nullptr);

}  // namespace RGL
