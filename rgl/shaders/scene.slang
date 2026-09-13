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
//   @524944 per-object 材质/几何表：stride 48 × 4096，[objectIndex]（Phase 7b+ 续：
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
// push 仅留 objectIndex）。基址 @524944 = 128+4096×64+528+4096×64，stride 48，
// vkresources kSceneDataDrawTableOffset / ObjectDrawGpu 同布局（字面量同步，无反射端）：
//   @0  uint totalTriangles | uint vertexOffset | float roughness | float metallic
//   @16 float4 baseColor
//   @32 float4 emissive（Phase 8 ③：xyz=emissiveFactor、w 留白；无 emissive 材质 = 0）
struct ObjectDraw
{
    uint totalTriangles;
    uint vertexOffset;
    float4 baseColor;
    float2 roughnessMetallic;
    float4 emissive;  // Phase 8 ③：xyz=emissiveFactor（model_pbr_ibl.frag 叠加自发光）
};

ObjectDraw readObjectDraw(uint objectIndex)
{
    uint base = 524944u + objectIndex * 48u;
    float4 a = asfloat(sceneData.Load4(base));  // x=totalTri y=vertOff z=rough w=metal
    ObjectDraw d;
    d.totalTriangles = asuint(a.x);
    d.vertexOffset = asuint(a.y);
    d.roughnessMetallic = float2(a.z, a.w);
    d.baseColor = asfloat(sceneData.Load4(base + 16u));
    d.emissive = asfloat(sceneData.Load4(base + 32u));  // Phase 8 ③：@32 emissive（xyz=factor）
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

// ── Phase 8 批次 ④a：完整版 IBL 辅助（forward model_pbr_ibl 与 deferred light.frag 共用）──
//
// 漫反射辐照度 = CPU 投影的 radiance SH9（scene data 尾区 @721552，9×vec3）做辐照度余弦卷积。
// 卷积常数（Karis/Green 辐照度 A 因子：π·l0 + 2π/3·l1 + π/4·l2）在求值里施加，CPU 侧只投影
// radiance（texture_pool::projectRadianceSH9，基函数与本处逐式一致，两端必须同步）。
// 基函数与 CPU shBasis 严格相同（real SH，归一化常数烘焙）。
float3 irradianceSH(float3 n)
{
    uint base = 721552u;  // kSceneDataIrradianceOffset（vkresources.cppm 字面量同步）
    // 9 × vec4 排布（.xyz=系数，.w 留白；16B 对齐满足 ByteAddressBuffer.Load4 前提）
    float3 sh0 = asfloat(sceneData.Load4(base + 0u)).xyz;
    float3 sh1 = asfloat(sceneData.Load4(base + 16u)).xyz;
    float3 sh2 = asfloat(sceneData.Load4(base + 32u)).xyz;
    float3 sh3 = asfloat(sceneData.Load4(base + 48u)).xyz;
    float3 sh4 = asfloat(sceneData.Load4(base + 64u)).xyz;
    float3 sh5 = asfloat(sceneData.Load4(base + 80u)).xyz;
    float3 sh6 = asfloat(sceneData.Load4(base + 96u)).xyz;
    float3 sh7 = asfloat(sceneData.Load4(base + 112u)).xyz;
    float3 sh8 = asfloat(sceneData.Load4(base + 128u)).xyz;
    float3 r = 3.14159265 * sh0 * 0.282095;
    r += 2.09439510 * sh1 * (0.48860251 * n.y);
    r += 2.09439510 * sh2 * (0.48860251 * n.z);
    r += 2.09439510 * sh3 * (0.48860251 * n.x);
    r += 0.78539816 * sh4 * (1.09254843 * n.x * n.y);
    r += 0.78539816 * sh5 * (1.09254843 * n.y * n.z);
    r += 0.78539816 * sh6 * (0.31539157 * (3.0 * n.z * n.z - 1.0));
    r += 0.78539816 * sh7 * (1.09254843 * n.x * n.z);
    r += 0.78539816 * sh8 * (0.54627422 * (n.x * n.x - n.y * n.y));
    return max(r, 0.0);  // SH 截断振铃（高对比 HDR 尤甚）可产生负辐照度 → 负 radiance clamp 成黑点（Filament 同款钳制）
}

// 环境镜面 split-sum 第二项 BRDF 积分（免 LUT 纹理）。返回 (F0 * A + B)：
// prefiltered env 采样值乘此即环境镜面反射（能量守恒、粗糙金属不塌黑、恒非负）。
// A/B = GGX(height-correlated Smith, Schlick (1-VoH)^5) 半球积分的二维三次多项式拟合，
// 系数由蒙特卡洛数值积分（52 万样本/格 × 29×29 网格，x=NoV、y=roughness、定义域 [0.05,1]²）
// 最小二乘得出，最大误差 0.056（掠射角角落，典型区域 <0.02）；复现脚本 tools/brdf_fit.py。
// 前版手抄 "Karis 拟合" 的 B=-a1² 项致粗糙度全域大负值（非金属像素被减去天空亮度 →
// 墙面特定角度死黑 + 地面脏噪点），复盘见 docs/调试历史.md 2026-09-13。
float3 envBrdfFactor(float NoV, float roughness, float3 F0)
{
    float x = clamp(NoV, 0.05, 1.0);
    float y = clamp(roughness, 0.05, 1.0);
    float A = (-0.117235 + 5.220972 * x + -8.006398 * x * x + 3.885701 * x * x * x)
        + (1.954888 + -14.014978 * x + 28.979388 * x * x + -16.481228 * x * x * x) * y
        + (-0.131519 + 7.607602 * x + -28.805193 * x * x + 19.973097 * x * x * x) * y * y
        + (-0.881902 + -0.051407 * x + 9.053260 * x * x + -7.915266 * x * x * x) * y * y * y;
    float B = (1.155254 + -5.110554 * x + 7.342248 * x * x + -3.416029 * x * x * x)
        + (-3.041213 + 17.121746 * x + -28.588772 * x * x + 14.698225 * x * x * x) * y
        + (2.639804 + -20.172317 * x + 38.436536 * x * x + -21.248779 * x * x * x) * y * y
        + (-0.701261 + 8.016208 * x + -17.080260 * x * x + 9.950964 * x * x * x) * y * y * y;
    return F0 * A + B;
}
