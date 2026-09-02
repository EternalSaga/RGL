module;

export module vk_renderer;

import rlstd;
import rlvk;
import rllogger;

import vulkan_utils;
import vulkan_window;
import vk_device;
import vk_swapchain;
import vk_resources;
import VmaRaii;
import shader_registry;  // Phase 4 批次 2：管线表迁出（PipelineEntry/ShaderRegistry，零 entt）
import shader;
import vkerror;

// 绘制契约（DrawCall/SceneFrameData/FrameDrawList）拆出为独立模块——本模块 export import
// 保持 Renderer::drawFrame 接口类型对外可见（2026-09-02 拆分）
export import frame_draw_list;

namespace RGL {

// Renderer：每帧"录制 → 提交 → 显示"的驱动器 + 每帧同步状态的持有者。
//
// 渲染流程（一次 drawFrame(list) 调用，逐步骤注释见函数体内 ①~⑮）：
//   等待上一帧同 slot 的双 fence（submit 与 present 均完成）并重置
//   → acquireNextImage 获取后缓冲（信号 imageAvailable 信号量）
//   → 录制命令缓冲：图像 barrier → 动态渲染附件
//     → descriptor heap barrier/绑定 → beginRendering
//     → 动态状态 → 逐 draw：push data → 绑定 ShaderObject → drawMeshTasksEXT → endRendering
//     → PresentSrc barrier
//   → queueSubmit（等 imageAvailable / 信号 renderFinished / 挂 inFlight fence）
//   → presentKHR（pNext 挂 SwapchainPresentFenceInfoEXT 信号 present fence）
//   → currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT
//
// 绘制内容 = 渲染 system 产出的 FrameDrawList（本类是盲执行器，不认识具体场景资产，
// 见 DrawCall/FrameDrawList 注释）。核心现代特性：Dynamic Rendering（无 RenderPass）+
// ShaderObject（无 PipelineLayout，bindShadersEXT + 动态状态）+ DescriptorHeap
//（vkCmdBindResourceHeapEXT 取代 descriptor set，使用规范见 docs/DESCRIPTOR_HEAP_SPEC.md）。

export class Renderer;

export class Renderer {
   public:
	// 成员初始化列表顺序必须与下方声明顺序一致（RAII 规则：vk::raii::* 直接成员只能在
	// 初始化列表初始化）。管线表不再是 Renderer 成员（Phase 4 批次 2：迁出至 ShaderRegistry，
	// Vulkan 层零 entt）——registry 在 main 先于 Renderer 构造（需 Resources 的 cube 资产
	// + descriptor heap），Renderer 持引用；"cube" 条目恒建（批次 3：画不画 = cube entity
	// 有无，数据驱动，showCube 配置由 main 决定是否注册 entity；条目多建 2 个 shader object，
	// 成本可忽略）。模型管线条目不在构造期建（批次 3：管线表与场景内容解耦，E1.3）——
	// 资产上传期经 ShaderRegistry::addModelEntry 动态加入（按 (风格, 顶点 slot, 纹理 slot)
	// 组合去重，同组合共享一条，Phase 4 验收 392 primitive → 12 条）。
	Renderer(RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain, RGL::Resources& vkResources,
		RGL::ShaderRegistry& shaderRegistry);
	RGL::Swapchain& getSwapchainRef() const { return *swapchain; }
	RGL::Resources& getResourcesRef() const { return *resources; }
	// 离屏截图（llm_debug.cppm captureFrame）用的只读访问器
	RGL::DeviceContext& getDeviceContextRef() const { return vkDevice; }
	const vk::raii::CommandPool& getGraphicsCommandPoolRef() const { return graphicsCommandPool; }
	// 离屏截图（llm_debug.cppm captureFrame）用的只读访问器：委托管线表（ShaderRegistry）。
	// entry 访问器供截图路径取 pushDataSet（与主路径 drawCall 同一契约字段，不硬编码 0——
	// push set 显式化契约（L5）两条路径必须同步，否则 pushDataSet 离开 0 时截图静默分歧）
	const RGL::PipelineEntry& pipelineEntryAt(uint32_t index) const { return registry.entry(index); }
	const RGL::RenderState& getOpaqueState() const { return opaqueState; }
	const RGL::RenderState& getTransparentState() const { return transparentState; }
	// edS3：depth clip 动态状态能力（llm_debug 离屏重渲消费，同主路径同一门，避免两路分歧）
	[[nodiscard]] bool isDepthClipDynamic() const { return depthClipDynamic; }
	const RGL::RenderState& getSkyboxState() const { return skyboxState; }
	// 等待 GPU 空闲（resize / 重建 swapchain 前调用，确保旧资源不再被使用）
	void waitForIdle() { vkDevice.getDevice().waitIdle(); }
	// 渲染一帧：整体流程见类头注释；绘制内容 = 渲染 system 产出的 FrameDrawList
	//（盲执行器，不认识具体场景资产）；失败（device lost 等）时返回错误
	[[nodiscard]] std::expected<void, Error> drawFrame(const FrameDrawList& list);

   private:
	// DIAG（RGL_OFFSCREEN）：离屏颜色目标（8-bit UNORM，与后缓冲同 extent；
	// 同 llm_debug captureFrame 的颜色图形态，VMA 自动绑内存）
	static std::expected<RGL::VmaImage, Error> createOffscreenImage(RGL::VmaAllocatorRAII& allocator,
		vk::Extent2D extent);
	// DIAG（RGL_OFFSCREEN）：离屏目标 view 的 create info（!offscreen 时 image = null）
	VkImageViewCreateInfo buildOffscreenView() const;
	// 创建 MAX_FRAMES_IN_FLIGHT 套同步对象（每个帧 slot 一套，双缓冲）。
	//
	// 四个角色：
	//   imageAvailableSemaphores — acquireNextImage 信号，queueSubmit 等待
	//                              （通知 GPU 后缓冲已可绘制）
	//   renderFinishedSemaphores — queueSubmit 信号，presentKHR 等待
	//                              （通知合成器本帧绘制完成）
	//   inFlightFences — 挂 queueSubmit，追踪"命令缓冲执行完"
	//   presentFences — 经 SwapchainPresentFenceInfoEXT 挂 presentInfo.pNext，
	//                   追踪"present 完成"（VK_KHR_swapchain_maintenance1）
	//
	// 为什么是双 fence：单 fence 只挂 queueSubmit 时，triple-buffer（KWin）下 fence
	// 信号时 present 可能仍在异步等待，下一帧重用 semaphore 会违反 VUID-00067。
	// 见 docs/技术概念.md 的"双 Fence"一节。
	void createSyncObjects();
	// 批量分配 MAX_FRAMES_IN_FLIGHT 个 primary command buffer（每帧 slot 一个，
	// 每帧 reset() 后重新录制）
	void allocateCommandBuffers();
	// scene data staging ring（Phase 5 批次 2，全项目首个每帧动态 GPU 数据）：
	// ×MAX_FRAMES_IN_FLIGHT 常驻映射 host buffer。写安全 = 帧 slot 复用规则——
	// drawFrame ① 已 wait 本 currentFrame slot 的 inFlight fence（f-2 帧命令跑完）
	// 才写本 slot staging；录制期 vkCmdCopyBuffer 拷进 scene data device buffer
	//（命令流内天然有序），描述符静态指向该 buffer 永不改动。
	void createSceneStaging(RGL::Resources& vkResources);
	// 窗口尺寸变化（resize）或 acquire/present 返回 out-of-date 时重建 swapchain + 深度图。
	// 深度图必须与 swapchain extent 同步重建，否则 renderArea 会超出图像范围。
	[[nodiscard]] std::expected<void, Error> recreateSwapchainAndDepth(const char* context);
	RGL::DeviceContext& vkDevice;
	RGL::Swapchain* swapchain;
	RGL::Resources* resources;
	// 管线表（Phase 4 批次 2：迁出至 ShaderRegistry，零 entt；main 先于 Renderer 构造并持引用，
	// 析构序 Renderer 先于 registry 安全）
	RGL::ShaderRegistry& registry;
	// DIAG（可见窗 GPUVM fault 规避 2026-08-28）：RGL_OFFSCREEN=1 → 场景渲到离屏
	// 8-bit UNORM 目标（纹理采样路径与 llm_debug 离屏同构——隐藏窗离屏路径实测无
	// fault），再 blit 到后缓冲上屏；规避可见后缓冲（compositor 在读）关联的
	// 纹理 high VA 采样 fault（现象/排查过程见 docs/archive/heap_texture_research.md §十一）。
	// 默认关 = 原路径零变化。
	bool offscreen = false;
	// edS3：depth clip enable 动态状态能力门（vkCmdSetDepthClipEnableEXT；启动期一次探测，
	// 未支持 = false，applyDynamicState 内 no-op 不 set——"扩展缺失干净降级"原则）
	bool depthClipDynamic = false;
	// DIAG（window+texture 组合触发验证）：可见窗渲染但永不 present——KWin 永不
	// import 我们的 swapchain dmabuf。fault 在 frame 2（buffer pool 窗口内），
	// 前几帧渲染完整，判据有效
	bool noPresent = std::getenv("RGL_NO_PRESENT") != nullptr;
	// DIAG（可见窗 fault 排查）：drawFrame 帧计数——DEVICE_LOST 时打帧号，
	// 区分"帧数累积触发"（fault 帧号恒定）vs"窗口显示触发"（fault 帧号 >> showAfter）
	uint64_t frameCount = 0;
	VmaImage offscreenImage;
	// 离屏图真实布局跟踪（DIAG RGL_OFFSCREEN）：持久图 storeOp=eStore，endRendering 后
	// 布局保留；帧末 blit barrier 迁 TransferSrcOptimal 且不迁回 → 第 2 帧起 ④ 的
	// oldLayout 必须用本跟踪值（非 Undefined）。新图创建/重建时重置 Undefined。
	vk::ImageLayout offscreenLayout = vk::ImageLayout::eUndefined;
	// 离屏目标 view（raii 默认构造已删 + 构造期即调 vkCreateImageView——null image
	// 会被校验拦下，故不能进初始化列表；optional 惰性 emplace，默认路径零分配零调用）
	std::optional<vk::raii::ImageView> offscreenView;
	// 非 RAII 句柄（队列生命周期由 DeviceContext 的 raii Device 管理）
	vk::Queue graphicsQueue;
	const vk::raii::CommandPool& graphicsCommandPool;
	// 动态状态：opaque pass（默认值 = cube 的 opaque 状态），drawFrame() 在 opaque 段首
	// applyDynamicState 一次（Phase 7 多 pass 时按 pass 换状态）
	RGL::RenderState opaqueState{};
	// 动态状态：transparent pass（Phase 5 透明排序）——不写深度 + 开混合（factors 在
	// applyDynamicState 内接线），drawFrame() 在 transparent 段首 applyDynamicState 一次
	RGL::RenderState transparentState{};
	// 动态状态：skybox pass（Phase 5.5 批次 4）——不写深度 + LEQUAL（大三角 z=1 只在
	// 空像素通过），drawFrame() 在 skybox 段首 applyDynamicState 一次
	RGL::RenderState skyboxState{};
	// descriptor heap 的 GPU 地址（VmaDescriptorHeap 管理的"描述符槽位数组"，
	// 对齐细节见 VmaBuffer.cppm）
	vk::DeviceAddress heapAddress = 0;

	// 双缓冲：最多 2 帧同时在飞——当前帧在录制 + 上一帧还在 GPU 执行。
	// 下方同步对象数组按 currentFrame 索引，fence 信号后才可重用（双 fence 设计
	// 见 createSyncObjects 注释）
	static constexpr int MAX_FRAMES_IN_FLIGHT = 2;
	uint32_t currentFrame = 0;
	// scene data staging ring（Phase 5 批次 2，见 createSceneStaging）
	std::array<RGL::VmaBuffer, MAX_FRAMES_IN_FLIGHT> sceneStaging;
	std::vector<vk::raii::Semaphore> imageAvailableSemaphores;
	std::vector<vk::raii::Semaphore> renderFinishedSemaphores;
	std::vector<vk::raii::Fence> inFlightFences;
	std::vector<vk::raii::Fence> presentFences;
	std::vector<vk::raii::CommandBuffer> commandBuffers;
};

}  // namespace RGL
