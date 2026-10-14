module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）+ VK_QUEUE_FAMILY_IGNORED
#include <vk_mem_alloc.h>   // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 的 C 面）
#include <glm/glm.hpp>
#include <fmt/format.h>
module izb_shadow:impl;

import izb_shadow;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import offgraph_sync;  // 批次 B1b：RgSyncEdgeId / emit（import 不传递，接口里的不算）
import compute_push_types_generated;  // IzbCreatePush / IzbPrimePush（反射产物，布局有出处）
import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import VmaRaii;
import shader;  // loadShaderSpv（izb_create/izb_prime.comp.slang）

namespace RGL {

namespace {

// 两个 push 布局（IzbCreatePush / IzbPrimePush）**不在本文件**——由 compute-push 反射管线生成
// （generated/compute_push_types_generated.cppm，import 见文件头）。生成物与 shader 同名，调用点
// 一个字符都不用改。
//
// 🔴 这里原来是两个手写镜像 struct + 一整套 `static_assert(sizeof/offsetof …)`。删它们的理由不是
// "清理冗余"，而是**那套 assert 是关于 C++ 侧的同义反复**：量的是手抄进 C++ 的 struct，不是
// shader。shader 改字段名/换序而 C++ 没跟着动时 assert 全过，C++ 按旧布局发字段而 shader 按新布局
// 读 —— 静默（IZB 只在 shadow.lua 的 izb 开门时跑，图像上表现为 mask 几何错位，离真因三跳）。
// 布局现在由反射派生，生成物自带 sizeof/offsetof assert；在这里再留一份手抄数值 = 造第二个真相源。
// 唯一一处**类型**变化：shader 的 `float4 invVPM[4]` 生成的是 `std::array<glm::vec4, 4>`（不是
// 手写的 `glm::mat4`）——两者内存同构（glm 列主序 mat4 = 4 个连续 float4 列），调用点逐列拷，
// 与 MeshletCullPush.planes（同为反射生成的 std::array）在 meshlet_cull_service_impl 里的写法同款。
// "期望值 = 去 shader 逐字段手算"这条独立判据挪进了 tests/test_compute_push_layout.cppm。

constexpr uint32_t kNone = 0xFFFFFFFFu;

/** 位图 popcount（mask dump 被遮像素数统计；Kernighan 循环免 <bit> 依赖面）。 */
size_t popcountWords(const uint32_t* words, size_t count) {
	size_t total = 0;
	for (size_t w = 0; w < count; ++w)
		for (uint32_t x = words[w]; x != 0; x &= x - 1)
			++total;
	return total;
}

/** 单 cell 链游走：返回 {长度, 完好}（步数上限 cap = 像素数，触顶 = 坏链自证）。 */
std::pair<size_t, bool> walkChainAt(const uint32_t* nodeData, uint32_t head, size_t cap) {
	size_t len = 0;
	uint32_t h = head;
	while (h != kNone && len <= cap) {
		++len;
		h = nodeData[h * 4 + 3];
	}
	return {len, h == kNone};
}

// 单 buffer 的 sync2 barrier 曾住在这里（`bufferBarrier`，13 个调用点各写六个 mask 字面量）。
// 批次 B1b 第二刀把它删了：mask 的**唯一来源**改成 `offgraph_sync` 的 usage 词表，发射点只写
// 边名 + 句柄（`sync.emit(cmd, edge, buffer)`），帧末按边对账实发次数。
// ⚠ 别再在这里加回一个本地 barrier helper —— 那等于把"谁都能写 mask 字面量"这件事重新发明，
// 而对账会立刻看不见它（它不在边表里）。形态与理由 → `docs/compute_调研.md` §7.2 / §7.4。

/**
 * 深度图（eDepth aspect 单 level/layer）布局转换 barrier（sandwich 进/出两条共用）。
 *
 * **返回 `to`**（批 14.4b A 档护栏的机关所在）：调用方把它接进一条 `layout = barrier(...)`
 * 链，`recordFrame` 末尾返回的就是"最后一条深度 barrier 实际写成的布局"。
 * 若返回 void、调用方另写一个局部量记布局，那个量与 barrier 的 `to` 参数**是两个独立事实**
 * ——删掉还原 barrier 而忘了改局部量，护栏就永远为真（这正是"把约定变断言"最容易做成的
 * 假护栏）。让**发命令的那一处**顺带产出断言的输入，两者不可能各说各话。
 */
vk::ImageLayout depthLayoutBarrier(vk::raii::CommandBuffer& cmd, vk::Image image,
	vk::ImageLayout from, vk::ImageLayout to,
	vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
	vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
	vk::ImageMemoryBarrier2 b{};
	b.srcStageMask = srcStage;
	b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage;
	b.dstAccessMask = dstAccess;
	b.oldLayout = from;
	b.newLayout = to;
	b.image = image;
	b.subresourceRange = {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1};
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	vk::DependencyInfo dep{};
	dep.setImageMemoryBarriers(b);
	cmd.pipelineBarrier2(dep);
	return to;
}

}  // namespace

IzbShadow::IzbShadow(RGL::DeviceContext& vkDevice, RGL::Resources& vkResources,
	vk::Extent2D extent_, uint32_t gridDim_, OffGraphSync& sync_)
	: deviceCtx(&vkDevice), resources(&vkResources), sync(sync_), extent(extent_),
	  gridDim(gridDim_) {
	auto& dev = vkDevice.getDevice();
	const uint32_t pixels = extent.width * extent.height;
	const auto headBytes = static_cast<vk::DeviceSize>(gridDim) * gridDim * 4;
	const auto nodeBytes = static_cast<vk::DeviceSize>(pixels) * 16;

	const auto makeDevBuf = [&](vk::DeviceSize bytes, vk::BufferUsageFlags extra,
	                             const char* name) -> RGL::VmaBuffer {
		// name 既进错误消息也进债-31 归因台账（台账存指针不拷贝 ⟹ 调用方必须传字面量，
		// 本文件四处均是）
		vk::BufferCreateInfo bi{};
		bi.size = bytes;
		bi.usage = vk::BufferUsageFlagBits::eStorageBuffer
			| vk::BufferUsageFlagBits::eShaderDeviceAddress | extra;
		bi.sharingMode = vk::SharingMode::eExclusive;
		VmaAllocationCreateInfo ai{};
		ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// createBufferWithAddress（gpu_cull 同款）：普通 createBuffer 不查询 BDA，
		// getDeviceAddress() 恒 0（首跑 VVL 实录：写描述符 address is zero）
		auto r = resources->getAllocator().createBufferWithAddress(dev, bi, ai, nullptr, name);
		if (!r)
			throw std::runtime_error(fmt::format("izb: {} buffer: {}", name, r.error().describe()));
		return std::move(*r);
	};
	// head：eTransferSrc = dump 拷贝源；eTransferDst = 每帧 vkCmdFillBuffer 清哨兵。
	// nodes 也要 eTransferSrc（dump 帧同为拷贝源——首跑 VVL 实录纠正的注释错误）
	headBuffer = makeDevBuf(headBytes,
		vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, "izb.head");
	nodesBuffer = makeDevBuf(nodeBytes, vk::BufferUsageFlagBits::eTransferSrc, "izb.nodes");
	primedBuffer = makeDevBuf(headBytes, {}, "izb.primed");
	// mask 位图：⌈pixels/32⌉ word（FillBuffer 要求 4B 对齐天然满足）
	const auto maskBytes = (static_cast<vk::DeviceSize>(pixels) + 31) / 32 * 4;
	maskBuffer = makeDevBuf(maskBytes,
		vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
		"izb.mask");

	// head+nodes+mask 联合回读 ring（直方图要游走链 + 统计遮挡位）
	const auto dumpBytes = headBytes + nodeBytes + maskBytes;
	for (uint32_t s = 0; s < 2; ++s) {
		vk::BufferCreateInfo si{};
		si.size = dumpBytes;
		si.usage = vk::BufferUsageFlagBits::eTransferDst;
		si.sharingMode = vk::SharingMode::eExclusive;
		VmaAllocationCreateInfo sa{};
		sa.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
		sa.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VmaAllocationInfo sad{};
		auto r = resources->getAllocator().createBuffer(si, sa, &sad, "izb/readback_ring");
		if (!r)
			throw std::runtime_error(fmt::format("izb: staging: {}", r.error().describe()));
		staging[s] = std::move(*r);
		stagingMapped[s] = sad.pMappedData;
	}

	// IZB 四描述符槽（Resources 发放终身稳定，管线条目 bake 依赖；mask 的 fallback
	// 描述符由 Resources 发号时写零 buffer——light.frag binding 14 **无条件绑定**，
	// 门关读 fallback）；描述符内容随本实例写
	{
		auto slots = resources->ensureIzbSlots();
		if (!slots)
			throw std::runtime_error(fmt::format("izb: slots: {}", slots.error().describe()));
		headSlot = slots->head;
		nodesSlot = slots->nodes;
		primedSlot = slots->primed;
		maskSlot = slots->mask;
	}
	// 深度采样描述符与 SSAO 共用（同 view 同布局；ensure 幂等，先到先写）
	{
		auto depth = resources->ensureDepthSampleSlot();
		if (!depth)
			throw std::runtime_error(depth.error().describe());
		depthSlot = *depth;
	}

	// 描述符写入（eStorageBuffer + BDA 全范围——gpu_cull::writeHeapDescriptors 同款）
	const auto writeDesc = [&](const RGL::VmaBuffer& buf, vk::DeviceSize size, uint32_t slot) {
		vk::DeviceAddressRangeEXT range{buf.getDeviceAddress(), size};
		vk::ResourceDescriptorInfoEXT desc{};
		desc.type = vk::DescriptorType::eStorageBuffer;
		desc.data.pAddressRange = &range;
		auto addr = resources->getDescriptorHeap().getSlotCpuAddress(slot);
		if (!addr)
			throw std::runtime_error(addr.error().describe());
		vk::HostAddressRangeEXT dst{*addr, resources->getDescriptorHeap().getDescriptorSize()};
		dev.writeResourceDescriptorsEXT(
			std::array<vk::ResourceDescriptorInfoEXT, 1>{desc},
			std::array<vk::HostAddressRangeEXT, 1>{dst});
	};
	writeDesc(headBuffer, headBytes, headSlot);
	writeDesc(nodesBuffer, nodeBytes, nodesSlot);
	writeDesc(primedBuffer, headBytes, primedSlot);
	writeDesc(maskBuffer, maskBytes, maskSlot);

	const uint32_t descriptorSize = resources->getDescriptorHeap().getDescriptorSize();
	createShader.emplace(makeMappedCompute(dev, "generated_shaders/izb_create.comp.spv",
		std::vector<std::pair<uint32_t, RGL::HeapSlot>>{
			{0, HeapSlot{headSlot}}, {1, HeapSlot{nodesSlot}}, {2, HeapSlot{depthSlot}}}, descriptorSize));
	primeShader.emplace(makeMappedCompute(dev, "generated_shaders/izb_prime.comp.spv",
		std::vector<std::pair<uint32_t, RGL::HeapSlot>>{
			{0, HeapSlot{headSlot}}, {1, HeapSlot{nodesSlot}}, {3, HeapSlot{primedSlot}}}, descriptorSize));

	if (auto* lg = RLLogger::getInstance())
		lg->info("izb: 开门 grid={} extent={}x{} head={}B nodes={}B（cascade-0 近场 v1，"
			"light pass 消费 IZB_CONSUME 变体）",
			gridDim, extent.width, extent.height, headBytes, nodeBytes);
}

vk::ImageLayout IzbShadow::recordFrame(vk::raii::CommandBuffer& cmd,
	const IzbFrameInput& input, vk::ImageLayout depthCurrentLayout, bool dumpHead) {
	const auto depthImage = *resources->getDepthImage();
	// 布局链（批 14.4b）：`depth` 恒等于"深度图此刻真实所处的布局"，每一步都由**实际发出的
	// barrier** 的返回值推进。调用方拿返回值与追踪表比对 ⟹ 漂移立刻出声。
	// ⚠ 本护栏与 VVL **不是互斥而是分工**（14.4b 实测修正，别照抄旧理由）：删掉还原 barrier
	//   的变异里两者**都**响——VVL 报在"执行器下一次 barrier"那个**无辜调用点**（要人反推
	//   是谁没还原），本护栏报在**肇事模块**并直接给出进入/离开布局；且 VVL 只在调试层在场
	//   时存在。详见 docs/phase14_plan.md §1.11。
	vk::ImageLayout depth = depthCurrentLayout;

	// ① 深度 layout → SHADER_READ_ONLY（生产者 = 此前全部图形深度写/shader 读）
	depth = depthLayoutBarrier(cmd, depthImage, depth,
		vk::ImageLayout::eShaderReadOnlyOptimal,
		vk::PipelineStageFlagBits2::eAllGraphics,
		vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);

	// ② 清 head（NONE 哨兵 = 「地址即像素 ID」布局的帧初始化；节点数组无需清：
	// 旧节点从新 head 不可达）。FillBuffer 要 eTransferDst（VVL 实录）
	// ⚠ 清零**前面**这条边（`izb_head_prior_to_clear`）是跨帧依赖（债-30 方案 C 暴露，2026-10-09）：
	//   head 是**单份**跨 frames-in-flight 共享的 buffer，本帧的 FillBuffer 写与上一帧对它的
	//   compute 写 / mask pass frag 读 / dump 拷读之间必须有声明过的执行依赖。此前"没写也
	//   没事"是因为 cull 每帧同步等待把帧序列整个串行化了（等 = 等上一帧整帧排空，正是
	//   债-30 那笔账）；串行一去掉，syncval 立刻报 WRITE_AFTER_WRITE（实测 cpu / gpu_lagged
	//   两档各 8 条，gpu_sync 档 0 条 = 旧串行还在时看不见）。src 三档全给（宽档
	//   `priorAccessWideCs` 的成因）：head 上一帧的最后访问点取决于 dump 门开没开（Transfer 读
	//   只在 dumpHead 时发生），拿不准的一律纳入源作用域，代价 = 每帧一条 barrier，
	//   换 VVL 沉默 + 不靠"上一帧恰好排空了"。同一条依赖在 mask 清零处本来就有（下面 ⑧），
	//   head 是漏的那个。
	sync.emit(cmd, RgSyncEdgeId::izbHeadPriorToClear, *headBuffer);
	::vkCmdFillBuffer(*cmd, *headBuffer, 0, VK_WHOLE_SIZE, static_cast<uint32_t>(kNone));
	sync.emit(cmd, RgSyncEdgeId::izbHeadClearToCreate, *headBuffer);

	// ③④⑤ create →（链 barrier）→ prime
	recordCreateDispatch(cmd, input);
	sync.emit(cmd, RgSyncEdgeId::izbHeadCreateToPrime, *headBuffer);
	sync.emit(cmd, RgSyncEdgeId::izbNodesCreateToPrime, *nodesBuffer);
	recordPrimeDispatch(cmd);

	// ⑥ 深度 layout 还原（进出同值 = 渲染图布局追踪表零漂移；返回值即护栏的断言输入）
	depth = depthLayoutBarrier(cmd, depthImage, vk::ImageLayout::eShaderReadOnlyOptimal,
		depthCurrentLayout,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eAllGraphics,
		vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eShaderRead);

	// ⑦ dump（可选）+ ⑧ mask 清零 + 帧奇偶推进
	if (dumpHead)
		recordDumpCopy(cmd);
	// ⑧ mask **每帧无条件**清零（债-32）。清零属于每帧编舞，**不属于 mask pass**：mask pass
	//   是可以被整段跳过的（shadow0 段为空 = shadow_pass 关或无投影体，见
	//   `Renderer::replayIzbMaskSegment` 的早退），而 light.frag 的 `izbShadowFactor` 只看
	//   izb flag（= 本对象已构造，**不判 shadow_pass**）照常被消费。清零原先写在
	//   `beginMaskPass` 之内 ⟹ 段空的帧 mask 一帧都不被写 = 着色器读未初始化显存（真 UB；
	//   本机画面与 shadow-off 逐位相同纯属"新页读零 + bit=0 语义恰为无遮挡"的侥幸，换驱动
	//   或分配器复用脏页即翻车）。搬到本处后「flag=1 ⟹ mask 本帧被写过」由**结构**成立：
	//   本函数是 izbShadow 非空时每帧的唯一入口（`izbLightPassPre` 逐 pass 无条件调用），
	//   调用点再怎么早退都跳不掉它——不靠"记得走 mask pass"这种约定。
	//   ⚠ 位置红线：必须在 ⑦ 之后。dump 拷的是**上一帧**的 mask（`imgqa izb` 判据读它），
	//   清在前面会让 shadowed 恒 0。
	//   barrier：src 含 Transfer 读 = dump 帧的 WAR（上一行刚把 mask 拷进 staging）；
	//   dst 同时给 ShaderWrite|ShaderRead = 段空时**没有** endMaskPass 那条写→读 barrier，
	//   light.frag 的 Load 读得由这里排序（段不空时 endMaskPass 仍在 mask pass 后补一条）。
	sync.emit(cmd, RgSyncEdgeId::izbMaskPriorToClear, *maskBuffer);
	::vkCmdFillBuffer(*cmd, *maskBuffer, 0, VK_WHOLE_SIZE, 0u);
	sync.emit(cmd, RgSyncEdgeId::izbMaskClearToPass, *maskBuffer);
	parity ^= 1u;
	return depth;
}

void IzbShadow::recordCreateDispatch(vk::raii::CommandBuffer& cmd, const IzbFrameInput& input) {
	// heap 已由 ⑦ 绑定；bindShadersEXT 与 graphics pipeline 绑定互斥——light pass 的
	// draw 侧 bindShaders 在其后（drawOne 逐 draw 绑），天然安全
	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> handles = {*createShader};
	cmd.bindShadersEXT(stages, handles);
	IzbCreatePush push{};
	// shader 侧是 `float4 invVPM[4]` / `float4 lightVP[4]` ⟹ 反射生成物的字段类型是
	// std::array<glm::vec4, 4>（不是原先手写的 glm::mat4）。glm 列主序 mat4 的内存就是 4 个连续
	// float4 列，逐列赋值即可，与 MeshletCullPush.planes（同为反射生成的 std::array）在
	// meshlet_cull_service_impl 里的写法同款。🔴 不用 reinterpret_cast：那会把"两侧内存同构"
	// 从编译器能查的逐列赋值降级成一句谁都没法验的断言。
	for (int col = 0; col < 4; ++col) {
		push.invVPM[col] = input.invViewProj[col];
		push.lightVP[col] = input.lightViewProj[col];
	}
	push.pixelW = static_cast<int32_t>(extent.width);
	push.pixelH = static_cast<int32_t>(extent.height);
	push.gridDim = static_cast<int32_t>(gridDim);
	push.emptyDepth = 1.0f;  // 深度 ≥ 此值 = far 面 miss，不建节点
	cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
	cmd.dispatch((extent.width * extent.height + 63u) / 64u, 1, 1);
}

void IzbShadow::recordPrimeDispatch(vk::raii::CommandBuffer& cmd) {
	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> handles = {*primeShader};
	cmd.bindShadersEXT(stages, handles);
	// 指定初始化器 = 同上（位置聚合初始化与字段名无关；C++20 要求按声明序 ⟹ 换序也红）。
	// 两个 padding 字段照旧显式写 0：它们也是反射出来的契约面（shader 里加/删字段就会挪位），
	// 按名写出来才在"改 shader"时出声。
	const IzbPrimePush push{.gridDim = static_cast<int32_t>(gridDim),
	    .pixelTotal = static_cast<int32_t>(extent.width * extent.height), ._pad0 = 0, ._pad1 = 0};
	cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
	cmd.dispatch((gridDim * gridDim + 63u) / 64u, 1, 1);
}

void IzbShadow::recordDumpCopy(vk::raii::CommandBuffer& cmd) {
	const uint32_t slot = parity;
	const auto headB = static_cast<VkDeviceSize>(gridDim) * gridDim * 4;
	const auto nodeB = static_cast<VkDeviceSize>(extent.width) * extent.height * 16;
	sync.emit(cmd, RgSyncEdgeId::izbDumpHead, *headBuffer);
	sync.emit(cmd, RgSyncEdgeId::izbDumpNodes, *nodesBuffer);
	sync.emit(cmd, RgSyncEdgeId::izbDumpMask, *maskBuffer);
	// head → staging[0..headB)，nodes → staging[headB..)，mask → 尾段
	// （VkBufferCopy 的 size 必须显式——VK_WHOLE_SIZE 此字段非法，VVL 实录）
	const auto maskB = (static_cast<vk::DeviceSize>(extent.width) * extent.height + 31) / 32 * 4;
	VkBufferCopy c0{0, 0, headB};
	::vkCmdCopyBuffer(*cmd, *headBuffer, *staging[slot], 1, &c0);
	VkBufferCopy c1{0, headB, nodeB};
	::vkCmdCopyBuffer(*cmd, *nodesBuffer, *staging[slot], 1, &c1);
	VkBufferCopy c2{0, headB + nodeB, maskB};
	::vkCmdCopyBuffer(*cmd, *maskBuffer, *staging[slot], 1, &c2);
	sync.emit(cmd, RgSyncEdgeId::izbDumpStagingToHost, *staging[slot]);
	dumpPending[slot] = true;
}

void IzbShadow::beginMaskPass(vk::raii::CommandBuffer& cmd) {
	// head/nodes：compute 写（create）→ 本 pass frag 读，跨 pass 边界显式同步
	sync.emit(cmd, RgSyncEdgeId::izbHeadToMaskPass, *headBuffer);
	sync.emit(cmd, RgSyncEdgeId::izbNodesToMaskPass, *nodesBuffer);
	// ⚠ mask 的 FillBuffer 清零**不在这里**（债-32，2026-10-09 搬走）：清零若留在本函数，
	// 则"跳过 mask pass"与"跳过清零"是同一件事，而调用点确实会跳过本 pass（shadow0 段为空）。
	// 现在 mask 由 `recordFrame` 第 ⑧ 步每帧无条件清零，本函数只负责开 pass。
	// 零附件动态渲染（frag 只写 mask SSBO；viewport/scissor = G×G 由调用方
	// applyDynamicState 设——引擎负 viewport 口径自动生效，frag 侧 cell.y 翻转匹配）
	vk::RenderingInfo rendering{};
	rendering.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, vk::Extent2D{gridDim, gridDim}};
	rendering.layerCount = 1;
	cmd.beginRendering(rendering);
}

void IzbShadow::endMaskPass(vk::raii::CommandBuffer& cmd) {
	cmd.endRendering();  // 本版本 raii 无 DependencyInfo 形参重载（帧内无需跨 pass 同步依赖）
	// 批 11.3-1e：mask frag atomic 写 → light.frag Load 读——**两个 render pass 之间
	// 无隐式依赖**，必须显式同步（VVL 不报错 ≠ 数据可见：首挂实录 = 无此 barrier 时
	// light 读恒 0，画面与门关逐位一致）
	sync.emit(cmd, RgSyncEdgeId::izbMaskToLight, *maskBuffer);
}

void IzbShadow::drainReadback() {
	const uint32_t slot = parity ^ 1u;  // 上一帧 recordFrame 用的槽
	if (!dumpPending[slot])
		return;
	dumpPending[slot] = false;

	const size_t cells = static_cast<size_t>(gridDim) * gridDim;
	const size_t pixels = static_cast<size_t>(extent.width) * extent.height;
	const auto* headData = static_cast<const uint32_t*>(stagingMapped[slot]);
	const auto* nodeData = reinterpret_cast<const uint32_t*>(
		static_cast<const std::byte*>(stagingMapped[slot]) + cells * 4);

	size_t empty = 0, n1 = 0, n2 = 0, n3 = 0, nodes = 0, maxChain = 0;
	// mask 位图 popcount = 被遮像素数（阳性锚点——场景有阳关必有被遮像素）
	const auto* maskWords = reinterpret_cast<const uint32_t*>(
		static_cast<const std::byte*>(stagingMapped[slot]) + cells * 4 + pixels * 16);
	const size_t shadowed = popcountWords(maskWords, (pixels + 31) / 32);
	for (size_t c = 0; c < cells; ++c) {
		if (headData[c] == kNone) {
			++empty;
			continue;
		}
		const auto [len, ok] = walkChainAt(nodeData, headData[c], pixels);
		if (!ok) {
			if (auto* lg = RLLogger::getInstance())
				lg->error("izb: cell {} 链长触顶（坏链/环！）", c);
			continue;
		}
		nodes += len;
		maxChain = std::max(maxChain, len);
		if (len == 1)
			++n1;
		else if (len == 2)
			++n2;
		else
			++n3;
	}
	if (auto* lg = RLLogger::getInstance())
		lg->info("izb: dump cells={} empty={} n1={} n2={} n3+={} maxChain={} nodes={} shadowed={}",
			cells, empty, n1, n2, n3, maxChain, nodes, shadowed);
	dumpMaskPpm(maskWords, static_cast<uint32_t>(extent.width), static_cast<uint32_t>(extent.height));
}

// 批 11.3-1e 判据工具：mask 位图导出 PGM（w×h 0/255，行 = fb 行）——与 llm_debug
// 截图目检对位（阴影带结构应落在遮挡体光源侧地面）。固定覆盖写，dump 帧后有效。
void IzbShadow::dumpMaskPpm(const uint32_t* words, uint32_t w, uint32_t h) {
	// C stdio：模块环境里 ofstream << 的输出被污染成指针序列（首挂实录，原因未追——
	// 判据工具走最朴素的 stdio 保证可读性）
	auto* f = std::fopen("izb_mask.ppm", "wb");
	if (!f)
		return;
	const auto hdr = "P6\n" + std::to_string(w) + " " + std::to_string(h) + "\n255\n";
	std::fwrite(hdr.data(), 1, hdr.size(), f);
	std::vector<uint8_t> row(static_cast<size_t>(w) * 3);
	for (uint32_t y = 0; y < h; ++y) {
		for (uint32_t x = 0; x < w; ++x) {
			const uint32_t pid = y * w + x;
			const bool occl = (words[pid >> 5] >> (pid & 31)) & 1u;
			row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = occl ? 0 : 255;
		}
		std::fwrite(row.data(), 1, row.size(), f);
	}
	std::fclose(f);
}

}  // namespace RGL
