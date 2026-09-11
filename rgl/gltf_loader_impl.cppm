module;
// stb 全面断连（2026-09-11，图像编解码迁 image_codec 模块）：NO_STB_IMAGE +
// NO_STB_IMAGE_WRITE（tinygltf_impl.cpp 同定义，两 TU API 面一致）——tinygltf 不解码
// 像素，image 的 bufferView 原始字节由 texture_pool 走 image_codec 解码（SetImageLoader
// 注册 no-op 回调满足"有 bufferView 的 image 须有 loader"契约）
#define TINYGLTF_NO_STB_IMAGE
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
 * 节点世界矩阵求解（Phase 7：层级 TRS + 父链合成，替换旧"只取首 node 的 translation"）：
 * 根 = 无父节点；world[n] = world[parent(n)] · localTRS(n)。localTRS = node.matrix
 *（列主序，若有）否则 T·R·S（rotation 是 glTF xyzw → glm quat(w,x,y,z)）。合法 glTF
 * 层级无环；迭代加深求解并带环防御（残留 = 环 → 受影响 node 退化 local TRS + warn）。
 * @param gltf 已加载的 tinygltf 模型
 * @return nodeCount 个世界矩阵（环受影响 node = local TRS，调用方不可区分）
 */
static std::vector<glm::mat4> resolveNodeWorlds(const tinygltf::Model& gltf) {
	auto* logger = RLLogger::getInstance();
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
	return nodeWorld;
}

/**
 * mesh → 所属 node 下标映射（取首个引用该 mesh 的 node；同 mesh 多 node 实例化暂取
 * 首个，-1 = 无 node → 调用方按 identity 处理 + warn）。
 * @param gltf 已加载的 tinygltf 模型
 * @return meshes.size() 个 node 下标（-1 = 无）
 */
static std::vector<int> buildMeshToNode(const tinygltf::Model& gltf) {
	std::vector<int> meshToNode(gltf.meshes.size(), -1);
	for (size_t ni = 0; ni < gltf.nodes.size(); ++ni) {
		const int mesh = gltf.nodes[ni].mesh;
		if (mesh >= 0 && mesh < static_cast<int>(meshToNode.size()) && meshToNode[mesh] < 0)
			meshToNode[mesh] = static_cast<int>(ni);
	}
	return meshToNode;
}

/**
 * 材质字段提取（baseColorFactor + baseColorTexture + normalTexture +
 * metallicRoughnessTexture（Phase 8 ③a：ORM 贴图 R=Occlusion/G=Roughness/B=Metalness）+
 * alphaMode + metallic/roughness/unlit 风格字段 + 材质下标——批次 2 去重键）。
 * @param gltf 已加载的 tinygltf 模型
 * @param materialIndex materials 数组下标（-1 = 无材质，只写 materialIndex）
 * @param out 输出 primitive（覆盖材质相关字段；image 下标越界保留 -1）
 */
static void fillMaterialFields(const tinygltf::Model& gltf, int materialIndex, GltfPrimitive& out) {
	if (materialIndex < 0 || materialIndex >= static_cast<int>(gltf.materials.size()))
		return;
	const auto& mat = gltf.materials[materialIndex];
	out.materialIndex = materialIndex;
	const auto& f = mat.pbrMetallicRoughness.baseColorFactor;
	if (f.size() == 4)
		out.baseColor = glm::vec4(
			static_cast<float>(f[0]), static_cast<float>(f[1]),
			static_cast<float>(f[2]), static_cast<float>(f[3]));
	const auto& tc = mat.pbrMetallicRoughness.baseColorTexture;
	if (tc.index >= 0 && tc.index < static_cast<int>(gltf.images.size()))
		out.baseColorTextureIndex = tc.index;
	const auto& nt = mat.normalTexture;
	if (nt.index >= 0 && nt.index < static_cast<int>(gltf.images.size()))
		out.normalTextureIndex = nt.index;
	const auto& mrt = mat.pbrMetallicRoughness.metallicRoughnessTexture;
	if (mrt.index >= 0 && mrt.index < static_cast<int>(gltf.images.size()))
		out.metallicRoughnessTextureIndex = mrt.index;
	out.alphaModeBlend = (mat.alphaMode == "BLEND");
	out.metallic = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
	out.roughness = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
	out.unlit = mat.extensions.count("KHR_materials_unlit") > 0;
}

/**
 * 单个 glTF primitive 去索引展开（POSITION/NORMAL 必需 + TEXCOORD_0 可选（D9））：
 * 三角形 i 的 3 个连续顶点 = i*3..i*3+2；无 indices = 顺序展开。
 * @param gltf 已加载的 tinygltf 模型
 * @param meshIndex mesh 下标（仅错误信息用）
 * @param prim 待展开的 glTF primitive
 * @param out 输出 primitive（填 vertices/triangleCount/hasUv）
 * @return 错误描述（空 = 成功；调用方整体失败）
 */
static std::string expandPrimitive(const tinygltf::Model& gltf, int meshIndex,
	const tinygltf::Primitive& prim, GltfPrimitive& out) {
	auto accessorOf = [&](const char* name) -> int {
		auto it = prim.attributes.find(name);
		return it == prim.attributes.end() ? -1 : it->second;
	};
	std::vector<float> pos, nrm, uv;
	if (!readAccessorFloats(gltf, accessorOf("POSITION"), 3, pos))
		return fmt::format("missing or malformed POSITION attribute (mesh {})", meshIndex);
	if (!readAccessorFloats(gltf, accessorOf("NORMAL"), 3, nrm))
		return fmt::format("missing or malformed NORMAL attribute (mesh {})", meshIndex);
	// TEXCOORD_0 在 glTF 中可选（D9）：缺失 → uv 填 0 + 该 primitive 走白纹理；
	// 存在但数据畸形 → 仍整体失败（契约违例）
	const int uvAccessor = accessorOf("TEXCOORD_0");
	out.hasUv = uvAccessor >= 0 && readAccessorFloats(gltf, uvAccessor, 2, uv);
	if (uvAccessor >= 0 && !out.hasUv)
		return fmt::format("malformed TEXCOORD_0 attribute (mesh {})", meshIndex);
	const uint32_t vertexCount = static_cast<uint32_t>(pos.size() / 3);
	if (vertexCount != nrm.size() / 3 || (out.hasUv && vertexCount != uv.size() / 2))
		return fmt::format("attribute vertex counts differ (mesh {})", meshIndex);
	if (!out.hasUv)
		uv.assign(static_cast<size_t>(vertexCount) * 2, 0.0f);
	std::vector<uint32_t> indices;
	if (prim.indices >= 0) {
		if (!readIndices(gltf, prim.indices, indices))
			return fmt::format("malformed index data (mesh {})", meshIndex);
		for (uint32_t idx : indices)
			if (idx >= vertexCount)
				return fmt::format("index {} out of range (mesh {})", idx, meshIndex);
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
	return {};
}

/**
 * 纹理源数据收集（Phase 3）：buffer 0 原始数据 move 出（零拷贝，几何已拷入 vertices），
 * image span 以此为宿主；仅支持单 buffer（glb / 单外部 buffer），多 buffer .gltf 的
 * image 留空 span（不支持，调用方按无纹理处理）。
 * @param gltf 已加载的 tinygltf 模型
 * @param model 输出模型（填 bufferData/images）
 */
static void collectImages(const tinygltf::Model& gltf, GltfModel& model) {
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
}

/**
 * 包围球计算：逐图元节点局部球（over raw vertices.position，**不含 nodeWorld**——
 * registerScene 存 CullSphere，render_list 每帧 modelMatrix 变换后测视锥）+ 全模型世界
 * 聚合球（center = 全部顶点世界坐标质心，radius = 质心到最远顶点距离，保守界）。
 * @param model 输出模型（填 per-primitive bounds + 整体 bounds）
 */
static void computeBounds(GltfModel& model) {
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
	// NO_STB_IMAGE 下默认 loader = nullptr：带 bufferView 的 image 会报
	// "No LoadImageData callback specified"。本项目不让 tinygltf 解像素（纹理走
	// bufferView 原始字节 → texture_pool），注册 no-op loader 满足契约（v2 的
	// resolve_image_dependencies=false 参数在 v3 已移除，此为等价替代）
	gltfLoader.SetImageLoader([](tinygltf::Image*, const int, std::string*, std::string*,
		int, int, const unsigned char*, int, void*) { return true; }, nullptr);
	if (!gltfLoader.LoadBinaryFromFile(&gltf, &err, &warn, path)) {
		return std::unexpected(Error{ErrorCode::GltfLoadFailed,
			fmt::format("glTF load failed ({}): {}", path, err)});
	}
	if (!warn.empty() && logger) logger->warn("glTF ({})", warn);
	if (gltf.meshes.empty()) {
		return std::unexpected(Error{ErrorCode::GltfLoadFailed, fmt::format("glTF has no meshes: {}", path)});
	}

	GltfModel model;
	const auto nodeWorlds = resolveNodeWorlds(gltf);
	const auto meshToNode = buildMeshToNode(gltf);
	for (size_t mi = 0; mi < gltf.meshes.size(); ++mi) {
		for (const auto& prim : gltf.meshes[mi].primitives) {
			GltfPrimitive out;
			// 所属 node 世界矩阵（取首个引用该 mesh 的 node；同 mesh 多 node 的实例化
			// 暂取首个，无 node → identity，见 struct 注释）
			const int ni = meshToNode[mi];
			if (ni >= 0) {
				out.nodeWorld = nodeWorlds[ni];
			} else if (logger) {
				logger->warn("glTF: mesh {} has no owning node, nodeWorld defaults to identity", mi);
			}
			fillMaterialFields(gltf, prim.material, out);
			const std::string primErr = expandPrimitive(gltf, static_cast<int>(mi), prim, out);
			if (!primErr.empty())
				return std::unexpected(Error{ErrorCode::GltfLoadFailed, fmt::format("glTF: {}", primErr)});
			model.primitives.push_back(std::move(out));
		}
	}
	collectImages(gltf, model);
	computeBounds(model);

	if (logger) {
		uint32_t texturedPrims = 0, normalPrims = 0, ormPrims = 0, totalVertices = 0;
		for (const auto& p : model.primitives) {
			if (p.baseColorTextureIndex >= 0) texturedPrims++;
			if (p.normalTextureIndex >= 0) normalPrims++;
			if (p.metallicRoughnessTextureIndex >= 0) ormPrims++;
			totalVertices += static_cast<uint32_t>(p.vertices.size());
		}
		logger->info("glTF loaded ({}): {} primitives ({} textured / {} normal / {} orm), {} images, "
			"{} vertices (de-indexed); bounds center=({}, {}, {}) radius={:.2f}",
			path, model.primitives.size(), texturedPrims, normalPrims, ormPrims,
			model.images.size(), totalVertices,
			model.bounds.center.x, model.bounds.center.y, model.bounds.center.z,
			model.bounds.radius);
	}
	return model;
}

}  // namespace RGL
