// rig_attribution.cpp —— 机位几何类里「吃 manifest 区域归因」的三条判据命令（从 main.cpp 拆出）。
//
// ## 缝在哪
//
// 缝 = **仪器**，不是行数。attribution（一个机位下谁被画了、占多少、多亮）、faces（观测亮度簇
// 必须每簇都被某个面的闭式解解释）、switch（开关影响域：域外逐位为零 + 域内确实变了）共用同
// 一台采集机：`loadManifest → makeProfile → Engine → captureFrame → attribute()`，三条判据只是
// 这条链末端的三种读法（归属表 / 闭式解对拍 / 两帧差集）。把三条一起搬 = 一条缝；在任意一条
// 中间再切一刀 = 把同一条采集链抄两遍（第二份真相）。
//
// ## 搬动的纪律（纯重构）
//
// 三个命令的函数体、阈值常量与**容差理由注释**从 main.cpp **整块照搬**，一个字节未改——这些
// 打印出来的口径文字就是契约（判据台账与 docs 引用它们）。`addFaceClusterVerdict` 是 faces
// 专属的 static helper，跟着 faces 走，**只此一份**。
//
// 留在 main.cpp 的是路由（dispatch / dispatchRig / main）。`fmt6`（本 TU 唯一的打印格式化小工）
// 跟着唯一调用方住在这里，**不进任何头**——一个符号只准有一份，跨 TU 声明集中在 imgqa.h。
#include "imgqa.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {

namespace {
/// 五位小数定点格式化（attribution 表的线性亮度列）。
std::string fmt6(double v)
{
    std::ostringstream os;
    os << std::fixed << std::setprecision(5) << v;
    return os.str();
}
}  // namespace

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

/**
 * `face-clusters-explained` 的**可求值性闸门** + 求值（拆出来是为了让 cmdFaces 不长分支）。
 *
 * 闭式解只含**直接光**一项（`predictFaces` 无阴影项、无 env 项）⟹ 一旦开了 `--shadow` 或
 * `--env`，画面就多出一个模型里根本没有的项。此时**拒绝求值**，而不是调阈值让它过——
 * 判据的靶子里没有这一项，任何阈值都是在噪声上签字。
 *
 * 实测指纹（2026-10-10，同机位同 piece 两跑，其余配置逐字相同）：
 *   `--shadow` 关：观测簇 0.2214 / 0.2904 ≈ 预测 +Z 0.2221 / +Y 0.2885 ⟹ PASS
 *   `--shadow` 开：同两簇 0.1273 / 0.1936（各 ×0.575 / ×0.667）
 * 两簇系数**不同** = 逐像素阴影因子，正是模型缺的那一项。与 IZB 无关：faces 走合成 profile，
 * `ProfileOpts::izbShadow` 恒 false，所以这条红从 `--shadow` 落地那天起就在，与交付 izb 无关。
 *
 * 与旧 `--env` 注释的唯一区别：那里写"FAIL 是**预期**"= 留一盏永久红灯，而常亮的红等于没有红
 * （gate_lint 的防腐条款用的是同一条理由）⟹ 两个开关统一改成拒绝求值。
 */
static void addFaceClusterVerdict(std::vector<Verdict>& verdicts, const Frame& f,
    const Attribution& at, const std::string& piece, const std::vector<FacePrediction>& predict,
    bool shadowPass, bool envOn)
{
    if (!shadowPass && !envOn) {
        verdicts.push_back(judgeFaceClusters(f, at, piece, predict));
        return;
    }
    std::printf(
        "\n  SKIP [face-clusters-explained] 闭式解只含直接光项，本次开了 %s ⟹ 模型缺项、不可求值。\n"
        "       实测：--shadow 开时两簇各 ×0.575 / ×0.667（系数不同 = 逐像素阴影因子）。\n"
        "       要下闭式结论请两个开关都关；本命令的 --shadow/--env 只用于**复现观察**。\n",
        shadowPass ? (envOn ? "--shadow 与 --env" : "--shadow") : "--env");
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
        o.envIntensity = 1.0;  // 见下方 modelCovers：开 env 后闭式解不再覆盖画面
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
    addFaceClusterVerdict(verdicts, f, at, pieceName, predict, shadowPass, envOn);
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
    const std::string& allowPiecesCsv, bool envOn)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto it = man.cameras.find(camera);
    if (it == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    Frame fOn, fOff, fOff2;
    {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = true;
        // env 开关 = 对照组用：analytic 光照（env=0）下阴影该是纯黑（无补光），
        // 若 on/off 零差异只发生在 analytic 而 env=1 下有影，分叉就在光照配置。
        o.envIntensity = envOn ? 1.0 : 0.0;
        makeProfile(projectRoot() + "/build/imgqa_profile_sw_on", o);
        Engine eng(projectRoot() + "/build/imgqa_profile_sw_on");
        fOn = captureFrame(eng, it->second, "sw_on");
    }
    for (int pass = 0; pass < 2; ++pass) {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = false;
        o.envIntensity = envOn ? 1.0 : 0.0;
        makeProfile(projectRoot() + "/build/imgqa_profile_sw_off", o);
        Engine eng(projectRoot() + "/build/imgqa_profile_sw_off");
        (pass == 0 ? fOff : fOff2) = captureFrame(eng, it->second,
            pass == 0 ? "sw_off" : "sw_off2");
    }

    const Attribution at = attribute(fOn, man);
    // 允许域：允许名单里的 piece 并集外扩 3px（过滤光栅化差异，不改变拓扑）。
    std::stringstream ss(allowPiecesCsv);
    std::string name;
    cv::Mat allowed = cv::Mat::zeros(fOn.ids.size(), CV_8UC1);
    while (std::getline(ss, name, ',')) {
        cv::bitwise_or(allowed, dilateMask(maskOfPiece(fOn, at, name), 3), allowed);
    }
    // 前置判据先跑：同配置（shadow off）两次采集若不等，后面所有「域外恰为 0」
    // 都无从谈起——先把引擎非确定性从嫌疑链里排除，域 FAIL 才可信地归因到开关。
    const Verdict v0 = judgeDeterminism(fOff, fOff2);
    const Verdict v1 = judgeSwitchDomain(fOn, fOff, allowed);
    const Verdict v2 = judgeDeadBranch(fOn, fOff, allowed,
        "shadow mode csm/none 在 [" + allowPiecesCsv + "] 上");
    std::cout << "  " << v0.line() << "\n  " << v1.line() << "\n  " << v2.line() << "\n";
    std::printf("\n  容差理由: %s\n", v1.whyTol.c_str());
    logCall("switch", {{"glb", glb}, {"camera", camera}, {"allow", allowPiecesCsv}},
        {v0, v1, v2});
    return (v0.ok && v1.ok && v2.ok) ? 0 : 1;
}

}  // namespace RGL::imgqa
