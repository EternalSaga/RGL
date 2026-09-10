// 场景级 bindless 数据契约（Phase 5 批次 2/3：光照 + 对象矩阵表）
//
// 单一 storage buffer（descriptor heap binding 3，Resources::ensureSceneData 创建，
// 每帧 staging → vkCmdCopyBuffer 更新，见 vkrenderer.cppm）。
// 字节布局契约的单一真相源 = vkresources.cppm（LightDataGpu / kSceneData* 常量），
// 本文件字面量必须一致（kTrianglesPerGroup 先例，无反射端对应）：
//   @0   direction vec3 | @12 eyePos vec3 | @24 color vec3（CPU 预乘 intensity）| @36 ambient vec3
//        （两个 intensity 语义：光色在此 CPU 预乘成最终值；env intensity 是 @48 的裸标量、
//          GPU 端乘——env 颜色在 GPU 侧无法预乘，调光两条路线各改各 config 的一个 intensity）
//   @48  env 参数（Phase 5.5，env.slang readEnvParams 消费）| @64 skyboxInvViewProj
//        mat4 列主序（Phase 5.5 批次 4，sky.slang skyClipToWorld 消费）
//   @128 对象矩阵表：mat4 列主序 × kSceneMatrixCapacity(4096)，[objectIndex] = spin·world
//        （DrawArgs.objectIndex = MeshRef.objectSlot，实例级槽，Phase 6 instancing）
//   @262272 灯数组（lights.slang）
//   @262800 per-object MVP 表：mat4 列主序 × 4096，[objectIndex] = proj·view·world
//        （Phase 7b+ NV DGC 规避：DrawArgs 去 mvp 后 push 36B < NV 驱动 84B 挂死阈值；
//          CPU 每帧写，值 = 原 DrawArgs.mvp 位级一致；基址 = 128+4096×64+528，
//          vkresources kSceneDataMvpTableOffset 同值——字面量同步，无反射端）
//   @524944 per-object 材质/几何表：stride 32 × 4096，[objectIndex]（Phase 7b+ 续：
//        DrawArgs 最终只留 objectIndex 4B push，其余全进此表；readObjectDraw 消费）
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

// per-object MVP（列主序 4 列显式读，返回 ObjectMatrix 结构，消费方用 clipPos 列组合——
// 不用 float4x4(c0,c1,c2,c3)：Slang/HLSL 该构造把入参当【行】，会把列主序内存读成转置，
// 非对称变换（带旋转的建筑）直接炸形，地面近似对称侥幸正常。clipPos 与 mul(原push mvp) 位级一致）
float4 clipPos(ObjectMatrix m, float3 p)
{
    return m.c0 * p.x + m.c1 * p.y + m.c2 * p.z + m.c3;
}

ObjectMatrix readObjectMvp(uint objectIndex)
{
    uint base = 262800u + objectIndex * 64u;
    ObjectMatrix m;
    m.c0 = asfloat(sceneData.Load4(base));
    m.c1 = asfloat(sceneData.Load4(base + 16u));
    m.c2 = asfloat(sceneData.Load4(base + 32u));
    m.c3 = asfloat(sceneData.Load4(base + 48u));
    return m;
}

// Phase 7b+（NV DGC 规避）：per-object 材质/几何表（DrawArgs 去 mvp 后剩余字段也进 heap，
// push 仅留 objectIndex）。基址 @524944 = 128+4096×64+528+4096×64，stride 32，
// vkresources kSceneDataDrawTableOffset / ObjectDrawGpu 同布局（字面量同步，无反射端）：
//   @0  uint totalTriangles | uint vertexOffset | float roughness | float metallic
//   @16 float4 baseColor
struct ObjectDraw
{
    uint totalTriangles;
    uint vertexOffset;
    float4 baseColor;
    float2 roughnessMetallic;
};

ObjectDraw readObjectDraw(uint objectIndex)
{
    uint base = 524944u + objectIndex * 32u;
    float4 a = asfloat(sceneData.Load4(base));  // x=totalTri y=vertOff z=rough w=metal
    ObjectDraw d;
    d.totalTriangles = asuint(a.x);
    d.vertexOffset = asuint(a.y);
    d.roughnessMetallic = float2(a.z, a.w);
    d.baseColor = asfloat(sceneData.Load4(base + 16u));
    return d;
}

float3 worldPos(ObjectMatrix m, float3 p)
{
    return (m.c0 * p.x + m.c1 * p.y + m.c2 * p.z + m.c3).xyz;
}

float3 worldNormal(ObjectMatrix m, float3 n)
{
    return normalize(m.c0.xyz * n.x + m.c1.xyz * n.y + m.c2.xyz * n.z);
}
