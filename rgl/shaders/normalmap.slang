// Phase 8 方案 A：normal 贴图切线空间 → 世界空间（**标准 TBN**——CPU 烘焙的 TANGENT
// 顶点属性 + Gram-Schmidt 正交化 + 手性叉积，对齐 Filament surface_shading_parameters.fs）。
//
// 历史：③a 时代 bistro 无 TANGENT 属性，走屏幕空间导数 TBN（dFdx(P)/dFdx(uv) 解切线基）。
// 该方法在**掠射角**下 uv 屏幕 Jacobian 行列式 → 0，invDet 放大浮点误差 → world normal
// 逐像素椒盐噪声 → specular/Fresnel 放大成白噪点（git:调试历史 2026-09-13 续 5）。
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
 * ⚠️ 该"无关"声明对 **NaN** 切线不成立：0×NaN = NaN 会污染整面（2026-10-01 黑面
 * bug，git:调试历史）。刻意**不在这里吞 NaN**（如 `!(tlen > 1e-6)` 形态）——NaN 进来
 * 就该渲染成显眼的整面黑让探针当场抓到，静默回落顶点法线 = 把数据 bug 伪装成
 * 合理光照（沉默跳过 = 假绿）。数据正确性由 loader 侧单测钉住（test_frisvad_tangent）。
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
 * ⚠ 上面这句「必须」作为**普适断言是错的**（2026-10-05 两轮实验，数据见本文件末尾）：
 * 它只对「不做空间滤波就直接逐像素喂」成立。空间聚合后喂 shading 法线在合成靶上把
 * glint speck 打到 0、线性域相对对比度降 77% ⟹ **真在削对比度，不是遮**。本函数**现行
 * 实现未改**（仍只喂几何法线），否决 shading 法线的理由是 bistro 侧的观感与时域代价，
 * 不是「这条路不成立」。改这里之前先读末尾那段数据与机制结论。
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

// ──────────────────────────────────────────────────────────────────────────────
// 【实测裁决 2026-10-05 · 第六 + 第七轮】normal map 高频能不能进 specular AA 的方差
//
// 结论一句话：**机制成立、代价不可接受、且「把邻域改成抖动无关」这条自救路线的前提
// 本身就是错的**。三轮变体全部否决，本文件保持只喂几何法线。别再调系数。
//
// 靶与口径：合成靶 nm_rig（tools/qa_scenes.py::scene_nm_rig，1280×720 / env.intensity
// = 0.003 / r = 0.12 / block = 1；speck 取 TAA 关、locked/grain 取 TAA 开 16 帧）+
// bistro 湿面机位（同口径）。speck = 单帧 L≥200 且 4 邻域中位 ≤60 的像素数。
//
// 变体：M1 = shading 法线在**屏幕 3×3** 的最大朝向偏差（空间聚合）；
//       M2 = shading 法线**逐像素**方差（无空间滤波）；
//       T1 = normal 贴图在**纹素空间 3×3**（整数纹素偏移 + 整数 mip 采样）的最大朝向
//            偏差 —— 第七轮专为「把邻域定义改成抖动无关」而做，与 M1 只差邻域怎么给。
//
// ① 合成靶 speck（amp = 每 texel tilt 幅度；amp=0 是负对照）
//      amp     0      4      8      12
//      现行    0      979    12487  23796
//      M1      0      0      0      0        ← 空间滤波 = 决定性变量
//      M2      0      49     434    766      ← 逐像素只压 ~97%，amp 单调性仍在
//      T1      0      1      1      1        ← 同样归零
//    amp=0 三档**逐位相同**（中性/均匀法线贴图 ⟹ spread 恒 0）；T1 另在线性域逐档核过
//    mode=0 与 mode=3 的 amp=0 出图 sha **全等**（r=0.04 / 0.12 两档，FWHM 1.250° 不变）。
//    线性域（scene_hdr，尺度无关）amp=12 的 p99.9/median 51.2 → 11.7（**−77%**）、
//    spike>8 122391 → 37870 ⟹ 不是「抬亮背景让绝对阈值失效」。
//
// ② bistro 湿面（同亮度工作点，阳性锚点 603 / 43 / 26 两轮均精确复现）
//      现行   speck   603  locked  43  grain>2 32.19  >20  9.077  >64 1.824  L≥250 1.194%
//      M1     speck  1099  locked  ——  grain>2 40.74  >20 16.21   >64 2.461  L≥250 0.890%
//      M2     speck   945  ——      ——  ——
//      T1     speck  1158  locked   7  grain>2 40.17  >20 16.11   >64 2.497  L≥250 0.853%
//    **speck 变差 1.6~1.9 倍**，而 L≥250 与门板相位锁定白点（26→11）反而变好 = 「把尖峰
//    摊成一堆 200~250 的中等亮斑」，交付域 speck 的绝对阈值 + 孤立性判据惩罚能量摊开。
//    肉眼（湿鹅卵石 300×300 2× 裁切）：点状 glint **糊成沿灰缝的连续亮线**、石面发蒙，
//    该窗梯度 p99 90.8 → 81.9（−10%）= 材质观感改变，不是 AA。
//    代价（同靶线性域反射带 FWHM）：amp=4 r=0.12 6.06° → 8.88°（M1，+47%）。
//
// ③ 时域（这是立项闸门）：nm_rig amp=4、TAA 开、jitter_scale 0 vs 1
//      jitter=0：现行 0.000 / M1 0.000 / T1 **0.000**  ⟹ 三者都不引入帧号/哈希驱动的不稳定
//      jitter=1：现行 36.597 / M1 48.324 / T1 **59.648**  ⟹ T1 比 M1 还差
//    亮度混淆已用两条独立手段排掉，**不是**「画面被抬亮（meanL 37.7→55.3）⟹ 2 LSB 绝对
//    阈值更容易越」：
//      · 尺度无关判据（逐像素跨相位相对极差 r=(max−median)/max 的中位数）：
//        现行 0.05182 / M1 0.05428 / T1 **0.07037**；
//      · 亮度配平探针（T1 降 intensity 到 meanL 36.4 < 现行的 42.7）：grain>2 仍 38.692。
//
// ④ **机制结论（本轮真正的产出，比"某个 mode 能不能开"更值钱）**
//    「M1 的额外 grain 完全随抖动出现，因为 3×3 邻域由 ddx(uv) 定义」这条诊断**只对了
//    一半**。T1 把邻域换成整数纹素 + 整数 mip（双线性权重在中心与 8 邻域逐位相同，
//    故邻域**宽度**确实与亚像素相位无关），grain 不降反升。闭式归因（qa_scenes 的 uv
//    映射 × 16 个 Halton 相位逐行算 floor(lod)）：16 相位里会换 mip 的像素占比 ≈ **0%**
//    ⟹ 残留耦合**不是** mip 量化边界。
//    真原因：spread 是 normal 贴图的**逐点高通**，自变量是**采样位置**。抖动让采样点
//    亚像素地移动，取到的局部 patch 就变了，值跟着变——**这与偏移怎么定义无关**。
//    整数纹素只锁住「邻域宽度」，锁不住「采样点落在哪」。
//    ⟹ 想要抖动无关，方向不是"邻域怎么给"，而是**换掉逐点采样这个动作**：要么在远大于
//      抖动位移（±0.5 px）的窗口上对 spread 做空间平均，要么用与 uv 无关的世界空间量。
//      任何"点采样 + 任意邻域定义"的组合都必然随抖动变，别再试第四种邻域。
//
// ⑤ 顺带记一笔**反向**事实（别丢）：这一族在"压火点"这条轴上确实有效——默认机位
//    L≥250 0.00130% → 0.00022%（−83%，meanL 只动 0.002%、全图梯度 7.385 → 7.371）、
//    bistro 门板白点 26 → 11、湿面 phase-lock 43 → 7。所以火点不是治不了，而是**不该用
//    "预糊 roughness"去治**：HEAD 1a15240（债-40）已把白点定性为「本帧 HDR firefly 从不
//    被裁剪」，正解在 TAA 侧的裁剪，不在材质侧的模糊。
// ──────────────────────────────────────────────────────────────────────────────
