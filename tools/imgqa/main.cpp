// imgqa CLI。子命令：selftest / check / refuse / attribution / faces / switch / izb / swim /
// probe / temporal / temporal-residual / temporal-motion / temporal-feedback /
// temporal-flicker / temporal-homography / aa-coverage / taa-swim / note。
#include "imgqa.h"
#include "temporal_flicker.h"
#include "temporal_homography.h"

#include <cstdio>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include <glm/glm.hpp>

#include <opencv2/imgcodecs.hpp>
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
        "shadow_pass on/off 在 [" + allowPiecesCsv + "] 上");
    std::cout << "  " << v0.line() << "\n  " << v1.line() << "\n  " << v2.line() << "\n";
    std::printf("\n  容差理由: %s\n", v1.whyTol.c_str());
    logCall("switch", {{"glb", glb}, {"camera", camera}, {"allow", allowPiecesCsv}},
        {v0, v1, v2});
    return (v0.ok && v1.ok && v2.ok) ? 0 : 1;
}

// ── izb：IZB 消费域判据（mask↔画面寻址一致性；11.3 补账 B）─────────────────
// CSM 基线截图 vs IZB 门开截图 + izb_mask.ppm（门开跑内 dump 产出）三方对拍：
//   锚点   |变暗| ≥ 下限（消费链路活着——2026-10-02 字节偏移 bug 下恒 0）
//   域内   变暗 ⊆ dilate(mask遮,2)（IZB 不许在 mask 判受光处画影——ndc 镜像 bug 红）
//   消费   mask遮 ∩ CSM受光 ⊆ dilate(变暗,2)（mask 有货必须落到画面——消费断链红）
// 变亮集（CSM 遮而 IZB 放）只报不判：v1 近场语义下远场 CSM 影被拆属预期。
int cmdIzb(const std::string& glb, const std::string& camera)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto it = man.cameras.find(camera);
    if (it == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    Frame fCsm, fIzb;
    {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = true;  // CSM 基线；mask 重放的 shadow0 段也依赖此门
        o.variant = "deferred";  // IZB 仅 deferred 生效（forward 无 drawLight pass）
        makeProfile(projectRoot() + "/build/imgqa_profile_izb_csm", o);
        Engine eng(projectRoot() + "/build/imgqa_profile_izb_csm");
        fCsm = captureFrame(eng, it->second, "izb_csm");
    }
    {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = true;
        o.variant = "deferred";
        o.izbShadow = true;
        o.izbDumpFrame = 5;
        makeProfile(projectRoot() + "/build/imgqa_profile_izb_on", o);
        // 删陈旧 ppm（首跑实锤：dump 不生效时旧文件会伪装成"本轮产物"）
        std::filesystem::remove(std::filesystem::path(projectRoot()) / "build/rgl/izb_mask.ppm");
        Engine eng(projectRoot() + "/build/imgqa_profile_izb_on");
        fIzb = captureFrame(eng, it->second, "izb_on");
    }
    // mask PPM（遮挡=黑）→ 8UC1（遮=255）；尺寸必须与截图一致（extent 失配 = 采集错帧）
    const std::string ppm = projectRoot() + "/build/rgl/izb_mask.ppm";
    cv::Mat gray = cv::imread(ppm, cv::IMREAD_GRAYSCALE);
    if (gray.empty())
        throw std::runtime_error("izb_mask.ppm 不存在/读失败（izb_dump_frame 未生效？）");
    if (gray.size() != fIzb.bgr.size())
        throw std::runtime_error("mask 与截图 extent 不符: " + std::to_string(gray.cols)
            + "x" + std::to_string(gray.rows));
    cv::Mat shadowed;
    cv::threshold(gray, shadowed, 127, 255, cv::THRESH_BINARY_INV);

    const auto sumCh = [](const cv::Mat& bgr) {
        cv::Mat f;
        bgr.convertTo(f, CV_32S);
        std::vector<cv::Mat> ch;
        cv::split(f, ch);
        cv::Mat s;
        cv::add(ch[0], ch[1], s);
        cv::add(s, ch[2], s);
        return s;
    };
    const cv::Mat sC = sumCh(fCsm.bgr);
    const cv::Mat d = sumCh(fIzb.bgr) - sC;
    cv::Mat darkened, brightened, litCsm;
    cv::compare(d, cv::Scalar(-30), darkened, cv::CMP_LT);
    cv::compare(d, cv::Scalar(30), brightened, cv::CMP_GT);
    cv::compare(sC, cv::Scalar(90), litCsm, cv::CMP_GE);  // CSM 下受光（和≥90）

    cv::Mat darkIn = darkened.clone();
    cv::bitwise_and(darkIn, dilateMask(shadowed, 2), darkIn);
    cv::Mat darkOut = darkened - darkIn;  // 域外变暗 = 寻址错
    cv::Mat req;
    cv::bitwise_and(shadowed, litCsm, req);  // mask 遮 ∧ CSM 受光 → 必须变暗
    cv::Mat reqDil = req.clone();
    cv::bitwise_and(reqDil, dilateMask(darkened, 2), reqDil);
    cv::Mat unconsumed = req - reqDil;

    const int64_t nDark = cv::countNonZero(darkened);
    const int64_t nDarkOut = cv::countNonZero(darkOut);
    const int64_t nReq = cv::countNonZero(req);
    const int64_t nUncons = cv::countNonZero(unconsumed);
    const int64_t nBright = cv::countNonZero(brightened);

    const Verdict vAnchor = verdictFor("izb-consume-domain", nDark >= 500,
        "锚点：变暗 " + std::to_string(nDark) + " px（下限 500）");
    const Verdict vDomain = verdictFor("izb-consume-domain", nDarkOut == 0,
        "变暗域 ⊆ dilate(mask,2)：域外 " + std::to_string(nDarkOut) + " px（要求恰 0）");
    const Verdict vConsume = verdictFor("izb-consume-domain", nUncons == 0,
        "mask∩受光 ⊆ dilate(变暗,2)：未消费 " + std::to_string(nUncons) + "/"
            + std::to_string(nReq) + " px（要求恰 0）");
    std::cout << "  " << vAnchor.line() << "\n  " << vDomain.line() << "\n  "
        << vConsume.line() << "\n";
    std::printf("\n  INFO 变亮（CSM 遮而 IZB 放，v1 近场语义）: %lld px\n",
        static_cast<long long>(nBright));
    logCall("izb", {{"glb", glb}, {"camera", camera}}, {vAnchor, vDomain, vConsume});
    return (vAnchor.ok && vDomain.ok && vConsume.ok) ? 0 : 1;
}

// ── swim：影子世界空间游动量测（同场景两机位，静态太阳下影场必须世界不变）────
// 判据不是屏幕观感相关（视差会淹没它，5a-7 时代 1D 剖面互相关实测被骗）：
// 把两帧的地面像素各自重投影回世界 y=0 平面、栅格化 2cm 格的受光/遮阴影场，
// 对 B 场做平移搜索（±20cm）——**最优平移 = 影子在世界里滑了多少**。
// 重投影基向量若与引擎有出入只会把两帧**同样地**翘曲，平移差不受影响（漂移
// 量测自洽）；格内多数票消除像素级抖动。
int cmdSwim(const std::string& glb, const std::string& camera, double dx, bool izb, bool evssm)
{
    const std::string stem = glb.substr(0, glb.find_last_of('.'));
    const Manifest man = loadManifest(stem);
    const auto it = man.cameras.find(camera);
    if (it == man.cameras.end())
        throw std::runtime_error("机位不存在: " + camera);

    const auto captureGround = [&](const Engine::Camera& cam, const std::string& tag) {
        ProfileOpts o;
        o.glb = glb;
        o.shadowPass = true;
        o.variant = "deferred";
        o.izbShadow = izb;
        o.evssmShadow = evssm;
        makeProfile(projectRoot() + "/build/imgqa_profile_swim", o);
        Engine eng(projectRoot() + "/build/imgqa_profile_swim");
        return captureFrame(eng, cam, tag);
    };
    Engine::Camera camB = it->second;
    camB.position[0] += dx;
    camB.target[0] += dx;
    const Frame fA = captureGround(it->second, "swim_a");
    const Frame fB = captureGround(camB, "swim_b");

    const auto groundField = [](const Frame& f, const Manifest& m) {
        const Attribution at = attribute(f, m);
        const cv::Mat gnd = maskOfPiece(f, at, "ground");
        // 阴影阈值 = 地面亮度 p95 × 0.3（env=0 解析光照：受光亮/遮黑双峰，居中判）
        std::vector<double> vals;
        for (int y = 0; y < gnd.rows; ++y)
            for (int x = 0; x < gnd.cols; ++x)
                if (gnd.at<uint8_t>(y, x))
                    vals.push_back(f.luma.at<double>(y, x));
        if (vals.size() < 1000)
            throw std::runtime_error("地面像素过少，机位无效？");
        std::sort(vals.begin(), vals.end());
        const double thr = vals[static_cast<size_t>(vals.size() * 0.95)] * 0.3;
        // 重投影（引擎口径：fb 行 0 ↔ ndc.y +1；fov 竖直，x 乘 aspect）
        const glm::vec3 pos(f.camera.position[0], f.camera.position[1], f.camera.position[2]);
        const glm::vec3 tgt(f.camera.target[0], f.camera.target[1], f.camera.target[2]);
        const glm::vec3 fw = glm::normalize(tgt - pos);
        const glm::vec3 rt = glm::normalize(glm::cross(fw, glm::vec3(0.f, 1.f, 0.f)));
        const glm::vec3 up = glm::cross(rt, fw);
        const double tanH = std::tan(f.camera.fovDegrees * 3.14159265358979 / 360.0);
        const double aspect = static_cast<double>(f.bgr.cols) / f.bgr.rows;
        std::map<std::pair<int, int>, int> votes;
        for (int y = 0; y < gnd.rows; ++y)
            for (int x = 0; x < gnd.cols; ++x) {
                if (!gnd.at<uint8_t>(y, x))
                    continue;
                const double nx = (x + 0.5) / f.bgr.cols * 2.0 - 1.0;
                const double ny = 1.0 - (y + 0.5) / f.bgr.rows * 2.0;
                const glm::vec3 dir = glm::normalize(fw
                    + rt * static_cast<float>(nx * tanH * aspect)
                    + up * static_cast<float>(ny * tanH));
                if (std::abs(dir.y) < 1e-6)
                    continue;
                const double t = -pos.y / dir.y;
                if (t <= 0 || t > 5000.0)
                    continue;
                const glm::vec3 w = pos + dir * static_cast<float>(t);
                const auto cell = std::make_pair(static_cast<int>(std::floor(w.x / 0.05)),
                    static_cast<int>(std::floor(w.z / 0.05)));
                votes[cell] += f.luma.at<double>(y, x) < thr ? 1 : -1;
            }
        std::map<std::pair<int, int>, bool> field;
        for (const auto& [c, v] : votes)
            if (std::abs(v) >= 2)  // 格内至少 2 票同向（5cm 格 ≈ 3~4 像素，边缘半格噪声丢弃）
                field[c] = v > 0;
        return field;
    };
    const auto fieldA = groundField(fA, man);
    const auto fieldB = groundField(fB, man);

    // 零位移失配 = 主判据；FAIL 才做平移搜索量化"滑了多少"（候选覆盖率以零位移
    // 共同格数为基准，防止把低覆盖的角落平移当最优解——首跑 48 格教训）
    int mismatchAt0 = 0, common0 = 0;
    for (const auto& [c, sh] : fieldA) {
        const auto b = fieldB.find(c);
        if (b == fieldB.end())
            continue;
        ++common0;
        if (b->second != sh)
            ++mismatchAt0;
    }
    std::printf("  影场格 A=%zu B=%zu 共同=%d（5cm 格）；零位移失配 %d = %.2f%%\n",
        fieldA.size(), fieldB.size(), common0, mismatchAt0,
        100.0 * mismatchAt0 / std::max(1, common0));
    int bestX = 0, bestZ = 0;
    size_t bestMismatch = static_cast<size_t>(mismatchAt0);
    if (mismatchAt0 > std::max(20, common0 / 200)) {
        size_t bestN = 0;
        for (int sz = -10; sz <= 10; ++sz)
            for (int sx = -10; sx <= 10; ++sx) {
                size_t mm = 0, n = 0;
                for (const auto& [c, sh] : fieldA) {
                    const auto b = fieldB.find({c.first + sx, c.second + sz});
                    if (b == fieldB.end())
                        continue;
                    ++n;
                    if (b->second != sh)
                        ++mm;
                }
                if (n < static_cast<size_t>(common0) * 8 / 10)
                    continue;  // 覆盖率不足，该候选不可信
                if (mm < bestMismatch || (mm == bestMismatch && n > bestN)) {
                    bestMismatch = mm;
                    bestN = n;
                    bestX = sx;
                    bestZ = sz;
                }
            }
    }
    std::printf("  最优平移 (%d, %d) 格 = (%.0f, %.0f) cm，失配 %zu\n", bestX, bestZ,
        bestX * 5.0, bestZ * 5.0, bestMismatch);
    const bool still = mismatchAt0 <= std::max(20, common0 / 200);
    const Verdict v = verdictFor("shadow-swim", still,
        "机位平移 " + std::to_string(dx) + " m：影场零位移失配 "
            + std::to_string(mismatchAt0) + "/" + std::to_string(common0)
            + "（阈 max(20, 共同/200)），漂移估计 (" + std::to_string(bestX * 5) + ", "
            + std::to_string(bestZ * 5) + ") cm");
    std::cout << "  " << v.line() << "\n";
    logCall("swim", {{"glb", glb}, {"camera", camera}, {"dx", std::to_string(dx)},
        {"izb", izb ? "on" : "off"}}, {v});
    return v.ok ? 0 : 1;
}

// ── probe：闭式解探针（12.3b 接替 analytic_ref.py --probe；配置即契约）──
int cmdProbe(const std::string& configDir, int x, int y, Vec3 albedo, double roughness,
    double metallic, Vec3 l, double intensity, int tol, bool lightOverride)
{
    // 探针配置 qa/configs/analytic 是**冻结的已知量集合**：六个门逐项关掉写在配置
    // 注释里，着色链塌成闭式可解。默认模式工具不重写这份配置，只消费它。
    // 显式 --light/--intensity（lightOverride）：整目录复制到 build/rgl/imgqa_probe_profile
    // 后**只改副本**的 light 块（direction = -L）。不接这根线时 --light 改预测不改渲染，
    // 等于自欺判据——2026-10-01 黑面调查同日修复（此前该选项只动预测侧）。
    std::string runDir = configDir;
    if (lightOverride)
    {
        namespace fs = std::filesystem;
        const fs::path dst = fs::path(projectRoot()) / "build/rgl/imgqa_probe_profile";
        std::error_code ec;
        fs::remove_all(dst, ec);
        fs::create_directories(dst);
        for (const auto& e : fs::directory_iterator(configDir))
            fs::copy_file(e.path(), dst / e.path().filename(), fs::copy_options::overwrite_existing);
        const auto num = [](double v) {
            char buf[32];
            std::snprintf(buf, sizeof buf, "%.9g", v);
            return std::string(buf);
        };
        std::ifstream in(dst / "scene.lua");
        std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
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
        std::ofstream(dst / "scene.lua") << txt;
        runDir = dst.string();
    }
    runEngineOnce(runDir);
    const std::string png = projectRoot() + "/build/rgl/screenshots/analytic_probe.png";
    const cv::Mat bgr = readPngBgr(png);
    if (x < 0 || y < 0 || x >= bgr.cols || y >= bgr.rows)
        throw std::runtime_error("探针像素越界: (" + std::to_string(x) + ","
            + std::to_string(y) + ") vs " + std::to_string(bgr.cols) + "x"
            + std::to_string(bgr.rows));

    // 适用范围（承 Python 版模块头）：探针面无纹理/无 normal map、法线轴对齐，
    // 相机正视 ⟹ N=V=(0,0,1)。任何一项被改写过，先改这里的前提再谈着色链。
    const Vec3 linear = shadeDirect({0, 0, 1}, {0, 0, 1}, l, albedo, roughness, metallic,
        {intensity, intensity, intensity});
    const auto want = toBytes(linear);
    const cv::Vec3b px = bgr.at<cv::Vec3b>(y, x);  // OpenCV BGR 序
    const std::array<int, 3> got{px[2], px[1], px[0]};
    const std::array<int, 3> diff{
        std::abs(got[0] - want[0]), std::abs(got[1] - want[1]), std::abs(got[2] - want[2])};
    const bool ok = diff[0] <= tol && diff[1] <= tol && diff[2] <= tol;

    std::printf("探针 (%d,%d)  配置=%s%s\n", x, y, runDir.c_str(),
        lightOverride ? "（light 覆写副本）" : "");
    std::printf("  N=V=(0,0,1)  L=(%.6f,%.6f,%.6f)  albedo=(%.2f,%.2f,%.2f)"
        "  rough=%.2f metal=%.2f  I=%.1f\n",
        l.x, l.y, l.z, albedo.x, albedo.y, albedo.z, roughness, metallic, intensity);
    std::printf("  线性预测 = (%.6f, %.6f, %.6f)\n", linear.x, linear.y, linear.z);
    std::printf("  预测字节 = (%d, %d, %d)\n", want[0], want[1], want[2]);
    std::printf("  实际字节 = (%d, %d, %d)\n", got[0], got[1], got[2]);
    std::printf("  差       = (%d, %d, %d)   （阈值 %d）\n", diff[0], diff[1], diff[2], tol);
    const Verdict v = verdictFor("probe-pixel", ok,
        "(" + std::to_string(x) + "," + std::to_string(y) + ") 预测("
        + std::to_string(want[0]) + "," + std::to_string(want[1]) + ","
        + std::to_string(want[2]) + ") vs 实际(" + std::to_string(got[0]) + ","
        + std::to_string(got[1]) + "," + std::to_string(got[2]) + ")");
    std::cout << "  " << v.line() << "\n";
    if (!ok) {
        std::printf("  归因顺序（先怀疑前提，再怀疑引擎）：\n"
                    "    ① 探针面是否真的无纹理 / 无 normal map（适用范围）\n"
                    "    ② N/V/L 是否真如假设（机位与光向是否被相机模式改写过）\n"
                    "    ③ normalFiltering 是否抬了 roughness（平面正视图应为 0 方差）\n"
                    "    ④ 六个配置门是否都真的关上了（少关一项就少一个已知量）\n"
                    "    ⑤ 以上都排除了，才轮到怀疑着色链\n");
    }
    logCall("probe", {{"config", configDir}, {"xy", std::to_string(x) + "," + std::to_string(y)},
        {"light", std::to_string(l.x) + "," + std::to_string(l.y) + "," + std::to_string(l.z)},
        {"tol", std::to_string(tol)}}, {v});
    return v.ok ? 0 : 1;
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
        "  switch [--glb X.glb] [--camera 名] [--allow-piece a,b]  开关影响域 + 死分支（含确定性前置）\n"
        "  izb [--glb izb_rig.glb] [--camera ladder]  IZB 消费域：mask↔画面寻址一致性（三子判据）\n"
        "  swim [--glb izb_rig.glb] [--camera ladder] [--dx 0.06] [--izb]  影场世界不变量：机位平移后重投影比影场（0 位移）\n"
        "  probe [--x 640] [--y 360] [--light Lx,Ly,Lz] [--tol 2]  闭式解探针：该是多少 vs 实际是多少\n"
        "  aa-coverage [--variant forward] [--base-space] [--kernel 0]  逐杆覆盖率对拍闭式：单帧量"
        "「黑度积分 vs 投影宽度」（批 12.6 T4；--base-space = 批 13.2 门开态；--kernel = 批 13.8"
        " 输入侧重建核 0 双线性 / 1 CR 4×4 / 2 Lanczos3，本判据 = 核选型那条**静止态闭式**判据）\n"
        "  temporal [--pairs 32] [--warm 32] [--variant forward] [--base-space]  TAA 时域阳性锚点："
        "静止机位 + jitter，连续帧突发逐帧差（批 12.6 T4）\n"
        "  temporal-residual [--periods 2] [--warm-multiplier 6] [--variant forward]"
        " [--slow-feedback 0.002] [--skip-slow] [--base-space] [--static-boost 0.02]"
        " [--bistro] [--camera all] [--boost-floor 0.01]"
        "  时域**绝对**残差：静止机位连拍折"
        " (2,16,H,W) 后逐像素 max−median（线性域；补 temporal 相对比的盲区，批 13.1 主判据）。"
        " --static-boost <地板> = 批 13.7 `taa.static_boost` 变体（**只出数不判**：跑 B1–B4 四组"
        "出门开/门关 meanSwing/p99.9/frac>τ + 梯度比；⚠ 地板 = **本帧权重**，调小 = 更信历史）。"
        " --bistro = 批 13.7b 真实场景变体（债-48：交付配置 + 用户报障三机位 doorframe/wetground/"
        "doorpanel，N1–N5 五组出数 + 债-47 线性域梯度比 + swing 热图；--camera 选单机位）。"
        "批 13.7c 每机位另出**亮度 × 梯度二维分带**（门关/门开/差三张表，每格 share·rate·"
        "残余质量三数 + 敏感性），判「残余闪烁住在高亮边还是普通边缘」；判决线跑前写死，"
        "全文见 temporal_residual.h 同名段\n"
        "  taa-swim [--pairs 32] [--warm 32] [--variant forward]  真实场景逐帧差是否=整幅亚像素"
        "平移（游动）；含基空间门（批 13.2 T5/T6）与量具阳性锚点（批 13.1b）\n"
        "  temporal-motion [--camera doorframe] [--motion camera] [--angle 0.2] [--burst 32]"
        " [--warm 16] [--warm-still 64] [--edge-pct 0.90]  **相机运动下 TAA 抗了什么锯齿**："
        "--motion camera（默认）= 相机帧驱动 yaw（camera.lua frame_yaw，= 用户报障的运动源；"
        "angle=角度×帧号，纯函数）；--motion spin = 场景旋转相机不动（债-49 鬼影仪器）。"
        "M0–M4 对**同机位 2560×1440 超采样参考**量边缘像素线性域偏差（按帧号配对到同一视点，"
        "故不受 `moving-camera-l1-ratio` 反模式影响）+ M1m/M4m 连拍 taa_out alpha 出**历史被拒"
        "比例**（门 `taa.diag_reject`，RGB 逐位 = M1/M4）；只出数 + 一句判决，不改渲染\n"
        "  temporal-feedback [--camera doorframe] [--angle 0.2] [--burst 32] [--edge-pct 0.90]"
        " [--fb 0.02,0.05,0.12,0.25,0.5,0.8,1.0] [--kernel 0]  **运动态 feedback 扫描**：把「历史在非零分数"
        "偏移上被 CR 重采样 ⟹ 每帧一次低通、在 ≈1/α 窗内复利」这条假设变成数。fb=1 那格"
        "（零累积但仍带 jitter）拆开「累积」与「抖动」两个贡献；顺带在静止态扫 fb 出 swing，"
        "判「窗棂残余与运动失效是否同一个根」。每组回显**引擎自己报的**生效 feedback"
        "（`GET /state`）+ 同进程 /param A/B 字节差 = 参数生效自证；只出数，不改引擎默认值。"
        "S0 = 静止 TAA 关地板（批 13.8 第 0 步：静止态 dev(fb=1)/dev(S0) 与运动态 R(fb=1) 同构，"
        "两边同 ~4% ⟹ 那 4% 与运动无关）；--kernel = 批 13.8 输入侧重建核\n"
        "  temporal-flicker [--target edge] [--sweep 0.125] [--frames 32] [--halfwidth 8]"
        " [--rowfrac 0.14] [--verbose] [--kernel 0]  **相机移动下的时间闪烁（闭式解判据）**：相机帧驱动 yaw"
        " 以亚像素扫速扫过 1px 高对比边缘，量「每帧最大变化」（p99.5，线性域），与**闭式解**两端"
        "对拍——理想 AA（覆盖线性斜坡）给下界、完全无 AA（阶梯整段跳变）给上界，于是 G=实测/理想"
        "与 ζ=(实测−理想)/(无AA−理想) 有绝对刻度，不靠「动得慢」刷分。--target edge = aa_target"
        " 白板左边缘（闭式适用，主判据）；--target <bistro 机位名> = 交付配置口径（闭式只作参考"
        "刻度）。TAA 开/关各一组，出 flicker(on)/flicker(off)。FFT 只作诊断打印——高频带能量比"
        "已被闭式对照否决（混叠把锯齿变成方波），理由见 temporal_flicker.h。"
        "--kernel = 批 13.8 输入侧重建核（0 双线性 / 1 CR 4×4 / 2 Lanczos3），**只作用于 TAA 开组**"
        "⟹ TAA 关组 = 闭式无 AA 锚点，跨核重测它必须不动（ζ 的主判据）\n"
        "  temporal-homography [--target edge] [--sweep 0.125] [--frames 16] [--out 前缀]"
        "  **精确单应 warp 的逐像素闪烁归因图**：相机做原地绕 up 轴 yaw ⟹ 像面映射是纯旋转"
        "单应（与深度无关、对整幅画面精确），由 tan(Δψ)+焦距+主点**闭式**给出——零参考图、零"
        "对齐搜索、零 velocity buffer。把上一帧按它搬到本帧网格，剩下的差就是「搬不动的部分」"
        "（= 采样格移动的锯齿差 / TAA 时域滤波做的事）。同批出**往返地板** |I−H⁻¹(H·I)| 判"
        "「是量具在响还是真闪烁」，出**解释率** 1−残差/原始帧差，出固定标尺热图 PNG（TAA 开/关"
        "两张可直接对眼）。yaw 符号用 t 与 −t 两个闭式假设对拍取小者，两个残差都回显\n"
        "  ao-noise [--glb cornell_box.glb] [--camera front] [--variant deferred] [--strength 0.5]"
        " [--radius 40] [--pairs 16] [--no-temporal] [--bistro]  SSAO 噪声四组量测：闭式契约区偏离 / 分布 /"
        "亚像素爬行 / TAA 时域（同进程 /param A/B）\n"
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
                P.get(args, "--allow-piece", "ground"), P.has(args, "--env"));
        if (cmd == "izb")
            return cmdIzb(P.get(args, "--glb", "izb_rig.glb"),
                P.get(args, "--camera", "ladder"));
        if (cmd == "swim")
            return cmdSwim(P.get(args, "--glb", "izb_rig.glb"),
                P.get(args, "--camera", "ladder"), std::stod(P.get(args, "--dx", "0.06")),
                P.has(args, "--izb"), P.has(args, "--evssm"));
        if (cmd == "probe") {
            const auto parse3 = [](const std::string& s) {
                std::array<double, 3> v{0, 0, 0};
                std::stringstream ss(s);
                std::string tok;
                for (size_t i = 0; i < 3 && std::getline(ss, tok, ','); ++i)
                    v[i] = std::stod(tok);
                return v;
            };
            // --light = **指向光源**的方向 L（不是 scene.lua 的 direction）。默认值
            // 与 qa/configs/analytic 的 direction = (0.642788,0,-0.766044) 配对。
            const auto a = parse3(P.get(args, "--albedo", "0,0,1"));
            const auto lv = parse3(P.get(args, "--light", "-0.642788,0,0.766044"));
            return cmdProbe(projectRoot() + "/" + P.get(args, "--config", "qa/configs/analytic"),
                std::stoi(P.get(args, "--x", "640")), std::stoi(P.get(args, "--y", "360")),
                {a[0], a[1], a[2]}, std::stod(P.get(args, "--rough", "0.9")),
                std::stod(P.get(args, "--metal", "0")), {lv[0], lv[1], lv[2]},
                std::stod(P.get(args, "--intensity", "12")),
                std::stoi(P.get(args, "--tol", "2")),
                P.has(args, "--light") || P.has(args, "--intensity"));
        }
        if (cmd == "temporal")
            return cmdTemporal(std::stoi(P.get(args, "--pairs", "32")),
                std::stoi(P.get(args, "--warm", "32")), P.get(args, "--variant", ""),
                P.has(args, "--base-space"));
        if (cmd == "temporal-residual")
            return cmdTemporalResidual(std::stoi(P.get(args, "--periods", "2")),
                std::stod(P.get(args, "--warm-multiplier", "6")), P.get(args, "--variant", ""),
                std::stod(P.get(args, "--slow-feedback", "0.002")), P.has(args, "--skip-slow"),
                P.has(args, "--base-space"), std::stod(P.get(args, "--static-boost", "0")),
                P.has(args, "--bistro"), P.get(args, "--camera", "all"),
                std::stod(P.get(args, "--boost-floor", "0.01")), P.has(args, "--diag-bs-off"));
        if (cmd == "temporal-motion")
            return cmdTemporalMotion(P.get(args, "--camera", "doorframe"),
                P.get(args, "--motion", "camera"),
                std::stod(P.get(args, "--angle", "0.2")),
                std::stoi(P.get(args, "--burst", "32")),
                std::stoi(P.get(args, "--warm", "16")),
                std::stoi(P.get(args, "--warm-still", "64")),
                std::stod(P.get(args, "--edge-pct", "0.90")));
        if (cmd == "temporal-feedback") {
            std::vector<double> fbs;
            std::stringstream fs(P.get(args, "--fb", "0.02,0.05,0.12,0.25,0.5,0.8,1.0"));
            for (std::string t; std::getline(fs, t, ',');)
                if (!t.empty())
                    fbs.push_back(std::stod(t));
            return cmdTemporalFeedback(P.get(args, "--camera", "doorframe"),
                std::stod(P.get(args, "--angle", "0.2")),
                std::stoi(P.get(args, "--burst", "32")),
                std::stod(P.get(args, "--edge-pct", "0.90")), fbs,
                std::stod(P.get(args, "--kernel", "0")));
        }
        if (cmd == "temporal-flicker")
            return cmdTemporalFlicker(P.get(args, "--target", "edge"),
                std::stod(P.get(args, "--sweep", "0.125")),
                std::stoi(P.get(args, "--frames", "32")),
                std::stod(P.get(args, "--halfwidth", "8")),
                std::stod(P.get(args, "--rowfrac", "0.14")),
                P.has(args, "--verbose"), std::stod(P.get(args, "--kernel", "0")));
        if (cmd == "temporal-homography")
            return cmdTemporalHomography(P.get(args, "--target", "edge"),
                std::stod(P.get(args, "--sweep", "0.125")),
                std::stoi(P.get(args, "--frames", "16")),
                P.get(args, "--out", ""));
        if (cmd == "aa-coverage")
            return cmdAaCoverage(P.get(args, "--variant", ""), std::stoi(P.get(args, "--warm", "32")),
                std::stod(P.get(args, "--feedback", "0.12")), P.has(args, "--base-space"),
                std::stod(P.get(args, "--kernel", "0")));
        if (cmd == "taa-swim")
            return cmdTaaSwim(std::stoi(P.get(args, "--pairs", "32")),
                std::stoi(P.get(args, "--warm", "32")), P.get(args, "--variant", ""));
        if (cmd == "ao-noise")
            return cmdAoNoise(P.get(args, "--glb", "cornell_box.glb"),
                P.get(args, "--camera", "front"), P.get(args, "--variant", "deferred"),
                std::stod(P.get(args, "--strength", "0.5")),
                std::stoi(P.get(args, "--radius", "40")),
                std::stoi(P.get(args, "--pairs", "16")), !P.has(args, "--no-temporal"),
                P.has(args, "--bistro"));
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
