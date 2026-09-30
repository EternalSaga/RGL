// 闭式解：analytic_ref.py 的 C++ 转录（逐式对账 shader；Python 原版已于 12.3b 退役删除，
// 本文件是唯一参考实现——「两处转录互相矛盾」的 D1 双源债就此清偿）。
// 独立第二实现——不 import 引擎任何代码。锚点见 selftest.cpp（本机 RADV 实测）。
#include "imgqa.h"

#include <algorithm>
#include <cmath>

namespace RGL::imgqa {

Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 operator*(Vec3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 normalize(Vec3 a)
{
    const double len = std::sqrt(dot(a, a));
    return {a.x / len, a.y / len, a.z / len};
}

// 与 tonemap.frag.slang 逐元素一致的矩阵（转置存放，配 mul(x,M) 方向）。
// 内层花括号 = 行；「中性输入不变」要求列和 = 1（selftest 有断言）。
namespace {
constexpr double kAcesIn[3][3] = {
    {0.59719, 0.07600, 0.02840},
    {0.35458, 0.90834, 0.13383},
    {0.04823, 0.01566, 0.83777},
};
constexpr double kAcesOut[3][3] = {
    {1.60475, -0.10208, -0.00327},
    {-0.53108, 1.10813, -0.07276},
    {-0.07367, -0.00605, 1.07602},
};
constexpr double kPi = 3.14159265358979323846;

/// 对角向量 × 矩阵（shader 的 mul(x, M) 同向：结果第 j 分量 = Σ_i x_i·M[i][j]）。
struct D3 {
    double v[3];
};
D3 mulVecMat(D3 x, const double m[3][3])
{
    return D3{{x.v[0] * m[0][0] + x.v[1] * m[1][0] + x.v[2] * m[2][0],
        x.v[0] * m[0][1] + x.v[1] * m[1][1] + x.v[2] * m[2][1],
        x.v[0] * m[0][2] + x.v[1] * m[1][2] + x.v[2] * m[2][2]}};
}
}  // namespace

Vec3 shadeDirect(Vec3 n, Vec3 v, Vec3 l, Vec3 albedo, double roughness, double metallic,
    Vec3 lightColor)
{
    // 逐行对应 model_pbr.frag.slang shadeDirect。
    Vec3 f0{0.04 + (albedo.x - 0.04) * metallic, 0.04 + (albedo.y - 0.04) * metallic,
        0.04 + (albedo.z - 0.04) * metallic};  // lerp(float3(0.04), albedo, m)

    const double ndotl = std::max(0.0, dot(n, l));   // saturate(dot(N,L))
    const double ndotv = std::max(dot(n, v), 1e-4);  // max(dot(N,V), 1e-4)
    const Vec3 h = normalize(v + l);
    const double ndoth = std::max(0.0, dot(n, h));  // saturate
    const double vdoth = std::max(0.0, dot(v, h));  // saturate

    const double a = roughness * roughness;
    const double a2 = a * a;
    const double d = ndoth * ndoth * (a2 - 1.0) + 1.0;
    const double dd = kPi * d * d + 1e-7 * a2;  // 相对 epsilon（绝对 epsilon 削掉高光峰）
    const double dist = a2 / dd;
    const double k = a * 0.5;
    const double gspec
        = (ndotv / (ndotv * (1.0 - k) + k)) * (ndotl / (ndotl * (1.0 - k) + k));
    const auto fres = [&](double f0c) { return f0c + (1.0 - f0c) * std::pow(1.0 - vdoth, 5); };
    const double inv = 1.0 / (4.0 * ndotv * ndotl + 1e-4);
    Vec3 spec{dist * gspec * fres(f0.x) * inv, dist * gspec * fres(f0.y) * inv,
        dist * gspec * fres(f0.z) * inv};
    Vec3 diff{albedo.x * (1.0 - metallic) / kPi, albedo.y * (1.0 - metallic) / kPi,
        albedo.z * (1.0 - metallic) / kPi};
    Vec3 term{diff.x + spec.x, diff.y + spec.y, diff.z + spec.z};
    return {lightColor.x * ndotl * term.x, lightColor.y * ndotl * term.y,
        lightColor.z * ndotl * term.z};
}

Vec3 acesTonemap(Vec3 x)
{
    const D3 v{{x.x, x.y, x.z}};
    const D3 vv = mulVecMat(v, kAcesIn);
    D3 q{{vv.v[0] * (vv.v[0] + 0.0245786) - 0.000090537,
        vv.v[1] * (vv.v[1] + 0.0245786) - 0.000090537,
        vv.v[2] * (vv.v[2] + 0.0245786) - 0.000090537}};
    const D3 den{{vv.v[0] * (0.983729 * vv.v[0] + 0.432951) + 0.238081,
        vv.v[1] * (0.983729 * vv.v[1] + 0.432951) + 0.238081,
        vv.v[2] * (0.983729 * vv.v[2] + 0.432951) + 0.238081}};
    q.v[0] /= den.v[0];
    q.v[1] /= den.v[1];
    q.v[2] /= den.v[2];
    const D3 out = mulVecMat(q, kAcesOut);
    const auto sat = [](double c) { return std::min(1.0, std::max(0.0, c)); };
    return {sat(out.v[0]), sat(out.v[1]), sat(out.v[2])};
}

double srgbEncode(double c)
{
    // shader **不做** pow(1/2.2)——硬件 sRGB store 做。回读拿到的是编码后字节，
    // 闭式解算的是线性值，不补这一步两者天生差一个传输函数。
    c = std::min(1.0, std::max(0.0, c));
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

double srgbDecode(double c)
{
    c = std::min(1.0, std::max(0.0, c));
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

std::array<int, 3> toBytes(Vec3 linear, double exposure)
{
    const Vec3 mapped = acesTonemap(linear * exposure);
    const auto f = [](double c) { return static_cast<int>(std::lround(srgbEncode(c) * 255.0)); };
    return {f(mapped.x), f(mapped.y), f(mapped.z)};
}

double observedLuma(const std::array<int, 3>& byte)
{
    // Rec.709 权重在**解完 sRGB 的线性域**求和——观测像素与预测字节走同一条路，
    // 「比较域」在整套工具里只有这一个出处（文件头硬约束第 3 条）。
    return 0.2126 * srgbDecode(byte[0] / 255.0) + 0.7152 * srgbDecode(byte[1] / 255.0)
        + 0.0722 * srgbDecode(byte[2] / 255.0);
}

std::vector<FacePrediction> predictFaces(std::array<double, 3> lightDir,
    std::array<double, 3> albedo, double roughness, double metallic, double intensity,
    std::array<double, 3> camera, std::array<double, 3> center, double exposure)
{
    const Vec3 L = normalize(Vec3{-lightDir[0], -lightDir[1], -lightDir[2]});
    const Vec3 V = normalize(Vec3{camera[0] - center[0], camera[1] - center[1],
        camera[2] - center[2]});
    const Vec3 A{albedo[0], albedo[1], albedo[2]};
    const Vec3 lc{intensity, intensity, intensity};
    static constexpr const char* kNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
    static constexpr double kAxes[6][3] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    std::vector<FacePrediction> out;
    for (int i = 0; i < 6; ++i) {
        const Vec3 n{kAxes[i][0], kAxes[i][1], kAxes[i][2]};
        FacePrediction p;
        p.face = kNames[i];
        p.ndotl = std::max(0.0, dot(n, L));
        // 未被照到的面：NdotL=0 → 出射恒 0（shadeDirect 末尾 ×ndotl），直接给黑。
        // 「预测 0」必须在集合里——它用来解释观测到的暗簇是不是合法的背面。
        p.byte = p.ndotl > 0.0 ? toBytes(shadeDirect(n, V, L, A, roughness, metallic, lc),
            exposure)
                               : std::array<int, 3>{0, 0, 0};
        p.luma = observedLuma(p.byte);
        out.push_back(p);
    }
    return out;
}

}  // namespace RGL::imgqa
