// shadow_transport_metric.cpp —— 像素域地面搬运的口径算子（为什么在这一层 → 同名 .h）。
//
// 本文件**只有算子**：重投影、搬运、影缘带、对拍读数、平移搜索。「这个二值量从哪来」（cov 场怎么
// 算、光度可信集怎么判）不在这里——cov 吃 `seamCovField`（与 shadow-grain / shadow-edge 同式同序，
// 🔴 不在本层另立一份覆盖度定义），可信集与臂表住 `shadow_swim_flat.cpp`。
//
// 🔴 本层一条铁律：**没有任何循环的迭代次数取决于数据**。平移搜索的轮次与每轮候选数全部由调用侧
// 给死（`rounds`），本层不做收敛判定、不自加轮次、不 while。理由不是风格：本批的一次交付就折在
// 「搜索自己决定跑多久」的无界工作上，设备级仪器一旦允许数据依赖的循环，长尾会把整条验收拖死。
#include "shadow_transport_metric.h"
#include "shadow_seam_metric.h"  // kSeamBin（二值影状态阈值与全家族同一份）

#include <opencv2/imgproc.hpp>  // dilate/erode（影缘带，见 edgeBandMask 那条「闭/开恒零」的教训）

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace RGL::imgqa {
namespace {

double dot3(const std::array<double, 3>& a, const std::array<double, 3>& b)
{
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/// 双线性取 CV_64FC1 的 cov 值。2×2 邻域不全在图内 = false（不夹到边缘：夹 = 把屏外读成边缘值）。
bool sampleBilinear(const cv::Mat& f, double u, double v, double& out)
{
    const int x0 = static_cast<int>(std::floor(u));
    const int y0 = static_cast<int>(std::floor(v));
    if (x0 < 0 || y0 < 0 || x0 + 1 >= f.cols || y0 + 1 >= f.rows)
        return false;
    const double fx = u - x0;
    const double fy = v - y0;
    const double a00 = f.at<double>(y0, x0);
    const double a10 = f.at<double>(y0, x0 + 1);
    const double a01 = f.at<double>(y0 + 1, x0);
    const double a11 = f.at<double>(y0 + 1, x0 + 1);
    out = (1 - fx) * (1 - fy) * a00 + fx * (1 - fy) * a10 + (1 - fx) * fy * a01
        + fx * fy * a11;
    return true;
}

/// 二值影状态（严格 > kSeamBin，恰 0.5 不计影——与 shadow-grain 的 kGrainBin 同口径）。
inline bool shadowed(double cov) { return cov > kSeamBin; }

/// 该像素是否落在影缘带内（空图 = 不侵蚀，一律 false）。
inline bool onBandNear(const cv::Mat& band, int x, int y)
{
    return !band.empty() && x >= 0 && y >= 0 && x < band.cols && y < band.rows
        && band.at<uint8_t>(y, x);
}

/// 该采样点是否落在影缘带内（A 侧查本像素，B 侧查**双线性 2×2 抽头盒的四角**）。
/// 🔴 四角而不是参考像素：抽头最远可离参考点 1.5 像素，只查参考点会放行「参考点距缘 2 像素、
/// 某个抽头正压在缘上」的样本 ⟹ 世界不变的场也插出中间值（实测残差 0.095 挂在 16325 个点上，
/// selftest 的「侵蚀后 Σ|Δcov| 落在浮点闭合级」就是拦这条的）。
/// 抽成独立函数的另一个理由：`compareTransported` 的主循环要吃下它自己那五条早退，认知复杂度
/// 会越过工具侧的 15 线——这条判据本来也是独立的一件事，不是为了让检查变绿才切的。
bool onBandForSample(const cv::Mat& bandA, const cv::Mat& bandB, int xa, int ya, double dstX,
    double dstY)
{
    if (onBandNear(bandA, xa, ya))
        return true;
    if (bandB.empty())
        return false;
    const int x0 = static_cast<int>(std::floor(dstX));
    const int y0 = static_cast<int>(std::floor(dstY));
    return onBandNear(bandB, x0, y0) || onBandNear(bandB, x0 + 1, y0)
        || onBandNear(bandB, x0, y0 + 1) || onBandNear(bandB, x0 + 1, y0 + 1);
}

}  // namespace

bool groundHitDouble(const Projector& p, int x, int y, double maxDistanceM, double& wx,
    double& wz)
{
    // 像素中心 → ndc（与 projectPoint3 的 ndc→像素严格互逆，selftest 的往返锚点钉这条）。
    const double nx = (x + 0.5) / static_cast<double>(p.cols) * 2.0 - 1.0;
    const double ny = 1.0 - (y + 0.5) / static_cast<double>(p.rows) * 2.0;
    std::array<double, 3> d
        = {p.f[0] + p.r[0] * (nx * p.tanHalf * p.aspect) + p.up[0] * (ny * p.tanHalf),
              p.f[1] + p.r[1] * (nx * p.tanHalf * p.aspect) + p.up[1] * (ny * p.tanHalf),
              p.f[2] + p.r[2] * (nx * p.tanHalf * p.aspect) + p.up[2] * (ny * p.tanHalf)};
    const double n = std::sqrt(dot3(d, d));
    if (!(n > 0.0))
        return false;
    for (double& c : d)
        c /= n;
    if (std::abs(d[1]) < 1e-9)
        return false;  // 视线近平行地面：交点在无穷远
    const double t = -p.pos[1] / d[1];
    if (!(t > 0.0) || t > maxDistanceM)
        return false;  // 相机背后 / 远到不可信
    wx = p.pos[0] + d[0] * t;
    wz = p.pos[2] + d[2] * t;
    return true;
}

GroundSamples sampleGround(const Projector& from, int cols, int rows, double maxDistanceM)
{
    GroundSamples s;
    s.srcIndex.reserve(static_cast<size_t>(cols) * static_cast<size_t>(rows) / 4);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x) {
            double wx = 0.0;
            double wz = 0.0;
            if (!groundHitDouble(from, x, y, maxDistanceM, wx, wz))
                continue;
            s.srcIndex.push_back(y * cols + x);
            s.wx.push_back(wx);
            s.wz.push_back(wz);
        }
    return s;
}

void projectSamples(const Projector& into, const GroundSamples& s, double sx, double sz,
    std::vector<double>& dstX, std::vector<double>& dstY)
{
    dstX.assign(s.srcIndex.size(), kNaN);
    dstY.assign(s.srcIndex.size(), kNaN);
    for (size_t k = 0; k < s.srcIndex.size(); ++k) {
        double px = 0.0;
        double py = 0.0;
        // 🔴 −0.5：projectPoint3 回的是像素**边缘**坐标，本层全程用像素**中心**（见头注那节）。
        if (!projectPoint3(into, {s.wx[k] + sx, 0.0, s.wz[k] + sz}, px, py))
            continue;
        dstX[k] = px - 0.5;
        dstY[k] = py - 0.5;
    }
}

cv::Mat edgeBandMask(const cv::Mat& cov, int rad)
{
    if (cov.empty())
        return {};
    if (cov.type() != CV_64FC1)
        throw std::runtime_error("edgeBandMask 只吃 CV_64FC1 的 cov 场（seamCovField 的产物）");
    if (rad <= 0)
        return {};  // 0 = 不侵蚀（债-139 原口径），返回空图由调用侧当「全不在带内」处理
    cv::Mat bin(cov.size(), CV_8UC1);
    for (int y = 0; y < cov.rows; ++y)
        for (int x = 0; x < cov.cols; ++x)
            bin.at<uint8_t>(y, x) = shadowed(cov.at<double>(y, x)) ? 255 : 0;
    // 带 = 膨胀 − 腐蚀，即「距影缘 ≤ rad 的像素」。
    // 🔴 首版这里写的是 MORPH_CLOSE 与 MORPH_OPEN 取 absdiff，那是**恒零**的：闭运算与开运算对
    //   半平面这种凸区域都等于原区域（闭填小洞、开削小突起，两者都不动直边），absdiff 出来全 0
    //   ⟹ 侵蚀带形同不存在 ⟹ 「erode 2 后翻转恰 0」会变成「根本没侵蚀」的假绿。膨胀/腐蚀才是
    //   一对真正朝两侧扩的算子，其差恰为边界带。selftest 的 ⑥（刀锋场零侵蚀必翻）就是这条的判据。
    // 用形态学而不是逐像素 rad² 邻域循环：本函数一次采集要出 2 张带图，而带图**与候选平移无关**
    //（它长在各自的 cov 网格上），所以全程只算 2 次；但平移搜索要跑 240+ 次对拍，任何常数都值当。
    cv::Mat dilated;
    cv::Mat eroded;
    const cv::Mat se = cv::getStructuringElement(cv::MORPH_RECT, { 2 * rad + 1, 2 * rad + 1 });
    cv::dilate(bin, dilated, se);
    cv::erode(bin, eroded, se);
    cv::Mat band;
    cv::absdiff(dilated, eroded, band);
    return band;
}

cv::Mat photometricTrustMask(const cv::Mat& lumaA, const cv::Mat& lumaB, const GroundSamples& s,
    const std::vector<double>& dstX, const std::vector<double>& dstY, double relTol)
{
    if (lumaA.empty() || lumaA.type() != CV_64FC1 || lumaB.type() != CV_64FC1)
        throw std::runtime_error("photometricTrustMask 只吃 CV_64FC1 的线性亮度图（两臂同域）");
    if (lumaA.size() != lumaB.size())
        throw std::runtime_error("两臂线性亮度图尺寸不同 ⟹ 不是同一次采集的同一画幅");
    if (dstX.size() != s.srcIndex.size() || dstY.size() != s.srcIndex.size())
        throw std::runtime_error("photometricTrustMask：目标坐标长度与采样点集不符");
    cv::Mat keep = cv::Mat::zeros(lumaA.size(), CV_8UC1);
    for (size_t k = 0; k < s.srcIndex.size(); ++k) {
        const int ia = s.srcIndex[k];
        if (!std::isfinite(dstX[k]) || !std::isfinite(dstY[k]))
            continue;
        double vB = 0.0;
        if (!sampleBilinear(lumaB, dstX[k], dstY[k], vB))
            continue;
        const double vA = lumaA.at<double>(ia / lumaA.cols, ia % lumaA.cols);
        // 相对差（带一个绝对底，免得纯黑处 1e-9 vs 2e-9 被判「不吻合」而把天空剔光）。
        if (std::abs(vA - vB) <= relTol * std::max(vA, vB) + 1e-6)
            keep.at<uint8_t>(ia / lumaA.cols, ia % lumaA.cols) = 255;
    }
    return keep;
}

double ShadowFlipReadout::flipPct() const
{
    return trust ? 100.0 * static_cast<double>(flips) / static_cast<double>(trust) : 0.0;
}

double ShadowFlipReadout::meanAbsCovPermille() const
{
    return trust ? 1000.0 * sumAbsCov / static_cast<double>(trust) : 0.0;
}

namespace {

/// 固定分母（🔴 搜索的候选之间要可比，分母就必须固定；理由全文见同名头注的 ①）。
struct FixedDenominator {
    std::vector<int> at;       ///< 进入分母的采样点下标（`GroundSamples::srcIndex` 的第几个）
    std::vector<double> vA;    ///< 对应的 A 侧 cov 值（与 `at` 等长，不随候选变）
};

/// 在零位移的搬运上定义分母（四道闸与 `compareTransported` **同一份**：可信 / 投得出 / 抽头取到 /
/// 不在影缘带）。`sampled` / `edgeExcluded` 顺手带出来给调用侧做覆盖率与地板规模的读数。
FixedDenominator buildFixed(const cv::Mat& covA, const cv::Mat& covB, const GroundSamples& s,
    const std::vector<double>& dx, const std::vector<double>& dy, const cv::Mat& keep,
    const cv::Mat& bandA, const cv::Mat& bandB, size_t& sampled, size_t& edgeExcluded)
{
    FixedDenominator d;
    for (size_t k = 0; k < s.srcIndex.size(); ++k) {
        const int ia = s.srcIndex[k];
        if (!keep.empty() && !keep.at<uint8_t>(ia / covA.cols, ia % covA.cols))
            continue;
        if (!std::isfinite(dx[k]) || !std::isfinite(dy[k]))
            continue;
        double vB = 0.0;
        if (!sampleBilinear(covB, dx[k], dy[k], vB))
            continue;
        ++sampled;
        if (onBandForSample(bandA, bandB, ia % covA.cols, ia / covA.cols, dx[k], dy[k])) {
            ++edgeExcluded;
            continue;
        }
        d.at.push_back(static_cast<int>(k));
        d.vA.push_back(covA.at<double>(ia / covA.cols, ia % covA.cols));
    }
    return d;
}

/// 在固定分母上给一个候选打分。分母里投不出 B 画幅 / 抽头取不到的点**跳过并计入 unreachable**
/// （不记 0 差：把「看不见」当成「一致」是反向取巧），规模由调用侧的覆盖率下限卡住。
struct CandidateScore {
    size_t flips = 0;
    double sumAbs = 0.0;
    size_t trust = 0;
    size_t unreachable = 0;
};

CandidateScore scoreFixed(const FixedDenominator& d, const cv::Mat& covB,
    const std::vector<double>& dx, const std::vector<double>& dy,
    std::vector<double>* diffs = nullptr)
{
    CandidateScore cs;
    if (diffs != nullptr)
        diffs->clear();
    for (size_t j = 0; j < d.at.size(); ++j) {
        const size_t k = static_cast<size_t>(d.at[j]);
        double vB = 0.0;
        if (!std::isfinite(dx[k]) || !std::isfinite(dy[k])
            || !sampleBilinear(covB, dx[k], dy[k], vB)) {
            ++cs.unreachable;
            continue;
        }
        ++cs.trust;
        // 二值影状态 = `cov > kSeamBin`（与 shadow-grain / shadow-cascade-seam 同口径）。
        // 🔴 翻转数与 |Δcov| 是两个统计量，实测会分家：本批四格里 ③ 的翻转率比 ④ **还低**而
        // |Δcov| 高 5.7 倍——只留一个就会读错结论，所以两个都算、都报。
        if (shadowed(d.vA[j]) != shadowed(vB))
            ++cs.flips;
        const double diff = std::abs(d.vA[j] - vB);
        cs.sumAbs += diff;
        if (diffs != nullptr)
            diffs->push_back(diff);
    }
    return cs;
}


}  // namespace

ShadowFlipReadout compareTransported(const cv::Mat& covA, const cv::Mat& covB,
    const GroundSamples& s, const std::vector<double>& dstX, const std::vector<double>& dstY,
    const cv::Mat& keep, const cv::Mat& bandA, const cv::Mat& bandB, int erodePx)
{
    if (covA.empty() || covA.type() != CV_64FC1 || covB.type() != CV_64FC1)
        throw std::runtime_error("compareTransported 只吃 CV_64FC1 的 cov 场（seamCovField 的"
              "产物）；类型错了返回空读数会被读成「零翻转 = 判据通过」");
    if (covA.size() != covB.size())
        throw std::runtime_error("两 cov 场尺寸不同 ⟹ 不是同一次采集的同一画幅，对拍无意义");
    if (dstX.size() != s.srcIndex.size() || dstY.size() != s.srcIndex.size())
        throw std::runtime_error("projectSamples 的产物与采样点集长度不符（调用侧接错了线）");
    // 🔴 单次读数与平移搜索**共用同一对算子**（buildFixed 定义分母、scoreFixed 打分）。这里传的
    // dx/dy 就是「本次读数的搬运」，所以分母定义在这一次搬运上——与搜索侧「分母固定在零位移」
    // 是同一件事在两种调用形态下的写法，不存在第二条四道闸的实现会漂。
    ShadowFlipReadout r;
    r.erodePx = erodePx;
    size_t sampled = 0;
    size_t edgeExcluded = 0;
    const FixedDenominator denom
        = buildFixed(covA, covB, s, dstX, dstY, keep, bandA, bandB, sampled, edgeExcluded);
    r.sampled = sampled;
    r.edgeExcluded = edgeExcluded;
    std::vector<double> diff;
    const CandidateScore cs = scoreFixed(denom, covB, dstX, dstY, &diff);
    r.trust = cs.trust;
    r.flips = cs.flips;
    r.sumAbsCov = cs.sumAbs;
    if (!diff.empty()) {
        // nth_element 而不是 sort：p99 只要那一个分位，O(n) 就够。
        const size_t idx = static_cast<size_t>(0.99 * static_cast<double>(diff.size() - 1));
        std::nth_element(diff.begin(), diff.begin() + static_cast<long>(idx), diff.end());
        r.p99AbsCov = diff[idx];
    }
    return r;
}


ShiftCandidate searchWorldShift(const cv::Mat& covA, const cv::Mat& covB, const GroundSamples& s,
    const cv::Mat& keep, const cv::Mat& bandA, const cv::Mat& bandB, const Projector& into, const std::vector<std::vector<std::array<double, 2>>>& rounds,
    double coverageFloor, const std::array<double, 2>& center)
{
    std::vector<double> dx;
    std::vector<double> dy;
    projectSamples(into, s, 0.0, 0.0, dx, dy);
    size_t sampled = 0;
    size_t edgeExcluded = 0;
    const FixedDenominator denom
        = buildFixed(covA, covB, s, dx, dy, keep, bandA, bandB, sampled, edgeExcluded);
    const CandidateScore zero = scoreFixed(denom, covB, dx, dy);
    ShiftCandidate best;
    best.found = !denom.at.empty();  // 分母为空 ⟹ 整场搜索没有基准，不许报「最优 = 零失配」
    best.flips = zero.flips;
    best.trust = zero.trust;
    best.mismatch = zero.sumAbs;
    std::array<double, 2> cur = center;
    for (const auto& round : rounds) {
        for (const auto& c : round) {
            const std::array<double, 2> at { cur[0] + c[0], cur[1] + c[1] };
            projectSamples(into, s, at[0], at[1], dx, dy);
            const CandidateScore cs = scoreFixed(denom, covB, dx, dy);
            // 覆盖率下限：分母固定了，但候选把大量点评到画幅外仍然不可信（分母是彩票的另一种形态）。
            if (static_cast<double>(cs.trust) < coverageFloor * static_cast<double>(zero.trust))
                continue;
            // 字典序：**Σ|Δcov| → 平移最小 → 翻转数最少 → 覆盖最高**。
            // 🔴 为什么 Σ|Δcov| 排第一而不是翻转数（本批实机测出来的，不是审美）：本靶的**二值
            // 本影只占 0.14 %**（cov > 0.5 只有 2000 像素——这块平地上太阳只占受光的三成，见
            // shadow-edge-ruler 的 kEdgeCoverageFloor 段）。注入 0.5 m 的对照臂上，翻转数在零位移
            // 读 5、在真答案 (0.50, 0.00) 处读 34 —— **翻转数会把正确答案判成更差**，因为影缘像素
            // 本来就少，挪动后落进影里的点少于掉出影的点。同一批点上 Σ|Δcov| 读 5.487 ‰ → 0.382 ‰
            // （14 倍），干净地指回注入量。
            // 首版反过来做（翻转数优先），理由是「|Δcov| 会被挪到双方都无影处取巧」——那条实测
            // 成立的前提是**分母随候选缩水**；分母固定在零位移之后（见 buildFixed）这条路就封了，
            // 于是 |Δcov| 既有力气又不可取巧，翻转数只剩下「分辨率不够」这一个毛病。
            const double bestNorm = best.sx * best.sx + best.sz * best.sz;
            const double candNorm = at[0] * at[0] + at[1] * at[1];
            const bool lessAbs = cs.sumAbs < best.mismatch - 1e-9;
            const bool sameAbs = std::abs(cs.sumAbs - best.mismatch) <= 1e-9;
            // 🔴 同失配时取**平移最小者**：侵蚀带内的平移彼此不可分辨（挪 ≤ 2 像素不产生任何新
            // 差），若同分改取覆盖最高者，最优解会在带内随机漂 ⟹ 「最优平移恰为零位移」变成彩票。
            const bool smallerShift = sameAbs && candNorm < bestNorm;
            const bool fewerFlips = sameAbs && candNorm == bestNorm && cs.flips < best.flips;
            const bool moreTrust = sameAbs && candNorm == bestNorm && cs.flips == best.flips
                && cs.trust > best.trust;
            if (lessAbs || smallerShift || fewerFlips || moreTrust) {
                best.flips = cs.flips;
                best.trust = cs.trust;
                best.mismatch = cs.sumAbs;
                best.sx = at[0];
                best.sz = at[1];
            }
        }
        // 下一轮以本轮最优为中心（细网格绕着 0 转 = 白跑：粗轮报 0.5 而细轮只在 0±0.02 里找）。
        cur = { best.sx, best.sz };
    }
    return best;
}

}  // namespace RGL::imgqa
