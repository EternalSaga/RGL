#ifndef RGL_IMGQA_H
#define RGL_IMGQA_H
// imgqa —— 区域归因的图像判据工具箱（Phase 12 批 12.3，C++ 版）
//
// ## 为什么是 C++（而不是先写的 Python 版，那版已删）
//
// 本项目在**三台机器**上测试，工具的可迁移性是硬约束：所有依赖走 vcpkg manifest、
// 构建走 CMake、没有 Python 侧虚拟环境要在一台台机器上对齐。用户拍板的同款先例 =
// 模板引擎选 inja 不选 jinja。留在 Python 的工具只准 stdlib-only（qa_scenes.py /
// analytic_ref.py），因为那是唯一不依赖环境的形态；任何要图像算法的东西都该在 C++。
//
// ## 它不是"图像质量度量工具"
//
// "imgqa" 这名字暗示"拿两张图算一个数"。那条路在本项目**被物理堵死**：BIT-IDENTICAL
// 回归要求同会话双构建，跨日就有 ±1 LSB 叶缘噪声（git:调试历史 2026-09-08）。
// 本工具做的是另一件事：**判据的区域来自几何，不来自图像。** 引擎能给出逐像素
// objectID，于是问题从"图像变了吗"变成"对 piece X，落在它那些像素上的量 Q 是多少"。
// 失败信息直接点名"anchor_cube_near 的某簇像素线性亮度为 0，闭式解预测 0.239"，
// 而不是"SSIM 0.93"。
//
// ## 判据的可信度从哪来（一律 advisory，**没有晋升这回事**）
//
// 每条判据必须有：① 写得出**理由**的容差（不是"0.5%"，是"float32 多项式 + 8bit 取整，
// 实测 2 LSB"）；② 至少一条 guard 出处（钉住git:调试历史里的哪一次真实 bug 或真实变异）。
// 这两条是**对全部判据**的不变量，由 `imgqa selftest` 强制，`imgqa check` 打印的就是它们。
// 另有一份调用台账 qa/log/imgqa_calls.jsonl：哪条判据真在排查里被调用、事后看对不对，
// 每次真实调用后 `imgqa note` 回填事后真相。**为什么这里没有晋升机制** → docs/还债总账.md
// 债-08（一句话：晋升闸门只对 0 条生效，普适不变量对全部判据生效）。
//
// ## 三条硬约束（都来自踩过的坑）
//
// 1. **绝不许对"该机位下其实看不见的几何"下断言**——先由 objectID 反解可见集
//    （tools/qa_scenes.py 注释里付过学费："按预测边界定点 = 会绿 = 无效断言"）。
// 2. **统计一律在线性域做**；比值在 sRGB 字节域算出来是错的数。
// 3. **比较域必须是"观测域"**：观测像素是 曝光→ACES→sRGB store 之后的字节；闭式解
//    是 pre-tonemap 线性值。ACES 有通道串扰，两个域之间没有比例关系可谈——预测值
//    必须先走 toBytes 再 sRGB 解回来（predictFaces 里做），才能与像素同域比较。
//
// ## 运动态时域判据的两条方法学结论（批 13.11，`temporal-flicker` / `temporal-homography`）
//
// 1. **闭式解必须住在观测域**，且两端都要。相机以亚像素扫速扫过 1px 边缘时，
//    「理想 AA」= 覆盖线性斜坡、「完全无 AA」= 阶梯，两者的每帧变化都是闭式的
//    （`displayChain` 复用 toBytes/observedLuma）。只给上界不给下界的判据，读数没有刻度。
//    实机对拍：TAA 关的 ρ 实测与闭式无 AA 差 **0.07%**（aa_target 白板边缘，0.125px/帧）。
// 2. **「时间轴高频能量」不是闪烁的度量，别再试**。混叠是零阶保持，它把锯齿**变成方波**，
//    方波的高频能量并不比锯齿大；而亚像素相位 τ 一变等于占空比一变，同一信号的带比能在
//    [0.15, 0.60] 里乱跳，与无 AA 参考的 [0.00, 0.86] 完全重叠。2 次谐波比 A(2v)/A(v) 同理
//    （理想 [0.45,0.69]、无 AA [0,1]）。FFT 在本工具只作诊断打印，判据用「每帧变化 ÷ 闭式理想」。

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

namespace RGL::imgqa {

/// 工程根目录（编译期注入 RGL_SOURCE_DIR；= 找 build/rgl、configs、rgl 源码的位置）。
const std::string& projectRoot();

// ═══════════════════════════════════════════ 闭式解（analytic_ref.py 的 C++ 转录）
// 逐式转录 model_pbr.frag.slang + tonemap.frag.slang + 硬件 sRGB store。
// **独立第二实现**：不 import 引擎任何代码，与 shader 的对应关系写在行旁注释。
// 自检锚点 = 本机 RADV 实测（见 selftest.cpp），改 shader 着色链必须同步这里。

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
};
[[nodiscard]] Vec3 operator+(Vec3 a, Vec3 b);
[[nodiscard]] Vec3 operator-(Vec3 a, Vec3 b);
[[nodiscard]] Vec3 operator*(Vec3 a, double s);
[[nodiscard]] double dot(Vec3 a, Vec3 b);
[[nodiscard]] Vec3 normalize(Vec3 a);

/// 直接光 BRDF 出射（线性、未曝光、未 tonemap）。参数语义同 analytic_ref.shade_direct：
/// l = 片元→光源方向（= -light.direction 归一化）；lightColor 已含强度。
[[nodiscard]] Vec3 shadeDirect(Vec3 n, Vec3 v, Vec3 l, Vec3 albedo, double roughness,
    double metallic, Vec3 lightColor);

/// Narkowicz 2015 ACES 近似（输出仍线性显示域）。顺序同 shader：×曝光 → ACES → saturate。
[[nodiscard]] Vec3 acesTonemap(Vec3 x);

/// 硬件 linear→sRGB（swapchain *_SRGB store 自动做的那步）。
[[nodiscard]] double srgbEncode(double c);
/// 解析逆。所有比值/单调性判据先在解出来的线性域做。
[[nodiscard]] double srgbDecode(double c);

/// 线性 HDR → 屏幕字节（曝光 → ACES → sRGB → 8bit）。这是「该是多少」的最终答案。
[[nodiscard]] std::array<int, 3> toBytes(Vec3 linear, double exposure = 1.0);

/// 观测域亮度：字节 → sRGB 解码 → Rec.709 加权。**观测与预测必须用同一个函数**，
/// 这样"比较域"在代码里只有一个出处（文件头硬约束第 3 条）。
[[nodiscard]] double observedLuma(const std::array<int, 3>& byte);

struct FacePrediction {
    std::string face;               // "+X" … "-Z"
    double ndotl = 0.0;
    std::array<int, 3> byte{};      // 该面的预测屏幕字节
    double luma = 0.0;              // observedLuma(byte)，与像素同域
};

/// 六个轴对齐面的闭式解预测。V 用 normalize(camera - center)（探针几何刻意轴对齐，
/// N 就是六个单位轴——**这是探针场景设计的功劳，不是工具的聪明**）。
[[nodiscard]] std::vector<FacePrediction> predictFaces(std::array<double, 3> lightDir,
    std::array<double, 3> albedo, double roughness, double metallic, double intensity,
    std::array<double, 3> camera, std::array<double, 3> center, double exposure = 1.0);

// ═══════════════════════════════════════════ 配置目录生成 + 引擎生命周期

/// 生成一份配置目录（基底 = qa/configs/analytic/ 的 render_graph.lua 整份拷——
/// 它定义图本身，只给 {variant=...} 会得 0 个 swapchain 附件而启动失败，实测踩过）。
///
/// 两个真实陷阱在此收口：
///   ① lua 布尔必须小写——`{}` 直接格式化 C++ bool 若配错成 True 就是 nil 回落默认；
///   ② 键名对着**引擎解析器源码**校验（`shadowPass` 是 C++ 字段名，shadow.lua 的键是
///      `mode`——拼错键 = 静默用默认值 = 在默认值上下判据，拒绝）。
/// 抗锯齿三态（引擎 `runtime.lua` 的 `aa` 键，批 14.6）。imgqa 不链 rgl 库，同值重抄
/// （先例 = kTaaFeedbackLabel）；语义与"为什么是三态不是两个布尔" → `rgl/runtime_config.cppm`
/// 的 `AaMode`。三态**结构上排除了**「TAA 与 FSR 时域同开」——旧 `checkFsrOpts` 那条
/// 运行期守卫随之删除（写不出非法组合比拒绝非法组合更强）。
enum class Aa : std::uint8_t { Off = 0, Fsr, Taa };

struct ProfileOpts {
    /// **交付配置模式**（批 13.1e）：非空 = 以该目录（仓库 `configs/`）里的 *.lua / *.json
    /// 为基底**整份拷贝**，之后只覆写三处：`runtime.lua` 的 headless（+ aa/fsr/render_scale
    /// 等升频臂键）、`diagnostics.lua` 的 rpc、`scene.lua` 的 taa 块（feedback / jitter_scale
    /// / 各消融门；批 14.6 起**不含开关**——开关是 runtime 的 aa 三态）。其余一律不动。
    ///
    /// 为什么必须有这条：判据跑在**合成 profile** 上得出的绿不算绿。`temporal-identity`
    /// 就是这么假绿了一整轮——合成 profile + 默认机位上残差 0.016/255（绿），同一判据换到
    /// 交付配置 + 用户机位上是 **mad 1.688/255、逐像素最大 37、10.37% 像素 >2LSB**（红）。
    /// 差异不是噪声而是**另一个场景**：交付配置是 `variant = "deferred"` + `scale = 0.01`
    /// + izb_shadow + env/skybox/AO 全开，合成 profile 是 forward + 默认机位。
    /// ⚠ 本模式生效时 `glb` / `skybox` / `shadowPass` / `lightDir` / `lightIntensity` /
    ///   `exposure` **全部被忽略**（真相在交付配置里，工具再抄一份就是第二份真相）；
    ///   `variant` 留空 = 尊重交付配置自己写的路线。
    ///
    /// **例外**（`overridePicking` / `overrideEnvIntensity` / `overrideAoStrength` /
    /// `overrideShadowRoute` / `overrideLlmShot`）：这几件事不是"工具再抄一份真相"，而是
    /// **A/B 实验本身**（关 IBL / 关 AO / 换软影档）与**测量所需的观测通道或采集形态**
    /// （object_id 附件要 picking / 钉死帧号要 llm 截图）。默认所有门全关 = 一个键都不多写
    /// = 现有判据逐位不变；开了门才覆写，且覆写失败（键不存在）一律抛，**拒绝在旧值上下判据**。
    bool overridePicking = false;       ///< true = 把 `diagnostics.picking` 写成 `picking`
    bool overrideEnvIntensity = false;  ///< true = 把 `scene.lua` 的 `env.intensity` 写成 `envIntensity`
    bool overrideAoStrength = false;    ///< true = 把 `scene.lua` 的 `ao.strength` 写成 `aoStrength`
    /// **第 4 个例外**（批 N6d `shadow-softness`）：true = 覆写交付配置 `shadow.lua` 的
    /// **软影滤波轴**——`mode` ← `shadowRouteMode`、`izb` ← **false**、并按
    /// `shadowRouteSunDegrees` 写本档的 `sun_degrees`。
    ///
    /// 为什么 `izb` 必须被强制成 false（不是"顺手"）：IZB 是乘在软影之上的**光空间硬影 mask**，
    /// 开着它测半影等于让硬影吞掉软影——实测带核比 0.097（开）vs 0.185（关）（§4.1）。
    /// 交付形态（izb 开）的读数是 `izb` 命令的事，本判据测的是滤波轴本身。
    /// `izb` 在交付配置里是**子表形态**（`{ on = false, grid = 512, dump_frame = 0 }`），
    /// 引擎硬切后只收这一种（扁平布尔 = 启动报错）⟹ 覆写整值换成 `{ on = false }`；
    /// 覆写器两种文本跨度都吃（标量分支防旧形态残留），见 `overrideTopLevelValue`。
    bool overrideShadowRoute = false;
    std::string shadowRouteMode;  ///< "none"|"csm"|"pcss"|"evssm"（门开时必填，其余值抛）
    /// `shadowRouteSunDegrees` ≥ 0 = 把它写进**本档**的 `sun_degrees`（pcss 档写 pcss 块、
    /// evssm 档写 evssm 块——两档分立是消债-16⑤，写错块 = 静默拿出厂 0.53 当另一档的读数）。
    /// < 0 = **不写键** = 出厂值 0.53（`evssm` 默认臂要的就是这个：用户看到的就是出厂值）。
    /// 门开而 mode 是 none/csm 时给 ≥0 的值 = 抛（太阳角在两档下根本不进 shader，
    /// 写进去是"配置里写了、什么都没发生"）。
    double shadowRouteSunDegrees = -1.0;
    /// **第 5 个例外**（批 N6d `shadow-softness`）：true = 一次性截图形态——`runtime.lua` 的
    /// `llm_debug` ← true、`llm_shot.{file,size,delay_frames}` ← 给定值，并把 `diagnostics.rpc`
    /// ← false（引擎既有约定：llm 截图在跑时 `/shot` 返 409，两种采集形态互斥）。
    ///
    /// 为什么必须有这根线而不是走 RPC `Engine`：**帧号**。交付场景 `spin` 在转，同配置第 8 帧
    /// 与第 12 帧的影覆盖差 0.12pp（§4.1）⟹ 帧号是量具的一部分。RPC 的 `/shot` 落在哪一帧
    /// 取决于 HTTP 往返与帧首钩子的先后（= 墙钟）——单张没有锚，先等帧号再发也只有
    /// "大约第 N 帧"；而 `llm_shot.delay_frames` 是引擎在**第 N 帧渲染完成后**截图 = 精确
    /// 钉死，同配置两跑逐位相同（evssm_plan §6.1 的确定性实测就是这条形态）。
    /// ⚠ 债-89 后的口径：「帧号钉不死」只对**单张 `/shot`** 成立——连拍形态已有 RPC 侧锚
    /// （`/shotburst` 的 `start_frame`，见 `Engine::shotBurstAt`，同配置两跑逐帧相同）。
    /// 本形态仍然必要：它不依赖 RPC 门、拍完引擎自退，单张精确钉帧仍只有它。
    bool overrideLlmShot = false;
    std::string llmShotFile;       ///< `llm_shot.file`（相对引擎 CWD build/rgl；门开时必填）
    int llmShotDelayFrames = 8;    ///< `llm_shot.delay_frames`（消费方写死，别用默认值——理由见上）
    std::string deliveryConfigDir;
    /// 可选机位（**写进 profile 的 camera.lua**，从第 1 帧生效）。false = 不动 camera.lua。
    ///
    /// 为什么不能只靠 `Engine::setCamera`（RPC）：RPC 落在引擎已经跑了几帧之后，而那些帧是用
    /// camera.lua 的机位渲染的，**已经进了 TAA 历史**；历史按 (1−feedback) = 0.88/帧 衰减，
    /// 两个机位朝向差得远时污染要几十帧才洗掉。实测（交付配置 + 报障机位）：只发 RPC、预热
    /// 32 帧的 T3 恒等式残差 0.100/255 且逐帧仍在降（0.1208 → 0.100），机位从第 1 帧就对上的
    /// 同一场景同一配置只有 **0.0226** —— 差 4.4 倍，全部来自采集时序（参考图本身逐位相同）。
    bool hasCamera = false;
    std::array<double, 3> cameraPosition{0.0, 0.0, 0.0};
    std::array<double, 3> cameraTarget{0.0, 0.0, 0.0};
    double cameraFov = 45.0;
    std::string glb;                       // models/ 下的场景文件
    double envIntensity = 0.0;
    bool skybox = false;
    std::array<double, 3> ambient{0.0, 0.0, 0.0};
    double exposure = 1.0;
    double aoStrength = 0.0;
    bool shadowPass = false;               // → shadow.lua 的 mode（true="csm"，false="none"）
    bool izbShadow = false;                // → shadow.lua 的 izb（izb 消费域判据；与 mode 正交）
    bool evssmShadow = false;              // → shadow.lua 的 mode="evssm"（批 11.6d swim 判据）
    int izbDumpFrame = 0;                  // → shadow.lua 的 izb.dump_frame（>0：N 帧起每 30 帧覆盖写 izb_mask.ppm）
    /// **合成 profile 专用**（`shadow-kernel-scale`）：非空 = 合成路径写出的 shadow.lua
    /// **整份**由它给（调用方负责是合法 lua）。存在的理由：批 N6b 把三个布尔塌成 mode 单轴
    /// 枚举之后，**逐档调参块**在 ProfileOpts 里表达不出来——pcss 臂要
    /// `pcss = { sun_degrees = 5.0, search_radius = 16.0, max_penumbra = 32.0 }`，而「哪个档
    /// 的哪枚调参」是那台仪器的**臂定义**，不是工具的通用字段（为它加三枚字段 = 每个新档
    /// 都要来改 ProfileOpts）。
    /// ⚠ 交付配置路径**不吃**它：那条路径整份替换 shadow.lua = 把交付调参抄第二份（会漂），
    /// 走 `overrideShadowRoute` 的逐键覆写。
    std::string shadowLuaOverride;
    std::string variant;                   // "" = 模板默认（forward）；"deferred" 等 = 拷贝后重写 variant 行
    /// 抗锯齿选路（批 14.6：引擎 `runtime.lua` 的 `aa` 三态键）。Off = **不写键** = 出厂态。
    /// 原 `taa` 布尔（scene.lua `taa.enabled`）与 `fsrUpscaler` 布尔（`fsr_upscaler`）都收进
    /// 这一个槽位——「谁做抗锯齿」不存在同时命中两条的写法，臂表因此**写不出**非法组合
    /// （旧 checkFsrOpts 的「temporal 与 taa 互斥」守卫随双布尔一起删除）。
    /// TAA 的调参仍走 scene.lua `taa` 块（本结构的 taa* 系列字段），FSR 的走 `fsr` 块。
    Aa aa = Aa::Off;
    double taaFeedback = 0.12;             // scene.lua `taa.feedback`（= 本帧权重；**1** = 只抖不滤 = 消融组；0 = 「只要历史」的退化环，勿用）
    double taaJitterScale = 1.0;           // scene.lua `taa.jitter_scale`（0 = 只留时域累积，去抖动）
    /// 批 13.2：scene.lua `taa.base_space`（true = 最终消费处按内容偏移重采样，把输出从
    /// **抖动空间**搬回**基空间**）。false = 现行为（输出在抖动空间，游动 = 相邻帧抖动差）。
    /// ⚠ 本工具所有「游动 = 抖动差」的判据（`temporal-swim-pixels` / `temporal-jitter-*`）
    ///   钉的都是**门关**口径；门开态由 `temporal-base-space` 单独判（见其容差理由）。
    bool taaBaseSpace = false;
    /// scene.lua `taa.motion_compensation`（prevSpin 刚体补偿）。默认 true = **不写键**
    /// （写出的键越少漂移面越小，同其余诊断键的做法）；false = 显式写 `false`。
    /// ⚠ 它只关**场景旋转补偿**：spin 激励下历史错位 = 被测项（`--motion spin` 的 M3）；
    ///   **相机运动下它是恒等阵 = 关与不关逐位相同**，消融相机重投影必须用 `taaReprojection`。
    bool taaMotionCompensation = true;
    /// scene.lua `taa.reprojection`（prevViewProj 相机重投影消融门）。默认 true = 不写键；
    /// false = 历史按「同基位置」采样（prevViewProj := 本帧矩阵 + 位移清零 + motion 单位阵）。
    /// `temporal-motion --motion camera` 的 M3 用它——相机在动时这条路才真正被走到。
    bool taaReprojection = true;
    /// scene.lua `taa.diag_reject`（历史拒收诊断门，默认关）。开 = taa.frag 把逐像素
    /// 「历史怎么了」码编进 taa_out 的 alpha（0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 /
    /// 0.75 重投影拒收 / 1 无历史），RGB 逐位不变；回读 = shotBurst(attachment="taa_out")。
    bool taaDiagReject = false;
    /// scene.lua `taa.input_kernel`（批 13.8 输入侧 un-jitter 的**重建核选择**）。
    /// 0 = 双线性（**引擎默认 = 不写键 = 逐位今天**）/ 1 = Catmull-Rom 4×4 / 2 = Lanczos3 6×6。
    /// ⚠ 0 时**不写键**（写出的键越少漂移面越小，同其余诊断键的做法）；非 0 才写。
    /// 消费方 = `temporal-flicker` / `aa-coverage` / `temporal-feedback` 的 `--kernel`。
    double taaInputKernel = 0.0;
    /// 批 13.11 **历史裁剪消融门**（scene.lua `taa.clip_mode` / `taa.clip_soft`）。
    /// 0 = 现行 = **不写键**（门关态与交付配置逐字相同）。档位语义 → `rgl/scene_data.cppm`
    /// TaaParamsBlock::clipControl。消费方 = `imgqa temporal-residual --clip-abl`。
    double taaClipMode = 0.0;
    double taaClipSoft = 1.0;
    // ── Phase 14 批 14.5/14.6：升频 / aa 臂（量「闪烁↔鬼影」跷跷板用）──────────────────
    // 键的语义与合法区间以 `configs/runtime.lua` 为唯一真相源，此处不复述。
    // 批 14.6 换轨：`fsr_upscaler` 布尔 → `aa` 三态（见上）；`fsr_temporal` / `fsr_jitter`
    // 两个扁平键 → `fsr` 嵌套块的两枚调参（**aa=Fsr 时总是显式写两枚**——出厂默认是
    // true/true = 设计点，不写键 = 拿出厂值 = 想量纯空间却静默跑成时域+抖动，静默换臂）。
    /// runtime.lua `render_scale`。1.0 = **不写键** = 交付态（渲染网格 = 后缓冲逐像素）。
    /// ⚠ 创建期量（RT/深度按它分配）⟹ 只能靠重启换档，**不能** `/param` 热改。
    double renderScale = 1.0;
    /// runtime.lua `fsr` 块 `temporal`（让 vendor 累积历史）。false = 纯空间 EASU+RCAS。
    /// 仅 `aa == Fsr` 时被消费（其余态 = 空开关，`checkFsrOpts` 抛）。与 TAA 的互斥
    /// **不再需要守卫**：aa 一个槽位装不下两个时域滤波器。
    bool fsrTemporal = false;
    /// runtime.lua `fsr` 块 `jitter`（FSR 自己的 Halton 抖动，vendor 相位序列）。
    /// ✅ **与 `taaJitterScale` 是两个独立旋钮**——这是「鬼影仪器 jitter 恒 0」与「FSR 必须
    /// 带抖动输入」表面冲突的解法：FSR 臂的 aa 是 Fsr（结构上非 Taa）⟹ `taa.jitter_scale`
    /// 根本不被消费，FSR 的抖动走本键自足。仪器的三条 jitter 红利（B∞ 是固定点 / 零锚恰 0 /
    /// 分母无相位噪声）因此**一条都不必牺牲**。
    bool fsrJitter = false;
    /// 帧驱动 spin（scene.lua `spin_frame_angle`，rad/帧）。0 = **不写键** = 门关 = 逐位今天。
    /// ≠0 时同时写 `spin_axis`（`spinAxis`）——运动必须是绕 up 轴的定角 yaw（刚体、
    /// prevSpin 重投影能精确对齐；理由全文 = `temporal.cpp` 头注释）。
    /// ⚠ spin 转的是**场景**（相机不动）⟹ prevViewProj 恒定、相机重投影路不被走到；
    ///   量「相机运动的 TAA」必须用 `cameraFrameYaw`。消费方 = `temporal-motion --motion spin`。
    double spinFrameAngle = 0.0;
    std::array<double, 3> spinAxis{0.0, 1.0, 0.0};
    /// **相机**帧驱动 yaw（camera.lua `frame_yaw`，rad/帧）。0 = 不写键 = 门关 = 逐位今天。
    /// ≠0 ⟹ yaw = 基准 + frame_yaw·帧号（纯函数，跨进程可复现；基准被 RPC 写入时自动重定基
    /// ⟹ 跨进程只差常数帧偏移，由 temporal-motion 的 argmin 对齐吸收）。绕 up 轴定角 yaw 的
    /// 像素位移与深度无关（刚体相机运动，prevViewProj 重投影精确对齐；平移 dolly 位移 ∝1/深度
    /// 远处信号衰减——`temporal.cpp` 头注释当年登记的理由）。消费方 = `temporal-motion` 默认模式。
    double cameraFrameYaw = 0.0;
    /// 交付配置模式下**覆写 `runtime.lua::headless_size` 为 `size`**（默认 false = 不动，
    /// 现有判据全部钉 1280×720，隐式改尺寸 = 换口径）。`temporal-motion` 的超采样参考组用。
    bool overrideHeadlessSize = false;
    std::string entitiesLua;               // 非空 = 整条 entities 行由它给（内建合成靶无 glb 文件）
    std::array<double, 3> lightDir{0.5, -0.75, -0.45};
    double lightIntensity = 12.0;
    bool picking = true;                   // object_id 附件 = 区域归因的根基
    std::array<int, 2> size{1280, 720};
};
std::string makeProfile(const std::string& outDir, const ProfileOpts& opts);

/// 引擎回显的相机姿态（度）。批 13.14a 相机运动版尾长的**解析**映射与配对都靠它——
/// 见 `Engine::cameraPoseDegrees()` 的两条理由（pitch 进映射、yaw 定配对）。
struct EngineCameraPose {
    double yawDegrees = 0.0;
    double pitchDegrees = 0.0;
    double fovDegrees = 0.0;
};

/// 引擎日志尾部（默认尾 12 行，每行前缀 `\n    | `）：`Engine` 构造失败时把日志尾部带进
/// 异常的取证件（定义在 `engine.cpp`，理由全文见那里的头注释）。selftest 直接喂合成日志
/// 核「n 行取尾 lines 行」的口径（债-92：起点写成 `n` 让本函数在 n>lines 时恒返回空串）。
[[nodiscard]] std::string logTail(const std::string& path, int lines = 12);

// ── 债-89：连拍锚 = 绝对帧号（时域判据可复现性的地基）────────────────────────────
//
// 为什么必须有锚点：抖动相位 = Halton(**绝对帧号**)、frame_yaw 机位 = 帧号纯函数，而
// 相对锚（`advanceFrames` + 裸 `shotBurst`）下 burst 起始帧 = RPC 就绪落在第几帧 + 墙钟
// ⟹ 相位是彩票（债-89 亲验：同配置三跑边缘列飘 ±2px、一次整组 rc=1；`temporal-swim-anchor`
// PASS↔FAIL 互换；bistro G0 五连跑出 2 种结构签名）。
//
// 锚点基座必须**远高于**「RPC 就绪落在第几帧」的抖动（该抖动 = 就绪轮询粒度 × 帧率；
// 轮询粒度已收紧到 10ms，见 Engine 构造注释；本机 1280×720 headless 实测就绪帧 ≤ ~60）。
// 锚点被超过 = 引擎 409 / 客户端立刻抛 = 响亮失败，**唯一正确响应是抬高本常量**，
// 永远不许静默退回相对锚（那等于把彩票藏起来，比诚实的彩票更坏）。
inline constexpr uint32_t kBurstAnchorBase = 512;
/// `advanceToFrame` 与 `shotBurstAt` 之间的前置帧数：长等待留在客户端（300s 预算 + 卡死
/// 检测走 `GET /` 轮询），引擎侧只等最后 lead 帧（远低于 120s 的 burst 应答预算）。lead 必须
/// ≫「轮询过冲 + HTTP 往返」的帧数（本机 2500fps：20ms 轮询 ≈ 50 帧 + 往返 ≈ 3 帧）。
inline constexpr uint32_t kAnchorLead = 256;

/**
 * 债-89：burst 锚点校验的**纯函数**（selftest 直接喂合成值核口径，不吃 GPU；先例 = logTail）。
 *
 * 锚点已过 = 帧计数已越过 startFrame（**相等还来得及**：请求在帧首钩子里 drain，本帧
 * drawFrame 尚未跑）。已过 ⟹ 返回可归因错误文案（点名锚点、当前帧、正确响应）；未过 = 空串。
 */
[[nodiscard]] std::string burstAnchorMissError(uint32_t startFrame, uint32_t frameNow);

/**
 * 债-89：burst 锚点落帧自证的**纯函数**（同上，selftest 喂合成值）。
 *
 * 引擎回显首张实际落帧。**缺回显 = 旧构建静默忽略了 `start_frame`**——burst 实际按相对锚
 * 跑了而读数看起来完全正常 = 最贵的静默退化，必须报；落帧 ≠ 锚点（如锚帧被 acquire 跳帧）
 * 同样必须报：采集点已经不是请求的那个相位。通过 = 空串。
 */
[[nodiscard]] std::string burstAnchorEchoError(uint32_t startFrame, bool echoedPresent,
    uint32_t echoedFrame);

/// 引擎进程：起 headless、RPC 驱动、干净退出。单实例纪律 = 起跑前 pgrep，有残留拒跑
/// （并行实例互相遮挡合成，会把画面与帧率双双扭曲，AGENTS.md 测量教训同源）。
/// 起跑前先过 SPV 新鲜度守卫（.slang 比其所属 spv 新 = 疑似 stale 拒跑，批 12.3d-D2）。
class Engine {
public:
    explicit Engine(std::string profileDir, int port = 8788);
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    struct Camera {
        std::array<double, 3> position;
        std::array<double, 3> target;
        double fovDegrees = 60.0;
    };
    /**
     * 钉机位（RPC `POST /camera`）。
     *
     * @param yawAtFrame 债-89 机位锚点：>0 = 「第该帧**渲染出来**的朝向 = 本次写入的朝向」，
     *   引擎据此解 `yawBase`（`frameDrivenYawPin`）。为什么客户端自己预补偿不行：写入落帧
     *   由 HTTP 时序决定（本机 headless ~6000fps ⟹ 落帧抖动 ±36 帧量级 ≈ 4.5px），补偿量
     *   无论怎么算都带着这个不确定性；只有把机位写成**绝对帧号的函数**才与落帧无关。
     *   前提 = 该 profile 的 `camera.lua frame_yaw ≠ 0`（否则引擎 400 响亮拒绝）；锚已过 = 409。
     *   静止机位的仪器（无 frame_yaw）传 0 = 现行为逐位不变。
     */
    void setCamera(const Camera& cam, uint32_t yawAtFrame = 0);

    /**
     * 读回引擎当前的 yaw（度）。`/camera` 的空 body = 只读回显。
     *
     * 为什么必须有：移动态采集用 `camera.lua frame_yaw` 驱动，yaw 是 `基准 + 每帧角 × 帧号`
     * 的**纯函数**。要在这段运动的**中点姿态**上补拍 object_id，就得先知道基准 yaw 与当时的
     * 帧号，才能把中点姿态算出来并用 RPC 精确落位。
     */
    [[nodiscard]] double cameraYawDegrees();

    /**
     * 读回引擎当前的相机姿态（度）——一次空 body `POST /camera` 拿全三个字段。
     *
     * 为什么必须有（批 13.14a 相机运动版尾长）：
     *  - **pitch**：分母要**解析**的帧间像素映射，而该映射依赖 pitch（`frame_yaw` 动的是
     *    yaw、pitch 不变 ⟹ 旋转轴是世界 Y 不是相机 up，pitch ≈ 12° 时忽略它会错
     *    0.485px/帧、pitch ≈ −34° 的机位错 1.43px/帧）。从 position/target 反解 pitch
     *    等于**复制引擎公式**（会漂），故走 RPC 读回。
     *  - **yaw**：配对偏移的**唯一可靠来源**。首轮实测 `framePre` 之差在各组间是
     *    −12…−1，而绝对偏移宽扫的 argmin 落在 0 附近 ⟹ **渲染帧计数器不是 yaw 帧索引**。
     *    而 yaw 本身是 `yawBase + frameYaw·帧号` 的纯函数，且 burst 前后各读一次就得到
     *    **实测每帧角速度** ⟹ 配对偏移 = `Δyaw / 每帧角`，仍是「引擎报真值 + 解析换算」，
     *    **不引入任何图像配准**（本命令的设计红线）。
     *
     * ⚠ pitch 在整个 burst 内恒定（frame_yaw 不碰它），读一次即够；yaw 必须在 burst **前后各
     *    读一次**（burst 内它在动）。
     */
    [[nodiscard]] EngineCameraPose cameraPoseDegrees();

    /// 只改 yaw（position/target/fov 不动）。RPC 写相机 = **重设基准**，之后 frame_yaw 从新基准
    /// 继续推进 ⟹ 调用方要按「一次 /shot 会推进多少帧」预先补偿，见 captureFlickerSeries。
    void setCameraYaw(double yawDegrees);
    /**
     * 运行期改一个滑杆（POST /param）。
     *
     * 为什么判据必须有这根线：**跨进程 A/B 不是 A/B**。两次 `Engine` 是两次进程、两次
     * shader 编译、两次驱动初始化，把它们的不同归因给被测开关，等于把「环境差异」当成
     * 「被测项」。实测（ao-noise 首跑）：跨进程的 AO on/off 两张图在**契约区**里出现最大
     * 41 LSB 的差，而 AO 场在该区最大只偏离契约 16 LSB（×strength 0.5 最多 8 LSB）——
     * 多出来的那部分根本不是 AO。同进程内用 `/param` 翻强度，两张图之间**只隔一个 float**，
     * 差值才是被测项。
     * @param page 页名（如「渲染」；空 = 全页搜索）
     * @param label 滑杆 label（真相源 = `rgl/generated/param_semantics_generated.cppm`）
     * @param value 新值
     */
    void setParam(const std::string& page, const std::string& label, double value);

    /**
     * 读回引擎**当前生效**的滑杆值（`GET /state` → pages → entries → value，纯观测）。
     *
     * 为什么必须有这根线（feedback 扫描这轮立的红线）：**「我把它写进 lua 了」≠「引擎用了它」**。
     * lua 侧键名拼错、解析器改了读法、env 装配被后面的赋值覆盖——三种失效在 lua 文件里都
     * 看不出来，而参数扫描会给出一条**看起来完全合理**的曲线（参数根本没传进去时曲线是平的，
     * 平曲线恰好会被读成「该参数无效」这个结论）。故扫描的每一组必须回显**引擎自己报出来**的
     * 生效值，而不是回显写进 lua 的值。
     * @param page 页名（如「渲染」；空 = 全页搜索）
     * @param label 滑杆 label（真相源 = `rgl/generated/param_semantics_generated.cppm`）
     */
    [[nodiscard]] double paramValue(const std::string& page, const std::string& label);

    /// 拍一张。attachment=nullptr = 后缓冲；否则具名附件（"object_id"）。返回 PNG 绝对路径。
    [[nodiscard]] std::string shot(const std::string& tag, const char* attachment = nullptr);

    /// 引擎装配态的帧计数（`GET /` 回显的 `frame`，纯观测；每帧在帧首钩子里 +1）。
    /// 为什么读 `GET /` 而不是 `GET /state`：两者回显同一个计数器，而 `/state` 每次调用都
    /// 重建**全部参数页**（`rpcHandleState` → `buildDebugParamPages`），热轮询下纯烧。
    [[nodiscard]] uint32_t frameCount();

    /**
     * 空转推进 n 帧（**不写盘**）——长热身唯一可用的形态。
     *
     * 为什么必须有这根线（批 13.1，两条实测踩过的坑）：EMA 类累积的瞬态按 (1−α)^t 衰减，
     * 热身不足量到的是**瞬态**（历史还没动起来 = 画面冻在初始相位）而不是稳态；α=0.002 要
     * 6/α = 3000 帧。用 `/shotburst` 热身 = 写 3000 张 PNG，直接超时。本机 headless 实测
     * ≈2500 fps（`GET /` 的 frame 计数在 5s 内走了 12593），3000 帧 ≈ 1.2s，于是 α 可以
     * 一路推到 0.002 而整条判据仍是分钟级。
     *
     * 🔴 **不许拿它当连拍锚**（债-89）：相对推进只保证「又渲染了 n 帧」，burst 会落在第几帧
     * 由 RPC 就绪时序 + 墙钟决定，而抖动相位 = Halton(绝对帧号) ⟹ 读数成彩票。连拍锚走
     * `advanceToFrame` + `shotBurstAt`（绝对帧号）。本函数只留给「只要渲染够久就行、burst
     * 落点无所谓」的旧调用点（temporal-step / temporal-motion，转绝对锚 = 各自债的账）。
     * @param n 要推进的帧数（相对调用时刻）
     * @param timeoutSec 帧计数不推进 = 引擎卡死，超时抛错而不是静默少热身
     */
    void advanceFrames(uint32_t n, int timeoutSec = 300);

    /**
     * 空转推进到**绝对帧号** absFrame（不写盘；债-89，连拍锚的等待原语）。
     *
     * 与 `advanceFrames` 的分工：那边「再跑 n 帧」，这边「跑到第 N 帧」——只有后者能让
     * burst 的抖动相位（= Halton(绝对帧号)）与 frame_yaw 机位跨进程逐帧复现。长等待走这里
     * （客户端 300s 预算 + 卡死检测），随后 `shotBurstAt(absFrame + lead)` 让引擎只等最后
     * lead 帧——两段拼起来把 burst 起点钉死在绝对帧号上。
     *
     * 调用时帧计数已越过 absFrame = 锚点错过 ⟹ **立刻抛**（可归因文案）。绝不静默退回
     * 「再跑 n 帧」：那会把彩票藏进看起来正常的读数里（债-89 的教训本体）。
     * @param absFrame 目标绝对帧号（RPC 帧计数，引擎进程起跑记 0，帧首 +1）
     * @param timeoutSec 帧计数不推进 = 引擎卡死，超时/停滞抛错而不是静默少热身
     */
    void advanceToFrame(uint32_t absFrame, int timeoutSec = 300);

    /**
     * 连续采 n 帧（POST /shotburst）——**时域差分唯一合法的采集口**。
     * 与 shot() 的区别不是「省几次往返」：`/shot` 每张落在哪一帧取决于 HTTP 请求与
     * 帧首钩子的先后（= 墙钟），相邻两张根本不是相邻两帧，逐帧差于是测的是随机相位窗
     * （实测同一个量随预热长度差 3 倍）。`/shotburst` 的「下一张」由引擎在帧首结算
     * 钩子里自己登记（该钩子先于 drawFrame）⇒ 采集落在连续的第 N..N+n-1 帧上。
     * ⚠ 连续性 ≠ 可复现：本重载的 N（起始帧）仍由 HTTP 时序决定 = 相对锚。判据采集走
     * `shotBurstAt`（绝对锚，债-89）；本重载只留给「起点无所谓」的旧调用点与热身连拍。
     * @param dir 引擎 CWD（build/rgl）下的输出目录
     * @param n 张数（≥1）
     * @param prefix 文件名前缀（引擎写 `<prefix>%03d.png`）
     * @param attachment 非空 = 连拍**具名附件**（如 `taa_out`：temporal-motion 取历史
     *                   拒收 alpha 掩膜）；空 = swapchain 后缓冲
     * @return 引擎回显的绝对路径，顺序 = 帧序
     */
    [[nodiscard]] std::vector<std::string> shotBurst(const std::string& dir, uint32_t n,
        const std::string& prefix = "f", const std::string& attachment = "");

    /**
     * 从**绝对帧号** startFrame 起连续采 n 帧（POST /shotburst 的 `start_frame`；债-89）。
     *
     * 引擎在帧首钩子里等帧计数到 startFrame 才登记首张 ⇒ 采集恰落第 startFrame..
     * startFrame+n-1 帧，抖动相位与 frame_yaw 机位跨进程逐帧确定——W29/W30/W31/W37 的
     * 可复现性全押在这一句上。三道响亮闸门，任何一道都不静默：
     *  1. 发请求前预检：帧计数已越过锚点 ⟹ `burstAnchorMissError` 抛；
     *  2. 引擎侧：请求到得晚、锚已过 ⟹ 409（本函数转抛，带引擎原文）；
     *  3. 应答回显 `startFrame` ≠ 请求值（含**旧构建无回显**= 静默忽略 start_frame）
     *     ⟹ `burstAnchorEchoError` 抛。
     * @param startFrame 锚点绝对帧号（须 > 当前帧；用 `kBurstAnchorBase + 热身` 量级）
     */
    [[nodiscard]] std::vector<std::string> shotBurstAt(const std::string& dir, uint32_t n,
        uint32_t startFrame, const std::string& prefix = "f",
        const std::string& attachment = "");

private:
    struct Impl;
    Impl* impl_;
};

// ═══════════════════════════════════════════ 帧 + 解码 + 区域归因

/// 一次采集：color(+objectID) 两张图 + 预计算的线性域派生量。
struct Frame {
    cv::Mat bgr;             // 8UC3（观测字节，已 sRGB 编码）
    cv::Mat ids;             // 32SC1，= objectIndex+1，**0 = 背景**
    cv::Mat lin;             // CV_64FC3，sRGB 解码后的线性色
    cv::Mat luma;            // CV_64FC1，Rec.709 线性亮度
    Engine::Camera camera;
};

/// PNG → BGR（imread 失败立刻抛——读图器是判据的入口，静默读错 = 最贵的假线索）。
[[nodiscard]] cv::Mat readPngBgr(const std::string& path);

/// object_id PNG → id 图。**编码 = R=id[7:0] G=[15:8] B=[23:16]（小端 24bit）**，
/// OpenCV 通道是 BGR 序：id = ch[2] | ch[1]<<8 | ch[0]<<16。id 是**编码不是颜色**——
/// 既不过 gamma 也不 clamp，绝不与颜色图共用解码（llm_debug_impl.cppm convR32Uint）。
[[nodiscard]] cv::Mat decodeObjectId(const cv::Mat& bgr);

/// 填 lin/luma（sRGB LUT + 709 权重）。
void prepareLinear(Frame& f);

/// 一台机位拍齐 color + object_id（两者必须同视角，否则 mask 与像素对不上，
/// 后面所有区域统计都会串味——且串得"看着还挺像"）。
[[nodiscard]] Frame captureFrame(Engine& eng, const Engine::Camera& cam,
    const std::string& tag);

/// 一次性跑引擎到自然退出（llm_debug 截图模式专用：qa/configs/analytic 那套配置
/// llm_debug=true + rpc=false，与 Engine 的 RPC 形态互斥——llm 截图在跑时 /shot 会
/// 409，是引擎既有约定，工具迁就配置而不是反过来）。单实例纪律同 Engine；
/// 起跑前先过 SPV 新鲜度守卫（同 Engine，批 12.3d-D2）。
/// 非零退出抛异常并附日志路径（对着超时猜死因是排查时间的黑洞）。
void runEngineOnce(const std::string& configDir, int timeoutSec = 180);

struct ManifestPiece {
    std::string name;
    int32_t primitiveIndex = -1;  // manifest mapping 声明：piece 序 == globalPrimitiveIndex
    std::array<double, 3> color{};
    double roughness = 0.9, metallic = 0.0;
    std::array<double, 3> aabbMin{}, aabbMax{};
};
struct Manifest {
    std::vector<ManifestPiece> pieces;
    std::map<std::string, Engine::Camera> cameras;
};
/// 读 build/rgl/models/<stem>.manifest.json（objectIds **刻意 ABSENT**——id 是运行期
/// objectSlot 计数，写进 manifest 就是第二份真相；harness 用颜色共现反解）。
[[nodiscard]] Manifest loadManifest(const std::string& glbStem);

struct PieceStat {
    std::string piece;         // 认不出名字 = "unnamed#<oid>"
    int32_t oid = 0;
    int64_t pixels = 0;
    double medianLinear = 0.0, p05Linear = 0.0, p95Linear = 0.0;
    double darkFraction = 0.0;  // 线性亮度 < darkEps 的比例
    double chromaR = 0.0, chromaG = 0.0;  // 亮像素平均色度（排查反解错配用；全暗 = NaN）
};

/// id ↔ piece 反解。**色度只从亮像素取**——渲染色是 albedo 的标量倍（metal=0 + 白光）
/// ⟹ 色度不变，可按最近邻认 piece；但黑像素色度无定义（0/0），而本项目当下要判的
/// 恰是黑面。若某 id 全暗则**拒绝安名字**报 unnamed——宁可无名，不要安错名字：
/// 安错名字会让后续所有判据在错误的 piece 上求值，比失败更难查。
struct Attribution {
    std::vector<PieceStat> stats;  // 含 unnamed
    std::map<int32_t, std::string> nameByOid;
    std::vector<int32_t> unnamed;
    /// 反解冲突（"id A 与 id B 都判为 X"）。**不 throw**——排查现场工具必须先吐出
    /// 它看到的原始数据（异常 = 在离答案一步的地方失明）；冲突由判据报 FAIL。
    std::vector<std::string> conflicts;
    [[nodiscard]] std::vector<std::string> visiblePieces() const;
};
/// 色度弱校验的期望值 = **闭式解预测字节在观测域的色度**（不是 albedo 色度！
/// albedo 色度与观测色度之间隔着 ACES 通道串扰——纯绿的串扰最大，实测把期望设成
/// albedo 色度会让 far_tower 恒报警，验证器沦为噪音）。lightDir/intensity 必须与
/// makeProfile 写进配置的同一出处。
[[nodiscard]] Attribution attribute(const Frame& f, const Manifest& man,
    std::array<double, 3> lightDir = {0.5, -0.75, -0.45}, double lightIntensity = 12.0,
    double darkEps = 0.005, double chromaMinLuma = 0.02);

// ── 区域代数（整套系统只有一种数据：mask）──

[[nodiscard]] cv::Mat maskOfPiece(const Frame& f, const Attribution& at,
    const std::string& name);
[[nodiscard]] cv::Mat dilateMask(const cv::Mat& mask, int k = 1);
/// 轮廓带 = 膨胀 k 圈减原 mask——"物体边缘"的正规定义（大量 bug 只在边缘出现）。
[[nodiscard]] cv::Mat ringMask(const cv::Mat& mask, int k = 2);
/// 连通域计数（cv::connectedComponents）。一个 piece 渲成 N 个互不相交的岛通常就是 bug。
[[nodiscard]] int connectedCount(const cv::Mat& mask);

/// 任一通道不同的 8UC1 掩码 = 帧间差分的最小原语（`countNonZero` 数变化像素、
/// `bitwise_and` 与区域掩码求交都吃它）。
/// ⚠ **不要**"简化"成 `cv::reduce(axis=0)` 合通道——reduce 对多通道输入按通道独立归约、
///   输出仍是多通道，`countNonZero` 直接断言 `cn == 1` 炸（switch 判据首战抓到）。
/// 本函数曾在 4 个 TU 里各存一份逐字节相同的实现（`temporal.cpp::anyDiff`、
/// `judge.cpp` / `temporal_residual.cpp::anyChannelDiff`、
/// `temporal_motion.cpp::anyChannelDiffLocal`），上面那条教训也跟着抄了 4 遍——
/// 收敛到此处单点，改一次四处同时生效。
[[nodiscard]] cv::Mat anyChannelDiff(const cv::Mat& a, const cv::Mat& b);

/// 一维间隙聚类（同材质各面色度同向、只差 NdotL ⟹ 一维足够；不用 k-means：k 未知，
/// 而"簇 = 亮度直方图里的分离"在此是物理事实——每个可见面一个亮度）。
[[nodiscard]] std::vector<std::pair<double, int64_t>> lumaClusters(cv::InputArray luma,
    double gap = 0.02, double minFraction = 0.02);

/// 排序序列上取百分位（实现 = `image.cpp`，`fillReadout` 的底座）。⚠ `q` 是 **0..100**，与
/// `temporal_residual.h` 的 `percentileOf`（p = 0..1）**不是同一个刻度**，故刻意不叫那个名字。
/// 空集**抛**（债-99）：`size() − 1` 下溢成 SIZE_MAX = 越界读，把堆上的鬼数当分位数印出来；
/// 「无分位数」必须与「分位数是 0」可区分——0 是合法读数，占不了这个位置。
[[nodiscard]] double percentile100(const std::vector<double>& sorted, double q);

// ═══════════════════════════════════════════ 判据 + 反模式 + 台账

struct Verdict {
    std::string name;
    bool ok = false;
    std::string observed;
    std::string whyTol;                  // 必须写**理由**（普适不变量，selftest 强制）
    std::vector<std::string> guards;     // 台账：钉住git:调试历史里的哪一条
    [[nodiscard]] std::string line() const;
};

/// 判据族 = **成立条件的分组**，不是主题分类。扁平清单让"这条判据属于哪一族、它的成立
/// 条件是什么"只能靠读 564 行散文；而两族时域判据的比值口径根本不同（见下面 condition）。
enum class JudgmentFamily {
    Foundation,      ///< 地基：可见集 / 闭式解 / 确定性 / 影响域 / 影场不变量
    TemporalStatic,  ///< 静止机位 + jitter：要求**平均视点恒定**，比值才是时域累积的增益
    TemporalMotion,  ///< 运动 / 残余 / 鬼影 / feedback：视点真的在动，比值口径与上一族不同
    Attachment,      ///< 附件消费域：IZB / AO「这张附件被谁消费」
};

/// 判据元信息注册表（check 子命令按族分组打印用；求值函数在 commands.cpp 直接调用）。
struct JudgmentInfo {
    const char* name;
    JudgmentFamily family;               ///< 排第 2 位且无默认值：漏填 → const char* 转不进 enum → 编译失败
    const char* whyTol;                  // 每条必填（>20 字符的**理由**，selftest 强制）
    std::vector<std::string> guards;     // 每条至少一条真实 bug/变异出处（selftest 强制）
};
[[nodiscard]] const std::vector<JudgmentInfo>& registeredJudgments();

/// 族的中文标题 / **成立条件**（`imgqa check` 分组输出的口径单一真相源，在 judgments_table.cpp）。
[[nodiscard]] const char* judgmentFamilyTitle(JudgmentFamily family);
[[nodiscard]] const char* judgmentFamilyCondition(JudgmentFamily family);

// 具体判据（实现分散，声明集中）：
[[nodiscard]] Verdict judgeVisibility(const Frame& f, const Attribution& at,
    const std::vector<std::string>& expectVisible);
[[nodiscard]] Verdict judgePieceDrawn(const Frame& f, const Attribution& at,
    const std::string& piece, int64_t minPixels = 50);
[[nodiscard]] Verdict judgeFaceClusters(const Frame& f, const Attribution& at,
    const std::string& piece, const std::vector<FacePrediction>& predict, double tol = 0.02);
[[nodiscard]] Verdict judgeSwitchDomain(const Frame& base, const Frame& alt,
    const cv::Mat& allowed);
[[nodiscard]] Verdict judgeDeadBranch(const Frame& base, const Frame& alt,
    const cv::Mat& mask, const std::string& label);
/// 同配置重跑必须逐位相同（color + id 两通道面）。这是 switch-impact-domain 的
/// **前置判据**：非确定性在场时「域外恰为 0」永远不成立，会把引擎噪声归因到被测开关上。
[[nodiscard]] Verdict judgeDeterminism(const Frame& a, const Frame& b);

// ── 时域差分（批 12.6 T4：TAA 阳性锚点的度量本体）────────────────────────────
//
// 与上面所有判据的**根本差别**：这里比较的不是"两次采集"，而是**同一会话内的相邻帧**。
// 其余判据默认"画面是静态真值"，本判据默认"画面在动，动的幅度才是被测量"。
//
// 为什么相邻帧差是 TAA 的正确观测量：细杆这类亚像素宽度的高对比边缘，光栅化按像素中心
// 判覆盖 ⇒ 机位亚像素移动时覆盖状态**硬翻转**（不是渐变）= 闪烁。TAA 的作用正是把不同
// 采样相位的估计在时域上平均掉。所以"关 TAA 逐帧差大、开 TAA 逐帧差小一个量级"就是
// "时域累积确实在生效"的像素级证据（docs/taa_plan.md §6 硬判据）。
//
// ⚠ 两个会让这条判据变成假绿的设计错误（都写进实现注释，勿删）：
//  ① **静止机位下 TAA 关 = 逐位相同 = 零信号**。必须有亚像素运动在场，且运动由
//     **本工具制造**（RPC 微动），不是引擎自带（SceneSpin 速度不可配、~1°/帧 = 远超
//     亚像素档，重投影都救不回来）。
//  ② **匀速运动的单调分量时域 IIR 滤不掉**（稳态下输出斜率 = 输入斜率）。能压下去的
//     只有"同一视点下不同采样相位的估计差"——前提是**重投影把运动本身对齐掉了**。
//     故机位运动必须是重投影能精确处理的**刚体相机运动**（本项目 = 纯 yaw 转角，
//     像素位移与深度无关；平移 dolly 会让远处栅栏的单帧位移只剩 0.08px，信号随深度衰减）。

/// 相邻帧差分统计（mask 内、**线性域** luma；文件头硬约束第 2 条）。
struct TemporalStat {
    double meanAbsDiff = 0.0;      ///< 逐帧对 |Δluma| 均值的平均（主测量）
    double maxAbsDiff = 0.0;       ///< 单像素单帧对最大值（看有没有离群爆点）
    double changedFraction = 0.0;  ///< mask 内"任一字节不同"的像素占比（跨帧对取最大）
    double edgeSoftness = 0.0;     ///< mask 内 0.05<luma<0.95 的占比（跨帧均值）
    int64_t maskPixels = 0;
    int pairs = 0;
};
/// shots 必须**同机位序列的相邻帧**（本工具 = 等步长 yaw），且都已 prepareLinear。
/// mask 为空 = 全图。少于 2 帧 ⟹ pairs=0（统计量无定义，调用侧按失败处理）。
///
/// `edgeSoftness`（中间调占比）与其余三条量的是**不同轴**：逐帧差量"时域稳不稳"，
/// 它量"空间上有没有真的把边缘摊开"。加它的理由 = 一次真实歧义：A/F≈1.0 有两种解释
/// （① TAA 完全没干活；② TAA 在抗锯齿，但相邻帧差被"相机真的动了"的运动项主导），
/// 逐帧差本身分不了这两种，中间调占比能分（②会让它显著上升）。
///
/// ⚠ 带**必须按实测白电平取**，不许写死 [0.05,0.95]：靶的白件经 ACES 后落在 ~0.8 而非
/// 1.0（observedLuma 是 sRGB 解回来的线性值），写死的带会把整块白板算成"中间调"——
/// 实测首版就是这样 83% 恒饱和、什么也分不出来。现取 [0.15·max, 0.85·max]。
/// @param softMask 中间调统计用的区域（调用侧给**未外扩**的靶内部；外扩圈会把背景
///                 clear 色 ~0.11 算进来）。空 = 复用 mask。
[[nodiscard]] TemporalStat temporalDiffStat(const std::vector<Frame>& shots,
    const cv::Mat& mask, const cv::Mat& softMask = {});

/// 按注册表信息造 Verdict（容差理由/钉住条目从注册表单点取，判据实现里不手抄）。
[[nodiscard]] Verdict verdictFor(const char* name, bool ok, std::string observed);

/// 一条反模式**否掉的是哪一层**——必填、无默认值（漏填或把 why 串位 → 编译不过）。
/// 立规出处 = `screen-space-l1-frame-diff` 自己记的学费：作用域写宽一格（把「算子被否」
/// 写成「激励被否」），防腐条款就连带冻掉后来真正需要的激励（实测冻掉两天）。
enum class AntiScope {
    Operator,  ///< 否的是「怎么算」（算子 / 度量 / 容差形态）
    Stimulus,  ///< 否的是「怎么激励 / 怎么采集」
    Domain,    ///< 否的是「在哪些像素 / 几何上断言」（论域选择）
    Baseline,  ///< 否的是「拿什么当参考」（跨日 golden 之类）
};

/// 一条反模式的结构化登记。`stillOpen` 与 `why` **同权重**：只写"否了什么"会被读成
/// "这一片都别碰"——没被提到 = 被禁止。故必须正面写出未被否、仍然在用的那条路。
struct AntiPatternInfo {
    const char* name;
    AntiScope scope;        ///< 排第 2 位且无默认值：漏填 → const char* 转不进 enum → 编译失败
    const char* why;        ///< 被否的是什么、为什么（历史记录，含后续更新，不删）
    const char* stillOpen;  ///< **未被否的部分**（selftest 强制它含反引号包裹的具体名字）
};

/// 反模式登记（负面知识也是工具的一部分）：装一条但**拒绝执行**，比不装更好——
/// 否则下一个会话的我（或别人）会重新发明它。拒绝比缺失更能传达判断。
[[nodiscard]] const std::map<std::string, AntiPatternInfo>& antiPatterns();

/// AntiScope 的中文口径（`imgqa refuse` 打印用；单一真相源在 judge.cpp）。
[[nodiscard]] const char* antiScopeName(AntiScope scope);

// ── 调用记录（判据可信度的一手数据：哪条判据真在排查里被调用、事后看对不对）──
void logCall(const std::string& cmd, const std::vector<std::pair<std::string, std::string>>& args,
    const std::vector<Verdict>& verdicts);
/// 债-101①：命令耗时起点打点（main.cpp 在进 dispatch 前调 = 解析结束、命令真正开始干活的
/// 地方）。台账的 `secs` 字段 = 打点到该条记录写出时刻的秒数（毫秒精度）。
void markCallStart();
/// 未自记命令的兜底信封（债-101①）：本次调用已写过富记录则不动作；`error` 非空 = 崩溃
/// 路径的记录（崩掉的那次最想知道多久）。口径全文在 judge.cpp 定义处。
void logCallEnvelope(const std::string& cmd, const std::vector<std::string>& argv, int rc,
    const std::string& error = "");
/// 台账某一行是不是**信封行**（判据 = 带原始 `argv`；富记录用策展的 `args`，两者互斥）。
/// `note` 用它把"回填目标是一条兜底记录"这件事**大声说出来**——不拦（跑完只有信封记录的
/// 命令再 note 是正当用法），但数据侧的错挂是静默的。口径全文在 judge.cpp 定义处。
[[nodiscard]] bool isEnvelopeRecord(const std::string& line);
/// ⚠ **只给 selftest**：清掉探针测试置起的「已记录」旗（否则 selftest 自己的信封行会被
/// 探针验过的守卫吞掉）。生产路径无人调。理由全文在 judge.cpp 定义处。
void clearCallLoggedForSelftest();
/// 回填最后一条调用的事后真相（note 子命令）。
int noteLastCall(const std::string& text);

// ═══════════════════════════════════════════ 子命令 / 自检（main.cpp 分发）

int cmdSelftest();
int cmdCheck();
int cmdRefuse(const std::string& name);
int cmdAttribution(const std::string& glb, const std::string& camera);
int cmdFaces(const std::string& glb, const std::string& piece, bool shadowPass,
    bool envOn);
int cmdSwitch(const std::string& glb, const std::string& camera, const std::string& allowPiece,
    bool envOn);
/// IZB 消费域（批 11.3 补账 B）：mask ↔ 画面**寻址一致性**三条子判据。定义在
/// `rig_shadow_field.cpp`（izb_rig 影场仪器族，与 `cmdSwim` 同一套 deferred + shadowPass 采集）。
int cmdIzb(const std::string& glb, const std::string& camera);
/// 影场世界不变量（同场景两机位、静态太阳 ⟹ 影场必须跟着世界走）：机位平移 `dx` 后重投影比影场，
/// 零位移失配 = 地板，搜索到的最优平移 = 世界位移的证据。定义在 `rig_shadow_field.cpp`。
int cmdSwim(const std::string& glb, const std::string& camera, double dx, bool izb, bool evssm);
/// 闭式解探针（12.3b 接替 analytic_ref.py --probe）：跑 qa/configs/analytic 固定配置
/// （llm_debug 截图模式，**非** RPC——那份配置本身是契约，六门逐项关掉写在注释里），
/// 对指定像素出「该是多少 vs 实际是多少」。l = 指向光源方向（不是 direction）。
int cmdProbe(const std::string& configDir, int x, int y, Vec3 albedo, double roughness,
    double metallic, Vec3 l, double intensity, int tol, bool lightOverride = false);

/// 时域阳性锚点（批 12.6 T4，docs/taa_plan.md §6）：细线靶（`aa_target`）+ **静止机位**，
/// 量「相邻帧逐像素差」在「只抖不滤（feedback=1）」与「滤波（feedback=0.12）」两态下的
/// 比值（另配 TAA 全关的确定性地板组）。三条硬判据，理由见 temporal.cpp 头注释。
/// 采集走 `/shotburst`（连续 N 帧）——用 `/shot` 逐张拍不出任何结论。
/// @param pairs 相邻帧对数（shots = pairs + 1；内部向上对齐到抖动周期 16 的整数倍）
/// @param warm 每次采集前的预热帧数（TAA 历史收敛，feedback=0.12 ⇒ ≈19 帧）
/// @param variant 渲染路线（默认 forward）
/// @param baseSpace 批 13.2 `taa.base_space`（true = 输出在**基空间**）。⚠ 门开态下
///                S1（只抖不滤）的逐帧差不再由游动主导 ⟹ **S1/S2 比值失去原口径**，
///                两态都要跑才能解释数字（见 temporal.cpp 头注释「基空间口径」段）。
int cmdTemporal(int pairs, int warm, const std::string& variant, bool baseSpace = false);

/// 时域**绝对**残差（批 13.1 主判据）：`temporal` 的四条钉的是相对抑制比（S1/S2 ≥ 3.0），**没有任何
/// 一条约束"收敛后逐帧残差的绝对大小"**（同一份输出里 TAA 开仍有 20.78% 像素逐帧变化、最大
/// 摆幅 0.535 线性 luma = 136 显示 LSB，四条全绿 = 判据盲区）。本命令在**同一张细线靶**上量
/// 「静止机位收敛后，同一绝对抖动相位上像素还会摆多少」：连拍折成 (2,16,H,W) 后逐像素
/// `swing = max_over_16 − median_over_16`，报 meanSwing / p99.9 / frac(swing>τ)，另加一条
/// 收敛正确性（线性域梯度比 vs 同场景 feedback=1 的一周期相位平均）。
/// 度量定义、三条测量红线（线性域 / 热身 ≥6/α 且不写盘 / 配置须带 environment 段）与三条锚点
/// 的完整理由 → `temporal_residual.h` 头注释。
/// @param periods 抖动周期数（≥2 的偶数；连拍 = periods×16 帧，两半同列 = 同绝对相位）
/// @param warmMult 热身倍数：每组 warm = ⌈warmMult / feedback⌉ 向上对齐到周期（EMA 判据 6/α）
/// @param variant 渲染路线（默认 forward）
/// @param slowFeedback 单调性轴的低端 α（默认 0.002 = 等效窗 ≈500 帧 ≫ 抖动周期 16）
/// @param baseSpace 批 13.2 门 `taa.base_space`（true = 输出在**基空间**）。本命令用它做
///                **归因**：门关读数 = 整幅游动 + 闪烁，门开读数 ≈ 只剩闪烁。
/// @param windowAbl 批 13.7 变体的**开关**（合成靶长短窗对照，`--long-feedback` 显式给出即
///                true = 进变体模式：跑「fb=1 参考 / 短窗 .12 / 长窗档 / 长窗档+js=0 反向」
///                四组，**只出数不判**——⑤ 的带是短窗口径、刻意设计成修复落地后应当变红，判它 =
///                把「变好」判成「不合格」；false = 现行 7 条判据，短窗口径不变）。
///                历史：本变体曾由 `--static-boost <地板>` 驱动（批 13.7），批 13.14b 删
///                static boost 后长窗档直接用 `feedback` 显式表达，开关并入 `--long-feedback`。
/// @param bistro 批 13.7b `--bistro`（债-48 主体）：换**交付配置 + 用户报障机位**（bistro 三机位，
///                坐标 = 本文件 kBistroCams 的仓库资产）重测短窗 vs 长窗绝对残差 + 债-47 的
///                **线性域**梯度比 + 逐像素 swing 热图。**只出数不判** + 两条量具守卫
///                （doorframe 短窗阳性锚点挂零 = 立刻停下；长窗 js=0 反向 = 恰为 0，非零时
///                由 N5「js=0 短窗」对照归因）。口径差异（全帧区域 / base_space 钉 true）
///                → temporal_residual.cpp 头注释。
/// @param camera 机位过滤（"all" = 三机位全跑；doorframe / wetground / doorpanel = 单跑）
/// @param longFeedback **长窗档**的 `taa.feedback`（默认 0.01 = 仓库拍板档；短窗档恒 = 0.12）。
///        历史：这一档曾藏在 `static_feedback` 里，批 13.14b 删 boost 后直接写 `feedback`。
///        批 13.7c：`--bistro` 每机位另出**亮度 × 梯度二维分带**（短窗 / 长窗 / 差三张表，
///        每格 share·rate·残余质量三数 + 阈值敏感性），判「残余闪烁住在高亮边还是普通边缘」；
///        口径与判决线（跑前写死）→ `temporal_residual.h` 同名段。
int cmdTemporalResidual(int periods, double warmMult, const std::string& variant,
    double slowFeedback, bool skipSlow, bool baseSpace = false, bool windowAbl = false,
    bool bistro = false, const std::string& camera = "all", double longFeedback = 0.01,
    bool diagBsOff = false, bool reject = false, bool clipAbl = false, double clipMode = 0.0,
    double clipSoft = 1.0);

/**
 * **相机运动下 TAA 到底抗了什么锯齿**（Phase 13 诊断判据；只出数 + 一句判决，不改渲染）。
 *
 * 与 `temporal-residual` 的分工：那条量的是**静止机位**的绝对残差（同相位折叠），
 * 本条量的是**运动机位**的交付图离「没有锯齿该长什么样」有多远——后者需要一个参考，
 * 而参考由**同机位序列的 2× 超采样渲染**给出（2560×1440 + TAA 关 → 线性域 2×2 面积
 * 平均降到 1280×720）。主指标 = 候选图与参考在**边缘像素**上的线性域偏差。
 *
 * ⚠ 本条**不是**反模式 `moving-camera-l1-ratio`（那条拒的是「运动序列逐帧 L1 之比」=
 * 把「相机真的动了」的运动项当成被测量）。本条的偏差是**逐帧对齐到同一视点**之后
 * 再量的（对齐 = 帧号纯函数 + 全帧 L1 argmin，见 temporal_motion.cpp 头注释），
 * 量的对象是「同一视点的两次不同估计差多少」，与逐帧 L1 不是一回事。
 *
 * 运动源（`--motion`）：
 *   camera（**默认**）= 相机帧驱动 yaw（camera.lua `frame_yaw`）——用户报障的就是相机在动；
 *   spin              = 场景帧驱动旋转（scene.lua `spin_frame_angle`，相机不动）——债-49
 *                        「静止机位 + 动物」的鬼影仪器，保留但**不是**本判据的默认口径。
 *
 * 组（每组一个引擎进程；只差被测项）：
 *   M0 TAA 关 + 运动   → **地板**（无 AA 的单帧光栅，锯齿全在）
 *   M1 TAA 开 + 运动   → **被测**（交付态：base_space=true 引擎默认）
 *   M2 M1 + base_space=false → 历史住抖动空间的对照（读数附「按闭式 c_t 去抖」修正）
 *   M3 重投影消融 —— **按运动源选门**：camera 模式 = `reprojection=false`（prevViewProj
 *      重投影本身被关，历史按同基位置采样）；spin 模式 = `motion_compensation=false`
 *      （prevSpin 补偿关）。⚠ 相机运动下 motion_compensation 是恒等阵 = 消融无效，
 *      故 camera 模式另跑 M3x = motion_compensation=false 作**恒等对照**（应 ≈ M1）。
 *   M4 TAA 开 + 静止   → 「有 AA 该长什么样」的参照（同配置、机位不动）
 *   REFm / REFs        → 上面两组运动/静止各自的 2560×1440 超采样参考（TAA 关）
 *   M1m / M4m          → M1/M4 + `diag_reject=true`，连拍 **taa_out 附件**取 alpha =
 *                        逐像素「历史怎么了」掩膜（0 收下 / 0.25 盒裁剪 / 0.5 盒拒收 /
 *                        0.75 重投影拒收 / 1 无历史）→ 历史被拒比例（RGB 逐位 = M1/M4）
 *
 * 判决口径（R = dev(M1)/dev(M0)，边缘带上）：R ≥ 0.9 ⟹ 运动下 TAA **等于没抗锯齿**；
 * R ≤ 0.7 且明显差于 M4 ⟹ 生效但受固有边界限制；中间 = 部分生效。**两种结论都照实说。**
 *
 * @param camera 机位（doorframe / wetground / doorpanel，坐标 = 仓库资产 kBistroCams）
 * @param motion 运动源："camera"（默认）或 "spin"
 * @param angleDeg 每帧角（度/帧，默认 0.2；0.2° ≈ 门框处 3–5px/帧）
 * @param burst 每组连拍帧数（/shotburst 逐帧连续，非逐张 /shot）
 * @param warm 运动组热身帧数（空转不写盘；运动格**不适用**「≥6/α 收敛」——历史本来
 *             就在被内容变化刷新，回显帧数即可）
 * @param warmStill M4 静止组热身帧数（≥6/α=50 对齐周期 ⟹ 默认 64）
 * @param edgePercentile 边缘掩码的梯度分位（默认 0.90 = 梯度最高的 10% 像素带）
 * @param longFeedback 批 13.11：**长窗档覆写**（>0 = 把 TAA 开组的 `taa.feedback` 覆写为本值，
 *        即 α 恒 = 本值）。spin 模式下相机静止 ⟹ 长窗全程生效 ⟹ 这正是 债-49 风险区（静止机位 +
 *        场景内运动 + 超长累积窗）。默认 0 = 不覆写 = 交付态 = 已发表 M0…M4 读数口径。
 *        历史：本开关曾驱动 `static_boost` 地板（批 13.11），批 13.14b 删 boost 后直接覆写 `feedback`。
 * @param clipMode / clipSoft 批 13.11 **历史裁剪消融臂**（`taa.clip_mode` / `taa.clip_soft`）。
 *        默认 0 = 不写键 = 交付态。用途 = 量「放宽盒要付多少鬼影」：同一命令、同一构建、同一
 *        激励，只换臂 ⟹ dev(M1)/dev(M0) 的跨臂差就是鬼影代价。阳性锚点 = **关盒（臂 1）必须
 *        显著变差**；若不变差，说明邻域盒在鬼影上**根本没承重**，那就不必为它保留闪烁代价。
 */
int cmdTemporalMotion(const std::string& camera, const std::string& motion, double angleDeg,
    int burst, int warm, int warmStill, double edgePercentile, double longFeedback = 0.0,
    double clipMode = 0.0, double clipSoft = 1.0);

/**
 * **运动态 feedback 扫描**（Phase 13 诊断，只出数 + 一句判决，不改引擎默认值）。
 *
 * 判的假设 = 「静止 + base_space 下历史采样偏移 ≈0 ⟹ CR 核退化为恒等 ⟹ 零低通；相机一动
 * 每帧都在非零分数偏移上被 CR 重采样 ⟹ 每帧一次低通、在 ≈1/α 窗内复利 ⟹ 越攒越糊」。
 * 可检验推论 = 边缘偏差随累积窗变长而恶化 ⟹ 运动态调大 `feedback`（= 本帧权重 = 缩短窗）
 * 应让它回落。fb=1 那一格（零累积但**仍带 jitter**）把「累积」与「抖动」两个贡献拆开，
 * 同时是判决 ②/③ 的分界，不许跳过。顺带在静止态扫 fb 出 swing，判「窗棂残余与运动失效
 * 是否同一个根」。口径（分辨率 / 连拍 / 线性域 / 交付配置 / 边缘带 / τ）与
 * `temporal-motion` 逐条同源，全量理由 = `tools/imgqa/temporal_motion.cpp` 本命令的头注释。
 *
 * @param camera 机位（doorframe / wetground / doorpanel）
 * @param angleDeg 每帧角（度/帧，默认 0.2 = 与 temporal-motion 同一激励）
 * @param burst 每组连拍帧数（≥32 且为抖动周期 16 的整数倍：静止组要相位折叠）
 * @param edgePercentile 边缘掩码的梯度分位（默认 0.90）
 * @param fbs feedback 扫描档（默认 0.02/0.05/0.12/0.25/0.5/0.8/1.0）
 * @param kernel 批 13.8 `taa.input_kernel`（输入侧重建核：0 双线性 / 1 CR 4×4 / 2 Lanczos3）。
 *        只作用于 TAA 开组；M0/REFm/REFs/S0（= 静止 TAA 关地板，本命令为「抖动自身代价
 *        是不是运动专属」这个问题新增）都是 TAA 关 = 不消费本键 ⟹ 它们跨核可比。
 */
int cmdTemporalFeedback(const std::string& camera, double angleDeg, int burst,
    double edgePercentile, const std::vector<double>& fbs, double kernel = 0.0);

/**
 * 批 13.13 **鬼影尾长直测**（债-63 的出口）：静止机位 + 场景 spin，参考 = 同一次 spin 的
 * 「TAA 关 + jitter_scale=0」渲染（基位置真值），配对 = 引擎帧计数器之差（**零图像配准**）。
 * 主指标 = 归一化鬼影份额 `frac = mean|out−ref| ÷ mean|N−S|`（无量纲；关盒时闭式 = 1 − α）。
 * 四条判决线跑前写死（pairing / events / box-load / monotone），红一条即不许报臂间结论。
 * 完整设计与判据出处 → `tools/imgqa/temporal_motion.cpp` 本命令头注释。
 * @param angleDeg 场景 spin 角（度/帧，`spin_frame_angle` 纯函数）
 * @param maxLag   衰减曲线长度 K（同时是「让开后内容必须稳住」的窗长）
 * @param tau      「让开」阈值（线性 luma，唯一自由参数 ⟹ 输出必带三档敏感性表）
 * @param margin   共用内部窗口的两端余量（帧）。窗口 = `[maxLag+margin, n-1-maxLag-margin]`，
 *        **与帧号偏移无关** ⟹ 各臂评在逐位相同的事件集上。偏移覆盖不到 ⟹ 抛错（不静默裁帧）。
 */
int cmdTemporalTail(const std::string& camera, double angleDeg, int burst, int warm, int maxLag,
    double tau, int margin);

/**
 * 批 13.14a **相机运动版尾长**（`temporal-tail --motion camera`）：机位匀速 yaw 下的
 * 稳态鬼影份额。存在的理由不是「给 spin 版补对称性」——spin 下相机不动 ⟹ 臂 11 的
 * `gate = saturate(2 − motion)` **恒 1.0** ⟹ 现有仪器原理上进不了 gate 的斜坡区，
 * 而批 13.14 把 α 恒写 0.01 的全部赌注正压在这个门上（→ `phase13_plan.md` §1.16）。
 *
 * 分子 = `|out_j(p) − ref_j(p)|`（同帧同视点，**不需要配准**）；
 * 分母 = `Δ_j(p) = |ref_j(p) − ref_{j−1}(φ(p))|`，φ = **解析** yaw 映射（带 pitch，
 * `temporal_yaw_remap`）⟹ 主指标从「尾长」退化为「稳态份额」`frac = Δ加权残差 ÷ Δ`。
 * 闭式锚点跟着换：一阶稳态运动模糊 = `1 − α`。
 *
 * @param angleDeg      每帧 yaw 增量（度/帧，`camera.lua frame_yaw` 同语义）
 * @param gateAngleDeg  gate 断点的高档角度（度/帧）；**0 = 跳过判决线 4**（只跑前三条）
 * @param margin        共用内部窗口两端余量（帧）；⚠ 另需 ≥ 每帧位移的边界内缩
 */
int cmdTemporalTailCamera(const std::string& camera, double angleDeg, double gateAngleDeg,
    int burst, int warm, double tau, int margin);

/**
 * 批 13.15 **平移阶跃鬼影衰减**（`temporal-tail --motion step`，债-63 的真鬼影量具）。
 *
 * 存在的理由 = 前两台仪器**结构上量不到鬼影**：`temporal-residual` 的机位在整组里钉死
 * （唯一运动是抖动），`--motion camera` 是原地纯旋转 ⟹ 两者都**无视差、无去遮挡**，
 * 而鬼影的定义就是「让开的内容回不来」（→ `phase13_plan.md` §1.17 结论 6）。
 *
 * 协议（每臂三跑）：① 在机位 A 收敛到历史稳定 → ② 同机位连拍 = `A` 参考 →
 * ③ 在机位 B 收敛 = `B∞` 参考 → ④ 在 A 热身后**中途 `POST /camera` 瞬切到 B** 再连拍。
 * 主指标 = 掩膜上的**归一化残影衰减曲线** `ghost(n) = mean|f_n − B∞| ÷ mean|A − B∞|` 的
 * **AUC = Σ ghost(n)**（单位「帧·全对比度」）。🔴 **不是 n₅₀**：盒生效时 `ghost(0)` 第一帧就掉到
 * 0.02 量级 ⟹ 曲线永远碰不到 0.5 ⟹ n₅₀ 对带盒的臂**不存在**（恒 −1）。
 *
 * 🔴 闭式锚**只存在于 E 臂**（关盒 + **重投影关**），两个条件缺一不可（两条都是本轮实测踩的）：
 *  1. `clip_mode = 0` **不是「关盒」**——它带默认那条 5 抽头十字盒（关盒是臂 1）。拿臂 0 要
 *     `ghost(0) = 1−α`，实测 0.012~0.019，且**不随平移步长变**（阈值型失效会看到转变）。
 *  2. 关盒但**重投影开着**时闭式仍不成立：0.01 世界单位 ≈ 12 屏幕像素，重投影把历史搬到
 *     `p+12px` 采样 ⟹ 对非去遮挡像素 `A(p+12px) ≈ B(p)` 恰好成立 ⟹ 那部分变化被"解释掉"
 *     ⟹ `ghost(0)` 只有 0.271（= **去遮挡份额**，本身是个好数，但不是 EMA 闭式）。
 *  E 臂下 `hist(p) ≡ out_{n−1}(p)` 精确 ⟹ `ghost(n) = (1−α)ⁿ` ⟹ `AUC = (1−(1−α)^N)/α`。
 *  实测 47.050 vs 闭式 47.440（**−0.8%**），整条曲线 1.2% 内命中。
 *
 * 六臂 = Z（α=1 零锚）/ E（闭式锚）/ R（关盒 + 重投影开 = 重投影单独贡献）/ C0（现行盒）/
 * C11（交付）/ C13（批 13.15b 修法臂：α 也吃 gate）。
 *
 * 两条与其它时域仪器不同的口径，都是刻意的（理由写在 .cpp 头注释）：
 * **`jitter_scale` 恒 0**（换成「静止机位 ⟹ 收敛帧逐位相同」这条更强的零点），
 * 以及配对 = **我自己写的位姿序列**（零图像配准，同设计红线）。
 *
 * @param stepDist  A→B 的**平移**距离（世界单位，沿相机 right 轴纯横移；0 视同默认）。
 *        ⚠ 上限受引擎跳切判据 `cameraJumps` 管：位移 > 0.25·sceneRadius ⟹ 历史直接作废
 *        ⟹ 残影恒 0 = 假绿。该风险由判决线「激励在场」当场抓（见 `temporal-tail-step-stimulus`）。
 * @param burst     阶跃后连拍帧数（决定 AUC 的积分窗；E 臂闭式随 N 变，判决线按实际 N 算）
 * @param warm      收敛帧数下限（0 = 只用 `⌈6/α⌉` 的 EMA 判据）。🔴 语义 = **取大**不是覆写：
 *        `--warm` 默认 64 是三个 `--motion` 模式共用的，「> 0 即覆写」会让 α=0.01 的臂**永远**
 *        只热身 64 帧（EMA 判据要 600）。伪装极好——抖动关断下静止机位收敛帧是不动点，
 *        参考图完全正确，只有「历史攒了多久」被悄悄改了。两者均向上对齐到抖动周期。
 * @param tau       掩膜阈值（线性 luma，唯一自由参数 ⟹ 输出自带三档敏感性表）
 * @param margin    掩膜**向内腐蚀**的像素数（排掉「新内容从画外入画」，那不是鬼影）
 */
int cmdTemporalTailStep(const std::string& camera, double stepDist, int burst, int warm,
    double tau, int margin, bool fsr = false);

/// 逐杆覆盖率对拍闭式（批 12.6 T4，docs/taa_plan.md §7 第 10 条）：单帧量阶梯杆的
/// 「黑度积分 vs 投影宽度」，TAA 关应当被二值覆盖量化破坏、TAA 开应当贴合闭式。
/// 与采集时序无关（这是它优于逐帧差的地方）。
/// @param variant 渲染路线（默认 forward）
/// @param warm    预热帧数（feedback=0.88 的时间常数 ≈8.3 帧，预热不足会把初始黑历史
///                的残差算成覆盖率——判收敛必须靠调这个参数做对照，见 plan §7 第 10 条）
/// @param feedback scene.lua `taa.feedback`（默认 0.12）。**消融用**：=1 时输出 = 本帧
///                ⇒ TAA 开应与关**逐位相同**（一条免费的恒等式检查，见 plan §7 第 11 条）
/// @param baseSpace 批 13.2 `taa.base_space`（true = 输出在基空间）。判据域内的黑度积分
///                是**单帧**量，门开态把它从「抖动空间的一帧」换成「重采样到基网格的一帧」，
///                数字会动 ⟹ 两态都要跑（门关 = 0.138 列的历史读数口径）。
/// @param kernel  批 13.8 `taa.input_kernel`（输入侧重建核：0 双线性 / 1 CR 4×4 / 2 Lanczos3）。
///                本判据是**静止态那条有闭式的**判据（黑度积分 = 闭式投影宽度），批 13.2 当年
///                就是拿它否掉 CR 的；批 13.8 拿它做「运动态新判据 ζ 选出的核在静止态是否
///                退化」的**第二条闭式判据**（不许只看新判据换核）。TAA 关组不消费本键。
int cmdAaCoverage(const std::string& variant, int warm, double feedback,
    bool baseSpace = false, double kernel = 0.0);

/// AO 噪声量测（Phase 13 观测任务：把「SSAO 到底噪不噪」变成有数字有图有判据）。
///
/// **闭式锚点（期望值出处独立于实现）**：AGENTS.md 已登记的契约「开阔地面 AO 恒 1.000」
/// ——远离遮挡物的大平面上，depth-only SSAO 没有任何理由遮蔽（半径 0.5 m 内无遮挡候选，
/// 且平面自遮蔽被 kDepthBias=0.03 挡掉）。AO 附件是 r8_unorm 的**可见度**，契约值即字节
/// 255 ⟹ **契约区的偏离量就是噪声本身**，零容差、不需要基线图像。
///
/// 四组测量（编号与交付报告一致）：
///   ① A/B 空间噪声：同机位 `ao.strength` 0 vs 0.5，平坦区偏离量 + 颗粒度（拉普拉斯 RMS）
///   ② AO 场直方图 / 剖面：取值分布 + 穿过接触区的扫描线
///   ③ 亚像素爬行：机位 yaw 整 1 像素（哈希只吃 SV_Position ⟹ 噪声贴屏幕不贴世界）
///   ④ 时域：TAA 开态 `/shotburst` 连拍逐帧差，AO 0 vs 0.5 各一轮（平坦区 AO 关是**恰为 0**
///      的地板，故 AO 开的读数无需比值即可归因）
/// 外加掠射机位（候选根因 B3/B4 的角度判别：法线与核尺寸都来自屏幕空间导数）。
///
/// @param glb 程序化靶（默认 cornell_box.glb：纯色 + 无 TEXCOORD ⟹ 底图零空间高频）
/// @param strength 被测 AO 强度（交付配置 = 0.5）
/// @param radius 平坦区排除半径（像素）：距任何几何轮廓小于该值的像素，AO 有**合法**梯度，
///               不得进契约区（阈值出处：AO 半径 0.5 m ÷ 该处米每像素 ≈ 31 px + 余量）
/// @param pairs 时域组的相邻帧对数（向上对齐到抖动周期 16）
/// @param doTemporal false = 跳过④（只跑静态三组，调试用）
int cmdAoNoise(const std::string& glb, const std::string& camera, const std::string& variant,
    double strength, int radius, int pairs, bool doTemporal, bool doBistro);

/// 真实场景 TAA **游动**量测（批 13.1b，phase13 §1.6 末的未定案）。
/// 不问「逐帧差大不大」（那条路已被 `screen-space-l1-frame-diff` 登记为反模式），
/// 改问一个有唯一答案的几何问题：**逐帧差能不能被一个全局亚像素位移几乎完全解释？**
/// 只有「整幅平移」会让对齐后残差塌到近零；「逐像素独立闪烁」（历史被拒 ⟹ 输出退化成
/// 单帧的抖动采样）对齐后残差**不降**。两种解释在此分道，且不需要任何参考图。
/// 三组：T0 TAA 关（地板）· T1 TAA 开 feedback=1（**量具自身的阳性锚点**：按构造必须
/// 整幅平移，且位移须命中 Halton 闭式预测）· T2 TAA 开 feedback=.12（被测量）。
/// 批 13.2 追加两组：**T5** = T2 + `taa.base_space`（门开态，判据 `temporal-base-space`
/// 的主体，阳性锚点 = 同一次运行里的 T2 门关读数仍等于闭式抖动差）；**T6** = T5 的
/// feedback=1 版（无累积，逐帧差应塌到「重光栅化覆盖翻转」地板，用来把「重采样本身
/// 引入的帧间差」与「游动」分开）。
/// @param pairs 相邻帧对数（内部向上对齐到抖动周期 16 的整数倍）
/// @param warm  预热帧数（历史收敛）
/// @param variant 渲染路线（默认 forward）
int cmdTaaSwim(int pairs, int warm, const std::string& variant);

// ═══════════════════════════════════════════ 软影滤波轴量具（批 N6d 步 0）
//
// 判据设计定案 = `docs/shadow_routes_plan.md` §5.1，量具边界（为什么不是 shadow_rig）=
// 同文件 §4.1。此处只放**度量层**的声明；口径为什么长这样逐条写在 shadow_softness.cpp 头注释。

/// 一臂相对 `none` 参照的差分统计（§5.1 度量口径）。
/// ⚠ **比较域 = 存储字节域的 Rec.709 加权和**（不是 `Frame::luma` 的线性域）——理由见
///   `byteLuma` 注释：阈值 8/40 是 LSB 量，换域会让同一个数在暗部变成完全不同的判据。
struct SoftArmMetrics {
    double coverage = 0.0;   ///< 影覆盖率 = frac(Δ > 8 LSB)
    double meanDark = 0.0;   ///< 平均变暗 = mean(max(Δ, 0))，LSB
    double band = 0.0;       ///< 过渡带占比 = frac(8 < Δ ≤ 40)
    double core = 0.0;       ///< 深本影占比 = frac(Δ > 40)
    double bandCore = 0.0;   ///< 带核比 = band/core（core=0 时为 NaN，由调用侧显式处理）
};

/// 字节域 luma（0.2126/0.7152/0.0722 直接吃 8bit 字节，不做 sRGB 解码）。
/// 为什么不用现成的 `Frame::luma`：那条是 sRGB 解码后的**线性**亮度（比较域单点在
/// image.cpp），而本判据的阈值是**字节 LSB**（8/40）——线性域里 8 LSB 在暗部对应
/// 0.0004 的相对量、在亮部对应 0.03，同一个阈值会变成"只量亮部"的判据。
/// §4/§5 的全部现状数（9.84% / 0.14% / 1.955%）都在字节域，换域 = 换判据。
[[nodiscard]] cv::Mat byteLuma(const cv::Mat& bgr);

/// 一臂 vs `none` 参照的四数（两图尺寸必须相同，不同即抛）。
[[nodiscard]] SoftArmMetrics measureSoftArm(const cv::Mat& refLuma, const cv::Mat& armLuma);

/// 太阳角敏感度 = frac(|L_a − L_b| > 8 LSB)（§5.1 的 J1/J2 度量本体）。
/// 与"覆盖率之差"的差别：它不看参照臂、只看两臂**逐像素**是否翻转，因此不会被
/// "两边一起丢影"抵消（覆盖率之差在两条都丢影的档上是 0，看着像"无响应"）。
[[nodiscard]] double sunSensitivity(const cv::Mat& lumaA, const cv::Mat& lumaB);

/// 软影滤波轴量具（交付配置，五臂六跑）。**无 flag**：场景/臂/帧号/阈值都是量具的
/// 组成部分，能调 = 能调绿（§5.1「阈值一旦写死就不许为了让它绿回头改」）。
/// 三条判决线：J1 `shadow-softness-gauge-valid`（量具有效，必须绿）·
/// J2 `shadow-softness-sun-response`（现状缺陷，**现在必须红**）·
/// J3 `shadow-softness-vs-csm`（对账出数，advisory）。
int cmdShadowSoftness();

// ═══════════════════════════════════════════ CSM PCF 核宽**单位**护栏（shadow-kernel-scale）
//
// 缺陷本体 = `rgl/shaders/shadow.slang` ⑥ 把世界米/纹素的 `texel` 当归一化图集 uv 偏移用
// （误差 = 4r，随级联半径线性放大 ⟹ 大场景下阴影整体消失）。判据本体与全部口径理由
// = `tools/imgqa/shadow_kernel_scale.cpp` 头注释；此处只放**度量层**声明。

/// 扫描线上一个采样点。s = 沿扫描方向的**世界米**（不是像素！宽度因此直接以米计）；
/// value = 阴影因子 `S = (L_ref − L_arm) / L_ref`（env/ambient/ao 全 0 时与 albedo 无关）。
/// `valid = false` = 投出屏外，或参照黑到取不出比值（分母 < 1 LSB）。
struct ShadowScanSample {
    double s = 0.0;
    double value = 0.0;
    bool valid = false;
};

/// S 在窗口 [lo, hi] 内**第一次**穿过 thr 的 s（样本间线性插值）；取不到 = NaN。
/// 为什么是「第一个」而不是「最后一个」：下游缘是扫描方向上唯一该有半影的地方，而窗口内
/// 可能先撞上板体轮廓（板把自己的上游影段挡住了）——取最后一个会把轮廓当成影缘。
[[nodiscard]] double shadowCrossing(const std::vector<ShadowScanSample>& pr, double thr,
    double lo, double hi);

/// 半影宽度 = |s(90%) − s(10%)|（米）。任一交叉取不到 = **NaN 而不是 0**：
/// 「没有影」与「影缘无限硬」是两种相反的失效，混成 0 会让缺陷态看起来像满分。
[[nodiscard]] double shadowPenumbraWidth(const std::vector<ShadowScanSample>& pr, double lo,
    double hi);

/// 影覆盖率 = frac(L_ref ≥ 1 LSB 且 (L_ref − L_arm)/L_ref > 0.5)，分母 = **全幅**像素。
/// 参照下限不是可有可无：分母接近 0 时 S 会爆到 1，背景（env/skybox 全关 = 黑）会被
/// 整片算成「有影」——那正是本判据要抓的反面。
[[nodiscard]] double shadowUmbraFraction(const cv::Mat& refLuma, const cv::Mat& armLuma);

/// CSM PCF 核宽单位护栏（靶 = 程序化 `penumbra_ladder`，三臂 none/csm/pcss5）。
/// **无 flag**：场景 / 臂 / 帧号 / 阈值都是量具的组成部分，能调 = 能调绿。
/// 三条判决线：`shadow-csm-umbra-present`（CSM 必须有影）·
/// `shadow-csm-flat-in-gap`（CSM 的 W 对间隙 h 必须平）·
/// `shadow-target-resolves-gap`（**阳性锚点**：pcss5 的 W 必须随 h 涨，它红时第二条的绿不作数）。
/// 缺靶（.glb / .manifest.json）= 抛，**不 SKIP**。
int cmdShadowKernelScale();

// ═══════════════════════════════════════════ 结构差分（diffstruct，Phase 15 批 15.2 子步 D1）
//
// 判据本体（拍板 → docs/phase15_plan.md §2 批次表 15.2 行）= 湿面/SSR「反射结构的对比度」：
// 纯 Δ 图（A−B，**线性亮度域**）上的 p90/p10 比值 + 湿区内 Δ<0（变暗）像素数 > 0。
// 本算子**场景无关**：输入 = 磁盘两张 PNG，不启引擎、不读 manifest（bistro 没有 manifest）。
// 口径与一次性参考实现的对账全部写在 `diff_struct_metric.h` 头注释，此处不重述。

/**
 * 结构差分统计。A = 实验档、B = 基线档（方向反了 ⟹ 变暗计数读成变亮，不报错但结论相反）。
 *
 * 两条硬判据（其余全部是报告值）：
 *  · `diffstruct-dark-domain` —— 仅 gbufPath 非空时求值：变暗 ∧ roughness ≥ gate **非零即 FAIL**
 *  · `diffstruct-dark-anchor` —— 仅 expectDark 时求值：Δ<0 计数 > 0（SSR 开/关档对用；
 *    参数臂对不给该旗标 = 纯报告，因为参数扰动本来可以只变亮）
 *
 * @param gbufPath dump_gbuf=3 的 metalrough 出图（.g = UNORM8 roughness）；空 = 不做域交叉
 * @param gate     干/湿 roughness 门控（g/255 口径），过门控 = `0 < rough < gate`（g=0 天空除外）
 * @param minDelta 差分集合门槛 |Δ| > 此值（默认 1e-6 与「字节有任何不同」逐位等量，理由 → 头注释）
 */
int cmdDiffStruct(const std::string& aPath, const std::string& bPath,
    const std::string& gbufPath, double gate, bool expectDark, double minDelta);

// ═══════════════════════════════════════════ 差异形态判据（diffblock，★0.5-b1）
//
// 判据本体（拍板 → docs/mdi_plan.md §11.2）= 「UV 缝边缘有差异我能接受，整块不行」的形式化：
// M = {|luma(A)−luma(B)| ≥ τ}（线性亮度），U = area(erode(M, 圆盘半径 r))，判据 U == 0
// ⟺ 差异处处厚度 ≤ 2r 像素。**场景无关**（同 diffstruct）：输入 = 磁盘两张 PNG，不启引擎。
// 口径常量与 erode↔distanceTransform 等价性声明 → `diff_block_metric.h` 头注释，此处不重述。

/**
 * 形态判据。退出码 PASS（U(r)=0）= 0、FAIL = 1（进 just 脚本当闸门）。
 * 报告必须同时给三样（§11.2，阈值不许藏）：厚度分位 / U(r) 曲线（r=0,1,2,4）/ 连通域清单；
 * τ=0（任一位不同）的计数与 τ 的计数同时报（两数之差 = 交付颗粒噪声 vs 真差异）。
 *
 * @param radius   腐蚀半径 r（像素，默认 2 = 「处处 ≤ 4px 厚」）；**半径不是直径**
 * @param tau      M 的门槛，单位 = 1 个 sRGB 量化步长（默认 1；0 = 任一位不同，严格 >0）
 * @param top      连通域清单条数（面积降序；0 = 不打印）
 * @param jsonPath 机器可读输出（★1-a1ii 验收脚本读它）；空 = 不出
 */
int cmdDiffBlock(const std::string& aPath, const std::string& bPath, int radius, double tau,
    int top, const std::string& jsonPath);

}  // namespace RGL::imgqa

#endif
