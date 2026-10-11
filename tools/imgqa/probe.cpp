// probe.cpp —— 闭式解探针（从 main.cpp 拆出的独立仪器）。
//
// ## 缝在哪
//
// 缝 = **一台仪器一个 TU**（本仓既有先例：aa_coverage.cpp / shadow_softness.cpp）。probe 与
// main.cpp 里其余命令不同源：它不吃 `--glb`/manifest，而是**消费一份冻结的配置目录**
// （qa/configs/analytic，六个门逐项关掉写在配置注释里）+ 一个像素坐标，出「闭式解该是多少 vs
// 实际是多少」。它与 analytic.cpp 是同一条线（12.3b 接替 analytic_ref.py --probe），改动动机
// 永远是"着色链的哪个前提变了 / 六个门有没有真的关上"，与区域归因、影场两条判据族无关。
//
// ## 搬动的纪律（纯重构）
//
// 函数体、阈值与打印从 main.cpp **整块照搬**，一个字节未改。护栏点名 cmdProbe 认知复杂度
// 20 > 15 ⟹ 把 `--light/--intensity` 那条"复制配置副本 + 只改副本的 light 块"的分支抽成
// copyProbeConfigWithLight（它自带 4 个 npos 三元 + 5 项定位校验 = 复杂度的全部来源）；
// 判据本体（shadeDirect → toBytes → 逐通道比阈值）留在原处逐字未动。
#include "imgqa.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace RGL::imgqa {

namespace {

/// 把探针配置整目录复制到 build/rgl/imgqa_probe_profile，并**只改副本**的 light 块
/// （direction = −L、intensity = 给定值）。定位不到 direction/intensity = 抛错停下，绝不
/// "没改到也照跑"——那等于拿原配置的图去对一个新光向的预测（自欺判据）。
/// @return 副本目录，调用侧拿它当 runDir
std::string copyProbeConfigWithLight(const std::string& configDir, Vec3 l, double intensity)
{
    namespace fs = std::filesystem;
    const fs::path dst = fs::path(projectRoot()) / "build/rgl/imgqa_probe_profile";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst);
    for (const auto& e : fs::directory_iterator(configDir))
        fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing);
    const auto num = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g", v);
        return std::string(buf);
    };
    std::ifstream in(dst / "scene.lua");
    std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const size_t lp = txt.find("light = {");
    const size_t dp = lp == std::string::npos ? lp : txt.find("direction = {", lp);
    const size_t de = dp == std::string::npos ? dp : txt.find('}', dp);
    const size_t ip = dp == std::string::npos ? dp : txt.find("intensity = ", dp);
    const size_t ie = ip == std::string::npos ? ip : txt.find_first_of(",\n}", ip);
    if (lp == std::string::npos || dp == std::string::npos || de == std::string::npos
        || ip == std::string::npos || ie == std::string::npos)
        throw std::runtime_error("scene.lua 里定位不到 light 块（--light 覆写要求 direction/intensity 齐备）");
    txt.replace(dp, de - dp + 1,
        "direction = { " + num(-l.x) + ", " + num(-l.y) + ", " + num(-l.z) + " }");
    const size_t ip2 = txt.find("intensity = ", dp);  // 前一次 replace 可能挪了偏移
    const size_t ie2 = txt.find_first_of(",\n}", ip2);
    txt.replace(ip2, ie2 - ip2, "intensity = " + num(intensity));
    std::ofstream(dst / "scene.lua") << txt;
    return dst.string();
}

}  // namespace

// ── probe：闭式解探针（12.3b 接替 analytic_ref.py --probe；配置即契约）──
int cmdProbe(const std::string& configDir, int x, int y, Vec3 albedo, double roughness,
    double metallic, Vec3 l, double intensity, int tol, bool lightOverride)
{
    // 探针配置 qa/configs/analytic 是**冻结的已知量集合**：六个门逐项关掉写在配置
    // 注释里，着色链塌成闭式可解。默认模式工具不重写这份配置，只消费它。
    // 显式 --light/--intensity（lightOverride）：整目录复制到 build/rgl/imgqa_probe_profile
    // 后**只改副本**的 light 块（direction = -L）。不接这根线时 --light 改预测不改渲染，
    // 等于自欺判据——2026-10-01 黑面调查同日修复（此前该选项只动预测侧）。
    std::string runDir = configDir;
    if (lightOverride)
        runDir = copyProbeConfigWithLight(configDir, l, intensity);
    runEngineOnce(runDir);
    const std::string png = projectRoot() + "/build/rgl/screenshots/analytic_probe.png";
    const cv::Mat bgr = readPngBgr(png);
    if (x < 0 || y < 0 || x >= bgr.cols || y >= bgr.rows)
        throw std::runtime_error("探针像素越界: (" + std::to_string(x) + ","
            + std::to_string(y) + ") vs " + std::to_string(bgr.cols) + "x"
            + std::to_string(bgr.rows));

    // 适用范围（承 Python 版模块头）：探针面无纹理/无 normal map、法线轴对齐，
    // 相机正视 ⟹ N=V=(0,0,1)。任何一项被改写过，先改这里的前提再谈着色链。
    const Vec3 linear = shadeDirect({0, 0, 1}, {0, 0, 1}, l, albedo, roughness, metallic,
        {intensity, intensity, intensity});
    const auto want = toBytes(linear);
    const cv::Vec3b px = bgr.at<cv::Vec3b>(y, x);  // OpenCV BGR 序
    const std::array<int, 3> got{px[2], px[1], px[0]};
    const std::array<int, 3> diff{
        std::abs(got[0] - want[0]), std::abs(got[1] - want[1]), std::abs(got[2] - want[2])};
    const bool ok = diff[0] <= tol && diff[1] <= tol && diff[2] <= tol;

    std::printf("探针 (%d,%d)  配置=%s%s\n", x, y, runDir.c_str(),
        lightOverride ? "（light 覆写副本）" : "");
    std::printf("  N=V=(0,0,1)  L=(%.6f,%.6f,%.6f)  albedo=(%.2f,%.2f,%.2f)"
        "  rough=%.2f metal=%.2f  I=%.1f\n",
        l.x, l.y, l.z, albedo.x, albedo.y, albedo.z, roughness, metallic, intensity);
    std::printf("  线性预测 = (%.6f, %.6f, %.6f)\n", linear.x, linear.y, linear.z);
    std::printf("  预测字节 = (%d, %d, %d)\n", want[0], want[1], want[2]);
    std::printf("  实际字节 = (%d, %d, %d)\n", got[0], got[1], got[2]);
    std::printf("  差       = (%d, %d, %d)   （阈值 %d）\n", diff[0], diff[1], diff[2], tol);
    const Verdict v = verdictFor("probe-pixel", ok,
        "(" + std::to_string(x) + "," + std::to_string(y) + ") 预测("
        + std::to_string(want[0]) + "," + std::to_string(want[1]) + ","
        + std::to_string(want[2]) + ") vs 实际(" + std::to_string(got[0]) + ","
        + std::to_string(got[1]) + "," + std::to_string(got[2]) + ")");
    std::cout << "  " << v.line() << "\n";
    if (!ok) {
        std::printf("  归因顺序（先怀疑前提，再怀疑引擎）：\n"
                    "    ① 探针面是否真的无纹理 / 无 normal map（适用范围）\n"
                    "    ② N/V/L 是否真如假设（机位与光向是否被相机模式改写过）\n"
                    "    ③ normalFiltering 是否抬了 roughness（平面正视图应为 0 方差）\n"
                    "    ④ 六个配置门是否都真的关上了（少关一项就少一个已知量）\n"
                    "    ⑤ 以上都排除了，才轮到怀疑着色链\n");
    }
    logCall("probe", {{"config", configDir}, {"xy", std::to_string(x) + "," + std::to_string(y)},
        {"light", std::to_string(l.x) + "," + std::to_string(l.y) + "," + std::to_string(l.z)},
        {"tol", std::to_string(tol)}}, {v});
    return v.ok ? 0 : 1;
}

}  // namespace RGL::imgqa
