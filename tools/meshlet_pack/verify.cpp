// verify.cpp —— 产物自检三条 + 一条结构辅助 + 五条 must_L0 重排 invariant（N10 P1）。
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
//   ⑤ **分量划分互不相交、并集 = 该 prim 全部三角**（计划 §3.6 R5 的打包侧对应物）
//   ⑥ **每 prim 的 depth 序列仍单调不减**（= 引擎侧 meshlet_lod invariant 2 的对应物）
//   ⑦ **每 group 的簇仍连续、且集合与重排前逐位相同**（"只搬条目不改成员"的机器形态）
//   ⑧ **groups 数组顺序与重排前逐位相同**（refined 存的是它的下标，动不得）
//   ⑨ **MustL0 区间表与重排后的产物自洽**（判据重跑 + 被钉段是各 prim L0 段的前缀）
//
//   ⑤~⑨ 是 N10 P1（L0 段 group 重排）新增的五条，逐条的"为什么只有它能红"见函数注释。
//
// ## 🔴 变异红线（每批交付的测试由主 agent 当场变异验红后才入账）
//
//   改坏一个簇索引 / 把某一级三角数少算一组 → --verify 必须红。两个变异都打在
//   packer.cpp 的回调里（注入点见该文件头注释）：
//     · 变异 A：某簇 triOffset 处写坏一个 8bit 局部索引 → ①（L0 时）+ ② 必须红。
//     · 变异 B：depth≥1 的某个 group 整组不产出 → ④ 必须红（①②③ 绿 = 预期形态，
//       ① 只保 L0；若变异落在 depth 0 则 ① 也红）。
//   N10 P1 追加四条（验"判据真的在起作用"，不是验"能跑"；四条 2026-10-20 全部**实测**，
//   注入点见 components.cpp 头注；每条还原后都重跑过基线确认回绿）：
//     · 变异 C：关掉位置焊接（WeldTable::insert 里 memcmp 命中后不 return）→ 三个判据读数
//       一起变差：分量 145,828 → **282,242**（与 Python 探针 --no-weld 同口径实测逐位对上）、
//       must_L0 196 → 1,678、被钉 group 18 → 316、额外 L0 三角 16,380 → 329,958。
//       verify 九条**全绿 = 预期**（它验的是划分自洽，不是划分正确 —— 正确性由 C 的读数对账钉）。
//     · 变异 D1：stable 二分的两半对调（must_L0 排到最后）→ **只有 ⑨ 红**（"区间内第 0 个簇的
//       group 未被判据钉住"）。⑥⑦⑧ 全绿同样是预期：簇集合、depth 序列、groups 顺序都没变，
//       变的只是段内先后 —— ⑨ 是"前缀性"这件事**唯一**的抓手。
//     · 变异 D2：同 prim 内跨级对调两簇（52472 ↔ 52473，即把 depth==0 段最后一簇与 depth==1
//       段第一簇换位置）→ ④⑥⑦ 三条一起红，⑥ 点名"prim 1513 的 depth 序列在簇 52473 处由 1
//       降到 0"。⚠ 单靠"两半对调"红不了 ⑥（段内全是 depth 0），⑥ 只在**搬出段外**时红。
//     · 门槛 300 源单位（= 世界 3m）→ 被钉 group 数 **0**（探针数 6 同档实测也是 0）：
//       验"读数随门槛动"，排除"判据其实恒输出同一个数"。
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

// 返回 fail 串（空 = 绿）。以下 helper 同此约定。
//
// 逐 group：≥1 簇 + 区间不越界 + 区间内每个簇的 group 字段指回来（= 该 group 的簇是**一段连续
// 区间**）+ prim 下标有效；顺带收集区间供铺满检查（拆两刀 = 认知复杂度护栏逼的，缝在"逐 group"
// 与"跨 group"之间）。
std::string checkEachGroupRange(const PackedAsset& a, std::vector<std::pair<uint64_t, uint64_t>>& spans)
{
    for (size_t gi = 0; gi < a.groups.size(); ++gi) {
        const PackedGroup& g = a.groups[gi];
        const std::string where = "group " + std::to_string(gi) + " ";
        if (g.clusterCount == 0) return where + "不含任何簇（某一级少算一组的形态）";
        if (uint64_t(g.clusterOffset) + g.clusterCount > a.clusters.size()) return where + "簇区间越界";
        for (uint32_t k = 0; k < g.clusterCount; ++k)
            if (a.clusters[g.clusterOffset + k].group != gi)
                return where + "的簇区间里混进别的 group 的簇（簇不连续 / 搬错家）";
        if (g.prim >= a.prims.size()) return where + "的 primitive 下标越界";
        spans.emplace_back(g.clusterOffset, g.clusterCount);
    }
    return {};
}

// 铺满 [0, clusterCount)：按**起点排序后**首尾相接。
// 🔴 口径变更（N10 P1）：旧版是"按 group 下标序首尾相接"，那条比引擎真正吃的更强（引擎侧
// meshlet_ranges::scanPrimRange 自己按起点 sort，只要求"每 prim 一个连续区间"）。而 L0 段
// 重排**合法地**让 prim 内 group 下标序 ≠ 簇序——groups 顺序动不得（refined 吃它），被钉的
// 那批 group 天然不连续。判据因此取引擎吃的那条：不重叠、无缝隙、铺满。
// ⚠ 变异 B（group 整组不产出）不由这条抓、也从来不是由这条抓的，仍归 checkTerminalRefined /
// checkVertexSubset ⟹ 本条放松不削弱变异红线。
std::string checkRangeTiling(const PackedAsset& a, std::vector<std::pair<uint64_t, uint64_t>>& spans)
{
    std::sort(spans.begin(), spans.end());
    uint64_t cursor = 0;
    for (const auto& [start, count] : spans) {
        if (start != cursor)
            return "group 簇区间未铺满全表：起点 " + std::to_string(start) + " != " + std::to_string(cursor)
                + "（重叠或有缝）";
        cursor += count;
    }
    if (cursor != a.clusters.size()) return "group 簇区间未覆盖全部簇";
    return {};
}

std::string checkGroupRanges(const PackedAsset& a)
{
    std::vector<std::pair<uint64_t, uint64_t>> spans;
    spans.reserve(a.groups.size());
    std::string fail = checkEachGroupRange(a, spans);
    return fail.empty() ? checkRangeTiling(a, spans) : fail;
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

// ─── ⑤⑥⑦⑧⑨ shadow proxy N10 P1：分量划分与 L0 段重排的 invariant ──────────────────
//
// 五条各一条**两两不同**的错误串、前缀点名（形态照 rgl/meshlet_lod.cppm 的五条 invariant：
// 它们是"产物或判据漂了"的探测器，不是审美）。⑤ 与 ⑨ 内部的子判据同样各给一条串——一条
// invariant 塌成一条串就没法归因是"漏数"还是"重数"。

// ⑤ 分量划分互不相交、并集 = 该 prim 全部三角（计划 §3.6 R5 的打包侧对应物）
//
// 三条独立对照：
//   · Σ 分量三角 == 分析看到的 L0 三角 —— 划分不重不漏（互不相交由并查集**构造性**成立，
//     这条抓的是归属计数那一侧：漏一个三角、同一三角数两遍都会让它分家）
//   · 分析看到的 L0 三角 == stats.l0Tris —— 两条**独立** L0 口径互证（stats 走 group.depth==0
//     累加 triCount，分析走 clusters→tris→remap 逐三角解码；同 skill rgl-test-discipline
//     "独立复算同一量"那条：本探针四条量错里三条靠这个抓出来的）
//   · 三顶点跨分量的 L0 三角 == 0 —— "取首顶点当整三角归属"的前提（与探针同口径）
std::string checkComponentPartition(const PackedAsset& a)
{
    const MustL0Stats& m = a.mustL0;
    if (m.components == 0 && a.stats.l0Tris > 0)
        return "⑤ 分量划分为空而产物有 L0 三角：判据没跑（analyzeMustL0 未调用？）";
    if (m.attributedTris != m.l0Tris)
        return "⑤ 分量三角归属求和 " + std::to_string(m.attributedTris) + " != 分析看到的 L0 三角 "
            + std::to_string(m.l0Tris) + "（划分漏数或重数）";
    if (m.l0Tris != a.stats.l0Tris)
        return "⑤ 分析看到的 L0 三角 " + std::to_string(m.l0Tris) + " != stats.l0Tris "
            + std::to_string(a.stats.l0Tris) + "（两条独立 L0 口径分家：group.depth==0 累加 vs 逐簇解码）";
    if (m.splitTris != 0)
        return "⑤ 存在 " + std::to_string(m.splitTris)
            + " 个 L0 三角的三顶点落在不同分量（焊接或归属漂了，取首顶点归属当场失效）";
    return {};
}

// ⑥ 每 prim 的 depth 序列仍单调不减 = 引擎侧 meshlet_lod invariant 2 的对应物
//
// 它破了引擎直接报错（"级内连续"没了 ⟹ 取不出某一级的连续簇区间）。重排的合法性恰恰建立在
// "只在 depth==0 段内部动"，所以这条是**重排做错了**最直接的抓手（搬出段外 / 排序键写反）。
std::string checkDepthMonotonic(const PackedAsset& a)
{
    std::vector<uint32_t> lo(a.prims.size(), UINT32_MAX), hi(a.prims.size(), 0);
    for (const PackedGroup& g : a.groups) {
        lo[g.prim] = std::min(lo[g.prim], g.clusterOffset);
        hi[g.prim] = std::max(hi[g.prim], g.clusterOffset + g.clusterCount);
    }
    for (size_t p = 0; p < a.prims.size(); ++p) {
        int32_t prev = -1;
        for (uint32_t c = lo[p]; c < hi[p]; ++c) {
            const PackedCluster& cl = a.clusters[c];
            if (cl.group >= a.groups.size()) return "⑥ 簇 " + std::to_string(c) + " 的 group 下标越界";
            const int32_t d = a.groups[cl.group].depth;
            if (d < prev)
                return "⑥ prim " + std::to_string(p) + " 的 depth 序列在簇 " + std::to_string(c) + " 处由 "
                    + std::to_string(prev) + " 降到 " + std::to_string(d) + "（重排越出 depth==0 段）";
            prev = d;
        }
    }
    return {};
}

// ⑦ 每个 group 的簇仍是一段连续区间，且这段的**集合**与重排前逐位相同
//
// 簇身份 = triOffset：打包时逐簇追加 ⟹ 严格递增 ⟹ 唯一。按集合比而不是按位置比，正是
// "重排只搬条目、不改成员"这句话的机器形态：搬错一家（把 A group 的簇塞进 B group）当场红，
// 而"画哪些三角"这件事从头到尾没变——本批"截图逐位不变"的立论就在这一条上。
/// 单 group 的三条：簇数未变 / 区间不越界且仍是本 group 的连续一段 / 簇**集合**与重排前相同。
/// 簇身份 = triOffset：打包时逐簇追加 ⟹ 严格递增 ⟹ 唯一。按集合比而不是按位置比，正是
/// "重排只搬条目、不改成员"这句话的机器形态：搬错一家（把 A group 的簇塞进 B group）当场红，
/// 而"画哪些三角"这件事从头到尾没变——本批"截图逐位不变"的立论就在这一条上。
std::string checkOneGroupSet(const PackedAsset& a, size_t gi, std::vector<uint32_t>& before,
    std::vector<uint32_t>& after)
{
    const PackedGroup& g = a.groups[gi];
    const PackedGroup& old = a.groupsBeforeReorder[gi];
    const std::string where = "⑦ group " + std::to_string(gi) + " ";
    if (g.clusterCount != old.clusterCount)
        return where + "的簇数由 " + std::to_string(old.clusterCount) + " 变成 "
            + std::to_string(g.clusterCount) + "（重排只准搬条目，不准改成员）";
    if (uint64_t(g.clusterOffset) + g.clusterCount > a.clusters.size())
        return where + "的簇区间越出簇表（重排算错偏移）";
    for (uint32_t k = 0; k < g.clusterCount; ++k)
        if (a.clusters[g.clusterOffset + k].group != gi)
            return where + "的簇不再是一段连续区间（区间里混进别的 group 的簇）";
    before.clear();
    after.clear();
    for (uint32_t k = 0; k < g.clusterCount; ++k) {
        before.push_back(a.clustersBeforeReorder[old.clusterOffset + k].triOffset);
        after.push_back(a.clusters[g.clusterOffset + k].triOffset);
    }
    std::sort(before.begin(), before.end());
    std::sort(after.begin(), after.end());
    if (before != after) return where + "的簇集合与重排前不同（整组搬错家）";
    return {};
}

std::string checkGroupClusterSets(const PackedAsset& a)
{
    if (a.clustersBeforeReorder.size() != a.clusters.size())
        return "⑦ 簇总数 " + std::to_string(a.clusters.size()) + " 与重排前 "
            + std::to_string(a.clustersBeforeReorder.size()) + " 不符（重排搬丢或搬重）";
    if (a.groupsBeforeReorder.size() != a.groups.size())
        return "⑦ group 条数与重排前不符（快照缺失？packAsset 未无条件快照）";
    std::vector<uint32_t> before, after;
    for (size_t gi = 0; gi < a.groups.size(); ++gi) {
        std::string fail = checkOneGroupSet(a, gi, before, after);
        if (!fail.empty()) return fail;
    }
    return {};
}

// ⑧ groups 数组顺序与重排前逐位相同（除簇区间）
//
// 🔴 这条是"绝对不能动"那条红线的机器版：clodBuild 回调返回值被库原样记进新簇的 refined
// （= 全局 group 下标），groups 一旦被搬动，全部 refined 指错而**没有任何一条**其它检查会红
// （④ 只查 refined 指向更浅的同 prim group，搬动后照样成立）。
std::string checkGroupsOrderStable(const PackedAsset& a)
{
    if (a.groupsBeforeReorder.size() != a.groups.size()) return "⑧ group 条数与重排前不符";
    for (size_t i = 0; i < a.groups.size(); ++i) {
        const PackedGroup& x = a.groups[i];
        const PackedGroup& y = a.groupsBeforeReorder[i];
        if (x.depth != y.depth || x.prim != y.prim || x.radius != y.radius || x.error != y.error
            || std::memcmp(x.center, y.center, sizeof x.center) != 0)
            return "⑧ group " + std::to_string(i) + " 的非簇区间字段与重排前不同（groups 被搬动 ⟹ refined 全指错）";
    }
    return {};
}

// ⑨ MustL0 区间表与**重排后的产物**自洽
//
// 判据只吃簇的归属与几何、不吃簇的先后 ⟹ 在重排后的产物上重跑一遍必须给出同一个被钉集合。
// 不一致有两种解释，都该红：判据其实依赖簇序（那 P2 的对照表不可信），或重排改掉了判据吃的
// 东西（那"只改分组"这句话是假的）。再顺着表核**前缀性**：运行期"多发一条 draw"取的就是表里
// 这段簇区间，前缀断了就取不出单条连续簇区间。

/// 判据侧**独立**重建的每 prim 视图（不吃 MustL0 表）：depth==0 段的簇范围 + 被钉簇数。
struct MustL0PrimView {
    std::vector<uint32_t> l0Start, l0End, pinClusters;
};

MustL0PrimView scanMustL0Prims(const PackedAsset& a, const MustL0Result& r)
{
    MustL0PrimView v;
    v.l0Start.assign(a.prims.size(), UINT32_MAX);
    v.l0End.assign(a.prims.size(), 0);
    v.pinClusters.assign(a.prims.size(), 0);
    for (size_t gi = 0; gi < a.groups.size(); ++gi) {
        const PackedGroup& g = a.groups[gi];
        if (g.depth != 0) continue;
        v.l0Start[g.prim] = std::min(v.l0Start[g.prim], g.clusterOffset);
        v.l0End[g.prim] = std::max(v.l0End[g.prim], g.clusterOffset + g.clusterCount);
        if (r.pinned[gi] != 0) v.pinClusters[g.prim] += g.clusterCount;
    }
    return v;
}

/// 重跑判据与打包时读数的两条聚合对照。
std::string checkMustL0Rejudge(const PackedAsset& a, const MustL0Result& r)
{
    if (r.stats.mustL0Components != a.mustL0.mustL0Components)
        return "⑨ 重跑判据的 must_L0 分量数 " + std::to_string(r.stats.mustL0Components) + " != 打包时 "
            + std::to_string(a.mustL0.mustL0Components) + "（判据依赖簇序，重排改变了判据本身）";
    if (r.stats.pinnedTris != a.mustL0.pinnedTris)
        return "⑨ 重跑判据的额外 L0 三角 " + std::to_string(r.stats.pinnedTris) + " != 打包时 "
            + std::to_string(a.mustL0.pinnedTris) + "（同上，且这次连代价账都变了）";
    return {};
}

/// 表里一条 = 该 prim 被钉的那段簇区间。核四件事：与判据算出的簇数一致 / 从该 prim 的
/// depth==0 段起点开始（前缀性）/ 区间内每个簇的 group 都被钉 / 区间后紧跟着的簇**没**被钉
/// （否则前缀不完整，第二条 draw 会漏簇）。
std::string checkOneMustL0Entry(const PackedAsset& a, const MustL0Result& r, const MustL0PrimView& v,
    const DiskMustL0& t, size_t e)
{
    const std::string where = "⑨ MustL0 段第 " + std::to_string(e) + " 条 ";
    const uint32_t p = t.prim;
    if (t.clusterCount == 0) return where + "簇数为 0（空区间不进表）";
    if (t.clusterCount != v.pinClusters[p])
        return where + "簇数 " + std::to_string(t.clusterCount) + " != 判据被钉簇数 "
            + std::to_string(v.pinClusters[p]);
    if (t.clusterOffset != v.l0Start[p])
        return where + "起点 " + std::to_string(t.clusterOffset) + " != 该 prim depth==0 段起点 "
            + std::to_string(v.l0Start[p]) + "（重排没把 must_L0 排到最前面）";
    if (uint64_t(t.clusterOffset) + t.clusterCount > v.l0End[p])
        return where + "越出该 prim 的 depth==0 段（[" + std::to_string(t.clusterOffset) + ","
            + std::to_string(t.clusterOffset + t.clusterCount) + ") 不在 [" + std::to_string(v.l0Start[p]) + ","
            + std::to_string(v.l0End[p]) + ") 内";
    for (uint32_t k = 0; k < t.clusterCount; ++k)
        if (r.pinned[a.clusters[t.clusterOffset + k].group] == 0)
            return where + "区间内第 " + std::to_string(k) + " 个簇的 group 未被判据钉住（前缀里混进不该画的簇）";
    const uint32_t after = t.clusterOffset + t.clusterCount;
    if (after < v.l0End[p] && r.pinned[a.clusters[after].group] != 0)
        return where + "之后紧跟着仍是被钉 group（前缀不完整，第二条 draw 会漏簇）";
    return {};
}

std::string checkMustL0Table(const PackedAsset& a)
{
    const MustL0Result r = analyzeMustL0(a, a.mustL0Diag);
    std::string fail = checkMustL0Rejudge(a, r);
    if (!fail.empty()) return fail;
    const MustL0PrimView v = scanMustL0Prims(a, r);
    size_t e = 0;
    for (size_t p = 0; p < a.prims.size(); ++p) {
        if (v.pinClusters[p] == 0) continue;
        if (e >= a.mustL0Ranges.size())
            return "⑨ prim " + std::to_string(p) + " 有被钉簇却没进 MustL0 段（少一条）";
        if (a.mustL0Ranges[e].prim != p)
            return "⑨ MustL0 段第 " + std::to_string(e) + " 条的 prim = " + std::to_string(a.mustL0Ranges[e].prim)
                + " 与判据算出的 " + std::to_string(p) + " 不符（表少记或 prim 序错）";
        fail = checkOneMustL0Entry(a, r, v, a.mustL0Ranges[e], e);
        if (!fail.empty()) return fail;
        ++e;
    }
    if (e != a.mustL0Ranges.size())
        return "⑨ MustL0 段有 " + std::to_string(a.mustL0Ranges.size() - e) + " 条对不上判据（多记或 prim 序错）";
    return {};
}

bool runCheck(std::FILE* out, const char* name, const std::string& fail, const std::string& green)
{
    report(out, fail.empty(), name, fail.empty() ? green : fail);
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
    ok = runCheck(out, "⑤ 分量划分互不相交、并集 = 该 prim 全部三角", checkComponentPartition(asset),
            std::to_string(asset.mustL0.components) + " 分量 / " + std::to_string(asset.mustL0.l0Tris) + " L0 三角，不重不漏")
        && ok;
    ok = runCheck(out, "⑥ 每 prim 的 depth 序列单调不减（meshlet_lod invariant 2 对应物）",
            checkDepthMonotonic(asset), std::to_string(asset.prims.size()) + " prim 全部单调")
        && ok;
    ok = runCheck(out, "⑦ 每 group 的簇连续且集合与重排前逐位相同", checkGroupClusterSets(asset),
            std::to_string(asset.groups.size()) + " group 的簇集合未变")
        && ok;
    ok = runCheck(out, "⑧ groups 数组顺序与重排前逐位相同（refined 的命根子）", checkGroupsOrderStable(asset),
            "groups 未被搬动")
        && ok;
    ok = runCheck(out, "⑨ MustL0 区间表与重排后的产物自洽（判据重跑 + 前缀性）", checkMustL0Table(asset),
            std::to_string(asset.mustL0Ranges.size()) + " 条区间，被钉段均为各 prim L0 段前缀")
        && ok;
    std::fprintf(out, "%s\n", ok ? "verify: 全绿" : "verify: 有红项");
    return ok;
}

}  // namespace RGL::meshpack
