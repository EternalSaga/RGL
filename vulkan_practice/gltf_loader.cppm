module;
// 不解码纹理（Phase 2 flat unlit，无纹理）：关掉 stb_image 引入
#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>
#include <glm/glm.hpp>
export module gltf_loader;

import rlstd;
import rllogger;
import vkerror;
import model_types_generated;

namespace RGL {

// 去索引展开后的 primitive：vertices 按 ModelVertex packed 布局（32B，
// model_schema.lua 的 vertexLayouts 是契约 source of truth），
// triangleCount = vertices.size() / 3。
export struct GltfPrimitive {
	std::vector<ModelVertex> vertices;
	uint32_t triangleCount = 0;
	glm::vec3 translation{0.0f};  // 所属 node 的 translation（Phase 2 无层级嵌套、无旋转/缩放）
	glm::vec4 baseColor{1.0f};    // material 的 baseColorFactor（无纹理）
	bool alphaModeBlend = false;   // material alphaMode == "BLEND"（Phase 5 透明用，预留）
};

export struct BoundingSphere {
	glm::vec3 center{0.0f};
	float radius{0.0f};
};

export struct GltfModel {
	std::vector<GltfPrimitive> primitives;
	BoundingSphere bounds;  // 自适应空间范围用（ecs.cppm 的 renderSystem）
};

// 读 accessor 的 float 数据（契约要求 float32；byteStride != 0 时按元素步进读取，
// 0 视为紧密排列）。任何一环（类型/组件数/越界）不满足返回 false。
static bool readAccessorFloats(const tinygltf::Model& gltf, int accessorIndex, uint32_t components,
	std::vector<float>& out) {
	if (accessorIndex < 0 || accessorIndex >= static_cast<int>(gltf.accessors.size())) return false;
	const auto& acc = gltf.accessors[accessorIndex];
	if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT) return false;
	if (components == 2 && acc.type != TINYGLTF_TYPE_VEC2) return false;
	if (components == 3 && acc.type != TINYGLTF_TYPE_VEC3) return false;
	if (acc.count == 0) return false;
	if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(gltf.bufferViews.size())) return false;

	const auto& bv = gltf.bufferViews[acc.bufferView];
	if (bv.buffer < 0 || bv.buffer >= static_cast<int>(gltf.buffers.size())) return false;
	const auto& data = gltf.buffers[bv.buffer].data;

	const size_t elemSize = static_cast<size_t>(components) * sizeof(float);
	const size_t stride = bv.byteStride ? bv.byteStride : elemSize;
	const size_t required = bv.byteOffset + acc.byteOffset + (static_cast<size_t>(acc.count) - 1) * stride + elemSize;
	if (data.size() < required) return false;

	const unsigned char* base = data.data() + bv.byteOffset + acc.byteOffset;
	out.resize(static_cast<size_t>(acc.count) * components);
	for (size_t i = 0; i < acc.count; ++i)
		std::memcpy(out.data() + i * components, base + i * stride, elemSize);
	return true;
}

// 读索引 accessor（uint16/uint32 → 统一 uint32；byteStride 语义同上）
static bool readIndices(const tinygltf::Model& gltf, int accessorIndex, std::vector<uint32_t>& out) {
	if (accessorIndex < 0 || accessorIndex >= static_cast<int>(gltf.accessors.size())) return false;
	const auto& acc = gltf.accessors[accessorIndex];
	size_t elemSize = 0;
	if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
		elemSize = 2;
	} else if (acc.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
		elemSize = 4;
	} else {
		return false;
	}
	if (acc.count == 0) return false;
	if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(gltf.bufferViews.size())) return false;

	const auto& bv = gltf.bufferViews[acc.bufferView];
	if (bv.buffer < 0 || bv.buffer >= static_cast<int>(gltf.buffers.size())) return false;
	const auto& data = gltf.buffers[bv.buffer].data;

	const size_t stride = bv.byteStride ? bv.byteStride : elemSize;
	const size_t required = bv.byteOffset + acc.byteOffset + (static_cast<size_t>(acc.count) - 1) * stride + elemSize;
	if (data.size() < required) return false;

	const unsigned char* base = data.data() + bv.byteOffset + acc.byteOffset;
	out.resize(acc.count);
	for (size_t i = 0; i < acc.count; ++i) {
		const unsigned char* p = base + i * stride;
		if (elemSize == 2) {
			uint16_t u;
			std::memcpy(&u, p, 2);
			out[i] = u;
		} else {
			std::memcpy(&out[i], p, 4);
		}
	}
	return true;
}

// 加载 glTF / glTF-Binary：每个 mesh 的每个 primitive 展开为一个 GltfPrimitive
//（POSITION/NORMAL/TEXCOORD_0 齐全 + 去索引展开）。
// 任一 primitive 缺属性/数据畸形 → 整体失败（返回错误，不带部分结果）。
export [[nodiscard]] std::expected<GltfModel, Error> loadGltfModel(const std::string& path) {
	auto* logger = RLLogger::getInstance();

	tinygltf::Model gltf;
	std::string err, warn;
	tinygltf::TinyGLTF gltfLoader;  // v3 的类名（v2 叫 Loader）
	if (!gltfLoader.LoadBinaryFromFile(&gltf, &err, &warn, path)) {
		return std::unexpected(Error{ErrorCode::GltfLoadFailed,
			"glTF load failed (" + path + "): " + err});
	}
	if (!warn.empty() && logger) logger->warn("glTF ({})", warn);
	if (gltf.meshes.empty()) {
		return std::unexpected(Error{ErrorCode::GltfLoadFailed, "glTF has no meshes: " + path});
	}

	GltfModel model;
	for (size_t mi = 0; mi < gltf.meshes.size(); ++mi) {
		const auto& mesh = gltf.meshes[mi];
		for (const auto& prim : mesh.primitives) {
			GltfPrimitive out;

			// 所属 node 的 translation（Phase 2：无层级嵌套；rotation/scale 忽略）
			bool foundNode = false;
			for (const auto& node : gltf.nodes) {
				if (node.mesh == static_cast<int>(mi)) {
					if (node.translation.size() == 3)
						out.translation = glm::vec3(
							static_cast<float>(node.translation[0]),
							static_cast<float>(node.translation[1]),
							static_cast<float>(node.translation[2]));
					foundNode = true;
					break;
				}
			}
			if (!foundNode && logger)
				logger->warn("glTF: mesh {} has no owning node, translation defaults to 0", mi);

			// material（baseColorFactor + alphaMode；Phase 2 无纹理）
			if (prim.material >= 0 && prim.material < static_cast<int>(gltf.materials.size())) {
				const auto& mat = gltf.materials[prim.material];
				const auto& f = mat.pbrMetallicRoughness.baseColorFactor;
				if (f.size() == 4)
					out.baseColor = glm::vec4(
						static_cast<float>(f[0]), static_cast<float>(f[1]),
						static_cast<float>(f[2]), static_cast<float>(f[3]));
				out.alphaModeBlend = (mat.alphaMode == "BLEND");
			}

			auto accessorOf = [&](const char* name) -> int {
				auto it = prim.attributes.find(name);
				return it == prim.attributes.end() ? -1 : it->second;
			};

			std::vector<float> pos, nrm, uv;
			if (!readAccessorFloats(gltf, accessorOf("POSITION"), 3, pos))
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					"glTF: missing or malformed POSITION attribute (mesh " + std::to_string(mi) + ")"});
			if (!readAccessorFloats(gltf, accessorOf("NORMAL"), 3, nrm))
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					"glTF: missing or malformed NORMAL attribute (mesh " + std::to_string(mi) + ")"});
			if (!readAccessorFloats(gltf, accessorOf("TEXCOORD_0"), 2, uv))
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					"glTF: missing or malformed TEXCOORD_0 attribute (mesh " + std::to_string(mi) + ")"});

			const uint32_t vertexCount = static_cast<uint32_t>(pos.size() / 3);
			if (vertexCount != nrm.size() / 3 || vertexCount != uv.size() / 2)
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					"glTF: attribute vertex counts differ (mesh " + std::to_string(mi) + ")"});

			// 去索引展开：三角形 i 的 3 个连续顶点 = i*3..i*3+2
			std::vector<uint32_t> indices;
			if (prim.indices >= 0) {
				if (!readIndices(gltf, prim.indices, indices))
					return std::unexpected(Error{ErrorCode::GltfLoadFailed,
						"glTF: malformed index data (mesh " + std::to_string(mi) + ")"});
				for (uint32_t idx : indices) {
					if (idx >= vertexCount)
						return std::unexpected(Error{ErrorCode::GltfLoadFailed,
							"glTF: index " + std::to_string(idx) + " out of range (mesh " +
								std::to_string(mi) + ")"});
				}
			} else {
				indices.resize(vertexCount);
				for (uint32_t i = 0; i < vertexCount; ++i) indices[i] = i;
			}

			out.vertices.reserve(indices.size());
			for (uint32_t idx : indices) {
				ModelVertex v;
				v.position = glm::vec3(pos[idx * 3 + 0], pos[idx * 3 + 1], pos[idx * 3 + 2]);
				v.normal = glm::vec3(nrm[idx * 3 + 0], nrm[idx * 3 + 1], nrm[idx * 3 + 2]);
				v.uv = glm::vec2(uv[idx * 3 + 0], uv[idx * 3 + 1]);
				out.vertices.push_back(v);
			}
			out.triangleCount = static_cast<uint32_t>(indices.size() / 3);

			model.primitives.push_back(std::move(out));
		}
	}

	// 包围球（自适应空间范围）：center = 全部顶点世界坐标（含 translation）的质心，
	// radius = 质心到最远顶点的距离（保守界，任意方向都包住）
	uint32_t totalVertices = 0;
	glm::vec3 center{0.0f};
	for (const auto& p : model.primitives) {
		for (const auto& v : p.vertices) center += v.position + p.translation;
		totalVertices += static_cast<uint32_t>(p.vertices.size());
	}
	if (totalVertices > 0) {
		center /= static_cast<float>(totalVertices);
		float radius = 0.0f;
		for (const auto& p : model.primitives)
			for (const auto& v : p.vertices)
				radius = std::max(radius, glm::length(v.position + p.translation - center));
		model.bounds = {center, radius};
	}

	if (logger) {
		logger->info("glTF loaded ({}): {} primitives, {} vertices (de-indexed); "
			"bounds center=({}, {}, {}) radius={:.2f}",
			path, model.primitives.size(), totalVertices,
			model.bounds.center.x, model.bounds.center.y, model.bounds.center.z,
			model.bounds.radius);
	}
	return model;
}

}  // namespace RGL
