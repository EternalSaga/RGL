// imgqa CLI 注册表 · **blend-sort 段**（债-132 结案：透明合成次序的闭式判据）。
//
// 从 `cli_table.cpp` 另立 TU 的理由与 `cli_table_diffstruct.cpp` / `cli_table_diffblock.cpp`
// 逐字相同：主表撞「单文件 < 500 代码行」、而 `specsSwimAndProbe()` 装下本条后撞
// 「单函数 < 120 代码行」（145 > 120），缝按**仪器**切。规矩与主表一致：默认值只准写在这里；
// flags 顺序 = 代码读取顺序 = `imgqa args` 回显顺序 = -h 打印顺序；summary/notes 是判据契约
// 原文，help 只从既有权威散文（`blend_sort.cpp` / `blend_sort_metric.h` 头注释、
// docs/透明混合管线_plan.md §5 刀3）里摘，**不许新写事实**。
// ⚠ 拼接顺序只在 `cli_table.cpp` 的 allSpecs()。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> blendSortSpecs()
{
    return {
        /* blend-sort */
        SubcommandSpec{
            "blend-sort",
            "透明合成次序闭式判据：blend_sort 两层 unlit 半透，四区逐字节对拍（债-132）",
            {
            },
            {
                "输入来自仓库里两份东西：`build/rgl/models/blend_sort.glb` + 同名 "
                "`.manifest.json`（`tools/qa_scenes.py` 的 scene_blend_sort 生成：OPAQUE 背景 + "
                "α=0.25 远层 + α=0.5 近层，三层全 unlit ⟹ 着色链塌成「baseColor → 非预乘 over "
                "→ 曝光 → ACES → sRGB」），以及冻结配置目录 `qa/configs/blend_sort`（自截形态，"
                "第 8 帧出图；本命令只改**副本**的 llm_shot.file，冻结目录一个键不碰）。缺 `.glb` "
                "或缺 manifest 直接 FAIL，**不 SKIP**，生成命令打在错误消息里。",
                "四条判据各钉一件事（红灯就按这个顺序归因）：`blend-sort-backdrop` 背景区只钉显示"
                "链（它红 = tonemap 尾段 / ACES / sRGB 变了，与混合无关）· `blend-sort-layer-far` "
                "与 `blend-sort-layer-near` 各钉一个 α 上的非预乘 over（两个 α 是为了防「某个 α "
                "恰好被某种取整蒙对」）· `blend-sort-order` 重叠区按 distanceSq **降序**逐层合成"
                "——前三条绿而它红 = 合成次序错，正是透明管线最典型的错法。",
                "三条前置自检**不做成 PASS/FAIL，做成立即抛**（rc=3）：半透层的 distanceSq 相等"
                "（引擎用 stable_sort，相等键视为已序 ⟹ 比较子 `>` 与 `<` 给出同一个序列 = 变异"
                "结构性不红，而画面可能因作者顺序恰好正确）、camera.lua 与 manifest 的相机不一致"
                "（采样点量到别的几何上而闭式照旧自洽）、判别力 < 8 LSB（分不开对错的检查没有资格"
                "当验收）。这三条的含义是「这次测量没有意义」，让它输出一条 PASS 是在撒谎。",
                "口径（投影 / 合成 / 判别力 / 三条拒跑）住在 `blend_sort_metric.cpp`，**不启引擎、"
                "不读文件**，`just imgqa selftest` 直接断言那四个闭式数与两条拒跑；显示链一行都不"
                "新写，吃 `analytic.cpp` 的 `toBytes`（本项目唯一一份转录）。",
                "**无 flag 是设计**（同 `shadow-softness`）：判据区 / 帧号 / 阈值都是量具的组成"
                "部分，可调 = 能调绿。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
