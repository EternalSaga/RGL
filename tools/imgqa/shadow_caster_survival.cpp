// shadow-caster-survival —— 阴影遮挡体存活率：**绝对量**判据，参照 = `shadow_lod=-1` 全量几何。
//
// ── 为什么必须有它（这段是主 agent 立项原文，别删）────────────────────────────────────
// 上一批建的 `shadow-cascade-seam` 量的是**级与级之间的分歧**。主 agent 刚用一个实验证明它对
// 另一类缺陷**结构失明**：把簇 LOD 从「c1..3 走 LOD、c0 走全量」改成「四级全走 LOD」
// （`render_list_mdi.cppm` 的 `if (c >= 1u && …)` 去掉 `c >= 1u`）之后——
//   |                | 现状 X        | 四级全 LOD Y      |
//   |----------------|---------------|-------------------|
//   | J1 跨级缝增量   | 0.379（红）    | **0.000（满分绿）** |
//   | 级 0 GPU       | 1.408ms       | 0.380ms（−73%）    |
//   | 全帧 GPU       | 5.484ms       | 4.550ms（−17%）    |
//   | 绝对画质        | —             | **肉眼可见地丢影子** |
// Y 的代价（主 agent 实测，阴性对照 = 两配置 `none` 臂逐位相同 0 像素差，故可归因）：最大 8 块
// 丢失区每块 **237–557 像素、覆盖度 0.62–0.78**，形态多为 **20×60 / 17×61 / 28×52** 的细长条
// （= 栏杆/窗棂/挂灯这类细垂直遮挡体的影子）；四个肉眼实例 = 地面本影化成浅灰、**墙上牛腿的
// 接触阴影整条消失**、窗洞阴影被填平（深度线索没了）、**花箱底下的投影没了**；全幅 **0.58%
// 像素明显变亮**；近场本影连通块 84 → 75（小块 72 → 65）。
// **结论：一台只看「一致性」的仪器，对「四级一起变差」给满分。** 本判据就是来补这个洞的。
// ⚠ 顺带记一条：现有 `imgqa shadow-grain` 在这里**也是瞎的**——它跑合成靶，而合成靶没有簇产物
// （`cmake/RglMeshlets.cmake:28-31` 的清单只有 bistro；缺产物时 `rgl/main_scene.cppm:79-87` 只打
// 一条 INFO 就静默回落旧路径）⟹ **根本不进簇路径**。所以本判据**必须跑真资产**。
//
// ── 判据要回答的问题（决定读数形态；口径与边界归属的全文住 shadow_survival_metric.h）──────
// 参照 = `shadow_lod = -1`（四级全画原始几何 = 阴影的「应有样子」）。对每个候选 D 量「影子
// 活下来多少」：① 本影面积存活率；② 小块存活率（参照本影连通块按面积分四档，**主读数**——
// 🔴 必须分档：主 agent 实测丢的正是小块，而总面积几乎不变 48.55%→48.77%，只看总面积的判据
// 会报「没问题」，那正是本判据要防的失效，selftest 有一条合成对专门钉它）；③ 消失 vs 变淡
// 分开报（**归因**读数：「整簇被丢掉」改选级判据、「簇塌成零厚度」改打包工具，存活率同形、
// 这条分家）；④ 近场带（行 500–899）重算 ①②（接触阴影只在近场）。
//
// ── 臂与机位 ──────────────────────────────────────────────────────────────────────────
// `none`（覆盖度分母，每机位一次）+ 对 D ∈ {-1(参照), 0, 1, 2, 3, 5, 7, 11} 各一臂
// （`shadow:mode="csm"`，**不钉级**：`csm.force_cascade` 不覆写 = 出厂 -1，且逐臂回显确认
// 差集行里**没有**它——出现 = 交付配置把级钉了，本仪器「现行选级」的前提塌了）。
// 机位两枚都要跑（主 agent 实测过它们的行为不同，一枚不够）：
//   · `near` = 债-139 第三机位（与 shadow-cascade-seam 同机位，E5 对账锚要求同取景）
//   · `far`  = 出厂默认机位（`just shot` 那个）——实现 = **不写 camera.lua**，把出厂坐标抄进
//     本文件 = 第二份真相，configs 一改就和 `just shot` 分家。
// 尺寸 1600×900。走 `deliveryConfigDir`（仓库 `configs/`）——理由与 seam 同（合成 profile 的
// 绿不算绿 + 只有 bistro 有簇产物）。配置覆写只吃第 4/5/6 三枚例外门（mode / llm_shot /
// shadow_lod），第 7 枚（force_cascade）刻意关死。
//
// ── 四道闸门（全部**抛**，rc=3，不产 Verdict；顺序 = 采集期 1 → 2A → 2B → 3 → 4）─────────
// 1 **簇路径必须生效**（`cluster_path_gate`，与 seam 共用一份实现）：缺簇产物只打 INFO 就
//   静默回落 ⟹ 仪器永久全绿。
// 2 **分母臂在场**（`requireDenominatorAlive`：none 与每条 csm 臂不得逐位相同）+ **覆盖度
//   非零**（`requireCoveragePresent`：cov>0.5 面积 ≥ 1%）——复用 seam 的两道（探测器 B +
//   归因器 A 的分工住 `shadow_seam_metric.h`，本仪器不重述）。
// 3 🔴 **参照臂自洽 = 恰 1.000**（`requireReferenceSelfConsistent`，本判据的阳性锚点）：
//   `D=-1` 与 `D=-1` 比，四项读数必须全是 1.000（闭式恒等，无容差）。它不是 1 ⟹ 参照臂接线
//   错了（第一嫌疑 = 参照臂其实没走全量、或两臂取了同一条），整张表作废。
// 4 🔴 **参照臂必须真的与见证臂不同**（`requireLodChangesImage`，否则「全臂相同」会让四项
//   读数全部 = 1.000 = 满分绿）：见证臂 = `d11`（堵「LOD 整体没生效」，主 agent 实测相对参照
//   翻转率 0.994% 非零）+ `d5`（堵「参照臂与出厂同档」= E6② 形态，实测面积 = 参照的 100.5%）。
//   ⚠ 小 D **不能进见证清单**——本批实机首跑抓到 `d1` 与参照 cov 场**逐位相同**（LOD 级 1 在
//   阴影图分辨率下与全量不可区分，d2 起才有差异）：那是合法态（且是一条免费的画质结论），
//   逐臂判会把正常构建判死。见证选法全文住 requireReferenceGates 注释。
//
// ── 判决线（挂号 → judgments_table_survival.cpp）───────────────────────────────────────
// J1 `shadow-caster-survival-factory`：出厂档 D=5 的小块（<50px）存活率与近场接触阴影存活率。
//   🔴 **阈值本批不定死**（TODO(债-139)）：只报数不判（INFO，不出 PASS/FAIL、不进 rc）——
//   这台仪器今天才建，拿一个没推导过的数当门 = 把「实测没超所以钉个数」写进判据网。**本批的
//   交付物是那张扫描表 + 四条闸门，不是一条绿的判据。**
// J2 `shadow-caster-survival-monotone`：本影面积存活率随 D 单调不增（逐机位判）。抓「区间
//   错位/选级表漂了」这类形态（它会破坏单调）。D 点 < 3 个 ⟹ 抛。
//
// ── 输出 = 决策材料 ────────────────────────────────────────────────────────────────────
// 每机位一张扫描表（行 = D，列 = 本影面积存活率 / 四档小块存活率 / 近场接触存活 / 近场面积 /
// 消失 vs 变淡 / 翻转率）。这张表是「D 取几」的决策材料，也是后续 shadow-proxy 与轮廓保护
// 切法两条路的公共尺；要能直接贴进 `docs/还债实测数据.md`。同二进制连跑两次逐位一致
// （llm_shot 钉帧 + 无墙钟输入，同 seam 的确定性口径）。
//
// ── 为什么没有 flag ────────────────────────────────────────────────────────────────────
// 机位、尺寸、帧号、带子、分档边界都是量具的组成部分：能调 = 能调绿（同 shadow-softness /
// shadow-kernel-scale / shadow-grain / shadow-cascade-seam）。本命令零 flag ⟹ 进 main.cpp 的 kMeta 查表。
//
// ── 跑起来该看到什么（2026-10-20 首跑实机，本机 RADV STRIX_HALO，三次运行表逐行一致）──────
// 18 臂各一次引擎进程（串行，单实例纪律）⟹ 两张扫描表 ⟹ J1 两条 INFO ⟹ J2 一条 Verdict。
// 首跑四个必须复现的事实：
//   ① 参照臂自洽行：`全幅面积 1.000 / 近场面积 1.000 / 消失 0 / 变淡 0`（两机位各一行，E3）。
//   ② **d1 与参照逐位相同**（翻转率 0.000%、消失 0）——LOD 级 1 在阴影图分辨率下与全量不可
//      区分，合法态（见证臂因此不取小 D，见 requireReferenceGates 注释）。
//   ③ J2 现状**红**：far 曲线 2:1.000 → 3:1.011（轮廓胖压过丢失）。这条红 = J2 的物理前提
//      「影子只可能少」在远观机位被实测推翻，**主 agent 重裁射程前它保持红**（同 seam 的 J1
//      红是交付内容的先例）；near 曲线单调不增 ✓。
//   ④ 读数对账锚（E5 的正确版本，主 agent 改锚前拿这四个数认仪器没坏）：near D5 面积存活率
//      **0.9977**、far D5 **1.0020**（<50px 档 far D5 = 0.990 (1349/1362)）、far D5 消失
//      **326px**。⚠ 任务原锚「100.5% / 9px」经逐位图对账确认属于 **X→Y**（D5→四级全 LOD）
//      口径而非参照→D5（主 agent 的 X3/lod-1 图与本仪器 PNG 逐位相同，其 quality_y.py 在
//      先均值→clip 口径下给 X=48.55%；本仪器 cov 是逐通道 clip→均值，两口径面积存活率同为
//      0.9977）——**别拿那两位数改算子**。
//   ⑤ 地板：far_d11 臂 PNG 有 1 像素 run-to-run 漂移（同构建连跑，`just regress floor` 同族
//      现象）；扫描表全部读数不受它影响（三次运行表逐行一致）。跨批比 PNG 逐位时先认这条地板。
//
//
// ── 这台仪器还剩下什么**结构性的看不见**（写下来比藏起来有用，同 seam 的 TODO 段）────────
// #1 **参照臂自己也可以是错的**：全部读数 = test 与参照之比，参照（原始几何进阴影图）被当成
//    「应有样子」是**定义**不是测量。若原始几何本身在阴影图里就漏画某遮挡体（例如极细几何被
//    光栅化丢像素），test 与参照一起丢 ⟹ 存活率 1.000 = 绿，而画面是错的。闸门 3 只钉
//    「参照与自己一致」，钉不了「参照与世界一致」。
// #2 **cov 阈值把「浅影」洗成「无影」**：消失判据是 cov < 0.2，而 cov 的分母是 none 臂的亮度
//    ——亮面上 20% 的遮蔽与暗面上 90% 的遮蔽都可能落到同一条线以下。浅影整片变淡到 0.2 以下
//    会计入「消失」，与「整簇被丢掉」在这条读数上同形（变淡/消失的分家只到 0.2 为止）。
// #3 **分档边界是量程选择不是物理常数**：<50px 的档对「恰好 51px 的细条」不敏感；换场景
//    （细遮挡体更大或更小）分档要重定，而本判据的「主读数」名字里写着「小块」——名字会随
//    场景漂，边界常数不会自己跟着漂。
// #4 **带内连通域跨带不可加**：近场带把一块跨带界的大影切成「带内一截」重新分档，全幅与近场
//    两张表的块数不是子集关系（声明处已写，读表的人容易当成同一批块）。
// #5 **小 D 与参照不可区分是量程下限不是绿**：本批实机首跑抓到 D=1 的 cov 场与参照**逐位相同**
//    ——阴影图分辨率把 LOD 级 1 的轮廓差整个量化掉了。这台仪器的读数分辨率止于「阴影图纹素」，
//    比它细的轮廓损失（D=0/1 若有）在**任何**列上都不出声；闸门 D 因此只能拿 d11/d5 当见证，
//    拿小 D 当见证会把正常构建判死（要测那一档得换仪器：直接对阴影图深度做差分，不在本判据射程）。
//
// ── 变异红线（哪个变异让我红；实跑记录见交付说明）──────────────────────────────────────
//   · E6① 移走簇产物（`bistro_exterior.glb.meshlets.bin` 改名）⟹ 闸门 1 抛 rc=3，点名
//     「簇路径没生效」，**不出表不出判据**。
//   · E6② 把参照臂的 shadow_lod 改成 5（`kSurvRefD = -1` → `5` = 参照与 test 同档）⟹ 闸门 4
//     抛（d5 与 ref 逐位相同）——**抛而不是出一整表 1.000 的满分绿**。
//   · E6③ 把覆盖度分母换成分子自身（`seamCovField(img[i], img[i])`）⟹ 闸门 2 抛
//     （requireDenominatorAlive 点名全部臂与分母逐位相同）。
//   · 算子级变异（本影/存活/消失/变淡阈值、分档边界、8 邻接、空档 NaN、闸门 C/D 比较反向）
//     → `selftest_survival.cpp` 逐条钉死，清单住 `shadow_survival_metric.h` 头注。
//   · J2 单调判据 `>` 反向 → 实机判决翻转；「D 点 < 3 抛」删掉 → selftest 红。
//   · 回显对账删掉 `csm.force_cascade=` 缺席检查 ⟹ 交付配置被钉级时本仪器静默换臂（量的不再
//     是「现行选级」）；实机自证 = 临时给 shadow.lua 写 force_cascade=2 → 抛。
#include "imgqa.h"
#include "cluster_path_gate.h"
#include "shadow_caster_survival.h"
#include "shadow_seam_metric.h"  // seamCovField / flipFraction / coverageFraction / 闸门 A/B（复用，别重写）

#include <opencv2/core.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;

/// 一臂 = 一次引擎进程 + 一张钉帧截图 + 引擎回显里**必须**出现的证据。
/// `lod` 的负值语义见 `configs/runtime.lua`；`overrideLod` false = 不写 shadow_lod 键
/// （只有覆盖度分母臂 `none` 如此：mode=none 时阴影段零 draw，D 这个自变量不被消费）。
struct SurvArm {
    std::string name;       ///< "none" | "ref" | "d<D>"（唯一；PNG 名与归因都吃它）
    std::string wantMode;   ///< 路线行必须含 `shadow: mode=<wantMode>`
    int lod = kSurvRefD;    ///< 覆写 runtime.lua meshlet.shadow_lod
    bool overrideLod = true;
};

/// 机位表（顺序 = 采集序）。`useFactoryCamera` true = **不写 camera.lua**（出厂默认机位 =
/// `just shot` 那枚；不抄坐标 = 不建第二份真相）。
struct SurvCamera {
    const char* name;
    bool useFactoryCamera;
};

const std::vector<SurvCamera>& survCameras()
{
    static const std::vector<SurvCamera> kCams = {
        { "near", false },
        { "far", true },
    };
    return kCams;
}

/// 臂表（顺序 = 采集序 = 下面 covByArm 的下标）：none + ref(D=-1) + 扫描 D 升序。
const std::vector<SurvArm>& survArms()
{
    static const std::vector<SurvArm> kArms = [] {
        std::vector<SurvArm> a;
        a.push_back({ "none", "none", 0, false });
        a.push_back({ "ref", "csm", kSurvRefD, true });
        for (const int d : kSurvSweepD)
            a.push_back({ "d" + std::to_string(d), "csm", d, true });
        return a;
    }();
    return kArms;
}

/// 臂名 → 下标（臂表里名字唯一，重复即抛——两臂同名 = 后一臂的 PNG 盖掉前一臂，静默换臂）。
size_t armIndex(const std::string& name)
{
    const auto& arms = survArms();
    for (size_t i = 0; i < arms.size(); ++i)
        if (arms[i].name == name)
            return i;
    throw std::runtime_error("内部错误：臂表里没有臂 `" + name + "`（判据取数取到了不存在的臂）");
}

/**
 * 引擎回显对账（照 seam 的 requireArmEcho 形态，缺行/多行即抛）。三查：
 *  ① 路线行 `shadow: mode=…` ⟹ 证明 shadow.lua 这份覆写**真的落到了引擎**；
 *  ② 差集行里 `csm.force_cascade=` **必须缺席**——本仪器测的是「现行选级」，出厂 -1 = 默认 =
 *     差集行不打；它出现 = 交付配置（或残留的覆写）把级钉了，各臂不再是「现行选级下的 D 档」，
 *     整张表的语义换掉了（seam 查的是**在场**，这里查的是**缺席**——同一枚键，两台仪器对它的
 *     期望相反，所以本检查不能复用 seam 的那份，各写各的）；
 *  ③ shadow_lod 生效证据：D ≥ 0 查 `min(D=<D>,`（带值）；参照臂 D = -1 反过来查 `min(D=`
 *     **缺席** + 簇产物在场（缺席本身是弱证据，配上「本臂就该是参照臂」才是强证据——
 *     ⚠ 参照臂**不能**查 `shadow_lod=-1` 字面量：引擎在 D<0 时整条装配路径早退、一行日志
 *     都不打，查它 = 必抛。这条学费 seam 交过，抄在这里省得再交）。
 */
void requireArmEcho(const SurvArm& a, const std::string& logText)
{
    const std::string want = "shadow: mode=" + a.wantMode;
    if (logText.find(want) == std::string::npos)
        throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），全部读数作废，见 build/rgl/imgqa_run.log");
    if (logText.find("csm.force_cascade=") != std::string::npos)
        throw std::runtime_error("臂 " + a.name
            + " 的引擎回显里**出现了** `csm.force_cascade=` 差集行 ⟹ 级被钉住了（交付配置写了"
              "非默认值 / 上一台仪器的覆写残留）——本仪器测的是「现行选级」，钉级 = 静默换臂，"
              "全部读数作废，见 build/rgl/imgqa_run.log");
    if (!a.overrideLod)
        return;  // 覆盖度分母臂：mode=none 下 D 不被消费，没有 shadow_lod 证据可查
    if (a.lod >= 0) {
        const std::string e = "min(D=" + std::to_string(a.lod) + ",";
        if (logText.find(e) == std::string::npos)
            throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + e
                + "` ⟹ meshlet.shadow_lod 没生效为 " + std::to_string(a.lod)
                + "（拼错键 / 簇路径没生效 / 选级表全零任一），候选臂不是候选臂，见 "
                  "build/rgl/imgqa_run.log");
    } else if (logText.find("min(D=") != std::string::npos) {
        throw std::runtime_error("臂 " + a.name + "（参照臂 D=-1）的引擎回显里**出现了** "
            "`min(D=` ⟹ shadow_lod 没被写成 -1（覆写打偏或被后面的门盖掉），参照臂其实带着 "
            "LOD，「阴影的应有样子」名不副实，整张表的分母错了，见 build/rgl/imgqa_run.log");
    }
}

/// 采一臂：交付配置 + shadow_lod 单键覆写（**不钉级**）+ 机位从第 1 帧生效 + llm_shot 钉帧。
/// 每臂重建目录：臂与臂之间不共享可变配置（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const SurvArm& a, const SurvCamera& cam, const std::string& profileDir)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线，理由见头注
    o.size = { kSurvWidth, kSurvHeight };
    o.overrideShadowRoute = true;            // 第 4 例外门：钉 mode（izb 一并强制关，与 seam 同口径）
    o.shadowRouteMode = a.wantMode;
    o.overrideShadowLod = a.overrideLod;     // 第 6 例外门
    o.shadowLodValue = a.lod;
    // 🔴 第 7 例外门（force_cascade）**刻意不碰**：本仪器不钉级（回显 ② 反向核对缺席）。
    if (!cam.useFactoryCamera) {
        o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（RPC 补写会污染 TAA 历史）
        o.cameraPosition = { kSurvNearPosition[0], kSurvNearPosition[1], kSurvNearPosition[2] };
        o.cameraTarget = { kSurvNearTarget[0], kSurvNearTarget[1], kSurvNearTarget[2] };
        o.cameraFov = kSurvNearFov;
    }
    o.overrideLlmShot = true;  // 第 5 例外门：帧号是量具的一部分
    o.llmShotFile = "screenshots/imgqa_shadow_caster_survival_" + std::string(cam.name) + "_"
        + a.name + ".png";
    o.llmShotDelayFrames = kSurvDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    const std::string logText = readEngineLog();
    requireClusterPathLive("shadow-caster-survival", a.name, logText);  // 🔴 闸门 1
    requireArmEcho(a, logText);
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error("臂 " + a.name + "（机位 " + cam.name + "）没出图: " + png);
    return readPngBgr(png);
}

/**
 * 🔴 闸门 2（A 分母在场 → B 覆盖度非零，复用 seam metric 层的两道，**别重写**）。
 * 顺序与覆盖关系（B 是探测器、A 是归因器）住 `shadow_seam_metric.h` 两处声明的实测段，
 * 本处只写本仪器特有的一句：存活率判据的四项读数**全部**以 cov 为输入，分母一失效 ⟹ 每一臂
 * cov ≡ 0 ⟹ 参照臂本影为空 ⟹ 面积存活率先抛「参照本影为空」——那是**真因在分母**却报成
 * 「参照没影」的错位归因，所以 A/B 必须在读数之前。
 */
void requireSignalGates(const std::vector<cv::Mat>& img, const std::vector<cv::Mat>& cov,
    const std::vector<SurvArm>& arms, const cv::Rect& full)
{
    std::vector<std::pair<std::string, std::int64_t>> diffByArm;
    std::vector<std::pair<std::string, double>> fractionByArm;
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
        survPct3(minFrac->second).c_str(), minFrac->first.c_str(),
        survPct3(kSurvCoverageFloor).c_str());
    requireDenominatorAlive(diffByArm);                  // 闸门 2A
    requireCoveragePresent(fractionByArm, kSurvCoverageFloor);  // 闸门 2B（A 过了才有意义）
}

/// 往读数表追加一行（参照行 = covTest 与 covRef 同场，四项读数闭式恒等，正是闸门 3 的输入）。
void pushRow(SurvCameraReadout& r, int lod, const cv::Mat& covTest, const cv::Mat& covRef,
    const cv::Rect& full, const cv::Rect& nearBand, double flip)
{
    r.lods.push_back(lod);
    r.areaSurvival.push_back(umbraAreaSurvival(covTest, covRef, full));
    r.nearAreaSurvival.push_back(umbraAreaSurvival(covTest, covRef, nearBand));
    r.fullStats.push_back(umbraBlobStats(covTest, covRef, full));
    r.nearStats.push_back(umbraBlobStats(covTest, covRef, nearBand));
    r.flip.push_back(flip);
}

/**
 * 🔴 闸门 3 + 闸门 4（都在出表与判决线**之前**）：
 *  C 参照臂自洽 = 恰 1.000（阳性锚点：参照与自己比是恒等式，不是 1 = 参照臂接线错或算子坏）；
 *  D 参照臂与**见证臂**不得逐位相同（全臂相同 = 四项读数全 1.000 = 满分绿）。
 * 先 C 后 D：C 坏 = 分母本身不可信（表整个没意义），D 坏 = 分子全塌——归因顺序按「谁坏谁
 * 先说」。两道都抛时 C 的文案更具体（点名参照臂），所以 C 在前。
 *
 * 🔴 见证臂的选法（本批实机跑出来的第一条新知识，写死在这段注释里）：闸门 D 只装
 * **正常态必与参照不同**的臂，各堵一种失效，出处都是主 agent 实测：
 *   · `d11`（最粗档）堵「LOD 整体没生效」——覆写死掉时全臂塌成同图，d11 必在其中；
 *     主 agent 实测 d11 相对参照翻转率 0.994%（非零）⟹ 正常态必过。
 *   · `d5`（出厂档）堵「参照臂与出厂同档」（E6② 的形态：参照臂 shadow_lod 覆写打偏 =
 *     参照其实带着 D=5）；主 agent 实测 D=5 本影面积 = 参照的 100.5%（≠ 逐位相同）⟹ 正常态必过。
 * ⚠ 小 D **不能进见证清单**：本批实机首跑抓到 `d1` 与参照 cov 场**逐位相同**——LOD 级 1 在
 * 阴影图分辨率下与全量几何不可区分（d2 起才有差异）。那是合法态不是坏（它同时是一条免费的
 * 画质结论：D=1 的阴影侧损失低于量化噪声）。逐臂判会把正常构建判死，故只取两枚见证。
 */
void requireReferenceGates(SurvCameraReadout& r, const std::vector<cv::Mat>& cov,
    const cv::Rect& full, const cv::Rect& nearBand)
{
    const size_t refIdx = armIndex("ref");
    const cv::Mat& ref = cov[refIdx];
    const double areaSelf = umbraAreaSurvival(ref, ref, full);
    const double nearAreaSelf = umbraAreaSurvival(ref, ref, nearBand);
    const UmbraBlobStats fullSelf = umbraBlobStats(ref, ref, full);
    const UmbraBlobStats nearSelf = umbraBlobStats(ref, ref, nearBand);
    std::printf("  参照臂自洽（闸门 3，闭式恒等）：全幅面积 %s / 近场面积 %s / 消失 %lld / 变淡 %lld"
                "（全幅）\n",
        survFixed3(areaSelf).c_str(), survFixed3(nearAreaSelf).c_str(),
        static_cast<long long>(fullSelf.vanishedPx), static_cast<long long>(fullSelf.fadedPx));
    requireReferenceSelfConsistent(areaSelf, nearAreaSelf, fullSelf, nearSelf);

    for (const char* witness : { "d11", "d5" }) {  // 见证臂，理由见上方注释
        const size_t i = armIndex(witness);
        cv::Mat same;
        cv::compare(cov[i], ref, same, cv::CMP_NE);  // 逐位不同的像素（double 位比较，非容差）
        r.refDiffByArm.emplace_back(witness, cv::countNonZero(same));
    }
    requireLodChangesImage(r.refDiffByArm);  // 闸门 4
}

/// 扫描表（决策材料的本体；直接贴 `docs/还债实测数据.md`）。
void printTable(const SurvCameraReadout& r)
{
    std::printf("\n  ── 扫描表 @ %s（参照 = D=%d 全量几何；存活率 > 1 = LOD 轮廓略胖，实测事实）──\n",
        r.camera.c_str(), kSurvRefD);
    std::printf("  %-4s %-9s %-15s %-15s %-15s %-15s %-15s %-9s %-8s %-8s %-9s %-8s\n", "D",
        "本影面积", "<50px", "50–200", "200–1000", ">1000", "近场<50px", "近场面积", "消失px",
        "变淡px", "消失:淡", "翻转率");
    for (size_t i = 0; i < r.lods.size(); ++i) {
        const UmbraBlobStats& f = r.fullStats[i];
        const UmbraBlobStats& n = r.nearStats[i];
        const std::string ratio = (f.vanishedPx + f.fadedPx) == 0
            ? std::string("—")
            : survFixed3(static_cast<double>(f.vanishedPx)
                / static_cast<double>(f.vanishedPx + f.fadedPx));
        std::printf("  %-4d %-9s %-15s %-15s %-15s %-15s %-15s %-9s %-8lld %-8lld %-9s %-8s%s\n",
            r.lods[i], survFixed3(r.areaSurvival[i]).c_str(),
            survBucketCell(bucketSurvival(f, 0), f.bucketAlive[0], f.bucketTotal[0]).c_str(),
            survBucketCell(bucketSurvival(f, 1), f.bucketAlive[1], f.bucketTotal[1]).c_str(),
            survBucketCell(bucketSurvival(f, 2), f.bucketAlive[2], f.bucketTotal[2]).c_str(),
            survBucketCell(bucketSurvival(f, 3), f.bucketAlive[3], f.bucketTotal[3]).c_str(),
            survBucketCell(bucketSurvival(n, 0), n.bucketAlive[0], n.bucketTotal[0]).c_str(),
            survFixed3(r.nearAreaSurvival[i]).c_str(), static_cast<long long>(f.vanishedPx),
            static_cast<long long>(f.fadedPx), ratio.c_str(), survPct3(r.flip[i]).c_str(),
            r.lods[i] == kSurvRefD ? "  ← 参照（自洽恒等）"
                : (r.lods[i] == kSurvFactoryD ? "  ← 出厂档" : ""));
    }
}

/// 一台机位的完整读数：采齐全部臂 → cov 场 → 闸门 2 → 闸门 3/4 → 出表。
SurvCameraReadout collectCamera(const SurvCamera& cam, const std::string& profileDir)
{
    const std::vector<SurvArm>& arms = survArms();
    std::vector<cv::Mat> img(arms.size());
    for (size_t i = 0; i < arms.size(); ++i) {
        img[i] = captureArm(arms[i], cam, profileDir);
        std::printf("  ✓ [%s] 臂 %-4s %dx%d  D=%-2d\n", cam.name, arms[i].name.c_str(),
            img[i].cols, img[i].rows, arms[i].lod);
    }
    if (img[0].cols != kSurvWidth || img[0].rows != kSurvHeight)
        throw std::runtime_error("采集尺寸是 " + std::to_string(img[0].cols) + "x"
            + std::to_string(img[0].rows) + "，不是量具钉死的 " + std::to_string(kSurvWidth)
            + "x" + std::to_string(kSurvHeight) + "（llm_shot.size 没生效 = 近场带与分档全错位）");

    const cv::Rect full(0, 0, img[0].cols, img[0].rows);
    const cv::Rect nearBand(0, kSurvNearRow0, img[0].cols, kSurvNearRows);
    std::vector<cv::Mat> cov(img.size());
    for (size_t i = 1; i < img.size(); ++i)
        cov[i] = seamCovField(img[i], img[0]);  // 🔴 分母 = none 臂（E6③ 的变异点就在这行）
    requireSignalGates(img, cov, arms, full);

    SurvCameraReadout r;
    r.camera = cam.name;
    requireReferenceGates(r, cov, full, nearBand);

    pushRow(r, kSurvRefD, cov[armIndex("ref")], cov[armIndex("ref")], full, nearBand, 0.0);
    for (const int d : kSurvSweepD) {
        const size_t idx = armIndex("d" + std::to_string(d));
        const double flip = flipFraction(cov[idx], cov[armIndex("ref")], full);
        pushRow(r, d, cov[idx], cov[armIndex("ref")], full, nearBand, flip);
    }
    printTable(r);
    return r;
}

}  // namespace

size_t SurvCameraReadout::rowOf(int lod) const
{
    for (size_t i = 0; i < lods.size(); ++i)
        if (lods[i] == lod)
            return i;
    throw std::runtime_error("内部错误：机位 " + camera + " 的读数表里没有 D="
        + std::to_string(lod) + " 行（判据取数取到了不存在的行）");
}

int cmdShadowCasterSurvival()
{
    const std::string profile
        = projectRoot() + "/build/rgl/imgqa_shadow_caster_survival_profile";
    std::printf("shadow-caster-survival —— 阴影遮挡体存活率：**绝对量**，参照 = D=%d 全量几何"
                "（补 shadow-cascade-seam 对「四级一起变差」的结构失明）\n", kSurvRefD);
    std::printf("  仪器 = 现行选级（不钉级，csm.force_cascade 回显核对**缺席**）；配置 = 交付配置"
                "仓库 configs/（合成 profile 无簇产物 ⟹ LOD 不生效 ⟹ 恒绿）；帧号 = 第 %d 帧；"
                "采集 %dx%d\n", kSurvDelayFrames, kSurvWidth, kSurvHeight);
    std::printf("  机位 = near（债-139 第三机位 pos(%.6f,%.6f,%.6f) fov %.1f）+ far"
                "（出厂默认 = 不写 camera.lua = `just shot` 那枚）\n", kSurvNearPosition[0],
        kSurvNearPosition[1], kSurvNearPosition[2], kSurvNearFov);

    std::vector<SurvCameraReadout> cams;
    cams.reserve(survCameras().size());
    for (const auto& cam : survCameras())
        cams.push_back(collectCamera(cam, profile));

    // J1：阈值未定期（TODO(债-139)）——只报数不判，见头注与 judgments_table_survival.cpp。
    for (const auto& r : cams)
        printFactoryInfo(r);

    std::vector<Verdict> vs;
    pushMonotoneVerdict(vs, cams);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("shadow-caster-survival",
        { { "cameras", "near,far" },
            { "size", std::to_string(kSurvWidth) + "x" + std::to_string(kSurvHeight) },
            { "delay_frames", std::to_string(kSurvDelayFrames) },
            { "near_band", std::to_string(kSurvNearRow0) + "+" + std::to_string(kSurvNearRows) },
            { "arms", std::to_string(survArms().size()) },
            { "ref_lod", std::to_string(kSurvRefD) },
            { "factory_lod", std::to_string(kSurvFactoryD) } },
        vs);
    bool ok = true;
    for (const auto& v : vs)
        ok = ok && v.ok;
    std::printf("\n%s\n", ok ? "全部判据绿（⚠ J1 只报数不判——本批交付物是扫描表 + 四道闸门）"
                             : "有判据红（正文点名先查哪一层）。⚠ J2 红 = 扫描曲线有上升段，"
                               "先查选级表/回落策略而不是先查阴影质量");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
