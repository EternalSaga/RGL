#ifndef RGL_IMGQA_SHADOW_EDGE_METRIC_H
#define RGL_IMGQA_SHADOW_EDGE_METRIC_H
//
// shadow_edge_metric.h — `shadow-edge-ruler`（影边世界位置绝对尺）的**度量口径层**
//
//   缝同 `shadow_seam_metric.h` / `shadow_survival_metric.h`（「口径 vs 仪器」那条既有缝）：
//   `shadow_edge_ruler.cpp`（仪器：臂表 + 采集 + 引擎回显对账 + 四道闸门 + 误差曲线编排）
//   与本层（口径：cov 场沿世界扫描线 → 50% 过零点 → 世界毫米误差；级联/texel 的闭式模型；
//   闸门 C/D 的判据本体）不是同一条改动缝——改臂 / 阈值 / 机位动前者，改测量算子动后者，
//   而**只有后者被 `just imgqa selftest` 用闭式输入直接测**（不启引擎、毫秒级）。
//
//   ## 口径为什么长这样（缺陷本体与「为什么真值必须是算术」的论证全文在仪器层头注释）
//   缺陷类 = **级联把影边摆错世界位置**（各级纹素量化/吸附 + 曾怀疑的簇 LOD 偏差，都直接
//   搬进阴影图）。两台既有仪器都拿渲染当参照（级联比级联 / D=5 比 D=-1），对共模错误结构
//   失明。本层的分量：真值 = `qa_scenes.py:shadow_edge_rig` 的闭式影边（竖直板远侧面 +
//   |SHADOW_OFF|·H），读数 = cov 沿世界扫描线的**自身过渡中点**（定义见下，线性插值），
//   误差 = 实测 − 真值（世界 mm）。cov 场本身**不另立定义**：吃 `shadow_seam_metric.h` 的
//   `seamCovField`（与 shadow-grain / shadow-cascade-seam / shadow-caster-survival 同式同序：
//   线性域、逐通道 clip → 均值）。
//
//   ## 过零点 = 相对阈值（🔴 2026-10-20 改口径；原「50% 绝对」是主 agent 的规格错误）
//   原规格把 cov 的 **50% 绝对**过零点当影边——那是把 bistro 墙上深影（cov>0.5）外推到
//   地面：本靶地面受光里太阳只占 ~30%（env 天光补满），影深封顶 cov≈0.33（线性域实测
//   平台高 0.33），绝对 0.5 结构上不可达 ⟹ 首跑 112 个 (臂,板) 组合全「不可量」。
//   **影边位置的正确定义 = 该处自身过渡的中点**，与影的深浅无关。算法（逐步确定，无自由参数）：
//     ① 沿扫描线取 cov 剖面（等距 stepM）；梯度 g[i] = |v[i]−v[i−1]|（等距 ⟹ 差即梯度）。
//     ② 最大梯度处 k* = argmax g（gmax = 0 = 常函数 ⟹ 抛「找不到梯度」）；过渡区 = 从 k*
//        向两侧走，直到 g ≤ **0.5·gmax**（线性斜坡 ⟹ 过渡区 = 斜坡本身，闭式可推）。
//     ③ 平台窗 = 过渡区两侧各自**剩余的全部**（中位数抗过渡尾巴与个别污染样本）；
//        一侧不足 kEdgePlateauMinSamples 个样本 ⟹ 抛「平台窗退化」。
//     ④ umbra/lit = 两平台按高低排序（不假设扫描方向哪侧是影）；过渡高
//        h = umbra − lit < kEdgeTransitionMinH ⟹ 抛「过渡过浅」（噪声定位的「边」不许出数，
//        下界推导见常量注释）。
//     ⑤ 阈值 t = (lit + umbra)/2，**全窗口**扫 t 的过零点：0 个抛 / >1 个抛（点名各 s，
//        静默选边 = 把接触缘当远影缘）/ 恰 1 个 → 括号样本间线性插值（subpixel=false 给
//        括号中点，只作「插值确实在起作用」的对照）。
//   深影（lit=0, umbra=1）时 t 退化为 0.5——旧「50% 绝对」的全部闭式断言在新口径下逐字
//   成立（selftest 保留）；浅影（0→0.30、0.70→1.00）也精确（新增两条立命断言）。
//   斜坡宽度**不进读数**（「变宽 50% 点不动」保留——shadow-caster-survival 的教训：
//   拿被模糊量当位置量）。
//
//   ## 边界归属（每条都有 selftest 钉住；四类抛两两错误串不同，可归因）
//   · 过零判据 = cov 与 t 的**严格符号变化**；恰等于 t 的样本归入「已过零」侧。
//   · 多个过零点 ⟹ **抛**，绝不返回第一个（本仓 skill「静默选择器」同族）。
//   · 常函数（gmax=0）⟹ 抛，错误串按电平给归因提示（≈全受光：影没画出来 / ≈全本影：
//     影没退出去或窗口放错了——旧「全 0 / 全 1」两条守卫的语义原样保留，换了触发路径）。
//   · 有效样本 < kEdgeMinSamples ⟹ 抛（分母塌了读数就没有意义，返回 0/NaN 都会被读成数）。
//
//   ## 级联模型的闭式（texel 尺寸的出处 = 公式，不是实测）
//   `edgeCascadeModel` 是引擎公式链的**独立第二实现**（逐式对应 rgl/scene_context.cppm 的
//   quantizeShadowDistance / cascadeSplitDistance / frustumBoundingSphere 与
//   render_list_impl.cppm 的 texelSizes = 2r/块边长）；与生成器（qa_scenes.py edge_frame）
//   各自实现、互不抄数，由仪器的对账闸门比**同一组 manifest 输入**下的预测——漂了即抛。
//   🔴 不许从图像反推 texel 尺寸（那又是拿渲染当参照）。
//
//   ## 变异红线（哪个变异让我红——全部由 `selftest_edge.cpp` 钉死；实跑记录见交付说明）
//   · 相对阈值改回绝对 0.5（G5-① 候选）→ 浅斜坡两条（0→0.30 / 0.70→1.00）红——**浅影
//     立刻不可量**，正是首跑 112 组合全灭的形态；深斜坡断言照绿（旧口径是新口径的特例）。
//   · 平台中位数改成最大值（G5-② 弱化候选）→ 浅斜坡定位红（umbra 侧取 max 抬高阈值）。
//   · 过渡高下界挪到 0（G5-③ 候选）→ 「过渡过浅必须抛」（0.30/0.31）红。
//   · 过零点插值退化成取整（返回 prev 的 s）→ 「闭式斜坡 <1e-9」红（给到 step 量级误差）。
//   · 多过零守卫删掉（返回第一个）→ 「双斜坡应抛」红——**这条是静默选错边的入口**。
//   · 常函数守卫删掉 → 全 0 / 全 1 两条 throwsStd 红。
//   · 0.5·gmax 走行阈值挪到 0（过渡区吞掉全窗）→ 「平台窗退化应抛」红。
//   · subpixel=false 分支写成插值（与 true 同码）→ 「粗模式落在 ±step/2」红。
//   · worldPerpMm 的视深写成常数 → 「闭式 mm/px」红（深度依赖没了 = 换机位静默换单位）。
//   · quantizeShadowDistance 写成向下取整 → 「量化向上取整方向」红（⚠ 首版缺这条时变异
//     实测存活 = 自检真缺口，2026-10-20 补非幂输入用例堵上）；practical split 的 mix 写成
//     0 或 1 → splits 两条闭式红；texel 分母写成 2048（图集整幅）→ 两条 texel 闭式红一半
//     （块边长 = 1024，snap 格同源——render_list_impl 的注释专门写过这条）。
//   · 闸门 C（`requireEveryPlateMeasured`）的空集合守卫删掉 → 「零臂/零板应抛」红
//     （对空集合的断言恒真 = 假绿，本仓 skill 明写）；missing 非空改成只报第一条 →
//     「两条同时缺一次报全」红。
//   · 闸门 D（`requireReferenceEdgeAtTruth`）的比较 `>` 反向成永不抛 → 「超界应抛」红；
//     弱化成 `>=` → 「恰等于界算通过」红（对称判据每半边一条）。
//
#include "imgqa.h"
#include "shadow_kernel_metric.h"  // Projector / makeProjector / projectPoint3（复用，别重写）

#include <opencv2/core.hpp>

#include <array>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {

/// 过渡高下界（相对阈值口径的「不是量化噪声」判据；🔴 低于它**抛**，不许返回一个
/// 噪声定位的「边」）。推导（截图 = 8bit sRGB，cov 在**线性域**）：
///  · 1 LSB 折进 cov 的量 = dLinear/dbyte ÷ L_none。dLinear/dbyte = (1/255)·2.4·
///    ((b/255+0.055)/1.055)^1.4/1.055，在 b=255 取最大 ≈ 0.0119；本靶接收面（日光地面）
///    实测 b≈146 ⟹ dLinear/dbyte ≈ 0.0044、L_none ≈ 0.29 ⟹ **1 LSB ≈ 0.015 cov**
///    （主 agent 复算的锚：cov≈0.37 处同量级）。
///  · 过零位置误差 δs ≈ W·δcov/h（W = 过渡宽、δcov ≈ 1 LSB 的平台/阈值估计误差）。
///    要求 δs ≤ W/10（定位误差 ≤ 过渡宽的 10%，仍远小于一个纹素的吸附量）⟹
///    **h ≥ 10·LSB ≈ 0.15**。本靶实测全部可量格 h ≈ 0.33（裕量 2.2×，见交付说明 G2 输出）。
/// ⚠ 锚在本靶接收面亮度（b≈146）：更暗的接收面 1 LSB 更大，换靶/换面必须重推。
inline constexpr double kEdgeTransitionMinH = 0.15;

/// 过渡区走行的停止阈值（0.5·gmax，无量纲）：线性斜坡梯度恒定 ⟹ 过渡区 = 斜坡本身
/// （selftest 的浅/深斜坡闭式都靠这条确定性）；平滑边缘取的是过渡核心，尾巴留给
/// 平台窗的中位数去抗。挪到 0 = 过渡区吞掉全窗 ⟹ 「平台窗退化」红。
inline constexpr double kEdgeWalkFraction = 0.5;

/// 平台窗最少样本数（中位数的最小稳健样本量；5 = 允许 ±2 个离群）。本靶最紧一格
/// （f3 × 近界板）实测上游平台 ≈ 8–11 个样本，界取 5 留裕量。
inline constexpr std::size_t kEdgePlateauMinSamples = 5;

/// 一条扫描线的最少有效样本数（分母守卫：4 个样本连一条斜坡都框不住）。
inline constexpr std::size_t kEdgeMinSamples = 5;

/// `worldPerpMm` 的数值 Jacobian 步长（米）。1e-4 m 下透视二阶项 ~1e-9 相对，
/// 远小于它要检测的信号（毫米级）；selftest 的闭式对拍钉这个量级。
inline constexpr double kEdgeJacEpsM = 1e-4;

/**
 * 一条世界扫描线（影边垂直于它 → 过零点 = 影边的世界位置）。
 * from/to 在地面 y=0 上；s = 沿 (to−from) 单位方向的米数（s=0 在 from）。
 * vOffsets = 沿 vDir 的平行子线（去噪平均，同 `kVLineOffsets` 的机关：影缘沿 v 是直的
 * ⟹ 沿 v 平均无偏，沿 s 平滑会把缘本身摊宽）。
 */
struct EdgeScanLine {
    std::array<double, 3> from{};
    std::array<double, 3> to{};
    std::array<double, 3> vDir{};       ///< 单位向量（世界 xz 平面内）
    std::vector<double> vOffsets;       ///< 米；至少 1 条（0 = 单线）
    double stepM = 0.004;               ///< 采样步长（米）
};

/// 剖面样本 (s, cov)。s 升序；投影失败的 s 整条丢弃（各 v 线全失败才算丢）。
/// 守卫：cov 非 CV_64FC1 / 线长 ≤ 0 / step ≤ 0 / vOffsets 空 / 有效样本 < kEdgeMinSamples
/// ⟹ **抛**（返回空剖面会被下游读成「没数」或「无交叉」，两种都静默）。
[[nodiscard]] std::vector<std::pair<double, double>> sampleCovProfile(const cv::Mat& cov,
    const Projector& proj, const EdgeScanLine& line);

/**
 * cov 沿扫描线的**自身过渡中点**（相对阈值，定义与算法步骤住头注「过零点 = 相对阈值」），
 * 返回 s（米，自 from 起）。
 * subpixel=true：括号样本对间线性插值（闭式斜坡的过零误差 < 1e-9，selftest 钉，
 * 深/浅斜坡各一条）；subpixel=false：返回括号样本的 s 中点（粗模式，只用于
 * 「插值确实在起作用」的对照）。
 * 守卫（一律抛，返回 NaN 会被读成数；四类错误串两两不同，可归因）：
 * 找不到梯度（常函数，按电平附「≈全受光 / ≈全本影」归因提示）/ 平台窗退化（一侧样本
 * 不足）/ 过渡过浅（h < kEdgeTransitionMinH）/ 相对阈值无过零 / 多个过零（点名各 s）。
 */
[[nodiscard]] double edgePosition(const cv::Mat& cov, const Projector& proj,
    const EdgeScanLine& line, bool subpixel = true);

/// 扫描线上 s 米处的世界点（from + dir·s；dir = (to−from) 单位化）。
[[nodiscard]] std::array<double, 3> edgeWorldAt(const EdgeScanLine& line, double s);

/**
 * 像素位移 → 世界毫米：沿 dir 过 p 的扫描方向，投影的局部 Jacobian（闭式，从机位算）。
 * 🔴 **不许从图像反推**（反标定 = 拿渲染当参照）。返回 = px 位移换算的世界 mm。
 * 守卫：dir 零向量 / p 投不到（相机背后或屏外）⟹ 抛。
 */
[[nodiscard]] double worldPerpMm(const Projector& proj, const std::array<double, 3>& p,
    const std::array<double, 3>& dir, double px);

// ── 级联闭式模型（引擎公式链的独立第二实现；对账闸门比它和 manifest 预测）──────────────
struct EdgeCascadeModel {
    double nearRaw = 0.0;   ///< projectionSystem 的 max(0.05, dist−R)
    double farRaw = 0.0;    ///< dist + R
    double nearQ = 0.0;     ///< quantizeShadowDistance(nearRaw)（向上取整到 2 的幂）
    double farQ = 0.0;      ///< 同（= shadowFar，末级淡出的分母）
    std::array<double, 4> splits{};  ///< practical split（mix=0.5, count=4）各级远截止
    std::array<double, 4> texelM{};  ///< 各级纹素 = 2·r_c/1024（米）
};

/// 从机位 + 画幅 + SceneBounds（引擎同款 unionSphere 后的球）闭式推级联模型。
/// 守卫：radius ≤ 0 / nearQ ≥ farQ（量化退化，texel 无从谈起）⟹ 抛。
[[nodiscard]] EdgeCascadeModel edgeCascadeModel(const Engine::Camera& cam, int cols, int rows,
    const std::array<double, 3>& boundsCenter, double boundsRadius);

/// 视深 → 级联号（shadow.slang 三段顺序比较的同口径：splits 逐个比，落第几级）。
[[nodiscard]] int edgeCascadeOf(const EdgeCascadeModel& m, double viewDepth);

/// 世界点的视深（沿视轴；= shadow.slang 选级用的 depth 同量）。
[[nodiscard]] double edgeViewDepth(const Projector& proj, const std::array<double, 3>& p);

// ── 闸门（抛 = rc 3，不产 Verdict；两台差分仪器「平凡通过的闸门比没有闸门更坏」同纪律）──

/**
 * **闸门 C｜每块板必须真的量到过**（对空集合的断言恒真 = 假绿的直接守卫）：
 * `missing` = (臂名, 板名, 算子抛因) 的**意外失踪**清单（预期出块由调用方按闭式预判分家，
 * 不进这里），非空 ⟹ **抛并一次报全**（逐条抛会把第二条藏到下一轮）。🔴 空清单**不够**——
 * 臂数或板数为 0 时 missing 天然为空，断言恒真：armCount == 0 / plateCount == 0 同样抛
 * （前提守卫，本仓 skill 明写的那条）。
 */
void requireEveryPlateMeasured(
    const std::vector<std::tuple<std::string, std::string, std::string>>& missing,
    std::size_t armCount, std::size_t plateCount);

/**
 * **闸门 D｜参照态自洽**（靶或换算错了 vs 阴影错了的分界，本仪器的立命锚点）：
 * 参照态 = `shadow_lod=-1` 自然渲染 与 `force_cascade=0` 各臂——闭式预期误差 ≈ 0
 * （级 0 恒画全量几何；LOD 关 = 全量几何进阴影图）。`entries` = (读数名, |误差| mm,
 * 界 mm)，界由调用方从闭式推（纹素 + 像素刻度，推导住仪器层）；任一条超界 ⟹ 抛并点名。
 * 边界归属：**恰等于界算通过**（只有严格 `>` 抛，与 seam 的 requireBandValid 同口径）。
 * 空清单 ⟹ 抛（恒真断言）。
 */
void requireReferenceEdgeAtTruth(
    const std::vector<std::tuple<std::string, double, double>>& entries);

}  // namespace RGL::imgqa
#endif  // RGL_IMGQA_SHADOW_EDGE_METRIC_H
