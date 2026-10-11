// temporal-tail --motion camera —— **机位运动**下的稳态鬼影份额（φ 扫描 / 运动归因）
// （批次 B11 从 `temporal_motion.cpp` 拆出：同一文件里的**第四台仪器**）。设计全文 =
// 下面原样搬来的头注释。闸门与判决的编排在 `temporal_tail_camera.cpp`（缝 = 采集 vs 判决）。
#include "temporal_motion.h"

#include "imgqa.h"
#include "temporal_residual.h"
#include "temporal_yaw_remap.h"

#include <algorithm>
#include <cmath>
#include <numeric>  // std::accumulate（阶跃主指标 AUC = Σ ghost(n)）
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
using namespace tmotion;  // 本族跨 TU 的类型 / 常量 / 算子（唯一出处 = 本文件的第一条 include）

namespace tmotion {

// ══════════════════════════════════════════════════════════════════════════════
// 批 13.14a `temporal-tail --motion camera`：**机位运动**下的稳态鬼影份额
// ══════════════════════════════════════════════════════════════════════════════
//
// ## 为什么必须有这条（不是"给 spin 版补个对称性"）
//
// 臂 11 的记忆门是 `gate = saturate(2 − motion)`，`motion = ‖prevUvBase − baseUv‖`
// = **每帧**位移。而 §1.14 缺陷 2 登记的洞是：鬼影是整个窗（≈100 帧）的**累积**位移，
// 慢速运动每帧 1.5px 落在斜坡上（gate≈0.5）但 100 帧累积 150px ⟹ 记忆半开 = 洞。
// **spin 模式下相机不动 ⟹ `prevUvBase ≡ baseUv` ⟹ gate 恒 1.0**（那正是"静止机位
// 与臂 8 逐位相同"的机制）⟹ 现有 spin 版仪器**原理上进不了 gate 的斜坡区**。
// 而批 13.14 把 α 恒写 0.01 的全部赌注就压在这个门上（§1.15 第 3 条）。
//
// ## 算子：分子根本不需要配准，只有分母需要
//
// 参考 = 同一段 yaw 运动的「TAA 关 + jitter_scale=0」渲染（基位置真值，同 spin 版）。
//
//   分子 = |out_j(p) − ref_j(p)|        ← **同帧同视点**，相机动不动都不影响它
//   分母 = Δ_j(p) = |ref_j(p) − ref_{j−1}(φ(p))|
//        = 历史被解析重投影搬到 p 之后、p 处本帧与历史的差 = **鬼影的驱动量本身**
//
//   frac = mean(Δ · |out−ref|) ÷ mean(Δ)   （内缩区内，Δ 加权）
//
// φ = 解析 yaw 映射（`temporal_yaw_remap`，**带 pitch**）。⚠ 这是解析映射不是图像
// 配准：没有搜索、没有阈值、没有 argmin ⟹ 不违反本仪器「整条链上不出现图像配准」的红线
// （`temporal-motion` 那两条常红的对齐护栏就是这条红线的由来）。
//
// ## 匀速运动下没有「尾长」，只有稳态份额
//
// spin 版找的是「刚让开」事件（`d_j > τ 且 d_{j−1} ≤ τ`）——相机匀速运动下**每个边缘
// 每帧都在变**，那个检测器要么不点火要么到处点火。匀速运动下鬼影是**稳态**的，
// 于是主指标从「衰减曲线」退化成「稳态份额」，闭式锚点跟着换：
//
//   一阶稳态运动模糊 `out = α·Σ_k (1−α)^k N_{j−k}∘φ^k` ⟹ **frac ≈ 1 − α**
//   （关盒短窗 α=0.12 ⟹ 0.88；关盒长窗 α=0.01 ⟹ 0.99）
//
// ## 判决线（跑前写死，四条 AND；红一条就不许报臂间结论）
//
//  1. `temporal-tail-pairing` **零锚点**：Z = TAA 开 + `feedback=1` + `jitter_scale=0`
//     α=1 ⟹ `lerp(·,cur,1) ≡ cur`；抖动 0 ⟹ 采样是恒等映射
//     ⟹ Z 与参考只差一次 Reinhard 往返。**线：frac(Z) ≤ 0.02**。
//     相机运动下这条**更硬**：配对错一帧 ⟹ yaw 已转过 ⟹ frac 立刻跳。
//  2. `temporal-tail-smear-closed-form` **闭式锚点**：关盒短窗（B12）frac 必须落在
//     `1−α = 0.88` 附近（带 [0.75, 0.99]）。不成立 ⟹ frac 量的不是运动模糊，
//     整张臂表不许读。**这条是「仪器在量它声称的东西」的唯一直接证据。**
//  3. `temporal-tail-static-collapse` **反向**：`δ = 0`（相机静止）时同一算子必须
//     塌到 frac ≈ 0（画面逐帧静止 ⟹ 分子恒 0）。不塌 = 量的是噪声/配准残差。
//  4. `temporal-tail-gate-breakpoint` **阳性锚点（本仪器存在的理由）**：把每帧像素位移
//     扫过 `gate` 的**推导断点 motion = 2px**，`frac(臂11) − frac(臂0)` 必须
//     **低角度侧显著为正**（慢速运动 ⟹ gate 半开 ⟹ 记忆半开 = 洞）、
//     **高角度侧 ≈ 0**（motion ≥ 2 ⟹ gate 关门 ⟹ 臂 11 退回臂 0）。
//     断点位置是推出来的不是扫出来的 ⟹ 它是阳性锚点不是拟合。
//
// ⚠ **remap 的插值模糊混在分母里**（双线性 warp 让 Δ 偏小 ⟹ frac 偏大）。它是
//   **跨臂共同的加性偏置**（同一参考、同一 warp），故臂间**比值**不受影响；
//   绝对值与闭式 1−α 的偏离里有一部分是它。出结论按比值读，别把绝对值当份额。
// ⚠ **边界内缩**：宽度 = 每帧位移 × burst + 2px，排掉「新内容入画」（那不是鬼影），
//   同时盖住 remap 的 BORDER_CONSTANT 带。宽度不够 = 把入画读成鬼影。
// ⚠ 单实例串行（同 spin 版）。

/// 内缩掩膜：排掉「新内容入画」的边带 + remap 的 BORDER_CONSTANT 带。
cv::Mat smearInnerMask(cv::Size size, double shiftPerFrame, int frames)
{
    const int pad = static_cast<int>(std::ceil(shiftPerFrame * frames)) + 2;
    if (2 * pad >= size.width || 2 * pad >= size.height)
        throw std::runtime_error("内缩后为空：每帧位移 × burst 超过画面尺寸（pad = "
            + std::to_string(pad) + "）⟹ 换小 --angle 或减小 --burst");
    cv::Mat m = cv::Mat::zeros(size, CV_8UC1);
    m(cv::Rect(pad, pad, size.width - 2 * pad, size.height - 2 * pad)).setTo(255);
    return m;
}

/**
 * Δ 加权的稳态鬼影份额。配对 = **帧计数器之差**（整数常量，零阈值，同 spin 版）。
 *
 * 权重门 = `Δ > τ`：只在**确有驱动量**的像素上统计（大片平坦区 Δ≈0，放进去会把
 * frac 稀释成 0 而看不出任何事）。τ 因此是本算子唯一的自由阈值 ⟹ 调用方必须出
 * 三档敏感性表（同 spin 版纪律）。
 *
 * ⚠ `jlo/jhi` 必须是与偏移无关的常数区间（spin 版 `measureTail` 记的同一条坑：各臂
 *   各裁各的帧 ⟹ 比的是不同事件子集 ⟹ 臂间比值不可解释）。偏移覆盖不到就抛，不裁帧。
 */
/**
 * 预算每个 j 的 `Δ_j = |ref_j − ref_{j−1}∘φ|`。
 *
 * ⚠ **与臂、与帧号偏移都无关**（只依赖参考与 φ）⟹ 全表只算一次。这不只是省时间：
 * 它保证「各臂评在逐位相同的权重图上」，而 remap 的双线性插值偏置也因此跨臂逐位相同。
 */
std::vector<cv::Mat> buildSmearDeltas(const Group& ref, const YawMap& map, int jlo, int jhi)
{
    std::vector<cv::Mat> out;
    out.reserve(static_cast<size_t>(jhi - jlo + 1));
    for (int j = jlo; j <= jhi; ++j) {
        cv::Mat d;
        cv::absdiff(ref.luma[static_cast<size_t>(j)],
            remapWith(ref.luma[static_cast<size_t>(j - 1)], map), d);
        out.push_back(std::move(d));
    }
    return out;
}

/**
 * Δ 加权的稳态鬼影份额的**内核**：`off` 由调用方给定（= 候选相对参考的 burst 起始帧号之差）。
 *
 * 权重门 = `Δ > τ`：只在**确有驱动量**的像素上统计（大片平坦区 Δ≈0，放进去会把
 * frac 稀释成 0 而看不出任何事）。τ 因此是本算子唯一的自由阈值 ⟹ 调用方必须出
 * 三档敏感性表（同 spin 版纪律）。
 *
 * `meanResid` 是**分子零点**读数：不门控、不加权、与 Δ 无关。frac 红的时候靠它分家：
 * 分子本来就非零（配对/视差问题）⟺ 分子零但分母不对（φ/τ 问题）。
 *
 * ⚠ `jlo/jhi` 必须是与偏移无关的常数区间（spin 版 `measureTail` 记的同一条坑：各臂
 *   各裁各的帧 ⟹ 比的是不同事件子集 ⟹ 臂间比值不可解释）。偏移覆盖不到就抛，不裁帧。
 */
SmearRow measureSmearAt(const std::string& tag, double clip, const Group& ref, const Group& cand,
    const std::vector<cv::Mat>& deltas, const cv::Mat& inner, double tau, int jlo, int jhi,
    long off)
{
    SmearRow r;
    r.tag = tag;
    r.clip = clip;
    r.offset = off;
    const int nC = static_cast<int>(cand.luma.size());
    const int n = jhi - jlo + 1;
    double num = 0.0, den = 0.0, res = 0.0;
    for (int j = jlo; j <= jhi; ++j) {
        const long i = static_cast<long>(j) - off;  // 候选里同一绝对帧
        if (i < 0 || i >= nC)
            throw std::runtime_error(tag + "：帧号偏移 " + std::to_string(off)
                + " 超出内部窗口（burst 要加大或 margin 要加宽；**不要改成静默裁帧**）");
        const cv::Mat& delta = deltas[static_cast<size_t>(j - jlo)];
        cv::Mat resid, prod, gate;
        cv::absdiff(cand.luma[static_cast<size_t>(i)], ref.luma[static_cast<size_t>(j)], resid);
        cv::multiply(delta, resid, prod);
        cv::compare(delta, tau, gate, cv::CMP_GT);
        cv::bitwise_and(gate, inner, gate);
        num += cv::mean(prod, gate)[0];
        den += cv::mean(delta, gate)[0];
        res += cv::mean(resid, inner)[0];
        r.events += static_cast<double>(cv::countNonZero(gate));
    }
    r.meanDelta = den / static_cast<double>(n);
    r.meanResid = res / static_cast<double>(n);
    r.frac = den > 0.0 ? num / den : 0.0;
    return r;
}

/**
 * 候选相对参考的**配对偏移**（= 候选要错开几帧才与参考同一视点）。
 *
 * 🔴 **为什么不能用 `framePre` 之差**（首轮实测它在各臂间是 −12…−1，而绝对偏移宽扫的
 * argmin 落在 0 附近）。根因写在引擎自己的注释里（`rgl/camera.cppm`
 * `applyFrameDrivenYaw` 头注释，逐字）：
 *   「任何外部写入（RPC `POST /camera`…）都会在下一次 cameraSystem 被识别并以外部值重定基
 *    ……重定基后跨进程仍只差一个**常数帧偏移**（RPC 落在第几帧由启动时序决定）」
 * 即 `eng.setCamera()` 之后 `yaw(n) = yaw_written + dYaw·(n − F)`，`F` = 那次写入落在第几帧。
 * 于是真配对 = `off_framePre − ΔF`，而 **`F` 恰好是渲染帧计数器给不出的那个量**。
 * 引擎为这条给出的既有解法是「temporal-motion 的逐帧 argmin 对齐」= **图像配准**，
 * 而本命令的设计红线正是不出现图像配准。
 *
 * ✅ 第三条路：`POST /camera` 空 body 回显的就是**当前真实 yaw**，在 burst 起始读一次
 * 就把 `F` 吸收掉了（回显是只读的，不触发重定基 ⟹ **不改变相位**）。仍然是
 * 「引擎报真值 + 解析换算」，红线守住。
 *
 * 用错偏移的读数形态值得记一笔：错位 ≥4 帧后两幅画面在边缘上**去相关 ⟹ 差值饱和**，
 * 于是 frac 变成一条**平线**，看起来像「分子与配对无关」（本轮就是这样误读了一整轮），
 * 其实是扫的区间根本没盖住真值。
 *
 * @param yawPerFrameDeg 实测每帧角；≈0（δ=0 静止组）时返回 0：所有帧同视点，偏移无定义。
 */
long yawPairOffset(const Group& ref, const Group& cand, double yawPerFrameDeg)
{
    if (std::abs(yawPerFrameDeg) < 1e-9)
        return 0;
    // 折到 (−180, 180]：不折的话跨 0°/360° 边界会跳 360/每帧角 ≈ 上千帧
    double d = std::fmod(cand.yawPre - ref.yawPre, 360.0);
    if (d > 180.0)
        d -= 360.0;
    if (d < -180.0)
        d += 360.0;
    return std::lround(d / yawPerFrameDeg);
}


/// 除零安全的比值。写成函数而不是各处内联三元：这类守卫本该只有一份，而打表函数的
/// 认知复杂度上限很紧（style-check 点名的是 `printSmearTables`）。
double safeRatio(double num, double den) { return den > 0.0 ? num / den : 0.0; }

/// 「分母那一整块被搬掉了百分之几」，分母为 0 时给 **0**（不是 100——rawΔ=0 是「画面没动」，
/// 把它打成「φ 搬掉 100%」会被读成映射完美，方向正好反了）。
double percentRemoved(double num, double den) { return den > 0.0 ? 100.0 * (1.0 - num / den) : 0.0; }

/// 取主档读数（`oneFrameFrac` 在「所有帧都被内缩掉」的极端 burst 下会空）。默认值给调用方。
double mainTauValue(const std::vector<double>& v, double fallback)
{
    return v.size() > kMainTau ? v[kMainTau] : fallback;
}

/// 往返地板 `mean|I − φ⁻¹(φ(I))|`（口径与 `rawDelta`/`alignedDelta` 一致：内缩区无条件均值）。
/// 抽成独立函数的理由：① `collectSmearAngle` 只管编排（style-check 的认知复杂度上限）；
/// ② 这一列从此可以脱离主表被单独复用（例如给 `--motion spin` 也配一把同样的插值尺子）。
double roundtripFloorOf(const Group& ref, const YawMap& fwd, const YawMap& back, int jlo, int jhi,
    const cv::Mat& inner)
{
    double rt = 0.0;
    long n = 0;
    for (int j = jlo; j <= jhi; ++j) {
        const cv::Mat& src = ref.luma[static_cast<size_t>(j)];
        cv::Mat d;
        cv::absdiff(src, remapWith(remapWith(src, fwd), back), d);
        rt += cv::mean(d, inner)[0];
        ++n;
    }
    return n > 0 ? rt / static_cast<double>(n) : 0.0;
}

/**
 * 一档角度的采集 + 三档 τ 测量。**采集只做一次**（τ 只是换权重门）。
 *
 * 🔴 `mapAngleRad` 与 `angleRad` **分开传**，这个区分就是判决线 3 的全部价值：
 * 反向组要「相机静止（`angleRad = 0`）但**借主角度的 φ**」。若用 δ=0 的 φ，那是恒等映射
 * ⟹ 静止画面的 Δ ≡ 0 ⟹ `frac = 0/0` 被兜底成 0 ⟹ **一条永远绿的空断言**。借主角度 φ 后
 * Δ = 「画面平移 ≈3px 后的边缘」= 鬼影本该出现的位置，「分子必须塌」才是真判据。
 * 其余调用两者相同。
 */
SmearAngle collectSmearAngle(const std::string& base, const BistroCam& cam, int burst, int warm,
    double angleRad, double mapAngleRad, double tau, int margin,
    const std::vector<SmearArm>& arms, const std::string& suffix)
{
    auto collect = [&](const SmearArm& a, double yaw) {
        Spec sp;
        sp.aa = a.taa ? Aa::Taa : Aa::Off;
        sp.feedback = a.feedback;
        sp.jitterScale = a.jitter;
        sp.baseSpace = true;
        sp.clipMode = a.clip;
        sp.cameraMotion = true;  // ← 与 spin 版唯一的采集差异：**动的是相机**
        sp.motionAngle = yaw;
        sp.warm = warm;
        return collectGroup(base + "_" + suffix + a.tag, suffix + a.tag, suffix + a.tag, cam, sp,
            burst);
    };

    std::printf("── 采集 %s（串行，单实例纪律）──\n", suffix.c_str());
    std::vector<Group> groups;
    groups.reserve(arms.size());
    for (const SmearArm& a : arms) {
        groups.push_back(collect(a, angleRad));
        std::printf("  %-5s 帧号 [%u, %u)  taa=%d clip=%g fb=%g jitter=%g\n", a.tag,
            groups.back().framePre, groups.back().framePost, static_cast<int>(a.taa), a.clip,
            a.feedback, a.jitter);
    }
    const Group& ref = groups.front();
    if (arms.front().taa)
        throw std::runtime_error("collectSmearAngle：arms[0] 必须是 TAA 关的参考组");

    // 解析映射（**带 pitch**，pitch/fov 由引擎回显）。⚠ focalPx 传 fx，而本项目
    // fovDegrees 是**垂直** fov 且水平由 aspect 推 ⟹ fx = fy = (H/2)/tan(fov_v/2)。
    const double pi = 3.14159265358979323846;
    const cv::Size sz = ref.luma.front().size();
    const double focal = sz.height / (2.0 * std::tan(ref.fovDegrees * pi / 360.0));
    const cv::Matx33d H = yawRemapHomography(mapAngleRad, ref.pitchDegrees * pi / 180.0, focal,
        sz.width / 2.0, sz.height / 2.0);
    const YawMap map = buildYawMap(H, sz);
    std::printf("  解析映射：pitch=%g° fov=%g° → 焦距 %gpx，每帧位移 中心 %g / 最大 %gpx\n",
        ref.pitchDegrees, ref.fovDegrees, focal,
        std::hypot(static_cast<double>(map.mapX.at<float>(sz.height / 2, sz.width / 2))
                      - sz.width / 2.0,
                  static_cast<double>(map.mapY.at<float>(sz.height / 2, sz.width / 2))
                      - sz.height / 2.0),
        map.maxShiftPx);

    SmearAngle out;
    out.maxShiftPx = map.maxShiftPx;
    out.pitchRad = ref.pitchDegrees * pi / 180.0;
    out.focalPx = focal;
    out.jlo = margin + 1;  // 要取 j−1
    out.jhi = static_cast<int>(ref.luma.size()) - 2 - margin;
    out.centerShiftPx = std::hypot(
        static_cast<double>(map.mapX.at<float>(sz.height / 2, sz.width / 2)) - sz.width / 2.0,
        static_cast<double>(map.mapY.at<float>(sz.height / 2, sz.width / 2)) - sz.height / 2.0);
    const cv::Mat inner = smearInnerMask(sz, map.maxShiftPx, burst);

    // 往返地板 `|I − φ⁻¹(φ(I))|`（见 `SmearAngle::roundtripFloor`）。用**参考帧**算（与臂无关）
    // ⟹ 每个角度只算一次。逆映射取 `H.inv()` 而不是再构造一次 `H(−δ)`，是为了让「往返」在数值上
    // **恰好**是同一个映射的正逆——若哪天 `yawRemapHomography` 不再是群元素，这一列会突然变大，
    // 本身就是有效哨兵。
    out.roundtripFloor = roundtripFloorOf(ref, map, buildYawMap(H.inv(), sz), out.jlo, out.jhi, inner);

    // ── **配对偏移 = 引擎回显的 yaw 之差 ÷ 实测每帧角**（不是帧计数器之差！）──────────
    // 首轮实测：`framePre` 之差在各臂间是 −12…−1，而绝对偏移宽扫的 argmin 落在 0 附近
    // （A11 在 off+1 处 frac 从 0.185 掉进 0.073 的深谷）⟹ **渲染帧计数器 ≠ yaw 帧索引**。
    // 换成 yaw 之后仍然是「引擎报真值 + 解析换算」，**没有引入图像配准**（本命令红线）。
    // 每帧角用 burst 前后两次 yaw 回显**实测**（不是配置值）：它同时是第二把在场性尺子
    // ——实测 ≈ 0 而配置 ≠ 0 ⟺ `frame_yaw` 没生效（键名在 lua 侧拼错是**静默**的）。
    const double tick = std::max<long>(1,
        static_cast<long>(ref.framePost) - static_cast<long>(ref.framePre));
    out.yawPerFrameDeg = (ref.yawPost - ref.yawPre) / tick;
    std::printf("  在场性（引擎侧）：实测每帧 yaw %.6g° vs 配置 %.6g°（tick=%ld 帧）\n",
        out.yawPerFrameDeg, std::abs(mapAngleRad) < 1e-12 ? 0.0 : std::abs(mapAngleRad * 180.0 / pi),
        static_cast<long>(tick));
    out.pairOff.assign(groups.size(), 0);
    for (size_t gi = 1; gi < groups.size(); ++gi)
        out.pairOff[gi] = yawPairOffset(ref, groups[gi], out.yawPerFrameDeg);

    // ── **共用内部窗口 = 所有臂都合法的 j 区间** ────────────────────────────────
    // 窗口要同时满足
    //     j ≥ 1（要取 j−1）、j ≤ nR−1、0 ≤ j−off_k < nC_k ⟹ j ∈ [off_k, nC_k−1+off_k]
    // 取**全体臂的交集**而不是各臂各裁：不变量是「所有臂评在逐位相同的帧集上」，
    // 交集保住它，逐臂裁帧摧毁它（spin 版 `measureTail` 记的同一条坑）。
    // 交集为空 ⟹ 抛（不静默放宽）：此时样本数已不可信。
    long minOff = 0, maxOff = 0;
    long upper = static_cast<long>(ref.luma.size()) - 1;
    for (size_t gi = 1; gi < groups.size(); ++gi) {
        const long off = out.pairOff[gi];
        out.jlo = static_cast<int>(std::max<long>(out.jlo, off));
        upper = std::min(upper, static_cast<long>(groups[gi].luma.size()) - 1 + off);
        minOff = std::min(minOff, off);
        maxOff = std::max(maxOff, off);
    }
    out.jhi = static_cast<int>(std::min<long>(out.jhi, upper));
    if (out.jhi <= out.jlo)
        throw std::runtime_error("共用窗口为空：配对偏移 [" + std::to_string(minOff) + ", "
            + std::to_string(maxOff) + "] 已吃满 burst=" + std::to_string(burst)
            + "（加大 --burst 或减小 --margin）");
    std::printf("  共用窗口 j ∈ [%d, %d]（%d 帧，各臂逐位相同）· 内缩 %dpx · 配对偏移 [%ld, %ld]\n",
        out.jlo, out.jhi, out.jhi - out.jlo + 1,
        static_cast<int>(std::ceil(map.maxShiftPx * burst)) + 2, minOff, maxOff);

    out.deltas = buildSmearDeltas(ref, map, out.jlo, out.jhi);
    out.inner = inner;
    // **在场性自证**（不带 φ）。⚠ 必须与 `alignedDelta` **同口径**（都是内缩区**无条件**均值）：
    // 首轮我拿门控的 `meanΔ`（只在 Δ > τ 的像素上取均值）去除无条件 rawΔ，得到
    // 「φ 搬掉 −302.7%」的**假警报** —— 门控天然只留高 Δ 像素，两个均值根本不可比。
    double raw = 0.0, aligned = 0.0;
    for (int j = out.jlo; j <= out.jhi; ++j) {
        cv::Mat d;
        cv::absdiff(ref.luma[static_cast<size_t>(j)], ref.luma[static_cast<size_t>(j - 1)], d);
        raw += cv::mean(d, inner)[0];
        aligned += cv::mean(out.deltas[static_cast<size_t>(j - out.jlo)], inner)[0];
    }
    const int nFrames = out.jhi - out.jlo + 1;
    out.rawDelta = raw / static_cast<double>(nFrames);
    out.alignedDelta = aligned / static_cast<double>(nFrames);
    const double taus[] = { 0.02, tau, 0.10 };
    for (double t : taus) {
        std::vector<SmearRow> row;
        for (size_t ai = 1; ai < arms.size(); ++ai)
            row.push_back(measureSmearAt(arms[ai].tag, arms[ai].clip, ref, groups[ai], out.deltas,
                out.inner, t, out.jlo, out.jhi, out.pairOff[ai]));
        out.byTau.push_back(std::move(row));
        // 自标定刻度：把「候选 = 过期一帧的参考」当单位 1（见 `oneFrameFrac` 注释）
        double sq = 0.0, ln = 0.0;
        for (int j = out.jlo; j <= out.jhi; ++j) {
            const cv::Mat& delta = out.deltas[static_cast<size_t>(j - out.jlo)];
            cv::Mat gate, sq2;
            cv::compare(delta, t, gate, cv::CMP_GT);
            cv::bitwise_and(gate, inner, gate);
            cv::multiply(delta, delta, sq2);
            sq += cv::mean(sq2, gate)[0];
            ln += cv::mean(delta, gate)[0];
        }
        out.oneFrameFrac.push_back(ln > 0.0 ? sq / ln : 0.0);
    }
    out.groups = std::move(groups);  // ⚠ 必须最后搬：上面还在用 `ref`（= groups.front()）
    return out;
}

/// 臂 11 记忆门在给定**每帧**位移处的取值——逐字照抄
/// `rgl/shaders/taa.frag.slang` 的 `saturate(2.0 − length((prevUvBase − baseUv)/texel))`，
/// 取中心处代表值（全图最大值在画面边缘，被透视收缩放大，拿它对断点会低估所处一侧）。
double gateAt(double motionPx)
{
    return std::clamp(2.0 - motionPx, 0.0, 1.0);
}

/// 该位移落在记忆门的**哪一侧**。⚠ 斜坡宽 1px（`gate = 1` 要 motion ≤ 1，`= 0` 要 ≥ 2），
/// 所以「比断点小」**不够**——落在 `[1,2]` 里既不是门全开也不是门全关。本轮的教训正是
/// 把「低角度侧」取在 1.34px（= 斜坡中，gate 0.66）却当「门全开」读。
const char* gateSide(double motionPx)
{
    if (motionPx <= 1.0)
        return "门**全开** = 洞该在场";
    if (motionPx >= 2.0)
        return "门**全关** = 臂 11 该退回臂 0";
    return "**斜坡中**（既非全开也非全关，不许当两侧之一用）";
}

/// 打印主表 + τ 敏感性表。抽成独立函数的理由 = `cmdTemporalTailCamera` 的认知复杂度上限，
/// 但顺带的好处是：主表与敏感性表**必须**成对出现（τ 是唯一自由阈值 ⟹ 只出主表 = 违规），
/// 放在一起打印让「漏出敏感性表」在结构上不可能发生。
void printSmearTables(const std::vector<SmearArm>& arms, const SmearAngle& a, double tau)
{
    std::printf("\n── 主表：稳态鬼影（每帧最大位移 %gpx）──\n", a.maxShiftPx);
    // 🔴 记忆门落在哪一侧**必须打在表上**（本轮教训）：判决线写「低角度侧 vs 高角度侧」，
    //    而"低"若取在斜坡中间，阳性锚点会被读成「洞不存在」。采样点与断点的关系不出现在
    //    输出里，就一定会被误读——同「τ 敏感性表必须与主表成对」是同一条纪律。
    std::printf("  记忆门（臂 11）：gate = saturate(2 − motion)，斜坡 motion ∈ [1,2]px ⟹ 中心 "
        "%gpx → gate %g（%s）\n",
        a.centerShiftPx, gateAt(a.centerShiftPx), gateSide(a.centerShiftPx));
    // 在场性与刻度两行**永远先出**：frac 在任何别的读数之前先要成立「画面在动 + φ 搬得动 +
    // 读数有单位」。
    if (a.byTau.size() >= 2 && !a.byTau[1].empty()) {
        std::printf("  在场性：rawΔ %.6f（不带 φ）vs alignedΔ %.6f（带 φ）⟹ φ 搬掉 %.1f%%\n",
            a.rawDelta, a.alignedDelta, percentRemoved(a.alignedDelta, a.rawDelta));
        std::printf("  ⟹ rawΔ≈0 = 画面没动（`frame_yaw` 没生效）；alignedΔ>rawΔ = φ 比恒等还差\n");
        // 🔴 往返地板与 alignedΔ 的比值决定**这台仪器在这台机器上有没有东西可量**：≈1 ⟹ alignedΔ
        //    全是双线性插值误差。`temporal-homography` 的教训原话：「与残差同量级 ⟹ 单看残差会把
        //    量具当闪烁」。本机位仪器首轮没有这一列，于是「所有臂 ≈ 0.76 帧」被当成鬼影读了三轮。
        std::printf("  往返地板 |I−φ⁻¹(φ(I))| = %.6f ⟹ alignedΔ 高出地板 %.2f×"
            "（≈1 ⟹ 剩下的全是重采样误差，**无可归因时域残差**，臂间结论 = 读插值）\n",
            a.roundtripFloor, safeRatio(a.alignedDelta, a.roundtripFloor));
        std::printf("  刻度：「恰好过期一帧」≡ %.6f ⟹ 下表「几帧」列 = frac ÷ 它 = **相当于几帧过期历史**\n",
            mainTauValue(a.oneFrameFrac, 0.0));
    }
    std::printf("  %-5s %10s %12s %10s %12s %12s %14s\n", "臂", "clip", "frac", "几帧", "meanΔ",
        "meanResid", "样本数");
    // byTau[1] = 主档 τ（三档顺序 = {0.02, tau, 0.10}，在 collectSmearAngle 里定）
    const double unit = a.oneFrameFrac.size() > 1 ? a.oneFrameFrac[1] : 1.0;
    for (const SmearRow& r : a.byTau[1])
        std::printf("  %-5s %10g %12.4f %10.2f %12.6f %12.6f %14ld\n", r.tag.c_str(), r.clip,
            r.frac, unit > 0.0 ? r.frac / unit : 0.0, r.meanDelta, r.meanResid,
            static_cast<long>(r.events));
    std::printf("  %-5s %10s %12s %10s %12s %12s %14s\n", "REF", "-", "(参考)", "0", "-", "-", "-");

    const double taus[] = { 0.02, tau, 0.10 };
    std::printf("\n── τ 敏感性（三档方向必须一致）──\n");
    std::printf("  %-5s", "臂");
    for (double t : taus)
        std::printf(" %10s", ("τ=" + std::to_string(t)).c_str());
    std::printf("\n");
    for (size_t ai = 0; ai + 1 < arms.size(); ++ai) {
        std::printf("  %-5s", arms[ai + 1].tag);  // arms[0] 是参考，不参与
        for (size_t ti = 0; ti < std::size(taus); ++ti)
            std::printf(" %10.4f", a.byTau[ti][ai].frac);
        std::printf("\n");
    }
}

/**
 * 诊断（**不是判决，不进门禁**）：frac 红的时候用它把病因分开。
 *
 *  1. **分子零点** `meanResid`：与 Δ 无关的 mean|out−ref|。δ=0 那组必须 ≈ 0；
 *     若主角度组的 meanResid 也 ≈ δ=0 组 ⟹ 分子没病，红在分母（φ 或 τ）。
 *  2. **绝对偏移宽扫**：本仪器的配对前提是一句 ——「**同一绝对帧号 = 同一视点**」。
 *     首轮实测把它**证伪了**：`framePre` 之差在各组间是 −12…−1，而宽扫的 argmin 落在 0
 *     附近（A11 在 off+1 处 frac 从 0.185 掉到 0.073 的深谷）。形态学解释：两幅错开 ≥4 帧
 *     的画面在边缘上早已**去相关 ⟹ 差值饱和**，所以「远离真值」的读数是一条平线，
 *     看起来像「分子与配对无关」，其实是「扫的区间根本没盖住真值」。
 *     ⟹ 扫描必须按**绝对偏移**宽扫（而不是围绕 `framePre` 之差 ±2），否则仪器坏的时候
 *     读起来像仪器在响。
 *
 * 扫描复用**同一批已采数据**（`SmearAngle::groups`）——重新采集就不是在问
 * 「配对对不对」，而是在问「重跑稳不稳」，那是另一个问题（债-61）。
 */
void printSmearDiagnostics(const std::vector<SmearArm>& arms, const SmearAngle& a, double tau)
{
    constexpr int kScan = 16;  ///< 绝对偏移扫描半径（帧）；要 > 实测的帧号抖动
    std::printf("\n── 诊断：绝对偏移宽扫（argmin = 实测配对；不进门禁）──\n");
    std::printf("  %-5s %6s %8s %10s %10s  %s\n", "臂", "off", "argmin", "frac(off)",
        "frac(min)", "读法");
    for (size_t ai = 1; ai < arms.size(); ++ai) {
        const Group& cand = a.groups[ai];
        const long off = a.pairOff[ai];  ///< 仪器采用的配对（yaw 回显解析算出）
        const long nC = static_cast<long>(cand.luma.size());
        double atOff = 0.0, best = 1e300;
        long bestOff = off;
        for (long d = -kScan; d <= kScan; ++d) {
            // 越界 = 该偏移下 burst 覆盖不到共用窗口（不是错误，跳过）
            if (static_cast<long>(a.jlo) - d < 0 || static_cast<long>(a.jhi) - d > nC - 1)
                continue;
            const SmearRow r = measureSmearAt(arms[ai].tag, arms[ai].clip, a.groups[0], cand,
                a.deltas, a.inner, tau, a.jlo, a.jhi, d);
            if (d == off)
                atOff = r.frac;
            if (r.frac < best) {
                best = r.frac;
                bestOff = d;
            }
        }
        std::printf("  %-5s %6ld %8ld %10.4f %10.4f  %s\n", arms[ai].tag, off, bestOff, atOff, best,
            bestOff == off ? "off 即真值" : "⚠ framePre 之差不是配对");
    }
    std::printf("  ⟹ 读法：argmin 全等于 off ⟹ 配对前提成立，红在别处；否则**先修配对**，"
        "本轮所有 frac 作废\n");
}

/**
 * φ 标定诊断（**不是判决**）：扫映射角度的**倍数**，报**无条件** `alignedΔ`。
 *
 * 为什么必须有：`yawRemapHomography` 对金标准对拍到 2.3e-13px，**但金标准是我自己搭的**
 * （`selftest.cpp` `YawRemapFixture::truth`）——实现与金标准**共享同一个符号约定**时，
 * 对拍再准也测不出符号反了。首轮就是这种情形：同口径的 `alignedΔ` 比 `rawΔ`（完全不对齐）
 * 还大 29%，而单测全绿。⟹ **自搭金标准只能钉「实现 == 我的模型」，钉不了「模型 == 引擎」。**
 *
 * 读法（argmin 所在倍数）：`+1` = 映射对；`−1` = **符号反了**；`0` = 映射没起作用
 * （focal/pitch 传错）；`+0.5`/`+2` = 尺度错。⚠ 曲线在 argmin 附近不对称是正常的
 * （图像差值对位移**饱和**），所以只读 argmin，不读曲率。
 *
 * **返回 argmin 倍数**，由 `cmdTemporalTailCamera` 落成注册判据 `temporal-tail-phi-sign`。
 * 升格的理由：本文件第一轮就是靠它抓到 `yawRemapHomography` 的 yaw 反号（argmin = −1），
 * 而**单测全绿**——自搭金标准与实现共享同一个朝后的相机基，对拍到 2.3e-13px 也测不出
 * 「模型 ≠ 引擎」。⟹ 这一半只有实机数据能守，而文档只记录不阻止（skill
 * `rgl-test-discipline`：防线是测试和护栏）。
 */
double printSmearPhiCalibration(const SmearAngle& a, double mapAngleRad)
{
    static const double kScales[] = { -2.0, -1.0, -0.5, 0.0, 0.5, 1.0, 1.5, 2.0 };
    const Group& ref = a.groups[0];
    const cv::Size sz = ref.luma.front().size();
    std::printf("\n── 诊断：φ 标定（扫映射角度倍数 · 无条件 alignedΔ；不进门禁）──\n");
    std::printf("  %-9s", "倍数");
    for (double s : kScales)
        std::printf(" %10.2f", s);
    std::printf("  %s\n", "argmin");
    std::printf("  %-9s", "alignedΔ");
    double best = 1e300, bestScale = 0.0;
    for (double s : kScales) {
        const YawMap m = buildYawMap(
            yawRemapHomography(mapAngleRad * s, a.pitchRad, a.focalPx, sz.width / 2.0,
                sz.height / 2.0),
            sz);
        double acc = 0.0;
        for (int j = a.jlo; j <= a.jhi; ++j) {
            cv::Mat d;
            cv::absdiff(ref.luma[static_cast<size_t>(j)],
                remapWith(ref.luma[static_cast<size_t>(j - 1)], m), d);
            acc += cv::mean(d, a.inner)[0];
        }
        acc /= static_cast<double>(a.jhi - a.jlo + 1);
        std::printf(" %10.6f", acc);
        if (acc < best) {
            best = acc;
            bestScale = s;
        }
    }
    std::printf("  %+.2f\n", bestScale);
    std::printf("  ⟹ +1 = 映射对；−1 = **符号反了**；0 = 映射没起作用；≠1 的分数 = 尺度错。"
        "对照 rawΔ = %.6f（完全不对齐）\n", a.rawDelta);
    return bestScale;
}

}  // namespace tmotion

}  // namespace RGL::imgqa
