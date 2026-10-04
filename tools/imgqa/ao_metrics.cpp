#include "ao_metrics.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

// ───────────────────────────── 区域 ─────────────────────────────

/**
 * 几何轮廓图：id 与 4 邻域不同 = 物体边界（含「几何 ↔ 背景 0」的剪影边）。
 * 图像四边也算轮廓——AO 的采样点越出屏幕时**整条样本被跳过**（`ao.frag.slang:141`），
 * 屏幕边缘的 AO 因此有合法的衰减，不是噪声，不得进契约区。
 */
cv::Mat idEdges(const cv::Mat& ids)
{
    cv::Mat e = cv::Mat::zeros(ids.size(), CV_8UC1);
    for (int y = 0; y < ids.rows; ++y)
        for (int x = 0; x < ids.cols; ++x) {
            const int32_t v = ids.at<int32_t>(y, x);
            bool edge = x + 1 < ids.cols && ids.at<int32_t>(y, x + 1) != v;
            edge = edge || (x > 0 && ids.at<int32_t>(y, x - 1) != v);
            edge = edge || (y + 1 < ids.rows && ids.at<int32_t>(y + 1, x) != v);
            edge = edge || (y > 0 && ids.at<int32_t>(y - 1, x) != v);
            e.at<uint8_t>(y, x) = edge ? 255 : 0;
        }
    cv::rectangle(e, cv::Rect(0, 0, ids.cols, ids.rows), cv::Scalar(255), 1);
    return e;
}

/// 到最近轮廓像素的欧氏距离（distanceTransform 的语义：到最近 0 值像素）。
cv::Mat distToEdge(const cv::Mat& edges)
{
    cv::Mat dist;
    cv::distanceTransform(~edges, dist, cv::DIST_L2, 3);
    return dist;
}

/// 属于 oid、且距轮廓 ∈ (lo, hi] 的区域。hi = INT_MAX 级 = 「平坦契约区」；
/// (2, radius] = 「合法梯度带」（接触 AO 该在这里出现，是阳性锚点所在，不是噪声区）。
cv::Mat bandOf(const cv::Mat& ids, const cv::Mat& dist, int32_t oid, double lo, double hi)
{
    cv::Mat same, above, below, m;
    cv::compare(ids, cv::Scalar(oid), same, cv::CMP_EQ);
    cv::compare(dist, cv::Scalar(lo), above, cv::CMP_GT);
    cv::compare(dist, cv::Scalar(hi), below, cv::CMP_LE);
    cv::bitwise_and(same, above, m);
    cv::bitwise_and(m, below, m);
    return m;
}

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
    int grow)
{
    cv::Mat outer = cv::Mat::zeros(ids.size(), CV_8UC1);
    for (const int32_t o : outerOids) {
        cv::Mat m;
        cv::compare(ids, cv::Scalar(o), m, cv::CMP_EQ);
        cv::bitwise_or(outer, m, outer);
    }
    if (cv::countNonZero(outer) == 0)
        return cv::Mat::zeros(ids.size(), CV_8UC1);
    cv::Mat near;
    cv::dilate(outer, near,
        cv::getStructuringElement(cv::MORPH_RECT, {2 * grow + 1, 2 * grow + 1}));
    cv::Mat inner;
    cv::compare(ids, cv::Scalar(innerOid), inner, cv::CMP_EQ);
    cv::Mat ring;
    cv::bitwise_and(inner, near, ring);
    return ring;
}

/// 契约区（平坦区）掩码：`oid` 内且离任何 id 边界 > `radius` 像素。
///
/// ⚠ 就是 `bandOf(…, radius, +∞)`，**必须**走 `cv::compare`：`distToEdge` 出来的距离图是
/// **CV_32F**（distanceTransform 的输出类型），按 `ptr<uint8_t>` 逐行读会把浮点字节当整数读，
/// 「离边界 > 40px」于是变成随机判据——实测踩过：契约区从 37.6 万像素膨胀到 59.6 万，把
/// 离轮廓 0–15 px 的**合法接触遮蔽**（AO 低到 71）全算成噪声，σ 从 1.3 涨到 10.9。
cv::Mat flatRegion(const cv::Mat& ids, const cv::Mat& dist, int32_t oid, double radius)
{
    cv::Mat same, above, m;
    cv::compare(ids, cv::Scalar(oid), same, cv::CMP_EQ);
    cv::compare(dist, cv::Scalar(radius), above, cv::CMP_GT);
    cv::bitwise_and(same, above, m);
    return m;
}

// ───────────────────────────── 统计 ─────────────────────────────
/**
 * mask 上的单通道统计。`contract` < 0 = 不算契约偏离；`y0/y1` = 只统计该行的区间
 * （按行分带用；传 ROI mask 会被下游静默吞掉，实测踩过，故把「带」做成参数而不是 mask）。
 *
 * `lapRms` 与 `jitterRms` 是两个**不同轴**的量，都保留是因为一次真实歧义：拉普拉斯对
 * 「单调斜坡」也给非零响应（斜坡的二阶导为 0 但量化让它不为 0），单看它分不清「渐变」
 * 与「颗粒」；medianBlur 残差把渐变项减掉，剩下的才是逐像素抖动。两者一起看才不会被
 * 「接触区本来就有大梯度」骗成「接触区噪声大」。
 */
RegionStat statRegion(const cv::Mat& x8u, const cv::Mat& mask, double contract,
    int y0, int y1)
{
    RegionStat s;
    const int ye = y1 < 0 ? mask.rows : std::min(mask.rows, y1);
    cv::Mat f, lap, sm, jit, inner;
    x8u.convertTo(f, CV_32F);
    cv::Laplacian(f, lap, CV_32F, 1);
    // ⚠ ksize 只能是 3 或 5：OpenCV 4.12 对 ksize>5 断言 `src.depth() == CV_8U`（实测踩过），
    // 而抖动必须带符号（有正负），不能退回 8U。5×5 足够：AO 梯度在 ~30 px 尺度上变化，
    // 抖动是逐像素独立的，5 px 窗内的中值就是局部电平。
    cv::medianBlur(f, sm, 5);
    cv::subtract(f, sm, jit);
    cv::erode(mask, inner, cv::getStructuringElement(cv::MORPH_RECT, {3, 3}));
    std::vector<double> vals;
    int64_t below = 0, nLap = 0, nJit = 0;
    double lapAcc = 0.0, jitAcc = 0.0;
    for (int y = y0; y < ye; ++y)
        for (int x = 0; x < mask.cols; ++x) {
            if (!mask.at<uint8_t>(y, x))
                continue;
            const double v = x8u.at<uint8_t>(y, x);
            vals.push_back(v);
            if (contract >= 0.0 && v < contract)
                ++below;
            jitAcc += static_cast<double>(jit.at<float>(y, x)) * jit.at<float>(y, x);
            ++nJit;
            if (inner.at<uint8_t>(y, x)) {  // 窗口须整个落在 mask 内，否则 mask 边界本身
                const double l = lap.at<float>(y, x);  // 会被算成高频——那是区域形状
                lapAcc += l * l;
                ++nLap;
            }
        }
    s.pixels = static_cast<int64_t>(vals.size());
    if (vals.empty())
        return s;
    double sum = 0.0;
    for (const double v : vals)
        sum += v;
    s.mean = sum / static_cast<double>(vals.size());
    double var = 0.0;
    for (const double v : vals)
        var += (v - s.mean) * (v - s.mean);
    s.sd = std::sqrt(var / static_cast<double>(vals.size()));
    s.lo = *std::min_element(vals.begin(), vals.end());
    s.hi = *std::max_element(vals.begin(), vals.end());
    const auto pick = [&](double q) {
        const size_t i = std::min(vals.size() - 1, static_cast<size_t>(q * vals.size()));
        std::nth_element(vals.begin(), vals.begin() + i, vals.end());
        return vals[i];
    };
    s.p05 = pick(0.05);
    s.p95 = pick(0.95);
    s.fracBelowContract = static_cast<double>(below) / static_cast<double>(vals.size());
    s.lapRms = nLap > 0 ? std::sqrt(lapAcc / static_cast<double>(nLap)) : 0.0;
    s.jitterRms = nJit > 0 ? std::sqrt(jitAcc / static_cast<double>(nJit)) : 0.0;
    return s;
}

/// 线性亮度上的同族统计（A/B 差值图用；mask 外一律 0，不参与统计）。
LinStat statLinearDiff(const cv::Mat& d64f, const cv::Mat& mask)
{
    LinStat s;
    if (cv::countNonZero(mask) == 0)
        return s;
    cv::Mat d, sm, jit;
    d64f.convertTo(d, CV_32F);  // medianBlur 不吃 CV_64F
    cv::medianBlur(d, sm, 5);
    cv::subtract(d, sm, jit);
    double sum = 0.0, sq = 0.0, jsq = 0.0, ssq = 0.0;
    int64_t n = 0;
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x) {
            if (!mask.at<uint8_t>(y, x))
                continue;
            const double v = d.at<float>(y, x);
            sum += v;
            sq += v * v;
            jsq += static_cast<double>(jit.at<float>(y, x)) * jit.at<float>(y, x);
            ssq += static_cast<double>(sm.at<float>(y, x)) * sm.at<float>(y, x);
            s.maxAbs = std::max(s.maxAbs, std::abs(v));
            ++n;
        }
    s.pixels = n;
    if (n == 0)
        return s;
    s.mean = sum / static_cast<double>(n);
    s.rms = std::sqrt(sq / static_cast<double>(n));
    s.jitterRms = std::sqrt(jsq / static_cast<double>(n));
    s.smoothRms = std::sqrt(ssq / static_cast<double>(n));
    return s;
}

/**
 * **字节域**的 A/B 幅度（回答「AO 把交付像素挪动了几个 LSB」——线性亮度域的数字回答不了
 * 「看不看得见」，人眼与 8bit 帧缓冲都在字节域；skill rgl-verify 的「比较域 = 观测域」）。
 *
 * 逐像素取三通道 |Δ| 的**最大值**（不是均值）：AO 是乘数、三通道同向缩放，取最大通道就是
 * 「这个像素最坏挪动了多少」，不会被暗通道稀释成 0。
 * 再按 medianBlur(5) 拆成渐变项与颗粒项：`smooth` = AO 的形状（合法），`grain` = 逐像素
 * 独立抖动（噪声本体）。两者之比 = 「AO 施加到画面上的东西里有多少是颗粒而不是形状」。
 */
ByteAB byteAbsDiff(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask)
{
    ByteAB s;
    if (cv::countNonZero(mask) == 0)
        return s;
    std::vector<cv::Mat> ca, cb;
    cv::split(a, ca);
    cv::split(b, cb);
    cv::Mat dmax = cv::Mat::zeros(a.size(), CV_16SC1);
    for (int ch = 0; ch < 3; ++ch) {
        cv::Mat ad, bd, dd;
        ca[ch].convertTo(ad, CV_16S);
        cb[ch].convertTo(bd, CV_16S);
        cv::absdiff(ad, bd, dd);
        cv::max(dmax, dd, dmax);  // 同深度 16S 逐元素取大（混 8U/16S 会静默错）
    }
    cv::Mat d32, sm, grain;
    dmax.convertTo(d32, CV_32F);
    cv::medianBlur(d32, sm, 5);
    cv::subtract(d32, sm, grain);
    std::vector<double> vals;
    double gAcc = 0.0, sAcc = 0.0;
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x) {
            if (!mask.at<uint8_t>(y, x))
                continue;
            // ⚠ CV_16S 必须按 int16_t 读：`Mat::at<T>` 的行内偏移用 `sizeof(T)`，按 uint8_t
            // 读会隔一个像素读到**高字节 0**（实测 mean/p99 被腰斩、changed 减半；selftest
            // 的「恒定 5 LSB 偏移」闭式用例第一次跑就把它抓出来了）。
            const double v = dmax.at<int16_t>(y, x);
            vals.push_back(v);
            s.changed += v > 0 ? 1 : 0;
            gAcc += static_cast<double>(grain.at<float>(y, x)) * grain.at<float>(y, x);
            sAcc += static_cast<double>(sm.at<float>(y, x)) * sm.at<float>(y, x);
        }
    s.pixels = static_cast<int64_t>(vals.size());
    if (vals.empty())
        return s;
    double sum = 0.0;
    for (const double v : vals)
        sum += v;
    s.mean = sum / static_cast<double>(vals.size());
    s.mx = *std::max_element(vals.begin(), vals.end());
    const auto pick = [&](double q) {
        const size_t i = std::min(vals.size() - 1, static_cast<size_t>(q * vals.size()));
        std::nth_element(vals.begin(), vals.begin() + i, vals.end());
        return vals[i];
    };
    s.p95 = pick(0.95);
    s.p99 = pick(0.99);
    s.smoothRms = std::sqrt(sAcc / static_cast<double>(vals.size()));
    s.grainRms = std::sqrt(gAcc / static_cast<double>(vals.size()));
    return s;
}

/// mask 上两图逐像素 |Δ|（三通道取最大）的**最大值**（因果泄漏判据用：AO=255 处必须恰为 0）。
double maxByteDiff(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask)
{
    // ⚠ 取通道必须 `split`/`extractChannel`：`Mat::col(i)` 是**第 i 列**不是第 i 通道，
    // 拿它当通道用会把 Mat 缩成 720×1，之后按 1280 列遍历就是越界读（实测读出 32523 这种
    // 大于 255 的「字节差」——数字本身就自证读错了）。
    std::vector<cv::Mat> ca, cb;
    cv::split(a, ca);
    cv::split(b, cb);
    cv::Mat m, t, u;
    ca[0].convertTo(m, CV_16S);
    cb[0].convertTo(t, CV_16S);
    cv::absdiff(m, t, m);  // 16S：8U 相减会饱和成 0，暗→亮方向就丢了
    for (int ch = 1; ch < 3; ++ch) {
        ca[ch].convertTo(t, CV_16S);
        cb[ch].convertTo(u, CV_16S);
        cv::absdiff(t, u, t);
        cv::max(m, t, m);  // 逐像素三通道最大 |Δ|
    }
    double mx = 0.0;
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x)
            if (mask.at<uint8_t>(y, x))
                mx = std::max(mx, static_cast<double>(m.at<int16_t>(y, x)));
    return mx;
}

/// 两个 8UC1 图在 mask 上的 RMS 差（**单通道**；3 通道 Mat 上 at<float> 只会读到 ch0）。
double rmsDiff(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask)
{
    CV_Assert(a.channels() == 1 && b.channels() == 1);
    cv::Mat d;
    cv::subtract(a, b, d, cv::noArray(), CV_32F);
    double acc = 0.0;
    int64_t n = 0;
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x)
            if (mask.at<uint8_t>(y, x)) {
                const float v = d.at<float>(y, x);
                acc += static_cast<double>(v) * v;
                ++n;
            }
    return n > 0 ? std::sqrt(acc / static_cast<double>(n)) : 0.0;
}

/// 两个「偏离契约」集合的 Jaccard 重叠（组 3 的爬行判据：偏离契约的**是不是同一批像素**）。
double jaccard(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask)
{
    cv::Mat both, either;
    cv::bitwise_and(a, b, both);
    cv::bitwise_or(a, b, either);
    cv::Mat bIn, eIn;
    cv::bitwise_and(both, mask, bIn);
    cv::bitwise_and(either, mask, eIn);
    const double e = cv::countNonZero(eIn);
    return e > 0.0 ? cv::countNonZero(bIn) / e : 1.0;
}

// ───────────────────────────── 对齐算子 ─────────────────────────────

/**
 * 整数平移（**正 x = 内容向右移**），边界补 0。用于「图案是否随几何一起移动」的对齐检验。
 *
 * ⚠ 方向写反会把爬行判据整个反过来。初版把 src/dst 的起点写反（内容其实向左），文档却写
 * 「向右」——selftest 的 shiftX 用例（+3 应落在 x+3、且 +3 后 −3 逐位复原）钉住这条。
 * 爬行组本身对符号**不敏感**：对齐符号是用底图（几何必然跟着相机走）实测选出来的，
 * 不是照这里的注释写死的。
 */
cv::Mat shiftX(const cv::Mat& src, int dx)
{
    cv::Mat dst = cv::Mat::zeros(src.size(), src.type());
    if (dx == 0) {
        src.copyTo(dst);
        return dst;
    }
    const int w = src.cols - std::abs(dx);
    const int srcX = std::max(-dx, 0);
    const int dstX = std::max(dx, 0);
    src(cv::Rect(srcX, 0, w, src.rows)).copyTo(dst(cv::Rect(dstX, 0, w, src.rows)));
    return dst;
}

}  // namespace RGL::imgqa
