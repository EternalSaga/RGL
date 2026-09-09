module;
#include <entt/entt.hpp>
#include <glm/glm.hpp>  // registerBuiltinCube rootTransform 参数 glm::mat4 名可见
export module builtin_geometry;

import rlstd;
import vkerror;
import vk_device;
import vk_resources;
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
 * 两个 BDA storage buffer + 两个 heap **几何槽**（allocSlot 单调，计入
 * AssetTable::geometrySlotCount）+ 描述符写入 → 产物 = AssetTable 一条 MeshAsset
 *（AssetKind::Builtin，带索引 buffer/slot，groupCount = 1 固定）。
 * **时序契约**：与 uploadModelMeshes 同阶段（A1 几何段，先于任何纹理槽）——
 * "纹理首槽 = 白 = 全部几何槽之后"断言依赖本顺序。
 * @param vkDevice 设备层（VMA buffer 创建 / 描述符写入）
 * @param res 资源层（VMA 分配器 + 资产表 + 统一 slot 分配器）
 * @return 资产 id；失败 Error（VMA buffer 创建 / slot 越界 / 几何槽布局漂移，见 impl）
 */
export [[nodiscard]] std::expected<uint32_t, Error> uploadBuiltinCube(
	RGL::DeviceContext& vkDevice, RGL::Resources& res);

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
