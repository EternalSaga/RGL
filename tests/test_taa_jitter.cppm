module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::perspective（被测函数就是给它加列平移）/ glm::lookAt
#include <glm/gtc/matrix_inverse.hpp>    // glm::inverse（批 13.5 用例复刻 shader 的 worldFromDepth）
#include <algorithm>  // std::max（initializer_list 版）/ std::abs
#include <cmath>
#include <set>

/**
 * test_taa_jitter —— 批 12.6 TAA 的纯函数层（rgl/taa_jitter.cppm，rlbase）
 *
 * 被测 = Halton 抖动序列、jitter→投影矩阵注入、相机跳切判据、历史可用性判据、
 *        批 13.5 基空间累积的坐标口径不变量（`contentShiftPx` + 历史重投影闭环）。
 * 这些全都不需要 VkDevice（AGENTS 分层判据），却直接决定 TAA 是「真生效」还是
 * 「静默不生效」：抖动幅度写错 2 倍、jitter 只加到 x、判据恒 true/false —— 画面
 * 都可能只是"看起来还行"，必须靠闭式解钉住。
 *
 * ## 期望值出处（独立于被测实现）
 *  - Halton 项 = 定义式「base 进制位反写」手算（如 halton(5, base2)：5=101₂ →
 *    反写 .101₂ = 1/2+1/8 = 0.625）；
 *  - 影像平移量 = 透视矩阵的解析推导：clip.w = −v.z（透视矩阵 P[3][2]=−1），给
 *    P[2][0] 加 ε ⇒ ndc.x 变化 = −ε ⇒ 像素位移 = +jitterPx（推导全文见被测函数注释）；
 *  - 跳切阈值 45° 的 cos = √2/2（解析值）。
 *
 * ## 阳性锚点
 *  「16 帧内 x 分量互不相同」+「max|x| > 0.4」两条：若抖动序列恒 0（函数没生效），
 *  均值/值域两条断言会**恰好全绿**（恒 0 满足 |x|<0.5 且 mean=0），这两条堵的就是这个洞。
 */
export module test_taa_jitter;

import rlstd;
import taa_jitter;
import test_support;  // testing::expectNear（项目浮点判据：把误差摆在明面上）

namespace RGL {
namespace {

/** view 空间点 → framebuffer 像素坐标（viewport 变换的标准式，独立于被测）。 */
glm::vec2 projectPixel(const glm::mat4& proj, glm::vec3 viewPos, glm::vec2 extent) {
	const glm::vec4 clip = proj * glm::vec4(viewPos, 1.0f);
	const glm::vec2 ndc{clip.x / clip.w, clip.y / clip.w};
	return {(ndc.x * 0.5f + 0.5f) * extent.x, (ndc.y * 0.5f + 0.5f) * extent.y};
}

/**
 * 引擎**光栅化**像素口径（≠ 上面 `projectPixel` 的数学口径，**别混用**）。
 * 本引擎视口是**负高**——`shader.cppm:123` `vk::Viewport(x, yTop, w, -h, 0.0f, 1.0f)`
 *（2026-10-04 实地核实源码，不是照抄注释）⟹ ndc.y 增大 = framebuffer 行**减小**
 * ⟹ 行 = (1 − ndc.y)/2·H。shader 侧 `worldFromDepth` 用的正是 `ndc.y = 1 − uv.y·2`，互逆。
 * 上面 `projectPixel` 不翻 y，因为它测的是**矩阵列平移本身**（不涉视口）——两者不矛盾，
 * 但**凡涉及「内容位移」一律用本函数**：用 `projectPixel` 会得出 `(+jx, +jy)` 这个错答案。
 */
glm::vec2 enginePixelFromNdc(glm::vec2 ndc, glm::vec2 extent) {
	return {(ndc.x * 0.5f + 0.5f) * extent.x, (1.0f - ndc.y) * 0.5f * extent.y};
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// Halton 序列（定义式手算锚点）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("haltonSequence 逐项对上「进制位反写」定义式（手算值，不抄实现）") {
    testing::expectNear(haltonSequence(1u, 2u), 0.5);
    testing::expectNear(haltonSequence(2u, 2u), 0.25);
    testing::expectNear(haltonSequence(3u, 2u), 0.75);
    testing::expectNear(haltonSequence(5u, 2u), 0.625);  // 5 = 101₂ → .101₂
    testing::expectNear(haltonSequence(1u, 3u), 1.0 / 3.0);
    testing::expectNear(haltonSequence(2u, 3u), 2.0 / 3.0);
    testing::expectNear(haltonSequence(3u, 3u), 1.0 / 9.0);    // 3 = 10₃ → .01₃
    testing::expectNear(haltonSequence(4u, 3u), 4.0 / 9.0);    // 4 = 11₃ → .11₃
    // index=0 定义上无项 → 约定返回 0（调用方靠 +1 规避；这里钉的是约定而非计算）
    CHECK(haltonSequence(0u, 2u) == 0.0f);
    // 值域 (0,1)：越界 = 抖动会超出一个像素，16 帧均值也不再居中
    for (uint32_t i = 1; i <= 1000u; ++i) {
        CHECK(haltonSequence(i, 2u) > 0.0f);
        CHECK(haltonSequence(i, 2u) < 1.0f);
        CHECK(haltonSequence(i, 3u) > 0.0f);
        CHECK(haltonSequence(i, 3u) < 1.0f);
    }
    // 变异红线：把 i%base 写成 i/base（经典笔误）→ 上面逐项全红
}

TEST_CASE("taaJitterPixels：周期 16 / 值域 ±0.5 / **非退化**（阳性锚点）/ scale=0 归零") {
    constexpr uint32_t n = kTaaJitterPeriod;
    std::set<float> xs, ys;
    double sumX = 0.0, sumY = 0.0;
    float maxAbs = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        const glm::vec2 j = taaJitterPixels(i);
        CHECK(j.x > -0.5f);
        CHECK(j.x < 0.5f);
        CHECK(j.y > -0.5f);
        CHECK(j.y < 0.5f);
        xs.insert(j.x);
        ys.insert(j.y);
        sumX += j.x;
        sumY += j.y;
        maxAbs = std::max({maxAbs, std::abs(j.x), std::abs(j.y)});
    }
    // 阳性锚点①：16 帧的 x / y 各自互不相同（恒 0 序列在这里就红，值域/均值都拦不住）
    CHECK(xs.size() == n);
    CHECK(ys.size() == n);
    // 阳性锚点②：真的抖到接近半像素（若幅度少乘一个 2，本条红）
    CHECK(maxAbs > 0.4f);
    // 中心对称 ⇒ 均值 ≈ 0（Filament 靠 skip 前 409 项达到的性质，本实现靠值域摆到 ±0.5）
    testing::expectNear(sumX / n, 0.0, 1e-4, 0.05);
    testing::expectNear(sumY / n, 0.0, 1e-4, 0.05);

    // 周期性（bit 级）：游标取模，长跑不回漂
    for (uint32_t i = 0; i < n; ++i) {
        CHECK(taaJitterPixels(i) == taaJitterPixels(i + n));
        CHECK(taaJitterPixels(i) == taaJitterPixels(i + 7u * n));
    }
    // scale = 0 ⇒ 完全不抖（配置 jitter_scale = 0 的语义）
    for (uint32_t i = 0; i < n; ++i) {
        const glm::vec2 z = taaJitterPixels(i, 0.0f);
        CHECK(z.x == 0.0f);
        CHECK(z.y == 0.0f);
    }
    // scale 线性：2 倍 scale = 2 倍位移（本函数唯一的非线性入口是取模）
    const glm::vec2 a = taaJitterPixels(3u, 1.0f), b = taaJitterPixels(3u, 2.0f);
    testing::expectNear(b.x, 2.0 * a.x);
    testing::expectNear(b.y, 2.0 * a.y);
}

// ═══════════════════════════════════════════════════════════════════════════
// jitter → 投影矩阵（闭式解：影像平移 = +jitterPx）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("applyProjectionJitter 闭式解：影像整体平移 **+jitterPx** 像素，且与深度/位置无关") {
    const glm::vec2 extent{1280.0f, 720.0f};
    const glm::mat4 proj = glm::perspective(1.0f, extent.x / extent.y, 0.1f, 100.0f);
    const glm::vec2 j{0.37f, -0.21f};
    const glm::mat4 jittered = applyProjectionJitter(proj, j, extent);

    // 期望 = 解析推导（clip.w = −v.z ⇒ ndc 变化 = −ε ⇒ 像素 = +jitter）。
    // 变异红线：① 符号写反 → 位移 −j；② 2/extent 写成 1/extent → 位移减半；
    // ③ 写进行（row）而非列 → 位移随 x/y 变化，下面的多点位断言会红。
    for (const float depth : {0.5f, 5.0f, 42.0f}) {
        for (const glm::vec2 xy : {glm::vec2{0.0f, 0.0f}, glm::vec2{0.3f, -0.2f},
                 glm::vec2{-0.6f, 0.55f}}) {
            const glm::vec3 v{xy.x, xy.y, -depth};
            const glm::vec2 before = projectPixel(proj, v, extent);
            const glm::vec2 after = projectPixel(jittered, v, extent);
            INFO("depth=" << depth << " xy=" << xy.x << "," << xy.y);
            testing::expectNear(after.x - before.x, j.x, 1e-4, 1e-3);
            testing::expectNear(after.y - before.y, j.y, 1e-4, 1e-3);
        }
    }
    // 原矩阵不得被就地修改（返回值语义；写成就地改会让调用方的 projUnjittered 一起抖）
    CHECK(proj == glm::mat4{glm::perspective(1.0f, extent.x / extent.y, 0.1f, 100.0f)});
    // 零抖动 = 逐位不变（门关红线的函数级保证）
    CHECK(applyProjectionJitter(proj, {0.0f, 0.0f}, extent) == proj);
    // extent 退化（0 维窗口）→ 不平移而非 NaN/inf
    const glm::mat4 g = applyProjectionJitter(proj, {0.4f, 0.4f}, {0.0f, 720.0f});
    CHECK(g == proj);
}

// ═══════════════════════════════════════════════════════════════════════════
// 跳切判据 + 历史可用性判据
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("cameraJumps：小幅平移/旋转算连续，传送与大甩头算跳切") {
    const glm::vec3 pos{1.0f, 2.0f, 3.0f};
    const glm::vec3 fwd{0.0f, 0.0f, -1.0f};
    const float radius = 40.0f;
    // ⚠️ 位移一律写满三个分量：**GLM 的单参数构造是「各分量填满」**（vec3{8.0f} =
    // (8,8,8)，长度 13.9），不是 (8,0,0)。本测试首版就因此把 0.2·r 的位移算成 0.35·r。
    // 连续：正常 WASD 单帧位移 / 慢速转头
    CHECK_FALSE(cameraJumps(pos, pos + glm::vec3{0.01f, 0.0f, 0.0f}, fwd, fwd, radius));
    CHECK_FALSE(cameraJumps(pos, pos, glm::vec3{0.0f, 0.0f, -1.0f},
        glm::normalize(glm::vec3{0.08f, 0.0f, -1.0f}), radius));  // ≈4.6°
    // 跳切：位移越过 0.25·r；视线越过 45°
    CHECK(cameraJumps(pos, pos + glm::vec3{radius, 0.0f, 0.0f}, fwd, fwd, radius));
    CHECK(cameraJumps(pos, pos, fwd, glm::vec3{1.0f, 0.0f, 0.0f}, radius));  // 90°
    // 边界两侧各一条（防「> 写成 >=」这类不算错的改动悄悄改变行为）
    CHECK_FALSE(cameraJumps(pos, pos + glm::vec3{0.2f * radius, 0.0f, 0.0f}, fwd, fwd, radius));
    CHECK(cameraJumps(pos, pos + glm::vec3{0.3f * radius, 0.0f, 0.0f}, fwd, fwd, radius));
    // 空场景（radius ≤ 0）⇒ 位移判据不适用，只看角度
    CHECK_FALSE(cameraJumps(pos, pos + glm::vec3{1000.0f, 0.0f, 0.0f}, fwd, fwd, 0.0f));
    CHECK(cameraJumps(pos, pos + glm::vec3{1000.0f, 0.0f, 0.0f}, fwd, glm::vec3{1.0f, 0.0f, 0.0f}, 0.0f));
    // 退化朝向（零向量）⇒ 无从判断，保守作废
    CHECK(cameraJumps(pos, pos, fwd, glm::vec3{0.0f}, radius));
    // 变异红线：本函数若恒 false（guard 写坏），上面四条 CHECK(...) 全红
}

TEST_CASE("taaHistoryUsable：首帧 / 关 / resize / 跳切四类失效，配对一条可用") {
    const glm::vec3 pos{0.0f}, fwd{0.0f, 0.0f, -1.0f};
    const glm::ivec2 ext{1280, 720};
    const auto usable = [&](bool on, uint32_t frame, glm::ivec2 prev, glm::ivec2 cur,
                            glm::vec3 ppos) {
        return taaHistoryUsable(on, frame, prev, cur, ppos, pos, fwd, fwd, 10.0f);
    };
    // 正向锚点：开 + 非首帧 + 尺寸同 + 相机同 ⇒ 可用
    CHECK(usable(true, 5u, ext, ext, pos));
    // 四类失效各一条反向
    CHECK_FALSE(usable(false, 5u, ext, ext, pos));      // 门关
    CHECK_FALSE(usable(true, 0u, ext, ext, pos));       // 首帧（历史未初始化）
    CHECK_FALSE(usable(true, 5u, {800, 600}, ext, pos));  // resize（RT 重建）
    CHECK_FALSE(usable(true, 5u, ext, ext, pos + glm::vec3{100.0f, 0.0f, 0.0f}));  // 传送
    // 首帧判据是 frameIndex == 0 而非「prevExtent 无效」——钉住这条约定，
    // 否则改成 prev-extent 哨兵判定时这里会静默变绿
    CHECK(usable(true, 1u, {0, 0}, {0, 0}, pos));
}

/**
 * 批 13.5 基空间累积的**核心不变量**：静止机位下，历史采样点必须逐位落回本像素。
 *
 * 为什么这条判据值得写（阳性锚点自问）：整次改造只动了「三个 uv 用哪个」这一件事——
 * 输入 un-jitter 的 `baseUv`、重建世界坐标用的 uv、历史修正减的 c_{t−1}。**三处任一处
 * 符号或口径写错，画面都仍然"看起来像 TAA"**（只是静止机位下历史偏 ≤0.5px 且随相位变
 * = 用户报障的 shimmer 原样保留）。本用例把「静止机位 ⟹ 历史点采样」这个**架构契约**
 * 直接钉成断言，且期望值是**推导出来的闭式**（不是跑一遍实现记下来的数）。
 *
 * 记号：基网格位置 β；抖动后渲染 ⟹ 世界点 w 出现在像素 β(w) + c（`c = contentShiftPx`）。
 * 输出像素 p 混合的是 `cur = I_t(p + c_t) = F(p)` ⟹ 它代表**基位置 p** ⟹ 重建必须解出
 * β(w) = p 的那个 w（喂 baseUv = p + c_t 进抖动后逆矩阵），历史要落在 β_{t−1}(w)。
 * 静止机位 ⟹ β_{t−1}(w) = β_t(w) = p ⟹ **prevUvBase == p**（点采样，零重采样）。
 *
 * 变异红线（每条反向用例钉一个**具体的**失败形态，缺一条就有一个洞没人看）：
 *  ① 不减 c_{t−1}（= 改造前的形态）⟹ 误差恰为 **+c_{t−1}**；
 *  ② 减两次（= 「prevViewProj 改存未抖、shader 侧仍减 c」这个成对约束被破的形态）
 *     ⟹ 误差恰为 **−c_{t−1}**；
 *  ③ 重建喂 `input.uv` 而非 `baseUv`（**主 agent 本次实测写错过的地方**）⟹ 误差恰为
 *     **−c_t**。此条 2026-10-04 是先写出用例、才发现 shader 里真有这个错。
 * 三条都断**带符号的闭式值**，不是「非零」——只断非零的话，符号写反照样绿。
 *
 * ⚠ **本用例的边界（别高估它）**：它验的是**坐标代数**，即在 C++ 里重新推导一遍这套
 * 公式并确认自洽；它**不读 `taa.frag.slang` 一个字节**。所以「shader 里把 `baseUv` 写成
 * `input.uv`」**不会**让本用例变红——shader 侧的守门人是 `imgqa` 的门开/门关 A/B
 * （`taa.base_space` 就是为此保留的内置开关），不是这里。
 * 本用例真正的价值：把「`c_{t−1}` 而非 `δ_t`」「重建喂 `baseUv` 而非 `input.uv`」这两个
 * **推导层面**的坑钉成可执行结论——本次实施就是靠它先发现 shader 写错了（§3.1），
 * 若没有它，那个错只会在画面上表现为"改完还是有点抖"，无从归因。
 * 变异实测（2026-10-04）：`cPrev` 取反 → 62 条红；重建改喂 `uv` → 93 条红。非空转。
 */
TEST_CASE("批 13.5 静止机位不变量：历史采样落回本像素；三条反向各钉一个闭式偏移") {
    const glm::vec2 extent{1920.0f, 1080.0f};
    const glm::mat4 view = glm::lookAt(glm::vec3{0.0f, 0.0f, 0.0f},
        glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 proj = glm::perspective(glm::radians(60.0f), extent.x / extent.y, 0.05f, 100.0f);
    const glm::vec4 world{1.3f, -0.7f, -4.2f, 1.0f};  // 相机前方的世界点

    // 覆盖整个 Halton 周期（16 个相位对），顺带钉住「结论与相位无关」
    for (uint32_t phase = 0; phase < 16u; ++phase) {
        const glm::vec2 jCur = taaJitterPixels(phase);
        const glm::vec2 jPrev = taaJitterPixels(phase + 1u);
        const glm::mat4 vpCur = applyProjectionJitter(proj, jCur, extent) * view;
        const glm::mat4 vpPrev = applyProjectionJitter(proj, jPrev, extent) * view;
        const glm::vec2 cCur = contentShiftPx(jCur);
        const glm::vec2 cPrev = contentShiftPx(jPrev);

        // ① 本帧：世界点 → 抖动后 clip → 输出像素 p（= shader 的 input.uv）与 ndc 深度
        const glm::vec4 clip = vpCur * world;
        const glm::vec3 ndcDepth{clip.x / clip.w, clip.y / clip.w, clip.z / clip.w};
        const glm::vec2 uv = enginePixelFromNdc({ndcDepth.x, ndcDepth.y}, extent) / extent;

        // ② shader 侧重建：baseUv + 深度 →（**抖动后**逆矩阵）→ 世界坐标。
        //    逐式复刻 taa.frag::worldFromDepth（ndc.y = 1 − uv.y·2），只是 uv 换成 baseUv
        const glm::vec2 baseUv = uv + cCur / extent;
        const glm::vec4 clipIn{baseUv.x * 2.0f - 1.0f, 1.0f - baseUv.y * 2.0f, ndcDepth.z, 1.0f};
        const glm::vec4 rebuilt4 = glm::inverse(vpCur) * clipIn;
        const glm::vec3 rebuilt{rebuilt4.x / rebuilt4.w, rebuilt4.y / rebuilt4.w,
            rebuilt4.z / rebuilt4.w};
        // 容差纪律：本用例全部用**绝对阈（像素 / 世界单位）**，不用相对阈——
        // ① 主断言期望值是 0，`expectNear` 的文档明写「期望值接近 0 时必须给绝对阈」
        //   （否则 scale 退到 1e-12，任何有限误差都算红）；
        // ② 反向断言的期望值 |c| 最小可到 ~0.02px，相对阈会把它压到 2e-5px，
        //   低于 float32 走 4×4 逆的噪声（实测 ~2e-4px）= 会因精度而红。
        // 5e-3px = 信号（≥0.02px）的 1/4、噪声（2e-4px）的 25 倍，两头都留余量。
        constexpr double kPx = 5e-3;    // 像素
        constexpr double kWorld = 1e-2; // 世界单位（float32 逆矩阵往返的实测地板，见下）

        // 先确认模拟器本身自洽（否则下面的断言在测模拟器、不是在测契约）。
        // 判**误差模长**而非逐分量：逐分量对 `world.y = -0.7` 这种小分量用相对阈会失真，
        // 用绝对阈又得为每个分量单独定标。地板 = float32 走 4×4 逆往返的实测值
        //（16 个相位扫下来最大 2.1e-3 世界单位 ≈ 本深度处 0.47px），取 1e-2 留 5× 余量。
        // ⚠ 这个地板**不限制**下面的像素断言：那三条判的是 `prevUv − uv` 这类**差分**，
        //   重建误差在两边共模、相减即消。所以像素容差可以定到 5e-3px 而不自相矛盾。
        testing::expectNear(static_cast<double>(glm::length(rebuilt - glm::vec3(world))),
            0.0, 0.0, kWorld);

        // ③ 投到上一帧（**抖动后**矩阵）→ 抖动网格坐标 → 减 c_{t−1} 落回基空间
        const glm::vec4 prevClip = vpPrev * glm::vec4(rebuilt, 1.0f);
        const glm::vec2 prevUv = enginePixelFromNdc(
            {prevClip.x / prevClip.w, prevClip.y / prevClip.w}, extent) / extent;
        const glm::vec2 prevUvBase = prevUv - cPrev / extent;

        // 主断言：静止机位 ⟹ 历史落在**本像素**（CR 权重因此退化成 (0,1,0,0) = 点采样）
        testing::expectNear((prevUvBase.x - uv.x) * extent.x, 0.0, 0.0, kPx);
        testing::expectNear((prevUvBase.y - uv.y) * extent.y, 0.0, 0.0, kPx);

        // 反向 ①：不减 c_{t−1} ⟹ 恰偏 +c_{t−1}（改造前每帧的历史重采样量，闭式均值 0.667px）
        testing::expectNear((prevUv.x - uv.x) * extent.x, cPrev.x, 0.0, kPx);
        testing::expectNear((prevUv.y - uv.y) * extent.y, cPrev.y, 0.0, kPx);

        // 反向 ②：减两次 ⟹ 恰偏 −c_{t−1}
        testing::expectNear((prevUvBase.x - cPrev.x / extent.x - uv.x) * extent.x,
            -cPrev.x, 0.0, kPx);
        testing::expectNear((prevUvBase.y - cPrev.y / extent.y - uv.y) * extent.y,
            -cPrev.y, 0.0, kPx);

        // 反向 ③：重建喂 input.uv（不喂 baseUv）⟹ 恰偏 −c_t
        const glm::vec4 clipBuggy{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, ndcDepth.z, 1.0f};
        const glm::vec4 buggy4 = glm::inverse(vpCur) * clipBuggy;
        const glm::vec3 buggyWorld{buggy4.x / buggy4.w, buggy4.y / buggy4.w, buggy4.z / buggy4.w};
        const glm::vec4 buggyPrevClip = vpPrev * glm::vec4(buggyWorld, 1.0f);
        const glm::vec2 buggyUv = enginePixelFromNdc(
            {buggyPrevClip.x / buggyPrevClip.w, buggyPrevClip.y / buggyPrevClip.w}, extent)
            / extent;  // ⚠ 必须 / extent：enginePixelFromNdc 返回**像素**，断言要的是 uv
        testing::expectNear((buggyUv.x - cPrev.x / extent.x - uv.x) * extent.x, -cCur.x, 0.0, kPx);
        testing::expectNear((buggyUv.y - cPrev.y / extent.y - uv.y) * extent.y, -cCur.y, 0.0, kPx);
    }
}

}  // namespace RGL
