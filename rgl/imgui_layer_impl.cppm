module;
#include <SDL3/SDL.h>
#include <glm/glm.hpp>  // ParamPage 条目 get/set 用 glm::vec3（Color3/Drag3）
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

module imgui_layer:impl;

import imgui_layer;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import rllogger;
import vk_instance;
import vk_device;
import vk_swapchain;
import vulkan_window;
import profiler;  // 面板读数 = profilerSnapshotNow（Phase 10 批 10.1）
import param_registry;  // ParamPage/条目渲染（Phase 10 批 10.2b）

namespace RGL {

namespace {

/** 后端 CheckVkResultFn：只记日志不 abort——UI 是诊断面，不得反杀主程序（VVL 另有报警）。 */
void checkVkResult(VkResult err) {
	if (err == VK_SUCCESS)
		return;
	if (auto* logger = RLLogger::getInstance())
		logger->error("imgui backend: VkResult={}", static_cast<int>(err));
}

/**
 * 键盘吞没可观测（防御层，2026-09-28 "面板吞 WASD" 配套）：调试层吞键事件必须
 * 可查。分类：NavActive（键盘导航焦点、无主动交互）= 意外捕获 → warn（事故症状
 * 形态，恒真吞键只可能出自导航/模态误配置）；主动控件交互（滑条拖拽等，g.ActiveId
 * 非空）= 预期 → debug。节流：吞没段内至多 2s 一行，段结束（有键事件放行）复位
 * 后下一段重新上报。
 * @param swallowed 本键事件是否被吞（true = WantCaptureKeyboard）
 * @param io imgui IO（NavActive 区分捕获原因）
 */
void noteKeyEventCapture(bool swallowed, const ImGuiIO& io) {
	static uint32_t lastTicks = 0;
	static bool episodeActive = false;
	if (!swallowed) {
		episodeActive = false;
		return;
	}
	auto* logger = RLLogger::getInstance();
	if (!logger)
		return;
	const uint32_t now = SDL_GetTicks();
	if (episodeActive && now - lastTicks < 2000)
		return;
	episodeActive = true;
	lastTicks = now;
	if (io.NavActive)
		logger->warn("imgui: 键盘事件被吞没（NavActive 键盘导航焦点、无主动交互）——相机 "
			"WASD 期间不响应；若未在操作面板即为捕获回归（docs/调试历史.md 2026-09-28）");
	else
		logger->debug("imgui: 键盘事件被吞没（主动控件交互，预期）");
}

// ─── 面板字体（Phase 10b 批 11b.1；docs/phase10b_plan.md §2/§4）──────────────────

/**
 * 系统字体探测链（D2，命中即停；仅运行时读系统字体，**不进仓库**）：
 * 用户指定 `ui_font` 前置 → **MiSans Light 首选（用户指定）** → MiSans Regular →
 * Noto Sans CJK（Debian / Fedora-Arch 两路径；CFF 轮廓 = 必须开 `imgui[freetype]`，
 * 否则加载即失败）→ 文泉驿正黑 → Windows 微软雅黑/黑体（msyh 是 TrueType，两轮廓
 * FreeType 都吃）。全 miss = 回落 imgui 内建 13px 字体（中文仍问号，只 warn 不崩）。
 */
constexpr const char* kUiFontCandidates[] = {
	"/usr/share/fonts/misans/MiSans-Light.ttf",
	"/usr/share/fonts/misans/MiSans-Regular.ttf",
	"/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
	"/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
	"C:/Windows/Fonts/msyh.ttc",
	"C:/Windows/Fonts/simhei.ttf",
};

/**
 * 加载面板字体到默认图集（首个成功字体 = 默认字体，imgui 1.92 按需烘焙字形，
 * **无需指定 glyph ranges**——GetGlyphRanges* 已 obsolete，D4）。
 *
 * `ImFontConfig::Flags |= ImFontFlags_NoLoadError` 是探测的前提：缺文件/读失败时
 * imgui 默认 `IM_ASSERT_USER_ERROR`（Debug 构建 = abort），带此 flag 则返回 NULL
 * 由本函数继续试下一个候选（契约：调用方检查返回值）。
 *
 * @param io imgui IO（Fonts = 默认图集）
 * @param configuredPath configs/runtime.lua 的 ui_font（空 = 直接走探测链）
 * @param fontSize 字号 px（非正数回落 18；写进 ImFont::LegacySize，1.92 由
 *   `style.FontSizeBase <= 0` 分支采纳为渲染字号）
 * @param logger 日志（显式路径失败 warn / 命中 info / 全 miss warn）
 * @return true = 图集里加进了某个字体
 */
bool loadUiFont(ImGuiIO& io, std::string_view configuredPath, float fontSize,
	RLLogger* logger) {
	const float size = fontSize > 0.0f ? fontSize : 18.0f;
	ImFontConfig cfg;
	cfg.Flags |= ImFontFlags_NoLoadError;
	auto tryPath = [&io, size, &cfg](std::string_view path) -> ImFont* {
		if (path.empty())
			return nullptr;
		return io.Fonts->AddFontFromFileTTF(std::string(path).c_str(), size, &cfg);
	};

	ImFont* font = tryPath(configuredPath);
	std::string hit;
	if (font) {
		hit = configuredPath;
	} else {
		if (!configuredPath.empty() && logger)
			logger->warn("imgui: ui_font 指定字体加载失败（{}），转系统探测链",
				std::string(configuredPath));
		for (const char* candidate : kUiFontCandidates) {
			font = tryPath(candidate);
			if (font) {
				hit = candidate;
				break;
			}
		}
	}
	if (!font) {
		if (logger)
			logger->warn("imgui: 系统字体全 miss（ui_font=\"{}\"），回落内建字体——"
				"非 ASCII 文本显示为问号", std::string(configuredPath));
		return false;
	}
	if (logger)
		logger->info("imgui: 面板字体 {} @ {:.0f}px", hit, size);
	return true;
}

// ─── 参数条目 widget（Phase 10 批 10.2b）：每帧从源同步临时值 → widget → 编辑才写回
//（get 每帧跑 = 配置热重载/外部改动自动反映）；label 帧间稳定 = imgui id 稳定 ───

/** 标量滑条（SliderEntry）。 */
void renderSlider(const SliderEntry& e) {
	float v = e.get();
	if (ImGui::SliderFloat(e.label.c_str(), &v, e.minValue, e.maxValue, "%.3f"))
		e.set(v);
}

/** 颜色编辑（Color3Entry，HDR 线性域）。 */
void renderColor3(const Color3Entry& e) {
	const glm::vec3 c = e.get();
	float rgb[3] = {c.x, c.y, c.z};
	if (ImGui::ColorEdit3(e.label.c_str(), rgb))
		e.set(glm::vec3(rgb[0], rgb[1], rgb[2]));
}

/** 三维拖拽（Drag3Entry）。 */
void renderDrag3(const Drag3Entry& e) {
	const glm::vec3 v3 = e.get();
	float arr[3] = {v3.x, v3.y, v3.z};
	if (ImGui::DragFloat3(e.label.c_str(), arr, e.speed, e.minValue, e.maxValue, "%.3f"))
		e.set(glm::vec3(arr[0], arr[1], arr[2]));
}

/** 整型滑条（SliderIntEntry）。 */
void renderSliderInt(const SliderIntEntry& e) {
	int v = e.get();
	if (ImGui::SliderInt(e.label.c_str(), &v, e.minValue, e.maxValue))
		e.set(v);
}

/** 只读文本行（TextEntry，次要色）。 */
void renderText(const TextEntry& e) {
	ImGui::TextDisabled("%s", e.text().c_str());
}

/** 单页条目渲染（五类 widget 顺序：float/int 滑条、颜色、三维拖拽、文本）。 */
void renderPageEntries(const ParamPage& page) {
	for (const SliderEntry& e : page.sliders)
		renderSlider(e);
	for (const SliderIntEntry& e : page.intSliders)
		renderSliderInt(e);
	for (const Color3Entry& e : page.colors)
		renderColor3(e);
	for (const Drag3Entry& e : page.drags)
		renderDrag3(e);
	for (const TextEntry& e : page.texts)
		renderText(e);
}

}  // namespace

/** 析构兜底：未显式 shutdown（异常路径）也不泄漏 imgui context。 */
ImGuiLayer::~ImGuiLayer() { shutdown(); }

/** ImGuiLayer::init 定义（接口注释）：ctx → 字体（探测链）→ SDL3 后端 → Vulkan 后端（dynamic rendering）。 */
bool ImGuiLayer::init(AppContext& app, DeviceContext& device, Swapchain& swapchain, SDLWindow& window,
	std::string_view fontPath, float fontSize) {
	auto* logger = RLLogger::getInstance();
	if (initialized)
		return true;

	ImGuiContext* ctx = ImGui::CreateContext();
	if (!ctx) {
		if (logger)
			logger->error("imgui: CreateContext 失败，面板关闭");
		return false;
	}
	ImGui::SetCurrentContext(ctx);
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // vcpkg docking-experimental 构建
	// 键盘捕获红线（勿加回 NavEnableKeyboard，强制清除见 init 尾部守卫，2026-09-28
	// "面板吞 WASD" 根因链见 docs/调试历史.md）：NavEnableKeyboard ⇒ 面板点击一次后
	// g.NavWindow 常驻 ⇒ io.NavActive 恒真 ⇒ io.WantCaptureKeyboard 恒真（imgui 1.92
	// 语义）⇒ eventSystem 的 InputFilter 吞掉全部键事件 ⇒ fps 模式 WASD 移动失效。
	// 本面板无 InputText/键盘导航需求 → 键盘捕获只允许发生在"主动交互"期间
	//（滑条拖拽等，g.ActiveId 非空）。
	ImGui::StyleColorsDark();

	// 面板字体（Phase 10b 批 11b.1）：图集首字体 = 默认字体；1.92 字形按需烘焙，
	// 字体加载失败只回落内建字体（loadUiFont 内已 warn），不阻断后端 init。
	loadUiFont(io, fontPath, fontSize, logger);

	if (!ImGui_ImplSDL3_InitForVulkan(static_cast<SDL_Window*>(window))) {
		if (logger)
			logger->error("imgui: SDL3 后端初始化失败，面板关闭");
		ImGui::DestroyContext(ctx);
		return false;
	}

	// 后端 init：dynamic rendering（项目全 dynamic，无 VkRenderPass）。
	// pColorAttachmentFormats 指向成员 uiColorFormat（后端持有指针，生命周期契约）。
	uiColorFormat = static_cast<VkFormat>(swapchain.getFormat().format);
	ImGui_ImplVulkan_InitInfo info{};
	info.ApiVersion = VK_API_VERSION_1_3;
	info.Instance = static_cast<VkInstance>(*app.getInstance());
	info.PhysicalDevice = static_cast<VkPhysicalDevice>(*device.getPhysicalDevice());
	info.Device = static_cast<VkDevice>(*device.getDevice());
	info.QueueFamily = device.getGraphicsQueueFamily();
	info.Queue = static_cast<VkQueue>(device.getGraphicsQueue());
	info.DescriptorPool = VK_NULL_HANDLE;
	info.DescriptorPoolSize = 32;  // 后端自建池（与项目 descriptor heap 互不相干）
	info.MinImageCount = static_cast<uint32_t>(swapchain.getImages().size());
	info.ImageCount = info.MinImageCount;
	info.PipelineCache = VK_NULL_HANDLE;
	info.PipelineInfoMain.RenderPass = VK_NULL_HANDLE;
	info.PipelineInfoMain.Subpass = 0;
	info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	info.PipelineInfoMain.PipelineRenderingCreateInfo.sType =
		VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
	info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &uiColorFormat;
	info.PipelineInfoMain.PipelineRenderingCreateInfo.depthAttachmentFormat = VK_FORMAT_UNDEFINED;
	info.UseDynamicRendering = true;
	info.Allocator = nullptr;
	info.CheckVkResultFn = &checkVkResult;
	info.MinAllocationSize = 1024 * 1024;  // VVL 友好（字体上传块）
	if (!ImGui_ImplVulkan_Init(&info)) {
		if (logger)
			logger->error("imgui: Vulkan 后端初始化失败，面板关闭");
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext(ctx);
		return false;
	}

	// 键盘捕获红线守卫（防御性编程，置于 init 尾部 = 无论 flag 在何处被设置都能拦截；
	// 2026-09-28 "面板吞 WASD" 根因链见 init 头部注释与 docs/调试历史.md）：
	// NavEnableKeyboard 会让面板点击后 WantCaptureKeyboard 恒真 → InputFilter 吞掉
	// 全部键事件 → fps 模式 WASD 失效。本面板无键盘导航需求，违反即强制清除。
	if (io.ConfigFlags & ImGuiConfigFlags_NavEnableKeyboard) {
		if (logger)
			logger->error("imgui: ImGuiConfigFlags_NavEnableKeyboard 已强制清除——它会令 "
				"WantCaptureKeyboard 恒真（NavActive）吞掉相机 WASD 输入（根因见 "
				"docs/调试历史.md 2026-09-28）");
		io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
	}

	initialized = true;
	if (logger)
		logger->info("imgui: 调试面板就绪（dynamic rendering，swapchain images={}）",
			info.ImageCount);
	return true;
}

/** ImGuiLayer::shutdown 定义（接口注释）：后端逆序反初始化，幂等。 */
void ImGuiLayer::shutdown() {
	if (!initialized)
		return;
	initialized = false;
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplSDL3_Shutdown();
	ImGui::DestroyContext();
}

/** ImGuiLayer::handleEvent 定义（接口注释）：WantCapture 门；窗口生命周期事件恒放行。 */
bool ImGuiLayer::handleEvent(const void* sdlEvent) {
	if (!initialized || !sdlEvent)
		return false;
	const auto* event = static_cast<const SDL_Event*>(sdlEvent);
	ImGui_ImplSDL3_ProcessEvent(event);
	ImGuiIO& io = ImGui::GetIO();
	switch (event->type) {
		case SDL_EVENT_MOUSE_MOTION:
		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
		case SDL_EVENT_MOUSE_WHEEL:
			return io.WantCaptureMouse;
		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
		case SDL_EVENT_TEXT_INPUT:
			noteKeyEventCapture(io.WantCaptureKeyboard, io);  // 吞键可观测（防御层）
			return io.WantCaptureKeyboard;
		default:
			return false;  // QUIT/RESIZE/FOCUS…：退出与窗口路径不受 UI 劫持
	}
}

/** ImGuiLayer::renderFrame 定义（接口注释）：空 draw data 早退（不浪费 begin/end）。 */
bool ImGuiLayer::renderFrame(vk::raii::CommandBuffer& cmd, vk::Extent2D extent,
	vk::ImageView swapchainView) {
	if (!initialized)
		return false;
	ImGui_ImplSDL3_NewFrame();
	ImGui_ImplVulkan_NewFrame();
	ImGui::NewFrame();
	buildPanels();
	ImGui::Render();
	ImDrawData* drawData = ImGui::GetDrawData();
	if (!drawData || !drawData->Valid || drawData->CmdLists.empty())
		return false;

	// UI = load 语义叠加在场景之上；attachment 结构可栈上（vkCmdBeginRendering 即拷贝）
	VkRenderingAttachmentInfo color{};
	color.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
	color.imageView = swapchainView;
	color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	VkRenderingInfo rendering{};
	rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
	rendering.renderArea = {{0, 0}, {extent.width, extent.height}};
	rendering.layerCount = 1;
	rendering.colorAttachmentCount = 1;
	rendering.pColorAttachments = &color;
	::vkCmdBeginRendering(*cmd, &rendering);
	ImGui_ImplVulkan_RenderDrawData(drawData, *cmd);
	::vkCmdEndRendering(*cmd);
	return true;
}

/** 面板内容（接口注释）：profiler 读数区 + 参数页 TabBar（builder 非空时）。 */
void ImGuiLayer::buildPanels() {
	const ProfilerSnapshot snap = profilerSnapshotNow();
	ImGui::Begin("RGL Debug");
	if (!snap.enabled) {
		ImGui::TextUnformatted("profiler = 0（configs/diagnostics.lua 设 profiler=N 出读数）");
	} else {
		ImGui::Text("frame %llu / 窗口样本 %u 帧",
			static_cast<unsigned long long>(snap.frame), snap.sampleFrames);
		ImGui::Text("frame      last %.2f ms   avg %.2f ms", snap.lastFrameMs, snap.frameAvgMs);
		ImGui::Text("cpu.record avg %.2f ms", snap.cpuRecordAvgMs);
		ImGui::Text("gpu.frame  avg %.2f ms", snap.gpuFrameAvgMs);
	}
	ImGui::Separator();
	buildParamTabs();
	ImGui::End();
}

/** 渲染参数页（契约 = param_registry）：TabItem per page；非当前页不渲染
 *  （条目回调不跑 = 零副作用）。 */
void ImGuiLayer::buildParamTabs() {
	if (!pageBuilder)
		return;
	const std::vector<ParamPage> pages = pageBuilder();
	if (pages.empty())
		return;
	if (!ImGui::BeginTabBar("rgl_params"))
		return;
	for (const ParamPage& page : pages) {
		// 批 10.7：点选跳转——构建器给「选中」页标了 selectOnPick（一次性请求，
		// 构建器已清零）。SetSelected 让 imgui 本帧切到该页并**打断**用户当前所在
		// 页（点选后立刻看到该物体的材质控件，是 usability 诉求本身）。
		const ImGuiTabItemFlags flags = page.selectOnPick
			? ImGuiTabItemFlags_SetSelected
			: ImGuiTabItemFlags_None;
		if (!ImGui::BeginTabItem(page.name.c_str(), nullptr, flags))
			continue;  // 非当前页不渲染（条目回调不跑 = 零副作用）
		renderPageEntries(page);
		ImGui::EndTabItem();
	}
	ImGui::EndTabBar();
}

}  // namespace RGL
