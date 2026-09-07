module;
#include <glm/glm.hpp>  // RenderListState 成员 std::vector<glm::mat4> 需 glm::mat4 名可见
#include <entt/entt.hpp>
export module render_list;

import rlstd;
import material_types_generated;  // Phase 4：统一 DrawArgs 超集（scalar 布局 100B，Phase 5 +objectIndex）
import frame_draw_list;
import vk_resources;
import asset_table;
import scene;
import camera;

namespace RGL {

// renderlist system（ViewProjection + 场景实体 + 场景旋转 → FrameDrawList）拆出为独立
// 模块（2026-09-02）：ecs 帧循环骨架与最大块的 system 解耦——场景数据流
// （Transform/MeshRef/MaterialRef/材质资产/透明排序/光照接线/天空盒）集中一处。

// 每帧 push constant 存储（帧数据：活过 renderSystem 循环，DrawCall 的 span 指向其中；
// vkCmdPushDataEXT 录制时读取，只要求内存活到录制结束）
// Phase 4 归一（H1 解决）：单一统一 DrawArgs 超集（material_types_generated，scalar 布局 100B）
// 按资产 id 索引——删旧 cube 单例(64B) + model 每资产(96B，Phase 5 前) 双容器与 AssetKind 分支。
export struct RenderListState {
	std::vector<RGL::DrawArgs> args;  // 每资产一份（100B；资产 id 索引，cube/model 共用同一路径）
	// 对象矩阵表（Phase 5 批次 2，bindless）：资产 id 索引的 spin·world，DrawArgs.objectIndex
	// 进 shader 索引本表（世界空间光照）。帧局部存储（FrameDrawList.sceneData 持 span，
	// 录制期拷贝进 staging，生命周期规则同 args）
	std::vector<glm::mat4> matrices;
	// Phase 5.5 批次 4：天空盒 draw 的 push data（shader 不声明 push constants，
	// 推默认零值 DrawArgs 占位——pushDataEXT 与 shader 声明无关，见 DrawCall 契约）
	RGL::DrawArgs skyboxArgs{};
	// Phase 7 批次 4：CPU frustum culling 可见统计（renderListSystem 每帧填；可见/剔除计数）
	uint32_t visibleCount = 0;
	uint32_t culledCount = 0;
};

// ViewProjection + 场景实体 + 场景旋转 → FrameDrawList context（单一 draw 路径）。
//
// Phase 4 归一：统一 DrawArgs 超集（所有 shader 风格共享同一布局，material.slang 单一真相源，
// scalar 布局 100B）——单一写入路径写满字段，零 AssetKind 分支（旧 cube 64B / model 96B
// 双 struct 分支删除，ecs_compliance_review H1 注记解决）。各风格只消费自己需要的字段
//（未读字段无害）：cube 只读 mvp（每顶点色），model 读 mvp/totalTriangles/baseColor。
// groupCount 经资产静态字段（MeshAsset.groupCount，上传期按 shader 契约派生，D6）：
// model=ceil(tri/kTrianglesPerGroup)、cube=1 固定单 group、0=不渲染——每帧零推导零分支。
// vertexOffset 经资产静态字段（MeshAsset.vertexOffset，批次 2 拼接顶点 buffer per-primitive
// 偏移，上传期写入）；roughnessMetallic（批次 3 pbr 材质）当前写占位值，对应批次启用真实数据源。
// 画不画 = 实体有无（数据驱动）。
//
// Phase 5 透明排序（roadmap 决策：排序在 renderSystem 内做，不单独建 system）：
// 经 MaterialRef → MaterialAsset.alphaModeBlend（glTF alphaMode == "BLEND"）分桶——
// 不透明段保持实体迭代序（深度写正确），透明段按相机距离² 降序（back-to-front）稳定排序，
// 产出 [opaque | transparent 远→近] 单列表 + transparentStart 边界（drawFrame 两段
// 各 applyDynamicState 一次，见 vkrenderer.cppm）。MASK 材质暂按不透明处理（glTF 规范
// 要求 alphaTest 剔除，未实现）；排序距离 = primitive 实体 world 平移列到相机位置的
// distance2（无包围盒时的标准近似，legacy RenderQueue 同款思路）。
export void renderListSystem(entt::registry& rg, RGL::Resources& resources, RenderListState& state);

}  // namespace RGL
