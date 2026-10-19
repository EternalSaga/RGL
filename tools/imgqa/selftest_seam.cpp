// selftest_seam.cpp —— `shadow-cascade-seam`（相邻级联阴影分歧）**口径层算子自检**，无引擎。
//
// 另立 TU（不并进 `selftest_grain.cpp`）的理由 = 该文件头注释把它的缝声明为「`shadow-grain`
// 口径层（影场颗粒度，判据 3）」——本仪器测的是**两臂差分场**上的台阶与翻转率，不是单张
// cov 场的跳变密度，算子面完全不同。切法先例 = `selftest_block.cpp` / `selftest_blend_sort.cpp`
// （一台仪器一个自检 TU，跨 TU 只共享 `selftest.h` 的 Checker）。
//
// ## 这些断言钉的是「算子数的是我说的那个量」，期望值全部闭式（不抄实机读数、不抄被测码）
//   ① 常数场 → 台阶恰 0 且行号 = -1（零读数与「没数」可区分的另一面：真零台阶必须报 0）
//   ② 单条高度 0.25 的横向台阶 → 恰 0.25   ③ 高度 1 的台阶 → 恰 1.0
//   ④ 宽 33 的单调斜坡（halfWidth=1）→ 恰 1/33（grain 的刻度手法：斜坡的逐行斜率就是尺）
//   ⑤ flipFraction 在已知合成对上 → 精确分数（16/64 = 恰 0.25，分母 = band 不是整幅）
//   ⑥ 四条守卫各一条 throwsStd（band 空 / band 越界 / halfWidth ≤ 0 / 两图尺寸不同）
//   ⑦ **参照臂有效性闸门**五条（0 → 不抛 / 恰等于界 → 不抛 / 略超 → 抛 / 实测污染读数 0.239
//     → 抛 / 错误串带读数+界+行+带子）——它是量具自检不是画质判据，理由住
//     `shadow_seam_metric.h` 的声明处。四条吃 `kSeamTexelFloor` **本身**，不抄字面量。
//     ①–⑤ 住 `testShadowSeam`（闭式数值），⑥⑦ 住 `testShadowSeamGuards`（抛与挂号）：
//     单函数 120 代码行护栏点名后按缝切，改口径动前者、加守卫动后者。
//
// ## 变异红线（哪个变异让我红；实跑记录见交付说明）
//   · 去掉 srgbDecode（字节域算 cov）→ 「线性域锚点」红（0.78406109 → 0.49798941）。
//   · 删 eps → 「参照全黑」红（0 → NaN）。
//   · clip 下界删掉 → 「变亮不计」红；clip 上界删掉 → 「off=黑 on=白 恰 1/(1+eps)」红。
//   · 通道均值改成 max / luma → 「三通道取均值」红（闭式 0.59465370）。
//   · flipFraction 的 `> thresh` 弱化成 `>= thresh` → 「恰等于阈值不计」红；
//     弱化成 `>= 0`（恒真）→ 「两幅同图 = 0 翻转」红（给 1.0）。
//   · flipFraction 分母写成整幅 → 「band 是分母」红（小 band 给 1.0 的那条给 0.25）。
//   · maxRowStep 的「上下两窗中位之差」换成逐像素差 → ②③ 红（给 0.25/6 量级而非 0.25/1.0）、
//     ① 仍 0（所以必须同时钉 ②③，只钉 ① 抓不到这条）。
//   · halfWidth 不参与计算（写死 6）→ ④ 红（1/33 → 6/33）。
//   · 取 max 改成取均值 → ②③ 红（均值被非交界行的部分重叠窗拉低）；
//     改成取末行 → 「行号 = 台阶所在行」红。
//   · 中位数偶数窗改成「取下中位」（不取中间两值平均）→ 「偶数窗中位 = 中间两值平均」红。
//   · 四条守卫任一改成 return 0 → 对应那条 throwsStd 红（**返回 0 会被上层读成
//     「分歧为零 = 判据通过」**，这就是守卫必须抛的全部理由）。
//   · 上下窗越界守卫删掉 → 「窗越界立刻抛」红（越界的短窗静默变短 = 边界行换了一把尺量）。
//   · 闸门的 `>` 反向成 `<=`（= 永不抛）→ 「略超界应抛」与「污染读数应抛」两条同时红；
//     界换成 0 → 「恰等于界不抛」红；界换成 1e9 → 「略超界应抛」与「污染读数应抛」两条红
//     （selftest 把常数当**界**传，改常数打得到自检；连界带读数一起抄成字面量的话这条变异
//     只改实机、全绿，闸门就成了摆设）。
//   · 闸门的错误串里删掉读数或界 → 「错误串带读数+界+行+带子」红（只说「闸门失败」的文案
//     等于让人从零查起）。
#include "selftest.h"

#include "imgqa.h"
#include "shadow_cascade_seam.h"  // kSeamTexelFloor：闸门界吃**常数本身**，见下方注释
#include "shadow_seam_metric.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {
namespace {

/// 按闭式公式铺 cov / 差分场：f(x, y)（自检输入全部解析构造，不吃截图）。
cv::Mat field(int cols, int rows, double (*f)(int, int))
{
    cv::Mat m(rows, cols, CV_64FC1);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            m.at<double>(y, x) = f(x, y);
    return m;
}

double fZero(int, int)
{
    return 0.0;
}
double fSix(int, int)
{
    return 0.6;
}
/// 一条横向台阶（行 r0 起从 0 跳到 h）——差分场上「一条缝」的抽象形态。
double stepH(int x, int y, int r0, double h)
{
    (void)x;
    return y < r0 ? 0.0 : h;
}
double fStep25(int x, int y)
{
    return stepH(x, y, 20, 0.25);
}
double fStep1(int x, int y)
{
    return stepH(x, y, 20, 1.0);
}
/// 宽 33 的单调斜坡（沿 y，逐行斜率 1/33）：halfWidth=1 时台阶闭式 = 1/33。
double fRampY(int, int y)
{
    return static_cast<double>(y) / 33.0;
}
/// 台阶只在 x ∈ [4,8) 存在（band 是分母 / 列窗口的判据用）。
double fStepColBand(int x, int y)
{
    return (y >= 20 && x >= 4 && x < 8) ? 1.0 : 0.0;
}
/// 影只占右半幅（x ≥ 4）：有影面积在全幅 = 恰 0.5、左半 band = 恰 0、右半 band = 恰 1。
double fRightShadow(int x, int y)
{
    (void)y;
    return x >= 4 ? 1.0 : 0.0;
}
/// 一半像素 cov **恰等于** 0.5（钉「严格 >」：半影缘以内不算有影）。
double fHalfBin(int x, int y)
{
    (void)y;
    return x % 2 == 0 ? 0.5 : 0.0;
}

cv::Mat bgrField(int cols, int rows, cv::Vec3b v)
{
    return cv::Mat(rows, cols, CV_8UC3, cv::Scalar(v[0], v[1], v[2]));
}

double covAt(const cv::Mat& cov, int x = 0, int y = 0)
{
    return cov.at<double>(y, x);
}

/// 中央 40×20 的行窗（行 10..29），台阶行 20 落在窗内且上下 6 行窗都不越界。
const cv::Rect kWin(0, 10, 40, 20);

}  // namespace

/// `shadow-cascade-seam` 口径层：覆盖度算子（线性域锚点 + eps + clip + 通道均值）、
/// 翻转率算子（精确分数 + 严格阈值 + band 是分母）、最硬横向台阶算子（任务钦定四条闭式 +
/// 行号回传 + 偶数窗中位口径）与四条守卫，期望全部闭式，不启引擎。
void testShadowSeam(Checker& c)
{
    // ── 任务钦定的台阶闭式四条 ────────────────────────────────────────────────
    c.chk("seam 台阶①：常数场（0.6）台阶恰 0 且行号 -1（没有台阶就不许编一个行号出来）",
        [&] {
            const SeamStep s = maxRowStep(field(40, 40, fSix), kWin);
            return s.step == 0.0 && s.row == -1;
        }());
    c.chk("seam 台阶②：单条高度 0.25 的横向台阶 → 恰 0.25（p90 取的是满窗行，不被部分重叠窗稀释）",
        std::abs(maxRowStep(field(40, 40, fStep25), kWin).step - 0.25) < 1e-15);
    c.chk("seam 台阶③：高度 1 的横向台阶 → 恰 1.0（满幅同高度 ⟹ p90 = max = 台阶高度）",
        std::abs(maxRowStep(field(40, 40, fStep1), kWin).step - 1.0) < 1e-15);
    c.chk("seam 台阶④：宽 33 的单调斜坡（halfWidth=1）→ 恰 1/33（斜坡斜率就是这把尺的刻度）",
        std::abs(maxRowStep(field(40, 40, fRampY), kWin, 1).step - 1.0 / 33.0) < 1e-12);
    c.chk("seam 台阶④′：同一条斜坡 halfWidth=6 → 恰 6/33（halfWidth 真的进了窗计算，不是写死）",
        std::abs(maxRowStep(field(40, 40, fRampY), kWin, 6).step - 6.0 / 33.0) < 1e-12);
    // ── 行号回传（判据的可归因输出：债-139 那条缝靠它落到「第 472 行」）──────────
    // 用 halfWidth=1 钉行号：h=6 时窗内一半已过台阶的行**全都**取到满高（18..22 并列），
    // 报哪一行由平票规则决定，那是平票规则而不是台阶位置；h=1 时最大值唯一落在台阶行。
    c.chk("seam 台阶行号（halfWidth=1）：0.25 台阶报 20、1.0 台阶也报 20（回传台阶所在行）",
        maxRowStep(field(40, 40, fStep25), kWin, 1).row == 20
            && maxRowStep(field(40, 40, fStep1), kWin, 1).row == 20);
    c.chk("seam 台阶平票规则：h=6 时 18..22 并列，取**第一个**最硬行（可复现，不是随机）",
        maxRowStep(field(40, 40, fStep25), kWin).row == 18);
    c.chk("seam 台阶列窗口：台阶只在 x∈[4,8) 时，宽窗 p90 = 0（4/40 列 < 10%）、窄窗 = 1",
        maxRowStep(field(40, 40, fStepColBand), kWin).step == 0.0
            && maxRowStep(field(40, 40, fStepColBand), cv::Rect(4, 10, 4, 20)).step == 1.0);
    c.chk("seam 偶数窗中位口径：上窗 {0,0} 下窗 {0,1} → 台阶恰 0.5（偶数窗取中间两值平均，"
          "写成「取下中位」会给 0）",
        [&] {
            const cv::Mat m = field(4, 8, [](int, int y) { return y >= 5 ? 1.0 : 0.0; });
            // 只取 y=4 这一行：上窗 rows{2,3} = {0,0} → 0；下窗 rows{4,5} = {0,1} → 中位 0.5。
            const SeamStep s = maxRowStep(m, cv::Rect(0, 4, 4, 1), 2);
            return s.step == 0.5 && s.row == 4;
        }());
    c.chk("seam 交替场：halfWidth=2 的 0|1 逐行交替 → 两窗中位相同 ⟹ 台阶恰 0",
        [&] {
            const cv::Mat m = field(4, 8, [](int, int y) { return (y % 2) ? 0.0 : 1.0; });
            return maxRowStep(m, cv::Rect(0, 3, 4, 2), 2).step == 0.0;
        }());
    // ── 翻转率算子（精确分数 + 严格阈值 + band 是分母）────────────────────────
    {
        const cv::Mat z = field(8, 8, fZero);
        const cv::Rect all(0, 0, 8, 8);
        c.chk("seam 翻转率：两幅同图 → 恰 0（同图没有分歧可谈）",
            flipFraction(z, z, all) == 0.0);
        const cv::Mat one = field(8, 8, fSix);  // 0.6 > 0.5 ⟹ 与 0 的分歧算翻转
        c.chk("seam 翻转率：0 场 vs 0.6 场 → 恰 1.0（全幅都翻）",
            flipFraction(z, one, all) == 1.0);
        const cv::Mat half = field(8, 8, [](int x, int) { return x < 2 ? 1.0 : 0.0; });
        c.chk("seam 翻转率：16/64 像素翻 → 恰 0.25（分母 = band 像素数，闭式分数不是小数近似）",
            flipFraction(z, half, all) == 0.25);
        const cv::Mat edge = field(8, 8, fZero);
        const cv::Mat atThresh = field(8, 8, [](int, int) { return kSeamBin; });
        c.chk("seam 翻转率阈值：|Δ| 恰 = 0.5 不计（严格 >；挪成 >= 这条红）",
            flipFraction(edge, atThresh, all) == 0.0);
        const cv::Mat justOver = field(8, 8, [](int, int) { return kSeamBin + 1e-9; });
        c.chk("seam 翻转率阈值：|Δ| = 0.5+1e-9 计（阈值两侧各钉一条，防后来人只验半边）",
            flipFraction(edge, justOver, all) == 1.0);
        // band 是分母（不是整幅）：台阶只在 x∈[4,8) 且 y ≥ 20 的 40×40 场上。取行窗
        // [20,30)（整段都在台阶下）：小 band 全翻 = 恰 1.0，宽 band 只有 4/40 列翻 = 恰 0.1。
        // 分母若写成整幅，小 band 那条会给 0.1 量级而不是 1.0。
        const cv::Mat wide = field(40, 40, fStepColBand);
        const cv::Mat wideZero = field(40, 40, fZero);
        c.chk("seam 翻转率：band 是分母不是整幅（台阶只在 x∈[4,8)：小 band 恰 1.0、宽 band 恰 0.1）",
            flipFraction(wideZero, wide, cv::Rect(4, 20, 4, 10)) == 1.0
                && flipFraction(wideZero, wide, cv::Rect(0, 20, 40, 10)) == 0.1);
    }
    // ── 覆盖度算子（cov = mean_RGB clip((off−on)/(off+eps), 0, 1)，线性域）─────
    {
        const cv::Mat white = bgrField(2, 2, cv::Vec3b(255, 255, 255));
        const cv::Mat black = bgrField(2, 2, cv::Vec3b(0, 0, 0));
        c.chk("seam cov：on=off → 恒 0（同图无影可谈）",
            cv::countNonZero(seamCovField(white, white) != 0.0) == 0);
        c.chk("seam cov：off=白 on=黑 → 恰 1/(1+eps)=0.99990001（不是 1！eps 在场证明）",
            std::abs(covAt(seamCovField(black, white)) - 0.99990000999900008) < 1e-12);
        c.chk("seam cov：参照全黑（off=on=0）→ 0 不是 NaN（eps 守卫，黑参照没影可谈）",
            covAt(seamCovField(black, black)) == 0.0
                && !std::isnan(covAt(seamCovField(black, black))));
        c.chk("seam cov：on 比 off 亮（变亮）→ clip 到 0（负覆盖度不许进均值）",
            covAt(seamCovField(white, black)) == 0.0);
        // 🔴 线性域锚点（项目红线「统计一律线性域」的机器判据）：
        // lin(128) = ((128/255+0.055)/1.055)^2.4 = 0.21586050…（闭式，独立于实现）
        // cov = (1 − lin)/(1 + eps) = 0.78406109…。去掉 srgbDecode 的变异给 0.49798941。
        const cv::Mat gray = bgrField(2, 2, cv::Vec3b(128, 128, 128));
        c.chk("seam cov：线性域锚点 cov(白,128灰) = 0.78406109（字节域算给 0.49798941）",
            std::abs(covAt(seamCovField(gray, white)) - 0.78406109377672306) < 1e-12);
        // 通道均值：off=白、on=(0,128,255) → 三通道 cov = 0.99990001 / 0.78406109 / 0，
        // 均值 0.59465370（闭式）。顺带钉「RGB 三通道取均值」不是取 max/luma。
        const cv::Mat mix = bgrField(2, 2, cv::Vec3b(0, 128, 255));
        c.chk("seam cov：三通道取均值（0.99990001+0.78406109+0)/3 = 0.59465370",
            std::abs(covAt(seamCovField(mix, white)) - 0.59465370125857442) < 1e-12);
        // 🔴 **顺序靶**：逐通道 clip → 再取均值（与 grain 同口径），不是先均值再 clip。
        // 上面那条分不开两种顺序（那里没有通道被 clip，0.59465 两种顺序都给）。这一条能：
        // off=128 灰、on 有一通道**变亮**（255）→ 该通道 clip 成 0，另两通道 0.99953695
        // = o/(o+eps)，均值 0.66635797；先均值再 clip 则整像素被抵成负数 → clip 0.00000000。
        // 变异红线：把 `sum += clamp(...)` 改成先累加 o/n、出循环再 clamp → 本条红（实机
        // 翻转率会从 1.304% 漂到 1.290%，即参照脚本那个口径，见 shadow_seam_metric.cpp 注）。
        const cv::Mat brighten = bgrField(2, 2, cv::Vec3b(255, 0, 0));
        const cv::Mat gray128 = bgrField(2, 2, cv::Vec3b(128, 128, 128));
        c.chk("seam cov：逐通道 clip 再取均值（0.66635797），不是先均值再 clip（那给 0）",
            std::abs(covAt(seamCovField(brighten, gray128)) - 0.66635796823972520) < 1e-12);
    }
}

/// 守卫与挂号面（从 `testShadowSeam` 拆出 = 单函数 120 代码行护栏点名后按**缝**切：上面那台
/// 测的是「输入 → 读数」的闭式数值，这里测的是「输入不合法 ⟹ 抛」与「判据在不在注册表里」，
/// 两块的改动缝本来就不同——改口径动上面，加一条守卫动这里）。
void testShadowSeamGuards(Checker& c)
{
    // ── 四条守卫：一律**抛**，返回 0 会被读成「分歧为零 = 判据通过」──────────────
    c.chk("seam 守卫：band 为空立刻抛（空分母的「0 分歧」= 假绿）",
        throwsStd([&] { (void)flipFraction(field(8, 8, fZero), field(8, 8, fZero), cv::Rect(0, 0, 0, 8)); }));
    c.chk("seam 守卫：band 越界立刻抛（不夹到图像边缘再算）",
        throwsStd([&] { (void)flipFraction(field(8, 8, fZero), field(8, 8, fZero), cv::Rect(4, 0, 8, 8)); }));
    c.chk("seam 守卫：halfWidth ≤ 0 立刻抛（0 宽窗没有中位数，返回 0 = 缺陷态刷绿）",
        throwsStd([&] { (void)maxRowStep(field(8, 8, fSix), cv::Rect(0, 2, 8, 4), 0); }));
    c.chk("seam 守卫：两图尺寸不同立刻抛（on/off 不是同一帧，分歧率没有意义）",
        throwsStd([&] {
            (void)seamCovField(bgrField(2, 2, cv::Vec3b(1, 1, 1)),
                bgrField(3, 2, cv::Vec3b(1, 1, 1)));
        }));
    c.chk("seam 守卫：台阶的上下窗越出图外立刻抛（越界的窗静默变短 = 边界行换了一把尺量）",
        throwsStd([&] { (void)maxRowStep(field(8, 8, fSix), cv::Rect(0, 0, 8, 8), 6); }));
    c.chk("seam 守卫：非 CV_64FC1 的场立刻抛（读错内存的「0 台阶」是假绿）",
        throwsStd([&] {
            (void)maxRowStep(cv::Mat(8, 8, CV_32FC1, cv::Scalar(0)), cv::Rect(0, 2, 8, 4));
        }));
    // ── 参照臂有效性闸门（**量具自检**：带子被非纹素项污染 ⟹ 抛，不产出任何 Verdict）────
    // 四条全部把 `kSeamTexelFloor` 当**界**传进去（不抄成字面量），读数则取实机那两个绝对值
    // （0.000 = 几何一致参照的闭式态；0.058 / 0.239 = 略超界 / 实测污染台阶）。这样把常数
    // 改成 1e9 的变异会同时打掉「略超界应抛」与「污染读数应抛」两条——抄字面量的话那条变异
    // 只改实机、selftest 全绿，闸门就成了摆设。② 的读数必须**相对**界取（等于界），否则
    // 钉不住「含端点」这一格。
    const cv::Rect wide(160, 30, 1280, 841);  // = 实测会假绿的那格带子 x∈[160,1440)
    c.chk("seam 闸门：参照臂台阶 0 → 不抛（几何一致参照的闭式态，闸门不得拦正常读数）",
        !throwsStd([&] { requireBandValid(0.0, -1, wide, kSeamTexelFloor); }),
        "反向用例：闸门若恒抛，实机每次运行都 rc=3，判据永远出不来");
    c.chk("seam 闸门：台阶**恰等于**界 → 不抛（界是闭式上界，含端点；与 J1 的 ≤ 同口径）",
        !throwsStd([&] { requireBandValid(kSeamTexelFloor, 51, wide, kSeamTexelFloor); }),
        "把 > 写成 >= 的弱化合规变异只红这一条");
    c.chk("seam 闸门：台阶略大于界（0.058 > 0.057）→ 抛（把界加裕量 / 乘 2 / 改成 1e9 都在这里的红）",
        throwsStd([&] { requireBandValid(0.058, 51, wide, kSeamTexelFloor); }));
    // 实测污染格必须抛，且错误串**同时**带读数、界、行号、带子 x 区间：只说「闸门失败」的
    // 文案等于让人从零查起（本仓惯例：不同失效形态的错误串两两不同、各自可归因）。
    std::string gateMsg;
    bool gateThrew = false;
    try {
        requireBandValid(0.239, 51, wide, kSeamTexelFloor);
    } catch (const std::exception& e) {
        gateThrew = true;
        gateMsg = e.what();
    }
    c.chk("seam 闸门：实测污染读数 0.239 @ 行 51 → 抛（界改成 1e9 的变异在这里红）", gateThrew,
        gateThrew ? "" : "闸门放行了主 agent 实测会假绿的那格带子");
    c.chk("seam 闸门：错误串带读数 0.2390 + 下界 0.0570 + 行 51 + 带子 [160,1440)（可直接归因）",
        gateMsg.find("0.2390") != std::string::npos && gateMsg.find("0.0570") != std::string::npos
            && gateMsg.find("x∈[160,1440)") != std::string::npos
            && gateMsg.find("行 51") != std::string::npos,
        gateMsg.empty() ? "闸门根本没抛" : gateMsg.substr(0, 100));
    // ── 三条判决线必须挂号（改名漏挂 = verdictFor 运行期才炸，太晚）─────────────
    for (const char* name : { "shadow-seam-lod-growth", "shadow-cascade-seam-arms-alive",
                             "shadow-seam-sweep-monotone" }) {
        bool found = false;
        for (const auto& j : registeredJudgments())
            found = found || std::string(j.name) == name;
        c.chk("seam 判据已挂号进注册表", found, name);
    }
}

/**
 * 信号在场面：`coverageFraction` 的闭式刻度 + 两道「阴影信号在场」闸门。
 *
 * 这一面钉的是**这台差分仪器会不会因为分母失效而永久全绿**（2026-10-19 主 agent 实测：把
 * `seamCovField(img[i], img[0])` 的分母换成分子自身 ⟹ cov ≡ 0 ⟹ 三条判决线全绿 rc=0，
 * 带子闸门也放行）。cov ≡ 0 时**所有差分统计量同时失明**（两臂之差也 ≡ 0），所以这一面必须
 * 有**非差分**的判据：闸门 A 问「分母与分子是不是同一张图」（布尔、无阈值），闸门 B 问
 * 「单臂的有影面积是不是零」（带闭式下界）。两道各有一块对方照不到的盲区，理由住
 * `shadow_seam_metric.h` 的两处声明。
 *
 * 变异红线（实跑记录见交付说明 X5）：
 *   · 闸门 A 的比较恒真化（永不抛）→ 「diff=0 应抛」红；实机上 X2 那个变异重新变成全绿 rc=0。
 *   · 闸门 A 的 `diff > 0` 写反成 `diff >= 0`（= 永不放行）→ 「正常态不抛」红（仪器永久红）。
 *   · 闸门 B 的下界挪到 0.0 → 「低于下界应抛」红（0.005 ≥ 0 就不抛了）。
 *   · 闸门 B 的判据从「cov > 0.5 的**占比**」换成「cov **均值**」→ 「常数 0.6 场 → 恰 1.0」
 *     红（均值会给 0.6）。这条断言就是两种口径的分家点，也是 X5③ 要解释的东西。
 *   · 两道闸门的空清单守卫删掉 → 对应那条「空清单应抛」红（空清单平凡通过 = 闸门看起来在、
 *     其实什么都没查，与恒绿同罪）。
 */
void testShadowSeamSignalGates(Checker& c)
{
    const cv::Rect full8(0, 0, 8, 8);
    // ── coverageFraction 的闭式刻度（期望全部来自定义，不吃实机）────────────────
    c.chk("seam 有影面积：全 0 场（= 分母死的闭式态）→ 恰 0",
        coverageFraction(field(8, 8, fZero), full8) == 0.0);
    c.chk("seam 有影面积：常数 0.6 场 → 恰 **1.0**（是**占比**不是均值，均值会给 0.6）",
        coverageFraction(field(8, 8, fSix), full8) == 1.0,
        "这条就是「占比 vs 均值」两种口径的分家点");
    c.chk("seam 有影面积：右半幅全影 → 全幅恰 0.5（分母 = band 像素数，闭式分数）",
        coverageFraction(field(8, 8, fRightShadow), full8) == 0.5);
    c.chk("seam 有影面积：同场取左半 band → 恰 0（band 既是分子也是分母，band 外不许掺进来）",
        coverageFraction(field(8, 8, fRightShadow), cv::Rect(0, 0, 4, 8)) == 0.0);
    c.chk("seam 有影面积：同场取右半 band → 恰 1（上一条的反向半边，只钉一半会被读成整条已验）",
        coverageFraction(field(8, 8, fRightShadow), cv::Rect(4, 0, 4, 8)) == 1.0);
    c.chk("seam 有影面积：cov **恰等于** 0.5 不计（严格 >，半影缘以内的错位不算有影）",
        coverageFraction(field(8, 8, fHalfBin), full8) == 0.0);

    // ── 闸门 A｜分母臂在场（布尔，无阈值）─────────────────────────────────────
    const auto alive
        = std::vector<std::pair<std::string, std::int64_t>> { { "d5f0", 45251 }, { "d-1f1", 7 } };
    c.chk("seam 闸门A：每条臂与分母都逐位不同 → 不抛（正常态被拦死 = 仪器永久红）",
        !throwsStd([&] { requireDenominatorAlive(alive); }));
    const auto oneDead
        = std::vector<std::pair<std::string, std::int64_t>> { { "d5f0", 0 }, { "d-1f1", 45251 } };
    std::string msgA;
    bool threwA = false;
    try {
        requireDenominatorAlive(oneDead);
    } catch (const std::exception& e) {
        threwA = true;
        msgA = e.what();
    }
    c.chk("seam 闸门A：有一条臂与分母逐位相同 → 抛（分母失效的形态，X2/X3 靠它）", threwA,
        threwA ? "" : "闸门放行了「cov ≡ 0 ⟹ 三条判决线全绿」那个失效");
    c.chk("seam 闸门A：错误串点名死的那条臂、不误指活臂（可直接归因）",
        msgA.find("d5f0") != std::string::npos && msgA.find("d-1f1（") == std::string::npos, msgA.substr(0, 90));
    const auto twoDead
        = std::vector<std::pair<std::string, std::int64_t>> { { "d5f0", 0 }, { "d7f1", 0 } };
    std::string msgA2;
    try {
        requireDenominatorAlive(twoDead);
    } catch (const std::exception& e) {
        msgA2 = e.what();
    }
    c.chk("seam 闸门A：两条同时死 → 一次报全（逐条抛会把第二条藏到下一轮）",
        msgA2.find("d5f0") != std::string::npos && msgA2.find("d7f1") != std::string::npos,
        msgA2.substr(0, 90));
    c.chk("seam 闸门A：空清单 → 抛（空清单平凡通过 = 闸门看起来在、其实什么都没查）",
        throwsStd([&] { requireDenominatorAlive({}); }));

    // ── 闸门 B｜覆盖度非零（带闭式下界）──────────────────────────────────────
    // 下界一律吃 `kSeamCoverageFloor` **本身**（同带子闸门的处理）：把 1% 挪到 0.0 的变异必须
    // 打掉「低于下界应抛」那条，否则界被谁悄悄改大 selftest 也听不见。
    const auto okB = std::vector<std::pair<std::string, double>> { { "d5f0", 0.486 }, { "d5f1", 0.02 } };
    c.chk("seam 闸门B：有影面积都 ≥ 下界 → 不抛（48.6% 与 2% 两档都在界上）",
        !throwsStd([&] { requireCoveragePresent(okB, kSeamCoverageFloor); }));
    c.chk("seam 闸门B：读数**恰等于**下界 → 不抛（含端点，与 J1 的 ≤ 同口径）",
        !throwsStd([&] { requireCoveragePresent({ { "d5f0", kSeamCoverageFloor } }, kSeamCoverageFloor); }));
    std::string msgB;
    bool threwB = false;
    try {
        requireCoveragePresent({ { "d5f0", 0.005 } }, kSeamCoverageFloor);  // 0.5% < 1%
    } catch (const std::exception& e) {
        threwB = true;
        msgB = e.what();
    }
    c.chk("seam 闸门B：0.005 < 0.01 → 抛（下界挪到 0.0 的变异在这里红）", threwB,
        threwB ? "" : "下界形同不存在，cov ≡ 0 会直接放行");
    c.chk("seam 闸门B：错误串同时带读数 0.0050 与下界 0.0100（可归因，不是空喊「闸门失败」）",
        msgB.find("0.0050") != std::string::npos && msgB.find("0.0100") != std::string::npos
            && msgB.find("d5f0") != std::string::npos,
        msgB.substr(0, 90));
    c.chk("seam 闸门B：空清单 → 抛（同闸门A：平凡通过的闸门比没有闸门更坏）",
        throwsStd([&] { requireCoveragePresent({}, kSeamCoverageFloor); }));
}

}  // namespace RGL::imgqa
