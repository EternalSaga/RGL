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
#include <ranges>                        // std::views（AGENTS 例外：namespace alias 不可 re-export）
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
 * accessor 元素流定位结果（readAccessorFloats/readIndices 共享）：base = 首元素字节地址，
 * stride = 逐元素步进（byteStride==0 → 紧密排列取 elemSize），count = 元素数，
 * elemSize = 单元素字节数。
 */
struct AccessorStream {
	const unsigned char* base;
	size_t stride;
	size_t count;
	size_t elemSize;
};

/**
 * 定位 accessor 元素流（readAccessorFloats/readIndices 共享的边界校验 + stride/required 计算）：
 * 校验 accessor/bufferView/buffer 下标与 count，按调用方判定的 elemSize 算 stride
 *（byteStride==0 → 紧密排列）与 required 并核对数据长度；类型合法性（float/uint）留调用方。
 * @param gltf 已加载的 tinygltf 模型
 * @param accIndex accessor 下标
 * @param elemSize 单元素字节数（调用方按组件类型判定；0 = 类型不合法）
 * @return 定位结果；nullopt = 任一环不满足（下标越界/elemSize 非法/count 为 0/数据不足）
 */
static std::optional<AccessorStream> locateAccessorStream(const tinygltf::Model& gltf, int accIndex,
	size_t elemSize) {
	if (accIndex < 0 || accIndex >= static_cast<int>(gltf.accessors.size())) return std::nullopt;
	const auto& acc = gltf.accessors[accIndex];
	if (elemSize == 0) return std::nullopt;
	if (acc.count == 0) return std::nullopt;
	if (acc.bufferView < 0 || acc.bufferView >= static_cast<int>(gltf.bufferViews.size())) return std::nullopt;

	const auto& bv = gltf.bufferViews[acc.bufferView];
	if (bv.buffer < 0 || bv.buffer >= static_cast<int>(gltf.buffers.size())) return std::nullopt;
	const auto& data = gltf.buffers[bv.buffer].data;

	const size_t stride = bv.byteStride ? bv.byteStride : elemSize;
	const size_t required = bv.byteOffset + acc.byteOffset + (static_cast<size_t>(acc.count) - 1) * stride + elemSize;
	if (data.size() < required) return std::nullopt;

	return AccessorStream{data.data() + bv.byteOffset + acc.byteOffset, stride, acc.count, elemSize};
}

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
	const auto stream = locateAccessorStream(gltf, accessorIndex, static_cast<size_t>(components) * sizeof(float));
	if (!stream) return false;

	out.resize(stream->count * components);
	for (size_t i = 0; i < stream->count; ++i)
		std::memcpy(out.data() + i * components, stream->base + i * stream->stride, stream->elemSize);
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
	const auto stream = locateAccessorStream(gltf, accessorIndex, elemSize);
	if (!stream) return false;

	out.resize(stream->count);
	for (size_t i = 0; i < stream->count; ++i) {
		const unsigned char* p = stream->base + i * stream->stride;
		if (stream->elemSize == 2) {
			uint16_t u;
			std::memcpy(&u, p, 2);
			out[i] = u;
		} else {
			std::memcpy(&out[i], p, 4);
		}
	}
	return true;
}

/** 三个 double（translation/scale）→ glm::vec3（逐项 static_cast<float>，语义同原实现）。 */
static glm::vec3 vec3FromDoubles(const std::vector<double>& v) {
	return glm::vec3(static_cast<float>(v[0]), static_cast<float>(v[1]), static_cast<float>(v[2]));
}

/**
 * 节点局部矩阵：node.matrix（列主序，若有 16 元素）否则 T·R·S 合成
 *（rotation 是 glTF xyzw → glm quat(w,x,y,z)；缺失分量跳过对应变换）。
 */
static glm::mat4 localTransform(const tinygltf::Node& nd) {
	if (nd.matrix.size() == 16)
		return glm::make_mat4(nd.matrix.data());
	glm::mat4 m(1.0f);
	if (nd.translation.size() == 3)
		m = glm::translate(m, vec3FromDoubles(nd.translation));
	if (nd.rotation.size() == 4)
		m = m * glm::mat4_cast(glm::quat(static_cast<float>(nd.rotation[3]),
			static_cast<float>(nd.rotation[0]), static_cast<float>(nd.rotation[1]),
			static_cast<float>(nd.rotation[2])));
	if (nd.scale.size() == 3)
		m = glm::scale(m, vec3FromDoubles(nd.scale));
	return m;
}

/** 收集 child → 首见父 node 下标映射（合法层级唯一父；越界 child 忽略；nodeParent 预置 -1）。 */
static void collectNodeParents(const tinygltf::Model& gltf, std::vector<int>& nodeParent) {
	const size_t nodeCount = gltf.nodes.size();
	for (size_t ni = 0; ni < nodeCount; ++ni)
		for (int child : gltf.nodes[ni].children)
			if (child >= 0 && child < static_cast<int>(nodeCount))
				nodeParent[child] = static_cast<int>(ni);
}

/**
 * 迭代加深单轮扫描：解析所有"父已决或为根"的节点（world[n] = world[parent] · local[n]，
 * 根 = local），resolved 自增。
 * @return 本轮是否有推进（false = 停滞）
 */
static bool resolveNodePass(const std::vector<glm::mat4>& nodeLocal, const std::vector<int>& nodeParent,
	std::vector<glm::mat4>& nodeWorld, std::vector<uint8_t>& nodeDone, size_t& resolved) {
	bool progress = false;
	for (size_t ni = 0; ni < nodeLocal.size(); ++ni) {
		if (nodeDone[ni]) continue;
		const int p = nodeParent[ni];
		if (p < 0) { nodeWorld[ni] = nodeLocal[ni]; nodeDone[ni] = 1; ++resolved; progress = true; }
		else if (nodeDone[p]) { nodeWorld[ni] = nodeWorld[p] * nodeLocal[ni]; nodeDone[ni] = 1; ++resolved; progress = true; }
	}
	return progress;
}

/** 环防御：迭代加深停滞后的未决 node（真环成员，非法 glTF）退化为 local TRS。 */
static void fixCyclicNodes(const std::vector<glm::mat4>& nodeLocal,
	std::vector<glm::mat4>& nodeWorld, std::vector<uint8_t>& nodeDone) {
	for (size_t ni = 0; ni < nodeLocal.size(); ++ni)
		if (!nodeDone[ni]) { nodeWorld[ni] = nodeLocal[ni]; nodeDone[ni] = 1; }
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
	for (size_t ni = 0; ni < nodeCount; ++ni)
		nodeLocal[ni] = localTransform(gltf.nodes[ni]);
	collectNodeParents(gltf, nodeParent);

	std::vector<glm::mat4> nodeWorld(nodeCount, glm::mat4(1.0f));
	std::vector<uint8_t> nodeDone(nodeCount, 0);
	size_t resolved = 0;
	for (size_t pass = 0; pass < nodeCount + 1; ++pass) {
		const bool progress = resolveNodePass(nodeLocal, nodeParent, nodeWorld, nodeDone, resolved);
		if (resolved == nodeCount) break;  // 全部解析完成（正常）
		if (!progress) {                    // 停滞且仍有未决 = 真环（非法 glTF）
			fixCyclicNodes(nodeLocal, nodeWorld, nodeDone);
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

/** 扩展对象数字字段读取（KHR_materials_emissive_strength 等泛型扩展，同 unlit 的
 * extensions map 查法；缺字段/非对象/非数字 = fallback）。 */
static double extNumber(const tinygltf::Value& value, const char* key, double fallback) {
	if (!value.IsObject() || !value.Has(key))
		return fallback;
	const auto& v = value.Get(key);
	if (v.IsReal())
		return v.Get<double>();
	if (v.IsInt())
		return static_cast<double>(v.Get<int>());
	return fallback;
}

/**
 * 材质字段提取（baseColorFactor + baseColorTexture + normalTexture +
 * metallicRoughnessTexture（Phase 8 ③a：ORM 贴图 R=Occlusion/G=Roughness/B=Metalness）+
 * alphaMode（BLEND/MASK/cutoff）+ doubleSided + metallic/roughness/unlit 风格字段 +
 * emissiveStrength 预乘（Phase 8 A）+ 材质下标——批次 2 去重键）。
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
	const auto& ef = mat.emissiveFactor;  // Phase 8 ③：emissiveFactor（缺省 [0,0,0] = 不发光）
	if (ef.size() == 3)
		out.emissive = glm::vec3(
			static_cast<float>(ef[0]), static_cast<float>(ef[1]), static_cast<float>(ef[2]));
	// Phase 8 A：KHR_materials_emissive_strength（HDR 发光强度，spec 值域 ≥1.0）CPU 端
	// 预乘进 factor——ObjectDraw/scene.slang/两路 frag 零改动即得 HDR 亮度
	if (const auto es = mat.extensions.find("KHR_materials_emissive_strength");
		es != mat.extensions.end())
		out.emissive *= static_cast<float>(extNumber(es->second, "emissiveStrength", 1.0));
	const auto& et = mat.emissiveTexture;
	if (et.index >= 0 && et.index < static_cast<int>(gltf.images.size()))
		out.emissiveTextureIndex = et.index;
	out.alphaModeBlend = (mat.alphaMode == "BLEND");
	out.alphaModeMask = (mat.alphaMode == "MASK");  // Phase 8 C1：alpha-test cutout
	out.alphaCutoff = static_cast<float>(mat.alphaCutoff);
	out.doubleSided = mat.doubleSided;
	out.metallic = static_cast<float>(mat.pbrMetallicRoughness.metallicFactor);
	out.roughness = static_cast<float>(mat.pbrMetallicRoughness.roughnessFactor);
	out.unlit = mat.extensions.count("KHR_materials_unlit") > 0;
}

/**
 * 读取并校验 POSITION/NORMAL（必需）+ TEXCOORD_0（可选，D9：缺失 → uv 填 0 走白纹理，
 * 存在但数据畸形 → 整体失败）；三数组顶点数须一致（不一致 = 契约违例）。
 * @return 错误描述（空 = 成功；pos/nrm/uv/hasUv 仅成功时有意义）
 */
static std::string readVertexAttributes(const tinygltf::Model& gltf, int meshIndex,
	const tinygltf::Primitive& prim, std::vector<float>& pos, std::vector<float>& nrm,
	std::vector<float>& uv, std::vector<float>& tan, bool& hasUv, bool& hasTangent) {
	auto accessorOf = [&](const char* name) -> int {
		auto it = prim.attributes.find(name);
		return it == prim.attributes.end() ? -1 : it->second;
	};
	if (!readAccessorFloats(gltf, accessorOf("POSITION"), 3, pos))
		return fmt::format("missing or malformed POSITION attribute (mesh {})", meshIndex);
	if (!readAccessorFloats(gltf, accessorOf("NORMAL"), 3, nrm))
		return fmt::format("missing or malformed NORMAL attribute (mesh {})", meshIndex);
	// TEXCOORD_0 在 glTF 中可选（D9）：缺失 → uv 填 0 + 该 primitive 走白纹理；
	// 存在但数据畸形 → 仍整体失败（契约违例）
	const int uvAccessor = accessorOf("TEXCOORD_0");
	hasUv = uvAccessor >= 0 && readAccessorFloats(gltf, uvAccessor, 2, uv);
	if (uvAccessor >= 0 && !hasUv)
		return fmt::format("malformed TEXCOORD_0 attribute (mesh {})", meshIndex);
	// TANGENT（glTF 2.0 core，Phase 8 方案 A）：bistro 由 bistro_pack.py CPU 烘 Lengyel
	// 平滑切线；缺失（kyoto 等旧资产）→ expandPrimitive Frisvad 基兜底（normal 贴图
	// 中性时结果与切线基无关，仅带 normal 贴图的缺切线资产才有近似损失）
	const int tanAccessor = accessorOf("TANGENT");
	hasTangent = tanAccessor >= 0 && readAccessorFloats(gltf, tanAccessor, 3, tan);
	if (tanAccessor >= 0 && !hasTangent)
		return fmt::format("malformed TANGENT attribute (mesh {})", meshIndex);
	const uint32_t vertexCount = static_cast<uint32_t>(pos.size() / 3);
	if (vertexCount != nrm.size() / 3 || (hasUv && vertexCount != uv.size() / 2) ||
		(hasTangent && vertexCount != tan.size() / 3))
		return fmt::format("attribute vertex counts differ (mesh {})", meshIndex);
	if (!hasUv)
		uv.assign(static_cast<size_t>(vertexCount) * 2, 0.0f);
	return {};
}

/**
 * 解析 primitive 索引：有 indices accessor → 读取并逐元素校验 < vertexCount；无 → 顺序展开
 *（0..vertexCount-1）。
 * @return 错误描述（空 = 成功；indices 仅成功时有意义）
 */
static std::string resolvePrimitiveIndices(const tinygltf::Model& gltf, int meshIndex,
	const tinygltf::Primitive& prim, uint32_t vertexCount, std::vector<uint32_t>& indices) {
	if (prim.indices < 0) {
		indices.resize(vertexCount);
		for (uint32_t i = 0; i < vertexCount; ++i) indices[i] = i;
		return {};
	}
	if (!readIndices(gltf, prim.indices, indices))
		return fmt::format("malformed index data (mesh {})", meshIndex);
	for (uint32_t idx : indices)
		if (idx >= vertexCount)
			return fmt::format("index {} out of range (mesh {})", idx, meshIndex);
	return {};
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
/**
 * Frisvad 2012 解析正交基切线（无 TANGENT 属性资产的兜底；与 bistro_pack.py 的
 * degenerate 分支同式）：返回单位 tangent ⟂ 单位 n，奇点 (0,0,-1) 显式处理。
 */
static glm::vec3 frisvadTangent(const glm::vec3& n) {
	if (n.z < -0.9999999f)
		return glm::vec3(0.0f, -1.0f, 0.0f);
	const float a = 1.0f / (1.0f - n.z);
	const float b = -n.x * n.y * a;
	return glm::normalize(glm::vec3(1.0f - n.x * n.x * a, b, -n.x));
}

static std::string expandPrimitive(const tinygltf::Model& gltf, int meshIndex,
	const tinygltf::Primitive& prim, GltfPrimitive& out) {
	std::vector<float> pos, nrm, uv, tan;
	bool hasTangent = false;
	if (const std::string attrErr = readVertexAttributes(gltf, meshIndex, prim, pos, nrm, uv,
		tan, out.hasUv, hasTangent); !attrErr.empty())
		return attrErr;
	const uint32_t vertexCount = static_cast<uint32_t>(pos.size() / 3);
	std::vector<uint32_t> indices;
	if (const std::string idxErr = resolvePrimitiveIndices(gltf, meshIndex, prim, vertexCount, indices); !idxErr.empty())
		return idxErr;
	out.vertices.reserve(indices.size());
	for (uint32_t idx : indices) {
		ModelVertex v;
		v.position = glm::vec3(pos[idx * 3 + 0], pos[idx * 3 + 1], pos[idx * 3 + 2]);
		v.normal = glm::vec3(nrm[idx * 3 + 0], nrm[idx * 3 + 1], nrm[idx * 3 + 2]);
		v.uv = glm::vec2(uv[idx * 2 + 0], uv[idx * 2 + 1]);
		out.vertices.push_back(v);
	}
	// Phase 8 方案 A：切线填充——xyz 用烘焙 TANGENT（缺 → Frisvad(N) 兜底），手性 w 按
	// uv Jacobian det 符号**逐三角形**推（glTF core TANGENT 不含 w，spec 约定从几何推断；
	// bistro UV 手性 92% 为负——DirectX Y-down 传统，W=+1 硬编码会把镜像 UV 面的
	// normal map 副切线翻反）。三角形 t 的去索引顶点 = t*3..t*3+2，w 整三角形同值。
	const size_t triCount = indices.size() / 3;
	for (size_t t = 0; t < triCount; ++t) {
		float w = 1.0f;
		if (hasTangent && out.hasUv) {
			const uint32_t i0 = indices[t * 3], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
			const float s1x = uv[i1 * 2] - uv[i0 * 2], s1y = uv[i1 * 2 + 1] - uv[i0 * 2 + 1];
			const float s2x = uv[i2 * 2] - uv[i0 * 2], s2y = uv[i2 * 2 + 1] - uv[i0 * 2 + 1];
			w = (s1x * s2y - s2x * s1y) < 0.0f ? -1.0f : 1.0f;
		}
		for (int k = 0; k < 3; ++k) {
			const size_t vi = t * 3 + k;
			const size_t src = indices[vi];
			glm::vec3 t3;
			if (hasTangent) {
				t3 = glm::vec3(tan[src * 3], tan[src * 3 + 1], tan[src * 3 + 2]);
				if (glm::length(t3) < 1e-6f)  // 坏 texel 防御（pack 理论不产出）
					t3 = frisvadTangent(glm::normalize(out.vertices[vi].normal));
			} else {
				t3 = frisvadTangent(glm::normalize(out.vertices[vi].normal));
			}
			out.vertices[vi].tangent = glm::vec4(t3, w);
		}
	}
	out.triangleCount = static_cast<uint32_t>(indices.size() / 3);
	return {};
}

/**
 * Phase 8 C2：doubleSided 背面副本复制——每三角形追加一份绕序翻转（b/c 交换）+
 * 法线取反的拷贝：背面剔除下"背面"由翻转前片可见、法线已预取反，光照天然正确；
 * 动态 cull 状态零改动（盲执行器 / DGC 捕获 / cull 候选路径全不知情），
 * 与 scene.slang frontFace 翻向互补冗余（剔除开时后者恒 false）。
 * 代价 = doubleSided primitive 顶点数据 ×2（叶片量级，可忽略）。
 */
static void duplicateForDoubleSided(GltfPrimitive& prim) {
	const size_t tris = prim.vertices.size() / 3;
	if (tris == 0)
		return;
	prim.vertices.reserve(prim.vertices.size() + tris * 3);
	for (size_t t = 0; t < tris; ++t) {
		const ModelVertex a = prim.vertices[t * 3 + 0];
		const ModelVertex b = prim.vertices[t * 3 + 1];
		const ModelVertex c = prim.vertices[t * 3 + 2];
		// tangent：xyz 保持（切线方向与正/背面无关），手性 w 翻转（绕序反 = 镜像 UV 面，
		// 副切线 B = cross(N,T)·w 须随翻转后的面重新自洽）
		prim.vertices.push_back({a.position, -a.normal, a.uv, glm::vec4(a.tangent.x, a.tangent.y, a.tangent.z, -a.tangent.w)});
		prim.vertices.push_back({c.position, -c.normal, c.uv, glm::vec4(c.tangent.x, c.tangent.y, c.tangent.z, -c.tangent.w)});
		prim.vertices.push_back({b.position, -b.normal, b.uv, glm::vec4(b.tangent.x, b.tangent.y, b.tangent.z, -b.tangent.w)});
	}
	prim.triangleCount += static_cast<uint32_t>(tris);
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
 * 包围球（保守界）：center = 点集质心（按迭代序逐点累加再除 count），radius = 质心到最远点
 * 距离（同序逐点重算距离取 max）。浮点累加序 = range 迭代序，调用方须保证与旧实现顺序一致
 *（浮点加法不满足结合律）。空点集 → {0,0}（与 BoundingSphere 默认等价）。
 * @param pts glm::vec3 点范围（可多趟迭代）
 */
template <typename Range>
static BoundingSphere enclosingSphere(Range&& pts) {
	BoundingSphere sphere;
	uint32_t count = 0;
	for (const glm::vec3& p : pts) { sphere.center += p; ++count; }
	if (count == 0) return sphere;
	sphere.center /= static_cast<float>(count);
	for (const glm::vec3& p : pts)
		sphere.radius = std::max(sphere.radius, glm::length(p - sphere.center));
	return sphere;
}

/**
 * 包围球计算：逐图元节点局部球（over raw vertices.position，**不含 nodeWorld**——
 * registerScene 存 CullSphere，render_list 每帧 modelMatrix 变换后测视锥）+ 全模型世界
 * 聚合球（center = 全部顶点世界坐标质心，radius = 质心到最远顶点距离，保守界）。
 * 两球同一 enclosingSphere 算法：世界版用 views::transform（nodeWorld 变换）+ views::join
 * 组点视图，迭代序 = primitive 序 → 顶点序，与原嵌套循环逐点一致。
 * @param model 输出模型（填 per-primitive bounds + 整体 bounds）
 */
static void computeBounds(GltfModel& model) {
	for (auto& p : model.primitives) {
		if (p.vertices.empty())
			continue;
		p.bounds = enclosingSphere(p.vertices | std::views::transform(
			[](const ModelVertex& v) { return v.position; }));
	}
	auto worldPoints = model.primitives
		| std::views::transform([](const GltfPrimitive& p) {
			return p.vertices | std::views::transform([&p](const ModelVertex& v) {
				return glm::vec3(p.nodeWorld * glm::vec4(v.position, 1.0f));
			});
		})
		| std::views::join;
	model.bounds = enclosingSphere(worldPoints);  // 空点集 → {0,0}（= 默认，与原 totalVertices>0 守卫等价）
}

/**
 * 展开单个 glTF primitive 为一个 GltfPrimitive：nodeWorld 取首个所属 node（同 mesh 多 node
 * 的实例化暂取首个，无 node → identity + warn），再填材质字段 + 去索引几何展开。
 * @return 错误描述（空 = 成功）
 */
static std::string expandMeshPrimitive(const tinygltf::Model& gltf, size_t meshIndex,
	const tinygltf::Primitive& prim, int nodeIndex, const std::vector<glm::mat4>& nodeWorlds,
	GltfPrimitive& out) {
	// 所属 node 世界矩阵（取首个引用该 mesh 的 node；同 mesh 多 node 的实例化
	// 暂取首个，无 node → identity，见 struct 注释）
	if (nodeIndex >= 0) {
		out.nodeWorld = nodeWorlds[nodeIndex];
	} else if (auto* logger = RLLogger::getInstance()) {
		logger->warn("glTF: mesh {} has no owning node, nodeWorld defaults to identity", meshIndex);
	}
	fillMaterialFields(gltf, prim.material, out);
	return expandPrimitive(gltf, static_cast<int>(meshIndex), prim, out);
}

/** 加载成功后的统计日志：逐 primitive 汇总纹理/法线/ORM 计数 + 总顶点数 + 一行 info。 */
static void logLoadSummary(RLLogger* logger, const std::string& path, const GltfModel& model) {
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
			const std::string primErr = expandMeshPrimitive(gltf, mi, prim, meshToNode[mi], nodeWorlds, out);
			if (!primErr.empty())
				return std::unexpected(Error{ErrorCode::GltfLoadFailed, fmt::format("glTF: {}", primErr)});
			if (out.doubleSided)  // Phase 8 C2：双面 = 背面副本（见 duplicateForDoubleSided）
				duplicateForDoubleSided(out);
			model.primitives.push_back(std::move(out));
		}
	}
	collectImages(gltf, model);
	computeBounds(model);

	if (logger) logLoadSummary(logger, path, model);
	return model;
}

}  // namespace RGL
