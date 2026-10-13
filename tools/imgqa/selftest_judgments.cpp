// selftest_judgments.cpp —— **判定口径与配置解析**（拆文件的缝说明全文见 `selftest.h`）。
//
// 五台仪器同处的理由 = 它们都是「判据能不能被信」的**前置口径**，且全部**不启引擎**：
//   · `testTemporalDiff`   —— temporalDiffStat（所有时域判据的统计面本体）
//   · `testKeyValidation`  —— makeProfile 的键名校验器（校验静默失效比没有校验更糟）
//   · `testEngineLogTail`  —— engine.cpp 的 logTail（与上一条同属「配置必须被引擎接受」
//                             这条自检：键名校验是判定面，logTail 是失效取证面）
//   · `testAnalytic`       —— analytic.cpp 的闭式解（与实机截图逐字节对拍过的锚点）
//   · `testAaCoverage`     —— aa_coverage.cpp 的 ACES 反解（整条覆盖率判据都压在它上面）
// 加新用例先问「它测的是哪台仪器」，别按行数往这里塞。
//
// 搬动纪律：断言本体逐字搬自拆分前的 selftest.cpp（一条不多、一条不少、不改判定方向与阈值）。
#include "selftest.h"

#include "aa_coverage.h"
#include "engine_lua_edit.h"
#include "imgqa.h"
#include "material_attribution.h"

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

void testKeyValidation(Checker& c)
{
    // 键名校验器自己也要有用例：真实解析器源码里必须能抽到 picking / rpc / mode
    // （mode 在 shadow_config_impl.cppm，N6b 起校验器覆盖两份解析器）
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
            && content.find("rpc = true") != std::string::npos);
    // N6b：阴影门不在 diagnostics.lua 了（旧键出现 = 引擎启动失败），改由同目录
    // shadow.lua 承载。默认 opts（shadowPass=false）→ mode="none" + izb=false；
    // izb 若写成 True 就是 nil 回落**代码默认 true** = 静默开 IZB（真实事故的形状）。
    std::ifstream s(tmp + "/shadow.lua");
    const std::string shadow{std::istreambuf_iterator<char>{s}, {}};
    c.chk("shadow.lua：门关 = mode=\"none\" + 小写 izb=false",
        shadow.find("mode = \"none\"") != std::string::npos
            && shadow.find("izb = false") != std::string::npos);
    // 阳性锚点：三档 mode 各自写对（枚举塌缩方向写错 = 判据跑在另一条阴影路上）。
    // 变异红线：engine.cpp 的 shadowMode 三元塌错一档 → 本断言红。
    {
        const auto modeOf = [&](bool pass, bool evssm) {
            ProfileOpts p;
            p.glb = "shadow_rig.glb";
            p.shadowPass = pass;
            p.evssmShadow = evssm;
            makeProfile(tmp, p);
            std::ifstream f(tmp + "/shadow.lua");
            const std::string t{std::istreambuf_iterator<char>{f}, {}};
            return t;
        };
        c.chk("shadow.lua：shadowPass → mode=\"csm\"",
            modeOf(true, false).find("mode = \"csm\"") != std::string::npos);
        c.chk("shadow.lua：evssmShadow → mode=\"evssm\"",
            modeOf(true, true).find("mode = \"evssm\"") != std::string::npos);
        // 矛盾组合（EVSSM 开着但图集不画）必须**抛**而不是替调用方猜一档
        ProfileOpts bad;
        bad.glb = "shadow_rig.glb";
        bad.shadowPass = false;
        bad.evssmShadow = true;
        bool threw = false;
        try {
            makeProfile(tmp, bad);
        } catch (const std::exception&) {
            threw = true;
        }
        c.chk("shadow.lua：evssm × 无图集 = 拒绝生成（不静默猜档）", threw);
    }
}

void testAnalytic(Checker& c)
{
    // 锚点 A/B：来源 = 本机 RADV（STRIX_HALO）cornell_box 探针**实机截图逐字节对拍零
    // 误差**的两组参数（2026-09-30 会话，git:调试历史 收口条目）。不是记忆值。
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

void testEngineLogTail(Checker& c)
{
    // logTail = 「配置写出来必须能被引擎接受」这条自检的**取证**部分（Engine 构造失败时
    // 把日志尾部带进异常）。债-92：起点写成 `n > lines ? n : 0` ⟹ n>lines 时**恒返回空串**
    // ——存在理由被静默废掉而两个调用点全绿。锚点直接喂合成日志，期望串按闭式整串构造
    // （行数、内容、顺序、前缀一次全钉；变异红线：起点写成 n → 空串红；写成 n−lines−1 →
    //  行数 13 红；按 ring 序打印不取模 → 40 行档顺序红）。
    const std::string path = "/tmp/imgqa_selftest_logtail.log";
    const auto writeLog = [&path](int n) {
        std::ofstream f(path);
        for (int i = 1; i <= n; ++i)
            f << "L" << i << "\n";
    };
    const auto expectTail = [](int first, int last) {
        std::string want;
        for (int i = first; i <= last; ++i)
            want += "\n    | L" + std::to_string(i);
        return want;
    };

    // 13 行 → 尾 12 行 = L2..L13（默认 lines=12；n 恰好越过边界，ring 回绕被踩到）
    writeLog(13);
    c.chk("logTail：13 行取尾 12 行 = 闭式 L2..L13 整串（债-92 改前恒空串）",
        logTail(path) == expectTail(2, 13), "实得 [" + logTail(path) + "]");
    // 40 行 → 尾 12 行 = L29..L40（回绕两圈多，起点若再错一格这里先红）
    writeLog(40);
    c.chk("logTail：40 行取尾 12 行 = 闭式 L29..L40（顺序 + 内容 + 前缀）",
        logTail(path) == expectTail(29, 40), "实得 [" + logTail(path) + "]");
    // 反向用例（边界不许顺手改坏）：n == lines 全量输出、n < lines 也全量输出
    writeLog(12);
    c.chk("logTail：n = lines 全量 12 行（边界不许多砍一行）",
        logTail(path) == expectTail(1, 12));
    writeLog(5);
    c.chk("logTail：n < lines 全量 5 行", logTail(path) == expectTail(1, 5));
    // 空文件与读不到：文案是排查者唯一能看见的东西，逐字核
    writeLog(0);
    c.chk("logTail：空文件 → (日志为空)", logTail(path) == "(日志为空)", logTail(path));
    c.chk("logTail：文件不存在 → (日志读不到: …)",
        logTail("/tmp/imgqa_selftest_logtail_不存在.log").rfind("(日志读不到: ", 0) == 0);
}

/// 债-93：`replaceValueInBlock` 从表头往后找键、**没有表尾边界** ⟹ 目标表缺该键时静默
/// 命中**后面那张表**里的同名键（`environment` 没有 `intensity` 而其后的 `light` 有 ⟹
/// 改掉的是 `light.intensity`，且一声不响——改的就是判据输入）。边界语义与
/// `overrideValueInTable` 同口径：`valueSpanAt` 按花括号配对收尾，表跨度内未命中 = 抛，
/// **不越界去别的表找**。靶 = 合成 lua（scene.lua 的形状：environment 无 intensity、light 有）。
void testLuaEditBlockBoundary(Checker& c)
{
    const std::string kScene = R"lua(return {
    environment = {
        exposure = 1.0,
    },
    light = {
        intensity = 1.0,
    },
})lua";

    // 反向：environment 没有 intensity ⟹ 必须抛，且**文本一字未动**（抛发生在替换之前）。
    // 改前这两条正是缺陷证据：第一条 FAIL（没抛），第二条 FAIL 并把 light.intensity 被
    // 改成 9.9 之后的整份文本打出来——红输出本身就是「跨表改键」的证据。
    std::string t = kScene;
    c.chk("lua 表内覆写：目标表缺键 → 抛（改前静默改到后面那张表的同名键）",
        throwsStd([&] { replaceValueInBlock(t, "scene.lua", "environment", "intensity", "9.9"); }));
    c.chk("lua 表内覆写：抛后文本未动（light.intensity 保持原值；红时 extra 即跨表改键证据）",
        t.find("intensity = 1.0") != std::string::npos, "实得文本: " + t);

    // 阳性锚点（堵「加了边界后连正常路径也不灵」）：environment 确有该键 ⟹ 只改 environment
    // 那份，light.intensity 原样。变异红线：搜索起点写成表尾、或跨度写成全文 → 本断言红。
    std::string both = R"lua(return {
    environment = {
        exposure = 1.0,
        intensity = 2.0,
    },
    light = {
        intensity = 1.0,
    },
})lua";
    replaceValueInBlock(both, "scene.lua", "environment", "intensity", "0.25");
    c.chk("lua 表内覆写阳性锚点：键在场时只改 environment 那份（light.intensity 仍是 1.0）",
        both.find("intensity = 0.25") != std::string::npos
            && both.find("intensity = 1.0") != std::string::npos
            && both.find("intensity = 0.25") < both.find("light = {"),
        "实得文本: " + both);

    // 真交付靶阳性锚点（selftest_shadow「交付配置改了形」同款形状）：消费方 = makeProfile 的
    // envIntensity 覆写门（engine_profile.cpp）。边界若把交付路径一起杀掉，这里先红；
    // environment 里若哪天没了 intensity，这里同样先红（那时该改的是调用方，不是静默改错块）。
    std::ifstream sceneIn(projectRoot() + "/configs/scene.lua");
    const std::string scene{std::istreambuf_iterator<char>{sceneIn}, {}};
    std::string scene2 = scene;
    bool sceneOk = true;
    try {
        replaceValueInBlock(scene2, "scene.lua", "environment", "intensity", "0.125");
    } catch (const std::exception& e) {
        sceneOk = false;
        std::printf("  （交付 scene.lua 覆写抛: %s）\n", e.what());
    }
    c.chk("lua 表内覆写·真交付 scene.lua：environment.intensity 覆写成功且 light 那份原样",
        sceneOk && scene2.find("intensity = 0.125") != std::string::npos
            && scene2.find("intensity = 12.0") != std::string::npos,
        "environment 无 intensity / 表头排版变了 / 边界误杀交付路径，三选一");

    // 债-93 另一半 = `topLevelValueOf` 缺 `eq == npos` 守卫（与 `overrideTopLevelValue` 不对称，
    // `eq + 1` 会回绕）。守卫已补，🔴 但**不给它写锚点**：npos 分支经 `findKeyHits` 不可达
    // （`isKeyHit` 已要求命中必带 `=`）⟹「键后无 `=`」的输入根本进不到那一行，它抛的是
    // 「找不到键」那条**另一条**路径。写出来的断言改前改后都绿 = 假锚点（买了信心不买覆盖），
    // 故删。守卫本身作为防御性对称保留。
    // 阳性锚点：正常读回值不受影响（与 overrideTopLevelValue 同一套定位判据）。
    c.chk("topLevelValueOf 阳性锚点：读回 mode 的值 = \"csm\"",
        topLevelValueOf("return {\n    mode = \"csm\",\n}\n", "mode", "shadow.lua") == "\"csm\"");
}

/// 债-99：`readGlbJsonChunk` 只校验头 magic，**不读 totalLength、不校验 chunk 长度是否越过
/// 文件尾** ⟹ 坏文件的实际行为是「一直读到 EOF」而不是报错（台账实测：假 totalLength 两版
/// 行为一致 = 都不读它）。锚点直接构造 glb 字节流（假 totalLength / chunk 越尾 / BIN chunk
/// 在前的正常件）写进 /tmp 喂 `parseGlbPrimitives`——不落盘仓库。
void testGlbChunkValidation(Checker& c)
{
    const std::string jsonChunk
        = R"({"meshes":[{"name":"m0","primitives":[{"attributes":{},"material":0}]}],"materials":[{"name":"mat0"}]})";
    std::string padded = jsonChunk;  // chunk 按 4 字节对齐（glTF：JSON 空格补齐）
    while (padded.size() % 4 != 0)
        padded += ' ';
    const auto u32 = [](uint32_t v) {
        return std::string{static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
            static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF)};
    };
    const auto writeGlb = [](const std::string& path, const std::string& bytes) {
        std::ofstream f(path, std::ios::binary);
        f << bytes;
    };
    const std::string dir = "/tmp/imgqa_selftest_glb";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    // ① 假 totalLength（比文件尾还大 4 KiB）：改前没人读它 ⟹ 照常解析成功；改后必须抛。
    const std::string big = dir + "/fake_total_length.glb";
    writeGlb(big,
        "glTF" + u32(2) + u32(static_cast<uint32_t>(28 + padded.size() + 4096))
            + u32(static_cast<uint32_t>(padded.size())) + u32(0x4E4F534A) + padded);
    c.chk("glb：假 totalLength（越过文件尾）→ 抛（改前根本不读它，照常解析）",
        throwsStd([&] { (void)parseGlbPrimitives(big); }));

    // ② chunk 声明长度越过文件尾：必须抛且文案带「越过文件尾」——改前这里**也**抛，但抛的是
    // 「JSON chunk 解析失败」（读到 EOF 后把 NUL 补齐的缓冲当 JSON），排查方向被带去怀疑
    // JSON 本体，而真正坏的是长度字段。文案断言就是这条的区分力。
    const std::string over = dir + "/chunk_over.glb";
    writeGlb(over, "glTF" + u32(2) + u32(static_cast<uint32_t>(20 + padded.size()))
            + u32(1u << 20) + u32(0x4E4F534A) + padded);
    std::string msg;
    try {
        (void)parseGlbPrimitives(over);
    } catch (const std::exception& e) {
        msg = e.what();
    }
    c.chk("glb：chunk 声明长度越过文件尾 → 抛且文案可归因（改前抛「解析失败」，方向错）",
        msg.find("越过文件尾") != std::string::npos, msg);

    // ③ 阳性锚点：BIN chunk 在前的**正常**文件（逐 chunk「找」JSON 是原注释的契约）照常解析
    // 出 1 个 primitive。堵修法反向打坏正常路径（偏移累加错一格这里先红）。
    const std::string good = dir + "/good.glb";
    const std::string bin = std::string(8, '\0');
    const uint32_t total = 28 + static_cast<uint32_t>(bin.size() + padded.size());
    writeGlb(good, "glTF" + u32(2) + u32(total) + u32(static_cast<uint32_t>(bin.size()))
            + u32(0x004E4942) + bin + u32(static_cast<uint32_t>(padded.size())) + u32(0x4E4F534A)
            + padded);
    const auto prims = parseGlbPrimitives(good);
    c.chk("glb 阳性锚点：BIN chunk 在前的正常件 → 1 个 primitive、材质名 mat0",
        prims.size() == 1 && prims[0].material == "mat0" && prims[0].mesh == "m0",
        "实得 " + std::to_string(prims.size()) + " 条");
}

}  // namespace RGL::imgqa
