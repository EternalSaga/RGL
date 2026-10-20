// components.cpp —— 判据本体（焊接 / 并查集 / 逐级覆盖 / must_L0）+ L0 段 group 重排。
//
// 语义逐条对齐 tools/shadow_proxy_probe.py 的 weld() 与 analyze_prim()（口径全文见
// components.h 头注释）。四条"曾经量错过"的坑在本文件各有一条注释钉着：
//   ① 从未死亡的哨兵必须落在真实级号域之外（kNeverDies，不是 levelCount）；
//   ② 死亡级 == L 的含义是"级 L 里该分量已归零" ⟹ 保住它只能选 ≤ L−1，本文件的 must_L0
//      因此**只认恰为 1**（选级只剩 0 一个答案）；
//   ③ 逐三角归属取**首顶点**（探针同口径）。粗级三角的三个顶点**可以**跨分量（简化就是靠
//      合并邻近几何，跨分量的三角形正是分量死亡的方式），所以"逐顶点打标"与"逐三角首顶点
//      打标"不等价 —— 移植时改成前者会静默换判据，读数对不上又说不清差在哪；
//   ④ 🔴 **需求不是二值的**（移植时实测推翻 P0 数 5 的打印口径，全文见
//      docs/shadow_proxy_plan.md §7.3 教训 4）：对角线 ≥1m 的 10,203 个受保护分量里，
//      死亡级==1 只有 **196** 个，死在级 2~11 的有 6,476 个（旧读数"6,672 个必须 L0、中间级
//      零个"是探针 `min(d-1,0)` 把任意死亡级夹成 0 造成的）。本文件按定义算，不跟着那个数走：
//      两条 draw 只钉得住这 196 个，覆盖面比 §7.2 旧版宣称的小两个数量级。
//
// 变异自证（2026-10-20 全部实测，还原后重跑基线确认回绿；读数全文见 verify.cpp 头注的变异红线）：
//   · 关掉焊接（WeldTable::insert 里 memcmp 命中后不 return）⟹ 分量 145,828 → **282,242**
//     （与 Python 探针 --no-weld 同口径逐位对上）、must_L0 196 → 1,678、被钉 group 18 → 316、
//     额外 L0 三角 16,380 → 329,958。
//   · 门槛抬到 300 源单位（= 世界 3m）⟹ 被钉 group 数 **0**（探针数 6 同档也是 0）。
//   · stable 二分两半对调 → **只有 verify ⑨ 红**；同 prim 内跨级对调两簇 → ④⑥⑦ 一起红。
#include "components.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#include "packer.h"

namespace RGL::meshpack {
namespace {

/// 🔴 "从未死亡"的哨兵。真实级号上界 = levelCount（bistro 实测 12），取 INT32_MAX 落在真实
/// 级号域**之外**。用 levelCount 当哨兵的版本在 levelCount=5 的 shadow_edge_rig 上与真实级号
/// 5 同值 ⟹ 9 块板全被误判成"出厂档会丢遮挡"，而 bistro（levelCount=12）恰好躲过。
constexpr int32_t kNeverDies = std::numeric_limits<int32_t>::max();

[[noreturn]] void die(const char* what, uint64_t v0, uint64_t v1)
{
    std::fprintf(stderr, "meshlet_pack: %s (%llu, %llu)\n", what, (unsigned long long)v0, (unsigned long long)v1);
    std::abort();
}

uint64_t hashPos(const uint8_t* p)
{
    uint32_t x, y, z;
    std::memcpy(&x, p, 4);
    std::memcpy(&y, p + 4, 4);
    std::memcpy(&z, p + 8, 4);
    uint64_t h = (uint64_t(x) << 32) | y;
    h ^= uint64_t(z) + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdull;
    return h ^ (h >> 29);
}

/// 位置焊接表：开放寻址，键 = pos 的 12 字节**精确位模式**（🔴 无容差、无格距旋钮——
/// 一旦引入"格距"，判据就变成可以被调到绿的参数）。
///
/// 为什么不用 unordered_map<array<uint8_t,12>, uint32_t>：bistro 单 prim 最大 200 万+ 顶点，
/// 节点式哈希每槽开销 ~48B，这里 4B/槽（装载因子 ≤0.5）+ 12B/唯一位置的键副本。
struct WeldTable {
    std::vector<uint32_t> slots;  ///< 0 = 空，否则 logical id + 1
    std::vector<uint8_t> keys;    ///< logical id × 12B 位模式副本（探测时比较用）
    std::vector<uint32_t> first;  ///< logical id → prim 内首个顶点的下标（据此取该位置的坐标）
    uint32_t mask = 0;

    void reset(size_t vertexCount)
    {
        size_t n = 16;
        while (n < vertexCount * 2) n <<= 1;  // 装载因子 ≤ 0.5（线性探测不塌）
        slots.assign(n, 0u);
        keys.clear();
        first.clear();
        keys.reserve(vertexCount * 12);
        first.reserve(vertexCount);
        mask = uint32_t(n) - 1u;
    }

    uint32_t insert(const uint8_t* pos, uint32_t vertex)
    {
        uint32_t i = uint32_t(hashPos(pos)) & mask;
        while (true) {
            const uint32_t slot = slots[i];
            if (slot == 0) {
                const uint32_t id = uint32_t(first.size());
                keys.insert(keys.end(), pos, pos + 12);
                first.push_back(vertex);
                slots[i] = id + 1;
                return id;
            }
            const uint32_t id = slot - 1;
            if (std::memcmp(keys.data() + size_t(id) * 12, pos, 12) == 0) return id;
            i = (i + 1) & mask;
        }
    }

    uint32_t unique() const { return uint32_t(first.size()); }
};

/// 并查集（按 size 合并 + 迭代式路径压缩）。元素 = **逻辑顶点**下标（接口全取无符号，
/// 有符号的 size 只活在 p[] 内部，避免调用点满屏 cast 与 -Wsign-compare）。
struct DisjointSet {
    std::vector<int32_t> p;  ///< <0 = 根（值为 -size）；>=0 = 父

    void reset(size_t n) { p.assign(n, -1); }
    uint32_t find(uint32_t x)
    {
        uint32_t root = x;
        while (p[root] >= 0) root = uint32_t(p[root]);
        while (x != root) {
            if (p[x] < 0) break;
            const uint32_t next = uint32_t(p[x]);
            p[x] = int32_t(root);
            x = next;
        }
        return root;
    }
    void unite(uint32_t a, uint32_t b)
    {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (p[a] > p[b]) std::swap(a, b);  // p 存负 size：更负 = 更大的树，当根
        p[a] += p[b];
        p[b] = int32_t(a);
    }
};

/// prim 内分析的全部临时量（跨 prim 复用，避免逐 prim 重新 malloc）。
struct Scratch {
    WeldTable weld;
    DisjointSet ds;
    std::vector<uint32_t> lid;        ///< prim 顶点下标 → 逻辑顶点 id
    std::vector<uint32_t> triCount;   ///< 根 → L0 三角数（>0 的根才是分量）
    std::vector<int32_t> death;       ///< 根 → 死亡级（kNeverDies = 从未死亡）
    std::vector<float> box;           ///< 根 × 6（minX maxX minY maxY minZ maxZ）
    std::vector<uint8_t> has;         ///< 根 → 包围盒已初始化（componentBoxes 用）
    std::vector<uint8_t> must;        ///< 根 → must_L0
    std::vector<uint32_t> seen;       ///< 根 → 本级出现过的 stamp
    std::vector<uint32_t> compRoots;  ///< 有 L0 三角的根（= 分量清单，后续循环全按它走）
    std::vector<uint32_t> tris;       ///< 一簇的三角 → prim 内稠密顶点下标
    std::vector<uint32_t> levelOrder; ///< 该 prim 全部 group 下标，按 depth 升序（stable）
};

const uint8_t* posOf(const PackedAsset& a, uint32_t globalVertex)
{
    return a.vertexPool.data() + size_t(globalVertex) * kVertexStride + kVertPos;
}

/// 一簇的三角 → **prim 内稠密顶点下标**（Tris 的 8bit 局部索引经 Remap 落回全局，再减
/// vertexBase）。越出 [vertexBase, +vertexCount) = "prim 顶点区间封闭"这条假设破了 ⟹ 当场停：
/// 继续跑只会造出一张错归属表（探针原话："该断言不是可选的"）。
void decodeTris(const PackedAsset& a, const PackedCluster& c, uint32_t base, uint32_t vcnt, Scratch& sc)
{
    sc.tris.resize(size_t(c.triCount) * 3);
    for (uint32_t i = 0; i < c.triCount * 3; ++i) {
        const uint32_t global = a.remap[c.remapOffset + a.tris[c.triOffset + i]];
        if (global < base || global - base >= vcnt) die("簇三角的全局顶点下标越出 prim 顶点区间", global, base);
        sc.tris[i] = global - base;
    }
}

/// 步骤 1：焊接。返回逻辑顶点数。
/// 🔴 变异自证的注入点就在 `weld.insert`：把它换成"每个顶点一个新 id"（= 不焊接）⟹ bistro
/// 分量数从 145,828 涨到 282,242，三个判据读数一起变差（Python 版同口径实测）。
uint32_t weldPrim(const PackedAsset& a, uint32_t base, uint32_t vcnt, Scratch& sc)
{
    sc.weld.reset(vcnt);
    sc.lid.resize(vcnt);
    for (uint32_t v = 0; v < vcnt; ++v) sc.lid[v] = sc.weld.insert(posOf(a, base + v), v);
    return sc.weld.unique();
}

/// 逐 L0 三角回调（回调拿到三角三个顶点的 **prim 内稠密下标**）。
/// 拆出它的理由：三处使用（建并查集 / 归属计数 / 打钉）都要同一趟三层循环，而三层嵌套的
/// 认知复杂度是拆函数护栏真正想拦的东西——缝在"遍历"与"每三角做什么"之间。
template <typename Fn>
void forEachL0Triangle(const PackedAsset& a, const std::vector<uint32_t>& l0, uint32_t base, uint32_t vcnt,
    Scratch& sc, Fn fn)
{
    for (const uint32_t gi : l0) {
        const PackedGroup& g = a.groups[gi];
        for (uint32_t k = 0; k < g.clusterCount; ++k) {
            decodeTris(a, a.clusters[g.clusterOffset + k], base, vcnt, sc);
            for (size_t i = 0; i + 2 < sc.tris.size(); i += 3) fn(sc.tris[i], sc.tris[i + 1], sc.tris[i + 2]);
        }
    }
}

/// 步骤 2a：在逻辑顶点上跑并查集（只看 L0 三角——分量定义在原始几何上）。
void unionL0Triangles(const PackedAsset& a, const std::vector<uint32_t>& l0, uint32_t base, uint32_t vcnt,
    uint32_t nLogical, Scratch& sc)
{
    sc.ds.reset(nLogical);
    forEachL0Triangle(a, l0, base, vcnt, sc,
        [&](uint32_t x, uint32_t y, uint32_t z) {
            sc.ds.unite(sc.lid[x], sc.lid[y]);
            sc.ds.unite(sc.lid[y], sc.lid[z]);
        });
}

/// 步骤 2b：逐三角归属。**必须在全集合并完之后跑**：中途的根不是最终根，会把一个分量拆成
/// 多条计数。取首顶点当整三角的归属（与探针同口径），其前提"三顶点同分量"就地自证。
void attributeL0Triangles(const PackedAsset& a, const std::vector<uint32_t>& l0, uint32_t base, uint32_t vcnt,
    uint32_t nLogical, Scratch& sc, MustL0Stats& st)
{
    sc.triCount.assign(nLogical, 0);
    sc.compRoots.clear();
    forEachL0Triangle(a, l0, base, vcnt, sc,
        [&](uint32_t x, uint32_t y, uint32_t z) {
            const uint32_t r0 = sc.ds.find(sc.lid[x]);
            // 三顶点必同分量 ⟸ 三条边都在 2a 被 union 过。不成立 = 焊接或归属漂了，而"取首
            // 顶点当整三角的归属"（探针与这里共用）当场失效 ⟹ 计数交给 verify ⑤ 点名。
            if (r0 != sc.ds.find(sc.lid[y]) || r0 != sc.ds.find(sc.lid[z])) ++st.splitTris;
            if (sc.triCount[r0]++ == 0) sc.compRoots.push_back(r0);
            ++st.l0Tris;
        });
}

/// 步骤 2：并查集 + 归属两趟（同一批 L0 三角，两趟不能合并，理由见 2b）。
void buildComponents(const PackedAsset& a, const std::vector<uint32_t>& l0, uint32_t base, uint32_t vcnt,
    uint32_t nLogical, Scratch& sc, MustL0Stats& st)
{
    unionL0Triangles(a, l0, base, vcnt, nLogical, sc);
    attributeL0Triangles(a, l0, base, vcnt, nLogical, sc, st);
    for (const uint32_t r : sc.compRoots) st.attributedTris += sc.triCount[r];
    st.components += sc.compRoots.size();
}

/// 步骤 3：每分量包围盒对角线（**源 glb 单位**，未乘 scene.lua 的 scale）。
/// 遍历全部逻辑顶点（与探针一致）：孤立顶点自成分量但没有三角，按 compRoots 取用时自然被忽略。
void componentBoxes(const PackedAsset& a, uint32_t base, uint32_t nLogical, Scratch& sc)
{
    sc.box.assign(size_t(nLogical) * 6, 0.f);
    sc.has.assign(nLogical, 0);
    for (uint32_t j = 0; j < nLogical; ++j) {
        const uint32_t r = sc.ds.find(j);
        const float* p = reinterpret_cast<const float*>(posOf(a, base + sc.weld.first[j]));
        float* b = sc.box.data() + size_t(r) * 6;
        if (!sc.has[r]) {
            b[0] = b[1] = p[0];
            b[2] = b[3] = p[1];
            b[4] = b[5] = p[2];
            sc.has[r] = 1;
            continue;
        }
        for (int k = 0; k < 3; ++k) {
            b[2 * k] = std::min(b[2 * k], p[k]);
            b[2 * k + 1] = std::max(b[2 * k + 1], p[k]);
        }
    }
}

double diagonal(const Scratch& sc, uint32_t root)
{
    const float* b = sc.box.data() + size_t(root) * 6;
    const double dx = double(b[1]) - b[0], dy = double(b[3]) - b[2], dz = double(b[5]) - b[4];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/// 一个 group 的簇里，每三角的**首顶点**所属分量打上 stamp（"该分量在这一级出现过"）。
void markLevelSeen(const PackedAsset& a, uint32_t base, uint32_t vcnt, const PackedGroup& g, uint32_t stamp,
    Scratch& sc)
{
    for (uint32_t k = 0; k < g.clusterCount; ++k) {
        decodeTris(a, a.clusters[g.clusterOffset + k], base, vcnt, sc);
        for (size_t t = 0; t + 2 < sc.tris.size(); t += 3) sc.seen[sc.ds.find(sc.lid[sc.tris[t]])] = stamp;
    }
}

/// 步骤 4a：逐级覆盖 → 每分量的死亡级。
///
/// 🔴 逐级遍历的是**该 prim 实际存在的级**（升序），不是 0..levelCount：bistro 有 prim 的级集合
/// 是 {0,1,3}，去查不存在的级会把"画了"读成"没画"（探针首跑 off-by-one 的同族病）。
/// 🔴 判据用"三角归零"而不是"同簇混装"：L0 簇本来就常常跨分量（meshopt 按空间邻近聚簇，不保证
/// 连通性），混装不丢遮挡（整簇全画，两个部件都在），丢遮挡发生在**简化把某分量的三角删光**那一刻。
void deathLevels(const PackedAsset& a, uint32_t base, uint32_t vcnt, Scratch& sc)
{
    sc.death.assign(sc.ds.p.size(), kNeverDies);
    sc.seen.assign(sc.ds.p.size(), 0);
    uint32_t stamp = 0;
    size_t i = 0;
    while (i < sc.levelOrder.size()) {
        const int32_t lv = a.groups[sc.levelOrder[i]].depth;
        ++stamp;
        for (; i < sc.levelOrder.size() && a.groups[sc.levelOrder[i]].depth == lv; ++i)
            markLevelSeen(a, base, vcnt, a.groups[sc.levelOrder[i]], stamp, sc);
        for (const uint32_t r : sc.compRoots)
            if (sc.death[r] == kNeverDies && sc.seen[r] != stamp) sc.death[r] = lv;
    }
}

/// 步骤 4b：死亡级 == 1 **且**包围对角线 ≥ 门槛 ⟹ must_L0。
/// 🔴 只认"恰为 1"：死亡级 == L 的含义是"级 L 里该分量三角已归零"，保住它要求选级 ≤ L−1，
/// 而 L==1 时只剩级 0 一个答案——这才是"必须 L0"。死在级 2~11 的分量本方案救不到（两条 draw
/// 只表达"钉 0"），它们的真实规模见 tools/shadow_proxy_probe.py 数 5 那两行读数。
void pickMustL0(double threshold, Scratch& sc, MustL0Stats& st)
{
    sc.must.assign(sc.ds.p.size(), 0);
    for (const uint32_t r : sc.compRoots) {
        if (sc.death[r] != 1 || diagonal(sc, r) < threshold) continue;
        sc.must[r] = 1;
        ++st.mustL0Components;
    }
}

/// 步骤 5：整组钉 L0 —— 一个 group 只要**含任一** must_L0 分量的三角就整组钉。
/// 排序粒度是 group 不是簇（`DiskGroup.clusterOffset/clusterCount` 要求一个 group 的簇连续）。
bool groupHasMustL0(const PackedAsset& a, const PackedGroup& g, uint32_t base, uint32_t vcnt, Scratch& sc)
{
    for (uint32_t k = 0; k < g.clusterCount; ++k) {
        decodeTris(a, a.clusters[g.clusterOffset + k], base, vcnt, sc);
        for (size_t i = 0; i + 2 < sc.tris.size(); i += 3)
            if (sc.must[sc.ds.find(sc.lid[sc.tris[i]])] != 0) return true;
    }
    return false;
}

void pinGroups(const PackedAsset& a, const std::vector<uint32_t>& l0, uint32_t base, uint32_t vcnt, Scratch& sc,
    MustL0Result& res)
{
    for (const uint32_t gi : l0) {
        const PackedGroup& g = a.groups[gi];
        if (!groupHasMustL0(a, g, base, vcnt, sc)) continue;
        res.pinned[gi] = 1;
        ++res.stats.pinnedGroups;
        for (uint32_t k = 0; k < g.clusterCount; ++k) res.stats.pinnedTris += a.clusters[g.clusterOffset + k].triCount;
    }
}

}  // namespace

MustL0Result analyzeMustL0(const PackedAsset& asset, double thresholdSourceUnits)
{
    MustL0Result result;
    result.pinned.assign(asset.groups.size(), 0);
    const bool judge = thresholdSourceUnits > 0.0;  ///< 0 = 关闭判定（划分照算，供 verify ⑤ 用）

    // 逐 prim 的 group 分桶。🔴 并查集只在单个 primitive 内跑（跨 prim 的两个部件本就在不同
    // DAG、从不互相合并，跨 prim 算会造出一批不存在的分量边界）。这里**不假设** group 表按 prim
    // 连续分段——那条连续性由 verify ⑥⑦ 与引擎侧 meshlet_ranges invariant 1 分别钉。
    std::vector<std::vector<uint32_t>> byPrim(asset.prims.size());
    for (size_t gi = 0; gi < asset.groups.size(); ++gi) {
        // 越界 = 产物自相矛盾（verify ④ 也盯这条），但判据先于 verify 跑、且 ⑨ 会在坏产物上
        // 重跑判据 ⟹ 这里必须自己停，不能让 byPrim[] 越界写变成一段未定义读。
        if (asset.groups[gi].prim >= asset.prims.size())
            die("group 的 primitive 下标越界", gi, asset.groups[gi].prim);
        byPrim[asset.groups[gi].prim].push_back(uint32_t(gi));
    }

    Scratch sc;
    std::vector<uint32_t> l0;
    for (size_t p = 0; p < asset.prims.size(); ++p) {
        std::vector<uint32_t>& gs = byPrim[p];
        if (gs.empty()) continue;
        // 按 depth 升序（stable ⟹ 同级内保持 group 下标序，与探针 by_prim 的遍历序一致）
        std::stable_sort(gs.begin(), gs.end(),
            [&](uint32_t x, uint32_t y) { return asset.groups[x].depth < asset.groups[y].depth; });
        l0.clear();
        for (uint32_t gi : gs)
            if (asset.groups[gi].depth == 0) l0.push_back(gi);
        result.stats.l0Groups += l0.size();
        if (l0.empty()) continue;  // 无 L0 段的 prim 没有分量可谈（bistro 无此形态）

        const uint32_t base = asset.prims[p].vertexBase, vcnt = asset.prims[p].vertexCount;
        sc.levelOrder = gs;
        const uint32_t nLogical = weldPrim(asset, base, vcnt, sc);
        buildComponents(asset, l0, base, vcnt, nLogical, sc, result.stats);
        if (!judge) continue;
        componentBoxes(asset, base, nLogical, sc);
        deathLevels(asset, base, vcnt, sc);
        pickMustL0(thresholdSourceUnits, sc, result.stats);
        pinGroups(asset, l0, base, vcnt, sc, result);
    }
    return result;
}

namespace {

/// 每 prim 的 depth==0 group 段（按 group 下标升序）。
/// 🔴 并查集只在单个 primitive 内跑（跨 prim 的两个部件本就在不同 DAG、从不互相合并，跨 prim
/// 算会造出一批不存在的分量边界）；这里同样按 prim 分桶，且**不假设** group 表按 prim 连续分段
/// ——那条连续性由 verify ④⑦ 与引擎侧 meshlet_ranges invariant 1 分别钉。
std::vector<std::vector<uint32_t>> l0RunsByPrim(const PackedAsset& a)
{
    std::vector<std::vector<uint32_t>> runs(a.prims.size());
    for (size_t gi = 0; gi < a.groups.size(); ++gi) {
        if (a.groups[gi].depth != 0) continue;
        if (a.groups[gi].prim >= a.prims.size()) die("group 的 primitive 下标越界", gi, a.groups[gi].prim);
        runs[a.groups[gi].prim].push_back(uint32_t(gi));
    }
    return runs;
}

/// 该 prim depth==0 段的簇区间起点，顺带校验重排的**合法性前提**：group 段在 groups 里连续、
/// 其簇区间首尾相接成一段。前提破了就是打包口径变了（不是"排序没生效"），当场停。
uint32_t runSpanStart(const PackedAsset& a, const std::vector<uint32_t>& run, uint32_t prim)
{
    const uint32_t spanStart = a.groups[run.front()].clusterOffset;
    uint32_t cursor = spanStart;
    for (size_t k = 0; k < run.size(); ++k) {
        const uint32_t gi = run[k];
        if (gi != run.front() + uint32_t(k)) die("prim 的 depth==0 group 段在 groups 里不连续", prim, gi);
        if (a.groups[gi].clusterOffset != cursor) die("prim 的 L0 簇区间不连续", prim, cursor);
        cursor += a.groups[gi].clusterCount;
    }
    return spanStart;
}

/// stable 二分后的 group 新序：must_L0 在前，两半内部保持原相对序
/// （= 不引入第二个不可复现的排序键，重跑一次必须逐位同一个结果）。
std::vector<uint32_t> pinnedFirstOrder(const std::vector<uint32_t>& run, const std::vector<uint8_t>& pinned)
{
    std::vector<uint32_t> order;
    order.reserve(run.size());
    for (uint32_t gi : run)
        if (pinned[gi]) order.push_back(gi);
    for (uint32_t gi : run)
        if (!pinned[gi]) order.push_back(gi);
    return order;
}

/// 把 `order` 指定的 group 块依次搬回 [spanStart, …) 并重算 clusterOffset。
/// 只搬 clusters 里的簇条目 + 重算 clusterOffset（clusterCount 不变）；簇自带的
/// triOffset/remapOffset 指向 Tris/Remap 段，那两段内容不动所以偏移仍有效。
void permuteRun(PackedAsset& a, const std::vector<uint32_t>& order, uint32_t spanStart)
{
    std::vector<PackedCluster> moved;
    for (uint32_t gi : order) {
        const PackedGroup& g = a.groups[gi];
        moved.insert(moved.end(), a.clusters.begin() + g.clusterOffset,
            a.clusters.begin() + g.clusterOffset + g.clusterCount);
    }
    uint32_t off = spanStart;
    size_t src = 0;
    for (uint32_t gi : order) {
        PackedGroup& g = a.groups[gi];
        g.clusterOffset = off;
        for (uint32_t k = 0; k < g.clusterCount; ++k) a.clusters[off + k] = moved[src + k];
        off += g.clusterCount;
        src += g.clusterCount;
    }
}

uint32_t pinnedClusters(const PackedAsset& a, const std::vector<uint32_t>& run, const std::vector<uint8_t>& pinned)
{
    uint32_t n = 0;
    for (uint32_t gi : run)
        if (pinned[gi]) n += a.groups[gi].clusterCount;
    return n;
}

}  // namespace

void reorderL0Groups(PackedAsset& asset, const MustL0Result& result)
{
    asset.mustL0Ranges.clear();
    const std::vector<std::vector<uint32_t>> runs = l0RunsByPrim(asset);
    for (size_t p = 0; p < runs.size(); ++p) {
        const std::vector<uint32_t>& run = runs[p];
        const bool any = std::any_of(run.begin(), run.end(),
            [&](uint32_t gi) { return result.pinned[gi] != 0; });
        if (!any) continue;
        const uint32_t spanStart = runSpanStart(asset, run, uint32_t(p));
        const std::vector<uint32_t> order = pinnedFirstOrder(run, result.pinned);
        if (!std::equal(order.begin(), order.end(), run.begin())) permuteRun(asset, order, spanStart);
        // 区间表条目 = (prim, 该 prim depth==0 段起点, 被钉的簇数)。重排保证这簇数恰好铺满
        // [spanStart, +n)，且区间内每个簇的 group 都是 must_L0。🔴 记簇区间而不是 group 下标
        // 区间的理由见 format.h 的 DiskMustL0 注释（被钉的 group 在 groups 里天然不连续）。
        asset.mustL0Ranges.push_back(
            DiskMustL0{uint32_t(p), spanStart, pinnedClusters(asset, run, result.pinned)});
    }
}

}  // namespace RGL::meshpack
