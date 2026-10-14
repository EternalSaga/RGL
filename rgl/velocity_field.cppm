module;
#include <glm/glm.hpp>
export module velocity_field;

import rlstd;

/**
 * velocity_field —— 逐像素速度场的**纯函数层**（Phase 14 批 14.4，rlbase）
 *
 * 「本帧这个像素对应的表面点，在上一帧落在哪个像素」的解析解。债-35 / 债-71 的公共前置：
 * FSR 3.1 upscaler 把 `motionVectors` 当**硬要求**（缺它 = 时域累积找不到历史），而本项目
 * 此前只有 `taaMotion = prevSpin·inverse(curSpin)` 一条**全局矩阵**。
 *
 * ## 为什么住 rlbase（AGENTS 分层判据：这段逻辑需不需要一个 VkDevice 才能跑）
 * 不需要 —— 输入是两条相机矩阵 + 一个深度值，输出是二维向量，同 `taa_jitter` 先例。
 * 而它**必须**在毫秒级单测里钉住：本项目的坐标系口径已经第三次咬人（`phase13_plan` §1.6
 * 撤回条、§1.17 结论 5 的 φ 反号、债-65 漏掉的 v 分量），每一次都是"接上真画面之后才发现
 * 一开始就错"。速度场错一个符号，画面**照样能看**（只是历史搬错方向），靠眼睛抓不出来。
 *
 * ## 口径（四条，逐条都有对应用例钉住；改任何一条要同时改 `docs/FSR输入契约.md`）
 * 1. **uv = framebuffer uv**：原点左上、y 向下，纹素中心在 `((px+0.5)/W, (py+0.5)/H)`。
 *    ndc.x = uv.x·2−1，ndc.y = 1−uv.y·2 —— 与 `ao.frag.slang` / `taa.frag.slang` 的
 *    `worldFromDepth` **同一套**（本模块就是它的 CPU 侧镜像，两者必须互逆）。
 * 2. **depth = Vulkan 深度 [0,1]**，0 = near、1 = far。`depth == 1` 判为天空
 *    （与 `taa.frag.slang:533` 的 `depth < 1.0` 同判据）：天空不随场景自旋。
 * 3. **输出 = 渲染分辨率像素、prev − cur、y 向下** —— 这是 `docs/FSR输入契约.md` 里
 *    实机对拍钉死的 FSR 口径（`motionVectorScale` 是**分子**，所以像素单位的速度直接填
 *    `(1,1)`，不要再除分辨率）。
 * 4. **两帧矩阵必须同口径**：都喂**未抖** ⟹ 输出基空间速度 ⟹ FSR 的
 *    `ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` 必须**不开**；都喂**抖动后** ⟹ 输出抖动
 *    空间速度 ⟹ 必须**开**。两处成对，与 `taa.frag.slang` 的「prevViewProj 抖动后 + 减
 *    c_{t−1}」是同一类红线——**本模块看不出喂的是哪一份**，所以配对责任在调用方。
 *    用例「喂抖动后矩阵 ⟹ 输出恰好平移 jitter 差」把这条可推导性钉住。
 *
 * ## 现状（诚实记录）
 * 本批只落**纯函数层 + 单测**，尚无调用方。渲染 pass 侧的落点已勘察：用**全屏
 * `fullscreenPass`（`drawLight` kind）**而不是 plan 原文写的 compute pass —— tonemap /
 * ao / taa 三个 pass 全是这个形状（全屏 draw 读纹理写 RGBA16F RT），走它就不需要
 * `RgPassKind` 新增 compute kind、不依赖批 14.4b 的图外布局护栏、也不涉及独立计算队列
 *（⚠ 那条队列**并不存在**：申请的是 family 0 的名额、被钳回与图形同一条 → `docs/compute_调研.md` §3）。
 * 勘察结论与代价对比 → `docs/phase14_plan.md` §1.10；其中「图无 compute kind」那条理由已被
 * `docs/compute_调研.md` §6 D1 定案消掉，但「全屏 draw 的 vertex 开销可忽略」仍成立 ⟹ 形态不变。
 */

namespace RGL {

/**
 * 一帧内所有像素共用的量（每帧构造一次，逐像素调用 `pixelVelocity`）。
 *
 * 成员全部**按值**：这是给逐像素循环用的热结构，不该有间接引用。四个矩阵 + 一个 vec2
 * = 272B，调用方持有一份即可。
 */
export struct VelocityFrame {
	/**
	 * 本帧 viewProj 的逆，用于 uv + depth → 世界。
	 * ⚠ 必须与写进深度缓冲的那一份**同口径**（未抖配未抖、抖动配抖动）——不一致的
	 * 后果是重建出的世界点整体偏移，而深度本身是对的，事后极难归因。
	 */
	glm::mat4 curInvViewProj{1.0f};
	/** 上一帧 viewProj（世界 → 上一帧 clip）。口径与 `curInvViewProj` 成对，见文件头第 4 条。 */
	glm::mat4 prevViewProj{1.0f};
	/**
	 * 场景自旋补偿 = `prevSpin · inverse(curSpin)`：把「本帧世界坐标」刚体搬回
	 * 「上一帧世界坐标」。本项目全场景共用一个旋转矩阵，所以一条 mat4 就是**精确**补偿。
	 * 单位阵 = 无自旋（与 `env.taaMotionCompensation` 关时同形）。
	 */
	glm::mat4 sceneMotion{1.0f};
	/**
	 * 渲染网格（**不是**后缓冲尺寸，批 14.3 起两者可不等）。uv 差 → 像素差的乘数。
	 * 用渲染网格是因为速度场的分辨率就是渲染网格；写成窗口尺寸会让 renderScale≠1 时
	 * 速度按比例错（0.5 档错 2 倍）。
	 */
	glm::vec2 extent{0.0f, 0.0f};
};

/** 单像素速度结果。 */
export struct PixelVelocity {
	/**
	 * 速度（渲染分辨率像素、prev − cur、y 向下）。
	 * `valid == false` 时**不可使用**（本实现保证此时为 (0,0)，但调用方不得依赖
	 * 该值有意义 —— 无效就写 0 是调用方的策略，不是本层的语义）。
	 */
	glm::vec2 velocityPx{0.0f};
	/**
	 * false = 该表面点上一帧在相机背后（`prevClip.w ≤ 0`）或中间量非有限。
	 * 两种情况下"搬到哪里"没有定义，调用方须按无历史处理（FSR 侧对应写零速度）。
	 */
	bool valid = false;
};

/**
 * 屏幕 uv + 深度 → 世界坐标（`ao.frag.slang` / `taa.frag.slang` 的 `worldFromDepth`
 * 的 CPU 侧镜像，逐式相同：ndc.y = 1 − uv.y·2 做 y 翻转，Vulkan 深度 [0,1] 直接用）。
 *
 * 单独导出：调用方常常还需要世界坐标本身（膨胀深度、可视化），且"两者互逆"这件事
 * 需要一个可对拍的入口。
 * @param uv framebuffer uv（纹素中心取 ((px+0.5)/W, (py+0.5)/H)）
 * @param depth Vulkan 深度 [0,1]
 * @param curInvViewProj 与深度缓冲同口径的逆矩阵
 */
export inline glm::vec3 worldFromUvDepth(glm::vec2 uv, float depth, glm::mat4 curInvViewProj) {
	const glm::vec4 clip(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, depth, 1.0f);
	const glm::vec4 w = curInvViewProj * clip;
	return glm::vec3(w) / w.w;
}

/**
 * 世界 → 屏幕 uv（`worldFromUvDepth` 的逆方向；y 翻转互逆：uv.y = (1 − ndc.y)/2）。
 * @return uv；相机背后（w ≤ 0）或非有限 → nullopt
 */
export inline std::optional<glm::vec2> uvFromWorld(glm::vec3 world, glm::mat4 viewProj) {
	const glm::vec4 clip = viewProj * glm::vec4(world, 1.0f);
	if (clip.w <= 0.0f)
		return std::nullopt;  // 上一帧相机背后：无历史可言（与 taa.frag 同判据）
	const glm::vec2 ndc(clip.x / clip.w, clip.y / clip.w);
	const glm::vec2 uv(ndc.x * 0.5f + 0.5f, (1.0f - ndc.y) * 0.5f);
	if (!std::isfinite(uv.x) || !std::isfinite(uv.y))
		return std::nullopt;
	return uv;
}

/**
 * 逐像素速度：本帧 (uv, depth) 处的表面点在上一帧的像素位移。
 *
 * 三步（与 `taa.frag.slang:530-540` 的重投影**同构**，差别只在本函数输出位移而非采样点、
 * 且矩阵口径由调用方决定）：
 *   ① world = uv + depth 反投影
 *   ② 非天空才施加场景自旋补偿（天空不随场景转，`taa.frag.slang:533` 同判据）
 *   ③ 投到上一帧 → uv_prev，速度 = (uv_prev − uv) × extent
 *
 * 深度恰为 1 判天空用 `>=` 而不是 `==`：远平面上的所有点深度都是 1，而浮点上
 * 「恰等于 1」与「被夹到 1」不可区分——与 shader 的 `depth < 1.0` 严格互补。
 */
export inline PixelVelocity pixelVelocity(glm::vec2 uv, float depth, const VelocityFrame& frame) {
	const glm::vec3 world = worldFromUvDepth(uv, depth, frame.curInvViewProj);
	const glm::vec4 worldPrev =
	    depth < 1.0f ? frame.sceneMotion * glm::vec4(world, 1.0f) : glm::vec4(world, 1.0f);
	const auto prevUv = uvFromWorld(glm::vec3(worldPrev) / worldPrev.w, frame.prevViewProj);
	if (!prevUv)
		return {};
	PixelVelocity out;
	out.velocityPx = (*prevUv - uv) * frame.extent;
	if (!std::isfinite(out.velocityPx.x) || !std::isfinite(out.velocityPx.y))
		return {};
	out.valid = true;
	return out;
}

/**
 * 抖动空间速度 → **基空间**速度。
 *
 * 内容位移口径 = `taa_jitter.cppm::contentShiftPx`（`c = (+jx, −jy)`，**不是** jitterPx）：
 * 抖动帧 t 的像素 q 显示的是基位置 q − c_t 的内容，即「基位置 p 的内容出现在像素 p + c_t」。
 * 于是同一个静止表面点：
 *   抖动网格里的位移 = (p + c_{t−1}) − (p + c_t) = c_{t−1} − c_t   ← **与运动无关，纯抖动伪影**
 *   基空间位移       = p − p = 0
 * ⟹ `velocity_base = velocity_jittered + (c_t − c_{t−1})`。
 *
 * 为什么单独一个函数而不是并进 `pixelVelocity`：并进去了就有**两个旋钮互相决定输出空间**
 * （喂的矩阵口径 + 内容位移给没给），而那正是编译器看不出、错了画面照样能看的那类成对约束
 * ——与 `taa.frag.slang` 的「prevViewProj 抖动后 ⟺ 必须减 c_{t−1}」同款。拆开之后
 * 调用方**显式**组合一行，空间归属写在名字里。
 *
 * 与文件头第 4 条的配对：喂抖动后矩阵（本项目 scene data 里那两份**都是**抖动后）
 * ⟹ `pixelVelocity` 出抖动空间 ⟹ 过本函数得基空间 ⟹ FSR 的
 * `ENABLE_MOTION_VECTORS_JITTER_CANCELLATION` **不开**（基空间速度里没有 jitter 项可抵消）。
 *
 * @param velocityJittered `pixelVelocity` 的输出（两帧矩阵均为抖动后）
 * @param contentShiftCur  本帧内容位移 c_t 像素（scene data @723584.zw）
 * @param contentShiftPrev 上一帧内容位移 c_{t−1} 像素（scene data @723600.xy）
 */
export inline glm::vec2 toBaseSpace(glm::vec2 velocityJittered, glm::vec2 contentShiftCur,
                                    glm::vec2 contentShiftPrev) {
	return velocityJittered + (contentShiftCur - contentShiftPrev);
}

}  // namespace RGL
