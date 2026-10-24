// blend-sort —— 透明合成次序的闭式判据（债-132 的接线：把刀3 那份测试输入接成常驻检查）。
//
// ── 这台仪器防的是哪一类错 ─────────────────────────────────────────────────────
// 透明混合没有深度排序，正确性**全靠绘制次序**。引擎的次序来自
// `rgl/render_list_impl.cppm` 的 `stable_sort(a.distanceSq > b.distanceSq)`（远先画），
// 而排序代理是 entity 世界矩阵的平移列（`tf.world[3]`），不是几何中心。把那个 `>` 写成
// `<`、或代理键退化成几何中心、或分桶时漏掉一层——四种错法在交付图上都是"有两层玻璃，
// 看着挺像"。本命令把"透明几何画得对不对"变成四个像素的字节对拍。
//
// ── 四条判据为什么这样分（红灯归因，逐条正文在 judgments_table_blend.cpp）──────────
// 三个单层区各锚一次「显示链 + 单个 α」，于是 overlap 区新增的**唯一未知量就是合成次序**：
//   `blend-sort-backdrop`     背景区 → 只钉显示链（ACES / sRGB 尾段）
//   `blend-sort-layer-far`    远层区 → 钉 α=0.25 的非预乘 over
//   `blend-sort-layer-near`   近层区 → 第二个 α（0.5），防"某个 α 恰好被某种取整蒙对"
//   `blend-sort-order`        重叠区 → 本任务的正主；前三条绿而它红 = 合成次序错
//
// ── 口径住在 `blend_sort_metric.{h,cpp}`，本文件只做仪器 ─────────────────────────
// 投影 / 合成 / 判别力 / 三条拒跑条件都是纯算式，`just imgqa selftest` 直接打它们
// （不启引擎、不读文件）。显示链一行都不在这里：`toBytes` / `roundTrip8Srgb` 是
// `analytic.cpp` 那份本项目唯一转录，再抄一份 = 给显示链开第二个真相源。
//
// ── 为什么不走 RPC（与 `shadow-kernel-scale` 同形）──────────────────────────────
// 冻结目录 `qa/configs/blend_sort/` 已经是自截形态（runtime.lua：headless + llm_debug +
// llm_shot + delay_frames=8）。`Engine` 构造就要 RPC，而 llm 截图在跑时 `/shot` 返 409
// （引擎既有约定）——两种采集形态互斥，工具迁就配置。帧号走 `llm_shot.delay_frames`
// 而不是 RPC：那是引擎在第 N 帧渲染完成后截图 = 钉死，同配置两跑逐位相同。
//
// ── 为什么没有 flag ───────────────────────────────────────────────────────────
// 阈值是量具的组成部分（`kBlendTolLsb`），可调 = 能调绿。缺输入直接 FAIL，**不 SKIP**。
#include "imgqa.h"
#include "blend_sort_metric.h"
#include "engine_lua_edit.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using Vec3d = std::array<double, 3>;

constexpr const char* kConfigDir = "qa/configs/blend_sort";
constexpr const char* kGlbStem = "blend_sort";
/// 副本目录（**不改仓库里的冻结目录**，同 `probe.cpp` 的 stageProbeProfile 那条线）。
constexpr const char* kProfileDir = "build/rgl/imgqa_blend_sort_profile";
/// 只改副本的这一枚键：换成专用文件名，免得和手工 `just shot --name blend_sort` 的产物互串。
constexpr const char* kShotFile = "screenshots/imgqa_blend_sort.png";

std::string readFileText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到文件: " + p.string());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void writeFileText(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

std::string bytesText(const std::array<int, 3>& b)
{
    return "(" + std::to_string(b[0]) + "," + std::to_string(b[1]) + ","
        + std::to_string(b[2]) + ")";
}

std::string vecText(const Vec3d& v)
{
    return "(" + std::to_string(v[0]) + ", " + std::to_string(v[1]) + ", "
        + std::to_string(v[2]) + ")";
}

[[nodiscard]] int maxAbsDiff(const std::array<int, 3>& got, const std::array<int, 3>& want)
{
    return std::max({std::abs(got[0] - want[0]), std::abs(got[1] - want[1]),
        std::abs(got[2] - want[2])});
}

// ── 采集形态：整目录复制 + 只改写 llm_shot.file ─────────────────────────────────

/// 把冻结目录整份复制到 build/rgl 下，只改副本的 `llm_shot.file`。
/// 改不到那枚键**必须抛**（照 `probe.cpp` 的 rewriteKey 那条纪律）：没改到也照跑 =
/// 引擎把图写到 `screenshots/blend_sort.png`，本命令随后对着一个不存在的文件报「没出图」，
/// 白跑一场引擎（几十秒）才出声，比抛错贵得多。
std::string stageBlendSortProfile()
{
    const fs::path src = fs::path(projectRoot()) / kConfigDir;
    const fs::path dst = fs::path(projectRoot()) / kProfileDir;
    if (!fs::is_directory(src))
        throw std::runtime_error(std::string("找不到冻结配置目录 ") + src.string()
            + "——本命令的输入就是这份冻结配置，没有它无从可测");
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst);
    for (const auto& e : fs::directory_iterator(src))
        fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing);

    const fs::path runtime = dst / "runtime.lua";
    std::string edited = readFileText(runtime);
    replaceValueInBlock(edited, "runtime.lua", "llm_shot", "file",
        std::string("\"") + kShotFile + "\"");
    writeFileText(runtime, edited);
    return dst.string();
}

// ── 测试输入：manifest 的 closedForm 块是测量接口的唯一真相源 ────────────────────

struct BlendTarget {
    std::vector<BlendPiece> pieces;
    BlendCamera camera;
    int width = 0;
    int height = 0;
};

/// 数组字段 → std::array<double, N>。形状不对立刻抛：manifest 是判据的**前提**，不是参考。
template <size_t N>
std::array<double, N> asArray(const json& v, const std::string& what)
{
    if (!v.is_array() || v.size() != N)
        throw std::runtime_error("manifest 的 " + what + " 不是 " + std::to_string(N)
            + " 元数值数组——测量接口形状变了，本命令拒绝猜");
    std::array<double, N> out{};
    for (size_t i = 0; i < N; ++i) {
        if (!v[i].is_number())
            throw std::runtime_error("manifest 的 " + what + " 第 " + std::to_string(i)
                + " 元不是数值——测量接口形状变了，本命令拒绝猜");
        out[i] = v[i].get<double>();
    }
    return out;
}

const json& requireField(const json& obj, const char* key, const std::string& what)
{
    if (!obj.contains(key))
        throw std::runtime_error(std::string("manifest 里没有 ") + what + key
            + "——测量接口形状变了，本命令拒绝猜");
    return obj.at(key);
}

/// 一条 piece → BlendPiece。半透层缺 `alpha` **必须抛**：α 是闭式的自变量，缺了它默认成
/// 1.0 会把「半透」静默读成「不透明」，而四条判据照样能自洽地算出一组数。
BlendPiece readPiece(const json& p)
{
    BlendPiece out;
    out.name = requireField(p, "name", "pieces[].").get<std::string>();
    out.color = asArray<3>(requireField(p, "color", "pieces[].color"), "pieces[].color");
    out.translation
        = asArray<3>(requireField(p, "translation", "pieces[]."), "pieces[].translation");
    const json& aabb = requireField(p, "aabb", "pieces[].");
    out.aabbMin = asArray<3>(requireField(aabb, "min", "aabb."), "pieces.aabb.min");
    out.aabbMax = asArray<3>(requireField(aabb, "max", "aabb."), "pieces.aabb.max");
    const std::string mode
        = p.contains("alphaMode") ? p.at("alphaMode").get<std::string>() : std::string();
    out.blend = mode == "BLEND";
    if (out.blend && (!p.contains("alpha") || !p.at("alpha").is_number()))
        throw std::runtime_error("半透层 " + out.name
            + "（alphaMode=BLEND）缺 alpha 字段——α 是闭式的自变量，缺了它默认成 1.0 "
              "= 把半透静默读成不透明，四条判据会算出一组自洽但全错的数。拒绝");
    if (out.blend)
        out.alpha = p.at("alpha").get<double>();
    return out;
}

/**
 * 读测试输入。**缺文件 = 抛**（不 SKIP）：静默跳过 = 假绿，判据会在「什么都没测」的状态下报绿。
 *
 * 形状守卫（统一口径 =「测量接口形状变了，本命令拒绝猜」）：`closedForm` 块缺失 /
 * `camera` 不是三元数值数组 / 半透层缺 `alpha` / `regions` 名单与本算子的四个区不一致。
 */
BlendTarget loadBlendTarget()
{
    const fs::path dir = fs::path(projectRoot()) / "build/rgl/models";
    const fs::path glb = dir / (std::string(kGlbStem) + ".glb");
    const fs::path manifestPath = dir / (std::string(kGlbStem) + ".manifest.json");
    const std::string gen
        = "python3 tools/qa_scenes.py --out build/rgl/models --only blend_sort";
    if (!fs::exists(glb))
        throw std::runtime_error("测试输入不存在: " + glb.string() + "\n  生成：" + gen
            + "（缺输入必须 FAIL，不许 SKIP——跳过等于判据在「什么都没测」的状态下报绿）");
    if (!fs::exists(manifestPath))
        throw std::runtime_error("测试输入的 manifest 不存在: " + manifestPath.string()
            + "\n  只有 .glb 没有 manifest ⟹ 闭式的事实来源（颜色 / α / node 平移 / 相机）"
              "没有地方可取，本命令拒绝在 C++ 里重抄一份（第二真相，改场景必漂）。\n  生成："
              + gen);
    const json m = json::parse(readFileText(manifestPath));

    BlendTarget t;
    const json& cf = requireField(m, "closedForm", "");
    if (!cf.is_object())
        throw std::runtime_error("manifest 的 closedForm 不是对象——它是场景生成器留给本命令的"
              "闭式接口；没有它就得在 C++ 里重抄颜色与 α（第二真相），拒绝");
    const json& cam = requireField(cf, "camera", "closedForm.");
    t.camera.position
        = asArray<3>(requireField(cam, "position", "closedForm.camera."), "closedForm.camera.position");
    t.camera.target
        = asArray<3>(requireField(cam, "target", "closedForm.camera."), "closedForm.camera.target");
    t.camera.fovDegrees = requireField(cam, "fovDegrees", "closedForm.camera.").get<double>();
    const auto aspect = asArray<2>(
        requireField(cf, "aspect", "closedForm."), "closedForm.aspect");
    t.width = static_cast<int>(aspect[0]);
    t.height = static_cast<int>(aspect[1]);
    // 区域名单对账：本算子算出的四个区必须与 manifest 声明的**同一份名单**（两份独立实现，
    // 不一致 = 场景生成器与本命令对「哪些像素算判据区」各有各的理解，此时任何读数都没意义）。
    const std::vector<std::string> want{
        "bg_only", "far_only", "near_only", "overlap"};
    std::vector<std::string> got;
    for (const auto& r : requireField(cf, "regions", "closedForm."))
        got.push_back(r.get<std::string>());
    std::sort(got.begin(), got.end());
    if (got != want)
        throw std::runtime_error("manifest 的 closedForm.regions 与本算子的四个判据区不一致（manifest "
              "给了 "
            + std::to_string(got.size())
            + " 项）——两边对「哪些像素算判据区」各有各的理解，读数没有意义。改一处必须改另一处");
    for (const auto& p : requireField(m, "pieces", ""))
        t.pieces.push_back(readPiece(p));
    return t;
}

// ── 相机对账：camera.lua 与 manifest 必须是同一组数 ─────────────────────────────
// （`{ a, b, c }` 的解析件在 `engine_lua_edit.cpp` —— `blend-lit` 也要拿它做同一条对账，
//  两处各写一遍 = 两个「什么形态算合法三元组」的判据。）

/**
 * camera.lua ↔ manifest 的相机对账，不一致即**抛**。
 *
 * 三条拒跑条件里最阴的一条：四个采样点的像素坐标是按 manifest 那组相机**解析**算的，而引擎
 * 实际用的是 camera.lua。两者不一致时采样点量到的是**别的几何**，闭式预测却照旧自洽——它算的
 * 本来就是 manifest 那组相机下的期望值。于是「配置改了、判据还在按旧机位预测」这件事唯一的
 * 表征是四条一起红，很容易被读成「渲染坏了」。拒跑比那个诚实。
 */
void requireCameraAgreement(const BlendCamera& declared)
{
    const fs::path lua = fs::path(projectRoot()) / kConfigDir / "camera.lua";
    const std::string text = readFileText(lua);
    const std::string where = lua.string();
    const auto position
        = parseLuaVec3(topLevelValueOf(text, "position", where), "camera.lua 的 position");
    const auto target
        = parseLuaVec3(topLevelValueOf(text, "target", where), "camera.lua 的 target");
    const double fov = std::stod(topLevelValueOf(text, "fovDegrees", where));
    const auto same = [](const Vec3d& a, const Vec3d& b) {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    };
    if (!same(position, declared.position) || !same(target, declared.target)
        || fov != declared.fovDegrees)
        throw std::runtime_error(
            "camera.lua 与 manifest 的 closedForm.camera 不一致 ⟹ 采样点会量到别的几何上，"
            "而闭式预测照旧自洽（它算的就是 manifest 那组相机下的期望）= 假绿。拒跑。\n  "
            "camera.lua: position="
            + vecText(position) + " target=" + vecText(target) + " fov=" + std::to_string(fov)
            + "\n  manifest : position=" + vecText(declared.position) + " target="
            + vecText(declared.target) + " fov=" + std::to_string(declared.fovDegrees)
            + "\n  两处必须一起改：qa/configs/blend_sort/camera.lua 与 tools/qa_scenes.py 的 "
              "BLEND_SORT_CAM");
}

// ── 取图 ───────────────────────────────────────────────────────────────────────

/**
 * 跑一次引擎并读回截图。
 *
 * 🔴 **跑之前先删掉目标 PNG**：`llm_debug` 截图是「引擎写、工具读」的文件交接，而
 * `runEngineOnce` 只看退出码。上一轮的旧图留在原地时，本轮引擎即使一帧都没截出来，读图这一步
 * 也照样成功——判据会拿着**上一轮的图**给这一轮发绿。删掉它，「没出图」才是一句真话。
 */
cv::Mat captureBlendSort(const std::string& profileDir)
{
    const std::string png = projectRoot() + "/build/rgl/" + kShotFile;
    std::error_code ec;
    fs::remove(png, ec);
    runEngineOnce(profileDir);
    if (!fs::exists(png))
        throw std::runtime_error("引擎跑完了但没有出图: " + png
            + "（runtime.lua 的 llm_debug / llm_shot 被改了吗？本命令跑之前已把该文件删掉，"
              "所以这里不可能是上一轮的残留）");
    return readPngBgr(png);
}

std::array<int, 3> pixelRgb(const cv::Mat& bgr, int x, int y)
{
    if (x < 0 || y < 0 || x >= bgr.cols || y >= bgr.rows)
        throw std::runtime_error("判据区采样点越界: (" + std::to_string(x) + ","
            + std::to_string(y) + ") vs 截图 " + std::to_string(bgr.cols) + "x"
            + std::to_string(bgr.rows));
    const cv::Vec3b px = bgr.at<cv::Vec3b>(y, x);
    return {px[2], px[1], px[0]};
}

/// 判别力自检（**先于对拍**跑）：备选错误模型与正确模型分不开 ⟹ 这次测量没有意义。
/// 抛，不判红——红了会让人去查一个根本不存在的渲染 bug。
void requireDiscriminativePower(const BlendTarget& t)
{
    struct Variant {
        const char* what;
        BlendOrder order;
        BlendModel model;
    };
    static constexpr Variant kVariants[] = {
        {"反序（把 stable_sort 的 > 改成 <）", BlendOrder::FrontToBack,
            BlendModel::NonPremultiplied},
        {"预乘混合（把 SRC_ALPHA 读成 ONE）", BlendOrder::BackToFront,
            BlendModel::Premultiplied},
    };
    for (const auto& v : kVariants) {
        const int power
            = blendMaxModelDiff(t.pieces, t.camera, t.width, t.height, v.order, v.model);
        if (power < kBlendPowerFloorLsb)
            throw std::runtime_error(std::string("判别力不足：") + v.what
                + " 与正确模型的最大字节差只有 " + std::to_string(power) + " LSB（下限 "
                + std::to_string(kBlendPowerFloorLsb)
                + "）——这份输入分不开这种错法，拿它当验收 = 一条恒真的判据。"
                  "改场景让两种模型在某个判据区上看得见地不同（改颜色或 α），别改这个下限");
    }
}

// ── 四条判据 ───────────────────────────────────────────────────────────────────

/// 一条判据 = 一个判据区。区域名 / 判据名 / 打印用的覆盖层说明三样绑在一起，
/// 顺序 = 归因顺序（显示链 → 单 α → 第二个 α → 合成次序），也是打印顺序。
struct RegionVerdict {
    const char* region;
    const char* verdict;
    const char* stack;
};

constexpr RegionVerdict kRows[] = {
    {"bg_only", "blend-sort-backdrop", "—"},
    {"far_only", "blend-sort-layer-far", "layer_far"},
    {"near_only", "blend-sort-layer-near", "layer_near"},
    {"overlap", "blend-sort-order", "layer_far → layer_near"},
};

const char* const kAttribution = "\n  归因顺序：backdrop 红 = 显示链尾段（tonemap / ACES / sRGB）变了；"
    "backdrop 绿而某条 layer 红 = 混合方程或 α 通路变了；前三条全绿而 order 红 = "
    "合成次序错（先查 render_list_impl.cppm 的 stable_sort 比较子与 distanceSq 的代理键）";

void pushRegionVerdict(std::vector<Verdict>& vs, const BlendTarget& t, const cv::Mat& bgr,
    const std::map<std::string, int>& regions, const RegionVerdict& row)
{
    const int x = regions.at(row.region);
    const auto want = toBytes(blendLinearAt(t.pieces, t.camera, x, t.height / 2, t.width,
                                  t.height, BlendOrder::BackToFront,
                                  BlendModel::NonPremultiplied),
        kBlendExposure);
    const auto got = pixelRgb(bgr, x, t.height / 2);
    const int d = maxAbsDiff(got, want);
    std::string observed = std::string(row.region) + " x=" + std::to_string(x) + " 闭式"
        + bytesText(want) + " 实测" + bytesText(got) + "（Δ" + std::to_string(d) + " ≤ "
        + std::to_string(kBlendTolLsb) + "）";
    if (d > kBlendTolLsb)
        observed += kAttribution;
    vs.push_back(verdictFor(row.verdict, d <= kBlendTolLsb, std::move(observed)));
    std::printf("  %-9s %6d  %-26s %-16s %-14s %s\n", row.region, x, row.stack,
        bytesText(want).c_str(), bytesText(got).c_str(), d <= kBlendTolLsb ? "PASS" : "FAIL");
}

}  // namespace

// ── blend-sort：透明合成次序的四区闭式对拍 ──────────────────────────────────────
int cmdBlendSort()
{
    std::printf("blend-sort —— 透明合成次序的闭式判据（输入 = qa/configs/blend_sort）\n");

    // ① 读测试输入（缺文件 = 抛，不 SKIP）+ 三条拒跑自检，全部**先于**跑引擎：这三条的含义是
    //    「这次测量没有意义」，白跑一次引擎（几十秒）才说这句话太贵。
    const BlendTarget t = loadBlendTarget();
    requireCameraAgreement(t.camera);
    requireSortableKeys(t.pieces, t.camera);
    const auto regions = blendRegionSamples(t.pieces, t.camera, t.width, t.height);
    requireDiscriminativePower(t);
    std::printf("  机位 position=%s fov=%.1f°  画幅 %dx%d  采样行 y=%d\n",
        vecText(t.camera.position).c_str(), t.camera.fovDegrees, t.width, t.height,
        t.height / 2);
    std::printf("  三条拒跑自检通过（相机对账 / 排序键互异 / 判别力 ≥ %d LSB）\n",
        kBlendPowerFloorLsb);

    // ② 跑引擎取图（副本目录，不动仓库里的冻结配置）。
    const std::string profile = stageBlendSortProfile();
    const cv::Mat bgr = captureBlendSort(profile);
    if (bgr.cols != t.width || bgr.rows != t.height)
        throw std::runtime_error("截图 " + std::to_string(bgr.cols) + "x"
            + std::to_string(bgr.rows) + " != manifest 设计尺寸 " + std::to_string(t.width) + "x"
            + std::to_string(t.height) + "——判据区的像素坐标是按设计尺寸解析算的，换画幅要同时改 "
              "runtime.lua 的 headless_size / llm_shot.size 与 qa_scenes.py 的 aspect");

    // ③ 四条判据。
    std::printf("\n  %-9s %6s  %-26s %-16s %-14s %s\n", "区", "x", "覆盖的半透层", "闭式字节",
        "实测字节", "判定");
    std::vector<Verdict> vs;
    for (const auto& row : kRows)
        pushRegionVerdict(vs, t, bgr, regions, row);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("blend-sort", {{"config", std::string(kConfigDir)}, {"run", profile},
        {"shot", kShotFile}, {"size", std::to_string(t.width) + "x" + std::to_string(t.height)}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    std::printf("\n%s\n", ok ? "全部判据绿" : "有判据红（正文的归因顺序先查哪一层）");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
