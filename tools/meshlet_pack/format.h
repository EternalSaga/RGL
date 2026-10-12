#pragma once
//
// format.h —— meshlet_pack 二进制产物布局的**唯一真相源**（★0-a）。
//
// ## 文件布局（小端；每段起始 16B 对齐，段间以 0x00 填充）
//
//   [FileHeader]                        固定 192B（static_assert 钉死）
//   [PrimTable]      primCount × DiskPrim      16B/条
//   [VertexPool]     vertexCount × 48B         ModelVertex 契约布局（pos f32×3 @0 /
//                                              normal f32×3 @12 / uv f32×2 @24 /
//                                              tangent f32×4 @32，rgl/shaders/model_schema.lua
//                                              vertexLayouts 单一真相源，48B/顶点不变）
//   [Clusters]       clusterCount × DiskCluster 44B/条
//   [Tris]           扁平 u8 流                 逐簇 triCount×3 个 8bit 局部索引
//   [Remap]          扁平 u32 流                逐簇 vertCount 个全局顶点下标
//   [Groups]         groupCount × DiskGroup     36B/条（簇 LOD DAG 的 group 节点）
//   [Nodes]          nodeCount × DiskNode       32B/条（clodBuildHierarchy 空间层级森林）
//
//   段内寻址：DiskCluster.triOffset = Tris 段内**字节**偏移；
//            DiskCluster.remapOffset = Remap 段内**元素**偏移。
//   全局顶点下标 = Remap[remapOffset + 局部索引]，恒指向 VertexPool。
//
// ## 与 docs/mdi_plan.md §7.2 数据面口径的对应
//
//   顶点池 138.4MB = VertexPool；L0 索引 8.5MB(8bit) + 其余级索引 8.2MB = Tris 全量
//   （L0 = 前缀，口径取 `refined == -1`；`group.depth == 0` 在真产物上与它同集合，见下「语义」节）；重映射 11.9MB(32bit) = Remap 全量。
//   LOD 不增顶点（简化只在原顶点集上产索引表）⟹ 全部 12 级共用同一份 VertexPool。
//
// ## 语义（渲染侧读法，= clusterlod.h 头注释的渲染规则）
//
//   簇 c 属于 group c.group（DiskGroup.depth = DAG 级）；c.refined = 产出 c 的**更细**
//   group 下标（-1 = 原始几何 L0）。簇该画 ⟺ groups[c.group].error > 阈值 且
//   (c.refined == -1 或 groups[c.refined].error ≤ 阈值)。层级 Nodes 是每 DAG 级一棵
//   树的森林：前 levelCount 个节点是各级根，叶节点 .group ≥ 0 指回 Groups。
//
//   🔴 **L0 的口径取 `c.refined == -1`**（它是本文件对 L0 的**定义**，且不依赖 group 二次跳转）。
//   两条口径在真产物上**实测完全一致**（2026-10-12 直接读产物本体复核：`refined==-1` 与
//   `groups[c.group].depth==0` 给出**同一个簇集合** = 27,487 簇 / 2,832,120 三角 = header
//   .l0TriangleCount）。本文件旧版与 `docs/mdi_plan.md` §12.3/§18.2 曾写「depth==0 只有
//   2,518 簇」——**那个读数错了**（口径方向没错，数字是脚本量错），已按实测改写。两条口径
//   从此不再是散文断言：一致性由 `rgl/meshlet_lod.cppm` 的 **invariant 3** 机器钉住
//   （分家即报错并点名 prim）。引擎口径 = `meshlet_ranges_impl.cppm::isL0Cluster`，照它。
//
// ## 坐标系
//
//   顶点与包围体全部是**源 glb 单位**（未乘 scene.lua 的 scale=0.01、未应用 node
//   transform）——与 §7 实测口径一致（spike 喂的也是原始 accessor 数据）。世界换算
//   归引擎侧（★0-b/★1 经对象矩阵）。
//
//   🔴 **tangent.w 恒 +1（规范值）**——bistro 实测 UV 手性 **82.00% 三角为负**
//   （2,322,319 / 2,832,120，与引擎 loader 同法逐三角 UV Jacobian，--stats 每次
//   打印可复核）。手性是**逐三角形**量（引擎 normalmap.slang 的 applyNormalMapTBN
//   消费 B = cross(N,T)·w；gltf_loader fillTangents 按 UV Jacobian 符号逐三角推），
//   共享顶点池存不下 ⟹ **★1 的 mesh shader 必须逐三角重算手性**（loader 同法），
//   否则该比例的面法线翻错。
//
// ## 缓存键（§7.8 拍板 3：构建期生成、不进 git）
//
//   sourceSha1 = 输入 glb 全文件 SHA1；configFingerprint = 钉死配置
//   （clodDefaultConfig(128) 全默认）的 FNV-1a（字段序逐字段哈希，见 packer.cpp）。
//   ★0-b 的缓存键 = 两者拼接 + toolVersion。
//
#include <cstddef>  // offsetof（ModelVertexLayout 的字段偏移由它算）
#include <cstdint>

namespace RGL::meshpack {

inline constexpr char kMagic[8] = {'R', 'G', 'L', 'C', 'L', 'O', 'D', 0};
inline constexpr uint32_t kFormatVersion = 1;  ///< 产物布局版本（本文件描述的布局）
inline constexpr uint32_t kToolVersion = 1;    ///< 打包器版本（算法改动时抬）
inline constexpr uint32_t kSectionAlign = 16;
inline constexpr uint32_t kVertexStride = 48;  ///< ModelVertex 契约（model_schema.lua）
inline constexpr uint32_t kHierarchyNodeWidth = 8;  ///< §7.2 层级 8,705 节点的口径

// 顶点内字段**字节**偏移（由下面的类型化视图 `ModelVertexLayout` 用 offsetof 算出，编译器背书）。
// 写侧/读侧一律用这组常量，🔴 **不要写裸的 float 下标或裸字节数**：packer 曾把 tangent.w 写成
// `float[10]` = 字节 40 = **tangent.z**，于是每个顶点的真实烘焙切线 z 被覆写成 1.0、而 w 留在 0——
// 簇路径法线通道因此在全屏 15.4% 的像素上偏 5~6 个量化步长，由 ★0.5-b2 共面双臂 equal 档抓到
// （→ docs/mdi_plan.md §15）。那种差一在 `float[10]` 的写法下**看不出来**，在 offsetof 下写不出来。
struct ModelVertexLayout {
    float pos[3];  ///< 字节 0
    float nrm[3];  ///< 字节 12
    float uv[2];   ///< 字节 24
    float tan[4];  ///< 字节 32（xyz = 烘焙切线，w = 规范值 +1）
};
static_assert(sizeof(ModelVertexLayout) == kVertexStride, "ModelVertex 契约 48B，改字段要抬 kFormatVersion");

inline constexpr uint32_t kVertPos = static_cast<uint32_t>(offsetof(ModelVertexLayout, pos));
inline constexpr uint32_t kVertNormal = static_cast<uint32_t>(offsetof(ModelVertexLayout, nrm));
inline constexpr uint32_t kVertUv = static_cast<uint32_t>(offsetof(ModelVertexLayout, uv));
inline constexpr uint32_t kVertTangent = static_cast<uint32_t>(offsetof(ModelVertexLayout, tan));
/// tangent.w 的字节偏移 = tangent 之后第 3 个 float（写侧写法：`[kVertTangentW / sizeof(float)]`）
inline constexpr uint32_t kVertTangentW = kVertTangent + 3u * static_cast<uint32_t>(sizeof(float));
static_assert(kVertTangent + 16u == kVertexStride, "tangent 是 ModelVertex 的最后一个字段");

// 文件头：固定大小、字段即偏移表。新增字段 = 抬 kFormatVersion。
struct FileHeader {
    char magic[8];
    uint32_t formatVersion;
    uint32_t toolVersion;
    uint8_t sourceSha1[20];   ///< 输入 glb 全文件 SHA1
    uint32_t configFingerprint;  ///< 钉死配置的 FNV-1a（口径见 packer.cpp）
    uint32_t primCount;
    uint32_t vertexCount;   ///< 顶点池元素数（48B/元素）
    uint32_t clusterCount;
    uint32_t groupCount;
    uint32_t nodeCount;
    uint32_t levelCount;    ///< DAG 级数（bistro = 12）
    uint32_t nodeWidth;     ///< 层级分支因子（= kHierarchyNodeWidth）
    uint64_t l0TriangleCount;
    uint64_t totalTriangleCount;  ///< 全部 LOD 级三角合计
    uint64_t offPrimTable, offVertexPool, offClusters, offTris, offRemap, offGroups, offNodes;
    uint32_t reserved[12];  ///< 头部扩位（凑满 192B = 16 的倍数；用掉一个减一个）
};
static_assert(sizeof(FileHeader) == 192, "FileHeader 布局是产物契约，改字段必须抬 kFormatVersion");

// 源 primitive 表：★0-b 的 importer 用它把簇资产对回 glb 的 mesh/material。
// 顺序 = tinygltf meshes[].primitives[] 展平序（= §7 spike dump.py 的喂入序）。
struct DiskPrim {
    uint32_t vertexBase;   ///< 该 primitive 在 VertexPool 中的首顶点下标
    uint32_t vertexCount;
    uint32_t indexCount;   ///< 源索引数（三角数 ×3）
    uint32_t material;     ///< glb material 下标（无 = 0xFFFFFFFF）
};
static_assert(sizeof(DiskPrim) == 16);

// 簇描述：≤128 顶点 + 8bit 三角索引 + 逐簇包围体（bounds 仅用于剔除，
// error 跨 DAG 非单调——渲染判据走 group，见 clusterlod.h 头注释）。
struct DiskCluster {
    uint32_t group;
    int32_t refined;       ///< 产出本簇的更细 group 下标，-1 = 原始几何
    uint32_t triCount;     ///< ≤ 128
    uint32_t vertCount;    ///< ≤ 128（8bit 局部索引因此可解）
    uint32_t triOffset;    ///< Tris 段字节偏移（triCount×3 个 u8）
    uint32_t remapOffset;  ///< Remap 段元素偏移（vertCount 个 u32 全局下标）
    float center[3];
    float radius;
    float error;
};
static_assert(sizeof(DiskCluster) == 44);

// 簇 LOD DAG 的 group（= clodGroup）：simplified = 该 group 简化产物的保守包围体，
// error = 其簇的累积误差（terminal group = FLT_MAX）。
struct DiskGroup {
    int32_t depth;         ///< DAG 级（0 = L0）
    uint32_t prim;         ///< 归属 primitive（clodBuild 逐 primitive 跑）
    float center[3], radius, error;  ///< clodGroup::simplified
    uint32_t clusterOffset, clusterCount;  ///< 连续簇区间 [offset, offset+count)
};
static_assert(sizeof(DiskGroup) == 36);

// 空间层级节点（= clodNode）：森林，前 levelCount 个是各级根；bounds.error = 子树最坏误差。
struct DiskNode {
    float center[3], radius, error;
    int32_t group;         ///< 叶：group 下标；内部节点 = -1
    uint32_t childOffset, childCount;
};
static_assert(sizeof(DiskNode) == 32);

}  // namespace RGL::meshpack
