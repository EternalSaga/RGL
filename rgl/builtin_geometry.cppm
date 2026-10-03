module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>  // registerBuiltinCube rootTransform 参数 glm::mat4 名可见
export module builtin_geometry;

import rlstd;
import vkerror;
import vk_device;
import vk_resources;
import asset_table;  // AssetTable（资产注册目标，F 拆出：不再经内核透传）
import scene_context;

namespace RGL {

// 内建几何（程序化资产，无外部文件依赖）。架构债务 1 方案 B（2026-09-08）：cube 从
// "Resources 构造期硬编码成员 + 编译期 slot 0/1 契约"升格为与 gltf 模型**同一条注册路**的
// 普通资产——buffer/slot/描述符/MeshAsset 记录/管线条目/entity 六件事都在本模块做完，
// 资源内核（vk_resources）不认识 cube。方向修正：内容在上消费内核，不再是内核持有内容。
// 用户可见行为零改动（scene.lua `entities = { { type = "cube" } }` / 旧 `cube = true` 字段
// 照旧生效），"零资产可跑"的排障场景保留。

/**
 * 上传内建 cube（8 顶点 / 36 索引 / 12 三角形 / 每顶点色，几何数据见 cube.cppm）：
 * 两个 BDA storage buffer + 两个 heap **几何段槽**（allocSlot Geometry，计入
 * AssetTable::geometrySlotCount）+ 描述符写入 → 产物 = AssetTable 一条 MeshAsset
 *（AssetKind::Builtin，带索引 buffer/slot，groupCount = 1 固定）。
 * **段契约**：与 uploadModelMeshes 同走几何段，期望槽号由几何段游标和
 * AssetTable::expectedGeometrySlot 校验；纹理首槽由纹理段首槽独立校验。
 * @param vkDevice 设备层（VMA buffer 创建 / 描述符写入）
 * @param res 资源层（VMA 分配器 + 资产表 + 统一 slot 分配器）
 * @return 资产 id；失败 Error（VMA buffer 创建 / slot 越界 / 几何槽布局漂移，见 impl）
 * ⚠ **容量上限 = 8 顶点 / 12 三角形，且不可扩**：`cube.mesh.slang` 把两者写死，
 * `AssetTable::addIndexedMesh` 的 groupCount 恒 1（批 12.6 T4 实测：喂更大的网格
 * 不报错，第 13 个三角形起**静默截断**——截图只剩一个角，最难查的一类假线索）。
 * 需要更复杂的程序化几何请走**模型路**（内存合成 `GltfModel` → `uploadModelMeshes`，
 * 细线抗锯齿靶 `aa_target.cppm` 即先例），别给本函数加参数。
 */
export [[nodiscard]] std::expected<uint32_t, Error> uploadBuiltinCube(
	RGL::DeviceContext& vkDevice, RGL::Resources& res, RGL::AssetTable& assets);

/**
 * cube entity 注册（与 registerScene 同构，ECS 规范 3：画不画 = 实体有无）：
 * Transform（local = world = rootTransform，几何已在原点 ±0.5）+ Bounds（外接球随根变换——
 * 球心 = 原点经根变换，半径 = 顶点到原点最大距离 × 根变换均匀缩放分量（列 0 长度），
 * 经 boundsSystem 纳入 SceneBounds 聚合 near/far）+ MeshRef{内建资产 id, 实例矩阵槽
 * （注册期从 NextObjectSlot 单调领取）} + MaterialRef + CullSphere（node-local 剔除球，
 * 球心原点 / 半径 = 顶点到原点最大距离）+ Relationship（根）。
 * cube 着色 = 每顶点色（cube.frag 从顶点读 color），MaterialRef 的 baseColor 不被 cube
 * 管线消费（Phase 4 归一：统一 DrawArgs 超集单一布局，各风格只消费自己字段）。
 * @param rg ECS registry
 * @param meshAsset 内建 mesh 资产 id（uploadBuiltinCube 产物）
 * @param materialAsset 材质资产 id（见上：baseColor 不被 cube 管线消费，字段仅为同构保留）
 * @param rootTransform 根变换（默认 identity；几何已在原点 ±0.5）
 * @return 创建的 cube entity
 */
export entt::entity registerBuiltinCube(entt::registry& rg, uint32_t meshAsset,
	uint32_t materialAsset, const glm::mat4& rootTransform = glm::mat4(1.0f));

}  // namespace RGL
