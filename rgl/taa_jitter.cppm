module;
#include <glm/glm.hpp>
export module taa_jitter;

import rlstd;

// 批 12.6 TAA：亚像素抖动序列与帧间连续性判据（**纯函数层**，rlbase）。
//
// 放 rlbase 的理由（AGENTS 分层判据：这段逻辑需不需要一个 VkDevice 才能跑）：不需要
// → 就该在 `just test` 里测。抖动序列、jitter→投影矩阵、历史跳切判据三件事全部是
// 纯数学，出错的代价是「TAA 静默不生效 / 每帧误判跳切」，都能在毫秒级单测里钉住。
//
// 作业参考 = 本地 Filament（`/home/robin/program/filament`）：
//  - 序列 = Halton(2,3)，周期 16（`Options.h` HALTON_23_X16）；
//  - 注入 = 投影矩阵第 3 列做 NDC 平移（`PostProcessManager.cpp:2942-2956`）。
// Filament 跳过 Halton 前 409 项是为了让**均值≈0.5**（它的 jitter 定义在 [0,1) 域）；
// 本实现直接把值域摆到 (-0.5, 0.5)，中心对称天然均值 0，不需要 skip。

namespace RGL {

/** jitter 周期（帧）——Filament 默认 HALTON_23_X16 同值。 */
export inline constexpr uint32_t kTaaJitterPeriod = 16u;

/**
 * Halton 低差异序列第 index 项（index 从 **1** 起；index=0 返回 0，调用侧不该传）。
 * 定义式（逐位取 base 进制倒数和）直接实现，不查表 —— 单测拿定义式当独立判据。
 * @param index 序号（≥1）
 * @param base 进制（2 / 3 构成本项目的二维抖动）
 * @return 值域 (0,1)
 */
export float haltonSequence(uint32_t index, uint32_t base) {
	float result = 0.0f;
	float factor = 1.0f / static_cast<float>(base);
	for (uint32_t i = index; i > 0u; i /= base) {
		result += factor * static_cast<float>(i % base);
		factor /= static_cast<float>(base);
	}
	return result;
}

/**
 * 帧号 → 亚像素抖动（**像素单位**，值域 (-0.5, 0.5)·scale）。
 * x = Halton(2)、y = Halton(3)，序号 = (frameIndex mod 16) + 1（+1 跳过全零点，
 * 否则每 16 帧有一帧完全无抖动 → 该帧边缘锯齿原样上屏）。
 * @param frameIndex 帧号（任意非负）
 * @param scale 抖动幅度倍率（1 = 半像素；0 = 关闭抖动）
 * @return 像素单位抖动
 */
export glm::vec2 taaJitterPixels(uint32_t frameIndex, float scale = 1.0f) {
	const uint32_t i = frameIndex % kTaaJitterPeriod + 1u;
	return (glm::vec2{haltonSequence(i, 2u), haltonSequence(i, 3u)} - 0.5f) * scale;
}

/**
 * 把像素抖动写进投影矩阵（Filament 同法：投影矩阵**第 3 列** xy 加 NDC 平移）。
 *
 * 推导（GLM 列主序、Vulkan clip space，y 向下）：透视矩阵 P[3][2] = −1 ⇒ clip.w = −v.z，
 * 给 P[2][0] 加 ε 则 clip.x 增加 ε·v.z = −ε·clip.w ⇒ ndc.x 变化 = **−ε**。
 * 故「影像整体平移 +jitterPx 像素」= 第 3 列减 2·jitter/extent（与 Filament 的
 * `projection[2].xy -= jitter·(2/extent)` 同式，y 不需额外翻向：本引擎 NDC y 与
 * framebuffer 行序同向，见 skill rgl-verify 的引擎口径纪律）。
 * @param proj 未抖投影
 * @param jitterPx 像素单位抖动
 * @param extentPx 渲染分辨率（像素；宽或高为 0 时不平移，防除零）
 * @return 抖动后投影（原矩阵不变）
 */
export glm::mat4 applyProjectionJitter(glm::mat4 proj, glm::vec2 jitterPx,
	glm::vec2 extentPx) {
	if (extentPx.x <= 0.0f || extentPx.y <= 0.0f)
		return proj;
	proj[2][0] -= 2.0f * jitterPx.x / extentPx.x;
	proj[2][1] -= 2.0f * jitterPx.y / extentPx.y;
	return proj;
}

/**
 * 抖动量 → **内容位移** `c`（像素单位）。
 *
 * 定义（唯一口径，别再造同义词）：抖动后渲染出的图像内容整体平移 `+c`，
 * 即 `I_t(x) = F(x − c)`，`F` = 未抖（基）网格上的理想图像。
 *
 * 推导（逐轴符号不同，**勿凭直觉写**）：
 *   `applyProjectionJitter` 给 `proj[2][0/1]` 加 `−2j/extent`，而 `clip.w = −v.z` 翻一次号
 *   ⟹ `Δndc = (+2jx/W, +2jy/H)`；
 *   x：ndc → 列 = (ndc.x+1)/2·W，不翻 ⟹ **列位移 = +jx**；
 *   y：本引擎用**负高视口**（`shader.cppm::applyDynamicState`「NDC y=+1 对应 area.y」）
 *      ⟹ ndc.y=+1 落在**首行**，行 = (1−ndc.y)/2·H 再翻一次 ⟹ **行位移 = −jy**。
 * ⟹ `c = (+jx, −jy)`。
 *
 * ⚠ 上面 `applyProjectionJitter` 的注释说「y 不需额外翻向」——那说的是**矩阵运算本身**，
 *   翻向发生在其后的 ndc→视口映射里。**内容位移口径以本函数为准**（两者不矛盾，但连读易错）。
 * ⚠ `tools/imgqa/temporal.cpp::jitterDeltaPx` 返回的是本函数的**相反数** `(−jx, +jy)`——
 *   不是矛盾：探针 `freeShift` 语义为 `frame(p) = ref(p + d)` ⟹ `d = −c`。
 *
 * **实测背书**（2026-10-04，`imgqa taa-swim`）：本符号若反，采样偏移 = `−c_t` ⟹ 静止机位游动
 * 从 **0.060px** 变 **~1.33px**（闭式 0.667px，20 倍差，不会被噪声掩盖）。逐轴符号亦被该结果
 * 钉住——若只有 y 反了，游动不会归零到 0.060px。
 */
export glm::vec2 contentShiftPx(glm::vec2 jitterPx) {
	return {jitterPx.x, -jitterPx.y};
}

/**
 * 相机「跳切」判据（相邻两帧位姿突变 ⇒ 上一帧历史不可复用）。
 *
 * 重投影能修的是**连续**运动；不连续（传送 / 切镜头）时上一帧的像素对应的世界点
 * 与本帧毫无关系，继续混 = 大面积鬼影。两个条件都通过才算连续：
 *  ① 位移 ≤ 0.25·sceneRadius（radius ≤ 0 = 空场景 ⇒ 跳过位移判据）；
 *  ② 视线方向夹角 ≤ 45°。
 * 阈值刻意宽松：正常 WASD/拖拽的单帧增量远小于此，只有 RPC 传送、camera_probe、
 * 拖动滑块跳参数这类才会触发。
 * @return true = 跳切（历史作废）
 */
export bool cameraJumps(glm::vec3 prevCamPos, glm::vec3 curCamPos,
	glm::vec3 prevForward, glm::vec3 curForward, float sceneRadius = 0.0f) {
	if (sceneRadius > 0.0f) {
		const float move = glm::length(curCamPos - prevCamPos);
		if (move > 0.25f * sceneRadius)
			return true;
	}
	const float a = glm::length(prevForward), b = glm::length(curForward);
	if (a <= 0.0f || b <= 0.0f)
		return true;  // 退化朝向 = 无从判断，保守作废
	const float cosang = glm::clamp(glm::dot(prevForward, curForward) / (a * b), -1.0f, 1.0f);
	return cosang < 0.70710678f;  // cos 45°
}

/**
 * 本帧历史可否复用（**taaSystem 的判据本体**，提纯成自由函数是为了进 `just test`——
 * system 体在 ecs:impl 分区里不导出，直接测不到）。四条全过才可用：
 *  ① TAA 开着；② 不是首帧（历史图内容未定义）；③ 窗口尺寸没变（RT 重建 ⇒ 内容丢弃）；
 *  ④ 相机没跳切（见 cameraJumps）。
 * false ⇒ taa.frag 走 historyValid=0 分支 = 直接输出本帧（逐位旁路）。
 */
export bool taaHistoryUsable(bool enabled, uint32_t frameIndex, glm::ivec2 prevExtent,
	glm::ivec2 curExtent, glm::vec3 prevCamPos, glm::vec3 curCamPos,
	glm::vec3 prevForward, glm::vec3 curForward, float sceneRadius) {
	if (!enabled || frameIndex == 0u)
		return false;
	if (prevExtent != curExtent)
		return false;
	return !cameraJumps(prevCamPos, curCamPos, prevForward, curForward, sceneRadius);
}

}  // namespace RGL
