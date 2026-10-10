module;
/**
 * test_mdi_drawindex —— [gpu] Multi Draw Indirect 的 DrawIndex 语义门（docs/mdi_plan.md §6）
 *
 * ## 为什么这是"门"而不是普通用例
 *
 * Phase A（阴影段 MDI 化）的**全部**前提是一句话：
 *   「`vkCmdDrawMeshTasksIndirectEXT(drawCount=N)` 的第 j 个 draw 里，`SV_DrawIndex == j`」。
 * spec 允许 ≠ 驱动实现正确，而本项目已两次为这类判断付过学费
 * （`docs/nvidia_compatibility_report.md` §十：当年"mesh + shader object 必崩"被判成驱动 bug，
 *   实为应用缺 `NO_TASK_SHADER` flag；`tests/test_conservative_mesh.cppm` 同款"spec 允许但没人实测"）。
 * 本用例把它变成可重跑的断言，**且在渲染路径之外**——错了只烧一条用例，不烧画面。
 *
 * ## 自证结构：索引与数据两条信息同源自证
 *
 *   · **画在哪一列** = `push.rowBase + SV_DrawIndex` 推出 row ⟹ 索引类信息
 *   · **写什么值**   = 同一 row 的 word[3]（tag）⟹ 数据类信息
 *
 * 于是两类坏法**可分辨**：DrawIndex 恒 0 → 反复画同一列、其余列保持 clear 哨兵；只是 `+12`
 * 偏移算错 → 列全对但值与列不符。哨兵取 0 而 tag 从 1000 起 ⟹ "根本没画"与任何真 tag 天然
 * 不同值，不需要第二个哨兵值。
 *
 * ## 七条用例各测一件事
 *
 * | 用例 | 被测语义 | 谁依赖它 |
 * |---|---|---|
 * | `capability` | `multiDrawIndirect` 真进链（缺 = drawCount>1 违反 VUID-02718） | 前置 |
 * | `control_drawCount1` | harness 阳性锚点：列映射 / stride-16 行内 +12 读 / 回读路径 | — |
 * | `drawindex_increments` | **核心门**：16 个 draw 各自 DrawIndex 正确 | Phase A |
 * | `base_plus_drawindex` | 均匀 push 基址 + DrawIndex **相加**（= Phase A 的桶基址那一步） | Phase A |
 * | `zero_groupcount_noop` | `x=0` = 空转**且后续 DrawIndex 照旧递增**（= 不跳号） | Phase C |
 * | `per_draw_groupcount` | 每 draw 读**自己那行**的 groupCountX（1/2/3/1 四种数量） | Phase A |
 * | `shader_object_variant` | shader object 绑定形态下同一命题 | Phase A |
 *
 * `zero_groupcount_noop` 是这里最容易被忽略却最贵的一条：spec 说 DrawIndex 对 "each draw
 * **executed**" 递增，而 0 workgroup 的 draw 算不算 executed、驱动会不会**跳号**，直接决定
 * Phase C 能否用"满槽孔洞流"（不压紧、被剔的留 x=0——`stream_fanout` 已是这个形态）。
 * 若跳号，孔洞设计作废，必须改紧凑流。**这条是本文件里唯一一条"结果会推翻已登记设计"的断言。**
 *
 * ## 能力口径（FAIL 不 SKIP，gpu 套件纪律）
 *
 * 无设备 / 缺 VVL / 缺扩展 / 缺 `multiDrawIndirect` = 失败，不静默跳过（静默跳过 = 假绿）。
 *
 * ## 本文件**没有**覆盖的引擎面（如实划界，别让"绿"被读成"整条引擎路径已验"）
 *
 * 探针用**经典 descriptor set + vkCmdPushConstants**，而引擎用 descriptor heap +
 * `vkCmdPushDataEXT`。`shader_object_variant` 补的是"**shader object 绑定形态**"这一半，
 * **没有**补 descriptor heap 那一半（heap + mapping 自建一套等于把 `Resources` 重做一遍，
 * 那部分是 Phase A 集成时由 `just regress` 的逐位红线兜，不在 spike 里假装覆盖）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *   shader 里 `push.rowBase + drawID` 写成 `push.rowBase`（DrawIndex 从未生效）
 *     → `drawindex_increments` / `shader_object_variant` 红（15 列保持哨兵），`control` 仍绿
 *   args 行地址算错（`*16` 写成 `*12`、或忘 `+12`）→ `control_drawCount1` 就红（列对值错）
 *   drawCount 传 1 → 门用例只有第 0 列有值 → 红
 *   `multiDrawIndirect` 没进 required 列表 → 带 VVL 跑被 VUID-02718 打断 + `capability` 红
 *   孔洞行的 x 改回 1 → `zero_groupcount_noop` 红（col5 出现 tag5）
 *   驱动对零计数 draw 跳号 → `zero_groupcount_noop` 红（col6..15 整体左移一格）
 *   `drawMdi` 里 drawCount/stride 互换 → `control_drawCount1` 红（drawCount 被换成 16，
 *     前向锚点"只有第 0 列有值"立刻破；且 stride=1 违反 VUID-07088）
 *
 * ## 🔴 为什么 `kCols` 必须 **≠** `kRowBytes`（别"顺手凑整"回 16）
 *
 * 首版 `kCols == kRowBytes == 16`，而 `drawMdi` 的参数序被我写反成 `(stride, drawCount)`。
 * 于是 `mdi_drawindex_increments`（drawCount=16、stride=16）**互换后数值完全相同 ⟹ 照样绿**，
 * 只有 `control`（drawCount=1）把 VUID-07088 撞出来。**核心门用例是假绿的**，抓到它纯属 control
 * 恰好用了一个不等的数。实测证据 = 本轮变异 M4：互换后只有 control 红。
 *
 * ⟹ 现在 `kCols = 12`、`kRowBytes = 16`，**任何一条用例的两个数都不相等**，互换在每条用例上
 * 都可检测。凑回 16 等于把这颗雷重新埋回去。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>  // VK_QUEUE_FAMILY_IGNORED 等宏（rlvk 不含宏）
#include <cstring>
#include <fstream>
#include <format>
#include <functional>
#include <string>

export module test_mdi_drawindex;

import rlstd;
import rlvk;
import test_device_primitives;  // Fixture / Image2D / makeImage / readImage（引擎同款引导链）
import shader;                  // RenderState / applyDynamicState（引擎实际动态状态配方）
import features;                // queryOptionalFeatures / depthClipDynamicSupported（能力门复用）

namespace RGL::testing::mdi {

using testing::gpu::Fixture;
using testing::gpu::Image2D;

namespace {

constexpr uint32_t kCols = 12u;       // = 探针 shader 的 kCols（一列一个 draw）
constexpr uint32_t kRows = 8u;        // RT 高（纵向铺满，取值不进判据）
constexpr uint32_t kMaxDraws = 12u;   // args 行容量（= kCols，一 draw 一行）
constexpr uint32_t kRowBytes = 16u;   // 行步长：前 12B 恰是 VkDrawMeshTasksIndirectCommandEXT
constexpr uint32_t kTagBase = 1000u;  // tag_i = kTagBase + i（clear 哨兵 = 0，天然可辨）
constexpr uint32_t kClearSentinel = 0u;
constexpr vk::Format kFmt = vk::Format::eR32Uint;

/** 一行 = 一条 indirect args + 探针载荷（Phase A 的"一张 buffer 两个读者"手法）。 */
struct ArgsRow {
	uint32_t x = 1, y = 1, z = 1, tag = 0;
};
static_assert(sizeof(ArgsRow) == 16, "ArgsRow 必须与 16B MDI 行布局一致（前 12B 是 args）");

std::vector<uint32_t> readSpirv(const std::string& path) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
	// spv 缺失 = 响亮失败（探针 shader 由 tests/CMakeLists.txt 的 compile_slang 产出）
	REQUIRE(f);
	const auto n = static_cast<size_t>(f.tellg());
	REQUIRE(n % 4u == 0u);
	std::vector<uint32_t> code(n / 4u);
	f.seekg(0);
	f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(n));
	return code;
}

vk::raii::ShaderModule makeModule(const Fixture& fx, const std::string& path) {
	const auto code = readSpirv(path);
	vk::ShaderModuleCreateInfo mi{};
	mi.setCode(code);
	return vk::raii::ShaderModule(fx.device(), mi);
}

/**
 * args buffer：host-visible + **INDIRECT_BUFFER**（VUID-buffer-02709）+ STORAGE_BUFFER
 * （同一张 buffer 被 shader 当 SSBO 读——Phase A 的核心前提之一，本用例顺手验掉）。
 */
struct ArgsBuffer {
	vk::raii::Buffer buffer;
	vk::raii::DeviceMemory memory;
};

ArgsBuffer makeArgsBuffer(const Fixture& fx, std::span<const ArgsRow> rows) {
	const size_t bytes = rows.size() * sizeof(ArgsRow);
	vk::BufferCreateInfo bi{};
	bi.size = bytes;
	bi.usage = vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eStorageBuffer;
	bi.sharingMode = vk::SharingMode::eExclusive;
	vk::raii::Buffer buf(fx.device(), bi);

	const auto req = buf.getMemoryRequirements();
	const auto props = fx.physical().getMemoryProperties();
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
		const auto f = static_cast<vk::MemoryPropertyFlags>(props.memoryTypes[i].propertyFlags);
		const auto want = vk::MemoryPropertyFlagBits::eHostVisible
			| vk::MemoryPropertyFlagBits::eHostCoherent;
		if ((req.memoryTypeBits & (1u << i)) != 0 && (f & want) == want) {
			type = i;
			break;
		}
	}
	// 找不到 host-visible+coherent 类型 = 环境异常，必须响亮失败（不静默换 device-local——
	// 那会让"CPU 每帧写 args"这条路根本没被建立起来，用例却仍然"绿"）
	REQUIRE(type != UINT32_MAX);
	vk::MemoryAllocateInfo ai{};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	vk::raii::DeviceMemory mem(fx.device(), ai);
	buf.bindMemory(mem, 0);  // VUID-buffer-02708：整块连续绑定到单一 DeviceMemory
	void* p = mem.mapMemory(0, req.size);
	std::memcpy(p, rows.data(), bytes);
	return ArgsBuffer{.buffer = std::move(buf), .memory = std::move(mem)};
}

/** binding 0 = args SSBO；push constant = rowBase（4B，仅 MESH stage）。 */
struct LayoutParts {
	vk::raii::DescriptorSetLayout dsLayout;
	vk::raii::DescriptorPool pool;
	std::vector<vk::raii::DescriptorSet> sets;
	vk::raii::PipelineLayout pipeLayout;
	// push 段的**唯一声明点**：管线形态（pipeLayout 内）与 shader object 形态
	//（createInfo.pPushConstantRanges）必须读同一份，否则两处各写各的 = 布局漂移
	vk::PushConstantRange pushRange;
};

LayoutParts makeLayout(const Fixture& fx, const ArgsBuffer& args, size_t argsBytes) {
	vk::DescriptorSetLayoutBinding lb{};
	lb.binding = 0;
	// ⚠ 不能用 eAllGraphics：VK_SHADER_STAGE_ALL_GRAPHICS 按定义**不含** MESH/TASK，
	//   而本探针的 args 只有 mesh stage 读。写成 ALL_GRAPHICS = 该 binding 对 mesh 不可见
	//   = "no descriptor bound" 一类 VVL 错。必须显式点 MESH。
	lb.stageFlags = vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment;
	lb.descriptorCount = 1;
	lb.descriptorType = vk::DescriptorType::eStorageBuffer;
	vk::DescriptorSetLayoutCreateInfo li{};
	li.setBindings(lb);
	vk::raii::DescriptorSetLayout dsLayout(fx.device(), li);

	vk::PushConstantRange pc{};
	pc.stageFlags = vk::ShaderStageFlagBits::eMeshEXT;
	pc.offset = 0;
	pc.size = 4;  // rowBase（scalar layout，无 padding）
	vk::PipelineLayoutCreateInfo plci{};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &*dsLayout;
	plci.setPushConstantRanges(pc);
	vk::raii::PipelineLayout pipeLayout(fx.device(), plci);

	const std::array<vk::DescriptorPoolSize, 1> poolSizes{
		vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 1}};
	vk::DescriptorPoolCreateInfo pi{};
	pi.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	pi.setPoolSizes(poolSizes);
	pi.setMaxSets(1);
	vk::raii::DescriptorPool pool(fx.device(), pi);

	vk::DescriptorSetAllocateInfo sai{};
	sai.descriptorPool = *pool;
	sai.descriptorSetCount = 1;
	sai.pSetLayouts = &*dsLayout;
	auto sets = fx.device().allocateDescriptorSets(sai);
	REQUIRE(sets.size() == 1);
	const vk::DescriptorBufferInfo bi{*args.buffer, 0, argsBytes};
	// 逐字段填（hpp 的 WriteDescriptorSet 聚合初始化字段数与裸 C 不一致，203 行的
	// "excess elements" 即此；izb_mask 用例同款写法）
	vk::WriteDescriptorSet w{};
	w.dstSet = *sets[0];
	w.dstBinding = 0;
	w.dstArrayElement = 0;
	w.descriptorCount = 1;
	w.descriptorType = vk::DescriptorType::eStorageBuffer;
	w.pBufferInfo = &bi;
	fx.device().updateDescriptorSets(w, {});
	return LayoutParts{.dsLayout = std::move(dsLayout), .pool = std::move(pool),
		.sets = std::move(sets), .pipeLayout = std::move(pipeLayout), .pushRange = pc};
}

/** 目标图（R32_UINT 色附件 + 可回读）+ view。 */
struct Target {
	Image2D img;
	vk::raii::ImageView view;
};

Target makeTarget(const Fixture& fx) {
	auto img = testing::gpu::makeImage(fx, kFmt, kCols, kRows,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc);
	vk::ImageViewCreateInfo vi{};
	vi.image = *img.image;
	vi.viewType = vk::ImageViewType::e2D;
	vi.format = kFmt;
	vi.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	return Target{.img = std::move(img), .view = vk::raii::ImageView(fx.device(), vi)};
}

void barrier(vk::raii::CommandBuffer& cmd, const vk::raii::Image& img, vk::ImageLayout oldL,
	vk::ImageLayout newL, vk::PipelineStageFlags2 srcSt, vk::AccessFlags2 srcAcc,
	vk::PipelineStageFlags2 dstSt, vk::AccessFlags2 dstAcc) {
	vk::ImageMemoryBarrier2 b{};
	b.srcStageMask = srcSt;
	b.srcAccessMask = srcAcc;
	b.dstStageMask = dstSt;
	b.dstAccessMask = dstAcc;
	b.oldLayout = oldL;
	b.newLayout = newL;
	b.image = img;
	b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	vk::DependencyInfo dep{};
	dep.setImageMemoryBarriers(b);
	cmd.pipelineBarrier2(dep);
}

/** beginRendering：clear 成哨兵 0（整数附件走 uint32 视图，不是 float）。 */
void beginRender(vk::raii::CommandBuffer& cmd, const Target& t) {
	barrier(cmd, t.img.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::AccessFlagBits2::eColorAttachmentWrite);
	vk::RenderingAttachmentInfo att{};
	att.imageView = *t.view;
	att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	att.loadOp = vk::AttachmentLoadOp::eClear;
	att.storeOp = vk::AttachmentStoreOp::eStore;
	att.clearValue = vk::ClearColorValue(std::array{
		kClearSentinel, kClearSentinel, kClearSentinel, kClearSentinel});
	vk::RenderingInfo r{};
	r.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, {kCols, kRows}};
	r.layerCount = 1;
	r.setColorAttachments(att);
	cmd.beginRendering(r);
}

void endRender(vk::raii::CommandBuffer& cmd, const Target& t) {
	cmd.endRendering();
	barrier(cmd, t.img.image, vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eGeneral,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone);
}

/** 管线形态的 mesh+frag 管线（cullMode NONE：绕序不进判据；无深度附件）。 */
vk::raii::Pipeline makePipeline(const Fixture& fx, const vk::raii::PipelineLayout& layout,
	const vk::raii::ShaderModule& meshMod, const vk::raii::ShaderModule& fragMod) {
	const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
		{vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eMeshEXT, *meshMod, "main"},
		{vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eFragment, *fragMod, "main"}}};
	vk::PipelineRasterizationStateCreateInfo rs{};
	rs.setDepthClampEnable(false);
	rs.setRasterizerDiscardEnable(false);
	rs.setPolygonMode(vk::PolygonMode::eFill);
	rs.setCullMode(vk::CullModeFlagBits::eNone);
	rs.setFrontFace(vk::FrontFace::eCounterClockwise);
	rs.setDepthBiasEnable(false);
	rs.setLineWidth(1.0f);
	// 正高度 viewport：探针每列纵向铺满 [-1,1] ⟹ y 翻转不进判据（刻意规避，坐标系是 izb_1b
	// 已结案的事，本文件只欠 DrawIndex 一个变量）
	vk::Viewport vp{0.0f, 0.0f, static_cast<float>(kCols), static_cast<float>(kRows), 0.0f, 1.0f};
	vk::Rect2D sc{vk::Offset2D{0, 0}, {kCols, kRows}};
	vk::PipelineViewportStateCreateInfo vs{};
	vs.viewportCount = 1;
	vs.pViewports = &vp;
	vs.scissorCount = 1;
	vs.pScissors = &sc;
	vk::PipelineMultisampleStateCreateInfo ms{};
	ms.setRasterizationSamples(vk::SampleCountFlagBits::e1);
	vk::PipelineColorBlendAttachmentState cba{};
	cba.setBlendEnable(false);
	cba.setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
		| vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendStateCreateInfo cb{};
	cb.setAttachments(cba);
	vk::PipelineRenderingCreateInfo ri{};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &kFmt;
	vk::GraphicsPipelineCreateInfo pci{};
	pci.setStages(stages);
	pci.layout = *layout;
	pci.pViewportState = &vs;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pColorBlendState = &cb;
	pci.pNext = &ri;
	// mesh 管线刻意不填 pVertexInputState / pInputAssemblyState（spec：有 mesh stage 时 ignored）
	return vk::raii::Pipeline(fx.device(), nullptr, pci);
}

/**
 * MDI 录制口——**唯一**允许出现在本文件的 draw 调用，参数序封死在这里。
 *
 * 为什么必须有这层：裸签名是 `(..., offset, drawCount, stride)`（vulkan_core.h:27021），
 * 首版按 `(buffer, offset, stride, drawCount)` 写，五处调用点全错。而 `kRowBytes == kCols == 16`
 * ⟹ **核心门用例参数互换后数值相同、照样绿**（一次假绿，被抓到纯因 control 用例的
 * drawCount/stride 两值不相等，把 VUID-07088 撞了出来）。
 * 收成一个具名入参的函数后，调用点只剩"画几个"，交换在结构上不可能。
 */
void drawMdi(vk::raii::CommandBuffer& cmd, const vk::raii::Buffer& args, uint32_t drawCount) {
	cmd.drawMeshTasksIndirectEXT(*args, /*offset=*/0, /*drawCount=*/drawCount, /*stride=*/kRowBytes);
}

/** 一次 MDI 渲染 + 整幅回读（返回逐像素 uint32，行主序）。 */
std::vector<uint32_t> renderAndRead(const Fixture& fx, const Target& t,
	const std::function<void(vk::raii::CommandBuffer&)>& draw) {
	fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
		beginRender(cmd, t);
		draw(cmd);
		endRender(cmd, t);
	});
	const auto bytes = testing::gpu::readImage(fx, t.img);
	std::vector<uint32_t> px(bytes.size() / 4u);
	std::memcpy(px.data(), bytes.data(), bytes.size());
	return px;
}

/** 判据：第 c 列（取 row 0）是否为期望值。 */
uint32_t colValue(const std::vector<uint32_t>& px, uint32_t col) {
	return px[col];
}

std::string dumpCols(const std::vector<uint32_t>& px) {
	std::string s = "cols ";
	for (uint32_t c = 0; c < kCols; ++c)
		s += std::format("{}:{} ", c, colValue(px, c));
	return s;
}

/**
 * 全套管线形态资源（每条用例一套，避免跨用例状态）。
 *
 * 🔴 **成员声明序 = 初始化序 = 依赖序**（C++ 按声明序初始化，与写在这里的先后无关）：
 *   `args` 必须先于 `lp`——`makeLayout` 要读 `args.buffer` 填 descriptor；
 *   `lp.pipeLayout` + `mesh`/`frag` 必须先于 `pipe`——管线创建期就要 layout 与 module。
 * 调换声明序不会报错、只会把已释放句柄喂进去（-Wreorder 只比对初始化列表与声明序不一致，
 * 读到未初始化成员则是静默 UB），故本注释与下方声明序一起看。
 */
struct Rig {
	explicit Rig(const Fixture& fx)
		: rows([] {
			  std::array<ArgsRow, kMaxDraws> r{};
			  for (uint32_t i = 0; i < kMaxDraws; ++i)
				  r[i] = ArgsRow{1u, 1u, 1u, kTagBase + i};
			  return r;
		  }()),
		  args(makeArgsBuffer(fx, rows)),
		  lp(makeLayout(fx, args, rows.size() * sizeof(ArgsRow))),
		  mesh(makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.mesh.spv")),
		  frag(makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv")),
		  pipe(makePipeline(fx, lp.pipeLayout, mesh, frag)),
		  target(makeTarget(fx)) {}

	std::array<ArgsRow, kMaxDraws> rows;
	ArgsBuffer args;
	LayoutParts lp;
	vk::raii::ShaderModule mesh;
	vk::raii::ShaderModule frag;
	vk::raii::Pipeline pipe;
	Target target;
};

}  // namespace

}  // namespace RGL::testing::mdi

// ═══════════════════════════════════════════════════════════════════════════
// 用例
// ═══════════════════════════════════════════════════════════════════════════

TEST_SUITE("gpu") {

using RGL::testing::mdi::ArgsRow;
using RGL::testing::mdi::colValue;
using RGL::testing::mdi::drawMdi;
using RGL::testing::mdi::dumpCols;
using RGL::testing::mdi::kClearSentinel;
using RGL::testing::mdi::kCols;
using RGL::testing::mdi::kMaxDraws;
using RGL::testing::mdi::kRowBytes;
using RGL::testing::mdi::kTagBase;
using RGL::testing::mdi::makeArgsBuffer;
using RGL::testing::mdi::makeLayout;
using RGL::testing::mdi::makeModule;
using RGL::testing::mdi::makePipeline;
using RGL::testing::mdi::makeTarget;
using RGL::testing::mdi::readSpirv;
using RGL::testing::mdi::renderAndRead;
using RGL::testing::mdi::Rig;
using RGL::testing::gpu::Fixture;

TEST_CASE("mdi_capability：multiDrawIndirect 真进了 required 链（缺则 drawCount>1 违反 VUID-02718）") {
	const Fixture fx;
	const auto feats = fx.physical().getFeatures2();
	// 本文件全部判据都依赖 features_config.json 的 multiDrawIndirect。它没进链时症状**不是**
	// "用例红"而是"带 VVL 被 VUID-02718 打断"，所以先在这里点名，别让下一个人去查着色器。
	CHECK(feats.features.multiDrawIndirect != 0);
	CHECK(fx.physical().getProperties().limits.maxDrawIndirectCount >= kMaxDraws);
	INFO(std::format("device={} maxDrawIndirectCount={}", fx.deviceName(),
		fx.physical().getProperties().limits.maxDrawIndirectCount));
}

TEST_CASE("mdi_control_drawCount1（harness 阳性锚点）：drawCount=1 只画第 0 列，其余保持哨兵") {
	const Fixture fx;
	// 这条**不测 DrawIndex**（drawCount=1 时 DrawIndex 恒 0，本就无信息）。它测列映射 /
	// stride-16 行内 +12 读 / 整数回读路径。"门用例绿"必须建立在这条绿之上——否则可能只是
	// "什么都没画对、但恰好也没多出列"。
	const Rig rig(fx);
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, rig.target, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *rig.pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *rig.lp.pipeLayout, 0,
			*rig.lp.sets[0], {});
		cmd.pushConstants(*rig.lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		drawMdi(cmd, rig.args.buffer, 1u);
	});
	INFO(dumpCols(px));
	CHECK(colValue(px, 0) == kTagBase + 0u);  // 第 0 列 = 第 0 行的 tag（+12 偏移读对了）
	for (uint32_t c = 1; c < kCols; ++c)
		CHECK(colValue(px, c) == kClearSentinel);  // 负向锚点：不该出现的列没出现
}

TEST_CASE("mdi_drawindex_increments（核心门）：drawCount=16 时每个 draw 的 DrawIndex 各自正确") {
	const Fixture fx;
	const Rig rig(fx);
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, rig.target, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *rig.pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *rig.lp.pipeLayout, 0,
			*rig.lp.sets[0], {});
		cmd.pushConstants(*rig.lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		drawMdi(cmd, rig.args.buffer, kCols);
	});
	INFO(dumpCols(px));
	// 失败形态自带负向锚点：若 DrawIndex 恒 0（DGC 那种直接-draw 语义泄漏进 MDI），16 个 draw
	// 全画第 0 列、tag 全 = kTagBase ⟹ col1..15 保持哨兵 ⟹ 本循环红。
	for (uint32_t c = 0; c < kCols; ++c)
		CHECK(colValue(px, c) == kTagBase + c);
}

TEST_CASE("mdi_base_plus_drawindex：均匀 push rowBase=6 + DrawIndex = 列 6..11（Phase A 那一步）") {
	const Fixture fx;
	// Phase A 的形态：一桶一次 MDI 调用，**桶首行号走 push**（一次调用内均匀、合法），
	// 桶内偏移走 DrawIndex。这条测的就是两者相加在真驱动上成立。
	std::array<ArgsRow, kMaxDraws> rows{};
	for (uint32_t i = 0; i < kMaxDraws; ++i)
		rows[i] = ArgsRow{1u, 1u, 1u, kTagBase + i};
	const auto args = makeArgsBuffer(fx, rows);
	const auto lp = makeLayout(fx, args, rows.size() * sizeof(ArgsRow));
	const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.mesh.spv");
	const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");
	const auto pipe = makePipeline(fx, lp.pipeLayout, mesh, frag);
	const auto t = makeTarget(fx);
	const uint32_t rowBase = 6u;
	const auto px = renderAndRead(fx, t, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *lp.pipeLayout, 0, *lp.sets[0], {});
		cmd.pushConstants(*lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		drawMdi(cmd, args.buffer, 6u);
	});
	INFO(dumpCols(px));
	for (uint32_t c = 0; c < 6; ++c)
		CHECK(colValue(px, c) == kClearSentinel);  // 基址被当成 0 用 → 这几列会出现 tag → 红
	for (uint32_t c = 6; c < kCols; ++c)
		CHECK(colValue(px, c) == kTagBase + c);    // 后 6 列 = 行 6..11
}

TEST_CASE("mdi_zero_groupcount_noop：x=0 = 空转，且后续 DrawIndex **照旧递增不跳号**（Phase C 前提）") {
	const Fixture fx;
	// 本条唯一"结果能推翻已登记设计"：Phase C 打算用满槽孔洞流（不压紧，被剔的留 x=0，
	// 与既有 stream_fanout.comp 同形态）。spec 说 DrawIndex 对 "each draw executed" 递增，
	// 而 0 workgroup 的 draw 算不算 executed、驱动会不会跳号，**没有先例量过**。
	// 跳号 ⟹ col6..15 整体左移 ⟹ 本条红 ⟹ 孔洞设计作废、必须改紧凑流（atomic 压紧 + 前缀和）。
	std::array<ArgsRow, kMaxDraws> rows{};
	for (uint32_t i = 0; i < kMaxDraws; ++i)
		rows[i] = ArgsRow{1u, 1u, 1u, kTagBase + i};
	rows[5] = ArgsRow{0u, 0u, 0u, kTagBase + 5};  // 孔洞
	const auto args = makeArgsBuffer(fx, rows);
	const auto lp = makeLayout(fx, args, rows.size() * sizeof(ArgsRow));
	const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.mesh.spv");
	const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");
	const auto pipe = makePipeline(fx, lp.pipeLayout, mesh, frag);
	const auto t = makeTarget(fx);
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, t, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *lp.pipeLayout, 0, *lp.sets[0], {});
		cmd.pushConstants(*lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		drawMdi(cmd, args.buffer, kCols);
	});
	INFO(dumpCols(px));
	CHECK(colValue(px, 5) == kClearSentinel);  // 孔洞行 = 没画（x=0 是合法空转，不是 VU）
	for (uint32_t c = 0; c < kCols; ++c) {
		if (c == 5)
			continue;
		CHECK(colValue(px, c) == kTagBase + c);  // c>5 这几列是关键：跳号必整体错位
	}
}

TEST_CASE("mdi_per_draw_groupcount：每 draw 读到自己那行的 groupCountX（Phase A 的 1591 种数量）") {
	const Fixture fx;
	// 上面几条用例里所有行的 x 都是 1，于是**没有任何一条**担保过"GPU 逐 draw 读自己的 x"。
	// 而 Phase A 的现实是 1591 个 draw 的 groupCount = ceil(totalTriangles/10) **各不相同**，
	// 这条才是那件事的判据：每 draw 发 n 个 workgroup、每个 workgroup 独占一列并写 tag+i。
	// x 读大 → 多出列被画（哨兵破）；读小 → 该有的列保持哨兵。两个方向都覆盖。
	constexpr uint32_t kRowsN = 4u;          // args 行数
	constexpr uint32_t kGpd = 3u;            // 每 draw 最多几列（= 探针 shader 的 kGroupsPerDraw）
	std::array<ArgsRow, kRowsN> rows{{
		ArgsRow{1u, 1u, 1u, 1000u},          // 只占 1 列
		ArgsRow{2u, 1u, 1u, 2000u},          // 占 2 列
		ArgsRow{3u, 1u, 1u, 3000u},          // 占满 3 列
		ArgsRow{1u, 1u, 1u, 4000u}}};        // 又只占 1 列（证明不是"恒 3"）
	// kGpd 与行数的乘积必须等于 RT 宽，否则"列缺失"与"列越界"两种坏法分不开
	static_assert(kRowsN * kGpd == kCols, "group 探针的列数必须铺满 RT 宽");

	const auto args = makeArgsBuffer(fx, rows);
	const auto lp = makeLayout(fx, args, rows.size() * sizeof(ArgsRow));
	const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_groups.mesh.spv");
	const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");
	const auto pipe = makePipeline(fx, lp.pipeLayout, mesh, frag);
	const auto t = makeTarget(fx);
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, t, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *lp.pipeLayout, 0, *lp.sets[0], {});
		cmd.pushConstants(*lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		drawMdi(cmd, args.buffer, kRowsN);
	});
	INFO(dumpCols(px));
	// 期望**由输入表推导**（不是手抄一张表）：行 r 的前 x_r 列 = tag_r + i，其余列 = 哨兵
	for (uint32_t r = 0; r < kRowsN; ++r) {
		for (uint32_t i = 0; i < kGpd; ++i) {
			const uint32_t col = r * kGpd + i;
			const uint32_t want = i < rows[r].x ? rows[r].tag + i : kClearSentinel;
			INFO(std::format("row {} (x={}) col {}", r, rows[r].x, col));
			CHECK(colValue(px, col) == want);
		}
	}
}

TEST_CASE("mdi_shader_object_variant：引擎的绑定形态（shader object + 全动态状态）下同一命题") {
	const Fixture fx;
	// 前四条走 VkPipeline，而**引擎不用管线**（shader object + 全动态状态）。"DrawIndex 的填充
	// 与绑定形态无关"是合理期待，但"测的不是引擎那条路"正是本项目反复登记的盲区
	//（tests/CMakeLists.txt 里 izb_engine_contract 就是为同一件事补的），故补一条。
	// ⚠ 本条补的是"shader object 绑定"这一半，**不含** descriptor heap（见文件头划界）。
	const Rig rig(fx);
	const auto meshCode = readSpirv(RGL_TEST_SHADER_DIR "/mdi_spike.mesh.spv");
	const auto fragCode = readSpirv(RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");

	// mesh 带 NO_TASK_SHADER（引擎同款，rgl/shader.cppm:502）⟹ VUID-None-08695 要求 task stage
	// 不绑实句柄（下面的七槽里 task 正是 null）。漏这个 flag 的历史代价 = NVIDIA Xid 13，
	// 见 docs/nvidia_compatibility_report.md §十。
	// ⚠ 走**裸 C 结构 + 包一层**（rgl/shader.cppm:497-520 同款，项目里已验证的写法）：
	//   vk::ShaderCreateInfoEXT 包装类没有 codeType/layout 这些 setter（枚举名也不同），
	//   首版按裸 C 字段名直接写在 hpp 对象上 → 9 处编译错。C 侧字段是权威面。
	const VkDescriptorSetLayout dsl = *rig.lp.dsLayout;  // raii → 裸句柄（取地址前先落本地变量）
	const VkPushConstantRange pcRaw = static_cast<VkPushConstantRange>(rig.lp.pushRange);
	std::array<VkShaderCreateInfoEXT, 2> raw{};
	raw[0].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	raw[0].flags = VK_SHADER_CREATE_NO_TASK_SHADER_BIT_EXT;
	raw[0].stage = VK_SHADER_STAGE_MESH_BIT_EXT;
	raw[0].nextStage = VK_SHADER_STAGE_FRAGMENT_BIT;
	raw[0].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	raw[0].codeSize = meshCode.size() * sizeof(uint32_t);
	raw[0].pCode = meshCode.data();
	raw[0].pName = "main";
	// shader object **没有** layout 字段——它直接收 setLayouts + pushConstantRanges
	//（vulkan_core.h:23628-23643）。经典 descriptor set 与 push 因此照常可用，
	// 只是声明处从 pipelineLayout 挪到了每个 createInfo 上（spec: shaders.adoc:298-307）。
	raw[0].setLayoutCount = 1;
	raw[0].pSetLayouts = &dsl;
	raw[0].pushConstantRangeCount = 1;
	raw[0].pPushConstantRanges = &pcRaw;
	raw[1].sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	raw[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	raw[1].codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	raw[1].codeSize = fragCode.size() * sizeof(uint32_t);
	raw[1].pCode = fragCode.data();
	raw[1].pName = "main";
	raw[1].setLayoutCount = 1;
	raw[1].pSetLayouts = &dsl;
	raw[1].pushConstantRangeCount = 1;
	raw[1].pPushConstantRanges = &pcRaw;
	const std::array<vk::ShaderCreateInfoEXT, 2> ci{vk::ShaderCreateInfoEXT(raw[0]),
		vk::ShaderCreateInfoEXT(raw[1])};
	const auto shaders = fx.device().createShadersEXT(ci);
	REQUIRE(shaders.size() == 2);

	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, rig.target, [&](vk::raii::CommandBuffer& cmd) {
		// 引擎配方：七个 stage 全绑、未用的传 null（rgl/shader.cppm RenderPipeline::bindShaders）
		const std::array<vk::ShaderStageFlagBits, 7> stages = {
			vk::ShaderStageFlagBits::eVertex, vk::ShaderStageFlagBits::eTessellationControl,
			vk::ShaderStageFlagBits::eTessellationEvaluation, vk::ShaderStageFlagBits::eGeometry,
			vk::ShaderStageFlagBits::eTaskEXT, vk::ShaderStageFlagBits::eMeshEXT,
			vk::ShaderStageFlagBits::eFragment};
		std::array<vk::ShaderEXT, 7> handles{};
		handles[5] = *shaders[0];
		handles[6] = *shaders[1];
		cmd.bindShadersEXT(stages, handles);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *rig.lp.pipeLayout, 0,
			*rig.lp.sets[0], {});
		cmd.pushConstants(*rig.lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		// 动态状态用**引擎那份配方**（含负高度 viewport 翻转），不在这里自造一套。
		// ⚠ depthClipDynamic 必须传**能力位**而不是 false：VVL 实测报 VUID-…-07633
		//   （shader object 下 depthClipEnable 动态态必须被 set）。能力判定**复用引擎的
		//   双条件门** features.cppm::depthClipDynamicSupported，不在这里重推一遍
		//   （重推 = 第二个真相源，正是 AGENTS 反复点名的漂移形态）。
		const auto opt = RGL::queryOptionalFeatures(fx.physical());
		RGL::RenderState state{};
		state.depthTest = false;  // 探针无深度附件
		state.depthWrite = false;
		RGL::applyDynamicState(cmd, vk::Rect2D{vk::Offset2D{0, 0}, {kCols, 8u}}, state,
			RGL::depthClipDynamicSupported(opt), 1);
		drawMdi(cmd, rig.args.buffer, kCols);
	});
	INFO(dumpCols(px));
	for (uint32_t c = 0; c < kCols; ++c)
		CHECK(colValue(px, c) == kTagBase + c);
}

}  // TEST_SUITE("gpu")
