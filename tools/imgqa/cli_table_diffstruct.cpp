// imgqa CLI 注册表 · **diffstruct 段**（Phase 15 批 15.2 子步 D1）。
//
// 从 `cli_table.cpp` 另立 TU 的理由是**文件**代码行护栏（该表现 481 代码行、上限 500，
// 一条带完整判据语义的条目装不下），缝按**仪器**选（同 `judgments_table_shadow.cpp` 的切法）。
// 规矩与主表逐字相同：默认值只准写在这里；flags 顺序 = 代码读取顺序 = `imgqa args` 回显顺序
// = -h 打印顺序；summary/notes 是判据契约原文，help 只从既有权威散文里摘。
// ⚠ 拼接顺序只在 `cli_table.cpp` 的 allSpecs()。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> diffStructSpecs()
{
    return {
        /* diffstruct */
        SubcommandSpec{
            "diffstruct",
            "**结构差分统计**（湿面/SSR 反射结构对比度判据，15.2 子步 D1 把一次性脚本升成常驻算子）："
            "A = 实验档、B = 基线档，Δ = lum(A) − lum(B) 在**线性亮度域**（sRGB 解码后 Rec.709）上出"
            "① 差分像素数与占比 ② **Δ<0（变暗）计数** = 「命中处减 IBL specular」生效的阳性锚点 "
            "③ |Δ| 的 p10/p50/p90 与 **p90/p10**（结构对比度，只在 |Δ|>min-delta 的差分集合上算）。"
            "**场景无关**：输入 = 磁盘两张 PNG，不启引擎、不读 manifest（bistro 没有 manifest），"
            "两档怎么来的不归本算子管——它只回答「这两张图的差长什么结构」",
            {
                FlagSpec{"--a", "", "实验档 PNG（Δ = lum(A) − lum(B)，方向别反：反了 ⟹ 变暗计数读成变亮）",
                    FlagKind::String},
                FlagSpec{"--b", "", "基线档 PNG（须与 --a 同 extent，否则像素对不上，直接抛）",
                    FlagKind::String},
                FlagSpec{"--gbuf", "",
                    "dump_gbuf=3 的 metalrough 出图（.g = UNORM8 roughness）⟹ 追加**影响域交叉**："
                    "变暗 ∧ roughness ≥ gate 计数必须恰 0（减法漏到干面 = 影响域破了）",
                    FlagKind::String},
                FlagSpec{"--gate", "0.40",
                    "干/湿门控（roughness 阈值，g/255 口径）。0.40 = 交付资产三档光滑材质 "
                    "0.05/0.35 与「不算湿」档之间的空档；过门控分母按 0 < rough < gate 数（g=0 天空除外）",
                    FlagKind::Number},
                FlagSpec{"--expect-dark", nullptr,
                    "把「Δ<0 计数 > 0」升成**硬判据**（SSR 开/关档对用；参数臂对不给它 = 纯报告，"
                    "因为参数扰动本来可以只变亮）",
                    FlagKind::Bool},
                FlagSpec{"--min-delta", "1e-6",
                    "差分集合门槛 |Δ| > 此值。1e-6 与「PNG 字节有任何不同」**逐位等量**：最小可量化"
                    "亮度台阶 = 0.0722·srgbDecode(1/255) ≈ 2.2e-5 ≫ 1e-6（selftest 钉这条台阶闭式）",
                    FlagKind::Number},
            },
            {
                "口径对账与全部理由 → `diff_struct_metric.h` 头注释；两条硬判据的容差理由与 guard "
                "出处 → `imgqa check`（`diffstruct-dark-anchor` / `diffstruct-dark-domain`）。",
                "p10 = 0 时比值报 **inf 并标注**，不写 0（比值无定义 ≠ 比值是 0，同 "
                "`shadowPenumbraWidth` 的 NaN 纪律：写 0 = 把量具取不出数读成满分）。",
                "④ **命中率 = |Δ|>ε ∧ 过门控 / 过门控**（还 §3.2 未还账⑤，报告值非判据）：只在 "
                "--gbuf 在场时报；缺席报「需 --gbuf」、过门控分母为 0 报「无定义」——**两者都不是 "
                "0**（缺输入/无定义被读成「零命中」= 把量具状态当读数）。分子分母口径与 ③ 同 "
                "（分母 = 全图 0<rough<gate，分子 = 其中 |Δ|>ε 者）。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
