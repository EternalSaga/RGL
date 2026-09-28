module;
#include <vk_mem_alloc.h>  // VmaAllocationCreateInfo（VmaRaii 不导出 C 层，使用方自含，gpu_cull_impl 同款）
#include <glm/glm.hpp>  // sizeof(glm::vec2/mat4) 参与布局常量派生（GMF 三方头白名单内）
export module scene_data;

import rlstd;
import rlvk;
import lights_types_generated;  // GpuLight struct + kMaxLights/kGpuLightStride/kSceneDataLightArrayBase
import scene_forward_types_generated;  // 前向头 SceneLight（C API 反射生成）
import VmaRaii;
import vk_device;
import vk_resources;  // 架构债务续账（2026-09-19）：服务向内核领 buffer/slot——单向依赖，内核不认识本模块

namespace RGL {

// SceneData：场景级每帧动态数据的 bindless 契约（Phase 5 批次 2；2026-09-19 架构债务
// 步骤 1 由 vk_resources 拆出——布局常量性质同 lights_types_generated，与"资源管理"无关）。
// 单一 storage buffer（heap slot 经 Resources::allocSlot Runtime 运行时段分配——纹理槽数随场景动态，
// light 槽无法钉编译期偏移，同 slotTexture 模式），字节布局的单一真相源（shader 端
// scene.slang 字面量必须一致，kTrianglesPerGroup 先例——无反射端对应）：
//   @0   SceneLight（48B：direction/eyePos/color/ambient 各 vec3，C API 反射生成，
//        见 scene_forward_types_generated / docs/slang_api_reflection_research.md）
//   @48  Phase 5.5 env 参数（原 16B 保留区启用）：envIntensity float + envMipRange
//        float（= mipLevels−1，0 = 无环境）+ 8B 留白——scene.slang/env.slang 字面量一致
//   @64  Phase 5.5 批次 4 skyboxInvViewProj（mat4 列主序 = inverse(proj·view)，
//        sky.slang skyClipToWorld 消费）
//   @128 对象矩阵表：mat4 列主序 × kSceneMatrixCapacity（[objectSlot] = spin·world，
//        实例级 Phase 6 instancing；DrawArgs.objectIndex = MeshRef.objectSlot 索引；
//        世界空间光照 + Phase 7 shadow/culling 复用）
// 内容每帧变 = 全项目首个动态 GPU 数据：CPU 写 staging ring（×MAX_FRAMES_IN_FLIGHT，
// drawFrame ① 已 wait 本 slot fence → 写安全）+ 录制期 vkCmdCopyBuffer 拷进本 buffer
//（命令流内天然有序；描述符静态指向本 buffer 永不改动，见 vkrenderer.cppm）。

// 前向头 SceneLight（scene data @0-48）= C API 反射生成（scene_forward_types_generated，
// 布局真相源 = scene.slang SceneLight，逐字段 offsetof static_assert 钉死）。原手写
// LightDataGpu 已删（双源消除）——默认成员初始化为死代码（renderListSystem 每帧显式
// 聚合初始化 .light，从不用默认值），故无默认不影响行为。
// 注意：SceneLight 无默认成员初始化；消费方须显式赋值（render_list 已保证）。

/** Phase 5.5：env 参数偏移（envIntensity float @48 + envMipRange float @52；env.slang
 * readEnvParams 的 Load(48)/Load(52) 字面量必须与本值一致——scene.slang 先例）。派生自
 * sizeof(SceneLight)（C API 反射生成），shader 布局漂移即编译失败。 */
export inline constexpr uint32_t kSceneDataEnvOffset = sizeof(SceneLight);
/** tonemap 曝光（scene data 头 @56，env 16B 之后的保留区；tonemap.frag Load(56) 直读，
 *  ACES 前线性倍率，1.0 = 直通）。 */
export inline constexpr uint32_t kSceneDataExposureOffset = 56;
/** Phase 5.5 批次 4：skyboxInvViewProj mat4 列主序 @64（inverse(proj·view)，sky.slang
 * skyClipToWorld 的 Load4(64/80/96/112) 字面量必须与本值一致）。 */
export inline constexpr uint32_t kSceneDataSkyOffset = 64;
export inline constexpr uint32_t kSceneDataHeaderBytes = 128;   // light 48 + env 16（含曝光 4B@56）+ sky invVP 64
static_assert(kSceneDataEnvOffset == 48 && kSceneDataSkyOffset == 64 &&
	kSceneDataExposureOffset == kSceneDataEnvOffset + sizeof(glm::vec2) &&
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
 * scene.slang readObjectMvp 按 objectIndex 读——DrawArgs 去 mvp 字段后 push 36B（当时值，
 * 后 bindless 化终态 4B，见 kSceneDataDrawTableOffset），
 * 绕 NV DGC PUSH token >83B 多序列挂死（见 docs/调试历史）。
 * 基址 = 灯数组尾（262272 + 528 = 262800，scene.slang 字面量同值）。 */
export inline constexpr uint32_t kSceneDataMvpTableOffset =
	kSceneDataLightArrayOffset + kSceneDataLightArrayBytes;
static_assert(kSceneDataMvpTableOffset % 16 == 0, "MVP table must stay 16B aligned");

/** Phase 7b+（NV DGC 规避，续 MVP 表）：per-object 材质/几何表区（DrawArgs 去 mvp 后
 * 仍剩 baseColor/roughnessMetallic/totalTriangles/vertexOffset 是 per-object 数据，一并
 * 进 heap，DrawArgs 最终只留 objectIndex（4B push）——彻底绕开 NV DGC push 挂死）。
 * 基址 = MVP 表尾（262800 + 4096×64 = 524944，scene.slang 字面量同值，下方 static_assert
 * 钉死）。stride 48（`ObjectDraw`，scene_forward_types_generated 生成；Phase 8 ③ 加
 * emissive 后 32→48）。 */
export inline constexpr uint32_t kSceneDataDrawTableOffset =
	kSceneDataMvpTableOffset + kSceneMatrixCapacity * kSceneMatrixStride;
export inline constexpr uint32_t kSceneDrawStride = 48;
static_assert(kSceneDataDrawTableOffset % 16 == 0, "draw table must stay 16B aligned");
// Phase 10 批 10.2a：钉死 scene.slang readObjectDraw 的字面量基址/stride——容量常量若变
// 而 shader 字面量未同步 = 此处编译报错（shader 侧无编译期反射通道，kTrianglesPerGroup 同纪律）
static_assert(kSceneDataDrawTableOffset == 524944, "scene.slang readObjectDraw literal 524944 out of sync");
static_assert(kSceneDrawStride == sizeof(ObjectDraw),
	"scene.slang ObjectDraw (reflection-generated) stride out of sync with table stride");

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
 * 辐照度 SH9 区（= 721696B，编译期派生）；SceneDataService::ensure 的分配尺寸与
 * drawFrame 每帧 staging 拷贝区尺寸均为本值（vkrenderer.cppm ⑥.5）。 */
export inline constexpr vk::DeviceSize kSceneDataBytes =
	kSceneDataHeaderBytes + vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride
	+ kSceneDataLightArrayBytes
	+ vk::DeviceSize{kSceneMatrixCapacity} * kSceneMatrixStride
	+ vk::DeviceSize{kSceneMatrixCapacity} * kSceneDrawStride
	+ kSceneDataIrradianceBytes;

/**
 * SceneData buffer 服务（2026-09-19 架构债务续账步骤 1：原 Resources::ensureSceneData/
 * getSceneDataBuffer + 三成员整体搬出——SceneData 是"shader 布局契约的 GPU 载体"，
 * 不是资源内核职责）。向内核领 buffer + 槽（Resources& 单向 import，内核不认识本模块），
 * 幂等语义与原 Resources 方法逐字节一致（scene.data 领运行时段槽）。
 * 生命周期：main 栈对象，构造于 Resources 之后、一切取用者（ShaderRegistry / GpuCull /
 * Renderer）之前。
 */
export class SceneDataService {
   public:
	/**
	 * @param kernel 资源内核（allocator / descriptorHeap / allocSlot 领槽）
	 * @param device 设备层（描述符写入）
	 */
	SceneDataService(RGL::Resources& kernel, RGL::DeviceContext& device)
		: kernel(kernel), device(device) {}

	/**
	 * SceneData buffer（Phase 5 批次 2）：幂等创建设备端 storage buffer（eTransferDst——
	 * 每帧内容由 staging 拷入）+ 描述符写 allocSlot 运行时段槽（同 slotTexture 模式，
	 * 纹理槽数随场景动态、light 槽无编译期偏移）。调用点 = 建模型管线条目前
	 *（ShaderRegistry::addModelEntry，同 ensureSharedSampler 时序）。
	 * @return resource heap 绝对槽号（binding mapping 烘焙用，shader 端 binding 3，
	 *   见 scene.slang）
	 */
	[[nodiscard]] uint32_t ensure();

	/** @return SceneData 设备端 buffer const 引用（drawFrame 每帧 staging copy 的
	 * 目标；未创建 = 空句柄，drawFrame 跳过拷贝——无模型场景 ensure 未被调用） */
	[[nodiscard]] const RGL::VmaBuffer& getBuffer() const { return sceneDataBuffer; }

   private:
	RGL::Resources& kernel;  // 资源内核引用（领 allocator/heap/槽）
	RGL::DeviceContext& device;  // 设备层引用（writeResourceDescriptorsEXT）
	RGL::VmaBuffer sceneDataBuffer;  // 设备端 scene data buffer（BDA + eTransferDst；未创建 = 空句柄）
	uint32_t sceneDataSlot = 0;  // scene data 在 resource heap 的槽号（allocSlot 领取）
	bool sceneDataReady = false;  // 就绪标记（ensure 幂等守卫）
};

/**
 * 幂等契约见 SceneDataService::ensure 注释。实现：buffer usage =
 * eShaderDeviceAddress | eStorageBuffer | eTransferDst，VMA_MEMORY_USAGE_AUTO
 * device-local（CPU 不直接写——每帧唯一写入方 = staging copy，勿用 createGpuBuffer 的
 * CPU_TO_GPU+MAPPED 形态，见体内注释）；slot = allocSlot(Runtime, "scene.data")；描述符 =
 * eStorageBuffer，data = buffer BDA 地址范围（全 kSceneDataBytes）。
 */
inline uint32_t SceneDataService::ensure() {
	if (sceneDataReady)
		return sceneDataSlot;
	vk::BufferCreateInfo info{};
	info.size = kSceneDataBytes;
	info.usage = vk::BufferUsageFlagBits::eShaderDeviceAddress |
		vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eTransferDst;
	VmaAllocationCreateInfo alloc{};
	// device-local：CPU 不直接写（每帧唯一写入方 = staging copy），勿用 createGpuBuffer 的
	// CPU_TO_GPU+MAPPED 形态
	alloc.usage = VMA_MEMORY_USAGE_AUTO;
	auto bufResult = kernel.getAllocator().createBufferWithAddress(device.getDevice(), info, alloc);
	if (!bufResult)
		throw std::runtime_error(bufResult.error().describe());
	sceneDataBuffer = std::move(*bufResult);

	auto slotResult = kernel.allocSlot(RGL::SlotSegment::Runtime, "scene.data");
	if (!slotResult)
		throw std::runtime_error(slotResult.error().describe());
	sceneDataSlot = *slotResult;

	vk::DeviceAddressRangeEXT range{sceneDataBuffer.getDeviceAddress(), kSceneDataBytes};
	vk::ResourceDescriptorInfoEXT desc{};
	desc.type = vk::DescriptorType::eStorageBuffer;
	desc.data.pAddressRange = &range;
	auto slotAddr = kernel.getDescriptorHeap().getSlotCpuAddress(sceneDataSlot);
	if (!slotAddr)
		throw std::runtime_error(slotAddr.error().describe());
	vk::HostAddressRangeEXT dst{*slotAddr, kernel.getDescriptorHeap().getDescriptorSize()};
	device.getDevice().writeResourceDescriptorsEXT(
		std::array<vk::ResourceDescriptorInfoEXT, 1>{desc},
		std::array<vk::HostAddressRangeEXT, 1>{dst});
	sceneDataReady = true;
	return sceneDataSlot;
}

}  // namespace RGL
