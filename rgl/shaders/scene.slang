// 场景级 bindless 数据契约（Phase 5 批次 2/3：光照 + 对象矩阵表）
//
// 单一 storage buffer（descriptor heap binding 3，Resources::ensureSceneData 创建，
// 每帧 staging → vkCmdCopyBuffer 更新，见 vkrenderer.cppm）。
// 🔴 类型化迁移（docs/constantbuffer_迁移计划.md §9）：各区逐批从「binding 3 + 手写字节
// 偏移」迁到**类型化载体**（本文件零字节数）；已迁区 = 湿面（binding 32，批 1）、
// 辐照度（binding 27，批 2）、灯数组（binding 24，批 3，消费端在 lights.slang）、
// 阴影光空间 VP（binding 28，批 S-1）、阴影采样参数（binding 29，批 S-2，含 IZB/PCSS/EVSSM）、
// SSR（binding 31，批 S-3）、TAA（binding 30，批 S-4）；
// 未迁区仍走 binding 3 字节读，**全部迁完后 binding 3 与 sceneData 声明一起删**。
// 字节布局真相源分两段（2026-09-15 勘误：旧注释称真相源 = vkresources.cppm 的
// LightDataGpu——该类型已删）：
//   · SceneLight（@0-48 头）真相源 = **本文件**——scene_forward 反射管线从本文件
//     生成 scene_forward_types_generated.cppm（逐字段 offsetof static_assert 钉死），
//     scene_data.cppm 的 kSceneDataEnvOffset 等派生自 sizeof(SceneLight) 并 static_assert
//     钉死各区偏移，shader 结构漂移 = C++ 编译失败；
//   · 其余表区（@256 矩阵表 / @263424 MVP 表 / @525824 draw 表 / @724224 簇区间表）
//     **批 T1 起已类型化迁出本文件** → `scene_tables.slang` 的四个 `StructuredBuffer`
//     （binding 23 / 26 / 25 / 33），本文件只留结构体声明（= 反射契约 + 布局唯一真相源），
//     不再有任何字节字面量。C++ 侧基址/stride 仍由 scene_data.cppm 容量常量推导 +
//     static_assert 钉死。
//   @0   direction vec3 | @12 eyePos vec3 | @24 color vec3（CPU 预乘 intensity）| @36 ambient vec3
//        （两个 intensity 语义：光色在此 CPU 预乘成最终值；env intensity 是 @48 的裸标量、
//          GPU 端乘——env 颜色在 GPU 侧无法预乘，调光两条路线各改各 config 的一个 intensity）
//   @48  env 参数（Phase 5.5，env.slang readEnvParams 消费）| @64 skyboxInvViewProj
//        mat4 列主序（Phase 5.5 批次 4，sky.slang skyClipToWorld 消费）
//   @256 对象矩阵表（批 L 起 256B 对齐，头 128B 后为对齐间隙）：mat4 列主序 ×
//        kSceneMatrixCapacity(4096)，[objectIndex] = spin·world
//        （DrawArgs.objectIndex = MeshRef.objectSlot，实例级槽，Phase 6 instancing）
//        消费端 = scene_tables.slang binding 23
//   @262656 灯数组（lights.slang 经 binding 24 类型化消费，本文件无字节读）
//   @263424 per-object MVP 表：mat4 列主序 × 4096，[objectIndex] = proj·view·world
//        （Phase 7b+ NV DGC 规避：DrawArgs 去 mvp（当时 push 36B，后 bindless 化终态 4B）
//          < NV 驱动 84B 挂死阈值；
//          CPU 每帧写，值 = 原 DrawArgs.mvp 位级一致；基址 = alignUp(256+4097×64+528,256)，
//          vkresources kSceneDataMvpTableOffset 同值——字面量同步，无反射端）
//   @525824 per-object 材质/几何表：stride 48 × 4096，[objectIndex]（Phase 7b+ 续：
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

// 表读取函数 readObjectMatrix / readObjectMvp 已迁 **scene_tables.slang**（批 T1，
// StructuredBuffer<ObjectMatrix> @binding 23 / 26）——本文件零字节数，消费端 import 它。

// per-object MVP（列主序 4 列显式读，返回 ObjectMatrix 结构，消费方用 clipPos 列组合——
// 不用 float4x4(c0,c1,c2,c3)：Slang/HLSL 该构造把入参当【行】，会把列主序内存读成转置，
// 非对称变换（带旋转的建筑）直接炸形，地面近似对称侥幸正常。clipPos 与 mul(原push mvp) 位级一致）
float4 clipPos(ObjectMatrix m, float3 p)
{
    return m.c0 * p.x + m.c1 * p.y + m.c2 * p.z + m.c3;
}


// Phase 7b+（NV DGC 规避）：per-object 材质/几何表（DrawArgs 去 mvp 后剩余字段也进 heap，
// push 仅留 objectIndex）。基址 @525824 = alignUp(263424+4097×64, 256)，stride 48。
// **Phase 10 批 10.2a 单源化**：本 struct 声明 = 表布局唯一真相源——字段声明序按
// 内存序排（scalar layout 下 0/4/8/16/32 无 padding），消费端 readObjectDraw 整读
// （编译器管偏移，无手写 Load4 链）；C API 反射（scene_forward_schema
// 登记）生成 C++ 同名 struct + 逐字段 offsetof static_assert（generated/
// scene_forward_types_generated.cppm），手写 ObjectDrawGpu 已删。
// 基址/stride 的 C++ 侧 = scene_data.cppm 由容量常量推导（static_assert 钉 525824）；
// 批 T1 起读取面在 scene_tables.slang（StructuredBuffer<ObjectDraw> @binding 25），
// 本文件不再出现该基址的字面量。
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

// readObjectDraw 已迁 **scene_tables.slang**（批 T1，StructuredBuffer<ObjectDraw> @binding 25）。

// 簇化轴 ★1-a1i：per-object 的**簇区间表**条目（scene data 尾区 @724224，stride 16，
// 容量 = kSceneMatrixCapacity，按 objectIndex 索引；基址/容量 = scene_data.cppm
// kSceneDataClusterRange* 推导 + static_assert 钉死；批 T1 起读取面在 scene_tables.slang
// 的 StructuredBuffer<ObjectClusterRange> @binding 33，本文件无字节字面量）。
// 布局走与 ObjectDraw 同一条反射路（scene_forward_schema 登记 → C API 反射生成 C++
// 同名 struct + 逐字段 offsetof static_assert），故本声明是布局唯一真相源。
// 消费者 = ★1-a1ii-α 的 meshlet.mesh.slang（`base + (groupID.x >> 1)` = 全局簇下标（★1-a1ii-α″ 去索引发射后每簇固定 2 槽：
// 半区号 = `groupID.x & 1`，每槽 ≤ 85 三角，3×85 = 255 ≤ maxMeshOutputVertices）；
// 恒 L0 档用 `l0Count`，`count` 留给 ★1-a2 全级档，见 docs/mdi_plan.md §10.1）。
// 表内容由装配层写入（门关 = 全 0），门关本区全 0 ⟹ 读取点天然零 group。
struct ObjectClusterRange
{
    uint base;      // 该 primitive 的簇区间起点（全局簇下标）
    uint count;     // 区间长度（全部 DAG 级；恒 L0 档只用前 l0Count 条）
    uint l0Count;   // 区间**前缀**里的 L0 簇条数（产物不变量：L0 恒为前缀）
    // ★1-a2-i 逐簇剔除项表的**本 object 首项下标**（= l0Count 按 objectSlot 升序的前缀和）。
    // 🔴 为什么不能拿 `base + 簇内序` 当剔除标志数组的下标：`base` 是**全局簇下标**，同资产被
    // 两个 object 复用时两者 `base` 相同 ⟹ 撞同一标志槽 = 一个对象的可见性替另一个决定
    // = **几何凭空消失**（误剔没有任何错误信号）。cullBase 按 objectSlot 前缀和 ⟹ 逐 object 唯一。
    // 本字段原为 `reserved` 补齐位（★2 选级预算）——★2 若要选级请另开字段/另表，勿与此争用。
    uint cullBase;
};

// readObjectClusterRange 已迁 **scene_tables.slang**（批 T1，StructuredBuffer<ObjectClusterRange>
// @binding 33）。原纪律（Load<T> 整读 = 反射真值 = 生成的 C++ 布局）不变，只是基址改由 binding
// 承载、步长由元素类型承载，本文件不再出现 724224 / 16 的字节字面量。

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
// 漫反射辐照度 = CPU 投影的 radiance SH9（scene data 尾区辐照度区）做辐照度余弦卷积。
// 卷积常数（Karis/Green 辐照度 A 因子：π·l0 + 2π/3·l1 + π/4·l2）在求值里施加，CPU 侧只投影
// radiance（texture_pool::projectRadianceSH9，基函数与本处逐式一致，两端必须同步）。
// 基函数与 CPU shBasis 严格相同（real SH，归一化常数烘焙）。
//
// 迁移批 2（§9.5 binding 27，先例 = 批 1 湿面）：载体 = `ConstantBuffer<IrradianceBlock>`
// 经 scene data 辐照度区**子范围 UBO 描述符**（C++ 侧 kSceneDataIrradianceOffset，
// SceneDataService::ensure 写一次）。本文件零字节字面量——9 条手写 Load4 清零，
// 字段偏移由 scalar layout + 反射契约（scene_forward_schema 登记 → 生成物
// IrradianceBlock，offsetof/sizeof assert 自带）两侧同源。
struct IrradianceBlock
{
    // 9 × vec4 排布（.xyz=系数，.w 留白；stride 16 与原 ByteAddressBuffer.Load4 的
    // 16B 对齐前提同源，布局真相源 = 本声明经反射）
    float4 sh[9];
};

[[vk::binding(27, 0)]]
ConstantBuffer<IrradianceBlock> sceneIrradiance;

// ── Phase 11 批 11.2：级联阴影（光空间 VP + 采样参数）──
//
// 迁移批 S-1（§9.5 binding 28，先例 = 批 2 辐照度）：4 槽光空间 VP 从「@722944 +
// cascade*64 手写四条 Load4」迁到 `ConstantBuffer<ShadowViewProjBlock>` 的**类型化数组**
// （载体 = scene data 阴影 VP 区子范围 UBO 描述符，C++ 侧 kSceneDataShadowMatrixOffset，
// SceneDataService::ensure 写一次）。字段偏移/总尺寸由 scalar layout + 反射契约
// （scene_forward_schema 登记 → 生成物 ShadowViewProjBlock，sizeof/offsetof assert 自带）
// 两侧同源，本函数零字节数。
//
// 元素类型 = 本文件的 `ObjectMatrix`（列主序 4 列）——与原手写读**逐字节同构**：
// 原来 Load4(base)/+16/+32/+48 依次填 c0..c3，现在数组元素就是同一个 ObjectMatrix，
// 元素步长 64 = 4×vec4（scalar layout 下无 padding），故 cascade 索引即原 cascade*64。
struct ShadowViewProjBlock
{
    ObjectMatrix viewProj[4];  // kSceneShadowCascadeCount = 4 槽，索引 = 级联号
};

[[vk::binding(28, 0)]]
ConstantBuffer<ShadowViewProjBlock> sceneShadowViewProj;

ObjectMatrix readShadowViewProj(uint cascadeIndex)
{
    return sceneShadowViewProj.viewProj[cascadeIndex];
}

// ── 阴影采样小参数（迁移批 S-2，§9.5 binding 29）──
//
// 原形态 = 7 个各 16B 的独立小区（splits/texel/params/atlas 连排 + IZB/PCSS/EVSSM/EVSSM
// 常量各占一个 256 台阶，共吃 1024B）。类型化后合并成**一个** `ShadowParamsBlock`
// （128B = 8×vec4，字段序 = 原物理序），载体 = scene data 阴影参数区子范围 UBO 描述符
// （C++ 侧 kSceneDataShadowParamsOffset）。字段偏移由反射契约钉死（scene_forward_schema
// 登记 → 生成物 ShadowParamsBlock，sizeof/offsetof assert），本文件零字节数；写端
// （writeSceneStaging）按同一个生成 struct 整块 memcpy ⟹ 两侧不可能各写一份偏移。
//
// 合并顺带把其后各区往下搬 768B（3 个 256 台阶的对齐间隙），搬家口径见 §9.7。
struct ShadowParamsBlock
{
    float4 splits;      // 级联分界远距（对 eyePos 的平方距离，选级用）
    float4 texelSizes;  // 各级阴影图 texel 世界尺寸（2·r/mapSize，normal offset 用）
    float4 params;      // (depthBias, normalOffsetScale, enabled, 保留)
    float4 atlas;       // (图集边长, 单块边长, 分块列数, 保留)
    float4 izb;         // IZB 消费区 (开关 1/0, 像素 W, 像素 H, 保留)——light.frag 消费
    float4 pcss;        // PCSS (半影全宽系数 2·tan(θ/2), blocker 搜索半径, 半影半径上限, 保留)
    float4 evssm;       // EVSSM (半坡 tan(θ/2), 搜索半径, 半影上限, 矩链最大 mip)
    float4 evssmConst;  // (x = EVSM 指数 c, y 留白, z = 核宽下限[图集纹素], w 留白)
};

[[vk::binding(29, 0)]]
ConstantBuffer<ShadowParamsBlock> sceneShadowParams;

float4 readCascadeSplits()
{
    return sceneShadowParams.splits;
}

float4 readCascadeTexelSizes()
{
    return sceneShadowParams.texelSizes;
}

// (x = NDC 深度偏置、y = normal offset 系数（以 texel 为单位）、z = 总开关、w 保留。
// 关闭时仍读一下（uniform 读代价极低）但不取抽样 —— 图仍在图里、布局链完整
// （否则会报"采样未转换布局的图"）。
float4 readShadowParams()
{
    return sceneShadowParams.params;
}

// (图集边长, 单块边长, 分块列数, 保留)——采样端把级联块的光空间 uv 叠到图集上
// 用。两个长度都从 C++ 传下来（不在本文件写死）——改图集尺寸时只要改 C++ 一处。
float4 readShadowAtlas()
{
    return sceneShadowParams.atlas;
}

// IZB 消费区（批 11.3-1e；light.frag 的 -DIZB_CONSUME 变体消费，原版 SPV 不读）。
// 迁移批 S-2 起本函数取代 light.frag 里手抄的 `sceneData.Load4(723456u)`——字面量只此一处。
float4 readShadowIzb()
{
    return sceneShadowParams.izb;
}

// 批 11.5 PCSS 参数（字段语义见上面 struct）：
// 只有 -DPCSS_SOFT 变体引用（shadow.slang），原版 SPV 不读本块。
float4 readShadowPcss()
{
    return sceneShadowParams.pcss;
}

// 批 11.6 EVSSM 参数（字段语义见上面 struct）：
// 只有 -DEVSSM_SOFT 变体引用（shadow.slang），原版 SPV 不读本块（门关红线构造性保证）。
float4 readShadowEvssm()
{
    return sceneShadowParams.evssm;
}

// 批 11.6 EVSSM 常量 vec4（x = EVSM 指数 c，yzw 留白）。
// c 由 host kEvssmExponent 下发——生产/消费/fallback 三方同源，此处禁写字面量。
float readShadowEvssmExponent()
{
    return sceneShadowParams.evssmConst.x;
}

// 滤波核宽下限（批 N6d，evssmConst.z；单位 = 图集纹素，0 = 不设下限）。
// ⚠ evssmConst.y 曾为「一致性闸门容差」，实测净负作用（foot=32 时覆盖率比 0.845→0.526）已删，
//   槽位保留为 0 不动布局 —— 理由与核宽权衡表见 shadow.slang STEP 2.5 注释。
// 出厂 0.53° 下半影 < 1 纹素 ⟹ 抽点半径 0.5 纹素 ⟹ 5 抽点全落同一双线性邻域（实测单抽头
// 变异与基线逐位相同）⟹ 核没张开、软度无从谈起。归因 → docs/shadow_routes_plan.md §4.2。
float readShadowEvssmFootprint()
{
    return sceneShadowParams.evssmConst.z;
}

float3 irradianceSH(float3 n)
{
    // 迁移批 2：9 条手写 Load4 字节字面量清零，读 binding 27 的 IrradianceBlock（.xyz =
    // 系数、.w 留白，与原排布逐字节同）；卷积算术一个 bit 未动。
    float3 sh0 = sceneIrradiance.sh[0].xyz;
    float3 sh1 = sceneIrradiance.sh[1].xyz;
    float3 sh2 = sceneIrradiance.sh[2].xyz;
    float3 sh3 = sceneIrradiance.sh[3].xyz;
    float3 sh4 = sceneIrradiance.sh[4].xyz;
    float3 sh5 = sceneIrradiance.sh[5].xyz;
    float3 sh6 = sceneIrradiance.sh[6].xyz;
    float3 sh7 = sceneIrradiance.sh[7].xyz;
    float3 sh8 = sceneIrradiance.sh[8].xyz;
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
// 墙面特定角度死黑 + 地面脏噪点），复盘见 git:调试历史 2026-09-13。
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

// ── 屏幕 ↔ 世界重建（Phase 14 批 14.4 自 ao.frag / taa.frag 抬升为单一来源）──
//
// 为什么抬上来：`clipToWorld` / `worldFromDepth` 此前在 ao.frag 与 taa.frag 里**逐字相同
// 各存一份**（本批实测 diff 过），速度场是第三家。而速度场与 TAA 重投影的正确性判据恰恰是
// 「两者必须互逆一致」——三份拷贝意味着"改了两处忘第三处"不报错、画面照样动，正是本项目
// 反复踩的漂移形态（同 `readAoStrength` 从 tonemap 局部函数抬进本模块的理由）。
// 抬升 = 逐字搬移，一个算术 bit 未动；红线 = `just shot` 与基线逐位相同（已实测）。
//
// ⚠ 口径（改任何一行都要同步 `rgl/velocity_field.cppm` 的 CPU 侧镜像与它的单测）：
//   · ndc.x = uv.x·2−1，**ndc.y = 1 − uv.y·2**（framebuffer uv 原点左上、NDC 原点左下）；
//   · ndc.z = Vulkan 深度 [0,1] 直接用（0 = near、1 = far，`GLM_FORCE_DEPTH_ZERO_TO_ONE`）；
//   · 用的矩阵是 sceneData@64 = inverse(proj·view) 的**抖动后**口径（与深度缓冲配套）。

// ObjectMatrix（列 c0..c3）× 列向量：clip/M 变换一律"列组合"写法（同 sky/ao 的
// clipToWorld 惯例；勿按行主序点乘——ObjectMatrix 存的是列）。
float4 mulM(ObjectMatrix m, float4 v)
{
    return m.c0 * v.x + m.c1 * v.y + m.c2 * v.z + m.c3 * v.w;
}

// clip → world（sceneData@64 = inverse(proj·view)，**抖动后**口径；字面量 64/80/96/112
// 与 scene_data.cppm kSceneDataSkyOffset + 16*i 一致）
float4 clipToWorld(float4 clip)
{
    float4 c0 = asfloat(sceneData.Load4(64));
    float4 c1 = asfloat(sceneData.Load4(80));
    float4 c2 = asfloat(sceneData.Load4(96));
    float4 c3 = asfloat(sceneData.Load4(112));
    return c0 * clip.x + c1 * clip.y + c2 * clip.z + c3 * clip.w;
}

// 屏幕 uv + 深度 → 世界坐标（y 翻转，Vulkan 深度直接用）
float3 worldFromDepth(float2 uv, float depth)
{
    const float4 clip = clipToWorld(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
    return clip.xyz / clip.w;
}

// clip → **framebuffer uv**（`worldFromDepth` 的逆方向；y 翻转互逆：uv.y = (1 − ndc.y)/2）。
// w ≤ 0（相机背后）时结果无意义，调用方须自行判——本函数不判，因为各消费者的拒收形态不同
// （taa.frag 直接输出本帧、velocity 写零）。刻意收 clip 而非世界点：调用方本来就要先拿
// clip 判 w，这样复用**不引入第二次矩阵乘**（taa.frag 换用它时算术逐位不变）。
float2 uvFromClip(float4 clip)
{
    const float2 ndc = clip.xy / clip.w;
    return float2(ndc.x * 0.5 + 0.5, (1.0 - ndc.y) * 0.5);
}

// 任意矩阵 × 世界点 → framebuffer uv（一步版；无需判 w 的调用方用这个）
float2 uvFromWorld(float3 world, ObjectMatrix m)
{
    return uvFromClip(mulM(m, float4(world, 1.0)));
}

// octahedral 解码（与 deferred_geometry.frag octEncode 同式自反；McTaggart **标准形
// +signNotZero**——折叠乘子两端必须一致。旧 −signNotZero 在赤道带 z≈0⁻ 产生 180°
// 翻转，2026-09-15 勘误）
//
// 批 15.1 后半自 light.frag 抬入本段（与 clipToWorld/worldFromDepth 同因、同法）：
// 它同属「从 G-buffer/深度恢复几何量」一族，而消费者从 light.frag 一个变成 light.frag +
// ssr.frag 多个——逐字各存一份正是上面段头写明的漂移形态（改了两处忘第三处不报错、
// 画面照样动）。抬升 = 逐字搬移，一个算术 bit 未动；红线 = `just regress ab` 逐位相同。
float3 octDecode(float2 e)
{
    float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    return normalize(n);
}

// ── 批 12.6 TAA（时域抗锯齿）尾区（迁移批 S-4 起 = 类型化载体，本文件零字节偏移）──
//
// 载体 = binding 30 的 `ConstantBuffer<TaaParamsBlock>`（§9.5 定案表给 TAA 区的号），物理
// 存储仍是 scene data 单 buffer 的 TAA 区（C++ 侧 kSceneDataTaaOffset 子范围描述符）。六段
// 本就同区连排（192B）⟹ 本批**零布局变更**；字段偏移只存在于反射生成物一处（写端
// writeSceneStaging 按同一个生成 struct 整块 memcpy ⟹ 两侧不可能各写一份偏移）。
//
// 字段语义（口径未变，全量推导 → scene_data.cppm kSceneDataTaa* 区注释）：
//   prevViewProj     上一帧**抖动后** proj·view（产出**抖动网格**坐标）
//   motion           prevSpin · inverse(curSpin)（spin 关 = 单位阵）
//   params           (feedback, historyValid, 本帧内容位移 c_t 的 x/y px)
//   prevContentShift (上一帧内容位移 c_{t−1} 的 x/y px, **AO 已施加标志** z, 保留) —— 13.5/13.6
//   inputKernel      (输入侧重建核选择 x, yzw 保留) —— 批 13.8，默认 0 = 双线性 = 逐位今天
//   clipControl      (历史裁剪消融门 x, 软裁剪 λ y, zw 保留) —— 批 13.11，默认 0 = 逐位今天
//
// 矩阵口径提醒（改动前必读）：
//   · 重建世界坐标用 sceneData@64 的 skyInvViewProj，它是**抖动后** proj·view 的逆（与深度配套）；
//   · 投回上一帧用这里的 prevViewProj（**也是抖动后**），产出**抖动网格**坐标；
//   · 批 13.5 起历史住**基空间** ⟹ 上一步的抖动网格坐标要**减 c_{t−1}** 才对齐历史。
//     ⚠ 这三条**成对**：prevViewProj 的口径与「减不减 c_{t−1}」必须二选一配套，
//       都做或都不做都会让静止机位 0.667px 游动原样回来。
// historyValid = 0 时 taa.frag 直接输出本帧（逐位旁路，首帧/resize/跳切走这条）。
struct TaaParamsBlock
{
    ObjectMatrix prevViewProj;  // @0  列主序 4 列（§9.4：契约里的矩阵一律 ObjectMatrix）
    ObjectMatrix motion;        // @64 列主序 4 列
    float4 params;              // @128
    float4 prevContentShift;    // @144
    float4 inputKernel;         // @160
    float4 clipControl;         // @176
};

[[vk::binding(30, 0)]]
ConstantBuffer<TaaParamsBlock> sceneTaa;

ObjectMatrix readTaaPrevViewProj()
{
    return sceneTaa.prevViewProj;
}

ObjectMatrix readTaaMotion()
{
    return sceneTaa.motion;
}

// ── Phase 15 批 15.1 SSR 尾区（迁移批 S-3 起 = 类型化载体，本文件零字节偏移）──
//
// 载体 = binding 31 的 `ConstantBuffer<SsrParamsBlock>`（§9.5 定案表给 SSR 区的号），物理
// 存储仍是 scene data 单 buffer 的 SSR 区（C++ 侧 kSceneDataSsrOffset 子范围描述符）。
// 三段合并成一个契约 struct ⟹ 原来「只有 C++ 那一侧有 static_assert 钉、shader 侧漂移
// 只能靠端到端判据 just ssr-check 抓」的**单向钉**换成两侧同源（生成物逐字段 offsetof
// assert + 写端整块 memcpy）。区内布局（字节 = 契约字段偏移，反射真值）：
//   @0   mat4 viewProj —— 本帧**抖动后** proj·view（与 @64 skyInvViewProj 严格互逆、
//   与深度缓冲配套；SSR 命中点投回屏幕 uv 用正向矩阵，消费方配 mulM/uvFromWorld 列组合惯例）。
//   @64  vec4 params —— (sceneRadius, maxDistanceRadii, thicknessPx, biasPx)：
//   x = SceneBounds.radius（世界空间场景并集包围球半径 = 尺度无关参数锚点），yzw = 由它
//   导出的三个无量纲系数（configs/runtime.lua `ssr` 块）。
//   @80  vec4 gate —— (roughFull, roughCutoff, blurScale, 保留)：x/y = 粗糙度门控区间
//   （与场景尺度无关，故单独一槽；分组理由 → scene_data.cppm 同槽注释）；z = 螺旋模糊半径
//   系数（批 15.3 子步 B，ssr_apply 消费，全分辨率 px 口径）。
// 为什么是 CPU 直传正向矩阵而不是 shader 里 inverse(@64)：inverse(inverse(M)) 与 M 无位级
// 契约，而 SSR 的 uv 精度直接进 ray march 采样位置（全量推导 → scene_data.cppm 同槽注释）。
struct SsrParamsBlock
{
    ObjectMatrix viewProj;  // 本帧抖动后 proj·view（列主序 4 列，同阴影 VP 惯用法）
    float4 params;          // (sceneRadius, maxDistanceRadii, thicknessPx, biasPx)
    float4 gate;            // (roughFull, roughCutoff, blurScale, 保留)
};

[[vk::binding(31, 0)]]
ConstantBuffer<SsrParamsBlock> sceneSsr;

ObjectMatrix readSsrViewProj()
{
    return sceneSsr.viewProj;
}

// SSR 尺度锚点与三个导出系数（x = SceneBounds.radius，空场景 = 0；yzw = maxDistanceRadii /
// thicknessPx / biasPx）。消费者 = ssr.frag；口径与兜底责任 → scene_data.cppm SSR 区注释。
// **系数无 shader 侧默认值**（唯一真相源 = RuntimeConfig）。
float4 readSsrParams()
{
    return sceneSsr.params;
}

// SSR 粗糙度门控 + 模糊旋钮（x = roughFull、y = roughCutoff、z = blurScale、w 保留）。
// x/y 消费者 = ssr_trace.frag 的 `1 - smoothstep(x, y, roughness)`；⚠ 要求 x < y（反了 =
// 门控反向），不变式落点与「配置层为何不校验」→ scene_data.cppm SSR 区注释。
// z 消费者 = ssr_apply.frag 的螺旋模糊半径（roughness × z，**全分辨率 px**；0 = 消融档 =
// 纯双线性上采样）。批 15.3 子步 B 起占用（此前措辞「zw 保留」，w 仍保留）。
float4 readSsrGate()
{
    return sceneSsr.gate;
}

// ── Phase 15 批 15.2 子步 A 湿面区（迁移批 1 起 = 类型化载体，本文件零字节偏移）──
//
// 载体 = binding 32 的 `ConstantBuffer<WetParamsBlock>`（§9.5 定案表给湿面区的号）。
// 物理存储仍是 scene data 单 buffer 的湿面区（C++ 侧 kSceneDataWetOffset 子范围描述符，
// SceneDataService::ensure 写一次），但**本文件不再有任何字节字面量**——字段偏移由
// scalar layout + 反射契约（scene_forward_schema 登记 → scene_forward_types_generated）
// 两侧同源，C++ 写侧子区偏移与 struct 字段偏移的同步由生成物 static_assert 兜底。
// 湿面**身份**（哪些面湿）不占参数 = `1 - smoothstep(readSsrGate().x, readSsrGate().y,
// roughness)`，与 SSR 粗糙度门控同区间同源（结构上排除「反射了但没压暗」的错配；
// 理由 → scene_data.cppm 湿面区注释）。
// 消费者 = deferred_geometry.frag.slang 湿面 G-buffer 调制（15.2B 已接线，默认开；
// 消融档 `--set strength=0` 实测逐位 = 换锚前旧图）。
// **系数无 shader 侧默认值**（唯一真相源 = WetTuning 成员初始化器，rgl/runtime_config.cppm）。
struct WetParamsBlock
{
    float4 params;   // (strength, dark, roughLo, roughHi) = 身份调制系数（@0）
    float4 spatial;  // (rippleAmp, puddleFreqRadii, 保留, 保留) = 空间变化参数（@16）
};

[[vk::binding(32, 0)]]
ConstantBuffer<WetParamsBlock> sceneWet;

float4 readWetParams()
{
    return sceneWet.params;
}

// 湿面空间变化参数（x = rippleAmp 水膜法线扰动幅度、y = puddleFreqRadii 水洼噪声频率
// = 本值 / sceneRadius，尺度无关口径同 ssrParams.x 的锚点；zw 保留）。消费者 = deferred
// _geometry 湿面 G-buffer 调制（15.2B 已接线）；⚠ wetParams 的 z < w（roughLo < roughHi，反了 = 空间
// 调制方向倒置），不变式落点与「配置层为何不校验」→ scene_data.cppm kSceneDataWetParamsOffset。
float4 readWetSpatial()
{
    return sceneWet.spatial;
}

// AO 施加强度：scene data @60，configs/scene.lua ao.strength，0 = 逐位旁路。
// ⚠ 自批 13.6 起有**两个**消费者（taa.frag 施加 / tonemap 按标志施加），故从 tonemap 的
//   局部函数**抬进本模块**——同一个 @60 字面量写两处必然漂移（本项目反复踩过）。
float readAoStrength()
{
    return asfloat(sceneData.Load(60u));
}

// (x = feedback 本帧权重, y = historyValid 1/0, z/w = **本帧内容位移 c_t** 像素)。
// ⚠ zw 自批 13.5 起被 taa.frag 真读（输入侧 un-jitter）；此前装 jitterPx 但从未被读。
// 口径 = `taa_jitter.cppm::contentShiftPx` = `(+jx, −jy)`，**不是** jitterPx（差一个 y 轴翻号）。
float4 readTaaParams()
{
    return sceneTaa.params;
}

// 批 13.5 历史基空间修正：**上一帧内容位移 c_{t−1}** 像素（x/y）+ 批 13.6 AO 施加点标志（z）。
// 用途：prevViewProj 产出的是抖动网格坐标，减本值才落到基空间历史网格。
// 语义与「为什么偏移由 CPU 算好」全在 scene_data.cppm TAA 区 `prevContentShift` 字段注释里。
float4 readTaaPrevContentShift()
{
    return sceneTaa.prevContentShift;
}

// 批 13.6：AO 是否**已在 tonemap 之前施加过**（1/0）。= 1 ⟹ tonemap 必须跳过自己的 AO
// 乘法，否则双施；= 0 ⟹ tonemap 照旧施加（没有更早的施加点时的唯一施加点）。
// 施加者有两个（批 14.5 第三批起）：**taa pass**（批 13.6，乘在时域滤波之前）或专用的
// **ao_apply pass**（与"谁做时域"解耦，FSR 当时域滤波器时用它）。两者互斥。
// 名字从 `readTaaAoApplied` 改成本名正因此——旧名在只有 taa 一个来源时对，现在会误导。
// ⚠ 该标志由 CPU 侧写**实际图形态**（decorateTaaPass 的返回值），**不是**配置键 taa.enabled
//   ——offscreen 变体配置开了 TAA 也装饰失败，若拿配置派生就会「taa pass 不存在 + tonemap
//   又跳过」= AO 一次都不施加。零布局变更（复用 .z 保留位，static_assert 不变）。
float readAoAppliedPreTemporal()
{
    return sceneTaa.prevContentShift.z;
}

// 历史拒收诊断门标志（`taa.diag_reject`，1/0，默认 0 = 今天）。= 1 ⟹ taa.frag 把逐像素
// 「历史怎么了」码写进输出 alpha（0 收下 / 0.25 裁剪 / 0.5 盒拒收 / 0.75 重投影拒收 /
// 1 无历史），RGB 一个 bit 不动。回读 = `POST /shot {"attachment":"taa_out"}`。
// 零布局变更（复用 .w 保留位，static_assert 不变）。
float readTaaDiagReject()
{
    return sceneTaa.prevContentShift.w;
}

// 输入侧 un-jitter 的**重建核选择**（`taa.input_kernel`，默认 0 = 双线性 = 逐位今天）。
// 0 = 双线性（4 抽头，权重恒 ∈[0,1]）｜ 1 = Catmull-Rom 4×4（与历史侧同一份权重实现）｜
// 2 = Lanczos3 6×6（Filament 输入侧用的那个）。消费端 = taa.frag.slang `sampleInputColor`。
// ⚠ 本区**新增** 16B（@723616，kSceneDataTaaBytes 160 → 176）——前四个槽的四个分量
//   全部有主（feedback/historyValid/c_t.xy、c_{t−1}.xy/aoApplied/diagReject），无处复用。
//   门关（0）⟹ taa.frag 走双线性分支，一个 bit 不动（红线由 `just shot` sha256 钉）。
float readTaaInputKernel()
{
    return sceneTaa.inputKernel.x;
}

/**
 * 历史裁剪消融门（`taa.clip_mode` / `taa.clip_soft`，批 13.11）。
 * x = clip_mode（0 现行 = **逐位今天** / 1 关盒 / 2 绕 centroid 扩 1.5× / 3 box9 / 4 软裁剪），
 * y = 软裁剪 λ（仅 mode=4 读；**无量纲** = 每帧最多走完「到盒边界距离」的 λ 分之一）。
 * 消费端 = taa.frag.slang 历史裁剪段。为什么这条门存在（clip 在 blend 之前 ⟹ 一次裁剪 =
 * 一次历史重置 ⟹ 相位依赖方差 100% 注入输出）→ `scene_data.cppm` 同槽注释与
 * `git:调试历史` 2026-10-06。
 */
float4 readTaaClipControl()
{
    return sceneTaa.clipControl;
}
