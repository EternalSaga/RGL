// imgqa CLI 注册表 · **material-swim 段**（材质场世界不变性，债-152）。
//
// 另立 TU 的理由与 cli_table_edge.cpp 逐字相同：主表撞「单文件 < 500 代码行」，缝按**仪器**
// 切。规矩与主表一致：默认值只准写在这里（本条**无 flag 是设计**）；拼接顺序只在
// `cli_table.cpp` 的 allSpecs()。help 只从既有权威散文（`material_swim.cpp` 头注释、
// `judgments_table_material.cpp`）里摘，**不许新写事实**。
#include "cli.h"

#include <vector>

namespace RGL::imgqa {

std::vector<SubcommandSpec> materialSwimSpecs()
{
    return {
        /* material-swim */
        SubcommandSpec{
            "material-swim",
            "材质场世界不变性：机位沿世界 +X 平移 0.5 m 后，同一个世界点的 G-buffer roughness "
            "必须相同（六臂四判据 + 五道闸门，靶 = ssr_rig_empty 零起伏平地，跑在交付配置上）",
            {
            },
            {
                "🔴 **补的是哪一类失明**（判据失明第四例）：批 5 的 no-harm 判据是「同一机位下"
                "门开 vs 门关比静态图」，而一个会滑的图案**在每一帧单独看都是合理的湿面图案**"
                "——斑块只是换了位置，静态对比只看到很小的差。⟹ 静态单姿态对比对「只在运动中"
                "出现」的缺陷结构性不可见；再怎么换判据算子也没用，必须换**激励**（把相机动起来）。",
                "缺陷本体 = `rgl/shaders/deferred_geometry.frag.slang:156` 的 `float2 q = "
                "P.xz * freq;`：水洼掩码 = 低频值噪声（世界 xz），而 P 在 rebase 之后是**相对"
                "渲染原点**的坐标，原点每帧 snap 到级联纹素粒度并跟着相机走 ⟹ 噪声场整片随相机"
                "平移。同一个 q 还喂 :168 的水膜法线波纹（一处病灶两个症状）。",
                "激励 = 相机**平移**（两枚机位同姿态、沿 +X 差 0.5 m）。🔴 旋转不暴露它：原地"
                "转向时相机位置不动 ⟹ 渲染原点不动 ⟹ 图案不滑。",
                "对拍口径 = **地面平面上的世界空间对拍**（债-139 定死的形状）：机位是本工具自己"
                "设的 ⟹ 像素→地面 y=0 的世界 xz 是闭式的，不需要引擎回显任何东西，也**不用** "
                "G-buffer 的 position 通道（门开态它是相对原点的，用它反而要把原点减回来 = 多一"
                "个自由度多一个错处）。比较对象 = metalrough 通道（gbuffer_contract 第 3 通道，"
                "G = roughness），因为水洼直接调制粗糙度。逐像素→世界格（5 cm，每格 ≥4 样本取"
                "中位数）→ 二值（湿/干，阈值 = 全臂字节极差中点，所有臂共用一个）。",
                "口径本体（重投影 / 格坐标 / 共同格计数 / ±20 格平移搜索）住 "
                "`ground_field_metric.{h,cpp}`，与 `swim`（影场世界不变性）**同一份**——两台仪器"
                "共用对拍口径，各自只管「这个逐像素二值量怎么来」。不抄第二份 = AGENTS 哲学第 1 条。",
                "靶 = `models/ssr_rig_empty.glb`（零起伏平地 y=0，无柱变体 ⟹ 画面里只有地面，"
                "「哪些像素是地面」由几何闭式决定，不需要 objectID 反解）。🔴 不拿 bistro 鹅卵石"
                "地当平地——债-139 就是卡在那个测量地板上。地面材质 roughness=0.12 ≤ "
                "ssr.rough_full=0.15 ⟹ 湿面身份恰为 1 ⟹ 水洼调制满生效（字节 13 水洼 / 89 湿石）。",
                "六臂（一次引擎进程一臂，单实例串行）：offA/offB = 门关两机位（**地板**）· "
                "onA/onB = 门开两机位（**主判据**）· onA2 = 门开同机位重跑（**阴性对照**）· "
                "offS = 门关、靶沿 +X 挪 0.5 m 且相机跟着挪（**注入已知世界偏移**）。配置覆写只吃"
                "第 5/8/9 枚例外门（交付配置 + 换 entities 装靶 + coordinate.rebase）。",
                "四条判决线（挂号 → judgments_table_material.cpp）：J1 `material-swim-world-"
                "invariant`（门开两机位翻转 ≤ 0.5% 且最优平移恰为零位移；🔴 **修前红**，实测 "
                "10.91% 且滑移恰等于相机平移量）· J2 `material-swim-determinism`（同机位两跑"
                "**恰为 0**）· J3 `material-swim-gate-closed-floor`（门关地板 ≤ 0.5%，实测 0.11%）"
                "· J4 `material-swim-offset-sensitivity`（注入 0.5 m 必须报出 0.5 m ± 1 格；"
                "🔴 **修前也红**——它证明 J1 的红不是量具坏）。",
                "🔴 **五道闸门**（全部抛 rc=3、一条 Verdict 都不产；表先出、闸门后抛）：G1 靶与"
                "地面矩形在场且不退化（缺靶抛，不 SKIP）；G2 引擎回显对账（门开臂必须有 "
                "`coordinate.rebase = true`、门关臂必须缺席）；G3 激励在场（onA 与 onB 不得逐位"
                "相同 = 相机根本没动）；G4 **场非退化**（每臂有效地面格 ≥ 2000、湿格占比落在 "
                "[5,95]%、字节极差 ≥ 60 LSB——恒值场上「翻转率 0」是恒真）；G5 共同覆盖 ≥ 较小"
                "场的 80%（两个几乎不相交的场对拍，分母是彩票）。",
                "**无 flag 是设计**（同 shadow-edge-ruler / shadow-edge-rebase）：靶 / 机位 / "
                "平移量 / 尺寸 / 格边长 / 阈值都是量具的组成部分，能调 = 能调绿。",
            },
            false,
            "",
        },
    };
}

}  // namespace RGL::imgqa
