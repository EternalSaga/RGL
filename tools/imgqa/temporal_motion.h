#pragma once
// temporal-motion 家族**内部头** —— 跨 TU 的类型 / 阈值常量 / 算子声明的唯一出处。
//
// 存在理由 = 护栏逼的拆分（批次 B11）：`temporal_motion.cpp` 曾经 2748 代码行 > 500，
// 里面住着**六台仪器**（temporal-motion / temporal-feedback / temporal-tail spin /
// temporal-tail --motion camera / temporal-tail --motion step / 阶跃 FSR 臂）。缝按仪器切，
// 缝表见 `tools/imgqa/CMakeLists.txt`。
//
// ⚠ 本头只准 temporal_motion*.cpp / temporal_feedback*.cpp / temporal_tail*.cpp /
//   temporal_smear.cpp / temporal_step*.cpp include。格式化助手 `d4`/`d2`/`pct2` 与
//   `temporal_swim.h`、`temporal_residual_metric.h` 里的同名件**格式不同**（本头是
//   `%.6f` / `%.3f` / `%.2f%%`），全部住进 `RGL::imgqa::tmotion` 才互不干扰；
//   **别把它们提回 `RGL::imgqa`**，也别再抄第三份。
// ⚠ 类型同样住 `tmotion`：`Group` 与 `temporal_flicker_metric.h` 的 `Group`、`kSize` 与
//   `temporal.cpp` 等的 `kSize` 在同一个二进制里，提到外层就是 ODR 事故。
// ⚠ 反过来，五个 `cmdTemporal*` 命令入口**必须留在 `RGL::imgqa`**（声明在 imgqa.h，
//   路由在 cli_table.cpp），搬进本命名空间就是「链接期找不到」。

#include "imgqa.h"
#include "temporal_residual.h"
#include "temporal_yaw_remap.h"  // 批 13.14a：相机运动版尾长的**解析**帧间映射（带 pitch）

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

// 引擎真值：Halton 序列与抖动周期（`kJitterPeriod` 必须是它的别名，理由见下面常量处）。
import taa_jitter;

namespace RGL::imgqa::tmotion {

/// 候选/交付分辨率（红线 2）。参考组渲染尺寸 = 2×，降采样后与候选同网格。
constexpr int kSize[2] = {1280, 720};
constexpr int kRefSize[2] = {2560, 1440};

/// 抖动周期 = 引擎真值 `kTaaJitterPeriod` 的**别名**（此前是手抄字面量 16）。
/// 保留本文件的名字是因为判据里它就是「本仪器的离散相位搜索周期」，语义不同；
/// 但值必须同源——周期若不再是 16，M2 离散相位搜索的最优 p 改善会**消失**而不是变好。
constexpr uint32_t kJitterPeriod = kTaaJitterPeriod;

/// 一组的采集规格（每组 = 一个引擎进程；默认值 = 交付态）。
struct Spec {
    /// 抗锯齿选路（批 14.6：原 `bool taa`，与下面的 FSR 两键一起收进这一个槽位）。
    /// Off = 不写键 = 出厂态。臂表因此**写不出**「TAA 与 FSR 时域同开」这类静默换臂的错法。
    Aa aa = Aa::Off;
    double feedback = 0.12;
    double jitterScale = 1.0;
    bool baseSpace = true;
    bool motionCompensation = true;
    bool reprojection = true;
    bool diagReject = false;
    /// 批 13.8 `taa.input_kernel`（输入侧重建核：0 双线性 / 1 CR 4×4 / 2 Lanczos3）。
    /// 默认 0 = 不写键 = 引擎默认 = 已发表 M0…M4 读数的口径。
    double kernel = 0.0;
    double motionAngle = 0.0;  ///< rad/帧（0 = 静止）
    bool cameraMotion = true;  ///< true = 相机 frame_yaw；false = 场景 spin
    int warm = 16;
    int refScale = 0;         ///< 1 = 以 2560×1440 渲染并线性域降采样回 1280×720
    std::string attachment;   ///< 非空 = 连拍具名附件（taa_out alpha 掩膜），不算 luma
    /// >0 = 采集结束后做**同进程 `/param` A/B 探针**（把 feedback 改成这个值再拍一张同相位
    /// 的图）。只许跑在「静止机位 + jitter_scale>0」的组上，理由见采集处注释。
    double probeFeedback = -1.0;
    /// true = 顺带攒「亮度 × 梯度」分带的**亮度轴尺子**（`byteMean`/`byteMax`，显示域字节亮度）。
    /// 批 13.9 唯一消费者 = `temporal-motion` 的 REFm（分带尺子必须来自**参考图**，理由见文件头
    /// 「批 13.9」段）。默认 false = 一字节不多算 = 其余组与 `temporal-feedback` 逐位今天。
    bool wantBytes = false;
    // 批 13.11 曾有 `staticBoostFloor`（`static_boost` 地板）；批 13.14b 删 boost 后长窗档
    // 直接写 `feedback`（= 下面的 feedback 字段本身），字段随之删除。
    /// 批 13.11 **历史裁剪消融臂**（`taa.clip_mode` / `taa.clip_soft`）。默认 0 = 不写键 = 交付态。
    /// 语义 → `rgl/scene_data.cppm` kSceneDataTaaClipControlOffset。
    double clipMode = 0.0;
    double clipSoft = 1.0;
    // ── Phase 14 批 14.5：升频 / FSR 臂（量「闪烁↔鬼影」跷跷板）；批 14.6 三态化 ──
    // 默认全关 = 一个键都不多写 = 现有全部臂逐位不变。语义以 `configs/runtime.lua` 为准。
    // 旧的「FSR 忘开 velocity / temporal 忘开 upscaler / temporal 与 taa 同开」三条错法已
    // **结构上消失**：velocity 由引擎在 aa=fsr 时隐含打开，其余两条被 aa 单槽位排除。
    double renderScale = 1.0;  ///< 1.0 = 不写键 = 交付态
    bool fsrTemporal = false;  ///< 仅 aa == Fsr 时被消费
    bool fsrJitter = false;    ///< FSR 自己的抖动，与 `jitterScale` 是两个独立旋钮
};

/// `taa.feedback` 滑杆的 label（真相源 = `rgl/generated/param_semantics_generated.cppm`
/// 的 `kTaaFeedback`；imgqa 不链 rgl 库，同值重抄，先例 = 本文件的 `kJitterPeriod`）。
/// 漂移由 `Engine::paramValue` 直接抛抓（label 改名 = 读不到 ⟹ 抛，不会静默读到一个别的值）。
constexpr const char* kTaaFeedbackLabel = "TAA 记忆 taa.feedback";

/// 一组采集（luma 一律 CV_64FC1、一律 1280×720、一律线性域；掩膜组只有 alpha）。
struct Group {
    std::string name;
    int warm = 0;
    cv::Size rawSize;   ///< **引擎实际渲染尺寸**（红线 2 自证：参考组必须是 2560×1440）
    cv::Size frameSize; ///< 统计用尺寸（参考组 = 降采样后，恒 1280×720）
    std::vector<cv::Mat> luma;
    std::vector<cv::Mat> alpha; ///< 掩膜组专用：PNG 的 alpha 通道（CV_8UC1）
    /// 分带**亮度轴**尺子（`Spec::wantBytes` 才填）：显示域字节亮度（Rec.601 灰）的跨帧均值 /
    /// 跨帧最大，**已降到 720p 统计网格**（与偏差图同网格才谈得上逐像素分带）。CV_64FC1，0..255。
    cv::Mat byteMean, byteMax;
    /// 第三把亮度轴：`byteMax` 的 **INTER_MAX** 降采样（每格峰值）。存在理由 = 面积平均会把
    /// 1px 高亮线稀释掉（doorframe 实测近削顶档因此空集），敏感性表用它对照「看不见 vs 不闪」。
    cv::Mat bytePeak;
    double byteMaxAdjacent = 0.0;  ///< 相邻帧最大字节差（运动激励在场性自证）
    uint32_t framePre = 0, framePost = 0;
    /// 引擎回显的相机姿态（度）。批 13.14a **相机运动版尾长**的两处都靠它：
    ///  - **pitch/fov** → 解析帧间映射（`frame_yaw` 只动 yaw 不动 pitch ⟹ 旋转轴是世界 Y；
    ///    忽略 pitch，brickfence 每帧 v 方向错 0.485px、pitch −34° 的机位错 1.43px）。
    ///  - **yaw（burst 前后各一次）** → **配对偏移**。
    /// spin 组的三个字段都用不上，但读一次零成本。
    double pitchDegrees = 0.0;
    double fovDegrees = 0.0;
    /// burst **前后各读一次**的 yaw（度）。两者之差 ÷ 帧计数之差 = **实测每帧角速度**，
    /// 两者之中… 配对 = `Δyaw(两组) ÷ 每帧角`。⚠ 不能用 `framePre` 之差：首轮实测它是
    /// −12…−1 而宽扫 argmin 在 0 附近 ⟹ 渲染帧计数器 ≠ yaw 帧索引。
    double yawPre = 0.0, yawPost = 0.0;
    /// **引擎自己回显**的生效 feedback（`GET /state` 读回，不是写进 lua 的值）。
    /// 参数扫描的红线 6：写进 lua 没生效 = 整条曲线是平的，而平曲线恰好会被读成
    /// 「该参数无效」这个结论 ⟹ 每组的生效值必须来自引擎，见 `Engine::paramValue`。
    double liveFeedback = -1.0;
    /// 同进程 `/param` A/B 的字节差（-1 = 本组没做）。**只隔一个 float** 的两张同相位图若
    /// 逐位相同，说明这个滑杆改的是一个没人读的字段 ⟹ 整条扫描曲线无解释力。
    double probeBytes = -1.0;
    double probeChangedPx = 0.0;  ///< 同上取「任一通道不同的像素占比」
};

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

/// 逐帧 argmin 对齐结果。
struct Alignment {
    std::vector<int> pairIndex;  ///< 候选第 i 帧 ↔ 参考第 pairIndex[i] 帧
    std::vector<int> offsets;    ///< 逐帧 (j − i)（offset 是它的中位数，halfGap 用它的前后半）
    int offset = 0;              ///< 逐帧 (j−i) 的中位数
    double minMargin = 0.0;      ///< min_i (次优−最优)/最优；≈0 = argmin 在平坦区里跳
    int halfGap = 0;             ///< 前半窗与后半窗偏移中位数之差（抓 burst 内掉帧）
};

/// 边缘带上的偏差读数（全部线性域）。
struct Deviation {
    double devEdge = 0.0;     ///< 边缘带上 mean|候选 − 参考|（主指标）
    double devFlat = 0.0;     ///< 平坦带同口径（对照：公共偏差地板 + 非边缘误差）
    double fracVisible = 0.0; ///< 边缘带上偏差 > τ（1 显示 LSB）的像素占比（跨帧均值）
    double gradRatio = 0.0;   ///< 边缘梯度能量 候选/参考（机制判读：>1 假边、<1 摊平）
    int64_t edgePixels = 0;
};

/// 离散抖动相位拟合（M2 去抖 / M1 自证）。零自由参数：p 只有 16 个取值。
struct PhaseFit {
    int bestPhase = 0;
    double bestDev = 0.0;
    double rawDev = 0.0;  ///< 不去抖（等价 p 使位移为 0）的读数，用来打印「去抖赚了多少」
    std::vector<cv::Mat> warped;  ///< 最优相位下的去抖序列（供最终读数用）
};

/// 一个臂在某一 τ 下的尾长读数。
struct TailRow {
    std::string tag;
    double clip = 0.0;
    double events = 0.0;
    double meanChange = 0.0;  ///< mean|N − S|（线性 luma，无量纲化的分母）
    double frac = 0.0;        ///< = decay[0]，主指标
    long offset = 0;          ///< 与参考的 burst 起始帧号之差（回显用，超窗口会抛）
    std::vector<double> decay;
};

/// 相机运动模式的一行读数。
struct SmearRow {
    std::string tag;
    double clip = 0.0;
    double frac = 0.0;       ///< 主指标：Δ 加权的稳态鬼影份额
    double meanDelta = 0.0;  ///< mean Δ（在场性自证：≈0 = 什么都没测到）
    double meanResid = 0.0;  ///< mean|out−ref|（**分子零点**：不门控、不加权、与 Δ 无关）
    double events = 0.0;     ///< 计入的 (帧,像素) 样本数（Δ > τ 才算）
    long offset = 0;         ///< 与参考的 burst 起始帧号之差
};

/// 相机运动版的一臂定义（与 spin 版 `Arm` 同形；REF 也当一臂传，`taa=false`）。
struct SmearArm {
    const char* tag;
    double clip;
    double feedback;
    bool taa;
    double jitter;
};

/// 一档角度的采集结果：`byTau[τ 档][臂]`（臂序与传入的 arms 一致）。
struct SmearAngle {
    std::vector<std::vector<SmearRow>> byTau;
    double maxShiftPx = 0.0;    ///< 全图最大帧间位移（内缩宽度由它定）
    double centerShiftPx = 0.0; ///< **主点处**帧间位移 ⟹ 与臂 11 的 `gate` 断点 2px 对表用
    /// **在场性自证**：mean|ref_j − ref_{j−1}|（**不带 φ**）= 参考画面每帧真的变了多少。
    /// 与 `alignedDelta`（带 φ，**同口径**）配成三态判据，缺一就无法分家：
    ///   raw ≈ 0            ⟹ 画面没动 ⟹ `frame_yaw` 没生效 ⟹ **仪器前提塌**（键名在 lua 侧拼错是静默的）
    ///   raw ≫ aligned      ⟹ φ 搬掉了大部分 ⟹ 映射与运动源一致（正常）
    ///   raw ≈ aligned      ⟹ φ 什么都没搬 ⟹ 映射错（pitch/focal/轴）或运动不是 yaw
    ///   aligned > raw      ⟹ φ 比恒等映射还差 ⟹ 方向反了
    /// ⚠ **口径必须一致**：首轮拿门控的 `meanΔ`（只在 Δ > τ 的像素上取均值）去除无条件 rawΔ，
    /// 得到「φ 搬掉 −302.7%」的**假警报**——门控天然只留高 Δ 像素，两个均值不可比。
    double rawDelta = 0.0;
    /// 与 `rawDelta` **同口径**（内缩区无条件均值）但**带 φ** 的 mean|ref_j − ref_{j−1}∘φ|。
    double alignedDelta = 0.0;
    /// 🔴 **往返地板** `mean|I − φ⁻¹(φ(I))|`（同口径：内缩区无条件均值）。`temporal-homography`
    /// 的判据里早就有这一列（"与残差同量级 ⟹ 单看残差会把量具当闪烁"），建这台仪器时**漏抄**，
    /// 于是 `alignedΔ` 剩下的部分到底是**重采样误差**还是**内容差**无从分辨。
    /// 判读：`alignedΔ ≈ 往返地板` ⟹ 这条链上**没有可归因的时域残差**，此时任何臂间结论都是在读
    /// 双线性插值（纯旋转激励下尤其可能——见 `docs/phase13_plan.md` §1.17 结论 6）。
    /// ⚠ 必须是**往返**：单程 `|I − φ(I)|` 量的是「平移后的边缘差」（≈ `rawDelta`），不是插值误差。
    double roundtripFloor = 0.0;
    /// **自标定刻度**（按 τ 档）：「恰好过期一帧」的 frac 值
    /// = `Σ mean(Δ², gate) / Σ mean(Δ, gate)`。
    /// 🔴 为什么需要它：Δ 加权份额的分母是 `Σ Δ`，所以「候选 = 过期一帧的参考」给出的
    /// **不是 1** 而是 `Σ Δ²/Σ Δ`（Δ 在空间上不均匀）。不除掉它，读数就没有单位。
    /// 除掉之后主指标的单位 = **「相当于几帧过期历史」**：0 = 与参考逐位相同（Z 组），
    /// 1 = 恰好一帧过期，≫1 = 长窗累积。与场景内容无关 ⟹ 跨机位可比。
    std::vector<double> oneFrameFrac;
    /// 建映射用的两个参数（存下来是给 `printSmearPhiCalibration` 复用同一份 φ 重建扫描映射，
    /// 而不是在别处重新推一遍 —— 重新推就是「两份实现」，正是本轮要防的那种错）。
    double pitchRad = 0.0, focalPx = 0.0;
    /// **实测**每帧 yaw 增量（度/帧）= `(yawPost − yawPre) / (framePost − framePre)`，
    /// 由参考组自己回显。它既是配对偏移的除数，也是第二把在场性尺子（≈0 = `frame_yaw` 没生效）。
    double yawPerFrameDeg = 0.0;
    /// 各臂的**配对偏移**（下标与 `groups` 同序，`[0] = 0`）。由 yaw 回显解析算出，
    /// 见 `yawPairOffset`。宽扫诊断拿它当「仪器认为的配对」与 argmin 对表。
    std::vector<long> pairOff;
    int jlo = 0, jhi = 0;
    /// 采到的组（`[0]` = 参考）。**留在结果里**是给 `printSmearDiagnostics` 复用的：
    /// 偏移扫描必须用**同一批数据**，重新采集就不是在问「配对对不对」而是在问「重跑稳不稳」。
    std::vector<Group> groups;
    std::vector<cv::Mat> deltas;  ///< 预算的 Δ_j（与臂/偏移无关，见 `buildSmearDeltas`）
    cv::Mat inner;                ///< 内缩掩膜
};

/// 三档 τ 里的**主档**下标（`collectSmearAngle` 固定产出 `{τ/2, τ, τ·2}`）。
/// 提到文件作用域：主角度组、静态组、gate 组三处都要取同一档，各写一份 `1` 迟早漂。
constexpr size_t kMainTau = 1;

/**
 * δ=0 反向组（**借主角度的 φ**）的采集 + 「运动归因」表。
 *
 * 🔴 **必须与主角度组同臂**（本轮补 B12/B01）。理由：无条件残差里有一块与运动无关的
 * **jitter 相位平均模糊**，而它**随窗长增长**（窗越长平均得越糊）——实测 `meanResid` 从
 * B12 的 0.0288 涨到 B01 的 0.0430（+49%），而同一时间 Δ 加权口径只涨 7%。⟹ 不**逐臂**减掉
 * 「同臂 δ=0」就把**模糊**当成了**鬼影**。只放 A0 的旧版能出地板，但减不了 B 族，于是 α 轴
 * 完全没法归因（这正是 `1−α` 那条线在两个角度都被证伪的机制）。
 * ⚠ 查找按 **tag** 不按位置：加臂时位置索引会**静默错位**（拿到隔壁臂的地板去减）。
 */
struct StaticBaseline {
    std::vector<SmearArm> arms;
    SmearAngle stat;
    const SmearRow& row(const std::string& tag) const
    {
        for (size_t i = 1; i < arms.size(); ++i)
            if (arms[i].tag == tag)
                return stat.byTau[kMainTau][i - 1];
        throw std::runtime_error("静态臂不存在: " + tag);
    }
    /// 运动归因 = 主角度 frac − 同臂 δ=0 frac。δ=0 组借的是**同一个 φ** ⟹ 两组的 Δ 逐帧同式、
    /// 门同式、内缩同式 ⟹ 相减**同口径**，减掉的正是「画面没动时也有的那部分」。
    double attrFrac(const SmearRow& r) const { return r.frac - row(r.tag).frac; }
};

/**
 * 判决线 4 的**数据侧**：在高角度档再采一组（臂 0 / 臂 11）并打对照表。
 *
 * 切出 `cmdTemporalTailCamera` 的理由：本函数只依赖「两臂的帧数 + 中心位移」，把它留在
 * 主函数里会让后者同时背「编排采集」「打表」「出判决」三件事（style-check 已点名）。
 * ⚠ 判决本身**不在这里**——判决线全部集中在 `cmdTemporalTailCamera` 末尾的 `vs`，
 * 这样「跑前写死的线」在一处可读，改线时不会漏打。
 */
struct GateBreakpoint {
    bool ran = false;                        ///< `--gate-angle = 0` 时不跑（本判据是可选档）
    double mainDiff = 0.0;                   ///< 主角度档 `frac(臂11) − frac(臂0)`（几帧过期历史）
    double highDiff = 0.0;                   ///< `--gate-angle` 档同一差值
    double mainMotion = 0.0, highMotion = 0.0; ///< 两档的中心位移（px/帧）
    /// 🔴 **无条件**内缩残差（线性亮度），与 Δ 加权的 `mainDiff/highDiff` 是**两条不同口径**。
    /// 实测（brickfence 0.742px，gate=1）：Δ 加权差 −0.0026 帧（**看不见门**），而
    /// `meanResid` 臂11/臂0 = 0.030474/0.021105 = **1.444**。原因见 §1.17 结论 6：原地纯旋转
    /// 无视差 ⟹ 无去遮挡 ⟹ Δ 加权把质量全压在「普通运动边缘」上，而门多出来的那部分落在
    /// **平坦区的时间平均模糊**。两臂只差 `clip_mode`（jitter/α/模糊地板全同）
    /// ⟹ 这个比值本身就是**干净的门在场性**，不需要 δ=0 归因。
    double mainResid0 = 0.0, mainResid11 = 0.0;
    double highResid0 = 0.0, highResid11 = 0.0;
};

/**
 * 判决发射（四条判据 + jsonl 落账 + rc）。切出 `cmdTemporalTailCamera` 的理由：判决线必须
 * **一处可读**（改线不漏打），而主函数还要管采集编排与打表——两件事挤在一起时先炸的是
 * 认知复杂度上限（style-check 点名），拆它是拆职责不是拆语句。
 */
struct CameraReadings {
    const SmearAngle& main;       ///< 主角度组
    const StaticBaseline& sb;     ///< δ=0 反向组（借主角度 φ）
    const SmearRow &zero, &boxShort, &boxLong;
    const SmearRow &staticZero, &staticFloor;
    double unit;                  ///< 「恰好过期一帧」刻度
    double phiScale;              ///< φ 标定扫描的 argmin
    const GateBreakpoint& gate;
};

/// gate 断点的**分侧计算**（判决本身留在 `emitCameraVerdicts`，线要一处可读）。
/// 🔴 分侧**按实测 gate 值**，不按 CLI 参数名。本轮的坑：主角度取 0.09°/帧 = 中心 1.34px，
/// 落在斜坡 `[1,2]` **中间**（gate 0.66），而参数名叫「低角度侧」——于是把斜坡中当门全开读，
/// 阳性锚点会被读成「洞不存在」。谁开谁关由 `gateAt()` 判。
struct GateSides {
    double openDiff = 0.0, closedDiff = 0.0;       ///< Δ 加权口径（几帧过期历史）
    double openResidRatio = 0.0, closedResidRatio = 0.0; ///< 无条件残差 臂11/臂0
    double openMotion = 0.0, closedMotion = 0.0;
    bool brackets = false;  ///< 两档是否**真的**夹住斜坡（开侧 ≤1px、关侧 ≥2px）
};

/// 收敛参考取末多少帧的均值。抖动关断后这些帧本应逐位相同，取均值是为了把跨进程确定性
/// 地板（债-61：1 LSB / 个位数像素）压到分子之下，而不是把它当成残影读出来。
inline constexpr int kConvergedAvg = 16;

/// 衰减曲线上打印/求 n₅₀ 的采样点（帧）。
constexpr int kStepProbes[] = { 0, 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 96, 128, 160, 224, 320 };

/// 三档掩膜阈值的敏感性（τ 是本仪器唯一自由阈值 ⟹ 主判决只读主 τ，其余档只打印）。
constexpr double kStepTauSweep[] = { 0.01, 0.02, 0.05 };

/**
 * 首帧残影的下限见判决线②（`temporal-tail-step-stimulus`）。
 *
 * 🔴 这里**曾经**是一个 `kStepStimulusLine = 0.5` 常量 + 「门关臂」的注释，两处都错：
 * ① 挂错了臂（`clip_mode = 0` 带默认 5 抽头盒，不是无裁剪）；② 阈值 0.5 是照着错的
 * 预测（1−α）拍的。现在预测挂在 E 臂上，是尖锐的 0.99，线取 0.9。
 */


/// 一个阶跃臂 = 三跑（A / B∞ / 阶跃）。
/// `clipMode` 1 = **关盒**（唯一无裁剪档；⚠ 0 不是关盒，0 带默认 5 抽头十字盒）。
/// `reprojection` false = 历史按屏幕坐标原样取（⟹ `hist(p) = out_{n−1}(p)` 精确）——
/// 闭式锚**必须**用它，理由见 `emitStepVerdicts` 第 ③ 条。
struct StepArm {
    const char* tag;
    double clipMode;
    double feedback;
    bool reprojection = true;
    // ── 批 14.5 升频 / FSR 臂（批 14.6 三态化）──
    /// 抗锯齿选路。`aa != Taa`（Off / Fsr）时 clipMode / feedback / reprojection 三个 TAA
    /// 内部旋钮全部不写键（`stepSpecFor` 负责）。旧「FSR 时域臂必须关 TAA（引擎互斥硬门）」
    /// 现在是**类型事实**：一个 aa 槽位装不下两个时域滤波器。
    Aa aa = Aa::Taa;
    double renderScale = 1.0;  ///< 1.0 = 不写键 = 交付态（原生分辨率）
    bool fsrTemporal = false;  ///< 仅 aa == Fsr 时被消费
    bool fsrJitter = false;    ///< FSR 自己的抖动，与 TAA 的 jitter_scale 是两个独立旋钮
};

/// 一跑的产物：连拍的线性 luma（收敛跑 = 收敛段）。
struct StepRun {
    std::string name;
    std::vector<cv::Mat> frames;
    int warm = 0;
};

/// 阶跃后第 n 帧的残影曲线（掩膜上的归一化均值 + 可见像素占比）。
struct StepCurve {
    std::string tag;
    double maskShare = 0.0;  ///< 掩膜面积占比（激励规模自证）
    double den = 0.0;        ///< mean_mask |A − B∞|（归一化分母）
    std::vector<double> g;   ///< ghost(n)，n = 0..burst−1
    std::vector<double> vis; ///< 掩膜内 |S_n − B∞| > 该像素 1 显示 LSB 的占比
    /// **主指标** = `Σ_n ghost(n)`，单位「帧·全对比度」= 屏幕上陈旧内容的总量。
    /// 换掉 n₅₀ 的理由（实测逼出来的）：盒**生效**时 ghost(0) 就已经掉到 0.015 量级
    /// ⟹ 曲线永远碰不到 0.5 ⟹ n₅₀ 对带盒的臂**不存在**。AUC 无阈值、单调、且关盒态有闭式。
    double auc = 0.0;
    /// 可见鬼影总量 = `Σ_n vis(n)`，单位「帧·满掩膜」——AUC 的可见性对偶（幅度小但面积大的
    /// 那种尾巴 AUC 看不见，本条看得见）。
    double visAuc = 0.0;
    double n50 = -1.0, n5 = -1.0;  ///< 只对**关盒**臂有定义（带盒臂恒 −1，见上）
    /// 与**臂 11** 在两个静止收敛态（A / B∞）上的最大逐像素差（线性 luma）。
    /// 臂 13 的「闪烁轴逐位不变」应当让它落在**跨进程确定性地板**量级（1 LSB ≈ 4e-3）。
    /// ⚠ 这不是 bit 级判据（跨进程本来就不逐位，债-61）；真正的 bit 级红线走 `just shot`
    ///   同会话双构建。本字段是**量级**检查：它要抓的是「差了一个 α」那种 1e-1 量级的错。
    double staticDiff = -1.0;
};

/// 一臂 = 三跑（A 收敛 / B∞ 收敛 / A→B 瞬切）+ 主曲线 + τ 敏感性行。
struct StepArmResult {
    StepCurve main;
    std::vector<double> sens;
    /// 两个**静止**收敛态（A / B∞）。臂 13 的「闪烁轴逐位不变」= 这两个与臂 11 的两个
    /// 应当只差跨进程确定性地板（`convergedRef` 返回独立 Mat，可安全跨臂持有）。
    cv::Mat aRef, bRef;
    /// 收敛态**内部**最大偏离（A / B∞ 两跑取大）= **诊断**用，见 `convergedSpread`。
    double spread = -1.0;
    /// 收敛态不稳定折算到鬼影分子上的**地板**（ghost 单位）= 闸门用，见 `convergedFloor`。
    double floor = 0.0;
};

/**
 * 十组采集（原 `cmdTemporalMotion` 里的 `gM0`…`gM4m` 十个 `Group&`）。
 * ⚠ 采集**顺序** = 原实现顺序 = 打印顺序；`slots` 让十组活到所有读数算完。
 * ⚠ `m3xHolder` 原样保留：M3x 用的是 `run()` 结果的**副本**（原实现就是 `make_unique<Group>(run(…))`），
 *   改成直接用 `run()` 的返回值虽然等价，但那是改代码不是搬代码。
 */
struct MotionGroups {
    std::vector<std::unique_ptr<Group>> slots;
    std::unique_ptr<Group> m3xHolder;
    Group *m0 = nullptr, *m1 = nullptr, *m2 = nullptr, *m3 = nullptr, *m4 = nullptr;
    Group *refm = nullptr, *refs = nullptr, *m1m = nullptr, *m4m = nullptr;
    Group* m3x = nullptr;  ///< 仅相机模式（M3x 恒等对照），spin 模式为 nullptr
};

/// A/B 表的一行（原 `cmdTemporalMotion` 内的局部 `struct Row`）。
struct MotionRow {
    const char* tag;
    const char* desc;
    Deviation dev;
    std::string note;
};

/// 区域与阈值（原 379–409 行的产物：口径来自 REF，不来自候选）。
struct MotionRoi {
    cv::Mat grad, edgeMask, flatMask;
    double tauEdge = 0.0, tau = 0.0, white = 0.0;
};

/// 历史拒收掩膜段（原 346–377 行）的产物。
struct MotionMasks {
    int clipModeInt = 0;
    bool alphaOccupied = false;
    RejectStats rsM1, rsM4;
};

/// 逐组跨进程对齐（原 411–419 行的产物）。
struct MotionAlign {
    Alignment m0, m1, m2, m3, m3x, m4;
};

/// A/B 表（原 479–521 行）的产物 + 判决要用的五个 devEdge。
struct MotionRows {
    std::vector<MotionRow> rows;
    double devM0 = 0.0, devM1 = 0.0, devM3 = 0.0, devM4 = 0.0, devM3x = 0.0;
};

/// 判决段算出的两个比值（落账要用）。
struct MotionRatios {
    double R = 0.0, Rbest = 0.0;
};

/// 下面两个是 `temporal_motion.cpp` 的**私有**分段（校验与抬头回显），定义就在那儿。

// ── 口径算子（定义在 `temporal_motion_metric.cpp`） ──────────────────────────────────
cv::Point2d contentShiftPx(uint32_t frameIndex, double scale);

[[nodiscard]] cv::Mat shiftLuma(const cv::Mat& luma, cv::Point2d d);

std::string d4(double v);

std::string d2(double v);

std::string pct2(double v);

cv::Mat maxPoolTo(const cv::Mat& src, cv::Size dst);

RejectStats rejectStats(const Group& g);

void assertMaskAlive(const std::string& name, const RejectStats& s);

double medianOf(std::vector<int> v);

[[nodiscard]] double frameL1(const cv::Mat& a, const cv::Mat& b);

Alignment alignSequence(const Group& cand, const Group& ref, int window, bool moving);

Deviation measureDeviation(const std::vector<cv::Mat>& cand, const Group& ref,
    const Alignment& al, const cv::Mat& edgeMask, const cv::Mat& flatMask, double tau);

PhaseFit fitJitterPhase(const Group& g, const Group& ref, const Alignment& al,
    const cv::Mat& edgeMask, double jitterScale);

std::vector<cv::Mat> revealMasks(const Group& ref, double tau, int maxLag);

TailRow measureTail(const std::string& tag, double clip, const Group& ref, const Group& cand,
    const std::vector<cv::Mat>& reveal, int maxLag, int jlo, int jhi);

double staticDiff(const cv::Mat& a, const cv::Mat& b);

// ── 采集（定义在 `temporal_motion_collect.cpp`） ───────────────────────────────────
Group collectGroup(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& cam, const Spec& sp, int burst);

// ── 移动机位分带（定义在 `temporal_motion_report.cpp`） ────────────────────────────────
cv::Mat movingDevMap(const std::vector<cv::Mat>& cand, const Group& ref, const Alignment& al);

std::string reportMovingBandsAgainstReference(const Group& ref, const cv::Mat& grad,
    double gradThr, double tau, const cv::Mat& devOff, const cv::Mat& devOn,
    const std::string& cameraName);

// ── 机位运动尾长 smear（定义在 `temporal_smear.cpp`） ──────────────────────────────────
cv::Mat smearInnerMask(cv::Size size, double shiftPerFrame, int frames);

std::vector<cv::Mat> buildSmearDeltas(const Group& ref, const YawMap& map, int jlo, int jhi);

SmearRow measureSmearAt(const std::string& tag, double clip, const Group& ref, const Group& cand,
    const std::vector<cv::Mat>& deltas, const cv::Mat& inner, double tau, int jlo, int jhi,
    long off);

long yawPairOffset(const Group& ref, const Group& cand, double yawPerFrameDeg);

double safeRatio(double num, double den);

double percentRemoved(double num, double den);

double mainTauValue(const std::vector<double>& v, double fallback);

double roundtripFloorOf(const Group& ref, const YawMap& fwd, const YawMap& back, int jlo, int jhi,
    const cv::Mat& inner);

SmearAngle collectSmearAngle(const std::string& base, const BistroCam& cam, int burst, int warm,
    double angleRad, double mapAngleRad, double tau, int margin,
    const std::vector<SmearArm>& arms, const std::string& suffix);

double gateAt(double motionPx);

const char* gateSide(double motionPx);

void printSmearTables(const std::vector<SmearArm>& arms, const SmearAngle& a, double tau);

void printSmearDiagnostics(const std::vector<SmearArm>& arms, const SmearAngle& a, double tau);

double printSmearPhiCalibration(const SmearAngle& a, double mapAngleRad);

// ── 机位运动闸门与判决（定义在 `temporal_tail_camera.cpp`） ───────────────────────────────
StaticBaseline collectStaticBaseline(const std::string& base, const BistroCam& cam, int burst,
    int warm, double mainRad, double tau, int margin,
    const std::vector<const SmearRow*>& mainRows);

GateBreakpoint collectGateBreakpoint(const std::string& base, const BistroCam& cam, int burst,
    int warm, double tau, int margin, double gateRad, double lowArm0, double lowArm11,
    double mainCenterShiftPx, double mainResid0, double mainResid11);

GateSides gateSidesOf(const GateBreakpoint& g);

int emitCameraVerdicts(const std::string& cameraName, double angleDeg, double gateAngleDeg,
    int burst, double tau, const CameraReadings& r);

// ── 平移阶跃（定义在 `temporal_step.cpp` 与 `temporal_step_fsr.cpp`） ─────────────────
BistroCam strafedCam(const BistroCam& c, double dist);

StepRun captureStepRun(const std::string& dir, const std::string& name, const std::string& tag,
    const BistroCam& from, const BistroCam& to, const Spec& sp, int burst);

cv::Mat convergedRef(const StepRun& r);

cv::Mat stepMask(const cv::Mat& aRef, const cv::Mat& bRef, double tau, int margin, double* den,
    double* share);

double convergedFloor(const StepRun& r, const cv::Mat& ref, const cv::Mat& mask, double den);

double convergedSpread(const StepRun& r);

double firstBelow(const std::vector<double>& g, double level);

StepCurve stepCurveOf(const StepArm& arm, const cv::Mat& aRef, const cv::Mat& bRef,
    const StepRun& run, double tau, int margin);

void printStepCurve(const StepCurve& c);

// ══════════════════════ temporal-feedback（B11 拆入）══════════════════════

/// temporal-feedback 的采集上下文。`slots` 持有全部组（`Group` 不可拷贝，故 unique_ptr），
/// 其余成员是指向它们的别名；`probeHost` = 承载同进程 `/param` A/B 探针的那一组。
struct FeedbackCollection {
    std::vector<std::unique_ptr<Group>> slots;
    Group* refm = nullptr;
    Group* m0 = nullptr;
    Group* zero = nullptr;
    Group* refs = nullptr;
    Group* s0 = nullptr;
    Group* probeHost = nullptr;
    std::vector<std::pair<double, Group*>> motion;
    std::vector<std::pair<double, Group*>> still;
};

/// temporal-feedback 的边缘带（口径与 temporal-motion 一致：**来自参考，不来自候选**）。
struct FeedbackBand {
    cv::Mat edge, flat;
    double tau = 0.0;
    double white = 0.0;
};

/// temporal-feedback 运动态一行（含对齐护栏的两个量，判决只认 `trust` 的组）。
struct FeedbackRow {
    std::string tag;
    double fb = 0.0;
    Deviation dev;
    double margin = 0.0;
    int gap = 0;
    bool curve = false;  ///< 进不进 fb 曲线（地板 / 零态锚点不进）
    bool trust = true;   ///< 对齐护栏是否通过（不通过 = 读数不可解释）
};

/// temporal-feedback 静止态一行。两列边缘偏差并列出数：本命令口径 vs 已发表 M4 的借掩码口径。
struct FeedbackStillRow {
    double fb = 0.0;
    int warm = 0;
    ResidualStat st;
    Deviation devAligned;  ///< 边缘带来自 **REFs**（与静止候选同视点）= 本命令口径
    Deviation devBorrowed; ///< 边缘带来自 **REFm** = 已发表 M4 的口径（借来的掩码）
};

/// temporal-feedback fb 曲线的聚合量（`dropped` 要同时进打印与 logCall，故一起带）。
struct FeedbackCurve {
    std::vector<std::pair<double, double>> curve;
    std::string dropped;
    double minDev = 0.0;
    double maxDev = 0.0;
    double bestFb = 0.0;
    double devAt100 = -1.0;
    double maxFb = -1.0;
    double devAtMaxFb = 0.0;
    double devAt12 = -1.0;
    int inversions = 0;
    double spread = 0.0;
    double rBest = 0.0;
    double r100 = 0.0;
    double r12 = -1.0;
};

/// 热身帧数 = align16(max(16, ⌈6/α⌉))，α = feedback（EMA 记忆长度 ≈ 1/α）。
int warmForFeedback(double fb);
/// fb 的文件名后缀（两位百分比，零填充）。
std::string fbTagOf(double fb);
/// 由参考首帧算边缘带 / 平坦带 / τ（闭式：白电平处 1 个显示 LSB）。
FeedbackBand feedbackBandOf(const Group& ref, double edgePercentile);
/// 采集全部组（运动 + 静止 + 两个超采样参考 + 零态锚点 + S0 地板）。**串行、单实例纪律**。
void collectFeedbackGroups(FeedbackCollection& c, const std::string& cameraName, double angle,
    int burst, double kernel, const std::vector<double>& fbs);
/// 守卫 1：运动激励在场。返回 false = 调用方立刻停下（退出码 2）。
bool guardMotionExcitation(std::vector<Verdict>& vs, const Group& gM0, const Group& gRefm,
    const std::string& cameraName);
/// 守卫 2：feedback **真的传进去了**（回显相符 + 互异 + 同进程 /param A/B 字节差 > 0）。
bool guardFeedbackParamLive(std::vector<Verdict>& vs,
    const std::vector<std::pair<double, Group*>>& motion, const Group& probeGroup,
    const std::string& cameraName);
/// 对齐 + 读数：地板 M0、每个 fb 档、零态锚点各一行。
void addFeedbackRows(std::vector<FeedbackRow>& rows, const Group& gM0, const Group& gRefm,
    const std::vector<std::pair<double, Group*>>& motion, const Group& gZero,
    const FeedbackBand& bM, int window);
/// 守卫 3：对齐护栏（复用 temporal-motion-alignment 的阈值）。
void guardFeedbackAlignment(std::vector<Verdict>& vs, const std::vector<FeedbackRow>& rows);
void printFeedbackMotionTable(const std::vector<FeedbackRow>& rows, const FeedbackBand& bM,
    double devM0, int warmMotion);
/// 守卫 4：零态锚点（fb=1 + js=0 按构造 ≈ M0）。
void guardFeedbackZeroState(std::vector<Verdict>& vs, const std::vector<FeedbackRow>& rows,
    double devM0);
/// 回锚：fb=0.12 那一档必须 ≈ 已发表 M1/M0 = 1.138（本扫描与那张表是同一台仪器的唯一证据）。
void guardFeedbackTieback(std::vector<Verdict>& vs, const std::vector<FeedbackRow>& rows,
    double devM0);
/// 静止态 swing 的 τ 由 fb=0.12 那组定（τ 是**场景×机位**属性不是组属性）。
double feedbackStillTau(const std::vector<std::pair<double, Group*>>& still, int periods);
void collectFeedbackStillRows(std::vector<FeedbackStillRow>& srows,
    const std::vector<std::pair<double, Group*>>& still, const Group& gS0, const Group& gRefs,
    const FeedbackBand& bS, const FeedbackBand& bM, double tauStill, int window, int periods);
void printFeedbackStillTable(const std::vector<FeedbackStillRow>& srows, double tauStill);
/// 批 13.8 第 0 步读数：**静止态**的「抖动自身代价」= dev(S fb=1) / dev(S0)。
void printFeedbackStillStepZero(const std::vector<FeedbackStillRow>& srows);
/// `Group` 的 luma 序列 → `phaseSwingStat` / `phaseAverageLuma` 要的 `std::vector<Frame>`。
std::vector<Frame> toFrames(const Group& g);
/// 扫曲线取带内最优 / 最大档 / fb=1.0 与 0.12 两档读数，写进 `cu` 对应字段。
void scanFeedbackCurve(FeedbackCurve& cu);
/// 逆序对数 = fb 更大反而更差的对数（0.5% 容差吸收跨进程噪声）。
int countFeedbackInversions(const std::vector<std::pair<double, double>>& curve);
/// 只用**对齐可信**的曲线组做聚合；不可信的逐条记进 `dropped`。
FeedbackCurve buildFeedbackCurve(const std::vector<FeedbackRow>& rows, double devM0);
/// 四条判决线（①/②/②′/③）+ 介于两可。
void printFeedbackVerdict(const FeedbackCurve& cu, double devM0);
/// 静止/运动两侧的 fb 敏感度（「窗棂残余与运动失效是否同一个根」的对照）。
void printFeedbackCrossCheck(const std::vector<FeedbackStillRow>& srows,
    const std::vector<std::pair<double, double>>& curve);

int emitStepVerdicts(const std::string& cameraName, double stepDist, int burst, double tau,
    int margin, const StepCurve& zero, const StepCurve& ema, const StepCurve& boxed,
    const StepCurve& delivery, const StepCurve& fix, bool* guardsOkOut);

int stepWarmFor(double alpha, int warmFloor);

Spec stepSpecFor(const StepArm& a, int warm);

StepArmResult collectStepArm(const std::string& dir, const BistroCam& from, const BistroCam& to,
    const StepArm& a, int burst, int overrideWarm, double tau, int margin);

int runFsrStepArms(const std::string& dir, const BistroCam& from, const BistroCam& to, int burst,
    int warm, double tau, int margin, bool taaGuardsOk);

}  // namespace RGL::imgqa::tmotion
