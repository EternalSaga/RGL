#pragma once
//
// temporal_swim.h —— `taa-swim` 判据的**跨 TU 共享层**（类型 + 平移搜索算子声明 + 定点格式化）
//
//   为什么拆：`temporal.cpp` 到 926 代码行（工具侧上限 500）被文件护栏逼出刀。那个文件里其实
//   住着**两台仪器**，缝按仪器切：
//
//     · 锚点线（`temporal`：静止机位 + jitter 的抑制比）留在 `temporal.cpp`。
//     · 游动线（`taa-swim`：真实场景逐帧差 = 整幅亚像素平移）拆成两个 TU：
//       - `temporal_swim_metric.cpp` = **口径层**——平移搜索与相位/λ 标定的纯算子
//         （jitterDeltaPx / freeShift / lagTranslation / phaseScan / lambdaScan / absMeasure）
//         + 真实场景采集 runSwimSequence。先例 = `shadow_kernel_metric.*` / `diff_struct_metric.*`
//         / `temporal_flicker_metric.*` 那条「口径 vs 仪器」的缝。⚠ freeShift / shiftInto /
//         lagTranslation 的边界处理与同分 tie-break 是判据的一部分；jitterDeltaPx 预测引擎抖动
//         再抖回去（Halton 与周期 = `import taa_jitter` 的引擎真值），都不许"化简"。
//       - `temporal_swim.cpp` = **仪器层**——cmdTaaSwim 与它的阶段函数（采集 → 逐帧对齐/平移
//         搜索 → 游动量读数 → 基空间门对照 → 阳性锚点 → 打印/判决）。打印顺序与判决线 = 契约。
//
//   类型与算子为什么住这里：SwimRun / FreeShift / LagStat / PhaseScan / ScanCurve / AbsStat
//   是口径层与仪器层之间**唯一**的数据通道（口径层填、仪器层读）；d6 / d2 两个定点格式化被
//   两台仪器的判决文案共用——两处各抄一份 = 第二份真相。
//   判决阈值（kEdgeAttributionFloor / kAnchorDx / kBaseSpaceSwimCap）**不在**这里：它们只被
//   仪器层的判决线消费一次，唯一出处就在判决线旁边。
//
//   ⚠ 本头**不是** taa-swim 的对外接口：命令入口 cmdTaaSwim / cmdTemporal 声明在 `imgqa.h`
//   （本仓惯例「实现分散、声明集中」）。这里声明的东西只给 temporal*.cpp 三个 TU 用，别的 TU 别接。
//
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace RGL::imgqa {

// ─────────────────────── 定点格式化（两台仪器共用，唯一出处）───────────────────────

/// 六位小数（判据读数文案的统一精度；进 logCall 与容差理由字符串）
inline std::string d6(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    return b;
}

/// 两位小数（比值 / 百分比文案）
inline std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

// ─────────────────── 口径层类型（算子定义在 metric TU，读数表在仪器层读）───────────────────

struct FreeShift {
    double dx = 0.0, dy = 0.0;  ///< 拟合位移（px）：frame(p) ≈ ref(p + d)，= 内容位移的相反数
    double resid = 0.0;         ///< 拟合位移处的残差（0..255）
    double zero = 0.0;          ///< 不搬的残差（= 两图原始差）
    double peak = 0.0;          ///< 相位相关峰值（0..1，平移越纯越接近 1）
};

/// 一次真实场景连拍。与合成靶的 runSequence 分开：这里**要**真实 bistro（§1.5 的教训 =
/// 判据全跑在 env 关闭的合成靶上，真实场景外推失败）。
///
/// ⚠ **配置与机位（批 13.1e 改）**：基底 = **交付配置 `configs/*.lua` 原样** + **用户机位**。
/// 之前跑的是合成 profile（forward + 无 izb + 默认机位），于是 `temporal-identity` 在
/// 那里绿（残差 0.016/255）而在交付配置 + 用户机位上红（mad 1.688、max 37、10.37% >2LSB）
/// ——**判据量在别的场景上得出的绿不是绿**。交付配置是 deferred + scale 0.01 + izb_shadow
/// + env/AO/shadow 全开，与合成 profile 不是同一个场景，别再拿两边的数互相解释。
struct SwimRun {
    std::vector<cv::Mat> bytes;  ///< 各帧观测字节（中央裁块，8UC3）
    double consecDiff = 0.0;     ///< 相邻帧 |Δ| 均值（0..255；§1.5 表同单位）
    double lagDiff = -1.0;       ///< 同相位（相隔一个抖动周期整数倍）|Δ| 均值；<0 = 序列太短
    double gradEnergy = 0.0;     ///< 线性 luma 梯度能量（§1.5 的 0.02498 同口径）
};

/// 逐 lag 的**平移假设**检验（本量具的核心）。
///
/// 稳态推导：收敛后 out_i(uv) = F(uv − j_i)（F = 基空间累积图，j = 本帧抖动）
/// ⟹ frame_{i+k}(uv) = frame_i(uv − (j_{i+k} − j_i))，即**同一段连拍里的两帧之间是精确平移**。
/// 关键在于 F 在两帧里**同时出现** ⟹ 累积糊度不参与残差，二值覆盖翻转也被一起搬走。
///
/// ⚠ 为什么不能拿「未抖基图」当参考去搬（第一版就是这么瞎的）：基图是**重新光栅化**出来的，
/// 不是基空间图插值出来的——亚像素偏移下边缘的二值覆盖翻转与三次插值给出的中间值根本不是一
/// 回事，这一项压过平移项，实测 T1 组（按构造该命中）搬基图反而**更差** 110.9%。lag 对比把
/// 这一项与糊度一起消掉，是这里唯一量得出「有没有整幅平移」的口径。
struct LagStat {
    int lag = 0;
    double pred = 0.0;    ///< 预测位移模长均值 |j_{i+k} − j_i|（px，Halton 闭式）
    double meas = 0.0;    ///< 实测 |frame_i − frame_{i+k}|（0..255）
    double resid = 0.0;   ///< 把 frame_i 按**预测位移**搬过去后与 frame_{i+k} 的差
    double wrong = 0.0;   ///< 同上但搬**错相位**的位移（相位整体错 8 = 反号）= 反向对照
    int sign = 1;         ///< 命中的符号（−1 = 引擎 y 口径与本工具相反）
};

/// **相位标定**：采集第 0 帧落在引擎抖动序列的哪个相位，是一个未知常数（引擎自己的
/// `frameIndex` 从 0 起算，但预热长度 + RPC 登记时序让它与采集下标不同源；引擎还额外
/// `+1` 跳过全零点）。⚠ `lag = 16 同相位差恰为 0` 那条免费标定**只能验周期，验不出常数
/// 相位偏移**——本轮就是栽在这里：按 λ=1 搬不动，第一反应是「画面没被搬起来」，
/// 实际可能只是**每对帧的预测位移整体错位了一个常数相位**。
/// 于是对 16 个候选相位各算一次「按预测位移搬完之后」的残差（±符号都取小），真相位必须
/// 显著塌下去。这是**零自由参数**的（16 个离散候选，无拟合）。
struct PhaseScan {
    std::vector<double> resid;  ///< resid[φ] = 平均残差（0..255）
    int best = 0;               ///< 残差最小的相位
    double bestVal = 0.0;
    double zero = 0.0;          ///< 「不搬」的基线（= 原始逐帧差）
};

/// 在**给定相位**上沿预测位移做带符号 λ 一维扫描（相位标定通过后才知道幅度对不对）。
struct ScanCurve {
    std::vector<std::pair<double, double>> pts;  ///< (λ, 残差 0..255)，λ 含正负
    double best = 0.0;
    double bestLam = 0.0;
    double zero = 0.0;
};

/// 绝对抖动传递的单帧自由拟合行（= 原 absMeasure 内联 Row，字段逐字）。
struct AbsRow {
    double dx, dy, resid, zero, peak;
};

/// 绝对抖动传递的一组统计（原 cmdTaaSwim 里的内联 AbsStat，提出来跨阶段用）
struct AbsStat {
    double meas = 0.0, pred = 0.0, err = 0.0, rms = 1e30, resid = 0.0, zero = 0.0, peak = 0.0;
    double swing = 0.0, swingMax = 0.0;  ///< 逐帧位移变化 |d_{i+1} − d_i| 的均值 / 最大值（px）
    int ph = 0, sx = 1, sy = 1;
};

/// absMeasure 的产物：逐帧拟合行 + 汇总统计
struct AbsMeasure {
    std::vector<AbsRow> rows;
    AbsStat st;
};

// ─────────────────────── 口径层算子（定义在 `temporal_swim_metric.cpp`）───────────────────────

/// 抖动引起的位移预测（像素，含尺度）——口径推导与标定实测的完整理由见定义处头注释。
std::array<double, 2> jitterDeltaPx(uint32_t frameIndex, double scale);

/// 自由拟合 `frame ≈ shift(ref, d)` 的二维位移（相位相关）——口径与自检读数见定义处头注释。
FreeShift freeShift(const cv::Mat& ref, const cv::Mat& frame);

/// 两帧平均绝对差（观测字节，0..255）。
double meanAbs(const cv::Mat& a, const cv::Mat& b);

/// 一次真实场景连拍（交付配置 + 用户机位）——配置口径的完整理由见定义处头注释。
SwimRun runSwimSequence(const std::string& dir, bool taa, double feedback, double jitterScale,
    int shots, int warm, const std::string& tag, int lag,
    bool baseSpace = false);

/// 逐 lag 的平移假设检验（本量具的核心）——稳态推导见 LagStat 头注释。
LagStat lagTranslation(const std::vector<cv::Mat>& frames, int lag, double jitterScale,
    uint32_t phase);

/// 与基图的逐像素最大偏离（所有帧取最大，观测字节 0..255）。
double maxDistTo(const cv::Mat& ref, const std::vector<cv::Mat>& frames);

/// 取连拍的收敛段（末尾 max(4, N/4) 帧）——为什么恒等式只准读尾段见定义处头注释。
std::vector<cv::Mat> convergedTail(const std::vector<cv::Mat>& frames);

/// 16 个候选常数相位的残差扫描（零自由参数）——为什么必须先于一切按预测位移的表见定义处头注释。
PhaseScan phaseScan(const std::vector<cv::Mat>& frames, double jitterScale);

/// 在给定相位上沿预测位移做带符号 λ 一维扫描（相位标定通过后才知道幅度对不对）。
ScanCurve lambdaScan(const std::vector<cv::Mat>& frames, double jitterScale, uint32_t phase);

/// 绝对口径：逐帧 freeShift + Halton 预测集匹配（16 相位 × 逐轴双符号，tie-break 是读数）。
/// 纯算子不打印；逐帧读数表由仪器层（temporal_swim.cpp）打印。
AbsMeasure absMeasure(const cv::Mat& ref, const std::vector<cv::Mat>& frames, double js);

/// 一组连拍的逐 lag 平移检验读数表（仪器层打印，**打印逐字是契约**）；返回
/// {搬对相位残差/实测, 搬错相位/实测} = 判决线 temporal-swim-discriminative 的两个输入。
/// 声明在此 = selftest 能直接喂合成帧序列核「输出行数不随噪声变」（债-94）。
std::array<double, 2> lagTable(const char* name, const std::vector<cv::Mat>& frames,
    double jitterScale, uint32_t phase);

}  // namespace RGL::imgqa
