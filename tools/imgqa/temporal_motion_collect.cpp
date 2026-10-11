// temporal-motion 家族**采集层** —— `collectGroup` = 「一组 = 一个引擎进程」：写配置 →
// 起引擎 → 热身 → /shotburst 逐帧采集 → 红线自证 → 可选 /param A/B 探针。
// 拆出来是因为 `temporal_motion.cpp` 2748 代码行 > 500（批次 B11），缝按**仪器层次**切：
// 这一层只碰引擎与磁盘，不做任何判据算术（算子在 `temporal_motion_metric.cpp`）。
// 六台仪器（motion / feedback / tail / smear / camera / step）共用这一条采集链。
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

/**
 * 采集规格 → 引擎配置（`collectGroup` 的配置段，逐字搬出）。
 *
 * ⚠ 逐键的「默认 = 一个字节都不多写」纪律全在这里，改任何一行的条件都等于改已发表读数的
 * 口径（clip 两键以 `o.aa == Taa` 为条件那条尤甚）。
 */
[[nodiscard]] ProfileOpts collectProfileOpts(const BistroCam& cam, const Spec& sp)
{
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 红线 3
    o.hasCamera = true;                                // 机位第 1 帧生效（RPC 只幂等重发）
    o.cameraPosition = cam.position;
    o.cameraTarget = cam.target;
    o.cameraFov = cam.fov;
    o.aa = sp.aa;
    o.taaFeedback = sp.feedback;
    o.taaJitterScale = sp.jitterScale;
    o.taaBaseSpace = sp.baseSpace;
    o.taaMotionCompensation = sp.motionCompensation;
    o.taaReprojection = sp.reprojection;
    o.taaDiagReject = sp.diagReject;
    o.taaInputKernel = sp.kernel;  // 批 13.8 输入侧重建核（0 = 不写键 = 已发表口径）
    // 批 13.11 鬼影 × 裁剪消融（默认全关 = 不写键 = 已发表 M0…M4 读数逐位不变）。
    // ⚠ clip 两键以 `o.aa == Taa` 为条件：TAA 关的组（M0/REFm/REFs）没有历史，写这些键是空操作，
    //   但会让「参考组 = 交付配置原样」这条自证失效 ⟹ 一个字节都不多写。
    //   （长窗档不再有独立键：批 13.14b 删 static boost 后它 = `sp.feedback` 本身。）
    o.taaClipMode = o.aa == Aa::Taa ? sp.clipMode : 0.0;
    o.taaClipSoft = sp.clipSoft;
    // 升频 / FSR 臂（默认全关 = 不写键 = 现有 M0…M4 等读数逐位不变）。
    // 批 14.6：velocity 不在这里写——`aa == Fsr` 时由引擎解析期隐含打开（单一来源）。
    o.renderScale = sp.renderScale;
    o.fsrTemporal = sp.fsrTemporal;
    o.fsrJitter = sp.fsrJitter;
    if (sp.cameraMotion)
        o.cameraFrameYaw = sp.motionAngle;  // 默认运动源 = **相机**绕 up 轴定角 yaw
    else {
        o.spinFrameAngle = sp.motionAngle;
        o.spinAxis = {0.0, 1.0, 0.0};  // 绕 up 轴 yaw（刚体；默认 (0.5,1,0) 不是纯 yaw）
    }
    if (sp.refScale) {
        o.size = {kRefSize[0], kRefSize[1]};
        o.overrideHeadlessSize = true;
    }
    return o;
}

/// 亮度轴尺子的收尾（`collectGroup` 的 `sp.wantBytes` 段，逐字搬出以降认知复杂度）。
void finalizeGroupBytes(Group& g, const Spec& sp, cv::Mat& byteSum, const cv::Mat& byteMax,
    int byteFrames)
{
    // 亮度轴尺子必须与偏差图**同网格**才能逐像素分带 ⟹ 参考组（1440p）的字节均值按
    // INTER_AREA 降到 720p。⚠ 这是「字节域的面积平均」，不是「降采样后重走显示链」；
    // 档位只有三档（T_lo=128 / T_hi=240 的粗分），亚像素口径差别不改变归属，如实记录。
    byteSum /= static_cast<double>(byteFrames);
    cv::Mat mean, mx;
    if (sp.refScale) {
        cv::resize(byteSum, mean, cv::Size(kSize[0], kSize[1]), 0, 0, cv::INTER_AREA);
        cv::resize(byteMax, mx, cv::Size(kSize[0], kSize[1]), 0, 0, cv::INTER_AREA);
    }
    else {
        mean = byteSum;
        mx = byteMax;
    }
    g.byteMean = mean;
    g.byteMax = mx;
    // ⚠ **第三把亮度轴**（doorframe 实测「近削顶档 = 0 像素」逼出来的敏感性口径）：
    // INTER_AREA 是面积平均 ⟹ 1px 高亮细线被邻居稀释，恰好把假设的元凶从尺子上抹掉。
    // 峰值轴回答「这个统计像素的邻域里**曾经**有多亮」，与主表（均值轴）的差 = 降采样
    // 本身看不见多细的线。只作敏感性，不换主表。
    g.bytePeak = maxPoolTo(byteMax, cv::Size(kSize[0], kSize[1]));
}

/// 附件捕获帧（BGRA）拆成「alpha 掩膜入 `g.alpha` + BGR 返回」；非 4 通道原样返回
/// （`collectGroup` 采集内层，逐字搬出以降认知复杂度）。
[[nodiscard]] cv::Mat groupFrameChannels(const cv::Mat& img, Group& g)
{
    if (img.channels() != 4)
        return img;
    // 附件捕获（taa_out）= BGRA：alpha 是拒收码，BGR 另作他用
    cv::Mat ch[4];
    cv::split(img, ch);
    g.alpha.push_back(ch[3].clone());
    std::vector<cv::Mat> bgrCh = {ch[0], ch[1], ch[2]};
    cv::Mat bgr;
    cv::merge(bgrCh, bgr);
    return bgr;
}

/// 字节亮度轴累加（`collectGroup` 的 `sp.wantBytes` 内层，逐字搬出以降认知复杂度）。
void accumulateByteLuma(const cv::Mat& bgr, cv::Mat& byteSum, cv::Mat& byteMax, int& byteFrames)
{
    // 亮度轴尺子 = **显示域字节**亮度（tonemap+sRGB 之后取 Rec.601 灰）的跨帧均值/最大。
    // 为什么不是线性域：这一轴要的是「交付面上这块**看起来**是不是高光区」，而截图里没有
    // 线性 HDR（ACES 已把 ≫1 压进 8bit）；「削顶」本来就是字节域的事件。口径与
    // `temporal_residual.cpp` 的 bistro 分带逐字相同（同一算子只许一份实现）。
    cv::Mat gray;
    cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
    cv::Mat gd;
    gray.convertTo(gd, CV_64FC1);
    if (byteSum.empty()) {
        byteSum = gd.clone();
        byteMax = gd.clone();
    }
    else {
        cv::add(byteSum, gd, byteSum);
        cv::max(byteMax, gd, byteMax);
    }
    ++byteFrames;
}

/// 逐帧读图 → `g.luma` / `g.alpha` / 字节亮度轴累加（`collectGroup` 的采集内层，逐字搬出）。
/// 打印顺序与 RPC 顺序都不经过这里，但 `g.luma` 的**推入顺序** = 帧序 = 后续对齐的下标语义。
void accumulateGroupFrames(Group& g, const std::string& name, const Spec& sp,
    const BistroCam& cam, const std::vector<std::string>& files)
{
    cv::Mat prev;
    cv::Mat byteSum, byteMax;  ///< `sp.wantBytes` 的累加器（**原生分辨率**，收尾才降网格）
    int byteFrames = 0;
    for (size_t i = 0; i < files.size(); ++i) {
        cv::Mat img = cv::imread(files[i], cv::IMREAD_UNCHANGED);
        if (img.empty())
            throw std::runtime_error(name + "：读不到 " + files[i]);
        const cv::Mat bgr = groupFrameChannels(img, g);
        g.rawSize = bgr.size();
        if (i > 0) {  // 字节域自证（激励在场判据读的是「画面到底动没动」，不是线性域小数值）
            cv::Mat d;
            cv::absdiff(prev, bgr, d);
            g.byteMaxAdjacent = std::max(g.byteMaxAdjacent, cv::norm(d, cv::NORM_INF));
        }
        prev = bgr.clone();
        if (sp.wantBytes)
            accumulateByteLuma(bgr, byteSum, byteMax, byteFrames);
        if (!sp.attachment.empty())
            continue;  // 掩膜组：RGB 是 tonemap 前的 HDR 数据，不进偏差统计
        Frame f;
        f.camera = {cam.position, cam.target, cam.fov};
        f.bgr = bgr;
        prepareLinear(f);  // 红线 1：复用，不另写解码
        cv::Mat luma = f.luma.clone();
        if (sp.refScale) {
            // 线性域 2×2 面积平均（luma 与 INTER_AREA 可交换，依据见文件头红线 1）。
            cv::resize(luma, luma, cv::Size(kSize[0], kSize[1]), 0, 0, cv::INTER_AREA);
        }
        g.luma.push_back(std::move(luma));
    }
    if (sp.wantBytes && byteFrames > 0)
        finalizeGroupBytes(g, sp, byteSum, byteMax, byteFrames);
}

/**
 * 红线 2 的自证（`collectGroup` 的校验 + 回显段，逐字搬出）：引擎**实际**渲染尺寸必须等于
 * 期望尺寸（否则「参考 = 超采样」这条根本不在自证范围内），统计网格恒 1280×720。
 * ⚠ 回显打印在抛错**之前**——首轮实测就是靠这行回显发现尺寸错的，顺序不许换。
 */
void validateGroupFrames(Group& g, const std::string& name, const Spec& sp)
{
    const bool empty = sp.attachment.empty() ? g.luma.empty() : g.alpha.empty();
    if (empty)
        throw std::runtime_error(name + "：/shotburst 一张都没拿到");
    g.frameSize = sp.attachment.empty() ? g.luma.front().size() : g.rawSize;
    const cv::Size wantRaw = sp.refScale ? cv::Size(kRefSize[0], kRefSize[1])
                                         : cv::Size(kSize[0], kSize[1]);
    if (!sp.attachment.empty()) {
        std::printf("  [%s] 渲染 %dx%d → 附件 %s alpha 掩膜（%zu 张，红线 2 自证）｜帧计数 %u → %u"
                    "（Δ=%u，热身 %d）｜引擎回显 feedback=%s\n",
            name.c_str(), g.rawSize.width, g.rawSize.height, sp.attachment.c_str(),
            g.alpha.size(), g.framePre, g.framePost, g.framePost - g.framePre, sp.warm,
            d4(g.liveFeedback).c_str());
    }
    else {
        std::printf("  [%s] 渲染 %dx%d → 统计 %dx%d（%zu 张，红线 2 自证）｜帧计数 %u → %u"
                    "（Δ=%u，热身 %d）｜引擎回显 feedback=%s\n",
            name.c_str(), g.rawSize.width, g.rawSize.height, g.frameSize.width,
            g.frameSize.height, g.luma.size(), g.framePre, g.framePost,
            g.framePost - g.framePre, sp.warm, d4(g.liveFeedback).c_str());
    }
    if (g.rawSize != wantRaw)
        throw std::runtime_error(name + "：引擎实际渲染 " + std::to_string(g.rawSize.width) + "x"
            + std::to_string(g.rawSize.height) + " ≠ 期望 " + std::to_string(wantRaw.width) + "x"
            + std::to_string(wantRaw.height) + "——headless_size 覆写没生效，参考不是超采样");
    if (sp.attachment.empty()) {
        const cv::Size want(kSize[0], kSize[1]);  // 参考组已在采集内降采样回同一网格
        if (g.frameSize != want)
            throw std::runtime_error(name + "：帧尺寸 " + std::to_string(g.frameSize.width) + "x"
                + std::to_string(g.frameSize.height) + " ≠ " + std::to_string(want.width) + "x"
                + std::to_string(want.height) + "——偏差口径按 " + std::to_string(kSize[0]) + "x"
                + std::to_string(kSize[1]) + " 钉，换分辨率即换口径");
    }
}

/// 同进程 `/param` A/B 探针（`collectGroup` 的探针段，逐字搬出）。⚠ 三次 RPC 的**顺序**就是
/// 判据本身：写滑杆 → 隔整周期 → 再拍，换成「先拍后写」量到的是另一个东西。
void runParamProbe(Group& g, Engine& eng, const std::string& name, const std::string& tag,
    const Spec& sp)
{
    // 红线 6 的**行为侧**证据（同进程，两张图之间只隔一个 float）。
    // ⚠ 为什么必须「**静止机位** + 隔**整一个抖动周期**」：
    //  · 相机在动 ⟹ 相邻两帧的画面本来就不同（每帧 ≈5px 位移），「A≠B」恒真 = 零信息；
    //  · 抖动相位逐帧变 ⟹ 同上。而 jitter 相位 = `frameIndex % 16`，静止机位下隔 16 帧
    //    = **同一相位 + 同一视点** ⟹ 输入光栅相同，A/B 之差只可能来自 feedback。
    //  · 反向也成立：滑杆若是死字段，两帧的历史与输入全都相同 ⟹ **逐位相同**。
    // 跨进程比不了：两次 burst 的起点帧差 1–3 帧（RPC 机位写入重定基），画面本来就不同。
    if (sp.motionAngle != 0.0 || sp.jitterScale <= 0.0)
        throw std::runtime_error(name
            + "：/param 探针只许跑在**静止机位 + jitter_scale>0** 的组上（否则 A/B 之差"
              "来自画面本身在动，判据恒真 = 没有信息）");
    const std::string pa = eng.shot(tag + "_pA");
    eng.setParam("", kTaaFeedbackLabel, sp.probeFeedback);
    eng.advanceFrames(kJitterPeriod);  // 一整周期 ⟹ 回到同一抖动相位
    const std::string pb = eng.shot(tag + "_pB");
    const double liveB = eng.paramValue("", kTaaFeedbackLabel);
    if (std::abs(liveB - sp.probeFeedback) > 1e-5)
        throw std::runtime_error(name + "：/param 写入后引擎回显 " + d4(liveB) + " ≠ 请求 "
            + d4(sp.probeFeedback) + "（滑杆写不进 env，整条扫描曲线无解释力）");
    const cv::Mat ia = cv::imread(pa, cv::IMREAD_UNCHANGED);
    const cv::Mat ib = cv::imread(pb, cv::IMREAD_UNCHANGED);
    if (ia.empty() || ib.empty() || ia.size() != ib.size())
        throw std::runtime_error(name + "：/param A/B 探针的图读不到或尺寸不同");
    cv::Mat d;
    cv::absdiff(ia, ib, d);
    g.probeBytes = cv::norm(d, cv::NORM_INF);
    g.probeChangedPx = static_cast<double>(cv::countNonZero(anyChannelDiff(ia, ib)))
        / static_cast<double>(ia.total());
    std::printf("      └ /param A/B 探针（同进程 · 静止机位 · 隔整 1 个抖动周期 = 同相位）："
                "fb %s → %s，引擎回显 %s，字节差 %d（变化 %.2f%%）\n",
        d4(sp.feedback).c_str(), d4(sp.probeFeedback).c_str(), d4(liveB).c_str(),
        static_cast<int>(g.probeBytes), g.probeChangedPx * 100.0);
}

/**
 * 跑一组：交付配置 → 起引擎 → 热身（空转不写盘）→ /shotburst 逐帧连续采集。
 * 掩膜组（spec.attachment 非空）只收 alpha（RGB 是 tonemap 前的 HDR，不进偏差统计）。
 *
 * 本函数只剩**编排**（批次 B11 拆小：原 176 代码行 / 认知 46）——四段各自搬进
 * `collectProfileOpts` / `accumulateGroupFrames` / `validateGroupFrames` / `runParamProbe`，
 * ⚠ 这里的**调用顺序 = 原实现从上到下的顺序 = RPC 与打印顺序**，一字不许换。
 */
Group collectGroup(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& cam, const Spec& sp, int burst)
{
    Group g;
    g.name = name;
    g.warm = sp.warm;

    makeProfile(dir, collectProfileOpts(cam, sp));

    Engine eng(dir);
    eng.setCamera({cam.position, cam.target, cam.fov});
    // 红线 6（参数扫描）：**引擎自己回显**的生效 feedback，不是写进 lua 的值。
    // 读回落在热身之前与之后都一样（本判据不写这个滑杆），落在前面是为了让「没生效」
    // 在采集开始前就出声。label 改名 / 滑杆被摘 ⟹ paramValue 抛 = 当场停下。
    g.liveFeedback = eng.paramValue("", kTaaFeedbackLabel);
    // 相机姿态回显（批 13.14a 相机运动版尾长）：pitch/fov 在 burst 内恒定（`frame_yaw` 不碰
    // 它们），读一次即够，但**必须来自引擎**（红线 6 同源）。
    const EngineCameraPose pose = eng.cameraPoseDegrees();
    g.pitchDegrees = pose.pitchDegrees;
    g.fovDegrees = pose.fovDegrees;
    eng.advanceFrames(static_cast<uint32_t>(sp.warm));
    g.framePre = eng.frameCount();
    // ⚠ yaw 与帧计数器**分两次 RPC**，两者之间可能过一帧 ⟹ 各自带 ±1 帧的不确定度。
    // 但每个组都是同样的时序 ⟹ 做差时抵消，而配对恰好只依赖差。
    g.yawPre = eng.cameraPoseDegrees().yawDegrees;
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(burst),
        tag + "_c", sp.attachment);
    g.framePost = eng.frameCount();
    g.yawPost = eng.cameraPoseDegrees().yawDegrees;

    accumulateGroupFrames(g, name, sp, cam, files);
    validateGroupFrames(g, name, sp);
    if (sp.probeFeedback > 0.0)
        runParamProbe(g, eng, name, tag, sp);
    return g;
}

}  // namespace tmotion

}  // namespace RGL::imgqa
