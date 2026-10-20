#pragma once
// 交付/合成**共用例外门**的跨 TU 面（B5 二次拆：engine_profile.cpp 加第 8 枚门后 521 代码行
// > 500，护栏逼的一刀；缝 = 「写盘之后的 read-modify-write 门」住 `engine_profile_gates.cpp`，
// 「配置怎么生成」住 `engine_profile.cpp`）。
//
// 五条门的共同契约（为什么它们是同一族）：
//  · 门闭 = 函数第一行返回 = 一个键都不多写 = 现有判据**逐位不变**（这条性质由门函数自己
//    保证，不靠调用侧记得加 if——这是它们从 makeProfile 里抽出来的原始动机，护栏 = 认知复杂度）；
//  · 门开 = 对已落盘的 profile 目录做 read-modify-write，**写后读回自检**（覆写与自检同一套
//    定位判据，`engine_lua_edit.h`），打偏即抛——「覆写了但没生效」是这类工具最贵的失效。
// 调用顺序有语义（后写的吃前写的结果），唯一出处 = `makeProfile` 调用点注释。

#include <string>

namespace RGL::imgqa {

struct ProfileOpts;

/// 第 4 枚门：覆写 `shadow.lua` 软影滤波轴（mode/izb/太阳角）。实现与逐条理由 = `engine_profile_gates.cpp`。
void applyShadowRouteGate(const std::string& dir, const ProfileOpts& o);
/// 第 5 枚门：profile 切成一次性截图形态（llm_debug/llm_shot/rpc）。交付与合成两条路径共用。
void applyLlmShotGate(const std::string& dir, const ProfileOpts& o);
/// 第 6 枚门：覆写 `runtime.lua` 的 `meshlet.shadow_lod`（★18 远级簇 LOD 选级 D）。
void applyShadowLodGate(const std::string& dir, const ProfileOpts& o);
/// 第 7 枚门：覆写 `shadow.lua` 的 `csm.force_cascade`（强制选级诊断键）。
void applyShadowForceCascadeGate(const std::string& dir, const ProfileOpts& o);
/// 第 8 枚门：整值覆写 `scene.lua` 顶层 entities（程序化靶装进交付配置）。
void applyDeliveryEntitiesGate(const std::string& dir, const ProfileOpts& o);

}  // namespace RGL::imgqa
