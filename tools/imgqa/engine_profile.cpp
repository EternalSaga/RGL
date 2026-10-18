// 配置语义层：把 `ProfileOpts` 翻译成 lua（交付侧逐键覆写 / 合成侧整份写出）。
//
// 三层缝（B5 拆文件）：本文件 = **配置语义**（认得 ProfileOpts 与 shadow.lua / scene.lua 这些
// 文件名）；`engine_lua_edit.cpp` = **lua 文本编辑原语**（不认识配置，只做定位与替换）；
// `engine.cpp` = **引擎会话**（profile 目录之外的一切：SPV 新鲜度守卫、子进程生命周期、RPC）。
// 缝的另一半理由：只有本文件吃 `engine_lua_edit.h`，改判据容差动那边、加诊断键动这里。

#include "imgqa.h"
#include "engine_lua_edit.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace fs = std::filesystem;

namespace RGL::imgqa {

namespace {

// 工程根下合成 profile 的模板目录（render_graph.lua / camera.lua 从这里拷）。
std::string profileTemplate() { return projectRoot() + "/qa/configs/analytic"; }

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

/// 从**引擎解析器源码**抽「配置解析器认识的根级键」。
/// 不手抄白名单：手抄的会和解析器一起漂移，最后变成"看起来在检查其实什么都没检查"
/// ——那比不检查更糟。两种读取形态都要覆盖（实测各配置模块用的不一样，只覆盖一种会
/// **误报**合法键，而误报的检查很快被当噪音绕过）：
///   diagnostics_impl：readBool("no_vvl", def) ／ scene_config_impl：readBool(root, "spin", def)
std::set<std::string> knownRootKeys(const std::string& implRel)
{
    const std::string src = readFile(fs::path(projectRoot()) / implRel);
    // raw string 用 rx 定界符：模式内容里有 `)"`（捕获组收尾紧跟引号），
    // 裸 R"(...)" 会被提前终止（实测踩过）。
    static const std::regex reRoot(R"rx(root\[\s*"([a-z_]{2,26})"\s*\])rx");
    static const std::regex reRead(R"rx(\bread\w*\([^"]*"([a-z_]{2,26})")rx");
    static const std::regex reGet(R"rx(\b(?:get|opt|has|contains|named)\(\s*"([a-z_]{2,26})")rx");
    std::set<std::string> keys;
    for (auto* re : {&reRoot, &reRead, &reGet})
        for (std::sregex_iterator it(src.begin(), src.end(), *re), e; it != e; ++it)
            keys.insert((*it)[1].str());
    return keys;
}

/// 写出的键必须在解析器里认得。**拼错键 = 静默用默认值**，是最坏的一类失效：
/// 配置看起来写了、引擎看起来读了、判据看起来在跑，而它跑在默认值上。
/// （真实事故：`shadowPass` 是 C++ 字段名，shadow.lua 的键实为 `mode`。）
void assertKeys(const std::string& fileName, const std::set<std::string>& emitted,
    const std::set<std::string>& known)
{
    std::string unknown;
    for (const auto& k : emitted)
        if (!known.count(k))
            unknown += (unknown.empty() ? "" : ", ") + k;
    if (!unknown.empty())
        throw std::runtime_error(fileName
            + " 写出了配置解析器不认识的键: " + unknown
            + "\n继续跑就等于在**默认值**上下判据——拒绝。");
}

const char* lb(bool v) { return v ? "true" : "false"; }

/// 交付配置模式：整段替换 `scene.lua` 的 `taa = { ... },` 块为显式键。
/// 缺块就在 `return {` 之后插一段——**不静默跳过**（跳过 = 判据跑在「TAA 关」的配置上，
/// 恒等式会因「pass 根本不在图里」而平凡成立 = 最阴的假绿）。
/// ⚠ 入参曾是 11 个位置参数（批 13.11 加 clip 门时到 13 个）——**改成直传 ProfileOpts**：
///   诊断键越加越多，位置参数是「加一个键改三处 + 中间默认值可能被跳过」的漂移面。
/// 约定：**默认态 = 不写键**（写出的键越少，漂移面越小）。
/// 🔴 但「不写键 = 与交付配置相同」这条**只在引擎默认值等于交付值时成立**。批 13.13 把交付
/// 默认改成 `clip_mode = 11` 后它就不成立了（不写键 = 引擎默认 0 = 门关），见下方出声守卫。
/// 交付态的 `clip_mode` 会被这次整块替换丢掉 ⟹ **出声**。抽成独立函数的理由：判据自成一件事，
/// 而 `rewriteTaaBlock` 的认知复杂度上限很紧（早返回把三个守卫从调用点挪走）。
/// ⚠ 判据**不硬编码交付值**，而是从即将被替换掉的原文里读——交付档位再改也不会漂。
void warnIfDropsDeliveryClipMode(const std::string& scene, std::string::size_type begin,
    std::string::size_type blockEnd, double clipMode)
{
    static bool warned = false;
    if (warned || clipMode != 0.0 || blockEnd == std::string::npos)
        return;
    const auto hit = scene.find("clip_mode", begin);
    if (hit == std::string::npos || hit >= blockEnd)
        return;
    warned = true;
    std::printf("⚠ 本次**未指定 `--clip`** ⟹ 不写 `clip_mode` 键 ⟹ 回落引擎默认 **0**（门关硬盒），"
                "而交付配置 scene.lua 的 taa 段里写的是非零档位（整块替换会把它丢掉）。"
                "**读数不是交付态**：要测交付态请显式 `--clip 11`，要测门关就接受它叫门关。"
                "（本行每次运行只打一次）\n");
}

void rewriteTaaBlock(std::string& scene, const ProfileOpts& o)
{
    // 批 14.6：本块**不再写 `enabled`**（键已删）——「TAA 在不在图里」是 runtime.lua 的
    // aa 三态（由 `fsrKeyValues` 写），本块只剩调参。块本身仍整段重写：feedback /
    // jitter_scale / base_space 三枚是判据的工作点，缺省值与交付配置不同就必须覆写。
    std::ostringstream blk;
    blk << "    taa = {\n        feedback = "
        << std::setprecision(6) << o.taaFeedback << ",\n        jitter_scale = "
        << o.taaJitterScale << ",\n        base_space = " << lb(o.taaBaseSpace);
    // 消融/诊断开关：默认态 = 不写键（键越少漂移面越小）。
    // motion_compensation 只关 prevSpin 场景旋转补偿（spin 模式 M3 用）；reprojection 关的是
    // prevViewProj 相机重投影本身（相机模式 M3 用）——两者在相机运动下**不等价**，
    // 语义 → rgl/scene_config.cppm 字段注释。
    if (!o.taaMotionCompensation)
        blk << ",\n        motion_compensation = false";
    if (!o.taaReprojection)
        blk << ",\n        reprojection = false";
    if (o.taaDiagReject)
        blk << ",\n        diag_reject = true";
    // 输入侧重建核（批 13.8）：0 = 双线性 = 引擎默认 = **不写键**（门关态与今天逐字相同）
    if (o.taaInputKernel != 0.0)
        blk << ",\n        input_kernel = " << std::setprecision(6) << o.taaInputKernel;
    // 历史裁剪消融门（批 13.11）：0 = 现行 = **不写键**。λ 只在 mode=4 有意义（一起写）。
    if (o.taaClipMode != 0.0)
        blk << ",\n        clip_mode = " << std::setprecision(6) << o.taaClipMode
            << ",\n        clip_soft = " << std::setprecision(6) << o.taaClipSoft;
    blk << ",\n    },\n";
    const auto begin = scene.find("\n    taa = {");
    if (begin != std::string::npos) {
        // 🔴 交付态的 clip_mode 会被这次整块替换丢掉 ⟹ 出声（理由见 helper 头注释）。
        warnIfDropsDeliveryClipMode(scene, begin, scene.find("\n    }", begin), o.taaClipMode);
        // 块收尾是 `\n    }` 再加**可选逗号**（交付配置里 taa 是 return 表最后一项，带逗号）。
        // 逗号必须一起吃掉——留一个孤零零的 `,` 在 `}` 前 = Lua 语法错（实测踩过：
        // `scene.lua:235: unexpected symbol near ','`）。
        const auto end = scene.find("\n    }", begin);
        if (end == std::string::npos)
            throw std::runtime_error("交付配置 scene.lua 的 taa 块没有收尾，无法覆写");
        std::string::size_type len = 6;  // == std::string_view("\n    }").size()
        if (end + len < scene.size() && scene[end + len] == ',')
            ++len;
        scene.replace(begin, end - begin + len, "\n" + blk.str());
        return;
    }
    const auto head = scene.find("return {");
    if (head == std::string::npos)
        throw std::runtime_error("交付配置 scene.lua 不是 `return {` 形状，无法注入 taa 块");
    scene.insert(head + 8, "\n" + blk.str());
}

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
 * 合成 profile 的 shadow.lua（从 makeProfile **抽出**：`shadow-kernel-scale` 的 pcss 臂要写
 * 逐档调参块，往 makeProfile 里再加一个分支会把它的认知复杂度再往上推，而它已经是
 * 基线里的 61-ERROR）。抽出后语义一字未改，只多一条「整份覆写」的早返回。
 *
 * 三个 C++ 布尔塌成**一个 mode 轴 + 一个正交 izb**——旧世界 `shadow_pass`/`shadow_pcss`/
 * `shadow_evssm` 三个独立布尔能写出「EVSSM 开着但图集不画」这类自相矛盾的组合，枚举下它
 * 表达不出来，塌缩方向因此唯一：evssm 档 → "evssm"（swim 臂的语义就是"开 EVSSM"），否则
 * shadowPass → "csm"，全关 → "none"。唯一剩下的矛盾组合显式拒绝而不是替调用方猜：EVSSM 要图集。
 */
void writeSyntheticShadowLua(const std::string& outDir, const ProfileOpts& o)
{
    // 整份覆写 = 调用方给臂定义（见 ProfileOpts::shadowLuaOverride）。mode 打偏在 lua 侧
    // 是**静默**的，所以消费方另有守卫：`shadow_kernel_scale.cpp::requireShadowEcho` 拿引擎
    // 自己的门回显对账。
    if (!o.shadowLuaOverride.empty()) {
        writeFile(outDir + "/shadow.lua", o.shadowLuaOverride);
        return;
    }
    if (o.evssmShadow && !o.shadowPass)
        throw std::runtime_error("ProfileOpts: evssmShadow=true 而 shadowPass=false 是矛盾组合"
            "（EVSSM 的矩图建立在 CSM 图集上）；批 N6b 起 mode 是单轴枚举，拒绝替你猜一档");
    const std::string shadowMode
        = o.evssmShadow ? "evssm" : (o.shadowPass ? "csm" : "none");
    // izb 与 mode 正交（旧 izb_shadow）。izb 开而 mode=none 是 gate_lint 的启动期 error
    // （mask 无内容 = 一帧阴影也不产，债-32），这里同样不写出去。
    const bool izb = o.izbShadow && o.shadowPass;
    // izb 子表是**唯一形态**（硬切 2026-10-17：扁平布尔 = 引擎启动报错），dump_frame 才按需写。
    // ⚠ 只校验两个顶层键：izb 子表的 on/grid/dump_frame 在解析器里是 `t["…"]` 形态，
    //   knownRootKeys 的 `root["…"]` 正则抽不到，写进校验集会把校验器自己变成假阳性源。
    assertKeys("shadow.lua", {"mode", "izb"}, knownRootKeys("rgl/shadow_config_impl.cppm"));
    writeFile(outDir + "/shadow.lua",
        "return { mode = \"" + shadowMode + "\", izb = { on = " + lb(izb)
        + (o.izbDumpFrame > 0 ? ", dump_frame = " + std::to_string(o.izbDumpFrame) : "")
        + " } }\n");
}

}  // namespace

/**
 * 机位 + 帧驱动 yaw 写进 camera.lua（**两条路径共用一份实现**）。
 *
 * 为什么必须共用：此前只有交付分支写，合成 profile 根本不产出 camera.lua
 * ⟹ `ProfileOpts::hasCamera` 在合成路径上**静默失效**——`temporal.cpp` 的注释一直声称
 * 「机位同时给两处」，实际只给了 RPC 一处，而 hasCamera 那条注释记的正是「RPC 之前的帧
 * 已经进了 TAA 历史」这条假绿教训。写出来的键仍然只在门开时出现（不写 = 引擎默认 = 逐位今天）。
 */
void applyCameraLua(const std::string& outDir, const ProfileOpts& o)
{
    if (!o.hasCamera && o.cameraFrameYaw == 0.0)
        return;  // 两边都没要求 ⟹ 不产出 camera.lua（保持既有合成判据的逐位行为）
    const std::string path = outDir + "/camera.lua";
    std::string cam = readFile(fs::exists(path) ? path : profileTemplate() + "/camera.lua");
    if (o.hasCamera) {
        const auto vec = [](const std::array<double, 3>& a) {
            std::ostringstream s;
            s.precision(9);
            s << "{ " << a[0] << ", " << a[1] << ", " << a[2] << " }";
            return s.str();
        };
        std::ostringstream fov;
        fov.precision(9);
        fov << o.cameraFov;
        subLuaLine(cam, "position", vec(o.cameraPosition), "camera.lua");
        subLuaLine(cam, "target", vec(o.cameraTarget), "camera.lua");
        subLuaLine(cam, "fovDegrees", fov.str(), "camera.lua");
    }
    if (o.cameraFrameYaw != 0.0) {
        // 插到 `mode` 行旁（两份 camera.lua 里 frame_yaw 都只有注释行，subLuaLine 找不到实体行）。
        // 键名拼错在 lua 侧是**静默**的，消费方的「运动激励在场性」守卫兜底。
        std::ostringstream yaw;
        yaw << std::setprecision(9) << "    frame_yaw = " << o.cameraFrameYaw << ",\n";
        insertAfterKey(cam, "camera.lua", "mode", yaw.str());
    }
    writeFile(path, cam);
}

/**
 * 升频 / aa 的键表（批 14.5 引入，批 14.6 换轨三态）：只返回**非默认**的键，默认一律不写
 * （写出的键越少，与交付配置的分歧面越小，同 taa 诊断键纪律）。
 *
 * 🔴 **两条 profile 路径都必须经过它**，这是本函数存在的唯一理由：交付路径的键已存在于
 * `configs/runtime.lua`（走 `replaceValue`），而合成路径的 `runtime.lua` 是**从零写的**
 * （那些键根本不存在，`replaceValue` 会抛"键不存在"）。只改交付那条的后果很具体：
 * `temporal-flicker --target edge`（合成 aa_target ⟹ **闭式唯一适用**的靶）上三条 FSR 臂
 * 会被静默当原生分辨率跑，却标着 "@0.75" 报进表里 —— 静默换臂。
 *
 * ⚠ `aa == Fsr` 时两枚子调参**总是显式写**（哪怕值 = 出厂默认）：出厂 `fsr` 块是
 * true/true = 设计点，"不写键 = 拿出厂值"会让想量纯空间的臂静默跑成时域+抖动。
 * "默认不写键"的纪律只适用于**引擎默认 = 本工具默认**的键，这里两者相反，写才是诚实。
 * 合成路径的嵌套形状（`temporal`/`jitter` 归进 `fsr = { … }`）由 makeProfile 负责，
 * 本函数只给扁平的 (键, 值) 对——交付路径的块内键恰好也是逐行 replaceValue 的形态。
 */
std::vector<std::pair<std::string, std::string>> fsrKeyValues(const ProfileOpts& o)
{
    std::vector<std::pair<std::string, std::string>> kv;
    if (o.renderScale != 1.0) {
        char sc[32];
        std::snprintf(sc, sizeof sc, "%g", o.renderScale);
        kv.emplace_back("render_scale", sc);
    }
    if (o.aa == Aa::Fsr) {
        kv.emplace_back("aa", "\"fsr\"");
        kv.emplace_back("temporal", lb(o.fsrTemporal));
        kv.emplace_back("jitter", lb(o.fsrJitter));
    } else if (o.aa == Aa::Taa) {
        kv.emplace_back("aa", "\"taa\"");
    }
    return kv;
}

/**
 * 升频 / aa 的守卫（批 14.6 三态化后从三条收成一条）。共同理由：这些错法**都不报错**，
 * 只是把臂悄悄换成另一条臂，然后读数被当成 FSR 读。判据工具最坏的失效不是崩，是
 * **静默换臂**（同 `requireJsonObject` 的教训：宁可在这里点名真因，也不要让下游抛一条
 * 指向错的一层的消息）。
 *
 * 删掉的两条旧守卫及理由：
 *  · 「fsrUpscaler 开了但 velocityField 没开」——aa=fsr 时 velocity 由**引擎解析期隐含
 *    打开**（单一来源在 applyAaKey），失效形态从根上消除；
 *  · 「fsrTemporal 与 taa 互斥」——aa 是一个槽位，非法组合**写不出来**（比运行期拒绝更强）。
 */
void checkFsrOpts(const ProfileOpts& o)
{
    if ((o.fsrTemporal || o.fsrJitter) && o.aa != Aa::Fsr)
        throw std::runtime_error(
            "fsrTemporal/fsrJitter 只在 aa == Fsr 时有意义（引擎只在 aa=fsr 时才接 FSR），"
            "其余态下开子选项 = 空开关（配置里写了、什么都没发生）。");
}

namespace {

// ── makeProfile 的按域切分（B5）─────────────────────────────────────────────────────
// 切法 = 配置域（一个域一个函数、一份 lua 文件一次写盘），不是按行数切。理由：这些函数
// 唯一的价值是「覆写顺序 + 默认态不写键」，按域切之后每个函数自己就能被读完，而
// `makeProfile` 退化成一张能一眼读完的顺序表。函数体逐字搬自原 `makeProfile`。

/// 交付配置模式的基底：把 `deliveryConfigDir` 里的 .lua/.json 全量拷进 profile 目录。
/// 本函数一个键都不覆写——覆写按域拆在下面各函数里（`makeProfile` 只留调用顺序）。
void copyDeliveryConfig(const std::string& outDir, const ProfileOpts& o)
{
    std::error_code ec;
    int copied = 0;
    for (fs::directory_iterator it(o.deliveryConfigDir, ec), e; it != e; it.increment(ec)) {
        if (!it->is_regular_file(ec))
            continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".lua" && ext != ".json")
            continue;
        fs::copy_file(it->path(), fs::path(outDir) / it->path().filename(),
            fs::copy_options::overwrite_existing);
        ++copied;
    }
    if (copied == 0)
        throw std::runtime_error("交付配置目录里没有 .lua/.json: " + o.deliveryConfigDir);
}

/// 交付配置模式 · runtime.lua 域：headless / llm_debug / 可选尺寸覆写 / 升频与 aa 键。
/// 从 `makeProfile` 原样搬出，覆写顺序一字未动。
void applyDeliveryRuntimeLua(const std::string& outDir, const ProfileOpts& o)
{
    std::string rt = readFile(outDir + "/runtime.lua");
    replaceValue(rt, "runtime.lua", "headless", "true");
    replaceValue(rt, "runtime.lua", "llm_debug", "false");  // RPC 形态与 llm 截图互斥
    // 尺寸覆写是**可选**的（默认关：现有判据全部钉 1280×720，隐式改尺寸 = 换口径）。
    // 表值必须走 replaceTableValue，理由见其注释（replaceValue 会截在花括号里的逗号；
    // 而参数**不含花括号**——含了就写出嵌套表，缺陷 B 的根因，函数内有形状自检）。
    if (o.overrideHeadlessSize)
        replaceTableValue(rt, "runtime.lua", "headless_size",
            std::to_string(o.size[0]) + ", " + std::to_string(o.size[1]));

    // ── 升频 / FSR 臂（批 14.5 量「闪烁↔鬼影」跷跷板）──
    // 键表与守卫都在 `fsrKeyValues` / `checkFsrOpts`，合成路径共用同一份（理由见彼处）。
    for (const auto& fk : fsrKeyValues(o))
        replaceValue(rt, "runtime.lua", fk.first, fk.second);
    writeFile(outDir + "/runtime.lua", rt);
}

/// 交付配置模式 · diagnostics.lua 域：rpc / rpc_port 恒覆写，picking 只在门开时写。
/// 同一个 `dg` 上改完再一次性写盘（早写一次 = picking 覆写被丢掉）。
void applyDeliveryDiagnosticsLua(const std::string& outDir, const ProfileOpts& o)
{
    std::string dg = readFile(outDir + "/diagnostics.lua");
    replaceValue(dg, "diagnostics.lua", "rpc", "true");
    replaceValue(dg, "diagnostics.lua", "rpc_port", std::to_string(8788));
    // ⚠ picking 覆写门在下面（同一个 dg 上继续改），所以这里**不**提前 writeFile

    // 三个**显式门**（默认关 = 一个键都不多写）。开着的语义 = 这次跑的就是 A/B 的另一侧，
    // 或需要 object_id 观测通道。见 ProfileOpts 的注释。
    if (o.overridePicking)
        replaceValue(dg, "diagnostics.lua", "picking", o.picking ? "true" : "false");
    writeFile(outDir + "/diagnostics.lua", dg);
}

/// 交付配置模式 · scene.lua 域：env / ao 强度、整段重写 taa 块、帧驱动 spin 诊断键。
/// 四个门默认全关 = 只重写 taa 块 = 逐位今天（`rewriteTaaBlock` 的默认态约定）。
void applyDeliverySceneLua(const std::string& outDir, const ProfileOpts& o)
{
    std::string sc = readFile(outDir + "/scene.lua");
    if (o.overrideEnvIntensity) {
        std::ostringstream v;
        v << std::setprecision(9) << o.envIntensity;
        // ⚠ 表名是 `environment`，**不是** `env`（实测第一版按 "env = {" 找，直接抛
        //   「交付配置 scene.lua 里没有表 env」）。而键名在 shader/文档里一律叫 env，
        //   正是这种"配置里叫 environment、口头叫 env"的错位最容易让覆写静默打偏。
        replaceValueInBlock(sc, "scene.lua", "environment", "intensity", v.str());
    }
    if (o.overrideAoStrength) {
        std::ostringstream v;
        v << std::setprecision(9) << o.aoStrength;
        replaceValueInBlock(sc, "scene.lua", "ao", "strength", v.str());
    }
    rewriteTaaBlock(sc, o);
    // 帧驱动 spin（诊断激励门）。**只在门开时写**：不写 = 交付配置原样 = 逐位今天。
    // 键名拼错在这里是**静默**的（scene.lua 的未知键引擎不打警告），所以消费方
    // temporal-motion 另有在场性守卫：运动组的连拍若逐位相同 = 键没生效 = 立刻停下。
    if (o.spinFrameAngle != 0.0) {
        std::ostringstream spin;
        spin << std::setprecision(9)
             << "    spin_frame_angle = " << o.spinFrameAngle << ",\n"
             << "    spin_axis = { " << o.spinAxis[0] << ", " << o.spinAxis[1] << ", "
             << o.spinAxis[2] << " },\n";
        insertAfterKey(sc, "scene.lua", "spin", spin.str());
    }
    writeFile(outDir + "/scene.lua", sc);
}

/// 合成 profile 的 render_graph.lua：整份拷模板（模板 = qa/configs/analytic，
/// 刀② 后其内容为空表 = 内建 deferred；variant 键与重写机制已随 RgGraphVariant 删除，
/// docs/图变体收敛_plan.md §6.1-2）。
void writeSyntheticRenderGraphLua(const std::string& outDir)
{
    // render_graph.lua 定义图本身，**必须整份拷**（缺文件虽回落内建 deferred，但合成
    // 路径保持"目录自足"的既有纪律：六份配置一个不缺，见 smoke.py CONFIG_FILES 同因）。
    fs::copy_file(profileTemplate() + "/render_graph.lua", outDir + "/render_graph.lua",
        fs::copy_options::overwrite_existing);
}

/// 合成 profile 的 runtime.lua（**从零写**，不是覆写）：尺寸 / llm_shot 骨架 + 升频与 aa 键。
/// 嵌套形状适配（`temporal`/`jitter` 归进 `fsr = { … }`）只在这一侧需要，理由见函数内注释。
void writeSyntheticRuntimeLua(const std::string& outDir, const ProfileOpts& o)
{
    // 升频 / aa 键（批 14.5 引入，批 14.6 三态化）。合成路径的 runtime.lua 是**从零写的**、
    // 这些键根本不存在 ⟹ 必须在这里追加。只改交付那条路径的后果：`--target edge`（闭式唯一
    // 适用的靶）上的 FSR 臂静默跑成原生分辨率，却标着 "@0.75" 报进表里。
    // 键源与交付路径**同一个 fsrKeyValues**；这里只多做一步形状适配：`temporal`/`jitter`
    // 归进嵌套的 `fsr = { … }` 表（引擎只从块内读它们，扁平写出 = 静默读默认）。
    std::string fsrLua;
    std::string fsrBlock;
    for (const auto& fk : fsrKeyValues(o)) {
        if (fk.first == "temporal" || fk.first == "jitter") {
            fsrBlock += (fsrBlock.empty() ? "  fsr = { " : ", ") + fk.first + " = " + fk.second;
            continue;
        }
        fsrLua += "  " + fk.first + " = " + fk.second + ",\n";
    }
    if (!fsrBlock.empty())
        fsrLua += fsrBlock + " },\n";
    writeFile(outDir + "/runtime.lua",
        "return { headless = true, headless_size = { " + std::to_string(o.size[0]) + ", "
            + std::to_string(o.size[1]) + " },\n"
          "  win_size = { 800, 600 }, show_after = -1, present_mode = \"immediate\",\n"
          "  ui_font = \"\", ui_font_size = 18.0,\n"
          + fsrLua
          // ⚠ `llm_shot` 必须**自成一块**、每个键以 `,` 收尾：`overrideValueInTable` 的表头按
          // **顶层键**口径找（行首），值跨度按「到 `,` 或换行为止」算。旧写法把三个键与
          // `llm_debug` 挤在同一行、`delay_frames = 8 } }` 收尾 ⟹ 门开时表头找不到（抛）、
          // 值跨度吞掉尾部 ` } }`（实测把 runtime.lua 写坏成 lua 语法错）。lua 语义一字未变。
          + "  llm_debug = false,\n"
            "  llm_shot = {\n"
            "    file = \"screenshots/unused.png\",\n"
            "    size = { " + std::to_string(o.size[0]) + ", " + std::to_string(o.size[1])
            + " },\n"
              "    delay_frames = 8,\n"
              "  },\n"
              "}\n");
}

/// 合成 profile 的 diagnostics.lua：先拿解析器源码校验键名，再整份写出。
void writeSyntheticDiagnosticsLua(const std::string& outDir, const ProfileOpts& o)
{
    const std::set<std::string> diagKeys = {"no_vvl", "no_hdr", "no_present", "vtx_canary",
        "texel_canary", "dump_gbuf", "style_dist", "tex_dist", "texture_force_size",
        "texture_force_white", "texture_force_unorm", "profiler", "imgui", "config_reload",
        "shader_reload", "picking", "rpc", "rpc_port"};
    assertKeys("diagnostics.lua", diagKeys, knownRootKeys("rgl/diagnostics_impl.cppm"));
    writeFile(outDir + "/diagnostics.lua",
        std::string(
            "return { no_vvl = false, no_hdr = false, no_present = false, vtx_canary = false,\n"
            "  texel_canary = -1, dump_gbuf = -1, style_dist = false, tex_dist = false,\n"
            "  texture_force_size = 0, texture_force_white = false, texture_force_unorm = false,\n"
            "  profiler = 0, imgui = false, config_reload = false, shader_reload = false,\n"
            "  picking = ")
        + lb(o.picking) + ", rpc = true, rpc_port = 8788 }\n");
}

/// 合成 profile 的 scene.lua：entities / styles / environment / light / spin / ao
/// （+ TAA 时的 taa 调参段）。styles 四风格必须显式写，理由见函数内那段长注释。
void writeSyntheticSceneLua(const std::string& outDir, const ProfileOpts& o)
{
    std::set<std::string> sceneKeys
        = {"entities", "styles", "material_styles", "deferred_styles", "environment", "light",
           "spin", "cube", "exposure", "ao"};
    if (o.aa == Aa::Taa)
        sceneKeys.insert("taa");  // 只在 TAA 真在图时写这段（批 14.6：开关在 runtime 的 aa，本块只剩调参）
    assertKeys("scene.lua", sceneKeys, knownRootKeys("rgl/scene_config_impl.cppm"));
    const auto j3 = [](std::array<double, 3> a) {
        return std::to_string(a[0]) + ", " + std::to_string(a[1]) + ", " + std::to_string(a[2]);
    };
    // entities：默认一份 glb；entitiesLua 非空 = 整条由调用侧给（内建合成靶 aa_target
    // 没有 glb 文件）。拼错 type 不会静默——parseEntityItem 直接启动报错列合法值。
    const std::string entities = o.entitiesLua.empty()
        ? "    entities = { { type = \"model\", file = \"models/" + o.glb + "\" } },\n"
        : "    entities = { " + o.entitiesLua + " },\n";
    // TAA 调参段（批 12.6 引入；批 14.6：**不再写 enabled**——开关在 runtime 的 aa 三态）：
    // 其余走引擎默认（运动补偿开）——写出来的键越少，漂移面越小。两个消融开关各有用途：
    // jitter_scale=0 = 只留时域累积；feedback=1 = 只抖不滤（输出 = 本帧，temporal 的阳性
    // 锚点组。别写 0——0 是「只要裁剪后的历史、完全丢本帧」的退化反馈环，测出来的不是
    // 闪烁本体）。
    const std::string taa = o.aa == Aa::Taa
        ? "    taa = { feedback = " + std::to_string(o.taaFeedback)
              + ", jitter_scale = " + std::to_string(o.taaJitterScale)
              + ", base_space = " + lb(o.taaBaseSpace)
              // 输入侧重建核（批 13.8）：0 = 双线性 = 引擎默认 = 不写键
              + (o.taaInputKernel != 0.0
                     ? ", input_kernel = " + std::to_string(o.taaInputKernel)
                     : "")
              // 历史裁剪消融门（批 13.11）：0 = 现行 = 不写键。合成靶也要能跑——
              // 「修法不得是 bistro 专用」这条护栏要求同一臂在合成靶上同向。
              + (o.taaClipMode != 0.0
                     ? ", clip_mode = " + std::to_string(o.taaClipMode) + ", clip_soft = "
                           + std::to_string(o.taaClipSoft)
                     : "")
              + " },\n"
        : "";
    // styles 必须显式写（switch 首战抓到的静默坑）：缺表回落默认 pbr→model_pbr，
    // 而 model_pbr.frag **不 import shadow/env**——阴影与 env 开关全部
    // 退化为逐位相同的假象（投影侧照画、采样侧不存在）。model_pbr_ibl 在
    // env=0/shadow=off 下直接光与 model_pbr 逐式相同（IBL 项 ×intensity=0 归零），
    // 闭式解前提不破。
    // 表 = **交付配置 configs/scene.lua 的全量四风格 + 材质名连连看**（13.1b 补：此前只写
    // unlit/pbr 两条，bistro 的 `light`/`vegetation` 材质直接启动失败
    // "style not in scene.lua styles: light"；缺 material_styles 则 Emissive/Foliage
    // 全落 pbr = 测的不是交付路径）。对无这些材质的探针场景（cornell_box / aa_target）
    // 逐位 inert——连连看按材质名命中，探针场景无命中项。
    const std::string styles =
        "    styles = { unlit = \"model\", pbr = \"model_pbr_ibl\", vegetation = "
        "\"model_vegetation\", light = \"model_light\" },\n"
        "    material_styles = { { pattern = \"Emissive\", style = \"light\" },\n"
        "      { pattern = \"Foliage\", style = \"vegetation\" } },\n"
        "    deferred_styles = { unlit = \"deferred_geometry_unlit\",\n"
        "      vegetation = \"deferred_geometry_vegetation\",\n"
        "      light = \"deferred_geometry_light\" },\n";
    writeFile(outDir + "/scene.lua",
        "return {\n" + entities
          + styles
        // 环境（intensity / skybox 由 ProfileOpts 给；env 文件与交付配置同一张 .hdr）。
        // ⚠ 资产**恒发**：styles 表引用 model_pbr_ibl，而引擎启动守卫拒「env 未启用 +
        //   pbr_ibl」（env 描述符未映射 = 未定义采样，实测报错文案如此）——整段省略走不通。
        //   deferred 天穹不读 skybox 标志（forward draw 侧 knob，随 forward 一起删）的污染
        //   问题在引擎侧修：light.frag 天穹分支门控 `intensity > 0`（env 总闸语义，
        //   2026-10-18 探针重立定案，docs/图变体收敛_plan.md §6.2）。
          + "    environment = { hdr = \"env/san_giuseppe_bridge_4k.hdr\", hdr_size = 2048,\n      intensity = "
            + std::to_string(o.envIntensity) + ", skybox = " + lb(o.skybox) + " },\n"
          + "    light = { direction = { "
            + j3(o.lightDir) + " }, color = { 1.0, 1.0, 1.0 },\n      intensity = "
            + std::to_string(o.lightIntensity) + ", ambient = { " + j3(o.ambient) + " } },\n"
            + "    spin = false, cube = false, exposure = " + std::to_string(o.exposure)
            + ",\n    ao = { strength = " + std::to_string(o.aoStrength) + " },\n"
            + taa + "}\n");
}

}  // namespace

/**
 * 生成一份 profile 目录。**本函数只负责两件事**：先过两条路径共用的守卫，再按**固定顺序**
 * 调用各配置域的覆写/写出函数。顺序就是口径——后一条覆写踩在前一条的结果上，换序 = 换一台
 * 仪器，所以域函数怎么切都不许动这里的调用次序。
 *
 * 两条路径（`deliveryConfigDir` 有没有给）各自的域函数都在本文件：交付侧 = 读回仓库配置逐键
 * 覆写（`applyDelivery*Lua`），合成侧 = 从零写整份 lua（`writeSynthetic*Lua`）。
 */
std::string makeProfile(const std::string& outDir, const ProfileOpts& o)
{
    // 守卫放在**最前面**：两条 profile 路径（交付 / 合成）都必须过它，放后面会被早退路径绕掉。
    checkFsrOpts(o);
    // 第 4 例外门（覆写**交付** shadow.lua 的软影轴）只在交付配置路径里有实现。放在最前面
    // 的理由：门开了却走合成路径 = **静默不生效** = 在出厂值上下判据，必须在生成期就拒。
    // 第 5 例外门（llm 截图形态）**两条路径都有实现**——它只改 runtime.lua 的 llm_debug /
    // llm_shot 与 diagnostics.lua 的 rpc，而合成路径写出的 runtime.lua 里这些键本来就在
    // （`applyLlmShotGate` 逐键覆写 + 写后自检，键不在就抛），故不再要求 deliveryConfigDir。
    if (o.deliveryConfigDir.empty() && o.overrideShadowRoute)
        throw std::runtime_error(
            "overrideShadowRoute 是交付配置模式的例外门（它覆写的是**交付** shadow.lua 的 "
            "mode/izb/sun_degrees），而本 opts 没给 deliveryConfigDir ⟹ 这个门根本没有实现路径"
            "（合成 profile 请直接写 shadowPass / shadowLuaOverride 等字段）。"
            "开着门却静默不生效 = 在出厂值上下判据，拒绝生成");
    fs::create_directories(outDir);
    // ── 交付配置模式（批 13.1e）：基底 = 仓库 configs/，只覆写 headless / rpc / taa ──
    // 判据必须在**用户实际跑的那份配置**上求值（ProfileOpts::deliveryConfigDir 的注释里
    // 记着这条是怎么用一轮假绿换来的）。刻意**不**在这里重抄 env/ao/shadow/light：
    // 那些键的真相在交付配置里，工具再抄一份就是第二份真相，会漂移。
    if (!o.deliveryConfigDir.empty()) {
        copyDeliveryConfig(outDir, o);
        applyDeliveryRuntimeLua(outDir, o);
        applyDeliveryDiagnosticsLua(outDir, o);
        applyDeliverySceneLua(outDir, o);
        // 机位写进 camera.lua（**从第 1 帧生效**，理由见 ProfileOpts::hasCamera 的注释）
        applyCameraLua(outDir, o);
        // 第 4/5 例外门（批 N6d `shadow-softness`）：门闭 = 函数第一行返回 = 交付配置
        // 一个键都不多写 = 现有判据逐位不变。放在既有写盘**之后**、直接读写 profile 目录，
        // 是为了让 makeProfile 本体不掺这两个门的 if（护栏：认知复杂度；语义：
        // 「门闭 = 逐位不变」由门函数自己保证，不靠调用侧记得加 if）。
        applyShadowRouteGate(outDir, o);
        applyLlmShotGate(outDir, o);
        return outDir;
    }
    // 合成路径：render_graph 整拷 → 两份 json → runtime / diagnostics / shadow / scene 逐份写。
    writeSyntheticRenderGraphLua(outDir);
    for (const char* j : {"log_level.json", "shaderRefLogLevel.json"})
        fs::copy_file(profileTemplate() + "/" + j, outDir + "/" + j,
            fs::copy_options::overwrite_existing);
    writeSyntheticRuntimeLua(outDir, o);
    writeSyntheticDiagnosticsLua(outDir, o);
    writeSyntheticShadowLua(outDir, o);  // 阴影门（整段实现见该函数注释）
    writeSyntheticSceneLua(outDir, o);
    applyCameraLua(outDir, o);  // 合成路径同样支持机位/帧驱动 yaw（见 applyCameraLua 注释）
    // 第 5 例外门（钉死帧号的一次性截图形态）。门闭 = 函数第一行返回 = 现有合成判据逐位不变。
    applyLlmShotGate(outDir, o);
    return outDir;
}

}  // namespace RGL::imgqa
