// 判据 + 反模式登记 + 调用台账。
#include "imgqa.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace RGL::imgqa {

std::string Verdict::line() const
{
    return (ok ? "PASS [" : "FAIL [") + name + "] " + observed;
}

const std::vector<JudgmentInfo>& registeredJudgments()
{
    static const std::vector<JudgmentInfo> kAll = {
        {"visibility",
            "无容差：集合断言，不是数值断言。manifest 声明的 piece 集与可见集之差要么为空"
            "要么点名，没有「差不多」。",
            {"qa_scenes.py：按预测边界定点 = 会绿 = 无效断言"}, false},
        {"piece-drawn",
            "面积下限按「该 piece 在该机位下投影应占的像素数」逐个给，不给全局比例。"
            "0 像素 = 没画；个位像素 = 被裁到只剩边。",
            {"近处盒子 +Z 面纯黑（先分叉「画了没着色」与「根本没画」）"}, false},
        {"face-clusters-explained",
            "±2 LSB / 255 ≈ 0.0078（沿用探针实测：float64 转录 vs 着色器 float32 + 8bit "
            "取整）。簇匹配间隙 0.02，因为各面 NdotL 差远大于此。",
            {"近处盒子 +Z 面纯黑", "闭式解探针实测零误差"}, false},
        {"switch-impact-domain",
            "域外**必须恰为 0**，不是「小于阈值」。正确的旁路是位级恒等（已有此性质："
            "lerp(1,x,0) ≡ 1.0、shadowPass=false 画面逐位不变）。既然是恒等，容差就该是"
            "0；给了容差等于放弃这条判据最锋利的部分。",
            {"5a-7：半径恒 0 的恒等阵伪影同时骗过稳定性对拍与开关对拍"}, false},
        {"dead-branch-scan",
            "无容差：恒等即恒等。「两个本该不同的量恒等 = 其中一个分支从未生效」。",
            {"调试纪律：恒等式日志是指纹"}, false},
        {"determinism",
            "无容差：同配置重跑必须逐位相同。任何差异（未初始化内存、同步时序、顺序抖动）"
            "都会让一切「域外恰为 0」判据失去意义——它是域判据的前置，不是另一条域判据。",
            {"AGENTS.md 测量纪律：单实例串行（并行双实例曾污染帧率测量）"}, false},
        {"probe-pixel",
            "±2 LSB：ACES 多项式 float64 转录 vs 着色器 float32 + 8bit 量化取整"
            "（沿用 12.2d-c 实机对拍实测）；再大就该当失败看待，不能靠放宽阈值混过去。",
            {"闭式解探针实测零误差（批 12.2d-c，analytic_ref.py 时代）"}, false},
    };
    return kAll;
}

namespace {
const JudgmentInfo& info(const char* name)
{
    for (const auto& j : registeredJudgments())
        if (std::string(j.name) == name)
            return j;
    throw std::runtime_error(std::string("判据未注册: ") + name);
}
}  // namespace

Verdict verdictFor(const char* name, bool ok, std::string observed)
{
    const auto& j = info(name);
    return Verdict{name, ok, std::move(observed), j.whyTol, j.guards, j.promoted};
}

namespace {
/// 任一通道不同的 8UC1 掩码。**不要**用 cv::reduce(axis=0) 合并通道——reduce 对多
/// 通道输入按通道独立归约、输出仍是多通道，countNonZero 直接断言 cn==1 炸（switch
/// 判据首战抓到，此前零实战）。
cv::Mat anyChannelDiff(const cv::Mat& a, const cv::Mat& b)
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
Verdict mk(const char* name, bool ok, std::string observed)
{
    return verdictFor(name, ok, std::move(observed));
}
}  // namespace

Verdict judgeVisibility(const Frame&, const Attribution& at,
    const std::vector<std::string>& expectVisible)
{
    // 这是**所有其它判据的前提**：某 piece 不在可见集里，关于它的任何断言都恒真。
    auto seen = at.visiblePieces();
    std::vector<std::string> missing;
    for (const auto& want : expectVisible)
        if (std::find(seen.begin(), seen.end(), want) == seen.end())
            missing.push_back(want);
    std::string obs = "可见 " + std::to_string(seen.size()) + " 个 piece；缺席 [";
    for (size_t i = 0; i < missing.size(); ++i)
        obs += (i ? " " : "") + missing[i];
    obs += "]";
    if (!at.unnamed.empty()) {
        obs += "；认不出名字的 id:";
        for (int32_t id : at.unnamed)
            obs += " " + std::to_string(id);
    }
    return mk("visibility", missing.empty(), std::move(obs));
}

Verdict judgePieceDrawn(const Frame& f, const Attribution& at, const std::string& piece,
    int64_t minPixels)
{
    // 为什么先问这个：未解现象「某面纯黑」曾被沿「画了但没着色」单一假设排查了
    // glb 数据/法线/绕序/阴影/AO——而一次 ID 解码就能先分叉：像素 id 属该 piece
    // ⟹ 画了（着色问题）；是背景 ⟹ 根本没画（剔除/变换/绕序）。
    const cv::Mat m = maskOfPiece(f, at, piece);
    const int64_t n = cv::countNonZero(m);
    return mk("piece-drawn", n >= minPixels,
        piece + ": " + std::to_string(n) + " 像素（阈值 " + std::to_string(minPixels) + "）");
}

Verdict judgeFaceClusters(const Frame& f, const Attribution& at, const std::string& piece,
    const std::vector<FacePrediction>& predict, double tol)
{
    // 「闭式解锚点」×「区域归因」的合流。绕开像素↔面几何配准的办法是**反着判**：
    // 不预测每像素，而要求观测的**亮度簇集合**被预测面值集合覆盖。出现解释不了的
    // 簇（比如亮度 0）就是发现。**把猜哪个面留在人这边**，机器只保证"这确实不是
    // 物理允许的取值"。
    const cv::Mat m = maskOfPiece(f, at, piece);
    if (cv::countNonZero(m) == 0)
        return mk("face-clusters-explained", false, piece + ": 无像素，无法判");
    // 只收集 mask 内像素——copyTo(dst, mask) 对 dst 是"掩码外保留原值"，新分配的 dst
    // 掩码外是未初始化内存，直接聚类会把垃圾当暗簇（假发现）。
    std::vector<double> insideVals;
    insideVals.reserve(static_cast<size_t>(cv::countNonZero(m)));
    for (int y = 0; y < f.luma.rows; ++y) {
        const auto* mrow = m.ptr<uchar>(y);
        const double* lrow = f.luma.ptr<double>(y);
        for (int x = 0; x < f.luma.cols; ++x)
            if (mrow[x])
                insideVals.push_back(lrow[x]);
    }
    const cv::Mat masked(static_cast<int>(insideVals.size()), 1, CV_64FC1, insideVals.data());
    const auto clusters = lumaClusters(masked);
    std::string obs = piece + ": 观测簇 ";
    for (size_t i = 0; i < clusters.size(); ++i) {
        std::ostringstream os;
        os << std::fixed << std::setprecision(4) << clusters[i].first << "("
           << clusters[i].second << "px)";
        obs += (i ? ", " : "") + os.str();
    }
    obs += " | 预测 ";
    for (size_t i = 0; i < predict.size(); ++i)
        obs += (i ? ", " : "") + predict[i].face + "="
            + [&] { std::ostringstream os; os << std::fixed << std::setprecision(4) << predict[i].luma; return os.str(); }();
    std::string unexplained;
    for (const auto& [med, n] : clusters) {
        bool explained = false;
        for (const auto& p : predict)
            if (std::abs(med - p.luma) <= std::max(tol * std::max(p.luma, 1e-6), 2.0 / 255.0))
                explained = true;
        if (!explained) {
            std::ostringstream os;
            os << std::fixed << std::setprecision(4) << med << "(" << n << "px)";
            unexplained += (unexplained.empty() ? "" : ", ") + os.str();
        }
    }
    if (!unexplained.empty())
        obs += " | **解释不了的簇**: " + unexplained;
    return mk("face-clusters-explained", unexplained.empty(), std::move(obs));
}

Verdict judgeSwitchDomain(const Frame& base, const Frame& alt, const cv::Mat& allowed)
{
    // 「开关对拍」强化版。弱版只问 on/off 是不是只变暗，5a-7 的恒 0 伪影同时骗过它和
    // 稳定性对拍——弱版没约束变化的**位置**。本判据约束位置：域外任何一位不同 = 旁路泄漏。
    cv::Mat anyDiff = anyChannelDiff(base.bgr, alt.bgr);
    cv::Mat outside, inside;
    cv::bitwise_and(anyDiff, ~allowed, outside);
    cv::bitwise_and(anyDiff, allowed, inside);
    const int total = cv::countNonZero(anyDiff);
    const int out = cv::countNonZero(outside);
    const int in = cv::countNonZero(inside);
    return mk("switch-impact-domain", out == 0,
        "变化像素 " + std::to_string(total) + "（允许域内 " + std::to_string(in)
            + "，**域外 " + std::to_string(out) + "**”）");
}

Verdict judgeDeadBranch(const Frame& base, const Frame& alt, const cv::Mat& mask,
    const std::string& label)
{    // 对偶于 switch-impact-domain：那条管「不该变的地方没变」，这条管「该变的地方
    // 没变」。两条合起来才堵住「0 差异」的两种解释（没影响 vs 没接上）。
    // 对偶于 switch-impact-domain：那条管「不该变的地方没变」，这条管「该变的地方
    // 没变」。两条合起来才堵住「0 差异」的两种解释（没影响 vs 没接上）。
    cv::Mat inMask;
    cv::bitwise_and(anyChannelDiff(base.bgr, alt.bgr), mask, inMask);
    const bool same = cv::countNonZero(inMask) == 0;
    return mk("dead-branch-scan", !same,
        label + ": 区域 " + std::to_string(cv::countNonZero(mask)) + + "px 两次采集"
            + (same ? "完全相同 ← 该改动从未生效" : "不同"));
}

Verdict judgeDeterminism(const Frame& a, const Frame& b)
{
    // 前置判据：同配置两次采集必须逐位相同（颜色面 + id 面都查——id 抖了区域统计
    // 一样完蛋）。尺寸不等直接算失败，不玩「差不多」。
    if (a.bgr.size() != b.bgr.size() || a.ids.size() != b.ids.size())
        return mk("determinism", false, "两次采集尺寸不同");
    const int colorDiff = cv::countNonZero(anyChannelDiff(a.bgr, b.bgr));
    // 输出 Mat 不复用：3 通道的旧壳接 1 通道 compare 会留 3 通道 → countNonZero 炸
    // （OpenCV 4.12 实测，本轮真实踩过）。
    cv::Mat idDiff;
    cv::compare(a.ids, b.ids, idDiff, cv::CMP_NE);
    const int idDiffN = cv::countNonZero(idDiff);
    return mk("determinism", colorDiff == 0 && idDiffN == 0,
        "同配置重跑：颜色差 " + std::to_string(colorDiff) + "px，id 差 "
        + std::to_string(idDiffN) + "px");
}

const std::map<std::string, std::string>& antiPatterns()
{
    // 装一条但**拒绝执行**，比不装更好：否则下一个会话的我（或别人）会重新发明它。
    static const std::map<std::string, std::string> kAnti = {
        {"ssim",
            "结构相似度不作主门（Filament 调研推翻原计划：其全库无 SSIM/PSNR/FLIP）。"
            "理由是判据性质：SSIM 把「整幅」当一个样本，失败时既说不出**哪块**错、也说不出"
            "错在亮度还是几何；且它有告警饱和——错得越多分母越稳。替代 = 闭式解锚点 + "
            "归属区域统计（本工具）+ 开关影响域。"},
        {"psnr",
            "PSNR 需要一张参考图，而本项目**跨日 ±1 LSB 噪声**使 golden 存不过一天"
            "（docs/调试历史.md 2026-09-08）。它把「多少像素差了多少」压成一个数，方向信息全丢。"},
        {"global-epsilon",
            "全局容差同时**漏掉大的、冤枉小的**。纪律：容差必须来自**为什么会差**（float32 "
            "多项式？8bit 取整？跨驱动 dither？）。写不出理由的判据就是还没想清楚，不该存在。"},
        {"assert-invisible-geometry",
            "对「该取景下其实看不见的几何」下断言 = 恒真 = 会绿 = 无效断言。先 attribute "
            "反解可见集，判据只在可见集上求值。"},
        {"cross-day-golden",
            "存一张基线隔日比 = 把驱动/编译器 codegen 微差当成「渲染变了」。本项目历史上"
            "因此连续作废 7 次基线。要么同会话双构建，要么根本不要 golden。"},
        {"eyeball-only",
            "纯目视不可复现、不可回归。目视用来**发现**问题，发现后必须落成判据。"},
    };
    return kAnti;
}

// ── 调用记录（晋升条件①②的计数来源；**没有记录，「打磨」就只是随手加 flag**）──

static fs::path callLogPath()
{
    fs::path p = fs::path(projectRoot()) / "qa/log";
    fs::create_directories(p);
    return p / "imgqa_calls.jsonl";
}

void logCall(const std::string& cmd,
    const std::vector<std::pair<std::string, std::string>>& args,
    const std::vector<Verdict>& verdicts)
{
    json rec;
    {
        const auto t = std::time(nullptr);
        char buf[32];
        std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", std::localtime(&t));
        rec["ts"] = buf;
    }
    rec["cmd"] = cmd;
    for (const auto& [k, v] : args)
        rec["args"][k] = v;
    rec["verdicts"] = json::array();
    for (const auto& v : verdicts)
        rec["verdicts"].push_back({{"name", v.name}, {"ok", v.ok}, {"observed", v.observed}});
    rec["outcome"] = "";  // 事后由 note 回填：这次判据说的对吗？真相是什么？
    std::ofstream out(callLogPath(), std::ios::app);
    out << rec.dump() << "\n";
}

int noteLastCall(const std::string& text)
{
    const fs::path p = callLogPath();
    if (!fs::exists(p)) {
        std::printf("还没有任何调用记录\n");
        return 2;
    }
    std::vector<std::string> lines;
    {
        std::ifstream in(p);
        std::string line;
        while (std::getline(in, line))
            if (!line.empty())
                lines.push_back(line);
    }
    if (lines.empty())
        return 2;
    json rec = json::parse(lines.back());
    rec["outcome"] = text;
    lines.back() = rec.dump();
    std::ofstream out(p, std::ios::trunc);
    for (const auto& l : lines)
        out << l << "\n";
    std::printf("已回填到最后一条调用记录: %s\n", rec["cmd"].get<std::string>().c_str());
    return 0;
}

}  // namespace RGL::imgqa
