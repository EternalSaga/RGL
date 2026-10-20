export module meshlet_asset;

import rlstd;

/**
 * meshlet_asset —— meshlet_pack 产物的**读取侧**：字节流 → CPU 侧只读视图 + 校验（★0-b1，rlbase）
 *
 * ## 职责边界
 * 只做「把产物字节流读成经校验的只读视图」这一件事。**不认识 Vulkan、不上传、不画**（那是
 * ★0-b2 的 importer）；无设备依赖（判据 = 「这段逻辑需不需要一个 VkDevice 才能跑」→ 不需要，
 * 归 rlbase，形状照 `velocity_field` 的纯函数层先例）。
 *
 * ## 🔴 裁决 1：引擎侧**不重算**源 glb 的 SHA1（主 agent 拍板，勿改）
 * bistro 源 glb 481MB，启动期重算 SHA1 ≈ 1s，为一条启动路径付这个钱不值。产物 header 里的
 * `sourceSha1` 在本模块**只作为日志身份**打印（`MeshletAsset::sourceSha1Hex`），**不做校验**；
 * 「产物 ↔ 源资产」的缓存键（源 SHA1 + 工具版本）由**构建期**负责（`docs/mdi_plan.md` §7.8
 * 拍板 3：产物构建期生成、不进 git）。运行期读到陈旧产物 = 构建期缓存键失灵的事故，不归本层。
 *
 * ## 🔴 裁决 2：本模块**不 include `tools/` 下任何东西**（★0 红线：工具与引擎不互相依赖）
 * 布局真相源 = `tools/meshlet_pack/format.h`（写侧），本模块的 `MeshletDisk*` 结构是它的
 * **读取侧镜像**（字段序/大小逐一对齐，static_assert 钉死）。两侧不共享代码 ⟹ 布局漂移的
 * 兜底只有三条：① 产物 header 自带段表（偏移 + 计数，读取侧逐条验证段表自洽，见下）
 * ② `formatVersion`（布局改动必须抬版本，本模块按兼容范围拒读）③ ★0-b2 的端到端冒烟。
 *
 * ⚠️ **弱点明写**：本模块的单测（`tests/test_meshlet_asset.cppm`）验的是「**读取器对字节流的
 * 健壮性**」——任意坏字节必须被拒且可归因；它**不验两侧格式一致**（测试合成缓冲的布局本来就
 * 抄自本模块的镜像结构，两侧同时漂则测试照样绿）。别把这份单测当「引擎读得懂真产物」的
 * 证据，那条证据只能由 ★0-b2 拿 meshlet_pack 真产物做端到端冒烟给出。
 *
 * ## 布局口径（= format.h 的读取侧重述，改动必须同步两侧与 formatVersion）
 * `[FileHeader 192B][PrimTable][VertexPool][Clusters][Tris][Remap][Groups][Nodes]`，
 * v2 起尾部还可有一段 `[MustL0]`（**本模块不读**，口径见上面版本范围那条；校验只保证它存在时
 * 不骗过「末段段尾 ≤ 文件长」）。小端、段起始 16B 对齐、段间 0x00 填充；顶点池 48B/顶点 =
 * ModelVertex 契约（`rgl/shaders/model_schema.lua` 单一真相源，本模块**不解释顶点字段**，
 * 只给字节视图）。
 * **段字节数从 header 段表推导**（相邻偏移差 + 文件尾），本模块不硬编码任何绝对偏移；
 * 定长段的 `count × stride` 只用于**验证段表自洽**（段表容得下、不重叠、落在文件内）。
 * `count × stride` 本身在 uint64 下恒不溢出（count 是 uint32、stride ≤ 48，积 < 2^38），
 * 会溢出的是「段偏移 + 段大小」（偏移来自文件、可被构造）——逐段显式设防，见 impl。
 *
 * ## 校验清单（每条一个反向用例钉住，错误串两两不同 = 可归因）
 * magic / formatVersion 兼容范围 / toolVersion 兼容范围 / 段偏移单调 + 不重叠 + 落在文件内 +
 * 对齐 + 偏移加法不绕回 / 段大小与簇计数自洽（含 `Tris` 段字节 == Σ triCount×3、`Remap` 段
 * 字节 == Σ vertCount×4，容差 = 段间对齐填充 < 16B，见 `kMeshletSectionAlign`）/ 逐簇 8bit
 * 契约与 tri/remap 范围 / `clusterCount == Σ group.clusterCount` 且逐 group 簇区间有界 /
 * `refined` 指向更浅 group（结构不变量，与工具 `--verify ④` 同口径）/ `remap` 下标全部
 * `< vertexCount`。
 */

namespace RGL {

/** 产物 magic（8 字节，写侧 `format.h::kMagic` 的镜像；不匹配 = 不是簇产物文件）。 */
export inline constexpr char kMeshletMagic[8] = {'R', 'G', 'L', 'C', 'L', 'O', 'D', 0};

/** 引擎可读的 `formatVersion` 兼容范围（产物布局版本；布局改动写侧必须抬版本，本模块按此拒读）。
 *
 *  🔴 **v2 = v1 + 尾部一段可选的 MustL0**（shadow proxy N10 P1，写侧 `format.h` 版本史）：
 *  本模块**不读**那段（P2 才读），抬 Max 到 2 的全部理由是「不认识 ≠ 拒读」——v2 相对 v1 只多了
 *  一张还没人消费的表，前 144B 字段序与七段布局逐位未动，照旧画得出来。反过来，若这里停在 1，
 *  出厂默认态（`meshlet.assets=true`）会**整资产拒读** ⟹ 阴影段回落旧路径 ⟹ 截图变、红线 A 与
 *  出厂锚一起飞。尾部多出来的字节由「末段（Nodes）段尾 ≤ 文件长」这条天然放过，不额外放宽判据。
 *  ⚠ 这条兼容是**单向**的：v1 读取器读到 v2 不会崩，但别指望它画得对（v2 的 L0 段内簇序变了）；
 *  布局一变就抬版本、抬版本就写清"旧读取器会怎样"，是 `format.h` 版本史那一节的职责。 */
export inline constexpr uint32_t kMeshletFormatVersionMin = 1;
export inline constexpr uint32_t kMeshletFormatVersionMax = 2;

/**
 * 引擎可读的 `toolVersion` 兼容范围（打包器版本 = 算法口径，非布局）。
 * 上界收紧的理由：toolVersion 抬版意味着误差度量/简化算法换口径，而渲染侧选级阈值绑该口径
 * ——读不懂的口径宁可拒读报错，不要静默按旧阈值选级。
 */
export inline constexpr uint32_t kMeshletToolVersionMin = 1;
export inline constexpr uint32_t kMeshletToolVersionMax = 1;

/** 顶点池元素字节数（ModelVertex 契约，`model_schema.lua` 单一真相源；本模块只按此切字节）。 */
export inline constexpr uint32_t kMeshletVertexStride = 48;

/**
 * 段起始对齐（写侧 `format.h::kSectionAlign` 的镜像）。
 * 也是「变长段字节数对账」的容差上界：写侧段间以 0x00 填充到 16B 对齐 ⟹ Tris/Remap 段的
 * 段字节数 = Σ(逐簇贡献) + [0,16) 字节填充；对账判据 = `Σ ≤ 段字节 < Σ + 本值`。
 */
export inline constexpr uint32_t kMeshletSectionAlign = 16;

/**
 * 产物文件头（= `format.h::FileHeader` 的读取侧镜像，192B 钉死）。
 * 字段序即字节布局；两侧任何一侧改字段而不抬 `formatVersion`，由「段表自洽 + 段大小对账」
 * 与 ★0-b2 端到端冒烟兜底（→ 文件头裁决 2）。
 */
export struct MeshletDiskHeader {
	/** 文件 magic，逐字节等于 `kMeshletMagic`。 */
	char magic[8];
	/** 产物布局版本（写侧 `kFormatVersion`）。 */
	uint32_t formatVersion;
	/** 打包器版本（写侧 `kToolVersion`，算法口径）。 */
	uint32_t toolVersion;
	/** 输入 glb 全文件 SHA1 —— **仅日志身份，不校验**（→ 文件头裁决 1）。 */
	uint8_t sourceSha1[20];
	/** 钉死配置的 FNV-1a（口径 = 写侧 packer.cpp；本模块不解释，只透传给日志）。 */
	uint32_t configFingerprint;
	/** 源 primitive 表条数。 */
	uint32_t primCount;
	/** 顶点池元素数（48B/元素）。 */
	uint32_t vertexCount;
	/** 簇总数（全部 DAG 级）。 */
	uint32_t clusterCount;
	/** 簇 LOD DAG group 总数。 */
	uint32_t groupCount;
	/** 空间层级节点总数（森林）。 */
	uint32_t nodeCount;
	/** DAG 级数（bistro = 12）。 */
	uint32_t levelCount;
	/** 层级分支因子（写侧 `kHierarchyNodeWidth` = 8）。 */
	uint32_t nodeWidth;
	/** L0 三角数（= 输入三角数，无损性口径）。 */
	uint64_t l0TriangleCount;
	/** 全部 LOD 级三角合计。 */
	uint64_t totalTriangleCount;
	/** 七段起始偏移（相对文件头；单调 + 不重叠 + 落在文件内由本模块校验）。 */
	uint64_t offPrimTable, offVertexPool, offClusters, offTris, offRemap, offGroups, offNodes;
	/** v2：MustL0 段起始偏移与条数。**本模块不读那段**（口径见上面版本范围那条注释），
	 *  镜像它只为「两侧字段序逐位相同」这条镜像契约本身——不解释、不校验、不给视图。 */
	uint64_t offMustL0;
	uint32_t mustL0Count;
	/** 头部扩位（写侧凑满 192B 用；读取侧不解释）。 */
	uint32_t reserved[9];
};
static_assert(sizeof(MeshletDiskHeader) == 192, "FileHeader 是产物契约（对齐写侧 format.h 的 static_assert）");

/** 源 primitive 表条目（= `format.h::DiskPrim` 镜像）：把簇资产对回 glb 的 mesh/material。 */
export struct MeshletDiskPrim {
	/** 该 primitive 在 VertexPool 中的首顶点下标。 */
	uint32_t vertexBase;
	/** 该 primitive 的顶点数。 */
	uint32_t vertexCount;
	/** 源索引数（三角数 ×3）。 */
	uint32_t indexCount;
	/** glb material 下标（无 = 0xFFFFFFFF）。 */
	uint32_t material;
};
static_assert(sizeof(MeshletDiskPrim) == 16);

/** 簇描述（= `format.h::DiskCluster` 镜像）：≤128 顶点 + 8bit 三角索引 + 逐簇包围体。 */
export struct MeshletDiskCluster {
	/** 所属 group 下标（选级判据住在 group，不住簇）。 */
	uint32_t group;
	/** 产出本簇的**更细** group 下标；-1 = 原始几何（L0）。 */
	int32_t refined;
	/** 三角数（≤ 128，8bit 局部索引因此可解）。 */
	uint32_t triCount;
	/** 簇内顶点数（≤ 128）。 */
	uint32_t vertCount;
	/** Tris 段内**字节**偏移（triCount×3 个 u8）。 */
	uint32_t triOffset;
	/** Remap 段内**元素**偏移（vertCount 个 u32 全局顶点下标）。 */
	uint32_t remapOffset;
	/** 包围球球心（源 glb 单位）。 */
	float center[3];
	/** 包围球半径。 */
	float radius;
	/** 累积误差（terminal group 的簇 = FLT_MAX 口径）。 */
	float error;
};
static_assert(sizeof(MeshletDiskCluster) == 44);

/** 簇 LOD DAG 的 group（= `format.h::DiskGroup` 镜像）：选级判据的载体。 */
export struct MeshletDiskGroup {
	/** DAG 级（0 = L0）。 */
	int32_t depth;
	/** 归属 primitive（clodBuild 逐 primitive 跑）。 */
	uint32_t prim;
	/** 简化产物保守包围体 + 累积误差（terminal group = FLT_MAX）。 */
	float center[3], radius, error;
	/** 连续簇区间 [clusterOffset, clusterOffset + clusterCount)。 */
	uint32_t clusterOffset, clusterCount;
};
static_assert(sizeof(MeshletDiskGroup) == 36);

/** 空间层级节点（= `format.h::DiskNode` 镜像）：每 DAG 级一棵树的森林。 */
export struct MeshletDiskNode {
	/** 子树包围体 + 子树最坏误差。 */
	float center[3], radius, error;
	/** 叶：group 下标；内部节点 = -1。 */
	int32_t group;
	/** 子节点区间（Nodes 段内元素偏移）。 */
	uint32_t childOffset, childCount;
};
static_assert(sizeof(MeshletDiskNode) == 32);

/**
 * 一份经校验的簇产物（只读视图 + 文件镜像）。
 *
 * 所有 span 指向内部文件镜像 `image`（段偏移的对齐由校验保证，reinterpret_cast 合法）。
 * **禁拷贝**：拷贝会让 span 悬在旧镜像上；移动安全（vector 移动保持堆缓冲地址，span 不失效）。
 * 视图语义（渲染读法，= format.h 头注释的口径）：簇 c 该画 ⟺
 * `groups[c.group].error > 阈值 && (c.refined == -1 || groups[c.refined].error ≤ 阈值)`；
 * 全局顶点下标 = `remap[c.remapOffset + 局部索引]`，恒指向 `vertexPool`。
 */
export struct MeshletAsset {
	/** header 元数据值拷贝（计数/三角数/段偏移/身份字段；视图已按它校验过）。 */
	MeshletDiskHeader header{};
	/**
	 * 文件镜像：上面各 span 的指向目标（16B 对齐由 new 保证，段对齐由校验保证）。
	 * ⚠ **parse 之后不得改动本成员**（resize/swap 会让各 span 悬空）——禁拷贝已挡住最常见
	 * 的误用形态，剩余误用形态由 ★0-b2 的调用方纪律负责（本模块无 member 可写视图，护栏
	 * 面「禁 friend」规则也不允许给外部开写入口）。
	 */
	std::vector<std::byte> image;
	/** 源 primitive 表（`header.primCount` 条）。 */
	std::span<const MeshletDiskPrim> prims;
	/** 顶点池字节（`header.vertexCount × kMeshletVertexStride`；字段解释归消费者）。 */
	std::span<const std::byte> vertexPool;
	/** 簇描述（`header.clusterCount` 条）。 */
	std::span<const MeshletDiskCluster> clusters;
	/** 扁平 u8 三角索引流（逐簇 triCount×3 个 8bit 局部索引；段长从段表推导）。 */
	std::span<const uint8_t> tris;
	/** 扁平 u32 重映射流（逐簇 vertCount 个全局顶点下标，全部 < vertexCount）。 */
	std::span<const uint32_t> remap;
	/** 簇 LOD DAG group（`header.groupCount` 条）。 */
	std::span<const MeshletDiskGroup> groups;
	/** 空间层级节点森林（`header.nodeCount` 条）。 */
	std::span<const MeshletDiskNode> nodes;
	/** header.sourceSha1 的小写 hex（40 字符）——**日志身份，不做校验**（→ 文件头裁决 1）。 */
	std::string sourceSha1Hex;

	/** 默认构造 = 空视图（仅服务于构造期填充路径；填充完整只发生在 `parseMeshletAsset`）。 */
	MeshletAsset() = default;
	/** 移动构造：文件镜像堆缓冲地址不变 ⟹ 各 span 保持有效。 */
	MeshletAsset(MeshletAsset&&) noexcept = default;
	/** 移动赋值：同上。 */
	MeshletAsset& operator=(MeshletAsset&&) noexcept = default;
	/** 禁拷贝：span 会指向被拷贝的旧镜像（悬空），只允许移动。 */
	MeshletAsset(const MeshletAsset&) = delete;
	/** 禁拷贝赋值：同拷贝构造。 */
	MeshletAsset& operator=(const MeshletAsset&) = delete;
	/** 默认析构：镜像随对象释放，span 无独立生命周期。 */
	~MeshletAsset() = default;
};

/**
 * 解析内存镜像为经校验的只读视图（`loadMeshletAsset` 的核；测试直接经本入口注入合成缓冲）。
 *
 * 校验清单 → 文件头末段；**每条失败返回各自不同的错误串**（都以 `meshlet asset: ` 开头，
 * 失效形态可直接打进日志归因，不必读代码）。校验全过才构造视图，返回的对象恒自洽。
 * @param image 产物文件全量字节（接管所有权）
 * @return 只读视图；任一校验失败 → 可归因的错误串
 */
export [[nodiscard]] std::expected<MeshletAsset, std::string> parseMeshletAsset(
	std::vector<std::byte>&& image);

/**
 * 从磁盘读产物并解析（整读 → `parseMeshletAsset`）。
 * @param path 产物文件路径
 * @return 只读视图；文件不存在/不可读/读取不完整/任一校验失败 → 可归因的错误串（带路径）
 */
export [[nodiscard]] std::expected<MeshletAsset, std::string> loadMeshletAsset(std::string_view path);

}  // namespace RGL
