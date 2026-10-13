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
 * 要走重启）；本结构的 6 枚是**每帧量**（每帧写进 scene data 尾区 @723776/@723792，
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
	/// roughness × 本值（scene data @723792.z，批 15.3 子步 B）。默认 8.0 ⟹ roughness
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
 * 簇路径叠加的**共面双臂**深度比较档（configs/runtime.lua `meshlet.coplanar` 键，★0.5-b2）。
 *
 * 为什么必须是 enum + fail-fast（同 `AaMode` / `present_mode` 纪律，本批尤其致命）：
 * 本批主判据 = 「门关 / off / equal / ne **四臂截图哈希全同**」。若笔误值被静默回落成
 * `off`，四臂照样全绿 ⟹ **整批假绿**（equal/ne 两臂根本没跑）。故未知值 = 启动报错，
 * 且每臂 INFO 打印**生效**档（独立于图像的生效证据，见 `wireMeshletOverlayDepthCompare`）。
 *
 * 三档语义（叠加 draw 与旧路径几何共面，pass 现行深度比较 = `eLess` ⟹ 全拒零写入）：
 *  · `off`   = 不碰动态状态（零额外 vkCmd，A1 逐 bit 不变的构造性保证）
 *  · `equal` = `eEqual`：共面像素放行写回 ⟹ 与门关逐位同 = 属性/材质/手性全对
 *  · `ne`    = `eNotEqual`：存在「簇覆盖但深度 ≠ 旧深度」即写 ⟹ 与门关逐位同 =
 *              位置逐位同 + 覆盖集同
 */
export enum class MeshletCoplanarMode : uint8_t {
	off,    // 默认 = 现行 eLess（叠加零写入，门关/off 动态状态序列逐 bit 不变）
	equal,  // 叠加 draw 深度比较 = vk::CompareOp::eEqual
	ne,     // 叠加 draw 深度比较 = vk::CompareOp::eNotEqual
};

/**
 * 阴影段簇路径的**回落策略**（configs/runtime.lua `meshlet.shadow_lod_fallback` 键，★18-B1）。
 *
 * 🔴 **为什么在配置层另立一枚枚举**而不是 `import meshlet_lod;` 复用 `MeshletLodFallback`：
 * 依赖图原则「配置模块不依赖执行模块」（AGENTS 依赖图原则，同 camera_config 不 import
 * llm_debug 那条）。`meshlet_lod` 是选级模块（虽然今天恰好无设备依赖），配置层 import 它
 * 就是给「runtime_config → 执行模块」开一条边——将来那条模块一旦要 VkDevice，配置层就跟着
 * 拖进设备依赖。枚举值到 `MeshletLodFallback` 的映射住在**装配层**（批 B2），一处赋值。
 * 两侧枚举值序一致（deepest=0 / l0=1），但**不靠 static_assert 跨模块钉**——映射点是显式
 * switch，加了第三档会编译不过或走到 default，比静默错位更安全。
 *
 * 语义（§18.2 的结论：回落级才是成本主导项，比 D 选几更重要）：
 *  · `deepest`（默认）= 本 prim 内 ≤ D 的最深存在级（D=5 时 5.10% L0 三角）
 *  · `l0`            = 回落级 0（对照臂：D=7 时 34.97%，比现状全量重画还贵）
 * 🔴 未知值 = 启动 fail-fast（`meshlet.coplanar` / `aa` / `present_mode` 同纪律：静默回落 =
 * 判据整批假绿的地基事故，本键尤其致命——两策略在 D=7 上差 10 倍成本）。
 */
export enum class MeshletLodFallbackMode : uint8_t {
	deepest = 0,  ///< 回落本 prim 内 ≤ D 的最深存在级（默认）
	l0 = 1,       ///< 回落级 0（对照臂）
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
	 * renderListSystem 每帧取 → fillSsrParams 写 scene data @723776/.yzw 与 @723792/.xyz
	 *（z = blurScale，批 15.3B 起占用，消费者 = ssr_apply）。
	 */
	SsrTuning ssr;
	/**
	 * 湿面调制的 6 枚**每帧**参数（configs/runtime.lua `wet` 块）。**每帧量**——与
	 * `ssr` 同构（湿面没有创建期开关：它不是独立 pass，是 deferred_geometry 的 G-buffer 调制，
	 * 不改图形态；语义与「身份不占参数」→ `WetTuning` 注释）。
	 * 消费链：main 启动期 `registry.ctx().emplace<WetTuning>(configs.runtime.wet)` →
	 * renderListSystem 每帧取 → fillWetParams 写 scene data @723968/.xyzw 与 @723984/.xy →
	 * shader 消费者 = deferred_geometry.frag.slang 湿面调制（15.2B 接线，默认开）。
	 */
	WetTuning wet;
	/**
	 * 簇化渲染路径（meshlet 产物）总门（configs/runtime.lua `meshlet` 块 `enable` 键，★0-b1）。
	 *
	 * 🔴 **★18-C 起默认开**（2026-10-12 用户拍板：连同 `meshlet.shadow_lod = 5` 出厂）。
	 * 这**没有废掉 P5**（「门关 = 逐位不变是硬护栏」仍是每批改渲染的必跑判据），改的只是
	 * **门关不再是出厂态** ⟹ 验收口径跟着变：想要门关必须**显式**给
	 * `--set meshlet.enable=false --set meshlet.shadow_lod=-1`（旧口径「出厂默认就是门关」已失效）。
	 * 出厂默认截图锚随之从 `54e40665…`（门关）换成 `95edc8cc…`（enable + shadow_lod=5）；
	 * 两个锚都记在 `docs/mdi_plan.md` §18.11，引用锚的判据必须同时写明是哪一态。
	 *
	 * 拍板时明写的三条代价（读数绑本机 AMD 8060S，→ §18.9）：
	 *  · 显存 **+175.5 MiB**（4 段簇 buffer，顶点池 138MB 占大头）
	 *  · 启动 **+1.2s**（解析 184MB 产物 + 上传）
	 *  · ~~**连带打开 α 影子叠加段 = 白付 1.59ms/帧**~~ —— 这条代价**已由债-105 摘掉**：叠加段
	 *    现在有自己独立的门 `meshlet.overlay`（默认关），本键开 ≠ 叠加段开。原委（α 是
	 *    ★1-a1ii-α 的验证脚手架、共面 + eLess 一个像素不写、却绑在产品路径开关上）→
	 *    `docs/mdi_plan.md` §18.11 欠账 ④ 与本文件 `meshletOverlay` 注释。
	 *
	 * 逐资产门不在本键 = 「该资产有没有簇产物」（§7.8 拍板 3：产物缺失 → 回落现行路径，
	 * 未打包资产零风险）。形态先例 = readSsrBlock（嵌套块，无扁平键双读兼容——两个真相源是
	 * 本项目反复登记的反模式）；★1-a1ii-β-1 起本块布尔走 readStrictBoolKey（类型错 = 启动失败，
	 * 理由 → 该函数注释）。gate_lint 不挂号的理由 → §18.10 欠账 ③。
	 */
	bool meshletEnable = true;
	/**
	 * α 影子**叠加段自己的**门（configs/runtime.lua `meshlet.overlay` 键，债-105，默认**关**）。
	 *
	 * 🔴 **本键的宿主是叠加段，不是 `meshlet.enable`**——生效条件里**不含** `enable`（同
	 * `meshlet.mdi_shadow` 的定性：`meshlet` 块里生效条件不依赖 `enable` 的键）。理由不是洁癖：
	 * 叠加条目的**烘焙**条件是 `wireMeshletBindings` 的 `slots`（= `enable`），而这些条目同时是
	 * β-1 `switch` 原位取代的宿主（`makeOpaqueCall` 读 `asset.meshletPipelineIndex`）⟹ 把烘焙挂到
	 * 本键门下会让 `switch` **静默失去宿主**（见 `bakeMeshletEntries` 的钉死注释）。本键只决定
	 * **采不采叠加 draw**，一条条目都不增减。
	 *
	 * **为什么要单独一枚**（债-105 的原文）：★18-C 翻默认后 `enable = true` 出厂，而叠加段的激活
	 * 条件当时只有「有簇管线条目 + 区间表存在」⟹ 白付 **+1571 draws / +1.59ms 每帧**买一个
	 * **一像素都不写**的验证脚手架（共面 + `eLess` 全拒）。α 已完工（★1-a1ii-α 的四臂判据），
	 * 不该再绑在产品路径的开关上。
	 *
	 * ⚠ `overlay = true` 而 `enable = false` 时确实**没有条目可采**（`slots = nullopt` = 烘焙整段
	 * 早退）——这种孤立组合装配层**出声**（`wireMeshletOverlay`），不静默空开关。
	 *
	 * 🔴 本键关 ⟹ `meshlet.debug` / `meshlet.tint` 这两枚**条目级 SPV 变体位**一律取 base
	 * （它们烘进的是 `switch` 也要用的那批条目，让叠加仪器改条目 = 诊断渗进产品取代路；
	 * 偏离默认时装配层 warn 出声，口径 → `wireMeshletOverlay` 变体选档注释）。
	 */
	bool meshletOverlay = false;
	/**
	 * 簇路径**阳性对照**档（configs/runtime.lua `meshlet.debug` 键，★1-a1ii-α）。
	 *
	 * = true（且 enable = true）时叠加条目改用 `meshlet.mesh_MESHLET_DEBUG_OFFSET.spv`
	 * 变体：clip.z 拉向相机 + 翻法线 ⟹ 叠加真正赢得深度测试并留下可见差异——判据 3
	 * 「画了相同内容 vs 什么都没画」的区分器（docs/mdi_plan.md §11）。门 = 选 SPV 变体
	 * 不是运行时 flag 分支（2026-10-02 slang fp 重排教训，同 IZB/PCSS/EVSSM 纪律）。
	 * 默认关；enable 关时本键无意义（条目根本不建）。
	 * 🔴 **overlay 关时本键被强制归 base**（债-105）——变体烘进的是 `switch` 原位取代共用的那批
	 * 条目，让叠加仪器改条目 = 把「clip.z 拉向相机」的几何错位带进产品路。偏离默认时装配层 warn
	 * 出声（`wireMeshletOverlay` 的变体选档）。
	 */
	bool meshletDebug = false;
	/**
	 * 簇路径叠加的**共面双臂**深度比较档（configs/runtime.lua `meshlet.coplanar` 键，★0.5-b2）。
	 *
	 * 叠加那批 draw 与旧路径几何**共面**，而 pass 的动态深度比较是 `eLess`（`shader.cppm`
	 * `RenderState::depthCompareOp` 默认值）⟹ 共面 ⟹ 深度测试全拒 ⟹ 一个像素都不写。
	 * 于是 α/α″ 的位比判据证明的是「旧路径没被碰坏」，**不是**「簇路径画得对」
	 * （`docs/mdi_plan.md` §14.1 两条变异钉死）。本键把叠加 draw 的深度比较换成
	 * `eEqual` / `eNotEqual`（**动态状态**，`depthCompareOp` 在本引擎是 per-draw 可改的，
	 * 不用新建管线），把屏幕切成两类且都要求逐位不变：
	 *  · `equal`：共面像素写回 ⟹ 与门关逐位同 = 属性/材质绑定/切线手性全对（盲区 = 不共面像素）
	 *  · `ne`：存在「簇覆盖但深度 ≠ 旧深度」即写 ⟹ 与门关逐位同 = 位置逐位同 + 覆盖集同
	 *    （盲区 = 簇没覆盖的像素，由已有的 `meshlet.debug` 覆盖臂补，M = 34.267%）
	 *
	 * 🔴 `off`（默认）= **一条 setDepthCompareOp 都不发** ⟹ 门关与 off 档的动态状态序列
	 * 逐 bit 不变（A1 判据的构造性保证）。未知值 = 启动 fail-fast（`aa` 三态键同纪律：
	 * 静默回落会让「四臂哈希全同」的主判据整批假绿）。仅 `meshletEnable` 为真时有意义。
	 * 🔴 **债-105 起还要 `meshlet.overlay` 为真才有消费者**：叠加段关 = 双臂没有任何 draw 可作用，
	 * 生效档 INFO 会当场点出这件事（不点出来会被读成「双臂还在跑」）。
	 */
	MeshletCoplanarMode meshletCoplanar = MeshletCoplanarMode::off;
	/**
	 * 簇路径叠加的 **tint** SPV 变体档（configs/runtime.lua `meshlet.tint` 键，★0.5-b2）。
	 *
	 * = true（且 enable = true）时叠加条目改用 `meshlet.mesh_MESHLET_TINT.spv`：着色输入
	 * 全写死成常数（normal/uv/tangent），**clip 一 bit 不动** ⟹ 深度与覆盖集与底逐位同，
	 * 只有写回的颜色不同（逐 primitive 平色块）。它是双臂的**阳性对照**：
	 *  · `coplanar=equal` + tint 图**必须变** = equal 臂的**非空洞证明**（若簇几何深度与旧
	 *    路径差哪怕 1 ULP，eEqual 处处拒绝 ⟹ 恒绿但毫无意义）
	 *  · `coplanar=off` + tint 必须仍 = 基线锚（证明变化来自「共面写回」，不是 tint 本身可见）
	 *
	 * 与 `meshletDebug` 同真时**本键优先**（判据 3 只用 tint、判据 4 只用 offset，从不同时
	 * 用，故不建第 4 个组合变体，改由装配层 warn 出声）。门 = 选 SPV 变体不是运行时 flag
	 * （2026-10-02 slang fp 重排教训，同 `meshletDebug`）。默认关；enable 关时无意义。
	 */
	bool meshletTint = false;
	/**
	 * 簇路径在 opaque 段**原位取代**旧 mesh draw 档（configs/runtime.lua `meshlet.switch`
	 * 键，★1-a1ii-β-1）。
	 *
	 * = true（且 `enable` = true）时，门开资产（有簇管线条目 + L0 簇数 > 0 + 非 blend）在
	 * 主 opaque 段里由簇路径 draw **1:1 原位**顶掉旧 mesh draw：同 objectIndex、同 pushData、
	 * 同段内位置，只换「管线条目 + task 数」（`meshletPipelineIndex` / `2 × l0Count`）。
	 * 取代发生在**原位**而不是另开一段，理由 = opaque 段序被 GPU culling 候选序对账
	 * （`collectCpuObjectSlots`）与 cull_driver 重放路强依赖，任何插段/换序都在非默认 cull
	 * 档打破对账；原位换 draw 内容而不动序列 = 对账零感知（★1-a1ii-α 的独立叠加向量正是
	 * 为了绕开这条才那样设计，本键是把它请回段内）。
	 *
	 * 三态语义（缺键 = false = 现行 α 形态 = 逐位不变护栏）：
	 *  · `enable = false`        → 本键无意义（簇条目根本没烘，无 draw 可取代）
	 *  · `enable && !switch`     → α 形态：旧路径照画 + 共面影子叠加
	 *  · `enable && switch`      → β 形态：簇路径顶掉旧 draw，**影子叠加同时清空**
	 *    （同一批几何不能既原位取代又再叠加画第二遍；互斥在装配层 warn 出声）
	 *
	 * 🔴 只在 `diagnostics.cull_replay == "cpu"`（默认档）生效：`gpu_sync` 档的 opaque 段由
	 * `cullDriver.executeOpaqueSegment` 按**候选表**重放（DGC 的 push 与 task 数按旧 mesh
	 * 管线烘焙），段内换管线不在那条路上。非默认档 + 本键 = **不切**、保持旧路径、启动
	 * warn 一次（判据点 = `wireMeshletSwitch`；静默切一半 = 最难查的假绿形态）。
	 * 边界（β-1 故意不做，归 β-2/β-3）：阴影/CSM、picking/objectID、IZB mask 重放、透明
	 * 排序仍走旧路径。
	 */
	bool meshletSwitch = false;

	/**
	 * ★1-a2-i 逐簇视锥剔除（`runtime.lua meshlet.cull`，默认关）。
	 *
	 * 开 = 每帧一次 compute 逐 L0 簇做保守视锥测试，mesh shader 对不可见簇
	 * `SetMeshOutputCounts(0,0)` 早退。判据与定价 → `docs/mdi_plan.md` §17。
	 *
	 * 🔴 依赖 `enable && switch`：本键只作用于**已被簇路径接管**的那批 draw（旧 mesh 管线
	 * 读不到标志数组）。单独开 = 无消费者，装配层 warn 出声（静默空开关是本项目反复踩的形态）。
	 * 与 `meshletSwitch` 同款限制：只在 `diagnostics.cull_replay == "cpu"` 生效。
	 */
	bool meshletCull = false;

	/**
	 * ★18-B1 阴影段簇路径的 **uniform 选级 D**（`runtime.lua meshlet.shadow_lod`）。
	 * 🔴 **★18-C 起默认 `5`**（2026-10-12 用户拍板「默认开 D=5」；数据推荐档 = 5.10% L0 三角，§18.2）。
	 *
	 * 三态语义（§18.7 已定案，别再改形态）：
	 *  · `-1` = 本档关：装配层一条阴影条目都不建、一行日志都不打，阴影全部走既有
	 *    `shadowPipelineIndex` 旧管线 ⟹ 逐位不变。🔴 **门关现在要显式给**（出厂不再是 -1）。
	 *  · `0` = 🔴 **阳性对照 L0 臂**（§18.6 判据 2）：同一新管线画每 prim 的 L0 前缀，先证明
	 *    「管线画的是对的几何」，再谈 level D。实测与门关**逐位相同**（depth-only 每纹素深度
	 *    = 覆盖它的图元之 min，与图元序无关）⟹ 这条同时是寻址正确性的证明。
	 *  · `N ≥ 1` = uniform 选级 D=N：每 prim 画 `min(D, 本 prim 最深级)` 那一段连续子区间
	 *    （§18.3：每 object 的第 D 级是单一连续区间；§18.4：uniform 整级不受 §7.4 那条
	 *    「group 粒度自适应选级」负知识影响，不读 error、不比阈值）。
	 *
	 * 与 `meshletEnable` 的关系：本键是**独立档**（阴影段的杠杆与主 pass 的取代档正交），但
	 * 簇四段 buffer 的描述符槽由 `meshlet.enable` 的 wireMeshletBindings 提供 ⟹ 生效需要
	 * `enable = true` 且本键 ≥ 0。🔴 两键**同时**给默认（`enable = true` + `shadow_lod = 5`）
	 * 正是为了不把本键放进「开了但不生效」的形态；只给本键不给 enable 时装配层出声
	 * （→ `warnIfShadowLodOrphaned`）。画质/成本对照表（D → diff 像素）→ §18.9 末。
	 * 🔴 类型必须是整数（走 readStrictIntKey，写成字符串/浮点 = 启动失败点名键，理由 →
	 * readStrictBoolKey 注释那条实测纪律）。负数除 -1 以外的值不当非法（本键只把 `< 0` 当
	 * 「关」这一件事读），但装配层不会为它们建条目以外的任何行为。
	 */
	int32_t meshletShadowLod = 5;
	/**
	 * ★18-B1 阴影段簇路径的**回落策略**（`runtime.lua meshlet.shadow_lod_fallback`，默认
	 * `"deepest"`）。仅在 `meshletShadowLod >= 0` 时有消费者。未知值 = 启动 fail-fast。
	 */
	MeshletLodFallbackMode meshletShadowLodFallback = MeshletLodFallbackMode::deepest;

	/**
	 * ★M-0（docs/mdi_plan.md §19.2 D2）阴影段**真 MDI** 档（`runtime.lua meshlet.mdi_shadow`，
	 * 默认关）。
	 *
	 * 开 = 阴影段 c1..c3 的簇路径 draw 由「逐候选一条 draw」换成「每级联一次
	 * `drawMeshTasksIndirectEXT`」（行表 = `mdi_rows.cppm` 的 24B 行数组）。
	 *
	 * 🔴 **本批（★M-0）零消费者**：行表构建器已落地但没有任何人调用它，本键被读到也不改变
	 * 任何一帧的行为（出厂默认态截图锚 `95edc8cc…` 必须逐位不变 = 本批的判据之一）。消费者 =
	 * ★M-1（GPU 载体 `MeshletShadowArgsService` + `MDI_SHADOW` SPV 变体 + 执行器分支）。
	 *
	 * 形态与 `meshletSwitch` / `meshletCull` 同款：**门 = 选 SPV 变体 / 选管线条目**，不是运行时
	 * flag（§12/§13 的 2026-10-02 slang fp 重排教训）⟹ 门关用的还是今天那份 SPV 与那条条目，
	 * 一字节不动 = P5 硬护栏的构造性成立。本键落进 `meshlet` 块 ⟹ 债-106（gate_lint 的 `Ctx`
	 * 不认整块）射程内，**不为它做特例**。
	 */
	bool meshletMdiShadow = false;

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
 * 解析器键轨迹（`DiagnosticsKeyTrace` 同形，债-11：docs/gate_lint_plan.md §3-B 的
 * 未知键 lint + 覆盖率护栏回灌 runtime.lua）。键名书写与 Lua 路径一致，嵌套块字段用
 * `表名.字段`（如 `meshlet.shadow_lod`）；数组形字段（headless_size/win_size 的 [1][2]）
 * 无键名，只记表名一个键。本 loader 没有门回显行 ⟹ 判据表叫 KnownKeys 而非 EchoKeys：
 * 与 kGateEcho 同角色（未知键 lint 的判据表 + 覆盖率断言的输入），只是无回显输出。
 */
export struct RuntimeKeyTrace {
	std::vector<std::string> read;     ///< 解析器读过的键（解析器总是全量问一遍 = 与文件内容无关）
	std::vector<std::string> present;  ///< 文件里实际出现的顶层键名（子表只记表名）
};

/**
 * 键表的全部键名（与 `RuntimeKeyTrace::read` 同拼写）——单测据此做「读过的键集合 ==
 * 键表键集合」的双向覆盖率断言（解析器读了而表不列 = 表撒谎，表列了而解析器不读 =
 * 陈旧键）；表本体在实现单元（不把 fmt 拉进本模块接口）。
 * @return 键名列表（顺序 = 键表顺序）
 */
export std::vector<std::string> runtimeConfigKnownKeys();

/**
 * 从 Lua 加载运行配置。文件缺失 → 默认 + warn；脚本/类型错误 → unexpected。
 * 沙箱限制同 camera_config（禁用 dofile/loadfile/package/require）。
 * @param path 配置文件路径
 * @param trace 可选出参：键轨迹（覆盖率护栏与未知键 lint 的输入；传 nullptr = 不记录）
 * @return 解析好的 RuntimeConfig；失败返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<RuntimeConfig, Error> loadRuntimeConfig(
	std::string_view path, RuntimeKeyTrace* trace = nullptr);

}  // namespace RGL
