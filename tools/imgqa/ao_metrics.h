#pragma once
// AO 噪声判据的**纯度量层**（无引擎、无 IO、除 OpenCV 外无依赖）——从 ao_noise.cpp 拆出来的
// 唯一理由：**判据的算子要能被合成靶自检**。留在匿名 namespace 里，selftest 就够不着它们，
// 「判据工具自己要有自检」这条纪律会退化成「只自检解码器」。
//
// 三个度量轴（缺一个就会被区域形状骗成「噪声大」，详见 ao_noise.cpp 头注释）：
//   偏离契约值（`fracBelowContract`）——只在真·世界空间契约区里才等于噪声；本工具没有深度缓冲，
//     画不出世界空间契约区，故它只作观察量（实测：离轮廓 >40 px 处 AO 可低到 37，是合法遮蔽）。
//   高频项（`jitterRms` = medianBlur(5) 残差 RMS）——对**光滑**梯度不敏感（中值滤波保斜坡 ⟹
//     线性渐变读 0，selftest 钉这条闭式），是区域定义不干净时仍然成立的主判据。
//   渐变/颗粒分解（`byteAbsDiff` 的 smooth/grain）——交付面上「AO 加进去的东西有多少是颗粒」。
//
// 定义在 `ao_metrics.cpp`；本头只放声明与类型（默认参数只写接口，实现侧不重复）。
#include <cstdint>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// 契约值：AO 附件 = r8_unorm **可见度**，开阔处契约 1.000 ⟹ 字节 255。
constexpr double kAoContract = 255.0;

// ───────────────────────────── 区域 ─────────────────────────────

/// 几何轮廓图：id 与 4 邻域不同 = 物体边界（含「几何 ↔ 背景 0」的剪影边）。
/// 图像四边也算轮廓——AO 的采样点越出屏幕时**整条样本被跳过**（`ao.frag.slang:141`），
/// 屏幕边缘的 AO 因此有合法的衰减，不是噪声，不得进契约区。
cv::Mat idEdges(const cv::Mat& ids);

/// 到最近轮廓像素的欧氏距离。⚠ **输出是 CV_32F**（distanceTransform 的约定），
/// 按 `ptr<uint8_t>` 读它就是拿浮点字节当整数读（实测踩过，见 `flatRegion`）。
cv::Mat distToEdge(const cv::Mat& edges);

/// 属于 oid、且距轮廓 ∈ (lo, hi] 的区域。hi = INT_MAX 级 = 「平坦契约区」；
/// (2, radius] = 「合法梯度带」（接触 AO 该在这里出现，是阳性锚点所在，不是噪声区）。
cv::Mat bandOf(const cv::Mat& ids, const cv::Mat& dist, int32_t oid, double lo, double hi);

/// 契约区（平坦区）掩码：`oid` 内且离任何 id 边界 > `radius` 像素。
cv::Mat flatRegion(const cv::Mat& ids, const cv::Mat& dist, int32_t oid, double radius);

/**
 * **凹角接触环** = 属于 `innerOid` 的、其邻域（膨胀 `grow` px 的范围内）贴着 `outerOids`
 * 之一的像素。
 *
 * 为什么需要它而不是「一切轮廓附近的带」当阳性锚点：带里既包含**凸边**（方块顶面外沿、
 * 球轮廓——那里 AO 恒 1 是**正确**的），也包含**凹角接触**（盒脚与地面、球与盒面——那里
 * AO 必须 < 1）。首跑就是被这个区分坑了一次：整条带的变暗占比只有 8.4%，看着像「AO 没跑」，
 * 实际是带里绝大多数像素本来就该是 1。**阳性锚点必须钉在闭式说「这里必须有遮蔽」的地方。**
 */
cv::Mat contactRing(const cv::Mat& ids, int32_t innerOid, const std::vector<int32_t>& outerOids,
    int grow);

// ───────────────────────────── 统计 ─────────────────────────────

struct RegionStat {
    int64_t pixels = 0;
    double mean = 0.0, sd = 0.0, lo = 0.0, hi = 0.0, p05 = 0.0, p95 = 0.0;
    double fracBelowContract = 0.0;  ///< 偏离契约值（< 255）的像素占比
    double lapRms = 0.0;             ///< 3×3 拉普拉斯响应 RMS（颗粒度；窗口须全在 mask 内）
    double jitterRms = 0.0;          ///< (x − medianBlur(x,5)) 的 RMS（逐像素抖动幅度）
};

/**
 * mask 上的单通道统计。`contract` < 0 = 不算契约偏离；`y0/y1` = 只统计该行的区间
 * （按行分带用；传 ROI mask 会被下游静默吞掉，实测踩过，故把「带」做成参数而不是 mask）。
 *
 * `lapRms` 与 `jitterRms` 是两个**不同轴**的量，都保留是因为一次真实歧义：拉普拉斯对
 * 「单调斜坡」也给非零响应（斜坡的二阶导为 0 但量化让它不为 0），单看它分不清「渐变」
 * 与「颗粒」；medianBlur 残差把渐变项减掉，剩下的才是逐像素抖动。两者一起看才不会被
 * 「接触区本来就有大梯度」骗成「接触区噪声大」。
 */
RegionStat statRegion(const cv::Mat& x8u, const cv::Mat& mask, double contract = kAoContract,
    int y0 = 0, int y1 = -1);

/// 线性亮度上的同族统计（A/B 差值图用；mask 外一律 0，不参与统计）。
struct LinStat {
    int64_t pixels = 0;
    double mean = 0.0, rms = 0.0, maxAbs = 0.0, jitterRms = 0.0, smoothRms = 0.0;
};
LinStat statLinearDiff(const cv::Mat& d64f, const cv::Mat& mask);

/**
 * **字节域**的 A/B 幅度（回答「AO 把交付像素挪动了几个 LSB」——线性亮度域的数字回答不了
 * 「看不看得见」，人眼与 8bit 帧缓冲都在字节域；skill rgl-verify 的「比较域 = 观测域」）。
 *
 * 逐像素取三通道 |Δ| 的**最大值**（不是均值）：AO 是乘数、三通道同向缩放，取最大通道就是
 * 「这个像素最坏挪动了多少」，不会被暗通道稀释成 0。
 * 再按 medianBlur(5) 拆成渐变项与颗粒项：`smooth` = AO 的形状（合法），`grain` = 逐像素
 * 独立抖动（噪声本体）。两者之比 = 「AO 施加到画面上的东西里有多少是颗粒而不是形状」。
 */
struct ByteAB {
    int64_t pixels = 0, changed = 0;
    double mean = 0.0, p95 = 0.0, p99 = 0.0, mx = 0.0;
    double smoothRms = 0.0, grainRms = 0.0;
};
ByteAB byteAbsDiff(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask);

/// mask 上两图逐像素 |Δ|（三通道取最大）的**最大值**（因果泄漏判据用：AO=255 处必须恰为 0）。
double maxByteDiff(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask);

/// 两个 8UC1 图在 mask 上的 RMS 差（**单通道**；3 通道 Mat 上 at<float> 只会读到 ch0）。
double rmsDiff(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask);

/// 两个「偏离契约」集合的 Jaccard 重叠（组 3 的爬行判据：偏离契约的**是不是同一批像素**）。
double jaccard(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask);

// ───────────────────────────── 对齐算子 ─────────────────────────────

/// 整数平移（正 x = 向右），边界补 0。用于「图案是否随几何一起移动」的对齐检验。
cv::Mat shiftX(const cv::Mat& src, int dx);

}  // namespace RGL::imgqa
