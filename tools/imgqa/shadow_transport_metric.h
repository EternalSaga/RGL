#ifndef RGL_IMGQA_SHADOW_TRANSPORT_METRIC_H
#define RGL_IMGQA_SHADOW_TRANSPORT_METRIC_H
//
// shadow_transport_metric.h — **像素域地面搬运**的度量口径层（债-139 结案批的共用算子）
//
//   缝同 `shadow_seam_metric.h` / `ground_field_metric.h`（「口径 vs 仪器」那条既有缝）：本层只有
//   算子（不启引擎、被 `just imgqa selftest` 用闭式输入直测、`just tools-check` 覆盖得到）；
//   「臂表 / 采集 / 闸门 / 判决线」住 `shadow_swim_flat.cpp`。
//
//   ## 为什么又开一层，而不是复用 `ground_field_metric`（这条必须写清，否则本层就是抄第二份）
//
//   `ground_field_metric` 把两枚机位各自的像素**栅格化成世界二值场**（5 cm 格 + 格内多数票）再按
//   格对拍。本层问的是同一件事（同一个世界点在两机位下影状态是否相同），但口径**必须**不同——
//   两条都是实测逼出来的，不是审美：
//
//   ① **精度域**。`ground_field_metric` 的重投影**刻意保持 float32**（它自己的头注写了理由：
//     `shadow-swim` 的判据是 `max(20, 共同/200)` 这种**计数**阈值，换 double 会挪动落在格边界上的
//     像素 = 改判据）。float32 在 |x| = 1e6 处 ULP = **0.0625 m** ⟹ 那一档的世界点被量化到 6.25 cm，
//     比本仪器要分辨的 5.7–24 mm 纹素还粗。四格对拍**必须**跨大坐标工作点 ⟹ 本层在 double 域
//     重投影。（债-159：那枚工作点现在由靶的 manifest 给且**不在 float32 网格上**，本层不抄它。）
//     这不是「把那份改好」——那份改不得（会改 `swim` 的判据），所以是**两个精度域各自的算子**，
//     各自有各自的自检与理由。
//   ② **分辨率**。5 cm 格 + 多数票对**毫米级刀锋微影场结构性不可见**（债-139 ⓓ 点过 `swim` 这条瞎，
//     本批实测钉死：同一份数据 5 cm 格读 0.036 %、像素域读 0.256 %，差 7 倍）。本层在像素域对拍，
//     分辨率 = 像素足迹（本靶 6 m 处 ≈ 5 mm）。
//
//   ## 本层唯一的新概念：影缘侵蚀带（🔴 它才是「无地板」三个字的载体）
//
//   像素域搬运要在 B 的网格上双线性采样，而影场是刀锋场（影缘在 1–2 像素内从受光跳到本影）。
//   于是**即使影场逐格世界不变**，两枚机位仍会在「影缘两侧若干像素」内翻标签——那是量具的
//   **重采样地板**，不是阴影真的变了。债-139 卡在鹅卵石起伏上的 2.73 % 与这个地板混在一起分不开。
//   本层的处置：把影缘邻域显式标出来并从分母里剔掉，剩下的「内部像素」上任何翻转都不是重采样
//   能造出来的 ⟹ 那才是**无地板读数**。实测（本靶出厂点门关、两机位差 0.5 m）：
//     erode 0 像素 → 0.2558 %；erode 2 像素 → **恰 0.0000 %**；erode 4 → 0.0000 %。
//   ⟹ 侵蚀半径就是本量具的地板刻度，报数必须连它一起报（`ShadowFlipReadout::erodePx`）。
//
//   ## 像素中心约定（首版在这里错过一次，写下来防重犯）
//   `projectPoint3` 回的是 `px = (ndc.x·0.5+0.5)·cols`（**像素边缘**坐标），而 `groundHitOf` 的正向
//   用的是 `(x+0.5)/cols·2−1`（**像素中心**）。两者差半个像素，直接对接 = 给整张搬运图加一个恒定
//   半像素偏移 ⟹ 在所有影缘上造出一片**假的**翻转。本层在 `projectSamples` 里减回 0.5 对齐，并由
//   selftest 的**往返恒等锚点**钉住（同机位下「像素→世界→像素」必须落回原像素 ±1e-6）。
//
//   ## 变异红线（哪个变异让本层红；🔴 下面每条都**实跑过**，写的就是实跑读数）
//   · M1 把平移搜索钉成恒零位移（`rounds` 直接 return）→ **1 条红**：「注入 (0.30,0.30) m 世界滑移
//     → 同号报回且该处失配恰 0」，报 (0.000000, 0.000000) 失配 16526/77193。任务书钦定的那条。
//   · M2 侵蚀带写回「闭运算 − 开运算」（对半平面恒零）→ **4 条红**：「侵蚀 2 像素后翻转恰 0」
//     （翻转 88）、「Σ|Δcov| 落在浮点闭合级」（Σ = 154.7）、「注入平移同号回收」（失配 497）、
//     「世界不变的场 → 最优平移恰为零位移」（失配 103）。
//   · M3 删掉 `projectSamples` 的 −0.5（像素边缘/中心混用）→ **1 条红**：「往返恒等」，最大偏差
//     **恰 0.500000** 像素。这条一红就说明四格所有影缘翻转都是半像素假号。
//   · M4 侵蚀带只查 A 臂 → **1 条红**：「B 臂的带单独也给就剔到点」，报「并集 18587 只A 18587
//     只B 0」——B 侧的缘整个留在分母里。
//   · ⚠ M5 把分母改回随候选缩水（每候选重建 `buildFixed`）→ **0 条红，变异存活**。原因见
//     `searchWorldShift` 头注 ①：真正拦住「挪到没影处取巧」的是 coverageFloor 与目标函数，不是
//     分母固定。⟹ **那条设计目前没有机器判据**（待开债；本批交付说明已如实报出，未登记进
//     docs/还债总账.md——那份台账不在本批授权范围内）。
//   · 恒等场锚点：同一张场对拍 ⟹ 翻转**恰为 0**、|Δcov| **恰为 0**（阳性锚点）。
//   · 反向用例：不同世界场翻转率 ≥ 20 %、无关场搜完仍有大量失配（堵「算子恒绿」与「搜索总能
//     找到 0」两种相反的失效）。
//   · 覆盖率下限：零可信基准必须 `found = false`（不许把「没有数据」报成「最优是零位移零失配」）。
//   · 类型/尺寸守卫各一条 `throwsStd`：改成返回空读数即红（空读数 = 零翻转 = 假绿）。
//   · 判据名拼错或漏 append → 挂号那条红。
//
#include "field_diff_metric.h"    // meanAbsPermille / p99Abs（连续量口径，与 material-swim 共用）
#include "plane_reconstruct.h"     // **平面接收面重建层**（本层的 y=0 特例全部转发到它）
#include "shadow_kernel_metric.h"  // Projector / makeProjector / projectPoint3（复用，别重写）

#include <opencv2/core.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace RGL::imgqa {

/**
 * 一枚机位下「像素 → 世界 y=0 平面」的采样点集（double 域，见头注 ①）。预计算一次、平移搜索
 * 时复用：搜索只改「世界点 → B 的像素」那一步，重投影不重算。
 *
 * 🔴 **它就是 `PlaneSamples`**（本层不再自己存 (wx, wz)）：接收面重建的口径住在
 * `plane_reconstruct.h`，本层只是它的 **y = 0 特例别名**。另立一份在只有一台平地仪器时还
 * 说得过去，到竖直门面那台就不成立了——复制一份改个名字 = 两份会各自漂的「同一件事」，
 * 漂了之后两台仪器报的两个「世界不变性残差」看着可比、其实不可比（AGENTS 哲学第 1 条）。
 * 用别名而不是新类型 ⟹ 两台仪器**结构上不可能**用不同的重建口径。
 */
using GroundSamples = PlaneSamples;

/**
 * 像素 (x,y) 的视线与世界 y=0 平面交点，**double 域**（`groundHitOf` 的高精度同式版）。
 * 🔴 实现 = `planeHitDouble(…, groundPlane())` 的**转发**（wx = u、wz = v），本函数一份算式
 * 都不留。与 `projectPoint3` 严格互逆（selftest 的往返锚点钉这条）。不可量 = false：
 * 视线近平行地面、交点在相机背后、交点距离超 `maxDistanceM`（掠射区一像素跨数米，不进判据）。
 */
[[nodiscard]] bool groundHitDouble(const Projector& p, int x, int y, double maxDistanceM,
    double& wx, double& wz);

/// 遍历 `from` 的全部像素，留下能打到地面且在距离上限内的采样点（顺序 = 行优先，确定）。
[[nodiscard]] GroundSamples sampleGround(const Projector& from, int cols, int rows,
    double maxDistanceM);

/// 把 `s` 的世界点整体挪 (sx, sz) 后投进 `into` 的网格，写回**像素中心**坐标（见上节约定）。
/// `dstX/dstY` 与 `s.srcIndex` 等长；投不出（相机背后 / 屏外）= NaN，调用侧跳过。
void projectSamples(const Projector& into, const GroundSamples& s, double sx, double sz,
    std::vector<double>& dstX, std::vector<double>& dstY);

/// 单张 cov 场的影缘带（8UC1，255 = 该像素 `rad` 邻域内存在二值影状态不同的像素）。
/// 🔴 带 = 膨胀 − 腐蚀；写成「闭运算 − 开运算」对半平面**恒零**（首版错过，见 .cpp 那条）。
[[nodiscard]] cv::Mat edgeBandMask(const cv::Mat& cov, int rad);

/**
 * 光度自吻合可信集（债-139 的既有口径，本层只是把它从「世界格」搬到「像素搬运」上）。
 * 判据：无阴影臂 A 的像素亮度，与无阴影臂 B 在**搬运后**对应位置的亮度，线性域相对差 ≤ `relTol`。
 * 为什么需要它：两机位下「同一个像素」根本不是「同一个世界点」，只有把 B 搬过来比才有意义；
 * 而垂直板面、天空这类打不到地面的像素，靠这条被自然剔掉（它们的搬运对应关系不成立）。
 * @param lumaA/lumaB  两臂的**线性域** Rec709 亮度（CV_64FC1；sRGB 域比 = 把暗部差异压掉，别用）
 * @return 与 lumaA 同尺寸的 8UC1 掩码（255 = 可信）
 * 🔴 阴性对照（同机位两次）下这张掩码几乎全 255，而它上面的翻转恰 0 —— 那才是「量具有读数能力」
 *   与「引擎是确定的」两件事同时成立的证据，缺一不可。
 */
[[nodiscard]] cv::Mat photometricTrustMask(const cv::Mat& lumaA, const cv::Mat& lumaB,
    const GroundSamples& s, const std::vector<double>& dstX, const std::vector<double>& dstY,
    double relTol);

/// 逐采样点的对拍读数。分母口径与侵蚀带都在里面，🔴 报数时三者必须一起报（离开 erodePx
/// 的翻转率没有意义，离开 trust 的百分比是彩票）。
struct ShadowFlipReadout {
    size_t sampled = 0;      ///< 搬运有效的采样点数（侵蚀剔除之前）
    size_t edgeExcluded = 0;  ///< 落在影缘侵蚀带里被剔除的点数（= 地板的规模，本身是个读数）
    size_t trust = 0;        ///< 可信点数（搬运有效 ∧ 不在侵蚀带）= 翻转率的分母
    size_t flips = 0;        ///< 可信集上二值影状态翻转的点数
    double sumAbsCov = 0.0;  ///< 可信集上 |Δcov| 之和（连续口径：没跨阈值的差在这里，翻转率看不见）
    double p99AbsCov = 0.0;  ///< 可信集上 |Δcov| 的 p99（尾部：本影整体错位靠它，不靠均值）
    int erodePx = 0;         ///< 🔴 本次读数的侵蚀半径 = 读数的**刻度**；0 = 债-139 原口径（含地板）

    [[nodiscard]] double flipPct() const;
    [[nodiscard]] double meanAbsCovPermille() const;
};

/**
 * 两覆盖度场经搬运后的对拍读数。
 * @param covA/covB  A、B 网格的 cov 场（CV_64FC1，`seamCovField` 的产物——🔴 本层不定义 cov）
 * @param dstX/dstY  与 `s.srcIndex` 等长的目标像素中心坐标（`projectSamples` 的产物，NaN = 作废）
 * @param keep       逐 A 像素的可信掩码（光度自吻合；空 = 全可信）
 * @param bandA/bandB  两臂的影缘带（`edgeBandMask` 的产物；空 = 不侵蚀）
 * @param erodePx    本次读数的侵蚀半径，只进读数做记录（0 = 债-139 原口径，含地板）
 * 二值影状态 = `cov > kSeamBin`（**严格 >**，与 shadow-grain / shadow-cascade-seam 同口径）。
 * 尺寸/类型不符 ⟹ **抛**（返回空读数会被读成「零翻转 = 判据通过」）。
 */
[[nodiscard]] ShadowFlipReadout compareTransported(const cv::Mat& covA, const cv::Mat& covB,
    const GroundSamples& s, const std::vector<double>& dstX, const std::vector<double>& dstY,
    const cv::Mat& keep, const cv::Mat& bandA, const cv::Mat& bandB, int erodePx);

/// 平移搜索的一个候选读数（最优 = 失配最小；同分取覆盖更高者，与 `searchShift` 同式）。
struct ShiftCandidate {
    double sx = 0.0;
    double sz = 0.0;
    size_t flips = 0;
    size_t trust = 0;
    double mismatch = 0.0; ///< 该平移下的 Σ|Δcov|（固定分母上的**次目标**，见 `searchWorldShift` ②）
    bool found = false;  ///< 一个可信候选都没有 ⟹ false（不许伪装成「最优是零位移零失配」）
};

/**
 * 在世界平面上搜「把 B 场挪多少最能对上 A 场」——**最优平移 = 影场在世界里滑了多少**。
 * @param rounds  由粗到细的多轮网格（每轮一批 (sx,sz) 偏移）；🔴 轮次与每轮候选数全由调用侧
 *   给死，本层不自己加轮次、不做收敛循环 ⟹ **没有无界工作**（本仓被无界搜索卡过一次，别再犯）。
 * @param center  本轮所有候选的**基准点**：细轮以已找到的最优为中心（否则细网格绕着 0 转，
 *   粗轮报 0.5 而细轮只能在 0±0.02 里找 = 白跑）。
 * @param coverageFloor  候选覆盖率下限（相对零位移可信点数；不足即该候选不可信，跳过）
 *
 * 🔴 **两条设计，但只有第二条有机器判据**（本批变异实跑的结论，别把第一条当成已被保护）：
 * ① **分母固定在零位移**（`buildFixed`）。动机是真的：首版每个候选重新过四道闸，分母随候选缩水
 *   （实测零位移 136474 点 → 0.5 m 处 114751 点，−16 %），候选之间因此**不可比**。
 *   ⚠ **但「把分母改回随候选缩水」这个变异（M5）在本仓任何闭式用例上都不红**——因为真正拦住
 *   「挪到没影的地方取巧」的是 `coverageFloor`（0.8）与目标函数本身，不是分母固定。所以 ① 目前是
 *   **防御性设计 + 无判据**：它让「候选可比」这件事由构造成立，但构造被拆掉时没有测试会出声。
 *   按本项目「写不出机器判据的『必须』当场开债，不许只留注释」的规矩，这条**待开债**（本批交付
 *   说明里已如实报出，未登记进 docs/还债总账.md——那份台账不在本批授权范围内）。
 * ② **目标 = Σ|Δcov|（连续量）**，不是翻转数。这条**有**判据（selftest ⑨/⑨b/⑩，且 M1/M2 都红）。
 *   实机依据：本靶**二值本影只占 0.14 %**（cov > 0.5 只有 2000 像素——这块平地上太阳只占受光的
 *   三成）。注入 0.5 m 的对照臂上，翻转数在零位移读 5、在真答案 (0.50, 0.00) 处读 34 ——
 *   **翻转数会把正确答案判成更差**。同一批点上 Σ|Δcov| 读 5.487 ‰ → 0.382 ‰（14 倍），干净指回
 *   注入量。首版用翻转数优先，实机就把 J-F5 判成红、并把最优平移报到 (−0.54, +0.22)。
 *   同失配时取**平移最小者**（Occam），再同分取翻转少者、覆盖高者。
 */
[[nodiscard]] ShiftCandidate searchWorldShift(const cv::Mat& covA, const cv::Mat& covB,
    const GroundSamples& s, const cv::Mat& keep, const cv::Mat& bandA, const cv::Mat& bandB,
    const Projector& into,
    const std::vector<std::vector<std::array<double, 2>>>& rounds, double coverageFloor,
    const std::array<double, 2>& center = { 0.0, 0.0 });

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_TRANSPORT_METRIC_H
