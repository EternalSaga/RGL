#ifndef RGL_IMGQA_SHADOW_SEAM_METRIC_H
#define RGL_IMGQA_SHADOW_SEAM_METRIC_H
//
// shadow_seam_metric.h — `shadow-cascade-seam`（相邻级联阴影分歧）的**度量口径层**
//
//   缝同 `shadow_grain_metric.h`（「口径 vs 仪器」那条既有缝）：`shadow_cascade_seam.cpp`
//   （仪器：臂表 + 采集 + 引擎回显对账 + 🔴 簇路径生效闸门 + 入口编排）与本层（口径：
//   两张截图 → 逐像素覆盖度场 → 翻转率 / 逐行台阶）不是同一条改动缝——改臂 / 阈值 / 机位
//   动前者，改测量算子动后者，而**只有后者被 `just imgqa selftest` 用闭式输入直接测**
//   （不启引擎、毫秒级，故 `just tools-check` 覆盖得到）。
//
//   ## 口径为什么长这样（缺陷本体与「为什么必须用差分」的论证全文在仪器层头注释）
//   缺陷类 = **相邻级联对「这块表面有没有影子」给出不同答案**。已定死的实例 = 债-104 的
//   远级簇 LOD（`rgl/render_list_mdi.cppm:284` 的 `if (c >= 1u && …)` ⟹ 级 0 画全量几何、
//   级 1..3 画粗簇 LOD）。量它的唯一无偏仪器 = 诊断键 `csm.force_cascade` 把全场景钉到
//   某一级采样（纯覆写 scene data 的 `splits`，**不改级联几何**），force=c 与 force=c+1
//   两臂**唯一差别 = 采哪张阴影图** ⟹ 本层算的全部分歧都可归因到「两级各自怎么说」。
//
//   覆盖度 cov 与 `shadow_grain` **同式同序**（线性域、逐通道 clip 后取 RGB 均值），
//   差别只在 on 臂是「钉级后的 csm」而不是「现行 csm」——cov 的定义因此不另立一份，
//   两个判据共享同一条 sRGB 解码路径（`srgbDecode`，imgqa.h 硬约束第 2 条）。
//   ⚠ 「逐通道 clip → 再取均值」这个顺序是口径的一部分（与"先均值再 clip"只在某通道
//   变亮的像素上分家，实机翻转率差 +0.014pp），selftest 有专门一条钉它。
//
//   ## 变异红线（哪个变异让我红——全部由 `selftest_seam.cpp` 钉死）
//   · 去掉线性域转换（直接拿 sRGB 字节算 cov）→ 「线性域锚点」红：cov(白,128灰) 闭式
//     0.78406109，字节域给 0.49798941——差 0.29，不是噪声量级。
//   · 删 eps（分母写成 off）→ 「参照全黑」红（0 → NaN；NaN 传染进差分场后所有比较全
//     false = 整窗读 0 = 缺陷态刷绿）。
//   · clip 下界删掉（on 比 off 亮的像素给负值）→ 「变亮不计」红。
//   · clip 上界删掉 → 「off=黑 on=白 恰 1/(1+eps)」红。
//   · 通道均值改成取 max / 取 luma → 「三通道取均值」红（闭式 0.59465370）。
//   · flipFraction 的 `> thresh` 弱化成 `>= thresh` → 「恰等于阈值不计」红；
//     弱化成 `>= 0`（恒真）→ 「全影全亮两臂 = 0」红（给 1.0）。
//   · flipFraction 的分母写成整幅而非 band → 「band 外不参与」红（合成对上分母不同）。
//   · maxRowStep 的台阶从「上下两窗中位之差」改成「逐像素差」→ 单条台阶 / 常数场两条红
//     （后者给噪声量级而非恰 0）。
//   · maxRowStep 取 max 改成取均值 / 取末行 → 「最硬台阶所在行」红（回传行号错位）。
//   · halfWidth 不参与窗口计算（写死 6）→ 「halfWidth=1 的窄窗」红。
//   · 四条守卫任一改成返回 0（band 空 / band 越界 / halfWidth ≤ 0 / 两图尺寸不同）→
//     对应那条 `throwsStd` 红。**返回 0 会被读成「分歧为零 = 判据通过」**，所以守卫一律抛。
//   · **参照臂闸门**（`requireBandValid`）的比较 `>` 反向成 `<=`（= 永不抛）→ 「略大于界应抛」
//     红，且实机 W2 那条全幅带子不再抛、直接吐 J1 绿（2026-10-19 实测：带子换成 x∈[160,1440)
//     时参照臂台阶 0.239 @ 行 51，J1 差只剩 +0.088 ≤ 0.1 = **缺陷在场的假绿**）。
//   · 闸门的界从 `kSeamTexelFloor` 换成更大的数（调用点或 selftest 的 floor 参数）→
//     「略大于界应抛」红；界换成 0 → 「恰等于界算通过」红（两条各钉一半，对称判据每半边一条）。
//   · **闸门 A**（`requireDenominatorAlive`）的比较 `diff > 0` 恒真化（永不抛）→ 「diff=0 应抛」
//     与「两条同时死一次报全」红。⚠ 实机上它**不会**让 X2（分母换成分子自身）变绿——那道本来
//     就是 B 拦的（A 比的是图，图没变）；它真正让实机退化的是 X3 那类形态：**仍抛 rc=3，但归因
//     从「分母没让画面少过一个像素」退化成「阴影没进画面」**，人会去查场景有没有影而不是查
//     分母接线（覆盖关系见该函数声明处的实测段）。
//     反向（`diff > 0` 写反成永不放行）→ 「正常态不抛」红（仪器永久红）。
//   · **闸门 B**（`requireCoveragePresent`）的下界从 1% 挪到 0.0 → 「0.005 应抛」红（0.005
//     > 0 不抛）；判据从「cov > 0.5 的**占比**」换成「cov **均值**」→ selftest 两条算子断言
//     红（常数场 0.6 的占比是 1.0、均值是 0.6，两个闭式期望立刻分家）。
//   · `coverageFraction` 的分母写成整幅而非 band / 阈值写成 `>=` → 对应两条闭式断言红。
//
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {

/// 参照下限 eps：off（mode=none 臂）为黑的像素上 (off−on)/(off+eps) 不爆（0/0 → 0）。
/// 与 `kGrainEps` 同值同理由（黑参照上没有「影」可谈），但**各留一份**：两条判据的口径
/// 一旦有一边要改（例如换分母下限），共享常量会把另一条一起改掉。
inline constexpr double kSeamEps = 1e-4;

/// flipFraction 的默认分歧阈值（**严格 >**，恰等于阈值不计影状态翻转）。
/// 0.5 不是拍的：cov ∈ [0,1] 且「这一级说有影 / 那一级说没有」的两个闭式端态是 0 与 1，
/// 阈值取两端中点 = 只有**跨越半影**的分歧才算翻转，半条影缘以内的错位不算。
inline constexpr double kSeamBin = 0.5;

/// maxRowStep 的默认上下窗半高（行）。6 = 与主 agent 定口径时的 seam.py 参考实现同值；
/// 它是量具的组成部分（改它 = 改判据），**不是**可调 flag。
inline constexpr int kSeamHalfWidth = 6;

/// maxRowStep 的读数（台阶最大值 + 它所在的行；行号是判据的**可归因输出**——
/// 债-139 那条缝就靠它落到「第 472 行」这个可复核的事实上）。
struct SeamStep {
    double step = 0.0;
    int row = -1;
};

/**
 * 逐像素阴影覆盖度 cov = 逐通道 clip((off − on)/(off + kSeamEps), 0, 1) 后取 RGB **均值**，
 * **线性域**。
 * on = 钉级后的 csm 臂，off = 同机位同帧的 mode=none 臂（开关对拍：albedo / 曝光 / ACES
 * 全部约掉，剩下的只有阴影项）。必须先 sRGB 解码再算比值（硬约束第 2 条）。
 * 两图尺寸不同 / 非 CV_8UC3 立刻**抛**（不是同一帧的读数没有意义，返回 0 会被读成零分歧）。
 */
[[nodiscard]] cv::Mat seamCovField(const cv::Mat& onBgr, const cv::Mat& offBgr);

/**
 * 两覆盖度场在 band 内的**阴影状态翻转率** = |{p ∈ band : |a(p) − b(p)| > thresh}| / |band|。
 * 闭式刻度：两臂逐位相同 → 恰 0；band 内一半像素从「全受光」翻到「全本影」→ 恰 0.5。
 * band 空 / band 越界 / 尺寸不同 / 非 CV_64FC1 → **抛**（空分母的「0 翻转率」= 假绿）。
 */
[[nodiscard]] double flipFraction(const cv::Mat& a, const cv::Mat& b, const cv::Rect& band,
    double thresh = kSeamBin);

/**
 * 差分场 dif 在 band 内的**最硬横向台阶**：逐行 y 取
 *     step(y) = p90_x |median(dif[y−h:y]) − median(dif[y:y+h])|   （x ∈ band 列）
 * 再对 y ∈ band 行取最大值，并回传所在行。
 * 为什么是「上下两窗中位之差」而不是逐像素差：一条缝在屏幕上占若干行，逐像素差被
 * 影场自身沿 y 的自然梯度淹掉；两窗中位之差把梯度压成 0、把台阶留住（seam.py 参考实现同式）。
 * 为什么取 p90 而不是 max：单列的量化抖动不该决定一条判据；p90 = 「这一行的分歧是
 * 普遍的还是只有一根线」。
 * band 空 / band 越界（含上下窗越出图外）/ halfWidth ≤ 0 / 非 CV_64FC1 → **抛**。
 */
[[nodiscard]] SeamStep maxRowStep(const cv::Mat& dif, const cv::Rect& band,
    int halfWidth = kSeamHalfWidth);

/**
 * **参照臂有效性闸门**（量具自检，不是画质判据）：参照臂在同一带子里的最硬横向台阶
 * `refStep` 必须 ≤ 固有下界 `floor`，超界 ⟹ **抛**，调用方因此产不出任何 Verdict。
 *
 * 为什么这条界是闭式可辩护的（不是"实测没超所以钉个阈值"）：参照臂画的是"四级同一套全量
 * 几何"（`shadow_lod = -1` = LOD 关），此时相邻级对同一表面的遮挡**一致** ⟹ 跨级分歧只可能
 * 来自纹素量化本身，而一个纹素的影边摆放误差折成 cov 台阶就是 `floor` 的推导值。所以
 * `refStep > floor` ⟺ 带子里混进了**不是纹素量化**的东西（出块旁路污染 / 另一个非 LOD 的
 * 跨级缺陷）⟹ J1 的"LOD 归因"不再成立 ⟹ 此时 J1 无论绿还是红都不可信，必须停下来。
 *
 * 边界归属：**等于算通过**（只有严格 `>` 才抛）。`floor` 是"允许的最大纹素错位"这个闭式
 * **上界**，落在界上仍属于"只可能来自纹素量化"的集合；与 J1 自己的 `增量 ≤ 上限` 同口径
 * （两条界都含端点）。selftest 钉住这一格，`>=` 的弱化合规变异会红。
 *
 * ⚠ 它抛不代表阴影变好或变坏，只代表**这台量具当前问不出这个问题**。
 * @param refStep 参照臂的 maxRowStep 读数（`SeamStep::step`）
 * @param refRow  该读数所在行（进错误串，便于直接回看那一行）
 * @param band    被 judged 的带子（进错误串点名 x 区间）
 * @param floor   固有下界（调用方传 `kSeamTexelFloor`；本层不认识仪器常数）
 */
void requireBandValid(double refStep, int refRow, const cv::Rect& band, double floor);

/**
 * 覆盖度场在 band 内 `cov > thresh` 的像素占比（**有影面积**）。
 * 闭式刻度：整场 0（无影 / 分母死）→ 恰 0；整场 1 → 恰 1；一半像素全影、一半全受光 → 恰 0.5。
 * 与 `flipFraction` 的区别：那条量的是**两臂之差**（差分信号），本条量的是**单臂的绝对量**
 * （阴影到底画没画）。差分统计量对「分母与分子同时归零」这一类失效是**结构性失明**的——
 * cov ≡ 0 时两臂之差也 ≡ 0，于是所有差分判据一起变绿。这就是本条必须独立存在的全部理由。
 * band 空 / band 越界 / 非 CV_64FC1 → **抛**（同其余守卫）。
 */
[[nodiscard]] double coverageFraction(const cv::Mat& cov, const cv::Rect& band,
    double thresh = kSeamBin);

/**
 * **闸门 A｜分母臂在场**（抛 = rc 3，不是 Verdict）：`none` 臂（cov 的公共分母）与每一条 csm
 * 臂**不得逐位相同**。
 *
 * 为什么是**布尔**判据（没有阈值可谈）：闭式只有两个态——分母臂与 csm 臂逐位相同 = 阴影项
 * 对画面**一个像素都没改过** = 分母死了；不同 = 阴影在画。中间没有"差一点点"的形态，
 * 取任何阈值都是把两态判据糊成一段区间（同 J2 `shadow-cascade-seam-arms-alive` 的口径）。
 *
 * 它防的是这台差分仪器的**结构性失明**：cov 的分母是 `none` 臂，分母一失效 ⟹ 每一臂的 cov
 * 恒 0 ⟹ Δ ≡ 0 ⟹ **J1 恒绿、J3 恒绿（全 0 满足"单调 + 饱和 + 反向对照 0"）、J2 仍绿**
 * （J2 比的是两 force 臂的原始图，压根不吃分母）、**带子闸门也放行**（参照臂台阶 = 0 ≤ 界）。
 * 2026-10-19 实测：把 `seamCovField(img[i], img[0])` 的分母换成分子自身 ⟹ 三条判决线全绿
 * rc=0。
 * 🔴 **两道闸门的覆盖关系是实测的，不是猜的（写下来防后来人把方向弄反）**：本闸门的触发集被
 * 闸门 B **包含**——「某臂与分母逐位相同」⟹ 该臂 cov ≡ 0 ⟹ 有影面积 = 0% ⟹ B 必响。实测：
 * 把 none 臂复制成 d5f0 的副本、同时废掉本闸门 ⟹ **B 照样抛 rc=3**。所以本闸门的价值**不在
 * 探测力**（那一维它被 B 罩住），在三件事：① **归因具体**——它说「分母没让画面少过一个像素」
 * 并点名三种成因，B 只会说「阴影没进画面」，会把人带去查场景有没有影而不是查分母接线；
 * ② **无阈值**——不依赖 B 那个 1% 界推得对不对，换机位把界改坏时本闸门仍然有效；
 * ③ 判的是**图**，与 cov 公式无关（将来分母改成逐臂、或 cov 换式，它照样守）。
 *
 * @param diffByArm 每条 csm 臂的 (臂名, 与分母臂逐位不同的像素数)，由调用方用
 *                  `anyChannelDiff` 算好传进来（本层不做像素差分，只判形状）。
 *                  **一次报全**所有为 0 的臂（逐条抛会把第二条死臂藏到下一轮）；
 *                  **空清单一律抛**——空清单会平凡通过 = 闸门看起来在、其实什么都没查。
 */
void requireDenominatorAlive(const std::vector<std::pair<std::string, std::int64_t>>& diffByArm);

/**
 * **闸门 B｜覆盖度非零**（抛 = rc 3，不是 Verdict）：每条 csm 臂的有影面积必须 ≥ `floor`。
 *
 * 界为什么是闭式可辩护的（不是"实测 48% 所以钉 1%"）：闭式的**死态**是 `cov ≡ 0` ⟹ 有影面积
 * **恰 0%**（不是"小于某个小数"）；本靶实测正常读数在 **48.6% 量级**（主 agent 定口径时记的
 * 「有影面积 48.55%」）。取 1% = 离死态留一个数量级以上的裕量、离正常读数留 ~48 倍，
 * ⟹ 它只在"阴影几乎没画"时响，不会在正常取景波动上乱叫。
 * ⚠ 换机位 / 换场景要重推这个数（有影面积随取景变，界跟着靶走）。
 *
 * 它补的是闸门 A 罩不住的那一半（覆盖关系是实测的，见 `requireDenominatorAlive` 声明处）：
 * **两臂图不同、但差异不在阴影上**——最典型的是分母被换成一张与阴影无关的图（X2 实测），此时
 * A 放行（图确实不同）而 cov ≡ 0 ⟹ 所有差分判据一起绿。B 判的是单臂绝对量，与差分无关。
 * @param fractionByArm 每条 csm 臂的 (臂名, coverageFraction 读数)
 * @param floor         下界（调用方传 `kSeamCoverageFloor`；本层不认识仪器常数）
 */
void requireCoveragePresent(const std::vector<std::pair<std::string, double>>& fractionByArm,
    double floor);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_SEAM_METRIC_H
