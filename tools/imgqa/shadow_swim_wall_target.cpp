// shadow_swim_wall_target.cpp —— `shadow-swim-wall` 的**靶侧与机位侧**：manifest 读取 +
// 机位构造 + 闭式门面掩码/视深场。
//
// ── 为什么这是一条职责边界（不是「哪个文件有空位」）─────────────────────────────────
// 判据只有一条，且可机器核对：**本文件的任何一个函数都不起引擎进程，也不写磁盘。**
// （机器口径 = **剥掉注释后**，本文件里 `makeProfile` / `runEngineOnce` / `readEngineLog` /
// `readPngBgr` / `ProfileOpts` / `ofstream` 各出现 **0** 次；本段注释提到这些名字是例外。）
// 它的输入是靶的 manifest 文件，输出是 `WallTarget` / `WallCamera` 两个值类型，中间全是
// 闭式算术（射线-平面求交、视深梯度、normalize）。反面 = `shadow_swim_wall.cpp`：那里
// 每次装配一臂都要 `makeProfile` + `runEngineOnce` 起一个引擎进程，还要读回 PNG 与引擎
// 回显对账。这条边界因此就是本项目分层反复在用的那个问题——**这段逻辑需不需要一个真设备
// 才能跑**——搬到底：本文件可以在没有 GPU 的机器上跑完、跑对。
//
// 三个后果，各自对应一类真实的改动：
//   · 改「测量接口的形状」（manifest 换键名 / 加档 / captureSize 换口径）只动本文件；
//     改「臂表与配置门」（钉哪一级、门开门关、帧号）只动 `shadow_swim_wall.cpp`。
//   · 闭式掩码与闭式视深是本仪器**判据权威的唯一来源**（区域来自几何，不读图）；把它们
//     和引擎采集放在同一个 TU 里，读代码的人会以为这两个场也是从引擎里量出来的。
//   · 本文件是这台仪器里唯一能脱离引擎做变异验证的一半。
//
// 先例：`shadow_edge_target.cpp`（同族同职责：manifest measure 块读取 + 闭式取景）、
// `temporal_motion_collect.cpp`（反面那一半单独成 TU 的既有做法）。
//
// ── 🔴 变异红线（哪个变异让本层红；实跑记录见交付说明）──────────────────────────────
//   · 删掉 captureSize 核对 → 偷改采集尺寸后，闭式取景（掩码/视深/投影器）按 1600x900 算，
//     而图是别的尺寸 ⟹ 掩码与像素错位，四格残差整块建立在错的区域上。
//   · 把 `cameraAt` 的 target 改成跟着相机动 → 「纯平移激励」变成转向，G1（两枚机位实测
//     级联集合相同）与 J-W4 的「残差不混级联本身换了」同时失效。
//   · 把注入方向从门面内的 e1 改回法线 n → J-W6 红（实测报 (0.000, −0.070) 而非 (0.250, 0)：
//     沿 n 挪 = 相机/靶/遮挡杆刚性平移，方向光下杆影落在门面哪一格一个字都不变）。
//   · 把视深梯度写成 `+dot(eye − O, f)`（符号反）→ 掩码不变但 J-W2 的单调性松弛量红。
#include "shadow_swim_wall.h"

#include "shadow_kernel_metric.h"  // kNaN（全家族同一个 NaN 约定，不另立）

#include <nlohmann/json.hpp>

#include <opencv2/core.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::string readFileText(const fs::path& p)
{
    std::ifstream f(p);
    if (!f)
        throw std::runtime_error("读不到 " + p.string());
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::array<double, 3> asVec3(const json& v, const std::string& what)
{
    if (!v.is_array() || v.size() != 3)
        throw std::runtime_error("manifest 的 " + what + " 不是 3 元数值数组——测量接口形状变了，"
            "本命令拒绝猜");
    return { v[0].get<double>(), v[1].get<double>(), v[2].get<double>() };
}

/// 沿 n 距离 a 的机位（与生成器 wall_frame 同式：eye = O + a·n，target = 门面中线）。
/// 🔴 target 不随相机动 ⟹ 姿态随 a 变，但**平移激励**（A→B）是纯平移：B = A + δ·n。
Engine::Camera cameraAt(const WallTarget& t, double a, const std::array<double, 3>& shift = {})
{
    const std::array<double, 3> nrm = planeNormal(t.plane);
    const std::array<double, 3> e2 = t.plane.e2;
    const double yMid = 0.5 * (t.vRange[0] + t.vRange[1]);
    Engine::Camera c;
    for (int k = 0; k < 3; ++k) {
        c.position[k] = a * nrm[k] + shift[k];          // eye = a·n（高度 0，同生成器）+ 注入量
        c.target[k] = yMid * e2[k] + shift[k];          // target = 中线（跟着相机一起挪）
    }
    c.fovDegrees = t.fovDegrees;
    return c;
}

}  // namespace

// ── 靶读取 ──────────────────────────────────────────────────────────────────────────
WallTarget loadWallTarget()
{
    const std::string dir = projectRoot() + "/" + kWallGlbDir;
    const fs::path glb = fs::path(dir) / (std::string(kWallStem) + ".glb");
    const fs::path mf = fs::path(dir) / (std::string(kWallStem) + ".manifest.json");
    if (!fs::exists(glb) || !fs::exists(mf))
        throw std::runtime_error(std::string("找不到靶 ") + kWallStem + "（需要 " + glb.string()
            + " 与 .manifest.json）——先跑 python3 tools/qa_scenes.py --out build/rgl/models --only "
            + kWallStem);
    const json m = json::parse(readFileText(mf));
    const json& ms = m.at("measure");

    WallTarget t;
    const json& pl = ms.at("plane");
    t.plane = makePlaneFrame(asVec3(pl.at("origin"), "plane.origin"),
        asVec3(pl.at("e1"), "plane.e1"), asVec3(pl.at("e2"), "plane.e2"));
    t.uRange = { pl.at("uMin").get<double>(), pl.at("uMax").get<double>() };
    t.vRange = { pl.at("yMin").get<double>(), pl.at("yMax").get<double>() };
    t.fovDegrees = ms.at("fovDegrees").get<double>();
    t.stimulusM = ms.at("stimulus").at("meters").get<double>();
    const auto cap = ms.at("captureSize");
    if (cap[0].get<int>() != kWallWidth || cap[1].get<int>() != kWallHeight)
        throw std::runtime_error("靶 manifest 的 captureSize 是 "
            + cap[0].dump() + "x" + cap[1].dump() + "，而量具钉死 "
            + std::to_string(kWallWidth) + "x" + std::to_string(kWallHeight)
            + " —— 生成器的取景 assert 按它算，不一致 = 闭式取景全部错位");
    for (const auto& [nm, c] : ms.at("cameras").items()) {
        t.camDist[nm] = { c.at("distanceN").get<double>(), 0.0, 0.0 };
        const auto sp = c.at("splits");
        t.splits[nm] = { sp[0].get<double>(), sp[1].get<double>(), sp[2].get<double>(),
            sp[3].get<double>() };
        const auto tx = c.at("texelM");
        t.texelM[nm] = { tx[0].get<double>(), tx[1].get<double>(), tx[2].get<double>(),
            tx[3].get<double>() };
        t.nearQ[nm] = c.at("nearQ").get<double>();
        t.farQ[nm] = c.at("farQ").get<double>();
    }
    for (const auto& p : ms.at("causalPairs")) {
        WallTarget::Pair pair;
        pair.a = p.at("a").get<std::string>();
        pair.b = p.at("b").get<std::string>();
        pair.kind = p.at("kind").get<std::string>();
        t.pairs.push_back(pair);
    }
    t.causalOrder = { "d1", "d2", "d3", "d4" };
    for (const auto& nm : t.causalOrder)
        if (t.splits.find(nm) == t.splits.end())
            throw std::runtime_error("靶 manifest 缺因果档 " + nm + "（生成器与仪器对不上）");
    return t;
}

// ── 闭式取景场（掩码 / 视深）：判据的区域与观测量都在这里，全部不读图 ─────────────────
cv::Mat wallMask(const WallCamera& c, const WallTarget& t)
{
    cv::Mat m = cv::Mat::zeros(kWallHeight, kWallWidth, CV_8UC1);
    for (int y = 0; y < kWallHeight; ++y)
        for (int x = 0; x < kWallWidth; ++x) {
            double u = 0.0;
            double v = 0.0;
            if (!planeHitDouble(c.proj, x, y, kWallMaxDistanceM, t.plane, u, v))
                continue;
            if (u >= t.uRange[0] && u <= t.uRange[1] && v >= t.vRange[0] && v <= t.vRange[1])
                m.at<uint8_t>(y, x) = 255;
        }
    return m;
}

cv::Mat wallDepthField(const WallCamera& c, const WallTarget& t)
{
    cv::Mat d(kWallHeight, kWallWidth, CV_64FC1, cv::Scalar(kNaN));
    for (int y = 0; y < kWallHeight; ++y)
        for (int x = 0; x < kWallWidth; ++x) {
            double u = 0.0;
            double v = 0.0;
            if (!planeHitDouble(c.proj, x, y, kWallMaxDistanceM, t.plane, u, v))
                continue;
            d.at<double>(y, x) = c.g[2] + c.g[0] * u + c.g[1] * v;
        }
    return d;
}

namespace {
/// 机位定稿：投影器 + 闭式视深梯度（gU = e1·f、gY = e2·f、c0 = −(eye − O)·f）。
/// 🔴 常规臂与注入臂**都必须**过这里：漏一步就是 Projector 没填，掩码/视深要等采集全部跑完
///    才炸（本批实跑炸过一次：区分力守卫那一步抛，前面 50 多次进程白跑）。
void finalizeWallCamera(WallCamera& c, const WallTarget& t)
{
    c.proj = makeProjector(c.cam, kWallWidth, kWallHeight);
    // 视深梯度（闭式，与生成器同式）：gU = e1·f、gY = e2·f、c0 = −(eye − O)·f
    const auto& e1 = t.plane.e1;
    const auto& e2 = t.plane.e2;
    const auto& f = [&] {
        std::array<double, 3> d {};  // 视线方向 = normalize(target − eye)
        for (int k = 0; k < 3; ++k)
            d[k] = c.cam.target[k] - c.cam.position[k];
        const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        return std::array<double, 3> { d[0] / l, d[1] / l, d[2] / l };
    }();
    const auto dot3 = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const std::array<double, 3> eyeRel { c.cam.position[0] - t.plane.origin[0],
        c.cam.position[1] - t.plane.origin[1], c.cam.position[2] - t.plane.origin[2] };
    c.g = { dot3(e1, f), dot3(e2, f), -dot3(eyeRel, f) };
}
}  // namespace

WallCamera makeInjectedCamera(const WallTarget& t, const std::string& baseName, double shift)
{
    WallCamera c;
    c.name = baseName + "inj";
    c.distanceN = t.camDist.at(baseName)[0];
    c.entityShift = shift;
    for (int k = 0; k < 3; ++k)
        c.shiftDir[k] = t.plane.e1[k];
    // 🔴 注入方向必须是**门面平面内**的 e1，不是法线 n。第一版沿 n 挪，实测报 (0, −0.070)
    //    而不是 (0.250, 0)：沿 n 挪 = 相机/靶/遮挡杆一起刚性平移，方向光下「杆影落在门面的
    //    哪一格」一个字都不变，而重建又把像素投回**固定**平面（沿 n 挪不出该平面）⟹ 世界滑移
    //    恒 0，注入的是空气。沿 e1 挪：e1·L = 0（生成器机关 ①）⟹ 杆影的 u 坐标 = 杆的 u 坐标，
    //    整组杆平移 d·e1 ⟹ 门面局部坐标里的影图案沿 u 滑 d，而取景逐字相同。
    std::array<double, 3> dir {};
    for (int k = 0; k < 3; ++k)
        dir[k] = shift * t.plane.e1[k];
    c.cam = cameraAt(t, c.distanceN, dir);  // 相机跟着靶挪 ⟹ 屏幕取景逐字相同
    finalizeWallCamera(c, t);
    return c;
}

WallCamera makeWallCamera(const WallTarget& t, const std::string& name)
{
    WallCamera c;
    c.name = name;
    c.distanceN = t.camDist.at(name)[0];
    c.cam = cameraAt(t, c.distanceN);
    finalizeWallCamera(c, t);
    return c;
}

}  // namespace RGL::imgqa
