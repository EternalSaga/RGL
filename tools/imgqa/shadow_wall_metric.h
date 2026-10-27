#ifndef RGL_IMGQA_SHADOW_WALL_METRIC_H
#define RGL_IMGQA_SHADOW_WALL_METRIC_H
//
// shadow_wall_metric.h — **垂直接收面**上的级联接缝口径层（`shadow-swim-wall` 的算子层）
//
//   缝同 `shadow_transport_metric.h` / `shadow_edge_metric.h`（「口径 vs 仪器」那条既有缝）：
//   `shadow_swim_wall.cpp`（仪器：起引擎的采集装配 + 出表编排）、`shadow_swim_wall_target.cpp`
//   （靶与机位：**闭式门面掩码 / 逐像素视深场**，零引擎进程）、`shadow_swim_wall_judge.cpp`
//   （闸门 + 判决线）与本层（口径：**级联索引读出 / 接缝视深**）不是同一条改动缝——改臂与
//   阈值动仪器，改测量算子动本层，而**只有本层被 `just imgqa selftest` 用闭式输入直接测**
//   （不启引擎、毫秒级）。
//
//   ## 本层的核心概念：**级联索引是逐位读出来的，不是算出来的**
//   `csm.force_cascade = k` 把写进 scene data 的 splits 覆写成 `splits[i] = (i<k) ? 0 : 1e9`
//   （rgl/scene_context.cppm `forceShadowCascadeSplits`），于是**全场景**片元都被推到第 k 级；
//   而它**只覆写这一份 splits**，CPU 侧的级联球/切片/阴影图内容一个字都不动
//   （rgl/render_list_impl.cppm 该处红字注释）。⟹ 拿四张「钉级图」当**四把尺**：
//
//       自然臂某个片元与钉级 k 图**逐位相同** ⟺ 该片元在自然臂里就取的第 k 级
//
//   这是构造性质，不是阈值判据：同一张图的同一个纹素、同一套采样输入。地板**恰 0**。
//   于是「门面这块垂直接收面由哪一级服务」「接缝有没有横切它」「接缝在屏幕哪一行」全部
//   变成**逐位可读**的数，不吃任何闭式级联模型。
//
//   ## 为什么不是「自然臂 vs 钉级臂的 cov 差」（本层第一版的死法，写下来防回退）
//   第一版把「两臂 cov 是否相同」当二值判据去定位接缝，隐含假设「差异只住在接缝附近」。
//   实测**不成立**：钉级换的是**整块表面的采样源**，远离接缝的深黑区两级也给不同答案
//   （本靶实测：门面有影像素里 pin2 与 pin3 的分歧 ≈ 50 %）。于是 cov 差场是一片糊的
//   「哪里都差」，边界估计退化成「门面视深区间的中点」——一个与接缝无关的数。
//   🔴 教训：**「钉级图只准拿来出差分图，不准拿来当判据的分子」**（configs/shadow.lua 该键
//   的注释早就写了这句，第一版没读进去）。逐位**匹配**（自然臂 == 哪一张钉级图）与
//   逐位**差分**（自然臂 − 某张钉级图）是两件事，前者给索引、后者只给分歧量。
//
//   ## 观测量为什么是视深、以及单调性为什么是判据而不是假设
//   选级用的是视深 `depth = dot(worldPos − eye, forward)`（rgl/shaders/shadow.slang ①，
//   三段无条件比较）。于是「索引对深度单调」是引擎语义的**直接推论**：本层把它做成
//   `monotone` 读数——若读出不单调（低级的最大视深 > 高级的最小视深），要么选级不是按
//   视深、要么掩码进来了别的几何，两种都必须让判据红，而不是让接缝位置带着这个洞继续报数。
//
//   ## 闭式门面掩码（为什么可以不用 objectID）
//   靶的遮挡杆**整根在画外**（qa_scenes.py 场景 11 的机关 ②，构建期 assert ⑤ 逐角点核对），
//   于是画内的每条射线要么命中门面矩形、要么命中天穹。而门面是**平面**：射线与平面的交点
//   落在门面矩形内 ⟺ 该像素就是门面。⟹ 掩码 = 「射线命中矩形」，闭式、零阈值、不读图。
//   ⚠ 成立**完全依赖**「画内没有别的几何」，那条前提由靶的构建期 assert 守着。
//
//   ## 变异红线（哪个变异让本层红；🔴 每条都在 selftest_swim_wall.cpp 里有对应闭式用例）
//   · 把接缝视深**钉成恒 0**（`cascadeIndexReadout` 直接返回 0）→ 「合成一个已知位置的接缝
//     必须原样读出」红，且「跨桶推拉相机必须报出同号位移」红。任务书钦定的那条。
//   · 把「与哪一级逐位相同」的搜索从「唯一」放宽成「取第一个命中的级」→ 四张钉级图全同的
//     像素（无信息）会被算成级 0，「直方图只统计分歧像素」那条反向用例红。
//   · 把掩码当成「全图」→ 天穹像素进分母，「索引对深度单调」与「分歧像素数 > 0」两条红。
//   · 去掉单调性守卫 → 「把两个级的像素互换深度后仍报绿」红（守卫失效不出声）。
//   · 守卫改成返回空读数 → 「空读数必须抛」红（空 = 「没有接缝」= 假绿）。
//
#include "plane_reconstruct.h"  // PlaneFrame / PlaneSamples（重建口径共用，本层不重写）

#include <opencv2/core.hpp>

#include <array>
#include <cstddef>
#include <vector>

namespace RGL::imgqa {

/// 级联数（= 引擎 `kActiveShadowCascades`，rgl/scene_data.cppm）。本层拿它当**尺子根数**：
/// 钉级臂必须凑满 4 把，少一把就有一个索引读不出来（`pins.size()` 不符 ⟹ 抛）。
inline constexpr size_t kWallCascadeCount = 4;

/// 一个像素「与自然臂逐位相同」的钉级臂集合里，只有**恰好一个**成员才算读出索引。
enum class CascadeVote : uint8_t {
    /// 🔴 0 必须是「四把尺全同 = 该像素没有信息」，因为它同时是 `cv::Mat` 的**初始化值**：
    /// 第一版把 kNone 放在 0，于是所有被跳过的无信息像素在下游被当成「一把都不匹配」计数，
    /// 门面 33 万点里报出 27 万「不匹配」（实测 0）——一个静默的假红（本批实跑抓到）。
    kNoInfo = 0,   ///< 四把尺给同一个答案 ⟹ 该像素不参与索引读出（既不是票也不是失配）
    kNone,         ///< 尺子有分歧而自然臂一把都不匹配 ⟹ 结构性违规（见 J-W1）
    kUnique,       ///< 恰好一张 ⟹ 该像素的级联索引 = 那张的级号
    kAmbiguous,    ///< ≥2 张 ⟹ 这些级在该像素给出同一个答案，索引读不出来（不记 0 票）
};

/// 门面像素的级联索引读出输入：每像素一票 + （投票唯一时的）级号。
struct CascadeVoteMaps {
    cv::Mat vote;    ///< CV_8UC1，取值 = CascadeVote
    cv::Mat index;   ///< CV_8SC1，kUnique 时为级号，否则 −1
};

/**
 * 把「自然臂 vs 四张钉级臂」的逐位匹配聚成级联索引场。
 *
 * 只统计**分歧像素**（四张钉级图不全同的像素）：四把尺给同一个答案的地方没有任何信息，
 * 把它们计入直方图会把「门面全在级 0」这种假读数做出来（反向用例守这一条）。
 * 尺寸/类型不符、钉级臂数量不是 `kWallCascadeCount` ⟹ **抛**。
 */
[[nodiscard]] CascadeVoteMaps cascadeVotes(const cv::Mat& natural,
    const std::vector<cv::Mat>& pins);

/// 接缝视深 + 索引分布的读数。🔴 `straddles == false`（门面只有一级）不是失败，是**结论**：
/// 它意味着「这块面上根本没有接缝」，一切「带随相机动」的归因都不能记在接缝头上。
struct CascadeIndexReadout {
    std::array<size_t, kWallCascadeCount> hist {};  ///< 分歧像素按级号计数
    size_t divergent = 0;                           ///< **唯一**匹配某把尺的门面像素数（读出分母）
    size_t informed = 0;                            ///< 四把尺不全同的门面像素数（含读不出的）
    size_t ambiguous = 0;                           ///< 匹配 ≥2 张的门面像素数（读不出索引）
    size_t unmatched = 0;                           ///< 一张都不匹配的门面像素数
    int lower = -1;                                 ///< 两个主导级里视深较小的那个
    int upper = -1;                                 ///< …较大的那个
    double seamDepth = 0.0;                         ///< 接缝视深 = 低级的最大视深（下界估计）
    double sameMinAbove = 0.0;                      ///< 高级的最小视深（= 上界，二者之差是格宽）
    double monotoneSlack = 0.0;                     ///< min(高级最小视深) − max(低级最大视深)
    bool straddles = false;                         ///< 是否恰有两个主导级（= 接缝横切门面）
    bool monotone = false;                          ///< 索引对视深单调（引擎语义的直接推论）
    bool found = false;                             ///< false = 什么都没测到（不许读成「零不连续」）
};

/**
 * 从级联索引场 + 每像素**闭式视深**里读出接缝。
 *
 * @param votes   `cascadeVotes` 的产物
 * @param mask    闭式门面掩码（8UC1，255 = 该像素在门面上）
 * @param depthAt 每像素视深（CV_64FC1，闭式：depth = c0 + gU·u + gY·v）
 * 主导级 = 分歧像素里票数 ≥ 2 % 的两级（多于两级 ⟹ `straddles = false` 并给出直方图，
 * 让调用侧的闸门去抛，而不是本层自己挑一个）。接缝视深 = 低级像素的最大视深（按构造是
 * **下界**：选级按视深，低级恒在接缝近侧）。单调性 = 低级的最大视深 ≤ 高级的最小视深。
 * 掩码为空 / 分歧像素为 0 ⟹ **抛**（空 = 「什么都没看见」，与「门面只有一级」两件事
 * 不能同形；后者是 divergent > 0 而直方图只有一级，是**结论**不是失明）。
 */
[[nodiscard]] CascadeIndexReadout cascadeIndexReadout(const CascadeVoteMaps& votes,
    const cv::Mat& mask, const cv::Mat& depthAt);

/**
 * 按视深把两个级的场合成一张 —— **接缝位置的闭式合成真值**（selftest 的阳性对照用）。
 *
 * 用途：本层唯一的「我知道接缝在哪」的输入。拿 lower/upper 两半场按 split 合成出
 * 一张索引场，再喂 `cascadeIndexReadout` ⟹ 必须把边界读回 split（± 一格）。
 * 归属：`depthAt > split` 取 upper（与 shadow.slang 的 `if (depth > splits.x)` 同口径）。
 * 尺寸/类型不符 ⟹ 抛。
 */
[[nodiscard]] cv::Mat composeByDepth(const cv::Mat& idxLower, const cv::Mat& idxUpper,
    const cv::Mat& depthAt, double split);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_WALL_METRIC_H
