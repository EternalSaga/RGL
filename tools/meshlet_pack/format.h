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
//   [MustL0]         mustL0Count × DiskMustL0   12B/条（**v2 新增**，可缺省 = 0 条）
//
//   段内寻址：DiskCluster.triOffset = Tris 段内**字节**偏移；
//            DiskCluster.remapOffset = Remap 段内**元素**偏移。
//   全局顶点下标 = Remap[remapOffset + 局部索引]，恒指向 VertexPool。
//   🔴 **末段之后不填充**：文件长度 == 末段末尾（DiskNode 是 32B 恒对齐，v2 的 DiskMustL0 是
//   12B 会差出填充，故写侧对末段不 alignUp）。读取侧靠这条做"字段表错一位"的硬自证，
//   见 tools/shadow_proxy_probe.py 的「偏移表闭合」。
//
// ## 版本史（🔴 抬版本 = 写侧 kFormatVersion 与读侧 kMeshletFormatVersionMax 一起抬）
//
//   **v1** ★0-a 首版八段布局（本文件旧版的全部描述）。
//   **v2** shadow proxy 批次 N10 的 P1（docs/shadow_proxy_plan.md §7.2）：
//     ① 尾部新增可选 [MustL0] 段（`offMustL0` / `mustL0Count` 从 reserved 里划出两枚 u32
//        + 一枚 u64，头部**仍是 192B**，前 144B 字段序与 v1 逐位相同）；
//     ② 每个 prim 的 depth==0 段内部按"是否必须 L0"做过一次 stable 二分重排（must_L0 的
//        group 在前），于是**同一段簇的先后**与 v1 不同 —— 簇的**集合**、Tris/Remap 两段、
//        groups 数组顺序全部不变（`refined` 存的是全局 group 下标，动不得）。
//     🔴 读取侧的兼容口径：v2 = v1 + 尾部一段。不认识 MustL0 的读取器**照旧能画**（它只是
//        少一张还没人消费的表），所以读侧把 Max 抬到 2 而不是拒读；反过来 v1 读取器读到 v2
//        的产物不会崩（末段校验是 `Nodes 段尾 ≤ 文件长`，尾部多一段不触发），但**别指望**
//        这个方向：布局一变就抬版本，抬版本就写清"旧读取器会怎样"。
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
//   **v2 的 L0 段内序**（shadow proxy N10 P1）：每个 prim 的 depth==0 段内部按"是否 must_L0"
//   做过一次 stable 二分（must_L0 的 group 在前，两半内部保持 clodBuild 的产出序）。三条不破，
//   每条都有打包侧 verify 与引擎侧 invariant 分别盯着：
//     · 每 prim 的 depth 序列仍**单调不减**（只在同一段内部动）⟹ meshlet_lod invariant 2；
//     · L0 簇仍是该 prim 簇区间的**前缀** ⟹ meshlet_ranges invariant 4；
//     · 簇的**集合**与 Tris/Remap 两段逐位不变 ⟹ "画哪些三角"没变，只改了分组；而阴影段
//       depthWrite + eLess 只存最近深度 ⟹ 图集内容相同（本批"动产物却不换锚"的全部依据）。
//   ⚠ 将来 `meshlet.mode` 开成 switch/overlay 时要重测主渲染：簇的光栅顺序变了，而主 opaque
//   段写 5 张 G-buffer、深度恰好相等的片元有先后歧义（`docs/mdi_plan.md` §5 风险 5）。
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
inline constexpr uint32_t kFormatVersion = 2;  ///< 产物布局版本（本文件描述的布局；版本史见头注）
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
    uint64_t offMustL0;     ///< v2：MustL0 段起始偏移（mustL0Count==0 时 = 文件尾，段不存在）
    uint32_t mustL0Count;   ///< v2：MustL0 段条数（0 = 未判定 / 无被钉 group）
    uint32_t reserved[9];   ///< 头部扩位（凑满 192B = 16 的倍数；用掉一个减一个）
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

// v2 新增：must_L0 的区间表（shadow proxy N10 P1，判据本体见 components.h）。
// 一条 = 一个 prim：该 prim 的 depth==0 段里，**最前面** clusterCount 个簇所属的 group 全部
// 被整组钉 L0。语义（P2 的运行期读法，本批引擎侧**不读**）：这种 prim 至多发两条 draw ——
// 本表指的这段簇区间选级钉死 0，其余照常走 D。
//
// 🔴 为什么记的是**簇区间**而不是 group 下标区间：重排只能搬 clusters 里的簇条目，groups 数组
// 一个元素都动不得（`refined` 存的是它的下标）⟹ 被钉的那批 group 在 groups 里**天然不连续**
// （must_L0 的判定与 group 下标无关，实测 bistro prim 1514 的被钉 group 是 {6733, 6737} 这种
// 形态），"group 区间"在这份产物里根本表达不出来。而排序单位仍然是 group（一个 group 的簇必须
// 连续），"排到最前面"这件事唯一可落盘的写法就是这段簇区间。
// 🔴 表里也**不抄**该 prim 的 L0 段起点之外的任何派生量（prim 区间与各级簇序的唯一真相源是
// Groups 段）：抄一份 = 造第二个会漂的真相（AGENTS 哲学第 1 条）。clusterOffset 保留是为了让
// verify ⑨ 能机器核对"它确实等于该 prim depth==0 段的起点"——前缀性正是运行期取单条 draw 的前提。
struct DiskMustL0 {
    uint32_t prim;           ///< 归属 primitive（每 prim 至多一条，表内按 prim 升序）
    uint32_t clusterOffset;  ///< 被钉簇区间起点（= 该 prim depth==0 段起点，⑨ 核对）
    uint32_t clusterCount;   ///< 被钉簇数（≥1，区间内每个簇的 group 都是 must_L0）
};
static_assert(sizeof(DiskMustL0) == 12);

}  // namespace RGL::meshpack
