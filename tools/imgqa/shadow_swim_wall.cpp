// shadow-swim-wall —— **垂直接收面**上影场的世界不变性 + 「这块面到底由哪一级级联服务」的
// 逐位干预实验。回答的问题（用户猜想）：立面上的影边带随相机动，是不是 CSM 级联接缝本身？
//
// ── 为什么必须有一台垂直接收面的仪器 ────────────────────────────────────────────────
// `shadow-swim-flat` 的接收面是 y=0 的水平地板，而太阳高度角 48.5° ⟹ 竖直面上的受光率与
// 水平面不同（本靶门面 0.6677 vs 地板 0.7444），更关键的是**级联接缝在竖直面上的轨迹**：
// 选级按视深（shadow.slang ①），等视深线在竖直立面上是一条横带，相机前后推拉时这条带会
// 沿立面**扫过**——这正是用户描述的现象。水平地板上同一件事表现为「影边随相机平移」，
// 已经被第 7 刀量过；竖直面上没人量过。
//
// ── 本仪器与全家族共用/不共用的东西 ─────────────────────────────────────────────────
// 共用（不抄第二份）：cov 场 `seamCovField`、搬运与平移搜索 `shadow_transport_metric`、
// 平面重建 `plane_reconstruct`、投影器、引擎日志读取 `cluster_path_gate`、侵蚀带口径。
// 新增（本仪器独有）：**级联索引的逐位读出**（`shadow_wall_metric`）。
//
// ── 本 TU 在四刀切法里的位置（判据 = **要不要引擎进程**）─────────────────────────────
// 本文件里的每次臂装配都要 `makeProfile` + `runEngineOnce` 起一个引擎进程（一台仪器跑完
// 是 50 多次），加上回显对账与出表编排。**不起引擎的那一半在 `shadow_swim_wall_target.cpp`**
// ——靶读取、机位构造、闭式门面掩码与视深场。分开的理由不是行数：那半边没有 GPU 也跑得完，
// 改「测量接口形状」只碰它，改「臂表与配置门」只碰这里。判决线在 `_judge.cpp`，
// 级联索引读出的算子在 `shadow_wall_metric.*`（selftest 直测的那一层）。
//
// ── 核心方法：把四张钉级图当**四把尺**，不是当差分对象 ──────────────────────────────
// `csm.force_cascade = k` 只覆写写进 scene data 的 splits（rgl/render_list_impl.cppm 红字：
// 级联球/切片/阴影图内容一个字都不动）⟹ 钉级 k 图与自然臂在「本来就取第 k 级」的片元上
// **逐位相同**。于是
//
//     自然臂某片元 == 钉级 k 图（逐位）  ⟺  该片元在自然臂里取第 k 级
//
// 是构造性质，地板恰 0，不吃任何闭式级联模型。门面被几级服务、接缝有没有横切它，全部
// 逐位可读。🔴 第一版把「自然臂 − 钉级图」的差分当接缝定位判据，实测死掉：钉级换的是
// **整块表面的采样源**，本靶门面有影像素里 pin2↔pin3 的分歧 ≈ 50 %，差场是一片糊，
// 边界估计退化成「门面视深区间的中点」。configs/shadow.lua 该键注释早就写了「钉级后的画面
// 是错的，只准拿来出差分图」——差分图能量分歧量，不能定位接缝。
//
// ── 因果（干预）在哪一条 ────────────────────────────────────────────────────────────
// J-W3：同一枚机位下，把选级**钉死到实测服务级**（干预 = 关掉选级机制）⟹ 画面必须逐位不变；
// 把选级钉死到**别的级** ⟹ 画面必须大改（阳性对照，量具有牙齿）。两条合起来才是
// 「这块立面上没有接缝，且我看得见接缝」——不是「我算了模型说没有」。
// J-W4：四格（自然/单级 × 门开/门关）的残差给出 S/T/C 归因，其中
// **S = A − B 就是干预量**（关掉选级后残差掉了多少 = 接缝机制对「带随相机动」的贡献）。
//
// ── 闸门（前提塌了必须抛，表先出、闸门后抛）────────────────────────────────────────
// G1 两枚机位由同一组级联服务（实测）· G2 激励在场 · G3 阴影在场且场非退化 ·
// G4 闭式掩码与可信集规模 · G5 四把钉级尺互不相同 · G6 引擎回显对账 · G7 索引可读性。
//
// ── 🔴 变异红线（哪个变异让本仪器红；逐条实跑读数见交付说明）────────────────────────
//   · 把级联索引读出的「唯一匹配」放宽成「第一张匹配的」→ J-W1 红（四把尺全同的像素被算成级 0）。
//   · 把「服务级」写死成 2（不实测）→ 引擎选级语义一变，J-W3 的钉级臂就不再逐位不变 → 红。
//   · 把单调性守卫删掉 → J-W2 红（索引不再对深度单调也照报接缝位置）。
//   · 把 `csm.force_cascade` 的覆写写死成 -1（钉级臂退化成自然臂）→ G5 红 + J-W3 阳性对照红。
//   · 把闭式门面掩码换成「全图」→ G4/G7 红（天穹像素进分母）。
//   · 把激励量写成 0（相机根本没动）→ G2 红。
//   · 门关/门开回显对账反接 → G6 红。
#include "imgqa.h"
#include "cluster_path_gate.h"  // readEngineLog（全家族共用单份真相）
#include "shadow_swim_wall.h"
#include "shadow_seam_metric.h"  // seamCovField / kSeamBin（与全家族同式同序）
#include "shadow_wall_metric.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;

std::string luaNum(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.10g", v);
    return b;
}

std::string f3(double v)
{
    char b[40];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

std::string f1(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", v);
    return b;
}

/// (radius, step) 铺成一轮候选网格（**不含**中心 (0,0)：零位移是基准，不是候选）。
std::vector<std::array<double, 2>> wallRound(double radius, double step)
{
    std::vector<std::array<double, 2>> g;
    const int n = static_cast<int>(std::round(radius / step));
    for (int iv = -n; iv <= n; ++iv)
        for (int iu = -n; iu <= n; ++iu) {
            if (iu == 0 && iv == 0)
                continue;
            g.push_back({ iu * step, iv * step });
        }
    return g;
}

const std::vector<std::vector<std::array<double, 2>>>& wallRounds()
{
    static const std::vector<std::vector<std::array<double, 2>>> kRounds = {
        wallRound(kWallSearchCoarseRadiusM, kWallSearchCoarseM),
        wallRound(kWallSearchFineRadiusM, kWallSearchFineM),
    };
    return kRounds;
}

}  // namespace

// ── 采集 ────────────────────────────────────────────────────────────────────────────
namespace {
void requireWallEcho(const std::string& title, const std::string& logText, bool noneArm,
    int forceCascade, bool rebase)
{
    const std::string want = "shadow: mode=" + std::string(noneArm ? "none" : "csm");
    if (logText.find(want) == std::string::npos)
        throw std::runtime_error(title + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），cov 场的分母不是同机位同帧的 none 臂");
    // 回显 = `shadow: <key>=<值>` 的**差集**（rgl/shadow_config_impl.cppm describeShadowGates：
    // 只列与出厂不同的键）。所以钉级臂必须**出现** `csm.force_cascade=k`，而自然臂（−1 =
    // 出厂值）必须**不出现**这个键——两个方向都判，才排掉「覆写没落盘」「落盘了引擎没吃」
    // 「钉级臂其实是自然臂」三种失效，第三种会让级联索引读出整块作废。
    if (!noneArm) {
        constexpr std::string_view key = "csm.force_cascade=";
        const std::string fc = std::string(key) + std::to_string(forceCascade);
        const bool present = logText.find(key) != std::string::npos;
        if (forceCascade >= 0) {
            if (logText.find(fc) == std::string::npos)
                throw std::runtime_error(title + " 的引擎回显里没有 `" + fc
                    + "`（差集回显，键只在偏离出厂时出现）⟹ 覆写落盘了但引擎没按它跑");
        } else if (present) {
            throw std::runtime_error(title + "（自然臂）的引擎回显里**出现了** `" + std::string(key)
                + "` ⟹ 这一臂其实被钉级了，四把尺里少了一把自然尺");
        }
    }
    const std::string needle = "coordinate.rebase = true";
    const bool on = logText.find(needle) != std::string::npos;
    if (rebase && !on)
        throw std::runtime_error(title + " 的引擎回显里没有 `" + needle
            + "` ⟹ 门开臂其实按门关跑，四格只剩两格");
    if (!rebase && on)
        throw std::runtime_error(title + "（门关）的引擎回显里**出现了** `" + needle
            + "` ⟹ 门关臂其实开着门，①③ 两格不是门关态");
}
}  // namespace

void captureWallArm(WallCamera& c, bool noneArm, int forceCascade, bool rebase,
    const std::string& profileDir, const std::string& what)
{
    const double entityShift = c.entityShift;  // 注入量随臂走，不另开参数
    const std::array<double, 3> entityDir = c.shiftDir;  // = 门面内的 e1（见 makeInjectedCamera）
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线：判据跑在交付配置上
    o.size = { kWallWidth, kWallHeight };
    o.overrideShadowRoute = true;  // 第 4 例外门：mode = none/csm + izb 强制关
    o.shadowRouteMode = noneArm ? "none" : "csm";
    o.overrideShadowForceCascade = !noneArm;  // 第 7 例外门：本仪器的自变量之一
    o.shadowForceCascadeValue = forceCascade;
    o.overrideDeliveryEntities = true;  // 第 8 例外门：装靶
    o.deliveryEntitiesLua = "{ type = \"model\", file = \"models/" + std::string(kWallStem)
        + ".glb\""
        + (entityShift != 0.0
               ? ", position = { " + luaNum(entityShift * entityDir[0]) + ", "
                   + luaNum(entityShift * entityDir[1]) + ", "
                   + luaNum(entityShift * entityDir[2]) + " }"
               : "")
        + " }";
    o.overrideCoordinateRebase = true;  // 第 9 例外门
    o.coordinateRebaseValue = rebase;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（RPC 补写会污染 TAA 历史）
    o.cameraPosition = c.cam.position;
    o.cameraTarget = c.cam.target;
    o.cameraFov = c.cam.fovDegrees;
    o.overrideLlmShot = true;  // 第 5 例外门：帧号是量具的一部分
    o.llmShotFile = std::string("screenshots/imgqa_shadow_swim_wall_") + c.name + "_" + what
        + (rebase ? "_on" : "_off")
        + (forceCascade >= 0 ? "_f" + std::to_string(forceCascade) : "")
        + (entityShift != 0.0 ? "_inj" : "") + ".png";
    o.llmShotDelayFrames = kWallDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    const std::string logText = readEngineLog();
    requireWallEcho(c.name + "/" + what, logText, noneArm, forceCascade, rebase);
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error(c.name + " 臂 " + what + " 没出图: " + png);
    cv::Mat img = readPngBgr(png);
    if (img.cols != kWallWidth || img.rows != kWallHeight)
        throw std::runtime_error(c.name + " 臂 " + what + " 采集尺寸是 "
            + std::to_string(img.cols) + "x" + std::to_string(img.rows) + "，不是量具钉死的 "
            + std::to_string(kWallWidth) + "x" + std::to_string(kWallHeight));
    if (noneArm)
        c.none = std::move(img);
    else if (forceCascade >= 0)
        c.pin[static_cast<size_t>(forceCascade)] = std::move(img);
    else
        c.csm = std::move(img);
}

void fillWallCamera(WallCamera& c, const WallTarget& t, bool rebase,
    const std::string& profileDir)
{
    captureWallArm(c, true, -1, rebase, profileDir, "none");
    captureWallArm(c, false, -1, rebase, profileDir, "csm");
    for (int k = 0; k < static_cast<int>(kWallCascadeCount); ++k)
        captureWallArm(c, false, k, rebase, profileDir, "pin" + std::to_string(k));
    c.arm = c.csm;
    c.cov = seamCovField(c.csm, c.none);
    for (size_t k = 0; k < kWallCascadeCount; ++k)
        c.covPin[k] = seamCovField(c.pin[k], c.none);
    c.mask = wallMask(c, t);
    c.depthAt = wallDepthField(c, t);
    c.maskPx = cv::countNonZero(c.mask);
    c.samples = samplePlane(c.proj, kWallWidth, kWallHeight, kWallMaxDistanceM, t.plane);
    // 🔴 算子读不出来时**抛**（拒绝把「什么都没看见」读成「接缝不在门面上」）。这里接住它、
    //    把原因记进表里，让表先出、闸门与判决线再判——`idx.found` 保持 false ⟹ J-W1/J-W3 必然
    //    红，不会假绿。
    try {
        c.votes = cascadeVotes(c.csm, { c.pin[0], c.pin[1], c.pin[2], c.pin[3] });
        c.idx = cascadeIndexReadout(c.votes, c.mask, c.depthAt);
    } catch (const std::exception& e) {
        c.idxError = e.what();
    }
}

void fillWallCell(WallCell& cell, const WallTarget& t, const std::string& profileDir)
{
    for (WallCamera* c : { &cell.a, &cell.b }) {
        captureWallArm(*c, true, -1, cell.rebase, profileDir, "none");
        if (cell.forceCascade < 0)
            captureWallArm(*c, false, -1, cell.rebase, profileDir, "csm");
        else
            captureWallArm(*c, false, cell.forceCascade, cell.rebase, profileDir,
                "pin" + std::to_string(cell.forceCascade));
        c->arm = cell.forceCascade < 0 ? c->csm
                                       : c->pin[static_cast<size_t>(cell.forceCascade)];
        c->cov = seamCovField(c->arm, c->none);
        c->mask = wallMask(*c, t);
        c->depthAt = wallDepthField(*c, t);
        c->maskPx = cv::countNonZero(c->mask);
        c->samples = samplePlane(c->proj, kWallWidth, kWallHeight, kWallMaxDistanceM, t.plane);
    }
    cell.shadowPx = cv::countNonZero(anyChannelDiff(cell.a.none, cell.a.arm));
    // ── 世界不变性对拍（搬运 + 侵蚀带，与 shadow-swim-flat 同口径）─────────────────
    Frame fa, fb;
    fa.bgr = cell.a.none;
    fb.bgr = cell.b.none;
    prepareLinear(fa);
    prepareLinear(fb);
    std::vector<double> dx;
    std::vector<double> dy;
    projectPlaneSamples(cell.b.proj, cell.a.samples, 0.0, 0.0, dx, dy);
    cell.keep = photometricTrustMask(fa.luma, fb.luma, cell.a.samples, dx, dy,
        kWallPhotometricTol);
    cell.bandA = edgeBandMask(cell.a.cov, kWallErodePx);
    cell.bandB = edgeBandMask(cell.b.cov, kWallErodePx);
    cell.raw = compareTransported(cell.a.cov, cell.b.cov, cell.a.samples, dx, dy, cell.keep,
        cv::Mat(), cv::Mat(), 0);
    cell.floor = compareTransported(cell.a.cov, cell.b.cov, cell.a.samples, dx, dy, cell.keep,
        cell.bandA, cell.bandB, kWallErodePx);
    cell.best = searchWorldShift(cell.a.cov, cell.b.cov, cell.a.samples, cell.keep, cell.bandA,
        cell.bandB, cell.b.proj, wallRounds(), kWallCoverageFloor);
    const cv::Rect full(0, 0, cell.a.cov.cols, cell.a.cov.rows);
    cell.umbra = coverageFraction(cell.a.cov, full);
}

// ── 入口 ────────────────────────────────────────────────────────────────────────────
namespace {

struct CellSpec {
    const char* title;
    bool rebase;
    bool natural;  ///< true = 自然（多级联选级）；false = 钉到实测服务级（= 关掉选级机制）
};

const std::vector<CellSpec>& cellSpecs()
{
    // A/B/C/D 四组（任务书钦定）。顺序在出表与判决线取数里都用它，同 shadow-edge-rebase。
    static const std::vector<CellSpec> kSpecs = {
        { "A 选级·门开", true, true }, { "B 单级·门开", true, false },
        { "C 选级·门关", false, true }, { "D 单级·门关", false, false },
    };
    return kSpecs;
}

/// 服务级（实测）：四档里票数最多的那一级。四档不一致 ⟹ 抛（那说明「钉到服务级」这一干预
/// 没有唯一含义，四格的 B/D 两格无从构造）。
int measuredServingCascade(const std::vector<WallCamera>& cams)
{
    int serving = -1;
    for (const auto& c : cams) {
        if (!c.idx.found)
            throw std::runtime_error("因果档 " + c.name + " 的级联索引没读出来（" + c.idxError
                + "）——「服务级」没有实测值，四格的单级臂无从构造");
        size_t best = 0;
        for (size_t k = 1; k < kWallCascadeCount; ++k)
            if (c.idx.hist[k] > c.idx.hist[best])
                best = k;
        if (serving < 0)
            serving = static_cast<int>(best);
        else if (serving != static_cast<int>(best))
            throw std::runtime_error("四档的实测服务级不一致（" + std::to_string(serving) + " vs "
                + c.name + " 的 " + std::to_string(best)
                + "）——「把选级钉死到服务级」这个干预在本靶没有唯一含义，抛");
    }
    return serving;
}

// ── 出表：三张互不相干的报表 + 判决线出表 ───────────────────────────────────────────
// 缝 = 「这张表的行是什么单位」。级联索引表**一行 = 一枚机位**（且必须与采集交错出，见下），
// 四格表**一行 = 一格配置**，注入表只有一行，判决线出表**一行 = 一条判据**。
// 它们的输入类型不同（`WallCamera` / `WallCell` / `Verdict`），改动原因也不同（加一列读数
// 只碰其中一张）。留在 `cmdShadowSwimWall` 里的只有编排：按什么顺序采、按什么顺序出。
//
// 🔴 出表与采集的**交错**是行为的一部分，不是随手写的顺序：`fillWallCamera` 里算子读不出来
// 时按设计**抛**，而仪器侧接住它、把原因记进 `idxError`，为的就是「表先出、闸门与判决线再判」
// （头注 G 段）。所以索引表必须逐档出在采集循环里——改成「先采满再整表出」会让一次抛在
// 表出全之前发生，坏前提下的现场就没了。下面的函数因此都是**单行**粒度。

/// 级联索引表的一行（一档机位的读出）+ 索引读不出来时那条附注。
void printWallIndexRow(const WallCamera& c, const WallTarget& t)
{
    std::string hist;
    for (size_t k = 0; k < kWallCascadeCount; ++k)
        hist += "c" + std::to_string(k) + ":" + std::to_string(c.idx.hist[k]) + " ";
    std::printf("  %-5s %-8s (%-6s,%-6s) %-9lld %-26s %-9lld %-9lld %lld\n", c.name.c_str(),
        f3(c.distanceN).c_str(), f1(t.nearQ.at(c.name)).c_str(),
        f1(t.farQ.at(c.name)).c_str(),
        static_cast<long long>(c.idx.divergent), hist.c_str(),
        static_cast<long long>(c.idx.ambiguous), static_cast<long long>(c.idx.unmatched),
        static_cast<long long>(c.maskPx));
    if (!c.idxError.empty())
        std::printf("        ↳ 索引没读出来：%s\n", c.idxError.c_str());
}

/// 四格对照表 + 在场性表（两张都以「格」为单位，共用同一个 cells）。
void printWallCellTables(const std::vector<WallCell>& cells)
{
    std::printf("\n  ── 四格对照：同一个门面点在两枚机位下的影状态对拍 ──\n");
    std::printf("  %-12s 可信 %-8s 剔缘 %-7s | erode0 %9s%% | erode%d %9s%% | meanΔcov %9s‰ "
                "p99 %9s‰ | 最优 (%+s,%+s) m\n",
        "格", "点数", "点数", "", kWallErodePx, "", "", "", "", "");
    for (const auto& cell : cells)
        std::printf("  %-12s %-16zu %-16zu | %9s | %9s | %9s | %9s | (%+s,%+s)\n",
            cell.title.c_str(), cell.floor.trust, cell.floor.edgeExcluded,
            f3(cell.raw.flipPct()).c_str(), f3(cell.floor.flipPct()).c_str(),
            f3(cell.floor.meanAbsCovPermille()).c_str(), f3(1000.0 * cell.floor.p99AbsCov).c_str(),
            f3(cell.best.sx).c_str(), f3(cell.best.sz).c_str());
    std::printf("\n  ── 在场性（阴影 = none↔本格臂 逐位不同像素数；恰 0 = 阴影整块消失）──\n");
    for (const auto& cell : cells)
        std::printf("  %-12s 阴影像素 = %-9lld 有影面积 = %s%%\n", cell.title.c_str(),
            static_cast<long long>(cell.shadowPx), f3(100.0 * cell.umbra).c_str());
}

/// 区分力守卫那一行（注入臂：取景逐字相同、只有影在世界上滑了 d）。
void printWallInjection(const WallTarget& t, const WallCell& inj)
{
    std::printf("\n  ── 区分力守卫（注入 = 靶沿 n 挪 %s m + 相机跟着挪，取景逐字相同）──\n",
        f3(t.stimulusM).c_str());
    std::printf("  可信 %-8zu 剔缘 %-7zu | erode%d %9s%% | meanΔcov %9s‰ | 最优 (%+s,%+s) m\n",
        inj.best.trust, inj.floor.edgeExcluded, kWallErodePx, f3(inj.floor.flipPct()).c_str(),
        f3(inj.floor.meanAbsCovPermille()).c_str(), f3(inj.best.sx).c_str(),
        f3(inj.best.sz).c_str());
}

/// 判决线出表：先一遍六条 PASS/FAIL 行，再一遍逐条容差理由（两个独立循环 = 出表的既有版式，
/// 理由块要能整段复制走，不能夹在 PASS 行中间）。
void printWallVerdicts(const std::vector<Verdict>& vs)
{
    std::printf("\n");
    for (const auto& v : vs)
        std::printf("  %s\n", v.line().c_str());
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());
}

}  // namespace

int cmdShadowSwimWall()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_swim_wall_profile";
    std::printf("shadow-swim-wall —— 垂直接收面：级联索引逐位读出 + 影场世界不变性四格\n");
    const WallTarget t = loadWallTarget();
    const std::array<double, 3> nrm = planeNormal(t.plane);
    std::printf("  靶 = models/%s.glb（竖直门面 + 画外竖直杆）；采集 %dx%d，帧号 %d\n",
        kWallStem, kWallWidth, kWallHeight, kWallDelayFrames);
    std::printf("  门面平面：n = (%s, %s, %s)  u∈[%s,%s]  v∈[%s,%s]\n", luaNum(nrm[0]).c_str(),
        luaNum(nrm[1]).c_str(), luaNum(nrm[2]).c_str(), f3(t.uRange[0]).c_str(),
        f3(t.uRange[1]).c_str(), f3(t.vRange[0]).c_str(), f3(t.vRange[1]).c_str());
    std::printf("  激励 = 相机沿门面法线 n 纯平移 %s m（A → B，两档的实测级联集合必须相同）\n",
        f3(t.stimulusM).c_str());

    // ── 因果序列：每档六张图（none / 自然 / 钉级 0..3），逐位读出门面由哪一级服务 ──────
    std::vector<WallCamera> cams;
    for (const auto& nm : t.causalOrder)
        cams.push_back(makeWallCamera(t, nm));
    std::printf("\n  ── 级联索引逐位读出（自然臂 == 哪张钉级图）──\n");
    std::printf("  %-5s %-8s %-16s %-9s %-26s %-9s %-9s %s\n", "档", "沿n距离",
        "模型桶(nQ,fQ)", "分歧像素", "各级票数", "不唯一", "不匹配", "掩码");
    for (auto& c : cams) {
        fillWallCamera(c, t, true, profile);
        printWallIndexRow(c, t);
    }
    requireWallPinsDistinct(cams);  // G5
    requireWallIndexReadable(cams);  // G7
    const int serving = measuredServingCascade(cams);
    std::printf("  G5/G7 过；实测服务级 = c%d（下面四格的单级臂钉到它）\n", serving);

    // ── 四格对照（S / T / C 归因）────────────────────────────────────────────────────
    std::vector<WallCell> cells;
    for (const auto& s : cellSpecs()) {
        WallCell cell;
        cell.title = s.title;
        cell.rebase = s.rebase;
        cell.forceCascade = s.natural ? -1 : serving;
        cell.stimulusM = t.stimulusM;
        cell.a = makeWallCamera(t, "A");
        cell.b = makeWallCamera(t, "B");
        cells.push_back(cell);
    }
    requireWallCellsValid(cells, cams);  // G1（实测级联集合相同）——采集前能判的先抛，省进程
    std::printf("\n  G1 过：四格两枚机位的实测级联集合相同（残差不混「级联本身换了」）\n");
    for (auto& cell : cells)
        fillWallCell(cell, t, profile);

    printWallCellTables(cells);

    // ── 阴性对照：A 机位重跑（同机位两次 ⟹ 逐位相同）─────────────────────────────────
    WallCamera dup = makeWallCamera(t, "A");
    dup.name = "A2";
    captureWallArm(dup, true, -1, true, profile, "none");
    captureWallArm(dup, false, -1, true, profile, "csm");
    const int64_t bitDiff = cv::countNonZero(anyChannelDiff(cells[0].a.none, dup.none))
        + cv::countNonZero(anyChannelDiff(cells[0].a.csm, dup.csm));
    std::vector<double> ddx;
    std::vector<double> ddy;
    projectPlaneSamples(cells[0].a.proj, cells[0].a.samples, 0.0, 0.0, ddx, ddy);
    const ShadowFlipReadout det = compareTransported(cells[0].a.cov,
        seamCovField(dup.csm, dup.none), cells[0].a.samples, ddx, ddy, cells[0].keep, {}, {}, 0);
    std::printf("\n  阴性对照（A 机位重跑）：逐位不同像素 = %lld，erode0 翻转 = %zu/%zu\n",
        static_cast<long long>(bitDiff), det.flips, det.trust);

    // ── 区分力守卫：把靶沿 n 挪 d、相机跟着挪 d（取景相同、只有影在世界上动了）──────
    // 🔴 与 shadow-swim-flat 的注入臂同形：挪的是**靶**（scene.lua 的 entity position），不是
    //    遮挡体，这样「屏幕取景逐字相同」而「影场在世界里滑了 d」。判据要求搜索**原样报出 d**。
    WallCell inj;
    inj.title = "E 注入·挪靶 d 跟相机";
    inj.rebase = false;  // 出厂态：缺陷态上做注入，才叫「看得见滑移」
    inj.forceCascade = -1;
    inj.stimulusM = t.stimulusM;
    inj.a = makeWallCamera(t, "A");
    inj.b = makeInjectedCamera(t, "A", t.stimulusM);
    fillWallCell(inj, t, profile);
    printWallInjection(t, inj);

    std::vector<Verdict> vs;
    vs.push_back(judgeWallIndexUnique(cams));
    vs.push_back(judgeWallIndexMonotone(cams));
    vs.push_back(judgeWallSeamIntervention(cams, serving));
    vs.push_back(judgeWallWorldInvariance(cells));
    vs.push_back(judgeWallDeterminism(cells[0].a.none, cells[0].a.csm, dup.none, dup.csm, det));
    vs.push_back(judgeWallOffsetSensitivity(inj.best, t.stimulusM,
        inj.floor.meanAbsCovPermille()));
    printWallVerdicts(vs);

    requireWallStimulusLive(cells);  // G2
    requireWallFieldAlive(cells);    // G3
    requireWallTrustSufficient(cells);  // G4
    if (std::any_of(vs.begin(), vs.end(), [](const Verdict& v) { return !v.ok; }))
        throw std::runtime_error("shadow-swim-wall：有判决线红（读数见上表与逐条 line）");
    std::printf("\nshadow-swim-wall PASS —— 级联索引读出 + 四格归因 + 注入对照全部达标\n");
    return 0;
}

}  // namespace RGL::imgqa
