// shadow_grain_judge.cpp —— `shadow-grain` 的**三条判决线**与逐带读数表（缝 = 仪器内部，
// 跨 TU 面在 shadow_grain.h；本文件只出 Verdict，不碰引擎也不读 manifest）。
//
// 判决方向的完整论证（含「密度必须低」为什么是被证伪的旧方向）住 `shadow_grain.cpp`
// 头注；容差理由的**用户可见版**住 `judgments_table_grain.cpp`（`imgqa check` 打印的
// 就是它，两边口径必须一致，改一边必须改另一边）。
//
// ## 变异红线（哪个变异让我红；实跑读数见 2026-10-19 交付说明）
//   · J1 比较方向翻转（`>=` → `<=`）→ 实机红（出厂档存活率实测 = 1.000 ≫ 0.5）。
//   · J1 下限挪到 > 1（1.05）→ 红（线在承重）；挪到 0.25 → 仍绿（余量读数进台账）。
//   · J2 高臂与 0 臂同值 → 差 = 0 → 红；J2 高臂上限从「半条带增量」放宽到整条 →
//     实测 0.0000 仍绿（机器只保证读数与线的相对位置被打印）。
//   · J3 的比较对象换成「dflt vs dflt」（自己比自己）→ 恒真——所以 J3 的两个比较
//     伙伴（csm0 / deliv）必须与 dflt 是**不同的 shadow.lua 文本**，这是本判据存在的
//     全部意义（对账 = 两条独立路径解析出的配置渲染出同一张图）。
#include "shadow_grain.h"

#include <cstdio>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

std::string pct(double survival)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.1f%%", 100.0 * survival);
    return buf;
}

std::string worstBand(const std::vector<GrainBandReadout>& readouts)
{
    size_t worst = 0;
    for (size_t i = 1; i < readouts.size(); ++i)
        if (readouts[i].survivalDefault < readouts[worst].survivalDefault)
            worst = i;
    const auto& r = readouts[worst];
    return "缝" + std::to_string(r.group) + "（d=" + std::to_string(r.depthM).substr(0, 4) + " m）";
}

}  // namespace

std::string grainFixed4(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

std::string grainFixed3(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}

/// J1：出厂档（引擎默认，由 J3 对账背书）真实槽底影带存活率下限，逐带取最严一条。
void pushSurvivalVerdict(std::vector<Verdict>& vs, const std::vector<GrainBandReadout>& readouts)
{
    double worst = 1.0;
    for (const auto& r : readouts)
        worst = worst < r.survivalDefault ? worst : r.survivalDefault;
    const bool ok = worst >= kSurvivalFloor;
    vs.push_back(verdictFor("shadow-grain-band-survival", ok,
        "出厂档（引擎默认档，J3 对账背书）靶上真实槽底影带存活率最小值 = " + pct(worst)
            + "（下限 " + pct(kSurvivalFloor) + "，逐带共 " + std::to_string(readouts.size())
            + " 条，最严 = " + worstBand(readouts) + "）"
            + (ok ? ""
                  : "\n  这条红 = 出厂档把真实影带吃掉了（或带缘在抖动 = 痤疮回来了）。"
                    "先分两种红：csm0 臂同带存活率也低 → 靶/接线问题（回看闸门为何放行）；"
                    "只有出厂档低 → slope_bias 的出厂值把「产生对比的阴影」压掉了——"
                    "configs/shadow.lua 的定档表（0→8 白拿、8→16 与抹除同段）就是这条红的"
                    "出处，改出厂值要连表一起改")));
}

/// J2：接线阳性锚点——slope_bias 自变量两臂（0 vs 32）显著不同且大值臂趋 0。
void pushWiringVerdict(std::vector<Verdict>& vs, double density0, double density32,
    double closed, double bandStep)
{
    const double gap = density0 - density32;
    const bool gapOk = gap >= kWiredGapFloor * closed;
    const bool highOk = density32 <= kWiredHighCeilFactor * bandStep;
    const bool ok = gapOk && highOk;
    vs.push_back(verdictFor("shadow-grain-wiring-anchor", ok,
        "csm0 臂密度 = " + grainFixed4(density0) + "，csm32 臂 = " + grainFixed4(density32)
            + "；差 " + grainFixed4(gap) + " ≥ " + grainFixed4(kWiredGapFloor * closed)
            + "（= " + std::to_string(kWiredGapFloor) + "×闭式刻度 " + grainFixed4(closed)
            + "）" + (gapOk ? " ✓" : " ✗") + "，且大值臂 " + grainFixed4(density32) + " ≤ "
            + grainFixed4(kWiredHighCeilFactor * bandStep) + "（= 半条带的残留增量 "
            + grainFixed4(bandStep) + "）" + (highOk ? " ✓" : " ✗")
            + "——本条红时**J1 的绿一律不作数**"
              + (ok ? ""
                    : "\n  这条红 = 旋钮到画面的通路断了/两档塌成同值，不是「痤疮修好了」："
                      "bias↑ ⟹ 槽带被抹除是闭式结论（前墙存储深度被推远 m·dZ/dpixel）。"
                      "先查两臂回显差集行（csm.slope_bias=0 / =32）、再查 vkCmdSetDepthBias"
                      " 形参顺序（rgl/shader.cppm applyDynamicState 注释：slope 送进过 "
                      "clamp 位 = 本判据要抓的接线形态实录）")));
}

/// J3：出厂档对账——默认臂与两枚参照臂逐位相同（J1 的读数成为陈述的 precondition）。
void pushIdentityVerdict(std::vector<Verdict>& vs, std::int64_t defaultDiff,
    std::int64_t deliveryDiff)
{
    const bool ok = defaultDiff == 0 && deliveryDiff == 0;
    vs.push_back(verdictFor("shadow-grain-factory-identity", ok,
        "逐位不同的像素数：dflt（不写键）vs csm0（显式 0.0）= " + std::to_string(defaultDiff)
            + "，deliv（configs/shadow.lua 全文）vs csm0 = " + std::to_string(deliveryDiff)
            + "（两条都必须 = 0）"
            + (ok ? ""
                  : "\n  这条红 = 出厂档漂了，J1 的读数失去解释（它读的是「不写键」臂，"
                    "对账不过 = 那一臂不再是出厂态）：dflt≠csm0 → rgl/shadow_config.cppm 的"
                    "代码默认已不是 0；deliv≠csm0 → configs/shadow.lua 的出厂值已不是 0 "
                    "（或与代码默认分叉）。两种情况都必须回到本仪器重新对账：改 csm0 臂的"
                    "显式值 + 更新定档表，**不许**删对账让它闭嘴")));
}

}  // namespace RGL::imgqa
