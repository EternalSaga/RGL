// shadow_edge_target.cpp —— `shadow-edge-ruler` 的**靶侧**：manifest measure 块读取 +
// 扫描线构造 + 「仪器闭式 vs 生成器预测」对账。缝 = 仪器内部（跨 TU 面在 shadow_edge_ruler.h）：
// 改测量接口形状动本文件，改臂/采集动 shadow_edge_ruler.cpp，改判决线动 _judge.cpp。
//
// 「区域来自几何」是 imgqa 的硬纪律：扫描线的世界起止、方向、真值影边全部来自 manifest
// （场景生成器留给测量方的接口），C++ 不重抄任何几何常量（重抄 = 第二真相，改靶必漂）。
//
// ## 变异红线（哪个变异让我红；实跑记录见交付说明）
//   · 删掉 captureSize 核对 → 偷改采集尺寸后，生成器的 assert ⑨（窗口在画内 + 30px）
//     没被断言过这个画幅，闭式取景前提静默失效（selftest 有一条「尺寸不符应抛」）。
//   · 删掉级联对账（requireCascadeModelAgreement）→ 公式链两个实现任一边漂了，误差曲线的
//     「本级纹素」刻度与台阶归因全建立在错的 texel 上；selftest 喂「splits 差 2%」应抛。
//   · 扫描线 from/to 与 vDir 的正交核对删掉 → v 子线不再平行于影缘，沿 v 平均从「无偏
//     去噪」变成「斜着摊宽」（头注口径层的存在理由），selftest 有一条「vDir 不垂直应抛」。
#include "shadow_edge_ruler.h"
#include "shadow_kernel_metric.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace RGL::imgqa {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

std::string readFileText(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到文件: " + p.string());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

std::array<double, 3> asVec3(const json& v, const std::string& what)
{
    if (!v.is_array() || v.size() != 3)
        throw std::runtime_error("manifest 的 " + what + " 不是 3 元数值数组——测量接口"
              "形状变了，本命令拒绝猜");
    return {v[0].get<double>(), v[1].get<double>(), v[2].get<double>()};
}

std::array<double, 2> asVec2(const json& v, const std::string& what)
{
    if (!v.is_array() || v.size() != 2)
        throw std::runtime_error("manifest 的 " + what + " 不是 2 元数值数组——测量接口"
              "形状变了，本命令拒绝猜");
    return {v[0].get<double>(), v[1].get<double>()};
}

}  // namespace

/**
 * 单块板（从 loadEdgeTarget 拆出 = 认知复杂度护栏；步骤顺序即原内联顺序，语义零改动）。
 * 正交核对与弧长都在这里——它们是「每板」判据，留在循环外会让 loadEdgeTarget 重新变胖。
 */
EdgePlate parseEdgePlate(const json& p, const std::array<double, 2>& vv, double step,
    const json& ms)
{
    EdgePlate e;
    e.name = p.at("name").get<std::string>();
    e.thicknessM = p.at("thicknessM").get<double>();
    e.farFace = {p.at("farFaceX").get<double>(), 0.0, p.at("farFaceZ").get<double>()};
    e.trueEdge = {p.at("trueEdgeX").get<double>(), 0.0, p.at("trueEdgeZ").get<double>()};
    const auto from = std::array<double, 3>{p.at("scanFromX").get<double>(), 0.0,
        p.at("scanFromZ").get<double>()};
    const auto to = std::array<double, 3>{p.at("scanToX").get<double>(), 0.0,
        p.at("scanToZ").get<double>()};
    e.scan = EdgeScanLine{from, to, {vv[0], 0.0, vv[1]}, {}, step};
    for (const auto& vl : ms.at("vLines"))
        e.scan.vOffsets.push_back(vl.get<double>());
    // 正交核对：扫描方向（from→to）必须 ⊥ vDir（都在 y=0 平面内）。平行 = v 子线
    // 沿扫描方向平移 = 把影缘摊宽（「沿 v 平均无偏」的前提破了）。
    const double dx = to[0] - from[0], dz = to[2] - from[2];
    const double dl = std::hypot(dx, dz);
    if (!(dl > 1e-9))
        throw std::runtime_error("板 " + e.name + " 的扫描线 from == to（窗口塌了）");
    const double dot = (dx * vv[0] + dz * vv[1]) / dl;
    if (std::abs(dot) > 1e-6)
        throw std::runtime_error("板 " + e.name + " 的扫描方向与 measure.v 不垂直"
              "（cos=" + std::to_string(dot) + "）——沿 v 平均会把影缘摊宽，闭式前提破了");
    // 真值影边在扫描线上的弧长（= (trueEdge − from) 在扫描方向上的投影；trueEdge
    // 本就在窗口中线上，投影即弧长）。
    e.sTrue = ((e.trueEdge[0] - from[0]) * dx + (e.trueEdge[2] - from[2]) * dz) / dl;
    return e;
}

/**
 * 读靶（缺文件 = 抛，不 SKIP）。measure 块的形状契约：
 * plates[] 每条给 thicknessM / farFace / trueEdge / scanFrom / scanTo（世界坐标）+
 * stepM / vLines（沿 v 的平行子线）；u/v 单位向量在 measure 顶层；cascadeModel 块是
 * 生成器的闭式预测（对账输入，不是求值输入）。
 */
EdgeTarget loadEdgeTarget()
{
    const fs::path dir = fs::path(projectRoot()) / "build/rgl/models";
    const std::string stem = kEdgeGlbStem;
    const fs::path glb = dir / (stem + ".glb");
    const fs::path manifestPath = dir / (stem + ".manifest.json");
    const fs::path meshlets = dir / (stem + ".glb.meshlets.bin");
    if (!fs::exists(glb))
        throw std::runtime_error("靶不存在: " + glb.string()
            + "\n  生成：python3 tools/qa_scenes.py --only shadow_edge_rig --out build/rgl/models"
              "（缺靶必须 FAIL，不许 SKIP）");
    if (!fs::exists(manifestPath))
        throw std::runtime_error("靶的 manifest 不存在: " + manifestPath.string()
            + "\n  只有 .glb 没有 manifest ⟹ 测量接口（measure 块）没有来源，拒绝猜几何");
    if (!fs::exists(meshlets))
        throw std::runtime_error("靶的簇产物不存在: " + meshlets.string()
            + "\n  生成：./build/tools/meshlet_pack/meshlet_pack " + glb.string() + " --out "
              + meshlets.string() + " --stats\n"
              "  缺簇产物 = 引擎只打一条 INFO 就回落旧路径（main_scene.cppm）⟹ 簇 LOD 根本不进"
              "阴影段 ⟹ 本仪器恒绿。闸门 ① 是运行期兜底，这里给构建期可归因的指路");
    const json m = json::parse(readFileText(manifestPath));
    if (!m.contains("measure") || !m.at("measure").is_object())
        throw std::runtime_error("manifest 里没有 measure 块——它是场景生成器留给本命令的"
              "测量接口；没有它就得在 C++ 里重抄几何常量（第二真相），拒绝");
    const json& ms = m.at("measure");
    EdgeTarget t;
    t.capture = {ms.at("captureSize")[0].get<int>(), ms.at("captureSize")[1].get<int>()};
    if (t.capture[0] != kEdgeWidth || t.capture[1] != kEdgeHeight)
        throw std::runtime_error("靶的 captureSize 是 " + std::to_string(t.capture[0]) + "x"
            + std::to_string(t.capture[1]) + "，量具钉死 " + std::to_string(kEdgeWidth) + "x"
            + std::to_string(kEdgeHeight) + "——生成器的取景 assert（窗口在画内 + 30px）按"
              "captureSize 算，偷改尺寸 = 在没被断言过的画幅上用闭式");
    const auto cam = ms.at("camera");
    t.camera.position = asVec3(cam.at("position"), "measure.camera.position");
    t.camera.target = asVec3(cam.at("target"), "measure.camera.target");
    t.camera.fovDegrees = ms.at("fovDegrees").get<double>();
    t.boundsCenter = asVec3(ms.at("sceneBounds").at("center"), "measure.sceneBounds.center");
    t.boundsRadius = ms.at("sceneBounds").at("radius").get<double>();
    const auto u = asVec2(ms.at("u"), "measure.u");
    const auto vv = asVec2(ms.at("v"), "measure.v");
    const double ulen = std::hypot(u[0], u[1]);
    if (std::abs(ulen - 1.0) > 1e-9)
        throw std::runtime_error("measure.u 不是单位向量（|u|=" + std::to_string(ulen)
            + "）——扫描线方向的世界米刻度就靠它，拒绝");
    const json& cm = ms.at("cascadeModel");
    for (int i = 0; i < 4; ++i) {
        t.splitsPredicted[i] = cm.at("splitsPredicted")[i].get<double>();
        t.texelMmPredicted[i] = cm.at("texelMmPredicted")[i].get<double>();
    }
    const auto step = ms.at("stepM").get<double>();
    const json& plates = ms.at("plates");
    if (!plates.is_array() || plates.empty())
        throw std::runtime_error("measure.plates 不是非空数组——板 = 自变量的载体，"
              "没有它本仪器无事可测（对空集合的判据恒真 = 假绿）");
    for (const auto& p : plates)
        t.plates.push_back(parseEdgePlate(p, vv, step, ms));
    return t;
}

/// 对账（相对容差 1%：两条 double 公式链只在 float/double 边界与三角函数实现上差，
/// 1e-2 之内的分歧只能是**公式抄错**，不是数值噪声；真漂了误差曲线的 texel 刻度与
/// 台阶归因都不可信，先停下）。
void requireCascadeModelAgreement(const EdgeTarget& t, const EdgeCascadeModel& m)
{
    const auto cmp = [&](const char* what, double mine, double theirs) {
        const double scale = std::max(1e-9, std::abs(theirs));
        if (std::abs(mine - theirs) / scale > 0.01)
            throw std::runtime_error(std::string("级联闭式对账失败：") + what + " 仪器算 "
                + std::to_string(mine) + "、生成器算 " + std::to_string(theirs)
                + "（相对差 > 1%）——公式链两个实现有一个漂了，误差曲线的纹素刻度与台阶"
                  "归因都不可信，先修实现再读数");
    };
    for (int c = 0; c < 4; ++c) {
        cmp("splits[", m.splits[c], t.splitsPredicted[c]);
        cmp("texelMm[", m.texelM[c] * 1000.0, t.texelMmPredicted[c]);
    }
}

}  // namespace RGL::imgqa
