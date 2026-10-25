#ifndef RGL_IMGQA_CLI_H
#define RGL_IMGQA_CLI_H
// imgqa CLI 注册表 —— 子命令 / flag / 默认值 / 帮助文本的**单一真相源**（批次 B）。
// 为什么必须有这张表、以及它堵掉的是哪次事故 → cli.cpp 头注释。
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace RGL::imgqa {

/// 参数取值形态。Bool = 原来的 `P.has("--x")` 形态（存在即真，没有"默认值字符串"）。
enum class FlagKind { String, Number, Bool };

/// 一个 flag 的全部真相：默认值 + 一行说明 + 取值形态。
struct FlagSpec {
    const char* name;  // "--burst"
    const char* def;   // "96"；Bool 填 nullptr
    const char* help;  // 一行说明（`imgqa -h <子命令>` 打印）；可 nullptr
    FlagKind kind;
    /// 值与「有没有出现」都被读的 flag（probe 的 --light/--intensity：给了任一才置
    /// lightOverride）。注册表里**仍只有一条**——同名两条会打破「同命令内 flag 名唯一」，
    /// 而 `args` 回显仍按两条输出（值一行 + `(bool)` 一行），与改造前的读取次数对齐。
    bool presenceQueried = false;
};

/// 一个子命令的全部真相：summary/notes 是判据契约原文（逐字保留），synopsis 由 flags 拼。
struct SubcommandSpec {
    const char* name;
    const char* summary;
    std::vector<FlagSpec> flags;      // 顺序 = 代码读取顺序 = args 回显顺序 = -h 打印顺序
    std::vector<const char*> notes;   // 追加散文段（打印时前缀一个空格）
    bool allowPositional = false;     // note / refuse / args 收位置参数
    const char* positionalUsage = "";  // 位置参数在 -h 里的占位（逐字打印，含方括号/引号）
};

/// 用法错误（拼错 flag / flag 缺值 / 不该有位置参数）。main 侧映射成 **rc=2**，与
/// 运行时错误（rc=3）分开：前者是"你敲错了，什么都没跑"，后者是"跑挂了"。
struct UsageError : std::runtime_error {
    explicit UsageError(const std::string& msg) : std::runtime_error(msg)
    {
    }
};

const std::vector<SubcommandSpec>& subcommands();
const SubcommandSpec* findSubcommand(const std::string& name);

/// `diffstruct` 段的正文在 `cli_table_diffstruct.cpp`（主表撞「单文件 < 500 代码行」护栏，
/// 缝按**仪器**切，同 `judgments_table_shadow.cpp`）。**拼接顺序只在** `cli_table.cpp` 的
/// `allSpecs()`，本头不抄条数。
[[nodiscard]] std::vector<SubcommandSpec> diffStructSpecs();

/// `diffblock` 段的正文在 `cli_table_diffblock.cpp`（同上一条的拆法：主表撞护栏，缝按仪器切）。
[[nodiscard]] std::vector<SubcommandSpec> diffBlockSpecs();

/// `blend-sort` 段的正文在 `cli_table_blend.cpp`（同上一条的拆法：主表撞「单文件 < 500 代码行」
/// 而 `specsSwimAndProbe()` 装下本条后撞「单函数 < 120 代码行」，缝按仪器切）。它在 -h 里的
/// 位置由 `cli_table.cpp` 的 allSpecs() 决定 = 紧跟 shadow/probe 那一段。
[[nodiscard]] std::vector<SubcommandSpec> blendSortSpecs();

/// `blend-lit` 段的正文在 `cli_table_blend_lit.cpp`（同 blend 段的拆法：主表撞 500 行、
/// `specsSwimAndProbe()` 撞 120 行，缝按仪器切）。-h 位置 = blend-sort 段之后（理由见
/// `cli_table.cpp` 的 allSpecs() 注）。
[[nodiscard]] std::vector<SubcommandSpec> blendLitSpecs();

/// `shadow-grain` 段的正文在 `cli_table_grain.cpp`（同 blend 段的拆法：主表撞 500 行、
/// `specsSwimAndProbe()` 撞 120 行，缝按仪器切）。-h 位置 = blend-sort 段之后（理由见
/// `cli_table.cpp` 的 allSpecs() 注）。
[[nodiscard]] std::vector<SubcommandSpec> shadowGrainSpecs();

/// `shadow-cascade-seam` 段的正文在 `cli_table_seam.cpp`（同 grain 段的拆法：主表撞 500 行、
/// `specsSwimAndProbe()` 撞 120 行，缝按仪器切）。-h 位置 = shadow-grain 段之后（理由见
/// `cli_table.cpp` 的 allSpecs() 注）。
[[nodiscard]] std::vector<SubcommandSpec> shadowCascadeSeamSpecs();

/// `shadow-caster-survival` 段的正文在 `cli_table_survival.cpp`（同 seam 段的拆法：主表撞
/// 500 行、`specsSwimAndProbe()` 撞 120 行，缝按仪器切）。-h 位置 = shadow-cascade-seam 段
/// 之后（理由见 `cli_table.cpp` 的 allSpecs() 注）。
[[nodiscard]] std::vector<SubcommandSpec> shadowCasterSurvivalSpecs();

/// `shadow-edge-ruler` 段的正文在 `cli_table_edge.cpp`（同 survival 段的拆法：主表撞 500 行、
/// `specsSwimAndProbe()` 撞 120 行，缝按仪器切）。-h 位置 = shadow-caster-survival 段之后
/// （理由见 `cli_table.cpp` 的 allSpecs() 注）。
[[nodiscard]] std::vector<SubcommandSpec> shadowEdgeRulerSpecs();

/// `shadow-edge-rebase` 段的正文与 `shadow-edge-ruler` 同住 `cli_table_edge.cpp`（缝按仪器族切）。
[[nodiscard]] std::vector<SubcommandSpec> shadowEdgeRebaseSpecs();

/// 绑定到某个 spec 的解析结果。**默认值只能来自 spec**——旧的三参数
/// `get(args, key, def)` 已删除：留着它，字面量默认值就还有地方活。
/// 读未声明的 flag 直接抛（不抛 = 校验是摆设）。
class Args {
public:
    Args(const std::vector<std::string>& argv, const SubcommandSpec& spec);
    [[nodiscard]] std::string get(const std::string& flag) const;
    [[nodiscard]] double number(const std::string& flag) const;
    /// 整数取值（std::stoi 语义，与改造前逐字相同——别"顺手"换成 number() 再截断）。
    [[nodiscard]] int integer(const std::string& flag) const;
    [[nodiscard]] bool has(const std::string& flag) const;
    /// 未知 flag / flag 缺值 / 不收位置参数的命令收到位置参数 → 抛 UsageError，
    /// 消息点名本命令的合法参数集（不点名等于没校验）。
    void rejectUnknown() const;
    [[nodiscard]] std::vector<std::string> positional() const;

private:
    [[nodiscard]] const FlagSpec& need(const std::string& flag) const;
    [[nodiscard]] std::string legalArgs() const;

    const SubcommandSpec* spec_;
    std::map<std::string, std::string> values_;
    std::set<std::string> supplied_;
    std::vector<std::string> positional_;
    std::vector<std::string> unknown_;   // 长得像 flag 但没声明的 token
    std::vector<std::string> missing_;   // 声明了却没跟值的 flag
};

/// -h 全文：synopsis 由表拼，散文（summary/notes）逐字来自表。
[[nodiscard]] std::string usageText();
/// 单个子命令的详细帮助（含每个 flag 的一行说明）。
[[nodiscard]] std::string detailFor(const SubcommandSpec& spec);
/// `imgqa args <cmd>` 的回显文本（同一个函数也是 selftest 的判据入口）。
[[nodiscard]] std::string echoFor(const SubcommandSpec& spec, const Args& args);
void printUsage();
/// args 子命令本体。它按**目标子命令**的 spec 解析，所以 main 必须在构造外层 Args
/// 之前分发它——否则 `imgqa args temporal-tail --burst 8` 会拿 args 的 spec 去校验
/// `--burst` 而报未知参数。
int cmdArgs(const std::vector<std::string>& argv);

}  // namespace RGL::imgqa

#endif
