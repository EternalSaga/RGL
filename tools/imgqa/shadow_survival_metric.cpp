// shadow_survival_metric.cpp —— `shadow-caster-survival` 的**度量口径层**实现
// （面积存活率 + 连通块分档存活率 + 消失/变淡归因 + 闸门 C/D）。
//
// 缝与全部口径理由见 `shadow_survival_metric.h` 头注释。本文件只放实现；所有期望值来自
// 闭式（`selftest_survival.cpp`：同图 → 恰 1、全 0 → 恰 0、「只丢小块总面积不变」合成对 →
// 面积存活率恰 1.000 而小块档恰 0.000、消失/变淡各一条闭式、边界每半边一条），
// 实机截图只证明「与上次一样」，闭式才证明「算子算的是我说的那个量」。
//
// 🔴 守卫一律**抛**，不许返回 0：返回 0 会被上层读成「存活率为 0 = 全丢」或「= 没数」，
// 两种读法都错（与 `shadow_seam_metric.cpp` 的「闸门 vs 判据」分工同一条：判据可以红
// （红 = 有缺陷，是结论），守卫抛 = 「这台仪器今天没出结论」（rc=3，不是结论））。
#include "shadow_survival_metric.h"
#include "imgqa.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {
namespace {

/// 两条 cov 场的公共前提检查（尺寸 / 类型 / band 非空 / band 在图内）。**抛**，理由见文件头。
/// 与 `shadow_seam_metric.cpp` 的 requireFields 同判据但**各留一份**：两条判据的守卫文案
/// 各自点名自己的算子名，合并只会让错误串指错函数。
void requireFields(const cv::Mat& a, const cv::Mat& b, const cv::Rect& band, const char* who)
{
    if (a.size() != b.size() || a.type() != CV_64FC1 || b.type() != CV_64FC1)
        throw std::runtime_error(std::string(who)
            + "：两图必须同尺寸 CV_64FC1（seamCovField 产出的覆盖度场）。尺寸不同 = 两臂不是"
              "同一机位同一帧的两次采集，存活率没有意义；返回 0 会被读成「全丢」，拒绝");
    if (band.width <= 0 || band.height <= 0)
        throw std::runtime_error(std::string(who)
            + "：band 为空（宽/高 ≤ 0）。空分母的「存活率 0」是假绿——它既可能是全丢，"
              "也可能是根本没数，两者必须可区分");
    if (band.x < 0 || band.y < 0 || band.x + band.width > a.cols || band.y + band.height > a.rows)
        throw std::runtime_error(std::string(who)
            + "：band 越界（" + std::to_string(band.x) + "," + std::to_string(band.y) + " "
            + std::to_string(band.width) + "x" + std::to_string(band.height) + " 不在 "
            + std::to_string(a.cols) + "x" + std::to_string(a.rows)
            + " 内）。夹到图像边缘算出来的分档是边缘截断的产物，不是画面的产物");
}

/// 面积 → 档号（下界含、上界不含：[0,50) [50,200) [200,1000) [1000,∞)）。
/// 吃 `kSurvivalBucketEdges` **本身**——把边界抄成字面量的话，改常数的变异打不到 selftest。
int bucketOf(std::int64_t area)
{
    for (int b = 0; b < 3; ++b)
        if (area < kSurvivalBucketEdges[b])
            return b;
    return 3;
}

/// 整数进错误串（存活率用 6 位定点：闸门 C 钉的是**闭式恒等**，读数与界 1.000000 要能在
/// 消息里逐位对上，三位小数会把 0.9996 洗成 1.000 = 把半失效藏进打印）。
std::string num6(double v)
{
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}

std::string i64(std::int64_t v)
{
    return std::to_string(static_cast<long long>(v));
}

/// 参照本影掩膜（带内 ROI 局部坐标；连通域只在带内生长，理由住头文件声明处）。
cv::Mat buildUmbraMask(const cv::Mat& covRef, const cv::Rect& band)
{
    cv::Mat mask(band.height, band.width, CV_8UC1);
    for (int by = 0; by < band.height; ++by) {
        const auto* pr = covRef.ptr<double>(band.y + by);
        auto* pm = mask.ptr<uint8_t>(by);
        for (int bx = 0; bx < band.width; ++bx)
            pm[bx] = pr[band.x + bx] > kSurvivalUmbraBin ? 255 : 0;
    }
    return mask;
}

/// 参照本影像素的逐像素归类（活 / 消失 / 变淡），按连通块标号累加。
/// 从 `umbraBlobStats` 拆出守认知复杂度阈值；三态归属（恰 0.2 归变淡、消失严格 <、
/// 变淡严格 < 0.5·ref）一字未动，边界理由住头文件「边界归属」一节。
void classifyBlobPixels(const cv::Mat& labels, const cv::Mat& covTest, const cv::Mat& covRef,
    const cv::Rect& band, std::vector<std::int64_t>& alive, std::vector<std::int64_t>& vanished,
    std::vector<std::int64_t>& faded)
{
    for (int by = 0; by < band.height; ++by) {
        const auto* pl = labels.ptr<int>(by);
        const auto* pr = covRef.ptr<double>(band.y + by);
        const auto* pt = covTest.ptr<double>(band.y + by);
        for (int bx = 0; bx < band.width; ++bx) {
            const int lab = pl[bx];
            if (lab == 0)
                continue;  // 背景（参照无影）不参与存活统计
            const double ref = pr[band.x + bx];
            const double test = pt[band.x + bx];
            alive[static_cast<size_t>(lab)] += test > kSurvivalAliveCov ? 1 : 0;
            // 消失/变淡是**互斥**两态（恰 0.2 归变淡：消失是严格 <，见头文件「边界归属」）。
            if (test < kSurvivalAliveCov)
                ++vanished[static_cast<size_t>(lab)];
            else if (test < kSurvivalFadeFactor * ref)
                ++faded[static_cast<size_t>(lab)];
        }
    }
}

}  // namespace

double umbraAreaSurvival(const cv::Mat& covTest, const cv::Mat& covRef, const cv::Rect& band)
{
    requireFields(covTest, covRef, band, "umbraAreaSurvival");
    std::int64_t num = 0;
    std::int64_t den = 0;
    for (int y = band.y; y < band.y + band.height; ++y) {
        const auto* pt = covTest.ptr<double>(y);
        const auto* pr = covRef.ptr<double>(y);
        for (int x = band.x; x < band.x + band.width; ++x) {
            // 分子分母**同一个阈值** kSurvivalUmbraBin（两阈值不同源 = 同图都不给 1，
            // selftest「同图 → 恰 1」钉住这一格）。
            den += pr[x] > kSurvivalUmbraBin ? 1 : 0;
            num += pt[x] > kSurvivalUmbraBin ? 1 : 0;
        }
    }
    if (den == 0)
        throw std::runtime_error(
            "umbraAreaSurvival：参照臂在本带内**一个本影像素都没有**（cov_ref > 0.5 的面积为 0）。"
            "空分母的存活率无定义——返回 0 会被读成「影子全丢」，返回 1 会被读成「全活」，"
            "两种都是假线索，一律抛。下一步：确认参照臂（D=-1）画了阴影（覆盖度非零闸门 B "
            "判的是全幅，本条判的是带内——全幅有影不代表近场带有影）");
    return static_cast<double>(num) / static_cast<double>(den);
}

UmbraBlobStats umbraBlobStats(const cv::Mat& covTest, const cv::Mat& covRef, const cv::Rect& band)
{
    requireFields(covTest, covRef, band, "umbraBlobStats");
    const cv::Mat mask = buildUmbraMask(covRef, band);
    if (cv::countNonZero(mask) == 0)
        throw std::runtime_error(
            "umbraBlobStats：参照臂在本带内没有本影（分母空）。同 umbraAreaSurvival 的纪律："
            "「一块都没有」与「一块都没活」必须可区分，一律抛");
    // 🔴 8 邻接（不是默认的 4）：细长条影子（主 agent 实测形态 20×60 / 17×61）在 4 邻接下
    // 会被对角缝拆碎，「一条栏杆影」数成几条短影 ⟹ 分档假性偏小、小块档被灌水。
    // selftest「对角链 = 一块」钉住本参数。
    cv::Mat labels;
    cv::Mat stats;
    cv::Mat centroid;
    const int n = cv::connectedComponentsWithStats(mask, labels, stats, centroid, 8);

    std::vector<std::int64_t> alive(static_cast<size_t>(n), 0);
    std::vector<std::int64_t> vanished(static_cast<size_t>(n), 0);
    std::vector<std::int64_t> faded(static_cast<size_t>(n), 0);
    classifyBlobPixels(labels, covTest, covRef, band, alive, vanished, faded);

    UmbraBlobStats s;
    for (int lab = 1; lab < n; ++lab) {
        const std::int64_t area = stats.at<int>(lab, cv::CC_STAT_AREA);
        const int b = bucketOf(area);
        ++s.bucketTotal[static_cast<size_t>(b)];
        // 存活占比 ≥ 0.5 用整数比较（2·alive ≥ area）：浮点除法会在「恰一半」的格子上
        // 给 0.4999999…，把含端点判据洗成不含——selftest「恰一半算活」钉这一格。
        s.bucketAlive[static_cast<size_t>(b)] +=
            2 * alive[static_cast<size_t>(lab)] >= area ? 1 : 0;
        s.refUmbraPx += area;
        s.vanishedPx += vanished[static_cast<size_t>(lab)];
        s.fadedPx += faded[static_cast<size_t>(lab)];
    }
    return s;
}

double bucketSurvival(const UmbraBlobStats& s, int bucket)
{
    if (bucket < 0 || bucket >= kSurvivalBucketCount)
        throw std::runtime_error("bucketSurvival：档号 " + std::to_string(bucket)
            + " 越界（本仪器共 " + std::to_string(kSurvivalBucketCount)
            + " 档）。返回 NaN 蒙过去 = 把调用错误读成「无数据」，拒绝");
    const std::int64_t total = s.bucketTotal[static_cast<size_t>(bucket)];
    if (total == 0)
        return std::numeric_limits<double>::quiet_NaN();  // 空档 = 无数据，不是满分（变异红线）
    return static_cast<double>(s.bucketAlive[static_cast<size_t>(bucket)])
        / static_cast<double>(total);
}

/**
 * **闸门 C｜参照臂自洽 = 恰 1.000**（本判据的阳性锚点）。完整理由住头文件声明处；
 * 本处只补实现侧的一条：四项全部**精确比较**（== 1.0 / == 0），无容差——它是恒等式
 * 不是测量。抛的文案第一嫌疑点名「参照臂不是全量」，因为那是唯一**图像侧**的成因
 * （算子坏了另有 selftest 拦，实机到不了这里）。
 */
void requireReferenceSelfConsistent(double areaFull, double areaNear,
    const UmbraBlobStats& fullStats, const UmbraBlobStats& nearStats)
{
    std::string bad;
    const auto checkArea = [&bad](const char* who, double v) {
        if (v == 1.0)
            return;
        bad += (bad.empty() ? "" : "；") + std::string(who) + " = " + num6(v) + " ≠ 1.000000";
    };
    checkArea("全幅本影面积存活率", areaFull);
    checkArea("近场本影面积存活率", areaNear);
    const auto checkBlobs = [&bad](const char* who, const UmbraBlobStats& s) {
        for (int b = 0; b < kSurvivalBucketCount; ++b) {
            const std::int64_t total = s.bucketTotal[static_cast<size_t>(b)];
            const std::int64_t alive = s.bucketAlive[static_cast<size_t>(b)];
            if (total == 0)
                continue;  // 空档无数据可谈，不算破（NaN 打印由调用方负责成「—」）
            if (alive == total)
                continue;
            bad += (bad.empty() ? "" : "；") + std::string(who) + "第 "
                + std::to_string(b) + " 档存活 " + i64(alive) + "/" + i64(total) + " ≠ 全活";
        }
        if (s.vanishedPx != 0 || s.fadedPx != 0)
            bad += (bad.empty() ? "" : "；") + std::string(who) + "消失 " + i64(s.vanishedPx)
                + " / 变淡 " + i64(s.fadedPx) + " px ≠ 0";
    };
    checkBlobs("全幅", fullStats);
    checkBlobs("近场带", nearStats);
    if (bad.empty())
        return;
    throw std::runtime_error(
        "参照臂自洽闸门：**D=-1 与自己比不是闭式恒等，整张表作废，拒绝出判据**\n"
        "  破项：" + bad + "。\n"
        "  参照臂 = 四级全画原始几何（shadow_lod = -1），它与**自己**比的四项读数（面积/分档/\n"
        "  消失-变淡/近场）是恒等式，闭式**恰** 1.000 / 0，无容差。它不是 1 只有两种成因：\n"
        "  ① 🔴 第一嫌疑 = **参照臂不是全量**——shadow_lod 覆写打偏（键拼错 / 被后面的门盖掉）\n"
        "    ⟹ 参照臂其实带着 LOD，或与某条 test 臂取了同一条；此时全表的分母不是「阴影的\n"
        "    应有样子」，每一项存活率都失去意义。查该臂引擎回显：参照臂必须**没有** `min(D=`\n"
        "    行（出现 = 带着 LOD），test 臂必须有 `min(D=<自己的值>,`。\n"
        "  ② 算子坏了（同图不给自己 1）——`imgqa selftest` 的「同图 → 四项恰 1.000」应同时红；\n"
        "    它绿而本闸门红，才回头查 ①。\n"
        "  ⚠ 本闸门是量具自检不是画质判据：它抛不代表阴影变好或变坏。");
}

/**
 * **闸门 D｜参照臂必须真的与 test 臂不同**（布尔，无阈值）。头文件声明处写着它防的失效
 * （全臂相同 ⟹ 四项读数全 1.000 = 满分绿）与「跳过 D=0 由调用方负责」的分工；
 * 与 seam 闸门 A 的口径同源（判图不判公式、一次报全、空清单抛）。
 */
void requireLodChangesImage(const std::vector<std::pair<std::string, std::int64_t>>& diffByArm)
{
    if (diffByArm.empty())
        throw std::runtime_error(
            "LOD 生效闸门：一条 test 臂都没拿到（空清单）。空清单会**平凡通过** = 闸门看起来"
            "在、其实什么都没查，拒绝放行");
    std::string same;
    for (const auto& [name, diff] : diffByArm) {
        if (diff > 0)
            continue;
        same += (same.empty() ? "" : "、") + name + "（与参照臂 cov 场逐位相同）";
    }
    if (same.empty())
        return;
    throw std::runtime_error(
        "LOD 生效闸门：**见证臂与参照臂逐位相同，LOD 没改到阴影图，全部存活率读数作废**\n"
        "  逐位相同的见证臂：" + same
        + "。\n"
          "  为什么这条是**布尔**判据：闭式只有两个态——相同（D 这个自变量对阴影图一个像素都\n"
          "  没改过）/ 不同。中间没有「差一点点」。全臂相同 ⟹ 四项读数全部 = 1.000 = 满分绿，\n"
          "  一台只看这个的仪器会给「四级一起变差」打满分——本判据立项要补的正是这个洞。\n"
          "  三种成因按可能性排：① `meshlet.shadow_lod` 覆写整体失效（键拼错 = 静默拿出厂值，\n"
          "  各臂同图）——但回显对账（`min(D=<值>,`）应先响，它没响而本条响 = 键被读到了但\n"
          "  阴影侧没有消费者；② 参照臂被改成与 test 同档（E6② 的形态：d5 见证臂点名它）；\n"
          "  ③ 选级表全零/回落策略把每档都画成 L0（引擎会打「零槽可切」WARNING，去日志找它）。\n"
          "  ⚠ 本清单只装**见证臂**（正常态必与参照不同的档，仪器层选定，见 requireReference-\n"
          "  Gates 注释）；小 D（0/1）实测可与参照逐位相同（合法态），误传进来会把正常构建判死。");
}

}  // namespace RGL::imgqa
