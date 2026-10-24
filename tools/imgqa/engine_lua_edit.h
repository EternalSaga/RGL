#pragma once
//
// engine_lua_edit.h —— 交付配置 lua 文本的**编辑原语**（实现 = `engine_lua_edit.cpp`）
//
//   为什么拆这一刀：这十五枚原语是「在一份 lua 文本里把一个键的值换掉」的纯文本操作，它们不
//   认识 `ProfileOpts`、不认识 shadow.lua / scene.lua，也不读写盘；而消费它们的是「把 ProfileOpts
//   翻译成 lua 覆写」的配置语义层（`engine_profile.cpp`）。两条改动缝本来就不同——定位判据/容差
//   漂了改这里，加一个诊断键改那里。文件代码行护栏（500）把这条缝逼了出来。
//
//   为什么只有这九枚进头：`valueSpanAt` / `scalarSpanTo` / `hitAtTopLevel` / `hitInsideTable` /
//   `isKeyHit` / `findKeyHits` 只被本 TU 的覆写器消费，留在 `engine_lua_edit.cpp` 的匿名
//   namespace 里——**跨 TU 暴露面 = 真被用到的面**，多暴露一枚就多一处以后不敢删的契约。
//   （`bracesBalanced` 是例外中的例外：它给的是 `engine_profile.cpp` 的生成期语法护栏，
//   判据本体住文本层、读盘与点名住配置层。）
//
//   行为契约（逐条都有注释在实现处展开，这里只挂号）：覆写目标定位失败一律**抛**，绝不静默
//   不替换——「配置看起来写了、判据看起来在跑、实际跑在出厂值上」是这类仪器最坏的失效。
//
#include "imgqa.h"

#include <array>
#include <string>

namespace RGL::imgqa {

/// 替换 lua 里第一条以 `key` 开头的未注释行的整行值（行尾注释一并丢掉）。找不到 = 抛。
void subLuaLine(std::string& text, const std::string& key, const std::string& value,
    const std::string& where);

/// 把 `key = <旧值>,` 一行的**值**换成新值（只认第一处）。找不到 = 抛。
void replaceValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newValue);

/// 限定在某个表内改写 `key = 值`（`env.intensity` 这种）。表或键不在 = 抛。
void replaceValueInBlock(std::string& text, const std::string& file, const std::string& block,
    const std::string& key, const std::string& newValue);

/// 把表值键 `key = { a, b }` 整体改写为 `key = { newContent }`（`newContent` **不含花括号**）。
void replaceTableValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newContent);

/// 在**第一条未注释的 `key = ...` 行**之后插入若干行（不碰原行）。找不到 = 抛。
void insertAfterKey(std::string& text, const std::string& file, const std::string& key,
    const std::string& block);

/// **顶层**键整值替换（标量与表值都吃）。命中数 != 1 / 形状不认识 = 抛。
void overrideTopLevelValue(std::string& text, const std::string& file, const std::string& key,
    const std::string& newValue);

/// 读回**顶层键当前的值文本**（去空白），供覆写器写后自检用。
std::string topLevelValueOf(const std::string& text, const std::string& key,
    const std::string& file);

/**
 * lua 的 `{ a, b, c }` 文本 → 三元数组（`topLevelValueOf` 的下游读法，两台仪器共用）。
 *
 * 为什么进头而不是各抄一份：`blend-sort` 与 `blend-lit` 都要拿 camera.lua 的 position/target
 * 与 manifest 对账（不一致就拒跑），解析规则是同一份。两处各写一遍 = 两个「什么形态算合法
 * 三元组」的判据，而它们可以在同一天各自漂一点而都不报错。
 * 解析失败一律**抛**——形状守卫与 manifest 侧同一条口径（「本命令拒绝猜」）。
 */
[[nodiscard]] std::array<double, 3> parseLuaVec3(const std::string& text, const std::string& what);

/// **限定在某个表的括号跨度内**整值替换（表头允许任意空白）。表/键不在或不唯一 = 抛。
void overrideValueInTable(std::string& text, const std::string& file, const std::string& table,
    const std::string& key, const std::string& newValue);

/// 花括号是否配对（跳过 `--` 行注释后计数）。生成期语法护栏的判据本体，见实现处注释。
[[nodiscard]] bool bracesBalanced(const std::string& src);

}  // namespace RGL::imgqa
