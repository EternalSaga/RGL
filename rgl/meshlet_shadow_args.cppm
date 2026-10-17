module;
#include <vulkan/vulkan.h>  // VkBuffer 裸句柄（设备侧回读校验的 vkCmdCopyBuffer src；rlvk 不含宏）
export module meshlet_shadow_args;

import rlstd;
import rlvk;
import vkerror;
import vk_device;     // DeviceContext（描述符写入取 device 句柄）
import vk_resources;  // Resources（allocSlot / descriptor heap 槽地址 / VMA 分配器）
import VmaRaii;       // VmaBuffer / VmaAllocatorRAII
import mdi_rows;      // MdiShadowRow / kMdiShadowRowStride + 尺寸/偏移算术与同源 pair

/**
 * meshlet_shadow_args —— 真 MDI 阴影段的**行表 GPU 载体**（docs/mdi_plan.md §19.5 ★M-1a，rlscene）
 *
 * ## 这批只做载体，一条 draw 都不接（🔴 零消费者）
 * 建一张 host 映射的行表 buffer + 领一个 descriptor heap 槽 + 把尺寸/偏移算术接上设备侧
 * 回读校验。**不改任何渲染路径**：没有 SPV 变体、没有管线条目、执行器一行不动。
 * 形态先例 = ★1-a0「只上传未消费，图像不变」（§10.5）与 ★18-B1「只建条目不接 draw」（§18.9）。
 * 于是出厂档截图必须仍逐位 = `95edc8cc…`，而日志里必须有本服务的 INFO 闭式锚 ⟹ 位比之前
 * 先证明「建了」（否则测的是两次「没建」）。
 *
 * ## ring×2 = **一张 buffer、一个槽**，轮转落在偏移上（§19.5 的设计收获）
 * 一张 buffer = `slices`（= `kFramesInFlight`）个等长 slice 首尾相接，描述符覆盖**整张**；
 * GPU 侧 `argsOffset` 与 shader 侧 `rowBase` 都由同一个 `frameSlot` 推出来（唯一产地 =
 * `mdiShadowRowLocation`）⟹ 分帧轮转不改句柄、不改描述符、不改管线条目。这正是
 * `MeshletCullService` 注释里骂过的「ring×2 要把条目表翻倍」那个代价，本形态绕开了。
 *
 * ## 🔴 为什么**不**套 `MeshletCullService` 的「单份 + 写前依赖 barrier」（§19.2 D4）
 * 那套的写者是 **GPU**：dispatch 前一条 barrier 排开上一帧的读。本表的写者是 **CPU**——
 * barrier 排不开主机写与 GPU 读（`eHost` 阶段的写只能靠内存可见性动作 + 分帧轮转排序），
 * 只有 ring×2 逐帧交替才是正确的形态（主机 fence 那是把同步点搬回来 = 本批要避免的代价）。
 *
 * ## 内存 = host-visible | host-coherent，CPU 直写映射（无 staging）
 * 分配口径照 `meshlet_geometry_impl.cppm::uploadSection`（= 现行大块只读 storage buffer 的
 * 唯一先例）：`VMA_MEMORY_USAGE_CPU_TO_GPU` + `MAPPED` + `HOST_ACCESS_SEQUENTIAL_WRITE`，
 * `memcpy` 进映射指针后**必刷** `vmaFlushAllocation`（映射内存不保证 coherent——独显无
 * large-BAR 时会落到非 coherent 系统内存；coherent 上这次调用是 VMA 内部提前返回，代价 0）。
 * usage 四件：`eIndirectBuffer`（VUID-buffer-02709 硬要求）+ `eStorageBuffer`（★M-1b 的
 * binding 22 让 shader 按 `rowBase + SV_DrawIndex` 读行）+ `eShaderDeviceAddress`（描述符
 * 走 BDA）+ `eTransferSrc`（只为设备级回读校验：本机统一内存上**映射回读是恒真判据**，
 * 真设备往返才证明「GPU 看到的那一行就是我写的那一行」——★1-a0 记过这条）。
 *
 * ## 🔴 尺寸/偏移算术**不在本模块重算**
 * slice 字节数、整张容量、`argsOffset`/`rowBase` 全取 `mdi_rows` 的具名公式（§19.5 末两条
 * buffer 边界 VUID 只能由一条闭式判据守）。本模块只做「按公式校验 → 不过就出声拒写」，
 * 自算第二个偏移 = 造第二个真相源，而它漂了的症状是画错几何而不是报错。
 *
 * ## 生命周期红线（AGENTS §生命周期红线 / docs/archive/issue_history.md 的 UAF 先例）
 * 本服务持一张 GPU buffer，**必须比 Renderer 活得久**（★M-1b 起 Renderer/执行器只持裸指针）。
 * 装配层声明序 = `wireMeshletPostRegistration` 返回的 `unique_ptr`，落在 main 的 try 块里
 * **先于 `backEnd`** 声明（析构序在其后）；🔴 不得放进 `main()` 的局部块——块尾析构 =
 * 第 0 帧起 GPU 侧引用已释放的 VA。
 *
 * ## 建不建 = 阴影簇路径的**宿主条件**（不是 `meshlet.mdi_shadow`）
 * 载体在 `meshlet.assets` 开 + `meshlet.shadow_lod ≥ 0` 且阴影簇路径**真生效**（ctx 里有
 * `MeshletShadowLodTable` = 有条目 474 的宿主）时才建；门关 = **连号都不领**（先例 =
 * `wireMeshletBindings` 首行 `if (!meshletAssets) return std::nullopt;`）。
 * 为什么载体的门不是 `mdi_shadow`：载体是「行表住在 GPU 里」这件事，与「阴影 draw 改用 MDI
 * 形态」这件事（那条才是 `mdi_shadow` = 选 SPV 变体/选条目，D2）分开 ⟹ 出厂默认态（mdi_shadow
 * = false）也建载体，但没有任何一条 draw 引用它 = 图像逐位不变。
 */

namespace RGL {

/**
 * ring×2 行表 buffer 的**裸载体**（不含 descriptor heap 槽）——设备级用例走这一层。
 *
 * 为什么单独成一层（而不是只有 `MeshletShadowArgsService`）：领槽要 `Resources`，而
 * `Resources` 构造要 Swapchain（= 拖进窗口/深度图/两个 heap，`test_meshlet_geometry` 文件头
 * 明确反对的做法）。设备侧真正要验的是「按 `rowBase`/`argsOffset` 取到的那一行是不是我写进去
 * 的那一行」，那件事只需要分配器 + 设备。槽由服务层领，描述符内容 = 本层的 `deviceAddress`。
 */
export struct MeshletShadowArgsRing {
	/** 整张行表（析构即 vmaDestroyBuffer；默认构造 = 空状态）。 */
	VmaBuffer buffer;
	/** 每个帧槽的行数（= `mdiShadowRowLocation` 的 `sliceRows` 入参，两处同一个变量）。 */
	uint32_t sliceRows = 0;
	/** 帧槽数（= `kFramesInFlight`；🔴 不抄字面 2，由调用方取那个唯一真相源）。 */
	uint32_t slices = 0;
	/** 整张字节容量 = `mdiShadowArgsCapacityBytes(sliceRows, slices)`（描述符 range 取本值）。 */
	uint64_t capacityBytes = 0;
	/** BDA 设备地址（0 = 未创建；句柄真值判据用它，不是 `capacityBytes > 0`）。 */
	vk::DeviceAddress deviceAddress = 0;
	/** host 映射指针（CPU 直写的目的端；nullptr = 未映射 = 载体不可用）。 */
	std::byte* mapped = nullptr;

	/**
	 * 一次 MDI 调用的同源 pair（🔴 只做转发，不在这里算偏移：产地 = `mdiShadowRowLocation`）。
	 *
	 * 前置校验（任一不成立返回的 pair **不可用**，调用方必须走 `upload` 那条带错误串的路或
	 * 自己先查）：`frameSlot < slices`、`firstRow < sliceRows`。
	 *
	 * @param frameSlot 帧槽号（与 GPU 侧同一个数，不许另取）
	 * @param firstRow  本段（本级联）在 slice 内的起始行号
	 * @return { argsOffset（字节）, rowBase（行号）}，两者同源于同一个 row
	 */
	[[nodiscard]] MdiShadowRowLocation location(uint32_t frameSlot, uint32_t firstRow) const;

	/**
	 * 本次调用在行表里**实际被设备读到**的字节跨度 = `mdiShadowTailBytes(…) − argsOffset`
	 * （= VUID-07090 的 `stride × (drawCount − 1) + 12`；`drawCount == 0` = 0）。
	 *
	 * 它不是「行数 × 步长」：每行只有前 12B 被读走，多出来的保留 word 不算进跨度。
	 * 用途 = 设备侧回读校验要拷多少字节，以及 `upload` 的边界判据输入。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param drawCount 本次 MDI 调用的行数
	 */
	[[nodiscard]] uint64_t argsBytes(uint32_t frameSlot, uint32_t firstRow, uint32_t drawCount) const;

	/**
	 * 把 `rows` 写进 `(frameSlot, firstRow)` 起的那段行表（memcpy + flush，🔴 不做设备侧同步）。
	 *
	 * 四道前置（顺序 = 由外而内，每条一个**两两不同**的错误串前缀 `meshlet shadow args:`）：
	 *  1. `frameSlot < slices`（越界 = 写进 buffer 尾外的设备内存，不报错只画垃圾）；
	 *  2. `firstRow + rows.size() ≤ sliceRows`（越界 = 踩进下一个帧槽 = 读回上一帧的行）；
	 *  3. `mdiShadowRangeFits(capacityBytes, argsOffset, rows.size())`（07089/07090 合一的
	 *     buffer 边界判据，与容量同一公式）；
	 *  4. 映射指针非空（`MAPPED_BIT` 没生效 = 主机写写进 null，必崩，早退成可归因错误）。
	 * 任一不过 = **一个字节都不写**（半张行表比整张旧行表更坏：逐行读歪没有信号）。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param rows      本段的行（行序 = 候选序，契约在 `mdi_rows` 文件头）；空 span = 合法空操作
	 * @return nullopt = 已写入并 flush；有值 = 可归因错误串（★M-1b 把它打进 WARNING，不抛——
	 *         这是每帧路径）
	 */
	[[nodiscard]] std::optional<std::string> upload(uint32_t frameSlot, uint32_t firstRow,
		std::span<const MdiShadowRow> rows) const;
};

/**
 * 建 ring×2 行表 buffer（host 映射直写形态；设备级用例与装配层共用这一条路径）。
 *
 * 创建后**整张清 0**：VMA 不清内存，未写入区留脏字节 ⟹「按 rowBase 取到的行」在空洞段上
 * 没有确定读数（★M-1b 的孔洞流靠 `gcX = 0` 空转，而 `objectIndex`/`clusterStart` 是垃圾时
 * 空转行也可能被 shader 读走）。清 0 让「没写过的行」成为可判据的事实，也让设备回读用例
 * 能把「未写入区」当反向锚点用。
 *
 * 两道闸门（不过 = 返回 `Error`，不建 buffer）：
 *  · `sliceRows > 0 && slices > 0`（`VkBufferCreateInfo::size` 必须 > 0，零容量无意义）；
 *  · `sliceRows × slices ≤ UINT32_MAX`（`rowBase` 是 push 里的 32 位行号；越界 = 静默截断
 *    = shader 读错行，正是本模块要避免的那类失效）。
 *
 * @param allocator VMA 分配器（引擎侧 = `Resources::getAllocator()`；测试自建第二分配器走同一条路）
 * @param device    逻辑设备（BDA 查询用）
 * @param sliceRows 每个帧槽的行数（= 一个级联段的行数上限，装配层给闭式锚）
 * @param slices    帧槽数（= `FrameSession::kFramesInFlight`）
 * @return 载体（含 `capacityBytes` / `deviceAddress` / `mapped`）；失败 = 可归因 `Error`
 */
export [[nodiscard]] std::expected<MeshletShadowArgsRing, Error> createMeshletShadowArgsRing(
	const VmaAllocatorRAII& allocator, const vk::raii::Device& device, uint32_t sliceRows,
	uint32_t slices);

/**
 * 装配层持有的**行表载体 + 一个 heap 槽**（★M-1a 的对外服务；ring×2 只在这一个对象里）。
 *
 * 构造做三件事（顺序 = 先领号后建 buffer 都行，但描述符必须在 buffer 地址已知之后写）：
 *  ① `resources.allocSlot(SlotSegment::Runtime, "meshlet.gpu.shadowArgs")` 领**一枚**槽
 *     （名字带 `meshlet.gpu.` 前缀 = 既有命名法，`wireMeshletBindings` 同源）；
 *  ② `createMeshletShadowArgsRing` 建整张 buffer；
 *  ③ 把 BDA 写成 storage buffer 描述符，**range = 整张容量**（覆盖所有 slice ⟹ 逐帧轮转
 *     不改描述符），host 直写不进命令流（`MeshletCullService::writeSlotDescriptor` 同法）。
 * 失败一律抛 `std::runtime_error`（启动期一次性路径，同 `MeshletCullService`）。
 *
 * 🔴 本批**没有任何消费者**：`argSlot()` 是给 ★M-1b 烘 binding 22 用的（届时条目侧声明
 * binding 与领槽必须同向，见 §19.5 的「声明 / mapping 必须同向」那条），`bufferAddress()`
 * 是给 `vkCmdDrawMeshTasksIndirectEXT` 的 `buffer` 参数用的（同一个 BDA 走两条路：
 * vkCmd 收句柄 + offset，shader 收描述符里的 address range）。
 */
export class MeshletShadowArgsService {
   public:
	/**
	 * 构造（启动期一次，由装配层的宿主条件决定建不建）。
	 *
	 * @param device    设备层（描述符写入取 device 句柄）
	 * @param resources 资源层（VMA 分配器 / allocSlot / descriptor heap 槽地址与步长）
	 * @param sliceRows 每个帧槽的行数 = 一个级联段的行数上限（装配层给**闭式锚**，见 INFO）
	 * @param slices    帧槽数（= `FrameSession::kFramesInFlight`，🔴 不在本模块写死 2）
	 * @throw std::runtime_error 领槽 / buffer 创建 / 描述符写入失败（启动期一次性路径）
	 */
	MeshletShadowArgsService(DeviceContext& device, Resources& resources, uint32_t sliceRows,
		uint32_t slices);
	/** 拷贝删除：本类持一张 GPU buffer + 一个 heap 槽，所有权唯一（同 `MeshletCullService`）。 */
	MeshletShadowArgsService(const MeshletShadowArgsService&) = delete;
	/** 拷贝赋值删除：同上。 */
	MeshletShadowArgsService& operator=(const MeshletShadowArgsService&) = delete;
	/** 移动删除：槽号届时已烘进管线条目（★M-1b），挪窝 = 条目指向的槽不变而 buffer 换了主人。 */
	MeshletShadowArgsService(MeshletShadowArgsService&&) = delete;
	/** 移动赋值删除：同上。 */
	MeshletShadowArgsService& operator=(MeshletShadowArgsService&&) = delete;

	/**
	 * 一次 MDI 调用的同源 pair（转发环层，🔴 不在调用点自己算偏移——见 `mdiShadowRowLocation`）。
	 *
	 * @param frameSlot 帧槽号（= `FrameSession::currentFrame`）
	 * @param firstRow  本段（本级联）在该帧槽内的起始行号
	 */
	[[nodiscard]] MdiShadowRowLocation location(uint32_t frameSlot, uint32_t firstRow) const;

	/**
	 * 本次调用的字节跨度（= `mdiShadowTailBytes − argsOffset`，用途见 `MeshletShadowArgsRing::argsBytes`）。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param drawCount 本次 MDI 调用的行数
	 */
	[[nodiscard]] uint64_t argsBytes(uint32_t frameSlot, uint32_t firstRow, uint32_t drawCount) const;

	/**
	 * 写一段行进 ring（每帧路径；🔴 失败返回错误串而不抛，四道前置见环层注释）。
	 *
	 * @param frameSlot 帧槽号
	 * @param firstRow  本段起始行号
	 * @param rows      本段的行（空 span = 合法空操作）
	 */
	[[nodiscard]] std::optional<std::string> upload(uint32_t frameSlot, uint32_t firstRow,
		std::span<const MdiShadowRow> rows) const;

	/** @return 行表的 descriptor heap 槽号（★M-1b 烘进 MDI 变体的 binding 22）。 */
	[[nodiscard]] uint32_t argSlot() const { return argHeapSlot; }
	/** @return 整张行表的 BDA 设备地址（描述符内容；0 = 未创建，句柄真值判据用它）。 */
	[[nodiscard]] vk::DeviceAddress bufferAddress() const { return ring.deviceAddress; }
	/** @return 整张容量字节数（= 描述符 range，也是 07089/07090 的判据输入）。 */
	[[nodiscard]] uint64_t capacityBytes() const { return ring.capacityBytes; }
	/** @return 每个帧槽的行数（闭式锚，INFO 与 ★M-1b 的对账都读它）。 */
	[[nodiscard]] uint32_t sliceRows() const { return ring.sliceRows; }
	/** @return 帧槽数（轮转分母）。 */
	[[nodiscard]] uint32_t slices() const { return ring.slices; }

	/**
	 * @return 裸 VkBuffer 句柄。
	 * 🔴 只给**设备级回读校验**用（`vkCmdCopyBuffer` 的 src 收句柄而不是地址）；渲染路径
	 * 一律走 `argSlot()` 的描述符或 `bufferAddress()`，别拿这个句柄去建第二条访问路。
	 */
	[[nodiscard]] VkBuffer buffer() const { return ring.buffer.buffer; }
	/**
	 * @return host 映射指针（CPU 写侧目的端）。
	 * ⚠ 统一内存上「读回它」是**恒真判据**（★1-a0 记过）：只作设备往返之外的辅助锚点。
	 */
	[[nodiscard]] const std::byte* mappedData() const { return ring.mapped; }

   private:
	/** 构造期：领 Runtime 段的一枚 heap 槽（ring×2 一张 buffer ⟹ 一槽，见类注释③）。 */
	void allocArgSlot();
	/**
	 * 构造期：建整张 ring×2 buffer（分配口径与四道 usage 见文件头；失败抛，启动期一次性路径）。
	 *
	 * @param sliceRows 每个帧槽的行数
	 * @param slices    帧槽数
	 */
	void createRing(uint32_t sliceRows, uint32_t slices);
	/**
	 * 构造期：把 ring 的 BDA 写成槽的 storage buffer 描述符（range = 整张容量）。
	 *
	 * 描述符内容 = 同一张 buffer 的地址，与 `MeshletCullService::writeSlotDescriptor` 同款
	 * （`writeResourceDescriptorsEXT` + `vk::DeviceAddressRangeEXT`，host 直写）。
	 */
	void writeSlotDescriptor();

	DeviceContext& device;    ///< 设备层（描述符写入）
	Resources& resources;     ///< 资源层（分配器 / allocSlot / heap 槽地址与步长）
	MeshletShadowArgsRing ring;  ///< 整张 ring×2 行表（本类唯一 GPU 资源，析构即销毁）
	uint32_t argHeapSlot = 0; ///< 行表 heap 槽号（★M-1b 的 binding 22 取它）
};

/**
 * INFO 的**闭式锚**（★M-1a 要求「让人能一眼对账」的那五个数），全部由 `mdi_rows` 的具名
 * 公式算出——**不许**从 `MeshletShadowArgsRing` 的成员读（那是被测对象自己报数，
 * 与公式分家时日志仍然自洽 = 假绿；同 `mdi_rows` 的「聚合锚期望值独立重算」纪律）。
 *
 * 最坏形态（`firstRow = 0, drawCount = sliceRows` 落进**末个**帧槽）是容量必须覆盖的那一格：
 * 它 = `mdiShadowArgsCapacityBytes` 减 `stride` 再加 12（`stride ≥ 12` 恒成立 ⟹ 永不越界，
 * 且容量少 1 字节立刻红，见 tests/test_mdi_rows）。
 */
export struct MeshletShadowArgsAnchors {
	uint32_t rows = 0;      ///< 每个帧槽的行数（闭式锚 = 级联数 × 本值 = 启动对账的 Σrows）
	uint32_t slices = 0;    ///< 帧槽数（= `kFramesInFlight`）
	uint32_t stride = 0;    ///< 行步长（= `kMdiShadowRowStride`，VUID-07088 的 stride）
	uint64_t capacity = 0;  ///< 整张字节容量（= `mdiShadowArgsCapacityBytes`）
	uint64_t tail = 0;      ///< 末行 `offset + 12`（= `mdiShadowTailBytes`，07089/07090 的左边）
};

/**
 * 算出上面那五个数（纯公式，无设备）。
 * @param sliceRows 每个帧槽的行数
 * @param slices    帧槽数
 */
export [[nodiscard]] MeshletShadowArgsAnchors meshletShadowArgsAnchors(uint32_t sliceRows,
	uint32_t slices);

}  // namespace RGL
