// 场景级 bindless 数据契约（Phase 5 批次 2/3：光照 + 对象矩阵表）
//
// 单一 storage buffer（descriptor heap binding 3，Resources::ensureSceneData 创建，
// 每帧 staging → vkCmdCopyBuffer 更新，见 vkrenderer.cppm）。
// 字节布局真相源分两段（2026-09-15 勘误：旧注释称真相源 = vkresources.cppm 的
// LightDataGpu——该类型已删）：
//   · SceneLight（@0-48 头）真相源 = **本文件**——scene_forward 反射管线从本文件
//     生成 scene_forward_types_generated.cppm（逐字段 offsetof static_assert 钉死），
//     scene_data.cppm 的 kSceneDataEnvOffset 等派生自 sizeof(SceneLight) 并 static_assert
//     钉死各区偏移，shader 结构漂移 = C++ 编译失败；
//   · 其余表区（@128 矩阵表 / @262272 灯数组 / @262800 MVP 表 / @524944 draw 表）
//     字节字面量与 scene_data.cppm kSceneData* 手工一致（灯数组基址经 lights_schema 常量
//     三方 static_assert 钉死；draw 表自 Phase 10 批 10.2a 起 = 生成 ObjectDraw 布局
//     static_assert + 基址 524944 钉死断言，MVP 表仍仅对齐 assert；改字面量须两端同步）。
//   @0   direction vec3 | @12 eyePos vec3 | @24 color vec3（CPU 预乘 intensity）| @36 ambient vec3
//        （两个 intensity 语义：光色在此 CPU 预乘成最终值；env intensity 是 @48 的裸标量、
//          GPU 端乘——env 颜色在 GPU 侧无法预乘，调光两条路线各改各 config 的一个 intensity）
//   @48  env 参数（Phase 5.5，env.slang readEnvParams 消费）| @64 skyboxInvViewProj
//        mat4 列主序（Phase 5.5 批次 4，sky.slang skyClipToWorld 消费）
//   @128 对象矩阵表：mat4 列主序 × kSceneMatrixCapacity(4096)，[objectIndex] = spin·world
//        （DrawArgs.objectIndex = MeshRef.objectSlot，实例级槽，Phase 6 instancing）
//   @262272 灯数组（lights.slang）
//   @262800 per-object MVP 表：mat4 列主序 × 4096，[objectIndex] = proj·view·world
//        （Phase 7b+ NV DGC 规避：DrawArgs 去 mvp（当时 push 36B，后 bindless 化终态 4B）
//          < NV 驱动 84B 挂死阈值；
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
// push 仅留 objectIndex）。基址 @524944 = 128+4096×64+528+4096×64，stride 48。
// **Phase 10 批 10.2a 单源化**：本 struct 声明 = 表布局唯一真相源——字段声明序按
// 内存序排（scalar layout 下 0/4/8/16/32 无 padding），readObjectDraw 直接
// Load<ObjectDraw>（编译器管偏移，无手写 Load4 链）；C API 反射（scene_forward_schema
// 登记）生成 C++ 同名 struct + 逐字段 offsetof static_assert（generated/
// scene_forward_types_generated.cppm），手写 ObjectDrawGpu 已删。
// 基址/stride 的 C++ 侧 = scene_data.cppm 由容量常量推导（static_assert 钉 524944）；
// 本文件字面量与其手工一致（同 kTrianglesPerGroup 纪律）。
//   @0  uint totalTriangles | uint vertexOffset | float roughness | float metallic
//   @16 float4 baseColor
//   @32 float4 emissive（Phase 8 ③：xyz=emissiveFactor；Phase 8 C1：w=MASK alphaCutoff，0 = 不测）
struct ObjectDraw
{
    uint totalTriangles;
    uint vertexOffset;
    float2 roughnessMetallic;  // x=roughness y=metallic（@8，声明序 = 内存序，勿与 baseColor 对调）
    float4 baseColor;
    float4 emissive;  // Phase 8 ③：xyz=emissiveFactor（model_pbr_ibl.frag 叠加自发光）

    // Phase 8 C1：alpha-test cutout（MASK 材质 w=alphaCutoff>0；其余材质 w=0 = 恒不 discard，
    // 非 MASK 路径逐位不变）。采样后、着色前调用（discard 后 derivatives 仅余存活 lane）。
    bool alphaDiscard(float alpha)
    {
        return emissive.w > 0.0 && alpha < emissive.w;
    }
};

ObjectDraw readObjectDraw(uint objectIndex)
{
    // 10.2a：Load<ObjectDraw> 整读（编译器按 scalar layout 定字段偏移 = 反射真值 =
    // 生成的 C++ 布局）；此前手写 Load4 + asuint 链与 struct 声明是双份知识。
    return sceneData.Load<ObjectDraw>(524944u + objectIndex * 48u);
}

float3 worldPos(ObjectMatrix m, float3 p)
{
    return (m.c0 * p.x + m.c1 * p.y + m.c2 * p.z + m.c3).xyz;
}

float3 worldNormal(ObjectMatrix m, float3 n)
{
    return normalize(m.c0.xyz * n.x + m.c1.xyz * n.y + m.c2.xyz * n.z);
}

// Phase 8 方案 A：object-space 切线 → 世界（与 worldNormal 同款 mat3 直乘约定——
// 均匀缩放假设一致，见 AGENTS.md Phase 5 已知项；手性 w 原样透传）
float4 worldTangent(ObjectMatrix m, float4 t)
{
    return float4(normalize(m.c0.xyz * t.x + m.c1.xyz * t.y + m.c2.xyz * t.z), t.w);
}

// ── Phase 8 批次 ④a：完整版 IBL 辅助（forward model_pbr_ibl 与 deferred light.frag 共用）──
//
// 漫反射辐照度 = CPU 投影的 radiance SH9（scene data 尾区 @721552，9×vec3）做辐照度余弦卷积。
// 卷积常数（Karis/Green 辐照度 A 因子：π·l0 + 2π/3·l1 + π/4·l2）在求值里施加，CPU 侧只投影
// radiance（texture_pool::projectRadianceSH9，基函数与本处逐式一致，两端必须同步）。
// 基函数与 CPU shBasis 严格相同（real SH，归一化常数烘焙）。
// ── Phase 11 批 11.2：级联阴影（光空间 VP + 采样参数）──
//
// 尾区基址 @721696（= 辐照度区尾，与 scene_data.cppm kSceneDataShadowMatrixOffset
// 钉死两侧）。区内布局（本文件字面量 = C++ 侧手写同源，改一侧必编译失账）：
//   @721696 + cascade*64  mat4 光空间 VP（kSceneShadowCascadeCount = 4 槽）
//   @721952  vec4 级联分界远距（对 eyePos 的平方距离，选级用）
//   @721968  vec4 各级阴影图 texel 世界尺寸（2·r/mapSize，normal offset 用）
//   @721984  vec4 (depthBias, normalOffsetScale, enabled, 保留)
//   @722000  vec4 (图集边长, 单块边长, 分块列数, 保留)
ObjectMatrix readShadowViewProj(uint cascadeIndex)
{
    uint base = 721696u + cascadeIndex * 64u;
    ObjectMatrix m;
    m.c0 = asfloat(sceneData.Load4(base));
    m.c1 = asfloat(sceneData.Load4(base + 16u));
    m.c2 = asfloat(sceneData.Load4(base + 32u));
    m.c3 = asfloat(sceneData.Load4(base + 48u));
    return m;
}

float4 readCascadeSplits()
{
    return asfloat(sceneData.Load4(721952u));
}

float4 readCascadeTexelSizes()
{
    return asfloat(sceneData.Load4(721968u));
}

// (x = NDC 深度偏置、y = normal offset 系数（以 texel 为单位）、z = 总开关、w 保留。
// 关闭时仍读一下（uniform 读代价极低）但不取抽样 —— 图仍在图里、布局链完整
// （否则会报"采样未转换布局的图"）。
float4 readShadowParams()
{
    return asfloat(sceneData.Load4(721984u));
}

// (图集边长, 单块边长, 分块列数, 保留)——采样端把级联块的光空间 uv 叠到图集上
// 用。两个长度都从 C++ 传下来（不在本文件写死）——改图集尺寸时只要改 C++ 一处。
float4 readShadowAtlas()
{
    return asfloat(sceneData.Load4(722000u));
}

// 批 11.5 PCSS 参数区（kSceneDataPcssOffset 字面量同步）：
// (半影全宽系数 2·tan(θ/2), blocker 搜索半径[图集纹素], 半影半径上限[图集纹素], 保留)。
// 只有 -DPCSS_SOFT 变体引用（shadow.slang），原版 SPV 不读本区。
float4 readShadowPcss()
{
    return asfloat(sceneData.Load4(722032u));
}

// 批 11.6 EVSSM 参数区（kSceneDataEvssmOffset 字面量同步）：
// (半坡 tan(θ/2), 搜索半径[图集纹素 mip0 域], 半影半径上限[图集纹素], 矩链最大 mip)。
// 只有 -DEVSSM_SOFT 变体引用（shadow.slang），原版 SPV 不读本区（门关红线构造性保证）。
float4 readShadowEvssm()
{
    return asfloat(sceneData.Load4(722048u));
}

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
