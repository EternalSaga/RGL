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
import debug_rpc_payload;  // B2-5：应答形状闸 + 参数页↔JSON 编解码（拆出去顶的是文件行数红线）
import asset_table;  // AssetTable（参数页构建）
import vk_renderer;  // Renderer（/shot 帧捕获 + writeCapturePng 宿主）
import vk_swapchain;  // B2-4③′：/resize 经 Renderer::getSwapchainRef 调 requestHeadlessExtent
import llm_debug;    // writeCapturePng（/shot 落盘，同 main 的 llm 截图出口）
import runtime_config;  // LlmDebugConfig（writeCapturePng 入参，只需 file 字段）

namespace RGL {

namespace {

/** GET / 路由表（agent 据此发现端点；frame = 装配态帧计数，纯观测）。 */
nlohmann::json rpcRoutesJson(uint32_t frame) {
	return nlohmann::json{{"ok", true}, {"frame", frame},
		{"routes", {"GET /", "GET /state", "POST /camera", "POST /param", "POST /shot",
			"POST /shotburst", "POST /resize", "POST /pass", "POST /quit"}}};
}

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
 * @param key 字段名
 * @return 三维值
 */
glm::vec3 rpcVec3(const nlohmann::json& body, const char* key) {
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
	 *  @param llmRunning llm 截图模式在跑（/shot 拒 409） */
	RpcWiring(entt::registry& rg, AssetTable& assetTable, Renderer& rendererRef, bool llmRunning)
		: registry(rg), assets(assetTable), renderer(rendererRef), llmActive(llmRunning) {}

	entt::registry& registry;
	AssetTable& assets;
	Renderer& renderer;
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
		}}});
}

/**
 * `mode` 字段套用（仅 rpcHandleCamera 内部使用）：trackball/fps 两档，缺字段不改，
 * 未知值 → nullopt 交调用方转 400。
 * @param body 请求体对象
 * @param cam 写入目标（仅命中已知值时改）
 * @return 已知 mode = true；未知 mode = false
 */
bool rpcApplyCameraMode(const nlohmann::json& body, Camera& cam) {
	if (!body.contains("mode"))
		return true;
	const std::string mode = body.at("mode").get<std::string>();
	if (mode == "trackball")
		cam.mode = CameraMode::Trackball;
	else if (mode == "fps")
		cam.mode = CameraMode::Fps;
	else
		return false;
	return true;
}

/**
 * `yaw_at_frame` 套用（债-89 机位锚点；从 `rpcHandleCamera` 拆出 = 认知复杂度红线）。
 *
 * 为什么必须在引擎侧（客户端预补偿必然失败）：`frame_yaw` 下写入落帧由 HTTP 时序决定，
 * 本机 headless ~6000fps 下落帧抖动 ±36 帧量级 = 0.125px/帧 × 36 ≈ 4.5px，与要消的噪声
 * 同源——只有把机位基准解成**绝对帧号的函数**（`frameDrivenYawPin`）才与落帧无关。
 * @param body 请求体
 * @param frameNow 引擎当前帧计数（`RpcWiring::frame`）
 * @param cam 已套用完本请求其他字段的相机（锚的是**套用后**的朝向）
 * @param pinned 通过时写入锚点帧号（0 = 本请求没带该字段）
 * @return 失败应答（形状错 = 400、锚已过 = 409）；空 = 通过
 */
std::optional<RpcOutcome> rpcApplyYawAnchor(const nlohmann::json& body, uint32_t frameNow,
	Camera& cam, uint32_t& pinned) {
	pinned = 0;
	if (!body.contains("yaw_at_frame"))
		return {};
	const auto& v = body.at("yaw_at_frame");
	if (!v.is_number_integer() || v.get<int64_t>() < 1 || v.get<int64_t>() > 0xFFFFFFFFLL)
		return rpcFail(400, "'yaw_at_frame' must be an integer in [1, 2^32)");
	const uint32_t n = static_cast<uint32_t>(v.get<int64_t>());
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
 * @param body 请求体对象
 * @return 套用后姿态回显
 */
RpcOutcome rpcHandleCamera(RpcWiring& w, const nlohmann::json& body) {
	if (!body.is_object())
		return rpcFail(400, "body must be a JSON object");
	auto& cam = w.registry.ctx().get<Camera>();
	if (!rpcApplyCameraMode(body, cam))
		return rpcFail(400, "mode must be \"trackball\" or \"fps\"");
	// position 必须先于 target：fps 下的朝向反推要用到新 position
	if (body.contains("position")) cam.position = rpcVec3(body, "position");
	if (body.contains("target")) {
		// fps 模式的 view 只看 yaw/pitch（cameraView），照直写 target 是死写入——回显会
		// 显示"已生效"而画面不动。故 fps 下（未显式给 yaw/pitch 时）走 cameraAimAt 反推，
		// 与 camera_config 初始机位同一约定。显式给了 yawDegrees/pitchDegrees 则尊重显式值。
		const bool explicitAngles = body.contains("yawDegrees") || body.contains("pitchDegrees");
		if (cam.mode == CameraMode::Fps && !explicitAngles)
			cameraAimAt(cam, rpcVec3(body, "target"));
		else
			cam.target = rpcVec3(body, "target");
	}
	if (body.contains("up")) cam.up = rpcVec3(body, "up");
	if (body.contains("yawDegrees")) cam.yaw = glm::radians(body.at("yawDegrees").get<float>());
	if (body.contains("pitchDegrees"))
		cam.pitch = glm::radians(body.at("pitchDegrees").get<float>());
	if (body.contains("fovDegrees")) cam.fov = glm::radians(body.at("fovDegrees").get<float>());
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
 * @param w 装配态
 * @param body 请求体对象
 * @return 写入回显；label 未知 → 404
 */
RpcOutcome rpcHandleParam(RpcWiring& w, const nlohmann::json& body) {
	if (!body.is_object())
		return rpcFail(400, "body must be a JSON object");
	const std::string label = body.at("label").get<std::string>();  // 缺/错类型 → 400
	if (body.contains("material"))
		w.registry.ctx().get<DebugPanelState>().materialSel =
			static_cast<uint32_t>(std::max(0, body.at("material").get<int>()));
	const auto& value = body.at("value");
	auto pages = buildDebugParamPages(w.registry, w.assets);

	if (body.contains("page")) {
		const std::string name = body.at("page").get<std::string>();
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
 * @param w 装配态
 * @param body 请求体对象（`file?` 相对路径、`gbuf?` G-buffer 通道、`attachment?` 附件名
 *   ——后两者互斥，同时给 = 400）
 * @param responder 跨帧响应句柄（移交给 w.pendingShot）
 */
void rpcHandleShot(RpcWiring& w, const nlohmann::json& body, RpcResponder responder) {
	if (!body.is_object()) {
		rpcReject(responder, 400, "body must be a JSON object");
		return;
	}
	if (w.llmActive) {
		rpcReject(responder, 409, "/shot unavailable: llm_debug capture mode is running");
		return;
	}
	if (w.pendingShot) {
		rpcReject(responder, 409, "another /shot is still pending");
		return;
	}
	std::string file;
	if (body.contains("file"))
		file = body.at("file").get<std::string>();
	int gbuf = -1;
	if (body.contains("gbuf"))
		gbuf = body.at("gbuf").get<int>();
	// attachment（批 12.2）：按名捕获，QA 层取 object_id 全分辨率。与 gbuf 互斥——
	// 两者都是"回读哪个源"，同时给必是客户端 bug，静默取一个会拍出用户没要的那张图。
	if (body.contains("attachment")) {
		if (body.contains("gbuf")) {
			rpcReject(responder, 400, "'attachment' and 'gbuf' are mutually exclusive");
			return;
		}
		const auto name = body.at("attachment").get<std::string>();
		if (name.empty()) {
			rpcReject(responder, 400, "'attachment' must be a non-empty name");
			return;
		}
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
 * @param body 请求体对象（`n` 必填 ≥1、`dir` 必填输出目录、`prefix?` 文件名前缀、
 *   `gbuf?` 回读通道、`attachment?` 具名附件回读（与 gbuf 互斥；temporal-motion 取
 *   taa_out alpha 的拒收掩膜连拍）、`start_frame?` **绝对帧号锚点**（债-89：首张保证落在
 *   第 N 帧；已过 = 409 响亮失败，绝不静默退回相对）；`n` 越界或类型错 = 400）
 * @param responder 跨帧响应句柄（移交给 w.pendingBurst）
 */
void rpcHandleShotBurst(RpcWiring& w, const nlohmann::json& body, RpcResponder responder) {
	if (!body.is_object()) {
		rpcReject(responder, 400, "body must be a JSON object");
		return;
	}
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
 * @param body {"width": N, "height": M}
 */
RpcOutcome rpcHandleResize(RpcWiring& w, const nlohmann::json& body) {
	const auto width = body.at("width").get<uint32_t>();
	const auto height = body.at("height").get<uint32_t>();
	if (width == 0 || height == 0)
		return rpcFail(400, "resize: width/height must be > 0");
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
 * @param w 装配态
 * @param body 见上
 */
RpcOutcome rpcHandlePass(RpcWiring& w, const nlohmann::json& body) {
	if (!body.contains("name"))
		return rpcFail(400,
			"pass: 需要 \"name\"（装配期登记过、可动态开关的 pass 名；见 /state 的 graph.dynamicPasses）");
	const std::string name = body.at("name").get<std::string>();
	if (body.contains("toggle_frames")) {
		const auto frames = body.at("toggle_frames").get<uint32_t>();
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
	if (!body.contains("on"))
		return rpcFail(400, "pass: 需要 \"on\"(bool) 或 \"toggle_frames\"(uint)");
	const bool on = body.at("on").get<bool>();
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
 * POST 路由（`/camera`、`/param`、`/shot`、`/quit`；body 解析在此，畸形 = 400）。
 * 异常由 rpcDispatch 的 try 统一转 400/500 —— 本函数在它的 try 内被调用。
 * @param w 装配态
 * @param request 原始请求
 * @param responder 响应句柄
 */
void rpcDispatchPost(RpcWiring& w, const RpcRequest& request, RpcResponder responder) {
	auto reply = [&](const RpcOutcome& outcome) {
		responder.resolve(outcome.status, outcome.body.dump());
	};
	const nlohmann::json body = request.body.empty()
		? nlohmann::json::object()
		: nlohmann::json::parse(request.body, /*callback=*/nullptr, /*allow_exceptions=*/false);
	if (body.is_discarded()) {
		reply(rpcFail(400, "body is not valid JSON"));
		return;
	}
	if (request.path == "/camera") {
		reply(rpcHandleCamera(w, body));
		return;
	}
	if (request.path == "/param") {
		reply(rpcHandleParam(w, body));
		return;
	}
	if (request.path == "/shot") {
		rpcHandleShot(w, body, std::move(responder));
		return;
	}
	if (request.path == "/shotburst") {
		rpcHandleShotBurst(w, body, std::move(responder));
		return;
	}
	if (request.path == "/quit") {
		// body 恒可解析（上面已兜 400），本端点无参
		reply(rpcHandleQuit(w));
		return;
	}
	if (request.path == "/resize") {
		reply(rpcHandleResize(w, body));
		return;
	}
	if (request.path == "/pass") {
		reply(rpcHandlePass(w, body));
		return;
	}
	reply(rpcFail(404, fmt::format("unknown route: {}", request.path)));
}

/**
 * applier 实体（传输层每个请求调一次，帧边界渲染线程）：方法/路径分发 → 兑现。
 * json 异常（形状/类型不符）→ 400，其余 std::exception → 500——调试面不崩进程。
 *
 * `rpcDispatchPost` 必须**拷贝**传句柄：它是按值收参的另一层 catch 不在场的分发点，
 * 若此处 move，下面 catch 拿到的就是空句柄（`resolve` = no-op）→ 异常请求永远不兑现，
 * 只能等传输层超时吐 504（症状 = "只有抛异常的请求 504"）。shared_ptr 拷贝廉价，勿改回 move。
 *
 * @param w 装配态
 * @param request 原始请求（方法 / 路径 / 体）
 * @param responder 响应句柄（/shot 转异步，由 rpcSettleShot 兑现）
 */
void rpcDispatch(RpcWiring& w, const RpcRequest& request, RpcResponder responder) {
	auto reply = [&](const RpcOutcome& outcome) {
		responder.resolve(outcome.status, outcome.body.dump());
	};
	try {
		if (request.method == "POST") {
			rpcDispatchPost(w, request, responder);  // 拷贝：本函数 catch 还要用（见上）
			return;
		}
		if (request.method != "GET") {
			reply(rpcFail(405, fmt::format("method not allowed: {}", request.method)));
			return;
		}
		if (request.path == "/") {
			responder.resolve(200, rpcRoutesJson(w.frame).dump());
			return;
		}
		if (request.path != "/state") {
			reply(rpcFail(404, fmt::format("unknown route: {}", request.path)));
			return;
		}
		reply(rpcHandleState(w));
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
	Renderer& renderer, bool llmActive) {
	if (!diagnostics().rpc)
		return nullptr;  // 总开关默认关 = 零行为变化（红线），独立于 imgui 门

	auto server = std::make_unique<DebugRpcServer>();
	auto wiring = std::make_shared<RpcWiring>(registry, assets, renderer, llmActive);
	server->setApplier([wiring](const RpcRequest& request, RpcResponder responder) {
		rpcDispatch(*wiring, request, std::move(responder));
	});
	if (!server->start("127.0.0.1", static_cast<int>(diagnostics().rpcPort)))
		return nullptr;  // bind 失败（端口占用）已在 start 内日志 → 降级关，不崩

	auto* raw = server.get();
	std::function<void()> tick = [wiring, raw] {
		++wiring->frame;
		rpcSettleShot(*wiring);
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
