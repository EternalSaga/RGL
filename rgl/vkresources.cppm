module;
#include <vk_mem_alloc.h>
#include <glm/glm.hpp>
#include <fmt/format.h>
export module vk_resources;

import rlstd;
import rlvk;
import rllogger;
import lights_types_generated;  // Phase 7 D3：GpuLight struct + kMaxLights/kGpuLightStride/kSceneDataLightArrayBase
import scene_forward_types_generated;  // 前向头 SceneLight（C API 反射生成，替换原手写 LightDataGpu，双源消除）
import VmaRaii;
import vk_instance;
import vk_device;
import vk_swapchain;
import mip_gen;
import env_prefilter;  // Phase 8 ④b：GGX 预滤波环境 cubemap（heap + shader object，mip_gen 变体）
import vkerror;

// 资产层（AssetKind/MeshAsset/TextureAsset/MaterialAsset/AssetTable）拆出为独立模块——
// 本模块 export import 保持 Resources 接口（getAssetTable 等）类型对外可见
// （2026-09-02 拆分）
export import asset_table;

namespace RGL {

/**
 * SceneData：场景级每帧动态数据的 bindless 契约（Phase 5 批次 2）。单一 storage buffer
 *（heap slot 经 allocSlot 运行时分配——纹理槽数随场景动态，light 槽无法钉编译期偏移，
 * 同 slotTexture 模式），字节布局的单一真相源（shader 端 scene.slang 字面量必须一致，
 * kTrianglesPerGroup 先例——无反射端对应）：
 *   @0   SceneLight（48B：direction/eyePos/color/ambient 各 vec3，C API 反射生成，
 *        见 scene_forward_types_generated / docs/slang_api_reflection_research.md）
 *   @48  Phase 5.5 env 参数（原 16B 保留区启用）：envIntensity float + envMipRange
 *        float（= mipLevels−1，0 = 无环境）+ 8B 留白——scene.slang/env.slang 字面量一致
 *   @64  Phase 5.5 批次 4 skyboxInvViewProj（mat4 列主序 = inverse(proj·view)，
 *        sky.slang skyClipToWorld 消费）
 *   @128 对象矩阵表：mat4 列主序 × kSceneMatrixCapacity（[objectSlot] = spin·world，
 *        实例级 Phase 6 instancing；DrawArgs.objectIndex = MeshRef.objectSlot 索引；
 *        世界空间光照 + Phase 7 shadow/culling 复用）
 * 内容每帧变 = 全项目首个动态 GPU 数据：CPU 写 staging ring（×MAX_FRAMES_IN_FLIGHT，
 * drawFrame ① 已 wait 本 slot fence → 写安全）+ 录制期 vkCmdCopyBuffer 拷进本 buffer
 *（命令流内天然有序；描述符静态指向本 buffer 永不改动，见 vkrenderer.cppm）。
 */
// 前向头 SceneLight（scene data @0-48）= C API 反射生成（scene_forward_types_generated，
// 布局真相源 = scene.slang SceneLight，逐字段 offsetof static_assert 钉死）。原手写
// LightDataGpu 已删（双源消除）——默认成员初始化为死代码（renderListSystem 每帧显式
// 聚合初始化 .light，从不用默认值），故无默认不影响行为。
// 注意：SceneLight 无默认成员初始化；消费方须显式赋值（render_list 已保证）。

/** Phase 5.5：env 参数偏移（envIntensity float @48 + envMipRange float @52；env.slang
 * readEnvParams 的 Load(48)/Load(52) 字面量必须与本值一致——scene.slang 先例）。派生自
 * sizeof(SceneLight)（C API 反射生成），shader 布局漂移即编译失败。 */
export inline constexpr uint32_t kSceneDataEnvOffset = sizeof(SceneLight);
/** Phase 5.5 批次 4：skyboxInvViewProj mat4 列主序 @64（inverse(proj·view)，sky.slang
 * skyClipToWorld 的 Load4(64/80/96/112) 字面量必须与本值一致）。 */
export inline constexpr uint32_t kSceneDataSkyOffset = 64;
export inline constexpr uint32_t kSceneDataHeaderBytes = 128;   // light 48 + env 16 + sky invVP 64
static_assert(kSceneDataEnvOffset == 48 && kSceneDataSkyOffset == 64 &&
	kSceneDataSkyOffset + sizeof(glm::mat4) == kSceneDataHeaderBytes);
/** mat4 列主序 stride：派生自 sizeof(glm::mat4) 而非裸字面量（C++ 侧漂移即编译失败；
 * shader 端 scene.slang 的字面量 64u 须与本值一致——无反射端对应，见 scene.slang 头注释）。 */
export inline constexpr uint32_t kSceneMatrixStride = sizeof(glm::mat4);
export inline constexpr uint32_t kSceneMatrixCapacity = 4096;   // 对象矩阵表容量（Phase 7：Bistro 1591 primitive 各一实例，1024 不够；scene data 缓冲随之 256KB）

/** Phase 7 D3：多灯数组区（追加于对象矩阵表之后——@0-128 头逐字节不动，forward/env/sky
 * shader 零改动 diff0）。基址 = 头(128) + 矩阵表容量×stride（= 128 + 4096×64 = 262272，
 * lights.slang kSceneDataLightArrayBase 同值）；static_assert 钉死与生成常量
 * kSceneDataLightArrayBase（lights_schema.lua）三方同步，任一侧漂移编译失败。 */
export inline constexpr uint32_t kSceneDataLightArrayOffset =
	kSceneDataHeaderBytes + kSceneMatrixCapacity * kSceneMatrixStride;
static_assert(kSceneDataLightArrayOffset == kSceneDataLightArrayBase,
	"scene data light-array offset drifted from lights_schema.lua kSceneDataLightArrayBase");
/** 灯数组区字节数 = 16B lightCount 头 + kMaxLights × kGpuLightStride（= 16 + 8×64 = 528；
 * 区布局见 lights.slang readLightCount/readGpuLight）。 */
export inline constexpr uint32_t kSceneDataLightArrayBytes = 16u + kMaxLights * kGpuLightStride;

/** Phase 7b+（NV DGC 规避）：per-object MVP 表区（追加于灯数组之后——既有各区基址
 * 逐字节不动 diff0）。CPU 每帧写（值 = 原 DrawArgs.mvp，位级一致），shader
 * scene.slang readObjectMvp 按 objectIndex 读——DrawArgs 去 mvp 字段后 push 36B，
 * 绕 NV DGC PUSH token >83B 多序列挂死（见 docs/调试历史）。
 * 基址 = 灯数组尾（262272 + 528 = 262800，scene.slang 字面量同值）。 */
export inline constexpr uint32_t kSceneDataMvpTableOffset =
	kSceneDataLightArrayOffset + kSceneDataLightArrayBytes;
static_assert(kSceneDataMvpTableOffset % 16 == 0, "MVP table must stay 16B aligned");

/** Phase 7b+（NV DGC 规避，续 MVP 表）：per-object 材质/几何表区（DrawArgs 去 mvp 后
 * 仍剩 baseColor/roughnessMetallic/totalTriangles/vertexOffset 是 per-object 数据，一并
 * 进 heap，DrawArgs 最终只留 objectIndex（4B push）——彻底绕开 NV DGC push 挂死）。
 * 基址 = MVP 表尾（262800 + 4096×64 = 524944，scene.slang 字面量同值）。stride 48
 *（ObjectDrawGpu，见 render_list.cppm；Phase 8 ③ 加 emissive 后 32→48）。 */
export inline constexpr uint32_t kSceneDataDrawTableOffset =
	kSceneDataMvpTableOffset + kSceneMatrixCapacity * kSceneMatrixStride;
export inline constexpr uint32_t kSceneDrawStride = 48;
static_assert(kSceneDataDrawTableOffset % 16 == 0, "draw table must stay 16B aligned");

/** Phase 8 批次 ④a：漫反射辐照度 SH9 区（追加于材质表之后——既有各区基址逐字节不动 diff0）。
 * 9 × vec4 = 144B（.xyz=系数、.w 留白，满足 ByteAddressBuffer.Load4 16B 对齐前提；CPU env
 * 加载期从等距柱状源余弦加权投影 radiance，shader scene.slang irradiance(N) 做辐照度卷积）。
 * 基址 = 材质表尾（524944 + 4096×48 = 721552，scene.slang 字面量同值；Phase 8 ③ stride
 * 32→48 后本区随之后移）。 */
export inline constexpr uint32_t kSceneDataIrradianceOffset =
	kSceneDataDrawTableOffset + kSceneMatrixCapacity * kSceneDrawStride;
export inline constexpr uint32_t kSceneDataIrradianceBytes = 144;  // 9×vec4=144（16B 对齐）
static_assert(kSceneDataIrradianceOffset % 16 == 0, "irradiance region must stay 16B aligned");

/** scene data buffer 总字节数 = 头 128 + 对象矩阵表 + 灯数组区 + MVP 表 + 材质表 +
 * 辐照度 SH9 区（= 721696B，编译期派生）；ensureSceneData 的分配尺寸与 drawFrame 每帧
 * staging 拷贝区尺寸均为本值（vkrenderer.cppm ⑥.5）。 */
export inline constexpr vk::DeviceSize kSceneDataBytes =
	kSceneDataHeaderBytes + vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride
	+ kSceneDataLightArrayBytes
	+ vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride
	+ vk::DeviceSize{kSceneMatrixCapacity} * kSceneDrawStride
	+ kSceneDataIrradianceBytes;

/**
 * GPU 资源层（main.cpp 初始化层构造顺序第 4 站：AppContext→DeviceContext→Swapchain→
 * Resources→ShaderRegistry→Renderer，见 main.cpp）：持有 VmaAllocatorRAII + 深度图/
 * view + 两个 descriptor heap（resource / sampler，VK_EXT_descriptor_heap）+ 共享/
 * sky sampler + env cubemap 单例 + SceneData 动态 buffer + offscreen RT 缓存 +
 * AssetTable。**内核不认识任何内容**（架构债务 1+2 重构）：构造期不创建任何内容
 * 资产——几何（内建 cube / gltf 模型）、纹理、scene data、RT 采样槽全部由内容层
 *（builtin_geometry / model_importer / texture_pool / ShaderRegistry）按上传时序经
 * allocSlot 领取。
 * **成员生命周期规则**（AGENTS.md"重要成员生命周期规则"）：raii/GPU 对象
 *（depthImage / depthImageView / descriptorHeap / samplerHeap）必须经成员初始化
 * 列表初始化，绝不在函数体内初始化；成员声明顺序 = 初始化依赖顺序（三个引用 →
 * allocator → depthImage → depthImageView → descriptorHeap / samplerHeap →
 * nextSlot 游标）。长生命周期 GPU 资源不得声明在 main() 局部块内——块尾析构 =
 * 第 0 帧起 GPU 采样已释放 VA（UAF 教训，见 docs/archive/heap_texture_research.md §12）。
 */
export class Resources {
   public:
	/**
	 * 构建资源层（全部逻辑在成员初始化列表，按成员声明序）：vkInstance → vkDevice →
	 * vkSwapchain → allocator（VMA，BDA flag）→ depthImage（createDepthImage，extent =
	 * swapchain）→ depthImageView → descriptorHeap（createDescriptorHeap）→
	 * samplerHeap（createSamplerHeap）→ mipGen（Phase 8 ③b compute mip 链管线）→
	 * nextSlot = reservedSlots（allocSlot 游标起点）。
	 * heap 容量与场景内容解耦（E1.3，批次 3）：固定裕量 kUserSlotMargin，不按内容
	 * 精确预分配。用户槽位布局（相对 reserved range 起点，Phase 3 统一 slot 分配器
	 * allocSlot 连续分配，顺序 = 上传顺序，几何全部先于纹理）：
	 *   [0, G)     几何槽（内建几何顶点/索引 + 模型拼接顶点块，每块一个；
	 *                基址与计数契约见 AssetTable::geometrySlotCount / uploadModelMeshes）
	 *   [G, …)     纹理槽（texture_pool.cppm uploadModelTextures：首槽 = 白 1×1 特例（契约断言），
	 *                其余按上传序；scene data / RT 采样槽等在后续上传点续接）
	 *   其余为裕量；运行时扩容 = 重建 heap + 重写全部描述符
	 * @param vkInstance 实例层（allocator 创建依赖其 raii Instance）
	 * @param vkDevice 设备层（heap 属性查询 / 描述符写入 / 深度图创建）
	 * @param vkSwapchain 交换链层（深度图 extent）
	 * @note createDescriptorHeap / createSamplerHeap 失败经 .value() 抛出（启动期一次性路径）
	 */
	Resources(RGL::AppContext& vkInstance, RGL::DeviceContext& vkDevice, RGL::Swapchain& vkSwapchain);

	/** @return 深度图 const 引用（VmaImage，eD32Sfloat，extent = 交换链 extent）；
	 * Renderer 作 depth 附件渲染目标。 */
	const RGL::VmaImage& getDepthImage() const { return depthImage; }
	/** @return 深度图 view const 引用（raii ImageView，eDepth aspect）；Renderer 绑定
	 * depth 附件用。 */
	const vk::raii::ImageView& getDepthImageView() const { return depthImageView; }

	/**
	 * 重建深度图 + view（交换链 extent 变化后旧深度图不可复用）：先 device waitIdle
	 *（双帧在飞下旧深度图可能仍被另一帧命令引用），再按新 extent 重建 depthImage 并
	 * 同步重建 depthImageView。
	 * @param extent 新的交换链 extent
	 * @return expected<void, Error>；Error = VMA 图像创建失败（createDepthImage 透传）
	 * @note 调用方：resizeSystem（ecs_impl，窗口 resize 且 swapchain.recreate 返回
	 *   true 后）/ Renderer::recreateSwapchainAndDepth（acquire/present OUT_OF_DATE）
	 */
	[[nodiscard]] std::expected<void, Error> recreateDepthImage(vk::Extent2D extent) {
		vkDevice.getDevice().waitIdle();

		auto result = createDepthImage(allocator, extent);
		if (!result) return std::unexpected(std::move(result).error());
		depthImage = std::move(*result);
		depthImageView = vk::raii::ImageView(vkDevice.getDevice(), buildDepthView(*depthImage));
		return {};
	}

	/** @return resource heap const 引用（VmaDescriptorHeap：BDA buffer 视为连续描述符槽
	 * 数组，步长 = max(buffer, image) descriptorSize 运行时查询）；Renderer 每帧绑定，
	 * 内容层经 allocSlot + getSlotCpuAddress 写描述符。 */
	const RGL::VmaDescriptorHeap& getDescriptorHeap() const { return descriptorHeap; }
	/** @return sampler heap const 引用（OpTypeSampler 独立 heap，Renderer 每帧经
	 * vkCmdBindSamplerHeapEXT 绑定）；共享 / sky sampler 槽位在其中。 */
	const RGL::VmaDescriptorHeap& getSamplerHeap() const { return samplerHeap; }
	/** @return VMA 分配器 const 引用（const 重载，与下方非 const 重载同一对象）；
	 * 创建 GPU 资源走非 const 重载。 */
	const RGL::VmaAllocatorRAII& getAllocator() const { return allocator; }
	/** @return resource heap 保留区偏移，恒 0：reserved 槽始终在 heap 头部
	 * [0, reservedSlots)（VmaDescriptorHeap 布局，docs/DESCRIPTOR_HEAP_SPEC.md §九.2）。 */
	vk::DeviceSize getReservedRangeOffset() const { return 0; }
	/** @return resource heap 保留区字节数 = reservedSlots × descriptorSize（≥ 设备
	 * minResourceHeapReservedRange；vkCmdBindResourceHeapEXT 的 reservedRange 声明为
	 * 不可用，如 NVIDIA 96768B）。 */
	vk::DeviceSize getReservedRangeSize() const { return descriptorHeap.getReservedSlots() * descriptorHeap.getDescriptorSize(); }
	/** @return sampler heap 绑定的保留区字节数 = reservedSlots × samplerDescriptorSize
	 *（同 resource heap 语义：reserved 槽声明为不可用，vkCmdBindSamplerHeapEXT 用）。 */
	vk::DeviceSize getSamplerReservedRangeSize() const { return samplerHeap.getReservedSlots() * samplerHeap.getDescriptorSize(); }
	/**
	 * 共享 sampler（批次 1：owner 从 TexturePool 构造移入 Resources——初始化层对象持有）：
	 * 一份 sampler 描述符（linear + mipmap LINEAR + CLAMP_TO_EDGE，maxLod=20）写
	 * sampler heap 首个可用槽，全模型管线映射指向同一槽。Phase 8 ③b 起 2D 纹理带
	 * compute mip 链（mip_gen）——采样自动用 mip（maxLod 20 覆盖 ≤2^20 纹理链长，
	 * Vulkan 按各图实际 mip 范围钳制，单 sampler 通吃不同尺寸）。幂等（首次调用写入，
	 * 之后返回既有槽号）；调用点 = 创建 textured 管线前（main 启动期）。
	 * sampler create info 存成员（稳定存储）：本机 RADV 写时即解析
	 *（docs/archive/heap_texture_research.md §12.2，"采样期解引用"旧归因已证伪），
	 * 但 spec 允许驱动延迟解释，生命周期须覆盖运行期。
	 * @return 共享 sampler 在 sampler heap 的槽号（首个可用槽 = reservedSlots）
	 */
	[[nodiscard]] uint32_t ensureSharedSampler();
	/**
	 * Phase 5.5 sky/env sampler：linear mipmap（mipmapMode=LINEAR + maxLod=mipLevels−1），
	 * slot = sampler heap reserved + 1（共享 sampler 之后第二槽，capacity reserved+8 足够）。
	 * 幂等（ensureSharedSampler 同模式）。不动共享 sampler——模型纹理路径零影响。
	 * maxLod 烘焙进描述符 → 调用点须已知环境贴图 mip 级数（env 上传期）。
	 * @param mipLevels 环境贴图 mip 级数（maxLod = mipLevels−1）
	 * @return sky sampler 在 sampler heap 的槽号（reservedSlots + 1）
	 */
	[[nodiscard]] uint32_t ensureSkySampler(uint32_t mipLevels);
	/**
	 * Phase 5.5 环境 cubemap（场景级单例资产，texture_pool::uploadCubeMap 产出入库）：
	 * 与材质纹理集（AssetTable::textures）分离——env 绑定场景而非材质，不占纹理 id 空间
	 *（白纹理 id 0 / 首槽契约不受影响）。
	 * @param asset env 纹理资产（移动入；其 heapSlot / mipLevels 供 addModelEntry /
	 *   addSkyEntry 烘焙 binding 4/5）
	 * @return 入库的 env 资产引用（optional 内部存储地址稳定——Resources = main 栈
	 *   对象永不 move，描述符指针链自引用在 emplace 之后设置即稳定）
	 */
	[[nodiscard]] TextureAsset& emplaceEnvTexture(TextureAsset asset) {
		envAsset.emplace(std::move(asset));
		return *envAsset;
	}
	/** @return env cubemap 资产指针（nullptr = 未上传环境）；addModelEntry /
	 * addSkyEntry 据此烘焙 binding 4/5（env 槽全局单例，不进组合去重 key）。 */
	[[nodiscard]] const TextureAsset* getEnvTexture() const {
		return envAsset ? &*envAsset : nullptr;
	}
	/** Phase 8 批次 ④a：环境漫反射辐照度 radiance SH9（9 × vec4，.xyz=系数、.w 留白；CPU
	 * env 加载期投影，renderListSystem 每帧拷进 SceneFrameData.irradianceSH → scene data 尾区
	 * → scene.slang irradiance(N) 做辐照度卷积消费）。 */
	void setEnvIrradianceSH(const std::array<glm::vec4, 9>& sh) { envIrradianceSH = sh; }
	/** @return 环境漫反射辐照度 radiance SH9（未设 = 全 0 = 无环境漫反射）。 */
	[[nodiscard]] const std::array<glm::vec4, 9>& getEnvIrradianceSH() const { return envIrradianceSH; }
	/**
	 * SceneData buffer（Phase 5 批次 2）：幂等创建设备端 storage buffer（eTransferDst——
	 * 每帧内容由 staging 拷入）+ 描述符写 allocSlot 运行时槽（同 slotTexture 模式，
	 * 纹理槽数随场景动态、light 槽无编译期偏移）。调用点 = 建模型管线条目前
	 *（ShaderRegistry::addModelEntry，同 ensureSharedSampler 时序）。
	 * @return resource heap 绝对槽号（binding mapping 烘焙用，shader 端 binding 3，
	 *   见 scene.slang）
	 */
	[[nodiscard]] uint32_t ensureSceneData();

	/**
	 * Phase 6 批次 4：render_graph offscreen attachment 的 RT 缓存条目（key = attachment 名）。
	 * 幂等创建 / extent 失配重建（随 swapchain resize，重建前 waitIdle——双帧在飞下
	 * 旧 RT 可能仍被另一帧命令引用，同 recreateDepthImage 规则）。
	 * usage = COLOR_ATTACHMENT | TRANSFER_SRC | TRANSFER_DST | SAMPLED（blit 上屏与
	 * shader 采样两条消费路都预留）；format 由调用点解析（"inherit" = swapchain 格式，
	 * blit 到后缓冲要求同格式类，LINEAR filter 的 VVL 兼容前提）。
	 * 存于 std::map，节点地址稳定（view 的 raii 对象与描述符指针链不 move）。
	 */
	struct RtAttachment {
		RGL::VmaImage image;  // offscreen RT 图（extent/format 漂移时重建）
		std::optional<vk::raii::ImageView> view;  // RT 颜色 view（随 image 同步重建）
		vk::Extent2D extent{};  // 图 extent（重建命中判断条件之一）
		vk::Format format = vk::Format::eUndefined;  // v341 raii::ImageView 无 getFormat，自建记录
		// 重建计数：extent/format 漂移换新图（真实布局回 Undefined）——消费方
		//（Renderer 布局追踪）据此重置，旧值失配 = spec 违例
		uint64_t generation = 0;
		// Phase 7 gap ④：供 deferred light pass 采样的 SAMPLED_IMAGE 描述符链。
		// heapSlot 首建 allocSlot 分配、跨 resize 复用（重建只换 image/view、槽不变，
		// 故 light 管线 baked 的槽号永久有效）。描述符指针链（imageDesc.pView→
		// sampleViewCreate、resourceDesc.data.pImage→imageDesc）存本结构体——std::map
		// 节点地址稳定，满足 v341 writeResourceDescriptors 指针生命周期前提（同 TextureAsset）
		uint32_t heapSlot = 0;  // SAMPLED_IMAGE 槽号（首建 allocSlot，跨 resize 复用）
		bool hasSampleDescriptor = false;  // 采样描述符已建标记（首建后恒 true）
		vk::ImageViewCreateInfo sampleViewCreate{};  // imageDesc.pView 的稳定指向目标
		vk::ImageDescriptorInfoEXT imageDesc{};  // data.pImage 链上游（v341：pView 指 create info）
		vk::ResourceDescriptorInfoEXT resourceDesc{};  // eSampledImage 描述符定义
		vk::HostAddressRangeEXT dst{};  // 描述符写入目标槽范围（getSlotCpuAddress 解析）
	};
	/**
	 * 幂等创建（或 extent/format 失配时重建）一个 offscreen RT，返回 map 节点稳定指针
	 *（std::expected 不支持引用类型，AGENTS 规范）。行为契约见 RtAttachment 注释。
	 * @param name attachment 名（rtAttachments 的 key；同名同 extent 同格式命中复用——
	 *   drawFrame 图循环按图声明每帧调用）
	 * @param extent RT extent（= swapchain extent）
	 * @param format RT 格式（调用点解析，"inherit" = swapchain 格式）
	 * @return 稳定指针（extent/format 漂移重建：waitIdle 后换 image/view、generation+1、
	 *   采样槽号不变；Error = VMA 图像创建 / allocSlot / 槽地址查询失败）
	 */
	[[nodiscard]] std::expected<RtAttachment*, Error> ensureRtAttachment(const std::string& name,
		vk::Extent2D extent, vk::Format format);
	/** @return RT 的 SAMPLED_IMAGE heap 槽号（ensureRtAttachment 首建分配、跨 resize
	 * 复用，管线 baked 永久有效）；尚未 ensure 的 RT 名 → nullopt（调用方须先 ensure
	 * 该 RT） */
	[[nodiscard]] std::optional<uint32_t> rtHeapSlot(const std::string& name) const {
		auto it = rtAttachments.find(name);
		if (it == rtAttachments.end() || !it->second.hasSampleDescriptor)
			return std::nullopt;
		return it->second.heapSlot;
	}
	/** @return 已存在 RT 的稳定指针（帧内捕获直读 GB image/extent/format；未建 =
	 * nullptr——deferred 图每帧 ensure，正常路径必命中） */
	RtAttachment* findRtAttachment(const std::string& name) {
		auto it = rtAttachments.find(name);
		return it == rtAttachments.end() ? nullptr : &it->second;
	}
	/** @return SceneData 设备端 buffer const 引用（drawFrame 每帧 staging copy 的
	 * 目标；未创建 = 空句柄，drawFrame 跳过拷贝——无模型场景 ensureSceneData 未被调用） */
	const RGL::VmaBuffer& getSceneDataBuffer() const { return sceneDataBuffer; }

	/**
	 * Phase 7 D1：确保 deferred G-buffer 4 个 offscreen RT 并返回其 SAMPLED_IMAGE 堆槽
	 *（[0]albedo [1]normal [2]position [3]metalrough）。名字/格式契约与
	 * deferredRenderGraphDesc 一致（共享 render_graph kGBufferAttachments 单一真相源）；
	 * drawFrame 图循环对同名同格式 ensureRtAttachment 命中复用、堆槽跨 resize 复用，
	 * light 管线 baked 有效。extent = 当前 swapchain。**必须在几何/纹理/env 槽之后调用**
	 *（allocSlot 单调游标，早分配破坏"纹理首槽 = 白 = 全部几何槽之后"契约断言——同
	 * env 上传时序契约）。
	 * @return 4 个 heap 槽（kGBufferAttachments 序）；Error = RT 创建 / 槽分配失败
	 */
	[[nodiscard]] std::expected<std::array<uint32_t, 4>, Error> ensureDeferredGBufferSlots();

	/**
	 * Phase 8 批次 ①：确保 HDR 累加链 scene_hdr RT（名字/格式 = render_graph
	 * kSceneHdrName/kSceneHdrFormat 单一真相源，rgba16_sfloat offscreen）并返回其
	 * SAMPLED_IMAGE 堆槽（addTonemapEntry 烘焙 binding 4；tonemap pass 采样）。
	 * 调用时序同 ensureDeferredGBufferSlots——几何/纹理/env（deferred 还有 G-buffer）
	 * 槽之后、Renderer 之前；drawFrame 图循环对同名同格式命中复用、堆槽跨 resize 不变。
	 * @return scene_hdr 的 SAMPLED_IMAGE heap 槽；Error = RT 创建 / 槽分配失败
	 */
	[[nodiscard]] std::expected<uint32_t, Error> ensureSceneHdrSlot();

	/** @return 资产表引用（已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线
	 * 条目引用；registry entity 经 MeshRef{asset} 引用，见 AssetTable 注释） */
	RGL::AssetTable& getAssetTable() { return assetTable; }
	/** @return VMA 分配器引用（非 const 重载，与上方 const 重载同一对象）；内容层上传
	 * 路径经它创建 buffer/image（builtin_geometry / model_importer / texture_upload /
	 * gpu_canary / Renderer staging）。 */
	RGL::VmaAllocatorRAII& getAllocator() { return allocator; }
	/** @return mip 链生成器（Phase 8 ③b；启动期一次性基础设施，构造于本对象构造期）。
	 * uploadTexture 2D 路径经它把 mip 链（level ≥1）录进该纹理的上传 CB：UNORM =
	 * compute 链（descriptor heap + shader object，须先经 ensureMipGenReady 建齐 level
	 * shader/槽）/ sRGB = LINEAR blit 链（见 mip_gen 模块头注释：RADV SRGB 无
	 * STORAGE_IMAGE 能力）。 */
	RGL::MipGen& getMipGen() { return mipGen; }
	/**
	 * 备齐 mip-gen 的 heap 基础设施（首张 UNORM compute 链纹理上传前调用，幂等增量）。
	 * 职责：首次注入 heap 引用 + 复用 ensureSharedSampler 的 LINEAR sampler（init）；
	 * 若已建 level shader 数 < levels，逐个 allocSlot 领取 src/dst 描述符槽 + MipGen
	 * 建对应 level 的 shader object（每 level 一 shader object + 一对独立固定槽，跨纹理
	 * fence 分隔复用）。
	 * **时序契约**：必须在"全部几何槽分配完 + 白纹理首槽契约断言通过"之后调用（首张
	 * UNORM 多 mip 纹理 = A2 阶段，天然满足——此处领槽会推进 allocSlot 游标，早于几何/白
	 * 将破坏 expectedGeometrySlot 断言）。
	 * @param levels 需要覆盖的最大 mip level 数（= 当纹理 mipLevels；只增不减）
	 * @return void；Error = allocSlot 越界 或 shader object 创建失败
	 */
	[[nodiscard]] std::expected<void, Error> ensureMipGenReady(uint32_t levels);
	/** @return GGX 预滤波生成器（Phase 8 ④b；启动期一次性基础设施，构造于本对象构造期）。
	 * cubemap 16F 上传路径经它把 env mip 链（level ≥1）录进该 cube 的上传 CB：GGX
	 * importance-sample 预滤波（descriptor heap + shader object，须先经 ensureEnvPrefilterReady
	 * 建齐 level shader/槽，见 env_prefilter 模块头注释）。 */
	RGL::EnvPrefilter& getEnvPrefilter() { return envPrefilter; }
	/**
	 * 备齐 GGX 预滤波的 heap 基础设施（env 16F cube 上传前调用，幂等增量）。职责：首次注入
	 * heap 引用 + 复用 ensureSharedSampler 的 LINEAR sampler（init）；若已建 level shader 数 <
	 * levels，逐个 allocSlot 领取 src/dst 描述符槽 + EnvPrefilter 建对应 level 的 shader object
	 * （每 level 一 shader object + 一对独立固定槽）。env 单例启动期只上传一次，实际只补一轮。
	 * **时序契约**：同 ensureMipGenReady——必须在"全部几何槽分配完 + 白纹理首槽契约断言通过"
	 * 之后调用（env 上传 = A2 之后阶段，天然满足；此处领槽会推进 allocSlot 游标）。
	 * @param levels env 总 mip 级数（= cube mipLevels；只增不减）
	 * @return void；Error = allocSlot 越界 或 shader object 创建失败
	 */
	[[nodiscard]] std::expected<void, Error> ensureEnvPrefilterReady(uint32_t levels);
	/**
	 * 统一 slot 分配器（Phase 3，纹理资产并案）：从 reservedSlots 起单调分配，
	 * name→slot 记日志（slot 表排查用）；调用方以生成的偏移常量校验布局契约（见构造器
	 * / model_importer）。
	 * @param name 槽用途名（仅日志，如 "scene.data" / RT 名）
	 * @return 槽号；越界（≥ capacity，含 reserved）→ DescriptorSlotOutOfBounds 错误
	 */
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
	/** @return 驱动保留槽位数（用户槽起点，allocSlot 初始游标；NVIDIA 3024 /
	 * RADV·Intel 0，见 createDescriptorHeap）。 */
	uint32_t reservedSlots() const { return descriptorHeap.getReservedSlots(); }

   private:
	/**
	 * 创建深度图（eD32Sfloat，单采样，eDepthStencilAttachment，VMA 专用内存）。
	 * @param allocator VMA 分配器
	 * @param extent 图 extent（= swapchain extent）
	 * @return VmaImage；VMA 失败 = VMASuccessFailed
	 */
	static std::expected<RGL::VmaImage, Error> createDepthImage(RGL::VmaAllocatorRAII& allocator, vk::Extent2D extent);

	/**
	 * 组装深度图 view create info（e2D / eD32Sfloat / eDepth aspect / levelCount =
	 * layerCount = 1）。
	 * @param image 深度图句柄
	 * @return 填好的 ImageViewCreateInfo（局部临时，仅供 raii ImageView 构造消费）
	 */
	static vk::ImageViewCreateInfo buildDepthView(VkImage image);

	/**
	 * 创建 resource descriptor heap。容量 = reserved + 用户槽位（kUserSlotMargin 固定
	 * 裕量）。用户槽位 = allocSlot 单调分配的连续段，按上传时序分区：几何槽（内建几何
	 * + 模型顶点块，AssetTable::geometrySlotCount 计数）→ 纹理槽（首槽 = 白，
	 * texture_pool.cppm）→ scene data / RT 采样槽等运行时槽。内核不预留任何内容专属
	 * 槽位（架构债务 1：cube 归还 builtin_geometry）。
	 * @return VmaDescriptorHeap（步长 = max(buffer, image) descriptorSize，运行时查询，
	 *   见 impl）；底层 buffer 创建失败 = VMASuccessFailed
	 */
	std::expected<RGL::VmaDescriptorHeap, Error> createDescriptorHeap();

	/**
	 * 创建 sampler heap（Phase 3）：OpTypeSampler 的独立 heap（vkCmdBindSamplerHeapEXT
	 * 每帧绑定）。背景：pEmbeddedSampler 对 Slang 的 OpTypeImage+OpTypeSampler 分离对
	 * 未获 RADV 采纳（spec 声称同等适用，实测采样恒 0；spike 只验证了 GLSL 组合
	 * sampler2D=OpTypeSampledImage），回退链（docs/archive/phase3_plan.md R1）= 真
	 * sampler heap。容量 = reserved + 固定小裕量（共享一份 sampler 描述符，全模型管线
	 * 映射指向同一槽）。
	 * @return VmaDescriptorHeap（步长 = samplerDescriptorSize，运行时查询，见 impl）；
	 *   底层 buffer 创建失败 = VMASuccessFailed
	 */
	std::expected<RGL::VmaDescriptorHeap, Error> createSamplerHeap();

	RGL::AppContext& vkInstance;  // 实例层引用（最先初始化）；allocator 创建依赖其 raii Instance
	RGL::DeviceContext& vkDevice;  // 设备层引用；heap 属性查询 / 描述符写入 / 深度图创建
	RGL::Swapchain& vkSwapchain;  // 交换链层引用；构造期深度图 extent（resize 后重建经 recreateDepthImage）
	RGL::VmaAllocatorRAII allocator;  // VMA 分配器（BDA flag，见 impl 构造器）；全部 buffer/image/heap 经它创建
	RGL::VmaImage depthImage;  // 深度图（eD32Sfloat；swapchain resize 后重建，重建前 waitIdle）
	vk::raii::ImageView depthImageView;  // 深度图 view（raii，不可拷贝——成员初始化列表构造；与 depthImage 同步重建）
	RGL::VmaDescriptorHeap descriptorHeap;  // resource heap（用户槽 = allocSlot 单调段，布局契约见构造器注释）
	RGL::VmaDescriptorHeap samplerHeap;  // Phase 3：OpTypeSampler 独立 heap
	RGL::MipGen mipGen;  // Phase 8 ③b：compute mip 链生成器（heap + shader object，启动期一次性）
	bool mipGenBound = false;  // mip-gen heap 引用已注入标记（ensureMipGenReady 首建守卫；幂等）
	RGL::EnvPrefilter envPrefilter;  // Phase 8 ④b：GGX 预滤波 env cubemap 生成器（heap + shader object，启动期一次性）
	bool envPrefilterBound = false;  // env-prefilter heap 引用已注入标记（ensureEnvPrefilterReady 首建守卫；幂等）
	// 共享 sampler 的 create info 稳定存储（ensureSharedSampler 写入；
	// 本机 RADV 写时即解析、spec 允许驱动延迟解释，生命周期须覆盖运行期，见其注释）
	vk::SamplerCreateInfo samplerCreateInfo{};
	uint32_t sharedSamplerSlot = 0;   // 共享 sampler 在 sampler heap 的槽号（ensureSharedSampler 启动期写入）
	bool sharedSamplerReady = false;  // 共享 sampler 就绪标记（ensureSharedSampler 幂等守卫）
	// Phase 5.5：sky/env sampler（第二槽，ensureSkySampler 幂等写入）+ 环境 cubemap 单例
	vk::SamplerCreateInfo skySamplerCreateInfo{};  // create info 稳定存储（同 samplerCreateInfo 规则）
	uint32_t skySamplerSlot = 0;  // sky sampler 槽号（= reservedSlots + 1）
	bool skySamplerReady = false;  // sky sampler 就绪标记（ensureSkySampler 幂等守卫）
	std::optional<TextureAsset> envAsset;  // env cubemap 单例（nullopt = 未上传环境；场景级资产，见 emplaceEnvTexture 注释）
	std::array<glm::vec4, 9> envIrradianceSH{};  // Phase 8 ④a：环境漫反射 radiance SH9（env 加载期算，见 setEnvIrradianceSH）
	// SceneData（Phase 5 批次 2）：设备端动态数据 buffer + 运行时槽（ensureSceneData 幂等写入）
	RGL::VmaBuffer sceneDataBuffer;  // 设备端 scene data buffer（BDA + eTransferDst；未创建 = 空句柄）
	uint32_t sceneDataSlot = 0;  // scene data 在 resource heap 的槽号（allocSlot 领取）
	bool sceneDataReady = false;  // 就绪标记（ensureSceneData 幂等守卫）
	// 统一 slot 分配器（Phase 3，纹理资产并案）：单调分配游标，从 reservedSlots 起
	uint32_t nextSlot;  // allocSlot 游标（构造期 = reservedSlots；单调递增，无回收）
	// 资产表（E1.1/E1.2：已上传 primitive 的 GPU 数据 + heap slot + 三角形数 + 管线条目引用）
	RGL::AssetTable assetTable;  // 资产表单一数据源（ECS 规范 5）；生命周期与本对象一致
	// Phase 6 批次 4：offscreen attachment RT 缓存（ensureRtAttachment，key = 名）
	std::map<std::string, RtAttachment> rtAttachments;  // std::map 节点地址稳定（RtAttachment 指针链前提）
	/** 用户槽位固定裕量（与场景内容解耦；Phase 3 统一步长 = max(buffer, image)
	 * descriptorSize，运行时查询——实测 RADV 64B（Mesa 26.2.1；首测 32B 为旧 Mesa 值）/
	 * NVIDIA 32B / Intel 64B，4096 槽为 MB 级开销）。Phase 8 批次 ③a：纹理四元组
	 *（base/normal/orm 独立去重上传 + 白 + 中性法线）后 bistro 纹理槽 ~1200+，
	 * 1024 不够 → 4096。 */
	static constexpr uint32_t kUserSlotMargin = 4096;
};

}  // namespace RGL
