// 引擎会话层：SPV 新鲜度守卫 + rgl 子进程生命周期 + httplib RPC 客户端。
//
// 三层缝（B5 拆文件）：本文件 = **会话**（profile 目录之外的一切）；`engine_profile.cpp` =
// **配置语义**（把 ProfileOpts 翻译成 lua）；`engine_lua_edit.cpp` = **lua 文本编辑原语**。
// 本层不认识任何一份 lua 文件的名字，配置层也不 fork 进程——改超时/重试口径动这里，
// 加一个诊断键动那里。

#include "imgqa.h"

#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
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

/// 引擎运行目录（声明在 `imgqa.h`，`blend_lit.cpp` 也吃它）。原先住本文件的匿名 namespace，
/// 提出来的理由不是"要给外面用"这一句空话，而是：`blend_lit.cpp` 要按**同一条基准**拼
/// llm_shot.file 的绝对路径，自己再拼一遍 `projectRoot() + "/build/rgl"` 就是第二份真相，
/// 而运行期可搬运（RGL_ROOT 覆盖 projectRoot）那条性质会跟着丢。
std::string buildRglDir() { return projectRoot() + "/build/rgl"; }

namespace {

// ── SPV 新鲜度守卫的三枚小件（口径全文见 `checkShadersFresh`）────────────────────────

/// spv 目录里有没有任何一个 `.spv`：一个都没有 = 从未构建 ⟹ 跳过守卫（后续引擎启动失败兜底）。
bool hasAnySpv(const fs::path& spvDir)
{
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(spvDir, ec))
        if (!ec && e.is_regular_file(ec) && e.path().extension() == ".spv")
            return true;
    return false;
}

/// 清单的一行（`<spv 名> <它自己的源闭包…>`）里比该 spv 新的源，逐条给「源 新于 spv」。
/// 该 spv 未构建（如 target 新加未编译）= 空：引擎启动失败兜底，不在这里喊。
std::vector<std::string> staleDepsOfLine(const fs::path& spvDir, const std::string& line)
{
    std::vector<std::string> out;
    if (line.empty() || line[0] == '#')
        return out;
    std::error_code ec;
    std::istringstream cols(line);
    std::string spvName;
    cols >> spvName;
    const auto spvT = fs::last_write_time(spvDir / spvName, ec);
    if (ec)
        return out;
    for (std::string depRel; cols >> depRel;) {
        const auto depT = fs::last_write_time(fs::path(projectRoot()) / depRel, ec);
        if (!ec && depT > spvT)
            out.push_back(depRel + " 新于 " + spvName);
    }
    return out;
}

/// 「源新于 spv」的总对数 + 要打进异常里的读数行。两者必须分开数：读数刷屏没有信息量，
/// 但总数被截成 5 就是把 N 对过期读成 5 对。
struct StaleScan {
    int pairs = 0;
    std::string lines;
};

/// 一对过期记进总账：`pairs` 照计，读数行只收头 5 条。
void noteStale(StaleScan& scan, const std::string& dep)
{
    ++scan.pairs;
    if (scan.pairs > 5)
        return;
    if (scan.pairs > 1)
        scan.lines += "\n";
    scan.lines += "  " + dep;
}

/// 逐行走完整份清单（口径 = 逐 spv 只比它自己的源闭包，见 `checkShadersFresh` 头注释）。
StaleScan scanStaleSpvs(const fs::path& spvDir, std::istream& manifest)
{
    StaleScan scan;
    for (std::string line; std::getline(manifest, line);)
        for (const auto& dep : staleDepsOfLine(spvDir, line))
            noteStale(scan, dep);
    return scan;
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
    if (!hasAnySpv(spvDir))
        return;

    const fs::path manifest = fs::path(buildRglDir()) / "shader_deps.txt";
    if (!fs::exists(manifest))
        throw std::runtime_error(
            "SPV 新鲜度无法判定：build/rgl/shader_deps.txt 缺失（正常构建随 shader_dep_check "
            "目标生成）。先 just build 重新生成后重跑。");

    std::ifstream in(manifest);
    if (!in)
        throw std::runtime_error("读不到 " + manifest.string());
    const StaleScan stale = scanStaleSpvs(spvDir, in);
    if (stale.pairs) {
        std::string tail;
        if (stale.pairs > 5)
            tail = "\n  …（共 " + std::to_string(stale.pairs) + " 对）";
        throw std::runtime_error(
            "SPV 疑似过期（测量会跑在旧着色器上）：\n" + stale.lines + tail
            + "\n  修复：just build（重编 SPV；改 shader 契约/schema 再 just reflect）后重跑");
    }
}

}  // namespace

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

/**
 * 引擎日志尾部（「配置写出来必须能被引擎接受」这条自检的**取证**部分）。
 *
 * 判据本体在 Engine 构造：RPC 就绪 = 引擎走完了全部配置加载（ConfigLoadFailed 抛在
 * RPC 挂起之前）；RPC 永不就绪 / 进程提前退出 = 配置**没被接受**。此时最贵的失效是
 * 拿着"超时"或 `strerror(status)`（对 wait status 用 strerror = 驴唇不对马嘴）去排查，
 * 而引擎早就把 `[error] ... ConfigLoadFailed ...` 写进了日志——把尾部原样带进异常，
 * 失效当场可读（本轮缺陷 B：REF 组 runtime.lua 被写成嵌套表，报错只有半行截断路径）。
 *
 * 声明在 `imgqa.h`（不在匿名 namespace：selftest 要直接喂合成日志核尾部口径——
 * 债-92 的教训正是「存在理由被静默废掉而无人红过闸门」，这条判据不许只活在注释里）。
 */
std::string logTail(const std::string& path, int lines)
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
    const long start = n > lines ? n - lines : 0;  // n 行取尾 lines 行：起点 = n − lines（债-92：
    // 写成 n 时循环体一次不进 = 恒空串，两个调用点全绿着把取证废掉）
    for (long i = start; i < n; ++i)
        out << "\n    | " << ring[static_cast<size_t>(i % lines)];
    return out.str();
}

// ── 债-89：连拍锚校验（纯函数，selftest 喂合成值直测，先例 = logTail 的「判据不许只活在
//    注释里」）──────────────────────────────────────────────────────────────────────

std::string burstAnchorMissError(uint32_t startFrame, uint32_t frameNow)
{
    // `==` 还来得及：请求在帧首钩子里 drain，本帧 drawFrame 尚未跑（引擎侧同口径，见
    // rpcHandleShotBurst）。只有**越过**才算错过。
    if (frameNow <= startFrame)
        return {};
    return "连拍锚点帧 " + std::to_string(startFrame) + " 已过（引擎当前帧 "
        + std::to_string(frameNow) + "）——锚点 = 绝对帧号，必须高于 RPC 就绪落在的第几帧："
          "抬高 kBurstAnchorBase，或查引擎是否被上一次 burst/卡死拖慢；**不许静默退回相对锚**"
          "（债-89：那等于把彩票藏起来）";
}

std::string burstAnchorEchoError(uint32_t startFrame, bool echoedPresent, uint32_t echoedFrame)
{
    // 缺回显 = 旧构建不认识 start_frame 字段（HTTP 层对未知字段静默宽容）⟹ burst 实际按
    // 相对锚跑了，而读数看起来完全正常 = 债-89 要消灭的正是这种「藏起来的彩票」。
    if (!echoedPresent)
        return "/shotburst 应答没有 startFrame 回显（请求锚点 " + std::to_string(startFrame)
            + "）——引擎构建不认识 start_frame，连拍实际按相对锚在跑（静默退化）。"
              "确认 rgl 是含债-89 锚点支持的构建后重跑";
    if (echoedFrame != startFrame)
        return "连拍首张落帧 " + std::to_string(echoedFrame) + " ≠ 锚点 "
            + std::to_string(startFrame) + "（锚帧被 acquire 跳帧？）——采集相位不是请求的"
              "相位，本组数作废，重跑或换锚点";
    return {};
}

namespace {

// ── 子进程与 RPC 就绪（从 `Engine` 构造切出：构造本体只留「守卫 → fork → 挂 http」）────

/// 下面两枚只是把 `Engine` 构造里那两个 `std::to_string(W…(st) ? … : …)` 挪出 throw 表达式：
/// 三元塞在 throw 里会把构造的认知复杂度顶到线上，而文案逐字不变。
/// wait status → 文案里的退出码（`WIFEXITED` 不成立时给 -1，与原表达式逐字同）。
std::string exitCodeText(int st) { return std::to_string(WIFEXITED(st) ? WEXITSTATUS(st) : -1); }

/// wait status → 文案里的信号号（不是被信号杀的 = 0，与原表达式逐字同）。
std::string signalText(int st) { return std::to_string(WIFSIGNALED(st) ? WTERMSIG(st) : 0); }

/// 子进程侧的全部动作（fork 之后才跑）：切 CWD → stdout/stderr 接日志 → 指定 profile 目录 exec。
/// **不得抛**：异常穿不过 `execl`，失败只能以非零码报出去（120/121/127 各指一处）。
void execEngineChild(const std::string& bin, const std::string& logPath,
    const std::string& profileDir)
{
    // 子进程：CWD 必须是 build/rgl（相对路径读 configs/ 与 generated_shaders/）。
    if (::chdir(buildRglDir().c_str()) != 0)
        _exit(120);
    if (freopen(logPath.c_str(), "wb", stdout) == nullptr)
        _exit(121);
    dup2(fileno(stdout), fileno(stderr));
    setenv("RGL_CONFIG_DIR", profileDir.c_str(), 1);
    execl(bin.c_str(), "rgl", static_cast<char*>(nullptr));
    _exit(127);
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
    if (pid == 0)
        execEngineChild(bin, impl_->logPath, impl_->profileDir);
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
                + exitCodeText(st) + ", signal=" + signalText(st)
                + "）= 写出的配置没被引擎接受，见 " + impl_->logPath + logTail(impl_->logPath));
        }
        if (impl_->http->Get("/"))
            return;
        // 10ms（原 250ms，债-89）：`GET /` 只在**帧边界**被 drain，就绪检测落在第几帧 =
        // 轮询粒度 × 帧率——250ms 粒度在本机 2500fps headless 下把就绪帧甩到 ~1260（改前
        // 亲验 burst 起始帧 194/1280 同命令两臂差 6 倍），绝对锚点基座就没有可信的上界。
        // 收紧到 10ms 后就绪帧 ≤ ~30，kBurstAnchorBase=512 留出一个数量级的余量。
        // 代价 = bind 前每轮一次 loopback 拒连（微秒级），60s 预算内 ≤ 数千次，可忽略。
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    impl_->killTree();
    throw std::runtime_error("RPC 端口 " + std::to_string(port) + " 60s 未就绪，见 "
        + impl_->logPath + logTail(impl_->logPath));
}

void Engine::setCamera(const Camera& cam, uint32_t yawAtFrame)
{
    // fps 模式下引擎先读 position 再读 target 反推朝向（引擎实现约定）。
    json body = {{"mode", "fps"}, {"position", cam.position},
        {"target", cam.target}, {"fovDegrees", cam.fovDegrees}};
    if (yawAtFrame != 0)
        body["yaw_at_frame"] = yawAtFrame;
    auto res = impl_->http->Post("/camera", body.dump(), "application/json");
    if (!res || res->status != 200)
        throw std::runtime_error("POST /camera 失败: "
            + (res ? std::to_string(res->status) + ": " + res->body : "无响应"));
    if (yawAtFrame == 0)
        return;
    // 回显自证（同 /shotburst 的 startFrame）：没回显 = 引擎没钉（旧构建 / 字段被吞），
    // 那机位就还是「写入落帧」的函数 = 彩票，必须抛，而不是继续跑出一组假可复现的数。
    const json out = json::parse(res->body);
    if (!out.contains("yawPinnedFrame") || out.at("yawPinnedFrame").get<uint32_t>() != yawAtFrame)
        throw std::runtime_error("/camera 没回显 yawPinnedFrame=" + std::to_string(yawAtFrame)
            + "（机位锚点未生效，读数不可复现）: " + res->body);
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

/**
 * `/state` 之类的响应**必须是 JSON 对象**；不是就把真因说出来。
 *
 * 为什么要有这条：引擎侧曾写成 `{{"render", {...}}}`（多一对花括号 ⟹ 该元素是单元素数组
 * 而不是 `{"key", value}` 二元对），nlohmann 于是把**整个** `/state` 序列化成数组。
 * 届时下游 `out.at("pages")` 只抛 `type_error.304 cannot use at() with array` —— 那条信息
 * 指向「调用侧用错了 API」，而真相是「服务端响应形状坏了」。差一层，指错人。
 */
static void requireJsonObject(const json& v, const char* what)
{
    if (v.is_object())
        return;
    throw std::runtime_error(std::string(what) + " 返回的不是 JSON 对象（是 "
        + std::string(v.type_name()) + "）⟹ 引擎侧响应构造坏了：查 rpcHandleState 的 "
          "initializer-list 里是否混入了非 {key, value} 二元对的元素");
}

double Engine::paramValue(const std::string& page, const std::string& label)
{
    auto res = impl_->http->Get("/state");
    if (!res || res->status != 200)
        throw std::runtime_error(
            "GET /state 失败: " + (res ? std::to_string(res->status) : std::string("无响应")));
    const json out = json::parse(res->body);
    requireJsonObject(out, "GET /state");
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

std::string Engine::stateRaw()
{
    auto res = impl_->http->Get("/state");
    // 两种失败分开报：无响应 = 连接层（进程已退 / 端口没绑），非 200 = 服务端拒了。
    // 合成一条会让「引擎跑完了」被读成「RPC 坏了」。
    if (!res)
        throw std::runtime_error("GET /state 无响应（连接失败：引擎进程已退出，或 diagnostics.rpc 没开）");
    if (res->status != 200)
        throw std::runtime_error("GET /state 失败 status=" + std::to_string(res->status));
    return res->body;
}

std::string Engine::shot(const std::string& tag, const char* attachment, int gbuf)
{
    json body{{"file", "screenshots/imgqa_" + tag + ".png"}};
    if (attachment)
        body["attachment"] = attachment;
    // 诊断直读 G-buffer 通道（dump_gbuf 同路，线性原字节）。写盘名由引擎硬编码
    // （gbuffer_N.png，见 writeCapturePng），file 键会被忽略——响应回真实路径，下面照它读。
    if (gbuf >= 0)
        body["gbuf"] = gbuf;
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

double Engine::cameraYawDegrees()
{
    auto res = impl_->http->Post("/camera", "{}", "application/json");
    if (!res || res->status != 200)
        throw std::runtime_error(
            "POST /camera（只读回显）失败: " + (res ? std::to_string(res->status) : "无响应"));
    return json::parse(res->body).at("camera").at("yawDegrees").get<double>();
}

EngineCameraPose Engine::cameraPoseDegrees()
{
    auto res = impl_->http->Post("/camera", "{}", "application/json");
    if (!res || res->status != 200)
        throw std::runtime_error(
            "POST /camera（只读回显姿态）失败: " + (res ? std::to_string(res->status) : "无响应"));
    const json cam = json::parse(res->body).at("camera");
    return { cam.at("yawDegrees").get<double>(), cam.at("pitchDegrees").get<double>(),
        cam.at("fovDegrees").get<double>() };
}

void Engine::setCameraYaw(double yawDegrees)
{
    const json body{{"yawDegrees", yawDegrees}};
    auto res = impl_->http->Post("/camera", body.dump(), "application/json");
    if (!res || res->status != 200)
        throw std::runtime_error("POST /camera(yawDegrees) 失败: "
            + (res ? std::to_string(res->status) : "无响应"));
}

namespace {

/// `GET /` 的 `frame` 字段（`Engine::frameCount` 与锚点预检共用一份解析；预检住在
/// postShotBurst 里，那里只拿得到 Impl，见其注释）。
[[nodiscard]] uint32_t readFrame(httplib::Client& http)
{
    auto res = http.Get("/");
    if (!res || res->status != 200)
        throw std::runtime_error("GET / 失败: " + (res ? std::to_string(res->status) : "无响应"));
    const json out = json::parse(res->body);
    return out.at("frame").get<uint32_t>();
}

}  // namespace

uint32_t Engine::frameCount()
{
    return readFrame(*impl_->http);
}

void Engine::advanceFrames(uint32_t n, int timeoutSec)
{
    // 目标 = 当前计数 + n。**相对只许用于「纯热身」**——它要的性质仅仅是「又渲染了 n 帧」，
    // 落点在第几帧无所谓。旧注释在此主张「用相对目标而不是绝对帧号：RPC 登记落在第几帧由
    // 启动时序决定，绝对号会让热身长度本身带噪声」——**该立场已被债-89 实测推翻**：它把
    // burst 落点的彩票扣在热身长度头上，而 burst 起始的绝对帧号**本身就是测量点**（抖动
    // 相位 = Halton(绝对帧号)、frame_yaw 机位 = 帧号纯函数；亲验同配置三跑边缘列飘 ±2px、
    // 一次整组 rc=1）。旧注释指出的困难是真的（RPC 就绪时引擎可能已跑过目标绝对帧号），
    // 现在的正面解法 = `advanceToFrame` + `shotBurstAt`：锚点选在远高于启动时序抖动
    // （kBurstAnchorBase），真错过 = 响亮抛，**永不**静默退回相对。
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

void Engine::advanceToFrame(uint32_t absFrame, int timeoutSec)
{
    // 调用时锚点已过 = 测量点已经丢了 ⟹ 当场抛（文案可归因到「抬基座/查卡死」，见
    // burstAnchorMissError）。这里**没有**「那就改成相对推进」的分支——那是债-89 判死的
    // 静默退化形态。
    if (const auto err = burstAnchorMissError(absFrame, frameCount()); !err.empty())
        throw std::runtime_error(err);
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSec);
    uint32_t last = 0;
    int stalled = 0;
    while (true) {
        const uint32_t now = frameCount();
        if (now >= absFrame)
            return;
        // 卡死检测与 advanceFrames 同口径（静默少热身 = 在历史瞬态上下判据）。
        stalled = now == last ? stalled + 1 : 0;
        last = now;
        if (stalled > 200)
            throw std::runtime_error("帧计数停在 " + std::to_string(now) + "（引擎卡死？），"
                + "绝对锚点帧 " + std::to_string(absFrame) + " 未到——拒绝在瞬态里继续采集");
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("advanceToFrame 超时：绝对锚点帧 " + std::to_string(absFrame)
                + "，停在 " + std::to_string(now));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

namespace {

/// 债-89 锚点闸门③：应答回显自证（burstAnchorEchoError 的文案直接做异常体）。拆成独立
/// 函数是认知复杂度红线，不是复用——只有 postShotBurst 调它。
void requireBurstAnchorEcho(const json& out, uint32_t startFrame)
{
    const bool present = out.contains("startFrame") && out["startFrame"].is_number_integer();
    const uint32_t echoed = present
        ? static_cast<uint32_t>(out["startFrame"].get<int64_t>()) : 0u;
    if (const auto err = burstAnchorEchoError(startFrame, present, echoed); !err.empty())
        throw std::runtime_error(err);
}

/// /shotburst 共用实现（`shotBurst` / `shotBurstAt` 两个入口；只吃 http 客户端——
/// `Engine::Impl` 是私有嵌套类型，自由函数点名越不过访问检查）。startFrame = 0 = **不发**
/// `start_frame` 字段（相对锚，现行为逐位不变）；> 0 = 绝对锚（债-89）。锚点路径的三道
/// 闸门全部响亮：① 发请求前预检帧计数（`burstAnchorMissError`）；② 引擎侧锚已过 = 409
/// 走 status 分支转抛（带引擎原文）；③ 应答回显自证（`burstAnchorEchoError`，缺回显 =
/// 旧构建静默忽略 start_frame，同样抛）。少任何一道，旧构建的静默忽略就会把判据悄悄退回
/// 彩票——三道都不许合并或省略。
std::vector<std::string> postShotBurst(httplib::Client& http, const std::string& dir,
    uint32_t n, uint32_t startFrame, const std::string& prefix, const std::string& attachment)
{
    json body{{"dir", dir}, {"n", static_cast<int>(n)}, {"prefix", prefix}};
    if (!attachment.empty())
        body["attachment"] = attachment;  // 具名附件连拍（temporal-motion 的 taa_out alpha 掩膜）
    if (startFrame != 0) {
        if (const auto err = burstAnchorMissError(startFrame, readFrame(http)); !err.empty())
            throw std::runtime_error(err);
        body["start_frame"] = startFrame;
    }
    // **读超时按张数放大**：/shotburst 的响应在最后一张写完才兑现 ⟹ 响应时间 = N×(帧时间 +
    // PNG 编码)。2560×1440 的 REF 组单张编码实测数百毫秒 ⟹ 32 张远超客户端默认 20s，
    // 症状是「POST /shotburst 无响应」（其实是客户端先挂断，引擎随后正常返回）。
    // 预算 8s/张（≈ 默认 20s 的 12 倍余量），上限 10 分钟；其它端点保持 20s（它们慢 = 真卡死）。
    // 带锚点的请求还含「等锚」段——调用侧先用 `advanceToFrame` 等到 lead 帧内才发（长等待
    // 留在客户端 300s 预算里），引擎侧等待 ≪ 本预算，也 ≪ 引擎侧 120s 的 burst 应答预算。
    http.set_read_timeout(std::min(600, std::max(20, static_cast<int>(n) * 8)), 0);
    auto res = http.Post("/shotburst", body.dump(), "application/json");
    http.set_read_timeout(20, 0);
    if (!res)
        throw std::runtime_error(
            "POST /shotburst 无响应（引擎没起来，或用的是没有该端点的旧构建）");
    if (res->status != 200)
        throw std::runtime_error(
            "POST /shotburst 失败 " + std::to_string(res->status) + ": " + res->body);
    const json out = json::parse(res->body);
    if (startFrame != 0)
        requireBurstAnchorEcho(out, startFrame);
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

}  // namespace

std::vector<std::string> Engine::shotBurst(const std::string& dir, uint32_t n,
    const std::string& prefix, const std::string& attachment)
{
    return postShotBurst(*impl_->http, dir, n, 0, prefix, attachment);
}

std::vector<std::string> Engine::shotBurstAt(const std::string& dir, uint32_t n,
    uint32_t startFrame, const std::string& prefix, const std::string& attachment)
{
    return postShotBurst(*impl_->http, dir, n, startFrame, prefix, attachment);
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
