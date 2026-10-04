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
// 回归要求同会话双构建，跨日就有 ±1 LSB 叶缘噪声（docs/调试历史.md 2026-09-08）。
// 本工具做的是另一件事：**判据的区域来自几何，不来自图像。** 引擎能给出逐像素
// objectID，于是问题从"图像变了吗"变成"对 piece X，落在它那些像素上的量 Q 是多少"。
// 失败信息直接点名"anchor_cube_near 的某簇像素线性亮度为 0，闭式解预测 0.239"，
// 而不是"SSIM 0.93"。
//
// ## 生命周期（三步，与用户商定）
//
// 1. **现在**：工具箱 + 判据（promoted=false），我在真实排查里主动调用。
// 2. **打磨**：每次真实调用留一条记录（qa/log/imgqa_calls.jsonl），事后回填真相。
//    "打磨"的可检验含义 = 记录里不再出现"判据说不出理由"或"为它加了个新 flag"。
// 3. **晋升**：判据同时满足四条才 promoted=true，进 `imgqa check --promoted`：
//    ① 至少一个真实 bug 或真实变异上正确变红过；② 跨 ≥20 次无关改动从未误报；
//    ③ 容差写得出**理由**（不是"0.5%"，是"float32 多项式 + 8bit 取整，实测 2 LSB"）；
//    ④ 有反向用例。**判错的测试比没有测试更糟**——它教会未来的你把世界读成正常。
//
// ## 三条硬约束（都来自踩过的坑）
//
// 1. **绝不许对"该机位下其实看不见的几何"下断言**——先由 objectID 反解可见集
//    （tools/qa_scenes.py 注释里付过学费："按预测边界定点 = 会绿 = 无效断言"）。
// 2. **统计一律在线性域做**；比值在 sRGB 字节域算出来是错的数。
// 3. **比较域必须是"观测域"**：观测像素是 曝光→ACES→sRGB store 之后的字节；闭式解
//    是 pre-tonemap 线性值。ACES 有通道串扰，两个域之间没有比例关系可谈——预测值
//    必须先走 toBytes 再 sRGB 解回来（predictFaces 里做），才能与像素同域比较。

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
///   ② 键名对着**引擎解析器源码**校验（`shadowPass` 是 C++ 字段名，lua 键是
///      `shadow_pass`——拼错键 = 静默用默认值 = 在默认值上下判据，拒绝）。
struct ProfileOpts {
    /// **交付配置模式**（批 13.1e）：非空 = 以该目录（仓库 `configs/`）里的 *.lua / *.json
    /// 为基底**整份拷贝**，之后只覆写三处：`runtime.lua` 的 headless、`diagnostics.lua` 的
    /// rpc、`scene.lua` 的 taa 块（enabled / feedback / jitter_scale）。其余一律不动。
    ///
    /// 为什么必须有这条：判据跑在**合成 profile** 上得出的绿不算绿。`temporal-identity`
    /// 就是这么假绿了一整轮——合成 profile + 默认机位上残差 0.016/255（绿），同一判据换到
    /// 交付配置 + 用户机位上是 **mad 1.688/255、逐像素最大 37、10.37% 像素 >2LSB**（红）。
    /// 差异不是噪声而是**另一个场景**：交付配置是 `variant = "deferred"` + `scale = 0.01`
    /// + izb_shadow + env/skybox/AO 全开，合成 profile 是 forward + 默认机位。
    /// ⚠ 本模式生效时 `glb` / `envIntensity` / `skybox` / `aoStrength` / `shadowPass` /
    ///   `lightDir` / `lightIntensity` / `exposure` / `picking` **全部被忽略**（真相在交付
    ///   配置里，工具再抄一份就是第二份真相）；`variant` 留空 = 尊重交付配置自己写的路线。
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
    bool shadowPass = false;
    bool izbShadow = false;                // diagnostics.izb_shadow（izb 消费域判据）
    bool evssmShadow = false;              // diagnostics.shadow_evssm（批 11.6d swim 判据）
    int izbDumpFrame = 0;                  // diagnostics.izb_dump_frame（>0：N 帧起每 30 帧覆盖写 izb_mask.ppm）
    std::string variant;                   // "" = 模板默认（forward）；"deferred" 等 = 拷贝后重写 variant 行
    bool taa = false;                      // scene.lua `taa.enabled`（**图形态**开关，批 12.6）
    double taaFeedback = 0.12;             // scene.lua `taa.feedback`（= 本帧权重；**1** = 只抖不滤 = 消融组；0 = 「只要历史」的退化环，勿用）
    double taaJitterScale = 1.0;           // scene.lua `taa.jitter_scale`（0 = 只留时域累积，去抖动）
    /// 批 13.2：scene.lua `taa.base_space`（true = 最终消费处按内容偏移重采样，把输出从
    /// **抖动空间**搬回**基空间**）。false = 现行为（输出在抖动空间，游动 = 相邻帧抖动差）。
    /// ⚠ 本工具所有「游动 = 抖动差」的判据（`temporal-swim-pixels` / `temporal-jitter-*`）
    ///   钉的都是**门关**口径；门开态由 `temporal-base-space` 单独判（见其容差理由）。
    bool taaBaseSpace = false;
    std::string entitiesLua;               // 非空 = 整条 entities 行由它给（内建合成靶无 glb 文件）
    std::array<double, 3> lightDir{0.5, -0.75, -0.45};
    double lightIntensity = 12.0;
    bool picking = true;                   // object_id 附件 = 区域归因的根基
    std::array<int, 2> size{1280, 720};
};
std::string makeProfile(const std::string& outDir, const ProfileOpts& opts);

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
    void setCamera(const Camera& cam);
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
    /// 拍一张。attachment=nullptr = 后缓冲；否则具名附件（"object_id"）。返回 PNG 绝对路径。
    [[nodiscard]] std::string shot(const std::string& tag, const char* attachment = nullptr);

    /// 连续采 n 帧（POST /shotburst）——**时域差分唯一合法的采集口**。
    /// 与 shot() 的区别不是「省几次往返」：`/shot` 每张落在哪一帧取决于 HTTP 请求与
    /// 帧首钩子的先后（= 墙钟），相邻两张根本不是相邻两帧，逐帧差于是测的是随机相位窗
    /// （实测同一个量随预热长度差 3 倍）。`/shotburst` 的「下一张」由引擎在帧首结算
    /// 钩子里自己登记（该钩子先于 drawFrame）⇒ 采集落在连续的第 N..N+n-1 帧上。
    /// @param dir 引擎 CWD（build/rgl）下的输出目录
    /// @param n 张数（≥1）
    /// @param prefix 文件名前缀（引擎写 `<prefix>%03d.png`）
    /// @return 引擎回显的绝对路径，顺序 = 帧序
    [[nodiscard]] std::vector<std::string> shotBurst(const std::string& dir, uint32_t n,
        const std::string& prefix = "f");

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

/// 一维间隙聚类（同材质各面色度同向、只差 NdotL ⟹ 一维足够；不用 k-means：k 未知，
/// 而"簇 = 亮度直方图里的分离"在此是物理事实——每个可见面一个亮度）。
[[nodiscard]] std::vector<std::pair<double, int64_t>> lumaClusters(cv::InputArray luma,
    double gap = 0.02, double minFraction = 0.02);

// ═══════════════════════════════════════════ 判据 + 反模式 + 台账

struct Verdict {
    std::string name;
    bool ok = false;
    std::string observed;
    std::string whyTol;                  // 必须写**理由**（晋升条件③）
    std::vector<std::string> guards;     // 台账：钉住调试历史里的哪一条
    bool promoted = false;
    [[nodiscard]] std::string line() const;
};

/// 判据元信息注册表（check 子命令打印用；求值函数在 commands.cpp 直接调用）。
struct JudgmentInfo {
    const char* name;
    const char* whyTol;
    std::vector<std::string> guards;
    bool promoted;
};
[[nodiscard]] const std::vector<JudgmentInfo>& registeredJudgments();

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

/// 反模式登记（负面知识也是工具的一部分）：装一条但**拒绝执行**，比不装更好——
/// 否则下一个会话的我（或别人）会重新发明它。拒绝比缺失更能传达判断。
[[nodiscard]] const std::map<std::string, std::string>& antiPatterns();

// ── 调用记录（"打磨"的计量单位；晋升条件①②的计数来源）──
void logCall(const std::string& cmd, const std::vector<std::pair<std::string, std::string>>& args,
    const std::vector<Verdict>& verdicts);
/// 回填最后一条调用的事后真相（note 子命令）。
int noteLastCall(const std::string& text);

// ═══════════════════════════════════════════ 子命令 / 自检（main.cpp 分发）

int cmdSelftest();
int cmdCheck(bool onlyPromoted);
int cmdRefuse(const std::string& name);
int cmdAttribution(const std::string& glb, const std::string& camera);
int cmdFaces(const std::string& glb, const std::string& piece, bool shadowPass,
    bool envOn);
int cmdSwitch(const std::string& glb, const std::string& camera, const std::string& allowPiece,
    bool envOn);
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
int cmdAaCoverage(const std::string& variant, int warm, double feedback,
    bool baseSpace = false);

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

}  // namespace RGL::imgqa

#endif
