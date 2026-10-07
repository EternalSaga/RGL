module;
#include <vulkan/vulkan.h>  // VK_* 宏/C 函数（rlvk 不含宏）+ VK_QUEUE_FAMILY_IGNORED
#include <vk_mem_alloc.h>   // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 的 C 面）
#include <glm/glm.hpp>
#include <fmt/format.h>
module izb_shadow:impl;

import izb_shadow;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
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

/** IzbCreatePush —— 与 izb_create.comp.slang 同名 struct 逐字段一致（glm 列主序
 *  mat4 = 4×float4 连续 = shader 侧 float4[4]；总 144B ≤ maxPushDataSize）。 */
struct IzbCreatePush {
	glm::mat4 invVPM;
	glm::mat4 lightVP;
	int32_t pixelW;
	int32_t pixelH;
	int32_t gridDim;
	float emptyDepth;
};

/** IzbPrimePush —— 与 izb_prime.comp.slang 同名 struct 逐字段一致。 */
struct IzbPrimePush {
	int32_t gridDim;
	int32_t pixelTotal;
	int32_t pad0;
	int32_t pad1;
};

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

/** 单 buffer 的 sync2 barrier（录制 + 执行；本模块内多条同构 barrier 的公共体）。 */
void bufferBarrier(vk::raii::CommandBuffer& cmd, vk::Buffer buffer,
	vk::PipelineStageFlags2 srcStage, vk::AccessFlags2 srcAccess,
	vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
	vk::BufferMemoryBarrier2 b{};
	b.srcStageMask = srcStage;
	b.srcAccessMask = srcAccess;
	b.dstStageMask = dstStage;
	b.dstAccessMask = dstAccess;
	b.buffer = buffer;
	b.size = VK_WHOLE_SIZE;
	vk::DependencyInfo dep{};
	dep.setBufferMemoryBarriers(b);
	cmd.pipelineBarrier2(dep);
}

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

/**
 * heap 映射的 compute shader object（gpu_cull_impl::createCullShaders 同款模式）：
 * binding→heap 槽一对一；push constant 无需映射条目（heap 下 push data 区自动供给）。
 */
vk::raii::ShaderEXT makeMappedCompute(vk::raii::Device& dev, const char* spvPath,
	const std::vector<std::pair<uint32_t, uint32_t>>& bindingToSlot, uint32_t descriptorSize) {
	auto spvResult = loadShaderSpv(spvPath);
	if (!spvResult)
		throw std::runtime_error(spvResult.error().describe());
	const auto spv = std::move(*spvResult);

	std::vector<VkDescriptorSetAndBindingMappingEXT> mappings;
	mappings.reserve(bindingToSlot.size());
	for (const auto& [binding, slot] : bindingToSlot) {
		VkDescriptorSetAndBindingMappingEXT m{};
		m.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_AND_BINDING_MAPPING_EXT;
		m.descriptorSet = 0;
		m.firstBinding = binding;
		m.bindingCount = 1;
		m.resourceMask = VK_SPIRV_RESOURCE_TYPE_ALL_EXT;
		m.source = VK_DESCRIPTOR_MAPPING_SOURCE_HEAP_WITH_CONSTANT_OFFSET_EXT;
		m.sourceData.constantOffset.heapOffset = slot * descriptorSize;
		m.sourceData.constantOffset.heapArrayStride = descriptorSize;
		mappings.push_back(m);
	}
	VkShaderDescriptorSetAndBindingMappingInfoEXT mappingInfo{};
	mappingInfo.sType = VK_STRUCTURE_TYPE_SHADER_DESCRIPTOR_SET_AND_BINDING_MAPPING_INFO_EXT;
	mappingInfo.mappingCount = static_cast<uint32_t>(mappings.size());
	mappingInfo.pMappings = mappings.data();

	VkShaderCreateInfoEXT raw{};
	raw.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT;
	raw.pNext = &mappingInfo;
	raw.flags = VK_SHADER_CREATE_DESCRIPTOR_HEAP_BIT_EXT;
	raw.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	raw.codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT;
	raw.codeSize = spv.size() * sizeof(uint32_t);
	raw.pCode = spv.data();
	raw.pName = "main";
	vk::ShaderCreateInfoEXT createInfo(raw);
	auto shaders = dev.createShadersEXT(std::vector<vk::ShaderCreateInfoEXT>{createInfo});
	return std::move(shaders[0]);
}

}  // namespace

IzbShadow::IzbShadow(RGL::DeviceContext& vkDevice, RGL::Resources& vkResources,
	vk::Extent2D extent_, uint32_t gridDim_)
	: deviceCtx(&vkDevice), resources(&vkResources), extent(extent_), gridDim(gridDim_) {
	auto& dev = vkDevice.getDevice();
	const uint32_t pixels = extent.width * extent.height;
	const auto headBytes = static_cast<vk::DeviceSize>(gridDim) * gridDim * 4;
	const auto nodeBytes = static_cast<vk::DeviceSize>(pixels) * 16;

	const auto makeDevBuf = [&](vk::DeviceSize bytes, vk::BufferUsageFlags extra,
	                             const char* name) -> RGL::VmaBuffer {
		vk::BufferCreateInfo bi{};
		bi.size = bytes;
		bi.usage = vk::BufferUsageFlagBits::eStorageBuffer
			| vk::BufferUsageFlagBits::eShaderDeviceAddress | extra;
		bi.sharingMode = vk::SharingMode::eExclusive;
		VmaAllocationCreateInfo ai{};
		ai.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		// createBufferWithAddress（gpu_cull 同款）：普通 createBuffer 不查询 BDA，
		// getDeviceAddress() 恒 0（首跑 VVL 实录：写描述符 address is zero）
		auto r = resources->getAllocator().createBufferWithAddress(dev, bi, ai);
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
		auto r = resources->getAllocator().createBuffer(si, sa, &sad);
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
		{{0, headSlot}, {1, nodesSlot}, {2, depthSlot}}, descriptorSize));
	primeShader.emplace(makeMappedCompute(dev, "generated_shaders/izb_prime.comp.spv",
		{{0, headSlot}, {1, nodesSlot}, {3, primedSlot}}, descriptorSize));

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
	::vkCmdFillBuffer(*cmd, *headBuffer, 0, VK_WHOLE_SIZE, static_cast<uint32_t>(kNone));
	bufferBarrier(cmd, *headBuffer, vk::PipelineStageFlagBits2::eAllCommands,
		vk::AccessFlagBits2::eMemoryWrite,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite);

	// ③④⑤ create →（链 barrier）→ prime
	recordCreateDispatch(cmd, input);
	bufferBarrier(cmd, *headBuffer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
	bufferBarrier(cmd, *nodesBuffer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead);
	recordPrimeDispatch(cmd);

	// ⑥ 深度 layout 还原（进出同值 = 渲染图布局追踪表零漂移；返回值即护栏的断言输入）
	depth = depthLayoutBarrier(cmd, depthImage, vk::ImageLayout::eShaderReadOnlyOptimal,
		depthCurrentLayout,
		vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eAllGraphics,
		vk::AccessFlagBits2::eDepthStencilAttachmentWrite | vk::AccessFlagBits2::eShaderRead);

	// ⑦ dump（可选）+ 帧奇偶推进
	if (dumpHead)
		recordDumpCopy(cmd);
	parity ^= 1u;
	return depth;
}

void IzbShadow::recordCreateDispatch(vk::raii::CommandBuffer& cmd, const IzbFrameInput& input) {
	// heap 已由 ⑦ 绑定；bindShadersEXT 与 graphics pipeline 绑定互斥——light pass 的
	// draw 侧 bindShaders 在其后（drawOne 逐 draw 绑），天然安全
	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> handles = {*createShader};
	cmd.bindShadersEXT(stages, handles);
	const IzbCreatePush push{
		input.invViewProj, input.lightViewProj,
		static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height),
		static_cast<int32_t>(gridDim), 1.0f};
	cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
	cmd.dispatch((extent.width * extent.height + 63u) / 64u, 1, 1);
}

void IzbShadow::recordPrimeDispatch(vk::raii::CommandBuffer& cmd) {
	std::array<vk::ShaderStageFlagBits, 1> stages = {vk::ShaderStageFlagBits::eCompute};
	std::array<vk::ShaderEXT, 1> handles = {*primeShader};
	cmd.bindShadersEXT(stages, handles);
	const IzbPrimePush push{static_cast<int32_t>(gridDim),
		static_cast<int32_t>(extent.width * extent.height), 0, 0};
	cmd.pushDataEXT(vk::PushDataInfoEXT(0, vk::HostAddressRangeConstEXT(&push, sizeof(push))));
	cmd.dispatch((gridDim * gridDim + 63u) / 64u, 1, 1);
}

void IzbShadow::recordDumpCopy(vk::raii::CommandBuffer& cmd) {
	const uint32_t slot = parity;
	const auto headB = static_cast<VkDeviceSize>(gridDim) * gridDim * 4;
	const auto nodeB = static_cast<VkDeviceSize>(extent.width) * extent.height * 16;
	bufferBarrier(cmd, *headBuffer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
	bufferBarrier(cmd, *nodesBuffer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
	bufferBarrier(cmd, *maskBuffer, vk::PipelineStageFlagBits2::eAllGraphics,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
	// head → staging[0..headB)，nodes → staging[headB..)，mask → 尾段
	// （VkBufferCopy 的 size 必须显式——VK_WHOLE_SIZE 此字段非法，VVL 实录）
	const auto maskB = (static_cast<vk::DeviceSize>(extent.width) * extent.height + 31) / 32 * 4;
	VkBufferCopy c0{0, 0, headB};
	::vkCmdCopyBuffer(*cmd, *headBuffer, *staging[slot], 1, &c0);
	VkBufferCopy c1{0, headB, nodeB};
	::vkCmdCopyBuffer(*cmd, *nodesBuffer, *staging[slot], 1, &c1);
	VkBufferCopy c2{0, headB + nodeB, maskB};
	::vkCmdCopyBuffer(*cmd, *maskBuffer, *staging[slot], 1, &c2);
	bufferBarrier(cmd, *staging[slot], vk::PipelineStageFlagBits2::eTransfer,
		vk::AccessFlagBits2::eTransferWrite,
		vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
	dumpPending[slot] = true;
}

void IzbShadow::beginMaskPass(vk::raii::CommandBuffer& cmd) {
	// head/nodes：compute 写（create）→ 本 pass frag 读，跨 pass 边界显式同步
	bufferBarrier(cmd, *headBuffer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
	bufferBarrier(cmd, *nodesBuffer, vk::PipelineStageFlagBits2::eComputeShader,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
	// mask hazard 簿记：上次写 = 上一帧本 pass 的 frag atomic、上次读 = 上一帧
	// light.frag（同队列执行序天然有序，barrier 按 bookkeeping 补齐；src 含读 =
	// fill 覆写前的 WAR 同步）→ FillBuffer 清 0 → 本帧 frag atomic 写
	bufferBarrier(cmd, *maskBuffer, vk::PipelineStageFlagBits2::eAllGraphics,
		vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eShaderRead,
		vk::PipelineStageFlagBits2::eAllCommands, vk::AccessFlagBits2::eMemoryWrite);
	::vkCmdFillBuffer(*cmd, *maskBuffer, 0, VK_WHOLE_SIZE, 0u);
	bufferBarrier(cmd, *maskBuffer, vk::PipelineStageFlagBits2::eAllCommands,
		vk::AccessFlagBits2::eMemoryWrite,
		vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderWrite);
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
	bufferBarrier(cmd, *maskBuffer, vk::PipelineStageFlagBits2::eAllGraphics,
		vk::AccessFlagBits2::eShaderWrite,
		vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead);
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
