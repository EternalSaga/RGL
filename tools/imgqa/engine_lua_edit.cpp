// 交付配置 lua 文本的编辑原语（缝与「为什么只有这八枚进头」见 `engine_lua_edit.h`）。
// 本 TU 只做文本操作：不认识 ProfileOpts、不读盘也不写盘。

#include "engine_lua_edit.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace RGL::imgqa {

namespace {

// ── 覆写器的内部定位件 ──────────────────────────────────────────────────────────────
// 下面五枚只服务本 TU 的 `overrideTopLevelValue` / `overrideValueInTable`（两种定位口径的
// 判据本体），不跨 TU ⟹ 留在匿名 namespace：跨 TU 暴露面 = 真被用到的面。

/// 定位 `key =` 之后**值**的跨度：标量吃到 `,`/换行，表值吃到配对 `}`（嵌套计数）。
/// ⚠ 不支持"引号内含逗号/花括号"的值（`mode = "csm"` / 路径都不含，本工具的键已逐一核对）。
std::pair<size_t, size_t> valueSpanAt(const std::string& text, size_t from,
    const std::string& file, const std::string& what)
{
    size_t b = from;
    while (b < text.size() && (text[b] == ' ' || text[b] == '\t'))
        ++b;
    if (b < text.size() && text[b] == '{') {
        for (size_t i = b, depth = 0; i < text.size(); ++i) {
            if (text[i] == '{')
                ++depth;
            else if (text[i] == '}' && --depth == 0)
                return {b, i + 1 - b};
        }
        throw std::runtime_error("交付配置 " + file + " 的 `" + what + "` 表值没有收尾");
    }
    const size_t e = text.find_first_of(",\n", b);
    return {b, (e == std::string::npos ? text.size() : e) - b};
}

/// 顶层口径的位置判据：键必须在行首（允许缩进）。
bool hitAtTopLevel(const std::string& text, size_t p, size_t ls)
{
    return text.find_first_not_of(" \t", ls) == p;
}

/// 表内口径的位置判据：左邻必须是空白 / `{` / `,`（键名不得是更长标识符的后缀），
/// 且不得落在本行 `--` 之后（行内注释里的键名不是覆写目标）。
bool hitInsideTable(const std::string& text, size_t p, size_t ls, size_t end)
{
    const char left = p == ls ? ' ' : text[p - 1];
    if (p != ls && !(left == ' ' || left == '\t' || left == '{' || left == ','))
        return false;
    const size_t dash = text.find("--", ls);
    return dash == std::string::npos || dash >= end || dash >= p;
}

/// 命中的 `key` 算不算覆写目标（两种口径的判据本体，见 findKeyHits 注释）。
/// 拆成独立函数的理由不是审美：本文件的样式护栏在认知复杂度 15，把"扫描"与"判据"混在
/// 一个 while 里直接冲到 33（error 级）。
bool isKeyHit(const std::string& text, size_t p, const std::string& key, size_t end,
    bool lineStartOnly)
{
    const size_t ls = p == 0 ? 0 : text.rfind('\n', p - 1) + 1;
    if (text.compare(ls, std::min<size_t>(2, end - ls), "--") == 0)
        return false;  // 整行注释
    if (!(lineStartOnly ? hitAtTopLevel(text, p, ls) : hitInsideTable(text, p, ls, end)))
        return false;
    size_t i = p + key.size();
    while (i < end && (text[i] == ' ' || text[i] == '\t'))
        ++i;
    return i < end && text[i] == '=';  // 键名是更长标识符的前缀时这里挡住
}

/// 找 `key =` 的出现位置。两种口径：
///  - **顶层**（lineStartOnly=true，同 insertAfterKey）：键必须在行首（允许缩进）——`izb` 因此
///    不会命中 `izb_shadow = …`，也不会命中注释里 `"evssm" = 远场软影矩图` 那种散文。
///  - **表内**（lineStartOnly=false）：`evssm = { sun_degrees = 0.53, … }` 的键**和表头同一行**，
///    行首口径永远找不到它。改要求左邻是空白 / `{` / `,`，并排除"本行 `--` 之后"的命中。
std::vector<size_t> findKeyHits(const std::string& text, const std::string& key,
    size_t from = 0, size_t to = std::string::npos, bool lineStartOnly = true)
{
    std::vector<size_t> hits;
    const size_t end = std::min(to, text.size());
    for (size_t pos = from; (pos = text.find(key, pos)) != std::string::npos;) {
        if (pos >= end)
            break;
        if (isKeyHit(text, pos, key, end, lineStartOnly))
            hits.push_back(pos);
        pos += key.size();
    }
    return hits;
}

}  // namespace

/**
 * 替换 lua 里**第一条以 `key` 开头的未注释行**的整行值（行尾注释一并丢掉——这是生成出来的
 * profile 副本，不是仓库配置，留着解释新值的旧注释更坏）。
 *
 * 为什么手写行扫描而不是 `std::regex`：`std::regex` 的 ECMAScript **没有 multiline**，`^` 只
 * 匹配整个输入的开头，`(?m)` 前缀会直接语法错。行首是空白但不允许是 `--`：camera.lua 里
 * 排着一整列注释掉的备选机位，那些必须原样留着。
 */
void subLuaLine(std::string& text, const std::string& key, const std::string& value,
    const std::string& where)
{
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    bool hit = false;
    while (std::getline(in, line)) {
        const auto first = line.find_first_not_of(" \t");
        if (!hit && first != std::string::npos && line.compare(first, key.size(), key) == 0
            && line.substr(first, 2) != "--") {
            const bool comma = line.find(',') != std::string::npos;
            out << std::string(first, ' ') << key << " = " << value << (comma ? "," : "") << "\n";
            hit = true;
            continue;
        }
        out << line << "\n";
    }
    if (!hit)
        throw std::runtime_error(where + " 里没有未注释的 " + key + " 行");
    text = out.str();
}

/// 交付配置模式：把 `key = <旧值>,` 一行的**值**换成新值（只认第一处，且必须找到）。
/// 找不到就抛——静默不替换 = 判据跑在**交付配置的原值**上而工具以为改了，
/// 与「拼错键 = 静默用默认值」同罪（makeProfile 头注释）。
void replaceValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newValue)
{
    const std::string head = key + " = ";
    const auto at = text.find(head);
    if (at == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 里没有键 `" + key
            + "`——覆写失败，拒绝在旧值上下判据");
    const auto eol = text.find_first_of(",\n", at + head.size());
    if (eol == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 的键 `" + key + "` 形状不认识");
    text.replace(at + head.size(), eol - (at + head.size()), newValue);
}

/// 交付配置模式：**限定在某个表内**改写 `key = 值`（`env.intensity` 这种）。
/// 为什么不能直接用 `replaceValue`：`intensity` 在 scene.lua 里出现两次（`env.intensity`
/// 与 `light.intensity`），`replaceValue` 取**第一个命中**——今天恰好是 env 那个，但那是
/// **文档顺序**在替我们做决定。键不存在于该表内 = 抛（拒绝在旧值上下判据）。
void replaceValueInBlock(std::string& text, const std::string& file, const std::string& block,
    const std::string& key, const std::string& newValue)
{
    const std::string bhead = block + " = {";
    const auto bpos = text.find(bhead);
    if (bpos == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 里没有表 `" + block + "`，无法覆写 " + key);
    // 表尾边界（债-93）：旧版从表头一路找到**文件尾** ⟹ 目标表缺该键时静默命中后面那张表
    // （`environment` 没有 `intensity` 而其后的 `light` 有 ⟹ 改掉的是 `light.intensity`，
    // 一声不响——改的就是判据输入）。边界口径与 `overrideValueInTable` 一致：`valueSpanAt`
    // 按花括号配对收尾，表跨度内未命中 = 抛，**不越界去别的表找**。（本原语找表头是字面量
    // `"block = {"`，排版必须恰好一个空格——那是它与此前那条注释 ② 的既有分工，不在本次范围。）
    const auto span = valueSpanAt(text, bpos + bhead.size() - 1, file, block);
    const std::string head = key + " = ";
    const auto at = text.find(head, bpos + bhead.size());
    if (at == std::string::npos || at >= span.first + span.second)
        throw std::runtime_error("交付配置 " + file + " 的 `" + block + "` 表里没有键 `" + key
            + "`（表跨度内未命中 = 不越界去别的表找，那会改错块）");
    const auto eol = text.find_first_of(",\n", at + head.size());
    if (eol == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 的键 `" + block + "." + key + "` 形状不认识");
    text.replace(at + head.size(), eol - (at + head.size()), newValue);
}

/// 交付配置模式：把表值键 `key = { a, b }` 整体改写为 `key = { newContent }`。
/// 为什么不能复用 `replaceValue`：它按第一个 `,` 或换行收尾，而表值**内部就有逗号**
/// （`headless_size = { 1280, 720 }` ⟹ 截在 1280 后 = 留下 `720 },` = Lua 语法错）。
/// ⚠ `newContent` 是**花括号内部**的内容（不含花括号）——上一版把开括号混进参数、函数
/// 又补一次，写出 `headless_size = { { 2560, 1440 } }`：语法合法（嵌套表）但 loader 把
/// [1] 当数字读 = ConfigLoadFailed，REF 超采样组整组引擎启动即退（本轮缺陷 B 的根因）。
/// 形状自检（写出后必须匹配 `key = { <num>, <num> }`）就钉住这一类「语法合法、语义是
/// 嵌套表」的失效——只测「正则命中数」抓不到它，命中了恰恰说明写坏了。
void replaceTableValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newContent)
{
    const std::string head = key + " = {";
    const auto at = text.find(head);
    if (at == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 里没有表值键 `" + key
            + "`——覆写失败，拒绝在旧值上下判据");
    const auto close = text.find('}', at + head.size());
    if (close == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 的表值键 `" + key + "` 没有收尾");
    text.replace(at + head.size(), close - (at + head.size()) + 1,
        " " + newContent + " }");
    // 写出后的形状自检：扁平数值表 `{ 数字, 数字 }`，**不许**出现嵌套花括号。
    const std::regex flatRx(key + R"aaa(\s*=\s*\{\s*-?[0-9]+(\.[0-9]+)?\s*,\s*-?[0-9]+(\.[0-9]+)?\s*\})aaa");
    if (!std::regex_search(text, flatRx))
        throw std::runtime_error("交付配置 " + file + " 的 `" + key
            + "` 改写后不是扁平数值表（疑似嵌套花括号 = 引擎 loader 会把表当数字读 = "
              "ConfigLoadFailed）。改写前的原文片段: "
            + text.substr(at, std::min<size_t>(64, text.size() - at)));
}

/// 交付配置模式：在**第一条未注释的 `key = ...` 行**之后插入若干行（不碰原行）。
/// 与 `subLuaLine` 的区别 = 原行要留着（`spin = false` 是交付配置的既有键，本工具只
/// 追加同级的诊断键）；与 `rewriteTaaBlock` 的「插到 `return {` 之后」的区别 =
/// 插到兄弟键旁边，读生成出来的 profile 时键的归属一眼可见。
void insertAfterKey(std::string& text, const std::string& file, const std::string& key,
    const std::string& block)
{
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    bool hit = false;
    while (std::getline(in, line)) {
        out << line << "\n";
        const auto first = line.find_first_not_of(" \t");
        if (!hit && first != std::string::npos && line.compare(first, key.size(), key) == 0
            && line.substr(first, 2) != "--"
            // 键名边界：`spin` 不得命中同前缀的兄弟键（scene.lua 里就有 `spin_frame_angle`，
            // 插到它后面 = 插到自己刚写的那两行之后，第二次调用直接跑偏）。
            && (first + key.size() >= line.size()
                || !(std::isalnum(static_cast<unsigned char>(line[first + key.size()]))
                    || line[first + key.size()] == '_'))) {
            out << block;
            hit = true;
        }
    }
    if (!hit)
        throw std::runtime_error("交付配置 " + file + " 里没有未注释的 " + key
            + " 行——诊断键无处可插，拒绝在缺键的配置上下判据");
    text = out.str();
}

// ── 交付配置覆写器的两个底层件（批 N6d 第 4/5 例外门）────────────────────────────
//
// 为什么不能复用 replaceValue / replaceValueInBlock（那两个服务的是"标量键 + 文档顺序
// 里唯一的表"）：
//  ① `izb = { on = true, grid = 512, dump_frame = 0 }` 的**值里有逗号**，
//     `replaceValue` 按第一个 `,` 收尾 ⟹ 截在 `{ on = true` 后面 = 留下 `grid = 512 }`
//     = Lua 语法错。引擎硬切后只收子表形态（扁平布尔 = 启动报错），但覆写器是文本层，
//     两种跨度都得能吃（标量分支防旧形态残留，见 selftest_shadow 面 C）。
//  ② `pcss  = {` 在 configs/shadow.lua 里是**两个空格**（对齐排版），
//     `replaceValueInBlock` 找字面 `"pcss = {"` 直接抛「没有表 pcss」——排版改一个空格
//     就让覆写器打偏，而打偏的方向是"抛"，看起来还像是配置的问题。
//  ③ 表内找键必须**限定在该表的括号跨度内**：`sun_degrees` 在 pcss 与 evssm 两块里各有
//     一份（两档分立是消债-16⑤），"从 pcss 块开头往后找第一个"在 pcss 缺键时会静默
//     命中 evssm 那份 = 改错档、读数却标着 pcss。

/// 交付配置覆写器：**顶层**键整值替换（标量与表值都吃）。找不到 / 命中多处 / 形状不认识
/// 一律抛——覆写失败必须在生成期炸，不能到判据里发现（同"三个例外"的纪律）。
void overrideTopLevelValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newValue)
{
    const auto hits = findKeyHits(text, key);
    if (hits.size() != 1)
        throw std::runtime_error("交付配置 " + file + " 里顶层键 `" + key + "` 命中 "
            + std::to_string(hits.size()) + " 处（要恰好 1 处）——覆写目标不唯一或不存在，"
              "拒绝在旧值上下判据");
    const size_t eq = text.find('=', hits[0] + key.size());
    if (eq == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 的键 `" + key + "` 后面没有 `=`");
    const auto span = valueSpanAt(text, eq + 1, file, key);
    text.replace(span.first, span.second, newValue);
}

/// 读回**顶层键当前的值文本**（去空白），供覆写器写后自检用。
/// 与 `overrideTopLevelValue` 走同一套定位判据 ⟹ 自检问的是"引擎会读到的那个值是什么"，
/// 而不是"文件里有没有出现过这串字符"（后者会被注释里的同名示例骗到）。
std::string topLevelValueOf(const std::string& text, const std::string& key,
    const std::string& file)
{
    const auto hits = findKeyHits(text, key);
    if (hits.size() != 1)
        throw std::runtime_error("交付配置 " + file + " 里顶层键 `" + key + "` 命中 "
            + std::to_string(hits.size()) + " 处（要恰好 1 处）——自检无法定位");
    const size_t eq = text.find('=', hits[0] + key.size());
    // 与 `overrideTopLevelValue` 同形的防御守卫（债-93）：缺了它 `eq + 1` 在 npos 处回绕成 0，
    // 会把**文件开头**的值读成自检读数。今天 isKeyHit 已保证命中必带 `=`（不可达），但两条
    // 同定位判据的路径必须同守卫——漂移只会发生在"其中一条被改"的那天。
    if (eq == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 的键 `" + key + "` 后面没有 `=`");
    const auto span = valueSpanAt(text, eq + 1, file, key);
    const std::string v = text.substr(span.first, span.second);
    const auto a = v.find_first_not_of(" \t\r\n");
    if (a == std::string::npos)
        return "";
    return v.substr(a, v.find_last_not_of(" \t\r\n") - a + 1);
}

/// 交付配置覆写器：**限定在某个表的括号跨度内**整值替换（表头允许任意空白，
/// `pcss  = {` 这种对齐排版也认）。见文件头 ②③ 两条为什么必须有它。
void overrideValueInTable(std::string& text, const std::string& file, const std::string& table,
    const std::string& key, const std::string& newValue)
{
    // 表头：先按"顶层键"口径找 `table`，再要求其后（跳过 `=` 与空白）是 `{`
    size_t head = std::string::npos;
    for (const auto hit : findKeyHits(text, table)) {
        size_t i = hit + table.size();
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '='))
            ++i;
        if (i < text.size() && text[i] == '{') {
            head = i;
            break;
        }
    }
    if (head == std::string::npos)
        throw std::runtime_error("交付配置 " + file + " 里没有表 `" + table + "`（或它不是表）"
            "，无法覆写 " + key + "——拒绝在旧值上下判据");
    const auto span = valueSpanAt(text, head, file, table);
    const auto inner = findKeyHits(text, key, span.first, span.first + span.second, false);
    if (inner.empty())
        throw std::runtime_error("交付配置 " + file + " 的 `" + table + "` 表里没有键 `" + key
            + "`（表跨度内未命中 = 不越界去别的表找，那会改错块）");
    if (inner.size() > 1)
        throw std::runtime_error("交付配置 " + file + " 的 `" + table + "` 表里 `" + key
            + "` 命中 " + std::to_string(inner.size()) + " 处，覆写目标不唯一");
    const size_t eq = text.find('=', inner[0] + key.size());
    const auto val = valueSpanAt(text, eq + 1, file, table + "." + key);
    text.replace(val.first, val.second, newValue);
}

}  // namespace RGL::imgqa
