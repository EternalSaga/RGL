module;
/**
 * test_mip_gen_chain —— [gpu] 钉死引擎 UNORM mip 链的**降采样采样点**（债-137 / e098932）
 *
 * ## 为什么必须有这条测试（历史 bug 一句话）
 *
 * `rgl/shaders/mip_gen.comp.slang` 原写 `uv = (float2(gid) + 0.25f) / float2(dstW, dstH)`，
 * 正确常数是 **0.5**（本提交 e098932 修好）。0.25 让双线性落到 src texel `2x` 的**正中心**
 * （权重 1/0/0/0）⟹ 整条 mip 链退化成隔行点抽样、**零滤波**——而"滤波"是这个模块存在的唯一
 * 理由（缩小面散斑）。症状是「albedo 干净、normal 碎」，曾被整轮误诊成材质/光照问题
 *（完整推导与实机判据 → 该 shader 文件头，此处不重述）。修完之后仓库里**没有任何一条测试会
 * 因为这个常数而变红**——本文件补的就是这一格：将来有人把偏移量改错、或只改对一条轴，它必须红。
 *
 * ## 跑的是引擎真产物，不是测试侧副本
 *
 * 加载 `RGL_ENGINE_SHADER_DIR "/mip_gen.comp.spv"`（= `build/rgl/generated_shaders/`，rgl 目标
 * 的真编译产物；`rgl_tests` 已 `add_dependencies(rgl_tests rgl)`）。测试侧另编一份"等价
 * shader"等于测自己——那条盲区与记法先例见 `test_izb_engine_contract.cppm` 文件头（它同样用
 * `RGL_ENGINE_SHADER_DIR` 而非 `RGL_TEST_SHADER_DIR`：后者是 tests/ 自己的 spike 产物目录，
 * 里面根本没有引擎 shader）。
 *
 * ## 场景与判据（三条用例，各自独立可归因；合成、确定性、零 RNG、零墙钟）
 *
 * 8×8 `R8G8B8A8_UNORM`，2 个 mip 级；sampled view 覆盖 level 0、storage view 覆盖 level 1
 *（= 引擎 `MipGen::recordChain` 的 per-level 视图形态：视图范围 = 精确访问子资源，VVL 逐
 * subresource 布局检查天然干净）。push 12B = `{srcMip, dstWidth, dstHeight}`，**类型直接吃反射
 * 生成物** `RGL::MipGenPush`（`compute_push_types_generated`，自带 sizeof/offsetof assert）——
 * 测试不手抄布局。dispatch 1×1×1（`[numthreads(8,8,1)]` ≥ 4×4 目标级，每线程一个 dst texel）。
 * 回读 **level 1**（4×4）。level 1 在 dispatch 前被 clear 成 **255 哨兵**并**回读自证**
 *（判据 ⓪④）：三条用例的期望值没有一条等于 255 ⟹「compute 从未写目标级」是**确定可观测**
 * 的，不靠"新分配内存恰好是 0"的运气。共享 `readImage` 按整幅 level 0 读，故本文件自带
 * 单级回读 `readMip`（共享 helper 的既有语义不动）；共享 `makeImage` 把 mipLevels 硬编码成
 * 1，故本文件自带 `makeMipChainImage`（同款内存/绑定路径，mipLevels=2）。
 *
 * 期望值出处 = **CPU 侧独立盒平均参考实现**（对源图案显式取 2×2 四个纹素的平均），与 shader 的
 * 机制（单 tap 双线性 at `uv=(gid+0.5)/dstSize`）**不同实现路径**、同一条数学契约；再与手推
 * 闭式字面量互相对账（判据 ⓪，见下）。三条图案：
 *
 *   ① 棋盘 `v = ((x+y)&1) ? 252 : 0`：任一 2×2 块恒含两个 0 与两个 252 ⟹ 盒平均 **126**
 *     （断言 ∈ [125,127]）。点抽样会得 0 或 252，差得远。选 252 不选 255：让 126 是**精确**
 *     的 UNORM 值，不踩舍入平局。
 *   ② 水平梯度 `v = 32x`（沿 y 恒定）：mip1(x,·) = **64x + 16**（x 轴偏移错会得 64x 或 64x+32）。
 *   ③ 垂直梯度 `v = 32y`（沿 x 恒定）：mip1(·,y) = **64y + 16**。
 *
 * **② 与 ③ 是刻意的对称对**（skill `rgl-test-discipline`：「对称判据的每一半边各要一条用例」）：
 * 只把 x 轴改对、y 轴留错，必须让 ③ 红——一条"半边"用例会被后来人读成"整条已验"。
 *
 * ## 判据 ⓪：判据工具自检（三条用例各自开头都跑，先于任何 GPU 断言）
 *
 * ① 盒平均参考实现 == 手推闭式字面量（126 / 64x+16 / 64y+16）——参考实现自己写错就地红；
 * ② 「点抽样参考」（块原点 (2x,2y) 单 texel）**必须不等于**盒平均期望——否则这条用例对
 *   历史 bug 结构上不敏感，是摆设；
 * ③ 「右移一格参考」（块原点 (2x+1,2y+1)）与期望的关系**按图案分别自证**：用例②③ 必须不等
 *  （这两条用例对 0.5→1.5 敏感），用例① **必须相等**（棋盘对 1.5 天然不敏感——两轴各自错一格
 *  仍是"两个 0 两个 252"）。用例① 的这条不敏感是**实测事实**，写在这里防止后来人把"用例① 对
 *  1.5 绿"读成"用例① 坏了"。
 *   ⓪③ 只判内部 3×3：块原点在最后一行/列时四 tap 会伸出图边，而图外取什么值由 sampler 的 wrap
 *   模式决定（= 被测配置），不抄进参考实现——敏感性的成立本来也不需要第 4 行。
 * ④ **哨兵机制自检**：铺完 level 1 立刻回读，逐字节 REQUIRE 全 255，然后才轮到 compute。
 *   这条不是形式主义——它抓的是本文件**首版实踩**的自己的 bug：`ClearColorValue` 是 union，
 *   UNORM 格式取的是 `float32` 成员，首版写 `uint32 = 255` 被当成 3.6e-43 的 float ⟹ 清成
 *   **0** ⟹「目标级从未写」锚点静默失效（M4 首轮"红是红了、归因全错、锚点 0 次出声"就是这么
 *   露的）。教训的沉淀地址 = 本条断言本身（TEST: 本文件 `runMipGen` 的哨兵回读循环）。
 *
 * ## 变异红线声明（哪个变异让我红 —— 全部**实测**于本机 AMD Radeon 8060S / RADV STRIX_HALO）
 *
 * M1–M3 改 `rgl/shaders/mip_gen.comp.slang`（→ `just reflect` → `cmake --build build --target
 * rgl_tests`），M4–M5 改本文件的调用点（那两件事在 shader 侧结构上写不出来：dispatch 与视图
 * 级号都由宿主给）。每条还原后都重跑过基线并断言全绿。
 *
 *   M1 偏移 0.5→0.25（**历史 bug 本体**，两轴同时）    → 用例①②③ **全红**。实测 ① 126→**0**（点抽样取 (2x,2y)，其 x+y 恒为偶数 ⟹ 恒 0）、② 64x+16→**64x**（实读 0 64 128 192）、③ 64y+16→**64y**
 *   M2 偏移 0.5→1.5（两轴同时右移一格）                → 用例②③ 红、用例① **绿**。实测 ② 64x+16→**64x+48**（实读 80 144 208 16，末位因 sampler REPEAT 回绕）、③ 同理；① 恒 126 = 判据 ⓪③ 预告的不敏感
 *   M3 只改对 x 轴（x=0.5、y 留 0.25）                 → **用例③ 红**（实测 64y+16→64y）、用例①② 绿 ⟹ 对称对的必要性实证（少了用例③，这个变异全绿过关）
 *   M4 dispatch 忘发（目标级从未写）                    → 用例①②③ **全红**。实测四通道全 **255**（哨兵未动），"目标级未写"锚点各自出声：`mip1 仍有 64 个通道 = 哨兵 255`
 *   M5 src/dst 级号写反（sampled view←level 1、storage view←level 0）→ 用例①②③ **全红**。实测目标级 level 1 保持 **255**；VVL 另两条独立报错（VUID-vkCmdDispatch-imageLayout-00344 / VUID-vkCmdDraw-None-09600 布局不匹配）——**双保险**，但判据不依赖 VVL
 *   M6 dstWidth/dstHeight 写反（本测试两者同为 4 ⟹ **结构上不可观测**）→ **不声明**：要钉它得换非方形图，
 *     而那会把"偏移量"与"宽高"两个变量混进同一条用例；宽高契约的机器判据在 `MipGenPush` 反射
 *     生成物的 sizeof/offsetof assert 里，不在本文件（写出来防止后来人把"没测"读成"测过"）。
 *
 * 注：M5 的级号写反只能从**视图**注入——`SampleLevel` 的 LOD 会被 view 的 level 范围钳制
 *（引擎 `recordChain` 正是靠视图选级，`srcMip` 恒被钳到 0），所以改 shader 里的 `srcMip`
 * 在本配置下结构上不可观测。这条与 M6 同理，写出来同样是为了"没测"不被读成"测过"。
 *
 * 容差口径：逐通道 ±1（`expectNear(got, want, 0.0, 1.0)`）。本机实测**逐字节精确相等**
 *（棋盘全 126、梯度全 64x+16 / 64y+16，diff=0），±1 是给跨驱动留的：UNORM↔float 转换与
 * 硬件过滤只保证 ≥8bit 精度。判别信号是 **16**（64x+16 vs 64x）与 **126**（vs 0/252），
 * 容差比信号小一个量级 ⟹ 放宽它不损失任何一条变异（M1–M5 的实测红/绿即在此容差下取得）。
 *
 * ## ⚠️ 描述符红线（仅 spike 放宽，引擎集成绝不允许）
 *
 * 本测试用**老式 VkDescriptorPool + descriptor set + 传统 compute pipeline**——仅因测试侧放宽
 *（fixture 没有 VMA/Resources，接全量 heap 会把「验一个常数」变成「验引擎装配」）。**引擎侧
 * 绝不允许这套**：`MipGen::recordChain` 走 `vk_resources` 的 VmaDescriptorHeap 用户槽 + shader
 * object（全项目零 pool），纪律与机制 → `docs/DESCRIPTOR_HEAP_SPEC.md`。本文件的 descriptor
 * 代码 = **反例存档**，不构成引擎侧范例，勿从本文件搬运。
 */
#include <doctest/doctest.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import 带进来
#include <string>
#include <vector>

export module test_mip_gen_chain;

import rlstd;
import rlvk;
import compute_push_types_generated;  // RGL::MipGenPush（反射生成物 = 布局单一来源）
import test_device_primitives;  // Fixture / Image2D / writeImage / submitAndWait
import test_support;            // expectNear（明面断言：实际/期望/容差全打出来）

namespace RGL::testing::mipgen {

namespace {

using testing::gpu::Fixture;
using testing::gpu::Image2D;
using testing::gpu::writeImage;

constexpr uint32_t kSrcSize = 8;    // level 0 边长
constexpr uint32_t kDstSize = 4;    // level 1 边长（= max(1, 8>>1)）
constexpr uint32_t kDstMip = 1;     // 本测试生成的目标级
constexpr uint32_t kMipLevels = 2;  // 图总级数（>= 2 才有 level 1 可写）
constexpr uint32_t kBpp = 4;        // R8G8B8A8_UNORM
constexpr uint8_t kSentinel = 0xFF; // level 1 的「从未被 compute 写过」哨兵

/** 输入图案（四通道同值；闭式、零 RNG）。 */
enum class Pattern {
    Checker, ///< v = ((x+y)&1) ? 252 : 0 —— 两轴同时
    GradX,   ///< v = 32x（沿 y 恒定）—— 只激励 x 轴偏移
    GradY,   ///< v = 32y（沿 x 恒定）—— 只激励 y 轴偏移
};

/** 源纹素值（level 0）。 */
uint8_t sourceValue(Pattern p, uint32_t x, uint32_t y) {
    switch (p) {
        case Pattern::Checker: return static_cast<uint8_t>(((x + y) & 1u) != 0u ? 252u : 0u);
        case Pattern::GradX: return static_cast<uint8_t>(32u * x);
        case Pattern::GradY: return static_cast<uint8_t>(32u * y);
    }
    return 0;  // 不可达（穷尽 switch 不写 default：-Wcovered-switch-default 已第一方启用）
}

/**
 * 独立参考实现：以 (x0,y0) 为块原点的 2×2 四纹素盒平均（CPU 上显式取四个源纹素）。
 *
 * 与 shader 的机制（单 tap 双线性 at uv=(gid+0.5)/dstSize）**实现路径不同**、契约相同。
 * 判据 ⓪ 用它取两种块原点：(2dx,2dy) = 正确的 2× 降采样块；(2dx+1,2dy+1) = 「右移一格」
 * 的等价形态（0.5→1.5 偏移会算成什么）。点抽样参考（0.25 偏移的等价形态）不走本函数，
 * 它直接取 `sourceValue(p, 2dx, 2dy)` 单纹素。
 * ⚠ 调用方须保证四个 tap 都在 [0, kSrcSize) 内（两种块原点的最大 tap 分别是 7 与 6，均满足）。
 * 越界**不建模**：图外的值该是多少由 sampler 的 wrap 模式决定，把被测对象的配置抄进参考
 * 实现，会让"参考"跟着一起错而判据失效。
 */
uint32_t boxFrom(Pattern p, uint32_t x0, uint32_t y0) {
    const uint32_t a = sourceValue(p, x0, y0);
    const uint32_t b = sourceValue(p, x0 + 1, y0);
    const uint32_t c = sourceValue(p, x0, y0 + 1);
    const uint32_t d = sourceValue(p, x0 + 1, y0 + 1);
    return (a + b + c + d) / 4u;
}

/** 手推闭式期望（与 boxFrom 出处不同：一条是"块平均"，一条是"解出 64x+16"的代数结论）。 */
uint32_t closedForm(Pattern p, uint32_t dx, uint32_t dy) {
    switch (p) {
        case Pattern::Checker: return 126u;
        case Pattern::GradX: return 64u * dx + 16u;
        case Pattern::GradY: return 64u * dy + 16u;
    }
    return 0;  // 不可达
}

/** 判据 ⓪：参考实现自己先过阳性锚点，并自证本用例对每条变异敏感/不敏感。 */
void selfCheckReference(Pattern p) {
    for (uint32_t dy = 0; dy < kDstSize; ++dy) {
        for (uint32_t dx = 0; dx < kDstSize; ++dx) {
            INFO("pattern=" << (p == Pattern::Checker ? "checker"
                                  : p == Pattern::GradX ? "gradX" : "gradY")
                             << " dst(" << dx << "," << dy << ")");
            const uint32_t want = closedForm(p, dx, dy);
            // ⓪① 盒平均参考 == 闭式字面量（块原点 (2dx,2dy) → 四 tap 最大 7，全在界内）
            CHECK(boxFrom(p, 2 * dx, 2 * dy) == want);
            // ⓪② 点抽样参考（= 0.25 偏移的等价形态）必须**不等于**期望，否则本用例对历史
            //     bug 结构上不敏感（摆设）
            CHECK(sourceValue(p, 2 * dx, 2 * dy) != want);
        }
    }
    // ⓪③ 「右移一格」参考（= 0.5→1.5 偏移的等价形态）：梯度必须**不等**（这两条用例对该变异
    //     敏感），棋盘必须**相等**（该用例不敏感，文件头已声明）——两个方向都自证，防"敏感/
    //     不敏感"记反。只判内部 3×3：块原点 (2dx+1,2dy+1) 在 dx=3 时四 tap 会伸出图边，而图外
    //     取什么值由 sampler 的 wrap 模式决定（= 被测配置），不进参考实现（见 boxFrom 头注释）。
    //     敏感性的成立不需要第 4 行：内部 9 个 texel 每一个都必须红，就已经足够。
    for (uint32_t dy = 0; dy + 1 < kDstSize; ++dy) {
        for (uint32_t dx = 0; dx + 1 < kDstSize; ++dx) {
            INFO("pattern=" << (p == Pattern::Checker ? "checker"
                                  : p == Pattern::GradX ? "gradX" : "gradY")
                             << " dst(" << dx << "," << dy << ") shifted-box");
            const uint32_t shifted = boxFrom(p, 2 * dx + 1, 2 * dy + 1);
            const uint32_t want = closedForm(p, dx, dy);
            if (p == Pattern::Checker)
                CHECK(shifted == want);  // 棋盘对 1.5 不敏感：错一格仍是"两个 0 两个 252"
            else
                CHECK(shifted != want);  // 梯度对 1.5 敏感：差 32
        }
    }
}

// ── 设备侧脚手架 ────────────────────────────────────────────────────────────

/** 找同时满足 want 全部属性的内存类型（共享夹具没导出这个，测试侧最小自造）。 */
uint32_t findMemoryType(const Fixture& fx, uint32_t bits, vk::MemoryPropertyFlags want) {
    const auto props = fx.physical().getMemoryProperties();
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) == 0)
            continue;
        const auto f = static_cast<vk::MemoryPropertyFlags>(props.memoryTypes[i].propertyFlags);
        if ((f & want) == want)
            return i;
    }
    REQUIRE_MESSAGE(false, "找不到满足属性的内存类型（device-local / host-visible 之一）");
    return UINT32_MAX;
}

/**
 * 建 8×8 RGBA8 / 2 mip 级的图（**不用共享 makeImage**：它把 mipLevels 硬编码成 1，
 * 而本测试的全部对象就是 level 1。共享夹具的既有语义不改，故测试侧自建同形状图，
 * 内存/绑定路径与 `test_device_primitives_impl.cppm::makeImage` 同款）。
 */
Image2D makeMipChainImage(const Fixture& fx) {
    vk::ImageCreateInfo ii{};
    ii.imageType = vk::ImageType::e2D;
    ii.format = vk::Format::eR8G8B8A8Unorm;
    ii.extent = vk::Extent3D{kSrcSize, kSrcSize, 1};
    ii.mipLevels = kMipLevels;
    ii.arrayLayers = 1;
    ii.samples = vk::SampleCountFlagBits::e1;
    ii.tiling = vk::ImageTiling::eOptimal;
    // level 0 = 采样源（eSampled + writeImage 的 eTransferDst）；level 1 = compute 目标
    //（eStorage）+ 回读源（eTransferSrc）
    ii.usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage
        | vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc;
    ii.sharingMode = vk::SharingMode::eExclusive;
    ii.initialLayout = vk::ImageLayout::eUndefined;
    vk::raii::Image image(fx.device(), ii);

    const auto req = image.getMemoryRequirements();
    vk::MemoryAllocateInfo ai{};
    ai.allocationSize = req.size;  // 🔴 取**需求**而非请求值（先例 → test_device_primitives_impl）
    ai.memoryTypeIndex = findMemoryType(fx, req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal);
    vk::raii::DeviceMemory memory(fx.device(), ai);
    image.bindMemory(memory, 0);
    return Image2D{.image = std::move(image), .memory = std::move(memory),
        .format = vk::Format::eR8G8B8A8Unorm, .width = kSrcSize, .height = kSrcSize};
}

/** 覆盖单个 mip 级的 view（= 引擎 recordChain 的 per-level 视图形态）。 */
vk::raii::ImageView makeMipView(const Fixture& fx, const Image2D& img, uint32_t mip) {
    vk::ImageViewCreateInfo vi{};
    vi.image = *img.image;
    vi.viewType = vk::ImageViewType::e2D;
    vi.format = img.format;
    vi.subresourceRange = {vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1};
    return vk::raii::ImageView(fx.device(), vi);
}

/**
 * 引擎共享 sampler 的同款参数（`SamplerLibrary::ensureShared`）：LINEAR/LINEAR + mip LINEAR +
 * U/V REPEAT、W CLAMP + minLod 0 / maxLod 20 + 不比较 + 归一化坐标。
 * 各向异性刻意不带：它只参与**隐式 LOD** 选择与过滤，本 shader 用 SampleLevel 显式给级
 *（引擎侧同理由 → sampler_library.cppm ensureShared 头注释）。
 */
vk::raii::Sampler makeEngineLikeSampler(const Fixture& fx) {
    vk::SamplerCreateInfo si{};
    si.magFilter = vk::Filter::eLinear;
    si.minFilter = vk::Filter::eLinear;
    si.mipmapMode = vk::SamplerMipmapMode::eLinear;
    si.addressModeU = vk::SamplerAddressMode::eRepeat;
    si.addressModeV = vk::SamplerAddressMode::eRepeat;
    si.addressModeW = vk::SamplerAddressMode::eClampToEdge;
    si.compareEnable = false;
    si.minLod = 0.0f;
    si.maxLod = 20.0f;
    si.unnormalizedCoordinates = false;
    si.borderColor = vk::BorderColor::eIntTransparentBlack;
    return vk::raii::Sampler(fx.device(), si);
}

/** 读 SPV 建 shader module（文件缺失 = 响亮失败，不 skip）。 */
vk::raii::ShaderModule makeModule(const Fixture& fx, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    REQUIRE_MESSAGE(f.is_open(), "打不开 " << path << "（引擎产物缺失 = 失败，不 skip）");
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
    ci.layout = *layout;
    return vk::raii::Pipeline(fx.device(), nullptr, ci);
}

vk::ImageMemoryBarrier2 mipBarrier(const vk::raii::Image& image, uint32_t mip,
    vk::ImageLayout oldLayout, vk::ImageLayout newLayout, vk::PipelineStageFlags2 srcStage,
    vk::AccessFlags2 srcAccess, vk::PipelineStageFlags2 dstStage, vk::AccessFlags2 dstAccess) {
    vk::ImageMemoryBarrier2 b{};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = *image;
    b.subresourceRange = {vk::ImageAspectFlagBits::eColor, mip, 1, 0, 1};
    return b;
}

void emitBarrier(vk::raii::CommandBuffer& cmd, const vk::ImageMemoryBarrier2& b) {
    vk::DependencyInfo dep{};
    dep.setImageMemoryBarriers(b);
    cmd.pipelineBarrier2(dep);
}

struct HostBuffer {
    vk::raii::Buffer buffer;
    vk::raii::DeviceMemory memory;
    void* mapped = nullptr;
};

/** host-visible + coherent 映射 buffer（共享夹具没导出，测试侧最小自造，口径同 impl）。 */
HostBuffer makeHostBuffer(const Fixture& fx, VkDeviceSize bytes, vk::BufferUsageFlags usage) {
    vk::BufferCreateInfo bi{};
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = vk::SharingMode::eExclusive;
    vk::raii::Buffer buf(fx.device(), bi);
    const auto req = buf.getMemoryRequirements();
    vk::MemoryAllocateInfo ai{};
    ai.allocationSize = req.size;  // 🔴 取需求（56B 实例外伤 → test_device_primitives_impl）
    ai.memoryTypeIndex = findMemoryType(fx, req.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    vk::raii::DeviceMemory mem(fx.device(), ai);
    buf.bindMemory(mem, 0);
    void* p = mem.mapMemory(0, bytes);
    return HostBuffer{.buffer = std::move(buf), .memory = std::move(mem), .mapped = p};
}

/**
 * 回读**单个 mip 级**（共享 `readImage` 按整幅 level 0 读，改它既有语义会波及全部既有
 * 用例，故测试侧另开一条；调用约定与 readImage 一致：图须处于 eGeneral，内部转进转出）。
 */
std::vector<uint8_t> readMip(const Fixture& fx, const Image2D& img, uint32_t mip) {
    const uint32_t w = std::max(1u, img.width >> mip);
    const uint32_t h = std::max(1u, img.height >> mip);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * kBpp;
    auto hb = makeHostBuffer(fx, bytes, vk::BufferUsageFlagBits::eTransferDst);
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        emitBarrier(cmd, mipBarrier(img.image, mip, vk::ImageLayout::eGeneral,
                                vk::ImageLayout::eTransferSrcOptimal,
                                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead));
        vk::BufferImageCopy region{};
        region.bufferRowLength = w;      // 显式给（行序假设 → test_device_primitives）
        region.bufferImageHeight = h;
        region.imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1};
        region.imageExtent = vk::Extent3D{w, h, 1};
        cmd.copyImageToBuffer(img.image, vk::ImageLayout::eTransferSrcOptimal, *hb.buffer, region);
        emitBarrier(cmd, mipBarrier(img.image, mip, vk::ImageLayout::eTransferSrcOptimal,
                                vk::ImageLayout::eGeneral,
                                vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead,
                                vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone));
    });
    std::vector<uint8_t> out(static_cast<size_t>(bytes));
    std::memcpy(out.data(), hb.mapped, static_cast<size_t>(bytes));
    hb.memory.unmapMemory();
    return out;
}

/**
 * 给 level 1 铺「从未被 compute 写过」的哨兵（UNORM 255）。
 *
 * 布局链：UNDEFINED → TRANSFER_DST（clearColorImage 要求 GENERAL 或 TRANSFER_DST）→ GENERAL
 *（= storage image 描述符声明的布局）。
 * 🔴 clear value 必须经 **float32** 通道给：UNORM 格式取的是 `ClearColorValue::float32`
 *（uint32 视图只服务 UINT 格式）。首轮实测写 `uint32 = 255` → 被当成 3.6e-43 的 float
 * → 清成 **0**，「目标级从未写」锚点当场静默失效（M4 首轮"红是红了、归因全错"就是这么露的）。
 * 该失效由 `runMipGen` 里的哨兵回读自证（判据 ⓪④）兜住，不靠注释。
 */
void primeSentinel(const Fixture& fx, const Image2D& img) {
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        emitBarrier(cmd, mipBarrier(img.image, kDstMip, vk::ImageLayout::eUndefined,
                               vk::ImageLayout::eTransferDstOptimal,
                               vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                               vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite));
        vk::ClearColorValue sentinel{};
        sentinel.float32[0] = 1.0f;
        sentinel.float32[1] = 1.0f;
        sentinel.float32[2] = 1.0f;
        sentinel.float32[3] = 1.0f;
        const vk::ImageSubresourceRange range(
            vk::ImageAspectFlagBits::eColor, kDstMip, 1, 0, 1);
        cmd.clearColorImage(img.image, vk::ImageLayout::eTransferDstOptimal, sentinel, range);
        emitBarrier(cmd, mipBarrier(img.image, kDstMip, vk::ImageLayout::eTransferDstOptimal,
                               vk::ImageLayout::eGeneral,
                               vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferWrite,
                               vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone));
    });
}

/**
 * 跑一次引擎 mip 链的单级生成并回读 level 1。
 *
 * @return level 1 的行紧凑 RGBA8（kDstSize×kDstSize×4 字节）
 * @note 资源全部是本函数局部量，析构反序 = 依赖倒序（view 先于 image、set 先于 pool），
 *       不需要手写销毁顺序，也不需要把一摞 raii 句柄塞进结构体去拧声明序。
 */
std::vector<uint8_t> runMipGen(const Fixture& fx, Pattern pattern) {
    // ── 输入图案（host 侧闭式生成，四通道同值）────────────────────────────
    std::vector<uint8_t> src(static_cast<size_t>(kSrcSize) * kSrcSize * kBpp);
    for (uint32_t y = 0; y < kSrcSize; ++y) {
        for (uint32_t x = 0; x < kSrcSize; ++x) {
            const uint8_t v = sourceValue(pattern, x, y);
            const size_t o = (static_cast<size_t>(y) * kSrcSize + x) * kBpp;
            src[o + 0] = v;
            src[o + 1] = v;
            src[o + 2] = v;
            src[o + 3] = v;
        }
    }

    // ── 图 + level 0 图案（复用共享 writeImage：它只碰 mip 0，正是本测试要的）──
    auto img = makeMipChainImage(fx);
    writeImage(fx, img, src);  // 收尾把 level 0 停在 eGeneral

    // ── 判据 ⓪④：哨兵机制自检（先铺、再回读确认铺上了，然后才轮到 compute）────
    // 「目标级从未被写」这条锚点的全部价值在于哨兵**真的**是 255；不判这一步，clear 取错
    // union 成员（见 primeSentinel 头注释）会让锚点静默失效，M4/M5 红了也归因错。
    primeSentinel(fx, img);
    const auto primed = readMip(fx, img, kDstMip);
    REQUIRE(primed.size() == static_cast<size_t>(kDstSize) * kDstSize * kBpp);
    for (size_t i = 0; i < primed.size(); ++i) {
        CHECK_MESSAGE(primed[i] == kSentinel,
            "哨兵未生效：level 1 字节偏移 " << i << " 读到 " << static_cast<int>(primed[i])
                                            << "（应为 255）⟹「目标级从未写」锚点会静默失效");
    }

    // ── 视图 / sampler / 描述符（⚠️ 老式 pool + set：测试侧例外，见文件头描述符红线）──
    const auto srcView = makeMipView(fx, img, 0);
    const auto dstView = makeMipView(fx, img, kDstMip);
    const auto sampler = makeEngineLikeSampler(fx);

    const std::array<vk::DescriptorSetLayoutBinding, 3> bindings{{
        {0, vk::DescriptorType::eSampledImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
        {1, vk::DescriptorType::eStorageImage, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
        {2, vk::DescriptorType::eSampler, 1, vk::ShaderStageFlagBits::eCompute, nullptr},
    }};
    vk::DescriptorSetLayoutCreateInfo li{};
    li.setBindings(bindings);
    const vk::raii::DescriptorSetLayout dsLayout(fx.device(), li);
    const std::array<vk::DescriptorPoolSize, 3> poolSizes{{
        vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1},
        vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1},
    }};
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

    // 描述符里的 layout 必须与 dispatch 时的**实际**布局一致（下面逐 subresource 过渡）
    const std::array<vk::DescriptorImageInfo, 3> imgInfos{{
        {{}, *srcView, vk::ImageLayout::eShaderReadOnlyOptimal},
        {{}, *dstView, vk::ImageLayout::eGeneral},
        {*sampler, {}, vk::ImageLayout::eUndefined},
    }};
    std::array<vk::WriteDescriptorSet, 3> writes{};
    for (uint32_t i = 0; i < 3u; ++i) {
        writes[i].dstSet = *sets[0];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = bindings[i].descriptorType;
        writes[i].pImageInfo = &imgInfos[i];
    }
    fx.device().updateDescriptorSets(writes, {});  // raii 版叫 update（vk 版才叫 write）

    vk::PushConstantRange pcr{vk::ShaderStageFlagBits::eCompute, 0, sizeof(MipGenPush)};
    vk::PipelineLayoutCreateInfo plci{};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &*dsLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    const vk::raii::PipelineLayout pipeLayout(fx.device(), plci);

    // **引擎真产物**（非测试侧副本）——本测试的全部意义在这一行
    const auto mod = makeModule(fx, RGL_ENGINE_SHADER_DIR "/mip_gen.comp.spv");
    const auto pipe = makeCompute(fx, pipeLayout, mod);

    // 指定初始化器 = 「shader 改字段名/换序」在**本调用点**编译期红（位置聚合初始化与字段名
    // 无关，实测不红 → mip_gen_impl.cppm 同款告诫；AGENTS.md 红线 2 / 债-118）
    const MipGenPush push{.srcMip = 0u, .dstWidth = kDstSize, .dstHeight = kDstSize};

    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        // level 0：writeImage 停在 GENERAL → 采样源（跨 submission 已由 queue.waitIdle 分隔，
        // 故 srcStage 用 TopOfPipe/eNone 即可）
        emitBarrier(cmd, mipBarrier(img.image, 0, vk::ImageLayout::eGeneral,
                               vk::ImageLayout::eShaderReadOnlyOptimal,
                               vk::PipelineStageFlagBits2::eTopOfPipe, vk::AccessFlagBits2::eNone,
                               vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderSampledRead));
        // level 1 已由 primeSentinel 落到 eGeneral（= storage 描述符声明的布局），与本次
        // dispatch 之间隔着 queue.waitIdle，无需再录布局转换。

        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipe);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *pipeLayout, 0, *sets[0], {});
        cmd.pushConstants(*pipeLayout, vk::ShaderStageFlagBits::eCompute, 0,
            static_cast<uint32_t>(sizeof(push)), &push);
        // 1×1×1 组 × [numthreads(8,8,1)] ≥ 4×4 目标级（引擎同式：(dstW+7)/8）
        cmd.dispatch(1, 1, 1);
    });

    return readMip(fx, img, kDstMip);
}

/** 逐 texel 逐通道判据：盒平均期望（±1）+「目标级真被写过」阳性锚点。 */
void checkChain(const Fixture& fx, Pattern p, const std::vector<uint8_t>& px) {
    REQUIRE(px.size() == static_cast<size_t>(kDstSize) * kDstSize * kBpp);
    int unwritten = 0;
    std::string got;  // 实读 R 通道的 dump（一次看全，不被首个 FATAL 截断）
    for (uint32_t dy = 0; dy < kDstSize; ++dy) {
        for (uint32_t dx = 0; dx < kDstSize; ++dx) {
            const uint32_t want = closedForm(p, dx, dy);
            for (uint32_t c = 0; c < kBpp; ++c) {
                const size_t o = (static_cast<size_t>(dy) * kDstSize + dx) * kBpp + c;
                const uint8_t v = px[o];
                INFO("mip1 texel (" << dx << "," << dy << ") channel " << c << " on "
                                    << fx.deviceName());
                if (v == kSentinel)
                    ++unwritten;  // 哨兵未动 = compute 从未写这个通道
                RGL::testing::expectNear(static_cast<double>(v), static_cast<double>(want), 0.0, 1.0);
            }
            got += std::to_string(px[(static_cast<size_t>(dy) * kDstSize + dx) * kBpp]);
            got += ' ';
        }
        got += '\n';
    }
    MESSAGE("mip1 R 通道实读（" << kDstSize << "x" << kDstSize << "）:\n" << got);
    // 阳性锚点（独立于上面的数值判据、单独可归因）：目标级一个哨兵都不该留。
    // 期望值恒不等于 255（棋盘 126 / 梯度 16..208）⟹ 这条与 expectNear 不会互相掩盖。
    CHECK_MESSAGE(unwritten == 0,
        "mip1 仍有 " << unwritten << " 个通道 = 哨兵 255 ⟹ compute 从未写目标级"
                     << "（dispatch 忘发 / 写错级 / 视图级号写反）");
}

}  // namespace

}  // namespace RGL::testing::mipgen

TEST_SUITE("gpu") {

using namespace RGL::testing::mipgen;  // 图案 / 参考实现 / 设备脚手架（同 TU 内部名）

TEST_CASE("mip_gen_chain 棋盘（两轴同时）：2×2 盒平均 = 126（点抽样会得 0 或 252）") {
    const RGL::testing::gpu::Fixture fx;  // 无设备 / 缺 VVL 即抛（**不 skip**，gpu 套件纪律）
    selfCheckReference(Pattern::Checker);
    // 输入设计自证：棋盘沿 x、沿 y **都**交替（只在图内判——图外的值不属于本测试的输入）。
    // 若图案退化成全同，126 这条期望就白给（盒平均与点抽样会算出同一个数）。
    for (uint32_t y = 0; y + 1 < kSrcSize; ++y) {
        CHECK(sourceValue(Pattern::Checker, 0, y) != sourceValue(Pattern::Checker, 1, y));
        CHECK(sourceValue(Pattern::Checker, 0, y) != sourceValue(Pattern::Checker, 0, y + 1));
    }
    checkChain(fx, Pattern::Checker, runMipGen(fx, Pattern::Checker));
}

TEST_CASE("mip_gen_chain 水平梯度：mip1(x,·) = 64x + 16（x 轴偏移错会得 64x 或 64x+32）") {
    const RGL::testing::gpu::Fixture fx;
    selfCheckReference(Pattern::GradX);
    // 输入设计自证：沿 x 变、沿 y 恒定（否则这条用例根本测不到 x 轴，会被读成"测过了"）
    for (uint32_t y = 0; y + 1 < kSrcSize; ++y) {
        CHECK(sourceValue(Pattern::GradX, 2, y) != sourceValue(Pattern::GradX, 3, y));
        CHECK(sourceValue(Pattern::GradX, 1, y) == sourceValue(Pattern::GradX, 1, y + 1));
    }
    checkChain(fx, Pattern::GradX, runMipGen(fx, Pattern::GradX));
}

TEST_CASE("mip_gen_chain 垂直梯度：mip1(·,y) = 64y + 16（y 轴偏移错会得 64y 或 64y+32）") {
    const RGL::testing::gpu::Fixture fx;
    selfCheckReference(Pattern::GradY);
    // 输入设计自证：沿 y 变、沿 x 恒定 —— 与上一条**刻意对称**（只改对 x 轴的变异必须让这条红）
    for (uint32_t x = 0; x + 1 < kSrcSize; ++x) {
        CHECK(sourceValue(Pattern::GradY, x, 2) != sourceValue(Pattern::GradY, x, 3));
        CHECK(sourceValue(Pattern::GradY, x, 1) == sourceValue(Pattern::GradY, x + 1, 1));
    }
    checkChain(fx, Pattern::GradY, runMipGen(fx, Pattern::GradY));
}

}  // TEST_SUITE("gpu")
