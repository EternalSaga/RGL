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
//   binding 0 = 源 env cubemap（TextureCube，**视图起点 = level+1，逐样本 LOD 读盒式 mip 链**，
//               视图内全部 SRO；起点 > 被写级 ⟹ 读不到自己，见文件头「逐样本 mip」段）
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
//
// ── 逐样本 mip（2026-10-18 立，湿面白散斑的根因）──
// 原式每个样本都读 **mip0 锐利源**。交付 env（san_giuseppe_bridge_4k）最亮纹素 82637、中值
// 0.266 = **30 万:1 动态范围**，而太阳只占约 110/8.4M 个纹素 ⟹ 256 样本命中它的概率 ≈ 1e-2
// /纹素：绝大多数纹素估出 0，极少数估出 82637/256 ≈ 323。预滤波图因此不是「太阳的模糊像」
// 而是**孤立单纹素尖峰**（CPU 独立复刻本估计器实测：r=0.122 有 12/24576 个纹素超邻域 751 倍，
// r=0.35 有 65 个）。渲染端湿石 roughness 0.05–0.35 逐像素反射方向扫过这些尖峰 ⟹ 交付图白散斑
//（判据 speck：湿石窗 30、全图 360；env 强度归零 ⟹ 4）。归因全文 → docs/还债总账.md 债-138。
//
// 修法 = Karis《Moving Frostbite to PBR》/ GPU Gems 3 §20.4 的 **filtered importance sampling**：
// 每个样本按**它自己覆盖的立体角**去读源图对应模糊级，而不是全读 mip0。样本立体角 = 1/(N·pdf)，
// pdf = D_GGX(NoH)/4（V=N ⟹ H→L 的 Jacobian 就是 1/4，与 Khronos glTF-IBL-Sampler 同式）；
// 与一个源纹素立体角（= 4π/(6·W²)）之比开 log2 即 mip 级。它同时做两件事：① 把单样本的取值
// 上限从「太阳峰值」压到「样本足迹内的盒平均」；② 让相邻输出纹素读到**相邻**足迹 ⟹ 太阳的高光
// 从孤立点恢复成连续一片。CPU 同口径验证：r=0.122 孤立尖峰 12 → **0**、最坏孤立比 751 → **2.6**；
// r=0.35 65 → **0**、641 → **1.1**。
//
// 🔴 **上面推导里的 4π 在代码里漏了七天**（2026-10-25 修，债-144）：第 39 行写的
// saTexel = 4π/(6·W²) 一直是对的，而 LOD 那一行写成 `0.5·log2(6W²/(N·pdf))`——分母少 4π ⟹
// LOD 恒定偏大 0.5·log2(4π) = **1.83 级**，每个样本都去读比它自己足迹糊近两级的源，整张预滤波
// 图偏糊。为什么"CPU 同口径验证"没抓到：它跑的是**独立复刻的估计器**（不是这份 shader），验的是
// "逐样本 LOD 有效"这个方向，验不出两侧常数不一致。外部对照 = HEngine 同一处
// （`Modern/SceneEnvironmentPrecompute.hlsl`）是 Karis 原式、带 4π。
//
// 🔴 **源视图看不到被写的那一级**（结构性防自引用，不靠 shader 里钳位）：C++ 侧把 src 视图的
// baseMip 定为 level+1（末两级见 env_prefilter_impl.cppm），本 shader 因此**读不到 mip0，也
// 读不到正在写的 level**。这不是省事——逐样本 LOD 的落点在 level=9..11 会掉到 level±1 内，
// 若视图能看到自身，就是同一 dispatch 内读写同一纹素（UB，且 VVL 只会报布局不匹配）。
// 代价 = 最锐的样本也只能从 level+1 读起。
//
// LOD 下界（= 峰值 pdf 样本的 LOD；下界被地板顶起只影响最锐的那一小截样本尾巴，其余样本读到
// 更高 LOD）。口径 W=2048 / L=12 / N=256，**2026-10-25 按修正后公式重算**：
//   level        1     5     8     9    10    11
//   下界        1.37  6.02  7.37  7.71  8.02  8.29
//   地板        2     6     9    10    11    10*
// ⟹ **除 level 5 外，每一级的最锐样本都被地板顶起**。旧结论"钳位只在 roughness ≥ 0.82 的最后
// 两级生效"是缺 4π 撑出来的，方向整个反了（*level 11 走末两级特殊视图 base=10；level 10 的
// srcMaxLod=0，恒读 11）。顺带：旧结论**即便按旧数也不成立**——旧下界 3.20/7.84/9.20/9.54/
// 9.84/10.12（五格与旧记录逐位对上、level 10 差 0.06，反过来证明那串数是照代码算的）里
// level 9 的 9.54 已 < 地板 10。
// 修正后暴露一条与 4π 无关的既有限制：level 1（roughness 0.09，近镜面）想要 LOD 1.37，地板却是
// 2 = 512² 的盒平均 ⟹ 最锐的预滤波级达不到该粗糙度该有的锐度。缺 4π 时这条被掩盖（3.20 > 2
// 看着"不钳位"）。账 → 债-145。
// 前提：源图 level ≥1 必须是**盒式降采样链**（本图 level ≥1 原为 UNDEFINED，现由上传路径的
// LINEAR blit 链先填，见 texture_upload_cubemap.cppm::recordGgxMipChain）。

[[vk::binding(0, 0)]]
TextureCube<float4> srcEnv;
[[vk::binding(1, 0)]]
RWTexture2DArray<float4> dst;
[[vk::binding(2, 0)]]
SamplerState srcSampler;

struct EnvPrefilterPush
{
    uint level;        // 目标 mip 级（1..mipLevels−1）
    uint mipLevels;    // 总级数（roughness = level/(mipLevels−1)）
    uint faceSize;     // 目标级面边长（= max(1, mip0Size >> level)，C++/shader 同式）
    uint srcFaceSize;  // 源 mip0 面边长（逐样本 LOD 的「一个源纹素立体角」归一化，见文件头）
    uint srcBaseMip;   // 源视图起始 mip（= level+1，末两级特殊化；视图看不到本 level ⟹ 结构上读不到自身）
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
    // 逐样本 LOD（Karis 原式，与文件头推导逐字同形）：lod = 0.5·log2(saSample/saTexel)。
    // saSample = 1/(N·pdf) 是一个样本覆盖的立体角，saTexel = 4π/(6·W²) 是一个源纹素的立体角
    // （立方图 6 面各 W² 纹素铺满 4π 球面），pdf = D_GGX/4。
    // 🔴 saTexel 分子上的 4π **不是可约掉的常数**（2026-10-25 修，债-144）：本行此前写成
    // `0.5·log2(6W²/(N·pdf))`，等价于把 saTexel 当成 1/(6W²)，LOD 恒定偏大 0.5·log2(4π)=1.83
    // 级 ⟹ 每个样本都去读比它自己足迹糊近两级的源，整张预滤波图偏糊。之所以把两个立体角各写
    // 成一个具名量、而不是把 4π 折进分子：折进去后这一行与文件头推导就再也对不上字，而本文件
    // 头 2026-10-18 那版推导**本来就是带 4π 的正确式**——错的一直是代码，它却靠"注释与代码
    // 不一致"活了七天（AGENTS 哲学 §1 点名的形态：注释里的推导像护栏）。
    // a2 = roughness⁴ = α²，与 importanceSampleGGX 内的 a2 同值（两处必须一致，否则样本
    // 分布与它的 pdf 对不上，LOD 就选错）。
    const float r2 = roughness * roughness;
    const float a2 = r2 * r2;
    const float saTexel =
        4.0 * kPi / (6.0 * float(push.srcFaceSize) * float(push.srcFaceSize));
    // 源视图只覆盖 [srcBaseMip, mipLevels)，故 LOD 先减 baseMip 再钳到视图内（视图外的级
    // 硬件也会钳，这里显式写出来是为了让「读不到什么」在本文件可读）
    const float srcBase = float(push.srcBaseMip);
    const float srcMaxLod = float(push.mipLevels - push.srcBaseMip) - 1.0;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i)
    {
        float2 Xi = hammersley(i, SAMPLE_COUNT);
        float3 H = importanceSampleGGX(Xi, N, roughness);
        float3 L = normalize(reflect(-V, H));
        float NoL = dot(N, L);
        if (NoL > 0.0)
        {
            // Heitz D_GGX（与上面样本分布同一 a2）
            float NoH = max(dot(N, H), 0.0);
            float d = NoH * NoH * (a2 - 1.0) + 1.0;
            float D = a2 / (kPi * d * d);
            float pdf = max(D * 0.25, 1e-20);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf);
            float lod = clamp(0.5 * log2(saSample / saTexel) - srcBase, 0.0, srcMaxLod);
            sum += srcEnv.SampleLevel(srcSampler, L, lod).rgb * NoL;
            weight += NoL;
        }
    }
    float3 color = weight > 0.0 ? sum / weight : float3(0.0, 0.0, 0.0);
    dst[gid] = float4(color, 1.0);
}
