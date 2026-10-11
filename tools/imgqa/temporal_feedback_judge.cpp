// temporal-feedback —— fb 曲线聚合与四条判决线（①/②/②′/③）+ 静↔动对照
// （批次 B11 从 `temporal_feedback.cpp` 拆出；缝 = 出表留原文件，判决那台仪器住这里。
// 只用**对齐可信**的曲线组；不可信的逐条点名——它们的大读数是对配对误差的测量。）
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

/// 只用**对齐可信**的曲线组做聚合；不可信的逐条记进 `dropped`（大读数是配对误差的测量）。
void scanFeedbackCurve(FeedbackCurve& cu)
{
    double minDev = std::numeric_limits<double>::max(), maxDev = 0.0, bestFb = 0.0;
    double devAt100 = -1.0, maxFb = -1.0, devAtMaxFb = 0.0, devAt12 = -1.0;
    for (const auto& c : cu.curve) {
        if (c.second < minDev) {
            minDev = c.second;
            bestFb = c.first;
        }
        maxDev = std::max(maxDev, c.second);
        if (c.first > maxFb) {
            maxFb = c.first;
            devAtMaxFb = c.second;
        }
        if (std::abs(c.first - 1.0) < 1e-9)
            devAt100 = c.second;
        if (std::abs(c.first - 0.12) < 1e-9)
            devAt12 = c.second;
    }
    if (devAt100 < 0.0)
        devAt100 = devAtMaxFb;  // 扫描档里没有 1.0 ⟹ 用最大档代替并如实说明
    cu.minDev = minDev;
    cu.maxDev = maxDev;
    cu.bestFb = bestFb;
    cu.devAt100 = devAt100;
    cu.maxFb = maxFb;
    cu.devAtMaxFb = devAtMaxFb;
    cu.devAt12 = devAt12;
}

/// 逆序对数 = fb 更大反而更差的对数（0.5% 容差吸收跨进程噪声）。
int countFeedbackInversions(const std::vector<std::pair<double, double>>& curve)
{
    int inversions = 0;
    for (size_t i = 0; i < curve.size(); ++i)
        for (size_t j = i + 1; j < curve.size(); ++j)
            if (curve[j].first > curve[i].first && curve[j].second > curve[i].second * 1.005)
                ++inversions;
    return inversions;
}

FeedbackCurve buildFeedbackCurve(const std::vector<FeedbackRow>& rows, double devM0)
{
    FeedbackCurve cu;
    for (const auto& r : rows) {
        if (!r.curve)
            continue;
        if (!r.trust) {
            cu.dropped += "F" + d2(r.fb) + "(余量 " + d4(r.margin) + ") ";
            continue;
        }
        cu.curve.push_back({r.fb, r.dev.devEdge});
    }
    std::sort(cu.curve.begin(), cu.curve.end());
    scanFeedbackCurve(cu);
    cu.inversions = countFeedbackInversions(cu.curve);
    cu.spread = cu.maxDev / std::max(cu.minDev, 1e-12);
    cu.rBest = cu.minDev / std::max(devM0, 1e-12);
    cu.r100 = cu.devAt100 / std::max(devM0, 1e-12);
    cu.r12 = cu.devAt12 >= 0.0 ? cu.devAt12 / std::max(devM0, 1e-12) : -1.0;
    return cu;
}

/// 四条判决线（①/②/②′/③）+ 介于两可。
void printFeedbackVerdict(const FeedbackCurve& cu, double devM0)
{
    const double spread = cu.spread, rBest = cu.rBest, r100 = cu.r100, r12 = cu.r12;
    const double bestFb = cu.bestFb, maxFb = cu.maxFb;
    const int inversions = cu.inversions;
    const std::string dropped = cu.dropped;
    std::printf("\n── 判决 ──\n");
    if (!dropped.empty())
        std::printf("  ⚠ 对齐失效、已排除出判决的组：%s（它们的大读数是**配对误差**，不是渲染）\n",
            dropped.c_str());
    std::printf("  回锚 dev(fb=.12)/dev(M0) = %s（已发表 = 1.138）· 地板 dev(M0) = %s\n",
        d4(r12).c_str(), d4(devM0).c_str());
    std::printf("  曲线（%zu 档可信）spread = max dev / min dev = %s（判据①线 1.02）· 逆序对 %d"
                "（0 = 随 fb↑ 单调回落）\n",
        cu.curve.size(), d4(spread).c_str(), inversions);
    std::printf("  min R = %s（fb=%s）· R(fb=1) = %s ⟹ **抖动本身**在运动下的贡献 = %s× 地板"
                "· R(fb=.12) = %s\n",
        d4(rBest).c_str(), d2(bestFb).c_str(), d4(r100).c_str(), d4(r100).c_str(),
        d4(r12).c_str());
    if (cu.curve.size() < 3) {
        std::printf("\n  ★ 可信档位不足 3 档，**拒绝出判决**（对齐失效 %s）\n", dropped.c_str());
    }
    else {
        std::string verdict;
        if (spread < 1.02)
            verdict = "① **偏差与 fb 无关**（spread " + d4(spread)
                + " < 1.02）⟹ 假设**否证**：累积窗长短不是运动态偏差的主因，得换变量找";
        else if (r100 >= 1.0 && rBest >= 1.0)
            verdict = "③ **病灶不在累积，在 jitter 本身**：fb 拉到 1.0（完全不累积）仍是地板的 "
                + d4(r100) + "×，且带内最小也有 " + d4(rBest)
                + "× ⟹ 缩短窗收不回来，**运动下抖动没有被正确抵消**";
        else if (rBest < 1.0 && bestFb < maxFb && r100 > rBest * 1.02)
            verdict = "②′ **方向成立但不是单调**：偏差随 fb↑（窗变短）先回落到带内最优 fb="
                + d2(bestFb) + "（地板的 " + d4(rBest) + "×，已低于 TAA 关），随后 fb→1 又反弹到 "
                + d4(r100) + "× ⟹ 「累积」与「抖动」是**两份独立误差**：前者 fb 能调掉，后者"
                  "（抖动本身 " + d4(r100 - rBest) + "×）调不掉";
        else if (rBest < 1.0)
            verdict = "② **假设成立**：偏差随 fb↑ 单调回落到 fb=" + d2(bestFb) + "（地板的 "
                + d4(rBest) + "×，<1）"
                + (std::abs(bestFb - maxFb) < 1e-9
                        ? "；⚠ 最优在**端点** = 「几乎不累积」，不是「上调 feedback 有最优」"
                        : "；最优在带内，修法 = 运动态上调 feedback");
        else
            verdict = "介于两可：spread " + d4(spread) + "、逆序对 " + std::to_string(inversions)
                + "、minR " + d4(rBest) + "、R(fb=1) " + d4(r100) + "，四条判决线都未越过，"
                  "不能一句话定性";
        std::printf("\n  ★ %s\n", verdict.c_str());
    }
}

/// 静止/运动两侧的 fb 敏感度（「窗棂残余与运动失效是否同一个根」的对照）。
void printFeedbackCrossCheck(const std::vector<FeedbackStillRow>& srows,
    const std::vector<std::pair<double, double>>& curve)
{
    auto swingAt = [&](double fb) {
        for (const auto& r : srows)
            if (std::abs(r.fb - fb) < 1e-9)
                return r.st.meanSwing;
        return 0.0;
    };
    auto fracAt = [&](double fb) {
        for (const auto& r : srows)
            if (std::abs(r.fb - fb) < 1e-9)
                return r.st.fracOverTau;
        return 0.0;
    };
    auto devAt = [&](double fb) {
        for (const auto& c : curve)
            if (std::abs(c.first - fb) < 1e-9)
                return c.second;
        return 0.0;
    };
    const double sSwing = swingAt(0.12) > 0.0 ? swingAt(1.0) / swingAt(0.12) : 0.0;
    const double sFrac = fracAt(0.12) > 0.0 ? fracAt(1.0) / fracAt(0.12) : 0.0;
    const double mDev = devAt(0.12) > 0.0 ? devAt(1.0) / devAt(0.12) : 0.0;
    std::printf("  ★ 统一解释的对照（fb .12→1 的同向变化倍数）：静止 meanSwing ×%s"
                "（frac>τ ×%s）｜ 运动 边缘偏差 ×%s\n",
        d4(sSwing).c_str(), d4(sFrac).c_str(), d4(mDev).c_str());
    std::printf("    读法：两侧**同号**（fb↑ 让同一个量同向变）⟹ 支持「同一个根 = 抖动」；"
                "**反号**（静止变差 / 运动变好或反之）⟹ 否证，窗棂残余与运动失效是两件事\n");
}

}  // namespace RGL::imgqa::tmotion

}  // namespace RGL::imgqa
