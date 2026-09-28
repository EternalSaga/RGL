module;
#include <glm/glm.hpp>
#include <vulkan/vulkan.h>  // C 句柄/地址类型（VkBuffer/VkDeviceAddress 访问器返回）
export module gpu_cull;

import rlstd;
import rlvk;
import rllogger;
import vkerror;
import vk_device;
import vk_resources;
import scene_data;  // SceneDataService（架构债务续账步骤 1：scene data buffer 移出内核）
import VmaRaii;
import frame_draw_list;
import material_types_generated;  // DrawArgs（writeSequenceStaging：序列 CPU 侧基值）

namespace RGL {

/**
 * GPU culling 静态候选表条目（Phase 7b 批次 5）：per-candidate 一次性上传（host 写，
 * 每帧不变）。C++/shader 布局契约单一真相源 = 本 struct（cull.comp.slang 字面量必须
 * 一致——scene.slang 先例，本表不接反射管线）：
 *   @0  center vec3（node-local 剔除球心，与 CullSphere 组件同源）
 *   @12 radius
 *   @16 objectSlot（scene data 对象矩阵表下标 = MeshRef.objectSlot，实例级）
 *   @20 pipelineIndex（管线表下标 = DGC execution set 槽 / 重放管线表下标）
 *   @24 groupCount（mesh task group 数 = MeshAsset.groupCount）
 *   @28 totalTriangles / @32 vertexOffset（DrawArgs 透传字段，MeshAsset 同源）
 *   @36 baseColor vec4 / @52 roughnessMetallic vec2（MaterialAsset 同源，x=roughness）
 */
export struct CullEntry {
	glm::vec3 center{0.0f};
	float radius = 0.0f;
	uint32_t objectSlot = 0;
	uint32_t pipelineIndex = 0;
	uint32_t groupCount = 0;
	uint32_t totalTriangles = 0;
	uint32_t vertexOffset = 0;
	glm::vec4 baseColor{1.0f};
	glm::vec2 roughnessMetallic{0.0f};
};
static_assert(sizeof(CullEntry) == 60, "CullEntry layout drifted from cull.comp.slang");

/** 输出序列步长（120B：execSlot 2×u32（@0 frag / @4 mesh——DGC EXECUTION_SET token 按
 * shaderStages 位 LSB→MSB 读 u32 数组）+ DrawArgs 槽区（@8..108 共 100B 保留宽度，
 * DrawArgs 本身 Phase 7b+ 起 4B = 仅 objectIndex，其余 96B 空置不写）+
 * VkDrawMeshTasksIndirectCommandEXT 12（@108）；cull.comp.slang / DGC layout token
 * 偏移同源）。读回重放路只消费可见性（@108.x）与 CPU 侧 cullArgs——execSlot 仅
 * DGC 路消费（值 = 2×pipelineIndex / 2×pipelineIndex+1，execution set 下标约定见
 * vk_indirect_commands.cppm）。≤ RADV maxIndirectCommandsIndirectStride 2048。 */
export inline constexpr uint32_t kCullSequenceStride = 120;
/** count buffer 字节数（@0 保留未用——旧 visible atomicAdd 无人清零已删，2026-09-15
 * | @4 total 每帧 thread0 写；64B 对齐留白）。 */
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
	 *   entryStreamCount × candidateCount × 120B 流 buffer + fanout shader object，
	 *   recordCullPass 尾部追加扇出 dispatch；0 = 不建，NV/重放路零开销——
	 *   docs/radv_static_dgc_spike.md §3.2/§3.3）
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
	 * execSlotFrag@0（= 2×pipelineIndex）+ execSlotMesh@4（= 2×pipelineIndex+1）+
	 * DrawArgs@8（4B = objectIndex，material_types_generated；mvp 走 scene data
	 * per-object 表，CPU = 位级真相源）+ meshCmd@108（x=groupCount y=z=1，
	 * GPU dispatch 只覆盖 x）——偏移与 gpu_cull_impl.cppm writeSequenceStaging
	 * 实现及 cull.comp.slang 头注释同源。
	 * @param slot 帧 slot（staging ring 下标，0/1）
	 * @param args 逐 candidate DrawArgs（candidate 序，render_list 产出；
	 *             size 必须 = candidateCount，否则越界）
	 */
	void writeSequenceStaging(uint32_t slot, const std::vector<RGL::DrawArgs>& args);
	/**
	 * 在指定命令缓冲上录制 culling 数据通路（自含：heap barrier → scene data 拷贝 →
	 * 序列 staging 拷贝（CPU 基值 → device 序列）+ Transfer→Compute 屏障 → heap 绑定 →
	 * bindShadersEXT + pushDataEXT + dispatch（GPU 覆盖 meshCmd.x + count）→
	 * Compute→Transfer 屏障 → 读回拷贝 → Transfer→HostRead 屏障）。调用方提交本 CB
	 * 并同步等 fence 后调 readCullResult。
	 * @param cmd 命令缓冲（录制期；调用方负责 begin/end/submit）
	 * @param input 每帧 culling 输入（视锥 6 面 + candidate 数）
	 * @param sceneStaging scene data staging ring 本帧 slot（拷贝源；调用方已写完）
	 * @param slot 帧 slot（staging/读回 ring 下标，0/1）
	 */
	void recordCullPass(vk::raii::CommandBuffer& cmd, const CullFrameInput& input,
		RGL::VmaBuffer& sceneStaging, uint32_t slot);
	/**
	 * 读回结果（调用方已同步等待本帧 culling 提交 fence）：visible 计数 + 逐 candidate
	 * 可见位（不可见 = 序列 meshCmd.x == 0）。
	 * @param slot 帧 slot（与 recordCullPass 同值）
	 * @return 可见序列数（由序列 meshCmd.x 位统计，非 count buffer @0——@0 保留未用）
	 */
	uint32_t readCullResult(uint32_t slot);
	/** @return candidate i 是否可见（readCullResult 后有效）。 */
	[[nodiscard]] bool isVisible(uint32_t i) const { return visibleFlags[i]; }
	/** @return 序列 buffer 句柄（DGC 路调用方置 Compute→CommandPreprocess 屏障）。 */
	[[nodiscard]] VkBuffer sequenceBufferHandle() const { return *sequenceBuffer; }
	/** @return 序列 buffer device 地址（DGC execute/preprocess 的 indirectAddress）。 */
	[[nodiscard]] VkDeviceAddress sequenceAddress() const { return sequenceBuffer.getDeviceAddress(); }
	/** @return count buffer @4（total = candidate 数，GPU thread0 写）device 地址
	 *（DGC sequenceCountAddress；非紧凑语义）。 */
	[[nodiscard]] VkDeviceAddress countTotalAddress() const { return countBuffer.getDeviceAddress() + 4; }
	/**
	 * @return 本帧读回的序列区 host 指针（布局 = kCullSequenceStride 步长，
	 * [slot 区基址 + i×120]：execSlot@0/@4 / DrawArgs@8 / meshCmd@108；重放消费）。
	 * 未 readCullResult = nullptr。
	 */
	[[nodiscard]] const std::byte* readbackSequences(uint32_t slot) const { return readbackSeqPtr[slot]; }
	/** @return 静态 DGC 路的 per-entry 流是否启用（= 构造期 entryStreamCount > 0）。 */
	[[nodiscard]] bool entryStreamsEnabled() const { return nEntryStreams > 0; }
	/** @return 流 buffer device 地址（条目 j 流基址 = 此值 + j × candidateCount × kCullSequenceStride）。 */
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
	 * 分支抽出）。Compute→Compute 屏障后绑 fanoutShader + pushData(sequenceCount) +
	 * dispatch(candidate, nEntryStreams)。nEntryStreams==0 时调用方跳过（NV/重放路零开销）。
	 * @param cmd recordCullPass 正在录制的同一命令缓冲（pass 外）
	 */
	void recordFanoutDispatch(vk::raii::CommandBuffer& cmd);

	RGL::Resources& resources;  // 资源层引用（记录期 heap 信息；main 栈对象生命周期覆盖）
	RGL::SceneDataService& sceneData;  // scene data 服务引用（buffer / 槽；架构债务续账步骤 1）
	std::vector<CullEntry> entryTable;  // 候选表（构建期存副本；对账/重放映射用）
	uint32_t nCandidates = 0;

	RGL::VmaBuffer tableBuffer;  // 静态表（host-visible + BDA + eStorageBuffer；启动期 memcpy）
	RGL::VmaBuffer sequenceBuffer;  // 输出序列（device-local + BDA + eTransferSrc/Dst；candidateCount × 120B）
	RGL::VmaBuffer countBuffer;  // 输出计数（device-local + BDA + eTransferSrc；64B）
	std::array<RGL::VmaBuffer, 2> seqStaging;  // 序列 staging ring（×2 host-visible；CPU 写基值 → 拷入 device 序列）
	std::array<void*, 2> seqStagingMapped{};  // 序列 staging 映射指针
	std::array<RGL::VmaBuffer, 2> readback;  // 读回 ring（×2 host-visible；[0..64)=count 区 [64..)=序列区）
	std::array<void*, 2> readbackMapped{};  // 读回 ring 映射指针（createBuffer 不回传，随分配自存）
	std::array<const std::byte*, 2> readbackSeqPtr{};  // readCullResult 后 = 序列区入口（重放用）

	uint32_t tableSlot = 0;  // 静态表 heap 槽（allocSlot）
	uint32_t sequenceSlot = 0;  // 序列 buffer heap 槽
	uint32_t countSlot = 0;  // count buffer heap 槽
	uint32_t sceneDataSlot = 0;  // scene data heap 槽（SceneDataService::ensure 幂等返回）

	// 静态 shader DGC 路（spike，docs/radv_static_dgc_spike.md §3.2/§3.3；
	// nEntryStreams = 0 时以下全不创建，NV/重放路零开销）
	uint32_t nEntryStreams = 0;         // per-entry 流数（= 构造期管线表条目数）
	RGL::VmaBuffer streamsBuffer;      // per-entry 满槽孔洞流（N×candidateCount×120B；
	                                   // device-local + BDA + INDIRECT_BUFFER[VUID-12407]）
	uint32_t streamsSlot = 0;          // 流 buffer heap 槽（fanout kernel binding 2）
	vk::raii::ShaderEXT fanoutShader = nullptr;  // stream_fanout.comp（3 binding；仅启用时建）

	vk::raii::ShaderEXT cullShader = nullptr;  // compute shader object（4 binding 映射烘焙；inactive 保持 null）
	std::vector<uint8_t> visibleFlags;  // readCullResult 填充（candidate 可见位）
	uint32_t lastVisible = 0;  // 最近一次 readCullResult 的 visible 数
};

}  // namespace RGL
