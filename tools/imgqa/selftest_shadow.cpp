// selftest_shadow.cpp —— **阴影族两台量具**（shadow-softness / shadow-kernel-scale）。
//
// 两台量具各有两个面，四个块同处一个 TU 的理由是**它们的改动缝其实是同一条**（ProfileOpts
// 的覆写键 + 阴影路由枚举 + shadow.lua 的键形状）：一次改配置键要同时动这四个块，切成四个
// 文件只会把「漏改一处」变成静默漂移。
//   · profile 生成面（`testShadowSoftnessArms` / `…IzbForm` / `…CommentTrap` /
//     `testShadowKernelScaleInstrument`）—— 测 `makeProfile` 的 lua 覆写门：臂塌缩、矛盾
//     组合拒绝、izb 的覆写形态（引擎只收子表；覆写器两种文本跨度都吃）、注释陷阱、截图门、以及「门关态逐位不变」。
//   · 度量算子面（`testShadowSoftnessMetrics` / `testShadowKernelScale` / `…Sampling`）——
//     期望全部来自闭式（阈值交叉位置是解析的、覆盖率是整数比），不吃截图。
// 实机数字只能证明「与上次一样」，闭式才能证明「算子算的是我说的那个量」（同 testAoMetrics）。
//
// 搬动纪律：断言本体逐字搬自拆分前的 selftest.cpp（一条不多、一条不少、不改判定方向与阈值）。
#include "selftest.h"

#include "shadow_kernel_metric.h"
#include "imgqa.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {

// ── 软影滤波轴量具（批 N6d 步 0，shadow_softness.cpp）───────────────────────────
//
// 两条独立面，各自堵不同的失效：**臂塌缩**（生成的 shadow.lua 真的是那一臂）与
// **度量算子**（期望全部来自定义本身，不吃截图）。实机数字只能证明「与上次一样」，
// 闭式才能证明「算子算的是我说的那个量」（同 testAoMetrics 的纪律）。
int64_t softnessCount(const std::string& text, const std::string& needle)
{
    int64_t n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1))
        ++n;
    return n;
}

/// 生成一臂的 shadow.lua 文本（门的形态 = 整份拷交付配置 + 覆写，**不启引擎**）
std::string softnessShadowOf(const char* mode, double sun)
{
    const std::string tmp = "/tmp/imgqa_selftest_softness";
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";
    o.overrideShadowRoute = true;
    o.shadowRouteMode = mode;
    o.shadowRouteSunDegrees = sun;
    makeProfile(tmp, o);
    std::ifstream f(tmp + "/shadow.lua");
    return std::string { std::istreambuf_iterator<char>{f}, {} };
}

/// makeProfile 的「不许抛」包装：门抛 = 覆写没成功（正确行为是响），但不该让整份自检
/// 中途退出——后面的用例就再也不跑了。落成一条 FAIL 才既响又不吞。
void softnessGen(Checker& c, const std::string& dir, const ProfileOpts& o)
{
    try {
        makeProfile(dir, o);
    } catch (const std::exception& e) {
        c.chk("softness profile 生成成功（门不该抛：抛 = 覆写没成功）", false, e.what());
    }
}

// ── shadow-kernel-scale 的度量层与 profile 生成（shadow_kernel_scale.cpp）───────────
//
// 期望值一律来自**闭式**（线性斜坡的阈值交叉位置是解析的、覆盖率是整数比），不来自实机
// 截图——截图只能证明「与上次一样」，闭式才能证明「算子算的是我说的那个量」。每条都配
// **反向用例**（算子在应该无响应的地方必须无响应），并写明**变异红线**（把实现改坏成哪种
// 具体形态时本用例会红）。
/// 斜坡剖面：[lo, s0) 恒 v0、[s0, s1] 线性到 v1、(s1, hi] 恒 v1。
std::vector<ShadowScanSample> rampProfile(double s0, double v0, double s1, double v1,
    double step, double lo, double hi)
{
    std::vector<ShadowScanSample> out;
    const int n = static_cast<int>(std::llround((hi - lo) / step));
    for (int i = 0; i <= n; ++i) {
        ShadowScanSample p;
        p.s = lo + step * i;
        p.valid = true;
        const double t = p.s <= s0 ? 0.0 : (p.s >= s1 ? 1.0 : (p.s - s0) / (s1 - s0));
        p.value = v0 + (v1 - v0) * t;
        out.push_back(p);
    }
    return out;
}

/// 平坦剖面（可整片置为无效点，用来演「参照全黑」与「投出屏」）。
std::vector<ShadowScanSample> flatProfile(double v, double step, double lo, double hi,
    bool valid = true)
{
    std::vector<ShadowScanSample> out = rampProfile(lo, v, lo, v, step, lo, hi);
    for (auto& p : out)
        p.valid = valid;
    return out;
}

cv::Mat grayLuma(const cv::Size& sz, double v)
{
    return cv::Mat(sz.height, sz.width, CV_64FC1, cv::Scalar(v));
}

}  // namespace

/// 面 A：臂塌缩与矛盾组合拒绝（生成的 shadow.lua 是不是真的那一臂）
void testShadowSoftnessArms(Checker& c)
{
    const std::string tmp = "/tmp/imgqa_selftest_softness";
    // 门抛 = 「拒绝在旧值上下判据」的正确行为，但**不该让整份自检中途退出**（后面的用例
    // 就再也不跑了）——落成一条 FAIL 才既响又不吞（同 testKeyValidation 的 try/catch 先例）。
    const auto shadowOf = [&c](const char* mode, double sun) {
        try {
            return softnessShadowOf(mode, sun);
        } catch (const std::exception& e) {
            c.chk("softness 臂生成成功（门不该抛：抛 = 覆写没成功）", false, e.what());
            return std::string();
        }
    };
    const auto countOf = softnessCount;

    // ① 出厂档（sun 不写键）：mode 换对、izb 子表整值换成最小子表、两档 sun_degrees 原封不动
    const std::string def = shadowOf("evssm", -1.0);
    c.chk("softness 臂塌缩：mode=\"evssm\" 写对", def.find("mode = \"evssm\"") != std::string::npos);
    c.chk("softness 臂塌缩：izb 子表整值换成 { on = false }（旧子表键必须离场）",
        def.find("izb = { on = false }") != std::string::npos && def.find("on = true") == std::string::npos,
        "留 `grid = 512` = Lua 语法错；留 on = true = 硬影 mask 吞掉半影");
    c.chk("softness 臂塌缩：sun 不写键 = 两档都还是出厂 0.53",
        countOf(def, "sun_degrees = 0.53") == 2, std::to_string(countOf(def, "sun_degrees")));

    // ② sun0 臂**真的写了 0**，而不是"没写"（没写 = 出厂 0.53 = 另一臂，判据就在出厂值上跑）
    const std::string s0 = shadowOf("evssm", 0.0);
    c.chk("softness 臂塌缩：evssm_sun0 真的写了 sun_degrees = 0（不是漏写）",
        countOf(s0, "sun_degrees = 0,") == 1 && countOf(s0, "sun_degrees = 0.53") == 1);
    // 写进**本档**的块：evssm 块头是文件里最后一个 "evssm"（前面两处都在注释里），
    // sun_degrees = 0 必须落在它之后 —— 否则就是改到了 pcss 那份（两档分立是消债-16⑤）。
    c.chk("softness 臂塌缩：sun_degrees=0 落在 evssm 块内（不越界改到 pcss）",
        s0.rfind("evssm") < s0.find("sun_degrees = 0,"));

    // ③ pcss 档同理反向对账：pcss 块被改、evssm 块保持出厂
    const std::string p5 = shadowOf("pcss", 5.0);
    c.chk("softness 臂塌缩：pcss_sun5 只改 pcss 块（evssm 仍是 0.53）",
        countOf(p5, "sun_degrees = 5,") == 1 && countOf(p5, "sun_degrees = 0.53") == 1
            && p5.find("sun_degrees = 5,") < p5.rfind("evssm"));

    // ④ 矛盾组合一律拒（"配置里写了、什么都没发生" 是这类工具最贵的失效）
    // 拒绝类用例走**未包装**的 softnessShadowOf：这里要的就是"它抛"（包装版会把抛吞成
    // 一条 FAIL，两个方向都想要只能分开调用点）。
    c.chk("softness 门：非法档名被拒",
        throwsStd([&] { softnessShadowOf("evssm_soft", -1.0); }));
    c.chk("softness 门：太阳角给 csm 档被拒（csm 不消费它）",
        throwsStd([&] { softnessShadowOf("csm", 5.0); }));
    c.chk("softness 门：没有 deliveryConfigDir 时开门被拒（合成路径无实现 = 静默不生效）",
        throwsStd([&] {
            ProfileOpts o;
            o.glb = "shadow_rig.glb";
            o.overrideShadowRoute = true;
            o.shadowRouteMode = "evssm";
            makeProfile(tmp, o);
        }));

    // ⑤ 一次性截图门：帧号与开关都写对，且 rpc 同时关（引擎约定：llm 截图在跑时 /shot 409）
    {
        ProfileOpts o;
        o.deliveryConfigDir = projectRoot() + "/configs";
        o.overrideLlmShot = true;
        o.llmShotFile = "screenshots/selftest_softness.png";
        o.llmShotDelayFrames = 12;
        softnessGen(c, tmp, o);
        std::ifstream rt(tmp + "/runtime.lua");
        const std::string rtText { std::istreambuf_iterator<char>{rt}, {} };
        std::ifstream dg(tmp + "/diagnostics.lua");
        const std::string dgText { std::istreambuf_iterator<char>{dg}, {} };
        c.chk("softness 截图门：llm_debug=true + delay_frames=12 写对",
            rtText.find("llm_debug = true") != std::string::npos
                && rtText.find("delay_frames = 12") != std::string::npos);
        c.chk("softness 截图门：file 落在 llm_shot 块内被改写",
            rtText.find("selftest_softness.png") != std::string::npos
                && countOf(rtText, "file = ") == 1);
        c.chk("softness 截图门：rpc 同时关（两种采集形态互斥）",
            dgText.find("rpc = false") != std::string::npos);
        c.chk("softness 截图门：file 为空被拒", throwsStd([&] {
            ProfileOpts bad;
            bad.deliveryConfigDir = projectRoot() + "/configs";
            bad.overrideLlmShot = true;
            makeProfile(tmp, bad);
        }));
    }

}

/// 面 C：`izb` 的**两种文本跨度**都要能覆写（主 agent 的硬要求：两种都要能写，或明确拒绝一种）。
/// 引擎硬切后只收子表形态（扁平布尔 = 启动报错），但覆写器是**文本层**——交付目录哪天残留
/// 布尔形态（旧副本 / 手改），标量跨度分支仍要能整值换掉，打偏的方向必须是"抛"而不是静默。
/// 交付配置今天是子表形态（`{ on = false, grid = 512, dump_frame = 0 }`），面 A 已覆盖；
/// 这里造一份布尔形态的临时交付目录——`izb = true` 改成 `{ on = false }` 走的是**标量值跨度**
/// 那条分支（子表走的是配对 `}` 分支）。只测一条 = 覆写器哪天静默打偏。
void testShadowSoftnessIzbForm(Checker& c)
{
    namespace fs = std::filesystem;
    const std::string src = projectRoot() + "/configs";
    const std::string dst = "/tmp/imgqa_selftest_softness_bool";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst);
    for (const auto& e : fs::directory_iterator(src)) {
        if (e.path().extension() != ".lua")
            continue;
        fs::copy_file(e.path(), dst + "/" + e.path().filename().string(),
            fs::copy_options::overwrite_existing);
    }
    // 把交付的 izb 子表整行换成布尔形态 true（引擎硬切后不收这种 = 本靶只喂给**覆写器**，
    // 测它的标量跨度分支；覆写落上后产物是合法子表形态，引擎吃）
    std::ifstream in(dst + "/shadow.lua");
    std::string sh { std::istreambuf_iterator<char>{in}, {} };
    const size_t at = sh.find("izb = {");
    const size_t tail = sh.find("},", at);
    c.chk("softness 布尔形态靶：交付 shadow.lua 里找到 izb 子表（找不到 = 交付配置改了形）",
        at != std::string::npos && tail != std::string::npos);
    sh = sh.substr(0, at) + "izb = true," + sh.substr(tail + 2);

    std::ofstream out(dst + "/shadow.lua");
    out << sh;
    out.close();
    ProfileOpts o;
    o.deliveryConfigDir = dst;
    o.overrideShadowRoute = true;
    o.shadowRouteMode = "evssm";
    o.shadowRouteSunDegrees = 0.0;
    softnessGen(c, dst + "/profile", o);
    std::ifstream f(dst + "/profile/shadow.lua");
    const std::string got { std::istreambuf_iterator<char>{f}, {} };
    c.chk("softness 布尔形态：izb = true 被覆写成 { on = false }（标量分支）",
        got.find("izb = { on = false }") != std::string::npos && got.find("mode = \"evssm\"") != std::string::npos,
        "覆写器只吃子表分支的话，这里会留下 izb = true = 硬影 mask 吞掉半影");
}

/// 面 D：注释里的同名示例**不得**骗过覆写器的写后自检（主 agent 复核时加的回归靶）。
/// 自检原本是 `sh.find("on = true") == npos` 那种"旧值离场"全文搜法——今天能过只因为
/// configs/shadow.lua 的注释恰好没写这个字面量。哪天有人在注释里补一行子表写法示例，
/// 旧搜法就抛一句"覆写后自检失败"，而配置其实是对的（排查方向会被带去怀疑覆写器）。
/// 现在自检读回**解析后的值**，与注释彻底解耦。
/// 变异红线：把 `topLevelValueOf` 换回全文搜字面量 → 本用例红。
void testShadowSoftnessCommentTrap(Checker& c)
{
    namespace fs = std::filesystem;
    const std::string src = projectRoot() + "/configs";
    const std::string dst = "/tmp/imgqa_selftest_softness_trap";
    std::error_code ec;
    fs::remove_all(dst, ec);
    fs::create_directories(dst);
    for (const auto& e : fs::directory_iterator(src)) {
        if (e.path().extension() != ".lua")
            continue;
        fs::copy_file(e.path(), dst + "/" + e.path().filename().string(),
            fs::copy_options::overwrite_existing);
    }
    std::ifstream in(dst + "/shadow.lua");
    std::string sh { std::istreambuf_iterator<char>{in}, {} };
    const size_t at = sh.find("izb = {");
    c.chk("softness 注释陷阱靶：交付 shadow.lua 里找到 izb 键", at != std::string::npos);
    // 在 izb 键**之前**插一行讲子表写法的注释（含 `on = true` 字面量）
    sh = sh.substr(0, at) + "    -- 子表形态示例：izb = { on = true, grid = 512, dump_frame = 0 }\n"
        + sh.substr(at);
    std::ofstream out(dst + "/shadow.lua");
    out << sh;
    out.close();

    ProfileOpts o;
    o.deliveryConfigDir = dst;
    o.overrideShadowRoute = true;
    o.shadowRouteMode = "csm";
    softnessGen(c, dst + "/profile", o);  // 门内自检若被注释骗到 → 这里抛，chk 记红
    std::ifstream f(dst + "/profile/shadow.lua");
    const std::string got { std::istreambuf_iterator<char>{f}, {} };
    c.chk("softness 注释陷阱：注释含 `on = true` 时覆写仍成功且 izb = { on = false }",
        got.find("izb = { on = false }") != std::string::npos && got.find("mode = \"csm\"") != std::string::npos,
        "自检若在全文搜 `on = true`，会因这行注释抛「覆写后自检失败」= 配置没错、工具自己错");
}

/// 面 B：度量算子的闭式期望（不吃截图——实机数字只能证明「与上次一样」）
void testShadowSoftnessMetrics(Checker& c)
{
    // ⑥ 度量算子的闭式期望（字节域 luma + 四个差分统计 + 太阳角敏感度）
    const auto flat = [](double v) {
        cv::Mat m(2, 2, CV_64FC1, cv::Scalar(v));
        return m;
    };
    cv::Mat bgr = cv::Mat(1, 3, CV_8UC3, cv::Scalar(0, 0, 0));
    bgr.at<cv::Vec3b>(0, 1) = cv::Vec3b(0, 0, 255);  // 纯红
    bgr.at<cv::Vec3b>(0, 2) = cv::Vec3b(255, 255, 255);
    const cv::Mat lum = byteLuma(bgr);
    c.chk("softness byteLuma：黑=0 / 纯红=0.2126×255 / 白=255（权重与通道序）",
        std::abs(lum.at<double>(0, 0)) < 1e-9
            && std::abs(lum.at<double>(0, 1) - 0.2126 * 255.0) < 1e-6
            && std::abs(lum.at<double>(0, 2) - 255.0) < 1e-9);
    {
        const SoftArmMetrics same = measureSoftArm(flat(100.0), flat(100.0));
        c.chk("softness 度量：同图 = 覆盖 0 且带核比 NaN（分母 0 不假装是 0）",
            same.coverage == 0.0 && !std::isfinite(same.bandCore));
        const SoftArmMetrics deep = measureSoftArm(flat(100.0), flat(50.0));  // Δ=50 > 40
        c.chk("softness 度量：Δ=50 全落深本影（core=1、band=0、带核比 0）",
            deep.coverage == 1.0 && deep.core == 1.0 && deep.band == 0.0
                && deep.bandCore == 0.0 && std::abs(deep.meanDark - 50.0) < 1e-9);
        const SoftArmMetrics soft = measureSoftArm(flat(100.0), flat(70.0));  // 8<Δ=30≤40
        c.chk("softness 度量：Δ=30 全落过渡带（core=0 ⟹ 带核比 NaN）",
            soft.coverage == 1.0 && soft.band == 1.0 && soft.core == 0.0
                && !std::isfinite(soft.bandCore));
        cv::Mat mix = flat(70.0);
        mix.at<double>(1, 0) = 50.0;
        mix.at<double>(1, 1) = 50.0;
        const SoftArmMetrics both = measureSoftArm(flat(100.0), mix);
        c.chk("softness 度量：半带半核 ⟹ 带核比恰 1.0（比值口径的阳性锚点）",
            std::abs(both.band - 0.5) < 1e-12 && std::abs(both.core - 0.5) < 1e-12
                && std::abs(both.bandCore - 1.0) < 1e-12);
        // 区间开闭：8 不算、40 归带（定义是 8<Δ≤40）；写反 = 边界像素被算两次或一次不算
        c.chk("softness 度量：Δ=8 不计（严格 >）、Δ=40 归过渡带（≤40）",
            measureSoftArm(flat(100.0), flat(92.0)).coverage == 0.0
                && measureSoftArm(flat(100.0), flat(60.0)).core == 0.0
                && measureSoftArm(flat(100.0), flat(60.0)).band == 1.0);
        // 反向用例：变亮不得计入"影"（否则 skybox 变化之类的整体提亮会伪装成覆盖率暴涨）
        const SoftArmMetrics bright = measureSoftArm(flat(100.0), flat(120.0));
        c.chk("softness 度量：变亮不计入覆盖、平均变暗恒非负",
            bright.coverage == 0.0 && bright.meanDark == 0.0);
        c.chk("softness 太阳角敏感度：同图 0 / Δ=8 不计 / Δ=9 全计（与覆盖同一判定线）",
            sunSensitivity(flat(10.0), flat(10.0)) == 0.0
                && sunSensitivity(flat(10.0), flat(2.0)) == 0.0
                && sunSensitivity(flat(10.0), flat(1.0)) == 1.0);
        c.chk("softness 度量：尺寸不同立刻抛（两臂不同帧/不同尺寸不能被算成 0 差）",
            throwsStd([&] { (void)measureSoftArm(flat(1.0), cv::Mat(3, 3, CV_64FC1, cv::Scalar(1.0))); }));
    }

    // ⑦ 三条判决线必须在注册表里（改名而漏挂号 = verdictFor 运行期才炸，太晚）
    for (const char* name :
        { "shadow-softness-gauge-valid", "shadow-softness-sun-response", "shadow-softness-vs-csm" }) {
        bool found = false;
        for (const auto& j : registeredJudgments())
            found = found || std::string(j.name) == name;
        c.chk("softness 判据已挂号进注册表", found, name);
    }
}

void testShadowKernelScale(Checker& c)
{
    // ① 闭式：下降斜坡 S: 1→0 跨 [0,10] ⟹ 90% 交叉在 s=1、10% 在 s=9 ⟹ W = 0.8×10 = **8.0**。
    //    这条钉的是「宽度 = |s90 − s10| 且线性插值」，变异红线：把 10/90 写成 5/95 或把
    //    插值改成取整（返回 s0）都立刻红。
    const std::vector<ShadowScanSample> ramp = rampProfile(0.0, 1.0, 10.0, 0.0, 0.5, 0.0, 10.0);
    c.chk("kernelscale 交叉：下降斜坡的 90%/10% 交叉恰在 1.0 / 9.0（线性插值闭式）",
        std::abs(shadowCrossing(ramp, 0.9, 0.0, 10.0) - 1.0) < 1e-9
            && std::abs(shadowCrossing(ramp, 0.1, 0.0, 10.0) - 9.0) < 1e-9);
    c.chk("kernelscale 宽度：斜坡 W = 0.8×跨度 = 8.0（米，不做任何像素换算）",
        std::abs(shadowPenumbraWidth(ramp, 0.0, 10.0) - 8.0) < 1e-9,
        std::to_string(shadowPenumbraWidth(ramp, 0.0, 10.0)));

    // ② 反向（**缺陷态的算子级保证**）：全 0 剖面（影根本没画出来）必须给 **NaN**，
    //    既不是 0 也不是任何数。写成 0 = 「影缘无限硬」= 把缺陷读成满分；写成 1 = 「完美地平」。
    c.chk("kernelscale 宽度：无影剖面 → NaN（不是 0！）",
        !std::isfinite(shadowPenumbraWidth(flatProfile(0.0, 0.5, 0.0, 16.0), 0.0, 16.0)));
    c.chk("kernelscale 宽度：常数剖面（S 恒 0.5，无 10/90 交叉）→ NaN",
        !std::isfinite(shadowPenumbraWidth(flatProfile(0.5, 0.5, 0.0, 16.0), 0.0, 16.0)));
    c.chk("kernelscale 交叉：整片无效点（投出屏 / 参照全黑）→ NaN",
        !std::isfinite(shadowCrossing(flatProfile(1.0, 0.5, 0.0, 16.0, false), 0.5, 0.0, 16.0)));

    // ③ 窗口是判据的一部分：交叉落在 [lo,hi] 外**不许**被拿去用（否则「扫描窗随便放宽」
    //    就能把上游缘当成下游缘，宽度读数换的靶子都没人知道）。
    c.chk("kernelscale 交叉：窗口外的交叉不计（同一条剖面换窗即换读数）",
        !std::isfinite(shadowCrossing(ramp, 0.5, 6.0, 10.0))
            && std::abs(shadowCrossing(ramp, 0.5, 0.0, 10.0) - 5.0) < 1e-9);

    // ④ 取**第一个**交叉：窗口内先撞上板体轮廓（下穿）再撞上影缘（上穿）时，判据量的是
    //    先出现的那条。变异红线：改成从后往前扫 → 本条红。
    std::vector<ShadowScanSample> two = rampProfile(0.0, 1.0, 2.0, 0.0, 0.5, 0.0, 16.0);
    for (auto& p : two)
        if (p.s > 8.0)
            p.value = 1.0 - (p.s - 8.0) / 2.0;  // 8→10 再下一次（第二条缘）
    c.chk("kernelscale 交叉：两条缘都在窗口内时取**第一个**（板体轮廓不得冒充影缘）",
        std::abs(shadowCrossing(two, 0.5, 0.0, 16.0) - 1.0) < 1e-9,
        std::to_string(shadowCrossing(two, 0.5, 0.0, 16.0)));

    // ⑤ 影覆盖率（判据 1 的算子本体）。闭式：半幅 S=1、半幅 S=0 ⟹ 恰 0.5。
    const cv::Size sz(4, 4);
    cv::Mat ref = grayLuma(sz, 100.0);
    cv::Mat arm = grayLuma(sz, 100.0);
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < sz.width; ++x)
            arm.at<double>(y, x) = 0.0;  // 上半幅全影（S=1），下半幅同参照（S=0）
    c.chk("kernelscale 覆盖率：半幅全影 → 恰 0.5（分母 = 全幅）",
        std::abs(shadowUmbraFraction(ref, arm) - 0.5) < 1e-12);
    c.chk("kernelscale 覆盖率：同图 → 恰 0（反向用例）",
        shadowUmbraFraction(ref, ref) == 0.0);
    cv::Mat bright = grayLuma(sz, 200.0);
    c.chk("kernelscale 覆盖率：变亮不计入影（反向用例，否则整体提亮伪装成覆盖率暴涨）",
        shadowUmbraFraction(ref, bright) == 0.0);
    // 🔴 分母下限是本判据最要紧的一条反向：env/skybox 全关时**背景就是黑的**，
    // S = (0−0)/0 会爆成 1 ⟹ 整屏算成「有影」= 缺陷态刷绿。变异红线：删掉 `>= 1 LSB`
    // 这个下限 → 下面两条同时红。
    c.chk("kernelscale 覆盖率：参照全黑（背景）不得算成影",
        shadowUmbraFraction(grayLuma(sz, 0.0), grayLuma(sz, 0.0)) == 0.0);
    c.chk("kernelscale 覆盖率：参照 0.5 LSB（低于 1 LSB 下限）→ 0 而不是 1",
        shadowUmbraFraction(grayLuma(sz, 0.5), grayLuma(sz, 0.0)) == 0.0);
    c.chk("kernelscale 覆盖率：S 恰为 0.5 不计（严格 >），0.5+ε 才计",
        shadowUmbraFraction(grayLuma(sz, 100.0), grayLuma(sz, 50.0)) == 0.0
            && shadowUmbraFraction(grayLuma(sz, 100.0), grayLuma(sz, 49.0)) == 1.0);
    c.chk("kernelscale 覆盖率：尺寸不同立刻抛（两臂不是同一帧/同一尺寸）",
        throwsStd([&] {
            (void)shadowUmbraFraction(ref, cv::Mat(3, 3, CV_64FC1, cv::Scalar(1.0)));
        }));
}

/// ⑥ 取样层（`shadowScanPoint`）的闭式：合成图 + 合成机位，**不启引擎**。
/// 单独成函数的理由 = 认知复杂度护栏（合成进上一条 19 > 15）；缝也正当：上面测的是
/// 「剖面 → 读数」的算子，这里测的是「两张图 → 一条剖面」的取样。
void testShadowKernelScaleSampling(Checker& c)
{
    // 机位：(0, 9.5, 16.5) → 原点，fov 40°，画幅 320×180 ⟹ 地面 1 m ≈ 13 px，
    // 五条取样线（dv = 0/±1/±2 m）彼此相距 ≈13 px，互不污染。
    Engine::Camera cam;
    cam.position = {0.0, 9.5, 16.5};
    cam.target = {0.0, 0.0, 0.0};
    cam.fovDegrees = 40.0;
    const Projector proj = makeProjector(cam, 320, 180);
    ScanLine line;
    line.origin = {0.0, 0.0};
    line.u = {1.0, 0.0};
    line.v = {0.0, 1.0};
    line.lo = -1.0;
    line.hi = 1.0;
    line.step = 0.5;
    const cv::Mat refFlat = grayLuma(cv::Size(320, 180), 100.0);
    const cv::Mat armFlat = grayLuma(cv::Size(320, 180), 100.0);
    const ShadowScanSample same = shadowScanPoint(refFlat, armFlat, proj, line, 0.0);
    c.chk("kernelscale 取样：两图相同 → S 恰为 0（零点）", same.valid && same.value == 0.0);
    // 五条线里**只有一条**被涂黑（3×3 块盖住它的 2×2 双线性足迹）⟹ 平均后 S **恰为 1/5**。
    // 🔴 这条就是 kVLineOffsets 的存在性证明：把五线平均删掉（退回单线）这里读到的是 1.0。
    cv::Mat armOne = armFlat.clone();
    {
        double fx = 0.0, fy = 0.0;
        const bool ok = projectTo(proj, 0.0, 0.0, fx, fy);
        c.chk("kernelscale 取样：机位视线扫得到影心（合成机位不是随手取的）", ok,
            ok ? "" : "投影落在屏外");
        const int cx0 = static_cast<int>(std::lround(fx));
        const int cy0 = static_cast<int>(std::lround(fy));
        for (int y = cy0 - 1; y <= cy0 + 1; ++y)
            for (int x = cx0 - 1; x <= cx0 + 1; ++x)
                armOne.at<double>(y, x) = 0.0;
    }
    const ShadowScanSample one = shadowScanPoint(refFlat, armOne, proj, line, 0.0);
    c.chk("kernelscale 取样：五条线只有一条全黑 → S 恰为 1/5（沿 v 平均在起作用）",
        one.valid && std::abs(one.value - 0.2) < 1e-12, std::to_string(one.value));
    // 阳性锚点（反向）：**整条竖带**涂黑 = 真影缘 ⟹ 五条线一起读到 0 ⟹ S 恰为 1。
    // 平均不许把真的缘也摊掉；把 v 偏移放大到越出板宽（±8 m）就是靠这条变红。
    cv::Mat armBand = armFlat.clone();
    {
        double fx = 0.0, fy = 0.0;
        projectTo(proj, 0.0, 0.0, fx, fy);
        const int cx0 = static_cast<int>(std::lround(fx));
        for (int y = 0; y < armBand.rows; ++y)
            for (int x = cx0 - 1; x <= cx0 + 1; ++x)
                armBand.at<double>(y, x) = 0.0;
    }
    const ShadowScanSample band = shadowScanPoint(refFlat, armBand, proj, line, 0.0);
    c.chk("kernelscale 取样：整条竖带全黑（真影缘）→ S 恰为 1（平均不摊平缘）",
        band.valid && band.value == 1.0, std::to_string(band.value));
    // 投出屏的样本必须**作废**而不是夹到边缘像素（夹 = 把屏外读成边缘，剖面会凭空多出端点）。
    ScanLine far2 = line;
    far2.origin = {5000.0, 5000.0};
    const ShadowScanSample off = shadowScanPoint(refFlat, armFlat, proj, far2, 0.0);
    c.chk("kernelscale 取样：投出屏 → valid=false（不夹到边缘像素）", !off.valid);
}

/// `shadow-kernel-scale` 的**仪器面**（⑦ 合成 profile 的两个门 + ⑧ 判据挂号）。
/// 与算子面 `testShadowKernelScale` 分成两个函数的直接理由 = 认知复杂度护栏（合成 17 > 15）；
/// 缝本身也正当：算子面不碰磁盘、不启引擎，本面不碰算子。
void testShadowKernelScaleInstrument(Checker& c)
{
    // ⑦ 合成 profile：shadow.lua 整份覆写 + llm 截图门在**合成路径**上有实现。
    //    后者是本轮新开的线（旧守卫认为合成路径无实现 ⟹ 直接拒）；没有这组用例，
    //    「门开了却静默不生效」就只能在实机跑的时候才发现。
    const std::string tmp = "/tmp/imgqa_selftest_kernelscale";
    {
        ProfileOpts o;
        o.glb = "penumbra_ladder.glb";
        o.variant = "deferred";
        o.shadowLuaOverride = "return { mode = \"pcss\", izb = { on = false }, pcss = { sun_degrees = 5.0 } }\n";
        o.overrideLlmShot = true;
        o.llmShotFile = "screenshots/selftest_kernelscale.png";
        o.llmShotDelayFrames = 13;  // ≠ 模板默认 8，否则「覆写真的落上」这条测不到
        // 机位走 camera.lua（从第 1 帧生效）——本命令的投影必须知道相机在哪，
        // 而 RPC 补写的机位不进 camera.lua（假绿教训见 ProfileOpts::hasCamera 注释）。
        o.hasCamera = true;
        o.cameraPosition = { 1.0, 2.0, 3.0 };
        o.cameraTarget = { 0.0, 0.0, 0.0 };
        o.cameraFov = 40.0;
        softnessGen(c, tmp, o);
        std::ifstream sh(tmp + "/shadow.lua");
        const std::string shText{std::istreambuf_iterator<char>{sh}, {}};
        c.chk("kernelscale profile：shadow.lua 整份 = 臂定义（pcss 调参块在场）",
            shText.find("mode = \"pcss\"") != std::string::npos
                && shText.find("sun_degrees = 5.0") != std::string::npos,
            shText);
        std::ifstream rt(tmp + "/runtime.lua");
        const std::string rtText{std::istreambuf_iterator<char>{rt}, {}};
        std::ifstream dg(tmp + "/diagnostics.lua");
        const std::string dgText{std::istreambuf_iterator<char>{dg}, {}};
        c.chk("kernelscale profile：合成路径的 llm 截图门写对（帧号 = 量具的一部分）",
            rtText.find("llm_debug = true") != std::string::npos
                && rtText.find("delay_frames = 13") != std::string::npos
                && rtText.find("selftest_kernelscale.png") != std::string::npos);
        c.chk("kernelscale profile：合成路径同时关 rpc（llm 截图与 /shot 互斥）",
            dgText.find("rpc = false") != std::string::npos);
        // 🔴 覆写**不许把配置写坏**。这条是本轮实测付出来的：旧合成模板把 `llm_shot` 三个键
        // 挤在一行、`delay_frames = 8 } }` 收尾，值跨度按「到 , 或换行为止」算 ⟹ 覆写把尾部
        // ` } }` 一起吃掉，runtime.lua 变成 lua 语法错，引擎 status=1 退出（实测）。
        // 只查括号平衡与收尾，不查语义——语义错由引擎启动期喊，形状错本条喊。
        const auto braces = [](const std::string& t) {
            int open = 0;
            for (const char ch : t)
                open += ch == '{' ? 1 : (ch == '}' ? -1 : 0);
            return open;
        };
        c.chk("kernelscale profile：覆写后的 runtime.lua 括号平衡且以 } 收尾（覆写没吃掉收尾）",
            braces(rtText) == 0 && !rtText.empty()
                && rtText.find_last_not_of(" \t\n\r") == rtText.find_last_of('}'),
            "brace_delta=" + std::to_string(braces(rtText)));
        c.chk("kernelscale profile：机位写进 camera.lua（从第 1 帧生效，不是 RPC 补写）",
            [&] {
                std::ifstream cam(tmp + "/camera.lua");
                const std::string t{std::istreambuf_iterator<char>{cam}, {}};
                return t.find("position = { 1, 2, 3 }") != std::string::npos;
            }());
    }
    {
        // 门关态必须**逐位不变**：现有合成判据（temporal / aa-coverage / ao-noise…）都走
        // 这条路径，任何"顺手多写一个键"都会让它们的历史读数失效。
        ProfileOpts o;
        o.glb = "cornell_box.glb";
        softnessGen(c, tmp, o);
        std::ifstream rt(tmp + "/runtime.lua");
        const std::string rtText{std::istreambuf_iterator<char>{rt}, {}};
        std::ifstream sh(tmp + "/shadow.lua");
        const std::string shText{std::istreambuf_iterator<char>{sh}, {}};
        c.chk("kernelscale profile：门关 = 不写 llm 截图键（现有合成判据逐位不变）",
            rtText.find("llm_debug = false") != std::string::npos
                && rtText.find("unused.png") != std::string::npos);
        c.chk("kernelscale profile：shadowLuaOverride 为空时仍走 mode/izb 生成（不写空串）",
            shText.find("mode = \"none\"") != std::string::npos && shText.size() > 10,
            shText);
    }
    c.chk("kernelscale profile：overrideShadowRoute 在合成路径仍被拒（那条门确实无实现）",
        throwsStd([&] {
            ProfileOpts bad;
            bad.glb = "penumbra_ladder.glb";
            bad.overrideShadowRoute = true;
            bad.shadowRouteMode = "pcss";
            makeProfile(tmp, bad);
        }));

    // ⑦ 三条判决线必须在注册表里（改名而漏挂号 = verdictFor 运行期才炸，太晚）
    for (const char* name :
        { "shadow-csm-umbra-present", "shadow-csm-flat-in-gap", "shadow-target-resolves-gap" }) {
        bool found = false;
        for (const auto& j : registeredJudgments())
            found = found || std::string(j.name) == name;
        c.chk("kernelscale 判据已挂号进注册表", found, name);
    }
}

}  // namespace RGL::imgqa
