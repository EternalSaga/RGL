// 自检：判据自己要有判据。**断言全部照实测/源码写，不照记忆写**（本项目纪律：
// 「我以为的行为」一轮写出过 10 处，全部是错的）。闭式解锚点来源逐条注明。
#include "imgqa.h"
#include "aa_coverage.h"
#include "cli.h"
#include "ao_metrics.h"
#include "temporal_flicker.h"
#include "temporal_homography.h"
#include "temporal_residual.h"
#include "temporal_yaw_remap.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <string>

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
    // **普适不变量**（债-08 降级后这条机制的牙）：**每条**判据都必须有 ① 写得出理由的容差
    // （>20 字符，不是"0.5%"式敷衍）② 至少一条 guard 出处（真实 bug 或真实变异）。
    // 原来这条挂在「已晋升判据」的分支下，而那个标志无一处置 true ⟹ 循环体恒不执行 = 摆设。
    // 前提守卫：注册表若为空，下面两条对空循环**恒真** = 假绿，所以先钉住它非空。
    const auto& all = registeredJudgments();
    c.chk("判据注册表非空（否则下面两条恒真是假绿）", !all.empty(), std::to_string(all.size()));
    for (const auto& j : all) {
        c.chk("每条判据必须有 guard 出处", !j.guards.empty(), j.name);
        c.chk("每条判据的容差理由必须 >20 字符", std::strlen(j.whyTol) > 20, j.name);
    }
}

/// stillOpen 里的「具体名字」判据：至少一对反引号、其间不是空白。空话（"其他方法仍然可用"）
/// 写不出名字 ⟹ 这一条就是「不许写空话」的机器判据（作用域写宽付过两天学费，见 judge.cpp）。
bool hasBacktickedName(const char* s)
{
    if (s == nullptr)
        return false;
    const char* open = std::strchr(s, '`');
    if (open == nullptr)
        return false;
    const char* close = std::strchr(open + 1, '`');
    if (close == nullptr)
        return false;
    for (const char* p = open + 1; p != close; ++p)
        if (*p != ' ' && *p != '\t')
            return true;
    return false;
}

/// 族拆分的护栏：拆表最容易出的三种事故（掉条目 / 复制粘贴出重名 / 全塞一族）。
void testJudgmentFamilies(Checker& c)
{
    const auto& all = registeredJudgments();
    // 51 是**当前**条数，写死它才有牙：族拆分/合并时漏接一个子表，这里立刻红。
    // 43 → 46 = 批 13.14a `temporal-tail --motion camera` 新增三条（smear-alpha-monotone /
    // static-collapse / gate-breakpoint）；46 → 47 = 同批把 **φ 符号标定**从诊断升格成判据
    // （`temporal-tail-phi-sign`）——理由见其契约：自搭金标准与实现共享朝后的相机基，
    // 单测对拍 2.3e-13px 全绿仍漏掉 yaw 反号，那一半只有实机数据守得住。
    // 47 → 51 = 批 13.15 `temporal-tail --motion step`（债-63 真鬼影量具）四条：零锚 /
    // 历史存活（防 `cameraJumps` 跳切假绿）/ 闭式锚 / 主判据。
    c.chk("判据总数 == 51（族拆分最容易掉条目）", all.size() == 51, std::to_string(all.size()));
    size_t n[4] = {0, 0, 0, 0};
    for (const auto& j : all) {
        switch (j.family) {
            case JudgmentFamily::Foundation: ++n[0]; break;
            case JudgmentFamily::TemporalStatic: ++n[1]; break;
            case JudgmentFamily::TemporalMotion: ++n[2]; break;
            case JudgmentFamily::Attachment: ++n[3]; break;
        }
    }
    const std::string detail
        = "地基" + std::to_string(n[0]) + " 静止" + std::to_string(n[1]) + " 运动"
        + std::to_string(n[2]) + " 附件" + std::to_string(n[3]);
    // 编译器管"漏填族"（const char* 转不进 enum），这条管"族字段白加"——全塞一族。
    c.chk("四个族都非空（全塞一族 = 族字段是装饰）",
        n[0] > 0 && n[1] > 0 && n[2] > 0 && n[3] > 0, detail);
    std::set<std::string> names;
    for (const auto& j : all)
        names.insert(j.name);
    c.chk("判据名全局唯一（防拆分时复制粘贴出重复条目）", names.size() == all.size(),
        std::to_string(names.size()) + " 个唯一名 / " + std::to_string(all.size()) + " 条");
}

void testAntiPatterns(Checker& c)
{
    // 反模式登记必须是**带作用域的结构化登记**，不是一段散文。三格各有机器判据：
    //   ① why 非空（被否的是什么）② stillOpen 非空且 >20 字符（未被否的部分不许敷衍）
    //   ③ stillOpen 必须点名具体命令/判据（反引号包裹）——"其他方法仍可用"过不了这条。
    // ⚠ ②是 std::strlen（**字节**）判据：8 个汉字（24 字节）的空话过得了 ②，只有 ③ 拦得住
    //   ——变异实测过（stillOpen = "其他方法仍然可用" ⟹ 仅 ③ 红）。两条不是冗余。
    // 前提守卫同 testRegistry：登记表若为空，下面三条对空循环**恒真** = 假绿。
    const auto& anti = antiPatterns();
    c.chk("反模式登记表非空（否则下面三条恒真是假绿）", !anti.empty(), std::to_string(anti.size()));
    for (const auto& [name, a] : anti) {
        c.chk("每条反模式必须有 why（被否的是什么）", a.why != nullptr && a.why[0] != '\0',
            name.c_str());
        c.chk("每条反模式必须写 stillOpen 且 >20 字符",
            a.stillOpen != nullptr && std::strlen(a.stillOpen) > 20, name.c_str());
        c.chk("每条反模式的 stillOpen 必须点名具体命令/判据（反引号名）",
            hasBacktickedName(a.stillOpen), name.c_str());
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
            // 计数口径的反向锚点：max = 0 时**超 τ 的像素数必须也是 0**（若计数写成"≥"或按相位
            // 而非按像素累加，这条会给出 16 或 16×像素数，而不是 0）。
            c.chk("残差：halfGapOverPx 按**像素**计数（同相位 → 恰为 0，反向锚点）",
                s.halfGapOverPx == 0, "overPx=" + std::to_string(s.halfGapOverPx));
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
            // 计数口径的阳性锚点：τ=0 且每像素 gap=32 ⟹ **每个像素都超**（=maskPixels）。
            // 若计数误按「像素×相位」累加，这里会给出 16×，当场红。
            c.chk("残差：halfGapOverPx = maskPixels（τ=0 且逐像素都超，阳性锚点）",
                s.halfGapOverPx == s.maskPixels,
                "overPx=" + std::to_string(s.halfGapOverPx) + " mask="
                    + std::to_string(s.maskPixels));
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

/// 断言"这一句必须抛 UsageError"。校验类代码不抛就是摆设——阳性锚点。
/// 抛**别的**类型也算失败：main 靠 UsageError 这个类型把 rc 分成 2（你敲错了）
/// 与 3（跑挂了），混型就等于把用法错误伪装成运行故障。
template <typename Fn>
bool throwsUsage(Fn&& fn)
{
    try {
        fn();
    } catch (const UsageError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

/// 断言"这一句不许抛"（反向用例：合法参数被挡下 = 判据反过来咬人）。
template <typename Fn>
bool noThrow(Fn&& fn)
{
    try {
        fn();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

/// Number 类 flag 的默认值串必须能被 stod 解析。不查这条的后果很具体：dispatch 里
/// `P.number("--burst")` 当场抛，而它抛的是 std::invalid_argument（不是 UsageError）
/// ⟹ rc=3「跑挂了」，用户看不出真正的问题是注册表里写错了默认值。
bool numberDefaultParses(const FlagSpec& f)
{
    try {
        std::stod(f.def);
    } catch (...) {
        return false;
    }
    return true;
}

/// CLI 注册表自检·**结构面**（批次 B）：这张表是默认值的唯一出处，它自己得先自洽。
void testCliRegistryShape(Checker& c)
{
    const auto& table = subcommands();
    c.chk("CLI 注册表非空（20+ 子命令）", table.size() >= 20, std::to_string(table.size()));

    std::set<std::string> names;
    for (const auto& s : table) {
        c.chk("子命令名唯一", names.insert(s.name).second, s.name);
        std::set<std::string> flags;
        for (const auto& f : s.flags) {
            c.chk("同命令内 flag 名唯一", flags.insert(f.name).second,
                std::string(s.name) + " " + f.name);
            c.chk("flag 名以 -- 开头", std::string(f.name).rfind("--", 0) == 0, f.name);
            if (f.kind == FlagKind::Bool) {
                c.chk("Bool flag 不许有默认值串", f.def == nullptr,
                    std::string(s.name) + " " + f.name);
                continue;
            }
            c.chk("非 Bool flag 必须有默认值串", f.def != nullptr, f.name);
            if (f.kind == FlagKind::Number)
                c.chk("Number 类默认值能被 stod 解析", numberDefaultParses(f),
                    std::string(s.name) + " " + f.name + "='" + (f.def ? f.def : "") + "'");
        }
    }

    // 反向用例：不存在的命令返回空，而不是某个默认项。
    c.chk("findSubcommand(不存在的命令) → 空", findSubcommand("不存在的命令") == nullptr);
    c.chk("findSubcommand(空串) → 空", findSubcommand("") == nullptr);
}

/// CLI 注册表自检·**行为面**：默认值只有一个出处 + 未知参数校验不是摆设。不碰引擎。
void testCliArgsBehavior(Checker& c)
{
    const auto* tail = findSubcommand("temporal-tail");
    c.chk("注册表里有 temporal-tail", tail != nullptr);
    if (tail == nullptr)
        return;
    const std::vector<std::string> noargs;
    const Args empty(noargs, *tail);

    // ── 阳性锚点：读未声明的 flag 必须抛（不抛 = 默认值没有真相源）──
    c.chk("get 未声明 flag → 抛", throwsUsage([&] { (void)empty.get("--strenght"); }));
    c.chk("has 未声明 flag → 抛", throwsUsage([&] { (void)empty.has("--strength"); }));
    c.chk("number 未声明 flag → 抛", throwsUsage([&] { (void)empty.number("--nope"); }));

    // ── 已声明 flag 的默认值 = 注册表里那一串（含被证伪后修正的那几个）──
    c.chk("默认值来自注册表：--burst = 96", empty.integer("--burst") == 96,
        empty.get("--burst"));
    c.chk("默认值来自注册表：--max-lag = 16", empty.integer("--max-lag") == 16,
        empty.get("--max-lag"));
    const Args homo(noargs, *findSubcommand("temporal-homography"));
    c.chk("--env 默认 -1（负数 = 门关；帮助文本曾写 1.0 = 误导判据口径）",
        homo.number("--env") == -1.0, homo.get("--env"));
    c.chk("--ao 默认 -1（同上，帮助文本曾写 0.5）", homo.number("--ao") == -1.0,
        homo.get("--ao"));
    const Args resid(noargs, *findSubcommand("temporal-residual"));
    c.chk("--long-feedback 默认 0.01（长窗档 = 仓库拍板值；批 13.14b 前这一档藏在 static boost 地板里）",
        resid.number("--long-feedback") == 0.01, resid.get("--long-feedback"));

    // ── 显式值覆盖默认值；没给的保持默认 ──
    const Args given({"--burst", "8"}, *tail);
    c.chk("显式值覆盖注册表默认", given.integer("--burst") == 8);
    c.chk("没给的 flag 仍是注册表默认", given.integer("--warm") == 64);
    c.chk("重复 flag 取首次出现（与改造前 P.get 逐字一致）",
        Args({"--burst", "7", "--burst", "9"}, *tail).integer("--burst") == 7);

    // ── rejectUnknown：两个方向都要测，否则等于没测 ──
    const Args bad({"--strenght", "0.5"}, *tail);
    c.chk("rejectUnknown 挡下拼错的 flag", throwsUsage([&] { bad.rejectUnknown(); }));
    c.chk("rejectUnknown 放行合法 flag", noThrow([&] { given.rejectUnknown(); }));
    const Args miss({"--glb"}, *findSubcommand("attribution"));
    c.chk("flag 后面漏值也要挡（否则静默用默认值）", throwsUsage([&] { miss.rejectUnknown(); }));
    const Args stray({"多出来的"}, *findSubcommand("attribution"));
    c.chk("不收位置参数的命令拒位置参数", throwsUsage([&] { stray.rejectUnknown(); }));
    const Args pos({"ssim"}, *findSubcommand("refuse"));
    c.chk("refuse 收位置参数", noThrow([&] { pos.rejectUnknown(); }));
    c.chk("位置参数可取回", pos.positional().size() == 1 && pos.positional()[0] == "ssim");

    // ── args 回显：格式与「默认值快照」同构（验收 3 的机械入口）──
    const std::string echo = echoFor(*tail, empty);
    c.chk("回显首行是命令名", echo.rfind("temporal-tail\n", 0) == 0, echo.substr(0, 24));
    c.chk("回显逐值成行 --burst = '96'", echo.find("   --burst = '96'\n") != std::string::npos,
        echo);
    const Args on({"--bistro"}, *findSubcommand("temporal-residual"));
    c.chk("Bool 未点亮不写值", echo.find("(bool) = ") == std::string::npos, echo);
    c.chk("Bool 点亮写 = true",
        echoFor(*findSubcommand("temporal-residual"), on)
                .find("--bistro(bool) = true")
            != std::string::npos);
    const auto* probe = findSubcommand("probe");
    const std::string pe = echoFor(*probe, Args(noargs, *probe));
    c.chk("probe 的 --light 既出值行也出 (bool) 行",
        pe.find("   --light = '") != std::string::npos
            && pe.find("   --light(bool)") != std::string::npos,
        pe);

    // ── args 子命令本体：未知参数不许逃到 main 外面（曾 uncaught → abort rc=134）──
    c.chk("cmdArgs 未知 flag → UsageError",
        throwsUsage([&] { cmdArgs({"temporal-tail", "--strenght", "0.5"}); }));
    c.chk("cmdArgs 未知子命令 → UsageError", throwsUsage([&] { cmdArgs({"不存在的命令"}); }));
    c.chk("cmdArgs 不带参数 → UsageError", throwsUsage([&] { cmdArgs({}); }));

    // ── -h 与表同源：每个子命令都必须出现在 -h 里（漏一条 = 帮助与代码又分家）──
    const std::string usage = usageText();
    for (const auto& s : subcommands())
        c.chk("-h 含子命令行",
            usage.find(std::string("  ") + s.name + " ") != std::string::npos, s.name);
    // 反向：-h 里不许再出现被证伪的旧默认值（它们只该留在 git history 里）
    c.chk("-h 不再写 [--env 1.0]", usage.find("[--env 1.0]") == std::string::npos);
    c.chk("-h 不再写 [--burst 64]", usage.find("[--burst 64]") == std::string::npos);
    c.chk("-h 不再写 [--max-lag 24]", usage.find("[--max-lag 24]") == std::string::npos);
    c.chk("-h 不再写 [--static-boost 0.02]", usage.find("[--static-boost 0.02]") == std::string::npos);
}

// ── 批 13.14a：带 pitch 的解析 yaw 重映射（temporal_yaw_remap）────────────────
//
// 金标准 = **显式构造两帧相机基**（yaw 与 yaw+δ 各建一次，经世界系回投）。基的三列
// **逐字照抄** `rgl/camera.cppm` 的 `cameraRight/Up/Forward`（见 `engineBasis`），
// 与被测实现（合并成 `R_x(p)·R_y(−δ)·R_x(−p)` 一步）之间**没有任何可共享的中间约定**。
// 两者的公共假设只剩「yaw 会消掉」——那正是本判据要验的命题，所以这个对照**是独立的**。
//
// 为什么这条必须存在：本文件的**手推闭式连错两次**（求逆漏 `(ABC)⁻¹ = C⁻¹B⁻¹A⁻¹`
// 的顺序翻转一次、符号展开错一次），第二次错到 pitch≠0 时偏 1.76px 而 pitch=0 时
// 完全正确 —— 也就是说**只测 pitch=0 的退化情形抓不到它**。故金标准必须扫 pitch。
//
// ### ⚠ 但「独立金标准」也漏过一次：**共用的基**比共用的公式更危险
//
// 本判据第一版金标准与实现**都用** `R_y(yaw)·R_x(pitch)` 当相机基，而该矩阵第三列是
// `−fwd`（z 轴朝后）⟹ 两边**同时**把 yaw 符号弄反，对拍拿到 2.3e-13px 全绿，实机 φ
// 标定扫描却给出 argmin = **−1**（`alignedΔ` 比完全不对齐还大 29%）。
// ⟹ **自搭金标准能钉「实现 == 我的模型」，钉不了「模型 == 引擎」。** 剩下那一半
// 只能由实机数据裁决（`temporal-tail --motion camera` 的 φ 标定扫描）。
//
// ### 变异红线（2026-10-07 当场注入实测，四条各验一条，全部验红后才入账）
//
// | 注入的变异 | 变红的断言（读数） |
// |---|---|
// | 求逆顺序翻转 `R_x(−p)·R_y(δ)·R_x(p)`（**我犯的原错**） | ① 金标准 13.46px |
// | pitch 乘 0（退化成一维） | ① 金标准 7.74px ＋ ⑤ pitch 锚点 `max|Δv| = 0.000` |
// | **yaw 增量反号**（= 用朝后的基，本轮实机抓到的那个错） | ① 53.63px ＋ ③b 互逆 9.38px ＋ ④ 符号锚点 `map_x(中心)=643.04` ＋ **算子链合成帧对 `aligned = 2.02 × raw`** ＋ H⁻¹ 反向用例互换 |
// | `Ksi` 主点 y 符号写反（推导时差点写错的那一位） | ② δ=0 恒等 720px ＋ ① 730.9px ＋ ③b ＋ maxShiftPx |
//
// ⚠ 变异 2 的第一版写成把 `rotX` 整个删掉，拿到的是 `-Werror=unused-parameter` **编译红**
//   而不是判据红（skill `rgl-test-discipline` 记的那条坑）。改成 `pitchRad * 0.0`
//   保留参数使用，才拿到真正的判据红。
// ⚠ 变异 3 那一行**在修正前也存在**（当时读数 54.93px），却仍然没拦住这个错——因为它
//   只说「与金标准不符」，而金标准与实现共享同一个反了的约定。⟹ 入账时把「变红」与
//   「红得有意义」分开记；判据若抄实现的结论，它就不是判据（④ 当时正是绿的）。

/// testYawRemap 的针孔相机（1280×720、垂直 fov 45°、主点居中）+ **金标准**。
/// 抽成 fixture 的理由：两条测试共用，且金标准独立命名才看得出它**没有**复用被测实现。
struct YawRemapFixture {
    double f = 360.0 / std::tan(0.5 * 45.0 * 3.14159265358979323846 / 180.0);
    double cx = 640.0, cy = 360.0;
    cv::Size sz { 1280, 720 };
    /// 采样点：四角 + 中心 + 两条边上的点。⚠ 必须是**数组**而不是嵌套花括号的
    /// `initializer_list`——后者在这里推导成 void（`cannot use type 'void' as a range`）。
    inline static const double kPts[5][2] = { { 0, 0 }, { 640, 360 }, { 1279, 719 },
        { 100, 700 }, { 1200, 20 } };

    /// 引擎相机基：三列**逐字照抄** `rgl/camera.cppm` 的 `cameraRight/Up/Forward`。
    /// ⚠ 金标准必须用这个而不是 `rotY(yaw)·rotX(pitch)`——后者的第三列是 `−fwd`
    ///   （z 轴朝后），拿它配 `u = cx + c[0]/c[2]·f` 会把 yaw 符号静默反掉。金标准若与
    ///   被测实现**共用同一个朝后的基**，对拍再准也测不出符号（首轮 2.3e-13px 全绿的真相）。
    ///   逐列照抄 ⟹ 金标准与实现之间**没有任何可共享的中间约定**。
    static cv::Matx33d engineBasis(double yaw, double pitch)
    {
        const double cy = std::cos(yaw), sy = std::sin(yaw);
        const double cp = std::cos(pitch), sp = std::sin(pitch);
        return cv::Matx33d(
            cy, sy * sp, -sy * cp, // 行 0：right.x, up.x, fwd.x
            0.0, cp, sp,           // 行 1：right.y, up.y, fwd.y
            -sy, cy * sp, -cy * cp // 行 2：right.z, up.z, fwd.z
        );
    }
    static std::pair<double, double> apply(const cv::Matx33d& H, double x, double y)
    {
        const cv::Vec3d q = H * cv::Vec3d(x, y, 1.0);
        return { q[0] / q[2], q[1] / q[2] };
    }
    /// 金标准：像素 → t+1 帧相机方向 → 世界 → t 帧相机方向 → 像素（完全不做化简）。
    /// 两帧各用**显式**的引擎基投影，与被测单应完全无关。
    std::pair<double, double> truth(double x, double y, double yaw, double pitch, double d) const
    {
        const cv::Matx33d Rt = engineBasis(yaw, pitch), Rt1 = engineBasis(yaw + d, pitch);
        const cv::Vec3d w = Rt1 * cv::Vec3d((x - cx) / f, (cy - y) / f, 1.0);
        const cv::Vec3d c0 = Rt.t() * w;
        return { cx + c0[0] / c0[2] * f, cy - c0[1] / c0[2] * f };
    }
};

/// ① 金标准对拍 + ③b 与既有闭式一致（**正确性**侧）。
void testYawRemapAgainstTruth(Checker& c)
{
    const YawRemapFixture fx;
    // 三个 pitch × 三个 yaw × 两个 δ。**同时**钉住正确性与「与 yaw 无关」
    // （金标准显式换 yaw，被测实现压根不含 yaw 参数 ⟹ 一旦 yaw 其实消不掉就红）。
    double worst = 0.0;
    for (const double p : { 0.0, -0.401425, 0.541052 })
        for (const double yw : { 0.0, 0.7, -1.3 })
            for (const double d : { 0.0035, 0.02 }) {
                const cv::Matx33d H = yawRemapHomography(d, p, fx.f, fx.cx, fx.cy);
                for (const auto& q : YawRemapFixture::kPts) {
                    const auto a = YawRemapFixture::apply(H, q[0], q[1]);
                    const auto b = fx.truth(q[0], q[1], yw, p, d);
                    worst = std::max(
                        { worst, std::abs(a.first - b.first), std::abs(a.second - b.second) });
                }
            }
    c.chk("yaw 重映射对金标准（显式两帧相机基，含 pitch）", worst < 1e-9,
        "worst=" + std::to_string(worst) + "px");

    // ③b pitch = 0 必须与既有闭式 `yawHomography` **互为逆**（不是相等！）。
    //   两条独立推导的**方向约定不同**：既有那条返回**前向**映射（t → t+1，所以
    //   `temporal_homography` 用之前 `buildSrcMap(H.inv(), …)` 取逆），本实现返回
    //   **src map**（t+1 → t，直接喂 remap）。
    //   ⚠ 本条第一版写的是「相等」并且**是绿的**——那时两边共享同一个朝后的基。它当时
    //   给我的信心恰好是错的：跨函数对拍只在对两边**方向语义**都写对时才有意义，
    //   而 docstring 里的方向没人验。⟹ 现在显式比 `legacy(δ).inv()`，把约定写进判据。
    const cv::Matx33d legacy = yawHomography(0.0035, fx.f, fx.cx, fx.cy).inv();
    const cv::Matx33d mine = yawRemapHomography(0.0035, 0.0, fx.f, fx.cx, fx.cy);
    double worstLegacy = 0.0;
    for (const auto& q : YawRemapFixture::kPts) {
        const auto a = YawRemapFixture::apply(legacy, q[0], q[1]);
        const auto b = YawRemapFixture::apply(mine, q[0], q[1]);
        worstLegacy = std::max(
            { worstLegacy, std::abs(a.first - b.first), std::abs(a.second - b.second) });
    }
    c.chk("pitch=0 与既有 yawHomography 互逆（方向约定：它是前向、我是 src map）",
        worstLegacy < 5e-3, "worst=" + std::to_string(worstLegacy) + "px（既有闭式是二阶近似）");
}

/// ② 恒等 / ③ 几何形状 / ④ 符号 / ⑤ pitch 不可忽略（**退化与方向**侧）。
void testYawRemapAnchors(Checker& c)
{
    const YawRemapFixture fx;

    // ② δ = 0 必须逐位恒等（反向用例：任何"顺手加的偏移/半像素修正"都会让它红）
    const YawMap id = buildYawMap(yawRemapHomography(0.0, -0.4, fx.f, fx.cx, fx.cy), fx.sz);
    double worstId = 0.0;
    for (int y = 0; y < fx.sz.height; y += 37)
        for (int x = 0; x < fx.sz.width; x += 41)
            worstId = std::max({ worstId,
                std::abs(static_cast<double>(id.mapX.at<float>(y, x)) - x),
                std::abs(static_cast<double>(id.mapY.at<float>(y, x)) - y) });
    c.chk("δ=0 逐位恒等", worstId < 1e-3,
        "worst=" + std::to_string(worstId) + "px（float map 精度）");

    // ③ pitch = 0 时**垂直线必须映到垂直线**（map_x 与 y 无关）——「绕竖轴转只改水平」的
    //   正确表述。⚠ 直觉版本「v 不变」是**错的**（本条第一版就栽在这）：透视除法给出
    //   v′ = v/(1 − u·tanδ)，分母与 u 耦合 ⟹ v 实测确实变 0.92px。那正是
    //   temporal_homography 头注释写的「垂直线仍映到垂直线，y′ 的分母与 x 耦合 =
    //   **透视收缩，不是仿射**」。把它机器化，防将来有人把它"优化"成仿射。
    const YawMap p0 = buildYawMap(yawRemapHomography(0.0035, 0.0, fx.f, fx.cx, fx.cy), fx.sz);
    double worstCol = 0.0;
    for (const int x : { 0, 200, 640, 1000, 1279 })
        for (const int y : { 0, 180, 360, 719 })
            worstCol = std::max(worstCol,
                std::abs(static_cast<double>(p0.mapX.at<float>(y, x)) - p0.mapX.at<float>(0, x)));
    c.chk("pitch=0 时垂直线映到垂直线（map_x 与 y 无关）", worstCol < 1e-3,
        "worst=" + std::to_string(worstCol) + "px");

    // ③b 透视收缩必须**在场**（阳性锚点）：v 的位移随 x 变化 ⟹ 不是仿射。
    //   与 ③ 配成一对：③ 说「x 方向不随 y 变」，③b 说「y 方向随 x 变」，两条同时成立
    //   才是纯旋转单应；少任何一条都退化成别的变换。
    //   ⚠ 必须取**远离主点**的行：v = (cy − y)/f 在 y = cy 处恰为 0 ⟹ v′ ≡ 0，
    //   收缩在那一行不显现（第一版取 y=360 主点行，测出 Δv ≡ 0 差点被读成「量具有问题」）。
    const double vAtLeft = static_cast<double>(p0.mapY.at<float>(100, 0)) - 100.0;
    const double vAtRight = static_cast<double>(p0.mapY.at<float>(100, 1279)) - 100.0;
    c.chk("透视收缩在场（v 位移随 x 变 ⟹ 非仿射）", std::abs(vAtLeft - vAtRight) > 0.5,
        "Δv(左) = " + std::to_string(vAtLeft) + " vs Δv(右) = " + std::to_string(vAtRight));

    // ④ 符号锚点（**本轮实机 φ 标定扫描 argmin = −1 逼出来的改写**）。
    //   从引擎公式直接推，不靠记忆：`right = (cos yaw, 0, −sin yaw)`，取正前方世界点
    //   P = (0,0,−d) ⟹ `right·P = d·sin yaw` 随 yaw **增大**而增大 ⟹ 世界固定点在画面里
    //   **右移** `f·tan δ`。src map 要取「上一帧这块内容在哪」⟹ 必须往**左**取。
    //   ⟹ δ > 0 时 `map_x(中心) < cx`。原断言写的是 `> 641`，与被测实现**同一个错约定**，
    //   所以它当时是绿的——锚点抄实现的结论 = 没有锚点。
    const double shifted = p0.mapX.at<float>(360, 640);
    c.chk("符号：δ>0 时 src map 往左取（引擎 right·P 随 yaw 增）", shifted < 639.0,
        "map_x(中心) = " + std::to_string(shifted) + "（期望 < 639；内容右移 f·tanδ）");

    // ⑤ **pitch 不可忽略**的阳性锚点：bistro 机位 pitch ≈ −23° 时，v 方向的位移必须
    //   达到亚像素量级。哪天有人把 pitch 参数退化掉，这条会红（②③③b 全都测不出来）。
    const double pitch = -23.0 * 3.14159265358979323846 / 180.0;
    const YawMap pm = buildYawMap(yawRemapHomography(0.0035, pitch, fx.f, fx.cx, fx.cy), fx.sz);
    double worstPitch = 0.0;
    for (int y = 0; y < fx.sz.height; y += 37)
        for (int x = 0; x < fx.sz.width; x += 41)
            worstPitch = std::max(worstPitch,
                std::abs(static_cast<double>(pm.mapY.at<float>(y, x)) - p0.mapY.at<float>(y, x)));
    c.chk("pitch 不可忽略（−23° 时 v 位移达亚像素）", worstPitch > 0.5,
        "max|Δv| = " + std::to_string(worstPitch) + "px/帧（实测 0.952）");
    c.chk("maxShiftPx 量级合理（0.2°/帧 ≈ 3px）", pm.maxShiftPx > 2.0 && pm.maxShiftPx < 5.0,
        "maxShiftPx = " + std::to_string(pm.maxShiftPx));
}

/**
 * **端到端算子链自检**：合成一对「纯 yaw」帧，φ 对齐必须把差塌掉。
 *
 * 为什么必须有（本轮实测教训）：`yawRemapHomography` 对金标准对拍到 2.3e-13px，**但那条
 * 自检只测矩阵**，从没测过 `buildYawMap` + `remapWith` 的**合成分支**（`dst = src∘H`
 * 还是 `src∘H⁻¹`）。应用侧反一次 ⟹ 净 **2δ** 位移 ⟹ 对齐后**比完全不对齐还差**，
 * 而首轮实机读数恰好就是这个形态（`alignedΔ = 1.29 × rawΔ`，量级也对上 2δ 的帧差）。
 *
 * 两帧由**各自显式的相机基**投影同一个球面图案生成（**不走被测单应**），⟹ 本条测的是
 * 「实现 == 我的模型」之外的东西：**算子链的合成本身**。
 * ⚠ 它仍测不到「引擎 `frame_yaw` 的正方向 ≠ 本变换的 δ 正方向」——那一半只能由
 * `temporal-tail --motion camera` 的 **φ 标定扫描**（实机数据）裁决。两条各守一半。
 */
void testSmearPhiChain(Checker& c)
{
    const cv::Size sz(256, 144);
    const double f = 869.117, cx = sz.width / 2.0, cy = sz.height / 2.0;
    const double d = 0.0035, pitch = -0.401425, yaw0 = 0.7;

    // 世界方向上的确定图案（与相机无关 ⟹ 两帧看的是同一个"场景"）。
    // 用高频正弦积而不是棋盘：边缘丰富、无周期性巧合，且不需要随机数。
    auto scene = [](const cv::Vec3d& w) {
        const cv::Vec3d v = w * (1.0 / std::max(1e-12, cv::norm(w)));
        return 0.5 + 0.5 * std::sin(18.0 * v[0]) * std::sin(14.0 * v[1] + 3.0 * v[2]);
    };
    auto render = [&](double yaw) {
        cv::Mat img(sz, CV_64F);
        const cv::Matx33d R = YawRemapFixture::engineBasis(yaw, pitch);
        for (int y = 0; y < sz.height; ++y)
            for (int x = 0; x < sz.width; ++x)
                img.at<double>(y, x) = scene(R * cv::Vec3d((x - cx) / f, (cy - y) / f, 1.0));
        return img;
    };
    const cv::Mat prev = render(yaw0), cur = render(yaw0 + d);

    const cv::Matx33d H = yawRemapHomography(d, pitch, f, cx, cy);
    const cv::Mat aligned = remapWith(prev, buildYawMap(H, sz));

    // 只在中部统计：remap 的 BORDER_CONSTANT 带会造出巨大假差（首轮差点把它读成信号）
    const cv::Rect roi(40, 24, sz.width - 80, sz.height - 48);
    const double raw = cv::mean(cv::abs(cur(roi) - prev(roi)))[0];
    const double res = cv::mean(cv::abs(cur(roi) - aligned(roi)))[0];
    c.chk("φ 算子链把合成纯旋转帧对的差塌掉（合成分支不是 H⁻¹）",
        res < 0.15 * raw && raw > 1e-3,
        "raw=" + std::to_string(raw) + " → aligned=" + std::to_string(res) + "（比值 "
            + std::to_string(raw > 0.0 ? res / raw : 0.0) + "，线 < 0.15）");

    // 反向：喂 H⁻¹ 必须**不塌**，否则上面那条是空洞绿。
    // 注：本变换族满足 `H(δ)⁻¹ = H(−δ)`（纯旋转单应，取逆 = 反向转同一个角），
    // 所以这条反向用例同时就是「应用侧符号反了」的**阳性对照**：首轮实机读数的形态
    // （alignedΔ > rawΔ）正是它该有的样子，而当时没有任何一条自检能复现它。
    const double wrong
        = cv::mean(cv::abs(cur(roi) - remapWith(prev, buildYawMap(H.inv(), sz))(roi)))[0];
    c.chk("同图喂 H⁻¹ 必须不塌（防空洞绿）", wrong > 3.0 * res,
        "H⁻¹ 残差 " + std::to_string(wrong) + " vs 正确 H 残差 " + std::to_string(res));
}

int cmdSelftest()
{
    Checker c;
    testSrgb(c);
    testObjectId(c);
    testRegion(c);
    testClusters(c);
    testAnalytic(c);
    testRegistry(c);
    testJudgmentFamilies(c);
    testAntiPatterns(c);
    testTemporalDiff(c);
    testAaCoverage(c);
    testAoMetrics(c);
    testTemporalResidual(c);
    testLumaGradBands(c);
    testRejectCrossTab(c);
    testBoostFidelity(c);
    testTemporalFlicker(c);
    testYawRemapAgainstTruth(c);
    testYawRemapAnchors(c);
    testSmearPhiChain(c);
    testKeyValidation(c);
    testCliRegistryShape(c);
    testCliArgsBehavior(c);
    std::printf("%s：%d 条失败\n", c.bad ? "自检失败" : "自检通过", c.bad);
    return c.bad ? 1 : 0;
}

}  // namespace RGL::imgqa
