export module render_graph:usage;

import render_graph;  // clang 不给 partition 单元隐式 import 主接口——必须显式
import rlstd;
import rlvk;

// usage 推导表 + 四个 kind 谓词（2026-10-15 批次 B2-3 自 render_graph_impl 拆出，解该文件
// 500 代码行红线；先例 = :formats / :decorate / :rtslot 三次同款拆分）。
//
// 为什么这一批**一起**搬而不是各搬各的：它们是同一条链的源头——`usageInfo` 决定 barrier 的
// 布局/stage/access，四个谓词决定 aspect / 是否图属 RT / 走不走深度面 / 帧首丢不丢布局。
// 全仓按 kind 或 usage 分派的地方最终都落到这七个函数上，把它们放在一个文件里，"加一档要
// 回答哪几个问题"就是一次 grep 能问完的事。
//
// 为什么是**接口分区**（`export module`）而不是实现分区：这七个函数只被本模块的兄弟分区
// （:impl）消费，主接口 `render_graph` 里没有它们的声明 ⟹ 外部模块 import 不到它们，
// 公开面一个名字都没多（形态先例 = `ecs:frame_diag`）。写成实现分区就得把声明抬进主接口，
// 那是为了搬文件而扩大 API。
//
// 🔴 四个 kind 谓词必须保持**穷尽 switch、无 default**，`usageInfo` / `isWriteUsage` /
// `isReadUsage` 同理。理由逐字写在下面的注释块里，一句话版：`-Wswitch` 只对「switch 缺
// case」出声，写了 default 就永远不缺；兜底 = 新档静默按 image 走完全链（§7.10 点名的形态）。

namespace RGL {

/**
 * usage 推导三元组：目标布局 + 管线 stage + access mask。
 *
 * 🔴 `layout` 是 **optional**，而 nullopt 有明确语义 = 「这个 usage 作用在**没有布局**的资源上」,
 * 也就是 buffer 档（`storageRead` / `storageWrite`）。这不是为了少写一个分支，而是把
 * **plan-only 红线做成类型**：`planBarrier` 与 `planBeginRenderingEntry` 拿到 nullopt 必须早退,
 * ⟹ "buffer 附件出现在 image barrier 计划里"这件事在代码路径上写不出来（判据 =
 * `tests/test_attachment_buffer.cppm`「buffer 档不进 barrier 计划」那条）。
 * 反过来的写法（layout 给个 `eUndefined` 兜底）会把这条红线降级成"记得写注释"——而 eUndefined
 * 作 newLayout 是**合法**的 Vulkan 值，VVL 不报，静默程度与 §7.10 那条 aspect 回落同级。
 */
struct UsageInfo {
	std::optional<vk::ImageLayout> layout;
	vk::PipelineStageFlags2 stage;
	vk::AccessFlags2 access;
};

/**
 * usage → {layout, stage, access} 推导表（接口注释速览的权威实现，单一真相源）：
 * colorWrite→CAO/写/ColorAttachmentOutput；depthWrite→DSAO/读+写/Early+LateFragmentTests；
 * sampleRead→SRO/ShaderRead/AllGraphics；transferRead→TsrcO/TransferRead/Transfer；
 * transferWrite→TdstO/TransferWrite/Transfer。
 *
 * 🔴 depthWrite 的 stage 必须是 **Early | Late 两段并集**（syncval 实测：只给 Early 时
 * VVL 报 `WRITE_AFTER_WRITE hazard ... layout transition ... previously written at the end
 * of the render pass instance by the attachment storeOp`）。依据 spec
 * `synchronization-pipeline-stages`：EARLY_FRAGMENT_TESTS 覆盖早期深度测试与该格式附件的
 * **load**（clear）操作，LATE_FRAGMENT_TESTS 覆盖晚期深度测试与该格式附件的 **store** 操作
 * ——本表同时当 barrier 的 src（上次 usage）与 dst（本次 usage）用，作 src 时漏掉 Late
 * 就等于"深度写入不需要等"，作 dst 时漏掉 Late 就等于"本 pass 不在晚期写深度"。
 * 两段并集是 spec 允许的最小覆盖（不是兜底 AllGraphics；IZB 图外 barrier 用 AllGraphics
 * 是另一条路径，见 izb_shadow_impl）。
 * @param u 附件角色
 * @return 推导三元组（枚举穷尽，末条 return 不可达）
 */
export UsageInfo usageInfo(RgUsage u) {
	using Stage = vk::PipelineStageFlagBits2;
	using Acc = vk::AccessFlagBits2;
	switch (u) {
		case RgUsage::colorWrite:
			return {vk::ImageLayout::eColorAttachmentOptimal, Stage::eColorAttachmentOutput,
				Acc::eColorAttachmentWrite};
		case RgUsage::depthWrite:
			return {vk::ImageLayout::eDepthStencilAttachmentOptimal,
				Stage::eEarlyFragmentTests | Stage::eLateFragmentTests,
				Acc::eDepthStencilAttachmentRead | Acc::eDepthStencilAttachmentWrite};
		case RgUsage::sampleRead:
			return {vk::ImageLayout::eShaderReadOnlyOptimal, Stage::eAllGraphics, Acc::eShaderRead};
		case RgUsage::transferRead:
			return {vk::ImageLayout::eTransferSrcOptimal, Stage::eTransfer, Acc::eTransferRead};
		case RgUsage::transferWrite:
			return {vk::ImageLayout::eTransferDstOptimal, Stage::eTransfer, Acc::eTransferWrite};
		// storage 两档：**layout 留 nullopt**（buffer 不在任何布局状态机里），stage/access 只
		// 表达"谁在读写 storage"这一语义角色，供校验侧与日志归因用。
		// 🔴 这两个 stage/access **刻意不物化成任何命令**：buffer 的 barrier 由归属模块经
		// `offgraph_sync` 的边名发（plan-only 红线，`docs/compute_调研.md` §5.2）。图一旦拿它们
		// 去发 barrier，模块就无从返回"我真实发了什么"，A 档 `layoutDrift` 那类对账对这类资源
		// 会变成恒真——那正是 14.4b B 档被否决的形态。dst 侧给 AllGraphics 而不是 ComputeShader：
		// buffer 的消费者在本项目里既有 compute（cull 写）也有 mesh/frag（标志表被 mesh shader
		// 读），而图**不知道**具体是哪个 shader 阶段（那是模块的知识），给宽一档 + 不发命令，
		// 比给一个看起来精确但可能错的档位诚实。
		case RgUsage::storageRead:
			return {std::nullopt, Stage::eAllGraphics, Acc::eShaderRead};
		case RgUsage::storageWrite:
			return {std::nullopt, Stage::eAllGraphics, Acc::eShaderWrite};
	}
	return {std::nullopt, Stage::eTopOfPipe, Acc::eNone};  // 不可达（枚举穷尽）
}

// 写/读角色判定：两档 storage 各自归写/读，**穷尽 switch、不写 default**。
// 🔴 为什么这里不许留 `||` 链：`||` 链对"新增一个 usage"是**静默**的——加一档而不改这里，
// 它既不算写也不算读，于是 `advanceCursors` 不推进 `everWritten`（读前置校验从此看不见这个
// 附件）、`finishPresent` 把"写过 swapchain"判成没写。与四个 kind 谓词同一条理由（见下方
// 注释块）：把失效方向从"静默走错"换成"编译不过"。

/** 写角色判定（colorWrite/depthWrite/transferWrite/storageWrite）。 */
export bool isWriteUsage(RgUsage u) {
	switch (u) {
		case RgUsage::colorWrite: return true;
		case RgUsage::depthWrite: return true;
		case RgUsage::transferWrite: return true;
		case RgUsage::storageWrite: return true;
		case RgUsage::sampleRead: return false;
		case RgUsage::transferRead: return false;
		case RgUsage::storageRead: return false;
	}
	return false;  // 不可达（穷尽），消 -Wreturn-type
}
/** 读角色判定（sampleRead/transferRead/storageRead）。 */
export bool isReadUsage(RgUsage u) {
	switch (u) {
		case RgUsage::sampleRead: return true;
		case RgUsage::transferRead: return true;
		case RgUsage::storageRead: return true;
		case RgUsage::colorWrite: return false;
		case RgUsage::depthWrite: return false;
		case RgUsage::transferWrite: return false;
		case RgUsage::storageWrite: return false;
	}
	return false;  // 不可达（穷尽），消 -Wreturn-type
}

// ── 三个 kind 谓词：全部写成**穷尽 switch、无 default**（正面样板 = vkrenderer_passrec
//    的 imageForSlot）。为什么不许写 default、也不许留三元式：`-Wswitch` 只对「switch 缺
//    case」出声，写了 default 就永远不缺；三元式 `a || b ? X : Y` 更是把「其余一律 Y」这个
//    兜底写进了语义里。本枚举下一批要加第五个值（buffer 档），届时这三个谓词是整条链的
//    源头（aspect 进 barrier / view / 描述符，两个布尔位进校验与 clearValue 选择），
//    兜底 = 新 kind 静默按 image 档走完全链，而 docs/compute_调研.md §7.10 点名的正是这条。
//    穷尽 switch 把失效方向换成「编译不过」：加值的人必须在这里逐个回答它是什么面、
//    是不是图属 RT、走不走深度面——答不出来就说明机制还没想清楚，这正是需要的刹车。

/**
 * 附件 kind → aspect mask（深度两档 eDepth，颜色两档 eColor）。
 * @param kind 附件种类
 * @return 该附件全链（barrier / view / 描述符）使用的 aspect mask
 */
export vk::ImageAspectFlags aspectOf(RgAttachmentKind kind) {
	switch (kind) {
		case RgAttachmentKind::swapchain: return vk::ImageAspectFlagBits::eColor;
		case RgAttachmentKind::depth: return vk::ImageAspectFlagBits::eDepth;
		case RgAttachmentKind::offscreen: return vk::ImageAspectFlagBits::eColor;
		case RgAttachmentKind::depthOffscreen: return vk::ImageAspectFlagBits::eDepth;
		// buffer 档**没有 aspect**（aspect 是 image 子资源的概念）。返回**空 mask** 而不是
		// 挑一个兜底：空 mask 在下游任何一处都是"这不是 image"的显式记号，而 eColor/eDepth
		// 都会把 buffer 伪装成 image 继续往下走。它实际不可达——buffer 档不进 barrier 计划、
		// 不进 beginRendering 条目（`usageInfo` 的 nullopt layout 挡住），所以
		// `RgCompiledAttachment::aspect` 对 buffer 恒空，而那个空值没有任何消费者。
		case RgAttachmentKind::buffer: return {};
	}
	return {};  // 不可达（穷尽），消 -Wreturn-type
}
/**
 * 是否"图属 RT"（执行器按名 ensure 的两类：颜色 offscreen + 深度 depthOffscreen）。
 * 判据 = 「资源由渲染图按名申请」，不是「是不是 image」——buffer 档若走的是另一套
 * 申请路径，这里就该答 false（穷尽 switch 逼着当场回答，见上方注释）。
 * @param kind 附件种类
 * @return true = 执行器按名 ensure 的图属 RT
 */
export bool isRtBackedKind(RgAttachmentKind kind) {
	switch (kind) {
		case RgAttachmentKind::swapchain: return false;  // 后缓冲：imageIndex 录制期解析
		case RgAttachmentKind::depth: return false;  // 主深度图：Resources 持有
		case RgAttachmentKind::offscreen: return true;
		case RgAttachmentKind::depthOffscreen: return true;
		// buffer 档的资源由**归属模块**申请（VmaBuffer），不由 `RenderTargetCache` 按名 ensure；
		// 它的"独立尺寸"也不该走 `RgAttachmentDesc::extent`（那是像素二维尺寸，buffer 只有
		// 字节数，字节数在注册表里）⟹ 填 extent 必须被拒，判 false 正是让它被拒的那一步。
		case RgAttachmentKind::buffer: return false;
	}
	return false;  // 不可达（穷尽），消 -Wreturn-type
}
/**
 * 是否深度类附件（主深度图 / 图属深度 RT）——clearValue 取 depth 分量、aspect 走深度面。
 * @param kind 附件种类
 * @return true = 走深度面（clearValue 用 depth、depthWrite 合法、blit 两端不得用）
 */
export bool isDepthKind(RgAttachmentKind kind) {
	switch (kind) {
		case RgAttachmentKind::swapchain: return false;
		case RgAttachmentKind::depth: return true;
		case RgAttachmentKind::offscreen: return false;
		case RgAttachmentKind::depthOffscreen: return true;
		// buffer 既不是颜色面也不是深度面——它没有面。判 false 的效果：clearValue 不走 depth
		// 分量（buffer 根本没有 clearValue，因为它不进 beginRendering 条目）、depthWrite 被拒、
		// blit 的"不得是深度类"那条 guard 不拦它（拦它的是 usage×kind 匹配那条，更靠前）。
		case RgAttachmentKind::buffer: return false;
	}
	return false;  // 不可达（穷尽），消 -Wreturn-type
}

/**
 * 帧首写是否走"内容丢弃"规则（决定 barrier 的 oldLayout 取 eUndefined 还是取执行器
 * 跨帧追踪值）：swapchain/depth/depthOffscreen 每帧内容按 loadOp 重建（阴影图每帧整幅
 * clear 亦归此档）；offscreen 是持久图，必须追踪真实布局。
 *
 * 🔴 为什么这里是穷尽 switch 而不是 `kind != offscreen`：本函数**就在 barrier 物化链上**
 * （RgCompiledAttachment::frameStartDiscard → planBarrier → RgBarrierPlan::oldLayoutDiscard
 * → SlotResolver::emitBarriers 的 oldLayout 选择）。写成不等式，第五个 kind 会静默落进
 * "丢弃"档 = 每帧首写把 oldLayout 报成 eUndefined，而 eUndefined 的语义是"旧内容随便
 * 丢"——若那一档其实是需要跨帧存活的资源，症状是隔一帧才现的内容丢失，且 VVL 不报
 * （eUndefined 作 oldLayout 合法）。与三个 kind 谓词同一条理由，见上方注释块。
 * @param kind 附件种类
 * @return true = 帧首写可丢弃旧布局
 */
export bool frameStartDiscards(RgAttachmentKind kind) {
	switch (kind) {
		case RgAttachmentKind::swapchain: return true;
		case RgAttachmentKind::depth: return true;
		case RgAttachmentKind::offscreen: return false;  // 持久图：跨帧追踪真实布局
		case RgAttachmentKind::depthOffscreen: return true;
		// buffer 档**没有布局**，"帧首丢弃旧布局"这个概念对它不成立。判 false 而不是 true：
		// 本函数的两个取值在下游是"oldLayout = eUndefined"与"oldLayout = 追踪值"，两者对 buffer
		// 都是**编出来的**语义（它压根不进 barrier 计划）。选 false 是为了让"buffer 有跨帧真实
		// 状态"这句话留在数据里——它的真实对应物是"单份 buffer 跨 frames-in-flight 复用"
		// （meshlet 标志表正是这一档，见 `MeshletCullService` 类注释），与 offscreen 同侧。
		// 判据 = 单测钉住 buffer 的 frameStartDiscard == false，改这一行必红。
		case RgAttachmentKind::buffer: return false;
	}
	return false;  // 不可达（穷尽），消 -Wreturn-type
}

}  // namespace RGL
