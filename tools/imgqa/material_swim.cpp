// material-swim —— **材质场世界不变性**（债-152 的守卫；相对坐标批 5 第 5 刀）。
//
// ## 缺陷本体（钉到行；**已修**，下面是修前的形态）
//
// 修前 `rgl/shaders/deferred_geometry.frag.slang` 的 `float2 q = P.xz * freq;`（现 :166，
// 改成 `q = (P.xz + ws.zw) * freq`）——水洼掩码 = 低频值噪声（世界 xz），而 `P` 在 rebase
// 之后是**相对渲染原点**的坐标，原点每帧 snap 到级联纹素粒度并跟着相机走 ⟹ **噪声场整片随
// 相机平移**。同一个 `q` 还喂水膜法线波纹（一处病灶两个症状）。
//
// ## 为什么必须有这台仪器（判据失明第四例的补位）
//
// 批 5 的 `no-harm` 判据是「同一机位下门开 vs 门关比静态图」。而一个会滑的图案**在每一帧
// 单独看都是合理的湿面图案**——斑块只是换了位置，静态对比只看到很小的差。⟹ **静态单姿态
// 对比对「只在运动中出现」的缺陷结构性不可见**。前三条失明是「归一化量饱和」「差分对常量
// 位移失明」「注入量无区分力」，这一条是**激励形态不对**：再怎么换判据算子也没用，必须换
// 激励（把相机动起来）。本仪器就是那个「动起来」的激励 + 一条能看见滑移的对拍口径。
//
// ## 激励 = 相机**平移**（旋转不暴露它）
//
// 原地转视角时相机位置不动 ⟹ 渲染原点不动 ⟹ 图案不滑。所以两枚机位必须是**同姿态、纯横移**
// （target 与 position 一起平移 ⟹ 姿态逐字相同，只有位置差 kStimulusM）。
//
// ## 对拍口径 = 地面平面上的世界空间对拍（债-139 已定死的形状）
//
// 两枚机位是本工具自己设的 ⟹ 每枚机位下「像素 → 地面 y=0 的世界 xz」是**闭式**的，不需要
// 引擎回显任何东西，也**不用 G-buffer 的 position 通道**（那个通道在门开态是相对原点的，
// 用它反而要把原点减回来 = 多一个自由度多一个错处）。比较对象 = **metalrough 通道**
// （`gbuffer_contract.cppm:48` 的第 3 通道，G = roughness），因为水洼直接调制粗糙度。
// 判据 = 两枚机位下**同一个世界点的粗糙度必须相同**（水洼场是世界 xz 的函数，与相机无关）。
//
// 口径本体（重投影 / 格坐标 / 共同格计数 / 平移搜索）住 `ground_field_metric.{h,cpp}`，与
// `swim`（影场世界不变性）**同一份**——两台仪器共用对拍口径，各自只管「这个逐像素二值量
// 怎么来」。不抄第二份 = AGENTS 哲学第 1 条。
//
// ## 靶 = `ssr_rig_empty`（零起伏平地，🔴 不是 bistro 鹅卵石地）
//
// 债-139 就是卡在「拿 bistro 鹅卵石地当平地」的测量地板上。本靶地面 = 一片 y=0 的平地 quad
// （manifest 的 aabb 给出矩形，仪器按它筛像素），材质 roughness = 0.12 ≤ `ssr.rough_full`
// = 0.15 ⟹ 湿面身份 `wet = 1 − smoothstep(0.15, 0.40, roughness)` **恰为 1** ⟹ 水洼调制满
// 生效，G-buffer 的 roughness 字节在水洼(13) 与湿石(89) 两态之间跳变（实测 min/max）。
// 无柱变体 = 画面里没有第二个物体 ⟹ 「哪些像素是地面」由几何闭式决定，不需要 objectID
// 反解（那是 swim 那边必须的，它的靶上有遮挡体）。
//
// ## 七臂（一次引擎进程一臂；单实例串行）
//
//   | 臂     | coordinate.rebase | 相机 | entity position | 湿面对比度 | 用途 |
//   |--------|-------------------|------|-----------------|-----------|------|
//   | offA   | 关                | A    | 0               | 出厂      | 门关基准 |
//   | offB   | 关                | B=A+d| 0               | 出厂      | 门关对拍（**地板**）|
//   | onA    | 开                | A    | 0               | 出厂      | 门开基准 |
//   | onB    | 开                | B=A+d| 0               | 出厂      | 门开对拍（**主判据**）|
//   | onA2   | 开                | A    | 0               | 出厂      | 同机位重跑（**阴性对照**）|
//   | offS   | 关                | B=A+d| **d**           | 出厂      | 注入已知世界偏移（**区分力守卫**）|
//   | offT   | 关                | B=A+d| 0               | **±δ**    | 注入**亚阈值**场变化（**新判据区分力守卫**）|
//
// offS 与 offB 的差别只有「整个靶沿世界 +X 挪 d，相机跟着挪 d」⟹ 屏幕取景逐字相同，而
// **图案的锚**（修法给出的场景锚）跟着靶挪了 d ⟹ 两场的对拍必须报出 ≈ d 的滑移。它守的是
// 「通过可能只是量具瞎」：一条只会读 0 的判据在这里当场红。
//
// ## 亚阈值注入臂 offT —— 本批（债-154）的核心，也是「旧绿新红」的载体
//
// **为什么需要它**：J1–J4 数的是**二值标签翻转**，而二值化把「场动了」投影成「有没有跨过
// 阈值」。一个把粗糙度对比度改了、但让每一格仍在阈值同一侧的改动，在翻转率上读 0 = 读成
// 「比按构造不变的门关地板还干净」。这正是债-154 要修的读反。要证明新判据真的多抓东西，
// 必须造出一个**标签一张都不翻、而场确实动了**的注入，并实测旧判据全绿 / 新判据红。
//
// **注入形态 = 关于阈值的对称对比度展开**（🔴 不是「整体加偏置」）：`rough_lo −= δ` 且
// `rough_hi += δ`（第 10 枚例外门 `applyWetContrastGate`，只在这一臂开）。推导：地面
// roughness = `lerp(roughHi, roughLo, puddle)`，二值阈值 = 全臂字节 (min+max)/2 = **两端点
// 中点**，而线性插值的中点恰落在 `puddle = 0.5` ⟹ **标签边界的位置只由 `puddle` 决定，与两
// 端点取值无关**。两端等量反向挪 ⟹ 每一格的字节都被搬动、却一格都不搬过阈值。单端挪会把
// 中点一起搬走 ⟹ 边界带上的格翻标签 ⟹ 新旧判据都看得见，本批验收当场失效。
//
// **为什么不是「恰 0 翻转」而是 3 格**：字节域是 8 位量子，阈值两侧各 1 LSB 内的格会因舍入
// 跨过去（实测 δ=0.005/0.01/0.02 → 翻转 2/3/6 格，随 δ 线性）。这**不等于**注入不成立：
// 场是连续的（13..89 之间有 smoothstep 过渡带），连续量在过渡带上定义良好，而翻转率读到的是
// 0.01 % —— 比门关地板的 0.12 % **还低一个数量级**，即旧判据把这个缺陷读成「比按构造不变的
// 场更干净」。这就是债-154 说的读反，实测到了，不是推演。
//
// **δ = 0.01 是夹出来的，不是调出来的**：上界 = 再大就开始翻较多标签（阈值虽不动，但舍入
// 跨界数随 δ 涨）；下界 = 再小则 mean 与门关地板同量级。三档实测见下方变异声明后的表。
//
// ## 两种统计量、一套格坐标口径（🔴 分母必须相同）
//
// 连续量 = 共同格上 `|Δroughness|` 的 mean 与 p99（满量程千分点，1 LSB = 3.922 ‰）。算法不在
// 这里第二份：住 `field_diff_metric.{h,cpp}`，与 `shadow-swim-flat` 的 mean(Δcov) **同一份**
// （AGENTS 哲学第 1 条）。格坐标与共同格判定住 `ground_field_metric`，二值的 `overlapOf` 与
// 连续的 `overlapRealOf` **逐字同式**（同一批共同格）——分母一旦分家，「翻转率绿而连续量红」
// 这个结论比的是两批格，整个不成立。⟹ `compareFields` 里有一条**分母不等即抛**的守卫钉它。
//
// ## 八条判决线（挂号与容差理由 → `imgqa check`）
//
// J1 `material-swim-world-invariant`（**修前必须红**）：门开两机位的材质场翻转率 ≤ 0.5% 且
//   最优平移 ≤ 1 格。J2 `material-swim-determinism`：同机位两跑**恰为 0**。
// J3 `material-swim-gate-closed-floor`：门关两机位（按构造世界不变）读到的地板 ≤ 0.5%。
// J4 `material-swim-offset-sensitivity`（🔴 区分力守卫，**修前必须红**）：注入 d 必须报出 d。
// —— 以上四条本批**一字未动**（0.12 % 那个读数仍在原位，是回归判据）。以下四条是债-154 补的：
// J5 `material-swim-continuous-floor`：门关地板的 mean ≤ 1.0 ‰ 且 p99 ≤ 7.843 ‰（= 2 LSB）。
// J6 `material-swim-continuous-no-harm`（🔴 **主判据**）：门开两机位的 mean/p99 同界。
// J7 `material-swim-continuous-determinism`：同机位两跑的 mean/p99 **恰为 0**。
// J8 `material-swim-subthreshold-visible`（🔴 新判据的区分力守卫）：offT 必须**旧读绿、新读
//   红**（翻转率 ≤ 0.5 % 而 mean 与 p99 都越界）。它红 = 连续量判据自己失去区分力。
//
// 界的推导全部挂在实测数据上（门关地板 mean **0.272 ‰**、p99 **恰 1 LSB**；注入 mean
// **9.366 ‰**、p99 **11.765 ‰**），逐条理由 → `judgments_table_material_cont.cpp`。
//
// ## 六道闸门（全部抛 rc=3、一条 Verdict 都不产；表先出、闸门后抛）
//
// G1 靶三件套在场（loadManifest 抛）+ 地面 piece 的 aabb 退化守卫（零厚度矩形 = 筛不出像素）。
// G2 引擎回显对账：门开臂必须有 `coordinate.rebase = true`、门关臂必须缺席（「覆写落盘了但
//   引擎没按它跑」与「原点恒零」两种失效在图像上都长得像绿）。
// G3 **激励在场**：onA 与 onB 的图不得逐位相同（相机没动 = 七臂只剩六臂，而 J1 恒真）。
// G4 **场非退化**（本仪器最重要的一道）：每臂的有效地面格数 ≥ 下限、湿格占比落在 [5%,95%]、
//   roughness 字节的极差 ≥ 60 LSB。恒值场上「翻转率 0」是**恒真**——那正是本项目反复登记的
//   「对空集合的断言恒真 = 假绿」。
// G5 **共同覆盖足够**：每一对的共同格数 ≥ 较小场的 80%（只比两个几乎不相交的场 = 分母是彩票）。
// G6 **两条读数的分母对账**（债-154 新增）：`compareFields` 里二值 `overlapOf` 与连续
//   `overlapRealOf` 算出的共同格数不等即抛。它不是防御性断言——本仪器的核心结论是「同一批格上
//   翻转率绿而连续量红」，两条读数一旦吃了不同批的格，这句话立刻变成比两批格的假话。
//   实跑 M5（让连续侧每两格跳一格）就是被它当场炸掉（rc=3，一条判决线都不产）。
//
// ## 已知边界：四对判据的前提**只在渲染原点附近成立**（远点 1e6 已实测）
//
// 把整套仪器临时搬到 x0 = 1e6（相机、靶、地面矩形一起挪，其余一字不动）跑过一遍，读数：
//
//   | 对 | 近点（出厂）| 远点 1e6 |
//   |----|-------------|----------|
//   | J1 门开 onA vs onB | 33/28270 = 0.12 %、mean 0.271 ‰、p99 3.922 ‰ | 16/22978 = **0.07 %**、mean **0.253 ‰**、p99 3.922 ‰ |
//   | J3 门关 offA vs offB | 33/28270 = 0.12 %、mean 0.272 ‰、p99 3.922 ‰ | 435/22978 = **1.89 %**、mean **5.099 ‰**、p99 **47.059 ‰** |
//   | J8 亚阈值 offB vs offT | 3/30076 = 0.01 %、mean 9.366 ‰ | 0/24290 = **0.00 %**、mean **9.379 ‰**（同量级 ⟹ 注入是材质参数改动，与工作点无关）|
//
// 三条结论，逐条都有出处：
//   ① **交付路径（门开）在远点没有差异**，且与近点逐数同量级 ⟹ rebase 的修法在 1e6 上成立
//     （这一条是好消息，也是「如实报没有差异」——不是没测，是测了没有）。
//   ② **门关两机位在远点按构造就会变**：同一台仪器、同一工作点、同一 float32 口径下，门开对
//     读 0.253 ‰ 而门关对读 5.099 ‰ = **20 倍**，⟹ 多出来的不是量具地板（地板就是门开那对的读数），
//     是引擎侧真实信号：rebase 关掉后远点世界坐标被 float32 量化到 0.0625 m，水洼图案随相机走
//     = 债-152 那一类缺陷在远点的放大形态。⟹ **J3/J5 的「门关 = 按构造世界不变」这个前提只在
//     原点附近真**，远点照搬这两对必然红（红得有道理，但不是本仪器要判的事）。远点若要立常驻
//     判据，正确形态是「门关 vs 门开」的**传输对**（同 `shadow-swim-flat` 的四格），不是复制这四对。
//   ③ 口径自身在远点的代价（独立探针实测，与引擎无关）：float32 重投影在 1e6 处有 **32.5 %**
//     的像素落进与 double 域不同的 5 cm 格，单臂有效格 30482 → 24416、两臂共同格 28842 → 22978
//     （都 −20 %）。⟹ 远点判据要做常驻**必须**换 double 域重投影（那份口径已在
//     `shadow_transport_metric`，理由见它头注 ①），而 `ground_field_metric` 的 float32 路径
//     改不得（`swim` 的计数阈值依赖它）。近点（本仪器出厂工作点）无此问题：float32 与 double
//     的命中差 2e-6 m = 格的 4e-5 倍，格归属零错位。
//
// ## 变异声明（哪个变异让本仪器红，实跑读数）
//
// ### 缺陷态注入（shader 侧，债-152 那批实跑）
//
// 工装 = 备份 → 改 → `just reflect` → 建 → 跑 → 反向还原，构建带失败重试；还原后
// `reflection_manifest.txt` 与变异前**逐位相同**。本批（债-154）的红线不碰 `.slang`，故
// **没有**重跑这一条；J1–J4 四条判决线本批一字未动，重跑应复现同样的红/绿分布。
//   把 `deferred_geometry.frag.slang` 的锚定改回吃相对坐标（`q = (P.xz + ws.zw) * freq`
//   → `q = P.xz * freq`），其余一字不动。
//     · J1 **FAIL** 翻转 3279/28270 = **11.60 %**，最优平移 **(+0.50, 0.00) m 处失配恰 0**
//       （= 场是自身的精确平移，滑移量恰等于相机平移量，不是"约等于"）；
//     · J4 **FAIL** 报 **(0.00, 0.00)** 而非注入的 0.50 m（零位移处翻转 0.00 % = 量具在
//       缺陷态下会读绿，所以这条守卫必须独立存在）；
//     · J2 PASS 0/30076、J3 PASS 33/28270 = 0.12 % ——**两条防线在缺陷态下照旧绿**，
//       这正是"J1 红 = 缺陷，而不是量具坏了"的证据；
//     · 佐证：G3（onA vs onB 逐位不同像素）从修后的 307266 掉到 **603**——图案钉在屏幕上
//       不随世界走，两机位的地面粗糙度自然几乎相同；门开态湿格占比 63.3 % 而门关 40.0 %。
//   修后（同一仪器、同一构建口径）：J1 **PASS** 33/28270 = **0.12 %**、最优平移 (0,0)，
//   与 J3 **逐数相同**；J4 **PASS** 报 (+0.50, 0.00) m。
//
// ### 本批新增判据的变异（M1–M6，2026-10-26 **全部实跑**）
//
// 工装 = 一次性变异脚本（备份 → 改 → 建 → 跑 → 反向还原 → **还原后重跑基线必须全绿**；构建带
// 失败重试；脚本本身不入仓，入仓的是下面这张读数表）。"只有 J8 红"是本批想要的形态：新判据的
// 区分力守卫独立存在。
//
//   | # | 变异落点 | 实跑结果 |
//   |---|----------|----------|
//   | M1 | `overlapRealOf` 的 sumAbs 恒 0（= 连续量**看不见**） | **只有 J8 红**，J5/J6/J7 照绿（0 ≤ 界）⟹ J8 是唯一能否掉「连续量看不见」的守卫，另三条按构造拦不住它 |
//   | M2 | p99 取序下标 0.99 → 0.50（统计量定义漂移） | selftest 红 4 条；**仪器层八条全绿**（把 J8 的 p99 条件从 `> 0` 收紧成 `> 界` 后仍不红——注入把约三成格都搬动了，中位数本身就有 2 LSB）⟹ **统计量定义的变异归闭式层，不归仪器层** |
//   | M3 | mean 忘乘 1000（单位口径漂移） | selftest 红 2 条 + **J8 红** |
//   | M4 | δ = 0（注入消失） | **只有 J8 红** ⟹ J8 确实挂在注入上，不是挂在别处的噪声上 |
//   | M5 | 连续侧每两格跳一格（两条读数分母分家） | 仪器**抛 rc=3**（G6 分母守卫当场炸，一条判决线都不产） |
//   | M6 | J1 的条件改成恒真（判据本体失效） | 0 条红，**预期存活**——本项目没有「判据的判据」，这一层只剩挂号表 + 人，写清楚比假装拦住更诚实 |
//
// **δ 的三档实测**（同一仪器、只改 `kContrastBias`；翻转格数随 δ 线性 = 舍入跨界，不是标签翻得多）：
// δ=0.005 → 2 格 / mean 5.842 ‰；**δ=0.01 → 3 格 / mean 9.366 ‰ / p99 11.765 ‰**；
// δ=0.02 → 6 格 / mean 16.326 ‰。取 0.01 的理由：mean 是门关地板（0.272 ‰）的 **34 倍**，
// 而翻转率 0.01 % 比门关地板（0.12 %）**还低一个数量级**——旧判据把它读成「比按构造不变的场
// 更干净」，正是债-154 说的读反。
//
// **写得出但没跑的变异**（按可信度排序，红的那条判据在括号里）：
//   · `fillWetParams` 的 origin/anchor 传成两个 0（CPU 侧同形变异，J1/J4）；
//   · 偏移符号写反（`anchor − origin`，J1：滑移变成 2× 相机位移，翻转率更高）；
//   · 减法先在 float 里做（远工作点上偏移塌成 0，出厂工作点**不红**——本仪器机位在原点
//     附近，抓不到它；那条由 `tests/test_wet_params.cppm` 的阳性对照钉，不由本仪器钉）；
//   · 阈值改成各臂自算（J3 与 J1 口径分叉，地板失去意义）；
//   · 重投影漏乘 aspect（J3 门关地板跟着红 = 由 J3 拦下，不是让 J1 假绿）；
//   · 阈值池**不**排除 offT（实测阈值 51 → 52，J8 的翻转率读数跟着漂；本批靠调用点注释钉，
//     没跑成变异——它是「注入臂会不会污染其余六臂口径」这条约束的唯一防线）。
//
// 🔴 零 flag 是设计：靶 / 机位 / 平移量 / 尺寸 / 格边长 / 阈值都是量具的组成部分，能调 =
// 能调绿（同 shadow-edge-ruler / shadow-edge-rebase）。
#include "imgqa.h"
#include "cluster_path_gate.h"    // readEngineLog（与交付配置上的两台阴影仪器共用）
#include "field_diff_metric.h"    // meanAbsPermille / p99Abs（连续量口径，与 shadow-swim-flat 同一份）
#include "ground_field_metric.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

// ── 量具常数（改动 = 改判据）──────────────────────────────────────────────────────────
/// 靶（tools/qa_scenes.py 的 SCENES 键；无柱变体 = 画面里只有平地）。
constexpr const char* kRigStem = "ssr_rig_empty";
/// 地面 piece 名（manifest 里给 aabb，仪器按它筛像素；不在 C++ 里重抄几何）。
constexpr const char* kGroundPiece = "ground_metal";
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
/// 竖直 fov（度）。机位高度与俯角一起决定「哪些像素落在地面上」。
constexpr double kFovDegrees = 45.0;
constexpr double kCameraHeightM = 1.6;
/// 机位 A 的 position / target（target 在 A 的前下方 ⟹ 俯角 ≈ 25°，整幅都是地面）。
constexpr double kCamAx = 0.0, kCamAz = 4.0, kCamTz = -6.0;
/// 🔴 激励 = 相机沿世界 +X 平移这个量（旋转不暴露缺陷，见文件头）。
constexpr double kStimulusM = 0.5;
/// 世界格边长（米）——与 swim 同刻度；水洼噪声特征尺寸 = 2·sceneRadius/puddleFreqRadii
/// ≈ 0.94 m，即一格 = 噪声特征尺寸的 1/19，滑移不会被格边长吃掉。
constexpr double kCellM = 0.05;
/// 交点距离上限（米）：超出即掠射区，一像素跨数米、格内票数噪声主导，不进判据。
constexpr double kMaxDistanceM = 12.0;
/// 每格至少这么多样本才要（近场一像素跨 1–6 cm，4 票 ≈ 覆盖住一格的采样抖动）。
constexpr int kMinVotes = 4;
/// 平移搜索半径（格）= ±1 m = 激励量的 2 倍（守卫要报出 d，搜索窗必须容得下它）。
constexpr int kSearchRadius = 20;
/// 一臂的有效地面格数下限（低于它 = 靶没装对 / 机位没看到地面，全部读数作废）。
constexpr int kMinCells = 2000;

/// 湿面对比度偏置 δ（粗糙度线性域）。🔴 这个数的选取**不是**调参，是被两条实测夹出来的：
/// 上界 = 单端挪到多少会**开始翻标签**（翻了就说明阈值被一起挪走，注入不再是「亚阈值」形态）；
/// 下界 = 对称展开到多少，连续量 mean(Δ) 才**明显高出门关地板**（太小就是量具精度）。
/// 实测读数与推导 → 文件头「亚阈值注入臂 offT」一节 + `judgments_table_material.cpp` 的 J8 容差理由。
constexpr double kContrastBias = 0.01;

/// 连续量 mean(|Δroughness|) 的上界（满量程千分点 = 0.001 粗糙度满量程）。
/// 🔴 界的推导挂在实测数据上（本机 2026-10-26，完整理由 → `judgments_table_material.cpp`）：
///   门关地板（offA vs offB，按构造世界不变）实测 **0.272 ‰** ⟹ 取 **1.0 ‰** ≈ 地板的 3.7 倍
///   ≈ **0.255 LSB**（粗糙度字节域）；而亚阈值注入臂实测 9.366 ‰ ⟹ 界两侧各留 3.7× / 9.4× 余量。
constexpr double kMeanTolPermilleValue = 1.0;
/// 连续量 p99(|Δroughness|) 的上界。🔴 **1 LSB = 1/255 = 3.922 ‰**，而门关地板实测的 p99
/// **恰为 1 LSB**（3.922 ‰，不是近似——字节域里第 99 百分位就是一个整数量子）⟹ 界取 **2 LSB
/// = 7.843 ‰**，即「尾部最多再差一个量子的舍入」；亚阈值注入实测 11.765 ‰ = 3 LSB，越界。
constexpr double kP99TolPermilleValue = 7.843;

/// 一臂 = 一次引擎进程 + 一张 metalrough 直读图 + 引擎回显里**必须**出现/缺席的证据。
struct Arm {
    std::string name;
    bool rebase = false;   ///< coordinate.rebase
    double camShift = 0.0; ///< 相机沿 +X 的平移量（激励）
    double entityShift = 0.0;  ///< 靶沿 +X 的平移量（区分力守卫的注入量）
    /// 🔴 湿面**对比度**偏置 δ（债-154 的亚阈值注入臂专用，0 = 出厂值 = 门闭）。
    ///
    /// 为什么这一枚注入是「标签一张都不翻、而场确实动了」的**构造性**形态（不是碰巧翻得少）：
    /// 地面的 roughness = `lerp(roughHi, roughLo, puddle)`（`wet` 身份与 `strength` 都是 1），
    /// 二值阈值取全臂观测字节的 (min+max)/2 = **两端点的中点**，而线性插值的中点恰落在
    /// `puddle = 0.5` 上 ⟹ **标签边界的位置只由 `puddle` 决定，与两端点的取值无关**。
    /// 把两端**等量反向**挪（`rough_lo −= δ`、`rough_hi += δ`）因此把每一格的字节都搬动，
    /// 却一格都不搬过阈值。⚠ 单端挪不成立——那会连中点一起挪走，边界带上的格就翻标签，
    /// 注入退化成「新旧判据都看得见」，本批的核心验收当场失效（推导全文 →
    /// `engine_profile_gates.cpp` 第 10 枚门头注）。
    /// ⚠ 阈值池必须排除本臂（见 `cmdMaterialSwim` 里的 `thr` 计算）：本臂的 max 是 92 而不是 89，
    ///   拿它进池会把阈值从 51 抬到 52 = 改掉其余六臂的判据（实测 0.12 % 那个数就会漂）。
    double wetContrastBias = 0.0;
};

const std::vector<Arm>& arms()
{
    static const std::vector<Arm> kArms = {
        { "offA", false, 0.0, 0.0 },        { "offB", false, kStimulusM, 0.0 },
        { "onA", true, 0.0, 0.0 },          { "onB", true, kStimulusM, 0.0 },
        { "onA2", true, 0.0, 0.0 },         { "offS", false, kStimulusM, kStimulusM },
        // 债-154：与 offB 只差湿面对比度（相机、靶、门态逐字相同）⟹ 取景相同、图案不滑、
        // 标签不翻，**只有场的数值**动了。旧判据对它恒绿，新判据必须红。
        { "offT", false, kStimulusM, 0.0, kContrastBias },
    };
    return kArms;
}

/// 两位小数（读数文案的统一精度）。⚠ 不复用 `temporal_swim.h` 的 `d2`：那个头自己写着
/// 「本头**不是** taa-swim 的对外接口……别的 TU 别接」，跨仪器接它就是把两台仪器的
/// 打印口径焊在一起。
std::string f2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

/// 三位小数（连续量 ‰ 读数的精度，与 `shadow-swim-flat` 的 `f3` 同口径——两台仪器的数要能并排读）。
std::string f3(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.3f", v);
    return b;
}

/// lua 表里的浮点字面量：17 位有效数字保证 double 逐位往返（少一位 = 引擎读到的不是我们
/// 算出的那个数，而「相机平移了多少」正是判据的自变量）。
std::string luaNum(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

/// 本臂装进 scene.lua 的 entity 文本（区分力守卫臂把靶挪 d；其余臂不写 position 键）。
std::string entitiesLua(const Arm& a)
{
    return "{ type = \"model\", file = \"models/" + std::string(kRigStem) + ".glb\""
        + (a.entityShift != 0.0 ? ", position = { " + luaNum(a.entityShift) + ", 0.0, 0.0 }"
                                : "")
        + " }";
}

Engine::Camera cameraOf(const Arm& a)
{
    return Engine::Camera { { kCamAx + a.camShift, kCameraHeightM, kCamAz },
        { kCamAx + a.camShift, 0.0, kCamTz }, kFovDegrees };
}

/// G2：引擎回显对账。门开臂必须在场、门关臂必须缺席——在场 = 上一臂残留或覆写没落到
/// false，那「门关」两臂其实开着门，地板与区分力守卫一起失效。
void requireRebaseEcho(const Arm& a, const std::string& logText)
{
    const std::string needle = "coordinate.rebase = true";
    const bool present = logText.find(needle) != std::string::npos;
    if (a.rebase && !present)
        throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + needle
            + "` ⟹ coordinate.rebase=true 落盘了但**引擎没按它跑**（键被 gate_lint 拒 / 读键"
              "路径没接上）。开着门却静默按门关跑 = 主判据在门关态上下结论，全部读数作废");
    if (!a.rebase && present)
        throw std::runtime_error("门关臂 " + a.name + " 的引擎回显里**出现了** `" + needle
            + "` ⟹ 该臂其实开着门（覆写打偏或被后面的门盖掉），地板与区分力守卫不是门关态");
}

/// 采一臂：交付配置 + 换 entities 装靶 + 机位从第 1 帧生效 + metalrough 通道直读。
/// 每臂重建目录（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const Arm& a, const std::string& profileDir, std::string& logText)
{
    std::error_code ec;
    std::filesystem::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线：判据跑在交付配置上
    o.size = { kWidth, kHeight };
    o.overrideDeliveryEntities = true;  // 第 8 例外门：装靶
    o.deliveryEntitiesLua = entitiesLua(a);
    o.overrideCoordinateRebase = true;  // 第 9 例外门：本仪器的自变量
    o.coordinateRebaseValue = a.rebase;
    // 第 10 例外门（债-154）：只有亚阈值注入臂 offT 开这扇门（δ ≠ 0），其余六臂门闭 = 逐位不变。
    o.overrideWetContrast = a.wetContrastBias != 0.0;
    o.wetContrastBias = a.wetContrastBias;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效
    o.cameraPosition = cameraOf(a).position;
    o.cameraTarget = cameraOf(a).target;
    o.cameraFov = kFovDegrees;
    makeProfile(profileDir, o);
    cv::Mat img;
    {
        Engine eng(profileDir);
        eng.advanceFrames(8);  // 热身按帧数（墙钟睡会在争用下量到瞬态）
        img = readPngBgr(eng.shot("material_swim_" + a.name, nullptr, 3));
    }  // 析构 = 引擎退出、日志落盘，回显对账必须在它之后
    logText = readEngineLog();
    requireRebaseEcho(a, logText);
    return img;
}

/// 一张地面图 → 世界格 → 每格的 roughness 字节集合。
/// 只做一件事：**按闭式地面命中把像素栅格进 5 cm 格**，并取本臂要读的那一枚通道。
/// 不可量的像素（掠射 / 相机背后 / 出地面矩形）直接跳过，不给假格。
std::map<GroundCell, std::vector<uint8_t>> rasterGroundCells(const cv::Mat& bgr, const Arm& a,
    const cv::Rect2d& groundRect)
{
    const GroundCamera cam = makeGroundCamera(cameraOf(a).position, cameraOf(a).target,
        kFovDegrees, bgr.cols, bgr.rows);
    const cv::Rect2d rect(groundRect.x + a.entityShift, groundRect.y, groundRect.width,
        groundRect.height);
    std::map<GroundCell, std::vector<uint8_t>> cells;
    for (int y = 0; y < bgr.rows; ++y)
        for (int x = 0; x < bgr.cols; ++x) {
            glm::dvec2 w;
            if (!groundHitOf(cam, x, y, kMaxDistanceM, w))
                continue;  // 掠射区 / 相机背后：这一像素量不到
            if (w.x < rect.x || w.x >= rect.x + rect.width || w.y < rect.y
                || w.y >= rect.y + rect.height)
                continue;  // 出地面矩形
            cells[{ static_cast<int>(std::floor(w.x / kCellM)),
                static_cast<int>(std::floor(w.y / kCellM)) }].push_back(
                bgr.at<cv::Vec3b>(y, x)[1]);  // G = roughness
        }
    return cells;
}

/// 一臂的地面格 → roughness 字节集合 + 要不要进全局阈值池。
/// `thresholdPool` = 本臂的字节**要不要进阈值池**（🔴 亚阈值注入臂 offT 必须排除，理由见
/// 调用点：它的 max 是 92 而不是 89，进池会把阈值从 51 抬到 52 = 改掉其余六臂的判据）。
/// 拆成两枚函数是护栏逼的（认知复杂度 16 > 15），但缝本来就对：**栅格化**是几何，
/// **阈值池**是判据口径，后者跟着前者的产物走、两者可以各自漂。
std::map<GroundCell, std::vector<uint8_t>> gatherCells(const cv::Mat& bgr, const Arm& a,
    const cv::Rect2d& groundRect, std::vector<uint8_t>& allBytes, bool thresholdPool)
{
    auto cells = rasterGroundCells(bgr, a, groundRect);
    if (thresholdPool)
        for (const auto& [c, v] : cells)
            allBytes.insert(allBytes.end(), v.begin(), v.end());
    return cells;
}

/// 逐格中位数 → **连续**世界场（格 → 归一化 roughness ∈ [0,1]）。样本数不足 kMinVotes 的格
/// **丢弃**（不是记成某个值：一格的样本本来就跨在湿/干界两侧时，中位数是假样本）。
/// 🔴 与 `binarize` 吃**同一个**中位数（先取中位数、再过阈值），两条读数才落在同一批格上——
/// 分母一旦不同，「翻转率绿而连续量红」这个债-154 的核心结论比的是两批格，整个不成立。
GroundRealField medianField(const std::map<GroundCell, std::vector<uint8_t>>& cells)
{
    GroundRealField field;
    for (const auto& [c, v] : cells) {
        if (static_cast<int>(v.size()) < kMinVotes)
            continue;
        std::vector<uint8_t> s = v;
        std::sort(s.begin(), s.end());
        field[c] = static_cast<double>(s[s.size() / 2]) / 255.0;
    }
    return field;
}

/// 逐格中位数 → 二值场（湿 = true）。阈值 = 连续场与 1 的比值口径下的 `thr/255`，
/// 由调用侧给（🔴 所有臂共用同一个阈值——各臂各算 = 同一条判据在不同机位下有不同口径）。
GroundField binarize(const GroundRealField& values, int thr)
{
    GroundField field;
    const double t = static_cast<double>(thr) / 255.0;
    for (const auto& [c, v] : values)
        field[c] = v < t;
    return field;
}

/// 一次对拍的读数（二值翻转率 + 最优平移 + 🔴 连续量 mean/p99）。
struct PairReadout {
    int common = 0;
    int mismatch = 0;
    ShiftSearch best { };
    double sumAbs = 0.0;   ///< 共同格上 |Δroughness| 之和（线性 [0,1] 域）
    double p99Abs = 0.0;   ///< 共同格上 |Δroughness| 的 p99（同单位）
    [[nodiscard]] double flipPct() const
    {
        return 100.0 * static_cast<double>(mismatch) / std::max(1, common);
    }
    double shiftM() const { return std::hypot(best.x, best.z) * kCellM; }
    /// 🔴 连续量主统计量：mean(|Δ|) 千分点。口径不在这里（→ `field_diff_metric`），
    /// 本函数只是把本对拍的 (sumAbs, common) 喂进去——与影场的 `meanAbsCovPermille` 同一份算法。
    [[nodiscard]] double meanPermille() const
    {
        return meanAbsPermille(sumAbs, static_cast<std::size_t>(common));
    }
    [[nodiscard]] double p99Permille() const { return 1000.0 * p99Abs; }
};

PairReadout compareFields(const GroundField& a, const GroundField& b,
    const GroundRealField& ra, const GroundRealField& rb)
{
    PairReadout r;
    const FieldOverlap zero = overlapOf(a, b, 0, 0);
    r.common = zero.common;
    r.mismatch = zero.mismatch;
    r.best.mismatch = static_cast<size_t>(zero.mismatch);
    if (r.mismatch > 0)
        r.best = searchShift(a, b, r.common, static_cast<size_t>(r.mismatch), kSearchRadius);
    // 🔴 连续量走 `overlapRealOf`——它的共同格判定与上面 `overlapOf` 逐字同式（同一个头、
    // 同一套格坐标口径），所以这里的 common 必须等于 r.common（下面这条守卫就是钉这件事：
    // 两条读数一旦分母分家，本仪器的核心结论「翻转率绿而连续量红」立刻不成立，必须当场抛）。
    const RealFieldOverlap ro = overlapRealOf(ra, rb, 0, 0);
    if (ro.common != r.common)
        throw std::runtime_error("二值场与连续场的共同格数不符（" + std::to_string(r.common)
            + " vs " + std::to_string(ro.common)
            + "）⟹ 两条读数吃了不同批的格，「翻转率绿而连续量红」这个结论比的是两批格，全部作废");
    r.sumAbs = ro.sumAbs;
    r.p99Abs = ro.p99Abs;
    return r;
}

void printPair(const char* label, const PairReadout& r, const char* expect)
{
    std::printf("  %-34s 共同格 %-7d 翻转 %-6d = %-6.2f%%  meanΔ %7s‰ p99 %7s‰"
                "  最优平移 (%+.2f, %+.2f) m  失配 %zu   %s\n",
        label, r.common, r.mismatch, r.flipPct(), f3(r.meanPermille()).c_str(),
        f3(r.p99Permille()).c_str(), r.best.x * kCellM, r.best.z * kCellM, r.best.mismatch,
        expect);
}

/// G1：靶与地面矩形（缺任何一件 = 抛，不 SKIP；矩形从 manifest 读，不在 C++ 里重抄几何）。
cv::Rect2d requireGroundRect()
{
    const Manifest man = loadManifest(kRigStem);
    const ManifestPiece* ground = nullptr;
    for (const auto& p : man.pieces)
        if (p.name == kGroundPiece)
            ground = &p;
    if (ground == nullptr)
        throw std::runtime_error(std::string("靶 ") + kRigStem + " 的 manifest 里没有 piece `"
            + kGroundPiece + "` ⟹ 靶换了名或换了生成器，地面矩形无从取（仪器不在 C++ 里重抄几何）");
    const cv::Rect2d rect(ground->aabbMin[0], ground->aabbMin[2],
        ground->aabbMax[0] - ground->aabbMin[0], ground->aabbMax[2] - ground->aabbMin[2]);
    if (rect.width <= 0.0 || rect.height <= 0.0)
        throw std::runtime_error(std::string("靶 ") + kRigStem + " 的地面 aabb 在 xz 上退化（"
            + std::to_string(rect.width) + "x" + std::to_string(rect.height)
            + "）⟹ 筛不出任何地面像素，而「空集上的翻转率 0」是恒真");
    std::printf("  G1 过：地面矩形 = [%.1f, %.1f] x [%.1f, %.1f]（manifest 的 %s.aabb）\n",
        rect.x, rect.x + rect.width, rect.y, rect.y + rect.height, kGroundPiece);
    return rect;
}

/// 七臂采集（单实例串行；一臂一次引擎进程，回显对账在 captureArm 内 = G2）。
std::map<std::string, cv::Mat> captureAllArms(const std::string& profileDir)
{
    std::map<std::string, cv::Mat> img;
    for (const auto& a : arms()) {
        std::string logText;
        img[a.name] = captureArm(a, profileDir, logText);
        std::printf("  ✓ 臂 %-5s rebase=%-5s camShift=%+.2f entityShift=%+.2f  %dx%d\n",
            a.name.c_str(), a.rebase ? "true" : "false", a.camShift, a.entityShift,
            img[a.name].cols, img[a.name].rows);
    }
    return img;
}

/// G3：激励在场（相机没动 = 七臂只剩六臂，而 J1 恒真）。
void requireStimulusLive(const std::map<std::string, cv::Mat>& img)
{
    const long long stimulus = cv::countNonZero(anyChannelDiff(img.at("onA"), img.at("onB")));
    std::printf("\n  G3 门开态 onA vs onB 逐位不同像素 = %lld%s\n", stimulus,
        stimulus == 0 ? "   🔴 相机根本没动" : "");
    if (stimulus == 0)
        throw std::runtime_error("G3: 门开态两机位**逐位相同** ⟹ 相机平移没进链路（camera.lua"
            " 覆写打偏 / 机位被后面的门盖掉），「两机位对拍」其实是一机位自比，恒真");
}

/// G4：每臂的场必须非退化（有效地面格数够 + 湿格占比落在 [5,95]%）。恒值场上「翻转率 0」
/// 是**恒真**——本项目反复登记的「对空集合的断言恒真 = 假绿」正是这条要拦的。
void requireFieldsNonDegenerate(const std::map<std::string, GroundField>& field)
{
    for (const auto& [name, f] : field) {
        int wet = 0;
        for (const auto& [c, label] : f)
            wet += label ? 1 : 0;
        const int n = static_cast<int>(f.size());
        const double frac = n > 0 ? 100.0 * static_cast<double>(wet) / n : 0.0;
        std::printf("  臂 %-5s 有效地面格 %-7d 湿格占比 %5.1f%%\n", name.c_str(), n, frac);
        if (n < kMinCells)
            throw std::runtime_error("G4: 臂 " + name + " 的有效地面格只有 "
                + std::to_string(n) + "（< " + std::to_string(kMinCells)
                + "）⟹ 分母太小，翻转率是彩票，全部读数作废");
        if (frac < 5.0 || frac > 95.0)
            throw std::runtime_error("G4: 臂 " + name + " 的湿格占比 = " + std::to_string(frac)
                + "% 落在 [5,95] 之外 ⟹ 材质场**恒值**（水洼调制没生效，或整幅都是水洼）。"
                  "恒值场上「翻转率 0」是恒真 = 假绿，停下来报告");
    }
}

/// G5：每一对的共同覆盖必须够（只比两个几乎不相交的场 = 分母是彩票）。
void requireCoverage(const char* what, const PairReadout& r, const GroundField& a,
    const GroundField& b)
{
    const int smaller = static_cast<int>(std::min(a.size(), b.size()));
    if (r.common < smaller * 8 / 10)
        throw std::runtime_error(std::string("G5: ") + what + " 的共同格 "
            + std::to_string(r.common) + " < 较小场的 80%（" + std::to_string(smaller)
            + "）⟹ 两个几乎不相交的场对拍，分母是彩票");
}

/// 八条判决线（阈值理由的完整推导 → judgments_table_material.cpp，这里只写判据形状）。
/// J1–J4 = 二值翻转率（债-152 立的原有四条，**一条都不动**）；
/// J5–J8 = 连续量 mean(Δ)/p99(Δ)（债-154 补的四条，🔴 主判据）。
void pushVerdicts(std::vector<Verdict>& vs, const PairReadout& det, const PairReadout& floor_,
    const PairReadout& main_, const PairReadout& inj, const PairReadout& sub)
{
    constexpr double kFlipTolPct = 0.5;   // = 实测地板 0.11% 的 4 倍（理由见注册表）
    constexpr double kShiftTolM = kCellM;  // 一格 = 量具自身的分辨率
    vs.push_back(verdictFor("material-swim-world-invariant",
        main_.flipPct() <= kFlipTolPct && main_.best.x == 0 && main_.best.z == 0,
        "门开态（coordinate.rebase=true）机位沿 +X 平移 " + f2(kStimulusM) + " m 后，同一世界"
            "点的 roughness 标签翻转 " + std::to_string(main_.mismatch) + "/"
            + std::to_string(main_.common) + " = " + f2(main_.flipPct())
            + "%，最优平移 (" + f2(main_.best.x * kCellM) + ", "
            + f2(main_.best.z * kCellM) + ") m、该平移处失配 "
            + std::to_string(main_.best.mismatch)));
    vs.push_back(verdictFor("material-swim-determinism",
        det.mismatch == 0 && det.common > 0,
        "同机位两跑（门开态）翻转 " + std::to_string(det.mismatch) + "/"
            + std::to_string(det.common) + "（须**恰为 0**）"));
    vs.push_back(verdictFor("material-swim-gate-closed-floor",
        floor_.flipPct() <= kFlipTolPct && floor_.best.x == 0 && floor_.best.z == 0,
        "门关态同一算子的地板：翻转 " + std::to_string(floor_.mismatch) + "/"
            + std::to_string(floor_.common) + " = " + f2(floor_.flipPct())
            + "%，最优平移 (" + f2(floor_.best.x * kCellM) + ", "
            + f2(floor_.best.z * kCellM) + ") m"));
    vs.push_back(verdictFor("material-swim-offset-sensitivity",
        std::abs(inj.best.x * kCellM - kStimulusM) <= kShiftTolM
            && std::abs(inj.best.z * kCellM) <= kShiftTolM && inj.flipPct() > kFlipTolPct,
        "注入已知世界偏移 d = " + f2(kStimulusM) + " m（靶挪 d、相机跟着挪 d ⟹ 取景逐字相同、"
            "只有图案的锚挪了 d）：量具报出 (" + f2(inj.best.x * kCellM) + ", "
            + f2(inj.best.z * kCellM) + ") m，该处失配 "
            + std::to_string(inj.best.mismatch) + "，零位移翻转 " + f2(inj.flipPct()) + "%"));

    // ── J5–J8：连续量（债-154）─────────────────────────────────────────────────────────
    constexpr double kMeanTolPermille = kMeanTolPermilleValue;  // 界 → 注册表（挂在实测地板上）
    constexpr double kP99TolPermille = kP99TolPermilleValue;
    vs.push_back(verdictFor("material-swim-continuous-floor",
        floor_.meanPermille() <= kMeanTolPermille && floor_.p99Permille() <= kP99TolPermille
            && floor_.common > 0,
        "门关两机位（offA vs offB，按构造世界不变）的连续量地板：mean |Δroughness| = "
            + f3(floor_.meanPermille()) + " ‰、p99 = " + f3(floor_.p99Permille())
            + " ‰（满量程千分点），可信 " + std::to_string(floor_.common) + " 格"));
    vs.push_back(verdictFor("material-swim-continuous-no-harm",
        main_.meanPermille() <= kMeanTolPermille && main_.p99Permille() <= kP99TolPermille,
        "门开两机位（onA vs onB）的连续量：mean |Δroughness| = " + f3(main_.meanPermille())
            + " ‰（门关地板 " + f3(floor_.meanPermille()) + " ‰），p99 = "
            + f3(main_.p99Permille()) + " ‰（门关 " + f3(floor_.p99Permille())
            + " ‰）——🔴 本条是主判据，翻转率 " + f2(main_.flipPct()) + "% 只是辅助"));
    vs.push_back(verdictFor("material-swim-continuous-determinism",
        det.meanPermille() == 0.0 && det.p99Permille() == 0.0 && det.common > 0,
        "同机位两跑（onA vs onA2）的连续量：mean |Δroughness| = " + f3(det.meanPermille())
            + " ‰、p99 = " + f3(det.p99Permille())
            + " ‰（两条都须**恰为 0**，不是「小于某个小数」）"));
    // 🔴 三条都要过：① 旧判据读绿（翻转率 ≤ 它自己的界）② 新判据读**红**（mean 越过 J5/J6 的界）
    //    ③ 尾部也读**红**（p99 越过 J5/J6 的界）。
    //    ⚠ ③ 一度写成 `p99 > 0.0`，收紧成 `> 界` 是**语义修正**（本条主张的是「尾部也看得见」，
    //    写成 > 0 等于没主张）——但别把它读成「它拦得住 p99 口径漂移」：实跑 M2（把 p99 下标
    //    0.99 改成 0.50）在**两种写法下 J8 都还是 PASS**（注入把约三成格都搬动了，中位数本身
    //    就有 2 LSB），真正拦住它的是闭式 selftest 的 4 条（`field_diff·p99 下标口径`、
    //    `p99 ≠ median`、`n=1000`、`落在闭式解上`）。⟹ **统计量定义的变异归闭式层，不归仪器层**；
    //    仪器层只守「这个场有没有动」，它没有能力怀疑自己用的尺。
    vs.push_back(verdictFor("material-swim-subthreshold-visible",
        sub.flipPct() <= kFlipTolPct && sub.meanPermille() > kMeanTolPermille
            && sub.p99Permille() > kP99TolPermille && sub.common > 0,
        "🔴 亚阈值注入（offB vs offT：只把湿面对比度关于阈值对称展开 δ=" + f3(kContrastBias)
            + "，相机、靶、门态逐字相同）：翻转 " + std::to_string(sub.mismatch) + "/"
            + std::to_string(sub.common) + " = " + f2(sub.flipPct())
            + "%（旧判据读绿），而 mean |Δroughness| = " + f3(sub.meanPermille())
            + " ‰、p99 = " + f3(sub.p99Permille())
            + " ‰（新判据读红）——旧绿新红同时成立才证明新判据真的多抓东西"));
}

}  // namespace

/// material-swim 入口（注册见 cli_table_material.cpp 与 main.cpp 的 kMeta）。
int cmdMaterialSwim()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_material_swim_profile";
    std::printf("material-swim —— 材质场（G-buffer metalrough 的 roughness 通道）的世界不变性"
                "：机位沿世界 +X 平移 %.2f m，同一个世界点的粗糙度必须相同\n",
        kStimulusM);
    std::printf("  靶 = models/%s.glb（零起伏平地 y=0）；采集 %dx%d，fov %.0f°，格边长 %.0f cm"
                "，交点上限 %.0f m，每格 ≥ %d 样本\n",
        kRigStem, kWidth, kHeight, kFovDegrees, kCellM * 100.0, kMaxDistanceM, kMinVotes);

    const cv::Rect2d groundRect = requireGroundRect();  // G1

    const std::map<std::string, cv::Mat> img = captureAllArms(profile);  // 单实例串行

    requireStimulusLive(img);  // G3

    // 阈值 = **出厂六臂**地面字节的 (min+max)/2（水洼 13 / 湿石 89 的双峰中点；一次算定，
    // 🔴 所有臂共用同一个阈值——各臂各算 = 同一条判据在不同机位下有不同口径）。
    // 🔴 阈值池**必须排除亚阈值注入臂 offT**：它的湿石端被对称展开到 92，进池会把阈值从 51
    //   抬到 52 —— 那不是「注入」而是**改掉其余六臂共用的判据口径**（实测 0.12 % 那个数会漂）。
    //   排除之后，offT 仍按出厂阈值 51 二值化，这才是「拿同一把尺子量一个被挪动过的场」。
    std::vector<uint8_t> allBytes;
    std::map<std::string, std::map<GroundCell, std::vector<uint8_t>>> cells;
    for (const auto& a : arms())
        cells[a.name] = gatherCells(img.at(a.name), a, groundRect, allBytes,
            /*thresholdPool =*/a.wetContrastBias == 0.0);
    if (allBytes.empty())
        throw std::runtime_error("地面像素为空 ⟹ 机位没看到地面（靶装错了？）");
    const int lo = *std::min_element(allBytes.begin(), allBytes.end());
    const int hi = *std::max_element(allBytes.begin(), allBytes.end());
    const int thr = (lo + hi) / 2;
    std::printf("\n  地面 roughness 字节（出厂六臂，注入臂 offT 不入阈值池）："
                "min=%d max=%d ⟹ 二值阈值 = %d（所有臂共用）\n", lo, hi, thr);

    std::map<std::string, GroundRealField> real;
    std::map<std::string, GroundField> field;
    for (const auto& a : arms()) {
        real[a.name] = medianField(cells.at(a.name));
        field[a.name] = binarize(real.at(a.name), thr);
    }
    requireFieldsNonDegenerate(field);  // G4
    if (hi - lo < 60)
        throw std::runtime_error("G4: 地面 roughness 字节极差 = " + std::to_string(hi - lo)
            + " LSB（< 60）⟹ 湿/干两态分不开，二值化没有意义");

    // 五对读数（🔴 先出表再判决：本批的交付物就是这些数）
    std::printf("\n  ── 对拍读数（翻转率 = 同一世界点标签不同的格占比；meanΔ/p99 = 同一世界点"
                "roughness 的连续差，‰ 满量程）──\n");
    const PairReadout rDet = compareFields(field.at("onA"), field.at("onA2"), real.at("onA"),
        real.at("onA2"));
    const PairReadout rFloor = compareFields(field.at("offA"), field.at("offB"), real.at("offA"),
        real.at("offB"));
    const PairReadout rMain = compareFields(field.at("onA"), field.at("onB"), real.at("onA"),
        real.at("onB"));
    const PairReadout rInj = compareFields(field.at("offB"), field.at("offS"), real.at("offB"),
        real.at("offS"));
    // 🔴 债-154 的核心一对：offB vs offT = **只挪湿面对比度、不挪相机也不挪靶** ⟹ 取景逐字相同、
    // 图案一格都不滑、二值标签一格都不翻，**只有场的数值**动了。旧判据对它必然绿。
    const PairReadout rSub = compareFields(field.at("offB"), field.at("offT"), real.at("offB"),
        real.at("offT"));
    printPair("J2 同机位两跑（onA vs onA2）", rDet, "须**恰为 0**");
    printPair("J3 门关地板（offA vs offB）", rFloor, "按构造世界不变");
    printPair("J1 门开主判据（onA vs onB）", rMain, "🔴 缺陷态在此红");
    printPair("J4 注入 d（offB vs offS）", rInj, "须报出 ≈ 注入量");
    printPair("J8 亚阈值注入（offB vs offT）", rSub, "🔴 旧判据绿 / 新判据红");

    // G5：共同覆盖足够
    requireCoverage("J2", rDet, field.at("onA"), field.at("onA2"));
    requireCoverage("J3", rFloor, field.at("offA"), field.at("offB"));
    requireCoverage("J1", rMain, field.at("onA"), field.at("onB"));
    requireCoverage("J4", rInj, field.at("offB"), field.at("offS"));
    requireCoverage("J8", rSub, field.at("offB"), field.at("offT"));

    std::vector<Verdict> vs;
    pushVerdicts(vs, rDet, rFloor, rMain, rInj, rSub);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("material-swim",
        { { "rig", kRigStem }, { "stimulus_m", luaNum(kStimulusM) },
            { "cell_m", luaNum(kCellM) }, { "size", std::to_string(kWidth) + "x" + std::to_string(kHeight) },
            { "thr", std::to_string(thr) }, { "j1_flip", f2(rMain.flipPct()) },
            { "j1_shift_x", f2(rMain.best.x * kCellM) }, { "j1_shift_z", f2(rMain.best.z * kCellM) },
            { "j2_flip", std::to_string(rDet.mismatch) }, { "j3_flip", f2(rFloor.flipPct()) },
            { "j4_shift_x", f2(rInj.best.x * kCellM) }, { "j4_flip", f2(rInj.flipPct()) },
            // 🔴 债-154 的四个台账字段（连续量 + 亚阈值注入的两条读数）
            { "j1_mean_dpermille", f3(rMain.meanPermille()) },
            { "j1_p99_dpermille", f3(rMain.p99Permille()) },
            { "j3_mean_dpermille", f3(rFloor.meanPermille()) },
            { "j3_p99_dpermille", f3(rFloor.p99Permille()) },
            { "j2_mean_dpermille", f3(rDet.meanPermille()) },
            { "j4_mean_dpermille", f3(rInj.meanPermille()) },
            { "j8_flip_pct", f2(rSub.flipPct()) },
            { "j8_mean_dpermille", f3(rSub.meanPermille()) },
            { "j8_p99_dpermille", f3(rSub.p99Permille()) },
            { "j8_contrast_bias", luaNum(kContrastBias) } },
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    std::printf("\n闸门 G1–G6 全过；J1–J4（翻转率，辅助）与 J5–J8（连续量，主判据）见上"
                "（挂号与容差理由 → imgqa check）。🔴 J1 或 J4 红 = 材质场随相机滑（债-152）；"
                "J5–J7 红 = 材质场在**没跨阈值**的地方动了（债-154，翻转率看不见那一格）；"
                "J8 红 = 连续量判据自己失去了区分力（此时 J5–J7 的绿不作数）。\n");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
