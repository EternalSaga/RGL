// temporal-motion —— **相机运动下 TAA 到底抗了什么锯齿**（Phase 13 诊断判据，只出数 + 一句判决）
//
// ## 为什么必须有这条（问题本体）
//
// 用户报「**相机在动**（WASD/鼠标转视角）时边缘闪烁很严重，和没抗锯齿一样」。现有时域判据
// 全部**量不到**这句话：
//  - `temporal` / `temporal-residual` 钉的是**静止机位**（激励 = jitter），它们的头注释里
//    明确写着「静止机位下 TAA 关 = 逐位相同 = 零信号，所以激励必须是 jitter 而不是运动」——
//    那是为了把「视点真的动了」这个混淆量摘干净。本条**反过来**：视点动就是被测项。
//  - `taa-swim` 量「逐帧差能不能被一个全局位移解释」（游动），它不问「离理想多远」。
//  - 反模式 `moving-camera-l1-ratio` / `screen-space-l1-frame-diff` 否决的是**逐帧 L1 之比**
//    （病根 = L1 对模糊近似守恒 + 视点变化不是噪声）。本条**不是**那条：见下一节。
//
// ## 本条问的是一个有唯一答案的问题
//
// 「候选交付图，在**同一视点**下，离『没有锯齿也不糊的 720p 图』有多远？」
// 参考 = **同机位、同帧序列**的 2560×1440 渲染（TAA 关）→ **线性域** 2×2 面积平均降到
// 1280×720。超采样是「抗锯齿该长什么样」的定义级答案，不需要基线图像。
// 主指标 = 候选与参考在**边缘像素**上的线性域平均偏差；副指标 = 边缘梯度能量比（机制判读）。
//
// ⚠ **与反模式的区别是"对齐"两个字**：逐帧差比较的是「相邻两帧的同一屏幕像素」= 不同世界点；
// 本条把候选第 i 帧与参考第 j 帧**按帧号配对到同一视点**之后再比，运动项被完全消掉。
// 所以它不受 L1 守恒病影响——但也因此**依赖对齐正确**，护栏见 `temporal-motion-alignment`。
//
// ## 运动源 = **相机**帧驱动 yaw（默认；spin 降级为可选模式）
//
// `--motion camera`（默认）：camera.lua `frame_yaw` ≠ 0 ⟹ yaw = 基准 + 每帧角·帧号（纯函数）。
// 为什么必须是**相机**：spin 转的是**场景**、相机不动 ⟹ `prevViewProj` 恒定 ⟹ TAA 历史
// 重投影退化成恒等映射 ⟹ `prevViewProj` / 重投影那条路**一次都没被走到**——而用户报的
// 症状恰好落在那条路上（相机动 = 每帧 view 变）。spin 那套量的是「静止机位 + 动物」
// （= 债-49 的鬼影仪器，另一件事，有价值但**不是本任务**），保留为 `--motion spin`。
//
// 为什么是**帧号纯函数**（`temporal.cpp` 头注释当年登记的那条「要做微动激励得先加按帧索引
// 驱动的运动」，本文件就是那个消费方）：候选与参考是**两个引擎进程**，dt 累积的每帧角随
// 帧时长漂 ⟹ 两边第 i 帧不是同一视点 ⟹ 对齐根本不存在。纯函数让「第 i 帧 = 哪个视点」与
// 运行速度无关；RPC 机位写入只引入**常数帧偏移**（引擎侧自动重定基，见 camera.cppm），
// 由下面的跨进程对齐吸收。
//
// 为什么是**绕 up 轴定角 yaw**（理由同 `temporal.cpp` 头注释）：刚体相机运动、像素位移与
// 深度无关（u′ = (u+tanθ)/(1−u·tanθ) 只依赖屏位不依赖深度），`prevViewProj` 重投影能
// **精确对齐**；平移 dolly 会让远处物体的单帧位移只剩近处的百分之几十，信号随深度衰减。
//
// ## 偏差这一个数为什么够用（以及它会把什么混起来）
//
// 参考是「理想 720p」。锯齿是偏离、模糊**也是**偏离，所以「偏差」= 总误差，方向正确；
// 但它把**机制**混在一起了（锯齿 vs 模糊都给非零偏差）。故梯度能量比是**共同主指标**：
//   梯度比 > 1 ⟹ 多出来的是**假边**（锯齿/振铃）；< 1 ⟹ 边缘被**摊平**（模糊）。
// 两个数一起读才出结论：「偏差降 + 梯度比降」= 用糊换掉了锯齿；「偏差不降 + 梯度比不降」
// = 什么都没发生（no-op）。判决句必须同时引用两者。
//
// ## 组（每组一个引擎进程；只差被测项）
//
//   M0  TAA 关 + 运动            → **地板**：无 AA 的单帧光栅，锯齿全在
//   M1  TAA 开 + 运动（交付态）  → **被测**（base_space=true = 引擎默认，交付口径）
//   M2  M1 + base_space=false    → 历史住**抖动空间**的对照（输出额外带 c_t ≤0.71px 位移
//        ⟹ 对参考有一个**系统性**错位的贡献；故附「按闭式 c_t 去抖」修正读数，见下）
//   M3  重投影消融 —— **按运动源选门**：
//        · camera 模式 = `reprojection=false`：prevViewProj 重投影**本身**被关（历史按同基
//          位置采样）⟹ 相机每帧动 ⟹ 历史错位 = 每帧像素位移 ⟹ 读数必须显著变差。
//          ⚠ 为什么不能用 `motion_compensation=false`：那条只关 **prevSpin 场景旋转补偿**，
//          相机运动、spin 关时它是恒等阵 ⟹ 关与不关**逐位相同** = 消融无效（上一版把 M3
//          定义成它，在相机激励下会退化成 M1 的复读机）。
//        · spin 模式 = `motion_compensation=false`（prevSpin 补偿关，历史按上一帧物体位置
//          累积）——spin 模式下这才是有效消融，旧口径原样保留。
//   M3x camera 模式专属对照 = `motion_compensation=false`：按上面的论证必须 ≈ M1
//        （恒等对照：证明「spin 补偿在相机运动下确实惰性」是**量出来的**不是断言的）
//   M4  TAA 开 + 静止            → 「有 AA 该长什么样」的参照（同配置、机位不动、热身 64）
//   REFm / REFs                  → 运动 / 静止各自的 2560×1440 超采样参考（TAA 关）
//   M1m / M4m                    → M1/M4 + `diag_reject=true`，连拍 **taa_out 附件**取
//        alpha = 逐像素「历史怎么了」码（0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 / 0.75 重投影
//        拒收 / 1 无历史）⟹ **历史被拒比例**。门只动 alpha（RGB 逐位 = M1/M4），
//        「窗棂残余闪烁」与「相机运动失效」两个问题一起判：若 M1 的边缘像素大多住在
//        「盒拒收」桶里，α 调多小都没用（历史根本没进混合）。
//
// ## 跨进程帧对齐（本判据的地基）
//
// `angle = 每帧角 × 帧号` 让「第 i 帧 = 哪个视点」可复现，但**绝对帧偏移**未知
// （RPC 登记落在第几帧由启动时序决定，headless 高速下差 1–3 帧）。做法：
//   ① 每组 burst 前后读 `Engine::frameCount()` 回显（自证 burst 连续、无掉帧）；
//   ② 逐候选帧在 `i ± window` 内按**全帧线性 L1** argmin 选参考帧，偏移取逐帧差的**中位数**；
//   ③ 护栏两条：argmin 余量（次优 vs 最优的相对差）+ 前/后半窗偏移是否一致（抓 burst 内掉帧）。
// 为什么用全帧 L1 而不是边缘带 L1：对齐要的是「视点是否一致」，全帧信息最多、与掩码无关；
// 用边缘带会让掩码的定义依赖对齐结果（循环）。
//
// ## M2 的去抖修正是**离散相位搜索**，不是自由参数
//
// 抖动相位 = `Halton(2,3)` 周期 16 ⟹ 第 i 帧的内容位移 `c_i` 只有 **16 个可能值**，
// 且整组共用一个未知常数相位偏移 p。于是「去抖」= 在 p ∈ [0,16) 里 argmin，
// **零自由参数**（连续亚像素搜索才是自由参数，那才是该拒绝的口径）。
// 同一套搜索也跑在 M1 上作**自证**：M1 若真在基空间，最优 p 的改善应当很小、且最优位移
// 接近 0（读数打印出来，不判）。
//
// ## 三条测量红线（违反即测量无效）
//
//  1. **统计一律在 sRGB 解码后的线性域**（`prepareLinear` 复用，不另写解码）。降采样也必须在
//     线性域做——luma 是 RGB 的线性泛函、INTER_AREA 是线性平均，两者可交换，故「先解线性求
//     luma 再 2×2 面积平均」与「先降 RGB 再求 luma」**严格相等**（这是本文件敢直接缩 luma 的依据）。
//  2. **分辨率钉死 1280×720**（参考组 2560×1440 后降回）且**回显实际帧尺寸**：偏差是逐像素
//     均值，换分辨率 = 换口径。参考组回显「渲染尺寸 + 统计尺寸」两个数。
//  3. **配置 = 交付配置**（`deliveryConfigDir`，env/skybox/IBL/AO/shadow 全开、deferred）。
//     合成 profile 上没有用户报的那些 1px 高亮细线（激励源），在它上面量到的「没效果」不算数。
//
// ⚠ **AO 带来的公共偏差地板（读跨组数字前必读）**：AO 是屏幕空间螺旋采样，1440p 参考的 AO
// 细节与 720p 候选**本来就不同**（半径按像素算），这部分偏差与 TAA 无关、**各组共用**。
// 它抬高所有组的绝对偏差，但 M0/M1/M3/M4 之间的**相对比较**不受影响（同一项加在两边）。
// 故本判据只出**比值**结论，不出「偏差绝对值 = 多少 LSB 可见」的绝对结论。
//
// ## 判决口径（写死，别事后调）
//
// R = devEdge(M1) / devEdge(M0)（边缘带上）：
//   R > 1.05 ⟹ 运动下 TAA **主动变差**（偏差比 TAA 关还大 + 梯度比在降 = 拖影。R>1 不是
//     「没作用」而是「反作用」，与 no-op 是两种结论，实测踩过：1.138 曾被旧口径读成 no-op）；
//   0.90 ≤ R ≤ 1.05 ⟹ 运动下 TAA **等于没抗锯齿**（no-op）；
//   R ≤ 0.70 且 dev(M1) 明显 > dev(M4) ⟹ **生效但受固有边界限制**（运动本身是误差源，
//     「运动下可用累积窗天然短」是固有代价，**这不是 bug**，要如实说）；
//   之间 ⟹ 部分生效，逐条说明。
// 参照读数 dev(M4)/dev(M0) = 「有 AA 该长什么样」与地板之比，它给出 R 的**可达下界**语义。
// **两种结论都要照实说**——本判据不是用来证明「有 bug」的。
// 支撑对照 = M3（消融）：它挂零 = 量具坏了（对齐/掩码/域坏掉时所有组会**一起**变成同一个
// 无意义的数，而「M1/M0 ≈ 1」看起来仍像一个结论），整张表作废，不用它下画质结论。
//
// ══════════════════════════════════════════════════════════════════════════════════════
// ## 批 13.9：【机位状态 = **相机移动**】亮度 × 梯度 二维分带（本命令 = 两把尺子里的**尺子 2**）
//
// ### 要判的假设（用户 2026-10-06）
//
// 「所有**移动**锯齿的来源都是 1px **高亮**直线；**普通边缘确实没锯齿**。」
//
// 昨天批 13.7c 给**静止机位**跨相位摆动加了同一套分带（判出「静止残余非高光专属」），但那条
// 只覆盖静止态 —— 「**移动** × 亮度 × 梯度」这一格至今是空的，用户的主张既没证实也没否证。
//
// ### 为什么这条尺子大概最贴用户的原话
//
// 用户说的是「**锯齿**」。本命令已经有「候选图离『没有锯齿也不糊的 720p 图』有多远」这个
// 定义级问题（参考 = 同机位序列 2560×1440 + TAA 关 → 线性域面积平均）。把它**逐像素化**并按
// 亮度×梯度分带，问的就是：那些「偏离理想 720p」的像素，是不是住在近削顶×高梯度那一格？
// 三张表：
//   * `dev(TAA 关)` = 无 AA 单帧光栅的锯齿本体（**主表**，用户肉眼看到的就是这个）
//   * `dev(TAA 开)` = 开 TAA 之后还剩的偏差
//   * `max(dev(开) − dev(关), 0)` = **正向超额** = 「开 TAA 让哪里**更**偏离真值」（R=1.14 这个
//     整体比值在二维上的展开；负的部分只报总量，不进分带 —— 分带要的是「超额住在哪」）
//
// ### ⚠ 分带尺子**必须来自参考图**（这条是昨天选择偏差的教训，与尺子 1 的口径差别就在这一点）
//
// 亮度档与梯度档都定在 **REFm（超采样那张）** 上，不定在候选上：参考定义了「这个像素**本来**
// 是什么」，用被测量自己定档 = 用被测量选区域（批 13.7c 已因这种口径翻过一次方向）。
// 具体：亮度轴 = REFm 显示域字节亮度的多帧均值（1440p 上求均值再 INTER_AREA 降到 720p 网格，
// 与偏差图同网格才谈得上逐像素分带）；梯度轴 = `sobelAbsGrad(REFm.luma.front())` 的 **≥p90**
// = **就是本命令已有的那条边缘带**（`--edge-pct 0.90`），一个阈值都不新拍。
//
// ### ⭐ 判决线（**2026-10-06 跑前写死**，防事后挑数；三分支全部命名）
//
// 记 A = 中间调×高梯度（普通几何边缘）、B = 近削顶×高梯度（假设的元凶）。
// 主表 = `dev(TAA 关)`。每格出**四数**：share / rate>τ / **计数份额** / **幅度份额**。
//
// * **分支①「支持」** ⟺ B 的**幅度份额** ≥ 30%（`kMovingSupportAmpShare`）
//   **或** 正向超额集中在 B：`rB ≥ 1.5 × rA`，`rX = rate_on(X)/rate_off(X)`
//   （1.5 = 跑前沿用的 `kBandSupportRatio`，不新拍）。
// * **分支②「否证」** ⟺ B 的幅度份额**与**计数份额**都** < 5%（`kMovingRefuteShare`）
//   **且** 超额不向 B 集中。
// * **分支③「分不出」** ⟺ 其余；出表处必须再报性质（三选一）：**阈值没分开**（附敏感性表）/
//   **两格本来同分布** / **反向分离**（超额集中在 A = 普通边缘，与主张方向相反）。
//
// 三分支由 `classifyMovingBand` 执行（`temporal_residual.cpp`，两把尺子共用同一份）。
// **幅度份额是本次新增的胜负手**：`frac>τ` 不加权，平坦中间调摆 1 LSB 与高光摆一大截各算一票，
// 而高光是对比度最高处、同样绝对摆动最看得见 ⟹「我一直看到高亮在闪」与「高亮只占残余 0.5%」
// 可同时为真。只出计数份额 = 未完成。
//
// ⚠ 本段**只出数 + 一句判决**，不产 Verdict、不进 rc：上面那三条判据（激励在场 / 对齐 / 消融）
//   的 rc 语义不许被新表污染。既有 A/B 表与判决逐字不动。


#include "temporal_motion.h"

#include "imgqa.h"
#include "temporal_residual.h"
#include "temporal_yaw_remap.h"

#include <algorithm>
#include <cmath>
#include <numeric>  // std::accumulate（阶跃主指标 AUC = Σ ghost(n)）
#include <cstdio>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace RGL::imgqa {
using namespace tmotion;  // 本族跨 TU 的类型 / 常量 / 算子（唯一出处 = 本文件的第一条 include）

namespace tmotion {

/// 本命令的七个上下文结构体（MotionGroups / MotionRow / MotionRoi / MotionMasks /
/// MotionAlign / MotionRows / MotionRatios）唯一出处 = `temporal_motion.h`。

/// 参数校验（原样搬出）。返回 nullptr = 已打 stderr，调用方 rc=2（先 stderr 后 rc，顺序不变）。
[[nodiscard]] const BistroCam* validateMotionArgs(const std::string& cameraName,
    const std::string& motion, int burst, double edgePercentile)
{
    const BistroCam* cam = nullptr;
    for (const auto& c : kBistroCams)
        if (cameraName == c.name)
            cam = &c;
    if (!cam) {
        std::fprintf(stderr, "机位不存在: %s（可选 %s）\n", cameraName.c_str(),
            bistroCamNames().c_str());
        return nullptr;
    }
    if (motion != "camera" && motion != "spin") {
        std::fprintf(stderr, "--motion 只认 camera / spin（收到 %s）\n", motion.c_str());
        return nullptr;
    }
    if (burst < 4)
        throw std::runtime_error("--burst 至少 4（对齐护栏要分前后半窗）");
    if (edgePercentile <= 0.0 || edgePercentile >= 1.0)
        throw std::runtime_error("--edge-pct 必须在 (0,1)，它是分位不是百分比");
    return cam;
}

/// 抬头回显（原样搬出）。⚠ 消融开关那段**默认全关时不打**——已发表读数的输出文本也要逐字不变。
void printMotionHeader(const std::string& cameraName, bool cameraMode, double angleDeg, int burst,
    int warm, int warmStill, double longFeedback, double clipMode)
{
    std::printf("=== temporal-motion：%s运动下 TAA 的抗锯齿效果（机位 %s）===\n",
        cameraMode ? "相机" : "场景 spin（相机静止）", cameraName.c_str());
    std::printf("  配置 = 交付 configs/（deferred + env/skybox/IBL/AO/shadow 全开，红线 3）\n");
    std::printf("  运动 = %s：angle = %.4f°/帧 × 帧号（纯函数），绕 up 轴 yaw\n",
        cameraMode ? "相机帧驱动 yaw（camera.lua frame_yaw，= 用户报障的运动源）"
                   : "场景帧驱动 spin（scene.lua spin_frame_angle，相机不动 = 债-49 鬼影仪器）",
        angleDeg);
    std::printf("  参考 = 同机位同帧序列 %dx%d + TAA 关 → 线性域面积平均降到 %dx%d\n",
        kRefSize[0], kRefSize[1], kSize[0], kSize[1]);
    std::printf("  连拍 = %d 帧（/shotburst 逐帧连续）；热身 = 运动组 %d 帧 / 静止组 %d 帧"
                "（空转不写盘；运动格**不适用**「≥6/α 收敛」——历史本来就在被内容变化刷新）\n",
        burst, warm, warmStill);
    if (cameraMode)
        std::printf("  M3 = reprojection=false（关 prevViewProj 重投影本身）；M3x = "
                    "motion_compensation=false（恒等对照，应 ≈ M1）\n\n");
    else
        std::printf("  M3 = motion_compensation=false（关 prevSpin 场景旋转补偿）\n"
                    "  ⚠ spin 转**场景**、相机不动 ⟹ prevViewProj 恒定 ⟹ 相机重投影路不被走到\n\n");
    // 批 13.11 消融开关回显（默认全关时**不打**——已发表读数的输出文本也要逐字不变）
    if (longFeedback > 0.0 || clipMode != 0.0) {
        const std::string longTxt =
            longFeedback > 0.0 ? std::to_string(longFeedback) : std::string("关（沿用 .12）");
        std::printf("  ⚠ 消融开关：长窗档 α（覆写 feedback）= %s · clip_mode = %g\n"
                    "     spin 下相机静止 ⟹ 长窗**全程生效** ⟹ 这就是 债-49 风险区（静止机位 +\n"
                    "     场景内运动 + 长窗）。跨臂读 dev(M1)/dev(M0)：关盒（臂 1）不显著变差\n"
                    "     = 邻域盒在鬼影上**没承重**，就不必为它付静止闪烁的代价。\n",
            longTxt.c_str(), clipMode);
    }

}

/// 采集十组（每组一个引擎进程；串行，单实例纪律）。原样搬出，规格派生关系一字未动
/// （m2/m3/m3x/m4/m1m/m4m 全由 m1 复制派生 ⟹ 消融开关一处生效全覆盖）。
MotionGroups collectMotionGroups(const std::string& base, const BistroCam& cam,
    const std::string& pre, int burst, bool cameraMode, double angle, int warm, int warmStill,
    double longFeedback, double clipMode, double clipSoft)
{
    // ── 采集（每组一个进程；串行，单实例纪律）────────────────────────────────
    std::printf("── 采集 ──\n");
    std::vector<std::unique_ptr<Group>> slots;
    auto run = [&](const std::string& name, const std::string& sub, const Spec& sp) -> Group& {
        slots.push_back(std::make_unique<Group>(
            collectGroup(base + "_" + sub, name, pre + sub, cam, sp, burst)));
        return *slots.back();
    };
    Spec m0;
    m0.aa = Aa::Off;
    m0.motionAngle = angle;
    m0.cameraMotion = cameraMode;
    m0.warm = warm;
    Spec m1 = m0;
    m1.aa = Aa::Taa;
    // 批 13.11 鬼影 × 裁剪消融：只给 **TAA 开**的组设（m2/m3/m3x/m4/m1m/m4m 全由 m1 复制派生
    // ⟹ 一处生效全覆盖）。TAA 关的组（M0/REFm/REFs）没有历史，臂对它们是空操作。
    // 默认（0 / 0）= 不覆写不写键 = 交付态 = 已发表 M0…M4 读数逐位不变。
    // 长窗档 = **覆写 feedback**（批 13.14b 删 static boost 后 α 恒 = feedback，没有第二个旋钮）。
    if (longFeedback > 0.0)
        m1.feedback = longFeedback;
    m1.clipMode = clipMode;
    m1.clipSoft = clipSoft;
    Spec m2 = m1;
    m2.baseSpace = false;
    Spec m3 = m1;
    if (cameraMode)
        m3.reprojection = false;  // 相机模式：关 prevViewProj 重投影本身
    else
        m3.motionCompensation = false;  // spin 模式：关 prevSpin 场景旋转补偿
    Spec m3x = m1;
    m3x.motionCompensation = false;  // 恒等对照（仅相机模式）：按论证必须 ≈ M1
    Spec m4 = m1;
    m4.motionAngle = 0.0;
    m4.warm = warmStill;
    Spec refm = m0;
    refm.refScale = 1;
    refm.wantBytes = true;  // 批 13.9：分带尺子的**亮度轴**只能来自参考图（理由见文件头）
    Spec refs = m4;
    refs.aa = Aa::Off;
    refs.refScale = 1;
    Spec m1m = m1;
    m1m.diagReject = true;
    m1m.attachment = "taa_out";
    Spec m4m = m4;
    m4m.diagReject = true;
    m4m.attachment = "taa_out";

    Group& gM0 = run("M0 TAA关 + 运动", "m0", m0);
    Group& gM1 = run("M1 TAA开 + 运动(交付态)", "m1", m1);
    Group& gM2 = run("M2 M1 + base_space=false", "m2", m2);
    Group& gM3 = run(cameraMode ? "M3 M1 + 关重投影" : "M3 M1 + 关场景补偿", "m3", m3);
    std::unique_ptr<Group> gM3xHolder;
    Group* gM3x = nullptr;
    if (cameraMode) {
        gM3xHolder = std::make_unique<Group>(run("M3x M1 + 关spin补偿(恒等对照)", "m3x", m3x));
        gM3x = gM3xHolder.get();
    }
    Group& gM4 = run("M4 TAA开 + 静止", "m4", m4);
    Group& gRefm = run("REFm 超采样参考(运动)", "refm", refm);
    Group& gRefs = run("REFs 超采样参考(静止)", "refs", refs);
    Group& gM1m = run("M1m M1 + 拒收掩膜", "m1m", m1m);
    Group& gM4m = run("M4m M4 + 拒收掩膜", "m4m", m4m);
    MotionGroups out;
    out.slots = std::move(slots);
    out.m3xHolder = std::move(gM3xHolder);
    out.m0 = &gM0; out.m1 = &gM1; out.m2 = &gM2; out.m3 = &gM3; out.m4 = &gM4;
    out.refm = &gRefm; out.refs = &gRefs; out.m1m = &gM1m; out.m4m = &gM4m;
    out.m3x = gM3x;
    return out;
}

/// 守卫 1：运动激励在场（`temporal-motion-excitation`）。红 ⟹ 调用方落账后 rc=2 停下
/// （原实现就是「先打停下提示、再 logCall、再 return 2」，顺序保持）。
bool guardMotionExcitation(const MotionGroups& g, std::vector<Verdict>& vs)
{
    Group &gM0 = *g.m0, &gRefm = *g.refm;
    // ── 守卫 1：运动激励在场（`temporal-motion-excitation`）───────────────────
    // frame_yaw / spin_frame_angle 的键名拼错在 lua 侧**静默**（未知键不报错）⟹ 各组会
    // 全跑在静止场景上，而 M0≈M1≈M4 看起来完全像一个「TAA 无效」的结论。唯一能抓它的是
    // 「画面到底动没动」——探针必须选 **TAA 关**的组（M0/REFm）：TAA 开的组即使画面静止，
    // 抖动本身就让相邻帧不同，byteMaxAdjacent > 0 是恒真的（阳性锚点必须落在
    // 「被测机制从未生效时这条会绿吗」= 不会 的组上）。
    std::printf("\n── 守卫 ──\n");
    {
        std::string obs;
        for (const auto* g : {&gM0, &gRefm})
            obs += g->name.substr(0, 2) + " 相邻帧最大字节差 " + std::to_string(
                       static_cast<int>(g->byteMaxAdjacent)) + "  ";
        vs.push_back(verdictFor("temporal-motion-excitation",
            gM0.byteMaxAdjacent > 0.0 && gRefm.byteMaxAdjacent > 0.0,
            obs + "（M0/REFm = TAA 关组，必须 > 0；= 0 ⟺ frame_yaw/spin_frame_angle 没生效）"));
        std::cout << "  " << vs.back().line() << "\n";
        if (!vs.back().ok) {
            std::printf("  ⟹ 立刻停下：运动激励不在场，整张 A/B 表会跑在静止场景上。\n");
            return false;
        }
    }
    return vs.back().ok;
}

/// 历史拒收掩膜（`taa.diag_reject` → taa_out alpha；RGB 逐位 = M1/M4）。原样搬出。
MotionMasks reportMotionMasks(const MotionGroups& g, double clipMode)
{
    Group &gM1m = *g.m1m, &gM4m = *g.m4m;
    MotionMasks out;
    // ── 历史拒收掩膜（`taa.diag_reject` → taa_out alpha；RGB 逐位 = M1/M4）────
    // ⚠ **alpha 载体冲突（债-55 型事故：读数在，读的是别的量）**：臂 6 / 臂 7 把 taa_out 的 alpha
    //   征用成自己的时域状态载体（`taa.frag.slang` 里 `alpha = trust` / `alpha = mem`）⟹ 拒收码表
    //   （0/0.25/0.5/0.75/1）读出来是**垃圾**：mem≈0.25 的像素会被算成「盒裁剪」。这不是精度问题
    //   而是**读错量**，故整段跳过并出声——静默打出一个像模像样的百分比才是最贵的假数据。
    out.clipModeInt = static_cast<int>(clipMode + 0.5);
    // ⚠ **判据与 shader 侧 `noHistoryAlpha` 同步写成 `>= 6`（开区间上界）**：闭区间 `[6, N]` 在
    //   每加一个新臂时都会漏扩一次，而两侧漏扩的代价不对称——shader 侧漏扩臂 10 实测过：出画像素
    //   写满记忆 ⟹ 下一帧 mem 顶满 ⟹ 历史全收（`git:调试历史` 2026-10-06）。这里漏扩则是
    //   「读的是别的量却打出像模像样的百分比」= 假数据。alpha 自臂 6 起就是这些臂共用的载体。
    out.alphaOccupied = (out.clipModeInt >= 6);
    std::printf("\n── 历史拒收掩膜（taa_out alpha；0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 / "
                "0.75 重投影拒收 / 1 无历史）──\n");
    out.rsM1 = rejectStats(gM1m);
    out.rsM4 = rejectStats(gM4m);
    auto echoMask = [&](const char* tag, const RejectStats& s) {
        std::printf("  %-3s 收下 %s · 盒裁剪 %s · **盒拒收 %s** · 重投影拒收 %s · 无历史 %s"
                    "  ⟹ 历史被拒合计 %s\n",
            tag, pct2(s.accepted).c_str(), pct2(s.clipped).c_str(), pct2(s.collapsed).c_str(),
            pct2(s.reproject).c_str(), pct2(s.noHistory).c_str(), pct2(s.rejected()).c_str());
    };
    if (out.alphaOccupied) {
        std::printf("  **本臂跳过**：clip_mode = %d 把 alpha 征用成时域状态载体 ⟹ 拒收码表读出来"
                    "是垃圾（读了就是读错量）\n", out.clipModeInt);
    } else {
        assertMaskAlive("M1m", out.rsM1);
        assertMaskAlive("M4m", out.rsM4);
        echoMask("M1", out.rsM1);
        echoMask("M4", out.rsM4);
        std::printf("  ⚠ 读法：M1 的边缘像素若大多住在「盒拒收」桶 ⟹ 历史根本没进混合，"
                    "α（= feedback）调多小都没用——「窗棂残余」与「相机运动失效」同判\n");
    }
    return out;
}

/// 区域与阈值（口径来自 REF，不来自候选 ⟹ 各候选共用一把尺子）。原样搬出。
MotionRoi measureMotionRoi(const MotionGroups& g, double edgePercentile)
{
    Group& gRefm = *g.refm;
    MotionRoi out;
    // ── 区域与阈值（口径来自 REF，不来自候选 ⟹ 各候选共用一把尺子）──────────
    // 边缘带 = 参考线性 luma 梯度 ≥ p90（top 10%）。为什么用分位而不是绝对阈值：
    // 绝对阈值是「拍出来的数」，而 bistro 的梯度跨机位跨曝光量级；p90 让「边缘带」=
    // 「这个画面里梯度最高的那 10% 像素」，与场景亮度尺度无关（同 phase13 §5.1b 口径）。
    // 为什么在**降采样后的参考**上算：候选住在 720p 网格，边缘必须在同一网格上定义。
    cv::Mat gx, gy;
    cv::Mat& grad = out.grad;
    cv::Sobel(gRefm.luma.front(), gx, CV_64F, 1, 0, 1);
    cv::Sobel(gRefm.luma.front(), gy, CV_64F, 0, 1, 1);
    grad = cv::abs(gx) + cv::abs(gy);  // 与 gradEnergy 同式（同一算子只许一份实现）
    auto gv = valuesInMask(grad, {});
    std::sort(gv.begin(), gv.end());
    out.tauEdge = percentileOf(gv, edgePercentile);
    const double tauFlat = percentileOf(gv, 0.50);
    cv::Mat& edgeMask = out.edgeMask;
    cv::Mat& flatMask = out.flatMask;
    cv::compare(grad, out.tauEdge, edgeMask, cv::CMP_GE);
    cv::compare(grad, tauFlat, flatMask, cv::CMP_LT);

    // τ（可见性阈值）= 参考白电平处 1 个显示 LSB 的线性跨度（闭式，零自由参数）。
    auto rv = valuesInMask(gRefm.luma.front(), edgeMask);
    std::sort(rv.begin(), rv.end());
    out.white = percentileOf(rv, 0.99);
    out.tau = lsbLinearAtLuma(out.white);

    std::printf("\n── 区域与阈值 ──\n");
    std::printf("  边缘带 = 参考梯度 ≥ p%.0f（%lld px，占 %.1f%%）；平坦带 = 梯度 ≤ p50\n",
        edgePercentile * 100.0, static_cast<long long>(cv::countNonZero(edgeMask)),
        100.0 * cv::countNonZero(edgeMask) / static_cast<double>(grad.total()));
    std::printf("  τ = 白电平(p99=%.5f) 处 1 显示 LSB = %s（闭式 lsbLinearAtLuma，非拍的阈值）\n",
        out.white, d4(out.tau).c_str());
    std::printf("  ⚠ AO 是屏幕空间采样 ⟹ 1440p 参考与 720p 候选的 AO 细节本就不同 ⟹ 各组共用"
                "一个偏差地板，**只出比值结论**\n");
    return out;
}

/// 逐组跨进程对齐 + 守卫 `temporal-motion-alignment`（原样搬出，M3 例外那条红线也在里面）。
MotionAlign alignMotionGroups(const MotionGroups& g, bool cameraMode, std::vector<Verdict>& vs)
{
    Group &gM0 = *g.m0, &gM1 = *g.m1, &gM2 = *g.m2, &gM3 = *g.m3, &gM4 = *g.m4;
    Group &gRefm = *g.refm, &gRefs = *g.refs;
    Group* gM3x = g.m3x;  // 仅相机模式非空（M3x 恒等对照）
    MotionAlign al;
    // ── 对齐 ────────────────────────────────────────────────────────────────
    Alignment &aM0 = al.m0, &aM1 = al.m1, &aM2 = al.m2, &aM3 = al.m3, &aM3x = al.m3x, &aM4 = al.m4;
    const int window = 8;
    aM0 = alignSequence(gM0, gRefm, window, true);
    aM1 = alignSequence(gM1, gRefm, window, true);
    aM2 = alignSequence(gM2, gRefm, window, true);
    aM3 = alignSequence(gM3, gRefm, window, true);
    aM3x = cameraMode ? alignSequence(*gM3x, gRefm, window, true)
                                : alignSequence(gM4, gRefs, window, false);
    aM4 = alignSequence(gM4, gRefs, window, false);

    std::printf("\n── 跨进程帧对齐（窗口 ±%d）──\n", window);
    auto echoAlign = [&](const char* tag, const Alignment& a, bool moving) {
        if (!moving) {
            std::printf("  %-4s 不跑 argmin：静止组所有参考帧逐位相同 ⟹ 代价面恒平坦，"
                        "配对无信息（判它 = 判一个没有信息的量）\n", tag);
            return;
        }
        std::printf("  %-4s 偏移 %+d  argmin 余量 %.4f  前后半窗差 %d\n", tag, a.offset,
            a.minMargin, a.halfGap);
    };
    echoAlign("M0", aM0, true);
    echoAlign("M1", aM1, true);
    echoAlign("M2", aM2, true);
    echoAlign("M3", aM3, true);
    if (cameraMode)
        echoAlign("M3x", aM3x, true);
    echoAlign("M4", aM4, false);
    {
        // 🔴 **M3 不进最小值**（2026-10-07 实测定位，别再改回去）。M3 = 关重投影，是按设计
        // **故意弄坏**的消融组。记忆臂（11/13）在它下面 `motion ≡ 0`：`ecs_impl.cppm` 的消融门把
        // prevViewProj 换成本帧抖动后矩阵 + 位移清零 ⟹ `prevUvBase ≡ baseUv` ⟹ `gate ≡ 1`
        // ⟹ 历史无界累积成多视点糊 ⟹ 代价面**恒平坦**（实测余量 0.0001~0.0004、前后半窗差 1）。
        // 把它算进「配对是否有效」的最小值 = 用「被测链路坏掉时应当坏」的那一组去否决
        // 「被测链路是否可信」——**判据把有效性检查套在了按设计就该无效的组上**。
        // 实测判据：同角度同激励下臂 0 的 M3 余量 0.29（无记忆 ⟹ 不糊）而臂 11/13 塌到 0.0002，
        // 红的一直只有 M3，M0/M1/M2/M3x 全部健康（0.16~3.7）。
        // M3 怎么塌的是**发现**（门在关重投影时失明），单独打印，不参与否决。
        double worst = std::min({aM0.minMargin, aM1.minMargin, aM2.minMargin});
        int gap = std::max({aM0.halfGap, aM1.halfGap, aM2.halfGap});
        if (cameraMode) {
            worst = std::min(worst, aM3x.minMargin);
            gap = std::max(gap, aM3x.halfGap);
        }
        const bool m3Blind = aM3.minMargin < 0.02 || aM3.halfGap != 0;
        vs.push_back(verdictFor("temporal-motion-alignment", worst >= 0.02 && gap == 0,
               "最小 argmin 余量 " + d4(worst) + "（下限 0.02；**M3 除外**，理由见判据契约）· "
                   "最大前后半窗差 " + std::to_string(gap) + "（要求 0）· 偏移 M0/M1/M2/M3 = "
                   + std::to_string(aM0.offset) + "/" + std::to_string(aM1.offset) + "/"
                   + std::to_string(aM2.offset) + "/" + std::to_string(aM3.offset)
                   + (m3Blind
                           ? "\n       ⚠ M3（关重投影消融）余量 " + d4(aM3.minMargin)
                             + " 已塌 = 记忆臂在此配置下**门失明**、历史糊成多视点。这是发现不是故障，"
                               "但它意味着下一条 ablation 锚点由错位撑大"
                           : "")));
        std::cout << "  " << vs.back().line() << "\n";
    }
    return al;
}

/// M2 去抖 + M1 自证 + 逐组偏差读数 + A/B 表（原样搬出，打印顺序一字未动）。
MotionRows measureMotionRows(const MotionGroups& g, const MotionAlign& al, const MotionRoi& roi,
    bool cameraMode)
{
    Group &gM0 = *g.m0, &gM1 = *g.m1, &gM2 = *g.m2, &gM3 = *g.m3, &gM4 = *g.m4;
    Group &gRefm = *g.refm, &gRefs = *g.refs;
    Group* gM3x = g.m3x;  // 仅相机模式非空（M3x 恒等对照）
    const Alignment &aM0 = al.m0, &aM1 = al.m1, &aM2 = al.m2, &aM3 = al.m3, &aM3x = al.m3x, &aM4 = al.m4;
    const cv::Mat& edgeMask = roi.edgeMask;
    const cv::Mat& flatMask = roi.flatMask;
    const double tau = roi.tau;
    MotionRows out;
    // ── M2 去抖 + M1 自证（离散 16 相位 argmin，零自由参数）────────────────────
    PhaseFit fM2 = fitJitterPhase(gM2, gRefm, aM2, edgeMask, 1.0);
    PhaseFit fM1 = fitJitterPhase(gM1, gRefm, aM1, edgeMask, 1.0);
    std::printf("\n── 抖动相位拟合（p ∈ [0,16) argmin）──\n");
    std::printf("  M2 不去抖 %s → 最优 p=%2d 去抖后 %s（赚 %.1f%%）\n", d4(fM2.rawDev).c_str(),
        fM2.bestPhase, d4(fM2.bestDev).c_str(), 100.0 * (1.0 - fM2.bestDev / fM2.rawDev));
    std::printf("  M1 不去抖 %s → 最优 p=%2d 去抖后 %s（赚 %.1f%%）"
                "  ← 交付态应接近 0（base_space=true 已把输出搬回基网格）\n",
        d4(fM1.rawDev).c_str(), fM1.bestPhase, d4(fM1.bestDev).c_str(),
        100.0 * (1.0 - fM1.bestDev / fM1.rawDev));

    // ── 读数 ────────────────────────────────────────────────────────────────
    auto measure = [&](const std::vector<cv::Mat>& seq, const Group& ref, const Alignment& a) {
        return measureDeviation(seq, ref, a, edgeMask, flatMask, tau);
    };
    auto& rows = out.rows;
    rows.push_back(MotionRow{"M0", "TAA 关 + 运动（地板）", measure(gM0.luma, gRefm, aM0), ""});
    rows.push_back(MotionRow{"M1", "TAA 开 + 运动（交付态）", measure(gM1.luma, gRefm, aM1), ""});
    rows.push_back(MotionRow{"M2", "M1 + base_space=false",
        measure(fM2.warped.empty() ? gM2.luma : fM2.warped, gRefm, aM2),
        "去抖修正（p=" + std::to_string(fM2.bestPhase) + "）"});
    rows.push_back(MotionRow{"M3",
        cameraMode ? "M1 + 关重投影（消融）" : "M1 + 关场景补偿（消融）",
        measure(gM3.luma, gRefm, aM3), ""});
    rows.push_back(MotionRow{"M4", "TAA 开 + 静止（参照）", measure(gM4.luma, gRefs, aM4), "对 REFs"});
    rows.push_back(
        MotionRow{"M2x", "M2 未去抖（对照）", measure(gM2.luma, gRefm, aM2), "含 ≤0.71px 系统位移"});
    if (cameraMode)
        rows.push_back(MotionRow{"M3x", "M1 + 关spin补偿（恒等对照）",
            measure(gM3x->luma, gRefm, aM3x), "按论证应 ≈ M1"});

    out.devM0 = rows[0].dev.devEdge;
    out.devM1 = rows[1].dev.devEdge;
    out.devM3 = rows[3].dev.devEdge;
    out.devM4 = rows[4].dev.devEdge;
    out.devM3x = cameraMode ? rows[6].dev.devEdge : 0.0;

    std::printf("\n── A/B 表（线性域；边缘带 = 梯度 top10%%）──\n");
    std::printf("  %-4s %-28s %10s %8s %9s %10s %8s  %s\n", "组", "配置", "边缘偏差",
        "(÷LSB)", "可见边缘%", "平坦偏差", "梯度比", "备注");
    for (const auto& r : rows) {
        std::printf("  %-4s %-28s %10s %8s %8.2f%% %10s %8.3f  %s\n", r.tag, r.desc,
            d4(r.dev.devEdge).c_str(), d2(r.dev.devEdge / tau).c_str(),
            r.dev.fracVisible * 100.0, d4(r.dev.devFlat).c_str(), r.dev.gradRatio,
            r.note.c_str());
    }
    std::printf("  （边缘像素 %lld；平坦带同 τ，读的是「非边缘有没有被牵连」= 公共偏差地板）\n",
        static_cast<long long>(rows[0].dev.edgePixels));
    return out;
}

/// 守卫 2：消融组必须显著变差（量具敏感性阳性锚点）。原样搬出。
void guardMotionAblation(const MotionAlign& al, const MotionRows& rows,
    std::vector<Verdict>& vs)
{
    const Alignment& aM3 = al.m3;
    const double devM3 = rows.devM3, devM1 = rows.devM1;
    // ── 守卫 2：消融组必须显著变差（量具敏感性阳性锚点）───────────────────────
    {
        const double ratio = devM3 / std::max(devM1, 1e-12);
        // ⚠ **空洞满足风险**（与上一条的 M3 例外成对，别只改一边）：M3 对不齐时 dev(M3) 被
        //   **错位**撑大 ⟹ `ratio ≥ 1.5` 可以靠错位满足，而不是靠「关掉了搬运历史的那条路」。
        //   所以 M3 余量低于对齐地板时，本锚点**只算「读数动了」（必要条件仍在），不算阳性锚点**。
        //   彻底修法 = 用 M1 的偏移**强制**测 M3 的 dev（两边同偏移 ⟹ 错位项相消），→ 债-70。
        const bool m3Blind = aM3.minMargin < 0.02 || aM3.halfGap != 0;
        vs.push_back(verdictFor("temporal-motion-ablation", ratio >= 1.5,
            "dev(M3 关重投影)/dev(M1) = " + d2(ratio) + "（下限 1.5）· dev M3 = " + d4(devM3)
                + "，M1 = " + d4(devM1)
                + "；挂零 = 对齐/掩码/域坏了，整张表作废，不用它下画质结论"
                + (m3Blind
                        ? "\n       ⚠ M3 余量低于对齐地板 ⟹ 本比值由**错位**撑大，只算「读数动了」"
                          "**不算阳性锚点**（彻底修法 = 强制同偏移测 dev，→ 债-70）"
                        : "")));
        std::cout << "  " << vs.back().line() << "\n";
    }
}

/// 判决段（原样搬出）。返回两个比值供落账。
MotionRatios printMotionVerdict(const MotionRows& rows, const MotionMasks& masks,
    bool cameraMode)
{
    const double devM0 = rows.devM0, devM1 = rows.devM1, devM3 = rows.devM3, devM4 = rows.devM4,
                 devM3x = rows.devM3x;
    const std::vector<MotionRow>& rows_of = rows.rows;
    const RejectStats &rsM1 = masks.rsM1, &rsM4 = masks.rsM4;
    const bool alphaOccupied = masks.alphaOccupied;
    const int clipModeInt = masks.clipModeInt;
    MotionRatios out;
    // ── 判决 ────────────────────────────────────────────────────────────────
    const double R = devM1 / std::max(devM0, 1e-12);
    out.R = R;
    const double Rbest = devM4 / std::max(devM0, 1e-12);
    out.Rbest = Rbest;
    const double GR0 = rows_of[0].dev.gradRatio, GR1 = rows_of[1].dev.gradRatio;
    std::printf("\n── 判决 ──\n");
    std::printf("  R = dev(M1)/dev(M0) = %s（判决线：>1.05 主动变差 · 0.90–1.05 no-op · "
                "≤0.70 生效 · 之间部分生效）\n",
        d2(R).c_str());
    std::printf("  参照 dev(M4)/dev(M0) = %s（静止收敛态 = 运动态的**可达下界**语义，非等号）\n",
        d2(Rbest).c_str());
    std::printf("  梯度比 M0 %.3f · M1 %.3f · M4 %.3f（>1 = 多假边/锯齿；<1 = 边缘被摊平）\n",
        GR0, GR1, rows_of[4].dev.gradRatio);
    std::string verdict;
    if (R > 1.05)
        // R > 1 不是「没作用」而是「反作用」：偏差比 TAA 关还大，同时梯度比在降（= 变糊）。
        // 两者同向 ⟹ 时域历史把**上一帧的位置**抹进了本帧（拖影），既没对齐参考也没消锯齿。
        verdict = "运动下 TAA **主动变差**（不是 no-op）：边缘偏差是 TAA 关的 " + d2(R)
            + "×（更偏离超采样参考），而梯度比 " + d2(GR0) + " → " + d2(GR1)
            + "（确实在变糊）——历史把上一帧的位置抹进本帧 = **拖影**，糊掉锯齿的同时"
              "引入了更大的位置误差";
    else if (R >= 0.90)
        verdict = "运动下 TAA **等于没抗锯齿**（no-op）：边缘偏差与 TAA 关之比 " + d2(R)
            + "，梯度比也几乎没动（" + d2(GR0) + " → " + d2(GR1) + "）——不是「变差了」，是"
              "**什么都没发生**";
    else if (R <= 0.70)
        verdict = "运动下 TAA **确实生效但受固有边界限制**：边缘偏差降到地板的 " + d2(R)
            + "×，但仍显著高于静止收敛参照（M1 " + d4(devM1) + " vs M4 " + d4(devM4)
            + "）——运动本身是误差源（可用累积窗被内容变化刷新 = 天然短），"
              "**这不是 bug，是固有代价**";
    else
        verdict = "运动下 TAA **部分生效**：边缘偏差降到地板的 " + d2(R)
            + "×（判决线 0.70/0.90 之间），需按梯度比与 M4 参照逐条解读，不能一句话定性";
    std::printf("\n  ★ %s\n", verdict.c_str());
    // 消融对照的结论**由比值算出来**，不写死（写死过一次，读起来像"永远起作用"）。
    const double ablationRatio = devM3 / std::max(devM1, 1e-12);
    std::printf("  ★ 支撑对照：M3（关重投影）= %s，M3/M1 = %s ⟹ 重投影链路**%s**"
                "（阈值 1.20：关掉后偏差没涨 = 链路没在读数里起作用）\n",
        d4(devM3).c_str(), d2(ablationRatio).c_str(),
        ablationRatio > 1.20 ? "确实在读数里起作用" : "**没**起作用");
    if (cameraMode)
        std::printf("  ★ 恒等对照：M3x（关 spin 补偿）= %s，M3x/M1 = %s（按论证应 ≈ 1.00："
                    "spin 补偿矩阵在相机运动下是恒等阵）\n",
            d4(devM3x).c_str(), d2(devM3x / std::max(devM1, 1e-12)).c_str());
    if (!alphaOccupied)
        std::printf("  ★ 历史被拒合计：M1 %s · M4 %s（掩膜 = taa_out alpha，RGB 逐位 = M1/M4；"
                    "「拒」= 盒拒收 + 重投影出画，**不含**盒裁剪 M1 %s / M4 %s = 部分保留）\n",
            pct2(rsM1.rejected()).c_str(), pct2(rsM4.rejected()).c_str(),
            pct2(rsM1.clipped).c_str(), pct2(rsM4.clipped).c_str());
    else
        std::printf("  ★ 历史被拒合计：**本臂不可读**（alpha 被 clip_mode = %d 征用，见上方掩膜段）\n",
            clipModeInt);
    std::printf("  ★ 口径边界：AO 的 1440p/720p 差异是各组共用的偏差地板，只出比值结论；"
                "spin 模式（--motion spin）另量「静止机位 + 动物」，两者不可互推\n");
    return out;
}

/// 批 13.9 移动机位分带（**只出数 + 一句判决**，不产 Verdict、不进 rc）。原样搬出。
std::string reportMotionBands(const MotionGroups& g, const MotionAlign& al, const MotionRoi& roi,
    const std::string& cameraName)
{
    Group &gM0 = *g.m0, &gM1 = *g.m1, &gRefm = *g.refm;
    const Alignment &aM0 = al.m0, &aM1 = al.m1;
    const cv::Mat& grad = roi.grad;
    const double tauEdge = roi.tauEdge, tau = roi.tau;
    // ── 批 13.9：【机位状态 = 相机移动】亮度 × 梯度 二维分带（**只出数 + 一句判决**）──────
    // 不产 Verdict、不进 rc：上面三条判据（激励在场 / 对齐 / 消融）的 rc 语义不许被新表污染。
    std::string bandVerdict = "未出";
    {
        // 差图的两边各自按**自己组**的对齐配对（与上面 devEdge 的口径一致）。两侧配对若不同，
        // 差图里就掺进一个跨配对项 ⟹ 必须回显有多少帧配到了不同的参考帧（自曝，不藏）。
        int pairMismatch = 0;
        for (size_t i = 0; i < aM0.pairIndex.size() && i < aM1.pairIndex.size(); ++i)
            pairMismatch += aM0.pairIndex[i] == aM1.pairIndex[i] ? 0 : 1;
        std::printf("  ★ 分带用图：dev(关) 配对偏移 %+d、dev(开) 配对偏移 %+d，逐帧配对不同的有 "
                    "%d/%zu 帧（差图里掺的跨配对项就来自这些帧）\n",
            aM0.offset, aM1.offset, pairMismatch, aM0.pairIndex.size());
        const cv::Mat devOffMap = movingDevMap(gM0.luma, gRefm, aM0);
        const cv::Mat devOnMap = movingDevMap(gM1.luma, gRefm, aM1);
        bandVerdict = reportMovingBandsAgainstReference(gRefm, grad, tauEdge, tau, devOffMap,
            devOnMap, cameraName);
    }
    return bandVerdict;
}

}  // namespace tmotion

/// `temporal-motion` 入口：校验 → 抬头 → 采集 → 守卫 → 读数 → 判决 → 分带 → 落账。
/// 批次 B11 拆小（原 357 代码行 / 认知 56）：各段搬进 `collectMotionGroups` /
/// `guardMotionExcitation` / `reportMotionMasks` / `measureMotionRoi` / `alignMotionGroups` /
/// `measureMotionRows` / `guardMotionAblation` / `printMotionVerdict` / `reportMotionBands`。
/// ⚠ 本函数里的**调用顺序 = 原实现从上到下的顺序 = 打印与落账顺序**，一字不许换。
int cmdTemporalMotion(const std::string& cameraName, const std::string& motion,
    double angleDeg, int burst, int warm, int warmStill, double edgePercentile,
    double longFeedback, double clipMode, double clipSoft)
{
    const BistroCam* const cam = validateMotionArgs(cameraName, motion, burst, edgePercentile);
    if (!cam)
        return 2;

    const bool cameraMode = motion == "camera";
    const double angle = angleDeg * 3.14159265358979323846 / 180.0;
    const std::string base =
        projectRoot() + "/build/imgqa_motion_" + (cameraMode ? "cam_" : "spin_") + cameraName;
    const std::string pre = cameraName.substr(0, 2) + (cameraMode ? "c_" : "s_");

    printMotionHeader(cameraName, cameraMode, angleDeg, burst, warm, warmStill, longFeedback,
        clipMode);
    const MotionGroups g = collectMotionGroups(base, *cam, pre, burst, cameraMode, angle, warm,
        warmStill, longFeedback, clipMode, clipSoft);
    std::vector<Verdict> vs;
    if (!guardMotionExcitation(g, vs)) {
        logCall("temporal-motion",
            {{"camera", cameraName}, {"motion", motion}, {"angle", d4(angle)}}, vs);
        return 2;
    }
    const MotionMasks masks = reportMotionMasks(g, clipMode);
    const MotionRoi roi = measureMotionRoi(g, edgePercentile);
    const MotionAlign al = alignMotionGroups(g, cameraMode, vs);
    const MotionRows rows = measureMotionRows(g, al, roi, cameraMode);
    guardMotionAblation(al, rows, vs);
    const MotionRatios ratios = printMotionVerdict(rows, masks, cameraMode);
    const std::string bandVerdict = reportMotionBands(g, al, roi, cameraName);

    logCall("temporal-motion",
        {{"camera", cameraName}, {"motion", motion},
            {"angleDeg", std::to_string(angleDeg)}, {"burst", std::to_string(burst)},
            {"warm", std::to_string(warm)}, {"warmStill", std::to_string(warmStill)},
            {"edgePct", std::to_string(edgePercentile)}, {"R", d4(ratios.R)},
            {"R_still_ref", d4(ratios.Rbest)}, {"rejectM1", d4(masks.rsM1.rejected())},
            {"rejectM4", d4(masks.rsM4.rejected())},
            {"movingBandVerdict", bandVerdict}},
        vs);
    bool allOk = true;
    for (const auto& v : vs)
        allOk &= v.ok;
    return allOk ? 0 : 1;
}

}  // namespace RGL::imgqa
