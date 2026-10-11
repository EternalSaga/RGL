// selftest_motion.cpp —— **运动态时域族**（拆文件的缝说明全文见 `selftest.h`）：
// temporal_flicker_metric.cpp 的闭式与半谱 / temporal_homography_metric.cpp 的闭式单应 /
// temporal_yaw_remap.cpp 的带 pitch 重映射。
//
// 三台仪器同处的理由 = 它们回答的是同一个问题的三半：「机位在动的时候，像素位移是不是
// 我算的那个」。flicker 问「每帧扫过多少亚像素」、homography 问「纯 yaw 的单应长什么样」、
// yaw_remap 问「带 pitch 时怎么把 t+1 帧取回 t 帧」——三者的相机基与 φ 换算**必须同式**，
// 分文件各抄一份基 = 静默反号（本族真付过的学费见下方 yaw 段的变异红线表）。
//
// 搬动纪律：断言本体逐字搬自拆分前的 selftest.cpp（一条不多、一条不少、不改判定方向与阈值）。
#include "selftest.h"

#include "temporal_flicker.h"
#include "temporal_homography.h"
#include "temporal_yaw_remap.h"
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

/// 闭式锚点侧：无 AA 端恒等式 / 两个实机锚点 / 理想 < 无 AA / v=0 与 v=1 两条反向 /
/// 增益随扫速单调降。拆出来的直接理由 = 认知复杂度护栏（合成进下一条 >15）。
void checkFlickerClosedForm(Checker& c)
{
    const FlickerScale s{ 0.0987f, 0.9840f };
    const FlickerAnchor a = flickerClosedForm(0.125f, s);

    // 恒等式：无 AA 端 = 阶梯的整段跳变，与扫速无关。变异红线：noAaRho 写成 chain(1) 本身。
    c.chk("闭式·无AA 端 = chain(1)−chain(0)",
        std::abs(a.noAaRho - (displayChain(1.0f, s) - displayChain(0.0f, s))) < 1e-9f);
    // 实机锚点（±1%）。变异红线：displayChain 换掉 ACES 或漏掉 8bit 取整 → 两端同时漂出带。
    c.chk("闭式·理想端 实机锚点 0.10219", std::abs(a.idealRho - 0.10219f) < 1.1e-3f,
        std::to_string(a.idealRho));
    c.chk("闭式·无AA 端 实机锚点 0.57451", std::abs(a.noAaRho - 0.57451f) < 5.8e-3f,
        std::to_string(a.noAaRho));
    // 理想必须严格低于无 AA（否则判据的分母是负的、ζ 无意义）。变异红线：max 写成 min。
    c.chk("闭式·理想 < 无AA", a.idealRho < a.noAaRho);

    // 反向用例 ①：扫速 0 = 静止 ⟹ 理想每帧变化恰为 0（不是「接近 0」）。
    c.chk("闭式·v=0 理想端恰为 0", flickerClosedForm(0.0f, s).idealRho == 0.0f);
    // 反向用例 ②：整数像素平移（v=1）⟹ 斜坡只是整体搬一格，与阶梯同幅 ⟹ 两端相等。
    // 这条钉住「v≥1 没有混叠可测」的用法红线（cmd 侧拒收 v≥1 的理由）。
    const FlickerAnchor i1 = flickerClosedForm(1.0f, s);
    c.chk("闭式·v=1 两端相等（无混叠可测）", std::abs(i1.idealRho - i1.noAaRho) < 1e-6f,
        std::to_string(i1.idealRho) + " vs " + std::to_string(i1.noAaRho));

    // 单调性：扫速越快，理想端越接近无 AA 端 ⟹ 增益 G = noAA/ideal 单调下降。
    // 变异红线：网格步长粗到 ≥ 扫速（上确界取不到斜坡段）→ 小 v 的 G 塌到 1。
    double prev = 1e30;
    for (float v: { 0.0625f, 0.125f, 0.25f, 0.5f }) {
        const FlickerAnchor g = flickerClosedForm(v, s);
        const double gain = g.noAaRho / static_cast<double>(g.idealRho);
        c.chk("闭式·增益随扫速单调降", gain < prev, "v=" + std::to_string(v) + " G=" + std::to_string(gain));
        prev = gain;
    }
}

/// 幅值反标定往返 + Hann 半谱（正弦能量集中在主瓣 / 非整数 bin 的远端泄漏才是「Hann 窗
/// 在不在」的判据 / 常数信号去 DC 后无能量 / 长度不足返回空）。
void checkFlickerSpectrum(Checker& c)
{
    // 幅值反标定往返：观测两端 → ACES 前两端 → 再走一遍链，应回到观测两端（±1 LSB）。
    // 变异红线：反标定二分方向写反 → 往返差一个量级。
    const FlickerScale back = scaleFromObserved(0.0423f, 0.6172f);
    c.chk("反标定·黑端往返 ≤1LSB", std::abs(displayChain(0.0f, back) - 0.0423f) < 1.0 / 255.0,
        std::to_string(displayChain(0.0f, back)));
    c.chk("反标定·白端往返 ≤1LSB", std::abs(displayChain(1.0f, back) - 0.6172f) < 1.0 / 255.0,
        std::to_string(displayChain(1.0f, back)));

    // Hann 半谱：纯正弦（整数周期）能量应集中在一个 bin；常数信号去 DC 后应无能量。
    // Hann 主瓣 4 bin 宽（能量落在 m−1/m/m+1 三格），故判「激励 bin ±1」而不是单 bin。
    // 变异红线：Hann 窗或去均值漏掉 → 常数信号漏出巨大 DC、正弦泄漏铺满半谱。
    std::vector<float> sine(64), flat(64, 0.37f);
    for (int k = 0; k < 64; ++k)
        sine[static_cast<size_t>(k)] = 0.5f + 0.2f * std::sin(2.0f * 3.14159265358979f * 4.0f * k / 64.0f);
    const auto sp = hannHalfSpectrum(sine);
    const auto sf = hannHalfSpectrum(flat);
    double tot = 0.0, lobe = 0.0;
    for (size_t m = 1; m < sp.size(); ++m) {
        tot += static_cast<double>(sp[m]) * sp[m];
        if (m >= 3 && m <= 5)
            lobe += static_cast<double>(sp[m]) * sp[m];
    }
    c.chk("FFT·正弦能量集中在激励主瓣", tot > 0 && lobe / tot > 0.9, std::to_string(lobe / tot));
    // 泄漏判据（这条才是「Hann 窗在不在」的判据）：频率取**非整数 bin**（4.5），
    // 远端（≥8 bin）能量占比 Hann 实测 0.00006、矩形窗 0.0129（差 200 倍）⟹ 阈值取 0.002。
    // 变异红线：去掉 Hann 窗 → 本条以 1.3% 红（而整数频率的正弦两条都「完美」，故必须用 4.5）。
    std::vector<float> half(64);
    for (int k = 0; k < 64; ++k)
        half[static_cast<size_t>(k)] = std::sin(2.0f * 3.14159265358979f * 4.5f * k / 64.0f);
    const auto sh = hannHalfSpectrum(half);
    double hTot = 0.0, hFar = 0.0;
    for (size_t m = 1; m < sh.size(); ++m) {
        hTot += static_cast<double>(sh[m]) * sh[m];
        if (m >= 8)
            hFar += static_cast<double>(sh[m]) * sh[m];
    }
    c.chk("FFT·Hann 远端泄漏 <0.2%", hTot > 0 && hFar / hTot < 0.002, std::to_string(hFar / hTot));
    double flatE = 0.0;
    for (size_t m = 1; m < sf.size(); ++m)
        flatE += static_cast<double>(sf[m]) * sf[m];
    c.chk("FFT·常数信号去 DC 后无能量", flatE < 1e-6, std::to_string(flatE));
    c.chk("FFT·长度不足返回空", hannHalfSpectrum({ 1.0f, 2.0f }).empty());
}

/// 亚像素边缘定位 + 局部扫速与焦距换算 + 逐帧差分位（后两条是闭式锚的输入面）。
void checkFlickerEdgeAndSweep(Checker& c)
{
    // 亚像素边缘定位：覆盖 = clamp(x − 39.8) 的 1px 边（覆盖 0.5 处在 x = 40.3），应找回 ±0.1。
    // ⚠ 位置**故意不取半整数**：取 40.5 时抛物线修正恒为 0，符号写反也测不出（首跑就是这么漏的，
    //   变异「插值符号反号」存活）。实测该形状下正确符号误差 0.000、反号误差 0.600。
    std::vector<float> prof(64, 0.0f);
    for (int i = 0; i < 64; ++i)
        prof[static_cast<size_t>(i)] = std::clamp(static_cast<float>(i) - 39.8f, 0.0f, 1.0f);
    const float found = steepestEdgeColumn(prof);
    c.chk("边缘定位·1px 边亚像素 ±0.1px", std::abs(found - 40.3f) < 0.1f, std::to_string(found));

    // 局部扫速：主点处 = f·|dψ|；u=1 处 = 2 倍（透视修正 dx/dψ = f(1+u²)）。
    // 变异红线：u² 项漏掉 → 偏轴机位的扫速系统性偏低，闭式锚跟着偏。
    c.chk("局部扫速·主点 = f·dψ", std::abs(localSweepPxPerFrame(0.001f, 800.0f, 100.0f, 100.0f) - 0.8f) < 1e-5f);
    c.chk("局部扫速·u=1 处翻倍",
        std::abs(localSweepPxPerFrame(0.001f, 800.0f, 900.0f, 100.0f) - 1.6f) < 1e-5f);
    c.chk("焦距·fov45 @720 = 869.1", std::abs(verticalFocalPx(720, 45.0f) - 869.1f) < 0.2f,
        std::to_string(verticalFocalPx(720, 45.0f)));

    // 逐帧差分位：同帧 ⟹ 恰为 0；整体抬 0.5 ⟹ 恰为 0.5。变异红线：ROI 行列写反。
    cv::Mat f0 = cv::Mat::ones(8, 8, CV_64FC1) * 0.25, f1 = cv::Mat::ones(8, 8, CV_64FC1) * 0.75;
    const cv::Rect roi(1, 1, 5, 6);
    c.chk("帧差·同帧恰为 0", frameDeltaPercentile({ f0, f0 }, roi, 0.995) == 0.0);
    c.chk("帧差·整体抬 0.5 = 0.5", std::abs(frameDeltaPercentile({ f0, f1 }, roi, 0.995) - 0.5) < 1e-12);
}

/// 闭式单应（`temporal_homography_metric.cpp`）：Δψ=0 恒等 / 主点位移 = f·tanΔψ / 垂直线
/// 仍垂直 / 往返闭合 / 偏离主点位移更大。它住在闪烁这条线上是因为**同一个 φ 换算**：
/// localSweepPxPerFrame 的依据就是这里量到的 dx/dψ = f(1+u²)。
void checkYawHomographyClosedForm(Checker& c)
{
    // 闭式单应：Δψ=0 ⟹ 恒等；主点是不动点；垂直线仍垂直（x′ 与 y 无关）；往返闭合。
    // 变异红线：矩阵任一行写错 → 往返/不动点两条同时红（它们不依赖行的具体形式）。
    const double f = 869.1, cx = 640.0, cy = 360.0, dpsi = 0.00057530;
    const cv::Matx33d id = yawHomography(0.0, f, cx, cy);
    c.chk("单应·Δψ=0 是恒等（差比例因子）",
        std::abs(id(0, 0) / id(2, 2) - 1.0) < 1e-12 && std::abs(id(1, 1) / id(2, 2) - 1.0) < 1e-12
            && std::abs(id(0, 1)) < 1e-12 && std::abs(id(2, 0)) < 1e-12);
    const cv::Matx33d H = yawHomography(dpsi, f, cx, cy), Hi = H.inv();
    auto map = [](const cv::Matx33d& M, double x, double y) {
        const double w = M(2, 0) * x + M(2, 1) * y + M(2, 2);
        return std::array<double, 2>{ (M(0, 0) * x + M(0, 1) * y + M(0, 2)) / w,
            (M(1, 0) * x + M(1, 1) * y + M(1, 2)) / w };
    };
    // 主点处的位移 = f·tan(Δψ)（= 「扫速 → 每帧角」换算的逆命题，判据的立足点）。
    // ⚠ 纯旋转单应**没有实不动点**（u′=u ⟹ t(1+u²)=0 ⟹ u=±i），首跑按「主点是不动点」
    //   写断言被本条当场抓红——不动点是复的，别拿它当锚。
    const double shift = map(H, cx, cy)[0] - cx;
    c.chk("单应·主点位移 = f·tan(Δψ)", std::abs(shift - f * std::tan(dpsi)) < 1e-9,
        std::to_string(shift));
    c.chk("单应·主点处 y 不变", std::abs(map(H, cx, cy)[1] - cy) < 1e-9);
    const auto p1 = map(H, 300.0, 100.0), p2 = map(H, 300.0, 600.0);
    c.chk("单应·垂直线仍垂直", std::abs(p1[0] - p2[0]) < 1e-9);
    const auto rt = map(Hi, map(H, 123.0, 456.0)[0], map(H, 123.0, 456.0)[1]);
    c.chk("单应·往返闭合", std::abs(rt[0] - 123.0) < 1e-6 && std::abs(rt[1] - 456.0) < 1e-6,
        std::to_string(rt[0]));
    // 透视：偏离主点处同样的角度产生**更大**的像素位移（dx/dψ = f(1+u²)，localSweep 的依据）。
    const auto off = map(H, cx + 1000.0, cy);
    c.chk("单应·偏离主点位移更大", std::abs(off[0] - (cx + 1000.0)) > std::abs(shift) + 0.4,
        std::to_string(off[0] - (cx + 1000.0)));
}

// ── 批 13.14a：带 pitch 的解析 yaw 重映射（temporal_yaw_remap）────────────────
//
// 金标准 = **显式构造两帧相机基**（yaw 与 yaw+δ 各建一次，经世界系回投）。基的三列
// **逐字照抄** `rgl/camera.cppm` 的 `cameraRight/Up/Forward`（见 `engineBasis`），
// 与被测实现（合并成 `R_x(p)·R_y(−δ)·R_x(−p)` 一步）之间**没有任何可共享的中间约定**。
// 两者的公共假设只剩「yaw 会消掉」——那正是本判据要验的命题，所以这个对照**是独立的**。
//
// 为什么这条必须存在：本文件的**手推闭式连错两次**（求逆漏 `(ABC)⁻¹ = C⁻¹B⁻¹A⁻¹`
// 的顺序翻转一次、符号展开错一次），第二次错到 pitch≠0 时偏 1.76px 而 pitch=0 时
// 完全正确 —— 也就是说**只测 pitch=0 的退化情形抓不到它**。故金标准必须扫 pitch。
//
// ### ⚠ 但「独立金标准」也漏过一次：**共用的基**比共用的公式更危险
//
// 本判据第一版金标准与实现**都用** `R_y(yaw)·R_x(pitch)` 当相机基，而该矩阵第三列是
// `−fwd`（z 轴朝后）⟹ 两边**同时**把 yaw 符号弄反，对拍拿到 2.3e-13px 全绿，实机 φ
// 标定扫描却给出 argmin = **−1**（`alignedΔ` 比完全不对齐还大 29%）。
// ⟹ **自搭金标准能钉「实现 == 我的模型」，钉不了「模型 == 引擎」。** 剩下那一半
// 只能由实机数据裁决（`temporal-tail --motion camera` 的 φ 标定扫描）。
//
// ### 变异红线（2026-10-07 当场注入实测，四条各验一条，全部验红后才入账）
//
// | 注入的变异 | 变红的断言（读数） |
// |---|---|
// | 求逆顺序翻转 `R_x(−p)·R_y(δ)·R_x(p)`（**我犯的原错**） | ① 金标准 13.46px |
// | pitch 乘 0（退化成一维） | ① 金标准 7.74px ＋ ⑤ pitch 锚点 `max|Δv| = 0.000` |
// | **yaw 增量反号**（= 用朝后的基，本轮实机抓到的那个错） | ① 53.63px ＋ ③b 互逆 9.38px ＋ ④ 符号锚点 `map_x(中心)=643.04` ＋ **算子链合成帧对 `aligned = 2.02 × raw`** ＋ H⁻¹ 反向用例互换 |
// | `Ksi` 主点 y 符号写反（推导时差点写错的那一位） | ② δ=0 恒等 720px ＋ ① 730.9px ＋ ③b ＋ maxShiftPx |
//
// ⚠ 变异 2 的第一版写成把 `rotX` 整个删掉，拿到的是 `-Werror=unused-parameter` **编译红**
//   而不是判据红（skill `rgl-test-discipline` 记的那条坑）。改成 `pitchRad * 0.0`
//   保留参数使用，才拿到真正的判据红。
// ⚠ 变异 3 那一行**在修正前也存在**（当时读数 54.93px），却仍然没拦住这个错——因为它
//   只说「与金标准不符」，而金标准与实现共享同一个反了的约定。⟹ 入账时把「变红」与
//   「红得有意义」分开记；判据若抄实现的结论，它就不是判据（④ 当时正是绿的）。
/// testYawRemap 的针孔相机（1280×720、垂直 fov 45°、主点居中）+ **金标准**。
/// 抽成 fixture 的理由：两条测试共用，且金标准独立命名才看得出它**没有**复用被测实现。
struct YawRemapFixture {
    double f = 360.0 / std::tan(0.5 * 45.0 * 3.14159265358979323846 / 180.0);
    double cx = 640.0, cy = 360.0;
    cv::Size sz { 1280, 720 };
    /// 采样点：四角 + 中心 + 两条边上的点。⚠ 必须是**数组**而不是嵌套花括号的
    /// `initializer_list`——后者在这里推导成 void（`cannot use type 'void' as a range`）。
    inline static const double kPts[5][2] = { { 0, 0 }, { 640, 360 }, { 1279, 719 },
        { 100, 700 }, { 1200, 20 } };

    /// 引擎相机基：三列**逐字照抄** `rgl/camera.cppm` 的 `cameraRight/Up/Forward`。
    /// ⚠ 金标准必须用这个而不是 `rotY(yaw)·rotX(pitch)`——后者的第三列是 `−fwd`
    ///   （z 轴朝后），拿它配 `u = cx + c[0]/c[2]·f` 会把 yaw 符号静默反掉。金标准若与
    ///   被测实现**共用同一个朝后的基**，对拍再准也测不出符号（首轮 2.3e-13px 全绿的真相）。
    ///   逐列照抄 ⟹ 金标准与实现之间**没有任何可共享的中间约定**。
    static cv::Matx33d engineBasis(double yaw, double pitch)
    {
        const double cy = std::cos(yaw), sy = std::sin(yaw);
        const double cp = std::cos(pitch), sp = std::sin(pitch);
        return cv::Matx33d(
            cy, sy * sp, -sy * cp, // 行 0：right.x, up.x, fwd.x
            0.0, cp, sp,           // 行 1：right.y, up.y, fwd.y
            -sy, cy * sp, -cy * cp // 行 2：right.z, up.z, fwd.z
        );
    }
    static std::pair<double, double> apply(const cv::Matx33d& H, double x, double y)
    {
        const cv::Vec3d q = H * cv::Vec3d(x, y, 1.0);
        return { q[0] / q[2], q[1] / q[2] };
    }
    /// 金标准：像素 → t+1 帧相机方向 → 世界 → t 帧相机方向 → 像素（完全不做化简）。
    /// 两帧各用**显式**的引擎基投影，与被测单应完全无关。
    std::pair<double, double> truth(double x, double y, double yaw, double pitch, double d) const
    {
        const cv::Matx33d Rt = engineBasis(yaw, pitch), Rt1 = engineBasis(yaw + d, pitch);
        const cv::Vec3d w = Rt1 * cv::Vec3d((x - cx) / f, (cy - y) / f, 1.0);
        const cv::Vec3d c0 = Rt.t() * w;
        return { cx + c0[0] / c0[2] * f, cy - c0[1] / c0[2] * f };
    }
};

}  // namespace

/**
 * 运动态时域两条判据的纯函数自检（temporal-flicker / temporal-homography）。
 *
 * 每条带「变异红线」= 把被测实现改坏成什么样子，本条会红。
 * 数值锚点出处：aa_target 白板左边缘实机跑（0.125px/帧，ACES 前黑=0.0987 白=0.9840，
 * 闭式给 ρ_ideal=0.10219 / ρ_noAA=0.57451，实测 ρ=0.57490 → 差 0.07%）。
 *
 * 拆成四个 checkXxx（闭式锚点 / 反标定与半谱 / 边缘与扫速 / 闭式单应）：120 行与认知
 * 复杂度两条护栏逼的，缝按「问的是哪一个算子」切。
 */
void testTemporalFlicker(Checker& c)
{
    checkFlickerClosedForm(c);
    checkFlickerSpectrum(c);
    checkFlickerEdgeAndSweep(c);
    checkYawHomographyClosedForm(c);
}

/// ① 金标准对拍 + ③b 与既有闭式一致（**正确性**侧）。
void testYawRemapAgainstTruth(Checker& c)
{
    const YawRemapFixture fx;
    // 三个 pitch × 三个 yaw × 两个 δ。**同时**钉住正确性与「与 yaw 无关」
    // （金标准显式换 yaw，被测实现压根不含 yaw 参数 ⟹ 一旦 yaw 其实消不掉就红）。
    double worst = 0.0;
    for (const double p : { 0.0, -0.401425, 0.541052 })
        for (const double yw : { 0.0, 0.7, -1.3 })
            for (const double d : { 0.0035, 0.02 }) {
                const cv::Matx33d H = yawRemapHomography(d, p, fx.f, fx.cx, fx.cy);
                for (const auto& q : YawRemapFixture::kPts) {
                    const auto a = YawRemapFixture::apply(H, q[0], q[1]);
                    const auto b = fx.truth(q[0], q[1], yw, p, d);
                    worst = std::max(
                        { worst, std::abs(a.first - b.first), std::abs(a.second - b.second) });
                }
            }
    c.chk("yaw 重映射对金标准（显式两帧相机基，含 pitch）", worst < 1e-9,
        "worst=" + std::to_string(worst) + "px");

    // ③b pitch = 0 必须与既有闭式 `yawHomography` **互为逆**（不是相等！）。
    //   两条独立推导的**方向约定不同**：既有那条返回**前向**映射（t → t+1，所以
    //   `temporal_homography` 用之前 `buildSrcMap(H.inv(), …)` 取逆），本实现返回
    //   **src map**（t+1 → t，直接喂 remap）。
    //   ⚠ 本条第一版写的是「相等」并且**是绿的**——那时两边共享同一个朝后的基。它当时
    //   给我的信心恰好是错的：跨函数对拍只在对两边**方向语义**都写对时才有意义，
    //   而 docstring 里的方向没人验。⟹ 现在显式比 `legacy(δ).inv()`，把约定写进判据。
    const cv::Matx33d legacy = yawHomography(0.0035, fx.f, fx.cx, fx.cy).inv();
    const cv::Matx33d mine = yawRemapHomography(0.0035, 0.0, fx.f, fx.cx, fx.cy);
    double worstLegacy = 0.0;
    for (const auto& q : YawRemapFixture::kPts) {
        const auto a = YawRemapFixture::apply(legacy, q[0], q[1]);
        const auto b = YawRemapFixture::apply(mine, q[0], q[1]);
        worstLegacy = std::max(
            { worstLegacy, std::abs(a.first - b.first), std::abs(a.second - b.second) });
    }
    c.chk("pitch=0 与既有 yawHomography 互逆（方向约定：它是前向、我是 src map）",
        worstLegacy < 5e-3, "worst=" + std::to_string(worstLegacy) + "px（既有闭式是二阶近似）");
}

/// ② 恒等 / ③ 几何形状 / ④ 符号 / ⑤ pitch 不可忽略（**退化与方向**侧）。
void testYawRemapAnchors(Checker& c)
{
    const YawRemapFixture fx;

    // ② δ = 0 必须逐位恒等（反向用例：任何"顺手加的偏移/半像素修正"都会让它红）
    const YawMap id = buildYawMap(yawRemapHomography(0.0, -0.4, fx.f, fx.cx, fx.cy), fx.sz);
    double worstId = 0.0;
    for (int y = 0; y < fx.sz.height; y += 37)
        for (int x = 0; x < fx.sz.width; x += 41)
            worstId = std::max({ worstId,
                std::abs(static_cast<double>(id.mapX.at<float>(y, x)) - x),
                std::abs(static_cast<double>(id.mapY.at<float>(y, x)) - y) });
    c.chk("δ=0 逐位恒等", worstId < 1e-3,
        "worst=" + std::to_string(worstId) + "px（float map 精度）");

    // ③ pitch = 0 时**垂直线必须映到垂直线**（map_x 与 y 无关）——「绕竖轴转只改水平」的
    //   正确表述。⚠ 直觉版本「v 不变」是**错的**（本条第一版就栽在这）：透视除法给出
    //   v′ = v/(1 − u·tanδ)，分母与 u 耦合 ⟹ v 实测确实变 0.92px。那正是
    //   temporal_homography 头注释写的「垂直线仍映到垂直线，y′ 的分母与 x 耦合 =
    //   **透视收缩，不是仿射**」。把它机器化，防将来有人把它"优化"成仿射。
    const YawMap p0 = buildYawMap(yawRemapHomography(0.0035, 0.0, fx.f, fx.cx, fx.cy), fx.sz);
    double worstCol = 0.0;
    for (const int x : { 0, 200, 640, 1000, 1279 })
        for (const int y : { 0, 180, 360, 719 })
            worstCol = std::max(worstCol,
                std::abs(static_cast<double>(p0.mapX.at<float>(y, x)) - p0.mapX.at<float>(0, x)));
    c.chk("pitch=0 时垂直线映到垂直线（map_x 与 y 无关）", worstCol < 1e-3,
        "worst=" + std::to_string(worstCol) + "px");

    // ③b 透视收缩必须**在场**（阳性锚点）：v 的位移随 x 变化 ⟹ 不是仿射。
    //   与 ③ 配成一对：③ 说「x 方向不随 y 变」，③b 说「y 方向随 x 变」，两条同时成立
    //   才是纯旋转单应；少任何一条都退化成别的变换。
    //   ⚠ 必须取**远离主点**的行：v = (cy − y)/f 在 y = cy 处恰为 0 ⟹ v′ ≡ 0，
    //   收缩在那一行不显现（第一版取 y=360 主点行，测出 Δv ≡ 0 差点被读成「量具有问题」）。
    const double vAtLeft = static_cast<double>(p0.mapY.at<float>(100, 0)) - 100.0;
    const double vAtRight = static_cast<double>(p0.mapY.at<float>(100, 1279)) - 100.0;
    c.chk("透视收缩在场（v 位移随 x 变 ⟹ 非仿射）", std::abs(vAtLeft - vAtRight) > 0.5,
        "Δv(左) = " + std::to_string(vAtLeft) + " vs Δv(右) = " + std::to_string(vAtRight));

    // ④ 符号锚点（**本轮实机 φ 标定扫描 argmin = −1 逼出来的改写**）。
    //   从引擎公式直接推，不靠记忆：`right = (cos yaw, 0, −sin yaw)`，取正前方世界点
    //   P = (0,0,−d) ⟹ `right·P = d·sin yaw` 随 yaw **增大**而增大 ⟹ 世界固定点在画面里
    //   **右移** `f·tan δ`。src map 要取「上一帧这块内容在哪」⟹ 必须往**左**取。
    //   ⟹ δ > 0 时 `map_x(中心) < cx`。原断言写的是 `> 641`，与被测实现**同一个错约定**，
    //   所以它当时是绿的——锚点抄实现的结论 = 没有锚点。
    const double shifted = p0.mapX.at<float>(360, 640);
    c.chk("符号：δ>0 时 src map 往左取（引擎 right·P 随 yaw 增）", shifted < 639.0,
        "map_x(中心) = " + std::to_string(shifted) + "（期望 < 639；内容右移 f·tanδ）");

    // ⑤ **pitch 不可忽略**的阳性锚点：bistro 机位 pitch ≈ −23° 时，v 方向的位移必须
    //   达到亚像素量级。哪天有人把 pitch 参数退化掉，这条会红（②③③b 全都测不出来）。
    const double pitch = -23.0 * 3.14159265358979323846 / 180.0;
    const YawMap pm = buildYawMap(yawRemapHomography(0.0035, pitch, fx.f, fx.cx, fx.cy), fx.sz);
    double worstPitch = 0.0;
    for (int y = 0; y < fx.sz.height; y += 37)
        for (int x = 0; x < fx.sz.width; x += 41)
            worstPitch = std::max(worstPitch,
                std::abs(static_cast<double>(pm.mapY.at<float>(y, x)) - p0.mapY.at<float>(y, x)));
    c.chk("pitch 不可忽略（−23° 时 v 位移达亚像素）", worstPitch > 0.5,
        "max|Δv| = " + std::to_string(worstPitch) + "px/帧（实测 0.952）");
    c.chk("maxShiftPx 量级合理（0.2°/帧 ≈ 3px）", pm.maxShiftPx > 2.0 && pm.maxShiftPx < 5.0,
        "maxShiftPx = " + std::to_string(pm.maxShiftPx));
}

/**
 * **端到端算子链自检**：合成一对「纯 yaw」帧，φ 对齐必须把差塌掉。
 *
 * 为什么必须有（本轮实测教训）：`yawRemapHomography` 对金标准对拍到 2.3e-13px，**但那条
 * 自检只测矩阵**，从没测过 `buildYawMap` + `remapWith` 的**合成分支**（`dst = src∘H`
 * 还是 `src∘H⁻¹`）。应用侧反一次 ⟹ 净 **2δ** 位移 ⟹ 对齐后**比完全不对齐还差**，
 * 而首轮实机读数恰好就是这个形态（`alignedΔ = 1.29 × rawΔ`，量级也对上 2δ 的帧差）。
 *
 * 两帧由**各自显式的相机基**投影同一个球面图案生成（**不走被测单应**），⟹ 本条测的是
 * 「实现 == 我的模型」之外的东西：**算子链的合成本身**。
 * ⚠ 它仍测不到「引擎 `frame_yaw` 的正方向 ≠ 本变换的 δ 正方向」——那一半只能由
 * `temporal-tail --motion camera` 的 **φ 标定扫描**（实机数据）裁决。两条各守一半。
 */
void testSmearPhiChain(Checker& c)
{
    const cv::Size sz(256, 144);
    const double f = 869.117, cx = sz.width / 2.0, cy = sz.height / 2.0;
    const double d = 0.0035, pitch = -0.401425, yaw0 = 0.7;

    // 世界方向上的确定图案（与相机无关 ⟹ 两帧看的是同一个"场景"）。
    // 用高频正弦积而不是棋盘：边缘丰富、无周期性巧合，且不需要随机数。
    auto scene = [](const cv::Vec3d& w) {
        const cv::Vec3d v = w * (1.0 / std::max(1e-12, cv::norm(w)));
        return 0.5 + 0.5 * std::sin(18.0 * v[0]) * std::sin(14.0 * v[1] + 3.0 * v[2]);
    };
    auto render = [&](double yaw) {
        cv::Mat img(sz, CV_64F);
        const cv::Matx33d R = YawRemapFixture::engineBasis(yaw, pitch);
        for (int y = 0; y < sz.height; ++y)
            for (int x = 0; x < sz.width; ++x)
                img.at<double>(y, x) = scene(R * cv::Vec3d((x - cx) / f, (cy - y) / f, 1.0));
        return img;
    };
    const cv::Mat prev = render(yaw0), cur = render(yaw0 + d);

    const cv::Matx33d H = yawRemapHomography(d, pitch, f, cx, cy);
    const cv::Mat aligned = remapWith(prev, buildYawMap(H, sz));

    // 只在中部统计：remap 的 BORDER_CONSTANT 带会造出巨大假差（首轮差点把它读成信号）
    const cv::Rect roi(40, 24, sz.width - 80, sz.height - 48);
    const double raw = cv::mean(cv::abs(cur(roi) - prev(roi)))[0];
    const double res = cv::mean(cv::abs(cur(roi) - aligned(roi)))[0];
    c.chk("φ 算子链把合成纯旋转帧对的差塌掉（合成分支不是 H⁻¹）",
        res < 0.15 * raw && raw > 1e-3,
        "raw=" + std::to_string(raw) + " → aligned=" + std::to_string(res) + "（比值 "
            + std::to_string(raw > 0.0 ? res / raw : 0.0) + "，线 < 0.15）");

    // 反向：喂 H⁻¹ 必须**不塌**，否则上面那条是空洞绿。
    // 注：本变换族满足 `H(δ)⁻¹ = H(−δ)`（纯旋转单应，取逆 = 反向转同一个角），
    // 所以这条反向用例同时就是「应用侧符号反了」的**阳性对照**：首轮实机读数的形态
    // （alignedΔ > rawΔ）正是它该有的样子，而当时没有任何一条自检能复现它。
    const double wrong
        = cv::mean(cv::abs(cur(roi) - remapWith(prev, buildYawMap(H.inv(), sz))(roi)))[0];
    c.chk("同图喂 H⁻¹ 必须不塌（防空洞绿）", wrong > 3.0 * res,
        "H⁻¹ 残差 " + std::to_string(wrong) + " vs 正确 H 残差 " + std::to_string(res));
}

}  // namespace RGL::imgqa
