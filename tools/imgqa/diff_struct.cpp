// imgqa diffstruct —— 结构差分统计（Phase 15 批 15.2 子步 D1：验收工具化）。
//
// 把 15.2 判据（拍板 → docs/phase15_plan.md §2 批次表 15.2 行）从一次性脚本
// /tmp/opencode/ssr_subtract_check.py 升成常驻算子；该脚本自此作废。
//
// **场景无关是设计约束**：bistro 没有 manifest（`imgqa switch` 等靶算子是 manifest/glb
// 机位驱动，loadManifest 只认 build/rgl/models/*.manifest.json）⟹ 本算子的输入 =
// 磁盘上两张 PNG，**不启动引擎、不读 manifest**。A/B 两档怎么来的（just shot --ssr
// 开/关、参数臂、任意机位）不归本算子管——它只回答「这两张图的差长什么结构」。
//
// 判据语义（算子本体与全部口径对账 → diff_struct_metric.h）：
//   · 报告值：画面尺寸、差分像素数与占比、Δ<0/Δ>0 计数、变暗 Δ 最小值、
//     |Δ| p10/p50/p90 与 p90/p10（结构对比度）
//   · 硬判据一（--gbuf 在场）：变暗 ∧ roughness ≥ gate 计数 **非零即 FAIL**
//   · 硬判据二（--expect-dark）：Δ<0 数 > 0（SSR 开/关对用；参数臂对不带它 = 纯报告）
// 比较域 = 线性亮度（sRGB 解码后 Rec.709，`prepareLinear` 单点）——文件头硬约束第 2 条。
#include "diff_struct_metric.h"
#include "imgqa.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {
namespace {

/// 定点短格式（与一次性脚本的 %.6f 读数对账用；inf 显式写 inf，别打 1.#inf）。
std::string dsNum(double v, int prec = 6)
{
    char buf[40];
    if (v == std::numeric_limits<double>::infinity())
        return "inf";
    std::snprintf(buf, sizeof buf, "%.*f", prec, v);
    return buf;
}

/// PNG → 线性亮度图（比较域单点 = prepareLinear 的 luma，与所有时域判据同一条路）。
cv::Mat linearLuma(const std::string& path)
{
    Frame f;
    f.bgr = readPngBgr(path);
    prepareLinear(f);
    return f.luma;
}

/// dump_gbuf=3 出图（metalrough）→ .g 通道归一 roughness（UNORM8 → g/255）。
/// OpenCV BGR 序：.g = ch[1]。尺寸与 A/B 不符 = 采集错帧/错档，抛。
cv::Mat roughnessFromGbuf(const cv::Mat& gbufBgr, const cv::Size& extent)
{
    if (gbufBgr.size() != extent)
        throw std::runtime_error("gbuf 与 A/B 尺寸不符: " + std::to_string(gbufBgr.cols)
            + "x" + std::to_string(gbufBgr.rows) + " vs "
            + std::to_string(extent.width) + "x" + std::to_string(extent.height)
            + "（dump_gbuf 必须与差分对同 extent 采集）");
    cv::Mat ch[3];
    cv::split(gbufBgr, ch);
    cv::Mat rough;
    ch[1].convertTo(rough, CV_64FC1);
    rough /= 255.0;
    return rough;
}

/// 报告值（行式与一次性脚本逐项对齐，对拍靠它）。**只报不判**——两条硬判据在调用侧，
/// 判据的容差理由一律来自注册表（`imgqa check`），不在打印里现写。
void printDiffStructReport(const DiffStructStat& st, const std::string& aPath,
    const std::string& bPath, double gate, double minDelta)
{
    std::printf("画面 %d x %d = %lld px\n", st.width, st.height,
        static_cast<long long>(st.total));
    std::printf("  A(实验档) = %s\n  B(基线档) = %s\n", aPath.c_str(), bPath.c_str());
    std::printf("① 差分像素（线性亮度 |Δ|>%s）: %lld = %.2f%%\n", dsNum(minDelta).c_str(),
        static_cast<long long>(st.diff), 100.0 * st.diff / std::max<int64_t>(1, st.total));
    std::printf("② 变暗（Δ<0，线性亮度域）   : %lld（占差分 %.2f%%，占画面 %.3f%%）\n",
        static_cast<long long>(st.darker),
        100.0 * st.darker / std::max<int64_t>(1, st.diff),
        100.0 * st.darker / std::max<int64_t>(1, st.total));
    std::printf("② 变亮（Δ>0）               : %lld；变暗像素 Δ 最小值 = %s\n",
        static_cast<long long>(st.brighter), dsNum(st.minDarkDelta).c_str());
    if (st.diff > 0) {
        std::printf("|Δ| 分位（差分集合）p10/p50/p90 = %s / %s / %s；p90/p10 = %s%s\n",
            dsNum(st.p10).c_str(), dsNum(st.p50).c_str(), dsNum(st.p90).c_str(),
            dsNum(st.ratio).c_str(),
            st.ratioFinite ? "" : "（p10=0 ⟹ 比值无定义，标注不当 0 用）");
    } else {
        std::printf("|Δ| 分位：差分集合为空，p10/p50/p90 与比值无定义（不是 0）\n");
    }
    if (!st.hasRough) {
        // 命中率没有 roughness 通道就没有分母——**报「需 --gbuf」而不是 0**（0 会被读成
        // 「一个都没命中」，缺输入与读数是两回事，同 ratio 的 NaN 纪律）。
        std::printf("④ 命中率（|Δ|>ε ∧ 过门控 / 过门控）: 需 --gbuf（过门控分母要 roughness 通道）\n");
        return;
    }
    std::printf("③ 变暗 ∧ roughness≥%.2f      : %lld（判据：必须 = 0）\n", gate,
        static_cast<long long>(st.darkOutsideGate));
    if (st.roughDistValid) {
        std::printf("③ 变暗像素 roughness(g/255) 分布: min=%s p50=%s max=%s\n",
            dsNum(st.roughMin, 3).c_str(), dsNum(st.roughP50, 3).c_str(),
            dsNum(st.roughMax, 3).c_str());
    }
    std::printf("参考：过门控（0<roughness<%.2f，g=0 天空除外）像素 = %lld = %.2f%%\n", gate,
        static_cast<long long>(st.gatePixels),
        100.0 * st.gatePixels / std::max<int64_t>(1, st.total));
    // ④ 命中率（§3.2 未还账⑤：此前手算 = dump_gbuf=3 + 开关差分两个已有产物相除）。
    // 分母 0 = 无定义，报「无定义」不报 0%（同 p90/p10 的 NaN 纪律）。
    if (st.gatePixels > 0) {
        std::printf("④ 命中率（|Δ|>ε ∧ 过门控 / 过门控）: %lld / %lld = %.2f%%\n",
            static_cast<long long>(st.gateDiff), static_cast<long long>(st.gatePixels),
            100.0 * st.hitRate);
    } else {
        std::printf("④ 命中率（|Δ|>ε ∧ 过门控 / 过门控）: 无定义（过门控分母 = 0，不是 0%%）\n");
    }
}

/// 两条硬判据（其余全是报告值），容差理由一律从注册表取（`imgqa check` 打印的就是它们）。
/// 两条都**只在对应输入在场时**才存在：`--gbuf` 不在场就没有域可判，`--expect-dark` 不给
/// 就只是报告——"没判"与"判过且绿"必须在返回值里可区分（返回条数就是那个区分）。
std::vector<Verdict> diffStructVerdicts(const DiffStructStat& st, double gate, bool expectDark)
{
    std::vector<Verdict> verdicts;
    if (st.hasRough) {
        verdicts.push_back(verdictFor("diffstruct-dark-domain", st.darkOutsideGate == 0,
            "变暗 ∧ roughness≥" + dsNum(gate, 2) + " = " + std::to_string(st.darkOutsideGate)
                + " px（要求恰 0）；变暗 " + std::to_string(st.darker) + " px / 过门控 "
                + std::to_string(st.gatePixels) + " px"));
    }
    if (expectDark) {
        // 空差分集合的比值是 NaN（无定义），p10 = 0 才是 inf。两者都不可当数用，但**混写成
        // 同一个 "inf"** 会把「什么都没差」与「差全挤在一个量级上」读成同一件事。
        const std::string ratioText = st.diff == 0
            ? std::string("无定义（差分集合为空）")
            : (st.ratioFinite ? dsNum(st.ratio) : "inf（p10=0）");
        verdicts.push_back(verdictFor("diffstruct-dark-anchor", st.darker > 0,
            "Δ<0（变暗）= " + std::to_string(st.darker) + " px（要求 > 0）；差分 "
                + std::to_string(st.diff) + " px，p90/p10 = " + ratioText));
    }
    return verdicts;
}

}  // namespace

int cmdDiffStruct(const std::string& aPath, const std::string& bPath,
    const std::string& gbufPath, double gate, bool expectDark, double minDelta)
{
    if (aPath.empty() || bPath.empty())
        throw std::runtime_error(
            "diffstruct 需要 --a 与 --b（磁盘上两张 PNG；A = 实验档，B = 基线档）");
    const cv::Mat lumaA = linearLuma(aPath);
    const cv::Mat lumaB = linearLuma(bPath);
    if (lumaA.size() != lumaB.size())
        throw std::runtime_error("A/B 尺寸不符: " + std::to_string(lumaA.cols) + "x"
            + std::to_string(lumaA.rows) + " vs " + std::to_string(lumaB.cols) + "x"
            + std::to_string(lumaB.rows) + "（两图必须同 extent，否则像素对不上）");
    cv::Mat rough;
    if (!gbufPath.empty())
        rough = roughnessFromGbuf(readPngBgr(gbufPath), lumaA.size());

    const DiffStructStat st = diffStructStat(lumaA, lumaB, rough, gate, minDelta);
    printDiffStructReport(st, aPath, bPath, gate, minDelta);
    if (!expectDark) {
        std::printf(
            "（--expect-dark 未给 ⟹ 变暗计数只作报告值——参数臂对用这个口径；"
            "SSR 开/关对要加该旗标升成硬判据）\n");
    }
    const std::vector<Verdict> verdicts = diffStructVerdicts(st, gate, expectDark);
    std::printf("\n");
    for (const auto& v : verdicts)
        std::cout << "  " << v.line() << "\n";

    logCall("diffstruct", {{"a", aPath}, {"b", bPath}, {"gbuf", gbufPath},
                              {"gate", dsNum(gate, 2)}, {"min_delta", dsNum(minDelta)},
                              {"expect_dark", expectDark ? "1" : "0"}},
        verdicts);
    bool allOk = true;
    for (const auto& v : verdicts)
        allOk = allOk && v.ok;
    return allOk ? 0 : 1;
}

}  // namespace RGL::imgqa
