module;
/**
 * test_shadow_texel_absolute —— 相对坐标批 5 第 0.5 刀：**只测量，零生产改动**
 *
 * ## 本文件要回答的唯一问题
 * 把 `buildShadowVolume` 的 texel 吸附输入从**相对**切片球心换成**绝对**切片球心（下称 H3），
 * 能不能把「渲染原点每跨一格 → 四级阴影图集同时错位最多 0.4586 纹素」压到 0？
 *
 * ## 为什么这条代数值得单独测（而不是直接照它改生产码）
 * 记 `D = dot(O, a)`、`g` = 该级该轴纹素、`c_*` = 切片球心。现状吸附吃 `c_rel = c_abs − O`：
 * ```
 * 图集坐标 = dot(p_rel, a) − round(dot(c_rel, a)/g)·g      ← 吸附格跟着 O 在光基里滑
 * ```
 * 换成吃 `c_abs`（double 域吸附，吸完再减回原点落回相对空间）：
 * ```
 * 图集坐标 = dot(p_abs, a) − round(dot(c_abs, a)/g)·g       ← O 被消掉
 * ```
 * ⟹ 跳变恒为 0，**不需要任何整除条件、不需要放大盒**（零画质代价）。
 * 🔴 但「代数里消掉了」不等于「实现出来是 0」，本文件测的就是那三处不等：
 *  ① 盒心要落 **float** 才能进 `glm::lookAt`（`ShadowVolume::view` 是 float 矩阵）；
 *  ② 顶点路径 `float(p_abs − O)` 随 O 重舍入（上一刀已实测该项残余 2.7e-4 纹素）；
 *  ③ `c_abs` 本身在 `|O| ≈ 1e6` 处若落 float 只剩 0.0625 m 分辨率 ≫ 任何一级纹素。
 *
 * ## 🔴 口径：`|O| ≈ 1e6` 处必须换成「跨格」口径（本批跑出来的，不是先验设计的）
 * 上一刀的口径是「原点从 0 变成 O」。在出厂工作点那等价（0 = 门关态，就是要对比的对象）。
 * 但在 1e6 工作点，**门关态自己的切片球心 `Bounds::center` 是一个 float 存的 1e6 大数**
 * （ULP = 0.0625 m = 123 个最细级纹素）⟹ 「0 → O」量到的是**门关态的 float 缺陷**，
 * 不是 H3 的跳变（实测：H3-double 在该口径下读出 0.46 纹素 + 吸附项 123 纹素，全是假信号）。
 * ⟹ 测 3 与阈值推导改用**「原点从 O₁ 跨到 O₂ = O₁ + 一格」**这个口径 —— 它恰好也是
 * `docs/相对坐标地基_plan.md` §3 批 5 判据原文的措辞（「每次原点跨格的图集坐标跳变」）。
 * 出厂工作点仍保留「0 → O」口径，与上一刀 H0/H1/H2 那张表直接并排。
 *
 * ## 与上一刀（tests/test_shadow_texel_grid.cppm）的口径对齐
 *  ① 比较算子（`readbackSnappedCenter` / `atlasTexelOf` / `fracDistance`）与 64 个测试点、
 *     12 个随机原点**逐式同**上一刀。上一刀的 helper 住在它的匿名 namespace 里（模块内部
 *     链接），import 不进来 ⟹ 本文件只能重写一份。
 *  ② 「口径真对齐了」不靠本文件自称：有一条**校准用例**用本文件的算子复算上一刀已入账的
 *     H0 读数（实测复算 0.458633 vs 已入账 0.4586，级 3 的 s 轴），对上了才让 H3 的数进表。
 *  ③ 期望值出处 = 上面那行闭式（纯算术，出处 = IEEE 取整定义）+ 生产产物读回（被测者吐什么
 *     就读什么）。H3 的盒由本文件的**独立第二实现**给：按闭式自己写，**没有**复制
 *     `buildShadowVolume` 的函数体；它与生产的差别由「量具校准」那条逐位对过。
 *
 * ## 读数口径（🔴 三个残余分开报，别混）
 *  - **吸附项残余** = `max |q_abs(O₁) − q_abs(O₂)| / g`，纯 double 闭式，**不含任何 float
 *    落库**。闭式算式里没有原点这个符号 ⟹ 它只可能来自「相对球心落 float 再抬回绝对」那一步
 *    的舍入，实测出厂工作点为 0；非 0 时它必须是**整数个纹素**（整纹素平移无害）。
 *  - **盒心落库残余** = 上一刀同定义的 `δ_a = q_a(O₁) − q_a(O₂) − dot(ΔO,a)`（`q` 从真实
 *    float view 矩阵读回）与吸附项残余之差 —— 即「把 double 域算好的盒心写进 float 矩阵」付的。
 *  - **顶点路径残余** = 逐点图集位移 − 闭式 `δ_a/g` 的最大偏离。
 *
 * ## 变异红线声明（「改坏什么 → 哪条用例红」，全部**实测**，读数见交付报告）
 *  - **M1** 测 2 的反向半边（`SnapMode::NoSnap` 分支也去量化）→ 「测 2 姿态不变性」红 1 条
 *      （`offGrid > 0` 与 `slidLevels > 0` 两条反向半边同时读成 0）。该形态平时**常驻**在用例里。
 *  - **M2** 测 1 的比较算子 `bitwiseEqual` 从逐位（memcmp）换成容差 1e-3 纹素
 *      → 「测 1 的比较算子有区分力」红。🔴 抓手**不是**「脱格偏差 ≠ 0」那半边（H3 的残余本来
 *      就远小于容差，容差化照样判「相同」，与上一刀 H2 的形态**相反**），而是半边②主动注入的
 *      那个 0.0005 纹素偏移（实测最大偏差 7.9e-4 纹素 < 容差 1e-3，逐位判分家、容差判相同）。
 *  - **M3a** H3 吸完**忘记把原点减回相对空间**（`q_abs` 直接当相对盒心用）
 *      → 「测 1 决定性测量」红 3 条（跳变从 1.3e-3 弹回 0.46 纹素量级）。
 *  - **M3b**（探针，**实测不红是正确行为**）吸附输入从绝对球心退回相对球心（去掉 `+ origin`）
 *      → 跨格跳变仍 ≈ 0（实测 2.1e-3 纹素），只有「残余归属」那条间接出声。
 *      🔴 闭式解释：两种写法算出的盒心都落在**同一族绝对格线** `g·ℤ` 上（`round((C−D)/g)·g − D`
 *      与 `round(C/g)·g − D` 相差整数个 g），⟹ 「吸附的输入是不是绝对球心」**不是**承重项，
 *      承重的是**吸完之后要减掉 `dot(O,a)`**。下一刀可以据此不引 `c_abs` 这条数据通路。
 *  - **M4** 吸附从 double 域整体退到 float 域（`AbsoluteDouble` 走 `AbsoluteFloat` 的算式）
 *      → 「测 3 精度前提」的**落点硬界**红。🔴 这条是补出来的：跨格跳变**抓不到它**
 *      （`q_abs` 的算式里没有原点，float 也好 double 也好，换原点都不动它 ⟹ 跳变恒 0），
 *      float 形态真正的破坏是**盒锚在错误的格点上**：吸附的定义要求
 *      `|q_abs − dot(c_abs,a)| ≤ g/2`，而 float32 在 1e6 处 ULP = 0.0625 m = 123 个纹素，
 *      表达不出这枚格距（实测落点误差：double 0.48 纹素 / float 141 纹素）。
 *      ⟹ 教训：**「跳变为零」不等于「吸附是对的」**，只看跳变的判据对这类形态结构性失明。
 *  - **M5** 量具的盒 Z 界把 `pad` 写成 0（`r * 0.02f` → `0.0f`）
 *      → 「量具校准」红 24 条（与生产产物不再逐位相同）。这条证明「独立第二实现」真被逐位对过。
 *  - **M6** `readbackSnappedCenter` 的 d 轴符号写反（`-row(2)` → `row(2)`）
 *      → 「量具自检」红 12 条（硬界 `|q_d − dot(球心,d)| ≤ 半纹素` 越界上千纹素）。
 *  —— 以上 M1–M6 是第 0.5 刀的实测记录（当时零生产改动）。以下 **M7–M10 是第 1 刀**，
 *     打的是**生产码**（`scene_context.cppm` 的门开分支），读数 → 本批交付报告。
 *  - **M7** 门开分支**只漏 `d` 轴**的回减（三根轴减两根）→ 「测 1 决定性测量（生产函数）」
 *      红（跳变从 1.3e−3 弹回 0.4x）+「落地对账」红 +「门开态落位硬界」红。
 *      🔴 这条就是任务书点名的「部分修好」形态，也是主 agent 独立验红要复用的那条。
 *  - **M8** 吸附输入退回**相对**球心（`c_abs` 少加 `origin`；= 任务书那条「不必吃绝对球心」）
 *      → 「门开态落位硬界」红（盒心离球心 1e4~1e5 纹素）+「落地对账」红。
 *      ⚠ **测 1 的跨格跳变不红**（这就是第 ⑤ 条说的结构性失明，也是本批推翻任务书的凭据）。
 *  - **M9** 删掉门关早退分支（门关也走 double 域那条）→ 「门关逐位不变（函数级）」红
 *      +「量具校准」门关那半红。⚠ **出厂截图锚不红**（差 2.36e−4 纹素 < 1 LSB）。
 *  - **M10** 门开分支的吸附从 double 域退到 float 域 → 「测 3 精度前提」的**落点硬界**红
 *      （1e6 工作点 0.48 → 90 纹素）。⚠ 跨格跳变**不红**（同 M4 的理由）。
 *
 * ## 🔴 批 5 第 1 刀（本文档所在这一刀）改了什么 —— 读旧结论前必读
 *  ① `SnapMode::Production` 的含义**变了**：原本 = 现状 H0（吸相对球心、不回减），现在 =
 *     **已落地的 H3**（生产 `buildShadowVolume` 多了 origin 入参）。原本长在它身上的 H0 读数
 *     （0.4586 纹素、测 5 的 H0 行）现在由 `RelativeFloat` 那一臂给 —— 它就是改动前三行
 *     float 原式的独立复现，且仍由「量具校准」在门关态逐位对过生产产物。
 *  ② 新增「落地对账」：真实 `buildShadowVolume` 与本文件的 H3 假想形态**逐位相同**，把原本
 *     「覆盖边界」里那句「不保证改出来的生产码与本文件那份逐式相同」**关掉**。
 *  ③ 新增「门关逐位不变（函数级）」：origin=0 时六个字段与改动前逐位相同（240 次比较），
 *     非零原点必须分家（60/60 帧）。这条比出厂截图锚更定位 —— 早退分支被删掉时截图**照样绿**
 *     （门关两种写法只差 2.36e−4 纹素，落在像素量化之下）。
 *  ④ 新增「门开态落位硬界」：盒心离相对球心 ≤ 半纹素。**这条推翻了任务书的一条裁决**，
 *     见下面第 5 条。
 *  ⑤ 🔴 **推翻任务书「不需要引入绝对球心 `c_abs` 这条数据通路」那条**（原文依据 = 本文 M3b
 *     探针「吸附输入退回相对球心，跨格跳变仍 ≈ 0」）。该结论只对**跳变**这个量成立，而跳变对
 *     盒心的绝对落位结构性失明：两种写法只差整数个纹素，整数个纹素在「到最近整数的距离」里
 *     恒为 0。少加 origin 那版把盒心放到 `c_rel − O`（出厂工作点 |O| ≈ 1.6 m，级 0 盒半宽只有
 *     0.26 m ⟹ 盒整个离开场景），跳变照样是 0，图却直接没有影子 ——
 *     实测门关 vs 门开差 **65285 像素 / 最大 169 LSB**（正确实现是 13796 像素 / 165 LSB）。
 *     ⟹ 生产实现**必须**先 `c_abs = double(c_rel) + origin` 再吸附；`c_abs` 由两个既有入参
 *     现算，确实不是一条新数据通路（任务书这句话对，但由它推出的「不必吃绝对球心」不对）。
 *     判据 = ④（`TEST: tests/test_shadow_texel_absolute.cppm`「门开态落位硬界」）。
 *  ⑥ 测 3 的「double 形态」那一臂从 `AbsoluteDouble` 换成 `Production`（②已证两者逐位相同），
 *     落点硬界改成**从 view 矩阵读回产物**再抬回绝对层（生产不导出闭式吸附值）。原「吸附项
 *     有效残余 ≤ 容差」那条在生产臂上会退化成 `0 ≤ 容差` 的恒真断言，已删，职责由落点硬界接。
 *  ⑦ 新增变异（在下面红线声明里追加，全部实测）：M7 漏减一根轴 / M8 吸附退回相对球心 /
 *     M9 删门关早退 / M10 吸附退回 float 域。
 *
 * ## 🔴 本批跑出来的、与任务书预设不符的三件事（写在这，免得下一个会话重查）
 *  ① **「跳变为零」不等于「吸附是对的」**（M4 首轮全绿逼出来的）。float 域吸附的跨格跳变也是
 *     0（算式里没有原点），它坏在**落点**：吸附的定义要求 `|q_abs − dot(c_abs,a)| ≤ g/2`，
 *     float32 在 1e6 处 ULP = 0.0625 m = 123 个纹素，表达不出这枚格距。⟹ 测 3 必须同时量
 *     跳变**和**落点，只量跳变的判据对这类形态失明（实测落点误差 double 0.48 / float 141 纹素）。
 *  ② **承重的是「吸完减掉 `dot(O,a)`」，不是「吸附吃绝对球心」**（M3b 探针实测不红）。
 *     `round(dot(c_rel,a)/g)·g − dot(O,a)` 与 `round(dot(c_abs,a)/g)·g − dot(O,a)` 的盒心
 *     都落在同一族绝对格线 `g·ℤ` 上，只差整数个纹素（整纹素平移无害）。
 *     ⟹ 下一刀**可以不引 `c_abs` 这条数据通路**，只在现有吸附后减原点 —— 改动面更小。
 *     ⚠ 但 ① 说的那条精度前提仍然成立：减完之后的量级是相对量，落 float 才不丢精度。
 *  ③ **门关态确实会被改**（测 4）：`O = 0` 下 H3 与生产**不逐位相同**（盒心漂移 2.4e-4 纹素、
 *     viewProj 分量差 4.8e-7），且换盒心重建式也躲不掉 ⟹ 根源是**数值域**（float 吸附 →
 *     double 吸附）。⟹ 下一刀必须照 `motionWithOriginShift` 的**门关早退**先例，
 *     「交付图逐位不变」靠的是那条早退分支，不是靠数学恒等。
 *
 * ## 覆盖边界（如实报）
 *  - 第 0.5 刀：本文件**零生产改动**，H3 是**测试侧的假想形态**。批 5 第 1 刀已把 H3 落进
 *    生产，并由「落地对账」那条把「不保证改出来的生产码与本文件那份逐式相同」这条边界**关掉**
 *    （逐位相同）。H1/H2 那两臂仍是测试侧假想形态（生产没有「纹素抬 2 的幂」那条路）。
 *  - H3 的 `c_abs` 由 `double(c_rel) + origin` 得到，而 `c_rel` 本身是 float（`Bounds::center`）。
 *    ⟹ 本文件测的是「**在 float 精度的相对球心上做 double 域吸附**」这一形态。测 3 的另一半
 *    读数说明：这一形态在 1e6 工作点**跨格**够用，但**门关态本身**的球心精度不够。
 *  - 「门开原点由谁生产、粒度从哪条链来」不在本文件射程（那是 `projectionSystem` 的接线），
 *    判据 = tests/test_render_origin.cppm 的「门开原点闭式」+「两条粒度路径一致」+
 *    「粒度 NaN 必须响亮 throw」三条。
 *  - 🔴 **绝对落位那条判据在 1e6 工作点结构性判不了**（「门开态落位硬界」用例内写了理由）：
 *    读回的盒心是 float 矩阵里的**相对**量，抬回绝对层要加 `dot(O,a)`（1e6 量级），而读回轴与
 *    本文件按定义算的轴那份 ~1e-7 的相对角差乘上 |O| 就是几百个纹素。1e6 那侧由「半纹素位移
 *    硬界」+「测 3 落点硬界」两条覆盖，别把这条读成「1e6 没验」。
 */
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::lookAt / ortho / perspective / radians
#include <algorithm>                     // std::sort / std::unique / std::distance
#include <cmath>                         // std::round / std::abs / std::isfinite / std::frexp
#include <cstring>                       // memcmp（「逐位」按字面判）
#include <initializer_list>              // modules 坑：花括号列表不从 import 带进来
#include <utility>                       // std::pair（姿态扰动表）

export module test_shadow_texel_absolute;

import rlstd;
import test_support;       // testing::Lcg / expectNear
import scene_context;      // buildShadowVolume / quantizeShadowDistance / ShadowVolume / Light / Bounds
import scene_data;         // kActiveShadowCascades
import render_graph;       // shadowAtlasColumns / shadowTileSize
import render_list;        // computeCascadeSpheres / shadowTexelSize
import coordinate_rebase;  // rebaseSnapGranularity / snapOriginToGranularity / toRelative

namespace RGL {
namespace {

using testing::Lcg;

/** 属性测试种子（写死；换种子 = 换一组输入，不是「重试」）。 */
constexpr uint32_t kSeedH3 = 20261105u;

/** 上一刀那 12 个随机原点用的种子（**照抄它的值**，样本集合必须同一批才能并表）。 */
constexpr uint32_t kSeedGridOrigins = 20261103u;

// ─── 出厂配置读数（与上一刀同一组；改配置 = 本文件读数跟着重立，这是有意的）─────

/** `configs/camera.lua` 的 `fovDegrees = 45.0`。 */
constexpr float kFovYDeg = 45.0f;
/** `configs/runtime.lua` 的 `headless_size = { 1280, 720 }` ⟹ 渲染网格宽高比 16:9。 */
constexpr float kAspect = 16.0f / 9.0f;
/** `configs/camera.lua` 的交付机位（绝对层，double）。 */
const glm::dvec3 kEyeAbs{1.1, 0.75, 1.05};
const glm::dvec3 kTargetAbs{0.14, 0.12, 0.02};
/** `configs/scene.lua` 的 `light.direction`（消费时归一化）。 */
const glm::vec3 kSunDir{0.5f, -0.75f, -0.45f};

/**
 * 级联深度界的出厂读数（出处 = `docs/还债总账.md` 债-139 行的实机记录，与上一刀同一组）。
 * 运行期 near/far 要 SceneBounds（要装模型），无设备单测拿不到 ⟹ 取台账唯一一组实机读数。
 */
constexpr float kNearRaw = 0.05f;
constexpr float kFarRaw = 1.713496f;

/** 合成投影体 AABB（与出厂场景同量级）。只进盒的 Z 界，不进 XY 吸附的判据。 */
constexpr glm::dvec3 kCasterMinAbs{-1.5, -0.5, -1.5};
constexpr glm::dvec3 kCasterMaxAbs{1.5, 2.0, 1.5};

/**
 * 测 3 的工作点：整个场景（相机 + 投影体 + 测试点）一起搬到 `|O| ≈ 1e6`。
 *
 * 🔴 这不是极端假设，是 rebase 真开工那天的量级（债-85：0.5px 可见爬行触发点 ≈ 7.6e5，
 * 而 rebase 的立项理由正是「未来消费者要的世界坐标比这更大」）。取非轴对齐方向，
 * 免得某个分量恰好落在 2 的幂上把 float 形态的精度损失掩盖掉。
 */
const glm::dvec3 kFarFieldShift{1.0e6, 2.5e5, -7.5e5};

/** 图集单块边长 = 出厂态 snap 格（`buildShadowVolumes` 里那个 `snapGrid`）。 */
const uint32_t kTile = shadowTileSize(shadowAtlasColumns(kActiveShadowCascades));

/** 出厂主光（只 direction 参与光空间基）。 */
const Light kSun{.index = 0u, .direction = kSunDir, .color = glm::vec3{1.0f, 0.98f, 0.95f},
                 .intensity = 12.0f, .ambient = glm::vec3{0.25f}, .position = glm::dvec3{0.0},
                 .range = 0.0f, .type = 0u};

/** 上一刀已入账的 H0 读数（出处 = docs/相对坐标地基_plan.md §3 批 5 第 0 刀那张表）。 */
constexpr double kPublishedH0Jump = 0.4586;

/** float32 在 1e6 处的 ULP（出处 = docs/还债总账.md 债-85 与 coordinate_rebase 注释）。 */
constexpr double kUlpAtOneMillion = 0.0625;

// ─── 场景与帧产物 ───────────────────────────────────────────────────────────

/** 一次测量的绝对层输入：相机机位 + 投影体 AABB（测 3 整体平移，相对几何不变）。 */
struct AbsoluteScene {
	glm::dvec3 eye;
	glm::dvec3 target;
	glm::dvec3 casterMin;
	glm::dvec3 casterMax;
};

const AbsoluteScene kFactoryScene{.eye = kEyeAbs, .target = kTargetAbs,
                                   .casterMin = kCasterMinAbs, .casterMax = kCasterMaxAbs};

/** 整体平移一个场景（相机与投影体一起搬 ⟹ 相对几何与出厂那一帧同构）。 */
AbsoluteScene shifted(const AbsoluteScene& s, const glm::dvec3& delta) {
	return AbsoluteScene{.eye = s.eye + delta, .target = s.target + delta,
	                     .casterMin = s.casterMin + delta, .casterMax = s.casterMax + delta};
}

/**
 * 吸附算式吃哪个球心、在哪个数值域做、用哪种重建式（本批的自变量）。
 *
 * 🔴 **批 5 第 1 刀改了本枚举的含义**（读旧读数时必读）：`Production` 原本 = 现状 H0
 * （吸附吃相对球心、不回减原点），现在 = **生产已落地的 H3**。原本长在 `Production` 上的
 * 那些读数（H0 = 0.4586 纹素、测 5 的取舍表 H0 行）现在由 `RelativeFloat` 那一臂接住 ——
 * 它就是**改动前的生产规则**的独立复现，且由「量具校准」逐位对过改动前的生产产物。
 */
enum class SnapMode {
	Production,          ///< 真实 `buildShadowVolume`（批 5 第 1 刀起 = H3：绝对球心吸附 + 回减原点）
	RelativeFloat,       ///< **改动前**的规则（吸相对球心、不回减）；门关逐位不变的参照物 + H0 读数臂
	AbsoluteFloat,       ///< H3 的 float 形态：`c_abs` 落 float、吸附在 float 域做
	AbsoluteDouble,      ///< H3 主张形态：double 域吸附，盒心按 `Σ q_a·a` 重建
	AbsoluteDoubleRecon, ///< 同上但按生产的形式重建盒心（`c + Σ(q_a − dot(c,a))·a`）——归因用
	NoSnap,              ///< 测 2 的反向半边：`q = dot(c_abs,a)` 直传（不量化）
};

/**
 * 一帧的级联产物。字段与上一刀的 `CascadeFrame` 同义（读数才能并表），外加一列闭式吸附值。
 * @param qAbs 闭式吸附值 `round(dot(c_abs,a)/g)·g`（double，**未落库**）；只有
 *             `AbsoluteDouble` / `AbsoluteDoubleRecon` 臂填，其余臂 `hasQAbs = false`。
 */
struct Frame {
	std::array<Bounds, kActiveShadowCascades> slices;
	std::array<ShadowVolume, kActiveShadowCascades> volumes;
	std::array<float, kActiveShadowCascades> texels;
	std::array<glm::dvec3, kActiveShadowCascades> qAbs{};
	bool hasQAbs = false;
};

/** 光基三轴 + up（与生产同定义）。 */
struct LightAxes {
	glm::vec3 s, u, d, up;
};

/**
 * 光基（出处 = `buildShadowVolume` 函数头「固定光基」那句给的**定义**：
 * `d = normalize(光向)`、`up` 在 d 接近 ±Y 时换 Z-up、`s = normalize(d×up)`、`u = s×d`）。
 * ⚠ 它**不**与 `readbackSnappedCenter` 读回的三轴逐位相同 —— 读回的是 `glm::lookAt` 内部
 * 重新归一化（`normalize(center − eye)`，而 `center − eye = d·eyeOffset`）后的那份，最后一位
 * 与生产自己那组 `s/u/d` 不同。所以「本文件的光基 == 生产的光基」这条**由「量具校准」逐位
 * 对生产产物来验**（写错 up 选择规则或叉积顺序，校准立刻红），自检里只按定义验正交与手性。
 */
LightAxes lightAxesOf(const Light& light) {
	const glm::vec3 d = glm::normalize(light.direction);
	const glm::vec3 up = (d.y > 0.99f || d.y < -0.99f) ? glm::vec3{0.0f, 0.0f, 1.0f}
	                                                   : glm::vec3{0.0f, 1.0f, 0.0f};
	const glm::vec3 s = glm::normalize(glm::cross(d, up));
	return LightAxes{.s = s, .u = glm::cross(s, d), .d = d, .up = up};
}

/**
 * 吸附后盒心（相对空间，float）+ 闭式吸附值（double）。
 *
 * 闭式（出处 = 文件头那行推导，纯算术，不抄被测码）：
 * ```
 * c_abs          = double(c_rel) + origin
 * q_abs_a        = round(dot(c_abs, a) / g) · g
 * snappedCenter  = Σ_a (q_abs_a − dot(origin, a)) · a     ← 落 float（生产喂 glm::lookAt）
 * ```
 * 🔴 `RelativeFloat` 臂**故意**与生产同式：它不是判据对象，是校准臂（校准盒的其余部分），
 * 吸附本身对不对由「吸附项单独测」与「测 1 决定性测量」两条管。
 */
glm::vec3 snapCenter(const LightAxes& ax, const glm::vec3& cRel, float texel,
                     const glm::dvec3& origin, SnapMode mode, glm::dvec3* qAbsOut) {
	const std::array<glm::vec3, 3> axes{ax.s, ax.u, ax.d};
	if (mode == SnapMode::RelativeFloat) {
		const auto snap = [texel, cRel](const glm::vec3& a) {
			return std::round(glm::dot(cRel, a) / texel) * texel;
		};
		return cRel + ax.s * (snap(ax.s) - glm::dot(cRel, ax.s))
		     + ax.u * (snap(ax.u) - glm::dot(cRel, ax.u))
		     + ax.d * (snap(ax.d) - glm::dot(cRel, ax.d));
	}
	const std::array<glm::dvec3, 3> axesD{glm::dvec3{ax.s}, glm::dvec3{ax.u}, glm::dvec3{ax.d}};
	const glm::dvec3 cAbsD{glm::dvec3{cRel} + origin};
	const double g = static_cast<double>(texel);
	glm::dvec3 qAbs{0.0, 0.0, 0.0};
	for (int a = 0; a < 3; ++a) {
		if (mode == SnapMode::AbsoluteFloat) {
			// 🔴 绝对球心落 float：|O| ≈ 1e6 处 ULP = 0.0625，比任何一级纹素都粗
			const glm::vec3 cAbsF{glm::vec3{cAbsD}};
			qAbs[a] = static_cast<double>(std::round(glm::dot(cAbsF, axes[a]) / texel) * texel);
		} else if (mode == SnapMode::NoSnap) {
			qAbs[a] = glm::dot(cAbsD, axesD[a]);  // 反向半边：不量化
		} else {
			qAbs[a] = std::round(glm::dot(cAbsD, axesD[a]) / g) * g;  // double 域
		}
	}
	if (qAbsOut != nullptr)
		*qAbsOut = qAbs;
	if (mode == SnapMode::AbsoluteFloat) {
		glm::vec3 center{0.0f, 0.0f, 0.0f};
		for (int a = 0; a < 3; ++a)
			center += axes[a] * (static_cast<float>(qAbs[a])
			                     - static_cast<float>(glm::dot(origin, axesD[a])));
		return center;
	}
	if (mode == SnapMode::AbsoluteDoubleRecon) {
		// 生产形式的重建：c + Σ(q_a − dot(c,a))·a（浮点结合律不同 ⟹ 最后一位可差）
		glm::dvec3 centerD{cRel};
		for (int a = 0; a < 3; ++a)
			centerD += axesD[a] * (qAbs[a] - glm::dot(glm::dvec3{cRel}, axesD[a]));
		return glm::vec3{centerD};
	}
	glm::dvec3 centerD{0.0, 0.0, 0.0};
	for (int a = 0; a < 3; ++a)
		centerD += axesD[a] * (qAbs[a] - glm::dot(origin, axesD[a]));
	return glm::vec3{centerD};
}

/**
 * 按**定义**重建一级光空间盒（独立第二实现，不是复制被测函数体）。
 *
 * 定义出处 = `ShadowVolume` 的字段注释与 `buildShadowVolume` 的函数头：
 *  - XY 半宽 = 切片球半径（正交盒 `ortho(-r, r, -r, r, …)`）；
 *  - Z 覆盖**全部投影体**：受光面占 `[−r, r]`，投影体占其世界 AABB 在光轴上的投影，
 *    取两者之并再各加 `r·0.02` 的余量；极值角按 d 的分量符号取（support function ——
 *    5a-8b 那条「用错角 → 建筑整批被近平面裁掉、影子忽明忽暗」的坑记录的就是它）；
 *  - 眼点 = 盒心 − 光向·eyeOffset，`eyeOffset = −tMin`。
 * 它与生产产物逐位相同由「量具校准」那条实测对过（M5 就打在 `pad` 这一行上）。
 */
ShadowVolume buildVolumeFromCenter(const LightAxes& ax, const glm::vec3& snappedCenter, float r,
                                   const glm::vec3& casterMin, const glm::vec3& casterMax,
                                   float padScale = 0.02f) {
	const glm::vec3 d = ax.d;
	const float pad = r * padScale;
	const glm::vec3 casterFar(d.x >= 0.0f ? casterMin.x : casterMax.x,
	                          d.y >= 0.0f ? casterMin.y : casterMax.y,
	                          d.z >= 0.0f ? casterMin.z : casterMax.z);
	const glm::vec3 casterNear(d.x >= 0.0f ? casterMax.x : casterMin.x,
	                           d.y >= 0.0f ? casterMax.y : casterMin.y,
	                           d.z >= 0.0f ? casterMax.z : casterMin.z);
	const float tMin = std::min(-r, glm::dot(casterFar - snappedCenter, d)) - pad;
	const float tMax = std::max(r, glm::dot(casterNear - snappedCenter, d)) + pad;
	const float eyeOffset = -tMin;
	const glm::mat4 view = glm::lookAt(snappedCenter - d * eyeOffset, snappedCenter, ax.up);
	const glm::mat4 proj = glm::ortho(-r, r, -r, r, 0.0f, tMax - tMin);
	return ShadowVolume{.viewProj = proj * view, .view = view, .xyRadius = r,
	                    .eyeOffset = eyeOffset, .tMin = tMin, .tMax = tMax};
}

/**
 * 「渲染原点 = origin」这一帧的级联产物。
 *
 * 切片球与纹素**一律来自生产函数**（H3 只改吸附的输入，不改级联几何；上一刀已实测
 * 拟合球半径与原点无关），盒由 `mode` 决定：`Production` 走真实 `buildShadowVolume`，
 * 其余走本文件的独立实现。
 * 🔴 相对坐标的减法一律走 `toRelative`（全仓唯一换算入口），本文件不自己减原点。
 * @param viewOverride 非空 = 用它当本帧 view（测 2 只换相机姿态、原点不动）
 * @param radiusOverride 非空 = 用给定半径替换拟合球半径（测 5 的 H2 臂要「纹素抬 2 的幂」
 *                       的盒，那是假想生产改法，只能改入参）
 */
Frame buildFrame(const AbsoluteScene& scene, const glm::dvec3& origin, SnapMode mode,
                 const glm::mat4* viewOverride = nullptr,
                 const float* radiusOverride = nullptr) {
	const glm::mat4 view = viewOverride != nullptr
	                           ? *viewOverride
	                           : glm::lookAt(toRelative(scene.eye, origin),
	                                         toRelative(scene.target, origin),
	                                         glm::vec3{0.0f, 1.0f, 0.0f});
	ViewProjection vp;
	vp.view = view;
	vp.proj = glm::perspective(glm::radians(kFovYDeg), kAspect, kNearRaw, kFarRaw);
	vp.nearPlane = kNearRaw;
	vp.farPlane = kFarRaw;

	Frame frame;
	frame.slices = computeCascadeSpheres(vp, quantizeShadowDistance(kNearRaw),
	                                     quantizeShadowDistance(kFarRaw));
	if (radiusOverride != nullptr)
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
			frame.slices[c] = Bounds{frame.slices[c].center, radiusOverride[c]};

	const glm::vec3 casterMin = toRelative(scene.casterMin, origin);
	const glm::vec3 casterMax = toRelative(scene.casterMax, origin);
	const LightAxes ax = lightAxesOf(kSun);
	// qAbs 三臂都填：测 3 的落点硬界要读**各臂自己的产物**（读用例另算的那份就抓不到变异）
	frame.hasQAbs = mode == SnapMode::AbsoluteDouble || mode == SnapMode::AbsoluteDoubleRecon
	                || mode == SnapMode::AbsoluteFloat;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		frame.texels[c] = shadowTexelSize(frame.slices[c].radius);
		if (mode == SnapMode::Production) {
			// 🔴 批 5 第 1 刀：生产现在**吃原点**（门关 = 逐位 0 = 走原式早退）。
			frame.volumes[c] =
			    buildShadowVolume(kSun, frame.slices[c], casterMin, casterMax, kTile, origin);
			continue;
		}
		glm::dvec3 qAbs;
		const glm::vec3 center = snapCenter(ax, frame.slices[c].center, frame.texels[c], origin,
		                                   mode, frame.hasQAbs ? &qAbs : nullptr);
		if (frame.hasQAbs)
			frame.qAbs[c] = qAbs;
		frame.volumes[c] =
		    buildVolumeFromCenter(ax, center, frame.slices[c].radius, casterMin, casterMax);
	}
	return frame;
}

// ─── 比较算子（与上一刀逐式同；它的 helper 在匿名 namespace 里，import 不进来）────

/** 光基与吸附后盒心的读回（推导见上一刀 `readbackSnappedCenter` 的注释）。 */
struct LightFrameReadback {
	glm::dvec3 s, u, d;  ///< 光基三轴
	glm::dvec3 q;        ///< 吸附后盒心在 (s,u,d) 下的坐标
};

LightFrameReadback readbackSnappedCenter(const ShadowVolume& v) {
	// 🔴 取的是**行**（glm 列主序 ⟹ 行 r = view[0][r], view[1][r], view[2][r]）。
	// 取成列不编译错、列也照样正交，只有手性会反 ⟹ 由「量具自检」的手性检查抓（M6）。
	const auto row = [&v](uint32_t r) {
		return glm::dvec3{static_cast<double>(v.view[0][r]), static_cast<double>(v.view[1][r]),
		                  static_cast<double>(v.view[2][r])};
	};
	LightFrameReadback out;
	out.s = row(0);
	out.u = row(1);
	out.d = -row(2);  // 行 2 存的是 −f
	out.q = glm::dvec3{-static_cast<double>(v.view[3][0]), -static_cast<double>(v.view[3][1]),
	                   static_cast<double>(v.view[3][2]) + static_cast<double>(v.eyeOffset)};
	return out;
}

std::array<glm::dvec3, 3> axesOf(const LightFrameReadback& r) { return {r.s, r.u, r.d}; }

/** 轴序号名：0 = s、1 = u、2 = d。 */
constexpr std::array<char, 3> kAxisNames{'s', 'u', 'd'};

/**
 * 世界点 `pAbs` 落在该级图集的哪个纹素（x, y, 深度三个分量，单位 = 纹素）。
 * 与上一刀同定义：矩阵乘在 double 域做，入参是 float 矩阵与 float 相对坐标（被测者产物）。
 */
glm::dvec3 atlasTexelOf(const ShadowVolume& v, float texel, const glm::dvec3& pAbs,
                        const glm::dvec3& origin) {
	const glm::vec3 pRel = toRelative(pAbs, origin);
	const glm::dvec4 ls = glm::dmat4(v.view) * glm::dvec4(glm::dvec3{pRel}, 1.0);
	const double u = ls.x, w = ls.y;
	const double t = -ls.z - static_cast<double>(v.eyeOffset);
	const double half = static_cast<double>(kTile) * 0.5;
	const double depth = (t - static_cast<double>(v.tMin)) /
	                     (static_cast<double>(v.tMax) - static_cast<double>(v.tMin)) *
	                     static_cast<double>(kTile);
	return glm::dvec3{u / static_cast<double>(texel) + half, w / static_cast<double>(texel) + half,
	                  depth};
}

/** 逐位相同（memcmp：+0.0 与 −0.0 判不同 —— 「逐位」两字不许读成「值相等」）。 */
bool bitwiseEqual(const glm::dvec3& a, const glm::dvec3& b) {
	return std::memcmp(&a, &b, sizeof(glm::dvec3)) == 0;
}
bool bitwiseEqual(const glm::mat4& a, const glm::mat4& b) {
	return std::memcmp(&a, &b, sizeof(glm::mat4)) == 0;
}

/** 分量取用（不用 `&v.x + k`：那是 glm 内部布局的未定义用法）。 */
double comp(const glm::dvec3& v, int k) { return k == 0 ? v.x : (k == 1 ? v.y : v.z); }

/** 到最近整数的距离（= 「有效错位」：整纹素平移无害，只有小数部分会改采样格）。 */
double fracDistance(double v) { return std::abs(v - std::round(v)); }

/**
 * 「两个格点值之差是不是整数个纹素」的判据容差（单位 = 纹素）。
 *
 * 🔴 这条**不许**取 `== 0`，理由不是保守：`q_abs = round(·)·g` 是一枚 double 乘积，两个
 * 格点值相减再除以 `g` 会带上各自乘积的舍入残量（|q_abs| ≈ 1e6 处相对 1e-16 ⟹ 绝对 1e-10，
 * 除以 g ≈ 5e-4 ⟹ 残量 ~2e-7 纹素）。逐位判整数会把「盒恰好挪了整纹素」读成不整，而整纹素
 * 平移正是**无害**的那一类 ⟹ 假红。
 * 先例 = 上一刀 H1 的吸附自检（那边取 1e-3，因为它的光基是从 float 矩阵读回的、正交性只有
 * 1e-7）；本文件吃的是纯 double 闭式，残量小六个数量级，1e-6 已含 5 倍余量。
 */
constexpr double kGridTolerance = 1e-6;

/** 2 的幂判定（`quantizeShadowDistance` 的档位定义：frexp 尾数恰为 0.5）。 */
bool isPowerOfTwo(double v) {
	int exponent = 0;
	return v > 0.0 && std::isfinite(v) && std::frexp(v, &exponent) == 0.5;
}

/** 一批测试点：铺在出厂场景的受光面附近（写死种子），覆盖四级级联的盒。 */
std::vector<glm::dvec3> makeTestPoints(const glm::dvec3& shift = glm::dvec3{0.0}) {
	std::vector<glm::dvec3> out;
	Lcg rng{kSeedH3};
	for (int i = 0; i < 64; ++i)
		out.push_back(glm::dvec3{rng.uniform(-1.2, 1.2), rng.uniform(-0.4, 0.4),
		                         rng.uniform(-1.2, 1.2)} + shift);
	return out;
}

/** 光基吸附（上一刀 H1/H2 臂的原点生成器，本文件复用它造**同一批**原点）。 */
glm::dvec3 snapInLightBasis(const glm::dvec3& desired, double granularity,
                            const std::array<glm::dvec3, 3>& axes) {
	glm::dvec3 out{0.0, 0.0, 0.0};
	for (const glm::dvec3& a : axes) {
		const double k = std::round(glm::dot(desired, a) / granularity);
		out += a * (k * granularity);
	}
	return out;
}

/** 上一刀「测 4 的补充」那 12 个随机原点（同种子、同调用序 ⟹ 同一批样本）。 */
std::vector<glm::dvec3> makeGridOrigins(double granularity, const std::array<glm::dvec3, 3>& axes,
                                        const glm::dvec3& shift) {
	std::vector<glm::dvec3> out;
	Lcg rng{kSeedGridOrigins + 3u};
	for (int i = 0; i < 12; ++i) {
		const glm::dvec3 desired{shift.x + kEyeAbs.x + rng.uniform(-4.0, 4.0), shift.y + kEyeAbs.y,
		                         shift.z + kEyeAbs.z + rng.uniform(-4.0, 4.0)};
		out.push_back(snapInLightBasis(desired, granularity, axes));
	}
	return out;
}

/** 一批相机姿态扰动（yaw, pitch）：24 组大幅度（跨格）+ 6 组 1e-5（同格）。 */
std::vector<std::pair<double, double>> makePerturbations() {
	std::vector<std::pair<double, double>> out;
	Lcg rng{kSeedH3 + 11u};
	for (int i = 0; i < 24; ++i)
		out.emplace_back(rng.uniform(-0.06, 0.06), rng.uniform(-0.03, 0.03));
	for (int i = 0; i < 6; ++i)
		out.emplace_back(rng.uniform(-1e-5, 1e-5), rng.uniform(-1e-5, 1e-5));
	return out;
}

/** 只换姿态（原点不动）造一帧：view 由「相对 eye + 扰动后的朝向」现造，其余同 buildFrame。 */
Frame yawPitchFrame(const AbsoluteScene& scene, const glm::dvec3& origin, SnapMode mode,
                    double yaw, double pitch) {
	const glm::vec3 eyeRel = toRelative(scene.eye, origin);
	const glm::vec3 dir0 = glm::normalize(toRelative(scene.target, origin) - eyeRel);
	const glm::vec3 right = glm::normalize(glm::cross(dir0, glm::vec3{0.0f, 1.0f, 0.0f}));
	const glm::vec3 upv = glm::cross(right, dir0);
	const glm::vec3 dir =
	    glm::normalize(dir0 + static_cast<float>(yaw) * right + static_cast<float>(pitch) * upv);
	const glm::mat4 view = glm::lookAt(eyeRel, eyeRel + dir, glm::vec3{0.0f, 1.0f, 0.0f});
	return buildFrame(scene, origin, mode, &view);
}

// ─── 读数本体（三个残余分开）────────────────────────────────────────────────

/** 「渲染原点从 originBase 变成 originMoved」的一次读数，单位一律 = 纹素。 */
struct H3Reading {
	double maxRaw = 0.0;        ///< 最大原始错位 |δ|
	double maxEffective = 0.0;  ///< 最大有效错位（= δ 到最近整数的距离）—— 与上一刀同定义
	int worstCascade = -1;
	int worstAxis = -1;
	double perCascadeAxis[4][3]{};
	double perCascadeAxisEffective[4][3]{};
	double snapResidual = 0.0;     ///< 吸附项残余：`|q_abs(O₁) − q_abs(O₂)| / g`（纯 double 闭式）
	double snapResidualEffective = 0.0; ///< 吸附项**有效**残余（到最近整数的距离）—— 判据用的这个
	double vertexResidual = 0.0;   ///< 顶点路径残余：逐点位移 − 闭式 δ 的最大偏离
	double pointSpread = 0.0;      ///< 级内散布（刚性平移检验）
	double maxPointDelta = 0.0;    ///< 逐点最大位移
	bool hasSnap = false;
};

/**
 * 量「原点从 originBase 变成 originMoved 后，图集相对世界挪了多少」，残余拆三份。
 *
 * 与上一刀 `measureJump` 同算子（δ 的定义、逐点位移的取法、级内散布按分量各自算），
 * 只多一个入参（起点原点不必是 0 —— 见文件头「1e6 处必须换跨格口径」那条）。
 */
H3Reading measureJump(const Frame& base, const glm::dvec3& originBase, const Frame& moved,
                      const glm::dvec3& originMoved, const glm::dvec3& shift) {
	H3Reading out;
	const glm::dvec3 deltaOrigin = originMoved - originBase;
	const auto points = makeTestPoints(shift);
	out.hasSnap = base.hasQAbs && moved.hasQAbs;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const auto frame = readbackSnappedCenter(moved.volumes[c]);
		const auto baseFrame = readbackSnappedCenter(base.volumes[c]);
		const auto ax = axesOf(frame);
		const double g = static_cast<double>(base.texels[c]);
		REQUIRE(base.texels[c] == moved.texels[c]);  // 前提：两帧同一套 texel（半径与原点无关）
		for (int a = 0; a < 3; ++a) {
			const double delta =
			    baseFrame.q[a] - frame.q[a] - glm::dot(deltaOrigin, ax[a]);
			out.perCascadeAxis[c][a] = delta / g;
			out.perCascadeAxisEffective[c][a] = fracDistance(delta / g);
			if (out.perCascadeAxisEffective[c][a] > out.maxEffective) {
				out.maxEffective = out.perCascadeAxisEffective[c][a];
				out.worstCascade = static_cast<int>(c);
				out.worstAxis = a;
			}
			out.maxRaw = std::max(out.maxRaw, std::abs(delta / g));
			if (out.hasSnap) {
				const double snapSteps = std::abs(base.qAbs[c][a] - moved.qAbs[c][a]) / g;
				out.snapResidual = std::max(out.snapResidual, snapSteps);
				out.snapResidualEffective =
				    std::max(out.snapResidualEffective, fracDistance(snapSteps));
			}
		}
		// 逐点位移的散布**按分量各自算**再取最大（x 与 y 的常数位移本来就不相等，混在一起
		// 量到的是「两根轴的差」而不是「同一根轴随点的散布」）。
		for (int k = 0; k < 2; ++k) {
			double lo = 1e300, hi = -1e300;
			for (const glm::dvec3& p : points) {
				const glm::dvec3 a0 = atlasTexelOf(base.volumes[c], base.texels[c], p, originBase);
				const glm::dvec3 a1 = atlasTexelOf(moved.volumes[c], moved.texels[c], p, originMoved);
				const double d = comp(a1, k) - comp(a0, k);
				lo = std::min(lo, d);
				hi = std::max(hi, d);
				out.maxPointDelta = std::max(out.maxPointDelta, std::abs(d));
				// 逐点位移的闭式期望 = δ（上一刀「测 2 的闭式对账」已验过这条映射）
				out.vertexResidual =
				    std::max(out.vertexResidual, std::abs(d - out.perCascadeAxis[c][k]));
			}
			out.pointSpread = std::max(out.pointSpread, hi - lo);
		}
	}
	return out;
}

/** 便捷形态：起点原点 = 0（门关态），与上一刀那张表同口径。 */
H3Reading measureJumpFromClosed(const Frame& base, const Frame& moved, const glm::dvec3& origin,
                                const glm::dvec3& shift) {
	return measureJump(base, glm::dvec3{0.0}, moved, origin, shift);
}

/**
 * 把一份读数打到 doctest 输出（读数就是本批的交付物，必须打得出来）。
 * ⚠ 臂名走 `switch` + 字符串字面量：doctest 2.5.3 在本构建把 `const char*` 变量打成指针。
 */
void reportJump(int arm, const H3Reading& r) {
	switch (arm) {
	case 0: MESSAGE("【读数】H0 现状（生产 buildShadowVolume）"); break;
	case 1: MESSAGE("【读数】H2 光基 + 每级纹素抬 2 的幂"); break;
	case 2: MESSAGE("【读数】H3-float（绝对球心落 float）"); break;
	case 3: MESSAGE("【读数】H3-double（绝对球心 + double 域吸附；批 5 第 1 刀起 = 生产实现）"); break;
	default: MESSAGE("【读数】未登记臂名"); break;
	}
	MESSAGE("  最大原始错位 = " << r.maxRaw << " 纹素，最大有效错位 = " << r.maxEffective
	          << " 纹素（出现在级 " << r.worstCascade << " 轴#" << r.worstAxis
	          << "）；吸附项残余 = " << r.snapResidual << "（有效 " << r.snapResidualEffective
	          << "），顶点路径残余 = " << r.vertexResidual << "，级内散布 = " << r.pointSpread);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		MESSAGE("  c" << c << " 原始 δ/texel (s,u,d) = " << r.perCascadeAxis[c][0] << ", "
		          << r.perCascadeAxis[c][1] << ", " << r.perCascadeAxis[c][2]
		          << " ⟹ 有效 " << r.perCascadeAxisEffective[c][0] << ", "
		          << r.perCascadeAxisEffective[c][1] << ", "
		          << r.perCascadeAxisEffective[c][2]);
}

/** 把纹素数换算成**最粗级**的世界毫米（与上一刀那张表的第二列同口径）。 */
double toCoarsestMm(double texels, float coarsestTexel) {
	return texels * static_cast<double>(coarsestTexel) * 1000.0;
}

/** 出厂态四级纹素 + 吸附格距 G（多条用例都要，出处 = 生产链路本身）。 */
double granularityOf(const Frame& f) {
	return rebaseSnapGranularity(std::span<const float>{f.texels.data(), f.texels.size()});
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 量具自检 + 量具校准（H3 的读数要进上一刀那张表，这两条是入场券）
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 自检：读回算子自己要先证明没写错（否则下面所有读数都可能是量具的 bug）。
 * 判据一律是「把两个独立读出的量对上一条硬界」——只查读回对象自身的性质（正交/单位长）
 * 拦不住「读对了矩阵、配错了轴」这种错（上一刀实测过两条，本文件的 M6 是同族）。
 */
TEST_CASE("H3 量具自检：读回的光基单位正交且手性为 −d、盒心恰在格点上且离球心 ≤ 半纹素") {
	const Frame frame = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::Production);
	REQUIRE(kTile == 1024u);  // 前提：出厂 4 级 ⟹ 2×2 图集 ⟹ 单块 1024
	const LightAxes ax = lightAxesOf(kSun);

	int snappedAwayFromCenter = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const auto rb = readbackSnappedCenter(frame.volumes[c]);
		const auto rax = axesOf(rb);
		const double g = static_cast<double>(frame.texels[c]);
		REQUIRE(g > 0.0);

		// ① 本文件按定义算的光基必须与生产 view 读回的**同轴**（容差 1e-6：读回那份是
		//    glm::lookAt 内部重新归一化的结果，最后一位不同；逐位同轴由「量具校准」验）
		for (int a = 0; a < 3; ++a) {
			const glm::dvec3 mine = a == 0 ? glm::dvec3{ax.s} : (a == 1 ? glm::dvec3{ax.u}
			                                                            : glm::dvec3{ax.d});
			INFO("轴 " << kAxisNames[a]);
			testing::expectNear(glm::length(mine), 1.0, 1e-6, 1e-6);
			testing::expectNear(glm::dot(mine, rax[a]), 1.0, 1e-6, 1e-6);  // 同向 ⟹ 没配错轴
		}

		// ② 光基单位正交 + 手性：生产是 u = s×d ⟹ s×u = **−d**（上一刀实测，别写 +1）
		for (int a = 0; a < 3; ++a) {
			testing::expectNear(glm::length(rax[a]), 1.0, 1e-6, 1e-6);
			for (int b = 0; b < 3; ++b)
				testing::expectNear(glm::dot(rax[a], rax[b]), a == b ? 1.0 : 0.0, 1e-6, 1e-6);
		}
		testing::expectNear(glm::dot(glm::cross(rb.s, rb.u), rb.d), -1.0, 1e-5, 1e-5);

		// ③ 读回的盒心必须**恰在 texel 格点上**（吸附真的按这枚格距做的机器证据）
		for (int a = 0; a < 3; ++a) {
			const double onGrid = std::round(rb.q[a] / g) * g;
			INFO("c = " << c << " 轴 " << kAxisNames[a] << " q = " << rb.q[a] << " g = " << g);
			testing::expectNear(rb.q[a], onGrid, 0.0, 1e-3 * g);
		}

		// ④ 吸附位移 ≤ 半个纹素（round 的定义 ⟹ 硬界）。d 轴符号写反会差出 2·|dot(盒心,轴)|
		const glm::dvec3 center{glm::dvec3{frame.slices[c].center}};
		for (int a = 0; a < 3; ++a) {
			const double raw = glm::dot(center, rax[a]);
			INFO("c = " << c << " 轴 " << kAxisNames[a] << " 球心 = " << raw
			           << " 读回盒心 = " << rb.q[a] << " 半纹素 = " << 0.5 * g);
			CHECK(std::abs(rb.q[a] - raw) <= 0.5 * g + 1e-3 * g);
			if (std::abs(rb.q[a] - raw) > 0.01 * g)
				++snappedAwayFromCenter;  // 吸附**真的动了**（阳性锚点）
		}
	}
	CHECK(snappedAwayFromCenter > 0);
}

/**
 * 校准：本文件的独立实现喂**现状规则**时必须与生产产物逐位相同。
 * 为什么需要：H3 的盒由本文件自己建（生产的吸附写死了吃相对球心）。若这段「按定义重建」
 * 与生产有一处不同（光基、Z 界、pad、lookAt 参数），H3 的读数就混进了量具自己的偏差
 * —— M5 就打在 `pad` 这一行上。
 */
TEST_CASE("H3 量具校准：独立实现复现**改动前**规则 ⟹ 门关态与生产逐位相同、脱格原点必须分家") {
	const Frame prod = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::Production);
	const Frame reimpl = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::RelativeFloat);
	int matched = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		INFO("c = " << c);
		// 逐位相同的是**盒**（viewProj / view / tMin / tMax / eyeOffset / xyRadius）
		CHECK(bitwiseEqual(reimpl.volumes[c].viewProj, prod.volumes[c].viewProj));
		CHECK(bitwiseEqual(reimpl.volumes[c].view, prod.volumes[c].view));
		CHECK(reimpl.volumes[c].tMin == prod.volumes[c].tMin);
		CHECK(reimpl.volumes[c].tMax == prod.volumes[c].tMax);
		CHECK(reimpl.volumes[c].eyeOffset == prod.volumes[c].eyeOffset);
		CHECK(reimpl.volumes[c].xyRadius == prod.volumes[c].xyRadius);
		++matched;
	}
	CHECK(matched == static_cast<int>(kActiveShadowCascades));  // 空转守卫

	// 🔴 前提守卫的方向**随批 5 第 1 刀翻过来了**。原本这条是「校准在脱格原点也成立」
	// （两边都 = 改动前的规则）；现在生产在脱格原点走的是**回减原点**那条新分支，两边
	// 必须**分家** —— 逐位相同反而说明传进去的 origin 根本没参与（= 「接了但没接上」）。
	// 这条同时是「门关早退分支真在生效」的另一半：门关（origin=0）上面已验相同。
	const double G = granularityOf(prod);
	const glm::dvec3 o = snapOriginToGranularity(kEyeAbs, G);
	CHECK(glm::length(o) > 0.0);  // 前提：原点真的非零
	const Frame prodM = buildFrame(kFactoryScene, o, SnapMode::Production);
	const Frame reimM = buildFrame(kFactoryScene, o, SnapMode::RelativeFloat);
	int differ = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		differ += bitwiseEqual(reimM.volumes[c].viewProj, prodM.volumes[c].viewProj) ? 0 : 1;
	MESSAGE("【读数】脱格原点下「生产 vs 改动前规则」viewProj 分家的级数 = " << differ << "/4（须 4）");
	CHECK(differ == static_cast<int>(kActiveShadowCascades));
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 2（批 5 第 1 刀）：门关逐位不变（函数级）+ 门开态的落位硬界
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 🔴 **门关逐位不变（函数级）**：`origin = dvec3(0)` 时真实 `buildShadowVolume` 的产物
 * （viewProj / view / tMin / tMax / eyeOffset / xyRadius）与**改动前**逐位相同。
 *
 * 期望值出处 = `RelativeFloat` 那一臂，它是改动前那三行 float 原式的**独立转录**
 * （由上一条「量具校准」逐位对过改动前的生产产物；本批的改动没有动那条早退分支的表达式）。
 * 为什么这条比图像锚更定位：出厂锚 efa70587… 只说「交付图没变」，早退分支被删掉时它**照样绿**
 * （门关态两种写法只差 2.36e−4 纹素，落在像素量化之下）；本条按字面判逐位，一删就红。
 *
 * 覆盖：出厂工作点 + `|O| ≈ 1e6` 工作点（门关 = 原点仍是 0，但**球心是 1e6 量级的 float**，
 * 那是早退分支最容易被打断的输入形态）+ 三组相机姿态。
 * 阳性锚点：同一批输入换成非零原点，产物必须**分家**（否则「逐位相同」是空判据 ——
 * 那等于什么都没喂进去）。
 */
TEST_CASE("🔴 测 2 门关逐位不变（函数级）：origin=0 的六个字段与改动前逐位相同，非零原点必须分家") {
	int sameFields = 0, differLevels = 0, scenes = 0;
	for (const bool far : {false, true}) {
		const AbsoluteScene scene = far ? shifted(kFactoryScene, kFarFieldShift) : kFactoryScene;
		for (const auto& [yaw, pitch] : makePerturbations()) {
			const Frame closed = yawPitchFrame(scene, glm::dvec3{0.0}, SnapMode::Production, yaw, pitch);
			const Frame legacy = yawPitchFrame(scene, glm::dvec3{0.0}, SnapMode::RelativeFloat, yaw, pitch);
			// 门关：逐位相同（memcmp 判 +0.0/−0.0，「逐位」两字不许读成「值相等」）
			for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
				INFO("far=" << far << " yaw=" << yaw << " c=" << c);
				CHECK(bitwiseEqual(closed.volumes[c].viewProj, legacy.volumes[c].viewProj));
				CHECK(bitwiseEqual(closed.volumes[c].view, legacy.volumes[c].view));
				CHECK(closed.volumes[c].tMin == legacy.volumes[c].tMin);
				CHECK(closed.volumes[c].tMax == legacy.volumes[c].tMax);
				CHECK(closed.volumes[c].eyeOffset == legacy.volumes[c].eyeOffset);
				CHECK(closed.volumes[c].xyRadius == legacy.volumes[c].xyRadius);
				++sameFields;
			}
			// 阳性锚点：非零原眼下生产必须与改动前的规则分家（早退分支没生效 = 这里相同 = 红）
			const glm::dvec3 o = snapOriginToGranularity(scene.eye, granularityOf(closed));
			REQUIRE(glm::length(o) > 0.0);
			const Frame opened = yawPitchFrame(scene, o, SnapMode::Production, yaw, pitch);
			int differ = 0;
			for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
				differ += bitwiseEqual(opened.volumes[c].viewProj, legacy.volumes[c].viewProj) ? 0 : 1;
			if (differ > 0)
				++differLevels;
			++scenes;
		}
	}
	MESSAGE("【读数】门关逐位不变的字段比较次数 = " << sameFields
	          << "，门开态与改动前规则分家的帧数 = " << differLevels << "/" << scenes);
	CHECK(sameFields == 2 * 30 * static_cast<int>(kActiveShadowCascades));  // 空转守卫
	CHECK(differLevels == scenes);  // 每一帧都分家（不分家 = 原点没参与）
}

/**
 * 🔴 **门开态的落位硬界**：吸附后的盒心（相对空间）离该级相对球心必须 ≤ 半个纹素，
 * 且逐轴落在「绝对格线平移 dot(origin,a)」那一族格点上。
 *
 * 期望值出处 = 闭式，两条独立：
 *  ① 吸附的定义 = 「落到离球心最近的那条格线」⟹ 位移 ≤ g/2（round 的定义，与实现无关）；
 *  ② 盒心的绝对位置 = `round(dot(c_abs,a)/g)·g` ⟹ 相对位置 + dot(origin,a) 必是 g 的整数倍。
 *
 * 🔴 **这条就是本批推翻任务书那条「不必引入绝对球心」读数的判据**。任务书依据的是第 0.5 刀的
 * M3b 探针（「吸附输入退回相对球心，跨格跳变仍 ≈ 0」），但**跳变那个量对盒心的绝对落位结构性
 * 失明**：两种写法只差整数个纹素，而整数个纹素在「到最近整数的距离」里恒为 0。少加 origin 的
 * 写法把盒心放到 `c_rel − O`（出厂工作点 |O| ≈ 1.6 m，级 0 的盒半宽只有 0.26 m ⟹ 盒整个离开
 * 场景），跨格跳变照样是 0，图却直接没有影子了（实测门关 vs 门开差 65285 像素 / 169 LSB）。
 * ⟹ 判据必须问「盒落在哪」，不能只问「盒动多少」。这是本项目第三次登记的同一形态
 * （判据在要抓的失效上饱和 → docs/相对坐标地基_plan.md §3 批 5）。
 */
TEST_CASE("🔴 门开态落位硬界：盒心离相对球心 ≤ 半纹素（少加 origin 的那版在此红，跳变不红）") {
	const AbsoluteScene farScene = shifted(kFactoryScene, kFarFieldShift);
	const LightAxes ax = lightAxesOf(kSun);
	int checked = 0, onGrid = 0, movedAway = 0;
	for (const bool far : {false, true}) {
		const AbsoluteScene& scene = far ? farScene : kFactoryScene;
		const Frame closed = buildFrame(scene, glm::dvec3{0.0}, SnapMode::Production);
		const double G = granularityOf(closed);
		// 三组门开原点，全部取在**相机附近**（rebase 的定义就是「把原点搬到相机」——
		// 拿一个离相机 1e6 的「原点」来测会把相对球心自己变成大 float，半纹素判据必然破，
		// 那是量具的输入不合法，不是生产的错。本批第一版就踩过这条，读数 13068 纹素）。
		const glm::dvec3 o0 = snapOriginToGranularity(scene.eye, G);
		const std::array<glm::dvec3, 3> axesOfLight{glm::dvec3{ax.s}, glm::dvec3{ax.u},
		                                            glm::dvec3{ax.d}};
		std::vector<glm::dvec3> origins{o0,
		                                 o0 + glm::dvec3{G, 0.0, 0.0},  // 沿世界 x 跨一格
		                                 snapInLightBasis(scene.eye + glm::dvec3{2.0, -1.0, 0.5},
		                                                  G, axesOfLight)};
		for (const glm::dvec3& o : origins) {
			REQUIRE(glm::length(o) > 0.0);  // 前提：门开态原点非零
			const Frame f = buildFrame(scene, o, SnapMode::Production);
			const std::array<glm::dvec3, 3> axesD{glm::dvec3{ax.s}, glm::dvec3{ax.u}, glm::dvec3{ax.d}};
			for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
				const double g = static_cast<double>(f.texels[c]);
				REQUIRE(g > 0.0);
				const auto rb = readbackSnappedCenter(f.volumes[c]);
				const glm::dvec3 cRel{glm::dvec3{f.slices[c].center}};
				// 🔴 绝对落位那条**必须**用读回来的轴（`axesOf(rb)`），不能用本文件按定义
				// 算的那组：读回的是 glm::lookAt 内部重新归一化后的那份，与生产自己那组
				// 差 ~1e-7 相对角。乘上 |c_abs| ≈ 3.4e6 ⟹ 绝对坐标差 0.34 m = 700 个纹素
				// —— 第一版混用了两套轴，1e6 工作点 72 个样本里红了 2 个，那是量具的
				// 精度问题不是生产的错（本文件头「覆盖边界」记着这条边界）。
				const auto rax = axesOf(rb);
				const bool absoluteResolvable = glm::length(o) < 1.0e3;
				for (int a = 0; a < 3; ++a) {
					// ① 位移 ≤ 半纹素（吸附的定义；容差 = 盒心落 float 那一次舍入，
					//    |q| 出厂 ≤ 2 m ⟹ ULP ≈ 2.4e-7，取 1e-3·g ≈ 5e-7 m 已含 2 倍余量）
					const double raw = glm::dot(cRel, axesD[a]);
					INFO("far=" << far << " c=" << c << " 轴" << kAxisNames[a] << " |O|="
					         << glm::length(o) << " 球心=" << raw << " 盒心=" << rb.q[a]
					         << " 半纹素=" << 0.5 * g);
					CHECK(std::abs(rb.q[a] - raw) <= 0.5 * g + 1e-3 * g);
					if (std::abs(rb.q[a] - raw) > 0.01 * g)
						++movedAway;  // 吸附**真的动了**（阳性锚点，防「恒等吸附」假绿）
					// ② 绝对落位 = 格线（相对盒心 + dot(O,a) 必是 g 的整数倍）。
					// 🔴 只在 |O| < 1e3 的工作点判：读回的盒心是 float 矩阵里的**相对**量，
					// 抬到绝对层要加 dot(O,a)（1e6 量级），而两套光基那份 ~1e-7 的相对角差
					// 乘上 |O| 就是几百个纹素 ⟹ 1e6 工作点上这条**结构性判不了**（不是生产错）。
					// 1e6 那侧由 ① 的半纹素位移硬界覆盖（它两边同用读回的轴，误差不自放大）。
					if (absoluteResolvable) {
						const double absPos = rb.q[a] + glm::dot(o, rax[a]);
						const double steps = absPos / g;
						INFO("c = " << c << " 轴 " << kAxisNames[a] << " steps = " << steps
						         << " absPos = " << absPos);
						CHECK(std::abs(steps - std::round(steps)) <= 1e-3);
						++onGrid;
						++checked;
					}
				}
			}
		}
	}
	MESSAGE("【读数】门开落位：绝对格线样本 " << checked
	          << " 个（出厂工作点：3 组原点 × 4 级 × 3 轴），全部落在格线上 = "
	          << (onGrid == checked) << "；吸附位移 > 0.01 纹素的 = " << movedAway
	          << "（1e6 工作点只判半纹素位移那条，理由见用例内注释）");
	CHECK(checked == 3 * static_cast<int>(kActiveShadowCascades) * 3);  // 空转守卫（出厂点 3 组原点）
	CHECK(onGrid == checked);   // 每一个样本都必须落在格线上
	CHECK(movedAway > 0);       // 吸附真的发生过（否则上面那条是恒真断言）
}

/**
 * 🔴 **落地对账：改出来的生产码就是量具量过的那份。**
 * 第 0.5 刀的 H3 是**测试侧假想形态**，它的读数（跨格跳变 0.0013 纹素、盒放大 1.000）是
 * 本批落地形态的设计依据；而那个文件自己写着覆盖边界「不保证改出来的生产码与本文件那份逐式
 * 相同」。本条把那句边界**关掉**：同一组输入下真实 `buildShadowVolume` 与 `AbsoluteDouble`
 * 那一臂的盒**逐位相同**（viewProj 全矩阵 + view + tMin/tMax/eyeOffset/xyRadius）。
 *
 * 为什么必须逐位而不是「同量级」：两条算式是同一个闭式
 * （`c_abs = double(c_rel) + origin` → `round(dot(c_abs,a)/g)·g − dot(O,a)` → `Σ q_a·a` →
 * 落 float），任何一位不同都说明其中一步的数值域 / 求和顺序 / 轴集合被改过 —— 而那正是本批
 * 最容易犯的三类错（三根轴漏一根、double 掉回 float、重建式换成 `c + Σ(q_a − dot(c,a))·a`）。
 * 期望值出处 = 第 0.5 刀文件头那两行闭式，不是抄生产码。
 * 阳性锚点：同一批输入下 `RelativeFloat`（改动前的规则）必须与生产**分家**，否则上面那些
 * CHECK 是「什么都没喂进去」的恒真断言。
 */
TEST_CASE("🔴 落地对账：真实 buildShadowVolume 与第 0.5 刀的 H3 假想形态逐位相同") {
	int compared = 0, differ = 0;
	for (const bool far : {false, true}) {
		const AbsoluteScene scene = far ? shifted(kFactoryScene, kFarFieldShift) : kFactoryScene;
		const Frame closed = buildFrame(scene, glm::dvec3{0.0}, SnapMode::Production);
		const double G = granularityOf(closed);
		const auto axes = axesOf(readbackSnappedCenter(closed.volumes[0]));
		// 原点样本 = 工作点相机吸附位 + 上一刀那 12 个随机原点（与测 1 同一批，读数才能并表）
		std::vector<glm::dvec3> origins{snapOriginToGranularity(scene.eye, G)};
		for (const glm::dvec3& o : makeGridOrigins(G, axes, far ? kFarFieldShift : glm::dvec3{0.0}))
			origins.push_back(o);
		REQUIRE(origins.size() == 13);
		for (const glm::dvec3& o : origins) {
			REQUIRE(glm::length(o) > 0.0);
			const Frame prod = buildFrame(scene, o, SnapMode::Production);
			const Frame h3 = buildFrame(scene, o, SnapMode::AbsoluteDouble);
			for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
				INFO("far=" << far << " 原点=" << o.x << "," << o.y << "," << o.z << " c=" << c);
				CHECK(bitwiseEqual(prod.volumes[c].viewProj, h3.volumes[c].viewProj));
				CHECK(bitwiseEqual(prod.volumes[c].view, h3.volumes[c].view));
				CHECK(prod.volumes[c].tMin == h3.volumes[c].tMin);
				CHECK(prod.volumes[c].tMax == h3.volumes[c].tMax);
				CHECK(prod.volumes[c].eyeOffset == h3.volumes[c].eyeOffset);
				CHECK(prod.volumes[c].xyRadius == h3.volumes[c].xyRadius);
				++compared;
			}
		}
		// 阳性锚点：改动前的规则与生产分家（不分家 = 原点根本没参与）
		const glm::dvec3 o = snapOriginToGranularity(scene.eye, G);
		const Frame legacy = buildFrame(scene, o, SnapMode::RelativeFloat);
		const Frame prod = buildFrame(scene, o, SnapMode::Production);
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
			differ += bitwiseEqual(prod.volumes[c].viewProj, legacy.volumes[c].viewProj) ? 0 : 1;
	}
	MESSAGE("【读数】落地对账：逐位比较的盒 = " << compared
	          << " 个（2 工作点 × 13 组原点 × 4 级）全部相同；改动前的规则与生产分家 = " << differ
	          << "/8");
	CHECK(compared == 2 * 13 * static_cast<int>(kActiveShadowCascades));  // 空转守卫
	CHECK(differ == 2 * static_cast<int>(kActiveShadowCascades));
}


/**
 * 🔴 **本批主判据（第 1 刀）：H3 判据打在真实生产函数上**。
 * 样本 = 上一刀那 12 个随机原点 + 交付机位那一组，比较算子与测试点同上一刀，
 * 口径 = 「原点从 0（门关态）跨到 O」，与 H0/H1/H2 那张表直接并排。
 *
 * 与第 0.5 刀那条同编号用例的**唯一区别** = H3 那一臂从测试侧假想形态（`AbsoluteDouble`）
 * 换成真实 `buildShadowVolume`（喂 origin 入参）。⟹ 判据从「这样改行不行」变成「改出来的
 * 生产码确实是这样」。
 *
 * 四条判决（期望值出处逐条给）：
 *  ① 生产态跨格跳变 **≤ 1e−2 纹素** = 批 5 判据定稿值（出处 = 第 0.5 刀 26 组样本实测最大
 *     7.92e−4 × 12.6 余量；比它要拦的 H0 跳变小 46 倍）。
 *  ② H0 复算必须对上已入账的 0.4586 纹素 —— 这条是**口径对齐的入场券**：H0 那一臂现在由
 *     `RelativeFloat`（改动前规则的独立复现，已逐位对过改动前的生产产物）给。
 *  ③ 反向半边**常驻**：同一套算子下 `RelativeFloat`（= 不回减原点）的跳变必须 > 0.1 纹素。
 *     它红 = 本用例的判据没有区分力（「≤1e−2」变成了「什么都量不到」）。
 *  ④ 顶点路径残余 > 0 且与总跳变同量级 ⟹ 剩下的确实是顶点重舍入，不是吸附没吸住。
 */
TEST_CASE("🔴 测 1 决定性测量（生产函数）：真实 buildShadowVolume 跨格跳变 ≤ 1e−2 纹素") {
	const Frame base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::Production);
	const double G = granularityOf(base);
	REQUIRE(isPowerOfTwo(G));
	const auto axes = axesOf(readbackSnappedCenter(base.volumes[0]));

	// ── 口径对齐的入场券：用本文件的算子复算上一刀已入账的 H0 读数（改动前的规则）──
	const glm::dvec3 deliveryOrigin = snapOriginToGranularity(kEyeAbs, G);
	const H3Reading h0 = measureJumpFromClosed(
	    base, buildFrame(kFactoryScene, deliveryOrigin, SnapMode::RelativeFloat), deliveryOrigin,
	    glm::dvec3{0.0});
	INFO("H0 复算 = " << h0.maxEffective << " 纹素（上一刀已入账 " << kPublishedH0Jump
	                 << "），最差落点 = 级 " << h0.worstCascade << " 轴#" << h0.worstAxis);
	CHECK(h0.worstCascade == 3);
	CHECK(h0.worstAxis == 0);  // s 轴
	testing::expectNear(h0.maxEffective, kPublishedH0Jump, 1e-2, 1e-3);

	// ── 主读数：真实生产函数，交付机位那一组 + 上一刀那 12 个随机原点 ──
	std::vector<glm::dvec3> origins = makeGridOrigins(G, axes, glm::dvec3{0.0});
	origins.push_back(deliveryOrigin);
	REQUIRE(origins.size() == 13);

	double maxJump = 0.0, maxVertex = 0.0, maxSpread = 0.0;
	for (const glm::dvec3& o : origins) {
		const H3Reading r =
		    measureJump(base, glm::dvec3{0.0},
		                buildFrame(kFactoryScene, o, SnapMode::Production), o, glm::dvec3{0.0});
		MESSAGE("【读数】生产 原点 = " << o.x << "," << o.y << "," << o.z
		          << " 最大有效错位 = " << r.maxEffective
		          << " 纹素（顶点路径残余 = " << r.vertexResidual
		          << "，级内散布 = " << r.pointSpread << "）");
		maxJump = std::max(maxJump, r.maxEffective);
		maxVertex = std::max(maxVertex, r.vertexResidual);
		maxSpread = std::max(maxSpread, r.pointSpread);
		CHECK(!r.hasSnap);                       // 生产不吐闭式吸附值（吐了就是量具在自证）
		CHECK(r.maxEffective <= 0.5 + 1e-3);     // 闭式上界仍然不许破
	}
	// ── 反向半边（常驻）：不回减原点那一臂在同一套算子下必须读出 H0 量级的跳变 ──
	double maxJumpNoSubtract = 0.0;
	for (const glm::dvec3& o : origins)
		maxJumpNoSubtract = std::max(
		    maxJumpNoSubtract,
		    measureJump(base, glm::dvec3{0.0},
		                buildFrame(kFactoryScene, o, SnapMode::RelativeFloat), o, glm::dvec3{0.0})
		        .maxEffective);
	MESSAGE("【读数】生产 @ 出厂工作点：13 组原点最大有效错位 = " << maxJump
	          << " 纹素 = " << toCoarsestMm(maxJump, base.texels[3]) << " mm（最粗级）；"
	             "顶点路径残余 = " << maxVertex << "，级内散布 = " << maxSpread);
	MESSAGE("【读数】反向半边（不回减原点 = 改动前的规则）同 13 组 = " << maxJumpNoSubtract
	          << " 纹素 = " << toCoarsestMm(maxJumpNoSubtract, base.texels[3]) << " mm");

	// 🔴 判决
	CHECK(maxJump <= 1e-2);  // ① 批 5 定稿阈值（出处见用例头 ①）
	CHECK(h0.maxEffective > 0.1);  // ③ 反向半边：判据有区分力（实测 ≈ 0.46）
	CHECK(maxJump * 10.0 < h0.maxEffective);  // ①' 比要拦的量低一个数量级以上
	CHECK(maxVertex > 0.0);                   // ④ 残余确实存在（不是「恒等于 0」的空判据）
	CHECK(maxVertex < 3.0 * maxJump + 1e-9);  // ④ 残余归属：跳变就是顶点重舍入，不是吸附
	CHECK(maxJump < 3.0 * maxVertex + 1e-9);
}

/**
 * 吸附项残余的专属用例（任务书：「吸附项单独测：固定测试点为绝对坐标、只换原点」）。
 * 🔴 「固定绝对坐标」是本用例的关键：`c_rel` 必须随原点一起变（`c_rel = float(c_abs − O)`），
 * 否则测的是「两个不同的绝对球心」而不是「同一个绝对球心换原点」—— 本文件第一版就犯过这个错，
 * 症状是残余读成 123 纹素（其实是两个不同的绝对点）。
 * 期望值出处 = 闭式：q_abs 的算式里**没有 origin 这个符号**。
 */
TEST_CASE("🔴 测 1 的吸附项单独测：固定绝对球心、只换原点，round(dot(c_abs,a)/g)·g 逐位不变") {
	const Frame base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble);
	const double G = granularityOf(base);
	const auto axes = axesOf(readbackSnappedCenter(base.volumes[0]));
	const LightAxes ax = lightAxesOf(kSun);
	const std::array<glm::dvec3, 3> axesD{glm::dvec3{ax.s}, glm::dvec3{ax.u}, glm::dvec3{ax.d}};
	REQUIRE(base.hasQAbs);

	// 三组原点：门关 / 交付机位吸附 / 沿光基挪几格
	std::vector<glm::dvec3> origins{glm::dvec3{0.0}, snapOriginToGranularity(kEyeAbs, G)};
	origins.push_back(snapInLightBasis(kEyeAbs * 3.7 + glm::dvec3{2.0, 0.0, -1.0}, G, axes));

	Lcg rng{kSeedH3};
	int checked = 0, bitwiseStable = 0;
	for (int i = 0; i < 24; ++i) {
		// 🔴 固定的量是**绝对**球心；每个原点对应的相对球心由它减出来（并落 float，
		//    因为生产里 `Bounds::center` 就是 float —— 这一道收窄正是残余的唯一可能来源）
		const glm::dvec3 cAbs{rng.uniform(-1.5, 1.5), rng.uniform(-0.5, 0.5), rng.uniform(-1.5, 1.5)};
		const float texel = base.texels[static_cast<size_t>(i) % kActiveShadowCascades];
		std::vector<glm::dvec3> qPerOrigin;
		for (const glm::dvec3& o : origins) {
			const glm::vec3 cRel{glm::vec3{cAbs - o}};
			glm::dvec3 q;
			snapCenter(ax, cRel, texel, o, SnapMode::AbsoluteDouble, &q);
			qPerOrigin.push_back(q);
		}
		const double g = static_cast<double>(texel);
		bool allSame = true;
		for (size_t k = 1; k < qPerOrigin.size(); ++k) {
			if (!bitwiseEqual(qPerOrigin[0], qPerOrigin[k]))
				allSame = false;
			// 即便不同，也只许差**整数个纹素**（整纹素平移 = 采样格没变 = 无害）
			for (int a = 0; a < 3; ++a)
				CHECK(fracDistance((qPerOrigin[k][a] - qPerOrigin[0][a]) / g) <= kGridTolerance);
		}
		bitwiseStable += allSame ? 1 : 0;
		// 阳性锚点：q 必须**恰在格点上**且**真的离球心 ≤ 半纹素**（否则「不变」是空判据）
		for (int a = 0; a < 3; ++a) {
			CHECK(std::round(qPerOrigin[0][a] / g) * g == qPerOrigin[0][a]);
			CHECK(std::abs(qPerOrigin[0][a] - glm::dot(cAbs, axesD[a])) <= 0.5 * g + 1e-3 * g);
		}
		++checked;
	}
	CHECK(checked == 24);  // 空转守卫
	MESSAGE("【读数】吸附项单独测：24 个绝对球心里换三组原点**逐位不变**的 = " << bitwiseStable
	          << "（其余的差值全部是整数个纹素，上面已逐条 CHECK）");
	CHECK(bitwiseStable == 24);

	// 反向半边：判据不是「q 变不变」（两种规则的盒心都落在同一枚格上，差值恒是整数个纹素，
	// 拿它当抓手是空的 —— 本文件第一版就写了这条空判据，实测 nonIntegerSteps 恒为 0）。
	// 真正要判的是**盒有没有跟着几何走**。图集坐标 = dot(p_rel,a) − qrel_a，几何随原点动
	// −dot(O,a) ⟹ 要图集不变必须
	// ```
	// track_a ≔ ( qrel_a(O) − qrel_a(0) + dot(O,a) ) / g ≡ 0
	// ```
	// 一律在 **double 域**算（不落 float）—— 落库那一次舍入是测 1 的判据对象，混进来会让
	// 这条「代数对不对」的判据退化到 2e-4 纹素的噪声底，看不见 0.46 纹素以外的结构。
	// H3：qrel_a = round(dot(c_abs,a)/g)·g − dot(O,a) ⟹ track 恒 0
	// 现状：qrel_a = round(dot(c_rel,a)/g)·g，c_rel = c_abs − O ⟹ track 一般是任意非整数
	auto trackWorst = [&](bool absoluteRule) {
		double worst = 0.0;
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const double g = static_cast<double>(base.texels[c]);
			Lcg r2{kSeedH3 + static_cast<uint32_t>(c)};
			for (int i = 0; i < 8; ++i) {
				const glm::dvec3 cAbs{r2.uniform(-1.5, 1.5), r2.uniform(-0.5, 0.5),
				                      r2.uniform(-1.5, 1.5)};
				for (const glm::dvec3& o : origins) {
					std::array<double, 3> t{};
					for (int a = 0; a < 3; ++a) {
						// 起点（原点 = 0）时两种规则重合，q0 只有一份
						const double q0 = std::round(glm::dot(cAbs, axesD[a]) / g) * g;
						const double q1 =
						    absoluteRule
						        ? std::round(glm::dot(cAbs, axesD[a]) / g) * g - glm::dot(o, axesD[a])
						        : std::round(glm::dot(cAbs - o, axesD[a]) / g) * g;
						t[a] = fracDistance((q1 - q0 + glm::dot(o, axesD[a])) / g);
					}
					worst = std::max({worst, t[0], t[1], t[2]});
				}
			}
		}
		return worst;
	};
	const double h3TrackWorst = trackWorst(true);
	const double h0TrackWorst = trackWorst(false);
	MESSAGE("【读数】吸附项反向半边（盒跟不跟几何走，track 到最近整数的距离）：H3 最差 = "
	        << h3TrackWorst << "，现状最差 = " << h0TrackWorst << " 纹素");
	CHECK(h3TrackWorst <= kGridTolerance);  // H3 的盒精确跟着几何走
	CHECK(h0TrackWorst > 0.1);              // 现状的盒没跟着走（= 上一刀量到的那个跳变）
}

/**
 * 比较算子的区分力（任务书变异 ② 的常驻判据）。
 * 🔴 与上一刀形态**相反**：上一刀的抓手是「亚容差偏差（2.7e-4 纹素）只有逐位看得见」，
 * 而 H3 的残余本来就远小于容差 ⟹ 那一半边在这里没有抓手。本条半边②改成**主动注入**一个
 * 亚容差偏移：逐位判「分家」、容差 1e-3 判「相同」⟹ 把算子换成容差后该半边读成「相同」，
 * 本用例红（M2）。
 */
TEST_CASE("🔴 测 1 的比较算子有区分力：同帧逐位相同 / 注入亚容差偏移逐位必须判分家") {
	const Frame base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble);
	const auto points = makeTestPoints();
	REQUIRE(points.size() == 64);
	const double G = granularityOf(base);
	const glm::dvec3 o = snapOriginToGranularity(kEyeAbs, G);

	// ── 半边 ①：同一个原点独立重建两帧 ⟹ 必须逐位相同（算子的阴性对照）──
	const Frame first = buildFrame(kFactoryScene, o, SnapMode::AbsoluteDouble);
	const Frame second = buildFrame(kFactoryScene, o, SnapMode::AbsoluteDouble);
	int identicalSame = 0;
	for (const glm::dvec3& p : points)
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
			identicalSame += bitwiseEqual(atlasTexelOf(first.volumes[c], first.texels[c], p, o),
			                             atlasTexelOf(second.volumes[c], second.texels[c], p, o))
			                     ? 1
			                     : 0;
	CHECK(identicalSame == 64 * static_cast<int>(kActiveShadowCascades));

	// ── 半边 ②：注入一个**小于容差 1e-3 纹素**的盒心偏移 ⟹ 逐位必须判分家 ──
	// 偏移量取 0.0005 个**该级自己**的纹素 ⟹ 对每一级都严格 < 1e-3 纹素，
	// 落在「逐位看得见 / 容差 1e-3 看不见」的窗口里。
	// ⚠ 两个盒必须由**同一个盒心**出发（只差这个偏移）。第一版拿「未吸附的球心 + 偏移」去比
	//   「已吸附的盒心」，量到的是吸附本身挪的那半格（实测 0.49 纹素），不是注入量。
	// ⚠ 也必须用本文件的独立实现直接改盒心：生产吸附会把 0.0005 纹素的偏移**吸回**原格点，
	//   拿生产函数注入不进去（第一版在这里注入失败，读出的偏差恰为 0）。
	int bitwiseDiffer = 0;
	double maxInjected = 0.0;
	const glm::vec3 cmin = toRelative(kCasterMinAbs, glm::dvec3{0.0});
	const glm::vec3 cmax = toRelative(kCasterMaxAbs, glm::dvec3{0.0});
	const LightAxes ax = lightAxesOf(kSun);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double g = static_cast<double>(base.texels[c]);
		const glm::vec3 center0 = base.slices[c].center;
		const ShadowVolume v0 = buildVolumeFromCenter(ax, center0, base.slices[c].radius, cmin, cmax);
		const ShadowVolume v1 =
		    buildVolumeFromCenter(ax, center0 + ax.s * static_cast<float>(0.0005 * g),
		                          base.slices[c].radius, cmin, cmax);
		for (const glm::dvec3& p : points) {
			const glm::dvec3 a0 = atlasTexelOf(v0, base.texels[c], p, glm::dvec3{0.0});
			const glm::dvec3 a1 = atlasTexelOf(v1, base.texels[c], p, glm::dvec3{0.0});
			for (int k = 0; k < 3; ++k)
				maxInjected = std::max(maxInjected, std::abs(comp(a1, k) - comp(a0, k)));
			if (!bitwiseEqual(a0, a1))
				++bitwiseDiffer;
		}
	}
	MESSAGE("【读数】注入亚容差偏移后：逐位分家样本数 = " << bitwiseDiffer << " / "
	          << 64 * static_cast<int>(kActiveShadowCascades) << "，最大偏差 = " << maxInjected
	          << " 纹素（比较容差取 1e-3）");
	CHECK(bitwiseDiffer == 64 * static_cast<int>(kActiveShadowCascades));
	CHECK(maxInjected > 0.0);
	CHECK(maxInjected < 1e-3);  // 🔴 落在「容差化就看不见」的窗口里 —— M2 的抓手
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 2：H3 必须保住 5a-8b 原本买到的性质（相机姿态变化时图集仍格点量化）
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 吸附改成吃 `c_abs` 之后，「转视角影子游动」有没有从另一个方向被引回来。
 * 判据 = 盒心在光基三轴上的坐标**逐位落在格点上**，且随姿态**分段恒定**：
 * 同一格内多姿态 → 逐位相同；跨格 → 恰好差整数个纹素。
 * 反向半边常驻在本文件里（`SnapMode::NoSnap`，q 直传不量化）：上面两条必须不成立，
 * 否则「分段恒定」可能只是「盒心与姿态无关」的假绿。
 */
TEST_CASE("🔴 测 2 姿态不变性：H3 的盒心逐位落在格点上且随姿态分段恒定（不量化臂必须破）") {
	const Frame base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble);
	const LightAxes ax = lightAxesOf(kSun);
	const glm::dvec3 origin = snapOriginToGranularity(kEyeAbs, granularityOf(base));
	REQUIRE(base.hasQAbs);
	REQUIRE(glm::length(origin) > 0.0);  // 前提：原点非零（门关态测不出接线）

	const auto perturbations = makePerturbations();
	REQUIRE(perturbations.size() == 30);

	int levelsChecked = 0, multiMemberLevels = 0, crossCellPairs = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double g = static_cast<double>(base.texels[c]);
		std::vector<std::array<long long, 3>> cellKeys;
		std::vector<glm::dvec3> qPerPose;
		for (const auto& [yaw, pitch] : perturbations) {
			// 只换姿态、原点不动
			const Frame f = yawPitchFrame(kFactoryScene, origin, SnapMode::AbsoluteDouble, yaw, pitch);
			REQUIRE(f.hasQAbs);
			glm::dvec3 q;
			snapCenter(ax, f.slices[c].center, f.texels[c], origin, SnapMode::AbsoluteDouble, &q);
			// ① 逐位落在格点上（round 的像必是格点）
			for (int a = 0; a < 3; ++a)
				CHECK(std::round(q[a] / g) * g == q[a]);
			cellKeys.push_back(std::array<long long, 3>{
			    static_cast<long long>(std::llround(q[0] / g)),
			    static_cast<long long>(std::llround(q[1] / g)),
			    static_cast<long long>(std::llround(q[2] / g))});
			qPerPose.push_back(q);
		}
		// ② 同格 → 逐位相同；跨格 → 恰好差整数个纹素
		for (size_t i = 0; i < cellKeys.size(); ++i)
			for (size_t j = i + 1; j < cellKeys.size(); ++j) {
				if (cellKeys[i] == cellKeys[j]) {
					CHECK(bitwiseEqual(qPerPose[i], qPerPose[j]));  // 同格 ⟹ 逐位相同
				} else {
					for (int a = 0; a < 3; ++a) {
						const double steps = (qPerPose[j][a] - qPerPose[i][a]) / g;
						if (steps != 0.0) {
							// 跨格：差必须是**整数个纹素**（容差 = 格点量化自己的 double 残量）
							CHECK(fracDistance(steps) <= kGridTolerance);
							++crossCellPairs;
						}
					}
				}
			}
		std::sort(cellKeys.begin(), cellKeys.end());
		const auto lastUnique = std::unique(cellKeys.begin(), cellKeys.end());
		const size_t distinct = static_cast<size_t>(std::distance(cellKeys.begin(), lastUnique));
		if (distinct < cellKeys.size())
			++multiMemberLevels;
		MESSAGE("【读数】测 2 级 " << c << "：30 组姿态落在 " << distinct << " 个吸附格里");
		++levelsChecked;
	}
	CHECK(levelsChecked == static_cast<int>(kActiveShadowCascades));  // 空转守卫
	// 前提守卫：样本里必须**既有**同格对（分段恒定的抓手）**又有**跨格对（整数纹素的抓手）
	CHECK(multiMemberLevels > 0);
	CHECK(crossCellPairs > 0);

	// ── 反向半边（常驻）：吸附不量化 ⟹ 上面两条必须不成立 ──
	// 若这条不红，说明格点量化判据本身是空的（盒心与姿态无关 = 什么都没测）。
	int offGrid = 0, slidLevels = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double g = static_cast<double>(base.texels[c]);
		std::vector<glm::dvec3> raw;
		for (const auto& [yaw, pitch] : perturbations) {
			const Frame f = yawPitchFrame(kFactoryScene, origin, SnapMode::NoSnap, yaw, pitch);
			glm::dvec3 q;
			snapCenter(ax, f.slices[c].center, f.texels[c], origin, SnapMode::NoSnap, &q);
			raw.push_back(q);
			for (int a = 0; a < 3; ++a)
				if (std::round(q[a] / g) * g != q[a])
					++offGrid;
		}
		// 半格之内的两个姿态，不量化臂给出**不同**的 q（= 图集相对世界在连续滑 = 游动回来了）
		bool slid = false;
		for (size_t i = 0; i < raw.size() && !slid; ++i)
			for (size_t j = i + 1; j < raw.size() && !slid; ++j)
				if (std::abs(glm::dot(raw[i] - raw[j], glm::dvec3{ax.s})) < 0.5 * g
				    && !bitwiseEqual(raw[i], raw[j]))
					slid = true;
		slidLevels += slid ? 1 : 0;
	}
	MESSAGE("【读数】测 2 反向半边：不量化臂落在格点外的样本数 = " << offGrid
	          << "，出现「半格内仍在滑」的级数 = " << slidLevels << "（两条都必须 > 0）");
	CHECK(offGrid > 0);
	CHECK(slidLevels > 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 3：精度前提 —— 吸附必须在 double 域做
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 🔴 这条测的是**修法自身的可行性边界**，不是「double 比 float 准」的常识复述。
 * 工作点 = 整个场景搬到 `|O| ≈ 1e6`（rebase 真开工那天）。若 double 形态在此残余仍不可忽略，
 * H3 就做不到，批 5 的依赖要重排（往「绝对层本来就该是 double」上追，即批 3 第二刀那笔账）。
 *
 * 🔴 口径 = **原点跨一格**（O₁ → O₁ + G·x̂），不是「0 → O」。理由见文件头：门关态自己的
 * `Bounds::center` 是一个 float 存的 1e6 大数，「0 → O」量到的是那个缺陷而不是 H3。
 * 本用例把两种口径都跑出来，好让读者看见差别本身（读数 ④）。
 */
TEST_CASE("🔴 测 3 精度前提：|O| ≈ 1e6 下 H3 的 double 形态跨格仍成立、float 形态彻底崩") {
	const AbsoluteScene farScene = shifted(kFactoryScene, kFarFieldShift);
	const Frame base = buildFrame(farScene, glm::dvec3{0.0}, SnapMode::Production);
	const double G = granularityOf(base);

	// 工作点的原点 = 相机位置吸附到格 ⟹ |O| 就是相机离绝对原点的距离
	const glm::dvec3 o1 = snapOriginToGranularity(farScene.eye, G);
	const double originNorm = glm::length(o1);
	MESSAGE("【读数】测 3 工作点 |O| = " << originNorm << " m，G = " << G);
	CHECK(originNorm > 1.0e6);
	CHECK(originNorm < 1.0e7);

	// 跨一格：沿世界 x 挪一个 G（这就是批 5 判据原文的「原点跨一格」）
	const glm::dvec3 o2 = o1 + glm::dvec3{G, 0.0, 0.0};
	CHECK(glm::length(o2 - o1) == G);

	// float32 在 1e6 处的 ULP = 0.0625（台账里那条），必须比任何一级纹素粗
	const float finest = base.texels[0];
	CHECK(kUlpAtOneMillion > static_cast<double>(finest));
	MESSAGE("【读数】float32 @1e6 的 ULP = " << kUlpAtOneMillion << " m，最细级纹素 = " << finest
	          << " m，比值 = " << kUlpAtOneMillion / static_cast<double>(finest) << " 个纹素");

	// ── 跨格口径（主读数）──
	// 🔴 H0 臂 = `RelativeFloat`（**改动前**的规则）。批 5 第 1 刀起 `Production` 已经是 H3，
	// 拿它当 H0 会读出「现状本来就没跳变」，把这条用例的对照半边整个抹掉。
	const H3Reading h0 = measureJump(
	    buildFrame(farScene, o1, SnapMode::RelativeFloat), o1,
	    buildFrame(farScene, o2, SnapMode::RelativeFloat), o2, kFarFieldShift);
	const H3Reading hf = measureJump(
	    buildFrame(farScene, o1, SnapMode::AbsoluteFloat), o1,
	    buildFrame(farScene, o2, SnapMode::AbsoluteFloat), o2, kFarFieldShift);
	const H3Reading hd = measureJump(
	    buildFrame(farScene, o1, SnapMode::Production), o1,
	    buildFrame(farScene, o2, SnapMode::Production), o2, kFarFieldShift);
	reportJump(0, h0);
	reportJump(2, hf);
	reportJump(3, hd);
	MESSAGE("【读数】跨格口径 @ |O| ≈ 1e6：H0 = " << h0.maxEffective << " 纹素；H3-float = "
	          << hf.maxEffective << " 纹素；H3-double = " << hd.maxEffective
	          << " 纹素（吸附项残余 = " << hd.snapResidual << " 个纹素，有效 "
	          << hd.snapResidualEffective << "；顶点路径残余 = " << hd.vertexResidual << "）");

	// ① float 形态远大于 double 形态（`c_rel + O` 在 float 域把 c_rel 的有效位吃光）。
	// 🔴 比的是**原始**错位：有效错位按定义 ≤ 0.5 纹素（到最近整数的距离），float 形态早就
	//    饱和在这条上（实测有效 0.494 / 原始 10.3 纹素），拿它当判据读不出「崩得多厉害」。
	CHECK(hf.maxRaw > 1.0);
	CHECK(hd.maxRaw * 100.0 < hf.maxRaw);
	// ② 🔴 double 形态在 1e6 工作点**仍然可用**：吸附自己不动，残余塌到落库噪声量级
	// ⚠ 原本这条是「吸附项有效残余 ≤ kGridTolerance」，但生产函数不吐闭式吸附值（吐了就是
	//   拿量具自证），拿掉之后它会退化成「0 ≤ 容差」的恒真断言 ⟹ 换成下面 ⑤ 的**落点硬界**
	//   承担同一职责（那条读的确实是产物，且 M4 只有它能抓）。
	CHECK(hd.maxEffective < 1e-3);
	// ③ double 形态比现状低两个数量级以上（H3 的净收益在工作点仍在）
	CHECK(hd.maxEffective * 100.0 < h0.maxEffective);
	// ④ 精度前提的量化陈述：残余的上界 = 相对坐标落 float 那一次舍入 = ULP(|p_rel|)/g。
	//    测试点离原点 ≤ 2 m ⟹ ULP(2) = 1.19e-7，取最细纹素、放宽 30 倍（覆盖 4 级 + 盒心
	//    落库那一次）—— 出处是这条闭式论证，不是拍的。
	const double budget = 30.0 * 1.19e-7 / static_cast<double>(finest);
	INFO("double 形态残余 = " << hd.maxEffective << " 纹素，闭式预算 = " << budget);
	CHECK(hd.maxEffective < budget);

	// ⑤ 🔴 **落点**硬界（M4 的抓手，且是这条用例里唯一能抓到 M4 的判据）
	// 吸附的**定义**是「落到离球心最近的那条格线」⟹ 硬界 |q_abs − dot(c_abs,a)| ≤ g/2。
	// float 域在 1e6 处**表达不出**这枚格距（ULP 0.0625 m = 123 个纹素），落点会错到几十
	// 纹素之外 —— 而**跨格跳变仍然是 0**（q_abs 的算式里没有原点，float 也好 double 也好，
	// 换原点都不动它）。⟹ 只看跳变的判据对「锚在错误格点上」这种失效形态结构性失明。
	// 本批实测 M4 全绿就是踩在这条上，故补。
	const LightAxes ax = lightAxesOf(kSun);
	const std::array<glm::dvec3, 3> axesD{glm::dvec3{ax.s}, glm::dvec3{ax.u}, glm::dvec3{ax.d}};
	// 🔴 读的是**该臂自己的产物**：盒心从真实 float view 矩阵里读回（`readbackSnappedCenter`），
	//   再加回 dot(origin,a) 抬到绝对层。绝不读用例另算的那份闭式 —— 否则 M4 那种「生产分支
	//   偷偷走 float 算式」的变异改的是产物、判据却还在读用例自己那份 double，永远抓不到
	//   （第一版就犯了这个错，M4 全绿）。批 5 第 1 刀起生产不导出闭式吸附值，本读法也是
	//   **唯一**还能同时套在 Production / AbsoluteFloat 两臂上的读法。
	auto landingErrorOfArm = [&](const Frame& f, const glm::dvec3& o) {
		double worst = 0.0;
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const double g = static_cast<double>(f.texels[c]);
			REQUIRE(g > 0.0);
			const auto rb = readbackSnappedCenter(f.volumes[c]);
			const glm::dvec3 cAbs{glm::dvec3{f.slices[c].center} + o};
			for (int a = 0; a < 3; ++a) {
				const double qAbsFromProduct = rb.q[a] + glm::dot(o, axesD[a]);
				worst = std::max(worst, std::abs(qAbsFromProduct - glm::dot(cAbs, axesD[a])) / g);
			}
		}
		return worst;
	};
	const double landDouble = landingErrorOfArm(buildFrame(farScene, o1, SnapMode::Production), o1);
	const double landFloat = landingErrorOfArm(buildFrame(farScene, o1, SnapMode::AbsoluteFloat), o1);
	MESSAGE("【读数】落点硬界（吸附该落在离球心最近的格线上，界 = 0.5 纹素）：double 域实测 = "
	        << landDouble << " 纹素，float 域实测 = " << landFloat
	        << " 纹素（float 在 1e6 处 ULP = 123 纹素 ⟹ 表达不出这枚格距）");
	CHECK(landDouble <= 0.5 + 1e-6);  // 吸附的定义成立
	CHECK(landFloat > 1.0);           // float 域连「吸到哪条格线」都表示不出
	CHECK(landDouble * 100.0 < landFloat);

	// ── 对照：同一批帧改用「0 → O」口径会读出什么（本批发现的口径陷阱，如实入账）──
	// 🔴 这一臂**只能**用测试侧的 `AbsoluteDouble`：它要读的是 `snapResidual`（闭式吸附项），
	// 而生产函数不导出那个中间量（导出了就是让生产为量具让路）。它验的是「口径陷阱」这件事
	// 本身，与生产实现无关，所以不需要换成 Production。
	const H3Reading hdClosedCaliber = measureJumpFromClosed(
	    buildFrame(farScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble),
	    buildFrame(farScene, o1, SnapMode::AbsoluteDouble), o1, kFarFieldShift);
	MESSAGE("【读数】口径陷阱对照：同一个 H3-double 用「0 → O」口径读出最大有效错位 = "
	        << hdClosedCaliber.maxEffective << " 纹素、吸附项残余 = "
	        << hdClosedCaliber.snapResidual << " 个纹素 —— 全部来自门关态的 "
	        << "Bounds::center 是 float 存的 1e6 大数（ULP = 0.0625 m），不是 H3 的跳变");
	CHECK(hdClosedCaliber.snapResidual > 1.0);  // 门关态球心精度不够的直接读数
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 4：门关态会不会被改（决定下一刀要不要「门关早退」）
// ═══════════════════════════════════════════════════════════════════════════

/**
 * `O = 0` 时 H3 与现状**数学恒等**，但实现把吸附从 float 域换到 double 域，最后一位可能漂。
 * 这条的读数直接决定下一刀的实现形态：逐位相同 ⟹ 可以无条件生效；漂了 ⟹ 必须照
 * `motionWithOriginShift` 的门关早退先例（门关走原式，不是加 0）。
 * 另配一条**归因臂**：把漂移拆成「换数值域」与「换盒心重建式」两份（下一刀若只改吸附输入、
 * 保留生产那行重建式，代价可能更小）。
 */
TEST_CASE("🔴 测 4 门关态：O = 0 下 H3 的盒心与 viewProj 是否与生产逐位相同") {
	const Frame prod = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::Production);
	const Frame h3 = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble);
	const Frame h3recon = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDoubleRecon);
	REQUIRE_FALSE(prod.hasQAbs);
	REQUIRE(h3.hasQAbs);

	auto drift = [&prod](const Frame& f, int& viewProjSameOut, int& centerSameOut) {
		double maxCenter = 0.0, maxViewProj = 0.0;
		viewProjSameOut = centerSameOut = 0;
		for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
			const double g = static_cast<double>(prod.texels[c]);
			const auto qp = readbackSnappedCenter(prod.volumes[c]);
			const auto qh = readbackSnappedCenter(f.volumes[c]);
			centerSameOut += bitwiseEqual(qp.q, qh.q) ? 1 : 0;
			for (int a = 0; a < 3; ++a)
				maxCenter = std::max(maxCenter, std::abs(qh.q[a] - qp.q[a]) / g);
			viewProjSameOut += bitwiseEqual(f.volumes[c].viewProj, prod.volumes[c].viewProj) ? 1 : 0;
			for (uint32_t col = 0; col < 4u; ++col)
				for (uint32_t row = 0; row < 4u; ++row)
					maxViewProj =
					    std::max(maxViewProj,
					             std::abs(static_cast<double>(f.volumes[c].viewProj[col][row])
					                      - static_cast<double>(
					                            prod.volumes[c].viewProj[col][row])));
		}
		return std::pair<double, double>{maxCenter, maxViewProj};
	};

	int vpSame = 0, centerSame = 0;
	const auto [driftCenter, driftComponent] = drift(h3, vpSame, centerSame);
	int vpSameRecon = 0, centerSameRecon = 0;
	const auto [driftRecon, driftReconComponent] = drift(h3recon, vpSameRecon, centerSameRecon);
	MESSAGE("【读数】门关态 H3（Σ q_a·a 重建）：盒心逐位相同 " << centerSame << "/4，viewProj 逐位相同 "
	          << vpSame << "/4，最大盒心漂移 = " << driftCenter
	          << " 纹素，最大 viewProj 分量差 = " << driftComponent);
	MESSAGE("【读数】门关态归因臂 H3（生产式重建 c + Σ(q_a − dot(c,a))·a）：盒心逐位相同 "
	          << centerSameRecon << "/4，viewProj 逐位相同 " << vpSameRecon
	          << "/4，最大盒心漂移 = " << driftRecon
	          << " 纹素，最大 viewProj 分量差 = " << driftReconComponent);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const auto qp = readbackSnappedCenter(prod.volumes[c]);
		const auto qh = readbackSnappedCenter(h3.volumes[c]);
		MESSAGE("【读数】门关态 级 " << c << " 生产 q = " << qp.q.x << "," << qp.q.y << ","
		          << qp.q.z << " ⟂ H3 q = " << qh.q.x << "," << qh.q.y << "," << qh.q.z);
	}

	// 归因守卫：同一套 double 域算式在 origin = 0 时**必须**逐位确定（排除「实现不确定」）
	const Frame h3Again = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble);
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		CHECK(bitwiseEqual(h3Again.volumes[c].viewProj, h3.volumes[c].viewProj));

	// 🔴 读数即判据（实测是什么就钉什么，并把归因钉死）：
	//  - 漂移必须**远小于**一级纹素 ⟹ 只是最后一位，不是「换了个格距」；
	//  - 漂移必须**大于 0** ⟹ 门关态被改了，下一刀必须走门关早退。
	CHECK(driftCenter < 1e-3);
	CHECK(driftCenter > 0.0);
	CHECK(centerSame < static_cast<int>(kActiveShadowCascades));
	CHECK(vpSame < static_cast<int>(kActiveShadowCascades));
	// 归因：两种重建式都漂 ⟹ 漂的根源是**数值域**（float 吸附 → double 吸附），
	// 不是「盒心怎么写成向量」。⟹ 门关早退省不掉，换重建式省不掉。
	CHECK(driftRecon > 0.0);
	CHECK(driftRecon < 1e-3);
}

// ═══════════════════════════════════════════════════════════════════════════
// 测 5：H3 vs H2 的取舍表（给裁决用，不新建判据）
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 把 H2（每级纹素抬 2 的幂）与 H3 并排报：跳变 / 盒放大倍数 / 是否必然改门关态 /
 * 实现要动几个函数。H3 若成立，H2 应当被否决 —— 但否决要落在读数上。
 * 🔴 两臂必须用**同一批原点**（13 组）与同一套算子，否则「谁跳得多」没有意义。
 */
TEST_CASE("测 5 取舍表：H3 与 H2 的跳变 / 盒放大倍数 / 门关态影响 并排") {
	const Frame base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::Production);
	const double G = granularityOf(base);
	const auto axes = axesOf(readbackSnappedCenter(base.volumes[0]));
	const glm::vec3 cmin0 = toRelative(kCasterMinAbs, glm::dvec3{0.0});
	const glm::vec3 cmax0 = toRelative(kCasterMaxAbs, glm::dvec3{0.0});
	// 🔴 H2 那一臂是「**改动前的规则** + 每级纹素抬 2 的幂」，所以盒必须由 `RelativeFloat`
	// 的独立实现建（批 5 第 1 刀起生产走回减原点那条分支，拿它建 H2 臂的盒 = 量的是 H2+H3
	// 的混合体，取舍表的 H2 行就不再是第 0 刀那个数）。xyRadius 恒 = 入参球半径（吸附不改
	// 半宽），所以这一臂的盒半径与生产逐位同，比值读数是 H2 自己的。
	const LightAxes axH2 = lightAxesOf(kSun);

	// ── H2：每级纹素抬 2 的幂（盒要放大）──
	std::array<float, kActiveShadowCascades> raised{};
	std::array<float, kActiveShadowCascades> radii{};
	double coarseRaised = 0.0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		raised[c] = quantizeShadowDistance(base.texels[c]);
		radii[c] = static_cast<float>(static_cast<double>(raised[c]) * 0.5
		                              * static_cast<double>(kTile));
		coarseRaised = std::max(coarseRaised, static_cast<double>(raised[c]));
		CHECK(isPowerOfTwo(static_cast<double>(raised[c])));
	}
	// H2 的盒放大倍数 = 生产盒半径 vs 放大后盒半径（逐位读两次真实调用的产物，
	// 出处 = 「XY 半宽 = 切片球半径」这条定义，不经过纹素这一层）
	double h2Cost = 1.0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const ShadowVolume raisedVolume = buildVolumeFromCenter(
		    axH2, base.slices[c].center, radii[c], cmin0, cmax0);
		const double ratio =
		    static_cast<double>(raisedVolume.xyRadius) / static_cast<double>(base.volumes[c].xyRadius);
		h2Cost = std::max(h2Cost, ratio);
		MESSAGE("【读数】H2 代价 级 " << c << " 盒半径放大倍数 = " << ratio);
	}
	// H2 的公共粗格必须真能整除每一级（H2 的定义，也是它在测这同一件事）
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
		const double ratio = coarseRaised / static_cast<double>(raised[c]);
		CHECK(ratio == std::round(ratio));
	}

	// ── 同一批 13 组原点，H0 / H2 / H3 三臂并排 ──
	std::vector<glm::dvec3> origins = makeGridOrigins(G, axes, glm::dvec3{0.0});
	origins.push_back(snapOriginToGranularity(kEyeAbs, G));
	REQUIRE(origins.size() == 13);

	const Frame h2Base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::RelativeFloat,
	                                nullptr, radii.data());
	const Frame h3Base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble);
	double h0Max = 0.0, h2Max = 0.0, h3Max = 0.0, h2SnapMax = 0.0, h3SnapMax = 0.0;
	for (const glm::dvec3& o : origins) {
		const H3Reading r0 =
		    measureJumpFromClosed(base, buildFrame(kFactoryScene, o, SnapMode::RelativeFloat), o,
		                          glm::dvec3{0.0});
		// H2 的原点要按**抬后的公共粗格**在光基里吸（H2 的定义），不是沿用 G
		const glm::dvec3 o2 = snapInLightBasis(o, coarseRaised, axes);
		const H3Reading r2 =
		    measureJumpFromClosed(h2Base,
		                          buildFrame(kFactoryScene, o2, SnapMode::RelativeFloat, nullptr,
		                                     radii.data()),
		                          o2, glm::dvec3{0.0});
		const H3Reading r3 =
		    measureJump(h3Base, glm::dvec3{0.0},
		                buildFrame(kFactoryScene, o, SnapMode::AbsoluteDouble), o, glm::dvec3{0.0});
		h0Max = std::max(h0Max, r0.maxEffective);
		h2Max = std::max(h2Max, r2.maxEffective);
		h3Max = std::max(h3Max, r3.maxEffective);
		h2SnapMax = std::max(h2SnapMax, r2.snapResidualEffective);
		h3SnapMax = std::max(h3SnapMax, r3.snapResidualEffective);
	}
	// H3 的盒放大倍数：本文件的 H3 **不碰半径** ⟹ 必须逐位等于 1（这条是读数不是声明）
	double h3Cost = 1.0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		h3Cost = std::max(h3Cost, static_cast<double>(h3Base.volumes[c].xyRadius)
		                              / static_cast<double>(base.volumes[c].xyRadius));

	// H2 **必然改门关态**：写进 scene data 的 texelSizes 每级都变了（实测计数，不是推理）
	int h2TexelChanged = 0;
	for (uint32_t c = 0; c < kActiveShadowCascades; ++c)
		h2TexelChanged += h2Base.texels[c] == base.texels[c] ? 0 : 1;

	MESSAGE("═══════════ 测 5 取舍表（13 组原点最大值；最粗级纹素 = "
	        << base.texels[3] << " m）═══════════");
	MESSAGE("H0 现状      跳变 = " << h0Max << " 纹素 = " << toCoarsestMm(h0Max, base.texels[3])
	          << " mm；盒放大 = 1.000；门关态 = 基准；实现 = 0 个函数");
	MESSAGE("H2 抬纹素    跳变 = " << h2Max << " 纹素 = " << toCoarsestMm(h2Max, base.texels[3])
	          << " mm；盒放大（实测最大级） = " << h2Cost
	          << "；门关态 = 必改（四级 texel 全变，计数 " << h2TexelChanged
	          << "/4）；实现 = 级联几何 + 吸附两处");
	MESSAGE("H3 绝对球心  跳变 = " << h3Max << " 纹素 = " << toCoarsestMm(h3Max, base.texels[3])
	          << " mm；盒放大（实测） = " << h3Cost
	          << "；门关态 = 见测 4（float→double 域吸附，最后一位漂，需门关早退）；"
	             "实现 = 吸附输入一处");

	// 判据：H3 与 H2 的跳变同量级（都不到 5e-3 纹素），但 H3 的盒放大恰为 1
	CHECK(h3Cost == 1.0);
	CHECK(h2Cost > 1.0);
	CHECK(h2Cost < 2.0);  // 量化向上取 2 的幂 ⟹ 严格小于 2 倍
	CHECK(h3Max < 5e-3);
	CHECK(h2Max < 5e-3);
	CHECK(h3SnapMax <= kGridTolerance);  // H3 的吸附项有效残余为 0（H2 那一臂没有这项可测）
	CHECK(h0Max > 0.1);       // 现状仍在
	CHECK(h0Max > 100.0 * h3Max);
	CHECK(h0Max > 100.0 * h2Max);
	CHECK(h2TexelChanged == static_cast<int>(kActiveShadowCascades));  // H2 必改门关态
}

// ═══════════════════════════════════════════════════════════════════════════
// 收尾：批 5 判据阈值的推导依据（把「余量」这句话变成读数）
// ═══════════════════════════════════════════════════════════════════════════

/**
 * 建议的批 5 判据阈值 = 实测最大残余 × 余量。这条把「余量多少」从拍脑袋变成可复核的数：
 * 扫两个工作点各 13 组原点（共 26 组），取最大残余再乘余量。跨格口径（与批 5 判据原文一致）。
 */
TEST_CASE("测 1 的阈值推导：H3 残余的实测最大值 × 10 倍余量（批 5 判据直接引用）") {
	const Frame base = buildFrame(kFactoryScene, glm::dvec3{0.0}, SnapMode::AbsoluteDouble);
	const double G = granularityOf(base);
	const auto axes = axesOf(readbackSnappedCenter(base.volumes[0]));
	const AbsoluteScene farScene = shifted(kFactoryScene, kFarFieldShift);

	double maxResidual = 0.0;
	int samples = 0;
	for (const bool far : {false, true}) {
		const AbsoluteScene& scene = far ? farScene : kFactoryScene;
		const glm::dvec3 o1 = snapOriginToGranularity(scene.eye, G);
		std::vector<glm::dvec3> origins =
		    makeGridOrigins(G, axes, far ? kFarFieldShift : glm::dvec3{0.0});
		origins.push_back(o1);
		REQUIRE(origins.size() == 13);
		for (const glm::dvec3& o : origins) {
			// 跨格口径：从 o 跨到「o 沿世界 x 挪一格 G」（批 5 判据原文的措辞）
			// 🔴 起点帧必须**就在 o 上**建。第一版复用了一个 origin = 0 建好的帧却按 o 记账，
			//    量到的是「门关态 vs 门开态」而不是「跨一格」，读数 0.4999 纹素（假信号）。
			const glm::dvec3 oNext = o + glm::dvec3{G, 0.0, 0.0};
			const Frame from = buildFrame(scene, o, SnapMode::AbsoluteDouble);
			const Frame to = buildFrame(scene, oNext, SnapMode::AbsoluteDouble);
			const H3Reading r = measureJump(from, o, to, oNext,
			                                far ? kFarFieldShift : glm::dvec3{0.0});
			CHECK(r.snapResidualEffective <= kGridTolerance);
			maxResidual = std::max(maxResidual, r.maxEffective);
			++samples;
		}
	}
	CHECK(samples == 26);
	// 余量取 10 倍的理由（写清推导，不拍脑袋）：残余的唯一来源是「相对坐标与盒心各落一次
	// float32」，上界 = ULP(|相对坐标|)/g。测试点离原点 ≤ 2 m ⟹ ULP(2) = 1.19e-7，最细纹素
	// 5.06e-4 ⟹ 闭式上界 ≈ 2.4e-4 纹素/次落库，两次 ≈ 5e-4。实测最大 1.3e-3（含跨格时
	// 原点本身把测试点推到 |p_rel| ≈ 6 m 的那批，ULP 随 |p_rel| 线性放大）⟹ 10 倍覆盖
	// 「换机位 + 换深度界档 + 场景尺度到 20 m」，而阈值仍比要拦的 H0 跳变小两个数量级以上。
	const double threshold = maxResidual * 10.0;
	MESSAGE("【读数】阈值推导：26 组样本（两工作点 × 13 原点，跨格口径）实测最大残余 = "
	        << maxResidual << " 纹素 ⟹ 建议批 5 阈值 = " << threshold
	        << " 纹素（×10 余量）；换算最粗级 = " << toCoarsestMm(threshold, base.texels[3])
	        << " mm；取整到 1e-2 纹素仍留 " << (1e-2 / threshold) << " 倍余量");
	CHECK(maxResidual > 0.0);  // 残余确实存在（不是「恒等于 0」的空判据）
	CHECK(maxResidual < 5e-3);
	CHECK(threshold < 5e-2);
	CHECK(threshold * 10.0 < kPublishedH0Jump);  // 阈值与要拦的东西之间还有一个数量级
}

}  // namespace RGL
