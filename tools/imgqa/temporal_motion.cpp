// temporal-motion —— **相机运动下 TAA 到底抗了什么锯齿**（Phase 13 诊断判据，只出数 + 一句判决）
//
// ## 为什么必须有这条（问题本体）
//
// 用户报「**相机在动**（WASD/鼠标转视角）时边缘闪烁很严重，和没抗锯齿一样」。现有时域判据
// 全部**量不到**这句话：
//  - `temporal` / `temporal-residual` 钉的是**静止机位**（激励 = jitter），它们的头注释里
//    明确写着「静止机位下 TAA 关 = 逐位相同 = 零信号，所以激励必须是 jitter 而不是运动」——
//    那是为了把「视点真的动了」这个混淆量摘干净。本条**反过来**：视点动就是被测项。
//  - `taa-swim` 量「逐帧差能不能被一个全局位移解释」（游动），它不问「离理想多远」。
//  - 反模式 `moving-camera-l1-ratio` / `screen-space-l1-frame-diff` 否决的是**逐帧 L1 之比**
//    （病根 = L1 对模糊近似守恒 + 视点变化不是噪声）。本条**不是**那条：见下一节。
//
// ## 本条问的是一个有唯一答案的问题
//
// 「候选交付图，在**同一视点**下，离『没有锯齿也不糊的 720p 图』有多远？」
// 参考 = **同机位、同帧序列**的 2560×1440 渲染（TAA 关）→ **线性域** 2×2 面积平均降到
// 1280×720。超采样是「抗锯齿该长什么样」的定义级答案，不需要基线图像。
// 主指标 = 候选与参考在**边缘像素**上的线性域平均偏差；副指标 = 边缘梯度能量比（机制判读）。
//
// ⚠ **与反模式的区别是"对齐"两个字**：逐帧差比较的是「相邻两帧的同一屏幕像素」= 不同世界点；
// 本条把候选第 i 帧与参考第 j 帧**按帧号配对到同一视点**之后再比，运动项被完全消掉。
// 所以它不受 L1 守恒病影响——但也因此**依赖对齐正确**，护栏见 `temporal-motion-alignment`。
//
// ## 运动源 = **相机**帧驱动 yaw（默认；spin 降级为可选模式）
//
// `--motion camera`（默认）：camera.lua `frame_yaw` ≠ 0 ⟹ yaw = 基准 + 每帧角·帧号（纯函数）。
// 为什么必须是**相机**：spin 转的是**场景**、相机不动 ⟹ `prevViewProj` 恒定 ⟹ TAA 历史
// 重投影退化成恒等映射 ⟹ `prevViewProj` / 重投影那条路**一次都没被走到**——而用户报的
// 症状恰好落在那条路上（相机动 = 每帧 view 变）。spin 那套量的是「静止机位 + 动物」
// （= 债-49 的鬼影仪器，另一件事，有价值但**不是本任务**），保留为 `--motion spin`。
//
// 为什么是**帧号纯函数**（`temporal.cpp` 头注释当年登记的那条「要做微动激励得先加按帧索引
// 驱动的运动」，本文件就是那个消费方）：候选与参考是**两个引擎进程**，dt 累积的每帧角随
// 帧时长漂 ⟹ 两边第 i 帧不是同一视点 ⟹ 对齐根本不存在。纯函数让「第 i 帧 = 哪个视点」与
// 运行速度无关；RPC 机位写入只引入**常数帧偏移**（引擎侧自动重定基，见 camera.cppm），
// 由下面的跨进程对齐吸收。
//
// 为什么是**绕 up 轴定角 yaw**（理由同 `temporal.cpp` 头注释）：刚体相机运动、像素位移与
// 深度无关（u′ = (u+tanθ)/(1−u·tanθ) 只依赖屏位不依赖深度），`prevViewProj` 重投影能
// **精确对齐**；平移 dolly 会让远处物体的单帧位移只剩近处的百分之几十，信号随深度衰减。
//
// ## 偏差这一个数为什么够用（以及它会把什么混起来）
//
// 参考是「理想 720p」。锯齿是偏离、模糊**也是**偏离，所以「偏差」= 总误差，方向正确；
// 但它把**机制**混在一起了（锯齿 vs 模糊都给非零偏差）。故梯度能量比是**共同主指标**：
//   梯度比 > 1 ⟹ 多出来的是**假边**（锯齿/振铃）；< 1 ⟹ 边缘被**摊平**（模糊）。
// 两个数一起读才出结论：「偏差降 + 梯度比降」= 用糊换掉了锯齿；「偏差不降 + 梯度比不降」
// = 什么都没发生（no-op）。判决句必须同时引用两者。
//
// ## 组（每组一个引擎进程；只差被测项）
//
//   M0  TAA 关 + 运动            → **地板**：无 AA 的单帧光栅，锯齿全在
//   M1  TAA 开 + 运动（交付态）  → **被测**（base_space=true = 引擎默认，交付口径）
//   M2  M1 + base_space=false    → 历史住**抖动空间**的对照（输出额外带 c_t ≤0.71px 位移
//        ⟹ 对参考有一个**系统性**错位的贡献；故附「按闭式 c_t 去抖」修正读数，见下）
//   M3  重投影消融 —— **按运动源选门**：
//        · camera 模式 = `reprojection=false`：prevViewProj 重投影**本身**被关（历史按同基
//          位置采样）⟹ 相机每帧动 ⟹ 历史错位 = 每帧像素位移 ⟹ 读数必须显著变差。
//          ⚠ 为什么不能用 `motion_compensation=false`：那条只关 **prevSpin 场景旋转补偿**，
//          相机运动、spin 关时它是恒等阵 ⟹ 关与不关**逐位相同** = 消融无效（上一版把 M3
//          定义成它，在相机激励下会退化成 M1 的复读机）。
//        · spin 模式 = `motion_compensation=false`（prevSpin 补偿关，历史按上一帧物体位置
//          累积）——spin 模式下这才是有效消融，旧口径原样保留。
//   M3x camera 模式专属对照 = `motion_compensation=false`：按上面的论证必须 ≈ M1
//        （恒等对照：证明「spin 补偿在相机运动下确实惰性」是**量出来的**不是断言的）
//   M4  TAA 开 + 静止            → 「有 AA 该长什么样」的参照（同配置、机位不动、热身 64）
//   REFm / REFs                  → 运动 / 静止各自的 2560×1440 超采样参考（TAA 关）
//   M1m / M4m                    → M1/M4 + `diag_reject=true`，连拍 **taa_out 附件**取
//        alpha = 逐像素「历史怎么了」码（0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 / 0.75 重投影
//        拒收 / 1 无历史）⟹ **历史被拒比例**。门只动 alpha（RGB 逐位 = M1/M4），
//        「窗棂残余闪烁」与「相机运动失效」两个问题一起判：若 M1 的边缘像素大多住在
//        「盒拒收」桶里，α 调多小都没用（历史根本没进混合）。
//
// ## 跨进程帧对齐（本判据的地基）
//
// `angle = 每帧角 × 帧号` 让「第 i 帧 = 哪个视点」可复现，但**绝对帧偏移**未知
// （RPC 登记落在第几帧由启动时序决定，headless 高速下差 1–3 帧）。做法：
//   ① 每组 burst 前后读 `Engine::frameCount()` 回显（自证 burst 连续、无掉帧）；
//   ② 逐候选帧在 `i ± window` 内按**全帧线性 L1** argmin 选参考帧，偏移取逐帧差的**中位数**；
//   ③ 护栏两条：argmin 余量（次优 vs 最优的相对差）+ 前/后半窗偏移是否一致（抓 burst 内掉帧）。
// 为什么用全帧 L1 而不是边缘带 L1：对齐要的是「视点是否一致」，全帧信息最多、与掩码无关；
// 用边缘带会让掩码的定义依赖对齐结果（循环）。
//
// ## M2 的去抖修正是**离散相位搜索**，不是自由参数
//
// 抖动相位 = `Halton(2,3)` 周期 16 ⟹ 第 i 帧的内容位移 `c_i` 只有 **16 个可能值**，
// 且整组共用一个未知常数相位偏移 p。于是「去抖」= 在 p ∈ [0,16) 里 argmin，
// **零自由参数**（连续亚像素搜索才是自由参数，那才是该拒绝的口径）。
// 同一套搜索也跑在 M1 上作**自证**：M1 若真在基空间，最优 p 的改善应当很小、且最优位移
// 接近 0（读数打印出来，不判）。
//
// ## 三条测量红线（违反即测量无效）
//
//  1. **统计一律在 sRGB 解码后的线性域**（`prepareLinear` 复用，不另写解码）。降采样也必须在
//     线性域做——luma 是 RGB 的线性泛函、INTER_AREA 是线性平均，两者可交换，故「先解线性求
//     luma 再 2×2 面积平均」与「先降 RGB 再求 luma」**严格相等**（这是本文件敢直接缩 luma 的依据）。
//  2. **分辨率钉死 1280×720**（参考组 2560×1440 后降回）且**回显实际帧尺寸**：偏差是逐像素
//     均值，换分辨率 = 换口径。参考组回显「渲染尺寸 + 统计尺寸」两个数。
//  3. **配置 = 交付配置**（`deliveryConfigDir`，env/skybox/IBL/AO/shadow 全开、deferred）。
//     合成 profile 上没有用户报的那些 1px 高亮细线（激励源），在它上面量到的「没效果」不算数。
//
// ⚠ **AO 带来的公共偏差地板（读跨组数字前必读）**：AO 是屏幕空间螺旋采样，1440p 参考的 AO
// 细节与 720p 候选**本来就不同**（半径按像素算），这部分偏差与 TAA 无关、**各组共用**。
// 它抬高所有组的绝对偏差，但 M0/M1/M3/M4 之间的**相对比较**不受影响（同一项加在两边）。
// 故本判据只出**比值**结论，不出「偏差绝对值 = 多少 LSB 可见」的绝对结论。
//
// ## 判决口径（写死，别事后调）
//
// R = devEdge(M1) / devEdge(M0)（边缘带上）：
//   R > 1.05 ⟹ 运动下 TAA **主动变差**（偏差比 TAA 关还大 + 梯度比在降 = 拖影。R>1 不是
//     「没作用」而是「反作用」，与 no-op 是两种结论，实测踩过：1.138 曾被旧口径读成 no-op）；
//   0.90 ≤ R ≤ 1.05 ⟹ 运动下 TAA **等于没抗锯齿**（no-op）；
//   R ≤ 0.70 且 dev(M1) 明显 > dev(M4) ⟹ **生效但受固有边界限制**（运动本身是误差源，
//     「运动下可用累积窗天然短」是固有代价，**这不是 bug**，要如实说）；
//   之间 ⟹ 部分生效，逐条说明。
// 参照读数 dev(M4)/dev(M0) = 「有 AA 该长什么样」与地板之比，它给出 R 的**可达下界**语义。
// **两种结论都要照实说**——本判据不是用来证明「有 bug」的。
// 支撑对照 = M3（消融）：它挂零 = 量具坏了（对齐/掩码/域坏掉时所有组会**一起**变成同一个
// 无意义的数，而「M1/M0 ≈ 1」看起来仍像一个结论），整张表作废，不用它下画质结论。

#include "imgqa.h"
#include "temporal_residual.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

/// 候选/交付分辨率（红线 2）。参考组渲染尺寸 = 2×，降采样后与候选同网格。
constexpr int kSize[2] = {1280, 720};
constexpr int kRefSize[2] = {2560, 1440};

/// 抖动周期（帧）——与 `rgl/taa_jitter.cppm::kTaaJitterPeriod` 同值（imgqa 不链 rgl 库，
/// 同式重抄；先例 = `temporal_residual.h::kResidualPeriod`，漂移由判据自己抓：M2 的
/// 离散相位搜索若周期不再是 16，最优 p 的改善会**消失**而不是变好，读数当场失真）。
constexpr uint32_t kJitterPeriod = 16u;

/// Halton 低差异序列第 index 项（同式重抄 `taa_jitter.cppm::haltonSequence`，index 从 1 起）。
double halton(uint32_t index, uint32_t base)
{
    double result = 0.0;
    double factor = 1.0 / static_cast<double>(base);
    for (uint32_t i = index; i > 0u; i /= base) {
        result += factor * static_cast<double>(i % base);
        factor /= static_cast<double>(base);
    }
    return result;
}

/**
 * 帧号 → **内容位移** c_t（像素单位）。口径 = `taa_jitter.cppm::contentShiftPx` =
 * `(+jx, −jy)`，**勿凭直觉写符号**（该函数注释里写着：x 不翻、y 被负高视口翻一次；
 * 符号若反，M2 的去抖会把 0.7px 的错位**翻倍**成 1.4px，实测可分辨）。
 */
cv::Point2d contentShiftPx(uint32_t frameIndex, double scale)
{
    const uint32_t i = frameIndex % kJitterPeriod + 1u;
    const double jx = (halton(i, 2u) - 0.5) * scale;
    const double jy = (halton(i, 3u) - 0.5) * scale;
    return {jx, -jy};
}

/// 把一张线性 luma 平移 (dx,dy)（双线性）。dst(x,y) = src(x−dx, y−dy)，边界钳边。
[[nodiscard]] cv::Mat shiftLuma(const cv::Mat& luma, cv::Point2d d)
{
    if (std::abs(d.x) < 1e-9 && std::abs(d.y) < 1e-9)
        return luma;  // 零位移**原样返回**：M1 的 p=0 读数必须与「不去抖」逐位相同
    const cv::Mat m = (cv::Mat_<double>(2, 3) << 1, 0, d.x, 0, 1, d.y);
    cv::Mat out;
    cv::warpAffine(luma, out, m, luma.size(), cv::INTER_LINEAR, cv::BORDER_REPLICATE);
    return out;
}

/// 一组的采集规格（每组 = 一个引擎进程；默认值 = 交付态）。
struct Spec {
    bool taa = false;
    double feedback = 0.12;
    double jitterScale = 1.0;
    bool baseSpace = true;
    bool motionCompensation = true;
    bool reprojection = true;
    bool diagReject = false;
    double motionAngle = 0.0;  ///< rad/帧（0 = 静止）
    bool cameraMotion = true;  ///< true = 相机 frame_yaw；false = 场景 spin
    int warm = 16;
    int refScale = 0;         ///< 1 = 以 2560×1440 渲染并线性域降采样回 1280×720
    std::string attachment;   ///< 非空 = 连拍具名附件（taa_out alpha 掩膜），不算 luma
};

/// 一组采集（luma 一律 CV_64FC1、一律 1280×720、一律线性域；掩膜组只有 alpha）。
struct Group {
    std::string name;
    int warm = 0;
    cv::Size rawSize;   ///< **引擎实际渲染尺寸**（红线 2 自证：参考组必须是 2560×1440）
    cv::Size frameSize; ///< 统计用尺寸（参考组 = 降采样后，恒 1280×720）
    std::vector<cv::Mat> luma;
    std::vector<cv::Mat> alpha; ///< 掩膜组专用：PNG 的 alpha 通道（CV_8UC1）
    double byteMaxAdjacent = 0.0;  ///< 相邻帧最大字节差（运动激励在场性自证）
    uint32_t framePre = 0, framePost = 0;
};

/**
 * 跑一组：交付配置 → 起引擎 → 热身（空转不写盘）→ /shotburst 逐帧连续采集。
 * 掩膜组（spec.attachment 非空）只收 alpha（RGB 是 tonemap 前的 HDR，不进偏差统计）。
 */
Group collectGroup(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& cam, const Spec& sp, int burst)
{
    Group g;
    g.name = name;
    g.warm = sp.warm;

    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 红线 3
    o.hasCamera = true;                                // 机位第 1 帧生效（RPC 只幂等重发）
    o.cameraPosition = cam.position;
    o.cameraTarget = cam.target;
    o.cameraFov = cam.fov;
    o.taa = sp.taa;
    o.taaFeedback = sp.feedback;
    o.taaJitterScale = sp.jitterScale;
    o.taaBaseSpace = sp.baseSpace;
    o.taaMotionCompensation = sp.motionCompensation;
    o.taaReprojection = sp.reprojection;
    o.taaDiagReject = sp.diagReject;
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
    makeProfile(dir, o);

    Engine eng(dir);
    eng.setCamera({cam.position, cam.target, cam.fov});
    eng.advanceFrames(static_cast<uint32_t>(sp.warm));
    g.framePre = eng.frameCount();
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(burst),
        tag + "_c", sp.attachment);
    g.framePost = eng.frameCount();

    cv::Mat prev;
    for (size_t i = 0; i < files.size(); ++i) {
        cv::Mat img = cv::imread(files[i], cv::IMREAD_UNCHANGED);
        if (img.empty())
            throw std::runtime_error(name + "：读不到 " + files[i]);
        cv::Mat bgr;
        if (img.channels() == 4) {  // 附件捕获（taa_out）= BGRA：alpha 是拒收码，BGR 另作他用
            cv::Mat ch[4];
            cv::split(img, ch);
            g.alpha.push_back(ch[3].clone());
            std::vector<cv::Mat> bgrCh = {ch[0], ch[1], ch[2]};
            cv::merge(bgrCh, bgr);
        }
        else
            bgr = img;
        g.rawSize = bgr.size();
        if (i > 0) {  // 字节域自证（激励在场判据读的是「画面到底动没动」，不是线性域小数值）
            cv::Mat d;
            cv::absdiff(prev, bgr, d);
            g.byteMaxAdjacent = std::max(g.byteMaxAdjacent, cv::norm(d, cv::NORM_INF));
        }
        prev = bgr.clone();
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
    const bool empty = sp.attachment.empty() ? g.luma.empty() : g.alpha.empty();
    if (empty)
        throw std::runtime_error(name + "：/shotburst 一张都没拿到");
    g.frameSize = sp.attachment.empty() ? g.luma.front().size() : g.rawSize;
    const cv::Size wantRaw = sp.refScale ? cv::Size(kRefSize[0], kRefSize[1])
                                         : cv::Size(kSize[0], kSize[1]);
    if (!sp.attachment.empty()) {
        std::printf("  [%s] 渲染 %dx%d → 附件 %s alpha 掩膜（%zu 张，红线 2 自证）｜帧计数 %u → %u"
                    "（Δ=%u，热身 %d）\n",
            name.c_str(), g.rawSize.width, g.rawSize.height, sp.attachment.c_str(),
            g.alpha.size(), g.framePre, g.framePost, g.framePost - g.framePre, sp.warm);
    }
    else {
        std::printf("  [%s] 渲染 %dx%d → 统计 %dx%d（%zu 张，红线 2 自证）｜帧计数 %u → %u"
                    "（Δ=%u，热身 %d）\n",
            name.c_str(), g.rawSize.width, g.rawSize.height, g.frameSize.width,
            g.frameSize.height, g.luma.size(), g.framePre, g.framePost,
            g.framePost - g.framePre, sp.warm);
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
    return g;
}

// ── 历史拒收掩膜（`taa.diag_reject` → taa_out alpha）─────────────────────────
//
// alpha 码（taa.frag.slang，raw8 直出）：0 = 历史原样收下 ｜ 64(0.25) = 被邻域盒裁剪
// （部分保留）｜ 128(0.5) = **塌回邻域盒 centroid = 拒收**（历史贡献变成本帧邻域均值
// ⟹ 输出 ≈ 本帧，α 调多小都没用）｜ 192(0.75) = 重投影出画拒收 ｜ 255 = 本帧无历史。
// 分桶边界取码值中点（raw8 舍入 ±1 吸收）。

struct RejectStats {
    double accepted = 0.0;   ///< 收下
    double clipped = 0.0;    ///< 盒裁剪（部分保留）
    double collapsed = 0.0;  ///< 盒拒收（塌回 centroid）
    double reproject = 0.0;  ///< 重投影出画拒收
    double noHistory = 0.0;  ///< 无历史（historyValid 旁路）
    int frames = 0;
    double rejected() const { return collapsed + reproject; }
};

RejectStats rejectStats(const Group& g)
{
    RejectStats s;
    for (const auto& a : g.alpha) {
        const double n = a.total();
        cv::Mat b;
        // 每帧先算**本帧**的四个累积分位（f0≤f1≤f2≤f3），再作差进桶。
        // ⚠ 别拿累加器作差（实测踩过：`leClipped - s.accepted` 在第 2 帧起减的是「前若干帧
        // 之和」，盒裁剪直接变成 −1388% 这种物理上不可能的数）。
        cv::compare(a, 32, b, cv::CMP_LT);
        const double f0 = static_cast<double>(cv::countNonZero(b)) / n;
        cv::compare(a, 96, b, cv::CMP_LT);
        const double f1 = static_cast<double>(cv::countNonZero(b)) / n;
        cv::compare(a, 160, b, cv::CMP_LT);
        const double f2 = static_cast<double>(cv::countNonZero(b)) / n;
        cv::compare(a, 224, b, cv::CMP_LT);
        const double f3 = static_cast<double>(cv::countNonZero(b)) / n;
        s.accepted += f0;
        s.clipped += f1 - f0;
        s.collapsed += f2 - f1;
        s.reproject += f3 - f2;
        s.noHistory += 1.0 - f3;
        ++s.frames;
    }
    // 自检（判据工具自己要有自检）：五桶是**划分** ⟹ 占比之和必须为 1。
    // 作差写错 / 边界重叠 / 掩膜不是单通道，都会先在这里出声，而不是把 −1388% 打到表里。
    const double nfr = std::max(1, s.frames);
    const double sum = s.accepted + s.clipped + s.collapsed + s.reproject + s.noHistory;
    if (std::abs(sum - nfr) > 1e-9)
        throw std::runtime_error("拒收掩膜分桶不是划分：五桶之和 = " + std::to_string(sum)
            + "（应 = 帧数 " + std::to_string(s.frames) + "）——分桶代码坏了，读数无效");
    const double n = nfr;
    s.accepted /= n;
    s.clipped /= n;
    s.collapsed /= n;
    s.reproject /= n;
    s.noHistory /= n;
    return s;
}

/// 掩膜自检（阳性锚点）：diag_reject 键没生效 = alpha 恒 1.0 = 全桶「无历史」。
/// 历史在热身 16 帧后必然有效 ⟹ noHistory 占比 > 0.5 只可能是门没开，当场停下。
void assertMaskAlive(const std::string& name, const RejectStats& s)
{
    if (s.noHistory > 0.5)
        throw std::runtime_error(name + "：掩膜全桶「无历史」（noHistory = "
            + std::to_string(s.noHistory) + "）——diag_reject 没生效（键名/载荷/alpha 通道"
              "任一环断了都会这样），拒收比例读数无效");
}

double medianOf(std::vector<int> v)
{
    if (v.empty())
        return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 == 1 ? static_cast<double>(v[n / 2]) : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// 全帧线性 L1（对齐代价函数；与掩码无关，避免「掩码依赖对齐、对齐依赖掩码」的循环）。
[[nodiscard]] double frameL1(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat d;
    cv::absdiff(a, b, d);
    return cv::mean(d)(0);
}

/// 逐帧 argmin 对齐结果。
struct Alignment {
    std::vector<int> pairIndex;  ///< 候选第 i 帧 ↔ 参考第 pairIndex[i] 帧
    std::vector<int> offsets;    ///< 逐帧 (j − i)（offset 是它的中位数，halfGap 用它的前后半）
    int offset = 0;              ///< 逐帧 (j−i) 的中位数
    double minMargin = 0.0;      ///< min_i (次优−最优)/最优；≈0 = argmin 在平坦区里跳
    int halfGap = 0;             ///< 前半窗与后半窗偏移中位数之差（抓 burst 内掉帧）
};

Alignment alignSequence(const Group& cand, const Group& ref, int window, bool moving)
{
    Alignment a;
    const int n = static_cast<int>(cand.luma.size());
    const int m = static_cast<int>(ref.luma.size());
    a.pairIndex.assign(static_cast<size_t>(n), 0);
    if (!moving) {  // 静止组：所有参考帧逐位相同 ⟹ argmin 恒平坦，配对无信息，钉 0 并跳过护栏
        for (int i = 0; i < n; ++i)
            a.pairIndex[static_cast<size_t>(i)] = std::min(i, m - 1);
        a.minMargin = std::numeric_limits<double>::quiet_NaN();
        return a;
    }
    std::vector<int> deltaFirstHalf, deltaSecondHalf;
    a.minMargin = std::numeric_limits<double>::max();
    for (int i = 0; i < n; ++i) {
        double best = std::numeric_limits<double>::max();
        double second = std::numeric_limits<double>::max();
        int bestJ = std::min(i, m - 1);
        for (int j = std::max(0, i - window); j <= std::min(m - 1, i + window); ++j) {
            const double c = frameL1(cand.luma[static_cast<size_t>(i)],
                ref.luma[static_cast<size_t>(j)]);
            if (c < best) {
                second = best;
                best = c;
                bestJ = j;
            }
            else if (c < second) {
                second = c;
            }
        }
        a.pairIndex[static_cast<size_t>(i)] = bestJ;
        const double margin = (second - best) / std::max(best, 1e-12);
        a.minMargin = std::min(a.minMargin, margin);
        a.offsets.push_back(bestJ - i);
        (i < n / 2 ? deltaFirstHalf : deltaSecondHalf).push_back(bestJ - i);
    }
    a.offset = static_cast<int>(std::lround(medianOf(a.offsets)));
    a.halfGap = static_cast<int>(std::lround(
        std::abs(medianOf(deltaFirstHalf) - medianOf(deltaSecondHalf))));
    return a;
}

/// 边缘带上的偏差读数（全部线性域）。
struct Deviation {
    double devEdge = 0.0;     ///< 边缘带上 mean|候选 − 参考|（主指标）
    double devFlat = 0.0;     ///< 平坦带同口径（对照：公共偏差地板 + 非边缘误差）
    double fracVisible = 0.0; ///< 边缘带上偏差 > τ（1 显示 LSB）的像素占比（跨帧均值）
    double gradRatio = 0.0;   ///< 边缘梯度能量 候选/参考（机制判读：>1 假边、<1 摊平）
    int64_t edgePixels = 0;
};

Deviation measureDeviation(const std::vector<cv::Mat>& cand, const Group& ref,
    const Alignment& al, const cv::Mat& edgeMask, const cv::Mat& flatMask, double tau)
{
    Deviation d;
    d.edgePixels = cv::countNonZero(edgeMask);
    double sumEdge = 0.0, sumFlat = 0.0, sumFrac = 0.0, sumGrad = 0.0;
    int frames = 0;
    for (size_t i = 0; i < cand.size() && i < ref.luma.size(); ++i) {
        const cv::Mat& r = ref.luma[static_cast<size_t>(al.pairIndex[i])];
        cv::Mat diff;
        cv::absdiff(cand[i], r, diff);
        sumEdge += cv::mean(diff, edgeMask)(0);
        sumFlat += cv::mean(diff, flatMask)(0);
        cv::Mat over;
        cv::compare(diff, tau, over, cv::CMP_GT);
        cv::bitwise_and(over, edgeMask, over);
        sumFrac += static_cast<double>(cv::countNonZero(over))
            / static_cast<double>(std::max<int64_t>(1, d.edgePixels));
        const double gc = gradEnergy(cand[i], edgeMask);
        const double gr = gradEnergy(r, edgeMask);
        sumGrad += gc / std::max(gr, 1e-12);
        ++frames;
    }
    const double n = std::max(1, frames);
    d.devEdge = sumEdge / n;
    d.devFlat = sumFlat / n;
    d.fracVisible = sumFrac / n;
    d.gradRatio = sumGrad / n;
    return d;
}

/// 离散抖动相位拟合（M2 去抖 / M1 自证）。零自由参数：p 只有 16 个取值。
struct PhaseFit {
    int bestPhase = 0;
    double bestDev = 0.0;
    double rawDev = 0.0;  ///< 不去抖（等价 p 使位移为 0）的读数，用来打印「去抖赚了多少」
    std::vector<cv::Mat> warped;  ///< 最优相位下的去抖序列（供最终读数用）
};

PhaseFit fitJitterPhase(const Group& g, const Group& ref, const Alignment& al,
    const cv::Mat& edgeMask, double jitterScale)
{
    PhaseFit fit;
    const int n = static_cast<int>(g.luma.size());
    auto devOf = [&](const std::vector<cv::Mat>& seq) {
        double s = 0.0;
        int k = 0;
        for (int i = 0; i < n; ++i) {
            cv::Mat d;
            cv::absdiff(seq[static_cast<size_t>(i)],
                ref.luma[static_cast<size_t>(al.pairIndex[i])], d);
            s += cv::mean(d, edgeMask)(0);
            ++k;
        }
        return s / std::max(1, k);
    };
    fit.rawDev = devOf(g.luma);
    fit.bestDev = fit.rawDev;
    for (uint32_t p = 0; p < kJitterPeriod; ++p) {
        std::vector<cv::Mat> seq;
        seq.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            const cv::Point2d c = contentShiftPx(static_cast<uint32_t>((p + i) % kJitterPeriod),
                jitterScale);
            // 候选 = F(x − c) ⟹ 复原 F̂(x) = cand(x + c) ⟹ 平移量取 −c（shiftLuma 的符号约定
            // 是 dst(x)=src(x−d)，故 d = −c）。
            seq.push_back(shiftLuma(g.luma[static_cast<size_t>(i)], {-c.x, -c.y}));
        }
        const double dev = devOf(seq);
        if (dev < fit.bestDev) {
            fit.bestDev = dev;
            fit.bestPhase = static_cast<int>(p);
            fit.warped = std::move(seq);
        }
    }
    return fit;
}

std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}
std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}
std::string pct2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f%%", v * 100.0);
    return b;
}

}  // namespace

int cmdTemporalMotion(const std::string& cameraName, const std::string& motion,
    double angleDeg, int burst, int warm, int warmStill, double edgePercentile)
{
    const BistroCam* cam = nullptr;
    for (const auto& c : kBistroCams)
        if (cameraName == c.name)
            cam = &c;
    if (!cam) {
        std::fprintf(stderr, "机位不存在: %s（可选 doorframe / wetground / doorpanel）\n",
            cameraName.c_str());
        return 2;
    }
    if (motion != "camera" && motion != "spin") {
        std::fprintf(stderr, "--motion 只认 camera / spin（收到 %s）\n", motion.c_str());
        return 2;
    }
    if (burst < 4)
        throw std::runtime_error("--burst 至少 4（对齐护栏要分前后半窗）");
    if (edgePercentile <= 0.0 || edgePercentile >= 1.0)
        throw std::runtime_error("--edge-pct 必须在 (0,1)，它是分位不是百分比");

    const bool cameraMode = motion == "camera";
    const double angle = angleDeg * 3.14159265358979323846 / 180.0;
    const std::string base =
        projectRoot() + "/build/imgqa_motion_" + (cameraMode ? "cam_" : "spin_") + cameraName;
    const std::string pre = cameraName.substr(0, 2) + (cameraMode ? "c_" : "s_");

    std::printf("=== temporal-motion：%s运动下 TAA 的抗锯齿效果（机位 %s）===\n",
        cameraMode ? "相机" : "场景 spin（相机静止）", cameraName.c_str());
    std::printf("  配置 = 交付 configs/（deferred + env/skybox/IBL/AO/shadow 全开，红线 3）\n");
    std::printf("  运动 = %s：angle = %.4f°/帧 × 帧号（纯函数），绕 up 轴 yaw\n",
        cameraMode ? "相机帧驱动 yaw（camera.lua frame_yaw，= 用户报障的运动源）"
                   : "场景帧驱动 spin（scene.lua spin_frame_angle，相机不动 = 债-49 鬼影仪器）",
        angleDeg);
    std::printf("  参考 = 同机位同帧序列 %dx%d + TAA 关 → 线性域面积平均降到 %dx%d\n",
        kRefSize[0], kRefSize[1], kSize[0], kSize[1]);
    std::printf("  连拍 = %d 帧（/shotburst 逐帧连续）；热身 = 运动组 %d 帧 / 静止组 %d 帧"
                "（空转不写盘；运动格**不适用**「≥6/α 收敛」——历史本来就在被内容变化刷新）\n",
        burst, warm, warmStill);
    if (cameraMode)
        std::printf("  M3 = reprojection=false（关 prevViewProj 重投影本身）；M3x = "
                    "motion_compensation=false（恒等对照，应 ≈ M1）\n\n");
    else
        std::printf("  M3 = motion_compensation=false（关 prevSpin 场景旋转补偿）\n"
                    "  ⚠ spin 转**场景**、相机不动 ⟹ prevViewProj 恒定 ⟹ 相机重投影路不被走到\n\n");

    // ── 采集（每组一个进程；串行，单实例纪律）────────────────────────────────
    std::printf("── 采集 ──\n");
    std::vector<std::unique_ptr<Group>> slots;
    auto run = [&](const std::string& name, const std::string& sub, const Spec& sp) -> Group& {
        slots.push_back(std::make_unique<Group>(
            collectGroup(base + "_" + sub, name, pre + sub, *cam, sp, burst)));
        return *slots.back();
    };
    Spec m0;
    m0.taa = false;
    m0.motionAngle = angle;
    m0.cameraMotion = cameraMode;
    m0.warm = warm;
    Spec m1 = m0;
    m1.taa = true;
    Spec m2 = m1;
    m2.baseSpace = false;
    Spec m3 = m1;
    if (cameraMode)
        m3.reprojection = false;  // 相机模式：关 prevViewProj 重投影本身
    else
        m3.motionCompensation = false;  // spin 模式：关 prevSpin 场景旋转补偿
    Spec m3x = m1;
    m3x.motionCompensation = false;  // 恒等对照（仅相机模式）：按论证必须 ≈ M1
    Spec m4 = m1;
    m4.motionAngle = 0.0;
    m4.warm = warmStill;
    Spec refm = m0;
    refm.refScale = 1;
    Spec refs = m4;
    refs.taa = false;
    refs.refScale = 1;
    Spec m1m = m1;
    m1m.diagReject = true;
    m1m.attachment = "taa_out";
    Spec m4m = m4;
    m4m.diagReject = true;
    m4m.attachment = "taa_out";

    Group& gM0 = run("M0 TAA关 + 运动", "m0", m0);
    Group& gM1 = run("M1 TAA开 + 运动(交付态)", "m1", m1);
    Group& gM2 = run("M2 M1 + base_space=false", "m2", m2);
    Group& gM3 = run(cameraMode ? "M3 M1 + 关重投影" : "M3 M1 + 关场景补偿", "m3", m3);
    std::unique_ptr<Group> gM3xHolder;
    Group* gM3x = nullptr;
    if (cameraMode) {
        gM3xHolder = std::make_unique<Group>(run("M3x M1 + 关spin补偿(恒等对照)", "m3x", m3x));
        gM3x = gM3xHolder.get();
    }
    Group& gM4 = run("M4 TAA开 + 静止", "m4", m4);
    Group& gRefm = run("REFm 超采样参考(运动)", "refm", refm);
    Group& gRefs = run("REFs 超采样参考(静止)", "refs", refs);
    Group& gM1m = run("M1m M1 + 拒收掩膜", "m1m", m1m);
    Group& gM4m = run("M4m M4 + 拒收掩膜", "m4m", m4m);

    // ── 守卫 1：运动激励在场（`temporal-motion-excitation`）───────────────────
    // frame_yaw / spin_frame_angle 的键名拼错在 lua 侧**静默**（未知键不报错）⟹ 各组会
    // 全跑在静止场景上，而 M0≈M1≈M4 看起来完全像一个「TAA 无效」的结论。唯一能抓它的是
    // 「画面到底动没动」——探针必须选 **TAA 关**的组（M0/REFm）：TAA 开的组即使画面静止，
    // 抖动本身就让相邻帧不同，byteMaxAdjacent > 0 是恒真的（阳性锚点必须落在
    // 「被测机制从未生效时这条会绿吗」= 不会 的组上）。
    std::printf("\n── 守卫 ──\n");
    std::vector<Verdict> vs;
    {
        std::string obs;
        for (const auto* g : {&gM0, &gRefm})
            obs += g->name.substr(0, 2) + " 相邻帧最大字节差 " + std::to_string(
                       static_cast<int>(g->byteMaxAdjacent)) + "  ";
        vs.push_back(verdictFor("temporal-motion-excitation",
            gM0.byteMaxAdjacent > 0.0 && gRefm.byteMaxAdjacent > 0.0,
            obs + "（M0/REFm = TAA 关组，必须 > 0；= 0 ⟺ frame_yaw/spin_frame_angle 没生效）"));
        std::cout << "  " << vs.back().line() << "\n";
        if (!vs.back().ok) {
            std::printf("  ⟹ 立刻停下：运动激励不在场，整张 A/B 表会跑在静止场景上。\n");
            logCall("temporal-motion",
                {{"camera", cameraName}, {"motion", motion}, {"angle", d4(angle)}}, vs);
            return 2;
        }
    }

    // ── 历史拒收掩膜（`taa.diag_reject` → taa_out alpha；RGB 逐位 = M1/M4）────
    std::printf("\n── 历史拒收掩膜（taa_out alpha；0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 / "
                "0.75 重投影拒收 / 1 无历史）──\n");
    const RejectStats rsM1 = rejectStats(gM1m);
    const RejectStats rsM4 = rejectStats(gM4m);
    assertMaskAlive("M1m", rsM1);
    assertMaskAlive("M4m", rsM4);
    auto echoMask = [&](const char* tag, const RejectStats& s) {
        std::printf("  %-3s 收下 %s · 盒裁剪 %s · **盒拒收 %s** · 重投影拒收 %s · 无历史 %s"
                    "  ⟹ 历史被拒合计 %s\n",
            tag, pct2(s.accepted).c_str(), pct2(s.clipped).c_str(), pct2(s.collapsed).c_str(),
            pct2(s.reproject).c_str(), pct2(s.noHistory).c_str(), pct2(s.rejected()).c_str());
    };
    echoMask("M1", rsM1);
    echoMask("M4", rsM4);
    std::printf("  ⚠ 读法：M1 的边缘像素若大多住在「盒拒收」桶 ⟹ 历史根本没进混合，"
                "α（feedback/static_boost）调多小都没用——「窗棂残余」与「相机运动失效」同判\n");

    // ── 区域与阈值（口径来自 REF，不来自候选 ⟹ 各候选共用一把尺子）──────────
    // 边缘带 = 参考线性 luma 梯度 ≥ p90（top 10%）。为什么用分位而不是绝对阈值：
    // 绝对阈值是「拍出来的数」，而 bistro 的梯度跨机位跨曝光量级；p90 让「边缘带」=
    // 「这个画面里梯度最高的那 10% 像素」，与场景亮度尺度无关（同 phase13 §5.1b 口径）。
    // 为什么在**降采样后的参考**上算：候选住在 720p 网格，边缘必须在同一网格上定义。
    cv::Mat gx, gy, grad;
    cv::Sobel(gRefm.luma.front(), gx, CV_64F, 1, 0, 1);
    cv::Sobel(gRefm.luma.front(), gy, CV_64F, 0, 1, 1);
    grad = cv::abs(gx) + cv::abs(gy);  // 与 gradEnergy 同式（同一算子只许一份实现）
    auto gv = valuesInMask(grad, {});
    std::sort(gv.begin(), gv.end());
    const double tauEdge = percentileOf(gv, edgePercentile);
    const double tauFlat = percentileOf(gv, 0.50);
    cv::Mat edgeMask, flatMask;
    cv::compare(grad, tauEdge, edgeMask, cv::CMP_GE);
    cv::compare(grad, tauFlat, flatMask, cv::CMP_LT);

    // τ（可见性阈值）= 参考白电平处 1 个显示 LSB 的线性跨度（闭式，零自由参数）。
    auto rv = valuesInMask(gRefm.luma.front(), edgeMask);
    std::sort(rv.begin(), rv.end());
    const double white = percentileOf(rv, 0.99);
    const double tau = lsbLinearAtLuma(white);

    std::printf("\n── 区域与阈值 ──\n");
    std::printf("  边缘带 = 参考梯度 ≥ p%.0f（%lld px，占 %.1f%%）；平坦带 = 梯度 ≤ p50\n",
        edgePercentile * 100.0, static_cast<long long>(cv::countNonZero(edgeMask)),
        100.0 * cv::countNonZero(edgeMask) / static_cast<double>(grad.total()));
    std::printf("  τ = 白电平(p99=%.5f) 处 1 显示 LSB = %s（闭式 lsbLinearAtLuma，非拍的阈值）\n",
        white, d4(tau).c_str());
    std::printf("  ⚠ AO 是屏幕空间采样 ⟹ 1440p 参考与 720p 候选的 AO 细节本就不同 ⟹ 各组共用"
                "一个偏差地板，**只出比值结论**\n");

    // ── 对齐 ────────────────────────────────────────────────────────────────
    const int window = 8;
    Alignment aM0 = alignSequence(gM0, gRefm, window, true);
    Alignment aM1 = alignSequence(gM1, gRefm, window, true);
    Alignment aM2 = alignSequence(gM2, gRefm, window, true);
    Alignment aM3 = alignSequence(gM3, gRefm, window, true);
    Alignment aM3x = cameraMode ? alignSequence(*gM3x, gRefm, window, true)
                                : alignSequence(gM4, gRefs, window, false);
    Alignment aM4 = alignSequence(gM4, gRefs, window, false);

    std::printf("\n── 跨进程帧对齐（窗口 ±%d）──\n", window);
    auto echoAlign = [&](const char* tag, const Alignment& a, bool moving) {
        if (!moving) {
            std::printf("  %-4s 不跑 argmin：静止组所有参考帧逐位相同 ⟹ 代价面恒平坦，"
                        "配对无信息（判它 = 判一个没有信息的量）\n", tag);
            return;
        }
        std::printf("  %-4s 偏移 %+d  argmin 余量 %.4f  前后半窗差 %d\n", tag, a.offset,
            a.minMargin, a.halfGap);
    };
    echoAlign("M0", aM0, true);
    echoAlign("M1", aM1, true);
    echoAlign("M2", aM2, true);
    echoAlign("M3", aM3, true);
    if (cameraMode)
        echoAlign("M3x", aM3x, true);
    echoAlign("M4", aM4, false);
    {
        double worst = std::min({aM0.minMargin, aM1.minMargin, aM2.minMargin, aM3.minMargin});
        int gap = std::max({aM0.halfGap, aM1.halfGap, aM2.halfGap, aM3.halfGap});
        if (cameraMode) {
            worst = std::min(worst, aM3x.minMargin);
            gap = std::max(gap, aM3x.halfGap);
        }
        vs.push_back(verdictFor("temporal-motion-alignment", worst >= 0.02 && gap == 0,
               "最小 argmin 余量 " + d4(worst) + "（下限 0.02）· 最大前后半窗差 "
                   + std::to_string(gap) + "（要求 0）· 偏移 M0/M1/M2/M3 = "
                   + std::to_string(aM0.offset) + "/" + std::to_string(aM1.offset) + "/"
                   + std::to_string(aM2.offset) + "/" + std::to_string(aM3.offset)));
        std::cout << "  " << vs.back().line() << "\n";
    }

    // ── M2 去抖 + M1 自证（离散 16 相位 argmin，零自由参数）────────────────────
    PhaseFit fM2 = fitJitterPhase(gM2, gRefm, aM2, edgeMask, 1.0);
    PhaseFit fM1 = fitJitterPhase(gM1, gRefm, aM1, edgeMask, 1.0);
    std::printf("\n── 抖动相位拟合（p ∈ [0,16) argmin）──\n");
    std::printf("  M2 不去抖 %s → 最优 p=%2d 去抖后 %s（赚 %.1f%%）\n", d4(fM2.rawDev).c_str(),
        fM2.bestPhase, d4(fM2.bestDev).c_str(), 100.0 * (1.0 - fM2.bestDev / fM2.rawDev));
    std::printf("  M1 不去抖 %s → 最优 p=%2d 去抖后 %s（赚 %.1f%%）"
                "  ← 交付态应接近 0（base_space=true 已把输出搬回基网格）\n",
        d4(fM1.rawDev).c_str(), fM1.bestPhase, d4(fM1.bestDev).c_str(),
        100.0 * (1.0 - fM1.bestDev / fM1.rawDev));

    // ── 读数 ────────────────────────────────────────────────────────────────
    struct Row {
        const char* tag;
        const char* desc;
        Deviation dev;
        std::string note;
    };
    auto measure = [&](const std::vector<cv::Mat>& seq, const Group& ref, const Alignment& a) {
        return measureDeviation(seq, ref, a, edgeMask, flatMask, tau);
    };
    std::vector<Row> rows;
    rows.push_back(Row{"M0", "TAA 关 + 运动（地板）", measure(gM0.luma, gRefm, aM0), ""});
    rows.push_back(Row{"M1", "TAA 开 + 运动（交付态）", measure(gM1.luma, gRefm, aM1), ""});
    rows.push_back(Row{"M2", "M1 + base_space=false",
        measure(fM2.warped.empty() ? gM2.luma : fM2.warped, gRefm, aM2),
        "去抖修正（p=" + std::to_string(fM2.bestPhase) + "）"});
    rows.push_back(Row{"M3",
        cameraMode ? "M1 + 关重投影（消融）" : "M1 + 关场景补偿（消融）",
        measure(gM3.luma, gRefm, aM3), ""});
    rows.push_back(Row{"M4", "TAA 开 + 静止（参照）", measure(gM4.luma, gRefs, aM4), "对 REFs"});
    rows.push_back(
        Row{"M2x", "M2 未去抖（对照）", measure(gM2.luma, gRefm, aM2), "含 ≤0.71px 系统位移"});
    if (cameraMode)
        rows.push_back(Row{"M3x", "M1 + 关spin补偿（恒等对照）",
            measure(gM3x->luma, gRefm, aM3x), "按论证应 ≈ M1"});

    const double devM0 = rows[0].dev.devEdge;
    const double devM1 = rows[1].dev.devEdge;
    const double devM3 = rows[3].dev.devEdge;
    const double devM4 = rows[4].dev.devEdge;
    const double devM3x = cameraMode ? rows[6].dev.devEdge : 0.0;

    std::printf("\n── A/B 表（线性域；边缘带 = 梯度 top10%%）──\n");
    std::printf("  %-4s %-28s %10s %8s %9s %10s %8s  %s\n", "组", "配置", "边缘偏差",
        "(÷LSB)", "可见边缘%", "平坦偏差", "梯度比", "备注");
    for (const auto& r : rows) {
        std::printf("  %-4s %-28s %10s %8s %8.2f%% %10s %8.3f  %s\n", r.tag, r.desc,
            d4(r.dev.devEdge).c_str(), d2(r.dev.devEdge / tau).c_str(),
            r.dev.fracVisible * 100.0, d4(r.dev.devFlat).c_str(), r.dev.gradRatio,
            r.note.c_str());
    }
    std::printf("  （边缘像素 %lld；平坦带同 τ，读的是「非边缘有没有被牵连」= 公共偏差地板）\n",
        static_cast<long long>(rows[0].dev.edgePixels));

    // ── 守卫 2：消融组必须显著变差（量具敏感性阳性锚点）───────────────────────
    {
        const double ratio = devM3 / std::max(devM1, 1e-12);
        vs.push_back(verdictFor("temporal-motion-ablation", ratio >= 1.5,
            "dev(M3 关重投影)/dev(M1) = " + d2(ratio) + "（下限 1.5）· dev M3 = " + d4(devM3)
                + "，M1 = " + d4(devM1)
                + "；挂零 = 对齐/掩码/域坏了，整张表作废，不用它下画质结论"));
        std::cout << "  " << vs.back().line() << "\n";
    }

    // ── 判决 ────────────────────────────────────────────────────────────────
    const double R = devM1 / std::max(devM0, 1e-12);
    const double Rbest = devM4 / std::max(devM0, 1e-12);
    const double GR0 = rows[0].dev.gradRatio, GR1 = rows[1].dev.gradRatio;
    std::printf("\n── 判决 ──\n");
    std::printf("  R = dev(M1)/dev(M0) = %s（判决线：>1.05 主动变差 · 0.90–1.05 no-op · "
                "≤0.70 生效 · 之间部分生效）\n",
        d2(R).c_str());
    std::printf("  参照 dev(M4)/dev(M0) = %s（静止收敛态 = 运动态的**可达下界**语义，非等号）\n",
        d2(Rbest).c_str());
    std::printf("  梯度比 M0 %.3f · M1 %.3f · M4 %.3f（>1 = 多假边/锯齿；<1 = 边缘被摊平）\n",
        GR0, GR1, rows[4].dev.gradRatio);
    std::string verdict;
    if (R > 1.05)
        // R > 1 不是「没作用」而是「反作用」：偏差比 TAA 关还大，同时梯度比在降（= 变糊）。
        // 两者同向 ⟹ 时域历史把**上一帧的位置**抹进了本帧（拖影），既没对齐参考也没消锯齿。
        verdict = "运动下 TAA **主动变差**（不是 no-op）：边缘偏差是 TAA 关的 " + d2(R)
            + "×（更偏离超采样参考），而梯度比 " + d2(GR0) + " → " + d2(GR1)
            + "（确实在变糊）——历史把上一帧的位置抹进本帧 = **拖影**，糊掉锯齿的同时"
              "引入了更大的位置误差";
    else if (R >= 0.90)
        verdict = "运动下 TAA **等于没抗锯齿**（no-op）：边缘偏差与 TAA 关之比 " + d2(R)
            + "，梯度比也几乎没动（" + d2(GR0) + " → " + d2(GR1) + "）——不是「变差了」，是"
              "**什么都没发生**";
    else if (R <= 0.70)
        verdict = "运动下 TAA **确实生效但受固有边界限制**：边缘偏差降到地板的 " + d2(R)
            + "×，但仍显著高于静止收敛参照（M1 " + d4(devM1) + " vs M4 " + d4(devM4)
            + "）——运动本身是误差源（可用累积窗被内容变化刷新 = 天然短），"
              "**这不是 bug，是固有代价**";
    else
        verdict = "运动下 TAA **部分生效**：边缘偏差降到地板的 " + d2(R)
            + "×（判决线 0.70/0.90 之间），需按梯度比与 M4 参照逐条解读，不能一句话定性";
    std::printf("\n  ★ %s\n", verdict.c_str());
    // 消融对照的结论**由比值算出来**，不写死（写死过一次，读起来像"永远起作用"）。
    const double ablationRatio = devM3 / std::max(devM1, 1e-12);
    std::printf("  ★ 支撑对照：M3（关重投影）= %s，M3/M1 = %s ⟹ 重投影链路**%s**"
                "（阈值 1.20：关掉后偏差没涨 = 链路没在读数里起作用）\n",
        d4(devM3).c_str(), d2(ablationRatio).c_str(),
        ablationRatio > 1.20 ? "确实在读数里起作用" : "**没**起作用");
    if (cameraMode)
        std::printf("  ★ 恒等对照：M3x（关 spin 补偿）= %s，M3x/M1 = %s（按论证应 ≈ 1.00："
                    "spin 补偿矩阵在相机运动下是恒等阵）\n",
            d4(devM3x).c_str(), d2(devM3x / std::max(devM1, 1e-12)).c_str());
    std::printf("  ★ 历史被拒合计：M1 %s · M4 %s（掩膜 = taa_out alpha，RGB 逐位 = M1/M4；"
                "「拒」= 盒拒收 + 重投影出画，**不含**盒裁剪 M1 %s / M4 %s = 部分保留）\n",
        pct2(rsM1.rejected()).c_str(), pct2(rsM4.rejected()).c_str(),
        pct2(rsM1.clipped).c_str(), pct2(rsM4.clipped).c_str());
    std::printf("  ★ 口径边界：AO 的 1440p/720p 差异是各组共用的偏差地板，只出比值结论；"
                "spin 模式（--motion spin）另量「静止机位 + 动物」，两者不可互推\n");

    logCall("temporal-motion",
        {{"camera", cameraName}, {"motion", motion},
            {"angleDeg", std::to_string(angleDeg)}, {"burst", std::to_string(burst)},
            {"warm", std::to_string(warm)}, {"warmStill", std::to_string(warmStill)},
            {"edgePct", std::to_string(edgePercentile)}, {"R", d4(R)},
            {"R_still_ref", d4(Rbest)}, {"rejectM1", d4(rsM1.rejected())},
            {"rejectM4", d4(rsM4.rejected())}},
        vs);
    bool allOk = true;
    for (const auto& v : vs)
        allOk &= v.ok;
    return allOk ? 0 : 1;
}

}  // namespace RGL::imgqa
