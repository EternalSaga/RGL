module;
#include <fmt/format.h>
module render_graph:builtin;

import render_graph;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;

namespace RGL {

// 内建图描述 + 编译期自检（2026-09-11 Phase 8 批次 ①自 render_graph_impl 拆出：
// impl 文件超 500 行阈值，内置三图描述属"数据"、compileRenderGraph 属"算法"，
// 分单元各管一头）。本单元 = default/deferred/offscreen 三图声明 + defaultRenderGraph
// 单例 + renderGraphSelfCheck；算法与 usage 推导留 render_graph_impl。

namespace {

/** 附件引用速构（outputs/inputs 共用；clear 仅写角色有意义）。 */
RgPassAttachment writeAtt(std::string name, RgUsage usage, RgLoadOp load, RgStoreOp store,
	RgClearValue clear = {}) {
	return RgPassAttachment{.attachment = std::move(name), .usage = usage, .loadOp = load,
		.storeOp = store, .clear = clear};
}

/** tonemap pass 速构（Phase 8 ①：forward/deferred 共用拓扑尾段——采 scene_hdr 写 swapchain）。 */
RgPassDesc tonemapPass() {
	return RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
		.outputs = {writeAtt("swapchain", RgUsage::colorWrite, RgLoadOp::clear, RgStoreOp::store)},
		.inputs = {writeAtt(std::string{kSceneHdrName}, RgUsage::sampleRead, RgLoadOp::load,
			RgStoreOp::store)}};
}

}  // namespace

/**
 * defaultRenderGraphDesc 定义（接口注释见 render_graph.cppm）。Phase 8 ①：三段渲入
 * scene_hdr（rgba16_sfloat HDR 累加）+ tonemap pass 上屏——原直写 swapchain 的
 * diff=0 基准随 ACES 换 baseline（docs/phase8_plan.md 批次 ①）。与单 render pass
 * 实例的等价性靠"中间 scope store 保内容"实现：depth 仅末段 DontCare（中途 store
 * 是多 pass 拆分的必要代价，像素结果不受影响）。
 */
const RgGraphDesc& defaultRenderGraphDesc() {
	static const RgGraphDesc desc = [] {
		auto hdrOut = [](RgLoadOp load, RgStoreOp store) {
			return writeAtt(std::string{kSceneHdrName}, RgUsage::colorWrite, load, store);
		};
		auto depthOut = [](RgLoadOp load, RgStoreOp store) {
			return writeAtt("depth", RgUsage::depthWrite, load, store);
		};
		return RgGraphDesc{
			.attachments =
				{
					RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
					RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
					RgAttachmentDesc{.name = std::string{kSceneHdrName},
						.kind = RgAttachmentKind::offscreen,
						.format = std::string{kSceneHdrFormat}},
				},
			.passes =
				{
					RgPassDesc{.name = "opaque", .state = "opaque", .drawSource = "opaque",
						.outputs = {hdrOut(RgLoadOp::clear, RgStoreOp::store),
							depthOut(RgLoadOp::clear, RgStoreOp::store)}},
					RgPassDesc{.name = "skybox", .state = "skybox", .drawSource = "skybox",
						.outputs = {hdrOut(RgLoadOp::load, RgStoreOp::store),
							depthOut(RgLoadOp::load, RgStoreOp::store)}},
					RgPassDesc{.name = "transparent", .state = "transparent",
						.drawSource = "transparent",
						.outputs = {hdrOut(RgLoadOp::load, RgStoreOp::store),
							depthOut(RgLoadOp::load, RgStoreOp::dontCare)}},
					tonemapPass(),
				},
		};
	}();
	return desc;
}

/** defaultRenderGraph 定义：进程内静态单例，首次调用时编译默认图（编译失败抛 runtime_error——默认图是硬契约，失败 = 工程自身 bug）。 */
const CompiledRenderGraph& defaultRenderGraph() {
	static const CompiledRenderGraph graph = [] {
		auto compiled = compileRenderGraph(defaultRenderGraphDesc());
		if (!compiled)
			throw std::runtime_error(
				fmt::format("defaultRenderGraph: {}", compiled.error().describe()));
		return *compiled;
	}();
	return graph;
}

/** offscreenRenderGraphDesc 定义（接口注释见 render_graph.cppm）。Phase 8 决策：DIAG 变体保持 SDR 不接 tonemap。 */
const RgGraphDesc& offscreenRenderGraphDesc() {
	// offscreen 图变体（批次 4，旧 DIAG 转正）：三段渲 scene_rt + composite blit。
	// scene_rt format 留 "inherit"——执行器解析为 swapchain 格式（blit LINEAR 兼容前提）；
	// 清除值与默认图一致（0.1 灰背景 / depth 1.0）
	static const RgGraphDesc desc = [] {
		auto scenePass = [](std::string name, const char* source, RgLoadOp colorLoad,
		                    RgLoadOp depthLoad, RgStoreOp depthStore) {
			return RgPassDesc{.name = std::move(name), .state = source, .drawSource = source,
				.outputs = {writeAtt("scene_rt", RgUsage::colorWrite, colorLoad, RgStoreOp::store),
					writeAtt("depth", RgUsage::depthWrite, depthLoad, depthStore)}};
		};
		return RgGraphDesc{
			.attachments =
				{
					RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
					RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
					RgAttachmentDesc{.name = "scene_rt", .kind = RgAttachmentKind::offscreen},
				},
			.passes =
				{
					scenePass("opaque", "opaque", RgLoadOp::clear, RgLoadOp::clear, RgStoreOp::store),
					scenePass("skybox", "skybox", RgLoadOp::load, RgLoadOp::load, RgStoreOp::store),
					scenePass("transparent", "transparent", RgLoadOp::load, RgLoadOp::load,
						RgStoreOp::dontCare),
					RgPassDesc{.name = "composite", .kind = RgPassKind::blit,
						.outputs = {writeAtt("swapchain", RgUsage::transferWrite, RgLoadOp::load,
							RgStoreOp::store)},
						.inputs = {writeAtt("scene_rt", RgUsage::transferRead, RgLoadOp::load,
							RgStoreOp::store)}},
				},
		};
	}();
	return desc;
}

/** deferredRenderGraphDesc 定义（接口注释见 render_graph.cppm）。Phase 8 ①：light 输出改 scene_hdr + tonemap 上屏。 */
const RgGraphDesc& deferredRenderGraphDesc() {
	// Phase 7 D1：3-pass deferred 图（Phase 8 ① +tonemap）。geometry（drawMesh/opaque 段）
	// 渲入 4 G-buffer RT（显式格式）+ depth；light（drawLight）全屏采样 4 RT 渲入 scene_hdr；
	// tonemap（drawLight）采 scene_hdr ACES 上屏 swapchain。storeOp：G-buffer 全 store
	//（light 读），帧首 clear；scene_hdr/swapchain 全屏覆盖 clear。present 由 presentTransition 派生。
	static const RgGraphDesc desc = [] {
		RgGraphDesc d{
			.attachments =
				{
					RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain},
					RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
					RgAttachmentDesc{.name = std::string{kSceneHdrName},
						.kind = RgAttachmentKind::offscreen,
						.format = std::string{kSceneHdrFormat}},
				},
		};
		RgPassDesc geometry{.name = "geometry", .state = "opaque", .drawSource = "opaque"};
		RgPassDesc light{.name = "light", .kind = RgPassKind::drawLight, .state = "light"};
		// 4 G-buffer 附件（名字/格式 = kGBufferAttachments 单一真相源，geometry 写序 =
		// light 读序 = light.frag binding 4-7）。GB 清黑（0）——默认 0.1 灰会让空像素
		// albedo=0.1 → deferred 背景发灰（帧内回读截图暴露的真实输出 bug，旧重渲路径
		// 自清 0 掩盖了它）
		for (const auto& gb : kGBufferAttachments) {
			d.attachments.push_back(RgAttachmentDesc{
				.name = std::string{gb.name}, .kind = RgAttachmentKind::offscreen,
				.format = std::string{gb.format}});
			geometry.outputs.push_back(writeAtt(std::string{gb.name}, RgUsage::colorWrite,
				RgLoadOp::clear, RgStoreOp::store,
				RgClearValue{{0.0f, 0.0f, 0.0f, 0.0f}, 1.0f}));
			light.inputs.push_back(writeAtt(std::string{gb.name}, RgUsage::sampleRead,
				RgLoadOp::load, RgStoreOp::store));
		}
		geometry.outputs.push_back(writeAtt("depth", RgUsage::depthWrite, RgLoadOp::clear,
			RgStoreOp::store));
		// Phase 8 ①：light 不再直写 swapchain——渲入 scene_hdr（tonemap 上屏）
		light.outputs.push_back(writeAtt(std::string{kSceneHdrName}, RgUsage::colorWrite,
			RgLoadOp::clear, RgStoreOp::store));
		d.passes = {std::move(geometry), std::move(light), tonemapPass()};
		return d;
	}();
	return desc;
}

namespace {

/** selfCheck 前半：合法/非法图拓扑样本跑 compileRenderGraph（expectOk/expectErr 成对收集）。 */
void selfCheckTopology(std::vector<std::string>& failures) {
	auto expectOk = [&](const char* what, const RgGraphDesc& desc) {
		if (auto r = compileRenderGraph(desc); !r)
			failures.push_back(fmt::format("{}: unexpected failure: {}", what, r.error().describe()));
	};
	auto expectErr = [&](const char* what, const RgGraphDesc& desc) {
		if (auto r = compileRenderGraph(desc); r)
			failures.push_back(fmt::format("{}: expected failure, compiled OK", what));
	};
	auto colorOut = [](std::string name) {
		return RgPassAttachment{.attachment = std::move(name), .usage = RgUsage::colorWrite};
	};
	auto scAtt = [] { return RgAttachmentDesc{.name = "swapchain"}; };

	expectOk("default", defaultRenderGraphDesc());

	// Phase 7 D1 + Phase 8 ①：deferred 图（geometry 4 G-buffer + light 采 GB 渲 scene_hdr
	// + tonemap 采 scene_hdr 上屏）应编译通过
	expectOk("deferred 3-pass", deferredRenderGraphDesc());

	// blit 图：offscreen RT 渲三段 + blit 上屏（批次 4 的转正拓扑）
	expectOk("offscreen blit", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth},
			RgAttachmentDesc{.name = "scene_rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {
			RgPassDesc{.name = "scene", .state = "opaque", .drawSource = "opaque",
				.outputs = {colorOut("scene_rt")}},
			RgPassDesc{.name = "composite", .kind = RgPassKind::blit,
				.outputs = {RgPassAttachment{.attachment = "swapchain",
					.usage = RgUsage::transferWrite, .storeOp = RgStoreOp::store}},
				.inputs = {RgPassAttachment{.attachment = "scene_rt",
					.usage = RgUsage::transferRead}}},
		},
	});

	// 非法样本：引用不存在 / 读先于写 / 同 pass feedback / DontCare 后还有使用 /
	// draw pass 缺段名 / blit 拓扑错误 / swapchain 永不写
	expectErr("unknown attachment", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("nope")}}},
	});
	expectErr("read before write", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {RgPassDesc{.name = "p", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("swapchain")},
			.inputs = {RgPassAttachment{.attachment = "rt", .usage = RgUsage::sampleRead}}}},
	});
	expectErr("same-pass feedback", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("swapchain")},
			.inputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::sampleRead}}}},
	});
	expectErr("dontCare then use", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {
			RgPassDesc{.name = "a", .state = "opaque", .drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = "rt", .usage = RgUsage::colorWrite,
					.storeOp = RgStoreOp::dontCare}}},
			RgPassDesc{.name = "b", .state = "opaque", .drawSource = "opaque",
				.outputs = {colorOut("swapchain")},
				.inputs = {RgPassAttachment{.attachment = "rt", .usage = RgUsage::sampleRead}}},
		},
	});
	expectErr("draw pass without drawSource", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .state = "opaque",
			.outputs = {colorOut("swapchain")}}},
	});
	expectErr("blit wrong topology", RgGraphDesc{
		.attachments = {scAtt()},
		.passes = {RgPassDesc{.name = "p", .kind = RgPassKind::blit,
			.outputs = {colorOut("swapchain")}}},
	});
	expectErr("swapchain never written", RgGraphDesc{
		.attachments = {scAtt(),
			RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen}},
		.passes = {RgPassDesc{.name = "a", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("rt")}}},
	});
	// 显式格式（Phase 7 G-buffer 前提）：offscreen 词表具名格式编译通过 / 未知格式拒绝
	expectOk("offscreen explicit format", RgGraphDesc{
		.attachments = {RgAttachmentDesc{.name = "gbuffer", .kind = RgAttachmentKind::offscreen,
			.format = "rgba16_sfloat"}},
		.passes = {RgPassDesc{.name = "g", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("gbuffer")}}},
	});
	expectErr("offscreen unknown format", RgGraphDesc{
		.attachments = {RgAttachmentDesc{.name = "gbuffer", .kind = RgAttachmentKind::offscreen,
			.format = "totally_bogus_format"}},
		.passes = {RgPassDesc{.name = "g", .state = "opaque", .drawSource = "opaque",
			.outputs = {colorOut("gbuffer")}}},
	});
}

/** selfCheck：默认图首 pass（opaque）帧首 barrier 与 color load 断言（scene_hdr tracked + depth discard 两条；首 color = Clear）。 */
void selfCheckDefaultFirstPass(const RgPassPlan& first, std::vector<std::string>& failures) {
	if (first.barriers.size() != 2 || first.barriers[0].oldLayoutDiscard ||
		first.barriers[0].aspect != vk::ImageAspectFlagBits::eColor ||
		!first.barriers[1].oldLayoutDiscard ||
		first.barriers[1].aspect != vk::ImageAspectFlagBits::eDepth)
		failures.push_back(
			"default: first pass should carry scene_hdr tracked + depth discard barriers");
	if (!first.colors.empty() && first.colors[0].loadOp != vk::AttachmentLoadOp::eClear)
		failures.push_back("default: first color load should be Clear");
}

/** selfCheck：默认图中段（skybox）identity barrier 与 color load 断言（color/depth 各一条追踪 barrier；mid color = Load）。 */
void selfCheckDefaultMidPass(const RgPassPlan& mid, std::vector<std::string>& failures) {
	if (mid.barriers.size() != 2 || mid.barriers[0].oldLayoutDiscard ||
		mid.barriers[1].oldLayoutDiscard)
		failures.push_back("default: mid pass should carry 2 tracked identity barriers");
	if (!mid.colors.empty() && mid.colors[0].loadOp != vk::AttachmentLoadOp::eLoad)
		failures.push_back("default: mid color load should be Load");
}

/** selfCheck：默认图末段（tonemap）kind/state 断言。 */
void selfCheckDefaultLastPass(const RgPassPlan& last, std::vector<std::string>& failures) {
	if (last.kind != RgPassKind::drawLight || last.state != "tonemap")
		failures.push_back("default: last pass should be drawLight/tonemap");
}

/** selfCheck：格式词表断言——inherit→fallback、具名→映射值（表拼错 = Phase 7 静默错图，编译校验查不出，回读值守卫）；membership 判定与 resolve 一致。 */
void selfCheckFormats(std::vector<std::string>& failures) {
	if (resolveRgFormat("inherit", vk::Format::eB8G8R8A8Srgb) != vk::Format::eB8G8R8A8Srgb)
		failures.push_back("format: 'inherit' should resolve to fallback");
	if (resolveRgFormat("rgba16_sfloat", vk::Format::eB8G8R8A8Unorm) !=
		vk::Format::eR16G16B16A16Sfloat)
		failures.push_back("format: 'rgba16_sfloat' mapping broken");
	if (resolveRgFormat("r32g32b32_sfloat", vk::Format::eB8G8R8A8Unorm) !=
		vk::Format::eR32G32B32Sfloat)
		failures.push_back("format: 'r32g32b32_sfloat' mapping broken");
	if (!isKnownRgFormat("inherit") || !isKnownRgFormat("rg16_snorm") ||
		isKnownRgFormat("totally_bogus_format"))
		failures.push_back("format: isKnownRgFormat membership broken");
}

/** selfCheck 后半：默认图结构断言（barrier 形态/present 转换）+ 格式词表断言（各段拆 selfCheckDefault* / selfCheckFormats，本函数只做编排、保 push 顺序）。 */
void selfCheckStructure(std::vector<std::string>& failures) {
	// 结构断言（Phase 8 ①后默认图 = 4 pass）：首 pass（opaque）两条帧首转换——
	// scene_hdr（offscreen = 追踪布局非 discard，声明序在前）+ depth（discard）；
	// skybox 段对 color/depth 各一条 identity barrier（跨 render pass 实例 LOAD
	// 执行依赖，old==new 合法）；present 转换来自 tonemap 段的 swapchain
	// colorWrite（CAO→PresentSrc）
	const auto& g = defaultRenderGraph();
	if (g.passes.size() != 4)
		failures.push_back("default: expected 4 passes");
	if (g.passes.size() == 4) {
		selfCheckDefaultFirstPass(g.passes[0], failures);
		selfCheckDefaultMidPass(g.passes[1], failures);
		selfCheckDefaultLastPass(g.passes[3], failures);
	}
	if (!g.presentTransition ||
		g.presentTransition->newLayout != vk::ImageLayout::ePresentSrcKHR)
		failures.push_back("default: missing present transition");
	// 格式词表断言：inherit→fallback、具名→映射值（表拼错 = Phase 7 静默错图，
	// 编译校验查不出，回读值守卫）；membership 判定与 resolve 一致
	selfCheckFormats(failures);
}

}  // namespace

/** renderGraphSelfCheck 定义（接口注释见 render_graph.cppm）：拓扑样本 + 结构/词表断言两半（自 render_graph_impl 拆债），全部失败项收集后返回。 */
std::vector<std::string> renderGraphSelfCheck() {
	std::vector<std::string> failures;
	selfCheckTopology(failures);
	selfCheckStructure(failures);
	return failures;
}

}  // namespace RGL
