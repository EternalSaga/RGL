#pragma once
//
// material_attribution.h — 把「逐像素残差」按**材质**拆开（批 13.9d）
//
// ## 为什么要把归因轴从「亮度 × 梯度」换成「材质」
//
// 亮度 × 梯度那张六格表（`temporal_residual.h` 的 `BandTable`）在门框机位上是**按构造失效**的：
// 近削顶 × 高梯度那一格（假设的元凶 B）实测只有 **115 px = 0.01% 面积**，六格表里
// 「clip」两列加起来 296 px，而它的补集「mid × HIGH」有 46217 px。也就是说：**用户主张的
// 对象在这台机器这个机位上几乎没有面积**，任何"份额"数字都是在给噪声算比例（工具自己也在
// `printMovingBandVacuity` 里报了"近空集"）。继续在这张表上加分辨力是在问错问题。
//
// 真正该问的是「**哪些材质在驱动移动闪烁**」——金属 / 玻璃 / 湿地面这些**靠 IBL 反射**的材质，
// 与漫反射材质，在同一个机位同一个运动下的残差各占多少。而这个问题有一个现成的、
// **不依赖亮度代理**的逐像素身份来源：`diagnostics.picking` 打开后的 **object_id 附件**。
//
// ## objectID 的语义（为什么能一路映射回材质名）
//
// object_id = `MeshRef.objectSlot` = **全局 primitive 序**，由 `rgl/model_importer_impl.cppm:359`
// 在遍历 `model.primitives` 时按序分配；而 glTF 加载器把 `meshes[]` 按文档序摊平成
// `primitives[]`（mesh 为主序、primitive 为次序）。所以
//
//     objectID  ⟷  glb 里第 k 个 primitive（mesh-major / primitive-minor）
//             ⟷  该 primitive 的 `material`  ⟷  materials[material].name
//
// ⚠ **bistro_exterior 没有 `.manifest.json`**（`loadManifest` 直接抛），所以 piece 名只能从
//   glb 的 JSON chunk 现读。这里读的是**已提交资产本身**，不是第二份真相：名字的唯一出处就是
//   那个文件。解析失败 / id 越界一律退化成 `unnamed#<oid>`，**宁可无名不要安错名字**
//   （`image.cpp` 的 `attribute()` 同一条纪律：安错名字会让后续判据在错误的对象上求值）。
//
// ## 为什么两个份额都必须出（与六格表同一条纪律）
//
// `cntShare`（像素计数份额，不加权）与 `ampShare`（Σ残差 幅度份额）。只有后者能分辨
// 「像素少但每像素摆动巨大」——而金属/玻璃高光恰恰就是这种。只出计数 = 未完成。
//
// 第三个数 `residOverFloor` = 该材质的平均残差 / 全帧同态往返地板。它回答「这块的残差是
// **信号**还是**重采样**」：≈1 的格子即使幅度份额高也不该当闪烁读。
//
#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// glb 里一个 primitive 的身份（objectID 的语义出处）。
struct GlbPrim {
    int32_t index = 0;       ///< 全局 primitive 序（= 引擎 objectSlot）
    std::string mesh;        ///< meshes[i].name（无名 = "mesh<i>"）
    std::string material;    ///< materials[m].name（无名 = "mat<m>"，无材质 = "no-material"）
    int32_t materialIndex = -1;
};

/**
 * 解析 glb 的 JSON chunk，按 **mesh-major / primitive-minor** 摊平成全局 primitive 序。
 *
 * 读的是 `build/rgl/models/<stem>.glb`（引擎加载的同一个文件）。解析不了 = 抛（调用方
 * 必须显式处理"归因不可用"，不许静默给一张空表让人以为"没有材质在闪"）。
 */
[[nodiscard]] std::vector<GlbPrim> parseGlbPrimitives(const std::string& glbPath);

/// 一个材质的一行归因数字。
struct MaterialRow {
    std::string material;
    std::string exampleMesh;  ///< 该材质下残差最大的那个 mesh 名（定位用）
    int64_t primsVisible = 0; ///< 本帧可见的 primitive 个数（同一材质常有几十个）
    int64_t pixels = 0;       ///< 该材质覆盖的像素数
    int64_t over = 0;         ///< 其中残差 > τ 的像素数
    double mass = 0.0;        ///< Σ残差
    double pixelShare = 0.0;  ///< 面积份额
    double cntShare = 0.0;    ///< 计数份额 = 该材质 over / 全帧 over
    double ampShare = 0.0;    ///< 幅度份额 = 该材质 Σ残差 / 全帧 Σ残差
    double rate = 0.0;        ///< 该材质内 over-τ 率
    double meanResid = 0.0;   ///< 该材质内平均残差
    double residOverFloor = 0.0; ///< meanResid / 全帧地板（≈1 ⟹ 那只是重采样）
};

/**
 * 按材质聚合逐像素残差。
 *
 * @param ids    CV_32SC1 的 objectID（`decodeObjectId` 的产物；0 = 背景，不计入任何材质）
 * @param resid  CV_64FC1 残差场（与分带表同一份：跨帧最大）
 * @param prims  `parseGlbPrimitives` 的产物；id 越界退化成 unnamed
 * @param tau    与分带表同一个 τ（同一个"什么算超阈值"）
 * @param floorMean 全帧同态往返地板均值（`residOverFloor` 的分母）
 */
[[nodiscard]] std::vector<MaterialRow> materialAttribution(const cv::Mat& ids,
    const cv::Mat& resid, const std::vector<GlbPrim>& prims, double tau, double floorMean);

/**
 * 出表（**按幅度份额降序**——用户问的是"什么在驱动闪烁"，驱动 = 幅度贡献，不是面积）。
 *
 * 表头必须回显：面积份额 / 计数份额 / 幅度份额 **三个都在**，以及 `residOverFloor` 的读法。
 * 尾部固定补一行「其余 N 个材质合计」，否则读者会把 top-N 当成全集。
 */
void printMaterialTable(const std::string& title, const std::vector<MaterialRow>& rows,
    size_t topN, double tau, double floorMean, const std::string& residualName);
}  // namespace RGL::imgqa
