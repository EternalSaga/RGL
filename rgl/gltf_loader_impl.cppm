module;
// stb_image 解码启用（Phase 3 纹理）：实现体在 stb_impl.cpp（STB_IMAGE_IMPLEMENTATION）
// NO_STB_IMAGE_WRITE 保留（tinygltf_impl.cpp 同定义，两 TU API 面一致）
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>
#include <fmt/format.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // node TRS 合成：translate/scale
#include <glm/gtc/quaternion.hpp>        // node.rotation（xyzw）→ mat4_cast
#include <glm/gtc/type_ptr.hpp>          // node.matrix（列主序）→ make_mat4
module gltf_loader:impl;

import gltf_loader;  // clang 不给 partition impl unit 隐式 import 主接口——必须显式
import rlstd;
import rllogger;
import vkerror;
import model_types_generated;

namespace RGL {

// readAccessorFloats/readIndices（内部 helper）+ loadGltfModel 定义（2026-09-09
// 接口瘦身）：tinygltf 解析实现移出接口 TU；本单元是叶子边。

/**
 * 读 accessor 的 float 数据（契约要求 float32；byteStride != 0 时按元素步进读取，
 * 0 视为紧密排列）。
 * @param gltf 已加载的 tinygltf 模型
 * @param accessorIndex accessor 下标
 * @param components 期望的每元素分量数（2 = VEC2，3 = VEC3）
 * @param out 输出：count × components 个 float
 * @return false = 任何一环不满足（下标越界/组件类型非 float/分量数不符/count 为 0/byteView 越界）
 */
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

/**
 * 读索引 accessor（uint16/uint32 → 统一 uint32；byteStride 语义同 readAccessorFloats）。
 * @param gltf 已加载的 tinygltf 模型
 * @param accessorIndex 索引 accessor 下标
 * @param out 输出：count 个 uint32 顶点下标
 * @return false = 下标越界/组件类型非 uint16/uint32/count 为 0/bufferView 越界
 */
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

/**
 * 加载 glTF / glTF-Binary（实现体，语义/契约见接口声明）：每个 mesh 的每个
 * primitive 展开为一个 GltfPrimitive；节点世界矩阵经层级 TRS + 父链合成
 *（迭代加深求解 + 环防御）。
 */
std::expected<GltfModel, Error> loadGltfModel(const std::string& path) {
	auto* logger = RLLogger::getInstance();

	tinygltf::Model gltf;
	std::string err, warn;
	tinygltf::TinyGLTF gltfLoader;  // v3 的类名（v2 叫 Loader）
	if (!gltfLoader.LoadBinaryFromFile(&gltf, &err, &warn, path)) {
		return std::unexpected(Error{ErrorCode::GltfLoadFailed,
			fmt::format("glTF load failed ({}): {}", path, err)});
	}
	if (!warn.empty() && logger) logger->warn("glTF ({})", warn);
	if (gltf.meshes.empty()) {
		return std::unexpected(Error{ErrorCode::GltfLoadFailed, fmt::format("glTF has no meshes: {}", path)});
	}

	GltfModel model;

	// 节点世界矩阵（Phase 7：层级 TRS + 父链合成，替换旧"只取首 node 的 translation"）。
	// 根 = 无父节点（覆盖 scene.nodes 与孤立根）；world[n] = world[parent(n)] · localTRS(n)。
	// localTRS：node.matrix（列主序，若有）否则 T·R·S（rotation 是 glTF xyzw → glm quat(w,x,y,z)）。
	// 合法 glTF 层级无环；迭代加深求解并带环防御（残留 = 环，退化 local + warn）。
	const size_t nodeCount = gltf.nodes.size();
	std::vector<glm::mat4> nodeLocal(nodeCount, glm::mat4(1.0f));
	std::vector<int> nodeParent(nodeCount, -1);
	for (size_t ni = 0; ni < nodeCount; ++ni) {
		const auto& nd = gltf.nodes[ni];
		if (nd.matrix.size() == 16) {
			nodeLocal[ni] = glm::make_mat4(nd.matrix.data());
		} else {
			glm::mat4 m(1.0f);
			if (nd.translation.size() == 3)
				m = glm::translate(m, glm::vec3(static_cast<float>(nd.translation[0]),
					static_cast<float>(nd.translation[1]), static_cast<float>(nd.translation[2])));
			if (nd.rotation.size() == 4)
				m = m * glm::mat4_cast(glm::quat(static_cast<float>(nd.rotation[3]),
					static_cast<float>(nd.rotation[0]), static_cast<float>(nd.rotation[1]),
					static_cast<float>(nd.rotation[2])));
			if (nd.scale.size() == 3)
				m = glm::scale(m, glm::vec3(static_cast<float>(nd.scale[0]),
					static_cast<float>(nd.scale[1]), static_cast<float>(nd.scale[2])));
			nodeLocal[ni] = m;
		}
		for (int child : nd.children)
			if (child >= 0 && child < static_cast<int>(nodeCount))
				nodeParent[child] = static_cast<int>(ni);
	}
	std::vector<glm::mat4> nodeWorld(nodeCount, glm::mat4(1.0f));
	std::vector<uint8_t> nodeDone(nodeCount, 0);
	size_t resolved = 0;
	for (size_t pass = 0; pass < nodeCount + 1; ++pass) {
		bool progress = false;
		for (size_t ni = 0; ni < nodeCount; ++ni) {
			if (nodeDone[ni]) continue;
			const int p = nodeParent[ni];
			if (p < 0) { nodeWorld[ni] = nodeLocal[ni]; nodeDone[ni] = 1; ++resolved; progress = true; }
			else if (nodeDone[p]) { nodeWorld[ni] = nodeWorld[p] * nodeLocal[ni]; nodeDone[ni] = 1; ++resolved; progress = true; }
		}
		if (resolved == nodeCount) break;  // 全部解析完成（正常）
		if (!progress) {                    // 停滞且仍有未决 = 真环（非法 glTF）
			for (size_t ni = 0; ni < nodeCount; ++ni)
				if (!nodeDone[ni]) { nodeWorld[ni] = nodeLocal[ni]; nodeDone[ni] = 1; }
			if (logger) logger->warn("glTF: node hierarchy has a cycle (illegal) — affected nodes use local TRS only");
			break;
		}
	}

	for (size_t mi = 0; mi < gltf.meshes.size(); ++mi) {
		const auto& mesh = gltf.meshes[mi];
		for (const auto& prim : mesh.primitives) {
			GltfPrimitive out;

			// 所属 node 世界矩阵（取首个引用该 mesh 的 node；同 mesh 多 node 的实例化暂取首个，
			// 无 node → identity，见 struct 注释）
			bool foundNode = false;
			for (int ni = 0; ni < static_cast<int>(nodeCount); ++ni) {
				if (gltf.nodes[ni].mesh == static_cast<int>(mi)) {
					out.nodeWorld = nodeWorld[ni];
					foundNode = true;
					break;
				}
			}
			if (!foundNode && logger)
				logger->warn("glTF: mesh {} has no owning node, nodeWorld defaults to identity", mi);

			// material（baseColorFactor + baseColorTexture + alphaMode + 材质下标——批次 2 去重键；
			// Phase 4 批次 3：metallic/roughness/unlit 风格字段）
			if (prim.material >= 0 && prim.material < static_cast<int>(gltf.materials.size())) {
				const auto& mat = gltf.materials[prim.material];
				out.materialIndex = prim.material;
				const auto& f = mat.pbrMetallicRoughness.baseColorFactor;
				if (f.size() == 4)
					out.baseColor = glm::vec4(
						static_cast<float>(f[0]), static_cast<float>(f[1]),
						static_cast<float>(f[2]), static_cast<float>(f[3]));
				const auto& tc = mat.pbrMetallicRoughness.baseColorTexture;
				if (tc.index >= 0 && tc.index < static_cast<int>(gltf.images.size()))
					out.baseColorTextureIndex = tc.index;
				out.alphaModeBlend = (mat.alphaMode == "BLEND");
				out.metallic = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
				out.roughness = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
				out.unlit = mat.extensions.count("KHR_materials_unlit") > 0;
			}

			auto accessorOf = [&](const char* name) -> int {
				auto it = prim.attributes.find(name);
				return it == prim.attributes.end() ? -1 : it->second;
			};

			std::vector<float> pos, nrm, uv;
			if (!readAccessorFloats(gltf, accessorOf("POSITION"), 3, pos))
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					fmt::format("glTF: missing or malformed POSITION attribute (mesh {})", mi)});
			if (!readAccessorFloats(gltf, accessorOf("NORMAL"), 3, nrm))
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					fmt::format("glTF: missing or malformed NORMAL attribute (mesh {})", mi)});

			// TEXCOORD_0 在 glTF 中可选（D9）：缺失 → uv 填 0 + 该 primitive 走白纹理；
			// 存在但数据畸形 → 仍整体失败（契约违例）
			const int uvAccessor = accessorOf("TEXCOORD_0");
			out.hasUv = uvAccessor >= 0 && readAccessorFloats(gltf, uvAccessor, 2, uv);
			if (uvAccessor >= 0 && !out.hasUv)
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					fmt::format("glTF: malformed TEXCOORD_0 attribute (mesh {})", mi)});

			const uint32_t vertexCount = static_cast<uint32_t>(pos.size() / 3);
			if (vertexCount != nrm.size() / 3 || (out.hasUv && vertexCount != uv.size() / 2))
				return std::unexpected(Error{ErrorCode::GltfLoadFailed,
					fmt::format("glTF: attribute vertex counts differ (mesh {})", mi)});
			if (!out.hasUv)
				uv.assign(static_cast<size_t>(vertexCount) * 2, 0.0f);

			// 去索引展开：三角形 i 的 3 个连续顶点 = i*3..i*3+2
			std::vector<uint32_t> indices;
			if (prim.indices >= 0) {
				if (!readIndices(gltf, prim.indices, indices))
					return std::unexpected(Error{ErrorCode::GltfLoadFailed,
						fmt::format("glTF: malformed index data (mesh {})", mi)});
				for (uint32_t idx : indices) {
					if (idx >= vertexCount)
						return std::unexpected(Error{ErrorCode::GltfLoadFailed,
							fmt::format("glTF: index {} out of range (mesh {})", idx, mi)});
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
				v.uv = glm::vec2(uv[idx * 2 + 0], uv[idx * 2 + 1]);
				out.vertices.push_back(v);
			}
			out.triangleCount = static_cast<uint32_t>(indices.size() / 3);

			model.primitives.push_back(std::move(out));
		}
	}

	// 纹理源数据（Phase 3）：buffer 0 原始数据 move 出（零拷贝，几何已拷入 vertices），
	// image span 以此为宿主；仅支持单 buffer（glb / 单外部 buffer），多 buffer .gltf 的
	// image 留空 span（不支持，调用方按无纹理处理）
	if (!gltf.buffers.empty())
		model.bufferData = std::move(gltf.buffers[0].data);
	for (size_t ii = 0; ii < gltf.images.size(); ++ii) {
		GltfImage img;
		img.mimeType = gltf.images[ii].mimeType;
		const int bvIndex = gltf.images[ii].bufferView;
		if (bvIndex >= 0 && bvIndex < static_cast<int>(gltf.bufferViews.size()) &&
			gltf.bufferViews[bvIndex].buffer == 0 && !model.bufferData.empty()) {
			const auto& bv = gltf.bufferViews[bvIndex];
			const size_t offset = bv.byteOffset;
			if (offset + bv.byteLength <= model.bufferData.size())
				img.data = std::span<const uint8_t>(model.bufferData.data() + offset, bv.byteLength);
		}
		model.images.push_back(std::move(img));
	}

	// 逐图元节点局部包围球（over raw vertices.position，**不含 nodeWorld**——registerScene 存
	// CullSphere，render_list 每帧 modelMatrix（= spin·root·nodeWorld）变换后测视锥）。
	for (auto& p : model.primitives) {
		if (p.vertices.empty())
			continue;
		glm::vec3 pc{0.0f};
		for (const auto& v : p.vertices)
			pc += v.position;
		pc /= static_cast<float>(p.vertices.size());
		float pr = 0.0f;
		for (const auto& v : p.vertices)
			pr = std::max(pr, glm::length(v.position - pc));
		p.bounds = {pc, pr};
	}

	// 包围球（自适应空间范围）：center = 全部顶点世界坐标（经 nodeWorld 变换）的质心，
	// radius = 质心到最远顶点的距离（保守界，任意方向都包住）
	uint32_t totalVertices = 0;
	glm::vec3 center{0.0f};
	for (const auto& p : model.primitives) {
		for (const auto& v : p.vertices)
			center += glm::vec3(p.nodeWorld * glm::vec4(v.position, 1.0f));
		totalVertices += static_cast<uint32_t>(p.vertices.size());
	}
	if (totalVertices > 0) {
		center /= static_cast<float>(totalVertices);
		float radius = 0.0f;
		for (const auto& p : model.primitives)
			for (const auto& v : p.vertices)
				radius = std::max(radius, glm::length(
					glm::vec3(p.nodeWorld * glm::vec4(v.position, 1.0f)) - center));
		model.bounds = {center, radius};
	}

	if (logger) {
		uint32_t texturedPrims = 0;
		for (const auto& p : model.primitives)
			if (p.baseColorTextureIndex >= 0) texturedPrims++;
		logger->info("glTF loaded ({}): {} primitives ({} textured), {} images, {} vertices (de-indexed); "
			"bounds center=({}, {}, {}) radius={:.2f}",
			path, model.primitives.size(), texturedPrims, model.images.size(), totalVertices,
			model.bounds.center.x, model.bounds.center.y, model.bounds.center.z,
			model.bounds.radius);
	}
	return model;
}

}  // namespace RGL
