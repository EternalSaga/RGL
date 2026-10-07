// RGL-PATCH(P7): ffx_vk.cpp:349 无条件把 fpSwapChainConfigureFrameGeneration 指向
// ffxSetFrameGenerationConfigToSwapchainVK，而那个函数的实现在
// src/backends/vk/FrameInterpolationSwapchain/（本 vendor 树**故意不含**——它是
// DXGI/DX12 的 present 替换，Vulkan 上要自己接 present 节奏，phase14_plan §4 明确不做）。
// 不补一个定义的话，只要有人链 ffx_vk.cpp.o，链接期就报缺符号。
//
// 这里给的是**显式失败**的桩，不是空实现：调用它 = 用了没带的功能，必须出声，
// 不能静默成功然后 Frame Generation 从此不生效（那种 bug 查起来要命）。
#include <FidelityFX/host/backends/vk/ffx_vk.h>

extern "C" FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const* config)
{
    (void)config;
    return FFX_ERROR_BACKEND_API_ERROR;
}
