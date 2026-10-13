// bistro **真实场景**采集原语（批 13.7b，债-48 主体）+ 批 13.10 历史拒收 × 残余交叉表。
//
//   从 `temporal_residual.cpp` 拆出（文件护栏 500 代码行）。缝 = **采集原语 vs 机位编排**：
//   这一层只有「一组采集」（`runBistroGroup` / `runBistroRejectGroup`）与「一张拒收表」
//   （`reportRejectAttribution`）；六组怎么排、两条守卫在哪一步中止、热图与分带表的先后，
//   是 `temporal_residual_bistro_camera.cpp` 的契约。
//
//   与 aa_target profile 的三处口径差异（配置 / 区域 / 机位）与每机位五组的编号语义 =
//   下面那段批 13.7b 注释（原样搬来，读数字前必读）。

#include "temporal_residual_metric.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

// ── 批 13.7b `--bistro` 变体：**真实场景** bistro（债-48 主体，**只出数不判** + 两条量具守卫）──
//
// 为什么必须有这条（一句话版）：合成靶 aa_target 是 LDR 黑白细杆，交付档 `frac>64LSB` 本来就是
// 0，症状幅度比 bistro 的 HDR 高亮细线低一个量级 ⟹「meanSwing 降 7.2×」推不出「门框缝隙没了」。
// 本变体在**交付配置**（deferred + env/skybox/IBL/AO/shadow 全开）+ **用户报障机位**上重测
// 短窗 vs 长窗（两档显式 `feedback`，历史：长窗档曾由 `static_boost` 给出，批 13.14b 已删）
// 的绝对残差，并顺带出债-47：`phaseAverageLuma` 梯度能量比
// 的**线性域**读数（`docs/phase13_plan.md` §1.5 的 0.671 是显示空间旧账，本处只出数、改判由
// 主 agent 记账，本工具不动那份文档）。
//
// 与 aa_target profile 的三处口径差异（每处都是被迫的，读数字前必读）：
//   ① 配置 = `deliveryConfigDir` 模式（红线 4：合成副本不覆盖 env/skybox 路径，bistro 的 HDR
//      高亮细线正是本症状的激励源）。variant 留空 = 尊重交付的 deferred；`base_space` **钉真**
//      ——引擎默认 true（`scene_config.cppm::SceneConfig::taaBaseSpace`，批 13.5 起 = 交付态），
//      而 `ProfileOpts::taaBaseSpace` 默认 false 是批 13.2 的旧口径且 `rewriteTaaBlock` 会
//      **显式写键**，不钉 = 静默换场景。
//   ② 区域 = **全帧**：交付配置 `diagnostics.picking = false`（object_id pass 不在图里），
//      打开它 = 改图形态 = 不再是交付路径。aa_target 的「objectID 内部外扩 3px」是为排除
//      常量 clear 背景；bistro 画面几乎被几何填满，全帧就是「交付了什么」的最直白区域。
//      ⚠ 因此 bistro 的数与 aa_target 的数**不可直接比**（区域不同），可比的是算子、域、τ 定义。
//   ③ 机位 = `ProfileOpts::hasCamera` 写进 profile 的 camera.lua（**第 1 帧生效**；RPC 的
//      `setCamera` 只是幂等重发，理由全文见 `ProfileOpts::hasCamera` 注释）。
//
// 每机位五组（只差被测项；编号 N 与 aa_target 的 R/B 错开防混读；N5 为后补的归因对照。
// 短窗档 = fb 0.12，长窗档 = `--long-feedback`，默认 0.01）：
//   N1 短窗 fb=.12 js=1   → 被测（用户现状的对照端）+ **定 τ**（五组共用）+ 阳性锚点的载体
//   N2 长窗 fb=<档> js=1  → 被测（长窗档，α = `--long-feedback`，默认 0.01；历史：这一档曾由
//        `static_boost` 的地板给出，批 13.14b 后直接写 `feedback`）
//   N3 长窗 js=0          → **反向**：js=0 + 静止机位 ⟹ 首帧起历史 = 本帧（EMA 不动点，
//        与 α 无关）⟹ 输出逐帧逐位相同 ⟹ 残差必须**恰为 0**；非零 = 长窗档接错地方或引擎
//        有第二时域源，该机位 N1/N2 的读数一并作废
//   N5 短窗 js=0          → 反向**对照**（α=.12 无长窗）：N3 非零时用它归因——doorframe
//        实测 bistro 在 js=0 有 8px×1LSB 的奇偶帧交替地板（与窗档无关；机制候选 =
//        jitterPx=(halton−0.5)·0 的**零符号**随相位翻，详见 G2 注释）
//   N0 TAA 全关           → **确定性地板**（`determinism` 在 bistro 交付配置上的实例化）：
//        连拍必须逐位相同；非零 ⟹ 非确定性在 TAA 之外，全表读数存疑（G0，见 runBistroCamera）
//   N6（--diag-bs-off）   → 长窗 js=0 但 base_space 关：把 N3 的翻转归因到 base_space 路径还是
//        jitter 注入/裁剪盒等公共路径
//   N4 fb=1  js=1         → 梯度参考（理想 16 相位平均；**长窗档不碰它**——把 fb=1 也压到
//        长窗，「只抖不滤」的参考语义就没了，同 B1 的理由）
//
// 两条守卫（不是质量判据，是「量具还活着吗」；质量结论本轮**只出数不判**，口径同
// 合成靶长短窗变体先例——判据 ⑤ 的带是短窗口径，拿长窗读数撞它 = 把「变好」判成「不合格」）：
//   G1 阳性锚点：doorframe 短窗 frac(swing>τ) ≥ 1%（与 aa_target 锚点同常量 `kAnchorFracFloor`）。
//      用户肉眼看得见闪烁是既成事实，量出来接近 0 = 量具坏了 ⟹ **立刻停下**（rc=2），不跑其余机位。
//   G2 反向：N3 的 meanSwing / maxSwing **恰为 0**（无容差，机制见上）。
//   半间差 halfGapMax 与闭式容差 (1−α)^warm×满幅+量化项 只打印作**热身自证**（bistro 全帧 p99
//   白电平高于合成靶，该容差在这里偏保守；超了打 ⚠ 不判红——它防的是「量到瞬态」，而 N3 恰 0
//   已经排除了「连拍不是相位的确定性函数」里最贵的那类失效）。
//
// 批 13.7c 追加**亮度 × 梯度二维分带**（每机位一张短窗表 + 一张长窗表 + 一张差表，每格三个数：
// share / rate>τ / 残余质量占比；另附阈值与梯度轴的敏感性）。它要回答的是现有分带口径答不了的
// 问题——所有现有测量只按梯度分带，而梯度分不清「几何边」与「高亮边」。要判的假设、两根轴的
// 口径与定阈值的规则（闭式反解显示链 + 实测直方图门槛）、⭐ 判决线（**跑前写死**）全文 =
// `temporal_residual.h`「亮度 × 梯度 二维分带」段；出表代码在本文件 `reportLumaGradBands`。
// 尺子取 N1（短窗 = 用户现状的对照端）并**两态共用**，差表里 Δshare 恰为 0 就是这件事的自证。

/**
 * 一组 bistro 采集。红线逐条落点：分辨率自证（红线 1）、热身空转（红线 2）、
 * `prepareLinear` 复用（红线 3）、交付配置（红线 4）。
 *
 * `taa=false` = N0 确定性地板（TAA 整个关，连 jitter 注入都不发生）；`baseSpace=false` 只服务
 * N6 诊断组（把 base_space 的 un-jitter / c_{t−1} 修正摘掉，其余不动）——两者默认值 = 交付态。
 * `wantMaps` = 留下**逐像素图**（swing / 显示域亮度 / 两域梯度）：热图与「亮度×梯度」分带表
 * 要它们，其余组（N0/N3/N4/N5/N6）只出标量读数，不留图省内存。
 */
BistroRun runBistroGroup(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& cam, double feedback, double jitterScale, int periods, double warmMult,
    double& tau, bool wantMaps, bool taa,
    bool baseSpace, const ClipArm& arm)
{
    BistroRun r;
    r.name = name;

    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 红线 4（口径差异①）
    o.hasCamera = true;                                // 机位第 1 帧生效（口径差异③）
    o.cameraPosition = cam.position;
    o.cameraTarget = cam.target;
    o.cameraFov = cam.fov;
    o.aa = taa ? Aa::Taa : Aa::Off;
    o.taaFeedback = feedback;
    o.taaJitterScale = jitterScale;
    o.taaBaseSpace = baseSpace;  // 交付态 = 引擎默认 true（口径差异①，勿改默认）
    o.taaClipMode = arm.mode;    // 批 13.11 消融臂（0 = 门关 = 不写键）
    // ⚠ 0 **不再等于「交付配置原样」**：批 13.13 起交付默认是 scene.lua 的 clip_mode = 11，
    // 而本工具整块替换 taa 段 ⟹ 不写键 = 引擎默认 0。要测交付态请显式 `--clip 11`。
    o.taaClipSoft = arm.soft;
    makeProfile(dir, o);

    // 热身 = ⌈warmMult/α⌉ 对齐到抖动周期；α = **写进 feedback 的那一档**（长短窗两档都是显式
    // α，长窗档按长窗热身——按 0.12 热身长窗 = 把画面冻在瞬态上读「更好」的假绿）。红线 2。
    // TAA 关无历史。
    r.alpha = taa ? std::clamp(feedback, 1e-6, 1.0) : 1.0;
    const int warmRaw = static_cast<int>(std::ceil(warmMult / r.alpha));
    r.warm = ((warmRaw + kResidualPeriod - 1) / kResidualPeriod) * kResidualPeriod;

    const int frames = kResidualPeriod * periods;
    Engine eng(dir);
    eng.setCamera({cam.position, cam.target, cam.fov});
    // 债-89：空转到**绝对锚点**（= 基座 + 热身，到锚帧已渲染 ≥ warm 帧，红线 2 的空转
    // 口径保持）后从锚点起连拍——G0「TAA 全关连拍 = 确定性地板」要恒真，burst 起始的
    // 绝对帧号必须确定（改前 pristine 五连跑出 2 种结构签名 = 相位彩票）。
    const uint32_t anchor = kBurstAnchorBase + static_cast<uint32_t>(r.warm);
    eng.advanceToFrame(anchor - kAnchorLead);
    const auto files = eng.shotBurstAt("screenshots", static_cast<uint32_t>(frames), anchor,
        tag + "_c");

    std::vector<Frame> shots;
    shots.reserve(files.size());
    std::vector<cv::Mat> gray;  ///< 显示空间报告用（判据不读它，同 Run 的注释）
    gray.reserve(files.size());
    cv::Mat first;
    for (size_t i = 0; i < files.size(); ++i) {
        Frame f;
        f.camera = {cam.position, cam.target, cam.fov};
        f.bgr = readPngBgr(files[i]);
        if (i == 0)
            first = f.bgr.clone();
        else {
            cv::Mat d;
            cv::absdiff(first, f.bgr, d);
            r.byteMaxVsFirst = std::max(r.byteMaxVsFirst, cv::norm(d, cv::NORM_INF));
            const int64_t ch = cv::countNonZero(anyChannelDiff(first, f.bgr));
            r.changedPx = std::max(r.changedPx, ch);
            r.changedVsFirst =
                std::max(r.changedVsFirst, static_cast<double>(ch) / static_cast<double>(
                                                       f.bgr.total()));
        }
        cv::Mat g;
        cv::cvtColor(f.bgr, g, cv::COLOR_BGR2GRAY);
        gray.push_back(std::move(g));
        prepareLinear(f);  // 红线 3：复用，不另写解码
        f.bgr.release();
        f.lin.release();
        shots.push_back(std::move(f));
    }
    // 红线 1 自证：残差是逐像素计数/均值，换分辨率 = 换口径（实测湿面 720p vs 1440p ≈ 4× 像素数）。
    r.frameSize = shots.front().luma.size();
    std::printf("  [%s] 帧 %dx%d（红线 1 自证）\n", name.c_str(), r.frameSize.width,
        r.frameSize.height);
    if (r.frameSize != cv::Size(kSize[0], kSize[1]))
        throw std::runtime_error(name + "：帧尺寸 " + std::to_string(r.frameSize.width) + "x"
            + std::to_string(r.frameSize.height) + " ≠ " + std::to_string(kSize[0]) + "x"
            + std::to_string(kSize[1]) + "——残差口径按 720p 钉，换分辨率即换口径");

    r.avg = phaseAverageLuma(shots, periods);
    auto vals = valuesInMask(r.avg, {});  // 区域 = 全帧（口径差异②）
    std::sort(vals.begin(), vals.end());
    r.whiteLuma = percentileOf(vals, 0.99);
    r.maxLuma = vals.empty() ? 0.0 : vals.back();
    r.quant = maxLsbStepUpTo(r.maxLuma);
    // τ 是**场景×机位**属性（白电平处 1 个显示 LSB）不是组属性 ⟹ N1 定一次，四组共用
    // （跨组换 τ = frac 的跨组比较失去意义，同 aa_target 的纪律）。
    if (tau <= 0.0)
        tau = lsbLinearAtLuma(r.whiteLuma);
    r.stat = phaseSwingStat(shots, {}, tau, periods, wantMaps ? &r.swingMap : nullptr);
    r.grad = gradEnergy(r.avg, {});
    {
        // **显示域**字节亮度多帧均值（先 tonemap 再平均 = 红线 1 禁止的那个**统计**口径）。
        // 它在这里不是统计而是**分带尺子**：亮度轴要的就是「交付面上这块看起来多亮」，
        // 而截图里没有线性 HDR。同一个 acc 顺带给出 byteGrad（§1.5 的 0.671 就是这类读数，
        // 两个域同时出数，域选错的代价当场可见）。
        cv::Mat acc = cv::Mat::zeros(r.avg.size(), CV_64FC1), tmp;
        for (const auto& g : gray) {
            g.convertTo(tmp, CV_64F);
            cv::add(acc, tmp, acc);
        }
        acc /= static_cast<double>(gray.size());
        r.byteGrad = gradEnergy(acc, {});
        if (wantMaps) {
            r.byteMean = acc;
            r.gradLin = sobelAbsGrad(r.avg);   // 梯度轴主口径（与 r.grad 同一算子）
            r.gradByte = sobelAbsGrad(acc);    // 敏感性口径（显示域，对高亮边更保守）
            // 跨帧**最大**：均值把「逐帧闪出 255 的真削顶像素」判出档外（偏保守方向），
            // 最大口径是它的对照。两者结论不一致 = 亮度代理本身不稳，须报「分不出」。
            cv::Mat mx;
            gray.front().convertTo(mx, CV_8UC1);
            for (const auto& g : gray)
                cv::max(mx, g, mx);
            mx.convertTo(r.byteMax, CV_64F);
        }
    }
    // 在场性守卫（红线 4 的兜底）：全黑画面的残差**恰好为 0**，什么判据都会绿。
    if (r.stat.meanLuma <= 0.0)
        throw std::runtime_error(name + "：画面是黑的（meanLuma=" + d6(r.stat.meanLuma)
            + "）——交付配置坏了？全零的残差不是「稳定」，是「没画」");
    return r;
}

// ── 批 13.10：历史拒收 × 残余 的因果交叉表（判「长窗压不住」是不是邻域盒）────────────
//
// 假设、口径三条、⭐ 判决线（跑前写死）全文 = `temporal_residual.h`「历史拒收 × 残余」段。
// 本处只放采集与出表；分桶与交叉计数是**共用算子**（`rejectFractionOf` / `rejectCrossTab`）。

/**
 * 一组「历史拒收」采集：与 N1/N2 **只差 `diag_reject` 一个键**（RGB 逐位不变，见
 * `taa.frag.slang` 诊断 alpha 段），连拍取 **taa_out 附件**的 alpha 码。
 *
 * 热身与 α 的口径与 `runBistroGroup` 逐字相同（α = 写进 feedback 的那一档）——拒收模式**依赖历史状态**，
 * 热身不足读到的是瞬态的盒行为，不是稳态的。
 */
BistroReject runBistroRejectGroup(const std::string& dir, const std::string& name,
    const std::string& tag, const BistroCam& cam, double feedback, int periods, double warmMult)
{
    BistroReject r;
    r.name = name;

    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";
    o.hasCamera = true;
    o.cameraPosition = cam.position;
    o.cameraTarget = cam.target;
    o.cameraFov = cam.fov;
    o.aa = Aa::Taa;
    o.taaFeedback = feedback;
    o.taaJitterScale = 1.0;
    o.taaBaseSpace = true;
    o.taaDiagReject = true;  // ← 与被测组唯一的差别（RGB 逐位不变）
    makeProfile(dir, o);

    const double alpha = std::clamp(feedback, 1e-6, 1.0);
    const int warmRaw = static_cast<int>(std::ceil(warmMult / alpha));
    const int warm = ((warmRaw + kResidualPeriod - 1) / kResidualPeriod) * kResidualPeriod;

    Engine eng(dir);
    eng.setCamera({cam.position, cam.target, cam.fov});
    // 债-89：锚点采集，与 runBistroGroup 同口径（拒收码依赖历史状态，到锚帧已渲染
    // anchor ≥ warm 帧，热身口径保持）。
    const uint32_t anchor = kBurstAnchorBase + static_cast<uint32_t>(warm);
    eng.advanceToFrame(anchor - kAnchorLead);
    const auto files = eng.shotBurstAt("screenshots", static_cast<uint32_t>(kResidualPeriod * periods),
        anchor, tag + "_c", "taa_out");

    for (size_t i = 0; i < files.size(); ++i) {
        cv::Mat img = cv::imread(files[i], cv::IMREAD_UNCHANGED);
        if (img.empty() || img.channels() != 4)
            throw std::runtime_error(name + "：taa_out 附件读不到或不是 BGRA: " + files[i]);
        cv::Mat ch[4];
        cv::split(img, ch);
        const RejectFraction f = rejectFractionOf(ch[3]);
        r.frac.accepted += f.accepted;
        r.frac.clipped += f.clipped;
        r.frac.collapsed += f.collapsed;
        r.frac.reproject += f.reproject;
        r.frac.noHistory += f.noHistory;
        cv::Mat m, mi;
        if (i == 0) {
            r.nAny = cv::Mat::zeros(ch[3].size(), CV_32SC1);
            r.nCollapse = cv::Mat::zeros(ch[3].size(), CV_32SC1);
        }
        // ≥32 = 不是「原样收下」（0 码）。掩膜是 0/255 的 CV_8U，**必须换算成 0/1 再累加**
        // （直接 `+=` 会把每帧记成 255 帧，且类型不符当场抛）。
        cv::compare(ch[3], 32, m, cv::CMP_GE);
        m.convertTo(mi, CV_32SC1, 1.0 / 255.0);
        r.nAny += mi;
        // [96,160) = 塌回邻域盒 centroid（0.5 码）= 「历史贡献变成本帧邻域均值」的那一档。
        cv::Mat lo, hi, both;
        cv::compare(ch[3], 96, lo, cv::CMP_GE);
        cv::compare(ch[3], 160, hi, cv::CMP_LT);
        cv::bitwise_and(lo, hi, both);
        both.convertTo(mi, CV_32SC1, 1.0 / 255.0);
        r.nCollapse += mi;
        ++r.frames;
        // 在场性守卫（同交付组）：全黑画面的拒收码照样能读，但那张图不是「交付了什么」。
        cv::Mat gray;
        cv::cvtColor(img, gray, cv::COLOR_BGRA2GRAY);
        if (cv::mean(gray)[0] <= 1.0)
            throw std::runtime_error(name + "：taa_out 画面是黑的——交付配置坏了？");
    }
    if (r.frames == 0)
        throw std::runtime_error(name + "：连拍 0 帧（/shotburst 没出图）");
    // 自检 1（判据工具自己要有自检）：五桶是**划分** ⟹ 之和必须 = 帧数。
    const double sum = r.frac.accepted + r.frac.clipped + r.frac.collapsed + r.frac.reproject
        + r.frac.noHistory;
    if (std::abs(sum - static_cast<double>(r.frames)) > 1e-6)
        throw std::runtime_error(name + "：五桶之和 = " + d6(sum) + " ≠ 帧数 "
            + std::to_string(r.frames) + "——分桶代码坏了，读数无效");
    r.frac.accepted /= r.frames;
    r.frac.clipped /= r.frames;
    r.frac.collapsed /= r.frames;
    r.frac.reproject /= r.frames;
    r.frac.noHistory /= r.frames;
    // 自检 2（阳性锚点）：门没生效 = alpha 恒 1.0 = 全桶「无历史」。历史在热身 16 帧后必然
    // 有效 ⟹ noHistory > 0.5 只可能是 `diag_reject` 键没生效（键名/载荷/alpha 任一环断了）。
    if (r.frac.noHistory > 0.5)
        throw std::runtime_error(name + "：掩膜全桶「无历史」(noHistory = " + d6(r.frac.noHistory)
            + "）——diag_reject 没生效，拒收读数无效");
    return r;
}

/**
 * 出拒收交叉表 + 逐档抑制比 + 执行两条判决线（两条都**跑前写死**在头注释；这里只执行）。
 *
 * @param swingOn  长窗档的逐像素 swing（`--reject` 用它做交叉表）
 * @param swingOff 短窗档的逐像素 swing（抑制比的分母；同一次采集）
 * @param alphaOn  长窗档的**本帧权重**（= `--long-feedback`）。**>0 才出抑制比与主判据**
 *                 （短窗档那次没有「压」这个动作，比无可比）
 * @param alphaOff 短窗档的本帧权重（= 0.12）
 */
/// 交叉表的表头与五行（「%d 帧里有几帧历史被动过」× swing）。
/// ⚠ 五个档名与 0 档那句标注进结构签名；ρ 列在抑制表无效时打 `n/a` 而不是 0（0 会被读成「完美抑制」）。
void printRejectBinRows(const RejectCrossTab& t, const SuppressionTable& sup, int frames)
{
    static const char* const kBinTag[5] = {
        "0 全程收下", "1-2 偶发", "3-8", "9-16", "17+" };
    const double n = static_cast<double>(t.total);
    std::printf("  ┌ 每像素「%d 帧里有几帧历史被动过」× swing（ρ = 长窗/短窗平均 swing）\n",
        frames);
    std::printf("    %-13s %9s %9s %10s %10s %10s\n", "档", "share", "rate>τ", "残余质量",
        "Σswing", "ρ 抑制比");
    for (int i = 0; i < 5; ++i) {
        const RejectBinCell& c = t.bin[i];
        std::printf("    %-13s %8s%% %8s%% %9s%% %8s%% %10s%s\n", kBinTag[i],
            d2(100.0 * c.pixels / n).c_str(),
            d2(c.pixels > 0 ? 100.0 * c.over / c.pixels : 0.0).c_str(),
            d2(t.over > 0 ? 100.0 * c.over / t.over : 0.0).c_str(),
            d2(t.mass > 0 ? 100.0 * c.mass / t.mass : 0.0).c_str(),
            sup.valid ? d4(sup.bin[i].rho).c_str() : "n/a",
            i == 0 ? "   ← 纯 EMA 档（ρ 的闭式期望 = ρ*）" : "");
    }
}

/// 执行归因判决线（跑前写死）并把四个读数 + 分支写进 `rep`。
void printRejectVerdict(const RejectCrossTab& t, double n, RejectReport& rep)
{
    const double rAll = t.total > 0 ? static_cast<double>(t.over) / n : 0.0;
    const double r0 = t.bin[0].pixels > 0
            ? static_cast<double>(t.bin[0].over) / static_cast<double>(t.bin[0].pixels)
            : 0.0;
    const double m3p = t.mass > 0.0
            ? (t.bin[2].mass + t.bin[3].mass + t.bin[4].mass) / t.mass
            : 0.0;
    const double bin0Share = t.bin[0].pixels / n;
    const RejectBranch br = classifyReject(t, bin0Share);
    std::printf("  ⭐ 归因判决（跑前写死：支持 = r0 ≤ %s×rAll 且 m3p ≥ %s；否证 = r0 ≥ %s×rAll）\n",
        d2(kRejectAnchorQuiet).c_str(), d2(kRejectMassLine).c_str(), d2(kRejectRefuteSame).c_str());
    std::printf("    r0 = %s%%  rAll = %s%%（r0/rAll = %s）  m3p = %s%%  0 档面积 = %s%%%s\n",
        d2(r0 * 100.0).c_str(), d2(rAll * 100.0).c_str(),
        rAll > 0.0 ? d4(r0 / rAll).c_str() : "n/a", d2(m3p * 100.0).c_str(),
        d2(bin0Share * 100.0).c_str(),
        bin0Share < kRejectMinBin0Share ? "  ⚠ 低于门槛 5% ⟹ 无「老实累积」对照" : "");
    std::printf("    ⟹ **%s**\n", rejectBranchName(br));
    rep.r0 = r0;
    rep.rAll = rAll;
    rep.m3p = m3p;
    rep.bin0Share = bin0Share;
    rep.branch = br == RejectBranch::Support ? 0 : (br == RejectBranch::Refute ? 1 : 2);
}

/// 主判据：抑制保真度（闭式锚定，`temporal-residual-window-fidelity`）。
/// 线 = 1.5 × ρ*，ρ* = |H(α_on)|/|H(α_off)| 由**配置的 α** 运行时算（改档位自动自适应）；
/// 下标 on/off = **长窗/短窗**两档（变量名沿用采集侧的 swingOn/swingOff，语义同）。
/// ⚠ **>0 且抑制表有效才出**（短窗档那次没有「压」这个动作，比无可比）。
void printRejectFidelity(const SuppressionTable& sup, double alphaOn, double alphaOff,
    RejectReport& rep)
{
    if (!(alphaOn > 0.0 && sup.valid))
        return;
    rep.fidelityRan = true;
    rep.bin0Rho = sup.bin[0].rho;
    rep.rho = sup.total.rho;
    rep.rhoStar = emaSuppressionRatio(alphaOn, alphaOff);
    rep.line = kBoostFidelityLine * rep.rhoStar;
    rep.fidelityPass = rep.rho <= rep.line;
    std::printf("  ⭐ 抑制保真度（主判据，跑前写死：ρ ≤ %s × ρ*；ρ* = |H(α=%.4g)|/|H(α=%.4g)|）\n",
        d2(kBoostFidelityLine).c_str(), alphaOn, alphaOff);
    std::printf("    ρ = %s（%s→%s）  ρ* 闭式 = %s  线 = %s  ⟹ **%s**（超线 %s×）\n",
        d4(rep.rho).c_str(), d6(sup.total.swingOff).c_str(), d6(sup.total.swingOn).c_str(),
        d4(rep.rhoStar).c_str(), d4(rep.line).c_str(),
        rep.fidelityPass ? "PASS" : "FAIL", d4(rep.rho / rep.line).c_str());
    std::printf("    内部阳性锚点：0 档 ρ = %s（闭式期望 %s，偏 %s×）⟹ 绿例就在同一帧里，"
                "本判据不是永远红的摆设\n",
        d4(rep.bin0Rho).c_str(), d4(rep.rhoStar).c_str(),
        rep.rhoStar > 0.0 ? d4(rep.bin0Rho / rep.rhoStar).c_str() : "n/a");
}

RejectReport reportRejectAttribution(const BistroReject& R, const cv::Mat& swingOn,
    const cv::Mat& swingOff, double tau, const std::string& camName, double alphaOn,
    double alphaOff)
{
    std::printf("\n── 历史拒收 × 残余 交叉表（%s｜连拍 %d 帧取 taa_out alpha；τ = %s）──\n",
        camName.c_str(), R.frames, d6(tau).c_str());
    RejectReport rep;
    rep.collapsed = R.frac.collapsed;
    rep.clipped = R.frac.clipped;
    std::printf("  五桶（帧均占比）：收下 %s%% · 盒裁剪 %s%% · **塌回 centroid %s%%** · "
                "重投影拒收 %s%% · 无历史 %s%%\n",
        d2(R.frac.accepted * 100.0).c_str(), d2(R.frac.clipped * 100.0).c_str(),
        d2(R.frac.collapsed * 100.0).c_str(), d2(R.frac.reproject * 100.0).c_str(),
        d2(R.frac.noHistory * 100.0).c_str());
    const RejectCrossTab t = rejectCrossTab(R.nAny, swingOn, tau);
    if (!t.valid()) {
        std::printf("  ✗ 交叉表未出（swing 与拒收图尺寸/类型不符 = 不是同一次采集）\n");
        return rep;
    }
    rep.ok = true;
    const SuppressionTable sup = suppressionByRejectBin(swingOn, swingOff, R.nAny);
    printRejectBinRows(t, sup, R.frames);
    printRejectVerdict(t, static_cast<double>(t.total), rep);
    printRejectFidelity(sup, alphaOn, alphaOff, rep);
    return rep;
}

}  // namespace RGL::imgqa
