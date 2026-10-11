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
 * SSR 的 6 枚**每帧**画质参数（configs/runtime.lua `ssr` 块，Phase 15 批 15.1 参数面；
 * 第 6 枚 blurScale 是批 15.3 子步 B 的模糊旋钮）。
 *
 * ## 为什么是一个结构而不是一排扁平字段
 *
 * 这 6 个量**永远作为一个整体**从配置流到 scene data（RuntimeConfig → registry ctx →
 * fillSsrParams → 两个 vec4），中间没有任何一处只读其中一枚。收成一体 = 「配置键 ↔
 * scene data 分量」的映射只有一处（`fillSsrParams`）。护栏分两半：字节布局与填值 =
 * tests/test_ssr_params；配置往返（lua 文本 → loadRuntimeConfig → 默认值/逐键覆盖）=
 * tests/test_scene_config 的 ssr 块用例（那里是五份 loader 解析契约的住处）。
 *
 * ## 🔴 本结构体成员初始化值 = 这 6 个默认值的**全项目唯一真相源**
 *
 * shader 侧**不留 fallback 常量**（原 `ssr.frag.slang`（15.3B 拆为 ssr_trace/ssr_apply）里
 * 五枚 `static const` 已删）。两处
 * 定义 = 迟早漂移，而漂移的形态是"配置改了没反应"，比写死更难查。
 * ⚠ 默认值不是拍脑袋，且**两枚来源不同**（这个区别是本参数的全部教训）：
 *  - `thicknessPx` / `biasPx`（6.0 / 0.6）= 闭式靶调定值反推，**像素单位**、与场景尺度无关，
 *    靶上标定即交付态（推导 → `docs/phase15_plan.md` §3.2）。
 *  - `maxDistanceRadii` = **0.52**，2026-10-10 在交付场景 bistro 上按**饱和判据**定，
 *    **不再用靶的 0.26**：靶的 SceneBounds.radius 被那块 50m 地面 quad 撑到 31.065，
 *    不代表"可反射物的大小"，且靶上该参数 0.13 以上就饱和 ⟹ **靶给不出这个参数的信息**。
 *    bistro 实测差分 px 随本值：0.13→2492 / 0.26→4871 / 0.45→6545 / **0.52→6649** /
 *    0.70→6662 / 1.0→6663（曲线在 0.52 后进入饱和）。取 0.52 = 拿到渐近线反射量的 99.8%
 *    而只花 84% 的成本（GPU 实测 ssr pass：0.26=159µs / 0.52=259µs / 1.0=308µs）。
 *    ⚠ 0.26 的形态是**反射带被截断在湿步道中间**（0.26→0.52 新增的 1779 px 是一条连续带
 *    的延伸、不是散落噪声），不是"少一点反射"那么无害。
 *
 * 🔴 **改本结构的值不影响红线 A**（实测）：`enabled` 默认关 ⟹ 六枚系数虽仍**无条件**写进
 * scene data，但**没有消费者** ⟹ 出厂画面逐位不变。红线 A 只被 `enabled` 与图形态动到。
 *
 * ## 与 `ssrEnabled` 的区别（别混）
 *
 * `enabled` 是**创建期量**（决定渲染图里有没有 ssr pass、要不要领 ssr_applied RT，改它
 * 要走重启）；本结构的 6 枚是**每帧量**（每帧写进 scene data 尾区 @722336/@722352，
 * 改它不改图形态、不重建任何 RT）。⚠ 但 runtime.lua **不在热重载清单里**（热重载只套
 * scene.lua / camera.lua / log_level.json，见 `wireHotReload`）⟹ 改文件仍需重启进程才
 * 生效；"每帧量"说的是**机制**（引擎每帧现取），不是"热重载已接"。
 */
export struct SsrTuning {
	/// maxDistance = SceneBounds.radius × 本值（射线长度上限的尺度系数）。
	float maxDistanceRadii = 0.52f;
	/// 命中窗口深度 = 本值 × worldPerPixel(**采样点**视深)。与 `biasPx` 构成不变式
	/// `biasPx < thicknessPx`（见下方两条注释）。
	/// `0` 是**合法消融档**（命中窗口退化为空 ⟹ 闭式覆盖率该恰为 0），配置层不 clamp。
	/// 🔴 **15.3 子步 C 重定 6.0 → 12.0**：本窗口的物理含义 = 「一步行程允许穿深」，必须**跟
	/// 住 march 网格**。15.1 定 6.0 时格 = 2 全分辨率 px；15.3B 半分辨率后格 = 2 半分辨率 px
	/// = 4 全分辨率 px，一步穿深翻倍而窗口没跟 ⟹ 诊断档实测 M3 厚度拒绝 15.45→20.42pp。
	/// 翻倍后（诊断档，参考机位过门控分母 209824）：M3 20.42→10.75%、命中 38.64→48.31%
	/// （M3 的 47.4% 转化为命中）；靶判据 A 98.5% / 判据 B 泄漏 **0**（反向消融 = 撤窗口
	/// thickness_px=1e6 ⟹ 泄漏 283 px、最远 141.8px，判据 B 红 = 窗口仍有牙）。
	/// 再往上（24/48）收益递减（+2.98/+1.14pp）且靶对 sticking 的敏感度不足以约束，不加。
	float thicknessPx = 12.0f;
	/// 自交偏置 = 本值 × worldPerPixel(采样点)。射线起点就在 G-buffer 表面上（along(0)=0），
	/// 数值噪声会立刻给出 along<0 的假命中，故命中窗口是 `[-thickness, -bias]` 而非 `[-t, 0]`。
	/// 🔴 **shader 侧不变式 `biasPx < thicknessPx`**：违反 = 窗口为空 = **永不命中**（不是
	/// 崩溃、不是 NaN，是"SSR 看起来完全没作用"）。`biasPx >= thicknessPx` 因此是判据 6
	/// 的**反向消融档**，配置层**必须允许写出来**（见 readSsrBlock 注释）。
	float biasPx = 0.6f;
	/// roughness 低于此 = 满强度反射。🔴 **shader 侧不变式 `roughFull < roughCutoff`**：
	/// 违反 = `smoothstep(e0, e1, ·)` 的 e0 > e1 ⟹ **门控反向**（光滑面 gate=0 = SSR 整个
	/// 不起作用、粗糙面 gate=1 = 满反射）。⚠ 实测（2026-10-10 靶反向档 0.40/0.15）：凸包内
	/// 覆盖率**恰 0**、凸包外泄漏 0 ⟹ 失效形态是"没反射"，**不是** NaN 垃圾画面；e0 == e1
	/// 也只在 roughness 恰等于 e0 的像素上出 0/0（其余 ±inf 被 clamp 收成 0/1）。
	float roughFull = 0.15f;
	/// roughness 高于此 = 完全不反射（中间 smoothstep 过渡；硬切会在材质边界出现可见开关线）。
	float roughCutoff = 0.40f;
	/// 粗糙度感知模糊半径上限系数（**全分辨率 px**）：apply 的螺旋 taps 半径 =
	/// roughness × 本值（scene data @722352.z，批 15.3 子步 B）。默认 8.0 ⟹ roughness
	/// 0.05 → 0.4px ≈ 无模糊、0.35 → 2.8px（湿区 0.05–0.35 区间标定；gate.y 以上本来
	/// 就没有 trace 可糊）。`0` 是**合法消融档**（半径 0 = taps 全落中心 = 纯双线性上采样），
	/// 配置层不 clamp（同 readSsrBlock 纪律：消融档必须写得出来）。
	float blurScale = 8.0f;
};

/**
 * 湿面调制的 6 枚**每帧**参数（configs/runtime.lua `wet` 块，Phase 15 批 15.2 子步 A 参数面）。
 *
 * ## 为什么是一个结构（同 `SsrTuning`，理由不重复）
 *
 * 6 枚量**永远作为一个整体**从配置流到 scene data（RuntimeConfig → registry ctx →
 * fillWetParams → 两个 vec4），中间没有任何一处只读其中一枚。收成一体 = 「配置键 ↔
 * scene data 分量」的映射只有一处（`fillWetParams`）。护栏分两半：字节布局与填值 =
 * tests/test_wet_params；配置往返（lua 文本 → loadRuntimeConfig → 默认值/逐键覆盖）=
 * tests/test_scene_config 的 wet 块用例。
 *
 * ## 🔴 本结构体成员初始化值 = 这 6 个默认值的**全项目唯一真相源**
 *
 * shader 侧**不留 fallback 常量**（scene.slang 的 readWetParams/readWetSpatial 只读内存）。
 *
 * ## 湿面**身份**不占本结构的参数（设计定案 → `docs/phase15_plan.md` §2.4 修正块）
 *
 * 「哪些面湿」= `1 − smoothstep(ssr.roughFull, ssr.roughCutoff, roughness)`，与 SSR 粗糙度
 * 门控**同区间同源**（资产已标定：湿材质 roughness 0.122 在区间下方 → wetness = 1，干面
 * ~0.74/1.0 在区间上方 → wetness = 0，两簇间隔着整个门控区间）⟹ 结构上排除「反射了但没
 * 压暗」的错配。本结构只放**调制系数**与**空间变化参数**。
 *
 * 🔴 **改本结构的值 = 改出厂画面**（15.2 子步 B 起消费者已接线）：shader 消费者 =
 * `deferred_geometry.frag.slang` 湿面 G-buffer 调制（身份 = readSsrGate 同区间同源）。
 * 红线 A 已随之重立 = `54e40665…`（`docs/还债总账.md` 红线声明块）；`strength = 0` 消融档
 * 实测逐位 = 旧锚 `5ff793bf…`（恒等旁路的机器可验形态，`just shot --set strength=0`）。
 */
export struct WetTuning {
	/// 总开关兼强度（0 = 关 = 消融档）。**默认开**（用户拍板：湿面默认开、15.2B 重立红线 A）。
	/// `0` 是**合法消融档**，配置层不校验不 clamp（理由同 readSsrBlock：消融档必须写得出来）。
	float strength = 1.0f;
	/// 湿区 albedo 压暗系数（×0.5–0.7 区间取中；真实湿面水填进微观凹凸 → 压暗漫反射，
	/// 现状「只加高光不压暗」正是 AGENTS.md 登记的边界，本值就是还这笔的旋钮）。
	float dark = 0.6f;
	/// 水洼粗糙度（水膜表面）。🔴 **shader 侧不变式 `roughLo < roughHi`**（水洼必须比
	/// 湿石更光滑，反了 = 空间调制方向倒置）。配置层**不校验**，理由同 readSsrBlock。
	float roughLo = 0.05f;
	/// 湿石粗糙度（资产湿面 roughness 均匀 0.122 给不了「水洼比湿石更滑」的空间结构，
	/// 由 shader 端程序化调制，本值是调制上端点）。
	float roughHi = 0.35f;
	/// 水膜法线扰动幅度（0 = 无扰动 = 逐位旁路扰动项）。
	float rippleAmp = 0.3f;
	/// 水洼噪声频率 = 本值 / sceneRadius（**尺度无关**口径，同 ssrParams 的尺度锚点纪律；
	/// bistro radius≈1.16 → 特征尺寸 ≈0.1 世界单位）。
	float puddleFreqRadii = 12.0f;
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
 * 抗锯齿选路（configs/runtime.lua 的 `aa` 键，批 14.6）：**三态互斥**，不是两个布尔。
 *
 * 为什么必须是 enum（用户拍板的理由，写下来防"顺手加个布尔"）：两个布尔
 * （`taa.enabled` × `fsr_temporal`）的失效形态 = 「两个都开然后引擎静默关掉一个」
 * （`resolveFsrTemporal` 强制 FSR 时域关，而配置里仍写着 true）——申报值与生效值
 * 分家，看配置会以为开着。三态让它**结构上不可能**：一个槽位只有一个值，
 * 「谁做抗锯齿」根本不存在同时命中两条的写法。
 *
 * 消费侧一律走 `RuntimeConfig::aaWantsTaa()/aaWantsFsr()` 谓词，不在各处比字符串
 * （字符串比较 = 拼错静默失配；enum 拼错编译失败）。
 */
export enum class AaMode {
	off,  // 默认 = 出厂态：无 TAA 无 FSR（红线 A 那张图 `fdece9d0…`）
	fsr,  // FSR3-upscaler 做抗锯齿（隐含 velocity_field，见 RuntimeConfig::velocityField）
	taa,  // 自研 TAA 做抗锯齿（图形态开关 = 本键推导，scene.lua `taa` 块只剩调参）
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
	 * 为什么保持**独立键**而不是并进 `aa`（批 14.6 裁决）：它有非 FSR 消费者——批 14.4
	 * 起的 TAA 逐像素重投影线要它，与「谁做抗锯齿」无关。但 `aa` 为 fsr 时它被**隐含
	 * 打开**（FSR 的 motionVectors 输入就是那张图）——隐含只发生在解析期一处
	 * （runtime_config_impl.cppm 的 `applyAaKey` 段），本字段因此仍是**单一真相源**，
	 * 消费侧不需要知道隐含关系的存在。
	 *
	 * 开 = 每帧多一个全屏 pass（读 depth、写 RGBA16F）+ 一张渲染分辨率 RGBA16F RT。
	 */
	bool velocityField = false;
	/**
	 * 批 14.6：抗锯齿选路（三态语义与"为什么不是两个布尔" → `AaMode` 注释）。
	 * 缺键 = off = 出厂默认 = 红线 A 那张图。未知值 = 解析期 fail-fast（present_mode
	 * 的既有纪律：配置笔误不静默回落）。
	 *
	 * fsr 态 = 原 `fsr_upscaler` 的语义（构造 vendor context + 图外 compute + 四张
	 * 私有图；关态零构造逐位不变的红线原样继承，只是门从布尔换成了槽位）。
	 * taa 态 = 原 `scene.lua taa.enabled` 的语义（decorateTaaPass 的图形态开关）。
	 */
	AaMode aa = AaMode::off;
	/// fsr 态的两枚**调参**（configs/runtime.lua 嵌套块 `fsr = { temporal, jitter }`），
	/// 只在 aa == fsr 时被消费；其余态下它们是无人读的默认值。
	/// 默认 true/true = FSR 的**设计点**（vendor 累积历史 + 亚像素抖动）——纯空间是
	/// 消融臂要显式写 false。曾长期默认关，理由「实测退化」**已作废**（根因 = 申报
	/// Y 反号 + Lanczos LUT 缺陷，均修，phase14_plan §1.18/§1.19）。
	/// temporal 与 TAA 的互斥**不再靠运行期裁决**（三态结构上不可能同开），
	/// `resolveFsrTemporal` 只留作防御性断言。
	bool fsrTemporal = true;
	/// 把亚像素 Halton 抖动接进投影矩阵并申报给 FSR（原 `fsr_jitter` 键）。
	/// 只在 FSR 真的接起来时生效；设计意图 = 让输入落到 FSR 的设计点上（§1.16）。
	bool fsrJitter = true;
	/**
	 * Phase 15 批 15.1：SSR（屏幕空间反射）总开关（configs/runtime.lua 嵌套块
	 * `ssr = { enabled = … }`）。**创建期**量——它决定渲染图里**有没有** ssr pass。
	 *
	 * 关（默认）= `decorateSsrPass` 根本不调用 ⟹ 图里没有这个 pass、不领 ssr_applied RT、
	 * 不建管线条目 ⟹ **结构性**逐位不变（不是"有但旁路"——运行时分支挡不住 slang 对源码
	 * 差异的浮点重排，教训原文 = `shaders/light.frag.slang` 的 IZB_CONSUME 注释）。
	 *
	 * 与 `aa` 三态键**正交**（拍板记录 → `docs/phase15_plan.md` §3 批 15.1）：`aa` 管
	 * 「谁做抗锯齿」（TAA/FSR），SSR 是独立特性，两键可任意组合（SSR 开 × aa 三档都要冒烟）。
	 * deferred-only：forward 图无 G-buffer，装饰器按「图里缺 kGBufferAttachments 契约表
	 * 任一通道」跳过（D1 对等豁免 → `docs/phase15_plan.md` §0.1）。
	 */
	bool ssrEnabled = false;
	/**
	 * SSR 的 6 枚**每帧**画质参数（configs/runtime.lua `ssr` 块）。**每帧量**——与上面
	 * `ssrEnabled`（创建期量）的区别与"为什么是一个结构" → `SsrTuning` 注释。
	 * 消费链：main 启动期 `registry.ctx().emplace<SsrTuning>(configs.runtime.ssr)` →
	 * renderListSystem 每帧取 → fillSsrParams 写 scene data @722336/.yzw 与 @722352/.xyz
	 *（z = blurScale，批 15.3B 起占用，消费者 = ssr_apply）。
	 */
	SsrTuning ssr;
	/**
	 * 湿面调制的 6 枚**每帧**参数（configs/runtime.lua `wet` 块）。**每帧量**——与
	 * `ssr` 同构（湿面没有创建期开关：它不是独立 pass，是 deferred_geometry 的 G-buffer 调制，
	 * 不改图形态；语义与「身份不占参数」→ `WetTuning` 注释）。
	 * 消费链：main 启动期 `registry.ctx().emplace<WetTuning>(configs.runtime.wet)` →
	 * renderListSystem 每帧取 → fillWetParams 写 scene data @722368/.xyzw 与 @722384/.xy →
	 * shader 消费者 = deferred_geometry.frag.slang 湿面调制（15.2B 接线，默认开）。
	 */
	WetTuning wet;

	/** 「TAA 在不在图里」的唯一推导点（原 `scene.lua taa.enabled` 的消费者全部改走这里）。 */
	[[nodiscard]] constexpr bool aaWantsTaa() const {
		return aa == AaMode::taa;
	}
	/** 「FSR 要不要接」的唯一推导点（原 `fsr_upscaler` 的消费者全部改走这里）。 */
	[[nodiscard]] constexpr bool aaWantsFsr() const {
		return aa == AaMode::fsr;
	}
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
