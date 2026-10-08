// FSR 3.1.4 Upscaler / Vulkan —— **合成靶实验台**（Phase 14 批 14.5 诊断工具）
//
// 要回答的问题：时域档（temporal）画面发糊，消融已把糊**全部**归因到 accumulate pass
// 的历史读取（ReprojectHistoryColor → HistorySample{,16}），但按 vendor 源码数学，
// 1:1 + 零运动 + 零抖动时 fPxFrac 应为 0 ⟹ Lanczos2 权重应为 delta ⟹ 历史读取应为
// **恒等**。恒等与实测矛盾，渲染器里读权重被色彩链污染读不干净 ⟹ 本工具把 FSR 放到
// 一个没有色彩链的台子上：输入图由本工具分配并喂已知靶，每帧回读输出图
// （enableSharpening=false ⟹ vendor 走 pipelineAccumulate，accumulate 直接
// StoreUpscaledOutput ⟹ **输出图 == 历史图**）。
//
// 三条判据（缺一条即工具失效）：
//   ① 恒等锚点：delta 靶 + 时域档 + N 帧。若历史读取恒等，输出应收敛到输入
//      （中心纹素能量占比 → 1，二阶矩 σ → 0）。演化表逐帧打印。
//   ② 反向用例：原设计为「vendor 打同纹素直取补丁后重跑，σ 必须回 ~0」。**该前提已被
//      本工具的 D1 实测推翻**（历史读取本来就恒等），故 ② 改由「D1 采样器指纹 +
//      D5 四路分解」承担——即先证明"测的是它声称在测的东西"，再谈归因（详见下方结论块）。
//   ③ 可观测性：--manifest/--expect-sha 给 blobs/manifest.txt 的 SHA1 对账，
//      且本二进制 mtime 必须新于 manifest（产物比 blob 旧 = 改了个空操作旋钮）。
//      不满足直接拒绝继续（exit 4）。
//
// 诊断 shader 补丁（临时，收尾必须 git checkout 复原 + 重生成 blobs）把输出图变成
// 诊断图，本工具用 --stats / --dump-row 把原始 float 读出来。**已跑完的轮次与结论**
// （全部 1:1 / 256² / jitter=0 / mv=0 / 深度常量 / frameTimeDelta 钉死 / 仅第 0 帧 reset）：
//
//   F  采样器指纹：float 路径写 (11,12,13,1)、Min16 写 (21,22,23,2)、else 分支写 −7
//      ⟹ 实测输出 = (21,22,23,2) ⟹ **运行时实例化的是 Min16（FFX_HALF=1）**，与
//         caps.fp16Supported=false 的推导冲突（生成器已排除：非-16bit 表 0/12 含
//         OpTypeFloat 16，16bit 表 12/12 含）。未结案。
//   D1 fPxFrac 实测 = **(0, 0)**（每个内部像素）⟹ 历史读取的采样坐标**完全恒等**，
//      而 half 版 Lanczos2 在 t=0 处权重 = (0,1,0,0) ⟹ **历史读取不是糊的来源**。
//   D5 四路分解（当前帧贡献 / 原始历史 / 整流后历史 / 最终混合）：
//        · 当前帧贡献 fUpsampledColor：帧≥1 = **像素级精确锐利**（0/0.9995，均值 0.4998）；
//          **帧 0 = 0.1615–0.5645、均值 0.363**（对比度 −60%、能量 −27%）
//        · 整流后历史 == 原始历史（逐位相同）⟹ **RectifyHistory 一帧都没触发**
//        · 最终混合 ≈ 原始历史 ⟹ 混合系数极小，画面被历史冻住
//   D6 权重链：fAccumulation 0→0.333→0.667→1.0（= host 的 accumulationAddedPerFrame=1/3，
//        3 帧饱和）；fUpsampledWeight 帧 0 = 1.0、帧≥1 = **0.0231**
//        （= fAverageLanczosWeightPerFrame 0.74/16=0.04625 × Σ权重 0.5）
//        ⟹ **fAlpha = 0.0231/(1+0.0231) = 0.0226 恒定**（帧 0 则 = 1.0）。
//
// 一句话根因（本台子量出来的）：糊不来自历史重采样，而来自**权重链**——第 0 帧因
// accumulation mask=0 走 bIsInitialSample 分支、拿 3×3 box 中心当"上采样结果"（糊且暗），
// 且该帧 alpha=1 把它原样写进历史；此后 alpha 恒 2.3%，当前帧的锐利贡献基本进不来，
// 再叠加 HDR Tonemap/InverseTonemap 往返非恒等逐帧压高光 ⟹ 平衡点远低于锐利信号。
//
// 判据 ②（消融 C 同纹素直取 ⟹ σ→0）**在本台子上前提已失效**：D1 已直接量到历史读取
// 恒等（frac=0、权重 delta），同纹素直取必然改变不了任何东西。故该反向用例不再作为
// 工具有效性的判据，改由「D1 指纹 + D5 四路分解」承担同一职责（见文件头结论块）。
//
// 闭式对照（独立第二实现，全部 CPU float32）：
//   · 零运动 1:1 时 fPxSample 的逐纹素精确偏差 δ 的上下界（float 舍入的可测预言）
//   · 128 纹素 R16_SNORM Lanczos LUT + 线性 clamp 采样的逐 t 权重表
//
// 不进 rgl、不进 just build 运行路径；`just fsr-lab` 单独拉。样板抄 fsr_smoke.cpp。
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef FFX_LAB_VULKAN_API_VERSION
#define FFX_LAB_VULKAN_API_VERSION VK_API_VERSION_1_3
#endif

namespace {

void log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf("[fsr-lab] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    std::fflush(stdout);
    va_end(ap);
}

[[noreturn]] void die(const char* what, VkResult r = VK_ERROR_UNKNOWN)
{
    std::fprintf(stderr, "[fsr-lab] FAIL: %s (VkResult=%d)\n", what, (int)r);
    std::exit(2);
}

void check(VkResult r, const char* what)
{
    if (r != VK_SUCCESS) die(what, r);
}

void check(FfxErrorCode r, const char* what)
{
    if (r != FFX_OK)
    {
        std::fprintf(stderr, "[fsr-lab] FAIL: %s (FfxErrorCode=%d)\n", what, (int)r);
        std::exit(2);
    }
}

// ── SHA1（stdlib-only；判据 ③ 用，不引外部依赖）─────────────────────────────────
struct Sha1
{
    uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    uint64_t len = 0;
    uint8_t  buf[64] = {};
    size_t   fill = 0;

    static uint32_t rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

    void block(const uint8_t* p)
    {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                   (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;
            if      (i < 20) { f = (b & c) | (~b & d);       k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                k = 0xCA62C1D6u; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const uint8_t* p, size_t n)
    {
        len += n;
        while (n)
        {
            const size_t take = std::min(n, size_t(64) - fill);
            std::memcpy(buf + fill, p, take);
            fill += take; p += take; n -= take;
            if (fill == 64) { block(buf); fill = 0; }
        }
    }

    std::string hex()
    {
        const uint64_t bits = len * 8;
        const uint8_t pad = 0x80;
        update(&pad, 1);
        const uint8_t zero = 0;
        while (fill != 56) update(&zero, 1);
        uint8_t lb[8];
        for (int i = 0; i < 8; ++i) lb[i] = uint8_t(bits >> (56 - 8 * i));
        update(lb, 8);
        char out[41] = {};
        for (int i = 0; i < 5; ++i)
            std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

bool fileSha1(const std::string& path, std::string& out)
{
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    Sha1 s;
    uint8_t tmp[65536];
    size_t n;
    while ((n = std::fread(tmp, 1, sizeof(tmp), f)) > 0) s.update(tmp, n);
    std::fclose(f);
    out = s.hex();
    return true;
}

long long fileMtime(const std::string& path)
{
#if defined(__linux__)
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return -1;
    return (long long)st.st_mtime;
#else
    (void)path; return -1;
#endif
}

std::string selfExePath()
{
#if defined(__linux__)
    char buf[4096] = {};
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; return buf; }
#endif
    return {};
}

// ── VVL 记账（白名单与 fsr_smoke 同源，根因 → third_party/fsr/PATCHES.md）───────
struct VvlLedger
{
    static constexpr int kKnownCount = 3;
    static const char* const kKnown[kKnownCount];
    int total = 0;
    int unexpected = 0;
    int hits[kKnownCount] = {};
};
const char* const VvlLedger::kKnown[VvlLedger::kKnownCount] = {
    "VUID-vkAllocateMemory-deviceCoherentMemory-02790",
    "VUID-VkImageSubresourceRange-layerCount-01721",
    "VUID-vkDestroyDevice-device-05137",
};
VvlLedger g_vvl;

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        return VK_FALSE;
    ++g_vvl.total;
    for (int i = 0; i < VvlLedger::kKnownCount; ++i)
        if (std::strstr(d->pMessage, VvlLedger::kKnown[i])) { ++g_vvl.hits[i]; return VK_FALSE; }
    ++g_vvl.unexpected;
    std::fprintf(stderr, "[vvl] %s\n", d->pMessage);
    return VK_FALSE;
}

// ── 极小 Vulkan 脚手架（fsr_smoke.cpp 同款）────────────────────────────────────
struct VkCtx
{
    VkInstance               instance  = VK_NULL_HANDLE;
    VkPhysicalDevice         phys      = VK_NULL_HANDLE;
    VkDevice                 device    = VK_NULL_HANDLE;
    uint32_t                 queueFamily = ~0u;
    VkQueue                  queue     = VK_NULL_HANDLE;
    VkCommandPool            pool      = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures2 features2 = {};
    bool               validation = false;
};

VkFormat toVkFormat(FfxSurfaceFormat f)
{
    switch (f)
    {
    case FFX_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32G32_FLOAT: return VK_FORMAT_R32G32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_SURFACE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case FFX_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case FFX_SURFACE_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default:
        std::fprintf(stderr, "[fsr-lab] 未映射的 FfxSurfaceFormat=%d\n", (int)f);
        std::exit(3);
    }
}

struct Image
{
    VkImage          image = VK_NULL_HANDLE;
    VkDeviceMemory   mem   = VK_NULL_HANDLE;
    VkFormat         fmt   = VK_FORMAT_UNDEFINED;
    FfxSurfaceFormat ffxFormat = FFX_SURFACE_FORMAT_UNKNOWN;
    uint32_t         w = 0, h = 0;
};

void createImage(VkCtx& c, Image& out, uint32_t w, uint32_t h, FfxSurfaceFormat ffxFmt,
                 VkImageUsageFlags usage)
{
    const VkFormat fmt = toVkFormat(ffxFmt);
    out.ffxFormat = ffxFmt;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.fmt = fmt;
    out.w = w;
    out.h = h;
    check(vkCreateImage(c.device, &ci, nullptr, &out.image), "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    bool found = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount && !found; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        { ai.memoryTypeIndex = i; found = true; }
    if (!found) die("no DEVICE_LOCAL memory type");
    check(vkAllocateMemory(c.device, &ai, nullptr, &out.mem), "vkAllocateMemory");
    check(vkBindImageMemory(c.device, out.image, out.mem, 0), "vkBindImageMemory");

    // UNDEFINED → GENERAL（fsr_smoke 同款一次性推进；FSR 全程 GENERAL）
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "barrier cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "barrier begin");
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = out.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    check(vkEndCommandBuffer(cb), "barrier end");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "barrier submit");
    check(vkQueueWaitIdle(c.queue), "barrier wait");
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
}

/** 真实深度格式图（D32_SFLOAT + DEPTH aspect），与渲染器口径一致（fsr_smoke 探针 4）。*/
void createDepthImage(VkCtx& c, Image& out, uint32_t w, uint32_t h)
{
    out.ffxFormat = FFX_SURFACE_FORMAT_R32_FLOAT;   // 申报口径（配 DEPTHTARGET 映射成 D32）
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_D32_SFLOAT;
    ci.extent = {w, h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    out.fmt = ci.format;
    out.w = w;
    out.h = h;
    check(vkCreateImage(c.device, &ci, nullptr, &out.image), "vkCreateImage(depth)");
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    bool found = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount && !found; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        { ai.memoryTypeIndex = i; found = true; }
    if (!found) die("no DEVICE_LOCAL memory type (depth)");
    check(vkAllocateMemory(c.device, &ai, nullptr, &out.mem), "vkAllocateMemory(depth)");
    check(vkBindImageMemory(c.device, out.image, out.mem, 0), "vkBindImageMemory(depth)");

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer tcb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &tcb), "depth barrier cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(tcb, &bi), "depth barrier begin");
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = out.image;
    b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(tcb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    check(vkEndCommandBuffer(tcb), "depth barrier end");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &tcb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "depth barrier submit");
    check(vkQueueWaitIdle(c.queue), "depth barrier wait");
    vkFreeCommandBuffers(c.device, c.pool, 1, &tcb);
}

// ── half ⇄ float（fsr_smoke 的解码 + 本工具需要的编码）─────────────────────────
float halfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t man = h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (man == 0) bits = sign;
        else {
            uint32_t e = 0, m = man;
            while (!(m & 0x400u)) { m <<= 1; ++e; }
            m &= 0x3FFu;
            bits = sign | ((127u - 15u - e) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

uint16_t floatToHalf(float f)
{
    uint32_t b = 0;
    std::memcpy(&b, &f, sizeof(b));
    const uint32_t sign = (b >> 16) & 0x8000u;
    int32_t  exp  = int32_t((b >> 23) & 0xFFu) - 127 + 15;
    uint32_t man  = b & 0x7FFFFFu;
    if (((b >> 23) & 0xFFu) == 0xFFu) return uint16_t(sign | 0x7C00u | (man ? 0x200u : 0u));
    if (exp >= 0x1F) return uint16_t(sign | 0x7C00u);
    if (exp <= 0) {
        if (exp < -10) return uint16_t(sign);
        man |= 0x800000u;
        const int shift = 14 - exp;
        uint32_t half = man >> shift;
        if ((man >> (shift - 1)) & 1) ++half;
        return uint16_t(sign | half);
    }
    uint32_t half = (uint32_t(exp) << 10) | (man >> 13);
    if (man & 0x1000u) ++half;              // round-to-nearest-even 的进位近似（靶值都是精确值）
    return uint16_t(sign | half);
}

// ── host 可见 buffer 小工具 ─────────────────────────────────────────────────────
struct HostBuffer
{
    VkBuffer       buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void*          map = nullptr;
};

HostBuffer createHostBuffer(VkCtx& c, VkDeviceSize bytes, VkBufferUsageFlags usage)
{
    HostBuffer hb{};
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = usage;
    check(vkCreateBuffer(c.device, &bci, nullptr, &hb.buf), "host vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, hb.buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    uint32_t mt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        { mt = i; break; }
    if (mt == 0xFFFFFFFFu) die("需要 host-visible coherent 内存类型");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    check(vkAllocateMemory(c.device, &mai, nullptr, &hb.mem), "host vkAllocateMemory");
    check(vkBindBufferMemory(c.device, hb.buf, hb.mem, 0), "host vkBindBufferMemory");
    check(vkMapMemory(c.device, hb.mem, 0, VK_WHOLE_SIZE, 0, &hb.map), "host vkMapMemory");
    return hb;
}

void destroyHostBuffer(VkCtx& c, const HostBuffer& hb)
{
    vkUnmapMemory(c.device, hb.mem);
    vkDestroyBuffer(c.device, hb.buf, nullptr);
    vkFreeMemory(c.device, hb.mem, nullptr);
}

// 把 RGBA float 图上传进 GENERAL 的 RGBA16F 图（一次 submit + wait，调用方保证无在途）。
void uploadRgba16f(VkCtx& c, const Image& im, const std::vector<float>& rgba)
{
    const VkDeviceSize bytes = VkDeviceSize(im.w) * im.h * 8;
    HostBuffer hb = createHostBuffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* dst = reinterpret_cast<uint16_t*>(hb.map);
    for (size_t i = 0; i < size_t(im.w) * im.h * 4; ++i)
        dst[i] = floatToHalf(rgba[i]);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "upload cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "upload begin");
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {im.w, im.h, 1};
    vkCmdCopyBufferToImage(cb, hb.buf, im.image, VK_IMAGE_LAYOUT_GENERAL, 1, &reg);
    check(vkEndCommandBuffer(cb), "upload end");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "upload submit");
    check(vkQueueWaitIdle(c.queue), "upload wait");
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
    destroyHostBuffer(c, hb);
}

void uploadDepthConstant(VkCtx& c, const Image& im, float value)
{
    const VkDeviceSize bytes = VkDeviceSize(im.w) * im.h * 4;
    HostBuffer hb = createHostBuffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    auto* dst = reinterpret_cast<float*>(hb.map);
    for (size_t i = 0; i < size_t(im.w) * im.h; ++i) dst[i] = value;

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "depth upload cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "depth upload begin");
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    reg.imageExtent = {im.w, im.h, 1};
    vkCmdCopyBufferToImage(cb, hb.buf, im.image, VK_IMAGE_LAYOUT_GENERAL, 1, &reg);
    check(vkEndCommandBuffer(cb), "depth upload end");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "depth upload submit");
    check(vkQueueWaitIdle(c.queue), "depth upload wait");
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
    destroyHostBuffer(c, hb);
}

void clearImageTo(VkCtx& c, const Image& im, VkClearColorValue v)
{
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "clear cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "clear begin");
    VkImageSubresourceRange r{};
    r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.levelCount = 1;
    r.layerCount = 1;
    vkCmdClearColorImage(cb, im.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
    check(vkEndCommandBuffer(cb), "clear end");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "clear submit");
    check(vkQueueWaitIdle(c.queue), "clear wait");
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
}

// 回读整张 RGBA16F 图 → 每像素 4 个 float（half 解码）。
std::vector<float> readBackRgba16f(VkCtx& c, const Image& im)
{
    const VkDeviceSize bytes = VkDeviceSize(im.w) * im.h * 8;
    HostBuffer hb = createHostBuffer(c, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "readback cb");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "readback begin");
    VkBufferImageCopy reg{};
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent = {im.w, im.h, 1};
    vkCmdCopyImageToBuffer(cb, im.image, VK_IMAGE_LAYOUT_GENERAL, hb.buf, 1, &reg);
    check(vkEndCommandBuffer(cb), "readback end");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "readback submit");
    check(vkQueueWaitIdle(c.queue), "readback wait");

    std::vector<float> out(size_t(im.w) * im.h * 4);
    const auto* src = reinterpret_cast<const uint16_t*>(hb.map);
    for (size_t i = 0; i < size_t(im.w) * im.h * 4; ++i) out[i] = halfToFloat(src[i]);
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
    destroyHostBuffer(c, hb);
    return out;
}

FfxResource wrap(const Image& im, FfxResourceUsage usage)
{
    FfxResourceDescription d{};
    d.type = FFX_RESOURCE_TYPE_TEXTURE2D;
    d.width = im.w;
    d.height = im.h;
    d.mipCount = 1;
    d.usage = usage;
    d.flags = FFX_RESOURCE_FLAGS_NONE;
    d.format = im.ffxFormat;
    FfxResource r = ffxGetResourceVK((void*)im.image, d, L"fsr_lab", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    r.description = d;
    return r;
}

// ── 合成靶 ─────────────────────────────────────────────────────────────────────
enum class Target { Delta, Step, Checker };
const char* targetName(Target t)
{
    switch (t) {
    case Target::Delta: return "delta";
    case Target::Step: return "step";
    case Target::Checker: return "checker";
    }
    return "?";
}

// 返回 RGBA float（alpha=1）。delta：全黑 + 正中心一个亮纹素（脉冲响应）。
std::vector<float> makeTarget(Target t, uint32_t w, uint32_t h)
{
    std::vector<float> img(size_t(w) * h * 4, 0.0f);
    const uint32_t cx = w / 2, cy = h / 2;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            float v = 0.0f;
            switch (t) {
            case Target::Delta:   v = (x == cx && y == cy) ? 1.0f : 0.0f; break;
            case Target::Step:    v = (x >= cx) ? 1.0f : 0.0f; break;
            case Target::Checker: v = ((x + y) & 1u) ? 1.0f : 0.0f; break;
            }
            float* p = &img[(size_t(y) * w + x) * 4];
            p[0] = p[1] = p[2] = v;
            p[3] = 1.0f;
        }
    return img;
}

// ── 指标 ───────────────────────────────────────────────────────────────────────
struct FrameMetrics
{
    int    frame = 0;
    double edgeEnergy = 0;    // mean |∇luma|（内部像素，|dx|+|dy|）
    double centerFrac = 0;    // delta：正 luma 能量落在中心纹素的比例
    double sigma = 0;         // delta：正 luma 加权二阶矩 σ（相对中心纹素，像素单位）
    double sigmaEdge = 0;     // step：σ = A/(√(2π)·max|dP/dx|)（erf 边缘的等效高斯 σ）
    double checkerC = 0;      // checker：Nyquist 分量幅值（输入 0/1 棋盘 = 0.5）
    double peak = 0;          // delta：中心纹素值（收敛后应 = 输入值 1）
};

double lumaAt(const std::vector<float>& px, size_t i)
{
    return 0.2126 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.0722 * px[i * 4 + 2];
}

FrameMetrics computeMetrics(Target t, const std::vector<float>& px, uint32_t w, uint32_t h,
                            int frame)
{
    FrameMetrics m{};
    m.frame = frame;
    // 边缘能量（内部像素）
    double gsum = 0.0;
    size_t gn = 0;
    for (uint32_t y = 1; y + 1 < h; ++y)
        for (uint32_t x = 1; x + 1 < w; ++x) {
            const size_t i = size_t(y) * w + x;
            const double L = lumaAt(px, i);
            gsum += std::fabs(lumaAt(px, i + 1) - L) + std::fabs(lumaAt(px, i + w) - L);
            ++gn;
        }
    m.edgeEnergy = gn ? gsum / double(gn) : 0.0;

    const uint32_t cx = w / 2, cy = h / 2;
    if (t == Target::Delta) {
        double e0 = 0, ex = 0, ey = 0, etot = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x) {
                const double L = lumaAt(px, size_t(y) * w + x);
                const double p = L > 0 ? L : 0;
                // 坑：x/cx 是 uint32_t，直接相减在左半屏下溢成 ~4.29e9（σ 曾因此报 1.5e9）。
                const double dx = double(x) - double(cx);
                const double dy = double(y) - double(cy);
                etot += p;
                ex += p * dx * dx;
                ey += p * dy * dy;
            }
        const size_t c = size_t(cy) * w + cx;
        m.centerFrac = etot > 0 ? std::max(lumaAt(px, c), 0.0) / etot : 0.0;
        m.sigma = etot > 0 ? std::sqrt((ex + ey) / etot) : 0.0;
        m.peak = lumaAt(px, c);
    }
    if (t == Target::Step) {
        // 行平均剖面 P(x)，测 max 正斜率 → 等效高斯 σ
        std::vector<double> prof(w, 0.0);
        for (uint32_t y = 2; y + 2 < h; ++y)
            for (uint32_t x = 0; x < w; ++x) prof[x] += lumaAt(px, size_t(y) * w + x);
        for (auto& v : prof) v /= double(h - 4);
        const double A = prof[w - 1] - prof[0];
        double slope = 0;
        for (uint32_t x = 0; x + 1 < w; ++x) slope = std::max(slope, prof[x + 1] - prof[x]);
        m.sigmaEdge = slope > 1e-9 ? A / (std::sqrt(2.0 * 3.14159265358979) * slope) : 1e9;
    }
    if (t == Target::Checker) {
        double s = 0;
        size_t n = 0;
        for (uint32_t y = 1; y + 1 < h; ++y)
            for (uint32_t x = 1; x + 1 < w; ++x) {
                const double sign = ((x + y) & 1u) ? 1.0 : -1.0;
                s += sign * lumaAt(px, size_t(y) * w + x);
                ++n;
            }
        m.checkerC = n ? s / double(n) : 0.0;
    }
    return m;
}

void printMetrics(const FrameMetrics& m, Target t)
{
    std::printf("  frame %3d  EE=%9.5f", m.frame, m.edgeEnergy);
    if (t == Target::Delta)
        std::printf("  center_frac=%8.5f  sigma=%8.4f  peak=%8.5f", m.centerFrac, m.sigma, m.peak);
    if (t == Target::Step)
        std::printf("  sigma_edge=%8.4f", m.sigmaEdge);
    if (t == Target::Checker)
        std::printf("  checkerC=%8.5f", m.checkerC);
    std::printf("\n");
    std::fflush(stdout);
}

// 每通道 min/max/mean（内部区域）——诊断 shader 的输出判读全靠它。
void printChannelStats(const std::vector<float>& px, uint32_t w, uint32_t h)
{
    double mn[4] = {1e30, 1e30, 1e30, 1e30}, mx[4] = {-1e30, -1e30, -1e30, -1e30}, sum[4] = {};
    size_t n = 0;
    for (uint32_t y = 1; y + 1 < h; ++y)
        for (uint32_t x = 1; x + 1 < w; ++x) {
            for (int ch = 0; ch < 4; ++ch) {
                const double v = px[(size_t(y) * w + x) * 4 + ch];
                mn[ch] = std::min(mn[ch], v);
                mx[ch] = std::max(mx[ch], v);
                sum[ch] += v;
            }
            ++n;
        }
    for (int ch = 0; ch < 4; ++ch)
        std::printf("  ch%d: min=%12.6f max=%12.6f mean=%12.6f\n", ch, mn[ch], mx[ch],
                    n ? sum[ch] / double(n) : 0.0);
}

void printRow(const std::vector<float>& px, uint32_t w, uint32_t h, uint32_t y, int lo, int hi)
{
    if (y >= h) return;
    std::printf("  row y=%u ch0 x=[%d,%d]:", y, lo, hi);
    for (int x = lo; x <= hi && x >= 0 && uint32_t(x) < w; ++x)
        std::printf(" %.6f", px[(size_t(y) * w + size_t(x)) * 4]);
    std::printf("\n");
}

// ── 闭式对照（独立第二实现，全部 CPU float32）─────────────────────────────────
// vendor 的 Lanczos2NoClamp（ffx_fsr3upscaler_sample.h:128-138）的 CPU 复刻。
float lanczos2Ref(float x)
{
    const float PI = 3.141592653589793f;
    const float eps = 6.10e-05f;   // FSR3UPSCALER_EPSILON
    const float ax = std::fabs(x);
    if (ax < eps) return 1.0f;
    return (std::sin(PI * x) / (PI * x)) * (std::sin(0.5f * PI * x) / (0.5f * PI * x));
}

// host 写入的 LUT（ffx_fsr3upscaler.cpp:540-547）：128 纹素 R16_SNORM，
// texel[i] = round(L(2i/127)·32767)/32767。
float lutTexel(int i)
{
    const float x = 2.0f * float(i) / 127.0f;
    const float y = lanczos2Ref(x);
    const float q = std::round(y * 32767.0f) / 32767.0f;   // R16_SNORM 量化
    return q;
}

// shader 侧 SampleLanczos2Weight（callbacks_glsl.h:735-742）：
// textureLod(linear-clamp, (x/2, 0.5)) ⟹ 纹素坐标 u·128−0.5，线性 + clamp。
float lutSampleRef(float x)
{
    const float coord = (x / 2.0f) * 128.0f - 0.5f;
    const float c = std::min(std::max(coord, 0.0f), 127.0f);
    const int i0 = int(std::floor(c));
    const int i1 = std::min(i0 + 1, 127);
    const float f = c - float(i0);
    return (1.0f - f) * lutTexel(i0) + f * lutTexel(i1);
}

// DeclareCustomTextureSample{,Min16} + Lanczos2LUT 的 4 个归一化权重（单轴）。
void weightsRef(float t, float w[4])
{
    w[0] = lutSampleRef(std::fabs(-1.0f - t));
    w[1] = lutSampleRef(std::fabs(-0.0f - t));
    w[2] = lutSampleRef(std::fabs(1.0f - t));
    w[3] = lutSampleRef(std::fabs(2.0f - t));
    const float s = w[0] + w[1] + w[2] + w[3];
    for (int i = 0; i < 4; ++i) w[i] /= s;
}

// 零运动 1:1 时 fPxSample 的逐纹素精确偏差 δ 的闭式（float32 逐步复刻 shader 数学）：
//   fHrUv = (n+0.5)/S;  fPxSample = fHrUv·S − 0.5;  δ = fPxSample − n
// 返回 [min,max]，并给 half 化后的 fPxFrac 的取值集合摘要。
void closedFormDelta(int S, double& dmin, double& dmax, double& fracHalfMin, double& fracHalfMax,
                     long& nNeg, long& nZero)
{
    dmin = 1e30; dmax = -1e30; fracHalfMin = 1e30; fracHalfMax = -1e30;
    nNeg = 0; nZero = 0;
    const float Sf = float(S);
    for (int n = 0; n < S; ++n) {
        const float uv = (float(n) + 0.5f) / Sf;
        const float px = uv * Sf - 0.5f;
        const double d = double(px) - double(n);
        dmin = std::min(dmin, d);
        dmax = std::max(dmax, d);
        if (d < 0) ++nNeg;
        if (d == 0) ++nZero;
        float fr = px - std::floor(px);          // ffxFract（float32）
        fr = halfToFloat(floatToHalf(fr));       // Min16 路径的 half 化
        fracHalfMin = std::min(fracHalfMin, double(fr));
        fracHalfMax = std::max(fracHalfMax, double(fr));
    }
}

void printClosedForm(uint32_t w, uint32_t h)
{
    std::printf("=== 闭式对照（CPU float32，独立第二实现）===\n");
    struct { const char* axis; uint32_t S; } axes[2] = {{"x", w}, {"y", h}};
    for (auto& a : axes) {
        double dmin, dmax, fhMin, fhMax;
        long nNeg = 0, nZero = 0;
        closedFormDelta(int(a.S), dmin, dmax, fhMin, fhMax, nNeg, nZero);
        std::printf("  δ(fPxSample−n) @%s S=%u: min=%.3e max=%.3e  (δ<0 的纹素 %ld/%u, δ==0 %ld)\n",
                    a.axis, a.S, dmin, dmax, nNeg, a.S, nZero);
        std::printf("      fPxFrac half 化后 ∈ [%.6f, %.6f]（Min16 路径实际吃的 frac）\n",
                    fhMin, fhMax);
    }
    std::printf("  Lanczos LUT 权重（归一化，t = fPxFrac）：\n");
    const float ts[] = {0.0f, 0.05f, 0.1f, 0.25f, 0.5f, 0.75f, 0.8f, 0.85f, 0.9f, 0.95f, 1.0f};
    for (float t : ts) {
        float wq[4];
        weightsRef(t, wq);
        std::printf("    t=%5.2f  w=(%9.6f, %9.6f, %9.6f, %9.6f)\n", t, wq[0], wq[1], wq[2], wq[3]);
    }
    std::fflush(stdout);
}

// ── 实验执行 ───────────────────────────────────────────────────────────────────
struct LabArgs
{
    uint32_t w = 256, h = 256;
    int frames = 60;
    bool temporal = true;   // 默认两种模式全跑（--mode 单选）
    bool spatial = true;
    bool runDelta = true, runStep = true, runChecker = true;
    bool stats = false;         // 诊断模式：打印每通道 min/max/mean + 行数据
    int  dumpRowY = -1;
    int  dumpRowLo = -3, dumpRowHi = 3;   // 相对中心的 x 窗口
    std::string manifest = "third_party/fsr/blobs/manifest.txt";
    std::string lib = "build/third_party/fsr/libffx_fsr3upscaler_vk.a";
    std::string expectSha;
    bool force = false;
};

struct Experiment
{
    Target target;
    bool   temporal;   // false = 每帧 reset（纯空间档）
};

void runExperiment(VkCtx& c, const FfxInterface& backend, const LabArgs& args,
                   const Experiment& exp)
{
    const uint32_t W = args.w, H = args.h;
    log("─── exp target=%s mode=%s size=%ux%u frames=%d ───", targetName(exp.target),
        exp.temporal ? "temporal" : "spatial", W, H, args.frames);

    FfxFsr3UpscalerContextDescription cd{};
    // 与渲染器一致（rgl/fsr_upscaler_impl.cppm:239）：只开 HDR。
    cd.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
    cd.maxRenderSize = {W, H};
    cd.maxUpscaleSize = {W, H};
    cd.backendInterface = backend;
    static FfxFsr3UpscalerContext ctx;   // ~1 MiB，别放栈上
    check(ffxFsr3UpscalerContextCreate(&ctx, &cd), "ContextCreate");

    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    check(ffxFsr3UpscalerGetSharedResourceDescriptions(&ctx, &shared), "GetSharedResourceDescriptions");

    const VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags suSrc = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    Image color, depth, motion, output, dilatedDepth, dilatedMv, reconDepth;
    createImage(c, color, W, H, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, suSrc);
    createDepthImage(c, depth, W, H);
    createImage(c, motion, W, H, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, suSrc);  // 渲染器同款 RGBA16F
    createImage(c, output, W, H, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, su);
    auto createShared = [&](Image& im, const FfxCreateResourceDescription& want) {
        if (want.resourceDescription.type != FFX_RESOURCE_TYPE_TEXTURE2D) die("共享图不是 TEXTURE2D");
        createImage(c, im, want.resourceDescription.width, want.resourceDescription.height,
                    want.resourceDescription.format, su);
    };
    createShared(dilatedDepth, shared.dilatedDepth);
    createShared(dilatedMv, shared.dilatedMotionVectors);
    createShared(reconDepth, shared.reconstructedPrevNearestDepth);

    // 输入：靶 / 常量深度 / 零速度（与主 agent 渲染器实验同口径：mv=0、抖动=0、深度常量）
    uploadRgba16f(c, color, makeTarget(exp.target, W, H));
    uploadDepthConstant(c, depth, 0.5f);
    { VkClearColorValue z{}; clearImageTo(c, motion, z); }

    const FfxResource rColor = wrap(color, FFX_RESOURCE_USAGE_READ_ONLY);
    const FfxResource rDepth = wrap(depth, FfxResourceUsage(FFX_RESOURCE_USAGE_READ_ONLY |
                                                            FFX_RESOURCE_USAGE_DEPTHTARGET));
    const FfxResource rMotion = wrap(motion, FFX_RESOURCE_USAGE_READ_ONLY);
    const FfxResource rOutput = wrap(output, FFX_RESOURCE_USAGE_UAV);
    auto wrapShared = [&](const Image& im, const FfxCreateResourceDescription& want) {
        return wrap(im, FFX_RESOURCE_USAGE_UAV);
    };
    const FfxResource rDilatedDepth = wrapShared(dilatedDepth, shared.dilatedDepth);
    const FfxResource rDilatedMv = wrapShared(dilatedMv, shared.dilatedMotionVectors);
    const FfxResource rReconDepth = wrapShared(reconDepth, shared.reconstructedPrevNearestDepth);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "dispatch cb");

    std::vector<float> lastPx;
    for (int f = 0; f < args.frames; ++f) {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cb, &bi), "dispatch begin");

        FfxFsr3UpscalerDispatchDescription dd{};
        dd.commandList = ffxGetCommandListVK(cb);
        dd.color = rColor;
        dd.depth = rDepth;
        dd.motionVectors = rMotion;
        dd.exposure = FfxResource{};   // NULL → vendor 默认曝光 1.0（渲染器同款）
        dd.dilatedDepth = rDilatedDepth;
        dd.dilatedMotionVectors = rDilatedMv;
        dd.reconstructedPrevNearestDepth = rReconDepth;
        dd.output = rOutput;
        dd.jitterOffset = {0.0f, 0.0f};
        dd.motionVectorScale = {1.0f, 1.0f};
        dd.renderSize = {W, H};
        dd.upscaleSize = {W, H};
        dd.enableSharpening = false;   // ⟹ pipelineAccumulate，输出图 == 历史图
        dd.sharpness = 0.0f;
        dd.frameTimeDelta = 16.667f;   // 钉死，不用墙钟（时域滤波器时间常数）
        dd.preExposure = 1.0f;
        dd.reset = (f == 0) ? true : !exp.temporal;   // 时域档：只第 0 帧 reset
        dd.cameraNear = 0.1f;
        dd.cameraFar = 1000.0f;
        dd.cameraFovAngleVertical = 1.0f;
        dd.viewSpaceToMetersFactor = 0.0f;

        check(ffxFsr3UpscalerContextDispatch(&ctx, &dd), "ContextDispatch");
        check(vkEndCommandBuffer(cb), "dispatch end");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        check(vkQueueSubmit(c.queue, 1, &si, nullptr), "dispatch submit");
        check(vkQueueWaitIdle(c.queue), "dispatch wait");

        lastPx = readBackRgba16f(c, output);
        const bool show = args.stats
            ? (f >= std::max(args.frames - 3, 0) || f == args.frames / 2)
            : (f < 12 || f % 10 == 0 || f == args.frames - 1);
        if (show) printMetrics(computeMetrics(exp.target, lastPx, W, H, f), exp.target);
    }

    // 恒等锚点的字节判据：delta 靶收敛后输出 vs 输入（half 位级）。
    if (exp.target == Target::Delta && exp.temporal) {
        const std::vector<float> in = makeTarget(exp.target, W, H);
        size_t diffBits = 0, diffEps = 0;
        double maxAbs = 0;
        for (size_t i = 0; i < size_t(W) * H; ++i) {
            for (int ch = 0; ch < 3; ++ch) {
                const uint16_t a = floatToHalf(in[i * 4 + ch]);
                const uint16_t b = floatToHalf(lastPx[i * 4 + ch]);
                if (a != b) ++diffBits;
                const double d = std::fabs(double(in[i * 4 + ch]) - double(lastPx[i * 4 + ch]));
                maxAbs = std::max(maxAbs, d);
                if (d > 1e-3) ++diffEps;
            }
        }
        const FrameMetrics fm = computeMetrics(exp.target, lastPx, W, H, args.frames - 1);
        std::printf("  ANCHOR delta/temporal: 输出 vs 输入 位级差异纹素=%zu/%zu (|d|>1e-3: %zu) "
                    "max|d|=%.6f  末帧 center_frac=%.6f sigma=%.4f peak=%.6f\n",
                    diffBits, size_t(W) * H * 3, diffEps, maxAbs, fm.centerFrac, fm.sigma, fm.peak);
    }

    if (args.stats) {
        std::printf("  末帧每通道统计（内部区域）：\n");
        printChannelStats(lastPx, W, H);
    }
    if (args.dumpRowY >= 0) {
        const int cx = int(W / 2);
        printRow(lastPx, W, H, uint32_t(args.dumpRowY), cx + args.dumpRowLo, cx + args.dumpRowHi);
        printRow(lastPx, W, H, uint32_t(args.dumpRowY), 1, 6);           // 左边缘窗口
        printRow(lastPx, W, H, uint32_t(args.dumpRowY), int(W) - 7, int(W) - 2);  // 右边缘窗口
    }
    std::fflush(stdout);

    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
    ffxFsr3UpscalerContextDestroy(&ctx);
    vkDeviceWaitIdle(c.device);
    auto destroyImg = [&](Image& im) {
        vkDestroyImage(c.device, im.image, nullptr);
        vkFreeMemory(c.device, im.mem, nullptr);
    };
    destroyImg(color); destroyImg(depth); destroyImg(motion); destroyImg(output);
    destroyImg(dilatedDepth); destroyImg(dilatedMv); destroyImg(reconDepth);
}

}  // namespace

int main(int argc, char** argv)
{
    LabArgs args;
    for (int i = 1; i < argc; ++i) {
        const auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "缺参数: %s\n", what); std::exit(2); }
            return argv[++i];
        };
        if (!std::strcmp(argv[i], "--size")) {
            unsigned wv = 0, hv = 0;
            if (std::sscanf(next("--size"), "%ux%u", &wv, &hv) != 2 || !wv || !hv) die("--size WxH");
            args.w = wv; args.h = hv;
        } else if (!std::strcmp(argv[i], "--frames"))
            args.frames = std::atoi(next("--frames"));
        else if (!std::strcmp(argv[i], "--target")) {
            const char* t = next("--target");
            args.runDelta = args.runStep = args.runChecker = false;
            if (!std::strcmp(t, "delta")) args.runDelta = true;
            else if (!std::strcmp(t, "step")) args.runStep = true;
            else if (!std::strcmp(t, "checker")) args.runChecker = true;
            else die("--target ∈ delta|step|checker");
        } else if (!std::strcmp(argv[i], "--mode")) {
            const char* m = next("--mode");
            if (!std::strcmp(m, "temporal")) args.spatial = false;
            else if (!std::strcmp(m, "spatial")) args.temporal = false;
            else if (!std::strcmp(m, "both")) {}
            else die("--mode ∈ temporal|spatial|both");
        } else if (!std::strcmp(argv[i], "--stats")) args.stats = true;
        else if (!std::strcmp(argv[i], "--dump-row")) {
            args.dumpRowY = std::atoi(next("--dump-row"));
            // 窗口形如 "-3:3"（本身以 '-' 开头，不能按前缀判断），解析成功才吞掉
            if (i + 1 < argc &&
                std::sscanf(argv[i + 1], "%d:%d", &args.dumpRowLo, &args.dumpRowHi) == 2)
                ++i;
        } else if (!std::strcmp(argv[i], "--manifest")) args.manifest = next("--manifest");
        else if (!std::strcmp(argv[i], "--lib")) args.lib = next("--lib");
        else if (!std::strcmp(argv[i], "--expect-sha")) args.expectSha = next("--expect-sha");
        else if (!std::strcmp(argv[i], "--force")) args.force = true;
        else if (!std::strcmp(argv[i], "--help")) {
            std::printf(
                "fsr_lab —— FSR3-upscaler 合成靶实验台（判据与诊断补丁说明见文件头注释）\n"
                "  --size WxH        1:1 档尺寸（renderSize==upscaleSize），默认 256x256\n"
                "  --frames N         帧数，默认 60\n"
                "  --target T         delta|step|checker（默认全跑）\n"
                "  --mode M           temporal|spatial|both（默认 temporal+spatial 全跑）\n"
                "  --stats            末帧每通道 min/max/mean（诊断 shader 输出判读用）\n"
                "  --dump-row Y [lo:hi] 末帧第 Y 行 ch0 原始值（中心/左右边缘窗口）\n"
                "  --manifest PATH    blobs/manifest.txt（判据③），默认 third_party/fsr/blobs/manifest.txt\n"
                "  --lib PATH         libffx_fsr3upscaler_vk.a（新鲜度戳），默认 build/third_party/fsr/...\n"
                "  --expect-sha SHA1  manifest 的 SHA1 必须等于它，否则 exit 4 拒绝继续\n"
                "  --force            跳过判据③的新鲜度拒绝（SHA1 对账仍生效）\n");
            return 0;
        } else {
            std::fprintf(stderr, "未知参数: %s\n", argv[i]);
            return 2;
        }
    }

    // ── 判据 ③：可观测性断言（改了 shader 没重生成 / 产物比 blob 旧 = 拒绝继续）──
    std::string sha;
    if (fileSha1(args.manifest, sha)) {
        log("manifest %s sha1=%s", args.manifest.c_str(), sha.c_str());
        if (!args.expectSha.empty() && sha != args.expectSha) {
            std::fprintf(stderr,
                "[fsr-lab] FAIL(判据③): manifest sha1=%s ≠ 期望 %s —— blobs 不是你要测的那份，"
                "先 just fsr-blobs 再重建。拒绝继续。\n", sha.c_str(), args.expectSha.c_str());
            return 4;
        }
        const long long mMan = fileMtime(args.manifest);
        const std::string exe = selfExePath();
        const long long mExe = exe.empty() ? -1 : fileMtime(exe);
        const long long mLib = fileMtime(args.lib);
        log("新鲜度: manifest mtime=%lld 本二进制 mtime=%lld (%s) lib mtime=%lld",
            mMan, mExe, exe.c_str(), mLib);
        if (!args.force && (mExe < 0 || mMan < 0 || mExe < mMan || mLib < mMan)) {
            std::fprintf(stderr,
                "[fsr-lab] FAIL(判据③): 产物（二进制/静态库）mtime 早于 manifest —— 你改过 vendor "
                "但产物没重建。拒绝继续（--force 可跳过，但先想清楚你在测什么）。\n");
            return 4;
        }
    } else if (!args.expectSha.empty()) {
        std::fprintf(stderr, "[fsr-lab] FAIL(判据③): 读不到 manifest %s\n", args.manifest.c_str());
        return 4;
    } else {
        log("WARN: 读不到 manifest（%s），判据③未生效——只在纯 stock 观察时可接受", args.manifest.c_str());
    }

    printClosedForm(args.w, args.h);

    VkCtx c{};
    // ── instance（VVL 开）───────────────────────────────────────────────────────
    const char* iext[] = {"VK_KHR_surface", "VK_EXT_debug_utils"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "fsr_lab";
    app.apiVersion = FFX_LAB_VULKAN_API_VERSION;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VkDebugUtilsMessengerCreateInfoEXT mci{};
    mci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                      VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = debugCb;
    ici.pNext = &mci;
    const char* ilayers[] = {"VK_LAYER_KHRONOS_validation"};
    ici.enabledLayerCount = 1;
    ici.ppEnabledLayerNames = ilayers;
    const VkResult ir = vkCreateInstance(&ici, nullptr, &c.instance);
    if (ir != VK_SUCCESS) {
        ici.enabledLayerCount = 0;
        check(vkCreateInstance(&ici, nullptr, &c.instance), "vkCreateInstance (no validation)");
    } else
        c.validation = true;
    log("validation layer: %s", c.validation ? "ON" : "OFF");
    if (auto f = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(c.instance, "vkCreateDebugUtilsMessengerEXT")))
        f(c.instance, &mci, nullptr, &c.messenger);

    // ── device（选集成 GPU；能力链与 fsr_smoke 一致）────────────────────────────
    uint32_t pc = 0;
    check(vkEnumeratePhysicalDevices(c.instance, &pc, nullptr), "enumerate phys");
    std::vector<VkPhysicalDevice> pds(pc);
    vkEnumeratePhysicalDevices(c.instance, &pc, pds.data());
    if (pc == 0) die("no Vulkan device");
    c.phys = pds[0];
    for (auto d : pds) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        log("candidate: %s (type=%d)", p.deviceName, (int)p.deviceType);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) c.phys = d;
    }
    {
        uint32_t qc = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qc, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qc);
        vkGetPhysicalDeviceQueueFamilyProperties(c.phys, &qc, qs.data());
        for (uint32_t i = 0; i < qc; ++i)
            if (qs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { c.queueFamily = i; break; }
        if (c.queueFamily == ~0u) die("no compute queue family");
    }
    VkPhysicalDeviceDescriptorIndexingFeatures di{};
    di.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceShaderFloat16Int8Features f16{};
    f16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    VkPhysicalDevice16BitStorageFeatures b16{};
    b16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT ssc{};
    ssc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
    di.pNext = &f16;
    f16.pNext = &b16;
    b16.pNext = &ssc;
    c.features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    c.features2.pNext = &di;
    vkGetPhysicalDeviceFeatures2(c.phys, &c.features2);

    const char* dext[] = {"VK_EXT_subgroup_size_control", "VK_EXT_descriptor_indexing",
                          "VK_KHR_maintenance4"};
    uint32_t ec = 0;
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &ec, nullptr);
    std::vector<VkExtensionProperties> es(ec);
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &ec, es.data());
    std::vector<const char*> on;
    for (const char* want : dext)
        for (auto& e : es)
            if (!std::strcmp(e.extensionName, want)) { on.push_back(want); break; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = c.queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = (uint32_t)on.size();
    dci.ppEnabledExtensionNames = on.data();
    di.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    di.runtimeDescriptorArray = VK_TRUE;
    di.descriptorBindingPartiallyBound = VK_TRUE;
    f16.shaderFloat16 = VK_TRUE;
    b16.storageBuffer16BitAccess = VK_TRUE;
    ssc.subgroupSizeControl = VK_TRUE;
    ssc.computeFullSubgroups = VK_TRUE;
    dci.pNext = &c.features2;
    check(vkCreateDevice(c.phys, &dci, nullptr, &c.device), "vkCreateDevice");
    vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = c.queueFamily;
    check(vkCreateCommandPool(c.device, &cpi, nullptr, &c.pool), "vkCreateCommandPool");

    // ── FFX backend ─────────────────────────────────────────────────────────────
    const size_t scratchSize = ffxGetScratchMemorySizeVK(c.phys, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    void* scratch = std::malloc(scratchSize);
    VkDeviceContext vdc{c.device, c.phys, &vkGetDeviceProcAddr};
    FfxDevice dev = ffxGetDeviceVK(&vdc);
    FfxInterface backend{};
    check(ffxGetInterfaceVK(&backend, dev, scratch, scratchSize, FFX_FSR3UPSCALER_CONTEXT_COUNT),
          "ffxGetInterfaceVK");
    {
        FfxDeviceCapabilities caps{};
        check(backend.fpGetDeviceCapabilities(&backend, &caps), "fpGetDeviceCapabilities");
        const bool useLut = (caps.waveLaneCountMin == 32 && caps.waveLaneCountMax == 64);
        // 注意：这里只是**按 host 源码推导**的排列输入，**已被实测推翻**（见下 WARN）。
        // 实测方法：给 DeclareCustomTextureSample / …Min16 两条采样器各写不可混淆指纹
        // （float → (11,12,13,1)，Min16 → (21,22,23,2)，accumulate 的 else 分支 → −7），
        // 在 1:1 台子上读回输出图 = (21,22,23,2) ⟹ **实际实例化的是 Min16（FFX_HALF=1）**。
        // 离线对账同时排除了生成器串表：非-16bit 表 12 个模块 0 个含 OpTypeFloat 16，
        // 16bit 表 12/12 含 ⟹ 表没错，是 host 真的选了 16bit 排列（is16bit ← ALLOW_FP16 位）。
        // 也就是说 caps.fp16Supported 这里报 false，host 建管线时却按 true 走。未结案。
        log("permutation inputs(推导): shaderModel=%d wave=%u/%u fp16=%d ⟹ useLut=%d fp16Perm=%d",
            (int)caps.maximumSupportedShaderModel, caps.waveLaneCountMin, caps.waveLaneCountMax,
            (int)caps.fp16Supported, (int)useLut, (int)caps.fp16Supported);
        log("WARN 推导与实测冲突：推导说走 float 采样器，shader 指纹实测走 **Min16(half)**。"
            "读 fPxFrac/权重时以 Min16 那条为准（两条的 frac 计算式相同，结论不受影响）。");
    }

    std::vector<Experiment> exps;
    for (auto t : {Target::Delta, Target::Step, Target::Checker}) {
        if (t == Target::Delta && !args.runDelta) continue;
        if (t == Target::Step && !args.runStep) continue;
        if (t == Target::Checker && !args.runChecker) continue;
        if (args.temporal) exps.push_back({t, true});
        if (args.spatial) exps.push_back({t, false});
    }
    for (const auto& e : exps) runExperiment(c, backend, args, e);

    vkDeviceWaitIdle(c.device);
    if (c.messenger) {
        auto d = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(c.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (d) d(c.instance, c.messenger, nullptr);
    }
    vkDestroyDevice(c.device, nullptr);
    vkDestroyInstance(c.instance, nullptr);
    std::free(scratch);

    log("VVL messages: %d total, %d unexpected", g_vvl.total, g_vvl.unexpected);
    for (int i = 0; i < VvlLedger::kKnownCount; ++i)
        log("  known[%d] %-52s x%d", i, VvlLedger::kKnown[i], g_vvl.hits[i]);
    if (g_vvl.unexpected > 0) {
        std::fprintf(stderr, "[fsr-lab] FAIL: %d 条白名单外的 VVL 报错\n", g_vvl.unexpected);
        return 1;
    }
    log("PASS");
    return 0;
}
