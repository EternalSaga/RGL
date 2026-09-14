// Phase 8 方案 A：normal 贴图切线空间 → 世界空间（**标准 TBN**——CPU 烘焙的 TANGENT
// 顶点属性 + Gram-Schmidt 正交化 + 手性叉积，对齐 Filament surface_shading_parameters.fs）。
//
// 历史：③a 时代 bistro 无 TANGENT 属性，走屏幕空间导数 TBN（dFdx(P)/dFdx(uv) 解切线基）。
// 该方法在**掠射角**下 uv 屏幕 Jacobian 行列式 → 0，invDet 放大浮点误差 → world normal
// 逐像素椒盐噪声 → specular/Fresnel 放大成白噪点（调试历史 2026-09-13 续 5）。
// 根治 = 切线基挪出片元期：bistro_pack.py 导入期烘 Lengyel 平滑切线（glTF core
// TANGENT），loader 逐三角形推手性 W，顶点属性直通到 FS——本文件只做一次基变换。
// Filament 同样立场：lit 材质编译期强制 TANGENTS 属性，FS 零 dFdx 造 TBN 路径。
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
 * 标准 TBN：把切线空间法线变换到世界空间（对齐 Filament prepareMaterial 消费点）。
 * worldT = 世界空间切线 xyz + 手性 w（worldTangent() 产物；w 由 loader 逐三角形按
 * uv Jacobian 符号推——bistro UV 手性 92% 为负，缺 W 则镜像 UV 面法线翻错）。
 * T 先做 Gram-Schmidt 去 N 分量（顶点插值 + 非均匀缩放破坏正交性，Filament CPU 侧
 * TANGENTS_PROVIDED 重正交化同款理由"honor the exact normal over the exact tangent"），
 * B = cross(N, T)·w（叉积 + 手性，不信任烘焙 B），再一次基变换。掠射角下无任何
 * 导数参与 → 根除 ③a 导数 TBN 的 det→0 白噪点。
 * 中性 normal 贴图不动点：texTangent=(0,0,1) → 返回值 = worldN，与切线基无关
 *（无 normal 贴图 primitive 绑中性纹理，loader 的 Frisvad 兜底值因此无害）。
 * @param worldN 世界空间顶点法线（归一化）
 * @param worldT 世界空间切线 xyz + 手性 ±1（worldTangent() 输出）
 * @param texTangent 解码后的切线空间法线（decodeNormalDx 产物，已 flip-G + Z 重建）
 * @return 世界空间法线（归一化）
 */
float3 applyNormalMapTBN(float3 worldN, float4 worldT, float3 texTangent)
{
    float3 T = worldT.xyz - worldN * dot(worldN, worldT.xyz);  // Gram-Schmidt 正交化
    float tlen = length(T);
    if (tlen < 1e-6)
        return normalize(worldN);  // 退化切线（零向量/平行 N）→ 顶点法线回落
    T /= tlen;
    float3 B = cross(worldN, T) * worldT.w;  // 副切线 = 叉积 × 手性
    return normalize(T * texTangent.x + B * texTangent.y + worldN * texTangent.z);
}

/**
 * Geometric specular anti-aliasing（Phase 8 方案 C，Kaplanyan 2016 "Stable specular
 * highlights" / Tokuyoshi & Kaplanyan 2019；对齐 Filament surface_shading_lit.fs
 * normalFiltering）：掠射角 / 远处下低多边形起伏的微面高光会因 specular lob 小于
 * 像素而闪烁成中性白噪点。用**几何世界法线**（normal map 之前、插值顶点法线——干净
 * 信号，不含 normal map 高频）的屏幕导数估计亚像素朝向方差，抬高 roughness 把 lob
 * 铺过像素。**必须喂几何法线**：喂 normal-mapped 法线会把 normal map 高频也计入方差、
 * 逐像素抖动反而加剧（Filament 刻意传 getWorldGeometricNormalVector 的原因）。
 * 域换算对齐 Filament：本引擎 roughness = perceptual（下游 a = roughness²），kernel
 * 加到 a²（= pr⁴）再开四次方回 perceptual，直接喂回现有 a = roughness*roughness 流。
 * 常数 = Filament MaterialBuilder 默认（variance 0.15 / threshold 0.2，glTF 默认开）。
 * @param roughness 材质 perceptual roughness（clamp 后）
 * @param geoWorldNormal 几何世界法线（normal map 之前，归一化）
 * @return 反锯齿后的 perceptual roughness（≥ 输入，掠射/起伏处抬升）
 */
float normalFiltering(float roughness, float3 geoWorldNormal)
{
    float3 du = ddx(geoWorldNormal);
    float3 dv = ddy(geoWorldNormal);
    float variance = 0.15 * (dot(du, du) + dot(dv, dv));
    float kernelRoughness = min(2.0 * variance, 0.2);
    float r2 = roughness * roughness;
    return sqrt(sqrt(saturate(r2 * r2 + kernelRoughness)));  // (pr⁴ + kernel)^(1/4)
}
