// selftest_ao_residual.cpp —— **AO 噪声度量层**（ao_metrics.cpp）+ **时域绝对残差**
// （temporal_residual.cpp 的 phaseSwingStat / gradEnergy / τ 闭式）。
//
// 两台仪器同处的理由 = 它们是同一条改动缝上的两半：AO 那条判据问「附件自己有没有加颗粒」，
// 时域残差那条问「同一相位折叠之后还剩多少摆动」，而两者的期望值**全部来自闭式**（中值滤波
// 的数学性质、集合论、整数几何计数、srgbDecode 的导数），都不吃实机截图、都不启引擎。
//
// 每条断言都带**反向用例**（算子在应该无响应的地方必须无响应）与**变异红线**（把实现改坏成
// 哪种具体形态时本用例会红）。
//
// 搬动纪律：断言本体逐字搬自拆分前的 selftest.cpp（一条不多、一条不少、不改判定方向与阈值）。
// 唯一一处非搬动的改写：残差段原先自带一个 `throws` lambda（与本头 `throwsStd` 同义），
// 按「共享小工具单一出处」收敛掉了。
#include "selftest.h"

#include "ao_metrics.h"
#include "temporal_residual.h"
#include "imgqa.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {

// ── AO 噪声判据的度量层（ao_metrics.cpp）─────────────────────────────────────────
//
// 每条断言的期望值都来自**闭式**（中值滤波的数学性质、集合论、整数几何计数），不来自实机
// 截图——实机数字只能证明「与上次一样」，闭式才能证明「算子算的是我说的那个量」。
// 每条都带**反向用例**（算子在应该无响应的地方必须无响应）与**变异红线**（我把实现改坏成
// 哪种具体形态时，本用例会红）。
/// testAoMetrics 的共用画幅与全 1 mask。拆成三个 checkXxx 之后共享件**只能有一份**——
/// 各抄一份的话三段测的其实不再是同一台仪器（分母换了都没人喊）。
struct AoFixture {
    cv::Size sz { 64, 64 };
    cv::Mat all = cv::Mat::ones(sz, CV_8UC1) * 255;
};

// 时域绝对残差的四个算子（`temporal_residual.h`）。合成序列的 luma 按闭式函数直接填：判据的
// 算子只吃 `luma`，「域必须是线性」这条红线在 `prepareLinear` 里，那边由 testSrgb 钉。
/// testTemporalResidual 的合成序列 fixture：8×4 画幅 + 全 1 mask +「按 (帧号, 列号) 闭式填
/// luma」的生成器。拆成五个 checkXxx 之后画幅与生成器只能有一份（同 AoFixture）。
struct ResidualFixture {
    cv::Size sz { 8, 4 };
    cv::Mat all = cv::Mat(sz, CV_8UC1, cv::Scalar(255));

    /// 生成 `kResidualPeriod × periods` 帧；第 i 帧第 x 列的 luma = value(i, x)（整行同值）。
    template <class V>
    std::vector<Frame> frames(int periods, V&& value) const
    {
        std::vector<Frame> out;
        out.reserve(static_cast<size_t>(kResidualPeriod) * periods);
        for (int i = 0; i < kResidualPeriod * periods; ++i) {
            Frame f;
            f.luma = cv::Mat(sz, CV_64FC1);
            for (int y = 0; y < sz.height; ++y)
                for (int x = 0; x < sz.width; ++x)
                    f.luma.at<double>(y, x) = value(i, x);
            out.push_back(std::move(f));
        }
        return out;
    }
};

/// AO 度量 ①–④（**中值残差与契约偏离**）：线性斜坡不被污染 / 单像素脉冲的闭式 RMS /
/// 平坦契约场的四个读数全 0（零点）/ <255 占比。拆出来的直接理由 = 120 行护栏；
/// 缝也正当：这四段问的都是「抖动算子自己算得对不对」。
void checkAoMedianResidual(Checker& c, const AoFixture& fx)
{
    const cv::Size sz = fx.sz;
    const cv::Mat all = fx.all;
    // ① 中值残差保线性斜坡（**主判据成立的前提**：区域里混进光滑梯度时 jitterRms 不受污染）。
    //    闭式：5×5 窗内单调 ⟹ 中值 = 中心值 ⟹ 残差**恰为 0**。
    {
        cv::Mat ramp(sz.height, sz.width, CV_8UC1);
        for (int y = 0; y < sz.height; ++y)
            for (int x = 0; x < sz.width; ++x)
                ramp.at<uint8_t>(y, x) = static_cast<uint8_t>(100 + x);  // +1/px
        const RegionStat s = statRegion(ramp, all);
        c.chk("AO 度量：线性斜坡的抖动 RMS 恰为 0（主判据不被梯度污染）", s.jitterRms == 0.0,
            "jitterRms=" + std::to_string(s.jitterRms));
        // 变异红线：把 medianBlur 换成 blur（均值）→ 斜坡的均值 ≠ 中心（端点处窗口被边界裁）
        // 会出非零；换成 Laplacian 当抖动 → 直接红。
    }
    // ② 中值**完全抑制**单像素脉冲（闭式：25 个样本里 24 个是背景 ⟹ 中值 = 背景）。
    //    这条钉的是「只有中值滤波能当抖动算子」：均值滤波会把脉冲摊成邻域残差。
    {
        cv::Mat m = cv::Mat::ones(sz, CV_8UC1) * 100;
        m.at<uint8_t>(32, 32) = 200;
        const RegionStat s = statRegion(m, all);
        // 闭式：中值把脉冲从**平滑图**里完全去掉 ⟹ 残差只在脉冲本身 = 幅度 A ⟹
        // RMS = A/√N = 100/√4096 = 1.5625（均值滤波会把脉冲摊到 25 个像素，RMS 不同）。
        c.chk("AO 度量：单像素脉冲 → 残差 RMS = A/√N（中值而非均值，闭式）",
            std::abs(s.jitterRms - 100.0 / std::sqrt(4096.0)) < 1e-9,
            "jitterRms=" + std::to_string(s.jitterRms));
        c.chk("AO 度量：同一脉冲的拉普拉斯有响应（两轴不同，缺一轴会瞎）", s.lapRms > 1.0,
            "lapRms=" + std::to_string(s.lapRms));
    }
    // ③ 反向用例：逐位 255 的场（= strength 0 的附件）在**所有**噪声读数上必须是 0。
    //    这是整条判据的零点（`ao.frag.slang:118` 的位级早退），它红了后面所有数字全部作废。
    {
        const RegionStat s = statRegion(all, all);
        c.chk("AO 度量：平坦契约场的四个读数全为 0（零点）",
            s.jitterRms == 0.0 && s.lapRms == 0.0 && s.fracBelowContract == 0.0 && s.sd == 0.0);
        // 变异红线：`fracBelowContract` 的比较符从 `<` 写成 `<=` → 本用例立刻红（255 ≤ 255）。
        const RegionStat h = statRegion(all, all, /*contract=*/-1.0);
        c.chk("AO 度量：contract<0 时不统计契约偏离（底图分母走这条）", h.fracBelowContract == 0.0);
    }
    // ④ 契约偏离占比（闭式：一半像素 254 ⟹ 恰 0.5）。
    {
        cv::Mat m(sz.height, sz.width, CV_8UC1, cv::Scalar(255));
        m(cv::Rect(0, 0, sz.width / 2, sz.height)).setTo(254);
        const RegionStat s = statRegion(m, all);
        c.chk("AO 度量：<255 占比 = 0.5（闭式）", std::abs(s.fracBelowContract - 0.5) < 1e-12,
            std::to_string(s.fracBelowContract));
    }
}

/// AO 度量 ⑤–⑧（**字节域 A/B 与集合**）：恒定偏移的颗粒/渐变分解 / 三通道取最大且 16S
/// 不饱和 / rmsDiff 的 mask 纪律与拒收 / Jaccard 闭式。
void checkAoByteMetrics(Checker& c, const AoFixture& fx)
{
    const cv::Size sz = fx.sz;
    const cv::Mat all = fx.all;
    // ⑤ 字节域 A/B 的**恒定偏移**：grain 必须恰为 0、smooth 必须恰为偏移量。
    //    闭式：median(常数) = 常数 ⟹ 残差 0。这条把「AO 把画面整体压暗」与「AO 加了颗粒」
    //    分开的分解算子钉住——偏移是合法形状，颗粒才是噪声。
    {
        cv::Mat a(sz, CV_8UC3, cv::Scalar(100, 100, 100));
        cv::Mat b(sz, CV_8UC3, cv::Scalar(95, 95, 95));
        const ByteAB s = byteAbsDiff(a, b, all);
        c.chk("AO 度量：恒定 5 LSB 偏移 → 颗粒项恰为 0（偏移不是噪声）", s.grainRms == 0.0,
            "grain=" + std::to_string(s.grainRms));
        c.chk("AO 度量：恒定偏移 → 渐变项 = 5、均值/最大 = 5",
            std::abs(s.smoothRms - 5.0) < 1e-9 && s.mean == 5.0 && s.mx == 5.0,
            "smooth=" + std::to_string(s.smoothRms) + " mean=" + std::to_string(s.mean)
                + " mx=" + std::to_string(s.mx) + " changed=" + std::to_string(s.changed)
                + " pixels=" + std::to_string(s.pixels));
        const ByteAB r = byteAbsDiff(a, a, all);  // 反向：同一张图
        c.chk("AO 度量：同一张图 → 全读数 0（反向用例）",
            r.changed == 0 && r.mx == 0.0 && r.grainRms == 0.0);
        // 变异红线：三通道若取**均值**而非最大值，暗通道会把 5 稀释；取 max 才对。
    }
    // ⑥ 三通道取**最大**且 16S 不饱和（闭式：+7/−9/0 → 9；反向 −250 方向也必须算出来）。
    {
        cv::Mat a(sz, CV_8UC3, cv::Scalar(200, 200, 200));
        cv::Mat b(sz, CV_8UC3, cv::Scalar(193, 209, 200));  // B +7、G −9、R 0
        c.chk("AO 度量：三通道最大 |Δ| = 9（含变暗方向，8U 饱和会读成 0）",
            maxByteDiff(a, b, all) == 9.0, std::to_string(maxByteDiff(a, b, all)));
        c.chk("AO 度量：同一张图的最大差 = 0（反向）", maxByteDiff(a, a, all) == 0.0);
        // 变异红线：这里若用 `Mat::col(i)` 当第 i 通道 → Mat 缩成 64×1、按 64 列遍历越界，
        // 本用例读到的是垃圾值（实测真机读出过 32523 这种 >255 的「字节差」）。
    }
    // ⑦ rmsDiff：恒定差 → 恰等于该差；mask 外的差异不得参与（闭式）。
    {
        cv::Mat a = cv::Mat::ones(sz, CV_8UC1) * 100;
        cv::Mat b = cv::Mat::ones(sz, CV_8UC1) * 96;
        c.chk("AO 度量：恒定差 4 → rmsDiff = 4（闭式）", std::abs(rmsDiff(a, b, all) - 4.0) < 1e-9,
            std::to_string(rmsDiff(a, b, all)));
        cv::Mat mask = cv::Mat::zeros(sz, CV_8UC1);
        mask(cv::Rect(0, 0, sz.width, sz.height)).setTo(255);
        b.setTo(100);  // mask 内改成无差
        c.chk("AO 度量：mask 内的差为 0 → rmsDiff = 0（mask 外不参与）",
            rmsDiff(a, b, mask) == 0.0);
        cv::Mat three(sz, CV_8UC3, cv::Scalar(1, 2, 3));
        bool threw = false;
        try {
            static_cast<void>(rmsDiff(three, three, all));
        } catch (const cv::Exception&) {
            threw = true;
        }
        c.chk("AO 度量：rmsDiff 收到 3 通道必须拒收（at<float> 静默读 ch0 是假绿源）", threw);
    }
    // ⑧ Jaccard（闭式：同集合 1、不相交 0、半重叠 |∩|/|∪| = 1/3）。
    {
        cv::Mat a = cv::Mat::zeros(sz, CV_8UC1), b = cv::Mat::zeros(sz, CV_8UC1);
        a(cv::Rect(0, 0, 30, 64)).setTo(255);
        b(cv::Rect(20, 0, 30, 64)).setTo(255);  // 交 10 列、并 50 列
        c.chk("AO 度量：Jaccard 同集合 = 1", std::abs(jaccard(a, a, all) - 1.0) < 1e-12);
        c.chk("AO 度量：Jaccard 半重叠 = 10/50（闭式）",
            std::abs(jaccard(a, b, all) - 0.2) < 1e-12, std::to_string(jaccard(a, b, all)));
        cv::Mat z = cv::Mat::zeros(sz, CV_8UC1);
        c.chk("AO 度量：空并集 → 1（约定：无证据时不判「不重叠」）",
            jaccard(z, z, all) == 1.0);
    }
}

/// AO 度量 ⑨–⑪（**区域定义与对齐算子**）：distToEdge 的类型是判据的一部分 / 凹角接触环
/// 必须是 AND / shiftX 的符号约定与边界补 0。这三段自带 fixture 图、不吃共用 mask，
/// 故不接 fixture。
void checkAoRegionMetrics(Checker& c)
{
    // ⑨ distToEdge 的类型是**判据的一部分**（CV_32F）。flatRegion 曾按 uint8_t 读它，
    //    「离边界 > 40px」变成随机判据（实测契约区膨胀 2 倍、σ 从 1.3 涨到 10.9）。
    {
        cv::Mat ids = cv::Mat::zeros(100, 100, CV_32SC1);
        ids(cv::Rect(30, 30, 41, 41)).setTo(1);
        const cv::Mat dist = distToEdge(idEdges(ids));
        c.chk("AO 度量：距离图是 CV_32F（按 8U 读就是随机判据——真实事故）",
            dist.type() == CV_32FC1, "type=" + std::to_string(dist.type()));
        // 整数几何闭式：41×41 的块，dist > 5 ⟹ 去掉 6 圈 ⟹ 29×29 = 841 像素。
        const cv::Mat flat = flatRegion(ids, dist, 1, 5.0);
        c.chk("AO 度量：契约区像素数 = 29²（整数几何闭式）",
            cv::countNonZero(flat) == 29 * 29, std::to_string(cv::countNonZero(flat)));
        c.chk("AO 度量：契约区不含背景（oid 过滤）",
            cv::countNonZero(flatRegion(ids, dist, 2, 5.0)) == 0);
        // 变异红线：把 flatRegion 改回逐行 `ptr<uint8_t>` 读 → 上面两条同时红（且第二条的
        // 像素数会变成随机值）。这就是「区域定义不干净」最便宜的探测器。
    }
    // ⑩ 凹角接触环：只收「inner 且邻域贴着 outer」，且**必须**是 AND（闭式构造）。
    {
        cv::Mat ids = cv::Mat::zeros(100, 100, CV_32SC1);
        ids(cv::Rect(0, 80, 100, 20)).setTo(1);   // 地面 = inner
        ids(cv::Rect(40, 40, 20, 40)).setTo(2);   // 贴地方块 = outer
        const cv::Mat ring = contactRing(ids, 1, {2}, 6);
        const int n = cv::countNonZero(ring);
        c.chk("AO 度量：接触环非空（方块脚两侧 + 正下方）", n > 0 && n < 20 * 100,
            "n=" + std::to_string(n));
        cv::Mat onlyInner;
        cv::compare(ids, cv::Scalar(1), onlyInner, cv::CMP_EQ);
        cv::Mat notInner;
        cv::bitwise_and(ring, ~onlyInner, notInner);
        c.chk("AO 度量：接触环 ⊆ inner（AND 写成 OR 就红——真实变异）",
            cv::countNonZero(notInner) == 0);
        // 反向：outer 挪到 grow 之外 → 环必须空。
        cv::Mat far_ = ids.clone();
        far_(cv::Rect(40, 40, 20, 40)).setTo(0);
        far_(cv::Rect(40, 0, 20, 40)).setTo(2);  // 离地面 > 6 px
        c.chk("AO 度量：outer 远离 grow → 环为空（反向用例）",
            cv::countNonZero(contactRing(far_, 1, {2}, 6)) == 0);
    }
    // ⑪ 对齐算子 shiftX：符号约定 + 往返 + 边界补 0（爬行判据的方向全靠它）。
    {
        cv::Mat src = cv::Mat::zeros(10, 20, CV_8UC1);
        src.at<uint8_t>(5, 4) = 255;
        c.chk("AO 度量：shiftX(+3) 把标记搬到 x=7（符号约定）",
            shiftX(src, 3).at<uint8_t>(5, 7) == 255 && shiftX(src, 3).at<uint8_t>(5, 4) == 0);
        c.chk("AO 度量：shiftX(−3) 把标记搬到 x=1（反向符号）",
            shiftX(src, -3).at<uint8_t>(5, 1) == 255);
        const cv::Mat rt = shiftX(shiftX(src, 3), -3);
        c.chk("AO 度量：+3 后 −3 逐位复原（往返；Rect 写反就红）",
            cv::countNonZero(rt != src) == 0);
        c.chk("AO 度量：dx=0 逐位相同（反向）", cv::countNonZero(shiftX(src, 0) != src) == 0);
        cv::Mat edge = cv::Mat::zeros(10, 20, CV_8UC1);
        edge.at<uint8_t>(5, 19) = 255;
        c.chk("AO 度量：右移把边界标记推出画布（补 0，不绕回）",
            shiftX(edge, 3).at<uint8_t>(5, 19) == 0);
    }
}

/// 残差 ①②③（**折叠口径本身**）：零点 / 同相位折叠的阳性面 / swing = max − median。
/// 拆出来的直接理由 = 120 行与认知复杂度两条护栏；缝也正当：这三段全在问「折叠与中位数
/// 怎么算」，后面的区域纪律、τ 闭式、梯度能量各问各的。
void checkResidualFold(Checker& c, const ResidualFixture& fx)
{
    const cv::Mat all = fx.all;
    // ① 零点：同一张图连拍 ⟹ 五个读数**恰为 0**。⚠ 本条对算子变异的力气最小（常数序列
    //    在很多错实现上也是 0），它的价值是给后面所有条一个「非零即异常」的参照。
    {
        const auto shots = fx.frames(2, [](int, int) { return 0.37; });
        const ResidualStat s = phaseSwingStat(shots, all, 0.001, 2);
        c.chk("残差：同一张图连拍 → 五个读数恰为 0（零点）",
            s.meanSwing == 0.0 && s.p999Swing == 0.0 && s.maxSwing == 0.0
                && s.fracOverTau == 0.0 && s.halfGapMax == 0.0,
            "mean=" + std::to_string(s.meanSwing) + " max=" + std::to_string(s.maxSwing));
        c.chk("残差：meanLuma 走的是像素值不是常数（在场性守卫的输入）",
            std::abs(s.meanLuma - 0.37) < 1e-12, std::to_string(s.meanLuma));
    }
    // ② 同相位折叠的**阳性面**：只有第 3 个相位亮，且每 16 帧重复一次 ⟹ 折叠后 16 列里
    //    恰好一列 = 1、其余 = 0 ⟹ 中位数 0 ⟹ swing **恰为 1**；两半同相位 ⟹ halfGap 恰 0。
    //    变异红线：折叠若按「连续 32 帧两两配对」而非「同相位配对」，那一列会被摊成两个
    //    0.5 列 ⟹ swing = 0.5，本条当场红（这是全组里唯一钉住折叠口径本身的一条）。
    {
        const auto shots = fx.frames(4, [](int i, int) { return (i % 16 == 3) ? 1.0 : 0.0; });
        const ResidualStat s = phaseSwingStat(shots, all, 0.5, 4);
        c.chk("残差：单相位离群 → swing 恰为 1（同相位折叠，闭式）",
            s.meanSwing == 1.0 && s.maxSwing == 1.0 && s.fracOverTau == 1.0,
            "mean=" + std::to_string(s.meanSwing));
        c.chk("残差：两半同相位 → halfGapMax 恰为 0（折叠自检的阳性面）",
            s.halfGapMax == 0.0, "gap=" + std::to_string(s.halfGapMax));
        // 计数口径的反向锚点：max = 0 时**超 τ 的像素数必须也是 0**（若计数写成"≥"或按相位
        // 而非按像素累加，这条会给出 16 或 16×像素数，而不是 0）。
        c.chk("残差：halfGapOverPx 按**像素**计数（同相位 → 恰为 0，反向锚点）",
            s.halfGapOverPx == 0, "overPx=" + std::to_string(s.halfGapOverPx));
    }
    // ③ swing = max − **median**，不是 max − min。闭式：16 列 = 8×0.0 + 7×0.5 + 1×1.0
    //    ⟹ 排序第 8、9 位 = 0.0 与 0.5 ⟹ 中位数 0.25 ⟹ swing = 1.0 − 0.25 = **0.75**；
    //    写成 max−min 会给 1.0。这条把「为什么用 median」从注释变成了会红的断言。
    {
        const auto shots = fx.frames(2, [](int i, int) {
            const int p = i % 16;
            return p < 8 ? 0.0 : (p < 15 ? 0.5 : 1.0);
        });
        const ResidualStat s = phaseSwingStat(shots, all, 0.0, 2);
        c.chk("残差：swing = max−median（不是 max−min），闭式 0.75",
            std::abs(s.meanSwing - 0.75) < 1e-12, "mean=" + std::to_string(s.meanSwing));
    }
}

/// 残差 ④⑤（**区域纪律 + 折叠算术的双闭式**）：mask 外的摆动不计 / 空区域不出数 /
/// 线性漂移下 halfGap 与 swing 两个数同时命中（同时命中 = 折叠、中位数、两半差三条都对）。
void checkResidualRegionDrift(Checker& c, const ResidualFixture& fx)
{
    const cv::Size sz = fx.sz;
    const cv::Mat all = fx.all;
    // ④ 区域纪律：mask 外的剧烈摆动一个都不计；同一序列换全图口径必须测得到它
    //    （反向：防「mask 恰好全 0 / 全在别处」把一切都判成稳定）。
    {
        cv::Mat mask = cv::Mat::zeros(sz, CV_8UC1);
        mask(cv::Rect(0, 0, sz.width / 2, sz.height)).setTo(255);
        const auto shots = fx.frames(2, [&](int i, int x) {
            return x < sz.width / 2 ? 0.5 : ((i % 16 == 3) ? 1.0 : 0.0);
        });
        const ResidualStat in = phaseSwingStat(shots, mask, 0.01, 2);
        c.chk("残差：mask 外的摆动不计入（区域纪律）",
            in.meanSwing == 0.0 && in.maskPixels == sz.area() / 2,
            "mean=" + std::to_string(in.meanSwing) + " px=" + std::to_string(in.maskPixels));
        const ResidualStat full = phaseSwingStat(shots, cv::Mat(), 0.01, 2);
        c.chk("残差：同一序列全图口径必须测到（反向用例）",
            std::abs(full.meanSwing - 0.5) < 1e-12 && full.maskPixels == sz.area(),
            "mean=" + std::to_string(full.meanSwing));
        const cv::Mat none = cv::Mat::zeros(sz, CV_8UC1);
        c.chk("残差：空区域不出数（maskPixels=0，不给「测得 0」的机会）",
            phaseSwingStat(shots, none, 0.01, 2).maskPixels == 0);
    }
    // ⑤ 折叠算术的**双**闭式：value = 帧号（线性漂移）⟹ 半 A 第 p 列 = p+8、半 B = p+40
    //    ⟹ halfGap = halfGapMax = **32**（每列都是）；两半均值 = p+24 ⟹ max 39、中位数
    //    (31+32)/2 = 31.5 ⟹ swing = **7.5**。两个数同时命中 = 折叠/中位数/两半差三条都对。
    {
        const auto shots = fx.frames(4, [](int i, int) { return static_cast<double>(i); });
        const ResidualStat s = phaseSwingStat(shots, all, 0.0, 4);
        c.chk("残差：线性漂移 → halfGap = halfGapMax = 32、swing = 7.5（双闭式）",
            s.halfGap == 32.0 && s.halfGapMax == 32.0 && s.meanSwing == 7.5,
            "gap=" + std::to_string(s.halfGap) + " swing=" + std::to_string(s.meanSwing));
        // 计数口径的阳性锚点：τ=0 且每像素 gap=32 ⟹ **每个像素都超**（=maskPixels）。
        // 若计数误按「像素×相位」累加，这里会给出 16×，当场红。
        c.chk("残差：halfGapOverPx = maskPixels（τ=0 且逐像素都超，阳性锚点）",
            s.halfGapOverPx == s.maskPixels,
            "overPx=" + std::to_string(s.halfGapOverPx) + " mask="
                + std::to_string(s.maskPixels));
    }
}

/// 残差 ⑥（**前提守卫**）：缺帧 / 奇周期 / 混进没 prepareLinear 或别的 extent 的帧一律**抛**。
/// 「列与相位脱钩」的读数看起来完全正常，是最贵的一类假绿 ⟹ 守卫单独成函数：红的时候一眼
/// 知道是守卫掉了，而不是读数漂了。
void checkResidualGuards(Checker& c, const ResidualFixture& fx)
{
    // ⑥ 前提守卫：缺帧 / 奇周期 / 混进没 prepareLinear 或别的 extent 的帧，一律**抛**，
    //    不许出数（「列与相位脱钩」的读数看起来完全正常，是最贵的一类假绿）。
    {
        const auto good = fx.frames(2, [](int, int) { return 0.2; });
        c.chk("相位平均：帧数不是整数周期就抛", throwsStd([&] { (void)phaseAverageLuma(good, 3); }));
        c.chk("相位平均：周期数 <2 就抛（奇数半没有同相位对照）",
            throwsStd([&] { (void)phaseAverageLuma(good, 1); }));
        auto noLin = good;
        noLin.push_back(Frame{});
        c.chk("相位平均：混进没 prepareLinear 的帧就抛", throwsStd([&] { (void)phaseAverageLuma(noLin, 2); }));
        auto otherSize = good;
        otherSize.back().luma = cv::Mat(cv::Size(3, 3), CV_64FC1, cv::Scalar(0.0));
        c.chk("相位平均：混进别的 extent 就抛（两帧不是同一次采集）",
            throwsStd([&] { (void)phaseAverageLuma(otherSize, 2); }));
    }
}

/// 残差 ⑦（**τ 闭式**）：与真解码器中心差分对拍、随电平单调、以及「容差取 max 而 τ 取 p99」
/// 这两件事的上界性质。拆出来的直接理由 = 认知复杂度护栏（合成进上一条 >15）。
void checkResidualTau(Checker& c)
{
    // ⑦ τ 闭式与真解码器对拍（**拐点以上**；`srgbDecode` 是分段函数，byte ≤ 10 走线性段）。
    //    中心差分步长取 0.01 字节（亚 LSB），二阶误差 ~1e-7。
    {
        double worst = 0.0;
        for (int b = 11; b <= 254; ++b) {
            const double x = static_cast<double>(b) / 255.0;
            const double d = (srgbDecode(x + 0.01 / 255.0) - srgbDecode(x - 0.01 / 255.0)) / 0.02;
            worst = std::max(worst,
                std::abs(d - lsbLinearAtLuma(srgbDecode(x))) / std::max(1e-30, d));
        }
        c.chk("τ 闭式：拐点以上与 srgbDecode 中心差分对拍（相对误差 <1e-5）", worst < 1e-5,
            "worst=" + std::to_string(worst));
        c.chk("τ 闭式：lin=1 处 = 2.4/1.055/255",
            std::abs(lsbLinearAtLuma(1.0) - 2.4 / 1.055 / 255.0) < 1e-15,
            std::to_string(lsbLinearAtLuma(1.0)));
        c.chk("τ 随线性电平单调递增（= 拿白电平 τ 当 mask 内上界的前提）",
            lsbLinearAtLuma(0.05) < lsbLinearAtLuma(0.617)
                && lsbLinearAtLuma(0.617) < lsbLinearAtLuma(1.0));
        // 判据真正依赖的安全性质：**不亮于 maxLin 的像素，翻一格的实际跨度都有界**。
        // 这条看着像「按构造成立」，但它钉的是容差用 max 而不是 p99、用离散步长而不是
        // 导数这两件事——任一件被改回去（半间差容差就会把正常读数判成结构失效）。
        const double maxLin = srgbDecode(206.0 / 255.0);
        const double bound = maxLsbStepUpTo(maxLin);
        bool bounded = true;
        for (int b = 0; b < 255; ++b) {
            const double lo = srgbDecode(static_cast<double>(b) / 255.0);
            if (lo > maxLin)
                break;
            bounded = bounded && srgbDecode(static_cast<double>(b + 1) / 255.0) - lo <= bound;
        }
        c.chk("τ：不亮于 maxLin 的像素翻一格 ≤ maxLsbStepUpTo(maxLin)（容差的上界性质）",
            bounded, "bound=" + std::to_string(bound));
        // 离散步长与闭式导数在同一电平处应当一致（两条独立实现互相对拍，差 <1%）。
        c.chk("τ：离散一格与闭式导数在白电平处一致（<1%）",
            std::abs(bound / lsbLinearAtLuma(maxLin) - 1.0) < 0.01,
            "disc=" + std::to_string(bound) + " closed=" + std::to_string(lsbLinearAtLuma(maxLin)));
        c.chk("τ：一格跨度随电平单调（= 取最亮电平即上界的前提）",
            maxLsbStepUpTo(0.05) < maxLsbStepUpTo(0.617)
                && maxLsbStepUpTo(0.617) < maxLsbStepUpTo(1.0));
        // ⚠ 容差取 max、τ 取 p99，两者**必须**能分开：mask 里若有比 p99 更亮的高光，用 p99
        //    当容差会低估。本条把「别把两个数合并成一个」钉住。
        c.chk("τ：比 p99 白电平更亮的高光，一格能翻过 τ（所以容差走 max 不走 p99）",
            maxLsbStepUpTo(0.9) > lsbLinearAtLuma(maxLin),
            "bright=" + std::to_string(maxLsbStepUpTo(0.9)) + " tau=" + std::to_string(bound));
        c.chk("τ：maxLin=1 时 = 1 − srgbDecode(254/255)（端点闭式）",
            std::abs(maxLsbStepUpTo(1.0) - (1.0 - srgbDecode(254.0 / 255.0))) < 1e-15,
            std::to_string(maxLsbStepUpTo(1.0)));
    }
}

/// 残差 ⑧⑨（**梯度轴**）：gradEnergy 的 Sobel + REFLECT_101 闭式、「mask 是选择器不是
/// 裁剪器」、以及梯度图本体与梯度能量**同一算子**。
void checkResidualGradient(Checker& c, const ResidualFixture& fx)
{
    const cv::Size sz = fx.sz;
    const cv::Mat all = fx.all;
    // ⑧ 梯度能量：常量图恰为 0（收敛正确性判据的零点）；斜坡走 Sobel 核 [−1,0,1] +
    //    BORDER_REFLECT_101 的闭式 —— 内部 gx = 2·slope，**两列边界**反射后 gx = 0
    //    ⟹ 均值 = 2·slope·(W−2)/W。变异红线：borderType 改成 REPLICATE 会给 1.75·slope。
    {
        const cv::Mat flat(sz, CV_64FC1, cv::Scalar(0.3));
        c.chk("梯度能量：常量图恰为 0（零点）", gradEnergy(flat, all) == 0.0,
            std::to_string(gradEnergy(flat, all)));
        const double slope = 0.1;
        cv::Mat ramp(sz.height, sz.width, CV_64FC1);
        for (int y = 0; y < sz.height; ++y)
            for (int x = 0; x < sz.width; ++x)
                ramp.at<double>(y, x) = slope * x;
        const double expect = 2.0 * slope * (sz.width - 2) / sz.width;
        c.chk("梯度能量：线性斜坡 = 2·slope·(W−2)/W（Sobel 核 + REFLECT_101 闭式）",
            std::abs(gradEnergy(ramp, all) - expect) < 1e-12,
            "got=" + std::to_string(gradEnergy(ramp, all)) + " expect=" + std::to_string(expect));
        // mask 是**选择器不是裁剪器**：算子必须在整图上跑完再按 mask 取均值。fixture =
        // 全图斜坡 + 只遮左半 ⟹ mask 内 gx = [0, 2s, 2s, 2s]（x=0 是画布边界，REFLECT_101
        // 反射后自减为 0）⟹ 均值 = 2·slope·(Wm−1)/Wm。**变异红线**：若实现先裁剪再 Sobel，
        // 裁剪区的右边缘会变成边界（gx=0）⟹ 均值降成 2·slope·(Wm−2)/Wm，本条当场红。
        cv::Mat half = cv::Mat::zeros(sz, CV_8UC1);
        half(cv::Rect(0, 0, sz.width / 2, sz.height)).setTo(255);
        const double expectL = 2.0 * slope * (sz.width / 2 - 1) / (sz.width / 2);
        c.chk("梯度能量：mask 是选择器不是裁剪器（闭式，Sobel 支撑跨 mask 边界）",
            std::abs(gradEnergy(ramp, half) - expectL) < 1e-12,
            "got=" + std::to_string(gradEnergy(ramp, half)) + " expect=" + std::to_string(expectL));
    }
    // ⑨ 梯度图本体与梯度能量**同一算子**（分带表用本体、债-47 用均值，两者若各写一份
    //    Sobel，「边缘带」与「梯度比」说的就不是同一张图）。变异红线：sobelAbsGrad 的
    //    核尺寸改成 5，或 gradEnergy 不再走它 → 两条同时红。
    {
        const double slope = 0.1;
        cv::Mat ramp(sz.height, sz.width, CV_64FC1);
        for (int y = 0; y < sz.height; ++y)
            for (int x = 0; x < sz.width; ++x)
                ramp.at<double>(y, x) = slope * x;
        const cv::Mat g = sobelAbsGrad(ramp);
        c.chk("梯度图：常量图恰为 0（零点）",
            cv::norm(sobelAbsGrad(cv::Mat(sz, CV_64FC1, cv::Scalar(0.3))), cv::NORM_INF)
                == 0.0);
        c.chk("梯度图：与 gradEnergy 同算子（均值相等，闭式 2·slope·(W−2)/W）",
            std::abs(cv::mean(g)(0) - 2.0 * slope * (sz.width - 2) / sz.width) < 1e-12
                && std::abs(cv::mean(g)(0) - gradEnergy(ramp, all)) < 1e-15,
            "mean=" + std::to_string(cv::mean(g)(0)));
    }
}

}  // namespace

/// AO 噪声判据度量层的自检，拆成三个 checkXxx（中值残差 / 字节域与集合 / 区域与对齐）。
/// 120 行护栏逼的，缝按「问的是哪一类算子」切。
void testAoMetrics(Checker& c)
{
    const AoFixture fx;
    checkAoMedianResidual(c, fx);
    checkAoByteMetrics(c, fx);
    checkAoRegionMetrics(c);
}

/// 时域绝对残差四个算子的自检，拆成五个 checkXxx（折叠口径 / 区域纪律 / 前提守卫 /
/// τ 闭式 / 梯度轴）。120 行与认知复杂度两条护栏逼的，缝按「问的是哪件事」切。
void testTemporalResidual(Checker& c)
{
    const ResidualFixture fx;
    checkResidualFold(c, fx);
    checkResidualRegionDrift(c, fx);
    checkResidualGuards(c, fx);
    checkResidualTau(c);
    checkResidualGradient(c, fx);
}

}  // namespace RGL::imgqa
