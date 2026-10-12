// `imgqa selftest` · **diffblock 段**（★0.5-b1）——形态判据的五条阳性锚点 + τ 语义 +
// erode↔distanceTransform 等价性自检（锚点清单 = docs/mdi_plan.md §11.2，缺任一条算子不许入账）。
//
// 缝与被测仪器：这里打的是 `diff_block_metric.h` 的口径层（合成亮度图 → 读数，不读 PNG
// 不启引擎，同 `testDiffStructMetric` 的打法）。每条锚点都写了「算子坏成什么样这条会红」
// ——反向可失败是这批锚点的入账条件，写不出失效形态的锚点 = 装饰。
//
// ⚠ 锚点 (c) 的 r=1/r=2 结果按 §11.2 公式实现（3px 线在 r=1 **FAIL**、r=2 **PASS**）：
//   任务下发文本写的是「r=1 PASS、r=2 FAIL」，与公式 U==0 ⟺ 厚度 ≤ 2r 矛盾（3 > 2·1 ⟹
//   r=1 必 FAIL；erode 与 distanceTransform 两实现实测一致，见 diff_block_metric.h 头）。
//   修正后的 (c) 才钉得住「r 是半径不是直径」：若实现把 r 当直径（U=#{d>2r}），r=1 会
//   错误地绿——本锚点在错误实现下**必红**。
//   ✅ **主 agent 已拍板：按公式，任务文本作废**（§11.2(c) 已就地改正，不留双层）。
//
// 合成图约定：底色 luma 0.5，差异区 0.7（Δ=0.2 ≫ τ=1 的台阶 2.2e-5）⟹ M **恰等于**画出来的
// 形状——锚点钉的是形态，不是阈值。形状一律不贴图边（贴边处 distanceTransform 与 erode 的
// 边界约定本就不同，那是定义域不是 bug，理由 → diff_block_metric.h 头注释）。
//
// 拆四个 testXxx 是认知复杂度护栏逼的（合写 25 > 15），缝按锚点组切：
//   · `testDiffBlockAnchors`          = (a) 1px 线 + (b) 20×20 块 + 曲线自检 U(0)≡|M|/单调
//   · `testDiffBlockThinShapes`       = (d) 对角/阶梯线 + (e) 整幅平移 1px（两条「薄 vs 厚」）
//   · `testDiffBlockRadiusSemantics`  = (c) 3px 线钉 r=半径 + τ 的两个计数语义
//   · `testDiffBlockErodeEquivalence` = §3 等价性自检 + 「容差吞不掉直径错」反向用例
//   · `testDiffBlockErodeLaw`         = 腐蚀面积闭式定律 `U(r) = (20−2r)²`（逐位精确）
//   · `testDiffBlockReportSurface`    = **报告面**（分位数统计域 / 连通域降序 / --top 截断）
//     ——主 agent 质检补：两条报告面变异在补它之前**全部存活**（都不动 U，判据全绿而报告骗人）
#include "selftest.h"
#include "diff_block_metric.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <string>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
namespace {

/// 相对差的短格式（等价性断言的 extra 读数）。
std::string fmtRel(double v)
{
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.4f", v);
    return buf;
}

/// 合成图对：mask 指定差异区（CV_8UC1，255=差）。byteDiff 一并给出（字节域 = 同一形状），
/// 让「任一位不同」计数在锚点里也是可断言的数而不是 -1。
struct SyntheticPair {
    cv::Mat lumaA, lumaB, mask;
    [[nodiscard]] cv::Mat byteDiff() const
    {
        cv::Mat bd;
        cv::compare(lumaA, lumaB, bd, cv::CMP_NE);
        return bd;
    }
};

SyntheticPair paintPair(const cv::Size& sz, const std::function<void(cv::Mat&)>& paint)
{
    SyntheticPair p;
    p.mask = cv::Mat::zeros(sz, CV_8UC1);
    paint(p.mask);
    p.lumaA = cv::Mat(sz, CV_64FC1, cv::Scalar(0.5));
    p.lumaB = p.lumaA.clone();
    p.lumaB.setTo(0.7, p.mask);
    return p;
}

/// 默认 τ、指定 r、top 给足（锚点要看 bbox，不能被截断）。
DiffBlockStat statAt(const SyntheticPair& p, int radius, int top = 8)
{
    return diffBlockStat(p.lumaA, p.lumaB, p.byteDiff(), kDiffBlockDefaultTau, radius, top);
}

/// 棋盘对（锚点 (e) 的靶）：A = 1px 棋盘格，B = **内部相位翻转** = A 平移 1px 的差分形态
/// （每个内部像素都翻）。留 1px 同值边框：M 不贴图边（边界约定见文件头）。
void paintShiftedChecker(const cv::Size& sz, cv::Mat& a, cv::Mat& b)
{
    a = cv::Mat(sz, CV_64FC1);
    b = cv::Mat(sz, CV_64FC1);
    for (int y = 0; y < sz.height; ++y) {
        for (int x = 0; x < sz.width; ++x) {
            const bool light = ((x + y) & 1) != 0;
            a.at<double>(y, x) = light ? 0.7 : 0.3;
            const bool inner
                = x > 0 && y > 0 && x + 1 < sz.width && y + 1 < sz.height;
            b.at<double>(y, x) = (inner != light) ? 0.7 : 0.3;
        }
    }
}

}  // namespace

/// 锚点 (a)(b) + 曲线自检 U(0) ≡ |M| 与 U(r) 单调。
void testDiffBlockAnchors(Checker& c)
{
    const cv::Size sz(96, 96);

    // (a) 沿一条「边缘」画 1px 线 → **PASS**（默认 r=2 与 r=1 都绿：1px 线处处 d=1）。
    //     坏成什么样会红：① τ=0 语义写成 ≥ 0 → M = 整幅图 → 恒 FAIL（τ 锚点同红）；
    //     ② 距离变换吃反掩码（背景当差异）→ 整幅厚差异 → FAIL；③ 阈值 τ 单位错成 1.0
    //     （一个「整亮度」）→ M 空、厚度分位无定义，②③ 都会把「缝该放行」读成红或把
    //     「整块该红」读成绿。
    {
        const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
            m(cv::Range(47, 48), cv::Range(10, 86)) = 255;
        });
        const DiffBlockStat st2 = statAt(p, kDiffBlockDefaultRadius);
        c.chk("diffblock(a)：1px 线默认 r=2 → U=0 且 M 恰 = 线长 76",
            st2.uAtRadius == 0 && st2.mPixels == 76,
            std::to_string(st2.uAtRadius) + "/" + std::to_string(st2.mPixels));
        c.chk("diffblock(a)：1px 线 r=1 也 PASS（1px 缝是判据要放行的主形态）",
            statAt(p, 1).uAtRadius == 0);
        c.chk("diffblock(a)：1px 线厚度 max ≈ 1（d 的边界约定：直邻 = 1，非 0 非 2）",
            st2.thicknessValid && st2.thicknessMax > 0.9 && st2.thicknessMax < 1.1,
            std::to_string(st2.thicknessMax));
    }

    // (b) 平坦区 20×20 实心块 → **FAIL 且 bbox 报对**（§11.2 输出③的归因面：人只看这几个框）。
    //     坏成什么样会红：① 腐蚀半径当直径 → r=2 也绿（U=#{d>4}=0）；② 连通域按 4-连通 +
    //     bbox 抄错 stats 列（LEFT/TOP/WIDTH/HEIGHT 错位）→ 框不对；③ 面积/厚度串位。
    {
        const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
            m(cv::Range(30, 50), cv::Range(30, 50)) = 255;
        });
        const DiffBlockStat st = statAt(p, kDiffBlockDefaultRadius);
        c.chk("diffblock(b)：20×20 块 r=2 → U>0（FAIL 形态）且 M 恰 = 400",
            st.uAtRadius > 0 && st.mPixels == 400, std::to_string(st.uAtRadius));
        c.chk("diffblock(b)：连通域 #1 = area 400 / bbox [30,30,20,20]（归因框必须报对）",
            st.components.size() == 1 && st.components[0].area == 400 && st.components[0].x == 30
                && st.components[0].y == 30 && st.components[0].width == 20
                && st.components[0].height == 20,
            st.components.empty() ? "无域" : std::to_string(st.components[0].area));
        // 20×20 块中心到最近背景 = 10（行 39/40 到行 29/50）；(1,√2) 近似低估 ~4% ⟹ 断言区间。
        c.chk("diffblock(b)：块最大厚度 ∈ (9,10.5)（闭式：中心 d=10，近似容差内）",
            st.components.size() == 1 && st.components[0].maxThickness > 9.0
                && st.components[0].maxThickness < 10.5,
            st.components.empty() ? "无域" : std::to_string(st.components[0].maxThickness));
    }

    // 曲线自检：U(0) ≡ |M|（r=0 的腐蚀 = 恒等，曲线档位与 M 计数必须互洽——两遍独立计数，
    // 坏一个就红；也是「距离变换吃反掩码」这类接线错的第二道闸）。U(r) 单调不增是「r=4 才
    // 归零 = 稀释一眼可见」的前提。
    {
        const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
            m(cv::Range(20, 40), cv::Range(20, 40)) = 255;
            m(cv::Range(60, 63), cv::Range(60, 90)) = 255;
        });
        const DiffBlockStat st = statAt(p, kDiffBlockDefaultRadius);
        c.chk("diffblock：U(r=0) 恒 = |M|（曲线档位与 M 计数互洽）",
            !st.uCurve.empty() && st.uCurve.front().first == 0
                && st.uCurve.front().second == st.mPixels,
            std::to_string(st.uCurve.front().second));
        bool monotonic = true;
        for (size_t i = 1; i < st.uCurve.size(); ++i)
            monotonic = monotonic && st.uCurve[i].second <= st.uCurve[i - 1].second;
        c.chk("diffblock：U(r) 随 r 单调不增（判据稀释一眼可见的前提）", monotonic);
    }
}

/// 锚点 (d)(e)：两条「薄 vs 厚」——对角阶梯线必须放行，整幅错位必须红。
void testDiffBlockThinShapes(Checker& c)
{
    const cv::Size sz(96, 96);

    // (d) 对角 1px 阶梯线 → **PASS**（8-连通 / 结构元的经典坑）。
    //     坏成什么样会红：① 连通域用 4-连通 → 对角线碎成 N 个域（componentCount 红）；
    //     ② 结构元取方形（含对角偏移）且判据写成 d ≥ r → 对角线被算出「内部」→ FAIL；
    //     ③ 阶梯（每 2px 一台阶）与纯对角都钉，只钉一条时另一种画法的坑漏守。
    {
        const SyntheticPair diag = paintPair(sz, [](cv::Mat& m) {
            for (int i = 10; i < 86; ++i)
                m.at<uchar>(i, i) = 255;
        });
        const DiffBlockStat st = statAt(diag, kDiffBlockDefaultRadius);
        c.chk("diffblock(d)：对角 1px 线 r=2 PASS 且 8-连通计为 1 个域",
            st.uAtRadius == 0 && st.componentCount == 1,
            std::to_string(st.uAtRadius) + "/" + std::to_string(st.componentCount));
        const SyntheticPair stair = paintPair(sz, [](cv::Mat& m) {
            for (int i = 0; i < 20; ++i)
                m(cv::Range(12 + i, 13 + i), cv::Range(12 + 2 * i, 14 + 2 * i)) = 255;
        });
        const DiffBlockStat stS = statAt(stair, 1);
        c.chk("diffblock(d)：1px 阶梯线 r=1 PASS（台阶拐角不算内部）",
            stS.uAtRadius == 0 && stS.componentCount == 1,
            std::to_string(stS.uAtRadius) + "/" + std::to_string(stS.componentCount));
    }

    // (e) **整幅平移 1px** → **FAIL**（「整块错」最真实的形态之一：几何整体错位）。
    //     靶 = 1px 棋盘格（处处高频纹理），B = A 平移 1px ⟹ 相位翻转 ⟹ 内部像素**全部**不同。
    //     坏成什么样会红：算子若对「M 铺满全图」都给绿（U 恒 0 的接线错、距离变换根本没跑），
    //     这条是全表唯一的全图级 FAIL 锚点，必红。
    //     ⚠ 设计边界（不是本锚点的失效）：**自然图像**平移 1px 只在边缘产生 1–2px 薄带 ⟹
    //     本判据 PASS——「整块错」的主判据是零阈值的 gbuffer 逐字节对拍（§11.1），本条钉的是
    //     「差异成厚块时判据必须红」，不是「一切平移都红」。
    {
        cv::Mat a, b;
        paintShiftedChecker(sz, a, b);
        const DiffBlockStat st
            = diffBlockStat(a, b, cv::Mat(), kDiffBlockDefaultTau, kDiffBlockDefaultRadius, 1);
        c.chk("diffblock(e)：整幅平移 1px（棋盘靶）→ r=2 FAIL 且 M = 内部 94×94",
            st.uAtRadius > 0 && st.mPixels == 94 * 94,
            std::to_string(st.uAtRadius) + "/" + std::to_string(st.mPixels));
        c.chk("diffblock(e)：byteDiff 缺席时「任一位不同」报 -1（未测 ≠ 0，NaN 纪律）",
            st.anyDiffPixels == -1, std::to_string(st.anyDiffPixels));
    }
}

/// 锚点 (c)「r 是半径不是直径」+ τ 的两个计数语义。
void testDiffBlockRadiusSemantics(Checker& c)
{
    const cv::Size sz(96, 96);

    // (c) 3px 粗线：r=1 **FAIL**（中心行 d=2 > 1，厚度 3 > 2r=2）、r=2 **PASS**（3 ≤ 4）。
    //     钉住 r 的语义：把 r 当直径（U=#{d>2r}）的实现会在 r=1 错误地绿 → 本条红（变异实测：
    //     uAt 改 2r 后本条与等价性自检同时红）。⚠ `>` 改 `≥` 的变异在本实现**不可见**——
    //     (1,√2) 近似距离从不取整数值（0.96/1.91/2.87…），整数阈值上 > 与 ≥ 等价；真正的
    //     语义风险在 r 的量纲（半径 vs 直径），由本条 + 等价性反向用例双闸守住。
    //     ⚠ 任务下发文本把两个结果写反了（见文件头），此处按 §11.2 公式实现，已上报。
    {
        const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
            m(cv::Range(30, 33), cv::Range(10, 86)) = 255;
        });
        const DiffBlockStat st1 = statAt(p, 1);
        c.chk("diffblock(c)：3px 线 r=1 FAIL（厚度 3 > 2r=2 ⟹ 有内部）",
            st1.uAtRadius > 0, std::to_string(st1.uAtRadius));
        const DiffBlockStat st2 = statAt(p, 2);
        c.chk("diffblock(c)：3px 线 r=2 PASS（3 ≤ 2r=4 ⟹ 处处够薄）",
            st2.uAtRadius == 0, std::to_string(st2.uAtRadius));
        // 中心行长度 76，两端点 d=1 不算 ⟹ U(r=1) 闭式 = 74（近似距离不改这条整数判定）。
        c.chk("diffblock(c)：U(r=1) 闭式 = 74（中心行去两端点，钉住 d>r 的整数边界）",
            st1.uAtRadius == 74, std::to_string(st1.uAtRadius));
    }

    // τ 语义：台阶常数闭式 + 「差半步」只进 τ=0 不进 τ=1 + 「差两步」两边都进。
    // 坏成什么样会红：τ 单位换算错（把「1 个量化步长」当成 1/255 或 1.0）→ 半步像素进 M、
    // τ=0/τ=1 两数相等的信息量没了；τ=0 写成 ≥0 → M = 全图。
    {
        const double step = kSrgbLumaStep;
        c.chk("diffblock：1 个 sRGB 量化步长 ≈ 2.2e-5（0.0722·srgbDecode(1/255)，同 diffstruct 台阶）",
            step > 2.1e-5 && step < 2.3e-5, std::to_string(step));
        const cv::Size s2(8, 8);
        cv::Mat a = cv::Mat(s2, CV_64FC1, cv::Scalar(0.5));
        cv::Mat b = a.clone();
        b.at<double>(0, 0) = 0.5 + 0.5 * step;  // 差半个步长
        b.at<double>(0, 1) = 0.5 + 2.0 * step;  // 差两个步长
        const DiffBlockStat st1 = diffBlockStat(a, b, cv::Mat(), 1.0, 2, 8);
        c.chk("diffblock：τ=1 时半步差不进 M、两步差进（τ 的单位是量化步长不是字节）",
            st1.mPixels == 1, std::to_string(st1.mPixels));
        const DiffBlockStat st0 = diffBlockStat(a, b, cv::Mat(), 0.0, 2, 8);
        c.chk("diffblock：τ=0 = 严格 >0（任一位不同）→ 两个差像素都进 M",
            st0.mPixels == 2, std::to_string(st0.mPixels));
    }
}

/// §3 的等价性自检：`U = #{distanceTransform(M, DIST_L2, 3) > r}` 与字面定义
/// `area(erode(M, MORPH_ELLIPSE, 2r+1))` 的面积一致（容差理由 → kDiffBlockErodeTol 注释）。
/// 这条守的是**实现选择**：判据语义由 erode 定义，算子却跑距离变换（为了同口径的厚度分布）；
/// 两者一旦漂（改 maskSize、改比较方向、改结构元），判据静默换语义——这里当场红。
void testDiffBlockErodeEquivalence(Checker& c)
{
    const cv::Size sz(96, 96);
    struct ShapeCase {
        const char* name;
        std::function<void(cv::Mat&)> paint;
    };
    const ShapeCase shapes[] = {
        { "1px-line", [](cv::Mat& m) { m(cv::Range(47, 48), cv::Range(10, 86)) = 255; } },
        { "3px-line", [](cv::Mat& m) { m(cv::Range(30, 33), cv::Range(10, 86)) = 255; } },
        { "20x20-block", [](cv::Mat& m) { m(cv::Range(30, 50), cv::Range(30, 50)) = 255; } },
        { "disk-r12", [](cv::Mat& m) { cv::circle(m, { 48, 48 }, 12, 255, cv::FILLED); } },
        { "ring-w2", [](cv::Mat& m) { cv::circle(m, { 48, 48 }, 14, 255, 2); } },
    };
    for (const auto& shape : shapes) {
        const SyntheticPair p = paintPair(sz, shape.paint);
        // 距离变换侧：判据实际跑的 U（曲线档位 = {0,1,2,4}）。
        const DiffBlockStat st = statAt(p, kDiffBlockDefaultRadius);
        for (const auto& [r, u] : st.uCurve) {
            if (r == 0)
                continue;  // r=0：SE=1×1 恒等，两侧都 = |M|，由曲线锚点守
            const int64_t er = erodedAreaReference(p.mask, r);
            const double denom = static_cast<double>(u > er ? u : er);
            const double rel
                = denom > 0.0
                    ? std::abs(static_cast<double>(u) - static_cast<double>(er)) / denom
                    : 0.0;
            c.chk(("diffblock 等价性：" + std::string(shape.name) + " @ r=" + std::to_string(r)
                      + " 面积相对差 ≤ 10%")
                      .c_str(),
                rel <= kDiffBlockErodeTol, fmtRel(rel));
        }
    }
    // 容差不是摆设：把「半径当直径」的 U=#{d>2r} 直接和 erode(r) 比，20×20 块 @ r=1 相对差
    // = 1 − 256/324 = 21% > 10% ⟹ 上面那批断言在直径错下必红（曲边形状 8.8% 的定义差被吸收，
    // 粗错不被吸收——容差的两面都要有数）。
    {
        const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
            m(cv::Range(30, 50), cv::Range(30, 50)) = 255;
        });
        cv::Mat dist;
        cv::distanceTransform(p.mask, dist, cv::DIST_L2, kDiffBlockDistMaskSize);
        cv::Mat gt;
        cv::compare(dist, 2.0f, gt, cv::CMP_GT);  // 直径错：r=1 却拿 2r=2 比
        const int64_t diameterU = cv::countNonZero(gt);
        const int64_t er = erodedAreaReference(p.mask, 1);
        const double rel = std::abs(static_cast<double>(diameterU) - static_cast<double>(er))
            / static_cast<double>(er);
        c.chk("diffblock 等价性：容差吞不掉「半径当直径」（20×20 块 @r=1 差 21% > 10%）",
            rel > kDiffBlockErodeTol, fmtRel(rel));
    }
}


/**
 * **报告面**判据（主 agent 在 ★0.5-b1 质检时补）。
 *
 * 为什么单独立一组：判据 U 对了**不等于报告对了**——人只读报告（§11.2 输出①③就是给人定位用的）。
 * 两条变异在补本函数之前**全部存活**（实测）：① 厚度分位数的统计域从「M 内」漂成「全图」；
 * ② 连通域排序方向翻转（`--top` 于是打印**最小**的几个，却仍自称"按面积降序"）。
 * 两条都不动 U，所以判据全绿而报告把人引到错的框上——**报告面必须有自己独立的判据**。
 */
/**
 * **腐蚀面积的闭式定律**（主 agent 补）：实心方块的 `U(r) = (20 − 2r)²` **逐位精确**
 * （整数 r：像素到最近背景的距离 = 离第几圈内环，方块无曲边 ⟹ (1,√2) 近似不参与）。
 * 一条定律同时钉死：半径量纲（错成直径 → 144/36）、`>` 与 `≥` 的边界（差一整圈）、
 * 距离变换掩码尺寸（3→5 只动曲边形状，方块不受 ⟹ 与等价性自检互补）。
 * 单独成函数是认知复杂度护栏逼的（塞进 `testDiffBlockAnchors` 会 16 > 15）。
 */
void testDiffBlockErodeLaw(Checker& c)
{
    const cv::Size sz(96, 96);
    const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
        m(cv::Range(30, 50), cv::Range(30, 50)) = 255;
    });
    const DiffBlockStat st = statAt(p, kDiffBlockDefaultRadius);
    // 🔴 **闭式定律**（主 agent 补）：实心方块的腐蚀面积不是"大于 0"，而是精确的
    // `U(r) = (20 − 2r)²`（整数 r：像素到最近背景的距离恰等于"离第几圈内环"，`U = #{d > r}`
    // 数出的正是去掉 r 圈后的内核；方块无曲边 ⟹ (1,√2) 近似**不参与**，逐位精确）。
    // 一条定律同时钉死：半径的量纲（错成直径 → 144/36）、`>` 与 `≥` 的边界（差一整圈）、
    // 距离变换的掩码尺寸（3→5 会改近似值 ⟹ 曲边形状才受影响，方块不受 ⟹ 与等价性自检互补）。
    // 出处 = 本注释的推导，不是被测码。
    {
        static const struct { int r; int64_t want; const char* label; } kClosed[] = {
            { 0, 400, "diffblock 定律：U(r=0) 恰 = (20−0)² = 400" },
            { 1, 324, "diffblock 定律：U(r=1) 恰 = (20−2)² = 324" },
            { 2, 256, "diffblock 定律：U(r=2) 恰 = (20−4)² = 256（判据默认档）" },
            { 4, 144, "diffblock 定律：U(r=4) 恰 = (20−8)² = 144（稀释档也要有牙）" },
        };
        for (const auto& e : kClosed) {
            int64_t got = -1;
            for (const auto& [cr, cu] : st.uCurve)
                if (cr == e.r)
                    got = cu;
            c.chk(e.label, got == e.want,
                "期望 " + std::to_string(e.want) + " 实得 " + std::to_string(got));
        }
    }
}

void testDiffBlockReportSurface(Checker& c)
{
    const cv::Size sz(200, 200);

    // ① 分位数统计域 = **M 内**（§11.2 输出①）。1px 线：M 内每像素 d ≈ 1 ⟹ p50/p90/p99 全 ≈ 1。
    //    图取 200×200 而 M 只有 76 px ⟹ 统计域若漂成全图，p50 立刻掉到 0（>99% 的像素 d=0）。
    //    坏成什么样会红：统计域写成全图 / 写成「非 M 像素」/ 分位在排序前取。
    {
        const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
            m(cv::Range(99, 100), cv::Range(60, 136)) = 255;  // 1px × 76
        });
        const DiffBlockStat st = statAt(p, kDiffBlockDefaultRadius);
        c.chk("diffblock 报告①：1px 线的 p50/p90/p99 全 ≈ 1（统计域若含背景则 p50 掉 0）",
            st.thicknessValid && st.thicknessP50 > 0.9 && st.thicknessP50 < 1.1
                && st.thicknessP90 > 0.9 && st.thicknessP90 < 1.1
                && st.thicknessP99 > 0.9 && st.thicknessP99 < 1.1,
            "p50=" + std::to_string(st.thicknessP50) + " p90=" + std::to_string(st.thicknessP90));
    }

    // ② 连通域**按面积降序** + `--top` 截断留的是**最大**的几个。
    //    坏成什么样会红：排序方向翻转（→ [400,36,9] 变 [9,36,400]）/ 截断在排序**之前**做
    //    （→ 留到的是 label 序前两个，400 那个可能不在里面）/ 面积与 bbox 串位。
    {
        const SyntheticPair p = paintPair(sz, [](cv::Mat& m) {
            m(cv::Range(20, 40), cv::Range(20, 40)) = 255;   // 20×20 = 400
            m(cv::Range(80, 86), cv::Range(120, 126)) = 255; //  6×6  = 36
            m(cv::Range(150, 153), cv::Range(60, 63)) = 255; //  3×3  = 9
        });
        const DiffBlockStat st = statAt(p, kDiffBlockDefaultRadius, 8);
        c.chk("diffblock 报告②：三个独立块 → 域数恰 3",
            st.componentCount == 3, std::to_string(st.componentCount));
        bool orderOk = st.components.size() == 3 && st.components[0].area == 400
            && st.components[1].area == 36 && st.components[2].area == 9;
        c.chk("diffblock 报告②：components 按面积**降序** = [400, 36, 9]", orderOk,
            st.components.empty() ? "无域"
                                  : std::to_string(st.components[0].area) + "/"
                                        + std::to_string(st.components[1].area) + "/"
                                        + std::to_string(st.components[2].area));
        const DiffBlockStat cut = statAt(p, kDiffBlockDefaultRadius, 2);
        c.chk("diffblock 报告②：--top 2 截断后仍是**最大**的两个（截断不许在排序之前）",
            cut.components.size() == 2 && cut.components[0].area == 400
                && cut.components[1].area == 36
                && cut.componentCount == 3,  // 截断只砍清单，域数读数不许跟着变小
            cut.components.empty() ? "无域" : std::to_string(cut.components[0].area));
    }
}

}  // namespace RGL::imgqa
