//
// material_attribution.cpp — objectID → 材质名 → 残差份额（批 13.9d）
//
// 设计理由与 objectID 语义映射链见 `material_attribution.h` 头注释。本文件只写实现，
// 两条**不能改**的纪律在原地重复一遍：
//   1. 认不出名字一律 `unnamed#<oid>`，**不许安错名字**；
//   2. 计数份额与幅度份额**同时出**（只出加权的那个 = 未完成）。
//
#include "material_attribution.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <stdexcept>
#include <cstdio>

#include <nlohmann/json.hpp>

namespace RGL::imgqa {
namespace {

using nlohmann::json;

std::string d2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f%%", v * 100.0);
    return b;
}
std::string d4(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", v);
    return b;
}

/// glb chunk 头里的 JSON 类型标记（`GLB_CHUNK_TYPE_JSON = 0x4E4F534A`，小端 "JSON"）。
constexpr uint32_t kChunkJson = 0x4E4F534A;

/// 读 glb 的 JSON chunk 原文。逐 chunk **找** JSON（chunk 1 = BIN，必须跳过；顺序不保证，
/// 所以是"找"不是"读第二段"）。任何一步读不出来都抛（调用方不许静默给空表）。
std::string readGlbJsonChunk(const std::string& glbPath)
{
    std::ifstream in(glbPath, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到 glb（材质归因需要它当 objectID → 名字的真相源）: " + glbPath);
    // 12 字节头：magic 'glTF' | version | totalLength
    uint8_t head[12] = {};
    in.read(reinterpret_cast<char*>(head), 12);
    if (!in || std::memcmp(head, "glTF", 4) != 0)
        throw std::runtime_error("不是 glb（头不是 'glTF'）: " + glbPath);
    std::string jsonText;
    while (in) {
        uint32_t clen = 0, ctype = 0;
        in.read(reinterpret_cast<char*>(&clen), 4);
        in.read(reinterpret_cast<char*>(&ctype), 4);
        if (!in)
            break;
        if (ctype == kChunkJson) {
            std::string buf(clen, '\0');
            in.read(buf.data(), static_cast<std::streamsize>(clen));
            jsonText = std::move(buf);
            break;
        }
        in.seekg(clen, std::ios::cur);  // 4 字节对齐的填充由长度本身吸收，不用额外处理
    }
    if (jsonText.empty())
        throw std::runtime_error("glb 里没有 JSON chunk: " + glbPath);
    return jsonText;
}

/// JSON chunk 原文 → 可查的 json（坏 JSON / 没有 meshes[] 都抛，meshes 是名字的出处）。
json parseGlbJson(const std::string& glbPath)
{
    const json j = json::parse(readGlbJsonChunk(glbPath), nullptr, false);
    if (j.is_discarded())
        throw std::runtime_error("glb 的 JSON chunk 解析失败: " + glbPath);
    if (!j.contains("meshes") || !j.at("meshes").is_array())
        throw std::runtime_error("glb 没有 meshes[]，无法建立 objectID → 材质映射: " + glbPath);
    return j;
}

/// objectID → 材质名（materials[mi].name；无索引 / 越界 = no-material，无名 = mat<mi>）。
/// ⚠ 与下面 `glbMeshName` 的 name 查询同形但**别合并成公共 helper**：两处回落名不同，
///   且 `"name"` 的字面量条数参与本文件的文案对账（合并 = 少三条，对账读成"文案被动过"）。
std::string glbMaterialName(const json& j, int32_t mi)
{
    if (mi < 0 || !j.contains("materials") || !j.at("materials").is_array()
        || mi >= static_cast<int32_t>(j.at("materials").size()))
        return "no-material";
    const auto& m = j.at("materials")[static_cast<size_t>(mi)];
    if (m.is_object() && m.contains("name") && m.at("name").is_string())
        return m.at("name").get<std::string>();
    return "mat" + std::to_string(mi);
}

/// mesh 名（meshes[mi].name；无名 = mesh<mi>）。
std::string glbMeshName(const json& m, size_t mi)
{
    if (m.is_object() && m.contains("name") && m.at("name").is_string())
        return m.at("name").get<std::string>();
    return "mesh" + std::to_string(mi);
}

/// primitive 的 material 索引（非对象 / 缺字段 / 非整数 = -1，与"无材质"同一个表示）。
int32_t glbMaterialIndex(const json& p)
{
    if (!p.is_object() || !p.contains("material") || !p.at("material").is_number_integer())
        return -1;
    return p.at("material").get<int32_t>();
}

/// 一个 primitive 的身份。index 由调用方按摊平顺序给（= 引擎分配的 objectSlot）。
GlbPrim glbPrimIdentity(const json& j, const json& p, const std::string& mesh, int32_t index)
{
    GlbPrim g;
    g.index = index;
    g.mesh = mesh;
    g.materialIndex = glbMaterialIndex(p);
    g.material = glbMaterialName(j, g.materialIndex);
    return g;
}

/// 单个 objectID 的累加器（先按 oid 累加，再按材质名合并）。
struct OidAcc {
    int64_t pixels = 0, over = 0;
    double mass = 0.0;
};

/// 全帧分母（三个份额的除数都来自这里）。
struct FrameTotals {
    int64_t overAll = 0;
    double massAll = 0.0;
    int64_t pxAll = 0;
};

/// 逐像素按 oid 累加残差（oid 上界 = max(prims.size(), 实测最大 id)+1，比先扫一遍求 max 少一趟）。
std::map<int32_t, OidAcc> accumulateByOid(const cv::Mat& ids, const cv::Mat& resid, double tau,
    FrameTotals& totals)
{
    std::map<int32_t, OidAcc> byOid;
    for (int r = 0; r < ids.rows; ++r) {
        const int32_t* ip = ids.ptr<int32_t>(r);
        const double* rp = resid.ptr<double>(r);
        for (int c = 0; c < ids.cols; ++c) {
            const int32_t oid = ip[c];
            if (oid <= 0)
                continue;  // 0 = 背景（没有 primitive 写进这一格），不属于任何材质
            const double v = rp[c];
            OidAcc& a = byOid[oid];
            ++a.pixels;
            a.mass += v;
            if (v > tau)
                ++a.over;
            ++totals.pxAll;
            totals.massAll += v;
            if (v > tau)
                ++totals.overAll;
        }
    }
    return byOid;
}

/// 材质级累加器（同一材质常有几十个 primitive，故还要按材质名并一次）。
struct MatAcc {
    int64_t pixels = 0, over = 0, prims = 0;
    double mass = 0.0;
    std::string exampleMesh;
    double exampleMass = -1.0;
};

/// oid → 材质名（认不出 = unnamed#<oid>），再按材质名合并。
std::map<std::string, MatAcc> mergeByMaterial(
    const std::map<int32_t, OidAcc>& byOid, const std::vector<GlbPrim>& prims)
{
    std::map<std::string, MatAcc> byMat;
    for (const auto& [oid, a]: byOid) {
        const bool known = static_cast<size_t>(oid) < prims.size();
        const GlbPrim& g = known ? prims[static_cast<size_t>(oid)] : GlbPrim{};
        const std::string mat = known ? g.material : "unnamed#" + std::to_string(oid);
        MatAcc& m = byMat[mat];
        m.pixels += a.pixels;
        m.over += a.over;
        m.mass += a.mass;
        ++m.prims;
        // exampleMesh 取"该材质下残差最大的那个 primitive 的 mesh 名"，方便下一步定位
        if (a.mass > m.exampleMass) {
            m.exampleMass = a.mass;
            m.exampleMesh = known ? g.mesh : ("oid" + std::to_string(oid));
        }
    }
    return byMat;
}

/// 一行判据数字：面积/计数/幅度**三个份额同时出**（只出加权的那个 = 未完成），分母全来自全帧。
MaterialRow materialRow(const std::string& name, const MatAcc& m, const FrameTotals& totals,
    double floorMean)
{
    MaterialRow r;
    r.material = name;
    r.exampleMesh = m.exampleMesh;
    r.primsVisible = m.prims;
    r.pixels = m.pixels;
    r.over = m.over;
    r.mass = m.mass;
    r.pixelShare = totals.pxAll > 0
        ? static_cast<double>(m.pixels) / static_cast<double>(totals.pxAll)
        : 0.0;
    r.cntShare = totals.overAll > 0
        ? static_cast<double>(m.over) / static_cast<double>(totals.overAll)
        : 0.0;
    r.ampShare = totals.massAll > 0.0 ? m.mass / totals.massAll : 0.0;
    r.rate = m.pixels > 0 ? static_cast<double>(m.over) / static_cast<double>(m.pixels) : 0.0;
    r.meanResid = m.pixels > 0 ? m.mass / static_cast<double>(m.pixels) : 0.0;
    r.residOverFloor = floorMean > 0.0 ? r.meanResid / floorMean : 0.0;
    return r;
}

}  // namespace

std::vector<GlbPrim> parseGlbPrimitives(const std::string& glbPath)
{
    const json j = parseGlbJson(glbPath);

    // ⚠ 摊平顺序 = **mesh 为主序、primitive 为次序**，且按 meshes[] 的文档序。
    //   这条必须与引擎的 glTF 加载器一致（消费方 = model_importer_impl.cppm:359 的 objectSlot
    //   计数）。两边一旦不同序，表里的名字会**全部安错**而数字照样好看 —— 所以调用方必须
    //   回显「解析到 N 个 primitive」并与实测 max(objectID)+1 对账（对不上就是序不同）。
    std::vector<GlbPrim> out;
    const auto& meshes = j.at("meshes");
    for (size_t mi = 0; mi < meshes.size(); ++mi) {
        const auto& m = meshes[mi];
        std::string name = glbMeshName(m, mi);
        if (!m.is_object() || !m.contains("primitives") || !m.at("primitives").is_array())
            continue;
        const auto& prims = m.at("primitives");
        for (const auto& p: prims)
            out.push_back(glbPrimIdentity(j, p, name, static_cast<int32_t>(out.size())));
    }
    if (out.empty())
        throw std::runtime_error("glb 的 meshes[] 里一个 primitive 都没有: " + glbPath);
    return out;
}

std::vector<MaterialRow> materialAttribution(const cv::Mat& ids, const cv::Mat& resid,
    const std::vector<GlbPrim>& prims, double tau, double floorMean)
{
    if (ids.empty() || resid.empty() || ids.size() != resid.size()
        || ids.type() != CV_32SC1 || resid.type() != CV_64FC1)
        throw std::runtime_error("materialAttribution：ids/resid 形状或类型不符");

    FrameTotals totals;
    const auto byOid = accumulateByOid(ids, resid, tau, totals);
    if (byOid.empty() || totals.massAll <= 0.0)
        return {};

    const auto byMat = mergeByMaterial(byOid, prims);
    std::vector<MaterialRow> rows;
    rows.reserve(byMat.size());
    for (const auto& [name, m]: byMat)
        rows.push_back(materialRow(name, m, totals, floorMean));
    // **按幅度份额降序**：用户问的是"什么在驱动闪烁"，驱动 = 幅度贡献，不是面积。
    std::sort(rows.begin(), rows.end(),
        [](const MaterialRow& a, const MaterialRow& b) { return a.ampShare > b.ampShare; });
    return rows;
}

void printMaterialTable(const std::string& title, const std::vector<MaterialRow>& rows,
    size_t topN, double tau, double floorMean, const std::string& residualName)
{
    std::printf("  ┌ %s\n", title.c_str());
    std::printf("    被分组的量 = %s；分组键 = objectID → glb primitive → **材质名**\n",
        residualName.c_str());
    std::printf("    τ = %s ｜ 全帧地板 = %s（`残差/地板` ≈ 1 的格子即使份额高也**只是重采样**）\n",
        d4(tau).c_str(), d4(floorMean).c_str());
    std::printf("    %-34s %5s %8s %9s %9s %9s %9s %8s\n", "材质", "prim", "面积份额", "计数份额",
        "幅度份额", "over率", "残差/地板", "最大mesh");
    size_t n = std::min(topN, rows.size());
    double ampShown = 0.0, cntShown = 0.0, pxShown = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const MaterialRow& r = rows[i];
        std::string nm = r.material;
        if (nm.size() > 34)
            nm = nm.substr(0, 31) + "...";
        std::string ex = r.exampleMesh;
        if (ex.size() > 22)
            ex = ex.substr(0, 19) + "...";
        std::printf("    %-34s %5lld %7s %8s %8s %8s %9s %8s\n", nm.c_str(),
            static_cast<long long>(r.primsVisible), d2(r.pixelShare).c_str(),
            d2(r.cntShare).c_str(), d2(r.ampShare).c_str(), d2(r.rate).c_str(),
            d2(r.residOverFloor).c_str(), ex.c_str());
        ampShown += r.ampShare;
        cntShown += r.cntShare;
        pxShown += r.pixelShare;
    }
    if (rows.size() > n) {
        double ampRest = 0.0, cntRest = 0.0, pxRest = 0.0;
        for (size_t i = n; i < rows.size(); ++i) {
            ampRest += rows[i].ampShare;
            cntRest += rows[i].cntShare;
            pxRest += rows[i].pixelShare;
        }
        std::printf("    %-34s %5s %7s %8s %8s %8s %9s %8s\n",
            ("── 其余 " + std::to_string(rows.size() - n) + " 个材质合计").c_str(), "-",
            d2(pxRest).c_str(), d2(cntRest).c_str(), d2(ampRest).c_str(), "-", "-", "-");
    }
    std::printf("    %-34s %5s %7s %8s %8s\n", "TOP 合计", "-", d2(pxShown).c_str(),
        d2(cntShown).c_str(), d2(ampShown).c_str());
    std::printf("    读法：**幅度份额**（Σ残差）才是「谁在驱动闪烁」，面积份额大只说明它占屏幕。\n");
    std::printf("    ⚠ 材质名来自 glb（唯一真相源），objectID 越界的退化成 unnamed#<oid> ——\n");
    std::printf("       表里若出现 unnamed，说明 objectID 序与 glb 摊平序**不一致**，名字不可信。\n");
}

}  // namespace RGL::imgqa
