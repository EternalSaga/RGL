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
import asset_table;  // AssetTable（参数页构建）
import vk_renderer;  // Renderer（/shot 帧捕获 + writeCapturePng 宿主）
import llm_debug;    // writeCapturePng（/shot 落盘，同 main 的 llm 截图出口）
import runtime_config;  // LlmDebugConfig（writeCapturePng 入参，只需 file 字段）

namespace RGL {

namespace {

/** 协议应答结果（成功 = 应答体；失败 = HTTP 状态 + 错误体）。 */
struct RpcOutcome {
	int status = 200;
	nlohmann::json body;
};

/** 错误体（统一形状 `{"ok":false,"error":...}`；运输层错误体另见 debug_rpc_impl）。 */
nlohmann::json rpcErrorJson(std::string message) {
	return nlohmann::json{{"ok", false}, {"error", std::move(message)}};
}

/** 成功应答（端点自行补字段）。 */
RpcOutcome rpcOk(nlohmann::json body) { return RpcOutcome{200, std::move(body)}; }

/** 失败应答。 */
RpcOutcome rpcFail(int status, std::string message) {
	return RpcOutcome{status, rpcErrorJson(std::move(message))};
}

/** 直接兑现失败（不经 RpcOutcome 的早退路径：/shot 的 409 拒绝等）。 */
void rpcReject(const RpcResponder& responder, int status, std::string message) {
	responder.resolve(status, rpcErrorJson(std::move(message)).dump());
}

/** GET / 路由表（agent 据此发现端点；frame = 装配态帧计数，纯观测）。 */
nlohmann::json rpcRoutesJson(uint32_t frame) {
	return nlohmann::json{{"ok", true}, {"frame", frame},
		{"routes", {"GET /", "GET /state", "POST /camera", "POST /param", "POST /shot",
			"POST /shotburst", "POST /quit"}}};
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
 * 一页 → `entries` 数组（kind 与面板控件一一对应，`/param` 据 kind 校验 value 类型）。
 * 取值经条目 get 回调读源头（组件/ctx/材质表），故 /state 永远是当前真实值。
 * @param page 参数页
 * @return 条目数组（JSON）
 */
nlohmann::json rpcEntriesJson(const ParamPage& page) {
	nlohmann::json out = nlohmann::json::array();
	for (const auto& e : page.sliders)
		out.push_back({{"kind", "slider"}, {"label", e.label}, {"min", e.minValue},
			{"max", e.maxValue}, {"speed", e.speed}, {"value", e.get()}});
	for (const auto& e : page.intSliders)
		out.push_back({{"kind", "int"}, {"label", e.label}, {"min", e.minValue},
			{"max", e.maxValue}, {"value", e.get()}});
	for (const auto& e : page.colors) {
		const glm::vec3 v = e.get();
		out.push_back({{"kind", "color"}, {"label", e.label},
			{"value", {v.x, v.y, v.z}}});
	}
	for (const auto& e : page.drags) {
		const glm::vec3 v = e.get();
		out.push_back({{"kind", "drag3"}, {"label", e.label}, {"min", e.minValue},
			{"max", e.maxValue}, {"speed", e.speed}, {"value", {v.x, v.y, v.z}}});
	}
	for (const auto& t : page.texts)
		out.push_back({{"kind", "text"}, {"value", t.text()}});
	return out;
}

/**
 * 页内按 label 写入（label 页内唯一 = `param_semantics.json` 单一事实来源，
 * 这就是 D11 的寻址约定）。
 * @param page 目标页
 * @param label 条目标签（须来自 /state 发现）
 * @param value 新值（类型须与 kind 相符：slider/int = 数字，color/drag3 = 三维数组）
 * @return nullopt = 本页无此 label；有 = 已写入的应答（或类型不符的 400）
 */
std::optional<RpcOutcome> rpcWriteParamPage(const ParamPage& page, const std::string& label,
	const nlohmann::json& value) {
	auto byLabel = [&label](const auto& entry) { return entry.label == label; };
	auto ok = [&label, &value] { return rpcOk({{"ok", true}, {"label", label}, {"value", value}}); };

	if (auto it = std::ranges::find_if(page.sliders, byLabel); it != page.sliders.end()) {
		if (!value.is_number())
			return rpcFail(400, fmt::format("{}: value must be a number", label));
		it->set(value.get<float>());
		return ok();
	}
	if (auto it = std::ranges::find_if(page.intSliders, byLabel); it != page.intSliders.end()) {
		if (!value.is_number())
			return rpcFail(400, fmt::format("{}: value must be a number", label));
		it->set(static_cast<int>(value.get<double>()));  // 向零取整（离散 id 类，语义可接受）
		return ok();
	}
	if (auto it = std::ranges::find_if(page.colors, byLabel); it != page.colors.end()) {
		if (!value.is_array() || value.size() != 3)
			return rpcFail(400, fmt::format("{}: value must be [r,g,b]", label));
		it->set(glm::vec3{value.at(0).get<float>(), value.at(1).get<float>(),
			value.at(2).get<float>()});
		return ok();
	}
	if (auto it = std::ranges::find_if(page.drags, byLabel); it != page.drags.end()) {
		if (!value.is_array() || value.size() != 3)
			return rpcFail(400, fmt::format("{}: value must be [x,y,z]", label));
		it->set(glm::vec3{value.at(0).get<float>(), value.at(1).get<float>(),
			value.at(2).get<float>()});
		return ok();
	}
	return std::nullopt;
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
	std::string pendingShotFile;              // 期望 PNG 相对路径（gbuf 通道路径由写盘方定）
	uint32_t shotSeq = 0;                     // 缺省文件名序号（screenshots/rpc_<N>.png）
	uint32_t frame = 0;                       // 帧计数（/state 与根路由回显，纯观测）
};

/** GET /state：相机姿态 + 全部参数页（label 发现面，/param 按 label 寻址）。 */
RpcOutcome rpcHandleState(RpcWiring& w) {
	auto pages = buildDebugParamPages(w.registry, w.assets);
	nlohmann::json pageList = nlohmann::json::array();
	for (const auto& page : pages)
		pageList.push_back({{"name", page.name}, {"entries", rpcEntriesJson(page)}});
	return rpcOk({{"ok", true}, {"frame", w.frame},
		{"camera", rpcCameraJson(w.registry.ctx().get<Camera>())}, {"pages", std::move(pageList)}});
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
	return rpcOk({{"ok", true}, {"camera", rpcCameraJson(cam)}});
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
 * @brief 解析 /shotburst 的 `attachment` 字段（temporal-motion 的 taa_out 拒收掩膜连拍）。
 *
 * 白名单只两条：`swapchain`（= 不传，旧行为逐位不变）与 `taa_out`（具名附件回读）。
 * 抽成独立函数的原因：把 4 个校验分支挪出 handler，认知复杂度回到红线内。
 *
 * @param body 请求体
 * @param out  通过时写入附件名（未传 = 空串 = swapchain 路）
 * @return 错误消息（空 = 通过）
 */
std::string parseBurstAttachment(const nlohmann::json& body, std::string& out) {
	out.clear();
	if (!body.contains("attachment"))
		return {};
	if (body.contains("gbuf"))
		return "'attachment' and 'gbuf' are mutually exclusive";
	if (!body["attachment"].is_string())
		return "'attachment' must be a string";
	out = body["attachment"].get<std::string>();
	if (out.empty())
		return "'attachment' must be a non-empty name";
	if (out != "swapchain" && out != "taa_out")
		return "'attachment' only supports swapchain | taa_out";
	return {};
}

/**
 * POST /shotburst：登记「连续 N 帧」采集（应答在最后一张写盘后才兑现）。
 *
 * 与 `/shot` 的**本质**区别不是省了几次往返，而是连续性：`/shot` 每张的帧号取决于
 * 请求落在帧首钩子前还是后（= 墙钟），`/shotburst` 的「下一张」由帧首结算钩子自己登记
 * （见 `rpcSettleShot`），该钩子先于 drawFrame ⟹ 第 N、N+1、N+2… 帧逐张采集。
 * 时域差分（相邻帧差）只有后者测得准（`imgqa temporal`，docs/taa_plan.md §7 第 8 条）。
 *
 * @param w 装配态
 * @param body 请求体对象（`n` 必填 ≥1、`dir` 必填输出目录、`prefix?` 文件名前缀、
 *   `gbuf?` 回读通道、`attachment?` 具名附件回读（与 gbuf 互斥；temporal-motion 取
 *   taa_out alpha 的拒收掩膜连拍）；`n` 越界或类型错 = 400）
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
	if (!body.contains("n") || !body.at("n").is_number_integer() || body.at("n").get<int>() < 1
		|| body.at("n").get<int>() > 1024) {
		rpcReject(responder, 400, "'n' must be an integer in [1, 1024]");
		return;
	}
	if (!body.contains("dir") || !body.at("dir").is_string()
		|| body.at("dir").get<std::string>().empty()) {
		rpcReject(responder, 400, "'dir' must be a non-empty path");
		return;
	}
	w.burstDir = body.at("dir").get<std::string>();
	if (body.contains("prefix"))
		w.burstPrefix = body.at("prefix").get<std::string>();
	// attachment（temporal-motion 的 taa_out alpha 掩膜连拍）：与 gbuf 互斥，同 /shot 的约定。
	if (auto err = parseBurstAttachment(body, w.burstAttachment); !err.empty()) {
		rpcReject(responder, 400, err);
		return;
	}
	w.burstGbuf = body.contains("gbuf") ? body.at("gbuf").get<int>() : -1;
	w.burstRemaining = static_cast<uint32_t>(body.at("n").get<int>());
	w.burstIndex = 0;
	w.burstFiles.clear();

	if (w.burstAttachment.empty())
		w.renderer.requestFrameCapture(w.burstGbuf);  // 第一张 = 本帧 drawFrame 回读
	else
		w.renderer.requestFrameCapture(std::string_view(w.burstAttachment));
	w.pendingBurst = std::move(responder);
}

/**
 * /shot 跨帧结算（帧首、drain 之前调用）：捕获产物就绪 → 写 PNG → 兑现响应。
 * 未就绪 = 保持 pending（acquire 超时跳帧时请求保持 pending 重试，同 llm_debug）。
 *
 * **突发分支先走**（与单张互斥，不会两边都活）：写完一张就在**本钩子**里登记下一张——
 * 本钩子先于 drawFrame，所以登记立刻作用在本帧，这就是「连续 N 帧」的全部机制。
 * @param w 装配态
 */
void rpcSettleShot(RpcWiring& w) {
	if (w.pendingBurst) {
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
		++w.burstIndex;
		w.burstFiles.push_back(*file);
		if (--w.burstRemaining == 0) {
			auto responder = std::move(*w.pendingBurst);
			w.pendingBurst.reset();
			responder.resolve(200,
				rpcOk({{"ok", true}, {"count", w.burstIndex}, {"files", w.burstFiles}})
					.body.dump());
			return;
		}
		// 下一张 = 本帧（连续，不跨 HTTP）；回读源与首张同款选择
		if (w.burstAttachment.empty())
			w.renderer.requestFrameCapture(w.burstGbuf);
		else
			w.renderer.requestFrameCapture(std::string_view(w.burstAttachment));
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
