// 交付/合成共用例外门（第 4..8 枚）——从 `engine_profile.cpp` 原样搬出（一字未改语义），
// 搬出理由与五条门的共同契约 = `engine_profile_gates.h` 头注。
#include "engine_profile_gates.h"

#include "imgqa.h"
#include "engine_lua_edit.h"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace RGL::imgqa {
namespace {

// 与 engine_profile.cpp 的同名件是三行 stdlib 的**重复**（blend_sort/probe/shadow_grain 等
// 各自也有一份 readFileText）——不为此开跨 TU 面：缝变贵不值得，语义零漂移。
std::string readFile(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    if (!in)
        throw std::runtime_error("读不到文件: " + p.string());
    return std::string{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
}

void writeFile(const fs::path& p, const std::string& text)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
}

/**
 * 块内键的**写后读回**（第 6/7 例外门共用）。读回的是 `topLevelValueOf(表名)` 拿到的**整块
 * 值文本**，再在里面找 `键 = 值`。为什么不在全文里搜：
 *  ① 块内搜 = 只可能命中本块的那枚键（与 `overrideValueInTable` 的「表跨度内未命中 = 不越界
 *     去别的表找」同口径）——覆写与自检**必须同一套定位判据**，否则覆写打偏到别的块而自检说
 *     「没问题」，自检就成了摆设（`applyShadowRouteGate` 的注释把这条写在了顶层口径上）；
 *  ② 带 ` = ` 的形态顺带避开同前缀兄弟键（`shadow_lod_fallback` 不含 `shadow_lod = `）。
 * ⚠ 本自检**不是**「引擎读到的值」的终局证据，它只保证「我写进去的就是我要的那个值」。引擎
 * 是否真按这个值跑，由消费方的**引擎回显对账**钉（`shadow_cascade_seam.cpp` 查
 * `csm.force_cascade=` 与 `min(D=` 两条回显）。两层各管一段，缺任一段就有一段静默。
 */
void requireBlockKey(const std::string& text, const std::string& file,
    const std::string& table, const std::string& key, const std::string& value)
{
    const std::string block = topLevelValueOf(text, table, file);
    if (block.find(key + " = " + value) == std::string::npos
        && block.find(key + "=" + value) == std::string::npos)
        throw std::runtime_error(file + " 覆写后自检失败：`" + table + "." + key
            + "` 读回的不是 " + value + "（读回块 = " + block
            + "）——覆写打偏或被别处盖掉，拒绝在旧值上下判据");
}

}  // namespace

/**
 * 第 4 个例外门（批 N6d）：覆写交付配置 `shadow.lua` 的软影滤波轴。
 * 门闭 = 直接返回（一个键都不写 = 现有判据逐位不变）。
 * @throws 档名非法 / 键缺失 / 太阳角给了不消费它的档
 */
void applyShadowRouteGate(const std::string& dir, const ProfileOpts& o)
{
    if (!o.overrideShadowRoute)
        return;
    const std::string mode = o.shadowRouteMode;
    if (mode != "none" && mode != "csm" && mode != "pcss" && mode != "evssm")
        throw std::runtime_error("overrideShadowRoute=true 而 shadowRouteMode='" + mode
            + "' 不是 none|csm|pcss|evssm 之一（拼错的档名引擎只在启动期喊，本工具在生成期就拒）");
    std::string sh = readFile(dir + "/shadow.lua");
    overrideTopLevelValue(sh, "shadow.lua", "mode", "\"" + mode + "\"");
    // izb 强制关：整值换成**最小子表** `{ on = false }`（引擎硬切后只收子表形态，扁平
    // 布尔 = 启动报错）。留着它 = 硬影 mask 乘在软影之上 = 测的不是滤波轴（§4.1）。
    overrideTopLevelValue(sh, "shadow.lua", "izb", "{ on = false }");
    if (o.shadowRouteSunDegrees >= 0.0) {
        if (mode != "pcss" && mode != "evssm")
            throw std::runtime_error("shadowRouteSunDegrees="
                + std::to_string(o.shadowRouteSunDegrees) + " 而 mode='" + mode
                + "'：太阳角只有 pcss/evssm 两档消费，写进 none/csm = 配置里写了、什么都没发生");
        std::ostringstream v;
        v << std::setprecision(9) << o.shadowRouteSunDegrees;
        overrideValueInTable(sh, "shadow.lua", mode, "sun_degrees", v.str());
    }
    // 写出后自检：读回**解析后的值**比对，而不是在文件里搜字面量。
    // "覆写了但没生效"是这类工具最贵的失效（键名打偏 = 静默拿出厂值下判据）。
    // ⚠ 刻意不用 `sh.find("on = true") == npos` 那种"旧值离场"搜法：它把判据耦合到
    //   **全文**（含注释）——configs/shadow.lua 的注释块正在讲子表形态怎么写，哪天补一行
    //   `-- izB = { on = true, ... }` 示例，旧搜法就抛一句看不懂的"自检失败"。
    const bool modeOk = topLevelValueOf(sh, "mode", "shadow.lua") == "\"" + mode + "\"";
    const bool izbOk = topLevelValueOf(sh, "izb", "shadow.lua") == "{ on = false }";
    if (!modeOk || !izbOk)
        throw std::runtime_error("shadow.lua 覆写后自检失败（mode=" + mode
            + " / izb = { on = false } 之一没真正落上，读回值 = \"" + topLevelValueOf(sh, "mode", "shadow.lua")
            + "\" / \"" + topLevelValueOf(sh, "izb", "shadow.lua")
            + "\"）——拒绝在旧值上下判据");
    writeFile(dir + "/shadow.lua", sh);
}

/**
 * 第 5 个例外门（批 N6d）：把 profile 切成**一次性截图**形态（钉死帧号）。
 * 门闭 = 直接返回。rpc 同时关掉是引擎既有约定（llm 截图在跑时 `/shot` 返 409）。
 */
void applyLlmShotGate(const std::string& dir, const ProfileOpts& o)
{
    if (!o.overrideLlmShot)
        return;
    if (o.llmShotFile.empty())
        throw std::runtime_error("overrideLlmShot=true 而 llmShotFile 为空——截图落在哪里都不知道");
    if (o.llmShotDelayFrames < 0)
        throw std::runtime_error("llmShotDelayFrames 不能为负: "
            + std::to_string(o.llmShotDelayFrames));
    std::string rt = readFile(dir + "/runtime.lua");
    replaceValue(rt, "runtime.lua", "llm_debug", "true");
    overrideValueInTable(rt, "runtime.lua", "llm_shot", "file", "\"" + o.llmShotFile + "\"");
    overrideValueInTable(rt, "runtime.lua", "llm_shot", "size",
        "{ " + std::to_string(o.size[0]) + ", " + std::to_string(o.size[1]) + " }");
    overrideValueInTable(rt, "runtime.lua", "llm_shot", "delay_frames",
        std::to_string(o.llmShotDelayFrames));
    if (rt.find("llm_debug = true") == std::string::npos)
        throw std::runtime_error("runtime.lua 覆写后自检失败（llm_debug 不是 true）");
    writeFile(dir + "/runtime.lua", rt);

    std::string dg = readFile(dir + "/diagnostics.lua");
    replaceValue(dg, "diagnostics.lua", "rpc", "false");
    writeFile(dir + "/diagnostics.lua", dg);
}

/**
 * 第 6 个例外门（`shadow-cascade-seam`，债-110）：覆写交付配置 `runtime.lua` 的
 * `meshlet.shadow_lod`（★18 远级簇 LOD 的 uniform 选级 D）。门闭 = 第一行返回 = 一个键都不
 * 多写 = 现有判据逐位不变。取值语义（-1 = 关 / 0 = 阳性对照 L0 臂 / N≥1 = 选级 D=N）的唯一
 * 真相源 = `configs/runtime.lua` 与 `rgl/runtime_config.cppm`，本函数只搬运不解释。
 * @throws 块/键不存在（`overrideValueInTable` 抛）/ 写后读回不是给定值
 */
void applyShadowLodGate(const std::string& dir, const ProfileOpts& o)
{
    if (!o.overrideShadowLod)
        return;
    std::string rt = readFile(dir + "/runtime.lua");
    const std::string v = std::to_string(o.shadowLodValue);
    overrideValueInTable(rt, "runtime.lua", "meshlet", "shadow_lod", v);
    requireBlockKey(rt, "runtime.lua", "meshlet", "shadow_lod", v);
    writeFile(dir + "/runtime.lua", rt);
}

/**
 * 第 7 个例外门（同上判据）：覆写交付配置 `shadow.lua` 的 `csm.force_cascade`（**强制选级
 * 诊断键**，不是画质旋钮）。为什么不并进第 4 枚（`overrideShadowRoute`）：那枚覆写的是**软影
 * 滤波轴**、且会连带把 `izb` 强制成 `{ on = false }`，与本键正交——两件事装一扇门，改一条
 * 必踩另一条（而这两条的改动缝本来就不同：一条是滤波档定案，一条是诊断仪器）。
 * 门闭 = 第一行返回 = 逐位不变。
 * @throws `csm` 块/键不存在 / 写后读回不是给定值
 */
void applyShadowForceCascadeGate(const std::string& dir, const ProfileOpts& o)
{
    if (!o.overrideShadowForceCascade)
        return;
    std::string sh = readFile(dir + "/shadow.lua");
    const std::string v = std::to_string(o.shadowForceCascadeValue);
    overrideValueInTable(sh, "shadow.lua", "csm", "force_cascade", v);
    requireBlockKey(sh, "shadow.lua", "csm", "force_cascade", v);
    writeFile(dir + "/shadow.lua", sh);
}

/**
 * 第 8 个例外门（`shadow-edge-ruler`）：把交付配置 scene.lua 的**顶层 entities 表**整值
 * 覆写成程序化靶（被测靶几何 = 实验自变量，交付场景 bistro 不是被测物；论证全文住
 * `imgqa.h` 的 ProfileOpts 字段注释）。覆写器 = `overrideTopLevelValue`（命中数 != 1 /
 * 形状不认识 = 抛）；写后读回**必须与覆写同一套定位判据**（`topLevelValueOf` 顶层口径），
 * 否则「打偏到别的表而自检说没问题」的形态又回来了（requireBlockKey 的同一条纪律，顶层版）。
 * @throws entities 键不在/不唯一、读回不含给定值
 */
void applyDeliveryEntitiesGate(const std::string& dir, const ProfileOpts& o)
{
    if (!o.overrideDeliveryEntities)
        return;
    if (o.deliveryEntitiesLua.empty())
        throw std::runtime_error("overrideDeliveryEntities=true 而 deliveryEntitiesLua 为空"
            "——装进场景的是哪个实体都不知道，拒绝生成");
    std::string sc = readFile(dir + "/scene.lua");
    overrideTopLevelValue(sc, "scene.lua", "entities", "{ " + o.deliveryEntitiesLua + " }");
    const std::string back = topLevelValueOf(sc, "entities", "scene.lua");
    if (back.find(o.deliveryEntitiesLua) == std::string::npos)
        throw std::runtime_error("scene.lua 覆写 entities 后自检失败：读回的不是 "
            + o.deliveryEntitiesLua + "（读回 = " + back
            + "）——覆写打偏或被别处盖掉，拒绝在交付场景上跑靶判据");
    writeFile(dir + "/scene.lua", sc);
}
/**
 * 第 9 个例外门（`shadow-edge-rebase`，相对坐标批 5 第 2 刀）：覆写交付配置 `runtime.lua` 的
 * `coordinate.rebase`（渲染原点 rebase 总闸）。四格对拍的自变量**就是这一枚键**，
 * 与第 6/7 枚同族同判据；门闭 = 第一行返回 = 逐位不变。
 * ⚠ 本门只保证「写进去的是要的值」；引擎是否真按它跑由消费方查回显行
 * `coordinate.rebase = true`（写这行的理由与位置 → `rgl/main_impl.cppm` 的 wireTuningContexts）。
 */
void applyCoordinateRebaseGate(const std::string& dir, const ProfileOpts& o)
{
    if (!o.overrideCoordinateRebase)
        return;
    std::string rt = readFile(dir + "/runtime.lua");
    const std::string v = o.coordinateRebaseValue ? "true" : "false";
    overrideValueInTable(rt, "runtime.lua", "coordinate", "rebase", v);
    requireBlockKey(rt, "runtime.lua", "coordinate", "rebase", v);
    writeFile(dir + "/runtime.lua", rt);
}

}  // namespace RGL::imgqa
