// rig_shadow_field.cpp —— izb_rig 影场仪器族：izb（消费域寻址一致性）+ swim（影场世界游动）。
//
// ## 缝在哪
//
// 缝 = **同一台影场仪器**。两条命令都在 `izb_rig.glb` 上做同一套采集（`shadowPass=true` +
// deferred 图，izb 再叠 `izbShadow`），都在问"影子画在了它该在的地方吗"，只是一个
// 从**寻址**问（mask↔画面三方对拍）、一个从**世界不变性**问（两机位重投影对平移）。默认
// `--glb` 同为 izb_rig.glb（见 cli_table.cpp），改动这两条判据的动机（换影场口径 / 换机位）
// 从来是同一件事。
//
// ## 搬动的纪律（纯重构）
//
// `cmdIzb` 的函数体与容差注释从 main.cpp **整块照搬**，一个字节未改。`cmdSwim` 同块照搬之后
// 就地降认知复杂度（护栏：38 > 30 error 级，其中内联 lambda 独立计 26 > 15）：把"阈值 /
// 重投影 / 投票 / 多数票 / 平移搜索"五段各抽成有名小函数，**判据阈值（p95×0.3、±10 格、
// 覆盖率 8/10、max(20, 共同/200)、5cm 格）与全部打印逐字未动**，切法见各函数上方注释。
//
// 两条命令的**声明在 imgqa.h**（本仓惯例：实现分散、声明集中，不为拆 TU 另设声明头）。
#include "imgqa.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

// ── izb：IZB 消费域判据（mask↔画面寻址一致性；11.3 补账 B）─────────────────
// CSM 基线截图 vs IZB 门开截图 + izb_mask.ppm（门开跑内 dump 产出）三方对拍：
//   锚点   |变暗| ≥ 下限（消费链路活着——2026-10-02 字节偏移 bug 下恒 0）
//   域内   变暗 ⊆ dilate(mask遮,2)（IZB 不许在 mask 判受光处画影——ndc 镜像 bug 红）
//   消费   mask遮 ∩ CSM受光 ⊆ dilate(变暗,2)（mask 有货必须落到画面——消费断链红）
// 变亮集（CSM 遮而 IZB 放）只报不判——但**报出来要带定性**：它是 IZB 缺陷②（管辖范围内系统性
// 欠遮挡，rig 占屏 44%）的读数，不是"v1 语义属预期"。2026-10-10 据此把交付 izb 关成 false。
int cmdIzb(const std::string& glb, const std::string& camera)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto it = man.cameras.find(camera);
    if (it == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    Frame fCsm, fIzb;
    {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = true;  // CSM 基线；mask 重放的 shadow0 段也依赖此门
        makeProfile(projectRoot() + "/build/imgqa_profile_izb_csm", o);
        Engine eng(projectRoot() + "/build/imgqa_profile_izb_csm");
        fCsm = captureFrame(eng, it->second, "izb_csm");
    }
    {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = true;
        o.izbShadow = true;
        o.izbDumpFrame = 5;
        makeProfile(projectRoot() + "/build/imgqa_profile_izb_on", o);
        // 删陈旧 ppm（首跑实锤：dump 不生效时旧文件会伪装成"本轮产物"）
        std::filesystem::remove(std::filesystem::path(projectRoot()) / "build/rgl/izb_mask.ppm");
        Engine eng(projectRoot() + "/build/imgqa_profile_izb_on");
        fIzb = captureFrame(eng, it->second, "izb_on");
    }
    // mask PPM（遮挡=黑）→ 8UC1（遮=255）；尺寸必须与截图一致（extent 失配 = 采集错帧）
    const std::string ppm = projectRoot() + "/build/rgl/izb_mask.ppm";
    cv::Mat gray = cv::imread(ppm, cv::IMREAD_GRAYSCALE);
    if (gray.empty())
        throw std::runtime_error("izb_mask.ppm 不存在/读失败（shadow.lua 的 izb.dump_frame 未生效？）");
    if (gray.size() != fIzb.bgr.size())
        throw std::runtime_error("mask 与截图 extent 不符: " + std::to_string(gray.cols)
            + "x" + std::to_string(gray.rows));
    cv::Mat shadowed;
    cv::threshold(gray, shadowed, 127, 255, cv::THRESH_BINARY_INV);

    const auto sumCh = [](const cv::Mat& bgr) {
        cv::Mat f;
        bgr.convertTo(f, CV_32S);
        std::vector<cv::Mat> ch;
        cv::split(f, ch);
        cv::Mat s;
        cv::add(ch[0], ch[1], s);
        cv::add(s, ch[2], s);
        return s;
    };
    const cv::Mat sC = sumCh(fCsm.bgr);
    const cv::Mat d = sumCh(fIzb.bgr) - sC;
    cv::Mat darkened, brightened, litCsm;
    cv::compare(d, cv::Scalar(-30), darkened, cv::CMP_LT);
    cv::compare(d, cv::Scalar(30), brightened, cv::CMP_GT);
    cv::compare(sC, cv::Scalar(90), litCsm, cv::CMP_GE);  // CSM 下受光（和≥90）

    cv::Mat darkIn = darkened.clone();
    cv::bitwise_and(darkIn, dilateMask(shadowed, 2), darkIn);
    cv::Mat darkOut = darkened - darkIn;  // 域外变暗 = 寻址错
    cv::Mat req;
    cv::bitwise_and(shadowed, litCsm, req);  // mask 遮 ∧ CSM 受光 → 必须变暗
    cv::Mat reqDil = req.clone();
    cv::bitwise_and(reqDil, dilateMask(darkened, 2), reqDil);
    cv::Mat unconsumed = req - reqDil;

    const int64_t nDark = cv::countNonZero(darkened);
    const int64_t nDarkOut = cv::countNonZero(darkOut);
    const int64_t nReq = cv::countNonZero(req);
    const int64_t nUncons = cv::countNonZero(unconsumed);
    const int64_t nBright = cv::countNonZero(brightened);

    const Verdict vAnchor = verdictFor("izb-consume-domain", nDark >= 500,
        "锚点：变暗 " + std::to_string(nDark) + " px（下限 500）");
    const Verdict vDomain = verdictFor("izb-consume-domain", nDarkOut == 0,
        "变暗域 ⊆ dilate(mask,2)：域外 " + std::to_string(nDarkOut) + " px（要求恰 0）");
    const Verdict vConsume = verdictFor("izb-consume-domain", nUncons == 0,
        "mask∩受光 ⊆ dilate(变暗,2)：未消费 " + std::to_string(nUncons) + "/"
            + std::to_string(nReq) + " px（要求恰 0）");
    std::cout << "  " << vAnchor.line() << "\n  " << vDomain.line() << "\n  "
        << vConsume.line() << "\n";
    std::printf("\n  INFO 变亮（CSM 遮而 IZB 放）: %lld px = 占屏 %.1f%%\n",
        static_cast<long long>(nBright), 100.0 * static_cast<double>(nBright) / d.total());
    std::printf(
        "       ⚠ 这个数**不是**「v1 近场语义属预期」，它是 IZB 的缺陷②读数（管辖范围内系统性\n"
        "         欠遮挡）。2026-10-10 已据此把交付 IZB 关掉（izb 默认 false），本判据的两条\n"
        "         硬判据只测**寻址一致性**、不测遮挡量——别拿本命令绿了当「IZB 画质可用」。\n"
        "         两个缺陷的完整读数 → docs/shadow_routes_plan.md §7\n");
    logCall("izb", {{"glb", glb}, {"camera", camera}}, {vAnchor, vDomain, vConsume});
    return (vAnchor.ok && vDomain.ok && vConsume.ok) ? 0 : 1;
}

namespace {

/// 影场格 = 世界 y=0 平面上的栅格坐标（swim 的读数单位：一格 5cm）。
using GroundCell = std::pair<int, int>;

/// 多数票之后的影场：格 → 受光（true）/ 遮阴影（false）。
using ShadowField = std::map<GroundCell, bool>;

/// 重投影基向量与视锥参数（引擎口径：fb 行 0 ↔ ndc.y +1；fov 竖直，x 乘 aspect）。
struct GroundCamera {
    glm::vec3 pos;   ///< 机位
    glm::vec3 fw;    ///< 前向（target − position 归一化）
    glm::vec3 rt;    ///< 右向
    glm::vec3 up;    ///< 上向
    double tanH;     ///< 半高视场的正切（fov 是竖直的 ⟹ 换算除以 360 不是 2）
    double aspect;   ///< 宽/高，x 方向要乘它
};

/// 两场地面的重叠计数（零位移主判据与平移搜索**共用同一条计数口径**，两处各写一份 = 两个口径）。
struct FieldOverlap {
    int common = 0;     ///< 共同格数
    int mismatch = 0;   ///< 共同格里受光性不一致的格数
};

/// 平移搜索的读数。未触发搜索时保持零位移初值（x=z=0、失配 = 零位移失配数）。
struct ShiftSearch {
    int x = 0;
    int z = 0;
    size_t mismatch = 0;
};

/// 从截图自带的机位算重投影基向量。两帧各算各的：与引擎有出入也只会**同样地**翘曲两帧。
GroundCamera groundCameraOf(const Frame& f)
{
    const glm::vec3 pos(f.camera.position[0], f.camera.position[1], f.camera.position[2]);
    const glm::vec3 tgt(f.camera.target[0], f.camera.target[1], f.camera.target[2]);
    const glm::vec3 fw = glm::normalize(tgt - pos);
    const glm::vec3 rt = glm::normalize(glm::cross(fw, glm::vec3(0.f, 1.f, 0.f)));
    const glm::vec3 up = glm::cross(rt, fw);
    const double tanH = std::tan(f.camera.fovDegrees * 3.14159265358979 / 360.0);
    const double aspect = static_cast<double>(f.bgr.cols) / f.bgr.rows;
    return GroundCamera{pos, fw, rt, up, tanH, aspect};
}

/// 像素 (x,y) 的视线与世界 y=0 平面交点所在的格。视线近平行于地面（|dir.y| < 1e-6）或交点出界
/// （t ≤ 0、t > 5000）= 这一像素量不到，返回 false（原实现：continue 跳过）。
bool groundCellOf(const GroundCamera& cam, int x, int y, const cv::Size& extent,
    GroundCell& cell)
{
    const double nx = (x + 0.5) / extent.width * 2.0 - 1.0;
    const double ny = 1.0 - (y + 0.5) / extent.height * 2.0;
    const glm::vec3 dir = glm::normalize(cam.fw
        + cam.rt * static_cast<float>(nx * cam.tanH * cam.aspect)
        + cam.up * static_cast<float>(ny * cam.tanH));
    if (std::abs(dir.y) < 1e-6)
        return false;
    const double t = -cam.pos.y / dir.y;
    if (t <= 0 || t > 5000.0)
        return false;
    const glm::vec3 w = cam.pos + dir * static_cast<float>(t);
    cell = std::make_pair(static_cast<int>(std::floor(w.x / 0.05)),
        static_cast<int>(std::floor(w.z / 0.05)));
    return true;
}

/// 阴影阈值 = 地面亮度 p95 × 0.3（env=0 解析光照：受光亮/遮黑双峰，居中判）。
/// 地面像素过少 = 机位无效，抛错而不是给一个噪声阈值出读数。
double groundShadowThreshold(const Frame& f, const cv::Mat& gnd)
{
    std::vector<double> vals;
    for (int y = 0; y < gnd.rows; ++y)
        for (int x = 0; x < gnd.cols; ++x)
            if (gnd.at<uint8_t>(y, x))
                vals.push_back(f.luma.at<double>(y, x));
    if (vals.size() < 1000)
        throw std::runtime_error("地面像素过少，机位无效？");
    std::sort(vals.begin(), vals.end());
    return vals[static_cast<size_t>(vals.size() * 0.95)] * 0.3;
}

/// 地面像素 → 世界格投票：受光记 +1、遮阴影记 −1（逐像素抖动交给后面的多数票抹掉）。
std::map<GroundCell, int> voteGround(const Frame& f, const cv::Mat& gnd, double thr)
{
    const GroundCamera cam = groundCameraOf(f);
    std::map<GroundCell, int> votes;
    for (int y = 0; y < gnd.rows; ++y)
        for (int x = 0; x < gnd.cols; ++x) {
            if (!gnd.at<uint8_t>(y, x))
                continue;
            GroundCell cell;
            if (!groundCellOf(cam, x, y, f.bgr.size(), cell))
                continue;
            votes[cell] += f.luma.at<double>(y, x) < thr ? 1 : -1;
        }
    return votes;
}

/// 格内多数票 → 二值影场：格内至少 2 票同向才要（5cm 格 ≈ 3~4 像素，边缘半格噪声丢弃）。
ShadowField majorityField(const std::map<GroundCell, int>& votes)
{
    ShadowField field;
    for (const auto& [c, v] : votes)
        if (std::abs(v) >= 2)
            field[c] = v > 0;
    return field;
}

/// 一帧的地面影场：阈值 → 重投影投票 → 多数票（原先是 cmdSwim 里的内联 lambda，
/// 独立计认知复杂度 26 > 15；拆成上面四个各 ≤ 15 的小函数后本函数只做串联）。
ShadowField groundField(const Frame& f, const Manifest& m)
{
    const Attribution at = attribute(f, m);
    const cv::Mat gnd = maskOfPiece(f, at, "ground");
    const double thr = groundShadowThreshold(f, gnd);
    return majorityField(voteGround(f, gnd, thr));
}

/// 两场在平移 (sx,sz) 下的共同格数与失配格数；(0,0) 就是主判据要的零位移读数。
FieldOverlap overlapOf(const ShadowField& a, const ShadowField& b, int sx, int sz)
{
    FieldOverlap o;
    for (const auto& [c, sh] : a) {
        const auto hit = b.find({c.first + sx, c.second + sz});
        if (hit == b.end())
            continue;
        ++o.common;
        if (hit->second != sh)
            ++o.mismatch;
    }
    return o;
}

/// 对 B 场做 ±10 格平移搜索——**最优平移 = 影子在世界里滑了多少**。候选覆盖率以零位移
/// 共同格数为基准（不足 8/10 不可信，防止把低覆盖的角落平移当最优解——首跑 48 格教训），
/// 同失配取覆盖更高者。
ShiftSearch searchShift(const ShadowField& a, const ShadowField& b, int common0,
    size_t initialMismatch)
{
    ShiftSearch best;
    best.mismatch = initialMismatch;
    size_t bestN = 0;
    for (int sz = -10; sz <= 10; ++sz)
        for (int sx = -10; sx <= 10; ++sx) {
            const FieldOverlap o = overlapOf(a, b, sx, sz);
            const size_t mm = static_cast<size_t>(o.mismatch);
            const size_t n = static_cast<size_t>(o.common);
            if (n < static_cast<size_t>(common0) * 8 / 10)
                continue;  // 覆盖率不足，该候选不可信
            if (mm < best.mismatch || (mm == best.mismatch && n > bestN)) {
                best.mismatch = mm;
                bestN = n;
                best.x = sx;
                best.z = sz;
            }
        }
    return best;
}

/// 采集一个机位的地面帧（swim 两臂只差机位）。影场配置 = 固定五项 + izb/evssm 由调用侧给。
Frame captureSwimGround(const std::string& glb, const Engine::Camera& cam,
    const std::string& tag, bool izb, bool evssm)
{
    ProfileOpts o;
    o.glb = glb;
    o.shadowPass = true;
    o.izbShadow = izb;
    o.evssmShadow = evssm;
    makeProfile(projectRoot() + "/build/imgqa_profile_swim", o);
    Engine eng(projectRoot() + "/build/imgqa_profile_swim");
    return captureFrame(eng, cam, tag);
}

}  // namespace

// ── swim：影子世界空间游动量测（同场景两机位，静态太阳下影场必须世界不变）────
// 判据不是屏幕观感相关（视差会淹没它，5a-7 时代 1D 剖面互相关实测被骗）：
// 把两帧的地面像素各自重投影回世界 y=0 平面、栅格化 2cm 格的受光/遮阴影场，
// 对 B 场做平移搜索（±20cm）——**最优平移 = 影子在世界里滑了多少**。
// 重投影基向量若与引擎有出入只会把两帧**同样地**翘曲，平移差不受影响（漂移
// 量测自洽）；格内多数票消除像素级抖动。
int cmdSwim(const std::string& glb, const std::string& camera, double dx, bool izb, bool evssm)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto it = man.cameras.find(camera);
    if (it == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    Engine::Camera camB = it->second;
    camB.position[0] += dx;
    camB.target[0] += dx;
    const Frame fA = captureSwimGround(glb, it->second, "swim_a", izb, evssm);
    const Frame fB = captureSwimGround(glb, camB, "swim_b", izb, evssm);
    const ShadowField fieldA = groundField(fA, man);
    const ShadowField fieldB = groundField(fB, man);

    // 零位移失配 = 主判据；FAIL 才做平移搜索量化"滑了多少"（候选覆盖率以零位移
    // 共同格数为基准，防止把低覆盖的角落平移当最优解——首跑 48 格教训）
    const FieldOverlap zero = overlapOf(fieldA, fieldB, 0, 0);
    const int mismatchAt0 = zero.mismatch, common0 = zero.common;
    std::printf("  影场格 A=%zu B=%zu 共同=%d（5cm 格）；零位移失配 %d = %.2f%%\n",
        fieldA.size(), fieldB.size(), common0, mismatchAt0,
        100.0 * mismatchAt0 / std::max(1, common0));
    ShiftSearch best;
    best.mismatch = static_cast<size_t>(mismatchAt0);
    if (mismatchAt0 > std::max(20, common0 / 200))
        best = searchShift(fieldA, fieldB, common0, best.mismatch);
    std::printf("  最优平移 (%d, %d) 格 = (%.0f, %.0f) cm，失配 %zu\n", best.x, best.z,
        best.x * 5.0, best.z * 5.0, best.mismatch);
    const bool still = mismatchAt0 <= std::max(20, common0 / 200);
    const Verdict v = verdictFor("shadow-swim", still,
        "机位平移 " + std::to_string(dx) + " m：影场零位移失配 "
            + std::to_string(mismatchAt0) + "/" + std::to_string(common0)
            + "（阈 max(20, 共同/200)），漂移估计 (" + std::to_string(best.x * 5) + ", "
            + std::to_string(best.z * 5) + ") cm");
    std::cout << "  " << v.line() << "\n";
    logCall("swim", {{"glb", glb}, {"camera", camera}, {"dx", std::to_string(dx)},
        {"izb", izb ? "on" : "off"}}, {v});
    return v.ok ? 0 : 1;
}

}  // namespace RGL::imgqa
