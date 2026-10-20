// shadow_edge_ruler_judge.cpp —— `shadow-edge-ruler` 的**判决线层**（缝 = 出表 vs 判决，
// 照 shadow_grain_judge / shadow_cascade_seam_judge 的先例）。
//
// 2026-10-20 语义改版（两处主 agent 规格错误修正的判决侧，全文住 ruler 头注 E1/E2）：
// 三条线**全部 INFO 只报数不判**（不进 rc）；裁决面 = 五道闸门 + 缝宽毫米数本身。
//   J1 `shadow-edge-absolute-error`：每板每级 |误差| 与**本级纹素尺寸**并排——这张表就是
//     定 K 的推导材料。🔴 K 本批不定（TODO）：仪器今天第一次出数，拿一个没推导过的数当门
//     = 把「实测没超所以钉个数」写进判据网（survival J1 先例）。界该由什么推导：正常态 =
//     全量几何进阴影图时影边的量化噪声上界（≈ 0.5 纹素吸附 + PCF 半核，闸门 4 同款）；
//     缺陷态 = 本批主表的偏移分布。定界取法照 seam J1：固有下界**推导**，读数只核对中段。
//   J2 `shadow-edge-seam-width`：🔴 **本靶头条交付数** = 缝宽毫米（seam(c↔c+1) =
//     |误差(级c) − 误差(级c+1)|，逐板 + 每交界中位/最大）。= 债-139「交界带混合该开多宽」
//     的设计输入——本会话从头到尾想要而拿不到的就是它。N 未定 ⟹ 只报数。
//     两种读法并出：钉级臂差（主读法）与自然曲线台阶（跨级相邻板）。🔴 首跑实测两读法
//     **不逐数相等**（0.43mm vs 8–16mm）且原因正当：自然台阶跨两枚板求差，混进板间差；
//     缝是 (板, 交界) 二元性质（同交界 t100=0 / t200=16.6mm）——台阶只当形态参考，
//     读数以钉级为主。旧「应当一致」是假前提，2026-10-20 实测改掉。
//   J3 `shadow-edge-cascade-agreement`：seam / max(两纹素) 的无量纲比值；N 的推导两头
//     住注册表。原「误差随 D 单调」（shadow-edge-monotone）**退役**：本靶上 D 逐位空操作
//     （六臂 md5 全等，主 agent 复算），「随 D」这个自变量在靶上不存在——继续判它 =
//     判一个前提不存在的命题，恒真是假绿。
//
// ── 变异红线（哪个变异让我红；实跑记录见交付说明）───────────────────────────────────
//   · seamWidths 的 `|a − b|` 改成带符号 `a − b` → selftest「反号同缝宽应同读数」红
//     （先取绝对值再比较与先比较再取绝对值**不同**，符号约定不该进缝宽定义）。
//   · seamWidths 的 NaN 传播改成当 0 → selftest「任一侧不可量应给 NaN 不猜」红
//     （把「没量到」读成「缝为 0」= 缺陷隐身）。
//   · seamWidths 空板清单守卫删掉 → selftest「空清单应抛」红（对空集合恒真 = 假绿）。
//   · 某交界无任何可量板被静默跳过 → selftest「全 NaN 交界应抛」红。
//   · J1/J2/J3 的 INFO 打印改成产 Verdict → 「未定期不进 rc」的契约破了（挂号处写着这条）。
#include "shadow_edge_ruler.h"
#include "imgqa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {

std::string edgeMm(double v)
{
    if (std::isnan(v))
        return "—";
    std::ostringstream o;
    o.precision(2);
    o << std::fixed << v;
    return o.str();
}

std::string edgePct(double v)
{
    if (std::isnan(v))
        return "—";
    std::ostringstream o;
    o.precision(3);
    o << std::fixed << (v * 100.0) << "%";
    return o.str();
}

std::vector<std::vector<double>> seamWidths(const EdgeReadout& r)
{
    if (r.plateNames.empty())
        throw std::runtime_error("内部错误：seamWidths 拿到的板清单为空（对空集合的"
              "「每交界缝宽都达标」恒真 = 假绿）");
    if (r.errPinned.size() != 4 || r.errPinned[0].empty())
        throw std::runtime_error("内部错误：seamWidths 拿到的钉级读数形状不对（4 级 × "
              "2 档 D，实得 " + std::to_string(r.errPinned.size()) + " 级）");
    // 出厂 D=5 列（下标 0）；D=-1 列是闸门 4 的参照态，不是另一条缝。
    std::vector<std::vector<double>> seams(3, std::vector<double>(r.plateNames.size()));
    for (int c = 0; c < 3; ++c)
        for (size_t p = 0; p < r.plateNames.size(); ++p) {
            const double a = r.errPinned[c][0][p];
            const double b = r.errPinned[c + 1][0][p];
            // 🔴 先各自减、后取绝对值；任一侧 NaN ⟹ NaN（「没量到」不许读成「缝为 0」）。
            seams[c][p] = std::isnan(a) || std::isnan(b)
                              ? std::numeric_limits<double>::quiet_NaN()
                              : std::abs(b - a);
        }
    return seams;
}

namespace {
/// 一组读数里非 NaN 部分的中位数与最大值；全 NaN 返回 (NaN, NaN)。
std::pair<double, double> medianMax(std::vector<double> v)
{
    v.erase(std::remove_if(v.begin(), v.end(),
                [](double d) { return std::isnan(d); }),
        v.end());
    if (v.empty())
        return { std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::quiet_NaN() };
    const size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + static_cast<long>(mid), v.end());
    double med = v[mid];
    if (v.size() % 2 == 0) {
        const double lo = *std::max_element(v.begin(), v.begin() + static_cast<long>(mid));
        med = 0.5 * (lo + v[mid]);
    }
    return { med, *std::max_element(v.begin(), v.end()) };
}
}  // namespace

void printAbsoluteErrorInfo(const EdgeReadout& r)
{
    std::printf("\n  ── J1 shadow-edge-absolute-error（INFO 只报数不判；🔴 K 未定 TODO——"
                "每格 = |误差| mm（= 几倍本级纹素），与本级纹素 mm 并排；这张表就是定 K 的"
                "推导材料）──\n");
    std::printf("  %-14s %18s", "板", "自然 D=5");
    for (int c = 0; c < 4; ++c)
        std::printf("      钉c%d", c);
    std::printf("\n  %-14s %18s", "", "纹素mm | err mm (×tex)");
    for (int c = 0; c < 4; ++c)
        std::printf("  %14s", "纹素mm | err(×tex)");
    std::printf("\n");
    for (size_t i = 0; i < r.plateNames.size(); ++i) {
        std::printf("  %-14s", r.plateNames[i].c_str());
        const auto cell = [&](double err, double texel) {
            if (std::isnan(err))
                return std::string(edgeMm(texel)) + " |       —";
            std::ostringstream o;
            o.precision(2);
            o << std::fixed << texel << " | " << edgeMm(err) << " ("
              << std::abs(err) / texel << "×)";
            return o.str();
        };
        std::printf(" %26s", cell(r.errNat[i], r.texelMm[i]).c_str());
        for (int c = 0; c < 4; ++c)
            std::printf(" %26s",
                cell(r.errPinned[c][0][i], r.texelMmByCascade[c]).c_str());
        std::printf("\n");
    }
}

/// 自然曲线的台阶读法（相邻板跨级处的跳变）——与钉级读法应当一致（不一致 = 前提破）。
void printNaturalStaircase(const EdgeReadout& r)
{
    std::printf("  自然曲线台阶读法（跨级相邻板，D=5）：");
    bool any = false;
    for (size_t i = 0; i + 1 < r.plateNames.size(); ++i) {
        if (r.cascadePred[i] == r.cascadePred[i + 1])
            continue;
        const double d = r.errNat[i + 1] - r.errNat[i];
        std::printf(" %s→%s %s mm;", r.plateNames[i].c_str(), r.plateNames[i + 1].c_str(),
            edgeMm(d).c_str());
        any = true;
    }
    if (!any)
        throw std::runtime_error("自然曲线没有任何跨级相邻板（级联台阶不存在）——靶的"
              "「影边跨 ≥3 级联」前提在运行期没成立，缝宽的台阶读法无事可读");
    std::printf("\n");
}

void printSeamWidthInfo(const EdgeReadout& r)
{
    std::printf("\n  ── J2 shadow-edge-seam-width —— 🔴 **本靶头条交付数**：缝宽毫米"
                "（seam(c↔c+1) = |误差(级c) − 误差(级c+1)|，出厂 D=5 钉级臂；INFO 不判，"
                "N 未定）= 债-139 交界带混合带宽的设计输入 ──\n");
    const auto seams = seamWidths(r);
    // 每交界至少一块板可量（否则缝宽缺角 = 输出看起来完整而少一条交界 = 假绿）。
    for (int c = 0; c < 3; ++c)
        if (std::isnan(medianMax(seams[c]).first))  // 全 NaN ⟺ 该交界无任何可量板
            throw std::runtime_error("J2：交界 c" + std::to_string(c) + "↔c"
                + std::to_string(c + 1) + " 没有任何一块板在两侧都可量（缝宽无从定义）——"
                  "级片区重叠带里没板 = 靶摆错了，缝宽不许缺角");
    std::printf("  %-14s %12s %12s %12s\n", "板", "c0↔c1 mm", "c1↔c2 mm", "c2↔c3 mm");
    std::vector<double> all;
    for (const auto& row : seams)
        for (const double v : row)
            if (!std::isnan(v))
                all.push_back(v);
    for (size_t p = 0; p < r.plateNames.size(); ++p) {
        std::printf("  %-14s", r.plateNames[p].c_str());
        for (int c = 0; c < 3; ++c)
            std::printf(" %12s", edgeMm(seams[c][p]).c_str());
        std::printf("\n");
    }
    for (int c = 0; c < 3; ++c) {
        const auto [med, mx] = medianMax(seams[c]);
        std::printf("  c%d↔c%d 交界：中位 %s mm · 最大 %s mm\n", c, c + 1,
            edgeMm(med).c_str(), edgeMm(mx).c_str());
    }
    const auto [medAll, mxAll] = medianMax(all);
    std::printf("  全部交界合计：中位 %s mm · 最大 %s mm\n", edgeMm(medAll).c_str(),
        edgeMm(mxAll).c_str());
    printNaturalStaircase(r);
}

void printCascadeAgreementInfo(const EdgeReadout& r)
{
    std::printf("\n  ── J3 shadow-edge-cascade-agreement（INFO 只报数不判；🔴 N 未定——"
                "seam / max(两纹素) 的无量纲比值；N 的推导两头住注册表：固有下界 = 两级"
                "吸附+PCF 不对称做差**不消**，缺陷态 = 混合方案容忍的缝）──\n");
    const auto seams = seamWidths(r);
    for (int c = 0; c < 3; ++c) {
        const double texel = std::max(r.texelMmByCascade[c], r.texelMmByCascade[c + 1]);
        std::printf("  c%d↔c%d（max 纹素 %s mm）：", c, c + 1, edgeMm(texel).c_str());
        for (size_t p = 0; p < r.plateNames.size(); ++p)
            if (!std::isnan(seams[c][p]))
                std::printf(" %s=%.2f×", r.plateNames[p].substr(6).c_str(), seams[c][p] / texel);
        std::printf("\n");
    }
    std::printf("  （退役说明：原 J3「误差随 D 单调」已删——本靶上 D 逐位空操作，"
                "前提不存在；LOD 画质/成本 = 下一台「细附属结构」靶）\n");
}

}  // namespace RGL::imgqa
