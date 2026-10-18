// shadow-kernel-scale —— CSM PCF 核宽的**单位**护栏（靶 = 程序化 penumbra_ladder）。
//
// ── 这台仪器防的是哪一个真实缺陷 ───────────────────────────────────────────────
// `rgl/shaders/shadow.slang` 的 CSM 分支（4 抽头 PCF）曾把**世界米/纹素**的 `texel`
// （`fillShadowData` 里 = 2·r/块边长）直接当**归一化图集 uv** 的偏移用：
//     const float2 o = float2(texel * 0.5, texel * 0.5);   // ← 单位错
// 正确值 = 半个图集纹素 = `0.5 / atlas.x`。误差倍数闭式 = **4r**（r = 该级联光空间半径，
// 取米数），于是误差**随级联半径线性放大**：bistro 交付档 r=0.57 m → 偏移 1.1 纹素（应有
// 0.5，肉眼看不出 ⟹ 潜伏了很久）；r 上到几十米 → 抽头跑到遮挡体图像之外、采到空图集
// （远深度）⟹ **整场景判成受光 = 阴影完全消失**。PCSS/EVSSM 分支因 blocker 搜索有
// `clamp(uv+off, lo, hi)` 夹回本 tile 而**免疫**——所以本命令的 pcss5 臂不是被测项，
// 它是判据 3 的阳性锚点（见下）。
//
// ── 为什么必须换靶（本护栏存在的全部理由）─────────────────────────────────────
// 项目里**全部**既有阴影判据都在 bistro 尺度上跑，而那个尺度下这个 bug 看不出来
// （1.1 vs 0.5 纹素 = 影缘糊一点点，且交付红线截图的 sha 恰好也抓不到它——偏移方向
// 对称、影缘仍在原处）。护栏要能红，就得有一个 **r 几十米**的靶：`penumbra_ladder`
// （`tools/qa_scenes.py` 场景 6）四块 16 m 薄板、间隙 h = 8/16/32/64 m、只在横向 v 上排开、
// 接收面是同一块 ±200 m 平地——设计约束只有一条：**只让 h 变**。
// 大 h ⟹ 级联半径大 ⟹ texel 大 ⟹ 单位错的偏移被放大到「抽头出图」= 缺陷当场现形。
//
// ── 几何常量一律读 manifest，不在 C++ 里重抄 ──────────────────────────────────
// `build/rgl/models/penumbra_ladder.manifest.json` 的 `measure` 块是场景生成器留给测量方
// 的接口（u 方向、shadowOffsetPerMeter、边长、扫描窗、步长、逐板 shadowCenter），机位在
// `cameras.ladder`。C++ 里再抄一份数字 = 造第二真相：生成器改了而 C++ 没改时，判据会在
// **错误的线**上求值并且照样绿。缺 .glb / .manifest.json = **抛**（静默跳过 = 假绿，
// AGENTS.md 测试纪律「设备级用例在无设备机器上必须 FAIL 不 SKIP」同理）。
//
// ── 测量口径（照抄已验证的 Python 参考实现，别"顺手优化"）─────────────────────
// 每块板：从 `shadowCenter` 起沿 u 以 manifest 的 step 米步长扫到 `edgeS ± scanHalfSpan`
// （edgeS = 下游缘 = 边长/2，闭式）。世界点 (x,0,z) → 像素 = 针孔透视（position/target/
// fovDegrees 来自 manifest 机位，up=+Y，aspect 取自截图），双线性取**字节域** luma
// （`byteLuma`，与 `shadow-softness` 同域）。阴影因子 `S = (L_none − L_arm) / L_none`
// ——env/ambient/ao 全 0 时它与 albedo 无关，所以四块不同颜色的板可以横向比较。
// 半影宽度 `W(h)` = 下游缘上 S 的 **10%↔90% 交叉间距**（线性插值）。
// 每个 s 处沿 v 取 **5 条线**（0/±1/±2 m）再平均：PCSS 的 S 剖面带像素尺度噪声，单线取样时
// 「第一个穿阈」会被单个噪声样本决定（实测同一批图换扫描步长，W(64) 在 4.86↔3.55 m 之间跳，
// 判据 3 随之从 2.06 掉到 1.50）。影缘沿 v 是直的 ⟹ 沿 v 平均**无偏**；5 线下三种步长给出
// 逐位相同的 8 个 W，且与 Python 参考（单线）八值全在 ±25% 内。理由全文 = kVLineOffsets 注释。
// ⚠ **世界步长采样 ⟹ W 直接以米计**，全程不做像素↔米换算（换算要把透视/像素尺寸再抄
//   一遍，那才是真误差源）。上游缘不测：h < 17.8 m 时它在板正下方、被板本身挡住，
//   这是几何事实（`qa_scenes.py` 场景 6 头注），不是配置没调好。
//
// ── 三条判据为什么这样分（阈值由判据设计者定死，不为绿而改）───────────────────
// 1 `shadow-csm-umbra-present`  CSM 必须有影（S>0.5 的像素占全幅 ≥0.50%）。
//   缺陷态实测 **0.00%**（相机距离 25/50/100/200/400 m、场景缩放 1×~0.03×、地面
//   ±200→±40 m 三种变量下全部恰 0）。
// 2 `shadow-csm-flat-in-gap`    CSM 的 W 对 h **必须平**（W(64)/W(16) ∈[0.55,1.80]）。
//   闭式预期 = 1：4 抽头 PCF 的核宽绑阴影贴图纹素，与 h 无关。现状 1.12。
// 3 `shadow-target-resolves-gap` **阳性锚点**：pcss5 的 W(64)/W(16) ≥1.60（现状 2.06）。
//   没有它，判据 2 的「平」毫无意义——靶子失去分辨力（机位/几何/阴影图分辨率漂了）时
//   四条曲线会一起塌成水平，判据 2 反而**更绿**。它必须能红，判据 2 的绿才是陈述。
//   为什么锚点用 PCSS 而不是「CSM 自己再加一臂」：与 `shadow-softness` J1 同一条理由——
//   拿被测对象当锚点是循环论证；PCSS 的半影由 Δz·2tan(θ/2) 现算，是独立实现。
//
// ── 为什么没有 flag ───────────────────────────────────────────────────────────
// 场景、臂、帧号、阈值**都是量具的组成部分**：能调 = 能调绿（同 `shadow-softness`）。
// 帧号走 `llm_shot.delay_frames` 而不是 RPC `/shot`：RPC 落在哪帧取决于 HTTP 往返与帧首
// 钩子的先后 = 墙钟，而 delay_frames 是引擎在第 N 帧渲染完成后截图 = 钉死。
//
// ── 跑起来该看到什么 ─────────────────────────────────────────────────────────
// 三臂各一次引擎进程（串行，单实例纪律）；打印逐板 W（米）+ 下游缘 50% 位置（阳性锚点，
// 闭式 = 边长/2）+ 影内最暗 S，再打三条判决。现状（2026-10-10 本机）：CSM W =
// 0.15/0.20/0.21/0.22 m、pcss5 W = 0.15/2.36/3.68/4.86 m（h = 8/16/32/64）⟹ 三条全绿。

#include "imgqa.h"
#include "shadow_kernel_metric.h"  // 口径层共用的常数（kNaN 等）

#include <opencv2/core.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <numbers>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

// ── 写死的量具常数（改动 = 改判据）─────────────────────────────────────────────
// kNaN / kLumaUndefined / kUmbraFactor / kCrossLo / kCrossHi 在
// `shadow_kernel_metric.h`（口径层与仪器层共用，唯一出处）。
constexpr int kWidth = 1280;              ///< 采集宽（与 manifest 机位的 aspect 配对）
constexpr int kHeight = 720;              ///< 采集高
constexpr int kDelayFrames = 8;           ///< 截图帧号（llm_shot.delay_frames）
constexpr double kUmbraFloorPct = 0.50;   ///< 判据 1 下限（占**全幅**百分比）
constexpr double kFlatLo = 0.55;          ///< 判据 2 下界
constexpr double kFlatHi = 1.80;          ///< 判据 2 上界
constexpr double kResolveFloor = 1.60;    ///< 判据 3 下限（靶的分辨力）
/// 判据比值用的两档间隙（米）。**按值**在 manifest 里查档：间隙梯若改了，本命令报错
/// 而不是悄悄换一档去比——那等于把判据的自变量也一起调绿。
constexpr double kGapNear = 16.0;
constexpr double kGapFar = 64.0;
constexpr double kGapEps = 1e-9;  ///< 间隙档匹配容差（manifest 里是整米，精确相等即可）
constexpr const char* kGlbStem = "penumbra_ladder";

/// 一臂 = shadow.lua 的整份内容 + 引擎回显里**必须**出现的证据串。
struct Arm {
    const char* name;
    const char* mode;       ///< 引擎回显 `shadow: mode=…` 应当是它
    const char* echoExtra;  ///< 额外必须出现在回显里的串（nullptr = 无此项要求）
    const char* shadowLua;
};

/// 三臂。`pcss5` 的三枚调参与 `docs/shadow_routes_plan.md` 的 PCSS 能力档同值；
/// sun_degrees=5 是把物理半影顶出量化地板的那一档（0.53° 出厂档下 W 全塌成核宽）。
const std::vector<Arm>& arms()
{
    static const std::vector<Arm> kArms = {
        {"none", "none", nullptr, "return { mode = \"none\", izb = { on = false } }\n"},
        {"csm", "csm", nullptr, "return { mode = \"csm\", izb = { on = false } }\n"},
        {"pcss5", "pcss", "pcss.sun_degrees=5",
            "return { mode = \"pcss\", izb = { on = false }, pcss = { sun_degrees = 5.0, "
            "search_radius = 16.0, max_penumbra = 32.0 } }\n"},
    };
    return kArms;
}

std::string pct(double frac)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f%%", 100.0 * frac);
    return buf;
}

/// 数出串（2 位小数）：NaN 打成 `—`——"取不到交叉"与"宽度是 0"是两件事，
/// 混成一个数会被读成"影缘极硬"，而那恰恰是缺陷态想伪装成的样子。
std::string fixed2(double v)
{
    if (!std::isfinite(v))
        return "—";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

// ── 靶：manifest.measure 是测量接口的唯一真相源 ────────────────────────────────

/// 一块板 = 一个间隙档。
struct Plate {
    std::string name;
    double h = 0.0;
    std::array<double, 2> shadowCenter{};  ///< 影心 = 板心 + h·shadowOffsetPerMeter
    double edgeS = 0.0;                    ///< 下游缘在沿 u 距影心多少米处（闭式 = 边长/2）
};

/// manifest.measure 整块（+ 只用于打印对照的闭式斜率）。
struct Measure {
    std::array<double, 2> u{};  ///< 光的水平行进方向 = 扫描方向
    std::array<double, 2> v{};  ///< 影缘方向（与 u 正交）：沿它铺取样线去噪
    double edgeS = 0.0;
    double scanHalfSpan = 0.0;
    double step = 0.0;
    std::vector<Plate> plates;
    double penumbraPerMeterAt5 = 0.0;  ///< 闭式 dW/dh（α=5°），只用于打印对照列
};

struct LadderTarget {
    Measure measure;
    Engine::Camera camera;
};

std::string readFileText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到文件: " + p.string());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

/// 数组字段 → std::array<double, N>（形状不对立刻抛：manifest 是判据的前提，不是参考）。
template <size_t N>
std::array<double, N> asArray(const json& v, const std::string& what)
{
    if (!v.is_array() || v.size() != N)
        throw std::runtime_error("manifest 的 " + what + " 不是 " + std::to_string(N)
            + " 元数值数组——测量接口形状变了，本命令拒绝猜");
    std::array<double, N> out{};
    for (size_t i = 0; i < N; ++i)
        out[i] = v[i].get<double>();
    return out;
}

/**
 * 读靶。**缺文件 = 抛**（不 SKIP）：静默跳过 = 假绿，判据会在"什么都没测"的状态下报绿。
 * 机位取 `cameras.ladder`，测量接口取 `measure`（生成器留给本命令的接口块）。
 */
LadderTarget loadLadderTarget()
{
    const fs::path dir = fs::path(projectRoot()) / "build/rgl/models";
    const fs::path glb = dir / (std::string(kGlbStem) + ".glb");
    const fs::path manifestPath = dir / (std::string(kGlbStem) + ".manifest.json");
    if (!fs::exists(glb))
        throw std::runtime_error("靶不存在: " + glb.string()
            + "\n  生成：python3 tools/qa_scenes.py --only penumbra_ladder --out "
              "build/rgl/models（缺靶必须 FAIL，不许 SKIP）");
    if (!fs::exists(manifestPath))
        throw std::runtime_error("靶的 manifest 不存在: " + manifestPath.string()
            + "\n  只有 .glb 没有 manifest ⟹ 测量接口（measure 块）没有来源，拒绝猜几何");
    const json m = json::parse(readFileText(manifestPath));
    if (!m.contains("measure") || !m.at("measure").is_object())
        throw std::runtime_error("manifest 里没有 measure 块——它是场景生成器留给本命令的"
              "测量接口；没有它就得在 C++ 里重抄几何常量（第二真相），拒绝");

    Measure out;
    const json& ms = m.at("measure");
    out.u = asArray<2>(ms.at("u"), "measure.u");
    out.v = asArray<2>(ms.at("v"), "measure.v");
    out.edgeS = ms.at("edgeS").get<double>();
    out.scanHalfSpan = ms.at("scanHalfSpan").get<double>();
    out.step = ms.at("step").get<double>();
    if (out.step <= 0.0 || out.scanHalfSpan <= 0.0)
        throw std::runtime_error("manifest 的 measure.step / measure.scanHalfSpan 必须为正");
    for (const auto& p : ms.at("plates")) {
        Plate pl;
        pl.name = p.at("name").get<std::string>();
        pl.h = p.at("h").get<double>();
        pl.shadowCenter = asArray<2>(p.at("shadowCenter"), "measure.plates[].shadowCenter");
        pl.edgeS = p.at("edgeS").get<double>();
        out.plates.push_back(std::move(pl));
    }
    if (out.plates.size() < 2)
        throw std::runtime_error("manifest 的 measure.plates 少于两块板——间隙梯塌成一点，"
              "任何 W(h) 斜率判据都无从谈起");
    out.penumbraPerMeterAt5 = m.at("penumbraPerMeterOfGap").at("at_5").get<double>();

    const json& cam = m.at("cameras").at("ladder");
    LadderTarget t;
    t.camera.position = asArray<3>(cam.at("position"), "cameras.ladder.position");
    t.camera.target = asArray<3>(cam.at("target"), "cameras.ladder.target");
    t.camera.fovDegrees = cam.value("fovDegrees", 60.0);
    t.measure = std::move(out);
    return t;
}

// ── 剖面取样（口径层，实现在 `shadow_kernel_metric.cpp`）───────────────────────
// 投影复刻 / 双线性字节域取样 / 沿 v 五线平均 / 10↔90 交叉三算子都在口径层——它们被
// `just imgqa selftest` 用闭式直接测，本文件只把 manifest 的测量接口喂给它。

ScanLine scanLineFor(const Plate& plate, const Measure& m)
{
    ScanLine line;
    line.origin = plate.shadowCenter;
    line.u = m.u;
    line.v = m.v;  // 影缘方向：沿它铺 5 条取样线去噪（理由见 kVLineOffsets 注释）
    line.lo = plate.edgeS - m.scanHalfSpan;
    line.hi = plate.edgeS + m.scanHalfSpan;
    line.step = m.step;
    return line;
}

}  // namespace

// 三个公开算子（shadowCrossing / shadowPenumbraWidth / shadowUmbraFraction）的实现
// 在 `shadow_kernel_metric.cpp`——口径层与仪器层的缝见 `shadow_kernel_metric.h` 头注释。

namespace {

// ── 采集 ─────────────────────────────────────────────────────────────────────

/**
 * 引擎启动日志里的阴影门回显（`rgl/shadow_config_impl.cppm` logShadowGateEcho）：
 * `shadow: mode=pcss pcss.sun_degrees=5`。整份覆写 shadow.lua 之后，这是**唯一**能证明
 * 「引擎真按这一档跑了」的外部证据——mode 打偏或调参键拼错在 lua 侧都是**静默**的
 * （未知键只 warn，值错没人管），届时读数会被当成 pcss5 读。宁可抛。
 */
void requireShadowEcho(const Arm& a)
{
    const fs::path log = fs::path(projectRoot()) / "build/rgl/imgqa_run.log";
    std::ifstream in(log, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到引擎日志: " + log.string());
    const std::string txt{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
    const std::string want = "shadow: mode=" + std::string(a.mode);
    if (txt.find(want) == std::string::npos)
        throw std::runtime_error("臂 " + std::string(a.name) + " 的引擎回显里没有 `" + want
            + "` ⟹ shadow.lua 覆写没生效（静默换臂），全部读数作废，见 " + log.string());
    if (a.echoExtra != nullptr && txt.find(a.echoExtra) == std::string::npos)
        throw std::runtime_error("臂 " + std::string(a.name) + " 的引擎回显里没有 `"
            + a.echoExtra + "` ⟹ pcss 调参键没被解析器读到（拼错键 = 静默拿出厂值 0.53°），"
              "阳性锚点臂名不副实，见 "
            + log.string());
}

/// 采一臂：合成 profile（基底 = qa/configs/analytic 的图 + deferred）→ 引擎跑到自然退出
/// → 读图。每臂重建目录：臂与臂之间不共享可变配置（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const Arm& a, const LadderTarget& t, const std::string& profileDir)
{
    std::error_code ec;
    fs::remove_all(profileDir, ec);
    ProfileOpts o;
    // ⚠ 带扩展名：`ProfileOpts::glb` 是**整文件名**（写进 scene.lua 的
    // `entities[].file = "models/<glb>"`，引擎不补后缀）。给 stem 会得到
    // `GltfLoadFailed: Failed to read file: models/penumbra_ladder`（实测）。
    o.glb = std::string(kGlbStem) + ".glb";
    o.picking = false;       // 本判据的区域来自几何（世界米扫描线），不消费 object_id
    o.shadowLuaOverride = a.shadowLua;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效（不是 RPC 补写）
    o.cameraPosition = t.camera.position;
    o.cameraTarget = t.camera.target;
    o.cameraFov = t.camera.fovDegrees;
    o.overrideLlmShot = true;
    o.llmShotFile = "screenshots/imgqa_shadow_kernel_scale_" + std::string(a.name) + ".png";
    o.llmShotDelayFrames = kDelayFrames;
    makeProfile(profileDir, o);
    runEngineOnce(profileDir);
    requireShadowEcho(a);
    const std::string png = projectRoot() + "/build/rgl/" + o.llmShotFile;
    if (!fs::exists(png))
        throw std::runtime_error("臂 " + std::string(a.name) + " 没出图: " + png);
    return readPngBgr(png);
}

std::map<std::string, cv::Mat> captureArms(const LadderTarget& t, const std::string& profile)
{
    std::map<std::string, cv::Mat> luma;
    for (const auto& a : arms()) {
        const cv::Mat bgr = captureArm(a, t, profile);
        luma[a.name] = byteLuma(bgr);
        std::printf("  ✓ 臂 %-6s mode=%-6s %dx%d\n", a.name, a.mode, bgr.cols, bgr.rows);
    }
    return luma;
}

// ── 读数 ─────────────────────────────────────────────────────────────────────

/// 一块板在一条臂上的读数（全部以**世界米**计）。
struct PlateReading {
    double width = kNaN;   ///< 半影宽度 = S 的 10↔90 交叉间距
    double edge50 = kNaN;  ///< 下游缘 50% 交叉位置（闭式 = edgeS）——投影复刻的阳性锚点
    double darkest = kNaN; ///< 影内最暗 S（应 ≈ 1，否则影没全黑）
};

/// 影内的 S（|s| < edgeS − 2：本影中段，避开两侧半影）。
std::vector<double> innerShadowFactors(const std::vector<ShadowScanSample>& pr, double edgeS)
{
    std::vector<double> out;
    for (const auto& p : pr)
        if (p.valid && std::abs(p.s) < edgeS - 2.0)
            out.push_back(p.value);
    return out;
}

PlateReading measurePlate(const cv::Mat& refLuma, const cv::Mat& armLuma,
    const Projector& proj, const Plate& plate, const Measure& m)
{
    const ScanLine line = scanLineFor(plate, m);
    const std::vector<ShadowScanSample> pr = scanShadowProfile(refLuma, armLuma, proj, line);
    PlateReading out;
    out.width = shadowPenumbraWidth(pr, line.lo, line.hi);
    out.edge50 = shadowCrossing(pr, 0.5, line.lo, line.hi);
    const std::vector<double> inner = innerShadowFactors(pr, plate.edgeS);
    if (!inner.empty())
        out.darkest = *std::ranges::min_element(inner);
    return out;
}

/// 一条臂在全部板上的读数（顺序 = manifest 的 plates 顺序）。
std::vector<PlateReading> measureArmReadings(const std::map<std::string, cv::Mat>& luma,
    const std::string& arm, const LadderTarget& t, const Projector& proj)
{
    std::vector<PlateReading> out;
    for (const auto& plate : t.measure.plates)
        out.push_back(measurePlate(luma.at("none"), luma.at(arm), proj, plate, t.measure));
    return out;
}

const PlateReading& readingAtH(const std::map<std::string, std::vector<PlateReading>>& all,
    const std::string& arm, const Measure& m, double h)
{
    for (size_t i = 0; i < m.plates.size(); ++i)
        if (std::abs(m.plates[i].h - h) < kGapEps)
            return all.at(arm)[i];
    throw std::runtime_error("靶的间隙梯里没有 h = " + std::to_string(h)
        + " m 这一档——判据的自变量在靶上不存在，拒绝换一档去比（那等于把判据调绿）");
}

double widthRatio(const std::map<std::string, std::vector<PlateReading>>& all,
    const std::string& arm, const Measure& m)
{
    const double a = readingAtH(all, arm, m, kGapFar).width;
    const double b = readingAtH(all, arm, m, kGapNear).width;
    if (!std::isfinite(a) || !std::isfinite(b) || b == 0.0)
        return kNaN;
    return a / b;
}

// ── 打印 ─────────────────────────────────────────────────────────────────────

/// 逐板读数表：W（米）+ 下游缘 50% 位置（阳性锚点）+ 影内最暗 S。
/// 表头用 ASCII：中文列头 printf 按**字节**补位、终端按**显示宽度**排 ⟹ 必然错位。
void printReadoutTable(const LadderTarget& t,
    const std::map<std::string, std::vector<PlateReading>>& all)
{
    std::printf("\n半影宽度 W（米，S 的 10–90%% 交叉间距；世界步长采样，不做像素↔米换算）\n");
    std::printf("  图例：W=半影宽度  edge50=下游缘 50%% 交叉位置（闭式 %.1f）  dark=影内最暗 S\n",
        t.measure.edgeS);
    std::printf("  edge50 是**诊断列**不是判据：① 抗 acne 的法线偏移（1.5 texel 沿 +Y）把整条"
                "影缘往上游挪 ~1.5·texel/tan(仰角)，② h 小的档其影的上游段被板本身挡住（板轮廓"
                "先过 50%%）——两条都是渲染/几何事实。它只用来在「W 全 NaN」时区分"
                "「投影没复刻对（扫描线根本不在那条缘上）」与「影真没画出来」。\n");
    std::printf("  %6s %9s %8s %7s   %9s %8s %7s   %9s\n", "h(m)", "csm W", "edge50", "dark",
        "pcss5 W", "edge50", "dark", "闭式@5°");
    for (size_t i = 0; i < t.measure.plates.size(); ++i) {
        const Plate& p = t.measure.plates[i];
        const PlateReading& c = all.at("csm")[i];
        const PlateReading& s = all.at("pcss5")[i];
        std::printf("  %6.0f %9s %8s %7s   %9s %8s %7s   %9.2f\n", p.h, fixed2(c.width).c_str(),
            fixed2(c.edge50).c_str(), fixed2(c.darkest).c_str(), fixed2(s.width).c_str(),
            fixed2(s.edge50).c_str(), fixed2(s.darkest).c_str(),
            t.measure.penumbraPerMeterAt5 * p.h);
    }
    std::printf("  （对照：已验证的 Python 参考实现 CSM = 0.15/0.20/0.21/0.22、"
                "pcss5 = 0.15/2.36/3.68/4.86 m）\n");
    std::printf("   两实现**不是同一取样**：参考 = 单线 @0.04 m，本命令 = 沿 v 五线 @manifest "
                "步长。八值全在 ±25%% 内；五线是**无偏**去噪（影缘平行于 v），"
                "它换来的是「换扫描步长读数逐位不变」——单线时 W(64) 会随步长在 4.86↔3.55 "
                "之间跳。\n");
}

// ── 三条判决线 ───────────────────────────────────────────────────────────────

std::string gapPair()
{
    return "W(" + std::to_string(static_cast<int>(kGapFar)) + ")/W("
        + std::to_string(static_cast<int>(kGapNear)) + ")";
}

void pushUmbraVerdict(std::vector<Verdict>& vs, double umbraFrac)
{
    const bool ok = umbraFrac * 100.0 >= kUmbraFloorPct;
    vs.push_back(verdictFor("shadow-csm-umbra-present", ok,
        "CSM 臂相对 none 的阴影因子 S > " + std::to_string(kUmbraFactor) + " 的像素占全幅 = "
            + pct(umbraFrac) + "（下限 " + std::to_string(kUmbraFloorPct) + "%）"
            + (ok ? ""
                  : "\n  这条红 = CSM 的 4 抽头 PCF **把整个场景判成受光**：抽头偏移若把世界米"
                    "当归一化 uv 用，偏移 = 应有值的 4r 倍（r = 级联光空间半径），r 上到几十米时"
                    "抽头跑到遮挡体图像之外、采到空图集（远深度）⟹ 深度比较恒「受光」。"
                    "先查 shadow.slang ⑥ 的 `halfTexelUv` 单位")));
}

void pushFlatVerdict(std::vector<Verdict>& vs, double ratio)
{
    const bool ok = std::isfinite(ratio) && ratio >= kFlatLo && ratio <= kFlatHi;
    vs.push_back(verdictFor("shadow-csm-flat-in-gap", ok,
        !std::isfinite(ratio)
            ? gapPair() + " 取不出来（CSM 臂在 h=" + std::to_string(static_cast<int>(kGapNear))
                  + " 或 " + std::to_string(static_cast<int>(kGapFar))
                  + " m 档没有 10/90 交叉）⟹ 影根本没画出来，「平」无从谈起"
            : gapPair() + " = " + fixed2(ratio) + "（判据 ∈[" + std::to_string(kFlatLo) + ", "
                  + std::to_string(kFlatHi) + "]，闭式预期 1）"
                  + (ok ? ""
                        : "\n  这条红 = CSM 的核宽开始**跟着间隙走**：4 抽头 PCF 的核宽绑的是"
                          "阴影贴图纹素、与 h 无关；比值出界说明核宽里混进了与 h 相关的量"
                          "（texelWorld 被误用进 uv 偏移就是这一类——偏移随级联半径放大 ⟹ 高档"
                          "抽头出图、宽度读数塌掉）")));
}

void pushResolveVerdict(std::vector<Verdict>& vs, double ratio)
{
    const bool ok = std::isfinite(ratio) && ratio >= kResolveFloor;
    vs.push_back(verdictFor("shadow-target-resolves-gap", ok,
        !std::isfinite(ratio)
            ? "pcss5 臂的 " + gapPair()
                  + " 取不出来 ⟹ **量具自己瞎了**，不是 CSM 的问题（PCSS 分支对那个单位错免疫）"
            : "pcss5 臂的 " + gapPair() + " = " + fixed2(ratio) + "（下限 "
                  + std::to_string(kResolveFloor)
                  + "）——本条红时**判据 2 的绿一律不作数**"
                    + (ok ? ""
                          : "\n  这条红 = 靶失去分辨力（机位/几何/阴影图分辨率漂了）：PCSS 的"
                            "半影按闭式线性于 h，四档一起塌平 ⟹ 判据 2 的「平」就不再是陈述，"
                            "只是量具瞎了的样子。先查 pcss 调参有没有生效（引擎回显 "
                            "pcss.sun_degrees=5）、再查靶的间隙梯与机位视线净空（qa_scenes.py "
                            "场景 6 的构建期 assert）")));
}

}  // namespace

int cmdShadowKernelScale()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_shadow_kernel_scale_profile";
    std::printf("shadow-kernel-scale —— CSM PCF 核宽**单位**护栏（靶 = penumbra_ladder）\n");
    std::printf("  缺陷本体 = shadow.slang ⑥ 把世界米/纹素的 texel 当图集 uv 偏移用，误差 = 4r\n");
    std::printf("  帧号 = 第 %d 帧（llm_shot.delay_frames 写死）；采集 %dx%d\n", kDelayFrames,
        kWidth, kHeight);

    const LadderTarget t = loadLadderTarget();  // 缺靶 = 抛（不 SKIP）
    std::printf("  靶 = %s（%zu 块板，步长 %.2f m，扫描窗 [%.1f, %.1f] 沿 u）\n", kGlbStem,
        t.measure.plates.size(), t.measure.step, t.measure.edgeS - t.measure.scanHalfSpan,
        t.measure.edgeS + t.measure.scanHalfSpan);

    const std::map<std::string, cv::Mat> luma = captureArms(t, profile);
    // 画幅跟着**实际截图**走（不在口径层写死 1280×720）：采集尺寸是臂定义，
    // 投影若另抄一份，改尺寸时解出来的像素就与图不是同一张了。
    const Projector proj
        = makeProjector(t.camera, luma.at("none").cols, luma.at("none").rows);

    std::map<std::string, std::vector<PlateReading>> all;
    for (const auto& a : arms())
        if (std::string(a.name) != "none")
            all[a.name] = measureArmReadings(luma, a.name, t, proj);

    const double umbra = shadowUmbraFraction(luma.at("none"), luma.at("csm"));
    printReadoutTable(t, all);
    std::printf("\n影覆盖率（S > %.1f 占全幅）= %s（判据 1 下限 %.2f%%）\n", kUmbraFactor,
        pct(umbra).c_str(), kUmbraFloorPct);

    std::vector<Verdict> vs;
    pushUmbraVerdict(vs, umbra);
    pushFlatVerdict(vs, widthRatio(all, "csm", t.measure));
    pushResolveVerdict(vs, widthRatio(all, "pcss5", t.measure));

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("shadow-kernel-scale",
        {{"glb", std::string(kGlbStem)}, {"delay_frames", std::to_string(kDelayFrames)},
            {"arms", "none,csm,pcss5"}},
        vs);
    bool ok = true;
    for (const auto& v : vs)
        ok = ok && v.ok;
    std::printf("\n%s\n", ok ? "全部判据绿" : "有判据红（正文点名先查哪一层）");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
