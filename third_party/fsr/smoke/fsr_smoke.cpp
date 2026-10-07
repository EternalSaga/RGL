// FSR 3.1.4 Upscaler / Vulkan 后端 —— **最小设备 + 单 pass 冒烟**（批 14.1 的第一个里程碑）
//
// 为什么先做这个而不是整库编译：phase14_plan §5 风险 2 = 「1.1.4 的 VK 后端在 RADV / anv
// 上跑不起来」。这个问题只有真开一次设备、真创建一次 pipeline 才有答案，而它决定后面
// 所有批次值不值得投入。所以本程序只做一件事：
//
//   VkInstance → VkDevice → ffxGetInterfaceVK → ffxFsr3UpscalerContextCreate
//              → 建输入/输出图 → 录一条命令缓冲 → ContextDispatch ×2 帧 → 等 GPU
//
// 跑通 = 「vendor 的 SPIR-V 排列 + 自建 descriptor pool + pipeline 创建（含
// RequiredSubgroupSize 那条条件式）在本驱动上成立」。跑不通 = 当场拿到是哪一步、
// VVL 怎么说，代价是半天而不是两周。
//
// 不进 rgl、不进 just build 的运行路径；`just fsr-smoke` 单独拉。
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>
#if defined(__linux__)
#include <execinfo.h>
#endif

#ifndef FFX_SMOKE_VULKAN_API_VERSION
#define FFX_SMOKE_VULKAN_API_VERSION VK_API_VERSION_1_3
#endif

namespace {

void log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::printf("[fsr-smoke] ");
    std::vprintf(fmt, ap);
    std::printf("\n");
    std::fflush(stdout);
    va_end(ap);
}

[[noreturn]] void die(const char* what, VkResult r = VK_ERROR_UNKNOWN)
{
    std::fprintf(stderr, "[fsr-smoke] FAIL: %s (VkResult=%d)\n", what, (int)r);
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
        std::fprintf(stderr, "[fsr-smoke] FAIL: %s (FfxErrorCode=%d)\n", what, (int)r);
        std::exit(2);
    }
}

// ── 极小 Vulkan 脚手架 ────────────────────────────────────────────────────────
struct VkCtx
{
    VkInstance               instance      = VK_NULL_HANDLE;
    VkPhysicalDevice         phys          = VK_NULL_HANDLE;
    VkDevice                 device        = VK_NULL_HANDLE;
    uint32_t                 queueFamily   = ~0u;
    VkQueue                  queue         = VK_NULL_HANDLE;
    VkPipelineCache          cache         = VK_NULL_HANDLE;
    VkCommandPool            pool          = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger     = VK_NULL_HANDLE;
    VkPhysicalDeviceFeatures2 features2    = {};
    bool               validation = false;
};

// 崩在 vendor 里时 lldb 拿不到栈（跳进 0x0 就没帧可展），自己打一份。
// 装了就抢在调试器前面 _Exit，所以默认**不装**：`FSR_SMOKE_TRACE=1` 才启用。
void installCrashTrace()
{
    if (!std::getenv("FSR_SMOKE_TRACE")) return;
    std::signal(SIGSEGV, [](int) {
        void* frames[32] = {};
        const int n = backtrace(frames, 32);
        std::fprintf(stderr, "[fsr-smoke] SIGSEGV，调用栈：\n");
        backtrace_symbols_fd(frames, n, 2);
        std::_Exit(139);
    });
}

// ── VVL 记账 ──────────────────────────────────────────────────────────────────
// 冒烟的判据之一就是「VVL 干净」，但 AMD FSR 3.1.4 的 VK 后端在 RADV 上有若干类**上游自己**
// 的 spec 违规（根因逐条见 third_party/fsr/PATCHES.md §已知上游问题）。白名单不等于忽略：
// 它记的是「已知且已归因」，**任何不在表内的新报错都会让冒烟 FAIL**，这样上游升级或我们
// 改接线时新引入的问题不会静默溜过。计数会打印出来，白名单命中数变了也看得见。
struct VvlLedger
{
    // 特征子串取 VUID 或唯一短语，不取整句——上游改措辞时宁可失配报错，也不要静默漏过。
    // ⚠ **修掉一条就把它从表里删掉**：留着等于「修没修都绿」。历史上第 4 条
    //   "which doesn't match the VkImageView"（luma history 的 rgba8 vs R16G16B16A16F）
    //   已由 P11 修复并删除；若回归，它会以「白名单外」的形式把冒烟打红。
    // 表长写死：加条目忘了改 kKnownCount 是编译错误，而不是把 hits[] 写越界。
    static constexpr int kKnownCount = 3;
    static const char* const kKnown[kKnownCount];

    int total      = 0;
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
debugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        return VK_FALSE;
    ++g_vvl.total;
    for (int i = 0; i < VvlLedger::kKnownCount; ++i) {
        if (std::strstr(d->pMessage, VvlLedger::kKnown[i])) {
            ++g_vvl.hits[i];
            return VK_FALSE;
        }
    }
    ++g_vvl.unexpected;
    std::fprintf(stderr, "[vvl] %s\n", d->pMessage);
    return VK_FALSE;
}

// FfxSurfaceFormat → VkFormat。只需要 FSR3-upscaler 实际会要的那几个；命中不了
// 直接退出——静默退回 UNDEFINED 就是第一版 VVL 报一片的根因（image view 格式未定义
// → 所有 storage 特性位为 0 → 一串 VUID 连带炸）。
VkFormat toVkFormat(FfxSurfaceFormat f)
{
    switch (f)
    {
    case FFX_SURFACE_FORMAT_R32_FLOAT: return VK_FORMAT_R32_SFLOAT;
    case FFX_SURFACE_FORMAT_R32_UINT: return VK_FORMAT_R32_UINT;
    case FFX_SURFACE_FORMAT_R16G16_FLOAT: return VK_FORMAT_R16G16_SFLOAT;
    case FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case FFX_SURFACE_FORMAT_R8_UNORM: return VK_FORMAT_R8_UNORM;
    case FFX_SURFACE_FORMAT_R8_UINT: return VK_FORMAT_R8_UINT;
    case FFX_SURFACE_FORMAT_R16_FLOAT: return VK_FORMAT_R16_SFLOAT;
    case FFX_SURFACE_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case FFX_SURFACE_FORMAT_R11G11B10_FLOAT: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    default:
        std::fprintf(stderr, "[fsr-smoke] 未映射的 FfxSurfaceFormat=%d（去 toVkFormat 补）\n", (int)f);
        std::exit(3);
    }
}

struct Image
{
    VkImage        image = VK_NULL_HANDLE;
    VkDeviceMemory mem   = VK_NULL_HANDLE;
    VkFormat       fmt   = VK_FORMAT_UNDEFINED;
    FfxSurfaceFormat ffxFormat = FFX_SURFACE_FORMAT_UNKNOWN;
    uint32_t       w     = 0, h = 0;
};

void createImage(VkCtx& c, Image& out, uint32_t w, uint32_t h, FfxSurfaceFormat ffxFmt, VkImageUsageFlags usage)
{
    const VkFormat fmt = toVkFormat(ffxFmt);
    out.ffxFormat      = ffxFmt;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType      = VK_IMAGE_TYPE_2D;
    ci.format         = fmt;
    ci.extent         = {w, h, 1};
    ci.mipLevels      = 1;
    ci.arrayLayers    = 1;
    ci.samples        = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling         = VK_IMAGE_TILING_OPTIMAL;
    ci.usage          = usage;
    ci.sharingMode    = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    out.fmt = fmt;
    out.w   = w;
    out.h   = h;
    check(vkCreateImage(c.device, &ci, nullptr, &out.image), "vkCreateImage");

    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(c.device, out.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize  = req.size;
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    bool found = false;
    for (uint32_t i = 0; i < mp.memoryTypeCount && !found; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        {
            ai.memoryTypeIndex = i;
            found              = true;
        }
    if (!found) die("no DEVICE_LOCAL memory type");
    check(vkAllocateMemory(c.device, &ai, nullptr, &out.mem), "vkAllocateMemory");
    check(vkBindImageMemory(c.device, out.image, out.mem, 0), "vkBindImageMemory");

    // 一次性把布局推到 GENERAL：FSR 全程用 storage image / sampled image，GENERAL
    // 省掉冒烟里的布局簿记（真实接线由渲染图 + 14.4b 护栏管，不在这里预演）。
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool        = c.pool;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cb     = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask       = 0;
    b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image               = out.image;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "vkQueueSubmit(barrier)");
    check(vkQueueWaitIdle(c.queue), "vkQueueWaitIdle(barrier)");
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
}

// ── 契约探针用的两个小工具 ─────────────────────────────────────────────────────
// 把一张已经是 GENERAL 布局的图清成常量（源图喂已知值用）。
void clearImageTo(VkCtx& c, VkCommandBuffer cb, const Image& im, VkClearColorValue v)
{
    VkImageSubresourceRange r{};
    r.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    r.levelCount     = 1;
    r.layerCount     = 1;              // ⚠ 0 是非法值（vendor 自己就在 :4223 踩了这个，见 PATCHES.md）
    vkCmdClearColorImage(cb, im.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &r);
}

// IEEE 754 half → float。dilatedMotionVectors 是 R16G16_FLOAT，回读要自己解
// （只准 stdlib：不引 Half/half.hpp，也不假设 <bit> 里有 from_bits）。
float halfToFloat(uint16_t h)
{
    const uint32_t sign = uint32_t(h & 0x8000u) << 16;
    const uint32_t exp  = (h >> 10) & 0x1Fu;
    const uint32_t man  = h & 0x3FFu;
    uint32_t bits = 0;
    if (exp == 0) {
        if (man == 0) bits = sign;                       // ±0
        else {                                           // 次正规：规格化
            uint32_t e = 0;
            uint32_t m = man;
            while (!(m & 0x400u)) { m <<= 1; ++e; }
            m &= 0x3FFu;
            bits = sign | ((127u - 15u - e) << 23) | (m << 13);
        }
    } else if (exp == 0x1F) {
        bits = sign | 0x7F800000u | (man << 13);         // inf / nan
    } else {
        bits = sign | ((exp + 112u) << 23) | (man << 13);
    }
    float f = 0.0f;
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// 回读整张图到 CPU（调用方负责队列已 idle）。bpp = 每像素字节数，如 R16G16_FLOAT=4、
// R16G16B16A16_FLOAT=8。返回原始位模式，调用方按格式解（half 用上面的 halfToFloat）。
std::vector<uint8_t> readBackImage(VkCtx& c, const Image& im, uint32_t bpp)
{
    const VkDeviceSize bytes = VkDeviceSize(im.w) * im.h * bpp;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    check(vkCreateBuffer(c.device, &bci, nullptr, &buf), "回读 vkCreateBuffer");
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(c.device, buf, &req);
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(c.phys, &mp);
    uint32_t mt = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { mt = i; break; }
    if (mt == 0xFFFFFFFFu) die("回读需要 host-visible coherent 内存类型");
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = mt;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    check(vkAllocateMemory(c.device, &mai, nullptr, &mem), "回读 vkAllocateMemory");
    check(vkBindBufferMemory(c.device, buf, mem, 0), "vkBindBufferMemory");

    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = c.pool; cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cai.commandBufferCount = 1;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "回读 vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cb, &bi), "回读 vkBeginCommandBuffer");
    VkBufferImageCopy reg{};                       // bufferRowLength=0 ⟹ 行紧排（满足 4B 行对齐）
    reg.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    reg.imageExtent      = {im.w, im.h, 1};
    vkCmdCopyImageToBuffer(cb, im.image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &reg);
    check(vkEndCommandBuffer(cb), "回读 vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    check(vkQueueSubmit(c.queue, 1, &si, nullptr), "回读 vkQueueSubmit");
    check(vkQueueWaitIdle(c.queue), "回读 vkQueueWaitIdle");

    std::vector<uint8_t> out(bytes);
    void* p = nullptr;
    check(vkMapMemory(c.device, mem, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
    std::memcpy(out.data(), p, size_t(bytes));
    vkUnmapMemory(c.device, mem);
    vkDestroyBuffer(c.device, buf, nullptr);
    vkFreeMemory(c.device, mem, nullptr);
    vkFreeCommandBuffers(c.device, c.pool, 1, &cb);
    return out;
}

// 取第 index 个像素的第 ch 个 half 通道（行主序）。channels ∈ {2,4}。
inline float halfChannelAt(const std::vector<uint8_t>& px, size_t index, uint32_t channels, uint32_t ch)
{
    const uint16_t* h = reinterpret_cast<const uint16_t*>(px.data());
    return halfToFloat(h[index * channels + ch]);
}





// FfxSurfaceFormat → VkFormat 的映射在文件下方（toVkFormat），createImage 之前声明。
FfxResource wrap(const Image& im, FfxResourceUsage usage)
{
    FfxResourceDescription d{};
    d.type     = FFX_RESOURCE_TYPE_TEXTURE2D;
    d.width    = im.w;
    d.height   = im.h;
    d.mipCount = 1;
    d.usage    = usage;
    d.flags    = FFX_RESOURCE_FLAGS_NONE;
    // 反查回 FFX 的枚举：VkFormat → FfxSurfaceFormat 有现成的 ffxGetSurfaceFormatVK
    // 的逆映射需求，这里直接用我们建图时用的格式，避免两条真相源。
    d.format = im.ffxFormat;
    FfxResource r = ffxGetResourceVK((void*)im.image, d, L"fsr_smoke", FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    r.description = d;
    return r;
}

}  // namespace

int main(int argc, char** argv)
{
    const uint32_t renderW = argc > 1 ? std::atoi(argv[1]) : 640;
    const uint32_t renderH = argc > 2 ? std::atoi(argv[2]) : 360;
    installCrashTrace();
    const uint32_t outW    = renderW * 2;
    const uint32_t outH    = renderH * 2;
    log("render=%ux%u upscale=%ux%u", renderW, renderH, outW, outH);

    VkCtx c{};

    // ── instance ──────────────────────────────────────────────────────────────
    const char* iext[] = {"VK_KHR_surface", "VK_EXT_debug_utils"};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName   = "fsr_smoke";
    app.apiVersion         = FFX_SMOKE_VULKAN_API_VERSION;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo       = &app;
    ici.enabledExtensionCount  = 2;
    ici.ppEnabledExtensionNames = iext;
    VkDebugUtilsMessengerCreateInfoEXT mci{};
    mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mci.pfnUserCallback = debugCb;
    ici.pNext             = &mci;
    const char* ilayers[]   = {"VK_LAYER_KHRONOS_validation"};
    ici.enabledLayerCount   = 1;
    ici.ppEnabledLayerNames = ilayers;
    VkResult ir               = vkCreateInstance(&ici, nullptr, &c.instance);
    if (ir != VK_SUCCESS)
    {
        ici.enabledLayerCount = 0;
        check(vkCreateInstance(&ici, nullptr, &c.instance), "vkCreateInstance (no validation)");
    }
    else
        c.validation = true;
    log("validation layer: %s", c.validation ? "ON" : "OFF");
    auto getMessenger = [&](const char* n) {
        return reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(c.instance, n));
    };
    if (auto f = getMessenger("vkCreateDebugUtilsMessengerEXT")) f(c.instance, &mci, nullptr, &c.messenger);

    // ── device ────────────────────────────────────────────────────────────────
    uint32_t pc = 0;
    check(vkEnumeratePhysicalDevices(c.instance, &pc, nullptr), "enumerate phys");
    std::vector<VkPhysicalDevice> pds(pc);
    vkEnumeratePhysicalDevices(c.instance, &pc, pds.data());
    if (pc == 0) die("no Vulkan device on this machine");
    c.phys = pds[0];
    for (auto d : pds)
    {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(d, &p);
        log("candidate: %s (type=%d api=%u.%u)", p.deviceName, (int)p.deviceType, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion));
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

    // FFX 的 VK 后端会**查询**这些特性（fp16 / descriptor indexing / subgroup size
    // control）；不启用不会立刻炸，但 wave64 变体与 FFX_HALF=1 变体的能力判定会走
    // 到没测过的分支。冒烟就把能拿的都开上，让「跑不通」归因到 FFX 而不是我们的设备。
    VkPhysicalDeviceDescriptorIndexingFeatures      di{};
    di.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    VkPhysicalDeviceShaderFloat16Int8Features       f16{};
    f16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT16_INT8_FEATURES;
    VkPhysicalDevice16BitStorageFeatures            b16{};
    b16.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES;
    VkPhysicalDeviceSubgroupSizeControlFeaturesEXT  ssc{};
    ssc.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES_EXT;
    di.pNext = &f16;
    f16.pNext = &b16;
    b16.pNext = &ssc;
    c.features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    c.features2.pNext = &di;
    vkGetPhysicalDeviceFeatures2(c.phys, &c.features2);

    const char* dext[] = {"VK_EXT_subgroup_size_control", "VK_EXT_descriptor_indexing", "VK_KHR_maintenance4"};
    uint32_t     ec     = 0;
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &ec, nullptr);
    std::vector<VkExtensionProperties> es(ec);
    vkEnumerateDeviceExtensionProperties(c.phys, nullptr, &ec, es.data());
    std::vector<const char*> on;
    for (const char* want : dext)
        for (auto& e : es)
            if (std::strcmp(e.extensionName, want) == 0) { on.push_back(want); break; }
    log("device extensions on: %zu/%zu", on.size(), std::size(dext));

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex   = c.queueFamily;
    qi.queueCount         = 1;
    qi.pQueuePriorities   = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount     = 1;
    dci.pQueueCreateInfos        = &qi;
    dci.enabledExtensionCount    = (uint32_t)on.size();
    dci.ppEnabledExtensionNames  = on.data();
    di.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    di.runtimeDescriptorArray                    = VK_TRUE;
    di.descriptorBindingPartiallyBound           = VK_TRUE;
    f16.shaderFloat16                            = VK_TRUE;
    b16.storageBuffer16BitAccess                 = VK_TRUE;
    ssc.subgroupSizeControl                      = VK_TRUE;
    ssc.computeFullSubgroups                     = VK_TRUE;
    dci.pNext = &c.features2;
    check(vkCreateDevice(c.phys, &dci, nullptr, &c.device), "vkCreateDevice");
    vkGetDeviceQueue(c.device, c.queueFamily, 0, &c.queue);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = c.queueFamily;
    check(vkCreateCommandPool(c.device, &cpi, nullptr, &c.pool), "vkCreateCommandPool");

    // ── FFX backend ───────────────────────────────────────────────────────────
    const size_t scratchSize = ffxGetScratchMemorySizeVK(c.phys, FFX_FSR3UPSCALER_CONTEXT_COUNT);
    void*        scratch     = std::malloc(scratchSize);
    log("scratch = %zu bytes", scratchSize);

    VkDeviceContext vdc{c.device, c.phys, &vkGetDeviceProcAddr};
    FfxDevice        dev = ffxGetDeviceVK(&vdc);

    FfxInterface backend{};
    check(ffxGetInterfaceVK(&backend, dev, scratch, scratchSize, FFX_FSR3UPSCALER_CONTEXT_COUNT), "ffxGetInterfaceVK");
    log("backend interface OK");

    FfxFsr3UpscalerContextDescription cd{};
    cd.flags           = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING
                           | FFX_FSR3UPSCALER_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION;
    cd.maxRenderSize   = {renderW, renderH};
    cd.maxUpscaleSize  = {outW, outH};
    cd.backendInterface = backend;
    static FfxFsr3UpscalerContext ctx;  // 1 MiB（见 PATCHES.md P6），别放栈上
    check(ffxFsr3UpscalerContextCreate(&ctx, &cd), "ffxFsr3UpscalerContextCreate");
    log("context created");

    // ── 资源 ──────────────────────────────────────────────────────────────────
    const VkImageUsageFlags su = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // 三张共享图**按 FFX 自己给的描述**建（格式/尺寸不对 = 它内部按自己的假设读写，
    // 是那种「跑通了但结果错」的坑），所以先问它要，再建图。
    FfxFsr3UpscalerSharedResourceDescriptions shared{};
    check(ffxFsr3UpscalerGetSharedResourceDescriptions(&ctx, &shared), "GetSharedResourceDescriptions");

    Image color, depth, motion, output, exposure, dilatedDepth, dilatedMv, reconDepth;
    createImage(c, color, renderW, renderH, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, su);
    createImage(c, depth, renderW, renderH, FFX_SURFACE_FORMAT_R32_FLOAT, su);
    createImage(c, motion, renderW, renderH, FFX_SURFACE_FORMAT_R16G16_FLOAT, su);
    createImage(c, output, outW, outH, FFX_SURFACE_FORMAT_R16G16B16A16_FLOAT, su);
    createImage(c, exposure, 1, 1, FFX_SURFACE_FORMAT_R32_FLOAT, su);
    auto createShared = [&](Image& im, const FfxCreateResourceDescription& want, const char* what) {
        if (want.resourceDescription.type != FFX_RESOURCE_TYPE_TEXTURE2D)
            die(what);
        createImage(c, im, want.resourceDescription.width, want.resourceDescription.height,
                    want.resourceDescription.format, su);
    };
    createShared(dilatedDepth, shared.dilatedDepth, "dilatedDepth desc 不是 TEXTURE2D");
    createShared(dilatedMv, shared.dilatedMotionVectors, "dilatedMotionVectors desc 不是 TEXTURE2D");
    createShared(reconDepth, shared.reconstructedPrevNearestDepth, "reconstructedPrevNearestDepth desc 不是 TEXTURE2D");
    log("images created");

    FfxResource rColor    = wrap(color, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResource rDepth    = wrap(depth, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResource rMotion   = wrap(motion, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResource rExposure = wrap(exposure, FFX_RESOURCE_USAGE_READ_ONLY);
    FfxResource rOutput   = wrap(output, FFX_RESOURCE_USAGE_UAV);
    auto wrapShared = [&](const Image& im, const FfxCreateResourceDescription& want) {
        // 建图就是照 want 来的，这里再确认一次没有第二套真相源
        if (im.ffxFormat != want.resourceDescription.format || im.w != want.resourceDescription.width || im.h != want.resourceDescription.height)
            die("共享图与 FFX 期望不一致");
        return wrap(im, FFX_RESOURCE_USAGE_UAV);
    };
    FfxResource rDilatedDepth = wrapShared(dilatedDepth, shared.dilatedDepth);
    FfxResource rDilatedMv    = wrapShared(dilatedMv, shared.dilatedMotionVectors);
    FfxResource rReconDepth   = wrapShared(reconDepth, shared.reconstructedPrevNearestDepth);

    // ── dispatch 两帧 ─────────────────────────────────────────────────────────
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool        = c.pool;
    cai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer        cb = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(c.device, &cai, &cb), "vkAllocateCommandBuffers");

    auto dispatchFrame = [&](int frame, bool reset, FfxFloatCoords2D mvScale = {1.0f, 1.0f},
                               FfxFloatCoords2D jitter = {0.0f, 0.0f})
    {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");

        FfxFsr3UpscalerDispatchDescription dd{};
        dd.commandList    = ffxGetCommandListVK(cb);
        dd.color          = rColor;
        dd.depth          = rDepth;
        dd.motionVectors  = rMotion;
        dd.exposure       = rExposure;
        dd.dilatedDepth   = rDilatedDepth;
        dd.dilatedMotionVectors = rDilatedMv;
        dd.reconstructedPrevNearestDepth = rReconDepth;
        dd.output         = rOutput;
        dd.jitterOffset   = jitter;
        dd.motionVectorScale = mvScale;   // (1,1) = 输入按「像素」计，见 docs/FSR输入契约.md §1
        dd.renderSize     = {renderW, renderH};
        dd.upscaleSize    = {outW, outH};
        dd.enableSharpening = false;
        dd.sharpness      = 0.0f;
        dd.frameTimeDelta = 16.6f;
        dd.preExposure    = 1.0f;
        dd.reset          = reset;
        dd.cameraNear     = 0.1f;
        dd.cameraFar      = 1000.0f;
        dd.cameraFovAngleVertical = 1.0f;
        dd.viewSpaceToMetersFactor = 0.0f;

        check(ffxFsr3UpscalerContextDispatch(&ctx, &dd), "ffxFsr3UpscalerContextDispatch");
        check(vkEndCommandBuffer(cb), "vkEndCommandBuffer");

        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers    = &cb;
        check(vkQueueSubmit(c.queue, 1, &si, nullptr), "vkQueueSubmit(fsr)");
        check(vkQueueWaitIdle(c.queue), "vkQueueWaitIdle(fsr)");
        log("frame %d dispatched + executed", frame);
    };

    for (int frame = 0; frame < 2; ++frame) dispatchFrame(frame, frame == 0);

    // ── 契约探针：速度场口径（批 14.2 判据 ②③；口径本体 → docs/FSR输入契约.md）──
    // 契约是 `stored = input × params.motionVectorScale / motionVectorsTargetSize`
    // （ffx_fsr3upscaler.cpp:996-997 + callbacks_glsl.h:373）。本探针钉死三件事：
    //   A. **motionVectorScale 是分子不是分母**：像素单位的输入要填 **(1,1)**；填 renderSize
    //      等于声明「我的 MV 已经是 UV」（scale 变成 1，恒等）——第一版探针就是这么写错的，
    //      回读直接等于输入，被这条对拍当场抓住。
    //   B. **分母是 renderSize 还是 upscaleSize**。scale 固定成 1 后，输出完全由分母决定；
    //      本机 render=640 / out=1280 差 2 倍，可分辨。没开 DISPLAY_RESOLUTION flag
    //      ⟹ 源码说期望 renderSize。
    //   C. **有没有隐藏取负**。±dx 各跑一次，回读必须同号；反向用例就是它自己。
    // 读的是 dilatedMotionVectors：它由 FSR 在 prepare_inputs 里写出、**由我们分配**，
    // 所以能直接回读（源码里它存的已经是 UV 单位）。
    {
        const float  dxPx   = 8.0f;
        const float  expect = dxPx / float(renderW);      // = 0.0125；若分母是 upscaleSize 则是 0.00625
        int          bad    = 0;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        for (float sign : {1.0f, -1.0f})
        {
            // 源图喂「像素」值；深度清成常量 ⟹ 9 点邻域深度全等，膨胀取到的就是本像素，无歧义
            check(vkBeginCommandBuffer(cb, &bi), "probe vkBeginCommandBuffer");
            VkClearColorValue mv{};   mv.float32[0]   = sign * dxPx;
            VkClearColorValue dep{};  dep.float32[0]  = 0.5f;
            clearImageTo(c, cb, motion, mv);
            clearImageTo(c, cb, depth, dep);
            check(vkEndCommandBuffer(cb), "probe vkEndCommandBuffer");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            check(vkQueueSubmit(c.queue, 1, &si, nullptr), "probe vkQueueSubmit");
            check(vkQueueWaitIdle(c.queue), "probe vkQueueWaitIdle");

            dispatchFrame(100, /*reset=*/true);

            const auto mvPx = readBackImage(c, dilatedMv, 4);   // R16G16_FLOAT = 4 B/px
            const size_t center = size_t(dilatedMv.h / 2) * dilatedMv.w + dilatedMv.w / 2;
            const float gx = halfChannelAt(mvPx, center, 2, 0);
            const float gy = halfChannelAt(mvPx, center, 2, 1);
            const float wantX = sign * expect, wantY = 0.0f;
            // 容差按 R16G16_FLOAT 的精度给：0.0125 附近 half 的间隔约 6e-6，放 1e-3 已极宽
            const bool okX = std::fabs(gx - wantX) < 1e-3f;
            const bool okY = std::fabs(gy - wantY) < 1e-3f;
            log("probe mv: 输入 %+g px → dilatedMotionVectors (%.6f, %.6f)，期望 (%.6f, %.6f) %s",
                sign * dxPx, gx, gy, wantX, wantY, (okX && okY) ? "OK" : "**FAIL**");
            if (!okX || !okY) ++bad;
        }
        if (bad) {
            std::fprintf(stderr,
                "[fsr-smoke] FAIL: 速度场口径对账 %d 条不符。分母若为 upscaleSize 会得到 %.6f"
                "（期望 %.6f）；符号不符 = 链路上有隐藏取负。口径出处 → docs/FSR输入契约.md §1\n",
                bad, dxPx / float(outW), expect);
            return 1;
        }
    }

    // ── 契约探针：jitter 进入速度场的符号（批 14.2 判据 ②③；口径 → docs/FSR输入契约.md §3）
    // 为什么用这条而不是「量输出图里边缘的位移」：jitter 的**合法范围只有 ±0.5 px**
    // （Halton 生成器值域，host 在 ffx_fsr3upscaler.cpp:219-221 还会对 >1.0 告警），
    // 预测位移 ≤1 显示像素，而重建滤波器本身的量化就有 ~2 px —— 实测扫 jitter 时
    // j=+2 与 j=+4 给出同一个值、|j|=8 直接测不到边缘。**那个仪器是坏的**，撤掉了。
    //
    // 这条走 jitter cancellation：开启 flag 后 `mv_uv -= (previousJitter − curJitter)/targetSize`
    // （ffx_fsr3upscaler.cpp:1002-1006 + callbacks_glsl.h:375-377），而 mv_uv 会写进
    // dilatedMotionVectors ⟹ **可以直接精确回读**，不受滤波器影响。
    // 输入 mv=0 ⟹ 回读值 = −(prev − cur)/renderSize，把符号、分母、以及
    // 「previous 到底是上一帧的 jitter」三件事一次钉死。
    {
        const float jy = 0.5f;                       // 合法范围上限
        int   bad  = 0;
        bool  first = true;
        for (float cur : {0.0f, jy, -jy})
        {
            // 源速度清零、深度清常量（膨胀取本像素，无歧义）
            VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            check(vkBeginCommandBuffer(cb, &bi), "jit vkBegin");
            VkClearColorValue z{};
            z.float32[0] = 0.0f; clearImageTo(c, cb, motion, z);
            z.float32[0] = 0.5f; clearImageTo(c, cb, depth,  z);
            check(vkEndCommandBuffer(cb), "jit vkEnd");
            VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            si.commandBufferCount = 1; si.pCommandBuffers = &cb;
            check(vkQueueSubmit(c.queue, 1, &si, nullptr), "jit submit");
            check(vkQueueWaitIdle(c.queue), "jit wait");

            dispatchFrame(300, /*reset=*/first, {1.0f, 1.0f}, {0.0f, cur});
            first = false;

            // 期望：stored = −(prev − cur)/renderH，prev 是**上一轮**传进来的 jitter
            static float prev = 0.0f;
            const float prevUsed = prev;
            const float want = -(prevUsed - cur) / float(renderH);
            prev = cur;

            const auto px = readBackImage(c, dilatedMv, 4);
            const size_t center = size_t(dilatedMv.h / 2) * dilatedMv.w + dilatedMv.w / 2;
            const float gx = halfChannelAt(px, center, 2, 0);
            const float gy = halfChannelAt(px, center, 2, 1);
            const bool okX = std::fabs(gx) < 1e-5f;
            const bool okY = std::fabs(gy - want) < 1e-5f;
            log("probe jitter: prev=%+.2f cur=%+.2f → dilatedMV (%.6f, %.6f)，期望 (0, %.6f) %s",
                prevUsed, cur, gx, gy, want, (okX && okY) ? "OK" : "**FAIL**");
            if (!okX || !okY) ++bad;
        }
        if (bad) {
            std::fprintf(stderr,
                "[fsr-smoke] FAIL: jitter cancellation 对账 %d 条不符。口径 = "
                "mv_uv -= (previousJitter - curJitter)/motionVectorsTargetSize，"
                "出处 → docs/FSR输入契约.md §3\n", bad);
            return 1;
        }
    }

    ffxFsr3UpscalerContextDestroy(&ctx);
    vkDeviceWaitIdle(c.device);
    vkDestroyDevice(c.device, nullptr);
    if (c.messenger)
    {
        auto d = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(c.instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (d) d(c.instance, c.messenger, nullptr);
    }
    vkDestroyInstance(c.instance, nullptr);
    std::free(scratch);
    // VVL 判据：白名单外的报错 = FAIL。白名单命中数逐条打印，避免「0 unexpected」被
    // 误读成「VVL 一条都没报」。
    log("VVL messages: %d total, %d unexpected", g_vvl.total, g_vvl.unexpected);
    for (int i = 0; i < VvlLedger::kKnownCount; ++i)
        log("  known[%d] %-52s x%d", i, VvlLedger::kKnown[i], g_vvl.hits[i]);
    if (g_vvl.unexpected > 0) {
        std::fprintf(stderr, "[fsr-smoke] FAIL: %d 条白名单外的 VVL 报错\n", g_vvl.unexpected);
        return 1;
    }
    log("PASS");
    return 0;
}
