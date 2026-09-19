module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>  // registerScene rootTransform 参数 glm::mat4 名可见
export module model_importer;

import rlstd;
import vkerror;
import vk_device;
import vk_resources;
import asset_table;  // AssetTable（mesh/material 资产注册目标，F 拆出：不再经内核透传）
import gltf_loader;

namespace RGL {

/**
 * GPU 上传（Phase 4 批次 2 分块拼接）：模型 primitive 拼成 16MB 块（kVertexChunkBytes，
 * 见 impl），**每块一个 BDA storage buffer**（CPU→GPU mapped，块内 primitive 按块内偏移
 * 写入全部顶点数据）+ **每块一个 heap 描述符槽**（块 = 单一地址范围），注册进
 * AssetTable（每 primitive 一条 MeshAsset 记录，共享所在块 buffer 的 shared_ptr +
 * per-primitive vertexOffset，E1.1）。资产数据生命周期归 main 装配层持有的 AssetTable，不再经
 * 返回值持有（E1.2/L6）。返回资产 id 列表（与 model.primitives 下标对齐：上传顺序 = primitive 顺序）。
 *
 * 管线去重依赖"同 (顶点 slot, 纹理 slot) 组合共享一条管线条目"——binding mapping 是
 * shader object 级状态（无法运行时换），per-primitive 各占 slot → 392 条；分块后同块
 * primitive 共享一个顶点 slot，条目拆分由 (块 slot, 纹理 slot) 组合决定（cottage 392
 * primitive → 6 个顶点块，12 个纹理 slot）。
 *
 * 声明在接口、定义在实现单元 model_importer_impl.cppm（2026-09-02 编译提速 A 方案）：
 * 函数体 + static 辅助不进 BMI，实现单元是叶子边、编译不上关键路径。
 * @param vkDevice 设备层（VMA buffer 创建 / 描述符写入）
 * @param res 资源层（VMA 分配器 + 资产表 + 统一 slot 分配器 + heap）
 * @param model 已解析 gltf 模型（primitives 顶点数据 / 三角形数 / 材质字段）
 * @return 资产 id 列表（与 model.primitives 下标对齐）；失败 Error（VMA buffer 创建 /
 *   slot 越界 / 几何槽布局漂移）
 */
export [[nodiscard]] std::expected<std::vector<uint32_t>, Error> uploadModelMeshes(
	RGL::DeviceContext& vkDevice, RGL::Resources& res, RGL::AssetTable& assets,
	const RGL::GltfModel& model);

/**
 * 场景实体注册：场景根 entity 挂 Bounds + Transform（+ Relationship 根）+ 每 primitive
 * 一个 entity（Transform + MeshRef{资产 id, objectSlot 实例矩阵槽} + MaterialRef 真引用
 * （E5.3）+ Relationship（parent = 根）+ CullSphere（node-local 剔除球））。材质身份
 *（baseColor/textureId/alphaMode）收敛进资产表，纹理→primitive 映射获得数据归属；
 * 同 materialIndex 的材质字段一致性做导入期金丝雀（违例 = loader bug，启动期直接 throw，
 * 不静默带病渲染）。
 * rootTransform（Phase 6 批次 5，多模型场景位姿）：场景根 local = 该矩阵，per-primitive
 * world = rootTransform · local（local = loader 合成的 node 世界矩阵，Phase 7）；Bounds
 * 球心随根变换、半径乘均匀缩放分量（scale 约束同 Phase 5 已知近似②：world 法线 mat3
 * 直乘假定均匀缩放）。默认 identity = 批次 5 前行为。
 * @param rg ECS registry
 * @param model 已解析 gltf 模型
 * @param assetIds mesh 资产 id 列表（uploadModelMeshes 产物，与 model.primitives 下标对齐）
 * @param primitiveTextureIds 纹理资产 id 列表（texture_pool::uploadModelTextures 产物，
 *   与 model.primitives 下标对齐；材质引用图解析有效纹理 id 用）
 * @param resources 资源层（资产表：材质资产注册 / 查询）
 * @param rootTransform 场景根变换（默认 identity）
 * @return 场景根 entity
 */
export entt::entity registerScene(entt::registry& rg, const RGL::GltfModel& model,
	std::span<const uint32_t> assetIds, std::span<const uint32_t> primitiveTextureIds,
	RGL::Resources& resources, RGL::AssetTable& assets, const glm::mat4& rootTransform = glm::mat4(1.0f));

}  // namespace RGL
