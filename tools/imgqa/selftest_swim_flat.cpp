// selftest_swim_flat.cpp —— **像素域地面搬运**口径层（`shadow_transport_metric.cpp`）的自检。
//
// 全部闭式、不启引擎（判据：算子坏了读数也是假的，先钉算子）。这层是债-139 结案批那台仪器的
// 分母与分子的唯一出处 ⟹ 这里红的四格读数一个都不作数。
//
// ## 测试的构造方式：世界场正向栅格化，**不做任何图像重采样**
// 首版拿 `slideField`（把 A 的图按世界平移重采样成 B 的图）造「同一个世界场的两枚机位」，那是
// 错的两次：① 重采样本身用最近邻，边界上凭空造出真值里没有的差 ⟹ 「失配恰 0」永远不成立，
// 只能把断言放松成「比零位移小」= 放松后的断言不再能区分「搜索找到了」与「搜索没找到」；
// ② 更糟的是它**测不到恒等**——「同一张图对拍自己」在两台机位下根本不是恒等映射（A 的第 100 列
// 与 B 的第 100 列是两个世界点），首版 ④ 就是这样报出 1579 翻转的。
// 现在的构造是**正向**的：先定义一个只吃世界 xz 的函数 W，再分别栅格化进两枚机位的网格。
// 「同一个世界点在两机位下影状态相同」这件事于是由构造保证，而算子若正确就必须读出 0。
//
// ## 拆成六个测试函数的缝（按**问的东西**切，不是按行数）
//   · `testShadowTransportRoundTrip`  ① 往返恒等（🔴 立命前提，像素中心约定）+ ② 投出画幅 = NaN
//   · `testShadowTransportTranslation` ③ 横移不变性（两机位对拍的成立条件）
//   · `testShadowTransportIdentity`    ④ 同一世界场两机位：erode 0 必翻（地板在）+ ⑤ 无关场显著失配
//   · `testShadowTransportErosion`     ⑥⑦⑧ 侵蚀带：把地板剔干净、两臂并集、不是靠删分母
//   · `testShadowTransportSearch`      ⑨ 注入平移**同号**回收且该处失配恰 0 + ⑩ 目标函数 + ⑪ 无基准
//   · `testShadowTransportGuards`      ⑫ 类型/尺寸守卫各抛 + ⑬ 五条判决线挂号
//
// ## 变异红线（哪个变异让我红；逐条实跑记录见交付说明）
//   · `projectSamples` 的 −0.5 删掉（像素边缘/中心混用）→ ① 红（往返差半个像素），且 ④⑥⑨ 一起红
//     ——四格所有影缘翻转立刻变成假号。这条是这层最要紧的锚点，因为它错了**所有**读数都错得
//     看起来很合理。
//   · `groundHitDouble` 的 aspect 漏乘 → ①③ 双红。
//   · `ny` 少一次翻转（y 轴方向）→ ① 红（上下镜像）。
//   · `edgeBandMask` 写回「闭运算 − 开运算」→ ⑥ 红（对半平面恒零 = 根本没侵蚀，地板回到读数里）。
//   · 侵蚀带只查 A 臂（`onBandA || onBandB` 改成 `onBandA`）→ ⑦ 红（B 侧的缘留在分母里）。
//   · 侵蚀半径不参与（band 恒空）→ ⑥ 红（刀锋场的重采样地板回到读数里）。
//   · 🔴 搜索钉成恒零位移（`rounds` 全跳过 / 直接 `return best`）→ ⑨ 红（注入的平移报不出来）。
//     这条就是任务书钦定的那条变异。
//   · 目标函数从 Σ|Δcov| 换回翻转数优先 → ⑨ 红（本靶二值本影只占 0.14 %，翻转数会把正确答案
//     判成更差：注入 0.5 m 时零位移读 5、真答案处读 34）。实跑记录见 `shadow_transport_metric.h`
//     的「变异红线」段（M1–M5 逐条读数，含**存活**的 M5）。
//   · 覆盖率下限删掉 → ⑪ 红（零基准被报成「最优 = 零位移零失配」）。
//   · `compareTransported` 的类型/尺寸守卫改成返回空读数 → ⑫ 红（空读数 = 零翻转 = 假绿）。
//   · 判据名拼错或漏 append → ⑬ 红。
#include "selftest.h"

#include "imgqa.h"
#include "shadow_transport_metric.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

// 画幅取 480×320 而不是更小：像素足迹必须与**实机同量级**，否则侵蚀带的语义会变。本几何下
// 12 m 处足迹 = 2·12·tan20°/320 ≈ 2.7 cm ⟹ ±2 像素的带 = ±5.5 cm，与实机（1600×900，6 m 处
// 足迹 4.9 mm、带 ±1 cm）是同一个「带 ≪ 影缘间距、带 ≪ 注入量」的形态。首版用 240×160 时带
// 宽 0.11 m，注入 0.30 m 只比带宽大 3 倍 ⟹ 搜索极小值是一片平台，报回 (0.26, 0.16)。
constexpr int kW = 480;
constexpr int kH = 320;
constexpr double kFov = 40.0;
/**
 * 世界棋盘格边长（米）。🔴 这个数与相机几何是**一对**，必须同时满足三条，改一条要重推另两条
 * （三条各由一条断言把守，任何一条不满足都有断言红，不会静默）：
 * ① 侵蚀带（±2 像素）只能吃掉分母的一小截 —— 否则「翻转恰 0」只是把分母删了（⑧ 拦这条）；
 * ② 注入的滑移必须**远大于侵蚀带宽** —— 带内的平移彼此不可分辨，注入量落在带里就等于没注入
 *    （⑨ 拦这条：报回来的平移会一路漂到零附近）；
 * ③ 场里得有足够多条影缘，否则平移搜索在 x/z 两轴退化（⑩ 的无关场失配率拦这条）。
 * 首版用「相机高 4.6 m 俯视 + 格 0.40 m」：掠射区把像素足迹拉到 ~0.11 m ⟹ 2 像素带 = 0.22 m，
 * ①②同时崩（带吃掉 92 % 分母、注入 0.30 m 被报成 0.08）。改成**近距平视**（高 1.6 m、
 * 5 m 处足迹 ≈ 2.3 cm）后三条同时成立。
 */
constexpr double kCellWorldM = 1.20;
/// 注入搜索测试的世界滑移（米）。≈ 13 枚像素足迹 = 侵蚀带（±2 像素）的 6 倍 ⟹ 落在带外，
/// 「搜索能不能定位滑移」这件事才是可分辨的（条件 ②）。
constexpr double kInjectM = 0.30;
/// 地面交点距离上限（米）。🔴 与仪器同一个口径：不设上限时掠射区会进采样集，那里**一像素跨
/// 好几个棋盘格** ⟹ 侵蚀带（±2 像素）在结构上盖不住影缘 ⟹ 分母被带吃掉 76 %、平移搜索的极小值
/// 变成一片平台（本自检首版就是这样：注入 0.10 m 报回 0.08/0.03）。仪器侧同一条限制写在
/// `shadow_transport_metric.h` 的 `groundHitDouble` 上，不是仪器专属。
constexpr double kMaxDistM = 12.0;
/// 一个粗网格步长（= 本量具能分辨最小平移的口径，仪器侧 J-F5 的容差同式）。
constexpr double kCoarseStepM = 0.10;

/// 栅格化用的地面距离上限（米）。🔴 **必须严格大于**采样上限 `kMaxDistM`，否则搬运会落到
/// 「没栅格化」的像素上：A 里 12 m 处的地面点，搬到 B 的网格里对应的是 B 眼里的 12.5 m 处，
/// 若栅格化也按 12 m 截断，那里就被写成 0.0（= 受光），与真正的 0.02 同标签不同值 ⟹ 双线性
/// 插出中间值 ⟹ 世界不变的场读不出 Σ|Δcov| = 0（实测 0.54 的残差挂在 77205 个点上）。
/// 实机不吃这个亏（cov 场是整幅截图算的，「不是地面」由光度可信集挡），但合成场必须自己补上。
constexpr double kRasterDistM = 25.0;

Engine::Camera camA() { return { { { 0.0, 1.6, 3.0 } }, { { 0.0, 0.0, -2.0 } }, kFov }; }
Engine::Camera camB() { return { { { 0.5, 1.6, 3.0 } }, { { 0.5, 0.0, -2.0 } }, kFov }; }

/**
 * **世界场** W：只吃世界 xz，与任何相机无关。`sx/sz` = 把整个场沿世界挪这么多（注入缺陷态）。
 * 二值棋盘（0.95 本影 / 0.02 受光）⟹ 影状态 = 棋盘格奇偶，边界是**刀锋**（无过渡带），
 * 正是本仪器要分辨的那种影场（PCSS 的 1 像素半影在这里由双线性插值给出，形态一致）。
 */
double worldCov(double x, double z, double sx, double sz)
{
    const long ix = static_cast<long>(std::floor((x - sx) / kCellWorldM));
    const long iz = static_cast<long>(std::floor((z - sz) / kCellWorldM));
    return ((ix + iz) & 1L) ? 0.95 : 0.02;
}

/// 把世界场 W 栅格化进 `cam` 的网格（**正向**：像素 → 世界 → 取值，绝不做图像域重采样）。
cv::Mat rasterWorld(const Projector& cam, double sx, double sz)
{
    cv::Mat f(kH, kW, CV_64FC1, cv::Scalar(0.0));
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            double wx = 0.0;
            double wz = 0.0;
            if (groundHitDouble(cam, x, y, kRasterDistM, wx, wz))
                f.at<double>(y, x) = worldCov(wx, wz, sx, sz);
        }
    return f;
}

/// 确定性伪随机**世界**场（Knuth MMIX LCG，写死种子：自检必须跨机器逐位可复现，禁 random_device）。
/// 与 `worldCov` 一样是世界的函数，只是换成随机块 ⟹ 平移搜索在 x/z 两轴都不退化。
double worldNoise(double x, double z, int blocks)
{
    const long ix = static_cast<long>(std::floor(x / kCellWorldM)) % blocks;
    const long iz = static_cast<long>(std::floor(z / kCellWorldM)) % blocks;
    uint64_t h = (static_cast<uint64_t>(ix + blocks) << 32) | static_cast<uint64_t>(iz + blocks);
    h = h * 6364136223846793005ULL + 1442695040888963407ULL;
    h ^= h >> 33;
    return ((h >> 7) & 1U) ? 0.95 : 0.02;
}

cv::Mat rasterWorldNoise(const Projector& cam, double sx, double sz, int blocks)
{
    cv::Mat f(kH, kW, CV_64FC1, cv::Scalar(0.0));
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            double wx = 0.0;
            double wz = 0.0;
            if (groundHitDouble(cam, x, y, kRasterDistM, wx, wz))
                f.at<double>(y, x) = worldNoise(wx - sx, wz - sz, blocks);
        }
    return f;
}

/// 搜索用的两轮网格：粗 ±0.40 m 步 0.10、细 ±0.10 m 步 0.02（与仪器同形态，候选数写死）。
std::vector<std::vector<std::array<double, 2>>> twoRounds()
{
    std::vector<std::vector<std::array<double, 2>>> rounds;
    std::vector<std::array<double, 2>> coarse;
    for (int iz = -4; iz <= 4; ++iz)
        for (int ix = -4; ix <= 4; ++ix)
            if (ix || iz)
                coarse.push_back({ ix * kCoarseStepM, iz * kCoarseStepM });
    std::vector<std::array<double, 2>> fine;
    for (int iz = -5; iz <= 5; ++iz)
        for (int ix = -5; ix <= 5; ++ix)
            if (ix || iz)
                fine.push_back({ ix * 0.02, iz * 0.02 });
    rounds.push_back(coarse);
    rounds.push_back(fine);
    return rounds;
}

bool hasJudgment(const char* name)
{
    for (const auto& j : registeredJudgments())
        if (j.name == std::string(name))
            return true;
    return false;
}

}  // namespace

void testShadowTransportRoundTrip(Checker& c)
{
    const Projector p = makeProjector(camA(), kW, kH);
    // ① 往返恒等（🔴 立命前提）：像素 → 世界 y=0 → 同一枚相机 → 必须落回原像素。
    // 这一条钉的是「本层的正逆两向是同一个相机模型」，以及像素中心约定（差 0.5 就红）。
    double worst = 0.0;
    int samples = 0;
    for (const int y : { 20, 90, 160, 240, 300 })
        for (const int x : { 10, 120, 240, 360, 470 }) {
            double wx = 0.0;
            double wz = 0.0;
            if (!groundHitDouble(p, x, y, kMaxDistM, wx, wz))
                continue;
            GroundSamples s;
            s.plane = groundPlane();
            s.srcIndex = { y * kW + x };
            s.u = { wx };
            s.v = { wz };
            std::vector<double> dx;
            std::vector<double> dy;
            projectSamples(p, s, 0.0, 0.0, dx, dy);
            if (dx.size() != 1 || !std::isfinite(dx[0]))
                continue;
            worst = std::max({ worst, std::abs(dx[0] - x), std::abs(dy[0] - y) });
            ++samples;
        }
    c.chk("transport·往返恒等：像素→地面→同机位像素 落回原像素（±1e-6，钉像素中心约定）",
        samples >= 12 && worst < 1e-6,
        "样本 " + std::to_string(samples) + "，最大偏差 " + std::to_string(worst));

    // ② 投出画幅 / 相机背后 = NaN（不许夹到边缘：夹 = 把屏外读成边缘像素的值）
    const Projector behind = makeProjector({ { { 0.0, 1.0, 0.0 } }, { { 0.0, 1.0, 10.0 } }, kFov },
        kW, kH);
    GroundSamples gs;
    gs.plane = groundPlane();
    gs.srcIndex = { 0 };
    gs.u = { 0.0 };
    gs.v = { 0.0 };
    std::vector<double> bx;
    std::vector<double> by;
    projectSamples(behind, gs, 0.0, 0.0, bx, by);
    c.chk("transport·世界点在该机位背后 → NaN（不夹边、不当 0 用）",
        bx.size() == 1 && !std::isfinite(bx[0]), bx.empty() ? "空" : std::to_string(bx[0]));
}

void testShadowTransportTranslation(Checker& c)
{
    // ③ 横移不变性（两机位对拍的成立条件）：相机沿 +X 挪 d ⟹ 同一像素的世界点恰平移 d。
    // 这条不成立，「同一个世界点在两机位下必须同影状态」就是一句空话。
    const Projector a = makeProjector(camA(), kW, kH);
    const Projector b = makeProjector(camB(), kW, kH);
    const double d = 0.5;
    double worst = 0.0;
    int samples = 0;
    for (const int y : { 20, 90, 160, 240, 300 })
        for (const int x : { 10, 120, 240, 360, 470 }) {
            double wax = 0.0;
            double waz = 0.0;
            double wbx = 0.0;
            double wbz = 0.0;
            if (!groundHitDouble(a, x, y, kMaxDistM, wax, waz))
                continue;
            if (!groundHitDouble(b, x, y, kMaxDistM, wbx, wbz))
                continue;
            worst = std::max(worst, std::abs((wbx - wax) - d));
            worst = std::max(worst, std::abs(wbz - waz));
            ++samples;
        }
    c.chk("transport·横移不变性：相机沿 +X 挪 d ⟹ 同一像素的世界点恰平移 d（与深度无关）",
        samples >= 12 && worst < 1e-9,
        "样本 " + std::to_string(samples) + "，最大偏差 " + std::to_string(worst));
}

void testShadowTransportIdentity(Checker& c)
{
    const Projector a = makeProjector(camA(), kW, kH);
    const Projector b = makeProjector(camB(), kW, kH);
    const GroundSamples s = sampleGround(a, kW, kH, kMaxDistM);
    std::vector<double> dx;
    std::vector<double> dy;
    projectSamples(b, s, 0.0, 0.0, dx, dy);

    // ④ 阳性锚点（**世界不变**的场）：同一个世界场栅格化进两枚机位 ⟹ 零侵蚀必须翻（那是量具的
    // 重采样地板，双线性在刀锋上必然插出中间值），侵蚀后必须**恰 0**。
    // 🔴 首版这里拿「同一张图对拍自己」当恒等锚点，是错的：两枚机位下同一张图 = 两个不同的世界
    // 场，实测报 1579 翻转。恒等的定义只能是「同一个世界函数」。
    const cv::Mat covA = rasterWorld(a, 0.0, 0.0);
    const cv::Mat covB = rasterWorld(b, 0.0, 0.0);
    const ShadowFlipReadout raw = compareTransported(covA, covB, s, dx, dy, {}, {}, {}, 0);
    c.chk("transport·世界不变场在两机位下**零侵蚀必翻**（重采样地板确实在读数里，不是判据坏了）",
        raw.trust > 0 && raw.flips > 0,
        std::to_string(raw.flips) + "/" + std::to_string(raw.trust) + " = "
            + std::to_string(raw.flipPct()) + "%");

    // ⑤ 反向用例：两个**结构不同**的世界场必须显著失配（堵「算子恒绿」）。
    // 用「棋盘 vs 随机块」而不是「同一场挪一点点」：后者在格尺度上仍是同一场，翻转率会随挪量
    // 连续变化，作为「显著」的下界不稳。
    const cv::Mat other = rasterWorldNoise(b, 0.0, 0.0, 11);
    const ShadowFlipReadout unrel = compareTransported(covA, other, s, dx, dy, {}, {}, {}, 0);
    c.chk("transport·不同世界场翻转率 ≥ 20%（反向用例：算子不是恒绿）",
        unrel.trust > 0 && unrel.flipPct() >= 20.0,
        std::to_string(unrel.flips) + "/" + std::to_string(unrel.trust) + " = "
            + std::to_string(unrel.flipPct()) + "%");
}

void testShadowTransportErosion(Checker& c)
{
    const Projector a = makeProjector(camA(), kW, kH);
    const Projector b = makeProjector(camB(), kW, kH);
    const GroundSamples s = sampleGround(a, kW, kH, kMaxDistM);
    std::vector<double> dx;
    std::vector<double> dy;
    projectSamples(b, s, 0.0, 0.0, dx, dy);
    const cv::Mat covA = rasterWorld(a, 0.0, 0.0);
    const cv::Mat covB = rasterWorld(b, 0.0, 0.0);

    // ⑥ 🔴「无地板」三个字的机器定义：世界不变的场，零侵蚀有翻转、侵蚀 2 像素后**恰 0**。
    // 这一条同时钉两件事：侵蚀带确实把重采样地板剔干净了（不是靠放松阈值），且剩下的内部像素
    // 上算子给的是**真 0**（不是「小于某个小数」）。
    const cv::Mat bandA = edgeBandMask(covA, 2);
    const cv::Mat bandB = edgeBandMask(covB, 2);
    const ShadowFlipReadout raw = compareTransported(covA, covB, s, dx, dy, {}, {}, {}, 0);
    const ShadowFlipReadout e2
        = compareTransported(covA, covB, s, dx, dy, {}, bandA, bandB, 2);
    // 🔴 拆成三条而不是合成一条：这条是「无地板」的机器定义，红了必须一眼看出是**哪半句**塌了
    //（翻转不是 0 / 分母被删光 / 连续量没归零），合成一条每次都要重跑一遍才知道。
    c.chk("侵蚀带·世界不变场侵蚀 2 像素后翻转**恰 0**（= 无地板读数的定义）",
        e2.flips == 0 && e2.trust > 0 && e2.edgeExcluded > 0,
        "可信 " + std::to_string(e2.trust) + " 剔除 " + std::to_string(e2.edgeExcluded)
            + " 翻转 " + std::to_string(e2.flips));
    // Σ|Δcov| 的下界只能是**浮点闭合级**，不是「逐位 0」：双线性四个权重 (1−fx)(1−fy)+fx(1−fy)
    // +(1−fx)fy+fx·fy 在浮点里不恰为 1（差 ~1e-17），于是四个抽头等值时插出来的仍是 a·(1±1e-17)。
    // 首版把它写成 `== 0.0`，红了一次报「Σ = 0.000000」——打印精度盖住了 1e-17，看着像判据坏了。
    // 🔴 真正的**逐位**锚点是上面那条二值翻转恰 0；连续量在这里能验到的最强陈述是「比任何真实
    // 残差小 12 个数量级」（仪器阈值 2.0 ‰ = 2e-3，本界 1e-9）。
    c.chk("侵蚀带·侵蚀后 Σ|Δcov| 落在**浮点闭合级**（连续量口径的阳性锚点；真实残差会大 12 个量级）",
        e2.sumAbsCov < 1e-9 && e2.p99AbsCov < 1e-9,
        "Σ = " + std::to_string(e2.sumAbsCov) + "，p99 = " + std::to_string(e2.p99AbsCov));

    // ⑦ 两臂并集：只查 A 臂的带**不够**。判据 = 单独给 B 臂的带也必须剔到点（A 臂的带剔不到那些），
    // 且「只给 A 臂」与「两臂都给」的剔除数不同。
    const cv::Mat noBand;
    const ShadowFlipReadout onlyA
        = compareTransported(covA, covB, s, dx, dy, {}, bandA, noBand, 2);
    const ShadowFlipReadout onlyB
        = compareTransported(covA, covB, s, dx, dy, {}, noBand, bandB, 2);
    c.chk("侵蚀带·B 臂的带单独也给就剔到点（两臂并集，不是只查一臂）",
        onlyB.edgeExcluded > 0 && onlyA.edgeExcluded > 0 && onlyB.edgeExcluded != onlyA.edgeExcluded
            && e2.edgeExcluded > onlyA.edgeExcluded && e2.edgeExcluded > onlyB.edgeExcluded,
        "并集 " + std::to_string(e2.edgeExcluded) + " 只A " + std::to_string(onlyA.edgeExcluded)
            + " 只B " + std::to_string(onlyB.edgeExcluded));

    // ⑧ 剔分母不等于剔问题：侵蚀后剩下的可信点必须**仍然成规模**。
    // 🔴 不写成「原集的多数」：带面积是场的**影缘密度**的函数（棋盘是横竖两族缘，交叉成网格），
    // 首版那条写成 `> raw.trust / 2` 在一个合法的高缘密度场上就红了——它想拦的是「把分母删光」，
    // 不是「这个场缘多」。改成两条真下限：绝对点数（分母不能是彩票）+ 相对比例（不能删光）。
    // 仪器侧对应的闸门是 G4（≥ 2 万点且 ≥ 全幅 5 %），本靶实测带只吃 ~10 %。
    c.chk("侵蚀带·侵蚀后可信集仍是**成规模的分母**（不是靠删光分母得到 0）",
        e2.trust > 2000 && e2.trust > raw.trust / 10,
        std::to_string(e2.trust) + " vs " + std::to_string(raw.trust) + "（带吃掉 "
            + std::to_string(100.0 * static_cast<double>(e2.edgeExcluded)
                / static_cast<double>(raw.trust ? raw.trust : 1))
            + "%）");
}

void testShadowTransportSearch(Checker& c)
{
    const Projector a = makeProjector(camA(), kW, kH);
    const Projector b = makeProjector(camB(), kW, kH);
    const GroundSamples s = sampleGround(a, kW, kH, kMaxDistM);

    // ⑨ 注入 kInjectM 的世界滑移必须被**同号**报回，且该处失配**恰 0**。
    // 🔴 这条就是任务书钦定的变异「把平移搜索钉成恒 0」的判据本体：搜索恒返回 (0,0) 时它红。
    // 容差取一个粗网格步长，**不是**偷懒：侵蚀带内的平移彼此不可分辨 ⟹ 本量具的平移分辨率在
    // 结构上就等于「搜索步长」与「侵蚀带宽」里的粗者。写「恰等于注入量」会把这条正确的物理限制
    // 当成 bug 去修。仪器侧 J-F5 用的是同一个口径（一个粗网格步长）。
    const cv::Mat covA = rasterWorldNoise(a, 0.0, 0.0, 7);
    const cv::Mat slid = rasterWorldNoise(b, kInjectM, kInjectM, 7);
    const cv::Mat bandA = edgeBandMask(covA, 2);
    const cv::Mat bandB = edgeBandMask(slid, 2);
    const ShiftCandidate got = searchWorldShift(
        covA, slid, s, {}, bandA, bandB, b, twoRounds(), 0.8);
    c.chk("search·注入 (0.30,0.30) m 世界滑移 → 同号报回（±一个粗网格步长）且该处失配恰 0",
        got.found && std::abs(got.sx - kInjectM) <= kCoarseStepM
            && std::abs(got.sz - kInjectM) <= kCoarseStepM && got.flips == 0 && got.trust > 0,
        "(" + std::to_string(got.sx) + "," + std::to_string(got.sz) + ") 失配 "
            + std::to_string(got.flips) + "/" + std::to_string(got.trust));

    // ⑨b 世界不变的场 → 最优平移必须**恰为零位移**（反向钉 ⑨）。
    // 两条合起来才排除两种相反的失效：搜索恒报 0（⑨ 红）/ 搜索恒报非零或带内乱漂（本条红）。
    // 本条同时钉 tie-break：同失配时取**平移最小者**，否则最优解会在侵蚀带内随机漂。
    const cv::Mat same = rasterWorldNoise(b, 0.0, 0.0, 7);
    const ShiftCandidate aligned = searchWorldShift(
        covA, same, s, {}, bandA, edgeBandMask(same, 2), b, twoRounds(), 0.8);
    c.chk("search·世界不变的场 → 最优平移**恰为零位移**（Occam tie-break，J-F1 的判据本体）",
        aligned.found && aligned.sx == 0.0 && aligned.sz == 0.0 && aligned.flips == 0,
        "(" + std::to_string(aligned.sx) + "," + std::to_string(aligned.sz) + ") 失配 "
            + std::to_string(aligned.flips));

    // ⑩ 无关场之间搜索不许「总能找到 0」。
    // 两条实测教训叠在这条上（都是本批真踩的）：
    //   · 目标只拿 |Δcov| 之和、分母又随候选缩水时，真激励 0.5 m 的最优平移被报成 (−0.5,−0.5)
    //     且失配 0——「挪到两边都没有影」既减样本又减差，是条廉价得多的路。
    //   · 目标只拿翻转数、而本靶二值本影只占 0.14 % 时，翻转数几乎恒 0 ⟹ 极小值是一片平台，
    //     最优解落在 (−0.42,−0.60) 这种地方。
    // 现在两条都堵：分母固定在零位移 + 字典序 (翻转数, Σ|Δcov|)。
    const cv::Mat other = rasterWorldNoise(b, 0.31, -0.13, 11);
    const ShiftCandidate unrel = searchWorldShift(
        covA, other, s, {}, bandA, edgeBandMask(other, 2), b, twoRounds(), 0.8);
    c.chk("search·无关世界场搜完仍有大量失配（搜索不是「总能找到 0」的机器）",
        unrel.found && unrel.trust > 0 && unrel.flips > 0,
        "最优 (" + std::to_string(unrel.sx) + "," + std::to_string(unrel.sz) + ") 失配 "
            + std::to_string(unrel.flips) + "/" + std::to_string(unrel.trust));

    // ⑪ 覆盖率下限：可信基准为 0 时不许报「找到最优」（不许伪装成零失配）
    const cv::Mat empty = cv::Mat(kH, kW, CV_64FC1, cv::Scalar(0.0));
    const GroundSamples noSamples;
    const std::vector<std::vector<std::array<double, 2>>> none;
    const ShiftCandidate nodata = searchWorldShift(empty, empty, noSamples, {}, {}, {}, b, none, 0.8);
    c.chk("search·零可信基准 → found=false（不许把「没有数据」报成「最优是零位移零失配」）",
        !nodata.found, nodata.found ? "报成了找到" : "如实报没有基准");
}

void testShadowTransportGuards(Checker& c)
{
    const Projector a = makeProjector(camA(), kW, kH);
    const GroundSamples s = sampleGround(a, kW, kH, kMaxDistM);
    std::vector<double> dx;
    std::vector<double> dy;
    projectSamples(a, s, 0.0, 0.0, dx, dy);
    const cv::Mat f = rasterWorld(a, 0.0, 0.0);

    // ⑫ 守卫一律抛：空读数会被读成「零翻转 = 判据通过」（本仓 skill 明写的那条）
    cv::Mat wrongType;  // 🔴 必须让它自己分配：convertTo 回同一个 Mat 会撞 OpenCV 的
                        //    「Can't reallocate Mat with locked type」，那是工具自己炸，不是判据
    f.convertTo(wrongType, CV_32FC1);
    c.chk("compareTransported·非 CV_64FC1 必须抛（空读数=零翻转=假绿）",
        throwsStd([&] { (void)compareTransported(wrongType, wrongType, s, dx, dy, {}, {}, {}, 0); }));
    const cv::Mat otherSize = cv::Mat::zeros(kH + 4, kW, CV_64FC1);
    c.chk("compareTransported·两臂尺寸不同必须抛",
        throwsStd([&] { (void)compareTransported(f, otherSize, s, dx, dy, {}, {}, {}, 0); }));
    c.chk("compareTransported·目标坐标长度不符必须抛（调用侧接错线）", throwsStd([&] {
              std::vector<double> shortX;
              shortX.push_back(1.0);
              (void)compareTransported(f, f, s, shortX, shortX, {}, {}, {}, 0);
          }));
    c.chk("edgeBandMask·非 CV_64FC1 必须抛", throwsStd([&] { (void)edgeBandMask(wrongType, 2); }));
    c.chk("photometricTrustMask·非 CV_64FC1 必须抛",
        throwsStd([&] {
            std::vector<double> d;
            (void)photometricTrustMask(wrongType, wrongType, s, d, d, 0.02);
        }));

    // ⑬ 五条判决线挂号在场（容差理由与 guard 的**内容**由 testRegistry 通用强制）
    for (const char* name : { "shadow-swim-flat-floor", "shadow-swim-flat-determinism",
             "shadow-swim-flat-no-harm", "shadow-swim-flat-benefit",
             "shadow-swim-flat-offset-sensitivity" })
        c.chk("挂号：shadow-swim-flat 五条判决线在注册表里", hasJudgment(name), name);
}

}  // namespace RGL::imgqa
