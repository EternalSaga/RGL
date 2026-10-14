module;
#include <vulkan/vulkan.h>  // C 句柄/地址类型（VkBuffer/VkDeviceAddress 访问器返回）；offsetof 经其 stddef.h 提供
export module gpu_cull;

import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import scene_data;  // SceneDataService（架构债务续账步骤 1：scene data buffer 移出内核）
import offgraph_sync;  // 批次 B1b：图外同步发射账（8 条 barrier 的参数由边表物化）
import VmaRaii;
import frame_draw_list;
import material_types_generated;  // DrawArgs（writeSequenceStaging：序列 CPU 侧基值 + 槽区宽度断言）

/**
 * GPU culling 三张 buffer 的**元素契约**（`CullEntry` / `CullSequence` / `CullCount`）。
 * 真相源 = `rgl/shaders/cull_tables.slang`，经 `cull_tables_schema.lua` 反射出本生成物；
 * `cull.comp.slang` 与 `stream_fanout.comp.slang` 读的是**同一份声明**。迁移前这里是手写
 * `struct CullEntry` + `kCullSequenceStride` + `kCullSeq*` 四常量 + 三条 `static_assert`，
 * shader 侧再手抄第三、四份（`kEntryStride = 60u` / `kSequenceStride = 120u` / `+108u` /
 * `8u..120u` 拷贝段），机器只比 C++ 单边——原注释自己承认："跨侧漂移靠三条 static_assert 拦
 * C++ 侧单边改动 + 两侧注释互指"。现在**抄写那一步不存在了**：改 shader 字段 ⟹ 生成物跟着变 ⟹
 * 本模块与 vk_indirect_commands 的 offsetof 取值点自动跟上；改不动的是 schema 交叉校验
 * （just reflect 期 [FAIL]）。
 * 🔴 **为什么 `export import`**：本模块的公开接口直接吃/吐这些类型（构造参数与 `entries()` 收
 * `std::vector<CullEntry>`、`readbackSequences()` 的步长 = `sizeof(CullSequence)`），而 C++20
 * 的 import **不传递** ⟹ 不 export 就得让 main_scene / cull_driver / vk_indirect_commands 各补
 * 一句 import（先例 = `features.cppm` 的 `export import features_generated;`）。
 */
export import cull_tables_types_generated;

namespace RGL {

/**
 * 序列行内不变式（债-12 护栏的**继任者**：原三条 static_assert 量的是手抄进 C++ 的常量，是关于
 * 副本的同义反复——shader 单边改布局时它们照样全过；本组量的是反射实查的 shader 布局）。
 * 逐字段偏移由生成物自带的 sizeof/offsetof assert 钉；这里补的是**关系**——生成物逐字段报偏移，
 * 不报"它们必须相邻 / 恰好收尾"。运行期的同一批不变式另有独立判据
 * （tests/test_cull_tables_layout.cppm，期望值出处 = 迁移前人工核对过的字节表，不抄生成物）。
 */
// #1 execSlot 2×u32 必须**连续**：DGC EXECUTION_SET token 按 shaderStages 位 LSB→MSB 读 2 个
//     连续 u32（第 1 位 = FRAGMENT = LSB、第 2 位 = MESH），execution set 下标约定 2j = frag /
//     2j+1 = mesh（CPU writeSequenceStaging 按同约定写值）。
static_assert(offsetof(CullSequence, execSlotFrag) + 4 == offsetof(CullSequence, execSlotMesh)
		&& offsetof(CullSequence, execSlotMesh) + 4 == offsetof(CullSequence, drawArgs),
	"cull 序列 execSlot 2×u32 与 DrawArgs 的相邻契约漂移（消费者 = vk_indirect_commands 的 "
	"EXECUTION_SET token：它按 shaderStages 位读 2 个连续 u32，中间插字段 = execution set 下标读错）");
// #2 meshCmd 12B 必须恰好收尾于序列行末端：DGC DRAW_MESH_TASKS token 从 token.offset 起按
//     sizeof(VkDrawMeshTasksIndirectCommandEXT) 取字节，越出行 = 读进下一条序列。
static_assert(offsetof(CullSequence, meshCmdX) + sizeof(VkDrawMeshTasksIndirectCommandEXT)
		== sizeof(CullSequence),
	"meshCmd 12B 必须恰好收尾于序列行末端（序列步长的另一半现在住在 CullSequence 这个类型里，"
	"不再是 cull.comp.slang / stream_fanout.comp.slang 的 kSequenceStride = 120u 字面量）");
// #3 DrawArgs 不得超出 drawArgs..meshCmdX 的保留槽区（DGC PUSH_DATA token 按 sizeof(DrawArgs) 推）。
static_assert(sizeof(RGL::DrawArgs)
		<= offsetof(CullSequence, meshCmdX) - offsetof(CullSequence, drawArgs),
	"DrawArgs 超出 DrawArgs 槽区保留宽度 = 序列布局契约破裂（DrawArgs 是反射生成物，4B 契约见 "
	"tests/test_reflection_layout.cppm 组 2；DGC PUSH_DATA token 按 sizeof(DrawArgs) 推）");

/** count buffer 字节数（`CullCount` 只占前 sizeof(CullCount) 字节，其余是历史留白——旧 visible
 * atomicAdd 无人清零已删，2026-09-15；64B 对齐留白）。buffer 尺寸**不在**契约类型里：契约管元素
 * 布局，这里管分配；total 的落点由 `offsetof(CullCount, total)` 取（见 countTotalAddress）。 */
export inline constexpr uint32_t kCullCountBytes = 64;

/**
 * GPU culling 子系统（Phase 7b 批次 5；main.cpp 初始化层构造，先于 Renderer——
 * Renderer 持引用在 drawFrame 数据通路固定段消费）。职责 = 静态表上传 + 输出缓冲 +
 * compute shader object + 每帧 dispatch 录制 + 读回；提交方式（DGC execute / 读回
 * 重放）由 Renderer 按 DeviceContext::DgcSupport 能力位选路（本模块不感知提交）。
 * 候选表空 = inactive（drawFrame 全路径不碰 culling，行为中性回落 CPU 路）。
 * **顺序契约**：candidate 序 = 构建方（main）的 registry 迭代序 = render_list CPU
 * opaque 段序——GPU 输出固定槽非紧凑，重放序与 CPU 逐位一致（截图 BIT-IDENTICAL 前提）。
 */
export class GpuCull {
   public:
	/**
	 * 构建 culling 子系统（启动期一次性）：静态表 buffer（host-visible + BDA）+
	 * 序列/ count 输出 buffer（device-local + BDA + eTransferSrc）+ 读回 ring
	 * （×2 host-visible）+ 四个 resource heap 描述符槽（allocSlot）+ compute
	 * shader object（stage=COMPUTE + heap bit + 4 binding 映射，RenderPipeline
	 * 同款模式）+ 表数据 memcpy。
	 * @param vkDevice 设备层（buffer / shader 创建 / 描述符写入）
	 * @param resources 资源层（VMA 分配器 / allocSlot / 描述符 heap）
	 * @param sceneData scene data 服务（幂等领 buffer/槽 + staging 拷贝目标，架构债务续账步骤 1）
	 * @param entries 候选表（main 从 registry 构建，顺序契约见类注释）；空 = inactive
	 * @param entryStreamCount 静态 shader DGC 路的 per-entry 流数（>0 = 启用：分配
	 *   entryStreamCount × candidateCount × sizeof(CullSequence) 流 buffer + fanout shader
	 *   object，recordCullPass 尾部追加扇出 dispatch；0 = 不建，NV/重放路零开销——
	 *   docs/archive/radv_static_dgc_spike.md §3.2/§3.3）
	 */
	GpuCull(RGL::DeviceContext& vkDevice, RGL::Resources& resources,
		RGL::SceneDataService& sceneData,
		const std::vector<CullEntry>& entries, uint32_t entryStreamCount = 0);

	/** @return 子系统是否激活（候选表非空；Renderer 据此决定是否走 culling 数据通路）。 */
	[[nodiscard]] bool active() const { return nCandidates > 0; }
	/** @return 候选数（序列 buffer 容量 = dispatch group 数派生源 = DGC maxSequenceCount）。 */
	[[nodiscard]] uint32_t candidateCount() const { return nCandidates; }
	/** @return 候选表 const 引用（对账/重放经 entries[i].objectSlot 映射 CPU draw）。 */
	[[nodiscard]] const std::vector<CullEntry>& entries() const { return entryTable; }
	/**
	 * 序列 staging 写 CPU 侧基值（recordCullPass 之前调用）：逐 candidate
	 * execSlotFrag（= 2×pipelineIndex）+ execSlotMesh（= 2×pipelineIndex+1）+
	 * DrawArgs（4B = objectIndex，material_types_generated；mvp 走 scene data
	 * per-object 表，CPU = 位级真相源）+ meshCmd 三量（x=groupCount y=z=1，
	 * GPU dispatch 只覆盖 x）——落点全部经 `CullSequence` 的字段取，本模块零字节字面量。
	 * @param slot 帧 slot（staging ring 下标，0/1）
	 * @param args 逐 candidate DrawArgs（candidate 序，render_list 产出；
	 *             size 必须 = candidateCount，否则越界）
	 */
	void writeSequenceStaging(uint32_t slot, const std::vector<RGL::DrawArgs>& args);
	/**
	 * 在指定命令缓冲上录制 culling 数据通路（自含：heap 边 → scene data 拷贝 →
	 * 序列 staging 拷贝（CPU 基值 → device 序列）+ Transfer→Compute 边 → heap 绑定 →
	 * bindShadersEXT + pushDataEXT + dispatch（GPU 覆盖 meshCmdX + count）→
	 * Compute→Transfer 边 → 读回拷贝 → Transfer→HostRead 边）。调用方提交本 CB
	 * 并同步等 fence 后调 readCullResult。
	 * @param cmd 命令缓冲（录制期；调用方负责 begin/end/submit）
	 * @param input 每帧 culling 输入（视锥 6 面 + candidate 数）
	 * @param sceneStaging scene data staging ring 本帧 slot（拷贝源；调用方已写完）
	 * @param slot 帧 slot（staging/读回 ring 下标，0/1）
	 * @param sync 图外同步发射账（批次 B1b：8 条 barrier 的参数由 `offgraph_sync` 的 usage
	 *        词表物化，本模块只写边名 + buffer 句柄，不写 stage/access 字面量）。
	 *        🔴 **为什么是形参而不是成员**：本类在 `main_scene.cppm` 构造，**早于 Renderer**
	 *        （Renderer 只持其裸指针），而 `OffGraphSync` 是 `Renderer` 的成员 ⟹ 构造期那个
	 *        引用根本还不存在。IzbShadow / EvssmShadow 能存成员是因为它们在 Renderer 内懒构造。
	 */
	void recordCullPass(vk::raii::CommandBuffer& cmd, const CullFrameInput& input,
		RGL::VmaBuffer& sceneStaging, uint32_t slot, OffGraphSync& sync);
	/**
	 * 读回结果（调用方已同步等待本帧 culling 提交 fence）：visible 计数 + 逐 candidate
	 * 可见位（不可见 = 序列 meshCmdX == 0）。
	 * @param slot 帧 slot（与 recordCullPass 同值）
	 * @return 可见序列数（由序列 meshCmdX 位统计，非 count buffer 的 reserved——它保留未用）
	 */
	uint32_t readCullResult(uint32_t slot);
	/** @return candidate i 是否可见（readCullResult 后有效）。 */
	[[nodiscard]] bool isVisible(uint32_t i) const { return visibleFlags[i]; }
	/** @return 序列 buffer 句柄（DGC 路调用方置 Compute→CommandPreprocess 屏障）。 */
	[[nodiscard]] VkBuffer sequenceBufferHandle() const { return *sequenceBuffer; }
	/** @return 序列 buffer device 地址（DGC execute/preprocess 的 indirectAddress）。 */
	[[nodiscard]] VkDeviceAddress sequenceAddress() const { return sequenceBuffer.getDeviceAddress(); }
	/** @return count 区的 `CullCount::total`（= candidate 数，GPU thread0 写）的 device 地址
	 *（DGC sequenceCountAddress；非紧凑语义）。偏移取自契约类型，不写裸 4。 */
	[[nodiscard]] VkDeviceAddress countTotalAddress() const {
		return countBuffer.getDeviceAddress() + offsetof(CullCount, total);
	}
	/**
	 * @return 本帧读回的序列区 host 指针（元素 = `CullSequence`，步长 sizeof(CullSequence)；
	 * 重放消费 execSlot 两量 / DrawArgs / meshCmd 三量）。未 readCullResult = nullptr。
	 */
	[[nodiscard]] const std::byte* readbackSequences(uint32_t slot) const { return readbackSeqPtr[slot]; }
	/** @return 静态 DGC 路的 per-entry 流是否启用（= 构造期 entryStreamCount > 0）。 */
	[[nodiscard]] bool entryStreamsEnabled() const { return nEntryStreams > 0; }
	/** @return 流 buffer device 地址（条目 j 流基址 = 此值 + j × candidateCount × sizeof(CullSequence)）。 */
	[[nodiscard]] VkDeviceAddress streamsAddress() const { return streamsBuffer.getDeviceAddress(); }

   private:
	/**
	 * 构造期：静态表 / 序列 / count / 流 buffer 分配（候选表空 = 跳过表/序列/count
	 * 段；流仅 nEntryStreams>0 时建）。
	 * @param device 设备层（VMA createBuffer[WithAddress]）
	 */
	void createCullBuffers(vk::raii::Device& device);
	/**
	 * 构造期：序列 staging ring×2 + 读回 ring×2（host-visible 常驻映射）。
	 */
	void createHostRings();
	/**
	 * 构造期：运行时段 heap 槽分配（cull.table/sequences/count[/streams] +
	 * SceneDataService::ensure 幂等 scene data 槽）。
	 */
	void allocRuntimeSlots();
	/**
	 * 构造期：descriptor heap 写入（eStorageBuffer + BDA 全范围；
	 * SceneDataService::ensure 同款模式；候选表空 = 跳过）。
	 * @param device 设备层（writeResourceDescriptorsEXT）
	 */
	void writeHeapDescriptors(vk::raii::Device& device);
	/**
	 * 构造期：compute cull shader object（4 binding heap 映射）+（nEntryStreams>0）
	 * stream fanout shader object（3 binding）。
	 * @param device 设备层（createShadersEXT）
	 */
	void createCullShaders(vk::raii::Device& device);
	/**
	 * 静态 DGC 路：基础序列 → per-entry 流扇出 dispatch（recordCullPass 内 nEntryStreams>0
	 * 分支抽出）。Compute→Compute 边后绑 fanoutShader + pushData(sequenceCount) +
	 * dispatch(candidate, nEntryStreams)。nEntryStreams==0 时调用方跳过（NV/重放路零开销）。
	 * @param cmd recordCullPass 正在录制的同一命令缓冲（pass 外）
	 * @param sync 同 recordCullPass（形参而非成员的理由写在那处）
	 */
	void recordFanoutDispatch(vk::raii::CommandBuffer& cmd, OffGraphSync& sync);

	RGL::Resources& resources;  // 资源层引用（记录期 heap 信息；main 栈对象生命周期覆盖）
	RGL::SceneDataService& sceneData;  // scene data 服务引用（buffer / 槽；架构债务续账步骤 1）
	std::vector<CullEntry> entryTable;  // 候选表（构建期存副本；对账/重放映射用）
	uint32_t nCandidates = 0;

	RGL::VmaBuffer tableBuffer;  // 静态表（host-visible + BDA + eStorageBuffer；启动期 memcpy）
	RGL::VmaBuffer sequenceBuffer;  // 输出序列（device-local + BDA + eTransferSrc/Dst；candidateCount × sizeof(CullSequence)）
	RGL::VmaBuffer countBuffer;  // 输出计数（device-local + BDA + eTransferSrc；64B）
	std::array<RGL::VmaBuffer, 2> seqStaging;  // 序列 staging ring（×2 host-visible；CPU 写基值 → 拷入 device 序列）
	std::array<void*, 2> seqStagingMapped{};  // 序列 staging 映射指针
	std::array<RGL::VmaBuffer, 2> readback;  // 读回 ring（×2 host-visible；[0..64)=count 区 [64..)=序列区）
	std::array<void*, 2> readbackMapped{};  // 读回 ring 映射指针（createBuffer 不回传，随分配自存）
	std::array<const std::byte*, 2> readbackSeqPtr{};  // readCullResult 后 = 序列区入口（重放用）

	uint32_t tableSlot = 0;  // 静态表 heap 槽（allocSlot）
	uint32_t sequenceSlot = 0;  // 序列 buffer heap 槽
	uint32_t countSlot = 0;  // count buffer heap 槽
	uint32_t objectMatrixSlot = 0;  // 对象矩阵表 SSBO 子范围槽（SceneDataService::objectMatrixSlot，compute binding 23）

	// 静态 shader DGC 路（spike，docs/archive/radv_static_dgc_spike.md §3.2/§3.3；
	// nEntryStreams = 0 时以下全不创建，NV/重放路零开销）
	uint32_t nEntryStreams = 0;         // per-entry 流数（= 构造期管线表条目数）
	RGL::VmaBuffer streamsBuffer;      // per-entry 满槽孔洞流（N×candidateCount×sizeof(CullSequence)；
	                                   // device-local + BDA + INDIRECT_BUFFER[VUID-12407]）
	uint32_t streamsSlot = 0;          // 流 buffer heap 槽（fanout kernel binding 2）
	vk::raii::ShaderEXT fanoutShader = nullptr;  // stream_fanout.comp（3 binding；仅启用时建）

	vk::raii::ShaderEXT cullShader = nullptr;  // compute shader object（4 binding 映射烘焙；inactive 保持 null）
	std::vector<uint8_t> visibleFlags;  // readCullResult 填充（candidate 可见位）
	uint32_t lastVisible = 0;  // 最近一次 readCullResult 的 visible 数
};

}  // namespace RGL
