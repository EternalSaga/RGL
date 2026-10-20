// shadow-cascade-seam —— 相邻级联对「这块表面有没有影子」的分歧：**远级簇 LOD 不许造出缝**。
//
// ── 这台仪器防的是哪一个缺陷类 ──────────────────────────────────────────────────
// **缺陷类 = 相邻级联对同一块表面的「有没有影子」给出不同答案。** 已定死的实例 = 债-104 的
// 远级簇 LOD：`rgl/render_list_mdi.cppm:284` 的 `if (c >= 1u && cd.meshletPipeline != …)`
// ⟹ **级 0 画全量几何、级 1..3 画粗簇 LOD**，于是 c0 的阴影图里有某个遮挡体、c1 的图里没有它。
// 用户报障（债-139）= 那块地面随相机移动突然出现/消失一片阴影，肉眼可见；主 agent 用级号
// 哨兵把交界定死在第 476 行，台阶是其他所有行的 4–5 倍。
//
// ── 为什么必须用**差分**，不能用屏幕空间统计量（本判据的立命之本，别改回去）──────────
// 上一批先试过「单张 cov 场里最硬的横向台阶」，**主 agent 实测证伪**（1600×900、本机位）：
//   | 统计量                                   | D=5            | D=-1（LOD 关）  |
//   |------------------------------------------|----------------|-----------------|
//   | 单张 cov 场·**全幅**最大横向台阶         | 0.699 @ 行 52  | **0.699 @ 行 52（逐位同值）** |
//   | 单张 cov 场·最大台阶（手工挑 x∈[680,1150]）| 0.704 @ 行 476 | 0.394           |
// 全幅上两臂同值 ⟹ **那个统计量对 LOD 是瞎的**（缝被无关的真影边淹掉），只有手工挑段才现形
// = 看着答案定窗口。照它建判据会在 LOD 坏掉时照样绿，精确重演这次漏检。
//
// 正确仪器 = **强制选级差分**（诊断键 `csm.force_cascade`，commit 6cd65b4 入库）：它纯覆写
// 写进 scene data 的那份 `splits`（**不改级联几何**，CPU 侧级联球/切片照常算），force=c 与
// force=c+1 两臂**唯一差别 = 采哪张阴影图** ⟹ ① 无关几何自动相消；② 出块旁路（`shadow.slang`
// ④）给的 cov=0 在 D 两臂之间是**共模**，做差即消——这正是单张统计量淹死它的那一项。
//
// ── 区域为什么是固定带而不是几何投影（imgqa 硬约束①的**已知偏离**，写清楚不装看不见）────
// 台阶带 = 中央 1/2 幅（列 x∈[400,1200)）× 行 [30,870]。它不是 objectID/manifest 反解出来的。
// 为什么这里可以：判据量的是**差分场**上的台阶，而差分场已经把无关几何消成 0（上面第②条），
// 所以「带内有没有缝」不再依赖「带是不是某块表面的投影」；反过来，**挑带子救不了判据**——
// 上表第二行就是「手工挑段让单张统计量现形」的形态，而差分场在**全幅**上给同一个 0.379 @ 行
// 472（主 agent 实测），中央带只是把上下 6 行的窗留在画面内、并把左右两侧的建筑边缘排除掉。
// ⚠ 这条偏离**只对差分场成立**：任何拿本文件的带子去量单张 cov 场的改动都会把上面那个被证伪
// 的统计量重新请回来。
//
// ── 🔴 为什么必须走 deliveryConfigDir（仓库 configs/），合成 profile 一条都不许用 ──────
// ① `engine_profile.cpp` 那条既有红线：**判据跑在合成 profile 上得出的绿不算绿**
//    （`temporal-identity` 用它换过一整轮假绿）。
// ② 🔴 更要命的一条是本仪器特有的：**簇产物清单只有 `assets/bistro_exterior.glb`**
//    （`cmake/RglMeshlets.cmake` 的 RGL_MESHLET_MODELS）。合成靶（shadow_grain.glb 之类）
//    旁没有 `.meshlets.bin` ⟹ 引擎只打一条 INFO 就静默回落旧路径（`rgl/main_scene.cppm:85`）
//    ⟹ **LOD 根本不生效** ⟹ 两臂逐位相同 ⟹ 本判据恒绿 = 一台永远绿的仪器。恒绿比红危险得多。
// 代价是臂表只能改交付配置的两枚键（第 6/7 例外门），不能换场景、不能换尺寸之外的任何事。
//
// ── 🔴 簇路径生效闸门（不是 Verdict，是**抛**，rc=3）——这条比 J1 更重要 ──────────────
// 上面 ② 的失效是**静默**的，所以每臂跑完先查引擎日志：
//   正向证据 = `已加载: prims=`（`main_scene.cppm:96`，簇产物真的读进来了）
//   负向证据 = `无簇产物（` / `走现行路径` / `回落现行路径`（`main_scene.cppm:85/91` 的原文）
// 任一负向出现、或正向缺席 ⟹ 抛，点名「簇路径没生效，全部读数作废」并给生成产物的命令。
// ⚠ 为什么查的是 `无簇产物（` 而**不是**裸的 `无簇产物`：健康的「阴影远级联 LOD 生效」INFO
//   里也含这四个字（`… 可切图元 1530 doubleSided 不切 0 无簇产物 0（blend 图元不投影…）`），
//   拿裸串当负向证据会把每一臂都判死 = 一台永远红的仪器（与恒绿同罪）。**去读准确文案，
//   别照抄文档里的关键词**——这条是本次实现时实测抓出来的。
// 两道正向对账（`已加载` 之外还有 `min(D=<D>,` 与 `csm.force_cascade=<值>`）见 requireArmEcho。
// 闸门本体已搬进 `cluster_path_gate.{h,cpp}`（与 shadow-caster-survival 共用一份，单份真相）。
//
// ── 🔴 参照臂有效性闸门（不是 Verdict，是**抛**，rc=3）——带子是 J1 的承重项，它裸奔过一次 ──
// J1 的形状是「两臂台阶之差 ≤ 上限」，它**整个押在**「参照臂那一侧是常数（闭式 0，最多一个
// 纹素）」上。带子取窄还是取宽会直接决定这句话成不成立，而**上一批没有任何东西守着它**
// （2026-10-19 主 agent 亲验，同一批 13 臂图、同一个 J1，只换带子）：
//   带子 x 区间            D=5 台阶        参照臂台阶        J1 差      上限 0.1 的判定
//   [400,1200) 中央 1/2    0.379 @ 行472   0.000             +0.379     红 ✓（现用带子）
//   [160,1440) 去边 10%    0.327 @ 行472   **0.239 @ 行51**  +0.088     **绿 ✗ 假绿**
//   [0,1600) 整幅          0.188 @ 行476   0.006 @ 行48      +0.182     红 ✓
//   中央 1/4 偏左          0.381 @ 行474   0.000             +0.381     红 ✓
//   中央 1/4 偏右          0.397 @ 行465   0.091 @ 行668     +0.306     红 ✓
//   [200,1400) 宽 3/4      0.350 @ 行51    0.320 @ 行51      +0.030     **绿 ✗ 假绿**
// **六格里两格在缺陷在场的情况下报绿。** 机理：画面顶部（远端像素被钉进近级 ⟹ 落在该级盒子外
// ⟹ `shadow.slang` ④ 出块旁路返回全亮）在**两个臂里都**造台阶，而 `maxRowStep` 取的是逐行
// 最大——最大项一旦被行 51 那种污染项抢走，缝（行 472）的信号就从统计量里整个消失，剩下的
// 两臂残差近似相消但**不精确**，残差把信号盖掉。
// 闸门 = `requireBandValid(参照臂台阶, 所在行, 带子, kSeamTexelFloor)`（实现在 metric 层，
// 界为什么是闭式可辩护的住 `shadow_seam_metric.h` 声明处）：参照臂读数 > 固有下界 ⟹ 带子里
// 混进了**不是纹素量化**的东西 ⟹ J1 的「LOD 归因」不成立 ⟹ 抛，**一条 Verdict 都不产**。
// ⚠ 它是**量具自检不是画质判据**：它抛不代表阴影变好或变坏，只代表这台量具当前问不出这个问题。
// 顺序：闸门在 `collectReadout` 之后、三条判决线之前（先出数再拦，红的时候人眼能看到那两行台阶）。
// 实机自证（验收 W2）：带子临时改 160/1280 ⟹ rc=3 点名「带子已失效」且判决线 0 条；改回
// 400/800 ⟹ 闸门过、J1 红。
//
// ── 🔴 两道「阴影信号在场」闸门（同样是**抛**，rc=3）——分母一坏这台仪器就永久全绿 ──────
// cov 的公共分母是 `none` 臂。它一失效 ⟹ 每一臂 cov ≡ 0 ⟹ **三条判决线一起变绿**（Δ ≡ 0 ⟹
// J1 绿；全 0 满足"单调 + 饱和 + 反向对照 0" ⟹ J3 绿；J2 比的是两 force 臂的原始图、根本不吃
// 分母 ⟹ 照样绿），带子闸门也放行（参照臂台阶 = 0 ≤ 界）。2026-10-19 主 agent 实测：把
// `seamCovField(img[i], img[0])` 的分母换成分子自身 ⟹ **全部判据绿 rc=0**。
//   闸门 A `requireDenominatorAlive`：`none` 臂与每条 csm 臂**不得逐位相同**（布尔，无阈值）。
//   闸门 B `requireCoveragePresent`：每条 csm 臂的有影面积（cov > 0.5 占比，全幅）≥ 1%
//          （闭式死态 = 恰 0%，本靶实测 47.7%~48.6% 量级 ⟹ 1% 离死态一个数量级以上）。
// 顺序 A → B，且都在三条判决线之前。**覆盖关系是实测的**：A 的触发集被 B 包含（逐位相同 ⟹
// 该臂 cov ≡ 0 ⟹ B 必响；实测把 A 废掉后「none = d5f0 副本」仍被 B 抛），而 B 的触发集 A 罩
// 不住（分母换成无关的图，图不同 ⟹ A 放行）。所以 **B 是探测器、A 是归因器**（无阈值、判图不
// 判公式，抛时直接点名分母与三种成因）。两道各自的完整理由与实跑记录住 metric 层声明处。
//
// ── 三条判决线为什么这样分（缺一条就退化成假绿）─────────────────────────────────
// 1 `shadow-seam-lod-growth`  **J1（主判据，今天必须红）**：
//    `maxRowStep(Δ|D=5) − maxRowStep(Δ|D=-1) ≤ kSeamLodGrowthCeil`。参照臂 D=-1 是**几何一致**
//    的（LOD 关 ⟹ 四级画同一份全量几何 ⟹ 相邻级对同一表面的遮挡**闭式一致** ⟹ Δ 场逐位为 0
//    ⟹ 台阶闭式 = 0）。于是这个差就是「LOD 单独造出来的那条缝」，无关项全部约掉。
//    今天读数 0.379 − 0.000 = 0.379 = 固有下界 0.057 的 6.6 倍 ⟹ **红，且这条红是正确的交付**。
// 2 `shadow-cascade-seam-arms-alive` **J2（阳性锚点，本仪器的命门）**：D=5 的 force=0 与
//    force=1 两臂**不得逐位相同**。J1 的全部力量来自「两臂唯一差别 = 采哪张阴影图」，而这句话
//    只有在两臂**真的不同**时才有内容：force 键拼错、级联只剩一级、诊断键被后面的覆写盖掉——
//    任何一种都会让两臂塌成同图、Δ 恒 0、J1 恒绿。它红时 **J1 的绿一律不作数**。
//    ⚠ 只在 D=5 上判，不在 D=-1 上判：几何一致时两臂**本就该**可能逐位相同，那是正常不是坏。
// 3 `shadow-seam-sweep-monotone` **J3（债-110 的字面要求：D 扫描入库）**：各 D 相对 D=-1 参照
//    的翻转率随 D 单调不减且高段饱和（逐段增量不增）。抓的是「漏画 / 区间错位」——那种形态
//    会让这条曲线失稳（某个 D 突然掉回 0 = 选级区间错位；某个 D 突然爆高 = 整级漏画）。
//    顺带钉一条**反向对照**：force=0 那一列必须逐位为 0（force=0 时全屏采 c0，而 c0 恒旧路径
//    ⟹ D 这个自变量**根本不被消费**）。它非 0 = 诊断键或 LOD 接线漏到了级 0。
// 4 算子自检（`selftest_seam.cpp`，无引擎毫秒级，`just tools-check` 在 gates 里）。
//
// ── TODO：两道闸门补完之后**仍然剩余**的两条静默绿路径（本批不修，主 agent 已认）─────────
// #2 **缝移出带子 = 信号丢失，四道自检全部放行**。J1 的形状是「两臂台阶之差 ≤ 上限」，带子
//    闸门钉的是「参照臂那一侧干净」，但**没有任何东西钉「缝还在带子里」**：机位 / 尺寸 /
//    级联数一变，交界行落到行 [30,870] 之外或列段之外 ⟹ stepLod 与 stepRef 一起趋 0 ⟹ J1 绿、
//    带子闸门过（参照臂台阶 0 ≤ 界）、J2 绿（原始图仍不同）、A/B 也过（阴影在场、cov 非零）。
//    这是「对空集合的断言恒真」的图像版，也是四道自检里唯一没有对应物的一道。
//    非循环的补法：把级联交界行**从引擎回显 / 闭式投影算出来**，断言它落在带子内（配置侧事实，
//    不拿图像证图像）——`just analytic` 那条闭式探针链是现成数据源。
// #3 **回显「存在」≠「被消费」**。`csm.force_cascade` 被解析并回显，但若只有 CPU 侧 `splits`
//    被覆写、着色器仍逐像素自选级 ⟹ 两臂塌成同图 ⟹ Δ ≡ 0 ⟹ J1 绿。这一条 **J2 拦得住**
//    （J2 就是为它立的），但 J2 只在 D=5 上判；若失效是 D 相关的（只有某一级之后不消费），
//    D=5 的 J2 可能仍绿而 J1 已失真。最省的补法：把「两臂不得逐位相同」在扫描的每个 D 上各出
//    一遍（现在 J3 用翻转率间接覆盖，力度弱一档——翻转率非零不等于两臂不同）。
// 另有一条不构成静默绿、但会影响跨批读数的：本仪器跑在 `rewriteTaaBlock` 的 TAA 工作点
// （feedback=0.12 / clip_mode=0）而不是交付态（0.01 / 13），两臂共模、不影响差分读数，
// 但**跨批次比读数必须认这个工作点**（每次运行开头那行 ⚠ 就是它）。
//
// ── 为什么没有 flag ────────────────────────────────────────────────────────────
// 机位、尺寸、帧号、带子、阈值都是量具的组成部分：能调 = 能调绿（同 shadow-softness /
// shadow-kernel-scale / shadow-grain）。本命令零 flag ⟹ 进 main.cpp 的 kMeta 查表。
//
// ── 变异红线（哪个变异让我红；实跑读数见交付说明）──────────────────────────────────
//   · 灵魂变异 = `render_list_mdi.cppm:284` 的 `c >= 1u` 改成 `c >= 0u`（级 0 也走簇路径）
//     ⟹ 两臂几何重新一致 ⟹ **J1 转绿**（缝被消掉），这是本判据的方向自检。
//   · 把 `shadow_lod` 出厂值改成 -1（不改代码）⟹ D=5 臂静默变几何一致 ⟹ **J1 转绿**，
//     而 J2 仍绿（force 键还活着）——**这就是为什么 J1 的绿必须配一条「D 真的生效」的对账**：
//     本仪器对「LOD 被关掉」不给红，它守的是「LOD 开着时不许造缝」。改出厂值的人必须自己
//     回到 `imgqa check` 读这句话（容差理由里同样写着）。
//   · `csm.force_cascade` 键名拼错 ⟹ 引擎按未知键 warn、差集行缺席 ⟹ requireArmEcho 抛。
//   · `meshlet.shadow_lod` 键名拼错 ⟹ 同上，`min(D=` 回显缺席 ⟹ requireArmEcho 抛。
//   · 把簇产物改名（V5）⟹ 簇路径闸门抛 rc=3 并点名「簇路径没生效」，**不出三条绿**。
//   · 删掉簇路径闸门（M7，上一批实测）⟹ **不是**假绿：第五臂（d5f0）的引擎回显对账独立接住
//     （`min(D=5,` 缺席 ⟹ 仍 rc=3）。闸门的不可替代处在两点：① 首臂即抛且给出可执行修法
//     （`meshlet_pack`），② 覆盖 `none` 臂与 D=-1 臂——那几臂的回显**证明不了**簇路径
//     （`none` 臂明确跳过 `min(D=` 检查）。写文档别把它写成"唯一防线"，那是错的。
//   · `maxRowStep` 恒返回 0 ⟹ selftest 台阶四条红 **且 J1 变绿**（J2/闸门拦不住算子失效，
//     所以算子自检必须进门禁——`just tools-check` 覆盖的就是这一层）。
//   · **参照臂闸门**的比较 `>` 反向成 `<=`（= 永不抛）⟹ selftest 两条红 **且 W2 第一条失效**
//     （全幅带子下不再抛、直接吐 J1 绿）。界换成 1e9 ⟹ selftest「污染读数应抛」红。闸门调用
//     删掉或挪到判决线之后 ⟹ W2 第一条同样失效（判决线已经打完了）。
//   · `flipFraction` 的 `> thresh` 弱化成 `>= 0` ⟹ selftest 红；实机上 J3 的 force=0 反向
//     对照列会给 1.0 ⟹ J3 红。
//
// ── 跑起来该看到什么 ───────────────────────────────────────────────────────────
// 13 臂各一次引擎进程（串行，单实例纪律）。现状（2026-10-19 本机 RADV STRIX_HALO，出厂
// `meshlet.shadow_lod = 5` + `csm.slope_bias = 0`）：**闸门过（参照臂台阶 0.000 ≤ 0.057）
// ⟹ J1 红 / J2 绿 / J3 绿**，四个必须复现主 agent 的读数 = c0↔c1 翻转率 D=5 1.304% /
// D=-1 0.534%，Δ 台阶 D=5 0.379 @ 行 472 / D=-1 精确 0.000（连跑三次逐位一致）。
// ⚠ 翻转率与主 agent 一次性脚本的 1.290% / 0.523% 差 +0.014pp / +0.011pp，**不是噪声也不是
// 采到不同图**（两边 PNG 的 md5 逐位相同）：本仪器的 cov 是「逐通道 clip → 再取均值」（与
// shadow-grain 同口径），脚本是「先均值再 clip」，两者只在某通道变亮的像素上分家。推导与
// 不许改回去的理由住 `shadow_seam_metric.cpp` 的 seamCovField。
// 修法（债-139 定案 = 交界带内两级混合）落地后 J1 该转绿，届时**不许**顺手
// 把上限从 0.1 往下调——那条上限的出处是纹素推导，不是今天的读数。
#include "imgqa.h"
#include "cluster_path_gate.h"  // 簇路径生效闸门 + readEngineLog（与 shadow-caster-survival 共用，单份真相）
#include "shadow_cascade_seam.h"
#include "shadow_seam_metric.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <array>
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
/// `lod`/`force` 的负值语义见 `shadow_cascade_seam.h`；`wantLod` 空 = 本臂不覆写 shadow_lod
/// （只有覆盖度分母臂 `none` 如此：mode=none 时阴影段零 draw，D 这个自变量不被消费）。
struct SeamArm {
    std::string name;       ///< 打印与 PNG 文件名用（唯一）
    std::string wantMode;   ///< 路线行必须含 `shadow: mode=<wantMode>`
    int lod = kSeamRefLodArm;      ///< 覆写 runtime.lua meshlet.shadow_lod
    int force = kSeamForceLow;     ///< 覆写 shadow.lua csm.force_cascade
    bool overrideLod = true;       ///< false = 不写 shadow_lod 键
    bool overrideForce = true;     ///< false = 不写 force_cascade 键
};

/// 臂表（顺序 = 采集序 = 下面 covByArm 的下标）。
/// 🔴 每臂都**同时**覆写 `csm.force_cascade` 与 `meshlet.shadow_lod` 两枚键：只钉级不选 D，
/// 量的是「现行 LOD 下两级的分歧」；只选 D 不钉级，量的是「混合选级下的画面」——两个自变量
/// 必须各自钉住，Δ 场才只由「采哪张阴影图」这一个变量决定（差分仪器的成立条件）。
const std::vector<SeamArm>& seamArms()
{
    static const std::vector<SeamArm> kArms = [] {
        std::vector<SeamArm> a;
        // 覆盖度分母：mode=none（与 LOD 无关，只跑一次）。
        a.push_back({"none", "none", 0, 0, false, false});
        // J1 的四臂：D ∈ {5, -1} × force ∈ {0, 1}。
        for (const int d : { kSeamLodArm, kSeamRefLodArm })
            for (const int f : { kSeamForceLow, kSeamForceHigh })
                a.push_back({"d" + std::to_string(d) + "f" + std::to_string(f), "csm", d, f});
        // J3 的扫描臂：D ∈ {0, 3, 7, 11} × force ∈ {0, 1}（D=5 复用上面那对，省两次引擎）。
        for (const int d : kSeamSweepD)
            for (const int f : { kSeamForceLow, kSeamForceHigh })
                a.push_back({"d" + std::to_string(d) + "f" + std::to_string(f), "csm", d, f});
        return a;
    }();
    return kArms;
}

/// 臂名 → 下标（臂表里名字唯一，重复即抛——两臂同名 = 后一臂的 PNG 盖掉前一臂，静默换臂）。
size_t armIndex(const std::string& name)
{
    const auto& arms = seamArms();
    for (size_t i = 0; i < arms.size(); ++i)
        if (arms[i].name == name)
            return i;
    throw std::runtime_error("内部错误：臂表里没有臂 `" + name + "`（判据取数取到了不存在的臂）");
}

// readEngineLog / requireClusterPathLive 已搬进 `cluster_path_gate.{h,cpp}`（本仪器与
// shadow-caster-survival 共用一份闸门逻辑——needle 名单是「读准确文案才写得对」的东西，
// 抄两份 = 第二份真相，漂一份就有一台仪器静默失效）。调用点行为逐字不变。

/**
 * 引擎回显对账（照 `shadow_grain.cpp::requireShadowEcho` 的形态，缺行即抛）。三查：
 *  ① 路线行 `shadow: mode=…` ⟹ 证明 shadow.lua 这份覆写**真的落到了引擎**；
 *  ② 差集行 `csm.force_cascade=<本臂的值>` ⟹ 证明诊断键被解析器读到（键拼错 = 未知键 warn
 *     + 差集行缺席 = 静默拿出厂值 -1 = 全屏按现行选级跑 = 整条判据作废）；
 *  ③ shadow_lod 的生效证据：D ≥ 0 查 `min(D=<D>,`（`main_meshlet_tables.cppm` 的「阴影远级联
 *     LOD 生效」INFO，**带值**）；D = -1 查「`min(D=` 缺席 + `已加载: prims=` 在场」——
 *     缺席本身是弱证据，配上「产物已加载」才只剩一种解释（LOD 关），配上「本臂就该是参照臂」
 *     这个语义才是我们要的强证据。⚠ 参照臂**不能**查 `shadow_lod=-1` 字面量：引擎在 D<0 时
 *     整条装配路径早退、一行日志都不打（门关 `grep -ci meshlet = 0` 的护栏），查它 = 必抛。
 */
void requireArmEcho(const SeamArm& a, const std::string& logText)
{
    const std::string want = "shadow: mode=" + a.wantMode;
    if (logText.find(want) == std::string::npos)
        throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），全部读数作废，见 build/rgl/imgqa_run.log");
    if (a.overrideForce) {
        const std::string f = "csm.force_cascade=" + std::to_string(a.force);
        if (logText.find(f) == std::string::npos)
            throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + f
                + "` ⟹ csm.force_cascade 键没被解析器读到（拼错键 = 静默拿出厂值 -1 = 全屏按"
                  "现行选级跑），差分仪器名不副实，见 build/rgl/imgqa_run.log");
    }
    if (!a.overrideLod)
        return;  // 覆盖度分母臂：mode=none 下 D 不被消费，没有 shadow_lod 证据可查
    if (a.lod >= 0) {
        const std::string e = "min(D=" + std::to_string(a.lod) + ",";
        if (logText.find(e) == std::string::npos)
            throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + e
                + "` ⟹ meshlet.shadow_lod 没生效为 " + std::to_string(a.lod)
                + "（拼错键 / 簇路径没生效 / 选级表全零任一），LOD 臂不是 LOD 臂，见 "
                  "build/rgl/imgqa_run.log");
    } else if (logText.find("min(D=") != std::string::npos) {
        throw std::runtime_error("臂 " + a.name + "（参照臂 D=-1）的引擎回显里**出现了** "
            "`min(D=` ⟹ shadow_lod 没被写成 -1（覆写打偏或被后面的门盖掉），几何一致参照臂"
            "其实带着 LOD，J1 的减法失去参照，见 build/rgl/imgqa_run.log");
    }
}

/// 采一臂：交付配置 + 两枚键覆写 + 机位从第 1 帧生效 + llm_shot 钉帧 → 引擎跑到自然退出。
/// 每臂重建目录：臂与臂之间不共享可变配置（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const SeamArm& a, const std::string& profileDir)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线，见头注「为什么必须走交付配置」
    o.size = { kSeamWidth, kSeamHeight };
    o.overrideShadowRoute = true;                      // 第 4 例外门：钉 mode（izb 一并强制关）
    o.shadowRouteMode = a.wantMode;
    o.overrideShadowLod = a.overrideLod;               // 第 6 例外门
    o.shadowLodValue = a.lod;
    o.overrideShadowForceCascade = a.overrideForce;    // 第 7 例外门
    o.shadowForceCascadeValue = a.force;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（RPC 补写会污染 TAA 历史）
    o.cameraPosition = { kSeamCamPosition[0], kSeamCamPosition[1], kSeamCamPosition[2] };
    o.cameraTarget = { kSeamCamTarget[0], kSeamCamTarget[1], kSeamCamTarget[2] };
    o.cameraFov = kSeamCamFov;
    o.overrideLlmShot = true;  // 第 5 例外门：帧号是量具的一部分（/shot 落在哪帧 = 墙钟）
    o.llmShotFile = "screenshots/imgqa_shadow_cascade_seam_" + a.name + ".png";
    o.llmShotDelayFrames = kSeamDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    const std::string logText = readEngineLog();
    requireClusterPathLive("shadow-cascade-seam", a.name, logText);  // 🔴 先过闸门：它一失效，后面全部读数无意义
    requireArmEcho(a, logText);
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error("臂 " + a.name + " 没出图: " + png);
    return readPngBgr(png);
}

/// 差分场 Δ = covA − covB（逐像素，线性域标量场；两图已由 seamCovField 保证同尺寸）。
cv::Mat minusField(const cv::Mat& a, const cv::Mat& b)
{
    cv::Mat out;
    cv::subtract(a, b, out);
    return out;
}

/// 臂名（`d<D>f<force>`）→ 下标（与臂表同一套拼写，写错即抛，不会静默取到别的臂）。
std::string armName(int d, int force)
{
    return "d" + std::to_string(d) + "f" + std::to_string(force);
}

/**
 * 每条 csm 臂的覆盖度场（下标与臂表一致；`[0]` 是分母臂自身，留空）。
 * on = 本臂（钉级后的 csm），off = none 臂（与 LOD 无关的公共分母）。
 * 🔴 分母 `img[0]` 是这台仪器的**承重墙**：它一失效 ⟹ 每一臂 cov ≡ 0 ⟹ 所有差分判据一起绿，
 * 所以算完立刻过 `requireShadowSignalPresent` 的两道闸门，再谈读数。
 */
std::vector<cv::Mat> collectCovFields(const std::vector<cv::Mat>& img)
{
    std::vector<cv::Mat> cov(img.size());
    for (size_t i = 1; i < img.size(); ++i)
        cov[i] = seamCovField(img[i], img[0]);
    return cov;
}

/**
 * 🔴 两道「阴影信号在场」闸门（抛 = rc 3，一条 Verdict 都不产）。
 *
 * **顺序 = A 先于 B，且都在三条判决线之前**：A 判的是「cov 这个量**有没有定义**」（分母与
 * 分子是不是同一张图），A 不过时 cov 场整个没意义，B 的读数无从解释；B 判的才是「定义了但
 * 值是不是零」。反过来排会在"分母就是分子"这种形态下先出一句"覆盖度为零"，把归因带去查
 * 场景有没有影，而真因是分母接线。
 *
 * 两道都必须过，但**探测力不是对称的**（实测，别照抄直觉）：A 的触发集被 B 包含——「某臂与
 * 分母逐位相同」⟹ 该臂 cov ≡ 0 ⟹ B 必响（实测：X3 那种「none 臂 = d5f0 副本」的形态，把 A
 * 整个废掉后 B 照样抛 rc=3）。反过来 B 的触发集 A 罩不住（X2：分母换成分子自身，图全没变，
 * A 放行、只有 B 拦得住）。所以：
 *   · **B 是探测器**（唯一能看见「图不同但差异不在阴影上」的那道）；
 *   · **A 是归因器**（无阈值、判图不判公式，抛的时候直接点名分母与三种成因；B 只会说
 *     「阴影没进画面」，会把人带去查场景有没有影）。
 * A 先于 B 跑，正是为了让 X3 那类形态拿到 A 的具体归因，而不是 B 的笼统归因。
 * 2026-10-19 实测：把 `seamCovField(img[i], img[0])` 的分母换成分子自身 ⟹ 三条判决线全绿
 * rc=0，带子闸门也放行（参照臂台阶 = 0 ≤ 界）——这就是本函数存在的理由。
 */
void requireShadowSignalPresent(const std::vector<cv::Mat>& img, const std::vector<cv::Mat>& cov,
    const std::vector<SeamArm>& arms, const cv::Rect& full)
{
    std::vector<std::pair<std::string, std::int64_t>> diffByArm;
    std::vector<std::pair<std::string, double>> fractionByArm;
    for (size_t i = 1; i < img.size(); ++i) {
        diffByArm.emplace_back(arms[i].name, cv::countNonZero(anyChannelDiff(img[0], img[i])));
        fractionByArm.emplace_back(arms[i].name, coverageFraction(cov[i], full));
    }
    // 先出数再拦：闸门抛的时候这两串数就是第一手归因材料（同 collectReadout 的打印纪律）。
    const auto minDiff = std::min_element(diffByArm.begin(), diffByArm.end(),
                             [](const auto& a, const auto& b) { return a.second < b.second; });
    const auto minFrac
        = std::min_element(fractionByArm.begin(), fractionByArm.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    std::printf("  阴影信号在场：none↔csm 逐位不同像素最少 %lld（臂 %s）；有影面积最小 %s（臂 %s，"
                "下界 %s）\n",
        static_cast<long long>(minDiff->second), minDiff->first.c_str(),
        seamPct3(minFrac->second).c_str(), minFrac->first.c_str(),
        seamPct3(kSeamCoverageFloor).c_str());
    requireDenominatorAlive(diffByArm);    // 闸门 A
    requireCoveragePresent(fractionByArm, kSeamCoverageFloor);  // 闸门 B（A 过了才有意义）
}

/**
 * 三条判决线的读数表（从 cmdShadowCascadeSeam 拆出，守认知复杂度阈值；打印与语义一字未动）。
 * 覆盖度场由 `collectCovFields` 先算好传进来（本函数不再碰分母接线），且必须先过
 * `requireShadowSignalPresent` 的两道闸门。
 * 打印分三段（J1 级对 / J2 锚点 / J3 扫描），每段先出数再进判据——出判据前人眼就能看到
 * 「参照臂是不是 0」这类仪器自检读数（判据红时的第一手归因材料）。
 */
SeamReadout collectReadout(const std::vector<cv::Mat>& img, const std::vector<cv::Mat>& cov,
    const cv::Rect& band, const cv::Rect& full)
{
    const auto covAt = [&](const std::string& name) -> const cv::Mat& {
        return cov[armIndex(name)];
    };

    SeamReadout r;
    // ── J1：Δ(D) = cov(D, force=0) − cov(D, force=1) 的最硬横向台阶 ────────────────
    r.stepLod = maxRowStep(minusField(covAt(armName(kSeamLodArm, kSeamForceLow)),
                           covAt(armName(kSeamLodArm, kSeamForceHigh))), band);
    r.stepRef = maxRowStep(minusField(covAt(armName(kSeamRefLodArm, kSeamForceLow)),
                           covAt(armName(kSeamRefLodArm, kSeamForceHigh))), band);
    r.flipLod = flipFraction(covAt(armName(kSeamLodArm, kSeamForceLow)),
        covAt(armName(kSeamLodArm, kSeamForceHigh)), full);
    r.flipRef = flipFraction(covAt(armName(kSeamRefLodArm, kSeamForceLow)),
        covAt(armName(kSeamRefLodArm, kSeamForceHigh)), full);
    std::printf("\n  级对 c0↔c1（主 agent 指纹表的那四个数）：\n");
    std::printf("    D=%-2d 翻转率 %s   Δ 最硬横向台阶 %s @ 行 %d   ← LOD 臂\n", kSeamLodArm,
        seamPct3(r.flipLod).c_str(), seamFixed3(r.stepLod.step).c_str(), r.stepLod.row);
    std::printf("    D=%-2d 翻转率 %s   Δ 最硬横向台阶 %s @ 行 %d   ← 几何一致参照臂\n",
        kSeamRefLodArm, seamPct3(r.flipRef).c_str(), seamFixed3(r.stepRef.step).c_str(),
        r.stepRef.row);

    // ── J2：D=5 的两 force 臂不得逐位相同（阳性锚点）──────────────────────────────
    r.diffPxLod = cv::countNonZero(anyChannelDiff(
        img[armIndex(armName(kSeamLodArm, kSeamForceLow))],
        img[armIndex(armName(kSeamLodArm, kSeamForceHigh))]));
    std::printf("  阳性锚点：D=%d 的 force0 vs force1 逐位不同的像素数 = %lld（须 > 0）\n",
        kSeamLodArm, static_cast<long long>(r.diffPxLod));

    // ── J3：D 扫描（含 D=5，按 D 升序去重）────────────────────────────────────────
    std::vector<int> ds;
    for (const int d : kSeamSweepD)
        ds.push_back(d);
    ds.push_back(kSeamLodArm);
    std::sort(ds.begin(), ds.end());
    ds.erase(std::unique(ds.begin(), ds.end()), ds.end());
    for (const int d : ds) {
        SeamSweepPoint p;
        p.lod = d;
        p.flipForce1 = flipFraction(covAt(armName(d, kSeamForceHigh)),
            covAt(armName(kSeamRefLodArm, kSeamForceHigh)), full);
        p.flipForce0 = flipFraction(covAt(armName(d, kSeamForceLow)),
            covAt(armName(kSeamRefLodArm, kSeamForceLow)), full);
        r.sweep.push_back(p);
    }
    std::printf("  D 扫描（相对 D=%d 参照的翻转率，全幅）：\n    ", kSeamRefLodArm);
    for (const auto& p : r.sweep)
        std::printf("D=%-2d force1 %s / force0 %s   ", p.lod, seamPct3(p.flipForce1).c_str(),
            seamPct3(p.flipForce0).c_str());
    std::printf("\n    （force0 列必须逐位 0：force=0 时全屏采 c0，而 c0 恒旧路径 ⟹ D 不被消费）\n");
    return r;
}

}  // namespace

int cmdShadowCascadeSeam()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_cascade_seam_profile";
    std::printf("shadow-cascade-seam —— 相邻级联对阴影的分歧：**远级簇 LOD 不许造出缝**"
                "（债-110 / 债-104 / 债-139）\n");
    std::printf("  仪器 = 强制选级差分（csm.force_cascade）；单张屏幕空间统计量已被实测证伪"
                "（对 LOD 是瞎的），理由全文住本文件头注\n");
    std::printf("  配置 = 交付配置仓库 configs/（合成 profile 无簇产物 ⟹ LOD 不生效 ⟹ 恒绿）；"
                "帧号 = 第 %d 帧；采集 %dx%d\n", kSeamDelayFrames, kSeamWidth, kSeamHeight);
    std::printf("  机位 = 债-139 第三机位 pos(%.6f,%.6f,%.6f) fov %.1f\n", kSeamCamPosition[0],
        kSeamCamPosition[1], kSeamCamPosition[2], kSeamCamFov);

    const std::vector<SeamArm>& arms = seamArms();
    std::vector<cv::Mat> img(arms.size());
    for (size_t i = 0; i < arms.size(); ++i) {
        img[i] = captureArm(arms[i], profile);
        std::printf("  ✓ 臂 %-7s %dx%d  D=%-2d force=%d\n", arms[i].name.c_str(), img[i].cols,
            img[i].rows, arms[i].lod, arms[i].force);
    }
    if (img[0].cols != kSeamWidth || img[0].rows != kSeamHeight)
        throw std::runtime_error("采集尺寸是 " + std::to_string(img[0].cols) + "x"
            + std::to_string(img[0].rows) + "，不是量具钉死的 " + std::to_string(kSeamWidth)
            + "x" + std::to_string(kSeamHeight) + "（llm_shot.size 没生效 = 带子与台阶全错位）");

    const cv::Rect band(kSeamBandX, kSeamBandRow0, kSeamBandCols, kSeamBandRows);
    const cv::Rect full(0, 0, img[0].cols, img[0].rows);
    // 带子占比**从常数算出来**，不硬写「中央 1/2 幅」——那行字与 kSeamBandCols 是两份真相，
    // 改带子做实验时它会静默变成一句谎话（本批 W2 就要临时改带子）。
    std::printf("  台阶带 = 列 [%d,%d) × 行 [%d,%d]（占幅 %d/%d）；翻转率带 = 全幅\n", band.x,
        band.x + band.width, band.y, band.y + band.height - 1, kSeamBandCols, kSeamWidth);

    // 🔴 覆盖度场先算，再过两道「阴影信号在场」闸门（A 分母在场 → B 覆盖度非零），**都在三条
    // 判决线之前**：这两道判的是「cov 这个量有没有定义 / 是不是恒零」，不过时下面三条判决线
    // 的绿全是假的（Δ ≡ 0 ⟹ J1 绿、全 0 ⟹ J3 绿、J2 不吃分母照样绿）。
    const std::vector<cv::Mat> cov = collectCovFields(img);
    requireShadowSignalPresent(img, cov, arms, full);

    const SeamReadout r = collectReadout(img, cov, band, full);

    // 🔴 **闸门先于三条判决线**（顺序就是语义）：参照臂那一侧本应是**常数**（闭式 0，最多一个
    // 纹素），J1 的「两臂台阶之差」才是 LOD 的归因量。带子一旦被非纹素项污染（首要嫌疑 =
    // `shadow.slang` ④ 出块旁路在两臂里都造台阶，而 maxRowStep 取逐行最大 ⟹ 最大项被污染行
    // 抢走后缝的信号整个消失），"差"就不再是归因量——此时出绿会被读成「LOD 没造缝」、出红会
    // 被读成「LOD 造了缝」，**两个方向都是假线索**。所以闸门没过就一个 Verdict 都不产（rc=3
    // = 今天没出结论），而不是把不可信的读数交给判据。实测两格假绿（x∈[160,1440) 与
    // x∈[200,1400)）的完整表住本文件头注。
    requireBandValid(r.stepRef.step, r.stepRef.row, band, kSeamTexelFloor);

    std::vector<Verdict> vs;
    pushLodGrowthVerdict(vs, r);
    pushArmsAliveVerdict(vs, r);
    pushSweepVerdict(vs, r);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    bool aliveOk = true;
    for (const auto& v : vs)
        if (v.name == "shadow-cascade-seam-arms-alive")
            aliveOk = v.ok;
    if (!aliveOk)
        std::printf("\n  ⚠ J2 红：J1 的绿**不作数**（两臂塌成同图 = 差分仪器没接线，"
                    "Δ 恒 0 会让 J1 恒绿）\n");
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("shadow-cascade-seam",
        {{"camera", "d139-third"}, {"size", std::to_string(kSeamWidth) + "x" + std::to_string(kSeamHeight)},
            {"delay_frames", std::to_string(kSeamDelayFrames)},
            {"band", std::to_string(kSeamBandX) + "," + std::to_string(kSeamBandRow0) + " "
                + std::to_string(kSeamBandCols) + "x" + std::to_string(kSeamBandRows)},
            {"arms", std::to_string(arms.size())}, {"lod_arm", std::to_string(kSeamLodArm)},
            {"ref_lod_arm", std::to_string(kSeamRefLodArm)}},
        vs);
    bool ok = true;
    for (const auto& v : vs)
        ok = ok && v.ok;
    std::printf("\n%s\n", ok ? "全部判据绿"
                             : "有判据红（正文点名先查哪一层）。⚠ J1 红 = LOD 在造缝，"
                               "**这是本批交付的内容，不是判据坏了**");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
