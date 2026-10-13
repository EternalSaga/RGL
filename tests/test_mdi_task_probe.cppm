module;
/**
 * test_mdi_task_probe —— [gpu] task 路 MDI 机制探针（docs/mdi_plan.md §19.12）
 *
 * ## 本文件回答的唯一问题
 *
 * `SV_DrawIndex` **只在 task 入口声明**（mesh 入口完全不碰它），行信息经 payload 交给 mesh ——
 * 这个形态（§19.9 选项② / §19.12 的"task 传统路"）在三家驱动上是否成立、以及它是否**摆脱**
 * 8060S 实测的毒化（RADV gfx1151：同提交里一枚声明 DrawIndex 的 mesh draw 之后，后续所有
 * 直接 mesh draw 不产出像素）。
 *
 * ## 与 test_mdi_drawindex.cppm 的关系：换的是"读 DrawIndex 的那一方"，判据骨架不动
 *
 * mesh 路探针（同目录上一文件）测「mesh 入口自己算 row」；本文件把 row 查找整体搬进 task，
 * mesh 只消费 payload。列映射/值域/哨兵三件套与那边**同构但独立数值**（行号×3 起步的列布局、
 * tag 步长 100、守卫值 0xBEEF），一处漂移两边互验、不会同错。
 *
 * ## 自证结构（三条信息流各自有锚）
 *
 *   · **索引流** task: row = push.rowBase + DrawIndex → 列位置
 *   · **数据流** task: args 行 word[3]（tag）+ tid.x → 像素值，**经 payload** 到达 mesh
 *     （payload 通路坏 = mesh 读到 0/错值 = 列对值错，与"索引坏=列错位"可分辨）
 *   · **守卫流** 同一 command buffer 在 task-MDI **之后**直接 draw 一枚**不声明 DrawIndex、
 *     不读 payload、不读任何资源**的 mesh-only 管线（守卫列 12 固定值）——STRIX 失效形态的
 *     检测器：task 若也毒化后续 draw，守卫列保持哨兵 0 ⟹ 红。
 *     🔴 这一条就是带去 STRIX 机重跑的那条（§19.12 拍板的两步走之第 2 步）。
 *
 * ## 能力口径（FAIL 不 SKIP，gpu 套件纪律）
 *
 * 无设备 / 缺扩展 / 缺 multiDrawIndirect = 失败，不静默跳过。
 *
 * ## 本文件**没有**覆盖的（如实划界）
 *
 * - descriptor heap + vkCmdPushDataEXT 引擎绑定形态下的 task 路（M-1b 用的是 push **常量**
 *   rowBase，引擎那边是 heap 行表 + pushData——集成时机工由重做批在引擎臂上跑 A3 逐位兜）。
 * - task payload 与 mesh 输入的**跨模块匹配**由 VVL 守（本文件带 VVL 跑，07295 一类入参错
 *   会当场点名）；而 slang#10123 的"task+mesh 同模块 payload 复制"坑已用**分模块编译**规避
 *   （两个 .slang 两份 SPV，勿合并）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   task 里 `push.rowBase + drawID` 写成 `push.rowBase`（DrawIndex 从未生效）
 *     → `drawindex_increments_task` 红（col 3..11 保持哨兵），`control` 仍绿（drawCount=1 无信息）
 *   task 的 payload 字段互换（col↔value）→ control 就红（列对值错 / 值当列号 → 越界列）
 *   mesh 里 `tp.col` 改成常量 → 除 control 外全红（列全堆一列）
 *   孔洞行 x 改回 1 → `zero_taskcount_noop_task` 红（col3..5 出现 tag）
 *   驱动对 x=0 的 draw 跳号 → `zero_taskcount_noop_task` 红（col6..11 整体错位）
 *   **驱动 task 路毒化后续 draw（STRIX 形态）** → `poison_guard_after_task_mdi` 红（守卫列=0），
 *     数据列仍对（两种坏法在同一用例里同时可分辨）
 *   `drawMdi` 的 drawCount/stride 互换 → control（1≠16）立刻撞 VUID-07088/07089
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>  // VK_QUEUE_FAMILY_IGNORED 等宏（rlvk 不含宏）
#include <cstring>
#include <fstream>
#include <format>
#include <functional>
#include <string>

export module test_mdi_task_probe;

import rlstd;
import rlvk;
import test_device_primitives;  // Fixture / Image2D / makeImage / readImage（引擎同款引导链）

namespace RGL::testing::mditask {

using testing::gpu::Fixture;
using testing::gpu::Image2D;

namespace {

constexpr uint32_t kCols = 16u;       // RT 宽：列 0..11 = 数据（4 行 × 3），列 12 = 守卫列
constexpr uint32_t kRows = 8u;        // RT 高（纵向铺满，不进判据）
constexpr uint32_t kRowBytes = 16u;   // 行步长：前 12B = VkDrawMeshTasksIndirectCommandEXT（task 计数）
constexpr uint32_t kColsPerRow = 3u;  // 每行最多几列（= task shader 的 kColsPerRow，手工同源）
constexpr uint32_t kMaxDraws = 4u;    // args 行容量
constexpr uint32_t kTagStep = 100u;   // tag_r = 1000 + 100·r（步长 100 ⟹ 值域与列域、守卫值互不重叠）
constexpr uint32_t kTagBase = 1000u;
constexpr uint32_t kGuardCol = 12u;   // = mdi_task_probe_guard.mesh.slang 里写死的列
constexpr uint32_t kGuardValue = 48879u;  // 0xBEEF，与守卫 shader 手工同源
constexpr uint32_t kClearSentinel = 0u;
constexpr vk::Format kFmt = vk::Format::eR32Uint;

struct ArgsRow {
	uint32_t x = 1, y = 1, z = 1, tag = 0;
};
static_assert(sizeof(ArgsRow) == kRowBytes, "ArgsRow 必须与 16B MDI 行布局一致（前 12B 是 args）");

std::vector<uint32_t> readSpirv(const std::string& path) {
	std::ifstream f(path, std::ios::binary | std::ios::ate);
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

/** args buffer：host-visible + INDIRECT_BUFFER（VUID-buffer-02709）+ STORAGE_BUFFER（task 读）。 */
struct ArgsBuffer {
	vk::raii::Buffer buffer;
	vk::raii::DeviceMemory memory;
};

ArgsBuffer makeArgs(const Fixture& fx, std::span<const ArgsRow> rows) {
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
		const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
		if ((req.memoryTypeBits & (1u << i)) != 0 && (f & want) == want) { type = i; break; }
	}
	REQUIRE(type != UINT32_MAX);
	vk::MemoryAllocateInfo ai{};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	vk::raii::DeviceMemory mem(fx.device(), ai);
	buf.bindMemory(mem, 0);
	void* p = mem.mapMemory(0, req.size);
	std::memcpy(p, rows.data(), bytes);
	return ArgsBuffer{.buffer = std::move(buf), .memory = std::move(mem)};
}

/** binding 0 = args SSBO（🔴 stage = **TASK**——本探针 args 只有 task 读，mesh 完全不碰）；
 *  push 4B = rowBase（同样只进 task）。 */
struct LayoutParts {
	vk::raii::DescriptorSetLayout dsLayout;
	vk::raii::DescriptorPool pool;
	std::vector<vk::raii::DescriptorSet> sets;
	vk::raii::PipelineLayout pipeLayout;
};

LayoutParts makeLayout(const Fixture& fx, const ArgsBuffer& args, size_t argsBytes) {
	vk::DescriptorSetLayoutBinding lb{};
	lb.binding = 0;
	lb.stageFlags = vk::ShaderStageFlagBits::eTaskEXT;
	lb.descriptorCount = 1;
	lb.descriptorType = vk::DescriptorType::eStorageBuffer;
	vk::DescriptorSetLayoutCreateInfo li{};
	li.setBindings(lb);
	vk::raii::DescriptorSetLayout dsLayout(fx.device(), li);

	vk::PushConstantRange pc{};
	pc.stageFlags = vk::ShaderStageFlagBits::eTaskEXT;
	pc.offset = 0;
	pc.size = 4;
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
	vk::WriteDescriptorSet w{};
	w.dstSet = *sets[0];
	w.dstBinding = 0;
	w.dstArrayElement = 0;
	w.descriptorCount = 1;
	w.descriptorType = vk::DescriptorType::eStorageBuffer;
	w.pBufferInfo = &bi;
	fx.device().updateDescriptorSets(w, {});
	return LayoutParts{.dsLayout = std::move(dsLayout), .pool = std::move(pool),
		.sets = std::move(sets), .pipeLayout = std::move(pipeLayout)};
}

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

void beginRender(vk::raii::CommandBuffer& cmd, const Target& t) {
	barrier(cmd, t.img.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput, vk::AccessFlagBits2::eColorAttachmentWrite);
	vk::RenderingAttachmentInfo att{};
	att.imageView = *t.view;
	att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	att.loadOp = vk::AttachmentLoadOp::eClear;
	att.storeOp = vk::AttachmentStoreOp::eStore;
	att.clearValue = vk::ClearColorValue(std::array{kClearSentinel, kClearSentinel, kClearSentinel, kClearSentinel});
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

vk::PipelineRasterizationStateCreateInfo rasterDefaults() {
	vk::PipelineRasterizationStateCreateInfo rs{};
	rs.setDepthClampEnable(false);
	rs.setRasterizerDiscardEnable(false);
	rs.setPolygonMode(vk::PolygonMode::eFill);
	rs.setCullMode(vk::CullModeFlagBits::eNone);
	rs.setFrontFace(vk::FrontFace::eCounterClockwise);
	rs.setDepthBiasEnable(false);
	rs.setLineWidth(1.0f);
	return rs;
}

/** 管线公共段。🔴 stages 传**引用**（span 指回调用方数组）：数组生命周期若止于 lambda，
 *  vk::raii::Pipeline 构造读到的是悬空 pStages（首版用 FillStages&& 回调写进 pci，
 *  lambda 返回即析构 → createPipeline 段错误）。 */
vk::raii::Pipeline makeProbePipeline(const Fixture& fx, const vk::raii::PipelineLayout& layout,
	std::span<const vk::PipelineShaderStageCreateInfo> stages) {
	const auto rs = rasterDefaults();
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
	pci.layout = *layout;
	pci.pViewportState = &vs;
	pci.pRasterizationState = &rs;
	pci.pMultisampleState = &ms;
	pci.pColorBlendState = &cb;
	pci.pNext = &ri;
	pci.setStages(stages);
	// mesh/task 管线刻意不填 pVertexInputState / pInputAssemblyState（spec：有 mesh stage 时 ignored）
	return vk::raii::Pipeline(fx.device(), nullptr, pci);
}

/** task+mesh+frag 三槽（裸句柄入参——vk::ShaderModule 包装类不在 rlvk 导出面）。 */
std::array<vk::PipelineShaderStageCreateInfo, 3> makeTaskStages(VkShaderModule task,
	VkShaderModule mesh, VkShaderModule frag) {
	return {{
		{vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eTaskEXT, task, "main"},
		{vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eMeshEXT, mesh, "main"},
		{vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eFragment, frag, "main"}}};
}

/** mesh+frag 两槽（守卫管线：无 task stage 的经典 mesh-only 形态）。 */
std::array<vk::PipelineShaderStageCreateInfo, 2> makeGuardStages(VkShaderModule mesh,
	VkShaderModule frag) {
	return {{
		{vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eMeshEXT, mesh, "main"},
		{vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eFragment, frag, "main"}}};
}

/** task-MDI 录制口（参数序封死的同款纪律：vulkan_core 的裸签名是 offset, drawCount, stride）。 */
void drawMdi(vk::raii::CommandBuffer& cmd, const vk::raii::Buffer& args, uint32_t drawCount) {
	cmd.drawMeshTasksIndirectEXT(*args, /*offset=*/0, /*drawCount=*/drawCount, /*stride=*/kRowBytes);
}

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

uint32_t colValue(const std::vector<uint32_t>& px, uint32_t col) { return px[col]; }

std::string dumpCols(const std::vector<uint32_t>& px) {
	std::string s = "cols ";
	for (uint32_t c = 0; c < kCols; ++c)
		s += std::format("{}:{} ", c, colValue(px, c));
	return s;
}

std::array<ArgsRow, kMaxDraws> defaultRows() {
	std::array<ArgsRow, kMaxDraws> r{};
	for (uint32_t i = 0; i < kMaxDraws; ++i)
		r[i] = ArgsRow{kColsPerRow, 1u, 1u, kTagBase + kTagStep * i};
	return r;
}

/** task 管线一套资源（task + mesh + frag + 共用 guard frag）。 */
struct TaskRig {
	explicit TaskRig(const Fixture& fx)
		: rows(defaultRows()),
		  args(makeArgs(fx, rows)),
		  lp(makeLayout(fx, args, rows.size() * sizeof(ArgsRow))),
		  taskMod(makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_task_probe.task.spv")),
		  meshMod(makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_task_probe.mesh.spv")),
		  fragMod(makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv")),
		  pipe(makeProbePipeline(fx, lp.pipeLayout,
			  std::span<const vk::PipelineShaderStageCreateInfo>(makeTaskStages(
				  *taskMod, *meshMod, *fragMod)))),
		  target(makeTarget(fx)) {}

	std::array<ArgsRow, kMaxDraws> rows;
	ArgsBuffer args;
	LayoutParts lp;
	vk::raii::ShaderModule taskMod;
	vk::raii::ShaderModule meshMod;
	vk::raii::ShaderModule fragMod;
	vk::raii::Pipeline pipe;
	Target target;
};

}  // namespace

}  // namespace RGL::testing::mditask

// ═══════════════════════════════════════════════════════════════════════════
// 用例
// ═══════════════════════════════════════════════════════════════════════════

TEST_SUITE("gpu") {

using RGL::testing::mditask::ArgsRow;
using RGL::testing::mditask::colValue;
using RGL::testing::mditask::dumpCols;
using RGL::testing::mditask::drawMdi;
using RGL::testing::mditask::kClearSentinel;
using RGL::testing::mditask::kCols;
using RGL::testing::mditask::kColsPerRow;
using RGL::testing::mditask::kMaxDraws;
using RGL::testing::mditask::kGuardCol;
using RGL::testing::mditask::kGuardValue;
using RGL::testing::mditask::kTagBase;
using RGL::testing::mditask::kTagStep;
using RGL::testing::mditask::makeArgs;
using RGL::testing::mditask::makeLayout;
using RGL::testing::mditask::makeGuardStages;
using RGL::testing::mditask::makeModule;
using RGL::testing::mditask::makeProbePipeline;
using RGL::testing::mditask::makeTarget;
using RGL::testing::mditask::makeTaskStages;
using RGL::testing::mditask::renderAndRead;
using RGL::testing::mditask::TaskRig;
using RGL::testing::gpu::Fixture;

TEST_CASE("task_probe_control（harness 阳性锚点）：drawCount=1 × x=3，payload 通路把 tag 送进像素") {
	const Fixture fx;
	// 这条**不测 DrawIndex**（drawCount=1 时恒 0 无信息）。它测 task→payload→mesh 的整条
	// 数据通路 + 列映射 + 整数回读——后面三条的"绿"必须建立在这条绿之上（同 mdi 探针 control 纪律）。
	const TaskRig rig(fx);
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, rig.target, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *rig.pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *rig.lp.pipeLayout, 0, *rig.lp.sets[0], {});
		cmd.pushConstants(*rig.lp.pipeLayout, vk::ShaderStageFlagBits::eTaskEXT, 0, 4u, &rowBase);
		drawMdi(cmd, rig.args.buffer, 1u);
	});
	INFO(dumpCols(px));
	for (uint32_t i = 0; i < kColsPerRow; ++i)
		CHECK(colValue(px, i) == kTagBase + i);       // 行 0 的 tag 段经 payload 到达
	for (uint32_t c = kColsPerRow; c < kCols; ++c)
		CHECK(colValue(px, c) == kClearSentinel);      // 负向锚点：其余列没出现
}

TEST_CASE("task_probe_drawindex_increments（核心门）：drawCount=4，每个 draw 的 **task 侧** DrawIndex 各自正确") {
	const Fixture fx;
	// 被测点 = 本探针的存在理由：DrawIndex 在 **task 入口**逐 draw 递增（§19.9 选项② 的语义前提）。
	// 坏法可分辨：恒 0 → col3..11 保持哨兵；tag 段错 → 列对值错（与列错位不同签名）。
	const TaskRig rig(fx);
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, rig.target, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *rig.pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *rig.lp.pipeLayout, 0, *rig.lp.sets[0], {});
		cmd.pushConstants(*rig.lp.pipeLayout, vk::ShaderStageFlagBits::eTaskEXT, 0, 4u, &rowBase);
		drawMdi(cmd, rig.args.buffer, 4u);
	});
	INFO(dumpCols(px));
	for (uint32_t r = 0; r < 4u; ++r) {
		for (uint32_t i = 0; i < kColsPerRow; ++i) {
			INFO(std::format("row {} col {}", r, r * kColsPerRow + i));
			CHECK(colValue(px, r * kColsPerRow + i) == kTagBase + kTagStep * r + i);
		}
	}
}

TEST_CASE("task_probe_zero_taskcount_noop：task 侧 x=0 行空转且不跳号（Phase C 孔洞流的 task 形态前提）") {
	const Fixture fx;
	// mesh 路已钉过不跳号（mdi_zero_groupcount_noop）；**task 计数语义是否同样**没人测过，
	// 而重做批的孔洞流要把 x=0 行放进 task-MDI（行 x = task 数）。跳号 → col6..11 整体错位 → 红。
	auto rows = std::array<ArgsRow, kMaxDraws>{};
	for (uint32_t i = 0; i < kMaxDraws; ++i)
		rows[i] = ArgsRow{kColsPerRow, 1u, 1u, kTagBase + kTagStep * i};
	rows[1] = ArgsRow{0u, 0u, 0u, kTagBase + kTagStep * 1u};
	const auto args = makeArgs(fx, rows);
	const auto lp = makeLayout(fx, args, rows.size() * sizeof(ArgsRow));
	const auto t = makeTarget(fx);
	const auto taskMod = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_task_probe.task.spv");
	const auto meshMod = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_task_probe.mesh.spv");
	const auto fragMod = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");
	const auto stages = makeTaskStages(*taskMod, *meshMod, *fragMod);
	const auto pipe = makeProbePipeline(fx, lp.pipeLayout, std::span<const vk::PipelineShaderStageCreateInfo>(stages));
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, t, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *lp.pipeLayout, 0, *lp.sets[0], {});
		cmd.pushConstants(*lp.pipeLayout, vk::ShaderStageFlagBits::eTaskEXT, 0, 4u, &rowBase);
		drawMdi(cmd, args.buffer, 4u);
	});
	INFO(dumpCols(px));
	CHECK(colValue(px, 0) == kTagBase + 0u);  // 行 0 照画
	for (uint32_t c = 3; c < 6; ++c)
		CHECK(colValue(px, c) == kClearSentinel);  // 孔洞行 = 一列都不画
	for (uint32_t r = 2; r < 4u; ++r)
		for (uint32_t i = 0; i < kColsPerRow; ++i)
			CHECK(colValue(px, r * kColsPerRow + i) == kTagBase + kTagStep * r + i);  // 不跳号
}

TEST_CASE("task_probe_poison_guard：task-MDI 之后的直接 mesh draw **必须照常出像素**（STRIX 毒化检测器）") {
	const Fixture fx;
	// 8060S 的失效形态 = 同提交里 mesh 入口声明 DrawIndex 之后，后续所有直接 mesh draw
	// 不产出像素（bistro 整块消失，§19.9）。本条把同一探测器压进 task 形态：task 声明了
	// DrawIndex，守卫 draw 不声明、不读 payload、不读任何资源。
	//   · 数据列对 + 守卫列对 → task 路无毒化（§19.12 的"机制绿"）
	//   · 数据列对 + 守卫列 0 → **task 路也毒化** ⟹ §19.9 选项② 作废，只剩 issue/换宿主
	//   · 数据列错 → 先修用例（control 同红可归因），毒化判据不成立
	// 🔴 带去 STRIX 机重跑的就是这条（连同它的 control）。
	const TaskRig rig(fx);
	const auto guardMesh = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_task_probe_guard.mesh.spv");
	// 守卫管线：mesh+frag、无 task 入口、无 DrawIndex 声明（rgl/shader.cppm 的 NO_TASK 教训
	// 只关 shader object 路，经典管线无 task stage 本就合法，izb 探针同型在先）。
	const auto guardStages = makeGuardStages(*guardMesh, *rig.fragMod);
	const auto guardPipe = makeProbePipeline(fx, rig.lp.pipeLayout,
		std::span<const vk::PipelineShaderStageCreateInfo>(guardStages));
	const uint32_t rowBase = 0u;
	const auto px = renderAndRead(fx, rig.target, [&](vk::raii::CommandBuffer& cmd) {
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *rig.pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *rig.lp.pipeLayout, 0, *rig.lp.sets[0], {});
		cmd.pushConstants(*rig.lp.pipeLayout, vk::ShaderStageFlagBits::eTaskEXT, 0, 4u, &rowBase);
		drawMdi(cmd, rig.args.buffer, 4u);            // 先 task-MDI
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *guardPipe);
		cmd.drawMeshTasksEXT(1u, 1u, 1u);             // 后直接 mesh draw（守卫列 12）
	});
	INFO(dumpCols(px));
	for (uint32_t r = 0; r < 4u; ++r)
		for (uint32_t i = 0; i < kColsPerRow; ++i)
			CHECK(colValue(px, r * kColsPerRow + i) == kTagBase + kTagStep * r + i);  // task 路自己也对
	CHECK(colValue(px, kGuardCol) == kGuardValue);   // ← 毒化探测器本体
	for (uint32_t c = 13; c < kCols; ++c)
		CHECK(colValue(px, c) == kClearSentinel);
}

}  // TEST_SUITE("gpu")
