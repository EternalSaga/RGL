module;
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <fmt/format.h>
export module vk_resources;

import rlstd;
import rlvk;
import rllogger;
import lights_types_generated;  // Phase 7 D3：GpuLight struct + kMaxLights/kGpuLightStride/kSceneDataLightArrayBase
import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import cube;
import vkerror;
import types_generated;
import model_types_generated;

// 资产层（AssetKind/MeshAsset/TextureAsset/MaterialAsset/AssetTable）拆出为独立模块——
// 本模块 export import 保持 Resources 接口（getAssetTable 等）类型对外可见
// （2026-09-02 拆分）
export import asset_table;

namespace RGL {

// ===== SceneData：场景级每帧动态数据的 bindless 契约（Phase 5 批次 2）=====
// 单一 storage buffer（heap slot 经 allocSlot 运行时分配——纹理槽数随场景动态，
// light 槽无法钉编译期偏移，同 slotTexture 模式），布局（字节偏移，shader 端
// scene.slang 字面量必须一致，kTrianglesPerGroup 先例）：
//   @0   LightDataGpu（48B：direction/eyePos/color/ambient 各 vec3）
//   @48  Phase 5.5 env 参数（原 16B 保留区启用）：envIntensity float + envMipRange
//        float（= mipLevels−1，0 = 无环境）+ 8B 留白——scene.slang/env.slang 字面量一致
//   @64  Phase 5.5 批次 4 skyboxInvViewProj（mat4 列主序 = inverse(proj·view)，
//        sky.slang skyClipToWorld 消费）
//   @128 对象矩阵表：mat4 列主序 × kSceneMatrixCapacity（[assetId] = spin·world，
//        DrawArgs.objectIndex 索引；世界空间光照 + Phase 7 shadow/culling 复用）
// 内容每帧变 = 全项目首个动态 GPU 数据：CPU 写 staging ring（×MAX_FRAMES_IN_FLIGHT，
// drawFrame ① 已 wait 本 slot fence → 写安全）+ 录制期 vkCmdCopyBuffer 拷进本 buffer
//（命令流内天然有序；描述符静态指向本 buffer 永不改动，见 vkrenderer.cppm）。
export struct LightDataGpu {
	glm::vec3 direction{0.0f, -1.0f, 0.0f};  // @0 世界空间光方向（归一化，照射方向）
	glm::vec3 eyePos{0.0f};                    // @12 世界空间相机位置（高光视向）
	glm::vec3 color{1.0f};                     // @24 光色 × intensity（CPU 预乘）
	glm::vec3 ambient{0.1f};                   // @36 环境光
};
static_assert(sizeof(LightDataGpu) == 48);

// Phase 5.5：env 参数偏移（envIntensity float @48 + envMipRange float @52；
// env.slang readEnvParams 的 Load(48)/Load(52) 字面量必须与本值一致——scene.slang 先例）
export inline constexpr uint32_t kSceneDataEnvOffset = sizeof(LightDataGpu);
// Phase 5.5 批次 4：skyboxInvViewProj mat4 列主序 @64（inverse(proj·view)，
// sky.slang skyClipToWorld 的 Load4(64/80/96/112) 字面量必须与本值一致）
export inline constexpr uint32_t kSceneDataSkyOffset = 64;
export inline constexpr uint32_t kSceneDataHeaderBytes = 128;   // light 48 + env 16 + sky invVP 64
static_assert(kSceneDataEnvOffset == 48 && kSceneDataSkyOffset == 64 &&
	kSceneDataSkyOffset + sizeof(glm::mat4) == kSceneDataHeaderBytes);
// mat4 列主序 stride：派生自 sizeof(glm::mat4) 而非裸字面量（C++ 侧漂移即编译失败；
// shader 端 scene.slang 的字面量 64u 须与本值一致——无反射端对应，见 scene.slang 头注释）
export inline constexpr uint32_t kSceneMatrixStride = sizeof(glm::mat4);
export inline constexpr uint32_t kSceneMatrixCapacity = 4096;   // 对象矩阵表容量（Phase 7：Bistro 1591 primitive 各一实例，1024 不够；scene data 缓冲随之 256KB）

// Phase 7 D3：多灯数组区（追加于对象矩阵表之后——@0-128 头逐字节不动，forward/env/sky
// shader 零改动 diff0）。基址 = 头 + 矩阵表容量；static_assert 钉死与生成常量
// kSceneDataLightArrayBase（lights_schema.lua）三方同步，任一侧漂移编译失败。
export inline constexpr uint32_t kSceneDataLightArrayOffset =
	kSceneDataHeaderBytes + kSceneMatrixCapacity * kSceneMatrixStride;
static_assert(kSceneDataLightArrayOffset == kSceneDataLightArrayBase,
	"scene data light-array offset drifted from lights_schema.lua kSceneDataLightArrayBase");
export inline constexpr uint32_t kSceneDataLightArrayBytes = 16u + kMaxLights * kGpuLightStride;

export inline constexpr vk::DeviceSize kSceneDataBytes =
	kSceneDataHeaderBytes + vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride
	+ kSceneDataLightArrayBytes;

export class Resources {
   public:
	// heap 容量与场景内容解耦（E1.3，批次 3）：固定裕量 kUserSlotMargin，不再按
	// primitive 数精确预分配（原 modelPrimitiveCount 构造参数删除）。用户槽位布局
	//（相对 reserved range 起点，Phase 3 统一 slot 分配器 allocSlot 连续分配）：
	//   [0, 2)    cube（顶点 slot 0 / 索引 slot 1，types_generated）
	//   [2, 2+N)  模型顶点 slot（每模型一个——拼接顶点 buffer，Phase 4 批次 2；
	//   slot = reserved + HeapSlotModelVertexBase + 模型序，见 model_importer.cppm）
	//   [2+N, …)  纹理 slot（texture_pool.cppm uploadModelTextures：2+N = 白 1×1 特例（契约断言），
	//   其余按上传序，资产记录在 AssetTable，见 texture_pool.cppm）
	//   其余为裕量；运行时扩容 = 重建 heap + 重写全部描述符
	Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain);

	const RGL::VmaImage& getDepthImage() const { return depthImage; }
	const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }

	[[nodiscard]] std::expected<void, Error> recreateDepthImage(vk::Extent2D extent) {
		vkDevice.getDevice().waitIdle();

		auto result = createDepthImage(allocator, extent);
		if (!result) return std::unexpected(std::move(result).error());
		depthImage = std::move(*result);
		depthImageView = vk::raii::ImageView(vkDevice.getDevice(), buildDepthView(*depthImage));
		return {};
	}

	const RGL::VmaDescriptorHeap& getDescriptorHeap() const { return descriptorHeap; }
	const RGL::VmaDescriptorHeap& getSamplerHeap() const { return samplerHeap; }
	const RGL::VmaAllocatorRAII& getAllocator() const { return allocator; }
	vk::DeviceSize getReservedRangeOffset() const { return 0; }
	vk::DeviceSize getReservedRangeSize() const { return descriptorHeap.getReservedSlots() * descriptorHeap.getDescriptorSize(); }
	// sampler heap 绑定的保留区（同 resource heap 语义：reserved 槽声明为不可用）
	vk::DeviceSize getSamplerReservedRangeSize() const { return samplerHeap.getReservedSlots() * samplerHeap.getDescriptorSize(); }
	// 共享 sampler（批次 1：owner 从 TexturePool 构造移入 Resources——初始化层对象持有）：
	// 一份 sampler 描述符（linear + CLAMP_TO_EDGE，无 mipmap）写 sampler heap 首个可用槽，
	// 全模型管线映射指向同一槽。幂等（首次调用写入，之后返回既有槽号）；调用点 = 创建
	// textured 管线前（main 启动期）。
	// sampler create info 存成员（稳定存储）：本机 RADV 写时即解析
	//（docs/archive/heap_texture_research.md §12.2，"采样期解引用"旧归因已证伪），
	// 但 spec 允许驱动延迟解释，生命周期须覆盖运行期
	[[nodiscard]] uint32_t ensureSharedSampler();
	// Phase 5.5 sky/env sampler：linear mipmap（mipmapMode=LINEAR + maxLod=mipLevels−1），
	// slot = sampler heap reserved + 1（共享 sampler 之后第二槽，capacity reserved+8 足够）。
	// 幂等（ensureSharedSampler 同模式）。不动共享 sampler——模型纹理路径零影响。
	// maxLod 烘焙进描述符 → 调用点须已知环境贴图 mip 级数（env 上传期）。
	[[nodiscard]] uint32_t ensureSkySampler(uint32_t mipLevels);
	// Phase 5.5 环境 cubemap（场景级单例资产，texture_pool::uploadCubeMap 产出入库）：
	// 与材质纹理集（AssetTable::textures）分离——env 绑定场景而非材质，不占纹理 id 空间
	//（白纹理 id 0 / 首槽契约不受影响）。optional 内部存储地址稳定（Resources = main 栈
	// 对象永不 move），描述符指针链自引用在 emplace 之后设置即稳定。
	[[nodiscard]] TextureAsset& emplaceEnvTexture(TextureAsset asset) {
		envAsset.emplace(std::move(asset));
		return *envAsset;
	}
	[[nodiscard]] const TextureAsset* getEnvTexture() const {
		return envAsset ? &*envAsset : nullptr;
	}
	// SceneData buffer（Phase 5 批次 2）：幂等创建设备端 storage buffer（eTransferDst——
	// 每帧内容由 staging 拷入）+ 描述符写 allocSlot 运行时槽（同 slotTexture 模式，
	// 纹理槽数随场景动态、light 槽无编译期偏移）。调用点 = 建模型管线条目前
	//（ShaderRegistry::addModelEntry，同 ensureSharedSampler 时序）。
	// 返回 resource heap 绝对槽号（binding mapping 烘焙用）。
	[[nodiscard]] uint32_t ensureSceneData();

	// Phase 6 批次 4：render_graph offscreen attachment 的 RT 缓存（key = attachment 名）。
	// 幂等创建 / extent 失配重建（随 swapchain resize，重建前 waitIdle——双帧在飞下
	// 旧 RT 可能仍被另一帧命令引用，同 recreateDepthImage 规则）。
	// usage = COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST | SAMPLED（blit 上屏与
	// shader 采样两条消费路都预留）；format 由调用点解析（"inherit" = swapchain 格式，
	// blit 到后缓冲要求同格式类，LINEAR filter 的 VVL 兼容前提）。
	// map 节点地址稳定（view 的 raii 对象不 move）。
	struct RtAttachment {
		RGL::VmaImage image;
		std::optional<vk::raii::ImageView> view;
		vk::Extent2D extent{};
		vk::Format format = vk::Format::eUndefined;  // v341 raii::ImageView 无 getFormat，自建记录
		// 重建计数：extent/format 漂移换新图（真实布局回 Undefined）——消费方
		//（Renderer 布局追踪）据此重置，旧值失配 = spec 违例
		uint64_t generation = 0;
		// Phase 7 gap ④：供 deferred light pass 采样的 SAMPLED_IMAGE 描述符链。
		// heapSlot 首建 allocSlot 分配、跨 resize 复用（重建只换 image/view、槽不变，
		// 故 light 管线 baked 的槽号永久有效）。描述符指针链（imageDesc.pView→
		// sampleViewCreate、resourceDesc.data.pImage→imageDesc）存本结构体——std::map
		// 节点地址稳定，满足 v341 writeResourceDescriptors 指针生命周期前提（同 TextureAsset）
		uint32_t heapSlot = 0;
		bool hasSampleDescriptor = false;
		vk::ImageViewCreateInfo sampleViewCreate{};
		vk::ImageDescriptorInfoEXT imageDesc{};
		vk::ResourceDescriptorInfoEXT resourceDesc{};
		vk::HostAddressRangeEXT dst{};
	};
	// 返回稳定指针（map 节点；std::expected 不支持引用类型，AGENTS 规范）
	[[nodiscard]] std::expected<RtAttachment*, Error> ensureRtAttachment(const std::string& name,
		vk::Extent2D extent, vk::Format format);
	// G-buffer RT 的 SAMPLED_IMAGE heap 槽号（ensureRtAttachment 首建分配、跨 resize 复用，
	// 管线 baked 永久有效）；尚未 ensure 的 RT 名 → nullopt（调用方须先 ensure 该 RT）
	[[nodiscard]] std::optional<uint32_t> rtHeapSlot(const std::string& name) const {
		auto it = rtAttachments.find(name);
		if (it == rtAttachments.end() || !it->second.hasSampleDescriptor)
			return std::nullopt;
		return it->second.heapSlot;
	}
	// 已存在 RT 的稳定指针（帧内捕获直读 GB image/extent/format；未建 = nullptr——
	// deferred 图每帧 ensure，正常路径必命中）
	RtAttachment* findRtAttachment(const std::string& name) {
		auto it = rtAttachments.find(name);
		return it == rtAttachments.end() ? nullptr : &it->second;
	}
	// scene data buffer（drawFrame 每帧 staging copy 的目标；未创建 = 空句柄，跳过拷贝）
	const RGL::VmaBuffer& getSceneDataBuffer() const { return sceneDataBuffer; }

	// Phase 7 D1：确保 deferred G-buffer 4 个 offscreen RT 并返回其 SAMPLED_IMAGE 堆槽
	//（[0]albedo [1]normal [2]position [3]metalrough）。名字/格式契约与 deferredRenderGraphDesc 一致
	//（drawFrame 图循环对同名同格式 ensureRtAttachment 命中复用、堆槽跨 resize 复用，light 管线 baked 有效）。
	// extent = 当前 swapchain。**必须在模型/纹理/env 槽之后调用**（allocSlot 单调游标，早分配破坏
	// "[2,2+N) 模型顶点槽"布局契约断言——同 env 上传时序契约）。
	[[nodiscard]] std::expected<std::array<uint32_t, 4>, Error> ensureDeferredGBufferSlots();

	// cube 一等资产（批次 3）：资产记录 = cube 顶点/索引 buffer + slot 的唯一 holder
	uint32_t cubeAssetId() const { return cubeAsset; }
	// 资产表（已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用；
	// registry entity 经 MeshRef{asset} 引用，见 AssetTable 注释）
	RGL::AssetTable& getAssetTable() { return assetTable; }
	RGL::VmaAllocatorRAII& getAllocator() { return allocator; }
	// 统一 slot 分配器（Phase 3，纹理资产并案）：从 reservedSlots 起单调分配，
	// name→slot 记日志（slot 表排查用）；调用方以生成的偏移常量校验布局契约（见构造器 / model_importer）。
	// 越界（≥ capacity，含 reserved）→ 错误
	[[nodiscard]] std::expected<uint32_t, Error> allocSlot(const char* name) {
		if (nextSlot >= descriptorHeap.getCapacity())
			return std::unexpected(Error{ErrorCode::DescriptorSlotOutOfBounds,
				fmt::format("descriptor heap capacity exceeded ({} >= {}): {}",
					nextSlot, descriptorHeap.getCapacity(), name)});
		auto* logger = RLLogger::getInstance();
		const uint32_t slot = nextSlot++;
		if (logger) logger->debug("descriptor heap slot {}: {}", slot, name);
		return slot;
	}

   private:
	uint32_t reservedSlots() const { return descriptorHeap.getReservedSlots(); }

   private:
	static std::expected<RGL::VmaImage, Error> createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent);

	static vk::ImageViewCreateInfo buildDepthView(VkImage image);

	std::expected<RGL::VmaBuffer, Error> createGpuBuffer(vk::DeviceSize size, vk::BufferUsageFlags extraUsage, const void* initData = nullptr);

	// 容量 = reserved + 用户槽位。用户槽位布局（相对 reserved range 起点，allocSlot 连续分配）：
	//   cube 顶点 slot 0、索引 slot 1（types_generated 的 HeapSlotVertexOffset/HeapSlotIndexOffset）
	//   模型顶点 slot 基址 HeapSlotModelVertexBase=2（model_types_generated），每模型一个（拼接顶点 buffer，Phase 4 批次 2）；
	//   纹理 slot 自 2+N 起（texture_pool.cppm，资产记录在 AssetTable）；
	//   剩余裕量 kUserSlotMargin（固定，与场景内容解耦——E1.3）
	std::expected<RGL::VmaDescriptorHeap, Error> createDescriptorHeap();

	// sampler heap（Phase 3）：OpTypeSampler 的独立 heap（vkCmdBindSamplerHeapEXT 每帧绑定）。
	// 背景：pEmbeddedSampler 对 Slang 的 OpTypeImage+OpTypeSampler 分离对未获 RADV 采纳
	//（spec 声称同等适用，实测采样恒 0；spike 只验证了 GLSL 组合 sampler2D=OpTypeSampledImage），
	// 回退链（docs/archive/phase3_plan.md R1）= 真 sampler heap。容量 = reserved + 固定小裕量
	//（共享一份 sampler 描述符，全模型管线映射指向同一槽）。
	std::expected<RGL::VmaDescriptorHeap, Error> createSamplerHeap();

	void writeDescriptors();

	RGL::AppContext& vkInstance;
	RGL::DeviceContext& vkDevice;
	RGL::Swapchain& vkSwapchain;
	RGL::VmaAllocatorRAII allocator;
	RGL::VmaImage depthImage;
	vk::raii::ImageView depthImageView;
	// cube 顶点/索引 buffer（批次 3：构造体移入 cube 资产记录后为空，访问经 cubeAssetId）
	RGL::VmaBuffer vertexBuffer;
	RGL::VmaBuffer indexBuffer;
	RGL::VmaDescriptorHeap descriptorHeap;
	RGL::VmaDescriptorHeap samplerHeap;  // Phase 3：OpTypeSampler 独立 heap
	// 共享 sampler 的 create info 稳定存储（ensureSharedSampler 写入；
	// 本机 RADV 写时即解析、spec 允许驱动延迟解释，生命周期须覆盖运行期，见其注释）
	vk::SamplerCreateInfo samplerCreateInfo{};
	uint32_t sharedSamplerSlot = 0;   // 共享 sampler 在 sampler heap 的槽号（ensureSharedSampler 启动期写入）
	bool sharedSamplerReady = false;
	// Phase 5.5：sky/env sampler（第二槽，ensureSkySampler 幂等写入）+ 环境 cubemap 单例
	vk::SamplerCreateInfo skySamplerCreateInfo{};  // create info 稳定存储（同 samplerCreateInfo 规则）
	uint32_t skySamplerSlot = 0;
	bool skySamplerReady = false;
	std::optional<TextureAsset> envAsset;
	uint32_t heapSlotVertex;
	uint32_t heapSlotIndex;
	// SceneData（Phase 5 批次 2）：设备端动态数据 buffer + 运行时槽（ensureSceneData 幂等写入）
	RGL::VmaBuffer sceneDataBuffer;
	uint32_t sceneDataSlot = 0;
	bool sceneDataReady = false;
	// 统一 slot 分配器（Phase 3，纹理资产并案）：单调分配游标，从 reservedSlots 起
	uint32_t nextSlot;
	// cube 一等资产 id（批次 3：构造体写入）
	uint32_t cubeAsset = 0;
	// 资产表（E1.1/E1.2：已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用）
	RGL::AssetTable assetTable;
	// Phase 6 批次 4：offscreen attachment RT 缓存（ensureRtAttachment，key = 名）
	std::map<std::string, RtAttachment> rtAttachments;
	// 用户槽位固定裕量（与场景内容解耦；Phase 3 统一步长 = max(buffer, image) descriptorSize，
	// RADV 32B，1024 槽为 KB 级开销）
	static constexpr uint32_t kUserSlotMargin = 1024;
};

}  // namespace RGL
