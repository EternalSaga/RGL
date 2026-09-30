// 配置目录生成 + 引擎进程生命周期（httplib RPC）。
#include "imgqa.h"

#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <regex>
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
}  // namespace

std::string makeProfile(const std::string& outDir, const ProfileOpts& o)
{
    fs::create_directories(outDir);
    // render_graph.lua 定义图本身，**必须整份拷**（只给 {variant=...} 得 0 个
    // swapchain 附件 → 启动失败，实测踩过）。
    fs::copy_file(profileTemplate() + "/render_graph.lua", outDir + "/render_graph.lua",
        fs::copy_options::overwrite_existing);
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
        "shader_reload", "picking", "rpc", "rpc_port", "shadow_pass"};
    assertKeys("diagnostics.lua", diagKeys, knownRootKeys("rgl/diagnostics_impl.cppm"));
    writeFile(outDir + "/diagnostics.lua",
        std::string(
            "return { no_vvl = false, no_hdr = false, no_present = false, vtx_canary = false,\n"
            "  texel_canary = -1, dump_gbuf = -1, style_dist = false, tex_dist = false,\n"
            "  texture_force_size = 0, texture_force_white = false, texture_force_unorm = false,\n"
            "  profiler = 0, imgui = false, config_reload = false, shader_reload = false,\n"
            "  picking = ")
        + lb(o.picking) + ", rpc = true, rpc_port = 8788,\n  shadow_pass = " + lb(o.shadowPass)
        + " }\n");

    assertKeys("scene.lua",
        {"entities", "environment", "light", "spin", "cube", "exposure", "ao"},
        knownRootKeys("rgl/scene_config_impl.cppm"));
    const auto j3 = [](std::array<double, 3> a) {
        return std::to_string(a[0]) + ", " + std::to_string(a[1]) + ", " + std::to_string(a[2]);
    };
    writeFile(outDir + "/scene.lua",
        "return {\n    entities = { { type = \"model\", file = \"models/" + o.glb + "\" } },\n"
        "    environment = { hdr = \"env/san_giuseppe_bridge_4k.hdr\", hdr_size = 2048,\n"
        "      intensity = "
            + std::to_string(o.envIntensity) + ", skybox = " + lb(o.skybox) + " },\n"
          "    light = { direction = { "
            + j3(o.lightDir) + " }, color = { 1.0, 1.0, 1.0 },\n      intensity = "
            + std::to_string(o.lightIntensity) + ", ambient = { " + j3(o.ambient) + " } },\n"
            + "    spin = false, cube = false, exposure = " + std::to_string(o.exposure)
            + ",\n    ao = { strength = " + std::to_string(o.aoStrength) + " },\n}\n");
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

Engine::Engine(std::string profileDir, int port) : impl_(new Impl{.profileDir = profileDir, .port = port})
{
    // 单实例纪律：pgrep 用 returncode 判存在。**不要**拿 `pgrep -c` 的 stdout 判真假
    // ——没找到时它打印 "0"，非空 → 误判成"有残留"而永远拒跑（本轮真实踩过）。
    if (std::system("pgrep -x rgl >/dev/null 2>&1") == 0)
        throw std::runtime_error(
            "已有 rgl 进程在跑。imgqa 要求单实例——并行实例会互相遮挡合成，同时扭曲画面与帧率");

    const std::string bin = buildRglDir() + "/rgl";
    impl_->logPath = buildRglDir() + "/imgqa_run.log";
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
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {
            impl_->pid = -1;
            throw std::runtime_error("引擎提前退出（status=" + std::to_string(st) + "），见 "
                + impl_->logPath + "\n  末行: " + std::string(std::strerror(st)));
        }
        if (impl_->http->Get("/"))
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    impl_->killTree();
    throw std::runtime_error("RPC 端口 " + std::to_string(port) + " 60s 未就绪，见 "
        + impl_->logPath);
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

}  // namespace RGL::imgqa
