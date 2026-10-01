module;
/**
 * test_conservative_mesh —— [gpu] 批 11.3 子批 1b 成败门：保守光栅 × mesh shader × RADV
 * （docs/hfts_izb_plan.md §4.1；FTIZB 路线最大单项风险，不过门即走 fallback/收手）
 *
 * ## 为什么这是"门"而不是普通用例
 *
 * FTIZB 的可见性 pass 依赖：光空间**保守光栅**画遮挡三角形（OVERESTIMATE：像素方格与
 * 三角形有重叠就出 fragment），fragment 里遍历 IZB 链表。论文全部是传统 GL 光栅管线，
 * 本引擎是 mesh shader 管线——**RADV 上保守光栅状态 × mesh 派生图元从未被任何人实测过**
 * （phase11_plan §9 只查了 vulkaninfo 属性面）。本用例把"能不能用"变成可重跑的断言。
 *
 * ## 探针几何与闭式解（tests/shaders/izb_spike.mesh.slang，64×64 R8G8B8A8_UNORM）
 *
 *   wg0 亚像素三角：严格在像素 (17,13) 内部、不含像元中心。
 *     中心采样（保守关）= 0 像素【负向锚点】；OVERESTIMATE = 恰 1 像素 @(17,13)【门】。
 *   wg1 大三角：T = {x≥36.25 ∧ y≥36.25 ∧ x+y≤96.5}（三边避开像元中心/整边界歧义）。
 *     中心采样：i,j≥36 ∧ i+j≤95 → 24·25/2 = **300**（逐点数过，非面积近似）。
 *     保守：方格[i,i+1]×[j,j+1]∩T≠∅ ⟺ i,j≥36 ∧ i+j≤96 → 25·26/2 = **325**。
 *     （325 逐格含真实面积非零交区；边界接触不存在——边全在 .25/.5 上，方格边界在整数。）
 *   两区按 x<32 / x≥32 分离计数：亚像素区被大三角污染 = 0，反之亦然。
 *
 * ## 坐标系事实（本探针首跑实测抓获，2026-10-01）
 *
 * Vulkan NDC 的 y 向下（framebuffer 原点左上），而探针着色器按 y-up 写（与引擎 glm 约定
 * 一致）——正 viewport 下首跑 y 镜像（像素 13 ↔ 50，位置锚点抓到）。引擎的负高度
 * viewport（shader.cppm applyDynamicState：`vk::Viewport(x, yTop, w, -h)`）就是在补偿
 * 这一层，本测试同款负 viewport 录制后 y-up 约定即成立。这不是 RADV/mesh 的怪癖，是
 * spec 行为；**1d 真实集成必须继承引擎那对翻转，别在别处再补一刀**。
 *
 * ## harness 自证（阳性锚点的锚点）
 *
 * 基线用例（保守 OFF）断言 big == 300 **且** sub == 0：若 NDC 翻向/视口映射/绘制数错，
 * 300 或位置断言立刻红——"保守用例绿"必须建立在"基线用例也绿"之上，否则可能只是
 * "什么都没画对但保守恰好没多出像素"。亚像素位置断言 (17,13) 精确抓 y/x 翻向。
 *
 * ## 变异红线声明（哪个变异让我红）
 *   rasterization.pNext 摘掉保守结构（保守从未生效）→ 静态用例红（sub 0≠1、big 300≠325）
 *   动态 set 调用不执行/被驱动吞掉 → 动态用例两向红（DISABLED 管线→OVER 期望 sub=1 得 0；
 *                                          OVER 管线→DISABLED 期望 sub=0 得 1）
 *   dynamicStates 漏加 eConservativeRasterMode → 被 VVL 打断（带 VVL 跑，fixture 硬要求）
 *   着色器 rig 坐标错（翻向/比例）→ 基线用例 big≠300 或位置≠(17,13) 红（用例集自证）
 *
 * ## 能力口径（FAIL 不 SKIP，gpu 套件纪律）
 *
 * 本机（RADV Raphael）必须给出 extendedDynamicState3ConservativeRasterizationMode = true
 * 且四条光栅 golden 全中；同时把 phase11_plan §9 的两个 vulkaninfo 事实（OVERESTIMATE
 * 基线可用、maxExtraPrimitiveOverestimationSize = 0）钉成 golden——Mesa 升级若变，这里
 * 先响，别让 1d 拿旧假设写代码。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>  // VK_QUEUE_FAMILY_IGNORED 等宏（rlvk 不含宏）
#include <initializer_list>
#include <format>
#include <fstream>
#include <cstring>

export module test_conservative_mesh;

import rlstd;
import rlvk;
import test_device_primitives;  // Fixture / makeImage / readImage（引擎同款 instance→device 链路）

namespace RGL::testing::izb {

using testing::gpu::Fixture;  // 引擎同款 AppContext→DeviceContext 夹具（gpu 套件共享）

namespace {

constexpr uint32_t kSize = 64;  // 探针渲染目标边长
constexpr vk::Format kFmt = vk::Format::eR8G8B8A8Unorm;
constexpr uint32_t kSplitX = 32;               // 亚像素区 x<32 / 大三角区 x≥32
constexpr int kBigCenter = 300;                // 中心采样闭式解（推导见文件头）
constexpr int kBigConservative = 325;          // 保守覆盖闭式解
constexpr int kSubpxX = 17, kSubpxY = 13;      // 亚像素三角所在像素

std::vector<uint32_t> readSpirv(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f)
        throw std::runtime_error("izb_spike: SPIR-V 读不到（spv 未构建？路径对吗）: " + path);
    const auto n = static_cast<size_t>(f.tellg());
    std::vector<uint8_t> bytes(n);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(n));
    if (n % 4 != 0)
        throw std::runtime_error("izb_spike: SPIR-V 长度非 4 倍数: " + path);
    std::vector<uint32_t> code(n / 4);
    std::memcpy(code.data(), bytes.data(), n);
    return code;
}

vk::raii::ShaderModule makeModule(const Fixture& fx, const std::string& path) {
    const auto code = readSpirv(path);
    vk::ShaderModuleCreateInfo mi{};
    mi.setCode(code);
    return vk::raii::ShaderModule(fx.device(), mi);
}

/** color attachment 图 + view（view 合法：usage 含 COLOR_ATTACHMENT，不踩 VUID-04441）。 */
struct Target {
    testing::gpu::Image2D img;
    vk::raii::ImageView view;
};

Target makeTarget(const Fixture& fx) {
    auto img = testing::gpu::makeImage(fx, kFmt, kSize, kSize,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc);
    vk::ImageViewCreateInfo vi{};
    vi.image = *img.image;
    vi.viewType = vk::ImageViewType::e2D;
    vi.format = kFmt;
    vi.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    vk::raii::ImageView view(fx.device(), vi);
    return Target{.img = std::move(img), .view = std::move(view)};
}

void barrier(vk::raii::CommandBuffer& cmd, const vk::raii::Image& img,
    vk::ImageLayout oldL, vk::ImageLayout newL, vk::PipelineStageFlags2 srcSt,
    vk::AccessFlags2 srcAcc, vk::PipelineStageFlags2 dstSt, vk::AccessFlags2 dstAcc) {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = srcSt;
    b.srcAccessMask = srcAcc;
    b.dstStageMask = dstSt;
    b.dstAccessMask = dstAcc;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.image = img;
    b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vk::DependencyInfo dep{};
    dep.setImageMemoryBarriers(b);
    cmd.pipelineBarrier2(dep);
}

/**
 * 建 mesh+frag 光栅管线（保守模式 = 静态状态；dynamicMode = 额外把保守模式列进
 * DynamicState3 动态名单）。mesh 管线刻意**不填** pVertexInputState/pInputAssemblyState
 * （spec：有 mesh stage 时二者 ignored）。cullMode=NONE：探针只测覆盖，绕序不进判据。
 */
vk::raii::Pipeline makePipeline(const Fixture& fx, const vk::raii::PipelineLayout& layout,
    const vk::raii::ShaderModule& meshMod, const vk::raii::ShaderModule& fragMod,
    vk::ConservativeRasterizationModeEXT mode, bool dynamicMode) {
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eMeshEXT, *meshMod, "main"},
        {vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eFragment, *fragMod, "main"},
    }};
    vk::PipelineRasterizationConservativeStateCreateInfoEXT cons{};
    cons.setConservativeRasterizationMode(mode);
    cons.setExtraPrimitiveOverestimationSize(0.0f);  // RADV maxExtra=0，且门只测基线语义
    vk::PipelineRasterizationStateCreateInfo rs{};
    rs.setDepthClampEnable(false);
    rs.setRasterizerDiscardEnable(false);
    rs.setPolygonMode(vk::PolygonMode::eFill);
    rs.setCullMode(vk::CullModeFlagBits::eNone);
    rs.setFrontFace(vk::FrontFace::eClockwise);
    rs.setDepthBiasEnable(false);
    rs.setLineWidth(1.0f);
    rs.pNext = &cons;  // 摘掉这行 = 本用例集的变异红线命中处
    vk::Viewport vp{0.0f, static_cast<float>(kSize), static_cast<float>(kSize),
        -static_cast<float>(kSize), 0.0f, 1.0f};  // 负高度 = y 翻转（引擎同款，见 renderOnce 注释）
    vk::Rect2D sc{vk::Offset2D{0, 0}, vk::Extent2D{kSize, kSize}};
    vk::PipelineViewportStateCreateInfo vs{};
    vs.viewportCount = 1;
    vs.pViewports = &vp;
    vs.scissorCount = 1;
    vs.pScissors = &sc;
    vk::PipelineMultisampleStateCreateInfo ms{};
    ms.setRasterizationSamples(vk::SampleCountFlagBits::e1);
    ms.setSampleShadingEnable(false);
    vk::PipelineColorBlendAttachmentState cba{};
    cba.setBlendEnable(false);
    cba.setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
        | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    vk::PipelineColorBlendStateCreateInfo cb{};
    cb.setAttachments(cba);
    vk::PipelineRenderingCreateInfo ri{};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &kFmt;  // 动态渲染：单色附件、无深度
    // 动态名单数组：成员生命周期 = 本函数体，够 vkCreateGraphicsPipelines 用
    static const std::array<vk::DynamicState, 1> dyn{
        vk::DynamicState::eConservativeRasterizationModeEXT};
    vk::PipelineDynamicStateCreateInfo ds{
        {}, 1u, dyn.data() };  // ctor 序：flags, dynamicStateCount, pDynamicStates
    vk::GraphicsPipelineCreateInfo pci{};
    pci.setStages(stages);
    pci.layout = *layout;
    pci.pViewportState = &vs;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = dynamicMode ? &ds : nullptr;
    pci.pNext = &ri;
    // 第 2 参 = Optional<PipelineCache>（nullptr = 无缓存；本头版本 Optional 收 nullptr_t）
    return vk::raii::Pipeline(fx.device(), nullptr, pci);
}

/** 一次渲染：clear 黑 → draw 2 workgroup → 回 General 布局（dynMode 非空 = 录制动态切换）。
 *  viewport 刻意与引擎 applyDynamicState 同款 **负高度翻转**（vk::Viewport(x, yTop, w, -h)，
 *  shader.cppm——RADV mesh 输出 clip 为 D3D12 口径 +Y 向下 [2026-10-01 本探针实测：
 *  正 viewport 下探针 y 镜像 50↔13]，引擎全靠负 viewport 补偿）。探针测的是保守覆盖
 *  语义本身，坐标系必须与 1d 真实集成环境一致；且此选择自证——基线用例的位置锚点
 *  只在"翻转+约定"同时成立时才绿。scissor 不受负高度影响（引擎同态）。 */
void renderOnce(const Fixture& fx, const vk::raii::Pipeline& pipe, Target& t,
    const std::optional<vk::ConservativeRasterizationModeEXT>& dynMode) {
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        barrier(cmd, t.img.image, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
        vk::RenderingAttachmentInfo att{};
        att.imageView = *t.view;
        att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        att.loadOp = vk::AttachmentLoadOp::eClear;
        att.storeOp = vk::AttachmentStoreOp::eStore;
        att.clearValue = vk::ClearColorValue(std::array{0.0f, 0.0f, 0.0f, 0.0f});
        vk::RenderingInfo r{};
        r.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, vk::Extent2D{kSize, kSize}};
        r.layerCount = 1;
        r.setColorAttachments(att);
        cmd.beginRendering(r);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
        // viewport/scissor 走管线静态状态（负高度翻转在 makePipeline 里）——
        // 未列入动态名单就 cmd.setViewport 是 UB，勿加回
        if (dynMode.has_value())
            cmd.setConservativeRasterizationModeEXT(*dynMode);
        cmd.drawMeshTasksEXT(2, 1, 1);  // wg0 亚像素、wg1 大三角（一次进两区）
        cmd.endRendering();
        barrier(cmd, t.img.image, vk::ImageLayout::eColorAttachmentOptimal,
            vk::ImageLayout::eGeneral, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite, vk::PipelineStageFlagBits2::eTopOfPipe,
            vk::AccessFlagBits2::eNone);
    });
}

struct Counts {
    int sub = 0;      // x<32 区非黑像素数
    int big = 0;      // x≥32 区非黑像素数
    int subX = -1, subY = -1;  // sub==1 时记录唯一白像素位置（位置判据抓翻向）
    int minX = 999, maxX = -1, minY = 999, maxY = -1;  // 白像素包围盒（失败时定位翻向/错位）
};

Counts countCoverage(const Fixture& fx, const Target& t) {
    const auto px = testing::gpu::readImage(fx, t.img);
    Counts c;
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x) {
            const size_t o = (static_cast<size_t>(y) * kSize + x) * 4;
            const bool nonBlack = px[o] | px[o + 1] | px[o + 2] | px[o + 3];
            if (!nonBlack)
                continue;
            // golden：探针是恒白输出，任何非 255 的"半白"都说明链路有别扭（抖动/混色）
            CHECK(px[o + 0] == 255);
            CHECK(px[o + 1] == 255);
            CHECK(px[o + 2] == 255);
            CHECK(px[o + 3] == 255);
            if (static_cast<int>(x) < c.minX) c.minX = static_cast<int>(x);
            if (static_cast<int>(x) > c.maxX) c.maxX = static_cast<int>(x);
            if (static_cast<int>(y) < c.minY) c.minY = static_cast<int>(y);
            if (static_cast<int>(y) > c.maxY) c.maxY = static_cast<int>(y);
            if (x < kSplitX) {
                ++c.sub;
                c.subX = static_cast<int>(x);
                c.subY = static_cast<int>(y);
            } else {
                ++c.big;
            }
        }
    return c;
}

}  // namespace

}  // namespace RGL::testing::izb

TEST_SUITE("gpu") {

using RGL::testing::gpu::Fixture;
using RGL::testing::izb::countCoverage;
using RGL::testing::izb::kBigCenter;
using RGL::testing::izb::kBigConservative;
using RGL::testing::izb::kSubpxX;
using RGL::testing::izb::kSubpxY;
using RGL::testing::izb::makeModule;
using RGL::testing::izb::makePipeline;
using RGL::testing::izb::makeTarget;
using RGL::testing::izb::renderOnce;
using RGL::testing::izb::Target;

TEST_CASE("izb_1b 能力面：EDS3 保守动态模式 + §9 vulkaninfo 事实钉成 golden") {
    const Fixture fx;
    auto feats = fx.physical().getFeatures2<vk::PhysicalDeviceFeatures2,
        vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
    const auto& eds3 = feats.get<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
    // 本批全部动态路的前提；缺 = FAIL 不 SKIP（gpu 套件纪律：能跑到这里 = 用户要了设备测试）
    CHECK(eds3.extendedDynamicState3ConservativeRasterizationMode);
    auto props = fx.physical().getProperties2<vk::PhysicalDeviceProperties2,
        vk::PhysicalDeviceConservativeRasterizationPropertiesEXT>();
    const auto& cr = props.get<vk::PhysicalDeviceConservativeRasterizationPropertiesEXT>();
    // phase11_plan §9 的两个事实：extra 外扩不存在（软件 dilation 红线的依据）——钉成 golden
    CHECK(cr.maxExtraPrimitiveOverestimationSize == 0.0f);
    INFO("primitiveOverestimationSize=" << cr.primitiveOverestimationSize);
}

TEST_CASE("izb_1b 基线（harness 阳性锚点）：非保守 mesh 管线 = 中心采样 0 / 300") {
    const Fixture fx;
    const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_spike.mesh.spv");
    const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_spike.frag.spv");
    const vk::raii::PipelineLayout layout(fx.device(), vk::PipelineLayoutCreateInfo{});
    const auto pipe = makePipeline(fx, layout, mesh, frag,
        vk::ConservativeRasterizationModeEXT::eDisabled, false);
    auto t = makeTarget(fx);
    renderOnce(fx, pipe, t, std::nullopt);
    const auto c = countCoverage(fx, t);
    INFO(std::format("bbox=[{},{}]-[{},{}]", c.minX, c.minY, c.maxX, c.maxY));
    CHECK(c.sub == 0);      // 亚像素三角中心采样必不出像素（负向锚点）
    CHECK(c.big == kBigCenter);    // 中心采样闭式解；错了 = rig 自己的映射/绘制坏了
}

TEST_CASE("izb_1b 成败门：静态 OVERESTIMATE × mesh shader = 恰 1 像素 @(17,13) / 325") {
    const Fixture fx;
    const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_spike.mesh.spv");
    const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_spike.frag.spv");
    const vk::raii::PipelineLayout layout(fx.device(), vk::PipelineLayoutCreateInfo{});
    const auto pipe = makePipeline(fx, layout, mesh, frag,
        vk::ConservativeRasterizationModeEXT::eOverestimate, false);
    auto t = makeTarget(fx);
    renderOnce(fx, pipe, t, std::nullopt);
    const auto c = countCoverage(fx, t);
    INFO(std::format("device={} bbox=[{},{}]-[{},{}]", fx.deviceName(), c.minX, c.minY,
                     c.maxX, c.maxY));
    CHECK(c.sub == 1);      // 保守光栅对 mesh 派生三角形生效的唯一判据（1 → 门开）
    CHECK(c.subX == kSubpxX);    // 位置精确 → 抓 NDC 翻向（(46,13)/(17,50) 都算 rig 坏）
    CHECK(c.subY == kSubpxY);
    CHECK(c.big == kBigConservative);    // 保守覆盖闭式解（300→325 = 边界 25 格全补上）
}

TEST_CASE("izb_1b 动态态：EDS3 保守模式在 mesh 管线上双向切换均生效") {
    const Fixture fx;
    const auto mesh = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_spike.mesh.spv");
    const auto frag = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_spike.frag.spv");
    const vk::raii::PipelineLayout layout(fx.device(), vk::PipelineLayoutCreateInfo{});
    // 正向：静态 DISABLED + 动态切 OVERESTIMATE → 应出保守结果（1/325）
    const auto pipeD = makePipeline(fx, layout, mesh, frag,
        vk::ConservativeRasterizationModeEXT::eDisabled, true);
    auto t1 = makeTarget(fx);
    renderOnce(fx, pipeD, t1, vk::ConservativeRasterizationModeEXT::eOverestimate);
    const auto c1 = countCoverage(fx, t1);
    CHECK(c1.sub == 1);
    CHECK(c1.big == kBigConservative);
    // 反向：静态 OVERESTIMATE + 动态切 DISABLED → 应回中心采样（0/300）。
    // 只测正向会漏"管线其实无视动态状态、恒保守"这一整类坏法。
    const auto pipeO = makePipeline(fx, layout, mesh, frag,
        vk::ConservativeRasterizationModeEXT::eOverestimate, true);
    auto t2 = makeTarget(fx);
    renderOnce(fx, pipeO, t2, vk::ConservativeRasterizationModeEXT::eDisabled);
    const auto c2 = countCoverage(fx, t2);
    CHECK(c2.sub == 0);
    CHECK(c2.big == kBigCenter);
}

}  // TEST_SUITE("gpu")
