#pragma once
//
// ao_noise_metric.h —— `ao-noise` 判据的**跨 TU 共享层**（类型 + 阈值唯一出处 + 算子声明）
//
//   为什么拆：`ao_noise.cpp` 到 903 代码行（工具侧上限 500）被文件护栏逼出第三刀。缝按
//   **仪器**切，一个命令的三块各归一处：
//
//     · `ao_noise_metric.cpp` = **口径层**（帧差算子 / 逐位相同判定 / 取值分布与扫描线 /
//       存图可视化 / 机位偏航换算）。先例 = `shadow_kernel_metric.*` 与 `diff_struct_metric.*`，
//       缝 = 「口径 vs 仪器」：改测量算子动这里，改阈值文案与采集流程不动这里。
//     · `ao_noise_groups.cpp` = **组 2/3/4/5/6**（取值分布 / 亚像素爬行 / 时域 / 掠射角 /
//       交付场景）。`组` 本来就是这台判据工具天然的仪器边界：一组一次采集、一组一张读数表，
//       组间唯一的共享物是入参与 `Results` 里点了名的读数（不用文件级全局，见下方 `Results`）。
//     · `ao_noise.cpp` = **入口 + 采集 + 组 1/1b + 判定**。`collect` 是唯一建主进程的地方，
//       组 1/1b 读的就是它的产物；「清空 outDir 陈旧产物」必须只在主流程执行一次 ⟹ 入口留下。
//
//   类型与常量为什么住这里：`Ctx`/`Collected`/`Results` 是三段之间**唯一**的数据通道（组函数
//   吃前两者、写第三者），`kAoSize`/`kShiftPx`/`kAoPage` 这些画幅、机位偏移与滑杆定位既进采集
//   （`profileOf` 的 size、`captureAb` 的 `/param`）又进读数表文案——两处各抄一份 = 第二份真相，
//   改一处忘另一处时**读数与解释会分家**（判据绿着说一件没发生的事）。
//   契约区半径**不在**这里：它是命令行 `--radius`，唯一出处 = CLI 表，运行期走 `Ctx::radius`。
//
//   ⚠ 本头**不是** ao-noise 的对外接口：命令入口 `cmdAoNoise` 声明在 `imgqa.h`（本仓惯例
//   「实现分散、声明集中」）。这里声明的东西只给 ao_noise*.cpp 三个 TU 用，别的 TU 别接。
//   跨 TU 的名字一律 `RGL::imgqa` 作用域（匿名 namespace 的成员跨不了 TU）；只被单个 TU 用的
//   小函数仍留在各自文件里的匿名 namespace。
//
#include "ao_metrics.h"
#include "imgqa.h"

#include <opencv2/core.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace RGL::imgqa {

// ───────────────────── 阈值与定位（全命令唯一出处）─────────────────────

inline constexpr int kAoSize[2] = {1280, 720};
inline constexpr double kAoFovDeg = 45.0;
/// 抖动周期（Halton(2,3)×16，= rgl/taa_jitter.cppm::kTaaJitterPeriod）。此处**不 import**
/// 引擎头（imgqa 是独立二进制、刻意不链引擎），值与之一致并由④的窗口对齐检查兜底。
inline constexpr int kJitterPeriod = 16;

/// 爬行检验的机位偏移。**必须是整数像素**：对齐于是成为精确的整数平移算子，
/// 不需要亚像素插值（插值自己会引入高频，把被测项淹掉）。
inline constexpr double kShiftPx = 1.0;

/// AO 强度滑杆的定位。真相源 = `rgl/generated/param_semantics_generated.cppm` 的
/// `kAoStrength`（label）+ `rgl/param_pages_impl.cppm` 的 `renderPage.name`（page）。
/// 写错不会静默：`/param` 对未知 label 回 404，`Engine::setParam` 抛异常。
/// 定义在 `ao_noise.cpp`（**不在本头**：字面量留在 .cpp，文案对账只比 .cpp 就能红）。
extern const char* const kAoPage;
extern const char* const kAoLabel;

// ───────────────────────────── 跨 TU 的类型 ─────────────────────────────

struct AoCapture {
    Frame frame;   ///< color + object_id + luma（区域归因的载体）
    cv::Mat ao;    ///< 8UC1 AO 附件（0..255 = 可见度）
};

/// 同进程、同机位下的一对 A/B（AO 开 / AO 关）+ 两根确定性锚。
///
/// **为什么必须同进程**（首跑之后改的）：跨进程的两张图之间隔着整个渲染环境（两次进程、
/// 两次 shader 编译、两次驱动初始化），把它们之差归因给 AO 是错的。实测跨进程在**契约区**
/// 出现最大 41 LSB 的差，而 AO 场在该区最多偏离契约 16 LSB（×strength 0.5 至多 8 LSB）——
/// 多出来的部分根本不是 AO。同进程内用 `/param` 翻强度，两张图之间只隔 scene data 里
/// 那一个 float，差值才是被测项。
struct AbPair {
    AoCapture on;        ///< 终帧 + object_id + AO 附件 @ strength
    cv::Mat aoOff;       ///< AO 附件 @ 0（反向用例：必须逐位 255）
    cv::Mat colorOff;    ///< 终帧 @ 0
    bool repeatable = false;  ///< 同参数连拍两次终帧是否逐位相同
    bool reversible = false;  ///< 强度切回 strength 后终帧是否与第一次逐位相同
};

/// 一帧的逐帧差幅度（8UC3 两张 → 非零像素数 / 灰度均值 / 灰度最大）。
struct FrameDiff {
    int64_t changed = 0;
    double mean = 0.0, mx = 0.0;
};

/**
 * 一次 ao-noise 运行的只读上下文：入参 + 目录 + 机位 + manifest。
 *
 * 为什么要有它：cmdAoNoise 原本是「一个函数装完六组测量」，组间全靠局部变量与捕获式
 * lambda 串起来。按组拆开后跨函数流动的只有两类东西——入参（进 Ctx，只读）与该组读数
 * （进 Results，显式传引用）。
 */
struct Ctx {
    std::string glb;        ///< 靶模型（合成场景，见文件头「为什么必须程序化靶」）
    std::string camera;     ///< 机位名（只进表头；机位本体已解析进 cam）
    Manifest man;           ///< piece 表 + 机位表（区域归因的期望色度出处）
    Engine::Camera cam;     ///< = man.cameras[camera]
    std::string outDir;     ///< 产物目录（主流程开头清空一次，见 cmdAoNoise）
    std::string profRoot;   ///< 各合成 profile 目录的父目录
    double strength = 0.0;  ///< ao.strength 的 A 档（B 档恒 0）
    int radius = 0;         ///< 契约区排除半径（px）
    int pairs = 0;          ///< 组 4 的 burst 对数（换算成抖动周期的整数倍帧数）
    bool doTemporal = false;
    bool doBistro = false;
};

/**
 * 采集段（主进程，TAA 全关）的产物：三组测量要的帧 + 区域归因 + 契约区掩码。
 *
 * 掩码只在这里算一次：契约区/梯度带的口径必须跨组一致（组 1 的契约区与组 3 的契约区
 * 不是同一个区域 = 两条判据各测各的地方，比值不可解释）。
 */
struct Collected {
    AbPair ab;                                       ///< 机位 cam 下的 A/B（组 1/1b/2 的被测帧）
    AbPair crawl;                                    ///< 机位纯偏航整 1 px 的同款 A/B（组 3）
    AoCapture grazeCap;                              ///< 掠射机位（组 5）
    Frame attr;                                      ///< AO 关的终帧 + on 的 id 图（区域归因载体）
    Attribution at;                                  ///< id ↔ piece 反解结果
    std::map<std::string, int32_t> oidByName;        ///< piece 名 → objectID
    cv::Mat dist;                                    ///< 到最近轮廓的距离（CV_32F）
    cv::Mat flat;                                    ///< 契约区（离边界 > radius px）
    cv::Mat bandLit;                                 ///< 梯度带内且 AO 确有遮蔽处
    cv::Mat allMask;                                 ///< 全图（观察量的区域）
    std::map<std::string, cv::Mat> flatByPiece;      ///< 逐 piece 的契约区（噪声按朝向分布）
    std::string flatNames;                           ///< 进了 flatByPiece 的 piece 名（表头）
};

/// 各组产出、判定段要跨组读的量（显式传参的载体）。默认值 = 原主函数里的初值，逐条对齐。
struct Results {
    // 组 1（AO 场本体）
    RegionStat aoOnFlat, aoOffFlat, aoOnAll, baseFlat;
    // 组 1b（交付画面 A/B）
    cv::Mat contactMask;  ///< 凹角接触环（时域组也要按它分区）
    double contactMin = 255.0, convexFracBelow = 0.0, contactByteGrain = 0.0;
    double byteFlatGrain = 0.0, byteFlatP99 = 0.0, byteFlatMax = 0.0;
    double leakMax = 0.0;
    int64_t leakPixels = 0, contactPixels = 0;
    // 组 3（亚像素爬行）
    bool crawlAgree = false, crawlScreenAnchored = false;
    double baseUnaligned = 0.0, baseAligned = 0.0;
    // 组 4（TAA 开态）
    int64_t floorChanged = 0;
    bool temporalHasResolution = false;
    // 组 6（交付场景）
    double bistroLeak = 0.0;
    int64_t bistroLeakPx = 0;
    bool bistroLeakOk = false;
};

// ─────────────── 口径层算子（定义在 `ao_noise_metric.cpp`）───────────────

/// 定点小数格式化（3/5 位）。列宽 = 读数表的表结构，精度 = 判据台账引用的口径。
std::string d3(double v);
std::string d5(double v);

/// 两张 PNG 是否**逐位**相同（解码后比像素，不比文件字节——PNG 编码本身不是判据）。
bool samePng(const std::string& a, const std::string& b);

/// 一帧的逐帧差幅度（8UC3 两张 → 非零像素数 / 灰度均值 / 灰度最大）。默认参数只写接口。
FrameDiff diffFrames(const cv::Mat& a, const cv::Mat& b, const cv::Mat& mask = {});

/// 绕相机自身 up（世界 Y）纯 yaw 整数字像素：像素位移与深度无关。
Engine::Camera yawCam(const Engine::Camera& base, double shiftPx);

/// 单通道可视化三件套：放大 / mask 红叠加 / 值域拉伸。
void saveAmp(const cv::Mat& x, const cv::Mat& mask, double gain, const std::string& path);
void saveMaskOverlay(const cv::Mat& base, const cv::Mat& mask, const std::string& path);
void saveStretched(const cv::Mat& ao, double lo, double hi, const std::string& path);

/// piece 名 → objectID（认不出 = **抛**，信息带认出的 piece 名清单——债-96：返回 0 = 背景
/// 会让换靶后的锚点在「背景 vs 背景」上求值，判据静默失效）。
int32_t oidOf(const Collected& col, const std::string& n);

/**
 * 一轮 burst 的逐帧差累计（下标 0 = 全图，1 = 契约区，2 = 接触环）+ 全图非零像素合计。
 *
 * 从 `ao_noise_groups.cpp` 的 `burstDiff` 拆出（债-96）：`/shotburst` 部分失败（<2 帧）时
 * 旧实现直接 `files[1]` 越界 + `files.size() − 1` 下溢成 0 = 整数除零。帧数不足是**无定义**
 * 输入 ⟹ 早退并**出声**（「0 对帧」与「测到了没噪声」是两回事，静默会被读成后者）。
 * 拆到口径层的理由 = selftest 能不吃 Engine 直接测这条早退（先例 = diffFrames / printAoScanline）。
 */
std::pair<std::array<FrameDiff, 3>, int64_t> burstDiffFiles(const std::vector<std::string>& files,
    const cv::Mat& flat, const cv::Mat& contactMask, const std::string& tag,
    const std::string& outDir);

/// AO 场取值分布四件套：直方图 / 非契约取值 / 分桶 / 扫描线（组 2 的读数面）。
std::vector<int> aoHistogram(const cv::Mat& ao);
void printNonContractValues(const std::vector<int>& hist);
void printAoBuckets(const std::vector<int>& hist);
void printAoScanline(const cv::Mat& ao);

// ─────────────── 采集与读数表（定义在 `ao_noise.cpp` 主 TU）───────────────

/// 把引擎产物收进本次测量目录（留证据）。burst 组与交付组都要它，故跨 TU。
void keep(const std::string& src, const std::string& outDir, const std::string& name);

/// 拍一对 A/B（同进程翻强度 + 两根确定性锚）。组 6 复用同一台采集。
AbPair captureAb(Engine& eng, const Engine::Camera* cam, double strength,
    const std::string& outDir, const std::string& prefix, bool withId = true);

/// 建一个本工具的合成 profile 并返回目录（ao/env 的口径只有一份；skybox 随刀②.2 删除）。
std::string profileOf(const Ctx& ctx, const std::string& name, double aoStrength, bool taa,
    double feedback);

/// 一行 RegionStat 读数（组 1 的表与组 1b 的接触环、组 5 的掠射表共用同一套列宽）。
void printRegionRow(const char* name, const RegionStat& s);

// ─────────── 组 2/3/4/5/6 的入口（定义在 `ao_noise_groups.cpp`）───────────
//
// 组 1/1b 只有主 TU 的 `cmdAoNoise` 会调（与 `collect` 同 TU），故不进本头；这里列的五条是
// **真的跨 TU**——`cmdAoNoise` 在主 TU 按原顺序逐组调用它们（打印顺序 = 验收的结构签名）。

void group2ValueDistribution(const Collected& col);
void group3Crawl(const Ctx& ctx, const Collected& col, Results& res);
void group5Grazing(const Ctx& ctx, const Collected& col, const Results& res);
void group4Temporal(const Ctx& ctx, const Collected& col, Results& res);
void group6Delivery(const Ctx& ctx, Results& res);

}  // namespace RGL::imgqa
