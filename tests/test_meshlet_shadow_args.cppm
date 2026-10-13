module;
/**
 * test_meshlet_shadow_args —— ★M-1a（docs/mdi_plan.md §19.5）真 MDI 阴影段行表 GPU 载体的
 * **设备侧往返**（`TEST_SUITE("gpu")`——本构建的 `TEST_CASE` 宏只接受一个参数，设备级靠套件分流）
 *
 * ## 测什么，以及为什么截图测不到它
 * `rgl/meshlet_shadow_args.cppm` 的环层（`createMeshletShadowArgsRing` / `location` / `argsBytes`
 * / `upload`）。★M-1a 是**零消费者**批 ⟹ 出厂截图逐位不变（判据 B2），于是「行落在哪一格」这件事
 * 没有任何像素判据：它红了也不会有人看见。只能在这里钉。
 *
 * ## 🔴 主判据 = 设备侧往返，映射回读只是辅助锚点
 * 本机 = AMD Radeon 8060S / RADV STRIX_HALO，**统一内存**：映射指针与设备侧同一页 ⟹「读映射
 * 指针证明上传对了」是**恒真判据**（★1-a0 记过，`test_meshlet_geometry` 文件头有全文）。
 * 于是本文件把行表 buffer 先 `vkCmdCopyBuffer` 进第二张 buffer（**GPU 读源**），再从第二张的
 * host 侧读。两点定位：映射红 = 上传本身写错；映射绿而设备红 = 回读 harness 坏。
 *
 * ## 🔴 还有一条截图永远测不到的判据：`argsOffset` 与 `rowBase` 必须同源
 * 这对数一旦分家，症状是 **GPU 画的是另一条合法命令**（不报错、不触发 VVL、位比也可能照过）。
 * 用例 4（`mdi_expansion`）用一条**自证回路**钉它：
 *   ① CPU 按 `upload(frameSlot=1, firstRow=3)` 写两行（每行四个字段两两不同）
 *   → ② 对同一个载体发**一次真 MDI**：`drawMeshTasksIndirectEXT(buffer, argsOffset, 2, 24)`
 *   → ③ 测试自带 SPV（`tests/shaders/mdi_shadow_rows.mesh.slang`）里
 *      `row = rowBase + SV_DrawIndex`，从同一张 buffer 读那一行的 word[3]/word[4]，拼成 tag
 *   → ④ R32_UINT 整数附件按列（列号 == 行号）回读。
 * 判据 = 「**按 rowBase 索引到的那一行，就是设备按 argsOffset 取到的那一行**」，且同一次调用里
 * 混一条 `gcX = 0` 的孔洞行与 slice 0 的空白对照。用例 5 是它的**阳性对照**（把行 10 的 `gcX`
 * 改成 0 ⟹ 那一列必须变哨兵，而另一列照旧）——没有这条，「什么都没画对但也没多出列」会读成绿。
 *
 * ## 前置一律 `REQUIRE`，不是 `CHECK`（★1-a0 教训的载体）
 * `CHECK` 完继续拿 `VK_NULL_HANDLE` / `nullptr` 去 `vkCmdCopyBuffer` = SIGSEGV **打断整个测试
 * 二进制**，其余用例静默不跑 = 假绿面被扩大。故设备/句柄真值/映射非空/容量对账/feature 位
 * 全部 `REQUIRE`。句柄真值判据 = `deviceAddress != 0`（真实分配的 BDA 恒非 0）+ `buffer != VK_NULL_HANDLE`。
 *
 * ## 无设备 = FAIL，不 SKIP
 * `Fixture` 构造即抛（缺设备 / 缺 VVL / 缺扩展），doctest 记失败——静默跳过 = 假绿。
 *
 * ## 数值纪律（🔴 两个数相等 ⟹ 互换测不出东西；spike §6.3 的 M4 教训同族）
 * `kMdiShadowRowStride` = 24。本文件的 sliceRows = 7、slices = 2、frameSlot = 1、firstRow = 3、
 * drawCount = 2、objectIndex = 37/41、clusterStart = 13/19、groupCountX = 1/2
 * ⟹ **没有一个等于 24**；`argsOffset`(240) ≠ `rowBase`(10) ≠ sliceRows(7) ≠ slices(2)；
 * 每行的四个字段两两不同 ⟹ 「读歪一格」「字段互换」「帧槽项被丢」三种坏形态各有专属信号。
 *
 * ## 变异红线声明（2026-10-13 全部实测；harness = `/tmp/opencode/m1a/mutate.py`，
 * 注入点 `rgl/meshlet_shadow_args_impl.cppm`；「红 N 条」= 失败的 TEST_CASE 数，本文件共 5 条。
 * 每条还原后按**进场快照**逐字节复核 + 重跑基线断言全绿；全批终态 `snapshot identical: True`）
 *  - **N5** memcpy 目的偏移少乘 stride（`mapped + loc.rowBase`）→ 红 **4** 条（主判据 + guards +
 *    自证回路 + 阳性对照；行落在别的字节上，映射侧与设备侧同时错）
 *  - **N6** 删掉 `frameSlot >= slices` 闸门（`false && …`，保留参数使用）→ 红 **1** 条（guards
 *    的「越界必须报错」半边）
 *  - **N7** 删掉 `endRow > sliceRows` 闸门 → 红 **1** 条（guards 的另一边）
 *  - **N8** 删掉 `mdiShadowRangeFits` buffer 边界判据 → **红 0 条 = 存活**（合法入参下**结构上
 *    到不了**：前两道闸门已拦掉所有会越界的形态，只有「容量公式与偏移公式分家」才现形
 *    ——见缺口④，不当判据用）
 *  - **N9** 建 buffer 时删掉整张清 0（换成 `if (capacity == 0)` 的恒假 memset）→ 红 **1** 条
 *    （主判据的「未写入区恒 0」半边）。🔴 前提是 `poisonPool` 先把池子填 0xCD——否则驱动恰好
 *    给 0 时该变异存活（★1-a0 的 M4 同款坑，本文件把它写成了防线）
 *  - **N10** `createMeshletShadowArgsRing` 的容量漏乘 `slices`（= 一个 slice）→ 红 **5** 条
 *    （载体账 336≠168 + 后面四条的拷贝/绘制全部越出 buffer 尾）
 *  - **N11** `sliceRows × slices ≤ UINT32_MAX` 闸门短路 → **红 0 条 = 存活**（现实尺寸造不出
 *    越界激励，见缺口⑤）
 *  - **N12** `argsBytes` 把跨度写成 `drawCount × stride` → 红 **1** 条（载体账：36 ≠ 48）。
 *    ⚠ 首版写成 `return drawCount * kMdiShadowRowStride;`（丢掉 `loc`）= **构建红**
 *    （`-Werror=unused-variable`），读数作废；改成 `+ 0u * loc.rowBase` 才是判据红
 *  - **N13** 两条闸门错误串文案并流（frame-slot 串里写进行数超界的关键词）→ 红 **1** 条
 *    （guards 的「两两不同 + 各自不含金关键词」半边）——可归因性本身是判据
 *  - 纯函数侧的六条（N1-N4 / N14-N15，同源 pair 与容量公式）**同时**打到本文件的 2-5 条用例，
 *    逐条读数记在 `tests/test_mdi_rows.cppm` 文件头（同一份 pair 是两侧共用的产地）
 *  - 顺带钉住的 harness 自身两条（不是被测码的判据，少了它们用例不可信）：
 *    ① `copyWholeRing` 前必须 `REQUIRE(mapped != nullptr)`；
 *    ② 「被拒的写不动一个字节」的快照对里**不许**夹任何合法的写（首版夹了一条合法贴尾写，
 *      于是红在 328 字节 = 那条合法写入的 `clusterStart`——判据自我矛盾，不是被测码坏）
 *
 * ## 已知覆盖缺口（写出来，别让「全绿」被读成「全验过」）
 *  ① **不 flush 测不出**：本机映射内存 host-coherent，`vmaFlushAllocation` 是 VMA 内部提前返回
 *     ⟹ 删掉它逐字节相同。防线只剩「照 `meshlet_geometry` 先例写 + 注释」。
 *     换非 coherent 的机器（无 large-BAR 独显）再补账（★M-2 三机那一档顺手重跑本文件）。
 *  ② `MeshletShadowArgsService`（领槽 + 写描述符）要 `Resources`，而 `Resources` 构造要
 *     Swapchain（= 拖进窗口/深度图/两个 heap）——`test_meshlet_geometry` 文件头明确反对这种做法。
 *     于是服务层由**启动 INFO 闭式锚 + 判据 B2 位比**兜（日志里那五个数由 `meshletShadowArgsAnchors`
 *     现算、不取载体成员；槽号由 `allocSlot` 记账）。「描述符 range == 整张容量」在设备侧**没有**
 *     被验过——★M-1b 烘 binding 22 之后，「shader 按描述符读到的行 == vkCmd 按 offset 读到的行」
 *     才第一次成为可测事实（那一步必须补，别把这批的绿当成它已被验过）。
 *  ③ 「真产物切片（3 个级联段 × 可切上界 1591）」不在这里做：本批零消费者，装配层不产候选序列。
 *  ④ N8 的 buffer 边界判据是**哨兵**（合法入参下不可达），不是可达分支——它防的是两处公式分家。
 *  ⑤ N11 的 32 位行号闸门同④（要触发得分配 100GB buffer，造不出激励）。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>    // VK_* 宏 / vkCmdCopyBuffer（rlvk 不含宏）
#include <vk_mem_alloc.h>     // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 只导出 C++ 包装）
#include <cstring>
#include <initializer_list>   // 🔴 modules 坑：range-for 的花括号列表要真 STL 头（只 include doctest 不够）
#include <fstream>            // SPV 读盘（照 test_mdi_drawindex 的量具口径）
#include <string>             // INFO 里的 std::string 要有 operator<<，否则失败信息变成字节数组

export module test_meshlet_shadow_args;

import rlstd;
import rlvk;
import vkerror;
import VmaRaii;
import meshlet_shadow_args;  // createMeshletShadowArgsRing / MeshletShadowArgsRing
import mdi_rows;             // MdiShadowRow / kMdiShadowRowStride / 尺寸算术（同源 pair 的产地）
import test_device_primitives;  // Fixture / Image2D / makeImage / readImage（引擎同款引导链）

namespace RGL::testing::shadowargs {

using testing::gpu::Fixture;
using testing::gpu::Image2D;

namespace {

// ─── 数值纪律：没有一个数等于 stride(=24)，且每行四个字段两两不同 ─────────────────
constexpr uint32_t kCaseSliceRows = 7u;   // 每个帧槽的行数
constexpr uint32_t kCaseSlices = 2u;      // 帧槽数（= kFramesInFlight 的形态）
constexpr uint32_t kCaseFrameSlot = 1u;   // 本用例写的帧槽（非 0 ⟹ 「帧槽项被丢」立刻可辨）
constexpr uint32_t kCaseFirstRow = 3u;    // 段起点（非 0 ⟹ 「段起点被丢」立刻可辨）
constexpr uint32_t kCaseDrawCount = 2u;   // 本段行数
constexpr uint32_t kCaseTotalRows = kCaseSliceRows * kCaseSlices;  // = 14（RT 宽 = 行号即列号）
// 🔴 RT 高 = 8 而不是 1：`testing::gpu::readImage` 建的 staging buffer 是 w×h×4 字节，而 RADV
// 对 buffer 尺寸有 64B 的隐含对齐要求 ⟹ 14×1 = 56B 会踩 **VUID-vkBindBufferMemory-None-10741**
// （实测报 "requirements size 64 vs allocationSize 56"，且回读全 0 = 判据假空）。8 行 = 448B = 7×64 ✓。
// 纵向铺满 [-1,1] ⟹ 取哪一行读都一样，本用例读第 0 行。
constexpr uint32_t kCaseRtRows = 8u;
constexpr uint32_t kCaseObjectA = 37u;    ///< 行 10 的 objectIndex（word[3]）
constexpr uint32_t kCaseObjectB = 41u;    ///< 行 11 的 objectIndex
constexpr uint32_t kCaseStartA = 13u;     ///< 行 10 的 clusterStart（word[4]）
constexpr uint32_t kCaseStartB = 19u;     ///< 行 11 的 clusterStart
constexpr uint32_t kCaseTasksA = 1u;      ///< 行 10 的 groupCountX（用例 5 把它改成 0 = 阳性对照）
constexpr uint32_t kCaseTasksB = 2u;      ///< 行 11 的 groupCountX（两个 task 同列同值 = 无害重复）
constexpr uint32_t kClearSentinel = 0u;   ///< 附件 clear 值：未画到的列必须保持它（空白对照）
constexpr vk::Format kFmt = vk::Format::eR32Uint;  // 整数附件：tag 是判据本体，不走颜色链路

static_assert(RGL::kMdiShadowRowStride == 24u, "本文件的期望值全部按 24B 行手算（§19.2 D1）");
static_assert(offsetof(RGL::MdiShadowRow, objectIndex) == 12u,
	"SPV 侧 kObjBytes = 12（word[3]）；偏移漂了列值就对不上");
static_assert(offsetof(RGL::MdiShadowRow, clusterStart) == 16u,
	"SPV 侧 kStartBytes = 16（word[4]）");

/** 本用例写进 (frameSlot=1, firstRow=3) 的两行（行序 = 候选序；孔洞由用例 5 现造）。 */
std::vector<MdiShadowRow> caseRows(uint32_t tasksA = kCaseTasksA) {
	std::vector<MdiShadowRow> rows;
	rows.push_back(MdiShadowRow { tasksA, 1u, 1u, kCaseObjectA, kCaseStartA, 0u });
	rows.push_back(MdiShadowRow { kCaseTasksB, 1u, 1u, kCaseObjectB, kCaseStartB, 0u });
	return rows;
}

/** SPV 侧的 tag 公式（`obj × 4 + start`，与 mdi_shadow_rows.mesh.slang 手工同源）。 */
constexpr uint32_t wantTag(uint32_t objectIndex, uint32_t clusterStart) {
	return objectIndex * 4u + clusterStart;
}

/** 读 SPV（缺失 = 响亮失败；产物由 tests/CMakeLists.txt 的 compile_slang 产出）。 */
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
	vk::ShaderModuleCreateInfo mi {};
	mi.setCode(code);
	return vk::raii::ShaderModule(fx.device(), mi);
}

/**
 * 把分配器的内存池**弄脏**（申请一块 → 填 0xCD → 释放回池）。
 * 「未写入区恒 0」在驱动恰好给 0 时不可判据化（★1-a0 的 M4 实测存活过）⟹ 先脏池，让 VMA
 * 大概率把脏块复用给被测 buffer。🔴 不会造成假红：实现正确时该区恒 0，与是否复用脏块无关。
 */
void poisonPool(const RGL::VmaAllocatorRAII& allocator) {
	constexpr vk::DeviceSize kPoisonBytes = 1ull << 20;
	vk::BufferCreateInfo bi {};
	bi.size = kPoisonBytes;
	bi.usage = vk::BufferUsageFlagBits::eTransferDst;
	bi.sharingMode = vk::SharingMode::eExclusive;
	VmaAllocationCreateInfo ai {};
	ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT
	         | VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo ad;
	auto poison = allocator.createBuffer(bi, ai, &ad);  // 出作用域即 vmaDestroyBuffer → 脏块回池
	REQUIRE(poison.has_value());
	std::memset(ad.pMappedData, 0xCD, static_cast<size_t>(kPoisonBytes));
}

/** host-visible + coherent 的裸 buffer（回读目标 / 附件用；不走 VMA，照 `makeReadback` 先例）。 */
struct HostBuf {
	vk::raii::Buffer buffer;
	vk::raii::DeviceMemory memory;
	std::byte* mapped = nullptr;
};

HostBuf makeHostBuf(const Fixture& fx, vk::DeviceSize bytes, vk::BufferUsageFlags usage) {
	vk::BufferCreateInfo bi {};
	bi.size = bytes;
	bi.usage = usage;
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
	// 找不到 host-visible+coherent = 环境异常，响亮失败（静默换 device-local 会让回读恒读 0，
	// 把「上传全错」读成「上传全对」——恒真判据就是这么来的）
	REQUIRE(type != UINT32_MAX);
	vk::MemoryAllocateInfo ai {};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	vk::raii::DeviceMemory mem(fx.device(), ai);
	buf.bindMemory(mem, 0);
	// ⚠ 必须在 **move 之前** map：raii 句柄 move 后源对象置空，之后再 mapMemory 就是拿
	// null VkDeviceMemory 调 vkMapMemory（实测 SIGSEGV，崩点离真因两跳）。
	std::byte* mapped = static_cast<std::byte*>(mem.mapMemory(0, req.size));
	return HostBuf { .buffer = std::move(buf), .memory = std::move(mem), .mapped = mapped };
}

/** 整张 ring → host buffer 的一次 `vkCmdCopyBuffer`（🔴 GPU 读源，这才是设备侧往返）。 */
void copyWholeRing(const Fixture& fx, VkBuffer src, uint64_t bytes, HostBuf& dst) {
	REQUIRE(dst.mapped != nullptr);
	std::memset(dst.mapped, 0xAB, static_cast<size_t>(bytes));  // 脏化：防「没拷也绿」
	fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
		const VkBufferCopy region { 0, 0, bytes };
		::vkCmdCopyBuffer(*cmd, src, *dst.buffer, 1, &region);
	});
}

/** 逐字节对账：报首个差异与差异总数（整段 CHECK_EQ 的失败信息不可读）。 */
size_t countDiff(std::span<const std::byte> got, std::span<const std::byte> want, size_t& firstOut) {
	firstOut = want.size();
	if (got.size() != want.size())
		return static_cast<size_t>(-1);  // 尺寸就不对，别再逐字节比
	size_t diff = 0;
	for (size_t i = 0; i < want.size(); ++i)
		if (got[i] != want[i]) {
			++diff;
			firstOut = i;
		}
	return diff;
}

/** 一行的字节视图（期望值出处 = 本文件构造的 `MdiShadowRow`，不是被测码）。 */
std::span<const std::byte> rowBytes(const MdiShadowRow& r) {
	return std::span<const std::byte>(reinterpret_cast<const std::byte*>(&r), sizeof(MdiShadowRow));
}

/** 一行的可读文本（失败时直接看出是哪一行、哪个字段）。 */
std::string rowText(const MdiShadowRow& r) {
	return "gcX=" + std::to_string(r.groupCountX) + " obj=" + std::to_string(r.objectIndex)
	     + " start=" + std::to_string(r.clusterStart) + " res=" + std::to_string(r.reserved0);
}

// ─── 用例 4/5 的量具：mesh+frag 管线 + R32_UINT 整数附件（照 test_mdi_drawindex 的形态）───

/** binding 0 = 行表 SSBO；push = 4B 的 rowBase（🔴 只有 MESH stage 读，勿写 eAllGraphics）。 */
struct LayoutParts {
	vk::raii::DescriptorSetLayout dsLayout;
	vk::raii::DescriptorPool pool;
	std::vector<vk::raii::DescriptorSet> sets;
	vk::raii::PipelineLayout pipeLayout;
};

LayoutParts makeLayout(const Fixture& fx, VkBuffer args, uint64_t argsBytes) {
	vk::DescriptorSetLayoutBinding lb {};
	lb.binding = 0;
	// ⚠ VK_SHADER_STAGE_ALL_GRAPHICS 按定义**不含** MESH/TASK，而本探针的 args 只有 mesh stage
	//   读 ⟹ 写成 ALL_GRAPHICS = 「no descriptor bound」一类 VVL 错（test_mdi_drawindex 记过）。
	lb.stageFlags = vk::ShaderStageFlagBits::eMeshEXT | vk::ShaderStageFlagBits::eFragment;
	lb.descriptorCount = 1;
	lb.descriptorType = vk::DescriptorType::eStorageBuffer;
	vk::DescriptorSetLayoutCreateInfo li {};
	li.setBindings(lb);
	vk::raii::DescriptorSetLayout dsLayout(fx.device(), li);

	vk::PushConstantRange pc {};
	pc.stageFlags = vk::ShaderStageFlagBits::eMeshEXT;
	pc.offset = 0;
	pc.size = 4;  // rowBase（scalar block layout，无 padding）
	vk::PipelineLayoutCreateInfo plci {};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &*dsLayout;
	plci.setPushConstantRanges(pc);
	vk::raii::PipelineLayout pipeLayout(fx.device(), plci);

	const std::array<vk::DescriptorPoolSize, 1> poolSizes {
		vk::DescriptorPoolSize { vk::DescriptorType::eStorageBuffer, 1 } };
	vk::DescriptorPoolCreateInfo pi {};
	pi.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
	pi.setPoolSizes(poolSizes);
	pi.setMaxSets(1);
	vk::raii::DescriptorPool pool(fx.device(), pi);

	vk::DescriptorSetAllocateInfo sai {};
	sai.descriptorPool = *pool;
	sai.descriptorSetCount = 1;
	sai.pSetLayouts = &*dsLayout;
	auto sets = fx.device().allocateDescriptorSets(sai);
	REQUIRE(sets.size() == 1);
	const vk::DescriptorBufferInfo bi { args, 0, argsBytes };
	vk::WriteDescriptorSet w {};
	w.dstSet = *sets[0];
	w.dstBinding = 0;
	w.dstArrayElement = 0;
	w.descriptorCount = 1;
	w.descriptorType = vk::DescriptorType::eStorageBuffer;
	w.pBufferInfo = &bi;
	fx.device().updateDescriptorSets(w, {});
	return LayoutParts { .dsLayout = std::move(dsLayout), .pool = std::move(pool),
		.sets = std::move(sets), .pipeLayout = std::move(pipeLayout) };
}

/** R32_UINT 附件（列号 == 行号：每行画在自己那一列，读回即知索引对不对）。 */
struct Target {
	Image2D img;
	vk::raii::ImageView view;
};

Target makeTarget(const Fixture& fx) {
	auto img = testing::gpu::makeImage(fx, kFmt, kCaseTotalRows, kCaseRtRows,
		vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc);
	vk::ImageViewCreateInfo vi {};
	vi.image = *img.image;
	vi.viewType = vk::ImageViewType::e2D;
	vi.format = kFmt;
	vi.subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
	return Target { .img = std::move(img), .view = vk::raii::ImageView(fx.device(), vi) };
}

void barrier(vk::raii::CommandBuffer& cmd, const vk::raii::Image& img, vk::ImageLayout oldL,
	vk::ImageLayout newL, vk::PipelineStageFlags2 srcSt, vk::AccessFlags2 srcAcc,
	vk::PipelineStageFlags2 dstSt, vk::AccessFlags2 dstAcc) {
	vk::ImageMemoryBarrier2 b {};
	b.srcStageMask = srcSt;
	b.srcAccessMask = srcAcc;
	b.dstStageMask = dstSt;
	b.dstAccessMask = dstAcc;
	b.oldLayout = oldL;
	b.newLayout = newL;
	b.image = img;
	b.subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 };
	b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	vk::DependencyInfo dep {};
	dep.setImageMemoryBarriers(b);
	cmd.pipelineBarrier2(dep);
}

/** beginRendering：clear 成哨兵 0（整数附件走 uint32 视图，不是 float）。 */
void beginRender(vk::raii::CommandBuffer& cmd, const Target& t) {
	barrier(cmd, t.img.image, vk::ImageLayout::eUndefined, vk::ImageLayout::eColorAttachmentOptimal,
		vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
		vk::PipelineStageFlagBits2::eColorAttachmentOutput,
		vk::AccessFlagBits2::eColorAttachmentWrite);
	vk::RenderingAttachmentInfo att {};
	att.imageView = *t.view;
	att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
	att.loadOp = vk::AttachmentLoadOp::eClear;
	att.storeOp = vk::AttachmentStoreOp::eStore;
	att.clearValue = vk::ClearColorValue(std::array { kClearSentinel, kClearSentinel,
		kClearSentinel, kClearSentinel });
	vk::RenderingInfo r {};
	r.renderArea = vk::Rect2D { vk::Offset2D { 0, 0 }, { kCaseTotalRows, kCaseRtRows } };
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

/** mesh+frag 管线（cullMode NONE：绕序不进判据；无深度附件；正 viewport：y 翻向不进判据）。 */
vk::raii::Pipeline makePipeline(const Fixture& fx, const vk::raii::PipelineLayout& layout,
	const vk::raii::ShaderModule& meshMod, const vk::raii::ShaderModule& fragMod) {
	const std::array<vk::PipelineShaderStageCreateInfo, 2> stages { {
		{ vk::PipelineShaderStageCreateFlags {}, vk::ShaderStageFlagBits::eMeshEXT, *meshMod, "main" },
		{ vk::PipelineShaderStageCreateFlags {}, vk::ShaderStageFlagBits::eFragment, *fragMod, "main" } } };
	vk::PipelineRasterizationStateCreateInfo rs {};
	rs.setDepthClampEnable(false);
	rs.setRasterizerDiscardEnable(false);
	rs.setPolygonMode(vk::PolygonMode::eFill);
	rs.setCullMode(vk::CullModeFlagBits::eNone);
	rs.setFrontFace(vk::FrontFace::eCounterClockwise);
	rs.setDepthBiasEnable(false);
	rs.setLineWidth(1.0f);
	vk::Viewport vp { 0.0f, 0.0f, static_cast<float>(kCaseTotalRows),
		static_cast<float>(kCaseRtRows), 0.0f, 1.0f };
	vk::Rect2D sc { vk::Offset2D { 0, 0 }, { kCaseTotalRows, kCaseRtRows } };
	vk::PipelineViewportStateCreateInfo vs {};
	vs.viewportCount = 1;
	vs.pViewports = &vp;
	vs.scissorCount = 1;
	vs.pScissors = &sc;
	vk::PipelineMultisampleStateCreateInfo ms {};
	ms.setRasterizationSamples(vk::SampleCountFlagBits::e1);
	vk::PipelineColorBlendAttachmentState cba {};
	cba.setBlendEnable(false);
	cba.setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
	                      | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
	vk::PipelineColorBlendStateCreateInfo cb {};
	cb.setAttachments(cba);
	vk::PipelineRenderingCreateInfo ri {};
	ri.colorAttachmentCount = 1;
	ri.pColorAttachmentFormats = &kFmt;
	vk::GraphicsPipelineCreateInfo pci {};
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
 * MDI 录制口——🔴 本文件**唯一**允许出现的 indirect draw，参数序封死在这里（§19.2 D3 的
 * 「vkCmd 参数序收进唯一具名入口」纪律；真实签名是 `(..., offset, drawCount, stride)`，
 * spike §6.3 的 M4 就是栽在「两个数相等 ⟹ 互换测不出东西」）。
 */
void drawMdiShadowRows(vk::raii::CommandBuffer& cmd, VkBuffer args, uint64_t argsOffset,
	uint32_t drawCount) {
	cmd.drawMeshTasksIndirectEXT(args, argsOffset, drawCount, RGL::kMdiShadowRowStride);
}

/** 一次渲染 + 整幅回读（逐 uint32，行主序；列号 = 下标 % kCaseTotalRows）。 */
std::vector<uint32_t> renderAndReadCols(const Fixture& fx, const Target& t,
	const vk::raii::Pipeline& pipe, const LayoutParts& lp, const std::function<void(vk::raii::CommandBuffer&)>& draw) {
	fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
		beginRender(cmd, t);
		cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
		cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *lp.pipeLayout, 0, *lp.sets[0], {});
		draw(cmd);
		endRender(cmd, t);
	});
	const auto bytes = testing::gpu::readImage(fx, t.img);
	std::vector<uint32_t> px(bytes.size() / 4u);
	std::memcpy(px.data(), bytes.data(), bytes.size());
	// 前提守卫：回读尺寸必须与附件一致（空回读 / 半幅回读会让下面的遍历退化成恒真或漏行）
	REQUIRE(px.size() == static_cast<size_t>(kCaseTotalRows) * kCaseRtRows);
	return px;
}

/** 某一列在**所有行**上的读数（纵向铺满的判据本体：只有一行对 = 覆盖不足也照样绿）。 */
std::string colAllRows(const std::vector<uint32_t>& px, uint32_t col) {
	std::string s = "col " + std::to_string(col) + " rows ";
	for (uint32_t r = 0; r < kCaseRtRows; ++r)
		s += std::to_string(px[static_cast<size_t>(r) * kCaseTotalRows + col]) + " ";
	return s;
}

/** 一列的逐行校验：每行都必须等于 want（返回不等的第一行号，== kCaseRtRows = 全对）。 */
uint32_t firstRowDiffers(const std::vector<uint32_t>& px, uint32_t col, uint32_t want) {
	for (uint32_t r = 0; r < kCaseRtRows; ++r)
		if (px[static_cast<size_t>(r) * kCaseTotalRows + col] != want)
			return r;
	return kCaseRtRows;
}

std::string dumpCols(const std::vector<uint32_t>& px) {
	std::string s = "row0 cols ";
	for (uint32_t c = 0; c < kCaseTotalRows; ++c)
		s += std::to_string(c) + ":" + std::to_string(px [c]) + " ";
	return s;
}

}  // namespace

}  // namespace RGL::testing::shadowargs

// ═══════════════════════════════════════════════════════════════════════════
TEST_SUITE("gpu") {

using RGL::MdiShadowRow;
using RGL::testing::shadowargs::barrier;
using RGL::testing::shadowargs::caseRows;
using RGL::testing::shadowargs::colAllRows;
using RGL::testing::shadowargs::copyWholeRing;
using RGL::testing::shadowargs::countDiff;
using RGL::testing::shadowargs::drawMdiShadowRows;
using RGL::testing::shadowargs::dumpCols;
using RGL::testing::shadowargs::firstRowDiffers;
using RGL::testing::shadowargs::Fixture;
using RGL::testing::shadowargs::HostBuf;
using RGL::testing::shadowargs::kCaseDrawCount;
using RGL::testing::shadowargs::kCaseFirstRow;
using RGL::testing::shadowargs::kCaseFrameSlot;
using RGL::testing::shadowargs::kCaseObjectA;
using RGL::testing::shadowargs::kCaseObjectB;
using RGL::testing::shadowargs::kCaseRtRows;
using RGL::testing::shadowargs::kCaseSliceRows;
using RGL::testing::shadowargs::kCaseSlices;
using RGL::testing::shadowargs::kCaseStartA;
using RGL::testing::shadowargs::kCaseStartB;
using RGL::testing::shadowargs::kClearSentinel;
using RGL::testing::shadowargs::kCaseTotalRows;
using RGL::testing::shadowargs::LayoutParts;
using RGL::testing::shadowargs::makeHostBuf;
using RGL::testing::shadowargs::makeLayout;
using RGL::testing::shadowargs::makeModule;
using RGL::testing::shadowargs::makePipeline;
using RGL::testing::shadowargs::makeTarget;
using RGL::testing::shadowargs::poisonPool;
using RGL::testing::shadowargs::renderAndReadCols;
using RGL::testing::shadowargs::rowBytes;
using RGL::testing::shadowargs::rowText;
using RGL::testing::shadowargs::Target;
using RGL::testing::shadowargs::wantTag;

TEST_CASE("shadow_args_carrier（载体账）：容量/pair/跨度 = 本文件独立手算的三个数") {
	const Fixture fx;  // 无设备 / 缺 VVL / 缺扩展即抛（**不 skip**）
	RGL::VmaAllocatorRAII allocator(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion());
	poisonPool(allocator);
	const auto ring = RGL::createMeshletShadowArgsRing(allocator, fx.device(), kCaseSliceRows,
		kCaseSlices);
	INFO("device=" << fx.deviceName());
	REQUIRE(ring.has_value());
	// 🔴 前置一律 REQUIRE：空 VmaBuffer 的句柄是 VK_NULL_HANDLE，VVL 只报错不拦，随后
	// vkCmdCopyBuffer = SIGSEGV 打断整个二进制（实测：其余 214 条用例静默不跑 = 假绿面扩大）。
	REQUIRE(ring->deviceAddress != 0);  // 句柄真值判据（真实分配的 BDA 恒非 0）
	REQUIRE(ring->buffer.buffer != VK_NULL_HANDLE);
	REQUIRE(ring->mapped != nullptr);
	REQUIRE(ring->sliceRows == kCaseSliceRows);
	REQUIRE(ring->slices == kCaseSlices);

	// 容量账（期望值 = 24B/行 × 7 行 × 2 帧槽 = 336，出处 §19.5 的公式而不是被测码）
	CHECK(ring->capacityBytes == 336u);
	CHECK(RGL::mdiShadowArgsCapacityBytes(kCaseSliceRows, kCaseSlices) == 336u);
	// 变异 S7（漏乘 slices）的专属信号
	CHECK(ring->capacityBytes != static_cast<uint64_t>(kCaseSliceRows) * RGL::kMdiShadowRowStride);

	// 🔴 同源 pair（手算：rowBase = 1×7+3 = 10，argsOffset = 10×24 = 240）
	const auto loc = ring->location(kCaseFrameSlot, kCaseFirstRow);
	CHECK(loc.rowBase == 10u);
	CHECK(loc.argsOffset == 240u);
	CHECK(loc.argsOffset == static_cast<uint64_t>(loc.rowBase) * RGL::kMdiShadowRowStride);
	CHECK(loc.argsOffset != loc.rowBase);  // 两数相等时「互换」测不出东西（M4 纪律）
	CHECK(loc.argsOffset % 4u == 0u);      // VUID-…-offset-02710
	// 轮转落在**偏移**上而不是句柄上：frameSlot 0 与 1 差恰一个 slice（168B），同一张 buffer
	const auto loc0 = ring->location(0u, kCaseFirstRow);
	CHECK(loc0.rowBase == 3u);
	CHECK(loc0.argsOffset == 72u);
	CHECK(loc.argsOffset - loc0.argsOffset == RGL::mdiShadowSliceBytes(kCaseSliceRows));

	// 跨度（变异 S6 的信号）：2 行 = 24×(2−1)+12 = 36，🔴 **不是** 48 = 2×24
	CHECK(ring->argsBytes(kCaseFrameSlot, kCaseFirstRow, kCaseDrawCount) == 36u);
	CHECK(ring->argsBytes(kCaseFrameSlot, kCaseFirstRow, 1u) == 12u);  // 07089 的 drawCount==1
	CHECK(ring->argsBytes(kCaseFrameSlot, kCaseFirstRow, 0u) == 0u);   // 零 draw 不回绕
	// 末行 offset+12 落在容量内（07089/07090 合一），而容量少 1 字节必须报（反向半边）
	CHECK(RGL::mdiShadowTailBytes(loc.argsOffset, kCaseDrawCount) == 276u);
	CHECK(RGL::mdiShadowRangeFits(ring->capacityBytes, loc.argsOffset, kCaseDrawCount));
	const uint64_t worstOffset = ring->location(kCaseSlices - 1u, 0u).argsOffset;
	// 最小够用容量 = **末行尾**（324），不是整张容量（336）：每行只读 12B 而容量按 stride 算
	// ⟹ 恒留 `stride − 12` 的余量。判据两半各钉一条（拿「容量 − 1」测必红是首版的错，见
	// tests/test_mdi_rows 的同名坑注释）。
	const uint64_t worstTail = RGL::mdiShadowTailBytes(worstOffset, kCaseSliceRows);
	CHECK(worstTail == 324u);
	CHECK(ring->capacityBytes == 336u);
	CHECK(RGL::mdiShadowRangeFits(worstTail, worstOffset, kCaseSliceRows));       // 等号放行
	CHECK_FALSE(RGL::mdiShadowRangeFits(worstTail - 1u, worstOffset, kCaseSliceRows));  // 少 1 必报
}

TEST_CASE("shadow_args_device_roundtrip（主判据）：设备按 argsOffset 取到的字节 == 我写进去的字节") {
	const Fixture fx;
	RGL::VmaAllocatorRAII allocator(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion());
	poisonPool(allocator);
	const auto ring = RGL::createMeshletShadowArgsRing(allocator, fx.device(), kCaseSliceRows,
		kCaseSlices);
	REQUIRE(ring.has_value());
	REQUIRE(ring->deviceAddress != 0);
	REQUIRE(ring->buffer.buffer != VK_NULL_HANDLE);

	const std::vector<MdiShadowRow> rows = caseRows();
	REQUIRE(rows.size() == kCaseDrawCount);
	REQUIRE_FALSE(ring->upload(kCaseFrameSlot, kCaseFirstRow, rows).has_value());
	const auto loc = ring->location(kCaseFrameSlot, kCaseFirstRow);

	// ── 主判据：整张 ring → 第二张 buffer（GPU 读源），再按**行号**逐字节对账 ──
	HostBuf dst = makeHostBuf(fx, ring->capacityBytes, vk::BufferUsageFlagBits::eTransferDst);
	copyWholeRing(fx, ring->buffer.buffer, ring->capacityBytes, dst);
	for (uint32_t row = 0; row < kCaseTotalRows; ++row) {
		INFO("设备回读 行 " << row << "（argsOffset " << static_cast<uint64_t>(row) * RGL::kMdiShadowRowStride << "）");
		const std::span<const std::byte> got(dst.mapped
			+ static_cast<uint64_t>(row) * RGL::kMdiShadowRowStride, sizeof(MdiShadowRow));
		size_t first = 0;
		if (row >= loc.rowBase && row < loc.rowBase + kCaseDrawCount) {
			const MdiShadowRow& want = rows [row - loc.rowBase];
			CHECK(rowText(*reinterpret_cast<const MdiShadowRow*>(got.data())) == rowText(want));
			CHECK(countDiff(got, rowBytes(want), first) == 0);
		}
		else {
			// 未写入区恒 0（poisonPool 之后这才是判据；S1 删 memset 就红在这里）
			const std::vector<std::byte> zero(sizeof(MdiShadowRow), std::byte { 0 });
			CHECK(countDiff(got, std::span(zero.data(), zero.size()), first) == 0);
		}
	}
	// 辅助锚点（🔴 不是主判据）：映射侧同一块内存的 CPU 视图。统一内存上它是**恒真判据**，
	// 单靠它「回读一致」什么都没证明；留着是为了失败时能两点定位（映射红 = 上传写错）。
	{
		const std::span<const std::byte> mapped(ring->mapped + loc.argsOffset,
			rows.size() * sizeof(MdiShadowRow));
		const std::span<const std::byte> want(reinterpret_cast<const std::byte*>(rows.data()),
			rows.size() * sizeof(MdiShadowRow));
		size_t first = 0;
		INFO("映射回读 first=" << first << "（辅助锚点，统一内存上恒真）");
		CHECK(countDiff(mapped, want, first) == 0);
	}
}

TEST_CASE("shadow_args_upload_guards（反向）：两道可达闸门各自报错且被拒的写不动一个字节") {
	const Fixture fx;
	RGL::VmaAllocatorRAII allocator(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion());
	poisonPool(allocator);
	const auto ring = RGL::createMeshletShadowArgsRing(allocator, fx.device(), kCaseSliceRows,
		kCaseSlices);
	REQUIRE(ring.has_value());
	REQUIRE(ring->deviceAddress != 0);
	const std::vector<MdiShadowRow> rows = caseRows();
	REQUIRE_FALSE(ring->upload(kCaseFrameSlot, kCaseFirstRow, rows).has_value());

	// 基线快照（之后每次「被拒的写」都要与它逐字节相同 = 「不动一个字节」是可测事实）
	HostBuf before = makeHostBuf(fx, ring->capacityBytes, vk::BufferUsageFlagBits::eTransferDst);
	copyWholeRing(fx, ring->buffer.buffer, ring->capacityBytes, before);

	// ① frameSlot 越界（写进相邻帧槽 = 把上一帧的行当本帧读，不报错只滞后一帧）
	const auto badSlot = ring->upload(kCaseSlices, kCaseFirstRow, rows);
	REQUIRE(badSlot.has_value());
	CHECK(badSlot->find("frame slot out of range") != std::string::npos);
	// ② 段越出 slice（firstRow 6 + 2 行 > sliceRows 7 = 踩进下一个帧槽 = 读回上一帧的行）
	const auto badRows = ring->upload(kCaseFrameSlot, kCaseSliceRows - 1u, rows);
	REQUIRE(badRows.has_value());
	CHECK(badRows->find("rows exceed the slice") != std::string::npos);
	// ③ 段起点贴到 slice 尾、但只有 1 行 = **合法**形态（反向的半边：不许把合法输入也拦下来，
	//    那是把「保守」写成「什么都不许写」）。`mdiShadowRangeFits` 那道哨兵在这里**不**出声——
	//    它在合法入参下结构上到不了（尾 ≤ (firstRow+count)×stride ≤ 容量），只有「容量公式与
	//    偏移公式分家」时才现形（变异 S7 走这条路，届时错误串含 "indirect range exceeds buffer"）。
	// ④ 空 span = 本级联零候选 = 合法空操作（不是错误，也不写任何字节）
	CHECK_FALSE(ring->upload(kCaseFrameSlot, kCaseFirstRow, {}).has_value());
	// 三条错误串两两不同（读日志要能分清是哪道闸门拦的）
	REQUIRE(badSlot.has_value());
	REQUIRE(badRows.has_value());
	CHECK_FALSE(*badSlot == *badRows);
	CHECK(badSlot->find("rows exceed the slice") == std::string::npos);
	CHECK(badRows->find("frame slot out of range") == std::string::npos);

	// 🔴 被拒的写没动过内容：再拷一次整张，与基线逐字节相同。
	// ⚠ 快照必须**只夹住被拒的两次写**——把合法的写也夹进来会让这条判据自我矛盾
	//    （首版就这么错过：③ 那条「段起点贴到 slice 尾、只 1 行」是**合法**形态，
	//     它当然会改内存，于是「不动一个字节」红在 328 字节处 = 合法写入的 clusterStart）。
	HostBuf after = makeHostBuf(fx, ring->capacityBytes, vk::BufferUsageFlagBits::eTransferDst);
	copyWholeRing(fx, ring->buffer.buffer, ring->capacityBytes, after);
	size_t first = 0;
	const size_t drift = countDiff(std::span(after.mapped, static_cast<size_t>(ring->capacityBytes)),
		std::span(before.mapped, static_cast<size_t>(ring->capacityBytes)), first);
	INFO("被拒的写动了内存：首个差异字节 " << first);
	CHECK(drift == 0);

	// ③ 合法贴尾形态（前两道放行、尾也仍在容量内）：必须**真写进去**，而且是写对地方
	//    （rowBase = 1×7+6 = 13 ⟹ argsOffset = 312；这一格是 slice 1 的最后一行）。
	const auto tailRow = ring->upload(kCaseFrameSlot, kCaseSliceRows - 1u,
		std::span<const MdiShadowRow> { rows.data(), 1u });
	CHECK_FALSE(tailRow.has_value());
	HostBuf tail = makeHostBuf(fx, ring->capacityBytes, vk::BufferUsageFlagBits::eTransferDst);
	copyWholeRing(fx, ring->buffer.buffer, ring->capacityBytes, tail);
	const uint64_t tailOffset = ring->location(kCaseFrameSlot, kCaseSliceRows - 1u).argsOffset;
	CHECK(tailOffset == 312u);
	const std::span<const std::byte> gotTail(tail.mapped + tailOffset, sizeof(MdiShadowRow));
	size_t tailFirst = 0;
	INFO("贴尾合法写入的 argsOffset = " << tailOffset);
	CHECK(countDiff(gotTail, rowBytes(rows [0]), tailFirst) == 0);
}

TEST_CASE("shadow_args_mdi_expansion（🔴 自证回路）：一次真 MDI 展开，shader 按 rowBase 取到的就是我写的那一行") {
	const Fixture fx;
	// 前置一律 REQUIRE（缺 feature 时症状不是「用例红」而是带 VVL 被 VUID-02718 打断）
	const auto feats = fx.physical().getFeatures2();
	REQUIRE(feats.features.multiDrawIndirect != 0);
	REQUIRE(fx.physical().getProperties().limits.maxDrawIndirectCount >= kCaseDrawCount);

	RGL::VmaAllocatorRAII allocator(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion());
	const auto ring = RGL::createMeshletShadowArgsRing(allocator, fx.device(), kCaseSliceRows,
		kCaseSlices);
	REQUIRE(ring.has_value());
	REQUIRE(ring->deviceAddress != 0);
	const std::vector<MdiShadowRow> rows = caseRows();
	REQUIRE_FALSE(ring->upload(kCaseFrameSlot, kCaseFirstRow, rows).has_value());
	const auto loc = ring->location(kCaseFrameSlot, kCaseFirstRow);

	// 同一张 ring 两个读者：vkCmd 收 (句柄, argsOffset)，shader 收 (描述符, rowBase)
	const auto lp = makeLayout(fx, ring->buffer.buffer, ring->capacityBytes);
	const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_shadow_rows.mesh.spv");
	const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");
	const auto pipe = makePipeline(fx, lp.pipeLayout, mesh, frag);
	const auto t = makeTarget(fx);
	const uint32_t rowBase = loc.rowBase;
	const auto px = renderAndReadCols(fx, t, pipe, lp, [&](vk::raii::CommandBuffer& cmd) {
		cmd.pushConstants(*lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		drawMdiShadowRows(cmd, ring->buffer.buffer, loc.argsOffset, kCaseDrawCount);
	});
	INFO(dumpCols(px));
	// 🔴 判据：第 10 列 == 行 10 的两个字段拼出来的 tag（**期望值 = 本文件手算**，
	// 出处 = §19.2 D1 的行布局 + SPV 里的 obj*4+start 公式），且**整列 8 行**都必须该值
	// ——只有一行对 = 覆盖不足也会读成绿（首版就是只读第 0 行，被这条自己绊了一下：
	// 14×1 的附件让 readImage 的 staging buffer 只有 56B，踩 VUID-vkBindBufferMemory-None-10741
	// 而回读全 0，「列没画到」与「量具坏」当场分不开）。
	CHECK(px.size() == static_cast<size_t>(kCaseTotalRows) * kCaseRtRows);
	{
		const uint32_t badA = firstRowDiffers(px, 10u, wantTag(kCaseObjectA, kCaseStartA));
		INFO("列 10：" << colAllRows(px, 10u) << "首个不符行 " << badA);
		CHECK(badA == kCaseRtRows);
		const uint32_t badB = firstRowDiffers(px, 11u, wantTag(kCaseObjectB, kCaseStartB));
		INFO("列 11：" << colAllRows(px, 11u) << "首个不符行 " << badB);
		CHECK(badB == kCaseRtRows);
	}
	// 空白对照（三种坏形态各有专属信号）：
	//  · 列 0..9 = **另一个帧槽**（slice 0）+ 本帧槽前几行没写过 ⟹ 必须哨兵
	//    （「轮转落在偏移上」的证据：写的是 slice 1 的行 10/11）
	//  · 列 12、13 = 同一帧槽里没写的行 ⟹ 必须哨兵（rowBase 加错维度的证据）
	for (uint32_t c = 0; c < 10u; ++c)
		CHECK(firstRowDiffers(px, c, kClearSentinel) == kCaseRtRows);
	for (uint32_t c = 12u; c < kCaseTotalRows; ++c)
		CHECK(firstRowDiffers(px, c, kClearSentinel) == kCaseRtRows);
}

TEST_CASE("shadow_args_groupcount（阳性对照）：行 10 的 groupCountX 改成 0 ⟹ 那一列必须变哨兵") {
	// 上一条的「什么都没画对但也没多出列」防线：把 gcX 清零后**只有那一列**变空白，另一列照旧
	// ⟹ 证明那些像素确实是「设备读到我写的那一行的 groupCountX」之后画出来的。
	const Fixture fx;
	const auto feats = fx.physical().getFeatures2();
	REQUIRE(feats.features.multiDrawIndirect != 0);
	RGL::VmaAllocatorRAII allocator(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion());
	const auto ring = RGL::createMeshletShadowArgsRing(allocator, fx.device(), kCaseSliceRows,
		kCaseSlices);
	REQUIRE(ring.has_value());
	REQUIRE(ring->deviceAddress != 0);
	// 🔴 行 10 的 gcX = 0 而**其余字段照写**（§19.3 ★M-0 口径：整行清零会让行表失去可归因性）
	const std::vector<MdiShadowRow> rows = caseRows(0u);
	REQUIRE(rows [0].groupCountX == 0u);
	REQUIRE(rows [0].objectIndex == kCaseObjectA);
	REQUIRE_FALSE(ring->upload(kCaseFrameSlot, kCaseFirstRow, rows).has_value());
	const auto loc = ring->location(kCaseFrameSlot, kCaseFirstRow);

	const auto lp = makeLayout(fx, ring->buffer.buffer, ring->capacityBytes);
	const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_shadow_rows.mesh.spv");
	const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/mdi_spike.frag.spv");
	const auto pipe = makePipeline(fx, lp.pipeLayout, mesh, frag);
	const auto t = makeTarget(fx);
	const uint32_t rowBase = loc.rowBase;
	const auto px = renderAndReadCols(fx, t, pipe, lp, [&](vk::raii::CommandBuffer& cmd) {
		cmd.pushConstants(*lp.pipeLayout, vk::ShaderStageFlagBits::eMeshEXT, 0, 4u, &rowBase);
		drawMdiShadowRows(cmd, ring->buffer.buffer, loc.argsOffset, kCaseDrawCount);
	});
	INFO(dumpCols(px));
	CHECK(firstRowDiffers(px, 10u, kClearSentinel) == kCaseRtRows);   // 孔洞行 = 没画（合法空转）
	{
		const uint32_t badB = firstRowDiffers(px, 11u, wantTag(kCaseObjectB, kCaseStartB));
		INFO("列 11：" << colAllRows(px, 11u) << "首个不符行 " << badB);
		CHECK(badB == kCaseRtRows);   // 后一列照旧 = **不跳号**（0 计数不跳号，§19.2 D4）
	}
	for (uint32_t c = 0; c < 10u; ++c)
		CHECK(firstRowDiffers(px, c, kClearSentinel) == kCaseRtRows);
	for (uint32_t c = 12u; c < kCaseTotalRows; ++c)
		CHECK(firstRowDiffers(px, c, kClearSentinel) == kCaseRtRows);
}

}  // TEST_SUITE("gpu")
