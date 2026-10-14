module;
/**
 * test_device_primitives 的夹具实现（契约见接口 TU 头注释）。
 *
 * 分区实现单元（`module X:impl;`）：CMake 的 CXX_MODULES FILE_SET **不收纯实现单元**，
 * 故「接口声明 + 分区实现」是本项目既定形状（同 ecs/ecs_impl）。接口 TU 另含 TEST_CASE
 * 注册（这里不 define DOCTEST_CONFIG_IMPLEMENT）。
 */
#include <vulkan/vulkan.h>  // VK_* 宏（rlvk 只导出 C++ 名，不导出宏，见 rlvk 头注释）
#include <initializer_list>  // 必须：modules 下 std::initializer_list 不从 import 带进来，
                             // 少了它 `extent = {w, h, 1}` 会报「ambiguous with void」
                             // （花括号推不出类型 → 退化成 void），症状离真因很远
#include <cstdint>           // UINT32_MAX
#include <cstring>           // std::memcpy

module test_device_primitives:impl;

import test_device_primitives;  // clang 不给 partition impl unit 隐式 import 主接口
import rlstd;
import rlvk;
import vk_instance;
import vk_device;

namespace RGL::testing::gpu {

namespace {

constexpr uint32_t kNoIndex = UINT32_MAX;

/** 单 mip / 单层的色彩子资源 barrier 目标设定（本层图全是 1×1×1）。 */
void setBarrierTarget(vk::ImageMemoryBarrier2& b, const vk::raii::Image& img) {
    b.image = img;
    b.subresourceRange.aspectMask = vk::ImageAspectFlagBits::eColor;
    b.subresourceRange.baseMipLevel = 0;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.baseArrayLayer = 0;
    b.subresourceRange.layerCount = 1;
}

/** 在内存类型里找同时满足 want 全部标志的那个；找不到返回 kNoIndex。 */
uint32_t findMemoryType(const vk::raii::PhysicalDevice& phys, uint32_t requirementBits,
    vk::MemoryPropertyFlags want) {
    const vk::PhysicalDeviceMemoryProperties mp = phys.getMemoryProperties();
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
        if (!(requirementBits & (1u << i)))
            continue;
        const auto flags = static_cast<vk::MemoryPropertyFlags>(mp.memoryTypes[i].propertyFlags);
        if ((flags & want) == want)
            return i;
    }
    return kNoIndex;
}

/** 本层用到的格式的每像素字节数。名单外的格式**拒绝猜测**（宁抛不猜）。 */
uint32_t bytesPerPixel(vk::Format f) {
    switch (f) {
        case vk::Format::eR32Uint:
        case vk::Format::eR32Sfloat:  // 11.3 补账：izb 引擎契约测试的深度输入（实测用过才入册）
        case vk::Format::eR8G8B8A8Unorm:
        case vk::Format::eR8G8B8A8Srgb:
        case vk::Format::eB8G8R8A8Unorm:
        case vk::Format::eB8G8R8A8Srgb: return 4;
        default:
            throw std::runtime_error(
                "test_device_primitives: 未经测量的格式，bpp 拒绝猜测（见 bytesPerPixel）");
    }
}

/** host-visible + coherent 的常驻映射 staging buffer（raii 成员不可默认构造 → 只能移动返回）。 */
struct HostBuffer {
    vk::raii::Buffer buffer;
    vk::raii::DeviceMemory memory;
    void* mapped = nullptr;
};

HostBuffer makeHostBuffer(const Fixture& fx, VkDeviceSize bytes, vk::BufferUsageFlags usage) {
    vk::BufferCreateInfo bi{};
    bi.size = bytes;
    bi.usage = usage;
    bi.sharingMode = vk::SharingMode::eExclusive;
    vk::raii::Buffer buf(fx.device(), bi);

    // raii 句柄自带 getMemoryRequirements()（句柄认识自己的 device/physical device），
    // 比 physical().getBufferMemoryRequirements(*buf) 少绕一层，也没有 const 歧义。
    const vk::MemoryRequirements req = buf.getMemoryRequirements();
    const uint32_t t = findMemoryType(fx.physical(), req.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent);
    if (t == kNoIndex)
        throw std::runtime_error("test_device_primitives: 找不到 host-visible+coherent 内存类型");
    // 🔴 分配大小取**需求** `req.size`，不是请求的 `bytes`（§19.8 挂号，2026-10-14 收账）。
    // 为什么旧写法一直没出声：本机（RADV STRIX_HALO）这四个夹具的 42 次分配实测
    // `req.size ≡ bytes`（对齐 16，请求值全是 16 的倍数）⟹ 两种写法**数值相同**。
    // 但它不是假想风险，**同一后端**在别的夹具上咬过：`testing::gpu::readImage` 的 56B staging
    // buffer 实测报 `requirements size 64 vs allocationSize 56`（VUID-vkBindBufferMemory-None-10741）
    // 且回读全 0 = 判据假空 —— 原文与规避方式见 `test_meshlet_shadow_args.cppm` 文件头。
    // 下面这条守的是「驱动把需求报得比请求还小」——真发生时就地定位，别留到绑定之后猜。
    if (req.size < static_cast<VkDeviceSize>(bytes))
        throw std::runtime_error(
            "test_device_primitives: 内存需求小于请求大小（驱动异常，宁抛不猜）");
    vk::MemoryAllocateInfo ai{};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = t;
    vk::raii::DeviceMemory mem(fx.device(), ai);
    buf.bindMemory(mem, 0);

    // raii 版 map() 直接返回指针并在失败时抛（不返 Result），比裸 mapMemory 少一段判断
    void* p = mem.mapMemory(0, bytes);
    return HostBuffer{.buffer = std::move(buf), .memory = std::move(mem), .mapped = p};
}

}  // namespace

// ── Fixture ───────────────────────────────────────────────────────────────

Fixture::Fixture()
    // 声明序 = 构造序：app_ 先建（instance），dev_ 后建（持 app_ 引用）。析构自动反序。
    // headless=true：不建窗口、不碰 SDL surface，instance 扩展走 VK_EXT_headless_surface。
    // 失败路径全部**向上抛**（缺 validation layer / 缺 required 扩展 / 无设备），
    // 由 doctest 记成失败 —— 刻意不 catch 后 skip（见接口 TU 文件头纪律）。
    : app_(std::make_unique<AppContext>(nullptr, /*headless=*/true)),
      dev_(std::make_unique<DeviceContext>(*app_)),
      deviceName_(dev_->getPhysicalDevice().getProperties().deviceName.data()) {}

void Fixture::submitAndWait(const std::function<void(vk::raii::CommandBuffer&)>& record) const {
    vk::CommandBufferAllocateInfo ai{};
    ai.commandPool = dev_->getGraphicsCommandPool();
    ai.level = vk::CommandBufferLevel::ePrimary;
    ai.commandBufferCount = 1;
    // 本版本 raii 没有 CommandBuffer(Device, AllocInfo) 构造，走 allocateCommandBuffers
    std::vector<vk::raii::CommandBuffer> cmds = dev_->getDevice().allocateCommandBuffers(ai);
    auto& cmd = cmds.front();
    cmd.begin(vk::CommandBufferBeginInfo{vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    record(cmd);
    cmd.end();
    vk::SubmitInfo si{};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &*cmd;
    dev_->getGraphicsQueue().submit(si, nullptr);
    // 等完成的口径：queue.waitIdle() 而不是 fence + waitForFences。本层命令是一次性
    // 微秒级 clear/copy，两者的可观测结果相同；而本版本 raii 的 waitForFences 要
    // ArrayProxy<const vk::Fence>（不给 raii Fence 转换路），为它拧代理不划算。
    // **不带超时**是刻意的：挂死比失败更糟，超时后继续跑会读到半成品数据。
    dev_->getGraphicsQueue().waitIdle();
}

// ── 图像 / 读写 ───────────────────────────────────────────────────────────

Image2D makeImage(const Fixture& fx, vk::Format format, uint32_t w, uint32_t h,
    vk::ImageUsageFlags usage) {
    vk::ImageCreateInfo ii{};
    ii.imageType = vk::ImageType::e2D;
    ii.format = format;
    ii.extent = vk::Extent3D{w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = vk::SampleCountFlagBits::e1;
    ii.tiling = vk::ImageTiling::eOptimal;
    ii.usage = usage;
    ii.sharingMode = vk::SharingMode::eExclusive;
    ii.initialLayout = vk::ImageLayout::eUndefined;
    vk::raii::Image image(fx.device(), ii);

    const vk::MemoryRequirements req = image.getMemoryRequirements();
    const uint32_t t = findMemoryType(fx.physical(), req.memoryTypeBits,
        vk::MemoryPropertyFlagBits::eDeviceLocal);
    if (t == kNoIndex)
        throw std::runtime_error("test_device_primitives: 找不到 device-local 内存类型");
    vk::MemoryAllocateInfo ai{};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = t;
    vk::raii::DeviceMemory memory(fx.device(), ai);
    image.bindMemory(memory, 0);

    // 不建 ImageView（理由见接口 Image2D 注释：transfer-only 的图建 view 违 VUID-04441）
    // raii 成员无默认构造 ⟹ 不能「先声明后逐个赋值」，只能建好后移动聚合进返回值
    return Image2D{.image = std::move(image), .memory = std::move(memory),
        .format = format, .width = w, .height = h};
}

void writeImage(const Fixture& fx, const Image2D& img, const std::vector<uint8_t>& src) {
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(img.width) * img.height * bytesPerPixel(img.format);
    if (src.size() != bytes)
        throw std::runtime_error("test_device_primitives: writeImage 源长度与图像不符");
    auto hb = makeHostBuffer(fx, bytes, vk::BufferUsageFlagBits::eTransferSrc);
    std::memcpy(hb.mapped, src.data(), src.size());
    hb.memory.unmapMemory();  // 提交前解映射：不靠 vkFreeMemory 的隐式解映射

    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        // 建图时 initialLayout = Undefined ⟹ 第一次使用前必须从 Undefined 转出
        vk::ImageMemoryBarrier2 to{};
        to.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
        to.srcAccessMask = vk::AccessFlagBits2::eNone;
        to.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        to.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
        setBarrierTarget(to, img.image);
        to.oldLayout = vk::ImageLayout::eUndefined;
        to.newLayout = vk::ImageLayout::eTransferDstOptimal;
        vk::DependencyInfo dep{};
        dep.setImageMemoryBarriers(to);
        cmd.pipelineBarrier2(dep);

        // **bufferRowLength / bufferImageHeight 必须显式给**。这正是本用例要钉的东西
        // 之一：行序假设错了（llm_debug 的读回同理）会让行与行错位，而错位后的图
        // 「大体对、细节偏」，肉眼几乎发现不了。
        vk::BufferImageCopy region{};
        region.bufferRowLength = img.width;
        region.bufferImageHeight = img.height;
        region.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{img.width, img.height, 1};
        cmd.copyBufferToImage(*hb.buffer, img.image, vk::ImageLayout::eTransferDstOptimal, region);

        // 收尾复原成 eGeneral（与 readImage 的调用约定对齐）
        vk::ImageMemoryBarrier2 back{};
        back.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        back.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
        back.dstStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
        back.dstAccessMask = vk::AccessFlagBits2::eNone;
        setBarrierTarget(back, img.image);
        back.oldLayout = vk::ImageLayout::eTransferDstOptimal;
        back.newLayout = vk::ImageLayout::eGeneral;
        vk::DependencyInfo dep2{};
        dep2.setImageMemoryBarriers(back);
        cmd.pipelineBarrier2(dep2);
    });
}

std::vector<uint8_t> readImage(const Fixture& fx, const Image2D& img) {
    const VkDeviceSize bytes =
        static_cast<VkDeviceSize>(img.width) * img.height * bytesPerPixel(img.format);
    auto hb = makeHostBuffer(fx, bytes, vk::BufferUsageFlagBits::eTransferDst);
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        vk::ImageMemoryBarrier2 to{};
        to.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
        to.srcAccessMask = vk::AccessFlagBits2::eNone;
        to.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        to.dstAccessMask = vk::AccessFlagBits2::eTransferRead;
        setBarrierTarget(to, img.image);
        to.oldLayout = vk::ImageLayout::eGeneral;
        to.newLayout = vk::ImageLayout::eTransferSrcOptimal;
        vk::DependencyInfo dep{};
        dep.setImageMemoryBarriers(to);
        cmd.pipelineBarrier2(dep);

        vk::BufferImageCopy region{};
        region.bufferRowLength = img.width;  // 显式给（见 writeImage 里的注）
        region.bufferImageHeight = img.height;
        region.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1};
        region.imageExtent = vk::Extent3D{img.width, img.height, 1};
        cmd.copyImageToBuffer(img.image, vk::ImageLayout::eTransferSrcOptimal, *hb.buffer, region);

        // **复原成 eGeneral**：本函数的契约是「可重复调用」。漏了这条，第二次读会因
        // 实际布局与命令里声明的 oldLayout 不符而拿垃圾或触发 validation —— 而单次
        // 读的用例永远看不出它有没有。配对用例见接口 TU「连续读两次」那条。
        vk::ImageMemoryBarrier2 back{};
        back.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
        back.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
        back.dstStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
        back.dstAccessMask = vk::AccessFlagBits2::eNone;
        setBarrierTarget(back, img.image);
        back.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        back.newLayout = vk::ImageLayout::eGeneral;
        vk::DependencyInfo dep2{};
        dep2.setImageMemoryBarriers(back);
        cmd.pipelineBarrier2(dep2);
    });
    const auto* p = static_cast<const uint8_t*>(hb.mapped);
    std::vector<uint8_t> out(p, p + bytes);
    hb.memory.unmapMemory();
    return out;
}

Image2D cleared(const Fixture& fx, vk::Format fmt, uint32_t w, uint32_t h,
    const vk::ClearColorValue& cc) {
    auto img = makeImage(fx, fmt, w, h,
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst);
    fx.submitAndWait([&](vk::raii::CommandBuffer& cmd) {
        // vkCmdClearColorImage 要求目标处于 GENERAL 或 TRANSFER_DST_OPTIMAL；
        // 这里用 GENERAL（与 readImage/writeImage 的约定一致）。
        vk::ImageMemoryBarrier2 to{};
        to.srcStageMask = vk::PipelineStageFlagBits2::eTopOfPipe;
        to.srcAccessMask = vk::AccessFlagBits2::eNone;
        to.dstStageMask = vk::PipelineStageFlagBits2::eTransfer;
        to.dstAccessMask = vk::AccessFlagBits2::eTransferWrite;
        setBarrierTarget(to, img.image);
        to.oldLayout = vk::ImageLayout::eUndefined;
        to.newLayout = vk::ImageLayout::eGeneral;
        vk::DependencyInfo dep{};
        dep.setImageMemoryBarriers(to);
        cmd.pipelineBarrier2(dep);

        const vk::ImageSubresourceRange range(
            vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
        cmd.clearColorImage(img.image, vk::ImageLayout::eGeneral, cc, range);
    });
    return img;
}

}  // namespace RGL::testing::gpu
