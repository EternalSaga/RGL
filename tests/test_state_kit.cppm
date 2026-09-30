module;
/**
 * test_state_kit —— StateKit 九段动态状态表（批 12.2d）
 *
 * 为什么值得测：StateKit 是**执行器与着色器之间唯一的状态契约面**。九段里有好几处
 * 「反直觉但有理由」的设定（skybox 用 LEQUAL、picking 用 LEQUAL 而非 LESS、transparent
 * 剔背面、shadow depth-only 且双面写入），它们一旦被改动，症状是画面慢慢变得不对而
 * 没有任何报错——正是本项目最贵的那类 bug（阴影链 5a-3→5a-9 七轮里有若干轮是状态
 * 被改坏，而当时所有数值指标全绿）。
 *
 * 本模块 import rlvk（RenderState 里全是 vk:: 枚举），但**不需要任何 VkDevice**：
 * 构造与查表都是纯内存操作。故它能在 `just test`（无 GPU 子集）里跑。
 *
 * ## 一条与直觉相反的事实（本文件的核心发现之一）
 *
 * 九段**不是**九种不同形态，只有四种。把 (cullMode, depthCompareOp, depthTest,
 * depthWrite, colorBlend) 取成指纹后实际分组是：
 *
 *   A = opaque / shadow        : None, Less, 测, 写, 不混
 *   B = light / tonemap / ao   : None, Less, 不测, 不写, 不混
 *   C = skybox / picking       : None, LEQ,  测, 不写, 不混
 *   D = transparent            : Back, Less, 测, 不写, 混
 *   E = highlight              : Back, LEQ,  测, 不写, 混
 *
 * 「九段两两不同」是**错的**（tonemap/ao 与 light 同形、picking 与 skybox 同形、
 * shadow 与 opaque 同形）。真正的不变量是**地址**：stateFor 返回各成员存储的引用，
 * 九个名字必须映射到九个不同对象，否则执行器改一段会连带改另一段。故「九段不同」
 * 改成断言地址互异 + 指纹分组如上。
 */
#include <doctest/doctest.h>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import rlstd 带进来
                         // （clang 需要本 TU 的 GMF 里有一个真 STL 头才声明它；只 include
                         //  doctest 不够）——少了这行，`for (x : {a, b})` 报 "initializer_list
                         //  was not found"。同文件用 <cmath> 的 test_camera 是巧合受益者。

export module test_state_kit;

import rlstd;
import rlvk;
import state_kit;
import shader;  // RenderState

namespace RGL {
namespace {

/** 逐字段比对两个 RenderState（19 个可见字段），失败时指名道姓而不是只报 "!="。 */
void checkSameState(const RenderState& got, const RenderState& want, const char* what) {
    INFO(what);
    CHECK(got.polygonMode == want.polygonMode);
    CHECK(got.cullMode == want.cullMode);
    CHECK(got.frontFace == want.frontFace);
    CHECK(got.rasterizerDiscard == want.rasterizerDiscard);
    CHECK(got.depthBiasEnable == want.depthBiasEnable);
    CHECK(got.depthTest == want.depthTest);
    CHECK(got.depthClipEnable == want.depthClipEnable);
    CHECK(got.depthWrite == want.depthWrite);
    CHECK(got.depthCompareOp == want.depthCompareOp);
    CHECK(got.depthBoundsTest == want.depthBoundsTest);
    CHECK(got.stencilTest == want.stencilTest);
    CHECK(got.rasterizationSamples == want.rasterizationSamples);
    CHECK(got.sampleMask == want.sampleMask);
    CHECK(got.alphaToCoverage == want.alphaToCoverage);
    CHECK(got.alphaToOne == want.alphaToOne);
    CHECK(got.colorBlend == want.colorBlend);
    CHECK(got.colorWriteMask == want.colorWriteMask);
    CHECK(got.logicOp == want.logicOp);
}

constexpr RenderState kDefaults{};  // 全默认

/** 状态指纹（只取描述形态的 5 个字段；其余 14 个字段九段全同，另由逐字段比对覆盖）。 */
std::array<int, 5> fingerprint(const RenderState& s) {
    return {static_cast<int>(s.cullMode), static_cast<int>(s.depthCompareOp),
        s.depthTest ? 1 : 0, s.depthWrite ? 1 : 0, s.colorBlend ? 1 : 0};
}

constexpr std::array<std::string_view, 9> kAllNames{
    "opaque", "skybox", "transparent", "light", "tonemap", "ao", "picking", "highlight", "shadow"};

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// 组 1：opaque —— 全默认，且是**反向锚点**
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("opaque 段 = RenderState 全默认（反向用例：任何字段偏离都判红）") {
    const StateKit kit{/*depthClipEnable=*/true};
    // 这条是**反向用例**：不测"它做了什么"，测"它**没**做什么"。若哪天有人给 opaque
    // 加了 cullMode / depthWrite 之类改动想试效果，这条立刻红——而那正是 5a-7
    // 「验的是退化数据」的同类：状态被改而指标全绿。
    checkSameState(kit.stateFor("opaque"), kDefaults, "opaque 应逐字段等于默认值");
    // 内建 cube 绕序**朝内**（test_cube_geometry 实测 12/12 dot=−0.5，法线指向中心），
    // 故 opaque 段 cullMode=eNone 是必须的，不是可选优化。改成 eBack 会剔掉全部朝外面。
    CHECK(kit.stateFor("opaque").cullMode == vk::CullModeFlagBits::eNone);
    CHECK(kit.stateFor("opaque").colorBlend == false);
    CHECK(kit.stateFor("opaque").depthWrite == true);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 2：transparent —— 不写深度 + 开混合 + 剔背面（差异**恰为三项**）
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("transparent 段：与默认的差异恰为三项（不写深度 / 开混合 / 剔背面）") {
    const StateKit kit{true};
    const RenderState& t = kit.stateFor("transparent");
    // 逐项钉住，且用「默认 + 三项改动」的完整比对把**其余 16 个字段**也锁住 ——
    // 只 CHECK 三个字段的话，"顺手多改一项"（例如误加 depthBiasEnable）无人察觉。
    RenderState expect = kDefaults;
    expect.depthWrite = false;
    expect.colorBlend = true;
    expect.cullMode = vk::CullModeFlagBits::eBack;
    checkSameState(t, expect, "transparent 应恰为「默认 + 三项差异」");
}

TEST_CASE("transparent 三项差异各自守一个失效模式（逐项独立断言）") {
    const StateKit kit{true};
    const RenderState& t = kit.stateFor("transparent");
    CHECK(t.depthWrite == false);      // 关掉 → 后画的透明体会盖住先画的，alpha 语义崩
    CHECK(t.colorBlend == true);       // 关掉 → alpha 通道被丢弃，透明度设置完全无效
    // 关掉背面剔除 → 封闭网格正反两面同屏各混合一次，有效 alpha 翻倍
    //（实测 0.25 → 1−(1−0.25)² = 0.4375，ctor 注释记的原始值）
    CHECK(t.cullMode == vk::CullModeFlagBits::eBack);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 3：skybox —— 大三角 z=1（远平面）故必须 LEQUAL
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("skybox 段：LEQUAL + 不写深度 + 不剔除（只在未被几何占据的像素通过）") {
    const StateKit kit{true};
    // LEQUAL 而非 LESS：大三角在 z=1（远平面），深度 clear 也是 1.0，数值恰好相等。
    // 用 LESS 会让天空**永远画不出来**——这是"恰好相等"型依赖的经典。
    RenderState expect = kDefaults;
    expect.depthCompareOp = vk::CompareOp::eLessOrEqual;
    expect.depthWrite = false;
    checkSameState(kit.stateFor("skybox"), expect, "skybox 应恰为「默认 + LEQUAL + 不写深度」");
    CHECK(kit.stateFor("skybox").cullMode == vk::CullModeFlagBits::eNone);  // 绕序不剔
}

TEST_CASE("skybox 用 LEQUAL 而 opaque 用 LESS（对照：同库内两种 CompareOp 都存在）") {
    // 反向锚点：把任一边的期望改掉都会立刻红——证明这两条断言**会**红，
    // 而不是"恒真的摆设"（AGENTS.md：会绿 = 无效断言）。
    const StateKit kit{true};
    CHECK(kit.stateFor("skybox").depthCompareOp == vk::CompareOp::eLessOrEqual);
    CHECK(kit.stateFor("opaque").depthCompareOp == vk::CompareOp::eLess);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 4：deferred 终图三段（light / tonemap / ao）—— 形态相同
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("light / tonemap / ao 三段同形：全屏终图，无深度附件故 depthTest 必须关") {
    const StateKit kit{true};
    for (const char* name : {"light", "tonemap", "ao"}) {
        INFO("pass state = " << name);
        const RenderState& s = kit.stateFor(name);
        // 这三段渲染到**不挂深度附件**的 RT。depthTest 开着会因无可测深度而产生
        // 未定义行为（规范上深度附件缺席时的 depthTest 行为未定义）。
        RenderState expect = kDefaults;
        expect.depthTest = false;
        expect.depthWrite = false;
        checkSameState(s, expect, name);
        CHECK(s.cullMode == vk::CullModeFlagBits::eNone);  // 大三角绕序不剔
        CHECK(s.colorBlend == false);
    }
}

TEST_CASE("tonemap / ao 与 light 逐字段完全相同（批 11.4 起 ao pass 走同一形态）") {
    const StateKit kit{true};
    // 这三条是**故意**相同，故断言相同本身有价值：任一段被单独改动都会在这里暴露。
    // 措辞要说清"这是期望"，否则读者会以为这是待修的重复。
    checkSameState(kit.stateFor("tonemap"), kit.stateFor("light"), "tonemap 应与 light 同形");
    checkSameState(kit.stateFor("ao"), kit.stateFor("light"), "ao 应与 light 同形");
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 5：picking / highlight —— 两个调试叠加段
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("picking 段：depth 只测不写 + LEQUAL（eLess 全拒 = 实测踩过的坑）") {
    const StateKit kit{true};
    const RenderState& p = kit.stateFor("picking");
    CHECK(p.depthWrite == false);  // 写了就污染后续 transparent 段的测深
    // 与 scene depth **同面等值** → 必须 LEQUAL。源码注释明写「eLess 全拒 = 实测踩坑」：
    // 被拾取像素的深度恰好等于 scene depth 里存的同面深度，LESS 会把它全拒掉。
    CHECK(p.depthCompareOp == vk::CompareOp::eLessOrEqual);
    CHECK(p.depthTest == true);
    CHECK(p.colorBlend == false);
    CHECK(p.cullMode == vk::CullModeFlagBits::eNone);  // 与 opaque 一致即可
}

TEST_CASE("highlight 段：LEQUAL + 不写深度 + 开混合 + 剔背面") {
    const StateKit kit{true};
    const RenderState& h = kit.stateFor("highlight");
    // 与 picking 同为「借 scene depth 做可见性测试」→ 同样 LEQUAL + 不写深度
    CHECK(h.depthCompareOp == vk::CompareOp::eLessOrEqual);
    CHECK(h.depthWrite == false);
    // 高亮是叠加在成片上的半透明边缘光 → 必须开混合
    CHECK(h.colorBlend == true);
    // 与 transparent 同理：双面材质不剔则正反两面各混合一次 = 边缘光翻倍
    CHECK(h.cullMode == vk::CullModeFlagBits::eBack);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 6：shadow —— depth-only，双面写入
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("shadow 段：depth-only，LESS 写光视最近深度 + cull=None 双面写入") {
    const StateKit kit{true};
    const RenderState& s = kit.stateFor("shadow");
    // ① 写深度：是深度生产者
    CHECK(s.depthWrite == true);
    // ② LESS：阴影图存光视空间最近深度，clear 1.0，LESS 保留最近者
    CHECK(s.depthCompareOp == vk::CompareOp::eLess);
    // ③ cull=None：双面写入。依据是 RADV 无保守光栅外扩余量
    //（maxExtraPrimitiveOverestimationSize = 0，薄几何/单面片无法靠 fattening 挡漏光），
    // 剔背面会让单面几何完全不出现在阴影图里 → 漏光。
    CHECK(s.cullMode == vk::CullModeFlagBits::eNone);
    // depthTest 保持 true：阴影 pass 挂深度附件，只是**只测不参与**后续语义。
    // 源码注释说「depth-only（无色附件）」——所以 colorWriteMask 的取值无关紧要
    //（该 pass 没有颜色附件可写），**不是**必须清零。别把它当契约。
    CHECK(s.colorBlend == false);
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 7：stateFor 分派 —— 地址互异 + 未知名抛异常
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("stateFor 九个名字映射到九个**不同**的存储对象（地址互异）") {
    const StateKit kit{true};
    // 契约是「返回成员存储的引用」。若某两个名字映射到同一成员，执行器改一段会
    // 连带改另一段——而两者形态本就不同（见分组测试），症状是画面局部错乱无报错。
    // 这里断言的是**地址**不是值：值相同是合法的（tonemap/ao 与 light 就同形）。
    std::array<const RenderState*, 9> addrs{};
    for (size_t i = 0; i < kAllNames.size(); ++i) {
        INFO("state = " << kAllNames[i]);
        addrs[i] = &kit.stateFor(kAllNames[i]);
    }
    for (size_t i = 0; i < addrs.size(); ++i)
        for (size_t j = i + 1; j < addrs.size(); ++j) {
            INFO("compare " << kAllNames[i] << " vs " << kAllNames[j]);
            CHECK(addrs[i] != addrs[j]);
        }
}

TEST_CASE("九段实际只有五种形态（分组如实记录，防有人悄悄加一段而静默扩面）") {
    // 文件头那份分组表就是从这条断言的期望反推出来的。分组数若变化，说明有人动过
    // 某段的状态——那时**先问是不是有意的**，再改这条。
    const StateKit kit{true};
    std::set<std::array<int, 5>> sigs;
    for (const auto n : kAllNames)
        sigs.insert(fingerprint(kit.stateFor(n)));
    INFO("实际形态数 = " << sigs.size());
    CHECK(sigs.size() == 5);
    // 逐组点名，避免"数量对了但分组错了"（数量相同的前提下）
    CHECK(fingerprint(kit.stateFor("opaque")) == fingerprint(kit.stateFor("shadow")));
    CHECK(fingerprint(kit.stateFor("light")) == fingerprint(kit.stateFor("tonemap")));
    CHECK(fingerprint(kit.stateFor("light")) == fingerprint(kit.stateFor("ao")));
    CHECK(fingerprint(kit.stateFor("skybox")) == fingerprint(kit.stateFor("picking")));
    // 组间必须真的不同：任取一对跨组的做不等断言
    CHECK(fingerprint(kit.stateFor("opaque")) != fingerprint(kit.stateFor("transparent")));
    CHECK(fingerprint(kit.stateFor("skybox")) != fingerprint(kit.stateFor("highlight")));
    CHECK(fingerprint(kit.stateFor("light")) != fingerprint(kit.stateFor("picking")));
}

TEST_CASE("stateFor 未知名抛 std::runtime_error（不是静默回落默认）") {
    const StateKit kit{true};
    // 为什么必须抛：执行器构造期有 validateRenderGraphForExecutor 保证图内名字全
    // 可解析，走到 stateFor 的未知名 = 校验被绕过，是编程错误。静默回落默认值会让它
    // 变成「图里写错名字 → 整场用某个默认状态渲染」且无任何日志。
    CHECK_THROWS_AS(static_cast<void>(kit.stateFor("nosuchstate")), std::runtime_error);
    CHECK_THROWS_AS(static_cast<void>(kit.stateFor("")), std::runtime_error);
    CHECK_THROWS_AS(static_cast<void>(kit.stateFor("Opaque")), std::runtime_error);  // 大小写敏感
}

// ═══════════════════════════════════════════════════════════════════════════
// 组 8：edS3 能力门
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("depthClipDynamicEnabled 忠实反映构造入参（edS3 能力门）") {
    // 设备不支持 extendedDynamicState3DepthClipEnable 时 applyDynamicState 必须
    // 干净 no-op（"扩展缺失不 set"原则）。这条锁住门控本身。
    CHECK(StateKit{true}.depthClipDynamicEnabled() == true);
    CHECK(StateKit{false}.depthClipDynamicEnabled() == false);
    // 反向用例：门为关时各段 depthClipEnable 默认值**不变**（仍是 true，真正的跳过
    // 发生在 applyDynamicState）。若这里被改成随门置 false，就等于把"不 set 命令"
    // 错译成"关闭深度裁剪"——那是两种完全不同的行为，前者无害后者画面错。
    CHECK(StateKit{false}.stateFor("opaque").depthClipEnable == true);
    CHECK(StateKit{false}.stateFor("shadow").depthClipEnable == true);
}

}  // namespace RGL
