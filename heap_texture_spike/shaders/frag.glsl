#version 460

// binding 0: sampler2D (OpTypeSampledImage) -> descriptor heap slot (SAMPLED_IMAGE 描述符)
//              + pEmbeddedSampler 提供采样器（替代 sampler heap 查找）
// binding 1: storage buffer (OpTypeBuffer)  -> descriptor heap slot (STORAGE_BUFFER, pAddressRange)
// 两个描述符共存于同一个 resource heap（Q3）。
// 注：GLSL 组合 sampler 对应 SPIR-V OpTypeSampledImage；Phase 3 主工程走 Slang
// （Texture2D<float> + samplerState = OpTypeImage + OpTypeSampler 对），内嵌 sampler
// 机制相同（spec 对两者均适用），Slang 变体在主工程中直接验证。
layout(set = 0, binding = 0) uniform highp sampler2D tex;
layout(set = 0, binding = 1) readonly buffer Data { float v; } data;

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 fragColor;

void main()
{
    fragColor = texture(tex, uv) + vec4(data.v);
}
