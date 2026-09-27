module;
#include <SDL3/SDL.h>
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

namespace RGL {

namespace {

/** 后端 CheckVkResultFn：只记日志不 abort——UI 是诊断面，不得反杀主程序（VVL 另有报警）。 */
void checkVkResult(VkResult err) {
	if (err == VK_SUCCESS)
		return;
	if (auto* logger = RLLogger::getInstance())
		logger->error("imgui backend: VkResult={}", static_cast<int>(err));
}

}  // namespace

/** 析构兜底：未显式 shutdown（异常路径）也不泄漏 imgui context。 */
ImGuiLayer::~ImGuiLayer() { shutdown(); }

/** ImGuiLayer::init 定义（接口注释）：ctx → SDL3 后端 → Vulkan 后端（dynamic rendering）。 */
bool ImGuiLayer::init(AppContext& app, DeviceContext& device, Swapchain& swapchain, SDLWindow& window) {
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
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::StyleColorsDark();

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

/** 面板内容（批 10.1 骨架）：profiler 读数区 + 参数区占位（10.2 接注册表）。 */
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
	ImGui::TextUnformatted("参数控件占位（10.2b 灯/曝光 → 10.2c 材质，见 docs/phase10_plan.md）");
	ImGui::End();
}

}  // namespace RGL
