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

#include <cstdarg>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
// 冒烟的判据之一就是「VVL 干净」，但 AMD FSR 3.1.4 的 VK 后端在 RADV 上有 4 类**上游自己**
// 的 spec 违规（根因逐条见 third_party/fsr/PATCHES.md §已知上游问题）。白名单不等于忽略：
// 它记的是「已知且已归因」，**任何不在表内的新报错都会让冒烟 FAIL**，这样上游升级或我们
// 改接线时新引入的问题不会静默溜过。计数会打印出来，白名单命中数变了也看得见。
struct VvlLedger
{
    int total      = 0;
    int unexpected = 0;
    int hits[4]    = {0, 0, 0, 0};

    // 特征子串取 VUID 或唯一短语，不取整句——上游改措辞时宁可失配报错，也不要静默漏过。
    static const char* const* known(int* n)
    {
        static const char* const kKnown[] = {
            "VUID-vkAllocateMemory-deviceCoherentMemory-02790",
            "VUID-VkImageSubresourceRange-layerCount-01721",
            "which doesn't match the VkImageView",
            "VUID-vkDestroyDevice-device-05137",
        };
        *n = int(sizeof(kKnown) / sizeof(kKnown[0]));
        return kKnown;
    }
};
VvlLedger g_vvl;

VKAPI_ATTR VkBool32 VKAPI_CALL
debugCb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* d, void*)
{
    if (sev < VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        return VK_FALSE;
    ++g_vvl.total;
    int n = 0;
    const char* const* known = VvlLedger::known(&n);
    for (int i = 0; i < n; ++i) {
        if (std::strstr(d->pMessage, known[i])) {
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
    cd.flags           = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE | FFX_FSR3UPSCALER_ENABLE_DEBUG_CHECKING;
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

    for (int frame = 0; frame < 2; ++frame)
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
        dd.jitterOffset   = {0.0f, 0.0f};
        dd.motionVectorScale = {(float)renderW, (float)renderH};
        dd.renderSize     = {renderW, renderH};
        dd.upscaleSize    = {outW, outH};
        dd.enableSharpening = false;
        dd.sharpness      = 0.0f;
        dd.frameTimeDelta = 16.6f;
        dd.preExposure    = 1.0f;
        dd.reset          = frame == 0;
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
    int n = 0;
    const char* const* known = VvlLedger::known(&n);
    log("VVL messages: %d total, %d unexpected", g_vvl.total, g_vvl.unexpected);
    for (int i = 0; i < n; ++i)
        log("  known[%d] %-52s x%d", i, known[i], g_vvl.hits[i]);
    if (g_vvl.unexpected > 0) {
        std::fprintf(stderr, "[fsr-smoke] FAIL: %d 条白名单外的 VVL 报错\n", g_vvl.unexpected);
        return 1;
    }
    log("PASS");
    return 0;
}
