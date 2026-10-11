export module meshlet_geometry;

import rlstd;
import rlvk;
import rllogger;
import VmaRaii;
import vkerror;
import meshlet_asset;

/**
 * meshlet_geometry —— 簇产物的 **GPU 侧持有者**（簇化轴 ★1-a0，rlgfx）
 *
 * ## 职责边界（docs/mdi_plan.md §10.5 ★1-a0 那一行）
 * 只做一件事：把 `MeshletAsset`（★0-b1 的 CPU 只读视图）的 **4 段**各上传成一个常驻 storage
 * buffer——**顶点池 / 簇描述表 / Tris（8bit 索引）/ Remap（u32）**——然后**什么都不做**。
 * 🔴 本批不画任何东西、不建管线条目、不加 per-entry 描述符、不写 descriptor heap 槽：
 * 条目与固定 binding 的设计归 ★1-a1（§10.2）。数据在 GPU 里"躺着"就是本批的全部交付。
 *
 * ## 为什么在 rlgfx 而不是 rlscene
 * 判据只有一个「这段逻辑需不需要一个 VkDevice 才能跑」——需要（VMA 分配 + BDA 查询）⟹ 不能留
 * rlbase；而它与 `Resources`/`AssetTable`/描述符堆**一概无关**（本批连 heap slot 都不领），
 * 依赖面恰好就是 rlgfx 的 `VmaRaii` + `rlvk`，放 rlscene 反而把"要 VkDevice"读成"要场景层"。
 *
 * ## 内存类型与上传路径 = **照抄现行大只读 storage buffer 的做法**
 * 先例 = `model_importer_impl.cppm::uploadChunk`（现行唯一"启动期一次性大块只读 storage buffer"）：
 * `VMA_MEMORY_USAGE_CPU_TO_GPU` + `MAPPED | HOST_ACCESS_SEQUENTIAL_WRITE`，**直接 memcpy 进映射指针，
 * 不走 staging**。理由与代价（本批实测口径，绑机）：
 *   · **峰值 = 1×，不是 2×**。staging 路线（host 暂存 → vkCmdCopyBuffer → device-local）要为
 *     顶点池 138MB 额外再要一份同大小暂存，峰值 ~2×；映射直写只有一份分配。
 *   · 本资产**启动期写一次、之后只读**，staging 唯一能换来的"落到 DEVICE_LOCAL"收益在
 *     本机（APU 统一内存）为 0，在有 large-BAR 的独显上由 CPU_TO_GPU 自己挑到可见显存。
 *   · 与先例一致还有个测试面上的好处：不引入第二条上传路径，回读校验测的就是引擎那条路。
 * 与先例**不同**的两处，各有理由，写在实现处注释里：① 额外 `eTransferSrc`（设备级回读校验要
 * `vkCmdCopyBuffer`，先例的 canary 缺这个 flag 而它默认关、从未在 VVL 下跑过）；
 * ② 写完 `vmaFlushAllocation`（先例没刷——映射内存非 coherent 的驱动上会静默读到脏数据）。
 *
 * ## 段大小一律从 `MeshletAsset` 的视图取，**不硬编码**
 * 四段字节数 = `vertexPool.size_bytes()` / `clusters.size_bytes()` / `tris.size_bytes()` /
 * `remap.size_bytes()`。buffer 大小按 `kMeshletBufferAlign` 向上补齐（补齐区清 0，见实现），
 * 空段也建 16B 的 buffer——`VkBufferCreateInfo::size` 必须 > 0。
 *
 * ## 生命周期红线（AGENTS §生命周期红线）
 * 本结构持 4 个 `VmaBuffer`（析构即 `vmaDestroyBuffer`）。装配层必须把它声明在**与
 * `MeshletAssetStore` 同一作用域**（main_impl 的初始化层，`assetTable` 附近），
 * **不得**放进 main() 的局部块——块尾析构 = 第 0 帧起 GPU 侧引用已释放的 VA。
 */

namespace RGL {

/**
 * 段 buffer 的补齐粒度（= 产物写侧段对齐 `kMeshletSectionAlign` 同值，但**语义不同**：
 * 那条是"段起始在文件里的对齐"，本条是"buffer 大小向上取整到"，取同值只为口径统一）。
 * 补齐的意义：GPU 侧按 16B 边界读（`uint4` 粒度）不会越过 buffer 尾，回读校验也能对
 * **整块 buffer**（含补齐区）逐字节求期望值。
 */
export inline constexpr vk::DeviceSize kMeshletBufferAlign = 16;

/**
 * 簇产物四段中的**一段**在 GPU 上的形态：一个 storage buffer + 它的字节账。
 * move-only（载荷 `VmaBuffer` 是 GPU 资源唯一所有权）。
 */
export struct MeshletGeometrySection {
	/** 本段的 storage buffer（未上传 = 空状态）。 */
	VmaBuffer buffer;
	/** 段**数据**字节数（从 `MeshletAsset` 视图取，不含补齐）。 */
	vk::DeviceSize bytes = 0;
	/** buffer 实际字节数（= `bytes` 按 `kMeshletBufferAlign` 向上取整，且 ≥ 该对齐）。 */
	vk::DeviceSize allocBytes = 0;
	/** BDA 设备地址（★1-a1 若走"固定 binding 描述符"用不到它，走 buffer 寻址时免二次查询）。 */
	vk::DeviceAddress deviceAddress = 0;
};

/**
 * 一份簇产物在 GPU 上的四段（★1-a0 的全部产物；顺序 = 产物落盘顺序的子集）。
 *
 * 🔴 **Groups / Nodes / PrimTable 三段本批不上传**：簇路径的选级与层级遍历（★1-a1/★1-a2）
 * 才需要它们，届时按同一形状加段——本批"4 个 buffer"是 §10.5 定死的交付面，不多做。
 */
export struct MeshletGeometry {
	/** 顶点池（48B/顶点，ModelVertex 契约；解释归 shader）。 */
	MeshletGeometrySection vertexPool;
	/** 簇描述表（`MeshletDiskCluster` 44B/条）。 */
	MeshletGeometrySection clusters;
	/** 扁平 8bit 三角索引流（逐簇 triCount×3 个 u8）。 */
	MeshletGeometrySection tris;
	/** 扁平 u32 顶点重映射流（逐簇 vertCount 个全局顶点下标）。 */
	MeshletGeometrySection remap;
	/** 簇总数（= 产物 header.clusterCount，日志与 ★1-a1 的 dispatch 上界都要它）。 */
	uint32_t clusterCount = 0;
	/** 顶点池元素数（= 产物 header.vertexCount）。 */
	uint32_t vertexCount = 0;
};

/**
 * 按模型路径索引的 GPU 侧簇几何容器（★1-a0）。
 *
 * 键与 `MeshletAssetStore::byFile` **同源**（= `SceneEntityDesc.file`，CWD 相对）：一份 CPU
 * 产物对一份 GPU 几何，不搞第二个身份。与 `MeshletAssetStore` 同层由装配层持有——
 * 🔴 声明位置的生命周期契约见本文件头红线。
 */
export struct MeshletGeometryStore {
	std::map<std::string, MeshletGeometry> byFile;
};

/**
 * 把一份簇产物的 4 段上传成 4 个常驻 storage buffer（**启动期一次**，不在帧路径）。
 *
 * 分配与写入路径 = 现行大块只读 storage buffer 的做法（映射直写，无 staging，理由见文件头）。
 * 门开时打一条 INFO：4 个 buffer 的字节数 + 簇/顶点计数（口径与 `meshlet_pack --stats` 对得上）。
 *
 * @param allocator VMA 分配器（引擎侧 = `Resources::getAllocator()`；测试自建第二分配器走同一条路）
 * @param device 逻辑设备（BDA 查询用）
 * @param asset 经校验的簇产物（只读其 4 个视图；调用方保证其存活期覆盖本次调用）
 * @param sourceLabel 日志身份（模型路径），只进 INFO，不参与任何判据
 * @return 四段 buffer + 字节账；任一分配/映射失败 → 可归因 `Error`（调用方决定回落策略）
 */
export [[nodiscard]] std::expected<MeshletGeometry, Error> uploadMeshletGeometry(
	const VmaAllocatorRAII& allocator, const vk::raii::Device& device, const MeshletAsset& asset,
	std::string_view sourceLabel);

/**
 * 上传 + 落容器 + 失败口径的**胶水**（装配层唯一调用点，写成一处免得两处容器同步漂）。
 *
 * 🔴 失败**不抛**：簇资产是可选加速资产，不能砖引擎（★0-b2「坏产物不砖引擎」的上传版）。
 * 失败打 WARNING 带可归因错误串并**不落容器**；★1-a0 无人消费 GPU 侧数据，回落 = 图像逐位不变
 * （★1-a1 接入后再议"上传失败要不要降级为不画簇路径"，那时才有语义可谈）。
 */
export inline void uploadMeshletGeometryInto(const MeshletAsset& asset, std::string_view key,
                                             MeshletGeometryStore& store,
                                             const VmaAllocatorRAII& allocator,
                                             const vk::raii::Device& device) {
	if (auto uploaded = uploadMeshletGeometry(allocator, device, asset, key))
		store.byFile.try_emplace(std::string(key), std::move(*uploaded));
	else if (auto* logger = RLLogger::getInstance())
		logger->warn("meshlet: GPU 上传失败 {}：{} —— 不落容器，回落现行路径", key,
			uploaded.error().describe());
}

}  // namespace RGL
