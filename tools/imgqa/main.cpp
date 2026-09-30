// imgqa CLI。子命令：selftest / check / refuse / attribution / faces / switch / note。
#include "imgqa.h"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {
struct Args {
    std::string get(const std::vector<std::string>& argv, const std::string& key,
        const std::string& def)
    {
        for (size_t i = 0; i + 1 < argv.size(); ++i)
            if (argv[i] == key)
                return argv[i + 1];
        return def;
    }
    bool has(const std::vector<std::string>& argv, const std::string& key)
    {
        return std::find(argv.begin(), argv.end(), key) != argv.end();
    }
};
}  // namespace

std::string fmt6(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(5) << v;
    return os.str();
}

// ── attribution：一个机位下「谁被画了、占多少、多亮」——存在与归属判据本体 ──
int cmdAttribution(const std::string& glb, const std::string& camera)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto it = man.cameras.find(camera);
    if (it == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    ProfileOpts o;
    o.glb = glb;
    const std::string prof = projectRoot() + "/build/imgqa_profile";
    makeProfile(prof, o);

    Engine eng(prof);
    const Frame f = captureFrame(eng, it->second, "attr_" + camera);
    const Attribution at = attribute(f, man);

    std::printf("%s @ %s  (%dx%d)\n\n", glb.c_str(), camera.c_str(), f.bgr.cols, f.bgr.rows);
    std::printf("  %-22s %4s %8s %10s %8s %8s %8s  %s\n", "piece", "id", "像素", "中位线性",
        "p05", "p95", "暗像素%", "色度(R,G)");
    auto stats = at.stats;
    std::sort(stats.begin(), stats.end(),
        [](const PieceStat& a, const PieceStat& b) { return a.pixels > b.pixels; });
    for (const auto& st : stats) {
        char chroma[32];
        std::snprintf(chroma, sizeof chroma, "(%.3f,%.3f)", st.chromaR, st.chromaG);
        std::printf("  %-22s %4d %8lld %10s %8s %8s %7.1f%%  %s\n", st.piece.c_str(), st.oid,
            static_cast<long long>(st.pixels), fmt6(st.medianLinear).c_str(),
            fmt6(st.p05Linear).c_str(), fmt6(st.p95Linear).c_str(),
            st.darkFraction * 100.0, chroma);
    }
    for (const auto& c : at.conflicts)
        std::printf("\n  ⚠ 反解冲突: %s（第二个 id 已退回 unnamed——**这本身就是发现**，"
                    "要么场景不是 manifest 声称的样子，要么渲染写错了 id）\n",
            c.c_str());
    if (!at.unnamed.empty()) {
        std::printf("\n  ⚠ 认不出名字的 id:");
        for (int32_t id : at.unnamed)
            std::printf(" %d", id);
        std::printf("（全为暗像素 ⟹ 色度无定义。宁可无名，不安错名字）\n");
    }
    auto seen = at.visiblePieces();
    std::printf("\n  可见 %zu/%zu:", seen.size(), man.pieces.size());
    for (const auto& s : seen)
        std::printf(" %s", s.c_str());
    std::printf("\n  未可见:");
    for (const auto& p : man.pieces)
        if (std::find(seen.begin(), seen.end(), p.name) == seen.end())
            std::printf(" %s", p.name.c_str());
    std::printf("  ← 对这些 piece 的任何判据在此机位**恒真**，禁止求值\n");

    logCall("attribution", {{"glb", glb}, {"camera", camera}}, {});
    return 0;
}

// ── faces：闭式解接进区域归因。观测亮度簇必须每簇都被某个面的预测解释 ──
int cmdFaces(const std::string& glb, const std::string& pieceName, bool shadowPass,
    bool envOn)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const ManifestPiece* p = nullptr;
    for (const auto& mp : man.pieces)
        if (mp.name == pieceName)
            p = &mp;
    if (!p)
        throw std::runtime_error("piece 不存在: " + pieceName);

    // 探针机位：从 piece 中心偏移，让**多个面同时可见**（垂直看只有一簇，判据退化成
    // 「这簇对不对」，失去「哪个面解释不了」的能力）。距离 = 3×最大边长：小物体在
    // 面心附近 V 近似误差可忽略，太大则像素太少。
    std::array<double, 3> center{}, ext{};
    double scale = 0.0;
    for (int i = 0; i < 3; ++i) {
        center[i] = (p->aabbMin[i] + p->aabbMax[i]) / 2.0;
        ext[i] = p->aabbMax[i] - p->aabbMin[i];
        scale = std::max(scale, ext[i]);
    }
    const Vec3 dir = normalize(Vec3{-0.45, 0.37, 0.85});
    Engine::Camera cam;
    cam.position = {center[0] + dir.x * 3.0 * scale, center[1] + dir.y * 3.0 * scale,
        center[2] + dir.z * 3.0 * scale};
    cam.target = center;
    cam.fovDegrees = 45.0;

    ProfileOpts o;  // 默认 = 分析光照（env/ambient/ao/skybox 归零、shadow 关、曝光 1）
    o.glb = glb;
    o.shadowPass = shadowPass;
    if (envOn)
        o.envIntensity = 1.0;  // ⚠ 开 env 后闭式解不再覆盖画面（预测只有直接光项）——
                               // 此时 face-clusters FAIL 是**预期**，此开关只用来复现
                               // 「某配置下有没有黑面」这类观察，不用来下闭式结论。
    const std::string prof = projectRoot() + "/build/imgqa_profile_faces";
    makeProfile(prof, o);
    Engine eng(prof);
    const Frame f = captureFrame(eng, cam, "faces_" + pieceName);
    const Attribution at = attribute(f, man);

    // **同一出处**：预测参数与 makeProfile 写进配置的参数必须是同一组值。
    const auto predict = predictFaces(o.lightDir, p->color, p->roughness, p->metallic,
        o.lightIntensity, cam.position, center, o.exposure);

    std::printf("%s: 机位 (%.2f,%.2f,%.2f) → (%.1f,%.1f,%.1f)\n", pieceName.c_str(),
        cam.position[0], cam.position[1], cam.position[2], center[0], center[1], center[2]);
    std::printf("  albedo=(%g,%g,%g) rough=%g metal=%g light=(%g,%g,%g)\n\n", p->color[0],
        p->color[1], p->color[2], p->roughness, p->metallic, o.lightDir[0], o.lightDir[1],
        o.lightDir[2]);
    std::printf("  闭式解各面预测（观测域亮度 + 字节）：\n");
    for (const auto& fp : predict) {
        std::printf("    %3s ndotl=%-8.4f luma=%-9.5f byte=(%3d,%3d,%3d)\n", fp.face.c_str(),
            fp.ndotl, fp.luma, fp.byte[0], fp.byte[1], fp.byte[2]);
    }

    std::vector<Verdict> verdicts;
    verdicts.push_back(judgePieceDrawn(f, at, pieceName));
    verdicts.push_back(judgeFaceClusters(f, at, pieceName, predict));
    std::printf("\n");
    for (const auto& v : verdicts)
        std::cout << "  " << v.line() << "\n";

    // 附：暗像素的位置画像（相对 piece 包围盒归一化）。刻意**不做几何投影**——
    // 复刻运行期自适应 near 平面是另一个兔子洞；v 画像足以人工认出「朝相机那侧」。
    const cv::Mat m = maskOfPiece(f, at, pieceName);
    if (cv::countNonZero(m) > 0) {
        cv::Mat dark;
        cv::compare(f.luma, 0.005, dark, cv::CMP_LT);
        cv::Mat darkPiece;
        cv::bitwise_and(m, dark, darkPiece);
        const long long nDark = cv::countNonZero(darkPiece);
        std::printf("\n  %s 占 %d px，其中纯黑 %lld (%.1f%%)\n", pieceName.c_str(),
            cv::countNonZero(m), nDark, 100.0 * nDark / cv::countNonZero(m));
        if (nDark > 0) {
            const cv::Moments mo = cv::moments(darkPiece);
            cv::Rect box = cv::boundingRect(m);
            const double u = (mo.m10 / mo.m00 - box.x) / std::max(1, box.width - 1);
            const double vPos = (mo.m01 / mo.m00 - box.y) / std::max(1, box.height - 1);
            std::printf("  暗簇质心（相对包围盒，v 向下）u=%.2f v=%.2f"
                        "   ← 上半=顶面方向，下半=朝向相机那侧\n",
                u, vPos);
        }
    }
    logCall("faces", {{"glb", glb}, {"piece", pieceName}}, verdicts);
    bool allOk = true;
    for (const auto& v : verdicts)
        allOk &= v.ok;
    return allOk ? 0 : 1;
}

// ── switch：开关影响域。域外必须逐位为零 + 域内必须确实变了（两条对偶）──
int cmdSwitch(const std::string& glb, const std::string& camera,
    const std::string& allowPiecesCsv)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto it = man.cameras.find(camera);
    if (it == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    Frame fOn, fOff;
    {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = true;
        makeProfile(projectRoot() + "/build/imgqa_profile_sw_on", o);
        Engine eng(projectRoot() + "/build/imgqa_profile_sw_on");
        fOn = captureFrame(eng, it->second, "sw_on");
    }
    {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = false;
        makeProfile(projectRoot() + "/build/imgqa_profile_sw_off", o);
        Engine eng(projectRoot() + "/build/imgqa_profile_sw_off");
        fOff = captureFrame(eng, it->second, "sw_off");
    }

    const Attribution at = attribute(fOn, man);
    // 允许域：允许名单里的 piece 并集外扩 3px（过滤光栅化差异，不改变拓扑）。
    std::stringstream ss(allowPiecesCsv);
    std::string name;
    cv::Mat allowed = cv::Mat::zeros(fOn.ids.size(), CV_8UC1);
    while (std::getline(ss, name, ',')) {
        cv::bitwise_or(allowed, dilateMask(maskOfPiece(fOn, at, name), 3), allowed);
    }
    const Verdict v1 = judgeSwitchDomain(fOn, fOff, allowed);
    const Verdict v2 = judgeDeadBranch(fOn, fOff, allowed,
        "shadow_pass on/off 在 [" + allowPiecesCsv + "] 上");
    std::cout << "  " << v1.line() << "\n  " << v2.line() << "\n";
    std::printf("\n  容差理由: %s\n", v1.whyTol.c_str());
    logCall("switch", {{"glb", glb}, {"camera", camera}, {"allow", allowPiecesCsv}},
        {v1, v2});
    return (v1.ok && v2.ok) ? 0 : 1;
}

int cmdCheck(bool onlyPromoted)
{
    std::printf("判据注册表（共 %zu 条）：\n\n", registeredJudgments().size());
    for (const auto& j : registeredJudgments()) {
        std::printf("  [%s] %s\n", j.promoted ? "PROMOTED" : "advisory", j.name);
        std::printf("             容差理由：%s\n", j.whyTol);
        for (const auto& g : j.guards)
            std::printf("             钉住：%s\n", g.c_str());
    }
    int nPromoted = 0;
    for (const auto& j : registeredJudgments())
        nPromoted += j.promoted;
    std::printf("\n");
    if (onlyPromoted && nPromoted == 0) {
        std::printf("晋升集为空。**这是诚实的状态**：还没有判据满足四条晋升条件"
                    "（真实 bug/变异上变红过、跨 ≥20 次无关改动不误报、容差有理由、有反向用例）。\n"
                    "晋升不是宣布，是攒出来的——攒的凭据在 qa/log/imgqa_calls.jsonl。\n");
        return 0;
    }
    std::printf("（本命令只列注册表与晋升状态；实际求值需要引擎，见 attribution / faces / switch）\n");
    return 0;
}

int cmdRefuse(const std::string& name)
{
    if (name.empty()) {
        std::printf("已登记的反模式（都会导致错误结论，故工具**主动拒绝**）：\n\n");
        for (const auto& [k, v] : antiPatterns())
            std::printf("  %s\n        %s\n\n", k.c_str(), v.c_str());
        return 0;
    }
    std::string key = name;
    std::replace(key.begin(), key.end(), '_', '-');
    const auto& ap = antiPatterns();
    const auto it = ap.find(key);
    if (it == ap.end()) {
        std::printf("未登记的反模式：%s\n已登记：", name.c_str());
        for (const auto& [k, v] : ap)
            std::printf("%s ", k.c_str());
        std::printf("\n");
        return 2;
    }
    std::printf("✗ 反模式 `%s` 被拒绝：\n        %s\n", key.c_str(), it->second.c_str());
    return 2;
}

void printUsage()
{
    std::printf(
        "imgqa —— 区域归因的图像判据工具箱（判据的区域来自几何，不来自图像）\n\n"
        "用法: imgqa <子命令> [选项]\n"
        "  selftest                                   判据/转录/解码器自检（无引擎）\n"
        "  check [--promoted]                         列判据注册表与晋升状态\n"
        "  refuse [name]                              列/触发反模式拒绝（ssim 等有理由的禁止）\n"
        "  attribution [--glb X.glb] [--camera 名]    机位下谁被画了/占多少/多亮/谁全黑\n"
        "  faces [--glb X.glb] --piece 名              闭式解逐面预测 vs 观测亮度簇\n"
        "  switch [--glb X.glb] [--camera 名] [--allow-piece a,b]  开关影响域 + 死分支\n"
        "  note \"事后真相\"                             回填最后一条调用记录（晋升凭据）\n");
}

}  // namespace

int main(int argc, char** argv)
{
    using namespace RGL::imgqa;
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "-h" || args[0] == "--help" || args[0] == "help") {
        printUsage();
        return 0;
    }
    Args P;
    const std::string cmd = args[0];
    try {
        if (cmd == "selftest")
            return cmdSelftest();
        if (cmd == "check")
            return cmdCheck(P.has(args, "--promoted"));
        if (cmd == "refuse")
            return cmdRefuse(args.size() > 1 ? args[1] : "");
        if (cmd == "attribution")
            return cmdAttribution(P.get(args, "--glb", "shadow_rig.glb"),
                P.get(args, "--camera", "near_anchor"));
        if (cmd == "faces")
            return cmdFaces(P.get(args, "--glb", "shadow_rig.glb"),
                P.get(args, "--piece", "anchor_cube_near"), P.has(args, "--shadow"),
                P.has(args, "--env"));
        if (cmd == "switch")
            return cmdSwitch(P.get(args, "--glb", "shadow_rig.glb"),
                P.get(args, "--camera", "near_anchor"),
                P.get(args, "--allow-piece", "ground"));
        if (cmd == "note") {
            std::string text;
            for (size_t i = 1; i < args.size(); ++i)
                text += (i > 1 ? " " : "") + args[i];
            return noteLastCall(text);
        }
        std::printf("未知子命令 %s（-h 看用法）\n", cmd.c_str());
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误: %s\n", e.what());
        return 3;
    }
}
