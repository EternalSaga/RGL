module;
/**
 * test_izb_engine_contract —— [gpu] 批 11.3 补账：钉**引擎真 SPV** 的屏幕→世界契约
 *
 * ## 为什么 1c 测过了还要再测（缺口实证）
 *
 * tests/test_izb_create.cppm 加载的是 **spike 自己的 shader**（tests/shaders/），
 * 钉的是链表机制；引擎 shader（rgl/shaders/izb_create.comp.slang → 真 SPV）的
 * **屏幕 ndc 口径 / 字节偏移 / guard 语义**没有任何测试覆盖——2026-10-02「影子几何
 * 严重错位」正是从这条缝里过的：create 用 ndc 直式（fb 行 0 = ndc.y −1），而引擎负
 * viewport 惯例是 fb 行 0 ↔ ndc.y +1（ao.frag worldFromDepth 先例）→ 深度重建世界
 * 位置上下镜像 → 节点光空间 uv 落错世界点。spike 的自洽场景（自己的约定自己测）
 * 结构上测不到这条契约。本文件把**引擎产物本体**（build/rgl/generated_shaders/
 * izb_create.comp.spv）拉上设备，用闭式解钉死它。
 *
 * ## 场景（合成，确定性，零 RNG；正交相机 = 无透视除法干扰）
 *
 * 相机：eye=(0,0,10) 看原点、up=+y；proj=ortho(±2,±2, 0.01,100)（Vulkan 深度 [0,1]）
 *   ⇒ 像素 (px,py) 深度 d 的世界点（**引擎口径**：ndc.y = 1−2(py+0.5)/H）：
 *      x = (px+0.5)/4 − 2；y = 2 − (py+0.5)/4；z = 9.99 − 99.99·d
 * 光：lookAt((0,20,0)→原点, up=(0,0,−1)) + ortho(±4,±4, 0.01,60)（正上方直下）
 *   ⇒ 光空间基 x=world x、up=−world z、深度轴 = 20−y：
 *      u = x/8 + 0.5；v = 0.5 − z/8；lightDepth = (19.99 − y)/59.99
 *   以上闭式与 glm 矩阵乘的**一致性**在判据 ⓪ 自检（5a-7 教训：闭式解也要探针，
 *   "这次我推对了"的记忆不可信）。
 *
 * 摆点（16×16 像素，G=8，emptyDepth 参数 = 0.08——**每个 guard 要有独立判别像素**，
 * 否则"没触发"与"坏掉了"不可分）：
 *   P1 (2,0) z=3.2 → cell(2,0)；P2 (2,1) z=3.4 → cell(2,0)（同格成链，长度 2）
 *   P3 (13,4) z=2.6 → cell(5,1)（独苗）
 *   P4 (7,7) d=0.085 ≥ emptyDepth → 不建节点（其 uv 在界内 = **只有** emptyDepth
 *      guard 能拦它；1c 场景里 d≥1 的像素同时被 uv 界拦，两 guard 混淆）
 *   P5 (9,2) z=5 → v=−0.125 越界 → 不建节点（其 d 有效 = **只有** uv 界能拦它）
 *   ⚠️ d≤0 guard 在本正交场景**结构上不可观测**（d≤0 ⇒ z≥9.99 ⇒ v<0 先被 uv 界
 *   拦），故不假装测它——写在这里是为了防止有人把"没测"读成"测过"。
 * 所有落点离 cell 边界 ≥0.2 cell（构建期 CHECK，float 噪声翻不动格）。
 *
 * ## 判据
 *
 * ⓪ 脚手架自检：闭式解 == glm 矩阵乘（invViewProj·(ndc,d,1) 与 lightVP·world），
 *    镜像口径（ndc.y = 2(py+0.5)/H−1）必须**不等**——判据本身先过阳性锚点。
 * ① head[cell] 链游走（顺序无关）：cell(2,0) 到达集 == {P1,P2}、cell(5,1) == {P3}，
 *    以 NONE 结尾、步数 ≤4（环自证）。
 * ② 其余 62 格全 NONE；P4/P5 像素 ID 全表不可达（反向用例）。
 * ③ 逐节点 payload（16B = u,v,lightDepth,next 位转 float）== 闭式值（expectNear
 *    1e-5）——**镜像 bug 的死刑条款**：ndc.y 复原直式 → P1 lightDepth
 *    0.3009→0.3655，差 0.065 ≫ 容差。
 * ④ 全表可达 ID 总数 == 3（不重不漏）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   ndc.y 复原直式（a791307 根因回归）        → ③ 红（P1/P2 lightDepth 镜像）
 *   nodes.Store 偏移 p*16 → p*4              → ③ 红（payload 错位）/ VVL OOB
 *   head 索引 idx*4 → idx（字节偏移丢 ×4）    → ① 红（链挂错格）
 *   emptyDepth guard 删                       → ② 红（P4 入链）
 *   uv 越界 guard 删                          → ② 红（P5 负 cell → VVL OOB 当场打断）
 *   dispatch 忘发 / shader 从未生效           → ① 红（head 全 NONE = 阳性锚点）
 *   闭式推导自己错                            → ⓪ 红（判据工具自检）
 *
 * ## ⚠️ 描述符红线
 *
 * 与 1c 同款放宽（老式 descriptor set，fixture 无 VMA/Resources）；引擎集成侧的
 * heap 纪律见 DESCRIPTOR_HEAP_SPEC.md，勿从本文件搬运。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <tuple>
#include <vector>

export module test_izb_engine_contract;

import rlstd;
import rlvk;
import test_device_primitives;  // Fixture / submitAndWait / makeImage / writeImage
import test_support;            // expectNear

TEST_SUITE("gpu") {

namespace {

using RGL::testing::gpu::Fixture;
using RGL::testing::gpu::makeImage;
using RGL::testing::gpu::writeImage;

constexpr uint32_t kRes = 16;        // 深度图边长（像素）
constexpr uint32_t kGrid = 8;        // 光空间网格 G
constexpr float kEmptyDepth = 0.08f; // push 参数（引擎真值 1.0；本测试取可分离值）
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kHeadBytes = kGrid * kGrid * 4;
constexpr size_t kNodeBytes = static_cast<size_t>(kRes) * kRes * 16;  // 16B/像素

/** 与 izb_create.comp.slang IzbCreatePush 逐字段一致（144B，glm 列主序 = float4[4]）。 */
struct CreatePush {
    glm::vec4 invVPM[4];
    glm::vec4 lightVP[4];
    int32_t pixelW;
    int32_t pixelH;
    int32_t gridDim;
    float emptyDepth;
};
static_assert(sizeof(CreatePush) == 144);

struct HostSsbo {
    vk::raii::Buffer buffer;
    vk::raii::DeviceMemory memory;
    void* mapped = nullptr;
};

HostSsbo makeHostSsbo(const Fixture& fx, size_t bytes, const void* src) {
    vk::BufferCreateInfo bi{};
    bi.size = bytes;
    bi.usage = vk::BufferUsageFlagBits::eStorageBuffer;
    bi.sharingMode = vk::SharingMode::eExclusive;
    vk::raii::Buffer buf(fx.device(), bi);
    const auto req = buf.getMemoryRequirements();
    const auto props = fx.physical().getMemoryProperties();
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        const auto f = static_cast<vk::MemoryPropertyFlags>(props.memoryTypes[i].propertyFlags);
        const auto want = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        if ((req.memoryTypeBits & (1u << i)) != 0 && (f & want) == want) {
            type = i;
            break;
        }
    }
    REQUIRE(type != UINT32_MAX);
    vk::MemoryAllocateInfo ai{};
    ai.allocationSize = bytes;
    ai.memoryTypeIndex = type;
    vk::raii::DeviceMemory mem(fx.device(), ai);
    buf.bindMemory(mem, 0);
    void* p = mem.mapMemory(0, bytes);
    if (src)
        std::memcpy(p, src, bytes);
    // 刻意不 unmap（1c 同款：提前 unmap 再读 = 悬空指针，free 时隐式 unmap 合法）
    return HostSsbo{.buffer = std::move(buf), .memory = std::move(mem), .mapped = p};
}

vk::raii::ShaderModule makeModule(const Fixture& fx, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f.is_open());  // 引擎产物缺失 = 响亮失败（不 skip）
    std::vector<char> bytes(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    vk::ShaderModuleCreateInfo mi{};
    const std::vector<uint32_t> code(
        reinterpret_cast<const uint32_t*>(bytes.data()),
        reinterpret_cast<const uint32_t*>(bytes.data()) + bytes.size() / 4);
    mi.setCode(code);  // ArrayProxyNoTemporaries：必须具名左值
    return vk::raii::ShaderModule(fx.device(), mi);
}

vk::raii::Pipeline makeCompute(const Fixture& fx, const vk::raii::PipelineLayout& layout,
    const vk::raii::ShaderModule& mod) {
    vk::PipelineShaderStageCreateInfo si{};
    si.stage = vk::ShaderStageFlagBits::eCompute;
    si.module = *mod;
    si.pName = "main";
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = si;
    ci.layout = *layout;
    return vk::raii::Pipeline(fx.device(), nullptr, ci);
}

vk::BufferMemoryBarrier2 bufBarrier(vk::Buffer buffer, vk::PipelineStageFlags2 srcS,
    vk::AccessFlags2 srcA, vk::PipelineStageFlags2 dstS, vk::AccessFlags2 dstA) {
    vk::BufferMemoryBarrier2 b{};
    b.srcStageMask = srcS;
    b.srcAccessMask = srcA;
    b.dstStageMask = dstS;
    b.dstAccessMask = dstA;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = buffer;
    b.offset = 0;
    b.size = VK_WHOLE_SIZE;
    return b;
}

/** 闭式解（引擎口径）。d 无效（guard 拦下）时返回 inserted=false。 */
struct Expect {
    bool inserted;
    uint32_t cell;
    float x, y, z, u, v, lightDepth;
};

Expect closedForm(uint32_t px, uint32_t py, float d) {
    const float ndcx = (px + 0.5f) / kRes * 2.f - 1.f;
    const float ndcy = 1.f - (py + 0.5f) / kRes * 2.f;  // 引擎惯例：fb 行 0 ↔ ndc.y +1
    Expect e{};
    e.x = 2.f * ndcx;
    e.y = 2.f * ndcy;
    e.z = 9.99f - 99.99f * d;
    e.u = e.x / 8.f + 0.5f;
    e.v = 0.5f - e.z / 8.f;
    e.lightDepth = (19.99f - e.y) / 59.99f;
    e.inserted = d < kEmptyDepth && d > 0.f && e.u >= 0.f && e.u < 1.f && e.v >= 0.f
        && e.v < 1.f;
    if (e.inserted)
        e.cell = static_cast<uint32_t>(std::floor(e.u * kGrid))
            + static_cast<uint32_t>(std::floor(e.v * kGrid)) * kGrid;
    return e;
}

float dOfZ(float z) { return (9.99f - z) / 99.99f; }

uint32_t pixelId(uint32_t px, uint32_t py) { return py * kRes + px; }

}  // namespace

TEST_CASE("izb 引擎契约：真 izb_create.comp.spv 的 ndc 口径/字节偏移/guard 语义（闭式锚点）") {
    const Fixture fx;

    // ── 相机与光（矩阵与闭式解必须同参数）──────────────────────────────────
    const glm::mat4 view = glm::lookAt(glm::vec3(0.f, 0.f, 10.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
    const glm::mat4 proj = glm::ortho(-2.f, 2.f, -2.f, 2.f, 0.01f, 100.f);
    const glm::mat4 invViewProj = glm::inverse(proj * view);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0.f, 20.f, 0.f), glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f));
    const glm::mat4 lightProj = glm::ortho(-4.f, 4.f, -4.f, 4.f, 0.01f, 60.f);
    const glm::mat4 lightVP = lightProj * lightView;

    // ── 摆点表 ──────────────────────────────────────────────────────────────
    struct Placed { uint32_t px, py; float d; };
    const std::vector<Placed> placed{
        {2, 0, dOfZ(3.2f)},    // P1 → cell(2,0)
        {2, 1, dOfZ(3.4f)},    // P2 → cell(2,0) 同链
        {13, 4, dOfZ(2.6f)},   // P3 → cell(5,1)
        {7, 7, 0.085f},        // P4 → emptyDepth guard（uv 界内）
        {9, 2, dOfZ(5.0f)},    // P5 → uv 越界 guard（d 有效）
    };

    // ── 判据 ⓪：闭式解 == glm 矩阵乘（脚手架自检 + 镜像口径必须不等）───────
    for (const auto& pl : placed) {
        const auto e = closedForm(pl.px, pl.py, pl.d);
        INFO("pixel (" << pl.px << "," << pl.py << ")");
        const auto ndc = glm::vec2((pl.px + 0.5f) / kRes * 2.f - 1.f, 1.f - (pl.py + 0.5f) / kRes * 2.f);
        const glm::vec4 w = invViewProj * glm::vec4(ndc.x, ndc.y, pl.d, 1.f);
        const glm::vec4 ls = lightVP * (w / w.w);
        RGL::testing::expectNear(w.x / w.w, e.x, 1e-5);
        RGL::testing::expectNear(w.y / w.w, e.y, 1e-5);
        RGL::testing::expectNear(w.z / w.w, e.z, 1e-5);
        RGL::testing::expectNear(ls.x * 0.5f + 0.5f, e.u, 1e-5);
        RGL::testing::expectNear(ls.y * 0.5f + 0.5f, e.v, 1e-5);
        RGL::testing::expectNear(ls.z, e.lightDepth, 1e-5);
        // 镜像口径（被修掉的 bug）必须给出**不同**的 y——否则判据③对镜像不敏感。
        // 只对建节点像素判（guard 像素的 cell 归属与 y 无关，镜像不镜像都不可达）
        if (e.inserted) {
            const glm::vec4 wMirror = invViewProj * glm::vec4(ndc.x, -ndc.y, pl.d, 1.f);
            CHECK(std::abs(wMirror.y / wMirror.w - w.y / w.w) > 0.5f);
        }
        // 落点余量自证（1c 同款）：离 cell 边界 ≥0.2 cell
        if (e.inserted) {
            const float bu = std::abs(std::floor(e.u * kGrid) - e.u * kGrid);
            const float bv = std::abs(std::floor(e.v * kGrid) - e.v * kGrid);
            CHECK(std::min(bu, bv) > 0.2f);
        }
    }
    // 输入设计自证：P1..P3 建节点、P4/P5 被拦且**各只被一个 guard 拦**
    const auto e1 = closedForm(2, 0, dOfZ(3.2f));
    const auto e2 = closedForm(2, 1, dOfZ(3.4f));
    const auto e3 = closedForm(13, 4, dOfZ(2.6f));
    const auto e4 = closedForm(7, 7, 0.085f);
    const auto e5 = closedForm(9, 2, dOfZ(5.0f));
    REQUIRE(e1.inserted);
    REQUIRE(e2.inserted);
    REQUIRE(e3.inserted);
    REQUIRE(e1.cell == e2.cell);       // 同链
    REQUIRE(e1.cell != e3.cell);       // 异格
    REQUIRE(!e4.inserted);             // 仅 emptyDepth 拦（uv 界内 = 下面自证）
    REQUIRE(e4.v >= 0.f);
    REQUIRE(e4.v < 1.f);
    REQUIRE(e4.u >= 0.f);
    REQUIRE(e4.u < 1.f);
    REQUIRE(!e5.inserted);             // 仅 uv 界拦（d 有效 = 下面自证）
    REQUIRE(e5.v < 0.f);
    CHECK(e1.lightDepth < e2.lightDepth);  // y 大者光深浅（20−y 语义自证，防闭式解写反）

    // ── GPU 资源 ────────────────────────────────────────────────────────────
    // 默认铺 d=1.0：≥ emptyDepth 且 uv 界外 → 全部不建节点（只有摆点参与）
    std::vector<float> depthIn(kRes * kRes, 1.0f);
    for (const auto& pl : placed) depthIn[pixelId(pl.px, pl.py)] = pl.d;
    const auto imgDepth = [&] {
        auto img = makeImage(fx, vk::Format::eR32Sfloat, kRes, kRes,
            vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eSampled);
        writeImage(fx, img, {reinterpret_cast<const uint8_t*>(depthIn.data()),
            reinterpret_cast<const uint8_t*>(depthIn.data()) + depthIn.size() * 4});
        return img;
    }();
    vk::ImageViewCreateInfo vi{};
    vi.image = *imgDepth.image;
    vi.viewType = vk::ImageViewType::e2D;
    vi.format = vk::Format::eR32Sfloat;
    vi.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
    const vk::raii::ImageView depthView(fx.device(), vi);

    std::vector<uint32_t> headInit(kGrid * kGrid, kNone);
    auto ssboHead = makeHostSsbo(fx, kHeadBytes, headInit.data());
    const std::vector<uint32_t> nodeInit(kNodeBytes / 4, 0xFFFFFFFFu);  // next=NONE 确定初值
    auto ssboNodes = makeHostSsbo(fx, kNodeBytes, nodeInit.data());

    // 描述符：0 head、1 nodes（storage）、2 depthTex（sampled image，GENERAL 布局合法）
    std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {0, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
        {1, vk::DescriptorType::eStorageBuffer, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
        {2, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
    }};
    vk::DescriptorSetLayoutCreateInfo li{};
    li.setBindings(bindings);
    const vk::raii::DescriptorSetLayout dsLayout(fx.device(), li);
    const std::array<vk::DescriptorPoolSize, 2> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 2},
        vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 1}};
    vk::DescriptorPoolCreateInfo pi{};
    pi.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    pi.setPoolSizes(poolSizes);
    pi.setMaxSets(1);
    const vk::raii::DescriptorPool pool(fx.device(), pi);
    vk::DescriptorSetAllocateInfo sai{};
    sai.descriptorPool = *pool;
    sai.descriptorSetCount = 1;
    sai.pSetLayouts = &*dsLayout;
    const auto sets = fx.device().allocateDescriptorSets(sai);
    REQUIRE(sets.size() == 1);
    const std::array<vk::DescriptorBufferInfo, 2> bufInfos{{
        {*ssboHead.buffer, 0, kHeadBytes},
        {*ssboNodes.buffer, 0, kNodeBytes},
    }};
    const vk::DescriptorImageInfo depthInfo{{}, *depthView, vk::ImageLayout::eGeneral};
    std::array<vk::WriteDescriptorSet, 3> writes{};
    writes[0].dstSet = *sets[0];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[0].pBufferInfo = &bufInfos[0];
    writes[1].dstSet = *sets[0];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = vk::DescriptorType::eStorageBuffer;
    writes[1].pBufferInfo = &bufInfos[1];
    writes[2].dstSet = *sets[0];
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = vk::DescriptorType::eSampledImage;
    writes[2].pImageInfo = &depthInfo;
    fx.device().updateDescriptorSets(writes, {});

    vk::PushConstantRange pcr{vk::ShaderStageFlagBits::eCompute, 0, sizeof(CreatePush)};
    vk::PipelineLayoutCreateInfo plci{};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &*dsLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    const vk::raii::PipelineLayout pipeLayout(fx.device(), plci);

    // **引擎真产物**（非 spike 副本）——本测试的全部意义在这行
    const auto mod = makeModule(fx, RGL_ENGINE_SHADER_DIR "/izb_create.comp.spv");
    const auto pipe = makeCompute(fx, pipeLayout, mod);

    CreatePush push{};
    std::memcpy(push.invVPM, glm::value_ptr(invViewProj), 64);
    std::memcpy(push.lightVP, glm::value_ptr(lightVP), 64);
    push.pixelW = static_cast<int32_t>(kRes);
    push.pixelH = static_cast<int32_t>(kRes);
    push.gridDim = static_cast<int32_t>(kGrid);
    push.emptyDepth = kEmptyDepth;

    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        std::array pre{
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
        };
        vk::DependencyInfo dep0{};
        dep0.setBufferMemoryBarriers(pre);
        cmd.pipelineBarrier2(dep0);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeLayout, 0, *sets[0], {});
        cmd.pushConstants(*pipeLayout, vk::ShaderStageFlagBits::eCompute, 0,
            static_cast<uint32_t>(sizeof(push)), &push);
        cmd.dispatch((kRes * kRes + 63) / 64, 1, 1);

        std::array post{
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
        };
        vk::DependencyInfo dep1{};
        dep1.setBufferMemoryBarriers(post);
        cmd.pipelineBarrier2(dep1);
    });

    // ── 回读 + 判据 ①②④：链游走（顺序无关）────────────────────────────────
    std::vector<uint32_t> head(kGrid * kGrid);
    std::memcpy(head.data(), ssboHead.mapped, kHeadBytes);
    std::vector<uint32_t> nodes(kNodeBytes / 4);
    std::memcpy(nodes.data(), ssboNodes.mapped, kNodeBytes);

    const auto nodeNext = [&](uint32_t p) { return nodes[p * 4 + 3]; };
    const auto nodeFloat = [&](uint32_t p, int comp) {
        const uint32_t bits = nodes[p * 4 + comp];
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    };

    std::set<uint32_t> reached;
    uint32_t nonEmptyCells = 0;
    for (uint32_t c = 0; c < kGrid * kGrid; ++c) {
        uint32_t h = head[c];
        if (h == kNone)
            continue;
        ++nonEmptyCells;
        int steps = 0;
        while (h != kNone) {
            REQUIRE(steps < 8);  // 环自证（3 节点链 ≤3 步）
            INFO("cell " << c << " step " << steps << " id " << h);
            CHECK(reached.insert(h).second);  // 双插自证
            h = nodeNext(h);
            ++steps;
        }
    }
    // ④ 不重不漏
    const std::set<uint32_t> expectIds{pixelId(2, 0), pixelId(2, 1), pixelId(13, 4)};
    CHECK(reached == expectIds);
    // ② 恰好两格非空（阳性锚点：dispatch 没跑 = 0 格 = ①④ 已红，这里钉"没有多余格"）
    CHECK(nonEmptyCells == 2);
    // ② 反向用例：P4/P5 的 payload 区必须保持 0xFF 初值（从未被写）
    for (uint32_t p : {pixelId(7, 7), pixelId(9, 2)})
        CHECK(nodes[p * 4] == 0xFFFFFFFFu);

    // ── 判据 ③：payload 逐字段 == 闭式解（镜像 bug 死刑条款）───────────────
    struct Node { uint32_t px, py; Expect e; };
    const std::vector<Node> nodes3{{2, 0, e1}, {2, 1, e2}, {13, 4, e3}};
    for (const auto& n : nodes3) {
        const uint32_t id = pixelId(n.px, n.py);
        INFO("node " << id << " (pixel " << n.px << "," << n.py << ")");
        RGL::testing::expectNear(nodeFloat(id, 0), n.e.u, 1e-5);
        RGL::testing::expectNear(nodeFloat(id, 1), n.e.v, 1e-5);
        RGL::testing::expectNear(nodeFloat(id, 2), n.e.lightDepth, 1e-5);
        // cell 归属（head 字节偏移契约）：节点必须挂在闭式 cell 的链上
        CHECK(head[n.e.cell] != kNone);
        bool onChain = false;
        for (uint32_t h = head[n.e.cell]; h != kNone && !onChain; h = nodeNext(h))
            onChain = h == id;
        CHECK(onChain);
    }
}

}  // TEST_SUITE("gpu")
