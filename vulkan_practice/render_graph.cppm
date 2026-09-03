export module render_graph;

import rlstd;
import rlvk;
import vkerror;

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

export enum class RgAttachmentKind : uint8_t {
	swapchain,  // 后缓冲（image/view 由执行器录制期按 imageIndex 解析）
	depth,      // 深度图（Resources 持有）
	offscreen,  // 图属 transient RT（执行器按名确保分配；帧末布局跨帧追踪）
};

// attachment 在 pass 中的角色。写角色进 outputs，读角色进 inputs。
export enum class RgUsage : uint8_t {
	colorWrite,    // 渲染颜色附件（dynamic rendering color attachment）
	depthWrite,    // 渲染深度附件（每 pass 至多一个）
	sampleRead,    // 图形阶段 shader 采样读
	transferRead,  // blit/copy 源读
	transferWrite, // blit/copy 目标写
};

export enum class RgLoadOp : uint8_t { clear, load };
export enum class RgStoreOp : uint8_t { store, dontCare };

// 清除值：kind=depth 用 depth，其余用 color（执行器按 attachment kind 择一）
export struct RgClearValue {
	std::array<float, 4> color{0.1f, 0.1f, 0.1f, 1.0f};
	float depth = 1.0f;
};

export struct RgAttachmentDesc {
	std::string name;
	RgAttachmentKind kind = RgAttachmentKind::swapchain;
	std::string format = "inherit";  // "inherit" = 随 swapchain/depth 现有格式；offscreen 具体格式留批次 4
};

export struct RgPassAttachment {
	std::string attachment;  // 引用 RgAttachmentDesc.name
	RgUsage usage = RgUsage::colorWrite;
	RgLoadOp loadOp = RgLoadOp::clear;   // 仅写角色有意义；读角色忽略
	RgStoreOp storeOp = RgStoreOp::store;
	RgClearValue clear{};
};

export enum class RgPassKind : uint8_t {
	drawMesh,  // 执行 FrameDrawList 段（drawSource → 段边界映射由执行器完成）
	blit,      // transferRead input → transferWrite output 整幅拷贝
};

export struct RgPassDesc {
	std::string name;
	RgPassKind kind = RgPassKind::drawMesh;
	std::string state;       // RenderState 名（"opaque"/"skybox"/"transparent"…名字字符串，零 handle）
	                         // 执行器解析；drawMesh 必填，blit 忽略
	std::string drawSource;  // FrameDrawList 段名（"opaque"/"skybox"/"transparent"）；drawMesh 必填
	std::vector<RgPassAttachment> outputs;
	std::vector<RgPassAttachment> inputs;
};

// 渲染意图的完整声明：attachments 下标 = 编译产物中的 slot；passes 线性序 = 执行序
// （骨架版不做拓扑排序/死 pass 消除，脚本序即 GPU 序）
export struct RgGraphDesc {
	std::vector<RgAttachmentDesc> attachments;
	std::vector<RgPassDesc> passes;
};

export using RgAttachmentSlot = uint32_t;
export constexpr RgAttachmentSlot kRgNoAttachment = 0xFFFFFFFFu;

export struct RgCompiledAttachment {
	std::string name;
	RgAttachmentKind kind = RgAttachmentKind::swapchain;
	std::string format = "inherit";
	vk::ImageAspectFlags aspect = {};
	// 帧首写是否走"内容丢弃"规则（swapchain/depth = true；offscreen = false 需追踪布局）
	bool frameStartDiscard = false;
};

// 录制期一条 attachment 布局转换（执行器物化为 ImageMemoryBarrier2，
// image/view/oldLayout 由执行器按 slot 解析——编译器只产语义计划）
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

// 一个渲染附件在 beginRendering 中的条目（layout/loadOp/storeOp/clear 已定）
export struct RgAttachmentPlan {
	RgAttachmentSlot slot = kRgNoAttachment;
	vk::ImageLayout layout = vk::ImageLayout::eUndefined;
	vk::AttachmentLoadOp loadOp = vk::AttachmentLoadOp::eClear;
	vk::AttachmentStoreOp storeOp = vk::AttachmentStoreOp::eStore;
	vk::ClearValue clearValue{};
};

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

export struct CompiledRenderGraph {
	std::vector<RgCompiledAttachment> attachments;
	std::vector<RgPassPlan> passes;
	// 帧末 swapchain → PresentSrcKHR（无 swapchain 写入的图 = nullopt，
	// 执行器跳过 ⑫；present 语义本身仍归执行器）
	std::optional<RgBarrierPlan> presentTransition;
	// submit 的 imageAvailable wait stage = swapchain 附件在命令流中首次实际使用
	// 的 dstStage（编译期派生；现单段直渲 = ColorAttachmentOutput、RGL_OFFSCREEN
	// blit 图 = Transfer，与旧手工分支逐位一致）
	vk::PipelineStageFlags2 swapchainWaitStage = vk::PipelineStageFlagBits2::eTopOfPipe;
	std::vector<std::string> warnings;
};

// 物化：语义计划 + 执行器解析出的 image/oldLayout → ImageMemoryBarrier2
//（oldLayout 由执行器按 plan.oldLayoutDiscard 给 eUndefined 或跨帧/帧内追踪值）
export vk::ImageMemoryBarrier2 materializeBarrier(const RgBarrierPlan& plan, VkImage image,
	vk::ImageLayout oldLayout);

// 校验 + 布局链推导 + barrier 计划生成。失败返回带定位信息的 Error
//（名字引用、同 pass 读写冲突、读先于写、后续使用的写丢 store 等，见 impl）。
export [[nodiscard]] std::expected<CompiledRenderGraph, Error> compileRenderGraph(const RgGraphDesc& desc);

// 缺省图 = 现 drawFrame 三段语义的图表达（swapchain+depth；
// opaque/skybox/transparent 各一 pass，中间 attachment store 保内容、
// 末段 depth DontCare）——批次 2 的 diff=0 验收基准。
export const CompiledRenderGraph& defaultRenderGraph();
export const RgGraphDesc& defaultRenderGraphDesc();

// 编译期自检（无 GPU）：构造合法/非法图样本跑 compileRenderGraph，返回全部
// 失败描述（空 = 通过）。main 启动期 debug 日志通道消费。
export std::vector<std::string> renderGraphSelfCheck();

}  // namespace RGL
