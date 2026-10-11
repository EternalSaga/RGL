// serialize.cpp —— 按 format.h 的段表顺序落盘：头 → PrimTable → VertexPool → Clusters
// → Tris → Remap → Groups → Nodes，段间 16B 对齐填充。内存态与磁盘态字段一一对应
// （PackedCluster→DiskCluster 等），verifyOrig 不落盘（对照面只活在进程里）。
#include "serialize.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace RGL::meshpack {

namespace {

uint64_t alignUp(uint64_t v)
{
    return (v + kSectionAlign - 1) / kSectionAlign * kSectionAlign;
}

}  // namespace

void writeAsset(const std::string& path, const PackedAsset& a)
{
    // 先算段偏移（头里要写），再一次性落盘
    FileHeader h = {};
    std::memcpy(h.magic, kMagic, 8);
    h.formatVersion = kFormatVersion;
    h.toolVersion = kToolVersion;
    std::memcpy(h.sourceSha1, a.sourceSha1, 20);
    h.configFingerprint = a.configFingerprint;
    h.primCount = uint32_t(a.prims.size());
    h.vertexCount = a.vertexCount();
    h.clusterCount = uint32_t(a.clusters.size());
    h.groupCount = uint32_t(a.groups.size());
    h.nodeCount = uint32_t(a.nodes.size());
    h.levelCount = uint32_t(a.stats.levels);
    h.nodeWidth = kHierarchyNodeWidth;
    h.l0TriangleCount = a.stats.l0Tris;
    h.totalTriangleCount = a.stats.totalTris;

    uint64_t off = sizeof(FileHeader);
    h.offPrimTable = off; off = alignUp(off + a.prims.size() * sizeof(DiskPrim));
    h.offVertexPool = off; off = alignUp(off + a.vertexPool.size());
    h.offClusters = off; off = alignUp(off + a.clusters.size() * sizeof(DiskCluster));
    h.offTris = off; off = alignUp(off + a.tris.size());
    h.offRemap = off; off = alignUp(off + a.remap.size() * sizeof(uint32_t));
    h.offGroups = off; off = alignUp(off + a.groups.size() * sizeof(DiskGroup));
    h.offNodes = off; off = alignUp(off + a.nodes.size() * sizeof(DiskNode));

    std::vector<uint8_t> buf(size_t(off), 0);
    const auto put = [&](uint64_t at, const void* src, size_t n) {
        std::memcpy(buf.data() + size_t(at), src, n);
    };
    put(0, &h, sizeof h);

    {   // PrimTable
        std::vector<DiskPrim> t(a.prims.size());
        for (size_t i = 0; i < a.prims.size(); ++i)
            t[i] = {a.prims[i].vertexBase, a.prims[i].vertexCount, a.prims[i].indexCount, a.prims[i].material};
        put(h.offPrimTable, t.data(), t.size() * sizeof(DiskPrim));
    }
    put(h.offVertexPool, a.vertexPool.data(), a.vertexPool.size());
    {   // Clusters
        std::vector<DiskCluster> t(a.clusters.size());
        for (size_t i = 0; i < a.clusters.size(); ++i) {
            const PackedCluster& c = a.clusters[i];
            t[i] = {c.group, c.refined, c.triCount, c.vertCount, c.triOffset, c.remapOffset,
                {c.center[0], c.center[1], c.center[2]}, c.radius, c.error};
        }
        put(h.offClusters, t.data(), t.size() * sizeof(DiskCluster));
    }
    put(h.offTris, a.tris.data(), a.tris.size());
    put(h.offRemap, a.remap.data(), a.remap.size() * sizeof(uint32_t));
    {   // Groups
        std::vector<DiskGroup> t(a.groups.size());
        for (size_t i = 0; i < a.groups.size(); ++i) {
            const PackedGroup& g = a.groups[i];
            t[i] = {g.depth, g.prim, {g.center[0], g.center[1], g.center[2]}, g.radius, g.error,
                g.clusterOffset, g.clusterCount};
        }
        put(h.offGroups, t.data(), t.size() * sizeof(DiskGroup));
    }
    {   // Nodes
        std::vector<DiskNode> t(a.nodes.size());
        for (size_t i = 0; i < a.nodes.size(); ++i) {
            const PackedNode& n = a.nodes[i];
            t[i] = {{n.center[0], n.center[1], n.center[2]}, n.radius, n.error, n.group, n.childOffset, n.childCount};
        }
        put(h.offNodes, t.data(), t.size() * sizeof(DiskNode));
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("无法打开输出文件: " + path);
    const size_t written = std::fwrite(buf.data(), 1, buf.size(), f);
    const int err = std::fclose(f);
    if (written != buf.size() || err != 0) throw std::runtime_error("写输出失败: " + path);
}

}  // namespace RGL::meshpack
