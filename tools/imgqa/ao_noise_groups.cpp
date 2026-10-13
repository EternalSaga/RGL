// ao_noise_groups.cpp —— `ao-noise` 的**组 2/3/4/5/6**（取值分布 / 亚像素爬行 / 时域 /
// 掠射角 / 交付场景）。
//
// 缝按**仪器**切：`组` 本来就是这台判据工具天然的边界——一组一次采集、一组一张读数表，
// 组间唯一的共享物是 `Ctx`（只读入参）+ `Collected`（采集段产物）+ `Results`（本组读数写回），
// 于是「拆文件」与「拆函数」用的是同一条缝（原文「分组执行」那段缝说明留在 `ao_noise.cpp`）。
// 组 1/1b 不在这里：它们直接吃 `collect` 的产物、与主进程同处一个 TU，见 `ao_noise.cpp`。
//
// 搬动纪律：段内代码**逐字**搬（打印顺序 = 验收的结构签名）。只有整函数级的换位——
// `burstDiff` / `printTemporalRow` / `printBistroRow` 是本组私有小工具（匿名 namespace），
// 必须排在用它们的组 4 / 组 6 之前。纯函数换位不改行为：本文件**没有任何静态存储期状态**。
// （P1-G2 债-96：`burstDiff` 的逐帧差累计体拆到口径层 `burstDiffFiles`（早退守卫要可测），
//  本文件的 `burstDiff` 只剩「切强度 → 预热 → 连拍 → 交口径层」的仪器部分。）
//
// 已知在场但本批**不许修**的一条照原样搬着：组 4 的分区掩码（`col.flat` / `res.contactMask`）
// 是主进程机位算出来的，却套在 TAA profile 自己那台机位的 burst 上（→ 交付报告）。
#include "ao_noise_metric.h"

#include "imgqa.h"

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {
namespace {

// ──────────── 组 4 / 组 6 的私有小工具（原顺序里就在各自组之前）────────────

/**
 * 一轮 burst 的逐帧差累计（口径与「帧数不足早退出声」在 `burstDiffFiles`，债-96）。
 * 本函数只负责仪器：切强度 → 丢预热帧洗历史 → 连拍 → 交给口径层累计。
 */
std::pair<std::array<FrameDiff, 3>, int64_t> burstDiff(Engine& eng, const std::string& tag,
    double aoStrength, uint32_t nBurst, uint32_t warmFrames, const cv::Mat& flat,
    const cv::Mat& contactMask, const std::string& outDir)
{
    eng.setParam(kAoPage, kAoLabel, aoStrength);
    static_cast<void>(eng.shotBurst("screenshots", warmFrames, "aot_w" + tag));
    return burstDiffFiles(eng.shotBurst("screenshots", nBurst, "aot_c" + tag), flat, contactMask,
        tag, outDir);
}

/// 组 4 的一行：同一区域的地板（AO 关）与信号（AO 开）逐帧差 + 比值。
void printTemporalRow(const char* name, const FrameDiff& f, const FrameDiff& s)
{
    std::printf("  %-14s 地板 非零 %8lld 均值 %-9s 最大 %-5.0f │ 信号 非零 %8lld 均值 %-"
                "9s 最大 %-5.0f │ 比值 %s×\n",
        name, static_cast<long long>(f.changed), d3(f.mean).c_str(), f.mx,
        static_cast<long long>(s.changed), d3(s.mean).c_str(), s.mx,
        f.mean > 0.0 ? d3(s.mean / f.mean).c_str() : "inf");
}

/// 组 6 的一行：交付字节 Δ 的幅度与渐变/颗粒分解。
void printBistroRow(const char* name, const ByteAB& v)
{
    std::printf("  交付字节Δ %-10s 均值 %-9s p99 %-9s 最大 %-8s 渐变RMS %-9s 颗粒RMS %s\n",
        name, d3(v.mean).c_str(), d3(v.p99).c_str(), d3(v.mx).c_str(),
        d3(v.smoothRms).c_str(), d3(v.grainRms).c_str());
}

}  // namespace

/// 组 2：AO 场取值分布 + 扫描线（纯观察量，不产出判据——台阶本身不是噪声判据）。
void group2ValueDistribution(const Collected& col)
{
    const cv::Mat& ao = col.ab.on.ao;
    std::printf("\n── 组 2  AO 场取值分布（12 样本会不会走成离散台阶）─────────────\n");
    const std::vector<int> hist = aoHistogram(ao);
    int distinct = 0;
    for (int v = 0; v < 256; ++v)
        if (hist[v] > 0)
            ++distinct;
    std::printf("  不同取值 %d 个（12 样本若走成离散台阶应 ≈ 13）；255 占 %.2f%%\n", distinct,
        100.0 * hist[255] / (ao.rows * ao.cols));
    printNonContractValues(hist);
    printAoBuckets(hist);
    printAoScanline(ao);
}

/// 组 3：亚像素爬行（AO 是否锚在像素格上）。两个独立算子必须给同一结论。
void group3Crawl(const Ctx& ctx, const Collected& col, Results& res)
{
    const AbPair& ab = col.ab;
    const AbPair& crawl = col.crawl;
    const cv::Mat& flat = col.flat;
    const cv::Mat& allMask = col.allMask;
    const std::string& outDir = ctx.outDir;
    bool& crawlAgree = res.crawlAgree;
    bool& crawlScreenAnchored = res.crawlScreenAnchored;
    double& baseUnaligned = res.baseUnaligned;
    double& baseAligned = res.baseAligned;
    bool crawlAlignedWorse = false;

    std::printf("\n── 组 3  亚像素爬行（AO 是否锚在像素格上）─────────────────────\n");
    std::printf("  机位纯偏航（整数 %s px，深度无关，见 yawCam 注释）\n", d3(kShiftPx).c_str());
    // 整数平移 = 精确算子（无插值 → 不引入额外高频）。两假设可判别：
    //   H_世界（噪声长在世界表面）：B ≈ shift(A) ⟹ 对齐后差 ≈ 0
    //   H_屏幕（噪声长在像素格）  ：B ≈ A        ⟹ 对齐后差反而变大
    const int dx = static_cast<int>(kShiftPx);
    const auto gray = [](const cv::Mat& m) {
        cv::Mat g;
        cv::cvtColor(m, g, cv::COLOR_BGR2GRAY);
        return g;
    };
    const cv::Mat baseG = gray(ab.colorOff), crawlBaseG = gray(crawl.colorOff);
    const cv::Mat valid
        = shiftX(cv::Mat(col.attr.ids.size(), CV_8UC1, cv::Scalar(255)), dx);
    cv::Mat mFlat;
    cv::bitwise_and(flat, shiftX(flat, dx), mFlat);
    cv::bitwise_and(mFlat, valid, mFlat);

    // **算子的阳性锚点**：同一算子作用在底图（AO 关的终帧）上。底图是几何 + 视相关
    // 高光，几何**必然**跟着相机走 ⟹ 对齐后差必须显著下降。锚点必须在**有结构**的区域
    // 上求（契约区的底图本身零高频，锚在它上面是 0/0，什么也验不出），故用全图。
    // 平移符号也由锚点定（yawCam 的 Δpixel = −f_px·θ，符号约定容易记错；符号错则
    // 「对齐」是反向操作，整组结论反向）——用底图定符号是合法的：它的正确行为有闭式。
    baseUnaligned = rmsDiff(baseG, crawlBaseG, allMask);
    const double pos = rmsDiff(baseG, shiftX(crawlBaseG, dx), allMask);
    const double neg = rmsDiff(baseG, shiftX(crawlBaseG, -dx), allMask);
    const int sign = pos <= neg ? 1 : -1;
    baseAligned = std::min(pos, neg);
    std::printf("  算子阳性锚点（底图，全图）：未对齐 RMS %s → 对齐后 %s（符号 %+d px）→ %s\n",
        d3(baseUnaligned).c_str(), d3(baseAligned).c_str(), sign,
        baseAligned < baseUnaligned ? "符合预期（底图能被全局平移解释）"
                                    : "⚠ 锚点不成立，本组结论作废");

    // 判读方向（搞反就整组反向，故写死在注释里）：
    //   图案跟着几何走（世界锚定）⟹ B = shift(A) ⟹ **对齐后**差变小
    //   图案长在像素格上（屏幕锚定）⟹ B ≈ A      ⟹ **未对齐**差就小、对齐后反而变大
    const double aoUn = rmsDiff(ab.on.ao, crawl.on.ao, mFlat);
    const double aoAl = rmsDiff(ab.on.ao, shiftX(crawl.on.ao, sign * dx), mFlat);
    crawlAlignedWorse = aoAl > aoUn;  // true = 屏幕锚定
    const RegionStat un = statRegion(ab.on.ao, mFlat);
    std::printf("  AO 场（契约区）：未对齐差 RMS %s → 对齐后 %s（%.2f×）\n", d3(aoUn).c_str(),
        d3(aoAl).c_str(), aoUn > 0.0 ? aoAl / aoUn : 0.0);
    std::printf("    → %s\n", crawlAlignedWorse
            ? "对齐后**更大** = 图案没跟着几何走 = 长在像素格上（屏幕锚定）"
            : "对齐后**更小** = 图案跟着几何走（世界锚定）");

    // 集合版本（回答「偏离契约的是不是同一批像素」）：RMS 是幅度，Jaccard 是身份。
    cv::Mat devA, devB;
    cv::compare(ab.on.ao, cv::Scalar(254), devA, cv::CMP_LT);
    cv::compare(crawl.on.ao, cv::Scalar(254), devB, cv::CMP_LT);
    const double jScreen = jaccard(devA, devB, mFlat);
    const double jWorld = jaccard(devA, shiftX(devB, sign * dx), mFlat);
    std::printf("  偏离契约像素集合 Jaccard：同屏 %s  对齐后 %s → %s\n", d3(jScreen).c_str(),
        d3(jWorld).c_str(),
        jScreen >= jWorld ? "同一批屏幕像素在偏离（屏幕锚定）" : "同一批世界位置在偏离（世界锚定）");
    // 两个**独立**算子（幅度 RMS 与集合重合度）必须给同一个结论；不一致 = 判据本身失效，
    // 此时任何一侧的读数都不能写进报告。
    crawlAgree = (aoAl > aoUn) == (jScreen >= jWorld);
    crawlScreenAnchored = crawlAlignedWorse && jScreen >= jWorld;
    std::printf("  契约区抖动RMS（单帧内）%s LSB\n", d3(un.jitterRms).c_str());

    cv::Mat d, dAbs;
    cv::subtract(ab.on.ao, crawl.on.ao, d);
    cv::convertScaleAbs(d, dAbs, 8.0);
    cv::imwrite(outDir + "/g3_crawl_ao_x8.png", dAbs);
    cv::Mat db, dbAbs;
    cv::absdiff(ab.colorOff, crawl.colorOff, db);
    cv::convertScaleAbs(db, dbAbs, 8.0);
    cv::imwrite(outDir + "/g3_crawl_base_x8.png", dbAbs);
}

/// 组 5：掠射角对照（候选根因 B3/B4 的指纹）——与正视同一套契约区口径，只换机位。
void group5Grazing(const Ctx& ctx, const Collected& col, const Results& res)
{
    const AoCapture& grazeCap = col.grazeCap;
    const Attribution& at = col.at;
    const RegionStat& aoOnFlat = res.aoOnFlat;
    const std::string& outDir = ctx.outDir;

    std::printf("\n── 组 5  掠射角对照（候选根因 B3/B4 的指纹）───────────────────\n");
    const cv::Mat gEdges = idEdges(grazeCap.frame.ids);
    const cv::Mat gDist = distToEdge(gEdges);
    cv::Mat gFlat = cv::Mat::zeros(grazeCap.frame.ids.size(), CV_8UC1);
    for (const auto& [id, nm] : at.nameByOid)
        cv::bitwise_or(gFlat,
            flatRegion(grazeCap.frame.ids, gDist, id, static_cast<double>(ctx.radius)), gFlat);
    const RegionStat gStat = statRegion(grazeCap.ao, gFlat);
    std::printf("  %-24s px %8lld 均值 %-9s σ %-9s 最小 %-6.0f <255 占 %-8s 抖动RMS %s\n",
        "掠射 契约区", static_cast<long long>(gStat.pixels), d3(gStat.mean).c_str(),
        d3(gStat.sd).c_str(), gStat.lo, d3(gStat.fracBelowContract * 100.0).c_str(),
        d3(gStat.jitterRms).c_str());
    std::printf("  正视 → 掠射：σ %s → %s（%.2f×）；抖动RMS %s → %s（%.2f×）\n",
        d3(aoOnFlat.sd).c_str(), d3(gStat.sd).c_str(),
        aoOnFlat.sd > 0.0 ? gStat.sd / aoOnFlat.sd : 0.0, d3(aoOnFlat.jitterRms).c_str(),
        d3(gStat.jitterRms).c_str(),
        aoOnFlat.jitterRms > 0.0 ? gStat.jitterRms / aoOnFlat.jitterRms : 0.0);
    cv::Mat dev;
    cv::subtract(cv::Scalar(kAoContract), grazeCap.ao, dev);
    saveAmp(dev, gFlat, 16.0, outDir + "/g5_graze_dev_x16.png");
    cv::imwrite(outDir + "/g5_graze_occlusion.png", dev);
}

/**
 * 组 4：TAA 开时的时间行为（地板与信号在**同一进程**内取，都走 /shotburst）。
 *
 * 本组**只判量具**：地板非零（TAA jitter 自己每帧改画面）⟹ 比值不可解释，理由见文件头
 * 第二条被推翻的初版口径。AO 的时域结论走 TAA 关态的两条零锚点。
 */
void group4Temporal(const Ctx& ctx, const Collected& col, Results& res)
{
    int64_t& floorChanged = res.floorChanged;
    bool& temporalHasResolution = res.temporalHasResolution;

    std::printf("\n── 组 4  TAA 开（同进程 AO 关=地板 / AO 开=信号）──────────────\n");
    const std::string dir = profileOf(ctx, "taa", ctx.strength, true, 0.12);
    Engine eng(dir);
    // 抖动序列每 kJitterPeriod 帧一轮（`kJitterPeriod` = TAA jitter 周期）。两轮采集的
    // **帧数间隔取成周期的整数倍**，AO 关与 AO 开这两段 burst 就从**同一个抖动相位**起步
    // —— 否则「AO 开比 AO 关抖得多」可能只是相位不同，而不是 AO 的贡献。
    const uint32_t nBurst = static_cast<uint32_t>(
        ((ctx.pairs + 1 + kJitterPeriod - 1) / kJitterPeriod) * kJitterPeriod);
    const uint32_t kWarmFrames = 6 * static_cast<uint32_t>(kJitterPeriod);
    std::printf("  burst %u 帧（预热 %u 帧洗历史；两轮间隔 %u 帧 = 抖动周期的整数倍）\n", nBurst,
        kWarmFrames, kWarmFrames + nBurst);
    const auto off = burstDiff(eng, "off", 0.0, nBurst, kWarmFrames, col.flat, res.contactMask,
        ctx.outDir);
    const auto on = burstDiff(eng, "on", ctx.strength, nBurst, kWarmFrames, col.flat,
        res.contactMask, ctx.outDir);
    floorChanged = off.second;
    std::printf("\n");
    printTemporalRow("全图", off.first[0], on.first[0]);
    printTemporalRow("契约区（平坦）", off.first[1], on.first[1]);
    printTemporalRow("接触环（AO 生效）", off.first[2], on.first[2]);
    std::printf(
        "  ⚠ 地板非 0（TAA jitter 自己每帧都在改画面）⟹ **比值不可解释**（分母不是零，\n"
        "     也不是「AO 之外唯一变量」的对照，正是已登记反模式 screen-space-l1-frame-diff\n"
        "     的形态）。本组只作观察量；AO 的**时间**结论走 TAA 关那两条零锚点：\n"
        "     ① 静止机位下同参数连拍逐位相同（确定性锚①）⟹ 静止时无逐帧噪声；\n"
        "     ② 机位动 1 px 时图案不跟着几何走（组 3）⟹ 噪声是「爬行」而不是「闪烁」。\n");
    temporalHasResolution = off.first[1].mean > 0.0 && on.first[1].mean > 0.0
        && std::abs(on.first[1].mean / off.first[1].mean - 1.0) > 0.05;
}

/**
 * 组 6：交付场景（configs/ 原样，机位 = 交付 camera.lua）。
 *
 * 本组**不产出契约类判据**：bistro 有 395 张纹理，底图自身高频不是 0，「契约区」
 * 概念在这里不成立（见头注释）。唯一能带进真实场景的是**因果泄漏恒等式**——它
 * 不需要任何区域，只问「附件说没遮蔽的地方，画面动没动」。
 */
void group6Delivery(const Ctx& ctx, Results& res)
{
    double& bistroLeak = res.bistroLeak;
    int64_t& bistroLeakPx = res.bistroLeakPx;
    bool& bistroLeakOk = res.bistroLeakOk;
    const std::string& outDir = ctx.outDir;

    std::printf("\n── 组 6  交付场景（configs/ 原样，机位 = 交付 camera.lua）────────\n");
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 不重抄 ao/env/shadow（第二真相）
    const std::string dir = ctx.profRoot + "/imgqa_ao_bistro";
    makeProfile(dir, o);
    Engine eng(dir);
    const AbPair b = captureAb(eng, nullptr, ctx.strength, outDir, "g6", /*withId=*/false);
    const cv::Mat allB(b.on.ao.size(), CV_8UC1, cv::Scalar(255));
    cv::Mat active, contractB;
    cv::compare(b.on.ao, cv::Scalar(254), active, cv::CMP_LT);    // AO 生效处
    cv::compare(b.on.ao, cv::Scalar(254), contractB, cv::CMP_GT); // AO = 255 处
    const RegionStat allStat = statRegion(b.on.ao, allB, -1.0);
    const RegionStat actStat = statRegion(b.on.ao, active, -1.0);
    const ByteAB bAllB = byteAbsDiff(b.colorOff, b.on.frame.bgr, allB);
    const ByteAB bAct = byteAbsDiff(b.colorOff, b.on.frame.bgr, active);
    std::printf("  尺寸 %dx%d（交付 headless_size，非本工具的 %dx%d）\n", b.on.ao.cols,
        b.on.ao.rows, kAoSize[0], kAoSize[1]);
    std::printf("  AO 附件 全图  均值 %-9s σ %-9s 最小 %-6.0f  <255 占 %-9s 抖动RMS %s\n",
        d3(allStat.mean).c_str(), d3(allStat.sd).c_str(), allStat.lo,
        d3(active.empty() ? 0.0 : 100.0 * cv::countNonZero(active) / allB.total()).c_str(),
        d3(allStat.jitterRms).c_str());
    std::printf("  AO 附件 生效处 均值 %-9s σ %-9s 最小 %-6.0f  像素 %-9lld 抖动RMS %s\n",
        d3(actStat.mean).c_str(), d3(actStat.sd).c_str(), actStat.lo,
        static_cast<long long>(actStat.pixels), d3(actStat.jitterRms).c_str());
    printBistroRow("全图", bAllB);
    printBistroRow("AO 生效处", bAct);
    std::printf("  颗粒/渐变：全图 %s  生效处 %s   ← 真实场景里 AO 加的东西有多少是颗粒\n",
        bAllB.smoothRms > 0.0 ? d3(bAllB.grainRms / bAllB.smoothRms).c_str() : "inf",
        bAct.smoothRms > 0.0 ? d3(bAct.grainRms / bAct.smoothRms).c_str() : "inf");
    bistroLeak = maxByteDiff(b.colorOff, b.on.frame.bgr, contractB);
    bistroLeakPx = cv::countNonZero(contractB);
    bistroLeakOk = bistroLeak == 0.0;
    cv::Mat dev;
    cv::subtract(cv::Scalar(kAoContract), b.on.ao, dev);
    cv::imwrite(outDir + "/g6_bistro_ao_occlusion.png", dev);
    cv::Mat dAbs;
    cv::absdiff(b.colorOff, b.on.frame.bgr, dev);
    cv::convertScaleAbs(dev, dAbs, 8.0);
    cv::imwrite(outDir + "/g6_bistro_color_diff_x8.png", dAbs);
    keep(projectRoot() + "/build/rgl/screenshots/imgqa_g6_on_color.png", outDir,
        "g6_bistro_color_on.png");
    keep(projectRoot() + "/build/rgl/screenshots/imgqa_g6_coloroff.png", outDir,
        "g6_bistro_color_off.png");
}

}  // namespace RGL::imgqa
