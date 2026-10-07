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

// registeredJudgments()（43 条判据的容差理由 + guard 出处）已按族拆到 judgments_table.cpp
// 与 judgments_table_motion.cpp；族 = 成立条件分组（声明与理由 → judgments_table.h）。

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
    return Verdict{name, ok, std::move(observed), j.whyTol, j.guards};
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

const char* antiScopeName(AntiScope scope)
{
    switch (scope) {
        case AntiScope::Operator:  return "算子（否的是「怎么算」：算子/度量/容差形态）";
        case AntiScope::Stimulus:  return "激励（否的是「怎么激励 / 怎么采集」）";
        case AntiScope::Domain:    return "论域（否的是「在哪些像素 / 几何上断言」）";
        case AntiScope::Baseline:  return "参考（否的是「拿什么当参考」）";
    }
    return "?";
}

namespace {
/// 反模式登记表。四格顺序 = name, scope, why, stillOpen。
///
/// **名字只写一次**：`antiPatterns()` 的 map 键从 `name` 取，键与条目名漂移在构造上不可能
/// （把键再抄一遍就是第二份真相，本批次要治的正是这个）。
///
/// 每条三格：scope（否掉的是哪一层）+ why（历史记录，含后续更新，**不删**）+
/// stillOpen（**未被否的部分**）。stillOpen 不是装饰：`screen-space-l1-frame-diff` 为
/// "没被提到 = 被禁止"付过两天学费（原文末段），而当时的修法是往同一段散文再打一层补丁。
/// 分工（两次变异实测）：**编译器管"有没有"** —— 漏填 scope（把 why 串到第 2 格）= const
/// char* 转不进 enum class；漏填 stillOpen = `-Wmissing-field-initializers` 在 -Werror 下
/// 一样是 error。两条都**编译失败**，不是自检红。**selftest 管"是不是空话"** —— stillOpen
/// 非空、够长、且必须含反引号包裹的具体命令/判据名（`testAntiPatterns`）。
constexpr std::array<AntiPatternInfo, 8> kAntiAll = {{
        {"ssim", AntiScope::Operator,
            "结构相似度不作主门（Filament 调研推翻原计划：其全库无 SSIM/PSNR/FLIP）。"
            "理由是判据性质：SSIM 把「整幅」当一个样本，失败时既说不出**哪块**错、也说不出"
            "错在亮度还是几何；且它有告警饱和——错得越多分母越稳。替代 = 闭式解锚点 + "
            "归属区域统计（本工具）+ 开关影响域。"
            "2026-10-03 再裁决（用户提议「红线回归改 PSNR/SSIM」后否决）：截图回归的比对"
            "算子同样固定为**逐位相同**——逐位是零容差 A/B 算子，红线防的正是「泄漏一点点」；"
            "且现机制从不存 golden（同会话双构建），PSNR 那条拒绝理由在现机制里并不存在。",
            "被否的是「拿 SSIM 当比对算子」，**不是「不许比较」**：截图回归的比对算子固定为**逐位相同**（本条 2026-10-03 再裁决原文），替代算子全在用 = `probe` 的闭式解锚点、`attribution` / `faces` 的归属区域统计、`switch` 的开关影响域。"},
        {"psnr", AntiScope::Operator,
            "PSNR 需要一张参考图，而本项目**跨日 ±1 LSB 噪声**使 golden 存不过一天"
            "（docs/调试历史.md 2026-09-08）。它把「多少像素差了多少」压成一个数，方向信息全丢。",
            "被否的是 PSNR 这个算子，**不是「与参考对拍」这件事**：`probe` 拿闭式解当参考逐字节对拍、`aa-coverage` 拿投影宽度 w 当参考，两者都不存 golden（正是本条给的第二个选项）。"},
        {"global-epsilon", AntiScope::Operator,
            "全局容差同时**漏掉大的、冤枉小的**。纪律：容差必须来自**为什么会差**（float32 "
            "多项式？8bit 取整？跨驱动 dither？）。写不出理由的判据就是还没想清楚，不该存在。",
            "被否的是「一个全局 epsilon 通吃」，**不是「不许有容差」**：每条判据各自带理由的容差全在用（如 `probe-pixel` 的 ±2 LSB、`face-clusters-explained` 的簇间隙 0.02），`check` 逐条打印这些理由。"},
        {"assert-invisible-geometry", AntiScope::Domain,
            "对「该取景下其实看不见的几何」下断言 = 恒真 = 会绿 = 无效断言。先 attribute "
            "反解可见集，判据只在可见集上求值。",
            "被否的是论域选错（对看不见的几何下断言），**不是「不许对几何下断言」**：`attribution` 反解出可见集之后，`faces` 逐面、`switch` 逐 piece 的断言照下，只是求值域换成可见集。"},
        {"cross-day-golden", AntiScope::Baseline,
            "存一张基线隔日比 = 把驱动/编译器 codegen 微差当成「渲染变了」。本项目历史上"
            "因此连续作废 7 次基线。要么同会话双构建，要么根本不要 golden。",
            "被否的是「隔日基线」这个参考，**不是「不许有参考」**：同会话双构建逐位比对仍是红线回归的正解，闭式解参考（`probe`、`aa-coverage`）根本不存图，因此天然不受跨日 ±1 LSB 噪声影响。"},
        {"eyeball-only", AntiScope::Operator,
            "纯目视不可复现、不可回归。目视用来**发现**问题，发现后必须落成判据。",
            "被否的是「只目视就当结论」，**目视用来发现问题仍然有效且是常规入口**（本条原文即如此）：`llm_debug` 截图读图负责发现，落点必须是 `attribution` / `faces` / `switch` / `probe` 这类判据。"},
        {"moving-camera-l1-ratio", AntiScope::Operator,
            "「匀速微动机位下，开/关 TAA 的逐帧差之比降一个量级」不能当判据（plan §6 初稿"
            "就这么写的，实测对正确实现也只有 1.2×）。两条独立理由：① 时域滤波压的是「同"
            "视点不同相位」的估计差，压不掉「视点真的动了」的正确画面变化；② L1 逐帧差对"
            "模糊近似**守恒**——边缘摊开会让更多像素各差一点，均值几乎不变（实测：变化像素"
            "3.10%→5.54% 而幅度 0.53→0.245，乘积不变）。替代 = 静止机位 + jitter 激励 + "
            "feedback=1 消融组（imgqa temporal 的 S 组），比值可对拍一阶 IIR 闭式。"
            "⚠ 该替代方案本身也已被推翻（同一个 L1 守恒病根），见 `screen-space-l1-frame-diff`。",
            "被否的是「L1 总量比值」这个**算子**，**移动机位这个激励没有被否**：它现在天天在跑 = `temporal-motion`（默认运动源就是相机帧驱动 yaw）、`temporal-flicker`、`temporal-homography`。**别拿本条劝退它们**。"},
        {"screen-space-l1-frame-diff", AntiScope::Operator,
            "上一条的替代方案（静止机位 + jitter + 屏幕空间 L1 逐帧差）**同样不能当抑制比"
            "判据**，同一个病根换个激励没逃掉：jitter 让采样网格每帧动 ±0.5px，「相邻帧的"
            "同一屏幕像素」量的仍是**不同世界点**上的估计差 = 梯度 × 网格位移；边缘摊开后"
            "梯度变小、作用面积变大，乘积不变。实测指纹（/shotburst 连续帧、可复现）："
            "S1 均值 0.027060 / 变化 7.13% ⟹ 单像素幅度 0.380；S2 均值 0.022974 / 变化 "
            "12.32% ⟹ 幅度 0.186。**幅度降 2.04× 被像素数涨 1.73× 抵消**，净比值只剩 1.18×"
            "——与微动版指纹同构。闭式那个 3.95×/11× 是**对齐空间**的预测，两次都拿它对了"
            "屏幕空间的观测。换 L2 也救不回来（只给 √k，按同批数据算出 1.55×）。"
            "正确替代 = 单帧逐杆覆盖率对拍闭式（plan §7 第 10 条）：投影宽度 w 的杆，正确 "
            "AA 的输出亮度 ≈ w，无 AA 时按相位取 0 或 1。\n"
            "【2026-10-03 更新，不改变本条有效性】修好「跨帧历史每帧被 UNDEFINED 销毁」后 "
            "（plan §7 第 12 条），本条的**诊断被完整证实**：L1 总量仍只给 1.38×，而把观测换到"
            "**变化像素上的平均幅度**，实测降幅 3.99× ≈ 闭式 √16 = 4.00×。⟹ 被推翻的一直只是"
            "「用 L1 总量」这个算子，闭式预测本身没错；抑制比已在新观测空间重立为判据 "
            "`temporal-suppression-sqrtN`。**别拿这条反模式去否定抑制比本身**。\n"
            "【2026-10-06 补，**本条的作用域**】被否的一直是**算子**（L1 总量），**不是激励**"
            "（静止机位 + jitter + 逐帧差）。首句「同一个病根换个激励没逃掉」当时被读成了"
            "「这个激励不值得测」⟹ 该激励整条冻结两天；而 10-05 重立时用的**正是它**，只换了算子"
            "（变化像素**占比** + 幅度分层 + 闭式 ρ*）= `temporal-residual-*` 家族，也是唯一抓到"
            "批 13.10/13.11「静止机位仍闪」根因的判据。**登记反模式必须写明被否的是算子还是激励**"
            "——作用域写宽一格，防腐条款就连带冻掉后来真正需要的激励"
            "（同纪律见 skill `rgl-test-discipline` §负知识的作用域）。",
            "被否的是「屏幕空间 L1 总量」这个**算子**，**静止机位 + jitter 这个激励没有被否**：它是 `temporal-residual-*` 家族的地基，也是唯一抓到批 13.10/13.11「静止机位仍闪」根因的判据；抑制比换了算子后重立为 `temporal-suppression-sqrtN`。"},
}};
}  // namespace

const std::map<std::string, AntiPatternInfo>& antiPatterns()
{
    // 装一条但**拒绝执行**，比不装更好：否则下一个会话的我（或别人）会重新发明它。
    static const std::map<std::string, AntiPatternInfo> kAnti = [] {
        std::map<std::string, AntiPatternInfo> m;
        for (const auto& a : kAntiAll)
            m.emplace(a.name, a);
        return m;
    }();
    return kAnti;
}

// ── 调用记录（判据可信度的一手数据；**没有记录，「打磨」就只是随手加 flag**）──

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
