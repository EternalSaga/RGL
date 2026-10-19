// shadow_seam_metric.cpp —— `shadow-cascade-seam` 的**度量口径层**实现（覆盖度场 + 翻转率 + 逐行台阶）。
//
// 缝与全部口径理由见 `shadow_seam_metric.h` 头注释。本文件只放实现；三条算子（覆盖度 /
// 翻转率 / 最硬横向台阶）的期望值全部来自闭式（`selftest_seam.cpp`：常数场 = 恰 0、
// 单条 0.25 台阶 = 恰 0.25、宽 33 斜坡在 halfWidth=1 下 = 恰 1/33、cov 的线性域锚点 =
// 闭式 0.78406109…），实机截图只证明「与上次一样」，闭式才证明「算子算的是我说的那个量」。
//
// 参考实现 = 主 agent 定口径时用的 Python 脚本（`/tmp/opencode/d139/{seam,diff_metric}.py`：
// sRGB 解码逐式同 `srgbDecode`；台阶 = `np.percentile(np.abs(dn - up), 90)`，up/dn 是交界行
// 上下各 6 行的**逐列中位**）。本实现与它逐式同，别「顺手优化」成别的形态（比如把两窗中位
// 之差换成逐像素差——那会把「缝」与「影场自己沿 y 的自然梯度」混为一谈，闭式 1/33 立刻不成立）。
//
// 🔴 守卫一律**抛**，不许返回 0：返回 0 会被上层读成「分歧为零 = 判据通过」，而它真正的意思
// 是「我没算」。这与 `shadow_grain_metric.cpp` 的「闸门 vs 判据」分工同一条：判据可以红
// （红 = 有缺陷，是结论），守卫抛 = 「这台仪器今天没出结论」（rc=3，不是结论，不会被误读）。
#include "shadow_seam_metric.h"
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>

namespace RGL::imgqa {
namespace {

/// 小窗中位数（**偶数个取中间两个的平均**，与 numpy `np.median` 同口径——参考实现吃的
/// 就是它，改成「取下中位」会让 halfWidth 为偶数的全部读数漂半个纹素）。
/// 调用方持有 scratch（每列复用同一块内存，841 行 × 800 列 × 2 窗不重新分配）。
double medianInplace(std::vector<double>& v)
{
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 == 1 ? v[n / 2] : 0.5 * (v[n / 2 - 1] + v[n / 2]);
}

/// 两条 cov 场的公共前提检查（尺寸 / 类型 / band 非空 / band 在图内）。**抛**，理由见文件头。
void requireFields(const cv::Mat& a, const cv::Mat& b, const cv::Rect& band, const char* who)
{
    if (a.size() != b.size() || a.type() != CV_64FC1 || b.type() != CV_64FC1)
        throw std::runtime_error(std::string(who)
            + "：两图必须同尺寸 CV_64FC1（seamCovField 产出的覆盖度场）。尺寸不同 = 两臂不是"
              "同一帧的两次采集，分歧率没有意义；返回 0 会被读成「零分歧 = 通过」，拒绝");
    if (band.width <= 0 || band.height <= 0)
        throw std::runtime_error(std::string(who)
            + "：band 为空（宽/高 ≤ 0）。空分母的「0 分歧」是假绿——它既可能是没有分歧，"
              "也可能是根本没数，两者必须可区分");
    if (band.x < 0 || band.y < 0 || band.x + band.width > a.cols || band.y + band.height > a.rows)
        throw std::runtime_error(std::string(who)
            + "：band 越界（" + std::to_string(band.x) + "," + std::to_string(band.y) + " "
              + std::to_string(band.width) + "x" + std::to_string(band.height) + " 不在 "
              + std::to_string(a.cols) + "x" + std::to_string(a.rows)
              + " 内）。夹到图像边缘算出来的台阶/分歧率是边缘截断的产物，不是画面的产物");
}

/// 数字进错误串的固定形态（**4 位小数**）：界是 0.057 这种三位小数、读数常也是三位，取 4 位
/// 保证「读数」与「界」在消息里不会打成形同一个数，selftest 才能分别断言两个串都在场。
std::string num4(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.4f", v);
    return buf;
}

}  // namespace

cv::Mat seamCovField(const cv::Mat& onBgr, const cv::Mat& offBgr)
{
    if (onBgr.size() != offBgr.size() || onBgr.type() != CV_8UC3 || offBgr.type() != CV_8UC3)
        throw std::runtime_error("seamCovField: 两图必须同尺寸 CV_8UC3（on = 钉级 csm 臂、"
              "off = 同机位同帧的 mode=none 臂，不是同一帧的两次采集，覆盖度差没有意义）");
    cv::Mat cov(onBgr.rows, onBgr.cols, CV_64FC1);
    for (int y = 0; y < onBgr.rows; ++y) {
        const auto* on = onBgr.ptr<cv::Vec3b>(y);
        const auto* off = offBgr.ptr<cv::Vec3b>(y);
        auto* out = cov.ptr<double>(y);
        for (int x = 0; x < onBgr.cols; ++x) {
            // 三通道**均值**（不是 max、不是 luma），与 `shadow_grain_metric.cpp` 同口径。
            // 🔴 顺序是**逐通道 clip → 再取均值**，不是"先取均值再 clip"：两者只在「某通道
            // 变亮」的像素上分家（off=128 灰、on=(255,0,0) 时给 0.6666 vs 0.0000，selftest
            // 钉住）。债-110 的一次性参照脚本用的是后者，本机交付读数因此差 +0.014pp
            // （1.304% vs 1.290%，在 ±0.05pp 容差内）——**别"顺手改回去"**，那会让本仪器与
            // grain 分家，而两台的 cov 场必须同口径才能互相解释。
            double sum = 0.0;
            for (int c = 0; c < 3; ++c) {
                // 🔴 先 sRGB 解码再算比值（硬约束第 2 条「统计一律线性域」）。字节域里
                // 同一个绝对差在暗部/亮部是完全不同的物理量，cov 的 0.5 半影线会随亮度漂移。
                const double o = srgbDecode(off[x][c] / 255.0);
                const double n = srgbDecode(on[x][c] / 255.0);
                // clip 下界 0：on 比 off 亮（变亮）不算「被遮」，负值进均值会把隔壁通道的真
                // 遮挡抵掉。上界 1 是**防御性**的，线性域里可证不触发：n ≥ 0 ⟹ (o−n)/(o+eps)
                // ≤ o/(o+eps) < 1（所以 selftest 里没有、也不该有"上界生效"的用例——那种断言
                // 只能靠伪造输入成立，是假覆盖）。
                sum += std::clamp((o - n) / (o + kSeamEps), 0.0, 1.0);
            }
            out[x] = sum / 3.0;
        }
    }
    return cov;
}

double flipFraction(const cv::Mat& a, const cv::Mat& b, const cv::Rect& band, double thresh)
{
    requireFields(a, b, band, "flipFraction");
    int64_t hit = 0;
    for (int y = band.y; y < band.y + band.height; ++y) {
        const auto* pa = a.ptr<double>(y);
        const auto* pb = b.ptr<double>(y);
        for (int x = band.x; x < band.x + band.width; ++x)
            hit += std::abs(pa[x] - pb[x]) > thresh ? 1 : 0;  // 严格 >：恰等于阈值不算翻转
    }
    // 分母 = band 的像素数（不是整幅！整幅会把 band 外的「相同」算进分母 = 稀释读数）
    return static_cast<double>(hit) / static_cast<double>(band.width)
        / static_cast<double>(band.height);
}

SeamStep maxRowStep(const cv::Mat& dif, const cv::Rect& band, int halfWidth)
{
    if (dif.type() != CV_64FC1)
        throw std::runtime_error("maxRowStep: dif 必须是两幅 seamCovField 产出相减后的 CV_64FC1");
    if (halfWidth <= 0)
        throw std::runtime_error("maxRowStep: halfWidth 必须 ≥ 1（收到 "
            + std::to_string(halfWidth)
            + "）。0 宽的窗没有中位数，返回 0 会被读成「没有台阶 = 判据通过」，拒绝");
    requireFields(dif, dif, band, "maxRowStep");
    // 上下两窗 [y−h, y) 与 [y, y+h) 必须整个落在图内：越界的窗会**静默变短**，
    // 短窗的中位数与满窗不同口径 = 边界行的台阶被换了一把尺量（然后被 max 选中）。
    if (band.y < halfWidth || band.y + band.height + halfWidth > dif.rows + 1)
        throw std::runtime_error("maxRowStep: band 的行范围容不下 ±halfWidth 的窗（band.y="
            + std::to_string(band.y) + " 高 " + std::to_string(band.height) + " 图高 "
            + std::to_string(dif.rows) + " halfWidth=" + std::to_string(halfWidth)
            + "）。要么收 band，要么换机位，不许把越界的窗当「窗短一点没关系」");

    SeamStep best;
    std::vector<double> up;
    std::vector<double> dn;
    std::vector<double> cols;  // 本行逐列的 |台阶|，排序后取 p90
    up.reserve(static_cast<size_t>(halfWidth));
    dn.reserve(static_cast<size_t>(halfWidth));
    cols.resize(static_cast<size_t>(band.width));
    for (int y = band.y; y < band.y + band.height; ++y) {
        for (int i = 0; i < band.width; ++i) {
            const int x = band.x + i;
            up.clear();
            dn.clear();
            for (int k = 1; k <= halfWidth; ++k)
                up.push_back(dif.at<double>(y - k, x));
            for (int k = 0; k < halfWidth; ++k)
                dn.push_back(dif.at<double>(y + k, x));
            cols[static_cast<size_t>(i)] = std::abs(medianInplace(dn) - medianInplace(up));
        }
        std::sort(cols.begin(), cols.end());
        // percentile100：空集会抛，但 band.width ≥ 1 已由 requireFields 保证非空
        const double p90 = percentile100(cols, 90.0);
        if (p90 > best.step) {  // 严格 >：并列时保留**第一个**最硬行（可复现的平票规则）
            best.step = p90;
            best.row = y;
        }
    }
    return best;
}

/**
 * 参照臂有效性闸门（**量具自检，不是画质判据**）。完整理由（为什么这条界闭式可辩护、
 * 不装会发生什么、边界为什么含端点）住 `shadow_seam_metric.h` 的声明处，本处只补一条：
 *
 * 🔴 它必须**先于三条判决线**跑，且失效时**不产出任何 Verdict**。J1 的形状是「两臂台阶之差
 * ≤ 上限」，而参照臂那一侧本应是**常数**（闭式 0，最多一个纹素）；它一旦不是常数，"差"就
 * 不再是 LOD 的归因量——这时出一句绿会被读成「LOD 没造缝」，出一句红会被读成「LOD 造了缝」，
 * **两个方向都是假线索**。所以这里只有抛（rc=3 = 今天没出结论），没有红。
 */
void requireBandValid(double refStep, int refRow, const cv::Rect& band, double floor)
{
    // 含端点：等于界算通过（`floor` 是"允许的最大纹素错位"这个闭式上界，落在界上仍属于
    // "只可能来自纹素量化"的集合；与 J1 自己的 `≤` 同口径）。selftest 钉这一格。
    if (!(refStep > floor))
        return;
    throw std::runtime_error(
        "参照臂有效性闸门：**带子已失效，J1 读数不可归因，拒绝出判据**\n"
        "  带子 x∈["
        + std::to_string(band.x) + "," + std::to_string(band.x + band.width) + ") × 行 ["
        + std::to_string(band.y) + "," + std::to_string(band.y + band.height - 1)
        + "] 内，参照臂台阶 " + num4(refStep) + " > 固有下界 " + num4(floor)
        + "（台阶出现在行 " + std::to_string(refRow) + "）。\n"
        "  参照臂 = 四级同一套全量几何（LOD 关）⟹ 跨级分歧**只可能**来自纹素量化，而一个纹素\n"
        "  折成 cov 台阶就是上面那个界 ⟹ 超界 = 带子里混进了**不是纹素量化**的跨级分歧。\n"
        "  首要嫌疑 = `rgl/shaders/shadow.slang` ④ 出块旁路：远端像素被钉进近级后落在该级盒子\n"
        "  外、返回全亮，而这在**两个臂里都**造台阶；`maxRowStep` 取的是逐行最大，最大项一旦被\n"
        "  污染行抢走，缝的信号就从统计量里整个消失，剩下的两臂残差盖不住上限 = **缺陷在场的\n"
        "  静默假绿**（2026-10-19 实测：同一批 13 臂图只换带子，x∈[160,1440) 给参照臂 0.239 @ 行\n"
        "  51、J1 差 +0.088 ≤ 0.1 ⟹ 绿；x∈[200,1400) 给 0.320 @ 行 51、差 +0.030 ⟹ 绿）。\n"
        "  下一步（按成本排序）：① 用 `csm.force_cascade` 把该带子逐行钉级复现，看污染行的台阶\n"
        "  在两臂里是不是同一位置（同位置 = 共模污染，与 LOD 无关）；② 查 ④ 出块旁路在该带内的\n"
        "  占比；③ 收窄带子到缝所在列段——但**必须重推**固有下界（相邻级纹素比随取景与出厂 D\n"
        "  变，界跟着带子走，不是想缩就缩）。⚠ 本闸门是量具自检，它抛**不代表**阴影变好或变坏。");
}

double coverageFraction(const cv::Mat& cov, const cv::Rect& band, double thresh)
{
    requireFields(cov, cov, band, "coverageFraction");  // 单场：两参数同图，查的是类型与 band
    int64_t hit = 0;
    for (int y = band.y; y < band.y + band.height; ++y) {
        const auto* p = cov.ptr<double>(y);
        for (int x = band.x; x < band.x + band.width; ++x)
            hit += p[x] > thresh ? 1 : 0;  // 严格 >：与 flipFraction 同口径
    }
    return static_cast<double>(hit) / static_cast<double>(band.width)
        / static_cast<double>(band.height);
}

void requireDenominatorAlive(const std::vector<std::pair<std::string, std::int64_t>>& diffByArm)
{
    if (diffByArm.empty())
        throw std::runtime_error(
            "分母臂在场闸门：一条 csm 臂都没拿到（空清单）。空清单会**平凡通过** = 闸门看起来"
            "在、其实什么都没查，拒绝放行");
    std::string dead;
    for (const auto& [name, diff] : diffByArm) {
        if (diff > 0)
            continue;
        dead += (dead.empty() ? "" : "、") + name + "（与分母臂逐位相同）";
    }
    if (dead.empty())
        return;
    throw std::runtime_error(
        "分母臂在场闸门：**cov 的公共分母（mode=none 臂）没让画面少过一个像素，全部覆盖度"
        "读数作废**\n"
        "  逐位相同的 csm 臂："
        + dead
        + "。\n"
        "  为什么这条是**布尔**判据（没有阈值可谈）：闭式只有两个态——分母臂与 csm 臂逐位相同\n"
        "  = 阴影项对画面一个像素都没改过 = 分母死了；不同 = 阴影在画。中间没有「差一点点」。\n"
        "  分母一失效 ⟹ 每一臂 cov ≡ 0 ⟹ Δ ≡ 0 ⟹ **三条判决线一起变绿**（J2 也不吃分母，\n"
        "  照样绿；带子闸门判的是参照臂台阶 ≤ 界，0 ≤ 界，照样放行）。2026-10-19 实测过这个\n"
        "  失效形态：把 seamCovField 的分母换成分子自身 ⟹ 全绿 rc=0。\n"
        "  三种成因逐个查（按可能性排序）：① `shadow:mode=\"none\"` 没真的关掉阴影段 ⟹ 去引擎\n"
        "  日志看该臂的 `shadow: mode=` 回显，并数阴影段的 draw 数（该为 0）；② 阴影段**上游**\n"
        "  被禁（izb 门 / 形态决策表那条链，`rgl/shadow_config_impl.cppm`）⟹ 两臂都拿不到阴影，\n"
        "  于是两臂相同；③ 这个机位/场景本来没影 ⟹ 换一台有影的机位（本仪器钉的是债-139 第三\n"
        "  机位，它的影是实测过的）。⚠ 本闸门抓不到「两臂图不同、但差异不在阴影上」——那是\n"
        "  覆盖度非零闸门的活，两道都要过。");
}

void requireCoveragePresent(const std::vector<std::pair<std::string, double>>& fractionByArm,
    double floor)
{
    if (fractionByArm.empty())
        throw std::runtime_error(
            "覆盖度非零闸门：一条 csm 臂都没拿到（空清单）。空清单平凡通过 = 闸门什么都没查，拒绝放行");
    std::string low;
    for (const auto& [name, fraction] : fractionByArm) {
        if (fraction >= floor)
            continue;  // 含端点：等于下界算通过（与 J1 的 ≤、带子闸门的 > 同口径）
        low += (low.empty() ? "" : "、") + name + " " + num4(fraction);
    }
    if (low.empty())
        return;
    throw std::runtime_error(
        "覆盖度非零闸门：**cov 场几乎全零 = 阴影项没进画面，全部差分读数作废**\n"
        "  有影面积（cov > 0.5 的像素占比）低于下界 "
        + num4(floor) + " 的臂：" + low
        + "。\n"
        "  界为什么是闭式可辩护的：死态的有影面积是**恰 0%**（`cov ≡ 0`），不是「小于某个小数」；\n"
        "  本靶正常读数在 48.6% 量级（主 agent 定口径时记的「有影面积 48.55%」）⟹ 取 1% 离死态\n"
        "  一个数量级以上、离正常读数 ~48 倍，只在「阴影几乎没画」时响。换机位/换场景要重推。\n"
        "  与分母闸门的分工：那道查「分母与 csm 臂逐位相同」，本道查「两臂图即便不同、差异也\n"
        "  不在阴影上」（分母被换成一张无关的图；「这个机位本来没影」也会落到这里，只是那种\n"
        "  形态分母闸门会先响并给出更具体的归因）。差分统计量对这类失效是结构性失明的——\n"
        "  cov ≡ 0 时两臂之差也 ≡ 0，所有差分判据一起绿。\n"
        "  下一步：① 看 none 臂与 csm 臂的 RGB 差分图（有影边形状 = 阴影在画面里，问题出在 cov\n"
        "  的分母；一片噪声 = 阴影根本没进图）；② 数阴影段 draw 数并读 `shadow: mode=` / `izb.on=`\n"
        "  回显；③ 换一台有影的机位。");
}

}  // namespace RGL::imgqa
