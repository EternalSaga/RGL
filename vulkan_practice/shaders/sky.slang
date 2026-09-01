// 天空盒契约（Phase 5.5 批次 4）：全屏大三角 + clip→world 射线重建
//
// 数据通道 = scene data buffer（binding 3，Phase 5 bindless 原则：每帧变化的数据
// 一律进 descriptor heap，不扩 DrawArgs）：@64 skyboxInvViewProj mat4 列主序
// = inverse(proj·view)，CPU 侧真相源 vkresources.cppm kSceneDataSkyOffset +
// SceneFrameData.skyInvViewProj（scene.slang 先例，字面量手工一致，无反射端）。
// 布局（本文件消费区）：@64-127 invViewProj 四列；矩阵表自 @128（scene.slang）。
//
// 射线重建免 resolution：mesh 端把大三角 NDC xy 作插值器透传（重心插值 = 像素 NDC），
// frag 取 near/far 两点差 = 世界视线方向（z=0/1 对应 Vulkan 深度范围 [0,1] 的
// near/far 平面）。

import scene;  // sceneData buffer（binding 3）

struct SkyVertexOutput
{
    float4 position : SV_Position;
    float2 ndc      : TEXCOORD0;  // NDC xy（-1..3 大三角内插值覆盖全屏 -1..1）
};

// clip → world（列组合变换，readObjectMatrix 同款——无行/列主序约定歧义）
float4 skyClipToWorld(float4 clip)
{
    float4 c0 = asfloat(sceneData.Load4(64));
    float4 c1 = asfloat(sceneData.Load4(80));
    float4 c2 = asfloat(sceneData.Load4(96));
    float4 c3 = asfloat(sceneData.Load4(112));
    return c0 * clip.x + c1 * clip.y + c2 * clip.z + c3 * clip.w;
}
