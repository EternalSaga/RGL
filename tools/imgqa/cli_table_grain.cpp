// imgqa CLI 注册表 · **shadow-grain 段**（影场颗粒度：出厂档不许吃掉真实影带）。
//
// 从 `cli_table.cpp` 另立 TU 的理由与 `cli_table_blend.cpp` / `cli_table_diffblock.cpp`
// 逐字相同：主表撞「单文件 < 500 代码行」，而 `specsSwimAndProbe()` 装下本条后撞
// 「单函数 < 120 代码行」，缝按**仪器**切。规矩与主表一致：默认值只准写在这里（本条
// **无 flag 是设计**）；拼接顺序只在 `cli_table.cpp` 的 allSpecs()（本段在 blend-sort
// 之后，理由写在那里的注释，本文件不重述）。
// help 只从既有权威散文（`shadow_grain.cpp` / `shadow_grain_metric.h` 头注释、
// `qa_scenes.py` 场景 10 头注）里摘，**不许新写事实**。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> shadowGrainSpecs()
{
    return {
        /* shadow-grain */
        SubcommandSpec{
            "shadow-grain",
            "影场颗粒度：出厂档不许吃掉真实影带——逐缝深带存活率 + slope_bias 接线阳性"
            "锚点 + 出厂档对账（六臂三判决线）",
            {
            },
            {
                "补的是判据网的结构性洞：`imgqa swim` 测**世界不变性**（同一世界点的影值"
                "变没变）——那条性质是对的且确实该绿；而门板自阴影痤疮**钉死在世界表面**，"
                "swim 对它结构性失明。本命令测影场在世界空间上的**高频程度**：共面墙 + 缝槽"
                "几何上覆盖度只能沿闭式槽带各过渡一次（带在场密度 = 2·count/(cols−1)），"
                "除此之外反复翻转 = 噪声不是影子。两条合起来才封住「影子对不对」的两问。",
                "判决方向（2026-10-19 纠正）：守「**出厂档真影带存活率下限**」，不是首版的"
                "「出厂态密度必须低」——slope 扫描表显示 8→16 的稳定性「改善」与真实影带被"
                "抹到 0% 发生在同一段（抹得越干净读数越绿的旧方向，会把影子整个删掉判满分）。"
                "扫描表全文住 `shadow_grain.cpp` 头注与 `imgqa check` 的容差理由。",
                "口径：cov = mean_RGB clip((off−on)/(off+1e-4),0,1) **线性域**（on/off = 同机位"
                "同帧只差 shadow.lua mode=csm/none）；区域 density = bin(cov>0.5) 的水平跳变数"
                " ÷ rows×(cols−1)；逐带 survival = 带窗内跳变数 ÷ (2·rows)（在场 = 1、被抹 = 0，"
                "阈值取两闭式态中点，不抄实测）。算子自检（棋盘/竖条纹=1.0、34 宽斜坡=1/33、"
                "常数=0、带在场存活率=1.0）在 `imgqa selftest`（无引擎，`just tools-check` 覆盖）。",
                "靶 = 程序化 `models/shadow_grain.glb`（`tools/qa_scenes.py` 场景 10）：朝光墙"
                "（N·L̂=0.6）+ **多缝深组**（每组 4 条竖直缝槽、背板各退本组 d；bistro 门板"
                "读图定案形态的程序化重造）。缝深加密守的是 `ShadowCsmTuning::slopeBias` "
                "注释挂号的盲区「更细的缝会不会在 8 之下就被提前抹掉」。槽底闭式影带"
                "s = d·du/dn，分离窗 d ∈ span·(0.0015/dn+1.5/1024, 0.0015/dn+17.5/1024)。"
                "判据窗口 = manifest `measure` 块世界角点的投影（区域来自几何，不是"
                "屏幕矩形）；C++ 不重抄几何常量。缺 `.glb`/manifest 直接 FAIL，**不 SKIP**。",
                "六臂：`none`（参照）· `dflt`（出厂：shadow.lua **不写** slope_bias 键，吃"
                "引擎默认）· `x8`（显式 slope_bias=8.0，对账伙伴）· `deliv`（仓库 "
                "configs/shadow.lua 全文，对账伙伴）· `csm0`（slope_bias=0：在场闸门 + J2 "
                "低臂）· `csm32`（slope_bias=32：J2 高臂）。每臂对账引擎回显（mode 路线行 + "
                "csm0/csm32 的差集行），静默换臂 = 抛。三条判决线：J1 "
                "`shadow-grain-band-survival`（出厂档逐带存活率 ≥ 50%，取最严一条）· J2 "
                "`shadow-grain-wiring-anchor`（0 臂与 32 臂密度差 ≥ 50%×闭式刻度且高臂 ≤ "
                "半条带增量——**它红时 J1 的绿不作数**）· J3 `shadow-grain-factory-identity`"
                "（dflt ≡ x8 ≡ deliv 全图逐位——J1 读数成为陈述的 precondition，代码默认或 "
                "configs 出厂值任一漂了它立刻红）。另有逐条缝的在场闸门（跑在 csm0 臂：带"
                "中央 50% 列区间 cov>0.5 + 受光参照<0.5，画不出带的死缝 = 抛 rc=3，不许"
                "静默进 J1；blend-sort 前置自检先例）。阈值理由与 guard 出处见 `imgqa check`。",
                "**无 flag 是设计**（同 shadow-softness / shadow-kernel-scale）：靶 / 臂 / "
                "帧号 / 阈值都是量具的组成部分，能调 = 能调绿。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
