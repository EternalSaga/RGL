module;
#include <entt/entt.hpp>
export module model_importer;

import rlstd;
import vkerror;
import vk_device;
import vk_resources;
import gltf_loader;

namespace RGL {

// GPU 上传：拼接顶点 buffer（每模型一个 BDA storage buffer，CPU→GPU mapped 按各 primitive
// 偏移写入全部顶点数据）+ 一个 heap 描述符槽（整段拼接 buffer = 单一地址范围），注册进
// Resources 资产表（每 primitive 一条 MeshAsset 记录，共享拼接 buffer 的 shared_ptr +
// per-primitive vertexOffset，E1.1）。资产数据生命周期归资产表（Resources 成员），不再经
// 返回值持有（E1.2/L6）。返回资产 id 列表（与 model.primitives 下标对齐：上传顺序 = primitive 顺序）。
//
// Phase 4 批次 2：管线去重依赖"同 (顶点 slot, 纹理 slot) 组合共享一条管线条目"——binding
// mapping 是 shader object 级状态，per-primitive 各占 slot → 392 条；拼接后同模型全 primitive
// 共享一个顶点 slot，仅纹理 slot 差异决定条目拆分（cottage 12 个纹理 slot → 12 条）。
//
// 声明在接口、定义在实现单元 model_importer_impl.cppm（2026-09-02 编译提速 A 方案）：
// 函数体 + static 辅助不进 BMI，实现单元是叶子边、编译不上关键路径。
export [[nodiscard]] std::expected<std::vector<uint32_t>, Error> uploadModelMeshes(
	RGL::DeviceContext& vkDevice, RGL::Resources& res, const RGL::GltfModel& model);

// 场景实体注册：场景根 entity 挂 Bounds + 每 primitive 一个 entity（MeshRef 资产 id +
// MaterialRef 真引用（E5.3）——材质身份（baseColor/textureId/alphaMode）收敛进资产表，
// 纹理→primitive 映射获得数据归属；同 materialIndex 的材质字段一致性做导入期金丝雀
//（违例 = loader bug））。
export entt::entity registerScene(entt::registry& rg, const RGL::GltfModel& model,
	std::span<const uint32_t> assetIds, std::span<const uint32_t> primitiveTextureIds,
	RGL::Resources& resources);

}  // namespace RGL
