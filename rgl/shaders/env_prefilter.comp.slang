// Phase 8 批次 ④b：GGX importance-sample 预滤波环境 cubemap（Karis《Moving Frostbite to
// PBR》/ glTF prefilterEnvironmentMap 标准；启动期一次性，C++ 侧 env_prefilter.cppm /
// EnvPrefilter::recordPrefilter，契约单一真相源 = EnvPrefilterPush 12B static_assert 同源）
//
// 目的：把 HDR env mip 链从「LINEAR blit box 平均」换成「按粗糙度 GGX 重要性采样卷积」，
// 使采样端 `envMap.SampleLevel(R, roughness * mipRange)` 采到方向性正确的模糊反射——
// 消除金属锌屋顶上锐利的橙↔紫镜面条纹/色带。
//
// 读写（descriptor heap + shader object，同 mip_gen.comp.slang 配方，binding→heapOffset
// mapping 见 env_prefilter_impl.cppm；push constant 经 vkCmdPushDataEXT 供给）：
//   binding 0 = 源 env cubemap（TextureCube，取 mip0 sharp 源：SampleLevel(_, L, 0)，SRO）
//   binding 1 = 目标第 level 级（RWTexture2DArray，cube 第 level 级 6 层，GENERAL 写）
//   binding 2 = 共享 LINEAR sampler（sampler heap，constant-offset，同 mip_gen binding 2）
// dispatch = (ceil(faceSize/8), ceil(faceSize/8), 6)，z = cube face（0-5 = +X,-X,+Y,-Y,+Z,-Z）。
//
// **方向约定（务必与 CPU 侧 cubeFaceDir + Vulkan 采样器三者一致，写反 = 上下颠倒/左右镜像）**：
// texel (x,y)@face 的方向 = Vulkan cube map face/coordinate selection 表（textures.adoc
// §Cube Map Face Selection）的反函数，逐面与 texture_pool.cppm::cubeFaceDir 逐式相同：
//   s = 2·(x+0.5)/faceSize − 1, t = 2·(y+0.5)/faceSize − 1
//   +X:(1,−t,−s) −X:(−1,−t,s) +Y:(s,1,t) −Y:(s,−1,−t) +Z:(s,−t,1) −Z:(−s,−t,−1)
// 采样端 SampleLevel(R, lod) 用同一张表把方向 R 映射到 (face, s, t)，故写入本 texel 的
// 预滤波值正是 R 落在该 texel 时应读到的方向卷积结果（forward ∘ inverse = 恒等）。
//
// **roughness→level 映射（与采样端一致）**：roughness = level/(mipLevels−1)（level 0 = sharp
// 源不生成，1..L−1 生成；采样 lod = roughness·mipRange = roughness·(L−1) 线性插值相邻 mip）。

[[vk::binding(0, 0)]]
TextureCube<float4> srcEnv;
[[vk::binding(1, 0)]]
RWTexture2DArray<float4> dst;
[[vk::binding(2, 0)]]
SamplerState srcSampler;

struct EnvPrefilterPush
{
    uint level;      // 目标 mip 级（1..mipLevels−1）
    uint mipLevels;  // 总级数（roughness = level/(mipLevels−1)）
    uint faceSize;   // 目标级面边长（= max(1, mip0Size >> level)，C++/shader 同式）
};
[[vk::push_constant]] EnvPrefilterPush push;

static const uint SAMPLE_COUNT = 256u;
static const float kPi = 3.14159265358979323846;

// 低差异序列：radical inverse base 2（Van der Corput，位反转，Karis/GGX 标准，非 rand()）。
float radicalInverseVdC(uint bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return float(bits) * 2.3283064365386963e-10;  // / 0x100000000
}

// Hammersley 2D 低差异点集：Xi.x = radicalInverse（喂 cosTheta）、Xi.y = i/N（喂 phi）——
// 与 importanceSampleGGX 内的分量约定一一对应（任务指定）。
float2 hammersley(uint i, uint n)
{
    return float2(radicalInverseVdC(i), float(i) / float(n));
}

// GGX 半球重要性采样半向量 H（Karis importanceSampleGGX）：a = roughness²、a2 = a⁴，
// cosThetaH = sqrt((1−Xi.x)/((a2−1)·Xi.x + 1))，phi = 2π·Xi.y；绕 N 建 TBN 后返回世界半向量。
float3 importanceSampleGGX(float2 Xi, float3 N, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float phi = 2.0 * kPi * Xi.y;
    float cosTheta = sqrt((1.0 - Xi.x) / ((a2 - 1.0) * Xi.x + 1.0));
    float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
    // 以 +Z 为轴的半球半向量
    float3 Hh = float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
    // N→世界 TBN（up 退化保护：N 近 ±Z 时切换 up）
    float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    float3 tangent = normalize(cross(up, N));
    float3 bitangent = cross(N, tangent);
    return normalize(tangent * Hh.x + bitangent * Hh.y + N * Hh.z);
}

// 每线程一个 dst texel（8×8 组，z = face）。越界线程直接返回（faceSize 非 8 倍数时）。
[numthreads(8, 8, 1)]
void main(uint3 gid : SV_DispatchThreadID)
{
    uint fs = push.faceSize;
    if ((gid.x >= fs) || (gid.y >= fs))
    {
        return;
    }
    // texel 方向 = cubeFaceDir（Vulkan cube 展开约定，见文件头）
    float s = 2.0 * (float(gid.x) + 0.5) / float(fs) - 1.0;
    float t = 2.0 * (float(gid.y) + 0.5) / float(fs) - 1.0;
    float3 N;
    if (gid.z == 0u) { N = float3(1.0, -t, -s); }        // +X
    else if (gid.z == 1u) { N = float3(-1.0, -t, s); }   // -X
    else if (gid.z == 2u) { N = float3(s, 1.0, t); }     // +Y
    else if (gid.z == 3u) { N = float3(s, -1.0, -t); }   // -Y
    else if (gid.z == 4u) { N = float3(s, -t, 1.0); }    // +Z
    else { N = float3(-s, -t, -1.0); }                   // -Z
    N = normalize(N);

    // 与采样端一致的 roughness→level 映射（mipLevels>1 已由 C++ 保证，分母 ≥ 1）
    float roughness = float(push.level) / float(max(1u, push.mipLevels - 1u));
    float3 V = N;  // 预滤波：texel 方向同时当法线与反射视向（标准 prefilter 设定）

    float3 sum = float3(0.0, 0.0, 0.0);
    float weight = 0.0;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = hammersley(i, SAMPLE_COUNT);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(reflect(-V, H));
        float NoL = dot(N, L);
        if (NoL > 0.0)
        {
            sum += srcEnv.SampleLevel(srcSampler, L, 0.0).rgb * NoL;  // 只读 mip0 sharp 源
            weight += NoL;
        }
    }
    float3 color = weight > 0.0 ? sum / weight : float3(0.0, 0.0, 0.0);
    dst[gid] = float4(color, 1.0);
}
