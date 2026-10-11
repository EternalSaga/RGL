// 簇化实现 TU：meshoptimizer 库 + vendored clusterlod.h 的**唯一**实现单元
// （CLUSTERLOD_IMPLEMENTATION 只能有一个家，其余 TU 只见 packer.h）。
//
// 变异红线（验收必做，全文见 verify.cpp 头注释）：本文件是两处变异的注入点——
// 改坏一个簇索引 / 某一级少算一组，--verify 必须红。
#include "packer.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <cmath>

#include <meshoptimizer.h>

#define CLUSTERLOD_IMPLEMENTATION
#include "clusterlod.h"

namespace RGL::meshpack {

namespace {

// 转换配置 = clodDefaultConfig(128) **全默认，一个字段都不改**（§9 ★0-a 判据原文：
// max_vertices/max_triangles=128、min_triangles=42、partition_size=16、
// simplify_ratio=0.5、threshold=0.85、permissive=true）。数字对拍靠它，别调参。
const clodConfig& pinnedConfig()
{
    static const clodConfig cfg = clodDefaultConfig(128);
    return cfg;
}

[[noreturn]] void die(const char* what)
{
    std::fprintf(stderr, "meshlet_pack: %s\n", what);
    std::abort();
}

// FNV-1a（32bit）逐字段喂规范化字节；口径见 packer.h 声明处。
struct Fnv {
    uint32_t h = 2166136261u;
    void bytes(const void* p, size_t n)
    {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 16777619u; }
    }
    template <typename T> void field(T v) { bytes(&v, sizeof v); }
    void boolean(bool v) { field<uint8_t>(v ? 1 : 0); }
    void real(float v)
    {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        field(bits);
    }
};

}  // namespace

uint32_t configFingerprint()
{
    const clodConfig& c = pinnedConfig();
    Fnv f;
    f.field<uint64_t>(128);  // clodDefaultConfig 的 max_triangles 入参（钉死 128）
    f.field<uint64_t>(c.max_vertices);
    f.field<uint64_t>(c.min_triangles);
    f.field<uint64_t>(c.max_triangles);
    f.boolean(c.partition_spatial);
    f.boolean(c.partition_sort);
    f.field<uint64_t>(c.partition_size);
    f.boolean(c.cluster_spatial);
    f.real(c.cluster_fill_weight);
    f.real(c.cluster_split_factor);
    f.real(c.simplify_ratio);
    f.real(c.simplify_threshold);
    f.real(c.simplify_error_merge_previous);
    f.real(c.simplify_error_merge_additive);
    f.real(c.simplify_error_factor_sloppy);
    f.real(c.simplify_error_edge_limit);
    f.boolean(c.simplify_error_clamped);
    f.boolean(c.simplify_permissive);
    f.boolean(c.simplify_fallback_permissive);
    f.boolean(c.simplify_fallback_sloppy);
    f.boolean(c.simplify_regularize);
    f.boolean(c.simplify_preserve_folds);
    f.boolean(c.simplify_dilate_borders);
    f.boolean(c.optimize_bounds);
    f.boolean(c.optimize_clusters);
    f.field<int32_t>(c.optimize_clusters_level);
    return f.h;
}

const float* PackedAsset::vertexPos(uint32_t globalIndex) const
{
    return reinterpret_cast<const float*>(vertexPool.data() + size_t(globalIndex) * kVertexStride);
}

PackedAsset packAsset(const std::vector<SourcePrim>& prims, const uint8_t sourceSha1[20])
{
    PackedAsset out;
    std::memcpy(out.sourceSha1, sourceSha1, 20);
    out.configFingerprint = configFingerprint();
    const clodConfig& cfg = pinnedConfig();

    // 层级构建要 clodGroup 数组（与 out.groups 平行同序；字段是它的磁盘化投影）
    std::vector<clodGroup> clodGroups;
    std::vector<uint32_t> localVerts;  // clodLocalIndices scratch（跨簇/跨 prim 复用）
    std::vector<uint8_t> localTris;
    std::vector<float> densePositions;  // stride=12 位置 scratch（理由见循环内注释）

    int maxDepth = -1;

    // TODO(parallel): 本循环各 primitive 相互独立（全局下标 = 前缀和，可先算），可分
    // 段并行 + 末尾收口；单线程全场景 ~2.8s（§7.1），本批不做。
    for (size_t p = 0; p < prims.size(); ++p) {
        const SourcePrim& prim = prims[p];
        const uint32_t vertexBase = out.vertexCount();
        out.prims.push_back({vertexBase, prim.vertexCount, uint32_t(prim.indices.size()), prim.material});
        out.stats.sourceVerts += prim.vertexCount;
        out.stats.sourceTris += prim.indices.size() / 3;
        // 池逐 prim 增量拼接（vertexBase 必须在此刻取，早于任何"池已含本 prim"的假设）
        const size_t poolOld = out.vertexPool.size();
        out.vertexPool.resize(poolOld + prim.vertices.size());
        std::memcpy(out.vertexPool.data() + poolOld, prim.vertices.data(), prim.vertices.size());
        if (prim.indices.empty()) continue;  // 空 primitive：无三角可簇化（bistro 无此形态）

        // 位置喂**稠密 stride=12 数组**（= spike 的浮点输入路径）：meshoptimizer 的
        // clusterizer SIMD 路径只在 stride==12 时走，stride=48 落标量回退，浮点累加
        // 序不同会翻转比较位 ⟹ 簇数与 §7.2 对不上（实测 61,518 vs 61,547）。
        densePositions.resize(prim.vertexCount * 3);
        for (uint32_t v = 0; v < prim.vertexCount; ++v)
            std::memcpy(densePositions.data() + size_t(v) * 3, prim.vertices.data() + size_t(v) * kVertexStride, 12);

        clodMesh mesh = {};
        mesh.indices = prim.indices.data();
        mesh.index_count = prim.indices.size();
        mesh.vertex_count = prim.vertexCount;
        mesh.vertex_positions = densePositions.data();
        mesh.vertex_positions_stride = sizeof(float) * 3;

        clodBuild(cfg, mesh, [&](clodGroup group, const clodCluster* clusters, size_t clusterCount) -> int {
            // 回调返回值被库原样记进新簇的 refined（= 全局 group 下标，**不可再叠加偏移**）
            const int gid = int(out.groups.size());
            out.groups.push_back({group.depth, uint32_t(p),
                {group.simplified.center[0], group.simplified.center[1], group.simplified.center[2]},
                group.simplified.radius, group.simplified.error,
                uint32_t(out.clusters.size()), uint32_t(clusterCount)});
            clodGroups.push_back(group);
            maxDepth = std::max(maxDepth, group.depth);
            if (out.stats.levelTris.size() <= size_t(group.depth)) {
                out.stats.levelTris.resize(size_t(group.depth) + 1, 0);
                out.stats.levelClusters.resize(size_t(group.depth) + 1, 0);
            }

            for (size_t k = 0; k < clusterCount; ++k) {
                const clodCluster& cl = clusters[k];
                const uint32_t triCount = uint32_t(cl.index_count / 3);
                if (triCount > 128 || cl.vertex_count > 128) die("簇超出 128 三角/128 顶点契约");
                localVerts.resize(cl.vertex_count);
                localTris.resize(cl.index_count);
                const size_t vc = clodLocalIndices(localVerts.data(), localTris.data(), cl.indices, cl.index_count);
                if (vc != cl.vertex_count) die("clodLocalIndices 与 clodCluster::vertex_count 不一致");

                const uint32_t triOffset = uint32_t(out.tris.size());
                const uint32_t remapOffset = uint32_t(out.remap.size());
                const uint32_t origOffset = uint32_t(out.verifyOrig.size());
                out.tris.insert(out.tris.end(), localTris.begin(), localTris.begin() + cl.index_count);
                for (size_t v = 0; v < vc; ++v) out.remap.push_back(vertexBase + localVerts[v]);
                for (size_t i = 0; i < cl.index_count; ++i) out.verifyOrig.push_back(vertexBase + cl.indices[i]);

                out.clusters.push_back({uint32_t(gid), cl.refined, triCount, uint32_t(vc), triOffset, remapOffset,
                    origOffset,
                    {cl.bounds.center[0], cl.bounds.center[1], cl.bounds.center[2]}, cl.bounds.radius, cl.bounds.error});

                out.stats.levelTris[size_t(group.depth)] += triCount;
                out.stats.levelClusters[size_t(group.depth)]++;
            }
            return gid;
        });
    }

    // 空间层级：全局建一次（node_width=8 = §7.2 的 8,705 节点口径）。
    // clodNode → PackedNode 逐字段搬（两者布局同为 32B，但类型不同，不做类型双关）。
    const size_t levels = out.stats.levels = size_t(maxDepth) + 1;
    std::vector<clodNode> clodNodes(clodBuildHierarchyBound(out.groups.size(), kHierarchyNodeWidth, levels));
    const size_t nodeCount = clodBuildHierarchy(
        clodNodes.data(), clodGroups.data(), out.groups.size(), kHierarchyNodeWidth, levels);
    out.nodes.resize(nodeCount);
    for (size_t n = 0; n < nodeCount; ++n) {
        const clodNode& nd = clodNodes[n];
        out.nodes[n] = {{nd.bounds.center[0], nd.bounds.center[1], nd.bounds.center[2]},
            nd.bounds.radius, nd.bounds.error, nd.group, nd.child_offset, nd.child_count};
    }

    // 统计收口（口径逐项对 §7.2）
    PackStats& s = out.stats;
    s.prims = uint32_t(prims.size());
    s.groups = out.groups.size();
    s.nodes = out.nodes.size();
    s.totalClusters = out.clusters.size();
    for (size_t c = 0; c < out.clusters.size(); ++c) {
        const PackedCluster& cl = out.clusters[c];
        s.totalTris += cl.triCount;
        if (out.groups[cl.group].depth == 0) s.l0Clusters++, s.l0Tris += cl.triCount, s.l0Verts += cl.vertCount;
    }
    s.poolBytes = out.vertexPool.size();
    s.l0IndexBytes = s.l0Tris * 3;
    // §7.2 的「重映射 11.9MB」口径 = **L0** 簇的 remap（其余级的 remap 未计入那张表的
    // 167MB——本工具格式对全级都存 remap，故拆两行如实报，不并入凑数）
    s.remapBytes = s.l0Verts * 4;
    s.otherRemapBytes = (out.remap.size() - s.l0Verts) * 4;
    s.otherIndexBytes = (s.totalTris - s.l0Tris) * 3;

    std::vector<float> l0Radii;
    std::vector<float> l0Fill;
    l0Radii.reserve(s.l0Clusters);
    l0Fill.reserve(s.l0Clusters);
    for (const PackedCluster& cl : out.clusters)
        if (out.groups[cl.group].depth == 0) {
            l0Radii.push_back(cl.radius * kWorldScale);
            l0Fill.push_back(float(cl.triCount) / 128.0f);
        }
    const auto pct = [](std::vector<float>& sorted, float q) { return sorted.empty() ? 0.f : sorted[size_t(q * float(sorted.size() - 1))]; };
    std::sort(l0Radii.begin(), l0Radii.end());
    s.l0RadiusP50 = pct(l0Radii, 0.5f);
    s.l0RadiusP90 = pct(l0Radii, 0.9f);
    s.l0RadiusP99 = pct(l0Radii, 0.99f);
    s.l0RadiusMax = l0Radii.empty() ? 0.f : l0Radii.back();
    std::sort(l0Fill.begin(), l0Fill.end());
    s.l0MedianFill = pct(l0Fill, 0.5f);
    s.l0AvgTris = s.l0Clusters ? double(s.l0Tris) / double(s.l0Clusters) : 0.0;

    return out;
}

}  // namespace RGL::meshpack
