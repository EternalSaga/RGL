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
//
// ══════════════════════════════════════════════════════════════════════════════════════
// ## 批 13.9：【机位状态 = **相机移动**】亮度 × 梯度 二维分带（本命令 = 两把尺子里的**尺子 2**）
//
// ### 要判的假设（用户 2026-10-06）
//
// 「所有**移动**锯齿的来源都是 1px **高亮**直线；**普通边缘确实没锯齿**。」
//
// 昨天批 13.7c 给**静止机位**跨相位摆动加了同一套分带（判出「静止残余非高光专属」），但那条
// 只覆盖静止态 —— 「**移动** × 亮度 × 梯度」这一格至今是空的，用户的主张既没证实也没否证。
//
// ### 为什么这条尺子大概最贴用户的原话
//
// 用户说的是「**锯齿**」。本命令已经有「候选图离『没有锯齿也不糊的 720p 图』有多远」这个
// 定义级问题（参考 = 同机位序列 2560×1440 + TAA 关 → 线性域面积平均）。把它**逐像素化**并按
// 亮度×梯度分带，问的就是：那些「偏离理想 720p」的像素，是不是住在近削顶×高梯度那一格？
// 三张表：
//   * `dev(TAA 关)` = 无 AA 单帧光栅的锯齿本体（**主表**，用户肉眼看到的就是这个）
//   * `dev(TAA 开)` = 开 TAA 之后还剩的偏差
//   * `max(dev(开) − dev(关), 0)` = **正向超额** = 「开 TAA 让哪里**更**偏离真值」（R=1.14 这个
//     整体比值在二维上的展开；负的部分只报总量，不进分带 —— 分带要的是「超额住在哪」）
//
// ### ⚠ 分带尺子**必须来自参考图**（这条是昨天选择偏差的教训，与尺子 1 的口径差别就在这一点）
//
// 亮度档与梯度档都定在 **REFm（超采样那张）** 上，不定在候选上：参考定义了「这个像素**本来**
// 是什么」，用被测量自己定档 = 用被测量选区域（批 13.7c 已因这种口径翻过一次方向）。
// 具体：亮度轴 = REFm 显示域字节亮度的多帧均值（1440p 上求均值再 INTER_AREA 降到 720p 网格，
// 与偏差图同网格才谈得上逐像素分带）；梯度轴 = `sobelAbsGrad(REFm.luma.front())` 的 **≥p90**
// = **就是本命令已有的那条边缘带**（`--edge-pct 0.90`），一个阈值都不新拍。
//
// ### ⭐ 判决线（**2026-10-06 跑前写死**，防事后挑数；三分支全部命名）
//
// 记 A = 中间调×高梯度（普通几何边缘）、B = 近削顶×高梯度（假设的元凶）。
// 主表 = `dev(TAA 关)`。每格出**四数**：share / rate>τ / **计数份额** / **幅度份额**。
//
// * **分支①「支持」** ⟺ B 的**幅度份额** ≥ 30%（`kMovingSupportAmpShare`）
//   **或** 正向超额集中在 B：`rB ≥ 1.5 × rA`，`rX = rate_on(X)/rate_off(X)`
//   （1.5 = 跑前沿用的 `kBandSupportRatio`，不新拍）。
// * **分支②「否证」** ⟺ B 的幅度份额**与**计数份额**都** < 5%（`kMovingRefuteShare`）
//   **且** 超额不向 B 集中。
// * **分支③「分不出」** ⟺ 其余；出表处必须再报性质（三选一）：**阈值没分开**（附敏感性表）/
//   **两格本来同分布** / **反向分离**（超额集中在 A = 普通边缘，与主张方向相反）。
//
// 三分支由 `classifyMovingBand` 执行（`temporal_residual.cpp`，两把尺子共用同一份）。
// **幅度份额是本次新增的胜负手**：`frac>τ` 不加权，平坦中间调摆 1 LSB 与高光摆一大截各算一票，
// 而高光是对比度最高处、同样绝对摆动最看得见 ⟹「我一直看到高亮在闪」与「高亮只占残余 0.5%」
// 可同时为真。只出计数份额 = 未完成。
//
// ⚠ 本段**只出数 + 一句判决**，不产 Verdict、不进 rc：上面那三条判据（激励在场 / 对齐 / 消融）
//   的 rc 语义不许被新表污染。既有 A/B 表与判决逐字不动。


#include "imgqa.h"
#include "temporal_residual.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iostream>
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

/// 任一通道不同的 8UC1 掩码（同 `judge.cpp::anyChannelDiff`；那边在匿名 namespace 里，
/// 跨文件够不着。**不要**用 cv::reduce(axis=0) 合并通道——它对多通道输入按通道独立归约、
/// 输出仍是多通道，countNonZero 会断言 cn==1 炸）。
[[nodiscard]] cv::Mat anyChannelDiffLocal(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat d;
    cv::compare(a, b, d, cv::CMP_NE);
    std::vector<cv::Mat> chs;
    cv::split(d, chs);
    cv::Mat any = chs[0];
    for (size_t i = 1; i < chs.size(); ++i)
        cv::bitwise_or(any, chs[i], any);
    return any;
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
    /// 批 13.11：>0 = 开 `taa.static_boost`（地板 = **本帧权重**）。默认 0 = 不写键 = 交付态。
    /// spin 模式下相机静止 ⟹ boost 真生效 ⟹ 进入 债-49 风险区（静止机位 + 场景内运动 + 长窗）。
    double staticBoostFloor = 0.0;
    /// 批 13.11 **历史裁剪消融臂**（`taa.clip_mode` / `taa.clip_soft`）。默认 0 = 不写键 = 交付态。
    /// 语义 → `rgl/scene_data.cppm` kSceneDataTaaClipControlOffset。
    double clipMode = 0.0;
    double clipSoft = 1.0;
};

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
    /// **引擎自己回显**的生效 feedback（`GET /state` 读回，不是写进 lua 的值）。
    /// 参数扫描的红线 6：写进 lua 没生效 = 整条曲线是平的，而平曲线恰好会被读成
    /// 「该参数无效」这个结论 ⟹ 每组的生效值必须来自引擎，见 `Engine::paramValue`。
    double liveFeedback = -1.0;
    /// 同进程 `/param` A/B 的字节差（-1 = 本组没做）。**只隔一个 float** 的两张同相位图若
    /// 逐位相同，说明这个滑杆改的是一个没人读的字段 ⟹ 整条扫描曲线无解释力。
    double probeBytes = -1.0;
    double probeChangedPx = 0.0;  ///< 同上取「任一通道不同的像素占比」
};

/**
 * 2×2 **峰值**池化（把原生分辨率的字节亮度轴降到统计网格）。
 *
 * 为什么手写：`cv::resize` 的 INTER_MAX 在 OpenCV 4.12 里**不存在**（`resize()` 只认
 * NEAREST/AREA/LINEAR/CUBIC/LANCZOS4，其余一律 `CV_Error("Unknown interpolation method")`，
 * 实测在 REFm 组崩过一次 rc=3）。而 INTER_AREA 是面积**平均** ⟹ 1px 高亮细线被邻居稀释
 * （最坏 4 倍），恰好把「1px 高亮直线」这个假设的元凶从尺子上抹掉 —— 所以敏感性表需要一把
 * 「这个统计像素的邻域里**曾经**有多亮」的轴，它必须是 max 而不是 mean。
 *
 * 只支持整数倍降采样（本项目 = 1440p→720p 恰好 2×）；非整数倍退回 INTER_NEAREST 并如实记录。
 */
cv::Mat maxPoolTo(const cv::Mat& src, cv::Size dst)
{
    if (src.empty() || dst.width <= 0 || dst.height <= 0 || src.cols % dst.width != 0
        || src.rows % dst.height != 0) {
        cv::Mat fallback;
        cv::resize(src, fallback, dst, 0, 0, cv::INTER_NEAREST);
        return fallback;  // 非整数倍：本函数不适用（调用方只有 2× 一条路），退回最近邻
    }
    const int sx = src.cols / dst.width, sy = src.rows / dst.height;
    cv::Mat out(dst, CV_64FC1);
    for (int y = 0; y < dst.height; ++y) {
        double* drow = out.ptr<double>(y);
        for (int x = 0; x < dst.width; ++x) {
            double v = 0.0;
            for (int dy = 0; dy < sy; ++dy) {
                const double* r = src.ptr<double>(y * sy + dy) + x * sx;
                for (int dx = 0; dx < sx; ++dx)
                    v = std::max(v, r[dx]);
            }
            drow[x] = v;
        }
    }
    return out;
}

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
    o.taaInputKernel = sp.kernel;  // 批 13.8 输入侧重建核（0 = 不写键 = 已发表口径）
    // 批 13.11 鬼影 × 裁剪消融（默认全关 = 不写键 = 已发表 M0…M4 读数逐位不变）。
    // ⚠ 全部以 `o.taa` 为条件：TAA 关的组（M0/REFm/REFs）没有历史，写这些键是空操作，
    //   但会让「参考组 = 交付配置原样」这条自证失效 ⟹ 一个字节都不多写。
    o.taaStaticBoost = o.taa && sp.staticBoostFloor > 0.0;
    o.taaStaticFeedback = sp.staticBoostFloor;
    o.taaClipMode = o.taa ? sp.clipMode : 0.0;
    o.taaClipSoft = sp.clipSoft;
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
    // 红线 6（参数扫描）：**引擎自己回显**的生效 feedback，不是写进 lua 的值。
    // 读回落在热身之前与之后都一样（本判据不写这个滑杆），落在前面是为了让「没生效」
    // 在采集开始前就出声。label 改名 / 滑杆被摘 ⟹ paramValue 抛 = 当场停下。
    g.liveFeedback = eng.paramValue("", kTaaFeedbackLabel);
    eng.advanceFrames(static_cast<uint32_t>(sp.warm));
    g.framePre = eng.frameCount();
    const auto files = eng.shotBurst("screenshots", static_cast<uint32_t>(burst),
        tag + "_c", sp.attachment);
    g.framePost = eng.frameCount();

    cv::Mat prev;
    cv::Mat byteSum, byteMax;  ///< `sp.wantBytes` 的累加器（**原生分辨率**，收尾才降网格）
    int byteFrames = 0;
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
        if (sp.wantBytes) {
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
    if (sp.wantBytes && byteFrames > 0) {
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
    if (sp.probeFeedback > 0.0) {
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
        g.probeChangedPx = static_cast<double>(cv::countNonZero(anyChannelDiffLocal(ia, ib)))
            / static_cast<double>(ia.total());
        std::printf("      └ /param A/B 探针（同进程 · 静止机位 · 隔整 1 个抖动周期 = 同相位）："
                    "fb %s → %s，引擎回显 %s，字节差 %d（变化 %.2f%%）\n",
            d4(sp.feedback).c_str(), d4(sp.probeFeedback).c_str(), d4(liveB).c_str(),
            static_cast<int>(g.probeBytes), g.probeChangedPx * 100.0);
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
        // 分桶口径**唯一出处** = `rejectFractionOf`（temporal_residual.h）。曾经这里是手写的
        // 四次 `cv::compare` + 作差，写错过一次（拿累加器作差 → 盒裁剪 −1388%）；现在那类错误
        // 结构上消失，且与 `temporal-residual --reject` 的交叉表**共用同一把尺子**——两份分桶
        // 的边界一旦不同，两条判据的「拒收」就不是同一件事，而这种漂移不会让任何一条变红。
        const RejectFraction f = rejectFractionOf(a);
        s.accepted += f.accepted;
        s.clipped += f.clipped;
        s.collapsed += f.collapsed;
        s.reproject += f.reproject;
        s.noHistory += f.noHistory;
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

}  // namespace

// ── 批 13.9：【机位状态 = **相机移动**】亮度 × 梯度 二维分带（尺子 2）───────────────────
//
// 要判的假设、三张表各是什么、尺子为什么必须来自参考图、⭐ 判决线（**跑前写死**）全文 =
// 本文件头注释「批 13.9」段。这里只放执行代码；改判决线 = 改判据，两处一起改。
namespace {

/**
 * 逐像素偏差图 = 跨帧平均的 `|候选 − 参考（按帧号配对到同一视点）|`（线性域，CV_64FC1）。
 *
 * 与 `measureDeviation` 的关系：那条把这张图**压成一个数**（边缘带上的均值），本函数保留
 * 整张图供分带。两者是同一口径（同一配对、同一线性域、同一绝对值），只是一个求值一个成图
 * ——「表上的边缘偏差」与「分带表里边缘带的贡献」因此必然自洽。
 */
cv::Mat movingDevMap(const std::vector<cv::Mat>& cand, const Group& ref, const Alignment& al)
{
    cv::Mat acc = cv::Mat::zeros(cand.front().size(), CV_64FC1);
    int frames = 0;
    for (size_t i = 0; i < cand.size() && i < ref.luma.size(); ++i) {
        cv::Mat d;
        cv::absdiff(cand[i], ref.luma[static_cast<size_t>(al.pairIndex[i])], d);
        cv::add(acc, d, acc);
        ++frames;
    }
    acc /= static_cast<double>(std::max(1, frames));
    return acc;
}

/// 出三张六格表（dev 关 / dev 开 / 正向超额）+ 三分支判决 + 敏感性。返回判决文字供台账。
std::string reportMovingBandsAgainstReference(const Group& ref, const cv::Mat& grad,
    double gradThr, double tau, const cv::Mat& devOff, const cv::Mat& devOn,
    const std::string& cameraName)
{
    std::printf("\n── 【机位状态 = **相机移动**】亮度 × 梯度 二维分带（尺子 2：对超采样参考的"
                "逐像素偏差；机位 %s）──\n",
        cameraName.c_str());
    if (ref.byteMean.empty() || grad.empty() || devOff.empty()) {
        std::printf("  ✗ 尺子缺失（REFm 的字节亮度没攒到 = 采集侧 wantBytes 没开），分带表未出\n");
        return "未出（尺子缺失）";
    }
    const double tHi = static_cast<double>(bandClipProxyByte(kBandClipProxyLinear));
    std::printf("  分带尺子**来自参考图 REFm**（超采样那张）：参考定义了「这个像素**本来**是什么」，"
                "用被测量自己定档 = 用被测量选区域（昨天已因此翻过一次方向）\n");
    std::printf("    亮度轴 = REFm 显示域字节亮度的多帧均值（1440p 求均值后 INTER_AREA 降到 720p）；"
                "T_lo=%.0f · T_hi=%.0f（**闭式**：ACES 输入 ≥%s）\n",
        kBandMidByte, tHi, d4(kBandClipProxyLinear).c_str());
    // 梯度轴**回显实际分位**（不是写死 p90）：调用方可以 --edge-pct 改档，写死会让表说谎。
    std::vector<double> gv = valuesInMask(grad, {});
    std::sort(gv.begin(), gv.end());
    const double gradPct = gv.empty() ? 0.0
        : static_cast<double>(
              std::lower_bound(gv.begin(), gv.end(), gradThr) - gv.begin())
            / static_cast<double>(gv.size());
    std::printf("    梯度轴 = sobelAbsGrad(REFm 线性 luma) ≥ %s（= 参考梯度的 p%.2f，**就是本命令"
                "已有的那条边缘带**，一个阈值都不新拍）\n",
        d4(gradThr).c_str(), gradPct);
    std::printf("    τ = %s（= 上面判据用的同一个 τ，白电平 p99 处 1 个显示 LSB，闭式）\n",
        d4(tau).c_str());

    const BandTable tOff = bandTableOf(devOff, ref.byteMean, grad, kBandMidByte, tHi, gradThr, tau);
    const BandTable tOn = bandTableOf(devOn, ref.byteMean, grad, kBandMidByte, tHi, gradThr, tau);
    cv::Mat excess;
    cv::subtract(devOn, devOff, excess);
    cv::max(excess, 0.0, excess);  // **正向**超额 = 开 TAA 让这里更偏离真值；负的部分不进分带
    const BandTable tExc = bandTableOf(excess, ref.byteMean, grad, kBandMidByte, tHi, gradThr, tau);
    if (!tOff.valid() || !tOn.valid() || !tExc.valid()) {
        std::printf("  ✗ 分带表无像素（total=0），不出数\n");
        return "未出（无像素）";
    }
    printMovingBandTable("dev(TAA 关) —— 无 AA 单帧光栅的锯齿本体（**主表**）", tOff,
        "对超采样参考的逐像素偏差（TAA 关）");
    printMovingBandTable("dev(TAA 开) —— 开 TAA 之后还剩的偏差", tOn,
        "对超采样参考的逐像素偏差（TAA 开）");
    printMovingBandTable("max(dev(开) − dev(关), 0) —— **正向超额**：开 TAA 让哪里更偏离真值", tExc,
        "偏差差图的正部");

    const BandShare aOff = bandShareOf(tOff, 1, 1), aOn = bandShareOf(tOn, 1, 1);
    const BandShare bOff = bandShareOf(tOff, 2, 1), bOn = bandShareOf(tOn, 2, 1);
    const BandShare bExc = bandShareOf(tExc, 2, 1), aExc = bandShareOf(tExc, 1, 1);
    const double rA = aOff.rate > 0.0 ? aOn.rate / aOff.rate : 0.0;
    const double rB = bOff.rate > 0.0 ? bOn.rate / bOff.rate : 0.0;
    const bool excessInB = rA > 0.0 && rB >= kBandSupportRatio * rA;
    // ⚠ **率比退化闸门**（与尺子 1 同一条，实测逼出）：移动机位下 `frac>τ` 会饱和，四格 rate
    // 双双贴 100% 时 rX 的分子分母同贴 1.0，rB/rA 只剩浮点噪声 ⟹ 饱和时**方向不判**。
    const double kSat = 0.90;
    const bool rateSaturated = aOff.rate > kSat && bOff.rate > kSat && aOn.rate > kSat
        && bOn.rate > kSat;
    // rB/rA 的相对差（判据线要求的是 ≥50% 的分离，故这个数远小于 0.5 时「方向」根本没有内容）。
    const double relDiff = std::abs(rB - rA) / std::max({ rA, rB, 1e-12 });

    // 正/负超额的**总量**（读「R=1.14 这个整体比值在二维上展开成了什么」的守恒检查）
    const double posMass = cv::sum(excess)(0);
    cv::Mat deficit;
    cv::subtract(devOff, devOn, deficit);
    cv::max(deficit, 0.0, deficit);
    const double negMass = cv::sum(deficit)(0);

    std::printf("\n  正向超额合计 Σ=%.4f ｜ 反向（开 TAA 更贴真值）合计 Σ=%.4f ｜ 净 = %+.4f"
                "（>0 ⟺ 全帧平均上开 TAA 更偏离参考，与 R>1 同向）\n",
        posMass, negMass, posMass - negMass);
    std::printf("  率比 rX = rate_on(X)/rate_off(X)（该格 over-τ 比例开 TAA 后翻几倍）\n");
    std::printf("    A 中间调×高梯度 rate %s → %s（×%s）  B 近削顶×高梯度 rate %s → %s（×%s）\n",
        pct2(aOff.rate).c_str(), pct2(aOn.rate).c_str(), d4(rA).c_str(),
        pct2(bOff.rate).c_str(), pct2(bOn.rate).c_str(), d4(rB).c_str());
    std::printf("    B 幅度份额（主表 dev 关）%s ｜ B 在**正向超额**里的幅度份额 %s（面积只占 %s）"
                "｜ A 在正向超额里的幅度份额 %s（面积占 %s）\n",
        pct2(bOff.ampShare).c_str(), pct2(bExc.ampShare).c_str(),
        pct2(bExc.share).c_str(), pct2(aExc.ampShare).c_str(),
        pct2(aExc.share).c_str());
    if (rateSaturated)
        std::printf("    ⚠ **率比在本场景退化失效**：四格 rate 全部 > %.0f%%（`frac>τ` 饱和）⟹ rX 的"
                    "分子分母同贴 1.0，rB/rA 的相对差只有 %s ——**方向不判**，只看幅度份额与超额表\n",
            kSat * 100.0, d4(relDiff).c_str());

    const MovingBandBranch branch
        = classifyMovingBand(bOff.ampShare, bOff.cntShare, excessInB);
    std::printf("\n  ⭐ 判决（线**跑前写死**：支持 = B 幅度份额 ≥ %s 或 rB ≥ %s×rA；"
                "否证 = B 两份额都 < %s 且超额不向 B 集中）\n",
        pct2(kMovingSupportAmpShare).c_str(), d4(kBandSupportRatio).c_str(),
        pct2(kMovingRefuteShare).c_str());
    std::printf("    支撑格数字：B 幅度份额 %s（支持线 %s / 否证线 %s）· B 计数份额 %s · "
                "rB/rA = %s · 超额集中在B = %s\n",
        pct2(bOff.ampShare).c_str(), pct2(kMovingSupportAmpShare).c_str(),
        pct2(kMovingRefuteShare).c_str(), pct2(bOff.cntShare).c_str(),
        rA > 0.0 ? d4(rB / rA).c_str() : "inf", excessInB ? "是" : "否");
    std::printf("    ⟹ **%s**\n", movingBandBranchName(branch));
    printMovingBandVacuity(bOff, "REFm 超采样参考 INTER_AREA 降到 720p 后的字节亮度均值",
        "REFm 是 2560×1440 超采样图，亮度轴经 **INTER_AREA（面积平均）** 降到 720p 网格 ⟹ "
        "1px 高亮细线被邻居稀释（最坏 4 倍），**假设的元凶可能正好被尺子自己抹掉**。"
        "下面敏感性表的「亮度轴=原生格峰值」行就是量这件事的：若近削顶档在空集/非空之间翻，"
        "则「看不见」是降采样口径问题，不是「高光不闪」");
    // 分支③的**性质**也必须命名（昨天缺的就是这个）：方向 + 敏感性两条一起读。
    if (rateSaturated)
        std::printf("    分离方向：**不判**（rate 饱和；rB/rA 相对差 %s，而判据线要的是 ≥50%% 的分离"
                    "⟹ 方向这一维在本场景没有内容）——用户主张要求 rB > rA，但本场景 rate 轴没有"
                    "分辨力，方向问题由**幅度份额**与**正向超额表**回答\n",
            d4(relDiff).c_str());
    else
        std::printf("    分离方向：rB %s rA ⟹ %s（用户主张要求的是 rB > rA：B 比 A 更压不动）\n",
            rB < rA ? "<" : (rB > rA ? ">" : "="),
            rB < rA ? "**反向分离**（B 比 A 压得更狠，与主张方向相反）"
                    : (rB > rA ? "B 压得没那么狠（方向与主张一致，幅度见上）" : "两格同倍数"));

    // ── 敏感性（同一次采集换阈值，零额外引擎成本）。判「分不出」时的分工：换档结论跟着翻
    //    = **阈值没分开**；怎么换都不翻 = **两格本来同分布**。
    std::printf("\n  ── 敏感性（同一次采集换阈值；主档 = 上面那张）──\n");
    std::printf("    %-24s %12s %12s %16s %16s\n", "口径", "A 率比", "B 率比",
        "B 幅度份额(dev关)", "B 幅度份额(超额)");
    const auto sensLine = [&](const std::string& tag, const cv::Mat& lm, const cv::Mat& gm,
                                  double tl, double th, double gthr) {
        const BandTable o = bandTableOf(devOff, lm, gm, tl, th, gthr, tau);
        const BandTable n = bandTableOf(devOn, lm, gm, tl, th, gthr, tau);
        const BandTable e = bandTableOf(excess, lm, gm, tl, th, gthr, tau);
        const BandShare a1 = bandShareOf(o, 1, 1), a2 = bandShareOf(n, 1, 1);
        const BandShare b1 = bandShareOf(o, 2, 1), b2 = bandShareOf(n, 2, 1);
        const double ra = a1.rate > 0.0 ? a2.rate / a1.rate : 0.0;
        const double rb = b1.rate > 0.0 ? b2.rate / b1.rate : 0.0;
        std::printf("    %-24s %12s %12s %15s %15s\n", tag.c_str(), d4(ra).c_str(),
            d4(rb).c_str(), pct2(bandShareOf(o, 2, 1).ampShare).c_str(),
            pct2(bandShareOf(e, 2, 1).ampShare).c_str());
    };
    for (double th : kBandClipSensitivity)
        sensLine("T_hi=" + std::to_string(static_cast<int>(th))
                + (std::abs(th - tHi) < 0.5 ? "（主档）" : ""),
            ref.byteMean, grad, kBandMidByte, th, gradThr);
    sensLine("梯度轴=线性 p75", ref.byteMean, grad, kBandMidByte, tHi,
        percentileOf(gv, 0.75));
    sensLine("梯度轴=线性 p95", ref.byteMean, grad, kBandMidByte, tHi, percentileOf(gv, 0.95));
    sensLine("亮度轴=跨帧最大", ref.byteMax, grad, kBandMidByte, tHi, gradThr);
    sensLine("亮度轴=原生格峰值", ref.bytePeak, grad, kBandMidByte, tHi, gradThr);
    sensLine("T_lo=96", ref.byteMean, grad, 96.0, tHi, gradThr);
    sensLine("T_lo=160", ref.byteMean, grad, 160.0, tHi, gradThr);
    std::printf("    读法：换档后「B 幅度份额」跟着翻过 %s / %s 两条线 = **阈值没分开**；"
                "怎么换都不翻 = **两格本来同分布**\n",
        pct2(kMovingRefuteShare).c_str(), pct2(kMovingSupportAmpShare).c_str());
    return movingBandBranchName(branch);
}

}  // namespace

int cmdTemporalMotion(const std::string& cameraName, const std::string& motion,
    double angleDeg, int burst, int warm, int warmStill, double edgePercentile, double boostFloor,
    double clipMode, double clipSoft)
{
    const BistroCam* cam = nullptr;
    for (const auto& c : kBistroCams)
        if (cameraName == c.name)
            cam = &c;
    if (!cam) {
        std::fprintf(stderr, "机位不存在: %s（可选 %s）\n", cameraName.c_str(),
            bistroCamNames().c_str());
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
    // 批 13.11 消融开关回显（默认全关时**不打**——已发表读数的输出文本也要逐字不变）
    if (boostFloor > 0.0 || clipMode != 0.0) {
        const std::string floorTxt =
            boostFloor > 0.0 ? std::to_string(boostFloor) : std::string("关");
        std::printf("  ⚠ 消融开关：static_boost 地板 = %s（本帧权重）· clip_mode = %g\n"
                    "     spin 下相机静止 ⟹ boost **真生效** ⟹ 这就是 债-49 风险区（静止机位 +\n"
                    "     场景内运动 + 长窗）。跨臂读 dev(M1)/dev(M0)：关盒（臂 1）不显著变差\n"
                    "     = 邻域盒在鬼影上**没承重**，就不必为它付静止闪烁的代价。\n",
            floorTxt.c_str(), clipMode);
    }

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
    // 批 13.11 鬼影 × 裁剪消融：只给 **TAA 开**的组设（m2/m3/m3x/m4/m1m/m4m 全由 m1 复制派生
    // ⟹ 一处生效全覆盖）。TAA 关的组（M0/REFm/REFs）没有历史，臂对它们是空操作。
    // 默认（0 / 0）= 不写键 = 交付态 = 已发表 M0…M4 读数逐位不变。
    m1.staticBoostFloor = boostFloor;
    m1.clipMode = clipMode;
    m1.clipSoft = clipSoft;
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
    refm.wantBytes = true;  // 批 13.9：分带尺子的**亮度轴**只能来自参考图（理由见文件头）
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
    // ⚠ **alpha 载体冲突（债-55 型事故：读数在，读的是别的量）**：臂 6 / 臂 7 把 taa_out 的 alpha
    //   征用成自己的时域状态载体（`taa.frag.slang` 里 `alpha = trust` / `alpha = mem`）⟹ 拒收码表
    //   （0/0.25/0.5/0.75/1）读出来是**垃圾**：mem≈0.25 的像素会被算成「盒裁剪」。这不是精度问题
    //   而是**读错量**，故整段跳过并出声——静默打出一个像模像样的百分比才是最贵的假数据。
    const int clipModeInt = static_cast<int>(clipMode + 0.5);
    const bool alphaOccupied = (clipModeInt >= 6 && clipModeInt <= 9);
    std::printf("\n── 历史拒收掩膜（taa_out alpha；0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 / "
                "0.75 重投影拒收 / 1 无历史）──\n");
    const RejectStats rsM1 = rejectStats(gM1m);
    const RejectStats rsM4 = rejectStats(gM4m);
    auto echoMask = [&](const char* tag, const RejectStats& s) {
        std::printf("  %-3s 收下 %s · 盒裁剪 %s · **盒拒收 %s** · 重投影拒收 %s · 无历史 %s"
                    "  ⟹ 历史被拒合计 %s\n",
            tag, pct2(s.accepted).c_str(), pct2(s.clipped).c_str(), pct2(s.collapsed).c_str(),
            pct2(s.reproject).c_str(), pct2(s.noHistory).c_str(), pct2(s.rejected()).c_str());
    };
    if (alphaOccupied) {
        std::printf("  **本臂跳过**：clip_mode = %d 把 alpha 征用成时域状态载体 ⟹ 拒收码表读出来"
                    "是垃圾（读了就是读错量）\n", clipModeInt);
    } else {
        assertMaskAlive("M1m", rsM1);
        assertMaskAlive("M4m", rsM4);
        echoMask("M1", rsM1);
        echoMask("M4", rsM4);
        std::printf("  ⚠ 读法：M1 的边缘像素若大多住在「盒拒收」桶 ⟹ 历史根本没进混合，"
                    "α（feedback/static_boost）调多小都没用——「窗棂残余」与「相机运动失效」同判\n");
    }

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
    if (!alphaOccupied)
        std::printf("  ★ 历史被拒合计：M1 %s · M4 %s（掩膜 = taa_out alpha，RGB 逐位 = M1/M4；"
                    "「拒」= 盒拒收 + 重投影出画，**不含**盒裁剪 M1 %s / M4 %s = 部分保留）\n",
            pct2(rsM1.rejected()).c_str(), pct2(rsM4.rejected()).c_str(),
            pct2(rsM1.clipped).c_str(), pct2(rsM4.clipped).c_str());
    else
        std::printf("  ★ 历史被拒合计：**本臂不可读**（alpha 被 clip_mode = %d 征用，见上方掩膜段）\n",
            clipModeInt);
    std::printf("  ★ 口径边界：AO 的 1440p/720p 差异是各组共用的偏差地板，只出比值结论；"
                "spin 模式（--motion spin）另量「静止机位 + 动物」，两者不可互推\n");

    // ── 批 13.9：【机位状态 = 相机移动】亮度 × 梯度 二维分带（**只出数 + 一句判决**）──────
    // 不产 Verdict、不进 rc：上面三条判据（激励在场 / 对齐 / 消融）的 rc 语义不许被新表污染。
    std::string bandVerdict = "未出";
    {
        // 差图的两边各自按**自己组**的对齐配对（与上面 devEdge 的口径一致）。两侧配对若不同，
        // 差图里就掺进一个跨配对项 ⟹ 必须回显有多少帧配到了不同的参考帧（自曝，不藏）。
        int pairMismatch = 0;
        for (size_t i = 0; i < aM0.pairIndex.size() && i < aM1.pairIndex.size(); ++i)
            pairMismatch += aM0.pairIndex[i] == aM1.pairIndex[i] ? 0 : 1;
        std::printf("  ★ 分带用图：dev(关) 配对偏移 %+d、dev(开) 配对偏移 %+d，逐帧配对不同的有 "
                    "%d/%zu 帧（差图里掺的跨配对项就来自这些帧）\n",
            aM0.offset, aM1.offset, pairMismatch, aM0.pairIndex.size());
        const cv::Mat devOffMap = movingDevMap(gM0.luma, gRefm, aM0);
        const cv::Mat devOnMap = movingDevMap(gM1.luma, gRefm, aM1);
        bandVerdict = reportMovingBandsAgainstReference(gRefm, grad, tauEdge, tau, devOffMap,
            devOnMap, cameraName);
    }

    logCall("temporal-motion",
        {{"camera", cameraName}, {"motion", motion},
            {"angleDeg", std::to_string(angleDeg)}, {"burst", std::to_string(burst)},
            {"warm", std::to_string(warm)}, {"warmStill", std::to_string(warmStill)},
            {"edgePct", std::to_string(edgePercentile)}, {"R", d4(R)},
            {"R_still_ref", d4(Rbest)}, {"rejectM1", d4(rsM1.rejected())},
            {"rejectM4", d4(rsM4.rejected())},
            {"movingBandVerdict", bandVerdict}},
        vs);
    bool allOk = true;
    for (const auto& v : vs)
        allOk &= v.ok;
    return allOk ? 0 : 1;
}

// ═══════════════════════════════════════════════════════════════════════════════
// temporal-feedback —— **运动态 feedback 扫描**（把「历史在非零分数偏移上被 Catmull-Rom
// 重采样的复利低通」这条假设变成数；只出数 + 一句判决，不改引擎默认值、不动 docs/）
// ═══════════════════════════════════════════════════════════════════════════════
//
// ## 已确立的前提（`temporal-motion` 量的，本命令不重复做）
//
// R = dev(M1)/dev(M0) = 1.138 > 1（相机运动下 TAA **主动变差**），静止可达下界
// dev(M4)/dev(M0) = 0.779；`taa.diag_reject` 显示 M1 收下 90.36% / 盒拒收 **0.00%**
// ⟹ **不是历史被丢，是收下的历史内容不对**。
//
// ## 待验假设（本命令**判它**，不是信它）
//
//   静止 + base_space 下历史采样偏移 ≈0 ⟹ CR 核退化为恒等（权重 (0,1,0,0)）⟹ 零低通；
//   相机一动，**每帧**历史都在非零分数偏移上被 CR 重采样 ⟹ 每帧一次低通、在 ≈1/α 帧的
//   累积窗内**复利** ⟹ 越攒越糊。（与 §1.5/批 13.1e 的「双线性复利」同族**不同因**：
//   那次病灶是核形状，这次是偏移非零。）
//
// 可检验推论只有一个方向：偏差随累积窗变长而恶化 ⟹ 运动态把 `feedback`（= **本帧权重**，
// 调大 = 窗变短）拉高应让偏差**回落**，甚至低于 M0。
//
// ## 为什么 fb=1 那一格不许跳过
//
// fb=1 ⟹ `lerp(clipped, cur, 1) = cur` ⟹ **零累积**，但 **jitter 仍在**（投影照抖、
// base_space 照 un-jitter）。故 R(fb=1) = 「**抖动本身**在运动下贡献了多少偏差」——M0 连
// jitter 都没有。这一格把「累积」与「抖动」两个贡献拆开，也正是判决 ②/③ 的分界。
// 另配 `F1.0js0`（fb=1 且 jitter_scale=0）：按构造 ≈ M0（无抖动 + 无累积 ⟹ 只剩 taa pass
// 本身），是这条拆分的**恒等对照**；它离 M0 越远说明「TAA pass 在图里」这件事本身越不免费
// （AO 施加点从 tonemap 挪到 taa pass = 批 13.6），读数如实打印并进 rc。
//
// ## 判决口径（写死，三条都要能接受——本命令不是用来证明「有 bug 可修」的）
//
//   d(fb) = 运动态边缘带线性域偏差，R(fb) = d(fb)/d(M0)，spread = max d / min d：
//   ① spread < 1.02 ⟹ **与 fb 无关** ⟹ 假设否证。
//   ② d 随 fb↑ 回落**且 min R < 1** ⟹ **假设成立**，修法 = 运动态上调 fb；
//      带内最优 = argmin d，报它相对 M0 的改善倍数。
//   ③ d 随 fb↑ 回落但 **R(fb=1) ≥ 1**（拉到完全不累积仍差于地板）⟹ 病灶**不在累积**，
//      在 **jitter 本身**（运动下抖动没被正确抵消）——这是**另一个根因**。
//
// ## 顺带：静止态窗棂残余是不是同一个根
//
// 静止态 boost 后 frac(swing>τ) 仍有 1.61%。若判成 ③，两者很可能是同一个根（窗棂 =
// 亚像素细结构，对抖动相位最敏感）。对照 = 静止态也扫 fb（0.02/0.12/0.5/1），看 swing 随 fb 的
// **方向与幅度**是否与运动态同比例：同比例 ⟹ 支持统一解释；反向或幅度差一个量级 ⟹ 否证。
// 0.5 也在静止档里：交付建议若是「运动态把 fb 抬到 0.5」，必须能回答「静止态会不会被一起抬」
// ——fb 是同一个字段，不 gate 住就一起被抬，而静止态的窗棂恰恰靠**长窗**（static_feedback=0.01）压。
// swing 口径 = `phaseSwingStat`（同 `temporal-residual --bistro` 的 N1/N2；τ 由静止组自己
// 定、各组共用，与运动表的 τ 是两个数，别混读）。
//
// ## 红线（与 temporal-motion 同源，逐条落点）
//  1. 分辨率钉死 1280×720，每组回显实际帧尺寸（`collectGroup` 自证，不等即抛）。
//  2. 连拍一律 `/shotburst` 逐帧连续。
//  3. 统计在 sRGB 解码后的**线性域**（复用 `prepareLinear`，本文件不另写解码）。
//  4. 交付配置（`deliveryConfigDir` = 仓库 configs/，env/skybox/IBL/AO/shadow 全开）；
//     配置走 `makeProfile` 生成的独立目录，**不碰 build 端 configs 副本**。
//  5. 单实例：`Engine` 构造内 `pgrep -x rgl` 有残留直接拒跑。
//  6. **参数生效自证**：每组回显**引擎自己报的** feedback（`GET /state` 读回，不是写进 lua
//     的值），并在其中一组做**同进程 `/param` A/B**（两张图之间只隔一个 float）证明这个
//     滑杆真的改画面。「写进 lua 没生效」是参数扫描最容易犯的错，而它的症状（曲线是平的）
//     恰好会被读成「该参数无效」这个结论。
//
// ## 热身：两条约束不可同时满足，取哪一条写死在这里
//
// ① 跨进程对齐要求「候选与参考的**绝对帧号**差 ≤ window」，而 yaw = 每帧角 × 绝对帧号
//    ⟹ 同一参考下各组必须同 warm，且 **warm 一变采到的视点就变**（0.2°/帧 × 304 帧 = 61°
//    ⟹ 早已不是门框）。② EMA 瞬态按 (1−α)^t 衰减，小 fb 想要稳态要 warm ≈ 6/α。
// 取 ①：**运动组一律 warm=16**（= 已发表 M0/M1 的那个视点、那台仪器），② 逐行打印成
// `(1−fb)^warm` 作为**已知偏保守**的偏差（fb=0.02 是 0.72 ⟹ 长窗的糊被低估不会被高估）。
// 静止组不受 ① 约束（机位不动 ⟹ 任何两帧同视点），照 ② 用 align16(6/α)。
// 两次翻车的实测记录（每组各自 warm ⟹ 假配对、偏差虚高 10×；整套 warm=304 ⟹ 曲线跑在门框
// 外 61° 的另一个视点上，且 R(fb=1) 与 R(fb=.12) 的大小关系**随视点翻号**）见采集处 ⚠⚠。
//
// ## 跨进程重复性的地板（两轮实测，读结论前先量这个）
//
// 静止行**逐位相同**（0.001419 / 0.004850 / 0.022862 两轮一字不差）；运动行差 0.1–0.5%。
// 根因不在渲染非确定性，在**采集**：burst 的绝对起始帧号跨进程差 1 帧（RPC 机位写入重定基），
// 而 yaw = 每帧角 × 绝对帧号 ⟹ 整条轨迹差 0.2° = 略不同的场景内容。argmin 配对本身是对的
// （候选与参考在同一轮内共享那个偏移），残余是「视点真的不一样」。**故本表只认显著大于
// 0.5% 的结论**；fb=.12 → fb=.5 的落差两轮实测 1.147× / 1.154×，比地板大两个数量级。

int cmdTemporalFeedback(const std::string& cameraName, double angleDeg, int burst,
    double edgePercentile, const std::vector<double>& fbs, double kernel)
{
    const BistroCam* cam = nullptr;
    for (const auto& c : kBistroCams)
        if (cameraName == c.name)
            cam = &c;
    if (!cam) {
        std::fprintf(stderr, "机位不存在: %s（可选 %s）\n", cameraName.c_str(),
            bistroCamNames().c_str());
        return 2;
    }
    if (burst < 32 || burst % static_cast<int>(kJitterPeriod) != 0)
        throw std::runtime_error(
            "--burst 必须是 ≥32 且为抖动周期 16 的整数倍（静止组的 swing 相位折叠要整数个周期）");
    if (fbs.size() < 3)
        throw std::runtime_error("--fb 至少 3 档，否则「随 fb 单调」这个命题根本没法判");
    if (edgePercentile <= 0.0 || edgePercentile >= 1.0)
        throw std::runtime_error("--edge-pct 必须在 (0,1)，它是分位不是百分比");

    const double angle = angleDeg * 3.14159265358979323846 / 180.0;
    const int periods = burst / static_cast<int>(kJitterPeriod);
    const std::string base = projectRoot() + "/build/imgqa_fbsweep_" + cameraName;
    const std::string pre = "fbs_";

    // 热身 = align16(max(16, ⌈6/α⌉))。α = feedback（EMA 记忆长度 ≈ 1/α）。
    auto warmFor = [](double fb) {
        const int raw = static_cast<int>(std::ceil(6.0 / std::max(fb, 1e-6)));
        const int per = static_cast<int>(kJitterPeriod);
        return ((std::max(16, raw) + per - 1) / per) * per;
    };
    auto fbTag = [](double fb) {
        char b[16];
        std::snprintf(b, sizeof b, "%04d", static_cast<int>(std::lround(fb * 100.0)));
        return std::string(b);
    };

    std::printf("=== temporal-feedback：运动态 feedback 扫描（机位 %s）===\n", cameraName.c_str());
    std::printf("  假设 = 「静止 CR 核退化为恒等 ⟹ 零低通；相机一动每帧在非零分数偏移上重采样"
                "⟹ 每帧一次低通、在 ≈1/α 窗内复利」\n");
    std::printf("  推论 = fb↑（窗变短）应让边缘偏差**回落**；fb 是**本帧权重**，调大 = 更信本帧\n");
    std::printf("  运动 = 相机帧驱动 yaw %.4f°/帧 × 帧号（纯函数，= temporal-motion 的 M 组）\n",
        angleDeg);
    std::printf("  参考 = 同机位同帧序列 %dx%d + TAA 关 → 线性域面积平均降到 %dx%d\n",
        kRefSize[0], kRefSize[1], kSize[0], kSize[1]);
    std::printf("  连拍 = %d 帧（/shotburst 逐帧连续，= %d 个抖动周期）；运动组 warm 一律 16"
                "（= 已发表 M0/M1 的视点），静止组 warm = align16(6/α)\n",
        burst, periods);
    std::printf("  配置 = 交付 configs/（deferred + env/skybox/IBL/AO/shadow 全开）；单实例；"
                "统计一律线性域\n");
    std::printf("  输入侧重建核 taa.input_kernel = %g（0 双线性 / 1 CR 4×4 / 2 Lanczos3；"
                "只作用于 TAA 开组，M0/REFm/REFs/S0 = TAA 关 = 不消费本键）\n\n", kernel);
    // ── 采集（每组一个进程；串行，单实例纪律）────────────────────────────────
    //
    // ⚠⚠ **两条互相冲突的约束，取哪一条必须写下来**（本命令在这里翻过两次车，护栏都抓住了）：
    //  ① **跨进程对齐要求「候选与参考的绝对帧号差 ≤ window」**。yaw = 每帧角 × 绝对帧号，
    //     所以同一参考下的各组**必须同 warm**；而且 warm 一变，采到的**视点**就跟着变
    //     （0.2°/帧 × 304 帧 = 60.8° ⟹ 早已不是门框，实测截图 = 街景）。
    //  ② **EMA 瞬态按 (1−α)^t 衰减**，小 fb 想要稳态需要 warm ≈ 6/α（fb=0.02 ⟹ 304 帧）。
    // 两者不可同时满足。试过 ②优先（每组按 6/α 配 warm）：fb≤0.25 的四组与 warm=16 的参考
    // 差 48–288 帧 ⟹ argmin 配出**假对**，边缘偏差虚高 10×，被 `temporal-motion-alignment`
    // 判红（余量 0.0003、前后半窗差 1）。又试过整套 warm=304：对齐全绿、每组都在稳态，但
    // 整条曲线跑在**门框外 61° 的另一个视点**上，与已发表的 M0/M1 不可并读（且 R(fb=1) 与
    // R(fb=.12) 的大小关系**随视点翻号**——跨视点比较无效的直接证据）。
    // ⟹ 取 ①：**运动组一律 warm=16**（= 已发表 M0/M1 的那个视点、那台仪器），把 ② 作为
    // **已知偏保守**的偏差逐行打印 `(1−α)^warm`：fb=0.02 是 0.72 ⟹ 读数里 72% 权重还在第
    // 0 帧上，长窗的糊**被低估、不会被高估**，判决方向不受它影响。
    // 静止组不受 ① 约束（机位不动 ⟹ 任何两帧同视点，配对平凡正确），照 ② 用 6/α。
    const int warmMotion = 16;
    std::printf("── 采集 ──\n  运动组 warm 一律 %d（= 已发表 M0/M1 的视点；同一参考下必须同 warm，"
                "且 warm 会变视点，理由见 ⚠⚠ 注释）\n"
                "  静止组 warm = align16(6/α)（机位不动 ⟹ 配对平凡正确，只需稳态）\n",
        warmMotion);
    std::vector<std::unique_ptr<Group>> slots;
    auto run = [&](const std::string& name, const std::string& sub, const Spec& sp) -> Group& {
        slots.push_back(std::make_unique<Group>(
            collectGroup(base + "_" + sub, name, pre + sub, *cam, sp, burst)));
        return *slots.back();
    };

    Spec m0;
    m0.taa = false;
    m0.motionAngle = angle;
    m0.warm = warmMotion;
    Spec refm = m0;
    refm.refScale = 1;
    Group& gRefm = run("REFm 超采样参考(运动)", "refm", refm);
    Group& gM0 = run("M0 TAA 关 + 运动（地板）", "m0", m0);
    std::vector<std::pair<double, Group*>> motion;
    for (const double fb : fbs) {
        Spec s;
        s.taa = true;
        s.feedback = fb;
        s.kernel = kernel;
        s.motionAngle = angle;
        s.warm = warmMotion;
        motion.push_back({fb, &run("F" + d2(fb) + " 运动 fb=" + d2(fb), "f" + fbTag(fb), s)});
    }
    Spec zs;
    zs.taa = true;
    zs.feedback = 1.0;
    zs.jitterScale = 0.0;  // 零抖动 + 零累积 ⟹ 按构造 ≈ M0
    zs.kernel = kernel;
    zs.motionAngle = angle;
    zs.warm = warmMotion;
    Group& gZero = run("F1.0js0 零态锚点（应 ≈ M0）", "zero", zs);

    Spec refs = m0;
    refs.taa = false;
    refs.motionAngle = 0.0;
    refs.refScale = 1;
    refs.warm = 64;
    Group& gRefs = run("REFs 超采样参考(静止)", "refs", refs);
    std::vector<std::pair<double, Group*>> still;
    Group* probeHost = nullptr;  // 同进程 /param A/B 探针的宿主（必须是静止 + js>0 组）
    // 静止档里带上 0.5 = 运动态的带内最优：交付建议若是「运动态把 fb 抬到 0.5」，必须能
    // 回答「那静止态会不会被搞坏」——静止态的窗棂残余靠**长窗**压（static_feedback=0.01），
    // 而 fb 是同一个字段，不 gate 住就会一起被抬。
    for (const double fb : {0.02, 0.12, 0.5, 1.0}) {
        Spec s;
        s.taa = true;
        s.feedback = fb;
        s.kernel = kernel;
        s.motionAngle = 0.0;
        s.warm = warmFor(fb);
        if (std::abs(fb - 0.12) < 1e-9)
            s.probeFeedback = 1.0;  // 翻到「零累积」：同相位两帧之差只可能来自 fb
        Group& g = run("S" + d2(fb) + " 静止 fb=" + d2(fb), "s" + fbTag(fb), s);
        if (s.probeFeedback > 0.0)
            probeHost = &g;
        still.push_back({fb, &g});
    }
    // ── S0 = **静止 + TAA 关**（批 13.8 第 0 步的地板，缺它「运动专属」这句话没东西可对）──
    // 为什么要这一格：运动态已发表 R(fb=1) = 1.0395 被读成「抖动自身在**运动下**贡献 +3.95%
    // = 位置错」。但 fb=1 = 完全不累积 ⟹ 抖动买不到任何东西、而 un-jitter 那次重采样照付
    // ⟹ fb=1 比 TAA 关差是**应该的**。要判断那 4% 是不是「运动专属」，唯一的对照就是
    // **静止态**同一格：静止 + fb=1 vs 静止 + TAA 关。两边同参考（REFs）、同边缘带（bS）、
    // 同 τ、同 warm 口径（64 = 静止组口径）⟹ 比值里没有跨视点项。
    Spec s0;
    s0.taa = false;
    s0.motionAngle = 0.0;
    s0.warm = 64;
    Group& gS0 = run("S0 静止 TAA 关（静止地板）", "s0", s0);

    std::vector<Verdict> vs;
    // ── 守卫 1：运动激励在场（复用 `temporal-motion-excitation` 的口径与注册项）──────
    {
        std::string obs;
        for (const auto* g : {&gM0, &gRefm})
            obs += g->name.substr(0, 2) + " 相邻帧最大字节差 "
                + std::to_string(static_cast<int>(g->byteMaxAdjacent)) + "  ";
        vs.push_back(verdictFor("temporal-motion-excitation",
            gM0.byteMaxAdjacent > 0.0 && gRefm.byteMaxAdjacent > 0.0,
            obs + "（M0/REFm = TAA 关组，必须 > 0；= 0 ⟺ frame_yaw 没生效）"));
        std::cout << "\n── 守卫 ──\n  " << vs.back().line() << "\n";
        if (!vs.back().ok) {
            std::printf("  ⟹ 立刻停下：运动激励不在场，整张扫描表会跑在静止场景上。\n");
            logCall("temporal-feedback", {{"camera", cameraName}}, vs);
            return 2;
        }
    }

    // ── 守卫 2：feedback **真的传进去了**（红线 6）───────────────────────────────
    // 三条同时成立才算过：① 每组**引擎自己回显**的值 == 请求值；② 回显值互不相同（①的比较
    // 写坏了会恒真，②堵这条自欺路径）；③ **同进程** `/param` A/B 的字节差 > 0（静止机位 +
    // 隔整一个抖动周期 ⟹ 同相位同视点，两张图之间只隔一个 float，见采集处）。
    {
        if (probeHost == nullptr)
            throw std::runtime_error("没有 /param 探针宿主（静止组里没有 fb=0.12？）");
        std::string bad, obs;
        std::set<double> distinct;
        for (const auto& [fb, g] : motion) {
            distinct.insert(g->liveFeedback);
            if (std::abs(g->liveFeedback - fb) > 1e-5)
                bad += "F" + d2(fb) + " 回显 " + d4(g->liveFeedback) + " ";
        }
        const bool readbackOk = bad.empty();
        const bool distinctOk = distinct.size() == motion.size();
        const Group& probeGroup = *probeHost;
        obs = "引擎回显 vs 请求：" + (readbackOk ? "全部相符" : ("**不符** " + bad))
            + " ｜ 互异值 " + std::to_string(distinct.size()) + "/"
            + std::to_string(motion.size())
            + " ｜ 同进程 /param A/B（" + probeGroup.name + "，隔整 1 周期 = 同相位）字节差 "
            + std::to_string(static_cast<int>(probeGroup.probeBytes)) + "、变化 "
            + d2(probeGroup.probeChangedPx * 100.0) + "%";
        vs.push_back(verdictFor("temporal-feedback-param-live",
            readbackOk && distinctOk && probeGroup.probeBytes > 0.0, obs));
        std::cout << "  " << vs.back().line() << "\n";
        if (!vs.back().ok) {
            std::printf("  ⟹ 立刻停下：feedback 没被证明生效，曲线无论长什么样都不能解释。\n");
            logCall("temporal-feedback", {{"camera", cameraName}}, vs);
            return 2;
        }
    }

    // ── 区域与阈值（口径与 temporal-motion 一致：来自参考，不来自候选）──────────────
    // 运动套的掩码来自 REFm、静止套来自 REFs —— **掩码必须住在候选自己的视点上**。已发表的
    // M4 用运动参考的掩码量静止候选（两者差 ~5°）⟹ 掩码与边缘错开、读数被稀释，故静止表
    // 两列并列出数（本命令口径 + 借掩码口径），不判谁对。
    struct Band {
        cv::Mat edge, flat;
        double tau = 0.0;
        double white = 0.0;
    };
    auto bandOf = [&](const Group& ref) {
        Band b;
        cv::Mat gx, gy, grad;
        cv::Sobel(ref.luma.front(), gx, CV_64F, 1, 0, 1);
        cv::Sobel(ref.luma.front(), gy, CV_64F, 0, 1, 1);
        grad = cv::abs(gx) + cv::abs(gy);  // 与 gradEnergy 同式
        auto gv = valuesInMask(grad, {});
        std::sort(gv.begin(), gv.end());
        cv::compare(grad, percentileOf(gv, edgePercentile), b.edge, cv::CMP_GE);
        cv::compare(grad, percentileOf(gv, 0.50), b.flat, cv::CMP_LT);
        auto rv = valuesInMask(ref.luma.front(), b.edge);
        std::sort(rv.begin(), rv.end());
        b.white = percentileOf(rv, 0.99);
        b.tau = lsbLinearAtLuma(b.white);  // 闭式：白电平处 1 个显示 LSB
        return b;
    };
    const Band bM = bandOf(gRefm);
    const Band bS = bandOf(gRefs);
    std::printf("\n  边缘带（梯度 top%.0f%%，各套自算）：运动 %s px · 静止 %s px\n"
                "  τ（白电平 p99 处 1 显示 LSB，闭式）：运动 %s · 静止 %s\n",
        (1.0 - edgePercentile) * 100.0, std::to_string(cv::countNonZero(bM.edge)).c_str(),
        std::to_string(cv::countNonZero(bS.edge)).c_str(), d4(bM.tau).c_str(),
        d4(bS.tau).c_str());

    // ── 对齐 + 读数 ───────────────────────────────────────────────────────────
    const int window = 8;
    struct Row {
        std::string tag;
        double fb = 0.0;
        Deviation dev;
        double margin = 0.0;
        int gap = 0;
        bool curve = false;  ///< 进不进 fb 曲线（地板 / 零态锚点不进）
        bool trust = true;   ///< 对齐护栏是否通过（不通过 = 读数不可解释）
    };
    std::vector<Row> rows;
    auto addRow = [&](const std::string& tag, double fb, const Group& g, const Group& ref,
                          const Band& b, bool inCurve) {
        Row r;
        r.tag = tag;
        r.fb = fb;
        r.curve = inCurve;
        const Alignment a = alignSequence(g, ref, window, true);
        r.margin = a.minMargin;
        r.gap = a.halfGap;
        r.trust = a.minMargin >= 0.02 && a.halfGap == 0;
        r.dev = measureDeviation(g.luma, ref, a, b.edge, b.flat, b.tau);
        rows.push_back(std::move(r));
    };
    addRow("M0", 0.0, gM0, gRefm, bM, false);
    for (const auto& [fb, g] : motion)
        addRow("F" + d2(fb), fb, *g, gRefm, bM, true);
    addRow("F1.0js0", 1.0, gZero, gRefm, bM, false);

    const double devM0 = rows.front().dev.devEdge;

    // ── 守卫 3：对齐护栏（复用 temporal-motion-alignment 的阈值）─────────────────
    {
        double worst = std::numeric_limits<double>::max();
        int gap = 0;
        int untrusted = 0;
        for (const auto& r : rows) {
            worst = std::min(worst, r.margin);
            gap = std::max(gap, r.gap);
            untrusted += r.trust ? 0 : 1;
        }
        vs.push_back(verdictFor("temporal-motion-alignment", worst >= 0.02 && gap == 0,
            "最小 argmin 余量 " + d4(worst) + "（下限 0.02）· 最大前后半窗差 " + std::to_string(gap)
                + "（要求 0）· 组数 " + std::to_string(rows.size()) + "，其中对齐失效 "
                + std::to_string(untrusted) + " 组（失效组的读数不进判决）"));
        std::cout << "  " << vs.back().line() << "\n";
    }

    std::printf("\n── 运动态 A/B 表（线性域；边缘带 = 梯度 top10%%；warm 一律 %d）──\n",
        warmMotion);
    std::printf("  %-10s %6s %10s %8s %9s %10s %8s %8s %8s  %s\n", "组", "fb", "边缘偏差",
        "(÷LSB)", "可见边缘%", "平坦偏差", "梯度比", "dev/M0", "瞬态", "argmin 余量");
    for (const auto& r : rows)
        std::printf("  %-10s %6s %10s %8s %8.2f%% %10s %8.3f %8s %8s  %.4f%s\n", r.tag.c_str(),
            r.fb > 0.0 ? d2(r.fb).c_str() : "off", d4(r.dev.devEdge).c_str(),
            d2(r.dev.devEdge / bM.tau).c_str(), r.dev.fracVisible * 100.0,
            d4(r.dev.devFlat).c_str(), r.dev.gradRatio,
            d4(r.dev.devEdge / std::max(devM0, 1e-12)).c_str(),
            r.fb > 0.0 ? d4(std::pow(1.0 - r.fb, static_cast<double>(warmMotion))).c_str()
                       : "n/a",
            r.margin, r.trust ? "" : "  ⟵ **对齐失效，读数不可解释**");
    std::printf("  「瞬态」= (1−fb)^warm = 读数里还压着第 0 帧的权重比例（fb=0.02 的 0.72 ⟹ "
                "长窗的糊**被低估**，方向保守）\n");

    // ── 守卫 4：零态锚点（fb=1 + js=0 按构造 ≈ M0）+ 回锚（≈ 已发表 M1）───────────
    {
        const Row& zr = rows.back();
        const double ratio = zr.dev.devEdge / std::max(devM0, 1e-12);
        vs.push_back(verdictFor("temporal-feedback-zero-state", ratio <= 1.05,
            "dev(F1.0js0)/dev(M0) = " + d4(ratio)
                + "（上限 1.05）· 无抖动 + 无累积 ⟹ 只剩 taa pass 本身（AO 施加点挪位也记在"
                  "这里）；显著 >1 = 跨进程可比性坏了，本表比值不可信"));
        std::cout << "  " << vs.back().line() << "\n";
    }
    // 回锚：fb=0.12 那一档就是已发表的 M1（同 warm、同视点、同配置），比值必须 ≈ 1.138。
    double r12 = -1.0;
    {
        const auto it = std::find_if(rows.begin(), rows.end(),
            [](const Row& r) { return r.curve && std::abs(r.fb - 0.12) < 1e-9; });
        if (it != rows.end() && it->trust) {
            r12 = it->dev.devEdge / std::max(devM0, 1e-12);
            vs.push_back(verdictFor("temporal-feedback-tieback", std::abs(r12 - 1.138) <= 0.05,
                "dev(fb=.12)/dev(M0) = " + d4(r12)
                    + "（已发表 M1/M0 = 1.138，容差 ±0.05）· 本扫描与那张 A/B 表是不是同一台"
                      "仪器的**唯一**证据；漂了则两边的比值不能并读"));
            std::cout << "  " << vs.back().line() << "\n";
        }
        else {
            vs.push_back(verdictFor("temporal-feedback-tieback", false,
                "扫描档里没有可信的 fb=0.12 ⟹ 无法证明与已发表 M1 是同一台仪器"));
            std::cout << "  " << vs.back().line() << "\n";
        }
    }

    // ── 静止态对照（窗棂残余 = 相位折叠 swing，口径同 temporal-residual --bistro）──
    auto toFrames = [](const Group& g) {
        std::vector<Frame> v;
        v.reserve(g.luma.size());
        for (const auto& m : g.luma) {
            Frame f;
            f.luma = m;
            v.push_back(std::move(f));
        }
        return v;
    };
    // swing 的 τ 由 fb=0.12 那组定（τ 是**场景×机位**属性不是组属性；跨组换 τ = frac 不可比）。
    double tauStill = 0.0;
    for (const auto& [fb, g] : still)
        if (std::abs(fb - 0.12) < 1e-9) {
            const cv::Mat avg = phaseAverageLuma(toFrames(*g), periods);
            auto vals = valuesInMask(avg, {});
            std::sort(vals.begin(), vals.end());
            tauStill = lsbLinearAtLuma(percentileOf(vals, 0.99));
        }
    if (tauStill <= 0.0)
        throw std::runtime_error("静止态 swing 的 τ 定不出来（静止组里没有 fb=0.12？）");

    struct SRow {
        double fb = 0.0;
        int warm = 0;
        ResidualStat st;
        Deviation devAligned;  ///< 边缘带来自 **REFs**（与静止候选同视点）= 本命令口径
        Deviation devBorrowed; ///< 边缘带来自 **REFm** = 已发表 M4 的口径（借来的掩码）
    };
    std::vector<SRow> srows;
    auto stillRow = [&](double fb, const Group& g) {
        SRow r;
        r.fb = fb;
        r.warm = g.warm;
        r.st = phaseSwingStat(toFrames(g), {}, tauStill, periods, nullptr);
        const Alignment a = alignSequence(g, gRefs, window, false);
        r.devAligned = measureDeviation(g.luma, gRefs, a, bS.edge, bS.flat, bS.tau);
        r.devBorrowed = measureDeviation(g.luma, gRefs, a, bM.edge, bM.flat, bS.tau);
        if (r.st.meanLuma <= 0.0)
            throw std::runtime_error("静止组画面是黑的（meanLuma=0）——「没画」不是「稳定」");
        srows.push_back(std::move(r));
    };
    for (const auto& [fb, g] : still)
        stillRow(fb, *g);
    stillRow(-1.0, gS0);  // fb = −1 哨兵 = **TAA 关**（静止地板，批 13.8 第 0 步）
    std::printf("\n── 静止态对照（窗棂残余；swing = 相位折叠 max−median，τ_swing = %s）──\n",
        d4(tauStill).c_str());
    std::printf("  %-8s %5s %10s %10s %8s %10s %8s %10s\n", "fb", "warm", "meanSwing", "p99.9",
        "frac>τ", "边缘偏差", "梯度比", "借掩码偏差");
    for (const auto& r : srows)
        std::printf("  %-8s %5d %10s %10s %7.2f%% %10s %8.3f %10s\n",
            r.fb < 0.0 ? "off(S0)" : d2(r.fb).c_str(), r.warm, d4(r.st.meanSwing).c_str(),
            d4(r.st.p999Swing).c_str(), r.st.fracOverTau * 100.0, d4(r.devAligned.devEdge).c_str(),
            r.devAligned.gradRatio, d4(r.devBorrowed.devEdge).c_str());
    std::printf("  「边缘偏差」的边缘带来自 REFs（与静止候选同视点）；「借掩码偏差」借 REFm 的掩码"
                " = 已发表 M4 的口径（那张表的掩码算自**运动**参考首帧，比静止候选偏 ~5° ⟹ 掩码"
                "与边缘错开、读数被稀释），两列并列出数不判谁对\n");

    // ── 批 13.8 第 0 步读数：**静止态**的「抖动自身代价」= dev(S fb=1) / dev(S0) ──
    // 与运动态 R(fb=1) 同构（那边是 dev(F1.0)/dev(M0)）。两边同 ~4% ⟹ 那 4% 是「无收益
    // 重采样的代价」、与运动无关；静止 ~0% 而运动 4% ⟹ 才是「运动专属」。
    {
        const auto findRow = [&](double fb) -> const SRow* {
            for (const auto& r : srows)
                if (std::abs(r.fb - fb) < 1e-9)
                    return &r;
            return nullptr;
        };
        const SRow* sOff = findRow(-1.0);
        const SRow* sOne = findRow(1.0);
        if (sOff && sOne && sOff->devAligned.devEdge > 1e-12) {
            const double rs = sOne->devAligned.devEdge / sOff->devAligned.devEdge;
            std::printf("  ⭐ **第 0 步**：静止态 dev(fb=1)/dev(TAA 关) = %s"
                        "（运动态同构读数 R(fb=1) 已发表 = 1.0395）\n", d4(rs).c_str());
        }
    }

    // ── 判决 ──────────────────────────────────────────────────────────────────
    // 只用**对齐可信**的曲线组；不可信的逐条点名（它们的大读数是对配对误差的测量）。
    std::vector<std::pair<double, double>> curve;
    std::string dropped;
    for (const auto& r : rows) {
        if (!r.curve)
            continue;
        if (!r.trust) {
            dropped += "F" + d2(r.fb) + "(余量 " + d4(r.margin) + ") ";
            continue;
        }
        curve.push_back({r.fb, r.dev.devEdge});
    }
    std::sort(curve.begin(), curve.end());
    double minDev = std::numeric_limits<double>::max(), maxDev = 0.0, bestFb = 0.0;
    double devAt100 = -1.0, maxFb = -1.0, devAtMaxFb = 0.0, devAt12 = -1.0;
    for (const auto& c : curve) {
        if (c.second < minDev) {
            minDev = c.second;
            bestFb = c.first;
        }
        maxDev = std::max(maxDev, c.second);
        if (c.first > maxFb) {
            maxFb = c.first;
            devAtMaxFb = c.second;
        }
        if (std::abs(c.first - 1.0) < 1e-9)
            devAt100 = c.second;
        if (std::abs(c.first - 0.12) < 1e-9)
            devAt12 = c.second;
    }
    int inversions = 0;
    for (size_t i = 0; i < curve.size(); ++i)
        for (size_t j = i + 1; j < curve.size(); ++j)
            if (curve[j].first > curve[i].first && curve[j].second > curve[i].second * 1.005)
                ++inversions;  // fb 更大反而更差 = 逆序（0.5% 容差吸收跨进程噪声）
    if (devAt100 < 0.0)
        devAt100 = devAtMaxFb;  // 扫描档里没有 1.0 ⟹ 用最大档代替并如实说明

    const double spread = maxDev / std::max(minDev, 1e-12);
    const double rBest = minDev / std::max(devM0, 1e-12);
    const double r100 = devAt100 / std::max(devM0, 1e-12);
    r12 = devAt12 >= 0.0 ? devAt12 / std::max(devM0, 1e-12) : -1.0;

    std::printf("\n── 判决 ──\n");
    if (!dropped.empty())
        std::printf("  ⚠ 对齐失效、已排除出判决的组：%s（它们的大读数是**配对误差**，不是渲染）\n",
            dropped.c_str());
    std::printf("  回锚 dev(fb=.12)/dev(M0) = %s（已发表 = 1.138）· 地板 dev(M0) = %s\n",
        d4(r12).c_str(), d4(devM0).c_str());
    std::printf("  曲线（%zu 档可信）spread = max dev / min dev = %s（判据①线 1.02）· 逆序对 %d"
                "（0 = 随 fb↑ 单调回落）\n",
        curve.size(), d4(spread).c_str(), inversions);
    std::printf("  min R = %s（fb=%s）· R(fb=1) = %s ⟹ **抖动本身**在运动下的贡献 = %s× 地板"
                "· R(fb=.12) = %s\n",
        d4(rBest).c_str(), d2(bestFb).c_str(), d4(r100).c_str(), d4(r100).c_str(),
        d4(r12).c_str());
    if (curve.size() < 3) {
        std::printf("\n  ★ 可信档位不足 3 档，**拒绝出判决**（对齐失效 %s）\n", dropped.c_str());
    }
    else {
        std::string verdict;
        if (spread < 1.02)
            verdict = "① **偏差与 fb 无关**（spread " + d4(spread)
                + " < 1.02）⟹ 假设**否证**：累积窗长短不是运动态偏差的主因，得换变量找";
        else if (r100 >= 1.0 && rBest >= 1.0)
            verdict = "③ **病灶不在累积，在 jitter 本身**：fb 拉到 1.0（完全不累积）仍是地板的 "
                + d4(r100) + "×，且带内最小也有 " + d4(rBest)
                + "× ⟹ 缩短窗收不回来，**运动下抖动没有被正确抵消**";
        else if (rBest < 1.0 && bestFb < maxFb && r100 > rBest * 1.02)
            verdict = "②′ **方向成立但不是单调**：偏差随 fb↑（窗变短）先回落到带内最优 fb="
                + d2(bestFb) + "（地板的 " + d4(rBest) + "×，已低于 TAA 关），随后 fb→1 又反弹到 "
                + d4(r100) + "× ⟹ 「累积」与「抖动」是**两份独立误差**：前者 fb 能调掉，后者"
                  "（抖动本身 " + d4(r100 - rBest) + "×）调不掉";
        else if (rBest < 1.0)
            verdict = "② **假设成立**：偏差随 fb↑ 单调回落到 fb=" + d2(bestFb) + "（地板的 "
                + d4(rBest) + "×，<1）"
                + (std::abs(bestFb - maxFb) < 1e-9
                        ? "；⚠ 最优在**端点** = 「几乎不累积」，不是「上调 feedback 有最优」"
                        : "；最优在带内，修法 = 运动态上调 feedback");
        else
            verdict = "介于两可：spread " + d4(spread) + "、逆序对 " + std::to_string(inversions)
                + "、minR " + d4(rBest) + "、R(fb=1) " + d4(r100) + "，四条判决线都未越过，"
                  "不能一句话定性";
        std::printf("\n  ★ %s\n", verdict.c_str());
    }

    // 静止/运动两侧的 fb 敏感度（「窗棂残余与运动失效是否同一个根」的对照）
    {
        auto swingAt = [&](double fb) {
            for (const auto& r : srows)
                if (std::abs(r.fb - fb) < 1e-9)
                    return r.st.meanSwing;
            return 0.0;
        };
        auto fracAt = [&](double fb) {
            for (const auto& r : srows)
                if (std::abs(r.fb - fb) < 1e-9)
                    return r.st.fracOverTau;
            return 0.0;
        };
        auto devAt = [&](double fb) {
            for (const auto& c : curve)
                if (std::abs(c.first - fb) < 1e-9)
                    return c.second;
            return 0.0;
        };
        const double sSwing = swingAt(0.12) > 0.0 ? swingAt(1.0) / swingAt(0.12) : 0.0;
        const double sFrac = fracAt(0.12) > 0.0 ? fracAt(1.0) / fracAt(0.12) : 0.0;
        const double mDev = devAt(0.12) > 0.0 ? devAt(1.0) / devAt(0.12) : 0.0;
        std::printf("  ★ 统一解释的对照（fb .12→1 的同向变化倍数）：静止 meanSwing ×%s"
                    "（frac>τ ×%s）｜ 运动 边缘偏差 ×%s\n",
            d4(sSwing).c_str(), d4(sFrac).c_str(), d4(mDev).c_str());
        std::printf("    读法：两侧**同号**（fb↑ 让同一个量同向变）⟹ 支持「同一个根 = 抖动」；"
                    "**反号**（静止变差 / 运动变好或反之）⟹ 否证，窗棂残余与运动失效是两件事\n");
    }
    std::printf("  ★ 口径边界：AO 的 1440p/720p 差异是各组共用的偏差地板，只出比值结论；"
                "运动套与静止套的掩码和 τ 互不通用；换 warm = 换视点，跨 warm 的数不可并读\n");

    logCall("temporal-feedback",
        {{"camera", cameraName}, {"angleDeg", std::to_string(angleDeg)},
            {"burst", std::to_string(burst)}, {"edgePct", std::to_string(edgePercentile)},
            {"warmMotion", std::to_string(warmMotion)}, {"devM0", d4(devM0)},
            {"bestFb", d2(bestFb)}, {"Rbest", d4(rBest)}, {"Rfb1", d4(r100)},
            {"Rfb12", d4(r12)}, {"spread", d4(spread)}, {"inversions", std::to_string(inversions)},
            {"dropped", dropped}, {"tauM", d4(bM.tau)}, {"tauStill", d4(tauStill)}},
        vs);
    bool allOk = true;
    for (const auto& v : vs)
        allOk &= v.ok;
    return allOk ? 0 : 1;
}

}  // namespace RGL::imgqa
