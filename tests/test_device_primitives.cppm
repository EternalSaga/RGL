module;
/**
 * test_device_primitives —— [gpu] 帧回读原语层（批 12.2d）
 *
 * ## 这一层为什么必须有
 *
 * 批 12.1 只做了「纯函数/契约」层，缺 Filament 三层结构里的**第一层：后端原语
 * golden**。缺它的后果是：上层所有图像断言都建立在「回读拿到的就是屏幕上那个像素」
 * 这个**从未验证**的前提上。而这个前提正是本项目吃过大亏的地方——跨日 ±1LSB 叶缘
 * 噪声连续作废 7 次基线（git:调试历史 2026-09-08）。若回读链路本身有偏差，
 * 上层所有直方图 / objectID 锚点 / A-B 对拍判据会**一起偏**，而症状会被读成
 * 「渲染变了」。
 *
 * ## 为什么这一层必须真跑设备（故归入 gpu 套件）
 *
 * 它验的是**驱动与硬件**的行为：clear 后图像里的确切字节、UNORM 的量化取整、
 * image↔buffer 拷贝的逐字节保真与行序。这些不跑一遍真实设备就没有答案。
 *
 * ## 纪律：无设备时**响亮失败，绝不静默 skip**
 *
 * 静默跳过 = 假绿 = 「这批断言从未被执行，但报告显示全过」。本项目反复吃亏于
 * 「上次跑过」≠「这次验的是同一组输入」。故本文件**不做** SKIP：取不到设备即抛
 * 异常，doctest 记为失败。另外 `just test` 默认排除 gpu 套件，`just test-all` 才跑
 * —— 能跑到本文件就说明用户明确要了设备测试，此时没设备就该是错误而不是跳过。
 *
 * ## 为什么复用引擎的 AppContext / DeviceContext（一条改过主意的设计）
 *
 * 首版在这里手写裸 Vulkan 引导（自建 instance + 挑设备 + 建队列），理由是「本层要最小
 * 自足，不该把引擎启动流拖进来」。**改主意的理由**：
 *   ① 引擎那两个类已经把这些事做对了（raii 成员初始化列表顺序、required feature 链、
 *      三队列三命令池、headless 用 VK_EXT_headless_surface），手写一遍等于把这些
 *      结论重新赌一次——而 AGENTS.md 把「验证工具自己错」列为反复出现的元 bug。
 *   ② 本层要担保的恰恰是**引擎实际使用的那条链路**（AppContext→DeviceContext→
 *      图形队列→image→buffer）。自造一条更干净的路，测的就不是引擎那条路了。
 *   ③ 代价是真实的，写下来：本层现在继承了引擎的硬性前置——必须装
 *      `VK_LAYER_KHRONOS_validation`（AppContext 缺层即抛）、必须满足
 *      `kDeviceExtensions()` 的 9 项设备扩展。也就是说「没有 VVL」会被记成失败。
 *      这是**有意的**：对图像回归的地基而言，带验证层跑比不带更能兜住 barrier 误用。
 *
 * 内存刻意仍走原生 `vkAllocateMemory` 而非 VMA：本层只用到 image/buffer 与两种
 * 内存类型，引入 VMA 会让「验回读原语」混进「验 VMA 也工作正常」，哪条坏了说不清。
 */
#include <doctest/doctest.h>

export module test_device_primitives;

import rlstd;
import rlvk;
import vk_instance;  // AppContext（headless 引导）
import vk_device;    // DeviceContext（物理设备选择 + 图形队列 + 命令池）
import vulkan_window;  // SDLWindow 前置类型（AppContext 形参；本层传 nullptr）

namespace RGL::testing::gpu {

/**
 * 设备夹具：引擎的 instance + device + 图形队列（headless，不建窗口、不碰 surface）。
 *
 * @throws std::runtime_error / vk::SystemError 无可用设备、缺 validation layer、
 *   缺 required 设备扩展 —— **一律失败，不 skip**（理由见文件头）。
 */
export class Fixture {
   public:
    /** 构造 headless 实例 + 设备（声明序 = 构造序 = 依赖序：app 先于 device）。 */
    Fixture();

    [[nodiscard]] vk::raii::Device& device() const { return dev_->getDevice(); }
    [[nodiscard]] const vk::raii::PhysicalDevice& physical() const {
        return dev_->getPhysicalDevice();
    }
    [[nodiscard]] vk::Queue queue() const { return dev_->getGraphicsQueue(); }
    [[nodiscard]] const std::string& deviceName() const { return deviceName_; }

    /**
     * 阻塞执行一条命令并等 fence。本层全部是一次性原语，不需要并行或多次提交。
     * @param record 命令录制回调
     * @throws std::runtime_error 提交或等待失败
     */
    void submitAndWait(const std::function<void(vk::raii::CommandBuffer&)>& record) const;

   private:
    // unique_ptr 而非值成员：本类全部访问器是 const，而 raii 句柄的构造与提交入口要
    // **非 const** 的 Device&。值成员在 const 成员函数里只能给 const 引用（逻辑 const
    // 与 bitwise const 的经典分歧）；用指针把「夹具对象不变」与「设备句柄可操作」分开。
    // 声明序 = 构造序 = 依赖序：app_ 先建，dev_ 持其引用；析构自动反序。
    std::unique_ptr<AppContext> app_;
    std::unique_ptr<DeviceContext> dev_;
    std::string deviceName_;
};

/**
 * 一张图 + 独占内存（布局转换由调用方在自己命令里显式录，本层不做隐式封装）。
 *
 * **刻意不带 ImageView**：本层全部是 image 级操作（clearColorImage / copyImage*），
 * 一次 view 都不需要；而给「只有 transfer 用途」的图建 view 会直接撞
 * VUID-VkImageViewCreateInfo-image-04441（实测被 VVL 打断）。等真需要 view 的用例
 * 出现时再加，并且要按 usage 判定能否建。
 */
export struct Image2D {
    vk::raii::Image image;
    vk::raii::DeviceMemory memory;
    vk::Format format = vk::Format::eUndefined;
    uint32_t width = 0;
    uint32_t height = 0;
};

/**
 * 按给定格式建图（内容未定义；内存已绑定）。
 * @param fx 设备夹具
 * @param format 像素格式（须是 bytesPerPixel 认识的那几种）
 * @param w 宽（像素）
 * @param h 高（像素）
 * @param usage 用途（要回读必须含 eTransferSrc）
 * @return 图 + 内存 + view
 */
export Image2D makeImage(const Fixture& fx, vk::Format format, uint32_t w, uint32_t h,
    vk::ImageUsageFlags usage);

/**
 * 整幅回读：录 image→host-visible buffer 的拷贝，提交等完后拷出字节。
 *
 * 调用约定：图像当前布局必须是 eGeneral（本函数内部转进转出，收尾会**复原成 eGeneral**，
 * 故可重复调用）。
 * @return 行紧凑字节（w×h×bpp）
 */
export std::vector<uint8_t> readImage(const Fixture& fx, const Image2D& img);

/**
 * 整幅写入：录 host-visible buffer→image 拷贝（eUndefined→TransferDst→General）。
 *
 * 用途是**不依赖 clear** 地铺一张已知图案再回读 —— 这样验的是拷贝路径本身，
 * 而不是 clear 路径顺带把数据写对。
 * @param src 行紧凑源字节（长度须 = w×h×bpp）
 */
export void writeImage(const Fixture& fx, const Image2D& img, const std::vector<uint8_t>& src);

/**
 * 建图 + clear + 停在 eGeneral 布局（clear 路径不经过 fragment shader ⟹ 无抖动源，
 * 这正是选 clear 作第一条原语的原因）。
 * @return 已清成 cc 的图
 */
export Image2D cleared(const Fixture& fx, vk::Format fmt, uint32_t w, uint32_t h,
    const vk::ClearColorValue& cc);

}  // namespace RGL::testing::gpu

// ═══════════════════════════════════════════════════════════════════════════
// 用例
// ═══════════════════════════════════════════════════════════════════════════

// 设备级用例归入 gpu **测试套件**。是套件不是 tag —— 本项目 doctest 2.5.3 的
// TEST_CASE 宏只接受一个参数（tag 形态在该构建里不存在），而套件过滤是一等公民
// （-ts / -tse），入口见 Justfile 的 just test / just test-all。
TEST_SUITE("gpu") {

using RGL::testing::gpu::cleared;
using RGL::testing::gpu::Fixture;
using RGL::testing::gpu::makeImage;
using RGL::testing::gpu::readImage;
using RGL::testing::gpu::writeImage;

TEST_CASE("clear 红 → 逐像素精确 R=255 G=0 B=0 A=255") {
    const Fixture fx;  // 无设备 / 缺 VVL / 缺扩展即抛（**不 skip**，见文件头）
    constexpr uint32_t kW = 64, kH = 64;
    vk::ClearColorValue cc{};
    cc.float32[0] = 1.0f;
    cc.float32[1] = 0.0f;
    cc.float32[2] = 0.0f;
    cc.float32[3] = 1.0f;
    const auto img = cleared(fx, vk::Format::eR8G8B8A8Unorm, kW, kH, cc);
    const auto px = readImage(fx, img);
    REQUIRE(px.size() == kW * kH * 4u);
    // **逐像素**而非抽样：抽样会让「只有部分像素错」逃过。本项目历史上多次栽在
    // 「整体对、局部错」上——叶缘 ±1LSB 就是典型形态。
    for (uint32_t y = 0; y < kH; ++y)
        for (uint32_t x = 0; x < kW; ++x) {
            const size_t o = (static_cast<size_t>(y) * kW + x) * 4;
            INFO("pixel (" << x << "," << y << ") on " << fx.deviceName());
            CHECK(px[o + 0] == 255);
            CHECK(px[o + 1] == 0);
            CHECK(px[o + 2] == 0);
            CHECK(px[o + 3] == 255);
        }
}

TEST_CASE("R32_UINT clear 0xCAFEBABE → 逐像素整数精确回环") {
    const Fixture fx;
    // 魔数取 0xCAFEBABE 的理由：它的位模式若被当成 float 是一个荒谬的数
    //（约 −8.2e8），若中途被截成别的精度必然掉低位。位模式完全保持 ⟹ 格式与
    // 拷贝路径都没做任何数值转换。objectID 回读（eR32Uint → RGB24 无损编码）
    // 正是靠这条性质成立的。
    constexpr uint32_t kMagic = 0xCAFEBABEu;
    vk::ClearColorValue cc{};
    cc.uint32[0] = kMagic;  // VkClearValue 是 union：uint32 视图与 float 视图同址
    const auto img = cleared(fx, vk::Format::eR32Uint, 32, 32, cc);
    const auto px = readImage(fx, img);
    REQUIRE(px.size() == 32u * 32u * 4u);
    for (size_t i = 0; i < px.size(); i += 4) {
        uint32_t got = 0;
        std::memcpy(&got, px.data() + i, 4);
        INFO("pixel index " << i / 4);
        CHECK(got == kMagic);
    }
}

TEST_CASE("UNORM 量化取整精确：clear 0.5 → 恰好 128（不是 127 也不是 129）") {
    const Fixture fx;
    // 这条专治「看起来差不多」的偷懒断言。0.5 在 IEEE 里精确可表示，×255 = 127.5，
    // Vulkan 规定 UNORM 量化 round-to-nearest-even ⟹ 127.5 → 128。
    // 若链路某处有 ±1 偏移，「颜色差不多」会一直绿，而阴影/受光比会系统性偏 1/255
    // ——那种偏差在任何「整体相似度」指标下都看不见，却会直接进 QA 的比值判据。
    //
    // alpha 单独清成 1.0（不是 0.5）：它必须是**不透明**的，否则后面任何「按 RGB
    // 比色」的 QA 判据都会被 alpha 通道干扰；也正因它取不同的值，这里必须**分通道**
    // 断言——早先写成「四通道全 128」是错的（alpha 该是 255）。
    vk::ClearColorValue cc{};
    cc.float32[0] = 0.5f;
    cc.float32[1] = 0.5f;
    cc.float32[2] = 0.5f;
    cc.float32[3] = 1.0f;
    const auto img = cleared(fx, vk::Format::eR8G8B8A8Unorm, 8, 8, cc);
    const auto px = readImage(fx, img);
    for (size_t i = 0; i < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {  // RGB：clear 0.5 → 128
            INFO("pixel " << i / 4 << " channel " << c);
            CHECK(px[i + c] == 128);
        }
        INFO("pixel " << i / 4 << " alpha");
        CHECK(px[i + 3] == 255);  // alpha：clear 1.0 → 255
    }
}

TEST_CASE("image↔buffer 拷贝逐字节保真 + 行序正确（不经 clear 路径）") {
    const Fixture fx;
    // 铺一张**逐字节递增**的图案再回读：若行序/行宽假设错了（bufferRowLength 没
    // 显式给、imageExtent 写错），行与行会错位——而错位后的图「大体对、细节偏」，
    // 肉眼几乎发现不了。用 i&0xFF 的图案让任何错位都立刻在**字节**上暴露。
    constexpr uint32_t kW = 16, kH = 8;
    std::vector<uint8_t> src(kW * kH * 4);
    for (size_t i = 0; i < src.size(); ++i) src[i] = static_cast<uint8_t>(i & 0xFF);
    const auto img = makeImage(fx, vk::Format::eR8G8B8A8Unorm, kW, kH,
        vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc);
    writeImage(fx, img, src);
    const auto back = readImage(fx, img);
    REQUIRE(back.size() == src.size());
    for (size_t i = 0; i < src.size(); ++i) {
        INFO("byte " << i << " (row " << i / (kW * 4) << ", col " << (i % (kW * 4)) << ")");
        CHECK(back[i] == src[i]);
    }
}

TEST_CASE("反向用例：同一张图连续读两次必须拿到相同内容（布局复原不被搞坏）") {
    // readImage 内部把图从 eGeneral 转进 TransferSrcOptimal、读完**必须复原**。
    // 若漏了复原，第二次调用会因为布局与命令里声明的不符而拿到垃圾（或直接被
    // validation 打断）。这条是「不该触发」的那一半：只测一次读，永远看不出复原有没有。
    const Fixture fx;
    vk::ClearColorValue cc{};
    cc.uint32[0] = 0x00AABBCCu;
    const auto img = cleared(fx, vk::Format::eR32Uint, 8, 8, cc);
    const auto first = readImage(fx, img);
    const auto second = readImage(fx, img);
    REQUIRE(first.size() == second.size());
    CHECK(first == second);
    // 并且两次都真的是那个魔数（否则「两次相同的垃圾」也能让上一条绿）
    for (size_t i = 0; i < first.size(); i += 4) {
        uint32_t got = 0;
        std::memcpy(&got, first.data() + i, 4);
        INFO("pixel index " << i / 4);
        CHECK(got == 0x00AABBCCu);
    }
}

}  // TEST_SUITE("gpu")
