// material-swim —— **材质场世界不变性**（债-152 的守卫；相对坐标批 5 第 5 刀）。
//
// ## 缺陷本体（钉到行；**已修**，下面是修前的形态）
//
// 修前 `rgl/shaders/deferred_geometry.frag.slang` 的 `float2 q = P.xz * freq;`（现 :166，
// 改成 `q = (P.xz + ws.zw) * freq`）——水洼掩码 = 低频值噪声（世界 xz），而 `P` 在 rebase
// 之后是**相对渲染原点**的坐标，原点每帧 snap 到级联纹素粒度并跟着相机走 ⟹ **噪声场整片随
// 相机平移**。同一个 `q` 还喂水膜法线波纹（一处病灶两个症状）。
//
// ## 为什么必须有这台仪器（判据失明第四例的补位）
//
// 批 5 的 `no-harm` 判据是「同一机位下门开 vs 门关比静态图」。而一个会滑的图案**在每一帧
// 单独看都是合理的湿面图案**——斑块只是换了位置，静态对比只看到很小的差。⟹ **静态单姿态
// 对比对「只在运动中出现」的缺陷结构性不可见**。前三条失明是「归一化量饱和」「差分对常量
// 位移失明」「注入量无区分力」，这一条是**激励形态不对**：再怎么换判据算子也没用，必须换
// 激励（把相机动起来）。本仪器就是那个「动起来」的激励 + 一条能看见滑移的对拍口径。
//
// ## 激励 = 相机**平移**（旋转不暴露它）
//
// 原地转视角时相机位置不动 ⟹ 渲染原点不动 ⟹ 图案不滑。所以两枚机位必须是**同姿态、纯横移**
// （target 与 position 一起平移 ⟹ 姿态逐字相同，只有位置差 kStimulusM）。
//
// ## 对拍口径 = 地面平面上的世界空间对拍（债-139 已定死的形状）
//
// 两枚机位是本工具自己设的 ⟹ 每枚机位下「像素 → 地面 y=0 的世界 xz」是**闭式**的，不需要
// 引擎回显任何东西，也**不用 G-buffer 的 position 通道**（那个通道在门开态是相对原点的，
// 用它反而要把原点减回来 = 多一个自由度多一个错处）。比较对象 = **metalrough 通道**
// （`gbuffer_contract.cppm:48` 的第 3 通道，G = roughness），因为水洼直接调制粗糙度。
// 判据 = 两枚机位下**同一个世界点的粗糙度必须相同**（水洼场是世界 xz 的函数，与相机无关）。
//
// 口径本体（重投影 / 格坐标 / 共同格计数 / 平移搜索）住 `ground_field_metric.{h,cpp}`，与
// `swim`（影场世界不变性）**同一份**——两台仪器共用对拍口径，各自只管「这个逐像素二值量
// 怎么来」。不抄第二份 = AGENTS 哲学第 1 条。
//
// ## 靶 = `ssr_rig_empty`（零起伏平地，🔴 不是 bistro 鹅卵石地）
//
// 债-139 就是卡在「拿 bistro 鹅卵石地当平地」的测量地板上。本靶地面 = 一片 y=0 的平地 quad
// （manifest 的 aabb 给出矩形，仪器按它筛像素），材质 roughness = 0.12 ≤ `ssr.rough_full`
// = 0.15 ⟹ 湿面身份 `wet = 1 − smoothstep(0.15, 0.40, roughness)` **恰为 1** ⟹ 水洼调制满
// 生效，G-buffer 的 roughness 字节在水洼(13) 与湿石(89) 两态之间跳变（实测 min/max）。
// 无柱变体 = 画面里没有第二个物体 ⟹ 「哪些像素是地面」由几何闭式决定，不需要 objectID
// 反解（那是 swim 那边必须的，它的靶上有遮挡体）。
//
// ## 六臂（一次引擎进程一臂；单实例串行）
//
//   | 臂     | coordinate.rebase | 相机 | entity position | 用途 |
//   |--------|-------------------|------|-----------------|------|
//   | offA   | 关                | A    | 0               | 门关基准 |
//   | offB   | 关                | B=A+d| 0               | 门关对拍（**地板**）|
//   | onA    | 开                | A    | 0               | 门开基准 |
//   | onB    | 开                | B=A+d| 0               | 门开对拍（**主判据**）|
//   | onA2   | 开                | A    | 0               | 同机位重跑（**阴性对照**）|
//   | offS   | 关                | B=A+d| **d**           | 注入已知世界偏移（**区分力守卫**）|
//
// offS 与 offB 的差别只有「整个靶沿世界 +X 挪 d，相机跟着挪 d」⟹ 屏幕取景逐字相同，而
// **图案的锚**（修法给出的场景锚）跟着靶挪了 d ⟹ 两场的对拍必须报出 ≈ d 的滑移。它守的是
// 「通过可能只是量具瞎」：一条只会读 0 的判据在这里当场红。
//
// ## 四条判决线（挂号与容差理由 → `imgqa check`）
//
// J1 `material-swim-world-invariant`（🔴 主判据，**修前必须红**）：门开两机位的材质场翻转率
//   ≤ 0.5% 且最优平移 ≤ 1 格。J2 `material-swim-determinism`：同机位两跑**恰为 0**。
// J3 `material-swim-gate-closed-floor`：门关两机位（按构造世界不变）读到的地板 ≤ 0.5%。
// J4 `material-swim-offset-sensitivity`（🔴 区分力守卫，**修前必须红**）：注入 d 必须报出 d。
//
// ## 五道闸门（全部抛 rc=3、一条 Verdict 都不产；表先出、闸门后抛）
//
// G1 靶三件套在场（loadManifest 抛）+ 地面 piece 的 aabb 退化守卫（零厚度矩形 = 筛不出像素）。
// G2 引擎回显对账：门开臂必须有 `coordinate.rebase = true`、门关臂必须缺席（「覆写落盘了但
//   引擎没按它跑」与「原点恒零」两种失效在图像上都长得像绿）。
// G3 **激励在场**：onA 与 onB 的图不得逐位相同（相机没动 = 六臂只剩五臂，而 J1 恒真）。
// G4 **场非退化**（本仪器最重要的一道）：每臂的有效地面格数 ≥ 下限、湿格占比落在 [5%,95%]、
//   roughness 字节的极差 ≥ 60 LSB。恒值场上「翻转率 0」是**恒真**——那正是本项目反复登记的
//   「对空集合的断言恒真 = 假绿」。
// G5 **共同覆盖足够**：每一对的共同格数 ≥ 较小场的 80%（只比两个几乎不相交的场 = 分母是彩票）。
//
// ## 变异声明（哪个变异让本仪器红，实跑读数）
//
// **实跑过的那一条**（2026-10-26，工装 = 备份 → 改 → `just reflect` → 建 → 跑 → 反向还原，
// 构建带失败重试；还原后 `reflection_manifest.txt` 与变异前**逐位相同**）：
//   把 `deferred_geometry.frag.slang` 的锚定改回吃相对坐标（`q = (P.xz + ws.zw) * freq`
//   → `q = P.xz * freq`），其余一字不动。
//     · J1 **FAIL** 翻转 3279/28270 = **11.60 %**，最优平移 **(+0.50, 0.00) m 处失配恰 0**
//       （= 场是自身的精确平移，滑移量恰等于相机平移量，不是"约等于"）；
//     · J4 **FAIL** 报 **(0.00, 0.00)** 而非注入的 0.50 m（零位移处翻转 0.00 % = 量具在
//       缺陷态下会读绿，所以这条守卫必须独立存在）；
//     · J2 PASS 0/30076、J3 PASS 33/28270 = 0.12 % ——**两条防线在缺陷态下照旧绿**，
//       这正是"J1 红 = 缺陷，而不是量具坏了"的证据；
//     · 佐证：G3（onA vs onB 逐位不同像素）从修后的 307266 掉到 **603**——图案钉在屏幕上
//       不随世界走，两机位的地面粗糙度自然几乎相同；门开态湿格占比 63.3 % 而门关 40.0 %。
//   修后（同一仪器、同一构建口径）：J1 **PASS** 33/28270 = **0.12 %**、最优平移 (0,0)，
//   与 J3 **逐数相同**；J4 **PASS** 报 (+0.50, 0.00) m。
//
// **写得出但没跑的变异**（按可信度排序，红的那条判据在括号里）：
//   · `fillWetParams` 的 origin/anchor 传成两个 0（CPU 侧同形变异，J1/J4）；
//   · 偏移符号写反（`anchor − origin`，J1：滑移变成 2× 相机位移，翻转率更高）；
//   · 减法先在 float 里做（远工作点上偏移塌成 0，出厂工作点**不红**——本仪器机位在原点
//     附近，抓不到它；那条由 `tests/test_wet_params.cppm` 的阳性对照钉，不由本仪器钉）；
//   · 阈值改成各臂自算（J3 与 J1 口径分叉，地板失去意义）；
//   · 重投影漏乘 aspect（J3 门关地板跟着红 = 由 J3 拦下，不是让 J1 假绿）。
//
// 🔴 零 flag 是设计：靶 / 机位 / 平移量 / 尺寸 / 格边长 / 阈值都是量具的组成部分，能调 =
// 能调绿（同 shadow-edge-ruler / shadow-edge-rebase）。
#include "imgqa.h"
#include "cluster_path_gate.h"    // readEngineLog（与交付配置上的两台阴影仪器共用）
#include "ground_field_metric.h"

#include <opencv2/core.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

// ── 量具常数（改动 = 改判据）──────────────────────────────────────────────────────────
/// 靶（tools/qa_scenes.py 的 SCENES 键；无柱变体 = 画面里只有平地）。
constexpr const char* kRigStem = "ssr_rig_empty";
/// 地面 piece 名（manifest 里给 aabb，仪器按它筛像素；不在 C++ 里重抄几何）。
constexpr const char* kGroundPiece = "ground_metal";
constexpr int kWidth = 1280;
constexpr int kHeight = 720;
/// 竖直 fov（度）。机位高度与俯角一起决定「哪些像素落在地面上」。
constexpr double kFovDegrees = 45.0;
constexpr double kCameraHeightM = 1.6;
/// 机位 A 的 position / target（target 在 A 的前下方 ⟹ 俯角 ≈ 25°，整幅都是地面）。
constexpr double kCamAx = 0.0, kCamAz = 4.0, kCamTz = -6.0;
/// 🔴 激励 = 相机沿世界 +X 平移这个量（旋转不暴露缺陷，见文件头）。
constexpr double kStimulusM = 0.5;
/// 世界格边长（米）——与 swim 同刻度；水洼噪声特征尺寸 = 2·sceneRadius/puddleFreqRadii
/// ≈ 0.94 m，即一格 = 噪声特征尺寸的 1/19，滑移不会被格边长吃掉。
constexpr double kCellM = 0.05;
/// 交点距离上限（米）：超出即掠射区，一像素跨数米、格内票数噪声主导，不进判据。
constexpr double kMaxDistanceM = 12.0;
/// 每格至少这么多样本才要（近场一像素跨 1–6 cm，4 票 ≈ 覆盖住一格的采样抖动）。
constexpr int kMinVotes = 4;
/// 平移搜索半径（格）= ±1 m = 激励量的 2 倍（守卫要报出 d，搜索窗必须容得下它）。
constexpr int kSearchRadius = 20;
/// 一臂的有效地面格数下限（低于它 = 靶没装对 / 机位没看到地面，全部读数作废）。
constexpr int kMinCells = 2000;

/// 一臂 = 一次引擎进程 + 一张 metalrough 直读图 + 引擎回显里**必须**出现/缺席的证据。
struct Arm {
    std::string name;
    bool rebase = false;   ///< coordinate.rebase
    double camShift = 0.0; ///< 相机沿 +X 的平移量（激励）
    double entityShift = 0.0;  ///< 靶沿 +X 的平移量（区分力守卫的注入量）
};

const std::vector<Arm>& arms()
{
    static const std::vector<Arm> kArms = {
        { "offA", false, 0.0, 0.0 },        { "offB", false, kStimulusM, 0.0 },
        { "onA", true, 0.0, 0.0 },          { "onB", true, kStimulusM, 0.0 },
        { "onA2", true, 0.0, 0.0 },         { "offS", false, kStimulusM, kStimulusM },
    };
    return kArms;
}

/// 两位小数（读数文案的统一精度）。⚠ 不复用 `temporal_swim.h` 的 `d2`：那个头自己写着
/// 「本头**不是** taa-swim 的对外接口……别的 TU 别接」，跨仪器接它就是把两台仪器的
/// 打印口径焊在一起。
std::string f2(double v)
{
    char b[32];
    std::snprintf(b, sizeof b, "%.2f", v);
    return b;
}

/// lua 表里的浮点字面量：17 位有效数字保证 double 逐位往返（少一位 = 引擎读到的不是我们
/// 算出的那个数，而「相机平移了多少」正是判据的自变量）。
std::string luaNum(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

/// 本臂装进 scene.lua 的 entity 文本（区分力守卫臂把靶挪 d；其余臂不写 position 键）。
std::string entitiesLua(const Arm& a)
{
    return "{ type = \"model\", file = \"models/" + std::string(kRigStem) + ".glb\""
        + (a.entityShift != 0.0 ? ", position = { " + luaNum(a.entityShift) + ", 0.0, 0.0 }"
                                : "")
        + " }";
}

Engine::Camera cameraOf(const Arm& a)
{
    return Engine::Camera { { kCamAx + a.camShift, kCameraHeightM, kCamAz },
        { kCamAx + a.camShift, 0.0, kCamTz }, kFovDegrees };
}

/// G2：引擎回显对账。门开臂必须在场、门关臂必须缺席——在场 = 上一臂残留或覆写没落到
/// false，那「门关」两臂其实开着门，地板与区分力守卫一起失效。
void requireRebaseEcho(const Arm& a, const std::string& logText)
{
    const std::string needle = "coordinate.rebase = true";
    const bool present = logText.find(needle) != std::string::npos;
    if (a.rebase && !present)
        throw std::runtime_error("臂 " + a.name + " 的引擎回显里没有 `" + needle
            + "` ⟹ coordinate.rebase=true 落盘了但**引擎没按它跑**（键被 gate_lint 拒 / 读键"
              "路径没接上）。开着门却静默按门关跑 = 主判据在门关态上下结论，全部读数作废");
    if (!a.rebase && present)
        throw std::runtime_error("门关臂 " + a.name + " 的引擎回显里**出现了** `" + needle
            + "` ⟹ 该臂其实开着门（覆写打偏或被后面的门盖掉），地板与区分力守卫不是门关态");
}

/// 采一臂：交付配置 + 换 entities 装靶 + 机位从第 1 帧生效 + metalrough 通道直读。
/// 每臂重建目录（上一臂的残留键会静默活到下一臂）。
cv::Mat captureArm(const Arm& a, const std::string& profileDir, std::string& logText)
{
    std::error_code ec;
    std::filesystem::remove_all(profileDir, ec);
    ProfileOpts o;
    o.deliveryConfigDir = projectRoot() + "/configs";  // 🔴 红线：判据跑在交付配置上
    o.size = { kWidth, kHeight };
    o.overrideDeliveryEntities = true;  // 第 8 例外门：装靶
    o.deliveryEntitiesLua = entitiesLua(a);
    o.overrideCoordinateRebase = true;  // 第 9 例外门：本仪器的自变量
    o.coordinateRebaseValue = a.rebase;
    o.hasCamera = true;  // 机位写进 camera.lua = 从第 1 帧生效
    o.cameraPosition = cameraOf(a).position;
    o.cameraTarget = cameraOf(a).target;
    o.cameraFov = kFovDegrees;
    makeProfile(profileDir, o);
    cv::Mat img;
    {
        Engine eng(profileDir);
        eng.advanceFrames(8);  // 热身按帧数（墙钟睡会在争用下量到瞬态）
        img = readPngBgr(eng.shot("material_swim_" + a.name, nullptr, 3));
    }  // 析构 = 引擎退出、日志落盘，回显对账必须在它之后
    logText = readEngineLog();
    requireRebaseEcho(a, logText);
    return img;
}

/// 一臂的地面格 → roughness 字节集合（按 manifest 的 aabb + 本臂靶平移筛像素）。
std::map<GroundCell, std::vector<uint8_t>> gatherCells(const cv::Mat& bgr, const Arm& a,
    const cv::Rect2d& groundRect, std::vector<uint8_t>& allBytes)
{
    const GroundCamera cam = makeGroundCamera(cameraOf(a).position, cameraOf(a).target,
        kFovDegrees, bgr.cols, bgr.rows);
    const cv::Rect2d rect(groundRect.x + a.entityShift, groundRect.y, groundRect.width,
        groundRect.height);
    std::map<GroundCell, std::vector<uint8_t>> cells;
    for (int y = 0; y < bgr.rows; ++y)
        for (int x = 0; x < bgr.cols; ++x) {
            glm::dvec2 w;
            if (!groundHitOf(cam, x, y, kMaxDistanceM, w))
                continue;
            if (w.x < rect.x || w.x >= rect.x + rect.width || w.y < rect.y
                || w.y >= rect.y + rect.height)
                continue;
            const GroundCell cell = { static_cast<int>(std::floor(w.x / kCellM)),
                static_cast<int>(std::floor(w.y / kCellM)) };
            cells[cell].push_back(bgr.at<cv::Vec3b>(y, x)[1]);  // G = roughness
        }
    for (const auto& [c, v] : cells)
        for (const uint8_t b : v)
            allBytes.push_back(b);
    return cells;
}

/// 逐格中位数 → 二值场（湿 = true）。样本数不足 kMinVotes 的格**丢弃**（不是记成某一侧：
/// 一格的样本本来就跨在湿/干界两侧时，记哪一侧都是假标签）。
GroundField binarize(const std::map<GroundCell, std::vector<uint8_t>>& cells, int thr)
{
    GroundField field;
    for (const auto& [c, v] : cells) {
        if (static_cast<int>(v.size()) < kMinVotes)
            continue;
        std::vector<uint8_t> s = v;
        std::sort(s.begin(), s.end());
        field[c] = s[s.size() / 2] < thr;
    }
    return field;
}

/// 一次对拍的读数（翻转率 + 最优平移）。
struct PairReadout {
    int common = 0;
    int mismatch = 0;
    ShiftSearch best { };
    double flipPct() const { return 100.0 * static_cast<double>(mismatch) / std::max(1, common); }
    double shiftM() const { return std::hypot(best.x, best.z) * kCellM; }
};

PairReadout compareFields(const GroundField& a, const GroundField& b)
{
    PairReadout r;
    const FieldOverlap zero = overlapOf(a, b, 0, 0);
    r.common = zero.common;
    r.mismatch = zero.mismatch;
    r.best.mismatch = static_cast<size_t>(zero.mismatch);
    if (r.mismatch > 0)
        r.best = searchShift(a, b, r.common, static_cast<size_t>(r.mismatch), kSearchRadius);
    return r;
}

void printPair(const char* label, const PairReadout& r, const char* expect)
{
    std::printf("  %-34s 共同格 %-7d 翻转 %-6d = %-6.2f%%  最优平移 (%+.2f, %+.2f) m"
                "  失配 %zu   %s\n",
        label, r.common, r.mismatch, r.flipPct(), r.best.x * kCellM, r.best.z * kCellM,
        r.best.mismatch, expect);
}


/// G1：靶与地面矩形（缺任何一件 = 抛，不 SKIP；矩形从 manifest 读，不在 C++ 里重抄几何）。
cv::Rect2d requireGroundRect()
{
    const Manifest man = loadManifest(kRigStem);
    const ManifestPiece* ground = nullptr;
    for (const auto& p : man.pieces)
        if (p.name == kGroundPiece)
            ground = &p;
    if (ground == nullptr)
        throw std::runtime_error(std::string("靶 ") + kRigStem + " 的 manifest 里没有 piece `"
            + kGroundPiece + "` ⟹ 靶换了名或换了生成器，地面矩形无从取（仪器不在 C++ 里重抄几何）");
    const cv::Rect2d rect(ground->aabbMin[0], ground->aabbMin[2],
        ground->aabbMax[0] - ground->aabbMin[0], ground->aabbMax[2] - ground->aabbMin[2]);
    if (rect.width <= 0.0 || rect.height <= 0.0)
        throw std::runtime_error(std::string("靶 ") + kRigStem + " 的地面 aabb 在 xz 上退化（"
            + std::to_string(rect.width) + "x" + std::to_string(rect.height)
            + "）⟹ 筛不出任何地面像素，而「空集上的翻转率 0」是恒真");
    std::printf("  G1 过：地面矩形 = [%.1f, %.1f] x [%.1f, %.1f]（manifest 的 %s.aabb）\n",
        rect.x, rect.x + rect.width, rect.y, rect.y + rect.height, kGroundPiece);
    return rect;
}

/// 六臂采集（单实例串行；一臂一次引擎进程，回显对账在 captureArm 内 = G2）。
std::map<std::string, cv::Mat> captureAllArms(const std::string& profileDir)
{
    std::map<std::string, cv::Mat> img;
    for (const auto& a : arms()) {
        std::string logText;
        img[a.name] = captureArm(a, profileDir, logText);
        std::printf("  ✓ 臂 %-5s rebase=%-5s camShift=%+.2f entityShift=%+.2f  %dx%d\n",
            a.name.c_str(), a.rebase ? "true" : "false", a.camShift, a.entityShift,
            img[a.name].cols, img[a.name].rows);
    }
    return img;
}

/// G3：激励在场（相机没动 = 六臂只剩五臂，而 J1 恒真）。
void requireStimulusLive(const std::map<std::string, cv::Mat>& img)
{
    const long long stimulus = cv::countNonZero(anyChannelDiff(img.at("onA"), img.at("onB")));
    std::printf("\n  G3 门开态 onA vs onB 逐位不同像素 = %lld%s\n", stimulus,
        stimulus == 0 ? "   🔴 相机根本没动" : "");
    if (stimulus == 0)
        throw std::runtime_error("G3: 门开态两机位**逐位相同** ⟹ 相机平移没进链路（camera.lua"
            " 覆写打偏 / 机位被后面的门盖掉），「两机位对拍」其实是一机位自比，恒真");
}

/// G4：每臂的场必须非退化（有效地面格数够 + 湿格占比落在 [5,95]%）。恒值场上「翻转率 0」
/// 是**恒真**——本项目反复登记的「对空集合的断言恒真 = 假绿」正是这条要拦的。
void requireFieldsNonDegenerate(const std::map<std::string, GroundField>& field)
{
    for (const auto& [name, f] : field) {
        int wet = 0;
        for (const auto& [c, label] : f)
            wet += label ? 1 : 0;
        const int n = static_cast<int>(f.size());
        const double frac = n > 0 ? 100.0 * static_cast<double>(wet) / n : 0.0;
        std::printf("  臂 %-5s 有效地面格 %-7d 湿格占比 %5.1f%%\n", name.c_str(), n, frac);
        if (n < kMinCells)
            throw std::runtime_error("G4: 臂 " + name + " 的有效地面格只有 "
                + std::to_string(n) + "（< " + std::to_string(kMinCells)
                + "）⟹ 分母太小，翻转率是彩票，全部读数作废");
        if (frac < 5.0 || frac > 95.0)
            throw std::runtime_error("G4: 臂 " + name + " 的湿格占比 = " + std::to_string(frac)
                + "% 落在 [5,95] 之外 ⟹ 材质场**恒值**（水洼调制没生效，或整幅都是水洼）。"
                  "恒值场上「翻转率 0」是恒真 = 假绿，停下来报告");
    }
}

/// G5：每一对的共同覆盖必须够（只比两个几乎不相交的场 = 分母是彩票）。
void requireCoverage(const char* what, const PairReadout& r, const GroundField& a,
    const GroundField& b)
{
    const int smaller = static_cast<int>(std::min(a.size(), b.size()));
    if (r.common < smaller * 8 / 10)
        throw std::runtime_error(std::string("G5: ") + what + " 的共同格 "
            + std::to_string(r.common) + " < 较小场的 80%（" + std::to_string(smaller)
            + "）⟹ 两个几乎不相交的场对拍，分母是彩票");
}

/// 四条判决线（阈值理由的完整推导 → judgments_table_material.cpp，这里只写判据形状）。
void pushVerdicts(std::vector<Verdict>& vs, const PairReadout& det, const PairReadout& floor_,
    const PairReadout& main_, const PairReadout& inj)
{
    constexpr double kFlipTolPct = 0.5;   // = 实测地板 0.11% 的 4 倍（理由见注册表）
    constexpr double kShiftTolM = kCellM;  // 一格 = 量具自身的分辨率
    vs.push_back(verdictFor("material-swim-world-invariant",
        main_.flipPct() <= kFlipTolPct && main_.best.x == 0 && main_.best.z == 0,
        "门开态（coordinate.rebase=true）机位沿 +X 平移 " + f2(kStimulusM) + " m 后，同一世界"
            "点的 roughness 标签翻转 " + std::to_string(main_.mismatch) + "/"
            + std::to_string(main_.common) + " = " + f2(main_.flipPct())
            + "%，最优平移 (" + f2(main_.best.x * kCellM) + ", "
            + f2(main_.best.z * kCellM) + ") m、该平移处失配 "
            + std::to_string(main_.best.mismatch)));
    vs.push_back(verdictFor("material-swim-determinism",
        det.mismatch == 0 && det.common > 0,
        "同机位两跑（门开态）翻转 " + std::to_string(det.mismatch) + "/"
            + std::to_string(det.common) + "（须**恰为 0**）"));
    vs.push_back(verdictFor("material-swim-gate-closed-floor",
        floor_.flipPct() <= kFlipTolPct && floor_.best.x == 0 && floor_.best.z == 0,
        "门关态同一算子的地板：翻转 " + std::to_string(floor_.mismatch) + "/"
            + std::to_string(floor_.common) + " = " + f2(floor_.flipPct())
            + "%，最优平移 (" + f2(floor_.best.x * kCellM) + ", "
            + f2(floor_.best.z * kCellM) + ") m"));
    vs.push_back(verdictFor("material-swim-offset-sensitivity",
        std::abs(inj.best.x * kCellM - kStimulusM) <= kShiftTolM
            && std::abs(inj.best.z * kCellM) <= kShiftTolM && inj.flipPct() > kFlipTolPct,
        "注入已知世界偏移 d = " + f2(kStimulusM) + " m（靶挪 d、相机跟着挪 d ⟹ 取景逐字相同、"
            "只有图案的锚挪了 d）：量具报出 (" + f2(inj.best.x * kCellM) + ", "
            + f2(inj.best.z * kCellM) + ") m，该处失配 "
            + std::to_string(inj.best.mismatch) + "，零位移翻转 " + f2(inj.flipPct()) + "%"));
}

}  // namespace

/// material-swim 入口（注册见 cli_table_material.cpp 与 main.cpp 的 kMeta）。
int cmdMaterialSwim()
{
    const std::string profile = projectRoot() + "/build/rgl/imgqa_material_swim_profile";
    std::printf("material-swim —— 材质场（G-buffer metalrough 的 roughness 通道）的世界不变性"
                "：机位沿世界 +X 平移 %.2f m，同一个世界点的粗糙度必须相同\n",
        kStimulusM);
    std::printf("  靶 = models/%s.glb（零起伏平地 y=0）；采集 %dx%d，fov %.0f°，格边长 %.0f cm"
                "，交点上限 %.0f m，每格 ≥ %d 样本\n",
        kRigStem, kWidth, kHeight, kFovDegrees, kCellM * 100.0, kMaxDistanceM, kMinVotes);

    const cv::Rect2d groundRect = requireGroundRect();  // G1

    const std::map<std::string, cv::Mat> img = captureAllArms(profile);  // 单实例串行

    requireStimulusLive(img);  // G3

    // 阈值 = 全部臂地面字节的 (min+max)/2（水洼 13 / 湿石 89 的双峰中点；一次算定，
    // 🔴 所有臂共用同一个阈值——各臂各算 = 同一条判据在不同机位下有不同口径）
    std::vector<uint8_t> allBytes;
    std::map<std::string, std::map<GroundCell, std::vector<uint8_t>>> cells;
    for (const auto& a : arms())
        cells[a.name] = gatherCells(img.at(a.name), a, groundRect, allBytes);
    if (allBytes.empty())
        throw std::runtime_error("地面像素为空 ⟹ 机位没看到地面（靶装错了？）");
    const int lo = *std::min_element(allBytes.begin(), allBytes.end());
    const int hi = *std::max_element(allBytes.begin(), allBytes.end());
    const int thr = (lo + hi) / 2;
    std::printf("\n  地面 roughness 字节：min=%d max=%d ⟹ 二值阈值 = %d（所有臂共用）\n", lo,
        hi, thr);

    std::map<std::string, GroundField> field;
    for (const auto& a : arms())
        field[a.name] = binarize(cells.at(a.name), thr);
    requireFieldsNonDegenerate(field);  // G4
    if (hi - lo < 60)
        throw std::runtime_error("G4: 地面 roughness 字节极差 = " + std::to_string(hi - lo)
            + " LSB（< 60）⟹ 湿/干两态分不开，二值化没有意义");

    // 四对读数（🔴 先出表再判决：本批的交付物就是这些数）
    std::printf("\n  ── 对拍读数（翻转率 = 同一世界点在两机位下标签不同的格占比）──\n");
    const PairReadout rDet = compareFields(field.at("onA"), field.at("onA2"));
    const PairReadout rFloor = compareFields(field.at("offA"), field.at("offB"));
    const PairReadout rMain = compareFields(field.at("onA"), field.at("onB"));
    const PairReadout rInj = compareFields(field.at("offB"), field.at("offS"));
    printPair("J2 同机位两跑（onA vs onA2）", rDet, "须**恰为 0**");
    printPair("J3 门关地板（offA vs offB）", rFloor, "按构造世界不变");
    printPair("J1 门开主判据（onA vs onB）", rMain, "🔴 缺陷态在此红");
    printPair("J4 注入 d（offB vs offS）", rInj, "须报出 ≈ 注入量");

    // G5：共同覆盖足够
    requireCoverage("J2", rDet, field.at("onA"), field.at("onA2"));
    requireCoverage("J3", rFloor, field.at("offA"), field.at("offB"));
    requireCoverage("J1", rMain, field.at("onA"), field.at("onB"));
    requireCoverage("J4", rInj, field.at("offB"), field.at("offS"));

    std::vector<Verdict> vs;
    pushVerdicts(vs, rDet, rFloor, rMain, rInj);

    std::printf("\n");
    for (const auto& v : vs)
        std::cout << "  " << v.line() << "\n";
    for (const auto& v : vs)
        std::printf("\n  [%s] 容差理由: %s\n", v.name.c_str(), v.whyTol.c_str());

    logCall("material-swim",
        { { "rig", kRigStem }, { "stimulus_m", luaNum(kStimulusM) },
            { "cell_m", luaNum(kCellM) }, { "size", std::to_string(kWidth) + "x" + std::to_string(kHeight) },
            { "thr", std::to_string(thr) }, { "j1_flip", f2(rMain.flipPct()) },
            { "j1_shift_x", f2(rMain.best.x * kCellM) }, { "j1_shift_z", f2(rMain.best.z * kCellM) },
            { "j2_flip", std::to_string(rDet.mismatch) }, { "j3_flip", f2(rFloor.flipPct()) },
            { "j4_shift_x", f2(rInj.best.x * kCellM) }, { "j4_flip", f2(rInj.flipPct()) } },
        vs);
    const bool ok = std::ranges::all_of(vs, [](const Verdict& v) { return v.ok; });
    std::printf("\n闸门 G1–G5 全过；J1–J4 见上（挂号与容差理由 → imgqa check）。"
                "🔴 J1 或 J4 红 = 材质场随相机滑（债-152），别记成「量具坏了」——J2/J3 同时"
                "绿恰恰说明量具是好的。\n");
    return ok ? 0 : 1;
}

}  // namespace RGL::imgqa
