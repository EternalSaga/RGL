// imgqa CLI 注册表 · **diffblock 段**（★0.5-b1，docs/mdi_plan.md §11.2）。
//
// 从 `cli_table.cpp` 另立 TU 的理由与 `cli_table_diffstruct.cpp` 同一条：主表撞「单文件
// < 500 代码行」护栏，缝按**仪器**切。规矩与主表逐字相同：默认值只准写在这里；flags 顺序 =
// 代码读取顺序 = `imgqa args` 回显顺序 = -h 打印顺序；summary/notes 是判据契约原文，help 只从
// 既有权威散文（§11.2 / diff_block_metric.h 头注释）里摘，**不许新写事实**。
// ⚠ 拼接顺序只在 `cli_table.cpp` 的 allSpecs()。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> diffBlockSpecs()
{
    return {
        /* diffblock */
        SubcommandSpec{
            "diffblock",
            "**差异形态判据**（★0.5-b1，docs/mdi_plan.md §11.2）：两图之差里「有没有整块」——"
            "M = {|luma(A)−luma(B)| ≥ τ}（线性亮度），U = area(erode(M, 圆盘半径 r))，判据 "
            "**U == 0** ⟺ 差异处处厚度 ≤ 2r 像素 = 用户口径「UV 缝边缘有差异我能接受，整块不行」"
            "的形式化。**场景无关**：输入 = 磁盘两张 PNG，不启引擎、不读 manifest（同 "
            "`diffstruct`）。退出码 PASS=0 / FAIL=1（要能进 just 脚本当闸门）",
            {
                FlagSpec{"--a", "", "臂 A PNG（与 --b 同 extent，否则像素对不上，直接抛；"
                    "形态判据对 A/B 对称，方向不进判据）", FlagKind::String},
                FlagSpec{"--b", "", "臂 B PNG", FlagKind::String},
                FlagSpec{"--r", "2",
                    "腐蚀半径（像素）。U(r)=0 ⟺ 差异处处厚度 ≤ 2r 像素；默认 2 = 「处处不超过 "
                    "4 像素厚」。⚠ r 是半径不是直径（selftest 锚点 (c) 钉死）", FlagKind::Number},
                FlagSpec{"--tau", "1",
                    "M 的门槛，单位 = **1 个 sRGB 量化步长**（最小可量化亮度台阶 ≈ 2.2e-5 线性"
                    "亮度，与 diffstruct 的 ε 口径同常数）。0 = 任一位不同（严格 >0）",
                    FlagKind::Number},
                FlagSpec{"--top", "8",
                    "连通域清单打印前 K 个（按面积降序；人只看这几个框）。0 = 不打印清单",
                    FlagKind::Number},
                FlagSpec{"--json", "",
                    "机器可读输出路径（★1-a1ii 验收脚本读它；无定义量出 null 不出 0）；空 = 不出",
                    FlagKind::String},
            },
            {
                "必须同时输出三样（阈值是看得见的，不是藏起来的，§11.2）：① 厚度分布 p50/p90/"
                "p99/max（只统计 M 内像素；M 空 = **无定义**不是 0）② U(r) 随 r=0,1,2,4 的曲线"
                "（若 r=4 才归零 = 判据被稀释，要一眼看出来）③ 连通域 area/bbox/最大厚度 按面积"
                "降序前 --top 个。τ=0（任一位不同）的计数与 τ 的计数**同时报**——两数之差 = "
                "字节不同但不足 1 个亮度台阶（交付颗粒噪声 vs 真差异）。",
                "实现取 distanceTransform(M, DIST_L2, 3) 的 U = #{d > r}（一次计算同时给厚度分布"
                "与 U，两者天然同口径）；与字面 erode(MORPH_ELLIPSE, 2r+1) 的面积等价性由 selftest "
                "钉住（容差理由 → `diff_block_metric.h`）。",
                "**先立边界**（§11.1）：「整块错」的大多数形态不需要本判据——albedo/metalrough/"
                "object_id 三条逐字节精确通道零阈值对拍就抓走了；本判据只管**有损通道**（最终"
                "图像、法线）上的差异形态，通过后再用 G-buffer 边缘图给薄差异归类（只进报告）。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
