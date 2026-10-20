#ifndef RGL_IMGQA_SHADOW_SURVIVAL_METRIC_H
#define RGL_IMGQA_SHADOW_SURVIVAL_METRIC_H
//
// shadow_survival_metric.h — `shadow-caster-survival`（阴影遮挡体存活率）的**度量口径层**
//
//   缝同 `shadow_seam_metric.h`（「口径 vs 仪器」那条既有缝）：`shadow_caster_survival.cpp`
//   （仪器：臂表 + 双机位采集 + 引擎回显对账 + 四道闸门 + 扫描表编排）与本层（口径：
//   两幅覆盖度场 → 面积存活率 / 连通块分档存活率 / 消失-变淡归因）不是同一条改动缝——
//   改臂 / 机位 / 阈值动前者，改测量算子动后者，而**只有后者被 `just imgqa selftest`
//   用闭式输入直接测**（不启引擎、毫秒级，故 `just tools-check` 覆盖得到）。
//
//   ## 口径为什么长这样（缺陷本体与「为什么必须是绝对量」的论证全文在仪器层头注释）
//   缺陷类 = **LOD 把细遮挡体的影子整个弄丢**（栏杆/窗棂/挂灯/牛腿/花箱），而级与级之间
//   可以完全一致（`shadow-cascade-seam` 对这类缺陷结构失明：四级一起变差时它给满分）。
//   本层量的是「影子活下来多少」：参照 = `shadow_lod = -1`（四级全画原始几何 = 阴影的
//   「应有样子」），对每个候选 D 出四类**绝对量**读数：
//     ① 本影面积存活率 = area(cov_test > 0.5) / area(cov_ref > 0.5)（允许 > 1：LOD 轮廓
//        略胖是实测事实，主 agent 实测 48.55% → 48.77% = 100.5%）。
//     ② 小块存活率（**主读数**）：参照本影连通块按面积分四档（<50 / 50–200 / 200–1000 /
//        >1000 px），逐档算存活比例。🔴 必须分档：主 agent 实测丢的正是小块，而总面积几乎
//        不变（48.55%→48.77%）——只看总面积的判据会报「没问题」，那正是本仪器要防的失效。
//        块存活判据：块内参照 cov>0.5 的像素中 test cov 仍 > 0.2 的占比 ≥ 0.5 记为存活。
//     ③ 消失 vs 变淡 分开计数（**归因**读数，不是装饰）：`test < 0.2` 记「消失」、
//        `0.2 ≤ test < 0.5·ref` 记「变淡」。「整簇被丢掉」与「簇被简化塌成零厚度」在存活率
//        上同形、在这条上分家（前者改选级判据，后者要改打包工具）。
//     ④ 近场带（行 500–899）重算 ①②——接触阴影（牛腿/花箱那类）只发生在近场。
//   连通域用 **8 邻接**（`cv::connectedComponentsWithStats`）：细长条影子（20×60 / 17×61）
//   在 4 邻接下会被对角缝拆碎、把「一条栏杆影」数成几条短影，分档就假性偏小——8 邻接是
//   「一块连续的影」的白话定义，selftest 有一条对角链用例钉它。
//
//   cov 场本身**不另立定义**：吃 `shadow_seam_metric.h` 的 `seamCovField`（与 shadow-grain /
//   shadow-cascade-seam 同式同序：线性域、逐通道 clip → 再取 RGB 均值）。三台仪器共享同一条
//   sRGB 解码路径（`srgbDecode`，imgqa.h 硬约束第 2 条），读数才能互相解释。
//
//   ## 边界归属（每条都有 selftest 钉住，对称判据每半边各一条）
//   · 本影：cov **严格 >** 0.5（恰 0.5 = 半影缘以内，不算有影；与 kSeamBin/kGrainBin 同口径）。
//   · 存活像素：test cov **严格 >** 0.2（恰 0.2 不算活——它归「变淡」）。
//   · 消失：test cov **严格 <** 0.2（恰 0.2 不算消失）。
//   · 变淡：0.2 ≤ test < 0.5·ref（**严格 <**，恰等于 0.5·ref 算活得好）。
//   · 块存活：存活占比 **≥** 0.5（含端点，恰一半算活）。
//   · 分档：[0,50) / [50,200) / [200,1000) / [1000,∞)（下界含、上界不含）。
//
//   ## 变异红线（哪个变异让我红——全部由 `selftest_survival.cpp` 钉死）
//   · 本影阈值 `> 0.5` 写成 `>= 0.5` → 「恰 0.5 不计影」红；分母阈值写成与分子不同值 →
//     「同图 → 恰 1.0」红（两阈值必须同源，否则同图都不给 1）。
//   · 存活像素阈值 `> 0.2` 弱化成 `>= 0.2` → 「恰 0.2 不算活」红；删掉（恒真）→
//     「test 全 0 → 存活率恰 0」红。
//   · 消失判据 `< 0.2` 写成 `<= 0.2` → 「恰 0.2 记变淡不记消失」红。
//   · 变淡上界 `0.5·ref` 写成常数 0.5 → 「淡影闭式」红（ref=1.0 时两者同值，用例取
//     ref=0.8、test=0.45：0.45 < 0.5 但 ≥ 0.5·0.8=0.4 ⟹ 常数版记淡、真值记活）。
//   · 块存活占比 `2·alive >= area` 写成 `>` → 「恰一半算活」红。
//   · 分档边界 `< 50` 写成 `<= 50` → 「面积 50 落第二档」红；边界整体挪位 → 三条边界用例
//     （49/50、199/200、999/1000）至少一条红。
//   · 连通域 8 邻接改成 4 邻接 → 「对角链 = 一块」红（4 邻接给 3 块，分档立刻失真）。
//   · 「只丢小块」合成对上把分档删掉（只报总面积）→ 立命断言红：面积存活率恰 1.000 而
//     <50px 档存活率恰 0.000——**这条断言证明本判据不是 seam 判据的换皮**，删了它这台
//     仪器就退化成「又一条只看总面积的尺」。
//   · 守卫任一改成返回 0（band 空 / 越界 / 尺寸不同 / 非 CV_64FC1 / 参照本影为空）→
//     对应那条 `throwsStd` 红。**返回 0 会被读成「存活率为 0 = 全丢」或「= 没数」**，
//     两种读法都错，所以守卫一律抛（与 seam 的守卫纪律同一条）。
//   · 空桶存活率从 NaN 改成 1.0 → 「空桶 = 无数据不是满分」红（把「没数」读成「全活」=
//     对空集合的断言恒真的图像版）。
//   · **闸门 C**（`requireReferenceSelfConsistent`）的比较反向成永不抛 → 「非 1.000 应抛」红；
//     界从「恰 1.0」放宽成「≥ 0.99」→ 「0.999 应抛」红（参照臂自洽是**闭式恒等**，
//     任何容差都会把「参照臂其实带着 LOD」的半失效放过去）。
//   · **闸门 D**（`requireLodChangesImage`）的比较 `diff > 0` 恒真化 → 「有一条臂与参照逐位
//     相同应抛」红；反向（永不放行）→ 「正常态不抛」红（仪器永久红）。
//   · 空清单守卫删掉 → 对应「空清单应抛」红（平凡通过的闸门比没有闸门更坏，同 seam）。
//
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {

/// 本影二值化阈值（**严格 >**，恰 0.5 不计影——与 `kSeamBin` / `kGrainBin` 同口径同理由）。
inline constexpr double kSurvivalUmbraBin = 0.5;

/// 「test 臂仍算有影」的像素判据（**严格 >**）。0.2 不是拍的：主 agent 实测丢失区的覆盖度
/// 落在 0.62–0.78 而**变淡**区的 cov 明显更高，0.2 把「影还在但浅」与「影没了」分开；
/// 它同时是「消失」的上界（见下），两个名字同一个数——一个阈值只准有一个出处。
inline constexpr double kSurvivalAliveCov = 0.2;

/// 块存活判据：块内「test cov 仍 > kSurvivalAliveCov」的像素占比 **≥** 0.5 记为存活（含端点）。
/// 0.5 = 「这个块的大半还是影」与「已经没了」的中点；比它松会把残影当全活，比它紧会把
/// 边缘量化抖动的正常块判死。
inline constexpr double kSurvivalAliveFrac = 0.5;

/// 「变淡」上界系数：`kSurvivalAliveCov ≤ test < kSurvivalFadeFactor·ref` 记变淡（严格 <）。
/// 0.5·ref = 「亮度只回来一半」——与参照同比例，暗影与亮影用同一把尺。
inline constexpr double kSurvivalFadeFactor = 0.5;

/// 连通块面积分档上界（**不含**）：<50 / 50–200 / 200–1000 / ≥1000，共 4 档。
/// 50 的出处 = 主 agent 实测丢失块的最小尺寸 237px 是「细长条」量级、而肉眼实例全部落在
/// 小块档；1000 的出处 = 28×52 的细长条（1456px）已属「大块的窄条」，与真大块（本影主体）
/// 分开报。**这套分档是本仪器的量程选择，换场景要重定**（仪器层头注「还剩下什么看不见」）。
inline constexpr int kSurvivalBucketEdges[3] = { 50, 200, 1000 };
inline constexpr int kSurvivalBucketCount = 4;

/// 一档一次采集的读数（连通块**个数**，不是像素数——存活率按块算，像素数按消失/变淡算）。
struct UmbraBlobStats {
    std::array<int64_t, kSurvivalBucketCount> bucketTotal{};  ///< 各档参照本影连通块数
    std::array<int64_t, kSurvivalBucketCount> bucketAlive{};  ///< 各档存活块数
    int64_t refUmbraPx = 0;   ///< 带内参照本影像素总数（= 各块面积之和）
    int64_t vanishedPx = 0;   ///< 其中 test cov < 0.2 的（「消失」）
    int64_t fadedPx = 0;      ///< 其中 0.2 ≤ test cov < 0.5·ref 的（「变淡」）
};

/**
 * 本影面积存活率 = |{p ∈ band : covTest > 0.5}| / |{p ∈ band : covRef > 0.5}|。
 * **允许 > 1**（LOD 轮廓略胖是实测事实：主 agent 实测 48.55% → 48.77% = 1.005），
 * 判据读的是「随 D 的形状」（J2 单调性），不是「≤ 1」。
 * 闭式刻度：两图相同 → 恰 1；test 全 0 → 恰 0；一半本影像素翻没 → 恰 0.5。
 * band 空 / 越界 / 尺寸不同 / 非 CV_64FC1 / **参照本影为空**（空分母）→ **抛**。
 */
[[nodiscard]] double umbraAreaSurvival(const cv::Mat& covTest, const cv::Mat& covRef,
    const cv::Rect& band);

/**
 * 参照臂本影（covRef > 0.5，8 邻接）在 band 内的连通块分档存活统计 + 消失/变淡逐像素计数。
 * 块存活 = 块内 covTest > 0.2 的像素占比 ≥ 0.5（整数比较 `2·alive ≥ area`，不引入浮点边界）。
 * 守卫与 `umbraAreaSurvival` 同一条（参照本影为空 = 分母空 = 抛，返回全 0 会被读成
 * 「一块都没活」或「没数」，两种都错）。
 * ⚠ 块只在 band 内生长：跨带界的影块在带内的那截算一块（近场带与全幅两张表因此是
 * 「各自带内的形状」，不是同一批块的两个子集——分档读数跨带不可加）。
 */
[[nodiscard]] UmbraBlobStats umbraBlobStats(const cv::Mat& covTest, const cv::Mat& covRef,
    const cv::Rect& band);

/// 某档存活率 = bucketAlive / bucketTotal。**空档返回 NaN**（无数据 ≠ 满分 1.0：
/// 把「没数」读成「全活」= 对空集合的断言恒真的图像版）。bucket 越界 → 抛。
[[nodiscard]] double bucketSurvival(const UmbraBlobStats& s, int bucket);

/**
 * **闸门 C｜参照臂自洽 = 恰 1.000**（抛 = rc 3，本判据的阳性锚点，不是 Verdict）：
 * `D=-1` 与 `D=-1` 自己比，四项读数必须全是闭式恒等——面积存活率恰 1、每个非空档存活率
 * 恰 1、消失 = 变淡 = 0（全幅与近场带各一套）。它不是 1 ⟹ 参照臂接线错了（**第一嫌疑 =
 * 参照臂其实没走全量几何**：shadow_lod 覆写打偏 / 参照臂与某 test 臂取了同一条），
 * 或算子本身坏了 ⟹ 整张表作废，一条 Verdict 都不产。
 * 🔴 为什么**无容差**：这是「同一份数据自己比自己」的恒等式，不是测量；任何容差都会把
 * 「参照臂带着 LOD」的半失效放过去——参照一歪，全表的「存活率」分母就不是「应有样子」了。
 * 与 seam 的带子闸门同族（量具自检，不是画质判据：它抛不代表阴影变好或变坏）。
 */
void requireReferenceSelfConsistent(double areaFull, double areaNear,
    const UmbraBlobStats& fullStats, const UmbraBlobStats& nearStats);

/**
 * **闸门 D｜参照臂必须真的与 test 臂不同**（抛 = rc 3，不是 Verdict）：清单里的每条见证臂
 * 的 cov 场与参照臂 cov 场**不得逐位相同**。
 * 它防的是「全臂相同」让四项读数全部 = 1.000 = 满分绿：shadow_lod 覆写整体失效（键拼错 /
 * 门被后面的覆写盖掉 / 参照臂被改成与出厂同档）时，簇路径闸门与回显对账都可能照旧绿
 * （回显证明「键被解析器读到」，证明不了「D 改到了阴影图」）。闭式两态 = 相同（LOD 没
 * 生效）/ 不同，**布尔判据无阈值可谈**（同 seam 的闸门 A 口径）。
 * 🔴 **清单 = 调用方给定的「见证臂」，本层不认识 D**：哪些臂「正常态必与参照不同」是仪器侧
 * 的实测事实（本仪器取 D=11 与出厂 D=5 两枚见证，理由与失效覆盖表住仪器层 requireReference-
 * Gates 的注释），小 D（0/1）实测可与参照逐位相同（LOD 级 1 在阴影图分辨率下与全量不可区分）
 * ——把合法态传进来会把正常构建判死。
 * @param diffByArm 每条见证臂的 (臂名, 与参照臂 cov 场逐位不同的像素数)，调用方算好传入。
 *                  空清单一律抛（平凡通过的闸门比没有闸门更坏，同 seam 的 A/B）。
 */
void requireLodChangesImage(const std::vector<std::pair<std::string, std::int64_t>>& diffByArm);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_SURVIVAL_METRIC_H
