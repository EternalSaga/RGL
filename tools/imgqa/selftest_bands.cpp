// selftest_bands.cpp —— **不读图、不启引擎的纯度量层**：bandTableOf 分带 / rejectCrossTab
// 交叉表 / EMA 抑制比 / diffstruct 结构差分（拆文件的缝说明全文见 `selftest.h`）。
//
// 缝 = 「合成数组闭式直接测的度量层」这一条（`diff_struct_metric.h` 与 `bandTableOf` 各自的
// 头注释写的都是同一条缝）：四台仪器的输入都是手摆 fixture，期望值全部是**闭式计数**（每格
// 恰好几个像素、几个 over、质量多少），不抄实现、不吃截图。加新用例先问「它测的是哪台仪器」。
//
// 搬动纪律：断言本体逐字搬自拆分前的 selftest.cpp（一条不多、一条不少、不改判定方向与阈值）。
#include "selftest.h"

#include "temporal_residual.h"
#include "diff_struct_metric.h"
#include "imgqa.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {

/// 分带 ① fixture 第 (亮度档, 梯度档) 那格**该**是什么（闭式手算：除 clip×high 之外每格
/// 1 像素、1 个 over、质量 0.9）。抽成有名小函数的理由 = 认知复杂度护栏（三元与 `&&` 链
/// 嵌在双层循环里 17 > 15）；断言本体一字未动，只是把「该是多少」与「表是不是这样」分两处。
struct SixCellsWants {
    int64_t over;
    double mass;
};

SixCellsWants sixCellsWant(int lb, int gb)
{
    const bool clipHigh = lb == 2 && gb == 1;
    return clipHigh ? SixCellsWants { 0, 0.0 } : SixCellsWants { 1, 0.9 };
}
/// 分带 ①（**六格闭式计数**）：1×6 fixture 把亮度三档 × 梯度两档的全组合各摆一个像素，
/// 每格恰好几个像素、几个 over 全部手算；外加 τ 高于一切与 τ=0 两条反向。
/// 拆出来的直接理由 = 120 行与认知复杂度两条护栏（六格双重循环 + 一串 && 链）。
void checkBandSixCells(Checker& c)
{
    // ① 六格计数：1×6 fixture，亮度 {10,200,250} × 梯度 {0,1} 全组合各 1 像素，
    //    swing 除最后一格外全为 0.9（τ=0.5 之上）。闭式：每格 pixels=1、over=1（除 clip×high
    //    为 0）、mass=0.9（除其为 0）；total=6、over=5、mass=4.5。
    //    变异红线：亮度档 `<= tLo` → mid×low 少一格；梯度档 `>=` 写成 `>` → 两格计数搬家。
    {
        cv::Mat luma(1, 6, CV_64FC1), grad(1, 6, CV_64FC1), swing(1, 6, CV_64FC1);
        const double L[6] = { 10, 10, 200, 200, 250, 250 };
        const double G[6] = { 0, 1, 0, 1, 0, 1 };
        const double S[6] = { 0.9, 0.9, 0.9, 0.9, 0.9, 0.0 };
        for (int x = 0; x < 6; ++x) {
            luma.at<double>(0, x) = L[x];
            grad.at<double>(0, x) = G[x];
            swing.at<double>(0, x) = S[x];
        }
        const BandTable t = bandTableOf(swing, luma, grad, 128.0, 240.0, 0.5, 0.5);
        bool cellsOk = t.total == 6 && t.over == 5 && std::abs(t.mass - 4.5) < 1e-12;
        for (int lb = 0; lb < 3 && cellsOk; ++lb)
            for (int gb = 0; gb < 2 && cellsOk; ++gb) {
                const BandCell& cell = t.cell[lb][gb];
                const SixCellsWants want = sixCellsWant(lb, gb);
                cellsOk = cell.pixels == 1 && cell.over == want.over
                    && std::abs(cell.mass - want.mass) < 1e-12;
            }
        c.chk("分带：六格闭式计数（share/rate/mass 三个数的分子分母各自对）", cellsOk,
            "total=" + std::to_string(t.total) + " over=" + std::to_string(t.over));
        // 反向用例：τ 抬到 swing 之上 ⟹ over **恰为 0**（不是「接近 0」），但像素与质量照计。
        const BandTable hi = bandTableOf(swing, luma, grad, 128.0, 240.0, 0.5, 1e9);
        c.chk("分带：τ 高于一切 → over 恰为 0 而 total/mass 不动（rate 的分母不是 swing）",
            hi.over == 0 && hi.total == 6 && std::abs(hi.mass - 4.5) < 1e-12,
            "over=" + std::to_string(hi.over));
        // τ=0 与 swingFoldCore 同口径（「没有阈值」= 不计数，而不是「全计数」）。
        c.chk("分带：τ=0 不计数（与 fracOverTau 同严格性）",
            bandTableOf(swing, luma, grad, 128.0, 240.0, 0.5, 0.0).over == 0);
    }
}

/// 分带 ②③（**边界归属与前提守卫**）：`< tLo` 暗 / `[tLo,tHi)` 中间调 / `≥ tHi` 削顶 /
/// `≥ gradThr` 高的归属写死并被钉住；尺子与观测量尺寸不符 ⟹ 不出数（total=0）。
void checkBandBoundsAndGuards(Checker& c)
{
    // ② 边界归属（写死并被钉住：`< tLo` 暗 / `[tLo,tHi)` 中间调 / `≥ tHi` 削顶 / `≥ gradThr` 高）。
    //    变异红线：任一 `>=` 改 `>` 或 `<` 改 `<=` → 对应格像素数 -1，本条当场红。
    {
        cv::Mat luma(1, 4, CV_64FC1), grad(1, 4, CV_64FC1), swing(1, 4, CV_64FC1,
            cv::Scalar(1.0));
        const double L[4] = { 128.0, 240.0, 127.999, 239.999 };
        const double G[4] = { 0.5, 0.4, 0.5, 0.5 };
        for (int x = 0; x < 4; ++x) {
            luma.at<double>(0, x) = L[x];
            grad.at<double>(0, x) = G[x];
        }
        const BandTable t = bandTableOf(swing, luma, grad, 128.0, 240.0, 0.5, 0.5);
        c.chk("分带：亮度=128 归中间调、=240 归近削顶、梯度=阈值归高（边界闭式）",
            t.cell[1][1].pixels == 2 && t.cell[2][0].pixels == 1 && t.cell[0][1].pixels == 1
                && t.cell[0][0].pixels == 0 && t.cell[2][1].pixels == 0 && t.total == 4,
            "midHigh=" + std::to_string(t.cell[1][1].pixels)
                + " clipLow=" + std::to_string(t.cell[2][0].pixels));
    }
    // ③ 前提守卫：尺子与观测量尺寸不符 = 不是同一次采集 ⟹ **不出数**（total=0）。
    //    「表看起来完全正常但像素是错位的」是这套仪器里最贵的假绿，与 phaseAverageLuma
    //    拒收混 extent 同一课。变异红线：去掉尺寸校验 → 本条红。
    {
        const cv::Mat swing = cv::Mat::ones(4, 4, CV_64FC1);
        const cv::Mat small = cv::Mat::ones(3, 4, CV_64FC1);
        c.chk("分带：尺子与 swing 尺寸不符 → 不出数（total=0）",
            bandTableOf(swing, small, swing, 128.0, 240.0, 0.5, 0.5).total == 0
                && bandTableOf(swing, swing, small, 128.0, 240.0, 0.5, 0.5).total == 0
                && bandTableOf(cv::Mat(), cv::Mat(), cv::Mat(), 128.0, 240.0, 0.5, 0.5).total
                    == 0,
            "空图与错尺寸一律 total=0");
    }
}

/// 分带 ④（**近削顶阈值的闭式**）：bandClipProxyByte 反解显示链，锚点 240（lin=3）。
void checkBandClipThreshold(Checker& c)
{
    // ④ 近削顶阈值的闭式：byte = round(srgbEncode(ACES(lin))·255)。锚点 240（lin=3）的出处 =
    //    定阈值时的独立 Python 复算（同链复现，见 temporal_residual.h「亮度轴」段）。
    //    变异红线：把阈值改成写死 250 → 240 这条红；ACES 与 sRGB 顺序写反 → 全部漂。
    {
        c.chk("分带阈值：bandClipProxyByte(3.0) = 240（闭式反解显示链，实测定档）",
            bandClipProxyByte(3.0) == 240, std::to_string(bandClipProxyByte(3.0)));
        bool mono = bandClipProxyByte(0.0) == 0 && bandClipProxyByte(1.0) < 240
            && bandClipProxyByte(2.0) < bandClipProxyByte(3.0)
            && bandClipProxyByte(3.0) < bandClipProxyByte(8.0) && bandClipProxyByte(8.0) <= 252;
        c.chk("分带阈值：随线性 HDR 单调、0 给 0、8 仍在肩部（≤252，未削顶）", mono,
            "b1=" + std::to_string(bandClipProxyByte(1.0)) + " b8=" + std::to_string(bandClipProxyByte(8.0)));
        c.chk("分带阈值：lin=1e6 已真削顶 → 255（档位的上端就是「削顶」本身）",
            bandClipProxyByte(1e6) == 255, std::to_string(bandClipProxyByte(1e6)));
    }
}

/// 分带 ⑤（**四数里的胜负手**）：`幅度份额 ≠ 计数份额`——同一张表里像素少的那格幅度上
/// 反而是大头，只出计数份额就会把它读成「不是元凶」。
void checkBandFourNumbers(Checker& c)
{
    // ⑤ 四数里的**胜负手**：`幅度份额 ≠ 计数份额`（批 13.9 新增，移动机位判据的主数）。
    //    fixture（1×4，闭式手算）：mid×high 两像素 swing 0.6/0.6、clip×high 一像素 swing 3.0、
    //    dark×low 一像素 swing 0 ⟹ total=4、over=3、mass=4.2。
    //    闭式：mid×high 计数份额 2/3=66.67% 而幅度份额 1.2/4.2=28.57%；
    //          clip×high 计数份额 1/3=33.33% 而幅度份额 3.0/4.2=71.43%。
    //    ⟹ 同一张表里「像素少的那格」幅度上反而是大头 —— 只出计数份额就会把它读成「不是元凶」。
    //    变异红线：`ampShare` 若退化成用 `over` 算（= 抄 cntShare）→ 71.43 这条当场红；
    //              `mass` 若漏累加（恒 0）→ 两条幅度份额同时红。
    {
        cv::Mat luma(1, 4, CV_64FC1), grad(1, 4, CV_64FC1), swing(1, 4, CV_64FC1);
        const double L[4] = { 200, 200, 250, 10 };
        const double G[4] = { 1, 1, 1, 0 };
        const double S[4] = { 0.6, 0.6, 3.0, 0.0 };
        for (int x = 0; x < 4; ++x) {
            luma.at<double>(0, x) = L[x];
            grad.at<double>(0, x) = G[x];
            swing.at<double>(0, x) = S[x];
        }
        const BandTable t = bandTableOf(swing, luma, grad, 128.0, 240.0, 0.5, 0.5);
        const BandShare mid = bandShareOf(t, 1, 1);
        const BandShare clip = bandShareOf(t, 2, 1);
        const double k = 1e-9;
        c.chk("四数·闭式：mid×high share 50% rate 100% 计数份额 66.67% 幅度份额 28.57%",
            std::abs(mid.share - 0.5) < k && std::abs(mid.rate - 1.0) < k
                && std::abs(mid.cntShare - 2.0 / 3.0) < k && std::abs(mid.ampShare - 2.0 / 7.0) < k,
            "cnt=" + std::to_string(mid.cntShare) + " amp=" + std::to_string(mid.ampShare));
        c.chk("四数·闭式：clip×high 计数份额 33.33% 而幅度份额 71.43%（**两者必须能不等**）",
            std::abs(clip.share - 0.25) < k && std::abs(clip.cntShare - 1.0 / 3.0) < k
                && std::abs(clip.ampShare - 5.0 / 7.0) < k,
            "cnt=" + std::to_string(clip.cntShare) + " amp=" + std::to_string(clip.ampShare));
        // 反向用例：幅度份额与计数份额**不相等**这件事本身要被钉住（相等 = 幅度轴失效）。
        c.chk("四数·反向：同一格里幅度份额 ≠ 计数份额（退化即红，防「只出计数份额」蒙混）",
            std::abs(clip.ampShare - clip.cntShare) > 0.3
                && std::abs(mid.ampShare - mid.cntShare) > 0.3);
        // 空表守卫：total=0 时四个份额一律 0（不是 NaN、不是除零崩溃）。
        const BandTable empty = bandTableOf(swing, luma, grad, 128.0, 240.0, 0.5, 1e9);
        const BandShare e = bandShareOf(empty, 2, 1);
        c.chk("四数·反向：τ 高于一切 → over=0 使计数份额为 0，但幅度份额仍非 0（两轴独立）",
            empty.over == 0 && e.cntShare == 0.0 && e.ampShare > 0.0,
            "amp=" + std::to_string(e.ampShare));
    }
}

/// 分带 ⑥（**移动机位三分支判决**的纯函数面）：支持 / 否证 / 分不出各判各的，外加
/// 「三支名字两两不同且各含自己的分支号」（未命名兜底支吞真信号 = 翻错方向的直接原因）。
void checkMovingBandBranch(Checker& c)
{
    // ⑥ 移动机位三分支判决（批 13.9 判决线的纯函数面）。
    //    昨天的事故 = 真结果落进**未命名**兜底支被读成「没有信号」，故本条除了判分支归属，
    //    还钉住「三支名字两两不同且非空」——名字撞车 = 兜底支吞掉真信号的另一种形态。
    //    变异红线（**逐条实测过**，非声称）：支持线 `>=` 改 `>` → 踩线那条红；否证线 `<` 改 `<=`
    //    → 计数踩线那条红。⚠ 实测同时确认：`!excessInB` 那条守卫**改坏测不出**（支持支先判，
    //    excessInB=true 永远走不到否证支）= 防御性冗余，不是被本条保护的性质，别把它当红线。
    {
        const MovingBandBranch supAmp = classifyMovingBand(kMovingSupportAmpShare, 0.0, false);
        const MovingBandBranch supExc = classifyMovingBand(0.0, 0.0, true);
        const MovingBandBranch ref = classifyMovingBand(
            kMovingRefuteShare - 1e-9, kMovingRefuteShare - 1e-9, false);
        const MovingBandBranch indAmp = classifyMovingBand(0.10, 0.10, false);
        const MovingBandBranch indCnt = classifyMovingBand(
            kMovingRefuteShare - 1e-9, kMovingRefuteShare, false);
        c.chk("判决线·支持：B 幅度份额恰踩 30% 线（含等号）即分支①「支持」",
            supAmp == MovingBandBranch::Support);
        c.chk("判决线·支持：幅度不够但超额集中在 B（rB ≥ 1.5×rA）也判分支①",
            supExc == MovingBandBranch::Support);
        c.chk("判决线·否证：两份额都 < 5% 且超额不向 B → 分支②「否证」",
            ref == MovingBandBranch::Refute);
        // 反向用例 ×2：幅度份额抬到 10%（≥5%）→ 否证支**不许**抢判；
        // 计数份额恰好踩到 5%（不 <）→ 同样不许判否证。
        c.chk("判决线·反向：幅度 10% / 计数踩线 → 一律分支③（否证支不许越线抢判）",
            indAmp == MovingBandBranch::Indeterminate
                && indCnt == MovingBandBranch::Indeterminate);
        // 三支命名必须互不相同（未命名兜底 = 昨天翻错方向的直接原因）。
        const std::string nSup = movingBandBranchName(MovingBandBranch::Support);
        const std::string nRef = movingBandBranchName(MovingBandBranch::Refute);
        const std::string nInd = movingBandBranchName(MovingBandBranch::Indeterminate);
        c.chk("判决线·命名：三支文字两两不同且各含自己的分支号（兜底支不许与另两支同名）",
            !nSup.empty() && nSup != nRef && nRef != nInd && nSup != nInd
                && nSup.find("\xe2\x91\xa0") != std::string::npos
                && nRef.find("\xe2\x91\xa1") != std::string::npos
                && nInd.find("\xe2\x91\xa2") != std::string::npos,
            nSup + " | " + nRef + " | " + nInd);
        // 两条线互斥（同一输入不可能同时命中支持与否证）——判决函数是全函数。
        c.chk("判决线·自洽：支持线与否证线在数值上不可能同时命中（30% > 5%）",
            kMovingSupportAmpShare > kMovingRefuteShare
                && classifyMovingBand(kMovingRefuteShare - 1e-9, 0.0, false)
                    != classifyMovingBand(kMovingSupportAmpShare, 0.0, false));
    }
}

}  // namespace

/**
 * 「亮度 × 梯度」二维分带（批 13.7c）的纯函数自检。
 *
 * 每条带「变异红线」= 把被测实现改坏成什么样子，本条会红。
 * 期望值出处 = 手摆 fixture 的**闭式计数**（每格恰好几个像素、几个 over），不抄实现；
 * `bandClipProxyByte` 的 240 出处 = 显示链闭式（`toBytes`，selftest 另有真机锚点）+
 * 定阈值时的独立 Python 复算（`temporal_residual.h` 同名段）。
 */
void testLumaGradBands(Checker& c)
{
    checkBandSixCells(c);
    checkBandBoundsAndGuards(c);
    checkBandClipThreshold(c);
    checkBandFourNumbers(c);
    checkMovingBandBranch(c);
}

void testRejectCrossTab(Checker& c)
{
    // ① 五桶分档的边界（码值中点 32/96/160/224，raw8 舍入 ±1 被吸收）。
    //    变异红线：任一边界 `32` 改 `31`、或 `<` 改 `<=` → 对应码字搬家，本条当场红。
    {
        cv::Mat a(1, 10, CV_8UC1);
        const uchar v[10] = { 0, 31, 32, 95, 96, 159, 160, 223, 224, 255 };
        for (int x = 0; x < 10; ++x)
            a.at<uchar>(0, x) = v[x];
        const RejectFraction f = rejectFractionOf(a);
        c.chk("拒收：五桶边界（0/31 收下 · 32/95 盒裁剪 · 96/159 塌回 · 160/223 重投影 · 224+ 无历史）",
            std::abs(f.accepted - 0.2) < 1e-12 && std::abs(f.clipped - 0.2) < 1e-12
                && std::abs(f.collapsed - 0.2) < 1e-12 && std::abs(f.reproject - 0.2) < 1e-12
                && std::abs(f.noHistory - 0.2) < 1e-12,
            "acc=" + std::to_string(f.accepted));
        // 五桶必须是**划分**（和恰为 1）——调用侧的自检就靠这条数学事实。
        c.chk("拒收：五桶之和恰为 1（划分；调用侧「和 = 帧数」自检的前提）",
            std::abs(f.accepted + f.clipped + f.collapsed + f.reproject + f.noHistory - 1.0)
                    < 1e-12,
            "");
        // 反向用例：空图 / 类型不符 ⟹ 全 0（**不出数**，而不是「全收下」那种看着正常的假绿）。
        const RejectFraction e = rejectFractionOf(cv::Mat());
        const cv::Mat wrongType(2, 2, CV_32SC1, cv::Scalar(0));
        c.chk("拒收：空图与非 CV_8UC1 一律全 0（不出数 ≠ 全收下）",
            e.accepted == 0.0 && e.noHistory == 0.0 && rejectFractionOf(wrongType).accepted == 0.0,
            "");
    }
    // ② 交叉表五档的边界（0 / 1–2 / 3–8 / 9–16 / 17+）+ τ 严格大于。
    //    变异红线：档位边界 `<= 2` 改 `< 2` → 1 帧档搬家；`>` 改 `>=` → τ 上的像素少计。
    {
        cv::Mat r(1, 10, CV_32SC1), s(1, 10, CV_64FC1);
        const int32_t rv[10] = { 0, 0, 1, 2, 3, 8, 9, 16, 17, 32 };
        const double sv[10] = { 0.9, 0.0, 0.9, 0.0, 0.9, 0.0, 0.9, 0.0, 0.9, 0.0 };
        for (int x = 0; x < 10; ++x) {
            r.at<int32_t>(0, x) = rv[x];
            s.at<double>(0, x) = sv[x];
        }
        const RejectCrossTab t = rejectCrossTab(r, s, 0.5);
        bool ok = t.total == 10 && t.over == 5 && std::abs(t.mass - 4.5) < 1e-12
            && t.bin[0].pixels == 2 && t.bin[1].pixels == 2 && t.bin[2].pixels == 2
            && t.bin[3].pixels == 2 && t.bin[4].pixels == 2;
        ok = ok && t.bin[0].over == 1 && t.bin[1].over == 1 && t.bin[4].over == 1
            && std::abs(t.bin[0].mass - 0.9) < 1e-12;
        c.chk("拒收交叉表：五档闭式计数（share/rate/mass 的分子分母各自对）", ok,
            "total=" + std::to_string(t.total) + " over=" + std::to_string(t.over));
        // 反向用例：τ 抬到 swing 之上 ⟹ over **恰为 0**，但像素与质量照计（rate 的分母不是 swing）。
        const RejectCrossTab hi = rejectCrossTab(r, s, 1e9);
        c.chk("拒收交叉表：τ 高于一切 → over 恰为 0 而 total/mass 不动",
            hi.over == 0 && hi.total == 10 && std::abs(hi.mass - 4.5) < 1e-12,
            "over=" + std::to_string(hi.over));
        // 前提守卫：两侧尺寸/类型不符 = 不是同一次采集 ⟹ **不出数**（同 bandTableOf）。
        const cv::Mat small = cv::Mat::ones(4, 4, CV_32SC1);
        const cv::Mat swingBig = cv::Mat::ones(5, 4, CV_64FC1);
        c.chk("拒收交叉表：尺寸/类型不符 → total=0（错位像素不出表）",
            rejectCrossTab(small, swingBig, 0.5).total == 0
                && rejectCrossTab(cv::Mat(), cv::Mat(), 0.5).total == 0
                && rejectCrossTab(small, small, 0.5).total == 0,  // 两侧同尺寸但类型错
            "");
    }
    // ③ 判决三分支（线跑前写死：支持 = r0 ≤ 0.20×rAll 且 m3p ≥ 0.50；否证 = r0 ≥ 0.80×rAll）。
    //   fixture 直接搭 RejectCrossTab（不经过 rejectCrossTab），把「判决」与「计数」分开钉。
    {
        auto make = [](int64_t b0px, int64_t b0over, double m3, int64_t total) {
            RejectCrossTab t;
            t.total = total;
            t.bin[0].pixels = b0px;
            t.bin[0].over = b0over;
            t.mass = 1.0;
            t.bin[2].mass = m3;
            return t;
        };
        // 支持：0 档 1000/10000 像素只 1 个 over（r0/rAll 远小于 0.2），质量 0.6 住在 ≥3 档。
        RejectCrossTab sup = make(1000, 1, 0.6, 10000);
        sup.over = 500;  // rAll = 5%，r0 = 0.1% ⟹ 比值 0.02
        c.chk("拒收判决：0 档安静 + 质量住 ≥3 档 ⟹ 支持",
            classifyReject(sup, 0.1) == RejectBranch::Support, "");
        // 否证：0 档与全帧同率（r0 = 5% = rAll）。
        RejectCrossTab ref = make(1000, 50, 0.1, 10000);
        ref.over = 500;
        c.chk("拒收判决：0 档与全帧同率 ⟹ 否证（残余 = EMA 泄漏，动盒是白动）",
            classifyReject(ref, 0.1) == RejectBranch::Refute, "");
        // 灰带：0 档率居中（r0/rAll = 0.5）⟹ 分不出（不许硬掰成任一端）。
        RejectCrossTab mid = make(1000, 25, 0.6, 10000);
        mid.over = 500;
        c.chk("拒收判决：灰带（r0/rAll = 0.5）⟹ 分不出",
            classifyReject(mid, 0.1) == RejectBranch::Indeterminate, "");
        // 前置门槛（反向用例）：0 档面积 < 5% ⟹ **没有「老实累积」的对照**，即使数字像支持也不许判。
        c.chk("拒收判决：0 档面积不足 5% ⟹ 分不出（无对照不许比）",
            classifyReject(sup, 0.01) == RejectBranch::Indeterminate, "");
        // 空表 / 全帧不闪 ⟹ 分不出（无从判起，而不是「支持」）。
        c.chk("拒收判决：空表与 rAll=0 ⟹ 分不出",
            classifyReject(RejectCrossTab{}, 0.5) == RejectBranch::Indeterminate
                && [&] {
                       RejectCrossTab z = make(1000, 0, 0.6, 10000);
                       return classifyReject(z, 0.1) == RejectBranch::Indeterminate;
                   }(),
            "");
    }
}

void testBoostFidelity(Checker& c)
{
    // ① 闭式传递 |H(α,k)| 的三条数学边界 + 一条**文档锚点**。
    //    变异红线：把 `hypot(re,im)` 改成 `abs(re)+abs(im)` → 锚点 0.3115 → 0.3155 红；
    //    把 α 与 (1−α) 写反（本仓在 feedback 语义上翻过一次车）→ DC 增益不再是 1，红。
    {
        c.chk("闭式 |H|：DC 增益 k=0 必须恰为 1（任何 α；EMA 不改直流）",
            std::abs(emaTransmittance(0.12, 0) - 1.0) < 1e-12
                && std::abs(emaTransmittance(0.01, 0) - 1.0) < 1e-12,
            "");
        c.chk("闭式 |H|：α=1（完全不累积）必须恰为 1（输出 = 输入，无滤波）",
            std::abs(emaTransmittance(1.0, 1) - 1.0) < 1e-12
                && std::abs(emaTransmittance(1.0, 8) - 1.0) < 1e-12,
            "");
        // 文档锚点：configs/scene.lua 与 phase13 头注释写的「α=0.12 漏过 31%」。
        c.chk("闭式 |H|(0.12,k=1) = 0.3115（文档锚点 0.312，configs/scene.lua taa 段）",
            std::abs(emaTransmittance(0.12, 1) - 0.3115) < 5e-4,
            std::to_string(emaTransmittance(0.12, 1)));
        // 方向红线：本帧权重调**大** = 更信本帧 = 漏得更多。
        c.chk("闭式 |H| 对 α 单调增（本帧权重越大漏越多；方向反了整条判据反向）",
            emaTransmittance(0.01, 1) < emaTransmittance(0.12, 1)
                && emaTransmittance(0.12, 1) < emaTransmittance(0.5, 1),
            "");
    }
    // ② 抑制比 ρ*：恒等 = 1、方向、以及**形状无关性**（本判据成立的地基）。
    //    变异红线：把 period 写死成 8 → 跨谐波散布从 5% 跳到 >40%，本条红。
    {
        c.chk("抑制比 ρ*(α,α) 恰为 1（同档比自己 = 没压；判据的分母分子同式）",
            std::abs(emaSuppressionRatio(0.12, 0.12) - 1.0) < 1e-12, "");
        c.chk("抑制比 ρ*(0.01,0.12) = 0.0827（闭式；= 12.1× 抑制）",
            std::abs(emaSuppressionRatio(0.01, 0.12) - 0.0827) < 5e-4,
            std::to_string(emaSuppressionRatio(0.01, 0.12)));
        c.chk("抑制比方向：floor 抬到 0.05 → ρ* 变大（压得更少）",
            emaSuppressionRatio(0.05, 0.12) > emaSuppressionRatio(0.01, 0.12), "");
        double lo = 1e9, hi = 0.0;
        for (int k = 1; k <= 8; ++k) {
            const double r = emaSuppressionRatio(0.01, 0.12, k);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        // 5.0% 散布 ⟹ 「细线是方波还是尖峰」不影响这条线。放宽到 6% 只兜浮点，不留给人调。
        c.chk("抑制比跨谐波散布 ≤ 6%（k=1..8；形状无关性 = 本判据不需要知道内容）",
            hi / lo <= 1.06, "spread=" + std::to_string(hi / lo));
    }
    // ③ 逐档抑制比表：闭式 fixture（每档两态平均已知）+ 前提守卫 + 除零。
    //    变异红线：档位口径与 rejectCrossTab 分家（改本函数的 rejectBinIndexOf 调用）→ 搬家红。
    {
        cv::Mat on(1, 5, CV_64FC1), off(1, 5, CV_64FC1), r(1, 5, CV_32SC1);
        const int32_t rv[5] = { 0, 1, 4, 12, 20 };
        const double onv[5] = { 0.01, 0.10, 0.10, 0.10, 0.10 };
        const double offv[5] = { 0.20, 0.10, 0.20, 0.10, 0.10 };
        for (int x = 0; x < 5; ++x) {
            r.at<int32_t>(0, x) = rv[x];
            on.at<double>(0, x) = onv[x];
            off.at<double>(0, x) = offv[x];
        }
        const SuppressionTable t = suppressionByRejectBin(on, off, r);
        bool ok = t.valid && t.total.pixels == 5 && t.bin[0].pixels == 1
            && std::abs(t.bin[0].rho - 0.05) < 1e-12   // 0.01/0.20
            && std::abs(t.bin[1].rho - 1.0) < 1e-12    // 0.10/0.10
            && std::abs(t.bin[2].rho - 0.5) < 1e-12    // 0.10/0.20
            && std::abs(t.total.rho - (0.41 / 0.70)) < 1e-12;  // 面积加权的平均之比
        c.chk("逐档抑制比：五档 + 全帧闭式（ρ 是「平均之比」不是「比的平均」）", ok,
            "total rho=" + std::to_string(t.total.rho));
        // 反向用例：swingOff 全 0（门关不闪）⟹ ρ = 0 而不是 NaN/inf（除零守卫）。
        const SuppressionTable z = suppressionByRejectBin(on, cv::Mat::zeros(1, 5, CV_64FC1), r);
        c.chk("逐档抑制比：分母为 0 → ρ = 0（不产 NaN，「门关不闪」不能被读成无穷好）",
            z.valid && std::isfinite(z.total.rho) && z.total.rho == 0.0, "");
        // 前提守卫：三者尺寸/类型不符 ⟹ 不出数。
        c.chk("逐档抑制比：尺寸或类型不符 → valid=false（错位像素不出表）",
            !suppressionByRejectBin(on, off, cv::Mat::ones(3, 5, CV_32SC1)).valid
                && !suppressionByRejectBin(cv::Mat(), cv::Mat(), cv::Mat()).valid
                && !suppressionByRejectBin(on, off, cv::Mat::ones(1, 5, CV_8UC1)).valid,
            "");
    }
}

/// `diffstruct` 的**口径层**（`diffStructStat` / `diffStructRatio`）闭式自检（批 15.2 子步 D1）。
/// 缝 = 判据本体：纯度量，不读图、不启引擎（同 `shadow_kernel_metric` 的拆法）。实机对拍只能
/// 证明「与上次一样」，这里钉的是「算的就是它声称的那个式子」。本条钉四类：已知分布的分位数
/// 精确值 / 全非负 ⟹ 变暗恰 0 / 注入 N 个变暗 ⟹ 计数恰 N / ε 门槛的台阶闭式。
/// 域交叉（gbuf）与前提守卫在同名 `…DomainGuard`（合起来认知复杂度 16 > 15）。
void testDiffStructMetric(Checker& c)
{
    const cv::Size sz(10, 10);
    const auto flat = [&](double v) { return cv::Mat(sz, CV_64FC1, cv::Scalar(v)); };

    // ① 已知分布的闭式分位数：Δ_i = +0.01·(i+1)，i = 0..99 ⟹ 差分集合 |Δ| = {0.01 … 1.00}，
    //    nearest-rank `sorted[floor(q·99)]` 给 p10/p50/p90 = **0.10 / 0.50 / 0.90**、
    //    p90/p10 = **9.0**。变异红线：floor→round（p10 变 0.11）、q·(n−1)→q·n（三条全漂）、
    //    比值取反（9.0 → 0.111）都立刻红。
    {
        cv::Mat a(sz, CV_64FC1);
        for (int i = 0; i < 100; ++i)
            a.at<double>(i / sz.width, i % sz.width) = 0.01 * (i + 1);
        const DiffStructStat st = diffStructStat(a, flat(0.0), {}, 0.40, 1e-6);
        c.chk("diffstruct：全变亮 100 px、变暗恰 0（正负号分配）",
            st.diff == 100 && st.brighter == 100 && st.darker == 0, std::to_string(st.diff));
        c.chk("diffstruct：p10/p50/p90 = 0.10/0.50/0.90（nearest-rank 闭式精确值）",
            std::abs(st.p10 - 0.10) < 1e-12 && std::abs(st.p50 - 0.50) < 1e-12
                && std::abs(st.p90 - 0.90) < 1e-12,
            std::to_string(st.p10) + "/" + std::to_string(st.p50) + "/" + std::to_string(st.p90));
        c.chk("diffstruct：p90/p10 = 9.0（结构对比度 = 本判据的主读数）",
            st.ratioFinite && std::abs(st.ratio - 9.0) < 1e-9, std::to_string(st.ratio));
        c.chk("diffstruct：无变暗像素时 minDarkDelta = 0（不是 -inf、不是未初始化）",
            st.minDarkDelta == 0.0);
    }
    // ② 阴性对照：同图自比 ⟹ 差分集合**为空**。空集合的比值必须是 NaN 而不是 0——写 0 会被
    //    下游读成「p90/p10 = 0 = 结构完全平」= 把「量具取不出数」读成满分。
    {
        const DiffStructStat st = diffStructStat(flat(0.3), flat(0.3), {}, 0.40, 1e-6);
        c.chk("diffstruct：同图自比 → 差分集合为空（三个计数恰 0）",
            st.diff == 0 && st.darker == 0 && st.brighter == 0);
        c.chk("diffstruct：空集合的比值 = NaN 且 ratioFinite=false（不是 0）",
            !std::isfinite(st.ratio) && !st.ratioFinite);
        c.chk("diffstruct：rough 缺席时 hasRough=false（域交叉不做，不假装 0）",
            !st.hasRough && st.darkOutsideGate == 0 && st.gatePixels == 0);
    }
    // ③ 注入 N 个变暗像素 ⟹ 计数恰 N。这是 `diffstruct-dark-anchor` 的算子级保证：判据线是
    //    「> 0」，而它数的是**方向**，所以正负号分配错（把变亮数成变暗）在本条就红。
    {
        cv::Mat a = flat(0.5), b = flat(0.5);
        for (int i = 0; i < 7; ++i)
            b.at<double>(0, i) = 0.5 + 0.01 * (i + 1);  // Δ = −0.01 … −0.07（变暗）
        for (int i = 7; i < 17; ++i)
            a.at<double>(i / sz.width, i % sz.width) = 0.52;  // Δ = +0.02（变亮）
        const DiffStructStat st = diffStructStat(a, b, {}, 0.40, 1e-6);
        c.chk("diffstruct：注入 7 个变暗 → darker 恰 7、brighter 恰 10、diff 恰 17",
            st.darker == 7 && st.brighter == 10 && st.diff == 17,
            std::to_string(st.darker) + "/" + std::to_string(st.brighter));
        c.chk("diffstruct：变暗 Δ 最小值 = 最负者 −0.07（不是首个、不是绝对值）",
            std::abs(st.minDarkDelta + 0.07) < 1e-12, std::to_string(st.minDarkDelta));
    }
    // ⑦ 门槛 ε = 1e-6 与「PNG 字节有任何不同」**逐位等量**的闭式依据：最小可量化亮度台阶
    //    = 0.0722·srgbDecode(1/255) ≈ 2.2e-5（权重最小的通道动 1 LSB），它远大于 ε；ε 之下的
    //    差（本口径 = 字节相同）不计入差分。两个方向都钉，否则门槛是摆设。
    {
        const double step = 0.0722 * srgbDecode(1.0 / 255.0);
        c.chk("diffstruct：1 LSB 的最小亮度台阶 ≈ 2.2e-5 ≫ ε=1e-6（两口径等量的依据）",
            step > 2.1e-5 && step < 2.3e-5, std::to_string(step));
        cv::Mat a = flat(0.5), b = flat(0.5);
        a.at<double>(0, 0) = 0.5 + step;
        c.chk("diffstruct：台阶大小的差计入差分集合（ε 没把真差吃掉）",
            diffStructStat(a, b, {}, 0.40, 1e-6).diff == 1);
        a.at<double>(0, 0) = 0.5 + 1e-7;
        c.chk("diffstruct：ε 之下的差不计，而 ε=0 时它必须计（反向：门槛不是摆设）",
            diffStructStat(a, b, {}, 0.40, 1e-6).diff == 0
                && diffStructStat(a, b, {}, 0.40, 0.0).diff == 1);
    }
}

/// `diffstruct` 口径层·**域交叉与前提守卫**（上一条的认知复杂度已到护栏上限 15，而缝也正当：
/// 上面测的是「两幅亮度图 → 读数」，这里测的是「第三幅 roughness 图进来之后」与「喂错了怎样」）。
void testDiffStructDomainGuard(Checker& c)
{
    const cv::Size sz(10, 10);
    const auto flat = [&](double v) { return cv::Mat(sz, CV_64FC1, cv::Scalar(v)); };

    // ④ gbuf 域交叉（`diffstruct-dark-domain` 的 FAIL 形态）：变暗 ∧ rough ≥ gate 必须数得出来。
    {
        cv::Mat a = flat(0.5), b = flat(0.5);
        const double darkRough[7] = { 0.05, 0.06, 0.07, 0.08, 0.09, 0.10, 0.50 };
        cv::Mat rough = flat(0.2);
        for (int i = 0; i < 7; ++i) {
            b.at<double>(0, i) = 0.5 + 0.01 * (i + 1);
            rough.at<double>(0, i) = darkRough[i];  // 六档在门内，第七档 0.50 在门外
        }
        rough.at<double>(sz.height - 1, sz.width - 1) = 0.0;  // 天空：g=0 排除在分母之外
        const DiffStructStat st = diffStructStat(a, b, rough, 0.40, 1e-6);
        c.chk("diffstruct：变暗 ∧ rough≥gate → darkOutsideGate 恰 1（域外变暗 = FAIL 形态）",
            st.hasRough && st.darkOutsideGate == 1 && st.darker == 7,
            std::to_string(st.darkOutsideGate));
        // 过门控 = **全图** `0<rough<gate`：100 − 1（门外 0.50）− 1（天空 0.0）= 98，而差分集合
        // 只有 7。首版把这句写进了差分循环（只数变暗像素 ⟹ 分母被读成 7），本条钉住那个 bug。
        c.chk("diffstruct：过门控像素按全图口径数（98 ≫ 差分集 7，g=0 天空除外）",
            st.gatePixels == 98, std::to_string(st.gatePixels));
        c.chk("diffstruct：变暗像素 roughness 分布 min/p50/max = 0.05/0.08/0.50（闭式）",
            st.roughDistValid && std::abs(st.roughMin - 0.05) < 1e-12
                && std::abs(st.roughP50 - 0.08) < 1e-12 && std::abs(st.roughMax - 0.50) < 1e-12,
            std::to_string(st.roughP50));
    }
    // ④b 命中率（§3.2 未还账⑤的算子级保证）：**门内 8 px、其中 2 px 命中 → 恰 0.25**。
    //     分子 = |Δ|>ε ∧ 过门控 的**交集**——门外的差分与门内的未命中都不许进分子/分母，
    //     所以构造三个干扰项：门外差分 ×2（进 diff 不进 gateDiff）、门内无差分 ×6（进分母
    //     不进分子）、天空差分 ×1（两边都不进）。变异红线：分子漏与门控求交 → 4/8；
    //     分母漏「g=0 除外」→ 8/9。
    {
        cv::Mat a = flat(0.5), b = flat(0.5);
        cv::Mat rough = flat(0.5);  // 出厂全干（≥gate）：分母从 0 起步
        for (int i = 0; i < 8; ++i)
            rough.at<double>(0, i) = 0.2;  // 门内 8 px
        b.at<double>(0, 0) = 0.51;         // 门内命中（变暗）
        a.at<double>(0, 1) = 0.52;         // 门内命中（变亮）
        a.at<double>(0, 9) = 0.53;         // 门外差分（干扰项：进 diff 不进分子）
        b.at<double>(9, 9) = 0.54;         // 门外差分（同上）
        rough.at<double>(5, 5) = 0.0;      // 天空
        a.at<double>(5, 5) = 0.55;         // 天空差分（干扰项：两边都不进）
        const DiffStructStat st = diffStructStat(a, b, rough, 0.40, 1e-6);
        c.chk("diffstruct：门内 8 px 命中 2 → gateDiff=2、gatePixels=8、命中率恰 0.25",
            st.gatePixels == 8 && st.gateDiff == 2 && st.hitRate == 0.25,
            std::to_string(st.gateDiff) + "/" + std::to_string(st.gatePixels));
        c.chk("diffstruct：门外/天空差分不进命中率分子（diff=5 但 gateDiff=2）",
            st.diff == 5 && st.gateDiff == 2, std::to_string(st.diff));
    }
    // ④c 命中率的分母 0 形态：全图无过门控像素 → **NaN 不是 0**（同 ratio 的纪律：
    //     「量具取不出数」≠「一个都没命中」）。rough 缺席时同为 NaN（上面 ② 已覆盖缺席）。
    {
        const cv::Mat rough = flat(0.5);  // 全干，无天空也无湿面
        const DiffStructStat st = diffStructStat(flat(0.5), flat(0.4), rough, 0.40, 1e-6);
        c.chk("diffstruct：过门控分母 = 0 → 命中率 NaN（不是 0）",
            st.gatePixels == 0 && st.gateDiff == 0 && !std::isfinite(st.hitRate));
    }
    // ⑤ p10 = 0 → +inf。⚠ 这是**防御分支**：差分集合按 |Δ|>ε 构造，p10 数学上取不到 0，
    //    所以只能从公开 helper 钉（走 diffStructStat 永远走不到）。它防的是 ε 被显式给 0
    //    之类的调用形态——比值无定义 ≠ 比值是 0。
    c.chk("diffstruct：p10 = 0 → 比值 +inf（不崩、不写成 0；防御分支）",
        std::isinf(diffStructRatio(0.9, 0.0)) && diffStructRatio(0.9, 0.0) > 0.0);
    c.chk("diffstruct：p10 > 0 → 比值 = p90/p10（0.9/0.3 = 3.0）",
        std::abs(diffStructRatio(0.9, 0.3) - 3.0) < 1e-12);
    // ⑥ 前提守卫：错位/错型的两幅图不是同一次采集，出数就是假绿（同 bandTableOf 的纪律）。
    c.chk("diffstruct：A/B 尺寸不同立刻抛（像素对不上不出数）",
        throwsStd([&] {
            (void)diffStructStat(flat(0.3), cv::Mat(4, 4, CV_64FC1, cv::Scalar(0.3)), {}, 0.4,
                1e-6);
        }));
    c.chk("diffstruct：空图（读图失败传空 Mat）立刻抛",
        throwsStd([&] { (void)diffStructStat(cv::Mat(), flat(0.3), {}, 0.4, 1e-6); }));
    c.chk("diffstruct：非 CV_64FC1（字节域混进线性域）立刻抛",
        throwsStd([&] {
            (void)diffStructStat(cv::Mat(sz, CV_8UC1, cv::Scalar(3)), flat(0.3), {}, 0.4, 1e-6);
        }));
    c.chk("diffstruct：gbuf 与亮度图尺寸不同立刻抛（采错帧/采错档）",
        throwsStd([&] {
            (void)diffStructStat(flat(0.3), flat(0.2), cv::Mat(4, 4, CV_64FC1, cv::Scalar(0.2)),
                0.4, 1e-6);
        }));
}

}  // namespace RGL::imgqa
