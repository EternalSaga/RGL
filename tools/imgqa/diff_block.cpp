// imgqa diffblock —— 差异的**形态判据**：两图之差里「有没有整块」（★0.5-b1）。
//
// 判据本体与全部口径 → `diff_block_metric.h`（形式化 = docs/mdi_plan.md §11.2；为什么大多数
// 「整块错」根本不需要本判据——albedo/metalrough/object_id 三条逐字节精确通道零阈值就抓走了
// ——见 §11.1，本算子只管有损通道：最终图像与法线）。
//
// **场景无关是设计约束**（照抄 `diff_struct.cpp` 的输入面）：输入 = 磁盘上两张 PNG，
// 不启动引擎、不读 manifest。A/B 两臂怎么来的（同档两跑、wet/ssr 开关、簇化门开/关）
// 不归本算子管——它只回答「这两张图的差里有没有整块」。
//
// 退出码：PASS（U(r)=0）= 0，FAIL = 1（要能进 just 脚本当闸门）；用法错 2 / 跑挂 3 由
// main 的异常映射兜底。比较域 = 线性亮度（prepareLinear 的 luma，与全部时域判据同一条路）。
#include "diff_block_metric.h"
#include "imgqa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

/// 定点短格式（同 `diff_struct.cpp` 的 dsNum：读数进台账与 JSON，格式单点）。
std::string dbNum(double v, int prec = 6)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    return buf;
}

/// PNG → 线性亮度图（比较域单点 = prepareLinear 的 luma）。顺带回传 BGR 原图给字节差分。
cv::Mat linearLuma(const std::string& path, cv::Mat& bgrOut)
{
    bgrOut = readPngBgr(path);
    Frame f;
    f.bgr = bgrOut;
    prepareLinear(f);
    return f.luma;
}

/// M = { |Δluma| ≥ τ·kSrgbLumaStep }（8UC1 0/255）。τ=0 特判为**严格 > 0**：
/// 「任一位不同」的语义是「有差」，≥ 0 会把整幅图算进 M（判据恒红）。
cv::Mat diffMask(const cv::Mat& lumaA, const cv::Mat& lumaB, double tau)
{
    cv::Mat absDiff;
    cv::absdiff(lumaA, lumaB, absDiff);
    cv::Mat mask;
    cv::compare(absDiff, tau * kSrgbLumaStep, mask, tau > 0.0 ? cv::CMP_GE : cv::CMP_GT);
    return mask;
}

/// U(r) = #{ d > r }（判据与字面 erode 的等价性由 selftest 钉，容差理由 → kDiffBlockErodeTol）。
int64_t uAt(const cv::Mat& dist, int r)
{
    cv::Mat gt;
    cv::compare(dist, static_cast<float>(r), gt, cv::CMP_GT);
    return cv::countNonZero(gt);
}

/// nearest-rank 分位数 `sorted[floor(q·(n−1))]`（与 `diffStructStat` 同式，口径单点）。
double nearestRank(const std::vector<double>& sorted, double q)
{
    const size_t idx = static_cast<size_t>(q * static_cast<double>(sorted.size() - 1));
    return sorted[idx];
}

/// 厚度分布：只统计 M 内像素的 d 值（§11.2 输出①）。M 为空 → thicknessValid 保持 false，
/// 分位**无定义**不是 0（NaN 纪律，同 `diffStructStat` 的空差分集合）。
void thicknessDist(const cv::Mat& mask, const cv::Mat& dist, DiffBlockStat& st)
{
    std::vector<double> d;
    d.reserve(static_cast<size_t>(st.mPixels));
    for (int y = 0; y < mask.rows; ++y) {
        const uchar* rm = mask.ptr<uchar>(y);
        const float* rd = dist.ptr<float>(y);
        for (int x = 0; x < mask.cols; ++x)
            if (rm[x] != 0)
                d.push_back(static_cast<double>(rd[x]));
    }
    if (d.empty())
        return;
    std::sort(d.begin(), d.end());
    st.thicknessP50 = nearestRank(d, 0.50);
    st.thicknessP90 = nearestRank(d, 0.90);
    st.thicknessP99 = nearestRank(d, 0.99);
    st.thicknessMax = d.back();
    st.thicknessValid = true;
}

/// 连通域（8-连通——对角阶梯线是一个域，4-连通会碎成一串单像素，selftest 钉这条）。
/// 每域最大厚度 = 域内 d 的最大值：一遍逐像素扫按 label 归并（1280×720 量级零成本）。
void components(const cv::Mat& mask, const cv::Mat& dist, int top, DiffBlockStat& st)
{
    cv::Mat labels, stats, centroids;
    st.componentCount = cv::connectedComponentsWithStats(mask, labels, stats, centroids, 8, CV_32S)
        - 1;  // 返回值含背景，域数要减一
    std::vector<double> maxD(static_cast<size_t>(std::max<int64_t>(st.componentCount, 0) + 1), 0.0);
    for (int y = 0; y < labels.rows; ++y) {
        const int* rl = labels.ptr<int>(y);
        const float* rd = dist.ptr<float>(y);
        for (int x = 0; x < labels.cols; ++x)
            if (rl[x] > 0)
                maxD[static_cast<size_t>(rl[x])]
                    = std::max(maxD[static_cast<size_t>(rl[x])], static_cast<double>(rd[x]));
    }
    for (int i = 1; i <= st.componentCount; ++i) {
        DiffBlockComponent comp;
        comp.area = stats.at<int>(i, cv::CC_STAT_AREA);
        comp.x = stats.at<int>(i, cv::CC_STAT_LEFT);
        comp.y = stats.at<int>(i, cv::CC_STAT_TOP);
        comp.width = stats.at<int>(i, cv::CC_STAT_WIDTH);
        comp.height = stats.at<int>(i, cv::CC_STAT_HEIGHT);
        comp.maxThickness = maxD[static_cast<size_t>(i)];
        st.components.push_back(comp);
    }
    std::sort(st.components.begin(), st.components.end(),
        [](const DiffBlockComponent& a, const DiffBlockComponent& b) { return a.area > b.area; });
    if (top >= 0 && static_cast<size_t>(top) < st.components.size())
        st.components.resize(static_cast<size_t>(top));
}

/// 报告（§11.2「必须同时输出三样，让阈值是看得见的，不是藏起来的」+ τ=0/τ 两个计数）。
void printDiffBlockReport(const DiffBlockStat& st, const std::string& aPath,
    const std::string& bPath, double tau)
{
    std::printf("画面 %d x %d = %lld px\n", st.width, st.height, static_cast<long long>(st.total));
    std::printf("  A = %s\n  B = %s\n", aPath.c_str(), bPath.c_str());
    std::printf("τ = %s 个 sRGB 量化步长（= %s 线性亮度）；判据半径 r = %d\n",
        dbNum(tau, 2).c_str(), dbNum(tau * kSrgbLumaStep).c_str(), st.radius);
    std::printf("① 差异集合 M（|Δluma| %s τ）: %lld = %.3f%%；任一位不同（τ=0 字节域）: ",
        tau > 0.0 ? "≥" : ">", static_cast<long long>(st.mPixels),
        100.0 * static_cast<double>(st.mPixels) / static_cast<double>(std::max<int64_t>(1, st.total)));
    if (st.anyDiffPixels >= 0)
        std::printf("%lld（两数之差 = 字节不同但不足 1 个亮度台阶）\n",
            static_cast<long long>(st.anyDiffPixels));
    else
        std::printf("未测（需要字节图，本调用只喂了亮度）\n");
    if (st.thicknessValid) {
        std::printf("② 厚度分布（M 内像素，d=到最近非差异像素的距离）p50/p90/p99/max = %s / %s / %s / %s\n",
            dbNum(st.thicknessP50, 2).c_str(), dbNum(st.thicknessP90, 2).c_str(),
            dbNum(st.thicknessP99, 2).c_str(), dbNum(st.thicknessMax, 2).c_str());
    } else {
        std::printf("② 厚度分布：M 为空，分位**无定义**（不是 0）\n");
    }
    std::printf("③ U(r) 曲线（U = #{d > r}，判据 = U(%d) == 0）:", st.radius);
    for (const auto& [r, u] : st.uCurve)
        std::printf("  r=%d: %lld", r, static_cast<long long>(u));
    std::printf("\n");
    if (st.uAtRadius == 0 && st.mPixels > 0)
        std::printf("   （M 非空而 U(%d)=0：差异处处厚度 ≤ %d px——薄差异放行，这是判据的设计形态）\n",
            st.radius, 2 * st.radius);
    std::printf("④ 连通域（8-连通，共 %lld 个，按面积降序前 %zu 个）:\n",
        static_cast<long long>(st.componentCount), st.components.size());
    for (size_t i = 0; i < st.components.size(); ++i) {
        const auto& comp = st.components[i];
        std::printf("   #%zu area=%lld bbox=[%d,%d,%d,%d] max_thickness=%s\n", i + 1,
            static_cast<long long>(comp.area), comp.x, comp.y, comp.width, comp.height,
            dbNum(comp.maxThickness, 2).c_str());
    }
}

/// 机器可读版（★1-a1ii 验收脚本读它——字段名自解释，无定义量出 null 不出 0）。
nlohmann::json diffBlockJson(const DiffBlockStat& st, const std::string& aPath,
    const std::string& bPath, double tau, bool pass)
{
    nlohmann::json j;
    j["operator"] = "diffblock";
    j["a"] = aPath;
    j["b"] = bPath;
    j["width"] = st.width;
    j["height"] = st.height;
    j["total_pixels"] = st.total;
    j["tau_steps"] = tau;
    j["tau_linear_luma"] = tau * kSrgbLumaStep;
    j["radius"] = st.radius;
    j["any_diff_pixels"] = st.anyDiffPixels >= 0 ? nlohmann::json(st.anyDiffPixels) : nullptr;
    j["m_pixels"] = st.mPixels;
    j["thickness"] = nlohmann::json::object();
    if (st.thicknessValid) {
        j["thickness"]["p50"] = st.thicknessP50;
        j["thickness"]["p90"] = st.thicknessP90;
        j["thickness"]["p99"] = st.thicknessP99;
        j["thickness"]["max"] = st.thicknessMax;
    } else {
        j["thickness"] = nullptr;  // M 为空：分位无定义 ≠ 0
    }
    j["u_curve"] = nlohmann::json::array();
    for (const auto& [r, u] : st.uCurve)
        j["u_curve"].push_back({ { "r", r }, { "u", u } });
    j["u_at_radius"] = st.uAtRadius;
    j["pass"] = pass;
    j["components_total"] = st.componentCount;
    j["components_top"] = nlohmann::json::array();
    for (const auto& comp : st.components) {
        j["components_top"].push_back({ { "area", comp.area },
            { "bbox", { { "x", comp.x }, { "y", comp.y }, { "width", comp.width },
                { "height", comp.height } } },
            { "max_thickness", comp.maxThickness } });
    }
    return j;
}

}  // namespace

DiffBlockStat diffBlockStat(const cv::Mat& lumaA, const cv::Mat& lumaB, const cv::Mat& byteDiff,
    double tau, int radius, int top)
{
    // 前提守卫：错位/错型输入不是同一次采集，出数就是假绿（同 `diffStructStat` 的纪律）。
    if (lumaA.empty() || lumaA.type() != CV_64FC1 || lumaB.type() != CV_64FC1
        || lumaA.size() != lumaB.size())
        throw std::invalid_argument(
            "diffBlockStat：两侧 luma 必须同为 CV_64FC1 且同尺寸（不是同一次采集不出数）");
    if (!byteDiff.empty() && (byteDiff.type() != CV_8UC1 || byteDiff.size() != lumaA.size()))
        throw std::invalid_argument("diffBlockStat：byteDiff 必须为 CV_8UC1 且同尺寸（anyChannelDiff 的产物）");
    if (tau < 0.0 || radius < 0 || top < 0)
        throw std::invalid_argument("diffBlockStat：τ ≥ 0、r ≥ 0、top ≥ 0（负数没有判据语义）");

    DiffBlockStat st;
    st.width = lumaA.cols;
    st.height = lumaA.rows;
    st.total = static_cast<int64_t>(lumaA.total());
    st.radius = radius;
    if (!byteDiff.empty())
        st.anyDiffPixels = cv::countNonZero(byteDiff);

    const cv::Mat mask = diffMask(lumaA, lumaB, tau);
    st.mPixels = cv::countNonZero(mask);
    cv::Mat dist;
    cv::distanceTransform(mask, dist, cv::DIST_L2, kDiffBlockDistMaskSize);

    for (const int r : kDiffBlockRadiusCurve)
        st.uCurve.emplace_back(r, uAt(dist, r));
    st.uAtRadius = uAt(dist, radius);
    thicknessDist(mask, dist, st);
    // 连通域**总是**算（top=0 只截断清单，不跳过计数——「共 0 个」在 M 非空时是谎话）。
    components(mask, dist, top, st);
    return st;
}

int64_t erodedAreaReference(const cv::Mat& mask, int r)
{
    if (mask.empty() || mask.type() != CV_8UC1)
        throw std::invalid_argument("erodedAreaReference：mask 必须为非空 CV_8UC1");
    if (r < 0)
        throw std::invalid_argument("erodedAreaReference：r ≥ 0");
    const cv::Mat se = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(2 * r + 1, 2 * r + 1));
    cv::Mat eroded;
    cv::erode(mask, eroded, se);
    return cv::countNonZero(eroded);
}

int cmdDiffBlock(const std::string& aPath, const std::string& bPath, int radius, double tau,
    int top, const std::string& jsonPath)
{
    if (aPath.empty() || bPath.empty())
        throw std::runtime_error(
            "diffblock 需要 --a 与 --b（磁盘上两张 PNG；形态判据对 A/B 对称，方向不进判据）");
    cv::Mat bgrA, bgrB;
    const cv::Mat lumaA = linearLuma(aPath, bgrA);
    const cv::Mat lumaB = linearLuma(bPath, bgrB);
    if (lumaA.size() != lumaB.size())
        throw std::runtime_error("A/B 尺寸不符: " + std::to_string(lumaA.cols) + "x"
            + std::to_string(lumaA.rows) + " vs " + std::to_string(lumaB.cols) + "x"
            + std::to_string(lumaB.rows) + "（两图必须同 extent，否则像素对不上）");

    const cv::Mat byteDiff = anyChannelDiff(bgrA, bgrB);
    const DiffBlockStat st
        = diffBlockStat(lumaA, lumaB, byteDiff, tau, radius, top);
    printDiffBlockReport(st, aPath, bPath, tau);

    const bool pass = st.uAtRadius == 0;
    const Verdict verdict = verdictFor("diffblock-thickness", pass,
        "U(r=" + std::to_string(st.radius) + ") = " + std::to_string(st.uAtRadius)
            + "（要求恰 0）；M = " + std::to_string(st.mPixels) + " px，任一位不同 = "
            + std::to_string(st.anyDiffPixels) + " px，厚度 max = "
            + (st.thicknessValid ? dbNum(st.thicknessMax, 2) : "无定义（M 空）"));
    std::printf("\n");
    std::cout << "  " << verdict.line() << "\n";

    if (!jsonPath.empty()) {
        std::ofstream out(jsonPath);
        if (!out)
            throw std::runtime_error("写不出 --json 文件: " + jsonPath);
        out << diffBlockJson(st, aPath, bPath, tau, pass).dump(2) << "\n";
    }
    logCall("diffblock", { { "a", aPath }, { "b", bPath }, { "r", std::to_string(radius) },
                              { "tau", dbNum(tau, 4) }, { "top", std::to_string(top) },
                              { "json", jsonPath } },
        { verdict });
    return pass ? 0 : 1;
}

}  // namespace RGL::imgqa
