// verify.cpp —— 产物自检三条 + 一条结构辅助。
//
// ## 检查项
//
//   ① **L0 无损不变量**：L0 簇解码出的三角多重集 == 输入三角多重集（全局顶点下标
//      三元组排序后逐位比；两侧先规范到最小顶点在前的循环旋转——起点轮换是
//      clusterizer/optimizeMeshlet 的合法行为，绕序与顶点集不变，细则见函数注释）。
//      这是"簇化不改几何"的唯一硬判据。
//   ② **8bit 局部索引可解回全局**：每簇局部索引 < vertCount ≤ 128、每个局部顶点
//      至少被引用一次、remap 互异且在池内，且 vertices[triangles[i]] == indices[i]
//      （clodLocalIndices 语义，对照面 = 打包时留存的 verifyOrig 原始全局索引）。
//   ③ **逐簇包围体包含该簇全部顶点**：留 1e-5 相对余量（float 半径 + double 重算
//      距离的舍入差远小于此）。
//   ④ **DAG 结构完整性**（为变异红线 ② 而设的辅助：① 只覆盖 L0，"某一级少算一组"
//      若发生在 depth>0，唯一能抓住它的是结构不变量——簇的顶点必须 ⊆ 其 refined
//      group 的顶点集，简化只在原顶点集上产索引表、永不引入新顶点；group 整体被
//      丢掉时全局下标自洽地左移，①②③ 全绿，只有这条红）。另含：每 group ≥1 簇、
//      depth==0 ⟺ refined==-1、refined 指向同 primitive 的更浅 group、非 terminal
//      group 必被 ≥1 簇 refined。
//
// ## 🔴 变异红线（每批交付的测试由主 agent 当场变异验红后才入账）
//
//   改坏一个簇索引 / 把某一级三角数少算一组 → --verify 必须红。两个变异都打在
//   packer.cpp 的回调里（注入点见该文件头注释）：
//     · 变异 A：某簇 triOffset 处写坏一个 8bit 局部索引 → ①（L0 时）+ ② 必须红。
//     · 变异 B：depth≥1 的某个 group 整组不产出 → ④ 必须红（①②③ 绿 = 预期形态，
//       ① 只保 L0；若变异落在 depth 0 则 ① 也红）。
//   验完红必须回滚回绿才算交付；**回滚只准 `cp f f.bak`，禁止 `git checkout --`**。
#include "verify.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <cfloat>

namespace RGL::meshpack {

namespace {

struct Triangle {
    uint32_t v[3];
    bool operator<(const Triangle& o) const
    {
        return std::lexicographical_compare(v, v + 3, o.v, o.v + 3);
    }
    bool operator==(const Triangle& o) const { return v[0] == o.v[0] && v[1] == o.v[1] && v[2] == o.v[2]; }
};

void report(std::FILE* out, bool ok, const char* name, const std::string& detail)
{
    std::fprintf(out, "[%s] %s%s%s\n", ok ? "PASS" : "FAIL", name, detail.empty() ? "" : ": ", detail.c_str());
}

// ① L0 三角多重集 == 输入三角多重集
//
// 口径：三元组按**有向三角形**比较（绕序与顶点集都相同 = 同一三角），比较前把两侧
// 都规范到**最小顶点在前的循环旋转**。允许循环旋转不是放松判据：clusterizer 从相邻
// 边访问三角形、optimizeMeshlet 重写起点，都会合法地轮换起点（绕序不变 ⟹ 光栅化的
// 三角形逐位同一）；spike 只比数量没比过三元组，第一次比三元组必须以此为准。
// 改坏任一顶点索引（变异 A）仍会破坏多重集 ⟹ 照红。
bool checkL0Lossless(const PackedAsset& a, const std::vector<SourcePrim>& prims, std::FILE* out)
{
    const auto canonical = [](Triangle t) {
        const int start = t.v[0] <= t.v[1] && t.v[0] <= t.v[2] ? 0 : (t.v[1] <= t.v[2] ? 1 : 2);
        return Triangle{{t.v[start], t.v[(start + 1) % 3], t.v[(start + 2) % 3]}};
    };
    std::vector<Triangle> fromClusters, fromInput;
    fromClusters.reserve(a.stats.l0Tris);
    fromInput.reserve(a.stats.sourceTris);

    for (const PackedCluster& c : a.clusters) {
        if (a.groups[c.group].depth != 0) continue;
        for (uint32_t t = 0; t < c.triCount; ++t) {
            const uint8_t* tri = a.tris.data() + c.triOffset + t * 3;
            fromClusters.push_back(canonical(Triangle{{a.remap[c.remapOffset + tri[0]], a.remap[c.remapOffset + tri[1]],
                a.remap[c.remapOffset + tri[2]]}}));
        }
    }
    for (size_t p = 0; p < prims.size(); ++p) {
        const uint32_t base = a.prims[p].vertexBase;
        for (size_t t = 0; t + 2 < prims[p].indices.size(); t += 3)
            fromInput.push_back(canonical(Triangle{{base + prims[p].indices[t], base + prims[p].indices[t + 1], base + prims[p].indices[t + 2]}}));
    }

    std::sort(fromClusters.begin(), fromClusters.end());
    std::sort(fromInput.begin(), fromInput.end());
    if (fromClusters.size() != fromInput.size()) {
        std::string d = "L0 三角数 " + std::to_string(fromClusters.size()) + " != 输入 " + std::to_string(fromInput.size());
        report(out, false, "① L0 三角多重集 = 输入三角多重集", d);
        return false;
    }
    for (size_t i = 0; i < fromClusters.size(); ++i)
        if (!(fromClusters[i] == fromInput[i])) {
            std::string d = "排序后第 " + std::to_string(i) + " 个三角不一致: (" + std::to_string(fromClusters[i].v[0]) + ","
                + std::to_string(fromClusters[i].v[1]) + "," + std::to_string(fromClusters[i].v[2]) + ") vs ("
                + std::to_string(fromInput[i].v[0]) + "," + std::to_string(fromInput[i].v[1]) + "," + std::to_string(fromInput[i].v[2]) + ")";
            report(out, false, "① L0 三角多重集 = 输入三角多重集", d);
            return false;
        }
    report(out, true, "① L0 三角多重集 = 输入三角多重集", std::to_string(fromClusters.size()) + " 三角逐位一致");
    return true;
}

// 逐角扫描（② 的角级不变量）：局部索引值域 + 解码对照（vertices[triangles[i]] ==
// indices[i]，对照 = 打包时留存的原始全局索引）；成功时填 seen[]（哪些局部顶点被引用）。
const char* checkClusterCorners(const PackedAsset& a, const PackedCluster& c, uint8_t (&seen)[128])
{
    for (uint32_t i = 0; i < c.triCount * 3; ++i) {
        const uint8_t local = a.tris[c.triOffset + i];
        if (local >= c.vertCount) return "8bit 局部索引 >= vertCount";
        seen[local] = 1;
        if (a.remap[c.remapOffset + local] != a.verifyOrig[c.origOffset + i]) return "解码全局下标 != 簇化器原始索引";
    }
    return nullptr;
}

// 返回 fail 串（空 = 绿）。单簇检查独立成函数（认知复杂度护栏逼的刀，缝 = 「一条不变量一个早返回」）：
// 计数契约 → 段内边界 → 局部索引值域 + 解码对照 → 顶点全覆盖 → remap 互异/在池内。
std::string checkOneCluster(const PackedAsset& a, size_t ci)
{
    const PackedCluster& c = a.clusters[ci];
    const auto bad = [&](const char* what) { return "cluster " + std::to_string(ci) + " " + what; };

    if (c.vertCount == 0 || c.vertCount > 128) return bad("顶点计数越出 1..128 契约");
    if (c.triCount == 0 || c.triCount > 128) return bad("三角计数越出 1..128 契约");
    if (size_t(c.triOffset) + size_t(c.triCount) * 3 > a.tris.size()) return bad("triOffset 越出 Tris 段");
    if (c.remapOffset + c.vertCount > a.remap.size()) return bad("remapOffset 越出 Remap 段");

    uint8_t seen[128] = {};
    if (const char* what = checkClusterCorners(a, c, seen)) return bad(what);
    for (uint32_t v = 0; v < c.vertCount; ++v)
        if (!seen[v]) return bad("存在未被任何三角引用的局部顶点（vertices[] 不唯一/不全）");

    // remap 互异性：排序后查相邻重复（vertCount ≤ 128，逐对 O(n²) 在 6 万簇上会
    // 爬到 1e9 次比较，不许）
    std::vector<uint32_t> globals(a.remap.begin() + c.remapOffset, a.remap.begin() + c.remapOffset + c.vertCount);
    std::sort(globals.begin(), globals.end());
    if (std::adjacent_find(globals.begin(), globals.end()) != globals.end()) return bad("remap 顶点不唯一");
    if (globals.back() >= a.vertexCount()) return bad("remap 全局下标越出顶点池");
    return {};
}

bool checkLocalIndices(const PackedAsset& a, std::FILE* out)
{
    std::string fail;
    for (size_t ci = 0; ci < a.clusters.size() && fail.empty(); ++ci) fail = checkOneCluster(a, ci);
    report(out, fail.empty(), "② 每簇 8bit 局部索引可解回全局顶点", fail.empty() ? std::to_string(a.clusters.size()) + " 簇全部可解" : fail);
    return fail.empty();
}

// ③ 逐簇包围体包含该簇全部顶点（1e-5 相对余量 + float32 表示噪声地板）
//
// 容差 = radius × 1e-5（任务口径的语义余量）+ 2 ulp(坐标量级)（表示噪声地板，不是
// 放松）：meshopt 的包围球（meshletutils.cpp computeBoundingSphere<7>，近似球）中心
// 与半径都是 float32，存储中心相对内部计算值最多差半个 ulp；bistro 源单位坐标到
// ±8000 量级，ulp(2576) = 2.4e-4，double 重算距离超存储半径 ~1 ulp(中心) 纯属舍入
// （实测最大超出 4.7e-5 绝对 / 1.5e-5 相对）。改坏顶点索引（变异 A）把顶点挪到
// O(簇尺度) 之外，远超本地板 ⟹ 照红。
bool checkBounds(const PackedAsset& a, std::FILE* out)
{
    std::string fail;
    for (size_t ci = 0; ci < a.clusters.size() && fail.empty(); ++ci) {
        const PackedCluster& c = a.clusters[ci];
        for (uint32_t v = 0; v < c.vertCount; ++v) {
            const float* p = a.vertexPos(a.remap[c.remapOffset + v]);
            const double dx = double(p[0]) - c.center[0], dy = double(p[1]) - c.center[1], dz = double(p[2]) - c.center[2];
            const double dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            double scale = 1.0;
            for (int k = 0; k < 3; ++k) scale = std::max({scale, double(std::fabs(c.center[k])), double(std::fabs(p[k]))});
            const double tol = double(c.radius) * 1e-5 + 2.0 * FLT_EPSILON * scale;
            if (dist > double(c.radius) + tol) {
                fail = "cluster " + std::to_string(ci) + " 顶点 " + std::to_string(v) + " 距球心 " + std::to_string(dist)
                    + " > 半径 " + std::to_string(c.radius) + " + 容差 " + std::to_string(tol);
                break;
            }
        }
    }
    report(out, fail.empty(), "③ 逐簇包围体包含该簇全部顶点（1e-5 相对 + float32 噪声地板）", fail.empty() ? "全部包含" : fail);
    return fail.empty();
}

// ④ DAG 结构完整性（变异红线 ② 的抓手，理由见文件头）
//
// 拆四刀（认知复杂度 64 > 30 ERROR 逼的，缝 = **一条结构不变量一个函数**，fail 串
// 是唯一判决载体；checkDag 退化为按依赖顺序的编排——ranges 的「簇区间连续」是后三条
// 按下标直取簇的前提，顺序不许换）：
//   · checkGroupRanges      每 group ≥1 簇 + 簇区间连续覆盖 + prim 下标有效
//   · checkRefinedSemantics depth/refined 互推规则 + refinedBy 计数
//   · checkTerminalRefined  非 terminal group 必被 ≥1 簇 refined
//   · checkVertexSubset     簇顶点 ⊆ refined group 顶点集（group 整组丢失 → 下标
//                           左移指到邻居 group 时，这条是唯一会红的抓手）

// 返回 fail 串（空 = 绿）。以下四个 helper 同此约定。
std::string checkGroupRanges(const PackedAsset& a)
{
    uint32_t cursor = 0;
    for (size_t gi = 0; gi < a.groups.size(); ++gi) {
        const PackedGroup& g = a.groups[gi];
        const std::string where = "group " + std::to_string(gi) + " ";
        if (g.clusterCount == 0) return where + "不含任何簇（某一级少算一组的形态）";
        if (g.clusterOffset != cursor || cursor + g.clusterCount > a.clusters.size()) return where + "簇区间不连续/越界";
        for (uint32_t k = 0; k < g.clusterCount; ++k)
            if (a.clusters[cursor + k].group != gi) return "cluster 的 group 字段与所在 group 不符";
        cursor += g.clusterCount;
        if (g.prim >= a.prims.size()) return where + "的 primitive 下标越界";
    }
    if (cursor != a.clusters.size()) return "group 簇区间未覆盖全部簇";
    return {};
}

std::string checkRefinedSemantics(const PackedAsset& a, std::vector<uint32_t>& refinedBy)
{
    for (size_t ci = 0; ci < a.clusters.size(); ++ci) {
        const PackedCluster& c = a.clusters[ci];
        const PackedGroup& g = a.groups[c.group];
        const std::string where = "cluster " + std::to_string(ci) + " ";
        if (g.depth == 0 && c.refined != -1) return where + "L0 簇的 refined 应为 -1";
        if (g.depth > 0 && c.refined < 0) return where + "depth>0 但 refined==-1（group 被整组丢掉的形态）";
        if (c.refined < 0) continue;
        if (size_t(c.refined) >= a.groups.size()) return where + "refined 越界";
        const PackedGroup& r = a.groups[size_t(c.refined)];
        if (r.depth >= g.depth) return where + "refined group 不比本簇 group 更浅";
        if (r.prim != g.prim) return where + "refined group 跨 primitive";
        refinedBy[size_t(c.refined)]++;
    }
    return {};
}

std::string checkTerminalRefined(const PackedAsset& a, const std::vector<uint32_t>& refinedBy)
{
    for (size_t gi = 0; gi < a.groups.size(); ++gi)
        if (a.groups[gi].error < FLT_MAX && refinedBy[gi] == 0)
            return "group " + std::to_string(gi) + " 非 terminal 却无任何簇由它产出";
    return {};
}

std::string checkVertexSubset(const PackedAsset& a)
{
    // 每 group 的顶点集 = 其全部簇 remap 的去重并（简化永不引入新顶点）
    std::vector<std::vector<uint32_t>> groupVerts(a.groups.size());
    for (size_t gi = 0; gi < a.groups.size(); ++gi) {
        const PackedGroup& g = a.groups[gi];
        std::vector<uint32_t>& set = groupVerts[gi];
        for (uint32_t k = 0; k < g.clusterCount; ++k) {
            const PackedCluster& c = a.clusters[g.clusterOffset + k];
            set.insert(set.end(), a.remap.begin() + c.remapOffset, a.remap.begin() + c.remapOffset + c.vertCount);
        }
        std::sort(set.begin(), set.end());
        set.erase(std::unique(set.begin(), set.end()), set.end());
    }
    for (size_t ci = 0; ci < a.clusters.size(); ++ci) {
        const PackedCluster& c = a.clusters[ci];
        if (c.refined < 0) continue;
        const std::vector<uint32_t>& set = groupVerts[size_t(c.refined)];
        for (uint32_t v = 0; v < c.vertCount; ++v)
            if (!std::binary_search(set.begin(), set.end(), a.remap[c.remapOffset + v]))
                return "cluster " + std::to_string(ci) + " 的顶点不在其 refined group 的顶点集内（该级少了一组 / refined 指错）";
    }
    return {};
}

bool checkDag(const PackedAsset& a, std::FILE* out)
{
    std::vector<uint32_t> refinedBy(a.groups.size(), 0);
    std::string fail = checkGroupRanges(a);
    if (fail.empty()) fail = checkRefinedSemantics(a, refinedBy);
    if (fail.empty()) fail = checkTerminalRefined(a, refinedBy);
    if (fail.empty()) fail = checkVertexSubset(a);
    report(out, fail.empty(), "④ DAG 结构完整性（含簇顶点 ⊆ refined group 顶点集）", fail.empty()
        ? std::to_string(a.groups.size()) + " group / " + std::to_string(a.clusters.size()) + " 簇结构自洽"
        : fail);
    return fail.empty();
}

}  // namespace

bool verifyAsset(const PackedAsset& asset, const std::vector<SourcePrim>& prims, std::FILE* out)
{
    bool ok = true;
    ok = checkL0Lossless(asset, prims, out) && ok;
    ok = checkLocalIndices(asset, out) && ok;
    ok = checkBounds(asset, out) && ok;
    ok = checkDag(asset, out) && ok;
    std::fprintf(out, "%s\n", ok ? "verify: 全绿" : "verify: 有红项");
    return ok;
}

}  // namespace RGL::meshpack
