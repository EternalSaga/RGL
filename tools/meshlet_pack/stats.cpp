// stats.cpp —— 统计表打印。MB = 1e6 字节（§7.2 的口径，不是 MiB）。
// UV 手性读数（本 TU 只读，不碰打包数值路径）：与引擎 loader **同法**——
// rgl/gltf_loader_impl.cppm 的 uvTriangleHandedness：UV 空间边行列式
// det = s1x·s2y − s2x·s1y（s1 = uv[i1]−uv[i0]，s2 = uv[i2]−uv[i0]），det < 0 = 负手性；
// 逐三角形。loader 只在 hasTangent && hasUv 时逐三角推 w、否则恒 +1 ⟹ 分母只数
// 双属性齐备的 primitive 的三角（缺失形态的 prim 恒正，计进去会稀释读数）。
#include "stats.h"

#include <cstddef>
#include <cstdint>

namespace RGL::meshpack {

namespace {

double mb(uint64_t bytes) { return double(bytes) / 1e6; }

const float* uvOf(const SourcePrim& p, uint32_t v)
{
    return reinterpret_cast<const float*>(p.vertices.data() + size_t(v) * kVertexStride + 24);
}

struct HandStat {
    uint64_t total = 0, negative = 0;
};

HandStat uvHandedness(const std::vector<SourcePrim>& prims)
{
    HandStat h;
    for (const SourcePrim& p : prims) {
        if (!p.hasUv || !p.hasTangent) continue;
        for (size_t t = 0; t + 2 < p.indices.size(); t += 3) {
            const float* u0 = uvOf(p, p.indices[t]);
            const float* u1 = uvOf(p, p.indices[t + 1]);
            const float* u2 = uvOf(p, p.indices[t + 2]);
            const float s1x = u1[0] - u0[0], s1y = u1[1] - u0[1];
            const float s2x = u2[0] - u0[0], s2y = u2[1] - u0[1];
            ++h.total;
            if (s1x * s2y - s2x * s1y < 0.0f) ++h.negative;
        }
    }
    return h;
}

}  // namespace

void printStats(const PackedAsset& a, const std::vector<SourcePrim>& prims, const char* sourcePath, std::FILE* out)
{
    const PackStats& s = a.stats;
    std::fprintf(out, "== meshlet_pack stats: %s ==\n", sourcePath);
    std::fprintf(out, "config            clodDefaultConfig(128) 全默认 (fingerprint 0x%08x)\n", a.configFingerprint);
    std::fprintf(out, "source            %llu prims  %llu verts  %llu tris\n",
        (unsigned long long)s.prims, (unsigned long long)s.sourceVerts, (unsigned long long)s.sourceTris);
    std::fprintf(out, "L0 clusters         %llu   total clusters  %llu\n",
        (unsigned long long)s.l0Clusters, (unsigned long long)s.totalClusters);
    std::fprintf(out, "groups              %llu   levels            %llu   hierarchy nodes %llu (node_width=%u)\n",
        (unsigned long long)s.groups, (unsigned long long)s.levels, (unsigned long long)s.nodes, kHierarchyNodeWidth);
    std::fprintf(out, "L0 triangles        %llu (", (unsigned long long)s.l0Tris);
    if (s.sourceTris) std::fprintf(out, "%.2f%%", 100.0 * double(s.l0Tris) / double(s.sourceTris));
    std::fprintf(out, " of input %llu)\n", (unsigned long long)s.sourceTris);

    std::fprintf(out, "per-level tris    ");
    for (uint64_t t : s.levelTris) std::fprintf(out, " %llu", (unsigned long long)t);
    std::fprintf(out, "\nper-level clusters");
    for (uint64_t c : s.levelClusters) std::fprintf(out, " %llu", (unsigned long long)c);
    std::fprintf(out, "\n");

    std::fprintf(out, "L0 shape            avg %.1f tris/cluster  median fill %.1f%%\n", s.l0AvgTris, s.l0MedianFill * 100.0);
    std::fprintf(out, "L0 bounds radius  p50 %.2fm  p90 %.2fm  p99 %.2fm  max %.2fm (world)\n",
        s.l0RadiusP50, s.l0RadiusP90, s.l0RadiusP99, s.l0RadiusMax);
    const HandStat hand = uvHandedness(prims);
    std::fprintf(out, "uv handedness     %.2f%% negative (%llu / %llu tris, loader 同法逐三角 Jacobian; 分母 = tangent+uv 齐备的 prim)\n",
        hand.total ? 100.0 * double(hand.negative) / double(hand.total) : 0.0,
        (unsigned long long)hand.negative, (unsigned long long)hand.total);

    // ─── shadow proxy N10 P1：must_L0 判据读数（对账面 = tools/shadow_proxy_probe.py 的数 6）──
    // 🔴 三个承重读数（must_L0 分量数 / 被钉 group 数 / 额外 L0 三角）是**移植正确性判据**：
    // 门槛 100 源单位（= bistro 世界 1m）下应复现探针在 v1 产物上量到的 6,672 / 18 / 16,380。
    // 对不上就是移植错了（口径漂移），**不许调判据去凑**——components.h 头注记着四条曾量错过的坑。
    const MustL0Stats& m = a.mustL0;
    std::fprintf(out, "must_L0 diag          %.6g 源单位%s\n", a.mustL0Diag,
        a.mustL0Diag > 0.0 ? "" : "  (0 = 关闭判定：不排序、MustL0 段为空、簇序与 v1 逐位相同)");
    std::fprintf(out, "components            %llu  (位置焊接后的连通分量，逐 prim 内跑)\n", (unsigned long long)m.components);
    std::fprintf(out, "  must_L0 components  %llu  (死亡级==1 且包围对角线 >= 门槛)\n", (unsigned long long)m.mustL0Components);
    std::fprintf(out, "  pinned groups       %llu / %llu L0 group  (prim %llu → MustL0 段 %zu 条)\n",
        (unsigned long long)m.pinnedGroups, (unsigned long long)m.l0Groups,
        (unsigned long long)m.primsAffected, a.mustL0Ranges.size());
    std::fprintf(out, "  extra L0 tris       %llu  (", (unsigned long long)m.pinnedTris);
    if (s.l0Tris) std::fprintf(out, "%.2f%%", 100.0 * double(m.pinnedTris) / double(s.l0Tris));
    std::fprintf(out, " of L0 %llu)\n", (unsigned long long)s.l0Tris);

    const uint64_t total = s.poolBytes + s.l0IndexBytes + s.remapBytes + s.otherIndexBytes;
    std::fprintf(out, "data plane:\n");
    std::fprintf(out, "  vertex pool     %7.1f MB  (%llu x %uB)\n", mb(s.poolBytes), (unsigned long long)a.vertexCount(), kVertexStride);
    std::fprintf(out, "  L0 indices      %7.1f MB  (8bit)\n", mb(s.l0IndexBytes));
    std::fprintf(out, "  vertex remap    %7.1f MB  (32bit, L0)\n", mb(s.remapBytes));
    std::fprintf(out, "  other levels    %7.1f MB  (%llu tris, 8bit)\n", mb(s.otherIndexBytes),
        (unsigned long long)(s.totalTris - s.l0Tris));
    std::fprintf(out, "  total           %7.1f MB  vs deindexed %.1f MB  [§7.2 口径]\n", mb(total), mb(s.sourceTris * 3 * kVertexStride));
    std::fprintf(out, "  other-level remap %5.1f MB  (32bit; §7.2 的 167MB 未计, 全量合计 %.1f MB)\n",
        mb(s.otherRemapBytes), mb(total + s.otherRemapBytes));
}

}  // namespace RGL::meshpack
