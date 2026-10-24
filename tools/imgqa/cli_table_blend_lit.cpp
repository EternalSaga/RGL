// imgqa CLI 注册表 · **blend-lit 段**（债-134 先决：透明段 lit 变体的常驻像素贡献判据）。
//
// 从 `cli_table.cpp` 另立 TU 的理由与 `cli_table_blend.cpp` 逐字相同：主表撞「单文件 < 500
// 代码行」，缝按**仪器**切。规矩与主表一致：默认值只准写在这里；flags 顺序 = 代码读取顺序
// = `imgqa args` 回显顺序 = -h 打印顺序；summary/notes 是判据契约原文，help 只从既有权威
// 散文（`blend_lit.cpp` / `blend_lit_metric.h` 头注释、docs/透明混合管线_plan.md §4）里摘，
// **不许新写事实**。
// ⚠ 拼接顺序只在 `cli_table.cpp` 的 allSpecs()。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> blendLitSpecs()
{
    return {
        /* blend-lit */
        SubcommandSpec{
            "blend-lit",
            "透明段像素贡献判据：BLEND/OPAQUE 双臂同源自截，条数走引擎回显 + 像素按 footprint 分桶（债-134）",
            {
            },
            {
                "输入来自仓库里三份东西：`build/rgl/models/blend_lit.glb` 与 `blend_lit_opaque.glb`"
                "（+ 两份同名 `.manifest.json`，由 `tools/qa_scenes.py` 的**同一个函数** "
                "scene_blend_lit(blend=) 生成 ⟹ 两臂只差 alphaMode，同源是构造性的而不是注释里的），"
                "以及冻结配置目录 `qa/configs/blend_lit`（自截形态 + rpc=true）。本命令只改**副本**的 "
                "scene.lua entities[].file 与 llm_shot.file，冻结目录一个键不碰。缺 .glb 或缺 manifest "
                "直接 FAIL，**不 SKIP**，生成命令打在错误消息里。",
                "六条判据按「引擎侧的账 → 像素侧的账 → 同臂的账 → 过程干净」排：`blend-lit-draws` 引擎回显"
                "（`GET /state` → render.transparentDraws）等于 manifest 声明值 —— 条数是引擎侧的"
                "事实，**不许**数像素反推（那是用被测量证明被测量）；`blend-lit-reverse` 反向用例，"
                "OPAQUE 臂的条数必须**恰为 0**，否则判据 1 的「非 0」是恒真的；"
                "`blend-lit-contributes` 三个 pane footprint 内双臂确有不同；`blend-lit-confined` "
                "footprint **之外**双臂逐位相同（背景板一个字节都不许变）——只有「变了」没有「在哪"
                "变了」，判据可以在任何旁路上成立；`blend-lit-visible` **同臂自比**：pane 的 "
                "footprint 众数必须 ≠ 该臂自己的背景板众数（🔴 变异实测：删掉透明段时「两臂"
                "不同」照旧成立，因为 BLEND 臂露出的是背景板，只有同臂自比会红）；"
                "`blend-lit-clean` 两次启动的 `[error]` 计数为 0。",
                "四条前置自检**不做成 PASS/FAIL，做成立即抛**（rc=3）：两臂 manifest 的几何事实"
                "逐字段不一致（这一处不同就让「双臂不同」不再指向透明段）、camera.lua 与 manifest 的"
                "相机不一致（区域会框到别的几何上而「双臂不同」照旧成立）、背景板铺不满整屏"
                "（「footprint 之外只有背景板」这条归因不成立）、pane 的 footprint 重叠或同一次运行内"
                "条数变化（条数必须是事实而不是采样）。这些的含义是「这次测量没有意义」，"
                "让它输出一条 PASS 是在撒谎。",
                "口径（解析投影 / 区域分桶 / 铺满对账）住在 `blend_lit_metric.cpp`，**不启引擎、"
                "不读文件**，`just imgqa selftest` 用合成两臂小图直接断言分桶与边界。本判据**不预测"
                "任何像素值**（弱判据：透明光照的闭式要复刻 BRDF + IBL + CSM，台账裁「成本 > 收益」），"
                "显示链因此一行都不涉及。",
                "**无 flag 是设计**（同 `blend-sort`）：两臂 / 帧号 / pad / 阈值都是量具的组成部分，"
                "可调 = 能调绿。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
