// imgqa CLI 注册表本体（批次 B）——「子命令 → flag → 默认值 → 说明」这张表**只有这里一份**。
// 为什么必须有它、以及它堵掉的是哪次事故 → cli.cpp 头注释；解析/打印机制也在 cli.cpp。
//
// 改这张表的三条规矩：
//   1. 默认值只准写在这里（main.cpp 的 dispatch 里出现 "96" 这种字面量 = 本批次白做）；
//   2. flags 顺序 = 代码读取顺序 = `imgqa args <cmd>` 回显顺序 = -h 打印顺序（对拍靠它）；
//   3. summary/notes 是判据契约原文，逐字改；help 只准从既有权威散文里摘，不许新写事实。
#include "cli.h"

#include <iterator>
#include <vector>

namespace RGL::imgqa {
namespace {

// 表分四段写，不是审美：整张表 370 代码行塞进一个函数直接撞 tools/ 侧「函数 < 120 代码行」
// 护栏（而且 21 条命令一屏滚不到底，改了哪段都看不出）。**四段的拼接顺序 = -h 打印顺序**，
// 段内顺序 = 该组命令的 flags 对拍顺序，两处都不许乱。

/// 元命令 + 机位几何类（-h 顺序第 1 段：selftest, check, refuse, args, attribution, faces, switch, izb）。
std::vector<SubcommandSpec> specsMetaAndRig()
{
    return {
        /* selftest */
        SubcommandSpec{
            "selftest",
            "判据/转录/解码器自检（无引擎）",
            {
            },
            {
            },
            false,
            "",
        },
        /* check */
        SubcommandSpec{
            "check",
            "列每条判据的容差理由与 guard 出处（一律 advisory，没有晋升这回事）",
            {
            },
            {
            },
            false,
            "",
        },
        /* refuse */
        SubcommandSpec{
            "refuse",
            "列/触发反模式拒绝（ssim 等有理由的禁止）",
            {
            },
            {
            },
            true,
            "[name]",
        },
        /* args */
        SubcommandSpec{
            "args",
            "回显解析后的 (flag, 值)（默认值单一真相源；**不跑任何东西**）",
            {
            },
            {
            },
            true,
            "<子命令> [flags...]",
        },
        /* attribution */
        SubcommandSpec{
            "attribution",
            "机位下谁被画了/占多少/多亮/谁全黑",
            {
                FlagSpec{"--glb", "shadow_rig.glb", nullptr, FlagKind::String},
                FlagSpec{"--camera", "near_anchor", nullptr, FlagKind::String},
            },
            {
            },
            false,
            "",
        },
        /* faces */
        SubcommandSpec{
            "faces",
            "闭式解逐面预测 vs 观测亮度簇",
            {
                FlagSpec{"--glb", "shadow_rig.glb", nullptr, FlagKind::String},
                FlagSpec{"--piece", "anchor_cube_near", nullptr, FlagKind::String},
                FlagSpec{"--shadow", nullptr,
                    "开阴影（**只用于复现观察**：闭式解无阴影项 ⟹ 簇判据拒绝求值）",
                    FlagKind::Bool},
                FlagSpec{"--env", nullptr,
                    "开 env（同上，只用于复现观察；闭式解只含直接光项）", FlagKind::Bool},
            },
            {
            },
            false,
            "",
        },
        /* switch */
        SubcommandSpec{
            "switch",
            "开关影响域 + 死分支（含确定性前置）",
            {
                FlagSpec{"--glb", "shadow_rig.glb", nullptr, FlagKind::String},
                FlagSpec{"--camera", "near_anchor", nullptr, FlagKind::String},
                FlagSpec{"--allow-piece", "ground", nullptr, FlagKind::String},
                FlagSpec{"--env", nullptr, nullptr, FlagKind::Bool},
            },
            {
            },
            false,
            "",
        },
        /* izb */
        SubcommandSpec{
            "izb",
            "IZB 消费域：mask↔画面寻址一致性（三子判据）",
            {
                FlagSpec{"--glb", "izb_rig.glb", nullptr, FlagKind::String},
                FlagSpec{"--camera", "ladder", nullptr, FlagKind::String},
            },
            {
            },
            false,
            "",
        },
    };
}

/// 影场游动 / 闭式解探针 / 逐杆覆盖率 / 时域锚点（-h 顺序第 2 段：swim, probe, aa-coverage, temporal）。
std::vector<SubcommandSpec> specsSwimAndProbe()
{
    return {
        /* swim */
        SubcommandSpec{
            "swim",
            "影场世界不变量：机位平移后重投影比影场（0 位移）",
            {
                FlagSpec{"--glb", "izb_rig.glb", nullptr, FlagKind::String},
                FlagSpec{"--camera", "ladder", nullptr, FlagKind::String},
                FlagSpec{"--dx", "0.06", nullptr, FlagKind::Number},
                FlagSpec{"--izb", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--evssm", nullptr, nullptr, FlagKind::Bool},
            },
            {
            },
            false,
            "",
        },
        /* shadow-softness */
        SubcommandSpec{
            "shadow-softness",
            "软影滤波轴量具：交付配置七跑的影形态 + 太阳角敏感度（批 N6d 后全绿）",
            {
            },
            {
                "场景 = **交付配置 configs/**（只覆写 shadow.lua，`izb` 强制 false）；**不是 shadow_rig**"
                "——后者量不出半影（全部遮挡体 Δz≤12m 而级联 texelWorld 0.29…2.63m ⟹ penumbraTexels 恒<1），"
                "理由全文 = `docs/shadow_routes_plan.md` §4.1 量具边界。",
                "七跑：`none`（参照）· `csm` · `evssm`（出厂 0.53°）· `evssm_sun0` · `evssm_sun5` · "
                "`pcss_sun0` · `pcss_sun5`。pcss 两条只服务 J1：量具自证必须走 PCSS 这条**不经过 EVSM "
                "方差窗**的独立「太阳角→半影」实现，拿 EVSSM 自己当锚点是循环论证（§5.1 的设计自纠）。",
                "帧号写死第 12 帧（`llm_shot.delay_frames`，不走 RPC `/shot`）：交付场景 `spin` 在转，"
                "同配置第 8 帧与第 12 帧影覆盖差 0.12pp ⟹ 帧号是量具的一部分，不是采集细节。",
                "**无 flag 是设计**：场景 / 臂 / 帧号 / 阈值都是量具的组成部分，能调 = 能调绿"
                "（§5.1「阈值一旦写死就不许为了让它绿回头改」）。",
                "当前读数（2026-10-09 N6d 落地后）：J1 PASS · J2 PASS（接线判据，any-diff 口径）· "
                "J3 PASS（两比值两侧有界）· 确定性 PASS ⟹ rc=0。"
                "**J2 的靶子在 N6d 换过一次**（原判据要的是物理上不存在的软度），J3 的 0.9 是"
                "**转正门槛不是判据线**（实测天花板 0.74）—— 两条理由都在 `check` 的容差理由里，"
                "改判据前先读 `docs/shadow_routes_plan.md` §5.2。",
            },
            false,
            "",
        },
        /* shadow-kernel-scale */
        SubcommandSpec{
            "shadow-kernel-scale",
            "CSM 核尺度漂移量具：penumbra_ladder 上量半影宽度的世界米制（三臂 + 三条判决线）",
            {
            },
            {
                "靶 = 程序化 `models/penumbra_ladder.glb`（`tools/qa_scenes.py` 生成）。四块板离地"
                "高度 8/16/32/64m，级联半径随高度增长 ⟹ **滤波核 / 影宽** 之比是否随高度漂移被直接"
                "读成一条曲线。测量常量一律从 `penumbra_ladder.manifest.json` 的 `measure` 块读，"
                "工具不重抄（重抄 = 第二份真相，改靶必漂）。",
                "三臂：`none`（参照，无影）· `csm`（被测：4-tap PCF）· `pcss5`（阳性锚：PCSS "
                "sun_degrees=5）。S = (L_none − L_arm)/L_none，半影宽度 W = 剖面 10%↔90% 交叉的"
                "**世界米**距离（针孔投影反解，不做像素换算）。",
                "三条判决线：J1 CSM 本影存在（S>0.5 占全幅 ≥0.50%）· J2 CSM 的 W(64)/W(16) ∈ "
                "[0.55, 1.80]（不随 cascade 半径漂）· J3 pcss5 的 W(64)/W(16) ≥ 1.60（靶子真的"
                "在展开，J2 的绿不是「什么都量不到」）。阈值理由与 guard 出处见 `imgqa check`。",
                "**无 flag 是设计**（同 `shadow-softness`）：靶 / 臂 / 帧号 / 阈值都是量具的组成"
                "部分，能调 = 能调绿。缺 `.glb` 或缺 manifest 直接 FAIL，**不 SKIP**。",
            },
            false,
            "",
        },
        /* probe */
        SubcommandSpec{
            "probe",
            "闭式解探针：该是多少 vs 实际是多少",
            {
                FlagSpec{"--albedo", "0,0,1", nullptr, FlagKind::String},
                FlagSpec{"--light", "-0.642788,0,0.766044", "**指向光源**的方向 L（不是 scene.lua 的 direction）；与 --intensity 一起构成 lightOverride（给了任一 = 覆写配置里的光照）", FlagKind::String, true},
                FlagSpec{"--config", "qa/configs/analytic", nullptr, FlagKind::String},
                FlagSpec{"--x", "640", nullptr, FlagKind::Number},
                FlagSpec{"--y", "360", nullptr, FlagKind::Number},
                FlagSpec{"--rough", "0.9", nullptr, FlagKind::Number},
                FlagSpec{"--metal", "0", nullptr, FlagKind::Number},
                FlagSpec{"--intensity", "12", "与 --light 一起构成 lightOverride（给了任一 = 覆写配置里的光照）", FlagKind::Number, true},
                FlagSpec{"--tol", "2", nullptr, FlagKind::Number},
            },
            {
            },
            false,
            "",
        },
        /* aa-coverage */
        SubcommandSpec{
            "aa-coverage",
            "逐杆覆盖率对拍闭式：单帧量「黑度积分 vs 投影宽度」（批 12.6 T4；--base-space = 批 13.2 门开态；--kernel = 批 13.8 输入侧重建核 0 双线性 / 1"
            " CR 4×4 / 2 Lanczos3，本判据 = 核选型那条**静止态闭式**判据）",
            {
                FlagSpec{"--warm", "32", nullptr, FlagKind::Number},
                FlagSpec{"--feedback", "0.12", nullptr, FlagKind::Number},
                FlagSpec{"--base-space", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--kernel", "0", "批 13.8 输入侧重建核（0 双线性 / 1 CR 4×4 / 2 Lanczos3）；本判据 = 核选型那条**静止态闭式**判据", FlagKind::Number},
            },
            {
            },
            false,
            "",
        },
        /* temporal */
        SubcommandSpec{
            "temporal",
            "TAA 时域阳性锚点：静止机位 + jitter，连续帧突发逐帧差（批 12.6 T4）",
            {
                FlagSpec{"--pairs", "32", nullptr, FlagKind::Number},
                FlagSpec{"--warm", "32", nullptr, FlagKind::Number},
                FlagSpec{"--base-space", nullptr, "批 13.2 taa.base_space：true = 输出在**基空间**。⚠ 门开态下 S1（只抖不滤）的逐帧差不再由游动主导 ⟹ S1/S2 比值失去原口径", FlagKind::Bool},
            },
            {
            },
            false,
            "",
        },
    };
}

/// 时域绝对残差 / 真实场景游动 / 拖尾 / 相机运动（-h 顺序第 3 段：temporal-residual, taa-swim, temporal-tail, temporal-motion）。
std::vector<SubcommandSpec> specsResidualAndMotion()
{
    return {
        /* temporal-residual */
        SubcommandSpec{
            "temporal-residual",
            "时域**绝对**残差：静止机位连拍折 (2,16,H,W) 后逐像素 max−median（线性域；补 temporal 相对比的盲区，批 13.1 主判据）。 --long-feedback <档>"
            " = 批 13.7 长短窗变体（**只出数不判**：跑 B1–B4 四组出短窗/长窗 meanSwing/p99.9/frac>τ + 梯度比；短窗档恒 = 0.12，长窗档 = 本旗标的，"
            "⚠ 档 = **本帧权重**，调小 = 更信历史）。 --bistro = 批 13.7b 真实场景变体（债-48：交付配置 + 用户报障三机位"
            " doorframe/wetground/doorpanel，N1–N5 五组出数 + 债-47 线性域梯度比 + swing 热图；--camera 选单机位）。批 13.7c 每机位另出**亮度"
            " × 梯度二维分带**（短窗/长窗/差三张表，每格 share·rate·残余质量三数 + 敏感性），判「残余闪烁住在高亮边还是普通边缘」；判决线跑前写死，全文见"
            " temporal_residual.h 同名段。 `--reject` = 批 13.10 每机位另出**历史拒收 × 残余交叉表**（门 `taa.diag_reject` 开、连拍"
            " taa_out alpha；按「32 帧里有几帧历史被邻域盒动过」分档 × swing + **逐档抑制比 ρ**），判「长窗压不满的那部分残余是不是 `clipToAabb`"
            " 夹历史」——0 档（全程收下）= 纯 EMA 泄漏对照，其 ρ 必须 ≈ 闭式 ρ*（**内部阳性锚点**：绿例就在同一帧里）。并出**主判据"
            " `temporal-residual-window-fidelity`**：`ρ ≤ 1.5×ρ*`，ρ* = |H(α_long)|/|H(α_short)| 由配置的 α"
            " 运行时现算（改档位自动自适应；|H| 跨谐波只散布 5% ⟹ 与内容无关）。**FAIL 即 rc=1**（与两条守卫同级：本命令的结论不许被读成通过）。判决线跑前写死，见同名头注释段",
            {
                FlagSpec{"--periods", "2", nullptr, FlagKind::Number},
                FlagSpec{"--warm-multiplier", "6", nullptr, FlagKind::Number},
                FlagSpec{"--slow-feedback", "0.002", nullptr, FlagKind::Number},
                FlagSpec{"--skip-slow", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--base-space", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--long-feedback", "0.01", "批 13.7 长短窗变体的**长窗档** taa.feedback（**只出数不判**，短窗档恒 = 0.12）；合成靶侧：显式给出本旗 = 进 B1–B4 变体；⚠ 档 = **本帧权重**，调小 = 更信历史", FlagKind::Number, true},
                FlagSpec{"--bistro", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--camera", "all", nullptr, FlagKind::String},
                FlagSpec{"--diag-bs-off", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--reject", nullptr, "批 13.10 每机位另出**历史拒收 × 残余交叉表**（门 taa.diag_reject 开）", FlagKind::Bool},
                FlagSpec{"--clip-abl", nullptr, "批 13.11 **历史裁剪因果 A/B**（门 taa.clip_mode）：逐臂跑「门关/门开」两组，出每臂 ρ 并对闭式线判", FlagKind::Bool},
                FlagSpec{"--clip", "0", "批 13.11 taa.clip_mode 单臂透传；0 = 门关 = 不写键 = 交付配置原样", FlagKind::Number},
                FlagSpec{"--clip-soft", "1", "臂 4 的软裁剪 λ（历史每帧最多搬动 λ 份「到盒边界的距离」= 弹簧不是墙）/ 臂 5 的扩 K×", FlagKind::Number},
            },
            {
                "`--clip-abl` = 批 13.11 **历史裁剪因果 A/B**（门 `taa.clip_mode`）：逐臂跑「门关/门开」两组，出每臂 ρ 并对闭式线判。臂集 = 0 现行 / 1"
                " 关盒（短路天花板参照）/ 2 绕 centroid 扩 1.5×（债-60 重测臂）/ 3 box9（空间加宽）/ 4 软裁剪 λ（修法臂：历史每帧最多搬动 λ 份「到盒边界的距离」="
                " 弹簧不是墙）/ 5 绕 centroid 扩 K×（K = `--clip-soft`，量「盒差几倍」）。**每臂带锐度对照**（梯度比 vs 同次 fb=1 参考；出 [0.85,1.02] 带"
                " = 靠糊换安静 = 不算修好）。三条线跑前写死：① **坐实** = ρ ≤ 1.5ρ* 且 frac>τ 相对臂 0 降 ≥4×；② **否证** = 所有臂 ρ ≥ 0.8×ρ(臂0) ⟹"
                " 盒不是主因；③ **通用性** = 逐机位 / 逐靶方向一致（只有报障机位降 = 场景特异 = 不算修好）",
                "`--clip <mode> [--clip-soft λ]` = 单臂透传（合成靶侧配 `--long-feedback <档>` 用，B2/B3/B4 吃同一臂，ρ 同口径可比）；0 = 门关 ="
                " 不写键 = 交付配置原样",
            },
            false,
            "",
        },
        /* taa-swim */
        SubcommandSpec{
            "taa-swim",
            "真实场景逐帧差是否=整幅亚像素平移（游动）；含基空间门（批 13.2 T5/T6）与量具阳性锚点（批 13.1b）",
            {
                FlagSpec{"--pairs", "32", nullptr, FlagKind::Number},
                FlagSpec{"--warm", "32", nullptr, FlagKind::Number},
            },
            {
            },
            false,
            "",
        },
    };
}

/// 尾长与运动族。批 13.15 从 `specsResidualAndMotion` 拆出——加进 `--motion step` 的四条判决线
/// 说明后原函数撞「单函数 < 120 代码行」，而拆缝正好是语义缝：上面那条是**静止机位残差 + 游动**，
/// 这两条是**运动下的鬼影**。
std::vector<SubcommandSpec> specsTailAndMotion()
{
    return {
        /* temporal-tail */
        SubcommandSpec{
            "temporal-tail",
            "**鬼影尾长直测**（债-63 的出口）。`--motion spin`（默认）= 静止机位 + 场景 spin，主指标 = 归一化鬼影份额 `frac = mean|out−ref| ÷"
            " mean|N−S|`（关盒时闭式 = 1−α）。`--motion camera`（批 13.14a）= **机位匀速 yaw**，主指标退化为**稳态份额** `frac = Δ加权残差 ÷"
            " mean Δ`，`Δ = |ref_j − ref_{j−1}∘φ|`，φ = **解析** yaw 映射（带 pitch）——spin 下相机不动 ⟹ 臂 11 的 `gate` 恒 1.0 ⟹"
            " **spin 版原理上进不了 gate 斜坡区**，而批 13.14 的赌注正压在那个门上。两模式共用一条设计红线：参考 = 同一段运动的「TAA 关 +"
            " jitter_scale=0」渲染，配对 = **引擎帧计数器之差**（整数常量，**零图像配准**——`temporal-motion` 那两条常红的对齐护栏在这里根本不存在）。"
            "判决线跑前写死，**红一条即不许报臂间结论**。τ 是唯一自由阈值 ⟹ 输出自带三档敏感性表",
            {
                FlagSpec{"--camera", "brickfence", nullptr, FlagKind::String},
                FlagSpec{"--motion", "spin", "spin（默认）= 静止机位 + 场景 spin（既有六机位读数的口径）；camera = 相机帧驱动 yaw（批 13.14a，机位运动稳态份额）；step = **平移阶跃残影衰减**（批 13.15，唯一能去遮挡的激励；`frame_yaw` 是原地纯旋转 ⟹ 无视差 ⟹ 无鬼影）", FlagKind::String},
                FlagSpec{"--step", "0.02", "仅 --motion step：A→B 平移距离（世界单位，沿相机 right 轴纯横移）。⚠ 上限受引擎跳切判据 `cameraJumps` 管（位移 > 0.25·sceneRadius ⟹ 历史作废 = 假绿），由判决线 `temporal-tail-step-stimulus` 当场抓", FlagKind::Number},
                FlagSpec{"--angle", "0.3", nullptr, FlagKind::Number},
                FlagSpec{"--gate-angle", "0", "仅 --motion camera：gate 断点的高档角度（度/帧），用来把每帧像素位移扫过 `gate = saturate(2 − motion)` 的推导断点 2px；0 = 跳过判决线 4", FlagKind::Number},
                FlagSpec{"--burst", "96", nullptr, FlagKind::Number},
                FlagSpec{"--warm", "64", nullptr, FlagKind::Number},
                FlagSpec{"--max-lag", "16", "仅 --motion spin：衰减曲线长度 K", FlagKind::Number},
                FlagSpec{"--tau", "0.05", nullptr, FlagKind::Number},
                FlagSpec{"--margin", "8", nullptr, FlagKind::Number},
                FlagSpec{"--fsr", nullptr,
                    "仅 --motion step：追加**升频 / FSR 六臂**（B75 双线性 / S75 FSR 纯空间 / T11 TAA@原生"
                    "现行盒 / **T13 TAA@原生臂13** / T75 TAA臂13@0.75 / F75 FSR时域+抖动@0.75），与六条 "
                    "TAA 锚臂跑在**同一次阶跃、同一把尺**上。🔴 对手是 **T13** 不是 T11——债-69 的修法臂 13 "
                    "鬼影好 79×、只差转交付，赢 T11 = 赢一个下个月就换掉的配置。两条量具守卫："
                    "`temporal-tail-step-fsr-floor`（无历史臂 ghost ≡ 0）与 `temporal-tail-step-fsr-basespace`"
                    "（F75 收敛态是固定点；末 16 帧是 **TAA 的**抖动周期，FSR 的实测是 14）。"
                    "⚠ 只出**鬼影单轴**，不许据此下「谁更好」——闪烁轴在 temporal-flicker",
                    FlagKind::Bool},
            },
            {
                "`--motion camera` 的四条判决线（跑前写死）：① **配对零锚** = 主角度的 `Z`（α=1 + 无抖动）frac ≤ 0.02；② **灵敏度 + 单位** = 主指标"
                "单位是**「相当于几帧过期历史」**（刻度由参考自身算出），线 = 关盒短窗 ≥ 0.8 帧（推导下界 1−α）且关盒长窗 > 短窗；③ **静止零锚** = δ=0 的"
                " **Z 组**（jitter 0 + α=1）**借主角度的 φ** 量，frac ≤ 0.02（⚠ 两处坑：不能用 δ=0 的 φ，那会让 Δ ≡ 0 ⟹ 0/0 兜底成 0 = 空断言；也不能拿 "
                "jitter=1 的臂当零点，它的相位平均收敛值比无抖动参考真的更糊，那是**地板** ≈0.13，不是零点）；④ **gate 断点** = `臂11 − 臂0` 低角度侧显著为正"
                "（洞在场）、高角度侧 ≈ 0（gate 关门后臂 11 退回臂 0）。断点位置是**推出来的**不是扫出来的 ⟹ 阳性锚点不是拟合",
                "`--motion step` 的五条（批 13.15 建仪器 / 批 13.15b 加修法臂；债-63 真鬼影量具；每臂三跑 = A 收敛 / B∞ 收敛 / "
                "A→B 瞬切，`jitter_scale` 恒 0）："
                "**主指标 = AUC = Σ ghost(n)**，单位「帧·全对比度」= 屏幕上陈旧内容的总量（🔴 不是 n₅₀：盒生效时 "
                "ghost(0) 第一帧就掉到 0.015 量级 ⟹ 曲线永远碰不到 0.5 ⟹ n₅₀ 对带盒的臂**不存在**）。六臂 = Z（α=1 零锚）/ "
                "E（**闭式锚**：关盒 + 重投影关 ⟹ `ghost(n) = (1−α)ⁿ` 精确）/ R（关盒 + 重投影开 = 重投影单独贡献）/ C0（现行 5 "
                "抽头盒）/ C11（交付）/ C13（批 13.15b 修法臂：α 也吃 gate）。判决线：① **零锚** = Z 的 max ghost(n) ≤ 0.02；"
                "② **历史活过了这一刀** = E 的 ghost(0) ≥ 0.9（预测 1−α = 0.99 尖锐；掉到 0 = 引擎 `cameraJumps` 把这一刀当"
                "跳切切了 = 满分假绿）；③ **闭式锚 = 仪器自证** = E 的 AUC 落在 `(1−(1−α)^N)/α` ±10%；④ **主判据** = "
                "AUC(C11)/AUC(C0) ≤ 0.7 才算记忆门在去遮挡处多兑掉三成（> 1 = 交付臂**更鬼**）；⑤ **修法验收** = 臂 13 的 "
                "AUC ≤ 0.7 × C0 **且** 它与臂 11 在两个静止收敛态的最大偏离 ≤ 0.02（理论 0 精确）。🔴 ⑤ 的第二条是全部价值："
                "只判「鬼影变小」会奖励「把 α 恒置 1」= 关掉 TAA 这个退化解",
            },
            false,
            "",
        },
        /* temporal-motion */
        SubcommandSpec{
            "temporal-motion",
            "**相机运动下 TAA 抗了什么锯齿**：--motion camera（默认）= 相机帧驱动 yaw（camera.lua frame_yaw，="
            " 用户报障的运动源；angle=角度×帧号，纯函数）；--motion spin = 场景旋转相机不动（债-49 鬼影仪器）。M0–M4 对**同机位 2560×1440"
            " 超采样参考**量边缘像素线性域偏差（按帧号配对到同一视点，故不受 `moving-camera-l1-ratio` 反模式影响）+ M1m/M4m 连拍 taa_out alpha"
            " 出**历史被拒比例**（门 `taa.diag_reject`，RGB 逐位 = M1/M4）；只出数 + 一句判决，不改渲染",
            {
                FlagSpec{"--camera", "doorframe", nullptr, FlagKind::String},
                FlagSpec{"--motion", "camera", "camera（默认）= 相机帧驱动 yaw（camera.lua frame_yaw，= 用户报障的运动源；angle=角度×帧号，纯函数）；spin = 场景旋转相机不动（债-49 鬼影仪器）", FlagKind::String},
                FlagSpec{"--angle", "0.2", nullptr, FlagKind::Number},
                FlagSpec{"--burst", "32", nullptr, FlagKind::Number},
                FlagSpec{"--warm", "16", nullptr, FlagKind::Number},
                FlagSpec{"--warm-still", "64", nullptr, FlagKind::Number},
                FlagSpec{"--edge-pct", "0.90", nullptr, FlagKind::Number},
                FlagSpec{"--long-feedback", "0", "批 13.11 鬼影 × 裁剪消融的**长窗档覆写**：>0 = 把 TAA 开组的 taa.feedback 覆写为本值（α 恒 = 本值）；默认 0 = 不覆写不写键 = 已发表 M0–M4 读数逐位不变", FlagKind::Number},
                FlagSpec{"--clip", "0", nullptr, FlagKind::Number},
                FlagSpec{"--clip-soft", "1", nullptr, FlagKind::Number},
            },
            {
                "`--long-feedback <档> --clip <mode> [--clip-soft x]` = 批 13.11 **鬼影 × 裁剪消融**：spin 下相机静止 ⟹ 长窗全程生效"
                " ⟹ 进入 债-49 风险区（静止机位 + 场景内运动 + 超长累积窗）。跨臂比 dev(M1)/dev(M0) 即「放宽盒要付多少鬼影」。**阳性锚点 = 关盒（臂 1）必须显著变差**；不变差 ="
                " 邻域盒在鬼影上根本没承重，就不必为它付静止闪烁的代价。默认两键都不写 = 已发表 M0–M4 读数逐位不变",
            },
            false,
            "",
        },
    };
}

/// 反馈与闪烁 / 单应变换 / AO 颗粒 / 事后真相（-h 顺序第 4 段：temporal-feedback, temporal-flicker, temporal-homography, ao-noise, note）。
std::vector<SubcommandSpec> specsFlickerAndNoise()
{
    return {
        /* temporal-feedback */
        SubcommandSpec{
            "temporal-feedback",
            "**运动态 feedback 扫描**：把「历史在非零分数偏移上被 CR 重采样 ⟹ 每帧一次低通、在 ≈1/α 窗内复利」这条假设变成数。fb=1 那格（零累积但仍带"
            " jitter）拆开「累积」与「抖动」两个贡献；顺带在静止态扫 fb 出 swing，判「窗棂残余与运动失效是否同一个根」。每组回显**引擎自己报的**生效 feedback（`GET"
            " /state`）+ 同进程 /param A/B 字节差 = 参数生效自证；只出数，不改引擎默认值。S0 = 静止 TAA 关地板（批 13.8 第 0 步：静止态"
            " dev(fb=1)/dev(S0) 与运动态 R(fb=1) 同构，两边同 ~4% ⟹ 那 4% 与运动无关）；--kernel = 批 13.8 输入侧重建核",
            {
                FlagSpec{"--fb", "0.02,0.05,0.12,0.25,0.5,0.8,1.0", nullptr, FlagKind::String},
                FlagSpec{"--camera", "doorframe", nullptr, FlagKind::String},
                FlagSpec{"--angle", "0.2", nullptr, FlagKind::Number},
                FlagSpec{"--burst", "32", nullptr, FlagKind::Number},
                FlagSpec{"--edge-pct", "0.90", nullptr, FlagKind::Number},
                FlagSpec{"--kernel", "0", "批 13.8 输入侧重建核（0 双线性 / 1 CR 4×4 / 2 Lanczos3）", FlagKind::Number},
            },
            {
            },
            false,
            "",
        },
        /* temporal-flicker */
        SubcommandSpec{
            "temporal-flicker",
            "**相机移动下的时间闪烁（闭式解判据）**：相机帧驱动 yaw 以亚像素扫速扫过 1px 高对比边缘，量「每帧最大变化」（p99.5，线性域），与**闭式解**两端对拍——理想"
            " AA（覆盖线性斜坡）给下界、完全无 AA（阶梯整段跳变）给上界，于是 G=实测/理想与 ζ=(实测−理想)/(无AA−理想) 有绝对刻度，不靠「动得慢」刷分。--target edge ="
            " aa_target 白板左边缘（闭式适用，主判据）；--target <bistro 机位名> = 交付配置口径（闭式只作参考刻度）。TAA 开/关各一组，出"
            " flicker(on)/flicker(off)。FFT 只作诊断打印——高频带能量比已被闭式对照否决（混叠把锯齿变成方波），理由见 temporal_flicker.h。--kernel = 批"
            " 13.8 输入侧重建核（0 双线性 / 1 CR 4×4 / 2 Lanczos3），**只作用于 TAA 开组**⟹ TAA 关组 = 闭式无 AA 锚点，跨核重测它必须不动（ζ 的主判据）",
            {
                FlagSpec{"--target", "edge", "edge = aa_target 白板左边缘（闭式适用，主判据）；<bistro 机位名> = 交付配置口径（闭式只作参考刻度）", FlagKind::String},
                FlagSpec{"--sweep", "0.125", nullptr, FlagKind::Number},
                FlagSpec{"--frames", "32", nullptr, FlagKind::Number},
                FlagSpec{"--halfwidth", "8",
                    "ROI 宽度（px）。⚠ 两分支口径不同（债-98，拍板不统一）：--target edge = 半宽"
                    "（ROI 宽 2×）；--target <bistro 机位> = **全宽**（ROI = 自身×自身）",
                    FlagKind::Number},
                FlagSpec{"--rowfrac", "0.14", nullptr, FlagKind::Number},
                FlagSpec{"--verbose", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--kernel", "0", "批 13.8 输入侧重建核（0 双线性 / 1 CR 4×4 / 2 Lanczos3），**只作用于 TAA 开组** ⟹ TAA 关组 = 闭式无 AA 锚点", FlagKind::Number},
                FlagSpec{"--fsr", nullptr,
                    "追加三条升频臂跑**闪烁轴**（双线性@0.75 = 糊地板 / TAA开@0.75 = 同分辨率机制对照 / "
                    "FSR时域+抖动@0.75 = 候选），与既有两条 TAA 臂共用同一 ROI 与同一激励。"
                    "🔴 FSR 要算「买了东西」必须**同时**不劣于糊地板且优于 TAA 开——只赢后者说明它买到的"
                    "只是分辨率损失的模糊（两轴规则）。鬼影轴在 `temporal-tail --motion step --fsr`，"
                    "**两轴合起来才许下结论**",
                    FlagKind::Bool},
            },
            {
            },
            false,
            "",
        },
        /* temporal-homography */
        SubcommandSpec{
            "temporal-homography",
            "**精确单应 warp 的逐像素闪烁归因图**：相机做原地绕 up 轴 yaw ⟹ 像面映射是纯旋转单应（与深度无关、对整幅画面精确），由"
            " tan(Δψ)+焦距+主点**闭式**给出——零参考图、零对齐搜索、零 velocity buffer。把上一帧按它搬到本帧网格，剩下的差就是「搬不动的部分」（= 采样格移动的锯齿差 / TAA"
            " 时域滤波做的事）。同批出**往返地板** |I−H⁻¹(H·I)| 判「是量具在响还是真闪烁」，出**解释率** 1−残差/原始帧差，出固定标尺热图 PNG（TAA 开/关两张可直接对眼）。yaw"
            " 符号用 t 与 −t 两个闭式假设对拍取小者，两个残差都回显",
            {
                FlagSpec{"--target", "edge", nullptr, FlagKind::String},
                FlagSpec{"--sweep", "0.125", nullptr, FlagKind::Number},
                FlagSpec{"--frames", "16", nullptr, FlagKind::Number},
                FlagSpec{"--out", "", nullptr, FlagKind::String},
                FlagSpec{"--bands", nullptr, nullptr, FlagKind::Bool},
                FlagSpec{"--env", "-1", "A/B 侧覆写 env.intensity；**负数 = 不覆写 = 交付配置原样**（默认）", FlagKind::Number},
                FlagSpec{"--ao", "-1", "A/B 侧覆写 ao.strength；**负数 = 不覆写 = 交付配置原样**（默认）", FlagKind::Number},
            },
            {
            },
            false,
            "",
        },
        /* ao-noise */
        SubcommandSpec{
            "ao-noise",
            "SSAO 噪声四组量测：闭式契约区偏离 / 分布 /亚像素爬行 / TAA 时域（同进程 /param A/B）",
            {
                FlagSpec{"--glb", "cornell_box.glb", nullptr, FlagKind::String},
                FlagSpec{"--camera", "front", nullptr, FlagKind::String},
                FlagSpec{"--strength", "0.5", nullptr, FlagKind::Number},
                FlagSpec{"--radius", "40", nullptr, FlagKind::Number},
                FlagSpec{"--pairs", "16", nullptr, FlagKind::Number},
                FlagSpec{"--no-temporal", nullptr, "关掉 TAA 时域组（默认跑）", FlagKind::Bool},
                FlagSpec{"--bistro", nullptr, nullptr, FlagKind::Bool},
            },
            {
            },
            false,
            "",
        },
        /* note */
        SubcommandSpec{
            "note",
            "回填最后一条调用记录的事后真相（判据可信度的台账）",
            {
            },
            {
            },
            true,
            "\"事后真相\"",
        },
    };
}

/// 按顺序串起各段（顺序即 -h 顺序）。段数不在本文抄——`append` 行本身就是清单。
std::vector<SubcommandSpec> allSpecs()
{
    std::vector<SubcommandSpec> table;
    auto append = [&table](std::vector<SubcommandSpec> group) {
        table.insert(table.end(), std::make_move_iterator(group.begin()),
            std::make_move_iterator(group.end()));
    };
    append(specsMetaAndRig());
    append(specsSwimAndProbe());
    // `blend-sort` 排在 shadow/probe 那一段之后（施工单要求它紧跟 `shadow-kernel-scale`；
    // 主表撞护栏后正文搬进 `cli_table_blend.cpp`，这里就是它在 -h 里的位置）。
    append(blendSortSpecs());
    // `blend-lit` 排在 blend-sort 之后（正文住 `cli_table_blend_lit.cpp`）。同上一条 append
    // 注释的理由：不插进既有条目之间，那会让那些注释里的位置声明失效；-h 顺序只是打印序。
    // 两台透明仪器相邻倒是顺带的——它们共享「双臂同源」这条立命处。
    append(blendLitSpecs());
    // `shadow-grain` 排在 blend-sort 之后（正文住 `cli_table_grain.cpp`）。刻意**不**插进
    // shadow-kernel-scale 与 blend-sort 之间——那会让上一条注释的位置声明失效；-h 顺序
    // 只是打印序，同族相邻的好处在下一条 append 注释里已让位于既有声明的字面真。
    append(shadowGrainSpecs());
    // `shadow-cascade-seam` 排在 shadow-grain 之后（正文住 `cli_table_seam.cpp`）。同上一条
    // 的理由：不插进 shadow-kernel-scale 与 blend-sort 之间，那会让那两条 append 注释里的
    // 位置声明失效；-h 顺序只是打印序。
    append(shadowCascadeSeamSpecs());
    // `shadow-caster-survival` 排在 shadow-cascade-seam 之后（正文住 `cli_table_survival.cpp`）。
    // 同上一条的理由：-h 顺序只是打印序，不挪既有条目、不让既有 append 注释的位置声明失效。
    append(shadowCasterSurvivalSpecs());
    // `shadow-edge-ruler` 排在 shadow-caster-survival 之后（正文住 `cli_table_edge.cpp`）。
    // 同上一条的理由：-h 顺序只是打印序，不挪既有条目。
    append(shadowEdgeRulerSpecs());
    // `shadow-edge-rebase` 排在 shadow-edge-ruler 之后（同 `cli_table_edge.cpp`，缝按仪器族切）。
    append(shadowEdgeRebaseSpecs());
    append(specsResidualAndMotion());
    append(specsTailAndMotion());
    append(specsFlickerAndNoise());
    append(diffStructSpecs());
    append(diffBlockSpecs());
    return table;
}

}  // namespace

const std::vector<SubcommandSpec>& subcommands()
{
    static const std::vector<SubcommandSpec> kTable = allSpecs();
    return kTable;
}

}  // namespace RGL::imgqa
