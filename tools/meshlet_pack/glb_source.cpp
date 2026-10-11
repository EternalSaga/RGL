// glb 读取实现。tinygltf 面 = 引擎 rgl/gltf_loader_impl.cppm 的同一条路（bufferView +
// byteStride 定位元素流），但**不 import 引擎**——§7.8 拍板 3：引擎不链 meshoptimizer、
// 工具不链引擎，两侧只共享格式契约（format.h）不共享代码，谁也不把对方的依赖面拖进来。
#include "glb_source.h"

#include <cstring>
#include <stdexcept>

#define TINYGLTF_NO_STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

namespace RGL::meshpack {

namespace {

// no-op image loader（v3 签名，rgl/gltf_loader_impl.cppm 同法）：契约 = "有 bufferView
// 的 image 须有 loader"，返回 true 但不写任何像素。默认 stb loader 会把 405 张贴图
// 全解码 ⟹ 内存爆 + 变慢。
bool noopImageLoader(tinygltf::Image*, const int, std::string*, std::string*, int, int, const unsigned char*, int, void*)
{
    return true;
}

size_t componentSize(int componentType)
{
    switch (componentType) {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
    case TINYGLTF_COMPONENT_TYPE_SHORT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
    case TINYGLTF_COMPONENT_TYPE_INT:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
    case TINYGLTF_COMPONENT_TYPE_FLOAT: return 4;
    default: return 0;
    }
}

size_t typeChannels(int type)
{
    switch (type) {
    case TINYGLTF_TYPE_SCALAR: return 1;
    case TINYGLTF_TYPE_VEC2: return 2;
    case TINYGLTF_TYPE_VEC3: return 3;
    case TINYGLTF_TYPE_VEC4: return 4;
    default: return 0;
    }
}

// accessor 元素流定位（引擎 AccessorStream 同构）：base = 首元素字节地址，
// stride = 逐元素步进（bufferView.byteStride==0 → 紧密排列）。
struct AccessorStream {
    const unsigned char* base;
    size_t stride;
    size_t count;
};

AccessorStream locateStream(const tinygltf::Model& model, const char* what, int accessorIndex)
{
    if (accessorIndex < 0 || size_t(accessorIndex) >= model.accessors.size())
        throw std::runtime_error(std::string(what) + ": accessor 下标越界");
    const tinygltf::Accessor& acc = model.accessors[size_t(accessorIndex)];
    if (acc.bufferView < 0 || size_t(acc.bufferView) >= model.bufferViews.size())
        throw std::runtime_error(std::string(what) + ": bufferView 下标越界");
    const tinygltf::BufferView& bv = model.bufferViews[size_t(acc.bufferView)];
    if (bv.buffer < 0 || size_t(bv.buffer) >= model.buffers.size())
        throw std::runtime_error(std::string(what) + ": buffer 下标越界");
    const std::vector<unsigned char>& data = model.buffers[size_t(bv.buffer)].data;
    const size_t channels = typeChannels(acc.type);
    const size_t elemSize = componentSize(acc.componentType);
    if (!channels || !elemSize)
        throw std::runtime_error(std::string(what) + ": 不支持的 accessor type/componentType");
    const size_t stride = bv.byteStride > 0 ? bv.byteStride : channels * elemSize;
    const size_t offset = bv.byteOffset + acc.byteOffset;
    if (offset + (acc.count - 1) * stride + channels * elemSize > data.size())
        throw std::runtime_error(std::string(what) + ": accessor 越出 buffer 末尾");
    return {data.data() + offset, stride, size_t(acc.count)};
}

// 读一个 f32×N accessor 进目标记录（dst 步进 = kVertexStride，字段偏移 = offset）。
// copyBytes 允许小于 accessor 元素宽度（TANGENT vec4 只取 xyz）。
// 类型闸门：位置/法线/uv/切线必须是**未归一化的 float**（bistro 实测全 f32）——
// 半精度/归一化整数在这里静默换精度 = 与 §7 对拍作废，宁可红。
void readFloatsInto(const tinygltf::Model& model, const char* what, int accessorIndex, int wantType,
                    std::vector<uint8_t>& dst, size_t fieldOffset, size_t copyBytes)
{
    const tinygltf::Accessor& acc = model.accessors[size_t(accessorIndex)];
    if (acc.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT || acc.normalized)
        throw std::runtime_error(std::string(what) + ": 需要未归一化 float accessor");
    if (acc.type != wantType)
        throw std::runtime_error(std::string(what) + ": accessor 类型不符");
    AccessorStream s = locateStream(model, what, accessorIndex);
    const size_t channels = typeChannels(acc.type);
    if (channels * sizeof(float) < copyBytes)
        throw std::runtime_error(std::string(what) + ": 通道数与目标字段不符");
    if (dst.size() / kVertexStride < s.count)
        throw std::runtime_error(std::string(what) + ": 顶点数与 POSITION 不一致");
    for (size_t v = 0; v < s.count; ++v)
        std::memcpy(dst.data() + v * kVertexStride + fieldOffset, s.base + v * s.stride, copyBytes);
}

int findAttribute(const tinygltf::Primitive& prim, const char* name)
{
    auto it = prim.attributes.find(name);
    return it == prim.attributes.end() ? -1 : it->second;
}

uint32_t readIndexScalar(const unsigned char* p, size_t es)
{
    if (es == 1) return p[0];
    if (es == 2) return uint32_t(p[0] | (p[1] << 8));
    return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24));
}

// 顶点段：POSITION 必需 + NORMAL/TEXCOORD_0/TANGENT 可选（缺失 = 零填充），
// tangent.w 统一写规范值 +1（理由见 format.h 坐标系一节；手性实测见 --stats）。
void readPrimVertices(const tinygltf::Model& model, const tinygltf::Primitive& prim, SourcePrim& out)
{
    const int posAcc = findAttribute(prim, "POSITION");
    if (posAcc < 0)
        throw std::runtime_error("primitive 缺 POSITION");
    out.vertexCount = uint32_t(model.accessors[size_t(posAcc)].count);
    out.vertices.resize(size_t(out.vertexCount) * kVertexStride, 0);

    readFloatsInto(model, "POSITION", posAcc, TINYGLTF_TYPE_VEC3, out.vertices, 0, 12);
    if (int a = findAttribute(prim, "NORMAL"); a >= 0)
        readFloatsInto(model, "NORMAL", a, TINYGLTF_TYPE_VEC3, out.vertices, 12, 12);
    if (int a = findAttribute(prim, "TEXCOORD_0"); a >= 0) {
        readFloatsInto(model, "TEXCOORD_0", a, TINYGLTF_TYPE_VEC2, out.vertices, 24, 8);
        out.hasUv = true;
    }
    if (int a = findAttribute(prim, "TANGENT"); a >= 0) {
        out.hasTangent = true;
        // glb 里 TANGENT 可能是 vec3（bistro = vec3，bistro_pack.py 烘的 Lengyel 平滑
        // 切线）或 vec4；vec4 只取 xyz。
        const int type = model.accessors[size_t(a)].type == TINYGLTF_TYPE_VEC4 ? TINYGLTF_TYPE_VEC4 : TINYGLTF_TYPE_VEC3;
        readFloatsInto(model, "TANGENT", a, type, out.vertices, 32, 12);
    }
    for (uint32_t v = 0; v < out.vertexCount; ++v)
        reinterpret_cast<float*>(out.vertices.data() + size_t(v) * kVertexStride)[10] = 1.0f;
}

// 索引段：有索引 accessor = 按组件宽度读 u32；无索引 = 顺序展开（dump.py 同法，
// bistro 1591 个 primitive 全有索引，此路兜底）；两条都过三角数/范围校验。
void readPrimIndices(const tinygltf::Model& model, size_t meshIndex, const tinygltf::Primitive& prim, SourcePrim& out)
{
    const std::string where = "mesh " + std::to_string(meshIndex) + ": ";
    if (prim.indices >= 0) {
        const tinygltf::Accessor& acc = model.accessors[size_t(prim.indices)];
        if (acc.type != TINYGLTF_TYPE_SCALAR)
            throw std::runtime_error(where + "indices 不是 SCALAR");
        AccessorStream s = locateStream(model, "indices", prim.indices);
        const size_t es = componentSize(acc.componentType);
        out.indices.resize(s.count);
        for (size_t i = 0; i < s.count; ++i)
            out.indices[i] = readIndexScalar(s.base + i * s.stride, es);
    } else {
        out.indices.resize(out.vertexCount);
        for (uint32_t i = 0; i < out.vertexCount; ++i) out.indices[i] = i;
    }
    if (out.indices.size() % 3 != 0)
        throw std::runtime_error(where + "索引数不是 3 的倍数");
    for (uint32_t idx : out.indices)
        if (idx >= out.vertexCount)
            throw std::runtime_error(where + "索引越出顶点范围");
}

// 缝 = 失败面：顶点段（属性 accessor 形态）与索引段（组件宽度/范围）各有各的红，
// 出错信息里一眼定位是哪段炸的（认知复杂度护栏同时逼的刀，>15 不许整住一个函数）。
void expandPrim(const tinygltf::Model& model, size_t meshIndex, const tinygltf::Primitive& prim, SourcePrim& out)
{
    out.material = prim.material >= 0 ? uint32_t(prim.material) : 0xFFFFFFFF;
    readPrimVertices(model, prim, out);
    readPrimIndices(model, meshIndex, prim, out);
}

}  // namespace

std::vector<SourcePrim> loadGlbSource(const std::string& path)
{
    tinygltf::TinyGLTF loader;
    loader.SetImageLoader(noopImageLoader, nullptr);
    tinygltf::Model model;
    std::string err, warn;
    if (!loader.LoadBinaryFromFile(&model, &err, &warn, path))
        throw std::runtime_error("glb 加载失败: " + path + ": " + err);

    // 枚举序 = meshes[].primitives[] 展平（= dump.py / §7 对拍口径，不是节点序）
    std::vector<SourcePrim> prims;
    for (size_t m = 0; m < model.meshes.size(); ++m) {
        const tinygltf::Mesh& mesh = model.meshes[m];
        for (const tinygltf::Primitive& prim : mesh.primitives) {
            prims.emplace_back();
            expandPrim(model, m, prim, prims.back());
        }
    }
    return prims;
}

}  // namespace RGL::meshpack
