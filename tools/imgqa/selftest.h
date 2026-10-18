#pragma once
//
// selftest.h —— `imgqa selftest` 的**跨 TU 共享层**（Checker + 两个共享断言辅助 + 47 条
// testXxx 声明）。
//
//   为什么拆：`selftest.cpp` 到 1927 代码行（工具侧上限 500）被文件护栏逼出刀。缝按**被测
//   仪器**切，一个仪器族一个 TU（先例 = `ao_noise_metric.h` / `temporal_swim.h` /
//   `shadow_kernel_metric.h`；同一个命令的整块不切两刀）：
//
//     · `selftest.cpp`             = **地基**：`image.cpp` 的图像算子（sRGB 往返 / ID 解码 /
//                                    连通域 / 任一通道差 / 分簇 / percentile100 空集守卫）+
//                                    `judge.cpp` 的判据与反模式注册表 + `cli.cpp` 的 CLI 注册表
//                                    + `cmdSelftest` 汇总路由
//     · `selftest_judgments.cpp`   = **判定口径与配置解析**：`temporal.cpp` 的 temporalDiffStat /
//                                    `makeProfile` 的键名校验 / `engine.cpp` 的 logTail 取证 /
//                                    `engine_lua_edit.cpp` 的表跨度边界（债-93）/
//                                    `material_attribution.cpp` 的 glb 长度校验（债-99）/
//                                    `analytic.cpp` 闭式解 / `aa_coverage.cpp` 的覆盖率反解
//     · `selftest_shadow.cpp`      = **阴影族两台量具**（shadow-softness / shadow-kernel-scale）：
//                                    profile 生成面（lua 覆写门）+ 度量算子面（闭式）
//     · `selftest_ao_residual.cpp` = `ao_metrics.cpp` 的度量层 + `temporal_residual.cpp` 的
//                                    时域绝对残差（折叠 / 区域纪律 / τ 闭式 / 梯度能量）
//     · `selftest_bands.cpp`       = **不读图、不启引擎的纯度量层**：bandTableOf 分带 /
//                                    rejectCrossTab 交叉表 / EMA 抑制比 / diffstruct 结构差分
//     · `selftest_motion.cpp`      = **运动态时域族**：`temporal_flicker_metric.cpp` /
//                                    `temporal_homography_metric.cpp` / `temporal_yaw_remap.cpp`
//     · `selftest_block.cpp`       = **diffblock 形态判据**（★0.5-b1）：五条阳性锚点 /
//                                    τ 语义 / erode↔distanceTransform 等价性（合成亮度图直测
//                                    口径层，不读 PNG 不启引擎）
//     · `selftest_ao_noise.cpp`    = **ao-noise 口径层**（债-90/91/96）：diffFrames 帧差三量闭式 /
//                                    printAoScanline 的「无定义不出数」纪律 / oidOf 认不出即抛 /
//                                    burstDiffFiles 帧数不足早退出声（合成小图直测）
//     · `selftest_swim.cpp`        = **taa-swim 仪器层输出面**（债-94）：lagTable 输出行数
//                                    不随实测噪声变（合成帧序列 + captureStdout 直测打印形态）
//     · `selftest_blend_sort.cpp`  = **blend-sort 口径层**（债-132）：投影锚点 / 合成闭式 /
//                                    显示链金标准 / 判别力 / 三条拒跑 / 挂号（合成 BlendPiece
//                                    直打 `blend_sort_metric.cpp`，不启引擎、不读文件）
//
//   为什么 Checker 必须住在这里：`chk` **只在失败时打印** ⟹ 全绿时 stdout 只有一句汇总，
//   这套自检**没有「逐条通过」的正向读数**。每个 TU 各抄一份 Checker 的后果不是编译不过，
//   而是 `bad` 变成九份，`cmdSelftest` 只看得见自己那一份 = 另外八份的失败**静默吞掉**。
//   共享小工具（throwsStd / captureStdout）同理：单一出处，别处不许再抄第二份。
//
//   ⚠ 断言条数 = 本文件的机械护栏：**口径必须连命令一起写死**，否则两个人数出两个数（实测踩过：
//   旧注释只写"grep -cE 数 chk 调用点"，改前用 `c\.chk\(` 数 .cpp 得 338、用 `\bchk\b` 连 .h 与
//   注释一起数得 343，护栏当场不可证伪）。**唯一口径**：
//       grep -cE 'c\.chk\(' tools/imgqa/selftest*.cpp | awk -F: '{s+=$2} END {print s}'   # = 441
//   （只数 .cpp 里的调用点，**不含** selftest.h 的成员函数定义、不含注释。）
//   🔴 改这个数字的仪式：只在**逐条可红**的断言进出时改。曾删掉 `topLevelValueOf` 的两条断言
//   ——npos 分支经 findKeyHits 不可达 ⟹ 改前改后都绿，**买了信心不买覆盖**，那种断言不配进这个数。
//   （数字怎么从 362 一路涨到今天的流水账不在这里维护，见 git log -p 与各 testXxx 的注释。）
//   丢一条断言**不会让任何东西变红**，它只会让「0 条失败」少守一件事——所以这条计数比编译更值得守。
//   ✅ 已机械化（债-123，2026-10-16）：`tools/checks/rules_text.py::imgqa_selftest_checks` 每次构建
//   三口径对账（本行 grep 行数 / 剥注释后出现次数 / 这里写死的期望），并双向对账 testXxx 的
//   声明·定义·cmdSelftest 调用三方集合——定义了没被路由 = 整条测试静默不跑，只有那条护栏抓。
//   判据与盲区 → docs/风格检查规则.md §13。
//
#include "imgqa.h"

#include <unistd.h>  // captureStdout 的 fd 换绑（dup/dup2/close）

#include <cstdio>
#include <exception>
#include <string>
#include <vector>

namespace RGL::imgqa {

/// 失败计数器（**全部 selftest TU 只有这一份**）：只在失败时打印，`bad` = 失败条数，
/// 同时是 `cmdSelftest` 的退出码依据（非 0 → 1）。
struct Checker {
    int bad = 0;
    void chk(const char* name, bool cond, const std::string& extra = "")
    {
        if (!cond) {
            std::printf("  FAIL %s %s\n", name, extra.c_str());
            ++bad;
        }
    }
};

/// 断言「这一句必须抛 `std::exception`」。与 CLI 面的 `throwsUsage`（定义在 `selftest.cpp`）
/// **不是一条**：那条只吃 `UsageError` 类型，因为 main 靠它把 rc 分成 2（你敲错了）与 3
/// （跑挂了）；这条只问「抛没抛」，用于配置门与算子的前提守卫。
///
/// 跨 TU 共享：阴影族（臂塌缩拒绝 / 尺寸守卫）、diffstruct（前提守卫）、时域残差（缺帧 /
/// 奇周期）三处都用它。原先残差段自己还抄过一个同义的 `throws` lambda，已收敛到这一份。
template <class F>
bool throwsStd(F fn)
{
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

/// 捕获 `fn` 打印到 stdout 的内容（printf 级，含中文）。输出面自检共用（单一出处，
/// 同 `throwsStd` 的理由）：`printAoScanline`（债-91「不出数」= 输出里**没有**那行）与
/// `lagTable`（债-94 输出行数稳定性）判据的都是**打印形态**，只能从 stdout 上断言。
/// 实现 = 临时文件换绑 fd（`std::printf` 走 C 层 fd 1，C++ 流重定向管不到它）；
/// `fn` 抛异常也先把 stdout 换回来再传播——闸门自己不许把 stdout 弄丢。
template <class F>
std::string captureStdout(F fn)
{
    std::fflush(stdout);
    const int saved = ::dup(::fileno(stdout));
    std::FILE* tmp = saved < 0 ? nullptr : std::tmpfile();
    if (tmp == nullptr) {  // 兜底：环境造不出临时文件就照常执行不出捕获（断言会因空串红）
        fn();
        return {};
    }
    ::dup2(::fileno(tmp), ::fileno(stdout));
    std::exception_ptr err = nullptr;
    try {
        fn();
    } catch (...) {
        err = std::current_exception();
    }
    std::fflush(stdout);
    ::dup2(saved, ::fileno(stdout));
    ::close(saved);
    if (err)
        std::rethrow_exception(err);
    std::rewind(tmp);
    std::string out;
    char buf[4096];
    for (size_t n = 0; (n = std::fread(buf, 1, sizeof buf, tmp)) > 0;)
        out.append(buf, n);
    std::fclose(tmp);
    return out;
}

// ── 47 条测试的声明（定义按仪器族分散在十个 TU；调用顺序只写在 `cmdSelftest`）──
void testSrgb(Checker& c);
void testObjectId(Checker& c);
void testRegion(Checker& c);
void testAnyChannelDiff(Checker& c);
void testClusters(Checker& c);
void testAnalytic(Checker& c);
void testRegistry(Checker& c);
void testJudgmentFamilies(Checker& c);
void testAntiPatterns(Checker& c);
void testTemporalDiff(Checker& c);
void testAaCoverage(Checker& c);
void testAoMetrics(Checker& c);
void testTemporalResidual(Checker& c);
void testLumaGradBands(Checker& c);
void testRejectCrossTab(Checker& c);
void testBoostFidelity(Checker& c);
void testTemporalFlicker(Checker& c);
void testYawRemapAgainstTruth(Checker& c);
void testYawRemapAnchors(Checker& c);
void testSmearPhiChain(Checker& c);
void testKeyValidation(Checker& c);
void testShadowSoftnessArms(Checker& c);
void testShadowSoftnessIzbForm(Checker& c);
void testShadowSoftnessCommentTrap(Checker& c);
void testShadowSoftnessMetrics(Checker& c);
void testShadowKernelScale(Checker& c);
void testShadowKernelScaleSampling(Checker& c);
void testShadowKernelScaleInstrument(Checker& c);
void testDiffStructMetric(Checker& c);
void testDiffStructDomainGuard(Checker& c);
void testDiffBlockAnchors(Checker& c);
void testDiffBlockThinShapes(Checker& c);
void testDiffBlockRadiusSemantics(Checker& c);
void testDiffBlockErodeEquivalence(Checker& c);
void testDiffBlockErodeLaw(Checker& c);
void testDiffBlockReportSurface(Checker& c);
void testCliRegistryShape(Checker& c);
void testCliArgsBehavior(Checker& c);
void testEngineLogTail(Checker& c);
void testBurstAnchor(Checker& c);
void testAoNoiseMetric(Checker& c);
void testSwimLagTableSurface(Checker& c);
void testLuaEditBlockBoundary(Checker& c);
void testGlbChunkValidation(Checker& c);
void testPercentile100(Checker& c);
void testCallLedger(Checker& c);
void testBlendSort(Checker& c);

}  // namespace RGL::imgqa
