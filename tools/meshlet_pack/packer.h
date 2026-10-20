#pragma once
//
// packer.h —— 簇化核心：源 primitive → meshlet 簇 LOD DAG + 空间层级（内存态产物）。
//
// 算法面 = §7 spike（/tmp/opencode/mdi/spike/spike.cpp）的 lod 分支，配置钉死
// `clodDefaultConfig(128)` 全默认（§9 ★0-a 判据原文；改一个参数 = 对拍作废）。
// 逐 primitive 喂 clodBuild（bistro 1591 个），层级 clodBuildHierarchy 全局建一次
// （node_width=8 = §7.2 的 8,705 节点口径）。
//
// 🔴 clodBuild 回调的坑（spike 里踩过并修掉的真 bug）：clodCluster::refined 存的
// **就是回调返回值本身**（库原样回填，见 clusterlod.h outputGroup/clusterize），而本
// 工具的回调返回全局 group 下标 ⟹ refined 已是全局值，**再叠加任何 per-prim 偏移都会
// 指错**。verify 的 DAG 结构检查兜这条。
//
// 本批不做并行化（TODO(parallel)：逐 prim 的 clodBuild 相互独立，可分 prim 段并行，
// 层级与全局下标收口留串行；单线程全场景 ~2.8s 够用，见 §7.1 耗时行）。
//
#include <cstdint>
#include <vector>

#include "components.h"
#include "format.h"
#include "glb_source.h"

namespace RGL::meshpack {

// scene.lua 的 scale=0.01（源单位 → 世界单位）。只服务 --stats 的世界米制读数
// （§7.2 的簇半径 p50 0.39m 那一行）；产物本体恒为源单位（format.h 坐标系）。
inline constexpr float kWorldScale = 0.01f;

// 内存态簇（字段 = DiskCluster 一一对应；verifyOrig 例外，见 PackedAsset）。
struct PackedCluster {
    uint32_t group;
    int32_t refined;
    uint32_t triCount, vertCount;
    uint32_t triOffset;    ///< tris 字节偏移
    uint32_t remapOffset;  ///< remap 元素偏移
    uint32_t origOffset;   ///< verifyOrig 元素偏移（仅内存，不序列化）
    float center[3], radius, error;
};

struct PackedGroup {
    int32_t depth;
    uint32_t prim;
    float center[3], radius, error;  ///< clodGroup::simplified
    uint32_t clusterOffset, clusterCount;
};

struct PackedNode {
    float center[3], radius, error;
    int32_t group;
    uint32_t childOffset, childCount;
};

struct PrimEntry {
    uint32_t vertexBase, vertexCount, indexCount, material;
};

struct PackStats {
    uint64_t prims = 0, sourceVerts = 0, sourceTris = 0;
    uint64_t l0Clusters = 0, totalClusters = 0, groups = 0, nodes = 0, levels = 0;
    uint64_t l0Tris = 0, totalTris = 0, l0Verts = 0;
    std::vector<uint64_t> levelTris;      // 逐 DAG 级三角数（§7.2「各级三角」行）
    std::vector<uint64_t> levelClusters;  // 逐级簇数
    // 数据面（§7.2「数据面合计」行的四项口径，字节；remapBytes = L0，其余级 = otherRemapBytes）
    uint64_t poolBytes = 0, l0IndexBytes = 0, remapBytes = 0, otherRemapBytes = 0, otherIndexBytes = 0;
    // L0 簇形状（对拍 §7.2 L0 行的「平均 tris/簇」「中位填充率」「包围球半径」）
    double l0AvgTris = 0, l0MedianFill = 0;
    float l0RadiusP50 = 0, l0RadiusP90 = 0, l0RadiusP99 = 0, l0RadiusMax = 0;  // 世界米
};

struct PackedAsset {
    uint8_t sourceSha1[20] = {};
    uint32_t configFingerprint = 0;
    std::vector<PrimEntry> prims;
    std::vector<uint8_t> vertexPool;      ///< vertexCount × kVertexStride
    std::vector<PackedCluster> clusters;
    std::vector<uint8_t> tris;            ///< 扁平 8bit 局部索引（triCount×3/簇）
    std::vector<uint32_t> remap;          ///< 扁平全局顶点下标（vertCount/簇）
    std::vector<uint32_t> verifyOrig;     ///< 簇化器原始全局索引（verify ② 的对照面；不序列化）
    std::vector<PackedGroup> groups;
    std::vector<PackedNode> nodes;
    PackStats stats;
    // ─── shadow proxy N10 P1（判据本体见 components.h；除 mustL0Ranges 外都不落盘）───────
    MustL0Stats mustL0;                        ///< 判据读数（--stats 打印面 + verify ⑤ 入参）
    double mustL0Diag = 0.0;                   ///< 生效的门槛（源 glb 单位；0 = 关闭判定）
    std::vector<DiskMustL0> mustL0Ranges;      ///< 落盘为 MustL0 段（v2），按 prim 升序
    std::vector<PackedGroup> groupsBeforeReorder;      ///< 重排前快照（verify ⑦⑧ 的对照面）
    std::vector<PackedCluster> clustersBeforeReorder;  ///< 同上（簇身份 = triOffset，唯一）

    uint32_t vertexCount() const { return uint32_t(vertexPool.size() / kVertexStride); }
    const float* vertexPos(uint32_t globalIndex) const;  // 池记录偏移 0 = position
};

// 钉死配置的 FNV-1a 指纹：按字段序逐字段哈希规范化字节（bool→u8、size_t→u64、float
// →u32 位型），不哈希结构体裸内存（padding 不参与 = 跨编译器可复现的口径）。
uint32_t configFingerprint();

// 主流程：逐 prim clodBuild → 全局池/8bit 索引/重映射 → 全局层级 → **must_L0 判定 + L0 段
// group 重排**（components.h）。不抛「可恢复」错误：库返回的不变量被破坏（vertex_count 超限等）
// 直接 abort——工具红了比产物悄悄错好。
// @param mustL0Diag 分量包围对角线下限（**源 glb 单位**，bistro 的 100 = 世界 1m）；
//        **0 = 关闭判定**：仍算分量划分供 verify 用，但不排序、MustL0 段为空、簇序与 v1 逐位相同。
//        🔴 它**不进** configFingerprint：那个指纹钉的是 clodConfig（改了 DAG 就换口径），而本
//        门槛只改分组不改 DAG，且任务红线明写"configFingerprint 缓存键不能变"。构建期的缓存键
//        另有其人 = ninja 的 COMMAND 里带着这个参数（改 RglMeshlets.cmake 那行即重跑打包）。
PackedAsset packAsset(const std::vector<SourcePrim>& prims, const uint8_t sourceSha1[20],
    double mustL0Diag = 0.0);

}  // namespace RGL::meshpack
