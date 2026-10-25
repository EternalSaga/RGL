// shadow-edge-rebase —— 相对坐标原点 rebase 的**四格对拍**（相对坐标批 5 第 2 刀）。
//
// ── 为什么必须有它（判据变更由主 agent 裁决，原文在 docs/相对坐标地基_plan.md §3 批 5）──
// 批 5 原本给 rebase 定了「阴影图集逐位不变 / 屏幕落在闭式界内」两级图像判据，两条都不用：
//   ① 图集逐位**已经证明做不到**（第 0 刀：顶点路径 `float(p_abs − O)` 随原点重舍入，
//     实测残余 2.7e−4 纹素），而「跨格跳变 ≤ 1e−2 纹素」已在第 1 刀打到真实生产函数上并入
//     了单测（实测 0.00137）——图像级再测一遍是重复。
//   ② `shadow-edge-ruler` 是**更强的仪器**：真值是**算术**（竖直板远侧影边 = 板远侧面 +
//     |SHADOW_OFF|·H），不是「和另一次渲染比」。按「判据失明第四例」——差分判据只能证
//     「不变」，永远证不了「对」——逐位对拍恰好是差分判据，而这条是绝对判据。
//
// ── 四格（2 工作点 × 2 开关态），两条判据缺一不可 ────────────────────────────────────
//                 | 门关 rebase=false      | 门开 rebase=true
//   --------------|------------------------|--------------------------
//   出厂点（靶原位）| ① 基准                 | ② 「不坏」的比较对象
//   远点（+1e6 m） | ③ **预期最差**          | ④ **预期 ≈ ①②**
//   · **不坏** = 出厂点门开的逐板影边误差不得比门关变大（J-R1）。
//   · **变好** 🔴 = 远点门开必须把影边精度买回出厂水平（J-R2）。**这条才是 rebase 的存在
//     理由**——只验「不坏」会放行一个「线接完了但一分精度都没买到」的实现。
//   ⚠ 远点平移量住在 **entity 的 position**（模型根变换），不烘进 GLB 顶点：顶点缓冲是
//     float32，`float32(1e6 + 局部)` 的 ULP = 0.0625 m ⟹ 资产自己就带 ±31 mm 误差，而
//     `toRelative` 减的是变换平移列、碰不到顶点 ⟹ 那 31 mm 任何原点方案都消不掉。
//     实测两种形态在 1e6 下都「阴影整块消失」（下一节），所以这条选择不是绕开失效，
//     只是不让资产量化污染掉本该由 rebase 买单的那部分。
//
// ── 🔴 本批实测的第一条意外：远点门关态不是「厘米级抖动」，是**阴影整块消失** ──────────
// 任务书预期 ③ 读到 ULP(1e6)=0.0625 m 量级的抖动。实测（逐量级扫 csm 臂与 mode=none 臂是否
// 逐位相同；两臂逐位相同 ⟺ 阴影对交付图的贡献**恰为 0 像素**）：
//   |offset| = 128 / 192 / 256 m → 有阴影；**280 / 300 / 1e6 m → 阴影整块消失**。
//   三根轴（+X/+Y/+Z）同阈值。而 ULP(280) = 3.1e−5 m = 0.03 mm，比要解释的现象小六个数量级
//   ⟹ **这不是浮点精度**，是另一类失效（大坐标把某个盒或剔除算飞了）。
//   已排除三条（同一张 1e6 图的实机 A/B，三臂与出厂默认臂逐位相同）：
//   `meshlet.shadow_proxy=false` / `meshlet.shadow_lod=-1` / `meshlet.assets=false`
//   ⟹ 不是 proxy、不是阴影簇 LOD、不是簇路径（与计划 §2.5「proxy 碰过那条链吗 → 零命中」一致）。
//   门开态（rebase=true）在 1e6 下**阴影回来了**（图与门关态不同、影边可量）。
// ⟹ 本仪器把「这一格到底有没有阴影」做成**一条独立判据**（J-R3），而不是混进「rebase 的
//   收益」：消失本身是一条待立案的新缺陷，rebase 恰好把它遮住了。
//   🔴 因此「变好」的判据形态 = **门开在远点把影边精度买回到出厂水平**（可量 + 误差 ≤ 出厂
//   点同口径的界），而不是「④ 的毫米数是 ③ 的 1/N」——门关在远点根本没有毫米数可言。
//
// ── 六臂 × 四格 = 24 次引擎进程（+ 复现性 2 次）─────────────────────────────────────
//   `none`（覆盖度分母）+ 自然渲染 D=5 + 钉级 force_cascade ∈ {0,1,2,3} × D=5。
//   钉级列只取出厂 D：本靶上 D 逐位空操作是**已证事实**（ruler 头注 E2），D=−1 参照列对
//   「rebase 开/关」这个自变量不提供额外信息，砍掉它换四格 × 六臂的预算。
//   配置覆写只吃第 4/5/6/7/8/9 枚例外门。🔴 走 `deliveryConfigDir`（仓库 configs/）：级联 /
//   LOD / 原点三枚自变量键只存在于交付配置（第 6/7/9 枚门的既有红线）。
//
// ── 六道闸门（全部抛 rc=3、一条 Verdict 都不产；表先出、闸门后抛）────────────────────
// G1 **靶同源**（`requireFarIsTranslatedCopy`）：远点 manifest 的 worldOffset 必须 = 常数，
//    且逐板真值 / 机位 / sceneBounds 必须恰等于出厂靶 + worldOffset，级联闭式两靶逐数相同。
//    🔴 这是「闭式真值跟着平移」的**唯一机器判据**：生成器若漏挪真值，出厂点一切照旧绿、
//    远点误差凭空多出 1e6 mm，而其它闸门一条都不会红（变异红线 ① 打的就是它）。
// G2 **簇路径生效**（复用 `cluster_path_gate`）+ 两枚靶三件套在场（loadEdgeTarget 抛）。
// G3 **引擎回显四查**（requireRebaseEcho）：mode / force_cascade / min(D=5, 同 ruler 三查，
//    外加 `coordinate.rebase = true`——门开臂必须在场、门关臂必须缺席。为什么不能只靠配置
//    覆写器的写后读回：那只证明「文件里是 true」，证明不了引擎按它跑；而「键没被读到」与
//    「原点恒零」两种失效在图像上都长得像绿。
// G4 **门开确实动了图**：出厂点 ② 与 ① 的自然臂不得逐位相同（= 批 4 判据 ②「注入非零原点，
//    该消费者的图必须变」在这台仪器上的形态）。逐位相同 ⟹ 原点根本没进链路，四格只剩两格。
// G5 **每块板必须真的量到过**（`requireEveryPlateMeasured`），除非闭式预判它量不到或该格
//    阴影整块缺席（G6 已点名）。
// G6 **阴影在场性出声**（`fillCellReadouts` 打印 + J-R3 裁决）：恰 0 才叫没有。🔴 绝不允许
//    把「没有阴影」静默读成「误差为 0」或「误差量不到」。
//
// 判决线（J-R1/R2/R3）与界的推导 → `shadow_edge_rebase_judge.cpp` + `judgments_table_edge.cpp`。
//
// ── 为什么没有 flag ──────────────────────────────────────────────────────────────────
// 工作点 / 开关态 / 机位 / 尺寸 / 帧号 / 阈值都是量具的组成部分：能调 = 能调绿（同 ruler）。
// 本命令零 flag ⟹ 进 main.cpp 的 kMeta 查表。
#include "imgqa.h"
#include "cluster_path_gate.h"
#include "shadow_cascade_seam.h"  // seamCovField（与 ruler / grain / seam 同式同序）
#include "shadow_edge_rebase.h"
#include "shadow_kernel_metric.h"  // Projector / makeProjector

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;

/// 一臂 = 一次引擎进程 + 一张钉帧截图 + 引擎回显里**必须**出现/缺席的证据。
struct RebaseArm {
    std::string name;
    bool none = false;  ///< 覆盖度分母臂（shadow: mode=none）
    int force = -2;     ///< -2 = 不钉级（自然渲染）；0..3 = csm.force_cascade
};

/// 6 臂 = none + 自然 D=5 + 钉级 {0,1,2,3} × D=5。顺序即读数面下标，改动 = 改判据。
const std::vector<RebaseArm>& rebaseArms()
{
    static const std::vector<RebaseArm> kArms = [] {
        std::vector<RebaseArm> a;
        a.push_back({ "none", true, -2 });
        a.push_back({ "d5", false, -2 });
        for (const int c : kEdgeForceLevels)
            a.push_back({ "f" + std::to_string(c), false, c });
        return a;
    }();
    return kArms;
}

/// lua 表里的浮点字面量：17 位有效数字保证 double 逐位往返（少一位 = 引擎读到的不是我们
/// 算出的那个数，而 G1 对账比的就是那个数）。
std::string luaNum(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

/**
 * 引擎回显对账（照 ruler 的 requireArmEcho 形态，多第 4 查）。四查：
 *  ① 路线行 `shadow: mode=…`；② `csm.force_cascade=`（自然臂查缺席、钉级臂查在场且带值）；
 *  ③ `min(D=5,`（出厂 D 的簇路径证据）；④ 🔴 `coordinate.rebase = true`——门开臂必须在场、
 * 门关臂必须缺席（在场 = 上一臂残留或覆写没落到 false，那 ①③ 两格其实开着门）。
 */
void requireRebaseEcho(const Cell& c, const RebaseArm& a, const std::string& logText)
{
    const std::string want = "shadow: mode=" + std::string(a.none ? "none" : "csm");
    if (logText.find(want) == std::string::npos)
        throw std::runtime_error(c.title + " 臂 " + a.name + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），全部读数作废，见 build/rgl/imgqa_run.log");
    const std::string forceNeedle = "csm.force_cascade=";
    if (a.force < 0) {
        if (logText.find(forceNeedle) != std::string::npos)
            throw std::runtime_error(c.title + " 臂 " + a.name
                + "（自然渲染）的引擎回显里**出现了** `csm.force_cascade=` ⟹ 级被钉住了"
                  "（交付配置写了非默认值 / 上一臂残留）——本臂测的是「现行选级」，抛");
    } else if (logText.find(forceNeedle + std::to_string(a.force)) == std::string::npos) {
        throw std::runtime_error(c.title + " 臂 " + a.name + " 的引擎回显里没有 `"
            + forceNeedle + std::to_string(a.force) + "` ⟹ csm.force_cascade 覆写打偏，"
            "逐级联分布不可信");
    }
    if (!a.none && logText.find("min(D=" + std::to_string(kEdgeFactoryD) + ",")
            == std::string::npos)
        throw std::runtime_error(c.title + " 臂 " + a.name + " 的引擎回显里没有 `min(D="
            + std::to_string(kEdgeFactoryD) + ",` ⟹ meshlet.shadow_lod 没生效（拼错键 /"
              " 簇路径没生效任一），见 build/rgl/imgqa_run.log");
    const std::string rebaseNeedle = "coordinate.rebase = true";
    if (c.rebase && logText.find(rebaseNeedle) == std::string::npos)
        throw std::runtime_error(c.title + " 的引擎回显里没有 `" + rebaseNeedle
            + "` ⟹ coordinate.rebase=true 落盘了但**引擎没按它跑**（键被 gate_lint 拒 / 读键"
              "路径没接上）。开着门却静默按门关跑 = 四格只剩两格，全部读数作废");
    if (!c.rebase && logText.find(rebaseNeedle) != std::string::npos)
        throw std::runtime_error(c.title + "（门关）的引擎回显里**出现了** `" + rebaseNeedle
            + "` ⟹ 门关臂其实开着门（覆写打偏或被后面的门盖掉），①③ 两格不是门关态");
}

/// 本格那一格要装进 scene.lua 的 entity 文本（远工作点把 manifest 的 worldOffset 当
/// entity position 给引擎；出厂点不平移 ⟹ 不写 position 键 = 与改前逐字同）。
std::string entitiesLua(const Cell& c)
{
    const auto& off = c.target.worldOffset;
    const bool moved = std::abs(off[0]) + std::abs(off[1]) + std::abs(off[2]) > 0.0;
    return "{ type = \"model\", file = \"models/" + c.stem + ".glb\""
        + (moved ? ", position = { " + luaNum(off[0]) + ", " + luaNum(off[1]) + ", "
                       + luaNum(off[2]) + " }"
                 : "")
        + " }";
}

/// 采一臂：交付配置 + 换 entities 装靶 + 单键覆写（mode / shadow_lod / force_cascade /
/// coordinate.rebase）+ 机位从第 1 帧生效 + 钉帧截图。每臂重建目录（上一臂的残留键会静默
/// 活到下一臂）。
cv::Mat captureRebaseArm(const Cell& c, const RebaseArm& a, const std::string& profileDir)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线，理由见文件头
    o.size = { kEdgeWidth, kEdgeHeight };
    o.overrideShadowRoute = true;  // 第 4 例外门（izb 一并强制关，与 ruler 同口径）
    o.shadowRouteMode = a.none ? "none" : "csm";
    o.overrideShadowLod = !a.none;  // 第 6 例外门（出厂 D）
    o.shadowLodValue = kEdgeFactoryD;
    o.overrideShadowForceCascade = a.force >= 0;  // 第 7 例外门
    o.shadowForceCascadeValue = a.force;
    o.overrideDeliveryEntities = true;  // 第 8 例外门：装靶
    o.deliveryEntitiesLua = entitiesLua(c);
    o.overrideCoordinateRebase = true;  // 第 9 例外门（本仪器的自变量）
    o.coordinateRebaseValue = c.rebase;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（RPC 补写会污染 TAA 历史）
    o.cameraPosition = c.target.camera.position;
    o.cameraTarget = c.target.camera.target;
    o.cameraFov = c.target.camera.fovDegrees;
    o.overrideLlmShot = true;  // 第 5 例外门：帧号是量具的一部分
    o.llmShotFile = std::string("screenshots/imgqa_shadow_edge_rebase_")
        + (c.rebase ? "on" : "off") + "_" + c.stem + "_" + a.name + ".png";
    o.llmShotDelayFrames = kEdgeDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    const std::string logText = readEngineLog();
    requireClusterPathLive("shadow-edge-rebase", c.stem + "/" + a.name, logText);  // G2
    requireRebaseEcho(c, a, logText);  // G3
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error(c.title + " 臂 " + a.name + " 没出图: " + png);
    return readPngBgr(png);
}

/// 一格的逐板闭式装配：视深 / 预测级联 / 本级纹素（全部从 manifest 的机位与 bounds 推，
/// 与生成器的预测对账 = 两个独立实现，漂了即抛）。
void fillCellGeometry(Cell& c)
{
    c.proj = makeProjector(c.target.camera, kEdgeWidth, kEdgeHeight);
    c.model = edgeCascadeModel(c.target.camera, kEdgeWidth, kEdgeHeight,
        c.target.boundsCenter, c.target.boundsRadius);
    requireCascadeModelAgreement(c.target, c.model);
    for (const auto& plate : c.target.plates) {
        c.plateNames.push_back(plate.name);
        const double depth = edgeViewDepth(c.proj, plate.trueEdge);
        c.depthM.push_back(depth);
        const int cas = edgeCascadeOf(c.model, depth);
        c.cascadePred.push_back(cas);
        c.texelMm.push_back(c.model.texelM[cas] * 1000.0);
    }
}

/// 一臂一块板采一次过渡中点；采不到按两条闭式判据分家（阴影整块缺席 / 预判不可量 = 预期，
/// 否则进 G5 的抛集合）。与 ruler 的 sampleOnePlate 同纪律，多第一条是因为本仪器会出现
/// 「整格无阴影」这种 ruler 上不会出现的形态（远点门关态）。
void sampleOneRebasePlate(const std::vector<cv::Mat>& cov, Cell& c, std::size_t armIdx,
    const RebaseArm& arm, std::size_t p)
{
    const auto& plate = c.target.plates[p];
    double s = kNaN;
    std::string why;
    try {
        s = edgePosition(cov[armIdx], c.proj, plate.scan, true);
    } catch (const std::exception& e) {
        s = kNaN;
        why = e.what();  // 🔴 抛因必须留档：「影没画出来」「影边出窗」「平台窗退化」三种失效
                          // 在误差表上是同一个 —，不分家就会被读成同一种结论
    }
    if (std::isnan(s)) {
        const int cascade = arm.force < 0 ? c.cascadePred[p] : arm.force;
        if (!c.shadowAbsent && plateMeasurableInCascade(c.model, c.depthM[p], cascade))
            c.unexpected.emplace_back(
                c.title + " × " + arm.name + " × " + plate.name + " ⟵ " + why);
        return;
    }
    const double errMm = (s - plate.sTrue) * 1000.0;
    if (arm.force < 0)
        c.errNat[p] = errMm;
    else
        c.errPinned[armIdx - 2][p] = errMm;  // 臂序：0=none, 1=自然, 2..5=f0..f3
}

/// 逐 (臂, 板) 读影边 + 阴影在场性（G6 的打印面）。
void fillCellReadouts(Cell& c)
{
    const auto& arms = rebaseArms();
    const cv::Rect full(0, 0, c.img[0].cols, c.img[0].rows);
    std::vector<cv::Mat> cov(arms.size());
    for (std::size_t i = 1; i < arms.size(); ++i)
        cov[i] = seamCovField(c.img[i], c.img[0]);  // 🔴 分母 = none 臂
    c.shadowPx = cv::countNonZero(anyChannelDiff(c.img[0], c.img[1]));
    c.shadowAbsent = (c.shadowPx == 0);  // 🔴 恰 0，不是「小于某个小数」（变异红线）
    const double umbra = coverageFraction(cov[1], full);
    c.errNat.assign(c.target.plates.size(), kNaN);
    c.errPinned.assign(std::size(kEdgeForceLevels),
        std::vector<double>(c.target.plates.size(), kNaN));
    for (std::size_t i = 1; i < arms.size(); ++i)
        for (std::size_t p = 0; p < c.target.plates.size(); ++p)
            sampleOneRebasePlate(cov, c, i, arms[i], p);
    std::printf("  %-14s 阴影像素 none↔csm = %-8lld 有影面积 = %-9s 自然臂可量板 %zu/%zu%s\n",
        c.title.c_str(), static_cast<long long>(c.shadowPx), edgePct(umbra).c_str(),
        measurable(c.errNat), c.errNat.size(),
        c.shadowAbsent ? "   🔴 G6：阴影整块消失" : "");
}

/// 四格主表：行 = 板，列 = ①②③④ 的自然臂影边误差 mm；下面再给每格的钉级中位分布。
void printMainTable(const std::vector<Cell>& cells)
{
    std::printf("\n  ── 主表：自然渲染臂（现行选级，出厂 D=%d）逐板影边误差 mm —— 真值 = 闭式"
                "算术，与另一次渲染无关（— = 未量到，原因看在场性行）──\n",
        kEdgeFactoryD);
    std::printf("  %-14s 级联 纹素mm       ①出厂·门关       ②出厂·门开       ③远点·门关"
                "       ④远点·门开\n",
        "板");
    const Cell& base = cells[0];
    for (std::size_t p = 0; p < base.target.plates.size(); ++p) {
        std::printf("  %-14s %-4d %-7.1f", base.plateNames[p].c_str(), base.cascadePred[p],
            base.texelMm[p]);
        for (const auto& c : cells)
            std::printf(" %15s", edgeMm(c.errNat[p]).c_str());
        std::printf("\n");
    }
    std::printf("  逐级联分布（钉级臂 force_cascade=c 的 |误差| 中位 mm，括号 = 该级可量板数）"
                "：\n");
    for (const auto& c : cells) {
        std::printf("    %-14s", c.title.c_str());
        for (std::size_t csc = 0; csc < c.errPinned.size(); ++csc)
            std::printf(" c%zu:%-9s(%zu)", csc, edgeMm(medianAbs(c.errPinned[csc])).c_str(),
                measurable(c.errPinned[csc]));
        std::printf("\n");
    }
}

/// 四格定义（顺序 = ①②③④，主表列序与三条判决的取数都按它）+ 靶读取 + 闭式装配。
/// 用工厂函数而不是聚合初始化：Cell 有十个成员，聚合初始化要么写全（复制四遍）要么
/// `-Wmissing-field-initializers` 红（-Werror），而漏写的那几个恰恰是判据取数用的。
std::vector<Cell> makeCells()
{
    const auto make = [](std::string title, std::string stem, bool rebase) {
        Cell c;
        c.title = std::move(title);
        c.stem = std::move(stem);
        c.rebase = rebase;
        return c;
    };
    std::vector<Cell> cells = { make("① 出厂·门关", kEdgeGlbStem, false),
        make("② 出厂·门开", kEdgeGlbStem, true), make("③ 远点·门关", kFarStem, false),
        make("④ 远点·门开", kFarStem, true) };
    for (auto& c : cells) {
        c.target = loadEdgeTarget(c.stem);  // G2 前半：三件套缺任何一件 = 抛
        fillCellGeometry(c);
    }
    return cells;
}

/// 四格采集（单实例串行；每臂一次引擎进程）。
void captureAllCells(std::vector<Cell>& cells, const std::string& profileDir)
{
    const auto& arms = rebaseArms();
    for (auto& c : cells) {
        c.img.assign(arms.size(), cv::Mat());
        for (std::size_t i = 0; i < arms.size(); ++i) {
            c.img[i] = captureRebaseArm(c, arms[i], profileDir);
            std::printf("  ✓ %-14s 臂 %-4s %dx%d force=%s\n", c.title.c_str(),
                arms[i].name.c_str(), c.img[i].cols, c.img[i].rows,
                arms[i].force < 0 ? "—" : std::to_string(arms[i].force).c_str());
        }
        if (c.img[0].cols != kEdgeWidth || c.img[0].rows != kEdgeHeight)
            throw std::runtime_error(c.title + " 采集尺寸是 "
                + std::to_string(c.img[0].cols) + "x" + std::to_string(c.img[0].rows)
                + "，不是量具钉死的 " + std::to_string(kEdgeWidth) + "x"
                + std::to_string(kEdgeHeight) + "（llm_shot.size 没生效 = 闭式取景全错位）");
    }
}

/// B3：门开态的跨进程可复现性（② 与 ④ 各重跑一次自然臂，逐位比）。第 1 刀只验过出厂点，
/// 本批要在两枚靶上都成立；不一致 = 原点取值有帧序问题。
void requireRebaseReproducible(std::vector<Cell>& cells, const std::string& profileDir)
{
    std::printf("\n  ── 门开态跨进程可复现性（第 1 刀已验出厂点，本批要在两枚靶上都成立）──\n");
    for (const int idx : { 1, 3 }) {
        const cv::Mat again = captureRebaseArm(cells[idx], rebaseArms()[1], profileDir);
        const long long d = cv::countNonZero(anyChannelDiff(cells[idx].img[1], again));
        std::printf("  %-14s 重跑自然臂 逐位不同像素 = %lld%s\n", cells[idx].title.c_str(), d,
            d == 0 ? "   ✓" : "   🔴");
        if (d != 0)
            throw std::runtime_error("B3: " + cells[idx].title
                + " 的门开态跨进程不逐位相同 ⟹ 渲染原点的取值有帧序问题（第 1 刀验过的性质"
                  "在这台靶上破了），停下来报告");
    }
}

}  // namespace

/// shadow-edge-rebase 入口（注册见 cli_table_edge.cpp 与 main.cpp 的 kMeta）。
int cmdShadowEdgeRebase()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_edge_rebase_profile";
    std::printf("shadow-edge-rebase —— 相对坐标原点 rebase 的**四格对拍**：2 工作点"
                "（出厂 / 沿 +X 1e6 m）× 2 开关态（coordinate.rebase 关 / 开）\n");
    std::printf("  真值 = 闭式算术（同 shadow-edge-ruler，E1 相对阈值口径）；靶 = models/%s.glb"
                " 与 models/%s.glb；采集 %dx%d，帧号 %d，出厂 D=%d\n",
        kEdgeGlbStem, kFarStem, kEdgeWidth, kEdgeHeight, kEdgeDelayFrames, kEdgeFactoryD);
    std::printf("  🔴 远点平移量住在 entity 的 position（不烘进 GLB 顶点）；两枚靶的 GLB "
                "逐字节相同，差别只在 manifest\n");

    std::vector<Cell> cells = makeCells();
    requireFarIsTranslatedCopy(cells[0].target, cells[2].target, cells[0].model,
        cells[2].model);  // 🔴 G1
    std::printf("  G1 过：远点靶的机位 / 逐板真值 / sceneBounds 恰等于出厂靶 + worldOffset"
                "（%.0f m）\n",
        kEdgeFarOffsetM);
    std::printf("  级联闭式 splits = [%.3f, %.3f, %.3f, %.3f] m；texel = [%.2f, %.2f, %.2f,"
                " %.2f] mm（两靶逐数相同 = 级联只吃相机到 SceneBounds 的相对距离）\n",
        cells[0].model.splits[0], cells[0].model.splits[1], cells[0].model.splits[2],
        cells[0].model.splits[3], cells[0].model.texelM[0] * 1000,
        cells[0].model.texelM[1] * 1000, cells[0].model.texelM[2] * 1000,
        cells[0].model.texelM[3] * 1000);

    captureAllCells(cells, profile);

    // 读数 + 出表（🔴 先出表再跑闸门：闸门红时读数也必须拿到——本批的交付物就是那些数）
    std::printf("\n  ── 在场性（G6：阴影像素 = none↔csm 逐位不同像素数；恰 0 = 阴影整块消失）──\n");
    for (auto& c : cells)
        fillCellReadouts(c);
    printMainTable(cells);

    // C 部分：像素差**只报数**（阈值主 agent 定，本命令不自定「多少算通过」）
    std::printf("\n  ── 像素差（自然臂，只报数不判）──\n");
    printPixelDiff(cells[0], cells[1], "② vs ①（出厂点 门开 vs 门关）");
    printPixelDiff(cells[2], cells[3], "④ vs ③（远点 门开 vs 门关）");
    printPixelDiff(cells[0], cells[2], "③ vs ①（门关态 远点 vs 出厂）");

    requireRebaseReproducible(cells, profile);  // B3

    // G4：门开必须真的动了图（出厂点 ② vs ①）
    const long long d12 = cv::countNonZero(anyChannelDiff(cells[0].img[1], cells[1].img[1]));
    std::printf("\n  G4 出厂点 门开 vs 门关 逐位不同像素 = %lld%s\n", d12,
        d12 == 0 ? "   🔴" : "（≠0 = 原点确实进了链路）");
    if (d12 == 0)
        throw std::runtime_error("G4: 出厂点门开与门关**逐位相同** ⟹ 注入非零原点没有改变任何"
            "东西（批 4 判据 ② 的形态：接了但没接上）。四格只剩两格，停下来报告");

    std::vector<Verdict> vs;
    vs.push_back(judgeShadowPresent(cells));  // J-R3（先给：它决定另两条的读法）
    vs.push_back(judgeNoHarm(cells[0], cells[1]));  // J-R1 不坏
    vs.push_back(judgeBenefit(cells[0], cells[2], cells[3]));  // J-R2 变好 🔴
    // 🔴 判决线**先打印**再过 G5：本批的交付物就是那些数，G5 红（意外失踪）时更要拿到它们
    //    ——「某格该量到却没量到」这件事本身，只有配上另外三格的读数才可归因。
    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    requireEveryPlateMeasured(cells);  // 🔴 G5（最后抛）

    logCall("shadow-edge-rebase",
        { { "targets", std::string(kEdgeGlbStem) + "," + kFarStem },
            { "far_offset_m", luaNum(kEdgeFarOffsetM) },
            { "size", std::to_string(kEdgeWidth) + "x" + std::to_string(kEdgeHeight) },
            { "delay_frames", std::to_string(kEdgeDelayFrames) },
            { "arms", std::to_string(rebaseArms().size()) },
            { "plates", std::to_string(cells[0].target.plates.size()) } },
        vs);
    std::printf("\n闸门 G1–G6 全过；J-R1/R2/R3 见上（挂号与容差理由 → imgqa check）。"
                "🔴 J-R3 红 = 某一格阴影整块消失，那是另一类缺陷，别记进 rebase 的收益。\n");
    return 0;
}

}  // namespace RGL::imgqa
