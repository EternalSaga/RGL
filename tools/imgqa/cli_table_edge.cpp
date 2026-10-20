// imgqa CLI 注册表 · **shadow-edge-ruler 段**（影边世界位置绝对尺：真值 = 闭式算术）。
//
// 从 `cli_table.cpp` 另立 TU 的理由与 `cli_table_survival.cpp` 逐字相同：主表撞「单文件
// < 500 代码行」而 `specsSwimAndProbe()` 撞「单函数 < 120 代码行」，缝按**仪器**切。规矩
// 与主表一致：默认值只准写在这里（本条**无 flag 是设计**）；拼接顺序只在 `cli_table.cpp`
// 的 allSpecs()（本段在 shadow-caster-survival 之后，理由写在那里的注释）。
// help 只从既有权威散文（`shadow_edge_ruler.cpp` / `shadow_edge_metric.h` 头注释、
// `judgments_table_edge.cpp`、qa_scenes.py 靶注）里摘，**不许新写事实**。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> shadowEdgeRulerSpecs()
{
    return {
        /* shadow-edge-ruler */
        SubcommandSpec{
            "shadow-edge-ruler",
            "影边世界位置绝对尺：真值 = 闭式算术（竖直板远侧影边 = 板远侧面 + |SHADOW_OFF|"
            "·H），不是参照渲染（10 臂五闸门，头条数 = 级间缝宽毫米，靶 = shadow_edge_rig，"
            "跑在交付配置上）",
            {
            },
            {
                "补的是两台既有阴影判据的同一个根因：它们都是**拿一次渲染比另一次渲染**"
                "（级联比级联 / D=5 比 D=-1），结构上看不见共模错误。实测两次翻车——"
                "`shadow-cascade-seam` 对「四级全走 LOD」给满分 0.000（四级一起变差时级间"
                "没有分歧）；`shadow-caster-survival` 的本影面积存活率被「粗簇轮廓略胖」读"
                "到 1.0022（比参照还\"好\"）。本命令的真值是算术：靶 `shadow_edge_rig`"
                "（tools/qa_scenes.py 生成）上，高度 H 的竖直挡板其远侧影边落在世界 u = 板"
                "远侧面 + |SHADOW_OFF|·H（SHADOW_OFF 由 configs/scene.lua 的 light.direction "
                "闭式导出，= (0.6667, −0.6)）。",
                "读数 = cov 沿世界扫描线的**自身过渡中点**（相对阈值：最大梯度处定过渡区，"
                "两侧平台中位数得 lit/umbra，阈值 = 两者中点，邻域内线性插值取过零；cov 与 "
                "shadow-grain / shadow-cascade-seam / shadow-caster-survival 同式同序）。"
                "🔴 原「50% 绝对」口径是主 agent 的规格错误（把 bistro 墙上的深影 cov>0.5 "
                "外推到地面——本靶地面影深封顶 cov≈0.33，绝对 0.5 结构上不可达，首跑 112 个 "
                "(臂,板) 组合全「不可量」）；过渡中点与影的深浅无关，浅影（0→0.30）与深影"
                "同样精确（selftest 浅/深/近受光端三条立命断言）。过渡高 < 下界（10 个 "
                "8bit-LSB ≈ 0.15）抛，不许返回噪声定位的「边」。斜坡**宽度**不进读数"
                "（「变宽中点不动」专门钉它量的是位置不是陡峭度）。",
                "10 臂 = `none`（覆盖度分母）+ 自然渲染 × 出厂 D=5 × 1 + 钉级 force_cascade "
                "∈ {0,1,2,3} × D ∈ {5, −1}。🔴 原六档 D 扫描已删（主 agent 规格错误 E2：本靶"
                "上 D 逐位空操作——六臂 PNG md5 全等，凸平板的簇 DAG 只删面内部三角、轮廓恒"
                "存活；继续扫 = 演戏），**LOD 的画质/成本课题不在本靶**，它属于下一台「细附"
                "属结构」靶（贴在更大主体上的鳍/栏杆/花箱；现网最接近 = shadow-caster-"
                "survival，但它没有闭式位置刻度）——别在这个靶上找 LOD 信号。D=−1 钉级列留着"
                "是给闸门 ④ 供「LOD 关 = 全量几何」的闭式参照态。",
                "🔴 **五道闸门**（全部抛 rc=3、一条 Verdict 都不产；表先出、闸门后抛）："
                "① 簇路径必须生效（复用 `cluster_path_gate`）+ 靶三件套（.glb/.manifest/"
                ".meshlets.bin）在场；② 分母臂在场 + 覆盖度非零（复用 seam 两道，下界按本"
                "靶重推 = 0.002%（umbra 口径，浅地影），理由住 shadow_edge_ruler.h）；"
                "③ **每块板必须真的量到过，除非闭式预判它量不到**——「某板在某臂找不到过渡\n"
                "中点」且闭式预判该格可量（视深在该级切片区内、离近平面 ≥1 枚本级纹素）⟹ 抛\n"
                "并一次报全（意外失踪，带算子抛因）；片区外 = 出块旁路（shadow.slang ④ 返回\n"
                "全亮）、或影缘落在级近平面 1 纹素精度带内（光空间深度精度最差，实测 t003 影\n"
                "缘距级 3 近平面 11mm → 50mm 宽伪影平台）= **预期**，表里标 ⊘ 并打印清单；"
                "④ **参照态自洽**：全量几何进阴影图的全部臂（force=0×D∈{5,−1} + force∈{1,"
                "2,3}×D=−1）误差 ≤ 4·该级自身纹素 + 2·mm/px（闭式：级 0 恒画全量几何、LOD "
                "关 = 全量进阴影图；界是该级自身量化预算，**不是恰 0**）——超界说明**靶或"
                "换算错了**，不是阴影错了，整条曲线作废；⑤ **级间分歧阳性对照**（精化版，"
                "裁决权在主 agent）：规格原文「每板四臂读数不全等」写于「每板四臂都可量」的"
                "假设上，实测推翻——出块结构使每板只有 2–3 枚级可量，且 t100 的 c1/c2 影边"
                "位置真重合（过渡括号样本逐位相同、cov 场只差在半影尾巴）⟹ seam=0 是测量值。"
                "失明判据改为它本来该测的两条抛：(a) 同 D 不同钉级臂两两图不得逐位相同（臂"
                "混装）；(b) 缝宽不得在所有板所有交界上恒 0（算子对级不敏感）。逐板全等降级"
                "为显式警告（点名 + 归因，不静默）。",
                "判决线三条（全部 INFO 只报数不判；阈值理由与 guard 出处见 `imgqa check`）："
                "J1 `shadow-edge-absolute-error`（主表：每板每级 |误差| 与**本级纹素尺寸**"
                "并排——这张表就是定 K 的推导材料；K 未定不判）· J2 `shadow-edge-seam-width`"
                "（🔴 **本靶头条交付数** = 缝宽毫米：seam(c↔c+1) = |误差(级c) − 误差(级c+1)|"
                "，逐板 + 每交界中位/最大 = 债-139「交界带混合该开多宽」的设计输入；N 未定"
                "不判）· J3 `shadow-edge-cascade-agreement`（seam / max(两纹素) 的无量纲比值；"
                "N 的推导两头住注册表。原「误差随 D 单调」退役——本靶上 D 无影响，前提不存在）。",
                "靶 = 程序化 `models/shadow_edge_rig.glb`（`python3 tools/qa_scenes.py --only "
                "shadow_edge_rig --out build/rgl/models` + meshlet_pack 出簇产物）。零起伏"
                "平地 + 一排竖直挡板（长边 ⊥ 光的水平行进方向；每板独立 primitive）。机位在"
                "**下游**低位朝上游看（影边视线不经过自己的板）。测量常量一律从 manifest 的 "
                "measure 块读，C++ 不重抄几何；级联/texel 由仪器从机位闭式再算一遍并与生成器"
                "预测对账（两个独立实现，漂了即抛）。",
                "🔴 必须走**交付配置**（仓库 `configs/`）+ 第 8 例外门换 entities 装靶：级联"
                "与 LOD 两枚自变量键只存在于交付配置（第 6/7 枚门的既有红线）。配置覆写只吃"
                "第 4/5/6/7/8 枚例外门。**零引擎改动、零 shader 改动**。**无 flag 是设计**"
                "（同 seam / survival / grain）：机位 / 尺寸 / 帧号 / 扫描窗 / 阈值都是量具的"
                "组成部分，能调 = 能调绿。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
