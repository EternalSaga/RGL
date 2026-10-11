//
// temporal_homography.cpp — 精确单应 warp 的逐像素闪烁归因图
//
// 一句话：相机做**原地**绕 up 轴 yaw（`frame_yaw`，位置不动）⟹ 像面映射是纯旋转单应，
// 与深度无关、对整幅画面精确。把上一帧按它搬到本帧网格，剩下的差 = **搬不动的部分**
// = 采样格移动造成的锯齿差（TAA 关）/ 时域滤波做的事（TAA 开）。逐像素画出来 = 归因图。
//
// 三条纪律：
//   1. **零参考图、零对齐搜索**：单应由 tan(Δψ) + 焦距 + 主点闭式给出；符号（yaw 正方向
//      对应像面左移还是右移）用 t 与 −t **两个闭式假设对拍**取小者，不是亚像素搜索。
//      两个假设的残差都回显——符号搞错时两者差一个量级，当场可辨。
//   2. **warp 自己要插值** ⟹ 残差里混着插值误差。同批出**往返地板** |I − H⁻¹(H·I)| 的
//      p99.5：残差与地板同量级 = 量具在响，不是闪烁。
//   3. **解释率**才是主数：1 − 残差/原始帧差。残差小但原始帧差也小 = 什么都没解释。
//
//   本 TU = **采集与命令**（runHomography / deliveryGlb / cmdTemporalHomography）。重投影算子在
//   `temporal_homography_metric.cpp`、运动带判据（尺子 / 守卫 / 六格表 / 材质表）在
//   `temporal_homography_bands.cpp`，跨 TU 的类型与常量唯一出处 = `temporal_homography_metric.h`。
//   缝的完整说明写在那个头里。
//
#include "temporal_homography.h"
#include "temporal_homography_metric.h"

#include "material_attribution.h"
#include "temporal_flicker.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

constexpr int kSize[2] = { 1280, 720 };

std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}
std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f%%", v * 100.0);
    return b;
}

/// 对齐自证的扫描点（**跑前写死**）：含 0（没对齐）、±1（闭式，含反号）、两侧各两档。
inline const double kAlignSweepAlphas[]
    = { 0.0, 0.5, 0.75, 1.0, 1.25, 1.5, 2.0, -0.5, -0.75, -1.0, -1.25, -1.5, -2.0 };

/**
 * 从交付配置里读**当前用的那个 glb**，拼成 build 下的实际路径。
 *
 * 为什么不硬编码 `bistro_exterior.glb`：材质名的唯一真相源就是「引擎这次实际加载的那个 glb」。
 * 换模型时硬编码会**静默**给出旧模型的名字（数字照样出、名字全错），比崩掉更难查。
 */
std::string deliveryGlb(const std::string& configDir)
{
    std::ifstream in(configDir + "/scene.lua");
    if (!in)
        throw std::runtime_error("读不到交付配置: " + configDir + "/scene.lua");
    const std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 逐行找**未注释**的 `model = "...glb"`。为什么不用 `std::regex` 的 `(?m)`：本工具链的
    // `std::regex` 走 ECMAScript 语法，`(?...)` 只认零宽断言，`(?m)` 直接抛
    // `Invalid '(?...)' zero-width assertion`（实测踩过）。逐行 + 行首锚定既没有这个坑，
    // 也天然跳过 `-- model = ...` 这种注释行（它们的首个非空字符是 `-`）。
    static const std::regex rx(R"rx(^\s*model\s*=\s*"([^"]+\.glb)")rx");
    std::istringstream lines(txt);
    for (std::string line; std::getline(lines, line);) {
        std::smatch m;
        if (std::regex_search(line, m, rx))
            return projectRoot() + "/build/rgl/" + m[1].str();
    }
    throw std::runtime_error("交付配置 scene.lua 里没有未注释的 model 键，材质归因无法建立映射");
}

/// 全帧分位：1px 边缘只占 0.16% 像素，p99.5 会把它整条抹平（实测踩过）
constexpr double kP = 0.999;

/// warp 核自检回显（**工具自己的**，不是引擎的）：核抄错 / 坐标约定与 OpenCV 不一致 ⟹ 数字全废。
void printWarpSelfCheck(
    const cv::Mat& probeFrame, double yawRadPerFrame, double focal, double cx, double cy)
{
        const cv::Matx33d probe = yawHomography(yawRadPerFrame, focal, cx, cy);
        const double idL = identityCheck(probeFrame, WarpKernel::Lanczos3);
        const double idC = identityCheck(probeFrame, WarpKernel::CatmullRom);
        const double idB = identityCheck(probeFrame, WarpKernel::Bilinear);
        const double trL = translationCheck(probeFrame, WarpKernel::Lanczos3);
        const double trC = translationCheck(probeFrame, WarpKernel::CatmullRom);
        const double bl = bilinearCrossCheck(probeFrame, probe.inv());
        const bool exact = idL == 0.0 && idC == 0.0 && idB == 0.0 && trL == 0.0 && trC == 0.0;
        std::printf("\n  ── warp 核自检（**工具自己的**，不是引擎的）──\n");
        std::printf("    恒等性（零偏移逐位取回原值）L3 %.3e ｜ CR %.3e ｜ bilinear %.3e\n", idL, idC,
            idB);
        std::printf("    整数平移一列（钉住坐标约定 **+ 抽头基址 kBase**）L3 %.3e ｜ CR %.3e\n", trL,
            trC);
        std::printf("      %s\n",
            exact ? "✅ 两条都 = 0：核与引擎逐式同权重、抽头基址与坐标约定同源（自证的「零」真的是零）"
                  : "❌ 核抄错了（恒等性或整数平移不精确）——本次全部数字作废");
        std::printf("    与 cv::warpPerspective(INTER_LINEAR,BORDER_REPLICATE) 比量级：%.3e"
                    "（OpenCV 走 5 位定点权重，**本就该差到 1e-2 量级**；约定真错会是 O(1)，"
                    "差三个量级）%s\n",
            bl, bl < 0.1 ? "✅ 量级正常" : "❌ 量级不对，坐标/边界约定有问题");
}

/// 两个符号假设各跑一遍（闭式对拍，不是搜索）。下标 0 = +Δψ、1 = −Δψ
struct SignSweep {
    double resid[2] = { 0.0, 0.0 };
    std::vector<cv::Mat> heat;      ///< CV_32FC1，逐像素跨帧 max（选定符号后才写盘）
    std::vector<cv::Mat> residMax;  ///< 跨帧最大残差场（CV_64F，守卫与分带共用）
};

SignSweep sweepBothSigns(const FlickerSeries& g, const SrcMap& mapF, const SrcMap& mapR,
    WarpKernel kernel, const cv::Size& sz)
{
    SignSweep sw;
    sw.heat = std::vector<cv::Mat>(2, cv::Mat::zeros(sz, CV_32FC1));
    sw.residMax = std::vector<cv::Mat>(2);
    for (int si = 0; si < 2; ++si) {
        const SrcMap& mp = si == 0 ? mapF : mapR;
        for (size_t k = 1; k < g.luma.size(); ++k) {
            cv::Mat warped, res;
            warpKernel(g.luma[k - 1], mp, kernel, warped);
            cv::absdiff(g.luma[k], warped, res);
            maxInto(sw.residMax[static_cast<size_t>(si)], res);
            cv::Mat res32;
            res.convertTo(res32, CV_32FC1);
            sw.resid[si] += histPercentile(res32, kP);
            cv::max(sw.heat[static_cast<size_t>(si)], res32, sw.heat[static_cast<size_t>(si)]);
        }
    }
    return sw;
}

/// 原始帧差（解释率的分母）+ 往返地板（warp 插值自身误差，与符号无关）
/// + **符号自证**：两帧各自搬 Δψ/2 到**同一个中间视点**后对拍。
///   差值若由运动造成 ⟹ 搬到同一点后应当**塌到地板**；塌不下来 = 差值与运动无关。
struct RawFloor {
    double rawP995 = 0.0, floorP995 = 0.0;
    cv::Mat rawMax, floorHeat;  ///< 两样的跨帧最大场（均值口径 = 守卫用的那一份）
};

RawFloor rawAndFloor(
    const FlickerSeries& g, const SrcMap& mapF, const SrcMap& mapBack, WarpKernel kernel)
{
    RawFloor rf;
    double rawSum = 0.0, floorSum = 0.0;
    cv::Mat rawMax, floorHeat;
    for (size_t k = 1; k < g.luma.size(); ++k) {
        cv::Mat raw, there, back, rt;
        cv::absdiff(g.luma[k], g.luma[k - 1], raw);
        maxInto(rawMax, raw);
        cv::Mat raw32;
        raw.convertTo(raw32, CV_32FC1);
        rawSum += histPercentile(raw32, kP);
        warpKernel(g.luma[k], mapF, kernel, there);
        warpKernel(there, mapBack, kernel, back);
        cv::absdiff(g.luma[k], back, rt);
        maxInto(floorHeat, rt);
        cv::Mat rt32;
        rt.convertTo(rt32, CV_32FC1);
        floorSum += histPercentile(rt32, kP);
    }
    const double pairs = static_cast<double>(g.luma.size() - 1);
    rf.rawP995 = rawSum / pairs;
    rf.floorP995 = floorSum / pairs;
    rf.rawMax = rawMax;
    rf.floorHeat = floorHeat;
    return rf;
}

/// 残差 > 1 显示 LSB 的像素占比（对全帧）
double fracOverOneLsb(const cv::Mat& heat, const cv::Size& sz)
{
    cv::Mat over;
    cv::threshold(heat, over, 1.0 / 255.0, 1.0, cv::THRESH_BINARY);
    return static_cast<double>(cv::countNonZero(over))
        / (static_cast<double>(sz.width) * static_cast<double>(sz.height));
}

HomoStat runHomography(const FlickerSeries& g, double yawRadPerFrame, const std::string& heatPath,
    WarpKernel kernel, bool echoSelfCheck, bool doAlignSweep)
{
    HomoStat s;
    s.name = g.name;
    s.size = g.frameSize;
    s.byteMaxAdjacent = g.byteMaxAdjacent;
    s.lumaMean = g.byteMean.empty() ? -1.0 : cv::mean(g.byteMean)[0];
    s.kernel = kernel;
    if (g.luma.size() < 3)
        throw std::runtime_error(g.name + "：帧数不足 3，无法做相邻帧归因");

    const double focal = verticalFocalPx(g.frameSize.height, kFovDeg);
    const double cx = static_cast<double>(g.frameSize.width) * 0.5;
    const double cy = static_cast<double>(g.frameSize.height) * 0.5;
    const cv::Size sz = g.frameSize;
    // ⚠ 自检先跑（判据工具自己要有自检）：核抄错 / 坐标约定与 OpenCV 不一致，后面全部数字作废。
    if (echoSelfCheck)
        printWarpSelfCheck(g.luma[1], yawRadPerFrame, focal, cx, cy);

    // 单应与帧无关 ⟹ 每个矩阵只建一次映射表
    const cv::Matx33d Hf = yawHomography(yawRadPerFrame, focal, cx, cy);
    const cv::Matx33d Hr = yawHomography(-yawRadPerFrame, focal, cx, cy);
    const SrcMap mapF = buildSrcMap(Hf.inv(), sz);  ///< 把上一帧**推进** Δψ 到本帧网格
    const SrcMap mapR = buildSrcMap(Hr.inv(), sz);  ///< 反号假设
    const SrcMap mapBack = buildSrcMap(Hf, sz);  ///< 往返地板第二段：dst→src = H（与 mapF 互逆）

    // 两个符号假设各跑一遍（闭式对拍，不是搜索）；热图留到选定符号后再出
    const SignSweep sw = sweepBothSigns(g, mapF, mapR, kernel, sz);
    const size_t pairs = g.luma.size() - 1;
    s.pairs = pairs;
    s.residP995 = std::min(sw.resid[0], sw.resid[1]) / static_cast<double>(pairs);
    s.residOther = std::max(sw.resid[0], sw.resid[1]) / static_cast<double>(pairs);
    const int best = sw.resid[0] <= sw.resid[1] ? 0 : 1;

    const RawFloor rf = rawAndFloor(g, mapF, mapBack, kernel);
    s.rawP995 = rf.rawP995;
    s.floorP995 = rf.floorP995;
    s.rawMean = meanOf(rf.rawMax);
    s.residMean = meanOf(sw.residMax[static_cast<size_t>(best)]);
    s.floorMean = meanOf(rf.floorHeat);

    // 对齐自证扫描（**只在主表 = TAA 关那组跑**：守卫定义在那一组，另一组跑一遍只是白花 GPU）
    if (doAlignSweep) {
        for (double a: kAlignSweepAlphas)
            s.alignCurve.emplace_back(a, sweepResidual(g, yawRadPerFrame, a, kernel));
    }
    s.heat = sw.heat[static_cast<size_t>(best)];
    s.floorHeat = rf.floorHeat;
    s.frac1Lsb = fracOverOneLsb(s.heat, sz);
    if (!heatPath.empty())
        writeHeat(s.heat, heatPath);
    return s;
}

/// A/B 侧覆写的两个门（负数 = 不覆写 = 交付配置原样）
struct AbOverride {
    bool env = false;  ///< env.intensity 覆写生效
    bool ao = false;   ///< ao.strength 覆写生效
};

/// A/B 侧回显（**跑之前就出**，让读日志的人第一眼就知道这不是交付口径）
AbOverride echoAbOverride(double envIntensity, double aoStrength)
{
    const bool abEnv = envIntensity >= 0.0, abAo = aoStrength >= 0.0;
    if (abEnv || abAo) {
        std::printf("  ⚠ **A/B 口径**（不是交付配置）：");
        if (abEnv)
            std::printf("env.intensity = %g ", envIntensity);
        if (abAo)
            std::printf("%sao.strength = %g", abEnv ? "· " : "", aoStrength);
        std::printf("\n    两个门都是 IEEE 精确旁路（ao：tonemap 的 lerp(1,ao,0)≡1；env：几何体的 "
                    "IBL diffuse+specular 都乘 env.intensity）\n");
        if (abEnv)
            std::printf("    ⚠ 但 **天穹背景不受 env.intensity 影响**（skybox.frag.slang 与 "
                        "light.frag.slang 的 envSky 分支直接返回 env 采样、不乘它）\n"
                        "      ⟹ 背景会**原样亮着**。「背景还在」≠「IBL 没关掉」，"
                        "看几何体别看天。\n");
        if (abEnv && envIntensity <= 0.0)
            std::printf("    ⚠ 更要紧：env 是本场景**唯一的环境光**（scene.lua 原注释：0.2 时阴影面只剩"
                        "受光的 0.2%%，\n"
                        "      即「楼房背光面过暗」的根因）⟹ 只吃天光的机位（门框 / 门板）在 0 下"
                        "**整个关黑**。\n"
                        "      那种跑法守卫 ⓪ 会当场红并 rc≠0 —— 这是**预期**（没有画面可归因），"
                        "不是量具坏了。\n"
                        "      要问「IBL 是不是驱动」请改用**剂量响应**（`--env 0.5` / `0.25`），"
                        "别用开关。\n");
    }
    return { abEnv, abAo };
}

/// 两态（TAA 关 / 开）各采一段连拍并跑单应归因的产物；`--bands` 才顺带采 objectID 与分带尺子
struct HomoCapture {
    std::vector<HomoStat> stats;  ///< [0] = TAA 关（主表），[1] = TAA 开
    std::vector<cv::Mat> idsOf;   ///< 与 stats 同序；TAA 关那组带 objectID（材质归因用）
    std::vector<double> idErrOf;  ///< (yawErrPx, idBestDiff) 成对
    std::vector<int> idFrameOf;
    std::vector<double> idDiffOf;
    MovingBandRuler ruler;
    bool rulerOk = false;
};

/// 该臂的热图路径（`--out` 为空 = 不写盘）。后缀按臂固定，两组才能直接对眼。
std::string heatPathOf(const std::string& outPrefix, bool on)
{
    return outPrefix.empty() ? std::string()
        : outPrefix + (on ? "_taa_on_heat.png" : "_taa_off_heat.png");
}

/// objectID 只在 **TAA 关**那组采一次（材质是慢变量、两态同一姿态，理由见 FlickerSeries 注释）
bool wantObjectId(bool bands, bool edgeTarget, bool on)
{
    return bands && !edgeTarget && !on;
}

/// 返回 false = 采集失败（`[失败]` 已打印，调用方 rc=1）
bool captureBothStates(bool edgeTarget, const BistroCam* cam, double yawRad, int frames,
    bool bands, const AbOverride& ab, double envIntensity, double aoStrength,
    const std::string& outPrefix, WarpKernel kKernel, HomoCapture& cap)
{
    const bool abEnv = ab.env, abAo = ab.ao;
    for (bool on: { false, true }) {
        const std::string tag = on ? "homo_on" : "homo_off";
        const std::string dir = projectRoot() + "/build/imgqa_homo_" + tag;
        std::printf("  采集 %s …\n", tag.c_str());
        try {
            FlickerSeries g = captureFlickerSeries(dir, on ? "TAA 开" : "TAA 关", tag, edgeTarget, cam,
                on, yawRad, frames, 0.0, bands, wantObjectId(bands, edgeTarget, on), envIntensity,
                aoStrength, abEnv, abAo);
            if (bands) {
                cap.idsOf.push_back(g.ids);
                cap.idErrOf.push_back(g.yawErrPx);
                cap.idFrameOf.push_back(static_cast<int>(g.idBestFrame));
                cap.idDiffOf.push_back(g.idBestDiff);
            }
            HomoStat st = runHomography(g, yawRad,
                heatPathOf(outPrefix, on),
                kKernel, !on, bands && !on);
            // 尺子取 **TAA 关** 那组并两态共用（与静止态「尺子 = 门关 = 用户现状」同纪律：
            // 两态各定一套带边界，差表就失去意义）。
            if (bands && !on) {
                cap.ruler = movingBandRulerFrom(g);
                applyFloorTau(cap.ruler, st.floorHeat);  // τ ← 本次实测地板（旧口径留 tauLegacy）
                cap.rulerOk = cap.ruler.tau > 0.0;
            }
            cap.stats.push_back(std::move(st));
        }
        catch (const std::exception& e) {
            std::printf("  [失败] %s\n", e.what());
            return 1;
        }
    }
    return true;
}

/// 两组的读数表：p99.9 直方图口径 + 均值口径（守卫用的就是它）+ 对齐自证扫描 + 两条同量级 ⚠
void printHomographyTable(const std::vector<HomoStat>& stats, WarpKernel kKernel)
{
    std::printf("\n  组       原始帧差p99.9  残差p99.9  反号残差  往返地板  残差−地板  解释率  残差>1LSB\n");
    std::printf("  （warp 核 = %s；表里的 p99.9 走直方图，守卫用**跨帧最大场的均值**，两者不是同一个数）\n",
        kernelName(kKernel));
    std::printf("  %s\n", std::string(96, '-').c_str());
    for (const auto& s: stats) {
        const double explained = s.rawP995 > 1e-9 ? 1.0 - s.residP995 / s.rawP995 : 0.0;
        std::printf("  %-7s %-14s %-11s %-10s %-10s %-11s %-8s %s\n", s.name.c_str(),
            d4(s.rawP995).c_str(), d4(s.residP995).c_str(), d4(s.residOther).c_str(),
            d4(s.floorP995).c_str(), d4(s.residP995 - s.floorP995).c_str(), d2(explained).c_str(),
            d2(s.frac1Lsb).c_str());
        std::printf("    均值口径（守卫用的就是这一行）：原始帧差 %s ｜ 残差 %s ｜ 地板 %s\n",
            d4(s.rawMean).c_str(), d4(s.residMean).c_str(), d4(s.floorMean).c_str());
        if (!s.alignCurve.empty()) {
            std::printf("    对齐自证扫描（残差 vs warp 强度 α；α=0 不 warp，±1 闭式含反号）：\n      ");
            for (const auto& [a, v]: s.alignCurve)
                std::printf("α=%+5.2f:%-8s  ", a, d4(v).c_str());
            std::printf("\n");
        }
        if (s.residP995 < s.floorP995 * 1.5)
            std::printf("    ⚠ 残差与 warp 往返地板同量级（%s vs %s）——量具在响，归因图不可当闪烁读\n",
                d4(s.residP995).c_str(), d4(s.floorP995).c_str());
        if (s.residOther < s.residP995 * 1.5)
            std::printf("    ⚠ 两个符号假设的 **p99.9** 残差同量级（%s vs %s）——p99.9 是钝尺子"
                        "（只看最坏那 0.1%%），符号能不能分以**下面那条 α 扫描**为准\n",
                d4(s.residP995).c_str(), d4(s.residOther).c_str());
    }
}

/// `--bands` 的收尾：⭐ 守卫闸门在前 → 六格表 + 材质归因 → 台账。返回退出码。
int reportBandsAndLog(const std::string& target, double sweepPxPerFrame, int frames,
    WarpKernel kKernel, HomoCapture& cap, const std::vector<GlbPrim>& prims)
{
    std::string bandVerdict = "未开";
        // ⚠ 这里**不许**再拿 `rulerOk` 当进入条件（2026-10-06 实测修）：尺子空（τ=0，画面全黑
        // 时必然）以前会整段跳过 ⟹ 既不报守卫也不报 Indeterminate，直接 rc=0 —— 正是本工具
        // 要消灭的「空洞满足」。现在无论尺子成不成都要走守卫闸门，让红变成**出声**的红。
        cv::Mat residOff, residOn;
        cap.stats[0].heat.convertTo(residOff, CV_64FC1);
        cap.stats[1].heat.convertTo(residOn, CV_64FC1);
        // ⭐ **守卫闸门在前**（2026-10-06 用户拍板）：任一守卫不过 ⟹ 只报 Indeterminate、
        //    **不出三分支判决**、**rc≠0**。原来这两条守卫只是 ⚠ 警告，实测后果是「四率全 100%%
        //    + 自证失败」照样打印「分支②「否证」」并且 rc=0 —— 判决版的空洞满足。
        MovingGuards g = evaluateMovingGuards(cap.stats[0], cap.ruler, residOff);
        if (!cap.rulerOk && g.ok)
        {
            // 兜底（防回归）：尺子空却全绿 = 守卫 ③ 失效，宁可错杀不放过
            g.ok = false;
            g.why = "③ τ 没定出来（尺子空），守卫却全绿 ⟹ 工具缺陷，请报";
        }
        printMovingGuardEcho(cap.ruler, g, kKernel);
        if (!g.ok) {
            printIndeterminate(target, g);
            logCall("temporal-homography",
                {{"target", target}, {"sweep", std::to_string(sweepPxPerFrame)},
                    {"frames", std::to_string(frames)}, {"bands", "1"},
                    {"kernel", kernelName(kKernel)},
                    {"bandVerdict", "Indeterminate（守卫未过，未出判决）"}, {"guardWhy", g.why}},
                {});
            return 1;
        }
    bandVerdict = reportMovingBandsFromHomography({ cap.ruler, residOff, residOn, target,
        cap.idsOf.empty() ? cv::Mat() : cap.idsOf[0], prims, cap.stats[0].floorMean,
        cap.stats[1].floorMean, cap.idErrOf.empty() ? 0.0 : cap.idErrOf[0],
        cap.idFrameOf.empty() ? -1 : cap.idFrameOf[0],
        cap.idDiffOf.empty() ? 0.0 : cap.idDiffOf[0],
        sweepPxPerFrame * static_cast<double>(frames) });
        logCall("temporal-homography",
            {{"target", target}, {"sweep", std::to_string(sweepPxPerFrame)},
                {"frames", std::to_string(frames)}, {"bands", "1"},
                {"kernel", kernelName(kKernel)}, {"bandVerdict", bandVerdict}},
            {});
    return 0;
}

}  // namespace

int cmdTemporalHomography(const std::string& target, double sweepPxPerFrame, int frames,
    const std::string& outPrefix, bool bands, double envIntensity, double aoStrength)
{
    bool edgeTarget = false;
    const BistroCam* cam = nullptr;
    try {
        cam = resolveFlickerTarget(target, edgeTarget);
    }
    catch (const std::exception& e) {
        std::printf("%s\n", e.what());
        return 2;
    }
    if (sweepPxPerFrame <= 0.0 || sweepPxPerFrame >= 1.0) {
        std::printf("--sweep 须在 (0,1) px/帧\n");
        return 2;
    }
    const float focal = verticalFocalPx(kSize[1], kFovDeg);
    const double yawRad = sweepToYawRad(sweepPxPerFrame, focal);

    std::printf("=== 精确单应 warp 的逐像素闪烁归因（temporal-homography）===\n");
    std::printf("  目标 %s｜扫速 %g px/帧 → Δψ = %.8f rad/帧｜帧数 %d｜主点 = 画面中心（cameraAimAt 钉的）\n",
        target.c_str(), sweepPxPerFrame, yawRad, frames);
    if (bands)
        std::printf("  --bands = 开：**机位状态 = 相机移动**（frame_yaw 每帧 Δψ），残差图另出"
                    "「亮度 × 梯度」二维分带（判决线跑前写死，见本文件注释）\n");

    // 与引擎 TAA **默认核**同族（Lanczos3）。为什么不是 bilinear：见
    // `temporal_homography_metric.cpp`「为什么原来的 cv::warpPerspective(INTER_LINEAR) 不够」段——
    // 双线性没有负瓣，糊度差本身算进残差，且让「符号自证」因核太糊而**必然**失败。
    constexpr WarpKernel kKernel = WarpKernel::Lanczos3;
    const AbOverride ab = echoAbOverride(envIntensity, aoStrength);
    // 材质归因的真相源：交付配置里当前加载的那个 glb（不硬编码模型名）
    std::vector<GlbPrim> prims;
    if (bands && !edgeTarget) {
        try {
            prims = parseGlbPrimitives(deliveryGlb(projectRoot() + "/configs"));
            std::printf("  材质映射：%zu 个 primitive（glb 摊平：mesh-major / primitive-minor）\n",
                prims.size());
        }
        catch (const std::exception& e) {
            std::printf("  ⚠ 材质归因**不可用**（%s）⟹ 只出六格表，材质表跳过\n", e.what());
        }
    }
    HomoCapture cap;
    if (!captureBothStates(edgeTarget, cam, yawRad, frames, bands, ab, envIntensity, aoStrength,
            outPrefix, kKernel, cap))
        return 1;

    printHomographyTable(cap.stats, kKernel);
    if (!outPrefix.empty())
        std::printf("\n  热图（固定标尺 0→黑 / 1→白，两组可直接对眼）: %s_taa_{on,off}_heat.png\n", outPrefix.c_str());

    // ── 批 13.9：【机位状态 = 相机移动】亮度 × 梯度二维分带（**只出数 + 一句判决**，
    //    既有四条读数与 rc 一律不动 —— 本段不产 Verdict，判决是打印的一句结论）─────────
    if (bands && cap.stats.size() == 2)
        return reportBandsAndLog(target, sweepPxPerFrame, frames, kKernel, cap, prims);
    return 0;
}

}  // namespace RGL::imgqa
