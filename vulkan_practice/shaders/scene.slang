// 场景级 bindless 数据契约（Phase 5 批次 2/3：光照 + 对象矩阵表）
//
// 单一 storage buffer（descriptor heap binding 3，Resources::ensureSceneData 创建，
// 每帧 staging → vkCmdCopyBuffer 更新，见 vkrenderer.cppm）。
// 字节布局契约的单一真相源 = vkresources.cppm（LightDataGpu / kSceneData* 常量），
// 本文件字面量必须一致（kTrianglesPerGroup 先例，无反射端对应）：
//   @0   direction vec3 | @12 eyePos vec3 | @24 color vec3（CPU 预乘 intensity）| @36 ambient vec3
//   @48  env 参数（Phase 5.5，env.slang readEnvParams 消费）| @64 skyboxInvViewProj
//        mat4 列主序（Phase 5.5 批次 4，sky.slang skyClipToWorld 消费）
//   @128 对象矩阵表：mat4 列主序 × 1024，[objectIndex] = spin·world（DrawArgs.objectIndex 索引）
//
// 世界空间光照：本文件负责 object→world 变换（位置/法线各一次矩阵读，仅光照风格消费；
// unlit 风格不 import 本文件）。法线变换取 mat3 直接乘（假定均匀缩放——glTF 节点
// 基本满足；非均匀缩放失真为本期已知近似，Phase 7 真实 normalMatrix 再议）。

[[vk::binding(3, 0)]]
ByteAddressBuffer sceneData;

struct SceneLight
{
    float3 direction;  // 照射方向（世界空间，归一化）
    float3 eyePos;     // 相机世界位置
    float3 color;      // 光色 × intensity
    float3 ambient;
};

SceneLight readSceneLight()
{
    SceneLight l;
    l.direction = asfloat(sceneData.Load3(0));
    l.eyePos    = asfloat(sceneData.Load3(12));
    l.color     = asfloat(sceneData.Load3(24));
    l.ambient   = asfloat(sceneData.Load3(36));
    return l;
}

// 对象世界矩阵（列主序 4 列显式读，列组合变换——无行/列主序约定歧义）
struct ObjectMatrix
{
    float4 c0, c1, c2, c3;
};

ObjectMatrix readObjectMatrix(uint objectIndex)
{
    uint base = 128u + objectIndex * 64u;
    ObjectMatrix m;
    m.c0 = asfloat(sceneData.Load4(base));
    m.c1 = asfloat(sceneData.Load4(base + 16u));
    m.c2 = asfloat(sceneData.Load4(base + 32u));
    m.c3 = asfloat(sceneData.Load4(base + 48u));
    return m;
}

float3 worldPos(ObjectMatrix m, float3 p)
{
    return (m.c0 * p.x + m.c1 * p.y + m.c2 * p.z + m.c3).xyz;
}

float3 worldNormal(ObjectMatrix m, float3 n)
{
    return normalize(m.c0.xyz * n.x + m.c1.xyz * n.y + m.c2.xyz * n.z);
}
