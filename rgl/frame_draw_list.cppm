module;
#include <glm/glm.hpp>
export module frame_draw_list;

import rlstd;
import vk_resources;
import lights_types_generated;
import material_types_generated;  // DrawArgs（Phase 7b cullArgs：GPU culling 候选 args）
import scene_forward_types_generated;  // 前向头 SceneLight（C API 反射生成，替换原 LightDataGpu）

namespace RGL {

// 绘制契约（ECS 规范 4：渲染 system 产出、执行器盲执行的唯一接口）。
// 2026-09-02 拆自 vkrenderer：struct 与 Renderer 执行器解耦——ecs/llm_debug/main 只
// import 本模块即可表达契约，不必拖入整个 Renderer（Vulkan 执行层）。
//
/**
 * 一次绘制（FrameDrawList 的条目；由渲染 system 每帧产出，drawFrame 消费）：
 *   pipeline   = 管线表下标（直接取自资产记录 MeshAsset.pipelineIndex /
 *                SceneEnv.skyboxPipeline——启动期经 ShaderRegistry::bindPipelineToAsset
 *                写回，每帧路径零字符串查找，构造后稳定；见 render_list_impl）
 *   pushData   = 本次 draw 的 push constant，布局随管线（CPU 内存录制时读取——
 *                vkCmdPushDataEXT 只要求内存活到录制结束，故帧局部存储即可）
 *   groupCount = drawMeshTasksEXT 的 X（Y/Z 现状恒 1，见 vkrenderer draw 循环）
 */
export struct DrawCall {
	uint32_t pipeline;                    // 管线表下标（ShaderRegistry::entries 下标）
	std::span<const std::byte> pushData;  // push constant 字节序列（DrawArgs，帧局部存储）
	uint32_t groupCount;                  // mesh task group 数（资产记录 MeshAsset.groupCount）
};

/**
 * Phase 7b+（NV DGC 规避）：per-object 材质/几何表条目（scene data @kSceneDataDrawTableOffset，
 * stride 32，objectIndex 索引）。DrawArgs 去 mvp 后剩余字段一并进 heap，push 仅留 objectIndex
 *（4B）绕开 NV DGC push 挂死。布局与 scene.slang readObjectDraw 字面量手工一致：
 *   @0 uint totalTriangles | uint vertexOffset | float roughness | float metallic
 *   @16 float4 baseColor
 */
export struct ObjectDrawGpu {
	uint32_t totalTriangles = 0;
	uint32_t vertexOffset = 0;
	float roughness = 0.0f;
	float metallic = 0.0f;
	glm::vec4 baseColor{1.0f};
};
static_assert(sizeof(ObjectDrawGpu) == 32, "ObjectDrawGpu must match scene.slang readObjectDraw stride 32");

/**
 * 场景级每帧动态数据（Phase 5 批次 2，bindless）：渲染 system 产出、drawFrame 消费——
 * CPU 写 staging[currentFrame] + 录制期 vkCmdCopyBuffer 拷进 scene data buffer
 *（布局契约见 scene.slang SceneLight / scene_forward_types_generated）。objectMatrices 按
 * **对象矩阵槽**（MeshRef.objectSlot，实例级；DrawArgs.objectIndex = 同值——Phase 6
 * instancing：同 asset 多实例各持独立槽，**非**资产 id 索引），span 指向渲染 system
 * 帧局部存储（录制期读取即完成使命，同 DrawCall.pushData 生命周期规则）。
 */
export struct SceneFrameData {
	SceneLight light;  // scene data @0 头单灯（forward 消费 lights[0]，布局见 scene_forward_types_generated）
	// Phase 5.5 env 参数（scene data 头 @48/@52，原 16B 保留区启用；shader 端
	// env.slang readEnvParams 字面量一致）：x=envIntensity y=envMipRange（=L−1，0 = 无环境）
	glm::vec2 envParams{0.0f};
	// Phase 5.5 批次 4：天空盒 clip→world 矩阵（scene data @64，sky.slang skyClipToWorld；
	// = inverse(proj·view)，renderListSystem 每帧从 ViewProjection 推导）
	glm::mat4 skyInvViewProj{1.0f};
	std::span<const glm::mat4> objectMatrices;  // scene data @128 对象矩阵表（spin·world，按 objectSlot 索引）
	// Phase 7b+（NV DGC 规避）：per-object MVP 表（scene data @kSceneDataMvpTableOffset，
	// 按 objectSlot 索引，值 = 原 DrawArgs.mvp 位级一致；shader readObjectMvp）
	std::span<const glm::mat4> objectMvps;
	// Phase 7b+（续）：per-object 材质/几何表（scene data @kSceneDataDrawTableOffset，
	// 按 objectSlot 索引；shader readObjectDraw 消费，DrawArgs 仅剩 objectIndex）
	std::span<const ObjectDrawGpu> objectDraws;
	// Phase 7 D3：多灯数组（scene data 尾区 kSceneDataLightArrayOffset，4B count@base +
	// GpuLight[count]@base+16，契约见 lights.slang / lights_schema.lua）。lightCount=0 时
	// 该区不写。forward 仍走 .light（@0 头单灯），二者独立填。
	uint32_t lightCount = 0;
	std::array<GpuLight, kMaxLights> lightArray{};
};

/**
 * 每帧 GPU culling 输入（Phase 7b 批次 5；render system 产出 → drawFrame 经 push
 * constant 传给 culling compute）：planes 与 CPU 剔除路径同源同值（extractFrustum
 * 产物）——GPU/CPU 对账基准一致的保证。布局（cull.comp.slang CullPush 同字节，
 * scalar layout 无尾 pad = 100B ≤ maxPushDataSize 256B）：
 *   @0 planes[6] float4 | @96 sequenceCount u32（mvp 不经 push——CPU 算进序列）
 */
export struct CullFrameInput {
	glm::vec4 planes[6]{};
	uint32_t sequenceCount = 0;
};
static_assert(sizeof(CullFrameInput) == 100, "CullFrameInput layout drifted from cull.comp.slang CullPush");

/**
 * 每帧绘制列表：顺序 = 绘制顺序（Phase 5 透明排序已落地：不透明段在前、透明段按
 * 相机距离 back-to-front 排；Phase 7 culling 输出的落点）。
 * Phase 5.5 批次 4 三段：opaque = [0, skyboxStart)，skybox = [skyboxStart,
 * transparentStart)，transparent = [transparentStart, size)——两个边界字段与段序
 * **与 Phase 6 RenderGraph pass 序列同构**（机械翻译：每段 = 一个 Pass，边界 = 依赖边）。
 * drawFrame 段首各 applyDynamicState 一次（不透明写深度关混合 / 天空盒 LEQUAL 不写深度 /
 * 透明关深度写开混合），段内逐 draw 循环不变（盲执行契约保持）。
 */
export struct FrameDrawList {
	std::vector<DrawCall> calls;
	uint32_t skyboxStart = 0;       // 无天空盒 = 与 transparentStart 重合（空段跳过）
	uint32_t transparentStart = 0;
	SceneFrameData sceneData;
	// Phase 7b：GPU culling 每帧输入（render_list 与 CPU 剔除同源填充；GpuCull 未激活
	// 时 Renderer 忽略——始终填充，零条件分支）
	CullFrameInput cullInput;
	// Phase 7b：GPU culling 候选 args（**逐 candidate、candidate 序**——与 GpuCull 表同序
	// 同守卫，render_list 与 main C3 双处契约）。mvp 在剔除测试**前**算（被剔候选也要有
	// 值——序列 CPU 侧全量装配）；重放/DGC 消费 = 与 CPU drawOne 位级同源的 DrawArgs。
	std::vector<RGL::DrawArgs> cullArgs;
};

}  // namespace RGL
