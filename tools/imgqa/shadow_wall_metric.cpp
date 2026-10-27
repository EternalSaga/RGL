// shadow_wall_metric.cpp —— 垂直接收面上的级联索引读出算子（为什么在这一层 → 同名 .h）。
//
// 本文件**只有算子**：不启引擎、不读 PNG、不认识任何仪器与阈值。
// 🔴 铁律（从 `shadow_transport_metric` 继承）：**没有任何循环的迭代次数取决于数据**——
// 下面所有循环的边界都是「画幅常量」或「级联数（编译期 4）」，没有 while、没有收敛判定。
#include "shadow_wall_metric.h"

#include <cmath>
#include <string>
#include <limits>
#include <stdexcept>

namespace RGL::imgqa {
namespace {

/// 尺寸/类型守卫：错了**抛**而不是返回空读数（空读数 = 「没有接缝」= 假绿）。
void requireField(const cv::Mat& m, int type, const std::string& what, const cv::Size& ref)
{
    if (m.empty() || m.type() != type)
        throw std::runtime_error("shadow_wall_metric: " + what
            + " 必须是非空 " + (type == CV_64FC1 ? "CV_64FC1" : type == CV_8UC1 ? "CV_8UC1"
                                                                                 : "CV_8SC1")
            + "（类型错了返回空读数 = 「没有接缝」= 假绿）");
    if (m.size() != ref)
        throw std::runtime_error("shadow_wall_metric: " + what + " 与基准画幅尺寸不同"
            + " ⟹ 不是同一次采集的同一画幅，对拍无意义");
}

/// 主导级的门槛：分歧像素的 2 %。为什么不是「任意一张」：单像素噪声/边缘一个像素就能把
/// 一个根本不存在的级抬进直方图，边界会被定到错的地方；为什么不是 30 %：接缝斜切门面时
/// 两级比例可以很悬殊（本靶实测 45/55，但机位再远一档会走到 90/10）。
constexpr double kDominantShare = 0.02;

// ── `cascadeVotes` 的两段：取票 → 定票态 ─────────────────────────────────────────────
// 缝 = 「一枚像素上四把尺**怎么比**」（`matchPins`，只有相等性，没有任何口径）与
// 「比出来的量怎么映射成四态」（`voteOf`，本层全部的口径都在这一个函数里）。
// 合成一个循环体时的样子就是它超标时的样子：比较的嵌套与四态的分支互相抬对方的复杂度。

/// 一枚像素上「四把尺」的读数：自然臂与其中几把逐位相同、最后一把是哪级、四把尺彼此
/// 是否给出过不同答案。
/// 🔴 `divergent` 的定义是「与 `pins[0]` 不同的或」，不是「与邻居不同」：四把尺全同 ⟺ 这个或
/// 为假，而「全同」正是「该像素没有任何信息」的定义（`voteOf` 据此把像素整个剔掉）。
/// 循环边界 = 尺数（`cascadeVotes` 已核对为编译期常量 4），不随数据变。
struct PinMatch {
    int hits = 0;              ///< 与自然臂逐位相同的尺数
    int last = -1;             ///< 最后一把与之相同的级号（`hits == 1` 时它就是唯一那把）
    bool divergent = false;    ///< 四把尺彼此是否给出过不同答案（false = 无信息）
};

PinMatch matchPins(const cv::Mat& natural, const std::vector<cv::Mat>& pins, int y, int x)
{
    const auto px = natural.at<cv::Vec3b>(y, x);
    const auto p0 = pins[0].at<cv::Vec3b>(y, x);
    PinMatch m;
    for (size_t k = 0; k < pins.size(); ++k) {
        const auto q = pins[k].at<cv::Vec3b>(y, x);
        m.divergent = m.divergent || (q != p0);
        if (q == px) {
            ++m.hits;
            m.last = static_cast<int>(k);
        }
    }
    return m;
}

/// 由「尺子有无分歧 + 命中数」定票态。四态的分工全文见 .h 的 `CascadeVote`。
/// 🔴 顺序是口径的一部分：**先**判无分歧（无信息），再数命中——反过来会把四把尺全同的像素
/// 读成「命中 4 把 = 多义」，那是把「没有信息」伪装成「信息太多」，两种失效下游待遇完全不同。
CascadeVote voteOf(bool divergent, int hits)
{
    if (!divergent)
        return CascadeVote::kNoInfo;
    if (hits == 1)
        return CascadeVote::kUnique;
    return hits == 0 ? CascadeVote::kNone : CascadeVote::kAmbiguous;
}

// ── `cascadeIndexReadout` 的四段：归类 → 累计 → 遍历 → 选主导级 ───────────────────────
// 缝按「一个像素在该算子身上会碰到几件不同的事」切：一票四态怎么进分母（`tallyWallVote`）、
// 带有效级号的像素怎么在视深轴上记账（`accumulateWallDepth`）、遍历本身（`tallyWallVotes`）、
// 以及读完整张场之后「哪两级算主导」（`dominantCascadePair`）。前两段是**口径**（四态的待遇、
// 越界级号必须抛、2 % 门槛），后两段是**流程**——它们各自的改动原因不同，合成一段就分不出来。

/// 一票四态 → 分母与失配计数。返回 true = 该像素带**有效级号**（kUnique），调用侧才继续
/// 累计视深区间。
/// 🔴 `kNoInfo` 连 `informed` 都不进：四把尺全同 = 没有信息，它既不在分母里也不是失配。
/// 把它计入 informed 会让「门面无信息」读成「读不出接缝」而不是「什么都没看见」，而后者
/// 是必须抛的（`cascadeIndexReadout` 末尾那条守卫就永远不响）。
bool tallyWallVote(uint8_t v, CascadeIndexReadout& r)
{
    if (v == static_cast<uint8_t>(CascadeVote::kNoInfo))
        return false;
    ++r.informed;
    if (v == static_cast<uint8_t>(CascadeVote::kNone)) {
        ++r.unmatched;
        return false;
    }
    if (v == static_cast<uint8_t>(CascadeVote::kAmbiguous)) {
        ++r.ambiguous;
        return false;
    }
    return true;
}

/// 「唯一票」像素在视深轴上留下的三笔账：该级计一票 + 把视深并进该级的区间 [min, max]。
/// 越界级号 ⟹ **抛**：投票场说唯一而索引场给出四把尺之外的级号 = 上游两张场不同源，
/// 静默跳过只会让直方图少一格而分母照旧，读出的接缝位置于是建立在一个缺级的直方图上。
void accumulateWallDepth(int idx, double d, CascadeIndexReadout& r,
    std::array<double, kWallCascadeCount>& hi, std::array<double, kWallCascadeCount>& lo)
{
    if (idx < 0 || static_cast<size_t>(idx) >= kWallCascadeCount)
        throw std::runtime_error(
            "shadow_wall_metric: 索引场里出现级号 " + std::to_string(idx)
            + "（越界 = 投票与索引两张场不同源）");
    const size_t k = static_cast<size_t>(idx);
    ++r.hist[k];
    hi[k] = std::max(hi[k], d);
    lo[k] = std::min(lo[k], d);
}

/// 遍历门面掩码，把每个像素交给上面两段（循环边界 = 画幅常量，不随数据变）。
/// 掩码外 = 整个不看；视深算不出（NaN / 掠射投不出）= 不进分母也不记 0 票。
void tallyWallVotes(const CascadeVoteMaps& votes, const cv::Mat& mask, const cv::Mat& depthAt,
    CascadeIndexReadout& r, std::array<double, kWallCascadeCount>& hi,
    std::array<double, kWallCascadeCount>& lo)
{
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x) {
            if (!mask.at<uint8_t>(y, x))
                continue;
            if (!tallyWallVote(votes.vote.at<uint8_t>(y, x), r))
                continue;
            const double d = depthAt.at<double>(y, x);
            if (!std::isfinite(d))
                continue;  // 视深算不出（掠射/投不出）= 不参与定位，也不记 0 票
            ++r.divergent;
            accumulateWallDepth(votes.index.at<int8_t>(y, x), d, r, hi, lo);
        }
}

/// 主导级（票数 ≥ 分歧像素的 `kDominantShare`）。**不**在这里裁决「是不是两级」：
/// 一级 = 门面被单一级联覆盖（正是本仪器要回答用户的那个问题，是**结论**）；
/// ≥3 级 = 机位/靶与预期不符（交给调用侧与闸门，本层不自己挑两级）。
std::vector<int> dominantCascadePair(const CascadeIndexReadout& r)
{
    std::vector<int> dom;
    for (size_t k = 0; k < kWallCascadeCount; ++k)
        if (r.hist[k] >= kDominantShare * static_cast<double>(r.divergent))
            dom.push_back(static_cast<int>(k));
    return dom;
}

}  // namespace

CascadeVoteMaps cascadeVotes(const cv::Mat& natural, const std::vector<cv::Mat>& pins)
{
    if (pins.size() != kWallCascadeCount)
        throw std::runtime_error("shadow_wall_metric: 钉级臂数量是 " + std::to_string(pins.size())
            + "，需要恰好 " + std::to_string(kWallCascadeCount)
            + " 把（少一把就有一个级联索引读不出来，多一把说明臂表与引擎级联数漂了）");
    requireField(natural, CV_8UC3, "natural", natural.size());
    for (size_t k = 0; k < pins.size(); ++k)
        requireField(pins[k], CV_8UC3, "pins[" + std::to_string(k) + "]", natural.size());

    CascadeVoteMaps out;
    out.vote = cv::Mat(natural.rows, natural.cols, CV_8UC1, cv::Scalar(0));
    out.index = cv::Mat(natural.rows, natural.cols, CV_8SC1, cv::Scalar(-1));
    for (int y = 0; y < natural.rows; ++y)
        for (int x = 0; x < natural.cols; ++x) {
            const PinMatch m = matchPins(natural, pins, y, x);
            const CascadeVote v = voteOf(m.divergent, m.hits);
            // 🔴 四把尺全同的像素**不参与**：那里没有任何信息，计票会把「无信息」读成「级 0」。
            if (v == CascadeVote::kNoInfo)
                continue;
            out.vote.at<uint8_t>(y, x) = static_cast<uint8_t>(v);
            out.index.at<int8_t>(y, x)
                = v == CascadeVote::kUnique ? static_cast<int8_t>(m.last) : int8_t { -1 };
        }
    return out;
}

CascadeIndexReadout cascadeIndexReadout(const CascadeVoteMaps& votes, const cv::Mat& mask,
    const cv::Mat& depthAt)
{
    requireField(votes.vote, CV_8UC1, "votes.vote", mask.size());
    requireField(votes.index, CV_8SC1, "votes.index", mask.size());
    requireField(depthAt, CV_64FC1, "depthAt", mask.size());
    if (mask.empty() || mask.type() != CV_8UC1)
        throw std::runtime_error("shadow_wall_metric: 门面掩码为空/类型错 —— 闭式掩码没算出来，"
            "分母是彩票，任何比例都没有意义（抛，不当「没有接缝」放行）");

    CascadeIndexReadout r;
    // 每级一个 {最大视深, 最小视深}，循环边界 = 编译期常量 4，不随数据变。
    std::array<double, kWallCascadeCount> hi {};
    std::array<double, kWallCascadeCount> lo {};
    for (size_t k = 0; k < kWallCascadeCount; ++k) {
        hi[k] = -std::numeric_limits<double>::infinity();
        lo[k] = std::numeric_limits<double>::infinity();
    }
    tallyWallVotes(votes, mask, depthAt, r, hi, lo);
    if (r.informed == 0)
        throw std::runtime_error("shadow_wall_metric: 门面内分歧像素 = 0（掩码内 "
            + std::to_string(static_cast<long long>(mask.total()))
            + " 点）——四把钉级尺在门面上给出完全相同的答案 ⟹ 要么靶的遮挡体没投影到门面、"
              "要么钉级没生效。**拒绝**把「什么都没看见」读成「接缝不在门面上」这个结论");

    // 主导级：票数 ≥ 分歧像素的 2 %。
    const std::vector<int> dom = dominantCascadePair(r);
    r.found = true;
    if (dom.size() != 2) {
        // 一级 = 门面被单一级联覆盖（**结论**，正是本仪器要回答用户的那个问题）；
        // ≥3 级 = 机位/靶与预期不符，交给闸门去抛，本层不自己挑两级。
        r.straddles = false;
        r.monotone = true;  // 单级/多级时「单调」这条没有可判的边界，不假红也不假绿：见仪器
        return r;
    }
    r.lower = dom[0];
    r.upper = dom[1];
    r.straddles = true;
    r.seamDepth = hi[static_cast<size_t>(r.lower)];
    r.sameMinAbove = lo[static_cast<size_t>(r.upper)];
    r.monotoneSlack = r.sameMinAbove - r.seamDepth;
    r.monotone = r.monotoneSlack >= 0.0;
    return r;
}

cv::Mat composeByDepth(const cv::Mat& idxLower, const cv::Mat& idxUpper, const cv::Mat& depthAt,
    double split)
{
    requireField(idxLower, CV_8SC1, "idxLower", idxLower.size());
    requireField(idxUpper, CV_8SC1, "idxUpper", idxLower.size());
    requireField(depthAt, CV_64FC1, "depthAt", idxLower.size());
    cv::Mat out(idxLower.rows, idxLower.cols, CV_8SC1, cv::Scalar(-1));
    for (int y = 0; y < out.rows; ++y)
        for (int x = 0; x < out.cols; ++x) {
            const double d = depthAt.at<double>(y, x);
            if (!std::isfinite(d))
                continue;
            // 归属：严格 > split 取 upper（与 shadow.slang 的 `if (depth > splits.x)` 同口径；
            // 写成 >= 会把恰在 split 上的那一档归错边，selftest 的「边界落在真值 ± 一格」红）。
            out.at<int8_t>(y, x) = d > split ? idxUpper.at<int8_t>(y, x) : idxLower.at<int8_t>(y, x);
        }
    return out;
}

}  // namespace RGL::imgqa
