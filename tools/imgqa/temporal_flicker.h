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
    /// ── 以下两张 = 「亮度 × 梯度」分带的**尺子**（`wantBands=true` 才算，`temporal-homography`
    /// 的 `--bands` 是唯一消费者；`temporal-flicker` 不开 = 逐位今天）────────────────────
    /// 口径与 `temporal_residual.cpp` 的 bistro 分带**逐字相同**（亮度轴 = 显示域字节亮度的
    /// 多帧均值，梯度轴 = `sobelAbsGrad` 于线性 luma）。**唯一**被迫的差别：静止态的梯度算在
    /// 「相位平均图」上（静止机位下那张图是干净的），移动机位**没有**这张干净图——连拍平均会把
    /// 边缘糊掉（扫速 × 帧数的运动模糊），故梯度轴取**跨帧最大**：移动时「这个像素在边缘带上」
    /// 的正确含义是「burst 期间有边缘扫过它」。
    cv::Mat byteMean; ///< 显示域字节亮度（Rec.601 灰）的多帧均值，CV_64FC1，0..255
    cv::Mat byteMax;  ///< 同上取跨帧**最大**（敏感性口径，理由见 temporal_residual.h）
    cv::Mat gradMax;  ///< 跨帧最大的 |Sobel_x|+|Sobel_y|（线性 luma）= 梯度轴的尺子
    /// ── 中点姿态的 objectID（材质归因用；`wantBands=true` 且 bistro 机位才采）───────────
    /// 为什么能只拍一张：材质归因要的是「这段 burst 期间这块屏幕**是什么材质**」，而 burst
    /// 总位移只有 扫速 × 帧数（0.5 px/帧 × 16 帧 = 8 px），与分带尺子本身的口径（byteMean 是
    /// 同一段的均值、gradMax 是同一段的跨帧最大）**同一量级**。逐帧配 ID 反而要引入
    /// 「/shot 落在第几帧」这个本工具无法严格控制的量。
    /// 落位精度**实测回显**（yawErrPx），不靠推断：先标定一次 /shot 会推进多少 yaw，再预补偿。
    cv::Mat ids;        ///< CV_32SC1；空 = 未采（合成靶没有 glb，材质名无从映射）
    double yawErrPx = 0.0;  ///< objectID 实际落位姿态 与 中点姿态 的差（像面 px）
    int idBestFrame = -1;   ///< 同姿态补拍的**彩色**帧在 burst 里最接近第几帧（自证落位）
    double idBestDiff = 0.0;///< 与该帧的平均 |Δ|（线性 luma）；≈0 = 姿态逐位复现
};

/**
 * 一条闪烁臂（批 14.5 加 FSR 用）。
 *
 * 既有两条（TAA 关 / TAA 开）的 tag 与目录后缀**逐字不变** ⟹ 已发表读数可复现。
 * `renderScale` / `fsr*` 只改**渲染分辨率与升频器**；运动激励、ROI、帧数一律不动——
 * 换激励就是换口径，两条轴就没法比。
 */
struct FlickerArm {
    const char* tag;       ///< 采集目录后缀（`imgqa_flicker_<tag>`）
    const char* name;      ///< 表内显示名
    bool taa = false;
    double renderScale = 1.0;  ///< 1.0 = 不写键 = 原生
    bool fsrUpscaler = false;  ///< 自动连带 `velocity_field`（FSR 的前置）
    bool fsrTemporal = false;
    bool fsrJitter = false;
    /// 🔴 **工作点必须与鬼影轴对齐**，否则两条轴量的是两个配置（首轮就踩在这里）：
    /// 本仪器历史上恒用 `kTaaFeedback = 0.12`，而鬼影臂与交付配置都是 `α = 0.01`
    /// ——累积窗差 12 倍，闪烁自然差一截，不写这两个字段就会把 TAA 的闪烁**低估**、
    /// 把 FSR 的优势**高估**。
    double feedback = 0.12;
    double clipMode = 0.0;  ///< 0 = 不写键 = 引擎默认硬盒；11 = 交付；13 = 债-69 修法臂
};

/**
 * 起引擎采一段连拍（`temporal-flicker` 与 `temporal-homography` **共用同一个采集口**——
 * 两条判据看的是同一种激励，写两份采集代码迟早漂）。
 * @param edgeTarget true = aa_target 合成靶（闭式适用）；false = 交付配置 + bistro 机位
 * @param cam        edgeTarget = false 时给（坐标 = 仓库资产 kBistroCams）
 * @param yawRadPerFrame camera.lua `frame_yaw`（rad/帧，原地绕 up 轴 yaw）
 * @param kernel     scene.lua `taa.input_kernel`（批 13.8 输入侧重建核：0 双线性 / 1 CR 4×4 /
 *                   2 Lanczos3 6×6）。默认 0 = **不写键** = 引擎默认 = 已发表读数的口径
 * @param wantBands  true = 顺带算「亮度 × 梯度」分带的尺子（`byteMean`/`byteMax`/`gradMax`，
 *                   口径见 `FlickerSeries` 注释）。默认 false = 不算 = `temporal-flicker` 逐位今天
 * @param arm        非空 = 按该臂覆写渲染分辨率与升频器（批 14.5 闪烁轴）。默认 null = 不动
 */
[[nodiscard]] FlickerSeries captureFlickerSeries(const std::string& dir, const std::string& name,
    const std::string& tag, bool edgeTarget, const BistroCam* cam, bool taaOn,
    double yawRadPerFrame, int frames, double kernel = 0.0, bool wantBands = false,
    bool wantObjectId = false, double envIntensity = 1.0, double aoStrength = 0.5,
    bool overrideEnv = false, bool overrideAo = false, const FlickerArm* arm = nullptr);

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
 * @param kernel 输入侧重建核（`taa.input_kernel`，批 13.8）：0 双线性（默认 = 已发表口径）/
 *               1 CR 4×4 / 2 Lanczos3 6×6。**只作用于 TAA 开那一组**（TAA 关 = 没有 taa pass，
 *               键无消费者 ⟹ 它就是本判据的闭式刻度锚点，不随核变）
 */
int cmdTemporalFlicker(const std::string& target, double sweepPxPerFrame, int frames,
    double halfWidth, double rowFrac, bool verbose, double kernel = 0.0, bool fsr = false);

}  // namespace RGL::imgqa
