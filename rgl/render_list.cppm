module;
#include <glm/glm.hpp>  // RenderListState 成员 std::vector<glm::mat4> 需 glm::mat4 名可见
#include <entt/entt.hpp>
export module render_list;

import rlstd;
import material_types_generated;  // Phase 4：统一 DrawArgs 超集（scalar 布局 100B，Phase 5 +objectIndex）
import frame_draw_list;
import vk_resources;
import asset_table;
import scene_context;
import camera;

namespace RGL {

// renderlist system（ViewProjection + 场景实体 + 场景旋转 → FrameDrawList）拆出为独立
// 模块（2026-09-02）：ecs 帧循环骨架与最大块的 system 解耦——场景数据流
// （Transform/MeshRef/MaterialRef/材质资产/透明排序/光照接线/天空盒）集中一处。

/**
 * 每帧 push constant 存储（帧数据：活过 renderSystem 循环，DrawCall 的 span 指向其中；
 * vkCmdPushDataEXT 录制时读取，只要求内存活到录制结束）。
 * Phase 4 归一（H1 解决）：单一统一 DrawArgs 超集（material_types_generated，scalar 布局 100B）
 * ——删旧 cube 单例(64B) + model 每资产(96B，Phase 5 前) 双容器与 AssetKind 分支。
 * Phase 6 instancing：args/matrices 按 **objectSlot**（MeshRef.objectSlot，实例级——同
 * asset 多实例各持独立变换；容量 = NextObjectSlot.next，main 启动期契约检查
 * ≤ kSceneMatrixCapacity）索引，**非**资产 id。
 */
export struct RenderListState {
	std::vector<RGL::DrawArgs> args;  // 每 objectSlot 一份（Phase 7b+ 起 4B = 仅 objectIndex；实例级索引）
	// 对象矩阵表（Phase 5 批次 2，bindless）：objectSlot 索引（实例级，Phase 6
	// instancing）的 spin·world，DrawArgs.objectIndex
	// 进 shader 索引本表（世界空间光照）。帧局部存储（FrameDrawList.sceneData 持 span，
	// 录制期拷贝进 staging，生命周期规则同 args）
	std::vector<glm::mat4> matrices;
	// Phase 7b+（NV DGC 规避）：per-objectSlot MVP 表（scene data 尾区上传，
	// 填值 = args[slot].mvp 同一份，位级一致；shader readObjectMvp 消费）
	std::vector<glm::mat4> mvps;
	// Phase 7b+（NV DGC 规避）：per-objectSlot 材质/几何表（scene data 尾区上传，
	// shader readObjectDraw 消费；DrawArgs 仅剩 objectIndex）
	std::vector<ObjectDrawGpu> draws;
	// Phase 5.5 批次 4：天空盒 draw 的 push data（shader 不声明 push constants，
	// 推默认零值 DrawArgs 占位——pushDataEXT 与 shader 声明无关，见 DrawCall 契约）
	RGL::DrawArgs skyboxArgs{};
	// Phase 7 批次 4：CPU frustum culling 可见统计（renderListSystem 每帧填；可见/剔除计数）
	uint32_t visibleCount = 0;
	uint32_t culledCount = 0;
};

/**
 * ViewProjection + 场景实体 + 场景旋转 → FrameDrawList（registry ctx）context（单一 draw
 * 路径）。
 *
 * Phase 4 归一：统一 DrawArgs 超集（所有 shader 风格共享同一布局，material.slang 单一真相源，
 * scalar 布局 100B）——单一写入路径写满字段，零 AssetKind 分支（旧 cube 64B / model 96B
 * 双 struct 分支删除，ecs_compliance_review H1 注记解决）。各风格只消费自己需要的字段
 *（未读字段无害）：cube 只读 mvp（每顶点色），model 读 mvp/totalTriangles/baseColor。
 * groupCount 经资产静态字段（MeshAsset.groupCount，上传期按 shader 契约派生，D6）：
 * model=ceil(tri/kTrianglesPerGroup)、cube=1 固定单 group、0=不渲染——每帧零推导零分支。
 * vertexOffset 经资产静态字段（MeshAsset.vertexOffset，批次 2 拼接顶点 buffer per-primitive
 * 偏移，上传期写入）；roughnessMetallic（批次 3 pbr 材质）写材质资产值（pbr frag 消费）。
 * 画不画 = 实体有无（数据驱动）。
 *
 * Phase 5 透明排序（roadmap 决策：排序在 renderSystem 内做，不单独建 system）：
 * 经 MaterialRef → MaterialAsset.alphaModeBlend（glTF alphaMode == "BLEND"）分桶——
 * 不透明段保持实体迭代序（深度写正确），透明段按相机距离² 降序（back-to-front）稳定排序，
 * 产出 [opaque | skybox | transparent 远→近] 单列表 + skyboxStart/transparentStart 边界
 *（drawFrame 段首各 applyDynamicState 一次，见 vkrenderer.cppm）。MASK 材质留在不透明段
 *（Phase 8 C1：alpha-test discard 在 shader 内，cutoff 经 ObjectDraw.emissive.w；doubleSided
 * 的 cull 切换见 C2）；排序距离 = primitive 实体 world 平移列
 * 到相机位置的 distance2（无包围盒时的标准近似，legacy RenderQueue 同款思路）。
 *
 * Phase 7 批次 4 CPU frustum culling：视锥 6 面从 proj·view 提取（每帧一次），逐 entity
 * node-local 剔除球（CullSphere 组件）经 modelMatrix（spin·world）变换到世界后测——
 * 保守：跨界判可见，绝不误剔；可见/剔除计数写 state（仅计数变化时打日志）。
 *
 * 灯 entity 化（Phase 7，ECS 规范 3）：灯 = entity（Light 组件，画不画 = 实体有无），
 * 收集按 Light.index 落槽（不依赖 view 迭代序）：primary = index 0（forward @0 头），
 * 延迟灯数组按 index 填（≤ kMaxLights）。
 *
 * @param rg registry（读 ViewProjection/SceneSpin/Camera/SceneEnv/NextObjectSlot，
 *   遍历 Transform+MeshRef+MaterialRef+CullSphere 实体 view + Light 组件 view，写 ctx FrameDrawList）
 * @param resources 资源层（资产表：mesh/material 记录解析）
 * @param state 每帧 push constant 存储（args/matrices/skyboxArgs 填充 + culling 统计）
 */
export void renderListSystem(entt::registry& rg, RGL::Resources& resources, RenderListState& state);

}  // namespace RGL
