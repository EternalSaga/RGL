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
// ## 2026-10-18 重立：forward 单锚 → deferred 分段双锚（docs/图变体收敛_plan.md §6.1-1）
//
// forward 变体删除级联的硬前置是「闭式解探针不许失去唯一的无基线判据」。重立后的形态：
//
//   锚① gbuffer_albedo   期望字节 = round(srgbEncode(albedo)·255)（纯编码闭式，零光照建模）
//   锚② gbuffer_metalrough 期望字节 = round(metal·255) / round(rough·255)
//   锚③ 最终像素          闭式 = shadeDirect(**量化往返后的输入**) → ACES → sRGB → 字节
//
// 红灯即分段归因：①/② 红 = G-buffer 编码契约变了（gbuffer_contract / deferred_geometry 写侧）；
// ①② 绿而 ③ 红 = light pass 或 tonemap 尾段。锚①② 是 forward 下**不可能存在**的判据——
// 删 forward 不是失去无基线判据，是把它变深、变多。
//
// 取图 = RPC 单启动三拍（后缓冲 + dump_gbuf 通道 0/3，线性原字节无 gamma）。为此工具
// **总是**复制冻结目录并改两个回读管线键（diagnostics.rpc=true、runtime.llm_debug=false
// ——后者不自关会第 8 帧自截自退，且 RPC /shot 被 409 拒）；**渲染门一个不碰**，
// 冻结的已知量集合仍是契约本体。
//
// 光照式两路逐字相同（light.frag shadeLight ≡ model_pbr_ibl shadeDirect，连 epsilon 一致），
// tonemap 尾段两路共用同一个 tonemapPass() ⟹ 闭式模型只补 G-buffer 量化往返（analytic.cpp）。
#include "imgqa.h"

#include <algorithm>
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

std::string readFileText(const std::filesystem::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到探针配置: " + p.string());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void writeFileText(const std::filesystem::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

/// 把冻结配置里的一处 `key = <old>` 改成 `key = <new>`；定位不到 = 抛错停下，
/// 绝不"没改到也照跑"（回读管线键没翻 = 引擎第 8 帧自退 / RPC 永不可达，都是白跑一场）。
void rewriteKey(const std::filesystem::path& file, const std::string& key,
    const std::string& from, const std::string& to)
{
    std::string txt = readFileText(file);
    const std::string needle = key + " = " + from;
    const size_t pos = txt.find(needle);
    if (pos == std::string::npos)
        throw std::runtime_error(file.string() + " 里定位不到 '" + needle
            + "'（回读管线键被改名？先对配置再跑探针）");
    txt.replace(pos, needle.size(), key + " = " + to);
    writeFileText(file, txt);
}

/// 把探针配置整目录复制到 build/rgl/imgqa_probe_profile 并改写成 RPC 形态：
/// diagnostics.rpc → true、runtime.llm_debug → false；lightOverride 时**只改副本**的
/// light 块（direction = −L、intensity = 给定值；不接这根线时 --light 改预测不改渲染，
/// 等于自欺判据——2026-10-01 黑面调查同日修复）。渲染六门逐项不碰。
/// @return 副本目录，调用侧拿它当 runDir
std::string stageProbeProfile(const std::string& configDir, bool lightOverride, Vec3 l,
    double intensity)
{
    namespace fs = std::filesystem;
    const fs::path dst = fs::path(projectRoot()) / "build/rgl/imgqa_probe_profile";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst);
    for (const auto& e : fs::directory_iterator(configDir))
        fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing);

    rewriteKey(dst / "diagnostics.lua", "rpc", "false", "true");
    rewriteKey(dst / "runtime.lua", "llm_debug", "true", "false");

    if (!lightOverride)
        return dst.string();
    const auto num = [](double v) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.9g", v);
        return std::string(buf);
    };
    std::string txt = readFileText(dst / "scene.lua");
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
    writeFileText(dst / "scene.lua", txt);
    return dst.string();
}

/// 读 PNG 并取指定像素的 RGB 字节（readPngBgr 给 BGR 序，这里翻回 R,G,B 与预测同序）。
std::array<int, 3> pixelRgb(const std::string& png, int x, int y, const char* what)
{
    const cv::Mat bgr = readPngBgr(png);
    if (x < 0 || y < 0 || x >= bgr.cols || y >= bgr.rows)
        throw std::runtime_error(std::string(what) + " 探针像素越界: ("
            + std::to_string(x) + "," + std::to_string(y) + ") vs "
            + std::to_string(bgr.cols) + "x" + std::to_string(bgr.rows));
    const cv::Vec3b px = bgr.at<cv::Vec3b>(y, x);
    return {px[2], px[1], px[0]};
}

[[nodiscard]] int maxAbsDiff(const std::array<int, 3>& got, const std::array<int, 3>& want)
{
    return std::max({std::abs(got[0] - want[0]), std::abs(got[1] - want[1]),
        std::abs(got[2] - want[2])});
}

std::string bytesText(const std::array<int, 3>& b)
{
    return "(" + std::to_string(b[0]) + "," + std::to_string(b[1]) + ","
        + std::to_string(b[2]) + ")";
}

}  // namespace

// ── probe：闭式解探针（12.3b 接替 analytic_ref.py --probe；配置即契约；10-18 双锚重立）──
int cmdProbe(const std::string& configDir, int x, int y, Vec3 albedo, double roughness,
    double metallic, Vec3 l, double intensity, int tol, bool lightOverride)
{
    // 探针配置 qa/configs/analytic 是**冻结的已知量集合**：六个门逐项关掉写在配置
    // 注释里，着色链塌成闭式可解。工具只改回读管线键（rpc/llm_debug）与显式 --light
    // 覆写，渲染门一个不碰。
    const std::string runDir = stageProbeProfile(configDir, lightOverride, l, intensity);
    Engine eng(runDir);
    eng.advanceFrames(8);  // 热身按帧数（原 llm_shot delay_frames=8 同帧数；墙钟睡会量到瞬态）
    const std::string swapPng = eng.shot("probe");
    const std::string gbAlbedoPng = eng.shot("probe_gbuf0", nullptr, 0);
    const std::string gbMetalRoughPng = eng.shot("probe_gbuf3", nullptr, 3);

    const auto gotAlbedo = pixelRgb(gbAlbedoPng, x, y, "gbuffer_albedo");
    const auto gotMetalRough = pixelRgb(gbMetalRoughPng, x, y, "gbuffer_metalrough");
    const auto gotPixel = pixelRgb(swapPng, x, y, "swapchain");

    // ── 锚①②：G-buffer 编码契约（期望 = 存储字节本身；dump_gbuf 直出原字节无 gamma）。
    // metalrough 只 R=.r=metallic、G=.g=roughness 两通道有意义（B 恒 0、A 恒 1 不入判据）。
    const std::array<int, 3> wantAlbedo{
        encode8Srgb(albedo.x), encode8Srgb(albedo.y), encode8Srgb(albedo.z)};
    const std::array<int, 3> wantMetalRough{static_cast<int>(std::lround(
            static_cast<float>(metallic) * 255.0f)),
        static_cast<int>(std::lround(static_cast<float>(roughness) * 255.0f)), 0};
    const int kGbufTol = 1;  // 容差理由 → judgments_table（float32 vs float64 + 平局取整模式）
    const int dAlbedo = maxAbsDiff(gotAlbedo, wantAlbedo);
    const int dMr = std::max(std::abs(gotMetalRough[0] - wantMetalRough[0]),
        std::abs(gotMetalRough[1] - wantMetalRough[1]));
    const Verdict vAlbedo = verdictFor("probe-gbuffer-albedo", dAlbedo <= kGbufTol,
        "存储字节 预测" + bytesText(wantAlbedo) + " vs 实际" + bytesText(gotAlbedo)
            + "（Δ" + std::to_string(dAlbedo) + " ≤ " + std::to_string(kGbufTol) + "）");
    const Verdict vMetalRough = verdictFor("probe-gbuffer-metalrough", dMr <= kGbufTol,
        "R(metal)/G(rough) 预测(" + std::to_string(wantMetalRough[0]) + ","
            + std::to_string(wantMetalRough[1]) + ") vs 实际(" + std::to_string(gotMetalRough[0])
            + "," + std::to_string(gotMetalRough[1]) + ")（Δ" + std::to_string(dMr) + "）");

    // ── 锚③：最终像素 = 完整 deferred 链的闭式。着色输入先过 8bit 量化往返
    //（albedo 走 rgba8_srgb、metal/rough 走 rgba8_unorm；法线 rg16 轴对齐无损不建模）。
    // 适用范围（承 Python 版模块头）：探针面无纹理/无 normal map、法线轴对齐，
    // 相机正视 ⟹ N=V=(0,0,1)。任何一项被改写过，先改这里的前提再谈着色链。
    const Vec3 qAlbedo{roundTrip8Srgb(albedo.x), roundTrip8Srgb(albedo.y),
        roundTrip8Srgb(albedo.z)};
    const double qRough = roundTrip8Unorm(roughness);
    const double qMetal = roundTrip8Unorm(metallic);
    const Vec3 linear = shadeDirect({0, 0, 1}, {0, 0, 1}, l, qAlbedo, qRough, qMetal,
        {intensity, intensity, intensity});
    const auto wantPixel = toBytes(linear);
    const int dPixel = maxAbsDiff(gotPixel, wantPixel);
    const Verdict vPixel = verdictFor("probe-pixel", dPixel <= tol,
        "(" + std::to_string(x) + "," + std::to_string(y) + ") 预测" + bytesText(wantPixel)
            + " vs 实际" + bytesText(gotPixel) + "（Δ" + std::to_string(dPixel) + " ≤ "
            + std::to_string(tol) + "）");

    std::printf("探针 (%d,%d)  配置=%s%s  路线=deferred 双锚\n", x, y, runDir.c_str(),
        lightOverride ? "（light 覆写副本）" : "");
    std::printf("  N=V=(0,0,1)  L=(%.6f,%.6f,%.6f)  albedo=(%.2f,%.2f,%.2f)"
        "  rough=%.2f metal=%.2f  I=%.1f\n",
        l.x, l.y, l.z, albedo.x, albedo.y, albedo.z, roughness, metallic, intensity);
    std::printf("  锚① gbuffer_albedo    预测%s 实际%s  Δ%d（阈值 %d）  %s\n",
        bytesText(wantAlbedo).c_str(), bytesText(gotAlbedo).c_str(), dAlbedo, kGbufTol,
        vAlbedo.ok ? "PASS" : "FAIL");
    std::printf("  锚② gbuffer_metalrough 预测(m=%d,r=%d) 实际(m=%d,r=%d)  Δ%d（阈值 %d）  %s\n",
        wantMetalRough[0], wantMetalRough[1], gotMetalRough[0], gotMetalRough[1], dMr, kGbufTol,
        vMetalRough.ok ? "PASS" : "FAIL");
    std::printf("  量化输入 albedo'=(%.6f,%.6f,%.6f) rough'=%.6f metal'=%.6f\n",
        qAlbedo.x, qAlbedo.y, qAlbedo.z, qRough, qMetal);
    std::printf("  线性预测 = (%.6f, %.6f, %.6f)\n", linear.x, linear.y, linear.z);
    std::printf("  锚③ 最终像素           预测%s 实际%s  Δ%d（阈值 %d）  %s\n",
        bytesText(wantPixel).c_str(), bytesText(gotPixel).c_str(), dPixel, tol,
        vPixel.ok ? "PASS" : "FAIL");

    if (!vAlbedo.ok || !vMetalRough.ok || !vPixel.ok) {
        std::printf("  归因顺序（先分段，再前提，最后才怀疑引擎）：\n"
                    "    ① 锚①/② 红 = G-buffer 编码契约变了（rgl/gbuffer_contract.cppm 格式表 /\n"
                    "       deferred_geometry.frag.slang 写侧 / dump_gbuf 回读转换）——与光照无关\n"
                    "    ② 锚①② 绿而锚③ 红 = light pass 或 tonemap 尾段（两路共用的 tonemapPass\n"
                    "       与逐字同源的 shadeLight 式；先对 light.frag 与本闭式）\n"
                    "    ③ 前提：探针面是否真的无纹理/无 normal map；N/V/L 是否真如假设；\n"
                    "       normalFiltering 是否抬了 roughness（平面正视图应为 0 方差）\n"
                    "    ④ 六个配置门 + wet.strength=0（deferred 下承重）是否都真的关上\n");
    }
    logCall("probe", {{"config", configDir}, {"run", runDir},
        {"xy", std::to_string(x) + "," + std::to_string(y)},
        {"light", std::to_string(l.x) + "," + std::to_string(l.y) + "," + std::to_string(l.z)},
        {"tol", std::to_string(tol)}},
        {vAlbedo, vMetalRough, vPixel});
    return vAlbedo.ok && vMetalRough.ok && vPixel.ok ? 0 : 1;
}

}  // namespace RGL::imgqa
