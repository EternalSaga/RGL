// 自检：判据自己要有判据。**断言全部照实测/源码写，不照记忆写**（本项目纪律：
// 「我以为的行为」一轮写出过 10 处，全部是错的）。闭式解锚点来源逐条注明。
//
// 拆文件后的**地基族** + 汇总路由（缝的说明全文见 `selftest.h` 头注释）。本 TU 收三台
// 「谁都要踩一脚」的仪器：`image.cpp` 的图像算子、`judge.cpp` 的判据与反模式注册表、
// `cli.cpp` 的 CLI 注册表（默认值的唯一出处）。它们与 `cmdSelftest` 同处的理由不是「行数
// 还剩得多」，而是**注册表类自检的失败最会伪装成「什么都没发生」**（空表让后面所有断言恒真
// = 假绿）——把「注册表非空」这类前提守卫与汇总路由放在一起，读失败输出时第一眼就能分辨是
// 地基塌了还是某台量具塌了。
//
// 搬动纪律：断言本体逐字搬自拆分前的 selftest.cpp（一条不多、一条不少、不改判定方向与阈值）。
#include "selftest.h"

#include "cli.h"
#include "imgqa.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

using nlohmann::json;

namespace {

/// stillOpen 里的「具体名字」判据：至少一对反引号、其间不是空白。空话（"其他方法仍然可用"）
/// 写不出名字 ⟹ 这一条就是「不许写空话」的机器判据（作用域写宽付过两天学费，见 judge.cpp）。
bool hasBacktickedName(const char* s)
{
    if (s == nullptr)
        return false;
    const char* open = std::strchr(s, '`');
    if (open == nullptr)
        return false;
    const char* close = std::strchr(open + 1, '`');
    if (close == nullptr)
        return false;
    for (const char* p = open + 1; p != close; ++p)
        if (*p != ' ' && *p != '\t')
            return true;
    return false;
}

/// 断言"这一句必须抛 UsageError"。校验类代码不抛就是摆设——阳性锚点。
/// 抛**别的**类型也算失败：main 靠 UsageError 这个类型把 rc 分成 2（你敲错了）
/// 与 3（跑挂了），混型就等于把用法错误伪装成运行故障。
template <typename Fn>
bool throwsUsage(Fn&& fn)
{
    try {
        fn();
    } catch (const UsageError&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

/// 断言"这一句不许抛"（反向用例：合法参数被挡下 = 判据反过来咬人）。
template <typename Fn>
bool noThrow(Fn&& fn)
{
    try {
        fn();
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

/// Number 类 flag 的默认值串必须能被 stod 解析。不查这条的后果很具体：dispatch 里
/// `P.number("--burst")` 当场抛，而它抛的是 std::invalid_argument（不是 UsageError）
/// ⟹ rc=3「跑挂了」，用户看不出真正的问题是注册表里写错了默认值。
bool numberDefaultParses(const FlagSpec& f)
{
    try {
        std::stod(f.def);
    } catch (...) {
        return false;
    }
    return true;
}

}  // namespace

void testSrgb(Checker& c)
{
    // sRGB 往返：全域 decode(encode(x)) ≈ x。
    double worst = 0.0;
    for (int i = 0; i <= 2000; ++i) {
        const double x = i / 2000.0;
        worst = std::max(worst, std::abs(srgbDecode(srgbEncode(x)) - x));
    }
    c.chk("sRGB 往返全域", worst < 1e-6, "worst=" + std::to_string(worst));
    c.chk("sRGB(0.5) 线性 ≈ 0.214", std::abs(srgbDecode(0.5) - 0.216) < 2e-3,
        std::to_string(srgbDecode(0.5)));
}

void testObjectId(Checker& c)
{
    // 编码约定 = convR32Uint：R=id[7:0] G=[15:8] B=[23:16]；0 = 背景。
    cv::Mat bgr(3, 3, CV_8UC3, cv::Scalar(0, 0, 0));
    bgr.at<cv::Vec3b>(0, 0) = cv::Vec3b(0xFE, 0xBA, 0xBE);  // B=FE G=BA R=BE → 0xBEBEFE? 不
    // 上面故意不写死——按 R=id[7:0] 手推：R=0xBE G=0xBA B=0xFE → id=0xFEBABE。
    // cv::Vec3b 序是 (B,G,R)，所以写入 (0xFE,0xBA,0xBE) 恰是 B=FE G=BA R=BE。
    bgr.at<cv::Vec3b>(1, 1) = cv::Vec3b(0x00, 0x00, 0x01);  // R=1 → id=1
    const cv::Mat ids = decodeObjectId(bgr);
    c.chk("ID 小端解码 0xFEBABE", ids.at<int32_t>(0, 0) == 0xFEBABE,
        std::to_string(ids.at<int32_t>(0, 0)));
    c.chk("ID = 1 不是 0", ids.at<int32_t>(1, 1) == 1);
    c.chk("背景是 0", ids.at<int32_t>(2, 2) == 0);
}

void testRegion(Checker& c)
{
    cv::Mat m = cv::Mat::zeros(20, 30, CV_8UC1);
    m(cv::Rect(2, 2, 4, 4)).setTo(255);
    m(cv::Rect(12, 2, 4, 4)).setTo(255);
    m(cv::Rect(20, 12, 8, 6)).setTo(255);
    c.chk("连通域 = 3", connectedCount(m) == 3, std::to_string(connectedCount(m)));
    c.chk("空图连通域 = 0", connectedCount(cv::Mat::zeros(5, 5, CV_8UC1)) == 0);
    const cv::Mat d1 = dilateMask(m, 1), d3 = dilateMask(m, 3);
    c.chk("膨胀单调", cv::countNonZero(d1) >= cv::countNonZero(m)
        && cv::countNonZero(d3) >= cv::countNonZero(d1));
    cv::Mat containment;
    cv::bitwise_and(m, ~d1, containment);
    c.chk("膨胀含原集", cv::countNonZero(containment) == 0);
    cv::Mat overlap;
    cv::bitwise_and(ringMask(m, 2), m, overlap);
    c.chk("环带与原集不交", cv::countNonZero(overlap) == 0);
}

void testAnyChannelDiff(Checker& c)
{
    // 阳性锚点 = **逐通道分别**制造差异。只比 channel 0 的实现能过"BGR 全不同"这条，
    // 但过不了下面逐通道的三条；"简化"回 cv::reduce(axis=0) 的实现过不了最后一条
    // （reduce 对多通道输入按通道独立归约、输出仍是多通道，countNonZero 断言 cn==1 炸）。
    // 这条教训原本抄在 4 个 TU 里各一份，收敛单点后由本用例守住。
    const cv::Mat a = cv::Mat::zeros(4, 5, CV_8UC3);
    const cv::Mat same = a.clone();
    c.chk("相同图 → 全 0", cv::countNonZero(anyChannelDiff(a, same)) == 0);

    for (int ch = 0; ch < 3; ++ch) {
        cv::Mat b = a.clone();
        b.at<cv::Vec3b>(2, 3)[ch] = 7;
        const cv::Mat d = anyChannelDiff(a, b);
        c.chk("任一通道单独不同都必须抓到", cv::countNonZero(d) == 1,
            "ch=" + std::to_string(ch) + " got=" + std::to_string(cv::countNonZero(d)));
        c.chk("差异落在正确像素上", d.at<uint8_t>(2, 3) != 0);
    }

    const cv::Mat d = anyChannelDiff(a, same);
    c.chk("输出是单通道 8UC1（下游 countNonZero 的前提）",
        d.channels() == 1 && d.depth() == CV_8U,
        "channels=" + std::to_string(d.channels()) + " depth=" + std::to_string(d.depth()));
}

void testClusters(Checker& c)
{
    std::mt19937 rng(0);  // 写死种子（测试纪律：真随机的失败无法重跑）
    std::normal_distribution<double> noise(0.0, 1e-4);
    cv::Mat v(1, 1500, CV_64FC1);
    for (int i = 0; i < 500; ++i)
        v.at<double>(0, i) = 0.10 + noise(rng);
    for (int i = 500; i < 1000; ++i)
        v.at<double>(0, i) = 0.40 + noise(rng);
    for (int i = 1000; i < 1500; ++i)
        v.at<double>(0, i) = 0.85 + noise(rng);
    c.chk("三簇分出三簇", lumaClusters(v).size() == 3,
        std::to_string(lumaClusters(v).size()));
    cv::Mat one(1, 200, CV_64FC1, cv::Scalar(0.37));
    c.chk("单值 = 一簇", lumaClusters(one).size() == 1);
    c.chk("空输入 = 空簇", lumaClusters(cv::Mat()).empty());
}

/// 债-99：`percentile100` 空集时 `size() − 1` 下溢成 SIZE_MAX = **越界读**（触发条件真实
/// 存在：mask 全灭时 `sortedLumaInMask` 返回空 vector）。修法 = 抛；锚点必须同时钉住
/// 「0 是合法分位数」——否则「改成返回 0 哨兵」也能骗绿（本函数自己的注释写着两个刻度
/// 传错时「分位数静默变成 0」，修法不许再造同一个静默形态）。
void testPercentile100(Checker& c)
{
    bool threw = false;
    double got = -1.0;
    try {
        got = percentile100({}, 50.0);
    } catch (const std::exception&) {
        threw = true;
    }
    c.chk("percentile100：空集 → 抛（改前 size()−1 下溢 = 越界读，堆上鬼数当读数）", threw,
        "实得 " + std::to_string(got));
    // 可区分性：0.0 是**合法分位数**不是失败码——空集抛、含零正常出 0。
    c.chk("percentile100：全零序列 p50 = 0.0（0 是合法读数，与「空集抛」可区分）",
        percentile100(std::vector<double>{0.0, 0.0}, 50.0) == 0.0);
    // 闭式（截断下标口径，不做线性插值）：n=5 ⟹ p50 = sorted[0.5·4]=sorted[2]；
    // p95 = sorted[⌊0.95·4⌋]=sorted[3]；p100 = sorted[4]。变异红线：改成四舍五入/向上取整 → 后两条红。
    const std::vector<double> v{10.0, 20.0, 30.0, 40.0, 50.0};
    c.chk("percentile100：p50 = 闭式 30（下标 0.5·4 = 2 截断）", percentile100(v, 50.0) == 30.0);
    c.chk("percentile100：p95 = 闭式 40（下标 3.8 截断为 3，不插值）", percentile100(v, 95.0) == 40.0);
    c.chk("percentile100：p100 = 末元素（下标恰 n−1，不许越界）", percentile100(v, 100.0) == 50.0);
}

void testRegistry(Checker& c)
{
    c.chk("反模式登记含 ssim/psnr/global-epsilon",
        antiPatterns().count("ssim") && antiPatterns().count("psnr")
            && antiPatterns().count("global-epsilon"));
    // **普适不变量**（债-08 降级后这条机制的牙）：**每条**判据都必须有 ① 写得出理由的容差
    // （>20 字符，不是"0.5%"式敷衍）② 至少一条 guard 出处（真实 bug 或真实变异）。
    // 原来这条挂在「已晋升判据」的分支下，而那个标志无一处置 true ⟹ 循环体恒不执行 = 摆设。
    // 前提守卫：注册表若为空，下面两条对空循环**恒真** = 假绿，所以先钉住它非空。
    const auto& all = registeredJudgments();
    c.chk("判据注册表非空（否则下面两条恒真是假绿）", !all.empty(), std::to_string(all.size()));
    for (const auto& j : all) {
        c.chk("每条判据必须有 guard 出处", !j.guards.empty(), j.name);
        c.chk("每条判据的容差理由必须 >20 字符", std::strlen(j.whyTol) > 20, j.name);
    }
}

/// 族拆分的护栏：拆表最容易出的三种事故（掉条目 / 复制粘贴出重名 / 全塞一族）。
void testJudgmentFamilies(Checker& c)
{
    const auto& all = registeredJudgments();
    // 54 是**当前**条数，写死它才有牙：族拆分/合并时漏接一个子表，这里立刻红。
    // 43 → 46 = 批 13.14a `temporal-tail --motion camera` 新增三条（smear-alpha-monotone /
    // static-collapse / gate-breakpoint）；46 → 47 = 同批把 **φ 符号标定**从诊断升格成判据
    // （`temporal-tail-phi-sign`）——理由见其契约：自搭金标准与实现共享朝后的相机基，
    // 单测对拍 2.3e-13px 全绿仍漏掉 yaw 反号，那一半只有实机数据守得住。
    // 47 → 51 = 批 13.15 `temporal-tail --motion step`（债-63 真鬼影量具）四条：零锚 /
    // 历史存活（防 `cameraJumps` 跳切假绿）/ 闭式锚 / 主判据。
    // 51 → 52 = 批 13.15b 的 C13 臂判决「修法验收」。
    // 52 → 54 = 批 14.5 FSR 臂两条守卫（`temporal-tail-step-fsr-floor` / `-basespace`）。
    // 54 → 57：批 N6d 步 0 的 `shadow-softness` 三条（J1 量具有效 / J2 现状缺陷 / J3 对账出数）。
    // 57 → 60：`shadow-kernel-scale` 三条（CSM 本影存在性 / 核尺度不随 cascade 半径漂移 /
    // PCSS 半影随高度展开）。写法照抄 `shadow-softness` 那三条，见 judgments_table_shadow.cpp。
    // 60 → 62：批 15.2 子步 D1 的 `diffstruct` 两条（变暗阳性锚点 / 域外变暗恰 0），见
    // judgments_table_ssr.cpp。
    // 62 → 63：★0.5-b1 的 `diffblock` 一条（U(r) 恰 0 = 差异处处够薄没有整块），见
    // judgments_table_block.cpp。
    // 63 → 65：探针重立到 deferred 的两条 G-buffer 编码锚（`probe-gbuffer-albedo` /
    // `probe-gbuffer-metalrough`，2026-10-18 图变体收敛刀①，docs/archive/图变体收敛_plan.md §6.1-1）。
    // 65 → 69：债-132 结案的 `blend-sort` 四条（背景区钉显示链 / 两个 α 各钉一次非预乘 over /
    // 重叠区钉合成次序），见 judgments_table_blend.cpp。
    // 69 → 71 → 72：`shadow-grain` 首版两条（痤疮上界 + 锚点，69→71）→ 2026-10-19 判决
    // 方向纠正：两条退役、三条新立（J1 存活率 / J2 接线锚点 / J3 出厂档对账），见 judgments_table_grain.cpp。
    // 72 → 75：`shadow-cascade-seam` 三条（债-110 把 ★18 的图像侧判据从一次性脚本入库）：
    // J1 LOD 归因的缝台阶增量 / J2 两 force 臂阳性锚点 / J3 D 扫描形状，见 judgments_table_seam.cpp。
    // 75 → 77：`shadow-caster-survival` 两条（债-139 补位：差分判据对「四级一起变差」结构失明，
    // 本台量**绝对量**存活率）：J1 出厂档小块/近场存活率（🔴 阈值未定 TODO：运行期只出 INFO）/
    // J2 本影面积存活率随 D 单调不增，见 judgments_table_survival.cpp。
    // 77 → 80：`shadow-edge-ruler` 三条（真值 = 闭式算术，补两台「拿渲染比渲染」仪器对共模
    // 错误的结构失明）：J1 出厂档绝对误差 / J2 相邻级缝宽（两条界未定只报数）/ J3 |误差| 随 D
    // 单调不减（形状判据），见 judgments_table_edge.cpp。
    // 80 → 85 → 86：`blend-lit`（债-134 的先决：透明段 lit 变体的常驻像素贡献）——引擎回显
    // 条数 / 反向臂条数恰 0 / footprint 内确有不同 / footprint 外逐位相同 / 同臂内 pane 众数
    // ≠ 背景板众数 / [error] 计数为 0。第 6 条不是想到的，是**变异跑出来的**：删掉透明段
    // append 循环后「两臂不同」那条照旧绿（BLEND 臂露出背景板 vs OPAQUE 臂的不透明 pane），
    // 详见 judgments_table_blend_lit.cpp 的 blend-lit-visible 正文。
    // 与 blend-sort 四条的分工 = 那边证「画对了」（有闭式），这边证「**有**贡献」（无闭式，刻意）。
    // 86 → 89：`shadow-edge-rebase`（相对坐标批 5 第 2 刀，四格对拍 = 2 工作点 × 2 开关态）
    // 三条：J-R1 出厂点「不坏」/ J-R2 远点「变好」🔴（rebase 的存在理由）/ J-R3 阴影在场性。
    // J-R3 不是想到的，是**实测跑出来的**：远点门关态读出的不是「影边摆偏」而是「影边根本不
    // 存在」（csm 臂与 none 臂逐位相同），而那种格子在误差表上是一个 NaN，与「阴影对但量不出」
    // 同形 ⟹ 必须有一条独立判据在这里分家（详见 judgments_table_edge.cpp 的 rebase 段）。
    // 这条硬计数的意义 = 族拆分/改名最容易掉条目，而掉掉的正是没人再读的那几条。
    c.chk("判据总数 == 89（族拆分最容易掉条目）", all.size() == 89, std::to_string(all.size()));
    size_t n[4] = {0, 0, 0, 0};
    for (const auto& j : all) {
        switch (j.family) {
            case JudgmentFamily::Foundation: ++n[0]; break;
            case JudgmentFamily::TemporalStatic: ++n[1]; break;
            case JudgmentFamily::TemporalMotion: ++n[2]; break;
            case JudgmentFamily::Attachment: ++n[3]; break;
        }
    }
    const std::string detail
        = "地基" + std::to_string(n[0]) + " 静止" + std::to_string(n[1]) + " 运动"
        + std::to_string(n[2]) + " 附件" + std::to_string(n[3]);
    // 编译器管"漏填族"（const char* 转不进 enum），这条管"族字段白加"——全塞一族。
    c.chk("四个族都非空（全塞一族 = 族字段是装饰）",
        n[0] > 0 && n[1] > 0 && n[2] > 0 && n[3] > 0, detail);
    std::set<std::string> names;
    for (const auto& j : all)
        names.insert(j.name);
    c.chk("判据名全局唯一（防拆分时复制粘贴出重复条目）", names.size() == all.size(),
        std::to_string(names.size()) + " 个唯一名 / " + std::to_string(all.size()) + " 条");
}

void testAntiPatterns(Checker& c)
{
    // 反模式登记必须是**带作用域的结构化登记**，不是一段散文。三格各有机器判据：
    //   ① why 非空（被否的是什么）② stillOpen 非空且 >20 字符（未被否的部分不许敷衍）
    //   ③ stillOpen 必须点名具体命令/判据（反引号包裹）——"其他方法仍可用"过不了这条。
    // ⚠ ②是 std::strlen（**字节**）判据：8 个汉字（24 字节）的空话过得了 ②，只有 ③ 拦得住
    //   ——变异实测过（stillOpen = "其他方法仍然可用" ⟹ 仅 ③ 红）。两条不是冗余。
    // 前提守卫同 testRegistry：登记表若为空，下面三条对空循环**恒真** = 假绿。
    const auto& anti = antiPatterns();
    c.chk("反模式登记表非空（否则下面三条恒真是假绿）", !anti.empty(), std::to_string(anti.size()));
    for (const auto& [name, a] : anti) {
        c.chk("每条反模式必须有 why（被否的是什么）", a.why != nullptr && a.why[0] != '\0',
            name.c_str());
        c.chk("每条反模式必须写 stillOpen 且 >20 字符",
            a.stillOpen != nullptr && std::strlen(a.stillOpen) > 20, name.c_str());
        c.chk("每条反模式的 stillOpen 必须点名具体命令/判据（反引号名）",
            hasBacktickedName(a.stillOpen), name.c_str());
    }
}

/// CLI 注册表自检·**结构面**（批次 B）：这张表是默认值的唯一出处，它自己得先自洽。
void testCliRegistryShape(Checker& c)
{
    const auto& table = subcommands();
    c.chk("CLI 注册表非空（20+ 子命令）", table.size() >= 20, std::to_string(table.size()));

    std::set<std::string> names;
    for (const auto& s : table) {
        c.chk("子命令名唯一", names.insert(s.name).second, s.name);
        std::set<std::string> flags;
        for (const auto& f : s.flags) {
            c.chk("同命令内 flag 名唯一", flags.insert(f.name).second,
                std::string(s.name) + " " + f.name);
            c.chk("flag 名以 -- 开头", std::string(f.name).rfind("--", 0) == 0, f.name);
            if (f.kind == FlagKind::Bool) {
                c.chk("Bool flag 不许有默认值串", f.def == nullptr,
                    std::string(s.name) + " " + f.name);
                continue;
            }
            c.chk("非 Bool flag 必须有默认值串", f.def != nullptr, f.name);
            if (f.kind == FlagKind::Number)
                c.chk("Number 类默认值能被 stod 解析", numberDefaultParses(f),
                    std::string(s.name) + " " + f.name + "='" + (f.def ? f.def : "") + "'");
        }
    }

    // 反向用例：不存在的命令返回空，而不是某个默认项。
    c.chk("findSubcommand(不存在的命令) → 空", findSubcommand("不存在的命令") == nullptr);
    c.chk("findSubcommand(空串) → 空", findSubcommand("") == nullptr);
}

/// CLI 注册表自检·**行为面**：默认值只有一个出处 + 未知参数校验不是摆设。不碰引擎。
void testCliArgsBehavior(Checker& c)
{
    const auto* tail = findSubcommand("temporal-tail");
    c.chk("注册表里有 temporal-tail", tail != nullptr);
    if (tail == nullptr)
        return;
    const std::vector<std::string> noargs;
    const Args empty(noargs, *tail);

    // ── 阳性锚点：读未声明的 flag 必须抛（不抛 = 默认值没有真相源）──
    c.chk("get 未声明 flag → 抛", throwsUsage([&] { (void)empty.get("--strenght"); }));
    c.chk("has 未声明 flag → 抛", throwsUsage([&] { (void)empty.has("--strength"); }));
    c.chk("number 未声明 flag → 抛", throwsUsage([&] { (void)empty.number("--nope"); }));

    // ── 已声明 flag 的默认值 = 注册表里那一串（含被证伪后修正的那几个）──
    c.chk("默认值来自注册表：--burst = 96", empty.integer("--burst") == 96,
        empty.get("--burst"));
    c.chk("默认值来自注册表：--max-lag = 16", empty.integer("--max-lag") == 16,
        empty.get("--max-lag"));
    const Args homo(noargs, *findSubcommand("temporal-homography"));
    c.chk("--env 默认 -1（负数 = 门关；帮助文本曾写 1.0 = 误导判据口径）",
        homo.number("--env") == -1.0, homo.get("--env"));
    c.chk("--ao 默认 -1（同上，帮助文本曾写 0.5）", homo.number("--ao") == -1.0,
        homo.get("--ao"));
    const Args resid(noargs, *findSubcommand("temporal-residual"));
    c.chk("--long-feedback 默认 0.01（长窗档 = 仓库拍板值；批 13.14b 前这一档藏在 static boost 地板里）",
        resid.number("--long-feedback") == 0.01, resid.get("--long-feedback"));

    // diffstruct（批 15.2 子步 D1）的两枚阈值都是**判据的组成部分**，默认值写错 = 判据换口径。
    const Args dstruct(noargs, *findSubcommand("diffstruct"));
    c.chk("diffstruct --gate 默认 0.40（交付资产双峰 0.05/0.35 与 ≥0.74 之间的空档）",
        dstruct.number("--gate") == 0.40, dstruct.get("--gate"));
    c.chk("diffstruct --min-delta 默认 1e-6（与「PNG 字节有任何不同」逐位等量）",
        dstruct.number("--min-delta") == 1e-6, dstruct.get("--min-delta"));
    c.chk("diffstruct --expect-dark 是 Bool 且出厂关（不给 = 变暗只作报告值，参数臂对不许被硬判）",
        !dstruct.has("--expect-dark"));
    c.chk("diffstruct --a/--b 无出厂值（空串由算子拒掉，不拿空路径去读图）",
        dstruct.get("--a").empty() && dstruct.get("--b").empty());

    // ── 显式值覆盖默认值；没给的保持默认 ──
    const Args given({"--burst", "8"}, *tail);
    c.chk("显式值覆盖注册表默认", given.integer("--burst") == 8);
    c.chk("没给的 flag 仍是注册表默认", given.integer("--warm") == 64);
    c.chk("重复 flag 取首次出现（与改造前 P.get 逐字一致）",
        Args({"--burst", "7", "--burst", "9"}, *tail).integer("--burst") == 7);

    // ── rejectUnknown：两个方向都要测，否则等于没测 ──
    const Args bad({"--strenght", "0.5"}, *tail);
    c.chk("rejectUnknown 挡下拼错的 flag", throwsUsage([&] { bad.rejectUnknown(); }));
    c.chk("rejectUnknown 放行合法 flag", noThrow([&] { given.rejectUnknown(); }));
    const Args miss({"--glb"}, *findSubcommand("attribution"));
    c.chk("flag 后面漏值也要挡（否则静默用默认值）", throwsUsage([&] { miss.rejectUnknown(); }));
    const Args stray({"多出来的"}, *findSubcommand("attribution"));
    c.chk("不收位置参数的命令拒位置参数", throwsUsage([&] { stray.rejectUnknown(); }));
    const Args pos({"ssim"}, *findSubcommand("refuse"));
    c.chk("refuse 收位置参数", noThrow([&] { pos.rejectUnknown(); }));
    c.chk("位置参数可取回", pos.positional().size() == 1 && pos.positional()[0] == "ssim");

    // ── args 回显：格式与「默认值快照」同构（验收 3 的机械入口）──
    const std::string echo = echoFor(*tail, empty);
    c.chk("回显首行是命令名", echo.rfind("temporal-tail\n", 0) == 0, echo.substr(0, 24));
    c.chk("回显逐值成行 --burst = '96'", echo.find("   --burst = '96'\n") != std::string::npos,
        echo);
    const Args on({"--bistro"}, *findSubcommand("temporal-residual"));
    c.chk("Bool 未点亮不写值", echo.find("(bool) = ") == std::string::npos, echo);
    c.chk("Bool 点亮写 = true",
        echoFor(*findSubcommand("temporal-residual"), on)
                .find("--bistro(bool) = true")
            != std::string::npos);
    const auto* probe = findSubcommand("probe");
    const std::string pe = echoFor(*probe, Args(noargs, *probe));
    c.chk("probe 的 --light 既出值行也出 (bool) 行",
        pe.find("   --light = '") != std::string::npos
            && pe.find("   --light(bool)") != std::string::npos,
        pe);

    // ── args 子命令本体：未知参数不许逃到 main 外面（曾 uncaught → abort rc=134）──
    c.chk("cmdArgs 未知 flag → UsageError",
        throwsUsage([&] { cmdArgs({"temporal-tail", "--strenght", "0.5"}); }));
    c.chk("cmdArgs 未知子命令 → UsageError", throwsUsage([&] { cmdArgs({"不存在的命令"}); }));
    c.chk("cmdArgs 不带参数 → UsageError", throwsUsage([&] { cmdArgs({}); }));

    // ── -h 与表同源：每个子命令都必须出现在 -h 里（漏一条 = 帮助与代码又分家）──
    const std::string usage = usageText();
    for (const auto& s : subcommands())
        c.chk("-h 含子命令行",
            usage.find(std::string("  ") + s.name + " ") != std::string::npos, s.name);
    // 反向：-h 里不许再出现被证伪的旧默认值（它们只该留在 git history 里）
    c.chk("-h 不再写 [--env 1.0]", usage.find("[--env 1.0]") == std::string::npos);
    c.chk("-h 不再写 [--burst 64]", usage.find("[--burst 64]") == std::string::npos);
    c.chk("-h 不再写 [--max-lag 24]", usage.find("[--max-lag 24]") == std::string::npos);
    c.chk("-h 不再写 [--static-boost 0.02]", usage.find("[--static-boost 0.02]") == std::string::npos);
}

/**
 * 债-101①：调用台账 schema 的**端到端**锚点（写盘的那一行真有 secs，不是只测构造器）。
 *
 * 机位 = `RGL_IMGQA_CALLLOG` 覆写路径（judge.cpp::callLogPath）→ 临时探针文件，**不碰真台账**。
 * 哪个改动会让它红：judge.cpp 里 logCall/logCallEnvelope 任一条 `rec["secs"]`（或 rc/error/
 * argv）被删改名、守卫失效（富记录之后信封又写第二行）、写出非法 JSON、secs 计算成负数或
 * 倒序——全部红。测完必须 `clearCallLoggedForSelftest()`：守卫旗子不清，本进程 selftest
 * 自己的信封行会被刚验过的守卫吞掉（验收读数被验收工具吃掉）。
 */
void testCallLedger(Checker& c)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path probe = fs::temp_directory_path(ec) / "imgqa_calllog_probe.jsonl";
    if (ec) {  // 探针落不了地 = 整组测不到，必须**出声**（静默 return = 假绿）
        c.chk("台账探针: 临时目录可用", false, ec.message());
        return;
    }
    fs::remove(probe, ec);
    ::setenv("RGL_IMGQA_CALLLOG", probe.string().c_str(), 1);
    // ① 崩溃信封（未记录 → 写）② 再来一发信封（守卫必须吞）③ 富记录（照写）
    // ④ 富记录之后又一发信封（守卫也必须吞）。期望落盘恰两行。
    logCallEnvelope("probe-cmd", { "--flag", "1" }, 3, "boom");
    logCallEnvelope("probe-cmd", {}, 0);
    logCall("probe-cmd", { { "k", "v" } },
        { Verdict{ "probe判据", true, "observed", "探针容差", { "探针守卫" } } });
    logCallEnvelope("probe-cmd", {}, 0);
    ::unsetenv("RGL_IMGQA_CALLLOG");
    clearCallLoggedForSelftest();
    std::vector<std::string> lines;
    {
        std::ifstream in(probe.string());
        for (std::string l; std::getline(in, l);)
            if (!l.empty())
                lines.push_back(l);
    }
    fs::remove(probe, ec);
    c.chk("台账: 四次调用只落两行（崩溃信封 + 富记录，信封守卫生效）", lines.size() == 2,
        "实得 " + std::to_string(lines.size()) + " 行");
    if (lines.size() != 2)
        return;
    json e, r;
    bool parsed = true;
    try {
        e = json::parse(lines[0]);
        r = json::parse(lines[1]);
    } catch (const std::exception&) {
        parsed = false;
    }
    c.chk("台账: 两行都是合法 JSON", parsed);
    if (!parsed)
        return;
    c.chk("台账: 崩溃信封有 secs（数值 ≥ 0）与 rc/error/argv/空 verdicts",
        e.contains("secs") && e["secs"].is_number() && e["secs"].get<double>() >= 0.0
            && e["rc"] == 3 && e["error"] == "boom" && e["argv"].is_array()
            && e["argv"].size() == 2 && e["verdicts"].is_array() && e["verdicts"].empty(),
        lines[0]);
    c.chk("台账: 富记录有 secs（数值 ≥ 0，债-101① 的新字段）",
        r.contains("secs") && r["secs"].is_number() && r["secs"].get<double>() >= 0.0, lines[1]);
    c.chk("台账: 富记录旧字段齐全（ts/cmd/args/verdicts/outcome——只增不改的旧面）",
        r.contains("ts") && r.contains("cmd") && r.contains("args") && r.contains("verdicts")
            && r.contains("outcome"),
        lines[1]);
    c.chk("台账: verdicts 往返（name/ok/observed）",
        r["verdicts"].size() == 1 && r["verdicts"][0]["name"] == "probe判据"
            && r["verdicts"][0]["ok"] == true && r["verdicts"][0]["observed"] == "observed",
        lines[1]);
    c.chk("台账: secs 单调不减（先写的崩溃信封 ≤ 后写的富记录）",
        e.contains("secs") && r.contains("secs") && e["secs"].is_number() && r["secs"].is_number()
            && r["secs"].get<double>() >= e["secs"].get<double>(),
        e.value("secs", json()).dump() + " vs " + r.value("secs", json()).dump());
    // 信封判据（note 靠它把"回填到兜底记录"说出来）：判据必须**两边都验**——只验信封行为真
    // = 恒真的判据，富记录也被当成信封时 note 会对着正常记录喊 ⚠，喊几次之后就没人看了。
    c.chk("台账: 信封判据认得出信封行（判据 = 带原始 argv）", isEnvelopeRecord(lines[0]), lines[0]);
    c.chk("台账: 信封判据不把富记录当信封（有 args 无 argv → false）",
        !isEnvelopeRecord(lines[1]), lines[1]);
    c.chk("台账: 信封判据对坏行不出声（返回 false，不是抛）", !isEnvelopeRecord("{ 坏 JSON"));
}

// 调用顺序 = 失败时的打印顺序，**一字不许换**（排查记录与台账按这个顺序引用失败行）；
// 汇总行文案与退出码语义（非 0 → 1）同样不许动。
int cmdSelftest()
{
    Checker c;
    testSrgb(c);
    testObjectId(c);
    testRegion(c);
    testAnyChannelDiff(c);
    testClusters(c);
    testAnalytic(c);
    testRegistry(c);
    testJudgmentFamilies(c);
    testAntiPatterns(c);
    testTemporalDiff(c);
    testAaCoverage(c);
    testAoMetrics(c);
    testTemporalResidual(c);
    testLumaGradBands(c);
    testRejectCrossTab(c);
    testBoostFidelity(c);
    testTemporalFlicker(c);
    testYawRemapAgainstTruth(c);
    testYawRemapAnchors(c);
    testSmearPhiChain(c);
    testKeyValidation(c);
    testShadowSoftnessArms(c);
    testShadowSoftnessIzbForm(c);
    testShadowSoftnessCommentTrap(c);
    testShadowLuaSpanLastScalar(c);
    testShadowSoftnessMetrics(c);
    testShadowKernelScale(c);
    testShadowKernelScaleSampling(c);
    testShadowKernelScaleInstrument(c);
    testShadowGrain(c);
    testShadowSeam(c);
    testShadowSeamGuards(c);
    testShadowSeamSignalGates(c);
    testShadowCasterSurvival(c);
    testShadowCasterSurvivalGuards(c);
    testShadowEdgeRuler(c);
    testShadowEdgeRulerGuards(c);
    testDiffStructMetric(c);
    testDiffStructDomainGuard(c);
    testDiffBlockAnchors(c);
    testDiffBlockThinShapes(c);
    testDiffBlockRadiusSemantics(c);
    testDiffBlockErodeEquivalence(c);
    testDiffBlockErodeLaw(c);
    testDiffBlockReportSurface(c);
    testCliRegistryShape(c);
    testCliArgsBehavior(c);
    testEngineLogTail(c);
    testBurstAnchor(c);
    testAoNoiseMetric(c);
    testSwimLagTableSurface(c);
    testLuaEditBlockBoundary(c);
    testGlbChunkValidation(c);
    testPercentile100(c);
    testCallLedger(c);
    testBlendSort(c);
    testBlendLit(c);
    testBlendLitBuckets(c);
    std::printf("%s：%d 条失败\n", c.bad ? "自检失败" : "自检通过", c.bad);
    return c.bad ? 1 : 0;
}

}  // namespace RGL::imgqa
