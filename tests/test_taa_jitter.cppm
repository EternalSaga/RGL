module;
#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>  // glm::perspective（被测函数就是给它加列平移）
#include <algorithm>  // std::max（initializer_list 版）/ std::abs
#include <cmath>
#include <set>

/**
 * test_taa_jitter —— 批 12.6 TAA 的纯函数层（rgl/taa_jitter.cppm，rlbase）
 *
 * 被测 = Halton 抖动序列、jitter→投影矩阵注入、相机跳切判据、历史可用性判据。
 * 这四件事全都不需要 VkDevice（AGENTS 分层判据），却直接决定 TAA 是「真生效」还是
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

}  // namespace RGL
