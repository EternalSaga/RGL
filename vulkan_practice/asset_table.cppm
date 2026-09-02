module;
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_raii.hpp>
#include <glm/glm.hpp>
export module asset_table;

import rlstd;
import VmaRaii;
import model_types_generated;

namespace RGL {

// 资产层（ECS 规范 5：entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args 五方一致性
// 收敛为本表一处映射）。2026-09-02 拆自 vkresources：纯资产数据结构与
// Resources（Vulkan 初始化层：allocator/heap/scene data）解耦——引用资产的模块
// （model_importer/texture_pool/shader_registry/ecs/main）只依赖本轻量模块 +
// 按需 import vk_resources（GPU 侧操作），不必拖入整个初始化层。

// 管线表下标的"未设置"值（资产记录的 pipelineIndex 初始值；ShaderRegistry::addModelEntry / bindPipelineToAsset 写入真实下标）
export inline constexpr uint32_t kInvalidPipelineIndex = std::numeric_limits<uint32_t>::max();

// 纹理资产 id 的"未设置"值（同 kInvalidPipelineIndex 模式；批次 2 材质资产的 textureId 用它 = 无纹理 → 白）
export inline constexpr uint32_t kInvalidTextureId = std::numeric_limits<uint32_t>::max();

// 资产种类（批次 1 引入：白纹理 slot 契约断言只数模型顶点槽；批次 3 cube 实体化数据源）。
// Phase 4 已归一 push constant（统一 DrawArgs 超集单一布局）：renderListSystem 零 AssetKind
// 分支，kind 不再参与布局/写入路径选择（历史 64B/96B 双 struct 分支已删）
export enum class AssetKind : uint8_t {
	Model,
	Cube,
};

// 资产层：每个已上传 primitive 一条记录（GPU 顶点 buffer + heap slot + 三角形数 + 管线条目引用），
// 以稳定 id（vector 下标，上传顺序分配）索引。registry entity 经 MeshRef{asset}（scene.cppm）
// 引用资产，五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为本表
// 一处映射（E1.1）。生命周期：Resources 成员（与 descriptor heap / cube buffer 同层），
// buffer 随 SharedAllocator 控制块存活，不再靠 main.cpp 局部变量的隐式 RAII 保证（E1.2/L6）。
// 顶点 buffer 经 shared_ptr 共享（Phase 4 批次 2：拼接顶点 buffer——每模型一个 buffer +
// 一个 heap slot，同模型 N 个 primitive 资产记录持同一 shared_ptr，末引用析构释放；
// VmaBuffer 本体 move-only 不可拷贝，故经 shared_ptr 共享所有权）。
export struct MeshAsset {
	AssetKind kind = AssetKind::Model;
	std::shared_ptr<VmaBuffer> vertexBuffer;          // 顶点 buffer（模型 = 拼接 buffer 共享；cube = 独占 shared_ptr）
	uint32_t heapSlot = 0;                            // 绝对 heap slot（= reservedSlots() + 偏移，见 uploadModelMeshes）
	uint32_t triangleCount = 0;                       // primitive 三角形数
	uint32_t groupCount = 0;                          // mesh task group 数（上传期按 shader 契约派生，D6：
	                                                 //   model = ceil(triangleCount / kTrianglesPerGroup)，cube = 1 固定单 group——
	                                                 //   cube mesh shader 恒输出 8 顶点/12 三角形，不套 ceil 公式（group>1 会越界读）；
	                                                 //   0 = 不渲染。renderListSystem 每帧零推导零分支直接取）
	uint32_t pipelineIndex = kInvalidPipelineIndex;   // 管线表下标（ShaderRegistry::addModelEntry / bindPipelineToAsset 写入）
	uint32_t vertexOffset = 0;                        // 拼接顶点 buffer 内 per-primitive 字节偏移（Phase 4 批次 2；
	                                                 //   进 push constant 的 DrawArgs.vertexOffset，model.mesh.slang Load 前缀；cube = 0）
	// 索引 buffer（批次 3：cube 资产带索引；模型 primitive 去索引展开 = nullopt）
	std::optional<VmaBuffer> indexBuffer;
	std::optional<uint32_t> indexHeapSlot;
};

// 纹理资产（Phase 3 → 批次 1 迁入资产表）：GPU image + view + heap 描述符槽 + 描述符指针链。
// 原 owner = main 局部 TexturePool 对象（UAF 根因：块尾析构第 0 帧前释放纹理 BO，
// docs/heap_texture_research.md §12.6）；现活在 AssetTable（Resources 成员），VmaImage
// 包装体随 Resources 存活——所有权规则（§12.7 ①）在此首次落地。
// viewCreate 必须保留：v341 的 VkImageDescriptorInfoEXT::pView 要 image view 的
// create info（值拷贝），不是句柄（docs/heap_texture_research.md §二 条款表）。
// 描述符指针链（imageDesc/resourceDesc/dst）折进记录本体：记录活在 std::deque
//（push_back 元素地址稳定），自/兄弟引用在入表后设置——跨驱动防御稳定存储的单一介质
//（替代原 TexturePool 的 4 个 deque；本机 RADV 写时即解析，spec 允许驱动延迟解释，
// 见 docs/heap_texture_research.md §12.2）。
export struct TextureAsset {
	VmaImage image;
	vk::ImageViewCreateInfo viewCreate{};  // hpp create info（值保留）：v341 VkImageDescriptorInfoEXT::pView 要它
	vk::raii::ImageView view;
	uint32_t heapSlot = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	// Phase 5.5：mip 级数（2D 路径 = 1；cubemap = 1+log2(size)，sky sampler maxLod 由此派生）
	uint32_t mipLevels = 1;
	// 描述符指针链（自/兄弟引用，入 deque 后设置——元素地址稳定保证指针有效）
	vk::ImageDescriptorInfoEXT imageDesc{};        // pView = &viewCreate，layout = eShaderReadOnlyOptimal
	vk::ResourceDescriptorInfoEXT resourceDesc{};  // type = eSampledImage，data.pImage = &imageDesc
	vk::HostAddressRangeEXT dst{};                 // 描述符写入目标 slot 范围
};

// 材质资产（批次 2：MaterialRef 真引用，E5.3 解决）：glTF 材质身份（按 materialIndex 去重，
// -1 = 无材质 → 默认材质）。纹理→primitive 映射获得数据归属——材质记录引用纹理资产 id
//（可查询，Phase 5 透明排序 / Phase 7 culling 的材质分类数据基础）；glTF 材质身份不再
// 导入期丢弃。注意区分：textureId = 材质**引用**的纹理（baseColorTexture，可缺省）；
// primitive 的**有效**纹理 = 有 uv 且图有内嵌数据 ? material.textureId : 白（hasUv 是
// per-primitive 网格属性，同材质可因缺 uv 走白——有效选择留在上传返回值，见 model_importer）。
// GPU 侧仍走 per-primitive 管线 binding mapping 烘焙（行为不变，去重归 Phase 4 ShaderRegistry）。
export struct MaterialAsset {
	glm::vec4 baseColor{1.0f};
	uint32_t textureId = kInvalidTextureId;
	bool alphaModeBlend = false;  // glTF alphaMode == "BLEND"（Phase 5 透明用，预留）
	// Phase 4 批次 3：材质风格字段（registerScene 金丝雀保证同 materialIndex 一致）
	float metallic = 1.0f;        // pbrMetallicRoughness.metallicFactor（缺省 1.0）
	float roughness = 1.0f;       // pbrMetallicRoughness.roughnessFactor（缺省 1.0）
	bool unlit = false;           // KHR_materials_unlit 扩展存在性（风格 = unlit vs pbr）
};

export class AssetTable {
   public:
	// mesh（模型 primitive：去索引顶点流，无索引 buffer）
	// 注册资产（上传顺序调用）→ 资产 id
	// groupCount 按 model shader 契约派生（D6：ceil(tri/kTrianglesPerGroup)，0 = 不渲染）
	// vertexBuffer = 拼接顶点 buffer 的 shared_ptr（同模型 N 个 primitive 共享同一 buffer，
	// Phase 4 批次 2）；vertexOffset = 本 primitive 在拼接 buffer 内的字节偏移（进 push constant）
	uint32_t addMesh(std::shared_ptr<VmaBuffer> vertexBuffer, uint32_t heapSlot,
		uint32_t triangleCount, uint32_t vertexOffset) {
		const uint32_t groupCount = (triangleCount + kTrianglesPerGroup - 1) / kTrianglesPerGroup;
		meshes.push_back(
			MeshAsset{AssetKind::Model, std::move(vertexBuffer), heapSlot, triangleCount, groupCount,
				kInvalidPipelineIndex, vertexOffset});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	[[nodiscard]] const MeshAsset& get(uint32_t id) const { return meshes[id]; }
	// 管线条目引用（ShaderRegistry::addModelEntry / bindPipelineToAsset 在创建管线条目后写回）
	void setPipelineIndex(uint32_t id, uint32_t pipelineIndex) { meshes[id].pipelineIndex = pipelineIndex; }
	// 带索引 mesh（批次 3：cube 资产——顶点 + 索引双 buffer/双 slot）
	// groupCount = 1 固定（cube mesh shader 恒输出 8 顶点/12 三角形，单 group，不套 ceil 公式）
	uint32_t addIndexedMesh(VmaBuffer vertexBuffer, uint32_t vertexSlot, VmaBuffer indexBuffer,
		uint32_t indexSlot, uint32_t triangleCount) {
		meshes.push_back(MeshAsset{AssetKind::Cube, std::make_shared<VmaBuffer>(std::move(vertexBuffer)),
			vertexSlot, triangleCount, 1, kInvalidPipelineIndex, 0, std::move(indexBuffer), indexSlot});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	[[nodiscard]] uint32_t meshSize() const { return static_cast<uint32_t>(meshes.size()); }
	// 模型顶点槽计数（Phase 4 批次 2：拼接顶点 buffer = 每模型一个 slot，白纹理 slot 契约断言用：
	// "纹理首槽 = 白 = 模型顶点槽之后"）。按模型数（非 primitive 数）计数——uploadModelMeshes
	// 每模型上传期递增一次；批次 3 cube 资产与本容器共用但不占模型顶点槽
	uint32_t modelSlotCount = 0;
	[[nodiscard]] uint32_t getModelSlotCount() const { return modelSlotCount; }
	void incrementModelSlotCount() { ++modelSlotCount; }

	// 纹理（deque：元素地址稳定 = 描述符指针链的稳定存储介质，见 TextureAsset 注释）
	// 放置资产记录（deque）→ 调用方在返回引用上设置自/兄弟引用后写描述符 → 资产 id
	TextureAsset& emplaceTexture(TextureAsset asset) {
		textures.push_back(std::move(asset));
		return textures.back();
	}
	[[nodiscard]] const TextureAsset& getTexture(uint32_t id) const { return textures[id]; }
	[[nodiscard]] uint32_t textureSize() const { return static_cast<uint32_t>(textures.size()); }

	// 材质（批次 2：MaterialRef{material} 真引用；registerScene 按 glTF materialIndex 去重注册）
	uint32_t addMaterial(MaterialAsset material) {
		materials.push_back(std::move(material));
		return static_cast<uint32_t>(materials.size() - 1);
	}
	[[nodiscard]] const MaterialAsset& getMaterial(uint32_t id) const { return materials[id]; }
	[[nodiscard]] uint32_t materialSize() const { return static_cast<uint32_t>(materials.size()); }

   private:
	std::vector<MeshAsset> meshes;
	std::deque<TextureAsset> textures;
	std::vector<MaterialAsset> materials;
};

}  // namespace RGL
