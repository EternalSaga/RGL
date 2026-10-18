// selftest_blend_sort.cpp —— `blend-sort` 的**口径层**自检（债-132）。
//
// 全部**不启引擎、不读文件**：合成 BlendPiece 直接打 `blend_sort_metric.cpp` 的算子，
// 期望值全部来自闭式或已实测的读数。这一层是本任务的重点——没有它，`blend-sort` 这条
// 检查在 `just build-tools` 里等于零覆盖（只有插上设备跑一次才可能被看见），而"编译得过、
// 一条都没断言"恰恰是本项目反复红过的半吊子形态。
//
// 六组断言各钉一件事（逐条可红，反向用例写在各组末尾）：
//   ① 投影锚点 —— 算出的屏幕 x 边界必须命中实机游程 466/553/714/843（整条检查能拿解析
//      坐标去采样的前提；投影与引擎不一致时四条判据会一起红，而红得像是渲染坏了）
//   ② 合成闭式 —— 四个采样点的线性 HDR 精确相等（全是 2 的幂的有限和，没有舍入可争）
//   ③ 显示链金标准 —— toBytes 吃那四个线性值给出的字节
//      ⚠ 这条钉的是**工具的那份转录**（有人改了 `analytic.cpp` 的 ACES 常数会在这里红），
//        **不是引擎**——引擎那一半靠 GPU 上跑 `imgqa blend-sort`。两层不是一回事，别混。
//   ④ 判别力 —— 反序 42 / 预乘 95（与阈值 1 之间隔着 41 档），并有一条反向用例证明
//      这个数不是恒大于下限的摆设
//   ⑤ 三条拒跑 —— 排序键退化 / 机位不是精确 −Z 正视，都必须抛
//   ⑥ 挂号 —— 四条判据名必须已在注册表里（改名漏挂号 = verdictFor 运行期才炸，太晚）
#include "selftest.h"

#include "blend_sort_metric.h"
#include "imgqa.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace RGL::imgqa {

namespace {

constexpr int kW = 1280;
constexpr int kH = 720;
constexpr int kCy = kH / 2;

/// 一份与 `qa_scenes.py` 的 scene_blend_sort 同参数的合成输入（**不读 manifest**：
/// 本层要能在没有 build/rgl/models/ 的机器上跑，而读来的数只能证明"与生成器一样"）。
BlendPiece mkPiece(const std::string& name, std::array<double, 3> color, double alpha, bool blend,
    std::array<double, 3> translation, double half)
{
    BlendPiece p;
    p.name = name;
    p.color = color;
    p.alpha = alpha;
    p.blend = blend;
    p.translation = translation;
    p.aabbMin = {-half, -half, 0.0};
    p.aabbMax = {half, half, 0.0};
    return p;
}

std::vector<BlendPiece> target()
{
    return {
        mkPiece("backdrop", {0.0, 0.0, 1.0}, 1.0, false, {0.0, 0.0, -2.0}, 8.0),
        mkPiece("layer_far", {0.0, 1.0, 0.0}, 0.25, true, {-0.4, 0.0, -1.0}, 1.0),
        mkPiece("layer_near", {1.0, 0.0, 0.0}, 0.5, true, {0.4, 0.0, 0.0}, 1.0),
    };
}

BlendCamera camera()
{
    return { {0.0, 0.0, 6.0}, {0.0, 0.0, 0.0}, 45.0 };
}

Vec3 linearAt(const std::vector<BlendPiece>& pieces, int px, BlendOrder order,
    BlendModel model = BlendModel::NonPremultiplied)
{
    return blendLinearAt(pieces, camera(), px, kCy, kW, kH, order, model);
}

/// 线性值**精确**相等（本靶刻意只取 2 的幂：0/1 与 α∈{0.25,0.5} 的混合结果是有限和，
/// float64 里逐位精确 ⟹ 没有"要不要给容差"这个问题可争）。
bool isExact(const Vec3& v, double x, double y, double z)
{
    return v.x == x && v.y == y && v.z == z;
}

bool isBytes(const std::array<int, 3>& b, int r, int g, int bl)
{
    return b[0] == r && b[1] == g && b[2] == bl;
}

/// 两个线性值逐位相同？（`Vec3` 没有 `operator==`，而这里要问的正是**逐位**——本靶的
/// 全部依据是「2 的幂的有限和在 float64 里精确」，容差版会把这条依据抹掉。）
bool sameVec(const Vec3& a, const Vec3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

/// 像素边界口径：中心落在矩形内的第一个像素列 = `ceil(浮点边界 − 0.5)`。
int firstPixelInside(double edge)
{
    return static_cast<int>(std::ceil(edge - 0.5));
}

/// ① 投影锚点：屏幕 x 边界必须命中实机游程（2026-10-18 本机 strixhalo 同一行实测：
/// [0,465] 背景 · [466,552] 远层 · [553,713] 重叠 · [714,842] 近层 · [843,1279] 背景）。
void checkBlendProjectionAnchor(Checker& c)
{
    const auto pieces = target();
    const auto far = blendPixelSpan(pieces[1], camera(), kW, kH);
    const auto near = blendPixelSpan(pieces[2], camera(), kW, kH);
    const auto bg = blendPixelSpan(pieces[0], camera(), kW, kH);
    c.chk("blend-sort 投影：远层左边界 = 实机游程 466（±1）",
        std::abs(firstPixelInside(far.lo) - 466) <= 1, std::to_string(firstPixelInside(far.lo)));
    c.chk("blend-sort 投影：近层左边界 = 实机游程 553（±1）",
        std::abs(firstPixelInside(near.lo) - 553) <= 1, std::to_string(firstPixelInside(near.lo)));
    c.chk("blend-sort 投影：远层右边界 = 实机游程 714（±1）",
        std::abs(firstPixelInside(far.hi) - 714) <= 1, std::to_string(firstPixelInside(far.hi)));
    c.chk("blend-sort 投影：近层右边界 = 实机游程 843（±1）",
        std::abs(firstPixelInside(near.hi) - 843) <= 1, std::to_string(firstPixelInside(near.hi)));
    c.chk("blend-sort 投影：背景铺满整幅（左出画、右出画）",
        bg.lo < 0.0 && bg.hi > static_cast<double>(kW),
        "(" + std::to_string(bg.lo) + "," + std::to_string(bg.hi) + ")");
    // 排序代理 = node 原点（不是几何中心）：49.16 / 36.16 是 manifest 的 intent 字段写死的数。
    c.chk("blend-sort 排序键：远层 distanceSq = 49.16（node 原点，非几何中心）",
        std::abs(blendDistanceSq(pieces[1], camera()) - 49.16) < 1e-12,
        std::to_string(blendDistanceSq(pieces[1], camera())));
    c.chk("blend-sort 排序键：近层 distanceSq = 36.16 且严格小于远层",
        std::abs(blendDistanceSq(pieces[2], camera()) - 36.16) < 1e-12,
        std::to_string(blendDistanceSq(pieces[2], camera())));
    // 判据区采样点 = 交/差集中点（区域来自几何，不在图里找边界）。
    const auto regions = blendRegionSamples(pieces, camera(), kW, kH);
    c.chk("blend-sort 判据区：四个采样点 = 实测 509 / 633 / 778 / 1061",
        regions.at("far_only") == 509 && regions.at("overlap") == 633
            && regions.at("near_only") == 778 && regions.at("bg_only") == 1061,
        std::to_string(regions.at("far_only")) + "/" + std::to_string(regions.at("overlap"))
            + "/" + std::to_string(regions.at("near_only")) + "/"
            + std::to_string(regions.at("bg_only")));
    // 反向用例：机位带俯角 ⟹ 本算子覆盖不了，必须抛（静默按纯 −Z 投影 = 采样点量错几何）。
    c.chk("blend-sort 投影：机位带俯角 → 抛（只覆盖沿 −Z 精确正视）", throwsStd([&] {
        BlendCamera tilted = camera();
        tilted.target = {0.0, -1.0, 0.0};
        (void)blendPixelSpan(pieces[1], tilted, kW, kH);
    }));
    // 反向用例：片有 z 厚度 ⟹ "整片同一 depth" 这条前提没了，必须抛而不是给个近似 footprint。
    c.chk("blend-sort 投影：片不是局部 z=0 平面 → 抛", throwsStd([&] {
        auto thick = pieces[1];
        thick.aabbMax[2] = 0.5;
        (void)blendPixelSpan(thick, camera(), kW, kH);
    }));
    // 反向用例：面片跑到机位之后 ⟹ 它不在画面上，拿它算判据区 = 量错层。
    c.chk("blend-sort 投影：面片在机位之后 → 抛", throwsStd([&] {
        auto behind = pieces[1];
        behind.translation = {0.0, 0.0, 20.0};
        (void)blendPixelSpan(behind, camera(), kW, kH);
    }));
}

/// ② 合成闭式：四个采样点的线性 HDR（BackToFront + NonPremultiplied = 引擎契约）。
void checkBlendCompositeClosedForm(Checker& c)
{
    const auto pieces = target();
    c.chk("blend-sort 合成：bg_only 线性 = (0,0,1)（只有背景，精确）",
        isExact(linearAt(pieces, 1061, BlendOrder::BackToFront), 0.0, 0.0, 1.0));
    c.chk("blend-sort 合成：far_only 线性 = (0,0.25,0.75)（α=0.25 非预乘 over）",
        isExact(linearAt(pieces, 509, BlendOrder::BackToFront), 0.0, 0.25, 0.75));
    c.chk("blend-sort 合成：near_only 线性 = (0.5,0,0.5)（α=0.5 非预乘 over）",
        isExact(linearAt(pieces, 778, BlendOrder::BackToFront), 0.5, 0.0, 0.5));
    c.chk("blend-sort 合成：overlap 线性 = (0.5,0.125,0.375)（远先画、近后画）",
        isExact(linearAt(pieces, 633, BlendOrder::BackToFront), 0.5, 0.125, 0.375));
    // 反向用例（本任务正主的另一半）：次序换掉必须**不等**，且给出可解析的另一个值。
    // 0.25*(0,1,0) + 0.75*(0.5,0,0.5) = (0.375, 0.25, 0.375)。
    const auto flipped = linearAt(pieces, 633, BlendOrder::FrontToBack);
    c.chk("blend-sort 合成：overlap 反序线性 = (0.375,0.25,0.375)（与正序**不等**）",
        isExact(flipped, 0.375, 0.25, 0.375)
            && !sameVec(flipped, linearAt(pieces, 633, BlendOrder::BackToFront)));
    c.chk("blend-sort 合成：单层区不受次序影响（far_only 两序同值）",
        sameVec(linearAt(pieces, 509, BlendOrder::BackToFront),
            linearAt(pieces, 509, BlendOrder::FrontToBack)));
    // 反向用例：采样点落在背景之外 ⟹ 不透明层数 = 0，闭式要求恰好 1 层。
    c.chk("blend-sort 合成：采样点上没有背景 → 抛（不透明层数 ≠ 1）", throwsStd([&] {
        (void)blendLinearAt(pieces, camera(), 100000.0, static_cast<double>(kCy), kW, kH,
            BlendOrder::BackToFront, BlendModel::NonPremultiplied);
    }));
    // 反向用例：半透层不是恰好两层 ⟹ 四个判据区的定义本身塌了。
    c.chk("blend-sort 合成：三层半透 → 抛（判据区建立在两层上）", throwsStd([&] {
        auto three = pieces;
        three.push_back(mkPiece("layer_mid", {1.0, 1.0, 0.0}, 0.5, true, {0.0, 0.0, -0.5}, 1.0));
        (void)blendRegionSamples(three, camera(), kW, kH);
    }));
    c.chk("blend-sort 合成：两层 footprint 不相交 → 抛（overlap 区不存在）", throwsStd([&] {
        auto disjoint = pieces;
        disjoint[2].translation = {6.0, 0.0, 0.0};
        (void)blendRegionSamples(disjoint, camera(), kW, kH);
    }));
}

/// ③ 显示链金标准。⚠ 钉的是**工具的那份转录**（`analytic.cpp` 的 ACES 常数 + sRGB 编码 +
/// 8bit 取整），不是引擎——引擎那一半靠 GPU 上跑 `imgqa blend-sort` 与这四组字节对拍。
/// 有人改了 `analytic.cpp` 的常数会在这里红，而那时**四条 GPU 判据也会一起红**：本层先红，
/// 归因就是"工具的转录变了"，不是"渲染坏了"。
void checkBlendDisplayChain(Checker& c)
{
    const auto bg = toBytes(Vec3{0.0, 0.0, 1.0}, kBlendExposure);
    const auto far = toBytes(Vec3{0.0, 0.25, 0.75}, kBlendExposure);
    const auto near = toBytes(Vec3{0.5, 0.0, 0.5}, kBlendExposure);
    const auto over = toBytes(Vec3{0.5, 0.125, 0.375}, kBlendExposure);
    c.chk("blend-sort 显示链：(0,0,1) → (0,0,204)（工具转录，实机同值）",
        isBytes(bg, 0, 0, 204), std::to_string(bg[0]) + "," + std::to_string(bg[1]) + ","
            + std::to_string(bg[2]));
    c.chk("blend-sort 显示链：(0,0.25,0.75) → (0,113,187)",
        isBytes(far, 0, 113, 187),
        std::to_string(far[0]) + "," + std::to_string(far[1]) + "," + std::to_string(far[2]));
    c.chk("blend-sort 显示链：(0.5,0,0.5) → (158,0,159)",
        isBytes(near, 158, 0, 159),
        std::to_string(near[0]) + "," + std::to_string(near[1]) + ","
            + std::to_string(near[2]));
    c.chk("blend-sort 显示链：(0.5,0.125,0.375) → (162,73,141)",
        isBytes(over, 162, 73, 141),
        std::to_string(over[0]) + "," + std::to_string(over[1]) + "," + std::to_string(over[2]));
    // 变异那一侧的金标准：反序线性 → (142,115,142) = 把排序比较子 `>` 改成 `<` 后的**实机**
    // 读数（docs/透明混合管线_plan.md §5 刀3）。它把"工具的转录"与"实机的变异读数"钉在一起。
    c.chk("blend-sort 显示链：反序线性 (0.375,0.25,0.375) → (142,115,142) = 变异实测值",
        isBytes(toBytes(Vec3{0.375, 0.25, 0.375}, kBlendExposure), 142, 115, 142));
    // 反向用例：这四组字节必须**互不相同**，否则"命中闭式"分不出量的是哪个区。
    c.chk("blend-sort 显示链：四组金标准两两不同（命中其一不等于命中全部）",
        bg != far && bg != near && bg != over && far != near && far != over && near != over);
    // 反向用例：G-buffer 8bit sRGB 往返对纯 0/1 是**不动点**——闭式"不必模型化量化"这句
    // 话的全部依据。它一旦不成立，`blendLinearAt` 会抛，而这条断言先在这里红。
    c.chk("blend-sort 显示链：roundTrip8Srgb 对 0/1 是不动点（闭式免量化模型的前提）",
        roundTrip8Srgb(0.0) == 0.0 && roundTrip8Srgb(1.0) == 1.0
            && roundTrip8Srgb(0.5) != 0.5);
}

/// ④ 判别力：备选错误模型与正确模型的最大字节差（阈值 8 LSB，不足即拒跑）。
void checkBlendDiscriminativePower(Checker& c)
{
    const auto pieces = target();
    const int flipped
        = blendMaxModelDiff(pieces, camera(), kW, kH, BlendOrder::FrontToBack,
            BlendModel::NonPremultiplied);
    const int premul
        = blendMaxModelDiff(pieces, camera(), kW, kH, BlendOrder::BackToFront,
            BlendModel::Premultiplied);
    c.chk("blend-sort 判别力：反序模型 = 42 LSB（实测读数）", flipped == 42,
        std::to_string(flipped));
    c.chk("blend-sort 判别力：预乘混合模型 = 95 LSB（实测读数）", premul == 95,
        std::to_string(premul));
    c.chk("blend-sort 判别力：两种错法都过 8 LSB 下限（判据有资格当验收）",
        flipped >= kBlendPowerFloorLsb && premul >= kBlendPowerFloorLsb,
        std::to_string(flipped) + " / " + std::to_string(premul));
    c.chk("blend-sort 判别力：下限严格大于判据阈值 1 LSB（阈值不是从判别力里挤出来的）",
        kBlendPowerFloorLsb > kBlendTolLsb);
    // 反向用例（证明上面那个数不是"恒 ≥ 8"的摆设）：两层同色 ⟹ 次序不可判 ⟹ 判别力必须掉到
    // 0。这条红 = `blendMaxModelDiff` 变成了常数，而它一旦是常数，④ 前三条全是假绿。
    auto sameColor = pieces;
    sameColor[2].color = {0.0, 1.0, 0.0};
    const int deadFlipped
        = blendMaxModelDiff(sameColor, camera(), kW, kH, BlendOrder::FrontToBack,
            BlendModel::NonPremultiplied);
    c.chk("blend-sort 判别力：两层同色 → 次序判别力掉到 0（不是恒 ≥ 下限的摆设）",
        deadFlipped < kBlendPowerFloorLsb, std::to_string(deadFlipped));
    // 两条轴互相独立：同色只废掉次序判别力，混合方程那条仍然看得见（实测 114）。
    // 它同时说明"判别力不足"必须**逐条**判，不能只测一条就宣布量具有力气。
    c.chk("blend-sort 判别力：同色时方程判别力仍在（两条轴独立，实测 114）",
        blendMaxModelDiff(sameColor, camera(), kW, kH, BlendOrder::BackToFront,
            BlendModel::Premultiplied)
            >= kBlendPowerFloorLsb);
}

/// ⑤ 三条拒跑条件里的两条纯算式（第三条「相机对账」要读 camera.lua，住在仪器层）。
void checkBlendRefusals(Checker& c)
{
    const auto pieces = target();
    // 排序键退化：引擎用 stable_sort，相等键视为已序 ⟹ 比较子 `>` 与 `<` 给出**同一个序列**
    // ⟹ 变异结构性不红，而画面可能因作者顺序恰好正确 = 一条假绿的判据。这正是「所有 node
    // 都 identity」那份输入会掉进去的坑（排序代理是 node 原点，不是几何中心）。
    auto degenerate = pieces;
    degenerate[2].translation = degenerate[1].translation;
    c.chk("blend-sort 拒跑：两层 distanceSq 相等 → 抛（排序键退化 = 变异不红）",
        throwsStd([&] { requireSortableKeys(degenerate, camera()); }));
    c.chk("blend-sort 拒跑：本靶的 distanceSq 互异 → 不抛（阳性对照，否则上一条是恒真）",
        !throwsStd([&] { requireSortableKeys(pieces, camera()); }));
    // 机位不是精确 −Z 正视：forward 带任何 x/y 分量都得建 view 矩阵，而闭式没建。
    c.chk("blend-sort 拒跑：机位带偏航 → 抛（闭式按纯线性映射解像素）", throwsStd([&] {
        BlendCamera yawed = camera();
        yawed.target = {0.5, 0.0, 0.0};
        (void)blendRegionSamples(pieces, yawed, kW, kH);
    }));
    c.chk("blend-sort 拒跑：机位沿 +Z 看 → 抛（forward 不是 −Z）", throwsStd([&] {
        BlendCamera back = camera();
        back.position = {0.0, 0.0, -6.0};
        back.target = {0.0, 0.0, 0.0};
        (void)blendRegionSamples(pieces, back, kW, kH);
    }));
    c.chk("blend-sort 拒跑：画幅非正 → 抛（截图尺寸与 manifest 对不上时的形态）",
        throwsStd([&] { (void)blendRegionSamples(pieces, camera(), 0, kH); }));
}

/// ⑥ 四条判据名必须已在注册表里（改名而漏挂号 = `verdictFor` 运行期才炸，太晚）。
void checkBlendVerdictsRegistered(Checker& c)
{
    for (const char* name : { "blend-sort-backdrop", "blend-sort-layer-far",
             "blend-sort-layer-near", "blend-sort-order" }) {
        bool found = false;
        for (const auto& j : registeredJudgments())
            found = found || std::string(j.name) == name;
        c.chk("blend-sort 判据已挂号进注册表", found, name);
    }
}

}  // namespace

void testBlendSort(Checker& c)
{
    checkBlendProjectionAnchor(c);
    checkBlendCompositeClosedForm(c);
    checkBlendDisplayChain(c);
    checkBlendDiscriminativePower(c);
    checkBlendRefusals(c);
    checkBlendVerdictsRegistered(c);
}

}  // namespace RGL::imgqa
