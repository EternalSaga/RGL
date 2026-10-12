module;
/**
 * test_descriptor_heap_ubo —— [gpu] VK_EXT_descriptor_heap × UBO 能力 spike
 *
 * ## 要回答的唯一问题
 *
 * resource heap 里能不能放 `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` 的 descriptor，
 * 并被 Slang compute shader 以 `ConstantBuffer<T>` 正常读取？（本机 = AMD Radeon
 * 8060S Graphics / RADV STRIX_HALO / Mesa 26.2.3）
 *
 * 全仓此前只用过 `eSampledImage` / `eStorageBuffer` / `eStorageImage` 三种描述符类型，
 * `eUniformBuffer` 从未被试过，也没有任何代码禁止它。`StructuredBuffer<T>`
 * （= `eStorageBuffer`）走同一条 heap 通路是**退路臂**，同等重要。
 *
 * ## 为什么复用引擎而不是手写裸 Vulkan 引导
 *
 * 同 `test_device_primitives` 文件头那条改过主意的结论（本文件直接继承它的 Fixture）：
 * 引擎的 AppContext/DeviceContext 已经把 raii 成员初始化序、required feature 链、
 * headless surface 这些事做对了，手写一遍等于把这些结论重新赌一次。descriptor heap
 * 封装也**直接复用** `VmaRaii` 的 `VmaAllocatorRAII::createDescriptorHeap`（双端对齐
 * 记账都在里面），binding→heap 映射烘焙**直接复用** `shader` 模块的 `makeMappedCompute`
 * ——理由更强：本 spike 要回答的正是「**引擎那条** mapping 通路能不能带 UBO」，
 * 自造一份 mapping 构造代码就验的不是引擎那条路了。
 *
 * ## 顺带必须钉死的架构事实：binding → heap 槽的真实映射机制
 *
 * 立项时的假设是「binding 号 == heap 槽号」。**本文件用构造把它证伪**：
 * 真实机制 = `vkCreateShadersEXT` 期由 `VkDescriptorSetAndBindingMappingEXT`
 * 逐 binding 烘焙 `heapOffset`（**字节**），槽号来自 mapping，与 binding 号无关
 * （`shader.cppm::makeMappedCompute` 里 `heapOffset = slot * descriptorSize`）。
 * 判据设计（本文件的核心，缺它这条就只是"跑通了"而不是"机制查清了"）：
 *   ① 真描述符放在**不等于任何 binding 号**的槽（5/11/13/7 + reserved）；
 *   ② 同时在 binding 号对应的槽（0/1/2/3 + reserved）写入**诱饵**描述符
 *      （指向另一块 buffer、装另一组 magic）。
 *   ⟹ 若驱动真按「binding == 槽」取描述符，读到的是诱饵（且输出会写进诱饵 out
 *      buffer，真 out buffer 全零）；若按 mapping 的 heapOffset 取，读到真值。
 *   两种假说在**同一次 dispatch** 里被同一组断言分开，且失败时诱饵 out 的落字
 *   直接指出驱动实际走了哪条路（诊断信息，不只是红/绿）。
 *
 * ## 三条判据（缺一条不算验过）
 *
 *   ① 主判据：CPU 写 magic → shader 按 SPIR-V `Offset` 装饰读出 → 写回 → 逐字节相等。
 *      11 个输出槽位各装**互不相同的非零** magic（含 float 位模式），任何"整体错位
 *      一格"会同时打红多个槽位。
 *   ② 🔴 阳性对照（`dheap_ubo_wrong.comp.slang`）：同 descriptor、同 heap 布局，
 *      shader 改读**错误成员/错误元素索引/错误字节偏移**，断言每槽位 **≠** 主判据值。
 *      排除的不只是"零初始化恰好相等"，还有"驱动把 UBO 读取整体平移"（scalar layout
 *      与 std140/std430 混淆的典型症状）——非零 magic 排得掉全零，排不掉置换。
 *   ③ 🔴 反向对照：UBO 槽**不写**描述符（槽字节留全零 = 地址 0 的 null descriptor），
 *      跑同一条路，断言**不会**恰好得到期望值。⚠️ 本条可能触发设备挂死/驱动报错，
 *      这是**结论的一部分**，不是测试缺陷；故单独成 TEST_CASE，可被
 *      `-tc="*反向对照*"` 单独隔离运行（跑法见文件末注释）。
 *
 * ## 变异红线声明（哪个变异让我红）
 *
 *   mapping 的 heapOffset 传成槽号而非字节（DESCRIPTOR_HEAP_SPEC 坑 5）→ ① 全槽红
 *   真描述符改放到 binding 号槽（②的诱饵位）→ ① 红 + 诱饵 out 被写 → 机制假说被区分
 *   shader 读 hdr.roughness 而非 exposure → ① 槽 0 红、② 槽 0 由"≠"变"="红
 *   UBO 不被驱动支持、静默读零 → ① 红（magic 全非零）、③ 仍绿（正确：读不到是真）
 *   out buffer 忘 barrier → VVL 当场打断（不靠运气）
 *
 * ## 本机实测读数（2026-10-12，绑机；重测 = `just test-gpu -tc="dheap 0*"`）
 *
 *   bufferDescriptorSize=16B  imageDescriptorSize=32B  samplerDescriptorSize=16B
 *   bufferDescriptorAlignment=4B  imageDescriptorAlignment=4B  resourceHeapAlignment=4B
 *   minResourceHeapReservedRange=0B  maxUniformBufferRange=4294967295B  maxPushDataSize=256B
 *   vkGetPhysicalDeviceDescriptorSizeEXT: UNIFORM_BUFFER=16B  STORAGE_BUFFER=16B
 *                                         SAMPLED_IMAGE=32B
 *   ⟹ **UBO 与 SSBO 在本驱动同宽（16B）同对齐（4B）**，heap 通路对 UBO **无额外对齐要求**；
 *     引擎统一步长 = max(buffer,image) = 32B ≥ 16B，写 UBO 描述符不需要改步长。
 *
 * ## 三臂结论（本机 = AMD Radeon 8060S Graphics / RADV STRIX_HALO / Mesa 26.2.3）
 *
 *   `ConstantBuffer<T>`（Uniform + Block，descriptor = eUniformBuffer）= **可用**
 *       主判据 11/11 槽位逐字节相等；反向对照撤掉 UBO 描述符后 UBO 专属 7 槽命中 0/7
 *       （证明描述符真被消费，不是"数据从别处来"）。
 *   `StructuredBuffer<T>`（退路臂，eStorageBuffer）= 可用（同一次 dispatch 内 4/4 槽正确）
 *   `ByteAddressBuffer`（对照臂，eStorageBuffer）  = 可用（2/2 槽正确）
 *   反向对照（UBO 槽字节留全零 = 地址 0 的 null descriptor）= **不挂死、不 device lost、
 *       无驱动报错**（VVL 也不报），读到的是零 → 主判据不会因此假绿。
 *
 * ## binding → heap 槽的真实机制（立项假设「binding 号 == 槽号」= **已证伪**）
 *
 * 真实机制 = `vkCreateShadersEXT` 期 `VkDescriptorSetAndBindingMappingEXT` 逐 binding
 * 烘焙 `sourceData.constantOffset.heapOffset`（**字节**）；槽号来自 mapping，与 binding 号
 * **无关**。引擎 `makeMappedCompute` 里 `heapOffset = slot * descriptorSize`，所以"数值上
 * 相等"只是引擎自己按槽号喂参数的**巧合**，不是驱动的规则。
 * 本文件的判据证据：真描述符放槽 5/11/13/7，同时在 binding 号对应的槽 0/1/2/3 写**诱饵**
 * 描述符 ⟹ 主判据逐字节正确 **且** 诱饵 out buffer 一个字节都没被写（`decoyOutTouched()`
 * 为 false）。若驱动真按 binding 号取描述符，这两条不可能同时成立。
 *
 * ## 两个踩过的坑（写在这里免得下一个人重踩）
 *
 *   ① `vkGetPhysicalDeviceDescriptorSizeEXT` 是 EXT 的 instance 级命令，loader **不导出**
 *      它的强符号 → 直接调 = 链接期 undefined reference。必须 `vkGetInstanceProcAddr` 取。
 *   ② 派发锚点（`kOutAnchor`）不是装饰：本 spike 首跑 11 槽全零，真根因是夹具里
 *      `GpuBuf{std::move(*made), p, made->getDeviceAddress(), …}` —— 花括号初始化列表
 *      从左到右求值，第一项已把 `*made` 移空，地址读成 0，VVL 报
 *      VUID-VkDeviceAddressRangeKHR-size-11411 ×8。没有锚点就会把它答成
 *      "RADV 不支持 heap 里的 UBO"（锚点缺失时测试直接 REQUIRE 红并写明"本次不构成
 *      对 UBO 能力的任何结论"）。
 *
 * ## 跑法
 *
 *   just test-gpu                          # 六条全跑（含反向对照）
 *   just test-gpu -tc="dheap 3*"           # 只跑可能挂死的那条（隔离归因）
 *   just test-gpu -tc="dheap 1*"           # 只跑 UBO 主判据
 *   just test-gpu -tc="*stride*"           # 只跑 stride 往返两条（dheap 4/5）
 *
 * 无设备时六条一律**响亮 FAIL**（Fixture 构造即抛，实测 `VK_DRIVER_FILES` 指向空 ICD
 * 清单 → `ErrorIncompatibleDriver` 记为 FAILURE，不 SKIP）。
 *
 * ## dheap 4/5 是另一件事：StructuredBuffer 元素 stride 的设备端往返
 *
 * 搭本文件的夹具便车，但回答的是 **R3 红线撤销条件第 2 条**（docs/constantbuffer_迁移计划.md
 * §8.4）：8B 与 **44B**（非 16 倍数的奇数 stride）元素经 `StructuredBuffer<T>` 读元素 1/2，
 * 期望值走闭式解 `kStrideWordBase + (e*sizeof + b)/4`（C++ 侧只认反射生成物的 `sizeof` /
 * `offsetof`，与 shader 侧的 `ArrayStride` 是两条独立路径）。判据、变异红线声明与
 * "为什么读元素 1 不读元素 0"见 `tests/shaders/dheap_stride.comp.slang` 头注释。
 *
 * 与构建期 R4（`tools/check_spirv_stride.py`，每次 `just build` 跑）的分工：R4 证**编译器
 * 口径**（探针 SPV 的 `ArrayStride` == 反射 `sizeof`），本用例证**驱动真按那个 stride 取数**。
 * 两条缺一条，就还剩一段只有截图能抓的静默空间。
 *
 * 实测有牙（2026-10-12 本机变异）：shader 镜像 `float3 center` 改成 `float4`（stride 44→48）
 * ⟹ dheap 4 的 8 个 stride-44 槽位全红、3 个 stride-8 槽位不动；把阳性对照体改回与主判据
 * 同下标 ⟹ dheap 5 恰好在被改的两个槽位红。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>      // vkGetPhysicalDeviceDescriptorSizeEXT（rlvk 只出类型不出 C 函数）
#include <vk_mem_alloc.h>       // VmaAllocationCreateInfo / VMA_* 宏（同 vkresources.cppm 的 GMF 形态）
#include <cstddef>
#include <cstring>
#include <format>
#include <fstream>
#include <string>
#include <vector>

export module test_descriptor_heap_ubo;

import rlstd;
import rlvk;
import vkerror;                 // Error（createDescriptorHeap 的 expected 错误分支）
import VmaRaii;                 // VmaAllocatorRAII / VmaDescriptorHeap（引擎 heap 封装）
import shader;                  // makeMappedCompute（引擎 binding→heap mapping 烘焙）
import meshlet_cull_types_generated;  // 反射生成物本体 = stride 往返用例的 C++ 侧真相
import test_device_primitives;  // Fixture（引擎同款 headless 引导）

namespace RGL::testing::dheap {

using testing::gpu::Fixture;

namespace {

/**
 * UBO 的 C++ 侧布局：与 `dheap_ubo.comp.slang` 的 `Header` 在 **scalar block layout**
 * 下逐字段对账。下方 static_assert 是本文件最重要的机器可验部分——SPIR-V 侧
 * `OpMemberDecorate ... Offset` 的实测数值（0/4/8/12/16/32/44）必须与这里一致，
 * 否则「读对了」与「读错了但恰好相等」无法区分。
 */
struct HeaderGpu {
    float exposure = 0.f;     ///< SPIR-V Offset 0
    float roughness = 0.f;    ///< SPIR-V Offset 4
    uint32_t flags = 0;       ///< SPIR-V Offset 8
    uint32_t count = 0;       ///< SPIR-V Offset 12
    float tint[4] = {};       ///< SPIR-V Offset 16（float4 → 16B 对齐）
    float dir[3] = {};        ///< SPIR-V Offset 32（float3 在 scalar layout 下仍占 12B）
    float scale = 0.f;        ///< SPIR-V Offset 44
};
static_assert(sizeof(HeaderGpu) == 48, "scalar layout 下 Header 总大小 = 48B");
static_assert(offsetof(HeaderGpu, exposure) == 0);
static_assert(offsetof(HeaderGpu, roughness) == 4);
static_assert(offsetof(HeaderGpu, flags) == 8);
static_assert(offsetof(HeaderGpu, count) == 12);
static_assert(offsetof(HeaderGpu, tint) == 16);
static_assert(offsetof(HeaderGpu, dir) == 32);
static_assert(offsetof(HeaderGpu, scale) == 44);

/** StructuredBuffer 元素：SPIR-V `ArrayStride 64`（4×float4，无 padding）。 */
struct MvpGpu {
    float c0[4] = {};
    float c1[4] = {};
    float c2[4] = {};
    float c3[4] = {};
};
static_assert(sizeof(MvpGpu) == 64, "SPIR-V ArrayStride = 64");

/** 输出槽位下标（= RWStructuredBuffer 的 uint 下标），与两份 .slang 的头注释一一对应。 */
enum OutSlot : uint32_t {
    kOutExposure = 0,
    kOutRoughness = 1,
    kOutFlags = 2,
    kOutCount = 3,
    kOutTintY = 4,
    kOutDirZ = 5,
    kOutScale = 6,
    kOutMvps1C2X = 7,
    kOutRaw4 = 8,
    kOutMvps0C0X = 9,
    kOutRaw0 = 10,
    /**
     * 派发锚点槽（**不**来自任何描述符读，shader 写立即数 0xBEEFCAFE）。
     * 它把「读回全零」拆成两种互斥解释：通路没跑（锚点也零）vs 通路跑了但读侧
     * 描述符没被消费（锚点在、0..10 零）。缺这条时 spike 会把前者误读成后者，
     * 即把"我的测试没搭对"答成"驱动不支持 UBO"。
     */
    kOutAnchor = 11,
    kOutAnchorValue = 0xBEEFCAFEu,
    kOutCount32 = 16,  ///< out buffer 的 uint 个数（12..15 未写，必须恒 0）
};

/**
 * 11 个 magic：**互不相同且全非零**。
 *
 * 为什么这么挑：主判据的假绿形态有两种——「读回零初始化值」与「整体错位一格」。
 * 全非零排掉前者；两两不同 + 逐槽位单独断言排掉后者（置换后至少一个槽位对不上）。
 * float 成员写成位模式（`asuint` 语义），避免"1.0f 的位模式恰好是常见初值"这类巧合。
 */
constexpr uint32_t kMagicExposure = 0x41F00000u;  ///< 30.0f
constexpr uint32_t kMagicRoughness = 0x3DCCCCCDu;  ///< 0.1f
constexpr uint32_t kMagicFlags = 0xDEADBEEFu;
constexpr uint32_t kMagicCount = 0x00001234u;
constexpr uint32_t kMagicTintY = 0x3F000000u;  ///< 0.5f
constexpr uint32_t kMagicDirZ = 0xBF800000u;   ///< −1.0f
constexpr uint32_t kMagicScale = 0x40400000u;  ///< 3.0f
constexpr uint32_t kMagicMvps1C2X = 0x12345678u;
constexpr uint32_t kMagicRaw4 = 0xABCDEF01u;
constexpr uint32_t kMagicMvps0C0X = 0x0F0F0F0Fu;
constexpr uint32_t kMagicRaw0 = 0xF0F0F0F0u;

/** 诱饵 buffer 的填充字节（与全部真 magic 两两不同）：机制判据的"另一组值"。 */
constexpr uint32_t kDecoyFill = 0x5A5A5A5Au;

/** 主判据期望表（下标 = OutSlot）。 */
constexpr std::array<uint32_t, kOutCount32> expectedMain() {
    std::array<uint32_t, kOutCount32> e{};
    e[kOutExposure] = kMagicExposure;
    e[kOutRoughness] = kMagicRoughness;
    e[kOutFlags] = kMagicFlags;
    e[kOutCount] = kMagicCount;
    e[kOutTintY] = kMagicTintY;
    e[kOutDirZ] = kMagicDirZ;
    e[kOutScale] = kMagicScale;
    e[kOutMvps1C2X] = kMagicMvps1C2X;
    e[kOutRaw4] = kMagicRaw4;
    e[kOutMvps0C0X] = kMagicMvps0C0X;
    e[kOutRaw0] = kMagicRaw0;
    return e;
}

/** 位转：把 uint 位模式塞进 float 字段（不改变字节）。 */
inline float bitsToFloat(uint32_t bits) {
    float f = 0.f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

/**
 * 一条 heap 描述符的写入（照抄 `vkresources.cppm:181-189` 的机制，不自己发明）。
 * @param heap resource heap（引擎封装）
 * @param device 设备句柄
 * @param slot 目标槽号
 * @param type 描述符类型（本 spike 的主角 = eUniformBuffer）
 * @param addr 目标 buffer 的 device address
 * @param size 范围长度
 */
void writeDesc(const RGL::VmaDescriptorHeap& heap, vk::raii::Device& device, uint32_t slot,
    vk::DescriptorType type, uint64_t addr, uint64_t size) {
    vk::DeviceAddressRangeEXT range{addr, size};
    vk::ResourceDescriptorInfoEXT desc{};
    desc.type = type;
    desc.data.pAddressRange = &range;
    auto slotAddr = heap.getSlotCpuAddress(slot);
    REQUIRE(slotAddr.has_value());
    device.writeResourceDescriptorsEXT(desc,
        vk::HostAddressRangeEXT{*slotAddr, heap.getDescriptorSize()});
}

/** 一块 host 可写 + BDA 的 VMA buffer（描述符指向它）。 */
struct GpuBuf {
    RGL::VmaBuffer buf;
    void* mapped = nullptr;
    uint64_t address = 0;
    uint64_t size = 0;
};

/**
 * 建一块上述 buffer 并填初值（内容未定义时传 nullptr = 全零）。
 *
 * usage 同时给 `eUniformBuffer | eStorageBuffer`：本 spike 拿同一块 buffer 分别当 UBO 与
 * SSBO 描述符的目标，两个 usage 都给 ⟹ 排除「描述符类型对，但 buffer 建的时候没声明对应
 * usage」这一层混淆（那是另一个问题，会污染本次结论）。
 */
GpuBuf makeBuf(const RGL::VmaAllocatorRAII& alloc, vk::raii::Device& device, uint64_t bytes,
    const void* src) {
    vk::BufferCreateInfo bi{};
    bi.size = bytes;
    bi.usage = vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer
        | vk::BufferUsageFlagBits::eShaderDeviceAddress | vk::BufferUsageFlagBits::eTransferDst;
    bi.sharingMode = vk::SharingMode::eExclusive;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo ad{};
    auto made = alloc.createBufferWithAddress(device, bi, ai, &ad);
    REQUIRE(made.has_value());
    auto* p = static_cast<std::byte*>(ad.pMappedData);
    std::memset(p, 0, bytes);  // 显式清零：不靠"新页恰好是零"
    if (src)
        std::memcpy(p, src, bytes);
    // ⚠️ 地址必须**在 move 之前**取：花括号初始化列表从左到右求值，若把
    // `made->getDeviceAddress()` 直接写进 `GpuBuf{std::move(*made), ...}`，第一项就把
    // `*made` 移空，第二项读到的是**已移空对象**的 0 —— 描述符地址全 0，
    // VVL 报 VUID-VkDeviceAddressRangeKHR-size-11411 ×8，dispatch 静默不写。
    // （本 spike 首跑全零红的真实根因；派发锚点一条 REQUIRE 把它从"驱动不支持 UBO"
    //   的错误结论里区分出来。）
    const uint64_t addr = made->getDeviceAddress();
    return GpuBuf{std::move(*made), p, addr, bytes};
}

/**
 * spike 夹具：heap + 四块真 buffer + 一块诱饵 buffer + 全部 magic。
 *
 * 生命周期：`fx` 必须比本对象**先构造**（本对象持其引用并在构造期用其设备）。
 * 析构序自动反序（heap/buffer 先于 allocator 与 fx 销毁）。
 */
class Harness {
  public:
    /** heap 步长/保留区/容量与四块真 buffer 的创建 + 填 magic。 */
    explicit Harness(const Fixture& fx);

    /**
     * 写全部描述符。
     * @param withUbo false = **反向对照**：UBO 槽一个字节都不写（保持夹具构造期的全零）
     * @param decoys true = 同时在 binding 号对应的槽写诱饵（机制判据，见文件头）
     */
    void writeAllDescriptors(bool withUbo, bool decoys);

    /**
     * 建 shader object（引擎 makeMappedCompute）→ 绑 heap → dispatch → 等完成。
     * @param spvPath 探针 SPV 绝对路径宏展开后的字符串
     * @param entryName 入口名（本 spike 恒 "main"）
     */
    void runDispatch(const std::string& spvPath, const std::string& entryName = "main");

    /** @return 真 out buffer 的 16 个 uint（读回） */
    std::array<uint32_t, kOutCount32> readOut() const;
    /**
     * @return 诱饵 out buffer 是否被写过
     *
     * 机制假说的诊断面：若驱动把 binding 号当 heap 槽号（即 mapping 的 heapOffset
     * 不被消费），binding 3 会取到槽 reserved+3 的描述符 = 本 buffer，于是它被写满
     * 0x5A 图案。真 out buffer 全零 + 本函数 true = 「binding == 槽」假说成立；
     * 真 out 逐字节正确 + 本函数 false = 「mapping 的 heapOffset 决定槽」成立。
     */
    bool decoyOutTouched() const;

    /** 把 heap 属性与逐类型描述符尺寸打成 MESSAGE（报告要求"抄实际数值"）。 */
    void reportHeapProperties() const;

    [[nodiscard]] uint32_t slotHdr() const { return slotHdr_; }
    [[nodiscard]] uint32_t slotMvps() const { return slotMvps_; }
    [[nodiscard]] uint32_t slotRaw() const { return slotRaw_; }
    [[nodiscard]] uint32_t slotOut() const { return slotOut_; }
    [[nodiscard]] uint32_t reservedSlots() const { return heap_.getReservedSlots(); }
    [[nodiscard]] uint32_t descriptorSize() const { return heap_.getDescriptorSize(); }

  private:
    const Fixture& fx_;
    RGL::VmaAllocatorRAII alloc_;
    RGL::VmaDescriptorHeap heap_;
    GpuBuf ubo_;
    GpuBuf mvps_;
    GpuBuf raw_;
    GpuBuf out_;
    GpuBuf decoySrc_;   ///< 诱饵**源**（整块 0x5A）：喂 binding 0/1/2 的槽 reserved+0/1/2
    GpuBuf decoyOut_;   ///< 诱饵**输出**（清零）：喂 binding 3 的槽 reserved+3
    uint32_t slotHdr_ = 0;
    uint32_t slotMvps_ = 0;
    uint32_t slotRaw_ = 0;
    uint32_t slotOut_ = 0;
    vk::DeviceSize reservedRangeSize_ = 0;
};

Harness::Harness(const Fixture& fx)
    : fx_(fx), alloc_(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion()) {
    // ── heap 属性（与引擎 Resources::createDescriptorHeap 同口径）──────────────
    auto props = fx.physical().getProperties2<vk::PhysicalDeviceProperties2,
        vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    auto& hp = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    const uint32_t descriptorSize = static_cast<uint32_t>(
        std::max(vk::DeviceSize(hp.bufferDescriptorSize), vk::DeviceSize(hp.imageDescriptorSize)));
    const uint32_t reservedSlots = static_cast<uint32_t>(
        (hp.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize);
    // 32 个用户槽够用：真槽 4 + 诱饵槽 4，且要能放到"绝不等于 binding 号"的位置
    const uint32_t capacity = reservedSlots + 32;
    auto made = alloc_.createDescriptorHeap(fx.device(), capacity, descriptorSize,
        vk::BufferUsageFlagBits::eDescriptorHeapEXT, reservedSlots, hp.resourceHeapAlignment);
    REQUIRE(made.has_value());
    heap_ = std::move(*made);
    reservedRangeSize_ = static_cast<vk::DeviceSize>(reservedSlots) * descriptorSize;

    // heap 内容整体清零：反向对照的「不写」必须是**确定的全零**，而不是
    // "VMA 新页恰好是零"（换分配器复用脏页就翻车 —— izb_shadow 的 mask 清零同款教训）。
    std::memset(heap_.pMappedData, 0, static_cast<size_t>(heap_.getHeapSize()));

    // ── 真 buffer 填 magic ──────────────────────────────────────────────────
    HeaderGpu h{};
    h.exposure = bitsToFloat(kMagicExposure);
    h.roughness = bitsToFloat(kMagicRoughness);
    h.flags = kMagicFlags;
    h.count = kMagicCount;
    h.tint[0] = bitsToFloat(0x3F800000u);  // 1.0f（未被读，占位）
    h.tint[1] = bitsToFloat(kMagicTintY);
    h.tint[2] = bitsToFloat(0x3E4CCCCDu);  // 0.2f（阳性对照读它）
    h.tint[3] = bitsToFloat(0x3DCCCCCDu);  // 0.1f（阳性对照读它）
    h.dir[0] = bitsToFloat(0x3F800000u);
    h.dir[1] = bitsToFloat(0x3F800000u);
    h.dir[2] = bitsToFloat(kMagicDirZ);
    h.scale = bitsToFloat(kMagicScale);
    ubo_ = makeBuf(alloc_, fx.device(), 64, &h);  // Header 48B → 64B（对齐裕量）

    // mvps：3 个元素（阳性对照读 index 2，故必须真存在且越界不成立）
    std::vector<MvpGpu> mv(3);
    mv[0].c0[0] = bitsToFloat(kMagicMvps0C0X);
    mv[1].c2[0] = bitsToFloat(kMagicMvps1C2X);
    mv[1].c0[0] = bitsToFloat(0x22222222u);  // 阳性对照读 mvps[1].c0.x → 必须 ≠ 真值
    mv[2].c2[0] = bitsToFloat(0x33333333u);  // 阳性对照读 mvps[2].c2.x → 必须 ≠ 真值
    mvps_ = makeBuf(alloc_, fx.device(), sizeof(MvpGpu) * mv.size(), mv.data());

    // raw：ByteAddressBuffer，偏移 = 字节偏移
    std::array<uint32_t, 16> rawInit{};
    rawInit[0] = kMagicRaw0;
    rawInit[1] = kMagicRaw4;   // 字节偏移 4
    rawInit[2] = 0x76543210u;  // 字节偏移 8（阳性对照读它）
    raw_ = makeBuf(alloc_, fx.device(), sizeof(rawInit), rawInit.data());

    out_ = makeBuf(alloc_, fx.device(), sizeof(uint32_t) * kOutCount32, nullptr);
    // 诱饵**源**整块 0x5A（binding 0/1/2 若被当成槽号，读到的就是它 → 输出与真 magic 全不同）；
    // 诱饵**输出**保持清零（decoyOutTouched 判据要求"没被写过"的初值确定）。
    decoySrc_ = makeBuf(alloc_, fx.device(), 256, nullptr);
    std::memset(decoySrc_.mapped, static_cast<int>(kDecoyFill & 0xFFu),
        static_cast<size_t>(decoySrc_.size));
    decoyOut_ = makeBuf(alloc_, fx.device(), sizeof(uint32_t) * kOutCount32, nullptr);

    // ── 真槽号：全部 ≠ 任何 binding 号（0..3），机制判据的构造前提 ─────────────
    slotHdr_ = reservedSlots + 5;
    slotMvps_ = reservedSlots + 11;
    slotRaw_ = reservedSlots + 13;
    slotOut_ = reservedSlots + 7;
    REQUIRE(slotHdr_ > 3 + reservedSlots);
    REQUIRE(slotMvps_ > 3 + reservedSlots);
    REQUIRE(slotRaw_ > 3 + reservedSlots);
    REQUIRE(slotOut_ > 3 + reservedSlots);
}

void Harness::writeAllDescriptors(bool withUbo, bool decoys) {
    auto& dev = fx_.device();
    if (withUbo)
        writeDesc(heap_, dev, slotHdr_, vk::DescriptorType::eUniformBuffer, ubo_.address, ubo_.size);
    writeDesc(heap_, dev, slotMvps_, vk::DescriptorType::eStorageBuffer, mvps_.address, mvps_.size);
    writeDesc(heap_, dev, slotRaw_, vk::DescriptorType::eStorageBuffer, raw_.address, raw_.size);
    writeDesc(heap_, dev, slotOut_, vk::DescriptorType::eStorageBuffer, out_.address, out_.size);
    if (!decoys)
        return;
    // 诱饵写在 **binding 号**对应的槽（reserved + 0/1/2/3）。binding 0 是 Uniform 类
    // （ConstantBuffer），其余 storage 类。若驱动按「binding == 槽」取描述符，三个读
    // 描述符落到 0x5A buffer、写描述符落到 decoyOut_ ⟹ 与真路径的可观测结果互斥。
    const uint32_t base = heap_.getReservedSlots();
    writeDesc(heap_, dev, base + 0, vk::DescriptorType::eUniformBuffer, decoySrc_.address, decoySrc_.size);
    writeDesc(heap_, dev, base + 1, vk::DescriptorType::eStorageBuffer, decoySrc_.address, decoySrc_.size);
    writeDesc(heap_, dev, base + 2, vk::DescriptorType::eStorageBuffer, decoySrc_.address, decoySrc_.size);
    writeDesc(heap_, dev, base + 3, vk::DescriptorType::eStorageBuffer, decoyOut_.address, decoyOut_.size);
}

void Harness::runDispatch(const std::string& spvPath, const std::string& entryName) {
    // 引擎的 mapping 烘焙：binding → 真槽号（**不是** binding == 槽）
    const std::array<std::pair<uint32_t, uint32_t>, 4> bindingToSlot{{
        {0, slotHdr_},
        {1, slotMvps_},
        {2, slotRaw_},
        {3, slotOut_},
    }};
    auto shaderObj = RGL::makeMappedCompute(fx_.device(), spvPath,
        std::span<const std::pair<uint32_t, uint32_t>>(bindingToSlot), descriptorSize());

    fx_.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        // host 写（描述符 + buffer 内容）→ GPU 读。dst 必须含 eResourceHeapReadEXT：
        // 漏它 = 描述符堆的读侧没排序（heap_texture_research 的"采样恒 garbage"先例）。
        // ⚠️ dstAccessMask 里**不能**放 eTransferRead：dstStageMask 只有 eComputeShader，
        // VVL 实测报 "VK_ACCESS_2_TRANSFER_READ_BIT is not supported by stage mask"。
        vk::MemoryBarrier2 pre{};
        pre.srcStageMask = vk::PipelineStageFlagBits2::eHost;
        pre.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
        pre.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        pre.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT
            | vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        vk::DependencyInfo dep0{};
        dep0.setMemoryBarriers(pre);
        cmd.pipelineBarrier2(dep0);

        vk::DeviceAddressRangeEXT heapRange{heap_.getGpuAddress(), heap_.getHeapSize()};
        cmd.bindResourceHeapEXT(
            vk::BindHeapInfoEXT(heapRange, 0, reservedRangeSize_));

        std::array<vk::ShaderStageFlagBits, 1> stages{vk::ShaderStageFlagBits::eCompute};
        std::array<vk::ShaderEXT, 1> handles{*shaderObj};
        cmd.bindShadersEXT(stages, handles);
        cmd.dispatch(1, 1, 1);

        vk::MemoryBarrier2 post{};
        post.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        post.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
        post.dstStageMask = vk::PipelineStageFlagBits2::eHost;
        post.dstAccessMask = vk::AccessFlagBits2::eHostRead;
        vk::DependencyInfo dep1{};
        dep1.setMemoryBarriers(post);
        cmd.pipelineBarrier2(dep1);
    });
    (void)entryName;  // makeMappedCompute 固定 pName="main"；两份 SPV 各一个 main
}

std::array<uint32_t, kOutCount32> Harness::readOut() const {
    std::array<uint32_t, kOutCount32> got{};
    std::memcpy(got.data(), out_.mapped, sizeof(got));
    return got;
}

bool Harness::decoyOutTouched() const {
    const auto* w = static_cast<const uint32_t*>(decoyOut_.mapped);
    for (uint32_t i = 0; i < kOutCount32; ++i)
        if (w[i] != 0u)
            return true;
    return false;
}

void Harness::reportHeapProperties() const {
    auto props = fx_.physical().getProperties2<vk::PhysicalDeviceProperties2,
        vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    auto& hp = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    // ⚠️ 必须经 vkGetInstanceProcAddr 取指针：`vkGetPhysicalDeviceDescriptorSizeEXT` 是
    // EXT 的 instance 级命令，loader **不导出**它的强符号（直接调 = 链接期
    // undefined reference，实测踩过）。只有 core 命令与已提升命令才有导出符号。
    const VkInstance instance = *fx_.instance();
    const VkPhysicalDevice pd = *fx_.physical();
    const auto querySize = [instance, pd](VkDescriptorType type) -> uint64_t {
        const auto pfn = reinterpret_cast<PFN_vkGetPhysicalDeviceDescriptorSizeEXT>(
            ::vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceDescriptorSizeEXT"));
        // 取不到 = 返回 0，下面 CHECK(szUbo == szSsbo) 与读数 MESSAGE 一起暴露
        // （"0" 会被读成读数，不出声的失败不算读数）
        return pfn ? static_cast<uint64_t>(pfn(pd, type)) : 0;
    };
    const auto szUbo = querySize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    const auto szSsbo = querySize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    const auto szSampled = querySize(VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
    MESSAGE("heap 属性（本机 " << fx_.deviceName() << "）: bufferDescriptorSize="
        << hp.bufferDescriptorSize << "B imageDescriptorSize=" << hp.imageDescriptorSize
        << "B samplerDescriptorSize=" << hp.samplerDescriptorSize
        << "B bufferDescriptorAlignment=" << hp.bufferDescriptorAlignment
        << "B imageDescriptorAlignment=" << hp.imageDescriptorAlignment
        << "B resourceHeapAlignment=" << hp.resourceHeapAlignment
        << "B minResourceHeapReservedRange=" << hp.minResourceHeapReservedRange
        << "B maxUniformBufferRange=" << fx_.physical().getProperties().limits.maxUniformBufferRange
        << "B maxPushDataSize=" << hp.maxPushDataSize << "B");
    MESSAGE("vkGetPhysicalDeviceDescriptorSizeEXT: UNIFORM_BUFFER=" << szUbo
        << "B STORAGE_BUFFER=" << szSsbo << "B SAMPLED_IMAGE=" << szSampled
        << "B ⟹ 实际 heap 步长=" << descriptorSize()
        << "B reservedSlots=" << reservedSlots());
    // UBO 与 SSBO 同宽 ⟹ heap 通路对 UBO 无额外对齐要求（判据本体，不只是 dump）
    CHECK(szUbo == szSsbo);
    CHECK(szUbo <= descriptorSize());
}

// ─── StructuredBuffer 元素 stride 设备端往返（R3 撤销条件第 2 条，§8.4）──────

/**
 * 填充图案基址：第 k 个字（字节 4k）= `kStrideWordBase + k`。
 *
 * 为什么是这个形态：期望值有**闭式解**（见 `strideWord`），且字与字两两不同、全非零
 * ——「整体错位一格」会同时打红所有槽位，"读到零初始化"被基址排掉。
 */
constexpr uint32_t kStrideWordBase = 0x5A000000u;

/**
 * 闭式期望：元素 `elem` 的字节偏移 `byteOffset` 处**该**读到什么。
 *
 * 这就是"独立第二实现"：C++ 侧只认 `sizeof` / `offsetof`（反射生成物），shader 侧只认
 * `ArrayStride`（编译器），两条路径只在"stride 相同"这一点上相交。stride 一漂，
 * 所有槽位同时打红（不是"某一位不同"）。
 */
constexpr uint32_t strideWord(uint32_t elem, uint32_t stride, uint32_t byteOffset) {
    return kStrideWordBase + (elem * stride + byteOffset) / 4u;
}

/** 输出槽位（编号 = 两份 dheap_stride*.comp.slang 头注释里的表）。 */
enum StrideOutSlot : uint32_t {
    kStrideOutItem1A = 0,      ///< it[1].clusterIdx（字节 8）
    kStrideOutItem1B = 1,      ///< it[1].objectSlot（字节 12）
    kStrideOutItem2A = 2,      ///< it[2].clusterIdx（字节 16 = 2×stride）
    kStrideOutHdr1Group = 3,   ///< h[1].group（字节 44 —— **第一次用到 stride**）
    kStrideOutHdr1Remap = 4,   ///< h[1].remapOffset（字节 64）
    kStrideOutHdr1Cx = 5,      ///< h[1].center.x（字节 68）
    kStrideOutHdr1Cy = 6,      ///< h[1].center.y（字节 72）
    kStrideOutHdr1Cz = 7,      ///< h[1].center.z（字节 76）
    kStrideOutHdr1Radius = 8,  ///< h[1].radius（字节 80 —— float3 只占 12B 才落在这）
    kStrideOutHdr1Error = 9,   ///< h[1].error（字节 84）
    kStrideOutHdr2Group = 10,  ///< h[2].group（字节 88 = 2×44）
    kStrideOutLast = kStrideOutHdr2Group,
    kStrideOutAnchor = 11,      ///< 派发锚点槽（不来自任何描述符读）
    kStrideOutAnchorValue = 0xBEEFCAFEu,
    kStrideOutCount = 16,       ///< out buffer 的 uint 个数（12..15 未写，必须恒 0）
};

/**
 * 主判据期望表（下标 = StrideOutSlot 的 0..10）。
 *
 * 🔴 表里**没有任何手抄的字节偏移**：stride 取 `sizeof(生成物类型)`、成员偏移取
 * `offsetof(生成物类型, …)`。契约改了这张表自动跟着走，而 shader 侧不跟 ⟹ 用例红。
 */
inline std::array<uint32_t, kStrideOutLast + 1> strideExpectedMain() {
    constexpr uint32_t kItem = sizeof(RGL::MeshletCullItem);
    constexpr uint32_t kHeader = sizeof(RGL::MeshletClusterHeader);
    constexpr uint32_t kCenter = offsetof(RGL::MeshletClusterHeader, center);
    return {{
        strideWord(1, kItem, offsetof(RGL::MeshletCullItem, clusterIdx)),
        strideWord(1, kItem, offsetof(RGL::MeshletCullItem, objectSlot)),
        strideWord(2, kItem, offsetof(RGL::MeshletCullItem, clusterIdx)),
        strideWord(1, kHeader, offsetof(RGL::MeshletClusterHeader, group)),
        strideWord(1, kHeader, offsetof(RGL::MeshletClusterHeader, remapOffset)),
        strideWord(1, kHeader, kCenter + 0),
        strideWord(1, kHeader, kCenter + 4),
        strideWord(1, kHeader, kCenter + 8),
        strideWord(1, kHeader, offsetof(RGL::MeshletClusterHeader, radius)),
        strideWord(1, kHeader, offsetof(RGL::MeshletClusterHeader, error)),
        strideWord(2, kHeader, offsetof(RGL::MeshletClusterHeader, group)),
    }};
}

/** 第 k 个字 = `kStrideWordBase + k` 的填充（"字两两不同"是闭式期望成立的前提）。 */
inline std::vector<uint32_t> strideFillWords(size_t words) {
    std::vector<uint32_t> w(words);
    for (size_t k = 0; k < words; ++k)
        w[k] = kStrideWordBase + static_cast<uint32_t>(k);
    return w;
}

/**
 * stride 往返夹具：两块 `StructuredBuffer` + out，全部走引擎 descriptor heap 通路。
 *
 * 元素类型的 C++ 侧**直接用反射生成物本体**（`RGL::MeshletCullItem` /
 * `RGL::MeshletClusterHeader`），不自造镜像 struct ⟹ 本用例量的就是"实际在用的 stride"，
 * 而不是某个恰好长得像的 struct。shader 侧是镜像（tests/shaders 的 compile_slang 只 -I
 * 自己目录，import 不到引擎契约文件），两侧漂移正是本用例要抓的东西。
 *
 * 与 `Harness` 分开成两个夹具（而不是扩它）：`Harness` 的槽位布局是「binding→槽机制
 * 证伪」判据的一部分（真槽 ≠ binding 号 + 诱饵槽 = binding 号），加槽会动那四条用例的
 * 构造前提。本用例不重复验机制，只验 stride。
 */
class StrideHarness {
  public:
    /** 3 个元素：元素 1 首次用到 stride，元素 2 用到两次（把"步长"与"首元素偏移"分开）。 */
    static constexpr uint32_t kElements = 3;

    explicit StrideHarness(const Fixture& fx);
    void writeDescriptors();
    void runDispatch(const std::string& spvPath);
    [[nodiscard]] std::array<uint32_t, kStrideOutCount> readOut() const;

  private:
    const Fixture& fx_;
    RGL::VmaAllocatorRAII alloc_;
    RGL::VmaDescriptorHeap heap_;
    GpuBuf items_;
    GpuBuf headers_;
    GpuBuf out_;
    uint32_t slotItems_ = 0;
    uint32_t slotHeaders_ = 0;
    uint32_t slotOut_ = 0;
    vk::DeviceSize reservedRangeSize_ = 0;
};

StrideHarness::StrideHarness(const Fixture& fx)
    : fx_(fx), alloc_(*fx.instance(), *fx.physical(), *fx.device(), fx.apiVersion()) {
    auto props = fx.physical().getProperties2<vk::PhysicalDeviceProperties2,
        vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    auto& hp = props.get<vk::PhysicalDeviceDescriptorHeapPropertiesEXT>();
    const uint32_t descriptorSize = static_cast<uint32_t>(
        std::max(vk::DeviceSize(hp.bufferDescriptorSize), vk::DeviceSize(hp.imageDescriptorSize)));
    const uint32_t reservedSlots = static_cast<uint32_t>(
        (hp.minResourceHeapReservedRange + descriptorSize - 1) / descriptorSize);
    auto made = alloc_.createDescriptorHeap(fx.device(), reservedSlots + 16, descriptorSize,
        vk::BufferUsageFlagBits::eDescriptorHeapEXT, reservedSlots, hp.resourceHeapAlignment);
    REQUIRE(made.has_value());
    heap_ = std::move(*made);
    reservedRangeSize_ = static_cast<vk::DeviceSize>(reservedSlots) * descriptorSize;
    std::memset(heap_.pMappedData, 0, static_cast<size_t>(heap_.getHeapSize()));

    // 两块 buffer 的大小 = 元素数 × sizeof(生成物类型)，**不向上取整到 16**：
    // 44×3 = 132B 就是 132B。取整等于替驱动把越界读藏起来，本用例要的是"按 stride 数数"。
    const auto itemWords = strideFillWords(kElements * sizeof(RGL::MeshletCullItem) / 4);
    const auto headerWords = strideFillWords(kElements * sizeof(RGL::MeshletClusterHeader) / 4);
    items_ = makeBuf(alloc_, fx.device(), itemWords.size() * 4, itemWords.data());
    headers_ = makeBuf(alloc_, fx.device(), headerWords.size() * 4, headerWords.data());
    out_ = makeBuf(alloc_, fx.device(), sizeof(uint32_t) * kStrideOutCount, nullptr);

    // 槽号 ≠ binding 号（0/1/2）：与 Harness 同纪律，机制虽已由 dheap 1 钉死，
    // 但沿用同一形态可排除"这条用例恰好踩到 binding==槽的巧合"这一层混淆。
    slotItems_ = reservedSlots + 5;
    slotHeaders_ = reservedSlots + 11;
    slotOut_ = reservedSlots + 7;
}

void StrideHarness::writeDescriptors() {
    auto& dev = fx_.device();
    writeDesc(heap_, dev, slotItems_, vk::DescriptorType::eStorageBuffer, items_.address,
        items_.size);
    writeDesc(heap_, dev, slotHeaders_, vk::DescriptorType::eStorageBuffer, headers_.address,
        headers_.size);
    writeDesc(heap_, dev, slotOut_, vk::DescriptorType::eStorageBuffer, out_.address, out_.size);
}

void StrideHarness::runDispatch(const std::string& spvPath) {
    const std::array<std::pair<uint32_t, uint32_t>, 3> bindingToSlot{{
        {0, slotItems_},
        {1, slotHeaders_},
        {2, slotOut_},
    }};
    auto shaderObj = RGL::makeMappedCompute(fx_.device(), spvPath,
        std::span<const std::pair<uint32_t, uint32_t>>(bindingToSlot), heap_.getDescriptorSize());

    fx_.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        // host 写（描述符 + buffer 内容）→ GPU 读。dst 必须含 eResourceHeapReadEXT：
        // 漏它 = 描述符堆的读侧没排序（先例见 Harness::runDispatch 注释）。
        vk::MemoryBarrier2 pre{};
        pre.srcStageMask = vk::PipelineStageFlagBits2::eHost;
        pre.srcAccessMask = vk::AccessFlagBits2::eHostWrite;
        pre.dstStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        pre.dstAccessMask = vk::AccessFlagBits2::eResourceHeapReadEXT
            | vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite;
        vk::DependencyInfo dep0{};
        dep0.setMemoryBarriers(pre);
        cmd.pipelineBarrier2(dep0);

        vk::DeviceAddressRangeEXT heapRange{heap_.getGpuAddress(), heap_.getHeapSize()};
        cmd.bindResourceHeapEXT(vk::BindHeapInfoEXT(heapRange, 0, reservedRangeSize_));

        std::array<vk::ShaderStageFlagBits, 1> stages{vk::ShaderStageFlagBits::eCompute};
        std::array<vk::ShaderEXT, 1> handles{*shaderObj};
        cmd.bindShadersEXT(stages, handles);
        cmd.dispatch(1, 1, 1);

        vk::MemoryBarrier2 post{};
        post.srcStageMask = vk::PipelineStageFlagBits2::eComputeShader;
        post.srcAccessMask = vk::AccessFlagBits2::eShaderWrite;
        post.dstStageMask = vk::PipelineStageFlagBits2::eHost;
        post.dstAccessMask = vk::AccessFlagBits2::eHostRead;
        vk::DependencyInfo dep1{};
        dep1.setMemoryBarriers(post);
        cmd.pipelineBarrier2(dep1);
    });
}

std::array<uint32_t, kStrideOutCount> StrideHarness::readOut() const {
    std::array<uint32_t, kStrideOutCount> got{};
    std::memcpy(got.data(), out_.mapped, sizeof(got));
    return got;
}

}  // namespace

}  // namespace RGL::testing::dheap

// ═══════════════════════════════════════════════════════════════════════════
// 用例
// ═══════════════════════════════════════════════════════════════════════════

TEST_SUITE("gpu") {

using RGL::testing::dheap::expectedMain;
using RGL::testing::dheap::kOutAnchor;
using RGL::testing::dheap::kOutAnchorValue;
using RGL::testing::dheap::kOutRaw0;
using RGL::testing::dheap::kStrideOutAnchor;
using RGL::testing::dheap::kStrideOutAnchorValue;
using RGL::testing::dheap::kStrideOutLast;
using RGL::testing::dheap::strideExpectedMain;
using RGL::testing::dheap::Harness;
using RGL::testing::dheap::StrideHarness;
using RGL::testing::gpu::Fixture;

TEST_CASE("dheap 0：heap 属性与逐类型描述符尺寸读数（UBO 有无专属对齐要求）") {
    const Fixture fx;
    const Harness h(fx);
    h.reportHeapProperties();
}

TEST_CASE("dheap 1：主判据 —— ConstantBuffer<T> 经 resource heap 逐字节读回 + 两臂") {
    const Fixture fx;
    Harness h(fx);
    // 诱饵一起写：主判据同时担保「取的是 mapping 的 heapOffset，不是 binding 号」
    h.writeAllDescriptors(/*withUbo=*/true, /*decoys=*/true);
    h.runDispatch(RGL_TEST_SHADER_DIR "/dheap_ubo.comp.spv");

    const auto got = h.readOut();
    const auto want = expectedMain();
    // 派发锚点先判（**FATAL**）：锚点没写进来 = dispatch / out 描述符根本没生效，
    // 下面所有"读回零"都归因于通路而非 UBO，继续判会把工具故障答成驱动结论。
    REQUIRE_MESSAGE(got[kOutAnchor] == kOutAnchorValue,
        "派发锚点缺失（got=" << got[kOutAnchor] << "）⟹ dispatch/out 描述符未生效，"
        "本次不构成对 UBO 能力的任何结论");
    for (uint32_t i = 0; i <= kOutRaw0; ++i) {
        INFO("out slot " << i << " on " << fx.deviceName());
        CHECK(got[i] == want[i]);
    }
    // 未使用槽位必须恒 0：若驱动把 binding 号当槽号、out 被写到别处，这里不会红，
    // 但 decoyOutTouched 会红 —— 两条一起构成「写到了唯一那块 out buffer」。
    for (uint32_t i = kOutAnchor + 1; i < want.size(); ++i)
        CHECK(got[i] == 0u);
    // 机制判据的反向半边：诱饵 out 一个字节都不许被写
    CHECK_FALSE(h.decoyOutTouched());
}

TEST_CASE("dheap 2：阳性对照 —— 读错误成员/错误索引必须 ≠ 主判据值") {
    const Fixture fx;
    Harness h(fx);
    h.writeAllDescriptors(/*withUbo=*/true, /*decoys=*/false);
    h.runDispatch(RGL_TEST_SHADER_DIR "/dheap_ubo_wrong.comp.spv");

    const auto got = h.readOut();
    const auto want = expectedMain();
    // 同主判据：先钉"这个 shader 确实跑了并写进了 out"，否则"≠ 主判据值"
    // 可能只是"什么都没写"造成的假阳性对照。
    REQUIRE_MESSAGE(got[kOutAnchor] == kOutAnchorValue,
        "阳性对照体的派发锚点缺失（got=" << got[kOutAnchor] << "）⟹ 对照不成立");
    // 断言方向 = **不等于**。缺这条时，「主判据绿」无法排除两种假绿：
    //   ① 读回的是零初始化值而 magic 恰好为 0（本 spike magic 全非零，已排）；
    //   ② 驱动把 UBO 读取整体平移一格（scalar/std140 混淆症状）——非零 magic 排不掉，
    //      只有"错位版 shader 必须拿到不同结果"才排得掉。
    for (uint32_t i = 0; i <= kOutRaw0; ++i) {
        INFO("wrong-shader out slot " << i << " 必须 ≠ 主判据值（否则判据不 discriminative）");
        CHECK(got[i] != want[i]);
    }
}

TEST_CASE("dheap 3：反向对照 —— UBO 槽不写描述符，不得恰好得到期望值") {
    // ⚠️ 本条是四条里唯一可能触发设备挂死/驱动报错的：null descriptor（地址 0）被
    // shader 解引用。这是**结论的一部分**，不是测试缺陷。若它把进程带崩，用
    // `just test-gpu -tc="*反向对照*"` 单独跑做隔离归因（另三条已在此之前跑完）。
    const Fixture fx;
    Harness h(fx);
    h.writeAllDescriptors(/*withUbo=*/false, /*decoys=*/false);
    h.runDispatch(RGL_TEST_SHADER_DIR "/dheap_ubo.comp.spv");

    const auto got = h.readOut();
    const auto want = expectedMain();
    // 锚点在这里**必须仍然写进去**：反向对照只撤掉 UBO 描述符，out 描述符还在。
    // 锚点若也丢了，说明"撤一个描述符"把整条 dispatch 打挂（驱动行为，同样是结论）。
    MESSAGE("反向对照派发锚点 got=" << got[kOutAnchor]
        << "（期望 " << kOutAnchorValue << " = 通路仍在）");
    // 判据 = 「不是**全部**槽位都恰好等于期望值」。刻意不逐槽位 CHECK(!=)：
    // 反向对照要排的是「不写描述符也能拿到正确数据」（= 描述符根本没被消费、
    // 主判据的绿是假的），只要有一个槽位不对，就证明 shader 确实在消费那个描述符。
    // 逐槽位要求"全不对"是过强判据（例如某槽位恰好读到 0 而期望也是 0 的巧合
    // 会让它假红——本 spike 期望值全非零，但判据不该依赖这个巧合）。
    int matched = 0;
    for (uint32_t i = 0; i <= kOutRaw0; ++i)
        if (got[i] == want[i])
            ++matched;
    MESSAGE("反向对照：UBO 槽留空时 11 个数据槽位中恰好命中期望值的个数 = " << matched);
    CHECK(matched < static_cast<int>(kOutRaw0 + 1));
    // 单独钉 UBO 那 7 个槽位（0..6）：它们**只**可能来自 UBO 描述符。
    // 若这里全命中，说明 UBO 描述符被绕过（数据从别处来）→ 主判据不成立。
    int uboMatched = 0;
    for (uint32_t i = 0; i <= 6; ++i)
        if (got[i] == want[i])
            ++uboMatched;
    MESSAGE("反向对照：UBO 专属槽位（0..6）命中数 = " << uboMatched << " / 7");
    CHECK(uboMatched < 7);
}

// ─── dheap 4/5：StructuredBuffer 元素 stride 设备端往返 ────────────────────────
//
// R3 红线（全禁 StructuredBuffer）撤销条件第 2 条（docs/constantbuffer_迁移计划.md §8.4）：
// 「给 tests/ 的 gpu 套件加**实际在用的 stride**（8 / 44）的设备端往返用例——写已知模式到
// 元素 1、经 shader 读回比对。理由：R3 的理由是一条**驱动/布局事实**，事实只能测，
// 不能靠注释或推理定案。」
//
// 与构建期 R4（tools/check_spirv_stride.py）的分工：R4 证**编译器口径**（探针 SPV 的
// ArrayStride == 反射 sizeof），本用例证**驱动真按那个 stride 取数**。缺 R4 → 编译器
// 口径漂了没人知道；缺本用例 → 编译器说 44 而驱动按 48 数数，仍然静默。
//
// 变异红线声明（哪个变异让我红）：
//   去掉 -fvk-use-scalar-layout（std430 口径）→ 44 变 64 ⟹ 主判据 8 个 stride-44 槽位全红
//   契约 struct 加/删一个字段而 shader 镜像不改 → 期望表随 sizeof/offsetof 走 ⟹ 红
//   阳性对照体读元素 0（stride 不参与）→ 「≠ 主判据值」若被改成「==」立刻红（判据不
//     discriminative 时它自己会喊）
//   out buffer 忘 barrier → VVL 当场打断（不靠运气）

TEST_CASE("dheap 4：stride 往返主判据 —— 8B 与 44B 元素经 StructuredBuffer 逐槽位读回") {
    // 🔴 判据工具自检，放在起设备之前：44 这个数**就是**本用例存在的理由。哪天契约把
    // MeshletClusterHeader 改成 16 的倍数，本用例会**悄悄退化成**"又一个 16 对齐 stride
    // 的往返"，而它要抓的那类错位（float3 被补到 16B）根本不再发生 —— 那时"绿"会被读成
    // "还在守"，实际守的是一句空话。缺这几条，用例失效是完全无声的。
    CHECK(sizeof(RGL::MeshletCullItem) == 8);
    CHECK(sizeof(RGL::MeshletClusterHeader) == 44);
    CHECK(sizeof(RGL::MeshletClusterHeader) % 16 != 0);  // 关键：非 16 倍数的奇数 stride
    CHECK(offsetof(RGL::MeshletClusterHeader, center) == 24);
    CHECK(offsetof(RGL::MeshletClusterHeader, radius) == 36);  // float3 占 12B，不占 16B

    const Fixture fx;
    StrideHarness h(fx);
    h.writeDescriptors();
    h.runDispatch(RGL_TEST_SHADER_DIR "/dheap_stride.comp.spv");

    const auto got = h.readOut();
    const auto want = strideExpectedMain();
    // 派发锚点先判（**FATAL**）：锚点缺失 = dispatch / out 描述符没生效，继续判会把
    // "我的夹具没搭对"答成"驱动不支持非 16 倍数 stride"（同款教训见文件头坑 ②）。
    REQUIRE_MESSAGE(got[kStrideOutAnchor] == kStrideOutAnchorValue,
        "派发锚点缺失（got=" << got[kStrideOutAnchor] << "）⟹ dispatch/out 描述符未生效，"
        "本次不构成对 stride 的任何结论");
    MESSAGE("stride 往返（本机 " << fx.deviceName() << "，元素数 " << StrideHarness::kElements
        << "）：sizeof(MeshletCullItem)=" << sizeof(RGL::MeshletCullItem)
        << " sizeof(MeshletClusterHeader)=" << sizeof(RGL::MeshletClusterHeader));
    for (uint32_t i = 0; i <= kStrideOutLast; ++i) {
        INFO("stride 往返槽位 " << i << "（got=" << got[i] << " want=" << want[i] << "）");
        CHECK(got[i] == want[i]);
    }
    // 未使用槽位必须恒 0：写越界/写到别处会在这里现形（与 dheap 1 同形态）。
    for (uint32_t i = kStrideOutAnchor + 1; i < got.size(); ++i)
        CHECK(got[i] == 0u);
}

TEST_CASE("dheap 5：stride 往返阳性对照 —— 读错元素下标必须 ≠ 主判据期望值") {
    const Fixture fx;
    StrideHarness h(fx);
    h.writeDescriptors();
    h.runDispatch(RGL_TEST_SHADER_DIR "/dheap_stride_wrong.comp.spv");

    const auto got = h.readOut();
    const auto want = strideExpectedMain();
    // 同 dheap 2：先钉"这个 shader 确实跑了并写进了 out"，否则"≠ 期望值"可能只是
    // "什么都没写"造成的假阳性对照。
    REQUIRE_MESSAGE(got[kStrideOutAnchor] == kStrideOutAnchorValue,
        "阳性对照体的派发锚点缺失（got=" << got[kStrideOutAnchor] << "）⟹ 对照不成立");
    // 断言方向 = **不等于**。缺这条时主判据的绿排不掉两种假绿：
    //   ① 读回的是零初始化/未初始化值而期望恰好等于它（填充全非零已排一半）；
    //   ② 描述符根本没被消费 / 读到了别处（§3.4 第一条：漏写描述符**完全静默**，
    //      VVL 不报、不 device lost）——只有"错位版 shader 必须拿到不同结果"排得掉。
    for (uint32_t i = 0; i <= kStrideOutLast; ++i) {
        INFO("wrong-index out slot " << i << " 必须 ≠ 主判据值（否则判据不 discriminative）");
        CHECK(got[i] != want[i]);
    }
}

}  // TEST_SUITE("gpu")
