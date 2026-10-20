// shadow-edge-ruler —— 影边世界位置的**绝对尺**：真值 = 算术（闭式影边），不是参照渲染。
//
// ── 为什么必须有它（这段是主 agent 立项原文，别删）────────────────────────────────────
// 本会话已建两台阴影判据，**两台都是「拿一次渲染比另一次渲染」**（级联比级联、D=5 比
// D=-1），结构上看不见**共模错误**。已被实测证实的两次翻车：
//   ① 差分仪器 `shadow-cascade-seam` 对「四级全走 LOD」（一个肉眼可见在丢接触阴影的形态）
//     给出 **满分 0.000**——四级一起变差时级间没有分歧。
//   ② 绝对仪器 `shadow-caster-survival` 的**本影面积存活率是反向误导的**：那个坏形态读
//     **1.0022（比参照还"好"）**——粗簇轮廓比原始几何略胖，把丢掉的影子补回来还倒赚。
// **根因是参照本身也是渲染。** 本仪器把真值换成算术：qa_scenes.py 的 `shadow_edge_rig`
// 靶里，高度 H 的竖直挡板其远侧影边落在世界 u = 板远侧面 + |SHADOW_OFF|·H
// （SHADOW_OFF = (SUN.x/−SUN.y, SUN.z/−SUN.y)，SUN = configs/scene.lua 的 light.direction）。
// 读数 = cov 沿世界扫描线的 50% 过零点（线性插值）〔🔴 此句为立项原文，「50% 绝对」已被
// E1 修正为**自身过渡中点（相对阈值）**——真值定义不变，变的只是影边在 cov 上的取点法〕；
// 误差 = 实测 − 真值（世界 mm）。
// 沿板序（= 沿距离 = 跨级联）排开就是「误差随距离/随级联」曲线：**台阶 = 级联交界，
// 台阶高度 = 缝宽（毫米）；每段的偏移量 = 该级的绝对误差**。两个数都不需要参照渲染。
//
// ── 🔴 2026-10-20 两处规格错误修正（都是主 agent 的规格错，不在实现侧；复算已独立确认）──
// E1 「50% 绝对过零点」错 → 改成**自身过渡中点（相对阈值）**。错因 = 把 bistro 墙上的深影
//    （cov>0.5）外推到地面：本靶地面受光里太阳只占 ~30%，影深封顶 cov≈0.33（线性域），
//    绝对 0.5 结构上不可达 ⟹ 首跑 112 个 (臂,板) 组合全「不可量」。正确定义与推导
//    （含过渡高下界 = 10 个 8bit-LSB）住 `shadow_edge_metric.h` 头注。
// E2 「六臂 D 扫描当主读数」错 → 臂砍到 10 条、主读数改成**各级影边的世界位置 + 缝宽**。
//    错因 = 本靶上 D 是逐位空操作：六臂 PNG md5 全等（主 agent 复算），引擎成本表 D=4 已
//    塌到 1 簇/prim（6.2% 三角）而影不动 ⟹ **凸平板的簇 DAG 只删面内部三角、轮廓恒存活**
//    （剪影不变性）。继续摆 D 扫描 = 演戏，且会误导后来人以为这靶在测 LOD。
//    🔴 **LOD 的画质/成本课题不在本靶**：本靶对「LOD 是否毁剪影」结构性免疫；它属于下一台
//    「细附属结构」靶（贴在更大主体上的鳍/栏杆/花箱——bistro 实测四级全 LOD 时近场 186 个
//    小阴影丢 24 个的那种形态；现网最接近的是 `shadow-caster-survival`，但它没有闭式位置
//    刻度）。**别在这个靶上找 LOD 信号。**
//
// ── 臂（10 臂 = 1 + 1 + 8，全部走交付配置 + 换 entities 装靶）─────────────────────────
//   · `none` = 覆盖度分母（mode=none，阴影段零 draw）。
//   · 自然渲染 × 出厂 D=5 × 1（不钉级：`csm.force_cascade=` 回显核对**缺席**）——J1 的
//     「现行选级下每板摆在哪」列。D 语义（configs/runtime.lua / docs/mdi_plan.md §18.7）：
//     出厂 = 5（用户拍板）；−1 = 关只在钉级臂里当参照态。
//   · 钉级 × force_cascade ∈ {0,1,2,3} × shadow_lod ∈ {5, −1}：把全场景钉到级 c 采样，
//     同一块板在级 c 与级 c+1 的读数差 = 该交界的**缝宽（毫米）= 本靶头条交付数**
//     （债-139「交界带混合该开多宽」的设计输入）。D=5 列 = 出厂态（主表）；D=−1 列 =
//     参照态（闸门 4）。两列逐位相同是**已证事实**（E2），留着不是测 LOD，是给闸门 4
//     供「LOD 关 = 全量几何」的闭式参照臂。
//   · 🔴 钉级臂的**出块旁路是结构性质**：级 c 的阴影图只覆盖其视锥切片区，板在片区外 ⟹
//     shadow.slang ④ 返回全亮 ⟹ 该 (级, 板) 格无影可量。可量性由级片区区间**闭式预判**
//     （闸门 3 用它区分「预期出块」与「意外失踪」，见下）。
// 配置覆写只吃第 4/5/6/7/8 枚例外门（mode / llm_shot / shadow_lod / force_cascade /
// entities）。**零引擎改动、零 shader 改动**：被测的全部是配置键 + 靶几何。
// 🔴 走 `deliveryConfigDir`（仓库 configs/）：合成 profile 的绿不算绿 + 级联/LOD 两枚
// 自变量键只存在于交付配置（第 6/7 枚门的既有红线）。
//
// ── 五道闸门（全部**抛** rc=3，不产 Verdict；表先出、闸门后抛——扫描期也要拿到读数）────
// 1 **簇路径必须生效**（`cluster_path_gate`，复用）+ 靶三件套在场（loadEdgeTarget 抛）。
// 2 **分母在场 + 覆盖度非零**（复用 seam 的 `requireDenominatorAlive`/`requireCoveragePresent`，
//   下界按本靶重推 = kEdgeCoverageFloor，理由住 shadow_edge_ruler.h）。
// 3 🔴 **每块板必须真的量到过，除非闭式预判它量不到**（`requireEveryPlateMeasured`）：
//   「某板在某臂找不到过渡中点」且闭式预判该格**可量**（视深在该级切片区内、且离近平面
//   ≥1 枚本级纹素）⟹ 抛并一次报全（意外失踪 = 靶/影坏了，带算子抛因）；片区外（出块旁路
//   shadow.slang ④ 返回全亮）或近平面 1 纹素精度带内（光空间深度精度最差，首跑实测 t003
//   影缘落级 3 近平面内 11mm → 50mm 宽伪影平台）⟹ **预期**，表里标 ⊘ 并打印清单，
//   不进抛集合——预判吃的是闭式级联模型，不是图像。对空集合的断言恒真 = 假绿（本仓 skill
//   明写），静默跳过更不许。
// 4 🔴 **参照态自洽**（`requireReferenceEdgeAtTruth`）：全量几何进阴影图的全部臂
//   （`force=0`×D∈{5,−1}——级 0 恒旧路径画全量几何；`force∈{1,2,3}`×D=−1——LOD 关）
//   误差必须落在**该级自身纹素预算**内（界 = 4·本级纹素 + 2·mm/px；推导：常数深度偏置
//   0.0015 NDC ≈ 1.6 纹素 + PCF 采样不对称 ≤1 纹素 + 纹素吸附 ≤0.5 + 光栅化 ≤1，像素刻度
//   项收采样/插值噪声）。🔴 **不是恰 0**——本级纹素量化本来就有误差，预算内即自洽；
//   超界 = **靶或换算错了**，不是阴影错了，整条曲线作废。
// 5 🔴 **级间分歧阳性对照**（G4，本尺立命之本；**精化版，裁决权在主 agent**——规格原文
//   「每板四臂读数不全等」写于「每板四臂都可量」的假设上，实测推翻该假设：出块结构使每板
//   只有 2–3 枚级可量，且 t100 的 c1/c2 影边位置真重合（过渡括号样本逐位相同，cov 场只差
//   在半影尾巴）⟹ seam=0 是测量值。失明判据改为它本来该测的两条抛：(a) 同 D 不同钉级臂
//   两两图不得逐位相同（臂混装）；(b) 缝宽不得在所有板所有交界上恒 0（算子对级不敏感）。
//   逐板全等降级为**显式警告**（点名 + 归因，不静默）。理由全文住 requireCascadeDivergence。）
//
// ── 判决线（全部 INFO 只报数不判；挂号 → judgments_table_edge.cpp）────────────────────
// J1 `shadow-edge-absolute-error`：每板每级 |误差| 与**本级纹素尺寸**并排打印（主表）——
//   这就是定 K 的推导材料；K 未定不判。
// J2 `shadow-edge-seam-width`：🔴 **头条数** = seam(c↔c+1) = |误差(级c) − 误差(级c+1)|，
//   逐板 + 中位 + 最大（= 债-139 交界带混合带宽的设计输入）；N 未定不判。
// J3 `shadow-edge-cascade-agreement`：各级对同一块板的影边摆放差 ≤ N·max(两纹素)——N 的
//   推导两头写死在注册表（固有下界 = 两级吸附+PCF 不对称做差不消；缺陷态 = 混合方案容忍
//   的缝）。今天不定 N ⟹ INFO。原「误差随 D 单调」退役（E2：本靶上 D 无影响，前提不存在）。
//
// ── 为什么没有 flag ────────────────────────────────────────────────────────────────────
// 机位/尺寸/帧号/扫描窗/阈值都是量具的组成部分：能调 = 能调绿（同 seam / survival / grain）。
// 本命令零 flag ⟹ 进 main.cpp 的 kMeta 查表。
//
// ── 这台仪器还剩下什么**结构性的看不见**（读数版 = 交付说明；本表是设计期清单）──────────
// #1 **过渡中点只对「对称模糊」是影边**：PCF 核不对称化（改抽头布局）或 slope bias 被
//    调开时，斜坡仍单调但中点系统性偏移——本尺子把「模糊的不对称」读成「影边挪了」，
//    两者在单臂读数上同形（分家要靠 shadow-grain 那类附件侧仪器，不在本判据射程）。
//    ⚠ 相对阈值口径下这条**更成立**：阈值跟着平台走，模糊不对称原封不动进中点。
// #2 **量的是接收面上的影边，不是遮挡体轮廓**：板在阴影图里的轮廓怎么塌，只有它投到
//    平地的那条边被量到；悬空遮挡体（牛腿、挂灯）投在**竖直墙面**上的影边不在本靶几何里。
// #3 **闭式真值钉死在 SUN 与平地两个前提上**：改光向 = manifest 全部作废（honesty 前提③）；
//    接收面一斜，误差曲线整条平移（honesty 前提①）——仪器不会知道前提破了，它只信 manifest。
// #4 **末级淡出带内的读数被系统性抹向「受光」**：assert ⑤ 把影边挡在 0.85·shadowFar 之外，
//    代价 = 最远那枚级联的可测区被截短；淡出带内的世界（用户真会走到的远观取景）本尺不量。
// #5 **纹素网格以下的位移不可见**：影边位置被阴影图量化（本级纹素 = 刻度底噪），比纹素细
//    的轮廓损失在**任何**列上都不出声——与 survival 的「量程下限」同族。
// #6 🔴 **对「剪影不变」的塌缩满分沉默**（首跑实测坐实；E2 之后这是**已知边界**不是缺陷）：
//    凸平板的簇化只删面内部三角、轮廓（rim）恒存活——引擎成本表 D=4 已塌到 1 簇/prim
//    （6.2% 三角）而六条 D 臂 PNG **逐位全等**。DAG 有深度 ≠ 影边可动。本尺今天的量程 =
//    **级间摆放分歧**（钉级臂有效，主 agent 已证四臂 PNG 互不相同）；LOD 毁剪影的形态
//    （细附属结构被吞）是下一台靶的自变量，在这台靶上量它 = 恒绿仪器。
// #7 **相对阈值把「影深前提」换成了「平台前提」**：影浅不再致命（0→0.30 也精确，selftest
//    钉），但读数正确性改由两个新前提承重——① 窗口必须比过渡宽（两侧各留 ≥
//    kEdgePlateauMinSamples 个平台样本，粗级纹素 × PCF 把过渡摊宽到吃满窗口 ⟹ 抛
//    「平台窗退化」，f3 × 近界板实测就落在这）；② 平台必须平（窗口吞进第二条边/邻影
//    会抬歪中位数 ⟹ 多过零或阈值漂移）。仪器对这两条**会抛不会装**，但换靶/换机位
//    重摆窗口时它们是新射程边界。过渡高下界（10 LSB）锚在本靶接收面亮度上，换面重推。
#include "imgqa.h"
#include "cluster_path_gate.h"
#include "shadow_cascade_seam.h"   // seamCovField / requireDenominatorAlive / requireCoveragePresent（复用）
#include "shadow_edge_ruler.h"
#include "shadow_kernel_metric.h"  // Projector / makeProjector（复用，别重写）

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;

/// 一臂 = 一次引擎进程 + 一张钉帧截图 + 引擎回显里**必须**出现/缺席的证据。
struct EdgeArm {
    std::string name;         ///< "none" | "d5" | "f<c>d<D>"
    std::string wantMode;     ///< 路线行必须含 `shadow: mode=<wantMode>`
    int lod = -1;             ///< 覆写 runtime.lua meshlet.shadow_lod
    bool overrideLod = false; ///< false = 不写该键（只有 none 臂）
    int force = -2;           ///< -2 = 不覆写 csm.force_cascade（自然臂）；0..3 = 钉级
};

/// 10 臂 = none + 自然×出厂 D + 钉级{0..3}×D{5,−1}。🔴 原六档 D 扫描已删（头注 E2：
/// 本靶上 D 逐位空操作，继续扫 = 演戏）；D=−1 钉级列留着是给闸门 4 供闭式参照态。
const std::vector<EdgeArm>& edgeArms()
{
    static const std::vector<EdgeArm> kArms = [] {
        std::vector<EdgeArm> a;
        a.push_back({ "none", "none", 0, false, -2 });
        a.push_back({ "d" + std::to_string(kEdgeFactoryD), "csm", kEdgeFactoryD, true, -2 });
        for (const int c : kEdgeForceLevels)
            for (const int d : kEdgeForceD)
                a.push_back({ "f" + std::to_string(c) + "d" + std::to_string(d), "csm", d,
                    true, c });
        return a;
    }();
    return kArms;
}

/**
 * 引擎回显对账（照 seam/survival 的 requireArmEcho 形态）。三查：
 *  ① 路线行 `shadow: mode=…` ⟹ shadow.lua 覆写真的落到了引擎；
 *  ② `csm.force_cascade=`：自然臂查**缺席**（出现 = 交付配置/残留把级钉了，「现行选级」
 *     前提塌了）；钉级臂查 `csm.force_cascade=<c>` **在场且带值**（打偏 = 钉错了级，
 *     整列交叉核对的归因换掉）；
 *  ③ shadow_lod：D ≥ 0 查 `min(D=<D>,` 在场；D = -1 查 `min(D=` **缺席**（引擎在 D<0 时
 *     整条装配路径早退、一行日志都不打——查字面量 = 必抛，这条学费 seam 交过）。
 */
void requireArmEcho(const EdgeArm& a, const std::string& logText)
{
    const std::string want = "shadow: mode=" + a.wantMode;
    if (logText.find(want) == std::string::npos)
        throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），全部读数作废，见 build/rgl/imgqa_run.log");
    const std::string forceNeedle = "csm.force_cascade=";
    if (a.force < 0) {
        if (logText.find(forceNeedle) != std::string::npos)
            throw std::runtime_error("臂 " + a.name
                + "（自然渲染）的引擎回显里**出现了** `csm.force_cascade=` ⟹ 级被钉住了"
                  "（交付配置写了非默认值 / 上一臂残留）——本臂测的是「现行选级」，抛");
    } else {
        const std::string e = forceNeedle + std::to_string(a.force);
        if (logText.find(e) == std::string::npos)
            throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + e
                + "` ⟹ csm.force_cascade 覆写打偏（钉错了级或没生效），交叉核对的级归属不可信，"
                  "见 build/rgl/imgqa_run.log");
    }
    if (!a.overrideLod)
        return;  // 覆盖度分母臂：mode=none 下 D 不被消费，没有 shadow_lod 证据可查
    if (a.lod >= 0) {
        const std::string e = "min(D=" + std::to_string(a.lod) + ",";
        if (logText.find(e) == std::string::npos)
            throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + e
                + "` ⟹ meshlet.shadow_lod 没生效为 " + std::to_string(a.lod)
                + "（拼错键 / 簇路径没生效 / 选级表全零任一），见 build/rgl/imgqa_run.log");
    } else if (logText.find("min(D=") != std::string::npos) {
        throw std::runtime_error("臂 " + a.name + "（D=-1）的引擎回显里**出现了** `min(D=`"
            " ⟹ shadow_lod 没被写成 -1（覆写打偏或被后面的门盖掉），「LOD 关」名不副实，见 "
              "build/rgl/imgqa_run.log");
    }
}

/// 采一臂：交付配置 + 换 entities 装靶 + 单键覆写（mode / shadow_lod / force_cascade）+
/// 机位从第 1 帧生效 + llm_shot 钉帧。每臂重建目录（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const EdgeArm& a, const EdgeTarget& target, const std::string& profileDir)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线，理由见头注
    o.size = { kEdgeWidth, kEdgeHeight };
    o.overrideShadowRoute = true;            // 第 4 例外门（izb 一并强制关，与 seam 同口径）
    o.shadowRouteMode = a.wantMode;
    o.overrideShadowLod = a.overrideLod;     // 第 6 例外门
    o.shadowLodValue = a.lod;
    o.overrideShadowForceCascade = a.force >= 0;  // 第 7 例外门
    o.shadowForceCascadeValue = a.force;
    o.overrideDeliveryEntities = true;       // 第 8 例外门：装靶（被测几何 = 实验自变量）
    o.deliveryEntitiesLua
        = "{ type = \"model\", file = \"models/" + std::string(kEdgeGlbStem) + ".glb\" }";
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（RPC 补写会污染 TAA 历史）
    o.cameraPosition = target.camera.position;
    o.cameraTarget = target.camera.target;
    o.cameraFov = target.camera.fovDegrees;
    o.overrideLlmShot = true;  // 第 5 例外门：帧号是量具的一部分
    o.llmShotFile = "screenshots/imgqa_shadow_edge_ruler_" + a.name + ".png";
    o.llmShotDelayFrames = kEdgeDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    const std::string logText = readEngineLog();
    requireClusterPathLive("shadow-edge-ruler", a.name, logText);  // 🔴 闸门 1
    requireArmEcho(a, logText);
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error("臂 " + a.name + " 没出图: " + png);
    return readPngBgr(png);
}

/// 闸门 2（A 分母在场 → B 覆盖度非零，复用 seam 两道；覆盖关系住 shadow_seam_metric.h）。
void requireSignalGates(const std::vector<cv::Mat>& img, const std::vector<cv::Mat>& cov,
    const cv::Rect& full)
{
    std::vector<std::pair<std::string, std::int64_t>> diffByArm;
    std::vector<std::pair<std::string, double>> fractionByArm;
    const auto& arms = edgeArms();
    for (size_t i = 1; i < img.size(); ++i) {
        diffByArm.emplace_back(arms[i].name, cv::countNonZero(anyChannelDiff(img[0], img[i])));
        fractionByArm.emplace_back(arms[i].name, coverageFraction(cov[i], full));
    }
    const auto minDiff = std::min_element(diffByArm.begin(), diffByArm.end(),
                             [](const auto& a, const auto& b) { return a.second < b.second; });
    const auto minFrac
        = std::min_element(fractionByArm.begin(), fractionByArm.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    std::printf("  阴影信号在场：none↔csm 逐位不同像素最少 %lld（臂 %s）；有影面积最小 %s（臂 %s，"
                "下界 %s）\n",
        static_cast<long long>(minDiff->second), minDiff->first.c_str(),
        edgePct(minFrac->second).c_str(), minFrac->first.c_str(),
        edgePct(kEdgeCoverageFloor).c_str());
    requireDenominatorAlive(diffByArm);
    requireCoveragePresent(fractionByArm, kEdgeCoverageFloor,
        // 界的归因跟着本靶重推（默认那份是 bistro 墙影的 48.6% 论证，对浅地影不成立）。
        "  界为什么是闭式可辩护的：死态的有影面积是**恰 0%**（`cov ≡ 0`），不是「小于某个小数」；\n"
        "  本靶确定性渲染、none/csm 只差阴影段 ⟹ 噪声地板恰为 0。首跑实测 umbra（cov>0.5，\n"
        "  线性域）面积逐臂 0.01%（f3）…0.17%（自然臂）——影浅的根因是**地面受光里太阳只占\n"
        "  ~30%**（env 1.0 桥景天光补满，cov 线性域峰值 0.510 / p99 0.367），不是「没画」。\n"
        "  取 0.002% = 最弱臂的 1/5、死态的 ∞ 倍：本闸门只答「阴影画没画进画面」；浅影的\n"
        "  读数由**相对阈值**口径负责（shadow_edge_metric.h E1），「量不到」由闸门 ③ 点名。\n");
}

/// 闭式预判：板（视深 depth）在钉级/自有级 c 里**量得到影边**吗？两个结构条件：
///  ① 在级 c 的视锥切片区 [splits[c₋₁], splits[c]] 内——片区外 ⟹ shadow.slang ④ 出块旁路
///    返回全亮 ⟹ 无影可量；
///  ② 离级 c 的**近平面 ≥ 1 枚本级纹素**——近平面是光空间正交深度范围的起点，深度精度
///    在近平面附近最差；影缘落在近平面 ~1 纹素内时阴影图把影边糊成一条几十毫米宽的伪影
///    平台（首跑实测 t003：影缘 9.089m 在级 3 近平面 9.078m 内 11mm < 24.2mm 纹素 ⟹
///    cov 剖面呈 0.33→0.12→0 的双台阶，最大梯度落在伪影上）。这不是「量到错的边」就是
///    「量不到」，两种都不该进判据 ⟹ 归入**预期**（表里标 ⊘），不是意外失踪。
/// ⚠ 保守方向：片区外仍可能落进级 c 的外接球（球比片区胖，实测 f0 够到 5.9m 而
/// splits[0]=3.75m）——那种格照样量得到、照常出数；本预判**只**用来给「量不到」分家
/// （片区内且离近平面足够远却量不到 = 意外失踪 = 抛；否则 = 预期 = 标 ⊘）。
bool plateMeasurableInCascade(const EdgeCascadeModel& m, double depth, int c)
{
    const double nearClip = c == 0 ? 0.0 : m.splits[c - 1];
    if (depth < nearClip || depth > m.splits[c])
        return false;  // 片区外：出块旁路
    return depth - nearClip >= m.texelM[c];  // 近平面 1 纹素精度带内：伪影，不可量
}

/// 缺失分家（闸门 3 的输入）：unexpected = 抛集合（带原因）；expected = 表里标 ⊘ + 打印清单。
struct MissingSplit {
    std::vector<std::tuple<std::string, std::string, std::string>> unexpected;
    std::vector<std::tuple<std::string, std::string, std::string>> expected;
};

/// 一臂在读数面 (row, col) 的位置（钉级臂 = 级 × D 档；自然臂 = (0,0) 占位，写 errNat）。
struct ArmSlot {
    size_t row, col;
    bool pinned;
};
ArmSlot resolveArmSlot(const EdgeArm& arm)
{
    if (arm.force < 0)
        return { 0, 0, false };
    return {
        static_cast<size_t>(std::find(std::begin(kEdgeForceLevels), std::end(kEdgeForceLevels),
                                  arm.force)
            - std::begin(kEdgeForceLevels)),
        static_cast<size_t>(std::find(std::begin(kEdgeForceD), std::end(kEdgeForceD), arm.lod)
            - std::begin(kEdgeForceD)),
        true,
    };
}

/// 一臂一块板采一次；采不到按闭式预判分家（预期出块 vs 意外失踪）。
void sampleOnePlate(const std::vector<cv::Mat>& cov, const EdgeTarget& t, const Projector& proj,
    const EdgeCascadeModel& model, size_t armIdx, const EdgeArm& arm, const ArmSlot& slot,
    size_t p, EdgeReadout& r, MissingSplit& mi)
{
    const auto& plate = t.plates[p];
    double s = kNaN;
    std::string why;
    try {
        s = edgePosition(cov[armIdx], proj, plate.scan, true);
    } catch (const std::exception& e) {
        why = e.what();
    }
    if (std::isnan(s)) {
        // 分家：钉级臂看钉的那级、自然臂看板自己的预测级；片区外或近平面精度带内 = 预期。
        const int cascade = slot.pinned ? arm.force : r.cascadePred[p];
        if (plateMeasurableInCascade(model, r.depthM[p], cascade))
            mi.unexpected.emplace_back(arm.name, plate.name, std::move(why));  // 闸门 3 报全
        else
            mi.expected.emplace_back(arm.name, plate.name, std::move(why));
        return;
    }
    const double errMm = (s - plate.sTrue) * 1000.0;
    if (slot.pinned)
        r.errPinned[slot.row][slot.col][p] = errMm;
    else
        r.errNat[p] = errMm;
}

/// 全部臂的读数：NaN = 该 (臂, 板) 找不到可量的过渡中点（原因捕获后按闭式预判分家）。
void fillReadouts(const std::vector<cv::Mat>& cov, const EdgeTarget& t, const Projector& proj,
    const EdgeCascadeModel& model, EdgeReadout& r, MissingSplit& mi)
{
    const auto& arms = edgeArms();
    r.errNat.assign(t.plates.size(), kNaN);
    r.errPinned.assign(std::size(kEdgeForceLevels),
        std::vector<std::vector<double>>(std::size(kEdgeForceD),
            std::vector<double>(t.plates.size(), kNaN)));
    for (size_t i = 1; i < arms.size(); ++i) {  // 0 = none（分母，无影边可读）
        const ArmSlot slot = resolveArmSlot(arms[i]);
        for (size_t p = 0; p < t.plates.size(); ++p)
            sampleOnePlate(cov, t, proj, model, i, arms[i], slot, p, r, mi);
    }
}

/// 主表单元：可量 = "s_mm/err"；⊘ = 预期出块；— = 意外失踪（闸门 3 会点名）。
std::string mainCell(double err, const EdgeTarget& t, size_t p, bool expected)
{
    if (std::isnan(err))
        return expected ? "⊘" : "—";
    std::ostringstream o;
    o.precision(1);
    o << std::fixed << t.plates[p].sTrue * 1000.0 + err << "/" << err;
    return o.str();
}

/// D=−1 参照列（闸门 4 的取数面；⊘ = 预期出块）。
void printRefRow(const EdgeReadout& r, const MissingSplit& mi)
{
    const auto isExpected = [&](const std::string& arm, const std::string& plate) {
        for (const auto& [a, p, why] : mi.expected)
            if (a == arm && p == plate)
                return true;
        return false;
    };
    const auto refCell = [&](int c, size_t p) {
        const double e = r.errPinned[c][kEdgeRefDIdx][p];
        const std::string arm = "f" + std::to_string(c) + "d-1";
        if (std::isnan(e))
            return std::string(isExpected(arm, r.plateNames[p]) ? "⊘" : "—");
        return edgeMm(e);
    };
    std::printf("  （D=−1 参照列，误差 mm——闸门 4 的取数；⊘ = 预期不可量）：\n");
    for (size_t p = 0; p < r.plateNames.size(); ++p)
        std::printf("    %-13s c0:%-8s c1:%-8s c2:%-8s c3:%-8s\n", r.plateNames[p].c_str(),
            refCell(0, p).c_str(), refCell(1, p).c_str(), refCell(2, p).c_str(),
            refCell(3, p).c_str());
}

/// 预期出块清单（按臂分组打印；不进闸门 3 抛集合）。
void printExpectedBlock(const MissingSplit& mi)
{
    if (mi.expected.empty())
        return;
    std::printf("  预期不可量（出块 / 近平面 1 纹素精度带，闭式预判；不进闸门 3 抛集合）：");
    std::string lastArm;
    for (const auto& [a, p, why] : mi.expected) {
        if (a != lastArm) {
            std::printf("\n    %s:", a.c_str());
            lastArm = a;
        }
        std::printf(" %s", p.c_str());
    }
    std::printf("\n");
}

/// 主表（🔴 本批主读数 = 每板 × 每级：实测影边世界位置 s / 闭式真值 s / 误差 mm；
/// **先出表后过闸门**——闸门红时读数也要拿到）。s = 沿扫描线的世界米坐标（自窗口上游端，
/// manifest 给的世界点），正误差 = 影边向下游/相机侧挪。⊘ = 预期不可量（出块 / 近平面 1 纹素精度带，
/// 闭式预判，见 plateMeasurableInCascade）；— = 意外失踪（闸门 3 抛集合里，会点名）。
void printMainTable(const EdgeReadout& r, const EdgeTarget& t, const MissingSplit& mi)
{
    const auto isExpected = [&](const std::string& arm, const std::string& plate) {
        for (const auto& [a, p, why] : mi.expected)
            if (a == arm && p == plate)
                return true;
        return false;
    };
    std::printf("\n  ── 主表：影边世界位置 s(mm)/误差 mm（s = 沿扫描线自窗口上游端的世界坐标；"
                "真值 s 每板一枚，列在行首）──\n");
    std::printf("  %-14s 级联 纹素mm 视深m  真值s_mm   自然D5(s/err)      c0(s/err)        "
                "c1(s/err)        c2(s/err)        c3(s/err)\n",
        "板");
    const char* pinnedArm[] = { "f0d5", "f1d5", "f2d5", "f3d5" };
    for (size_t p = 0; p < r.plateNames.size(); ++p) {
        std::printf("  %-14s %-4d %-7.1f %-6.2f %9.1f  %16s  %16s  %16s  %16s  %16s\n",
            r.plateNames[p].c_str(), r.cascadePred[p], r.texelMm[p], r.depthM[p],
            t.plates[p].sTrue * 1000.0,
            mainCell(r.errNat[p], t, p, false).c_str(),
            mainCell(r.errPinned[0][0][p], t, p,
                isExpected(pinnedArm[0], r.plateNames[p])).c_str(),
            mainCell(r.errPinned[1][0][p], t, p,
                isExpected(pinnedArm[1], r.plateNames[p])).c_str(),
            mainCell(r.errPinned[2][0][p], t, p,
                isExpected(pinnedArm[2], r.plateNames[p])).c_str(),
            mainCell(r.errPinned[3][0][p], t, p,
                isExpected(pinnedArm[3], r.plateNames[p])).c_str());
    }
    printRefRow(r, mi);
    printExpectedBlock(mi);
}

/// 参照态自洽的界（闭式推导全文住头注闸门 4；这里给每一项的出处）。
double referenceBoundMm(double texelMm, double mmPerPx)
{
    return 4.0 * texelMm + 2.0 * mmPerPx;
}

/// 闸门 4 通过后的自足证据行（跑没跑、查了几格、最紧一格离界多远，从输出直接读）。
void printReferenceGatePass(const std::vector<std::tuple<std::string, double, double>>& entries)
{
    double worstRatio = 0.0;
    std::string worstName;
    for (const auto& [name, err, bound] : entries)
        if (err / bound > worstRatio) {
            worstRatio = err / bound;
            worstName = name;
        }
    std::printf("  闸门 4 过：%zu 格参照态（force=0×D∈{5,−1} + force∈{1,2,3}×D=−1）全部落在"
                "本级纹素预算内（最紧一格 = %s，|err|/界 = %s%%——**不是恰 0**，本级纹素量化"
                "本来就有误差，预算内即自洽）\n",
        entries.size(), worstName.c_str(), edgeMm(worstRatio * 100.0).c_str());
}

/// 闸门 4 的输入：**全量几何进阴影图**的全部臂 = `force=0`×D∈{5,−1}（级 0 恒旧路径画
/// 全量几何，与 D 无关）+ `force∈{1,2,3}`×D=−1（LOD 关）。🔴 界是**该级自身纹素预算**，
/// 不是恰 0——本级纹素量化本来就有误差（推导逐项住头注闸门 4）；自然 D=5 臂**不进**本
/// 集合（簇 LOD 生效中，「误差≈0」不是闭式预期——虽然 E2 实测它逐位等于 D=−1）。
void requireReferenceGates(const EdgeReadout& r, const EdgeTarget& t, const Projector& proj)
{
    std::vector<std::tuple<std::string, double, double>> entries;
    for (int c = 0; c < 4; ++c) {
        const double texel = r.texelMmByCascade[c];
        for (size_t p = 0; p < r.plateNames.size(); ++p) {
            // 钉级臂的像素刻度：板被钉到级 c 采样，纹素按级 c 取；mm/px 仍按该板真值点算。
            const double mmPerPx = worldPerpMm(proj, t.plates[p].trueEdge,
                { t.plates[p].scan.to[0] - t.plates[p].scan.from[0], 0.0,
                    t.plates[p].scan.to[2] - t.plates[p].scan.from[2] },
                1.0);
            const double bound = referenceBoundMm(texel, mmPerPx);
            const double eRef = r.errPinned[c][kEdgeRefDIdx][p];
            if (!std::isnan(eRef))  // NaN = 预期出块（闸门 3 已分家并打印），不是超界
                entries.emplace_back(
                    "f" + std::to_string(c) + "d-1 × " + r.plateNames[p], std::abs(eRef), bound);
            if (c == 0) {
                const double e0 = r.errPinned[0][0][p];
                if (!std::isnan(e0))
                    entries.emplace_back(
                        "f0d5 × " + r.plateNames[p], std::abs(e0), bound);
            }
        }
    }
    requireReferenceEdgeAtTruth(entries);
    printReferenceGatePass(entries);  // 通过也要出声（G3 证据自足）
}

/// 闸门 5（G4 阳性对照，本尺立命之本）——**精化版**，裁决权在主 agent（证据全文 = 交付说明）：
/// 规格原文「四臂对同一块板的读数必须不全等，全等 = 尺子量的不是级间分歧」写于「每板四臂
/// 都可量」的假设下；实测推翻该假设：① 出块结构使每板只有 2–3 枚级可量（⊘ 是闭式预判的
/// 结构性质）；② t100 的 c1/c2 读数逐位全等，但窗口内 cov 场并不相同（差 0.116，全在
/// 半影尾巴）——**过渡括号样本逐位相同** ⟹ 影边位置真的重合，seam=0 是测量值不是失明。
/// 本尺量的是位置不是半影形状（「拿被模糊量当位置量」是 survival 栽过的反向坑）。
/// 因此失明判据改成它本来该测的两条，逐板全等降级为**显式警告**（点名，不静默）：
///  (a) 图像级：同 D 的不同钉级臂两两不得逐位相同（臂混装 = force 没换采样源）；
///  (b) 读数级：缝宽不得在**所有板所有交界**上恒 0（算子对级不敏感）。
/// 闸门 5(a)：同 D 的不同钉级臂两两图不得逐位相同（臂混装 = force 没换采样源）。
/// 臂表顺序：0=none, 1=d5, 2=f0d5, 3=f0d-1, 4=f1d5, 5=f1d-1, 6=f2d5, 7=f2d-1, 8=f3d5, 9=f3d-1。
/// D=-1 列与同力度 D=5 列逐位相同是**已证事实**（E2），不在比对范围。
void requirePinnedArmsDiffer(const std::vector<cv::Mat>& img)
{
    static const int kPinnedD5[] = { 2, 4, 6, 8 };
    for (int i = 0; i < 4; ++i)
        for (int j = i + 1; j < 4; ++j)
            if (cv::countNonZero(anyChannelDiff(img[kPinnedD5[i]], img[kPinnedD5[j]])) == 0)
                throw std::runtime_error("闸门 5(a): 钉级臂 f" + std::to_string(i) + "d5 与 f"
                    + std::to_string(j)
                    + "d5 的图**逐位相同** ⟹ force_cascade 没换采样源（臂混装/覆写打偏），"
                      "级间分歧无从谈起——停下来报告，不改判据凑");
}

/// 一块板的可量钉级读数是否逐位全等（可量 <2 枚 = 无从判 → false，交给 ⊘ 清单）。
bool plateReadingsAllEqual(const EdgeReadout& r, size_t p, double& valueOut)
{
    std::vector<double> vals;
    for (int c = 0; c < 4; ++c)
        if (!std::isnan(r.errPinned[c][0][p]))
            vals.push_back(r.errPinned[c][0][p]);
    if (vals.size() < 2)
        return false;
    for (const double v : vals)
        if (v != vals[0])
            return false;
    valueOut = vals[0];
    return true;
}

void requireCascadeDivergence(const EdgeReadout& r, const std::vector<cv::Mat>& img)
{
    requirePinnedArmsDiffer(img);
    bool anySeam = false;
    for (size_t p = 0; p < r.plateNames.size(); ++p) {
        double v = 0.0;
        if (!plateReadingsAllEqual(r, p, v))
            anySeam = true;
        else
            std::printf("  ⚠ 闸门 5 警告（不抛，显式点名）: %s（≥2 枚可量级读数逐位全等 = %s"
                        " mm；若窗口内影边位置真重合则 seam=0 是测量值——本尺量位置不量半影"
                        "形状）\n",
                r.plateNames[p].c_str(), edgeMm(v).c_str());
    }
    if (!anySeam)
        throw std::runtime_error("闸门 5(b): **所有**板的跨级读数都逐位全等 ⟹ 这把尺子对"
              "级间分歧失明 = 恒绿仪器——停下来报告，不改判据凑");
}

/// 读数装配（从入口拆出 = 认知复杂度护栏）：逐板算视深/预测级/纹素刻度 → fillReadouts
/// 逐 (臂, 板) 过渡中点 → 主表先出。🔴 先出表再跑闸门 3/4/5：闸门红时也要拿到读数。
EdgeReadout buildReadout(const std::vector<cv::Mat>& cov, const EdgeTarget& target,
    const Projector& proj, const EdgeCascadeModel& model, MissingSplit& mi)
{
    EdgeReadout r;
    r.plateNames.reserve(target.plates.size());
    for (int c = 0; c < 4; ++c)
        r.texelMmByCascade[c] = model.texelM[c] * 1000.0;
    for (const auto& plate : target.plates) {
        r.plateNames.push_back(plate.name);
        const auto edge = plate.trueEdge;
        const double depth = edgeViewDepth(proj, edge);
        r.depthM.push_back(depth);
        const int cas = edgeCascadeOf(model, depth);
        r.cascadePred.push_back(cas);
        r.texelMm.push_back(model.texelM[cas] * 1000.0);
    }
    fillReadouts(cov, target, proj, model, r, mi);
    printMainTable(r, target, mi);
    return r;
}

}  // namespace

int cmdShadowEdgeRuler()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_edge_ruler_profile";
    std::printf("shadow-edge-ruler —— 影边世界位置**绝对尺**：真值 = 闭式算术（竖直板远侧影边"
                " = 板远侧面 + |SHADOW_OFF|·H），不是参照渲染\n");
    std::printf("  靶 = models/%s.glb（tools/qa_scenes.py）；配置 = 交付配置仓库 configs/"
                " + 换 entities；帧号 = 第 %d 帧；采集 %dx%d\n",
        kEdgeGlbStem, kEdgeDelayFrames, kEdgeWidth, kEdgeHeight);
    std::printf("  口径 = 自身过渡中点（相对阈值，E1）；主读数 = 各级影边世界位置 + 缝宽（E2）"
                "；D 扫描已删（本靶上 D 逐位空操作，LOD 画质/成本 = 下一台靶）\n");

    const EdgeTarget target = loadEdgeTarget();  // 缺靶/缺簇产物 = 抛（闸门 1 的构建期前半）
    const Projector proj = makeProjector(target.camera, kEdgeWidth, kEdgeHeight);
    const EdgeCascadeModel model
        = edgeCascadeModel(target.camera, kEdgeWidth, kEdgeHeight, target.boundsCenter,
            target.boundsRadius);
    requireCascadeModelAgreement(target, model);  // 两个独立实现的公式链对账
    std::printf("  级联闭式：near %g→%g far %g→%g（量化到 2 的幂）\n", model.nearRaw,
        model.nearQ, model.farRaw, model.farQ);
    std::printf("  splits = [%.3f, %.3f, %.3f, %.3f] m；texel = [%.2f, %.2f, %.2f, %.2f] mm\n",
        model.splits[0], model.splits[1], model.splits[2], model.splits[3],
        model.texelM[0] * 1000, model.texelM[1] * 1000, model.texelM[2] * 1000,
        model.texelM[3] * 1000);

    // 采集（单实例串行；每臂一次引擎进程）
    const auto& arms = edgeArms();
    std::vector<cv::Mat> img(arms.size());
    for (size_t i = 0; i < arms.size(); ++i) {
        img[i] = captureArm(arms[i], target, profile);
        std::printf("  ✓ 臂 %-6s %dx%d  D=%-2d force=%s\n", arms[i].name.c_str(), img[i].cols,
            img[i].rows, arms[i].lod,
            arms[i].force < 0 ? "—" : std::to_string(arms[i].force).c_str());
    }
    if (img[0].cols != kEdgeWidth || img[0].rows != kEdgeHeight)
        throw std::runtime_error("采集尺寸是 " + std::to_string(img[0].cols) + "x"
            + std::to_string(img[0].rows) + "，不是量具钉死的 " + std::to_string(kEdgeWidth)
            + "x" + std::to_string(kEdgeHeight) + "（llm_shot.size 没生效 = 全部闭式取景错位）");

    const cv::Rect full(0, 0, img[0].cols, img[0].rows);
    std::vector<cv::Mat> cov(img.size());
    for (size_t i = 1; i < img.size(); ++i)
        cov[i] = seamCovField(img[i], img[0]);  // 🔴 分母 = none 臂
    requireSignalGates(img, cov, full);  // 闸门 2

    // 读数 + 出表 + INFO 判决线（🔴 全部先出，闸门后抛：即使闸门红，主表与缝宽毫米数
    // 也必须已经打印——本批的交付物就是那些数，见头注「五道闸门」）。
    MissingSplit mi;
    const EdgeReadout r = buildReadout(cov, target, proj, model, mi);
    printAbsoluteErrorInfo(r);   // J1（INFO）
    printSeamWidthInfo(r);       // J2（INFO，🔴 头条缝宽毫米数）
    printCascadeAgreementInfo(r);  // J3（INFO）

    // 闸门 3：意外失踪抛、预期（出块/近平面精度带）放行（表已标 ⊘）；闸门 4：参照态自洽；
    // 闸门 5：级间分歧阳性对照。
    requireEveryPlateMeasured(mi.unexpected, arms.size() - 1, r.plateNames.size());  // 🔴 闸门 3
    requireReferenceGates(r, target, proj);  // 🔴 闸门 4
    requireCascadeDivergence(r, img);        // 🔴 闸门 5

    const std::vector<Verdict> vs;  // 本命令无裁决判据：裁决面 = 五道闸门（抛 = rc 3）
    logCall("shadow-edge-ruler",
        { { "glb", std::string(kEdgeGlbStem) },
            { "size", std::to_string(kEdgeWidth) + "x" + std::to_string(kEdgeHeight) },
            { "delay_frames", std::to_string(kEdgeDelayFrames) },
            { "arms", std::to_string(arms.size()) },
            { "plates", std::to_string(r.plateNames.size()) },
            { "factory_lod", std::to_string(kEdgeFactoryD) } },
        vs);
    std::printf("\n五道闸门全过；J1/J2/J3 全部 INFO 只报数不判（K/N 未定，推导两头住注册表）"
                "——本批交付物 = 主表 + 缝宽毫米数（债-139 交界带混合带宽的设计输入）\n");
    return 0;
}

}  // namespace RGL::imgqa
