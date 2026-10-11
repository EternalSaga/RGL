export module render_graph;

import rlstd;
import rlvk;
import vkerror;
import vk_swapchain;  // Swapchain&（rtslot 装配分区取 extent，架构债务续账步骤 2）
import render_target_cache;  // RenderTargetCache&（G-buffer/scene_hdr 装配，架构债务续账步骤 2）

// G-buffer 契约表转出口：表本体住在叶子模块 gbuffer_contract（纯 constexpr，无 Vulkan
// 类型），本模块 `export import` 转出，使既有消费方（render_graph_* 分区 /
// frame_capture_impl / shader_registry）**零改动**继续经 render_graph 拿到它。
// 为什么不直接让它们各自 import gbuffer_contract：那要在 6 个文件里加 import，
// 而"经 render_graph 拿"是既有约定（表与图编译器同源可读）；转出口让两条路都通。
export import gbuffer_contract;

// Phase 6：渲染意图图（声明 → 编译 → 执行三层中的"编译"层）。
//
// 定位：纯 C++ 数据结构 + GraphCompiler，零 Lua、零 entt、零 Vulkan 句柄——
// 输入 RgGraphDesc（attachment/pass 拓扑，可由 Lua/硬编码/测试构造），输出
// CompiledRenderGraph（逐 pass 的渲染附件描述 + barrier 计划），由 Renderer
// （执行层）在录制期消费。barrier 自动推导 = 本模块核心：attachment 的布局链
// 从 pass 声明的读写 usage 推导，执行器不再手写布局转换（Frostbite 第一代
// 全自动图的唯一保留核心；aliasing/pass fusion/多线程录制明确不做，见
// docs/next_roadmap.md Phase 6）。
//
// 图的边界（哪些同步**不**进图）：
//   - descriptor heap / sampler heap 的 host-write barrier 与 scene data
//     staging copy barrier 属"数据通路基础设施"（非 attachment 状态），留在
//     drawFrame 固定段（原 ⑥/⑥.5）。
//   - swapchain acquire/present 信号量、双 fence 帧槽复用属执行器时序，不进图。
//
// usage → {layout, stage, access} 推导规则（单一真相源 = impl 的 usageInfo 查表）：
//   colorWrite    → ColorAttachmentOptimal / 写 / ColorAttachmentOutput
//   depthWrite    → DepthStencilAttachmentOptimal / 读+写 / Early+LateFragmentTests
//   sampleRead    → ShaderReadOnlyOptimal / ShaderRead / AllGraphics
//   transferRead  → TransferSrcOptimal / TransferRead / Transfer
//   transferWrite → TransferDstOptimal / TransferWrite / Transfer
//
// 帧首规则（与既有实践逐位对齐，见 docs/技术概念.md "colorToDrawBarrier 的
// oldLayout"）：swapchain/depth 类 attachment 每帧首写 = oldLayout eUndefined +
// src TopOfPipe/eNone（内容按 loadOp 重建，旧布局不关心）；offscreen（持久图）
// 帧首 oldLayout = 上帧末真实布局（执行器跨帧追踪），src 仍 TopOfPipe/eNone
// （帧间已由 fence 同步）。帧内跨 pass 转换 src = 上次 usage 的 stage/access。
//
// blit pass（kind=blit）：无绘制，inputs[0](transferRead) → outputs[0]
// (transferWrite) 整幅拷贝（RGL_OFFSCREEN DIAG 转正的通路）。

namespace RGL {

/**
 * 附件种类：
 * - swapchain：后缓冲（image/view 由执行器录制期按 imageIndex 解析）；
 * - depth：深度图（Resources 持有）；
 * - offscreen：图属 transient RT（执行器按名确保分配；帧末布局跨帧追踪）；
 * - depthOffscreen：图属**深度** RT（阴影图类，批 11.2 基建）。与 offscreen 同为"执行器
 *   按名 ensure"，但三点不同，故不复用同一 kind：① 格式只收深度格式词表
 *   （offscreen 只收颜色词表——两侧各锁一侧，把"颜色 view 指向深度图"这类 invalid usage
 *   挡在编译期）；② aspect = eDepth（屏障 / view / 描述符全链走深度面）；③ 可带独立
 *   extent（见 `RgAttachmentDesc::extent`），阴影图分辨率与窗口解耦。
 */
export enum class RgAttachmentKind : uint8_t {
	swapchain,  // 后缓冲（image/view 由执行器录制期按 imageIndex 解析）
	depth,      // 深度图（Resources 持有）
	offscreen,  // 图属 transient RT（执行器按名确保分配；帧末布局跨帧追踪）
	depthOffscreen,  // 图属深度 RT（阴影图类；深度词表 + eDepth aspect + 可独立 extent）
};

/**
 * attachment 在 pass 中的角色。写角色进 outputs，读角色进 inputs。
 * usage → {layout, stage, access} 推导规则单一真相源 = impl 的 usageInfo 查表（见模块头注释）。
 */
export enum class RgUsage : uint8_t {
	colorWrite,    // 渲染颜色附件（dynamic rendering color attachment）
	depthWrite,    // 渲染深度附件（每 pass 至多一个）
	sampleRead,    // 图形阶段 shader 采样读
	transferRead,  // blit/copy 源读
	transferWrite, // blit/copy 目标写
};

/** 加载操作（仅写角色有意义）：clear = 清值重建，load = 保留上帧内容。 */
export enum class RgLoadOp : uint8_t { clear, load };

/** 存储操作：store = 保留内容，dontCare = 可丢弃（后续再使用的写会被 compile 拒绝）。 */
export enum class RgStoreOp : uint8_t { store, dontCare };

/** 清除值：kind=depth 用 depth，其余用 color（执行器按 attachment kind 择一）。 */
export struct RgClearValue {
	std::array<float, 4> color{0.1f, 0.1f, 0.1f, 1.0f};
	float depth = 1.0f;
};

/**
 * 附件声明（RgGraphDesc.attachments 元素）：name 全局唯一（编译产物中的 slot 下标）；
 * kind 决定资源来源；format 仅 offscreen/depthOffscreen 可取词表具名格式（各查各表），
 * swapchain/depth 必须 "inherit"（格式由执行器固定，compile 校验）。
 */
export struct RgAttachmentDesc {
	std::string name;
	RgAttachmentKind kind = RgAttachmentKind::swapchain;
	std::string format = "inherit";  // "inherit" = 随 swapchain/depth 现有格式；
	                                 // offscreen 可用具名格式（词表见 isKnownRgFormat）
	// 图属 RT（offscreen / depthOffscreen）专属尺寸：{0,0} = 跟随 swapchain extent
	// （scene_hdr / G-buffer / ao 全是这一档，随窗口重建）；非 0 = **独立分辨率**。
	// 阴影图必须走独立档——把阴影图绑在窗口尺寸上是错的（窗口缩一半阴影 texel 跟着变，
	// 且每次 resize 白白重建整张阴影图）。swapchain/depth 附件必须留 {0,0}（compile 校验）。
	std::array<uint32_t, 2> extent{0u, 0u};
	// **跟随窗口的缩放档**（批 15.3 子步 B；仅 kind=offscreen 合法，compile 校验）：
	// 0 = 跟随（现状逐位不变）；(0,1] = 渲染 extent × 本值（向下取整、下限 1，口径唯一
	// 落点 = scaleRtExtent）。与上面静态 extent 的差别 = 它**随窗口/resize 走**——
	// ssr_trace 半分辨率 march 要的是「渲染网格的一半」这种派生尺寸，不是独立分辨率。
	// 两者同时给时 extent 优先（scale 只作用于「跟随」档）。写进本字段的 pass 其渲染区域
	// 随缩放后的 RT extent（SlotResolver::renderRectFor 第 ② 档，viewport/renderArea 同源）。
	float extentScale = 0.0f;
	// **跨帧历史附件**（TAA 类；仅 kind=offscreen 合法，compile 校验）：语义 =
	// "本帧第一手读它时，读的是**上一帧留下的内容**"。三处后果（都在 compile 落实）：
	//   ① 豁免"读前置"校验（普通附件本帧未写先读 = 报错，历史附件必须允许）；
	//   ② 帧内必须至少有一个 pass 写它（否则永远读未初始化图，只能编译期抓）；
	//   ③ 帧首读它的 barrier src 从 TopOfPipe/eNone 提到 **AllCommands/MemoryWrite**
	//      ——跨 submission 的 RAW 依赖（kFramesInFlight=2 下帧 N 与 N-1 可重叠，
	//      TopOfPipe 不建立任何依赖；论证见 docs/taa_plan.md §2 gap ①）。
	bool history = false;
};

/**
 * 缩放 RT extent 的**口径唯一落点**（批 15.3 子步 B）：scale ≤ 0 = 原样跟随；否则
 * base × scale **向下取整、下限 1**。两处消费必须共用本函数——执行器逐帧装配
 * （Renderer::ensureOffscreenAttachments）与启动期领槽（ensureSsrTraceSlot）各写一份
 * floor = 两个真相源，漂移的形态 = 每帧 RT 重建（尺寸永远对不上缓存）。
 */
export inline constexpr vk::Extent2D scaleRtExtent(vk::Extent2D base, float scale) {
	if (scale <= 0.0f)
		return base;
	const auto one = [](uint32_t v, float f) {
		return std::max(1u, static_cast<uint32_t>(static_cast<float>(v) * f));
	};
	return {one(base.width, scale), one(base.height, scale)};
}

/**
 * offscreen attachment 格式词表成员判定 = compile 校验入口（"inherit" 与全部具名为 true）。
 * 词表（字符串 → vk::Format，小写下划线命名，表驱动、扩展 = impl 加一行）：
 * "inherit" = 随 swapchain 格式（执行器解析 fallback）；具名覆盖 Phase 7 G-buffer 类
 * 显式 RT 需求（rgba8_srgb/rgba8_unorm/b8g8r8a8_srgb/rgb8_srgb/rg16_snorm/rg16_unorm/
 * rgba16_sfloat/rgb16_sfloat/r32g32b32_sfloat/rgba32_sfloat/r32_sfloat/rg32_sfloat/
 * r32_uint）。
 * @param name 格式名（小写下划线）
 * @return true = 词表内（含 "inherit"）
 */
export bool isKnownRgFormat(const std::string& name);
/**
 * 执行器物化 vk::Format："inherit"→inheritFallback，具名→词表映射值（颜色词表
 * `rgFormatTable` 未命中再查深度词表 `kDepthRgFormats`，两表分立见其注释）。
 * @param name 格式名（调用方保证已 isKnownRgFormat / isKnownDepthRgFormat）
 * @param inheritFallback "inherit" 的替代格式（通常 = swapchain 格式）
 * @return 物化格式；漏网（未校验）返回 eUndefined
 */
export vk::Format resolveRgFormat(const std::string& name, vk::Format inheritFallback);

/**
 * **深度**附件格式词表成员判定（kind=depthOffscreen 专用；与 isKnownRgFormat 的
 * **颜色**词表刻意分成两张表）：若把 d32_sfloat 塞进通用词表，`kind=offscreen` 就能声明
 * 深度格式 → RT 缓存按颜色建它（COLOR_ATTACHMENT 用途 + eColor aspect view）= invalid
 * usage，而且只有 VVL 抓得到。两表分立让"offscreen 恒颜色 / depthOffscreen 恒深度"
 * 成为编译期可校验的不变式。
 *
 * 词表（实现侧 rgDepthFormatTable，与颜色词表同样住在 impl——`vk::Format::eXxx` 枚举常量
 * 在模块接口分区里**不可见**：rlvk 的 GMF 头是"可达但名字不可见"，写出来直接编译报错）：
 *   "d32_sfloat"（主用档，与主深度图同格式；Vulkan **没有** D32_UNORM 这个格式）/
 *   "d16_unorm"（省带宽档，多级 cascade 用）。
 * 选格式注意：**采样侧不可过滤**（深度格式非 filterable），阴影采样必须用 NEAREST
 * sampler（同 ao 的教训，见 sampler_library::ensurePointSampler）。
 * @param name 格式名（小写下划线）
 * @return true = 深度词表内（"inherit" **不算**——深度 RT 必须显式给格式）
 */
export bool isKnownDepthRgFormat(const std::string& name);

/**
 * pass 内一条附件引用（RgPassDesc.outputs/inputs 元素）：attachment 名引用
 * RgAttachmentDesc；usage 定角色（写进 outputs / 读进 inputs）；loadOp 仅写角色
 * 有意义（读角色忽略）；clear 在 loadOp=clear 时生效。
 * 同一 attachment 在同一 pass 内至多出现一次（compile 拒绝读写 feedback）。
 */
export struct RgPassAttachment {
	std::string attachment;  // 引用 RgAttachmentDesc.name
	RgUsage usage = RgUsage::colorWrite;
	RgLoadOp loadOp = RgLoadOp::clear;   // 仅写角色有意义；读角色忽略
	RgStoreOp storeOp = RgStoreOp::store;
	RgClearValue clear{};
};

/**
 * pass 种类：
 * - drawMesh：执行 FrameDrawList 段（drawSource → 段边界映射由执行器完成）；
 * - blit：transferRead input → transferWrite output 整幅拷贝（无绘制）；
 * - drawLight（Phase 7 D1 / Phase 8 ①泛化）：固定全屏单 group draw——执行器按
 *   **state 名**绑定预建管线条目（该 kind 的 state 词表 = pass_vocabulary 的
 *   fullscreenPass 档），不走 FrameDrawList 段；outputs 恰一 colorWrite、
 *   inputs = sampleRead（barrier 自动）。
 */
export enum class RgPassKind : uint8_t {
	drawMesh,  // 执行 FrameDrawList 段（drawSource → 段边界映射由执行器完成）
	blit,      // transferRead input → transferWrite output 整幅拷贝
	// 固定全屏单 group draw（Phase 8 ①起 state 名泛化，不再限 "light"）
	drawLight,
};

// pass.state / pass.draws 两个字符串字段的**全部合法值**已迁入叶子模块
// pass_vocabulary（2026-10-02）：state 名 = `kAllPassStates` 按 `RgStateScope` 分档
// 派生出 `kMeshPassStates`（drawMesh 用）与 `kFullscreenPassStates`（drawLight 用；
// state 名同时是 ShaderRegistry 固定条目名——"light" = deferred 逐灯 GGX、
// "ao" = 屏幕空间环境光遮蔽、"tonemap" = HDR→LDR ACES，执行器构造期按名取条目）；
// 段名 = `kAllDrawSources`。两档校验不可并成一张扁平表（否则 kind 与 state 串味的
// 图会放行到执行器深处）——理由与红线写在那张表的头注释里。

/**
 * Phase 8 批次 ①：HDR 累加链附件契约（单一真相源）——forward/deferred 图的颜色目标
 * scene_hdr（rgba16_sfloat offscreen RT），tonemap pass 采样后写 swapchain。
 * 消费方 = render_graph_builtin 三图描述 + Resources::ensureSceneHdrSlot（启动期建
 * RT + 采样堆槽）+ addTonemapEntry（烘焙 binding 4）。
 */
export inline constexpr std::string_view kSceneHdrName = "scene_hdr";
export inline constexpr std::string_view kSceneHdrFormat = "rgba16_sfloat";

/**
 * Phase 11 批 11.4：SSAO 附件契约（单一真相源）——ao 遮蔽量 RT（r8_unorm offscreen），
 * ao pass（state="ao"）写、tonemap pass 采样施加。消费方 = forward/deferred 图描述 +
 * ensureAoSlot（启动期建 RT + 采样堆槽）+ addTonemapEntry（烘焙 binding 6）。
 */
export inline constexpr std::string_view kAoName = "ao";
export inline constexpr std::string_view kAoFormat = "r8_unorm";

/**
 * 批 12.6 TAA：时域历史链两附件契约（单一真相源）。
 *  - `kTaaOutName` = 本帧 TAA 结果（taa pass 写、tonemap 采）；
 *  - `kTaaHistName` = **跨帧历史**（history=true：taa pass 首手读它拿上一帧结果，
 *    帧末 `taa_commit`(kind=blit) 把 taa_out 整幅拷回它）。
 * 两者同格式（与 scene_hdr 同为 rgba16_sfloat——TAA 在 tonemap 之前的线性 HDR 域）。
 * 消费方 = decorateTaaPass（图声明）+ ensureTaaSlots（启动期建 RT + 采样堆槽）+
 * addTaaEntry / addTonemapEntry（烘 binding）。
 */
export inline constexpr std::string_view kTaaOutName = "taa_out";
export inline constexpr std::string_view kTaaHistName = "taa_hist";
export inline constexpr std::string_view kTaaFormat = "rgba16_sfloat";
/** TAA pass 名与 commit pass 名（图内唯一；装饰器与消费方共用，避免字面量漂移）。 */
export inline constexpr std::string_view kTaaPassName = "taa";
export inline constexpr std::string_view kTaaCommitPassName = "taa_commit";

/**
 * Phase 14 批 14.4：逐像素速度场附件契约（单一真相源）。
 *
 * `velocity` RT（state="velocity" 的全屏 pass 写）装的是「本像素表面点在上一帧的像素位移」，
 * 口径 = **渲染分辨率像素 · prev − cur · y 向下**（`.xy` = 速度、`.z` = 有效标志、`.w` 弃用）。
 * 消费方 = 批 14.5 的 FSR upscaler（`motionVectors` 硬要求）+ 诊断回读。
 *
 * **格式刻意复用 `kTaaFormat`（rgba16_sfloat）而不是新开 rg16_sfloat**：新格式要在
 * `render_graph_formats` 词表 + `frame_capture` 的 `captureBpp` + `llm_debug` 的通道转换器
 * **三处**同时挂号，而漏掉后两处**不崩不报错**（staging 按错字节数算 → 捕获尾部是上一次
 * 残留，见 `frame_capture_impl.cppm` 头注释的 ⚠）。多花的 3.7 MB@720p 买掉这一整类风险。
 * 精度足够：FP16 是**相对**精度，速度量级 1–300 px 时 ulp ≈ 0.001–0.15 px。
 */
export inline constexpr std::string_view kVelocityName = "velocity";
export inline constexpr std::string_view kVelocityFormat = "rgba16_sfloat";
export inline constexpr std::string_view kVelocityPassName = "velocity";

/**
 * Phase 14 批 14.5 第三批：**AO 施加专用 pass** 的附件契约（单一真相源）。
 *
 * `ao_applied` RT 装的是「本帧成片 × AO 可见度」= **时域滤波器的输入**。它的存在把
 * 「AO 必须乘在时域之前」这条批 13.6 的不变量与「谁做时域滤波」**解耦**——此前 AO 乘法
 * 长在 taa.frag 里，换成 FSR 当时域滤波器就会退回 13.6 修掉的形态（AO 落回 tonemap =
 * 时域之后 = 噪声零平均，实测占残余 shimmer 62%）。推导全文 →
 * `shaders/ao_apply.frag.slang` 头注释与 `phase14_plan.md` §1.15。
 *
 * 格式沿用 rgba16_sfloat（同 scene_hdr / taa_out：tonemap 之前的线性 HDR），
 * 理由与精度口径同 `kVelocityFormat` 那条（新开格式要三处同时挂号）。
 */
export inline constexpr std::string_view kAoAppliedName = "ao_applied";
export inline constexpr std::string_view kAoAppliedFormat = "rgba16_sfloat";
export inline constexpr std::string_view kAoApplyPassName = "ao_apply";

/**
 * Phase 15 批 15.1 前半：**SSR（屏幕空间反射）pass** 的附件契约（单一真相源）。
 *
 * 批 15.3 子步 B 起 SSR = **两 pass 两附件**：
 *  - `ssr_trace`（**半分辨率**，premultiplied 反射贡献 `rgb = weight·(F·reflected − iblSpec)`、
 *    `a = weight`）由 trace pass（state="ssr_trace"）写；
 *  - `ssr_applied`（渲染分辨率，= 成片 + 粗糙度感知模糊后的反射）由 apply pass（state="ssr"）
 *    写，消费者链（tonemap / taa / ao_apply / FSR）零改动。
 * 拆分的论证与 premultiplied 契约 → `shaders/ssr_trace.frag.slang` 文件头。
 *
 * 格式**必须**与 `kSceneHdrFormat` 同为 rgba16_sfloat：apply 的直通早退路径要把 scene_hdr 的
 * Load 原值逐位写进 ssr_applied（「SSR 未作用 = 逐位相同」影响域判据的前提），格式不同连
 * 「拷贝都逐位」都不成立（新开格式还要在 render_graph_formats 词表 + frame_capture 的
 * captureBpp + llm_debug 通道转换器三处挂号，理由同 `kVelocityFormat`）。
 *
 * 消费方 = decorateSsrPass（图声明）+ ensureSsrTraceSlot / ensureSsrAppliedSlot（启动期建
 * RT + 采样堆槽）+ addSsrTraceEntry / addSsrApplyEntry / assembleHdrAoChain 色输入优先级链
 *（烘 binding）。
 */
export inline constexpr std::string_view kSsrAppliedName = "ssr_applied";
export inline constexpr std::string_view kSsrAppliedFormat = "rgba16_sfloat";
/** apply pass 名（**沿用 15.1 的 "ssr"**——消费者链 / 日志 / smoke 判据零改动）与 state 名同源。 */
export inline constexpr std::string_view kSsrPassName = "ssr";
/** trace pass 的附件 / pass 名 / 格式 / 缩放（批 15.3 子步 B；缩放口径 → scaleRtExtent）。 */
export inline constexpr std::string_view kSsrTraceName = "ssr_trace";
export inline constexpr std::string_view kSsrTracePassName = "ssr_trace";
export inline constexpr std::string_view kSsrTraceFormat = "rgba16_sfloat";
export inline constexpr float kSsrTraceScale = 0.5f;

/**
 * pass 声明（RgGraphDesc.passes 元素）：name 必填；kind 定执行语义；
 * state = RenderState 名（名字字符串，零 handle，执行器解析；drawMesh 必填，blit 忽略；
 * 合法值 = pass_vocabulary 按 kind 分档的 state 子集）；drawSource = FrameDrawList 段名
 *（drawMesh 必填；合法值 = kAllDrawSources）。outputs = 写角色、inputs = 读角色。
 */
export struct RgPassDesc {
	std::string name;
	RgPassKind kind = RgPassKind::drawMesh;
	std::string state;       // RenderState 名（名字字符串，零 handle，执行器解析）；
	                         // drawMesh 必填，blit 忽略；合法值见 pass_vocabulary::kAllPassStates
	std::string drawSource;  // FrameDrawList 段名（合法值见 pass_vocabulary::kAllDrawSources）；drawMesh 必填
	// 本 pass 的渲染区域（viewport + scissor）相对附件原点的偏移 / 尺寸。
	// 默认 {0,0,0,0} = 跟随本 pass 的渲染区域（交易窗口尺寸，或写 depthOffscreen 时
	// 该 RT 自己的 extent）——**既有 pass 逐位不变**。
	// 用途：阴影图集的 4 个分块（各一级一个 pass）同临一张图、各只渲染自己那一块
	// 子矩形——每个 pass 一张图 = 4 张独立图 + 4 套附件/堆槽/绑定（而 forward/deferred
	// 两路要各绑 4 个 = 8 个端）。子矩形同时是 **renderArea**（限定 clear 与渲染区）。
	std::array<uint32_t, 4> viewport{0u, 0u, 0u, 0u};  // {x, y, w, h}；w == 0 = 跟随渲染区域
	std::vector<RgPassAttachment> outputs;
	std::vector<RgPassAttachment> inputs;
};

/**
 * 渲染意图的完整声明：attachments 下标 = 编译产物中的 slot；passes 线性序 = 执行序
 *（骨架版不做拓扑排序/死 pass 消除，脚本序即 GPU 序）。
 */
export struct RgGraphDesc {
	std::vector<RgAttachmentDesc> attachments;
	std::vector<RgPassDesc> passes;
};

export using RgAttachmentSlot = uint32_t;
export constexpr RgAttachmentSlot kRgNoAttachment = 0xFFFFFFFFu;

/**
 * 编译后的附件记录（slot = 向量下标）：kind/format 透传声明，aspect 按 kind
 * 推导（depth/depthOffscreen → eDepth，其余 → eColor）；frameStartDiscard 定帧首写
 * 是否走"内容丢弃"规则（swapchain/depth/depthOffscreen = true；offscreen = false 需
 * 跨帧追踪布局）。
 */
export struct RgCompiledAttachment {
	std::string name;
	RgAttachmentKind kind = RgAttachmentKind::swapchain;
	std::string format = "inherit";
	// 独立 RT 尺寸透传（0 = 跟随 swapchain）：执行器装配 RT 时消费（批 11.2 阴影图）。
	// 非图属 RT 恒 {0,0}（registerAttachment 已校验）。
	std::array<uint32_t, 2> extent{0u, 0u};
	// 跟随缩放档透传（批 15.3 子步 B；0 = 跟随）：执行器装配 RT（scaleRtExtent）与
	// renderRectFor（渲染区域随缩放后 extent）消费。非图属 RT 恒 0（registerAttachment 已校验）。
	float extentScale = 0.0f;
	vk::ImageAspectFlags aspect = {};
	// 帧首写是否走"内容丢弃"规则（swapchain/depth/depthOffscreen = true；offscreen = false
	// 需追踪布局。depthOffscreen 归"丢弃"档：阴影图每帧整幅 clear，无跨帧内容依赖）
	bool frameStartDiscard = false;
	// 跨帧历史附件（透传 RgAttachmentDesc::history；true ⟺ kind==offscreen）。
	// 消费方 = planBarrier（帧首 barrier src 提档）+ 执行器 RT 装配（历史图不得 discard）。
	bool history = false;
};

/**
 * 录制期一条 attachment 布局转换（执行器物化为 ImageMemoryBarrier2，
 * image/view/oldLayout 由执行器按 slot 解析——编译器只产语义计划）。
 * oldLayout 来源：oldLayoutDiscard = true → eUndefined（帧首丢弃）；
 * false → 执行器追踪布局（帧首 = 上帧末；帧内 = 本帧上一条针对同
 * attachment 的计划的 newLayout）。srcStage/srcAccess = 上次 usage 的
 * stage/access（帧首 = TopOfPipe/eNone）。
 */
export struct RgBarrierPlan {
	RgAttachmentSlot attachment = kRgNoAttachment;
	vk::ImageAspectFlags aspect = {};
	// oldLayout 来源：true = eUndefined（帧首丢弃）；false = 执行器追踪布局
	// （帧首 = 上帧末；帧内 = 本帧上一条针对同 attachment 的计划的 newLayout）
	bool oldLayoutDiscard = false;
	vk::ImageLayout newLayout = vk::ImageLayout::eUndefined;
	vk::PipelineStageFlags2 srcStage = {};
	vk::PipelineStageFlags2 dstStage = {};
	vk::AccessFlags2 srcAccess = {};
	vk::AccessFlags2 dstAccess = {};
};

/** 一个渲染附件在 beginRendering 中的条目（layout/loadOp/storeOp/clear 已定）。 */
export struct RgAttachmentPlan {
	RgAttachmentSlot slot = kRgNoAttachment;
	vk::ImageLayout layout = vk::ImageLayout::eUndefined;
	vk::AttachmentLoadOp loadOp = vk::AttachmentLoadOp::eClear;
	vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eStore;
	vk::ClearValue clearValue{};
};

/**
 * 编译后的 pass 计划：barriers = 本 pass 前发射的布局转换（单 DependencyInfo 批量）；
 * colors 顺序 = 声明 outputs 中 colorWrite 序 = color index；depth = 至多一个
 * depthWrite 条目；blitSource/blitTarget = blit pass 的 transferRead/transferWrite
 * 两端（仅 kind=blit 有值）。
 */
export struct RgPassPlan {
	std::string name;
	RgPassKind kind = RgPassKind::drawMesh;
	std::string state;
	std::string drawSource;
	std::vector<RgBarrierPlan> barriers;  // 本 pass 前发射（单 DependencyInfo 批量）
	std::array<uint32_t, 4> viewport{0u, 0u, 0u, 0u};  // 见 RgPassDesc::viewport（{0,0,0,0} = 跟随渲染区域）
	std::vector<RgAttachmentPlan> colors;  // 顺序 = Lua outputs 中 colorWrite 序 = color index
	std::optional<RgAttachmentPlan> depth;
	std::optional<RgAttachmentSlot> blitSource;
	std::optional<RgAttachmentSlot> blitTarget;
};

/**
 * 编译产物（compileRenderGraph 输出，Renderer 录制期消费）：
 * attachments（slot 表）+ passes（执行序）+ 帧末 present 转换 + submit wait stage。
 * - presentTransition：帧末 swapchain → PresentSrcKHR（无 swapchain 写入的图 =
 *   nullopt，执行器跳过；present 语义本身仍归执行器）；
 * - swapchainWaitStage：submit 的 imageAvailable wait stage = swapchain 附件在
 *   命令流中首次实际使用的 dstStage（编译期派生；现单段直渲 =
 *   ColorAttachmentOutput、offscreen 变体 blit 图 = Transfer，与旧手工分支逐位一致）；
 * - warnings：非致命提示（如 attachment 从未被任何 pass 引用）。
 */
export struct CompiledRenderGraph {
	std::vector<RgCompiledAttachment> attachments;
	std::vector<RgPassPlan> passes;
	// 帧末 swapchain → PresentSrcKHR（无 swapchain 写入的图 = nullopt，
	// 执行器跳过 ⑫；present 语义本身仍归执行器）
	std::optional<RgBarrierPlan> presentTransition;
	// submit 的 imageAvailable wait stage = swapchain 附件在命令流中首次实际使用
	// 的 dstStage（编译期派生；现单段直渲 = ColorAttachmentOutput、offscreen 变体
	// blit 图 = Transfer，与旧手工分支逐位一致）
	vk::PipelineStageFlags2 swapchainWaitStage = vk::PipelineStageFlagBits2::eTopOfPipe;
	std::vector<std::string> warnings;
};

/**
 * **图事实层**判据：编译图里是否有 deferred 光照 pass（`kind == drawLight` 且
 * `state == kDeferredLightState`）。
 *
 * 它与配置层的 `RgGraphVariant::deferred` **是两件事**（2026-10-02 收口：此前
 * IZB 与 EVSSM 各写一份逐字相同的 `any_of` 现推）：消费方要的是"有没有人在跑 deferred
 * 光照 / 有没有 G-buffer 可消费"，不是"配置里 variant 写了什么"——自定义 Lua 图可以在
 * forward 变体下手写 light pass。两个真相源的分歧交给 gate_lint 交叉校验
 * （docs/gate_lint_plan.md §3-A），**不在这里抹平**。
 * @param passes 编译图 pass 序列（`CompiledRenderGraph::passes`）
 * @return true = 图里有 deferred 光照 pass
 */
export bool graphHasDeferredLight(std::span<const RgPassPlan> passes);

/**
 * 物化：语义计划 + 执行器解析出的 image/oldLayout → ImageMemoryBarrier2
 *（oldLayout 由执行器按 plan.oldLayoutDiscard 给 eUndefined 或跨帧/帧内追踪值）。
 * @param plan 编译产出的语义 barrier 计划
 * @param image 目标 image 句柄（执行器按 slot 解析）
 * @param oldLayout 旧布局（执行器追踪值或 eUndefined）
 * @return 填好的 vk::ImageMemoryBarrier2（queue family 均 IGNORED）
 */
export vk::ImageMemoryBarrier2 materializeBarrier(const RgBarrierPlan& plan, VkImage image,
	vk::ImageLayout oldLayout);

/**
 * 校验 + 布局链推导 + barrier 计划生成（RgGraphDesc → CompiledRenderGraph）。
 * 失败返回带定位信息的 RenderGraphInvalid Error（名字引用、同 pass 读写冲突、
 * 读先于写、后续使用的写丢 store、swapchain 永不写等，见 impl）。
 * @param desc 渲染意图声明
 * @return 编译产物；校验失败返回 Error
 */
export [[nodiscard]] std::expected<CompiledRenderGraph, Error> compileRenderGraph(const RgGraphDesc& desc);

/**
 * 缺省图 = forward HDR 链（Phase 8 批次 ①）：opaque/skybox/transparent 三段渲入
 * scene_hdr（rgba16_sfloat，中间 attachment store 保内容、末段 depth DontCare）+
 * tonemap pass（drawLight）ACES 上屏 swapchain。原三段直写 swapchain 的 diff=0
 * 基准随 ACES 换 baseline。进程内静态单例，首次调用时编译（失败抛 runtime_error）。
 * @return 编译好的默认图（引用，生命周期进程级）
 */
export const CompiledRenderGraph& defaultRenderGraph();
/** defaultRenderGraph 对应的图声明（静态单例；测试/变体构造的参照）。 */
export const RgGraphDesc& defaultRenderGraphDesc();

/**
 * offscreen 图变体（Phase 6 批次 4，旧 DIAG（RGL_OFFSCREEN env，已转 variant 配置）转正）：
 * 三段渲入 offscreen 附件 scene_rt（format=inherit → 执行器解析为 swapchain 格式，
 * blit LINEAR 的 VVL 格式兼容前提）+ composite blit pass 上屏。present 转换 src 自动
 * 变 Transfer（blit 是 swapchain 最后写），submit waitStage 派生 = Transfer——
 * 与旧手工分支语义等价。静态单例。
 * @return offscreen 变体图声明（引用）
 */
export const RgGraphDesc& offscreenRenderGraphDesc();

/**
 * Phase 7 D1（render_graph.lua variant="deferred"）：3-pass deferred 图——geometry pass
 * 把 opaque 段渲入 5 个 G-buffer RT（albedo rgba8_srgb / normal rg16_snorm oct /
 * worldPos rgba32_sfloat / metalRough rgba8_unorm / emissive rgba16_sfloat，显式格式见
 * kGBufferAttachments 词表）+ depth；light pass（drawLight kind）全屏采样 5 RT + 逐灯 GGX
 * 渲入 scene_hdr；
 * tonemap pass（drawLight，Phase 8 批次 ①）ACES 上屏 swapchain（present 由图派生）。
 * 本图为 exclusive 变体。静态单例。
 * @return deferred 图声明（引用）
 */
export const RgGraphDesc& deferredRenderGraphDesc();

/**
 * picking pass 装饰（Phase 10 批 10.5，D3/D10）：给图追加 object-id offscreen 附件
 * （r32_uint，clear 0 = 未命中）+ 一个 picking drawMesh pass（state="picking"、
 * drawSource="opaque" —— 执行器以克隆管线条目重放 opaque 段，见 vkrenderer），插在
 * **首个带 depth 输出的 opaque drawMesh pass 之后**（scene depth 已写 = 点选做 depth
 * 测试得可见层；picking 管线侧 depthWrite=false，不污染后续 transparent 段）。
 * 调用点 = main 装配层且 `diagnostics.picking` 门控（默认关 = 不调用 = 图逐位同，D10）。
 * 无 depth 输出 opaque pass 的图（offscreen DIAG 变体）不支持：原样不改返回 false。
 * @param desc 图声明（原地修改：追加 attachment + pass）
 * @return true = 已装饰；false = 图形态不支持（desc 未被修改）
 */
export bool decoratePickingPass(RgGraphDesc& desc);

/**
 * 高亮 pass 装饰（Phase 10 批 10.7）：给图追加一个 highlight drawMesh pass
 * （state="highlight"、drawSource="opaque" —— 执行器以克隆管线重放并**按材质过滤**
 * opaque 段，只画选中材质的 draw，见 vkrenderer）。
 *
 * 与 picking pass 的三点关键差异：
 *  1. **不新增附件**——写既有场景色附件（loadOp=load 保留已渲画面）+ 既有 depth
 *     （loadOp=load，depthWrite=false 见 state_kit highlightState），高亮以 alpha
 *     混合叠加在成片上。故格式/尺寸天然与被高亮内容一致，无 RT 分配开销。
 *  2. **插在「首个采样场景色附件的 pass」之前**（forward/deferred = tonemap 前，
 *     offscreen = composite blit 前），而非图末——图末会落在 tonemap 之后，此时色附件
 *     已无人采样，高亮画了也不可见。附件名由消费 pass 的 sampleRead 输入反查，不硬编码。
 *  3. 借 depth 前把上一个写 depth 的 pass 的 storeOp 从 dontCare 改成 store（forward 图
 *     transparent 段按「depth 后续无人用」声明 dontCare）——否则高亮读到未定义内容。
 *
 * 调用点 = main 装配层且 `diagnostics.picking` 门控（与 picking 同门：默认关 =
 * 不调用 = 图逐位同，维持 D10 的 BIT-IDENTICAL 基线）。
 * @param desc 图声明（原地修改：追加 pass + 修正前一 pass 的 depth storeOp）
 * @return true = 已装饰；false = 图形态不支持（desc 未被修改）
 */
export bool decorateHighlightPass(RgGraphDesc& desc);

/**
 * 阴影深度图附件契约（单一真相源，Phase 11 批 11.2 基建）——shadow pass 写的
 * depthOffscreen RT。消费方 = decorateShadowPass（图声明）+ 执行器 RT 装配 +
 * 后续阴影采样侧（批 11.2 剩余部分）。
 */
export inline constexpr std::string_view kShadowMapName = "shadow_map";
export inline constexpr std::string_view kShadowMapFormat = "d32_sfloat";

/**
 * 阴影图**图集整幅**（边长）——固定不随级联数变，分块边长 = 整幅/列数。
 *
 * 定义在本模块（而非 scene_data）是为了让 render_graph 这个**纯意图模块**不拉 vk_resources（场景数据布局无需
 * 知道阴影图多大），也让它与 kShadowMapName / kShadowMapFormat 住成同一组契约。
 * 2048 = 单级时的整幅（= 最高分辨率）；加级联后每块变细（见 shadowAtlasColumns）。
 */
export inline constexpr uint32_t kShadowMapSize = 2048;

/** 级联分块在 2×2 图集上的子矩形（视口 + renderArea）。
 *  N=4 时图集 2S×2S，四块各 S×S：cascade0 (0,0) / 1 (S,0) / 2 (0,S) / 3 (S,S)。
 *  采样端用同一块置把光空间 uv 叠上来（shadow.slang readShadowAtlas）——两侧必须同源，改一处即可。
 *  @param c 分块号
 *  @param tileSize 单块边长
 *  @return {x, y, w, h}（与 RgPassDesc::viewport 同布局）
 */
/** 图集整幅（永远 kShadowMapSize，不随级数变）。图集 = 列数×列数块，每块边长 = 整幅/列数。
 *
 * ## 为何总预算固定、而不跟级数走
 * 总纹素预算固定 = **加级联的代价是每块变细，不是白占画布**。
 * - 1 级：1 块 = 整幅（2048²）——分辨率最高；
 * - 4 级：4 块 = 1024² 每块，远级变粗但覆盖范围变大（这正是 CSM 的本意）。
 * 另外本场景开销**顶点主导**（实测：2.8M 三角形的光梲化），图尺寸对时间影响极小，
 * 所以额外分辨率几乎免费——这也是为何无需在级联间做分辨率取舍。
 *
 * @param count 生效级数
 * @return 列数（1 → 1，2 → 2，3/4 → 2） */
export inline uint32_t shadowAtlasColumns(uint32_t count) {
	return count <= 1u ? 1u : 2u;
}

/** 单块边长 = 图集整幅 / 列数。与 shadowTile 的 tileSize 参同源，采样端从 scene data 拿它算块内 uv → 图集 uv。
 * @param cols 图集列数（shadowAtlasColumns）
 * @return 单块边长（像素） */
export inline uint32_t shadowTileSize(uint32_t cols) {
	return kShadowMapSize / cols;
}

/** 级联分块在图集上的子矩形（视口 + renderArea）。与采样端的块内 uv → 图集 uv 叠映（shadow.slang
 *  readShadowAtlas）同源；分块排布 = 列号 c%cols / 行号 c/cols。
 *  @param c 分块号
 *  @param tileSize 单块边长
 *  @param cols 图集列数（shadowAtlasColumns）
 *  @return {x, y, w, h}（与 RgPassDesc::viewport 同布局） */
export std::array<uint32_t, 4> shadowTile(uint32_t c, uint32_t tileSize, uint32_t cols) {
	return {tileSize * (c % cols), tileSize * (c / cols), tileSize, tileSize};
}
/**
 * 阴影 pass 装饰（Phase 11 批 11.2 基建，2026-09-29）：追加 `shadow_map`
 * （kind=depthOffscreen，格式/尺寸 = kShadowMapFormat/kShadowMapSize）+ 一个 **depth-only**
 * drawMesh pass（state="shadow"、drawSource="shadow"），插在**图首**——阴影深度不依赖相机
 * 深度，放最前使 barrier 推导最短，且保证它先于任何读阴影图的 pass。
 *
 * **当前状态（诚实声明）**：`drawSource="shadow"` 段目前是**空段**（FrameDrawList 尚无阴影
 * 段，见 vkrenderer segmentRange），即该 pass 只建附件 / 清深度 / 零 draw → **画面逐位不变**。
 * 本函数的交付物是"渲染图能表达 depth-only pass"这条基建（新 kind + 深度词表 + eDepth
 * 屏障 + 独立 extent + 状态词表，全链 BIT-IDENTICAL 验证），深度-only 管线与阴影采样
 * 归批 11.2 剩余部分。
 *
 * 调用点 = main_impl::compileGraphOrThrow，`diagnostics.shadow_pass` 门控
 * （默认关 = 不调用 = 图逐位同，沿用 D10 纪律）。
 * @param desc 图声明（原地修改：追加 attachment + 图首插 pass）
 * @param mapSize 阴影图边长（调用方传入 scene_data::kShadowMapSize —— 图模块不认识
 *        scene data 契约，故不做隐式依赖；1 级 cascade = 整幅，将来 4 级走 2×2 tile
 *        时本值是图集整幅边长、每 tile = 本值/2）
 * @return true = 已装饰（任何图形态均支持：不读既有拓扑，只前插）
 */
export bool decorateShadowPass(RgGraphDesc& desc, uint32_t mapSize);

/**
 * **TAA 图装饰（批 12.6）**：往图里插入时域历史链的三件套，返回 false = 图形态不支持。
 *
 * 产物形状（角色反查，forward / deferred 两变体通用，纪律同本模块 picking / highlight）：
 *  ① 两个附件：`taa_out`（offscreen rgba16_sfloat，本帧 TAA 结果）+
 *     `taa_hist`（同上但 **history=true**，跨帧历史）；
 *  ② `taa` pass（kind=drawLight、state="taa"）插在**最终上屏消费者之前**——即"写 swapchain
 *     且 sampleRead 色附件"的那个 pass（两条路线都是 tonemap）。读 scene_hdr + depth +
 *     `taa_hist`，写 `taa_out`；
 *  ③ 把该消费者的色输入从 `scene_hdr` **改写为 `taa_out`**（tonemap 因此吃时域滤波结果）；
 *  ④ 帧末追加 `taa_commit`（kind=blit）：`taa_out` → `taa_hist` 整幅拷贝，
 *     使下一帧的 `taa_hist` = 本帧结果。
 *
 * 为何用"两图 + 帧末 copy"而不是单图原地：同 pass 读写同图 = feedback loop（编译期拒 +
 * Vulkan 非法）；静态图（启动期编译一次）也不能逐帧换附件角色。copy 代价 = 一次全屏
 * RGBA16F blit。双图奇偶交替可省掉它，但要执行器支持按帧跳 pass（侵入大）→ 记债
 * `docs/taa_plan.md` §7 第 4 条。
 *
 * 与 picking / highlight 装饰的次序契约：本装饰**必须最后调**（在 highlight 之后），
 * 否则 highlight 插进 taa 与 tonemap 之间会让高亮吃不到时域滤波。
 * @param desc 图声明（原地修改）
 * @return true = 已装饰；false = 图里没有"写 swapchain 且采色"的 pass（DIAG offscreen 变体）
 */
export bool decorateTaaPass(RgGraphDesc& desc);

/**
 * 批 14.5 第三批：插「AO 施加」专用全屏 pass（附件契约见 `kAoAppliedName`）。
 *
 * 做的事 = 三件套的简化版：一个附件（`ao_applied`）+ 一个全屏 pass（state="ao_apply"，
 * 读成片与 ao 写 ao_applied）+ 把「写 swapchain 且采色」那个 pass（tonemap）的色输入
 * 换成 `ao_applied`。**没有**帧末 copy（不跨帧）。
 *
 * **跳过条件（两条，缺一即不插，返回 false）**：
 *  1. 图里已有 `taa` pass —— AO 施加点就在它里面（批 13.6），再插本 pass = **双施**。
 *     Phase 13 已冻结，taa.frag 一字不动，所以解耦靠"谁在图里谁负责"而不是改 taa。
 *  2. 图里没有 `ao` 附件（无 SSAO 的图形态）—— 没有可见度可乘。
 * ⟹ 门关（TAA 关 + FSR 关）时本装饰**不插**，AO 仍由 tonemap 施加 = **图逐位不变**（红线）。
 *
 * **次序契约**：必须在 picking / highlight / taa 三个装饰**之后**调。本 pass 是真正的
 * 成片消费者（sampleRead 色附件），先插会让后续装饰的锚点搜索认错消费点（同 velocity
 * 装饰器那条纪律，只是 velocity 靠"不采色"天然免疫，本 pass 采色、免疫不了）。
 *
 * @param desc 图声明（原地修改）
 * @return true = 已装饰；false = 按上面两条跳过（图不变）
 */
export bool decorateAoApplyPass(RgGraphDesc& desc);

/**
 * Phase 15 批 15.1 前半：插「SSR（屏幕空间反射）」专用全屏 pass。**批 15.3 子步 B 起 =
 * 双 pass**（附件契约见 `kSsrTraceName` / `kSsrAppliedName`，拆分论证 → ssr_trace.frag 文件头）。
 *
 * 做的事 = 两附件 + 两全屏 pass + 一次消费者改写：
 *  - `ssr_trace` pass（state="ssr_trace"，半分辨率 extentScale）：读 light 的成片与
 *    G-buffer/深度，march 后写 premultiplied 反射贡献；
 *  - `ssr` pass（apply，state="ssr" **沿用原名**——消费者链/日志/smoke 判据零改动）：读
 *    scene_hdr + ssr_trace + gbuffer_metalrough，粗糙度感知模糊后加法写 ssr_applied；
 *  - 把「写 swapchain 且采色」那个 pass（tonemap）的色输入换成 `ssr_applied`。
 * 插入次序 = light → **trace → apply** → [taa | ao_applied] → tonemap（两 pass 一起插在
 * 消费者之前，trace 紧邻 apply 之前）。没有帧末 copy（不跨帧）。
 *
 * **跳过条件（四条，任一命中即不插、返回 false，图不变）**：
 *  1. 图里已有 `taa` 或 `ao_apply` pass —— **装饰太晚**（调用序不变量的结构性护栏：症状
 *     从不可观测的「SSR 建在时域滤波之后」变成可观测的「SSR 消失」）。与 ao_apply 的
 *     「taa 在图里就跳过」**语义不同**：那条是互斥（施加点唯一），本条是时序（串联）。
 *  2. 找不到「写 swapchain 且 sampleRead 色附件」的消费者（offscreen DIAG 变体用 blit
 *     上屏，transferRead ≠ sampleRead）——与 taa / velocity / ao_apply 同一锚点判据。
 *  3. 图里没有 **kGBufferAttachments 契约表的全部通道**（缺一即跳过）。**这就是 SSR 的
 *     deferred-only 边界**：D1 对等豁免已拍板（forward 不跟，`docs/phase15_plan.md` §0.1）；
 *     查全表而非只查 position = trace pass 的 inputs 引用四个 G-buffer 通道，半张契约表会
 *     装饰出引用不存在附件的 pass（编译期即错）。
 *  4. 插入点之前没有可 store 的 depth 写者（trace pass 以 sampleRead 读深度图）。
 * ⟹ 门关（`runtime.ssr.enabled = false`，默认）= 根本不调本函数 = 图里没有这两个 pass，
 *    **结构性**逐位不变（不是"有但旁路"——运行时分支挡不住 slang 的浮点重排，教训原文在
 *    `shaders/light.frag.slang` 的 IZB_CONSUME 注释）。
 *
 * **次序契约**：必须在 velocity / taa / ao_apply 三个装饰**之前**调（apply 读 light 的
 * 产物 scene_hdr，且这三个装饰器都动态读「消费者当前的色附件」⟹ SSR 先插它们自动读到
 * ssr_applied，链自己接成 scene_hdr → ssr_applied → [taa_out | ao_applied] → tonemap）。
 * 与 ao_apply 的关键差别：ao_apply 的「taa 在图里就跳过」是**互斥**（AO 施加点只能有一个），
 * 本装饰的跳过条件 1 是**时序**——SSR 与 TAA 是串联（TAA 读到含 SSR 的成片 = 正确，时域
 * 滤波该滤最终画面），那条只是把"调用序挪错"变成可观测失败，不改变正确顺序下的行为。
 *
 * @param desc 图声明（原地修改）
 * @return true = 已装饰；false = 按上面四条跳过（图不变）
 */
export bool decorateSsrPass(RgGraphDesc& desc);

/**
 * Phase 14 批 14.4 速度场装饰（契约见本函数实现段头）：加 `velocity` 附件 + 一个
 * 全屏 `state="velocity"` pass（读 depth，写速度场），插在「写 swapchain 且采色」的
 * pass（两条路线都是 tonemap）**之前**。
 *
 * 与 `decorateTaaPass` 的次序契约：本装饰**先于** TAA 调（TAA 保持「必须最后」的既有
 * 纪律）。落点顺序 = `[velocity, taa, tonemap]`——两者都只 sampleRead depth，彼此无依赖，
 * 而 TAA 的锚点搜索是**现搜**的（不缓存下标），故 velocity 先插不会让 TAA 认错消费者。
 * @param desc 图声明（原地修改）
 * @return true = 已装饰；false = 图形态不支持（无 swapchain 消费者 / 借不到可 store 的 depth）
 */
export bool decorateVelocityPass(RgGraphDesc& desc);

/** G-buffer 附件契约条目 + 契约表（名字 + 格式串）——已移入叶子模块 gbuffer_contract。 */

/**
 * 编译期自检（无 GPU）：构造合法/非法图样本跑 compileRenderGraph，返回全部
 * 失败描述（空 = 通过）。覆盖：默认/deferred/blit 拓扑、引用不存在、读先于写、
 * 同 pass feedback、DontCare 后使用、缺段名、blit 拓扑错误、swapchain 永不写、
 * 格式词表成员/映射 + 默认图结构断言。main 启动期 debug 日志通道消费。
 * @return 失败描述列表（空 = 全部通过）
 */
export std::vector<std::string> renderGraphSelfCheck();

/**
 * Phase 7 D1（架构债务续账步骤 2 自 Resources 搬入本模块——kGBufferAttachments 契约表
 * 的装配函数留在契约表本家）：确保 deferred G-buffer 5 个 offscreen RT 并返回其
 * SAMPLED_IMAGE 堆槽（[0]albedo [1]normal [2]position [3]metalrough [4]emissive）。
 * 名字/格式 = kGBufferAttachments 单一真相源（与 deferredRenderGraphDesc 共享）；
 * drawFrame 图循环对同名同格式 ensure 命中复用、堆槽跨 resize 复用，light 管线 baked
 * 有效。extent = 当前 swapchain。**调用序契约**：仍在装配后期、Renderer 前调用；RT
 * 采样槽走运行时段，不再插入几何/纹理契约段。
 * @param rts RT 缓存（ensure/heapSlot）
 * @param swapchain 交换链层（extent）
 * @return 5 个 heap 槽（kGBufferAttachments 序）；Error = RT 创建 / 槽分配失败
 */
export std::expected<std::array<uint32_t, 5>, Error> ensureDeferredGBufferSlots(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * Phase 8 批次 ①（架构债务续账步骤 2 自 Resources 搬入本模块）：确保 HDR 累加链
 * scene_hdr RT（名字/格式 = kSceneHdrName/kSceneHdrFormat 单一真相源，rgba16_sfloat
 * offscreen）并返回其 SAMPLED_IMAGE 堆槽（addTonemapEntry 烘焙 binding 4；tonemap pass
 * 采样）。调用时序同 ensureDeferredGBufferSlots——几何/纹理/env（deferred 还有
 * G-buffer）槽之后、Renderer 之前；drawFrame 图循环对同名同格式命中复用、堆槽跨
 * resize 不变。
 * @param rts RT 缓存（ensure/heapSlot）
 * @param swapchain 交换链层（extent）
 * @return scene_hdr 的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
 */
export std::expected<uint32_t, Error> ensureSceneHdrSlot(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * Phase 11 批 11.4：确保 SSAO 遮蔽量 RT（名字/格式 = kAoName/kAoFormat 单一真相源，
 * r8_unorm offscreen）并返回其 SAMPLED_IMAGE 堆槽（addTonemapEntry 烘焙 binding 6）。
 * 调用时序同 ensureSceneHdrSlot（其后），且**必须早于** addTonemapEntry。
 * @param rts RT 缓存（ensure/heapSlot）
 * @param swapchain 交换链层（extent）
 * @return ao RT 的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
 */
export std::expected<uint32_t, Error> ensureAoSlot(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * 批 12.6 TAA：确保 taa_out / taa_hist 两块 RT（格式 = kTaaFormat 契约常量单一真相源，
 * rgba16_sfloat offscreen）并各领 SAMPLED_IMAGE 堆槽。
 * 调用时序同 ensureSceneHdrSlot，且**必须早于** addTaaEntry——槽号一经烘进管线 binding
 * 即终身有效，resize 换图只重写描述符内容（RenderTargetCache::ensure）。
 * 仅 TAA 开启时调用（门默认关 = 不领槽 = 堆槽布局与基线零差异）。
 * @param rts RT 缓存（ensure/heapSlot）
 * @param swapchain 交换链层（extent）
 * @return {taa_out 槽, taa_hist 槽}（addTaaEntry 的 binding 4/5）；Error = 建图失败
 */
export std::expected<std::array<uint32_t, 2>, Error> ensureTaaSlots(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * 批 14.5 第三批：确保 AO 施加专用 pass 的 RT（名字/格式 = `kAoAppliedName` /
 * `kAoAppliedFormat` 单一真相源，rgba16_sfloat offscreen，**渲染分辨率**——它是时域滤波器的
 * 输入，必须与 scene_hdr / taa_out 同尺寸）并返回其 SAMPLED_IMAGE 堆槽。
 *
 * 调用时序同 `ensureTaaSlots`，且**必须早于** `addAoApplyEntry`。仅
 * `CompiledGraphResult::aoApplyDecorated` 为真时调用（门关 = 不领槽 = 堆槽布局与基线零差异）。
 * @return ao_applied RT 的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
 */
export std::expected<uint32_t, Error> ensureAoAppliedSlot(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * Phase 15 批 15.1 前半：确保 SSR apply pass 的 RT（名字/格式 = `kSsrAppliedName` /
 * `kSsrAppliedFormat` 单一真相源，rgba16_sfloat offscreen，**渲染分辨率**——它的消费者
 * （tonemap / taa / ao_apply / FSR）都在渲染网格上工作）并返回其 SAMPLED_IMAGE 堆槽。
 *
 * 调用时序同 `ensureAoAppliedSlot`，且**必须早于** `addSsrApplyEntry`。仅
 * `CompiledGraphResult::ssrDecorated` 为真时调用（门关 = 不领槽 = 堆槽布局与基线零差异）。
 * @return ssr_applied RT 的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
 */
export std::expected<uint32_t, Error> ensureSsrAppliedSlot(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * Phase 15 批 15.3 子步 B：确保 SSR trace pass 的 RT（名字/格式/缩放 = `kSsrTraceName` /
 * `kSsrTraceFormat` / `kSsrTraceScale` 单一真相源，rgba16_sfloat offscreen，**半渲染分辨率**
 * ——march 本体跑在半分辨率网格上，成本随像素数走）并返回其 SAMPLED_IMAGE 堆槽。
 *
 * extent 计算与执行器逐帧装配共用 `scaleRtExtent`（两处各写一份 floor = 每帧重建 RT）。
 * 调用时序同 `ensureSsrAppliedSlot`，且**必须早于** `addSsrTraceEntry`（槽号烘进 binding）。
 * 仅 `CompiledGraphResult::ssrDecorated` 为真时调用（门关 = 不领槽 = 零差异）。
 * @return ssr_trace RT 的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
 */
export std::expected<uint32_t, Error> ensureSsrTraceSlot(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * Phase 14 批 14.4：确保逐像素速度场 RT（名字/格式 = kVelocityName/kVelocityFormat
 * 单一真相源，rgba16_sfloat offscreen）并返回其 SAMPLED_IMAGE 堆槽。
 *
 * 调用时序同 ensureTaaSlots，且**必须早于** addVelocityEntry——堆槽号一经烘进管线 binding
 * 即终身有效（`RenderTargetCache` 跨 resize 复用槽号）。
 * 仅速度场开启时调用（门默认关 = 不领槽 = 堆槽布局与基线零差异）。
 * @param rts RT 缓存（ensure/heapSlot）
 * @param swapchain 交换链层（extent = **渲染**分辨率，批 14.3 起 ≠ 后缓冲）
 * @return velocity RT 的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
 */
export std::expected<uint32_t, Error> ensureVelocitySlot(
	RenderTargetCache& rts, Swapchain& swapchain);

/**
 * Phase 11 批 11.2：阴影图 RT（d32_sfloat depthOffscreen，aspect = eDepth）并返回其
 * SAMPLED_IMAGE 堆槽。extent = 2·mapSize（候逇 4 级 2×2 图集，切到 4 级时不需重建描述绑定）。
 *
 * **为何启动期必须建（而不是由 drawFrame 的 ensureOffscreenAttachments 顺手建）**：光照管线条目
 * 在启动期就把这个堆槽烘进 binding mapping，而堆槽号是 drawFrame 里首次 ensure 时才分配的——
 * 推荐顺序是先建实现。这与 ensureAoSlot / ensureSceneHdrSlot 同轴（启动期先领槽，每帧
 * ensure 命中缓存复用）。
 *
 * 消费方 = 全部光照管线条目（forward 模型条目 + deferred light 条目）的 binding 12/13，
 * 与 shadow.slang 的声明成对。
 * @param rts RT 缓存（ensure/heapSlot）
 * @param mapSize 单块边长（图集整幅 = 2·mapSize）
 * @return 阴影图的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
 */
export std::expected<uint32_t, Error> ensureShadowMapSlot(RenderTargetCache& rts, uint32_t mapSize);

}  // namespace RGL
