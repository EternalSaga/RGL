// Slang C API 反射 spike 测试 shader：
// 镜像 rgl 实际契约（scene.slang SceneLight 4×float3 / lights.slang GpuLight 4×float4
// + sceneData binding 3 + 常量），验证 C API 能否直接查出非资源 struct 的
// 编译器实际布局（scalar layout 下偏移 0/12/24/36 与 0/16/32/48）。

// 镜像 scene.slang SceneLight（forward 头 @0-48，4×vec3）
struct SceneLight
{
    float3 direction;
    float3 eyePos;
    float3 color;
    float3 ambient;
};

// 镜像 lights.slang GpuLight（灯数组元素，4×vec4 packed，stride 64）
struct GpuLightLike
{
    float4 direction;
    float4 position;
    float4 color;
    float4 params;
};

// 镜像 lights.slang 契约常量（验证常量可否经 API 查询）
static const int kMaxLights = 8;

// 镜像 sceneData 全局资源（binding 3, set 0）
[[vk::binding(3, 0)]]
ByteAddressBuffer sceneData;
