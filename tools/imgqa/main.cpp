// imgqa CLI。子命令：selftest / check / refuse / args / attribution / faces / switch / izb / swim /
// probe / temporal / temporal-residual / temporal-motion / temporal-feedback /
// temporal-flicker / temporal-homography / aa-coverage / taa-swim / shadow-softness /
// shadow-kernel-scale / shadow-grain / blend-sort / diffstruct / diffblock / note。
// （权威清单 = `cli_table.cpp` 的注册表，本行只是路标。）
//
// ## 本文件 = **路由**，不是仪器
//
// 这里只有：`main`（解析 → 校验 → 分发）与三条分派链（dispatch / dispatchRig / dispatchTemporal，
// 路由表顺序与 notWired 语义 = 契约，`imgqa -h` 与 `imgqa args` 逐字节对账打在它上面）。
// **判据命令一律住在自己的 TU**（一个仪器一个 TU 的既有先例：
// aa_coverage.cpp / shadow_softness.cpp）：
//
//   · rig_attribution.cpp  attribution / faces / switch —— 吃 manifest 区域归因的三条判据
//   · rig_shadow_field.cpp izb / swim —— izb_rig 影场仪器族（同一套 deferred+shadowPass 采集）
//   · probe.cpp            probe —— 闭式解探针（消费冻结配置目录，不吃 --glb）
//   · blend_sort.cpp       blend-sort —— 透明合成次序（同样消费冻结配置目录 + manifest 的
//                          closedForm 块；口径在 blend_sort_metric.cpp，selftest 直接打）
//   · registry_read.cpp    check / refuse —— 只读注册表的命令
//
// 缝按**仪器**切，不按行数切（护栏：单文件 < 500 代码行）。命令函数的**声明集中在 imgqa.h**
// （本仓惯例：实现分散、声明集中，不另设跨 TU 声明头）。加一条判据命令 = 新 TU + cli_table.cpp
// 的注册表 + 本文件分派链里的一行 + imgqa.h 一句声明，四处一起改（漏第三处 = notWired 那条红）。
#include "imgqa.h"
#include "cli.h"
#include "temporal_flicker.h"
#include "temporal_homography.h"

#include <array>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace RGL::imgqa {

namespace {

/// 注册表里有这条命令、三条分派链却都没接 = 表与 dispatch 分家（两处必须一起改）。
/// 正常走不到：main 先查过 findSubcommand。
int notWired(const std::string& cmd)
{
    std::printf("子命令 %s 在注册表里但没接线\n", cmd.c_str());
    return 2;
}

/// 机位几何类的名字表（dispatch 靠它选链，与 dispatchRig 一一对应）。
bool isRigCommand(const std::string& cmd)
{
    static const std::set<std::string> kRig{"attribution", "faces", "switch", "izb", "swim",
        "probe", "ao-noise"};
    return kRig.contains(cmd);
}

/**
 * `temporal-tail` 的 spin / camera / step 分派（批 13.14a / 批 13.15）。
 *
 * 抽成独立函数的理由有二：① `dispatchTemporal` 的认知复杂度已到上限；② 更要紧的是
 * **拼错 `--motion` 必须报错**，不能静默回落 spin——否则「以为跑了机位运动、实际拿到
 * 静止机位的数」这种错误完全没有暴露途径（两个模式的读数都长得很像）。
 */
int dispatchTemporalTail(const Args& P)
{
    const std::string motion = P.get("--motion");
    if (motion != "camera" && motion != "spin" && motion != "step") {
        std::fprintf(stderr, "--motion 只认 camera / spin / step（收到 %s）\n", motion.c_str());
        return 2;
    }
    if (motion == "step")
        return cmdTemporalTailStep(P.get("--camera"), P.number("--step"), P.integer("--burst"),
            P.integer("--warm"), P.number("--tau"), P.integer("--margin"), P.has("--fsr"));
    if (motion == "camera")
        return cmdTemporalTailCamera(P.get("--camera"), P.number("--angle"),
            P.number("--gate-angle"), P.integer("--burst"), P.integer("--warm"),
            P.number("--tau"), P.integer("--margin"));
    return cmdTemporalTail(P.get("--camera"), P.number("--angle"), P.integer("--burst"),
        P.integer("--warm"), P.integer("--max-lag"), P.number("--tau"), P.integer("--margin"));
}

/// temporal-* 家族（7 条）——按前缀转出去，见 dispatch。
int dispatchTemporal(const std::string& cmd, const Args& P)
{
    if (cmd == "temporal")
        return cmdTemporal(P.integer("--pairs"), P.integer("--warm"), P.has("--base-space"));
    if (cmd == "temporal-residual")
        return cmdTemporalResidual(P.integer("--periods"), P.number("--warm-multiplier"),
            P.number("--slow-feedback"), P.has("--skip-slow"),
            P.has("--base-space"), P.has("--long-feedback"), P.has("--bistro"), P.get("--camera"),
            P.number("--long-feedback"), P.has("--diag-bs-off"), P.has("--reject"),
            P.has("--clip-abl"), P.number("--clip"), P.number("--clip-soft"));
    if (cmd == "temporal-motion")
        return cmdTemporalMotion(P.get("--camera"), P.get("--motion"), P.number("--angle"),
            P.integer("--burst"), P.integer("--warm"), P.integer("--warm-still"),
            P.number("--edge-pct"), P.number("--long-feedback"), P.number("--clip"),
            P.number("--clip-soft"));
    if (cmd == "temporal-tail")
        return dispatchTemporalTail(P);
    if (cmd == "temporal-feedback") {
        std::vector<double> fbs;
        std::stringstream fs(P.get("--fb"));
        for (std::string t; std::getline(fs, t, ',');)
            if (!t.empty())
                fbs.push_back(std::stod(t));
        return cmdTemporalFeedback(P.get("--camera"), P.number("--angle"),
            P.integer("--burst"), P.number("--edge-pct"), fbs, P.number("--kernel"));
    }
    if (cmd == "temporal-flicker")
        return cmdTemporalFlicker(P.get("--target"), P.number("--sweep"),
            P.integer("--frames"), P.integer("--halfwidth"), P.number("--rowfrac"),
            P.has("--verbose"), P.number("--kernel"), P.has("--fsr"));
    // A/B 侧：负数 = 不覆写 = 交付配置原样（默认）。三个口径对拍才能把「移动闪烁是
    // IBL 反射 / AO / 几何边 哪个驱动的」这个问题拆开。
    if (cmd == "temporal-homography")
        return cmdTemporalHomography(P.get("--target"), P.number("--sweep"),
            P.integer("--frames"), P.get("--out"), P.has("--bands"), P.number("--env"),
            P.number("--ao"));
    return notWired(cmd);
}

/// 吃 --glb/--camera 的机位几何类（7 条）——参数最长、改判据时最常动。
int dispatchRig(const std::string& cmd, const Args& P)
{
    if (cmd == "attribution")
        return cmdAttribution(P.get("--glb"), P.get("--camera"));
    if (cmd == "faces")
        return cmdFaces(P.get("--glb"), P.get("--piece"), P.has("--shadow"), P.has("--env"));
    if (cmd == "switch")
        return cmdSwitch(P.get("--glb"), P.get("--camera"), P.get("--allow-piece"),
            P.has("--env"));
    if (cmd == "izb")
        return cmdIzb(P.get("--glb"), P.get("--camera"));
    if (cmd == "swim")
        return cmdSwim(P.get("--glb"), P.get("--camera"), P.number("--dx"), P.has("--izb"),
            P.has("--evssm"));
    if (cmd == "probe") {
        const auto parse3 = [](const std::string& s) {
            std::array<double, 3> v{0, 0, 0};
            std::stringstream ss(s);
            std::string tok;
            for (size_t i = 0; i < 3 && std::getline(ss, tok, ','); ++i)
                v[i] = std::stod(tok);
            return v;
        };
        // --light = **指向光源**的方向 L（不是 scene.lua 的 direction）。默认值
        // 与 qa/configs/analytic 的 direction = (0.642788,0,-0.766044) 配对。
        const auto a = parse3(P.get("--albedo"));
        const auto lv = parse3(P.get("--light"));
        return cmdProbe(projectRoot() + "/" + P.get("--config"), P.integer("--x"),
            P.integer("--y"), {a[0], a[1], a[2]}, P.number("--rough"), P.number("--metal"),
            {lv[0], lv[1], lv[2]}, P.number("--intensity"), P.integer("--tol"),
            P.has("--light") || P.has("--intensity"));
    }
    if (cmd == "ao-noise")
        return cmdAoNoise(P.get("--glb"), P.get("--camera"),
            P.number("--strength"), P.integer("--radius"), P.integer("--pairs"),
            !P.has("--no-temporal"), P.has("--bistro"));
    return notWired(cmd);
}

/// 子命令分发。**这里不许出现默认值字面量**——值一律来自注册表（cli_table.cpp），
/// 于是 -h 文本、`imgqa args` 回显与本函数不可能再各说各话。
///
/// 分派按家族拆三条链（temporal-* 走前缀、机位几何类走名字表、其余留在本函数）：
/// 21 条 if 挤一个函数 = 认知复杂度 33（error 级），拆开每条都在护栏内，
/// 而「一条命令一行」的形态没变。
int dispatch(const std::string& cmd, const Args& P)
{
    if (cmd.rfind("temporal", 0) == 0)
        return dispatchTemporal(cmd, P);
    if (isRigCommand(cmd))
        return dispatchRig(cmd, P);
    // 元命令 selftest / check / shadow-softness / shadow-kernel-scale 同形（零 flag、零
    // 参数）⟹ 合成一条查表。不是审美：本函数的认知复杂度已到护栏上限（15），加第 8 条子命令
    // （shadow-softness）就出界；查表把多条 if 收成一条，形态与上面「temporal 前缀」
    // 「isRigCommand 名字表」两条同构。
    // ⚠ 进这张表的前提 = **真的零 flag**。判据的阈值是量具的组成部分（写死在实现里的
    // constexpr），可调 flag 会让同一条判据在不同调用下给出不同的绿，那就不许走这张表。
    static const std::map<std::string, int (*)()> kMeta = {
        { "selftest", cmdSelftest },
        { "check", cmdCheck },
        { "shadow-softness", cmdShadowSoftness },
        { "shadow-kernel-scale", cmdShadowKernelScale },
        { "shadow-grain", cmdShadowGrain },
        { "blend-sort", cmdBlendSort },
    };
    if (const auto meta = kMeta.find(cmd); meta != kMeta.end())
        return meta->second();
    if (cmd == "refuse") {
        const auto pos = P.positional();
        return cmdRefuse(pos.empty() ? "" : pos[0]);
    }
    // 「读参数 → 一句转发」的算子同形 ⟹ 合成查表，理由与上面 kMeta 同一条：本函数的认知复杂度
    // 已到护栏上限（15），逐条加 if 会出界（第 8 条子命令就付过一次账）。
    // ⚠ 进这张表的前提 = 函数体**只有一句转发**；要判分支的（refuse / note 收位置参数）留在 if 里。
    // lambda 无捕获 ⟹ 转函数指针，与 kMeta 同型；`Args` 的取值接口全是 const，故按 const& 传。
    static const std::map<std::string, int (*)(const Args&)> kFlagCmd = {
        { "aa-coverage", +[](const Args& a) {
             return cmdAaCoverage(a.integer("--warm"), a.number("--feedback"),
                 a.has("--base-space"), a.number("--kernel"));
         } },
        { "taa-swim", +[](const Args& a) {
             return cmdTaaSwim(a.integer("--pairs"), a.integer("--warm"));
         } },
        { "diffstruct", +[](const Args& a) {
             return cmdDiffStruct(a.get("--a"), a.get("--b"), a.get("--gbuf"), a.number("--gate"),
                 a.has("--expect-dark"), a.number("--min-delta"));
         } },
        { "diffblock", +[](const Args& a) {
             return cmdDiffBlock(a.get("--a"), a.get("--b"), a.integer("--r"), a.number("--tau"),
                 a.integer("--top"), a.get("--json"));
         } },
    };
    if (const auto fn = kFlagCmd.find(cmd); fn != kFlagCmd.end())
        return fn->second(P);
    if (cmd == "note") {
        std::string text;
        for (const auto& p : P.positional())
            text += (text.empty() ? "" : " ") + p;
        return noteLastCall(text);
    }
    return notWired(cmd);
}
}  // namespace

}  // namespace RGL::imgqa

int main(int argc, char** argv)
{
    using namespace RGL::imgqa;
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        // 带子命令名 = 只看那一条（含每个 flag 的一行说明）；不带 = 全表。
        const SubcommandSpec* one = args.size() > 1 ? findSubcommand(args[1]) : nullptr;
        std::fputs(one != nullptr ? detailFor(*one).c_str() : usageText().c_str(), stdout);
        return 0;
    }
    const std::string cmd = args[0];
    const std::vector<std::string> rest(args.begin() + 1, args.end());
    try {
        // args 子命令按**目标子命令**的 spec 解析，必须先于外层 Args 分发——否则拿 args
        // 自己的 spec 去校验目标命令的 flag，必然报未知参数。
        if (cmd == "args")
            return cmdArgs(rest);
        const SubcommandSpec* spec = findSubcommand(cmd);
        if (spec == nullptr) {
            std::printf("未知子命令 %s（-h 看用法）\n", cmd.c_str());
            return 2;
        }
        const Args P(rest, *spec);
        P.rejectUnknown();  // 拼错的 flag 不许被静默忽略（那等于在默认值上跑判据）
        // 债-101①：耗时起点 = 进 dispatch 前（解析已结束，命令从这里起才**真正干活**）。
        // UsageError 只可能由解析层抛出（cli.cpp）⟹ 走到 catch(std::exception) 的必然是
        // 「已经干过活」的命令，崩溃记录天然有 secs 可写。
        markCallStart();
        const int rc = dispatch(cmd, P);
        // 未自记命令的兜底信封（logCallEnvelope 内部认 logCall 写过的富记录，不重复落笔）。
        // `note` 排除：它**回填最后一行**，信封再追一行会把下一次 note 的目标偷走。
        if (cmd != "note")
            logCallEnvelope(cmd, rest, rc);
        return rc;
    } catch (const UsageError& e) {
        std::fprintf(stderr, "错误: %s\n", e.what());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误: %s\n", e.what());
        // 崩溃也进台账（债-101①：崩掉的那次最想知道多久；已写过富记录则不动作）。
        logCallEnvelope(cmd, rest, 3, e.what());
        return 3;
    }
}
