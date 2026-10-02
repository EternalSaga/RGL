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
    /// 拍一张。attachment=nullptr = 后缓冲；否则具名附件（"object_id"）。返回 PNG 绝对路径。
    [[nodiscard]] std::string shot(const std::string& tag, const char* attachment = nullptr);

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

}  // namespace RGL::imgqa

#endif
