// 判据契约表 · **透明合成次序段**（`blend-sort-*`，4 条）。
//
// 从 `judgments_table.cpp` 另立 TU 的理由同 `judgments_table_shadow.cpp`：文件代码行护栏把
// 「四条带完整容差理由的条目」装不进任何现成函数，而缝按**仪器**切——本文件四条全部属于
// `imgqa blend-sort` 这一台仪器，成立条件共享「blend_sort 那份合成输入 + 闭式混合式 +
// 逐字节对拍」。族仍是 **Foundation**：与机位运动无关、预测 = 闭式解、比较算子 = 逐位相同。
// ⚠ 拼接顺序只在 `judgments_table.cpp` 的 registeredJudgments()，本文件不抄条数。
//
// 四条的分工（缺一条就退化成「红了不知道红了什么」）：backdrop 管显示链尾段，两条 layer
// 管「单个 α + 非预乘 over」，order 才是本任务的正主。前三条绿而 order 红，唯一还没被锚住
// 的未知量就只剩合成次序——这套分区的意义全在这里。
//
// 四条共用的容差理由（逐条正文各写一遍，因为 `imgqa check` 是按条打印的）：本机四区实测
// **零误差**，但项目三台机器轮换，不把 0 写死；而阈值 1 与「判别力最小 42 LSB」之间隔着
// 40 多档，没有争辩空间。判别力 = 备选错误模型与正确模型的最大字节差（反序 42 / 预乘 95），
// 它不足 8 LSB 时本命令**直接拒跑**，所以「阈值放宽到多少」这个问题不会与「分不分得开」混起来。
#include "judgments_table.h"

namespace RGL::imgqa {

const std::vector<JudgmentInfo>& blendSortJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"blend-sort-backdrop", JudgmentFamily::Foundation,
            "阈值 **1 LSB**（逐通道最大绝对差）。本机四区实测**零误差**，但项目三台机器轮换，"
            "不把 0 写死；判别力最小 42 LSB，1 与 42 之间没有争辩空间。\n"
            "· 期望字节 = `toBytes(背景 albedo 过 G-buffer 8bit sRGB 往返)` = (0,0,204)。背景是 "
            "unlit 纯蓝 (0,0,1)，而 0/1 恰是那条往返的**不动点** ⟹ 闭式里不必模型化量化。\n"
            "· 本条**只**钉显示链尾段（曝光 → Narkowicz ACES → sRGB 编码 → 8bit 取整），与混合"
            "方程、α、次序全部无关——它是这四条里的「地板」：它红时其余三条的读数一律不作数。\n"
            "· 为什么值得单列一条：显示链是四条判据**共用**的尾段。没有它，「ACES 常数被人改了」"
            "会同时让四条红，而现场要查的是 tonemap 而不是排序。",
            {"docs/透明混合管线_plan.md §5 刀3（四区闭式对拍读数表：bg_only 闭式 (0,0,204) = "
             "实测 (0,0,204)，零误差）",
             "tools/imgqa/analytic.cpp 的 toBytes / roundTrip8Srgb（本项目显示链闭式的唯一一份；"
             "本条就是它在透明场景上的锚）"}},
        {"blend-sort-layer-far", JudgmentFamily::Foundation,
            "阈值 **1 LSB**，与其余三条同值。本机实测零误差，三台机器轮换 ⟹ 不写死 0；"
            "判别力最小 42 LSB，中间没有争辩空间。\n"
            "· 期望 = `toBytes(α_far·c_far + (1−α_far)·bg)`，α_far = 0.25、c_far = (0,1,0)、"
            "bg = (0,0,1) ⟹ 线性 (0, 0.25, 0.75) = 2 的幂的有限和，透明段写 RGBA16F ⟹ half "
            "精度**逐位精确**，闭式与实机之间没有舍入差可争。\n"
            "· 本条红而 backdrop 绿 = **混合方程或 α 通路**变了（`rgl/shader.cppm` 的 colorBlend "
            "分支：SRC_ALPHA / ONE_MINUS_SRC_ALPHA），与 tonemap 尾段无关。\n"
            "· 判别力实测：把非预乘的 SRC_ALPHA 读成 ONE（预乘模型）时本区给 **95 LSB** 差"
            "（(0,0.25,0.75) 与 (0,1,0.75) 之别）——阈值 1 与 95 之间隔着 94 档。",
            {"docs/透明混合管线_plan.md §5 刀3（far_only 闭式 (0,113,187) = 实测 (0,113,187)；"
             "判别力读数：预乘混合 95 LSB）",
             "rgl/shader.cppm 的 colorBlend 分支（非预乘 SRC_ALPHA/ONE_MINUS_SRC_ALPHA —— 本条"
             "钉的就是这条方程）"}},
        {"blend-sort-layer-near", JudgmentFamily::Foundation,
            "阈值 **1 LSB**，与其余三条同值。本机实测零误差，三台机器轮换 ⟹ 不写死 0。\n"
            "· 期望 = `toBytes(α_near·c_near + (1−α_near)·bg)`，α_near = **0.5**（与远层的 0.25 "
            "不同）⟹ 线性 (0.5, 0, 0.5)。\n"
            "· 为什么必须有第二个 α 而不是只测一个：单点命中「α × src + (1−α) × dst」有两种非"
            "因果解释——方程对、或某个 α 恰好被某种取整蒙对（α=0.5 时 src 与 dst 的权重相等，"
            "把方程写成 `0.5·src + 0.5·dst` 这类错也照样命中）。两个互不相同的 α 各自命中一次，"
            "这两条路一起堵掉。\n"
            "· 本条与上一条的分工：两条同绿才是一句「非预乘 over 在 α∈{0.25,0.5} 上都对」。",
            {"docs/透明混合管线_plan.md §5 刀3（near_only 闭式 (158,0,159) = 实测 (158,0,159)）",
             "tools/qa_scenes.py 的 scene_blend_sort（两层 α 刻意取 0.25 / 0.5 两个不同值，"
             "理由就写在该场景的设计要点注释里）"}},
        {"blend-sort-order", JudgmentFamily::Foundation,
            "阈值 **1 LSB**，与其余三条同值。本机实测零误差，三台机器轮换 ⟹ 不写死 0；"
            "**判别力 42 LSB**（反序模型与正确模型的最大字节差），阈值与判别力之间隔着 41 档。\n"
            "· 期望 = 重叠区按 **distanceSq 降序**（远先画）逐层 over 的闭式：先 α=0.25 的远层、"
            "再 α=0.5 的近层 ⟹ 线性 (0.5, 0.125, 0.375) = (162,73,141)。\n"
            "· 🔴 **变异实测（本条能红的直接证据）**：把 `rgl/render_list_impl.cppm` 的"
            " `a.distanceSq > b.distanceSq` 改成 `<` 重新编译，本区实测变 **(142,115,142)** = "
            "**正好等于**按错误次序合成的闭式预测，而其余三区照旧逐字节命中。⟹ 本判据不是差分"
            "噪声计，它两种次序都预测得中，红的时候能一口咬定是次序而不是「整条链都可疑」。\n"
            "· 为什么前三条绿而本条红 = 合成次序错：backdrop 锚住显示链、两条 layer 各锚住"
            "「单个 α + 非预乘 over」，重叠区新增的**唯一未知量**就是逐层合成的先后。\n"
            "· 排序键退化时本命令**拒跑**（rc=3）而不是判绿：引擎用 stable_sort，相等 distanceSq "
            "视为已序 ⟹ 比较子 `>` 与 `<` 给出同一个序列，变异结构性不红，而画面可能因作者顺序"
            "恰好正确 = 一条恒真的判据。",
            {"docs/透明混合管线_plan.md §5 刀3 的变异对照（> → < 之后 overlap 实测 (142,115,142) "
             "= 反序闭式预测，其余三区照旧零误差命中；还原后复绿）",
             "docs/透明混合管线_plan.md §5 刀3 的判别力自检（反序 42 / 预乘 95，阈值 8 LSB 不足"
             "即拒跑）",
             "rgl/render_list_impl.cppm 的 stable_sort 比较子与 distanceSq 代理键（= 变异要改的"
             "那一行，也是本条唯一约束的量）",
             "tools/qa_scenes.py 的 Piece.translation（排序代理是 node 原点；「每层一个 node "
             "平移」这条设计就是为了让本条**能**红）"}},
    };
    return kPart;
}

}  // namespace RGL::imgqa
