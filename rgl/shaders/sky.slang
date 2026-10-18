// 天穹契约（Phase 5.5 批次 4 的 skybox 契约中存活的那半边）：clip→world 射线重建
// （原全屏大三角 mesh skybox.mesh.slang 已随刀②.2 删除，本模块现消费者 = light.frag /
//  ao.frag / velocity.frag / shadow.slang 选级）
//
// 数据通道 = 头区类型化 UBO（binding 22，`SceneHeaderBlock::skyInvVP`，迁移批 S-5；
// Phase 5 bindless 原则：每帧变化的数据一律进 descriptor heap，不扩 DrawArgs）
// = inverse(proj·view) mat4 列主序，CPU 侧真相源 SceneFrameData.skyInvViewProj
// （偏移由 scene_forward 反射生成物钉死，本文件零字节数）。
//
// 射线重建免 resolution：mesh 端把大三角 NDC xy 作插值器透传（重心插值 = 像素 NDC），
// frag 取 near/far 两点差 = 世界视线方向（z=0/1 对应 Vulkan 深度范围 [0,1] 的
// near/far 平面）。

import scene;  // SceneHeaderBlock UBO（binding 22）+ mulM 列组合

struct SkyVertexOutput
{
    float4 position : SV_Position;
    float2 ndc      : TEXCOORD0;  // NDC xy（-1..3 大三角内插值覆盖全屏 -1..1）
};

// clip → world（列组合变换，readObjectMatrix 同款——无行/列主序约定歧义）
float4 skyClipToWorld(float4 clip)
{
    return mulM(readSkyInvViewProj(), clip);
}
