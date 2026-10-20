// imgqa CLI 注册表 · **shadow-caster-survival 段**（阴影遮挡体存活率：绝对量，参照 = 全量几何）。
//
// 从 `cli_table.cpp` 另立 TU 的理由与 `cli_table_seam.cpp` / `cli_table_grain.cpp` 逐字相同：
// 主表撞「单文件 < 500 代码行」，而 `specsSwimAndProbe()` 装下本条后撞「单函数 < 120 代码行」，
// 缝按**仪器**切。规矩与主表一致：默认值只准写在这里（本条**无 flag 是设计**）；拼接顺序只在
// `cli_table.cpp` 的 allSpecs()（本段在 shadow-cascade-seam 之后，理由写在那里的注释）。
// help 只从既有权威散文（`shadow_caster_survival.cpp` / `shadow_survival_metric.h` 头注释、
// `judgments_table_survival.cpp`、主 agent Y 实验记录）里摘，**不许新写事实**。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> shadowCasterSurvivalSpecs()
{
    return {
        /* shadow-caster-survival */
        SubcommandSpec{
            "shadow-caster-survival",
            "阴影遮挡体存活率：LOD 不许把细遮挡体的影子弄丢——绝对量判据，参照 = D=-1 全量"
            "几何（18 臂双机位四闸门，跑在交付配置上）",
            {
            },
            {
                "补的是判据网的一个真空洞：上一批的 `shadow-cascade-seam` 量**级间分歧**，主"
                " agent 实测它对「四级一起变差」**结构失明**——把簇 LOD 改成四级全走后，跨级缝"
                "增量 J1 = 0.000 满分绿、级 0 GPU −73%，而肉眼可见地丢影子（最大 8 块丢失区每"
                "块 237–557 像素、覆盖度 0.62–0.78，形态 20×60 / 17×61 的细长条 = 栏杆/窗棂/"
                "挂灯的影子；墙上牛腿的接触阴影整条消失、花箱底下的投影没了）。**一台只看一致"
                "性的仪器，对「四级一起变差」给满分**——本命令量的是「影子活下来多少」。⚠ 现有"
                " `shadow-grain` 在这里也是瞎的：它跑合成靶，而合成靶没有簇产物（清单只有 "
                "bistro；缺产物时引擎只打一条 INFO 就静默回落旧路径）⟹ 根本不进簇路径。",
                "口径（绝对量，参照 = `shadow_lod = -1` 四级全画原始几何 = 阴影的「应有样子」）："
                "① 本影面积存活率 = area(cov_test>0.5)/area(cov_ref>0.5)，**允许 >1**（LOD 轮廓"
                "略胖是实测事实：D=5 = 参照的 100.5%）；② 小块存活率（**主读数**）= 参照本影连通"
                "块（8 邻接）按面积分四档 <50 / 50–200 / 200–1000 / >1000 px，逐档算「块内参照 "
                "cov>0.5 的像素中 test cov 仍 >0.2 的占比 ≥ 0.5」的块比例——🔴 必须分档：实测丢的"
                "正是小块而总面积几乎不变（48.55%→48.77%），只看总面积会报「没问题」；③ 消失"
                "（test cov<0.2）vs 变淡（0.2≤test<0.5·ref）分开计数（**归因**读数：整簇被丢掉改"
                "选级判据、簇塌成零厚度改打包工具）；④ 近场带（行 [500,899]）重算 ①②。cov 与 "
                "shadow-grain / shadow-cascade-seam **同式同序**（线性域、逐通道 clip→均值），"
                "算子自检（同图→四项恰 1.000、全 0→恰 0.000、「只丢小块总面积不变」合成对、边界"
                "每半边一条、守卫各一条抛）在 `imgqa selftest`（无引擎，`just tools-check` 覆盖）。",
                "18 臂 = 2 机位 × 9 臂：`none`（覆盖度分母，每机位一次）+ D ∈ {-1(参照), 0, 1, "
                "2, 3, 5, 7, 11}（`shadow:mode=\"csm\"`，**不钉级**——`csm.force_cascade` 不覆写"
                "且逐臂回显核对差集行**缺席**，出现 = 级被钉住 = 静默换臂即抛）。机位 `near` = "
                "债-139 第三机位（与 seam 同取景，读数可对账）；`far` = 出厂默认机位（`just shot` "
                "那枚，实现 = 不写 camera.lua，不抄坐标 = 不建第二份真相）。两枚都要跑：主 agent "
                "实测过它们的行为不同，一枚不够。",
                "🔴 **四道闸门**（全部抛 rc=3、一条 Verdict 都不产）：① 簇路径必须生效（与 seam "
                "共用 `cluster_path_gate` 一份实现——needle 名单抄两份 = 第二份真相）；② 分母臂在"
                "场 + 覆盖度非零（复用 seam 的 `requireDenominatorAlive`/`requireCoveragePresent`，"
                "分母一失效四项读数全 = 1.000 = 满分绿）；③ **参照臂自洽 = 恰 1.000**（本判据的"
                "阳性锚点：D=-1 与自己比四项读数必须全是闭式恒等，无容差——不是 1 ⟹ 参照臂接线"
                "错了，第一嫌疑 = 参照臂其实没走全量，整张表作废）；④ **参照臂必须真的与 test 臂"
                "不同**（见证臂 = d11 堵「LOD 整体没生效」+ 出厂 d5 堵「参照臂与出厂同档」，与参照 cov 场"
                "不得逐位相同；小 D 实测可与参照逐位相同——LOD 级 1 在阴影图分辨率下与全量不可"
                "区分，是合法态，不能当见证）。",
                "判决线两条（阈值理由与 guard 出处见 `imgqa check`）：J1 `shadow-caster-survival-"
                "factory`（出厂档 D=5 的小块存活率与近场接触存活率两条下限——🔴 **阈值本批不定 "
                "TODO(债-139)**：这台仪器今天才建，拿没推导过的数当门 = 把「实测没超所以钉个数」"
                "写进判据网；未定期只出 INFO，不出 PASS/FAIL、不进 rc。**本批交付物是扫描表 + 四"
                "道闸门，不是一条绿的判据**）· J2 `shadow-caster-survival-monotone`（本影面积存活"
                "率随 D 单调不增，逐机位判；抓区间错位/选级表漂；读数允许 >1，钉的是形状不是 ≤1；"
                "D 点 < 3 抛）。",
                "输出 = 每机位一张扫描表（行 = D，列 = 本影面积存活率 / 四档小块存活率(存活/总数)"
                " / 近场接触存活 / 近场面积 / 消失 vs 变淡 px / 翻转率）——这张表就是「D 取几」的"
                "决策材料，也是后续 shadow-proxy 与轮廓保护切法两条路的公共尺，可直接贴进 "
                "`docs/还债实测数据.md`。同二进制连跑两次逐位一致（llm_shot 钉帧 + 无墙钟输入）。",
                "🔴 必须走**交付配置**（仓库 `configs/`）：① 既有红线「合成 profile 上的绿不算"
                "绿」；② 只有 bistro 有簇产物（`cmake/RglMeshlets.cmake`），合成靶根本不进簇路径。"
                "配置覆写只吃第 4/5/6 三枚例外门（mode / llm_shot / shadow_lod），第 7 枚"
                "（force_cascade）刻意关死。**无 flag 是设计**（同 seam / grain / softness）：机位"
                "/ 尺寸 / 帧号 / 带子 / 分档边界都是量具的组成部分，能调 = 能调绿。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
