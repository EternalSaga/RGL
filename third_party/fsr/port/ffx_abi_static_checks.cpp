// RGL-PATCH(P9) 的**护栏**：不透明 context 的对齐守卫。
//
// 为什么单独一个 TU 而不是塞进 vendor 头：这条断言要防的是「vendor 升级 / 重放补丁时把 P9
// 弄丢」，而 P9 丢失后**编译照过、只在 -O2 运行期段错误**（-O0 侥幸不炸），排查成本极高
// ——见 docs/调试历史.md 2026-10-08。断言放这里 = 弄丢补丁立刻变成编译失败。
//
// 本文件没有别的职责，也不该有：加新的 FSR ABI 断言就往这里追加。
#include <FidelityFX/host/ffx_fsr3upscaler.h>

#include <cstddef>
#include <cstdint>

// P9：原本 `typedef struct { uint32_t data[N]; }` 的对齐只有 4，而
// ffx_fsr3upscaler.cpp:511 把它直接当 FfxFsr3UpscalerContext_Private* 用（含指针成员、
// 要求 8 对齐）。对齐不足 ⟹ 未定义行为，-O2 下 clang 发对齐 SSE 存指令 → SIGSEGV。
//
// 判据取 alignof(void*) 而不是 alignof(std::max_align_t)：后者在 x86-64 是 **16**
// （long double 的对齐），拿它当门槛会把一个本来正确的头判成坏的（第一版就这么写错过）。
// 真正的需求出处是使用方——私有结构里的指针成员，即 alignof(void*)。
static_assert(alignof(FfxFsr3UpscalerContext) >= alignof(void*),
              "RGL-PATCH(P9) 丢了：FfxFsr3UpscalerContext 的对齐不足，"
              "ffx_fsr3upscaler.cpp 会把它当含指针的结构用（-O2 下段错误）。"
              "恢复办法 = 在 ffx_fsr3upscaler.h 里给该 typedef 加回 8 字节对齐成员。");

// 抬对齐用的是 union（`data[]` 与一个 uint64_t 共用存储），所以 sizeof 只可能因 N 的
// 奇偶而多 4 字节尾 padding —— 那不影响正确性。真正不能破的是**存储起点**：
// ffx_fsr3upscaler.cpp:511 直接把 context 的地址当 `FfxFsr3UpscalerContext_Private*`
// 用，等于假定 `data` 就在偏移 0。哪天有人把它改成 struct 或前面插字段，这里立刻红。
static_assert(offsetof(FfxFsr3UpscalerContext, data) == 0,
              "FfxFsr3UpscalerContext::data 不在偏移 0：vendor 把 context 地址直接当私有结构用");
static_assert(sizeof(FfxFsr3UpscalerContext) >= sizeof(uint32_t) * FFX_FSR3UPSCALER_CONTEXT_SIZE,
              "context 存储小于 data[]：抬对齐的守卫成员挤掉了真实容量");
