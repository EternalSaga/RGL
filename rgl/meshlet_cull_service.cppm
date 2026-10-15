module;
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>  // VkBuffer（barrier 填字段用裸句柄）
export module meshlet_cull_service;

import rlstd;
import rlvk;
import vkerror;
import vk_device;
import vk_resources;
import scene_data;  // SceneDataService（barrier 要 scene data buffer 句柄）+ kSceneDataBytes
import offgraph_sync;  // OffGraphSync（B2-4②：barrier 参数由 usage 词表物化，本模块只发边名）
import VmaRaii;
import meshlet_cull_types_generated;  // MeshletCullItem / MeshletCullPush（反射单一真相源）

namespace RGL {

/**
 * 两张 buffer 在 `render_graph` **按名注册表**里的登记名（B2-3 的 buffer 附件名，单一来源）。
 *
 * 名字是这条链上唯一的连接件：图侧 `kind=buffer` 的附件、注册表的键、对账的入参三处都吃它，
 * 拼错一个字母不会编译失败，只会让 barrier 挂在一个不存在的资源上（`registerAttachment` 的
 * 拒绝面因此存在）。住在这里而不是注册表侧：本模块是这两张 buffer 的**所有者**，名字应当跟
 * 资源一起走；注册表刻意零模块知识（同 `OffGraphSync` 的口径）。
 */
export inline constexpr std::string_view kMeshletCullItemsAttachment = "meshletCullItems";
/** @see kMeshletCullItemsAttachment */
export inline constexpr std::string_view kMeshletVisibleFlagsAttachment = "meshletVisibleFlags";

/**
 * meshlet_cull_service —— **逐簇视锥剔除的 GPU 侧载体**（簇化轴 ★1-a2-i，rlscene）。
 *
 * ## 它管的那三样东西
 * | 资源 | 内存 | 谁读/写 | heap 槽 |
 * |---|---|---|---|
 * | 剔除项表（`MeshletCullItem` × itemCount） | host 映射直写（启动期一次） | compute binding 0 读 | 本服务领 |
 * | 簇头表（DiskCluster 44B/条） | 装配层已建（★1-a0 的 `MeshletGeometry`） | compute binding 1 读 | **复用** mesh binding 18 那槽 |
 * | 逐簇可见标志（u32 × 容量） | device-local，启动期 `vkCmdFillBuffer` 填 1 | compute binding 3 写 / mesh binding 21 读 | 本服务领 |
 *
 * ## 🔴 binding 号 ≠ heap 槽号
 * descriptor heap 下每个 binding 都要**显式领一个槽**并把槽号烘进 shader object 的 mapping
 * （`makeMappedCompute`）或管线条目的 `extra`（`addMeshletEntry`）。簇头表在 mesh 侧已经是
 * binding 18 = 装配层领的那个槽，compute 侧再领一个新号 = **同一张表两份描述符**，重写同槽
 * 之外没有任何收益 ⟹ 直接复用那个槽号（描述符内容不变，只是多一个 binding 指它）。
 *
 * ## 🔴 标志 buffer 为什么必须**无条件**存在且容量按「总簇数」而不是 itemCount
 * mesh SPV 只有一份（β-1 取代档与 cull 档共用 `meshlet.mesh.spv`），binding 21 是**无条件声明**
 * 的。descriptor heap 下「shader 声明了 binding 却读到描述符范围外」是 VVL 错误 + 真 UB，
 * 于是门关时也必须有一张**覆盖所有可能下标**的有效表。可能的下标 = `cullBase + 簇内序`，
 * 上界 = Σ l0Count ≤ 总簇数（`meshlet_ranges::buildClusterCullItems` 的前缀和口径），
 * **与 itemCount 无关**（门关时 itemCount 根本不存在）。⟹ 容量取装配层给的 `clusterCapacity`
 * （= 簇头表条数），门关/门开同一个容量，门关整张填 1 ⟹ 逐位不变。
 *
 * ## 跨帧复用：单份 buffer + 写前依赖 barrier（不是 ring×2）
 * `kFramesInFlight = 2` 下本帧 compute 写标志与上一帧 mesh 读标志之间没有天然依赖。两条路：
 * ① ring×2 逐帧交替；② 单份 + dispatch 前一条「写前依赖」barrier。**选 ②**，理由不是省事：
 * heap 槽号在 `vkCreateShadersEXT` 期**逐 binding 烘死**（compute 的 mapping 与 mesh 管线条目的
 * extra 都是启动期一次性烘焙），ring×2 要逐帧各一套管线条目与 shader object —— 而 mesh 条目
 * 已经按纹理集去重烘了一遍，再乘 2 是把条目表翻倍。同盘先例 = scene data 单份跨帧共享 buffer
 * 的写前依赖 barrier（`vkrenderer_passrec.cppm::recordFixedDataPath` 里那条，VVL syncval 实测
 * 缺它每帧 2 条 WRITE_AFTER_WRITE hazard）与 `IzbShadow` 的 head buffer。
 *
 * ## 生命周期红线
 * 本服务持有两张 GPU buffer，**必须比 Renderer 活得久**（Renderer 只持裸指针）。装配层声明序
 * 因此钉在 `backEnd` 之前（AGENTS「长生命周期 GPU 资源不得声明在 main() 局部块内」）。
 */
export class MeshletCullService {
	public:
	/**
	 * 构造（启动期一次，门 `meshlet.enable` 开且簇产物就绪才建）：领两张 heap 槽 +
	 * 建标志 buffer（device-local，`vkCmdFillBuffer` 填 1）+ 建 compute shader object
	 * （4 binding 的 heap mapping 在此烘死）。剔除项表**留到 uploadItems 再建**——
	 * 本函数被调时（`wireMeshletBindings`）实体还没注册、剔除项表还没扫出来。
	 * @param device 设备层（buffer / shader 创建、描述符写入、一次性 fill 提交）
	 * @param resources 资源层（VMA 分配器 / allocSlot / descriptor heap 尺寸）
	 * @param sceneData scene data 服务（幂等取槽号与 buffer 句柄：barrier 要它）
	 * @param clusterHeaderSlot 簇头表已在的 heap 槽号（= mesh binding 18 那槽，直接复用）
	 * @param clusterCapacity 簇头表条数（= 标志 buffer 容量上界，见类注释）
	 * @throw std::runtime_error buffer / shader object 创建失败（启动期一次性路径，同 GpuCull）
	 */
	MeshletCullService(DeviceContext& device, Resources& resources, SceneDataService& sceneData,
		uint32_t clusterHeaderSlot, uint32_t clusterCapacity);
	/** 拷贝删除：GPU 资源所有权唯一（两张 buffer + 一个 shader object），同 VmaBuffer。 */
	MeshletCullService(const MeshletCullService&) = delete;
	/** 拷贝赋值删除：同上。 */
	MeshletCullService& operator=(const MeshletCullService&) = delete;
	/** 移动删除：heap 槽号在构造期已烘进 compute shader object 与 mesh 管线条目的 mapping，
	 *  挪窝 = 那些 mapping 指向的槽号不变而 buffer 换了主人，语义上不可移动。 */
	MeshletCullService(MeshletCullService&&) = delete;
	/** 移动赋值删除：同上。 */
	MeshletCullService& operator=(MeshletCullService&&) = delete;

	/**
	 * 上传剔除项表（启动期一次，host 映射直写 + flush；照 `GpuCull` 的 cullTable 形态）。
	 * 空 span = 什么都不做（`itemCount()` 保持 0 ⟹ `recordDispatch` 零命令）。
	 * @throw std::runtime_error 项数超过构造期给的 `clusterCapacity`（标志表会写越界 =
	 *        踩到 buffer 外的设备内存，不报错只画垃圾，故必须在装配期拦下）
	 */
	void uploadItems(std::span<const MeshletCullItem> items);

	/**
	 * @return 逐簇可见标志的 heap 槽号。**同一个槽喂两处**：compute binding 3（写）与
	 * mesh binding 21（读）—— 描述符只有一份，两侧天然一致，不存在「改了这边忘了那边」。
	 */
	[[nodiscard]] uint32_t visibleSlot() const { return visibleBufferSlot; }
	/** @return 剔除项数（= 标志数组有效长度；0 = 尚未 uploadItems）。 */
	[[nodiscard]] uint32_t itemCount() const { return nItems; }

	/**
	 * 录一次剔除 dispatch（**调用方负责门控**：门关 / 服务为空时根本不该进来）。命令流固定
	 * 七条：`pipelineBarrier2` ×3（heap+scene 的合并执行序 → scene data 的 Transfer 写 → compute 读
	 * → 标志表的写前依赖）→ `bindShadersEXT` → `pushDataEXT`（100B）→ `dispatch` →
	 * `pipelineBarrier2`（compute 写 → 全图形读）。
	 *
	 * 🔴 批次 B2-4② 起 barrier **不再由本函数手抄掩码**：四条边各调一次 `OffGraphSync::emit`，
	 * 参数由 `offgraph_sync` 的 usage 词表物化（申报即发射）。代价 = 原本"一条命令装三条 barrier"
	 * 变成三条命令（不变式「一条边 = 一条命令」，见边表行注释），命令数 5→7，
	 * `recordMeshletCullDispatch` 的 `profilerAddCommands` 必须同步。
	 *
	 * 🔴 调用位置契约 = `recordFixedDataPath` **之后**（heap 已绑定、scene data 已拷完）、
	 * pass 循环**之前**（还在 `gpu.frame` 内、任何 render pass 外；eHost stage 不能在 pass 内）。
	 *
	 * @param cmd 本帧主命令缓冲（录制期）
	 * @param planes 视锥 6 面（`FrameDrawList::cullInput.planes`，与 CPU 剔除同源同值）
	 * @param sync 图外同步发射账（Renderer 成员，按引用转交；形参而非成员的形态先例 =
	 *        `GpuCull::recordFanoutDispatch`——本服务由装配层构造，拿不到 Renderer 的成员引用）
	 */
	void recordDispatch(vk::raii::CommandBuffer& cmd, std::span<const glm::vec4, 6> planes,
		OffGraphSync& sync);

   private:
	/** 构造期：领 items / visible 两个 Runtime 段 heap 槽（scene data 槽 = ensure 幂等返回）。 */
	void allocRuntimeSlots();
	/**
	 * 构造期：建标志 buffer（device-local + `eStorageBuffer | eTransferDst`）并一次性
	 * `vkCmdFillBuffer` 填 1 —— 门关时它就是「全可见」的全部含义，门开时它是首帧之前的初值。
	 */
	void createVisibleFlags();
	/** 构造期：compute shader object（binding 0=items 1=簇头 2=sceneData 3=flags）。 */
	void createCullShader();
	/**
	 * 把一个 buffer 的 BDA 写成 heap 槽的 storage buffer 描述符（`SceneDataService::ensure`
	 * 同款：`writeResourceDescriptorsEXT` + `vk::DeviceAddressRangeEXT`，host 直写不进命令流）。
	 */
	void writeSlotDescriptor(vk::DeviceAddress address, vk::DeviceSize bytes, uint32_t slot) const;

	DeviceContext& device;           ///< 设备层（buffer / shader 创建、描述符写、一次性 fill 提交）
	Resources& resources;            ///< 资源层（分配器 / heap 尺寸）
	SceneDataService& sceneData;     ///< scene data 服务（buffer 句柄 + 槽号）
	uint32_t clusterHeaderSlot = 0;  ///< compute binding 1 复用的槽号（mesh binding 18 同槽）
	uint32_t clusterCapacity = 0;    ///< 标志 buffer 容量（条）

	VmaBuffer itemsBuffer;   ///< 剔除项表（host 映射直写；uploadItems 才建）
	VmaBuffer visibleBuffer; ///< 逐簇可见标志（device-local，启动期填 1）
	uint32_t itemsSlot = 0;  ///< 剔除项表 heap 槽（compute binding 0）
	uint32_t visibleBufferSlot = 0;  ///< 可见标志 heap 槽（compute binding 3 = mesh binding 21）
	uint32_t objectMatrixSlot = 0;  ///< 对象矩阵表 SSBO 子范围槽（SceneDataService::objectMatrixSlot，compute binding 23）
	uint32_t nItems = 0;  ///< 已上传的剔除项数
	vk::raii::ShaderEXT cullShader = nullptr;  ///< meshlet_cull.comp（4 binding mapping 烘死）
};

}  // namespace RGL
