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
        {"shadow-swim",
            "0 位移 + 失配 ≤ max(20, 共同格/200)：静态几何 + 静态太阳 ⟹ 影场是世界不变量，"
            "任何机位依赖都是 bug（5a-7 三件套的端到端复验）。2cm 格分辨率对用户报的"
            "「井盖影时有时无」绰绰有余；亚格漂移本就不可见。平移搜索只认覆盖率 ≥80% 的候选"
            "（防止把「两边各遮一半」的垃圾平移当最优解）。",
            {"5a-7 平移游动 / 5a-8b 旋转游动（当年判据是人工三图对拍，本条机器化）"}, false},
        {"temporal-flicker-anchor",
            "变化像素占比下限 0.5%：激励 = jitter（±0.5px/帧，Halton 周期 16），靶的亚像素"
            "边缘按闭式投影约占靶面 11%（画板边界 2×(869+248) + 12 杆竖边 + 6 环 + 16 放射"
            "+ 12 斜杆 + 栅栏），±0.5px 的相位跳变让每条边几乎每帧都跨像素中心 ⟹ 期望远大于"
            "1%。下限 0.5% 只吸收「靶根本没画 / 投出屏 / id 面为空」这类归零失效；均值同时"
            "必须 > 0。⚠ 本条量的是 S1（feedback=1 ⇒ 输出 = 本帧，只抖不滤），不是「TAA "
            "关」——理由见 temporal.cpp 头注释「为什么激励是静止机位 + jitter」。",
            {"docs/taa_plan.md §6 阳性锚点（缺它 = 「0 差异」的两种解释堵不住）"}, false},
        {"temporal-aa-presence",
            "两条互为阴阳，缺一条就是假绿：① **TAA 关时中间调须恰为 0**（不是「接近 0」）——"
            "无 MSAA 的光栅化是二值覆盖，实测全图只有 3 个亮度级（背景 clear / 白板 / 黑件），"
            "任何非零说明靶画错了或链路上混了别的混合；有这条反向对照，② 才不是「靶本来就糊」。"
            "② TAA 开中间调 ≥ 1.0%：闭式预期约 11%（靶的亚像素边缘像素占比），下限取 1/10 "
            "只吸收「邻域裁剪把一部分历史拉回本帧」；带按**实测白电平**取（ACES 把白压到 ~0.8，"
            "写死绝对带会把整块白板算成中间调、83% 恒饱和——真实踩过的假判据，见 selftest）。"
            "⚠ 本条量的是**空间**抗锯齿在场，不是时域抑制比——后者不能用屏幕空间逐帧差量，"
            "见 `screen-space-l1-frame-diff`。",
            {"docs/taa_plan.md §6（2026-10-03 执行时由逐帧差判据改立）"}, false},
        {"temporal-suppression-sqrtN",
            "16 相位抖动 = 对同一视点的 16 个独立采样相位求平均 ⟹ 逐样本估计噪声按 **√N = 4.00**"
            " 下降（闭式，不依赖实现）。下限取 **3.0×**（= √N 的 75%）只吸收两件事：邻域裁剪会把"
            "一部分历史拉回本帧（相位间不再独立 ⟹ 实际降幅低于 √N），以及 Halton 是低差异序列而"
            "非独立随机（方差略优于独立 ⟹ 实际降幅也可能略高于 √N）。实测 3.99×。\n"
            "⚠ **观测空间是「变化像素上的平均幅度」，不是 L1 总量**——这是本判据与被打反模式的"
            "`temporal-taa-suppression` 的唯一但决定性差别：L1 总量 = 幅度 × 变化像素数，边缘摊开"
            "后两者互抵（实测 3.99× × 1/2.88 = 1.38×），对模糊守恒 ⟹ 量不出抑制。闭式预测从来"
            "是对的，**是它的适用域（逐样本幅度）被错配到了总量上**。修好历史销毁 bug 后两者同时"
            "对上，反模式条目保持有效。",
            {"docs/taa_plan.md §6 第 5 条 + §7 第 12 条（2026-10-03 换观测空间后重立）"}, false},
        {"aa-coverage-fidelity",
            "两条同时成立才绿，各堵一种假绿。**判据域 = 投影宽度 ≥2 列的杆**：① **TAA 开：域内"
            "最大逐杆黑度积分误差 ≤ 0.35 列**（闭式期望 = 投影宽度，白件 unlit 线性 1、黑件 0，"
            "覆盖 c ⟹ 线性 1−c；0.35 列 = 域内最窄杆 2.25 列的 1/6，即「把杆宽画错六分之一以上」"
            "才算坏）。② **TAA 关必须显著更差（≥3×）**——反向对照：没有它，① 可能只是靶太粗、"
            "二值覆盖也刚好贴闭式。实测 TAA 关的误差来自量化（每根 = `floor(期望)`，误差 ~0.2–0.9 "
            "列），与 ① 的量级差是结构性的，不是调出来的。\n"
            "⚠ **域外（<2 列）不是调阈值调出去的，是算法类没有保证**：邻域裁剪 TAA 对 ≲2 列的细杆"
            "必然欠累积——错相帧里本帧没采到杆 ⟹ box5 = 全白 ⟹ 已累积的黑被判在盒外拒掉，单帧值"
            "由抖动相位决定，不是稳定质量量。窄杆仍打印，欠累积量级记进 plan §7 记债（box9 / "
            "variance 扩盒 / Lanczos filterInput 是缓解方向）。\n"
            "⚠ 覆盖率由 `1 − aces⁻¹(luma)` 反解，正向复用 `analytic.cpp` 的闭式 ACES（"
            "`imgqa probe` 已拿真机 ±2 LSB 对拍验证），不新引入第二条曲线。\n"
            "⚠ 本判据是**单帧**判据，与采集时序无关——这是它取代屏幕空间逐帧差的根本理由"
            "（后者对模糊守恒，见 `screen-space-l1-frame-diff`）。",
            {"docs/taa_plan.md §7 第 10 条（逐杆覆盖率对拍闭式）"}, false},
        {"izb-consume-domain",
            "变暗阈 = 三通道和差 >30：rig 调色板 albedo（≥0.5）× 方向光 12 → 影差远大于 30"
            "（ladder 首跑实测：变暗 4705/域外 0/未消费 0）；域容差 = dilate(mask,2px)：mask "
            "与截图同帧同源，2px 只吸收保守光栅的过估边（mask 侧只会多遮不会少遮，方向安全）。"
            "三条子判据：锚点（变暗集非空 = 消费活着）、变暗域 ⊆ mask（寻址对得上）、mask∩受光 "
            "⊆ 变暗域（有遮必消费）。⚠️ 边界：本判据 mask↔画面**同源**，生产者侧几何错（ndc "
            "镜像类）会同步污染 mask 与画面 → 本判据不可见，那半边由 test_izb_engine_contract"
            "（A1 闭式）钉；mask vs manifest 预测影包络（生产者端到端）= 已识别扩展，未做。",
            {"2026-10-02 消费端字节偏移 ÷4 bug（门开无影）→ 锚点+消费两红——本判据设计原型即那次手工对拍"}, false},
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
        {"moving-camera-l1-ratio",
            "「匀速微动机位下，开/关 TAA 的逐帧差之比降一个量级」不能当判据（plan §6 初稿"
            "就这么写的，实测对正确实现也只有 1.2×）。两条独立理由：① 时域滤波压的是「同"
            "视点不同相位」的估计差，压不掉「视点真的动了」的正确画面变化；② L1 逐帧差对"
            "模糊近似**守恒**——边缘摊开会让更多像素各差一点，均值几乎不变（实测：变化像素"
            "3.10%→5.54% 而幅度 0.53→0.245，乘积不变）。替代 = 静止机位 + jitter 激励 + "
            "feedback=1 消融组（imgqa temporal 的 S 组），比值可对拍一阶 IIR 闭式。"
            "⚠ 该替代方案本身也已被推翻（同一个 L1 守恒病根），见 `screen-space-l1-frame-diff`。"},
        {"screen-space-l1-frame-diff",
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
            "`temporal-suppression-sqrtN`。**别拿这条反模式去否定抑制比本身**。"},
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
