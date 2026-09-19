export module render_graph;

import rlstd;
import rlvk;
import vkerror;
import vk_swapchain;  // Swapchain&（rtslot 装配分区取 extent，架构债务续账步骤 2）
import render_target_cache;  // RenderTargetCache&（G-buffer/scene_hdr 装配，架构债务续账步骤 2）

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
//   depthWrite    → DepthStencilAttachmentOptimal / 读+写 / EarlyFragmentTests
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
 * - offscreen：图属 transient RT（执行器按名确保分配；帧末布局跨帧追踪）。
 */
export enum class RgAttachmentKind : uint8_t {
	swapchain,  // 后缓冲（image/view 由执行器录制期按 imageIndex 解析）
	depth,      // 深度图（Resources 持有）
	offscreen,  // 图属 transient RT（执行器按名确保分配；帧末布局跨帧追踪）
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
 * kind 决定资源来源；format 仅 offscreen 可取词表具名格式，swapchain/depth 必须 "inherit"
 *（格式由执行器固定，compile 校验）。
 */
export struct RgAttachmentDesc {
	std::string name;
	RgAttachmentKind kind = RgAttachmentKind::swapchain;
	std::string format = "inherit";  // "inherit" = 随 swapchain/depth 现有格式；
	                                 // offscreen 可用具名格式（词表见 isKnownRgFormat）
};

/**
 * offscreen attachment 格式词表成员判定 = compile 校验入口（"inherit" 与全部具名为 true）。
 * 词表（字符串 → vk::Format，小写下划线命名，表驱动、扩展 = impl 加一行）：
 * "inherit" = 随 swapchain 格式（执行器解析 fallback）；具名覆盖 Phase 7 G-buffer 类
 * 显式 RT 需求（rgba8_srgb/rgba8_unorm/b8g8r8a8_srgb/rgb8_srgb/rg16_snorm/rg16_unorm/
 * rgba16_sfloat/rgb16_sfloat/r32g32b32_sfloat/rgba32_sfloat/r32_sfloat/rg32_sfloat）。
 * @param name 格式名（小写下划线）
 * @return true = 词表内（含 "inherit"）
 */
export bool isKnownRgFormat(const std::string& name);
/**
 * 执行器物化 vk::Format："inherit"→inheritFallback，具名→词表映射值。
 * @param name 格式名（调用方保证已 isKnownRgFormat）
 * @param inheritFallback "inherit" 的替代格式（通常 = swapchain 格式）
 * @return 物化格式；漏网（未校验）返回 eUndefined
 */
export vk::Format resolveRgFormat(const std::string& name, vk::Format inheritFallback);

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
 *   **state 名**绑定预建管线条目（词表 = "light"/"tonemap"，见 kFullscreenPassStates），
 *   不走 FrameDrawList 段；outputs 恰一 colorWrite、inputs = sampleRead（barrier 自动）。
 */
export enum class RgPassKind : uint8_t {
	drawMesh,  // 执行 FrameDrawList 段（drawSource → 段边界映射由执行器完成）
	blit,      // transferRead input → transferWrite output 整幅拷贝
	// 固定全屏单 group draw（Phase 8 ①起 state 名泛化，不再限 "light"）
	drawLight,
};

/**
 * drawLight kind 的 state 词表（单一真相源）：state 名同时是 ShaderRegistry 固定条目名
 *（"light" = deferred 逐灯 GGX，"tonemap" = HDR→LDR ACES）与 StateKit 状态名——
 * 执行器构造期按 state 名解析管线条目，图内 drawLight pass 的 state 必须在此表内
 *（validateRenderGraphForExecutor 校验）。
 */
export inline constexpr std::array<std::string_view, 2> kFullscreenPassStates{"light", "tonemap"};

/**
 * Phase 8 批次 ①：HDR 累加链附件契约（单一真相源）——forward/deferred 图的颜色目标
 * scene_hdr（rgba16_sfloat offscreen RT），tonemap pass 采样后写 swapchain。
 * 消费方 = render_graph_builtin 三图描述 + Resources::ensureSceneHdrSlot（启动期建
 * RT + 采样堆槽）+ addTonemapEntry（烘焙 binding 4）。
 */
export inline constexpr std::string_view kSceneHdrName = "scene_hdr";
export inline constexpr std::string_view kSceneHdrFormat = "rgba16_sfloat";

/**
 * pass 声明（RgGraphDesc.passes 元素）：name 必填；kind 定执行语义；
 * state = RenderState 名（"opaque"/"skybox"/"transparent"…名字字符串，零 handle，
 * 执行器解析；drawMesh 必填，blit 忽略）；drawSource = FrameDrawList 段名
 *（"opaque"/"skybox"/"transparent"；drawMesh 必填）。outputs = 写角色、inputs = 读角色。
 */
export struct RgPassDesc {
	std::string name;
	RgPassKind kind = RgPassKind::drawMesh;
	std::string state;       // RenderState 名（"opaque"/"skybox"/"transparent"…名字字符串，零 handle）
	                         // 执行器解析；drawMesh 必填，blit 忽略
	std::string drawSource;  // FrameDrawList 段名（"opaque"/"skybox"/"transparent"）；drawMesh 必填
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
 * 推导（depth → eDepth，其余 → eColor）；frameStartDiscard 定帧首写是否走
 * "内容丢弃"规则（swapchain/depth = true；offscreen = false 需跨帧追踪布局）。
 */
export struct RgCompiledAttachment {
	std::string name;
	RgAttachmentKind kind = RgAttachmentKind::swapchain;
	std::string format = "inherit";
	vk::ImageAspectFlags aspect = {};
	// 帧首写是否走"内容丢弃"规则（swapchain/depth = true；offscreen = false 需追踪布局）
	bool frameStartDiscard = false;
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

/** G-buffer 附件契约条目（名字 + 格式串）。 */
export struct RgGBufferDesc {
	std::string_view name;
	std::string_view format;
};
/**
 * Phase 7 D1：G-buffer 附件契约表（**单一真相源**）——消费方 =
 * deferredRenderGraphDesc（geometry outputs / light inputs 按本表顺序生成）、
 * vkresources ensureDeferredGBufferSlots、drawFrame 帧内捕获（dump_gbuf）。
 * 格式串 = isKnownRgFormat 词表可解析名（resolveRgFormat 物化）。
 * 顺序即 light.frag binding 4-7 + 尾部 11 = emissive（albedo/normal/position/metalrough/emissive——Phase 8
 * 剩余批次 B 加第 5 通道 emissive，RGBA16F 保 HDR 域）与 diagnostics.lua dump_gbuf 通道号。
 */
export inline constexpr std::array<RgGBufferDesc, 5> kGBufferAttachments{{
	{"gbuffer_albedo", "rgba8_srgb"},
	{"gbuffer_normal", "rg16_snorm"},
	{"gbuffer_position", "rgba32_sfloat"},
	{"gbuffer_metalrough", "rgba8_unorm"},
	{"gbuffer_emissive", "rgba16_sfloat"},
}};

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

}  // namespace RGL
