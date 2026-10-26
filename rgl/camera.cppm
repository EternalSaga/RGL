module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
export module camera;

import rlstd;
import rllogger;
import coordinate_rebase;  // 批 4 第二刀：toRelative 的点版重载（view 的 eye/target 减渲染原点）

namespace RGL {

/** 相机交互模式（config 加载期设定，运行期不变）：Trackball = 绕 target 公转；Fps = 自由飞行。 */
export enum class CameraMode {
	Trackball,
	Fps,
};

/**
 * 键位绑定（configs/camera.lua 的 keys 字段，sol2 沙箱 + 可选字段回落默认）：
 * 动作 → SDL 键码；默认 WASD。
 */
export struct KeyBindings {
	uint32_t forward = SDLK_W;
	uint32_t back = SDLK_S;
	uint32_t left = SDLK_A;
	uint32_t right = SDLK_D;
};

/**
 * 相机姿态：yaw/pitch 表示朝向（yaw=0, pitch=0 → forward=(0,0,-1)）。
 * trackball 模式下相机绕 target 公转（object 始终居屏中心）：拖拽旋转
 * position-target 偏移向量（与携带的 up 同步），view 矩阵经
 * lookAt(position, target, up) 保持朝向 target；yaw/pitch 拖拽中不更新，
 * 仅 config 加载期由初始 position→target 方向反推一次（见 camera_config）。
 * fps 模式下 target 不参与计算（自由飞行，forward/up 由 yaw/pitch 推导）。
 * @note 项目启用 GLM_FORCE_RADIANS，glm::radians() 是恒等函数，角度→弧度必须手动换算。
 */
export struct Camera {
	// ── 位置/注视点是**绝对层**（double，唯一真相源；相对坐标计划批 2 第一刀）──
	// 为什么是 double：位置这条数据将来要喂 origin rebasing（批 3 的「绝对 → 相对」换算），
	// 而 float32 存不下「绝对世界坐标 + 长时间 dt 逐帧累加」（债-146）。
	// 🔴 本刀**只换类型、不改任何数值路径**：所有交给 glm（lookAt / 拖拽 / 日志）的算术仍在
	// float32 做，在边界处显式 `glm::vec3(...)` 收窄（值本来就来自 float32，收窄是精确的）。
	// 落库形态（cameraView 产 float mat4）不变 ⟹ 交付截图逐位不变。
	// ⚠ 批 4 第二刀起 cameraView 的边界收窄变成「先减渲染原点再收窄」（门关 = 原点 0，
	// 仍是一次精确收窄，上面的逐位结论不变）。
	// 🔴 **上面那句「所有算术仍在 float32 做」已被债-151 的实测推翻，2026-10-26 改准**：
	// 「两个绝对层量各自先收窄、再相减」不是"精确收窄"，而是**用一个 0.0625 m 的格子去量一个
	// 几米长的差向量**（|p| = 1e6 处 float32 的 ULP = 0.0625 m）——视向角因此偏 0.20°，画面整体
	// 平移 ~5 px（门关门开同吃，因为收窄发生在配置读取期，那时渲染原点还不存在）。
	// ⟹ 现在的域约定是：**相减 / 归一化 / 落回 double 字段在 double 域**，收窄只发生在
	// 「差向量进 float 做旋转」和「角度落 float 字段」这两处**结果侧**（见各函数注释）。
	// 角度量（yaw / pitch）与方向量（up）本来就是 float，不变——它们不是绝对层位置。
	glm::dvec3 position{0.0, 0.0, 3.0};  ///< 世界位置（绝对层，double）
	glm::dvec3 target{0.0, 0.0, 0.0};    ///< 注视点（trackball 公转中心，绝对层，double）
	float yaw = 0.0f;
	float pitch = 0.0f;
	// trackball 携带的 up（basis 携带，同 legacy TrackballSystem）：随拖拽与 offset
	// 同步旋转，不反推 yaw/pitch → 极点无 atan2(0,0) 奇异、可翻过头顶。fps 模式不用它
	//（up 由 yaw/pitch 推导）。config 加载时按 world-up 对齐初始化。
	glm::vec3 up{0.0f, 1.0f, 0.0f};
	float fov = 45.0f * glm::pi<float>() / 180.0f;
	float nearPlane = 0.1f;
	float farPlane = 10.0f;
	CameraMode mode = CameraMode::Trackball;
	float fpsSpeed = 3.0f;
	float fpsSensitivity = 0.003f;
	float trackballSensitivity = 0.005f;
	float zoomSpeed = 0.03f;
	KeyBindings keys;  // 键位绑定（configs/camera.lua 的 keys 字段，E5.4：键位是数据不是硬编码）
	// ── 帧驱动 yaw（camera.lua `frame_yaw`，rad/帧；0 = 门关 = 逐位今天）──
	// 诊断激励门（消费方 = imgqa temporal-motion）：yaw = yawBase + frameYaw·帧号 的
	// **纯函数**（frameDrivenCameraYaw，先例 = scene_context 的 frameDrivenSpinAngle）——
	// 跨运行/跨进程可复现的相机运动，dt 累积随帧时长漂 = 对齐根本不存在
	// （tools/imgqa/temporal.cpp 头注释）。为什么必须是绕 up 轴定角 yaw：刚体相机运动
	// 的像素位移与深度无关（u′=(u+tanθ)/(1−u·tanθ)，只依赖屏位不依赖深度），
	// prevViewProj 重投影能精确对齐；平移 dolly 的位移 ∝1/深度，远处信号衰减。
	// ⚠ 仅 fps 模式生效（trackball 的 view 由 position/target 推导，yaw 是死字段）；
	//   门开时拖拽改朝向会被下一帧的纯函数值覆盖（诊断激励刻意压过交互，非 bug）。
	float frameYaw = 0.0f;       ///< rad/帧；≠0 = 门开
	float yawBase = 0.0f;        ///< 基准朝向（cameraSystem 自动重定基，见 applyFrameDrivenYaw）
	uint32_t yawFrameIndex = 0;  ///< cameraSystem 每帧 +1（先加后用 ⟹ 首帧偏转 = frameYaw）
};

/**
 * 帧驱动 yaw（门 `frame_yaw` 的判据本体；提纯函数的先例 = frameDrivenSpinAngle）。
 *
 * yaw = yawBase + yawPerFrame·frameIndex——无累积、无帧时长：同一 frameIndex 在任何
 * 运行给出同一朝向（跨进程 A/B 对齐的唯一前提）。float32 精度账同 frameDrivenSpinAngle：
 * 10⁴ 帧 × 3.5e-3 rad/帧 ≈ 35 rad，ULP ≈4e-6 rad ≈ 0.003px，远低于可测信号。
 * @param yawBase 基准朝向（rad）
 * @param yawPerFrame 每帧角增量（rad，可负 = 反向）
 * @param frameIndex 帧号（0 起；0 帧 = 基准）
 * @return 本帧 yaw（rad）
 */
export float frameDrivenCameraYaw(float yawBase, float yawPerFrame, uint32_t frameIndex) {
	return yawBase + yawPerFrame * static_cast<float>(frameIndex);
}

/**
 * `frameDrivenYawPin` 的返回值：一对要写进 `Camera` 的 (yawBase, yaw)。
 */
export struct YawPin {
	float yawBase;  ///< 写入 `Camera::yawBase`
	float yaw;      ///< 写入 `Camera::yaw`（= 本帧的纯函数期望值，见 `frameDrivenYawPin`）
};

/**
 * 把「第 pinnedFrame 帧**渲染出来**的 yaw」钉成 yawPinned（债-89：RPC 机位锚点）。
 *
 * 为什么需要它（客户端预补偿为什么必然失败）：`applyFrameDrivenYaw` 以「yaw ≠ 期望值」
 * 识别外部写入并重定基 ⟹ 写入后 yaw(帧) = 写入值 + 每帧角·(帧 − 落帧帧号)，而**落帧帧号
 * 由 HTTP 时序决定**。本机 headless ~6000fps、loopback 往返 ~1ms ⟹ 落帧抖动 ±36 帧量级
 * = 0.125px/帧 × 36 ≈ 4.5px，与要消除的噪声同量级——客户端无论怎么算补偿都补不掉自己
 * 请求的落帧不确定性。唯一正解 = 把补偿写成**帧号的函数**：给定目标帧直接解出 yawBase。
 *
 * 约定（与 `applyFrameDrivenYaw` 的「先加后用」逐字对齐，off-by-one 由单测钉死）：
 * 帧首钩子先 `++帧计数` 再跑 system 链，而 `applyFrameDrivenYaw` 先自增 `frameIndex`
 * 再取值 ⟹ 第 X 帧渲染用的 yaw = yawBase + yawPerFrame·(frameIndexNow + (X − 帧计数) + 1)。
 * 令它等于 yawPinned 即得 base；`yaw` 分量取本帧期望值，是为了让紧随其后的 cameraSystem
 * 认定「无外部写入」而**不重定基**——否则本针当场被抹掉。
 * @param yawPinned 第 pinnedFrame 帧要渲染出的 yaw（rad）
 * @param yawPerFrame 每帧角增量（rad，= `Camera::frameYaw`，门开时非零）
 * @param frameIndexNow `Camera::yawFrameIndex` 当前值（自增前）
 * @param engineFrameNow 引擎帧计数当前值（= RPC 回显的那个计数）
 * @param pinnedFrame 目标帧号（≥ engineFrameNow；更早的帧无从钉）
 * @return 要写入的 (yawBase, yaw)
 */
export YawPin frameDrivenYawPin(float yawPinned, float yawPerFrame, uint32_t frameIndexNow,
	uint32_t engineFrameNow, uint32_t pinnedFrame) {
	const uint32_t indexAtPinned = frameIndexNow + (pinnedFrame - engineFrameNow) + 1u;
	const float base = yawPinned - yawPerFrame * static_cast<float>(indexAtPinned);
	return { base, frameDrivenCameraYaw(base, yawPerFrame, frameIndexNow) };
}

/**
 * 输入事件（eventSystem 产生，cameraSystem 消费即清空）：
 * 键事件带 SDL 键码——键位映射不在 eventSystem，消费方按 Camera.keys 解释
 *（E5.4：输入是事件流不是共享 level-state 袋）；鼠标按键事件带 button + pressed，
 * level 状态由 cameraSystem 自管（CameraState），不经输入共享。
 */
export struct InputEvent {
	/** 事件种类（按类型取用字段：key / button+pressed / delta / amount，其余字段无意义）。 */
	enum class Type {
		KeyDown,
		KeyUp,
		MouseButton,
		MouseDelta,
		Wheel,
	};
	Type type = Type::KeyDown;
	uint32_t key = 0;      // SDL 键码（KeyDown/KeyUp）
	uint8_t button = 0;    // SDL 鼠标按键（MouseButton）
	bool pressed = false;  // MouseButton：true = 按下
	glm::vec2 delta{};     // MouseDelta（xrel/yrel 增量）
	glm::vec2 pos{};       // MouseButton/MouseDelta：窗口内绝对坐标（Phase 10 点选/imgui 用；其余类型无意义）
	float amount = 0.0f;   // Wheel（y 增量）
};

/**
 * 输入事件流（registry context；eventSystem 只产事件，cameraSystem 消费即清空；
 * 退出/窗口状态不走事件，见 ecs.cppm 的 AppExit/WindowExtent）
 */
export struct InputEvents {
	std::deque<InputEvent> events;
};

/**
 * 相机内部状态（cameraSystem 独占：键/鼠标按键 level 状态，每帧由输入事件更新；
 * 消费方显式管理，不与其他 system 共享——E5.4）
 */
export struct CameraState {
	bool keyForward = false;
	bool keyBack = false;
	bool keyLeft = false;
	bool keyRight = false;
	bool mouseLeftDown = false;
	bool mouseRightDown = false;
};

/**
 * 相机前向（视线）方向：由 yaw/pitch 推导的世界空间单位向量。
 * @param camera 相机姿态（读 yaw/pitch）
 * @return (-sin yaw·cos pitch, sin pitch, -cos yaw·cos pitch)；yaw=0, pitch=0 时为 (0,0,-1)
 */
export glm::vec3 cameraForward(const Camera& camera) {
	return glm::vec3(-std::sin(camera.yaw) * std::cos(camera.pitch),
		std::sin(camera.pitch),
		-std::cos(camera.yaw) * std::cos(camera.pitch));
}

/**
 * 相机右向量：由 yaw 推导（无 pitch 分量）的世界空间单位向量。
 * @param camera 相机姿态（读 yaw）
 * @return (cos yaw, 0, -sin yaw)；yaw=0 时为 (1,0,0)
 */
export glm::vec3 cameraRight(const Camera& camera) {
	return glm::vec3(std::cos(camera.yaw), 0.0f, -std::sin(camera.yaw));
}

/**
 * 相机 up 向量：必须与 cameraForward 同约定——先绕世界 Y 转 yaw，再绕相机右轴转 pitch。
 * 由该约定推得 up = (sin yaw·sin pitch, cos pitch, +cos yaw·sin pitch)——z 分量
 * 符号写反时 up 与视线会在特定 (yaw, pitch) 处反平行（yaw=0, pitch=±45° 时
 * cross(f,up)=0），lookAt 基退化 → 画面中途 180° 翻滚（2026-09-03 天旋地转根因）。
 * @param camera 相机姿态（读 yaw/pitch）
 * @return up 向量（单位向量）
 */
export glm::vec3 cameraUp(const Camera& camera) {
	return glm::vec3(std::sin(camera.yaw) * std::sin(camera.pitch),
		std::cos(camera.pitch),
		std::cos(camera.yaw) * std::sin(camera.pitch));
}

/**
 * 由 position→target 反推朝向（fps 模式专用换算）：写入 target 并按
 * `fwd = target - position` 推出 yaw/pitch。
 *
 * 动机：fps 模式的 view 只由 yaw/pitch 推导（见 cameraView），`target` 单独写不产生
 * 任何画面变化——"改了 target 却没转视角"。凡是"给 target 就该看向它"的写入路径
 * （llm_debug 机位、RPC `POST /camera`）都必须过这层换算。
 * 约定与 `camera_config` 的初始机位读取一致（camera_config_impl.cppm
 * readCameraPlacement 直接调用本函数），故两条路径反推结果**逐位**相同。
 *
 * 不改 `up`：trackball 的 up 由 lookAt 携带、fps 的 up 由 yaw/pitch 推导，
 * 两条路径都不需要反推 up。
 *
 * 🔴 **减法与归一化必须在 double 域**（债-151，2026-10-26 修）：`target` 与 `position` 都是
 * **绝对层**的点，先各自收窄成 float32 再相减 = 用 ULP 0.0625 m（|p| = 1e6 处）的格子去量一个
 * 几米长的差向量。实测代价：eye/target 间距 8 m 时视向偏 0.20°，1600×900 画面整体平移 ~5 px。
 * 收窄只发生在**结果侧**——角度落回 float 字段（yaw/pitch 是 float 角度量，与 cameraForward /
 * cameraUp 同约定），差向量本身不经 float。
 * @param camera 写入目标（position 须已定，target/yaw/pitch 被写）
 * @param target 期望看向的点（**绝对层**，与 position 同域；float 入参请在调用点显式放大）
 */
export void cameraAimAt(Camera& camera, const glm::dvec3& target) {
	camera.target = target;  // 绝对层 ↔ 绝对层，不经 float
	const glm::dvec3 fwd = target - camera.position;
	if (glm::length(fwd) <= 1e-6)
		return;  // target 与 position 重合 = 退化，朝向保持不变
	const glm::dvec3 unit = glm::normalize(fwd);
	// 唯一一次收窄在这里：double 角度量落进 float 字段（yaw/pitch 是 float 角度量）。
	camera.pitch = static_cast<float>(std::asin(std::clamp(unit.y, -1.0, 1.0)));
	camera.yaw = static_cast<float>(std::atan2(-unit.x, -unit.z));
}

/**
 * 相机 view 矩阵（列主序，world→view）。
 * Trackball：lookAt(position, target, 携带 up)，视线恒指向 target（极点无奇异）；
 * Fps：用 yaw/pitch 推导的 forward/up 做 lookAt，target 不参与（自由飞行）。
 *
 * ## 相对坐标批 4 第二刀：view 是「shader 侧世界坐标一律相对原点」的第一个出口
 * eye/target 是绝对层的**点**（dvec3），各**先在 double 域减原点、再收窄**进 float 的
 * `glm::lookAt`（减法只由 coordinate_rebase::toRelative 做，顺序承重的完整论证在那儿）。
 * 为什么这一处是主要收益：view 的平移列 = (−s·eye, −u·eye, +f·eye)，量级 ∝ |eye| ——
 * 正是债-85 ① 点名的抖动主导项。|camera| ≈ 1e6 而不减原点，eye 进 lookAt 之前就已经被
 * float32 舍过一道（该量级 ULP = 0.0625）；减完原点平移列量级只剩「离原点多远」。
 * 旋转部分只吃 target−eye 这类**差**（平移不变），不受原点影响——所以本刀不碰
 * cameraAimAt / applyTrackballDrag / applyMovement 里的相机内部几何（任务书「不许改」表）。
 * ⚠ 那句「不许改」只约束批 4 第二刀自己（它要守门关逐位不变），**不是这些函数的长期域约定**：
 * 它们内部的「先收窄再相减」随后由债-151 单独修掉（见 Camera 结构注释的域约定与 cameraAimAt）。
 *
 * 门关（origin = dvec3(0)，出厂默认）⟹ toRelative 恒等 ⟹ 与改前逐位相同（交付锚不变；
 * 单元级对应 = tests/test_view_light_rebase.cppm 的门关恒等锚）。
 * @param camera 相机姿态与模式
 * @param origin **本消费者（渲染管线）自己携带的渲染原点**（绝对层）。它是**参数不是全局
 *        单例**（docs/相对坐标地基_plan.md §2.1）：调用点 projectionSystem 传本帧
 *        RenderOrigin::position；物理 / 体积光将来各自传自己的原点，不改本签名。
 *        传 dvec3(0) = 不 rebase = 旧行为。
 * @return view 矩阵（float；它把「相对本原点的世界坐标」映到视空间）
 */
export glm::mat4 cameraView(const Camera& camera, const glm::dvec3& origin) {
	// trackball：视角恒指向 target，up 用携带状态（basis 携带，极点无奇异）；
	// fps：自由飞行，forward/up 由 yaw/pitch 推导，target 不参与
	// 🔴 落库形态 = float mat4（glm::lookAt 的 float 版）：position/target 是绝对层
	// （double），**先由 toRelative 在 double 域减原点、再收窄**（批 4 第二刀；门关 =
	// 原点 0 时这里就是一次收窄，与批 2 第一刀的 `glm::vec3{camera.position}` 逐位相同）。
	const glm::vec3 eye = toRelative(camera.position, origin);
	if (camera.mode == CameraMode::Trackball)
		return glm::lookAt(eye, toRelative(camera.target, origin), camera.up);
	return glm::lookAt(eye, eye + cameraForward(camera), cameraUp(camera));
}

/**
 * Rodrigues 旋转公式：绕已归一化 axis 转 angle（弧度）。
 * @param v 被旋转向量
 * @param angle 旋转角（弧度）
 * @param axis 旋转轴（@pre 已归一化）
 * @return 旋转后的向量
 * @note GLM 1.0.x 的向量版 glm::rotate 在已弃用的 gtx/rotate_vector.hpp，不引入。
 */
glm::vec3 rotateAroundAxis(glm::vec3 v, float angle, glm::vec3 axis) {
	return v * std::cos(angle) + glm::cross(axis, v) * std::sin(angle) +
		axis * glm::dot(axis, v) * (1.0f - std::cos(angle));
}

/** pitch 限制 ±89°，防止万向翻转（manual 换算：GLM_FORCE_RADIANS 下 radians() 无效） */
constexpr float kPitchLimit = 89.0f * glm::pi<float>() / 180.0f;
/**
 * 单帧鼠标增量限幅（x/y 各自独立 clamp）：Wayland 可能一次下发超大增量
 *（合并的 pointer 事件 / 指针 grab），不限幅时相机一帧内甩出几百度，
 * 目标物体直接飞出视野。
 */
constexpr float kMaxMouseDeltaPerFrame = 100.0f;
/** 单帧滚轮增量限幅（动机同 kMaxMouseDeltaPerFrame）。 */
constexpr float kMaxWheelPerFrame = 10.0f;

/** 本帧消费 InputEvents 后的增量累积（delta/wheel 事件求和）。 */
struct FrameInput {
	glm::vec2 mouseDelta{0.0f};
	float wheel = 0.0f;
};

/** F1 调试快捷键：当前机位打进日志（camera_saved 行，可直接抄回 camera.lua）。 */
void logCameraPose(const Camera& camera, RLLogger* logger) {
	if (!logger)
		return;
	// 机位打印的 `t`（fps 下"沿视线推一段的假想 target"）**整条改在 double 域**（债-151 同族，
	// 2026-10-26）：旧写法先把 position/target 各收窄成 float32 再相减取长度、再往 float 的 pos
	// 上加偏移——|camera| = 1e6 处那个"假想 target"会整分量掉进 0.0625 m 的网格。本行的用途是
	// **抄回 camera.lua 当绝对机位**，打错位的数比不打更坏。
	// ⚠ 但 position/target 六个分量打的是**绝对层的 double**，
	// 不再是它的 float32 影子：交付机位（configs/camera.lua 的 1.1/0.75/1.05/0.14/0.12/0.02
	// 与 fov 45）实测 {:.6f} 读数逐字符不变，|camera| 到 625 量级那档才会看到差别
	//（改前 625.123413 = float32 舍入的影子，改后 625.123400 = Lua 里那个数本身）。
	// 这条日志的用途是"抄回 camera.lua"，打原值比打影子更对。
	// ⚠ 口径差别（批 4 第二刀起要说明）：这里打的**永远是绝对层世界位置**——它回答
	// "相机在世界哪里"，绝对才是对的答案（camera.lua 里存的就是绝对机位）。而 shader 侧
	// 的世界坐标（view / scene data）在门开态是**相对渲染原点**的（cameraView 减过原点），
	// 两边数值不同是设计如此，不是漂移；门关（出厂默认）两者重合。
	const glm::dvec3 t = camera.mode == CameraMode::Fps
		? camera.position + glm::dvec3(cameraForward(camera)) *
			std::max(glm::length(camera.target - camera.position), 0.05)
		: camera.target;
	logger->info("[camera_saved] mode = \"{}\", position = {{ {:.6f}, {:.6f}, {:.6f} }}, target = {{ {:.6f}, {:.6f}, {:.6f} }}, fovDegrees = {:.4f}",
		camera.mode == CameraMode::Fps ? "fps" : "trackball",
		camera.position.x, camera.position.y, camera.position.z,
		t.x, t.y, t.z, glm::degrees(camera.fov));
}

/** 键事件（KeyDown/KeyUp）→ CameraState level 状态 + F1 打印机位。 */
void applyKeyEvent(const Camera& camera, CameraState& state, const InputEvent& ev,
	RLLogger* logger) {
	const auto& keys = camera.keys;
	if (ev.type == InputEvent::Type::KeyDown) {
		if (ev.key == SDLK_F1)
			logCameraPose(camera, logger);
		else if (ev.key == keys.forward) state.keyForward = true;
		else if (ev.key == keys.back) state.keyBack = true;
		else if (ev.key == keys.left) state.keyLeft = true;
		else if (ev.key == keys.right) state.keyRight = true;
		return;
	}
	// KeyUp
	if (ev.key == keys.forward) state.keyForward = false;
	else if (ev.key == keys.back) state.keyBack = false;
	else if (ev.key == keys.left) state.keyLeft = false;
	else if (ev.key == keys.right) state.keyRight = false;
}

/**
 * 消费 InputEvents（消费即清空）：键事件按 Camera.keys 映射为 CameraState level
 * 状态，鼠标按键事件更新左/右键 level，MouseDelta/Wheel 累积为本帧增量；
 * F1 = 打印机位。
 */
FrameInput consumeInputEvents(const Camera& camera, CameraState& state,
	InputEvents& events, RLLogger* logger) {
	FrameInput frame;
	for (const auto& ev : events.events) {
		switch (ev.type) {
			case InputEvent::Type::KeyDown:
			case InputEvent::Type::KeyUp:
				applyKeyEvent(camera, state, ev, logger);
				break;
			case InputEvent::Type::MouseButton:
				if (ev.button == SDL_BUTTON_LEFT) state.mouseLeftDown = ev.pressed;
				else if (ev.button == SDL_BUTTON_RIGHT) state.mouseRightDown = ev.pressed;
				break;
			case InputEvent::Type::MouseDelta:
				frame.mouseDelta += ev.delta;
				break;
			case InputEvent::Type::Wheel:
				frame.wheel += ev.amount;
				break;
		}
	}
	events.events.clear();
	return frame;
}

/** Fps 拖拽 look：原地转朝向（拖右→向右看、拖上→向上看）。 */
void applyFpsDrag(Camera& camera, float dx, float dy) {
	camera.yaw -= dx * camera.fpsSensitivity;
	camera.pitch -= dy * camera.fpsSensitivity;
}

/**
 * Trackball 拖拽公转：basis 携带（同 legacy TrackballSystem）——offset 与 up 随拖拽
 * 同步旋转，不反推 yaw/pitch（避开 atan2(0,0) 极点奇异）。up 与视线同旋转保内积，
 * 恒垂直视线 → 极点不抖、可翻过头顶（roll 连续累积，物理转球手感）。
 * 手感约定（同 three.js OrbitControls）：拖右→object 正面朝右（相机向左公转）；
 * 拖下→object 顶部朝向观察者（相机上移）。offset 长度 < 1e-4 时跳过。
 */
void applyTrackballDrag(Camera& camera, float dx, float dy) {
	// 🔴 差向量在 double 域算、落库也在 double 域加（债-151，2026-10-26；域约定见 Camera 结构注释）。
	// 两处各治一半，缺一不可：
	//  ① 相减：`glm::vec3(position) - glm::vec3(target)` 在 |p| = 1e6 处把 offset 量化到
	//     0.0625 m 的格子上，而 offset 本身只有几米 ⟹ 公转半径与 forward 都是错的。
	//     改成先减后收窄：差向量量级 ~1e0，收窄只有 1e-7 的相对误差。
	//  ② 落库：`glm::dvec3(glm::vec3(target) + offset)` 把**float 舍过的相机位置写回 double
	//     字段** ⟹ 每拖一次，position 被永久量化到 float32 网格（1e6 处 6 cm）且**逐次累积**
	//     ——比 ① 更隐蔽，因为它不在任何一帧"看起来"出错。
	// 旋转本身仍留在 float32：它吃的是差向量（相对量，量级 ~1e0），且 float 域行为是当年
	// trackballSensitivity 调参的依据，不顺手改。
	const glm::dvec3 offsetAbs = camera.position - camera.target;
	if (glm::length(offsetAbs) <= 1e-4)
		return;
	glm::vec3 offset{offsetAbs};
	const float s = camera.trackballSensitivity;
	// 竖直轴 = 屏幕右向量 forward × up（恒 ⊥ 二者，极点非零、无奇异）
	glm::vec3 forward = glm::vec3(glm::normalize(-offsetAbs));
	glm::vec3 right = glm::normalize(glm::cross(forward, camera.up));
	// 水平：绕世界 Y（offset 与 up 同步旋转）
	offset = rotateAroundAxis(offset, -dx * s, glm::vec3(0.0f, 1.0f, 0.0f));
	camera.up = rotateAroundAxis(camera.up, -dx * s, glm::vec3(0.0f, 1.0f, 0.0f));
	// 竖直：绕屏幕右轴（offset 与 up 同步旋转）
	offset = rotateAroundAxis(offset, -dy * s, right);
	camera.up = rotateAroundAxis(camera.up, -dy * s, right);
	// 落库回绝对层：double 域相加（见上面 ②），float 的只有旋转后的相对偏移。
	camera.position = camera.target + glm::dvec3(offset);
}

/** 拖拽旋转入口：增量限幅（触发时打 debug 日志）后按模式分发（dragging 判定在调用方）。 */
void applyDrag(Camera& camera, const FrameInput& frame, RLLogger* logger) {
	const float dx = std::clamp(frame.mouseDelta.x, -kMaxMouseDeltaPerFrame,
		kMaxMouseDeltaPerFrame);
	const float dy = std::clamp(frame.mouseDelta.y, -kMaxMouseDeltaPerFrame,
		kMaxMouseDeltaPerFrame);
	if (dx == kMaxMouseDeltaPerFrame || dx == -kMaxMouseDeltaPerFrame ||
		dy == kMaxMouseDeltaPerFrame || dy == -kMaxMouseDeltaPerFrame) {
		if (logger)
			logger->debug("camera: clamped huge mouse delta ({}, {}) -> ({}, {})",
				frame.mouseDelta.x, frame.mouseDelta.y, dx, dy);
	}
	if (camera.mode == CameraMode::Fps)
		applyFpsDrag(camera, dx, dy);
	else
		applyTrackballDrag(camera, dx, dy);
}

/**
 * 移动：Fps 按 WASD level 状态 × fpsSpeed × dt 平移；Trackball 滚轮相对缩放
 * （半径 ×(1∓zoom×zoomSpeed)，wheel y 正 = 滚近）。
 */
void applyMovement(Camera& camera, const CameraState& state, float wheel, float dt) {
	if (camera.mode == CameraMode::Fps) {
		const float fwd = (state.keyForward ? 1.0f : 0.0f) - (state.keyBack ? 1.0f : 0.0f);
		const float strafe = (state.keyRight ? 1.0f : 0.0f) - (state.keyLeft ? 1.0f : 0.0f);
		if (fwd != 0.0f || strafe != 0.0f) {
			// ⚠ **批 2 第一刀的过渡形态**（相对坐标计划 §3 批 2 / 风险表「分两刀」条）：
			// 增量**右值**仍按老办法在 float32 里算完（下面那个表达式一个字符没动），但
			// 🔴 **累加那一步已经是 double 了**——`dvec3 += vec3` 会把右值提升成 dvec3 再相加，
			// 这是 `Camera::position` 换类型的**直接后果，不是刻意选择**。
			// ⟹ 债-146 的两半里，「逐帧累加舍入」这一半**本刀已经顺带修掉**，
			//    第二刀剩下的只有「增量右值 double 化」（`cameraForward` / `fpsSpeed * dt`
			//    目前仍在 float 域）。
			// ⚠ 与下方 trackball 分支**不一致**：那边刻意先收窄成 float 把整个算术留在 float 域
			//    （相对缩放对半径做乘法，float 域行为是它当年调参的依据，不能顺手改）。
			// 交付图仍逐位不变，理由不是「数值路径没动」，而是**交付配置下相机根本不动**
			// （headless 无输入事件 ⟹ `fwd = strafe = 0` 早退；`frame_yaw = 0`）。
			camera.position += (cameraForward(camera) * fwd +
					cameraRight(camera) * strafe) * (camera.fpsSpeed * dt);
		}
		return;
	}
	if (wheel == 0.0f)
		return;
	// 相对缩放（同 three.js OrbitControls：半径 ×(1∓zoom)）——加法式（position +=
	// forward*zoom*step）在远机位失效（cottage 机位半径 ~3535 vs zoomSpeed 0.03 =
	// 每格 0.0009%，2026-08-28 用户反馈"滚轮缩放没用了"）；滚近（y 正）半径缩小
	// 🔴 半径（差向量）在 double 域相减、落库在 double 域相加（债-151，2026-10-26）：
	// 旧写法 `glm::dvec3(glm::vec3(target) + radius * k)` 把 float 舍过的位置写回 double 字段
	// ⟹ 每滚一格 position 被永久量化到 float32 网格（|p| = 1e6 处 6 cm，且逐次累积）。
	// 缩放系数仍作用在 float 的相对偏移上（同 applyTrackballDrag：float 域行为是当年
	// zoomSpeed 调参的依据，半径量级 ~1e0 时收窄只有 1e-7 相对误差）。
	const float clamped = std::clamp(wheel, -kMaxWheelPerFrame, kMaxWheelPerFrame);
	const glm::vec3 radius{camera.position - camera.target};
	camera.position = camera.target + glm::dvec3(radius * (1.0f - clamped * camera.zoomSpeed));
}

/**
 * 帧驱动 yaw 推进（门 `frame_yaw`，默认 0 = 本函数首行早退 = yaw 逐位今天）。
 *
 * 重定基判据 = 「yaw ≠ 纯函数期望值」：任何外部写入（RPC `POST /camera`、初始帧
 * yawBase 尚为 0 的自举）都会在下一次 cameraSystem 被识别并以外部值重定基——
 * 写入方**无需知道本门存在**（RPC 处理器 / 热重载零改动）。重定基后跨进程仍只差
 * 一个**常数帧偏移**（RPC 落在第几帧由启动时序决定），temporal-motion 的逐帧
 * argmin 对齐正是为此而设。等式两侧同式计算 ⟹ 门开且无外部写入时逐位相等，
 * float 比较无容差是刻意的（有容差 = 微小真实写入被吞，对齐偏移里混进亚帧漂移）。
 * @param camera 读写 yaw/yawBase/yawFrameIndex
 */
void applyFrameDrivenYaw(Camera& camera) {
	if (camera.frameYaw == 0.0f)
		return;  // 门关：不进任何算术（门红线）
	const float expected
		= frameDrivenCameraYaw(camera.yawBase, camera.frameYaw, camera.yawFrameIndex);
	if (camera.yaw != expected)
		camera.yawBase = camera.yaw
			- frameDrivenCameraYaw(0.0f, camera.frameYaw, camera.yawFrameIndex);
	camera.yawFrameIndex += 1u;
	camera.yaw = frameDrivenCameraYaw(camera.yawBase, camera.frameYaw, camera.yawFrameIndex);
}

/**
 * 相机 system（每帧一次，system 链首位：camera→origin→animation→bounds→projection→renderlist）：
 * 1) 消费 InputEvents（消费即清空）：键/鼠标按键 → CameraState level 状态，
 *    MouseDelta/Wheel → 本帧增量；
 * 2) 拖拽旋转（Fps = 右键，Trackball = 左键），增量限幅后按模式分发；
 * 3) pitch 每帧 clamp 到 ±89°（防万向翻转）；
 * 4) 移动（WASD 平移 / 滚轮缩放）；
 * 5) 帧驱动 yaw（门 `frame_yaw`，默认关；见 applyFrameDrivenYaw）。
 * @param rg registry（读写 ctx 中的 Camera/CameraState/InputEvents）
 * @param dt 帧间隔（秒，调用方已限幅到 0.1s）
 */
export void cameraSystem(entt::registry& rg, float dt) {
	auto& camera = rg.ctx().get<Camera>();
	auto& state = rg.ctx().get<CameraState>();
	auto& events = rg.ctx().get<InputEvents>();
	auto* logger = RLLogger::getInstance();

	const FrameInput frame = consumeInputEvents(camera, state, events, logger);

	const bool dragging = (camera.mode == CameraMode::Fps && state.mouseRightDown) ||
		(camera.mode == CameraMode::Trackball && state.mouseLeftDown);
	if (dragging)
		applyDrag(camera, frame, logger);

	camera.pitch = std::clamp(camera.pitch, -kPitchLimit, kPitchLimit);

	applyMovement(camera, state, frame.wheel, dt);

	applyFrameDrivenYaw(camera);  // 门 `frame_yaw`（默认 0 = 早退，yaw 逐位不变）
}


}  // namespace RGL
