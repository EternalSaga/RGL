module;
#include <glm/glm.hpp>
export module frame_draw_list;

import rlstd;
import vk_resources;

namespace RGL {

// 绘制契约（ECS 规范 4：渲染 system 产出、执行器盲执行的唯一接口）。
// 2026-09-02 拆自 vkrenderer：struct 与 Renderer 执行器解耦——ecs/llm_debug/main 只
// import 本模块即可表达契约，不必拖入整个 Renderer（Vulkan 执行层）。
//
// 一次绘制（FrameDrawList 的条目；由渲染 system 每帧产出，drawFrame 消费）：
//   pipeline   = 管线表下标（经 Renderer::pipelineIndex 由条目名解析，构造后稳定）
//   pushData   = 本次 draw 的 push constant，布局随管线（CPU 内存录制时读取——
//                vkCmdPushDataEXT 只要求内存活到录制结束，故帧局部存储即可）
//   groupCount = drawMeshTasksEXT 的 X（Y/Z 现状恒 1）
export struct DrawCall {
	uint32_t pipeline;
	std::span<const std::byte> pushData;
	uint32_t groupCount;
};

// 场景级每帧动态数据（Phase 5 批次 2，bindless）：渲染 system 产出、drawFrame 消费——
// CPU 写 staging[currentFrame] + 录制期 vkCmdCopyBuffer 拷进 scene data buffer
//（布局契约见 vkresources.cppm LightDataGpu / scene.slang）。objectMatrices 按资产 id
// 索引（DrawArgs.objectIndex），span 指向渲染 system 帧局部存储（录制期读取即完成使命，
// 同 DrawCall.pushData 生命周期规则）。
export struct SceneFrameData {
	LightDataGpu light;
	// Phase 5.5 env 参数（scene data 头 @48/@52，原 16B 保留区启用；shader 端
	// env.slang readEnvParams 字面量一致）：x=envIntensity y=envMipRange（=L−1，0 = 无环境）
	glm::vec2 envParams{0.0f};
	// Phase 5.5 批次 4：天空盒 clip→world 矩阵（scene data @64，sky.slang skyClipToWorld；
	// = inverse(proj·view)，renderListSystem 每帧从 ViewProjection 推导）
	glm::mat4 skyInvViewProj{1.0f};
	std::span<const glm::mat4> objectMatrices;
};

// 每帧绘制列表：顺序 = 绘制顺序（Phase 5 透明排序已落地：不透明段在前、透明段按
// 相机距离 back-to-front 排；Phase 7 culling 输出的落点）。
// Phase 5.5 批次 4 三段：opaque = [0, skyboxStart)，skybox = [skyboxStart,
// transparentStart)，transparent = [transparentStart, size)——两个边界字段与段序
// **与 Phase 6 RenderGraph pass 序列同构**（机械翻译：每段 = 一个 Pass，边界 = 依赖边）。
// drawFrame 段首各 applyDynamicState 一次（不透明写深度关混合 / 天空盒 LEQUAL 不写深度 /
// 透明关深度写开混合），段内逐 draw 循环不变（盲执行契约保持）。
export struct FrameDrawList {
	std::vector<DrawCall> calls;
	uint32_t skyboxStart = 0;       // 无天空盒 = 与 transparentStart 重合（空段跳过）
	uint32_t transparentStart = 0;
	SceneFrameData sceneData;
};

}  // namespace RGL
