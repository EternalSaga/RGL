module;
#include <fmt/format.h>
module render_graph:builtin;

import render_graph;
import scene_data;  // kActiveShadowCascades（阴影切分级数）  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rlvk;
import vkerror;

namespace RGL {

// 内建图描述 + 编译期自检（2026-09-11 Phase 8 批次 ①自 render_graph_impl 拆出：
// impl 文件超 500 行阈值，内置两图描述属"数据"、compileRenderGraph 属"算法"，
// 分单元各管一头）。本单元 = default/deferred 两图声明 + defaultRenderGraph
// 单例 + renderGraphSelfCheck；算法与 usage 推导留 render_graph_impl。

namespace {

/** 附件引用速构（outputs/inputs 共用；clear 仅写角色有意义）。 */
RgPassAttachment writeAtt(std::string name, RgUsage usage, RgLoadOp load, RgStoreOp store,
	RgClearValue clear = {}) {
	return RgPassAttachment{.attachment = std::move(name), .usage = usage, .loadOp = load,
		.storeOp = store, .clear = clear};
}

/** tonemap pass 速构（Phase 8 ①：forward/deferred 共用拓扑尾段——采 scene_hdr + ao 写
 * swapchain）。ao 输入对无 ao pass 的图（自定义图可不装配）无害：本速构只被
 * forward/deferred 使用，二者都有 ao pass。 */
RgPassDesc tonemapPass() {
	return RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
		.outputs = {writeAtt("swapchain", RgUsage::colorWrite, RgLoadOp::clear, RgStoreOp::store)},
		.inputs = {writeAtt(std::string{kSceneHdrName}, RgUsage::sampleRead, RgLoadOp::load,
				RgStoreOp::store),
			writeAtt(std::string{kAoName}, RgUsage::sampleRead, RgLoadOp::load,
				RgStoreOp::store)}};
}

/** ao pass 速构（Phase 11 批 11.4：forward/deferred 共用——采 depth 写 ao RT，置 tonemap 前）。 */
RgPassDesc aoPass() {
	return RgPassDesc{.name = "ao", .kind = RgPassKind::drawLight, .state = "ao",
		.outputs = {writeAtt(std::string{kAoName}, RgUsage::colorWrite, RgLoadOp::clear,
			RgStoreOp::store, RgClearValue{{0.0f, 0.0f, 0.0f, 0.0f}, 1.0f})},
		.inputs = {writeAtt("depth", RgUsage::sampleRead, RgLoadOp::load, RgStoreOp::store)}};
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
					RgAttachmentDesc{.name = std::string{kAoName},
						.kind = RgAttachmentKind::offscreen, .format = std::string{kAoFormat}},
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
							depthOut(RgLoadOp::load, RgStoreOp::store)}},
					aoPass(),
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

/** offscreen 图变体已删（2026-10-18 刀①，`docs/图变体收敛_plan.md` §2：零使用者）。
 *  blit pass 机制与 `RgAttachmentKind::offscreen` 固有能力**都不在本刀射程**——前者仍被
 *  本文件 selfCheck 的合成图与自定义 Lua 图消费，后者 pervasive（scene_hdr / ao / gbuffer_*）。 */

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
					RgAttachmentDesc{.name = std::string{kAoName},
						.kind = RgAttachmentKind::offscreen, .format = std::string{kAoFormat}},
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
		d.passes = {std::move(geometry), std::move(light), aoPass(), tonemapPass()};
		return d;
	}();
	return desc;
}

namespace {

/** selfCheck 用的阴影图边长（**刻意不硬绑生产值**：本组断言验的是"独立尺寸从声明透传到
 *  编译产物"这条通路，填任何非零值都能验出；生产值 = scene_data::kShadowMapSize，由
 *  main_impl 传给 decorateShadowPass）。 */
constexpr uint32_t kSelfCheckShadowSize = 1024u;

/**
 * selfCheck（Phase 11 批 11.2 基建）：**depth-only pass 拓扑**样本（与 selfCheckTopology
 * 分开是因为它连同 5 个样本把后者顶过了 120 代码行红线）。五条各锁一处"编译能过、
 * GPU 上错得隐蔽"的编译器不变式：
 *  ① 正例：depthOffscreen 附件 + 只写深度的 pass（无 colorWrite）——批 11.2 之前这条被
 *     precheckDrawPass 直接拒（B1 硬阻塞），列样本是防后人把"放宽"改回去；
 *  ②③ 两表分立：颜色附件不得拿深度格式、深度附件不得拿颜色格式（合成一张词表就是让
 *     这两条之一失效 → 颜色 view 指向深度图 = invalid usage，只有 VVL 抓得到）；
 *  ④ 独立 extent 只对图属 RT 开放（主深度图尺寸随 swapchain，声明独立尺寸 = 执行器
 *     不会遵守的意图）；
 *  ⑤ 放宽 colorWrite 的对偶校验：一个写附件都没有 = 空 pass，仍要拒。
 */
void selfCheckDepthOnlyTopology(std::vector<std::string>& failures) {
	auto expectOk = [&](const char* what, const RgGraphDesc& desc) {
		if (auto r = compileRenderGraph(desc); !r)
			failures.push_back(fmt::format("{}: unexpected failure: {}", what, r.error().describe()));
	};
	auto expectErr = [&](const char* what, const RgGraphDesc& desc) {
		if (auto r = compileRenderGraph(desc); r)
			failures.push_back(fmt::format("{}: expected failure, compiled OK", what));
	};
	auto shadowOut = [] {
		return RgPassAttachment{.attachment = std::string{kShadowMapName},
			.usage = RgUsage::depthWrite};
	};

	expectOk("depth-only pass", RgGraphDesc{
		.attachments = {RgAttachmentDesc{.name = std::string{kShadowMapName},
			.kind = RgAttachmentKind::depthOffscreen, .format = std::string{kShadowMapFormat},
			.extent = {kSelfCheckShadowSize, kSelfCheckShadowSize}}},
		.passes = {RgPassDesc{.name = "shadow", .state = "shadow", .drawSource = "shadow",
			.outputs = {shadowOut()}}},
	});
	expectErr("offscreen takes depth format", RgGraphDesc{
		.attachments = {RgAttachmentDesc{.name = "rt", .kind = RgAttachmentKind::offscreen,
			.format = std::string{kShadowMapFormat}}},
		.passes = {RgPassDesc{.name = "g", .state = "opaque", .drawSource = "opaque",
			.outputs = {RgPassAttachment{.attachment = "rt", .usage = RgUsage::colorWrite}}}},
	});
	expectErr("depthOffscreen takes color format", RgGraphDesc{
		.attachments = {RgAttachmentDesc{.name = std::string{kShadowMapName},
			.kind = RgAttachmentKind::depthOffscreen, .format = "rgba8_unorm"}},
		.passes = {RgPassDesc{.name = "shadow", .state = "shadow", .drawSource = "shadow",
			.outputs = {shadowOut()}}},
	});
	expectErr("depth attachment with custom extent", RgGraphDesc{
		.attachments = {RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth,
			.extent = {512u, 512u}}},
		.passes = {RgPassDesc{.name = "g", .state = "opaque", .drawSource = "opaque",
			.outputs = {RgPassAttachment{.attachment = "depth",
				.usage = RgUsage::depthWrite}}}},
	});
	expectErr("draw pass with no write output", RgGraphDesc{
		.attachments = {RgAttachmentDesc{.name = std::string{kShadowMapName},
			.kind = RgAttachmentKind::depthOffscreen, .format = std::string{kShadowMapFormat}},
			RgAttachmentDesc{.name = "swapchain"}},
		.passes = {RgPassDesc{.name = "shadow", .state = "shadow", .drawSource = "shadow",
				.inputs = {RgPassAttachment{.attachment = std::string{kShadowMapName},
					.usage = RgUsage::sampleRead}}},
			RgPassDesc{.name = "g", .state = "opaque", .drawSource = "opaque",
				.outputs = {RgPassAttachment{.attachment = "swapchain",
					.usage = RgUsage::colorWrite}}}},
	});
}

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

/** selfCheck：默认图 ao 段（Phase 11 批 11.4）kind/state + **深度转换**断言。
 *
 * 深度那条是 SSAO 能工作的硬前提：ao pass 以 sampleRead 读 depth，编译器须把布局从
 * eDepthStencilAttachmentOptimal 转到 eShaderReadOnlyOptimal。漏这条转换 = ao 采到未
 * 转换的深度（RADV 上表现为整屏 AO 恒 0 或校验层报错），且像素结果不会崩、很难目检发现。 */
void selfCheckDefaultAoPass(const RgPassPlan& ao, std::vector<std::string>& failures) {
	if (ao.kind != RgPassKind::drawLight || ao.state != "ao")
		failures.push_back("default: ao pass should be drawLight/ao");
	if (ao.colors.size() != 1)
		failures.push_back("default: ao pass should have exactly one color target");
	const auto depth = std::ranges::find_if(ao.barriers, [](const auto& b) {
		return b.aspect == vk::ImageAspectFlagBits::eDepth;
	});
	if (depth == ao.barriers.end())
		failures.push_back("default: ao pass lacks a depth layout transition");
	else if (depth->newLayout != vk::ImageLayout::eShaderReadOnlyOptimal)
		failures.push_back("default: ao pass depth must transition to ShaderReadOnly");
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
	// 结构断言（Phase 11 批 11.4 后默认图 = 5 pass：opaque → skybox → transparent →
	// **ao** → tonemap）：首 pass（opaque）两条帧首转换—— scene_hdr（offscreen = 追踪
	// 布局非 discard，声明序在前）+ depth（discard）；skybox 段对 color/depth 各一条
	// identity barrier（跨 render pass 实例 LOAD 执行依赖，old==new 合法）；present
	// 转换来自 tonemap 段的 swapchain colorWrite（CAO→PresentSrc）。
	// ao 段（index 3）额外断言 depth 走 sampleRead 转换（eDepthStencilAttachmentOptimal
	// → eShaderReadOnlyOptimal）——这条是 SSAO 能工作的前提，漏了 = ao 读到未转换的深度。
	const auto& g = defaultRenderGraph();
	if (g.passes.size() != 5)
		failures.push_back("default: expected 5 passes");
	if (g.passes.size() == 5) {
		selfCheckDefaultFirstPass(g.passes[0], failures);
		selfCheckDefaultMidPass(g.passes[1], failures);
		selfCheckDefaultAoPass(g.passes[3], failures);
		selfCheckDefaultLastPass(g.passes[4], failures);
	}
	if (!g.presentTransition ||
		g.presentTransition->newLayout != vk::ImageLayout::ePresentSrcKHR)
		failures.push_back("default: missing present transition");
	// 格式词表断言：inherit→fallback、具名→映射值（表拼错 = Phase 7 静默错图，
	// 编译校验查不出，回读值守卫）；membership 判定与 resolve 一致
	selfCheckFormats(failures);
}


/**
 * selfCheck（Phase 11 批 11.2 阴影基建·装饰侧）：decorateShadowPass 的产物形状断言。
 * 插图**首**而非图末是硬语义（阴影深度得先于任何读它的 pass）——插错位置不会让编译
 * 失败，只是让阴影永远读到上一帧/空内容，所以在这里钉死。
 * @param desc 装饰后的图声明（已就地修改）
 * @return true = 形状正确，可继续编译校验
 */
bool selfCheckShadowDecorate(const RgGraphDesc& desc, std::vector<std::string>& failures) {
	if (desc.passes.empty() || desc.passes.front().name != "shadow") {
		failures.push_back("shadow: decorated graph should start with the shadow pass");
		return false;
	}
	const bool hasAtt = std::ranges::any_of(desc.attachments, [](const RgAttachmentDesc& a) {
		return a.name == kShadowMapName && a.kind == RgAttachmentKind::depthOffscreen;
	});
	if (!hasAtt)
		failures.push_back("shadow: decorateShadowPass should append a depthOffscreen shadow_map");
	// 级联分块（批 11.2 阴影切分）：图首应有 kActiveShadowCascades 个 shadow pass，每个各占图集里自己
	// 那一块子矩形。数量不对 = 某级永远不被渲染；子矩形不对 = 那级会裁到
	// 刦尽那一块的光空间（或与相邻级重叠）。
	size_t nShadow = 0;
	for (const auto& p : desc.passes)
		if (p.state == "shadow")
			++nShadow;
	if (nShadow != kActiveShadowCascades)
		failures.push_back(fmt::format("shadow: expected {} shadow passes, got {}",
			kActiveShadowCascades, nShadow));
	for (size_t c = 0; c < nShadow; ++c) {
		const std::array<uint32_t, 4> want = shadowTile(static_cast<uint32_t>(c),
				shadowTileSize(shadowAtlasColumns(kActiveShadowCascades)),
				shadowAtlasColumns(kActiveShadowCascades));
		if (desc.passes[c].viewport != want)
			failures.push_back(fmt::format("shadow: cascade {} tile viewport mismatch", c));
	}
	return hasAtt;
}

/**
 * selfCheck（Phase 11 批 11.2 阴影基建·编译侧）：三条各管一处"编译能过、GPU 上错得隐蔽"
 * 的点：
 *  ① aspect 必须 eDepth —— 屏障拿 eColor 去转深度图，RADV 上是随机黑块且无校验层报错；
 *  ② 独立 extent 必须透传到编译产物 —— 丢了就是阴影图被悄悄建成窗口尺寸（分辨率随
 *     窗口变 + 每次 resize 白白重建）；
 *  ③ depthWrite 落进 plan.depth 且 colors 为空 —— 执行器照这两个字段建 beginRendering
 *     条目，落错位置 = 阴影图根本没被挂上渲染通道。
 * @param graph decorateShadowPass 后的编译产物
 * @param failures 失败项收集
 */
void selfCheckShadowCompiled(const CompiledRenderGraph& graph,
	std::vector<std::string>& failures) {
	const auto att = std::ranges::find_if(graph.attachments,
		[](const RgCompiledAttachment& a) { return a.name == kShadowMapName; });
	if (att == graph.attachments.end()) {
		failures.push_back("shadow: shadow_map attachment missing from compiled graph");
	} else {
		if (att->aspect != vk::ImageAspectFlagBits::eDepth)
			failures.push_back("shadow: shadow_map aspect should be eDepth (barriers would use eColor)");
		// 图集整幅 = 列数×mapSize（列数跟生效级数走）——断它是否与装饰端同源（不得写死成 2·mapSize）
		if (att->extent[0] != kSelfCheckShadowSize || att->extent[1] != kSelfCheckShadowSize)
			failures.push_back("shadow: shadow_map atlas extent (fixed mapSize) not propagated");
		if (!att->frameStartDiscard)
			failures.push_back("shadow: shadow_map should be in the frame-start-discard class");
	}
	const auto plan = std::ranges::find_if(graph.passes,
		[](const RgPassPlan& p) { return p.name == "shadow"; });
	if (plan == graph.passes.end()) {
		failures.push_back("shadow: shadow pass missing from compiled graph");
		return;
	}
	if (!plan->depth)
		failures.push_back("shadow: shadow pass has no depth plan (shadow map not attached)");
	if (!plan->colors.empty())
		failures.push_back("shadow: depth-only pass should have no color attachment plan");
	// 子矩形必须从 RgPassDesc 透传到编译产物（执行器的 viewport/scissor + renderArea 全按它走；
	// 丢了就是整张图被渲染 4 次，后 3 次把前面的结果清掉）
	if (plan->viewport != shadowTile(0, shadowTileSize(shadowAtlasColumns(kActiveShadowCascades)),
		shadowAtlasColumns(kActiveShadowCascades)))
		failures.push_back("shadow: cascade 0 tile viewport not propagated to plan");
	// 消费方声明（sampleRead 接到光照 pass）必须存在，否则编译器不会为阴影图生成
	// DSAO → ShaderReadOnly 布局转换，采样端报 VVL
	// RgCompiledAttachment 无 slot 字段（槽号 = 它在 attachments 里的下标），barrier 里的 attachment 就是这个下标
	const size_t attSlot = static_cast<size_t>(att - graph.attachments.begin());
	const bool lit = att != graph.attachments.end() && std::ranges::any_of(graph.passes,
		[&](const RgPassPlan& p) {
			return std::ranges::any_of(p.barriers, [&](const RgBarrierPlan& br) {
				return br.attachment == attSlot
					&& br.newLayout == vk::ImageLayout::eShaderReadOnlyOptimal; });
		});
	if (!lit)
		failures.push_back("shadow: no barrier transitions shadow_map to ShaderReadOnly "
			"(sampleRead input missing from lighting pass -> sampling would trip VVL)");
}

/** selfCheck 驱动：装饰 → 编译 → 两侧断言（拆两半是为守住单函数认知复杂度红线）。 */
void selfCheckShadowPass(std::vector<std::string>& failures) {
	auto desc = defaultRenderGraphDesc();
	decorateShadowPass(desc, kSelfCheckShadowSize);  // 无失败分支（返回 void，见其接口注释）
	if (!selfCheckShadowDecorate(desc, failures))
		return;
	auto compiled = compileRenderGraph(desc);
	if (!compiled) {
		failures.push_back(fmt::format("shadow: decorated default graph failed to compile: {}",
			compiled.error().describe()));
		return;
	}
	selfCheckShadowCompiled(*compiled, failures);
}

}  // namespace

/** renderGraphSelfCheck 定义（接口注释见 render_graph.cppm）：拓扑样本 + 结构/词表断言 +
 *  阴影基建断言三块（自 render_graph_impl 拆债），全部失败项收集后返回。 */
std::vector<std::string> renderGraphSelfCheck() {
	std::vector<std::string> failures;
	selfCheckTopology(failures);
	selfCheckDepthOnlyTopology(failures);
	selfCheckStructure(failures);
	selfCheckShadowPass(failures);
	return failures;
}

}  // namespace RGL
