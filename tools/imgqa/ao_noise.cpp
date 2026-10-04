// AO 噪声量测（Phase 13 观测任务）——「SSAO 到底噪不噪」的判据实现 + 四组测量执行。
//
// ## 锚点在哪（为什么不需要基线图像）
//
// 两个**恰为零**的锚点，都不需要参考图：
//
//   ① `ao.frag.slang:118` 的 strength≤0 早退是位级恒等 ⟹ AO 附件在 strength=0 时**逐位
//      255**（实测全图 σ=0.000）。于是同一算子作用在 strength=0.5 的附件上的高频读数，
//      它的零点不是估出来的，是恒等式给的。
//   ② AO 附件 = 255（附件自己说「这里没遮蔽」）的像素上，AO 开/关两张交付图必须**逐位
//      相同**（实测 876083 像素 max = 0 LSB）。这是**因果泄漏恒等式**：任何非零都说明
//      附件之外还有第二处东西在随强度变。它同时证明附件是交付差的唯一来源——交付 Δ 随
//      AO 偏离单调上升（AO≥245→max 3、≥235→5、≥225→8、≥215→10、≥195→16）。
//
// ## 两条实测推翻的初版口径（写在这里防后人重犯）
//
//   ✗ 「契约区（离轮廓 > 40 px）内的 (255 − AO) 就是噪声」。**不成立**：AO 半径 0.5 是
//     **世界**量，靠近相机的表面在轮廓内 40 px 处世界距离可远小于 0.5 m（实测 box_short
//     正面上离轮廓 >40 px 处 AO 低到 37，是合法遮蔽）。没有深度缓冲就画不出世界空间的
//     契约区。⇒ 偏离契约值降级为观察量；主判据改走**高频项**（medianBlur 残差），它对
//     合法的光滑遮蔽梯度不敏感，这才是区域定义不干净时仍然成立的量。
//   ✗ 「TAA 开 + 静止机位 + AO 关时逐帧差恰为 0（纯色平面无边缘 ⟹ jitter 不改颜色）」。
//     **不成立**：实测地板每帧 7.5 万像素在变（灰度均值 3.216/255）——deferred 的 GGX 高光
//     是视相关的，jitter 改视向量就改高光，与有没有边缘无关。⇒ 组 4 的 AO 开/关比值
//     （契约区 1.006×）**不可解释**，本组不判 AO；AO 的时域结论走 TAA 关态的两条零锚点。
//
// ## 为什么必须程序化靶
//
// `tools/qa_scenes.py` 的场景刻意不输出 TEXCOORD_0/TANGENT（缺 → uv 填 0 走白纹理），
// 每个 piece 是高饱和纯色 ⟹ **底图在 piece 内部零空间高频**（实测底图抖动 RMS 0.012）。
// 于是画面上出现的高频只可能来自 AO / 阴影 PCF / TAA，AO 噪声的隔离度远好于 bistro
// （395 张纹理把噪声埋在纹理里）。这条不是优化，是判据成立的前提：分母（底图自身高频）
// 在 bistro 上不是 0，比值不可解释。
//
// ## 四组测量
//
//   ① A/B 空间噪声（主判据）：**同进程** `POST /param` 翻 ao.strength 0 ↔ 0.5（跨进程
//      A/B 已证污染，见 `Engine::setParam` 注释）。契约区高频项 + 交付面字节颗粒项。
//   ② AO 场直方图 / 剖面：取值分布（12 样本会不会走成离散台阶）+ 穿过接触区的扫描线。
//   ③ 亚像素爬行：机位 yaw 整 1 像素。AO 的随机相位 = `hash12(SV_Position.xy)`（:134）
//      **只吃像素坐标** ⟹ 噪声贴在屏幕上。判据 = 两个独立算子（幅度 RMS 与偏离集合
//      Jaccard）必须给同一结论；算子自己的阳性锚点 = 同一算子作用在底图上必须「对齐后
//      变小」（几何必然跟着相机走）。
//   ④ 时域（TAA 开态）：`/shotburst` 连拍逐帧差，AO 0 vs 0.5 各一轮，两轮帧数间隔取抖动
//      周期的整数倍（同抖动相位起步）。**只判量具**（地板 > 0 = burst 真在交不同帧），
//      理由见上节第二条。
//
// 外加**掠射机位**一组：候选根因 B3（法线来自 ddx/ddy(P)）与 B4（核尺寸也来自导数）的
// 角度判别——导数重建的世界坐标在掠射/远处最差，噪声若随入射角掠射而暴涨即支持二者
// （实测 σ 5.14×、高频项 2.62×，支持 B3/B4；两者的进一步分离要改 shader，本任务红线
// 禁止，故只合并报告）。
//
// ## 反模式自查（为什么这条不是已登记的那些坑）
//
// - 不是 `ssim`/`psnr`：主测量是高频项对恒等式零点的偏离，区域来自 objectID 几何反解。
// - 不是 `global-epsilon`：容差理由见 judge.cpp 注册表（恒等式 + 量化步长，不是相似度）。
// - 不是 `screen-space-l1-frame-diff`：那条被推翻正是因为**分母非零的逐帧差比值**。本工具
//   的④实测就是这种形态（地板非零），所以它**不产出判据**，只判量具；时域结论走零锚点。
#include "imgqa.h"

#include "ao_metrics.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

namespace RGL::imgqa {
namespace {

constexpr int kAoSize[2] = {1280, 720};
constexpr double kAoFovDeg = 45.0;
/// 抖动周期（Halton(2,3)×16，= rgl/taa_jitter.cppm::kTaaJitterPeriod）。此处**不 import**
/// 引擎头（imgqa 是独立二进制、刻意不链引擎），值与之一致并由④的窗口对齐检查兜底。
constexpr int kJitterPeriod = 16;

std::string d3(double v)
{
    std::ostringstream o;
    o << std::fixed << std::setprecision(3) << v;
    return o.str();
}
std::string d5(double v)
{
    std::ostringstream o;
    o << std::fixed << std::setprecision(5) << v;
    return o.str();
}

// ───────────────────────────── 采集 ─────────────────────────────

struct AoCapture {
    Frame frame;   ///< color + object_id + luma（区域归因的载体）
    cv::Mat ao;    ///< 8UC1 AO 附件（0..255 = 可见度）
};

/// 把引擎产物收进本次测量目录（留证据；PNG 是判据的第一手数据，不是附属品）。
void keep(const std::string& src, const std::string& outDir, const std::string& name)
{
    std::error_code ec;
    fs::copy_file(src, fs::path(outDir) / name, fs::copy_options::overwrite_existing, ec);
    if (ec)
        throw std::runtime_error("产物归档失败: " + src + " → " + name + "（" + ec.message() + "）");
}

/**
 * 拍齐 color + object_id + ao 三张。
 *
 * ⚠ 三张是**三次 /shot = 三个不同帧**。TAA 关时这没问题（AO 无时间输入、着色链无跨帧
 * 状态 ⟹ 同机位逐帧逐位相同，`determinism` 判据就在钉这条）；TAA 开时**绝不能**用本函数
 * 做时域测量（那时必须 /shotburst，见 imgqa.h::shotBurst 注释）。
 */
AoCapture captureAoSet(Engine& eng, const Engine::Camera* cam, const std::string& tag,
    const std::string& outDir, const std::string& prefix, bool withId = true)
{
    AoCapture c;
    if (cam) {
        c.frame = captureFrame(eng, *cam, tag);
    } else {
        // **不动机位**（交付配置模式）：机位的真相在交付 camera.lua 里，工具再设一次就是
        // 第二份真相（`ProfileOpts::hasCamera` 的注释记着这条值一轮假绿）。
        c.frame.bgr = readPngBgr(eng.shot(tag + "_color"));
        // object_id 附件由 picking 门装饰，交付配置默认**关**着它（实测：请求不存在的附件
        // 会让引擎侧报「附件不存在」）。交付组不做区域归因，故连请求都不发。
        if (withId)
            c.frame.ids = decodeObjectId(readPngBgr(eng.shot(tag + "_objectid", "object_id")));
    }
    keep(projectRoot() + "/build/rgl/screenshots/imgqa_" + tag + "_color.png", outDir,
        prefix + "_color.png");
    if (withId) {
        keep(projectRoot() + "/build/rgl/screenshots/imgqa_" + tag + "_objectid.png", outDir,
            prefix + "_objectid.png");
    }

    const std::string aoPath = eng.shot(tag + "_ao", "ao");
    keep(aoPath, outDir, prefix + "_ao.png");
    const cv::Mat bgr = readPngBgr(aoPath);
    if (bgr.size() != c.frame.bgr.size())
        throw std::runtime_error("ao 附件与 color 尺寸不同（" + std::to_string(bgr.cols) + "x"
            + std::to_string(bgr.rows) + " vs " + std::to_string(c.frame.bgr.cols) + "x"
            + std::to_string(c.frame.bgr.rows) + "）—— 两图必然不同帧或 AO RT 尺寸另有出处");
    cv::extractChannel(bgr, c.ao, 2);  // BGR 序 → ch2 = R（convR8Unorm 三通道同值）
    return c;
}

// ───────────────────────────── 可视化 ─────────────────────────────

/// 单通道 → 放大可视化（×gain 后饱和到 255）。0 值区域保持 0（黑），故 mask 外是黑的。
void saveAmp(const cv::Mat& x, const cv::Mat& mask, double gain, const std::string& path)
{
    cv::Mat f;
    x.convertTo(f, CV_32F);
    cv::Mat vis = cv::Mat::zeros(x.size(), CV_32F);
    for (int y = 0; y < x.rows; ++y)
        for (int x0 = 0; x0 < x.cols; ++x0)
            if (mask.at<uint8_t>(y, x0))
                vis.at<float>(y, x0) = std::abs(f.at<float>(y, x0)) * gain;
    cv::Mat o;
    vis.convertTo(o, CV_8U);  // convertTo 自带饱和
    cv::imwrite(path, o);
}

/// 把 mask 画成半透明红叠加在灰度底图上（人眼复核区域是否落在该落的地方）。
void saveMaskOverlay(const cv::Mat& base, const cv::Mat& mask, const std::string& path)
{
    cv::Mat g;
    cv::cvtColor(base, g, cv::COLOR_BGR2GRAY);
    cv::Mat bgr;
    cv::cvtColor(g, bgr, cv::COLOR_GRAY2BGR);
    for (int y = 0; y < mask.rows; ++y)
        for (int x = 0; x < mask.cols; ++x)
            if (mask.at<uint8_t>(y, x))
                bgr.at<cv::Vec3b>(y, x) = cv::Vec3b(0, 0, 255);
    cv::imwrite(path, bgr);
}

// ───────────────────────────── 机位 ─────────────────────────────

/// 绕相机自身 up（世界 Y）纯 yaw：像素位移 = f_px·θ，**与深度无关**（刚体运动，
/// 重投影能精确对齐；平移 dolly 会让信号随深度衰减，见 temporal.cpp 头注释）。
/// 换算：pixel.x = W/2 + (W/2)·X/(aspect·tan(fov/2)·(−Z)) ⟹  yaw 角 θ 时
/// Δpixel = −f_px·θ，f_px = H/(2·tan(fov/2))。二阶项 O(θ²) 在 θ≈1/869 rad 下 < 1e-5 px。
Engine::Camera yawCam(const Engine::Camera& base, double shiftPx)
{
    const double fPx = (kAoSize[1] / 2.0)
        / std::tan(base.fovDegrees * 3.14159265358979323846 / 360.0);
    const double th = shiftPx / fPx;  // 弧度
    const double dx = base.target[0] - base.position[0];
    const double dy = base.target[1] - base.position[1];
    const double dz = base.target[2] - base.position[2];
    Engine::Camera c = base;
    c.target = {base.position[0] + std::cos(th) * dx + std::sin(th) * dz, base.position[1] + dy,
        base.position[2] - std::sin(th) * dx + std::cos(th) * dz};
    return c;
}

/// 把 AO 场写成文本化可视化（值域压到 0..255 的伪彩灰度）——直接看台阶/颗粒用。
void saveStretched(const cv::Mat& ao, double lo, double hi, const std::string& path)
{
    cv::Mat f;
    ao.convertTo(f, CV_32F);
    f = (f - lo) * (255.0 / std::max(1e-6, hi - lo));
    cv::Mat o;
    f.convertTo(o, CV_8U);
    cv::imwrite(path, o);
}

}  // namespace
// ───────────────────────────── 主流程 ─────────────────────────────

namespace {

/// AO 强度滑杆的定位。真相源 = `rgl/generated/param_semantics_generated.cppm` 的
/// `kAoStrength`（label）+ `rgl/param_pages_impl.cppm` 的 `renderPage.name`（page）。
/// 写错不会静默：`/param` 对未知 label 回 404，`Engine::setParam` 抛异常。
constexpr const char* kAoPage = "渲染";
constexpr const char* kAoLabel = "AO 强度 ao.strength";

/// 爬行检验的机位偏移。**必须是整数像素**：对齐于是成为精确的整数平移算子，
/// 不需要亚像素插值（插值自己会引入高频，把被测项淹掉）。
constexpr double kShiftPx = 1.0;

/// 两张 PNG 是否**逐位**相同（解码后比像素，不比文件字节——PNG 编码本身不是判据）。
bool samePng(const std::string& a, const std::string& b)
{
    const cv::Mat x = readPngBgr(a), y = readPngBgr(b);
    if (x.size() != y.size() || x.type() != y.type())
        return false;
    cv::Mat d;
    cv::absdiff(x, y, d);
    // 三通道 Mat 不能直接 countNonZero（OpenCV 断言 cn==1）；L1 范数对 8U 是精确整数和，
    // 为 0 ⟺ 每通道逐位相同，不需要先降通道（降通道会引入「加权抵消」这一层需要论证的东西）。
    return cv::norm(d, cv::NORM_L1) == 0.0;
}

/// 同进程、同机位下的一对 A/B（AO 开 / AO 关）+ 两根确定性锚。
///
/// **为什么必须同进程**（首跑之后改的）：跨进程的两张图之间隔着整个渲染环境（两次进程、
/// 两次 shader 编译、两次驱动初始化），把它们之差归因给 AO 是错的。实测跨进程在**契约区**
/// 出现最大 41 LSB 的差，而 AO 场在该区最多偏离契约 16 LSB（×strength 0.5 至多 8 LSB）——
/// 多出来的部分根本不是 AO。同进程内用 `/param` 翻强度，两张图之间只隔 scene data 里
/// 那一个 float，差值才是被测项。
struct AbPair {
    AoCapture on;        ///< 终帧 + object_id + AO 附件 @ strength
    cv::Mat aoOff;       ///< AO 附件 @ 0（反向用例：必须逐位 255）
    cv::Mat colorOff;    ///< 终帧 @ 0
    bool repeatable = false;  ///< 同参数连拍两次终帧是否逐位相同
    bool reversible = false;  ///< 强度切回 strength 后终帧是否与第一次逐位相同
};

/// 拍一对 A/B。`cam` 由本函数负责摆（调用方只保证 profile 已建好）。
AbPair captureAb(Engine& eng, const Engine::Camera* cam, double strength,
    const std::string& outDir, const std::string& prefix, bool withId = true)
{
    AbPair ab;
    eng.setParam(kAoPage, kAoLabel, strength);
    ab.on = captureAoSet(eng, cam, prefix + "_on", outDir, prefix + "_on", withId);
    // 确定性锚 ①：同参数连拍（帧间抖动 / 未初始化内存 / 异步竞态）。
    ab.repeatable = samePng(eng.shot(prefix + "_rep1"), eng.shot(prefix + "_rep2"));

    // 反向用例 + 终帧 A/B：强度切 0。
    eng.setParam(kAoPage, kAoLabel, 0.0);
    const std::string aoOffPath = eng.shot(prefix + "_aooff", "ao");
    keep(aoOffPath, outDir, prefix + "_ao_off.png");
    const cv::Mat aoOffBgr = readPngBgr(aoOffPath);
    cv::extractChannel(aoOffBgr, ab.aoOff, 2);
    ab.colorOff = readPngBgr(eng.shot(prefix + "_coloroff"));
    keep(projectRoot() + "/build/rgl/screenshots/imgqa_" + prefix + "_coloroff.png", outDir,
        prefix + "_color_off.png");

    // 确定性锚 ②：切回原强度，终帧必须与第一次**逐位**相同。这一条同时排除
    // 「/param 没生效」与「强度切换有残留状态」两种失效方式。
    eng.setParam(kAoPage, kAoLabel, strength);
    ab.reversible = samePng(eng.shot(prefix + "_onagain"),
        projectRoot() + "/build/rgl/screenshots/imgqa_" + prefix + "_on_color.png");
    return ab;
}

/// 一帧的逐帧差幅度（8UC3 两张 → 非零像素数 / 灰度均值 / 灰度最大）。
struct FrameDiff {
    int64_t changed = 0;
    double mean = 0.0, mx = 0.0;
};
/// `mask` 空 = 全图。分区是必需的：全图逐帧差被 TAA jitter 打在**几何边缘**上的变化主导
/// （实测地板 27.6 万像素/帧），AO 的时间贡献淹在里面；契约区（无边缘的平坦面）才是有
/// 分辨力的地方。
FrameDiff diffFrames(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask = {})
{
    FrameDiff d;
    cv::Mat ad, g, g32;
    cv::absdiff(a, b, ad);
    cv::cvtColor(ad, g, cv::COLOR_BGR2GRAY);
    if (mask.empty()) {
        d.changed = cv::countNonZero(g);
        g.convertTo(g32, CV_64F);
        d.mean = cv::mean(g32)[0];
        double hi = 0.0;
        cv::minMaxLoc(g32, nullptr, &hi);
        d.mx = hi;
        return d;
    }
    cv::Mat gm;
    cv::bitwise_and(g, mask, gm);
    d.changed = cv::countNonZero(gm);
    cv::Mat v;
    cv::meanStdDev(gm, v, v);  // mask 外是 0，均值是「含零填充」的，故按像素数自己除
    d.mean = d.changed > 0 ? v.at<double>(0) * gm.total() / static_cast<double>(cv::countNonZero(mask))
                           : 0.0;
    double hi = 0.0;
    cv::minMaxLoc(gm, nullptr, &hi);
    d.mx = hi;
    return d;
}

}  // namespace

int cmdAoNoise(const std::string& glb, const std::string& camera, const std::string& variant,
    double strength, int radius, int pairs, bool doTemporal, bool doBistro)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto camIt = man.cameras.find(camera);
    if (camIt == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);
    const Engine::Camera cam = camIt->second;

    const std::string outDir = projectRoot() + "/build/ao_noise";
    std::error_code ec;
    fs::create_directories(outDir, ec);
    for (const auto& e : fs::directory_iterator(outDir, ec))
        if (!ec && e.is_regular_file(ec))
            fs::remove(e.path(), ec);  // 陈旧产物会伪装成「本轮测的」（izb 首跑实锤过）

    std::printf("AO 噪声量测（同进程 A/B）  靶=%s  机位=%s  variant=%s  strength=%.3f  契约区排除半径=%d px\n",
        glb.c_str(), camera.c_str(), variant.empty() ? "forward" : variant.c_str(), strength,
        radius);
    std::printf("尺寸 %dx%d  光照 env=0 ambient=0 shadow=off（底图零高频 = 判据前提）\n", kAoSize[0],
        kAoSize[1]);
    std::printf("产物目录 %s\n\n", outDir.c_str());

    const std::string profRoot = projectRoot() + "/build";
    const auto profileOf = [&](const std::string& name, double aoStrength, bool taa,
                                  double feedback) {
        ProfileOpts o;
        o.glb = glb;
        o.variant = variant;
        o.aoStrength = aoStrength;
        o.shadowPass = false;
        o.envIntensity = 0.0;
        o.skybox = false;
        o.exposure = 1.0;
        o.picking = true;
        o.taa = taa;
        o.taaFeedback = feedback;
        o.taaJitterScale = 1.0;
        o.size = {kAoSize[0], kAoSize[1]};
        const std::string dir = profRoot + "/imgqa_ao_" + name;
        makeProfile(dir, o);
        return dir;
    };

    // ══ 采集：主进程跑完组 1/2/3/5（TAA 全关，AO 无时间输入 ⟹ 逐帧确定）═════════
    // 掠射机位：贴近地面平视，让地板/侧墙进入大掠射角。候选根因 B3（法线 = 重建世界坐标的
    // ddx/ddy 叉积）与 B4（核尺寸也来自导数）都预测「掠射处最差」。
    Engine::Camera graze;
    graze.position = {0.0, 0.6, 4.2};
    graze.target = {0.0, 0.35, -5.0};
    graze.fovDegrees = kAoFovDeg;

    AbPair ab, crawl;
    AoCapture grazeCap;
    {
        const std::string dir = profileOf("main", strength, false, 0.12);
        Engine eng(dir);
        ab = captureAb(eng, &cam, strength, outDir, "g1");
        // 组 3 的第二帧：同进程、同强度、机位纯偏航整 1 像素。
        const Engine::Camera crawlCam = yawCam(cam, kShiftPx);
        crawl = captureAb(eng, &crawlCam, strength, outDir, "g3");
        grazeCap = captureAoSet(eng, &graze, "g5", outDir, "g5_graze");
    }

    // 区域归因在 **AO 关**那帧做（色度最干净），故把 colorOff + on 的 id 拼成一个 Frame。
    Frame attr;
    attr.bgr = ab.colorOff;
    attr.ids = ab.on.frame.ids;
    attr.camera = cam;
    prepareLinear(attr);
    const Attribution at = attribute(attr, man);
    std::printf("可见 piece:");
    for (const auto& s : at.visiblePieces())
        std::printf(" %s", s.c_str());
    std::printf("\n");
    for (const auto& c : at.conflicts)
        std::printf("  ⚠ 反解冲突: %s\n", c.c_str());

    std::map<std::string, int32_t> oidByName;
    for (const auto& [id, nm] : at.nameByOid)
        oidByName[nm] = id;
    const auto oidOf = [&](const std::string& n) -> int32_t {
        const auto i = oidByName.find(n);
        return i == oidByName.end() ? 0 : i->second;
    };

    const cv::Mat edges = idEdges(attr.ids);
    const cv::Mat dist = distToEdge(edges);
    std::ignore = cv::imwrite(outDir + "/g1_edges.png", edges);

    cv::Mat flat = cv::Mat::zeros(attr.ids.size(), CV_8UC1);
    cv::Mat band = cv::Mat::zeros(attr.ids.size(), CV_8UC1);
    std::map<std::string, cv::Mat> flatByPiece;
    std::string flatNames;
    for (const auto& name : at.visiblePieces()) {
        const int32_t oid = oidOf(name);
        if (oid == 0)
            continue;
        const cv::Mat f = flatRegion(attr.ids, dist, oid, static_cast<double>(radius));
        cv::bitwise_or(flat, f, flat);
        cv::bitwise_or(band, bandOf(attr.ids, dist, oid, 2.0, static_cast<double>(radius)), band);
        if (cv::countNonZero(f) > 2000) {
            flatByPiece[name] = f;
            flatNames += (flatNames.empty() ? "" : ",") + name;
        }
    }
    if (cv::countNonZero(flat) < 2000)
        throw std::runtime_error("契约区像素不足（离边界 > " + std::to_string(radius)
            + "px）—— 换机位或调小 --radius");
    const cv::Mat allMask(attr.ids.size(), CV_8UC1, cv::Scalar(255));
    // 受光梯度带 = 带内且 AO 确有遮蔽（<255）处；「AO 有作用的地方」的载体。
    cv::Mat bandLit, aoBelow;
    cv::compare(ab.on.ao, cv::Scalar(254), aoBelow, cv::CMP_LT);
    cv::bitwise_and(band, aoBelow, bandLit);

    // 判据要跨块用的量（作用域收在主函数顶部，避免散落）。
    double contactDark = 0.0, contactMin = 255.0, convexFracBelow = 0.0;
    double contactByteGrain = 0.0;
    double leakMax = 0.0;
    int64_t leakPixels = 0;
    int64_t contactPixels = 0;
    double byteFlatGrain = 0.0, byteFlatP99 = 0.0, byteFlatMax = 0.0;
    bool crawlAlignedWorse = false, crawlAgree = false, crawlScreenAnchored = false;
    double baseUnaligned = 0.0, baseAligned = 0.0;
    cv::Mat contactMask;  // 凹角接触环（时域组也要按它分区）

    // ══ 组 1：AO 场本体（闭式锚点区）══════════════════════════════════════════
    const RegionStat aoOnFlat = statRegion(ab.on.ao, flat);
    const RegionStat aoOffFlat = statRegion(ab.aoOff, flat);
    const RegionStat aoOnAll = statRegion(ab.on.ao, allMask);
    cv::Mat baseGray;
    cv::cvtColor(ab.colorOff, baseGray, cv::COLOR_BGR2GRAY);  // statRegion 只读 ch0，先降灰度
    const RegionStat baseFlat = statRegion(baseGray, flat, -1.0);  // 底图自身高频 = 分母

    std::printf("\n── 组 1  闭式契约区（%s，离边界 > %dpx）──────────────────────\n",
        flatNames.c_str(), radius);
    const auto row = [](const char* name, const RegionStat& s) {
        std::printf("  %-24s px %8lld 均值 %-9s σ %-9s 最小 %-6.0f 最大偏离 %-9s"
                    " <255 占 %-8s 拉普拉斯RMS %-9s 抖动RMS %s\n",
            name, static_cast<long long>(s.pixels), d3(s.mean).c_str(), d3(s.sd).c_str(), s.lo,
            d3(kAoContract - s.lo).c_str(), d3(s.fracBelowContract * 100.0).c_str(),
            d3(s.lapRms).c_str(), d3(s.jitterRms).c_str());
    };
    row("AO 附件 @ strength", aoOnFlat);
    row("AO 附件 @ strength=0", aoOffFlat);
    row("AO 附件（全图，观察）", aoOnAll);
    std::printf("  %-24s px %8lld 均值 %-9s σ %-9s 拉普拉斯RMS %-9s 抖动RMS %s"
                "   ← 分母（底图自身高频）\n",
        "底图 @ strength=0", static_cast<long long>(baseFlat.pixels), d3(baseFlat.mean).c_str(),
        d3(baseFlat.sd).c_str(), d3(baseFlat.lapRms).c_str(), d3(baseFlat.jitterRms).c_str());

    std::printf("\n  逐 piece 的契约区（噪声按表面朝向分布 = 机制指纹）\n");
    std::printf("  %-16s %10s %10s %10s %8s %11s %11s\n", "piece", "像素", "均值", "σ", "最小",
        "<255 占%", "抖动RMS");
    for (const auto& [name, m] : flatByPiece) {
        const RegionStat s = statRegion(ab.on.ao, m);
        std::printf("  %-16s %10lld %10s %10s %8.0f %11s %11s\n", name.c_str(),
            static_cast<long long>(s.pixels), d3(s.mean).c_str(), d3(s.sd).c_str(), s.lo,
            d3(s.fracBelowContract * 100.0).c_str(), d3(s.jitterRms).c_str());
    }

    // ══ 组 1b：A/B 落在交付画面上的量 ══════════════════════════════════════════
    {
        Frame onFrame;
        onFrame.bgr = ab.on.frame.bgr;
        onFrame.ids = attr.ids;
        onFrame.camera = cam;
        prepareLinear(onFrame);
        cv::Mat delta(attr.ids.size(), CV_64F, cv::Scalar(0));
        cv::subtract(attr.luma, onFrame.luma, delta);  // off − on：AO 变暗故 ≥ 0

        const LinStat dFlat = statLinearDiff(delta, flat);
        const LinStat dBand = statLinearDiff(delta, bandLit);
        const LinStat dAll = statLinearDiff(delta, allMask);
        const ByteAB bFlat = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, flat);
        const ByteAB bBand = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, bandLit);
        const ByteAB bAll = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, allMask);
        std::printf("\n── 组 1b 交付画面 A/B 差（off vs on，**同进程**）───────────────\n");
        const auto lrow = [](const char* name, const LinStat& l, const ByteAB& b) {
            std::printf("  %-18s px %8lld 线性Δ 均值 %-11s RMS %-11s 最大 %-11s\n", name,
                static_cast<long long>(l.pixels), d5(l.mean).c_str(), d5(l.rms).c_str(),
                d5(l.maxAbs).c_str());
            std::printf("  %-18s             字节Δ 均值 %-11s p99 %-11s 最大 %-8s"
                        " 渐变RMS %-9s 颗粒RMS %s\n",
                "", d3(b.mean).c_str(), d3(b.p99).c_str(), d3(b.mx).c_str(), d3(b.smoothRms).c_str(),
                d3(b.grainRms).c_str());
        };
        lrow("契约区（闭式=0）", dFlat, bFlat);
        lrow("受光梯度带", dBand, bBand);
        lrow("全图（观察）", dAll, bAll);
        std::printf("  颗粒/渐变（字节域）：受光带 %s  全图 %s"
                    "   ← AO 加到画面上的东西里有多少是颗粒而不是形状\n",
            bBand.smoothRms > 0.0 ? d3(bBand.grainRms / bBand.smoothRms).c_str() : "inf",
            bAll.smoothRms > 0.0 ? d3(bAll.grainRms / bAll.smoothRms).c_str() : "inf");

        // 阳性锚点：**凹角接触环**（地面贴着方块/球的那些像素）。闭式说这里必须有遮蔽——
        // 方块脚与地面是凹角、间距 0，AO 半径 0.5 m 内全是遮挡候选。
        // ⚠ 首跑用「一切轮廓附近的带」当锚点，测出 8.4% 变暗，看着像「AO 没跑」；实际带里
        // 绝大多数像素是**凸边**（方块顶沿、球轮廓），那里 AO 恒 1 才是对的。锚点必须钉在
        // 闭式说「这里必须有遮蔽」的地方，否则阳性锚点自己就是假阴的来源。
        contactMask = contactRing(attr.ids, oidOf("floor"),
            {oidOf("box_tall"), oidOf("box_short"), oidOf("sphere")}, 24);
        const cv::Mat& contact = contactMask;
        const RegionStat contactStat = statRegion(ab.on.ao, contact);
        const ByteAB bContact = byteAbsDiff(ab.colorOff, ab.on.frame.bgr, contact);
        // **因果泄漏判据（本工具最硬的一条，且不需要任何区域）**：AO 附件说「这里没遮蔽」
        // （= 255）的地方，AO 开/关两张交付图必须**逐位相同**。它是恒等式而不是相似度——
        // 任何非零都意味着「AO 附件之外还有第二处东西在随强度变」（读错附件 / 强度进了别的
        // shader / 两张图不是同一帧）。另：Δ 随 AO 偏离单调上升（实测 AO≥245 → max 3、
        // ≥235 → 5、≥225 → 8、≥215 → 10、≥195 → 16），故附件是交付差的唯一来源。
        cv::Mat aoContractPx;
        cv::compare(ab.on.ao, cv::Scalar(254), aoContractPx, cv::CMP_GT);  // > 254 ⟺ == 255
        leakMax = maxByteDiff(ab.colorOff, ab.on.frame.bgr, aoContractPx);
        leakPixels = cv::countNonZero(aoContractPx);
        cv::Mat darkPos, darkM, darkContact;
        cv::max(delta, cv::Scalar(0), darkPos);
        cv::compare(darkPos, cv::Scalar(1e-4), darkM, cv::CMP_GT);
        cv::bitwise_and(darkM, contact, darkContact);
        contactDark = static_cast<double>(cv::countNonZero(darkContact))
            / std::max(1.0, static_cast<double>(cv::countNonZero(contact)));
        std::printf("  阳性锚点（凹角接触环 floor↔box/sphere，膨胀 24px）\n");
        row("    AO 附件", contactStat);
        std::printf("      终帧字节Δ 均值 %s p99 %s 最大 %s 颗粒RMS %s；变暗像素 %s%%\n",
            d3(bContact.mean).c_str(), d3(bContact.p99).c_str(), d3(bContact.mx).c_str(),
            d3(bContact.grainRms).c_str(), d3(contactDark * 100.0).c_str());
        std::printf("      因果泄漏（AO 附件 = 255 的像素上终帧 A/B 差）：max %s LSB，像素 %lld"
                    "   ← 恒等式，必须恰为 0\n", d3(leakMax).c_str(),
            static_cast<long long>(leakPixels));
        // 反向对照：**凸边**带（球自己的轮廓带）——那里 AO 恒 1 才正确。它证明「变暗」是
        // 「凹角接触」的因果，而不是「靠近轮廓」的伪相关。
        const cv::Mat convex = bandOf(attr.ids, dist, oidOf("sphere"), 2.0, static_cast<double>(radius));
        const RegionStat convexStat = statRegion(ab.on.ao, convex);
        std::printf("  反向对照（球轮廓带，凸边 → AO 应恒 255）：均值 %s  <255 占 %s%%\n",
            d3(convexStat.mean).c_str(), d3(convexStat.fracBelowContract * 100.0).c_str());
        saveMaskOverlay(ab.on.frame.bgr, contact, outDir + "/g1_mask_contact.png");

        cv::Mat dev;
        cv::subtract(cv::Scalar(kAoContract), ab.on.ao, dev);  // 255 − AO = 遮蔽量
        cv::imwrite(outDir + "/g1_ao_occlusion.png", dev);
        saveAmp(dev, flat, 16.0, outDir + "/g1_flat_dev_x16.png");
        saveAmp(dev, bandLit, 4.0, outDir + "/g1_band_dev_x4.png");
        saveStretched(ab.on.ao, aoOnAll.lo, 255.0, outDir + "/g1_ao_stretched.png");
        cv::Mat dAbs;
        cv::convertScaleAbs(delta, dAbs, 255.0 * 8.0);
        cv::imwrite(outDir + "/g1_color_diff_x8.png", dAbs);

        contactMin = contactStat.lo;
        convexFracBelow = convexStat.fracBelowContract;
        contactByteGrain = bContact.grainRms;
        contactPixels = contactStat.pixels;
        byteFlatGrain = bFlat.grainRms;
        byteFlatP99 = bFlat.p99;
        byteFlatMax = bFlat.mx;
    }

    // ══ 组 2：AO 场取值分布 + 扫描线 ═══════════════════════════════════════════
    std::printf("\n── 组 2  AO 场取值分布（12 样本会不会走成离散台阶）─────────────\n");
    std::vector<int> hist(256, 0);
    for (int y = 0; y < ab.on.ao.rows; ++y) {
        const uint8_t* r = ab.on.ao.ptr<uint8_t>(y);
        for (int x = 0; x < ab.on.ao.cols; ++x)
            ++hist[r[x]];
    }
    int distinct = 0;
    for (int v = 0; v < 256; ++v)
        if (hist[v] > 0)
            ++distinct;
    std::printf("  不同取值 %d 个（12 样本若走成离散台阶应 ≈ 13）；255 占 %.2f%%\n", distinct,
        100.0 * hist[255] / (ab.on.ao.rows * ab.on.ao.cols));
    std::printf("  非 255 的取值（值:像素，按像素降序前 12）：");
    {
        std::vector<int> order(256);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return hist[a] > hist[b]; });
        int shown = 0;
        for (int v : order) {
            if (v == 255 || hist[v] == 0)
                continue;
            std::printf("%d:%d ", v, hist[v]);
            if (++shown >= 12)
                break;
        }
        std::printf("\n");
    }
    std::printf("  分桶（每 8 一档，自 224 起）：");
    for (int lo = 224; lo < 255; lo += 8) {
        int sum = 0;
        for (int v = lo; v < lo + 8 && v < 255; ++v)
            sum += hist[v];
        std::printf("[%d,%d):%d  ", lo, lo + 8, sum);
    }
    std::printf("[255]:%d\n", hist[255]);

    int bestRow = -1;
    double bestLap = 0.0;
    for (int y = 0; y < ab.on.ao.rows; y += 4) {
        const uint8_t* up = ab.on.ao.ptr<uint8_t>(std::max(0, y - 1));
        const uint8_t* cur = ab.on.ao.ptr<uint8_t>(y);
        const uint8_t* dn = ab.on.ao.ptr<uint8_t>(std::min(ab.on.ao.rows - 1, y + 1));
        double sum = 0.0;
        for (int x = 1; x + 1 < ab.on.ao.cols; ++x) {
            const double l = 2.0 * cur[x] - up[x] - dn[x];
            sum += l * l;
        }
        if (sum > bestLap) {
            bestLap = sum;
            bestRow = y;
        }
    }
    {
        const uint8_t* r = ab.on.ao.ptr<uint8_t>(bestRow);
        int lo = 255, loX = -1;
        for (int x = 0; x < ab.on.ao.cols; ++x)
            if (r[x] < lo) {
                lo = r[x];
                loX = x;
            }
        std::string prof;
        for (int x = std::max(0, loX - 10); x <= std::min(ab.on.ao.cols - 1, loX + 10); ++x)
            prof += std::to_string(r[x]) + " ";
        std::printf("  扫描线 y=%d（全图拉普拉斯能量最大的一行）：最小 %d @ x=%d\n    邻域 %s\n",
            bestRow, lo, loX, prof.c_str());
    }

    // ══ 组 3：亚像素爬行 ═══════════════════════════════════════════════════════
    std::printf("\n── 组 3  亚像素爬行（AO 是否锚在像素格上）─────────────────────\n");
    std::printf("  机位纯偏航（整数 %s px，深度无关，见 yawCam 注释）\n", d3(kShiftPx).c_str());
    {
        // 整数平移 = 精确算子（无插值 → 不引入额外高频）。两假设可判别：
        //   H_世界（噪声长在世界表面）：B ≈ shift(A) ⟹ 对齐后差 ≈ 0
        //   H_屏幕（噪声长在像素格）  ：B ≈ A        ⟹ 对齐后差反而变大
        const int dx = static_cast<int>(kShiftPx);
        const auto gray = [](const cv::Mat& m) {
            cv::Mat g;
            cv::cvtColor(m, g, cv::COLOR_BGR2GRAY);
            return g;
        };
        const cv::Mat baseG = gray(ab.colorOff), crawlBaseG = gray(crawl.colorOff);
        const cv::Mat valid = shiftX(cv::Mat(attr.ids.size(), CV_8UC1, cv::Scalar(255)), dx);
        cv::Mat mFlat;
        cv::bitwise_and(flat, shiftX(flat, dx), mFlat);
        cv::bitwise_and(mFlat, valid, mFlat);

        // **算子的阳性锚点**：同一算子作用在底图（AO 关的终帧）上。底图是几何 + 视相关
        // 高光，几何**必然**跟着相机走 ⟹ 对齐后差必须显著下降。锚点必须在**有结构**的区域
        // 上求（契约区的底图本身零高频，锚在它上面是 0/0，什么也验不出），故用全图。
        // 平移符号也由锚点定（yawCam 的 Δpixel = −f_px·θ，符号约定容易记错；符号错则
        // 「对齐」是反向操作，整组结论反向）——用底图定符号是合法的：它的正确行为有闭式。
        baseUnaligned = rmsDiff(baseG, crawlBaseG, allMask);
        const double pos = rmsDiff(baseG, shiftX(crawlBaseG, dx), allMask);
        const double neg = rmsDiff(baseG, shiftX(crawlBaseG, -dx), allMask);
        const int sign = pos <= neg ? 1 : -1;
        baseAligned = std::min(pos, neg);
        std::printf("  算子阳性锚点（底图，全图）：未对齐 RMS %s → 对齐后 %s（符号 %+d px）→ %s\n",
            d3(baseUnaligned).c_str(), d3(baseAligned).c_str(), sign,
            baseAligned < baseUnaligned ? "符合预期（底图能被全局平移解释）"
                                        : "⚠ 锚点不成立，本组结论作废");

        // 判读方向（搞反就整组反向，故写死在注释里）：
        //   图案跟着几何走（世界锚定）⟹ B = shift(A) ⟹ **对齐后**差变小
        //   图案长在像素格上（屏幕锚定）⟹ B ≈ A      ⟹ **未对齐**差就小、对齐后反而变大
        const double aoUn = rmsDiff(ab.on.ao, crawl.on.ao, mFlat);
        const double aoAl = rmsDiff(ab.on.ao, shiftX(crawl.on.ao, sign * dx), mFlat);
        crawlAlignedWorse = aoAl > aoUn;  // true = 屏幕锚定
        const RegionStat un = statRegion(ab.on.ao, mFlat);
        std::printf("  AO 场（契约区）：未对齐差 RMS %s → 对齐后 %s（%.2f×）\n", d3(aoUn).c_str(),
            d3(aoAl).c_str(), aoUn > 0.0 ? aoAl / aoUn : 0.0);
        std::printf("    → %s\n", crawlAlignedWorse
                ? "对齐后**更大** = 图案没跟着几何走 = 长在像素格上（屏幕锚定）"
                : "对齐后**更小** = 图案跟着几何走（世界锚定）");

        // 集合版本（回答「偏离契约的是不是同一批像素」）：RMS 是幅度，Jaccard 是身份。
        cv::Mat devA, devB;
        cv::compare(ab.on.ao, cv::Scalar(254), devA, cv::CMP_LT);
        cv::compare(crawl.on.ao, cv::Scalar(254), devB, cv::CMP_LT);
        const double jScreen = jaccard(devA, devB, mFlat);
        const double jWorld = jaccard(devA, shiftX(devB, sign * dx), mFlat);
        std::printf("  偏离契约像素集合 Jaccard：同屏 %s  对齐后 %s → %s\n", d3(jScreen).c_str(),
            d3(jWorld).c_str(),
            jScreen >= jWorld ? "同一批屏幕像素在偏离（屏幕锚定）" : "同一批世界位置在偏离（世界锚定）");
        // 两个**独立**算子（幅度 RMS 与集合重合度）必须给同一个结论；不一致 = 判据本身失效，
        // 此时任何一侧的读数都不能写进报告。
        crawlAgree = (aoAl > aoUn) == (jScreen >= jWorld);
        crawlScreenAnchored = crawlAlignedWorse && jScreen >= jWorld;
        std::printf("  契约区抖动RMS（单帧内）%s LSB\n", d3(un.jitterRms).c_str());

        cv::Mat d, dAbs;
        cv::subtract(ab.on.ao, crawl.on.ao, d);
        cv::convertScaleAbs(d, dAbs, 8.0);
        cv::imwrite(outDir + "/g3_crawl_ao_x8.png", dAbs);
        cv::Mat db, dbAbs;
        cv::absdiff(ab.colorOff, crawl.colorOff, db);
        cv::convertScaleAbs(db, dbAbs, 8.0);
        cv::imwrite(outDir + "/g3_crawl_base_x8.png", dbAbs);
    }

    // ══ 组 5：掠射角对照 ═══════════════════════════════════════════════════════
    std::printf("\n── 组 5  掠射角对照（候选根因 B3/B4 的指纹）───────────────────\n");
    {
        const cv::Mat gEdges = idEdges(grazeCap.frame.ids);
        const cv::Mat gDist = distToEdge(gEdges);
        cv::Mat gFlat = cv::Mat::zeros(grazeCap.frame.ids.size(), CV_8UC1);
        for (const auto& [id, nm] : at.nameByOid)
            cv::bitwise_or(gFlat,
                flatRegion(grazeCap.frame.ids, gDist, id, static_cast<double>(radius)), gFlat);
        const RegionStat gStat = statRegion(grazeCap.ao, gFlat);
        std::printf("  %-24s px %8lld 均值 %-9s σ %-9s 最小 %-6.0f <255 占 %-8s 抖动RMS %s\n",
            "掠射 契约区", static_cast<long long>(gStat.pixels), d3(gStat.mean).c_str(),
            d3(gStat.sd).c_str(), gStat.lo, d3(gStat.fracBelowContract * 100.0).c_str(),
            d3(gStat.jitterRms).c_str());
        std::printf("  正视 → 掠射：σ %s → %s（%.2f×）；抖动RMS %s → %s（%.2f×）\n",
            d3(aoOnFlat.sd).c_str(), d3(gStat.sd).c_str(),
            aoOnFlat.sd > 0.0 ? gStat.sd / aoOnFlat.sd : 0.0, d3(aoOnFlat.jitterRms).c_str(),
            d3(gStat.jitterRms).c_str(),
            aoOnFlat.jitterRms > 0.0 ? gStat.jitterRms / aoOnFlat.jitterRms : 0.0);
        cv::Mat dev;
        cv::subtract(cv::Scalar(kAoContract), grazeCap.ao, dev);
        saveAmp(dev, gFlat, 16.0, outDir + "/g5_graze_dev_x16.png");
        cv::imwrite(outDir + "/g5_graze_occlusion.png", dev);
    }

    // ══ 组 4：TAA 开时的时间行为 ═══════════════════════════════════════════════
    // 地板（AO 关）与信号（AO 开）在**同一进程**内取，且都走 /shotburst（连续帧）。
    int64_t floorChanged = 0;
    bool temporalHasResolution = false;
    if (doTemporal) {
        std::printf("\n── 组 4  TAA 开（同进程 AO 关=地板 / AO 开=信号）──────────────\n");
        const std::string dir = profileOf("taa", strength, true, 0.12);
        Engine eng(dir);
        // 抖动序列每 kJitterPeriod 帧一轮（`kJitterPeriod` = TAA jitter 周期）。两轮采集的
        // **帧数间隔取成周期的整数倍**，AO 关与 AO 开这两段 burst 就从**同一个抖动相位**起步
        // —— 否则「AO 开比 AO 关抖得多」可能只是相位不同，而不是 AO 的贡献。
        const uint32_t nBurst = static_cast<uint32_t>(
            ((pairs + 1 + kJitterPeriod - 1) / kJitterPeriod) * kJitterPeriod);
        const uint32_t kWarmFrames = 6 * static_cast<uint32_t>(kJitterPeriod);
        std::printf("  burst %u 帧（预热 %u 帧洗历史；两轮间隔 %u 帧 = 抖动周期的整数倍）\n", nBurst,
            kWarmFrames, kWarmFrames + nBurst);
        const auto burstDiff = [&](const char* tag, double aoStrength) {
            eng.setParam(kAoPage, kAoLabel, aoStrength);
            // 强度切换要洗掉 TAA 历史（feedback 0.12 ⟹ 收敛要几十帧），先丢弃预热帧。
            static_cast<void>(eng.shotBurst("screenshots", kWarmFrames, "aot_w" + std::string(tag)));
            const auto files = eng.shotBurst("screenshots", nBurst, "aot_c" + std::string(tag));
            std::array<FrameDiff, 3> acc;
            int64_t changed = 0;
            for (size_t i = 1; i < files.size(); ++i) {
                const cv::Mat x = readPngBgr(files[i - 1]), y = readPngBgr(files[i]);
                // 0 = 全图，1 = 契约区，2 = 接触环
                const std::array<FrameDiff, 3> d{diffFrames(x, y), diffFrames(x, y, flat),
                    diffFrames(x, y, contactMask)};
                for (size_t k = 0; k < acc.size(); ++k) {
                    acc[k].changed += d[k].changed;
                    acc[k].mean += d[k].mean / static_cast<double>(files.size() - 1);
                    acc[k].mx = std::max(acc[k].mx, d[k].mx);
                }
                changed += d[0].changed;
            }
            for (auto& a : acc)
                a.changed /= static_cast<int64_t>(files.size() - 1);
            keep(files[0], outDir, std::string("g4_") + tag + "_f0.png");
            keep(files[1], outDir, std::string("g4_") + tag + "_f1.png");
            cv::Mat dd;
            cv::absdiff(readPngBgr(files[0]), readPngBgr(files[1]), dd);
            cv::Mat dAbs;
            cv::convertScaleAbs(dd, dAbs, 255.0 * 16.0);
            cv::imwrite(outDir + "/g4_" + tag + "_diff_x16.png", dAbs);
            return std::pair<std::array<FrameDiff, 3>, int64_t>{acc, changed};
        };
        const auto off = burstDiff("off", 0.0);
        const auto on = burstDiff("on", strength);
        floorChanged = off.second;
        const auto trow = [](const char* name, const FrameDiff& f, const FrameDiff& s) {
            std::printf("  %-14s 地板 非零 %8lld 均值 %-9s 最大 %-5.0f │ 信号 非零 %8lld 均值 %-"
                        "9s 最大 %-5.0f │ 比值 %s×\n",
                name, static_cast<long long>(f.changed), d3(f.mean).c_str(), f.mx,
                static_cast<long long>(s.changed), d3(s.mean).c_str(), s.mx,
                f.mean > 0.0 ? d3(s.mean / f.mean).c_str() : "inf");
        };
        std::printf("\n");
        trow("全图", off.first[0], on.first[0]);
        trow("契约区（平坦）", off.first[1], on.first[1]);
        trow("接触环（AO 生效）", off.first[2], on.first[2]);
        std::printf(
            "  ⚠ 地板非 0（TAA jitter 自己每帧都在改画面）⟹ **比值不可解释**（分母不是零，\n"
            "     也不是「AO 之外唯一变量」的对照，正是已登记反模式 screen-space-l1-frame-diff\n"
            "     的形态）。本组只作观察量；AO 的**时间**结论走 TAA 关那两条零锚点：\n"
            "     ① 静止机位下同参数连拍逐位相同（确定性锚①）⟹ 静止时无逐帧噪声；\n"
            "     ② 机位动 1 px 时图案不跟着几何走（组 3）⟹ 噪声是「爬行」而不是「闪烁」。\n");
        temporalHasResolution = off.first[1].mean > 0.0 && on.first[1].mean > 0.0
            && std::abs(on.first[1].mean / off.first[1].mean - 1.0) > 0.05;
    }

    // ══ 组 6：交付场景（可选）════════════════════════════════════════════════
    // 合成靶证明了机制与幅度，但「在真实场景里多大」只能问交付配置。
    // ⚠ 本组**不产出契约类判据**：bistro 有 395 张纹理，底图自身高频不是 0，「契约区」
    //   概念在这里不成立（见头注释）。唯一能带进真实场景的是**因果泄漏恒等式**——它
    //   不需要任何区域，只问「附件说没遮蔽的地方，画面动没动」。
    double bistroLeak = 0.0;
    int64_t bistroLeakPx = 0;
    bool bistroLeakOk = false;
    if (doBistro) {
        std::printf("\n── 组 6  交付场景（configs/ 原样，机位 = 交付 camera.lua）────────\n");
        ProfileOpts o;
        o.deliveryConfigDir = projectRoot() + "/configs";  // 不重抄 ao/env/shadow（第二真相）
        const std::string dir = profRoot + "/imgqa_ao_bistro";
        makeProfile(dir, o);
        Engine eng(dir);
        const AbPair b = captureAb(eng, nullptr, strength, outDir, "g6", /*withId=*/false);
        const cv::Mat allB(b.on.ao.size(), CV_8UC1, cv::Scalar(255));
        cv::Mat active, contractB;
        cv::compare(b.on.ao, cv::Scalar(254), active, cv::CMP_LT);    // AO 生效处
        cv::compare(b.on.ao, cv::Scalar(254), contractB, cv::CMP_GT); // AO = 255 处
        const RegionStat allStat = statRegion(b.on.ao, allB, -1.0);
        const RegionStat actStat = statRegion(b.on.ao, active, -1.0);
        const ByteAB bAllB = byteAbsDiff(b.colorOff, b.on.frame.bgr, allB);
        const ByteAB bAct = byteAbsDiff(b.colorOff, b.on.frame.bgr, active);
        std::printf("  尺寸 %dx%d（交付 headless_size，非本工具的 %dx%d）\n", b.on.ao.cols,
            b.on.ao.rows, kAoSize[0], kAoSize[1]);
        std::printf("  AO 附件 全图  均值 %-9s σ %-9s 最小 %-6.0f  <255 占 %-9s 抖动RMS %s\n",
            d3(allStat.mean).c_str(), d3(allStat.sd).c_str(), allStat.lo,
            d3(active.empty() ? 0.0 : 100.0 * cv::countNonZero(active) / allB.total()).c_str(),
            d3(allStat.jitterRms).c_str());
        std::printf("  AO 附件 生效处 均值 %-9s σ %-9s 最小 %-6.0f  像素 %-9lld 抖动RMS %s\n",
            d3(actStat.mean).c_str(), d3(actStat.sd).c_str(), actStat.lo,
            static_cast<long long>(actStat.pixels), d3(actStat.jitterRms).c_str());
        const auto brow = [](const char* name, const ByteAB& v) {
            std::printf("  交付字节Δ %-10s 均值 %-9s p99 %-9s 最大 %-8s 渐变RMS %-9s 颗粒RMS %s\n",
                name, d3(v.mean).c_str(), d3(v.p99).c_str(), d3(v.mx).c_str(),
                d3(v.smoothRms).c_str(), d3(v.grainRms).c_str());
        };
        brow("全图", bAllB);
        brow("AO 生效处", bAct);
        std::printf("  颗粒/渐变：全图 %s  生效处 %s   ← 真实场景里 AO 加的东西有多少是颗粒\n",
            bAllB.smoothRms > 0.0 ? d3(bAllB.grainRms / bAllB.smoothRms).c_str() : "inf",
            bAct.smoothRms > 0.0 ? d3(bAct.grainRms / bAct.smoothRms).c_str() : "inf");
        bistroLeak = maxByteDiff(b.colorOff, b.on.frame.bgr, contractB);
        bistroLeakPx = cv::countNonZero(contractB);
        bistroLeakOk = bistroLeak == 0.0;
        cv::Mat dev;
        cv::subtract(cv::Scalar(kAoContract), b.on.ao, dev);
        cv::imwrite(outDir + "/g6_bistro_ao_occlusion.png", dev);
        cv::Mat dAbs;
        cv::absdiff(b.colorOff, b.on.frame.bgr, dev);
        cv::convertScaleAbs(dev, dAbs, 8.0);
        cv::imwrite(outDir + "/g6_bistro_color_diff_x8.png", dAbs);
        keep(projectRoot() + "/build/rgl/screenshots/imgqa_g6_on_color.png", outDir,
            "g6_bistro_color_on.png");
        keep(projectRoot() + "/build/rgl/screenshots/imgqa_g6_coloroff.png", outDir,
            "g6_bistro_color_off.png");
    }

    // ═══════════════════════════════════════════════════════════════════════════ 判定
    std::printf("\n══════════════════════════ 判定 ══════════════════════════\n");
    std::vector<std::tuple<std::string, bool, std::string>> checks;
    const auto add = [&](const char* name, bool pass, const std::string& detail) {
        checks.emplace_back(name, pass, detail);
    };

    add("确定性锚①：同参数连拍逐位相同", ab.repeatable,
        ab.repeatable ? "逐位相同" : "同参数两帧不同 —— 帧间有未定来源");
    add("确定性锚②：强度切回后逐位复原（兼排除 /param 未生效）", ab.reversible,
        ab.reversible ? "切回后逐位相同" : "切回后不同 —— /param 未生效或有残留状态");
    add("反向用例：strength=0 时 AO 附件逐位 255", aoOffFlat.lo >= kAoContract,
        "契约区最小 " + std::to_string(static_cast<int>(aoOffFlat.lo)) + "（要求 255）");
    // 阳性锚点的口径是「凹角处**存在**闭式要求的深遮蔽」+「凸边处**没有**遮蔽」，
    // 不是「接触环平均被压暗」——环有 24 px 宽，绝大多数像素按闭式就该是 1，
    // 拿均值当锚点会把「锚点太宽」误读成「AO 没跑」（首跑正是这样假阴的）。
    add("阳性锚点：凹角接触环存在深遮蔽（闭式说这里必须 <1）",
        contactPixels > 500 && contactMin < 128.0, "接触环最小 AO " + d3(contactMin)
            + "（要求 < 128 = 半径 0.5 内贴地凹角必须明显遮蔽）、像素 " + std::to_string(contactPixels));
    add("反向对照：凸边（球轮廓带）不被遮蔽", convexFracBelow < 0.02,
        "球轮廓带 <255 占 " + d3(convexFracBelow * 100.0) + "%（要求 < 2%）——证明「变暗」是"
        "凹角接触的因果，不是「靠近轮廓」的伪相关");
    add("因果泄漏：AO 附件 = 255 处交付图逐位未变（恒等式）", leakMax == 0.0,
        "AO=255 的 " + std::to_string(leakPixels) + " 像素上 A/B 差 max " + d3(leakMax)
            + " LSB（要求恰为 0）；非 0 = 附件之外还有第二处随强度变的东西");
    // 「离轮廓 > radius 像素」**不是**世界空间的契约区：AO 半径 0.5 是世界量，靠近相机的表面
    // 在轮廓内 40 px 处世界距离可以远小于 0.5 m（实测 box_short 正面上离轮廓 > 40 px 处 AO 低到
    // 37，那是合法遮蔽不是噪声）。故偏离契约值只作**观察量**，不当判据；噪声判据走高频项
    // （高频项对合法的光滑遮蔽梯度不敏感，这才是区域定义不干净时仍然成立的量）。
    std::printf("\n  观察量：契约区偏离契约值 <255 占 %s%%、AO 最大偏离 %s LSB、交付字节Δ 最大 %s LSB"
                "\n         —— 两者同源污染（近处表面在轮廓内被合法遮蔽），均不作判据\n",
        d3(aoOnFlat.fracBelowContract * 100.0).c_str(), d3(kAoContract - aoOnFlat.lo).c_str(),
        d3(byteFlatMax).c_str());
    add("主判据：契约区 AO 场逐像素抖动 RMS（字节域）", aoOnFlat.jitterRms <= 1.0,
        "抖动RMS " + d3(aoOnFlat.jitterRms) + " LSB（要求 ≤ 1.0）；底图同算子 "
            + d3(baseFlat.jitterRms) + " 是分母");
    add("交付画面：契约区 A/B 差的颗粒项（字节域，AO≈契约值处）",
        byteFlatGrain <= 1.0, "颗粒RMS " + d3(byteFlatGrain)
            + " LSB（要求 ≤ 1.0 = 量化步长，低于它噪声被 8bit 量化吞掉）、p99 " + d3(byteFlatP99));
    add("交付画面：接触环（AO 真正起作用处）A/B 差的颗粒项", contactByteGrain <= 2.0,
        "颗粒RMS " + d3(contactByteGrain) + " LSB（要求 ≤ 2.0；此处含接触遮蔽的**台阶**残差，"
        "medianBlur 减不掉阶跃，故阈值放宽到 2×量化步长，且只作下限参考）");
    add("爬行：算子阳性锚点成立（底图能被全局平移解释）", baseAligned < baseUnaligned,
        "底图未对齐 RMS " + d3(baseUnaligned) + " → 对齐后 " + d3(baseAligned)
            + "；不成立则爬行组结论作废");
    add("爬行：幅度算子与集合算子结论一致（不一致 = 判据失效）", crawlAgree,
        crawlScreenAnchored ? "两个算子一致：**屏幕锚定**（噪声长在像素格上 ⟹ 相机一动就爬行）"
                            : "两个算子一致：世界锚定（噪声跟着表面走，相机一动图案一起动）");
    if (doBistro) {
        // 唯一能带进真实场景的判据：它不问「这块该多亮」（bistro 没有闭式），只问恒等式。
        add("因果泄漏（交付场景）：AO 附件 = 255 处交付图逐位未变", bistroLeakOk,
            "AO=255 的 " + std::to_string(bistroLeakPx) + " 像素上 A/B 差 max " + d3(bistroLeak)
                + " LSB（要求恰为 0）");
    }
    if (doTemporal) {
        // 阳性锚点：burst 必须真的在交**不同**的帧。若引擎把同一张文件交两次，逐帧差全 0，
        // 「AO 没有时间噪声」会假绿 —— 故这条钉的是量具本身，不是 AO。
        add("时域量具阳性锚点：burst 逐帧确实在变（地板 > 0）", floorChanged > 0,
            "地板非零像素合计 " + std::to_string(floorChanged)
                + "（= 0 说明 burst 交的是同一帧，本组任何读数都是假的）");
        std::printf("  [观察] TAA 开态下 AO 的时间贡献：契约区逐帧差%s（地板 = TAA jitter 自身，"
                    "非零）\n",
            temporalHasResolution ? "与地板有 >5% 的差（AO 的时间贡献可见）"
                                  : "与地板无 >5% 的差（AO 的时间贡献淹在 jitter 里，本量具测不出）");
    }

    int failed = 0;
    for (const auto& [name, pass, detail] : checks) {
        std::printf("  [%s] %-56s %s\n", pass ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        if (!pass)
            ++failed;
    }
    int nFiles = 0;
    for (const auto& e : fs::directory_iterator(outDir, ec)) {
        static_cast<void>(e);
        ++nFiles;
    }
    std::printf("\n产物目录：%s（%d 个文件）\n", outDir.c_str(), nFiles);
    return failed == 0 ? 0 : 1;
}

}  // namespace RGL::imgqa
