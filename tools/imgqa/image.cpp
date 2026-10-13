// 帧解码 + id↔piece 反解 + 区域代数 + 线性域统计。
#include "imgqa.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

using nlohmann::json;

namespace RGL::imgqa {

namespace {
/// 短浮点格式化（conflicts 消息用，别让 0.35000000000000003 糊在警告里）。
std::string fmt3(double v)
{
    char buf[24];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    return buf;
}
}  // namespace

cv::Mat readPngBgr(const std::string& path)
{
    // **强制 3 通道**（IMREAD_COLOR）：object_id 图可能恰好没有 B=0 以外的字节，
    // 但灰度优化读法会让"图是几通道"取决于像素内容——判据入口不接受这种不确定性。
    cv::Mat img = cv::imread(path, cv::IMREAD_COLOR);
    if (img.empty())
        throw std::runtime_error("PNG 读不出来: " + path + "（读图器是判据的入口，坏了会被读成「渲染变了」）");
    return img;
}

cv::Mat decodeObjectId(const cv::Mat& bgr)
{
    CV_Assert(bgr.type() == CV_8UC3);
    cv::Mat ch[3];
    cv::split(bgr, ch);  // ch[0]=B=id[23:16] ch[1]=G=[15:8] ch[2]=R=id[7:0]
    cv::Mat ids;
    ch[2].convertTo(ids, CV_32SC1);
    cv::Mat t;
    ch[1].convertTo(t, CV_32SC1);
    ids += t * 256;
    ch[0].convertTo(t, CV_32SC1);
    ids += t * 65536;
    return ids;
}

void prepareLinear(Frame& f)
{
    // sRGB 解码 LUT（256 项）：全图逐像素调 pow 是 90 万次，LUT 后是一次查表。
    static const auto lut = [] {
        std::array<double, 256> t{};
        for (int i = 0; i < 256; ++i)
            t[i] = srgbDecode(i / 255.0);
        return t;
    }();
    f.lin.create(f.bgr.rows, f.bgr.cols, CV_64FC3);
    f.luma.create(f.bgr.rows, f.bgr.cols, CV_64FC1);
    const double* p = lut.data();
    auto* dst = reinterpret_cast<cv::Vec3d*>(f.lin.data);
    auto* lum = f.luma.ptr<double>();
    for (int y = 0; y < f.bgr.rows; ++y) {
        const auto* row = f.bgr.ptr<cv::Vec3b>(y);  // BGR 序
        for (int x = 0; x < f.bgr.cols; ++x, ++dst, ++lum) {
            (*dst)[0] = p[row[x][2]];  // 存成 R,G,B 语义（lin[0]=R）
            (*dst)[1] = p[row[x][1]];
            (*dst)[2] = p[row[x][0]];
            // Rec.709 线性亮度。observedLuma 同权重同域——「比较域」只有一个出处。
            *lum = 0.2126 * (*dst)[0] + 0.7152 * (*dst)[1] + 0.0722 * (*dst)[2];
        }
    }
}

Frame captureFrame(Engine& eng, const Engine::Camera& cam, const std::string& tag)
{
    eng.setCamera(cam);
    Frame f;
    f.camera = cam;
    f.bgr = readPngBgr(eng.shot(tag + "_color"));
    f.ids = decodeObjectId(readPngBgr(eng.shot(tag + "_objectid", "object_id")));
    if (f.bgr.size() != f.ids.size())
        throw std::runtime_error("color 与 object_id 分辨率不同——两图必然不同帧/不同机位");
    prepareLinear(f);
    return f;
}

Manifest loadManifest(const std::string& glbStem)
{
    const std::string path
        = projectRoot() + "/build/rgl/models/" + glbStem + ".manifest.json";
    std::ifstream in(path);
    if (!in)
        throw std::runtime_error("manifest 不存在: " + path);
    const json m = json::parse(in);
    Manifest out;
    for (const auto& p : m.at("pieces")) {
        ManifestPiece mp;
        mp.name = p.at("name").get<std::string>();
        mp.primitiveIndex = p.value("primitiveIndex", -1);
        mp.color = p.at("color").get<std::array<double, 3>>();
        mp.roughness = p.value("roughness", 0.9);
        mp.metallic = p.value("metallic", 0.0);
        mp.aabbMin = p.at("aabb").at("min").get<std::array<double, 3>>();
        mp.aabbMax = p.at("aabb").at("max").get<std::array<double, 3>>();
        out.pieces.push_back(std::move(mp));
    }
    for (const auto& [name, c] : m.at("cameras").items()) {
        Engine::Camera cam;
        cam.position = c.at("position").get<std::array<double, 3>>();
        cam.target = c.at("target").get<std::array<double, 3>>();
        cam.fovDegrees = c.value("fovDegrees", 60.0);
        out.cameras.emplace(name, cam);
    }
    return out;
}

std::vector<std::string> Attribution::visiblePieces() const
{
    std::vector<std::string> v;
    for (const auto& [oid, name] : nameByOid)
        v.push_back(name);
    return v;
}

namespace {
/// 亮像素平均色度（验证器输入的一对分量，全暗 = 两个 NaN）。
struct Chroma {
    double r, g;
};

/// 收集 mask 内像素的线性亮度并升序排序（分位数与暗像素占比都在排序结果上取）。
std::vector<double> sortedLumaInMask(const Frame& f, const cv::Mat& mask)
{
    std::vector<double> vals;
    vals.reserve(static_cast<size_t>(cv::countNonZero(mask)));
    for (int y = 0; y < f.luma.rows; ++y) {
        const auto* mrow = mask.ptr<uchar>(y);
        const double* lrow = f.luma.ptr<double>(y);
        for (int x = 0; x < f.luma.cols; ++x)
            if (mrow[x])
                vals.push_back(lrow[x]);
    }
    std::sort(vals.begin(), vals.end());
    return vals;
}

/// 把排序后的亮度序列读数填进 `PieceStat` 的四个观测字段（中位数 / p05 / p95 与暗像素
/// 占比；暗阈 `darkEps` 由调用方给，判据侧可调）。
void fillReadout(PieceStat& st, const std::vector<double>& sorted, double darkEps)
{
    st.medianLinear = percentile100(sorted, 50.0);
    st.p05Linear = percentile100(sorted, 5.0);
    st.p95Linear = percentile100(sorted, 95.0);
    st.darkFraction = static_cast<double>(
        std::count_if(sorted.begin(), sorted.end(),
            [&](double v) { return v < darkEps; }))
        / static_cast<double>(sorted.size());
}

/// 亮像素平均色度（**验证器输入**，命名者已换成顺序假设）。黑像素 0/0 无定义 ⟹ 只从
/// 亮像素取；全暗 id 得 NaN = 不校验（文件头硬约束）。
Chroma brightChroma(const Frame& f, const cv::Mat& mask, double chromaMinLuma)
{
    double cx = std::nan(""), cy = std::nan("");
    cv::Mat bright;
    cv::compare(f.luma, chromaMinLuma, bright, cv::CMP_GE);
    cv::Mat chromaMask;
    cv::bitwise_and(mask, bright, chromaMask);
    if (cv::countNonZero(chromaMask) > 0) {
        const cv::Scalar meanLin = cv::mean(f.lin, chromaMask);  // lin 是 R,G,B 语义
        const double s = meanLin[0] + meanLin[1] + meanLin[2];
        if (s > 1e-9) {
            cx = meanLin[0] / s;
            cy = meanLin[1] / s;
        }
    }
    return {cx, cy};
}

/// 色度弱校验（验证器角色，不当命名者）。**期望色度 = 闭式解最强受光面预测字节解完 sRGB
/// 的色度**——与观测同域，ACES 串扰已在预测里。最强面用 V=N 退化：漫反射色度与 V 无关，
/// specular 只加白不改色度方向。阈值 0.35：只有**方向性**偏离（分配顺序变了 / 材质颜色
/// 坏了）才触发。全暗 id 色度 NaN = 不校验（它正是待判对象，不冤）。
/// 偏离 ⟹ 登记进 `at.conflicts`（不 throw：现场必须先看到它看到的原始数据）。
void checkChromaOrdering(Attribution& at, int32_t oid, const std::string& piece, Chroma obs,
    const ManifestPiece& mp, std::array<double, 3> lightDir, double lightIntensity)
{
    const double cx = obs.r, cy = obs.g;
    const Vec3 L = normalize(Vec3{-lightDir[0], -lightDir[1], -lightDir[2]});
    static constexpr double kAxes[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    Vec3 bestFace{1, 0, 0};
    double bestNdl = -1.0;
    for (const auto& ax : kAxes) {
        const Vec3 n{ax[0], ax[1], ax[2]};
        if (const double ndl = dot(n, L); ndl > bestNdl) {
            bestNdl = ndl;
            bestFace = n;
        }
    }
    const auto predByte = toBytes(shadeDirect(bestFace, bestFace, L,
        Vec3{mp.color[0], mp.color[1], mp.color[2]}, mp.roughness, mp.metallic,
        Vec3{lightIntensity, lightIntensity, lightIntensity}));
    const double pr = srgbDecode(predByte[0] / 255.0)
        + srgbDecode(predByte[1] / 255.0) + srgbDecode(predByte[2] / 255.0);
    // 取反早返回（与原来的 `pr > 1e-9 && !std::isnan(cx)` 逐值等价，含 pr 为 NaN 的边角）。
    if (!(pr > 1e-9) || std::isnan(cx))
        return;
    const double ex = srgbDecode(predByte[0] / 255.0) / pr;
    const double ey = srgbDecode(predByte[1] / 255.0) / pr;
    if (const double d = std::hypot(cx - ex, cy - ey); d > 0.35)
        at.conflicts.push_back("id " + std::to_string(oid) + " 按序应为 "
            + piece + "，但观测色度 (" + fmt3(cx) + "," + fmt3(cy)
            + ") 距期望 (" + fmt3(ex) + "," + fmt3(ey) + ") 达 " + fmt3(d)
            + "——分配顺序假设或材质颜色之一变了");
}
}  // namespace

/// 排序序列上取百分位：下标 = q/100·(n−1) 截断，**不做线性插值**——观测与闭式解都按这条取，
/// 换插值口径 = 换一台仪器。⚠ 本函数的 `q` 是 **0..100**，与 `temporal_residual.h` 导出的
/// `percentileOf`（p = **0..1**）**不是同一个刻度**，所以刻意不叫那个名字——两个同名不同刻度的
/// 量在同一工具里 = 迟早有人传错（传错不报错，只是分位数静默变成 0 或最后一个）。
/// 空集**抛**（债-99）：`size() − 1` 下溢成 SIZE_MAX = 越界读，堆上的鬼数会被当分位数印出来；
/// 「无分位数」与「分位数是 0」必须可区分，所以不许返回 0 哨兵——抛是唯一与任何数都不同的形态。
double percentile100(const std::vector<double>& sorted, double q)
{
    if (sorted.empty())
        throw std::runtime_error(
            "percentile100：空集没有分位数（mask 全灭 = 区域归因与像素对不上？）。"
            "不许读成 0——0 是合法分位数，「无分位数」必须与它可区分");
    return sorted[static_cast<size_t>(q / 100.0 * (sorted.size() - 1))];
}

Attribution attribute(const Frame& f, const Manifest& man, std::array<double, 3> lightDir,
    double lightIntensity, double darkEps, double chromaMinLuma)
{
    CV_Assert(!f.ids.empty());
    // 唯一 id 集合。
    std::vector<int32_t> all(static_cast<size_t>(f.ids.total()));
    std::memcpy(all.data(), f.ids.data, f.ids.total() * sizeof(int32_t));
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());

    Attribution at;
    // ── 主映射：id = primitiveIndex + 1（顺序假设），**色度降级为验证器** ──
    // 依据：manifest mapping 字段声明 piece order == globalPrimitiveIndex，引擎
    // objectIndex 分配顺序 == primitive 注册顺序，picking shader 写 +1（0=背景）。
    // 为什么不信纯色度反解（本工具的教训实测）：ACES 饱和把色度移位 0.1~0.15，
    // 与 piece 间色度距离同量级——far_cube 橙被判成 ground、ground 本体色度从
    // (0.5,0.5) 漂到 (0.424,0.412)。顺序假设 + 色度弱校验 = 两个独立来源互证：
    // 分配规则变了 → 色度报；材质颜色坏了 → 色度也报。任一失效都是可检测信号，
    // 而不是静默错配。
    std::map<int32_t, const ManifestPiece*> byIndex;
    for (const auto& p : man.pieces)
        byIndex[static_cast<int32_t>(p.primitiveIndex)] = &p;

    for (int32_t oid : all) {
        if (oid == 0)  // 背景，不是 piece
            continue;
        cv::Mat mask;
        cv::compare(f.ids, oid, mask, cv::CMP_EQ);
        PieceStat st;
        st.oid = oid;
        st.pixels = cv::countNonZero(mask);
        fillReadout(st, sortedLumaInMask(f, mask), darkEps);
        const Chroma chroma = brightChroma(f, mask, chromaMinLuma);
        // 顺序命名（主映射注释在 unique 循环上方）。
        const auto pit = byIndex.find(oid - 1);
        if (pit != byIndex.end()) {
            st.piece = pit->second->name;
            checkChromaOrdering(
                at, oid, st.piece, chroma, *pit->second, lightDir, lightIntensity);
            at.nameByOid.emplace(oid, st.piece);
        } else {
            st.piece = "unnamed#" + std::to_string(oid);  // manifest 之外的 id
            at.unnamed.push_back(oid);
        }
        st.chromaR = chroma.r;  // 全暗 = NaN（输出可见，不装成 0）
        st.chromaG = chroma.g;
        at.stats.push_back(std::move(st));
    }
    return at;
}

cv::Mat maskOfPiece(const Frame& f, const Attribution& at, const std::string& name)
{
    std::vector<int32_t> oids;
    for (const auto& [oid, n] : at.nameByOid)
        if (n == name)
            oids.push_back(oid);
    cv::Mat mask = cv::Mat::zeros(f.ids.size(), CV_8UC1);
    for (int32_t oid : oids) {
        cv::Mat m;
        cv::compare(f.ids, oid, m, cv::CMP_EQ);
        cv::bitwise_or(mask, m, mask);
    }
    return mask;
}

cv::Mat dilateMask(const cv::Mat& mask, int k)
{
    cv::Mat out;
    const cv::Mat kernel
        = cv::getStructuringElement(cv::MORPH_RECT, {2 * k + 1, 2 * k + 1});
    cv::dilate(mask, out, kernel);
    return out;
}

cv::Mat ringMask(const cv::Mat& mask, int k)
{
    cv::Mat out;
    cv::bitwise_and(dilateMask(mask, k), ~mask, out);
    return out;
}

int connectedCount(const cv::Mat& mask)
{
    cv::Mat labels;
    return cv::connectedComponents(mask, labels) - 1;  // 0 是背景
}

cv::Mat anyChannelDiff(const cv::Mat& a, const cv::Mat& b)
{
    // compare 出逐元素逐通道的 NE 掩码后，必须**逐通道 bitwise_or 收成单通道**：
    // cv::reduce(axis=0) 看着更短，但它对多通道输入按通道独立归约、输出仍是多通道，
    // 下游 countNonZero 断言 cn == 1 当场炸（switch 判据首战抓到）。
    cv::Mat d;
    cv::compare(a, b, d, cv::CMP_NE);
    std::vector<cv::Mat> ch;
    cv::split(d, ch);
    cv::Mat any = ch[0];
    for (size_t i = 1; i < ch.size(); ++i)
        cv::bitwise_or(any, ch[i], any);
    return any;
}

std::vector<std::pair<double, int64_t>> lumaClusters(cv::InputArray lumaIn, double gap,
    double minFraction)
{
    // 空输入先判——reshape(empty) 直接抛（"matrix is not continuous"），
    // 而空簇是合法输入（piece 无像素时）。
    const cv::Mat src = lumaIn.getMat();
    if (src.empty())
        return {};
    std::vector<double> s;
    cv::Mat m = src.reshape(1, 1);
    s.assign(m.begin<double>(), m.end<double>());
    if (s.empty())
        return {};
    std::sort(s.begin(), s.end());
    std::vector<size_t> bounds{0};
    for (size_t i = 1; i < s.size(); ++i)
        if (s[i] - s[i - 1] > gap)
            bounds.push_back(i);
    bounds.push_back(s.size());
    std::vector<std::pair<double, int64_t>> out;
    for (size_t b = 0; b + 1 < bounds.size(); ++b) {
        const size_t lo = bounds[b], hi = bounds[b + 1];
        if (static_cast<double>(hi - lo) / static_cast<double>(s.size()) >= minFraction)
            out.emplace_back(s[(lo + hi) / 2], static_cast<int64_t>(hi - lo));
    }
    return out;
}

}  // namespace RGL::imgqa
