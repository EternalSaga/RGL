module;
#include <glm/glm.hpp>
export module gltf_loader;

import rlstd;
import vkerror;
import model_types_generated;

namespace RGL {

/** 包围球（center + 半径，保守界）：GltfPrimitive::bounds 为节点局部球，GltfModel::bounds 为世界聚合球。 */
export struct BoundingSphere {
	glm::vec3 center{0.0f};
	float radius{0.0f};
};

/**
 * 去索引展开后的 primitive：vertices 按 ModelVertex packed 布局（32B，
 * model_schema.lua 的 vertexLayouts 是契约 source of truth）；
 * triangleCount = vertices.size() / 3。
 * 每个 glTF primitive 展开为一个实例（loadGltfModel 逐 primitive 产出）。
 */
export struct GltfPrimitive {
	std::vector<ModelVertex> vertices;
	uint32_t triangleCount = 0;
	glm::mat4 nodeWorld{1.0f};  // 所属 node 世界矩阵（Phase 7：TRS + 父链合成，不烘进顶点；无 node = identity）
	BoundingSphere bounds;       // 节点局部包围球（over raw vertices.position，不含 nodeWorld；剔除时 modelMatrix 再乘）
	glm::vec4 baseColor{1.0f};    // material 的 baseColorFactor
	int baseColorTextureIndex = -1;  // material baseColorTexture 的 image 下标（-1 = 无纹理 → 白纹理）
	bool hasUv = true;               // TEXCOORD_0 缺失（glTF 中可选）→ uv 填 0 + 白纹理（D9）
	bool alphaModeBlend = false;    // material alphaMode == "BLEND"（Phase 5 透明用，预留）
	int materialIndex = -1;         // glTF materials 数组下标（-1 = 无材质）——批次 2 材质资产去重键
	// Phase 4 批次 3：材质风格字段（loader 输出即数据源，风格→管线映射经 scene.lua styles）
	float metallic = 1.0f;          // pbrMetallicRoughness.metallicFactor（缺省 1.0）
	float roughness = 1.0f;         // pbrMetallicRoughness.roughnessFactor（缺省 1.0）
	bool unlit = false;             // material.extensions["KHR_materials_unlit"] 存在性
};

/**
 * 纹理源数据（Phase 3）：span 指向 GltfModel::bufferData（自 tinygltf buffer 0 move 而来，
 * GltfModel 存活期内有效）；外部多 buffer 的 .gltf 不支持（data 为空 span）。
 */
export struct GltfImage {
	std::span<const uint8_t> data;
	std::string mimeType;
};

/**
 * glTF 模型（loadGltfModel 产物）：primitives = 全部 mesh 的逐 primitive 去索引展开；
 * bounds = 全模型世界聚合包围球（自适应空间范围用，ecs.cppm 的 renderSystem）。
 * bufferData/images 为纹理源数据（Phase 3）：buffer 0 原始数据（move 自 tinygltf，零拷贝），
 * images 的 span 以此为宿主；几何数据已拷入 vertices，此处仅为纹理解码保留。
 */
export struct GltfModel {
	std::vector<GltfPrimitive> primitives;
	BoundingSphere bounds;  // 自适应空间范围用（ecs.cppm 的 renderSystem）
	std::vector<uint8_t> bufferData;
	std::vector<GltfImage> images;
};

/**
 * 加载 glTF / glTF-Binary：每个 mesh 的每个 primitive 展开为一个 GltfPrimitive
 *（POSITION/NORMAL 必需 + TEXCOORD_0 可选（D9，缺失 → 白纹理）+ 去索引展开 +
 * baseColorFactor/baseColorTexture/alphaMode + 纹理源数据 bufferData/images）。
 * 节点世界矩阵经层级 TRS + 父链合成（nodeWorld；环防御 = 残留退化 local + warn）。
 * 实现体在 gltf_loader_impl.cppm（tinygltf 解析 + accessor 读取 helper）。
 * @param path 文件路径（.gltf / .glb）
 * @return 解析好的 GltfModel；任一 primitive 缺必需属性/数据畸形 → 整体失败
 *         （返回 GltfLoadFailed，不带部分结果）
 */
export [[nodiscard]] std::expected<GltfModel, Error> loadGltfModel(const std::string& path);

}  // namespace RGL
