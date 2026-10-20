module;
#include <fmt/format.h>
module meshlet_asset:impl;

import meshlet_asset;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;

namespace RGL {
namespace {

/**
 * 段表条目（校验期中间量）：名字（报错归因用）+ 偏移（header 读来）+ 字节大小 + 对齐要求。
 * 定长段大小 = header 计数 × 元素 stride；变长段（Tris/Remap）大小 = 段表偏移差——
 * 「段字节数从段表推导」即此意（接口头注释），本模块不硬编码任何绝对偏移。
 */
struct SectionInfo {
	/** 段名（PrimTable / VertexPool / …），只进错误串。 */
	const char* name;
	/** 段起始偏移（相对文件头）。 */
	uint64_t offset;
	/** 段字节数（不含段间对齐填充）。 */
	uint64_t bytes;
	/** 元素对齐要求（reinterpret_cast 合法性的前提）。 */
	uint64_t align;
};

/** 七段固定顺序（= format.h 的落盘顺序），下标即段序。
 *  ⚠ v2 起产物尾部还可有第八段 MustL0，**本模块不读它**（口径见接口头 kMeshletFormatVersionMax
 *  那条）；本表只列"读取侧会拿视图的段"，所以 `checkSectionChain` 的末段仍是 Nodes，
 *  尾部多出来的字节由「Nodes 段尾 ≤ 文件长」这条放过。 */
constexpr size_t kSectionCount = 7;

/** sourceSha1 等身份字段的小写 hex（仅日志身份，→ 接口裁决 1）。 */
std::string toHexLower(const uint8_t* data, size_t count) {
	static constexpr char kDigits[] = "0123456789abcdef";
	std::string out;
	out.reserve(count * 2);
	for (size_t i = 0; i < count; ++i) {
		out.push_back(kDigits[data[i] >> 4]);
		out.push_back(kDigits[data[i] & 0x0F]);
	}
	return out;
}

/**
 * 头级校验：文件大小 ≥ 192 / magic / formatVersion 兼容范围 / toolVersion 兼容范围。
 * @param image 文件镜像（调用方保证非空指针语义：size 已先验）
 * @param outHeader 通过时写入头部指针（指向 image 缓冲，随镜像存活）
 * @return nullopt = 通过；有值 = 可归因错误串
 */
std::optional<std::string> checkHeader(const std::vector<std::byte>& image,
                                       const MeshletDiskHeader*& outHeader) {
	if (image.size() < sizeof(MeshletDiskHeader))
		return fmt::format(
		    "meshlet asset: file smaller than FileHeader ({} < {} bytes) — truncated or not a meshlet asset",
		    image.size(), sizeof(MeshletDiskHeader));
	const auto* h = reinterpret_cast<const MeshletDiskHeader*>(image.data());
	if (std::memcmp(h->magic, kMeshletMagic, sizeof(kMeshletMagic)) != 0)
		return std::string("meshlet asset: magic mismatch — not an RGLCLOD meshlet asset");
	if (h->formatVersion < kMeshletFormatVersionMin || h->formatVersion > kMeshletFormatVersionMax)
		return fmt::format(
		    "meshlet asset: unsupported formatVersion {} (engine understands [{}, {}]) — repackage with a matching meshlet_pack or upgrade the engine",
		    h->formatVersion, kMeshletFormatVersionMin, kMeshletFormatVersionMax);
	if (h->toolVersion < kMeshletToolVersionMin || h->toolVersion > kMeshletToolVersionMax)
		return fmt::format(
		    "meshlet asset: unsupported toolVersion {} (engine understands [{}, {}]) — packer algorithm contract unknown to this reader",
		    h->toolVersion, kMeshletToolVersionMin, kMeshletToolVersionMax);
	outHeader = h;
	return std::nullopt;
}

/**
 * 段表推导：按 header 计数算定长段大小、按偏移差导出 Tris/Remap 大小。
 *
 * 「count × stride 不溢出」的前半段论证：count 是 uint32、stride ≤ 48 ⟹ 积 < 2^38，
 * 在 uint64 下**恒不溢出**（无需运行时分支，静态成立）；会绕回的是「偏移 + 大小」
 * （偏移来自文件、可被构造）——在 checkEachSection 里逐段显式设防。
 * @return nullopt = 推导成功（out 已填）；有值 = 段表不自洽错误串
 */
std::optional<std::string> buildSections(const MeshletDiskHeader& h,
                                         std::array<SectionInfo, kSectionCount>& out) {
	if (h.offPrimTable < sizeof(MeshletDiskHeader))
		return fmt::format(
		    "meshlet asset: section table inconsistent: PrimTable offset {} precedes header end {}",
		    h.offPrimTable, sizeof(MeshletDiskHeader));
	// Tris/Remap 是变长流，段长 = 偏移差 ⟹ 先要求这三条偏移单调，否则差值绕回。
	if (h.offTris > h.offRemap || h.offRemap > h.offGroups)
		return fmt::format(
		    "meshlet asset: section table inconsistent: Tris/Remap/Groups offsets not monotonic (offTris {} offRemap {} offGroups {})",
		    h.offTris, h.offRemap, h.offGroups);
	out = {
	    SectionInfo{"PrimTable", h.offPrimTable, uint64_t(h.primCount) * sizeof(MeshletDiskPrim),
	        alignof(MeshletDiskPrim)},
	    SectionInfo{"VertexPool", h.offVertexPool, uint64_t(h.vertexCount) * kMeshletVertexStride, 1},
	    SectionInfo{"Clusters", h.offClusters, uint64_t(h.clusterCount) * sizeof(MeshletDiskCluster),
	        alignof(MeshletDiskCluster)},
	    SectionInfo{"Tris", h.offTris, h.offRemap - h.offTris, 1},
	    SectionInfo{"Remap", h.offRemap, h.offGroups - h.offRemap, alignof(uint32_t)},
	    SectionInfo{"Groups", h.offGroups, uint64_t(h.groupCount) * sizeof(MeshletDiskGroup),
	        alignof(MeshletDiskGroup)},
	    SectionInfo{"Nodes", h.offNodes, uint64_t(h.nodeCount) * sizeof(MeshletDiskNode),
	        alignof(MeshletDiskNode)},
	};
	return std::nullopt;
}

/** 逐段校验：偏移对齐（reinterpret_cast 前提）/ 偏移 + 大小不绕回 / 偏移落在文件内。 */
std::optional<std::string> checkEachSection(const std::array<SectionInfo, kSectionCount>& sections,
                                            uint64_t fileSize) {
	for (const SectionInfo& s : sections) {
		if (s.offset % s.align != 0)
			return fmt::format(
			    "meshlet asset: section table inconsistent: {} offset {} not a multiple of its {}-byte element alignment",
			    s.name, s.offset, s.align);
		if (s.bytes > std::numeric_limits<uint64_t>::max() - s.offset)
			return fmt::format(
			    "meshlet asset: section table inconsistent: {} offset {} + size {} wraps around 64-bit",
			    s.name, s.offset, s.bytes);
		if (s.offset > fileSize)
			return fmt::format(
			    "meshlet asset: section table inconsistent: {} offset {} beyond file size {}", s.name,
			    s.offset, fileSize);
	}
	return std::nullopt;
}

/** 段间校验：相邻段不重叠（前段尾 ≤ 后段头，段间只允许对齐填充）+ 末段整体落在文件内。 */
std::optional<std::string> checkSectionChain(const std::array<SectionInfo, kSectionCount>& sections,
                                             uint64_t fileSize) {
	for (size_t i = 0; i + 1 < sections.size(); ++i) {
		const uint64_t end = sections[i].offset + sections[i].bytes;
		if (end > sections[i + 1].offset)
			return fmt::format(
			    "meshlet asset: section table inconsistent: {} section end {} overlaps {} offset {}",
			    sections[i].name, end, sections[i + 1].name, sections[i + 1].offset);
	}
	const uint64_t tail = sections.back().offset + sections.back().bytes;
	if (tail > fileSize)
		return fmt::format(
		    "meshlet asset: Nodes section extends beyond file (end {} > file size {}) — truncated tail?",
		    tail, fileSize);
	return std::nullopt;
}

/**
 * 单簇校验：tri 与 remap 范围落在各自段内 / group 与 refined 下标有界 / `refined` 指向
 * **更浅** group（结构不变量，与工具 --verify ④ 同口径——「refined 指向同层或更深」=
 * DAG 侧产出被改坏的形态）。8bit 契约不在此（它先于 Σ 对账单独一轮，见 checkClusters）。
 */
std::optional<std::string> checkClusterAt(const MeshletAsset& a, size_t i) {
	const MeshletDiskCluster& c = a.clusters[i];
	const uint64_t groupCount = a.groups.size();
	if (uint64_t(c.triOffset) + uint64_t(c.triCount) * 3 > a.tris.size())
		return fmt::format(
		    "meshlet asset: cluster {} tri range [{}, {}) out of bounds (Tris section {} bytes)", i,
		    c.triOffset, uint64_t(c.triOffset) + uint64_t(c.triCount) * 3, a.tris.size());
	if (uint64_t(c.remapOffset) + c.vertCount > a.remap.size())
		return fmt::format(
		    "meshlet asset: cluster {} remap range [{}, {}) out of bounds (Remap section {} elements)",
		    i, c.remapOffset, uint64_t(c.remapOffset) + c.vertCount, a.remap.size());
	if (c.group >= groupCount)
		return fmt::format("meshlet asset: cluster {} references group {} out of range (groupCount {})",
		    i, c.group, groupCount);
	if (c.refined < -1 || (c.refined >= 0 && uint64_t(c.refined) >= groupCount))
		return fmt::format(
		    "meshlet asset: cluster {} references refined group {} out of range (groupCount {})", i,
		    c.refined, groupCount);
	if (c.refined >= 0 && a.groups[size_t(c.refined)].depth >= a.groups[c.group].depth)
		return fmt::format(
		    "meshlet asset: cluster {} refined group {} (depth {}) not shallower than its group {} (depth {}) — DAG structure violated",
		    i, c.refined, a.groups[size_t(c.refined)].depth, c.group, a.groups[c.group].depth);
	return std::nullopt;
}

/**
 * 簇段校验：逐簇（checkClusterAt）+ 段大小与簇计数自洽
 * （Tris 段字节 == Σ triCount×3、Remap 段字节 == Σ vertCount×4，**容差 = 段间对齐填充**：
 * 写侧段间 0x00 填充到 16B 对齐 ⟹ 判据是 `Σ ≤ 段字节 < Σ + kMeshletSectionAlign`，
 * 严格相等对写侧自己的产物就不成立——合成缓冲按写侧同法填充，本条判据被它实测钉死）。
 * Σ 上界 128×3×2^32 < 2^40，uint64 恒不溢出。
 */
std::optional<std::string> checkClusters(const MeshletAsset& a) {
	// 8bit 契约最先：它坏时 Σ 必坏（>128 的计数把 Σ 直接顶爆），先查才保证归因唯一。
	for (size_t i = 0; i < a.clusters.size(); ++i) {
		const MeshletDiskCluster& c = a.clusters[i];
		if (c.triCount > 128 || c.vertCount > 128)
			return fmt::format(
			    "meshlet asset: cluster {} violates 8-bit index contract (triCount {} / vertCount {} > 128)",
			    i, c.triCount, c.vertCount);
	}
	uint64_t sumTriBytes = 0;
	uint64_t sumRemapBytes = 0;
	for (const MeshletDiskCluster& c : a.clusters) {
		sumTriBytes += uint64_t(c.triCount) * 3;
		sumRemapBytes += uint64_t(c.vertCount) * 4;
	}
	if (sumTriBytes > a.tris.size() || a.tris.size() - sumTriBytes >= kMeshletSectionAlign)
		return fmt::format(
		    "meshlet asset: Tris section {} bytes inconsistent with sum(cluster.triCount)*3 = {} (gap beyond <{}B alignment padding) — section/cluster mismatch",
		    a.tris.size(), sumTriBytes, kMeshletSectionAlign);
	if (sumRemapBytes > a.remap.size() * 4 ||
	    a.remap.size() * 4 - sumRemapBytes >= kMeshletSectionAlign)
		return fmt::format(
		    "meshlet asset: Remap section {} bytes inconsistent with sum(cluster.vertCount)*4 = {} (gap beyond <{}B alignment padding) — section/cluster mismatch",
		    a.remap.size() * 4, sumRemapBytes, kMeshletSectionAlign);
	for (size_t i = 0; i < a.clusters.size(); ++i)
		if (auto err = checkClusterAt(a, i))
			return err;
	return std::nullopt;
}

/** group 段校验：逐 group 簇区间有界 + `clusterCount == Σ group.clusterCount`（覆盖对账）。 */
std::optional<std::string> checkGroups(const MeshletAsset& a) {
	uint64_t sumCover = 0;
	for (size_t i = 0; i < a.groups.size(); ++i) {
		const MeshletDiskGroup& g = a.groups[i];
		if (uint64_t(g.clusterOffset) + g.clusterCount > a.clusters.size())
			return fmt::format(
			    "meshlet asset: group {} cluster range [{}, {}) out of bounds (clusterCount {})", i,
			    g.clusterOffset, uint64_t(g.clusterOffset) + g.clusterCount, a.clusters.size());
		sumCover += g.clusterCount;
	}
	if (sumCover != a.clusters.size())
		return fmt::format(
		    "meshlet asset: clusterCount {} != sum(group.clusterCount) {} — group coverage mismatch",
		    a.clusters.size(), sumCover);
	return std::nullopt;
}

/** 重映射校验：全部全局顶点下标 < vertexCount（恒指向顶点池）。 */
std::optional<std::string> checkRemapValues(const MeshletAsset& a) {
	for (size_t i = 0; i < a.remap.size(); ++i)
		if (a.remap[i] >= a.header.vertexCount)
			return fmt::format(
			    "meshlet asset: remap[{}] = {} out of bounds (vertexCount {}) — remap index beyond vertex pool",
			    i, a.remap[i], a.header.vertexCount);
	return std::nullopt;
}

}  // namespace

std::expected<MeshletAsset, std::string> parseMeshletAsset(std::vector<std::byte>&& image) {
	const MeshletDiskHeader* header = nullptr;
	if (auto err = checkHeader(image, header))
		return std::unexpected(*err);
	std::array<SectionInfo, kSectionCount> sections;
	if (auto err = buildSections(*header, sections))
		return std::unexpected(*err);
	if (auto err = checkEachSection(sections, image.size()))
		return std::unexpected(*err);
	if (auto err = checkSectionChain(sections, image.size()))
		return std::unexpected(*err);

	// 校验全过才拼视图：镜像 move 进 asset 后堆缓冲地址不变（vector 移动语义），
	// span 与 header 指针在 move 之后重取，不依赖「move 保持地址」之外的任何假设。
	MeshletAsset asset;
	asset.image = std::move(image);
	const std::byte* base = asset.image.data();
	asset.header = *reinterpret_cast<const MeshletDiskHeader*>(base);
	asset.prims = {reinterpret_cast<const MeshletDiskPrim*>(base + sections[0].offset),
	    asset.header.primCount};
	asset.vertexPool = {base + sections[1].offset, sections[1].bytes};
	asset.clusters = {reinterpret_cast<const MeshletDiskCluster*>(base + sections[2].offset),
	    asset.header.clusterCount};
	asset.tris = {reinterpret_cast<const uint8_t*>(base + sections[3].offset), sections[3].bytes};
	asset.remap = {reinterpret_cast<const uint32_t*>(base + sections[4].offset),
	    sections[4].bytes / sizeof(uint32_t)};
	asset.groups = {reinterpret_cast<const MeshletDiskGroup*>(base + sections[5].offset),
	    asset.header.groupCount};
	asset.nodes = {reinterpret_cast<const MeshletDiskNode*>(base + sections[6].offset),
	    asset.header.nodeCount};
	asset.sourceSha1Hex = toHexLower(asset.header.sourceSha1, sizeof(asset.header.sourceSha1));

	if (auto err = checkClusters(asset))
		return std::unexpected(*err);
	if (auto err = checkGroups(asset))
		return std::unexpected(*err);
	if (auto err = checkRemapValues(asset))
		return std::unexpected(*err);
	return asset;
}

std::expected<MeshletAsset, std::string> loadMeshletAsset(std::string_view path) {
	std::ifstream in{std::string(path), std::ios::binary | std::ios::ate};
	if (!in.is_open())
		return std::unexpected(
		    fmt::format("meshlet asset: file not found or not readable: {}", path));
	const std::streamoff size = in.tellg();
	if (size < 0)
		return std::unexpected(
		    fmt::format("meshlet asset: failed to query file size: {}", path));
	std::vector<std::byte> image(static_cast<size_t>(size));
	in.seekg(0);
	in.read(reinterpret_cast<char*>(image.data()), size);
	if (!in)
		return std::unexpected(
		    fmt::format("meshlet asset: file read incomplete: {}", path));
	auto parsed = parseMeshletAsset(std::move(image));
	if (!parsed)
		return std::unexpected(fmt::format("{} (in {})", parsed.error(), path));
	return parsed;
}

}  // namespace RGL
