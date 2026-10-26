// shadow-swim-flat —— **零起伏平地上影场的世界不变性**（债-139 结案批的仪器）。
//
// ── 它补的是哪一类失明 ────────────────────────────────────────────────────────────────
// 债-139 的 2.73 % 残差是在 **bistro 鹅卵石地面**上量的，结案时留了一句没验的话：那 2.73 %
// 里有多少是「阴影真的随相机变」，有多少是**测量地板**（起伏地面的重投影误差 + 影缘重采样）。
// 本仪器把地面换成**零起伏平面**（`shadow_edge_rig` 的接收面 y=0），把「地面几何误差」这个
// 混淆项**按构造删掉**，再给像素域搬运加一条**影缘侵蚀带**把剩下的重采样地板显式剔出分母。
// ⟹ 读数是**无地板**的：侵蚀带之外任何翻转都不是重采样能造出来的。
//
// ── 与既有三台仪器的分工（这条必须写清，否则本仪器就是抄第三份）──────────────────────
//   · `swim`（`rig_shadow_field.cpp`）—— 同一件事的 **5 cm 世界格 + 格内多数票**口径。本批实测
//     它**结构性看不见**这条缺陷：同一份数据，5 cm 格读 **0.036 %**、像素域读 **0.256 %**（差
//     7 倍）。原因不是阈值，是分辨率——毫米级刀锋微影场在 5 cm 格里被多数票抹平。这是 Q3 的答。
//   · `material-swim`（债-152）—— 同一套「两机位 + 世界对拍 + 平移搜索」的形状，但比的是
//     **G-buffer roughness**（水洼场），且用的是 5 cm 格口径。本仪器比的是**阴影覆盖度**，
//     且必须换像素域（理由见 `shadow_transport_metric.h` 头注 ①②）。
//   · `shadow-edge-rebase` —— 同一个四格（2 工作点 × 2 开关态）、同一枚靶，但那条问的是
//     「影边的**绝对位置**对不对」（真值 = 闭式算术），本条问的是「影场**随不随相机滑**」
//     （真值 = 另一个机位）。前者是绝对尺、后者是世界不变性，两者正交：绝对尺对一台整体
//     平移的仪器照样读数正常，而「跟相机滑」恰恰是绝对尺看不见的那一维。
//
// ── 四格（2 工作点 × 2 开关态）+ 两条对照臂 ──────────────────────────────────────────
//                 | 门关 rebase=false  | 门开 rebase=true
//   --------------|--------------------|---------------------
//   出厂点（原位） | ① **无地板残差**     | ② 「不坏」
//   远点（+1e6 m） | ③ **预期最差**       | ④ **预期 ≈ ①②**
//   · 每格两枚机位：A = manifest 机位，B = A 沿**世界 +X 平移 0.5 m**（纯平移，姿态逐字相同）。
//   · 阴性对照 = ① 的 A 机位重跑（同机位两次 ⟹ 翻转必须**恰 0**，否则引擎不确定，全部读数作废）。
//   · 区分力守卫 = 把**靶**沿 +X 挪 0.5 m、相机跟着挪 0.5 m（取景逐字相同，而影场在世界里滑了
//     恰 0.5 m）⟹ 平移搜索必须报出 +0.5 m。报 0 = 量具看不见滑移，主判据的绿不作数。
//
// ── 六道闸门（全部抛 rc=3、一条 Verdict 都不产；🔴 表先出、闸门后抛）──────────────────
// G1 同桶 + 靶同源：远点 manifest 的 worldOffset 必须 = 1e6 且逐板真值 = 出厂靶 + 平移（复用
//    `requireFarIsTranslatedCopy`）；🔴 且每格两枚机位的 `edgeCascadeModel` 必须**逐数相同**
//    ——相机挪 0.5 m 会让 dist(相机, bounds 中心) 变 ~0.25 m，本靶的量化桶（near 2 / far 16）
//    要到 dx = 0.960 m 才翻，所以 0.5 m 安全；但这条**不靠手算兜**，翻桶就抛（那时「阴影随
//    相机变」有一半是级联本身变了，与 rebase 无关，不是本判据的射程）。
// G2 激励在场：每格 A/B 的 none 臂不得逐位相同（相机根本没动 = 四格只剩一格）。
// G3 阴影在场 + 场非退化：none↔csm 逐位不同像素数 > 0 且有影面积 > 死态（恒 0 场上「翻转率
//    0」是恒真，同 material-swim 的 G4）。
// G4 可信集规模：光度自吻合 + 搬运有效 + 侵蚀带之外的点数与占比双下限（分母是彩票时任何
//    百分比都没有意义）。
// G5 门开确实动了图：② vs ①、④ vs ③ 的自然臂不得逐位相同（原点没进链路 = 四格只剩两格）。
// G6 引擎回显对账：`shadow: mode=` / `min(D=5,` / `coordinate.rebase = true` 的在场与缺席。
//
// 判决线（J-F1…J-F5）与阈值推导 → `shadow_swim_flat_judge.cpp` + `judgments_table_swim_flat.cpp`。
//
// ── 为什么没有 flag ──────────────────────────────────────────────────────────────────
// 靶 / 机位 / 激励量 / 尺寸 / 帧号 / 侵蚀半径 / 搜索网格 / 阈值都是量具的组成部分：
// 能调 = 能调绿（同 shadow-edge-ruler / shadow-edge-rebase / material-swim）。
// 本命令零 flag ⟹ 进 main.cpp 的 kMeta 查表。
//
// ── 🔴 变异红线（哪个变异让本仪器红；逐条实跑读数见交付说明）────────────────────────
//   · 把 `projectSamples` 的平移搜索**钉成恒 0** → J-F5 红（注入的 0.5 m 报不出来）。这条是
//     任务书钦定的那条，也是本仪器唯一能证明「J-F1 的 0 不是量具瞎」的判据。
//   · 侵蚀带不参与（band 恒空）→ J-F1 从 0.0000 % 变 0.2558 %（重采样地板回到读数里）。
//   · 相机 B 的平移写成 0（激励丢失）→ G2 红（两臂逐位相同）。
//   · 门关/门开的回显对账反接 → G6 红（四格其实只剩两格）。
//   · 远点靶换成出厂靶（worldOffset 忘挪）→ G1 红。
#include "imgqa.h"
#include "shadow_swim_flat.h"
#include "cluster_path_gate.h"
#include "shadow_cascade_seam.h"  // seamCovField（与 grain / seam / ruler / rebase 同式同序）
#include "shadow_edge_rebase.h"   // requireFarIsTranslatedCopy（复用，不抄第二份靶同源判据）

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;

/// lua 表里的浮点字面量：17 位有效数字保证 double 逐位往返（少一位 = 引擎读到的不是我们
/// 算出的那个数，而「相机挪了多少」正是本判据的自变量）。
std::string luaNum(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

std::string f2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

std::string f3(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

/// 相机沿世界 +X 挪 dx 后的机位（target 同步平移 ⟹ 姿态逐字相同，只有位置动）。
/// 🔴 必须连 target 一起挪：只挪 position = 平移 + 转向，那会把「视向变了」这个混淆项
/// 引进来（级联的选级、掠射角、像素足迹全跟着动），本判据就再也不能归因给坐标原点。
Engine::Camera shiftedCamera(const Engine::Camera& base, double dx)
{
    Engine::Camera c = base;
    c.position[0] += dx;
    c.target[0] += dx;
    return c;
}

/// 本格装进 scene.lua 的 entity 文本。远工作点把 manifest 的 worldOffset 当 entity position
/// 给引擎（🔴 不烘进 GLB 顶点，理由住 `shadow_edge_rebase.cpp` 头注）；区分力守卫臂再沿 +X
/// 叠加激励量 d（挪靶 = 影场在世界里真的滑 d，而取景不变）。
std::string entitiesLua(const FlatCell& c, double entityShift)
{
    const auto& off = c.target.worldOffset;
    const double x = off[0] + entityShift;
    const bool moved = std::abs(x) + std::abs(off[1]) + std::abs(off[2]) > 0.0;
    return "{ type = \"model\", file = \"models/" + c.stem + ".glb\""
        + (moved ? ", position = { " + luaNum(x) + ", " + luaNum(off[1]) + ", "
                       + luaNum(off[2]) + " }"
                : "")
        + " }";
}

/// G6：引擎回显对账（照 shadow-edge-rebase 的 requireRebaseEcho 形态）。三查：
/// ① `shadow: mode=…`（覆写没生效 = 静默换臂）；② `min(D=5,`（簇路径的出厂 D 证据）；
/// ③ `coordinate.rebase = true` 门开臂必须在场、门关臂必须缺席。
void requireFlatEcho(const FlatCell& c, bool noneArm, const std::string& logText)
{
    const std::string want = "shadow: mode=" + std::string(noneArm ? "none" : "csm");
    if (logText.find(want) == std::string::npos)
        throw std::runtime_error(c.title + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），cov 场的分母不是同机位同帧的 none 臂"
              "，全部读数作废，见 build/rgl/imgqa_run.log");
    if (!noneArm
        && logText.find("min(D=" + std::to_string(kEdgeFactoryD) + ",") == std::string::npos)
        throw std::runtime_error(c.title + " 的引擎回显里没有 `min(D="
            + std::to_string(kEdgeFactoryD) + ",` ⟹ meshlet.shadow_lod 没生效（拼错键 / 簇路径"
              "没生效任一），级联与纹素的闭式刻度对不上实际渲染");
    const std::string needle = "coordinate.rebase = true";
    const bool present = logText.find(needle) != std::string::npos;
    if (c.rebase && !present)
        throw std::runtime_error(c.title + " 的引擎回显里没有 `" + needle
            + "` ⟹ coordinate.rebase=true 落盘了但**引擎没按它跑**（键被 gate_lint 拒 / 读键"
              "路径没接上）。开着门却静默按门关跑 = 四格只剩两格，全部读数作废");
    if (!c.rebase && present)
        throw std::runtime_error(c.title + "（门关）的引擎回显里**出现了** `" + needle
            + "` ⟹ 门关臂其实开着门（覆写打偏或被后面的门盖掉），①③ 两格不是门关态，"
              "「无地板残差」与「预期最差」两格都读错了对象");
}

/// 采一臂：交付配置 + 换 entities 装靶 + 单键覆写（mode / shadow_lod / coordinate.rebase）
/// + 机位从第 1 帧生效 + 钉帧截图。每臂重建目录（上一臂的残留键会静默活到下一臂）。
cv::Mat captureOne(FlatCell& c, double camShift, double entityShift, bool noneArm,
    const std::string& profileDir, const std::string& what)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线：判据跑在交付配置上
    o.size = { kEdgeWidth, kEdgeHeight };
    o.overrideShadowRoute = true;  // 第 4 例外门（izb 一并强制关，与 ruler / rebase 同口径）
    o.shadowRouteMode = noneArm ? "none" : "csm";
    o.overrideShadowLod = !noneArm;  // 第 6 例外门（出厂 D）
    o.shadowLodValue = kEdgeFactoryD;
    o.overrideDeliveryEntities = true;  // 第 8 例外门：装靶
    o.deliveryEntitiesLua = entitiesLua(c, entityShift);
    o.overrideCoordinateRebase = true;  // 第 9 例外门（本仪器的自变量之一）
    o.coordinateRebaseValue = c.rebase;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（RPC 补写会污染 TAA 历史）
    const Engine::Camera cam = shiftedCamera(c.target.camera, camShift);
    o.cameraPosition = cam.position;
    o.cameraTarget = cam.target;
    o.cameraFov = cam.fovDegrees;
    o.overrideLlmShot = true;  // 第 5 例外门：帧号是量具的一部分
    o.llmShotFile = std::string("screenshots/imgqa_shadow_swim_flat_")
        + (c.rebase ? "on" : "off") + "_" + c.stem + "_" + what + ".png";
    o.llmShotDelayFrames = kEdgeDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    const std::string logText = readEngineLog();
    requireClusterPathLive("shadow-swim-flat", c.stem + "/" + what, logText);  // G2 前半
    requireFlatEcho(c, noneArm, logText);                                      // G6
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error(c.title + " 臂 " + what + " 没出图: " + png);
    return readPngBgr(png);
}

/// 一格的四次采集（A/B × none/csm）。顺序写死：A-none, A-csm, B-none, B-csm。
void captureCell(FlatCell& c, const std::string& profileDir)
{
    c.noneA = captureOne(c, 0.0, 0.0, true, profileDir, "A-none");
    c.csmA = captureOne(c, 0.0, 0.0, false, profileDir, "A-csm");
    c.noneB = captureOne(c, kFlatStimulusM, 0.0, true, profileDir, "B-none");
    c.csmB = captureOne(c, kFlatStimulusM, 0.0, false, profileDir, "B-csm");
    if (c.noneA.cols != kEdgeWidth || c.noneA.rows != kEdgeHeight)
        throw std::runtime_error(c.title + " 采集尺寸是 " + std::to_string(c.noneA.cols) + "x"
            + std::to_string(c.noneA.rows) + "，不是量具钉死的 " + std::to_string(kEdgeWidth)
            + "x" + std::to_string(kEdgeHeight) + "（llm_shot.size 没生效 = 闭式取景全错位）");
    std::printf("  ✓ %-14s 四臂齐（A/B × none/csm）%dx%d，none↔csm 逐位不同 = %lld px\n",
        c.title.c_str(), c.noneA.cols, c.noneA.rows,
        static_cast<long long>(cv::countNonZero(anyChannelDiff(c.noneA, c.csmA))));
}

/// 把 (radius, step) 铺成一轮候选网格（**不含**中心 (0,0)：零位移是基准，不是候选）。
std::vector<std::array<double, 2>> searchRound(double radius, double step)
{
    std::vector<std::array<double, 2>> g;
    const int n = static_cast<int>(std::round(radius / step));
    for (int iz = -n; iz <= n; ++iz)
        for (int ix = -n; ix <= n; ++ix) {
            if (ix == 0 && iz == 0)
                continue;
            g.push_back({ ix * step, iz * step });
        }
    return g;
}

/// 两轮的候选数**在编译期之外也是写死的**（打印出来，跑一次就知道是不是 120+120）。
const std::vector<std::vector<std::array<double, 2>>>& searchRounds()
{
    static const std::vector<std::vector<std::array<double, 2>>> kRounds = {
        searchRound(kFlatSearchCoarseRadiusM, kFlatSearchCoarseM),
        searchRound(kFlatSearchFineRadiusM, kFlatSearchFineM),
    };
    return kRounds;
}

/// 一格的读数装配（cov → 可信集 → 侵蚀带 → 两种口径 → 平移搜索）。
/// 🔴 顺序即口径：可信集与侵蚀带都在**零位移**的搬运上算，两种 erode 共用同一份，否则
/// 「erode 0 vs erode 2」的差里就混进了「可信集也变了」。
void readOneCell(FlatCell& c)
{
    c.covA = seamCovField(c.csmA, c.noneA);  // 🔴 分母 = 同机位同帧的 none 臂
    c.covB = seamCovField(c.csmB, c.noneB);
    c.samples = sampleGround(c.projA, c.covA.cols, c.covA.rows, kFlatMaxDistanceM);

    Frame fa;
    Frame fb;
    fa.bgr = c.noneA;
    fb.bgr = c.noneB;
    prepareLinear(fa);
    prepareLinear(fb);
    std::vector<double> dx;
    std::vector<double> dy;
    projectSamples(c.projB, c.samples, 0.0, 0.0, dx, dy);
    c.keep = photometricTrustMask(fa.luma, fb.luma, c.samples, dx, dy, kFlatPhotometricTol);
    c.bandA = edgeBandMask(c.covA, kFlatErodePx);
    c.bandB = edgeBandMask(c.covB, kFlatErodePx);

    c.raw = compareTransported(c.covA, c.covB, c.samples, dx, dy, c.keep, {}, {}, 0);
    c.floor = compareTransported(c.covA, c.covB, c.samples, dx, dy, c.keep, c.bandA, c.bandB,
        kFlatErodePx);
    c.best = searchWorldShift(
        c.covA, c.covB, c.samples, c.keep, c.bandA, c.bandB, c.projB, searchRounds(), kFlatCoverageFloor);

    const cv::Rect full(0, 0, c.covA.cols, c.covA.rows);
    c.shadowPx = cv::countNonZero(anyChannelDiff(c.noneA, c.csmA));
    c.umbraA = coverageFraction(c.covA, full);
    c.umbraB = coverageFraction(c.covB, full);
}

/// 一格的一行读数（🔴 三种口径必须并排：erode 0 = 债-139 原口径、erode 2 = 无地板、最优平移）。
void printCellRow(const FlatCell& c)
{
    std::printf("  %-14s 可信 %-7zu 剔缘 %-7zu | erode0 %8s%% | erode%d %8s%% | "
                "meanΔcov %8s‰ p99 %8s‰ | 最优 (%+s,%+s) m 失配 %zu\n",
        c.title.c_str(), c.floor.trust, c.floor.edgeExcluded, f3(c.raw.flipPct()).c_str(),
        kFlatErodePx, f3(c.floor.flipPct()).c_str(), f3(c.floor.meanAbsCovPermille()).c_str(),
        f3(1000.0 * c.floor.p99AbsCov).c_str(), f2(c.best.sx).c_str(), f2(c.best.sz).c_str(),
        c.best.flips);
}

}  // namespace

std::vector<FlatCell> makeFlatCells()
{
    const auto make = [](std::string title, std::string stem, bool rebase) {
        FlatCell c;
        c.title = std::move(title);
        c.stem = std::move(stem);
        c.rebase = rebase;
        return c;
    };
    std::vector<FlatCell> cells = { make("① 出厂·门关", kEdgeGlbStem, false),
        make("② 出厂·门开", kEdgeGlbStem, true), make("③ 远点·门关", kFlatFarStem, false),
        make("④ 远点·门开", kFlatFarStem, true) };
    for (auto& c : cells) {
        c.target = loadEdgeTarget(c.stem);  // 三件套缺任何一件 = 抛
        const Engine::Camera camB = shiftedCamera(c.target.camera, kFlatStimulusM);
        c.projA = makeProjector(c.target.camera, kEdgeWidth, kEdgeHeight);
        c.projB = makeProjector(camB, kEdgeWidth, kEdgeHeight);
        c.modelA = edgeCascadeModel(c.target.camera, kEdgeWidth, kEdgeHeight, c.target.boundsCenter,
            c.target.boundsRadius);
        c.modelB = edgeCascadeModel(camB, kEdgeWidth, kEdgeHeight, c.target.boundsCenter,
            c.target.boundsRadius);
    }
    return cells;
}

void fillFlatCellReadout(FlatCell& c) { readOneCell(c); }

/// 两条对照臂的采集与读数。
/// · 阴性对照 = ① 的 A 机位重跑（erode 0 口径，最严：连重采样地板都不许翻，因为它比的是
///   **同一枚机位**，重采样误差按构造为 0）。
/// · 区分力守卫 = ① 的 B 机位 + 靶沿 +X 挪 d。取景与 offB 逐字相同（同一枚相机看同一块地），
///   但遮挡体在世界里挪了 d ⟹ 影场滑 d ⟹ 搜索必须报出 +d。
void fillFlatControls(FlatCell& base, FlatControl& ctl, const std::string& profileDir)
{
    ctl.noneA2 = captureOne(base, 0.0, 0.0, true, profileDir, "A2-none");
    ctl.csmA2 = captureOne(base, 0.0, 0.0, false, profileDir, "A2-csm");
    ctl.bitDiff = cv::countNonZero(anyChannelDiff(base.noneA, ctl.noneA2))
        + cv::countNonZero(anyChannelDiff(base.csmA, ctl.csmA2));
    // 同机位 ⟹ 搬运就是恒等映射（projA → projA，零位移）。这里**不**用侵蚀带：erode 0 是
    // 最严口径，而同一枚机位连重采样地板都该是 0（两次采到的 cov 场若逐位相同，差恒为 0）。
    std::vector<double> ax;
    std::vector<double> ay;
    projectSamples(base.projA, base.samples, 0.0, 0.0, ax, ay);
    ctl.determinism = compareTransported(seamCovField(base.csmA, base.noneA),
        seamCovField(ctl.csmA2, ctl.noneA2), base.samples, ax, ay, base.keep, {}, {}, 0);

    ctl.noneS = captureOne(base, kFlatStimulusM, kFlatStimulusM, true, profileDir, "S-none");
    ctl.csmS = captureOne(base, kFlatStimulusM, kFlatStimulusM, false, profileDir, "S-csm");
    // 🔴🔴 注入臂的对拍**必须以 B 的网格为源**，不能复用 base.samples（那是 A 的网格）。
    // 首版就是复用了它：`compareTransported(covB, covS, base.samples, ...)` 会拿 **A 的像素下标**
    // 去读 covB，而 A 与 B 是两枚相差 0.5 m 的相机 ⟹ 「A 的第 800 列」与「B 的第 800 列」是两个
    // 不同的世界点。后果不是「读数偏一点」而是**判据反向**：实测报 erode0 翻转 0.0000 %（看着像
    // 「量具看不见滑移」）+ 最优平移 (−0.54, +0.22)（看着像「搜索坏了」），两个归因都是假的。
    // 实机对账给出的真相：umbra 质心 A=(800,584)、B=(868,521)、S=(800,574) ⟹ S 的取景回到 A，
    // 而 B 与 S 的影在世界里差 0.5 m —— 注入是生效的，错的是我这根线。
    // 同一枚相机 ⟹ 搬运退化成恒等，可信集判据也随之退化成「同像素比两臂」，正是这里要的：
    // 板动过的像素剔掉，地面像素留下。
    const GroundSamples samplesB = sampleGround(base.projB, base.covB.cols, base.covB.rows,
        kFlatMaxDistanceM);
    std::vector<double> dx;
    std::vector<double> dy;
    projectSamples(base.projB, samplesB, 0.0, 0.0, dx, dy);
    Frame fb;
    Frame fs;
    fb.bgr = base.noneB;
    fs.bgr = ctl.noneS;
    prepareLinear(fb);
    prepareLinear(fs);
    ctl.keep = photometricTrustMask(fb.luma, fs.luma, samplesB, dx, dy, kFlatPhotometricTol);
    const cv::Mat covS = seamCovField(ctl.csmS, ctl.noneS);
    const cv::Mat bandS = edgeBandMask(covS, kFlatErodePx);
    ctl.trustKept = cv::countNonZero(ctl.keep);
    ctl.injected
        = compareTransported(base.covB, covS, samplesB, dx, dy, ctl.keep, base.bandB, bandS, 0);
    ctl.injectedFloor = compareTransported(
        base.covB, covS, samplesB, dx, dy, ctl.keep, base.bandB, bandS, kFlatErodePx);
    ctl.injectedBest = searchWorldShift(base.covB, covS, samplesB, ctl.keep, base.bandB, bandS,
        base.projB, searchRounds(), kFlatCoverageFloor);
}

/// shadow-swim-flat 入口（注册见 cli_table_swim_flat.cpp 与 main.cpp 的 kMeta）。
int cmdShadowSwimFlat()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_swim_flat_profile";
    std::printf("shadow-swim-flat —— **零起伏平地**上影场的世界不变性（债-139 结案批：无地板残差"
                " + 四格时间稳定性 + 与 swim 的口径对照）\n");
    std::printf("  靶 = models/%s.glb 与 models/%s.glb（接收面 y=0 零起伏，8 块竖直板投影）；"
                "采集 %dx%d，帧号 %d，出厂 D=%d\n",
        kEdgeGlbStem, kFlatFarStem, kEdgeWidth, kEdgeHeight, kEdgeDelayFrames, kEdgeFactoryD);
    std::printf("  激励 = 相机 B 沿世界 +X 平移 %s m（纯平移，姿态逐字相同）；无地板口径 = 影缘"
                "侵蚀 %d 像素；搬运在 **double 域**（float32 在 1e6 处 ULP = 0.0625 m）\n",
        f2(kFlatStimulusM).c_str(), kFlatErodePx);
    std::printf("  平移搜索候选 = %zu + %zu（写死，无收敛循环）\n",
        searchRounds()[0].size(), searchRounds()[1].size());

    std::vector<FlatCell> cells = makeFlatCells();
    requireFlatCellsValid(cells);  // G1（靶同源 + 同桶）—— 采集前就能判，先抛省 16 次进程
    std::printf("  G1 过：四格的两枚机位级联闭式**逐数相同**（同桶）；远点靶 = 出厂靶 + %.0f m\n",
        kEdgeFarOffsetM);

    for (auto& c : cells)
        captureCell(c, profile);

    // 🔴 先出表再跑闸门与判决：本批的交付物就是那些数，闸门红时更要拿到它们。
    std::printf("\n  ── 主表：两枚机位（差 %s m）下**同一个世界点**的影状态对拍 ──\n",
        f2(kFlatStimulusM).c_str());
    std::printf("  erode0 = 债-139 原口径（含重采样地板）；erode%d = **无地板**（剔掉影缘带后，"
                "剩下的翻转不可能是重采样）\n\n",
        kFlatErodePx);
    for (auto& c : cells)
        readOneCell(c);
    for (const auto& c : cells)
        printCellRow(c);

    std::printf("\n  ── 在场性（阴影 = none↔csm 逐位不同像素数；恰 0 = 阴影整块消失）──\n");
    for (const auto& c : cells)
        std::printf("  %-14s 阴影像素 = %-8lld 有影面积 A/B = %s / %s\n", c.title.c_str(),
            static_cast<long long>(c.shadowPx), edgePct(c.umbraA).c_str(),
            edgePct(c.umbraB).c_str());

    FlatControl ctl;
    fillFlatControls(cells[0], ctl, profile);
    std::printf("\n  ── 两条对照臂 ──\n");
    std::printf("  阴性对照（① 的 A 机位重跑）：逐位不同像素 = %lld，erode0 翻转 = %zu/%zu\n",
        static_cast<long long>(ctl.bitDiff), ctl.determinism.flips, ctl.determinism.trust);
    std::printf("  区分力守卫（挪靶 + 跟相机 %s m）：可信 %-7zu erode0 %.4f%% erode%d %.4f%%"
                "，最优平移 = (%+s, %+s) m 失配 %zu/%zu\n",
        f2(kFlatStimulusM).c_str(), ctl.trustKept, ctl.injected.flipPct(), kFlatErodePx,
        ctl.injectedFloor.flipPct(), f2(ctl.injectedBest.sx).c_str(),
        f2(ctl.injectedBest.sz).c_str(), ctl.injectedBest.flips, ctl.injectedBest.trust);

    std::vector<Verdict> vs;
    vs.push_back(judgeFlatDeterminism(ctl));       // J-F2（先给：它决定其余四条的读法）
    vs.push_back(judgeFlatFloor(cells[0]));        // J-F1（Q1 的头条数）
    vs.push_back(judgeFlatNoHarm(cells[0], cells[1]));  // J-F3
    vs.push_back(judgeFlatBenefit(cells[0], cells[2], cells[3]));  // J-F4 🔴（Q2 的头条数）
    vs.push_back(judgeFlatOffsetSensitivity(ctl)); // J-F5
    std::printf("\n");
    for (const auto& v : vs)
        std::printf("  %s\n", v.line().c_str());
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    // 🔴 闸门后抛（表与判决线已经出完）
    requireStimulusLive(cells);   // G2
    requireFieldAlive(cells);     // G3
    requireTrustSufficient(cells);  // G4
    requireRebaseMovedImage(cells);  // G5

    logCall("shadow-swim-flat",
        { { "targets", std::string(kEdgeGlbStem) + "," + kFlatFarStem },
            { "stimulus_m", luaNum(kFlatStimulusM) },
            { "erode_px", std::to_string(kFlatErodePx) },
            { "size", std::to_string(kEdgeWidth) + "x" + std::to_string(kEdgeHeight) },
            { "delay_frames", std::to_string(kEdgeDelayFrames) },
            { "c1_flip_erode0_pct", f3(cells[0].raw.flipPct()) },
            { "c1_flip_floor_pct", f3(cells[0].floor.flipPct()) },
            { "c3_mean_dcov_permille", f3(cells[2].floor.meanAbsCovPermille()) },
            { "c4_mean_dcov_permille", f3(cells[3].floor.meanAbsCovPermille()) },
            { "inj_shift_x", f2(ctl.injectedBest.sx) },
            { "det_bitdiff", std::to_string(ctl.bitDiff) } },
        vs);
    std::printf("\n闸门 G1–G6 全过；J-F1…J-F5 见上（挂号与容差理由 → imgqa check）。"
                "🔴 J-F5 红 = 量具看不见滑移，此时 J-F1 的 0 不作数。\n");
    return 0;
}

}  // namespace RGL::imgqa
