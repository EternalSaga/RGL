// 配置目录生成 + 引擎进程生命周期（httplib RPC）。
#include "imgqa.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <regex>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <httplib.h>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using nlohmann::json;

namespace RGL::imgqa {

const std::string& projectRoot()
{
    static const std::string root = [] {
        if (const char* env = std::getenv("RGL_ROOT"); env && *env)
            return std::string(env);
        return std::string(RGL_SOURCE_DIR);
    }();
    return root;
}

namespace {
std::string buildRglDir() { return projectRoot() + "/build/rgl"; }
std::string profileTemplate() { return projectRoot() + "/qa/configs/analytic"; }

std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到文件: " + p.string());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void writeFile(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

/**
 * 替换 lua 里**第一条以 `key` 开头的未注释行**的整行值（行尾注释一并丢掉——这是生成出来的
 * profile 副本，不是仓库配置，留着解释新值的旧注释更坏）。
 *
 * 为什么手写行扫描而不是 `std::regex`：`std::regex` 的 ECMAScript **没有 multiline**，`^` 只
 * 匹配整个输入的开头，`(?m)` 前缀会直接语法错。行首是空白但不允许是 `--`：camera.lua 里
 * 排着一整列注释掉的备选机位，那些必须原样留着。
 */
void subLuaLine(std::string& text, const std::string& key, const std::string& value,
    const std::string& where)
{
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    bool hit = false;
    while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (!hit && first != std::string::npos && line.compare(first, key.size(), key) == 0
            && line.substr(first, 2) != "--") {
            const bool comma = line.find(',') != std::string::npos;
            out << std::string(first, ' ') << key << " = " << value << (comma ? "," : "") << "\n";
            hit = true;
            continue;
        }
        out << line << "\n";
    }
    if (!hit)
        throw std::runtime_error(where + " 里没有未注释的 " + key + " 行");
    text = out.str();
}

/// 从**引擎解析器源码**抽「配置解析器认识的根级键」。
/// 不手抄白名单：手抄的会和解析器一起漂移，最后变成"看起来在检查其实什么都没检查"
/// ——那比不检查更糟。两种读取形态都要覆盖（实测各配置模块用的不一样，只覆盖一种会
/// **误报**合法键，而误报的检查很快被当噪音绕过）：
///   diagnostics_impl：readBool("no_vvl", def) ／ scene_config_impl：readBool(root, "spin", def)
std::set<std::string> knownRootKeys(const std::string& implRel)
{
    const std::string src = readFile(fs::path(projectRoot()) / implRel);
    // raw string 用 rx 定界符：模式内容里有 `)"`（捕获组收尾紧跟引号），
    // 裸 R"(...)" 会被提前终止（实测踩过）。
    static const std::regex reRoot(R"rx(root\[\s*"([a-z_]{2,26})"\s*\])rx");
    static const std::regex reRead(R"rx(\bread\w*\([^"]*"([a-z_]{2,26})")rx");
    static const std::regex reGet(R"rx(\b(?:get|opt|has|contains|named)\(\s*"([a-z_]{2,26})")rx");
    std::set<std::string> keys;
    for (auto* re : {&reRoot, &reRead, &reGet})
        for (std::sregex_iterator it(src.begin(), src.end(), *re), e; it != e; ++it)
            keys.insert((*it)[1].str());
    return keys;
}

/// 写出的键必须在解析器里认得。**拼错键 = 静默用默认值**，是最坏的一类失效：
/// 配置看起来写了、引擎看起来读了、判据看起来在跑，而它跑在默认值上。
/// （真实事故：`shadowPass` 是 C++ 字段名，lua 键实为 `shadow_pass`。）
void assertKeys(const std::string& fileName, const std::set<std::string>& emitted,
    const std::set<std::string>& known)
{
    std::string unknown;
    for (const auto& k : emitted)
        if (!known.count(k))
            unknown += (unknown.empty() ? "" : ", ") + k;
    if (!unknown.empty())
        throw std::runtime_error(fileName
            + " 写出了配置解析器不认识的键: " + unknown
            + "\n继续跑就等于在**默认值**上下判据——拒绝。");
}

const char* lb(bool v) { return v ? "true" : "false"; }

/// 交付配置模式：把 `key = <旧值>,` 一行的**值**换成新值（只认第一处，且必须找到）。
/// 找不到就抛——静默不替换 = 判据跑在**交付配置的原值**上而工具以为改了，
/// 与「拼错键 = 静默用默认值」同罪（makeProfile 头注释）。
void replaceValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newValue)
{
    const std::string head = key + " = ";
    const auto at = text.find(head);
    if (at == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 里没有键 `" + key
            + "`——覆写失败，拒绝在旧值上下判据");
    const auto eol = text.find_first_of(",\n", at + head.size());
    if (eol == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 的键 `" + key + "` 形状不认识");
    text.replace(at + head.size(), eol - (at + head.size()), newValue);
}

/// 交付配置模式：把表值键 `key = { a, b }` 整体改写为 `key = { newContent }`。
/// 为什么不能复用 `replaceValue`：它按第一个 `,` 或换行收尾，而表值**内部就有逗号**
/// （`headless_size = { 1280, 720 }` ⟹ 截在 1280 后 = 留下 `720 },` = Lua 语法错）。
/// ⚠ `newContent` 是**花括号内部**的内容（不含花括号）——上一版把开括号混进参数、函数
/// 又补一次，写出 `headless_size = { { 2560, 1440 } }`：语法合法（嵌套表）但 loader 把
/// [1] 当数字读 = ConfigLoadFailed，REF 超采样组整组引擎启动即退（本轮缺陷 B 的根因）。
/// 形状自检（写出后必须匹配 `key = { <num>, <num> }`）就钉住这一类「语法合法、语义是
/// 嵌套表」的失效——只测「正则命中数」抓不到它，命中了恰恰说明写坏了。
void replaceTableValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newContent)
{
    const std::string head = key + " = {";
    const auto at = text.find(head);
    if (at == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 里没有表值键 `" + key
            + "`——覆写失败，拒绝在旧值上下判据");
    const auto close = text.find('}', at + head.size());
    if (close == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 的表值键 `" + key + "` 没有收尾");
    text.replace(at + head.size(), close - (at + head.size()) + 1,
        " " + newContent + " }");
    // 写出后的形状自检：扁平数值表 `{ 数字, 数字 }`，**不许**出现嵌套花括号。
    const std::regex flatRx(key + R"aaa(\s*=\s*\{\s*-?[0-9]+(\.[0-9]+)?\s*,\s*-?[0-9]+(\.[0-9]+)?\s*\})aaa");
    if (!std::regex_search(text, flatRx))
        throw std::runtime_error("交付配置 " + file + " 的 `" + key
            + "` 改写后不是扁平数值表（疑似嵌套花括号 = 引擎 loader 会把表当数字读 = "
              "ConfigLoadFailed）。改写前的原文片段: "
            + text.substr(at, std::min<size_t>(64, text.size() - at)));
}

/// 交付配置模式：在**第一条未注释的 `key = ...` 行**之后插入若干行（不碰原行）。
/// 与 `subLuaLine` 的区别 = 原行要留着（`spin = false` 是交付配置的既有键，本工具只
/// 追加同级的诊断键）；与 `rewriteTaaBlock` 的「插到 `return {` 之后」的区别 =
/// 插到兄弟键旁边，读生成出来的 profile 时键的归属一眼可见。
void insertAfterKey(std::string& text, const std::string& file, const std::string& key,
    const std::string& block)
{
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    bool hit = false;
    while (std::getline(in, line)) {
        out << line << "\n";
        const auto first = line.find_first_not_of(" \t");
        if (!hit && first != std::string::npos && line.compare(first, key.size(), key) == 0
            && line.substr(first, 2) != "--"
            // 键名边界：`spin` 不得命中同前缀的兄弟键（scene.lua 里就有 `spin_frame_angle`，
            // 插到它后面 = 插到自己刚写的那两行之后，第二次调用直接跑偏）。
            && (first + key.size() >= line.size()
                || !(std::isalnum(static_cast<unsigned char>(line[first + key.size()]))
                    || line[first + key.size()] == '_'))) {
            out << block;
            hit = true;
        }
    }
    if (!hit)
        throw std::runtime_error("交付配置 " + file + " 里没有未注释的 " + key
            + " 行——诊断键无处可插，拒绝在缺键的配置上下判据");
    text = out.str();
}

/// 交付配置模式：整段替换 `scene.lua` 的 `taa = { ... },` 块为显式三键。
/// 缺块就在 `return {` 之后插一段——**不静默跳过**（跳过 = 判据跑在「TAA 关」的配置上，
/// 恒等式会因「pass 根本不在图里」而平凡成立 = 最阴的假绿）。
/// staticBoost/staticFeedback（批 13.7）：默认关 = 不写这两键（写出的键越少，漂移面越小）。
void rewriteTaaBlock(std::string& scene, bool enabled, double feedback, double jitterScale,
    bool baseSpace, bool staticBoost = false, double staticFeedback = 0.01,
    bool motionCompensation = true, bool reprojection = true, bool diagReject = false)
{
    std::ostringstream blk;
    blk << "    taa = {\n        enabled = " << lb(enabled) << ",\n        feedback = "
        << std::setprecision(6) << feedback << ",\n        jitter_scale = " << jitterScale
        << ",\n        base_space = " << lb(baseSpace);
    if (staticBoost)
        blk << ",\n        static_boost = true,\n        static_feedback = " << staticFeedback;
    // 消融/诊断开关：默认态 = 不写键（键越少漂移面越小）。
    // motion_compensation 只关 prevSpin 场景旋转补偿（spin 模式 M3 用）；reprojection 关的是
    // prevViewProj 相机重投影本身（相机模式 M3 用）——两者在相机运动下**不等价**，
    // 语义 → rgl/scene_config.cppm 字段注释。
    if (!motionCompensation)
        blk << ",\n        motion_compensation = false";
    if (!reprojection)
        blk << ",\n        reprojection = false";
    if (diagReject)
        blk << ",\n        diag_reject = true";
    blk << ",\n    },\n";
    const auto begin = scene.find("\n    taa = {");
    if (begin != std::string::npos) {
        // 块收尾是 `\n    }` 再加**可选逗号**（交付配置里 taa 是 return 表最后一项，带逗号）。
        // 逗号必须一起吃掉——留一个孤零零的 `,` 在 `}` 前 = Lua 语法错（实测踩过：
        // `scene.lua:235: unexpected symbol near ','`）。
        const auto end = scene.find("\n    }", begin);
        if (end == std::string::npos)
            throw std::runtime_error("交付配置 scene.lua 的 taa 块没有收尾，无法覆写");
        std::string::size_type len = 6;  // == std::string_view("\n    }").size()
        if (end + len < scene.size() && scene[end + len] == ',')
            ++len;
        scene.replace(begin, end - begin + len, "\n" + blk.str());
        return;
    }
    const auto head = scene.find("return {");
    if (head == std::string::npos)
        throw std::runtime_error("交付配置 scene.lua 不是 `return {` 形状，无法注入 taa 块");
    scene.insert(head + 8, "\n" + blk.str());
}

/// D2（批 12.3d）：SPV 新鲜度守卫（mtime 轻版，不核内容）。**所有引擎驱动路径的
/// 统一护**（Engine 构造 + runEngineOnce 起点都调）——"改了 .slang 没构建就测量"
/// 与并行实例污染同级（探针在旧着色器上下判据 = 最贵的假线索）。
///
/// 口径：**逐 spv** 只比"它自己的源闭包"（清单 build/rgl/shader_deps.txt，由每次
/// 构建的 shader_dep_check 目标生成，见 tools/check_shader_deps.py）。刻意不是
/// "最新 .slang vs 最旧 .spv" 的全局比较——增量构建只重编受影响 spv，无关 spv
/// （如 light.mesh.spv）必然比刚改的 .slang 旧，全局口径会让每次增量构建后恒红。
/// 边界：.spv 目录不存在/为空 = 从未构建 → 跳过（后续引擎启动失败兜底）；清单缺失
/// 但 spv 在场 = 构建状态与守卫不同步 → **报错不出声**（失效静默会被读成"新鲜度已检查"）。
void checkShadersFresh()
{
    const fs::path spvDir = fs::path(buildRglDir()) / "generated_shaders";
    std::error_code ec;
    bool anySpv = false;
    for (const auto& e : fs::directory_iterator(spvDir, ec))
        if (!ec && e.is_regular_file(ec) && e.path().extension() == ".spv") {
            anySpv = true;
            break;
        }
    if (!anySpv)
        return;

    const fs::path manifest = fs::path(buildRglDir()) / "shader_deps.txt";
    if (!fs::exists(manifest))
        throw std::runtime_error(
            "SPV 新鲜度无法判定：build/rgl/shader_deps.txt 缺失（正常构建随 shader_dep_check "
            "目标生成）。先 just build 重新生成后重跑。");

    std::ifstream in(manifest);
    if (!in)
        throw std::runtime_error("读不到 " + manifest.string());
    std::string stale;
    int shown = 0;
    for (std::string line; std::getline(in, line);) {
        if (line.empty() || line[0] == '#')
            continue;
        std::istringstream cols(line);
        std::string spvName;
        cols >> spvName;
        const fs::path spv = spvDir / spvName;
        const auto spvT = fs::last_write_time(spv, ec);
        if (ec)
            continue;  // 该 spv 未构建（如 target 新加未编译）：引擎启动失败兜底
        for (std::string depRel; cols >> depRel;) {
            const auto depT = fs::last_write_time(fs::path(projectRoot()) / depRel, ec);
            if (ec || depT <= spvT)
                continue;
            if (++shown <= 5) {
                if (shown > 1)
                    stale += "\n";
                stale += "  " + depRel + " 新于 " + spvName;
            }
        }
    }
    if (shown) {
        if (shown > 5)
            stale += "\n  …（共 " + std::to_string(shown) + " 对）";
        throw std::runtime_error(
            "SPV 疑似过期（测量会跑在旧着色器上）：\n" + stale
            + "\n  修复：just build（重编 SPV；改 shader 契约/schema 再 just reflect）后重跑");
    }
}
}  // namespace

/**
 * 机位 + 帧驱动 yaw 写进 camera.lua（**两条路径共用一份实现**）。
 *
 * 为什么必须共用：此前只有交付分支写，合成 profile 根本不产出 camera.lua
 * ⟹ `ProfileOpts::hasCamera` 在合成路径上**静默失效**——`temporal.cpp` 的注释一直声称
 * 「机位同时给两处」，实际只给了 RPC 一处，而 hasCamera 那条注释记的正是「RPC 之前的帧
 * 已经进了 TAA 历史」这条假绿教训。写出来的键仍然只在门开时出现（不写 = 引擎默认 = 逐位今天）。
 */
void applyCameraLua(const std::string& outDir, const ProfileOpts& o)
{
    if (!o.hasCamera && o.cameraFrameYaw == 0.0)
        return;  // 两边都没要求 ⟹ 不产出 camera.lua（保持既有合成判据的逐位行为）
    const std::string path = outDir + "/camera.lua";
    std::string cam = readFile(fs::exists(path) ? path : profileTemplate() + "/camera.lua");
    if (o.hasCamera) {
        const auto vec = [](const std::array<double, 3>& a) {
            std::ostringstream s;
            s.precision(9);
            s << "{ " << a[0] << ", " << a[1] << ", " << a[2] << " }";
            return s.str();
        };
        std::ostringstream fov;
        fov.precision(9);
        fov << o.cameraFov;
        subLuaLine(cam, "position", vec(o.cameraPosition), "camera.lua");
        subLuaLine(cam, "target", vec(o.cameraTarget), "camera.lua");
        subLuaLine(cam, "fovDegrees", fov.str(), "camera.lua");
    }
    if (o.cameraFrameYaw != 0.0) {
        // 插到 `mode` 行旁（两份 camera.lua 里 frame_yaw 都只有注释行，subLuaLine 找不到实体行）。
        // 键名拼错在 lua 侧是**静默**的，消费方的「运动激励在场性」守卫兜底。
        std::ostringstream yaw;
        yaw << std::setprecision(9) << "    frame_yaw = " << o.cameraFrameYaw << ",\n";
        insertAfterKey(cam, "camera.lua", "mode", yaw.str());
    }
    writeFile(path, cam);
}

std::string makeProfile(const std::string& outDir, const ProfileOpts& o)
{
    fs::create_directories(outDir);
    // ── 交付配置模式（批 13.1e）：基底 = 仓库 configs/，只覆写 headless / rpc / taa ──
    // 判据必须在**用户实际跑的那份配置**上求值（ProfileOpts::deliveryConfigDir 的注释里
    // 记着这条是怎么用一轮假绿换来的）。刻意**不**在这里重抄 env/ao/shadow/light：
    // 那些键的真相在交付配置里，工具再抄一份就是第二份真相，会漂移。
    if (!o.deliveryConfigDir.empty()) {
        std::error_code ec;
        int copied = 0;
        for (fs::directory_iterator it(o.deliveryConfigDir, ec), e; it != e; it.increment(ec)) {
            if (!it->is_regular_file(ec))
                continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".lua" && ext != ".json")
                continue;
            fs::copy_file(it->path(), fs::path(outDir) / it->path().filename(),
                fs::copy_options::overwrite_existing);
            ++copied;
        }
        if (copied == 0)
            throw std::runtime_error("交付配置目录里没有 .lua/.json: " + o.deliveryConfigDir);

        std::string rt = readFile(outDir + "/runtime.lua");
        replaceValue(rt, "runtime.lua", "headless", "true");
        replaceValue(rt, "runtime.lua", "llm_debug", "false");  // RPC 形态与 llm 截图互斥
        // 尺寸覆写是**可选**的（默认关：现有判据全部钉 1280×720，隐式改尺寸 = 换口径）。
        // 表值必须走 replaceTableValue，理由见其注释（replaceValue 会截在花括号里的逗号；
        // 而参数**不含花括号**——含了就写出嵌套表，缺陷 B 的根因，函数内有形状自检）。
        if (o.overrideHeadlessSize)
            replaceTableValue(rt, "runtime.lua", "headless_size",
                std::to_string(o.size[0]) + ", " + std::to_string(o.size[1]));
        writeFile(outDir + "/runtime.lua", rt);

        std::string dg = readFile(outDir + "/diagnostics.lua");
        replaceValue(dg, "diagnostics.lua", "rpc", "true");
        replaceValue(dg, "diagnostics.lua", "rpc_port", std::to_string(8788));
        writeFile(outDir + "/diagnostics.lua", dg);

        std::string sc = readFile(outDir + "/scene.lua");
        rewriteTaaBlock(sc, o.taa, o.taaFeedback, o.taaJitterScale, o.taaBaseSpace,
            o.taaStaticBoost, o.taaStaticFeedback, o.taaMotionCompensation, o.taaReprojection,
            o.taaDiagReject);
        // 帧驱动 spin（诊断激励门）。**只在门开时写**：不写 = 交付配置原样 = 逐位今天。
        // 键名拼错在这里是**静默**的（scene.lua 的未知键引擎不打警告），所以消费方
        // temporal-motion 另有在场性守卫：运动组的连拍若逐位相同 = 键没生效 = 立刻停下。
        if (o.spinFrameAngle != 0.0) {
            std::ostringstream spin;
            spin << std::setprecision(9)
                 << "    spin_frame_angle = " << o.spinFrameAngle << ",\n"
                 << "    spin_axis = { " << o.spinAxis[0] << ", " << o.spinAxis[1] << ", "
                 << o.spinAxis[2] << " },\n";
            insertAfterKey(sc, "scene.lua", "spin", spin.str());
        }
        writeFile(outDir + "/scene.lua", sc);

        // variant 覆写是**可选**的：留空 = 尊重交付配置自己写的路线（交付默认 deferred）。
        if (!o.variant.empty()) {
            std::string rg = readFile(outDir + "/render_graph.lua");
            static const std::regex variantRx(R"(variant\s*=\s*"[^"]*")");
            if (!std::regex_search(rg, variantRx))
                throw std::runtime_error("交付配置 render_graph.lua 无 variant 行，无法覆写");
            writeFile(outDir + "/render_graph.lua",
                std::regex_replace(rg, variantRx, "variant = \"" + o.variant + "\""));
        }
        // 机位写进 camera.lua（**从第 1 帧生效**，理由见 ProfileOpts::hasCamera 的注释）
        applyCameraLua(outDir, o);
        return outDir;
    }
    // render_graph.lua 定义图本身，**必须整份拷**（只给 {variant=...} 得 0 个
    // swapchain 附件 → 启动失败，实测踩过）。
    fs::copy_file(profileTemplate() + "/render_graph.lua", outDir + "/render_graph.lua",
        fs::copy_options::overwrite_existing);
    // variant 重写（izb 判据要 deferred；模板默认 forward）：只动 variant 行的字符串值
    if (!o.variant.empty()) {
        std::ifstream in(outDir + "/render_graph.lua");
        std::string rg((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        static const std::regex variantRx(R"(variant\s*=\s*"[^"]*")");
        if (!std::regex_search(rg, variantRx))
            throw std::runtime_error("render_graph.lua 模板无 variant 行，无法重写");
        const std::string out = std::regex_replace(rg, variantRx, "variant = \"" + o.variant + "\"");
        writeFile(outDir + "/render_graph.lua", out);
    }
    for (const char* j : {"log_level.json", "shaderRefLogLevel.json"})
        fs::copy_file(profileTemplate() + "/" + j, outDir + "/" + j,
            fs::copy_options::overwrite_existing);

    writeFile(outDir + "/runtime.lua",
        "return { headless = true, headless_size = { " + std::to_string(o.size[0]) + ", "
            + std::to_string(o.size[1]) + " },\n"
          "  win_size = { 800, 600 }, show_after = -1, present_mode = \"immediate\",\n"
          "  ui_font = \"\", ui_font_size = 18.0,\n"
          "  llm_debug = false, llm_shot = { file = \"screenshots/unused.png\", size = { "
            + std::to_string(o.size[0]) + ", " + std::to_string(o.size[1])
            + " }, delay_frames = 8 } }\n");

    const std::set<std::string> diagKeys = {"no_vvl", "no_hdr", "no_present", "vtx_canary",
        "texel_canary", "dump_gbuf", "style_dist", "tex_dist", "texture_force_size",
        "texture_force_white", "texture_force_unorm", "profiler", "imgui", "config_reload",
        "shader_reload", "picking", "rpc", "rpc_port", "shadow_pass", "izb_shadow",
        "shadow_evssm", "izb_grid", "izb_dump_frame"};
    assertKeys("diagnostics.lua", diagKeys, knownRootKeys("rgl/diagnostics_impl.cppm"));
    writeFile(outDir + "/diagnostics.lua",
        std::string(
            "return { no_vvl = false, no_hdr = false, no_present = false, vtx_canary = false,\n"
            "  texel_canary = -1, dump_gbuf = -1, style_dist = false, tex_dist = false,\n"
            "  texture_force_size = 0, texture_force_white = false, texture_force_unorm = false,\n"
            "  profiler = 0, imgui = false, config_reload = false, shader_reload = false,\n"
            "  picking = ")
        + lb(o.picking) + ", rpc = true, rpc_port = 8788,\n  shadow_pass = " + lb(o.shadowPass)
        + ", izb_shadow = " + lb(o.izbShadow)
        + ", shadow_evssm = " + lb(o.evssmShadow)
        + ", izb_dump_frame = " + std::to_string(o.izbDumpFrame) + " }\n");

    std::set<std::string> sceneKeys
        = {"entities", "styles", "material_styles", "deferred_styles", "environment", "light",
           "spin", "cube", "exposure", "ao"};
    if (o.taa)
        sceneKeys.insert("taa");  // 只在真要开的时候写这段（缺段 = 全默认即「关」）
    assertKeys("scene.lua", sceneKeys, knownRootKeys("rgl/scene_config_impl.cppm"));
    const auto j3 = [](std::array<double, 3> a) {
        return std::to_string(a[0]) + ", " + std::to_string(a[1]) + ", " + std::to_string(a[2]);
    };
    // entities：默认一份 glb；entitiesLua 非空 = 整条由调用侧给（内建合成靶 aa_target
    // 没有 glb 文件）。拼错 type 不会静默——parseEntityItem 直接启动报错列合法值。
    const std::string entities = o.entitiesLua.empty()
        ? "    entities = { { type = \"model\", file = \"models/" + o.glb + "\" } },\n"
        : "    entities = { " + o.entitiesLua + " },\n";
    // TAA 段（批 12.6）：enabled + feedback + jitter_scale（+ 批 13.2 base_space，
    // + 批 13.7 static_boost/static_feedback——仅置位时写，键越少漂移面越小），
    // 其余走引擎默认（运动补偿开）——写出来的键越少，漂移面越小。两个消融开关各有用途：
    // jitter_scale=0 = 只留时域累积；feedback=1 = 只抖不滤（输出 = 本帧，temporal 的阳性
    // 锚点组。别写 0——0 是「只要裁剪后的历史、完全丢本帧」的退化反馈环，测出来的不是
    // 闪烁本体）。
    const std::string taa = o.taa
        ? "    taa = { enabled = true, feedback = " + std::to_string(o.taaFeedback)
              + ", jitter_scale = " + std::to_string(o.taaJitterScale)
              + ", base_space = " + lb(o.taaBaseSpace)
              + (o.taaStaticBoost
                     ? ", static_boost = true, static_feedback = "
                           + std::to_string(o.taaStaticFeedback)
                     : "")
              + " },\n"
        : "";
    // styles 必须显式写（switch 首战抓到的静默坑）：缺表回落默认 pbr→model_pbr，
    // 而 model_pbr.frag **不 import shadow/env**——shadow_pass 与 env 开关全部
    // 退化为逐位相同的假象（投影侧照画、采样侧不存在）。model_pbr_ibl 在
    // env=0/shadow=off 下直接光与 model_pbr 逐式相同（IBL 项 ×intensity=0 归零），
    // 闭式解前提不破。
    // 表 = **交付配置 configs/scene.lua 的全量四风格 + 材质名连连看**（13.1b 补：此前只写
    // unlit/pbr 两条，bistro 的 `light`/`vegetation` 材质直接启动失败
    // "style not in scene.lua styles: light"；缺 material_styles 则 Emissive/Foliage
    // 全落 pbr = 测的不是交付路径）。对无这些材质的探针场景（cornell_box / aa_target）
    // 逐位 inert——连连看按材质名命中，探针场景无命中项。
    const std::string styles =
        "    styles = { unlit = \"model\", pbr = \"model_pbr_ibl\", vegetation = "
        "\"model_vegetation\", light = \"model_light\" },\n"
        "    material_styles = { { pattern = \"Emissive\", style = \"light\" },\n"
        "      { pattern = \"Foliage\", style = \"vegetation\" } },\n"
        "    deferred_styles = { vegetation = \"deferred_geometry_vegetation\",\n"
        "      light = \"deferred_geometry_light\" },\n";
    writeFile(outDir + "/scene.lua",
        "return {\n" + entities
          + styles
        // 环境（intensity / skybox 由 ProfileOpts 给；env 文件与交付配置同一张 .hdr）
          + "    environment = { hdr = \"env/san_giuseppe_bridge_4k.hdr\", hdr_size = 2048,\n"
        "      intensity = "
            + std::to_string(o.envIntensity) + ", skybox = " + lb(o.skybox) + " },\n"
          "    light = { direction = { "
            + j3(o.lightDir) + " }, color = { 1.0, 1.0, 1.0 },\n      intensity = "
            + std::to_string(o.lightIntensity) + ", ambient = { " + j3(o.ambient) + " } },\n"
            + "    spin = false, cube = false, exposure = " + std::to_string(o.exposure)
            + ",\n    ao = { strength = " + std::to_string(o.aoStrength) + " },\n"
            + taa + "}\n");
    applyCameraLua(outDir, o);  // 合成路径同样支持机位/帧驱动 yaw（见 applyCameraLua 注释）
    return outDir;
}

// ── Engine ─────────────────────────────────────────────────────────────────

struct Engine::Impl {
    std::string profileDir;
    int port;
    pid_t pid = -1;
    std::string logPath;
    std::unique_ptr<httplib::Client> http;

    void killTree()
    {
        if (pid <= 0)
            return;
        ::kill(pid, SIGTERM);
        int st = 0;
        ::waitpid(pid, &st, 0);
        pid = -1;
    }
};

namespace {
/**
 * 引擎日志尾部（「配置写出来必须能被引擎接受」这条自检的**取证**部分）。
 *
 * 判据本体在 Engine 构造：RPC 就绪 = 引擎走完了全部配置加载（ConfigLoadFailed 抛在
 * RPC 挂起之前）；RPC 永不就绪 / 进程提前退出 = 配置**没被接受**。此时最贵的失效是
 * 拿着"超时"或 `strerror(status)`（对 wait status 用 strerror = 驴唇不对马嘴）去排查，
 * 而引擎早就把 `[error] ... ConfigLoadFailed ...` 写进了日志——把尾部原样带进异常，
 * 失效当场可读（本轮缺陷 B：REF 组 runtime.lua 被写成嵌套表，报错只有半行截断路径）。
 */
std::string logTail(const std::string& path, int lines = 12)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return "(日志读不到: " + path + ")";
    std::vector<std::string> ring(static_cast<size_t>(lines));
    std::string line;
    long n = 0;
    while (std::getline(in, line)) {
        ring[static_cast<size_t>(n % lines)] = line;
        ++n;
    }
    std::ostringstream out;
    if (n == 0)
        return "(日志为空)";
    const long start = n > lines ? n : 0;
    for (long i = start; i < n; ++i)
        out << "\n    | " << ring[static_cast<size_t>(i % lines)];
    return out.str();
}
}  // namespace

Engine::Engine(std::string profileDir, int port) : impl_(new Impl{.profileDir = profileDir, .port = port})
{
    // 新鲜度守卫先于一切（所有引擎驱动路径统一受护，见 checkShadersFresh 注释）。
    checkShadersFresh();
    // 单实例纪律：pgrep 用 returncode 判存在。**不要**拿 `pgrep -c` 的 stdout 判真假
    // ——没找到时它打印 "0"，非空 → 误判成"有残留"而永远拒跑（本轮真实踩过）。
    if (std::system("pgrep -x rgl >/dev/null 2>&1") == 0)
        throw std::runtime_error(
            "已有 rgl 进程在跑。imgqa 要求单实例——并行实例会互相遮挡合成，同时扭曲画面与帧率");

    const std::string bin = buildRglDir() + "/rgl";
    impl_->logPath = buildRglDir() + "/imgqa_run.log";
    std::fflush(stdout);  // fork 前 flush，理由见 runEngineOnce 同处注释
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid < 0)
        throw std::runtime_error("fork 失败");
    if (pid == 0) {
        // 子进程：CWD 必须是 build/rgl（相对路径读 configs/ 与 generated_shaders/）。
        if (::chdir(buildRglDir().c_str()) != 0)
            _exit(120);
        if (freopen(impl_->logPath.c_str(), "wb", stdout) == nullptr)
            _exit(121);
        dup2(fileno(stdout), fileno(stderr));
        setenv("RGL_CONFIG_DIR", impl_->profileDir.c_str(), 1);
        execl(bin.c_str(), "rgl", static_cast<char*>(nullptr));
        _exit(127);
    }
    impl_->pid = pid;
    impl_->http = std::make_unique<httplib::Client>("127.0.0.1", port);
    impl_->http->set_connection_timeout(2, 0);
    impl_->http->set_read_timeout(20, 0);

    // 等 RPC 就绪（引擎启动要过 shader 反射/cubemap 卷积，数秒级；期间进程可能因配置
    // 错误退出——那要立刻报错并把日志路径给出，否则排查者对着超时猜）。
    //
    // **「配置写出来必须能被引擎接受」的自检本体**：RPC 就绪 ⟺ 引擎走完了全部配置加载
    // （ConfigLoadFailed 抛在 RPC 挂起之前）⟹ 本函数返回 = 写出的每个 profile 都被引擎
    // 接受；失败 = 配置没被接受，异常必须带日志尾部（logTail 注释：缺陷 B 的教训——
    // 「只测正则命中数」的自检恰恰放过了「语法合法但语义是嵌套表」的写坏形态）。
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            impl_->pid = -1;
            throw std::runtime_error("引擎提前退出（exit="
                + std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1) + ", signal="
                + std::to_string(WIFSIGNALED(st) ? WTERMSIG(st) : 0)
                + "）= 写出的配置没被引擎接受，见 " + impl_->logPath + logTail(impl_->logPath));
        }
        if (impl_->http->Get("/"))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    impl_->killTree();
    throw std::runtime_error("RPC 端口 " + std::to_string(port) + " 60s 未就绪，见 "
        + impl_->logPath + logTail(impl_->logPath));
}

void Engine::setCamera(const Camera& cam)
{
    // fps 模式下引擎先读 position 再读 target 反推朝向（引擎实现约定）。
    const json body = {{"mode", "fps"}, {"position", cam.position},
        {"target", cam.target}, {"fovDegrees", cam.fovDegrees}};
    auto res = impl_->http->Post("/camera", body.dump(), "application/json");
    if (!res || res->status != 200)
        throw std::runtime_error("POST /camera 失败: "
            + (res ? std::to_string(res->status) : "无响应"));
}

void Engine::setParam(const std::string& page, const std::string& label, double value)
{
    json body{{"label", label}, {"value", value}};
    if (!page.empty())
        body["page"] = page;
    auto res = impl_->http->Post("/param", body.dump(), "application/json");
    if (!res)
        throw std::runtime_error("POST /param 无响应");
    if (res->status != 200)
        throw std::runtime_error("POST /param 失败 " + std::to_string(res->status) + " label="
            + label + " page=" + page + ": " + res->body);
}

double Engine::paramValue(const std::string& page, const std::string& label)
{
    auto res = impl_->http->Get("/state");
    if (!res || res->status != 200)
        throw std::runtime_error(
            "GET /state 失败: " + (res ? std::to_string(res->status) : std::string("无响应")));
    const json out = json::parse(res->body);
    for (const auto& p : out.at("pages")) {
        if (!page.empty() && p.value("name", "") != page)
            continue;
        for (const auto& e : p.at("entries"))
            if (e.value("label", "") == label && e.contains("value")
                && e["value"].is_number())
                return e["value"].get<double>();
    }
    // 找不到 = 滑杆被摘了 / label 改了名。宁可抛，也不要让调用侧拿到「默认值」当成回显。
    throw std::runtime_error("GET /state 里没有数值滑杆: label=" + label
        + " page=" + (page.empty() ? "<全页>" : page) + "（label 真相源 = 引擎生成的参数语义表）");
}

std::string Engine::shot(const std::string& tag, const char* attachment)
{
    json body{{"file", "screenshots/imgqa_" + tag + ".png"}};
    if (attachment)
        body["attachment"] = attachment;
    auto res = impl_->http->Post("/shot", body.dump(), "application/json");
    if (!res)
        throw std::runtime_error("POST /shot 无响应");
    if (res->status != 200)
        throw std::runtime_error("POST /shot 失败 " + std::to_string(res->status) + ": "
            + res->body);
    const json out = json::parse(res->body);
    // 引擎跨帧结算后给**绝对路径**；若哪天变回相对，报错而不是静默按相对 CWD 找。
    fs::path p = out.at("file").get<std::string>();
    if (!fs::exists(p))
        throw std::runtime_error("/shot 返回的路径不存在: " + p.string());
    return p.string();
}

uint32_t Engine::frameCount()
{
    auto res = impl_->http->Get("/");
    if (!res || res->status != 200)
        throw std::runtime_error("GET / 失败: " + (res ? std::to_string(res->status) : "无响应"));
    const json out = json::parse(res->body);
    return out.at("frame").get<uint32_t>();
}

void Engine::advanceFrames(uint32_t n, int timeoutSec)
{
    // 目标 = 当前计数 + n。用**相对**目标而不是绝对帧号：RPC 登记落在第几帧由启动时序决定
    // （`temporal.cpp` 里 φ 每次启动不等的同一原因），绝对号会让热身长度本身带噪声。
    const uint32_t target = frameCount() + n;
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    uint32_t last = 0;
    int stalled = 0;
    while (true) {
        const uint32_t now = frameCount();
        if (now >= target)
            return;
        // 帧计数不推进 = 引擎卡死（不是"慢"）。静默少热身比报错贵得多：量到的是历史瞬态，
        // 而读数看起来完全正常（画面被冻在初始相位 ⟹ 残差反而更小 = 假绿）。
        stalled = now == last ? stalled + 1 : 0;
        last = now;
        if (stalled > 200)
            throw std::runtime_error("帧计数停在 " + std::to_string(now) + "（引擎卡死？），"
                + "热身 " + std::to_string(n) + " 帧未完成——拒绝在瞬态上下判据");
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("热身超时：目标帧 " + std::to_string(target) + " 停在 "
                + std::to_string(now));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

std::vector<std::string> Engine::shotBurst(const std::string& dir, uint32_t n,
    const std::string& prefix, const std::string& attachment)
{
    json body{{"dir", dir}, {"n", static_cast<int>(n)}, {"prefix", prefix}};
    if (!attachment.empty())
        body["attachment"] = attachment;  // 具名附件连拍（temporal-motion 的 taa_out alpha 掩膜）
    // **读超时按张数放大**：/shotburst 的响应在最后一张写完才兑现 ⟹ 响应时间 = N×(帧时间 +
    // PNG 编码)。2560×1440 的 REF 组单张编码实测数百毫秒 ⟹ 32 张远超客户端默认 20s，
    // 症状是「POST /shotburst 无响应」（其实是客户端先挂断，引擎随后正常返回）。
    // 预算 8s/张（≈ 默认 20s 的 12 倍余量），上限 10 分钟；其它端点保持 20s（它们慢 = 真卡死）。
    impl_->http->set_read_timeout(std::min(600, std::max(20, static_cast<int>(n) * 8)), 0);
    auto res = impl_->http->Post("/shotburst", body.dump(), "application/json");
    impl_->http->set_read_timeout(20, 0);
    if (!res)
        throw std::runtime_error(
            "POST /shotburst 无响应（引擎没起来，或用的是没有该端点的旧构建）");
    if (res->status != 200)
        throw std::runtime_error(
            "POST /shotburst 失败 " + std::to_string(res->status) + ": " + res->body);
    const json out = json::parse(res->body);
    auto files = out.at("files").get<std::vector<std::string>>();
    // 少交一张就是采集不完整——宁可抛，也不要拿缺帧的序列去算相邻帧差（那会把
    // 「跨了两帧」悄悄当成「跨了一帧」，正是本端点要修的那个 bug 的翻版）。
    if (files.size() != n)
        throw std::runtime_error("/shotburst 只交了 " + std::to_string(files.size()) + "/"
            + std::to_string(n) + " 张");
    for (const auto& f : files)
        if (!fs::exists(f))
            throw std::runtime_error("/shotburst 返回的路径不存在: " + f);
    return files;
}

Engine::~Engine()
{
    try {
        // /quit = 与用户关窗同一条干净退出路径（拿得到 profiler 收口；SIGTERM 会打断
        // present/in-flight）。失败才 SIGTERM 兜底。
        impl_->http->Post("/quit", "{}", "application/json");
        int st = 0;
        for (int i = 0; i < 80; ++i) {
            if (waitpid(impl_->pid, &st, WNOHANG) == impl_->pid) {
                impl_->pid = -1;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } catch (...) {
    }
    impl_->killTree();
}

void runEngineOnce(const std::string& configDir, int timeoutSec)
{
    // 新鲜度守卫先于一切（与 Engine 构造同一条，见 checkShadersFresh 注释）。
    checkShadersFresh();
    // 单实例纪律与 Engine 同款（见 Engine 构造处注释——pgrep 用 returncode，别拿
    // `-c` 的 stdout "0" 判真假）。
    if (std::system("pgrep -x rgl >/dev/null 2>&1") == 0)
        throw std::runtime_error(
            "已有 rgl 进程在跑。imgqa 要求单实例——并行实例会互相遮挡合成，同时扭曲画面与帧率");

    const std::string bin = buildRglDir() + "/rgl";
    const std::string logPath = buildRglDir() + "/imgqa_run.log";
    // **fork 前必须 flush**：子进程 freopen(stdout) 会关闭并冲刷继承来的 stdio 缓冲，
    // 父进程尚未落盘的输出会被子进程再打一遍（实测：表头打 5 份 = 4 次 fork + 1 次
    // 父进程退出）。管道下 stdout 是全缓冲，所以只在重定向到文件/终端时才复现。
    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t pid = ::fork();
    if (pid < 0)
        throw std::runtime_error("fork 失败");
    if (pid == 0) {
        if (::chdir(buildRglDir().c_str()) != 0)
            _exit(120);
        if (freopen(logPath.c_str(), "wb", stdout) == nullptr)
            _exit(121);
        dup2(fileno(stdout), fileno(stderr));
        setenv("RGL_CONFIG_DIR", configDir.c_str(), 1);
        execl(bin.c_str(), "rgl", static_cast<char*>(nullptr));
        _exit(127);
    }
    // llm_debug 截图模式下引擎自己退（llm_shot 拍完收工）；提前退 = 配置错，
    // 必须当场报错附日志——探针排查里"引擎没起来"被当成"闭式解不符"是最贵的假线索。
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(timeoutSec);
    while (std::chrono::steady_clock::now() < deadline) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            if (st != 0)
                throw std::runtime_error("引擎退出 status=" + std::to_string(st)
                    + "，见 " + logPath);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    ::kill(pid, SIGTERM);
    int st = 0;
    ::waitpid(pid, &st, 0);
    throw std::runtime_error("引擎 " + std::to_string(timeoutSec)
        + "s 未退出（llm_shot 未生效？配置未含 llm_debug），见 " + logPath);
}

}  // namespace RGL::imgqa
