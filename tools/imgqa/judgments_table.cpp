// 判据契约表 · 地基 + 静止机位 + 附件三族，外加 registeredJudgments() 的拼接。
//
// 条目正文（容差理由 / guard 出处）是**判据契约**，逐字搬自 judge.cpp 的 registeredJudgments()，
// 本批次只搬位置 + 加族字段，未改一字（对拍方法见批次 C3 交付）。
// 运动族（19 条）在 judgments_table_motion.cpp —— 表体合计 558 代码行，一个文件装不下
// （tools/ 侧护栏：单文件 < 500 代码行）。

#include "judgments_table.h"

namespace RGL::imgqa {

const std::vector<JudgmentInfo>& foundationJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"visibility", JudgmentFamily::Foundation,
            "无容差：集合断言，不是数值断言。manifest 声明的 piece 集与可见集之差要么为空"
            "要么点名，没有「差不多」。",
            {"qa_scenes.py：按预测边界定点 = 会绿 = 无效断言"},},
        {"piece-drawn", JudgmentFamily::Foundation,
            "面积下限按「该 piece 在该机位下投影应占的像素数」逐个给，不给全局比例。"
            "0 像素 = 没画；个位像素 = 被裁到只剩边。",
            {"近处盒子 +Z 面纯黑（先分叉「画了没着色」与「根本没画」）"},},
        {"face-clusters-explained", JudgmentFamily::Foundation,
            "±2 LSB / 255 ≈ 0.0078（沿用探针实测：float64 转录 vs 着色器 float32 + 8bit "
            "取整）。簇匹配间隙 0.02，因为各面 NdotL 差远大于此。",
            {"近处盒子 +Z 面纯黑", "闭式解探针实测零误差"},},
        {"switch-impact-domain", JudgmentFamily::Foundation,
            "域外**必须恰为 0**，不是「小于阈值」。正确的旁路是位级恒等（已有此性质："
            "lerp(1,x,0) ≡ 1.0、shadowPass=false 画面逐位不变）。既然是恒等，容差就该是"
            "0；给了容差等于放弃这条判据最锋利的部分。",
            {"5a-7：半径恒 0 的恒等阵伪影同时骗过稳定性对拍与开关对拍"},},
        {"dead-branch-scan", JudgmentFamily::Foundation,
            "无容差：恒等即恒等。「两个本该不同的量恒等 = 其中一个分支从未生效」。",
            {"调试纪律：恒等式日志是指纹"},},
        {"determinism", JudgmentFamily::Foundation,
            "无容差：同配置重跑必须逐位相同。任何差异（未初始化内存、同步时序、顺序抖动）"
            "都会让一切「域外恰为 0」判据失去意义——它是域判据的前置，不是另一条域判据。",
            {"AGENTS.md 测量纪律：单实例串行（并行双实例曾污染帧率测量）"},},
        {"probe-pixel", JudgmentFamily::Foundation,
            "±2 LSB：ACES 多项式 float64 转录 vs 着色器 float32 + 8bit 量化取整"
            "（沿用 12.2d-c 实机对拍实测）；再大就该当失败看待，不能靠放宽阈值混过去。"
            "2026-10-18 重立：吃的是 **G-buffer 量化往返后**的着色输入（deferred 链），"
            "量化本身由锚①② 独立钉，本锚只兜光照 + tonemap 尾段。",
            {"闭式解探针实测零误差（批 12.2d-c，analytic_ref.py 时代）"},},
        {"probe-gbuffer-albedo", JudgmentFamily::Foundation,
            "±1 LSB：期望 = 存储字节本身（round(srgbEncode·255)，硬件 sRGB store 同为化就近），"
            "留 1 档的余量只有 shader float32 输入 vs 预测 float64、平局取整模式（RNE vs "
            "half-up）两处。Δ>1 = G-buffer 编码契约变了（格式表 / 写侧 / 回读转换），"
            "与光照无关——这条锚的判据对象就是 pass 间附件本身。",
            {"2026-10-18 探针重立变异对照：deferred_geometry.frag.slang 的 albedo 写侧临时 ×0.5"
             " → 本锚红（Δ=67，直接点名写侧失效）而 metalrough 锚绿 = 分段隔离有效；还原即双绿"
             "（读数 → qa/log/imgqa_calls.jsonl 同日三条）"},},
        {"probe-gbuffer-metalrough", JudgmentFamily::Foundation,
            "±1 LSB：容差整格预留给**平局带**——探针默认 rough=0.9 的 ×255 = 229.5，闭式"
            "（含 float32 输入建模，乘回去仍舍回 229.5，half-up → 230）给不出引擎 float32 "
            "乘法链（factor×ORM、normalFiltering 的 sqrt 往返）在平局两侧的落点方向；2026-10-18 "
            "首跑实测引擎存 229（Δ1，稳定复现）。非平局输入 Δ0。失效形态都远大于 1：写侧串道 "
            "Δ=229、通道错认 Δ~255、格式换档 Δ 数十。",
            {"2026-10-18 探针重立变异对照：deferred_geometry.frag.slang 的 metalrough 写侧"
             "临时 g↔b 串道 → 本锚红（Δ=230）而 albedo 锚绿 = 分段归因成立；还原即双绿"
             "（读数 → qa/log/imgqa_calls.jsonl 同日三条）"},},
        {"shadow-swim", JudgmentFamily::Foundation,
            "0 位移 + 失配 ≤ max(20, 共同格/200)：静态几何 + 静态太阳 ⟹ 影场是世界不变量，"
            "任何机位依赖都是 bug（5a-7 三件套的端到端复验）。2cm 格分辨率对用户报的"
            "「井盖影时有时无」绰绰有余；亚格漂移本就不可见。平移搜索只认覆盖率 ≥80% 的候选"
            "（防止把「两边各遮一半」的垃圾平移当最优解）。",
            {"5a-7 平移游动 / 5a-8b 旋转游动（当年判据是人工三图对拍，本条机器化）"},},
        {"shadow-softness-gauge-valid", JudgmentFamily::Foundation,
            "下限 1.0%：本条是**量具自证**，不是画质判据——只要求「太阳角 0→5° 时，PCSS 这条"
            "不经过 EVSM 方差窗的独立实现有 ≥1% 像素翻面」。§5.1 定案时探针实测 1.955%"
            "（18019/921600，>8LSB），本命令首跑 1.928%（差 0.03pp = profile 构造路径不同，"
            "同量级）；1.0% 取在现状的一半以下：它要吸收的是驱动/帧号/量化边界带来的抖动，"
            "而不是把「量具根本没响应」也吸收掉——PCSS 的半影半径 = Δz·2tan(θ/2)/2/texelWorld，"
            "5° 时按闭式远大于 1 纹素，读数真为 0 只可能是覆写没生效（izb 没关掉 / 帧号漂了 / "
            "场景换成了量不出半影的 rig）。不给上限：本条没有「响应太大」的失效形态。",
            {"docs/shadow_routes_plan.md §5.1（J1 为什么用 PCSS 不用 EVSSM：拿被测对象当锚点 = 循环论证）",
             "docs/shadow_routes_plan.md §4.1 量具边界（shadow_rig 上 sun0↔sun5 实测 6 px / 逐位相同"
             "——场景选错时这条必红，红的是量具不是缺陷）",
             "shadow-softness 首跑（2026-10-09，8060S）：pcss 0°→5° = 1.928% >8LSB"
             "（定案时探针 1.955%）"},},
        {"shadow-softness-sun-response", JudgmentFamily::Foundation,
             "**接线判据，口径 = any-diff（不是 >8LSB），下限 0.02%**。三件事分开看："
             "① 出厂太阳角 0.53° 下 0°↔0.53° **逐位相同**（any-diff 恰 0）——因为 STEP 2.5 的核宽下限"
             "（16 图集纹素）远大于 0.53° 的物理半影（2·tan(0.265°)·Δz 在 bistro 尺度 < 1 纹素）。这是"
             "**设计事实不是缺陷**：拿它当判据 = 要求物理上不存在的软度，所以原判据（0.5% @ 0.53°、"
             ">8LSB）是**作废重定**而不是放宽。② 5° 能把半影项顶出核下限：实测 0°↔5° = 756 px any-diff，"
             "但 >8LSB = 0 px ⟹ 半影**接了线但不可见**。所以本条只保证「半影项进了计算」，可见性由 J3 "
             "的带核比负责（EVSSM 的软度来自核宽下限，不来自太阳角）。③ 0.02% = 实测 0.082% 的约 1/4："
             "容得下驱动/量化边界抖动，又远大于 0（0 = 断线）。**为什么留着而不是删**：删掉就没有任何"
             "机器判据能发现「penumbraSquashed 被改成常量 0」这类静默断线。",
             {"rgl/shaders/shadow.slang STEP 2.5（核宽下限权衡表：foot=16 时 0.53° 半影被完全压住）",
              "docs/shadow_routes_plan.md §5（J2 重定案：原判据的靶子 = 物理不存在的软度）",
              "shadow-softness 实测（2026-10-09，第 12 帧）：0°↔0.53° = 0 px any-diff；"
              "0°↔5° = 756 px any-diff / 0 px >8LSB"}},
        {"shadow-softness-vs-csm", JudgmentFamily::Foundation,
             "**两个比值都两侧有界**：覆盖率比 ∈[0.70, 1.30]、带核比 ≥0.9。"
             "① 上限 1.30 不是拍的——痤疮参照 = 实测变异「`chebyshevUpperBound` 去快路」= **1.720**，"
             "读图形态 = 整片朝光屋顶糊影（教科书自遮挡）；没有上限，下限会被痤疮刷绿。"
             "② 下限 0.70 = 实测最优工作点 0.736 的 0.95 倍，同时把改造前的 0.599 判红。"
             "③ **判据线 0.70 ≠ 转正门槛 0.9**：8 组实验 + 闭式证明「软度与不透明度同时贴上 CSM」时"
             "覆盖率比的天花板就是 0.74（权衡表在 shadow.slang STEP 2.5）。把判据线钉在够不到的 0.9 = "
             "永久红灯 = 没有信号的红灯；0.9 作为未达门槛留在台账与打印里，不留在绿灯里。"
             "④ 带核比不给上限：糊不是痤疮形态，且核宽是明写的调参键（糊是用户自己选的）。"
             "⑤ 分母取 CSM 而不是 none：§4.1 确证 B 记着软影档丢覆盖不是 EVSSM 独有（PCSS 也丢 30%），"
             "但 CSM 是当前交付档 ⟹ 比值才是用户视角的得失。",
             {"rgl/shaders/shadow.slang STEP 2.5 权衡表（foot 0/4/8/16/32 的覆盖率比 · 黑度比 · "
              "带核比 · 每影像素深度四列实测）",
              "docs/shadow_routes_plan.md §4.2 三实验归因（快路 ON = 漏光 5.89%、OFF = 痤疮 16.92%）",
              "docs/shadow_routes_plan.md §5（EVSSM 转正不成立的终局裁决与证据链）",
              "变异红线：min_footprint_texels 改 0 → 本条红（实测 0.600 / 0.208）"}},
    };
    return kPart;
}

const std::vector<JudgmentInfo>& temporalStaticAnchorJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"temporal-flicker-anchor", JudgmentFamily::TemporalStatic,
            "变化像素占比下限 0.5%：激励 = jitter（±0.5px/帧，Halton 周期 16），靶的亚像素"
            "边缘按闭式投影约占靶面 11%（画板边界 2×(869+248) + 12 杆竖边 + 6 环 + 16 放射"
            "+ 12 斜杆 + 栅栏），±0.5px 的相位跳变让每条边几乎每帧都跨像素中心 ⟹ 期望远大于"
            "1%。下限 0.5% 只吸收「靶根本没画 / 投出屏 / id 面为空」这类归零失效；均值同时"
            "必须 > 0。⚠ 本条量的是 S1（feedback=1 ⇒ 输出 = 本帧，只抖不滤），不是「TAA "
            "关」——理由见 temporal.cpp 头注释「为什么激励是静止机位 + jitter」。",
            {"docs/taa_plan.md §6 阳性锚点（缺它 = 「0 差异」的两种解释堵不住）"},},
        {"temporal-aa-presence", JudgmentFamily::TemporalStatic,
            "两条互为阴阳，缺一条就是假绿：① **TAA 关时中间调须恰为 0**（不是「接近 0」）——"
            "无 MSAA 的光栅化是二值覆盖，实测全图只有 3 个亮度级（背景 clear / 白板 / 黑件），"
            "任何非零说明靶画错了或链路上混了别的混合；有这条反向对照，② 才不是「靶本来就糊」。"
            "② TAA 开中间调 ≥ 1.0%：闭式预期约 11%（靶的亚像素边缘像素占比），下限取 1/10 "
            "只吸收「邻域裁剪把一部分历史拉回本帧」；带按**实测白电平**取（ACES 把白压到 ~0.8，"
            "写死绝对带会把整块白板算成中间调、83% 恒饱和——真实踩过的假判据，见 selftest）。"
            "⚠ 本条量的是**空间**抗锯齿在场，不是时域抑制比——后者不能用屏幕空间逐帧差量，"
            "见 `screen-space-l1-frame-diff`。",
            {"docs/taa_plan.md §6（2026-10-03 执行时由逐帧差判据改立）"},},
        {"temporal-suppression-sqrtN", JudgmentFamily::TemporalStatic,
            "16 相位抖动 = 对同一视点的 16 个独立采样相位求平均 ⟹ 逐样本估计噪声按 **√N = 4.00**"
            " 下降（闭式，不依赖实现）。下限取 **3.0×**（= √N 的 75%）只吸收两件事：邻域裁剪会把"
            "一部分历史拉回本帧（相位间不再独立 ⟹ 实际降幅低于 √N），以及 Halton 是低差异序列而"
            "非独立随机（方差略优于独立 ⟹ 实际降幅也可能略高于 √N）。实测 3.99×；CR 核修复后（phase13 §1.8）"
            "重测 **3.51×**——仍高于 3.0× 下限，但离闭式 4.00× 更远：核形状变了，重采样不再等价于理想 N 相位平均。\n"
            "⚠ **观测空间是「变化像素上的平均幅度」，不是 L1 总量**——这是本判据与被打反模式的"
            "`temporal-taa-suppression` 的唯一但决定性差别：L1 总量 = 幅度 × 变化像素数，边缘摊开"
            "后两者互抵（实测 3.99× × 1/2.88 = 1.38×），对模糊守恒 ⟹ 量不出抑制。闭式预测从来"
            "是对的，**是它的适用域（逐样本幅度）被错配到了总量上**。修好历史销毁 bug 后两者同时"
            "对上，反模式条目保持有效。",
            {"docs/taa_plan.md §6 第 5 条 + §7 第 12 条（2026-10-03 换观测空间后重立）"},},
        {"temporal-swim-anchor", JudgmentFamily::TemporalStatic,
            "**免费标定，无阈值**：lag = 一个完整抖动周期（16）时，Halton 预测位移恰为 0 ⟹\n"
            "同相位两帧必须**逐字节相同**。这一条不给「按 Halton 预测做判据」留任何讨价空间：\n"
            "它非零就说明连拍不是相位 16 的确定性函数（窗口没对齐周期 / 历史没收敛 / 抓的不是\n"
            "连续帧 / 抖动公式与本工具重抄的不一致），此时本命令所有按预测算的数**全部作废**。\n"
            "首轮实测：T1、T2 的 lag-16 差都恰为 0.000/255。",
            {"phase13 §1.6 末「震的当前解释（未定案）」；前两版量具（自由拟合 / 搬未抖基图）"
             "的作废理由原地可读 = temporal.cpp 头注释"},},
        {"temporal-identity", JudgmentFamily::TemporalStatic,
            "**恒等式，容差 = 舍入级（均值 ≤0.05/255 且逐像素 max ≤4/255 且 dev≥2 像素占比"
            " ≤0.01%；当 dev≥2 像素数 ≥1000 时另须「残差 ≥80% 落在边缘上」）**。"
            "配置 = `jitter_scale"
            " = 0` + 静止机位：每帧画面相同 ⟹ 历史与本帧是同一张图 ⟹ 重投影必须落在 `prevUv ≡ uv`"
            " ⟹ CR 核退化成单抽头（t=0 权重 (0,1,0,0)）⟹ 裁剪恒等 ⟹ 混合两项相同 ⟹ 输出与 TAA 关"
            "**逐字节相同**。\n"
            "为什么不是「无容差」：`prevViewProj` 是 CPU 侧 `(P·V)·pos`，本帧是着色器里 `P·(V·pos)`，"
            "浮点结合序不同 ⟹ `prevUv` 与 `uv` 差最后几个 bit ⟹ CR 权重不是精确 (0,1,0,0) ⟹ 混合"
            "进来 ~1 LSB。故 **max 才是判据**：均值小可能是「大量像素各差 1 LSB」（结合序，无害），"
            "也可能是「个别像素差几十 LSB」（实现缺陷）——故 max 与「dev≥2 占比」两条一起卡"
            "（max 卡离群、占比卡系统性；2026-10-04 前写的是 max ≤2，那是合成档上的读数，"
            "交付配置 + 用户机位上修复后的地板是 max=3 / 30 像素，见下）。\n"
            "这条的价值 = 把「抖动带来的固有代价」**整个摘掉**：残余差异与抖动幅度无关，只剩实现"
            "本身。与 `determinism` 互为阴阳——那条钉「关的时候要稳」，本条钉「开着但关掉激励时"
            "必须等价于关」。\n"
            "**战果（2026-10-03）**：本条首次运行即 FAIL（离基图 1.337/255），顺藤摸到 CR 核写成"
            "带符号 `a³`（应为 `|a|³`）⟹ t=0 权重 (0,1,−3,8)、Σ=6 ⟹ 2D 直流增益 36× ⟹ 历史被 "
            "saturate 顶界后 AABB 判越界 ⟹ 解释了 §1.7 的「98.3% 像素拒收历史」。修后残差 0.016。"
            "**但那 0.016 是合成档读数**：换到交付配置（deferred + env/AO/shadow 全开）+ 用户机位，"
            "同一条判据是 1.688/255、max 37、10.37% 像素 dev≥2。\n"
            "**战果（2026-10-04）**：上面那个 10.37% 的真凶 = **抽头与历史采样上的 `saturate()`**。"
            "本 pass 在 tonemap 之前，通道 ≫1 是常态；`saturate` 把 HDR 亮部顶平到 1.0 ⟹ 时域"
            "不动点从 `X = cur` 变成 `X = (1−f)·1.0 + f·cur` ⟹ **白色屋顶 8.83% 像素系统性压暗"
            " 18.5 LSB（最大 37）**，而该区域之外 mean|Δ| 只有 0.059。改成只夹下界（Reinhard 本身"
            "就负责把无上限 HDR 压进 [0,1)）后：mad 1.688 → **0.0226**、max 37 → **3**、dev≥2 "
            "10.37% → **0.0033%**、压暗区 −18.5 → **−0.014**。反向对照 = 把 `saturate` 换回去，"
            "判据立刻红。⚠ 同批被排除的错误归因：邻域盒退化（塌回 centroid）——两种修法"
            "（extent 相对下限 K=0.02…0.4 / 退化时跳过裁剪）在压暗区**逐位不改**（−18.506 全同），"
            "因为 js=0 稳态与裁剪算子无关（收敛点由「历史怎么被夹」决定，不由「历史怎么被裁」决定）。",
           {"docs/taa_plan.md §7 第 12 条（历史销毁那次的恒等式检查）",
            "docs/archive/phase13_plan.md §1.8（CR 核奇偶性 bug 全案）",
            "rgl/shaders/taa.frag.slang `clampHdr` 头注释（2026-10-04：saturate 顶平 HDR ⟹ 亮部压暗 18.5 LSB 全案）"},},
        {"temporal-fitter-anchor", JudgmentFamily::TemporalStatic,
            "**阳性锚点 + 反向用例（拟合器自己的刻度）**。把零抖动参考 R 按**已知**位移 "
            "(3.25, −2.125) 用 `warpAffine` 做成一张假抖动帧，再让 `freeShift`（相位相关）去量"
            "它：两轴误差各 ≤0.25px 且位移必须把残差压到 <10%；**反向用例** = 零位移须量回 "
            "|d| ≤0.25px（堵『拟合器卡常数』——只有阳性锚点时恒返回常数的坏拟合器也是绿的）。"
            "位移取值 = 非整、非零、双轴、跨符号，且**避开半整数** —— 尺度/符号/卷绕任何一处"
            "写错都会在这里暴露（尺度错 2× → ≥3.25px、符号反 → ≥6.5px、整数错位 → ≥1px）。\n"
            "⚠ 旧锚点位移 (3.50, −2.25) 的 x 恰为半整数，踩在 phaseCorrelate 三点抛物线亚像素"
            "拟合的奇点上：本场景 14 组已知位移实测，半整数处误差恒 −0.320±0.002px（x∈{1.5,"
            "2.5,3.5,7.5,−3.5}），四分之一整数/整数处 ≤0.002px ⟹ 旧读数红的是**估计器奇点不是"
            "拟合器分辨率**。2026-10-04 换探针位移出奇点，**阈值 0.25px/10% 一字未动**；奇点保留"
            "为只打印诊断。实测量程（T4 = 8j ∈ ℤ、T1/T2 ∈ {±⅛,±¼,±⅜}）从不落奇点，逐帧误差 "
            "0.030~0.063px。\n"
            "为什么必须有：本命令的『实测位移』全部建立在这个拟合器上，而**辅助函数不会自己喊**"
            "。第一版是手写网格搜索，把 1/4 降采样的步长当成全分辨率像素去 warp，于是搜索域只有 "
            "±2.25px 却报成 ±9px，打印出『拟合位移 7.9px、拟合后残差反而比不拟合更大』这种自相"
            "矛盾的数，而当时**没有任何一条判据拦它**——差一点就据此得出『抖动不产生平移』的错误"
            "结论。`resid ≤ zero` 是拟合器按构造必须成立的性质，它一破就说明拟合器坏了。",
           {"本命令 `freeShift` 头注释；skill rgl-test-discipline（辅助函数也要独立判据）"},},
    };
    return kPart;
}

const std::vector<JudgmentInfo>& temporalStaticJitterSpaceJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"temporal-jitter-transfer", JudgmentFamily::TemporalStatic,
            "**绝对口径**（本命令里唯一不靠「帧对帧差分」的判据），跑在 `feedback=1` + "
            "`jitter_scale=8` 组，参考 = T0（TAA 关，逐帧差恰为 0 ⟹ 第一帧就是零抖动参考 R）。"
            "fb=1 时输出 = 本帧光栅 ⟹「抖动帧 = R 平移 d_i」**按构造成立**，d_i 由相位相关量出。"
            "四条各自堵一种失败：\n"
            "① 实测 |d| 均值相对预测 ≤25% ⟹ 抖动确实按文档幅度把画面搬起来了（堵『注入断了』与 "
            "px/NDC 单位错配——后者至少差 2×）；\n"
            "② 逐帧误差 ≤25% 预测均值 ⟹ 搬的**序列**就是 Halton(2,3) 周期 16（堵『搬了但不按"
            " Halton 搬』）；\n"
            "③ 位移把「抖动帧 ↔ R」的差压到 ≤50% ⟹ 两图之间**确实是平移**。残差地板不是零而是"
            "「按亚像素偏移重新光栅化」的覆盖翻转，**随场景内容变**：合成 profile 档 18.9%、"
            "交付配置 + 用户机位 + 整帧档 36.7%（2026-10-04）——旧 30% 阈值是前档标定的，换"
            "场景后红的是地板不是模型。区分力：搬错相位 92.1%、不搬 100%，失败模式与地板隔着 "
            "55pp；地板在场性由 `temporal-swim-discriminative` 反向守；\n"
            "④ 匹配出的**额外**符号必须是 (+1,+1) ⟹ **坐标约定守卫**：预测函数 "
            "`jitterDeltaPx` 已把 freeShift 口径（= 图像内容位移 c_t 的相反数，c_t = "
            "`scale·(+jx, −jy)`，逐轴推导与最终裁决见 `rgl/render_list_impl.cppm` 与 "
            "`temporal.cpp::jitterDeltaPx` 头注释）写进预测，匹配器再需要翻号 = 引擎改了投影/"
            "翻转/抖动符号而预测没跟上。改投影、改翻转、改抖动符号都会在这里炸。\n"
            "**实测（2026-10-03 标定 / 2026-10-04 交付场景复量）**：① 2.917 vs 2.956px（1.3%）"
            "/ 2.984 vs 2.978px（0.2%）② 逐轴符号搜出后 RMS 由 1.748px 塌到亚像素 ③ 19.3% → "
            "36.7%（场景档差，见上）④ 额外符号 (+1,+1)、φ 每次启动常数（3/11/15 实测）。\n"
            "⚠ 搜索空间少一维，结论就会反过来：第一版只试「两轴同一符号」，永远匹配不上，于是"
            "把口径错配误读成『画面搬了但不按 Halton 搬』。",
           {"docs/archive/phase13_plan.md §1.8/§1.9", "git:调试历史 2026-10-03"},},
        {"temporal-swim-pixels", JudgmentFamily::TemporalStatic,
            "**单位 = 像素**，直接对应肉眼，不需要观测域/阈值换算：量「现行工作点（fb=.12、"
            "js=1）相邻两帧输出之间的整幅位移」（相位相关逐帧量出绝对位移后取相邻差）。闭式期望"
            " = 相邻两帧的抖动差 = `mean|j(i+1) − j(i)|`（一个周期内均值，零自由参数），实测须落在"
            " [0.75, 1.35]×。\n"
            "**这条钉的是机制，不是好坏**：若 TAA 的输出落在**基空间**，相邻帧位移应 ≈ 0；实测 ="
            " 抖动差 ⟹ 输出落在**抖动空间**（`out_t(uv) = F(uv − j_t)`，§1.6 假设成立）。且时域"
            "累积**压不掉**它——同表 `feedback=1`（完全不累积）的游动量几乎同值，因为重投影把历史"
            "对齐在基空间、而**输出采样位置**仍在本帧抖动位置，累积只改 F 不改 j_t。\n"
            "实测（2026-10-03 本机 bistro 1280×720）：fb=.12 游动 0.717px 均值 / 1.051px 最大，"
            "fb=1 游动 0.674px / 1.058px，闭式期望 0.667px。\n"
            "⚠ 为什么不用「逐帧差均值（0..255）」当游动量：那是观测域的量，边缘覆盖翻转会把它"
            "放大/缩小，说不清「几像素」。像素就是像素。",
           {"docs/archive/phase13_plan.md §1.9", "git:调试历史 2026-10-03"},},
        {"temporal-base-space", JudgmentFamily::TemporalStatic,
            "三条同时成立（批 13.2，门 `taa.base_space`）：① 门开态（T5 = T2 + 门）相邻帧整幅"
            "位移 ≤ **0.35×** 闭式抖动差；② 门关态（T2）游动仍 ≥ 0.75× 闭式 —— **阳性锚点**，"
            "堵「量具坏了 ⟹ swing 恒 0 ⟹ ① 白过」；③ 门开态 ≤ 门关态的一半 —— 堵「门没接线"
            "⟹ T5 与 T2 是同一份画面」。\n"
            "0.35 这个上限**不是**「效果越好收得越严」，而是量具地板：`freeShift` 是相位相关"
            "拟合，静止对静止时它自身的亚像素偏置就是零点几像素量级（同表 `temporal-fitter-"
            "anchor` 的自检读数 0.32px 就是同一把尺的分辨率）。真正的余量在 ③：门开态实测比"
            "门关态低一个数量级时，①② 都不需要重新标定。\n"
            "⚠ 本判据与 `temporal-swim-pixels` 是**对偶**的，不是替代：后者钉「门关态输出长在"
            "抖动空间」这个机制事实（门改了它就该红，那是回归），前者钉「门开态把它搬回了基"
            "网格」。两条在同一次运行里同时绿，才说明门是**开关**而不是**默认值漂移**。",
            {"docs/archive/phase13_plan.md 批 13.2", "git:调试历史 2026-10-04（游动 = 机制不是 bug）"},
        },
        {"temporal-jitter-phase", JudgmentFamily::TemporalStatic,
            "**量具自检（差分口径的相位标定）**：采集下标与引擎抖动序号之间差一个未知常数相位"
            "（引擎 `frameIndex` 自己起算 + 序号再 `+1`），而 `lag = 16 同相位差恰为 0` 那条免费"
            "标定**只能验周期、验不出常数偏移**。于是对 16 个候选相位各算一次「按预测位移搬完」的"
            "残差，argmin 必须显著塌下去（≤50% 不搬基线）且**唯一**（次小 ≥1.25×）。\n"
            "⚠ 实测教训：本条在 ±4px 档是**浅谷**（最优与次名只差 1%），也就是说「按预测位移搬」"
            "这个差分算子根本没有足够的信息量同时定出相位——**浅谷本身就是结论**：该口径不足以"
            "检验游动假设，必须换成绝对口径（见 `temporal-jitter-transfer`）。保留本条是为了让"
            "下一个人不再把浅谷 argmin 当成测量结果。",
           {"本命令头注释「两版废掉的量具」；`rgl/taa_jitter.cppm`（序号 = frameIndex%16 + 1）"},},
        {"temporal-jitter-calibration", JudgmentFamily::TemporalStatic,
            "**量具自检（λ 一维扫描）**，跑在 `feedback=1` + `jitter_scale=8` 组上。着色器尾部是 "
            "`lerp(clipped, cur, params.x)`，**fb 是本帧权重**（Filament 序）⟹ fb=1 时输出 = 本帧"
            "光栅（无累积、无糊），相邻两帧之间按构造 = 沿 Halton 闭式位移 `j_{t+1} − j_t`（×8）的"
            "**整幅平移**。于是沿该方向做带符号 λ 扫描，极小值必须落在 **λ = +1**：\n"
            "· argmin → 0 ⟹ 抖动**没有**把画面搬起来（jitter 注入断了）；\n"
            "· |argmin| 明显偏离 1 ⟹ 幅度口径与 `rgl/taa_jitter.cppm` 不符；\n"
            "· 只有 λ=−1 命中 ⟹ 引擎与本工具的 x/y 符号口径相反（`shiftInto` 的方向定义就错了）。\n"
            "为什么用一维扫描而不是自由拟合二维位移（那是本命令废掉的第 ① 版）：自然图像的 L1 "
            "极小值很浅，两个自由度无标定 ⟹ 拟合位移逐帧乱跳（实测对预测 RMS 0.33px）。这里方向"
            "已知、只剩一个标量，且 `λ=0` 那一档就是「不搬」的原始逐帧差 ⟹ 天然自带归一化分母。\n"
            "⚠ 本条红 ⟹ 本命令所有「按预测位移搬」的读数（含 `temporal-swim-*`）**全部作废**。",
           {"本命令头注释「两版废掉的量具」；`rgl/taa_jitter.cppm`（Halton(2,3) 周期 16）"},},
        {"temporal-swim-discriminative", JudgmentFamily::TemporalStatic,
            "**只在 ±4px 大抖动档（`jitter_scale=8` + `feedback=1`）判**。两条同时成立：\n"
            "① 搬对相位的残差 ≤50%：fb=1 时输出 = 本帧光栅，该组按构造 = 基图整体平移 8·jitter_t，"
            "所以按 Halton 闭式位移把前一帧搬过去必须显著压掉差异。压不下去 ⟹ 引擎的抖动**没有**"
            "把画面搬起来（那是比「游动」更根本的问题，得先查 jitter 注入）。\n"
            "② 搬错相位（相位整体错半个周期，量级相同方向系统性错开）的残差比搬对高 ≥15%：\n"
            "这是算子自身的区分力检查。首轮实测在 ±0.5px 档**两条都不成立**（搬对反而更差"
            "120%、搬错也不更高）——不是引擎的错，是 ±0.5px 的平移量与边缘二值覆盖翻转同量级，"
            "算子在那里本来就没力气。把抖动推到 8 倍让平移项压过覆盖翻转项，判据才有力气。\n"
            "⚠ fb 口径**别凭记忆写**：fb=0 是「输出 = 纯历史」的退化环（不是「无滤波」），拿它当"
            "「本帧档」会让整组读数失去意义，而读数本身看起来完全正常——本轮实测踩过，靠 "
            "`temporal-swim-anchor` 的 lag-16 恰为 0 抓出（当时 0.068 ≠ 0）。\n"
            "⚠ 本条与 `temporal-swim-anchor` / `temporal-identity` / `temporal-jitter-calibration` "
            "一样是**量具自检**。",
           {"docs/taa_plan.md §7 第 12 条末（box9 消融对竖直杆零区分力）；本命令头注释"
            "「两版废掉的量具」"},},
        {"aa-coverage-fidelity", JudgmentFamily::TemporalStatic,
            "两条同时成立才绿，各堵一种假绿。**判据域 = 投影宽度 ≥2 列的杆**：① **TAA 开：域内"
            "最大逐杆黑度积分误差 ≤ 0.35 列**（闭式期望 = 投影宽度，白件 unlit 线性 1、黑件 0，"
            "覆盖 c ⟹ 线性 1−c；0.35 列 = 域内最窄杆 2.25 列的 1/6，即「把杆宽画错六分之一以上」"
            "才算坏）。② **TAA 关必须显著更差（≥3×）**——反向对照：没有它，① 可能只是靶太粗、"
            "二值覆盖也刚好贴闭式。实测 TAA 关的误差来自量化（每根 = `floor(期望)`，误差 ~0.2–0.9 "
            "列），与 ① 的量级差是结构性的，不是调出来的。\n"
            "⚠ **域外（<2 列）不是调阈值调出去的，是算法类没有保证**：邻域裁剪 TAA 对 ≲2 列的细杆"
            "必然欠累积——错相帧里本帧没采到杆 ⟹ box5 = 全白 ⟹ 已累积的黑被判在盒外拒掉，单帧值"
            "由抖动相位决定，不是稳定质量量。窄杆仍打印，欠累积量级记进 plan §7 记债（box9 / "
            "variance 扩盒 / Lanczos filterInput 是缓解方向）。\n"
            "⚠ 覆盖率由 `1 − aces⁻¹(luma)` 反解，正向复用 `analytic.cpp` 的闭式 ACES（"
            "`imgqa probe` 已拿真机 ±2 LSB 对拍验证），不新引入第二条曲线。\n"
            "⚠ 本判据是**单帧**判据，与采集时序无关——这是它取代屏幕空间逐帧差的根本理由"
            "（后者对模糊守恒，见 `screen-space-l1-frame-diff`）。",
            {"docs/taa_plan.md §7 第 10 条（逐杆覆盖率对拍闭式）"},},
    };
    return kPart;
}

const std::vector<JudgmentInfo>& attachmentJudgments()
{
    static const std::vector<JudgmentInfo> kPart = {
        {"izb-consume-domain", JudgmentFamily::Attachment,            "变暗阈 = 三通道和差 >30：rig 调色板 albedo（≥0.5）× 方向光 12 → 影差远大于 30"
            "（ladder 首跑实测：变暗 4705/域外 0/未消费 0）；域容差 = dilate(mask,2px)：mask "
            "与截图同帧同源，2px 只吸收保守光栅的过估边（mask 侧只会多遮不会少遮，方向安全）。"
            "三条子判据：锚点（变暗集非空 = 消费活着）、变暗域 ⊆ mask（寻址对得上）、mask∩受光 "
            "⊆ 变暗域（有遮必消费）。⚠️ 边界：本判据 mask↔画面**同源**，生产者侧几何错（ndc "
            "镜像类）会同步污染 mask 与画面 → 本判据不可见，那半边由 test_izb_engine_contract"
            "（A1 闭式）钉；mask vs manifest 预测影包络（生产者端到端）= 已识别扩展，未做。"
            "⚠️ **绿不等于画质可用**（2026-10-10 补）：本判据只测寻址一致性，变亮集（rig 405048 px "
            "= 占屏 44%）是 IZB 系统性欠遮挡的读数、被三条子判据整个漏掉——正是这个漏判让 IZB 出厂开了"
            "很久。交付已据此关 izb（docs/shadow_routes_plan.md §7）。",
            {"2026-10-02 消费端字节偏移 ÷4 bug（门开无影）→ 锚点+消费两红——本判据设计原型即那次手工对拍"},},
        {"ao-noise", JudgmentFamily::Attachment,
            "**主判据的容差来自恒等式，不是相似度**：AGENTS.md 登记的契约「开阔地面 AO 恒 "
            "1.000」在 AO 附件（r8_unorm 可见度）上就是字节 255；`ao.frag.slang:118` 的 "
            "strength≤0 早退是**位级恒等**，实测 strength=0 时附件全图逐位 255（σ=0.000）——"
            "这就是噪声读数的**恰为零的锚点**，不需要基线图像。主判据取**高频项**（medianBlur(5) "
            "残差 RMS）≤ 1.0 LSB：1.0 = 8bit 量化步长，低于它噪声被量化吞掉、交付面上不可见；"
            "而高频项对合法的光滑遮蔽梯度不敏感，这是它在区域定义不干净时仍然成立的唯一理由。\n"
            "**为什么「偏离 255 的幅度」不作判据（实测推翻的初版口径）**：「离轮廓 > 40 px」"
            "**不是**世界空间的契约区——AO 半径 0.5 是世界量，靠近相机的表面在轮廓内 40 px 处"
            "世界距离可远小于 0.5 m（实测 box_short 正面上离轮廓 >40 px 处 AO 低到 37，是合法"
            "遮蔽）。故偏离契约值只作观察量。取而代之的硬判据是**因果泄漏恒等式**：AO 附件 = 255 "
            "的像素（876083 个）上，AO 开/关两张交付图必须**逐位相同**（实测 max = 0 LSB），"
            "且交付 Δ 随 AO 偏离单调上升（AO≥245→max 3、≥235→5、≥225→8、≥215→10、≥195→16）"
            "——附件是交付差的唯一来源，零容差、零自由参数。\n"
            "**四条子判据互为阴阳**（缺一条就是假绿）：① 反向 = strength=0 时附件逐位 255；"
            "② 阳性 = 凹角接触环**存在**深遮蔽（min AO < 128）且凸边对照干净（球轮廓带 <255 占 "
            "<2%）——⚠ 首跑拿「接触环均值/变暗占比」当锚点是错的口径（环宽 24 px，绝大多数像素"
            "按闭式就该是 1），会把锚点太宽误读成「AO 没跑」；③ 因果泄漏 = 上述恒等式；"
            "④ 交付面 = 契约区 A/B 字节差的颗粒项 ≤ 1.0 LSB（实测 0.203）、接触环 ≤ 2.0 LSB"
            "（此处含接触台阶残差，medianBlur 减不掉阶跃，故放宽 2×，只作下限）。\n"
            "**A/B 必须同进程**（`Engine::setParam`）：跨进程两张图之间隔着整个渲染环境，把它们"
            "之差归因给 AO 是错的（实测跨进程在契约区出现 41 LSB，而同进程为 0）。\n"
            "**分辨力**：同一运行里接触环的抖动是契约区的 5.3 倍（6.96 vs 1.31）、掠射机位是正视"
            "的 2.62 倍 ⟹ 通过线离「AO 真的在算」的信号差一个数量级以上，不是「勉强没炸」。\n"
            "⚠ 本判据**只在程序化靶上有效**（纯色 + 无 TEXCOORD ⟹ 底图在 piece 内部零空间高频，"
            "实测底图抖动 RMS 0.012）。bistro 上分母不是 0，「契约区」概念根本不成立；`--bistro` "
            "那组只带**因果泄漏恒等式**进真实场景（实测 767640 个 AO=255 像素 max 0 LSB），其余 "
            "全是观察量（交付字节Δ 在 AO 生效处均值 8.24 / p99 32 / 最大 51 LSB，颗粒/渐变 0.60 "
            "—— 但 bistro 的 AO 场自己就贴着立面几何，颗粒里混合法高频，该比值只是噪声占比的"
            "**上界**）。",
            {"docs/phase11_plan.md §8.4：开阔地面 AO 恒 1.000 = depth-only 的代价（契约，非 bug）",
                "docs/phase11_plan.md:351（拿无闭式的量当主判据的反例）",
                "rgl/shaders/ao.frag.slang:118（strength≤0 位级早退）/ :134（hash12 只吃 "
                "SV_Position ⟹ 噪声屏幕锚定）"},},
        {"ao-noise-temporal", JudgmentFamily::Attachment,
            "**本条记录的是「量具有分辨力」，不是 AO 的时域结论**（初版在这里写错了前提，实测推翻）："
            "原以为「TAA 开 + 静止机位 + AO off 时契约区逐帧差恰为 0」（纯色平面 + 无边缘 ⟹ jitter "
            "不改颜色）。实测**地板非 0**：契约区每帧 7.45 万像素在变、灰度均值 3.176/255——因为 "
            "deferred 的 GGX 高光是视相关的，jitter 改视向量就改高光，与有没有边缘无关。"
            "地板非 0 ⟹ AO 开/关的比值（实测契约区 1.000×、接触环 1.000×，两轮 burst 从同一抖动相位起步）**不可解释**，"
            "正是已登记反模式 `screen-space-l1-frame-diff` 的形态（分母不是零），故本组不判 AO，"
            "只判量具：burst 逐帧确实在变（地板 > 0）——若 burst 交的是同一帧，「AO 没有时间噪声」"
            "会假绿。\n"
            "**AO 的时域结论改走两条恰为零的锚点**（都在 TAA 关态）：① 静止机位下同参数连拍"
            "**逐位相同**（确定性锚①）⟹ 静止时零逐帧噪声；② 机位动整 1 px 时图案**不**跟着几何"
            "走（组 3，幅度 RMS 与集合 Jaccard 两个独立算子一致）⟹ 噪声表现为「爬行」而非「闪烁」。"
            "两条都有零锚点，因此不需要任何比值。",
            {"skill rgl-verify：凡被测项本身是时域的，必须 /shotburst 连拍测相邻帧差",
                "反模式 screen-space-l1-frame-diff（分母非零的逐帧差比值）"},},
    };
    return kPart;
}

// ── 族元数据：标题 + **成立条件**（check 分组输出的口径单一真相源）─────────────────────
//
// 族不是主题分类，是**成立条件的分组**：下面两条 condition 的差别就是本次拆分的主要收益，
// 写成空话等于没拆。
const char* judgmentFamilyTitle(JudgmentFamily family)
{
    switch (family) {
        case JudgmentFamily::Foundation:
            return "地基（可见集 / 闭式解 / 确定性 / 影响域 / 影场不变量）";
        case JudgmentFamily::TemporalStatic:
            return "静止机位 + jitter 的时域机制";
        case JudgmentFamily::TemporalMotion:
            return "运动 / 残余 / 鬼影尾长 / feedback";
        case JudgmentFamily::Attachment:
            return "附件消费域（IZB / AO）";
    }
    return "?";
}

const char* judgmentFamilyCondition(JudgmentFamily family)
{
    switch (family) {
        case JudgmentFamily::Foundation:
            // 与机位是否运动无关——这一族是其余两族的求值域与算子来源。
            return "与机位运动无关：区域 = objectID 反解出的可见集，预测 = 闭式解，比较算子 = "
                   "逐位相同或集合相等（无容差的那几条正是本工具的牙）。";
        case JudgmentFamily::TemporalStatic:
            // 成立条件写死在这里：S 组之所以能拿比值对拍闭式，全靠平均视点恒定。
            return "成立条件 = **平均视点恒定**，唯一变化是 jitter 的采样相位。此时消融组与默认组"
                   "输入统计完全相同，比值才是时域累积本身的增益（可拿一阶 IIR / sqrt(N) 闭式对拍）。"
                   "视点一旦真的移动，这个前提就没了 ⟹ 比值口径换到下一族，别跨族比数。";
        case JudgmentFamily::TemporalMotion:
            return "视点或场景**真的在动**（相机帧驱动 yaw / spin），或长窗档（α=0.01）把运动项摊进"
                   "≈100 帧窗。"
                   "运动项是正确影像、TAA 压不掉 ⟹ 本族不用「静止两组比值」那套口径，改用相位折叠、"
                   "闭式 ρ*、尾长与事件计数；同一台机器上两族的数不可互换解释。";
        case JudgmentFamily::Attachment:
            return "判的是「这张附件被谁消费、消费域对不对」（IZB 消费域、AO 场与交付颗粒），"
                   "与 TAA 的时域比值口径无关。";
    }
    return "?";
}

// ── 拼接：顺序 = 四族拼接后的顺序 = 拆分前的 43 条现序（逐字不变）──────────────────────
const std::vector<JudgmentInfo>& registeredJudgments()
{
    static const std::vector<JudgmentInfo> kAll = [] {
        std::vector<JudgmentInfo> v;
        const auto append = [&v](const std::vector<JudgmentInfo>& part) {
            v.insert(v.end(), part.begin(), part.end());
        };
        append(foundationJudgments());
        append(temporalStaticAnchorJudgments());
        append(temporalStaticJitterSpaceJudgments());
        append(temporalResidualJudgments());
        append(temporalTailBoostJudgments());
        append(temporalTailGuardJudgments());
        append(temporalTailCameraJudgments());
        append(temporalTailStepJudgments());
        append(temporalFeedbackClosureJudgments());
        append(attachmentJudgments());
        append(shadowKernelScaleJudgments());
        append(ssrDiffStructJudgments());
        append(mdiDiffBlockJudgments());
        append(blendSortJudgments());
        append(blendLitDrawCountJudgments());
        append(blendLitPixelJudgments());
        append(shadowGrainJudgments());
        append(shadowCascadeSeamJudgments());
        append(shadowCasterSurvivalJudgments());
        append(shadowEdgeRulerJudgments());
        append(shadowEdgeRebaseJudgments());
        append(materialSwimJudgments());
        // 材质场的**连续量半段**（债-154，4 条）紧跟二值半段：同一台仪器的两种统计量，相邻
        // 打印才能让读的人看见「换成连续量之后界挂在哪个实测上」，以及 J8 那条「旧绿新红」。
        append(materialSwimContinuousJudgments());
        // `shadow-swim-flat`（债-139 结案批，5 条）紧跟 material-swim：同一个形状的两副面孔
        // （材质场 / 影场），相邻能让 `imgqa check` 的读者看见「换了口径之后判据变成什么」。
        append(swimFlatJudgments());
        append(swimFlatFarJudgments());
        return v;
    }();
    return kAll;
}

}  // namespace RGL::imgqa
