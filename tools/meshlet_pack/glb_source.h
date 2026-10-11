#pragma once
//
// glb_source.h —— 输入侧：glb → 逐 primitive 的源几何（本工具唯一的 glb 读取面）。
//
// 喂入口径 = §7 spike 的 dump.py（数字对拍的口径来源，别改）：
//   · primitive 枚举序 = tinygltf `meshes[].primitives[]` 展平序（**不是** scene graph
//     节点序）；bistro = 1591 个。
//   · 位置 = 原始 POSITION accessor 数据（**源单位、不应用 node transform、不乘
//     scene.lua scale**）——§7.2 全部簇化数字在这个浮点输入上实测，换口径 = 对拍作废。
//   · 无 indices 的 primitive 补顺序索引（bistro 1591 个全有，兜底与 dump.py 对齐）。
//
// 与 dump.py 的唯一增量：顶点池要 48B 全顶点（ModelVertex 契约，format.h），
// 位置在记录偏移 0 处逐字节等于 accessor 数据 ⟹ clodMesh 直接以 stride=48 读它，
// 位置数值与 spike（stride=12）逐位一致。
//
#include <cstdint>
#include <string>
#include <vector>

#include "format.h"

namespace RGL::meshpack {

struct SourcePrim {
    uint32_t vertexCount = 0;
    uint32_t material = 0xFFFFFFFF;
    // loader 语义镜像（gltf_loader_impl.cppm fillTangents：w 只在 hasTangent && hasUv
    // 时逐三角推）——只服务 --stats 的手性读数分母，不参与打包
    bool hasUv = false;
    bool hasTangent = false;
    std::vector<uint8_t> vertices;   ///< vertexCount × kVertexStride（ModelVertex）
    std::vector<uint32_t> indices;   ///< 数量恒为 3 的倍数；primitive-local 下标
};

// 读 glb。失败抛 std::runtime_error（带 tinygltf 错误文本）。
// no-op image loader：481MB 资产的 405 张贴图**不解码**（默认 loader 会全量解码 ⟹
// 内存爆 + 变慢），只取几何。
std::vector<SourcePrim> loadGlbSource(const std::string& path);

}  // namespace RGL::meshpack
