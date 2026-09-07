// lights.slang — Phase 7 D3：多灯数据契约（第 4 条反射管线）
//
// scene data buffer（binding 3，import scene）尾部**灯数组区**的单一真相源。
// forward 灯路径仍走 scene.slang readSceneLight()（读 LightDataGpu@0 = 主光，字节布局不变
// → forward 回归 diff0）；deferred light pass 经本文件逐灯读灯数组累加。
// GpuLight struct + kMaxLights 经反射管线生成 C++ struct（generated/lights_types_generated.cppm），
// 替换 LightDataGpu 手工双源债（AGENTS 已知问题）。
//
// 布局（字节偏移真相源 = vkresources.cppm kSceneDataLightArray*，字面量手工一致，scene.slang 先例）：
//   @base + 0   uint lightCount（有效灯数，≤ kMaxLights）
//   @base + 16  GpuLight[count]（stride = kGpuLightStride = 64，全 Load4 读）
//   base = 头(128) + 对象矩阵表(kSceneMatrixCapacity·64) —— 追加于矩阵表之后，
//   既有 @0-128 头（含 env@48 / sky@64）逐字节不动 → forward/env/sky shader 零改动。

import scene;  // sceneData（binding 3）

// ---- 契约常量（schema 单一真相源生成 C++ constexpr；shader 端字面量必须一致）----
static const uint kMaxLights = 8u;
static const uint kGpuLightStride = 64u;
static const uint kSceneDataLightArrayBase = 262272u;  // 128 + 4096*64

// ---- 单灯（float4 对齐，stride=64）----
struct GpuLight
{
    float4 direction;  // dir 光：世界方向（归一化，照射向）；point 光未用
    float4 position;   // point 光：世界位置；dir 光未用
    float4 color;      // 光色 × intensity（CPU 预乘，同 LightDataGpu.color 语义）
    float4 params;     // .x = point 衰减半径（dir 光 = 0 表示无衰减），.y = 类型（0=dir, 1=point）
};

uint readLightCount()
{
    return sceneData.Load(kSceneDataLightArrayBase);
}

GpuLight readGpuLight(uint i)
{
    uint b = kSceneDataLightArrayBase + 16u + i * kGpuLightStride;
    GpuLight g;
    g.direction = asfloat(sceneData.Load4(b));
    g.position  = asfloat(sceneData.Load4(b + 16u));
    g.color     = asfloat(sceneData.Load4(b + 32u));
    g.params    = asfloat(sceneData.Load4(b + 48u));
    return g;
}
