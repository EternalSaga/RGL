// lights.slang — Phase 7 D3：多灯数据契约（第 4 条反射管线）
//
// scene data buffer 尾部**灯数组区**的单一真相源。
// forward 三支 frag（model_pbr_ibl/model_light/model_vegetation）与 deferred light.frag
// 均先 readLightCount()：>0 时逐灯经本文件读 GpuLight[] 累加；count=0 回落
// scene.slang readSceneLight() 的 @0 头单灯（旧默认场景无显式 Light entity 时的直接光）。
// GpuLight struct + kMaxLights 经反射管线生成 C++ struct（generated/lights_types_generated.cppm），
// 替换了旧 LightDataGpu 手工双源债（该类型已删，@0 头 = 反射生成的 SceneLight）。
//
// 🔴 迁移批 3（§9.5 binding 24，先例 = 批 1 湿面 / 批 2 辐照度）：载体 =
// `ConstantBuffer<LightArrayBlock> lightArray` 经灯数组区**子范围 UBO 描述符**
// （C++ 侧 kSceneDataLightArrayOffset，SceneDataService::ensure 写一次）。本文件
// **一个字节数都不写**——旧 kSceneDataLightArrayBase 字面量与手写 +16/+i*64 偏移清零。
// LightArrayBlock = **嵌套 struct + 嵌套 struct 数组**（嵌本文件的 GpuLight），是批 R
// 反射工具嵌套支持（§9.4"矩阵/灯必须能嵌进契约"）的实战阳性锚点：嵌套支持是假的，
// 本文件根本生成不出/编不过。布局真相源 = 本 struct 经反射（lights_schema 登记 →
// lights_types_generated，逐字段 offsetof static_assert）；count@0 + pad + lights@16
// 与旧字节布局逐字节同（旧注释：@base+0 count、@base+16 GpuLight[count]）。

// ---- 契约常量（schema 单一真相源生成 C++ constexpr；shader 端字面量必须一致）----
static const uint kMaxLights = 8u;

// ---- 单灯（float4 对齐，stride=64）----
struct GpuLight
{
    float4 direction;  // dir 光：世界方向（归一化，照射向）；point 光未用
    float4 position;   // point 光：世界位置；dir 光未用
    float4 color;      // 光色 × intensity（CPU 预乘，同 LightDataGpu.color 语义）
    float4 params;     // .x = point 衰减半径（dir 光 = 0 表示无衰减），.y = 类型（0=dir, 1=point）
};

// 灯数组块（528B = count@0 + pad 补齐到 16 + 8×GpuLight@16；.pad = 旧布局 16B 头的
// 对齐留白转正，语义不变）
struct LightArrayBlock
{
    uint count;  // 有效灯数（≤ kMaxLights）
    uint pad[3];
    GpuLight lights[kMaxLights];
};

[[vk::binding(24, 0)]]
ConstantBuffer<LightArrayBlock> lightArray;

uint readLightCount()
{
    return lightArray.count;
}

GpuLight readGpuLight(uint i)
{
    return lightArray.lights[i];
}
