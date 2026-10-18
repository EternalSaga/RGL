module;

export module imgui_layer;

import rlstd;
import rlvk;
import vk_instance;   // AppContext（instance 句柄 → 后端 init）
import vk_device;     // DeviceContext（device/queue/family → 后端 init）
import vk_swapchain;  // Swapchain（color format + imageCount → 后端 init）
import vulkan_window; // SDLWindow（sdl3-binding 挂窗）
import param_registry;  // ParamPage（参数页构建器返回类型，Phase 10 批 10.2b）

namespace RGL {

/**
 * IMGUI 调试面板层（Phase 10 批 10.1 骨架；docs/archive/phase10_plan.md §4）。
 *
 * 装配位置 = **上层调试胶水**（与 llm_debug 同层，允许认识 ECS/renderer；反向禁止）：
 * main 在 `!headless && diagnostics().imgui` 时构造本层并把
 * `handleEvent` 挂进 registry context 的 InputFilter、`renderFrame` 挂进
 * Renderer 帧末扩展段 hook——imgui.h include 只活在本模块 impl 的 GMF，
 * 接口与引擎其余模块零 imgui 依赖。
 *
 * 后端形态（imgui 1.92.8，vcpkg docking-experimental + sdl3-binding + vulkan-binding）：
 * dynamic rendering 路（PipelineRenderingCreateInfo 以 swapchain color format 建
 * 主 pipeline，无 VkRenderPass）；描述符池后端内部自建（与项目 descriptor heap
 * 互不相干）。UI = 帧末独立渲染段：调用方（Renderer hook）负责把 swapchain 推进
 * COLOR_ATTACHMENT_OPTIMAL，本层 beginRendering(load) → 画 → endRendering，
 * 布局追踪表由调用方收口（契约见 vk_renderer setFrameEndHook 注释）。
 *
 * 生命周期限制（v1 已知）：swapchain recreate 若改 format/imageCount 需
 * shutdown+init 重建本层（本机 resize 不改这两项，暂不自动处理，见计划 §4）。
 */
export class ImGuiLayer {
  public:
	/** 默认构造（未 init 态 = 各方法全早退，可安全闲置）。 */
	ImGuiLayer() = default;
	/** 析构幂等收口（未 init = 空转；防 main 异常路径泄漏 ctx）。 */
	~ImGuiLayer();

	/** 禁拷贝构造（本层持有 imgui 全局 context 状态，进程内唯一）。 */
	ImGuiLayer(const ImGuiLayer&) = delete;
	/** @see 禁拷贝构造（本层持有 imgui 全局 context 状态，进程内唯一）。 */
	ImGuiLayer& operator=(const ImGuiLayer&) = delete;

	/**
	 * 建 imgui context + SDL3/Vulkan 后端（main 装配唯一调用点，帧循环开始前）。
	 * 任一步失败 = 回滚已建部分 + 日志 + false（调用方按"无 UI 继续跑"降级）。
	 * 字体在 context 建成后、后端 init 前加载（探测链见 impl kUiFontCandidates；
	 * docs/archive/phase10b_plan.md 批 11b.1）。
	 * @param app 实例层（instance 句柄）
	 * @param device 设备层（device/graphics queue/family）
	 * @param swapchain 交换链（color format + imageCount）
	 * @param window SDL 窗口（sdl3-binding 事件挂钩）
	 * @param fontPath 面板字体路径（configs runtime.lua ui_font；空 = 自动探测链）
	 * @param fontSize 面板字号 px（非正数 = 回落 18）
	 * @return 成功 = active() 变 true
	 */
	bool init(AppContext& app, DeviceContext& device, Swapchain& swapchain, SDLWindow& window,
		std::string_view fontPath = {}, float fontSize = 18.0f);

	/** 后端反初始化 + DestroyContext（幂等；main 正常路径显式调，析构兜底）。 */
	void shutdown();

	/** @return 后端是否就绪（未就绪 = handleEvent/renderFrame 全早退）。 */
	[[nodiscard]] bool active() const { return initialized; }

	/**
	 * SDL 事件先手过滤器（InputFilter 挂载点；参数 = InputFilter 类型擦除约定，
	 * 实指 const SDL_Event*，见 ecs.cppm）。喂后端后按 WantCapture 判定是否消费：
	 * 鼠标类事件 imgui 要 → true（下游相机不收）；键盘类同理；
	 * 窗口生命周期事件（QUIT/RESIZE/…）恒不消费。
	 * @param sdlEvent const SDL_Event*（void* 擦除）
	 * @return true = 事件已消费
	 */
	bool handleEvent(const void* sdlEvent);

	/**
	 * 参数页构建器挂载点（Phase 10 批 10.2b；main 装配，缺省空 = 面板只显示读数区）。
	 * 每帧 UI 段调用一次重建页面（调试路径分配）；条目契约 = param_registry（回调对 +
	 * 语义层，零布局镜像）——本层只渲染条目，不认识 ECS/registry。
	 * @param builder 页构建回调（如 `buildDebugParamPages(registry)` 包装）
	 */
	void setPageBuilder(std::function<std::vector<ParamPage>()> builder) {
		pageBuilder = std::move(builder);
	}

	/**
	 * 帧末 UI 渲染段（Renderer setFrameEndHook 挂载点；当前 CB 录制中、swapchain
	 * 已推进到 COLOR_ATTACHMENT_OPTIMAL 是调用方契约）。
	 * NewFrame → buildPanels → Render → beginRendering(load) → 后端画 → end。
	 * @param cmd 录制中的命令缓冲
	 * @param extent swapchain extent（renderArea）
	 * @param swapchainView 本帧后缓冲 view（color attachment）
	 * @return true = 本帧录制了 UI 绘制（false = 未就绪/空 draw data）
	 */
	bool renderFrame(vk::raii::CommandBuffer& cmd, vk::Extent2D extent, vk::ImageView swapchainView);

  private:
	/** 面板内容（10.1 = profiler 读数；10.2b = 参数页 TabBar）。 */
	void buildPanels();
	/** 渲染参数页（pageBuilder 非空时；TabItem per page，条目 = Slider/ColorEdit3/Drag3）。 */
	void buildParamTabs();
	/** 把 imgui 坐标系钉成帧缓冲像素（每帧覆盖后端值；为什么必须 → impl 注释）。 */
	void useFramebufferPixelSpace(vk::Extent2D extent);
	/** UI 缩放观测（首帧回显 + 漂移告警；为什么必须在覆盖前调用 → impl 注释）。 */
	void observeUiScale(vk::Extent2D extent);

	bool initialized = false;
	// UI 缩放观测：首帧是否已回显 / 漂移是否已告警过（observeUiScale；理由见 impl）
	bool coordLogged = false;
	bool scaleDriftWarned = false;
	// 窗口内容缩放倍数（SDLWindow::displayScale；分数缩放桌面 = 1.75，普通屏 = 1.0）。
	// 字号按它烘焙，使 UI 在分数缩放下保持**物理尺寸与清晰度**（否则帧缓冲变大而
	// 字号不变 = UI 相对变小且字形被拉伸发糊）。
	float uiScale = 1.0f;
	// 后端 init 后仍持有指针（pColorAttachmentFormats），必须比后端活得久；
	// VkFormat{} = 0 = VK_FORMAT_UNDEFINED（枚举子不经 rlvk using 传播，用零值等价）
	VkFormat uiColorFormat = VkFormat{};
	// 参数页构建器（Phase 10 批 10.2b，setPageBuilder 装配；空 = 面板只显示读数区）
	std::function<std::vector<ParamPage>()> pageBuilder;
};

}  // namespace RGL
