// AO 噪声量测（Phase 13 观测任务）——「SSAO 到底噪不噪」的判据实现 + 四组测量执行。
//
// ## 锚点在哪（为什么不需要基线图像）
//
// 两个**恰为零**的锚点，都不需要参考图：
//
//   ① `ao.frag.slang:118` 的 strength≤0 早退是位级恒等 ⟹ AO 附件在 strength=0 时**逐位
//      255**（实测全图 σ=0.000）。于是同一算子作用在 strength=0.5 的附件上的高频读数，
//      它的零点不是估出来的，是恒等式给的。
//   ② AO 附件 = 255（附件自己说「这里没遮蔽」）的像素上，AO 开/关两张交付图必须**逐位
//      相同**（实测 876083 像素 max = 0 LSB）。这是**因果泄漏恒等式**：任何非零都说明
//      附件之外还有第二处东西在随强度变。它同时证明附件是交付差的唯一来源——交付 Δ 随
//      AO 偏离单调上升（AO≥245→max 3、≥235→5、≥225→8、≥215→10、≥195→16）。
//
// ## 两条实测推翻的初版口径（写在这里防后人重犯）
//
//   ✗ 「契约区（离轮廓 > 40 px）内的 (255 − AO) 就是噪声」。**不成立**：AO 半径 0.5 是
//     **世界**量，靠近相机的表面在轮廓内 40 px 处世界距离可远小于 0.5 m（实测 box_short
//     正面上离轮廓 >40 px 处 AO 低到 37，是合法遮蔽）。没有深度缓冲就画不出世界空间的
//     契约区。⇒ 偏离契约值降级为观察量；主判据改走**高频项**（medianBlur 残差），它对
//     合法的光滑遮蔽梯度不敏感，这才是区域定义不干净时仍然成立的量。
//   ✗ 「TAA 开 + 静止机位 + AO 关时逐帧差恰为 0（纯色平面无边缘 ⟹ jitter 不改颜色）」。
//     **不成立**：实测地板每帧 7.5 万像素在变（灰度均值 3.216/255）——deferred 的 GGX 高光
//     是视相关的，jitter 改视向量就改高光，与有没有边缘无关。⇒ 组 4 的 AO 开/关比值
//     （契约区 1.006×）**不可解释**，本组不判 AO；AO 的时域结论走 TAA 关态的两条零锚点。
//
// ## 为什么必须程序化靶
//
// `tools/qa_scenes.py` 的场景刻意不输出 TEXCOORD_0/TANGENT（缺 → uv 填 0 走白纹理），
// 每个 piece 是高饱和纯色 ⟹ **底图在 piece 内部零空间高频**（实测底图抖动 RMS 0.012）。
// 于是画面上出现的高频只可能来自 AO / 阴影 PCF / TAA，AO 噪声的隔离度远好于 bistro
// （395 张纹理把噪声埋在纹理里）。这条不是优化，是判据成立的前提：分母（底图自身高频）
// 在 bistro 上不是 0，比值不可解释。
//
// ## 四组测量
//
//   ① A/B 空间噪声（主判据）：**同进程** `POST /param` 翻 ao.strength 0 ↔ 0.5（跨进程
//      A/B 已证污染，见 `Engine::setParam` 注释）。契约区高频项 + 交付面字节颗粒项。
//   ② AO 场直方图 / 剖面：取值分布（12 样本会不会走成离散台阶）+ 穿过接触区的扫描线。
//   ③ 亚像素爬行：机位 yaw 整 1 像素。AO 的随机相位 = `hash12(SV_Position.xy)`（:134）
//      **只吃像素坐标** ⟹ 噪声贴在屏幕上。判据 = 两个独立算子（幅度 RMS 与偏离集合
//      Jaccard）必须给同一结论；算子自己的阳性锚点 = 同一算子作用在底图上必须「对齐后
//      变小」（几何必然跟着相机走）。
//   ④ 时域（TAA 开态）：`/shotburst` 连拍逐帧差，AO 0 vs 0.5 各一轮，两轮帧数间隔取抖动
//      周期的整数倍（同抖动相位起步）。**只判量具**（地板 > 0 = burst 真在交不同帧），
//      理由见上节第二条。
//
// 外加**掠射机位**一组：候选根因 B3（法线来自 ddx/ddy(P)）与 B4（核尺寸也来自导数）的
// 角度判别——导数重建的世界坐标在掠射/远处最差，噪声若随入射角掠射而暴涨即支持二者
// （实测 σ 5.14×、高频项 2.62×，支持 B3/B4；两者的进一步分离要改 shader，本任务红线
// 禁止，故只合并报告）。
//
// ## 反模式自查（为什么这条不是已登记的那些坑）
//
// - 不是 `ssim`/`psnr`：主测量是高频项对恒等式零点的偏离，区域来自 objectID 几何反解。
// - 不是 `global-epsilon`：容差理由见 judge.cpp 注册表（恒等式 + 量化步长，不是相似度）。
// - 不是 `screen-space-l1-frame-diff`：那条被推翻正是因为**分母非零的逐帧差比值**。本工具
//   的④实测就是这种形态（地板非零），所以它**不产出判据**，只判量具；时域结论走零锚点。
//
// ## 本命令拆成三个 TU + 一个内部头（缝的划法与理由 → `ao_noise_metric.h` 头注释）
//
//   · `ao_noise.cpp`（本文件）= 入口 + 采集 + 组 1/1b + 判定
//   · `ao_noise_metric.cpp`   = 口径层算子（帧差 / 逐位相同 / 取值分布 / 存图可视化）
//   · `ao_noise_groups.cpp`   = 组 2/3/4/5/6
//   · `ao_noise_metric.h`     = 跨 TU 的类型、阈值常量与声明（**唯一出处**）
#include "imgqa.h"

#include "ao_metrics.h"
#include "ao_noise_metric.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

namespace RGL::imgqa {
namespace {

// ───────────────────────────── 采集 ─────────────────────────────
//
// captureAoSet 只被本 TU 用（captureAb 与 collect 都在下面），故留在匿名 namespace；
// 而 keep / captureAb / profileOf / printRegionRow 要跨 TU（组 4 的 burst 归档、组 6 复用
// 同一台采集、组 5 复用同一套列宽），声明在 `ao_noise_metric.h`、定义在本 TU。

/**
 * 拍齐 color + object_id + ao 三张。
 *
 * ⚠ 三张是**三次 /shot = 三个不同帧**。TAA 关时这没问题（AO 无时间输入、着色链无跨帧
 * 状态 ⟹ 同机位逐帧逐位相同，`determinism` 判据就在钉这条）；TAA 开时**绝不能**用本函数
 * 做时域测量（那时必须 /shotburst，见 imgqa.h::shotBurst 注释）。
 */
AoCapture captureAoSet(Engine& eng, const Engine::Camera* cam, const std::string& tag,
    const std::string& outDir, const std::string& prefix, bool withId = true)
{
    AoCapture c;
    if (cam) {
        c.frame = captureFrame(eng, *cam, tag);
    } else {
        // **不动机位**（交付配置模式）：机位的真相在交付 camera.lua 里，工具再设一次就是
        // 第二份真相（`ProfileOpts::hasCamera` 的注释记着这条值一轮假绿）。
        c.frame.bgr = readPngBgr(eng.shot(tag + "_color"));
        // object_id 附件由 picking 门装饰，交付配置默认**关**着它（实测：请求不存在的附件
        // 会让引擎侧报「附件不存在」）。交付组不做区域归因，故连请求都不发。
        if (withId)
            c.frame.ids = decodeObjectId(readPngBgr(eng.shot(tag + "_objectid", "object_id")));
    }
    keep(projectRoot() + "/build/rgl/screenshots/imgqa_" + tag + "_color.png", outDir,
        prefix + "_color.png");
    if (withId) {
        keep(projectRoot() + "/build/rgl/screenshots/imgqa_" + tag + "_objectid.png", outDir,
            prefix + "_objectid.png");
    }

    const std::string aoPath = eng.shot(tag + "_ao", "ao");
    keep(aoPath, outDir, prefix + "_ao.png");
    const cv::Mat bgr = readPngBgr(aoPath);
    if (bgr.size() != c.frame.bgr.size())
        throw std::runtime_error("ao 附件与 color 尺寸不同（" + std::to_string(bgr.cols) + "x"
            + std::to_string(bgr.rows) + " vs " + std::to_string(c.frame.bgr.cols) + "x"
            + std::to_string(c.frame.bgr.rows) + "）—— 两图必然不同帧或 AO RT 尺寸另有出处");
    cv::extractChannel(bgr, c.ao, 2);  // BGR 序 → ch2 = R（convR8Unorm 三通道同值）
    return c;
}

}  // namespace

// 滑杆定位（口径与「写错不会静默」的理由 → `ao_noise_metric.h` 里这两个名字的注释）。
// 定义留在 .cpp 而非头里：文案对账（litdiff）只比 .cpp，字面量住头里就查不到它了。
const char* const kAoPage = "渲染";
const char* const kAoLabel = "AO 强度 ao.strength";

/// 把引擎产物收进本次测量目录（留证据；PNG 是判据的第一手数据，不是附属品）。
void keep(const std::string& src, const std::string& outDir, const std::string& name)
{
    std::error_code ec;
    fs::copy_file(src, fs::path(outDir) / name, fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("产物归档失败: " + src + " → " + name + "（" + ec.message() + "）");
}

/// 拍一对 A/B。`cam` 由本函数负责摆（调用方只保证 profile 已建好）。
AbPair captureAb(Engine& eng, const Engine::Camera* cam, double strength,
    const std::string& outDir, const std::string& prefix, bool withId)
{
    AbPair ab;
    eng.setParam(kAoPage, kAoLabel, strength);
    ab.on = captureAoSet(eng, cam, prefix + "_on", outDir, prefix + "_on", withId);
    // 确定性锚 ①：同参数连拍（帧间抖动 / 未初始化内存 / 异步竞态）。
    ab.repeatable = samePng(eng.shot(prefix + "_rep1"), eng.shot(prefix + "_rep2"));

    // 反向用例 + 终帧 A/B：强度切 0。
    eng.setParam(kAoPage, kAoLabel, 0.0);
    const std::string aoOffPath = eng.shot(prefix + "_aooff", "ao");
    keep(aoOffPath, outDir, prefix + "_ao_off.png");
    const cv::Mat aoOffBgr = readPngBgr(aoOffPath);
    cv::extractChannel(aoOffBgr, ab.aoOff, 2);
    ab.colorOff = readPngBgr(eng.shot(prefix + "_coloroff"));
    keep(projectRoot() + "/build/rgl/screenshots/imgqa_" + prefix + "_coloroff.png", outDir,
        prefix + "_color_off.png");

    // 确定性锚 ②：切回原强度，终帧必须与第一次**逐位**相同。这一条同时排除
    // 「/param 没生效」与「强度切换有残留状态」两种失效方式。
    eng.setParam(kAoPage, kAoLabel, strength);
    ab.reversible = samePng(eng.shot(prefix + "_onagain"),
        projectRoot() + "/build/rgl/screenshots/imgqa_" + prefix + "_on_color.png");
    return ab;
}

/// 建一个本工具的合成 profile 并返回目录（原 profileOf lambda：ao/env/skybox 的口径只有一份）。
std::string profileOf(const Ctx& ctx, const std::string& name, double aoStrength, bool taa,
    double feedback)
{
    ProfileOpts o;
    o.glb = ctx.glb;
    o.variant = ctx.variant;
    o.aoStrength = aoStrength;
    o.shadowPass = false;
    o.envIntensity = 0.0;
    o.skybox = false;
    o.exposure = 1.0;
    o.picking = true;
    o.aa = taa ? Aa::Taa : Aa::Off;
    o.taaFeedback = feedback;
    o.taaJitterScale = 1.0;
    o.size = {kAoSize[0], kAoSize[1]};
    const std::string dir = ctx.profRoot + "/imgqa_ao_" + name;
    makeProfile(dir, o);
    return dir;
}

// ─────────────────────── 分组执行（原 cmdAoNoise 的分段，一「组」一函数）───────────────────────
//
// 缝按「组」切：一个 `组` 一个函数，统一吃只读上下文 Ctx + 采集段产物 Collected，把该组
// 读数写进 Results。这么切不只是「函数太长」：这六段本来就各自独立，唯一的共享物就是
// 入参与采集结果，而跨组读的每个量都在 Results 里点了名——**不用文件级全局**，测量仪器
// 的状态一旦隐式化，跨组污染就查不出来了（本工具专门有一条「因果泄漏」判据）。
//
// 搬动纪律：段内代码**逐字**搬（打印顺序 = 验收的结构签名），只在函数头把「原来靠作用域
// 共享」的名字接成引用别名，于是段内不需要二次改写、也就没有二次改错的机会。

/// 一行 RegionStat 读数（组 1 的表与组 1b 的接触环共用同一套列宽）。
void printRegionRow(const char* name, const RegionStat& s)
{
    std::printf("  %-24s px %8lld 均值 %-9s σ %-9s 最小 %-6.0f 最大偏离 %-9s"
                " <255 占 %-8s 拉普拉斯RMS %-9s 抖动RMS %s\n",
        name, static_cast<long long>(s.pixels), d3(s.mean).c_str(), d3(s.sd).c_str(), s.lo,
        d3(kAoContract - s.lo).c_str(), d3(s.fracBelowContract * 100.0).c_str(),
        d3(s.lapRms).c_str(), d3(s.jitterRms).c_str());
}

namespace {

/**
 * 采集 + 区域归因 + 契约区掩码（原「══ 采集 ══」段）。
 *
 * 主进程一次跑完组 1/2/3/5 的全部帧：TAA 全关 ⟹ AO 无时间输入、同机位逐帧逐位相同，
 * 于是三组测量共用一个引擎进程（跨进程 = 跨环境，见 AbPair 注释）。
 */
void collect(const Ctx& ctx, Collected& col)
{
    AbPair& ab = col.ab;
    AbPair& crawl = col.crawl;
    AoCapture& grazeCap = col.grazeCap;
    Frame& attr = col.attr;
    Attribution& at = col.at;
    std::map<std::string, int32_t>& oidByName = col.oidByName;
    cv::Mat& dist = col.dist;
    cv::Mat& flat = col.flat;
    cv::Mat& bandLit = col.bandLit;
    cv::Mat& allMask = col.allMask;
    std::map<std::string, cv::Mat>& flatByPiece = col.flatByPiece;
    std::string& flatNames = col.flatNames;
    const Manifest& man = ctx.man;
    const Engine::Camera& cam = ctx.cam;
    const std::string& outDir = ctx.outDir;
    const double strength = ctx.strength;
    const int radius = ctx.radius;

    // 掠射机位：贴近地面平视，让地板/侧墙进入大掠射角。候选根因 B3（法线 = 重建世界坐标的
    // ddx/ddy 叉积）与 B4（核尺寸也来自导数）都预测「掠射处最差」。
    Engine::Camera graze;
    graze.position = {0.0, 0.6, 4.2};
    graze.target = {0.0, 0.35, -5.0};
    graze.fovDegrees = kAoFovDeg;

    {
        const std::string dir = profileOf(ctx, "main", strength, false, 0.12);
        Engine eng(dir);
        ab = captureAb(eng, &cam, strength, outDir, "g1");
        // 组 3 的第二帧：同进程、同强度、机位纯偏航整 1 像素。
        const Engine::Camera crawlCam = yawCam(cam, kShiftPx);
        crawl = captureAb(eng, &crawlCam, strength, outDir, "g3");
        grazeCap = captureAoSet(eng, &graze, "g5", outDir, "g5_graze");
    }

    // 区域归因在 **AO 关**那帧做（色度最干净），故把 colorOff + on 的 id 拼成一个 Frame。
    attr.bgr = ab.colorOff;
    attr.ids = ab.on.frame.ids;
    attr.camera = cam;
    prepareLinear(attr);
    at = attribute(attr, man);
    std::printf("可见 piece:");
    for (const auto& s : at.visiblePieces())
        std::printf(" %s", s.c_str());
    std::printf("\n");
    for (const auto& c : at.conflicts)
        std::printf("  ⚠ 反解冲突: %s\n", c.c_str());

    for (const auto& [id, nm] : at.nameByOid)
        oidByName[nm] = id;

    const cv::Mat edges = idEdges(attr.ids);
    dist = distToEdge(edges);
    std::ignore = cv::imwrite(outDir + "/g1_edges.png", edges);

    flat = cv::Mat::zeros(attr.ids.size(), CV_8UC1);
    cv::Mat band = cv::Mat::zeros(attr.ids.size(), CV_8UC1);
    for (const auto& name : at.visiblePieces()) {
        // oidOf 在这条循环里抛不了：name 与 oidByName 同出 nameByOid 一份来源
        //（债-96 把它改成认不出即抛；旧代码的 `oid == 0 continue` 兜的是「返回 0 = 背景」
        // 的旧契约，那个形态本身已不存在）。
        const int32_t oid = oidOf(col, name);
        const cv::Mat f = flatRegion(attr.ids, dist, oid, static_cast<double>(radius));
        cv::bitwise_or(flat, f, flat);
        cv::bitwise_or(band, bandOf(attr.ids, dist, oid, 2.0, static_cast<double>(radius)), band);
        if (cv::countNonZero(f) > 2000) {
            flatByPiece[name] = f;
            flatNames += (flatNames.empty() ? "" : ",") + name;
        }
    }
    if (cv::countNonZero(flat) < 2000)
        throw std::runtime_error("契约区像素不足（离边界 > " + std::to_string(radius)
            + "px）—— 换机位或调小 --radius");
    allMask = cv::Mat(attr.ids.size(), CV_8UC1, cv::Scalar(255));
    // 受光梯度带 = 带内且 AO 确有遮蔽（<255）处；「AO 有作用的地方」的载体。
    cv::Mat aoBelow;
    cv::compare(ab.on.ao, cv::Scalar(254), aoBelow, cv::CMP_LT);
    cv::bitwise_and(band, aoBelow, bandLit);
}

/// 组 1：AO 场本体（闭式锚点区）——契约区与逐 piece 的 AO 场统计（读数进 Results）。
void group1AoField(const Ctx& ctx, const Collected& col, Results& res)
{
    const AbPair& ab = col.ab;
    const cv::Mat& flat = col.flat;
    const std::string& flatNames = col.flatNames;
    RegionStat& aoOnFlat = res.aoOnFlat;
    RegionStat& aoOffFlat = res.aoOffFlat;
    RegionStat& aoOnAll = res.aoOnAll;
    RegionStat& baseFlat = res.baseFlat;

    aoOnFlat = statRegion(ab.on.ao, flat);
    aoOffFlat = statRegion(ab.aoOff, flat);
    aoOnAll = statRegion(ab.on.ao, col.allMask);
    cv::Mat baseGray;
    cv::cvtColor(ab.colorOff, baseGray, cv::COLOR_BGR2GRAY);  // statRegion 只读 ch0，先降灰度
    baseFlat = statRegion(baseGray, flat, -1.0);  // 底图自身高频 = 分母

    std::printf("\n── 组 1  闭式契约区（%s，离边界 > %dpx）──────────────────────\n",
        flatNames.c_str(), ctx.radius);
    printRegionRow("AO 附件 @ strength", aoOnFlat);
    printRegionRow("AO 附件 @ strength=0", aoOffFlat);
    printRegionRow("AO 附件（全图，观察）", aoOnAll);
    std::printf("  %-24s px %8lld 均值 %-9s σ %-9s 拉普拉斯RMS %-9s 抖动RMS %s"
                "   ← 分母（底图自身高频）\n",
        "底图 @ strength=0", static_cast<long long>(baseFlat.pixels), d3(baseFlat.mean).c_str(),
        d3(baseFlat.sd).c_str(), d3(baseFlat.lapRms).c_str(), d3(baseFlat.jitterRms).c_str());

    std::printf("\n  逐 piece 的契约区（噪声按表面朝向分布 = 机制指纹）\n");
    std::printf("  %-16s %10s %10s %10s %8s %11s %11s\n", "piece", "像素", "均值", "σ", "最小",
        "<255 占%", "抖动RMS");
    for (const auto& [name, m] : col.flatByPiece) {
        const RegionStat s = statRegion(ab.on.ao, m);
        std::printf("  %-16s %10lld %10s %10s %8.0f %11s %11s\n", name.c_str(),
            static_cast<long long>(s.pixels), d3(s.mean).c_str(), d3(s.sd).c_str(), s.lo,
            d3(s.fracBelowContract * 100.0).c_str(), d3(s.jitterRms).c_str());
    }
}

/// 组 1b 的一行：同一区域的线性域 Δ 与字节域 Δ（两行的列宽是表结构，别动）。
void printLinearRow(const char* name, const LinStat& l, const ByteAB& b)
{
    std::printf("  %-18s px %8lld 线性Δ 均值 %-11s RMS %-11s 最大 %-11s\n", name,
        static_cast<long long>(l.pixels), d5(l.mean).c_str(), d5(l.rms).c_str(),
        d5(l.maxAbs).c_str());
    std::printf("  %-18s             字节Δ 均值 %-11s p99 %-11s 最大 %-8s"
                " 渐变RMS %-9s 颗粒RMS %s\n",
        "", d3(b.mean).c_str(), d3(b.p99).c_str(), d3(b.mx).c_str(), d3(b.smoothRms).c_str(),
        d3(b.grainRms).c_str());
}

/**
 * 组 1b：A/B 落在**交付画面**上的量（线性域 + 字节域）+ 阳性锚点与因果泄漏的读数。
 *
 * 与组 1 分开的理由：组 1 读 AO 附件自己，本组读「附件 × 强度」作用到终帧之后的像素，
 * 两者的容差理由不同（附件有闭式零点，交付面还隔着一层 tonemap 量化）。
 */
void group1bDelivery(const Ctx& ctx, const Collected& col, Results& res)
{
    const AbPair& ab = col.ab;
    const Frame& attr = col.attr;
    const cv::Mat& dist = col.dist;
    const cv::Mat& flat = col.flat;
    const cv::Mat& bandLit = col.bandLit;
    const cv::Mat& allMask = col.allMask;
    const RegionStat& aoOnAll = res.aoOnAll;
    const std::string& outDir = ctx.outDir;
    const Engine::Camera& cam = ctx.cam;
    // 判据要跨块用的量（组 4 的分区掩码 + 判定段的读数）。
    cv::Mat& contactMask = res.contactMask;
    double& contactMin = res.contactMin;
    double& convexFracBelow = res.convexFracBelow;
    double& contactByteGrain = res.contactByteGrain;
    int64_t& contactPixels = res.contactPixels;
    double& leakMax = res.leakMax;
    int64_t& leakPixels = res.leakPixels;
    double& byteFlatGrain = res.byteFlatGrain;
    double& byteFlatP99 = res.byteFlatP99;
    double& byteFlatMax = res.byteFlatMax;

    Frame onFrame;
    onFrame.bgr = ab.on.frame.bgr;
    onFrame.ids = attr.ids;
    onFrame.camera = cam;
    prepareLinear(onFrame);
    cv::Mat delta(attr.ids.size(), CV_64F, cv::Scalar(0));
    cv::subtract(attr.luma, onFrame.luma, delta);  // off − on：AO 变暗故 ≥ 0

    const LinStat dFlat = statLinearDiff(delta, flat);
    const LinStat dBand = statLinearDiff(delta, bandLit);
    const LinStat dAll = statLinearDiff(delta, allMask);
    const ByteAB bFlat = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, flat);
    const ByteAB bBand = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, bandLit);
    const ByteAB bAll = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, allMask);
    std::printf("\n── 组 1b 交付画面 A/B 差（off vs on，**同进程**）───────────────\n");
    printLinearRow("契约区（闭式=0）", dFlat, bFlat);
    printLinearRow("受光梯度带", dBand, bBand);
    printLinearRow("全图（观察）", dAll, bAll);
    std::printf("  颗粒/渐变（字节域）：受光带 %s  全图 %s"
                "   ← AO 加到画面上的东西里有多少是颗粒而不是形状\n",
        bBand.smoothRms > 0.0 ? d3(bBand.grainRms / bBand.smoothRms).c_str() : "inf",
        bAll.smoothRms > 0.0 ? d3(bAll.grainRms / bAll.smoothRms).c_str() : "inf");

    // 阳性锚点：**凹角接触环**（地面贴着方块/球的那些像素）。闭式说这里必须有遮蔽——
    // 方块脚与地面是凹角、间距 0，AO 半径 0.5 m 内全是遮挡候选。
    // ⚠ 首跑用「一切轮廓附近的带」当锚点，测出 8.4% 变暗，看着像「AO 没跑」；实际带里
    // 绝大多数像素是**凸边**（方块顶沿、球轮廓），那里 AO 恒 1 才是对的。锚点必须钉在
    // 闭式说「这里必须有遮蔽」的地方，否则阳性锚点自己就是假阴的来源。
    contactMask = contactRing(attr.ids, oidOf(col, "floor"),
        {oidOf(col, "box_tall"), oidOf(col, "box_short"), oidOf(col, "sphere")}, 24);
    const cv::Mat& contact = contactMask;
    const RegionStat contactStat = statRegion(ab.on.ao, contact);
    const ByteAB bContact = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, contact);
    // **因果泄漏判据（本工具最硬的一条，且不需要任何区域）**：AO 附件说「这里没遮蔽」
    // （= 255）的地方，AO 开/关两张交付图必须**逐位相同**。它是恒等式而不是相似度——
    // 任何非零都意味着「AO 附件之外还有第二处东西在随强度变」（读错附件 / 强度进了别的
    // shader / 两张图不是同一帧）。另：Δ 随 AO 偏离单调上升（实测 AO≥245 → max 3、
    // ≥235 → 5、≥225 → 8、≥215 → 10、≥195 → 16），故附件是交付差的唯一来源。
    cv::Mat aoContractPx;
    cv::compare(ab.on.ao, cv::Scalar(254), aoContractPx, cv::CMP_GT);  // > 254 ⟺ == 255
    leakMax = maxByteDiff(ab.colorOff, ab.on.frame.bgr, aoContractPx);
    leakPixels = cv::countNonZero(aoContractPx);
    cv::Mat darkPos, darkM, darkContact;
    cv::max(delta, cv::Scalar(0), darkPos);
    cv::compare(darkPos, cv::Scalar(1e-4), darkM, cv::CMP_GT);
    cv::bitwise_and(darkM, contact, darkContact);
    const double contactDark = static_cast<double>(cv::countNonZero(darkContact))
        / std::max(1.0, static_cast<double>(cv::countNonZero(contact)));
    std::printf("  阳性锚点（凹角接触环 floor↔box/sphere，膨胀 24px）\n");
    printRegionRow("    AO 附件", contactStat);
    std::printf("      终帧字节Δ 均值 %s p99 %s 最大 %s 颗粒RMS %s；变暗像素 %s%%\n",
        d3(bContact.mean).c_str(), d3(bContact.p99).c_str(), d3(bContact.mx).c_str(),
        d3(bContact.grainRms).c_str(), d3(contactDark * 100.0).c_str());
    std::printf("      因果泄漏（AO 附件 = 255 的像素上终帧 A/B 差）：max %s LSB，像素 %lld"
                "   ← 恒等式，必须恰为 0\n", d3(leakMax).c_str(),
        static_cast<long long>(leakPixels));
    // 反向对照：**凸边**带（球自己的轮廓带）——那里 AO 恒 1 才正确。它证明「变暗」是
    // 「凹角接触」的因果，而不是「靠近轮廓」的伪相关。
    const cv::Mat convex
        = bandOf(attr.ids, dist, oidOf(col, "sphere"), 2.0, static_cast<double>(ctx.radius));
    const RegionStat convexStat = statRegion(ab.on.ao, convex);
    std::printf("  反向对照（球轮廓带，凸边 → AO 应恒 255）：均值 %s  <255 占 %s%%\n",
        d3(convexStat.mean).c_str(), d3(convexStat.fracBelowContract * 100.0).c_str());
    saveMaskOverlay(ab.on.frame.bgr, contact, outDir + "/g1_mask_contact.png");

    cv::Mat dev;
    cv::subtract(cv::Scalar(kAoContract), ab.on.ao, dev);  // 255 − AO = 遮蔽量
    cv::imwrite(outDir + "/g1_ao_occlusion.png", dev);
    saveAmp(dev, flat, 16.0, outDir + "/g1_flat_dev_x16.png");
    saveAmp(dev, bandLit, 4.0, outDir + "/g1_band_dev_x4.png");
    saveStretched(ab.on.ao, aoOnAll.lo, 255.0, outDir + "/g1_ao_stretched.png");
    cv::Mat dAbs;
    cv::convertScaleAbs(delta, dAbs, 255.0 * 8.0);
    cv::imwrite(outDir + "/g1_color_diff_x8.png", dAbs);

    contactMin = contactStat.lo;
    convexFracBelow = convexStat.fracBelowContract;
    contactByteGrain = bContact.grainRms;
    contactPixels = contactStat.pixels;
    byteFlatGrain = bFlat.grainRms;
    byteFlatP99 = bFlat.p99;
    byteFlatMax = bFlat.mx;
}

/**
 * 可选组的判据（组 6 / 组 4 **跑过**才进表）。
 *
 * 单独成函数的理由不是行数：这两条判据的**存在性**取决于跑没跑那一组——没跑就没有读数，
 * 报 PASS 或 FAIL 都是假的。`checks` 与判定段是同一个向量（追加，不另起一张表）。
 */
void addOptionalChecks(const Ctx& ctx, const Results& res,
    std::vector<std::tuple<std::string, bool, std::string>>& checks)
{
    if (ctx.doBistro) {
        // 唯一能带进真实场景的判据：它不问「这块该多亮」（bistro 没有闭式），只问恒等式。
        checks.emplace_back("因果泄漏（交付场景）：AO 附件 = 255 处交付图逐位未变",
            res.bistroLeakOk,
            "AO=255 的 " + std::to_string(res.bistroLeakPx) + " 像素上 A/B 差 max "
                + d3(res.bistroLeak) + " LSB（要求恰为 0）");
    }
    if (ctx.doTemporal) {
        // 阳性锚点：burst 必须真的在交**不同**的帧。若引擎把同一张文件交两次，逐帧差全 0，
        // 「AO 没有时间噪声」会假绿 —— 故这条钉的是量具本身，不是 AO。
        checks.emplace_back("时域量具阳性锚点：burst 逐帧确实在变（地板 > 0）",
            res.floorChanged > 0,
            "地板非零像素合计 " + std::to_string(res.floorChanged)
                + "（= 0 说明 burst 交的是同一帧，本组任何读数都是假的）");
        std::printf("  [观察] TAA 开态下 AO 的时间贡献：契约区逐帧差%s（地板 = TAA jitter 自身，"
                    "非零）\n",
            res.temporalHasResolution ? "与地板有 >5% 的差（AO 的时间贡献可见）"
                                      : "与地板无 >5% 的差（AO 的时间贡献淹在 jitter 里，本量具测不出）");
    }
}

/**
 * 判定段：把各组读数汇成 checks 表并打印（observed / whyTol 文案 = 判据台账的引用对象）。
 *
 * 返回退出码（0 = 全 PASS）。容差理由逐条留在 add(...) 的实参里，别往别处搬。
 */
int printVerdict(const Ctx& ctx, const Collected& col, const Results& res)
{
    const AbPair& ab = col.ab;
    const RegionStat& aoOnFlat = res.aoOnFlat;
    const RegionStat& aoOffFlat = res.aoOffFlat;
    const RegionStat& baseFlat = res.baseFlat;
    const std::string& outDir = ctx.outDir;

    std::printf("\n══════════════════════════ 判定 ══════════════════════════\n");
    std::vector<std::tuple<std::string, bool, std::string>> checks;
    const auto add = [&](const char* name, bool pass, const std::string& detail) {
        checks.emplace_back(name, pass, detail);
    };

    add("确定性锚①：同参数连拍逐位相同", ab.repeatable,
        ab.repeatable ? "逐位相同" : "同参数两帧不同 —— 帧间有未定来源");
    add("确定性锚②：强度切回后逐位复原（兼排除 /param 未生效）", ab.reversible,
        ab.reversible ? "切回后逐位相同" : "切回后不同 —— /param 未生效或有残留状态");
    add("反向用例：strength=0 时 AO 附件逐位 255", aoOffFlat.lo >= kAoContract,
        "契约区最小 " + std::to_string(static_cast<int>(aoOffFlat.lo)) + "（要求 255）");
    // 阳性锚点的口径是「凹角处**存在**闭式要求的深遮蔽」+「凸边处**没有**遮蔽」，
    // 不是「接触环平均被压暗」——环有 24 px 宽，绝大多数像素按闭式就该是 1，
    // 拿均值当锚点会把「锚点太宽」误读成「AO 没跑」（首跑正是这样假阴的）。
    add("阳性锚点：凹角接触环存在深遮蔽（闭式说这里必须 <1）",
        res.contactPixels > 500 && res.contactMin < 128.0,
        "接触环最小 AO " + d3(res.contactMin)
            + "（要求 < 128 = 半径 0.5 内贴地凹角必须明显遮蔽）、像素 "
            + std::to_string(res.contactPixels));
    add("反向对照：凸边（球轮廓带）不被遮蔽", res.convexFracBelow < 0.02,
        "球轮廓带 <255 占 " + d3(res.convexFracBelow * 100.0) + "%（要求 < 2%）——证明「变暗」是"
        "凹角接触的因果，不是「靠近轮廓」的伪相关");
    add("因果泄漏：AO 附件 = 255 处交付图逐位未变（恒等式）", res.leakMax == 0.0,
        "AO=255 的 " + std::to_string(res.leakPixels) + " 像素上 A/B 差 max " + d3(res.leakMax)
            + " LSB（要求恰为 0）；非 0 = 附件之外还有第二处随强度变的东西");
    // 「离轮廓 > radius 像素」**不是**世界空间的契约区：AO 半径 0.5 是世界量，靠近相机的表面
    // 在轮廓内 40 px 处世界距离可以远小于 0.5 m（实测 box_short 正面上离轮廓 > 40 px 处 AO 低到
    // 37，那是合法遮蔽不是噪声）。故偏离契约值只作**观察量**，不当判据；噪声判据走高频项
    // （高频项对合法的光滑遮蔽梯度不敏感，这才是区域定义不干净时仍然成立的量）。
    std::printf("\n  观察量：契约区偏离契约值 <255 占 %s%%、AO 最大偏离 %s LSB、交付字节Δ 最大 %s LSB"
                "\n         —— 两者同源污染（近处表面在轮廓内被合法遮蔽），均不作判据\n",
        d3(aoOnFlat.fracBelowContract * 100.0).c_str(), d3(kAoContract - aoOnFlat.lo).c_str(),
        d3(res.byteFlatMax).c_str());
    add("主判据：契约区 AO 场逐像素抖动 RMS（字节域）", aoOnFlat.jitterRms <= 1.0,
        "抖动RMS " + d3(aoOnFlat.jitterRms) + " LSB（要求 ≤ 1.0）；底图同算子 "
            + d3(baseFlat.jitterRms) + " 是分母");
    add("交付画面：契约区 A/B 差的颗粒项（字节域，AO≈契约值处）",
        res.byteFlatGrain <= 1.0, "颗粒RMS " + d3(res.byteFlatGrain)
            + " LSB（要求 ≤ 1.0 = 量化步长，低于它噪声被 8bit 量化吞掉）、p99 "
            + d3(res.byteFlatP99));
    add("交付画面：接触环（AO 真正起作用处）A/B 差的颗粒项", res.contactByteGrain <= 2.0,
        "颗粒RMS " + d3(res.contactByteGrain) + " LSB（要求 ≤ 2.0；此处含接触遮蔽的**台阶**残差，"
        "medianBlur 减不掉阶跃，故阈值放宽到 2×量化步长，且只作下限参考）");
    add("爬行：算子阳性锚点成立（底图能被全局平移解释）", res.baseAligned < res.baseUnaligned,
        "底图未对齐 RMS " + d3(res.baseUnaligned) + " → 对齐后 " + d3(res.baseAligned)
            + "；不成立则爬行组结论作废");
    add("爬行：幅度算子与集合算子结论一致（不一致 = 判据失效）", res.crawlAgree,
        res.crawlScreenAnchored
            ? "两个算子一致：**屏幕锚定**（噪声长在像素格上 ⟹ 相机一动就爬行）"
            : "两个算子一致：世界锚定（噪声跟着表面走，相机一动图案一起动）");
    addOptionalChecks(ctx, res, checks);

    int failed = 0;
    for (const auto& [name, pass, detail] : checks) {
        std::printf("  [%s] %-56s %s\n", pass ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        if (!pass)
            ++failed;
    }
    std::error_code ec;
    int nFiles = 0;
    for (const auto& e : fs::directory_iterator(outDir, ec)) {
        static_cast<void>(e);
        ++nFiles;
    }
    std::printf("\n产物目录：%s（%d 个文件）\n", outDir.c_str(), nFiles);
    return failed == 0 ? 0 : 1;
}

}  // namespace

/**
 * ao-noise 的骨架：入参 → 建目录清陈旧产物 → 打表头 → 采集 → 逐组 → 判定。
 *
 * 六组测量各自成函数（见上方「分组执行」的缝说明），本函数只保留**必须只执行一次**的
 * 事：清空 outDir。陈旧产物会伪装成「本轮测的」（izb 首跑实锤过），所以它既不能跟着某个
 * 组的函数被搬走，也不能被调两次。调用顺序 = 原实现 = 打印顺序（验收比的是结构签名）。
 */
int cmdAoNoise(const std::string& glb, const std::string& camera, const std::string& variant,
    double strength, int radius, int pairs, bool doTemporal, bool doBistro)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto camIt = man.cameras.find(camera);
    if (camIt == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    Ctx ctx;
    ctx.glb = glb;
    ctx.variant = variant;
    ctx.camera = camera;
    ctx.man = man;
    ctx.cam = camIt->second;
    ctx.outDir = projectRoot() + "/build/ao_noise";
    ctx.profRoot = projectRoot() + "/build";
    ctx.strength = strength;
    ctx.radius = radius;
    ctx.pairs = pairs;
    ctx.doTemporal = doTemporal;
    ctx.doBistro = doBistro;

    std::error_code ec;
    fs::create_directories(ctx.outDir, ec);
    for (const auto& e : fs::directory_iterator(ctx.outDir, ec))
        if (!ec && e.is_regular_file(ec))
            fs::remove(e.path(), ec);  // 陈旧产物会伪装成「本轮测的」（izb 首跑实锤过）

    std::printf("AO 噪声量测（同进程 A/B）  靶=%s  机位=%s  variant=%s  strength=%.3f  契约区排除半径=%d px\n",
        ctx.glb.c_str(), ctx.camera.c_str(),
        ctx.variant.empty() ? "forward" : ctx.variant.c_str(), ctx.strength, ctx.radius);
    std::printf("尺寸 %dx%d  光照 env=0 ambient=0 shadow=off（底图零高频 = 判据前提）\n", kAoSize[0],
        kAoSize[1]);
    std::printf("产物目录 %s\n\n", ctx.outDir.c_str());

    Collected col;
    collect(ctx, col);

    Results res;
    group1AoField(ctx, col, res);
    group1bDelivery(ctx, col, res);
    group2ValueDistribution(col);
    group3Crawl(ctx, col, res);
    group5Grazing(ctx, col, res);
    if (ctx.doTemporal)
        group4Temporal(ctx, col, res);
    if (ctx.doBistro)
        group6Delivery(ctx, res);
    return printVerdict(ctx, col, res);
}

}  // namespace RGL::imgqa
