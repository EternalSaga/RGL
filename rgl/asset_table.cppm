module;
#include <glm/glm.hpp>
export module asset_table;

import rlstd;
import rlvk;
import VmaRaii;
import model_types_generated;

namespace RGL {

// 资产层（ECS 规范 5：entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args 五方一致性
// 收敛为本表一处映射）。2026-09-02 拆自 vkresources：纯资产数据结构与
// Resources（Vulkan 初始化层：allocator/heap/scene data）解耦——引用资产的模块
// （model_importer/texture_pool/shader_registry/ecs/main）只依赖本轻量模块 +
// 按需 import vk_resources（GPU 侧操作），不必拖入整个初始化层。

/**
 * 管线表下标的"未设置"值（资产记录的 pipelineIndex 初始值）。真实下标在管线条目创建后写回
 * 资产记录：addModelEntry 返回条目下标、调用方经 bindToAsset 写回；bindPipelineToAsset 按条目名写回。
 */
export inline constexpr uint32_t kInvalidPipelineIndex = std::numeric_limits<uint32_t>::max();

/**
 * 纹理资产 id 的"未设置"值（同 kInvalidPipelineIndex 模式）；批次 2 材质资产的 textureId
 * 用它 = 无纹理 → 白。
 */
export inline constexpr uint32_t kInvalidTextureId = std::numeric_limits<uint32_t>::max();

/**
 * 资产种类（批次 1 引入：白纹理 slot 契约断言只数几何槽；批次 3 内建几何实体化数据源）。
 * Phase 4 已归一 push constant（统一 DrawArgs 超集单一布局）：renderListSystem 零 AssetKind
 * 分支，kind 不再参与布局/写入路径选择（历史 64B/96B 双 struct 分支已删）。
 */
export enum class AssetKind : uint8_t {
	Model,
	Builtin,  // 内建几何（cube 等程序化资产，架构债务 1：与 gltf 同路的普通资产，非内核成员）
};

/**
 * 资产层：每个已上传 primitive 一条记录（GPU 顶点 buffer + heap slot + 三角形数 + 管线条目引用），
 * 以稳定 id（vector 下标，上传顺序分配）索引。registry entity 经 MeshRef{asset}（scene_context.cppm）
 * 引用资产，五方一致性（entity ↔ primitive ↔ heap slot ↔ 管线条目 ↔ draw-args）收敛为本表
 * 一处映射（E1.1）。生命周期：main 装配层持有（架构债务续账 F），
 * buffer 随 SharedAllocator 控制块存活，不再靠 main.cpp 局部变量的隐式 RAII 保证（E1.2/L6）。
 * 顶点 buffer 经 shared_ptr 共享（Phase 4 批次 2 分块拼接：模型 primitive 拼成 16MB 块，
 * 每块一个 buffer + 一个 heap slot，同块内 primitive 共享同一 shared_ptr，末引用析构释放；
 * VmaBuffer 本体 move-only 不可拷贝，故经 shared_ptr 共享所有权）。
 */
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
	                                                 //   Phase 7b+ 起进 scene data ObjectDraw 表（model.mesh.slang readObjectDraw
	                                                 //   取 od.vertexOffset 做 Load 前缀，不再走 push constant）；cube = 0）
	// 索引 buffer（批次 3：cube 资产带索引；模型 primitive 去索引展开 = nullopt）
	std::optional<VmaBuffer> indexBuffer;
	std::optional<uint32_t> indexHeapSlot;
	// Phase 11 批 11.2：阴影 depth-only 管线条目下标（ShaderRegistry::addShadowEntry 返回，
	// main 经 bindShadowToAsset 写回）。为何单独一个而不复用 pipelineIndex：两者绑的不是同一个
	// 纹理 buffer ——pipelineIndex 指向“读纹理 + 纹理槽”的模型管线，阴影管线只读顶点，
	// 且两者的去重键不同（模型按六元组、阴影按 vertexSlot）。
	// kInvalidPipelineIndex = 未回填（门控不开时 renderListSystem 跳过阴影段）。
	uint32_t shadowPipelineIndex = kInvalidPipelineIndex;
	// 簇化轴 ★1-a1ii-α：簇路径**影子叠加**管线条目下标（ShaderRegistry::addMeshletEntry 返回，
	// main 经 bindMeshletToAsset 写回）。不复用 pipelineIndex 的理由同 shadowPipelineIndex：
	// mesh 阶段读的是簇 buffer 而非本记录的拼接顶点 buffer，去重键也不同（按纹理集，
	// 簇路径全局只有一份顶点池）。kInvalidPipelineIndex = 未回填（门关 / 无产物 / 多产物
	// 回落），renderListSystem 据此跳过叠加 draw。
	uint32_t meshletPipelineIndex = kInvalidPipelineIndex;
	// 簇化轴 ★18-B1：阴影段**簇路径** depth-only 管线条目下标（ShaderRegistry::addShadowMeshletEntry
	// 返回，装配层经 bindShadowMeshletToAsset 写回）。不复用上面两者的理由各自成立：
	//  · 不复用 shadowPipelineIndex —— 那是"读本记录拼接顶点 buffer"的旧 mesh 管线（去重键 =
	//    vertexSlot），本条读全局簇 buffer、push 多一个 clusterStart（12B ≠ 8B），两者画的
	//    是同一份几何的**两种管线**（c0 走旧的、c1..3 走新的），必须能同时存在；
	//  · 不复用 meshletPipelineIndex —— 那是主 opaque 段的叠加/取代条目（frag 完整光照、
	//    ModelVertexOutput 五字段、烘 binding 21），与本条目的 depth-only 形态无交集。
	// kInvalidPipelineIndex = 未回填（门关 / 本档关 / 无簇产物 / 多产物回落）——批 B2 靠这个
	// 字段判「这个资产能不能走簇路径」，故**没有簇产物的资产必须天然保持 kInvalid**（逐 prim
	// 调用的烘焙形态见 bakeShadowMeshletEntries 注释）。
	uint32_t shadowMeshletPipelineIndex = kInvalidPipelineIndex;
	// ★M-1b：阴影段**真 MDI** 管线条目下标（ShaderRegistry::addShadowMeshletMdiEntry 返回，
	// 装配层经 bindShadowMeshletMdiToAsset 写回）。与上一条**并存不互斥**：同一条几何的第三种
	// 管线（逐条 draw 的簇路径 vs 每级联一次 MDI 的孔洞流行表），门 meshlet.mdi_shadow 决定采哪
	// 条——条目侧两下标各自 kInvalid = 各自不可用（MDI 条目只在行表载体**建成并烘好 binding 34**
	// 后回填，声明/mapping 同向由装配层单点保证 → main.cppm wireMeshletPostRegistration）。
	uint32_t shadowMeshletMdiPipelineIndex = kInvalidPipelineIndex;
};

/**
 * 纹理资产（Phase 3 → 批次 1 迁入资产表）：GPU image + view + heap 描述符槽 + 描述符指针链。
 * 原 owner = main 局部 TexturePool 对象（UAF 根因：块尾析构第 0 帧前释放纹理 BO，
 * docs/archive/heap_texture_research.md §12.6）；现活在 main 装配层持有的 AssetTable，VmaImage
 * 包装体随 AssetTable 存活——所有权规则（§12.7 ①）在此首次落地。
 * viewCreate 必须保留：v341 的 VkImageDescriptorInfoEXT::pView 要 image view 的
 * create info（值拷贝），不是句柄（docs/archive/heap_texture_research.md §二 条款表）。
 * 描述符指针链（imageDesc/resourceDesc/dst）折进记录本体：记录活在 std::deque
 *（push_back 元素地址稳定），自/兄弟引用在入表后设置——跨驱动防御稳定存储的单一介质
 *（替代原 TexturePool 的 4 个 deque；本机 RADV 写时即解析，spec 允许驱动延迟解释，
 * 见 docs/archive/heap_texture_research.md §12.2）。
 */
export struct TextureAsset {
	VmaImage image;
	vk::ImageViewCreateInfo viewCreate{};  // hpp create info（值保留）：v341 VkImageDescriptorInfoEXT::pView 要它
	vk::raii::ImageView view;
	uint32_t heapSlot = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	// Phase 5.5：mip 级数（2D 路径 = 1+floor(log2(max(w,h))，Phase 8 ③b compute 链；
	// cubemap = 1+log2(size)，sky sampler maxLod 由此派生）
	uint32_t mipLevels = 1;
	// 描述符指针链（自/兄弟引用，入 deque 后设置——元素地址稳定保证指针有效）
	vk::ImageDescriptorInfoEXT imageDesc{};        // pView = &viewCreate，layout = eShaderReadOnlyOptimal
	vk::ResourceDescriptorInfoEXT resourceDesc{};  // type = eSampledImage，data.pImage = &imageDesc
	vk::HostAddressRangeEXT dst{};                 // 描述符写入目标 slot 范围
};

/**
 * 材质资产（批次 2：MaterialRef 真引用，E5.3 解决）：glTF 材质身份（按 materialIndex 去重，
 * -1 = 无材质 → 默认材质）。纹理→primitive 映射获得数据归属——材质记录引用纹理资产 id
 *（可查询，Phase 5 透明排序 / Phase 7 culling 的材质分类数据基础）；glTF 材质身份不再
 * 导入期丢弃。注意区分：textureId = 材质**引用**的纹理（baseColorTexture，可缺省）；
 * primitive 的**有效**纹理 = 有 uv 且图有内嵌数据 ? material.textureId : 白（hasUv 是
 * per-primitive 网格属性，同材质可因缺 uv 走白——有效选择留在上传返回值，见 model_importer）。
 * GPU 侧仍走 per-primitive 管线 binding mapping 烘焙（行为不变，去重归 Phase 4 ShaderRegistry）。
 */
export struct MaterialAsset {
	glm::vec4 baseColor{1.0f};
	uint32_t textureId = kInvalidTextureId;
	bool alphaModeBlend = false;  // glTF alphaMode == "BLEND"（Phase 5 透明用，预留）
	// Phase 4 批次 3：材质风格字段（registerScene 金丝雀保证同 materialIndex 一致）
	float metallic = 1.0f;        // pbrMetallicRoughness.metallicFactor（缺省 1.0）
	float roughness = 1.0f;       // pbrMetallicRoughness.roughnessFactor（缺省 1.0）
	bool unlit = false;           // KHR_materials_unlit 扩展存在性（风格 = unlit vs pbr）
	glm::vec3 emissive{0.0f};     // Phase 8 ③：material emissiveFactor（含 strength 预乘；缺省黑 = 不发光）
	bool alphaModeMask = false;   // Phase 8 C1：alphaMode == "MASK"（alpha-test cutout）
	float alphaCutoff = 0.5f;     // Phase 8 C1：MASK 截止（进 ObjectDraw.emissive.w；非 MASK = 0 = 不测）
	bool doubleSided = false;     // Phase 8 C1：doubleSided（背面不剔 + 法线翻向）
};

/**
 * MaterialAsset → roughnessMetallic vec2（**x=roughness / y=metallic**）——该字段序在
 * C++ 侧的唯一拼写处（债-12 单源化）。shader 侧字段序契约 = ObjectDraw.roughnessMetallic
 * @8（scene.slang，deferred_geometry.frag 按 x=roughness y=metallic 消费）与
 * CullEntry @52（cull.comp.slang 头注释）；两个写端（render_list 写 ObjectDraw、
 * main C3 写 CullEntry）都经本函数，字段序不可能只改一处。
 */
export inline glm::vec2 roughnessMetallic(const MaterialAsset& mat) {
	return {mat.roughness, mat.metallic};
}

/**
 * 资产表：资产层单一数据源（ECS 规范 5）——mesh / texture / material 三表，均以稳定
 * id（表内下标，注册顺序分配）索引，五方一致性收敛为本表一处映射。生命周期 =
 * main 装配层（架构债务续账 F）。几何槽计数（geometrySlotCount）+
 * expectedGeometrySlot 承载 descriptor heap 几何段布局契约（上传方断言：
 * model_importer / builtin_geometry）；白纹理契约由 Resources 纹理段首槽断言。
 */
export class AssetTable {
   public:
	/**
	 * 注册模型 primitive 资产（去索引顶点流，无索引 buffer；上传顺序调用，调用方 =
	 * uploadModelMeshes）→ 资产 id。groupCount 按 model shader 契约派生（D6：
	 * ceil(tri/kTrianglesPerGroup)，0 = 不渲染）。vertexBuffer = 拼接块 buffer 的
	 * shared_ptr（同块内 primitive 共享同一 buffer，Phase 4 批次 2 分块拼接）；
	 * vertexOffset = 本 primitive 在所在块内的字节偏移（进 push constant）；
	 * heapSlot = 本 primitive 所在块的绝对槽（同块 primitive 共享同一槽）。
	 */
	uint32_t addMesh(std::shared_ptr<VmaBuffer> vertexBuffer, uint32_t heapSlot,
		uint32_t triangleCount, uint32_t vertexOffset) {
		const uint32_t groupCount = (triangleCount + kTrianglesPerGroup - 1) / kTrianglesPerGroup;
		meshes.push_back(
			MeshAsset{AssetKind::Model, std::move(vertexBuffer), heapSlot, triangleCount, groupCount,
				kInvalidPipelineIndex, vertexOffset, std::nullopt, std::nullopt});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	/** @return 按 id 引用的 mesh 资产记录（无界检查，调用方保证 id 有效）。 */
	[[nodiscard]] const MeshAsset& get(uint32_t id) const { return meshes[id]; }
	/** 写回管线条目引用（ShaderRegistry::addModelEntry / bindPipelineToAsset 在创建管线条目后调用）。 */
	void setPipelineIndex(uint32_t id, uint32_t pipelineIndex) { meshes[id].pipelineIndex = pipelineIndex; }
	/** 写回阴影管线条目引用（ShaderRegistry::addShadowEntry 后由 bindShadowToAsset 调用）。 */
	void bindShadowPipeline(uint32_t id, uint32_t entryIdx) { meshes[id].shadowPipelineIndex = entryIdx; }
	/** 写回簇路径叠加管线条目引用（★1-a1ii-α，ShaderRegistry::addMeshletEntry 后调用）。 */
	void bindMeshletPipeline(uint32_t id, uint32_t entryIdx) { meshes[id].meshletPipelineIndex = entryIdx; }
	/** 写回簇路径**阴影段**管线条目引用（★18-B1，ShaderRegistry::addShadowMeshletEntry 后调用）。 */
	void bindShadowMeshletPipeline(uint32_t id, uint32_t entryIdx) {
		meshes[id].shadowMeshletPipelineIndex = entryIdx;
	}
	/** 写回阴影段**真 MDI** 管线条目引用（★M-1b，addShadowMeshletMdiEntry 后调用）。 */
	void bindShadowMeshletMdiPipeline(uint32_t id, uint32_t entryIdx) {
		meshes[id].shadowMeshletMdiPipelineIndex = entryIdx;
	}
	/**
	 * 注册带索引 mesh 资产（内建几何：顶点 + 索引双 buffer/双 slot，架构债务 1 方案 B 后由
	 * builtin_geometry 上传路径调用——Resources 不再创建 cube buffer）。
	 * groupCount = 1 固定（cube mesh shader 恒输出 8 顶点/12 三角形，单 group，不套 ceil 公式）
	 * → 资产 id。
	 */
	uint32_t addIndexedMesh(VmaBuffer vertexBuffer, uint32_t vertexSlot, VmaBuffer indexBuffer,
		uint32_t indexSlot, uint32_t triangleCount) {
		meshes.push_back(MeshAsset{AssetKind::Builtin, std::make_shared<VmaBuffer>(std::move(vertexBuffer)),
			vertexSlot, triangleCount, 1, kInvalidPipelineIndex, 0, std::move(indexBuffer), indexSlot});
		return static_cast<uint32_t>(meshes.size() - 1);
	}
	/** @return mesh 资产记录数（模型 primitive + 内建几何）。 */
	[[nodiscard]] uint32_t meshSize() const { return static_cast<uint32_t>(meshes.size()); }
	/**
	 * 几何槽计数（Phase 4 批次 2 分块拼接 = 模型每 16MB 块一个 slot；架构债务 1 方案 B 后
	 * 含内建几何的顶点槽与索引槽——一切几何都是 allocSlot Geometry 段的普通消费者）。
	 * expectedGeometrySlot 几何段契约断言用（texture_pool::ensureWhiteTexture 改按纹理段首槽）。
	 * 计数单位 = 槽（非模型、非 primitive 数）：uploadModelMeshes 每块 +1，
	 * uploadBuiltinCube +2（顶点 + 索引）
	 */
	uint32_t geometrySlotCount = 0;
	/** @return 几何槽计数（见 geometrySlotCount 注释）。 */
	[[nodiscard]] uint32_t getGeometrySlotCount() const { return geometrySlotCount; }
	/** 几何槽计数 +1（上传方在 allocSlot 成功后调用）。 */
	void incrementGeometrySlotCount() { ++geometrySlotCount; }
	/**
	 * 下一个几何槽的期望绝对槽号（= reservedSlots + HeapSlotGeometryBase + 已占几何槽数）。
	 * 上传方 allocSlot(Geometry) 后比对，违例 = 几何段中间插了别的分配（契约漂移，报
	 * DescriptorSlotOutOfBounds）；白纹理不再借用本断言，改断 Resources 纹理段首槽。
	 * @param reservedSlots descriptor heap 驱动保留槽位数（用户槽自 reservedSlots 起）
	 * @return 下一次几何分配的期望绝对槽号
	 */
	[[nodiscard]] uint32_t expectedGeometrySlot(uint32_t reservedSlots) const {
		return reservedSlots + HeapSlotGeometryBase + geometrySlotCount;
	}

	/**
	 * 放置纹理资产记录（deque：元素地址稳定 = 描述符指针链的稳定存储介质，见 TextureAsset
	 * 注释）→ 入表记录引用。调用方在返回引用上设置自/兄弟引用（imageDesc.pView /
	 * resourceDesc.data.pImage / dst）后写描述符；资产 id 由调用方按 textureSize()-1 派生。
	 * @param asset 待注册的纹理资产记录（GPU image + view + heap slot，按值入表）
	 * @return 入表后的记录引用（地址稳定，随表存活）
	 */
	TextureAsset& emplaceTexture(TextureAsset asset) {
		textures.push_back(std::move(asset));
		return textures.back();
	}
	/** @return 按 id 引用的纹理资产记录（无界检查，调用方保证 id 有效）。 */
	[[nodiscard]] const TextureAsset& getTexture(uint32_t id) const { return textures[id]; }
	/** @return 纹理资产记录数（首条 id 0 按调用序契约 = 白纹理，见 ensureWhiteTexture）。 */
	[[nodiscard]] uint32_t textureSize() const { return static_cast<uint32_t>(textures.size()); }

	/**
	 * 注册材质（批次 2：MaterialRef{material} 真引用；registerScene 按 glTF materialIndex
	 * 去重注册）。find-or-add（Phase 6 instancing）：材质是纯值数据，字段全等 = 同一资产——
	 * 同文件多 entity 重复 registerScene 时返回既有 id（纹理 id 本身经 loadEmbedded 去重，
	 * 参与比较稳定）。线性扫描，材质数量级（十~百）可忽略。
	 * @return 材质资产 id（全等既有则返回既有，否则新增）。
	 */
	uint32_t addMaterial(const MaterialAsset& material) {
		for (uint32_t i = 0; i < materials.size(); ++i)
			if (materials[i].baseColor == material.baseColor &&
				materials[i].textureId == material.textureId &&
				materials[i].alphaModeBlend == material.alphaModeBlend &&
				materials[i].metallic == material.metallic &&
				materials[i].roughness == material.roughness &&
				materials[i].unlit == material.unlit &&
				materials[i].emissive == material.emissive &&
				materials[i].alphaModeMask == material.alphaModeMask &&
				materials[i].alphaCutoff == material.alphaCutoff &&
				materials[i].doubleSided == material.doubleSided)
				return i;
		materials.push_back(material);
		return static_cast<uint32_t>(materials.size() - 1);
	}
	/** @return 按 id 引用的材质资产记录（无界检查，调用方保证 id 有效）。 */
	[[nodiscard]] const MaterialAsset& getMaterial(uint32_t id) const { return materials[id]; }
	/**
	 * 可变材质访问（Phase 10 批 10.2c 调试面板写入通道；无界检查同 getMaterial）。
	 * **调试语义注意**：材质 = find-or-add 值去重——改后与别的材质值全等也不会重新
	 * 归并（表内多一条，渲染正确性不受影响）；无回滚——编辑仅本会话有效（重启回
	 * config 值）。每帧 renderListSystem 读本表写 ObjectDraw 表 = 编辑下一帧生效。
	 */
	MaterialAsset& getMaterialMut(uint32_t id) { return materials[id]; }
	/** @return 材质资产记录数。 */
	[[nodiscard]] uint32_t materialSize() const { return static_cast<uint32_t>(materials.size()); }

   private:
	std::vector<MeshAsset> meshes;
	std::deque<TextureAsset> textures;
	std::vector<MaterialAsset> materials;
};

}  // namespace RGL
