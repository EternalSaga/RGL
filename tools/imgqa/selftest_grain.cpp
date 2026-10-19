// selftest_grain.cpp —— `shadow-grain`（影场颗粒度）**口径层算子自检**（判据 3，无引擎）。
//
// 另立 TU（不并进 selftest_shadow.cpp）的理由 = 该文件头注释把它的缝声明为「阴影族**两台**
// 量具（shadow-softness / shadow-kernel-scale）……改动缝是同一条（ProfileOpts 的覆写键 +
// 阴影路由枚举 + shadow.lua 的键形状）」——本仪器的改动缝多一枚 `csm.slope_bias` 键与
// 「区域来自几何」的投影件，塞进去会把那条声明变成假话。切法先例 = `selftest_block.cpp` /
// `selftest_blend_sort.cpp`（一台仪器一个自检 TU，跨 TU 只共享 `selftest.h` 的 Checker）。
//
// ## 这四条是任务口径钦定的算子自检（判据设计者已实测全中，逐条对应口径定义）
//   ① 棋盘噪声（每像素交替）→ 恰 1.0000   ② 竖条纹（每像素交替）→ 恰 1.0000
//   ③ 单调斜坡（一条影缘，宽 34）→ 恰 1/33 = 0.0303   ④ 常数场 → 恰 0.0000
// 它们钉的是「算子数的是我说的那个量」：①② 钉双向跳变与分母 rows×(cols−1)，③ 钉闭式刻度
// （一条缘 = 1/(cols−1)，判据 1 的阈值就长在这个刻度上），④ 钉零点。
//
// ## 变异红线（哪个变异让我红；实跑记录见交付说明）
//   · 只数单向跳变（`l != r` → `!l && r`）→ ①② 红（1.0 → 0.5）；逐带版同样红
//     （「带在场恰 1.0」给 0.5——grainBandFlips 与 grainDensity 是两份实现，各钉各的）。
//   · 分母 rows×cols → ③ 红（1/33 → 32/33÷34）。
//   · 去掉 srgbDecode（字节域算 cov）→ 「线性域锚点」红（0.784061 → 0.497989）。
//   · 删 eps → 「参照全黑」红（0 → NaN）。
//   · 二值线挪离 0.5 → 「恰 0.5 不计 / 0.5+ε 计」两条各红一边。
//   · clip 上界删掉 → 「变亮不计」在通道均值口径下红（一通道爆负拉过线）。
//   · 存活率分母 (2·rows) 写成 (rows) → 「带在场 = 恰 1.0」红（给 2.0）。
//   · 存活率把「半行带」与「整行带」混为一谈（如改成按像素占比）→ 「半高带 = 恰 0.5」红。
#include "selftest.h"

#include "imgqa.h"
#include "shadow_grain_metric.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {
namespace {

/// 按闭式公式铺 cov 场：cov(y,x) = f(x,y)（自检输入全部是解析构造，不吃截图）。
cv::Mat covField(int cols, int rows, double (*f)(int, int))
{
    cv::Mat m(rows, cols, CV_64FC1);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            m.at<double>(y, x) = f(x, y);
    return m;
}

double fChecker(int x, int y)
{
    return ((x + y) % 2) ? 1.0 : 0.0;
}
double fStripes(int x, int y)
{
    (void)y;
    return (x % 2) ? 1.0 : 0.0;
}
double fRamp(int x, int y)
{
    (void)y;
    return static_cast<double>(x) / 33.0;  // 宽 34 的单调斜坡（一条影缘）
}
double fZero(int, int)
{
    return 0.0;
}
double fSix(int, int)
{
    return 0.6;
}
double fHalf(int, int)
{
    return kGrainBin;
}
double fHalfUp(int x, int)
{
    return x ? kGrainBin - 1e-9 : kGrainBin + 1e-9;
}
double fLeftChecker(int x, int y)
{
    return x < 34 ? fChecker(x, y) : 0.0;
}
/// 一条完整闭式带（列 [10,15) 遮、其余受光）：带两缘各跨 0.5 线一次 ⟹ 存活窗内每行恰 2。
double fOneBand(int x, int)
{
    return (x >= 10 && x < 15) ? 1.0 : 0.0;
}
/// 半高带（列 [10,15) 遮但只在前 20 行）：存活窗内跳变 = 2×20 行 / 40 行 ⟹ 存活率恰 0.5。
double fHalfBand(int x, int y)
{
    return (x >= 10 && x < 15 && y < 20) ? 1.0 : 0.0;
}
/// 带缘抖动（列 10 与 15 处每行双跳变：0→1→0 与 1→0→1）：存活窗内每行 4 ⟹ 存活率恰 2.0。
double fJitterBand(int x, int y)
{
    (void)y;
    if (x >= 10 && x < 15)
        return (x % 2) ? 1.0 : 0.0;  // 带内椒盐：每行多对跳变
    return 0.0;
}

cv::Mat bgrField(int cols, int rows, cv::Vec3b v)
{
    return cv::Mat(rows, cols, CV_8UC3, cv::Scalar(v[0], v[1], v[2]));
}

double covAt(const cv::Mat& cov, int x = 0, int y = 0)
{
    return cov.at<double>(y, x);
}

}  // namespace

/// `shadow-grain` 口径层：密度算子（任务钦定四条 + 反向 + 窗口纪律）、逐带存活率算子
/// （2026-10-19 多缝深加密：在场 = 恰 1 / 半高 = 恰 0.5 / 被抹 = 恰 0 / 抖动 = 恰 2 +
/// 三条守卫）与覆盖度算子（线性域锚点 + eps 守卫 + clip + 通道均值），期望全部来自
/// 闭式，不启引擎。
void testShadowGrain(Checker& c)
{
    const cv::Rect win34(0, 0, 34, 40);
    // ── 任务钦定的四条算子自检（口径定义者实测全中，逐字对应）─────────────────
    c.chk("grain 自检①：棋盘噪声（每像素交替）密度恰 1.0000",
        std::abs(grainDensity(covField(34, 40, fChecker), win34) - 1.0) < 1e-12);
    c.chk("grain 自检②：竖条纹（每像素交替）密度恰 1.0000",
        std::abs(grainDensity(covField(34, 40, fStripes), win34) - 1.0) < 1e-12);
    c.chk("grain 自检③：单调斜坡（一条影缘，宽 34）密度恰 1/33（闭式刻度 = 判据 1 的尺）",
        std::abs(grainDensity(covField(34, 40, fRamp), win34) - 1.0 / 33.0) < 1e-12);
    c.chk("grain 自检④：常数场密度恰 0.0000",
        grainDensity(covField(34, 40, fZero), win34) == 0.0);
    // ── 反向与边界（密度量的是跳变，不是影量）─────────────────────────────────
    c.chk("grain 反向：全影常数场（cov≡0.6）密度也是 0（全遮没跳变，影量不是本判据）",
        grainDensity(covField(34, 40, fSix), win34) == 0.0);
    c.chk("grain 二值线：cov≡0.5 不计（严格 >）",
        grainDensity(covField(34, 40, fHalf), win34) == 0.0);
    c.chk("grain 二值线：0.5+ε|0.5−ε 相邻计 1 次跳变（阈值两侧各钉一条，防后来人只验半边）",
        grainDensity(covField(2, 1, fHalfUp), cv::Rect(0, 0, 2, 1)) == 1.0);
    {
        const cv::Mat half = covField(68, 40, fLeftChecker);
        c.chk("grain 窗口：左半棋盘右半常数 → 左窗 1.0 右窗 0.0（窗口是判据的一部分）",
            grainDensity(half, cv::Rect(0, 0, 34, 40)) == 1.0
                && grainDensity(half, cv::Rect(34, 0, 34, 40)) == 0.0);
    }
    c.chk("grain 守卫：窗口宽 1（分母 0）立刻抛，不给空分母的假绿",
        throwsStd([&] { (void)grainDensity(covField(4, 4, fZero), cv::Rect(0, 0, 1, 4)); }));
    c.chk("grain 守卫：窗口越界立刻抛（不夹到图像边缘再算）",
        throwsStd([&] { (void)grainDensity(covField(8, 8, fZero), cv::Rect(4, 0, 8, 8)); }));
    // ── 逐带存活率算子（2026-10-19 多缝深加密新增；期望全部闭式，分母 = 2·rows 刻度）──
    {
        const cv::Rect win40(0, 0, 34, 40);
        const cv::Range cols(8, 17);  // 完整包住带 [10,15) 两缘的跳变对
        c.chk("grain 存活率：一条完整闭式带（两缘每行恰 2 跳变）= 恰 1.000",
            grainBandSurvival(covField(34, 40, fOneBand), cols, win40) == 1.0);
        c.chk("grain 存活率：半高带（只前 20 行有带）= 恰 0.500（逐行计数，不是像素占比）",
            grainBandSurvival(covField(34, 40, fHalfBand), cols, win40) == 0.5);
        c.chk("grain 存活率：常数 0（带被抹）= 恰 0.000（J1 缺陷态的刻度）",
            grainBandSurvival(covField(34, 40, fZero), cols, win40) == 0.0);
        c.chk("grain 存活率：带缘抖动（每行 4 跳变）= 恰 2.000（可 >1，判决线只钉下限）",
            grainBandSurvival(covField(34, 40, fJitterBand), cols, win40) == 2.0);
        c.chk("grain 存活率守卫：列区间宽 2（闭式「每行 2」不成立）立刻抛",
            throwsStd([&] {
                (void)grainBandFlips(covField(34, 40, fOneBand), cv::Range(9, 11), win40);
            }));
        c.chk("grain 存活率守卫：列区间不在窗口 x 跨度内立刻抛（夹边 = 假绿的一种）",
            throwsStd([&] {
                (void)grainBandFlips(covField(34, 40, fOneBand), cv::Range(30, 40), win40);
            }));
        c.chk("grain 存活率守卫：窗口越界立刻抛（同 grainDensity 的纪律）",
            throwsStd([&] {
                (void)grainBandFlips(covField(8, 8, fZero), cv::Range(4, 10),
                    cv::Rect(4, 0, 8, 8));
            }));
    }
    // ── 覆盖度算子（cov = mean_RGB clip((off−on)/(off+eps), 0, 1)，线性域）─────
    {
        const cv::Mat white = bgrField(2, 2, cv::Vec3b(255, 255, 255));
        const cv::Mat black = bgrField(2, 2, cv::Vec3b(0, 0, 0));
        c.chk("grain cov：on=off → 恒 0（同图无影可谈）",
            cv::countNonZero(grainCoverage(white, white) != 0.0) == 0);
        c.chk("grain cov：off=白 on=黑 → 恰 1/(1+eps)=0.99990001（不是 1！eps 在场证明）",
            std::abs(covAt(grainCoverage(black, white)) - 0.99990000999900008) < 1e-12);
        c.chk("grain cov：参照全黑（off=on=0）→ 0 不是 NaN（eps 守卫，黑参照没影可谈）",
            covAt(grainCoverage(black, black)) == 0.0
                && !std::isnan(covAt(grainCoverage(black, black))));
        c.chk("grain cov：on 比 off 亮（变亮）→ clip 到 0（负覆盖度不许进均值）",
            covAt(grainCoverage(white, black)) == 0.0);
        // 🔴 线性域锚点（本判据的项目红线「统计一律线性域」的机器判据）：
        // lin(128) = ((128/255+0.055)/1.055)^2.4 = 0.21586050…（闭式，独立于实现）
        // cov = (1 − lin)/(1 + eps) = 0.78406109…。去掉 srgbDecode 的变异给 0.49798941。
        const cv::Mat gray = bgrField(2, 2, cv::Vec3b(128, 128, 128));
        c.chk("grain cov：线性域锚点 cov(白,128灰) = 0.78406109（字节域算给 0.49798941）",
            std::abs(covAt(grainCoverage(gray, white)) - 0.78406109377672306) < 1e-12);
        // 通道均值：off=白、on=(0,128,255) → 三通道 cov = 0.99990001 / 0.78406109 / 0，
        // 均值 0.59465370（闭式）。顺带钉「RGB 三通道取均值」不是取 max/luma。
        const cv::Mat mix = bgrField(2, 2, cv::Vec3b(0, 128, 255));
        c.chk("grain cov：三通道取均值（0.99990001+0.78406109+0)/3 = 0.59465370",
            std::abs(covAt(grainCoverage(mix, white)) - 0.59465370125857442) < 1e-12);
        c.chk("grain cov：尺寸不同立刻抛（on/off 不是同一帧）",
            throwsStd([&] {
                (void)grainCoverage(bgrField(2, 2, cv::Vec3b(1, 1, 1)),
                    bgrField(3, 2, cv::Vec3b(1, 1, 1)));
            }));
    }
    // ── 三条判决线必须挂号（改名漏挂 = verdictFor 运行期才炸，太晚）─────────────
    for (const char* name : { "shadow-grain-band-survival", "shadow-grain-wiring-anchor",
                             "shadow-grain-factory-identity" }) {
        bool found = false;
        for (const auto& j : registeredJudgments())
            found = found || std::string(j.name) == name;
        c.chk("grain 判据已挂号进注册表", found, name);
    }
}

}  // namespace RGL::imgqa
