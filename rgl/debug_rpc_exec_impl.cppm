module;
#include <entt/entt.hpp>      // registry.ctx() 查找/写入 Camera/ConfigReloadHook 等
#include <glm/glm.hpp>        // 相机姿态与三维参数值（vec3）
#include <fmt/format.h>       // 默认截图名 rpc_<N>.png + 错误串拼装
#include <nlohmann/json.hpp>  // 协议 JSON（第三方头，不受 GMF STL 白名单约束）
module debug_rpc_exec:impl;

import debug_rpc_exec;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import debug_rpc;   // DebugRpcServer / RpcRequest / RpcResponder（传输层）
import rlstd;
import rllogger;
import ecs;          // ConfigReloadHook（帧边界挂点，同 10.3/10.4 组合追加）
import diagnostics;  // 门 diagnostics.rpc + rpcPort
import param_pages;  // buildDebugParamPages（/state、/param 复用面板参数页）
import param_registry;   // ParamPage / DebugPanelState（页结构 + 材质选择器）
import camera;       // Camera ctx（/camera 写、/state 读）
import debug_rpc_burst;  // /shotburst 协议判据（形状解析 + 锚点裁决，rlbase 层纯逻辑）
import debug_rpc_shape;   // 批 2：请求形状校验（消费 rpc_schema.lua 生成表）
import debug_rpc_payload;  // B2-5：应答形状闸 + 参数页↔JSON 编解码（拆出去顶的是文件行数红线）
import debug_rpc_capabilities;  // 能力观测批 1：capabilities 段拼装 + 批 3 失败类别→状态码（纯函数，判据在 tests/）
                           //   + 批 2：fsr 段拼装（申报侧 × 生效侧八枚事实，同一纯函数模块）
import debug_rpc_decorate;     // 能力观测批 4：/decorate 探针（词表 + 分类 + 会话 + 结算全在这一家）
import scene_context;    // 能力观测批 2：SceneEnv 的 FSR 申报侧两枚 + 抖动生效值
import frame_draw_list;  // 债-134：/state 的透明段条数 = segmentSpan("transparent", …)（段口径单源）
import rpc_schema_generated;  // 路由表（分发的唯一来源；真相源 = rgl/rpc_schema.lua）
import asset_table;  // AssetTable（参数页构建）
import vk_renderer;  // Renderer（/shot 帧捕获 + writeCapturePng 宿主）
import rlvk;         // 能力观测批 1：capabilities 段入参 vk::Extent2D（渲染网格基准，与执行器同口径）
import vk_swapchain;  // B2-4③′：/resize 经 Renderer::getSwapchainRef 调 requestHeadlessExtent
import llm_debug;    // writeCapturePng（/shot 落盘，同 main 的 llm 截图出口）
import runtime_config;  // LlmDebugConfig（writeCapturePng 入参，只需 file 字段）

namespace RGL {

namespace {

/**
 * 相机姿态快照（/state 内嵌 + /camera 回显；角度字段一律**度**，与 camera.lua
 * `fovDegrees` 同语义）。生效字段随 mode 而异：trackball 看 position/target/up，
 * fps 看 position/yaw/pitch（见 cameraView），另一组字段照样回显。
 * fps 下的 `target` 经 cameraAimAt 与 yaw/pitch 保持一致（写入即换算朝向）。
 */
nlohmann::json rpcCameraJson(const Camera& cam) {
	return nlohmann::json{
		{"mode", cam.mode == CameraMode::Trackball ? "trackball" : "fps"},
		{"position", {cam.position.x, cam.position.y, cam.position.z}},
		{"target", {cam.target.x, cam.target.y, cam.target.z}},
		{"up", {cam.up.x, cam.up.y, cam.up.z}},
		{"yawDegrees", glm::degrees(cam.yaw)},
		{"pitchDegrees", glm::degrees(cam.pitch)},
		{"fovDegrees", glm::degrees(cam.fov)},
	};
}

/**
 * 读 `[x,y,z]` 字段（形状不符抛 json 异常 → `rpcDispatch` 统一转 400）。
 * 调用方先 `contains` 判存在——缺字段 = 不改，存在但畸形 = 400，二者不可混同。
 * @param body 请求体对象
 * @param key 字段名（来自生成表的 `rpc_field::camera::*` 常量）
 * @return 三维值
 */
glm::vec3 rpcVec3(const nlohmann::json& body, std::string_view key) {
	const auto& value = body.at(key);
	return glm::vec3{value.at(0).get<float>(), value.at(1).get<float>(),
		value.at(2).get<float>()};
}


/**
 * RPC 装配态（wireDebugRpc 内部；applier 闭包与帧边界闭包经 `shared_ptr` 共享，
 * 跨帧持有 = /shot 的响应句柄）。生命周期 = ctx 挂点（registry 活多久它活多久）。
 */
struct RpcWiring {
	/** @param rg 场景 registry（相机/面板态 + ConfigReloadHook 所在 ctx）
	 *  @param assetTable 资产表（材质写通道）
	 *  @param rendererRef 渲染器（帧捕获）
	 *  @param runtimeRef runtime.lua 生效值（/decorate 的配置门读数，只读）
	 *  @param llmRunning llm 截图模式在跑（/shot 拒 409） */
	RpcWiring(entt::registry& rg, AssetTable& assetTable, Renderer& rendererRef,
		const RuntimeConfig& runtimeRef, bool llmRunning)
		: registry(rg), assets(assetTable), renderer(rendererRef), runtime(runtimeRef),
		  llmActive(llmRunning) {}

	entt::registry& registry;
	AssetTable& assets;
	Renderer& renderer;
	const RuntimeConfig& runtime;  // /decorate 探针的配置门（aa / velocityField / ssr.enabled）
	const bool llmActive;                    // llm 截图模式在跑（/shot 会抢帧 → 拒）
	std::optional<RpcResponder> pendingShot;  // 在途 /shot（跨帧响应句柄）
	// ── /shotburst（批 12.6 T4）：连续 N 帧一次性采集 ──
	// 为什么必须有它：`/shot` 是「帧首收单、本帧回读」，两次 HTTP 之间引擎自由跑帧，
	// 跨了几帧**看墙钟时序** ⟹ 时域差分（相邻帧差）测的其实是随机相位窗，不可复现。
	// 突发端点把「下一张」的请求挂在**帧首结算钩子里**（该钩子先于 drawFrame），于是
	// 采集落在连续的第 N、N+1、N+2… 帧上，连续性由帧循环保证，与 HTTP 时序无关。
	std::optional<RpcResponder> pendingBurst;  ///< 在途 /shotburst（与 pendingShot 互斥）
	std::string burstDir;                      ///< 输出目录（相对 CWD）
	std::string burstPrefix = "burst_";        ///< 文件名前缀
	int burstGbuf = -1;                        ///< 回读源（-1 = swapchain 后缓冲）
	std::string burstAttachment;               ///< 具名附件回读（非空 = 覆盖 burstGbuf；temporal-motion 的 taa_out alpha 掩膜连拍）
	uint32_t burstRemaining = 0;               ///< 还差几张（0 = 本批已交齐）
	uint32_t burstIndex = 0;                   ///< 下一张的序号（文件名后缀）
	std::vector<std::string> burstFiles;       ///< 已写盘的绝对路径（完成时在应答里回显）
	// ── 债-89：连拍锚 = 绝对帧号（`start_frame`）──
	uint32_t burstStartFrame = 0;              ///< >0 = 锚点未到（帧首钩子里等帧边界；0 = 无锚/已触发）
	uint32_t burstActualStart = 0;             ///< 首张实际落帧（应答回显；首张写盘时记 = 跳帧时也是真相）
	std::string pendingShotFile;              // 期望 PNG 相对路径（gbuf 通道路径由写盘方定）
	uint32_t shotSeq = 0;                     // 缺省文件名序号（screenshots/rpc_<N>.png）
	uint32_t frame = 0;                       // 帧计数（/state 与根路由回显，纯观测）
	// ── /pass 逐帧翻转（批次 B2-5）：与 /shotburst 同一个理由的同一个解法 ──
	// 逐帧切换必须落在帧边界（渲染线程 tick），不能由 HTTP 往返驱动（跨几帧看墙钟）。
	std::string passToggleName;      ///< 翻转目标 pass 名
	uint32_t passToggleRemaining = 0;  ///< 还要翻几帧（0 = 没在跑）
	bool passToggleNext = false;     ///< 下一帧要置的状态（每帧取反）
	// ── /decorate 动态能力探针（能力观测批 4）：会话状态本体在 debug_rpc_decorate ──
	// 改图走 Renderer 既有复合事务（安全点执行），收单在帧边界、结果要下一帧才读得到；
	// 探针**不新开一条写图路径**（申报 ≠ 发射权在运行期的形态）。
	DecorateSession decorate;
};

/** GET /state：相机姿态 + 全部参数页（label 发现面，/param 按 label 寻址）。 */
RpcOutcome rpcHandleState(RpcWiring& w) {
	auto pages = buildDebugParamPages(w.registry, w.assets);
	nlohmann::json pageList = nlohmann::json::array();
	for (const auto& page : pages)
		pageList.push_back({{"name", page.name}, {"entries", rpcEntriesJson(page)}});
	// render 段（批 14.3，判据③「口径显式记录」）：三个 extent 并出 + 生效倍率。
	// 为什么必须在这里出声：像素类判据（imgqa / 闭式解探针）读的是**后缓冲**截图，而
	// renderScale≠1 时画面是内部网格放大来的。判据若不知道自己看的是哪档，就会把
	// "0.5 档变软"误读成"抗锯齿回归"。三个值都给，工具不必再猜换算。
	const auto& we = w.registry.ctx().get<WindowExtent>();
	const auto& re = w.registry.ctx().get<RenderExtent>();
	const auto graph = w.renderer.graphStatus();  // B2-5 图段快照（取一次，别每字段现取）
	// 透明段条数（债-134 的先决读数口）。口径**不在此重算**：吃 `segmentSpan("transparent", …)`
	// = 执行器画那一段时用的同一个 span，所以「RPC 报的条数」与「透明 pass 真录的 draw 数」
	// 结构上不可能各说各话（写成 `calls.size() − transparentStart` 就是第二份口径，会漂）。
	// ⚠ 读数时刻 = 上一次帧边界 drain 时 ctx 里那份 FrameDrawList，即**上一帧**的段：
	//   本端点的 poll 挂在 ConfigReloadHook（system 链**之前**），而 renderListSystem 在链里。
	//   ⟹ 第 1 次 drain（frame==1）读到的是装配期 emplace 的默认空列表，恒 0，
	//   **不是**「场景没有透明图元」。消费方（`imgqa blend-lit`）据此要求 frame ≥ 2。
	const auto transparentSpan = segmentSpan("transparent", w.registry.ctx().get<FrameDrawList>());
	// ⚠ **`render` 的值必须显式写成 `nlohmann::json{...}`**，不能只写 `{...}`。
	//   踩过的坑（批 14.3 引入、本批发现）：写成 `{{"render", {...}}}` 时该元素是
	//   「单元素数组」而不是 `{"key", value}` 二元对，而 nlohmann 的 initializer-list
	//   构造要求**每个**元素都是二元对 ⟹ 只要有一个不是，**整个 `/state` 退化成数组**。
	//   后果：`Engine::paramValue` 的 `out.at("pages")` 抛
	//   `type_error.304 cannot use at() with array` ⟹ `temporal-flicker` 与
	//   `temporal-motion`（含 `temporal-tail` 族）整台仪器起不来。那条异常**是**响亮的
	//   （实测 `imgqa temporal-flicker` 退出码 1），坏在**指错人**——它说"调用侧用了错的
	//   API"，而真因在服务端少一层花括号。形状守卫见 `rpcOk`。
	return rpcOk({{"ok", true},
		{"frame", w.frame},
		{"camera", rpcCameraJson(w.registry.ctx().get<Camera>())},
		{"pages", std::move(pageList)},
		{"render", nlohmann::json{
			{"windowWidth", we.width},
			{"windowHeight", we.height},
			{"renderWidth", re.width},
			{"renderHeight", re.height},
			{"scale", w.renderer.getSwapchainRef().getRenderScale()},
			// 段条数走 span 的 size（口径见上方 transparentSpan 的注释：与执行器同源）
			{"transparentDraws", static_cast<uint32_t>(transparentSpan.size())},
		}},
		// B2-5 图段（判据「开关真的发生了」的**唯一**观测面）：为什么不能只看截图——
		// highlight 在无选中时零 draw，画面本来就不该变，于是「图变了没有」必须从别处读。
		// passes = desc 里的 pass 数（插拔的直接读数）；recompiles / lastRecompileUs =
		// 代价读数（逐帧 = 测试模态不是 SLA 这条裁决靠它）；dynamicPasses = 在册可开关的。
		{"graph", nlohmann::json{
			{"passes", graph.passCount},
			{"presentPasses", graph.presentPasses},
			{"dynamicPasses", graph.dynamicPasses},
			{"recompiles", graph.recompiles},
			{"lastRecompileUs", static_cast<double>(graph.lastRecompileNs) / 1000.0},
		}},
		// 能力段（能力观测批 1，docs/能力观测_plan.md §3.2）：内容**一律从编译产物现算**，
		// 与上面的 graph 段是两个口径（graph = 声明侧既有读数；capabilities = 这一帧实际
		// 在画什么 + 每张图属 RT 建没建）。拼装是纯函数（判据 = tests/test_rpc_capabilities），
		// 这里只做取数：编译图 + 脏位来源 + 逐附件问缓存（名单来自图，缓存只被问，§3.3）。
		{"capabilities", rpcCapabilitiesJson(
			w.renderer.dynamicGraphRef(), w.renderer.getRenderGraphRef(),
			[&w](std::string_view name) { return w.renderer.getRtCacheRef().isBuilt(name); },
			vk::Extent2D { re.width, re.height }, w.frame)},
		// FSR 段（能力观测批 2）：FSR 是**图外 compute**，不在编译图里 ⟹ 判据吃不到能力段，
		// 给它一个兄弟段。申报侧两枚 = SceneEnv（装配期回填的配置原值），生效侧六枚 =
		// Renderer / FsrUpscaler 现状（context 建没建、时域跑没跑、vendor 实收尺寸）。
		// 抖动接线（jitterWired / jitterPhase）也是生效侧：SceneEnv 里那两枚就是
		// wireFsrJitter 按 fsrUsable 裁决后的值（申报值 = 配置键，今天无消费者，不报）。
		{"fsr", rpcFsrJson(RgFsrFacts {
			.gateOpen = w.registry.ctx().get<SceneEnv>().fsrGateOpen,
			.usable = w.renderer.fsrUsable(),
			.renderWidth = w.renderer.fsrRenderExtent().width,
			.renderHeight = w.renderer.fsrRenderExtent().height,
			.upscaleWidth = w.renderer.fsrUpscaleExtent().width,
			.upscaleHeight = w.renderer.fsrUpscaleExtent().height,
			.temporalEffective = w.renderer.fsrTemporalEffective(),
			.temporalConfig = w.registry.ctx().get<SceneEnv>().fsrTemporalConfig,
			.jitterWired = w.registry.ctx().get<SceneEnv>().fsrJitter,
			.jitterPhase = w.registry.ctx().get<SceneEnv>().fsrJitterPeriod,
		})}});
}

/**
 * `mode` 字段套用（仅 rpcHandleCamera 内部使用）：缺字段不改。
 *
 * 取值白名单与它的文案都在能力表里（`kind = "string"` + `oneOf`），所以走到这里 `mode`
 * 必是 trackball | fps 之一。分支吃**生成表的取值常量**而不是再抄一遍字面量——改 schema
 * 里的取值，这里当场编译不过（抄字面量就是第二份副本）。
 * @param body 请求体对象（已过形状校验）
 * @param cam 写入目标（仅命中已知值时改）
 */
void rpcApplyCameraMode(const nlohmann::json& body, Camera& cam) {
	if (!body.contains(rpc_field::camera::mode))
		return;
	const auto& mode = body.at(rpc_field::camera::mode).get_ref<const std::string&>();
	cam.mode = mode == rpc_value::camera::mode::trackball ? CameraMode::Trackball
	                                                      : CameraMode::Fps;
}

/**
 * `yaw_at_frame` 套用（债-89 机位锚点；从 `rpcHandleCamera` 拆出 = 认知复杂度红线）。
 *
 * 为什么必须在引擎侧（客户端预补偿必然失败）：`frame_yaw` 下写入落帧由 HTTP 时序决定，
 * 本机 headless ~6000fps 下落帧抖动 ±36 帧量级 = 0.125px/帧 × 36 ≈ 4.5px，与要消的噪声
 * 同源——只有把机位基准解成**绝对帧号的函数**（`frameDrivenYawPin`）才与落帧无关。
 *
 * 批 2 换血：本字段的**形状**（整数、[1, 2^32)）与那条 400 文案已进能力表；留在这里的
 * 两条是**运行期状态**判据（门红线 / 锚已过），它们要读 `Camera` 与帧计数，不是请求形状，
 * 因此本来也进不了 schema。
 * @param body 请求体（已过形状校验）
 * @param frameNow 引擎当前帧计数（`RpcWiring::frame`）
 * @param cam 已套用完本请求其他字段的相机（锚的是**套用后**的朝向）
 * @param pinned 通过时写入锚点帧号（0 = 本请求没带该字段）
 * @return 失败应答（锚点不可用 = 400、锚已过 = 409）；空 = 通过
 */
std::optional<RpcOutcome> rpcApplyYawAnchor(const nlohmann::json& body, uint32_t frameNow,
	Camera& cam, uint32_t& pinned) {
	pinned = 0;
	if (!body.contains(rpc_field::camera::yaw_at_frame))
		return {};
	const uint32_t n =
		static_cast<uint32_t>(body.at(rpc_field::camera::yaw_at_frame).get<int64_t>());
	// 门红线：`frame_yaw = 0` 时 yaw 不是帧号函数 ⟹ 无从钉，响亮拒绝，不静默忽略。
	if (cam.frameYaw == 0.0f)
		return rpcFail(400, "yaw_at_frame needs camera.lua frame_yaw != 0 —— yaw 不是帧号纯函数时无从钉");
	if (n < frameNow)
		return rpcFail(409, "yaw_at_frame " + std::to_string(n) + " already passed (frame "
			+ std::to_string(frameNow) + ") — 机位锚点是绝对帧号：更早发请求或抬高锚点");
	const YawPin pin = frameDrivenYawPin(cam.yaw, cam.frameYaw, cam.yawFrameIndex, frameNow, n);
	cam.yawBase = pin.yawBase;
	cam.yaw = pin.yaw;  // = 本帧期望值 ⟹ 紧随的 cameraSystem 不重定基（否则本针当场被抹）
	pinned = n;
	return {};
}

/**
 * POST /camera：任意子集套用（缺字段不改；形状错 → 400）。
 * 直接写 `Camera` ctx —— cameraSystem 只在有输入时改姿态，帧首套用本帧即生效。
 * @param w 装配态
 * @param body 请求体对象（已过 `rpcValidateRequest` 的形状校验）
 * @return 套用后姿态回显
 */
RpcOutcome rpcHandleCamera(RpcWiring& w, const nlohmann::json& body) {
	auto& cam = w.registry.ctx().get<Camera>();
	rpcApplyCameraMode(body, cam);
	// position 必须先于 target：fps 下的朝向反推要用到新 position
	if (body.contains(rpc_field::camera::position))
		cam.position = rpcVec3(body, rpc_field::camera::position);
	if (body.contains(rpc_field::camera::target)) {
		// fps 模式的 view 只看 yaw/pitch（cameraView），照直写 target 是死写入——回显会
		// 显示"已生效"而画面不动。故 fps 下（未显式给 yaw/pitch 时）走 cameraAimAt 反推，
		// 与 camera_config 初始机位同一约定。显式给了 yawDegrees/pitchDegrees 则尊重显式值。
		const bool explicitAngles = body.contains(rpc_field::camera::yawDegrees)
		    || body.contains(rpc_field::camera::pitchDegrees);
		if (cam.mode == CameraMode::Fps && !explicitAngles)
			cameraAimAt(cam, rpcVec3(body, rpc_field::camera::target));
		else
			cam.target = rpcVec3(body, rpc_field::camera::target);
	}
	if (body.contains(rpc_field::camera::up))
		cam.up = rpcVec3(body, rpc_field::camera::up);
	if (body.contains(rpc_field::camera::yawDegrees))
		cam.yaw = glm::radians(body.at(rpc_field::camera::yawDegrees).get<float>());
	if (body.contains(rpc_field::camera::pitchDegrees))
		cam.pitch = glm::radians(body.at(rpc_field::camera::pitchDegrees).get<float>());
	if (body.contains(rpc_field::camera::fovDegrees))
		cam.fov = glm::radians(body.at(rpc_field::camera::fovDegrees).get<float>());
	// 债-89：机位锚点（`yaw_at_frame = N` = 第 N 帧渲染出来的朝向 = 本请求套用后的朝向）。
	uint32_t yawPinned = 0;
	if (auto fail = rpcApplyYawAnchor(body, w.frame, cam, yawPinned); fail)
		return *fail;
	nlohmann::json out{ {"ok", true}, {"camera", rpcCameraJson(cam)} };
	if (yawPinned != 0)
		out["yawPinnedFrame"] = yawPinned;  // 回显自证：客户端核对锚点帧号（没回显 = 没钉）
	return rpcOk(std::move(out));
}

/**
 * POST /param：`{label, value, page?, material?}`。
 * `material` = 先写 `DebugPanelState.materialSel` 再重建页 → 该次写入作用于目标材质
 * （材质页/选中页共用 `appendMaterialControls`，与面板手选等价）。
 *
 * 批 2 换血：四个字段的名字/必填/类型在能力表里（`value` 的**类型**不在——它随目标条目的
 * kind 而异，判据本体在 `rpcWriteParamPage`，那里才知道 label 是滑杆还是颜色）。
 * @param w 装配态
 * @param body 请求体对象（已过形状校验）
 * @return 写入回显；label 未知 → 404
 */
RpcOutcome rpcHandleParam(RpcWiring& w, const nlohmann::json& body) {
	const std::string label = body.at(rpc_field::param::label).get<std::string>();
	if (body.contains(rpc_field::param::material))
		w.registry.ctx().get<DebugPanelState>().materialSel =
			static_cast<uint32_t>(std::max(0, body.at(rpc_field::param::material).get<int>()));
	const auto& value = body.at(rpc_field::param::value);
	auto pages = buildDebugParamPages(w.registry, w.assets);

	if (body.contains(rpc_field::param::page)) {
		const std::string name = body.at(rpc_field::param::page).get<std::string>();
		auto page = std::ranges::find_if(pages,
			[&name](const ParamPage& p) { return p.name == name; });
		if (page == pages.end())
			return rpcFail(404, fmt::format("page not found: {}", name));
		if (auto hit = rpcWriteParamPage(*page, label, value))
			return *hit;
		return rpcFail(404, fmt::format("label not found: {}", label));
	}
	for (const auto& page : pages) {
		if (auto hit = rpcWriteParamPage(page, label, value))
			return *hit;
	}
	return rpcFail(404, fmt::format("label not found: {}", label));
}

/**
 * POST /shot：登记跨帧捕获（应答不在此兑现）。
 * 解析全在 `requestFrameCapture` 与 responder 移交**之前**完成 → 后续抛异常
 * 仍由 rpcDispatch 转 400，不会留下半提交状态。
 *
 * 批 2 换血：`file` / `gbuf` / `attachment` 三个字段的名字、类型，以及「attachment 与 gbuf
 * 互斥」「attachment 非空」两条都在能力表里，由分发层的 `rpcValidateRequest` 执行；本函数
 * 只剩「状态冲突（llm 在跑 / 已有一张在途）→ 409」与登记。
 * @param w 装配态
 * @param body 请求体对象（已过形状校验：`file?` 相对路径、`gbuf?` G-buffer 通道、
 *   `attachment?` 附件名——后两者互斥，同时给 = 400）
 * @param responder 跨帧响应句柄（移交给 w.pendingShot）
 */
void rpcHandleShot(RpcWiring& w, const nlohmann::json& body, RpcResponder responder) {
	if (w.llmActive) {
		rpcReject(responder, 409, "/shot unavailable: llm_debug capture mode is running");
		return;
	}
	if (w.pendingShot) {
		rpcReject(responder, 409, "another /shot is still pending");
		return;
	}
	std::string file;
	if (body.contains(rpc_field::shot::file))
		file = body.at(rpc_field::shot::file).get<std::string>();
	int gbuf = -1;
	if (body.contains(rpc_field::shot::gbuf))
		gbuf = body.at(rpc_field::shot::gbuf).get<int>();
	// attachment（批 12.2）：按名捕获，QA 层取 object_id 全分辨率。与 gbuf 互斥的判据在
	// 能力表里（两者都是"回读哪个源"，同时给必是客户端 bug，静默取一个会拍出用户没要的那张图）。
	if (body.contains(rpc_field::shot::attachment)) {
		const auto name = body.at(rpc_field::shot::attachment).get<std::string>();
		if (file.empty())
			file = fmt::format("screenshots/attach_{}.png", name);
		w.renderer.requestFrameCapture(name);
		w.pendingShotFile = std::move(file);
		w.pendingShot = std::move(responder);
		return;
	}
	if (file.empty())
		file = fmt::format("screenshots/rpc_{}.png", ++w.shotSeq);

	w.renderer.requestFrameCapture(gbuf);  // 本帧 drawFrame 前挂回读（帧边界 = 本帧生效）
	w.pendingShotFile = std::move(file);
	w.pendingShot = std::move(responder);
}

/**
 * @brief 登记一张回读（swapchain 与具名附件二选一）。三处调用（锚点登记 / 到锚首张 /
 *        逐张续拍）同源一份——回读源选择在 burst 生命周期里必须处处一致。
 */
void rpcRequestBurstCapture(RpcWiring& w) {
	if (w.burstAttachment.empty())
		w.renderer.requestFrameCapture(w.burstGbuf);
	else
		w.renderer.requestFrameCapture(std::string_view(w.burstAttachment));
}

/**
 * POST /shotburst：登记「连续 N 帧」采集（应答在最后一张写盘后才兑现）。
 *
 * 与 `/shot` 的**本质**区别不是省了几次往返，而是连续性：`/shot` 每张的帧号取决于
 * 请求落在帧首钩子前还是后（= 墙钟），`/shotburst` 的「下一张」由帧首结算钩子自己登记
 * （见 `rpcSettleShot`），该钩子先于 drawFrame ⟹ 第 N、N+1、N+2… 帧逐张采集。
 * 时域差分（相邻帧差）只有后者测得准（`imgqa temporal`，docs/taa_plan.md §7 第 8 条）。
 *
 * 连续性只保证「相邻」，不保证「第几帧」——债-89：抖动相位 = Halton(绝对帧号)，起始帧
 * 随 HTTP 时序漂移 = 判据读数成彩票。`start_frame` 把起点也钉死：首张 = 第 N 帧，
 * 应答回显 `startFrame` 供调用侧自证；锚已过 = 409（**不静默退回相对**）。
 *
 * @param w 装配态
 * @param body 请求体对象（形状由分发层按能力表校验：`n` 必填 [1,1024]、`dir` 必填非空、
 *   `prefix?` 文件名前缀、`gbuf?` 回读通道、`attachment?` 具名附件回读（与 gbuf 互斥、
 *   白名单 swapchain | taa_out；temporal-motion 取 taa_out alpha 的拒收掩膜连拍）、
 *   `start_frame?` **绝对帧号锚点**（债-89：首张保证落在第 N 帧））
 * @param responder 跨帧响应句柄（移交给 w.pendingBurst）
 */
void rpcHandleShotBurst(RpcWiring& w, const nlohmann::json& body, RpcResponder responder) {
	if (w.llmActive) {
		rpcReject(responder, 409, "/shotburst unavailable: llm_debug capture mode is running");
		return;
	}
	// 与 /shot 抢同一个回读槽：同时挂两种采集 = 客户端 bug，静默排队会拍出错位的图
	if (w.pendingShot) {
		rpcReject(responder, 409, "a /shot is still pending");
		return;
	}
	if (w.pendingBurst) {
		rpcReject(responder, 409, "another /shotburst is still pending");
		return;
	}
	// 形状解析与锚点裁决住在 debug_rpc_burst（rlbase，可在 just test 里逐条钉死）；本处
	// 只做「解析 → 套用装配态 → 无锚则立即登记首帧」。错误分类不变：形状 = 400、锚已过 = 409。
	BurstRequest req;
	if (auto err = parseBurstRequest(body, req); !err.empty()) {
		rpcReject(responder, 400, err);
		return;
	}
	w.burstDir = req.dir;
	if (req.prefix)
		w.burstPrefix = *req.prefix;
	w.burstAttachment = req.attachment;
	w.burstGbuf = req.gbuf;
	w.burstRemaining = req.n;
	w.burstIndex = 0;
	w.burstFiles.clear();
	uint32_t armed = 0;
	if (auto err = resolveBurstAnchor(w.frame, req.startFrame, armed); !err.empty()) {
		rpcReject(responder, 409, err);
		return;
	}
	w.burstStartFrame = armed;
	if (w.burstStartFrame == 0)
		rpcRequestBurstCapture(w);  // 无锚/锚恰在本帧：第一张 = 本帧 drawFrame 回读
	w.pendingBurst = std::move(responder);
}

/**
 * /shotburst 帧首结算（从 `rpcSettleShot` 拆出——突发分支 = 锚点等待 + 逐张写盘 + 逐张
 * 登记，与单张路径同函数会把认知复杂度顶破红线）。
 *
 * 「连续 N 帧」的全部机制：写完一张就在**本钩子**里登记下一张——本钩子先于 drawFrame，
 * 所以登记立刻作用在本帧。带 `start_frame` 锚点（债-89）时还负责「到锚才起跑」：锚点未到
 * = 直接返回，到锚帧在本钩子里登记首张 ⟹ 首张恰落第 start_frame 帧。
 * @param w 装配态（调用方保证 pendingBurst 在途）
 */
void rpcSettleBurst(RpcWiring& w) {
	if (w.burstStartFrame != 0) {
		// 债-89 锚点未到：不登记回读、不写盘，只等帧边界（零 GPU 成本）。本钩子先于
		// drawFrame ⟹ 到锚帧在这里登记首张，采集就恰好落在第 burstStartFrame 帧。
		if (w.frame < w.burstStartFrame)
			return;
		w.burstStartFrame = 0;
		rpcRequestBurstCapture(w);
		return;  // 首张产物 = 本帧 drawFrame，下个帧边界结算
	}
	if (w.renderer.hasFrameCaptureFailure()) {
		// 回读源注定拿不到（附件不在图里 / RT 不建 / 通道越界）⟹ **必须在这里兑现错误**。
		// 旧写法只有「没产物就下帧重试」，而这个循环对「源永不存在」不收敛：
		// pendingBurst 永不复位 ⟹ 之后每个 /shot、/shotburst 都吃 409，
		// 一次拼错的附件名就把截图端点打死到进程重启（2026-10-19 修）。
		const auto why = w.renderer.takeFrameCaptureFailure();
		auto responder = std::move(*w.pendingBurst);
		w.pendingBurst.reset();
		w.burstRemaining = 0;
		// 状态码按**类别**挑（404 = 点名的东西不存在；400 = 参数越界），不解析 message 猜。
		rpcReject(responder, why ? rpcStatusForCaptureFailure(why->kind) : 500,
		          why ? std::move(why->message) : std::string("frame capture source unavailable"));
		return;
	}
	if (!w.renderer.hasFrameCapture())
		return;  // 本帧无产物（acquire 跳帧）：请求保持 pending，下个帧边界重试
	LlmDebugConfig shot;
	shot.file = fmt::format("{}/{}{:03d}.png", w.burstDir, w.burstPrefix, w.burstIndex);
	auto file = writeCapturePng(w.renderer, shot);
	if (!file) {
		auto responder = std::move(*w.pendingBurst);
		w.pendingBurst.reset();
		w.burstRemaining = 0;
		rpcReject(responder, 500, file.error().message);
		return;
	}
	if (w.burstIndex == 0)
		// 首张落帧自证：产物 = 上一帧 drawFrame 画的（锚帧被 acquire 跳帧时这里记的
		// 才是真正出图的那一帧——回显与请求锚点不符会被客户端判抛，不静默）。
		w.burstActualStart = w.frame - 1;
	++w.burstIndex;
	w.burstFiles.push_back(*file);
	if (--w.burstRemaining == 0) {
		auto responder = std::move(*w.pendingBurst);
		w.pendingBurst.reset();
		// startFrame 回显（债-89）：首张实际落帧。带 start_frame 的调用侧拿它自证
		// 「落帧 == 锚点」；不带锚点的旧调用多一个字段无害（少一个字段才有害——
		// 旧构建对 start_frame 静默无回显，客户端据此抛，见 Engine::shotBurstAt）。
		responder.resolve(200, rpcOk({{"ok", true}, {"count", w.burstIndex},
			{"startFrame", w.burstActualStart}, {"files", w.burstFiles}}).body.dump());
		return;
	}
	// 下一张 = 本帧（连续，不跨 HTTP）；回读源与首张同款选择
	rpcRequestBurstCapture(w);
}

/**
 * /shot 跨帧结算（帧首、drain 之前调用）：捕获产物就绪 → 写 PNG → 兑现响应。
 * 未就绪 = 保持 pending（acquire 超时跳帧时请求保持 pending 重试，同 llm_debug）。
 * ⚠ 但「未就绪」有两种，只有暂时的那种该重试：回读源注定不存在时 `hasFrameCapture()`
 *   恒 false，只按它重试 ⟹ 响应句柄永不兑现（客户端等到超时）且 `pendingShot` 卡死，
 *   之后每个 /shot 都吃 409。⟹ 先查 `hasFrameCaptureFailure()` 并立即回 400。
 * 突发分支先走（与单张互斥，不会两边都活），本体见 `rpcSettleBurst`。
 * @param w 装配态
 */
void rpcSettleShot(RpcWiring& w) {
	if (w.pendingBurst) {
		rpcSettleBurst(w);
		return;
	}
	if (!w.pendingShot)
		return;
	if (w.renderer.hasFrameCaptureFailure()) {
		const auto why = w.renderer.takeFrameCaptureFailure();
		auto responder = std::move(*w.pendingShot);
		w.pendingShot.reset();
		// 状态码按**类别**挑（404 = 点名的东西不存在；400 = 参数越界），不解析 message 猜。
		rpcReject(responder, why ? rpcStatusForCaptureFailure(why->kind) : 500,
		          why ? std::move(why->message) : std::string("frame capture source unavailable"));
		return;
	}
	if (!w.renderer.hasFrameCapture())
		return;  // 本帧无产物：等下个帧边界
	auto responder = std::move(*w.pendingShot);
	w.pendingShot.reset();

	LlmDebugConfig shot;
	shot.file = w.pendingShotFile;
	auto file = writeCapturePng(w.renderer, shot);
	if (!file)
		rpcReject(responder, 500, file.error().message);
	else
		responder.resolve(200, rpcOk({{"ok", true}, {"file", *file}}).body.dump());
}

/**
 * POST /quit：置 `AppExit.requested` = 帧循环正常收尾退出（**下个帧边界**生效）。
 *
 * 为什么需要（自动化测试）：llm_debug 一进程一张图即退，RPC `/shot` 可连拍但进程
 * 不自停——测试 harness 只能靠 SIGTERM 杀，那会打断 present/in-flight 且拿不到
 * profiler 收口输出（`profilerDump` 在帧循环之后）。走 AppExit = 与用户关窗完全
 * 同一条干净退出路径。
 *
 * 应答先兑现再置位：`AppExit` 在帧首判定，置位后本帧 HTTP 线程仍要拿到 200。
 * @param w 装配态
 */
RpcOutcome rpcHandleQuit(RpcWiring& w) {
	auto& exit = w.registry.ctx().get<AppExit>();
	exit.requested = true;
	return rpcOk({{"ok", true}, {"frame", w.frame}});
}

/**
 * POST /resize {width, height}（B2-4③′ resize 冒烟）：**headless** 模式改交换链尺寸。
 *
 * 写 WindowExtent ctx + `Swapchain::requestHeadlessExtent`；真正的重建由**下一帧**
 * resizeSystem 完成——与窗口拖拽 resize 完全同一条路（本端点只是把"SDL 拖窗口"换成
 * "发数字"），退役窗口 / 布局结转 / IZB 重建全部走既有判据。窗口模式 =
 * requestHeadlessExtent 出声拒（false）→ 400（那条路的 extent 真相源是 SDL）。
 * @param w 装配态
 * @param body `{"width": N, "height": M}`——两字段的必填与 `> 0` 都在能力表里（越界那条
 *   文案由 schema 给），本函数不再判 0。
 */
RpcOutcome rpcHandleResize(RpcWiring& w, const nlohmann::json& body) {
	const auto width = body.at(rpc_field::resize::width).get<uint32_t>();
	const auto height = body.at(rpc_field::resize::height).get<uint32_t>();
	if (!w.renderer.getSwapchainRef().requestHeadlessExtent(width, height))
		return rpcFail(400, "resize: headless 模式专用（窗口路径 extent 由 SDL 重查）");
	auto& extent = w.registry.ctx().get<WindowExtent>();
	extent.width = width;
	extent.height = height;
	return rpcOk({{"ok", true}, {"width", width}, {"height", height}, {"frame", w.frame}});
}

/**
 * POST /pass（批次 B2-5：pass 级动态开关）。
 *
 * 两种体：
 *  - `{"name": "highlight", "on": true|false}` → 收单一次意图，下个安全点生效（≤1 帧）；
 *  - `{"name": "highlight", "toggle_frames": N}` → **逐帧翻转 N 帧**（压力模态）。
 *
 * 为什么必须有第二种：逐帧切换是本批的测试模态（宪法：正确性 + ≤1 帧延迟，不是 SLA），
 * 而"逐帧"用 HTTP 驱动**做不到**——两次 POST 之间引擎自由跑帧，跨了几帧看墙钟时序，
 * 与 `/shotburst` 当年同一个理由。帧边界翻转落在 tick 里（渲染线程、drawFrame 之前），
 * 连续性由帧循环保证，与 HTTP 时序无关。
 *
 * ⚠ 应答里的 `passes` / `recompiles` 是**收单时刻**的读数（生效在之后的安全点），
 * 判据要读生效后的值就再发一次 `/state`——这里不回"未来"，否则读数与帧号对不上。
 *
 * 🔴 本端点是能力表里唯一标了 `manualShape` 的：`on` 是**条件性必填**（只在未给
 * `toggle_frames` 时才要），而约束语言只表达无条件必填/类型/范围/白名单/互斥——为一条端点
 * 加"requiredUnless"就是把字段约束语言往 OpenAPI 方向做（`docs/rpc_单源_plan.md` 风险 2）。
 * ⟹ 下面三条形状检查留在本函数，字段名仍吃生成常量、未知字段闸门照跑（分发层那次
 * `rpcValidateRequest`）。**残余面**：schema 与这三条手写检查之间无机器对账——改这里的
 * 判据不会有人回来改 schema 的字段清单，反之亦然。
 * @param w 装配态
 * @param body 见上
 */
RpcOutcome rpcHandlePass(RpcWiring& w, const nlohmann::json& body) {
	if (!body.contains(rpc_field::pass::name))
		return rpcFail(400,
			"pass: 需要 \"name\"（装配期登记过、可动态开关的 pass 名；见 /state 的 graph.dynamicPasses）");
	const std::string name = body.at(rpc_field::pass::name).get<std::string>();
	if (body.contains(rpc_field::pass::toggle_frames)) {
		const auto frames = body.at(rpc_field::pass::toggle_frames).get<uint32_t>();
		if (frames == 0)
			return rpcFail(400, "pass: toggle_frames 必须 > 0");
		if (w.passToggleRemaining > 0)
			return rpcFail(409, "pass: 已有一批逐帧翻转在跑（一帧只能有一个意图）");
		w.passToggleName = name;
		w.passToggleRemaining = frames;
		w.passToggleNext = false;  // tick 先翻一次 ⇒ 第 1 帧就换状态
		return rpcOk({{"ok", true}, {"name", name}, {"toggleFrames", frames},
			{"startFrame", w.frame + 1}, {"frame", w.frame}});
	}
	if (!body.contains(rpc_field::pass::on))
		return rpcFail(400, "pass: 需要 \"on\"(bool) 或 \"toggle_frames\"(uint)");
	const bool on = body.at(rpc_field::pass::on).get<bool>();
	w.renderer.requestPassToggle(name, on);
	const auto graph = w.renderer.graphStatus();
	return rpcOk({{"ok", true}, {"name", name}, {"on", on},
		{"passes", graph.passCount}, {"recompiles", graph.recompiles}, {"frame", w.frame}});
}

/**
 * 逐帧翻转的帧边界推进（B2-5；tick 里调，渲染线程、drawFrame 之前）。
 * @param w 装配态
 */
void rpcTickPassToggle(RpcWiring& w) {
	if (w.passToggleRemaining == 0)
		return;
	--w.passToggleRemaining;
	w.passToggleNext = !w.passToggleNext;
	w.renderer.requestPassToggle(w.passToggleName, w.passToggleNext);
}

/**
 * 认领这条请求：查生成表 → 方法核对 → **形状闸门**（批 2）。从 `rpcDispatch` 拆出 =
 * 认知复杂度红线，也为了让"每个 POST 端点都过形状闸"这件事**只有一个落点**。
 *
 * 方法对着表里登记的那一档核一遍（原 §10 自述的「方法不判」盲区由此关闭）：httplib 已在
 * 传输层按注册方法过滤过，这条兜的是「不经 HTTP 直接喂 applier」的调用形态。
 *
 * 🔴 形状校验**只有 POST 查**：GET 端点在调用方那里根本没解析 body（留 null）。判据本体 =
 * 能力表生成的字段清单（`rgl/rpc_schema.lua`），而本处是服务端**唯一**调用点 ⟹ 新增端点
 * 「忘了接这道闸」这件事不可能发生（对比：把校验散进各处理函数，忘接是静默的）。
 * 未知字段在这里被响亮拒绝（批 2 拍板 5 的行为变更：以前静默忽略）。
 *
 * @param request 原始请求（取路径）
 * @param isPost 上线方法是不是 POST（同时决定要不要过形状闸）
 * @param body 已解析的请求体（仅 isPost 时有意义）
 * @param route 认领成功时写入命中的路由身份
 * @return 失败应答（404 / 400）；空 = 认领成功
 */
std::optional<RpcOutcome> rpcClaimRoute(const RpcRequest& request, bool isPost,
	const nlohmann::json& body, RpcRouteId& route) {
	const auto id = rpcRouteByPath(request.path);
	if (!id)
		return rpcFail(404, fmt::format("unknown route: {}", request.path));
	if (rpcRouteById(*id).method != (isPost ? RpcRouteMethod::post : RpcRouteMethod::get))
		return rpcFail(404, fmt::format("unknown route: {}", request.path));
	if (isPost) {
		if (auto err = rpcValidateRequest(*id, body); !err.empty())
			return rpcFail(400, err);
	}
	route = *id;
	return {};
}

/**
 * 分发（GET 与 POST 同一条路）：方法门 → body 解析 → **`rpcClaimRoute`（查表 + 形状闸）
 * → 穷尽 switch**。
 *
 * 🔴 为什么 switch 里**不写 default**：404 是「路径不在表里」，不是「表里某个 id 没处理」。
 * 把 404 塞进 default 会同时 ① 被本仓第一方启用的 `-Wcovered-switch-default` 拦下，
 * ② 更要命的是新增端点忘写分支时 default 把它吞成 404 —— 那条路由静默变成「存在但永远 404」。
 * 穷尽 switch 让编译器当场点名缺的是哪个 case。
 *
 * 句柄**按值**交给跨帧兑现的处理函数（`/shot`、`/shotburst`），不要改成 `std::move`：
 * 下面的 catch 还要用同一个句柄，move 过去就是空句柄（`resolve` = no-op）→ 异常请求永远
 * 不兑现，只能等传输层超时吐 504（症状 = "只有抛异常的请求 504"）。RpcResponder 是
 * shared_ptr 包装，拷贝廉价，且 `resolve` 幂等。
 *
 * json 异常（形状/类型不符）→ 400，其余 std::exception → 500——调试面不崩进程。
 * @param w 装配态
 * @param request 原始请求（方法 / 路径 / 体）
 * @param responder 响应句柄（/shot 转异步，由 rpcSettleShot 兑现）
 */
void rpcDispatch(RpcWiring& w, const RpcRequest& request, RpcResponder responder) {
	auto reply = [&](const RpcOutcome& outcome) {
		responder.resolve(outcome.status, outcome.body.dump());
	};
	try {
		// 方法先判：非 GET 非 POST 一律 405，连路径都不看（与重构前同序）
		const bool isPost = request.method == "POST";
		if (!isPost && request.method != "GET") {
			reply(rpcFail(405, fmt::format("method not allowed: {}", request.method)));
			return;
		}
		// POST 的 body 解析在分发之前（畸形 = 400；GET 端点无 body，留 null）
		nlohmann::json body;
		if (isPost) {
			body = request.body.empty()
			    ? nlohmann::json::object()
			    : nlohmann::json::parse(request.body, /*callback=*/nullptr, /*allow_exceptions=*/false);
			if (body.is_discarded()) {
				reply(rpcFail(400, "body is not valid JSON"));
				return;
			}
		}
		RpcRouteId route = RpcRouteId::routes;
		if (auto fail = rpcClaimRoute(request, isPost, body, route); fail) {
			reply(*fail);
			return;
		}
		switch (route) {
		case RpcRouteId::routes:
			responder.resolve(200, rpcRoutesJson(w.frame).dump());
			return;
		case RpcRouteId::state:
			reply(rpcHandleState(w));
			return;
		case RpcRouteId::camera:
			reply(rpcHandleCamera(w, body));
			return;
		case RpcRouteId::param:
			reply(rpcHandleParam(w, body));
			return;
		case RpcRouteId::shot:  // 应答跨帧兑现（句柄按值交出，见函数头）
			rpcHandleShot(w, body, responder);
			return;
		case RpcRouteId::shotburst:  // 同上，整批交齐才兑现
			rpcHandleShotBurst(w, body, responder);
			return;
		case RpcRouteId::resize:
			reply(rpcHandleResize(w, body));
			return;
		case RpcRouteId::pass:
			reply(rpcHandlePass(w, body));
			return;
		case RpcRouteId::decorate:  // 应答可跨帧兑现（句柄按值交出，见函数头）
			rpcHandleDecorate(w.renderer, w.runtime, w.frame, w.decorate, body, responder);
			return;
		case RpcRouteId::quit:  // body 恒可解析（上面已兜 400），本端点无参
			reply(rpcHandleQuit(w));
			return;
		}
	} catch (const nlohmann::json::exception& e) {
		responder.resolve(400, rpcErrorJson(e.what()).dump());
	} catch (const std::exception& e) {
		responder.resolve(500, rpcErrorJson(e.what()).dump());
	}
}

}  // namespace

/**
 * wireDebugRpc 定义（契约见 debug_rpc_exec.cppm 接口注释）。要点：
 * ① 门关 / bind 失败都返回 nullptr 且**不留半装配**（server 析构自兜底，未 start 早退）；
 * ② 先 setApplier 再 start（HTTP 线程一旦起跑就可能立即消费）；
 * ③ 帧边界闭包顺序固定 = 计数 → 结算上帧 /shot → drain 本帧新请求（结算先于
 *    收单，否则已就绪的捕获会被新 /shot 判 409 busy）。
 */
std::unique_ptr<DebugRpcServer> wireDebugRpc(entt::registry& registry, AssetTable& assets,
	Renderer& renderer, const RuntimeConfig& runtime, bool llmActive) {
	if (!diagnostics().rpc)
		return nullptr;  // 总开关默认关 = 零行为变化（红线），独立于 imgui 门

	auto server = std::make_unique<DebugRpcServer>();
	auto wiring = std::make_shared<RpcWiring>(registry, assets, renderer, runtime, llmActive);
	server->setApplier([wiring](const RpcRequest& request, RpcResponder responder) {
		rpcDispatch(*wiring, request, std::move(responder));
	});
	if (!server->start("127.0.0.1", static_cast<int>(diagnostics().rpcPort)))
		return nullptr;  // bind 失败（端口占用）已在 start 内日志 → 降级关，不崩

	auto* raw = server.get();
	std::function<void()> tick = [wiring, raw] {
		++wiring->frame;
		rpcSettleShot(*wiring);
		rpcSettleDecorate(wiring->renderer, wiring->decorate);  // 批 4：读上一帧安全点的事务结果定案
		rpcTickPassToggle(*wiring);  // B2-5：逐帧翻转在**帧边界**推进（HTTP 驱动做不到逐帧）
		raw->poll();
	};
	// 复用 ConfigReloadHook 帧边界（同 10.3/10.4 组合追加）：system 链之前 →
	// /camera、/param 本帧即生效；挂点闭包持 shared_ptr 装配态，与服务同寿命
	if (auto* hook = registry.ctx().find<ConfigReloadHook>(); hook && hook->poll) {
		auto prev = std::move(hook->poll);
		hook->poll = [prev = std::move(prev), tick = std::move(tick)] {
			prev();
			tick();
		};
	} else {
		registry.ctx().emplace<ConfigReloadHook>(ConfigReloadHook{.poll = std::move(tick)});
	}
	return server;
}

}  // namespace RGL
