module render_graph:decorate;

import render_graph;
import scene_context;  // shadowPassName / shadowDrawSource（阴影 pass 名与段名的单一来源）
import scene_data;     // kActiveShadowCascades / kShadowMapSize  // clang 不给 partition 实现单元隐式 import 主接口——必须显式
import rlstd;

// 诊断装饰 pass（2026-09-29 自 render_graph_impl 拆出，解该文件 500 代码行红线）：
// picking / highlight 两个 pass 的"图装饰"——它们不参与正常渲染，只在
// `diagnostics.picking` 门开时由 main 装配层调用，往图里追加附件/pass。
// 契约（为何插在图中间、为何要改前一个 pass 的 storeOp）见 render_graph.cppm 接口注释。
//
// 纪律：装饰只认"角色"不认名字——depth 附件名从写它的 pass 反查、场景色附件名从采样它
// 的 pass 反查，因此 forward / deferred / offscreen 三个变体（附件名 scene_hdr /
// scene_rt 各不相同）都自动适用，无需为变体写分支。

namespace RGL {

namespace {

/** 按名查附件是否算"色附件"（非 depth；名不在表内 = 未知，按色处理）。 */
bool isColorAttachment(const RgGraphDesc& desc, const std::string& name) {
	const auto it = std::ranges::find_if(desc.attachments,
		[&name](const RgAttachmentDesc& a) { return a.name == name; });
	return it == desc.attachments.end() || it->kind != RgAttachmentKind::depth;
}

/** pass 是否写 depth（含 depthWrite 角色的输出）。 */
bool writesDepth(const RgPassDesc& pass) {
	return std::ranges::any_of(pass.outputs, [](const RgPassAttachment& o) {
		return o.usage == RgUsage::depthWrite;
	});
}

/**
 * pass 的 sampleRead 输入里**首个色附件**的名字（无则 nullopt）。
 * 必须排除 depth：ao pass（Phase 11 批 11.4）也以 sampleRead 读 depth，若只看 usage
 * 会把它误当成"消费成片"的点 → 高亮的 colorAtt 与 depthAtt 撞名 → 编译期报
 * "references attachment 'depth' more than once"。
 */
std::optional<std::string> sampledColorName(const RgGraphDesc& desc, const RgPassDesc& pass) {
	for (const auto& in : pass.inputs)
		if (in.usage == RgUsage::sampleRead && isColorAttachment(desc, in.attachment))
			return in.attachment;
	return std::nullopt;
}

/** 首个"采样场景色附件"的 pass 下标（高亮 pass 的插入点；无 = nullopt）。 */
std::optional<size_t> firstColorConsumerIndex(const RgGraphDesc& desc) {
	for (size_t i = 0; i < desc.passes.size(); ++i)
		if (sampledColorName(desc, desc.passes[i]))
			return i;
	return std::nullopt;
}

/**
 * 插入点之前**最后一个写 depth 的 pass** 的 depth 附件名（借它做可见性测试），并就地把它
 * 的 storeOp 修正为 store——否则 forward 图的 transparent 段按"depth 后续无人用"的优化
 * 声明 dontCare，高亮读到的是未定义内容。
 * @return depth 附件名；插入点之前无人写 depth 时 nullopt（高亮会糊成一片，判不支持）
 */
std::optional<std::string> borrowDepthFromPriorWriter(RgGraphDesc& desc, size_t insertIdx) {
	for (size_t i = insertIdx; i-- > 0;) {
		for (auto& out : desc.passes[i].outputs) {
			if (out.usage != RgUsage::depthWrite)
				continue;
			out.storeOp = RgStoreOp::store;  // 装饰的一部分；门关 = 不调用 = 图逐位同
			return out.attachment;
		}
	}
	return std::nullopt;
}

}  // namespace

/**
 * decoratePickingPass 定义（契约见 render_graph.cppm 接口注释）。depth 附件名从锚点
 * pass 的 depthWrite 输出取（不硬编码 "depth"，deferred/offscreen 变体名同约定但零
 * 假设）；object_id clear 全零（VkClearValue union：float 0.0 与 uint32 0 同表示，
 * r32ui 下 float 默认值 {0.1,…} 会 reinterpret 成垃圾 id，必须显式零）。
 */
bool decoratePickingPass(RgGraphDesc& desc) {
	// 锚点 = 首个带 depth 输出的 opaque drawMesh pass（scene depth 已写 = picking 测深得可见层）
	// 下标式而非迭代器：本分区 GMF 为空，std::next 不在 rlstd 导出面内（不为它扩面）。
	size_t anchorIdx = desc.passes.size();
	for (size_t i = 0; i < desc.passes.size(); ++i)
		if (desc.passes[i].kind == RgPassKind::drawMesh && desc.passes[i].drawSource == "opaque"
			&& writesDepth(desc.passes[i])) {
			anchorIdx = i;
			break;
		}
	if (anchorIdx == desc.passes.size())
		return false;  // 图形态不支持（object-id pass 无 depth 可测）
	std::string depthAtt;
	for (const auto& out : desc.passes[anchorIdx].outputs)
		if (out.usage == RgUsage::depthWrite) {
			depthAtt = out.attachment;
			break;
		}
	desc.attachments.push_back(RgAttachmentDesc{
		.name = "object_id", .kind = RgAttachmentKind::offscreen, .format = "r32_uint"});
	desc.passes.insert(desc.passes.begin() + static_cast<long>(anchorIdx) + 1, RgPassDesc{
		.name = "picking", .state = "picking", .drawSource = "opaque",
		.outputs = {
			RgPassAttachment{.attachment = "object_id", .usage = RgUsage::colorWrite,
				.loadOp = RgLoadOp::clear, .storeOp = RgStoreOp::store,
				.clear = RgClearValue{{0.0f, 0.0f, 0.0f, 0.0f}, 1.0f}},
			RgPassAttachment{.attachment = depthAtt, .usage = RgUsage::depthWrite,
				.loadOp = RgLoadOp::load, .storeOp = RgStoreOp::store}}});
	return true;
}

/**
 * decorateHighlightPass 定义（契约见 render_graph.cppm 接口注释）。插入点与 depth 来源
 * 均按角色反查（见本文件头部纪律）；颜色写既有场景色附件（load 保留成片，高亮 alpha
 * 混合叠加），插图末 = 落在 tonemap 之后、盖不到上屏内容，故必须插在消费 pass 之前。
 */
bool decorateHighlightPass(RgGraphDesc& desc) {
	auto consumeIdx = firstColorConsumerIndex(desc);
	if (!consumeIdx)
		return false;  // 图形态不支持（无「采样色附件」的消费 pass = 无成片可叠加）
	auto colorAtt = sampledColorName(desc, desc.passes[*consumeIdx]);  // 上一步已保证有值
	auto depthAtt = borrowDepthFromPriorWriter(desc, *consumeIdx);
	if (!depthAtt)
		return false;
	// 写场景色（load 保留成片，alpha 混合叠加）+ depth（load；实际是否真写由 state_kit
	// highlightState 关掉，此处 depthWrite 是「渲染深度附件」的声明位）
	desc.passes.insert(desc.passes.begin() + static_cast<long>(*consumeIdx),
		RgPassDesc{.name = "highlight", .state = "highlight", .drawSource = "opaque",
			.outputs = {
				RgPassAttachment{.attachment = *colorAtt, .usage = RgUsage::colorWrite,
					.loadOp = RgLoadOp::load, .storeOp = RgStoreOp::store},
				RgPassAttachment{.attachment = *depthAtt, .usage = RgUsage::depthWrite,
					.loadOp = RgLoadOp::load, .storeOp = RgStoreOp::store}}});
	return true;
}

/**
 * decorateShadowPass 定义（契约见 render_graph.cppm 接口注释，含"当前段内零 draw、
 * 画面逐位不变"的诚实声明）。与 picking / highlight 两个装饰的关键差别：**不需要**
 * 按角色反查——阴影 pass 自为一体（自带专属深度附件），既不读既有拓扑也不借既有附件，
 * 故任何图形态（forward / deferred / offscreen）都直接适用，前插即可。
 */
bool decorateShadowPass(RgGraphDesc& desc, uint32_t mapSize) {
	const uint32_t cols = shadowAtlasColumns(kActiveShadowCascades);
	const uint32_t tile = shadowTileSize(cols);
	desc.attachments.push_back(RgAttachmentDesc{
		.name = std::string{kShadowMapName},
		.kind = RgAttachmentKind::depthOffscreen,
		.format = std::string{kShadowMapFormat},
		// 图集整幅 = 列数×mapSize（列数跟**生效级数**走，见 shadowAtlasColumns）。独立分辨率，与窗口解耦。
		// 为何不固定 2×2：图集整幅决定每块的实际分辨率，固定 2×2 意味着只用 1 级时
		// 白占 3/4 画布、阴影分辨率从 2048² 降到 1024²。切到更多级时整幅变 → 新图/新槽在启动期重建
		// （ensureShadowMapSlot 已在烘管线条目之前用同一公式算出同一尺寸 → 命中缓存，不重建）。
		.extent = {mapSize, mapSize},
	});
	// 图首插入 N 个 pass（一级一个），各渲染图集里自己那一块子矩形。插图首（不是图末）：
	// ① 阴影深度不依赖相机深度，放最前 barrier 链最短；② 天然先于所有要读它的 pass。
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		// 每级都 loadOp = clear：**动态渲染的 clear 只作用于本 pass 的 renderArea**
		// （= 自己的分块子矩形，见 RgPassDesc::viewport 注），分块互不重叠 → 清不到别级。
		// 若误按"clear 会洗掉整幅"只清级 0（2026-09-29 4 级首测即此坑），块 1..N 永不初始化
		// → 采到未定义深度（实测 ≈0 → 全判影，画面整幅加深 60% 像素）。清值 1.0 = 无遮挡。
		desc.passes.insert(desc.passes.begin() + static_cast<long>(c), RgPassDesc{
			.name = std::string{shadowPassName()} + (c == 0 ? "" : std::to_string(c)),
			.kind = RgPassKind::drawMesh,
			.state = "shadow",
			.drawSource = shadowDrawSource(c),
			.viewport = shadowTile(c, tile, cols),
			.outputs = {RgPassAttachment{.attachment = std::string{kShadowMapName},
				.usage = RgUsage::depthWrite, .loadOp = RgLoadOp::clear,
				.storeOp = RgStoreOp::store}},
		});
	}
	// 消费方声明：把阴影图作为 sampleRead 输入接到**光照 pass**。不接的话编译器不会为它生成
	// DSAO → ShaderReadOnly 的布局转换，采样端就会报 VVL。按 state 而不是按 pass 名：forward 图的光照在
	// opaque/transparent 段，deferred 图的光照在 light pass（state="light"）——三个 state 名在两种图里都是语义角色。
	static constexpr std::array<std::string_view, 3> kShadowLitStates{"opaque", "transparent", "light"};
	for (auto& pass : desc.passes)
		if (std::ranges::any_of(kShadowLitStates, [&](std::string_view s) { return pass.state == s; }))
			pass.inputs.push_back(RgPassAttachment{.attachment = std::string{kShadowMapName},
				.usage = RgUsage::sampleRead, .loadOp = RgLoadOp::load, .storeOp = RgStoreOp::store});
	return true;
}

}  // namespace RGL
