// registry_read.cpp —— 判据/反模式**注册表**的两条读命令（从 main.cpp 拆出）。
//
// ## 缝在哪
//
// 缝 = **只读注册表的命令**这一族。`check` 读 `registeredJudgments()`（判据契约表，本体在
// judgments_table*.cpp），`refuse` 读 `antiPatterns()`（反模式登记，本体在 judge.cpp）——两条
// 命令都不碰引擎、不碰像素，输出**逐字节**由注册表决定（`verify.sh noengine` 那条逐字节对账
// 正是打在这两条上）。它们与"仪器"（要引擎采集的判据）不是同一条改动缝：加一条判据只动
// judgments_table*.cpp，加一种打印分组只动这里。
//
// 搬动的纪律：两个命令的函数体与文案从 main.cpp **整块照搬**，一个字节未改。
// `printAntiFields` 是 refuse 专属的 static helper（列表与单条共用同一份打印，免得格式漂移），
// 跟着 refuse 走，**只此一份**。
#include "imgqa.h"

#include <algorithm>
#include <cstdio>
#include <string>

namespace RGL::imgqa {

int cmdCheck()
{
    // 判据一律 advisory：这里打印的两样（容差理由 + guard 出处）**就是**它的可信度凭据，
    // 每条都有、由 imgqa selftest 强制（晋升机制已删，理由 → docs/还债总账.md 债-08）。
    //
    // **按族分组，不留扁平输出**：族 = 成立条件的分组。静止机位族与运动族的比值口径不同
    // （前者要求平均视点恒定），扁平清单正好把"这条判据能不能和那条比数"这件事藏起来。
    const auto& all = registeredJudgments();
    std::printf("判据注册表（共 %zu 条，一律 advisory，按族分组）：\n", all.size());
    for (const auto family : {JudgmentFamily::Foundation, JudgmentFamily::TemporalStatic,
                            JudgmentFamily::TemporalMotion, JudgmentFamily::Attachment}) {
        size_t count = 0;
        for (const auto& j : all)
            count += j.family == family;
        std::printf("\n══ %s（%zu 条）\n   成立条件：%s\n\n", judgmentFamilyTitle(family), count,
            judgmentFamilyCondition(family));
        for (const auto& j : all) {
            if (j.family != family)
                continue;
            std::printf("  %s\n", j.name);
            std::printf("             容差理由：%s\n", j.whyTol);
            for (const auto& g : j.guards)
                std::printf("             钉住：%s\n", g.c_str());
        }
    }
    std::printf("\n（本命令只列注册表；实际求值需要引擎，见 attribution / faces / switch）\n");
    return 0;
}

/// 一条反模式的三格（作用域 / 为什么被否 / 未被否的部分）。列表与单条共用同一份打印，
/// 免得两处格式漂移。**三格都要打**：只打 why 就还是「一段散文」，而作用域与未被否的部分
/// 正是 `screen-space-l1-frame-diff` 付过两天学费才要治的那两格。
static void printAntiFields(const AntiPatternInfo& a)
{
    std::printf("        作用域：%s\n        为什么被否：%s\n        未被否（仍然要用）：%s\n",
        antiScopeName(a.scope), a.why, a.stillOpen);
}

int cmdRefuse(const std::string& name)
{
    if (name.empty()) {
        std::printf("已登记的反模式（都会导致错误结论，故工具**主动拒绝**）。"
                    "「未被否」那格是红线之外的部分——别连带不做：\n\n");
        for (const auto& e : antiPatterns()) {
            std::printf("  %s\n", e.second.name);
            printAntiFields(e.second);
            std::printf("\n");
        }
        return 0;
    }
    std::string key = name;
    std::replace(key.begin(), key.end(), '_', '-');
    const auto& ap = antiPatterns();
    const auto it = ap.find(key);
    if (it == ap.end()) {
        std::printf("未登记的反模式：%s\n已登记：", name.c_str());
        for (const auto& e : ap)
            std::printf("%s ", e.first.c_str());
        std::printf("\n");
        return 2;
    }
    std::printf("✗ 反模式 `%s` 被拒绝：\n", key.c_str());
    printAntiFields(it->second);
    return 2;
}

}  // namespace RGL::imgqa
