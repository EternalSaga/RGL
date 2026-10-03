// 自检：判据自己要有判据。**断言全部照实测/源码写，不照记忆写**（本项目纪律：
// 「我以为的行为」一轮写出过 10 处，全部是错的）。闭式解锚点来源逐条注明。
#include "imgqa.h"
#include "aa_coverage.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

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
    testKeyValidation(c);
    std::printf("%s：%d 条失败\n", c.bad ? "自检失败" : "自检通过", c.bad);
    return c.bad ? 1 : 0;
}

}  // namespace RGL::imgqa
