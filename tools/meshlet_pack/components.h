#pragma once
//
// components.h —— 位置焊接 + 连通分量 + 逐分量逐级覆盖 ⟹ **must_L0 判定**与
// **prim 内 depth==0 段的 group 重排**（shadow proxy 批次 N10 的 P1，
// 计划 → docs/shadow_proxy_plan.md §7.2）。
//
// ## 判据本体（🔴 参考实现 = tools/shadow_proxy_probe.py 的 weld() 与 analyze_prim()）
// 那份 Python 已在真产物上跑通六条对账，本文件是**移植**，不是重新发明一套。三条定义逐条对齐：
//
//   · **分量** = 在**焊接后的逻辑顶点**上跑并查集。焊接 = 按 pos 的 12 字节**精确位模式**
//     合并同位置顶点。源数据是 de-indexed 的（逐面顶点，flat shading 要逐面法线）⟹ 相邻三角
//     在同一位置存两份不同下标的顶点，按下标跑并查集会**把一块实心平板切成几百个假分量**
//     （实测 shadow_edge_rig 的 36×12 网格平板：焊接后 1 个分量，不焊接 664 个）。
//     🔴 **不加容差旋钮**：de-index 是复制，坐标逐位相同 ⟹ 精确匹配就够；而一旦引入"格距"
//     这个参数，判据就变成可以被调到绿的旋钮（同 skill rgl-test-discipline 那条"能调=能调绿"）。
//     🔴 并查集**只在单个 primitive 内跑**：阴影合并发生在 clodBuild 逐 prim 建的 DAG 里，
//     跨 prim 的两个部件本就在不同 DAG、从不互相合并 ⟹ 跨 prim 算会造出一批不存在的分量边界。
//
//   · **死亡级** = 最小的 L 使该分量在"级 L 的簇集合"里三角数归零。🔴 "从未死亡"的哨兵取
//     kNeverDies（INT32_MAX），**不是 levelCount**：首版用 levelCount 当哨兵，在
//     levelCount=5 的 shadow_edge_rig 上哨兵与真实级号 5 同值 ⟹ 9 块板全被误判成"出厂档会丢
//     遮挡"，而 bistro（levelCount=12）恰好躲过——只看 bistro 这个 bug 会一路活到 C++ 移植。
//
//   · **must_L0(c)** ⟺ 死亡级 == 1 **且** c 的包围对角线 ≥ 门槛。"死亡级 == 1"是**资产固有
//     属性**、不依赖相机档 ⟹ 打包期就能判死，不需要运行期现算。
//     🔴 覆盖面实测（2026-10-20 移植时改准，旧版这里写的"需求是二值的 / 6,672 个"是探针
//     打印式夹逼出来的错数，见 components.cpp 头注坑 ④）：对角线 ≥1m 的 10,203 个受保护分量里
//     恒存活 3,531 / 必须 L0 **196** / 死在级 2~11 共 6,476 —— 本方案（两条 draw 只钉 0）
//     只救得到那 196 个。
//     🔴 门槛住**源 glb 单位**（format.h 坐标系一节：产物未乘 scene.lua 的 scale）：packer
//     不知道 scale，用米会变成隐式的资产相关参数。
//
// ## 为什么排序单位是 group 而不是簇
// `DiskGroup.clusterOffset/clusterCount` 要求一个 group 的簇**连续** ⟹ 能重排的只有 group
// 区间的先后。粒度看着粗（一个 L0 group 有 60.6% 跨 ≥5 个分量），但 1m 门槛下"整组钉 L0"的
// 代价与逐分量理想上限只差 1.8%（16,380 vs 16,096 额外三角）⟹ 粗粒度不花额外的钱，
// "拆 group / 逐分量重排 / 改 clodBuild"三条重路线全部不需要。
//
// ## 🔴 groups 数组的顺序绝对不能动
// `clodBuild` 回调返回值被库**原样**记进新簇的 `refined`（= 全局 group 下标，packer.cpp 注释
// 明写"不可再叠加偏移"）⟹ 重排只做两件事：搬 clusters 数组里的簇条目 + 重算各 group 的
// clusterOffset。DiskCluster.triOffset/remapOffset 指向 Tris/Remap 段，那两段内容不动所以
// 偏移仍有效；每 prim 的 depth 序列仍单调不减（只在 depth==0 段**内部**动）⟹ 引擎侧
// meshlet_lod 的 invariant 2 与 meshlet_ranges 的 invariant 4（L0 是区间前缀）都不破。
//
// ## 本文件的边界
// 纯函数：只读 PackedAsset 的 prims / vertexPool / clusters / tris / remap / groups，
// 不碰 clodBuild、不改 clodConfig（动了 configFingerprint 缓存键就变）。
//
#include <cstdint>
#include <vector>

#include "format.h"

namespace RGL::meshpack {

struct PackedAsset;

/// 判据读数（`--stats` 的打印面 + `verify.cpp` 的 invariant 入参，一条读数一条判据）。
struct MustL0Stats {
    uint64_t components = 0;       ///< 连通分量总数（焊接后，全资产；门槛无关）
    uint64_t mustL0Components = 0; ///< 其中 must_L0 的分量数（死亡级==1 且对角线 ≥ 门槛）
    uint64_t pinnedGroups = 0;     ///< 被钉 L0 的 group 数（含任一 must_L0 分量 ⟹ 整组钉）
    uint64_t pinnedTris = 0;       ///< 被钉 group 的 L0 三角合计（= 运行期额外三角数）
    uint64_t primsAffected = 0;    ///< 有被钉 group 的 prim 数（= MustL0 段条数）
    uint64_t l0Groups = 0;         ///< depth==0 的 group 总数（被钉数的分母）
    // ↓ invariant ⑤ 的四条对照量（判据自己也要出声：静默给错数比崩更坏）
    uint64_t l0Tris = 0;           ///< 分析看到的 L0 三角合计（与 stats.l0Tris 互证）
    uint64_t attributedTris = 0;   ///< Σ 分量三角数（划分不重不漏 ⟺ 与上一枚相等）
    uint64_t splitTris = 0;        ///< 三顶点落在不同分量的 L0 三角数（必须 0，否则"取首顶点"不成立）
};

/// must_L0 判定的产物（只在打包期内存态；落盘的是重排后的簇序 + DiskMustL0 区间表）。
struct MustL0Result {
    /// 与 `PackedAsset::groups` 同长的 0/1 表：1 = 该 group 整组钉 L0。
    std::vector<uint8_t> pinned;
    MustL0Stats stats;
};

/// 判据本体。`thresholdSourceUnits` = 分量包围对角线下限（**源 glb 单位**）；
/// **<= 0 = 关闭判定**（仍算分量划分供 verify 用，但 pinned 全 0、不排序、新段为空）。
/// 🔴 与探针同语义：门槛比较用 double（对角线由 float 位模式升 double 后开方）。
MustL0Result analyzeMustL0(const PackedAsset& asset, double thresholdSourceUnits);

/// 按 `result.pinned` 对每个 prim 的 depth==0 段做 **stable 二分重排**（must_L0 在前，
/// 两半内部保持原相对序），重算该段各 group 的 clusterOffset，并把每 prim 被钉的那段**簇区间**
/// 写进 `asset.mustL0Ranges`（落盘为 MustL0 段；为什么是簇区间而不是 group 下标区间见
/// format.h 的 DiskMustL0 注释）。
/// 🔴 groups 数组本身一个元素都不动（`refined` 存的是它的下标）；调用方负责先快照
/// `groupsBeforeReorder` / `clustersBeforeReorder`（packAsset 无条件快照，门关态也留对照）。
void reorderL0Groups(PackedAsset& asset, const MustL0Result& result);

}  // namespace RGL::meshpack
