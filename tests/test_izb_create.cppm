module;
/**
 * test_izb_create —— [gpu] 批 11.3-1c：IZB 创建 dispatch + early-Z priming
 * （docs/hfts_izb_plan.md §4.1-1c；FTIZB 2015 pass3/4 的 spike 级验证）
 *
 * ## 这一层为什么单独测（不等 1d 真实场景）
 *
 * IZB 的骨架 = **grid-of-lists 原子插入**（head/next 像素 ID 链表，地址即像素索引）
 * + **priming 写最远节点深度**。这两件事全在 compute 里，输入一个深度数组就能跑——
 * 用合成场景在设备原语层把**链表机制**钉死（插入完整性、环、越界丢弃、priming 语义），
 * 1d 接真实几何时坏的候选面就只剩几何投影本身。论文对创建 pass 的全部要求
 * （texel 平均节点数个位数、无长链爆炸）本质是对**这层数据结构**的断言。
 *
 * ## 场景（合成，确定性，零 RNG）
 *
 * eye = 水平正交相机（world x→屏幕 x、y→屏幕 y、深度 d→world z 线性带）；
 * 光 = 正上方方向光 lookAt((0,20,0)→原点)。于是全部闭式：
 *   世界 (x,y,z) 的  light 深度 = 20 − y；uv = (x·0.5+0.5, −z·0.5+0.5)；
 *   cell = floor(uv·8)；像素 (px,py) 的 x = (px+0.5)/4−2、y = (py+0.5)/4−2；
 *   d(z) = (9.99−z)/99.99（glm ortho n=0.01 f=100 Vulkan 深度，推导见场景表注释）。
 * 摆点：
 *   A 组 3 像素（同列 px=6，py 3/8/13，z .9/.88/.86 → 同 cell (2,0)）→ 长度 3 链，
 *     手工锚点（独立字面量，非 CPU 参考的输出！）：深度 = 20−y = {21.125, 19.875, 18.625}；
 *   B 组 1 像素 → 独苗 cell (5,4)，锚点深度 20.625；
 *   C 组：空哨兵 FLT_MAX（默认）+ d=2.0 ≥ emptyDepth=1.0 → **空像素不建节点**；
 *   D 组 1 像素 z=1.5 → uv 越出网格 → **越界丢弃**（其像素 ID 必须不可达）；
 *   scatter 6 点（手定表，非 RNG）铺出真实形状 → 顺带产出链长直方图（INFO，
 *     对照论文 Fig.4 的口径；合成场景只验机制，统计口径留给 1d 真场景）。
 * 所有落点与 cell 边界保持 ≥0.2 cell 的设计余量（构建期 CHECK，输入设计自证）。
 *
 * ## 判据（顺序无关——原子链表的顺序天然不确定，判据必须只依赖不变量）
 *
 * ① 每 cell 链深**多重集** == CPU 参考多重集（排序后 expectNear 逐对）；
 * ② 链以 NONE 结尾且游走步数 ≤ 像素数+1（**环检测**：坏链是原子写错的典型症状）；
 * ③ 全体可达像素 ID == 预期插入集合（不重不漏——漏插/双插都在此红）；
 * ④ priming 逐 cell == max(链深)（空 cell 恰为 0）；「非空 cell 变非零」计数 == 预期
 *    非空格数（**priming 真跑了**的阳性锚点，防"dispatch 忘发、初值恰好全对"的假绿）；
 * ⑤ D/C 组像素 ID 在全表不可达（反向用例）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   InterlockedExchange 退化成"只写 head 不接 next" → A 组长度 1、集合少 2 → ①③ 红
 *   payload 写漏/写错（ls.z 符号反） → ①④ 红（20−y vs y−20 差得远）
 *   emptyDepth 判据删掉 → FLT_MAX 像素入链 → ③ 红（可达集合多出）
 *   越界检查删掉 → 越界 cell 索引写出数组（VVL buffer-oob 当场打断）→ 红
 *   prime dispatch 忘发 → ④ 非空格计数 0 红
 *   priming 取 min 不取 max → ④ A 组红（21.125 → 18.625）
 *
 * ## ⚠️ 描述符红线（2026-10-01 用户拍板，勿搬运）
 *
 * 本测试用**老式 VkDescriptorPool + descriptor set**——仅因 spike 放宽（fixture 没有
 * VMA/Resources，接全量 heap 会把「验链表」变成「验引擎装配」）。**引擎集成（1d）绝不允许
 * 这套**：必须走 `vk_resources` 的 VmaDescriptorHeap 用户槽（三段分配器 runtime 段），
 * 纪律与机制 → `docs/DESCRIPTOR_HEAP_SPEC.md`。本文件的 descriptor 代码 = 反例存档。
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

export module test_izb_create;

import rlstd;
import rlvk;
import test_device_primitives;  // Fixture / submitAndWait（引擎同款引导链路）
import test_support;            // expectNear（明面断言：实际/期望/容差全打出来）

namespace RGL::testing::izbcreate {

using testing::gpu::Fixture;  // 引擎同款夹具在 RGL::testing::gpu（本文件命名空间内先解锁）

namespace {

constexpr uint32_t kRes = 16;      // 像素网格边长（输入深度）
constexpr uint32_t kGrid = 8;      // 光空间网格边长 G
constexpr float kEmptyDepth = 1.0f;  // 与着色器/场景表同一语义：d ≥ 此值不建节点
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kHeadBytes = kGrid * kGrid * 4;
constexpr size_t kNodeBytes = kRes * kRes * 8;      // {float depth, uint next}
constexpr size_t kDepthBytes = kRes * kRes * 4;
constexpr size_t kPrimedBytes = kGrid * kGrid * 4;
constexpr size_t kParamsBytes = 160;                // 布局声明 = izb_create.comp.slang 头注释

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
    void* p = mem.mapMemory(0, bytes);
    if (src)
        std::memcpy(p, src, bytes);
    // 刻意**不 unmap**：提交后经同一映射指针直读回传数据（HOST_COHERENT）。
    // 提前 unmap 再读 = 悬空指针（本测试首跑 SEGV 的根因）。free 时隐式 unmap 合法。
    return HostSsbo{.buffer = std::move(buf), .memory = std::move(mem), .mapped = p};
}

const uint32_t* headOf(const HostSsbo& s) { return static_cast<const uint32_t*>(s.mapped); }
const float* floatsOf(const HostSsbo& s) { return static_cast<const float*>(s.mapped); }
struct GpuNode { float depth; uint32_t next; };
const GpuNode* nodesOf(const HostSsbo& s) { return static_cast<const GpuNode*>(s.mapped); }

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
    mi.setCode(code);  // ArrayProxyNoTemporaries：必须具名左值
    return vk::raii::ShaderModule(fx.device(), mi);
}

vk::raii::Pipeline makeCompute(const Fixture& fx, const vk::raii::PipelineLayout& layout,
    const vk::raii::ShaderModule& mod) {
    vk::PipelineShaderStageCreateInfo st{};
    st.stage = vk::ShaderStageFlagBits::eCompute;
    st.module = *mod;
    st.pName = "main";
    vk::ComputePipelineCreateInfo ci{};
    ci.stage = st;
    ci.layout = *layout;  // 字段名是 layout（setLayout 是 setter 函数名，勿混）
    return vk::raii::Pipeline(fx.device(), nullptr, ci);
}

// ── CPU 参考（数学契约：矩阵变换 + floor；与着色器同式不同实现路径）────────────
struct Placement {
    bool inserted = false;
    int cell = -1;
    float lightDepth = 0.f;
};

Placement projectPixel(int px, int py, float d, const glm::mat4& invViewProj,
    const glm::mat4& lightView) {
    Placement out;
    const float ndcx = (px + 0.5f) / static_cast<float>(kRes) * 2.f - 1.f;
    const float ndcy = (py + 0.5f) / static_cast<float>(kRes) * 2.f - 1.f;
    const glm::vec4 w = invViewProj * glm::vec4(ndcx, ndcy, d, 1.f);
    const glm::vec4 ls = lightView * w;
    const float u = ls.x * 0.5f + 0.5f;
    const float v = ls.y * 0.5f + 0.5f;
    if (u < 0.f || u >= 1.f || v < 0.f || v >= 1.f)
        return out;  // 越界丢弃（与着色器同判据）
    const int cu = static_cast<int>(std::floor(u * kGrid));
    const int cv = static_cast<int>(std::floor(v * kGrid));
    out.inserted = true;
    out.cell = cu + cv * static_cast<int>(kGrid);
    out.lightDepth = -ls.z;
    return out;
}

constexpr float zToD(float z) { return (9.99f - z) / 99.99f; }
// 推导：eye 在 (0,0,10) 沿 -Z 看，view z = wz − 10；glm ortho(Vulkan 0..1)：
//   d = (10 − wz − n) / (f − n) = (9.99 − wz)/99.99。

}  // namespace

}  // namespace RGL::testing::izbcreate

TEST_SUITE("gpu") {

using RGL::testing::gpu::Fixture;
using RGL::testing::izbcreate::kDepthBytes;
using RGL::testing::izbcreate::kEmptyDepth;
using RGL::testing::izbcreate::kGrid;
using RGL::testing::izbcreate::kHeadBytes;
using RGL::testing::izbcreate::kNodeBytes;
using RGL::testing::izbcreate::kNone;
using RGL::testing::izbcreate::kPrimedBytes;
using RGL::testing::izbcreate::kRes;
using RGL::testing::izbcreate::zToD;

TEST_CASE("izb_1c：IZB 原子链表创建 + priming 全语义（合成场景，闭式锚点）") {
    using namespace RGL::testing::izbcreate;
    const Fixture fx;

    // ── 相机与光（闭式口径见文件头）────────────────────────────────────────
    const glm::mat4 view = glm::lookAt(glm::vec3(0.f, 0.f, 10.f), glm::vec3(0.f), glm::vec3(0.f, 1.f, 0.f));
    const glm::mat4 proj = glm::ortho(-2.f, 2.f, -2.f, 2.f, 0.01f, 100.f);
    const glm::mat4 invViewProj = glm::inverse(proj * view);
    const glm::mat4 lightView = glm::lookAt(glm::vec3(0.f, 20.f, 0.f), glm::vec3(0.f), glm::vec3(0.f, 0.f, -1.f));

    // ── 深度输入（场景摆点表 = 文件头）──────────────────────────────────────
    std::vector<float> depthIn(kRes * kRes, std::numeric_limits<float>::max());
    const int aPx = 6;
    depthIn[3 * kRes + aPx] = zToD(0.90f);   // A 组：同 cell (2,0)，长度 3
    depthIn[8 * kRes + aPx] = zToD(0.88f);
    depthIn[13 * kRes + aPx] = zToD(0.86f);
    depthIn[5 * kRes + 9] = zToD(-0.1f);     // B 组：独苗 cell (5,4)
    depthIn[1 * kRes + 4] = 2.0f;            // C 组：≥ emptyDepth → 不建节点
    const uint32_t idxD = 7 * kRes + 7;
    depthIn[idxD] = zToD(1.5f);              // D 组：uv 越界 → 丢弃
    const int sPx[6] = {4, 7, 10, 5, 8, 11}; // scatter：手定表（非 RNG，可复现）
    const int sPy[6] = {2, 9, 12, 1, 6, 11}; // ⚠️ 勿与 A/B/C/D 组落点相撞（i1 曾是 (7,7)=idxD）
    const float sZ[6] = {0.85f, 0.7f, 0.55f, 0.4f, -0.15f, -0.45f};
    for (int i = 0; i < 6; ++i)
        depthIn[sPy[i] * kRes + sPx[i]] = zToD(sZ[i]);

    // ── CPU 参考：期望 cell 归属与深度多重集 + 输入设计自证（边界余量）────
    std::map<int, std::vector<float>> expectCells;
    std::map<uint32_t, Placement> expectPixels;
    for (uint32_t p = 0; p < kRes * kRes; ++p) {
        const float d = depthIn[p];
        if (d >= kEmptyDepth)
            continue;
        const int px = static_cast<int>(p % kRes), py = static_cast<int>(p / kRes);
        const auto pl = projectPixel(px, py, d, invViewProj, lightView);
        if (!pl.inserted)
            continue;
        // 输入设计自证：落点离 cell 边界 ≥0.2 cell（float 噪声翻不动 cell）
        const float ndcx = (px + 0.5f) / kRes * 2.f - 1.f;
        const float ndcy = (py + 0.5f) / kRes * 2.f - 1.f;
        const glm::vec4 w = invViewProj * glm::vec4(ndcx, ndcy, d, 1.f);
        const glm::vec4 ls = lightView * w;
        const float bu = std::abs(std::floor((ls.x * 0.5f + 0.5f) * kGrid) - (ls.x * 0.5f + 0.5f) * kGrid);
        const float bv = std::abs(std::floor((ls.y * 0.5f + 0.5f) * kGrid) - (ls.y * 0.5f + 0.5f) * kGrid);
        CHECK(std::min(bu, bv) > 0.2f);
        expectCells[pl.cell].push_back(pl.lightDepth);
        expectPixels[p] = pl;
    }
    // 场景设计自证：预期插入 10 点（A3 + B1 + scatter6），D 组不在其中
    CHECK(expectPixels.size() == 10);
    CHECK(expectPixels.count(idxD) == 0);

    // ── 手工锚点（独立字面量：light 深度 = 20 − y、y = (py+0.5)/4 − 2）────
    // cell 索引 = cu + cv·8：A=(2,0)→2；B=(5,4)→37；scatter i2=(6,1)→14。
    REQUIRE(expectCells[2].size() == 3);  // 先钉大小，锚点索引不越界
    REQUIRE(expectCells[37].size() == 1);
    REQUIRE(expectCells[14].size() == 1);
    RGL::testing::expectNear(expectCells[2][0] + expectCells[2][1] + expectCells[2][2],
        21.125 + 19.875 + 18.625, 1e-5);
    RGL::testing::expectNear(expectCells[37][0], 20.625, 1e-5);
    RGL::testing::expectNear(expectCells[14][0], 18.875, 1e-5);

    // ── GPU 资源 ───────────────────────────────────────────────────────────
    std::vector<uint32_t> headInit(kGrid * kGrid, kNone);
    std::vector<float> primedInit(kGrid * kGrid, 0.f);
    std::vector<uint8_t> params(kParamsBytes, 0);
    auto putI = [&](size_t off, int32_t v) { std::memcpy(params.data() + off, &v, 4); };
    auto putF = [&](size_t off, float v) { std::memcpy(params.data() + off, &v, 4); };
    putI(0, static_cast<int32_t>(kRes));
    putI(4, static_cast<int32_t>(kGrid));
    putF(8, kEmptyDepth);
    putI(12, static_cast<int32_t>(kRes * kRes));
    std::memcpy(params.data() + 16, glm::value_ptr(invViewProj), 64);
    std::memcpy(params.data() + 80, glm::value_ptr(lightView), 64);

    auto ssboHead = makeHostSsbo(fx, kHeadBytes, headInit.data());
    // nodes 初始化为全 0xFF（next=NONE）：坏链读未初始化槽位时行为确定，不靠运气
    const std::vector<uint32_t> nodeInit(kNodeBytes / 4, 0xFFFFFFFFu);
    auto ssboNodes = makeHostSsbo(fx, kNodeBytes, nodeInit.data());
    auto ssboDepth = makeHostSsbo(fx, kDepthBytes, depthIn.data());
    auto ssboPrimed = makeHostSsbo(fx, kPrimedBytes, primedInit.data());
    auto ssboParams = makeHostSsbo(fx, kParamsBytes, params.data());

    // 5 槽位：0 head, 1 nodes, 2 depthIn, 3 primed, 4 params（两管线共用一套）
    std::array<vk::DescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t i = 0; i < 5; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = vk::ShaderStageFlagBits::eCompute;
    }
    vk::DescriptorSetLayoutCreateInfo li{};
    li.setBindings(bindings);
    const vk::raii::DescriptorSetLayout dsLayout(fx.device(), li);
    const std::array<vk::DescriptorPoolSize, 1> poolSizes{
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 5}};
    vk::DescriptorPoolCreateInfo pi{};
    pi.flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;  // raii 析构要归还 set
    pi.setPoolSizes(poolSizes);
    pi.setMaxSets(1);
    const vk::raii::DescriptorPool pool(fx.device(), pi);
    vk::DescriptorSetAllocateInfo sai{};
    sai.descriptorPool = *pool;
    sai.descriptorSetCount = 1;
    sai.pSetLayouts = &*dsLayout;
    const auto sets = fx.device().allocateDescriptorSets(sai);
    REQUIRE(sets.size() == 1);
    const std::array<vk::DescriptorBufferInfo, 5> bufInfos{{
        {*ssboHead.buffer, 0, kHeadBytes},
        {*ssboNodes.buffer, 0, kNodeBytes},
        {*ssboDepth.buffer, 0, kDepthBytes},
        {*ssboPrimed.buffer, 0, kPrimedBytes},
        {*ssboParams.buffer, 0, kParamsBytes},
    }};
    std::array<vk::WriteDescriptorSet, 5> writes{};
    for (uint32_t i = 0; i < 5; ++i) {
        writes[i].dstSet = *sets[0];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = vk::DescriptorType::eStorageBuffer;
        writes[i].pBufferInfo = &bufInfos[i];
    }
    fx.device().updateDescriptorSets(writes, {});  // raii 版叫 update（vk 版才叫 write）

    // 管线布局（descriptor set layout ≠ pipeline layout，两码事两名）
    vk::PipelineLayoutCreateInfo plci{};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &*dsLayout;
    const vk::raii::PipelineLayout pipeLayout(fx.device(), plci);

    const auto modCreate = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_create.comp.spv");
    const auto modPrime = makeModule(fx, RGL_TEST_SHADER_DIR "/izb_prime.comp.spv");
    const auto pipeCreate = makeCompute(fx, pipeLayout, modCreate);
    const auto pipePrime = makeCompute(fx, pipeLayout, modPrime);

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

    const uint32_t groupsPx = (kRes * kRes + 63) / 64;
    const uint32_t groupsCell = (kGrid * kGrid + 63) / 64;
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        // host 写入（深度/params/head 初值）→ compute 读
        std::array<vk::BufferMemoryBarrier2, 5> pre{{
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
        }};
        vk::DependencyInfo dep0{};
        dep0.setBufferMemoryBarriers(pre);
        cmd.pipelineBarrier2(dep0);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeCreate);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeLayout, 0, *sets[0], {});
        cmd.dispatch(groupsPx, 1, 1);

        // create 写 head/nodes → prime 读
        std::array<vk::BufferMemoryBarrier2, 2> mid{{
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderRead),
        }};
        vk::DependencyInfo dep1{};
        dep1.setBufferMemoryBarriers(mid);
        cmd.pipelineBarrier2(dep1);

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipePrime);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeLayout, 0, *sets[0], {});
        cmd.dispatch(groupsCell, 1, 1);

        // 全部写 → host 读回
        std::array<vk::BufferMemoryBarrier2, 5> post{{
            bufBarrier(*ssboHead.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
            bufBarrier(*ssboNodes.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
            bufBarrier(*ssboDepth.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
            bufBarrier(*ssboPrimed.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
            bufBarrier(*ssboParams.buffer, vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderWrite,
                vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead),
        }};
        vk::DependencyInfo dep2{};
        dep2.setBufferMemoryBarriers(post);
        cmd.pipelineBarrier2(dep2);
    });

    // ── 判据 ①②③：链遍历（顺序无关）───────────────────────────────────────
    std::vector<uint32_t> reachedIds;
    std::map<int, int> hist;  // 链长 → cell 数（论文 Fig.4 口径的 dump）
    std::string mismatch;     // 调试 dump：一次性看全错位，不被首个 FATAL 截断
    for (uint32_t c = 0; c < kGrid * kGrid; ++c) {
        uint32_t h = headOf(ssboHead)[c];
        int steps = 0;
        bool cycle = false;
        std::vector<float> depths;
        while (h != kNone) {
            if (++steps > static_cast<int>(kRes * kRes) + 1) {
                cycle = true;  // 判据②：坏链（环）
                break;
            }
            depths.push_back(nodesOf(ssboNodes)[h].depth);
            reachedIds.push_back(h);
            h = nodesOf(ssboNodes)[h].next;
        }
        CHECK_FALSE(cycle);
        ++hist[static_cast<int>(depths.size())];
        if (!depths.empty()) {
            std::ranges::sort(depths);
            auto exp = expectCells[c];
            std::ranges::sort(exp);
            if (depths.size() != exp.size()) {
                std::string gs, es;
                for (const float v : depths)
                    gs += std::format("{:.4f} ", v);
                for (const float v : exp)
                    es += std::format("{:.4f} ", v);
                mismatch += std::format("\ncell {} got[{}]({}) want[{}]({})", c, depths.size(), gs,
                    exp.size(), es);
                continue;
            }
            for (size_t i = 0; i < depths.size(); ++i)
                RGL::testing::expectNear(depths[i], exp[i], 1e-5);  // 判据①（多重集）
        } else {
            CHECK(expectCells.count(static_cast<int>(c)) == 0);  // 空格必须真空
        }
    }
    if (!mismatch.empty())
        MESSAGE("cell mismatch dump:" << mismatch);
    CHECK(mismatch.empty());  // 判据①（长度）
    // 判据③：可达集合 == 预期插入集合（不重不漏）
    std::ranges::sort(reachedIds);
    std::vector<uint32_t> expectIds;
    for (const auto& [id, pl] : expectPixels)
        expectIds.push_back(id);
    std::ranges::sort(expectIds);
    CHECK(reachedIds == expectIds);

    // 判据⑤（反向）：D 组像素 ID 不可达已由 ③ 覆盖，此处独立钉 C/D 语义
    CHECK(std::find(reachedIds.begin(), reachedIds.end(), idxD) == reachedIds.end());

    // ── 判据④：priming = max(链深) / 空为 0 + 阳性锚点 ─────────────────────
    int primedNonZero = 0;
    for (uint32_t c = 0; c < kGrid * kGrid; ++c) {
        const float got = floatsOf(ssboPrimed)[c];
        const auto it = expectCells.find(static_cast<int>(c));
        if (it == expectCells.end()) {
            CHECK(got == 0.0f);  // 空格恰为 0（early-Z 语义要求精确 0）
        } else {
            const float want = *std::ranges::max_element(it->second);
            RGL::testing::expectNear(got, want, 1e-5);
            ++primedNonZero;
        }
    }
    CHECK(primedNonZero == static_cast<int>(expectCells.size()));  // priming 真跑了

    // 直方图 dump（论文 Fig.4 口径；合成场景形状是设计出来的，1d 换真场景看统计）
    std::string histStr;
    for (const auto& [len, cnt] : hist)
        histStr += std::format("{}:{} ", len, cnt);
    MESSAGE("izb chain-length histogram: " << histStr);
}

}  // TEST_SUITE("gpu")
