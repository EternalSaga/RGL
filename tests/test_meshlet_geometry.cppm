module;
/**
 * test_meshlet_geometry —— [gpu] 簇产物 4 段 GPU 上传的**设备级回读校验**（簇化轴 ★1-a0）
 *
 * ## 测什么
 * `rgl/meshlet_geometry.cppm` 的 `uploadMeshletGeometry`：把一份 `MeshletAsset` 的
 * **顶点池 / 簇描述表 / Tris / Remap** 四段各做成一个 storage buffer，然后**逐段从设备里读回来**
 * 与 CPU 侧逐字节对账。四段全覆盖，一段不落。
 *
 * ## 🔴 为什么必须走 vkCmdCopyBuffer，而不是读映射指针
 * 本批的 buffer 是 `CPU_TO_GPU + MAPPED`（照抄现行大块只读 storage buffer 的做法）。在统一内存
 * 的机器上，映射指针与设备侧**同一页**——只读映射指针的"回读"是**恒真判据**：它验的是"我写进去的
 * 字节还在那儿"，永远验不到"设备看到的是不是那些字节"。故本用例走 `vkCmdCopyBuffer` 的**真设备
 * 往返**（GPU 读源 buffer → 写目标 buffer → CPU 读目标）。映射指针那条**也**查（见下"两点定位"），
 * 但它是辅助锚点不是判据本体。
 * 代价写清楚：这要求源 buffer 带 `eTransferSrc`（meshlet_geometry 的 usage 里那条注释就是为它）。
 *
 * ## 两点定位（映射指针 + 设备回读都查，失败形态可分辨）
 *   · 映射回读红、设备回读红 ⟹ 上传本身写错（memcpy 偏移/大小/段配错）
 *   · 映射回读绿、设备回读红 ⟹ 回读 harness 坏（dst 偏移/大小算错），不是被测码
 *   · 两个都绿 ⟹ 上传与往返都对
 * 只查设备回读也能抓变异，但红了分不清是哪儿坏；多一条映射侧断言的代价是 4 行。
 *
 * ## 输入规模纪律（🔴 与 ★0-b1 同口径）
 * 合成产物 = 1 prim / 6 顶点 / 2 簇 / 2 group / 2 节点 ≈ 700B，毫秒级。**不**读 184MB 的
 * bistro 产物、**不**依赖构建目录里的任何文件——产物不在时用例必须照样跑。
 * 期望值**不抄被测码**：四段的字节数由本文件按 format 规格常量（192/16/44/36/32/48 + 段间
 * 16B 对齐）独立算出，与被测模块从视图取到的数互为对账（它漂了这条先红）。
 *
 * ## 能力口径（FAIL 不 SKIP，gpu 套件纪律）
 * 无设备 / 缺 VVL / 缺扩展 = `Fixture` 构造即抛，记失败，不静默跳过（静默跳过 = 假绿）。
 *
 * ## 变异红线声明（**全部实测**于 2026-10-12：改 `meshlet_geometry_impl.cppm` → 建 rgl_tests
 * 跑 gpu 套件变红 → 不带 -p 的 `cp` 还原 → 还原后基线重跑全绿）
 *  - M1 **两段配错**（Tris ↔ Remap 源互换）      → 两段 `bytes`/`allocBytes` 与逐字节全红，
 *    另两段仍绿 = 可归因到段
 *  - M2 **段大小取元素数而非字节数**（clusters 的 `size()` 替 `size_bytes()`）→ 该段两条
 *    大小断言 + 逐字节红（期望值出处 = 本文件的规格常量，不是被测码，所以它漂了先红）
 *  - M3 **顶点池写入目的偏移挪 16B**（字节挪位）  → 仅该段逐字节红（图案 `p*37+0x11` 对任何
 *    非零位移都逐字节变），其余三段绿
 *  - M4 **补齐区不清 0**（删掉那句 memset）       → 该段尾部"应为 0"红。**这条一开始存活**：
 *    驱动给的新内存恰好全 0，"清了"与"没清"不可区分。加 `poisonPool`（先把分配器池子填 0xCD
 *    再释放）后红 ⟹ 教训：**"未初始化内存"类判据要先问"输入里有没有脏内存这个维度"**
 *  - M5 **空段保护删**（`max(alignUp(...), kAlign)` 简化成 `alignUp(...)`）→ 退化产物用例
 *    建 0 字节 buffer 失败 → `REQUIRE(uploaded.has_value())` 红
 *  - M6 **上传整段空转**（memcpy 不执行）         → 四段全红（映射侧就红）
 *  - 顺带钉住的 harness 自身两条（不是被测码的判据，但少了它们这两条用例不可信）：
 *    ① `Readback` 必须在 **move 之前** `mapMemory`（raii move 后源句柄为空 → SIGSEGV）；
 *    ② `expectedBytes` 在 `allocBytes < dataBytes`（只在实现坏时出现）时必须夹住，
 *      否则症状是堆损坏 abort 而不是干净的失败。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>    // vkCmdCopyBuffer / VkBufferCopy（rlvk 不含宏）
#include <vk_mem_alloc.h>     // VmaAllocationCreateInfo / VMA_MEMORY_USAGE_*（VmaRaii 只导出 C++ 包装）
#include <cstring>
#include <string>             // 🔴 modules 坑：INFO 里的 std::string / const char* 要有 operator<<
                              //    （rlstd 只 export `using std::string`，不带流运算符；缺这条 doctest
                              //    退回"字节数组/指针"渲染，失败信息变成 {65,77,68,…} 与 0x55…）

export module test_meshlet_geometry;

import rlstd;
import rlvk;
import vkerror;
import VmaRaii;
import meshlet_asset;
import meshlet_geometry;
import test_device_primitives;  // Fixture（引擎同款引导链：instance/device/queue/submit）

namespace RGL::testing::meshletgeo {

using testing::gpu::Fixture;

namespace {

// ─── 规格常量（按产物 format 口径手抄 = 期望值的独立出处，不取被测模块常量）────────
constexpr size_t kHdrBytes = 192, kPrimBytes = 16, kClusterBytes = 44, kGroupBytes = 36,
                 kNodeBytes = 32, kVertexBytes = 48;
constexpr uint32_t kVertexCount = 6, kClusterCount = 2;
constexpr vk::DeviceSize kAlign = 16;

vk::DeviceSize alignUp(vk::DeviceSize v) { return (v + kAlign - 1) / kAlign * kAlign; }
uint64_t alignUp16(uint64_t v) { return (v + 15) / 16 * 16; }

/** 合成产物 = 字节缓冲 + 各段偏移（期望值按偏移定位，与被测模块的取数路径无关）。 */
struct BuiltAsset {
	std::vector<std::byte> bytes;
	uint64_t offPrim = 0, offVertex = 0, offCluster = 0, offTris = 0, offRemap = 0, offGroup = 0,
	         offNode = 0;
};

/**
 * 最小合法产物（1 prim / 6 顶点 / 2 簇 / 2 group / 2 节点），段间 16B 对齐 0x00 填充。
 *
 * 四段的图案**各自不同源**（配错段立刻可辨），且**逐字节位置敏感**（挪位立刻可辨）：
 *   · 顶点池  字节 p = `p*37 + 0x11`（乘奇数模 256 ⟹ 任何非零位移都改变**每一个**字节）
 *   · 簇描述  两条真实 `MeshletDiskCluster`（字段各不相同；互换即逐字节差）
 *   · Tris    {0,1,2, 2,1,0}（两簇绕序相反 ⟹ 3 字节位移可辨）
 *   · Remap   {0,1,2,3,4,5}（u32 递增）
 * 内容必须同时**过读取侧校验**（triCount/vertCount ≤128、tri/remap 范围、refined 更浅、
 * remap < vertexCount、Σ 覆盖 == clusterCount），否则 parse 就红，测不到上传。
 */
BuiltAsset makeSynthetic() {
	BuiltAsset b;
	uint64_t off = kHdrBytes;
	b.offPrim = off;
	off = alignUp16(off + 1 * kPrimBytes);
	b.offVertex = off;
	off = alignUp16(off + kVertexCount * kVertexBytes);
	b.offCluster = off;
	off = alignUp16(off + kClusterCount * kClusterBytes);
	b.offTris = off;
	off = alignUp16(off + 6);  // 2 簇 × 1 三角 × 3 u8
	b.offRemap = off;
	off = alignUp16(off + kVertexCount * 4);  // 2 簇 × 3 顶点 × u32
	b.offGroup = off;
	off = alignUp16(off + 2 * kGroupBytes);
	b.offNode = off;
	off = alignUp16(off + 2 * kNodeBytes);
	b.bytes.assign(size_t(off), std::byte{0});
	std::byte* base = b.bytes.data();

	auto* h = reinterpret_cast<MeshletDiskHeader*>(base);
	std::memcpy(h->magic, kMeshletMagic, sizeof(kMeshletMagic));
	h->formatVersion = 1;
	h->toolVersion = 1;
	h->configFingerprint = 0x12345678;
	h->primCount = 1;
	h->vertexCount = kVertexCount;
	h->clusterCount = kClusterCount;
	h->groupCount = 2;
	h->nodeCount = 2;
	h->levelCount = 2;
	h->nodeWidth = 8;
	h->l0TriangleCount = 1;
	h->totalTriangleCount = 2;
	h->offPrimTable = b.offPrim;
	h->offVertexPool = b.offVertex;
	h->offClusters = b.offCluster;
	h->offTris = b.offTris;
	h->offRemap = b.offRemap;
	h->offGroups = b.offGroup;
	h->offNodes = b.offNode;

	reinterpret_cast<MeshletDiskPrim*>(base + b.offPrim)[0] = {0, kVertexCount, 3, 7};
	for (size_t p = 0; p < kVertexCount * kVertexBytes; ++p)
		base[b.offVertex + p] = static_cast<std::byte>(uint8_t(p * 37u + 0x11u));
	auto* clusters = reinterpret_cast<MeshletDiskCluster*>(base + b.offCluster);
	clusters[0] = {0, -1, 1, 3, 0, 0, {1.25f, 2.5f, 3.75f}, 4.5f, 0.0f};
	clusters[1] = {1, 0, 1, 3, 3, 3, {5.5f, 6.25f, 7.75f}, 8.5f, 0.5f};
	const uint8_t tris[6] = {0, 1, 2, 2, 1, 0};
	std::memcpy(base + b.offTris, tris, sizeof(tris));
	const uint32_t remap[6] = {0, 1, 2, 3, 4, 5};
	std::memcpy(base + b.offRemap, remap, sizeof(remap));
	auto* groups = reinterpret_cast<MeshletDiskGroup*>(base + b.offGroup);
	groups[0] = {0, 0, {0.0f, 0.0f, 0.0f}, 1.0f, 0.0f, 0, 1};
	groups[1] = {1, 0, {0.0f, 1.0f, 0.0f}, 2.0f, 0.5f, 1, 1};
	auto* nodes = reinterpret_cast<MeshletDiskNode*>(base + b.offNode);
	nodes[0] = {{0.0f, 0.0f, 0.0f}, 10.0f, 0.5f, -1, 0, 0};
	nodes[1] = {{0.0f, 0.0f, 1.0f}, 5.0f, 0.0f, 1, 0, 0};
	return b;
}

/**
 * 退化产物：只有 192B 文件头，七段偏移全指向文件尾、计数全 0（读取侧校验**通过**——
 * 每段 0 字节、互不重叠、落在文件内）。存在的理由 = 钉住"空段也要建 ≥16B buffer"：
 * `VkBufferCreateInfo::size` 必须 > 0，实现里那句 `max(alignUp(...), kAlign)` 没有这条
 * 用例就是无护栏的（被"顺手简化"后症状是 VVL 打断或分配失败，离真因两跳）。
 */
BuiltAsset makeEmpty() {
	BuiltAsset b;
	b.bytes.assign(kHdrBytes, std::byte{0});
	auto* h = reinterpret_cast<MeshletDiskHeader*>(b.bytes.data());
	std::memcpy(h->magic, kMeshletMagic, sizeof(kMeshletMagic));
	h->formatVersion = 1;
	h->toolVersion = 1;
	h->offPrimTable = h->offVertexPool = h->offClusters = h->offTris = h->offRemap = h->offGroups =
	    h->offNodes = kHdrBytes;
	return b;
}

/** 合成字节 → 经校验的只读视图（走真读取器，不手填 MeshletAsset 的 span）。 */
MeshletAsset parse(BuiltAsset&& built) {
	auto parsed = parseMeshletAsset(std::move(built.bytes));
	REQUIRE(parsed.has_value());
	return std::move(*parsed);
}

// ─── 设备侧回读（host-visible + coherent 目标 buffer + vkCmdCopyBuffer）──────────
struct Readback {
	vk::raii::Buffer buffer;
	vk::raii::DeviceMemory memory;
	void* mapped = nullptr;
};

Readback makeReadback(const Fixture& fx, vk::DeviceSize bytes) {
	vk::BufferCreateInfo bi{};
	bi.size = bytes;
	bi.usage = vk::BufferUsageFlagBits::eTransferDst;
	bi.sharingMode = vk::SharingMode::eExclusive;
	vk::raii::Buffer buf(fx.device(), bi);
	const auto req = buf.getMemoryRequirements();
	const auto props = fx.physical().getMemoryProperties();
	uint32_t type = UINT32_MAX;
	for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
		const auto f = static_cast<vk::MemoryPropertyFlags>(props.memoryTypes[i].propertyFlags);
		const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
		if ((req.memoryTypeBits & (1u << i)) != 0 && (f & want) == want) {
			type = i;
			break;
		}
	}
	// 找不到 host-visible+coherent = 环境异常，响亮失败（静默换 device-local 会让回读
	// 永远读到 0，把"上传全错"读成"上传全对"——恒真判据就是这么来的）
	REQUIRE(type != UINT32_MAX);
	vk::MemoryAllocateInfo ai{};
	ai.allocationSize = req.size;
	ai.memoryTypeIndex = type;
	vk::raii::DeviceMemory mem(fx.device(), ai);
	buf.bindMemory(mem, 0);
	// ⚠ 必须在 **move 之前** map：raii 句柄 move 后源对象句柄置空，之后再 mem.mapMemory
	// 就是拿 null VkDeviceMemory 调 vkMapMemory（实测 SIGSEGV，且崩点离真因两跳）。
	void* mapped = mem.mapMemory(0, req.size);
	return Readback{.buffer = std::move(buf), .memory = std::move(mem), .mapped = mapped};
}

/**
 * 把分配器的内存池**弄脏**（申请一块 → 填 0xCD → 释放回池）。
 *
 * 为什么必须做：本用例断言"补齐区是 0"。驱动给新分配的记忆**恰好全 0** 时，"实现清了 0"与
 * "实现没清、恰好是 0" 不可区分 ⟹ 删掉那句 memset 的变异会**存活**（实测确实存活）。
 * 脏池之后，VMA 大概率把这块脏内存复用给被测 buffer，补齐区就变成"不清就一定是 0xCD"。
 *
 * 🔴 这条**不会造成假红**：实现正确时补齐区恒 0，与是否复用脏块无关；不复用最坏只是
 * 这条断言又退化成"恰好为 0"，不会把正确的实现报成错的。
 */
void poisonPool(const RGL::VmaAllocatorRAII& allocator) {
	constexpr vk::DeviceSize kPoisonBytes = 1ull << 20;
	vk::BufferCreateInfo bi{};
	bi.size = kPoisonBytes;
	bi.usage = vk::BufferUsageFlagBits::eTransferDst;
	bi.sharingMode = vk::SharingMode::eExclusive;
	VmaAllocationCreateInfo ai{};
	ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
	ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
	    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
	VmaAllocationInfo ad;
	auto poison = allocator.createBuffer(bi, ai, &ad, "test/poison_pool");  // 出作用域即 vmaDestroyBuffer → 脏块回池
	REQUIRE(poison.has_value());
	std::memset(ad.pMappedData, 0xCD, static_cast<size_t>(kPoisonBytes));
}

/** 一段 → 目标 buffer 里的一格（srcOffset 恒 0：每段各是一个独立 buffer）。 */
struct Copy {
	VkBuffer src;
	vk::DeviceSize dstOffset = 0;
	vk::DeviceSize size = 0;
};

void copyAll(const Fixture& fx, const Readback& dst, std::span<const Copy> copies) {
	fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
		for (const Copy& c : copies) {
			const VkBufferCopy region{0, c.dstOffset, c.size};
			::vkCmdCopyBuffer(*cmd, c.src, *dst.buffer, 1, &region);
		}
	});
}

/**
 * 逐字节对账：报**首个差异**与**差异总数**（整段 CHECK_EQ 的失败信息不可读，
 * 而只报首个差异会看不出"整段错"与"一个字节错"的区别）。
 * @return 差异字节数（0 = 完全相同；断言在调用点，便于带段名 INFO）
 */
size_t countDiff(std::span<const uint8_t> got, std::span<const uint8_t> want, size_t& firstOut) {
	firstOut = want.size();
	if (got.size() != want.size())
		return static_cast<size_t>(-1);  // 尺寸就不对，别再逐字节比
	size_t diff = 0;
	for (size_t i = 0; i < want.size(); ++i) {
		if (got[i] != want[i]) {
			++diff;
			firstOut = i;
		}
	}
	return diff;
}

/**
 * 一段的期望字节 = 产物里该段的 dataBytes 个字节 + 补齐区全 0。
 *
 * ⚠ **两侧大小不等时必须夹住**（`allocBytes < dataBytes` 只在被测实现坏掉时出现，例如
 * 段大小取错）：不夹就是往一个短 vector 尾部越界写，症状从"用例红"变成堆损坏 + abort，
 * 归因成本翻几倍（实测：段配错变异下就是 double free 而不是干净的失败）。
 */
std::vector<uint8_t> expectedBytes(const BuiltAsset& built, uint64_t sectionOffset,
	vk::DeviceSize dataBytes, vk::DeviceSize allocBytes) {
	std::vector<uint8_t> out(static_cast<size_t>(allocBytes), uint8_t{0});
	const size_t copyBytes = static_cast<size_t>(
	    std::min(dataBytes, std::min(allocBytes, static_cast<vk::DeviceSize>(
	        built.bytes.size() - sectionOffset))));
	for (size_t i = 0; i < copyBytes; ++i)
		out[i] = static_cast<uint8_t>(built.bytes[static_cast<size_t>(sectionOffset) + i]);
	return out;
}

/** 四段的一份账：段名 + 产物内偏移 + **本文件独立算出**的数据字节数（不取被测模块的数）。 */
struct SectionExpectation {
	const char* name;
	uint64_t fileOffset;
	vk::DeviceSize dataBytes;
};

}  // namespace

}  // namespace RGL::testing::meshletgeo

// ═══════════════════════════════════════════════════════════════════════════
// 用例
// ═══════════════════════════════════════════════════════════════════════════

TEST_SUITE("gpu") {

using RGL::testing::meshletgeo::alignUp;
using RGL::testing::meshletgeo::BuiltAsset;
using RGL::testing::meshletgeo::Copy;
using RGL::testing::meshletgeo::countDiff;
using RGL::testing::meshletgeo::expectedBytes;
using RGL::testing::meshletgeo::Fixture;
using RGL::testing::meshletgeo::makeEmpty;
using RGL::testing::meshletgeo::makeReadback;
using RGL::testing::meshletgeo::makeSynthetic;
using RGL::testing::meshletgeo::parse;
using RGL::testing::meshletgeo::poisonPool;
using RGL::testing::meshletgeo::Readback;
using RGL::testing::meshletgeo::SectionExpectation;

TEST_CASE("meshlet_geometry_upload_readback（核心）：4 段上传后逐字节回环，段段可归因") {
	const Fixture fx;  // 无设备 / 缺 VVL / 缺扩展即抛（**不 skip**）
	// 测试自建 VMA 分配器（四参与引擎 Resources 构造器同源）：被测函数收的是分配器引用，
	// 不在这里重造 Resources——那要拖进 Swapchain/描述符堆，测的也不是更真的东西。
	RGL::VmaAllocatorRAII allocator(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion());
	poisonPool(allocator);  // 让"补齐区清 0"可判据化（理由 → poisonPool 注释）

	const BuiltAsset built = makeSynthetic();
	// 传**副本**给 parse（parse 接管字节所有权）：built 还要留着当期望值的出处。
	const RGL::MeshletAsset asset = parse(BuiltAsset(built));
	REQUIRE(asset.header.clusterCount == 2);  // 前提守卫：空集合上的"逐段全对"是假绿
	REQUIRE(asset.header.vertexCount == 6);

	const auto uploaded = RGL::uploadMeshletGeometry(allocator, fx.device(), asset, "synthetic");
	REQUIRE(uploaded.has_value());
	INFO("device=" << fx.deviceName());

	// 期望账（🔴 数值出处 = 本文件的 format 规格常量与段间 16B 对齐口径，不是被测码）：
	//   顶点池 6×48=288（视图给字节）  簇表 2×44=88（视图给元素数）
	//   Tris   段字节 = offRemap-offTris = 16（含 10B 段间填充，视图给字节）
	//   Remap  段元素 = (offGroups-offRemap)/4 = 8 → 32B（含 8B 段间填充）
	const SectionExpectation expect[4] = {
		{"vertexPool", built.offVertex, 6 * 48},
		{"clusters", built.offCluster, 2 * 44},
		{"tris", built.offTris, built.offRemap - built.offTris},
		{"remap", built.offRemap, built.offGroup - built.offRemap},
	};
	const RGL::MeshletGeometrySection* section[4] = {
		&uploaded->vertexPool, &uploaded->clusters, &uploaded->tris, &uploaded->remap};

	vk::DeviceSize total = 0;
	for (int i = 0; i < 4; ++i) {
		const SectionExpectation& e = expect[i];
		INFO("section " << e.name);
		CHECK(e.dataBytes > 0);  // 前提守卫：0 字节的段会让"逐字节相同"退化成恒真
		CHECK(RGL::kMeshletBufferAlign == 16);
		// 被测模块从视图取到的段大小 == 本文件独立算出的大小（它漂了这条先红）
		CHECK(section[i]->bytes == e.dataBytes);
		CHECK(section[i]->allocBytes == alignUp(e.dataBytes));
		total += section[i]->allocBytes;
	}
	CHECK(uploaded->clusterCount == 2);
	CHECK(uploaded->vertexCount == 6);
	// 🔴 段缺失必须先在这里变成**干净的用例失败**，不能让它走到下面的 vkCmdCopyBuffer：
	// 空 `VmaBuffer` 的句柄是 VK_NULL_HANDLE，VVL 只报错不拦，随后本用例解引用已空的状态
	// = SIGSEGV **打断整个测试二进制**（实测：其余 214 条用例静默不跑，"全绿"面被扩大成假绿）。
	// 判据取 `deviceAddress`——真实分配的 BDA 恒非 0，未上传的默认构造段恒 0（比 allocBytes>0 更直接）。
	for (int i = 0; i < 4; ++i)
		REQUIRE(section[i]->deviceAddress != 0);

	const Readback dst = makeReadback(fx, total);
	std::vector<Copy> copies;
	std::vector<vk::DeviceSize> offsets;
	vk::DeviceSize cursor = 0;
	for (int i = 0; i < 4; ++i) {
		offsets.push_back(cursor);
		copies.push_back(Copy{*section[i]->buffer, cursor, section[i]->allocBytes});
		cursor += section[i]->allocBytes;
	}
	copyAll(fx, dst, std::span(copies.data(), copies.size()));

	const auto* back = static_cast<const uint8_t*>(dst.mapped);
	for (int i = 0; i < 4; ++i) {
		const SectionExpectation& e = expect[i];
		const std::vector<uint8_t> want = expectedBytes(built, e.fileOffset, e.dataBytes,
			section[i]->allocBytes);
		const std::span<const uint8_t> wantSpan(want.data(), want.size());
		// 主判据：**设备侧**往返（GPU 读源 buffer 写目标 buffer）
		const std::span<const uint8_t> device(back + offsets[i], section[i]->allocBytes);
		size_t first = 0;
		const size_t diff = countDiff(device, wantSpan, first);
		INFO("section " << e.name << " 设备回读: diff=" << diff << " first=" << first
			<< " / " << want.size() << " 字节");
		CHECK(diff == 0);
		// 辅助锚点：映射指针（同一块内存的 CPU 侧视图）。它绿而设备回读红 = harness 坏；
		// 它红 = 上传本身写错。单靠它在统一内存上是恒真判据，故不作主判据。
		const std::span<const uint8_t> mapped(
			static_cast<const uint8_t*>(section[i]->buffer.getMappedData()), section[i]->allocBytes);
		size_t mappedFirst = 0;
		const size_t mappedDiff = countDiff(mapped, wantSpan, mappedFirst);
		INFO("section " << e.name << " 映射回读: diff=" << mappedDiff << " first=" << mappedFirst);
		CHECK(mappedDiff == 0);
	}
}

TEST_CASE("meshlet_geometry_empty_sections（反向配对）：退化产物的空段仍建 16B buffer 且清 0") {
	// 这条是"不该触发"的那一半：没有它，`max(alignUp(...), kAlign)` 那句被删后
	// 只会在真产物上"一切正常"，而在退化产物上建 0 字节 buffer（VVL 打断 / 分配失败）。
	const Fixture fx;
	RGL::VmaAllocatorRAII allocator(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion());
	poisonPool(allocator);  // 空段的**全部**字节都是补齐区 ⟹ 这条用例是"清 0"契约最强的观测面

	const RGL::MeshletAsset asset = parse(makeEmpty());
	REQUIRE(asset.header.clusterCount == 0);  // 前提守卫：本条测的就是"全空"这个形态

	const auto uploaded = RGL::uploadMeshletGeometry(allocator, fx.device(), asset, "empty");
	REQUIRE(uploaded.has_value());
	const RGL::MeshletGeometrySection* section[4] = {
		&uploaded->vertexPool, &uploaded->clusters, &uploaded->tris, &uploaded->remap};
	vk::DeviceSize total = 0;
	for (const RGL::MeshletGeometrySection* s : section) {
		CHECK(s->bytes == 0);
		CHECK(s->allocBytes == RGL::kMeshletBufferAlign);  // 空段也必须有非零 buffer
		// 🔴 同上一条用例：段缺失必须先红在这里。空段的 buffer 句柄一旦为 VK_NULL_HANDLE，
		// 下面那次 copy 只被 VVL 报错不拦，随后 SIGSEGV **打断整个测试二进制**（实测踩过）。
		REQUIRE(s->deviceAddress != 0);
		total += s->allocBytes;
	}
	const Readback dst = makeReadback(fx, total);
	std::vector<Copy> copies;
	for (int i = 0; i < 4; ++i)
		copies.push_back(Copy{*section[i]->buffer, vk::DeviceSize(i) * RGL::kMeshletBufferAlign,
			RGL::kMeshletBufferAlign});
	copyAll(fx, dst, std::span(copies.data(), copies.size()));
	// 空段没有数据可比 ⟹ 判据换成"整块都是 0"（补齐区清 0 的契约在无数据段上的可见形态）
	const auto* back = static_cast<const uint8_t*>(dst.mapped);
	for (vk::DeviceSize i = 0; i < total; ++i) {
		INFO("empty section byte " << i);
		CHECK(back[i] == 0);
	}
}

}  // TEST_SUITE("gpu")
