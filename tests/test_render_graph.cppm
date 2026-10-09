module;
/**
 * test_render_graph —— 渲染图编译（批 12.2d）
 *
 * 被测 = rgl/render_graph.cppm（rlscene 成员）的**纯计算**部分：校验、布局链推导、
 * barrier 计划生成、present 转换派生、RT slot 分配、三个装饰函数。
 *
 * 它 import rlvk（要 `vk::Format` / `vk::ImageLayout` 等类型），但**不需要任何
 * VkDevice** —— compileRenderGraph 是纯数据变换。**编译期类型依赖 ≠ 运行期设备依赖**，
 * 故这些用例在 `just test`（无 GPU 子集）里跑。
 *
 * ## 为什么值得测
 *
 * 渲染图是「意图 → 执行」的唯一翻译层，一处推导错就是整场画面错，而症状（画面某处
 * 莫名不对）离真因极远。本文件覆盖两类：
 *   ① **校验规则**：每条编译期拒绝路径都配一条**同形态但不该触发**的反向用例。
 *      否则无法区分「这条 guard 没生效」与「这条 guard 坏了」——两者都表现为绿。
 *   ② **推导规则**：slot = 声明序、aspect 按 kind、frameStartDiscard 三档、
 *      usage→layout/stage/access 查表、帧末 present 转换、swapchainWaitStage 派生。
 *
 * ## 写这份测试时纠正的六处「我以为的行为」（都以源码为准改了）
 *
 * 列在这里是因为**它们本身就是有价值的契约信息**，而不是我个人的失误记录：
 *   1. `RgUsage` 没有 `shaderRead`，采样读叫 **`sampleRead`**。
 *   2. swapchain / depth 附件的 format **必须** 是 `"inherit"`（给具体格式是编译错误），
 *      词表格式只对 offscreen / depthOffscreen 开放。
 *   3. depthWrite 的目标布局是 **DepthStencilAttachmentOptimal**、stage 是
 *      **Early|Late 两段并集**（load 写在 Early、store 写与深度测试在 Late）——
 *      不是「跟 colorWrite 一样的 ColorAttachmentOptimal」，也不是只给 Early。
 *   4. `decorateShadowPass` **无条件返回 true**（不校验 mapSize），且 extent 用**入参**
 *      mapSize、分块尺寸却用**常量** kShadowMapSize 推 —— 两者只在 mapSize ==
 *      kShadowMapSize 时自洽（见组 8，那条断言钉的就是这个耦合）。
 *   5. 全屏规则（恰一 colorWrite、无 depth 输出）按 **kind == drawLight** 判，不按
 *      state 名判。写 state="light" 但 kind=drawMesh 的 pass 走的是普通 draw pass 规则。
 *   6. 高亮装饰不只是插图：它还会**就地改写**被借 depth 的那个输出的 storeOp
 *      （dontCare → store）。改的是入参，故「装饰不该改图」这种直觉是错的。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
                             // （clang 需要本 TU 的 GMF 里有一个真 STL 头才声明它）

export module test_render_graph;

import rlstd;
import rlvk;
import render_graph;
import vkerror;  // Error（compileRenderGraph 的 expected 失败侧）
import scene_data;  // kActiveShadowCascades（render_graph 内部用它，但不 re-export）
import pass_vocabulary;  // kAllPassStates / kMeshPassStates / kFullscreenPassStates / kAllDrawSources
import frame_draw_list;  // FrameDrawList / segmentSpan（段名词表的映射侧）

namespace RGL {
namespace {

// ── 最小合法图（forward 直渲骨架）────────────────────────────────────────
// swapchain 颜色附件 + depth + 一个 opaque drawMesh pass 清 depth 并写 color。
// 注意 format：swapchain / depth 都必须是 "inherit"（词表格式只开放给图属 RT）。

RgAttachmentDesc swapchainAtt() {
    return RgAttachmentDesc{.name = "swapchain", .kind = RgAttachmentKind::swapchain};
}

RgAttachmentDesc depthAtt() {
    return RgAttachmentDesc{.name = "depth", .kind = RgAttachmentKind::depth};
}

RgAttachmentDesc offscreenAtt(std::string name, std::string format = "r8_unorm") {
    return RgAttachmentDesc{
        .name = std::move(name), .kind = RgAttachmentKind::offscreen, .format = std::move(format)};
}

RgPassAttachment colorOut(std::string name, RgLoadOp load = RgLoadOp::clear) {
    return RgPassAttachment{
        .attachment = std::move(name), .usage = RgUsage::colorWrite, .loadOp = load};
}

RgPassAttachment depthOut(std::string name, RgLoadOp load = RgLoadOp::clear,
    RgStoreOp store = RgStoreOp::store) {
    return RgPassAttachment{.attachment = std::move(name), .usage = RgUsage::depthWrite,
        .loadOp = load, .storeOp = store};
}

RgPassAttachment sampleIn(std::string name) {
    return RgPassAttachment{.attachment = std::move(name), .usage = RgUsage::sampleRead};
}

RgGraphDesc minimalGraph() {
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt()};
    d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
        .drawSource = "opaque", .outputs = {colorOut("swapchain"), depthOut("depth")}}};
    return d;
}

/** slot 反查（测试侧自建，不依赖被测代码的查找函数——否则查找函数坏了就一起瞎）。 */
RgAttachmentSlot slotOf(const CompiledRenderGraph& g, const std::string& name) {
    for (RgAttachmentSlot i = 0; i < g.attachments.size(); ++i)
        if (g.attachments[i].name == name)
            return i;
    return kRgNoAttachment;
}

/** 取某 pass 里针对某附件的 barrier（找不到即失败）。 */
const RgBarrierPlan* barrierFor(const RgPassPlan& p, RgAttachmentSlot slot) {
    for (const auto& b : p.barriers)
        if (b.attachment == slot)
            return &b;
    return nullptr;
}

std::string errOf(const std::expected<CompiledRenderGraph, Error>& g) {
    return g.has_value() ? "<编译竟然通过了>" : g.error().message;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：最小图能编译，且派生产物符合契约
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("最小图编译成功：slot = 声明序、aspect 按 kind 推、帧末 present 转换存在") {
    const auto g = compileRenderGraph(minimalGraph());
    REQUIRE(g.has_value());
    const CompiledRenderGraph& c = *g;

    // ① slot = 声明序（产物 attachments 向量下标即 slot；执行器按 slot 索引资源）
    REQUIRE(c.attachments.size() == 2);
    CHECK(c.attachments[0].name == "swapchain");
    CHECK(c.attachments[1].name == "depth");
    CHECK(slotOf(c, "swapchain") == 0);
    CHECK(slotOf(c, "depth") == 1);
    CHECK(slotOf(c, "nosuch") == kRgNoAttachment);

    // ② aspect 按 kind 推：color 类 → eColor，depth/depthOffscreen → eDepth。
    //   推错 = barrier 打在错误的 image view aspect 上 → 要么 VVL 报错要么静默失效。
    CHECK(c.attachments[0].aspect == vk::ImageAspectFlagBits::eColor);
    CHECK(c.attachments[1].aspect == vk::ImageAspectFlagBits::eDepth);

    // ③ 帧首写档：swapchain / depth = 丢弃（true）；offscreen = false（跨帧追踪）。
    //   决定帧首 barrier 的 oldLayout 是 eUndefined 还是追踪值 —— 错就是首帧脏内容。
    CHECK(c.attachments[0].frameStartDiscard == true);
    CHECK(c.attachments[1].frameStartDiscard == true);

    // ④ 帧末 present 转换：swapchain 被写过 ⟹ 必须有 → PresentSrcKHR
    REQUIRE(c.presentTransition.has_value());
    CHECK(c.presentTransition->attachment == 0);
    CHECK(c.presentTransition->newLayout == vk::ImageLayout::ePresentSrcKHR);

    // ⑤ swapchainWaitStage：单段直渲 = ColorAttachmentOutput（offscreen 变体 = Transfer，
    //   见组 6）。派生太早 = submit 时读到未就绪的 swapchain 图像（偶发花屏/撕裂）。
    CHECK(c.swapchainWaitStage == vk::PipelineStageFlagBits2::eColorAttachmentOutput);

    // ⑥ 无 warning：两个附件都被用了
    CHECK(c.warnings.empty());
}

TEST_CASE("pass 计划：colors 只收 colorWrite 且顺序 = 声明序，depth 至多一个") {
    const auto g = compileRenderGraph(minimalGraph());
    REQUIRE(g.has_value());
    REQUIRE(g->passes.size() == 1);
    const RgPassPlan& p = g->passes[0];
    CHECK(p.name == "opaque");
    CHECK(p.kind == RgPassKind::drawMesh);
    CHECK(p.state == "opaque");
    CHECK(p.drawSource == "opaque");
    // colors 顺序 = color index，执行器按它绑 RT；把 depthWrite 混进来会整体错位
    REQUIRE(p.colors.size() == 1);
    CHECK(p.colors[0].slot == 0);
    REQUIRE(p.depth.has_value());
    CHECK(p.depth->slot == 1);
    // blit 两端只对 blit pass 有值
    CHECK_FALSE(p.blitSource.has_value());
    CHECK_FALSE(p.blitTarget.has_value());
}

TEST_CASE("usage → {layout, stage} 查表：colorWrite 与 depthWrite 的布局**不同**") {
    const auto g = compileRenderGraph(minimalGraph());
    REQUIRE(g.has_value());
    REQUIRE(g->passes.size() == 1);
    const RgPassPlan& p = g->passes[0];

    // 首帧两个附件都是「本帧首次使用」⟹ src = TopOfPipe / eNone（没有可依赖的上一次），
    // oldLayoutDiscard = 该附件的 frameStartDiscard（这里都是 true）。
    REQUIRE(p.barriers.size() == 2);
    for (const auto& b : p.barriers) {
        INFO("barrier slot=" << b.attachment);
        CHECK(b.oldLayoutDiscard == true);
        CHECK(b.srcStage == vk::PipelineStageFlagBits2::eTopOfPipe);
        CHECK(b.srcAccess == vk::AccessFlagBits2::eNone);
    }
    // 关键：**newLayout 按 usage 分流**。若把两者都写成 ColorAttachmentOptimal，
    // 深度附件的 view 布局就与后续 depth test 期望的布局不符（VVL 报 + 深度可能失效）。
    const auto* cb = barrierFor(p, slotOf(*g, "swapchain"));
    const auto* db = barrierFor(p, slotOf(*g, "depth"));
    REQUIRE(cb != nullptr);
    REQUIRE(db != nullptr);
    CHECK(cb->newLayout == vk::ImageLayout::eColorAttachmentOptimal);
    CHECK(cb->dstStage == vk::PipelineStageFlagBits2::eColorAttachmentOutput);
    CHECK(db->newLayout == vk::ImageLayout::eDepthStencilAttachmentOptimal);
    // 深度附件的 stage 必须**同时**含 Early 与 Late 两段（spec synchronization-pipeline-stages：
    // 深度/模板格式附件的 **load** 操作归 EARLY_FRAGMENT_TESTS、**store** 操作与晚期深度
    // 测试归 LATE_FRAGMENT_TESTS）。本表同时喂 barrier 的 src 与 dst，只写 Early 的实测后果
    // = VVL syncval 报 5 条 "WRITE_AFTER_WRITE hazard ... previously written at the end of
    // the render pass instance by the attachment storeOp"（阴影图 DSAO→SRO 那条）。
    CHECK(db->dstStage == (vk::PipelineStageFlagBits2::eEarlyFragmentTests
                              | vk::PipelineStageFlagBits2::eLateFragmentTests));
}

TEST_CASE("第二次使用同一附件的 barrier：oldLayoutDiscard 转 false 且 src = 上一次 usage") {
    RgGraphDesc d;
    d.attachments = {swapchainAtt(),
        offscreenAtt("tmp", "rgba16_sfloat"), offscreenAtt("out", "rgba16_sfloat")};
    d.passes = {
        RgPassDesc{.name = "p1", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque", .outputs = {colorOut("tmp")}},
        RgPassDesc{.name = "p2", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("out")}, .inputs = {sampleIn("tmp")}},
        // 让 swapchain 也被写一次，否则「swapchain never written」会先拒掉整张图
        RgPassDesc{.name = "p3", .kind = RgPassKind::drawMesh, .state = "present",
            .drawSource = "opaque", .outputs = {colorOut("swapchain", RgLoadOp::load)}},
    };
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
    // p1 首写 tmp：offscreen 的 frameStartDiscard = false，但「本帧首次使用」走
    // 的是**该附件的** frameStartDiscard（offscreen = false → 追踪跨帧布局）
    const auto* b1 = barrierFor(g->passes[0], slotOf(*g, "tmp"));
    REQUIRE(b1 != nullptr);
    CHECK(b1->oldLayoutDiscard == false);
    // p2 采样 tmp：src 必须是上一次的 usage（ColorAttachmentOutput / color 写），
    // 这条 barrier 是**真正的同步边**，丢了就是读到未写完的内容（data race）。
    const auto* b2 = barrierFor(g->passes[1], slotOf(*g, "tmp"));
    REQUIRE(b2 != nullptr);
    CHECK(b2->oldLayoutDiscard == false);
    CHECK(b2->srcStage == vk::PipelineStageFlagBits2::eColorAttachmentOutput);
    CHECK(b2->newLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
}

TEST_CASE("depthWrite → sampleRead 的 barrier：src 必须覆盖 Late fragment tests（syncval 实测）") {
    // 实机 VVL 同步验证（VK_LAYER_VALIDATE_SYNC=1，2026-10-09）抓出的洞：deferred 图里阴影图
    // 由 shadow pass 以 storeOp=store 收尾，随后光照 pass 以 sampleRead 读它 ⟹ 图发一条
    // DSAO→SRO 布局转换。**布局转换本身是一次写**，其 src 侧必须覆盖"上一次写发生的阶段"，
    // 而深度格式附件的 storeOp 写在 LATE_FRAGMENT_TESTS（spec synchronization-pipeline-stages：
    // EARLY = 早期深度测试 + 该格式附件的 load 操作，LATE = 晚期深度测试 + 该格式附件的 store
    // 操作）。只给 EARLY 时 VVL 每帧报一条 WRITE_AFTER_WRITE（阴影图 5 条/轮冒烟）。
    // 变异红线：usageInfo(depthWrite).stage 退回只含 eEarlyFragmentTests → 本用例红。
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("rt", "rgba16_sfloat")};
    d.passes = {
        RgPassDesc{.name = "shadow", .kind = RgPassKind::drawMesh, .state = "shadow",
            .drawSource = "shadow", .outputs = {depthOut("depth")}},
        RgPassDesc{.name = "light", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("rt")}, .inputs = {sampleIn("depth")}},
        RgPassDesc{.name = "present", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("swapchain", RgLoadOp::load)}},
    };
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
    const auto* b = barrierFor(g->passes[1], slotOf(*g, "depth"));
    REQUIRE(b != nullptr);
    CHECK(b->newLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
    // 期望值出处 = spec 的阶段归属，不是 usageInfo 查表本身（抄表 = 表怎么改都绿）。
    CHECK((b->srcStage & vk::PipelineStageFlagBits2::eLateFragmentTests)
        == vk::PipelineStageFlagBits2::eLateFragmentTests);
    CHECK((b->srcStage & vk::PipelineStageFlagBits2::eEarlyFragmentTests)
        == vk::PipelineStageFlagBits2::eEarlyFragmentTests);  // load/clear 写在 Early
    CHECK((b->srcAccess & vk::AccessFlagBits2::eDepthStencilAttachmentWrite)
        == vk::AccessFlagBits2::eDepthStencilAttachmentWrite);
}

TEST_CASE("跨布局读→读必须发 barrier（TAA taa_out 首例）；同布局读→读保持不发") {
    // 实机 VVL 抓出的洞（2026-10-03）：旧 planBarrier 对「读→读」整体跳过，但
    // sampleRead(SRO) → transferRead(TsrcOptimal) 两次读的**目标布局不同**，跳过 =
    // 执行器按追踪布局 SRO 记 vkCmdBlitImage → layout mismatch（TAA 的 taa_out→
    // taa_commit 是本图形态首例）。反向对照：同布局读→读（sampleRead→sampleRead）
    // 依旧不发（identity barrier 无意义，且旧行为逐位不变）。
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), offscreenAtt("tmp", "rgba16_sfloat"),
        offscreenAtt("p3_rt", "rgba16_sfloat")};
    d.passes = {
        RgPassDesc{.name = "p1", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque", .outputs = {colorOut("tmp")}},
        RgPassDesc{.name = "p2", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("swapchain", RgLoadOp::load)}, .inputs = {sampleIn("tmp")}},
        RgPassDesc{.name = "p3", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("p3_rt", RgLoadOp::clear)}, .inputs = {sampleIn("tmp")}},
        RgPassDesc{.name = "commit", .kind = RgPassKind::blit,
            .outputs = {RgPassAttachment{.attachment = "swapchain",
                .usage = RgUsage::transferWrite}},
            .inputs = {RgPassAttachment{.attachment = "tmp",
                .usage = RgUsage::transferRead}}},
    };
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
    // p3（tmp 的第二次 sampleRead）：同布局 → **无** barrier
    CHECK(barrierFor(g->passes[2], slotOf(*g, "tmp")) == nullptr);
    // commit（transferRead）：跨布局 → **必须**有一条，src = 上次 sampleRead 的读侧
    const auto* bb = barrierFor(g->passes[3], slotOf(*g, "tmp"));
    REQUIRE(bb != nullptr);  // 变异红线：planBarrier 退回「读→读整体跳过」→ 本条红
    CHECK(bb->newLayout == vk::ImageLayout::eTransferSrcOptimal);
    CHECK(bb->srcStage == vk::PipelineStageFlagBits2::eAllGraphics);
    CHECK(bb->oldLayoutDiscard == false);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：格式词表
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("G-buffer 5 通道格式名全部可解析（少一个就静默走 inherit fallback）") {
    // 词表是 lua 侧 format 字符串 → vk::Format 的唯一映射点。少一个通道 = 那个通道
    // 静默拿 swapchain 格式 → G-buffer 的位深/通道数全错，而编译不报错。
    for (const auto& att : kGBufferAttachments) {
        INFO("attachment = " << att.name << " format = " << att.format);
        CHECK(isKnownRgFormat(std::string{att.format}));
        CHECK(resolveRgFormat(std::string{att.format}, vk::Format::eUndefined) !=
            vk::Format::eUndefined);
    }
    REQUIRE(kGBufferAttachments.size() == 5);
}

TEST_CASE("resolveRgFormat 的 fallback **只服务 \"inherit\"**；未知名返回 eUndefined") {
    // 契约（我原先以为 fallback 是通用兜底，实测不是）：
    //   "inherit" → 返回调用方给的 fallback（= swapchain 格式，由调用点决定）
    //   词表具名   → 返回映射的 vk::Format（先颜色词表 14 项，再深度词表）
    //   其他      → **eUndefined**，调用方须自己先用 isKnownRgFormat 把关
    // 把未知当 inherit 会把配置里的拼写错误静默变成 swapchain 格式（编译不报错、
    // 画面通道数/位深不对），所以「返回 eUndefined」是对的方向。
    CHECK(resolveRgFormat("inherit", vk::Format::eB8G8R8A8Srgb) == vk::Format::eB8G8R8A8Srgb);
    CHECK(resolveRgFormat("inherit", vk::Format::eR8G8B8A8Unorm) == vk::Format::eR8G8B8A8Unorm);
    CHECK(resolveRgFormat("no_such_format", vk::Format::eB8G8R8A8Srgb) == vk::Format::eUndefined);
    CHECK(resolveRgFormat("", vk::Format::eR8G8B8A8Unorm) == vk::Format::eUndefined);
    // 合法名不吃 fallback
    CHECK(resolveRgFormat("r32_uint", vk::Format::eUndefined) == vk::Format::eR32Uint);
    CHECK(resolveRgFormat("rgba16_sfloat", vk::Format::eUndefined) ==
        vk::Format::eR16G16B16A16Sfloat);
    // 深度词表也走同一个入口（先颜色后深度）
    CHECK(resolveRgFormat("d32_sfloat", vk::Format::eUndefined) != vk::Format::eUndefined);
}

TEST_CASE("深度词表与颜色词表互不越界（串了 = 深度附件拿到 color aspect 的 view）") {
    CHECK(isKnownDepthRgFormat("d32_sfloat"));
    CHECK(isKnownDepthRgFormat("d16_unorm"));
    CHECK_FALSE(isKnownDepthRgFormat("b8g8r8a8_unorm"));
    CHECK_FALSE(isKnownDepthRgFormat("r8_unorm"));
    CHECK_FALSE(isKnownDepthRgFormat("no_such_format"));
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：附件声明校验（每条拒绝 + 配对反向用例）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("校验：swapchain 给具体格式 → 拒绝；反向：inherit 通过") {
    RgGraphDesc d = minimalGraph();
    d.attachments[0].format = "b8g8r8a8_unorm";
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("inherit") != std::string::npos);

    // 反向用例：inherit 必须通过（上一轮的我以为可以填具体格式 —— 正是这条把它拦下的）
    CHECK(compileRenderGraph(minimalGraph()).has_value());
}

TEST_CASE("校验：offscreen 用未知格式 → 拒绝；反向：词表内格式通过") {
    RgGraphDesc d = minimalGraph();
    d.attachments.push_back(offscreenAtt("tmp", "no_such_format"));
    d.passes[0].outputs.push_back(colorOut("tmp"));
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("unknown format") != std::string::npos);

    RgGraphDesc e = minimalGraph();
    e.attachments.push_back(offscreenAtt("tmp", "r8_unorm"));
    e.passes[0].outputs.push_back(colorOut("tmp"));
    INFO(errOf(compileRenderGraph(e)));
    CHECK(compileRenderGraph(e).has_value());
}

TEST_CASE("校验：depthOffscreen 用颜色格式 → 拒绝；反向：d32_sfloat 通过") {
    // 图形态注意：一个 drawMesh pass **最多一个 depthWrite**（引擎规则，实测踩过）。
    // 所以被测的深度附件必须独占一个 pass，不能与骨架图的 depth 并列。
    auto buildWith = [](std::string format) {
        RgGraphDesc d;
        d.attachments = {swapchainAtt(), RgAttachmentDesc{
            .name = "shadowy", .kind = RgAttachmentKind::depthOffscreen,
            .format = std::move(format)}};
        d.passes = {RgPassDesc{.name = "shadow", .kind = RgPassKind::drawMesh,
                         .state = "shadow", .drawSource = "shadow",
                         .outputs = {depthOut("shadowy")}},
            RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
                .drawSource = "opaque", .outputs = {colorOut("swapchain", RgLoadOp::load)}}};
        return d;
    };
    const auto g = compileRenderGraph(buildWith("r8_unorm"));
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("depth format") != std::string::npos);

    INFO(errOf(compileRenderGraph(buildWith("d32_sfloat"))));
    CHECK(compileRenderGraph(buildWith("d32_sfloat")).has_value());
}

TEST_CASE("校验：同一 pass 两个 depthWrite → 拒绝（本测试自己就先踩过这条）") {
    RgGraphDesc d = minimalGraph();
    d.attachments.push_back(RgAttachmentDesc{
        .name = "depth2", .kind = RgAttachmentKind::depthOffscreen, .format = "d32_sfloat"});
    d.passes[0].outputs.push_back(depthOut("depth2"));  // 与 depth 并列 = 两个 depthWrite
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("depthWrite outputs (max 1)") != std::string::npos);
}

TEST_CASE("校验：空名 / 重名附件 → 拒绝；反向：改个名就通过") {
    RgGraphDesc a = minimalGraph();
    a.attachments[0].name = "";
    const auto ga = compileRenderGraph(a);
    REQUIRE_FALSE(ga.has_value());
    CHECK(ga.error().message.find("empty name") != std::string::npos);

    RgGraphDesc b = minimalGraph();
    b.attachments.push_back(swapchainAtt());  // 与 [0] 同名
    const auto gb = compileRenderGraph(b);
    REQUIRE_FALSE(gb.has_value());
    CHECK(gb.error().message.find("duplicate attachment name") != std::string::npos);

    RgGraphDesc c = minimalGraph();
    c.attachments[0].name = "SwapChain";  // 大小写敏感：这是**另一个**名字，不重名
    c.passes[0].outputs[0].attachment = "SwapChain";
    INFO(errOf(compileRenderGraph(c)));
    CHECK(compileRenderGraph(c).has_value());
}

TEST_CASE("校验：swapchain / depth 不得声明独立 extent（必须跟随窗口）") {
    RgGraphDesc d = minimalGraph();
    d.attachments[0].extent = {512, 512};
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("extent") != std::string::npos);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：pass 校验（三种 kind 各自的规则）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("校验：引用未声明的附件名 → 拒绝，且错误信息带 pass 名与附件名（可定位）") {
    RgGraphDesc d = minimalGraph();
    d.passes[0].outputs[0].attachment = "not_declared";
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    const std::string& msg = g.error().message;
    INFO("error = " << msg);
    // 错误信息必须**能定位**：含 pass 名 + 附件名，否则 30 个 pass 的图里无从查起
    CHECK(msg.find("opaque") != std::string::npos);
    CHECK(msg.find("not_declared") != std::string::npos);
}

TEST_CASE("校验：同 pass 内一个附件被引用两次 → 拒绝") {
    RgGraphDesc d = minimalGraph();
    d.passes[0].outputs.push_back(colorOut("swapchain"));
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("more than once") != std::string::npos);
}

TEST_CASE("校验：读先于写 → 拒绝；反向：先写后读（跨 pass）通过") {
    // 注意（我原先的图错在这）：在**已经写 depth 的那个 pass** 里再读 depth，触发的
    // 是「同 pass 一个附件引用两次（read/write feedback）」，不是「读先于写」。
    // 要命中前置规则，读者必须是**另一个** pass，且目标附件整帧没被写过。
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), offscreenAtt("never_written")};
    d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
        .drawSource = "opaque", .outputs = {colorOut("swapchain")}},
        RgPassDesc{.name = "reader", .kind = RgPassKind::drawMesh, .state = "post",
            .drawSource = "opaque", .outputs = {colorOut("swapchain", RgLoadOp::load)},
            .inputs = {sampleIn("never_written")}}};
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("before any write") != std::string::npos);

    // 反向用例：p1 写 tmp、p2 读 tmp ⟹ 合法。少了这条，「读前置」guard 被写成
    // 无条件拒绝也能过上面那条（那等于把图表达能力砍掉）。
    RgGraphDesc e;
    e.attachments = {swapchainAtt(), offscreenAtt("tmp")};
    e.passes = {RgPassDesc{.name = "w", .kind = RgPassKind::drawMesh, .state = "opaque",
                     .drawSource = "opaque", .outputs = {colorOut("swapchain"), colorOut("tmp")}},
        RgPassDesc{.name = "r", .kind = RgPassKind::drawMesh, .state = "post",
            .drawSource = "opaque", .outputs = {colorOut("swapchain", RgLoadOp::load)},
            .inputs = {sampleIn("tmp")}}};
    INFO(errOf(compileRenderGraph(e)));
    CHECK(compileRenderGraph(e).has_value());
}

TEST_CASE("校验：后续还要用的附件被 DontCare 存 → 拒绝；反向：无人用则合法") {
    auto build = [](RgStoreOp storeOp) {
        RgGraphDesc d;
        d.attachments = {swapchainAtt(), offscreenAtt("tmp")};
        d.passes = {RgPassDesc{.name = "w", .kind = RgPassKind::drawMesh, .state = "opaque",
                         .drawSource = "opaque",
                         .outputs = {colorOut("swapchain"),
                             RgPassAttachment{.attachment = "tmp", .usage = RgUsage::colorWrite,
                                 .storeOp = storeOp}}},
            RgPassDesc{.name = "r", .kind = RgPassKind::drawMesh, .state = "post",
                .drawSource = "opaque", .outputs = {colorOut("swapchain", RgLoadOp::load)},
                .inputs = {sampleIn("tmp")}}};
        return d;
    };
    const auto bad = compileRenderGraph(build(RgStoreOp::dontCare));
    REQUIRE_FALSE(bad.has_value());
    INFO("error = " << bad.error().message);
    CHECK(bad.error().message.find("DontCare") != std::string::npos);

    // 反向用例：同是 DontCare，但**没有**后续读者 ⟹ 必须通过。否则 offscreen 附件
    // 永远无法用 DontCare，而 dontCare 是省带宽的主要手段。
    RgGraphDesc ok;
    ok.attachments = {swapchainAtt(), offscreenAtt("tmp")};
    ok.passes = {RgPassDesc{.name = "w", .kind = RgPassKind::drawMesh, .state = "opaque",
        .drawSource = "opaque",
        .outputs = {colorOut("swapchain"),
            RgPassAttachment{.attachment = "tmp", .usage = RgUsage::colorWrite,
                .storeOp = RgStoreOp::dontCare}}}};
    INFO(errOf(compileRenderGraph(ok)));
    CHECK(compileRenderGraph(ok).has_value());
}

TEST_CASE("校验：swapchain 从未被写 → 拒绝（nothing to present）") {
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt()};
    d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
        .drawSource = "opaque", .outputs = {depthOut("depth")}}};  // 只写 depth
    const auto g = compileRenderGraph(d);
    REQUIRE_FALSE(g.has_value());
    INFO("error = " << g.error().message);
    CHECK(g.error().message.find("never written") != std::string::npos);
}

TEST_CASE("校验 drawMesh：空 state / 空 drawSource / 无写输出 / 两个 depthWrite") {
    // 四条都是 drawMesh 的形态规则，逐条给**独立**的破坏方式与独立断言，
    // 这样回归时能一眼看出是哪条规则松了。
    {  // 空 state
        RgGraphDesc d = minimalGraph();
        d.passes[0].state = "";
        const auto g = compileRenderGraph(d);
        REQUIRE_FALSE(g.has_value());
        CHECK(g.error().message.find("non-empty state") != std::string::npos);
    }
    {  // 空 drawSource
        RgGraphDesc d = minimalGraph();
        d.passes[0].drawSource = "";
        const auto g = compileRenderGraph(d);
        REQUIRE_FALSE(g.has_value());
        CHECK(g.error().message.find("non-empty state") != std::string::npos);
    }
    {  // 无写输出（只读不写）
        RgGraphDesc d = minimalGraph();
        d.attachments.push_back(offscreenAtt("src", "rgba16_sfloat"));
        d.passes.push_back(RgPassDesc{.name = "reader", .kind = RgPassKind::drawMesh,
            .state = "post", .drawSource = "opaque", .inputs = {sampleIn("src")}});
        // src 从没被写过，先被这条拒；换成立即可判的形态：让 reader 无任何 output
        const auto g = compileRenderGraph(d);
        REQUIRE_FALSE(g.has_value());  // 被拒（读先于写或无写输出，两条 guard 之一）
    }
}

TEST_CASE("校验 drawMesh：drawLight 规则**不按 state 名生效**（kind 才是判据）") {
    // 契约信息（我原先以为是按 state 判的）：全屏规则（恰一 colorWrite、无 depth 输出）
    // 挂在 kind == drawLight 上。所以 state="tonemap" + kind=drawMesh 的 pass 走的是
    // 普通 draw pass 规则 —— 可以带 depth 输出，编译器不管。
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt()};
    d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
                     .drawSource = "opaque", .outputs = {colorOut("swapchain"), depthOut("depth")}},
        // 故意画得不合规：state 叫 tonemap，但 kind 是 drawMesh 且带 depth 输出
        RgPassDesc{.name = "fake_tonemap", .kind = RgPassKind::drawMesh, .state = "tonemap",
            .drawSource = "opaque", .outputs = {depthOut("depth", RgLoadOp::load)}}};
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    CHECK(g.has_value());
    // 对照：把 kind 改成 drawLight，同一个形态就该被拒（恰一 colorWrite）
    RgGraphDesc e = d;
    e.passes[1].kind = RgPassKind::drawLight;
    e.passes[1].drawSource = "";
    const auto ge = compileRenderGraph(e);
    REQUIRE_FALSE(ge.has_value());
    INFO("对照 error = " << ge.error().message);
    CHECK(ge.error().message.find("colorWrite") != std::string::npos);
}

TEST_CASE("校验 blit：恰一 transferWrite + 一 transferRead；且不得碰 depth") {
    RgGraphDesc ok;
    ok.attachments = {swapchainAtt(), offscreenAtt("scene", "rgba16_sfloat")};
    ok.passes = {RgPassDesc{.name = "scene_pass", .kind = RgPassKind::drawMesh,
                     .state = "opaque", .drawSource = "opaque", .outputs = {colorOut("scene")}},
        RgPassDesc{.name = "blit", .kind = RgPassKind::blit,
            .outputs = {RgPassAttachment{.attachment = "swapchain",
                .usage = RgUsage::transferWrite}},
            .inputs = {RgPassAttachment{.attachment = "scene", .usage = RgUsage::transferRead}}}};
    INFO(errOf(compileRenderGraph(ok)));
    CHECK(compileRenderGraph(ok).has_value());  // 反向用例：合法形态必须通过

    // 少一端（没有 transferRead 输入）
    RgGraphDesc missing = ok;
    missing.passes[1].inputs.clear();
    const auto g1 = compileRenderGraph(missing);
    REQUIRE_FALSE(g1.has_value());
    CHECK(g1.error().message.find("exactly one transferWrite") != std::string::npos);

    // blit 触碰 depth 附件
    RgGraphDesc dep;
    dep.attachments = {swapchainAtt(), depthAtt()};
    dep.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
                     .drawSource = "opaque",
                     .outputs = {colorOut("swapchain"), depthOut("depth")}},
        RgPassDesc{.name = "blit", .kind = RgPassKind::blit,
            .outputs = {RgPassAttachment{.attachment = "swapchain",
                .usage = RgUsage::transferWrite}},
            .inputs = {RgPassAttachment{.attachment = "depth", .usage = RgUsage::transferRead}}}};
    const auto g2 = compileRenderGraph(dep);
    REQUIRE_FALSE(g2.has_value());
    INFO("error = " << g2.error().message);
    CHECK(g2.error().message.find("cannot touch depth attachment") != std::string::npos);

    // drawMesh pass 声明 transferWrite（transfer 写只属 blit）
    RgGraphDesc d = minimalGraph();
    d.passes[0].outputs.push_back(RgPassAttachment{
        .attachment = "swapchain", .usage = RgUsage::transferWrite});
    // swapchain 会被引用两次，先被「more than once」拒；改用独立 offscreen 附件测本规则
    RgGraphDesc e = minimalGraph();
    e.attachments.push_back(offscreenAtt("tgt"));
    e.passes[0].outputs.push_back(RgPassAttachment{
        .attachment = "tgt", .usage = RgUsage::transferWrite});
    const auto g3 = compileRenderGraph(e);
    REQUIRE_FALSE(g3.has_value());
    INFO("error = " << g3.error().message);
    CHECK(g3.error().message.find("blit pass only") != std::string::npos);
}

TEST_CASE("未被任何 pass 引用的附件 → 非致命 warning（不拒绝编译）") {
    RgGraphDesc d = minimalGraph();
    d.attachments.push_back(offscreenAtt("orphan"));
    const auto g = compileRenderGraph(d);
    // 「死附件」是配置疏漏不是编程错误 ⟹ 出 warning 而非硬失败。
    // 这条测的是**分级**：图能跑，但配置里有一行没用到。
    REQUIRE(g.has_value());
    const bool found = std::ranges::any_of(
        g->warnings, [](const std::string& w) { return w.find("orphan") != std::string::npos; });
    INFO("warnings 数量 = " << g->warnings.size());
    CHECK(found);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 5：frameStartDiscard 三档
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frameStartDiscard 三档：swapchain/depth/depthOffscreen = true，offscreen = false") {
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("tmp"),
        RgAttachmentDesc{
            .name = "shadowy", .kind = RgAttachmentKind::depthOffscreen, .format = "d32_sfloat"}};
    // 两个深度附件必须分属两个 pass（同 pass 最多 1 个 depthWrite）
    d.passes = {RgPassDesc{.name = "shadow", .kind = RgPassKind::drawMesh, .state = "shadow",
                     .drawSource = "shadow", .outputs = {depthOut("shadowy")}},
        RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque",
            .outputs = {colorOut("swapchain"), depthOut("depth"), colorOut("tmp")}}};
    const auto g = compileRenderGraph(d);
    REQUIRE(g.has_value());
    CHECK(g->attachments[0].frameStartDiscard == true);   // swapchain
    CHECK(g->attachments[1].frameStartDiscard == true);   // depth
    CHECK(g->attachments[2].frameStartDiscard == false);  // offscreen：跨帧追踪布局
    CHECK(g->attachments[3].frameStartDiscard == true);   // depthOffscreen：每帧整幅 clear
    // aspect 分档与之一致：depth/depthOffscreen 才 eDepth
    CHECK(g->attachments[2].aspect == vk::ImageAspectFlagBits::eColor);
    CHECK(g->attachments[3].aspect == vk::ImageAspectFlagBits::eDepth);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 6：blit 变体（offscreen 图）—— presentTransition 与 waitStage 都不同
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("blit 变体：swapchainWaitStage = Transfer，blit 两端被解析成 slot") {
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), offscreenAtt("scene", "rgba16_sfloat")};
    d.passes = {RgPassDesc{.name = "scene_pass", .kind = RgPassKind::drawMesh,
                     .state = "opaque", .drawSource = "opaque", .outputs = {colorOut("scene")}},
        RgPassDesc{.name = "blit", .kind = RgPassKind::blit,
            .outputs = {RgPassAttachment{.attachment = "swapchain",
                .usage = RgUsage::transferWrite}},
            .inputs = {RgPassAttachment{.attachment = "scene", .usage = RgUsage::transferRead}}}};
    const auto g = compileRenderGraph(d);
    REQUIRE(g.has_value());
    // 路由差异：直渲图 swapchain 首用 = ColorAttachmentOutput；blit 图 = Transfer。
    // 派生太早 ⟹ imageAvailable 的等待点不足，读到未就绪的 swapchain 图像。
    CHECK(g->swapchainWaitStage == vk::PipelineStageFlagBits2::eTransfer);
    REQUIRE(g->passes.size() == 2);
    // blit 两端解析成 slot（执行器据此 record blit；解析错 = 拷错图）
    REQUIRE(g->passes[1].blitSource.has_value());
    REQUIRE(g->passes[1].blitTarget.has_value());
    CHECK(*g->passes[1].blitSource == slotOf(*g, "scene"));
    CHECK(*g->passes[1].blitTarget == slotOf(*g, "swapchain"));
    // drawMesh pass 的 blit 字段必须为空（不串味）
    CHECK_FALSE(g->passes[0].blitSource.has_value());
    CHECK_FALSE(g->passes[0].blitTarget.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 7：pass 词表（pass_vocabulary）——派生子集 / 两档互斥红线 / 段名↔segmentSpan
// ═══════════════════════════════════════════════════════════════════════════

// 期望值写成**字面量**（出处 = 契约本身，不抄被测的派生代码）：改词表 = 本组红，
// 那是刻意的——名单变动必须是有意识的决定。
constexpr std::array<std::string_view, 6> kExpectMesh{"opaque", "skybox", "transparent",
    "picking", "highlight", "shadow"};
constexpr std::array<std::string_view, 6> kExpectFullscreen{"light", "ao", "taa", "tonemap",
    "velocity", "ao_apply"};  // 批 14.4 加 velocity（逐像素速度场）；批 14.5 加 ao_apply（AO 施加专用）
constexpr std::array<std::string_view, 1> kExpectInternal{"izb_mask"};

/** name 数组 → 集合（比较用；kAllPassStates 走 scope 过滤后也汇成它）。 */
std::set<std::string_view> toSet(std::span<const std::string_view> names) {
    return {names.begin(), names.end()};
}

/** internal 档名字集合（按 scope 现算——与「派生子集」不同的第二条路径，用来验派生）。 */
std::set<std::string_view> namesInInternal() {
    std::set<std::string_view> out;
    for (const auto& e : kAllPassStates)
        if (e.scope == RgStateScope::internal)
            out.insert(e.name);
    return out;
}

TEST_CASE("kAllPassStates = 十三名、无重复、每名恰属一档") {
    // 计数写死 = 「加一个状态」必须是显式决定（本项目的口径漂移正是从多一个名字
    // 而四处口径没人改开始的：izb_mask 落地时单测/Lua 头注释/AGENTS 三处都没跟）。
    REQUIRE(kAllPassStates.size() == 13);
    // 无重复 + 每名都能被自己那一档承认（档判据与表自洽）。
    for (const auto& e : kAllPassStates) {
        INFO("state = " << e.name);
        uint32_t hits = 0;
        for (const auto& f : kAllPassStates)
            if (f.name == e.name)
                ++hits;
        CHECK(hits == 1);
        const bool inSomeScope = isMeshPassState(e.name) || isFullscreenPassState(e.name)
            || e.scope == RgStateScope::internal;
        CHECK(inSomeScope);
    }
}

TEST_CASE("kFullscreenPassStates 恰含 light / ao / taa / tonemap / velocity / ao_apply 六个（派生自 fullscreenPass 档）") {
    // 这张表决定「哪些 state 是全屏终图」：少一个 = 该 pass 在图校验里不被承认为
    // 全屏，多一个 = kind 与 state 串味的图被放行。现由 kAllPassStates 派生，
    // 故这条同时在验派生机制本身（namesInScope 填错/漏填都会红）。
    REQUIRE(kFullscreenPassStates.size() == 6);
    CHECK(toSet(kFullscreenPassStates) == toSet(kExpectFullscreen));
}

TEST_CASE("kMeshPassStates 恰含六个（三主段 + picking / highlight / shadow）") {
    REQUIRE(kMeshPassStates.size() == 6);
    CHECK(toSet(kMeshPassStates) == toSet(kExpectMesh));
}

TEST_CASE("两档 state 词表互斥、并起来 = 全表减 internal（**防扁平化红线**）") {
    // 为什么单独一条：执行器按 pass.kind 分档校验 state。若有人为了"少写代码"把两档
    // 并成一张 kAllPassStates 直接判，`kind=drawLight, state="opaque"` 就会通过校验，
    // 而 drawLight 分支随后按 state 名去 registry 取预建全屏条目 → 深处崩溃或静默不画。
    // 本条把"两档必须是真分区"钉成断言：交集空 + 并集恰好是全表减 internal。
    const auto mesh = toSet(kMeshPassStates);
    const auto full = toSet(kFullscreenPassStates);
    for (const auto& n : mesh) {
        INFO("同时出现在两档的名字 = " << n);
        CHECK(full.count(n) == 0);  // 非空 = 档已塌成一张
    }
    std::set<std::string_view> unionMeshFull(mesh.begin(), mesh.end());
    unionMeshFull.insert(full.begin(), full.end());
    std::set<std::string_view> expectUnion(kExpectMesh.begin(), kExpectMesh.end());
    expectUnion.insert(kExpectFullscreen.begin(), kExpectFullscreen.end());
    CHECK(unionMeshFull == expectUnion);  // 两档并集必须正好是"能上图的十二名"
    CHECK(unionMeshFull.size() + kExpectInternal.size() == kAllPassStates.size());
}

TEST_CASE("izb_mask 属 internal 档：两档图校验都不认（反向用例）") {
    // 反向用例（不触发的分支也要有"不该触发"的用据）：izb_mask 由执行器硬编码录制
    // 路径直接 stateFor，不是一张图上的 pass。若有人把它塞进 meshPass 档，这里红；
    // 后果是"图里可以写 state=izb_mask"——而没有任何执行器路径会去录它（静默少一块）。
    CHECK(isKnownStateName("izb_mask"));  // StateKit 侧承认（stateFor 能解析）
    CHECK_FALSE(isMeshPassState("izb_mask"));
    CHECK_FALSE(isFullscreenPassState("izb_mask"));
    CHECK(namesInInternal() == toSet(kExpectInternal));
}

TEST_CASE("kAllDrawSources 段名词表 + isKnownDrawSource 的精确/前缀两式") {
    // shadow 是**前缀档**：分块号可变（kActiveShadowCascades 1→4），写死字面量会在
    // 切级联时漏校验。前缀档的宽松面如实记录（照源码写，不照期望写）：任何 "shadow"
    // 开头的名字都算合法，"shadowmap" 也放行，解析不出数字时落 0 级。
    REQUIRE(kAllDrawSources.size() == 4);
    CHECK(isKnownDrawSource("opaque"));
    CHECK(isKnownDrawSource("skybox"));
    CHECK(isKnownDrawSource("transparent"));
    CHECK(isKnownDrawSource("shadow"));    // 无后缀 = 0 级
    CHECK(isKnownDrawSource("shadow13"));  // 前缀档
    CHECK(isKnownDrawSource("shadowmap")); // 前缀档的宽松面（放行）
    CHECK_FALSE(isKnownDrawSource(""));
    CHECK_FALSE(isKnownDrawSource("Opaque"));    // 大小写敏感
    CHECK_FALSE(isKnownDrawSource("opaques"));   // 精确档不做前缀
    CHECK_FALSE(isKnownDrawSource("light"));     // state 名不是段名
}

TEST_CASE("segmentSpan 覆盖 kAllDrawSources 每个段名（表 ↔ if 链的漂移闸门）") {
    // 词表回答"段名合法吗"，**段名 → 哪一段 draw** 是 segmentSpan 的 if 链——两者不
    // 共享机制。把每个合法段名喂进链里逐段比对（含 shadow 前缀 → 对应级），链里
    // 写错边界 / 漏一条前缀分支都会红（漏前缀 = shadow pass 拿到 transparent 段 =
    // 把透明体画进阴影图）。
    FrameDrawList list;
    for (uint32_t i = 0; i < 6; ++i)
        list.calls.push_back(DrawCall{.pipeline = i, .pushData = {}, .groupCount = 1u});
    list.skyboxStart = 2;       // opaque = [0,2)
    list.transparentStart = 3;  // skybox = [2,3)、transparent = [3,6)
    for (uint32_t c = 0; c < kSceneShadowCascadeCount; ++c)
        list.shadowCalls.push_back(DrawCall{.pipeline = 100 + c, .pushData = {}, .groupCount = 1u});
    for (uint32_t c = 0; c < list.shadowBoundaries.size(); ++c)
        list.shadowBoundaries[c] = c;  // 每级各 1 个 draw

    // expectBegin ≥ 100 → 取自 shadowCalls（下标 = 级号），否则取自 calls。
    const auto segOf = [&list](std::string_view src) { return segmentSpan(src, list); };
    const auto expectSpan = [&list](uint32_t begin, uint32_t size) {
        return begin < 100
            ? std::span<const DrawCall>{list.calls.data() + begin, size}
            : std::span<const DrawCall>{list.shadowCalls.data() + (begin - 100), size};
    };
    const auto check = [&](std::string_view src, uint32_t begin, uint32_t size) {
        INFO("drawSource = " << src);
        const auto expect = expectSpan(begin, size);
        const auto got = segOf(src);
        CHECK(got.size() == expect.size());
        CHECK(got.data() == expect.data());  // 边界错 = 数据指针错
        if (!expect.empty())
            CHECK(got[0].pipeline == expect[0].pipeline);
    };
    check("opaque", 0, 2);
    check("skybox", 2, 1);
    check("transparent", 3, 3);
    check("shadow", 100, 1);  // 无后缀 = 0 级
    check("shadow2", 102, 1);
    check("shadow3", 103, 1);

    // **兜底分支的危险面**（图校验必须拦未知段名的理由）：不认识的段名不报错，
    // 而是返回 transparent 段——静默多画一遍透明体。
    const auto unknown = segOf("nosuchsegment");
    CHECK(unknown.size() == 3);
    CHECK(unknown.data() == list.calls.data() + 3);
}

TEST_CASE("graphHasDeferredLight：deferred 的「图事实」判据（三张内建图 + 两个反例）") {
    // 收口原 IZB / EVSSM 各写一份逐字相同的 any_of（vkrenderer_passrec 两处）。
    // 语义 = 图事实（有没有人在跑 deferred 光照），**不是**配置层的 variant——
    // 两者不等价正是本判据存在的理由（自定义 Lua 图可在 forward 变体下手写 light pass）。
    // 变异红线：判据的 kind 侧或 state 侧任一半被删 → 下面两条反例立刻变 true 而红。
    const auto passesOf = [](const RgGraphDesc& d) {
        auto g = compileRenderGraph(d);
        REQUIRE(g.has_value());
        return g->passes;
    };
    CHECK(graphHasDeferredLight(passesOf(defaultRenderGraphDesc())) == false);   // forward HDR
    CHECK(graphHasDeferredLight(passesOf(offscreenRenderGraphDesc())) == false); // blit 变体
    CHECK(graphHasDeferredLight(passesOf(deferredRenderGraphDesc())) == true);   // 内建 deferred

    // 反例 1：drawMesh 写 state="light" —— kind 不对，不算（且执行器 state 词表也不认它）
    RgGraphDesc meshLight = minimalGraph();
    meshLight.passes[0].state = "light";
    CHECK(graphHasDeferredLight(passesOf(meshLight)) == false);

    // 反例 2：只有 tonemap 的 drawLight（终图不是 deferred 光照消费方）
    RgGraphDesc d = minimalGraph();
    d.passes.push_back(RgPassDesc{
        .name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
        .outputs = {colorOut("swapchain")}});
    CHECK(graphHasDeferredLight(passesOf(d)) == false);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 8：三个装饰函数（行为以 rgl/render_graph_decorate.cppm 为准）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("decoratePickingPass：锚点后插 picking pass + 加 object_id 附件") {
    RgGraphDesc d = minimalGraph();
    const size_t before = d.passes.size();
    CHECK(decoratePickingPass(d));
    REQUIRE(d.passes.size() == before + 1);
    const RgPassDesc& p = d.passes[1];  // 锚点 = passes[0]，picking 紧随其后
    CHECK(p.name == "picking");
    CHECK(p.kind == RgPassKind::drawMesh);
    CHECK(p.state == "picking");
    CHECK(p.drawSource == "opaque");  // 整段重放（与 opaque 同一份 draw list）
    // 两个输出：object_id（色）+ 借来的 depth（**名字从锚点反查**，不硬编码）
    REQUIRE(p.outputs.size() == 2);
    CHECK(p.outputs[0].attachment == "object_id");
    CHECK(p.outputs[0].usage == RgUsage::colorWrite);
    CHECK(p.outputs[1].attachment == "depth");
    CHECK(p.outputs[1].usage == RgUsage::depthWrite);
    CHECK(p.outputs[1].loadOp == RgLoadOp::load);  // load：复用锚点写的深度，不清
    // object_id 附件真的被声明成了 r32_uint
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
    const auto idSlot = slotOf(*g, "object_id");
    REQUIRE(idSlot != kRgNoAttachment);
    CHECK(g->attachments[idSlot].format == "r32_uint");
    CHECK(g->attachments[idSlot].aspect == vk::ImageAspectFlagBits::eColor);
}

TEST_CASE("decoratePickingPass：object_id 的 clear **四个分量全为 0**（r32ui 下 float 默认值是垃圾 id）") {
    // VkClearValue 是 union：RgClearValue 的 color 默认是 {0.1,0.1,0.1,1}，
    // 在 r32uint 附件下 reinterpret 成一个巨大的非零 id ⟹ 背景不再是「无对象」，
    // 拾取会命中一个不存在的对象。所以装饰必须显式写零。
    RgGraphDesc d = minimalGraph();
    REQUIRE(decoratePickingPass(d));
    const RgPassAttachment& out = d.passes[1].outputs[0];
    REQUIRE(out.attachment == "object_id");
    CHECK(out.loadOp == RgLoadOp::clear);
    CHECK(out.storeOp == RgStoreOp::store);
    for (int i = 0; i < 4; ++i) {
        INFO("clear.color[" << i << "]");
        CHECK(out.clear.color[i] == 0.0f);
    }
}

TEST_CASE("反向用例：decoratePickingPass 对无 depth 输出的图返回 false（不插一个测不到深度的 pass）") {
    RgGraphDesc d;
    d.attachments = {swapchainAtt()};
    d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
        .drawSource = "opaque", .outputs = {colorOut("swapchain")}}};  // 无 depthWrite
    CHECK_FALSE(decoratePickingPass(d));
    CHECK(d.passes.size() == 1);  // 入参未被改动
    // 插一个坏 pass 比不插更糟：它能编译通过，但拾取永远命中背景（表现为「点不中」，
    // 而「点不中」的排查方向有一堆，成本远高于这里返回 false 后打一条日志）。
}

TEST_CASE("decorateHighlightPass：插在采样成片的 pass **之前**，写既有颜色附件（load）") {
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("scene", "rgba16_sfloat")};
    d.passes = {
        RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque",
            .outputs = {colorOut("scene"), depthOut("depth", RgLoadOp::clear, RgStoreOp::dontCare)}},
        RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("swapchain")}, .inputs = {sampleIn("scene")}},
    };
    const size_t before = d.passes.size();
    CHECK(decorateHighlightPass(d));
    REQUIRE(d.passes.size() == before + 1);
    // 插在 tonemap 之前：插图末 = 落在 tonemap 之后，盖不到上屏内容
    CHECK(d.passes[1].name == "highlight");
    CHECK(d.passes[1].state == "highlight");
    CHECK(d.passes[1].drawSource == "opaque");
    // 复用**场景色附件**（scene，不是 swapchain —— 按角色反查而非按名字）
    REQUIRE(d.passes[1].outputs.size() == 2);
    CHECK(d.passes[1].outputs[0].attachment == "scene");
    CHECK(d.passes[1].outputs[0].loadOp == RgLoadOp::load);  // ← 保留成片，不是 clear
    CHECK(d.passes[1].outputs[1].attachment == "depth");
}

TEST_CASE("decorateHighlightPass 会**改写**被借 depth 的那个输出的 storeOp（装饰不是纯函数）") {
    // opaque 段按「depth 后续无人用」把 depth 声明成 dontCare；高亮要读它，装饰必须
    // 就地改成 store，否则读到未定义内容。这条测的就是这次**入参副作用**。
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("scene", "rgba16_sfloat")};
    d.passes = {
        RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque",
            .outputs = {colorOut("scene"), depthOut("depth", RgLoadOp::clear, RgStoreOp::dontCare)}},
        RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("swapchain")}, .inputs = {sampleIn("scene")}},
    };
    // 前置条件：装饰前那个 depth 输出确实是 dontCare
    REQUIRE(d.passes[0].outputs[1].storeOp == RgStoreOp::dontCare);
    REQUIRE(decorateHighlightPass(d));
    CHECK(d.passes[0].outputs[1].storeOp == RgStoreOp::store);
    // 门关 = 不调用 = 图逐位不变（这个副作用只发生在装饰被调用时）
}

TEST_CASE("反向用例：decorateHighlightPass 两种不支持形态都返回 false 且不改图") {
    {  // 无「采样色附件」的消费 pass ⟹ 无成片可叠加
        RgGraphDesc d = minimalGraph();
        CHECK_FALSE(decorateHighlightPass(d));
        CHECK(d.passes.size() == 1);
    }
    {  // 有消费者，但插入点之前没人写 depth ⟹ 高亮会糊成一片
        RgGraphDesc d;
        d.attachments = {swapchainAtt(), offscreenAtt("scene", "rgba16_sfloat")};
        d.passes = {RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh,
                         .state = "opaque", .drawSource = "opaque",
                         .outputs = {colorOut("scene"), colorOut("swapchain", RgLoadOp::load)}},
            RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
                .outputs = {colorOut("swapchain", RgLoadOp::load)},
                .inputs = {sampleIn("scene")}}};
        const size_t before = d.passes.size();
        CHECK_FALSE(decorateHighlightPass(d));
        CHECK(d.passes.size() == before);
    }
}

TEST_CASE("decorateHighlightPass：采样**深度**的全屏 pass（ao 形态）不算色消费者（批 12.3d · B4）") {
    // sampledColorName（render_graph_decorate.cppm:41-46）必须排除 kind=depth 的 sampleRead：
    // ao pass（Phase 11 批 11.4）也以 sampleRead 读 depth，若只看 usage 会把它误当"成片消费
    // 者" → highlight 的 colorAtt 与借来的 depthAtt 同为 "depth" → 同 pass 引用同一附件两次，
    // 编译报 "references attachment 'depth' more than once"（源码注释点名这条撞名史）。
    // 图形态照 defaultRenderGraphDesc 的 aoPass()/tonemapPass()（render_graph_builtin.cppm:40-45）。
    // **精确语义**（isColorAttachment 实文 :22-26）：只排除 RgAttachmentKind::depth；
    // depthOffscreen（shadow_map 那类）仍按色处理——本条钉 depth 排除分支，不外推到 depthOffscreen。
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("scene", "rgba16_sfloat"),
        offscreenAtt("ao", "r8_unorm")};
    d.passes = {
        RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque",
            .outputs = {colorOut("scene"), depthOut("depth", RgLoadOp::clear, RgStoreOp::dontCare)}},
        RgPassDesc{.name = "ao", .kind = RgPassKind::drawLight, .state = "ao",
            .outputs = {colorOut("ao")}, .inputs = {sampleIn("depth")}},
        RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("swapchain")}, .inputs = {sampleIn("scene")}},
    };
    REQUIRE(decorateHighlightPass(d));
    REQUIRE(d.passes.size() == 4);
    // 插入点 = **首个色消费者 tonemap 之前**，与"无 ao pass"的基线图一致：只读 depth 的
    // ao 不得把插入点提前到它之前（变异红线：sampledColorName 去掉 depth 排除 →
    // ao 被判消费者 → 顺序变 opaque/highlight/ao/tonemap → 红）。
    CHECK(d.passes[0].name == "opaque");
    CHECK(d.passes[1].name == "ao");
    CHECK(d.passes[2].name == "highlight");
    CHECK(d.passes[3].name == "tonemap");
    // highlight 写场景色 scene（load 保留成片）+ 借 depth——两个输出附件名互不撞车
    REQUIRE(d.passes[2].outputs.size() == 2);
    CHECK(d.passes[2].outputs[0].attachment == "scene");
    CHECK(d.passes[2].outputs[0].usage == RgUsage::colorWrite);
    CHECK(d.passes[2].outputs[0].loadOp == RgLoadOp::load);
    CHECK(d.passes[2].outputs[1].attachment == "depth");
    // 借的 depth 来自 opaque（插入点前反查：先扫 ao——它无 depthWrite 输出——再到 opaque），
    // 其 storeOp 被装饰就地改写 dontCare → store
    CHECK(d.passes[0].outputs[1].storeOp == RgStoreOp::store);
    // 装饰后的图能编译（撞名形态是编译期硬错误，必须把"不撞"钉成绿）
    INFO(errOf(compileRenderGraph(d)));
    CHECK(compileRenderGraph(d).has_value());
}

TEST_CASE("decorateShadowPass：图首插 kActiveShadowCascades 个 pass + 阴影图附件") {
    RgGraphDesc d = minimalGraph();
    const size_t before = d.passes.size();
    CHECK(decorateShadowPass(d, kShadowMapSize));
    // 一级一个 pass，各渲染图集里自己那一块；前插（阴影深度不依赖相机深度 ⟹ barrier 链最短）
    REQUIRE(d.passes.size() == before + kActiveShadowCascades);
    CHECK(d.passes[0].name == "shadow");
    CHECK(d.passes[0].state == "shadow");
    if (kActiveShadowCascades > 1)
        CHECK(d.passes[1].name == "shadow1");  // 级 0 无后缀，级 ≥1 带序号
    // 每一级的 depth 输出 loadOp = **clear**（批 12.3d · B3，"各清各块"契约）：动态渲染的
    // clear 只作用于本 pass 的 renderArea（= 自己的分块子矩形，viewport 由下一条 TEST_CASE
    // 钉住）→ 分块互不重叠、清不到别级。若误改成 load（2026-09-29 4 级首测即"只清级 0"
    // 的坑，decorate cppm:160-163 注释点名）：块 1..N 永不初始化 → 采到未定义深度
    // （实测 ≈0 → 全判影，画面 60% 像素加深）。clear 值走默认 = 深度 1.0 = 无遮挡。
    for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
        INFO("cascade " << c);
        REQUIRE(d.passes[c].outputs.size() == 1);
        const RgPassAttachment& out = d.passes[c].outputs[0];
        CHECK(out.attachment == std::string{kShadowMapName});
        CHECK(out.usage == RgUsage::depthWrite);
        CHECK(out.loadOp == RgLoadOp::clear);   // 变异红线：clear→load → 红
        CHECK(out.storeOp == RgStoreOp::store);
    }
    // 消费方接线：state ∈ {opaque, transparent, light} 的 pass 拿到阴影图 sampleRead 输入
    //（不接的话编译器不生成 DSAO→ShaderReadOnly 的转换，采样端报 VVL）
    const RgPassDesc* opaque = nullptr;
    for (const auto& p : d.passes)
        if (p.state == "opaque") opaque = &p;
    REQUIRE(opaque != nullptr);
    const bool wired = std::ranges::any_of(opaque->inputs,
        [](const RgPassAttachment& a) {
            return a.attachment == std::string{kShadowMapName} && a.usage == RgUsage::sampleRead;
        });
    CHECK(wired);
    // 附件本体：depthOffscreen + d32_sfloat + aspect eDepth + frameStartDiscard true
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
    const auto slot = slotOf(*g, std::string{kShadowMapName});
    REQUIRE(slot != kRgNoAttachment);
    CHECK(g->attachments[slot].kind == RgAttachmentKind::depthOffscreen);
    CHECK(g->attachments[slot].format == std::string{kShadowMapFormat});
    CHECK(g->attachments[slot].aspect == vk::ImageAspectFlagBits::eDepth);
    CHECK(g->attachments[slot].frameStartDiscard == true);
}

TEST_CASE("decorateShadowPass 的一条**隐式耦合**：extent 用入参 mapSize，分块尺寸用常量 kShadowMapSize") {
    // 这是读实现时挖出来的契约（不是我以为的行为）：附件 extent = {mapSize, mapSize}，
    // 但每个 pass 的 viewport 来自 shadowTileSize(shadowAtlasColumns(n))
    // = **kShadowMapSize / cols**。所以只有 mapSize == kShadowMapSize 时分块才铺满图集。
    // 传别的值不报错、不返回 false —— 只会得到「分块只盖住图集一部分」的错误布局。
    // 这条断言钉住「canonical 调用下分块正好铺满」，任何人改其中一个来源就会红。
    const uint32_t cols = shadowAtlasColumns(kActiveShadowCascades);
    const uint32_t tile = shadowTileSize(cols);
    CHECK(cols == (kActiveShadowCascades <= 1u ? 1u : 2u));  // 列数规则本身
    CHECK(tile * cols == kShadowMapSize);                    // 分块 × 列数 = 图集整幅

    RgGraphDesc d = minimalGraph();
    REQUIRE(decorateShadowPass(d, kShadowMapSize));
    const auto g = compileRenderGraph(d);
    REQUIRE(g.has_value());
    const auto slot = slotOf(*g, std::string{kShadowMapName});
    REQUIRE(slot != kRgNoAttachment);
    CHECK(g->attachments[slot].extent[0] == kShadowMapSize);
    CHECK(g->attachments[slot].extent[1] == kShadowMapSize);
    // 每一级的 viewport 必须落在图集内（右/下边界不越界）
    for (uint32_t c = 0; c < kActiveShadowCascades; ++c) {
        INFO("cascade " << c);
        const auto& vp = d.passes[c].viewport;
        CHECK(vp[2] == tile);
        CHECK(vp[3] == tile);
        CHECK(vp[0] + vp[2] <= kShadowMapSize);
        CHECK(vp[1] + vp[3] <= kShadowMapSize);
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 9：跨帧历史附件（批 12.6 TAA）——读前置豁免 + 帧间 barrier 提档
// ═══════════════════════════════════════════════════════════════════════════

RgAttachmentDesc historyAtt(std::string name = "hist") {
    return RgAttachmentDesc{.name = std::move(name), .kind = RgAttachmentKind::offscreen,
        .format = "rgba16_sfloat", .history = true};
}

/**
 * 历史链最小图：opaque 写 src+depth → tonemap **读历史** 写 swapchain → blit 把 src 提交回历史。
 * 形状与 decorateTaaPass 的产物同构（taa_hist 被读在前、写在后，跨帧靠帧首 barrier）。
 */
RgGraphDesc historyGraph(bool history = true) {
    auto d = minimalGraph();
    d.attachments.push_back(offscreenAtt("src"));
    auto h = historyAtt();
    h.history = history;
    d.attachments.push_back(h);
    d.passes[0].outputs = {colorOut("src"), depthOut("depth")};  // opaque 不再写 swapchain
    d.passes.push_back(RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight,
        .state = "tonemap", .outputs = {colorOut("swapchain")}, .inputs = {sampleIn("hist")}});
    d.passes.push_back(RgPassDesc{.name = "commit", .kind = RgPassKind::blit,
        .outputs = {RgPassAttachment{.attachment = "hist", .usage = RgUsage::transferWrite}},
        .inputs = {RgPassAttachment{.attachment = "src", .usage = RgUsage::transferRead}}});
    return d;
}

TEST_CASE("history 附件：帧内未写先读**合法**，且帧首 barrier 提档成跨 submission 的真依赖") {
    // 阳性锚点：把 history 标志摘掉本图必须编译失败（组 4 的「读先于写」负例即其反面）；
    // 若豁免写成「无脑放行一切读」，本用例仍然绿 —— 所以同时断言 barrier 的 src：
    // 提档（AllCommands/MemoryWrite）才是真生效的证据，光"编译过"不是。
    const auto g = compileRenderGraph(historyGraph(/*history=*/true));
    INFO(errOf(g));
    REQUIRE(g.has_value());
    const auto hist = slotOf(*g, "hist");
    REQUIRE(hist != kRgNoAttachment);
    REQUIRE(g->attachments[hist].history == true);      // 声明透传到编译产物

    const RgPassPlan* tone = nullptr;
    for (const auto& p : g->passes)
        if (p.name == "tonemap") tone = &p;
    REQUIRE(tone != nullptr);
    const auto* b = barrierFor(*tone, hist);
    REQUIRE(b != nullptr);
    // ① src 必须是**真依赖**（TopOfPipe/eNone 一条依赖都不建立 = 跨帧读到旧帧/垃圾）
    CHECK(b->srcStage == vk::PipelineStageFlagBits2::eAllCommands);   // 变异红线：→TopOfPipe 红
    CHECK(b->srcAccess == vk::AccessFlagBits2::eMemoryWrite);         // 变异红线：→eNone 红
    // ② 布局**不得丢弃**：eUndefined 意味着每帧把历史内容当未初始化丢掉（TAA 永无历史）
    CHECK(b->oldLayoutDiscard == false);
    CHECK(b->newLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
    // 反向配对：同一张图里**非 history** 的 swapchain 帧首 barrier 仍是 TopOfPipe/eNone
    //（防「一刀切把帧首 src 全提档」的变异——那会让基线图的 barrier 静默变贵）
    const auto swap = slotOf(*g, "swapchain");
    const auto* bs = barrierFor(*tone, swap);
    REQUIRE(bs != nullptr);
    CHECK(bs->srcStage == vk::PipelineStageFlagBits2::eTopOfPipe);
    CHECK(bs->srcAccess == vk::AccessFlagBits2::eNone);
}

TEST_CASE("history 的两条反向用例：非 history 同形图仍被拒；history 但无人写也被拒") {
    // ① 同形态但 history=false → 必须报「read before write」（豁免只认 history，不是放行一切）
    const auto gOff = compileRenderGraph(historyGraph(/*history=*/false));
    CHECK_FALSE(gOff.has_value());
    CHECK(errOf(gOff).find("read") != std::string::npos);
    // ② history 但图里没有任何 pass 写它 → 永远读未初始化内存，只能编译期抓
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), historyAtt()};
    d.passes = {RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight,
        .state = "tonemap", .outputs = {colorOut("swapchain")}, .inputs = {sampleIn("hist")}}};
    const auto gNoWriter = compileRenderGraph(d);
    INFO(errOf(gNoWriter));
    CHECK_FALSE(gNoWriter.has_value());
    CHECK(errOf(gNoWriter).find("never written") != std::string::npos);
}

TEST_CASE("history 只能挂在 offscreen 上（swapchain/depth/depthOffscreen 声明历史 = 拒绝）") {
    // 只放这一个附件、零 pass：kind 校验在注册阶段就开火，不会被别的规则抢先/稀释。
    // 格式按各 kind 的合法值填（否则先撞「格式」那条 guard，测不到 history guard）。
    for (const auto [kind, format] : {std::pair{RgAttachmentKind::swapchain, "inherit"},
             {RgAttachmentKind::depth, "inherit"}, {RgAttachmentKind::depthOffscreen, "d32_sfloat"}}) {
        RgGraphDesc d;
        auto att = historyAtt("hist");
        att.kind = kind;
        att.format = format;
        if (kind == RgAttachmentKind::depthOffscreen)
            att.extent = {1024u, 1024u};
        d.attachments = {att};
        const auto g = compileRenderGraph(d);
        INFO("kind=" << static_cast<int>(kind) << " err=" << errOf(g));
        CHECK_FALSE(g.has_value());
        CHECK(errOf(g).find("history") != std::string::npos);
    }
    // 反向配对：同一条声明挂到 offscreen 上必须通过（否则上面三条可能只是图本身非法）
    CHECK(compileRenderGraph(historyGraph(/*history=*/true)).has_value());
}

TEST_CASE("decorateTaaPass 产物形状：两附件 + taa pass 插在 tonemap 前 + 消费者输入改写 + 帧末 commit") {
    // 图形态 = 两条路线的公共骨架：opaque 写 depth+scene_hdr，tonemap 采 scene_hdr 写 swapchain
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("scene_hdr", "rgba16_sfloat")};
    d.passes = {
        RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque", .outputs = {colorOut("scene_hdr"), depthOut("depth")}},
        RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
            .outputs = {colorOut("swapchain")}, .inputs = {sampleIn("scene_hdr")}},
    };
    REQUIRE(decorateTaaPass(d));

    // 附件：taa_out（普通 offscreen）+ taa_hist（history）
    const auto histIt = std::ranges::find(d.attachments, std::string{kTaaHistName},
        &RgAttachmentDesc::name);
    const auto outIt = std::ranges::find(d.attachments, std::string{kTaaOutName},
        &RgAttachmentDesc::name);
    REQUIRE(histIt != d.attachments.end());
    REQUIRE(outIt != d.attachments.end());
    CHECK(histIt->history == true);   // 变异红线：漏 history → 编译期即被「读先于写」打死
    CHECK(outIt->history == false);
    CHECK(histIt->format == std::string{kTaaFormat});

    // pass 序：taa 紧邻 tonemap 之前，帧末是 taa_commit(blit)
    size_t taaIdx = d.passes.size(), toneIdx = d.passes.size();
    for (size_t i = 0; i < d.passes.size(); ++i) {
        if (d.passes[i].name == kTaaPassName) taaIdx = i;
        if (d.passes[i].state == "tonemap") toneIdx = i;
    }
    REQUIRE(taaIdx != d.passes.size());
    CHECK(taaIdx + 1 == toneIdx);     // 变异红线：插到 tonemap 之后 → 滤波结果上不了屏
    CHECK(d.passes[taaIdx].kind == RgPassKind::drawLight);
    CHECK(d.passes[taaIdx].state == "taa");
    CHECK(d.passes.back().kind == RgPassKind::blit);
    CHECK(d.passes.back().name == std::string{kTaaCommitPassName});

    // taa pass 的输入 = 成片 + 深度 + 历史；输出 = taa_out
    const auto& taa = d.passes[taaIdx];
    REQUIRE(taa.outputs.size() == 1);
    CHECK(taa.outputs[0].attachment == std::string{kTaaOutName});
    for (std::string_view want : {"scene_hdr", "depth", "taa_hist"}) {
        const bool has = std::ranges::any_of(taa.inputs,
            [&](const RgPassAttachment& a) { return a.attachment == want && a.usage == RgUsage::sampleRead; });
        INFO(want);
        CHECK(has);
    }
    // 消费者的色输入被改写成 taa_out，且**不再**直接读 scene_hdr
    const auto& tone = d.passes[toneIdx];
    CHECK(std::ranges::any_of(tone.inputs,
        [&](const RgPassAttachment& a) { return a.attachment == std::string{kTaaOutName}; }));
    CHECK_FALSE(std::ranges::any_of(tone.inputs,
        [&](const RgPassAttachment& a) { return a.attachment == "scene_hdr"; }));

    // 整图能编译（阳性锚点：装饰产物必须自洽，任何一处形状错这里就红）
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
    // 历史图帧首 barrier 提档（装饰产物真的拿到了跨帧依赖，而不只是"图里有这附件"）
    const RgPassPlan* compiledTaa = nullptr;
    for (const auto& p : g->passes)
        if (p.name == kTaaPassName) compiledTaa = &p;
    REQUIRE(compiledTaa != nullptr);
    const auto* b = barrierFor(*compiledTaa, slotOf(*g, std::string{kTaaHistName}));
    REQUIRE(b != nullptr);
    CHECK(b->srcStage == vk::PipelineStageFlagBits2::eAllCommands);
    CHECK(b->srcAccess == vk::AccessFlagBits2::eMemoryWrite);
}

TEST_CASE("批 13.6：AO 施加点搬进 taa pass ⟹ 必须声明 ao 输入，且不得抢走「首个色附件」锚点") {
    // 【为什么这条值得单测】AO 在 taa pass 内施加 ⟹ 本 pass 开始**采样** ao 附件。图里少声明
    // 这条边 = 编译器不生成 ColorAttachmentOptimal → ShaderReadOnly 的布局转换 ⟹ 症状只在
    // 真机 VVL 上出现（无设备单测子集抓不到），所以这条边得由单测钉住。
    // 【另一半风险更阴】ao 附件也是**色**附件，而 picking / highlight / taa 三处装饰都按
    // 「首个 sampleRead 色附件」反查场景色（decorateHighlightPass::sampledColorName）
    // ⟹ ao 一旦排到 scene_hdr 前面，高亮 pass 会把 AO 图当场景色去写 = 画面直接花掉。
    auto build = [](bool withAo) {
        RgGraphDesc d;
        d.attachments = {swapchainAtt(), depthAtt(),
            offscreenAtt("scene_hdr", "rgba16_sfloat")};
        if (withAo)
            d.attachments.push_back(offscreenAtt("ao"));  // r8_unorm = kAoFormat
        d.passes = {
            RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
                .drawSource = "opaque", .outputs = {colorOut("scene_hdr"), depthOut("depth")}},
        };
        if (withAo)
            d.passes.push_back(RgPassDesc{.name = "ao", .kind = RgPassKind::drawLight,
                .state = "ao", .outputs = {colorOut("ao")}, .inputs = {sampleIn("depth")}});
        d.passes.push_back(RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight,
            .state = "tonemap", .outputs = {colorOut("swapchain")},
            .inputs = {sampleIn("scene_hdr")}});
        return d;
    };
    auto firstColorOf = [](const RgPassDesc& p) -> std::string {
        for (const auto& a : p.inputs)
            if (a.usage == RgUsage::sampleRead && a.attachment != "depth")
                return a.attachment;
        return {};
    };

    RgGraphDesc d = build(/*withAo=*/true);
    REQUIRE(decorateTaaPass(d));
    const auto it = std::ranges::find(d.passes, std::string{kTaaPassName}, &RgPassDesc::name);
    REQUIRE(it != d.passes.end());

    // ① 阳性锚点：taa pass 采 ao。删掉 decorateTaaPass 里那段 push_back → 本行红。
    CHECK(std::ranges::any_of(it->inputs, [&](const RgPassAttachment& a) {
        return a.attachment == std::string{kAoName} && a.usage == RgUsage::sampleRead; }));
    // 变异红线：删掉 AO 输入声明 → ① 红（真机表现 = 采样未转布局，VVL 报 VUID-VkCmdDraw*）
    // ② 锚点顺序：首个 sampleRead 色附件**仍是** scene_hdr。
    // 变异红线：把 AO 排到 inputs 首位 → 本行红（真机表现 = 高亮/装饰锚到 AO 图上，画面花）
    CHECK(firstColorOf(*it) == "scene_hdr");
    // 且 AO 排在深度之后、历史之前（输入序 = barrier 发射序：AO 是本帧刚写的色附件）
    const auto idxOf = [&](std::string_view n) {
        size_t i = 0;
        for (; i < it->inputs.size(); ++i)
            if (it->inputs[i].attachment == n) break;
        return i;
    };
    CHECK(idxOf("depth") < idxOf(std::string_view{kAoName}));
    CHECK(idxOf(std::string_view{kAoName}) < idxOf(std::string_view{kTaaHistName}));

    // ③ 反向配对：**没有** ao 附件的图形态不得凭空声明它（漏了存在性判断 → 这里会拿到一条
    // 指向不存在附件的输入；而"无 ao 图仍能编译"这条 guard 若写成"永远不加 AO"也会让 ① 红）
    RgGraphDesc d2 = build(/*withAo=*/false);
    REQUIRE(decorateTaaPass(d2));
    const auto it2 = std::ranges::find(d2.passes, std::string{kTaaPassName}, &RgPassDesc::name);
    REQUIRE(it2 != d2.passes.end());
    CHECK_FALSE(std::ranges::any_of(it2->inputs,
        [&](const RgPassAttachment& a) { return a.attachment == std::string{kAoName}; }));

    // ④ 整图编译通过（阳性锚点：装饰产物自洽，布局链推得出来）
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
}

TEST_CASE("decorateTaaPass 的图形态门槛：没有「写 swapchain 且采色」的 pass → 返回 false 且不动图") {
    // offscreen DIAG 变体用 blit 上屏（transferRead ≠ sampleRead）→ 不支持。
    // 反向配对：加一个采色写 swapchain 的 pass 就必须返回 true（否则这条门槛可能是恒 false）。
    RgGraphDesc d;
    d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("scene_rt", "rgba16_sfloat")};
    d.passes = {
        RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
            .drawSource = "opaque", .outputs = {colorOut("scene_rt"), depthOut("depth")}},
        RgPassDesc{.name = "composite", .kind = RgPassKind::blit,
            .outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::transferWrite}},
            .inputs = {RgPassAttachment{.attachment = "scene_rt", .usage = RgUsage::transferRead}}},
    };
    const size_t attsBefore = d.attachments.size();
    const size_t passesBefore = d.passes.size();
    CHECK_FALSE(decorateTaaPass(d));
    CHECK(d.attachments.size() == attsBefore);   // 失败路径不得留下半成品附件
    CHECK(d.passes.size() == passesBefore);

    d.passes.push_back(RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight,
        .state = "tonemap", .outputs = {colorOut("swapchain")}, .inputs = {sampleIn("scene_rt")}});
    CHECK(decorateTaaPass(d));
}

/**
 * 批 14.5 第三批：AO 施加专用 pass 的装饰产物。
 *
 * 【为什么这条值得单测】本 pass 一落地就同时踩两条既有纪律的刀刃：
 *  ① 它**采样** ao 附件 ⟹ 图里少声明这条边 = 编译器不生成 ColorAttachment→ShaderReadOnly
 *     转换，症状只在真机 VVL 上出现（无设备单测子集抓不到）。同 decorateTaaPass 挂 ao
 *     输入那条教训（本文件上方）。
 *  ② ao 也是**色**附件，而所有装饰都按「首个 sampleRead 色附件」反查场景色 ⟹ ao 一旦排到
 *     成片前面，后续装饰会把 AO 图当场景色去写 = 画面直接花掉。
 * 外加本 pass 独有的第三条：它存在的意义就是「AO 只施一次」，所以**跳过条件比产物更需要
 * 钉**——跳过条件失效的后果（AO 乘两次）是画面整体变暗，肉眼容易被当成"材质变暗了"放过。
 */
TEST_CASE("decorateAoApplyPass 产物形状：ao_applied 附件 + pass 插在 tonemap 前 + 消费者改写 + 无帧末 copy") {
    auto build = []() {
        RgGraphDesc d;
        d.attachments = {swapchainAtt(), depthAtt(),
            offscreenAtt("scene_hdr", "rgba16_sfloat"), offscreenAtt("ao")};
        d.passes = {
            RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
                .drawSource = "opaque", .outputs = {colorOut("scene_hdr"), depthOut("depth")}},
            RgPassDesc{.name = "ao", .kind = RgPassKind::drawLight, .state = "ao",
                .outputs = {colorOut("ao")}, .inputs = {sampleIn("depth")}},
            RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight, .state = "tonemap",
                .outputs = {colorOut("swapchain")},
                .inputs = {sampleIn("scene_hdr"), sampleIn("ao")}},
        };
        return d;
    };

    RgGraphDesc d = build();
    const size_t passesBefore = d.passes.size();
    REQUIRE(decorateAoApplyPass(d));

    // ① 附件：offscreen + rgba16_sfloat，且**不是** history（本 pass 不跨帧；误标 history
    //    会在帧首多一条 AllCommands barrier，且语义上假装自己是历史图）
    const auto att = std::ranges::find(d.attachments, std::string{kAoAppliedName},
        &RgAttachmentDesc::name);
    REQUIRE(att != d.attachments.end());
    CHECK(att->kind == RgAttachmentKind::offscreen);
    CHECK(att->format == std::string{kAoAppliedFormat});
    CHECK_FALSE(att->history);

    // ② pass 位置在 tonemap **之前**（AO 必须在消费者之前施加；插在之后 = 白插一 pass）
    const auto passIt = std::ranges::find(d.passes, std::string{kAoApplyPassName}, &RgPassDesc::name);
    REQUIRE(passIt != d.passes.end());
    const auto toneIt = std::ranges::find(d.passes, std::string{"tonemap"}, &RgPassDesc::name);
    REQUIRE(toneIt != d.passes.end());
    CHECK(passIt < toneIt);
    CHECK(passIt->state == std::string{kAoApplyPassName});

    // ③ 输入序（= barrier 发射序）：成片在前、ao 在后。
    // 变异红线：把两条 inputs 调换 → 本段红（真机表现 = 后续装饰把 AO 图认成场景色）
    REQUIRE(passIt->inputs.size() == 2);
    CHECK(passIt->inputs[0].attachment == "scene_hdr");
    CHECK(passIt->inputs[0].usage == RgUsage::sampleRead);
    CHECK(passIt->inputs[1].attachment == std::string{kAoName});
    CHECK(passIt->inputs[1].usage == RgUsage::sampleRead);
    // 变异红线：删掉 ao 输入声明 → 上面 inputs.size() 与 [1] 两行红（真机 = 采样未转布局）

    // ④ 消费者的色输入换成 ao_applied，且**不再**直接读 scene_hdr
    //   （不改写 = tonemap 仍吃未乘 AO 的成片，而施加点标志已置真 ⟹ AO 一次都不施加）
    CHECK(std::ranges::any_of(toneIt->inputs,
        [&](const RgPassAttachment& a) { return a.attachment == std::string{kAoAppliedName}; }));
    CHECK_FALSE(std::ranges::any_of(toneIt->inputs,
        [&](const RgPassAttachment& a) { return a.attachment == "scene_hdr"; }));

    // ⑤ **只多一个 pass**：本装饰没有帧末 copy（不跨帧，与 decorateTaaPass 的关键差别）
    CHECK(d.passes.size() == passesBefore + 1);

    // ⑥ 整图能编译（阳性锚点：装饰产物必须自洽，任何一处形状错这里就红）
    const auto g = compileRenderGraph(d);
    INFO(errOf(g));
    REQUIRE(g.has_value());
    // ⑦ 图真的承认了这两条写→读边（不只是"图里有这附件"）：ao_apply 读 ao、tonemap 读
    //    ao_applied，两者的 newLayout 都必须是 ShaderReadOnly。
    // 变异红线：把 usage 从 sampleRead 写成别的 → 本段红（真机 = 采样 COLOR_ATTACHMENT 布局）
    const RgPassPlan* compiled = nullptr;
    for (const auto& p : g->passes)
        if (p.name == kAoApplyPassName) compiled = &p;
    REQUIRE(compiled != nullptr);
    const auto* bAo = barrierFor(*compiled, slotOf(*g, std::string{kAoName}));
    REQUIRE(bAo != nullptr);
    CHECK(bAo->newLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
    // srcAccess 取的是**上一条使用者**的 access（帧内写→读），ao 的上一条 = ao pass 的
    // colorWrite ⟹ eColorAttachmentWrite。别抄成 eMemoryWrite——那是 history 附件跨
    // submission 才有的分支（见 planBarrier 的帧间分析注释）。
    CHECK(bAo->srcAccess == vk::AccessFlagBits2::eColorAttachmentWrite);
    const RgPassPlan* compiledTone = nullptr;
    for (const auto& p : g->passes)
        if (p.name == "tonemap") compiledTone = &p;
    REQUIRE(compiledTone != nullptr);
    const auto* bApplied = barrierFor(*compiledTone, slotOf(*g, std::string{kAoAppliedName}));
    REQUIRE(bApplied != nullptr);
    CHECK(bApplied->newLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
}

TEST_CASE("decorateAoApplyPass 三条跳过条件：taa 在图里 / 无 ao 附件 / 无采色上屏 pass → false 且不动图") {
    auto build = [](bool withAo) {
        RgGraphDesc d;
        d.attachments = {swapchainAtt(), depthAtt(),
            offscreenAtt("scene_hdr", "rgba16_sfloat")};
        if (withAo)
            d.attachments.push_back(offscreenAtt("ao"));
        d.passes = {
            RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
                .drawSource = "opaque", .outputs = {colorOut("scene_hdr"), depthOut("depth")}},
        };
        if (withAo)
            d.passes.push_back(RgPassDesc{.name = "ao", .kind = RgPassKind::drawLight,
                .state = "ao", .outputs = {colorOut("ao")}, .inputs = {sampleIn("depth")}});
        d.passes.push_back(RgPassDesc{.name = "tonemap", .kind = RgPassKind::drawLight,
            .state = "tonemap", .outputs = {colorOut("swapchain")},
            .inputs = {sampleIn("scene_hdr")}});
        return d;
    };
    auto unchanged = [](const RgGraphDesc& d, size_t a, size_t p) {
        CHECK(d.attachments.size() == a);   // 失败路径不得留下半成品附件
        CHECK(d.passes.size() == p);
    };

    // ①【最重要的一条】taa 在图里 ⟹ 不插。**这就是「AO 只施一次」的护栏**：AO 施加点在
    //   taa.frag 里（批 13.6），再插本 pass = 乘两次 = 遮蔽处全黑。
    //   用真实的调用次序复现（main_impl 就是先 decorateTaaPass 再本装饰），比手搓图更硬。
    //   变异红线：删掉 decorateAoApplyPass 开头那个 taa 检查 → 本行红
    //   （真机表现 = 开 TAA 后 AO 边缘明显发黑，容易被误读成"材质变暗"）。
    {
        RgGraphDesc d = build(/*withAo=*/true);
        REQUIRE(decorateTaaPass(d));   // 前置条件：taa 真在图里
        const size_t a = d.attachments.size(), p = d.passes.size();
        CHECK_FALSE(decorateAoApplyPass(d));
        unchanged(d, a, p);
    }

    // ② 无 ao 附件（无 SSAO 的图形态）⟹ 没有可见度可乘，不插。
    //   变异红线：把 hasAo 判据删掉 → 本行红（真机 = 装饰出一个采不存在附件的 pass，
    //   编译期即报"未知附件"，图起不来）
    {
        RgGraphDesc d = build(/*withAo=*/false);
        const size_t a = d.attachments.size(), p = d.passes.size();
        CHECK_FALSE(decorateAoApplyPass(d));
        unchanged(d, a, p);
    }

    // ③ 没有「写 swapchain 且采色」的 pass（offscreen DIAG 变体走 blit）⟹ 无锚点，不插。
    {
        RgGraphDesc d;
        d.attachments = {swapchainAtt(), depthAtt(), offscreenAtt("scene_rt", "rgba16_sfloat"),
            offscreenAtt("ao")};
        d.passes = {
            RgPassDesc{.name = "opaque", .kind = RgPassKind::drawMesh, .state = "opaque",
                .drawSource = "opaque", .outputs = {colorOut("scene_rt"), depthOut("depth")}},
            RgPassDesc{.name = "composite", .kind = RgPassKind::blit,
                .outputs = {RgPassAttachment{.attachment = "swapchain", .usage = RgUsage::transferWrite}},
                .inputs = {RgPassAttachment{.attachment = "scene_rt", .usage = RgUsage::transferRead}}},
        };
        const size_t a = d.attachments.size(), p = d.passes.size();
        CHECK_FALSE(decorateAoApplyPass(d));
        unchanged(d, a, p);
    }

    // ④ 阳性配对：三条都不命中时必须插得成（防上面三条门槛被写成"恒 false"）
    {
        RgGraphDesc d = build(/*withAo=*/true);
        CHECK(decorateAoApplyPass(d));
    }
}

}  // namespace RGL
