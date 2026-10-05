#pragma once
//
// temporal_flicker.h — 运动态时间闪烁的**闭式解**判据（判据编号 temporal-flicker）
//
//   问题：相机匀速扫过一个 1px 边缘时，像素每帧该变多少？
//     · 理想 AA（连续覆盖）：覆盖 = 线性斜坡，每帧变化 = 斜坡斜率 × 扫速 → **小**
//     · 完全无 AA（1 采样）：覆盖 = 阶梯，每帧变化 = **整段跳变**（黑端→白端）
//   两端都是闭式的，于是「实测每帧变化 ÷ 闭式理想」是一条**有绝对刻度**的闪烁增益，
//   不必先跑一遍 TAA 关才知道「不闪」该是多少（这是它相对 temporal-motion 空间代理的增量）。
//
//   与 temporal_residual 的关系：那条是**静态机位**「残差 ÷ 1 LSB」（运动为零，锚 = 量化台阶）；
//   本条是**运动机位**「残差 ÷ 闭式理想运动残差」（锚 = 理想 AA 的运动残差）。锚随扫速走
//   ⇒ 判据**不靠「动得慢」刷分**，不同扫速 / 不同 ROI 之间可比。
//
//   ⚠ 走过的弯路（实测数据在 temporal_flicker.cpp 头注释，登记在此防反复重提）：
//   最初想用「时间轴 FFT 高频带能量比」当标量，被自己的闭式对照否掉——混叠是零阶保持，
//   把锯齿**变成方波**，方波的高频能量并不比锯齿大；且亚像素相位 τ 一换等于换占空比，
//   带比在理想 [0.15, 0.60] 与无 AA [0.00, 0.86] 之间**完全重叠**。FFT 保留为打印诊断。
//
#include "imgqa.h"
#include "temporal_residual.h"

#include <opencv2/core.hpp>

#include <string>
#include <vector>

namespace RGL::imgqa {

/// 一组连拍的采集产物（`temporal-flicker` 与 `temporal-homography` **共用同一个采集口**——
/// 两条判据看的是同一种激励，写两份采集代码迟早漂）。
struct FlickerSeries {
    std::string name;
    cv::Size frameSize;           ///< 引擎**实际**渲染尺寸（自证：不是我以为的尺寸）
    std::vector<cv::Mat> luma;    ///< prepareLinear 后的线性亮度（CV_64FC1），顺序 = 帧序
    double byteMaxAdjacent = 0.0; ///< 相邻帧最大字节差（「相机真动了」的在场性自证）
    double liveFeedback = -1.0;   ///< 引擎自己回显的生效 feedback（不读 lua 写的值）
    uint32_t framePre = 0, framePost = 0;
};

/**
 * 起引擎采一段连拍（热身 → /shotburst）。
 * @param edgeTarget true = aa_target 合成靶（闭式适用）；false = 交付配置 + bistro 机位
 * @param cam        edgeTarget = false 时给（坐标 = 仓库资产 kBistroCams）
 * @param yawRadPerFrame camera.lua `frame_yaw`（rad/帧，原地绕 up 轴 yaw）
 */
[[nodiscard]] FlickerSeries captureFlickerSeries(const std::string& dir, const std::string& name,
    const std::string& tag, bool edgeTarget, const BistroCam* cam, bool taaOn,
    double yawRadPerFrame, int frames);

/// `--target` 解析（两条判据共用）：edge = 合成靶；否则 bistro 机位名。找不到 = 抛。
/// @param edgeTarget [out] true = 合成靶
[[nodiscard]] const BistroCam* resolveFlickerTarget(const std::string& target, bool& edgeTarget);

/// 名义扫速（px/帧，主点处）→ 每帧偏航角（rad/帧）。主点处 dx/dψ = f ⟹ dψ = atan(v/f)。
[[nodiscard]] double sweepToYawRad(double sweepPxPerFrame, float focalPx);

/// 边缘两侧的**ACES 前**线性电平（闭式解的两个端点，由 ROI 自身标定）。
struct FlickerScale {
    float black = 0.0f;  ///< 覆盖 = 0 一侧的线性幅值
    float white = 1.0f;  ///< 覆盖 = 1 一侧的线性幅值
};

/// 显示链复刻：覆盖率 [0,1] 线性插值到 [black, white] → ACES → sRGB 编码 → 8bit 取整 →
/// 解回线性。与引擎交付链同一条（acesTonemap / srgbEncode / srgbDecode 是 imgqa 既有实现），
/// 闭式解因此住在**观测域**里，与像素可比。
[[nodiscard]] float displayChain(float coverage, const FlickerScale& scale);

/// 闭式锚：理想 AA 与完全无 AA 两端各自的「每帧变化」（线性域，观测域取值）。
struct FlickerAnchor {
    float sweepPx = 0.0f;    ///< 边缘在像面上的扫速（px/帧，局部值）
    float idealRho = 0.0f;   ///< 理想 AA：max_c |chain(c+v) − chain(c)|
    float noAaRho = 0.0f;    ///< 完全无 AA：chain(1) − chain(0)（阶梯的整段跳变）
};

/// 闭式解本体。斜坡段等距、平台段恒 0 ⇒ 上确界必落在斜坡段；网格 1/8192 ≪ 扫速。
[[nodiscard]] FlickerAnchor flickerClosedForm(float sweepPx, const FlickerScale& scale);

/// 观测域两端电平 → ACES 前的线性两端（链单调 ⟹ 各自二分反解）。闭式解的**形状**是闭式的，
/// **幅值**是场景的（不是 AA 的），故由 ROI 自身标定；不标定就等于假定黑 = 0、白 = 1。
[[nodiscard]] FlickerScale scaleFromObserved(float darkObserved, float whiteObserved);

/// Hann 窗半谱幅度（自研 O(N²) DFT，N ≤ 256 够用；零新依赖）。已去均值（去 DC）。
[[nodiscard]] std::vector<float> hannHalfSpectrum(const std::vector<float>& signal);

/// 相邻帧逐像素差的 p 分位（线性域）——与 temporal_residual 同口径的残差统计量。
/// frames 各元素是 prepareLinear 后的 luma（CV_64FC1）。
[[nodiscard]] double frameDeltaPercentile(const std::vector<cv::Mat>& frames, const cv::Rect& roi,
    double percentile);

/// 列平均亮度剖面上**最陡梯度**处的亚像素列号（抛物线插值）。用于把 ROI 钉在边缘上。
[[nodiscard]] float steepestEdgeColumn(const std::vector<float>& profile);

/// 局部扫速：x = cx + f·tan(θ − ψ) ⇒ dx/dψ = f·(1 + u²)，u = (x − cx)/f。
/// 主点处退化为 f·dψ；偏离中心更快，不修正在 26° 偏轴处错 ~11%。
[[nodiscard]] float localSweepPxPerFrame(float yawRadPerFrame, float focalPx, float xPx, float cxPx);

/// 垂直 FOV 的像素焦距 f = (h/2)/tan(fov/2)（与 `temporal.cpp` 的 focalPx、
/// `Engine::Camera` 的 aim 约定同一条；ao_noise 的整数像素锚点已验过该换算）。
[[nodiscard]] float verticalFocalPx(int heightPx, float fovDeg);

/**
 * 相机移动下的时间闪烁（闭式解判据）：TAA 开 / 关各采一段连拍，报闪烁增益 G 与归一位置 ζ。
 *
 * @param target "edge" = aa_target 白板左边缘（闭式**适用**，主判据）；或 bistro 机位名
 *               （doorframe / wetground / doorpanel，闭式只作**参考刻度**，ROI 是画面中心窗）
 * @param sweepPxPerFrame 名义扫速（px/帧，主点处；亚像素，(0,1) 开区间）
 * @param frames 连拍帧数（≥16）
 * @param halfWidth ROI 半宽（px）
 * @param rowFrac ROI 半高占画面高的比例
 * @param verbose 打印 ROI 均值时间序列与 Hann 半谱（诊断用）
 */
int cmdTemporalFlicker(const std::string& target, double sweepPxPerFrame, int frames,
    double halfWidth, double rowFrac, bool verbose);

}  // namespace RGL::imgqa
