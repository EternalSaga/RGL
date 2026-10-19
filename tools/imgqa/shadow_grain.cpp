// shadow-grain —— 影场颗粒度：**出厂档不许吃掉真实影带**（逐缝深存活率 + 接线锚点 + 出厂档对账）。
//
// ── 这台仪器防的是哪一个真实缺陷 ────────────────────────────────────────────────
// CSM 修「影子随相机游动」的九轮里每轮指标全绿而 bug 全程在场。根因不是哪条判据测错，
// 是**判据网里没有任何一条测这件事**：用户看到的是门板上的自阴影痤疮——那片噪声钉死在
// 世界表面（世界不变性完全成立！），相机一动屏幕像素就扫过它，观感 = 「门上有块影子在
// 爬」。本判据量的是影场在**世界空间上的高频程度**：共面墙 + 缝槽几何上，覆盖度只能沿
// 槽底影带的两条缘**各过渡一次**（闭式密度 = 2·count/(cols−1)）；除此之外同一遮挡关系
// 内部反复翻转 = 痤疮/噪声，不是影子。
//
// ── 判决方向（2026-10-19 纠正；「出厂态密度必须低」的方向已被证伪，别改回去）────
// 首版把 J1 定成「出厂态密度越低越绿（= 槽底影带被抹得干净）」，并为「出厂态 cov 全 0」
// 做过辩护。**该方向与辩护被两个实验证伪，正文已删（旧条目全文见 git history @
// shadow-grain 首版），只留证伪它的两个实验——它们是本判据方向的出处**：
//   实验 1（分辨率探针，bistro）：图集 2048→8192（纹素细 4 倍）、斜率偏置关闭，门板
//     覆盖度 0.071→0.087、两机位差 0.069→0.078——那片阴影**不随精度消失反而增强**
//     ⟹ 它是真实遮挡，不是量化伪影。
//   实验 2（靶上真影带存活率扫描，d=0.30 单缝深）：
//     | slope_bias | 靶上真实槽底影带密度 | 存活率 | bistro 两机位覆盖度差 | bistro 全图阴影面积 |
//     |---|---|---|---|---|
//     | 0  | 0.0842 | 100% | 0.069 | 29.6% |
//     | 4  | 0.0842 | 100% | 0.060 | 28.6% |
//     | 8  | 0.0842 | 100% | 0.057 | 28.0% |
//     | 12 | 0.0053 | 6.3% | 0.034 | 27.3% |
//     | 16 | 0.0000 | 0%   | 0.017 | 26.8% |
//     0→8 的稳定性改善是**白拿的**（真影带一根没掉）；8→16 那 70% 的「改善」与真实
//     影带被抹到 0% 发生在**同一段**——那不是治好，是把产生对比的阴影本身压掉了。
// 所以判据该守的是「**出厂档不许吃掉真实影带**」（存活率下限），不是「出厂档密度必须低」
// （抹得越干净越绿的旧方向，会把「把影子整个抹掉」这种最坏的「修法」判成满分）。
// 出厂值 csm.slope_bias = 8.0 的定档依据（configs/shadow.lua / rgl/shadow_config.cppm）
// 引用的就是这张表；本判据是那条定档的**机器判据**——出厂值改大偷吃真影带时它必须红。
//
// ── 与 `imgqa swim` 的分工（务必读，别写成谁替代谁）───────────────────────────
// swim 测**世界不变性**：同一个世界点的影值在两机位下变没变。那条性质是对的、该测、
// 且确实该绿——主 agent 实测把阴影盒沿吸附格挪整数格影场逐位相同，纹素吸附工作正常。
// 本判据不替代它，也不指认它「漏了」：痤疮恰好是**世界不变**的噪声，swim 对它结构性
// 失明不是 swim 的缺陷，是它的量程边界。两条合起来才封住「影子对不对」的两问：
// 影场该不动的地方不动（swim）；影场不该高频的地方不高频、且该在的真影带必须在（本条）。
//
// ── 口径（定义者已用 Python 参考实现验过算子自检，逐式转录，别「顺手优化」）──────
// 覆盖度 cov = mean_RGB clip((off − on)/(off + 1e-4), 0, 1)，**先转线性域**（sRGB 解码，
// 项目红线「统计一律线性域」）；on/off = 同机位同帧、只差 shadow.lua 的 mode csm/none
// （开关对拍，albedo/曝光/ACES/AO 全约掉，剩下只有阴影项）。
// 区域密度 density = |{(x,y) : bin(y,x) ≠ bin(y,x+1)}| / (rows × (cols−1))，bin = cov > 0.5。
// 逐带存活率 survival = 带窗内跳变数 / (2·rows)：带在场闭式 = 1、被抹 = 0，**分母是
// 闭式刻度**——阈值（kSurvivalFloor = 两闭式态中点 0.5）从刻度算，不抄实测数字。
// 算子本体住 `shadow_grain_metric.{h,cpp}`（口径 vs 仪器的缝），被 selftest 用闭式直接测
// （棋盘/竖条纹 = 恰 1.0、34 宽斜坡 = 恰 1/33、常数 = 恰 0、带在场存活率 = 恰 1.0 ——
// 判据 4，`just tools-check` 覆盖）。
//
// ── 靶 = 程序化 shadow_grain（区域必须来自几何，这是 imgqa 的硬纪律）────────────
// 主 agent 在 bistro 门板上的发现地读数（slope=0 → 0.123/0.139，slope=16 → 0.079/0.068）
// 用的是固定屏幕矩形——**不可常驻**（换机位即失效）。读图定案（2026-10-18）：门板窗口的
// 72 个二值翻转全部单向（0 遮 / 16 受光）、列分布集中在两列 = 门板**竖直拼缝槽**的槽底
// 影带被斜率偏置抹除。常驻靶 = `tools/qa_scenes.py` 场景 10 把该形态程序化重造：朝光墙
// （N·L̂ = 0.6）+ **多缝深交错缝槽**（每档缝深一条缝、背板各退本组 d；2026-10-19 加密，
// 首版只有 0.30 m 一个缝深——出厂定档 8 只守住了那一个深度，「更细的缝会不会在 8 之下就
// 会被提前抹掉」是 rgl/shadow_config.cppm 注释里挂号的无人守盲区，本批把它变成有人守；
// 每档一条而非每组四条横向铺开，是实机翻车后定的——宽度撑大 SceneBounds 会漂 span，
// 见 qa_scenes.py 常量注释）。
// 槽底闭式影带 s = d·du/dn；分离窗 d ∈ span·(0.0015/dn + 1.5/1024, 0.0015/dn + 17.5/1024)
// （下界 = slope=0 存活，上界 = slope=16 抹除；推导与已证伪的旧模型见场景头注，改靶先读）。
// 判据窗口 = manifest `measure` 块的世界坐标角点经针孔投影（墙面 ⊥ 视轴 ⟹ 轴对齐屏幕
// 矩形），C++ 不重抄任何几何常量（重抄 = 第二真相，改靶必漂）。闭式刻度：region 窗
// density = 2·总带数/(cols−1)；逐带存活窗 density 刻度 = 2/(存活窗 cols−1)。
//
// ── 三条判决线为什么这样分（缺一条就退化成假绿）───────────────────────────────
// 1 `shadow-grain-band-survival`   **J1**：出厂档（引擎默认，见 J3）下每条缝的带存活率
//    ≥ kSurvivalFloor，取最严一条判。这条守的是「出厂档不许吃掉真实影带」——上一批的
//    「出厂态密度 ≤ 上限」方向被证伪后退役（论证全文见上「判决方向」）。
// 2 `shadow-grain-wiring-anchor`   **J2（接线阳性锚点，本判据的命门）**：拿 slope_bias 当
//    自变量证明「旋钮 → 画面」的通路活着：csm0 臂与 csm32 臂的区域密度必须**显著不同**
//    （差 ≥ kWiredGapFloor×闭式刻度）且大值臂**趋 0**（≤ 半条带的残留增量）。它红的
//    时候 J1 的绿一律不作数——J1 单独存在时，「靶的 d 掉出分离窗」（级联 span 漂了，
//    penumbra_ladder 的前车之鉴）与「出厂档确实保住了带」给出同一个绿。
// 3 `shadow-grain-factory-identity` **J3（出厂档对账，J1 的读数成为陈述的 precondition）**：
//    J1 的读数来自「不写 slope_bias 键」的臂 = 引擎出厂默认档——**上一批把臂写成显式
//    slope_bias=16 覆写，出厂值一改判据测的就不是出厂态了**。默认臂必须与两枚参照臂
//    **逐位相同**：csm0 臂（shadow.lua 显式写 0.0）⟹ 证明「引擎代码默认确实是 0」；
//    deliv 臂（仓库 configs/shadow.lua 全文）⟹ 证明「交付出厂值确实是 0、且与代码默认
//    同值」。默认漂了（改代码默认或改 configs），J3 立刻红，J1 的读数失去解释——
//    这条对账本身要能红：把 rgl/shadow_config.cppm 的默认改 8 → 默认臂变、csm0 臂不变
//    ⟹ J3 红且 J1 同红；把 configs/shadow.lua 的出厂值改 8 → deliv 臂变 ⟹ J3 红。
//    ⚠ csm0 臂**没有**回显差集行（0 = 出厂默认 ⟹ 差集为空、引擎不打第二行），它的证据
//    就是本条的逐位对账；csm32 臂仍带差集行（32 ≠ 默认 ⟹ 必打）。
// 4 算子自检（selftest_grain.cpp，无引擎毫秒级，`just tools-check` 在 gates 里）。
//    为什么锚点直接拿被测对象的旋钮（slope_bias）而不怕循环论证：这里要验的是**接线与
//    响应方向**（bias↑ ⟹ 槽带被抹除是闭式结论，不依赖本算子），不是阴影的正确性——
//    grain_metric.py 参考实现头注同判；与 kernelscale 用 PCSS 当「间隙→半影」锚点是
//    两种不同的锚（那条要验的是独立实现的量值，本条要验的是旋钮到画面的通路）。
//
// ── 槽带在场闸门（不是 Verdict，是**抛**，blend-sort 三条前置自检的先例）────────
// **csm0 臂**的 cov 场必须真的含那几条闭式影带：每条带的中央 50% 列区间 cov 均值 > 0.5、
// litProbe（分隔墙块中心）邻域均值 < 0.5。**逐条缝各判一次**——画不出带的缝是死缝，
// 让它进 J1 只会多一个恒真好消息（penumbra_ladder：「靶瞎了」与「带被正确抹除」给出
// 同一个绿）。为什么闸门在 csm0 而不是出厂臂：出厂档的存活率读数需要一个「带本来画得
// 出来」的阳性对照——出厂档抹掉与靶根本没画，在出厂臂的密度里是同一个 0，两种解释必须
// 在出判据**之前**被分开。⚠ 出厂值 = 0 时 csm0 与 dflt **同值**（这是 J3 要证的结论，
// 不是巧合），闸门读的是同一份图——它仍然成立，因为闸门问的是「靶画没画」，与哪一臂无关。
// 闸门红在 rc=3（异常路径），与「缺靶不 SKIP」同形。
//
// ── 为什么没有 flag ────────────────────────────────────────────────────────────
// 靶、臂、帧号、阈值都是量具的组成部分：能调 = 能调绿（同 shadow-softness /
// shadow-kernel-scale）。帧号走 llm_shot.delay_frames 钉死（RPC /shot 落在哪帧 = 墙钟）。
//
// ── 变异红线（哪个变异让我红；实跑读数见 2026-10-19 交付记录，弱化形态在内）──────
//   · 灵魂变异：rgl/shadow_config.cppm 出厂默认 0.0 → 16.0（重编）→ **J1 红**（靶上带
//     被抹光，存活率 → 0）**且 J3 红**（默认臂 ≠ csm0 臂）。这条红不了本判据就是摆设。
//     调到 8.0 同样红（d ≤ 0.20 三条缝存活率 0）——**本判据守的是"出厂档不许吃真影子"，
//     任何非零默认值都过不了 5 档扫描面**。
//   · configs/shadow.lua 出厂值 0.0 → 16.0（不改代码不重编）→ **J3 红**（deliv 臂 ≠
//     csm0 臂）——「交付出厂值漂了」与「代码默认漂了」是两条独立的漂移缝，J3 两条都守。
//   · J1 存活率下限挪到 > 1（1.05）→ 红（连最粗的缝 1.000 都不够，线在承重）。
//   · J2 高臂写成与 0 臂同值（连同 wantExtra 一起改，防回显守卫先抛）→ 两臂同图、
//     差 = 0 → J2 红；只改值不改 wantExtra → 回显差集行缺席 → requireShadowEcho 抛
//     （静默换臂是本项目最怕的失效，两道都响）。
//   · 把某条缝的闭式带位置挪出真带（manifest 角点平移 = 模拟「d 掉出分离窗」）→ 该缝的
//     在场闸门抛（rc=3，探针 0.244 点名到缝），不许静默进 J1；挪过头撞进邻带存活窗 →
//     互不重叠守卫先抛（rc=3）。生成器侧把 d 直接改到画不出带会先撞 assert ⑤′/⑧——
//     **两层都算「死缝进不了判据」**。
//   · 去掉线性域转换（字节域算 cov）→ selftest 线性域锚点红；实机两臂密度同时漂移。
//   · 只数单向跳变 → selftest 棋盘/竖条纹/带在场三条红。
//
// ── 跑起来该看到什么 ─────────────────────────────────────────────────────────
// 五臂各一次引擎进程（串行，单实例纪律）；打印逐臂密度 + 逐带存活率表 + 三条判决线
// 与容差理由。现状（2026-10-19 本机 RADV STRIX_HALO，**出厂 slope_bias = 0**）：三条判决
// 全绿，dflt 臂五条缝存活率逐条 = 1.000。本仪器是 2026-10-19 那次定档的**产物与护栏**：
// 主 agent 先按单缝深（d=0.30）扫出「8 = 零真影子损失最右端」并据此定了 8，加密到
// 0.14–0.30 五档后该结论**被推翻**——
//   存活率@    0.30   0.24   0.20   0.17   0.14   （列 = 缝深 m）
//   slope 0    1.00   1.00   1.00   1.00   1.00   ← 出厂
//   slope 4    1.00   1.00   1.00   1.00   0.00
//   slope 6    1.00   1.00   1.23   0.00   0.00
//   slope 8    1.00   1.40   0.00   0.00   0.00
//   slope ≥12  0.00   0.00   0.00   0.00   0.00
// ⟹ **斜率偏置的收益区间与它的真影子损失区间重合**（bistro 稳定性收益 0→8 只有 17%，
// 而 8 档把 d ≤ 0.20 的真实遮挡带整条抹光）⟹ 出厂取 0，本键留作可调旋钮。
// 教训（写进还债总账 债-139）：**扫描面只有一档时得出的"最右端"是那一档的局部事实**。
#include "imgqa.h"
#include "shadow_grain.h"
#include "shadow_grain_metric.h"
#include "shadow_kernel_metric.h"  // makeProjector：投影口径唯一出处

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;

/// 读仓库**交付配置** configs/shadow.lua 全文（deliv 臂的 shadow.lua）。读不到 = 抛——
/// 「交付出厂值」是 J3 对账的一枚伙伴，缺了它 J3 只剩半边。
std::string readDeliveryShadowLua()
{
    const fs::path p = fs::path(projectRoot()) / "configs/shadow.lua";
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到交付配置 " + p.string()
            + "\n  deliv 臂（J3 的对账伙伴之一）没有内容可用——交付出厂值漂了/没了，"
              "J1 的读数失去解释，拒绝跑");
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// 五臂（顺序 = GrainArmIdx）。`dflt` = **不写 slope_bias 键**（吃引擎默认，J1 的读数）；
/// `csm0` = 显式 0.0（对账伙伴 A：「引擎代码默认确实是 0」+ J2 低臂 + 在场闸门载体）；
/// `deliv` = 交付配置全文（对账伙伴 B：「交付出厂值确实是 0」）；`csm0`/`csm32` = J2 的两臂 + csm0 兼在场闸门
/// 载体；`none` = 覆盖度分母参照。
/// ⚠ csm0 臂把 0.0 **写死在臂表里**是刻意的：对账的语义 = 「引擎默认 == 本仪器解释 J1 时
/// 假定的那个出厂值」。出厂值一旦改（0 → 8），J3 立刻红，逼改的人回到本文件重新对账——「出厂值一改，判据测的就不是出厂态了」这条上一批的教训由 J3 机制化，不靠自觉。
const std::vector<GrainArm>& grainArms()
{
    static const std::vector<GrainArm> kArms = [] {
        std::vector<GrainArm> a;
        a.push_back({"none", "none", "", "return { mode = \"none\", izb = { on = false } }\n"});
        a.push_back({"dflt", "csm", "", "return { mode = \"csm\", izb = { on = false } }\n"});
        a.push_back({"deliv", "csm", "", readDeliveryShadowLua()});
        // ⚠ csm0 臂**没有** wantExtra 证据行：出厂默认 = 0 ⟹ 写 0.0 差集为空、引擎不打
        //   第二行（查"差集行缺席"有两种解释 = 拿弱证据冒充强证据，本项目红线）。它的
        //   证据改由 J3 提供：dflt（不写键）与 csm0（显式 0.0）必须**逐位相同**，
        //   不同 = 代码默认不是 0。csm32 臂仍带 wantExtra（32 ≠ 默认 ⟹ 差集行必打）。
        a.push_back({"csm0", "csm", "",
            "return { mode = \"csm\", izb = { on = false }, csm = { slope_bias = 0.0 } }\n"});
        a.push_back({"csm32", "csm", "csm.slope_bias=32",
            "return { mode = \"csm\", izb = { on = false }, csm = { slope_bias = 32.0 } }\n"});
        return a;
    }();
    return kArms;
}

/**
 * 引擎回显对账（同 kernelscale 的 requireShadowEcho，证据链不同故重写一份而非共用）：
 * 路线行 `shadow: mode=…` 证明**整份覆写生效**（合成路径不覆写时 mode 会是 none，
 * 「写了没生效」在这里不可能伪装成 mode=csm）；`csm0`/`csm32` 臂另查差集行
 * `csm.slope_bias=0` / `=32`（≠ 出厂默认 ⟹ info 差集行必打；键拼错则被未知键护栏换成
 * warn，差集行同样缺席）。
 * ⚠ `dflt`/`csm0`/`deliv` 三臂**没有**专属证据行（0 = 出厂默认，差集为空不打第二行）——
 * 它们的证据 = 路线行 mode=csm（覆写文件在场）+ J3 的逐位对账（三臂互为对账伙伴，
 * 任何一枚漂了图就不同）。不查「差集行缺席」（缺席有两种解释，拿弱证据冒充强证据是
 * 本项目红线）。
 */
void requireShadowEcho(const GrainArm& a)
{
    const fs::path log = fs::path(projectRoot()) / "build/rgl/imgqa_run.log";
    std::ifstream in(log, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到引擎日志: " + log.string());
    const std::string txt{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const std::string want = "shadow: mode=" + a.wantMode;
    if (txt.find(want) == std::string::npos)
        throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），全部读数作废，见 " + log.string());
    if (!a.wantExtra.empty() && txt.find(a.wantExtra) == std::string::npos)
        throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + a.wantExtra
            + "` ⟹ slope_bias 键没被解析器读到（拼错键 = 静默拿出厂值），"
              "接线阳性锚点臂名不副实，见 "
            + log.string());
}

/// 采一臂：合成 profile（靶 + 机位从第 1 帧生效 + 整份 shadow.lua）→ 引擎跑到自然退出。
/// 每臂重建目录：臂与臂之间不共享可变配置（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const GrainArm& a, const GrainTarget& t, const std::string& profileDir)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.glb = std::string(kGlbStem) + ".glb";  // ⚠ 带扩展名（ProfileOpts::glb 是整文件名）
    o.picking = false;  // 区域来自几何（manifest 角点投影），不消费 object_id
    o.shadowLuaOverride = a.shadowLua;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（不是 RPC 补写）
    o.cameraPosition = t.camera.position;
    o.cameraTarget = t.camera.target;
    o.cameraFov = t.camera.fovDegrees;
    o.overrideLlmShot = true;
    o.llmShotFile = "screenshots/imgqa_shadow_grain_" + a.name + ".png";
    o.llmShotDelayFrames = kDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    requireShadowEcho(a);
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error("臂 " + a.name + " 没出图: " + png);
    return readPngBgr(png);
}

/// 槽带在场闸门（抛 = rc 3，不是 Verdict——理由见头注「槽带在场闸门」）。跑在 **csm0 臂**：
/// 出厂档抹掉与靶根本没画在出厂臂密度里是同一个 0，只有在 csm0 臂逐条证明带在场，
/// 出厂档的存活率才是陈述。**逐条缝各判一次**，失败清单一次报全（逐条抛会让第二条死缝
/// 藏到下一轮）。geomByBand 与 t 的展平带序一致（组序 × 组内带序）。
void requireBandsPresent(const std::vector<cv::Mat>& covByArm, const GrainTarget& t,
    const std::vector<GrainBandGeom>& geomByBand, const Projector& proj,
    const GrainWindow& region)
{
    const cv::Mat& cov0 = covByArm[kArmCsm0];
    std::string dead;
    size_t flat = 0;
    for (size_t gi = 0; gi < t.groups.size(); ++gi) {
        for (size_t bi = 0; bi < t.groups[gi].bands.size(); ++bi, ++flat) {
            const double m
                = columnsMean(cov0, geomByBand[flat].probe, region.win);
            std::printf("缝%zu(d=%.2f) %.3f ", gi, t.groups[gi].depthM, m);
            if (m <= kGrainBin)
                dead += (dead.empty() ? "" : "\n  ") + std::string("缝") + std::to_string(gi)
                    + "（d=" + std::to_string(t.groups[gi].depthM).substr(0, 4)
                    + " m）探针 " + std::to_string(m) + " ≤ 0.5";
        }
    }
    const double pLit = probeMean(cov0, proj, t.litProbe, region.win, "litProbe");
    std::printf("受光参照 %.3f（带须 >0.5、参照须 <0.5）\n", pLit);
    if (pLit >= kGrainBin)
        dead += std::string(dead.empty() ? "" : "\n  ")
            + "litProbe 受光参照 " + std::to_string(pLit)
            + " ≥ 0.5（恒受光墙块不恒受光 = 机位/靶/光照漂了）";
    if (!dead.empty())
        throw std::runtime_error(
            "闭式槽带不在场（csm0 臂探针不过线）——slope_bias=0 档槽底影带没画在 region 上"
            "（d 掉出分离窗 / 级联 / 回显任一漂了），密度读数没有意义，拒绝出判据：\n  "
            + dead
            + "\n  死缝不许静默进 J1（penumbra_ladder 教训）：要么改回分离窗内的缝深"
              "（qa_scenes.py 场景 10 头注「机制」「靶子加密」），要么删该缝并在交付说明"
              "里写明。");
}

/// 存活窗互不重叠守卫：一条带的 ±裕量窗吃进邻带 = 把邻带的缘算进本带跳变 = 存活率虚高
/// （两条缝都能「互相证明对方在场」）。生成器 assert ⑪ 在构建期钉几何，这里在实机投影
/// 后复核（投影/取整/机位漂移的合谋只有跑起来才现形）。
void requireSurvivalWindowsDisjoint(const std::vector<GrainBandGeom>& geomByBand)
{
    std::vector<std::pair<int, int>> spans;
    for (const auto& g : geomByBand)
        spans.emplace_back(g.survival.start, g.survival.end);
    std::sort(spans.begin(), spans.end());
    for (size_t i = 1; i < spans.size(); ++i)
        if (spans[i].first < spans[i - 1].second)
            throw std::runtime_error(
                "相邻两条带的存活窗重叠（" + std::to_string(spans[i - 1].second) + " > "
                + std::to_string(spans[i].first)
                + "）——邻带的缘会被算进本带的跳变，逐带存活率互相污染，拒绝求值");
}

/// 逐带读数（J1 的表）：每条缝在出厂档与 csm0 档各出一个存活率（从 cmdShadowGrain 拆出
/// 守认知复杂度阈值；打印与语义一字未动）。geomByBand 展平序 = 组序 × 组内带序。
std::vector<GrainBandReadout> collectBandReadouts(const GrainTarget& t,
    const std::vector<GrainBandGeom>& geomByBand, const std::vector<cv::Mat>& cov,
    const GrainWindow& region)
{
    std::vector<GrainBandReadout> readouts;
    std::printf("\n  逐带读数（存活率 = 跳变/(2·rows)，闭式：在场 = 1、被抹 = 0）：\n");
    size_t flat = 0;
    for (size_t gi = 0; gi < t.groups.size(); ++gi) {
        for (size_t bi = 0; bi < t.groups[gi].bands.size(); ++bi, ++flat) {
            const GrainBandGeom& geom = geomByBand[flat];
            GrainBandReadout r;
            r.group = gi;
            r.indexInGroup = bi;
            r.depthM = t.groups[gi].depthM;
            r.bandStep = 2.0 / static_cast<double>(geom.survival.end - geom.survival.start - 1);
            r.survivalDefault = grainBandSurvival(cov[kArmDefault], geom.survival, region.win);
            r.survivalCsm0 = grainBandSurvival(cov[kArmCsm0], geom.survival, region.win);
            r.densityDefault = r.survivalDefault * r.bandStep;
            readouts.push_back(r);
            std::printf("    缝%zu d=%.2fm 存活窗 %dpx 闭式增量 %s：密度@出厂 %s "
                        "存活率@出厂 %s 存活率@csm0 %s\n",
                gi, r.depthM, geom.survival.end - geom.survival.start,
                grainFixed4(r.bandStep).c_str(), grainFixed4(r.densityDefault).c_str(),
                grainFixed3(r.survivalDefault).c_str(), grainFixed3(r.survivalCsm0).c_str());
        }
    }
    return readouts;
}

}  // namespace

int cmdShadowGrain()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_grain_profile";
    std::printf("shadow-grain —— 影场颗粒度：**出厂档不许吃掉真实影带**（靶 = shadow_grain）\n");
    std::printf("  判决方向（2026-10-19 纠正）：守「出厂档真影带存活率下限」，不是「出厂档密度必须低」\n");
    std::printf("  帧号 = 第 %d 帧（llm_shot.delay_frames 写死）；采集 %dx%d\n", kDelayFrames,
        kWidth, kHeight);

    const GrainTarget t = loadGrainTarget();  // 缺靶 = 抛（不 SKIP）
    std::printf("  靶 = %s（朝光墙 + %zu 个缝深组 × 共 %zu 条闭式带，区域 = manifest 角点投影）\n",
        kGlbStem, t.groups.size(), t.bandCount());

    std::array<cv::Mat, kArmCount> img;  // 下标 = GrainArmIdx
    for (size_t i = 0; i < grainArms().size(); ++i) {
        img[i] = captureArm(grainArms()[i], t, profile);
        std::printf("  ✓ 臂 %-6s %dx%d\n", grainArms()[i].name.c_str(), img[i].cols, img[i].rows);
    }

    const Projector proj = makeProjector(t.camera, img[0].cols, img[0].rows);
    const GrainWindow region = projectRegionWindow(proj, t);
    std::printf("  region 窗 = (%d,%d) %dx%d px ⟹ 闭式刻度 2·%zu/(cols−1) = %s\n", region.win.x,
        region.win.y, region.win.width, region.win.height, t.bandCount(),
        grainFixed4(region.closed).c_str());

    std::vector<cv::Mat> cov(grainArms().size());
    for (const size_t i : { static_cast<size_t>(kArmDefault), static_cast<size_t>(kArmCsm0),
                          static_cast<size_t>(kArmCsm32) })
        cov[i] = grainCoverage(img[i], img[kArmNone]);

    // 逐带几何（存活窗 + 探针窗，展平序 = 组序 × 组内带序）；先复核互不重叠再进闸门。
    const std::vector<GrainBandGeom> geomByBand = projectAllBandGeoms(t, proj, region);
    requireSurvivalWindowsDisjoint(geomByBand);
    requireBandsPresent(cov, t, geomByBand, proj, region);

    // ── 逐带读数（J1 的表）：每条缝在出厂档与 csm0 档各出一个存活率 ──────────────
    const std::vector<GrainBandReadout> readouts
        = collectBandReadouts(t, geomByBand, cov, region);

    const double d0 = grainDensity(cov[kArmCsm0], region.win);
    const double d32 = grainDensity(cov[kArmCsm32], region.win);
    const double bandStep = region.closed / static_cast<double>(t.bandCount());
    std::printf("\n  区域密度：csm0（阳性）= %s   csm32（大值臂）= %s   闭式刻度 = %s\n",
        grainFixed4(d0).c_str(), grainFixed4(d32).c_str(), grainFixed4(region.closed).c_str());

    // J3 对账：三枚出厂档臂**逐位**相同（全图面，不是判据窗口——窗口外的差异同样是漂）。
    const auto diffPx = [&](const cv::Mat& a, const cv::Mat& b) {
        return cv::countNonZero(anyChannelDiff(a, b));
    };
    const std::int64_t defaultDiff = diffPx(img[kArmDefault], img[kArmCsm0]);
    const std::int64_t deliveryDiff = diffPx(img[kArmDelivery], img[kArmCsm0]);
    std::printf("  出厂档对账（逐位不同的像素数）：dflt vs csm0 = %lld   deliv vs csm0 = %lld\n",
        static_cast<long long>(defaultDiff), static_cast<long long>(deliveryDiff));

    std::vector<Verdict> vs;
    pushSurvivalVerdict(vs, readouts);
    pushWiringVerdict(vs, d0, d32, region.closed, bandStep);
    pushIdentityVerdict(vs, defaultDiff, deliveryDiff);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    bool wiringOk = true, identityOk = true;
    for (const auto& v : vs) {
        if (v.name == "shadow-grain-wiring-anchor")
            wiringOk = v.ok;
        if (v.name == "shadow-grain-factory-identity")
            identityOk = v.ok;
    }
    if (!wiringOk || !identityOk)
        std::printf("\n  ⚠ J2/J3 有红：J1 的绿**不作数**（旋钮没接线或出厂档漂了，"
                    "存活率读数没有解释）\n");
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("shadow-grain",
        {{"glb", std::string(kGlbStem)}, {"delay_frames", std::to_string(kDelayFrames)},
            {"arms", "none,dflt,deliv,csm0,csm32"},
            {"window", std::to_string(region.win.width) + "x"
                + std::to_string(region.win.height)}},
        vs);
    bool ok = true;
    for (const auto& v : vs)
        ok = ok && v.ok;
    std::printf("\n%s\n", ok ? "全部判据绿" : "有判据红（正文点名先查哪一层）");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
