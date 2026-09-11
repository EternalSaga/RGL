// Phase 8 批次 ③a：normal 贴图切线空间 → 世界空间（屏幕空间导数 TBN，无 TANGENT 顶点属性）。
//
// 背景：bistro_exterior.glb 无 TANGENT 属性（0/1591 primitive），normal 贴图必须走
// 屏幕空间导数 TBN（由世界位置 + uv 的 dFdx/dFdy 解出切线基）——标准近似，屏幕边缘
// 精度下降（AGENTS.md Phase 8 风险表）；bistro_pack 重打包加 TANGENT 是后手。
//
// 约定（README 石锤）：
//   - normal 贴图 = DirectX 约定（Y-up）→ 解码 flip-G（y = -(g*2-1)）
//   - 可能 BC5 压缩无 Z 通道 → z = sqrt(saturate(1 - x² - y²)) 重建
//   - 中性 normal 纹理 RGBA8 (128,128,255,255) = (0.5,0.5,1,1) 是"flip-G 后 *2-1"
//     解码的不动点（flip 后仍 (0.5,0.5) → (0,0) → z=1）：无 normal 贴图的 primitive
//     绑它（texture_pool::ensureNeutralNormalTexture），shader 端零特判
//
// 本文件只传值无 buffer struct（Slang float3 16B 对齐陷阱不涉及）。

/**
 * 解码 DirectX 约定 normal 贴图 texel 为切线空间法线（flip-G + Z 重建）。
 * DX 的 g 通道对应 -y（OpenGL 约定），故 y 取负；BC5 无 Z 时重建
 * z = sqrt(saturate(1 - x² - y²))（saturate 防浮点误差出负）。
 * @param texel normal 贴图采样值（rgb ∈ [0,1]；中性 (0.5,0.5,1) → 输出 (0,0,1) 不动点）
 * @return 归一化切线空间法线
 */
float3 decodeNormalDx(float4 texel)
{
    float x = texel.r * 2.0 - 1.0;
    float y = -(texel.g * 2.0 - 1.0);
    float z = sqrt(saturate(1.0 - x * x - y * y));
    return normalize(float3(x, y, z));
}

/**
 * 屏幕空间导数 TBN：把切线空间法线变换到世界空间。
 * 由 dPdx = ddx(世界位置)、dPdy = ddy(世界位置) 与 st1 = ddx(uv)、st2 = ddy(uv)
 *（Slang 导数内建名；GLSL 式 dFdx/dFdy 名本版本不存在）解出切线 T = dP/du、
 * 副切线 B = dP/dv（[dPdx dPdy] = [T B]·[st1 st2] 的逆解），
 * 归一化后经 [T B N] 基变换切线法线到世界。曲面处 T/B 不严格正交 N = 方法固有
 * 近似（平面 patch 精确）。退化（uv 导数行列式 ≈ 0：无 uv / 屏幕边缘）→ 回落
 * 顶点世界法线 worldN。
 * @param worldN 世界空间顶点法线（归一化；退化回落用）
 * @param dPdx 世界位置导数（ddx(P)）
 * @param dPdy 世界位置导数（ddy(P)）
 * @param st1 uv 导数（ddx(uv)）
 * @param st2 uv 导数（ddy(uv)）
 * @param texTangent 解码后的切线空间法线（decodeNormalDx 产物，已 flip-G + Z 重建）
 * @return 世界空间法线（归一化）
 */
float3 applyNormalMap(float3 worldN, float3 dPdx, float3 dPdy,
    float2 st1, float2 st2, float3 texTangent)
{
    float det = st1.x * st2.y - st1.y * st2.x;
    if (abs(det) < 1e-8)
        return normalize(worldN);  // uv 退化（无 uv / 屏幕边缘）→ 顶点法线
    float invDet = 1.0 / det;
    float3 T = normalize((dPdx * st2.y - dPdy * st2.x) * invDet);  // dP/du
    float3 B = normalize((dPdy * st1.x - dPdx * st1.y) * invDet);  // dP/dv
    return normalize(T * texTangent.x + B * texTangent.y + worldN * texTangent.z);
}
