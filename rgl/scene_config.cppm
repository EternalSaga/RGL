module;
#include <glm/glm.hpp>  // SceneEntityDesc 成员 glm::vec3 需名可见（同 render_list.cppm 先例）
export module scene_config;

import rlstd;
import vkerror;
import rllogger;
import scene_context;

namespace RGL {

// Lua vec3 读取（可选字段回落默认值，同 camera.cppm loadCameraConfig 惯例）：{x,y,z} → glm::vec3
// readVec3 + 配置加载定义随 scene_config_impl.cppm 迁出（本接口只留类型与声明）

/**
 * 一条 pipeline 规格：shader 风格的 SPV 路径（由 configs/scene.lua 传入，
 * 取代原 vkrenderer.cppm 里写死的 SPV 文件名）。
 */
export struct PipelineSpec {
	std::string name;    // "cube" / "model"
	std::string meshSpv; // mesh shader SPV 相对路径
	std::string fragSpv; // fragment shader SPV 相对路径
	/** task stage 的 SPV 相对路径（★M-3T-a，docs/mdi_plan.md §19.16）。
	 *  🔴 **空 = 无 task stage**（= 改前形态：两条 createInfo + `NO_TASK_SHADER_BIT`）。
	 *  本字段是聚合**末尾**追加 + 带默认成员初始化器 ⟹ 既有全部三元素 brace-init 点不到它。
	 *  🔴 `= {}` 不是可选写法：本构建开 `-Werror,-Wmissing-field-initializers`，不带 DMI 则
	 *  20 处三元素 brace-init 当场编不过（实测；带 DMI 后该规则不对"有默认值的成员"出声）。 */
	std::string taskSpv = {};
};

/**
 * 环境贴图（Phase 5.5：scene.lua environment 段，可选缺省 = 不启用）。faces 按
 * **Vulkan 层序** 0-5 = (+X,−X,+Y,−Y,+Z,−Z) 排列（素材面顺序定案表 =
 * docs/archive/phase5_5_plan.md 素材定案；nightSky 层序 1,3,5,6,2,4 / rainbow 方案 B
 * ft,bk,up,dn,rt,lf），完整路径 = dir + "/" + faces[i]。intensity 进 scene data
 * @48（env.slang readEnvParams）；skybox = 背景天穹开关（批次 4 消费）。
 *
 * Phase 8 批次 ②b：`hdr` 非空 = 走 Radiance .hdr 环境路径（image_codec::decodeHdr →
 * CPU 等距柱状→cubemap 重投影 → RGBA16F 上传），**优先于** dir/faces 六面 PNG 路径；
 * .hdr 文件缺失时回落 PNG 路径（素材 gitignore，干净 checkout 仍可运行）。
 * hdrSize = 重投影面边长（2 的幂，默认 2048）。
 */
export struct EnvironmentConfig {
	bool enabled = false;
	std::string dir;
	std::array<std::string, 6> faces;
	float intensity = 1.0f;
	bool skybox = true;
	std::string hdr;      // Phase 8 ②b：.hdr 环境路径（非空优先于 faces）
	uint32_t hdrSize = 2048;  // 重投影面边长（2 的幂）
};

/**
 * 场景 entity 声明（Phase 6 批次 5，scene.lua entities 段）："场景里有什么"的
 * 数据驱动列表——每项注册为场景实体（画不画 = entity 有无，ECS 规范 3）。
 * type = "model"（file = glTF 路径）| "cube"（内建几何，无需 file，见 builtin_geometry）。
 * transform（仅 model/cube 通用）：position 世界平移 + rotation 欧拉角（度，
 * 应用序 X→Y→Z）+ scale 均匀缩放（约束同 Phase 5 已知近似②：world 法线 mat3
 * 直乘假定均匀缩放）。缺省 = identity（模型原始位姿）。
 */
export struct SceneEntityDesc {
	std::string type = "model";
	std::string file;
	glm::vec3 position{0.0f};
	glm::vec3 rotation{0.0f};
	float scale = 1.0f;
};

/**
 * 场景配置：加载哪些模型 + 用哪些 pipeline + 是否场景旋转。
 * 沿用 Phase 1 确立的 configs/ 目录 *.lua 模式（sol2 沙箱 + 字段可选回落默认值）。
 */
export struct SceneConfig {
	std::string modelPath;                 // 旧单模型字段（entities 缺省时的兼容合成源，空 = 无模型）
	std::vector<SceneEntityDesc> entities; // Phase 6 批次 5：entity 列表（非空 = 权威场景构成）
	std::vector<PipelineSpec> pipelines;   // pipeline 规格列表
	std::map<std::string, std::string> styleMap;  // Phase 4 批次 3：风格名→管线规格名
	// Phase 8 批次 F：材质名连连看规则表（有序，首个命中生效）——材质名含 pattern → 风格键。
	// forward 经 styleMap、deferred 经 deferredStyleMap 落到各自规格名；空 = 全走 pbr。
	std::vector<std::pair<std::string, std::string>> materialRules;
	std::map<std::string, std::string> deferredStyleMap;  // 风格名→deferred 规格名（缺省风格 = deferred_geometry）
	bool spin = true;                      // 场景整体时间旋转（便于肉眼判断渲染对错）
	// ── 帧驱动 spin（scene.lua `spin_frame_angle` / `spin_axis`，诊断激励门，默认 = 逐位今天）──
	// spin_frame_angle ≠ 0 = 门开：angle 按帧号定步推进（纯函数，见 SceneSpin 注释），
	// dt 累积停用；spin_axis 覆写旋转轴（默认 (0.5,1,0) = 今天）。imgqa temporal-motion
	// 用它造**跨进程可复现**的相机运动激励（绕 up 轴定角 yaw：刚体运动、prevSpin 重投影
	// 精确对齐；dt 累积随帧时长漂 = 不可复现，tools/imgqa/temporal.cpp 头注释）。
	float spinFrameAngle = 0.0f;           // rad/帧（0 = 门关）
	glm::vec3 spinAxis{0.5f, 1.0f, 0.0f};  // 旋转轴（默认 = SceneSpin.axis 现值）
	bool showCube = true;                  // 是否渲染硬编码 cube（Phase 1 遗留；模型在场时通常关掉）
	Light light;                           // Phase 5 批次 2：定向光（scene.lua light 段，可选回落默认）
	std::vector<Light> lights;             // Phase 7 D3：多灯（scene.lua lights 列表；空则 loadSceneConfig 回落合成 [light]）
	EnvironmentConfig environment;         // Phase 5.5：环境贴图（scene.lua environment 段，可选缺省）
	float exposure = 1.0f;                 // Phase 8 收尾：tonemap 曝光（scene.lua 顶层 exposure，ACES 前线性倍率，1.0 = 直通）
	// Phase 11 批 11.4：SSAO 施加强度（scene.lua ao 段 ao.strength，进 scene data @60；
	// 0 = 逐位旁路。半径/采样数等形状参数在 ao.frag.slang 内为常量，不走配置）
	float aoStrength = 0.0f;
	// ── 批 12.6 TAA（时域抗锯齿，scene.lua `taa` 段）──
	// 批 14.6：**本结构不再持有 TAA 开关**——「TAA 在不在图里」= **图形态**问题，归
	// runtime.lua 的 `aa` 三态（`RuntimeConfig::aaWantsTaa()`，原 `taa.enabled` 键已删）。
	// 留在 scene 侧的只有**调参**（下面各项是每帧运行参数）。开关搬走的理由不是洁癖：
	// 它与 `fsr_temporal` 分家两个布尔时，失效形态 = 两个都开、引擎静默关掉一个而配置
	// 里还写着 true；收进一个槽位后该写法不存在。
	// 本帧权重（taa.frag: lerp(clipped, cur, x)）⟹ **调小 = 更信历史**，等效累积窗 ≈1/α。
	// 0.01 是 2026-10-05 扫档拍板的交付档（原 static boost 的静止地板，boost 已删 ⟹ 它
	// 就是唯一档位）：aa_target 交付态 base_space=true 下 meanSwing / 梯度比 =
	// 门关 0.00446/0.968 · 0.05→0.00226/0.953 · 0.02→0.00110/0.941 · **0.01→0.00062/0.932**，
	// 三档梯度比全在 [0.85,1.02] 带内 ⟹ 带内取残差最低者（判据 = imgqa temporal-residual）。
	float taaFeedback = 0.01f;          ///< 本帧权重（0.01 ⇒ 等效累积窗 ≈100 帧）
	float taaJitterScale = 1.0f;        ///< 抖动幅度倍率（1 = ±0.5 像素）
	bool taaMotionCompensation = true;  ///< prevSpin 刚体补偿（关 = 纯相机重投影）
	// 批 13.5 **基空间累积**（`taa.base_space`，**语义于批 13.5 改义**，旧名沿用）：
	// 旧义（批 13.2）：在 tonemap 加 `+c_t` 采样偏移，把 TAA **输出**从抖动空间搬回基网格。
	//   那只修了显示那一半——历史仍住抖动空间 ⟹ 累积内部每帧按 `δ_t = c_t − c_{t−1}`
	//   （闭式均值 0.667px）亚像素重采样 ⟹ 「固定频率 + 固定位置」的 16 帧周期 shimmer
	//   原样留在交付画面（实测相邻帧 9.22% 像素 >2LSB），且老贡献被反复重采样 = 还糊 1.5 倍。
	// 新义（批 13.5）：在 **taa pass 的输入侧**按 c_t 做 un-jitter，并给历史采样修正
	//   c_{t−1} ⟹ **混合与累积都发生在基空间** ⟹ 静止机位下历史采样偏移恒 0（点采样）。
	//   tonemap 侧的偏移随之删除（留着 = 双重 un-jitter，比改前更糟）。
	// 门判定只有一处 = `render_list_impl.cppm::fillTaaData`（shader 侧无门逻辑）：
	//   门关 ⇒ 两个载荷恒 0 ⇒ 逐位回到**改前的 `base_space = false` 形态**（输出在抖动空间
	//   + 历史每帧重采样）⟹ 门开/门关之差 = 本次架构改造的净效果，**同一构建可 A/B**。
	// 默认 **true**。改前的拍板依据（游动 0.643→0.060px 等）见 docs/phase13_plan.md §1.10；
	// 本批改义后的实测 → docs/taa_history_space_plan.md §5（待回填）。全案 → 同文件。
	bool taaBaseSpace = true;
	// ── 重投影消融门（scene.lua `taa.reprojection`，默认 true = 逐位今天）──
	// false = **完全不重投影**：历史按「同基位置」采样（载荷 prevViewProj := 本帧抖动后
	// viewProj、prevContentShift := 0、motion := 单位阵 ⟹ prevUvBase ≡ baseUv）。
	// 与 `motion_compensation` 的分工：那条只关 **prevSpin 场景旋转补偿**（相机不动、
	// spin 关时它是恒等阵 = 关与不关逐位相同）；本条关的是 **prevViewProj 相机重投影**
	// 本身——相机运动激励下唯一有意义的重投影消融（消费方 = imgqa temporal-motion 的 M3）。
	// 静止机位下 prevViewProj ≡ 本帧矩阵 ⟹ 本门关与不关逐位相同（对照组免费成立）。
	bool taaReprojection = true;
	// ── 历史拒收诊断门（scene.lua `taa.diag_reject`，默认**关**）──
	// 开 = taa.frag 把逐像素「历史怎么了」编进 **taa_out 的 alpha 通道**（0=原样收下 /
	// 0.25=被邻域盒裁剪 / 0.5=塌回邻域盒 centroid=拒收 / 0.75=重投影出画拒收 /
	// 1=本帧无历史），经 `POST /shot {"attachment":"taa_out"}` 回读取数。
	// RGB 一个 bit 不动（tonemap 只读 rgb、历史采样只读 rgb）⟹ 交付图逐位不变；
	// 门关 = alpha 恒 1.0 = 今天。要回答的问题：「α 调多小都不用的像素，是不是历史被
	// 邻域盒拒收了」（docs/phase13_plan.md §1.7 的 98.3% 恒拒有前科，根因 CR 核奇偶性）。
	bool taaDiagReject = false;
	// ── 输入侧 un-jitter 的**重建核选择**（scene.lua `taa.input_kernel`，批 13.8，默认 0）──
	// 0 = 双线性（4 抽头，权重恒 ∈[0,1]，**= 改前行为，逐位不变**）/ 1 = Catmull-Rom 4×4
	// （与历史侧同一份权重实现）/ 2 = Lanczos3 6×6（Filament 输入侧那个）。
	// 为什么要有：批 13.2 定案双线性用的两条判据（梯度能量 / `aa-coverage`）全是**静止口径**，
	// 运动下从未重测；本门让「换核」在同一构建里可 A/B（判据 = `imgqa temporal-flicker` 的
	// 闭式刻度 ζ + `aa-coverage`，消费方 = imgqa `--kernel`）。核语义与实测判决 →
	// `taa.frag.slang` 的 `sampleInputColor` 与 main 内 un-jitter 段。
	float taaInputKernel = 0.0f;
	// ── **历史裁剪消融门**（scene.lua `taa.clip_mode` / `taa.clip_soft`，批 13.11）──
	// 默认 clip_mode = 0 = **现行行为，逐位不变**；4 = 软裁剪（弹簧不是墙），λ = clip_soft。
	// 档位语义、为什么这条门存在、以及 λ 的选值安全线（λ < α 时盒不再是鬼影刹车）
	// → `scene_data.cppm` TaaParamsBlock::clipControl 与 `git:调试历史` 2026-10-06。
	float taaClipMode = 0.0f;
	float taaClipSoft = 1.0f;
};

/**
 * 解析器键轨迹（`DiagnosticsKeyTrace` 同形，债-11：docs/gate_lint_plan.md §3-B 的
 * 未知键 lint + 覆盖率护栏回灌 scene.lua）。键名书写与 Lua 路径一致，子表字段用
 * `表名.字段`（如 `ao.strength`）。本 loader 没有门回显行 ⟹ 判据表叫 KnownKeys 而非
 * EchoKeys：与 kGateEcho 同角色（未知键 lint 的判据表 + 覆盖率断言的输入），只是无回显输出。
 */
export struct SceneKeyTrace {
	std::vector<std::string> read;     ///< 解析器读过的键（解析器总是全量问一遍 = 与文件内容无关）
	std::vector<std::string> present;  ///< 文件里实际出现的顶层键名（子表只记表名）
};

/**
 * 键表的全部键名（与 `SceneKeyTrace::read` 同拼写）——单测据此做「读过的键集合 ==
 * 键表键集合」的双向覆盖率断言（解析器读了而表不列 = 表撒谎，表列了而解析器不读 =
 * 陈旧键）；表本体在实现单元（不把 fmt 拉进本模块接口）。
 * @return 键名列表（顺序 = 键表顺序）
 */
export std::vector<std::string> sceneConfigKnownKeys();

/**
 * 默认配置：cube + model + model_pbr + model_phong（+model_pbr_ibl + skybox）
 * 六条内建 pipeline（SPV 路径与构建产物对应），无模型，开启旋转。
 * 默认 styles：unlit→model / pbr→model_pbr（Phase 4 批次 3，styles 表可选回落；
 * Phase 5 批次 3：改 pbr = "model_phong" 即切基础 Phong 路线，零 C++ 改动）。
 * @return 默认 SceneConfig（值）
 */
export SceneConfig defaultSceneConfig();

/**
 * 从 Lua 配置（configs/scene.lua）初始化场景（字段可选，缺省回落默认值）。
 * 文件缺失 → 返回默认 SceneConfig + warn；脚本/类型错误 → std::unexpected。
 * 沙箱限制与 camera.cppm / tools/schema_loader 一致：禁用 dofile/loadfile/package/require。
 * @param path 配置文件路径
 * @param trace 可选出参：键轨迹（覆盖率护栏与未知键 lint 的输入；传 nullptr = 不记录）
 * @return 解析好的 SceneConfig；失败返回 ConfigLoadFailed
 */
export [[nodiscard]] std::expected<SceneConfig, Error> loadSceneConfig(
	std::string_view path, SceneKeyTrace* trace = nullptr);

}  // namespace RGL
