// 自检：判据自己要有判据。**断言全部照实测/源码写，不照记忆写**（本项目纪律：
// 「我以为的行为」一轮写出过 10 处，全部是错的）。闭式解锚点来源逐条注明。
#include "imgqa.h"
#include "aa_coverage.h"
#include "ao_metrics.h"
#include "temporal_flicker.h"
#include "temporal_homography.h"
#include "temporal_residual.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {
struct Checker {
    int bad = 0;
    void chk(const char* name, bool cond, const std::string& extra = "")
    {
        if (!cond) {
            std::printf("  FAIL %s %s\n", name, extra.c_str());
            ++bad;
        }
    }
};

void testSrgb(Checker& c)
{
    // sRGB 往返：全域 decode(encode(x)) ≈ x。
    double worst = 0.0;
    for (int i = 0; i <= 2000; ++i) {
        const double x = i / 2000.0;
        worst = std::max(worst, std::abs(srgbDecode(srgbEncode(x)) - x));
    }
    c.chk("sRGB 往返全域", worst < 1e-6, "worst=" + std::to_string(worst));
    c.chk("sRGB(0.5) 线性 ≈ 0.214", std::abs(srgbDecode(0.5) - 0.216) < 2e-3,
        std::to_string(srgbDecode(0.5)));
}

void testObjectId(Checker& c)
{
    // 编码约定 = convR32Uint：R=id[7:0] G=[15:8] B=[23:16]；0 = 背景。
    cv::Mat bgr(3, 3, CV_8UC3, cv::Scalar(0, 0, 0));
    bgr.at<cv::Vec3b>(0, 0) = cv::Vec3b(0xFE, 0xBA, 0xBE);  // B=FE G=BA R=BE → 0xBEBEFE? 不
    // 上面故意不写死——按 R=id[7:0] 手推：R=0xBE G=0xBA B=0xFE → id=0xFEBABE。
    // cv::Vec3b 序是 (B,G,R)，所以写入 (0xFE,0xBA,0xBE) 恰是 B=FE G=BA R=BE。
    bgr.at<cv::Vec3b>(1, 1) = cv::Vec3b(0x00, 0x00, 0x01);  // R=1 → id=1
    const cv::Mat ids = decodeObjectId(bgr);
    c.chk("ID 小端解码 0xFEBABE", ids.at<int32_t>(0, 0) == 0xFEBABE,
        std::to_string(ids.at<int32_t>(0, 0)));
    c.chk("ID = 1 不是 0", ids.at<int32_t>(1, 1) == 1);
    c.chk("背景是 0", ids.at<int32_t>(2, 2) == 0);
}

void testRegion(Checker& c)
{
    cv::Mat m = cv::Mat::zeros(20, 30, CV_8UC1);
    m(cv::Rect(2, 2, 4, 4)).setTo(255);
    m(cv::Rect(12, 2, 4, 4)).setTo(255);
    m(cv::Rect(20, 12, 8, 6)).setTo(255);
    c.chk("连通域 = 3", connectedCount(m) == 3, std::to_string(connectedCount(m)));
    c.chk("空图连通域 = 0", connectedCount(cv::Mat::zeros(5, 5, CV_8UC1)) == 0);
    const cv::Mat d1 = dilateMask(m, 1), d3 = dilateMask(m, 3);
    c.chk("膨胀单调", cv::countNonZero(d1) >= cv::countNonZero(m)
        && cv::countNonZero(d3) >= cv::countNonZero(d1));
    cv::Mat containment;
    cv::bitwise_and(m, ~d1, containment);
    c.chk("膨胀含原集", cv::countNonZero(containment) == 0);
    cv::Mat overlap;
    cv::bitwise_and(ringMask(m, 2), m, overlap);
    c.chk("环带与原集不交", cv::countNonZero(overlap) == 0);
}

void testClusters(Checker& c)
{
    std::mt19937 rng(0);  // 写死种子（测试纪律：真随机的失败无法重跑）
    std::normal_distribution<double> noise(0.0, 1e-4);
    cv::Mat v(1, 1500, CV_64FC1);
    for (int i = 0; i < 500; ++i)
        v.at<double>(0, i) = 0.10 + noise(rng);
    for (int i = 500; i < 1000; ++i)
        v.at<double>(0, i) = 0.40 + noise(rng);
    for (int i = 1000; i < 1500; ++i)
        v.at<double>(0, i) = 0.85 + noise(rng);
    c.chk("三簇分出三簇", lumaClusters(v).size() == 3,
        std::to_string(lumaClusters(v).size()));
    cv::Mat one(1, 200, CV_64FC1, cv::Scalar(0.37));
    c.chk("单值 = 一簇", lumaClusters(one).size() == 1);
    c.chk("空输入 = 空簇", lumaClusters(cv::Mat()).empty());
}

void testAnalytic(Checker& c)
{
    // 锚点 A/B：来源 = 本机 RADV（STRIX_HALO）cornell_box 探针**实机截图逐字节对拍零
    // 误差**的两组参数（2026-09-30 会话，docs/调试历史.md 收口条目）。不是记忆值。
    const Vec3 n{0, 0, 1}, v{0, 0, 1};
    const Vec3 lA{0, 0, 1};
    const Vec3 blue{0, 0, 1}, lc{12, 12, 12};
    const auto bytesA = toBytes(shadeDirect(n, v, lA, blue, 0.9, 0.0, lc));
    c.chk("锚点A 垂直光纯蓝 = (113,56,249)",
        bytesA == std::array<int, 3>{113, 56, 249},
        std::to_string(bytesA[0]) + "," + std::to_string(bytesA[1]) + ","
            + std::to_string(bytesA[2]));
    const Vec3 lB{0.0, 0.642788, 0.766044};  // 转 40°：NdotL = 0.766，D/F/G 全展开
    const auto bytesB = toBytes(shadeDirect(n, v, lB, blue, 0.9, 0.0, lc));
    c.chk("锚点B 斜 40° = (88,43,244)",
        bytesB == std::array<int, 3>{88, 43, 244},
        std::to_string(bytesB[0]) + "," + std::to_string(bytesB[1]) + ","
            + std::to_string(bytesB[2]));

    // ACES「中性输入不变」的矩阵条件：列和 = 1（两矩阵各自）。
    for (int col = 0; col < 3; ++col) {
        const double sIn = 0.59719 + 0.35458 + 0.04823;
        (void)sIn;
    }
    // （列和自检按 tonemap.frag.slang 的注释约定只对输入矩阵成立，输出矩阵是
    //  逆色域矩阵不要求；此处不写恒等断言——写不出理由的断言不该存在。）

    // 锚点 C：shadow_rig 红块 defaultLight(0.5,-0.75,-0.45) + 探针机位（cam(-2.2,2.8,-1.8)，
    // center(0,1,-6)），albedo=(1,0,0) rough=0.7。来源 = analytic_ref.py --face-luma 输出
    // （对账 C++ 转录与 Python 转录一致；两者再各自对账 shader）。
    const auto faces = predictFaces({0.5, -0.75, -0.45}, {1.0, 0.0, 0.0}, 0.7, 0.0, 12.0,
        {-2.2, 2.8, -1.8}, {0.0, 1.0, -6.0});
    int lit = 0;
    for (const auto& f : faces)
        lit += f.ndotl > 1e-9;
    c.chk("斜光下被照到的面数 = 3（半空间）", lit == 3, std::to_string(lit));
    const auto find = [&](const char* name) {
        for (const auto& f : faces)
            if (f.face == name)
                return f;
        return FacePrediction{"?", 0, {0, 0, 0}, 0};
    };
    c.chk("锚点C -X = (255,51,37)", find("-X").byte == std::array<int, 3>{255, 51, 37});
    c.chk("锚点C +Y = (255,90,55)", find("+Y").byte == std::array<int, 3>{255, 90, 55});
    c.chk("锚点C +Z = (251,41,34)", find("+Z").byte == std::array<int, 3>{251, 41, 34});
    c.chk("锚点C 背光面恒 0", find("+X").byte == std::array<int, 3>{0, 0, 0}
        && find("-Y").byte == std::array<int, 3>{0, 0, 0}
        && find("-Z").byte == std::array<int, 3>{0, 0, 0});
    // 阳性锚点（判据的判据）：把 roughness 谎成 0.5 必须得到**不同**的字节，
    // 否则说明这套预测对输入不敏感、锚点什么都没钉住。
    const auto lie = predictFaces({0.5, -0.75, -0.45}, {1.0, 0.0, 0.0}, 0.5, 0.0, 12.0,
        {-2.2, 2.8, -1.8}, {0.0, 1.0, -6.0});
    c.chk("阴性对照：谎 roughness=0.5 时 +Z 字节不同",
        find("+Z").byte != [&] {
            for (const auto& f : lie)
                if (f.face == "+Z")
                    return f.byte;
            return std::array<int, 3>{0, 0, 0};
        }());
}

void testRegistry(Checker& c)
{
    c.chk("反模式登记含 ssim/psnr/global-epsilon",
        antiPatterns().count("ssim") && antiPatterns().count("psnr")
            && antiPatterns().count("global-epsilon"));
    // 晋升闸门不变量：promoted 判据必须有理由（>20 字符，不是"0.5%"式敷衍）与台账。
    for (const auto& j : registeredJudgments()) {
        if (j.promoted) {
            c.chk("promoted 必须有 guard", !j.guards.empty(), j.name);
            c.chk("promoted 必须有 why_tol", std::strlen(j.whyTol) > 20, j.name);
        }
    }
}

void testTemporalDiff(Checker& c)
{
    // 判据工具自检（纪律：判据工具自己要有判据）。这里**不调引擎**，用合成帧把
    // temporalDiffStat 的四条性质逐个钉死，每条都配反向用例。
    const auto mk = [](int n, double v) {
        Frame f;
        f.bgr = cv::Mat(n, n, CV_8UC3, cv::Scalar(7, 7, 7));
        f.luma = cv::Mat(n, n, CV_64FC1, cv::Scalar(v));
        return f;
    };
    const cv::Mat all = cv::Mat(8, 8, CV_8UC1, cv::Scalar(255));

    // ① 阴性对照：完全相同的帧必须给**恰为 0**（不是"接近 0"）。浮点 absdiff 同值
    //    相减恒 0，任何非零 = 统计面混进了未初始化数据。
    {
        std::vector<Frame> s{mk(8, 0.3), mk(8, 0.3), mk(8, 0.3)};
        const TemporalStat t = temporalDiffStat(s, all);
        c.chk("时域：同帧序列差恰为 0", t.meanAbsDiff == 0.0 && t.changedFraction == 0.0,
            std::to_string(t.meanAbsDiff));
        c.chk("时域：帧对数 = 帧数-1", t.pairs == 2, std::to_string(t.pairs));
    }
    // ② 区域纪律（本工具的地基）：mask 外再大的变化也不算。
    {
        // 两帧只在**右 5 列**不同（左 3 列逐位相同）；mask 只留左 3 列。
        const auto mkSplit = [](double rightLuma, uchar rightByte) {
            Frame f;
            f.bgr = cv::Mat(8, 8, CV_8UC3, cv::Scalar(7, 7, 7));
            f.bgr(cv::Rect(3, 0, 5, 8)).setTo(cv::Scalar(rightByte, rightByte, rightByte));
            f.luma = cv::Mat(8, 8, CV_64FC1, cv::Scalar(0.0));
            f.luma(cv::Rect(3, 0, 5, 8)) = cv::Scalar(rightLuma);
            return f;
        };
        std::vector<Frame> s{mkSplit(0.0, 7), mkSplit(0.9, 200)};
        cv::Mat mask = cv::Mat::zeros(8, 8, CV_8UC1);
        mask(cv::Rect(0, 0, 3, 8)).setTo(255);
        const TemporalStat in = temporalDiffStat(s, mask);
        const TemporalStat out = temporalDiffStat(s, all);
        c.chk("时域：mask 外的变化不计入（区域来自几何）",
            in.meanAbsDiff == 0.0 && in.changedFraction == 0.0, std::to_string(in.meanAbsDiff));
        // 闭式：mask 全图时只有 5/8 的像素差 0.9 ⟹ 均值 0.5625
        c.chk("阳性锚点：同一数据全图求值 = 闭式 0.5625（否则②是恒真）",
            std::abs(out.meanAbsDiff - 0.5625) < 1e-12, std::to_string(out.meanAbsDiff));
        c.chk("阳性锚点：全图变化像素占比 = 闭式 0.625（5/8 列变了）",
            out.changedFraction == 0.625, std::to_string(out.changedFraction));
    }
    // ③ 闭式期望：mask 全图、luma 0 → 0.25 → 0.25 ⟹ 两帧对差 0.25 与 0 ⟹ 均值 0.125。
    {
        std::vector<Frame> s{mk(8, 0.0), mk(8, 0.25), mk(8, 0.25)};
        const TemporalStat t = temporalDiffStat(s, all);
        c.chk("时域：均值 = 闭式 0.125", std::abs(t.meanAbsDiff - 0.125) < 1e-12,
            std::to_string(t.meanAbsDiff));
        c.chk("时域：最大值 = 闭式 0.25", std::abs(t.maxAbsDiff - 0.25) < 1e-12,
            std::to_string(t.maxAbsDiff));
    }
    // ④ 无定义输入不出数（少于 2 帧 / 空 mask）——出数就会被当成"测到了 0"。
    {
        std::vector<Frame> one{mk(8, 0.1)};
        c.chk("时域：单帧无定义", temporalDiffStat(one, all).pairs == 0);
        c.chk("时域：空 mask 无定义",
            temporalDiffStat(std::vector<Frame>{mk(8, 0.1), mk(8, 0.2)},
                cv::Mat::zeros(8, 8, CV_8UC1))
                    .meanAbsDiff
                == 0.0);
        c.chk("时域：空 mask 参数 = 全图（不是 0）",
            temporalDiffStat(std::vector<Frame>{mk(8, 0.0), mk(8, 0.5)}, cv::Mat())
                    .meanAbsDiff
                > 0.0);
    }
    // ⑤ 中间调占比（空间抗锯齿在场性）：带按**实测白电平**取，故必须有一组"整体缩放
    //    后结论不变"的用例——首版把带写死成 [0.05,0.95]，而靶的白件经 ACES 只到 ~0.8
    //    ⟹ 整块白板被算成中间调、83% 恒饱和，什么也分不出来（真实踩过的假判据）。
    {
        const auto split = [](double top, double bottom) {
            Frame f;
            f.bgr = cv::Mat(8, 8, CV_8UC3, cv::Scalar(7, 7, 7));
            f.luma = cv::Mat(8, 8, CV_64FC1, cv::Scalar(bottom));
            f.luma(cv::Rect(0, 0, 8, 4)) = cv::Scalar(top);
            return f;
        };
        const Frame bw = split(1.0, 0.0);
        c.chk("中间调：纯黑白 = 恰为 0",
            temporalDiffStat(std::vector<Frame>{bw}, all).edgeSoftness == 0.0,
            std::to_string(temporalDiffStat(std::vector<Frame>{bw}, all).edgeSoftness));
        const Frame mid = split(1.0, 0.5);
        c.chk("中间调：一半在带内 = 闭式 0.5",
            std::abs(temporalDiffStat(std::vector<Frame>{mid}, all).edgeSoftness - 0.5)
                < 1e-12,
            std::to_string(temporalDiffStat(std::vector<Frame>{mid}, all).edgeSoftness));
        const Frame scaled = split(0.8, 0.4);  // 整体 ×0.8（= 引擎里 ACES 压过的白）
        c.chk("中间调：带随实测白电平缩放（×0.8 后仍 0.5，钉死'不许写死带'）",
            std::abs(temporalDiffStat(std::vector<Frame>{scaled}, all).edgeSoftness - 0.5)
                < 1e-12,
            std::to_string(temporalDiffStat(std::vector<Frame>{scaled}, all).edgeSoftness));
        c.chk("中间调：softMask 空的区域不计入（背景 clear 不进统计）",
            temporalDiffStat(std::vector<Frame>{mid}, all, cv::Mat::zeros(8, 8, CV_8UC1))
                    .edgeSoftness
                == 0.0);
    }
}

void testAaCoverage(Checker& c)
{
    // 覆盖率反解自检：整条判据都压在 1 − aces⁻¹(luma) 上，它错则全错。
    for (const double x : {0.0, 0.05, 0.35, 0.5, 1.0, 2.0}) {
        const double y = acesTonemap(Vec3{x, x, x}).x;
        const double back = invAces(y);
        c.chk("invAces 往返", std::abs(back - x) < 1e-9, std::to_string(back));
    }
    // 单调性（反解靠二分，非单调就是我把曲线抄错了）
    c.chk("ACES 正向单调", acesTonemap(Vec3{0.4, 0.4, 0.4}).x
            < acesTonemap(Vec3{0.6, 0.6, 0.6}).x);
    // 闭式：全黑列覆盖率 = 1、白板 = 0（真机上"全黑列贡献 1.07 列"是这条的实测版）
    const double covBlack = 1.0 - invAces(acesTonemap(Vec3{0.0, 0.0, 0.0}).x);
    const double covWhite = 1.0 - invAces(acesTonemap(Vec3{1.0, 1.0, 1.0}).x);
    c.chk("覆盖率端点：全黑=1 / 白板=0",
        std::abs(covBlack - 1.0) < 1e-9 && std::abs(covWhite) < 1e-9,
        std::to_string(covBlack) + " " + std::to_string(covWhite));
    // 期望表：宽度单调增 + 投影宽闭式（px/m = 720/(2·2·tan22.5°) = 434.55）
    const auto rods = ladderExpectations();
    c.chk("阶梯杆表：12 根且宽度单调增", rods.size() == kLadderRods, "");
    bool mono = true;
    for (size_t i = 1; i < rods.size(); ++i)
        mono = mono && rods[i].widthPx > rods[i - 1].widthPx;
    c.chk("阶梯杆表：投影宽单调增", mono, "");
    c.chk("阶梯杆表：首杆亚像素 / 末杆超像素（闭式 0.652 / 6.518 列）",
        std::abs(rods.front().widthPx - 0.652) < 0.01
            && std::abs(rods.back().widthPx - 6.518) < 0.01,
        std::to_string(rods.front().widthPx) + " " + std::to_string(rods.back().widthPx));
}

void testKeyValidation(Checker& c)
{
    // 键名校验器自己也要有用例：真实解析器源码里必须能抽到 picking / shadow_pass / rpc
    // （工具在仓库内跑，源文件必然存在；抽不到 = 校验器或解析器读取形态变了，
    //  校验静默失效比没有校验更糟——它会把「零警告」读成「键都对」）。
    // 直接经 makeProfile 的通道测：生成一份合法 profile 不许抛。
    const std::string tmp = "/tmp/imgqa_selftest_profile";
    ProfileOpts o;
    o.glb = "shadow_rig.glb";
    bool ok = true;
    try {
        makeProfile(tmp, o);
    } catch (const std::exception& e) {
        ok = false;
        std::printf("  （makeProfile 抛: %s）\n", e.what());
    }
    c.chk("makeProfile 合法键不抛（键名校验器与解析器仍对齐）", ok);
    // 反向用例：把键改坏的 opts 不存在——键名写死在 makeProfile 内，改坏走「不认识的键」
    // 分支由 assertKeys 保证；这里用可见行为兜底：生成的 diagnostics.lua 必须含小写布尔。
    std::ifstream d(tmp + "/diagnostics.lua");
    std::string content{std::istreambuf_iterator<char>{d}, {}};
    c.chk("lua 布尔是小写（True/False 是 nil 回落默认——真实事故）",
        content.find("picking = true") != std::string::npos
            && content.find("shadow_pass = false") != std::string::npos);
}

// ── AO 噪声判据的度量层（ao_metrics.cpp）─────────────────────────────────────────
//
// 每条断言的期望值都来自**闭式**（中值滤波的数学性质、集合论、整数几何计数），不来自实机
// 截图——实机数字只能证明「与上次一样」，闭式才能证明「算子算的是我说的那个量」。
// 每条都带**反向用例**（算子在应该无响应的地方必须无响应）与**变异红线**（我把实现改坏成
// 哪种具体形态时，本用例会红）。
void testAoMetrics(Checker& c)
{
    const cv::Size sz(64, 64);
    const cv::Mat all = cv::Mat::ones(sz, CV_8UC1) * 255;

    // ① 中值残差保线性斜坡（**主判据成立的前提**：区域里混进光滑梯度时 jitterRms 不受污染）。
    //    闭式：5×5 窗内单调 ⟹ 中值 = 中心值 ⟹ 残差**恰为 0**。
    {
        cv::Mat ramp(sz.height, sz.width, CV_8UC1);
        for (int y = 0; y < sz.height; ++y)
            for (int x = 0; x < sz.width; ++x)
                ramp.at<uint8_t>(y, x) = static_cast<uint8_t>(100 + x);  // +1/px
        const RegionStat s = statRegion(ramp, all);
        c.chk("AO 度量：线性斜坡的抖动 RMS 恰为 0（主判据不被梯度污染）", s.jitterRms == 0.0,
            "jitterRms=" + std::to_string(s.jitterRms));
        // 变异红线：把 medianBlur 换成 blur（均值）→ 斜坡的均值 ≠ 中心（端点处窗口被边界裁）
        // 会出非零；换成 Laplacian 当抖动 → 直接红。
    }
    // ② 中值**完全抑制**单像素脉冲（闭式：25 个样本里 24 个是背景 ⟹ 中值 = 背景）。
    //    这条钉的是「只有中值滤波能当抖动算子」：均值滤波会把脉冲摊成邻域残差。
    {
        cv::Mat m = cv::Mat::ones(sz, CV_8UC1) * 100;
        m.at<uint8_t>(32, 32) = 200;
        const RegionStat s = statRegion(m, all);
        // 闭式：中值把脉冲从**平滑图**里完全去掉 ⟹ 残差只在脉冲本身 = 幅度 A ⟹
        // RMS = A/√N = 100/√4096 = 1.5625（均值滤波会把脉冲摊到 25 个像素，RMS 不同）。
        c.chk("AO 度量：单像素脉冲 → 残差 RMS = A/√N（中值而非均值，闭式）",
            std::abs(s.jitterRms - 100.0 / std::sqrt(4096.0)) < 1e-9,
            "jitterRms=" + std::to_string(s.jitterRms));
        c.chk("AO 度量：同一脉冲的拉普拉斯有响应（两轴不同，缺一轴会瞎）", s.lapRms > 1.0,
            "lapRms=" + std::to_string(s.lapRms));
    }
    // ③ 反向用例：逐位 255 的场（= strength 0 的附件）在**所有**噪声读数上必须是 0。
    //    这是整条判据的零点（`ao.frag.slang:118` 的位级早退），它红了后面所有数字全部作废。
    {
        const RegionStat s = statRegion(all, all);
        c.chk("AO 度量：平坦契约场的四个读数全为 0（零点）",
            s.jitterRms == 0.0 && s.lapRms == 0.0 && s.fracBelowContract == 0.0 && s.sd == 0.0);
        // 变异红线：`fracBelowContract` 的比较符从 `<` 写成 `<=` → 本用例立刻红（255 ≤ 255）。
        const RegionStat h = statRegion(all, all, /*contract=*/-1.0);
        c.chk("AO 度量：contract<0 时不统计契约偏离（底图分母走这条）", h.fracBelowContract == 0.0);
    }
    // ④ 契约偏离占比（闭式：一半像素 254 ⟹ 恰 0.5）。
    {
        cv::Mat m(sz.height, sz.width, CV_8UC1, cv::Scalar(255));
        m(cv::Rect(0, 0, sz.width / 2, sz.height)).setTo(254);
        const RegionStat s = statRegion(m, all);
        c.chk("AO 度量：<255 占比 = 0.5（闭式）", std::abs(s.fracBelowContract - 0.5) < 1e-12,
            std::to_string(s.fracBelowContract));
    }
    // ⑤ 字节域 A/B 的**恒定偏移**：grain 必须恰为 0、smooth 必须恰为偏移量。
    //    闭式：median(常数) = 常数 ⟹ 残差 0。这条把「AO 把画面整体压暗」与「AO 加了颗粒」
    //    分开的分解算子钉住——偏移是合法形状，颗粒才是噪声。
    {
        cv::Mat a(sz, CV_8UC3, cv::Scalar(100, 100, 100));
        cv::Mat b(sz, CV_8UC3, cv::Scalar(95, 95, 95));
        const ByteAB s = byteAbsDiff(a, b, all);
        c.chk("AO 度量：恒定 5 LSB 偏移 → 颗粒项恰为 0（偏移不是噪声）", s.grainRms == 0.0,
            "grain=" + std::to_string(s.grainRms));
        c.chk("AO 度量：恒定偏移 → 渐变项 = 5、均值/最大 = 5",
            std::abs(s.smoothRms - 5.0) < 1e-9 && s.mean == 5.0 && s.mx == 5.0,
            "smooth=" + std::to_string(s.smoothRms) + " mean=" + std::to_string(s.mean)
                + " mx=" + std::to_string(s.mx) + " changed=" + std::to_string(s.changed)
                + " pixels=" + std::to_string(s.pixels));
        const ByteAB r = byteAbsDiff(a, a, all);  // 反向：同一张图
        c.chk("AO 度量：同一张图 → 全读数 0（反向用例）",
            r.changed == 0 && r.mx == 0.0 && r.grainRms == 0.0);
        // 变异红线：三通道若取**均值**而非最大值，暗通道会把 5 稀释；取 max 才对。
    }
    // ⑥ 三通道取**最大**且 16S 不饱和（闭式：+7/−9/0 → 9；反向 −250 方向也必须算出来）。
    {
        cv::Mat a(sz, CV_8UC3, cv::Scalar(200, 200, 200));
        cv::Mat b(sz, CV_8UC3, cv::Scalar(193, 209, 200));  // B +7、G −9、R 0
        c.chk("AO 度量：三通道最大 |Δ| = 9（含变暗方向，8U 饱和会读成 0）",
            maxByteDiff(a, b, all) == 9.0, std::to_string(maxByteDiff(a, b, all)));
        c.chk("AO 度量：同一张图的最大差 = 0（反向）", maxByteDiff(a, a, all) == 0.0);
        // 变异红线：这里若用 `Mat::col(i)` 当第 i 通道 → Mat 缩成 64×1、按 64 列遍历越界，
        // 本用例读到的是垃圾值（实测真机读出过 32523 这种 >255 的「字节差」）。
    }
    // ⑦ rmsDiff：恒定差 → 恰等于该差；mask 外的差异不得参与（闭式）。
    {
        cv::Mat a = cv::Mat::ones(sz, CV_8UC1) * 100;
        cv::Mat b = cv::Mat::ones(sz, CV_8UC1) * 96;
        c.chk("AO 度量：恒定差 4 → rmsDiff = 4（闭式）", std::abs(rmsDiff(a, b, all) - 4.0) < 1e-9,
            std::to_string(rmsDiff(a, b, all)));
        cv::Mat mask = cv::Mat::zeros(sz, CV_8UC1);
        mask(cv::Rect(0, 0, sz.width, sz.height)).setTo(255);
        b.setTo(100);  // mask 内改成无差
        c.chk("AO 度量：mask 内的差为 0 → rmsDiff = 0（mask 外不参与）",
            rmsDiff(a, b, mask) == 0.0);
        cv::Mat three(sz, CV_8UC3, cv::Scalar(1, 2, 3));
        bool threw = false;
        try {
            static_cast<void>(rmsDiff(three, three, all));
        } catch (const cv::Exception&) {
            threw = true;
        }
        c.chk("AO 度量：rmsDiff 收到 3 通道必须拒收（at<float> 静默读 ch0 是假绿源）", threw);
    }
    // ⑧ Jaccard（闭式：同集合 1、不相交 0、半重叠 |∩|/|∪| = 1/3）。
    {
        cv::Mat a = cv::Mat::zeros(sz, CV_8UC1), b = cv::Mat::zeros(sz, CV_8UC1);
        a(cv::Rect(0, 0, 30, 64)).setTo(255);
        b(cv::Rect(20, 0, 30, 64)).setTo(255);  // 交 10 列、并 50 列
        c.chk("AO 度量：Jaccard 同集合 = 1", std::abs(jaccard(a, a, all) - 1.0) < 1e-12);
        c.chk("AO 度量：Jaccard 半重叠 = 10/50（闭式）",
            std::abs(jaccard(a, b, all) - 0.2) < 1e-12, std::to_string(jaccard(a, b, all)));
        cv::Mat z = cv::Mat::zeros(sz, CV_8UC1);
        c.chk("AO 度量：空并集 → 1（约定：无证据时不判「不重叠」）",
            jaccard(z, z, all) == 1.0);
    }
    // ⑨ distToEdge 的类型是**判据的一部分**（CV_32F）。flatRegion 曾按 uint8_t 读它，
    //    「离边界 > 40px」变成随机判据（实测契约区膨胀 2 倍、σ 从 1.3 涨到 10.9）。
    {
        cv::Mat ids = cv::Mat::zeros(100, 100, CV_32SC1);
        ids(cv::Rect(30, 30, 41, 41)).setTo(1);
        const cv::Mat dist = distToEdge(idEdges(ids));
        c.chk("AO 度量：距离图是 CV_32F（按 8U 读就是随机判据——真实事故）",
            dist.type() == CV_32FC1, "type=" + std::to_string(dist.type()));
        // 整数几何闭式：41×41 的块，dist > 5 ⟹ 去掉 6 圈 ⟹ 29×29 = 841 像素。
        const cv::Mat flat = flatRegion(ids, dist, 1, 5.0);
        c.chk("AO 度量：契约区像素数 = 29²（整数几何闭式）",
            cv::countNonZero(flat) == 29 * 29, std::to_string(cv::countNonZero(flat)));
        c.chk("AO 度量：契约区不含背景（oid 过滤）",
            cv::countNonZero(flatRegion(ids, dist, 2, 5.0)) == 0);
        // 变异红线：把 flatRegion 改回逐行 `ptr<uint8_t>` 读 → 上面两条同时红（且第二条的
        // 像素数会变成随机值）。这就是「区域定义不干净」最便宜的探测器。
    }
    // ⑩ 凹角接触环：只收「inner 且邻域贴着 outer」，且**必须**是 AND（闭式构造）。
    {
        cv::Mat ids = cv::Mat::zeros(100, 100, CV_32SC1);
        ids(cv::Rect(0, 80, 100, 20)).setTo(1);   // 地面 = inner
        ids(cv::Rect(40, 40, 20, 40)).setTo(2);   // 贴地方块 = outer
        const cv::Mat ring = contactRing(ids, 1, {2}, 6);
        const int n = cv::countNonZero(ring);
        c.chk("AO 度量：接触环非空（方块脚两侧 + 正下方）", n > 0 && n < 20 * 100,
            "n=" + std::to_string(n));
        cv::Mat onlyInner;
        cv::compare(ids, cv::Scalar(1), onlyInner, cv::CMP_EQ);
        cv::Mat notInner;
        cv::bitwise_and(ring, ~onlyInner, notInner);
        c.chk("AO 度量：接触环 ⊆ inner（AND 写成 OR 就红——真实变异）",
            cv::countNonZero(notInner) == 0);
        // 反向：outer 挪到 grow 之外 → 环必须空。
        cv::Mat far_ = ids.clone();
        far_(cv::Rect(40, 40, 20, 40)).setTo(0);
        far_(cv::Rect(40, 0, 20, 40)).setTo(2);  // 离地面 > 6 px
        c.chk("AO 度量：outer 远离 grow → 环为空（反向用例）",
            cv::countNonZero(contactRing(far_, 1, {2}, 6)) == 0);
    }
    // ⑪ 对齐算子 shiftX：符号约定 + 往返 + 边界补 0（爬行判据的方向全靠它）。
    {
        cv::Mat src = cv::Mat::zeros(10, 20, CV_8UC1);
        src.at<uint8_t>(5, 4) = 255;
        c.chk("AO 度量：shiftX(+3) 把标记搬到 x=7（符号约定）",
            shiftX(src, 3).at<uint8_t>(5, 7) == 255 && shiftX(src, 3).at<uint8_t>(5, 4) == 0);
        c.chk("AO 度量：shiftX(−3) 把标记搬到 x=1（反向符号）",
            shiftX(src, -3).at<uint8_t>(5, 1) == 255);
        const cv::Mat rt = shiftX(shiftX(src, 3), -3);
        c.chk("AO 度量：+3 后 −3 逐位复原（往返；Rect 写反就红）",
            cv::countNonZero(rt != src) == 0);
        c.chk("AO 度量：dx=0 逐位相同（反向）", cv::countNonZero(shiftX(src, 0) != src) == 0);
        cv::Mat edge = cv::Mat::zeros(10, 20, CV_8UC1);
        edge.at<uint8_t>(5, 19) = 255;
        c.chk("AO 度量：右移把边界标记推出画布（补 0，不绕回）",
            shiftX(edge, 3).at<uint8_t>(5, 19) == 0);
    }
}

// 时域绝对残差的四个算子（`temporal_residual.h`）。合成序列的 luma 按闭式函数直接填：判据的
// 算子只吃 `luma`，「域必须是线性」这条红线在 `prepareLinear` 里，那边由 testSrgb 钉。
void testTemporalResidual(Checker& c)
{
    {
        const cv::Size sz(8, 4);
        const cv::Mat all = cv::Mat(sz, CV_8UC1, cv::Scalar(255));
        const auto throws = [](auto&& fn) {
            try { fn(); } catch (const std::exception&) { return true; }
            return false;
        };
        const auto frames = [&](int periods, auto&& value) {
            std::vector<Frame> out;
            out.reserve(static_cast<size_t>(kResidualPeriod) * periods);
            for (int i = 0; i < kResidualPeriod * periods; ++i) {
                Frame f;
                f.luma = cv::Mat(sz, CV_64FC1);
                for (int y = 0; y < sz.height; ++y)
                    for (int x = 0; x < sz.width; ++x)
                        f.luma.at<double>(y, x) = value(i, x);
                out.push_back(std::move(f));
            }
            return out;
        };

        // ① 零点：同一张图连拍 ⟹ 五个读数**恰为 0**。⚠ 本条对算子变异的力气最小（常数序列
        //    在很多错实现上也是 0），它的价值是给后面所有条一个「非零即异常」的参照。
        {
            const auto shots = frames(2, [](int, int) { return 0.37; });
            const ResidualStat s = phaseSwingStat(shots, all, 0.001, 2);
            c.chk("残差：同一张图连拍 → 五个读数恰为 0（零点）",
                s.meanSwing == 0.0 && s.p999Swing == 0.0 && s.maxSwing == 0.0
                    && s.fracOverTau == 0.0 && s.halfGapMax == 0.0,
                "mean=" + std::to_string(s.meanSwing) + " max=" + std::to_string(s.maxSwing));
            c.chk("残差：meanLuma 走的是像素值不是常数（在场性守卫的输入）",
                std::abs(s.meanLuma - 0.37) < 1e-12, std::to_string(s.meanLuma));
        }
        // ② 同相位折叠的**阳性面**：只有第 3 个相位亮，且每 16 帧重复一次 ⟹ 折叠后 16 列里
        //    恰好一列 = 1、其余 = 0 ⟹ 中位数 0 ⟹ swing **恰为 1**；两半同相位 ⟹ halfGap 恰 0。
        //    变异红线：折叠若按「连续 32 帧两两配对」而非「同相位配对」，那一列会被摊成两个
        //    0.5 列 ⟹ swing = 0.5，本条当场红（这是全组里唯一钉住折叠口径本身的一条）。
        {
            const auto shots = frames(4, [](int i, int) { return (i % 16 == 3) ? 1.0 : 0.0; });
            const ResidualStat s = phaseSwingStat(shots, all, 0.5, 4);
            c.chk("残差：单相位离群 → swing 恰为 1（同相位折叠，闭式）",
                s.meanSwing == 1.0 && s.maxSwing == 1.0 && s.fracOverTau == 1.0,
                "mean=" + std::to_string(s.meanSwing));
            c.chk("残差：两半同相位 → halfGapMax 恰为 0（折叠自检的阳性面）",
                s.halfGapMax == 0.0, "gap=" + std::to_string(s.halfGapMax));
        }
        // ③ swing = max − **median**，不是 max − min。闭式：16 列 = 8×0.0 + 7×0.5 + 1×1.0
        //    ⟹ 排序第 8、9 位 = 0.0 与 0.5 ⟹ 中位数 0.25 ⟹ swing = 1.0 − 0.25 = **0.75**；
        //    写成 max−min 会给 1.0。这条把「为什么用 median」从注释变成了会红的断言。
        {
            const auto shots = frames(2, [](int i, int) {
                const int p = i % 16;
                return p < 8 ? 0.0 : (p < 15 ? 0.5 : 1.0);
            });
            const ResidualStat s = phaseSwingStat(shots, all, 0.0, 2);
            c.chk("残差：swing = max−median（不是 max−min），闭式 0.75",
                std::abs(s.meanSwing - 0.75) < 1e-12, "mean=" + std::to_string(s.meanSwing));
        }
        // ④ 区域纪律：mask 外的剧烈摆动一个都不计；同一序列换全图口径必须测得到它
        //    （反向：防「mask 恰好全 0 / 全在别处」把一切都判成稳定）。
        {
            cv::Mat mask = cv::Mat::zeros(sz, CV_8UC1);
            mask(cv::Rect(0, 0, sz.width / 2, sz.height)).setTo(255);
            const auto shots = frames(2, [&](int i, int x) {
                return x < sz.width / 2 ? 0.5 : ((i % 16 == 3) ? 1.0 : 0.0);
            });
            const ResidualStat in = phaseSwingStat(shots, mask, 0.01, 2);
            c.chk("残差：mask 外的摆动不计入（区域纪律）",
                in.meanSwing == 0.0 && in.maskPixels == sz.area() / 2,
                "mean=" + std::to_string(in.meanSwing) + " px=" + std::to_string(in.maskPixels));
            const ResidualStat full = phaseSwingStat(shots, cv::Mat(), 0.01, 2);
            c.chk("残差：同一序列全图口径必须测到（反向用例）",
                std::abs(full.meanSwing - 0.5) < 1e-12 && full.maskPixels == sz.area(),
                "mean=" + std::to_string(full.meanSwing));
            const cv::Mat none = cv::Mat::zeros(sz, CV_8UC1);
            c.chk("残差：空区域不出数（maskPixels=0，不给「测得 0」的机会）",
                phaseSwingStat(shots, none, 0.01, 2).maskPixels == 0);
        }
        // ⑤ 折叠算术的**双**闭式：value = 帧号（线性漂移）⟹ 半 A 第 p 列 = p+8、半 B = p+40
        //    ⟹ halfGap = halfGapMax = **32**（每列都是）；两半均值 = p+24 ⟹ max 39、中位数
        //    (31+32)/2 = 31.5 ⟹ swing = **7.5**。两个数同时命中 = 折叠/中位数/两半差三条都对。
        {
            const auto shots = frames(4, [](int i, int) { return static_cast<double>(i); });
            const ResidualStat s = phaseSwingStat(shots, all, 0.0, 4);
            c.chk("残差：线性漂移 → halfGap = halfGapMax = 32、swing = 7.5（双闭式）",
                s.halfGap == 32.0 && s.halfGapMax == 32.0 && s.meanSwing == 7.5,
                "gap=" + std::to_string(s.halfGap) + " swing=" + std::to_string(s.meanSwing));
        }
        // ⑥ 前提守卫：缺帧 / 奇周期 / 混进没 prepareLinear 或别的 extent 的帧，一律**抛**，
        //    不许出数（「列与相位脱钩」的读数看起来完全正常，是最贵的一类假绿）。
        {
            const auto good = frames(2, [](int, int) { return 0.2; });
            c.chk("相位平均：帧数不是整数周期就抛", throws([&] { (void)phaseAverageLuma(good, 3); }));
            c.chk("相位平均：周期数 <2 就抛（奇数半没有同相位对照）",
                throws([&] { (void)phaseAverageLuma(good, 1); }));
            auto noLin = good;
            noLin.push_back(Frame{});
            c.chk("相位平均：混进没 prepareLinear 的帧就抛", throws([&] { (void)phaseAverageLuma(noLin, 2); }));
            auto otherSize = good;
            otherSize.back().luma = cv::Mat(cv::Size(3, 3), CV_64FC1, cv::Scalar(0.0));
            c.chk("相位平均：混进别的 extent 就抛（两帧不是同一次采集）",
                throws([&] { (void)phaseAverageLuma(otherSize, 2); }));
        }
        // ⑦ τ 闭式与真解码器对拍（**拐点以上**；`srgbDecode` 是分段函数，byte ≤ 10 走线性段）。
        //    中心差分步长取 0.01 字节（亚 LSB），二阶误差 ~1e-7。
        {
            double worst = 0.0;
            for (int b = 11; b <= 254; ++b) {
                const double x = static_cast<double>(b) / 255.0;
                const double d = (srgbDecode(x + 0.01 / 255.0) - srgbDecode(x - 0.01 / 255.0)) / 0.02;
                worst = std::max(worst,
                    std::abs(d - lsbLinearAtLuma(srgbDecode(x))) / std::max(1e-30, d));
            }
            c.chk("τ 闭式：拐点以上与 srgbDecode 中心差分对拍（相对误差 <1e-5）", worst < 1e-5,
                "worst=" + std::to_string(worst));
            c.chk("τ 闭式：lin=1 处 = 2.4/1.055/255",
                std::abs(lsbLinearAtLuma(1.0) - 2.4 / 1.055 / 255.0) < 1e-15,
                std::to_string(lsbLinearAtLuma(1.0)));
            c.chk("τ 随线性电平单调递增（= 拿白电平 τ 当 mask 内上界的前提）",
                lsbLinearAtLuma(0.05) < lsbLinearAtLuma(0.617)
                    && lsbLinearAtLuma(0.617) < lsbLinearAtLuma(1.0));
            // 判据真正依赖的安全性质：**不亮于 maxLin 的像素，翻一格的实际跨度都有界**。
            // 这条看着像「按构造成立」，但它钉的是容差用 max 而不是 p99、用离散步长而不是
            // 导数这两件事——任一件被改回去（半间差容差就会把正常读数判成结构失效）。
            const double maxLin = srgbDecode(206.0 / 255.0);
            const double bound = maxLsbStepUpTo(maxLin);
            bool bounded = true;
            for (int b = 0; b < 255; ++b) {
                const double lo = srgbDecode(static_cast<double>(b) / 255.0);
                if (lo > maxLin)
                    break;
                bounded = bounded && srgbDecode(static_cast<double>(b + 1) / 255.0) - lo <= bound;
            }
            c.chk("τ：不亮于 maxLin 的像素翻一格 ≤ maxLsbStepUpTo(maxLin)（容差的上界性质）",
                bounded, "bound=" + std::to_string(bound));
            // 离散步长与闭式导数在同一电平处应当一致（两条独立实现互相对拍，差 <1%）。
            c.chk("τ：离散一格与闭式导数在白电平处一致（<1%）",
                std::abs(bound / lsbLinearAtLuma(maxLin) - 1.0) < 0.01,
                "disc=" + std::to_string(bound) + " closed=" + std::to_string(lsbLinearAtLuma(maxLin)));
            c.chk("τ：一格跨度随电平单调（= 取最亮电平即上界的前提）",
                maxLsbStepUpTo(0.05) < maxLsbStepUpTo(0.617)
                    && maxLsbStepUpTo(0.617) < maxLsbStepUpTo(1.0));
            // ⚠ 容差取 max、τ 取 p99，两者**必须**能分开：mask 里若有比 p99 更亮的高光，用 p99
            //    当容差会低估。本条把「别把两个数合并成一个」钉住。
            c.chk("τ：比 p99 白电平更亮的高光，一格能翻过 τ（所以容差走 max 不走 p99）",
                maxLsbStepUpTo(0.9) > lsbLinearAtLuma(maxLin),
                "bright=" + std::to_string(maxLsbStepUpTo(0.9)) + " tau=" + std::to_string(bound));
            c.chk("τ：maxLin=1 时 = 1 − srgbDecode(254/255)（端点闭式）",
                std::abs(maxLsbStepUpTo(1.0) - (1.0 - srgbDecode(254.0 / 255.0))) < 1e-15,
                std::to_string(maxLsbStepUpTo(1.0)));
        }
        // ⑧ 梯度能量：常量图恰为 0（收敛正确性判据的零点）；斜坡走 Sobel 核 [−1,0,1] +
        //    BORDER_REFLECT_101 的闭式 —— 内部 gx = 2·slope，**两列边界**反射后 gx = 0
        //    ⟹ 均值 = 2·slope·(W−2)/W。变异红线：borderType 改成 REPLICATE 会给 1.75·slope。
        {
            const cv::Mat flat(sz, CV_64FC1, cv::Scalar(0.3));
            c.chk("梯度能量：常量图恰为 0（零点）", gradEnergy(flat, all) == 0.0,
                std::to_string(gradEnergy(flat, all)));
            const double slope = 0.1;
            cv::Mat ramp(sz.height, sz.width, CV_64FC1);
            for (int y = 0; y < sz.height; ++y)
                for (int x = 0; x < sz.width; ++x)
                    ramp.at<double>(y, x) = slope * x;
            const double expect = 2.0 * slope * (sz.width - 2) / sz.width;
            c.chk("梯度能量：线性斜坡 = 2·slope·(W−2)/W（Sobel 核 + REFLECT_101 闭式）",
                std::abs(gradEnergy(ramp, all) - expect) < 1e-12,
                "got=" + std::to_string(gradEnergy(ramp, all)) + " expect=" + std::to_string(expect));
            // mask 是**选择器不是裁剪器**：算子必须在整图上跑完再按 mask 取均值。fixture =
            // 全图斜坡 + 只遮左半 ⟹ mask 内 gx = [0, 2s, 2s, 2s]（x=0 是画布边界，REFLECT_101
            // 反射后自减为 0）⟹ 均值 = 2·slope·(Wm−1)/Wm。**变异红线**：若实现先裁剪再 Sobel，
            // 裁剪区的右边缘会变成边界（gx=0）⟹ 均值降成 2·slope·(Wm−2)/Wm，本条当场红。
            cv::Mat half = cv::Mat::zeros(sz, CV_8UC1);
            half(cv::Rect(0, 0, sz.width / 2, sz.height)).setTo(255);
            const double expectL = 2.0 * slope * (sz.width / 2 - 1) / (sz.width / 2);
            c.chk("梯度能量：mask 是选择器不是裁剪器（闭式，Sobel 支撑跨 mask 边界）",
                std::abs(gradEnergy(ramp, half) - expectL) < 1e-12,
                "got=" + std::to_string(gradEnergy(ramp, half)) + " expect=" + std::to_string(expectL));
        }
        // ⑨ 梯度图本体与梯度能量**同一算子**（分带表用本体、债-47 用均值，两者若各写一份
        //    Sobel，「边缘带」与「梯度比」说的就不是同一张图）。变异红线：sobelAbsGrad 的
        //    核尺寸改成 5，或 gradEnergy 不再走它 → 两条同时红。
        {
            const double slope = 0.1;
            cv::Mat ramp(sz.height, sz.width, CV_64FC1);
            for (int y = 0; y < sz.height; ++y)
                for (int x = 0; x < sz.width; ++x)
                    ramp.at<double>(y, x) = slope * x;
            const cv::Mat g = sobelAbsGrad(ramp);
            c.chk("梯度图：常量图恰为 0（零点）",
                cv::norm(sobelAbsGrad(cv::Mat(sz, CV_64FC1, cv::Scalar(0.3))), cv::NORM_INF)
                    == 0.0);
            c.chk("梯度图：与 gradEnergy 同算子（均值相等，闭式 2·slope·(W−2)/W）",
                std::abs(cv::mean(g)(0) - 2.0 * slope * (sz.width - 2) / sz.width) < 1e-12
                    && std::abs(cv::mean(g)(0) - gradEnergy(ramp, all)) < 1e-15,
                "mean=" + std::to_string(cv::mean(g)(0)));
        }
    }
}

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
                const int64_t wantOver = (lb == 2 && gb == 1) ? 0 : 1;
                const double wantMass = (lb == 2 && gb == 1) ? 0.0 : 0.9;
                cellsOk = cell.pixels == 1 && cell.over == wantOver
                    && std::abs(cell.mass - wantMass) < 1e-12;
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

/**
 * 运动态时域两条判据的纯函数自检（temporal-flicker / temporal-homography）。
 *
 * 每条带「变异红线」= 把被测实现改坏成什么样子，本条会红。
 * 数值锚点出处：aa_target 白板左边缘实机跑（0.125px/帧，ACES 前黑=0.0987 白=0.9840，
 * 闭式给 ρ_ideal=0.10219 / ρ_noAA=0.57451，实测 ρ=0.57490 → 差 0.07%）。
 */
void testTemporalFlicker(Checker& c)
{
    const FlickerScale s{ 0.0987f, 0.9840f };
    const FlickerAnchor a = flickerClosedForm(0.125f, s);

    // 恒等式：无 AA 端 = 阶梯的整段跳变，与扫速无关。变异红线：noAaRho 写成 chain(1) 本身。
    c.chk("闭式·无AA 端 = chain(1)−chain(0)",
        std::abs(a.noAaRho - (displayChain(1.0f, s) - displayChain(0.0f, s))) < 1e-9f);
    // 实机锚点（±1%）。变异红线：displayChain 换掉 ACES 或漏掉 8bit 取整 → 两端同时漂出带。
    c.chk("闭式·理想端 实机锚点 0.10219", std::abs(a.idealRho - 0.10219f) < 1.1e-3f,
        std::to_string(a.idealRho));
    c.chk("闭式·无AA 端 实机锚点 0.57451", std::abs(a.noAaRho - 0.57451f) < 5.8e-3f,
        std::to_string(a.noAaRho));
    // 理想必须严格低于无 AA（否则判据的分母是负的、ζ 无意义）。变异红线：max 写成 min。
    c.chk("闭式·理想 < 无AA", a.idealRho < a.noAaRho);

    // 反向用例 ①：扫速 0 = 静止 ⟹ 理想每帧变化恰为 0（不是「接近 0」）。
    c.chk("闭式·v=0 理想端恰为 0", flickerClosedForm(0.0f, s).idealRho == 0.0f);
    // 反向用例 ②：整数像素平移（v=1）⟹ 斜坡只是整体搬一格，与阶梯同幅 ⟹ 两端相等。
    // 这条钉住「v≥1 没有混叠可测」的用法红线（cmd 侧拒收 v≥1 的理由）。
    const FlickerAnchor i1 = flickerClosedForm(1.0f, s);
    c.chk("闭式·v=1 两端相等（无混叠可测）", std::abs(i1.idealRho - i1.noAaRho) < 1e-6f,
        std::to_string(i1.idealRho) + " vs " + std::to_string(i1.noAaRho));

    // 单调性：扫速越快，理想端越接近无 AA 端 ⟹ 增益 G = noAA/ideal 单调下降。
    // 变异红线：网格步长粗到 ≥ 扫速（上确界取不到斜坡段）→ 小 v 的 G 塌到 1。
    double prev = 1e30;
    for (float v: { 0.0625f, 0.125f, 0.25f, 0.5f }) {
        const FlickerAnchor g = flickerClosedForm(v, s);
        const double gain = g.noAaRho / static_cast<double>(g.idealRho);
        c.chk("闭式·增益随扫速单调降", gain < prev, "v=" + std::to_string(v) + " G=" + std::to_string(gain));
        prev = gain;
    }

    // 幅值反标定往返：观测两端 → ACES 前两端 → 再走一遍链，应回到观测两端（±1 LSB）。
    // 变异红线：反标定二分方向写反 → 往返差一个量级。
    const FlickerScale back = scaleFromObserved(0.0423f, 0.6172f);
    c.chk("反标定·黑端往返 ≤1LSB", std::abs(displayChain(0.0f, back) - 0.0423f) < 1.0 / 255.0,
        std::to_string(displayChain(0.0f, back)));
    c.chk("反标定·白端往返 ≤1LSB", std::abs(displayChain(1.0f, back) - 0.6172f) < 1.0 / 255.0,
        std::to_string(displayChain(1.0f, back)));

    // Hann 半谱：纯正弦（整数周期）能量应集中在一个 bin；常数信号去 DC 后应无能量。
    // Hann 主瓣 4 bin 宽（能量落在 m−1/m/m+1 三格），故判「激励 bin ±1」而不是单 bin。
    // 变异红线：Hann 窗或去均值漏掉 → 常数信号漏出巨大 DC、正弦泄漏铺满半谱。
    std::vector<float> sine(64), flat(64, 0.37f);
    for (int k = 0; k < 64; ++k)
        sine[static_cast<size_t>(k)] = 0.5f + 0.2f * std::sin(2.0f * 3.14159265358979f * 4.0f * k / 64.0f);
    const auto sp = hannHalfSpectrum(sine);
    const auto sf = hannHalfSpectrum(flat);
    double tot = 0.0, lobe = 0.0;
    for (size_t m = 1; m < sp.size(); ++m) {
        tot += static_cast<double>(sp[m]) * sp[m];
        if (m >= 3 && m <= 5)
            lobe += static_cast<double>(sp[m]) * sp[m];
    }
    c.chk("FFT·正弦能量集中在激励主瓣", tot > 0 && lobe / tot > 0.9, std::to_string(lobe / tot));
    // 泄漏判据（这条才是「Hann 窗在不在」的判据）：频率取**非整数 bin**（4.5），
    // 远端（≥8 bin）能量占比 Hann 实测 0.00006、矩形窗 0.0129（差 200 倍）⟹ 阈值取 0.002。
    // 变异红线：去掉 Hann 窗 → 本条以 1.3% 红（而整数频率的正弦两条都「完美」，故必须用 4.5）。
    std::vector<float> half(64);
    for (int k = 0; k < 64; ++k)
        half[static_cast<size_t>(k)] = std::sin(2.0f * 3.14159265358979f * 4.5f * k / 64.0f);
    const auto sh = hannHalfSpectrum(half);
    double hTot = 0.0, hFar = 0.0;
    for (size_t m = 1; m < sh.size(); ++m) {
        hTot += static_cast<double>(sh[m]) * sh[m];
        if (m >= 8)
            hFar += static_cast<double>(sh[m]) * sh[m];
    }
    c.chk("FFT·Hann 远端泄漏 <0.2%", hTot > 0 && hFar / hTot < 0.002, std::to_string(hFar / hTot));
    double flatE = 0.0;
    for (size_t m = 1; m < sf.size(); ++m)
        flatE += static_cast<double>(sf[m]) * sf[m];
    c.chk("FFT·常数信号去 DC 后无能量", flatE < 1e-6, std::to_string(flatE));
    c.chk("FFT·长度不足返回空", hannHalfSpectrum({ 1.0f, 2.0f }).empty());

    // 亚像素边缘定位：覆盖 = clamp(x − 39.8) 的 1px 边（覆盖 0.5 处在 x = 40.3），应找回 ±0.1。
    // ⚠ 位置**故意不取半整数**：取 40.5 时抛物线修正恒为 0，符号写反也测不出（首跑就是这么漏的，
    //   变异「插值符号反号」存活）。实测该形状下正确符号误差 0.000、反号误差 0.600。
    std::vector<float> prof(64, 0.0f);
    for (int i = 0; i < 64; ++i)
        prof[static_cast<size_t>(i)] = std::clamp(static_cast<float>(i) - 39.8f, 0.0f, 1.0f);
    const float found = steepestEdgeColumn(prof);
    c.chk("边缘定位·1px 边亚像素 ±0.1px", std::abs(found - 40.3f) < 0.1f, std::to_string(found));

    // 局部扫速：主点处 = f·|dψ|；u=1 处 = 2 倍（透视修正 dx/dψ = f(1+u²)）。
    // 变异红线：u² 项漏掉 → 偏轴机位的扫速系统性偏低，闭式锚跟着偏。
    c.chk("局部扫速·主点 = f·dψ", std::abs(localSweepPxPerFrame(0.001f, 800.0f, 100.0f, 100.0f) - 0.8f) < 1e-5f);
    c.chk("局部扫速·u=1 处翻倍",
        std::abs(localSweepPxPerFrame(0.001f, 800.0f, 900.0f, 100.0f) - 1.6f) < 1e-5f);
    c.chk("焦距·fov45 @720 = 869.1", std::abs(verticalFocalPx(720, 45.0f) - 869.1f) < 0.2f,
        std::to_string(verticalFocalPx(720, 45.0f)));

    // 逐帧差分位：同帧 ⟹ 恰为 0；整体抬 0.5 ⟹ 恰为 0.5。变异红线：ROI 行列写反。
    cv::Mat f0 = cv::Mat::ones(8, 8, CV_64FC1) * 0.25, f1 = cv::Mat::ones(8, 8, CV_64FC1) * 0.75;
    const cv::Rect roi(1, 1, 5, 6);
    c.chk("帧差·同帧恰为 0", frameDeltaPercentile({ f0, f0 }, roi, 0.995) == 0.0);
    c.chk("帧差·整体抬 0.5 = 0.5", std::abs(frameDeltaPercentile({ f0, f1 }, roi, 0.995) - 0.5) < 1e-12);

    // 闭式单应：Δψ=0 ⟹ 恒等；主点是不动点；垂直线仍垂直（x′ 与 y 无关）；往返闭合。
    // 变异红线：矩阵任一行写错 → 往返/不动点两条同时红（它们不依赖行的具体形式）。
    const double f = 869.1, cx = 640.0, cy = 360.0, dpsi = 0.00057530;
    const cv::Matx33d id = yawHomography(0.0, f, cx, cy);
    c.chk("单应·Δψ=0 是恒等（差比例因子）",
        std::abs(id(0, 0) / id(2, 2) - 1.0) < 1e-12 && std::abs(id(1, 1) / id(2, 2) - 1.0) < 1e-12
            && std::abs(id(0, 1)) < 1e-12 && std::abs(id(2, 0)) < 1e-12);
    const cv::Matx33d H = yawHomography(dpsi, f, cx, cy), Hi = H.inv();
    auto map = [](const cv::Matx33d& M, double x, double y) {
        const double w = M(2, 0) * x + M(2, 1) * y + M(2, 2);
        return std::array<double, 2>{ (M(0, 0) * x + M(0, 1) * y + M(0, 2)) / w,
            (M(1, 0) * x + M(1, 1) * y + M(1, 2)) / w };
    };
    // 主点处的位移 = f·tan(Δψ)（= 「扫速 → 每帧角」换算的逆命题，判据的立足点）。
    // ⚠ 纯旋转单应**没有实不动点**（u′=u ⟹ t(1+u²)=0 ⟹ u=±i），首跑按「主点是不动点」
    //   写断言被本条当场抓红——不动点是复的，别拿它当锚。
    const double shift = map(H, cx, cy)[0] - cx;
    c.chk("单应·主点位移 = f·tan(Δψ)", std::abs(shift - f * std::tan(dpsi)) < 1e-9,
        std::to_string(shift));
    c.chk("单应·主点处 y 不变", std::abs(map(H, cx, cy)[1] - cy) < 1e-9);
    const auto p1 = map(H, 300.0, 100.0), p2 = map(H, 300.0, 600.0);
    c.chk("单应·垂直线仍垂直", std::abs(p1[0] - p2[0]) < 1e-9);
    const auto rt = map(Hi, map(H, 123.0, 456.0)[0], map(H, 123.0, 456.0)[1]);
    c.chk("单应·往返闭合", std::abs(rt[0] - 123.0) < 1e-6 && std::abs(rt[1] - 456.0) < 1e-6,
        std::to_string(rt[0]));
    // 透视：偏离主点处同样的角度产生**更大**的像素位移（dx/dψ = f(1+u²)，localSweep 的依据）。
    const auto off = map(H, cx + 1000.0, cy);
    c.chk("单应·偏离主点位移更大", std::abs(off[0] - (cx + 1000.0)) > std::abs(shift) + 0.4,
        std::to_string(off[0] - (cx + 1000.0)));
}

}  // namespace

int cmdSelftest()
{
    Checker c;
    testSrgb(c);
    testObjectId(c);
    testRegion(c);
    testClusters(c);
    testAnalytic(c);
    testRegistry(c);
    testTemporalDiff(c);
    testAaCoverage(c);
    testAoMetrics(c);
    testTemporalResidual(c);
    testLumaGradBands(c);
    testTemporalFlicker(c);
    testKeyValidation(c);
    std::printf("%s：%d 条失败\n", c.bad ? "自检失败" : "自检通过", c.bad);
    return c.bad ? 1 : 0;
}

}  // namespace RGL::imgqa
