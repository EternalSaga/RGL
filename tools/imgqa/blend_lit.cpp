// blend_lit.cpp —— 透明段 lit 变体的**常驻像素贡献**判据（债-134 的先决，六条判据）。
//
// ## 这台仪器防的是哪一类错
//
// 透明混合管线（ShaderObject + 独立 transparent pass）已经**接上了**：`renderListSystem`
// 分桶、`transparentStart` 赋值、执行器按段录 draw、三支 lit 类别变体的 SPV 都在。但
// `just gates` 里**没有任何一条检查会让它红**——透明段整段删掉，交付图也不会变，因为
// 交付场景 configs/scene.lua 里根本没有 BLEND 图元（实测：`transparentStart == size()`）。
// 账 → docs/还债总账.md 债-134。本命令把「透明段对交付图真的有贡献」变成一条能重放的检查。
//
// ## 双臂 = 同一份场景生成两次，只差 alphaMode（整台仪器的立命处）
//
// 判据要证「透明段画进了交付图」，唯一诚实的形态是**改一个变量看结果变不变**：
//   BLEND 臂  `build/rgl/models/blend_lit.glb`        三支 pane alphaMode=BLEND
//   OPAQUE 臂 `build/rgl/models/blend_lit_opaque.glb` 同三支 pane alphaMode 缺省(OPAQUE)
// 两臂由 `tools/qa_scenes.py` 的**同一个函数** `scene_blend_lit(blend=)` 生成，只差 α；
// 本命令跑之前先做**同源对账**（两臂 manifest 的相机/落位/尺寸/颜色逐字段全等），对不上
// 就拒跑——否则「双臂不同」可以来自任何一处别的差异，判据当场变成噪声计。
//
// ## 六条判据为什么这样分（红灯归因顺序）
//   `blend-lit-draws`      引擎回显透明段条数 == manifest 声明值（**不数像素**）
//   `blend-lit-reverse`    反向用例：OPAQUE 臂的条数必须**恰为 0**（堵「条数恒非 0」）
//   `blend-lit-contributes` footprint 内双臂确有不同（透明段往像素上写了东西）
//   `blend-lit-confined`   footprint 之外双臂**逐位相同**（写的位置就是它该写的位置）
//   `blend-lit-visible`    BLEND 臂自己的 footprint 众数 ≠ 自己的背景板众数（画得看得见）
//   `blend-lit-clean`      两次启动的 `[error]` 计数 = 0（VVL 干净）
// 前两条是「引擎侧的账」，三四是「像素侧的账」，五是「同臂的账」，六是「过程干净」。
// 缺任何一条都留一条旁路：只有 1 → 条数对但没画；只有 3 → 「图变了」可以是任何原因
// （阴影旁路就是这么冒出来的，见 qa/configs/blend_lit/shadow.lua）；没有 4 → 3 可以在
// 别的像素上成立；**没有 5 → 把透明段整段删掉时 3 照旧绿**（变异实测，见该条正文）。
//
// ## 为什么必须走 RPC 读条数（而不是数像素反推）
//
// 「透明段有几条 draw」是**引擎侧的事实**。从像素反推它 = 用被测量证明被测量：像素变了
// 只能说明"有别的东西变了"。读出口 = `GET /state` → `render.transparentDraws`，其口径
// 与执行器同源（引擎侧吃 `segmentSpan("transparent", …)`，见 debug_rpc_exec_impl.cppm）。
//
// 时序是**实测**出来的，不是推的：RPC 服务在进程起后约 2.1 s 绑上端口，请求只在帧边界被
// drain，而 poll 挂在 system 链**之前** ⟹ 读数落在第 2…delay_frames 帧，且第 1 帧读到的是
// 装配期默认空表（恒 0，**不是**「场景没有透明图元」）。故消费侧要求 frame ≥ 2
// （`kBlendLitMinFrame`）。delay_frames 因此从 blend_sort 的 8 提到 2000：窗口宽度
// ≈ delay_frames / 帧率，8 帧只给约 20 ms（本机够但没余量），2000 帧给约 0.8 s。
//
// ## 实测读数（本机 AMD Radeon 8060S Graphics / RADV STRIX_HALO，2026-10-24）
//
//   BLEND  臂：transparentDraws 值集 {3}，337 次读数覆盖帧 7…1996，exit=0
//   OPAQUE 臂：transparentDraws 值集 {0}，335 次读数覆盖帧 2…1999，exit=0
//   双臂逐像素：footprint(+1px) 内 changed = 185008 / 921600（20.07%），max = 165 LSB
//              footprint 之外 changed = **0**（背景板逐位相同）
//   同臂两跑：PNG 的 SHA256 逐位相同（spin=false + aa=off ⟹ 无时域历史，第 2000 帧可重放）
//
// ## 不覆盖什么（honesty，与 qa_scenes.py 的 scene_blend_lit 的 honesty 字段同步）
//
// **不预测任何像素值**（弱判据；透明光照的闭式要复刻 BRDF + IBL + CSM，台账裁「成本 > 收益」）
// ⟹ 它证「透明段对交付图有贡献」，**不证「透明画对了」**。逐条不覆盖：① 阴影遮挡
// （shadow mode=none 是刻意的，见该文件头注）；② MASK discard；③ velocity/TAA（透明段不写
// 深度）；④ picking / object_id；⑤ GPU culling 候选；⑥ 多层叠加与排序（那是 blend-sort）；
// ⑦ 自发光电平（pane 的 emissive 恒 None）；⑧ 法线贴图 / ORM / 湿面。
//
// ## 口径住在 `blend_lit_metric.{h,cpp}`，本文件只做仪器
//
// 投影 / 区域分桶 / 背景板铺满对账都是纯算式，`just imgqa selftest` 直接打它们（不启引擎）。
//
// ## 为什么没有 flag
//
// 阈值是量具的组成部分，可调 = 能调绿。缺输入 / 缺设备直接 FAIL，**不 SKIP**——静默跳过
// = 判据在「什么都没测」的状态下报绿。
#include "imgqa.h"
#include "blend_lit_metric.h"
#include "engine_lua_edit.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

constexpr const char* kConfigDir = "qa/configs/blend_lit";
constexpr const char* kProfileDir = "build/rgl/imgqa_blend_lit_profile";
/// 冻结目录里 rpc=true 用的端口（diagnostics.lua 的 rpc_port；缺省也是这个值，
/// 写出来是为了「读出口在哪个端口」是冻结的事实而不是回落的默认）。
constexpr int kRpcPort = 8788;
/// 轮询预算。引擎 exit 前 RPC 一直可连，读到进程消失为止；60s 与 `Engine` 的 RPC 就绪
/// 预算同值，超了就是引擎卡死（那时 PNG 也不会有，下面的读图会替它出声）。
constexpr auto kPollBudget = std::chrono::seconds(60);

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

/// llm_shot.file → 绝对路径。基准 = **build/rgl**（引擎子进程 chdir 到那里，见 engine.cpp 的
/// execEngineChild），不是 profile 目录——拼错就"引擎出了图而工具说没出图"。
std::string pngAbsPath(const std::string& shotFile)
{
    return buildRglDir() + "/" + shotFile;
}

// ── 测试输入：两臂 manifest 的 blendLit 块是测量接口的唯一真相源 ──────────────────

struct BlendLitArm {
    std::string stem;      ///< "blend_lit" / "blend_lit_opaque"
    std::string modelFile; ///< scene.lua 里要写成的 entities[].file
    std::string shotFile;  ///< 副本里 llm_shot.file
    BlendLitGeometry geom;
    int expectDraws = 0;   ///< manifest 声明的透明段条数（BLEND 臂 3 / OPAQUE 臂 0）
    std::string armTag;    ///< manifest 的 blendLit.arm（"blend" / "opaque"）
};

const json& requireField(const json& obj, const char* key, const std::string& what)
{
    if (!obj.contains(key))
        throw std::runtime_error(std::string("manifest 里没有 ") + what + key
            + "——测量接口形状变了，本命令拒绝猜");
    return obj.at(key);
}

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
                + " 元不是数值，本命令拒绝猜");
        out[i] = v[i].get<double>();
    }
    return out;
}

/// 读一臂的 manifest。**缺文件 = 抛**（不 SKIP），并把生成命令打进错误消息——
/// 「缺输入」与「输入坏了」在现场是两件事，前者给命令就够了。
BlendLitArm loadArm(const std::string& stem, const std::string& modelFile)
{
    const fs::path dir = fs::path(projectRoot()) / "build/rgl/models";
    const fs::path glb = dir / (stem + ".glb");
    const fs::path manifestPath = dir / (stem + ".manifest.json");
    const std::string gen = "python3 tools/qa_scenes.py --out build/rgl/models --only " + stem;
    if (!fs::exists(glb))
        throw std::runtime_error("测试输入不存在: " + glb.string() + "\n  生成：" + gen
            + "（缺输入必须 FAIL，不许 SKIP——跳过等于判据在「什么都没测」的状态下报绿）");
    if (!fs::exists(manifestPath))
        throw std::runtime_error("测试输入的 manifest 不存在: " + manifestPath.string()
            + "\n  只有 .glb 没有 manifest ⟹ 区域归因的事实来源（相机 / 落位 / 尺寸 / 条数）"
              "没有地方可取，本命令拒绝在 C++ 里重抄一份（第二真相，改场景必漂）。\n  生成："
              + gen);
    const json m = json::parse(readFileText(manifestPath));
    const json& bl = requireField(m, "blendLit", "");
    if (!bl.is_object())
        throw std::runtime_error("manifest 的 blendLit 不是对象——它是场景生成器留给本命令的"
              "测量接口；没有它就得在 C++ 里重抄相机与落位（第二真相），拒绝");

    BlendLitArm a;
    a.stem = stem;
    a.modelFile = modelFile;
    a.shotFile = std::string("screenshots/imgqa_") + stem + ".png";
    const json& cam = requireField(bl, "camera", "blendLit.");
    a.geom.cameraPosition = asArray<3>(requireField(cam, "position", "blendLit.camera."), "camera.position");
    a.geom.cameraTarget
        = asArray<3>(requireField(cam, "target", "blendLit.camera."), "camera.target");
    a.geom.fovDegrees = requireField(cam, "fovDegrees", "blendLit.camera.").get<double>();
    const auto aspect = asArray<2>(requireField(bl, "aspect", "blendLit."), "aspect");
    a.geom.width = static_cast<int>(aspect[0]);
    a.geom.height = static_cast<int>(aspect[1]);
    a.geom.paneZ = requireField(bl, "paneZ", "blendLit.").get<double>();
    a.geom.paneSize = requireField(bl, "paneSize", "blendLit.").get<double>();
    a.geom.backdropHalf = requireField(bl, "backdropHalf", "blendLit.").get<double>();
    a.geom.backdropZ = requireField(bl, "backdropZ", "blendLit.").get<double>();
    for (const auto& v : requireField(bl, "paneXs", "blendLit."))
        a.geom.paneXs.push_back(v.get<double>());
    a.expectDraws = requireField(bl, "expectTransparentDraws", "blendLit.").get<int>();
    a.armTag = requireField(bl, "arm", "blendLit.").get<std::string>();
    return a;
}

/// 两臂 manifest 的**几何事实必须逐字段全等**（只有 arm 标签与 .glb 里的 alphaMode 允许不同）。
/// 这条对账是「双臂只差一个变量」的**机器保证**——没有它，"两臂不同"可以来自任何一处
/// 手滑改动的落位/颜色/画幅，判据退化成噪声计。
void requireSameSourceArms(const BlendLitArm& b, const BlendLitArm& o)
{
    const auto& gb = b.geom;
    const auto& go = o.geom;
    const auto same3 = [](const std::array<double, 3>& x, const std::array<double, 3>& y) {
        return x[0] == y[0] && x[1] == y[1] && x[2] == y[2];
    };
    auto mismatch = [&](const char* what) {
        throw std::runtime_error(std::string("两臂 manifest 的 ") + what
            + " 不一致——本判据的全部意义在「双臂只差 alphaMode」，这一处不同就让"
              "「双臂像素不同」不再指向透明段。拒跑（改 tools/qa_scenes.py 的 "
              "scene_blend_lit，让两臂由同一次构造得到，别手改其中一份 manifest）");
    };
    if (!same3(gb.cameraPosition, go.cameraPosition))
        mismatch("camera.position");
    if (!same3(gb.cameraTarget, go.cameraTarget))
        mismatch("camera.target");
    if (gb.fovDegrees != go.fovDegrees)
        mismatch("camera.fovDegrees");
    if (gb.width != go.width || gb.height != go.height)
        mismatch("aspect");
    if (gb.paneZ != go.paneZ)
        mismatch("paneZ");
    if (gb.paneSize != go.paneSize)
        mismatch("paneSize");
    if (gb.backdropHalf != go.backdropHalf || gb.backdropZ != go.backdropZ)
        mismatch("backdrop");
    if (gb.paneXs != go.paneXs)
        mismatch("paneXs");
    // 条数**必须**不同（否则两臂根本不是单变量对照）。这是反向用例的另一面。
    if (b.expectDraws == o.expectDraws)
        throw std::runtime_error("两臂 manifest 的 expectTransparentDraws 相同（"
            + std::to_string(b.expectDraws) + "）——单变量对照不成立，拒跑");
    if (o.expectDraws != 0)
        throw std::runtime_error("OPAQUE 臂的 expectTransparentDraws 不是 0（"
            + std::to_string(o.expectDraws) + "）——反向用例要求它恰为 0，否则「条数恒非 0」"
              "这种错法测不出来");
}

// ── 配置侧对账：camera.lua 与 manifest 必须是同一组数 ────────────────────────────

/**
 * camera.lua ↔ manifest 的相机对账，不一致即**抛**。
 *
 * 三条拒跑条件里最阴的一条：footprint 的像素坐标是按 manifest 那组相机**解析**算的，而引擎
 * 实际用的是 camera.lua。两者不一致时区域框到的是**别的几何**，而"双臂不同"照旧成立
 * ——它不需要框对地方。于是「配置改了、判据还在按旧机位划区域」唯一的表征是判据仍然绿。
 */
void requireCameraAgreement(const BlendLitGeometry& g)
{
    const fs::path lua = fs::path(projectRoot()) / kConfigDir / "camera.lua";
    const std::string text = readFileText(lua);
    const std::string where = lua.string();
    const auto position = parseLuaVec3(topLevelValueOf(text, "position", where), "camera.lua 的 position");
    const auto target = parseLuaVec3(topLevelValueOf(text, "target", where), "camera.lua 的 target");
    const double fov = std::stod(topLevelValueOf(text, "fovDegrees", where));
    const auto same = [](const std::array<double, 3>& a, const std::array<double, 3>& b) {
        return a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
    };
    if (!same(position, g.cameraPosition) || !same(target, g.cameraTarget)
        || fov != g.fovDegrees)
        throw std::runtime_error(
            "camera.lua 与 manifest 的 blendLit.camera 不一致 ⟹ 区域会框到别的几何上，"
            "而「双臂像素不同」照旧成立 = 假绿。拒跑。\n  camera.lua: position="
            + std::to_string(position[0]) + "," + std::to_string(position[1]) + ","
            + std::to_string(position[2]) + " fov=" + std::to_string(fov)
            + "\n  manifest : position=" + std::to_string(g.cameraPosition[0]) + ","
            + std::to_string(g.cameraPosition[1]) + "," + std::to_string(g.cameraPosition[2])
            + " fov=" + std::to_string(g.fovDegrees)
            + "\n  两处必须一起改：qa/configs/blend_lit/camera.lua 与 tools/qa_scenes.py 的 "
              "BLEND_LIT_CAM");
}

// ── 采集形态：整目录复制 + 改写 entities[].file 与 llm_shot.file ──────────────────

/// 把冻结目录整份复制到 build/rgl 下，只改副本的两枚键（**不碰仓库里的冻结目录**）。
/// 改不到键**必须抛**：没改到也照跑 = 两臂跑的是同一份场景，然后对着两张一样的图报
/// 「透明段没有贡献」，白跑两次引擎才出声。
std::string stageProfile(const BlendLitArm& arm)
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

    const fs::path scene = dst / "scene.lua";
    std::string edited = readFileText(scene);
    // 切臂 = 换 entities 里那一枚模型路径。
    // 🔴 **不用** `replaceValue(..., "file", ...)`：它按「`key = 值,` 整行」的口径重写，
    //   而本靶那一行是 `{ type = "model", file = "models/blend_lit.glb" },` —— 值后面还跟着
    //   ` }` 收尾。实测它把 ` }` 一起吃掉，产出的 scene.lua 少一个花括号，引擎报
    //   `'}' expected (to close '{' at line 10) near <eof>`（第一次跑就是这么红的）。
    //   整串精确替换没有这个洞：它只动那一段带引号的路径，别的字节一个不改。
    const std::string needle = "\"models/blend_lit.glb\"";
    if (edited.find(needle) == std::string::npos)
        throw std::runtime_error("冻结 scene.lua 里找不到 " + needle
            + "——本命令靠改写这一枚路径切臂，改不到就是两臂跑同一份场景（那两张图当然一样，"
              "于是判据会红着说「透明段没贡献」）。拒跑");
    edited.replace(edited.find(needle), needle.size(), "\"" + arm.modelFile + "\"");
    writeFileText(scene, edited);
    const std::string want = "file = \"" + arm.modelFile + "\"";
    if (readFileText(scene).find(want) == std::string::npos)
        throw std::runtime_error("改写后的 scene.lua 里没有 " + want
            + "——写后读回自检失败（同 engine_profile_gates 那条纪律：写了不等于生效）");
    // 括号配对护栏：上面那条注释里讲的失效形态（吃掉 ` }`）在这里第二次被拦住——
    // 读回检查只证明"路径写进去了"，证明不了"文件还是合法 lua"。
    if (!bracesBalanced(readFileText(scene)))
        throw std::runtime_error("改写后的 scene.lua 花括号不配对——entities 那一行是表字面量，"
            "值后面还跟着 ` }`，任何按整行重写值的做法都会把它吃掉");

    const fs::path runtime = dst / "runtime.lua";
    std::string rt = readFileText(runtime);
    replaceValueInBlock(rt, "runtime.lua", "llm_shot", "file", "\"" + arm.shotFile + "\"");
    writeFileText(runtime, rt);
    return dst.string();
}

// ── 跑一臂：轮询 /state → 等退出 → 读图 ──────────────────────────────────────────

struct ArmRun {
    cv::Mat bgr;
    std::vector<std::pair<uint32_t, int>> reads;  ///< (frame, transparentDraws)
    int errorLines = 0;
    uint32_t maxFrame = 0;
    int draws = -1;  ///< 最大帧号那次读数的条数；-1 = 一条都没读到
};

/// 引擎日志里的 `[error]` 行数。口径与 `tools/smoke.py` 的 count_severities 逐字相同
/// （子串匹配 `"[error]"`；severity 映射见 `rgl/utils.cppm` vulkanDebugCallback——
/// 引擎自身 error 与 VVL 的 VUID 都落这一级）。两处各写一遍迟早漂，但跨语言（Python/C++）
/// 没有共享实现可放，故这里在注释里点名对方，改动时两边一起改。
int countErrorLines(const fs::path& logPath)
{
    std::ifstream in(logPath, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到引擎日志: " + logPath.string()
            + "——「[error] 计数 = 0」这条判据不能建立在读不到日志上（读不到 = 没测）");
    int n = 0;
    std::string line;
    while (std::getline(in, line))
        if (line.find("[error]") != std::string::npos)
            ++n;
    return n;
}

/**
 * 把一臂的读数序列收敛成「一个条数」。
 *
 * 两条规则，都不是可有可无的：
 *  · 帧号 ≥ `kBlendLitMinFrame` 的读数里若出现两个不同的条数 ⟹ 场景不是静态的（spin / aa
 *    被人改了），此时"条数"不是一个事实而是一个采样，任何判定都没意义 ⟹ **抛**，不挑一个数用。
 *  · 取**最大帧号**那次读数的条数 = 离 llm_shot 那一帧最近的读数，与像素是同一帧的账。
 */
void summarizeReads(ArmRun& run)
{
    if (run.reads.empty()) {
        std::printf("  ⚠ 一次 /state 都没读到（RPC 绑端口晚于进程退出？见 runtime.lua 的 delay_frames 注）\n");
        return;
    }
    run.maxFrame = std::ranges::max_element(run.reads, {},
                                  [](const std::pair<uint32_t, int>& r) { return r.first; })
                       ->first;
    std::vector<int> distinct;
    for (const auto& [frame, draws] : run.reads) {
        if (frame < kBlendLitMinFrame)
            continue;
        if (std::find(distinct.begin(), distinct.end(), draws) == distinct.end())
            distinct.push_back(draws);
    }
    if (distinct.size() > 1)
        throw std::runtime_error("透明段条数在同一次运行内变化（读到 " + std::to_string(distinct[0])
            + " 与 " + std::to_string(distinct[1])
            + "）——本靶要求静态场景（spin=false / aa=off），条数必须是事实而不是采样。拒跑");
    for (const auto& [frame, draws] : run.reads)
        if (frame == run.maxFrame)
            run.draws = draws;
}

/**
 * 跑一臂并采读数。
 *
 * 🔴 **跑之前先删掉目标 PNG**：`llm_debug` 截图是「引擎写、工具读」的文件交接，而
 * `Engine` 的析构只看退出码。上一轮的旧图留在原地时，本轮引擎即使一帧都没截出来，读图
 * 这一步也照样成功——判据会拿着**上一轮的图**给这一轮发绿。删掉它，「没出图」才是真话。
 *
 * 轮询的退出条件是 `stateRaw()` 抛（连接失败 = 进程已退，RPC 在 llm_shot 那帧之后立刻关）。
 * 解析失败**不**吞掉：那是响应形状坏了，必须当场出声（同 engine.cpp 的 requireJsonObject
 * 那条理由——指错人的异常比异常本身贵）。
 */
ArmRun runArm(const std::string& profile, const std::string& pngPath)
{
    std::error_code ec;
    fs::remove(pngPath, ec);
    ArmRun run;
    {
        Engine eng(profile, kRpcPort);  // 不是 const：stateRaw() 要走连接（有内部状态）
        const auto deadline = std::chrono::steady_clock::now() + kPollBudget;
        while (std::chrono::steady_clock::now() < deadline) {
            std::string body;
            try {
                body = eng.stateRaw();
            } catch (const std::exception&) {
                break;  // 连接层失败 = 引擎已经走了
            }
            const json j = json::parse(body);
            const uint32_t frame = j.at("frame").get<uint32_t>();
            const int draws = j.at("render").at("transparentDraws").get<int>();
            run.reads.emplace_back(frame, draws);
            std::this_thread::sleep_for(2ms);
        }
    }  // 析构里 waitpid 收尸；此后读 PNG 才不可能读到写一半的文件
    summarizeReads(run);
    if (!fs::exists(pngPath))
        throw std::runtime_error("引擎跑完了但没有出图: " + pngPath
            + "（runtime.lua 的 llm_debug / llm_shot 被改了吗？本命令跑之前已把该文件删掉，"
              "所以这里不可能是上一轮的残留）");
    run.bgr = readPngBgr(pngPath);
    run.errorLines = countErrorLines(fs::path(buildRglDir()) / "imgqa_run.log");
    return run;
}

// ── 六条判据的构造（每条一个 push，正文里的归因顺序 = 这里的调用顺序）──────────────

/// 判据 1 / 2 共用：一臂的「引擎回显条数 == 声明值」。
/// 读数缺失（`run.draws < 0`）走**同一条判据的 FAIL 分支**而不是抛——读出口坏了与透明段
/// 没画是两件事，但都不许报绿，也不许 SKIP。
void pushDrawsVerdict(std::vector<Verdict>& vs, const char* name, const char* armName,
    const ArmRun& run, int expect, const char* failHint)
{
    const bool read = run.draws >= 0;
    const bool ok = read && run.draws == expect;
    std::string obs = read
        ? (std::string(armName) + " /state 回显 transparentDraws=" + std::to_string(run.draws)
              + "，要求 " + std::to_string(expect) + "（读数 " + std::to_string(run.reads.size())
              + " 次，最大帧号 " + std::to_string(run.maxFrame) + "）")
        : (std::string(armName) + "一次 /state 都没读到（读数 0 次）——读出口坏了，"
             "不是「透明段没画」；本命令**不许**用数像素代替引擎回显，也不许 SKIP");
    if (read && !ok)
        obs += std::string("\n  归因：") + failHint;
    vs.push_back(verdictFor(name, ok, std::move(obs)));
}

/// 判据 3 / 4：像素侧的两条（贡献 / 归属）。两条吃同一次分桶读数，成对出现才有意义——
/// 只报「变了」= 任何旁路都能过；只报「之外没变」= 两臂完全相同也过。
void pushPixelVerdicts(std::vector<Verdict>& vs, const BlendLitArmDiff& d)
{
    {
        const bool ok = d.changedInside > 0 && d.maxLsbInside >= kBlendLitPowerFloorLsb;
        std::string obs = "footprint(+" + std::to_string(kBlendLitPadPx) + "px) 内 changed="
            + std::to_string(d.changedInside) + " 像素，最大字节差 " + std::to_string(d.maxLsbInside)
            + " LSB（要求 >0 且 ≥ " + std::to_string(kBlendLitPowerFloorLsb) + "）";
        if (!ok)
            obs += "\n  归因：透明段有 draw 却没往像素上写 ⟹ 先查 transparent pass 的 blend 状态"
                   "（rgl/shader.cppm 的 colorBlend 分支）与 pipeline 是否真被录进那一段";
        vs.push_back(verdictFor("blend-lit-contributes", ok, std::move(obs)));
    }
    {
        const bool ok = d.changedOutside == 0;
        std::string obs = "footprint 之外 changed=" + std::to_string(d.changedOutside)
            + " 像素，最大字节差 " + std::to_string(d.maxLsbOutside) + " LSB（要求**恰为 0**）";
        if (!ok)
            obs += "\n  归因：两臂在「透明段画不到」的地方也不同 ⟹ 双臂不是单变量对照"
                   "（最常见：阴影旁路——collectShadowCasters 排 blend，开着阴影时 OPAQUE 臂"
                   "会投影。查 qa/configs/blend_lit/shadow.lua 的 mode 还是不是 none）";
        vs.push_back(verdictFor("blend-lit-confined", ok, std::move(obs)));
    }
}

/// 判据 6：pane 在**BLEND 臂自己的交付图上看得见**（与该臂自己的背景板比，不跨臂）。
///
/// 🔴 为什么必须有这条（是**变异测出来的**，不是想到的）：把 `render_list_impl.cppm` 里
/// 「透明段 append 进 calls」那个循环删掉（= 透明段整段为空，正是债-134 记的那个状态），
/// 判据 1 红，但判据 3 **照旧绿** —— 因为透明段没了之后 BLEND 臂在 footprint 里露出的是
/// 背景板，而 OPAQUE 臂在那儿是不透明的 pane，两臂**还是不同**。
/// ⟹ `contributes` 单独担不起「透明段画了」这句话，它只证「这块像素在两臂之间不一样」。
/// 本条把这一洞补在**像素侧**：与背景板同色 = 这块根本没被透明段写过，红。
/// 补在像素侧的意义 = 万一引擎侧那条读数口坏了（RPC 拿不到 / 字段被改），像素侧仍然不是恒真。
void pushVisibleVerdict(std::vector<Verdict>& vs, const cv::Mat& blendBgr,
    const std::vector<BlendLitBox>& boxes)
{
    const auto backdrop = blendLitDominant(blendBgr, boxes, false);
    int worst = 1 << 30;
    int worstBox = -1;
    std::string detail;
    for (size_t i = 0; i < boxes.size(); ++i) {
        const std::vector<BlendLitBox> one{boxes[i]};
        const auto pane = blendLitDominant(blendBgr, one, true);
        const int delta = blendLitColorDelta(pane, backdrop);
        detail += " pane[" + std::to_string(i) + "] 众数(" + std::to_string(pane[0]) + ","
            + std::to_string(pane[1]) + "," + std::to_string(pane[2]) + ") Δ"
            + std::to_string(delta);
        if (delta < worst) {
            worst = delta;
            worstBox = static_cast<int>(i);
        }
    }
    const bool ok = worst >= kBlendLitPowerFloorLsb;
    std::string obs = "BLEND 臂自身对照：背景板众数(" + std::to_string(backdrop[0]) + ","
        + std::to_string(backdrop[1]) + "," + std::to_string(backdrop[2]) + ")，三支 pane 的"
        " footprint 内众数与之最小差 "
        + std::to_string(worst) + " LSB（要求 ≥ " + std::to_string(kBlendLitPowerFloorLsb)
        + "）" + detail;
    if (!ok)
        obs += "\n  归因：pane[" + std::to_string(worstBox)
               + "] 的 footprint 与背景板同色 ⟹ 透明段在那块像素上**什么都没写**（整段为空 / "
                 "该 pipeline 没被录进段 / 该类别变体的 SPV 没画）。判据 3 此时可能仍绿——"
                 "它只比两臂，不比背景板";
    vs.push_back(verdictFor("blend-lit-visible", ok, std::move(obs)));
}

/// 判据 5：两次启动的 `[error]` 计数之和 = 0。
void pushCleanVerdict(std::vector<Verdict>& vs, const ArmRun& rb, const ArmRun& ro)
{
    const int n = rb.errorLines + ro.errorLines;
    std::string obs = "两次启动的 [error] 行数 = " + std::to_string(rb.errorLines) + " + "
        + std::to_string(ro.errorLines) + "（口径同 tools/smoke.py 的 count_severities）";
    if (n != 0)
        obs += "\n  取证：build/rgl/imgqa_run.log（最后一次启动的日志，前一次的已被覆盖）";
    vs.push_back(verdictFor("blend-lit-clean", n == 0, std::move(obs)));
}

}  // namespace

// ── blend-lit：透明段像素贡献的六条判据 ──────────────────────────────────────────
int cmdBlendLit()
{
    std::printf("blend-lit —— 透明段 lit 变体的常驻像素贡献判据（输入 = qa/configs/blend_lit）\n");

    // ① 读两臂测试输入 + 四条拒跑自检，全部**先于**跑引擎。这四条的含义是「这次测量没有
    //    意义」，白跑两次引擎（每次约 3 秒 + 一次 shader 编译）才说这句话太贵。
    const BlendLitArm blend = loadArm("blend_lit", "models/blend_lit.glb");
    const BlendLitArm opaque = loadArm("blend_lit_opaque", "models/blend_lit_opaque.glb");
    requireSameSourceArms(blend, opaque);
    requireCameraAgreement(blend.geom);
    requireBackdropCoverage(blend.geom);
    const std::vector<BlendLitBox> boxes = blendLitBoxes(blend.geom);
    std::printf("  画幅 %dx%d  paneZ=%.2f 尺寸=%.2f 背景板半尺寸=%.1f@z=%.1f  footprint 数=%zu\n",
        blend.geom.width, blend.geom.height, blend.geom.paneZ, blend.geom.paneSize,
        blend.geom.backdropHalf, blend.geom.backdropZ, boxes.size());
    for (size_t i = 0; i < boxes.size(); ++i)
        std::printf("    pane[%zu] x[%d,%d] y[%d,%d]\n", i, boxes[i].x0, boxes[i].x1, boxes[i].y0,
            boxes[i].y1);
    std::printf("  四条拒跑自检通过（两臂同源 / 相机对账 / 背景板铺满 / footprint 互不重叠）\n");

    // ② 两臂各跑一次（同一次进程里既出图又出条数；副本目录，不动仓库里的冻结配置）。
    //     PNG 的落点 = **CWD（build/rgl）相对 llm_shot.file**，不是 profile 目录相对——引擎
    //     子进程的 chdir 目标是 build/rgl（见 engine.cpp 的 execEngineChild），拼错就找不到图。
    const std::string profBlend = stageProfile(blend);
    std::printf("\n  跑 BLEND 臂（%s）\n", blend.modelFile.c_str());
    const ArmRun rb = runArm(profBlend, pngAbsPath(blend.shotFile));
    const std::string profOpaque = stageProfile(opaque);
    std::printf("  跑 OPAQUE 臂（%s）\n", opaque.modelFile.c_str());
    const ArmRun ro = runArm(profOpaque, pngAbsPath(opaque.shotFile));

    // ③ 六条判据。画幅先对账：区域坐标是按 manifest 设计尺寸解析算的，尺寸不对时两条像素
    //    判据量到的是别的像素上 —— 那是「测量没有意义」，抛，不判红。
    if (rb.bgr.cols != blend.geom.width || rb.bgr.rows != blend.geom.height)
        throw std::runtime_error("截图 " + std::to_string(rb.bgr.cols) + "x"
            + std::to_string(rb.bgr.rows) + " != manifest 设计尺寸 " + std::to_string(blend.geom.width)
            + "x" + std::to_string(blend.geom.height)
            + "——区域坐标是按设计尺寸解析算的，换画幅要同时改 runtime.lua 的 headless_size / "
              "llm_shot.size 与 qa_scenes.py 的 aspect");
    const BlendLitArmDiff d = blendLitCompareArms(rb.bgr, ro.bgr, boxes);

    std::vector<Verdict> vs;
    pushDrawsVerdict(vs, "blend-lit-draws", "BLEND 臂", rb, blend.expectDraws,
        "条数 ≠ 声明值 ⟹ 先查 render_list_impl.cppm 的 blend 分桶与 transparentStart 赋值，"
        "再查 debug_rpc_exec_impl.cppm 的 segmentSpan 口径");
    pushDrawsVerdict(vs, "blend-lit-reverse", "OPAQUE 臂", ro, opaque.expectDraws,
        "OPAQUE 臂也进了透明段 ⟹ 分桶判据（material.alphaMode）没吃进去，"
        "于是判据 1 的「非 0」是恒真的");
    pushPixelVerdicts(vs, d);
    pushVisibleVerdict(vs, rb.bgr, boxes);
    pushCleanVerdict(vs, rb, ro);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("blend-lit", {{"config", std::string(kConfigDir)}, {"run", profBlend},
        {"twin", opaque.stem},
        {"size", std::to_string(blend.geom.width) + "x" + std::to_string(blend.geom.height)},
        {"drawsBlend", std::to_string(rb.draws)}, {"drawsOpaque", std::to_string(ro.draws)},
        {"changedInside", std::to_string(d.changedInside)},
        {"changedOutside", std::to_string(d.changedOutside)}},
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    std::printf("\n%s\n", ok ? "全部判据绿" : "有判据红（正文的归因顺序先查哪一层）");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
