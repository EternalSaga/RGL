// temporal-feedback —— **静止态**对照（窗棂残余 swing + 借掩码口径两列）
// （批次 B11 从 `temporal_feedback.cpp` 拆出；缝 = 采集/守卫留原文件，静止态那台仪器住这里。
// 两列并列出数不判谁对：本命令口径的边缘带来自 REFs，已发表 M4 的口径借 REFm 的掩码。）
#include "temporal_motion.h"

#include "imgqa.h"
#include "temporal_residual.h"
#include "temporal_yaw_remap.h"

#include <algorithm>
#include <cmath>
#include <numeric>  // std::accumulate（阶跃主指标 AUC = Σ ghost(n)）
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
using namespace tmotion;  // 本族跨 TU 的类型 / 常量 / 算子（唯一出处 = 本文件的第一条 include）

namespace tmotion {

/// `Group` 存 luma 序列，而 `phaseSwingStat` / `phaseAverageLuma` 吃 `std::vector<Frame>`。
std::vector<Frame> toFrames(const Group& g)
{
    std::vector<Frame> v;
    v.reserve(g.luma.size());
    for (const auto& m : g.luma) {
        Frame f;
        f.luma = m;
        v.push_back(std::move(f));
    }
    return v;
}


/// 静止态 swing 的 τ 由 fb=0.12 那组定（τ 是**场景×机位**属性不是组属性；跨组换 τ = frac 不可比）。
double feedbackStillTau(const std::vector<std::pair<double, Group*>>& still, int periods)
{
    double tauStill = 0.0;
    for (const auto& [fb, g] : still)
        if (std::abs(fb - 0.12) < 1e-9) {
            const cv::Mat avg = phaseAverageLuma(toFrames(*g), periods);
            auto vals = valuesInMask(avg, {});
            std::sort(vals.begin(), vals.end());
            tauStill = lsbLinearAtLuma(percentileOf(vals, 0.99));
        }
    if (tauStill <= 0.0)
        throw std::runtime_error("静止态 swing 的 τ 定不出来（静止组里没有 fb=0.12？）");
    return tauStill;
}

void collectFeedbackStillRows(std::vector<FeedbackStillRow>& srows,
    const std::vector<std::pair<double, Group*>>& still, const Group& gS0, const Group& gRefs,
    const FeedbackBand& bS, const FeedbackBand& bM, double tauStill, int window, int periods)
{

    auto stillRow = [&](double fb, const Group& g) {
        FeedbackStillRow r;
        r.fb = fb;
        r.warm = g.warm;
        r.st = phaseSwingStat(toFrames(g), {}, tauStill, periods, nullptr);
        const Alignment a = alignSequence(g, gRefs, window, false);
        r.devAligned = measureDeviation(g.luma, gRefs, a, bS.edge, bS.flat, bS.tau);
        r.devBorrowed = measureDeviation(g.luma, gRefs, a, bM.edge, bM.flat, bS.tau);
        if (r.st.meanLuma <= 0.0)
            throw std::runtime_error("静止组画面是黑的（meanLuma=0）——「没画」不是「稳定」");
        srows.push_back(std::move(r));
    };
    for (const auto& [fb, g] : still)
        stillRow(fb, *g);
    stillRow(-1.0, gS0);  // fb = −1 哨兵 = **TAA 关**（静止地板，批 13.8 第 0 步）
}

void printFeedbackStillTable(const std::vector<FeedbackStillRow>& srows, double tauStill)
{
    std::printf("\n── 静止态对照（窗棂残余；swing = 相位折叠 max−median，τ_swing = %s）──\n",
        d4(tauStill).c_str());
    std::printf("  %-8s %5s %10s %10s %8s %10s %8s %10s\n", "fb", "warm", "meanSwing", "p99.9",
        "frac>τ", "边缘偏差", "梯度比", "借掩码偏差");
    for (const auto& r : srows)
        std::printf("  %-8s %5d %10s %10s %7.2f%% %10s %8.3f %10s\n",
            r.fb < 0.0 ? "off(S0)" : d2(r.fb).c_str(), r.warm, d4(r.st.meanSwing).c_str(),
            d4(r.st.p999Swing).c_str(), r.st.fracOverTau * 100.0, d4(r.devAligned.devEdge).c_str(),
            r.devAligned.gradRatio, d4(r.devBorrowed.devEdge).c_str());
    std::printf("  「边缘偏差」的边缘带来自 REFs（与静止候选同视点）；「借掩码偏差」借 REFm 的掩码"
                " = 已发表 M4 的口径（那张表的掩码算自**运动**参考首帧，比静止候选偏 ~5° ⟹ 掩码"
                "与边缘错开、读数被稀释），两列并列出数不判谁对\n");
}

/// 批 13.8 第 0 步读数：**静止态**的「抖动自身代价」= dev(S fb=1) / dev(S0)。
void printFeedbackStillStepZero(const std::vector<FeedbackStillRow>& srows)
{
    auto findRow = [&](double fb) -> const FeedbackStillRow* {
        for (const auto& r : srows)
            if (std::abs(r.fb - fb) < 1e-9)
                return &r;
        return nullptr;
    };
    const FeedbackStillRow* sOff = findRow(-1.0);
    const FeedbackStillRow* sOne = findRow(1.0);
    if (sOff && sOne && sOff->devAligned.devEdge > 1e-12) {
        const double rs = sOne->devAligned.devEdge / sOff->devAligned.devEdge;
        std::printf("  ⭐ **第 0 步**：静止态 dev(fb=1)/dev(TAA 关) = %s"
                    "（运动态同构读数 R(fb=1) 已发表 = 1.0395）\n", d4(rs).c_str());
    }
}

}  // namespace RGL::imgqa::tmotion

}  // namespace RGL::imgqa
