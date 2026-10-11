module;
#include <fmt/format.h>
module meshlet_hand:impl;

import meshlet_hand;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import meshlet_asset;

namespace RGL {

// 布局常量对账（编译期）：uv 偏移必须落在顶点内部且顶点步长 = 产物契约。
// 🔴 `kMeshletHandUvByteOffset == offsetof(ModelVertex, uv)` 这条**在单测里钉**
// （`tests/test_meshlet_hand.cppm`：offsetof 宏需要 <cstddef>，引擎 GMF 被样式规则 B2 禁带
// STL 头，而测试 TU 不受该规则管）；`generated/model_types_generated.cppm` 自己已把
// `offsetof(ModelVertex, uv) == 24` 钉成 static_assert，两处合起来 = 常量与 schema 的联动。
static_assert(kMeshletHandUvByteOffset + 2 * sizeof(float) <= kMeshletVertexStride,
    "uv 读取窗口必须落在 48B 顶点内");
static_assert(kMeshletHandMaxClusterVertices == 128,
    "8bit 局部索引契约的推导上界（读取侧 vertCount ≤ 128）");

namespace {

/**
 * 逐三角 UV Jacobian 手性（loader `gltf_loader_impl.cppm::uvTriangleHandedness` 同式，
 * 语义逐字符照抄）：`det = s1x·s2y − s2x·s1y`，`det < 0 → −1`，**否则 +1**——含 `det == 0`
 * 与 NaN（NaN 的比较恒假 ⟹ 落 +1，现行语义）。参数 = 三顶点的 uv 逐分量（float，与 loader
 * 同精度：期望值对拍靠「输入取二进制精确格点 ⟹ float det 与 double det 同号」，见单测）。
 */
int8_t uvTriangleHandedness(float u0, float v0, float u1, float v1, float u2, float v2) {
	const float s1x = u1 - u0, s1y = v1 - v0;
	const float s2x = u2 - u0, s2y = v2 - v0;
	return (s1x * s2y - s2x * s1y) < 0.0f ? kMeshletHandNegative : kMeshletHandPositive;
}

/**
 * 读全局顶点的 uv（顶点池字节视图的两个 f32）。
 * 用 memcpy 而非 reinterpret：读取侧对 VertexPool 段只保证 1B 对齐（SectionInfo align=1），
 * 直接转 float* 是拿实现运气当契约；memcpy 在 coherent 目标上就是两条 mov。
 */
void readUv(const MeshletAsset& asset, uint32_t globalVertex, float& u, float& v) {
	const std::byte* p = asset.vertexPool.data()
	                   + static_cast<size_t>(globalVertex) * kMeshletVertexStride
	                   + kMeshletHandUvByteOffset;
	std::memcpy(&u, p, sizeof(float));
	std::memcpy(&v, p + sizeof(float), sizeof(float));
}

/**
 * 遍历单个簇的全部三角形，把结果累加进 `out`（簇筛选住在调用方，本函数只管读）。
 * @return nullopt = 正常；有值 = 断言形态触发的可归因错误串（文件头「越界纪律」）
 */
std::optional<std::string> accumulateCluster(const MeshletAsset& asset,
	const MeshletDiskCluster& c, uint64_t clusterIndex, bool wantPerTriangle,
	MeshletHandResult& out) {
	for (uint32_t k = 0; k < c.triCount; ++k) {
		// triOffset = Tris 段内**字节**偏移（u8 元素：字节偏移 == 元素下标，口径见接口文件头）。
		const size_t tb = static_cast<size_t>(c.triOffset) + static_cast<size_t>(k) * 3;
		const uint8_t local[3] = { asset.tris[tb], asset.tris[tb + 1], asset.tris[tb + 2] };
		// 断言形态（接口文件头「越界纪律」）：局部索引必须 < vertCount（由读取侧 vertCount ≤ 128
		// 推得「≥ 128 不可能」= 8bit 可解性）。与打包器 --verify ② 同判据；恒编译，Release 不关。
		for (const uint8_t l : local)
			if (l >= c.vertCount)
				return fmt::format(
				    "meshlet hand: cluster {} triangle {} local indices ({}, {}, {}) not below "
				    "vertCount {} — 8-bit local index contract violated",
				    clusterIndex, k, static_cast<unsigned>(local[0]),
				    static_cast<unsigned>(local[1]), static_cast<unsigned>(local[2]), c.vertCount);
		float u[3], v[3];
		for (int j = 0; j < 3; ++j)
			readUv(asset, asset.remap[c.remapOffset + local[j]], u[j], v[j]);
		const int8_t w = uvTriangleHandedness(u[0], v[0], u[1], v[1], u[2], v[2]);
		++out.total;
		out.negative += (w == kMeshletHandNegative) ? 1 : 0;
		if (wantPerTriangle)
			out.perTriangle.push_back(w);
	}
	return std::nullopt;
}

}  // namespace

std::expected<MeshletHandResult, std::string> traverseClusterHandedness(
	const MeshletAsset& asset, uint32_t base, uint32_t count, MeshletHandFilter filter,
	bool wantPerTriangle) {
	// 区间前置（本模块自己的 API 边界，不是读取侧重复校验）：base + count 在 uint64 下判，
	// 防 uint32 加法绕回把越界区间洗成合法区间。
	const uint64_t end = static_cast<uint64_t>(base) + count;
	if (end > asset.clusters.size())
		return std::unexpected(fmt::format(
		    "meshlet hand: cluster range [{}, {}) out of bounds ({} clusters) — caller range "
		    "beyond the cluster table",
		    base, end, asset.clusters.size()));

	MeshletHandResult out;
	for (uint64_t i = base; i < end; ++i) {
		const MeshletDiskCluster& c = asset.clusters[i];
		if (filter == MeshletHandFilter::kL0Only && c.refined != -1)
			continue;  // 非 L0 簇**一条字节都不读**（单测用垃圾 tris 钉住这条）
		if (auto err = accumulateCluster(asset, c, i, wantPerTriangle, out))
			return std::unexpected(*err);
	}
	return out;
}

}  // namespace RGL
