module;
#include <nlohmann/json.hpp>  // 协议 JSON（第三方头，不受 GMF STL 白名单约束）

export module debug_rpc_capabilities;

import rlstd;
import rlvk;                 // vk::Extent2D（渲染网格入参；纯值，不碰设备）
import render_graph;         // CompiledRenderGraph / RgGraphDesc / kindName / resolvedRtExtent
import dynamic_render_graph; // DynamicRenderGraph（脏位读数 + desc 声明侧）

namespace RGL {

/**
 * `/state` 的 `capabilities` 段拼装（`docs/能力观测_plan.md` 批 1；出口形状 §3.2）。
 *
 * ## 这一段的定位：能力事实的唯一结构化出口
 *
 * 「引擎当前具备哪些渲染能力」此前只有两处可观测形态：编译产物本体（进程内）与启动日志
 * 文案（外部只能抓这个，`tools/smoke.py` 的十几条正则）。本函数把前者变成一个 JSON 段，
 * 批 2 的消费者迁移（smoke.py / cpu_baseline.py 换读这里）以这一段为准。
 *
 * ## 🔴 内容一律从编译产物现算，本函数没有第二个内容来源
 *
 * 附件 / pass 的名单与字段只读 `compiled`（`CompiledRenderGraph`）。`graph` 参数**只**取
 * 一个布尔 `dirty`（声明侧有变更尚未落图），**不取任何名单**——`Renderer::graphStatus()`
 * 的 `presentPasses` 读 `desc().passes`（声明），那是既有 graph 段的口径，本段不跟
 * （计划 §1.1 点名的三处平行记录，这里只认编译产物这一处）。
 *
 * 为什么实测上两者今天逐条相等（2026-10-19 实机四档：出厂 / ssr / taa / fsr，pass 数与
 * 名单全等——`compileRenderGraph` 对 pass 是 1:1 保序的，结构上必然），**不代表来源正确**：
 * 声明与产物之间有一个「脏窗口」（`applyPassToggles` 改 desc 之后、`recompileGraphIfNeeded`
 * 换图之前；重编译失败时脏位保留、分歧跨帧持续，见 `reportRecompileFailure` 的重试路径）。
 * 能力段回答的是「这一帧实际在画什么」= 编译产物；「声明想要什么但还没落图」由 `dirty`
 * 一个布尔说清。判据 = `tests/test_rpc_capabilities.cppm` 的脏窗口用例（变异「passes 改从
 * desc 取」让它红，计划 §6 判据 ①）。
 *
 * ## rtBuilt：遍历图的附件、逐个问缓存（方向约束 §3.3）
 *
 * 「这张 RT 建没建」经 `rtBuilt` 回调注入（生产调用点 = `RenderTargetCache::isBuilt`），
 * 本函数**不碰设备**——名单永远来自 `compiled.attachments`，缓存只是被问的那一方；
 * 反过来枚举缓存会把「缓存里有什么」当成能力真相（孤儿条目 / 还没 ensure 的附件，两个
 * 方向都会错）。回调对**非图属 RT 档**（swapchain / depth / buffer）不被调用——它们
 * 不占 RT 缓存，问它们没有意义（`ensureOffscreenAttachments` 的穷尽 switch 是这一档
 * 归属的权威）。
 *
 * ## 尺寸口径
 *
 * 图属 RT 的 width/height = 执行器同一公式（`resolvedRtExtent`：独立 extent 优先，
 * 跟随档 = 渲染网格 × extentScale）；swapchain / depth = 渲染网格；buffer 档无图像
 * 尺寸恒 0（字节数在 buffer 注册表，不在这一段）。
 *
 * @param graph    图声明面（只读 `isDirty()`；名单不从它取，见上）
 * @param compiled 编译产物（能力段内容的唯一来源；执行器当前持有的那一份）
 * @param rtBuilt  按名问「这张 RT 建没建」（生产 = RenderTargetCache::isBuilt；单测注入
 *                 录制器核对「逐个问、只问图属 RT」）
 * @param renderExtent 渲染网格（跟随档尺寸的基准，与 ensureOffscreenAttachments 同口径）
 * @param frame    本快照属于哪一帧（运行期图可被 /pass 热插拔，没有帧号无法复现）
 * @return capabilities 段的 JSON 对象（键集 = `rgl/rpc_schema.lua` 的 `stateCapabilities`）
 */
export nlohmann::json rpcCapabilitiesJson(const DynamicRenderGraph& graph,
	const CompiledRenderGraph& compiled,
	const std::function<bool(std::string_view)>& rtBuilt,
	vk::Extent2D renderExtent, uint32_t frame) {
	nlohmann::json attachments = nlohmann::json::array();
	for (const auto& att : compiled.attachments) {
		nlohmann::json entry;
		entry["name"] = att.name;
		entry["kind"] = std::string { kindName(att.kind) };
		entry["format"] = att.format;  // 声明拼写原样（"inherit" / 具名），解析后的 vk::Format 是另一件事
		// 穷尽 switch（不写 default）：新增第六档 kind 必须在这里回答
		// 「占不占 RT 缓存、尺寸怎么算」——漏答编译不过，而不是静默少一个字段。
		switch (att.kind) {
			case RgAttachmentKind::swapchain:
			case RgAttachmentKind::depth:
				entry["width"] = renderExtent.width;
				entry["height"] = renderExtent.height;
				break;
			case RgAttachmentKind::offscreen:
			case RgAttachmentKind::depthOffscreen: {
				const auto rt = resolvedRtExtent(att.extent, att.extentScale, renderExtent);
				entry["width"] = rt.width;
				entry["height"] = rt.height;
				entry["rtBuilt"] = rtBuilt(att.name);
				break;
			}
			case RgAttachmentKind::buffer:
				entry["width"] = 0u;
				entry["height"] = 0u;
				break;
		}
		attachments.push_back(std::move(entry));
	}
	nlohmann::json passes = nlohmann::json::array();
	for (uint32_t order = 0; order < compiled.passes.size(); ++order) {
		const auto& pass = compiled.passes[order];
		nlohmann::json entry;
		entry["name"] = pass.name;
		entry["kind"] = std::string { kindName(pass.kind) };
		entry["state"] = pass.state;  // blit 档恒空串（编译产物透传声明，blit 无动态状态）
		entry["order"] = order;       // 执行序 = 编译产物下标（骨架版编译器不重排）
		passes.push_back(std::move(entry));
	}
	return nlohmann::json {
		{"frame", frame},
		{"dirty", graph.isDirty()},
		{"attachments", std::move(attachments)},
		{"passes", std::move(passes)},
	};
}

}  // namespace RGL
