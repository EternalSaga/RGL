// selftest_survival.cpp —— `shadow-caster-survival`（阴影遮挡体存活率）**口径层算子自检**，无引擎。
//
// 另立 TU（不并进 `selftest_seam.cpp`）的理由 = 该文件头注释把它的缝声明为「shadow-cascade-seam
// 口径层（相邻级联阴影分歧）」——本仪器测的是**绝对量存活率**（面积/分档连通块/消失-变淡），
// 不是差分场的台阶与翻转率，算子面完全不同。切法先例 = `selftest_block.cpp` / `selftest_grain.cpp`
// （一台仪器一个自检 TU，跨 TU 只共享 `selftest.h` 的 Checker）。
//
// ## 这些断言钉的是「算子数的是我说的那个量」，期望值全部闭式（不抄实机读数、不抄被测码）
//   ① 完全相同的两图 → 四项存活率全 = 恰 1.000（且闸门 C 对**真算子产出**放行——接线自检）
//   ② test 全 0 → 面积存活率 = 恰 0.000、每档存活率 = 恰 0.000、消失 = 参照本影全体
//   ③ 🔴 **立命断言**：「只丢小块、总面积不变」合成对 → 面积存活率恰 1.000 而小块档恰 0.000
//     ——这条证明「只看总面积」确实会漏，也就是证明本判据不是 shadow-cascade-seam 的换皮
//   ④ 变淡与消失各一条闭式（含「变淡上界是 0.5·ref 不是常数 0.5」的分家用例）
//   ⑤ 边界每半边一条：cov 恰 0.5 不计影 / 恰 0.2 归变淡不归消失也不算活 / 恰 0.5·ref 算活 /
//     恰一半存活算活（整数比较 2·alive ≥ area）
//   ⑥ 分档边界 49/50、199/200、999/1000 各落正确档
//   ⑦ 8 邻接：对角链 = 一块（4 邻接变异给 3 块）
//   ⑧ 空档 = NaN 不是 1.0；打印成「—」不是「1.000」
//   ⑨ 守卫各一条 throwsStd（band 空 / 越界 / 尺寸不同 / 非 CV_64FC1 / 参照本影空 / 档号越界）
//   ⑩ 闸门 C 五条（恒等不抛 / 面积 0.999 抛 / 档存活破抛 / 消失非零抛 / 错误串可归因）
//     闸门 D 五条（正常不抛 / 一条相同抛且点名 / 两条一次报全 / 空清单抛 / 反向对照）
//     两条判决线挂号。
//
// ## 变异红线（哪个变异让我红；实跑记录见交付说明）
//   · 本影阈值 `> 0.5` 写成 `>= 0.5` → 「恰 0.5 不计影」红；分子分母阈值不同源 → 「同图恰 1」红。
//   · 存活像素 `> 0.2` 弱化成 `>= 0.2` → 「恰 0.2 不算活」红；删掉 → 「test 全 0 存活率恰 0」红。
//   · 消失 `< 0.2` 写成 `<= 0.2` → 「恰 0.2 记变淡不记消失」红。
//   · 变淡上界 `0.5·ref` 写成常数 0.5 → 「ref=0.8 test=0.45 算活」分家用例红。
//   · 块存活 `2·alive >= area` 写成 `>` → 「恰一半算活」红。
//   · 分档 `< 50` 写成 `<= 50` → 「面积 50 落第二档」红（三条边界用例至少一条红）。
//   · 8 邻接改 4 → 「对角链 = 一块」红（给 3 块）。
//   · 空档 NaN 改成 1.0 → 「空档 = 无数据不是满分」红；打印「—」改成「1.000」→ 打印用例红。
//   · 守卫任一改成返回 0 → 对应那条 throwsStd 红（返回 0 会被读成「全丢」或「没数」，都错）。
//   · 闸门 C 比较反向（永不抛）→ 「面积 0.999 应抛」红；放宽成容差（≥0.99 放行）→ 同红。
//   · 闸门 D `diff > 0` 恒真化 → 「有一条臂逐位相同应抛」红；反向（永不放行）→ 「正常态不抛」红。
//   · 两道闸门空清单守卫删掉 → 对应「空清单应抛」红（平凡通过的闸门比没有闸门更坏）。
#include "selftest.h"

#include "imgqa.h"
#include "shadow_caster_survival.h"  // 闸门 C/D 的调用面 + 打印格式化
#include "shadow_survival_metric.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {
namespace {

/// 解析构造的 cov 场（自检输入全部闭式，不吃截图）。
cv::Mat field(int cols, int rows, double (*f)(int, int))
{
    cv::Mat m(rows, cols, CV_64FC1);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < cols; ++x)
            m.at<double>(y, x) = f(x, y);
    return m;
}

double fZero(int, int)
{
    return 0.0;
}

/// 矩形 [x0,x1)×[y0,y1)（面积 = (x1−x0)·(y1−y0)，闭下开上，边界用例靠它精确计数）。
struct Rect4 {
    int x0, y0, x1, y1;
    [[nodiscard]] int64_t area() const { return static_cast<int64_t>(x1 - x0) * (y1 - y0); }
};

/// 铺若干矩形（后铺的覆盖先铺的——「200 矩形挖 1 像素 = 199」这类用例靠它）。
cv::Mat blobField(int cols, int rows, const std::vector<std::pair<Rect4, double>>& blobs)
{
    cv::Mat m(rows, cols, CV_64FC1, cv::Scalar(0.0));
    for (const auto& [r, v] : blobs)
        for (int y = r.y0; y < r.y1; ++y)
            for (int x = r.x0; x < r.x1; ++x)
                m.at<double>(y, x) = v;
    return m;
}

const cv::Rect kAll80(0, 0, 80, 60);

}  // namespace

/// `shadow-caster-survival` 口径层：面积存活率 / 分档连通块存活率 / 消失-变淡归因的闭式刻度，
/// 期望全部闭式，不启引擎。
void testShadowCasterSurvival(Checker& c)
{
    // ── ① 完全相同的两图 → 四项存活率全 = 恰 1.000（阳性锚点的算子侧）────────────────
    // 两块影：6×6=36（<50 档）+ 10×10=100（50–200 档），间距 ≥2 保证 8 邻接也不粘连。
    const cv::Mat ref2 = blobField(80, 60,
        { { { 2, 2, 8, 8 }, 1.0 }, { { 20, 2, 30, 12 }, 1.0 } });
    c.chk("surv 同图：面积存活率恰 1.000（分子分母同一阈值，同图必须给恰 1）",
        umbraAreaSurvival(ref2, ref2, kAll80) == 1.0);
    const UmbraBlobStats self1 = umbraBlobStats(ref2, ref2, kAll80);
    c.chk("surv 同图：每档存活率恰 1.000、消失 = 变淡 = 0（四项全恒等）",
        bucketSurvival(self1, 0) == 1.0 && bucketSurvival(self1, 1) == 1.0
            && self1.vanishedPx == 0 && self1.fadedPx == 0 && self1.refUmbraPx == 136);
    c.chk("surv 同图：闸门 C 对**真算子产出**放行（接线自检——只喂手搓结构体钉不住调用面）",
        !throwsStd([&] { requireReferenceSelfConsistent(1.0, 1.0, self1, self1); }));

    // ── ② test 全 0 → 存活率 = 恰 0.000 ─────────────────────────────────────────────
    const cv::Mat zero = field(80, 60, fZero);
    c.chk("surv 全 0：面积存活率恰 0.000（影子全丢的闭式态）",
        umbraAreaSurvival(zero, ref2, kAll80) == 0.0);
    const UmbraBlobStats dead = umbraBlobStats(zero, ref2, kAll80);
    c.chk("surv 全 0：每档存活率恰 0.000、消失 = 参照本影全体、变淡 = 0",
        bucketSurvival(dead, 0) == 0.0 && bucketSurvival(dead, 1) == 0.0
            && dead.vanishedPx == 136 && dead.fadedPx == 0);

    // ── ③ 🔴 立命断言：「只丢小块、总面积不变」→ 面积存活率恰 1.000 而小块档恰 0.000 ──
    // 这条是整台仪器的存在理由：主 agent 实测丢的正是小块（栏杆/窗棂细长条）而总面积几乎不变
    // （48.55%→48.77%）。只看总面积的判据在这一对上读 1.000 = 「没问题」——**这就是本判据不是
    // shadow-cascade-seam 换皮的机器可执行证明**。合成 = 大块原样 + 4 块小块整体平移（面积和不变）。
    const cv::Mat refSmall = blobField(80, 60,
        { { { 0, 0, 40, 40 }, 1.0 }, { { 50, 0, 54, 4 }, 1.0 }, { { 50, 5, 54, 9 }, 1.0 },
            { { 50, 10, 54, 14 }, 1.0 }, { { 50, 15, 54, 19 }, 1.0 } });
    const cv::Mat testMoved = blobField(80, 60,
        { { { 0, 0, 40, 40 }, 1.0 }, { { 60, 0, 64, 4 }, 1.0 }, { { 60, 5, 64, 9 }, 1.0 },
            { { 60, 10, 64, 14 }, 1.0 }, { { 60, 15, 64, 19 }, 1.0 } });
    c.chk("surv 立命：只丢小块、总面积不变 → 面积存活率**恰 1.000**（只看总面积确实会漏）",
        umbraAreaSurvival(testMoved, refSmall, kAll80) == 1.0);
    const UmbraBlobStats moved = umbraBlobStats(testMoved, refSmall, kAll80);
    c.chk("surv 立命：同一样本小块档（<50px）存活率**恰 0.000**、大块档恰 1.000（分档才现形）",
        bucketSurvival(moved, 0) == 0.0 && bucketSurvival(moved, 3) == 1.0
            && moved.bucketTotal[0] == 4 && moved.bucketTotal[3] == 1);
    c.chk("surv 立命：同一样本消失 = 64px（4×16 小块全体）、变淡 = 0（整块被丢掉不是变浅）",
        moved.vanishedPx == 64 && moved.fadedPx == 0,
        "vanished=" + std::to_string(static_cast<long long>(moved.vanishedPx))
            + " faded=" + std::to_string(static_cast<long long>(moved.fadedPx))
            + " refUmbraPx=" + std::to_string(static_cast<long long>(moved.refUmbraPx)));

    // ── ④ 变淡与消失各一条闭式 ──────────────────────────────────────────────────────
    const cv::Mat ref10 = blobField(80, 60, { { { 10, 10, 20, 20 }, 1.0 } });  // 10×10 = 100
    const cv::Mat fadedOnly = blobField(80, 60, { { { 10, 10, 20, 20 }, 0.3 } });
    const UmbraBlobStats f1 = umbraBlobStats(fadedOnly, ref10, kAll80);
    c.chk("surv 变淡闭式：ref=1.0 test=0.3 → 变淡 100px、消失 0、块**仍算活**（浅影不是没影）",
        f1.fadedPx == 100 && f1.vanishedPx == 0 && bucketSurvival(f1, 1) == 1.0);
    const cv::Mat goneOnly = blobField(80, 60, { { { 10, 10, 20, 20 }, 0.1 } });
    const UmbraBlobStats v1 = umbraBlobStats(goneOnly, ref10, kAll80);
    c.chk("surv 消失闭式：ref=1.0 test=0.1 → 消失 100px、变淡 0、块死（与变淡分家）",
        v1.vanishedPx == 100 && v1.fadedPx == 0 && bucketSurvival(v1, 1) == 0.0);
    // 变淡上界是 **0.5·ref** 不是常数 0.5：ref=0.8、test=0.45 → 0.45 < 0.5（常数版记淡）但
    // ≥ 0.5·0.8 = 0.4（真值记活）。常数化变异只红这一条。
    const cv::Mat ref08 = blobField(80, 60, { { { 10, 10, 20, 20 }, 0.8 } });
    const cv::Mat test045 = blobField(80, 60, { { { 10, 10, 20, 20 }, 0.45 } });
    const UmbraBlobStats f2 = umbraBlobStats(test045, ref08, kAll80);
    c.chk("surv 变淡上界 = 0.5·ref 不是常数 0.5：ref=0.8 test=0.45 → 算活（常数化变异在这里红）",
        f2.fadedPx == 0 && f2.vanishedPx == 0 && bucketSurvival(f2, 1) == 1.0);

    // ── ⑤ 边界每半边一条（对称判据每半边各一条）───────────────────────────────────
    const cv::Mat halfBin = blobField(80, 60, { { { 10, 10, 20, 20 }, 0.5 } });
    c.chk("surv 本影严格 >：test cov 恰 0.5 **不计影**（面积存活率 0；>= 变异在这里红）",
        umbraAreaSurvival(halfBin, ref10, kAll80) == 0.0);
    const cv::Mat at02 = blobField(80, 60, { { { 10, 10, 20, 20 }, 0.2 } });
    const UmbraBlobStats b02 = umbraBlobStats(at02, ref10, kAll80);
    c.chk("surv 0.2 线：test cov 恰 0.2 → 不归消失、不算活、归变淡（三态归属各钉一半）",
        b02.vanishedPx == 0 && b02.fadedPx == 100 && bucketSurvival(b02, 1) == 0.0);
    const cv::Mat atHalfRef = blobField(80, 60, { { { 10, 10, 20, 20 }, 0.5 } });
    const UmbraBlobStats bh = umbraBlobStats(atHalfRef, ref10, kAll80);
    c.chk("surv 变淡严格 <：test cov 恰 = 0.5·ref → 不算变淡、算活（<= 变异在这里红）",
        bh.fadedPx == 0 && bh.vanishedPx == 0 && bucketSurvival(bh, 1) == 1.0);
    // 恰一半存活：8px 块中 4px 活 → 2·4 ≥ 8 → 活；3px 活 → 6 < 8 → 死。整数比较含端点。
    const cv::Mat ref8 = blobField(80, 60, { { { 10, 10, 14, 12 }, 1.0 } });  // 4×2 = 8
    const cv::Mat alive4px = blobField(80, 60, { { { 10, 10, 14, 11 }, 1.0 } });  // 上行 4px 活
    const cv::Mat alive3px = blobField(80, 60, { { { 10, 10, 13, 11 }, 1.0 } });  // 3px 活
    c.chk("surv 块存活含端点：8px 块活 4px（恰一半）→ 活；活 3px → 死（> 变异只红一半边）",
        bucketSurvival(umbraBlobStats(alive4px, ref8, kAll80), 0) == 1.0
            && bucketSurvival(umbraBlobStats(alive3px, ref8, kAll80), 0) == 0.0);

    // ── ⑥ 分档边界 49/50、199/200、999/1000 各落正确档 ─────────────────────────────
    // 199 = 200 矩形挖左上角 1 像素（挖点法保证连通；面积精确 199）。
    cv::Mat bucketField = blobField(160, 120,
        { { { 2, 2, 9, 9 }, 1.0 },      // 49 → 档 0
            { { 20, 2, 30, 7 }, 1.0 },  // 50 → 档 1
            { { 40, 2, 60, 12 }, 1.0 }, // 200 → 档 2（先铺，下面挖成 199）
            { { 70, 2, 90, 12 }, 1.0 }, // 200 → 档 2
            { { 2, 20, 39, 47 }, 1.0 }, // 37×27 = 999 → 档 2
            { { 50, 20, 90, 45 }, 1.0 } } );  // 40×25 = 1000 → 档 3
    bucketField.at<double>(2, 40) = 0.0;  // 挖点：40,2 起那块变 199
    const UmbraBlobStats bk = umbraBlobStats(bucketField, bucketField, cv::Rect(0, 0, 160, 120));
    c.chk("surv 分档边界：<50 / 50–200 / 200–1000 / ≥1000 各含下界不含上界（49|50, 199|200, "
          "999|1000 → 档数 1/2/2/1）",
        bk.bucketTotal[0] == 1 && bk.bucketTotal[1] == 2 && bk.bucketTotal[2] == 2
            && bk.bucketTotal[3] == 1 && bk.refUmbraPx == 49 + 50 + 199 + 200 + 999 + 1000);

    // ── ⑦ 8 邻接：对角链 = 一块（4 邻接变异给 3 块，分档立刻失真）───────────────────
    const cv::Mat diag = blobField(20, 20,
        { { { 2, 2, 3, 3 }, 1.0 }, { { 3, 3, 4, 4 }, 1.0 }, { { 4, 4, 5, 5 }, 1.0 } });
    const UmbraBlobStats dg = umbraBlobStats(diag, diag, cv::Rect(0, 0, 20, 20));
    c.chk("surv 8 邻接：对角 3 像素 = **一块**（area 3 落 <50 档；4 邻接变异给 3 块）",
        dg.bucketTotal[0] == 1 && dg.refUmbraPx == 3);

    // ── ⑧ 空档 = NaN 不是 1.0；打印成「—」 ─────────────────────────────────────────
    const UmbraBlobStats onlyBig = umbraBlobStats(ref2, ref2, cv::Rect(15, 0, 60, 30));
    c.chk("surv 空档：无样本的档存活率 = NaN 不是 1.0（把没数读成满分 = 恒真断言的图像版）",
        std::isnan(bucketSurvival(onlyBig, 0)) && bucketSurvival(onlyBig, 1) == 1.0);
    c.chk("surv 空档打印：NaN → 「—」不是「1.000」（survFixed3 / survBucketCell 两处）",
        survFixed3(std::nan("")) == "—" && survBucketCell(std::nan(""), 0, 0) == "— (0/0)");

    // ── 带子纪律：块只在带内生长，带外不参与（分子分母同带）─────────────────────────
    const cv::Mat twoBlobs = blobField(40, 40,
        { { { 2, 2, 6, 6 }, 1.0 }, { { 30, 30, 34, 34 }, 1.0 } });
    const cv::Rect kWin(0, 0, 20, 20);
    c.chk("surv 带子：带外本影不进统计（refUmbraPx = 带内 16；夹边算会给 32）",
        umbraBlobStats(twoBlobs, twoBlobs, kWin).refUmbraPx == 16
            && umbraAreaSurvival(twoBlobs, twoBlobs, kWin) == 1.0);
}

/// 守卫与闸门面（从 `testShadowCasterSurvival` 拆出 = 按**缝**切：上面测「输入 → 读数」的闭式
/// 数值，这里测「输入不合法 ⟹ 抛」与「判据在不在注册表里」——改口径动上面，加守卫动这里）。
void testShadowCasterSurvivalGuards(Checker& c)
{
    const cv::Mat ref = blobField(80, 60, { { { 2, 2, 8, 8 }, 1.0 } });
    const cv::Mat zero = field(80, 60, fZero);
    const cv::Rect all(0, 0, 80, 60);

    // ── 守卫：一律**抛**，返回 0 会被读成「全丢」或「没数」─────────────────────────
    c.chk("surv 守卫：band 为空立刻抛（空分母的「存活率 0」= 假绿）",
        throwsStd([&] { (void)umbraAreaSurvival(ref, ref, cv::Rect(0, 0, 0, 60)); }));
    c.chk("surv 守卫：band 越界立刻抛（不夹到图像边缘再算）",
        throwsStd([&] { (void)umbraAreaSurvival(ref, ref, cv::Rect(60, 0, 40, 60)); }));
    c.chk("surv 守卫：两图尺寸不同立刻抛（不是同一机位同一帧，存活率没有意义）",
        throwsStd([&] { (void)umbraAreaSurvival(field(70, 60, fZero), ref, all); }));
    c.chk("surv 守卫：非 CV_64FC1 的场立刻抛（读错内存的「0 存活」是假绿）",
        throwsStd([&] {
            (void)umbraAreaSurvival(cv::Mat(60, 80, CV_32FC1, cv::Scalar(0)), ref, all);
        }));
    c.chk("surv 守卫：参照本影为空立刻抛（面积版与分块版各一条——空分母两种误读都不许给）",
        throwsStd([&] { (void)umbraAreaSurvival(zero, zero, all); })
            && throwsStd([&] { (void)umbraBlobStats(ref, zero, all); }));
    c.chk("surv 守卫：档号越界立刻抛（返回 NaN 蒙过去 = 把调用错误读成「无数据」）",
        throwsStd([&] { (void)bucketSurvival(UmbraBlobStats{}, kSurvivalBucketCount); })
            && throwsStd([&] { (void)bucketSurvival(UmbraBlobStats{}, -1); }));

    // ── 闸门 C｜参照臂自洽 = 恰 1.000（闭式恒等，无容差）───────────────────────────
    UmbraBlobStats okStats;
    okStats.bucketTotal = { 4, 2, 1, 0 };
    okStats.bucketAlive = { 4, 2, 1, 0 };  // 空档（0/0）不算破
    std::string msgC;
    bool threwC = false;
    c.chk("surv 闸门C：恒等读数 → 不抛（含空档；闸门若恒抛 = 仪器永久红）",
        !throwsStd([&] { requireReferenceSelfConsistent(1.0, 1.0, okStats, okStats); }));
    try {
        requireReferenceSelfConsistent(0.999, 1.0, okStats, okStats);  // 面积差 0.001 也抛
    } catch (const std::exception& e) {
        threwC = true;
        msgC = e.what();
    }
    c.chk("surv 闸门C：面积存活率 0.999 → 抛（**无容差**；放宽成 ≥0.99 的变异在这里红）", threwC,
        threwC ? "" : "参照臂自洽是恒等式不是测量，任何容差都会把「参照带着 LOD」放过去");
    c.chk("surv 闸门C：错误串点名破项 + 第一嫌疑「参照臂不是全量」（可直接归因）",
        msgC.find("0.999000") != std::string::npos && msgC.find("参照臂不是全量") != std::string::npos,
        msgC.substr(0, 90));
    UmbraBlobStats badAlive = okStats;
    badAlive.bucketAlive[0] = 3;  // 4 块只活 3
    c.chk("surv 闸门C：档存活 3/4 → 抛（同图自己比自己不可能死一块）",
        throwsStd([&] { requireReferenceSelfConsistent(1.0, 1.0, badAlive, okStats); }));
    UmbraBlobStats badVanish = okStats;
    badVanish.vanishedPx = 1;
    c.chk("surv 闸门C：消失 = 1px → 抛（自比不可能有像素消失）",
        throwsStd([&] { requireReferenceSelfConsistent(1.0, 1.0, okStats, badVanish); }));

    // ── 闸门 D｜参照臂必须真的与 test 臂不同（布尔，无阈值）────────────────────────
    const auto alive
        = std::vector<std::pair<std::string, std::int64_t>> { { "d1", 45251 }, { "d5", 7 } };
    c.chk("surv 闸门D：每条 test 臂与参照都有逐位差异 → 不抛（正常态被拦死 = 仪器永久红）",
        !throwsStd([&] { requireLodChangesImage(alive); }));
    std::string msgD;
    bool threwD = false;
    try {
        requireLodChangesImage({ { "d1", 45251 }, { "d5", 0 } });
    } catch (const std::exception& e) {
        threwD = true;
        msgD = e.what();
    }
    c.chk("surv 闸门D：有一条 test 臂与参照逐位相同 → 抛（E6② 的形态：参照与 test 同档）", threwD,
        threwD ? "" : "闸门放行了「全臂相同 ⟹ 四项读数全 1.000 = 满分绿」那个失效");
    c.chk("surv 闸门D：错误串点名死臂 d5、不误指活臂 d1（可直接归因）",
        msgD.find("d5") != std::string::npos && msgD.find("d1（") == std::string::npos,
        msgD.substr(0, 90));
    std::string msgD2;
    try {
        requireLodChangesImage({ { "d1", 0 }, { "d11", 0 } });
    } catch (const std::exception& e) {
        msgD2 = e.what();
    }
    c.chk("surv 闸门D：两条同时相同 → 一次报全（逐条抛会把第二条藏到下一轮）",
        msgD2.find("d1") != std::string::npos && msgD2.find("d11") != std::string::npos,
        msgD2.substr(0, 90));
    c.chk("surv 闸门D：空清单 → 抛（平凡通过的闸门比没有闸门更坏，同 seam 的 A/B）",
        throwsStd([&] { requireLodChangesImage({}); }));

    // ── 两条判决线必须挂号（改名漏挂 = verdictFor 运行期才炸，太晚）─────────────────
    for (const char* name :
        { "shadow-caster-survival-factory", "shadow-caster-survival-monotone" }) {
        bool found = false;
        for (const auto& j : registeredJudgments())
            found = found || std::string(j.name) == name;
        c.chk("surv 判据已挂号进注册表", found, name);
    }
}

}  // namespace RGL::imgqa
