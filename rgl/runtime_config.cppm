export module runtime_config;

import rlstd;
import vkerror;

namespace RGL {

/**
 * 一次性截图配置（2026-09-08 自 camera_config 迁入——llm_debug 与相机语义无关，
 * 属运行模式）。llm_debug = true/false 纯开关（缺省 false，交互模式零变化）；
 * llm_shot = {file, size, delay_frames} 常驻截图配置（全可选回落默认）。
 */
export struct LlmDebugConfig {
	bool enabled = false;
	uint32_t delayFrames = 0;  // 第 N 帧渲染完成后截图（0 = 首帧）
	std::string file;          // PNG 路径（相对 CWD）；空 = screenshots/llm_<时间戳>.png
	uint32_t width = 1280;     // llm_debug 模式的离屏隐藏窗尺寸
	uint32_t height = 720;
};

/**
 * 窗口 present 模式偏好（configs/runtime.lua 的 present_mode 键，2026-09-27 批 9.3
 * 用户追加需求：FIFO/eImmediate 双模式对比测量）。仅**窗口路径**生效；headless
 * surface 无 eImmediate、present 为 no-op，恒走 eFifo 回落（忽略此偏好）。
 */
export enum class PresentModePref {
	immediate,  // 默认 = 现状：硬要求 eImmediate（缺失抛错，Wayland compositor 管同步）
	fifo,       // eFifo（spec 必保模式；vsync 阻塞、帧率 = 刷新率——present_timing 测量对比用）
};

/**
 * `renderScale` 的合法区间（**单一真相源**：配置解析钳制与 Swapchain 派生 render extent
 * 都读它，避免两处各写一个魔数、改一个漂一个）。
 *
 * 上界 1.0 = 内部分辨率不超过窗口。>1 是超采样，本 phase 不做（放大用 FSR，缩小不是
 * 任何批次的需求），留着的唯一后果是一条永不被测的路径。
 * 下界 0.25 = 1280×720 时内部 320×180；再低像素类判据会退化成"看色块"，测不出东西。
 */
export inline constexpr float kRenderScaleMin = 0.25f;
export inline constexpr float kRenderScaleMax = 1.0f;

/**
 * 渲染网格尺寸（Phase 14 批 14.3）。
 *
 * **为什么住 rlbase 而不是 `vk::Extent2D`**：这段是纯算术，而 `vk::Extent2D` 会把
 * `rlvk` 依赖拖进 `runtime_config`（rlbase 层红线：整层无设备依赖）。同时"纯算术"正是
 * `just test` 能覆盖的形态——放 rlgfx 就得连 SDL 一起链进测试。
 * Swapchain 侧负责 `RenderSize → vk::Extent2D` 的转换（一次 static_cast）。
 */
export struct RenderSize {
	uint32_t width = 0;
	uint32_t height = 0;
};

/**
 * 把任意输入钳进合法区间（非有限值 → 回落 `fallback`）。配置解析与 Swapchain 构造**共用**，
 * 所以"配置里钳过一次、构造时又钳一次"不会给出两个答案。
 */
export inline float clampRenderScale(float v, float fallback = 1.0f) {
	if (!std::isfinite(v))
		return fallback;
	return std::clamp(v, kRenderScaleMin, kRenderScaleMax);
}

/**
 * 渲染尺寸派生（**单一真相源**：Swapchain 构造期与 recreate() 都走它）。
 *
 * 各轴独立 `lround`、下限 1（0 会让 viewport / RT 尺寸为 0 = 未定义行为）。
 * **`scale == 1.0f` 时逐像素等于输入**（float 尾数 24 位足够精确表示任何实际分辨率的
 * 整数像素，`lround(w * 1.0f) == w` 恒成立）——这条就是"renderScale 默认档交付路径
 * 逐位不变"红线的算术根据，`tests/` 里有对应用例钉住。
 *
 * 宽高比：两轴独立取整可让比例偏差 ≤1 像素（1920×0.75=1440 精确；奇数尺寸会差半像素）。
 * 投影矩阵的比例来源必须与渲染网格同源，故 `projectionSystem` 读 `RenderExtent` context
 * 而不是 `WindowExtent`（见 ecs.cppm）。
 */
export inline RenderSize computeRenderSize(uint32_t baseWidth, uint32_t baseHeight, float scale) {
	auto one = [](float v) { return std::max<uint32_t>(1, static_cast<uint32_t>(std::lround(v))); };
	return {
	    one(static_cast<float>(baseWidth) * scale),
	    one(static_cast<float>(baseHeight) * scale),
	};
}


/**
 * 运行/窗口模式（configs/runtime.lua）：替代 RGL_HEADLESS / RGL_HEADLESS_SIZE /
 * RGL_WIN_SIZE / RGL_SHOW_AFTER 环境变量 + camera.lua 的 llm_debug/llm_shot 段。
 * 全字段可选，缺文件 = 全默认（交互窗 800x600、不截图）。
 * 注：驱动级 env（VK_DRIVER_FILES / prime offload）不在此列——
 * 由 Justfile run-* 配方注入，非应用配置。Mesa ≥26.2.3 起 RADV 无需 RADV_EXPERIMENTAL。
 */
export struct RuntimeConfig {
	bool headless = false;            // VK_EXT_headless_surface 脱 SDL/compositor（CI/agent 冒烟）
	uint32_t headlessWidth = 1280;
	uint32_t headlessHeight = 720;
	uint32_t winWidth = 800;          // 交互窗尺寸（headless / llm 模式下被各自尺寸覆盖）
	uint32_t winHeight = 600;
	/**
	 * 内部渲染分辨率倍率（Phase 14 批 14.3）：render extent = swapchain extent × 本值。
	 *
	 * 1.0 = 与窗口逐像素对齐（**默认，交付路径逐位不变**）。<1 = 内部分辨率低于窗口，
	 * 由 tonemap pass 采样 scene_hdr 时线性放大（该 pass 是全屏大三角 + linear sampler，
	 * 归一化 uv 采样 ⟹ 放大免费，不需要额外的 pass 或 RT）。
	 *
	 * 与债-37 的 HPD 分数缩放**正交**：那条管「窗口逻辑点 → 物理像素」，本值管
	 * 「物理像素 → 渲染像素」。imgui / 点选 / 鼠标一律走窗口像素，不随本值缩放。
	 *
	 * 创建期量（不是逐帧）：RT 与深度图按它分配，改它要走重建路径，热重载不生效。
	 * 越界 = 钳制 + warn（与 ui_font_size 同处理，不 fail-fast）。
	 */
	float renderScale = 1.0f;
	/**
	 * Phase 14 批 14.4：逐像素速度场（motion vectors）总开关。**创建期**量（改它要走
	 * 重建路径，热重载不生效）——它决定渲染图里**有没有** velocity pass。
	 *
	 * 关（默认）= `decorateVelocityPass` 根本不调用 ⟹ 图里没有这个 pass、不领 velocity
	 * RT、不建管线条目 ⟹ **结构性**逐位不变（不是"有但旁路"，与 ao 的 strength=0 早退
	 * 是两种不同的红线机制；与 taa 同构）。
	 *
	 * 为什么放 runtime.lua 而不是 scene.lua：本键**没有任何逐帧可调语义**（不像
	 * `ao.strength` / `taa.enabled` 那样有滑杆与逐帧状态），它只改图形态；同类的
	 * `render_scale` 也在这里。Phase 14 的 FSR 门将来落在本键旁边。
	 *
	 * 开 = 每帧多一个全屏 pass（读 depth、写 RGBA16F）+ 一张渲染分辨率 RGBA16F RT。
	 * 目前（批 14.4）尚无消费者——批 14.5 接 FSR upscaler 才用得上它。
	 */
	bool velocityField = false;
	/**
	 * Phase 14 批 14.5：FSR 3.1.4 upscaler 总开关。**创建期**量（同 `velocity_field`，
	 * 热重载不生效）——它决定要不要构造 vendor context 与它的四张私有图。
	 *
	 * 关（默认）= `Renderer::fsr` 恒空 ⟹ 零构造、零命令、**结构性**逐位不变。
	 *
	 * **依赖 `velocity_field`**：FSR 的 motionVectors 输入就是那张图，所以判据取
	 * **实际图形态**（`velocityDecorated`）而不是配置键——velocity 装饰失败时本键
	 * 自动失效（AO 施加点的先例，main_scene.cppm:437）。
	 *
	 * 开 = 每帧多一段图外 compute（vendor 内部一次录完全部 pass）+ 四张私有图
	 *      （输出 + 3 张 vendor 共享图）。
	 */
	bool fsrUpscaler = false;
	int showAfter = -1;               // DIAG：隐藏窗启动、第 N 帧后显示（-1 = 立即显示）
	PresentModePref presentMode = PresentModePref::immediate;  // 窗口 present 模式（headless 忽略）
	LlmDebugConfig llm;
	// imgui 面板字体（Phase 10b 批 11b.1）：路径空 = 自动探测链（imgui_layer_impl
	// kUiFontCandidates，MiSans Light 首选）；字号 <=0 = 回落 18px。仅 imgui 门开时用。
	std::string uiFont;
	float uiFontSize = 18.0f;
};

/**
 * 从 Lua 加载运行配置。文件缺失 → 默认 + warn；脚本/类型错误 → unexpected。
 * 沙箱限制同 camera_config（禁用 dofile/loadfile/package/require）。
 * @param path 配置文件路径
 * @return 解析好的 RuntimeConfig；失败返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<RuntimeConfig, Error> loadRuntimeConfig(std::string_view path);

}  // namespace RGL
