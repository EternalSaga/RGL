module;
/**
 * test_izb_mask —— [gpu] 批 11.3-1d：光空间保守光栅 × IZB 走链 → 可见性 mask
 * （docs/hfts_izb_plan.md §4.1-1d 的算法核心；FTIZB pass5 点采样退化版）
 *
 * ## 链路（一帧内）
 *
 * 1c 的 izb_create（复用同一 SPV 与 params 布局）从深度表建 IZB 链表 → 光空间
 * mesh 管线把 5 个遮挡三角保守光栅进 8×8 网格（1b 同款负 viewport）→ frag 走
 * 命中 texel 的链表，逐节点做「2D 棱柱（三边 edge function 同号）+ 深度（节点
 * 比 fragment 插值光深度更远）」判定，命中置 mask[pixel] |= 1<<triIdx。
 *
 * ## 场景（1c 锚点像素复用 + 2 点新增；三角形表 = 唯一几何源）
 *
 * 像素 uv/光深度（闭式，1c 同款推导）与手写期望 mask（OVER 跑）：
 *   36 (0.0625,0.075,21.375) → 0b00001  矩形下半 t0（对角线 v=.125−u/4 下方）
 *   54 (0.3125,0.050,21.125) → 0b01010  矩形上半 t1 ∧ 小三角 t3
 *  104 (0.5625,0.575,20.375) → 0b00100  斜三角 t2（frag 深度 = texel 中心插值
 *       20.125，节点 20.375 > 20.125 —— FTIZB texel 粒度近似按定义钉死）
 *  134 (0.3125,0.060,19.875) → 0b01010  同 54（深度差 .375 过 t0/t1 界）
 *  214 (0.3125,0.070,18.625) → 0b10000  三重判例：t1 2D 内但深度不够（18.625<
 *       19.5）∧ t3 2D 内深度不够（<19.0）∧ 条带 t4 全过（d_f=18.0）——
 *       **且 t4 不盖 cell 中心（strip v∈[.0655,.095] vs 中心 .0625）：这是
 *       「保守光栅专属正向对」，DISABLED 下中心采样全盲 → mask 必 0**
 * 其余 0：89 (0.6875,0.55) 斜三角 2D 外（frag 有、edge 拒）；151 (0.4375,0.15)
 * 矩形保守触达 cell 行 1 但 2D 拒；(6,10) (0.3125,0.11,19.375) 双拒（t1 深度 /
 * t4 2D）；(8,10) (0.5625,0.55,19.375) t2 2D 内深度不够（<20.125）；187/202 远
 * 离所有三角；(7,7) z=1.5 越 UV 建链期就无节点（1c 反向用例延续）；(4,1) d=2.0
 * ≥ emptyDepth。
 * 设计余量审计：所有 2D 判定离边 ≥0.003 uv（≈0.024 texel），深度判定离界
 * ≥0.25——float32 噪声（~1e-7）翻不动。
 *
 * ## 判据（三向对拍，任何一源单独错都红）
 *
 * ① CPU 参考（barycentric 插值 frag 深度 + edge 判定的独立实现）生成全表，
 *    先与上表 5 条**手写锚点**逐位相等（锚点 = 独立出处，防参考代码手抖）；
 * ② GPU OVER 跑 == 参考全表（256 像素逐位）；
 * ③ GPU DISABLED 跑 == 「中心规则」参考（三角盖住 cell 中心才有 fragment），
 *    且显式对：mask_OVER[214] == 0b10000 而 mask_DISABLED[214] == 0
 *    （保守覆盖对算法正确性的贡献被单点亮钉）；
 * ④ 阳性锚点：非零 mask 计数 == 5（两跑各自）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   edge test 删掉（只留深度）→ 89/151/187/202/(8,10)… 多置位 → ①② 红
 *   深度比较反号 → 214 多 bit0|bit3、104 丢 bit2 → ② 红
 *   OVER 管线误成 DISABLED（或反之）→ 214 单点对红
 *   mask 原子忘写 → 全 0，锚点④红
 *   mesh 端忘了 NDC y = 1−2v（或管线漏负 viewport）→ 全镜像 → cell 全错位 → ② 红
 *   InterlockedOr 偏移误用元素索引（字节偏移坑 1c-②）→ 位落错像素 → ② 红
 *
 * ## 描述符红线（同 test_izb_create.cppm）
 *
 * 老式 DescriptorPool 仅 spike 放宽；引擎集成（1d 下半程）必须走 vk_resources
 * descriptor heap（docs/DESCRIPTOR_HEAP_SPEC.md），老式 bindless 禁入。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>

export module test_izb_mask;

import rlstd;
import rlvk;
import test_device_primitives;
import test_support;

namespace RGL::testing::izbmask {

using testing::gpu::Fixture;

namespace {

constexpr uint32_t kRes = 16;
constexpr uint32_t kGrid = 8;
constexpr float kEmptyDepth = 1.0f;
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kHeadBytes = kGrid * kGrid * 4;
constexpr size_t kNodeBytes = kRes * kRes * 8;
constexpr size_t kDepthBytes = kRes * kRes * 4;
constexpr size_t kPrimedBytes = kGrid * kGrid * 4;
constexpr size_t kParamsBytes = 160;
constexpr uint32_t kTriCount = 5;
constexpr size_t kMaskBytes = kRes * kRes * 4;
constexpr size_t kPixelUvBytes = kRes * kRes * 8;
constexpr size_t kParams2Bytes = 16 + 64 * 8;  // 表容量 8 条（现役 5）
constexpr vk::Format kFmt = vk::Format::eR8G8B8A8Unorm;

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
    uint32_t type = UINT32_MAX;
    const auto req = buf.getMemoryRequirements();
    const auto props = fx.physical().getMemoryProperties();
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
    // 🔴 分配取需求 req.size 而非请求 bytes（本机数值相同 ⟹ 旧写法不出声，换后端即越界绑定；
    // 完整理由 + 实测先例 → `test_device_primitives_impl.cppm::makeHostBuffer`）
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = type;
    vk::raii::DeviceMemory mem(fx.device(), ai);
    buf.bindMemory(mem, 0);
    // 刻意不 unmap（1c 坑④）：提交后经同一映射读回，free 时隐式 unmap 合法
    void* p = mem.mapMemory(0, bytes);
    if (src)
        std::memcpy(p, src, bytes);
    return HostSsbo{.buffer = std::move(buf), .memory = std::move(mem), .mapped = p};
}

vk::raii::ShaderModule makeModule(const Fixture& fx, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE(f);
    std::vector<char> bytes(static_cast<size_t>(f.tellg()));
    f.seekg(0);
    f.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    vk::ShaderModuleCreateInfo mi{};
    const std::vector<uint32_t> code(
        reinterpret_cast<const uint32_t*>(bytes.data()),
        reinterpret_cast<const uint32_t*>(bytes.data()) + bytes.size() / 4);
    mi.setCode(code);
    return vk::raii::ShaderModule(fx.device(), mi);
}

/** mesh+frag 光栅管线（保守模式静态；8×8 负高度 viewport = 引擎同款补偿，1b 口径）。 */
vk::raii::Pipeline makePipeline(const Fixture& fx, const vk::raii::PipelineLayout& layout,
    const vk::raii::ShaderModule& meshMod, const vk::raii::ShaderModule& fragMod,
    vk::ConservativeRasterizationModeEXT mode) {
    const std::array<vk::PipelineShaderStageCreateInfo, 2> stages{{
        {vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eMeshEXT, *meshMod, "main"},
        {vk::PipelineShaderStageCreateFlags{}, vk::ShaderStageFlagBits::eFragment, *fragMod, "main"},
    }};
    vk::PipelineRasterizationConservativeStateCreateInfoEXT cons{};
    cons.setConservativeRasterizationMode(mode);
    cons.setExtraPrimitiveOverestimationSize(0.0f);
    vk::PipelineRasterizationStateCreateInfo rs{};
    rs.setDepthClampEnable(false);
    rs.setRasterizerDiscardEnable(false);
    rs.setPolygonMode(vk::PolygonMode::eFill);
    rs.setCullMode(vk::CullModeFlagBits::eNone);
    rs.setDepthBiasEnable(false);
    rs.setLineWidth(1.0f);
    rs.pNext = &cons;
    vk::Viewport vp{0.0f, static_cast<float>(kGrid), static_cast<float>(kGrid),
        -static_cast<float>(kGrid), 0.0f, 1.0f};
    vk::Rect2D sc{vk::Offset2D{0, 0}, vk::Extent2D{kGrid, kGrid}};
    vk::PipelineViewportStateCreateInfo vs{};
    vs.viewportCount = 1;
    vs.pViewports = &vp;
    vs.scissorCount = 1;
    vs.pScissors = &sc;
    vk::PipelineMultisampleStateCreateInfo ms{};
    ms.setRasterizationSamples(vk::SampleCountFlagBits::e1);
    vk::PipelineColorBlendAttachmentState cba{};
    cba.setBlendEnable(false);
    cba.setColorWriteMask(vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG
        | vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA);
    vk::PipelineColorBlendStateCreateInfo cb{};
    cb.setAttachments(cba);
    vk::PipelineRenderingCreateInfo ri{};
    ri.colorAttachmentCount = 1;
    ri.pColorAttachmentFormats = &kFmt;
    vk::GraphicsPipelineCreateInfo pci{};
    pci.setStages(stages);
    pci.layout = *layout;
    pci.pViewportState = &vs;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pColorBlendState = &cb;
    pci.pNext = &ri;
    return vk::raii::Pipeline(fx.device(), nullptr, pci);
}

// ── 几何/场景（闭式推导见文件头）─────────────────────────────────────────────
constexpr float zToD(float z) { return (9.99f - z) / 99.99f; }

struct Tri {
    float uv[6];  // v0.xy v1.xy v2.xy
    float d[3];
};

constexpr Tri kTris[kTriCount] = {
    {{0.f, 0.f, 0.5f, 0.f, 0.f, 0.125f}, {19.5f, 19.5f, 19.5f}},              // t0 矩形下半
    {{0.5f, 0.f, 0.5f, 0.125f, 0.f, 0.125f}, {19.5f, 19.5f, 19.5f}},          // t1 矩形上半
    {{0.5f, 0.5f, 0.75f, 0.5f, 0.5f, 0.625f}, {20.f, 20.f, 20.25f}},          // t2 斜深度
    {{0.27f, 0.005f, 0.36f, 0.005f, 0.315f, 0.10f}, {19.f, 19.f, 19.f}},      // t3 sub-texel
    {{0.28f, 0.0655f, 0.35f, 0.0655f, 0.315f, 0.095f}, {18.f, 18.f, 18.f}},   // t4 保守专属
};

float edgeF(const glm::vec2& a, const glm::vec2& b, const glm::vec2& p) {
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

bool inside2D(const Tri& t, const glm::vec2& p) {
    const glm::vec2 v0{t.uv[0], t.uv[1]}, v1{t.uv[2], t.uv[3]}, v2{t.uv[4], t.uv[5]};
    return edgeF(v0, v1, p) > 0.f && edgeF(v1, v2, p) > 0.f && edgeF(v2, v0, p) > 0.f;
}

// frag 深度 = 三角光深度在 cell 中心的线性插值（复刻 GPU 的 texel 粒度近似）
float fragDepthAt(const Tri& t, const glm::vec2& c) {
    const glm::vec2 v0{t.uv[0], t.uv[1]}, v1{t.uv[2], t.uv[3]}, v2{t.uv[4], t.uv[5]};
    const glm::vec2 e0 = v1 - v0, e1 = v2 - v0, e2 = c - v0;
    const float den = e0.x * e1.y - e0.y * e1.x;
    (void)den;
    float beta = 0.f, gamma = 0.f;
    if (std::fabs(e0.x) > std::fabs(e0.y)) {
        const float m01 = e1.x / e0.x, m20 = e2.x / e0.x;
        gamma = (m20 * e0.y - e2.y) / (m01 * e0.y - e1.y);
        beta = m20 - gamma * m01;
    } else {
        const float m10 = e1.y / e0.y, m21 = e2.y / e0.y;
        beta = (m21 * e0.x - e1.x) / (m10 * e0.x - e1.y);
        gamma = m21 - beta * m10;
    }
    return t.d[0] + beta * (t.d[1] - t.d[0]) + gamma * (t.d[2] - t.d[0]);
}

}  // namespace

}  // namespace RGL::testing::izbmask

TEST_SUITE("gpu") {

using RGL::testing::gpu::makeImage;
using RGL::testing::izbmask::kTriCount;

TEST_CASE("izb_1d：光空间保守光栅 × IZB 走链 mask（OVER/DISABLED 双跑对拍）") {
    using namespace RGL::testing::izbmask;
    const Fixture fx;

    // ── 相机与光（与 1c 完全同口径：eye 水平、光正上方方向光）──────────────
    const glm::mat4 view = glm::lookAt(glm::vec3(0.f, 0.f, 10.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
    const glm::mat4 proj = glm::ortho(-2.f, 2.f, -2.f, 2.f, 0.01f, 100.f);
    const glm::mat4 invViewProj = glm::inverse(proj * view);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0.f, 20.f, 0.f), glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f));

    // ── 深度表（1c 锚点 10 点 + (6,10)/(8,10) + 两负向）───────────────────
    std::vector<float> depthIn(kRes * kRes, std::numeric_limits<float>::max());
    const int aPx = 6;
    depthIn[3 * kRes + aPx] = zToD(0.90f);
    depthIn[8 * kRes + aPx] = zToD(0.88f);
    depthIn[13 * kRes + aPx] = zToD(0.86f);
    depthIn[5 * kRes + 9] = zToD(-0.1f);
    depthIn[1 * kRes + 4] = 2.0f;         // ≥ emptyDepth → 无节点
    depthIn[7 * kRes + 7] = zToD(1.5f);   // uv 越界 → 建链期丢弃
    const int sPx[6] = {4, 7, 10, 5, 8, 11};
    const int sPy[6] = {2, 9, 12, 1, 6, 11};
    const float sZ[6] = {0.85f, 0.7f, 0.55f, 0.4f, -0.15f, -0.45f};
    for (int i = 0; i < 6; ++i)
        depthIn[sPy[i] * kRes + sPx[i]] = zToD(sZ[i]);
    depthIn[10 * kRes + 6] = zToD(-0.12f);  // (6,10)：uv (0.3125,0.11)
    depthIn[10 * kRes + 8] = zToD(-0.10f);  // (8,10)：uv (0.5625,0.55)

    // ── CPU 参考：pixelUV + 参考 mask（OVER 规则 & 中心规则 DISABLED）─────
    std::vector<float> pixelUv(kRes * kRes * 2, 0.f);
    std::array<uint32_t, kRes * kRes> refMaskOver{};
    std::array<uint32_t, kRes * kRes> refMaskCenter{};
    for (uint32_t p = 0; p < kRes * kRes; ++p) {
        const int px = static_cast<int>(p % kRes), py = static_cast<int>(p / kRes);
        const float ndcx = (px + 0.5f) / kRes * 2.f - 1.f;
        const float ndcy = (py + 0.5f) / kRes * 2.f - 1.f;
        const float d = depthIn[p];
        if (d >= kEmptyDepth)
            continue;
        const glm::vec4 w = invViewProj * glm::vec4(ndcx, ndcy, d, 1.f);
        const glm::vec4 ls = lightView * w;
        const float u = ls.x * 0.5f + 0.5f;
        const float v = ls.y * 0.5f + 0.5f;
        if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f)
            continue;  // 建链期丢弃，与着色器同判据
        pixelUv[p * 2] = u;
        pixelUv[p * 2 + 1] = v;
        const glm::vec2 uv{u, v};
        const int cellX = static_cast<int>(std::floor(u * kGrid));
        const int cellY = static_cast<int>(std::floor(v * kGrid));
        const glm::vec2 center{(cellX + 0.5f) / kGrid, (cellY + 0.5f) / kGrid};
        for (uint32_t t = 0; t < kTriCount; ++t) {
            const float dFrag = fragDepthAt(kTris[t], center);
            if (-ls.z > dFrag && inside2D(kTris[t], uv))
                refMaskOver[p] |= 1u << t;
            // DISABLED（中心规则）：三角须盖住 frag 所在 cell 中心才有 fragment
            if (-ls.z > dFrag && inside2D(kTris[t], uv) && inside2D(kTris[t], center))
                refMaskCenter[p] |= 1u << t;
        }
    }

    // ── 手写锚点（文件头推导，独立于上面参考实现）─────────────────────────
    const std::map<uint32_t, uint32_t> kAnchor = {{36u, 0b00001u}, {54u, 0b01010u},
        {104u, 0b00100u}, {134u, 0b01010u}, {214u, 0b10000u}};
    for (const auto& [id, want] : kAnchor) {
        CAPTURE(id);
        CHECK(refMaskOver[id] == want);  // 参考实现 vs 手写锚点（先于 GPU）
    }
    for (uint32_t p = 0; p < kRes * kRes; ++p) {
        if (kAnchor.count(p) == 0)
            CHECK(refMaskOver[p] == 0u);  // 锚点之外的像素必须全零
    }

    // ── GPU 资源 ───────────────────────────────────────────────────────────
    std::vector<uint32_t> headInit(kGrid * kGrid, kNone);
    std::vector<uint32_t> nodeInit(kNodeBytes / 4, 0xFFFFFFFFu);
    std::vector<float> primedInit(kGrid * kGrid, 0.f);
    std::vector<uint8_t> params(kParamsBytes, 0);
    auto putI = [&](size_t off, int32_t v) { std::memcpy(params.data() + off, &v, 4); };
    putI(0, static_cast<int32_t>(kRes));
    putI(4, static_cast<int32_t>(kGrid));
    const float empty = kEmptyDepth;
    std::memcpy(params.data() + 8, &empty, 4);
    putI(12, static_cast<int32_t>(kRes * kRes));
    std::memcpy(params.data() + 16, glm::value_ptr(invViewProj), 64);
    std::memcpy(params.data() + 80, glm::value_ptr(lightView), 64);

    std::vector<uint8_t> params2(kParams2Bytes, 0);
    auto putI2 = [&](size_t off, int32_t v) { std::memcpy(params2.data() + off, &v, 4); };
    auto putF2 = [&](size_t off, float v) { std::memcpy(params2.data() + off, &v, 4); };
    putI2(0, static_cast<int32_t>(kTriCount));
    putI2(4, static_cast<int32_t>(kGrid));
    for (uint32_t t = 0; t < kTriCount; ++t) {
        const size_t b = 16 + 64 * t;
        for (int i = 0; i < 6; ++i)
            putF2(b + 4 * i, kTris[t].uv[i]);
        for (int i = 0; i < 3; ++i)
            putF2(b + 24 + 4 * i, kTris[t].d[i]);
    }

    auto ssboHead = makeHostSsbo(fx, kHeadBytes, headInit.data());
    auto ssboNodes = makeHostSsbo(fx, kNodeBytes, nodeInit.data());
    auto ssboDepth = makeHostSsbo(fx, kDepthBytes, depthIn.data());
    auto ssboPrimed = makeHostSsbo(fx, kPrimedBytes, primedInit.data());
    auto ssboParams = makeHostSsbo(fx, kParamsBytes, params.data());
    std::vector<uint32_t> maskInit(kRes * kRes, 0u);
    auto ssboMask = makeHostSsbo(fx, kMaskBytes, maskInit.data());
    auto ssboPixelUv = makeHostSsbo(fx, kPixelUvBytes, pixelUv.data());
    auto ssboParams2 = makeHostSsbo(fx, kParams2Bytes, params2.data());

    // 8 槽位：0 head, 1 nodes, 2 depthIn, 3 primed, 4 params（izb_create 布局）
    //          5 mask, 6 pixelUV, 7 params2（izb_mask 布局；stage = compute|graphics）
    std::array<vk::DescriptorSetLayoutBinding, 8> bindings{};
    for (uint32_t i = 0; i < 8; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute | vk::ShaderStageFlagBits::eAllGraphics;
    }
    vk::DescriptorSetLayoutCreateInfo li{};
    li.setBindings(bindings);
    const vk::raii::DescriptorSetLayout dsLayout(fx.device(), li);
    const std::array<vk::DescriptorPoolSize, 1> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8}};
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
    const std::array<vk::DescriptorBufferInfo, 8> bufInfos{{
        {*ssboHead.buffer, 0, kHeadBytes},
        {*ssboNodes.buffer, 0, kNodeBytes},
        {*ssboDepth.buffer, 0, kDepthBytes},
        {*ssboPrimed.buffer, 0, kPrimedBytes},
        {*ssboParams.buffer, 0, kParamsBytes},
        {*ssboMask.buffer, 0, kMaskBytes},
        {*ssboPixelUv.buffer, 0, kPixelUvBytes},
        {*ssboParams2.buffer, 0, kParams2Bytes},
    }};
    std::array<vk::WriteDescriptorSet, 8> writes{};
    for (uint32_t i = 0; i < 8; ++i) {
        writes[i].dstSet = *sets[0];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[i].pBufferInfo = &bufInfos[i];
    }
    fx.device().updateDescriptorSets(writes, {});

    vk::PipelineLayoutCreateInfo plci{};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &*dsLayout;
    const vk::raii::PipelineLayout pipeLayout(fx.device(), plci);

    const auto modCreate = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_create.comp.spv");
    const auto modMaskMesh = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_mask.mesh.spv");
    const auto modMaskFrag = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_mask.frag.spv");
    const vk::raii::Pipeline pipeCreate([&] {
        vk::PipelineShaderStageCreateInfo st{};
        st.stage = vk::ShaderStageFlagBits::eCompute;
        st.module = *modCreate;
        st.pName = "main";
        vk::ComputePipelineCreateInfo ci{};
        ci.stage = st;
        ci.layout = *pipeLayout;
        return vk::raii::Pipeline(fx.device(), nullptr, ci);
    }());
    const auto pipeOver = makePipeline(fx, pipeLayout, modMaskMesh, modMaskFrag,
        vk::ConservativeRasterizationModeEXT::eOverestimate);
    const auto pipeDisabled = makePipeline(fx, pipeLayout, modMaskMesh, modMaskFrag,
        vk::ConservativeRasterizationModeEXT::eDisabled);

    auto target = [&] {
        auto img = makeImage(fx, kFmt, kGrid, kGrid,
            vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc);
        vk::ImageViewCreateInfo vi{};
        vi.image = *img.image;
        vi.viewType = vk::ImageViewType::e2D;
        vi.format = kFmt;
        vi.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        vk::raii::ImageView view(fx.device(), vi);
        return std::pair{std::move(img), std::move(view)};
    }();

    const auto bufBarrier = [](vk::Buffer buffer, vk::PipelineStageFlags2 srcS, vk::AccessFlags2 srcA,
                                  vk::PipelineStageFlags2 dstS, vk::AccessFlags2 dstA) {
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
    };
    const auto imgBarrier = [](vk::Image img, vk::ImageLayout oldL, vk::ImageLayout newL,
                                  vk::PipelineStageFlags2 srcS, vk::AccessFlags2 srcA,
                                  vk::PipelineStageFlags2 dstS, vk::AccessFlags2 dstA) {
        vk::ImageMemoryBarrier2 b{};
        b.srcStageMask = srcS;
        b.srcAccessMask = srcA;
        b.dstStageMask = dstS;
        b.dstAccessMask = dstA;
        b.oldLayout = oldL;
        b.newLayout = newL;
        b.image = img;
        b.subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        return b;
    };

    const auto renderMask = [&](vk::raii::CommandBuffer& cmd, const vk::raii::Pipeline& pipe) {
        vk::RenderingAttachmentInfo att{};
        att.imageView = *target.second;
        att.imageLayout = vk::ImageLayout::eColorAttachmentOptimal;
        att.loadOp = vk::AttachmentLoadOp::eClear;
        att.storeOp = vk::AttachmentStoreOp::eStore;
        att.clearValue = vk::ClearColorValue(std::array{0.0f, 0.0f, 0.0f, 0.0f});
        vk::RenderingInfo r{};
        r.renderArea = vk::Rect2D{vk::Offset2D{0, 0}, vk::Extent2D{kGrid, kGrid}};
        r.layerCount = 1;
        r.setColorAttachments(att);
        cmd.beginRendering(r);
        cmd.bindPipeline(vk::PipelineBindPoint::eGraphics, *pipe);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eGraphics, *pipeLayout, 0, *sets[0], {});
        cmd.drawMeshTasksEXT(kTriCount, 1, 1);
        cmd.endRendering();
    };

    // ── 提交 A：建链 → OVER 光栅 mask ─────────────────────────────────────
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        const std::array<vk::BufferMemoryBarrier2, 8> pre{{
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboDepth.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboPrimed.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboParams.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboMask.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite),
            bufBarrier(*ssboPixelUv.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboParams2.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
        }};
        vk::DependencyInfo dep0{};
        dep0.setBufferMemoryBarriers(pre);
        cmd.pipelineBarrier2(dep0);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeCreate);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeLayout, 0, *sets[0], {});
        cmd.dispatch((kRes * kRes + 63) / 64, 1, 1);

        // 链 → 光栅读
        const std::array<vk::BufferMemoryBarrier2, 2> mid{{
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
        }};
        vk::DependencyInfo dep1{};
        dep1.setBufferMemoryBarriers(mid);
        cmd.pipelineBarrier2(dep1);

        vk::DependencyInfo depImg{};
        const auto ib = imgBarrier(*target.first.image, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
        depImg.setImageMemoryBarriers(ib);
        cmd.pipelineBarrier2(depImg);

        renderMask(cmd, pipeOver);

        const std::array<vk::BufferMemoryBarrier2, 3> post{{
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
            bufBarrier(*ssboMask.buffer, vk::PipelineStageFlagBits2::eAllGraphics,
                vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
        }};
        vk::DependencyInfo dep2{};
        dep2.setBufferMemoryBarriers(post);
        cmd.pipelineBarrier2(dep2);
    });

    std::array<uint32_t, kRes * kRes> maskOver{};
    std::memcpy(maskOver.data(), ssboMask.mapped, kMaskBytes);

    // ── 提交 B：host 清 mask → DISABLED 光栅 ──────────────────────────────
    std::memset(ssboMask.mapped, 0, kMaskBytes);
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        const std::array<vk::BufferMemoryBarrier2, 5> pre{{
            bufBarrier(*ssboMask.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostWrite,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite),
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboPixelUv.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboParams2.buffer, vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead,
                vk::PipelineStageFlagBits2::eAllGraphics, vk::AccessFlagBits2::eShaderRead),
        }};
        vk::DependencyInfo dep0{};
        dep0.setBufferMemoryBarriers(pre);
        cmd.pipelineBarrier2(dep0);

        vk::DependencyInfo depImg{};
        const auto ib = imgBarrier(*target.first.image, vk::ImageLayout::eUndefined,
            vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eTopOfPipe,
            vk::AccessFlagBits2::eNone, vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            vk::AccessFlagBits2::eColorAttachmentWrite);
        depImg.setImageMemoryBarriers(ib);
        cmd.pipelineBarrier2(depImg);

        renderMask(cmd, pipeDisabled);

        vk::DependencyInfo dep2{};
        const auto mb = bufBarrier(*ssboMask.buffer, vk::PipelineStageFlagBits2::eAllGraphics,
            vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
            vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead);
        dep2.setBufferMemoryBarriers(mb);
        cmd.pipelineBarrier2(dep2);
    });
    std::array<uint32_t, kRes * kRes> maskDisabled{};
    std::memcpy(maskDisabled.data(), ssboMask.mapped, kMaskBytes);

    // ── 判据 ②：OVER 全表 == CPU 参考 ──────────────────────────────────────
    for (uint32_t p = 0; p < kRes * kRes; ++p) {
        CAPTURE(p);
        if (maskOver[p] != refMaskOver[p])
            INFO(std::format("pix{} got={:05b} want={:05b}", p, maskOver[p], refMaskOver[p]));
        CHECK(maskOver[p] == refMaskOver[p]);
    }

    // ── 判据 ③：DISABLED == 中心规则 ──────────────────────────────────────
    for (uint32_t p = 0; p < kRes * kRes; ++p) {
        CAPTURE(p);
        if (maskDisabled[p] != refMaskCenter[p])
            INFO(std::format("pix{} got={:05b} want(center)={:05b}", p, maskDisabled[p], refMaskCenter[p]));
        CHECK(maskDisabled[p] == refMaskCenter[p]);
    }

    // ── 判据 ③'：保守专属单点对（本测试的主角）────────────────────────────
    CHECK(maskOver[214] == 0b10000u);     // OVER：t4 条带保守触达 → 判定通过 → bit4
    CHECK(maskDisabled[214] == 0u);       // DISABLED：中心采样全盲 → 无 fragment

    // ── 判据 ④：阳性锚点（两跑各自非零计数）──────────────────────────────
    int nzOver = 0, nzDisabled = 0;
    for (const uint32_t m : maskOver)
        nzOver += m != 0u;
    for (const uint32_t m : maskDisabled)
        nzDisabled += m != 0u;
    CHECK(nzOver == 5);
    CHECK(nzDisabled == 4);
}

}  // TEST_SUITE("gpu")
